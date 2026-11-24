"""The staging suite's health step: it measures the steady state, not the deploy before it."""

from __future__ import annotations

import time
from typing import Any

import pytest

from blinky_server.testing import staging

health = staging.health.__wrapped__  # type: ignore[attr-defined]  # the step's body, no DBOS


async def test_health_waits_out_the_deploys_flash_write_before_reading(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A long frame from the deploy's settings save sits only in windows that close during the
    settle; health's readings all come after it."""
    settle = 0.2
    monkeypatch.setattr(staging, "HEALTH_SETTLE_S", settle)
    monkeypatch.setattr(staging, "HEALTH_POLL_S", 0.02)
    start = time.monotonic()
    calls: list[float] = []

    async def info(_device_id: str) -> dict[str, Any]:
        t = time.monotonic() - start
        calls.append(t)
        stalled = t < settle  # the window holding the save's flash erase
        return {
            "millis": int(t * 1000) + 30_000,
            "fps": 100.0,
            "maxFrameMs": 113 if stalled else 32,
            "audioOverruns": 0,
        }

    monkeypatch.setattr(staging, "_info", info)
    limits = {**staging.DEFAULTS, "stable_minutes": 0.003}
    out = await health(["chip"], limits)
    assert calls and min(calls) >= settle
    assert out["chip"]["frametime"]["pass"] and out["chip"]["frametime"]["max_ms"] == 32
