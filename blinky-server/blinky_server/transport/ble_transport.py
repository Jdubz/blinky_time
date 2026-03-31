"""BLE NUS transport for nRF52840 devices.

Uses the `bleak` library to connect to nRF52840 devices via the Nordic
UART Service (NUS) for bidirectional command exchange. Same line-based
protocol as serial, but with BLE-specific timing constraints.

NUS UUIDs:
  Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
  RX (write): 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
  TX (notify): 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
"""

import asyncio
import logging
from collections.abc import Callable

from bleak import BleakClient, BleakScanner

from .base import Transport

log = logging.getLogger(__name__)

# Nordic UART Service UUIDs
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # Write to device
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # Notify from device

# BLE timing
CONNECT_TIMEOUT_S = 10.0
COMMAND_DELAY_S = 0.3     # Delay between commands (BLE stack needs settling time)
INIT_DELAY_S = 1.0        # Wait for device ready after connect


class BleTransport(Transport):
    """BLE NUS transport for nRF52840 devices.

    Connects to a device by BLE address, subscribes to NUS TX notifications
    for device output, and writes commands to NUS RX. Line reassembly handles
    MTU-fragmented responses.
    """

    def __init__(self, ble_address: str) -> None:
        self._address = ble_address
        self._client: BleakClient | None = None
        self._line_callback: Callable[[str], None] | None = None
        self._rx_buf = b""
        self._connected = False
        self._mtu = 20  # Default min MTU, negotiated on connect

    async def connect(self) -> None:
        if self._connected:
            return

        # Hard timeout wrapping entire connect sequence — bleak's built-in
        # timeout can hang on BlueZ when the HCI layer gets stuck.
        try:
            await asyncio.wait_for(
                self._connect_inner(), timeout=CONNECT_TIMEOUT_S + 10)
        except asyncio.TimeoutError:
            # Clean up partial state
            if self._client:
                try:
                    await self._client.disconnect()
                except Exception:
                    pass
            self._client = None
            self._connected = False
            raise ConnectionError(
                f"BLE connect to {self._address} timed out "
                f"after {CONNECT_TIMEOUT_S + 10}s")

    async def _connect_inner(self) -> None:
        log.info("BLE connecting to %s...", self._address)
        self._client = BleakClient(
            self._address,
            timeout=CONNECT_TIMEOUT_S,
        )
        await self._client.connect()

        # Get negotiated MTU
        self._mtu = self._client.mtu_size - 3  # ATT overhead
        log.info("BLE connected: %s (MTU %d)", self._address, self._mtu)

        # Subscribe to NUS TX notifications (device → host).
        # Force StartNotify over AcquireNotify — bleak 3.x defaults to
        # AcquireNotify which drops notifications from some peripherals.
        await self._client.start_notify(
            NUS_TX_UUID, self._on_notification,
            bluez={"use_start_notify": True},
        )

        self._connected = True

        # Wait for device ready + drain any startup messages
        await asyncio.sleep(INIT_DELAY_S)

        # Stop any stale streaming
        await self.write_line("stream off")
        await asyncio.sleep(COMMAND_DELAY_S)

        log.info("BLE ready: %s", self._address)

    async def disconnect(self) -> None:
        if not self._connected:
            return
        try:
            await self.write_line("stream off")
            await asyncio.sleep(0.1)
        except Exception:
            pass
        if self._client:
            try:
                await self._client.stop_notify(NUS_TX_UUID)
            except Exception:
                pass
            try:
                await self._client.disconnect()
            except Exception:
                pass
        self._client = None
        self._connected = False
        self._rx_buf = b""
        log.info("BLE disconnected: %s", self._address)

    async def write_line(self, line: str) -> None:
        """Send a command line to the device via NUS RX characteristic.

        Uses write-with-response for reliability. Fragments long lines
        to fit within the negotiated MTU.
        """
        if not self._client or not self._connected:
            raise ConnectionError(f"Not connected to {self._address}")

        data = (line + "\n").encode("utf-8")

        # Fragment if longer than MTU
        for i in range(0, len(data), self._mtu):
            chunk = data[i:i + self._mtu]
            await self._client.write_gatt_char(
                NUS_RX_UUID, chunk, response=True
            )

        # BLE stack needs time between commands
        await asyncio.sleep(COMMAND_DELAY_S)

    def on_line(self, callback: Callable[[str], None]) -> None:
        self._line_callback = callback

    def _on_notification(self, _sender: int, data: bytearray) -> None:
        """Handle NUS TX notification (device output).

        Notifications may split across MTU boundaries. Reassemble into
        complete lines and dispatch via callback.
        """
        log.info("BLE RX %s: %d bytes", self._address[:12], len(data))
        self._rx_buf += bytes(data)
        while b"\n" in self._rx_buf:
            line, self._rx_buf = self._rx_buf.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace").strip()
            if text and self._line_callback:
                self._line_callback(text)

    @property
    def is_connected(self) -> bool:
        return self._connected and self._client is not None and self._client.is_connected

    @property
    def transport_type(self) -> str:
        return "ble"

    @property
    def address(self) -> str:
        return self._address


async def scan_nus_devices(timeout: float = 5.0) -> list[dict]:
    """Scan for BLE devices advertising the NUS service.

    Returns list of {address, name, rssi} dicts for discovered devices.
    Uses bleak 3.x API with return_adv=True for RSSI data.
    """
    log.info("Scanning for BLE NUS devices (%ds)...", timeout)
    devices = await BleakScanner.discover(
        timeout=timeout,
        service_uuids=[NUS_SERVICE_UUID],
        return_adv=True,
    )

    results = []
    for addr, (dev, adv) in devices.items():
        results.append({
            "address": addr,
            "name": dev.name or "Unknown",
            "rssi": adv.rssi,
        })
        log.info("  Found: %s (%s) RSSI=%d", dev.name, addr, adv.rssi)

    return results
