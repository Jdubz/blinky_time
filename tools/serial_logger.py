#!/usr/bin/env python3
"""Serial logger for blinky-things device debugging."""

import serial
import time
import sys
from datetime import datetime

PORT = 'COM34'
BAUD = 115200
LOG_FILE = 'serial_log.txt'

def main():
    print(f"Connecting to {PORT} at {BAUD} baud...")
    print(f"Logging to {LOG_FILE}")
    print("Press Ctrl+C to stop\n")

    while True:
        try:
            with serial.Serial(PORT, BAUD, timeout=1) as ser:
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Connected!")
                with open(LOG_FILE, 'a') as log:
                    log.write(f"\n=== Session started {datetime.now()} ===\n")
                    while True:
                        if ser.in_waiting:
                            line = ser.readline().decode(errors='ignore').strip()
                            if line:
                                timestamp = datetime.now().strftime('%H:%M:%S.%f')[:-3]
                                entry = f"[{timestamp}] {line}"
                                print(entry)
                                log.write(entry + '\n')
                                log.flush()
                        time.sleep(0.01)
        except serial.SerialException as e:
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Disconnected: {e}")
            print("Waiting for device...")
            time.sleep(2)
        except KeyboardInterrupt:
            print("\nStopped.")
            sys.exit(0)

if __name__ == '__main__':
    main()
