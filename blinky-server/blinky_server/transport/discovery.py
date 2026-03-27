import asyncio
import logging
from dataclasses import dataclass, field

import serial.tools.list_ports

log = logging.getLogger(__name__)

# Known USB VID/PID pairs for blinky devices
KNOWN_DEVICES = {
    (0x2886, 0x8045): "nrf52840",  # Seeed XIAO nRF52840
    (0x303A, 0x1001): "esp32s3",  # Espressif ESP32-S3
}


@dataclass
class DiscoveredDevice:
    """A device found during discovery."""
    device_id: str          # Stable ID (serial number, BLE address, or IP)
    platform: str           # "nrf52840" or "esp32s3"
    transport_type: str     # "serial", "ble", or "wifi"
    address: str            # Port path, BLE address, or host:port
    description: str = ""
    rssi: int | None = None  # BLE/WiFi signal strength
    extra: dict = field(default_factory=dict)

    # Legacy compat
    @property
    def port(self) -> str:
        return self.address

    @property
    def serial_number(self) -> str:
        return self.device_id


def discover_serial_devices() -> list[DiscoveredDevice]:
    """Scan serial ports for known blinky devices by VID/PID."""
    devices = []
    for info in serial.tools.list_ports.comports():
        if info.vid is None or info.pid is None:
            continue
        platform = KNOWN_DEVICES.get((info.vid, info.pid))
        if platform is None:
            continue
        device_id = info.serial_number or info.device
        devices.append(
            DiscoveredDevice(
                device_id=device_id,
                platform=platform,
                transport_type="serial",
                address=info.device,
                description=info.description or "",
                extra={"vid": info.vid, "pid": info.pid},
            )
        )
        log.info("Discovered serial %s on %s (SN: %s)", platform, info.device, device_id[:12])
    return devices


async def discover_ble_devices(timeout: float = 5.0) -> list[DiscoveredDevice]:
    """Scan for BLE devices advertising the NUS service.

    Requires bleak. Returns empty list if bleak is not available
    or no Bluetooth adapter is present.
    """
    try:
        from bleak import BleakScanner
        from .ble_transport import NUS_SERVICE_UUID
    except ImportError:
        log.debug("bleak not installed, skipping BLE discovery")
        return []

    devices = []
    try:
        discovered = await BleakScanner.discover(
            timeout=timeout,
            service_uuids=[NUS_SERVICE_UUID],
            return_adv=True,
        )
        for addr, (dev, adv) in discovered.items():
            devices.append(
                DiscoveredDevice(
                    device_id=addr,  # BLE address as stable ID
                    platform="nrf52840",  # NUS = nRF52840
                    transport_type="ble",
                    address=addr,
                    description=dev.name or "BLE device",
                    rssi=adv.rssi,
                )
            )
            log.info("Discovered BLE %s (%s) RSSI=%d", dev.name, addr, adv.rssi)
    except Exception as e:
        log.warning("BLE scan failed: %s", e)

    return devices


def discover_wifi_devices(known_hosts: list[dict] | None = None) -> list[DiscoveredDevice]:
    """Return WiFi devices from a static registry.

    WiFi devices can't be auto-discovered without mDNS. For now, this uses
    a list of known host:port pairs. Future: mDNS discovery for _blinky._tcp.

    Args:
        known_hosts: List of {"host": "ip", "port": 3333, "device_id": "..."}
    """
    if not known_hosts:
        return []

    devices = []
    for entry in known_hosts:
        host = entry["host"]
        port = entry.get("port", 3333)
        device_id = entry.get("device_id", host)
        devices.append(
            DiscoveredDevice(
                device_id=device_id,
                platform="esp32s3",
                transport_type="wifi",
                address=f"{host}:{port}",
                description=f"WiFi device at {host}",
                extra={"host": host, "port": port},
            )
        )
        log.info("Registered WiFi device %s at %s:%d", device_id, host, port)
    return devices


async def discover_all(
    ble_scan: bool = True,
    ble_timeout: float = 5.0,
    wifi_hosts: list[dict] | None = None,
) -> list[DiscoveredDevice]:
    """Discover devices across all transport types.

    Serial discovery is synchronous (fast). BLE discovery is async (slow).
    WiFi uses a static registry for now.
    """
    serial_devs = discover_serial_devices()
    wifi_devs = discover_wifi_devices(wifi_hosts)

    ble_devs: list[DiscoveredDevice] = []
    if ble_scan:
        ble_devs = await discover_ble_devices(timeout=ble_timeout)

    all_devs = serial_devs + ble_devs + wifi_devs
    log.info(
        "Discovery: %d serial, %d BLE, %d WiFi",
        len(serial_devs), len(ble_devs), len(wifi_devs),
    )
    return all_devs
