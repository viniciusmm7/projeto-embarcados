import pyautogui
import serial
import argparse
import time
import logging
from ctypes import cast, POINTER
from comtypes import CLSCTX_ALL
from pycaw.pycaw import AudioUtilities, IAudioEndpointVolume

class MyControllerMap:
    def __init__(self):
        self.button = {} # Fast forward (10 seg) pro Youtube

class SerialControllerInterface:
    # Protocolo
    # byte 1 -> Botão 1 (estado - Apertado 1 ou não 0)
    # byte 2 -> EOP - End of Packet -> valor reservado 'X'

    def __init__(self, port, baudrate):
        self.ser = serial.Serial(port, baudrate=baudrate)
        self.mapping = MyControllerMap()
        self.incoming = b'0'
        pyautogui.PAUSE = 0  ## remove delay
    
    def update(self):
        try:
            ## Sync protocol
            print()
            print('update')
            while self.incoming != b'X':
                self.incoming = self.ser.read()
                print(f'Received INCOMING: {self.incoming}')
                logging.debug(f'Received INCOMING: {self.incoming}')

            data = self.ser.read()
            value = self.ser.read()
            value_bitshifited = self.ser.read()

            value = int.from_bytes(value_bitshifited + value, byteorder='big')

            print(value)

            match data:
                case b'h':
                    self.ser.write(b'h')

                case b'c':
                    logging.debug(f'Received DATA: {data}')
                    logging.info(f'KEYDOWN {value}')
                    match value:
                        case 1:
                            pyautogui.press('space')
                        
                        case 2:
                            pyautogui.press('left')

                        case 3:
                            pyautogui.press('right')

                        case 4:
                            pyautogui.press('f')

                case b'v':
                    logging.info(f'Analog value: {value}')
                    default_mixer = AudioUtilities.GetSpeakers()
                    volume = default_mixer.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
                    volume = cast(volume, POINTER(IAudioEndpointVolume))
                    volume_percentage = max(0, min(100, abs(int(((value - 80)/3950)*100))))
                    logging.info(f'New volume: {volume_percentage}%')
                    volume.SetMasterVolumeLevelScalar(volume_percentage / 100, None)

            self.incoming = self.ser.read()
        
        except Exception as e:
            logging.error(e)
            pass


class DummyControllerInterface:
    def __init__(self):
        self.mapping = MyControllerMap()

    def update(self):
        pyautogui.keyDown(self.mapping.button['A'])
        time.sleep(0.1)
        pyautogui.keyUp(self.mapping.button['A'])
        logging.info('[Dummy] Pressed A button')
        time.sleep(1)


if __name__ == '__main__':
    interfaces = ['dummy', 'serial']
    argparse = argparse.ArgumentParser()
    argparse.add_argument('serial_port', type=str)
    argparse.add_argument('-b', '--baudrate', type=int, default=115200)
    argparse.add_argument('-c', '--controller_interface', type=str, default='serial', choices=interfaces)
    argparse.add_argument('-d', '--debug', default=False, action='store_true')
    args = argparse.parse_args()
    if args.debug:
        logging.basicConfig(level=logging.DEBUG)

    print(f'Connection to {args.serial_port} using {args.controller_interface} interface ({args.baudrate})')
    if args.controller_interface == 'dummy':
        controller = DummyControllerInterface()
    else:
        controller = SerialControllerInterface(port=args.serial_port, baudrate=args.baudrate)

    while True:
        controller.update()
