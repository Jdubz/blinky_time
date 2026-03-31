"""Mock transport for testing without real hardware."""

from __future__ import annotations

import asyncio
import contextlib
import json
from collections.abc import Callable
from typing import Any

from blinky_server.transport.base import Transport


class MockTransport(Transport):
    """In-memory transport that simulates a blinky device.

    Responds to known commands with realistic device responses.
    Can inject lines (simulating streaming data) via inject_line().
    """

    def __init__(
        self,
        device_info: dict[str, Any] | None = None,
        settings: list[dict[str, Any]] | None = None,
    ) -> None:
        super().__init__()
        self._connected = False
        self._line_callback: Callable[[str], None] | None = None
        self._sent_commands: list[str] = []

        self._device_info = device_info or {
            "version": "1.0.1",
            "device": {
                "id": "long_tube_v1",
                "name": "Long Tube",
                "width": 4,
                "height": 60,
                "leds": 240,
                "configured": True,
            },
        }
        self._settings = settings or [
            {
                "name": "basespawnchance",
                "value": 0.5,
                "type": "float",
                "cat": "fire",
                "min": 0.0,
                "max": 1.0,
                "desc": "Base spark spawn chance",
            },
            {
                "name": "audiospawnboost",
                "value": 1.5,
                "type": "float",
                "cat": "fire",
                "min": 0.0,
                "max": 5.0,
                "desc": "Audio boost for spark spawning",
            },
        ]

        self._current_generator = "fire"
        self._current_effect = "none"
        self._streaming = False

    @property
    def is_connected(self) -> bool:
        return self._connected

    @property
    def transport_type(self) -> str:
        return "mock"

    @property
    def address(self) -> str:
        return "mock://test"

    @property
    def sent_commands(self) -> list[str]:
        return self._sent_commands

    async def connect(self) -> None:
        self._connected = True

    async def disconnect(self) -> None:
        self._connected = False

    def on_line(self, callback: Callable[[str], None]) -> None:
        self._line_callback = callback

    def fire_disconnect(self) -> None:
        """Simulate an unexpected transport disconnect (for testing)."""
        self._connected = False
        self._fire_disconnect()

    async def write_line(self, line: str) -> None:
        if not self._connected:
            raise ConnectionError("Not connected")
        self._sent_commands.append(line)
        # Simulate device response asynchronously
        await asyncio.sleep(0.01)  # Simulate serial latency
        response = self._generate_response(line)
        if response is not None:
            self._emit_line(response)

    def inject_line(self, line: str) -> None:
        """Inject a line as if the device sent it (for testing streaming)."""
        self._emit_line(line)

    def _emit_line(self, line: str) -> None:
        if self._line_callback:
            self._line_callback(line)

    def _generate_response(self, command: str) -> str | None:
        cmd = command.strip()

        if cmd == "json info":
            return json.dumps(self._device_info)

        if cmd == "json settings":
            return json.dumps({"settings": self._settings})

        if cmd.startswith("set "):
            parts = cmd.split(maxsplit=2)
            if len(parts) == 3:
                name, value = parts[1], parts[2]
                for s in self._settings:
                    if s["name"] == name:
                        with contextlib.suppress(ValueError):
                            s["value"] = float(value)
                        return f"OK {name} = {value}"
                return f"Unknown setting: {name}"
            return "Usage: set <name> <value>"

        if cmd.startswith("gen "):
            name = cmd[4:].strip()
            if name in ("fire", "water", "lightning", "audio"):
                self._current_generator = name
                return f"OK switched to {name.title()}"
            return f"Unknown generator: {name}"

        if cmd.startswith("effect "):
            name = cmd[7:].strip()
            if name in ("none", "hue"):
                self._current_effect = name
                return f"OK effect: {name}"
            return f"Unknown effect: {name}"

        if cmd == "stream off":
            self._streaming = False
            return "OK streaming off"

        if cmd.startswith("stream "):
            self._streaming = True
            return "OK streaming on"

        if cmd == "save":
            return "OK settings saved"

        if cmd == "load":
            return "OK settings loaded"

        if cmd == "defaults":
            return "OK defaults restored"

        if cmd == "ble":
            return "[BLE] role=scanner state=active\n[BLE] packets_rx=0 duped=0 dropped=0"

        return f"Unknown command: {cmd}"
