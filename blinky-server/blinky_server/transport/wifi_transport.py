"""WiFi transport stub for future implementation.

Will use TCP sockets or WebSocket to connect to ESP32-S3 devices
running a lightweight server on their WiFi interface.
"""

import logging
from collections.abc import Callable

from .base import Transport

log = logging.getLogger(__name__)


class WifiTransport(Transport):
    """WiFi transport - not yet implemented."""

    def __init__(self, host: str, port: int = 8266) -> None:
        self._host = host
        self._port = port

    async def connect(self) -> None:
        raise NotImplementedError("WiFi transport not yet implemented")

    async def disconnect(self) -> None:
        pass

    async def write_line(self, line: str) -> None:
        raise NotImplementedError("WiFi transport not yet implemented")

    def on_line(self, callback: Callable[[str], None]) -> None:
        pass

    @property
    def is_connected(self) -> bool:
        return False

    @property
    def transport_type(self) -> str:
        return "wifi"

    @property
    def address(self) -> str:
        return f"{self._host}:{self._port}"
