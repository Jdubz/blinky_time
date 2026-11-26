"""The hardware checks' synthetic stimuli (testing/hardware_checks.py; docs/TEST_BOX.md,
"Hardware checks"): short WAVs with isolated, known sections, so each analysis feature's yes/no
is unambiguous ("the onsets fired during the kick section and not during silence").

Generated in-process, deterministically (a fixed seed), with the standard library only: the
server's venv has no numpy, and these are a few seconds of simple synthesis. Each file is
written once per content hash under the server's data dir and reused.

Two stimuli:

- ``noise`` (the audio check): NOISE_S of broadband noise. The mic's raw level must rise
  clearly over the silence measured just before it.
- ``response`` (the response check): RESPONSE_SECTIONS below, in order. Kick-band pulses, then
  snare-band hits, then hat-band ticks, each at a fixed tempo with silence between, then a
  120 BPM pattern long enough for the tempo and the PLP phase to lock.

The synthesis (44.1 kHz would be wasted: the mic samples at 16 kHz and its mel bands stop at
8 kHz, so SAMPLE_RATE is 22.05 kHz):

| sound | made of | where its energy is |
|---|---|---|
| kick | a sine swept 180 -> 55 Hz (tau 40 ms), amplitude decay tau 180 ms, 0.3 s | 55-180 Hz |
| snare | noise band-passed at 3 kHz (Q 0.7), decay tau 70 ms, plus a 190 Hz body (tau 50 ms) | 1.5-5 kHz |
| hat | noise high-passed at 6 kHz (two biquads), decay tau 20 ms, 60 ms | 6-8 kHz at the mic |
| noise | white noise, constant level | broadband |
"""

from __future__ import annotations

import array
import hashlib
import json
import math
import random
import wave
from dataclasses import asdict, dataclass
from pathlib import Path

SAMPLE_RATE = 22050
SEED = 2026_11_25
NOISE_S = 3.0
BPM = 120.0
BEAT_S = 60.0 / BPM


@dataclass(frozen=True)
class Section:
    """One part of the response stimulus: ``kind`` from ``start`` for ``seconds``; ``hits`` is
    how many sounds of that kind it holds (the groove's are its kicks)."""

    kind: str  # silence | kick | snare | hat | groove
    start: float
    seconds: float
    period: float | None = None  # seconds between hits (kick, snare, hat)

    @property
    def end(self) -> float:
        return self.start + self.seconds

    @property
    def hits(self) -> int:
        if self.kind == "groove":
            return round(self.seconds / BEAT_S)
        if self.period is None:
            return 0
        return round(self.seconds / self.period)


def _sections() -> tuple[Section, ...]:
    plan = [
        ("silence", 2.0, None),
        ("kick", 8.0, BEAT_S),  # 16 kicks at 120 BPM
        ("silence", 3.0, None),
        ("snare", 8.0, BEAT_S),  # 16 snares at 120 BPM
        ("silence", 3.0, None),
        ("hat", 6.0, BEAT_S / 2),  # 24 hats, 8ths at 120 BPM
        ("silence", 3.0, None),
        ("groove", 24.0, None),  # kick every beat, snare on 2 and 4, hats on 8ths
        ("silence", 3.0, None),
    ]
    out, t = [], 0.0
    for kind, seconds, period in plan:
        out.append(Section(kind, t, seconds, period))
        t += seconds
    return tuple(out)


RESPONSE_SECTIONS = _sections()
RESPONSE_S = RESPONSE_SECTIONS[-1].end


# -- synthesis ---------------------------------------------------------------------------------


class _Biquad:
    """RBJ audio-EQ-cookbook biquad, direct form I."""

    def __init__(self, kind: str, f0: float, q: float) -> None:
        w0 = 2 * math.pi * f0 / SAMPLE_RATE
        alpha = math.sin(w0) / (2 * q)
        c = math.cos(w0)
        if kind == "bandpass":  # constant 0 dB peak gain
            b = (alpha, 0.0, -alpha)
        elif kind == "highpass":
            b = ((1 + c) / 2, -(1 + c), (1 + c) / 2)
        else:
            raise ValueError(f"unknown biquad kind {kind!r}")
        a0 = 1 + alpha
        self.b = tuple(x / a0 for x in b)
        self.a = (-2 * c / a0, (1 - alpha) / a0)

    def run(self, xs: list[float]) -> list[float]:
        b0, b1, b2 = self.b
        a1, a2 = self.a
        x1 = x2 = y1 = y2 = 0.0
        out = []
        for x in xs:
            y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
            x2, x1, y2, y1 = x1, x, y1, y
            out.append(y)
        return out


def _kick() -> list[float]:
    n = int(0.3 * SAMPLE_RATE)
    out, phase = [], 0.0
    for i in range(n):
        t = i / SAMPLE_RATE
        f = 55.0 + 125.0 * math.exp(-t / 0.04)
        phase += 2 * math.pi * f / SAMPLE_RATE
        out.append(math.sin(phase) * math.exp(-t / 0.18))
    return out


def _snare(rng: random.Random) -> list[float]:
    n = int(0.25 * SAMPLE_RATE)
    noise = _Biquad("bandpass", 3000.0, 0.7).run([rng.uniform(-1, 1) for _ in range(n)])
    out = []
    for i in range(n):
        t = i / SAMPLE_RATE
        body = 0.4 * math.sin(2 * math.pi * 190.0 * t) * math.exp(-t / 0.05)
        out.append(1.6 * noise[i] * math.exp(-t / 0.07) + body)
    return out


def _hat(rng: random.Random) -> list[float]:
    n = int(0.06 * SAMPLE_RATE)
    xs = [rng.uniform(-1, 1) for _ in range(n)]
    xs = _Biquad("highpass", 6000.0, 0.7).run(_Biquad("highpass", 6000.0, 0.7).run(xs))
    return [0.9 * x * math.exp(-(i / SAMPLE_RATE) / 0.02) for i, x in enumerate(xs)]


def _add(buf: list[float], sound: list[float], at_s: float, gain: float = 1.0) -> None:
    i0 = round(at_s * SAMPLE_RATE)
    for i, x in enumerate(sound[: max(0, len(buf) - i0)]):
        buf[i0 + i] += gain * x


def hit_times(section: Section, kind: str | None = None) -> list[float]:
    """The times (s, stimulus clock) of the section's hits of ``kind`` (default: its own)."""
    if section.kind == "groove":
        beats = [section.start + k * BEAT_S for k in range(section.hits)]
        if kind in (None, "kick"):
            return beats
        if kind == "snare":
            return [t for k, t in enumerate(beats) if k % 2 == 1]
        if kind == "hat":
            return [section.start + k * BEAT_S / 2 for k in range(2 * section.hits)]
        return []
    if kind not in (None, section.kind) or section.period is None:
        return []
    return [section.start + k * section.period for k in range(section.hits)]


def response_samples() -> list[float]:
    rng = random.Random(SEED)
    buf = [0.0] * math.ceil(RESPONSE_S * SAMPLE_RATE)
    kick = _kick()
    for s in RESPONSE_SECTIONS:
        if s.kind == "kick":
            for t in hit_times(s):
                _add(buf, kick, t)
        elif s.kind == "snare":
            for t in hit_times(s):
                _add(buf, _snare(rng), t)
        elif s.kind == "hat":
            for t in hit_times(s):
                _add(buf, _hat(rng), t)
        elif s.kind == "groove":
            for t in hit_times(s, "kick"):
                _add(buf, kick, t)
            for t in hit_times(s, "snare"):
                _add(buf, _snare(rng), t, 0.8)
            for t in hit_times(s, "hat"):
                _add(buf, _hat(rng), t, 0.6)
    return buf


def noise_samples() -> list[float]:
    rng = random.Random(SEED + 1)
    return [rng.uniform(-0.5, 0.5) for _ in range(int(NOISE_S * SAMPLE_RATE))]


def _write(path: Path, samples: list[float]) -> None:
    peak = max(1e-9, max(abs(x) for x in samples))
    scale = 0.8 * 32767 / peak  # normalised: 0.8 of full scale at the loudest sample
    pcm = array.array("h", (round(x * scale) for x in samples))
    tmp = path.with_suffix(".tmp")
    with wave.open(str(tmp), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm.tobytes())
    tmp.replace(path)


def _spec(kind: str) -> dict[str, object]:
    """What the file's content depends on: changing any of it makes a new file."""
    spec: dict[str, object] = {"kind": kind, "rate": SAMPLE_RATE, "seed": SEED, "v": 1}
    if kind == "noise":
        spec["seconds"] = NOISE_S
    else:
        spec["sections"] = [asdict(s) for s in RESPONSE_SECTIONS]
    return spec


def ensure(kind: str, directory: Path) -> Path:
    """The stimulus ``kind`` ("noise" | "response") as a WAV in ``directory``, made if missing
    (named by its spec's hash, so a changed spec never reuses an old file)."""
    spec = _spec(kind)
    digest = hashlib.sha256(json.dumps(spec, sort_keys=True).encode()).hexdigest()[:12]
    path = directory / f"hw-{kind}-{digest}.wav"
    if not path.exists():
        directory.mkdir(parents=True, exist_ok=True)
        _write(path, noise_samples() if kind == "noise" else response_samples())
    return path
