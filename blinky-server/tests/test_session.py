"""Tests for TestSession recording and Device integration."""

from __future__ import annotations

import json
import time

from blinky_server.device.device import Device
from blinky_server.testing.test_session import TestSession

from .mock_transport import MockTransport


def test_empty_session() -> None:
    """Stop recording with no data ingested."""
    session = TestSession()
    session.start_recording()
    result = session.stop_recording()
    assert result.transients == []
    assert result.music_states == []
    assert result.duration >= 0
    assert result.start_time > 0


def test_ingest_transient() -> None:
    """Transient events are captured during recording."""
    session = TestSession()
    session.start_recording()
    session.ingest("transient", {"timestampMs": 1000.0, "strength": 0.85})
    session.ingest("transient", {"timestampMs": 1500.0, "strength": 0.60})
    result = session.stop_recording()
    assert len(result.transients) == 2
    assert result.transients[0].timestamp_ms == 1000.0
    assert result.transients[0].strength == 0.85
    assert result.transients[0].type == "onset"
    assert result.transients[1].timestamp_ms == 1500.0


def test_ingest_audio_music_state() -> None:
    """Music state from audio stream is captured."""
    session = TestSession()
    session.start_recording()
    session.ingest(
        "audio",
        {
            "a": {"l": 0.5},
            "m": {"a": 1, "bpm": 120.0, "ph": 0.25, "pp": 0.8, "str": 0.7, "nn": 0.5},
        },
    )
    result = session.stop_recording()
    assert len(result.music_states) == 1
    ms = result.music_states[0]
    assert ms.active is True
    assert ms.phase == 0.25
    assert ms.plp_pulse == 0.8
    assert ms.confidence == 0.7
    assert ms.oss == 0.5
    assert ms._bpm == 120.0
    assert ms.timestamp_ms > 0


def test_ingest_audio_without_music() -> None:
    """Audio stream without 'm' key is ignored (no music state)."""
    session = TestSession()
    session.start_recording()
    session.ingest("audio", {"a": {"l": 0.3}})
    result = session.stop_recording()
    assert len(result.music_states) == 0


def test_not_recording_ignores_data() -> None:
    """Data ingested before start or after stop is dropped."""
    session = TestSession()
    session.ingest("transient", {"timestampMs": 100.0, "strength": 1.0})
    session.start_recording()
    session.ingest("transient", {"timestampMs": 200.0, "strength": 0.5})
    result = session.stop_recording()
    assert len(result.transients) == 1
    # After stop, new data is dropped
    session.ingest("transient", {"timestampMs": 300.0, "strength": 0.9})
    assert session.recording is False


def test_start_clears_previous() -> None:
    """Starting a new recording clears data from the previous one."""
    session = TestSession()
    session.start_recording()
    session.ingest("transient", {"timestampMs": 100.0, "strength": 1.0})
    session.stop_recording()
    session.start_recording()
    result = session.stop_recording()
    assert len(result.transients) == 0


def test_battery_and_status_ignored() -> None:
    """Non-audio, non-transient messages don't create data."""
    session = TestSession()
    session.start_recording()
    session.ingest("battery", {"b": {"v": 3.8, "p": 72}})
    session.ingest("status", {"type": "STATUS", "free": 1024})
    session.ingest("data", {"foo": "bar"})
    result = session.stop_recording()
    assert len(result.transients) == 0
    assert len(result.music_states) == 0


def test_device_routes_to_test_session() -> None:
    """Device._route_stream_line forwards data to attached test session."""
    transport = MockTransport()
    device = Device(
        device_id="test_device_123",
        port="/dev/ttyACM0",
        platform="nrf52840",
        transport=transport,
    )
    session = device.start_test_session()
    session.start_recording()

    # Simulate a transient event arriving on the stream
    transient_json = json.dumps({"type": "TRANSIENT", "timestampMs": 500.0, "strength": 0.9})
    device._route_stream_line(transient_json)

    # Simulate an audio frame with music state
    audio_json = json.dumps(
        {
            "a": {"l": 0.5},
            "m": {"a": 1, "bpm": 128.0, "ph": 0.5, "pp": 0.6, "str": 0.8, "nn": 0.3},
        }
    )
    device._route_stream_line(audio_json)

    result = session.stop_recording()
    assert len(result.transients) == 1
    assert result.transients[0].strength == 0.9
    assert len(result.music_states) == 1
    assert result.music_states[0].phase == 0.5


def test_device_stop_test_session() -> None:
    """After stopping test session, data is no longer captured."""
    transport = MockTransport()
    device = Device(
        device_id="test_device_456",
        port="/dev/ttyACM1",
        platform="nrf52840",
        transport=transport,
    )
    session = device.start_test_session()
    session.start_recording()

    device._route_stream_line(
        json.dumps({"type": "TRANSIENT", "timestampMs": 100.0, "strength": 0.5})
    )
    device.stop_test_session()

    # This should not be captured (session detached)
    device._route_stream_line(
        json.dumps({"type": "TRANSIENT", "timestampMs": 200.0, "strength": 0.8})
    )

    result = session.stop_recording()
    assert len(result.transients) == 1


def test_duration_tracks_wall_time() -> None:
    """Duration should approximate wall clock time between start and stop."""
    session = TestSession()
    session.start_recording()
    time.sleep(0.05)  # 50ms
    result = session.stop_recording()
    assert result.duration >= 40  # at least 40ms (allow for scheduling)
    assert result.duration < 500  # but not unreasonably long
