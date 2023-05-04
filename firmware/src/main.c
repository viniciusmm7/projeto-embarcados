/************************************************************************
* 5 semestre - Eng. da Computacao - Insper
*
* 2023 - Exemplo com HC05 com RTOS
*
*/

#include <asf.h>
#include "conf_board.h"
#include <string.h>

/************************************************************************/
/* defines                                                              */
/************************************************************************/

#define MY_EOF 'X'

// LED PB3
#define LED_PIO			PIOB
#define LED_PIO_ID		ID_PIOB
#define LED_IDX			3
#define LED_IDX_MASK	(1 << LED_IDX)

// Play/Pause PD28
#define PLAY_PIO		PIOD
#define PLAY_PIO_ID		ID_PIOD
#define PLAY_IDX		28
#define PLAY_IDX_MASK	(1 << PLAY_IDX)

// Backward (-10 s) PA0
#define BW_PIO			PIOA
#define BW_PIO_ID		ID_PIOA
#define BW_IDX			0
#define BW_IDX_MASK		(1 << BW_IDX)

// Forward (+10 s) PC17
#define FW_PIO			PIOC
#define FW_PIO_ID		ID_PIOC
#define FW_IDX			17
#define FW_IDX_MASK		(1 << FW_IDX)

// Fullscreen PC30
#define FS_PIO			PIOC
#define FS_PIO_ID		ID_PIOC
#define FS_IDX			30
#define FS_IDX_MASK		(1 << FS_IDX)

// Potenciômetro PD30
#define AFEC_POT		 AFEC0
#define AFEC_POT_ID		 ID_AFEC0
#define AFEC_POT_CHANNEL 0 // Canal do pino PD30

// usart (bluetooth ou serial)
// Descomente para enviar dados
// pela serial debug

//#define DEBUG_SERIAL

#ifdef DEBUG_SERIAL
#define USART_COM USART1
#define USART_COM_ID ID_USART1
#else
#define USART_COM USART0
#define USART_COM_ID ID_USART0
#endif

/************************************************************************/
/* RTOS                                                                 */
/************************************************************************/

#define TASK_BLUETOOTH_STACK_SIZE		(4096/sizeof(portSTACK_TYPE))
#define TASK_BLUETOOTH_STACK_PRIORITY	(tskIDLE_PRIORITY)
#define TASK_ADC_STACK_SIZE				(4096/sizeof(portSTACK_TYPE))
#define TASK_ADC_STACK_PRIORITY			(tskIDLE_PRIORITY)
#define TASK_PROC_STACK_SIZE			(4096/sizeof(portSTACK_TYPE))
#define TASK_PROC_STACK_PRIORITY		(tskIDLE_PRIORITY)
#define TASK_HANDSHAKE_STACK_SIZE		(4096/sizeof(portSTACK_TYPE))
#define TASK_HANDSHAKE_STACK_PRIORITY	(tskIDLE_PRIORITY)

TimerHandle_t xTimer;

/** Queue for msg log send data */
QueueHandle_t xQueueADC;
QueueHandle_t xQueueCommands;

TaskHandle_t xTaskHandshake;

typedef struct {
	uint value;
} adcData;

typedef struct {
	char type;
	uint value;
	char eof;
} packet;

/************************************************************************/
/* prototypes                                                           */
/************************************************************************/

extern void vApplicationStackOverflowHook(xTaskHandle *pxTask,
signed char *pcTaskName);
extern void vApplicationIdleHook(void);
extern void vApplicationTickHook(void);
extern void vApplicationMallocFailedHook(void);
extern void xPortSysTickHandler(void);

static void USART1_init(void);
static void config_AFEC_pot(Afec *afec, uint32_t afec_id, uint32_t afec_channel,
afec_callback_t callback);
static void configure_console(void);

void create_packet(packet *pack, char type, uint value);
static void task_bluetooth(void *pvParameters);
static void task_adc(void *pvParameters);

/************************************************************************/
/* constants                                                            */
/************************************************************************/

/************************************************************************/
/* variaveis globais                                                    */
/************************************************************************/

/************************************************************************/
/* RTOS application HOOK                                                */
/************************************************************************/

/* Called if stack overflow during execution */
extern void vApplicationStackOverflowHook(xTaskHandle *pxTask,
signed char *pcTaskName) {
	printf("stack overflow %x %s\r\n", pxTask, (portCHAR *)pcTaskName);
	/* If the parameters have been corrupted then inspect pxCurrentTCB to
	* identify which task has overflowed its stack.
	*/
	for (;;) {
	}
}

/* This function is called by FreeRTOS idle task */
extern void vApplicationIdleHook(void) {
	pmc_sleep(SAM_PM_SMODE_SLEEP_WFI);
}

/* This function is called by FreeRTOS each tick */
extern void vApplicationTickHook(void) { }

extern void vApplicationMallocFailedHook(void) {
	/* Called if a call to pvPortMalloc() fails because there is insufficient
	free memory available in the FreeRTOS heap.  pvPortMalloc() is called
	internally by FreeRTOS API functions that create tasks, queues, software
	timers, and semaphores.  The size of the FreeRTOS heap is set by the
	configTOTAL_HEAP_SIZE configuration constant in FreeRTOSConfig.h. */

	/* Force an assert. */
	configASSERT( ( volatile void * ) NULL );
}

/************************************************************************/
/* handlers / callbacks                                                 */
/************************************************************************/

static void play_callback(void) {
	packet pack;
	create_packet(&pack, 'c', 1); // Commands, play
	xQueueSendToBackFromISR(xQueueCommands, &pack, 0);
}

static void bw_callback(void) {
	packet pack;
	create_packet(&pack, 'c', 2); // Commands, backward -10 s
	xQueueSendToBackFromISR(xQueueCommands, &pack, 0);
}

static void fw_callback(void) {
	packet pack;
	create_packet(&pack, 'c', 3); // Commands, forward +10 s
	xQueueSendToBackFromISR(xQueueCommands, &pack, 0);
}

static void fs_callback(void) {
	packet pack;
	create_packet(&pack, 'c', 4); // Commands, fullscreen
	xQueueSendToBackFromISR(xQueueCommands, &pack, 0);
}

static void AFEC_pot_callback(void) {
	adcData adc;
	adc.value = afec_channel_get_value(AFEC_POT, AFEC_POT_CHANNEL);
	BaseType_t xHigherPriorityTaskWoken = pdTRUE;
	xQueueSendToBackFromISR(xQueueADC, &adc, &xHigherPriorityTaskWoken);
}

/************************************************************************/
/* funcoes                                                              */
/************************************************************************/

void create_packet(packet *pack, char type, uint value) {
	pack->type = type;
	pack->value = value;
	pack->eof = MY_EOF;
}

static void send_packet(packet pack) {
	while (!usart_is_tx_ready(USART_COM))
		vTaskDelay(10 / portTICK_PERIOD_MS);
	usart_write(USART_COM, pack.type);
	
	while (!usart_is_tx_ready(USART_COM))
		vTaskDelay(10 / portTICK_PERIOD_MS);
	usart_write(USART_COM, pack.value);
	
	while (!usart_is_tx_ready(USART_COM))
		vTaskDelay(10 / portTICK_PERIOD_MS);
	usart_write(USART_COM, pack.value >> 8);
	
	while (!usart_is_tx_ready(USART_COM))
		vTaskDelay(10 / portTICK_PERIOD_MS);
	usart_write(USART_COM, pack.eof);
}

static void verify_handshake(char handshake) {
	if (usart_read(USART_COM, &handshake) == 0) {
		if (handshake == 'h') {
			xTaskCreate(task_bluetooth, "BLUETOOTH", TASK_BLUETOOTH_STACK_SIZE, NULL, TASK_BLUETOOTH_STACK_PRIORITY, NULL);
			xTaskCreate(task_adc, "ADC", TASK_ADC_STACK_SIZE, NULL, TASK_ADC_STACK_PRIORITY, NULL);
		}
	}
}

void io_init(void) {

	// Ativa PIOs
	pmc_enable_periph_clk(ID_PIOA);
	pmc_enable_periph_clk(ID_PIOB);
	pmc_enable_periph_clk(ID_PIOC);
	pmc_enable_periph_clk(ID_PIOD);

	// Configura Pinos
	pio_configure(LED_PIO, PIO_OUTPUT_0, LED_IDX_MASK, PIO_DEFAULT | PIO_DEBOUNCE);
	pio_configure(PLAY_PIO, PIO_INPUT, PLAY_IDX_MASK, PIO_PULLUP | PIO_DEBOUNCE);
	pio_configure(BW_PIO, PIO_INPUT, BW_IDX_MASK, PIO_PULLUP | PIO_DEBOUNCE);
	pio_configure(FW_PIO, PIO_INPUT, FW_IDX_MASK, PIO_PULLUP | PIO_DEBOUNCE);
	pio_configure(FS_PIO, PIO_INPUT, FS_IDX_MASK, PIO_PULLUP | PIO_DEBOUNCE);
	
	pio_set_debounce_filter(LED_PIO, LED_IDX_MASK, 60);
	pio_set_debounce_filter(PLAY_PIO, PLAY_IDX_MASK, 60);
	pio_set_debounce_filter(BW_PIO, BW_IDX_MASK, 60);
	pio_set_debounce_filter(FW_PIO, FW_IDX_MASK, 60);
	pio_set_debounce_filter(FS_PIO, FS_IDX_MASK, 60);
	
	pio_handler_set(PLAY_PIO, PLAY_PIO_ID, PLAY_IDX_MASK, PIO_IT_FALL_EDGE, play_callback);
	pio_handler_set(BW_PIO, BW_PIO_ID, BW_IDX_MASK, PIO_IT_FALL_EDGE, bw_callback);
	pio_handler_set(FW_PIO, FW_PIO_ID, FW_IDX_MASK, PIO_IT_FALL_EDGE, fw_callback);
	pio_handler_set(FS_PIO, FS_PIO_ID, FS_IDX_MASK, PIO_IT_FALL_EDGE, fs_callback);
	
	pio_enable_interrupt(PLAY_PIO, PLAY_IDX_MASK);
	pio_enable_interrupt(BW_PIO, BW_IDX_MASK);
	pio_enable_interrupt(FW_PIO, FW_IDX_MASK);
	pio_enable_interrupt(FS_PIO, FS_IDX_MASK);
	
	pio_get_interrupt_status(PLAY_PIO);
	pio_get_interrupt_status(BW_PIO);
	pio_get_interrupt_status(FW_PIO);
	pio_get_interrupt_status(FS_PIO);
	
	NVIC_EnableIRQ(PLAY_PIO_ID);
	NVIC_SetPriority(PLAY_PIO_ID, 4);
	
	NVIC_EnableIRQ(BW_PIO_ID);
	NVIC_SetPriority(BW_PIO_ID, 4);
	
	NVIC_EnableIRQ(FW_PIO_ID);
	NVIC_SetPriority(FW_PIO_ID, 4);
	
	NVIC_EnableIRQ(FS_PIO_ID);
	NVIC_SetPriority(FS_PIO_ID, 4);
}

static void configure_console(void) {
	const usart_serial_options_t uart_serial_options = {
		.baudrate = CONF_UART_BAUDRATE,
		#if (defined CONF_UART_CHAR_LENGTH)
		.charlength = CONF_UART_CHAR_LENGTH,
		#endif
		.paritytype = CONF_UART_PARITY,
		#if (defined CONF_UART_STOP_BITS)
		.stopbits = CONF_UART_STOP_BITS,
		#endif
	};

	/* Configure console UART. */
	stdio_serial_init(CONF_UART, &uart_serial_options);

	/* Specify that stdout should not be buffered. */
	#if defined(__GNUC__)
	setbuf(stdout, NULL);
	#else
	/* Already the case in IAR's Normal DLIB default configuration: printf()
	* emits one character at a time.
	*/
	#endif
}

static void config_AFEC_pot(Afec *afec, uint32_t afec_id, uint32_t afec_channel,
afec_callback_t callback) {
	/*************************************
	* Ativa e configura AFEC
	*************************************/
	/* Ativa AFEC - 0 */
	afec_enable(afec);

	/* struct de configuracao do AFEC */
	struct afec_config afec_cfg;

	/* Carrega parametros padrao */
	afec_get_config_defaults(&afec_cfg);

	/* Configura AFEC */
	afec_init(afec, &afec_cfg);

	/* Configura trigger por software */
	afec_set_trigger(afec, AFEC_TRIG_SW);

	/*** Configuracao específica do canal AFEC ***/
	struct afec_ch_config afec_ch_cfg;
	afec_ch_get_config_defaults(&afec_ch_cfg);
	afec_ch_cfg.gain = AFEC_GAINVALUE_0;
	afec_ch_set_config(afec, afec_channel, &afec_ch_cfg);

	/*
	* Calibracao:
	* Because the internal ADC offset is 0x200, it should cancel it and shift
	down to 0.
	*/
	afec_channel_set_analog_offset(afec, afec_channel, 0x200);

	/***  Configura sensor de temperatura ***/
	struct afec_temp_sensor_config afec_temp_sensor_cfg;

	afec_temp_sensor_get_config_defaults(&afec_temp_sensor_cfg);
	afec_temp_sensor_set_config(afec, &afec_temp_sensor_cfg);

	/* configura IRQ */
	afec_set_callback(afec, afec_channel, callback, 1);
	NVIC_SetPriority(afec_id, 4);
	NVIC_EnableIRQ(afec_id);
}

uint32_t usart_puts(uint8_t *pstring) {
	uint32_t i ;

	while(*(pstring + i))
	if(uart_is_tx_empty(USART_COM))
	usart_serial_putchar(USART_COM, *(pstring+i++));
}

void usart_put_string(Usart *usart, char str[]) {
	usart_serial_write_packet(usart, str, strlen(str));
}

int usart_get_string(Usart *usart, char buffer[], int bufferlen, uint timeout_ms) {
	uint timecounter = timeout_ms;
	uint32_t rx;
	uint32_t counter = 0;

	while( (timecounter > 0) && (counter < bufferlen - 1)) {
		if(usart_read(usart, &rx) == 0) {
			buffer[counter++] = rx;
		}
		else{
			timecounter--;
			vTaskDelay(1);
		}
	}
	buffer[counter] = 0x00;
	return counter;
}

void usart_send_command(Usart *usart, char buffer_rx[], int bufferlen,
char buffer_tx[], int timeout) {
	usart_put_string(usart, buffer_tx);
	usart_get_string(usart, buffer_rx, bufferlen, timeout);
}

void config_usart0(void) {
	sysclk_enable_peripheral_clock(ID_USART0);
	usart_serial_options_t config;
	config.baudrate = 9600;
	config.charlength = US_MR_CHRL_8_BIT;
	config.paritytype = US_MR_PAR_NO;
	config.stopbits = false;
	usart_serial_init(USART0, &config);
	usart_enable_tx(USART0);
	usart_enable_rx(USART0);

	// RX - PB0  TX - PB1
	pio_configure(PIOB, PIO_PERIPH_C, (1 << 0), PIO_DEFAULT);
	pio_configure(PIOB, PIO_PERIPH_C, (1 << 1), PIO_DEFAULT);
}

int hc05_init(void) {
	char buffer_rx[128];
	usart_send_command(USART_COM, buffer_rx, 1000, "AT", 100);
	vTaskDelay( 500 / portTICK_PERIOD_MS);
	usart_send_command(USART_COM, buffer_rx, 1000, "AT", 100);
	vTaskDelay( 500 / portTICK_PERIOD_MS);
	usart_send_command(USART_COM, buffer_rx, 1000, "AT+NAMEvinicius", 100);
	vTaskDelay( 500 / portTICK_PERIOD_MS);
	usart_send_command(USART_COM, buffer_rx, 1000, "AT", 100);
	vTaskDelay( 500 / portTICK_PERIOD_MS);
	usart_send_command(USART_COM, buffer_rx, 1000, "AT+PIN0000", 100);
}

/************************************************************************/
/* TASKS                                                                */
/************************************************************************/

static void task_bluetooth(void *pvParameters) {
	printf("Task Bluetooth started \n");
	
	vTaskSuspend(xTaskHandshake);
	
	packet pack;
	
	// Task não deve retornar.
	while(1) {
		if (xQueueReceive(xQueueCommands, &pack, 0)) send_packet(pack);
	}
}

void vTimerCallback(TimerHandle_t xTimer) {
	/* Selecina canal e inicializa conversão */
	afec_channel_enable(AFEC_POT, AFEC_POT_CHANNEL);
	afec_start_software_conversion(AFEC_POT);
}

static void task_adc(void *pvParameters) {
	printf("Task adc started\n");
	config_AFEC_pot(AFEC_POT, AFEC_POT_ID, AFEC_POT_CHANNEL, AFEC_pot_callback);
	
	xTimer = xTimerCreate("Timer", 100, pdTRUE, (void *)0, vTimerCallback);
	xTimerStart(xTimer, 0);
	
	adcData adc;
	packet pack;
	
	while (1) {
		if (xQueueReceive(xQueueADC, &adc, 0)) {
			create_packet(&pack, 'v', adc.value); // Volume
			
			if (adc.value <= 100) pio_clear(LED_PIO, LED_IDX_MASK);
			else pio_set(LED_PIO, LED_IDX_MASK);
			
			xQueueSend(xQueueCommands, &pack, 0);
		}
	}
}

static void task_handshake(void *pvParameters) {
	printf("Task handshake started\n");
	// configura LEDs e Botões
	io_init();
	printf("Inicializando HC05\n");
	config_usart0();
	hc05_init();

	char h; // Handshake
	packet handshake;
	create_packet(&handshake, 'h', 0); // Handshake
	
	while (1) {
		verify_handshake(h);
		send_packet(handshake);
	}
}

/************************************************************************/
/* main                                                                 */
/************************************************************************/

int main(void) {
	/* Initialize the SAM system */
	sysclk_init();
	board_init();
	configure_console();
	
	xQueueADC = xQueueCreate(100, sizeof(adcData));
	if (xQueueADC == NULL) printf("Failed to create QUEUE ADC\r\n");
	
	xQueueCommands = xQueueCreate(100, sizeof(packet));
	if (xQueueCommands == NULL) printf("Failed to create QUEUE COMMANDS\r\n");

	if(xTaskCreate(task_handshake, "HANDSHAKE", TASK_HANDSHAKE_STACK_SIZE, NULL, TASK_HANDSHAKE_STACK_PRIORITY, &xTaskHandshake) != pdPASS)
	printf("Failed to create task HANDSHAKE\r\n");
	
	/* Start the scheduler. */
	vTaskStartScheduler();

	while(1){}

	/* Will only get here if there was insufficient memory to create the idle task. */
	return 0;
}
