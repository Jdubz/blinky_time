"""Per-device test recording session.

Accumulates streaming data (transient events and music states) into buffers
that can be scored against ground truth after the test completes.
"""

from __future__ import annotations

import time
from typing import Any

from .types import MusicState, TestData, TransientEvent


class TestSession:
    """Records streaming data from a device during a test.

    Attach to a Device via Device.start_test_session(). The device routes
    tagged stream messages to ingest(). Call stop_recording() to freeze
    the data into a TestData snapshot for scoring.
    """

    def __init__(self) -> None:
        self._recording = False
        self._start_time: float = 0.0
        self._transients: list[TransientEvent] = []
        self._music_states: list[MusicState] = []

    @property
    def recording(self) -> bool:
        return self._recording

    def start_recording(self) -> None:
        """Begin recording. Clears any previous data."""
        self._recording = True
        self._start_time = time.time() * 1000  # epoch ms
        self._transients.clear()
        self._music_states.clear()

    def stop_recording(self) -> TestData:
        """Stop recording and return a frozen snapshot."""
        self._recording = False
        duration = time.time() * 1000 - self._start_time
        return TestData(
            duration=duration,
            start_time=self._start_time,
            transients=list(self._transients),
            music_states=list(self._music_states),
        )

    def ingest(self, msg_type: str, data: dict[str, Any]) -> None:
        """Ingest a tagged stream message from the device.

        Called by Device._route_stream_line for each stream message while
        this session is active.

        Args:
            msg_type: Message type tag ("transient", "audio", etc.)
            data: The raw JSON data dict from the device stream
        """
        if not self._recording:
            return

        now_ms = time.time() * 1000

        if msg_type == "transient":
            self._transients.append(
                TransientEvent(
                    timestamp_ms=data.get("timestampMs", now_ms),
                    type="onset",
                    strength=data.get("strength", 0.0),
                )
            )
        elif msg_type == "audio":
            # Music state is in the "m" sub-object of the audio stream
            m = data.get("m")
            if m is None:
                return
            self._music_states.append(
                MusicState(
                    timestamp_ms=now_ms,
                    active=bool(m.get("a", 0)),
                    phase=m.get("ph", 0.0),
                    confidence=m.get("str", 0.0),  # rhythm strength as confidence
                    oss=m.get("nn", None),  # raw NN activation
                    plp_pulse=m.get("pp", None),  # PLP pulse value
                    _bpm=m.get("bpm", 0.0),  # internal: for autocorr lag
                )
            )
