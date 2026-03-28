import asyncio
import logging
from collections.abc import Callable

import serial_asyncio_fast as serial_asyncio

from .base import Transport

log = logging.getLogger(__name__)

BAUD_RATE = 115200
INIT_DELAY_S = 0.5
DRAIN_DELAY_S = 0.2


class _LineProtocol(asyncio.Protocol):
    """asyncio Protocol that splits incoming bytes into lines."""

    def __init__(self, line_callback: Callable[[str], None]) -> None:
        self._callback = line_callback
        self._buf = b""
        self.transport: asyncio.Transport | None = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]

    def data_received(self, data: bytes) -> None:
        self._buf += data
        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace").strip()
            if text:
                self._callback(text)

    def connection_lost(self, exc: Exception | None) -> None:
        self.transport = None


class SerialTransport(Transport):
    """Serial port transport using pyserial-asyncio."""

    def __init__(self, port: str, baud: int = BAUD_RATE) -> None:
        self._port = port
        self._baud = baud
        self._protocol: _LineProtocol | None = None
        self._serial_transport: asyncio.Transport | None = None
        self._line_callback: Callable[[str], None] | None = None
        self._connected = False

    async def connect(self) -> None:
        if self._connected:
            return

        def factory() -> _LineProtocol:
            return _LineProtocol(self._dispatch_line)

        transport, protocol = await serial_asyncio.create_serial_connection(
            asyncio.get_event_loop(),
            factory,
            self._port,
            baudrate=self._baud,
        )
        self._serial_transport = transport
        self._protocol = protocol  # type: ignore[assignment]
        self._connected = True

        # Toggle DTR to wake up TinyUSB CDC on nRF52840.
        # After a previous connection closes, TinyUSB's CDC may be stuck in
        # "disconnected" state where it silently drops output. Toggling DTR
        # forces TinyUSB to reinitialize its connected state.
        serial_obj = transport.serial
        if hasattr(serial_obj, 'dtr'):
            serial_obj.dtr = False
            await asyncio.sleep(0.1)
            serial_obj.dtr = True

        # Wait for device to be ready
        await asyncio.sleep(INIT_DELAY_S)

        # Stop any stale streaming from a previous session
        await self.write_line("stream off")
        await asyncio.sleep(DRAIN_DELAY_S)

        log.info("Serial connected: %s @ %d", self._port, self._baud)

    async def disconnect(self) -> None:
        if not self._connected:
            return
        try:
            await self.write_line("stream off")
            await asyncio.sleep(0.1)
        except Exception:
            pass
        if self._serial_transport:
            self._serial_transport.close()
        self._serial_transport = None
        self._protocol = None
        self._connected = False
        log.info("Serial disconnected: %s", self._port)

    async def write_line(self, line: str) -> None:
        if not self._serial_transport:
            raise ConnectionError(f"Not connected to {self._port}")
        self._serial_transport.write((line + "\n").encode("utf-8"))

    def on_line(self, callback: Callable[[str], None]) -> None:
        self._line_callback = callback

    def _dispatch_line(self, line: str) -> None:
        if self._line_callback:
            self._line_callback(line)

    @property
    def is_connected(self) -> bool:
        return self._connected

    @property
    def transport_type(self) -> str:
        return "serial"

    @property
    def address(self) -> str:
        return self._port
