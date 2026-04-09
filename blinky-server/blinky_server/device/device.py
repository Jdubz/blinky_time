import asyncio
import contextlib
import json
import logging
import time as _time
from enum import StrEnum
from typing import Any

from ..testing.test_session import TestSession
from ..transport.base import Transport
from .protocol import DeviceProtocol

log = logging.getLogger(__name__)


class DeviceState(StrEnum):
    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    CONNECTED = "connected"
    ERROR = "error"
    DFU_RECOVERY = "dfu_recovery"


class Device:
    """Represents a single connected blinky device."""

    def __init__(
        self,
        device_id: str,
        port: str,
        platform: str,
        transport: Transport,
    ) -> None:
        self.id = device_id  # USB serial number (stable)
        self.port = port  # e.g., /dev/ttyACM0
        self.platform = platform  # "nrf52840" or "esp32s3"
        self.transport = transport
        self.protocol = DeviceProtocol(transport)
        self.state = DeviceState.DISCONNECTED

        # Device info (populated on connect)
        self.version: str | None = None
        self.device_type: str | None = None
        self.device_name: str | None = None
        self.width: int | None = None
        self.height: int | None = None
        self.leds: int | None = None
        self.configured: bool = False
        self.safe_mode: bool = False

        # Cross-transport identity (populated from firmware json info)
        self.hardware_sn: str | None = None  # FICR DEVICEID (matches USB serial number)
        self.ble_address: str | None = None  # BLE MAC address (for BLE DFU fallback)

        # Connection health
        self.last_seen: float | None = None  # monotonic timestamp of last successful comms
        self.rssi: int | None = None  # BLE signal strength (dBm, from discovery)

        # Cached settings
        self.settings: list[dict[str, Any]] = []

        # Test session (active recording, if any)
        self._test_session: TestSession | None = None

        # Stream subscribers (WebSocket queues)
        self._stream_subscribers: set[asyncio.Queue[dict[str, Any] | None]] = set()

        # Wire up streaming data routing
        self.protocol.on_stream_line(self._route_stream_line)
        self.protocol.on_raw_line(self._on_raw_line)

    async def connect(self) -> None:
        """Connect to the device and retrieve its info.

        Retries get_info once if the first attempt returns no version —
        nRF52840 boot takes 5-6s (1s serial init + 3s LED test + BLE init)
        which can exceed the initial command window.
        """
        self.state = DeviceState.CONNECTING
        try:
            await self.transport.connect()
            self.transport.on_disconnect(self._on_transport_disconnect)
            info = await self.protocol.get_info()
            if not info.get("version"):
                # Device may still be booting (LED test, BLE init).
                # Wait and retry once before giving up.
                log.debug("No version from %s, retrying after 4s...", self.id[:12])
                await asyncio.sleep(4)
                info = await self.protocol.get_info()
            self.apply_info(info)
            self.last_seen = _time.monotonic()
            self.state = DeviceState.CONNECTED
            log.info(
                "Device connected: %s (%s) on %s - %s",
                self.id[:12],
                self.platform,
                self.port,
                self.device_name or "unconfigured",
            )
        except Exception as e:
            self.state = DeviceState.ERROR
            log.error("Failed to connect %s on %s: %s", self.id[:12], self.port, e)
            raise

    async def disconnect(self) -> None:
        """Disconnect from the device."""
        with contextlib.suppress(Exception):
            await self.transport.disconnect()
        self.state = DeviceState.DISCONNECTED
        # Notify all stream subscribers that device disconnected
        for q in self._stream_subscribers:
            with contextlib.suppress(asyncio.QueueFull):
                q.put_nowait(None)
        self._stream_subscribers.clear()

    def _on_transport_disconnect(self) -> None:
        """Called when the transport detects an unexpected disconnect."""
        if self.state == DeviceState.DISCONNECTED:
            return  # Already handled
        log.warning("Device %s transport disconnected unexpectedly", self.id[:12])
        self.state = DeviceState.DISCONNECTED
        # Wake up any pending command so it fails fast instead of timing out
        self.protocol.cancel_pending()
        # Notify stream subscribers (suppress QueueFull — subscriber may be backed up)
        dead: list[asyncio.Queue[dict[str, Any] | None]] = []
        for q in self._stream_subscribers:
            try:
                q.put_nowait(None)
            except asyncio.QueueFull:
                dead.append(q)
        for q in dead:
            self._stream_subscribers.discard(q)
        self._stream_subscribers.clear()

    def subscribe_stream(self) -> asyncio.Queue[dict[str, Any] | None]:
        """Subscribe to streaming data. Returns a queue that receives JSON dicts."""
        q: asyncio.Queue[dict[str, Any] | None] = asyncio.Queue[dict[str, Any] | None](maxsize=100)
        self._stream_subscribers.add(q)
        return q

    def unsubscribe_stream(self, q: asyncio.Queue[dict[str, Any] | None]) -> None:
        self._stream_subscribers.discard(q)

    def to_dict(self) -> dict[str, Any]:
        """Serialize device state for API responses."""
        d: dict[str, Any] = {
            "id": self.id,
            "port": self.port,
            "platform": self.platform,
            "transport": self.transport.transport_type,
            "state": self.state.value,
            "version": self.version,
            "device_type": self.device_type,
            "device_name": self.device_name,
            "width": self.width,
            "height": self.height,
            "leds": self.leds,
            "configured": self.configured,
            "safe_mode": self.safe_mode,
            "streaming": self.protocol.streaming,
            "hardware_sn": self.hardware_sn,
            "ble_address": self.ble_address,
            "rssi": self.rssi,
            "last_seen_ago": round(_time.monotonic() - self.last_seen, 1)
            if self.last_seen
            else None,
        }
        # BLE-specific: expose negotiated MTU for diagnostics
        if hasattr(self.transport, "mtu"):
            d["mtu"] = self.transport.mtu
        return d

    # ── Internal ──

    def apply_info(self, info: dict[str, Any]) -> None:
        """Apply parsed json info response to device state."""
        self.version = info.get("version")
        dev = info.get("device", {})
        self.device_type = dev.get("id")
        self.device_name = dev.get("name")
        self.width = dev.get("width")
        self.height = dev.get("height")
        self.leds = dev.get("leds")
        self.configured = dev.get("configured", False)
        self.safe_mode = dev.get("safeMode", False)

        # Detect platform from firmware's json info response
        platform = info.get("platform")
        if platform and platform != "unknown":
            self.platform = platform

        # Cross-transport identity fields (firmware v83+)
        sn = info.get("sn")
        if sn:
            self.hardware_sn = sn
        ble = info.get("ble")
        if ble:
            self.ble_address = ble

    def start_test_session(self) -> TestSession:
        """Create and attach a new test recording session."""
        session = TestSession()
        self._test_session = session
        return session

    def stop_test_session(self) -> None:
        """Detach the current test session (if any)."""
        self._test_session = None

    def _route_stream_line(self, line: str) -> None:
        """Parse a streaming JSON line and fan out to subscribers + test session."""
        self.last_seen = _time.monotonic()
        try:
            data = json.loads(line)
        except json.JSONDecodeError:
            return

        # Tag with device ID for multiplexed streams
        msg: dict[str, Any]
        if "a" in data:
            msg = {"type": "audio", "device_id": self.id, "data": data}
        elif "b" in data:
            msg = {"type": "battery", "device_id": self.id, "data": data}
        elif data.get("type") == "TRANSIENT":
            msg = {"type": "transient", "device_id": self.id, "data": data}
        elif data.get("type") == "RHYTHM":
            msg = {"type": "rhythm", "device_id": self.id, "data": data}
        elif data.get("type") == "STATUS":
            msg = {"type": "status", "device_id": self.id, "data": data}
        else:
            msg = {"type": "data", "device_id": self.id, "data": data}

        # Forward to test session if recording
        if self._test_session is not None and self._test_session.recording:
            self._test_session.ingest(msg["type"], data)

        # Fan out to all subscribers, drop if queue full
        dead = []
        for q in self._stream_subscribers:
            try:
                q.put_nowait(msg)
            except asyncio.QueueFull:
                dead.append(q)
        for q in dead:
            self._stream_subscribers.discard(q)

    def _on_raw_line(self, line: str) -> None:
        """Handle non-streaming, non-response lines (log output)."""
        log.debug("[%s] %s", self.id[:8], line)
