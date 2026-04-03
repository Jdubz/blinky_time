#!/usr/bin/env python3
"""Reset a USB device via USBDEVFS_RESET ioctl.

Usage: sudo usb_reset.py /dev/bus/usb/BBB/DDD

Sends the standard Linux USBDEVFS_RESET ioctl to reinitialize a USB device
without a power-cycle. Used by blinky-server to clear TinyUSB CDC state
after closing a serial connection.

Intended to be run via sudoers with a narrow rule:
    blinkytime ALL=(ALL) NOPASSWD: /path/to/usb_reset.py
"""

import fcntl
import os
import sys

# _IO('U', 20) = 0x5514 — linux/usbdevice_fs.h
USBDEVFS_RESET = 21780


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} /dev/bus/usb/BBB/DDD", file=sys.stderr)
        return 1

    path = sys.argv[1]

    # Validate path format to prevent misuse
    if not path.startswith("/dev/bus/usb/"):
        print(f"Error: path must start with /dev/bus/usb/, got: {path}", file=sys.stderr)
        return 1

    if not os.path.exists(path):
        print(f"Error: device not found: {path}", file=sys.stderr)
        return 1

    try:
        fd = os.open(path, os.O_WRONLY)
        fcntl.ioctl(fd, USBDEVFS_RESET, 0)
        os.close(fd)
    except OSError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
