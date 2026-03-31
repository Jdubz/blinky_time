from abc import ABC, abstractmethod
from collections.abc import Callable


class Transport(ABC):
    """Abstract bidirectional text transport to a blinky device."""

    def __init__(self) -> None:
        self._disconnect_callback: Callable[[], None] | None = None

    def on_disconnect(self, callback: Callable[[], None]) -> None:
        """Register callback invoked when the transport disconnects unexpectedly."""
        self._disconnect_callback = callback

    def _fire_disconnect(self) -> None:
        """Subclasses call this when an unexpected disconnect is detected."""
        if self._disconnect_callback:
            self._disconnect_callback()

    @abstractmethod
    async def connect(self) -> None: ...

    @abstractmethod
    async def disconnect(self) -> None: ...

    @abstractmethod
    async def write_line(self, line: str) -> None:
        """Send a text line to the device (newline appended automatically)."""

    @abstractmethod
    def on_line(self, callback: Callable[[str], None]) -> None:
        """Register callback invoked for each line received from the device."""

    @property
    @abstractmethod
    def is_connected(self) -> bool: ...

    @property
    @abstractmethod
    def transport_type(self) -> str:
        """Return 'serial', 'ble', or 'wifi'."""

    @property
    @abstractmethod
    def address(self) -> str:
        """Human-readable address (port path, BLE address, IP, etc.)."""
