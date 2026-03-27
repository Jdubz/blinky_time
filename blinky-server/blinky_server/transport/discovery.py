import logging
from dataclasses import dataclass

import serial.tools.list_ports

log = logging.getLogger(__name__)

# Known USB VID/PID pairs for blinky devices
KNOWN_DEVICES = {
    (0x2886, 0x8045): "nrf52840",  # Seeed XIAO nRF52840
    (0x303A, 0x1001): "esp32s3",  # Espressif ESP32-S3
}


@dataclass
class DiscoveredDevice:
    port: str
    platform: str  # "nrf52840" or "esp32s3"
    serial_number: str  # USB serial number (stable device ID)
    vid: int
    pid: int
    description: str


def discover_serial_devices() -> list[DiscoveredDevice]:
    """Scan serial ports for known blinky devices by VID/PID."""
    devices = []
    for info in serial.tools.list_ports.comports():
        if info.vid is None or info.pid is None:
            continue
        platform = KNOWN_DEVICES.get((info.vid, info.pid))
        if platform is None:
            continue
        devices.append(
            DiscoveredDevice(
                port=info.device,
                platform=platform,
                serial_number=info.serial_number or "",
                vid=info.vid,
                pid=info.pid,
                description=info.description or "",
            )
        )
        log.info(
            "Discovered %s on %s (SN: %s)",
            platform,
            info.device,
            info.serial_number,
        )
    return devices
