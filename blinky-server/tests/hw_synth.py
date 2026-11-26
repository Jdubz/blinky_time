"""Synthetic stream captures for the hardware checks' audio and response rules
(testing/hardware_checks.py): a chip that hears the stimulus and responds the way the box's chip
did on 2026-11-25, with knobs to break one feature at a time."""

from __future__ import annotations

from typing import Any

from blinky_server.testing import stimulus

STEP_MS = 10.0  # the fast stream's line period
LAG_MS = 300.0


def _hit_near(s: float) -> str | None:
    """The kind of hit whose first 120 ms holds stimulus time ``s`` (seconds), or None."""
    for sec in stimulus.RESPONSE_SECTIONS:
        if not (sec.start <= s < sec.end + 0.2):
            continue
        kinds = ("kick", "snare", "hat") if sec.kind == "groove" else (sec.kind,)
        for kind in kinds:
            for h in stimulus.hit_times(sec, kind):
                if 0.0 <= s - h < 0.12:
                    return kind
    return None


BASE = {
    "raw": 0.002,
    "l": 0.02,
    "e": 0.3,
    "p": 0.2,
    "nn": 0.05,
    "rflux": 0.01,
    "hfc": 0.05,
    "cent": 15.0,
    "roll": 0.0,
    "act": 0.04,
    "pp": 0.01,
}
AT_HIT = {
    "kick": {"raw": 0.2, "l": 0.6, "e": 0.55, "p": 0.5, "nn": 0.2, "rflux": 0.6, "hfc": 1500.0,
             "cent": 5.0, "roll": 1.0, "act": 0.07, "pp": 0.3},
    "snare": {"raw": 0.03, "l": 0.4, "e": 0.5, "p": 0.5, "nn": 0.2, "rflux": 0.2, "hfc": 120.0,
              "cent": 30.0, "roll": 3.0, "act": 0.06, "pp": 0.3},
    "hat": {"hfc": 3.0, "cent": 45.0, "roll": 40.0},
}  # fmt: skip


def response(
    start_ms: float = 1_000_000.0,
    lag_ms: float = LAG_MS,
    deaf: bool = False,
    flat: tuple[str, ...] = (),
    bpm: float = 120.0,
) -> dict[str, Any]:
    """A capture of the response stimulus. ``deaf``: nothing responds (silence throughout);
    ``flat``: those fields never move."""
    rows: list[dict[str, Any]] = []
    onsets: list[float] = []
    n = int((stimulus.RESPONSE_S + 2.0) * 1000 / STEP_MS)
    prev_ph = 0.0
    for i in range(n):
        t = start_ms + i * STEP_MS
        s = (t - start_ms - lag_ms) / 1000
        kind = None if deaf else _hit_near(s)
        row: dict[str, Any] = {"t": t, "alive": 1, **BASE}
        if kind:
            row.update(AT_HIT[kind])
        for f in flat:
            row[f] = BASE.get(f, 0.0)
        ph = (s * bpm / 60) % 1.0 if not deaf else 0.5
        row.update(
            bpm=bpm,
            per=round(60 * 62.5 / bpm),
            str=0.9,
            active=1,
            conf=0.9,
            od=3.0,
            ph=round(ph, 2),
            q=1 if prev_ph > 0.7 and ph < 0.3 else 0,
        )
        prev_ph = ph
        rows.append(row)
    if not deaf:
        for sec in stimulus.RESPONSE_SECTIONS:
            for kind in ("kick", "snare"):
                if sec.kind in (kind, "groove"):
                    onsets += [
                        start_ms + lag_ms + h * 1000 + 5 for h in stimulus.hit_times(sec, kind)
                    ]
    return {
        "start_ms": start_ms,
        "played": True,
        "devices": {"grid": {"rows": rows, "onsets": sorted(set(onsets))}},
    }


def level(start_ms: float, seconds: float, raw: float, alive: int = 1) -> dict[str, Any]:
    """A capture with the mic's raw level steady at ``raw``."""
    rows = [
        {"t": start_ms + i * STEP_MS, "raw": raw, "alive": alive}
        for i in range(int(seconds * 1000 / STEP_MS))
    ]
    return {"start_ms": start_ms, "played": True, "devices": {"grid": {"rows": rows, "onsets": []}}}
