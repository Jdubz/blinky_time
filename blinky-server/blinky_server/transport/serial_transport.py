"""Thread-based serial transport for nRF52840 TinyUSB CDC devices.

Uses a blocking reader thread instead of asyncio's select/epoll-based reader.
This works around a platform issue where select() on Linux cdc-acm fds
doesn't report readable even when data is available (confirmed on Raspberry
Pi ARM + Python 3.13 + serial_asyncio_fast 0.16). Blocking serial.read()
works reliably; the thread dispatches lines to the asyncio event loop via
call_soon_threadsafe.
"""

import asyncio
import logging
import termios
import threading
from collections.abc import Callable

import serial

from .base import Transport

log = logging.getLogger(__name__)

BAUD_RATE = 115200
INIT_DELAY_S = 2.0  # nRF52840 boot: SafeBootWatchdog + BLE + NN load takes 1-3s


class SerialTransport(Transport):
    """Serial port transport with thread-based reader."""

    def __init__(self, port: str, baud: int = BAUD_RATE) -> None:
        super().__init__()
        self._port = port
        self._baud = baud
        self._serial: serial.Serial | None = None
        self._reader_thread: threading.Thread | None = None
        self._stop_event = threading.Event()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._line_callback: Callable[[str], None] | None = None
        self._connected = False

    async def connect(self) -> None:
        if self._connected:
            return

        # Clear HUPCL so closing doesn't drop DTR. TinyUSB needs DTR
        # asserted to transmit — without it, all output is silently dropped.
        try:
            raw = serial.Serial(self._port, self._baud, timeout=0.5)
            attrs = termios.tcgetattr(raw.fd)
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(raw.fd, termios.TCSANOW, attrs)
            raw.dtr = True
            await asyncio.sleep(0.3)
            raw.close()  # DTR stays high (HUPCL cleared)
        except (OSError, serial.SerialException) as e:
            log.debug("HUPCL/DTR setup failed on %s (non-fatal): %s", self._port, e)

        # Wait for device to finish booting
        await asyncio.sleep(INIT_DELAY_S)

        # Open serial port for read/write
        self._serial = serial.Serial(self._port, self._baud, timeout=0.1)

        # Clear HUPCL on the persistent fd too
        try:
            attrs = termios.tcgetattr(self._serial.fd)
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(self._serial.fd, termios.TCSANOW, attrs)
        except (OSError, termios.error) as e:
            log.debug("HUPCL clear failed on %s: %s", self._port, e)

        self._loop = asyncio.get_running_loop()
        self._connected = True
        self._stop_event.clear()

        # Start reader thread (blocking reads dispatched to asyncio loop)
        self._reader_thread = threading.Thread(
            target=self._reader_loop,
            name=f"serial-reader-{self._port}",
            daemon=True,
        )
        self._reader_thread.start()

        # Drain boot messages, then stop stale streaming
        try:
            await self.write_line("stream off")
        except (OSError, serial.SerialException) as e:
            log.debug("Init 'stream off' failed on %s (non-fatal): %s", self._port, e)
        await asyncio.sleep(1.0)

        log.info("Serial connected: %s @ %d", self._port, self._baud)

    def _reader_loop(self) -> None:
        """Background thread: blocking read from serial, dispatch lines to asyncio."""
        buf = b""
        ser = self._serial
        if ser is None:
            return
        while not self._stop_event.is_set():
            try:
                chunk = ser.read(ser.in_waiting or 1)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    line_bytes, buf = buf.split(b"\n", 1)
                    text = line_bytes.decode("utf-8", errors="replace").strip()
                    if text and self._loop:
                        self._loop.call_soon_threadsafe(self._dispatch_line, text)
            except (serial.SerialException, OSError) as e:
                if self._stop_event.is_set():
                    break
                log.warning("Serial read error on %s: %s", self._port, e)
                if self._connected and self._loop:
                    self._connected = False
                    self._loop.call_soon_threadsafe(self._fire_disconnect)
                break

    async def disconnect(self) -> None:
        if not self._connected:
            return
        try:
            await self.write_line("stream off")
            await asyncio.sleep(0.1)
        except Exception:
            pass
        self._connected = False
        self._stop_event.set()
        if self._serial:
            self._serial.close()  # Unblocks blocking read() in the thread
        if self._reader_thread:
            self._reader_thread.join(timeout=2.0)
            if self._reader_thread.is_alive():
                log.warning("Reader thread did not exit for %s", self._port)
            self._reader_thread = None
        self._serial = None
        log.info("Serial disconnected: %s", self._port)

    async def write_line(self, line: str) -> None:
        if not self._serial or not self._serial.is_open:
            raise ConnectionError(f"Not connected to {self._port}")
        self._serial.write((line + "\n").encode("utf-8"))

    async def trigger_bootloader(self) -> None:
        """Enter UF2 bootloader via 1200-baud touch."""
        # Stop reader thread and close connection
        self._connected = False
        self._stop_event.set()
        if self._serial:
            self._serial.close()
        if self._reader_thread:
            self._reader_thread.join(timeout=2.0)
            self._reader_thread = None
        self._serial = None

        # 1200-baud touch (standard Arduino bootloader protocol)
        try:
            with serial.Serial(self._port, 1200, dsrdtr=False) as s:
                s.dtr = True
                await asyncio.sleep(0.05)
                s.dtr = False
        except Exception as e:
            log.debug("1200-baud touch: %s (expected if device reset fast)", e)

        log.info("1200-baud touch sent to %s", self._port)

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
