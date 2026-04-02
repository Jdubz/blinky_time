"""NN diagnostic stream capture.

Captures raw mel bands + NN onset activation from firmware's `stream nn` mode.
Used for offline validation of mel feature parity between firmware and training
pipeline, and for verifying NN inference on-device.

Firmware format (one JSON per frame at ~62.5 Hz):
  {"type":"NN","ts":<ms>,"mel":[26 floats],"onset":<float>,
   "nn":<0|1>,"bpm":<float>,"phase":<float>,"rstr":<float>,
   "lvl":<float>,"gain":<float>}
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from ..device.device import Device

log = logging.getLogger(__name__)


@dataclass
class NnCaptureResult:
    frames: int
    duration_sec: float
    frame_rate: float
    output_path: str
    nn_active: bool  # Was NN model loaded?
    onset_stats: dict[str, float]  # min/max/mean of onset activation
    level_stats: dict[str, float]  # min/max/mean of mic level


def validate_output_path(output_path: str) -> Path:
    """Validate output path is under /tmp or user's home directory.

    Raises ValueError if the path is outside allowed directories.
    """
    resolved = Path(output_path).resolve()
    allowed = [Path.home(), Path("/tmp")]
    if not any(str(resolved).startswith(str(d)) for d in allowed):
        msg = f"Output path must be under /tmp or home directory, got: {resolved}"
        raise ValueError(msg)
    return resolved


async def capture_nn_stream(
    device: Device,
    duration_ms: float,
    output_path: str,
) -> NnCaptureResult:
    """Capture NN diagnostic frames from a device.

    Enables `stream nn` mode, captures frames for the specified duration,
    saves to JSONL file, and restores normal streaming state.

    Args:
        device: Connected device to capture from
        duration_ms: Capture duration in milliseconds
        output_path: Path to save JSONL output

    Returns:
        NnCaptureResult with frame count, timing, and basic stats
    """
    t0 = time.monotonic()

    # Validate output path before starting capture
    out = validate_output_path(output_path)

    # Subscribe to the device's stream to receive NN frames
    queue = device.subscribe_stream()

    # Enable NN stream mode (disables normal streaming)
    await device.protocol.send_command("stream nn")

    frames: list[dict[str, Any]] = []
    try:
        deadline = time.monotonic() + duration_ms / 1000
        while time.monotonic() < deadline:
            try:
                msg = await asyncio.wait_for(queue.get(), timeout=0.5)
                if msg is None:
                    break  # Device disconnected
                # NN frames come through as type "data" (raw JSON with type:"NN")
                raw = msg.get("data", {})
                if isinstance(raw, dict) and raw.get("type") == "NN":
                    frames.append(raw)
            except TimeoutError:
                continue
    finally:
        # Restore normal state — stop NN streaming
        try:  # noqa: SIM105
            await device.protocol.send_command("stream off")
        except Exception:
            pass
        device.unsubscribe_stream(queue)

    elapsed = time.monotonic() - t0

    # Save to JSONL
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as f:
        for frame in frames:
            f.write(json.dumps(frame) + "\n")

    # Compute stats
    onsets = [f.get("onset", 0.0) for f in frames]
    levels = [f.get("lvl", 0.0) for f in frames]
    nn_flags = [f.get("nn", 0) for f in frames]

    def _stats(values: list[float]) -> dict[str, float]:
        if not values:
            return {"min": 0.0, "max": 0.0, "mean": 0.0}
        return {
            "min": round(min(values), 4),
            "max": round(max(values), 4),
            "mean": round(sum(values) / len(values), 4),
        }

    return NnCaptureResult(
        frames=len(frames),
        duration_sec=round(elapsed, 1),
        frame_rate=round(len(frames) / elapsed, 1) if elapsed > 0 else 0.0,
        output_path=str(out),
        nn_active=any(n == 1 for n in nn_flags),
        onset_stats=_stats(onsets),
        level_stats=_stats(levels),
    )
