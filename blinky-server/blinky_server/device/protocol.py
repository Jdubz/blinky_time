import asyncio
import contextlib
import json
import logging
from collections.abc import Callable
from typing import Any

from ..transport.base import Transport

log = logging.getLogger(__name__)

COMMAND_TIMEOUT_S = 2.0
RESPONSE_LINE_TIMEOUT_S = 0.1


class DeviceProtocol:
    """Stateful command/response protocol handler for a single device.

    Handles:
    - Sending commands and collecting multi-line responses
    - Auto-pausing/resuming streaming around commands
    - Routing streaming data (audio, battery, status) to subscribers
    """

    def __init__(self, transport: Transport) -> None:
        self._transport = transport
        self._streaming = False
        self._command_lock = asyncio.Lock()

        # Response collection state
        self._response_buf: list[str] = []
        self._response_event = asyncio.Event()
        self._response_timer: asyncio.TimerHandle | None = None
        self._pending = False

        # Streaming data callbacks
        self._on_stream_line: Callable[[str], None] | None = None
        self._on_raw_line: Callable[[str], None] | None = None

        # Wire up the transport line callback
        transport.on_line(self._handle_line)

    @property
    def streaming(self) -> bool:
        return self._streaming

    def on_stream_line(self, callback: Callable[[str], None]) -> None:
        """Register callback for streaming JSON lines (audio, battery, etc.)."""
        self._on_stream_line = callback

    def on_raw_line(self, callback: Callable[[str], None]) -> None:
        """Register callback for non-streaming lines (log output, etc.)."""
        self._on_raw_line = callback

    async def send_command(self, command: str, timeout: float = COMMAND_TIMEOUT_S) -> str:
        """Send a command and wait for the multi-line response.

        Automatically pauses/resumes streaming if active.
        """
        is_stream_enable = command.startswith("stream ") and command.split()[1] in (
            "on",
            "fast",
            "debug",
            "normal",
            "nn",
        )
        is_stream_disable = command == "stream off"

        async with self._command_lock:
            was_streaming = self._streaming

            # Pause streaming for non-stream commands
            if was_streaming and not is_stream_enable and not is_stream_disable:
                await self._raw_send("stream off")
                await asyncio.sleep(0.1)
                self._streaming = False

            # Send the actual command and collect response
            response = await self._send_and_collect(command, timeout)

            # Update streaming state
            if is_stream_enable:
                self._streaming = True
            elif is_stream_disable:
                self._streaming = False
            elif was_streaming:
                await self._raw_send("stream on")
                self._streaming = True

            return response

    async def get_info(self) -> dict[str, Any]:
        """Get device info as parsed JSON."""
        resp = await self.send_command("json info")
        result = self._parse_json_response(resp)
        return result if isinstance(result, dict) else {"raw": result}

    async def get_settings(self) -> list[dict[str, Any]]:
        """Get all settings as parsed JSON array."""
        resp = await self.send_command("json settings")
        data = self._parse_json_response(resp)
        if isinstance(data, dict):
            settings: list[dict[str, Any]] = data.get("settings", [])
            return settings
        if isinstance(data, list):
            return data
        return []

    async def set_setting(self, name: str, value: Any) -> str:
        return await self.send_command(f"set {name} {value}")

    async def save_settings(self) -> str:
        return await self.send_command("save")

    async def load_settings(self) -> str:
        return await self.send_command("load")

    async def restore_defaults(self) -> str:
        return await self.send_command("defaults")

    async def set_generator(self, name: str) -> str:
        return await self.send_command(f"gen {name}")

    async def set_effect(self, name: str) -> str:
        return await self.send_command(f"effect {name}")

    async def start_stream(self, mode: str = "on") -> str:
        return await self.send_command(f"stream {mode}")

    async def stop_stream(self) -> str:
        return await self.send_command("stream off")

    # ── Internal ──

    async def _raw_send(self, line: str) -> None:
        await self._transport.write_line(line)

    async def _send_and_collect(self, command: str, timeout: float) -> str:
        """Send command and accumulate response lines until a 100ms gap."""
        self._response_buf.clear()
        self._pending = True
        self._response_event.clear()

        await self._raw_send(command)

        with contextlib.suppress(TimeoutError):
            await asyncio.wait_for(self._response_event.wait(), timeout=timeout)

        self._pending = False
        result = "\n".join(self._response_buf)
        self._response_buf.clear()
        return result

    def _handle_line(self, line: str) -> None:
        """Route an incoming line from the transport."""
        # Streaming JSON data - route to subscribers
        if line.startswith("{"):
            if self._on_stream_line and not self._pending:
                self._on_stream_line(line)
                return
            # If we're collecting a command response and the line is JSON,
            # it could be the response itself (e.g., json info, json settings)
            if self._pending:
                self._add_response_line(line)
                return
            # Unsolicited JSON while not collecting - still route to stream
            if self._on_stream_line:
                self._on_stream_line(line)
            return

        # Non-JSON line during command collection
        if self._pending:
            self._add_response_line(line)
            return

        # Unsolicited non-JSON line (log output, etc.)
        if self._on_raw_line:
            self._on_raw_line(line)

    def _add_response_line(self, line: str) -> None:
        """Add a line to the response buffer, resetting the line timeout."""
        self._response_buf.append(line)
        # Cancel previous timer
        if self._response_timer:
            self._response_timer.cancel()
        # Set new timer - finalize after 100ms of silence
        loop = asyncio.get_event_loop()
        self._response_timer = loop.call_later(
            RESPONSE_LINE_TIMEOUT_S,
            self._finalize_response,
        )

    def _finalize_response(self) -> None:
        """Called when no more lines arrive within the timeout window."""
        self._response_timer = None
        self._response_event.set()

    @staticmethod
    def _parse_json_response(text: str) -> Any:
        """Try to parse JSON from a response. Try full text, then each line."""
        text = text.strip()
        if not text:
            return {}
        try:
            return json.loads(text)
        except json.JSONDecodeError:
            pass
        # Try each line individually
        for line in text.split("\n"):
            line = line.strip()
            if line.startswith("{") or line.startswith("["):
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    continue
        return {"raw": text}
