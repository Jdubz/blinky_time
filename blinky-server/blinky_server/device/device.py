import asyncio
import contextlib
import json
import logging
from enum import StrEnum
from typing import Any

from ..transport.base import Transport
from .protocol import DeviceProtocol

log = logging.getLogger(__name__)


class DeviceState(StrEnum):
    DISCONNECTED = "disconnected"
    CONNECTING = "connecting"
    CONNECTED = "connected"
    ERROR = "error"


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

        # Cached settings
        self.settings: list[dict[str, Any]] = []

        # Stream subscribers (WebSocket queues)
        self._stream_subscribers: set[asyncio.Queue[dict[str, Any] | None]] = set()

        # Wire up streaming data routing
        self.protocol.on_stream_line(self._route_stream_line)
        self.protocol.on_raw_line(self._on_raw_line)

    async def connect(self) -> None:
        """Connect to the device and retrieve its info."""
        self.state = DeviceState.CONNECTING
        try:
            await self.transport.connect()
            info = await self.protocol.get_info()
            self._apply_info(info)
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
            q.put_nowait(None)
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
        return {
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
        }

    # ── Internal ──

    def _apply_info(self, info: dict[str, Any]) -> None:
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

    def _route_stream_line(self, line: str) -> None:
        """Parse a streaming JSON line and fan out to subscribers."""
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
