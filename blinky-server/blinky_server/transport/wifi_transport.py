"""WiFi TCP transport for ESP32-S3 devices.

Connects to devices running WifiCommandServer on port 3333.
Same line-based protocol as serial and BLE NUS.
"""

import asyncio
import contextlib
import logging
from collections.abc import Callable

from .base import Transport

log = logging.getLogger(__name__)

DEFAULT_PORT = 3333
CONNECT_TIMEOUT_S = 5.0
INIT_DELAY_S = 0.3


class WifiTransport(Transport):
    """WiFi TCP transport using asyncio streams."""

    def __init__(self, host: str, port: int = DEFAULT_PORT) -> None:
        super().__init__()
        self._host = host
        self._port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._line_callback: Callable[[str], None] | None = None
        self._connected = False
        self._read_task: asyncio.Task[None] | None = None

    async def connect(self) -> None:
        if self._connected:
            return

        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self._host, self._port),
            timeout=CONNECT_TIMEOUT_S,
        )
        self._connected = True

        # Start background read task
        self._read_task = asyncio.create_task(self._read_loop())

        # Brief init delay
        await asyncio.sleep(INIT_DELAY_S)

        log.info("WiFi connected: %s:%d", self._host, self._port)

    async def disconnect(self) -> None:
        if not self._connected:
            return
        self._connected = False
        if self._read_task:
            self._read_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._read_task
            self._read_task = None
        if self._writer:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:
                pass
        self._reader = None
        self._writer = None
        log.info("WiFi disconnected: %s:%d", self._host, self._port)

    async def write_line(self, line: str) -> None:
        if not self._writer:
            raise ConnectionError(f"Not connected to {self._host}:{self._port}")
        self._writer.write((line + "\r\n").encode("utf-8"))
        await self._writer.drain()

    def on_line(self, callback: Callable[[str], None]) -> None:
        self._line_callback = callback

    async def _read_loop(self) -> None:
        """Background task: read lines from TCP socket."""
        try:
            while self._connected and self._reader:
                line_bytes = await self._reader.readline()
                if not line_bytes:
                    # EOF — server closed connection
                    break
                text = line_bytes.decode("utf-8", errors="replace").strip()
                if text and self._line_callback:
                    self._line_callback(text)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            log.warning("WiFi read error: %s", e)
        finally:
            if self._connected:
                self._connected = False
                log.info("WiFi connection lost: %s:%d", self._host, self._port)

    @property
    def is_connected(self) -> bool:
        return self._connected

    @property
    def transport_type(self) -> str:
        return "wifi"

    @property
    def address(self) -> str:
        return f"{self._host}:{self._port}"
