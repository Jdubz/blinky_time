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


DFU_SERVICE_UUID = "00001530-1212-efde-1523-785feabcd123"


def _bootloader_to_app_address(boot_addr: str) -> str:
    """Convert bootloader BLE address to app address (subtract 1 from 6-byte addr).

    Handles borrow propagation across octets (e.g., XX:XX:XX:XX:XX:00 → XX:XX:XX:XX:WW:FF).
    """
    parts = boot_addr.split(':')
    octets = [int(p, 16) for p in parts]
    borrow = 1
    for i in range(len(octets) - 1, -1, -1):
        octets[i] -= borrow
        if octets[i] < 0:
            octets[i] += 256
            borrow = 1
        else:
            break
    return ':'.join(f"{o:02X}" for o in octets)


async def discover_ble_devices(timeout: float = 5.0) -> list[DiscoveredDevice]:
    """Scan for BLE devices advertising NUS or DFU services.

    NUS devices are normal app-mode devices. DFU devices are stuck in
    BLE DFU bootloader (SafeBoot crash recovery or manual entry).

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
        # Scan for both NUS (app mode) and DFU (bootloader mode) devices
        discovered = await BleakScanner.discover(
            timeout=timeout,
            service_uuids=[NUS_SERVICE_UUID, DFU_SERVICE_UUID],
            return_adv=True,
        )
        for addr, (dev, adv) in discovered.items():
            svc_uuids = [str(u).lower() for u in (adv.service_uuids or [])]
            has_dfu = DFU_SERVICE_UUID in svc_uuids
            has_nus = NUS_SERVICE_UUID in svc_uuids
            # Bootloader mode: DFU service only (no NUS), or name is "AdaDFU".
            # App mode with Adafruit BSP advertises BOTH NUS + DFU (buttonless DFU).
            is_bootloader = has_dfu and (not has_nus or (dev.name or "").startswith("AdaDFU"))
            if is_bootloader:
                # Device is in BLE DFU bootloader mode.
                # Use the app-mode address as device_id so the same physical
                # device keeps the same identity after recovery.
                app_addr = _bootloader_to_app_address(addr)
                devices.append(
                    DiscoveredDevice(
                        device_id=app_addr,
                        platform="nrf52840",
                        transport_type="ble_dfu",
                        address=addr,
                        description=dev.name or "DFU bootloader",
                        rssi=adv.rssi,
                        extra={
                            "app_address": app_addr,
                            "bootloader_address": addr,
                        },
                    )
                )
                log.warning(
                    "Discovered BLE DFU bootloader %s (%s) RSSI=%d — "
                    "device in crash recovery (app addr: %s)",
                    dev.name, addr, adv.rssi, app_addr,
                )
            else:
                devices.append(
                    DiscoveredDevice(
                        device_id=addr,
                        platform="unknown",
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


async def discover_wifi_devices(known_hosts: list[dict] | None = None) -> list[DiscoveredDevice]:
    """Discover WiFi devices via mDNS and/or static registry.

    Scans for _blinky._tcp mDNS services (advertised by ESP32-S3 firmware).
    Also includes any statically configured host:port pairs.

    Args:
        known_hosts: List of {"host": "ip", "port": 3333, "device_id": "..."}
    """
    devices = []
    seen_hosts: set[str] = set()

    # mDNS discovery for _blinky._tcp
    try:
        from zeroconf import Zeroconf, ServiceBrowser

        zc = Zeroconf()
        found: list[dict] = []

        class Listener:
            def add_service(self, zc: Zeroconf, type_: str, name: str) -> None:
                info = zc.get_service_info(type_, name)
                if info:
                    addrs = info.parsed_addresses()
                    if not addrs:
                        return
                    host = addrs[0]  # First address (IPv4 or IPv6)
                    port = info.port or 3333
                    found.append({"host": host, "port": port, "name": name})

            def remove_service(self, zc: Zeroconf, type_: str, name: str) -> None:
                pass

            def update_service(self, zc: Zeroconf, type_: str, name: str) -> None:
                pass

        browser = ServiceBrowser(zc, "_blinky._tcp.local.", Listener())
        await asyncio.sleep(3.0)  # Wait for responses
        browser.cancel()
        zc.close()

        for entry in found:
            host = entry["host"]
            port = entry["port"]
            # IP address as device_id — not stable across DHCP renewals.
            # A future improvement could query the device for a hardware serial number.
            device_id = host
            seen_hosts.add(host)
            devices.append(
                DiscoveredDevice(
                    device_id=device_id,
                    platform="esp32s3",
                    transport_type="wifi",
                    address=f"{host}:{port}",
                    description=f"mDNS: {entry['name']}",
                    extra={"host": host, "port": port},
                )
            )
            log.info("Discovered WiFi device via mDNS: %s:%d", host, port)
    except ImportError:
        log.debug("zeroconf not installed, skipping mDNS WiFi discovery")
    except Exception as e:
        log.warning("mDNS WiFi scan failed: %s", e)

    # Static registry (skip hosts already found via mDNS)
    for entry in known_hosts or []:
        host = entry["host"]
        if host in seen_hosts:
            continue
        port = entry.get("port", 3333)
        device_id = entry.get("device_id", host)
        devices.append(
            DiscoveredDevice(
                device_id=device_id,
                platform="esp32s3",
                transport_type="wifi",
                address=f"{host}:{port}",
                description=f"Static: {host}",
                extra={"host": host, "port": port},
            )
        )
        log.info("Registered WiFi device %s at %s:%d", device_id, host, port)

    return devices


async def cleanup_stale_ble_connections() -> None:
    """Disconnect stale BlueZ BLE connections from previous server sessions.

    When the server restarts, BlueZ may retain connections from the previous
    session. These stale connections consume the device's single BLE peripheral
    slot, preventing new connections. This function finds all connected Blinky
    devices and disconnects them so the new server session can reconnect cleanly.
    """
    import subprocess

    def _cleanup():
        result = subprocess.run(
            ["bluetoothctl", "devices", "Connected"],
            capture_output=True, text=True, timeout=5,
        )
        if result.returncode != 0:
            return 0

        disconnected = 0
        for line in result.stdout.strip().split("\n"):
            if not line.strip():
                continue
            # Format: "Device XX:XX:XX:XX:XX:XX Name"
            parts = line.split()
            if len(parts) >= 2 and parts[0] == "Device":
                addr = parts[1]
                name = " ".join(parts[2:]) if len(parts) > 2 else ""
                # Only disconnect Blinky/AdaDFU devices (not other BLE peripherals)
                if name in ("Blinky", "AdaDFU"):
                    subprocess.run(
                        ["bluetoothctl", "disconnect", addr],
                        capture_output=True, timeout=5,
                    )
                    disconnected += 1
        return disconnected

    try:
        disconnected = await asyncio.to_thread(_cleanup)
        if disconnected:
            log.info("Cleaned up %d stale BLE connections", disconnected)
    except Exception as e:
        log.warning("BLE stale connection cleanup failed: %s", e)


async def discover_all(
    serial_scan: bool = True,
    ble_scan: bool = True,
    ble_timeout: float = 5.0,
    wifi_hosts: list[dict] | None = None,
) -> list[DiscoveredDevice]:
    """Discover devices across all transport types.

    Serial discovery is synchronous (fast) and runs first. BLE and WiFi/mDNS
    discovery are both async (slow) and run concurrently via asyncio.gather().
    Use serial_scan=False for wireless-only mode (no USB serial).
    """
    serial_devs = discover_serial_devices() if serial_scan else []

    ble_coro = discover_ble_devices(timeout=ble_timeout) if ble_scan else asyncio.sleep(0, result=[])
    wifi_coro = discover_wifi_devices(wifi_hosts)

    ble_devs, wifi_devs = await asyncio.gather(ble_coro, wifi_coro)

    all_devs = serial_devs + list(ble_devs) + list(wifi_devs)
    log.info(
        "Discovery: %d serial, %d BLE, %d WiFi",
        len(serial_devs), len(ble_devs), len(wifi_devs),
    )
    return all_devs
