#!/usr/bin/env python3
"""Log a serial console to a file, with a time stamp per chunk.

Made for the companion's debug console on the Tang Nano 20K (pin 48,
2,000,000 baud, see TANG_NANO_20K.md), but it logs any port:

    python conlog.py COM16 2000000 companion.log

Opening the port of an ESP32 dev board through its USB-UART bridge resets
the board (DTR/RTS), so do not start this in the middle of a transfer.
Needs pyserial.
"""
import sys
import time

import serial


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 1
    port, baud, log = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    with serial.Serial(port, baud, timeout=1) as s, open(log, 'ab', buffering=0) as f:
        f.write(('--- listening on %s at %d %s\n' % (port, baud, time.strftime('%H:%M:%S'))).encode())
        while True:
            d = s.read(4096)
            if d:
                f.write(('[%s] ' % time.strftime('%H:%M:%S')).encode() + d)


if __name__ == '__main__':
    sys.exit(main())
