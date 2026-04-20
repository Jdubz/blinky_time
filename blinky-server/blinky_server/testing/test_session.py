"""Per-device test recording session.

Accumulates streaming data (transient events and music states) into buffers
that can be scored against ground truth after the test completes.
"""

from __future__ import annotations

import logging
import time
from typing import Any

from .types import MusicState, NNFrame, TestData, TransientEvent

log = logging.getLogger(__name__)


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
        self._nn_frames: list[NNFrame] = []
        self._clock_offset: float | None = None  # server_epoch_ms - firmware_millis

    @property
    def recording(self) -> bool:
        return self._recording

    def set_clock_offset(self, offset_ms: float) -> None:
        """Set firmware-to-server clock offset from a sync measurement.

        When set, transient timestamps use firmware millis() + offset
        instead of server time.time(), reducing serial transport jitter.
        """
        self._clock_offset = offset_ms

    def start_recording(self) -> None:
        """Begin recording. Clears any previous data."""
        self._recording = True
        self._start_time = time.time() * 1000  # epoch ms
        self._transients.clear()
        self._music_states.clear()
        self._nn_frames.clear()

    def stop_recording(self) -> TestData:
        """Stop recording and return a frozen snapshot."""
        self._recording = False
        duration = time.time() * 1000 - self._start_time
        return TestData(
            duration=duration,
            start_time=self._start_time,
            transients=list(self._transients),
            music_states=list(self._music_states),
            nn_frames=list(self._nn_frames),
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
            # Prefer firmware timestamps when clock offset is available —
            # firmware millis() + offset eliminates serial transport jitter
            # (~2-10ms). Falls back to server system clock otherwise.
            fw_ts = data.get("timestampMs")
            if fw_ts is not None and self._clock_offset is not None:
                ts_ms = fw_ts + self._clock_offset
            else:
                ts_ms = now_ms
            self._transients.append(
                TransientEvent(
                    timestamp_ms=ts_ms,
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
                    confidence=m.get("str", 0.0),  # rhythm strength as confidence
                    plp_pulse=m.get("pp", None),  # PLP pulse value
                    plp_period=m.get("per", None),  # ACF period in frames (for autocorr lag)
                    reliability=m.get("rel", None),  # per-bin epoch consistency (debug)
                    nn_agreement=m.get("nna", None),  # flux/NN fold agreement (debug)
                )
            )
            # Hybrid features in debug stream: flat + rflux in "m" sub-object.
            # Require BOTH fields — a partial frame (only one present) would
            # bias the missing metric toward 0 and corrupt the gap comparison.
            if "flat" in m and "rflux" in m:
                flat_val = m["flat"]
                rflux_val = m["rflux"]
                if not self._nn_frames:
                    log.info(
                        "First NN frame captured: flat=%s rflux=%s",
                        flat_val,
                        rflux_val,
                    )
                self._nn_frames.append(
                    NNFrame(
                        timestamp_ms=now_ms,
                        # m["nn"] in the music stream is the raw NN activation,
                        # not the 0/1 loaded flag (that lives in the separate
                        # NN-diagnostic stream). See NNFrame docstring.
                        activation=m.get("nn", 0.0),
                        flatness=flat_val,
                        flux=rflux_val,
                    )
                )
