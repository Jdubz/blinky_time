"""The hardware checks (docs/TEST_BOX.md, "Hardware checks"; owner, 2026-11-25): one durable
job, ``test.hardware-checks``, of simple yes/no checks that block a merge. They are not quality
checks: the staging quality suite (testing/staging.py) scores the algorithm, and never blocks.

The phases run in order and the job stops at the first one that fails, because a later phase
depends on the earlier ones (no lights, and the camera can't judge anything later):

1. upload: does the firmware run? Flashed and verified through the deploy pipeline, the
   expected version, configured (not safe mode), up past the stability window (the firmware's
   markStable at 60 s; 75 s here, as deploy_local.py) with no reboot, ``json info`` sane.
2. lights: do the LEDs work? The firmware's ``testpattern`` walks the pixels while the camera
   watches the grid through the saved region map (scripts/testbox/camera/camera_lights.py):
   every region must light when its pixel is walked and stay dark otherwise.
3. audio: does the mic work? Its raw level rises clearly when the box speaker plays noise,
   against the silence just before, and no audio samples are lost meanwhile.
4. response: does each analysis feature respond to audio? A synthetic stimulus with isolated
   sections (testing/stimulus.py) and one yes/no per streamed feature (FEATURES below).
5. interfaces: serial round trip, the stream starting and stopping, a runtime settings
   round trip (never saved), the chip's BLE stack, a host BLE scan when the box has an adapter,
   and no reboot or firmware alert across the run.

Which checks a chip gets is its board's list (BOARDS, by device type).

**A rig error is not a firmware failure.** The camera or its region map unusable, the speaker
or ffplay failing, the build service or a package download failing: the job ends with
``rig_error`` set and no verdict on the firmware, and the runner tries again later. A preflight
checks the camera and the speaker before anything is flashed. A chip that stops answering is
judged a firmware failure (a crash is the likely cause; ``--retest`` covers a USB flake).

The job holds the audio lock throughout. The chips are lit (testing/leds.py) from the deploy's
end to the job's end, however it ends, and dark afterwards.

Each phase is one durable step: safe to repeat, its result small (a check list), bulk data (the
response capture) in a file.
"""

from __future__ import annotations

import asyncio
import bisect
import contextlib
import itertools
import json
import logging
import math
import os
import re
import shutil
import statistics
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..device.device import alerts_since
from ..firmware import deploy_pipeline
from ..paths import data_dir, firmware_dir
from ..work import durable, jobs
from . import leds, stimulus
from .audio_player import kill_orphan_audio, play_audio, stop_audio
from .test_runner import _configure_device, _start_streaming, _stop_streaming, _sync_clock
from .workflows import _connected, _fleet, lit, reserve

log = logging.getLogger(__name__)

YES, NO, NA = "yes", "no", "not applicable"
PHASES = ("upload", "lights", "audio", "response", "interfaces")
STABLE_UPTIME_S = 75.0  # past the firmware's markStable (60 s), as deploy_local.py waits
REBOOT_SLACK_MS = 15_000  # uptime may trail wall time this much (polling, clock rates)
MIN_FPS = 30.0  # "json info sane": the loop runs. Frame-time quality is the quality suite's
SILENCE_S = 3.0  # the audio check's silence, read before the noise plays
CAMERA_READY_S = 30.0
CAMERA_REPLY_S = 20.0
# The lights check: a whole-frame brightness rise this big when every LED is on, with the
# regions not lighting, is light the map doesn't cover (the camera or the grid moved): a rig
# error. Measured 2026-11-25: +0.54 with all 64 lit at the test brightness, 0.0 with leds 0.
MAP_MISFIT_FRAME_RISE = 0.25
LIGHTS_LEVEL = 80  # testpattern colour (white), camera_calibrate.py's default
CAMERA_PYTHON = os.environ.get("BLINKY_CAMERA_PYTHON", "/usr/bin/python3")
SETTINGS_PROBE = "hueshift"  # an effect setting: harmless, restored, never saved


class RigError(Exception):
    """The rig failed (camera, speaker, build service): no verdict on the firmware."""


@dataclass(frozen=True)
class Board:
    """What a device type's chip is checked for. ``regions``: the camera's region map for its
    LEDs (None: no camera view of them, the lights check is not applicable)."""

    platform: str
    leds: int
    regions: str | None
    ble_stack: bool  # the chip's `ble` report (nRF52840: NUS advertising, the fleet scanner)


# Per board. The ESP32-S3 on the box (fw#50) has no device config and no flash path yet; its
# entry comes with them (P3).
BOARDS: dict[str, Board] = {
    "testbox_grid_v1": Board(
        platform="nrf52840",
        leds=64,
        regions="~/.local/share/blinky-camera/regions-grid-1000us.npz",
        ble_stack=True,
    ),
}


def check(
    phase: str, name: str, device: str, result: str, rule: str, evidence: str, **data: Any
) -> dict[str, Any]:
    out: dict[str, Any] = {
        "phase": phase,
        "name": name,
        "device": device,
        "result": result,
        "rule": rule,
        "evidence": evidence,
    }
    if data:
        out["data"] = data
    return out


def phase_result(checks: list[dict[str, Any]]) -> str:
    return NO if any(c["result"] == NO for c in checks) else YES


# -- the camera worker (lights) ----------------------------------------------------------------


def _camera_script() -> Path:
    return (
        Path(__file__).resolve().parents[3] / "scripts" / "testbox" / "camera" / "camera_lights.py"
    )


class Camera:
    """scripts/testbox/camera/camera_lights.py on the system Python, one JSON line each way."""

    def __init__(self, proc: asyncio.subprocess.Process) -> None:
        self.proc = proc

    @classmethod
    async def open(cls, regions: str) -> tuple[Camera, dict[str, Any]]:
        script = _camera_script()
        if not script.is_file():
            raise RigError(f"camera: {script} missing")
        try:
            proc = await asyncio.create_subprocess_exec(
                CAMERA_PYTHON,
                str(script),
                "--regions",
                str(Path(regions).expanduser()),
                stdin=asyncio.subprocess.PIPE,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL,
            )
        except OSError as e:
            raise RigError(f"camera: can't start {CAMERA_PYTHON}: {e}") from e
        cam = cls(proc)
        try:
            ready = await cam._read(CAMERA_READY_S)
        except RigError:
            await cam.close()
            raise
        if "rig_error" in ready or not ready.get("ready"):
            await cam.close()
            raise RigError(str(ready.get("rig_error") or f"camera: not ready: {ready}"))
        return cam, ready

    async def _read(self, timeout: float) -> dict[str, Any]:
        assert self.proc.stdout is not None
        try:
            line = await asyncio.wait_for(self.proc.stdout.readline(), timeout)
        except TimeoutError as e:
            raise RigError(f"camera: no reply in {timeout:.0f} s") from e
        if not line:
            raise RigError(f"camera: the worker exited ({self.proc.returncode})")
        try:
            got: dict[str, Any] = json.loads(line)
        except ValueError as e:
            raise RigError(f"camera: garbled reply {line[:120]!r}") from e
        return got

    async def ask(self, req: dict[str, Any]) -> dict[str, Any]:
        assert self.proc.stdin is not None
        try:
            self.proc.stdin.write((json.dumps(req) + "\n").encode())
            await self.proc.stdin.drain()
        except (BrokenPipeError, ConnectionResetError) as e:
            raise RigError(f"camera: the worker went away: {e}") from e
        got = await self._read(CAMERA_REPLY_S)
        if "error" in got:
            raise RigError(f"camera: {got['error']}")
        return got

    async def close(self) -> None:
        if self.proc.returncode is None:
            with contextlib.suppress(Exception):
                assert self.proc.stdin is not None
                self.proc.stdin.write(b'{"op": "quit"}\n')
                await self.proc.stdin.drain()
            try:
                await asyncio.wait_for(self.proc.wait(), 10)
            except TimeoutError:
                self.proc.kill()
                await self.proc.wait()


# -- json info ---------------------------------------------------------------------------------


async def _info(device: Any) -> dict[str, Any]:
    return await deploy_pipeline._json_info(device)


async def _reply(device: Any, command: str) -> str:
    return str(await device.protocol.send_command(command))


# -- preflight ---------------------------------------------------------------------------------


def ffplay() -> str | None:
    return shutil.which("ffplay")


@durable.step
async def preflight(regions: list[str], need_audio: bool) -> dict[str, Any]:
    """The rig, before anything is flashed: each region map opens on the camera, ffplay is
    there, the stimuli are written. {"error": None | "..."}."""
    t0 = time.monotonic()
    try:
        if need_audio:
            if ffplay() is None:
                raise RigError("speaker: ffplay not found (install ffmpeg)")
            for kind in ("noise", "response"):
                await asyncio.to_thread(stimulus.ensure, kind, data_dir() / "stimuli")
        for path in regions:
            cam, _ = await Camera.open(path)
            await cam.close()
    except RigError as e:
        return {"error": str(e), "seconds": round(time.monotonic() - t0, 1)}
    return {"error": None, "seconds": round(time.monotonic() - t0, 1)}


@durable.step
async def boards(device_ids: list[str]) -> dict[str, str | None]:
    """Each chip's device type (its board), from the fleet."""
    return {d.id: d.device_type for d in await _connected(device_ids)}


@durable.step
async def versions_before(device_ids: list[str]) -> dict[str, str | None]:
    """What each chip runs before the flash: a chip still on it afterwards never took the
    write (a rig fault), unlike one that took it and doesn't come back (the firmware's)."""
    out: dict[str, str | None] = {}
    for d in await _connected(device_ids):
        try:
            out[d.id] = (await _info(d)).get("version")
        except Exception as e:  # unreadable before: no evidence either way
            log.warning("hardware checks: %s version before the flash unreadable: %s", d.id, e)
            out[d.id] = None
    return out


# -- 1. upload ---------------------------------------------------------------------------------


def judge_deploy(deploy: dict[str, Any], before: dict[str, str | None]) -> tuple[str, str]:
    """(result, evidence) for the flash: YES, NO (the firmware), or "rig" (no verdict)."""
    if deploy.get("status") == "ok":
        return YES, f"{deploy.get('version')} flashed and verified"
    step, msg = deploy.get("failed_step"), str(deploy.get("message") or "")
    if step in ("targets", "release", "store"):
        # Nothing was written: the build service, a download or a package check, the chips.
        return "rig", f"deploy failed before the flash ({step}): {msg}"
    if step == "flash":
        per = ((deploy.get("steps") or {}).get("flash") or {}).get("per_device") or {}
        unchanged = [
            d
            for d, r in per.items()
            if r.get("flash") != "ok" and r.get("version") and r.get("version") in before.values()
        ]
        if unchanged and len(unchanged) == len([r for r in per.values() if r.get("flash") != "ok"]):
            return "rig", f"the flash didn't take (still on {per[unchanged[0]]['version']}): {msg}"
    return NO, f"deploy failed at {step}: {msg}"


@durable.step
async def upload_checks(
    device_ids: list[str], expected: str | None, board_of: dict[str, str | None]
) -> dict[str, Any]:
    """Version, configured, stable past the window, json info sane. Waits until every chip has
    run STABLE_UPTIME_S, polling for a reboot meanwhile (a crash loop after the flash shows as
    an uptime that falls back)."""
    t0 = time.monotonic()
    out: list[dict[str, Any]] = []
    for d in await _connected(device_ids):
        board = BOARDS.get(board_of.get(d.id) or "")
        try:
            info = await _info(d)
        except Exception as e:
            for name in ("version", "configured", "stable", "info"):
                out.append(check("upload", name, d.id, NO, "json info answers", f"no reply: {e}"))
            continue
        version = info.get("version")
        if expected:
            out.append(
                check(
                    "upload",
                    "version",
                    d.id,
                    YES if version == expected else NO,
                    "json info version is the build's",
                    f"{version}" + ("" if version == expected else f", expected {expected}"),
                )
            )
        else:
            out.append(
                check("upload", "version", d.id, YES, "json info reports a version", f"{version}")
            )
        dev = info.get("device") or {}
        configured = bool(dev.get("configured")) and not dev.get("safeMode")
        want_type = board_of.get(d.id)
        out.append(
            check(
                "upload",
                "configured",
                d.id,
                YES if configured and (not want_type or dev.get("id") == want_type) else NO,
                "configured as its board, not safe mode",
                f"device {dev.get('id')!r}" if configured else f"unconfigured: {dev}",
            )
        )
        # Up past the stability window, no reboot meanwhile.
        rebooted, last = False, int(info.get("millis", 0))
        start_ms, start_t = last, time.time()
        while last < STABLE_UPTIME_S * 1000:
            await asyncio.sleep(min(5.0, max(0.5, STABLE_UPTIME_S - last / 1000)))
            try:
                ms = int((await _info(d)).get("millis", 0))
            except Exception as e:
                rebooted, last = True, 0
                log.warning("hardware checks: %s stopped answering while stabilising: %s", d.id, e)
                break
            if ms < last:
                rebooted = True
            last = ms
        info2 = await _info(d) if not rebooted else info
        reason = info2.get("resetReason")
        out.append(
            check(
                "upload",
                "stable",
                d.id,
                NO if rebooted else YES,
                f"up {STABLE_UPTIME_S:.0f} s (past markStable) with no reboot",
                f"rebooted (uptime {last // 1000} s)"
                if rebooted
                else f"uptime {int(info2.get('millis', 0)) // 1000} s, no reboot "
                f"(watched from {start_ms // 1000} s for {time.time() - start_t:.0f} s)"
                + (f", resetReason {reason}" if reason is not None else ""),
            )
        )
        problems = sane(info2, d.id, board)
        out.append(
            check(
                "upload",
                "info",
                d.id,
                NO if problems else YES,
                f"json info complete, fps >= {MIN_FPS:.0f}, no ISR allocations",
                "; ".join(problems)
                if problems
                else f"fps {info2.get('fps')}, maxFrameMs {info2.get('maxFrameMs')}, "
                f"heap {info2.get('heapUsed')}/{info2.get('heapTotal')}",
            )
        )
    return {"checks": out, "seconds": round(time.monotonic() - t0, 1)}


INFO_KEYS = ("version", "millis", "audioOverruns", "audioSamplesLost", "fps", "maxFrameMs")


def sane(info: dict[str, Any], device_id: str, board: Board | None) -> list[str]:
    """What's wrong with a json info reply, for "json info sane" (empty: nothing)."""
    bad = [f"no {k}" for k in INFO_KEYS if info.get(k) is None]
    fps = info.get("fps")
    if fps is not None and float(fps) < MIN_FPS:
        bad.append(f"fps {fps} < {MIN_FPS:.0f}")
    if info.get("mallocIsr"):
        bad.append(f"mallocIsr {info['mallocIsr']}")
    if board is not None:
        if info.get("platform") != board.platform:
            bad.append(f"platform {info.get('platform')!r}, not {board.platform}")
        leds_ = (info.get("device") or {}).get("leds")
        if leds_ != board.leds:
            bad.append(f"{leds_} LEDs, not {board.leds}")
    sn = info.get("sn")
    if sn and re.fullmatch(r"[0-9A-F]{16}", device_id) and sn != device_id:
        bad.append(f"sn {sn} isn't the device's {device_id}")
    return bad


# -- 2. lights ---------------------------------------------------------------------------------


@durable.step
async def lights(
    device_ids: list[str], board_of: dict[str, str | None], regions: str | None, brightness: int
) -> dict[str, Any]:
    """Per chip with a camera view: ``leds`` lights it, then the walk. A rig fault raises
    nothing: it returns {"rig_error": ...}."""
    t0 = time.monotonic()
    out: list[dict[str, Any]] = []
    for d in await _connected(device_ids):
        board = BOARDS.get(board_of.get(d.id) or "")
        path = regions or (board.regions if board else None)
        if path is None:
            out.append(
                check("lights", "walk", d.id, NA, "every LED lights", "no camera view of its LEDs")
            )
            continue
        try:
            out += await _lights_one(d, path, brightness)
        except RigError as e:
            return {"checks": out, "rig_error": str(e), "seconds": round(time.monotonic() - t0, 1)}
        finally:
            with contextlib.suppress(Exception):
                await _reply(d, "testpattern off")
            if brightness != leds.TEST_BRIGHTNESS:
                with contextlib.suppress(Exception):  # the run's level for what follows
                    await leds.send(d, str(leds.TEST_BRIGHTNESS))
    return {"checks": out, "rig_error": None, "seconds": round(time.monotonic() - t0, 1)}


async def _lights_one(d: Any, regions: str, brightness: int) -> list[dict[str, Any]]:
    rule_leds = f"`leds {brightness}` applies (runtime override)"
    try:
        status = await leds.send(d, str(brightness))
    except (ValueError, OSError, TimeoutError) as e:
        return [check("lights", "leds", d.id, NO, rule_leds, f"`leds` failed: {e}")]
    if status is None or status.get("brightness") != brightness or not status.get("override"):
        return [check("lights", "leds", d.id, NO, rule_leds, f"`leds {brightness}` left {status}")]
    out = [check("lights", "leds", d.id, YES, rule_leds, f"{status}")]
    info = await _info(d)
    dev = info.get("device") or {}
    width, height = int(dev.get("width") or 0), int(dev.get("height") or 0)
    rule = "every pixel lights its own region when walked, none lights otherwise"
    cam, ready = await Camera.open(regions)
    t0 = time.monotonic()
    try:
        reply = await _reply(d, "testpattern solid 0 0 0")
        if "OK" not in reply:
            out.append(
                check("lights", "walk", d.id, NO, rule, f"testpattern refused: {reply[:80]}")
            )
            return out
        await asyncio.sleep(0.2)
        dark = await cam.ask({"op": "dark", "n": 30})
        lvl = LIGHTS_LEVEL
        await _reply(d, f"testpattern solid {lvl} {lvl} {lvl}")
        allon = await cam.ask({"op": "lit", "skip": 4, "n": 3})
        n = int(ready.get("regions") or 0)
        if len(allon["lit"]) < 0.9 * n and float(allon["frame_rise"]) >= MAP_MISFIT_FRAME_RISE:
            raise RigError(
                f"camera: the region map doesn't fit ({len(allon['lit'])}/{n} regions lit with "
                f"every LED on, but the frame brightened by {allon['frame_rise']}): the camera "
                "or the grid moved; re-run the survey"
            )
        for y in range(height):
            for x in range(width):
                await _reply(d, f"testpattern pixel {x} {y} {lvl} {lvl} {lvl}")
                await cam.ask({"op": "lit", "skip": 4, "n": 3, "key": f"{x},{y}"})
        rep = await cam.ask({"op": "report", "width": width, "height": height})
    finally:
        await cam.close()
    ok = bool(rep.get("ok"))
    if ok:
        ev = (
            f"{width * height}/{n} pixels each lit one region, all {n} regions seen"
            f" (all-on: {len(allon['lit'])}/{n}; facing {'reading order' if rep.get('reading_order') else 'another way'})"
        )
    else:
        parts = []
        for key in ("dead", "ambiguous", "unseen"):
            if rep.get(key):
                parts.append(f"{len(rep[key])} {key}")
        if rep.get("shared"):
            parts.append(f"{len(rep['shared'])} shared regions")
        ev = ", ".join(parts) + f" (all-on: {len(allon['lit'])}/{n} regions lit)"
    out.append(
        check(
            "lights",
            "walk",
            d.id,
            YES if ok else NO,
            rule,
            ev,
            dead=rep.get("dead", [])[:64],
            unseen=rep.get("unseen", [])[:64],
            ambiguous=rep.get("ambiguous", [])[:16],
            shared=dict(list((rep.get("shared") or {}).items())[:16]),
            camera_s=round(time.monotonic() - t0, 1),
            dark_frame_mean=dark.get("frame_mean"),
            allon_frame_rise=allon.get("frame_rise"),
        )
    )
    return out


# -- capture (audio, response) -----------------------------------------------------------------

# The stream fields kept per line (debug + fast mode), by the name a row keeps them under: the
# level object "a", the music object "m" (whose "a", rhythm active, becomes "active") and the
# frame metrics "v".
FIELDS: dict[str, tuple[str, str]] = {
    "raw": ("a", "raw"),
    "l": ("a", "l"),
    "alive": ("a", "alive"),
    "act": ("v", "act"),
    "active": ("m", "a"),
    **{
        k: ("m", k)
        for k in (
            "e",
            "p",
            "nn",
            "pp",
            "bpm",
            "ph",
            "q",
            "str",
            "per",
            "od",
            "conf",
            "cent",
            "roll",
            "hfc",
            "rflux",
        )
    },
}


def _row(data: dict[str, Any], offset: float) -> dict[str, Any] | None:
    """One stream line as a row on the host clock (``t``, epoch ms), or None without a
    firmware timestamp."""
    m = data.get("m") or {}
    if "ts" not in m:
        return None
    row: dict[str, Any] = {"t": float(m["ts"]) + offset}
    for name, (sub, key) in FIELDS.items():
        v = (data.get(sub) or {}).get(key)
        if v is not None:
            row[name] = v
    return row


async def capture(devices: list[Any], wav: str | None, seconds: float) -> dict[str, Any]:
    """Each chip's stream while ``wav`` plays (or ``seconds`` of silence with None): rows on the
    host clock (epoch ms, through the firmware clock: _sync_clock), detections likewise, and
    when the playback started. Streaming (debug, fast, detections) only for the capture."""
    offsets: dict[str, float] = {}
    for d in devices:
        sync = await _sync_clock(d)
        offsets[d.id] = float(sync["offset_ms"]) if sync else math.nan
    queues = {d.id: d.subscribe_stream() for d in devices}
    got: dict[str, dict[str, list[Any]]] = {d.id: {"rows": [], "onsets": []} for d in devices}

    async def drain(dev: str, q: asyncio.Queue[dict[str, Any] | None]) -> None:
        while True:
            msg = await q.get()
            if msg is None:
                return
            off = offsets[dev]
            if math.isnan(off):  # no sync: the arrival time, less transport
                off = time.time() * 1000 - float(
                    ((msg.get("data") or {}).get("m") or {}).get("ts", 0)
                )
            data = msg.get("data") or {}
            if msg.get("type") == "audio":
                row = _row(data, off)
                if row:
                    got[dev]["rows"].append(row)
            elif msg.get("type") == "transient" and data.get("timestampMs") is not None:
                got[dev]["onsets"].append(float(data["timestampMs"]) + off)

    tasks = [asyncio.create_task(drain(i, q)) for i, q in queues.items()]
    result: dict[str, Any] = {"offsets": offsets}
    try:
        await kill_orphan_audio()
        await _start_streaming(devices)
        await asyncio.sleep(1.0)
        if wav is None:
            result["start_ms"] = time.time() * 1000
            await asyncio.sleep(seconds)
            result["played"] = True
        else:
            pb = await play_audio(wav)
            result["start_ms"] = pb.audio_start_time_ms
            result["played"] = pb.success
            result["play_error"] = pb.error
            await asyncio.sleep(1.0)  # the stimulus' tail through the lag
    finally:
        await _stop_streaming(devices)
        await stop_audio()
        for t in tasks:
            t.cancel()
        for d in devices:
            d.unsubscribe_stream(queues[d.id])
    result["devices"] = got
    return result


def _vals(rows: list[dict[str, Any]], key: str, a_ms: float, b_ms: float) -> list[float]:
    """``key`` in the rows from ``a_ms`` to ``b_ms`` (rows in time order: bisected)."""
    lo = bisect.bisect_left(rows, a_ms, key=lambda r: r["t"])
    hi = bisect.bisect_left(rows, b_ms, lo=lo, key=lambda r: r["t"])
    return [float(r[key]) for r in rows[lo:hi] if r.get(key) is not None]


def _median(xs: list[float]) -> float | None:
    return statistics.median(xs) if xs else None


def _rnd(x: float | None, n: int = 4) -> float | None:
    return None if x is None else round(x, n)


# -- 3. audio ----------------------------------------------------------------------------------

MIC_RISE = 4.0  # noise median raw level at least this many times silence's median


def judge_audio(
    device_id: str,
    silence: dict[str, Any],
    noise: dict[str, Any],
    before: dict[str, Any],
    after: dict[str, Any],
) -> list[dict[str, Any]]:
    s_rows = silence["devices"][device_id]["rows"]
    n_rows = sorted(noise["devices"][device_id]["rows"], key=lambda r: float(r["t"]))
    t0 = float(noise["start_ms"])
    # The noise's middle (past the speaker's start lag, before its end).
    quiet = [float(r["raw"]) for r in s_rows if r.get("raw") is not None]
    loud = _vals(n_rows, "raw", t0 + 1000, t0 + (stimulus.NOISE_S - 0.2) * 1000)
    q, lo = _median(quiet), _median(loud)
    alive = [r.get("alive") for r in s_rows + n_rows if r.get("alive") is not None]
    rule = f"raw level with noise >= {MIC_RISE:g}x silence's (medians), PDM alive throughout"
    if not quiet or not loud:
        mic = check(
            "audio",
            "mic",
            device_id,
            NO,
            rule,
            f"no level lines (silence {len(quiet)}, noise {len(loud)})",
        )
    else:
        ok = (
            q is not None
            and lo is not None
            and lo >= MIC_RISE * max(q, 1e-5)
            and all(a == 1 for a in alive)
        )
        dead = sum(1 for a in alive if a != 1)
        mic = check(
            "audio",
            "mic",
            device_id,
            YES if ok else NO,
            rule,
            f"raw {_rnd(q)} silent -> {_rnd(lo)} with noise ({(lo or 0) / max(q or 0, 1e-5):.0f}x)"
            + (f"; PDM dead in {dead} lines" if dead else ""),
            silence=_rnd(q),
            noise=_rnd(lo),
        )
    grew = {
        k: int(after.get(k) or 0) - int(before.get(k) or 0)
        for k in ("audioOverruns", "audioSamplesLost")
    }
    counters = check(
        "audio",
        "samples",
        device_id,
        YES if not any(grew.values()) else NO,
        "no audio overruns or samples lost while listening",
        ", ".join(f"{k} +{v}" for k, v in grew.items()),
    )
    return [mic, counters]


@durable.step
async def audio(device_ids: list[str]) -> dict[str, Any]:
    t0 = time.monotonic()
    devices = await _connected(device_ids)
    for d in devices:
        await _configure_device(d, None)  # runtime defaults, as a test session (never saved)
    before = {d.id: await _info(d) for d in devices}
    silence = await capture(devices, None, SILENCE_S)
    wav = await asyncio.to_thread(stimulus.ensure, "noise", data_dir() / "stimuli")
    noise = await capture(devices, str(wav), stimulus.NOISE_S)
    if not noise.get("played"):
        return {
            "checks": [],
            "rig_error": f"speaker: {noise.get('play_error') or 'playback failed'}",
            "seconds": round(time.monotonic() - t0, 1),
        }
    after = {d.id: await _info(d) for d in devices}
    out: list[dict[str, Any]] = []
    for d in devices:
        out += judge_audio(d.id, silence, noise, before[d.id], after[d.id])
    return {"checks": out, "rig_error": None, "seconds": round(time.monotonic() - t0, 1)}


# -- 4. response -------------------------------------------------------------------------------

LAG_RANGE_MS = (100, 900)  # ffplay's start to the sound at the mic: 0.24-0.53 s measured
LAG_TOL_MS = 40  # a detection within this of a hit (shifted by the lag) matches it
ON_WINDOW_MS = (-20, 120)  # a feature's samples "at a hit": from just before to 120 ms after
DEFAULT_LAG_MS = 300.0
LATE_S = 12.0  # the groove's last seconds: tempo and phase judged once locked


def _sec(kind: str) -> stimulus.Section:
    return next(s for s in stimulus.RESPONSE_SECTIONS if s.kind == kind)


def _hits_ms(t0: float, section: stimulus.Section, kind: str | None = None) -> list[float]:
    return [t0 + h * 1000 for h in stimulus.hit_times(section, kind)]


def find_lag(
    onsets: list[float], rows: list[dict[str, Any]], t0: float
) -> tuple[float, str, int, int]:
    """The rig's lag this run (ms): where the most kick/snare hits have a detection within
    LAG_TOL_MS (as the scorer searches it); if under half match, where the mic's raw level
    peaks after the hits; else DEFAULT_LAG_MS. (lag, source, matched, hits)."""
    hits = [h for k in ("kick", "snare") for h in _hits_ms(t0, _sec(k))] + _hits_ms(
        t0, _sec("groove"), "kick"
    )
    ons = sorted(onsets)
    counts = {
        lag: sum(1 for h in hits if _near(ons, h + lag, LAG_TOL_MS))
        for lag in range(LAG_RANGE_MS[0], LAG_RANGE_MS[1] + 1, 10)
    }
    top = max(counts.values())
    best = (top, statistics.median_low([lag for lag, n in counts.items() if n == top]))
    if best[0] >= len(hits) / 2:
        # The middle of the lags that match the most: the tolerance makes a plateau.
        return float(best[1]), "detections", best[0], len(hits)
    level = (-1.0, DEFAULT_LAG_MS)
    for lag in range(LAG_RANGE_MS[0], LAG_RANGE_MS[1] + 1, 10):
        v = [x for h in hits for x in _vals(rows, "raw", h + lag, h + lag + 40)]
        if v and statistics.fmean(v) > level[0]:
            level = (statistics.fmean(v), float(lag))
    if level[0] > 0:
        return level[1], "mic level", best[0], len(hits)
    return DEFAULT_LAG_MS, "default", best[0], len(hits)


def _near(sorted_xs: list[float], x: float, tol: float) -> bool:
    i = bisect.bisect_left(sorted_xs, x - tol)
    return i < len(sorted_xs) and sorted_xs[i] <= x + tol


def _on_off(
    rows: list[dict[str, Any]], hits: list[float], key: str, lag: float, half: float
) -> tuple[float | None, float | None]:
    """Mean of ``key`` in the windows at the hits and half a period later (between hits)."""

    def mean_at(cs: list[float]) -> float | None:
        v = [x for c in cs for x in _vals(rows, key, c + ON_WINDOW_MS[0], c + ON_WINDOW_MS[1])]
        return statistics.fmean(v) if v else None

    on = [h + lag for h in hits]
    return mean_at(on), mean_at([c + half for c in on])


@dataclass(frozen=True)
class Feature:
    name: str
    field: str  # the stream field (TRANSIENT: the detection events)
    rule: str
    judge: Callable[[dict[str, Any]], tuple[bool, str, dict[str, Any]]]


def _ctx_onoff(ctx: dict[str, Any], key: str, kind: str) -> tuple[float | None, float | None]:
    sec = _sec(kind)
    half = (sec.period or stimulus.BEAT_S) * 500
    return _on_off(ctx["rows"], _hits_ms(ctx["t0"], sec), key, ctx["lag"], half)


def _diff_rule(
    key: str, kinds: tuple[str, ...], min_diff: float = 0.0, min_ratio: float = 0.0
) -> Callable[[dict[str, Any]], tuple[bool, str, dict[str, Any]]]:
    def judge(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
        ok, parts, data = True, [], {}
        for kind in kinds:
            on, off = _ctx_onoff(ctx, key, kind)
            data[kind] = [_rnd(on), _rnd(off)]
            if on is None or off is None:
                ok = False
                parts.append(f"{kind}: no samples")
                continue
            good = on - off >= min_diff and on >= min_ratio * off and on > off
            ok = ok and good
            parts.append(f"{kind} {_rnd(on, 3)} vs {_rnd(off, 3)}")
        return ok, "; ".join(parts) + " (at hits vs between)", data

    return judge


def _onsets(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    """Detections at the kick and at the snare hits (each), more than between hits (pooled:
    a kick's tail still sounds half a beat on, and the chip detects in it too)."""
    ons, ok, parts, data = sorted(ctx["onsets"]), True, [], {}
    n_on = n_off = n = 0
    for kind in ("kick", "snare"):
        sec = _sec(kind)
        half = (sec.period or stimulus.BEAT_S) * 500
        hits = [h + ctx["lag"] for h in _hits_ms(ctx["t0"], sec)]
        on = sum(1 for h in hits if _near(ons, h + 20, 60))  # -40..+80 ms
        off = sum(1 for h in hits if _near(ons, h + half + 20, 60))
        n_on, n_off, n = n_on + on, n_off + off, n + len(hits)
        data[kind] = [round(on / len(hits), 2), round(off / len(hits), 2)]
        ok = ok and on / len(hits) >= ONSET_MIN_HIT
        parts.append(f"{kind} {on / len(hits):.0%} of hits (between hits {off / len(hits):.0%})")
    ok = ok and (n_on - n_off) / n >= ONSET_MARGIN
    parts.append(f"pooled {n_on / n:.0%} vs {n_off / n:.0%}")
    return ok, "; ".join(parts), data


def _groove_late(ctx: dict[str, Any], key: str) -> list[float]:
    g = _sec("groove")
    a = ctx["t0"] + (g.end - LATE_S) * 1000 + ctx["lag"]
    return _vals(ctx["rows"], key, a, a + LATE_S * 1000)


def _tempo_ok(bpm: float | None, tol: float) -> bool:
    return bpm is not None and any(
        abs(bpm - t) <= tol * t for t in (stimulus.BPM, stimulus.BPM / 2, stimulus.BPM * 2)
    )


def _tempo(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    bpm = _median(_groove_late(ctx, "bpm"))
    return (
        _tempo_ok(bpm, TEMPO_TOL),
        f"median {bpm} BPM over the groove's last {LATE_S:g} s",
        {"bpm": bpm},
    )


def _period(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    per = _median(_groove_late(ctx, "per"))
    bpm = 60 * FRAME_HZ / per if per else None
    return (
        _tempo_ok(bpm, PERIOD_TOL),
        f"median period {per} frames = {_rnd(bpm, 1)} BPM",
        {"per": per},
    )


def _strength(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    st = _median(_groove_late(ctx, "str"))
    act = _groove_late(ctx, "active")
    frac = sum(1 for a in act if a == 1) / len(act) if act else 0.0
    ok = st is not None and st >= MIN_RHYTHM and frac >= 0.5
    return (
        ok,
        f"median strength {st}, active {frac:.0%} of the groove's last {LATE_S:g} s",
        {"str": st, "active": round(frac, 2)},
    )


def _periodicity(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    c = _median(_groove_late(ctx, "conf"))
    return (
        c is not None and c >= MIN_RHYTHM,
        f"median {c} over the groove's last {LATE_S:g} s",
        {"conf": c},
    )


def _plp_phase(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    """The PLP phase locks: at the groove's late kicks it sits at one value (circular
    concentration), and it wraps once per beat of the reported tempo."""
    g = _sec("groove")
    rows = ctx["rows"]
    kicks = [
        h + ctx["lag"] for h in _hits_ms(ctx["t0"], g) if h >= ctx["t0"] + (g.end - LATE_S) * 1000
    ]
    ph = []
    for k in kicks:
        near = [r for r in rows if abs(r["t"] - k) <= 15 and r.get("ph") is not None]
        if near:
            ph.append(float(near[0]["ph"]))
    if not ph:
        return False, "no phase samples at the kicks", {}
    c = statistics.fmean(math.cos(2 * math.pi * p) for p in ph)
    s = statistics.fmean(math.sin(2 * math.pi * p) for p in ph)
    conc = math.hypot(c, s)
    late = [r for r in rows if r.get("ph") is not None and kicks[0] <= r["t"] < kicks[-1] + 500]
    wraps = sum(
        1 for a, b in itertools.pairwise(late) if float(a["ph"]) > 0.7 and float(b["ph"]) < 0.3
    )
    secs = max((late[-1]["t"] - late[0]["t"]) / 1000, 1e-3) if late else 1e-3
    bpm = _median(_groove_late(ctx, "bpm")) or 0.0
    rate, want = wraps / secs, bpm / 60
    ok = conc >= MIN_PHASE_CONC and want > 0 and abs(rate - want) <= WRAP_TOL * want
    return (
        ok,
        f"concentration {conc:.2f} at {len(ph)} kicks; {rate:.2f} wraps/s for {want:.2f} beats/s",
        {"conc": round(conc, 2), "wraps_per_s": round(rate, 2)},
    )


def _beats(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    q = _groove_late(ctx, "q")
    bpm = _median(_groove_late(ctx, "bpm")) or 0.0
    n = sum(1 for x in q if x == 1)
    want = bpm / 60 * LATE_S
    ok = want > 0 and abs(n - want) <= WRAP_TOL * want + 1
    return ok, f"{n} beat events in {LATE_S:g} s for {want:.1f} beats at {bpm} BPM", {"beats": n}


def _density(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    g = _sec("groove")
    a, b = ctx["t0"] + g.start * 1000 + ctx["lag"], ctx["t0"] + g.end * 1000 + ctx["lag"]
    rate = sum(1 for t in ctx["onsets"] if a <= t < b) / g.seconds
    od = _median(_groove_late(ctx, "od"))
    ok = od is not None and od >= 0.5 and rate > 0 and 0.5 * rate <= od <= 2 * rate
    return (
        ok,
        f"median {od}/s, detections {rate:.1f}/s in the groove",
        {"od": od, "rate": round(rate, 2)},
    )


def _bands(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    """Spectral centroid at the hits: low for kicks, higher for snares and hats."""
    at = {k: _ctx_onoff(ctx, "cent", k)[0] for k in ("kick", "snare", "hat")}
    k, s, h = at["kick"], at["snare"], at["hat"]
    ok = None not in (k, s, h) and min(s or 0, h or 0) >= BAND_RATIO * (k or 0) and (k or 0) >= 0
    return (
        ok,
        f"centroid at kicks {_rnd(k, 1)}, snares {_rnd(s, 1)}, hats {_rnd(h, 1)}",
        {k_: _rnd(v, 2) for k_, v in at.items()},
    )


def _rolloff(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    k = _ctx_onoff(ctx, "roll", "kick")[0]
    h = _ctx_onoff(ctx, "roll", "hat")[0]
    ok = k is not None and h is not None and h >= BAND_RATIO * k + 1
    return (
        ok,
        f"rolloff at kicks {_rnd(k, 1)}, hats {_rnd(h, 1)}",
        {"kick": _rnd(k, 2), "hat": _rnd(h, 2)},
    )


def _plp_pulse(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    g = _sec("groove")
    hits = [h for h in _hits_ms(ctx["t0"], g) if h >= ctx["t0"] + (g.end - LATE_S) * 1000]
    on, off = _on_off(ctx["rows"], hits, "pp", ctx["lag"], stimulus.BEAT_S * 500)
    ok = on is not None and off is not None and on - off >= 0.05
    return (
        ok,
        f"{_rnd(on, 3)} at the groove's late kicks vs {_rnd(off, 3)} between",
        {"on": _rnd(on), "off": _rnd(off)},
    )


def _render(ctx: dict[str, Any]) -> tuple[bool, str, dict[str, Any]]:
    """The rendered frame changes more at a hit than between hits (kick and snare pooled):
    the generator draws the audio."""
    ons, offs = [], []
    for kind in ("kick", "snare"):
        on, off = _ctx_onoff(ctx, "act", kind)
        if on is not None and off is not None:
            ons.append(on)
            offs.append(off)
    if not ons:
        return False, "no frame metrics in the stream", {}
    on, off = statistics.fmean(ons), statistics.fmean(offs)
    return (
        on >= RENDER_RATIO * off and on > off,
        f"frame activity {on:.3f} at hits vs {off:.3f} between",
        {"on": round(on, 4), "off": round(off, 4)},
    )


# The response thresholds: loose, "is it alive and wired", not accuracy. Measured on the box
# 2026-11-25 (b211-dev, the stimulus at the box speaker's level), at hits vs between hits:
# detections kick 94% vs 56%, snare 100% vs 12% (a second run: kick 100% vs 94%, the kick's
# tail, so the contrast is pooled over kick and snare); level 0.68 vs 0.02 and 0.43 vs 0.01; energy
# 0.58 vs 0.30, 0.49 vs 0.30; pulse 0.47 vs 0.31, 0.54 vs 0.15; activation 0.15 vs 0.09, 0.16
# vs 0.05; flux 0.71 vs 0.02, 0.17 vs 0.002; HFC (snare, hat) 125 vs 0.09, 2.6 vs 0.04;
# centroid at kicks 5, snares 31, hats 45; rolloff kicks 1.3, hats 37; tempo 119.9; period 31
# frames; strength 0.89, periodicity 0.89; phase concentration 1.0; frame activity 0.072 vs
# 0.056 (kick), 0.061 vs 0.038 (snare). The firmware detects ~3.5/s in the room's silence, so
# no rule counts detections in silence: every rule compares hits with between-hits instead.
ONSET_MIN_HIT = 0.6
ONSET_MARGIN = 0.2
TEMPO_TOL = 0.06
PERIOD_TOL = 0.08
FRAME_HZ = 62.5  # the spectral frame rate the ACF period counts (16 kHz / 256)
MIN_RHYTHM = 0.5
MIN_PHASE_CONC = 0.6
WRAP_TOL = 0.25
BAND_RATIO = 2.0
RENDER_RATIO = 1.1

# One yes/no per feature the firmware streams (blinky-firmware SerialConsole::streamTick, the
# debug + fast stream, and the TRANSIENT detection events). It has no per-drum detector: the
# kick, snare and hat bands show in the spectral shape (centroid, rolloff, HFC, flux).
FEATURES: tuple[Feature, ...] = (
    Feature(
        "onsets",
        "TRANSIENT",
        f"a detection at >= {ONSET_MIN_HIT:.0%} of kick and of snare hits, {ONSET_MARGIN:.0%} more than between hits (pooled)",
        _onsets,
    ),
    Feature(
        "activation",
        "m.nn",
        "onset activation higher at kick and snare hits than between (x1.3)",
        _diff_rule("nn", ("kick", "snare"), min_ratio=1.3),
    ),
    Feature(
        "pulse",
        "m.p",
        "pulse 0.1 higher at kick and snare hits than between",
        _diff_rule("p", ("kick", "snare"), min_diff=0.1),
    ),
    Feature(
        "level",
        "a.l",
        "level 0.1 higher at kick and snare hits than between",
        _diff_rule("l", ("kick", "snare"), min_diff=0.1),
    ),
    Feature(
        "energy",
        "m.e",
        "energy 0.05 higher at kick and snare hits than between",
        _diff_rule("e", ("kick", "snare"), min_diff=0.05),
    ),
    Feature(
        "flux",
        "m.rflux",
        "spectral flux x3 at kick and snare hits",
        _diff_rule("rflux", ("kick", "snare"), min_ratio=3.0),
    ),
    Feature(
        "hfc",
        "m.hfc",
        "high-frequency content x3 at snare and hat hits",
        _diff_rule("hfc", ("snare", "hat"), min_ratio=3.0),
    ),
    Feature(
        "centroid", "m.cent", f"centroid at snares and hats >= {BAND_RATIO:g}x at kicks", _bands
    ),
    Feature("rolloff", "m.roll", f"rolloff at hats >= {BAND_RATIO:g}x at kicks (+1)", _rolloff),
    Feature(
        "tempo", "m.bpm", f"groove tempo within {TEMPO_TOL:.0%} of 120 BPM (or 60, 240)", _tempo
    ),
    Feature(
        "period", "m.per", f"ACF period within {PERIOD_TOL:.0%} of 120 BPM (or 60, 240)", _period
    ),
    Feature(
        "rhythm",
        "m.str, m.a",
        f"rhythm strength >= {MIN_RHYTHM} and active in the groove",
        _strength,
    ),
    Feature("periodicity", "m.conf", f"periodicity >= {MIN_RHYTHM} in the groove", _periodicity),
    Feature(
        "phase",
        "m.ph",
        f"PLP phase concentrated at the kicks (>= {MIN_PHASE_CONC}), one wrap per beat",
        _plp_phase,
    ),
    Feature("beats", "m.q", "beat events at the reported tempo's rate", _beats),
    Feature(
        "plp pulse", "m.pp", "PLP pulse 0.05 higher at the groove's kicks than between", _plp_pulse
    ),
    Feature("density", "m.od", "onset density within 0.5-2x the groove's detection rate", _density),
    Feature(
        "render",
        "v.act",
        f"frame activity x{RENDER_RATIO} at kick and snare hits (the LEDs follow)",
        _render,
    ),
)


def judge_response(
    device_id: str, cap: dict[str, Any]
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    got = cap["devices"][device_id]
    t0 = float(cap["start_ms"])
    rows = sorted(got["rows"], key=lambda r: float(r["t"]))
    lag, source, matched, hits = find_lag(got["onsets"], rows, t0)
    ctx = {"rows": rows, "onsets": got["onsets"], "t0": t0, "lag": lag}
    out = []
    for f in FEATURES:
        try:
            ok, ev, data = f.judge(ctx)
        except (ValueError, ZeroDivisionError, TypeError, KeyError) as e:
            ok, ev, data = False, f"couldn't judge: {e}", {}
        out.append(
            check(
                "response", f.name, device_id, YES if ok else NO, f"{f.field}: {f.rule}", ev, **data
            )
        )
    meta = {
        "lag_ms": lag,
        "lag_source": source,
        "lag_matched": f"{matched}/{hits}",
        "rows": len(got["rows"]),
        "detections": len(got["onsets"]),
    }
    return out, meta


def _capture_path() -> Path:
    from dbos import DBOS

    d = (
        data_dir()
        / "hardware-checks"
        / re.sub(r"[^A-Za-z0-9_.-]", "_", str(DBOS.workflow_id or "run"))
    )
    d.mkdir(parents=True, exist_ok=True)
    return d / "response.json"


@durable.step
async def response(device_ids: list[str]) -> dict[str, Any]:
    t0 = time.monotonic()
    devices = await _connected(device_ids)
    wav = await asyncio.to_thread(stimulus.ensure, "response", data_dir() / "stimuli")
    cap = await capture(devices, str(wav), stimulus.RESPONSE_S)
    if not cap.get("played"):
        return {
            "checks": [],
            "rig_error": f"speaker: {cap.get('play_error') or 'playback failed'}",
            "seconds": round(time.monotonic() - t0, 1),
        }
    path = _capture_path()
    path.write_text(json.dumps(cap))
    out: list[dict[str, Any]] = []
    meta: dict[str, Any] = {}
    for d in devices:
        checks, meta[d.id] = judge_response(d.id, cap)
        out += checks
    return {
        "checks": out,
        "rig_error": None,
        "meta": meta,
        "capture": str(path),
        "seconds": round(time.monotonic() - t0, 1),
    }


# -- 5. interfaces -----------------------------------------------------------------------------


def _settings_value(reply: str, name: str) -> float | None:
    m = re.search(rf"^{re.escape(name)}\s*=\s*(-?[0-9.]+)", reply, re.M)
    return float(m.group(1)) if m else None


@durable.step
async def interfaces(
    device_ids: list[str], board_of: dict[str, str | None], start: dict[str, Any]
) -> dict[str, Any]:
    t0 = time.monotonic()
    out: list[dict[str, Any]] = []
    adapter = host_bluetooth()
    alerts = alerts_since(device_ids, float(start["alert_t"]))
    for d in await _connected(device_ids):
        board = BOARDS.get(board_of.get(d.id) or "")
        out.append(await _serial(d))
        out.append(await _stream(d))
        out.append(await _settings(d))
        if board is not None and board.ble_stack:
            out.append(await _ble_stack(d))
        out.append(await _ble_scan(d, adapter))
        info = await _info(d)
        ms = int(info.get("millis", 0))
        want = start["ms"].get(d.id, 0) + (time.time() - start["t"]) * 1000 - REBOOT_SLACK_MS
        lines = alerts.get(d.id, [])
        out.append(
            check(
                "interfaces",
                "no reboot",
                d.id,
                YES if ms >= want else NO,
                "no reset across the checks",
                f"uptime {ms // 1000} s",
            )
        )
        out.append(
            check(
                "interfaces",
                "alerts",
                d.id,
                NO if lines else YES,
                "no firmware [ASSERT]/[FALLBACK] line since the flash",
                f"{len(lines)}: {lines[0][:100]}" if lines else "none",
                lines=lines[:20],
            )
        )
    return {"checks": out, "seconds": round(time.monotonic() - t0, 1)}


async def _serial(d: Any) -> dict[str, Any]:
    rtts, version = [], None
    try:
        for _ in range(5):
            _, sent, times = await d.protocol.send_command_timed("json info")
            if times:
                rtts.append((times[-1] - sent) * 1000)
            version = version or True
    except Exception as e:
        return check(
            "interfaces",
            "serial",
            d.id,
            NO,
            "json info round trips over USB serial",
            f"failed: {e}",
        )
    med = _median(rtts)
    ok = bool(rtts) and med is not None and med < 2000
    return check(
        "interfaces",
        "serial",
        d.id,
        YES if ok else NO,
        "5 json info round trips, each answered",
        f"median {med:.0f} ms over {len(rtts)}" if med else "no reply lines",
        rtt_ms=_rnd(med, 1),
    )


async def _stream(d: Any) -> dict[str, Any]:
    rule = "`stream on` sends lines within 2 s; `stream off` stops them within 1 s"
    q = d.subscribe_stream()
    try:
        while not q.empty():
            q.get_nowait()
        await d.protocol.start_stream("on")
        n_on = 0
        end = time.monotonic() + 2.0
        while time.monotonic() < end:
            try:
                msg = await asyncio.wait_for(q.get(), max(0.01, end - time.monotonic()))
            except TimeoutError:
                break
            if msg and msg.get("type") == "audio":
                n_on += 1
        await d.protocol.stop_stream()
        await asyncio.sleep(1.0)
        while not q.empty():
            q.get_nowait()
        await asyncio.sleep(1.0)
        n_after = sum(
            1 for _ in range(q.qsize()) if (m := q.get_nowait()) and m.get("type") == "audio"
        )
    except Exception as e:
        with contextlib.suppress(Exception):
            await d.protocol.stop_stream()
        return check("interfaces", "stream", d.id, NO, rule, f"failed: {e}")
    finally:
        d.unsubscribe_stream(q)
    ok = n_on > 0 and n_after == 0
    return check(
        "interfaces",
        "stream",
        d.id,
        YES if ok else NO,
        rule,
        f"{n_on} lines in 2 s on; {n_after} in the second after off",
    )


async def _settings(d: Any) -> dict[str, Any]:
    """get -> set -> get -> set back -> get, of one runtime setting; never saved."""
    rule = f"`set {SETTINGS_PROBE}` reads back, and back again (runtime only, no `save`)"
    try:
        v0 = _settings_value(await _reply(d, f"get {SETTINGS_PROBE}"), SETTINGS_PROBE)
        if v0 is None:
            return check(
                "interfaces", "settings", d.id, NO, rule, f"`get {SETTINGS_PROBE}` unreadable"
            )
        v1 = round((v0 + 0.25) % 1.0, 3)
        await _reply(d, f"set {SETTINGS_PROBE} {v1}")
        got1 = _settings_value(await _reply(d, f"get {SETTINGS_PROBE}"), SETTINGS_PROBE)
        await _reply(d, f"set {SETTINGS_PROBE} {v0}")
        got0 = _settings_value(await _reply(d, f"get {SETTINGS_PROBE}"), SETTINGS_PROBE)
    except Exception as e:
        return check("interfaces", "settings", d.id, NO, rule, f"failed: {e}")
    ok = got1 is not None and got0 is not None and abs(got1 - v1) < 1e-3 and abs(got0 - v0) < 1e-3
    return check(
        "interfaces",
        "settings",
        d.id,
        YES if ok else NO,
        rule,
        f"{v0} -> {got1} (set {v1}) -> {got0}",
    )


async def _ble_stack(d: Any) -> dict[str, Any]:
    rule = "the chip's `ble` report: advertising, its fleet scanner active"
    try:
        r = await _reply(d, "ble")
    except Exception as e:
        return check("interfaces", "ble stack", d.id, NO, rule, f"failed: {e}")
    adv = "advertising=yes" in r
    scan = re.search(r"role=scanner state=active", r) is not None
    return check(
        "interfaces",
        "ble stack",
        d.id,
        YES if adv and scan else NO,
        rule,
        f"advertising {'yes' if adv else 'no'}, scanner {'active' if scan else 'not active'}",
    )


def host_bluetooth() -> str | None:
    """The box's Bluetooth adapter (hci0, ...), or None. setup.sh turns Bluetooth off on the box
    (dtoverlay=disable-bt), so normally None."""
    base = Path("/sys/class/bluetooth")
    try:
        names = sorted(p.name for p in base.iterdir() if re.fullmatch(r"hci\d+", p.name))
    except OSError:
        return None
    return names[0] if names else None


async def _ble_scan(d: Any, adapter: str | None) -> dict[str, Any]:
    """A scan only: the server runs --no-ble, so nothing connects or pairs."""
    rule = "a host BLE scan sees the chip advertise (scan only, never a connection)"
    if adapter is None:
        return check(
            "interfaces",
            "ble scan",
            d.id,
            NA,
            rule,
            "the box has no Bluetooth adapter (setup.sh: dtoverlay=disable-bt)",
        )
    addr = str(getattr(d, "ble_address", "") or "").upper()
    if not addr:
        return check("interfaces", "ble scan", d.id, NO, rule, "the chip reports no BLE address")
    try:
        proc = await asyncio.create_subprocess_exec(
            "bluetoothctl",
            "--timeout",
            "10",
            "scan",
            "on",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL,
        )
        out, _ = await asyncio.wait_for(proc.communicate(), 20)
    except (OSError, TimeoutError) as e:
        return check(
            "interfaces", "ble scan", d.id, NA, rule, f"no usable adapter ({adapter}): {e}"
        )
    seen = addr in out.decode(errors="replace").upper()
    return check(
        "interfaces",
        "ble scan",
        d.id,
        YES if seen else NO,
        rule,
        f"{addr} {'seen' if seen else 'not seen'} in 10 s on {adapter}",
    )


# -- the report --------------------------------------------------------------------------------


@durable.step
async def run_start() -> float:
    """The wall time now (a step: the workflow body never reads the clock)."""
    return time.time()


@durable.step
async def baseline(device_ids: list[str], alert_t: float) -> dict[str, Any]:
    """Each chip's uptime now (the run's no-reboot baseline) and when alerts start counting."""
    ms = {}
    for d in await _connected(device_ids):
        ms[d.id] = int((await _info(d)).get("millis", 0))
    return {"t": time.time(), "ms": ms, "alert_t": alert_t}


@durable.step
async def write_report(key: str, report: dict[str, Any]) -> str:
    d = data_dir() / "hardware-checks"
    d.mkdir(parents=True, exist_ok=True)
    path = d / f"{re.sub(r'[^A-Za-z0-9_.-]', '_', key)}.json"
    path.write_text(json.dumps(report, indent=2) + "\n")
    return str(path)


def _record_phase(report: dict[str, Any], name: str, res: dict[str, Any]) -> bool:
    """Record a phase; True to go on to the next one."""
    checks = list(res.get("checks") or [])
    report["checks"] += checks
    entry: dict[str, Any] = {"name": name, "seconds": res.get("seconds")}
    if res.get("rig_error"):
        entry["result"] = "rig error"
        report["rig_error"] = f"{name}: {res['rig_error']}"
        report["phases"].append(entry)
        return False
    entry["result"] = phase_result(checks)
    report["phases"].append(entry)
    return bool(entry["result"] == YES)


def verdict(report: dict[str, Any]) -> bool:
    return bool(
        not report.get("rig_error")
        and [p["name"] for p in report["phases"]] == list(report["plan"])
        and all(p["result"] == YES for p in report["phases"])
    )


@durable.workflow("test.hardware-checks")
async def hardware_checks(req: dict[str, Any]) -> dict[str, Any]:
    """req: device_ids, source ({"kind": "dev_build", "build_id", "sha"} | {"kind": "staging",
    "sha"} | {"kind": "none"}: the chip as it is, dev mode), optional sha, pr, expected_version
    (kind none), phases (a prefix-ordered subset of PHASES; default all), regions (a region map
    path for every chip, overriding its board's), lights_brightness (default the test
    brightness; 0 proves the lights check fails)."""
    device_ids = list(req["device_ids"])
    source = dict(req.get("source") or {"kind": "none"})
    plan = [p for p in PHASES if p in (req.get("phases") or PHASES)]
    brightness = int(req.get("lights_brightness", leds.TEST_BRIGHTNESS))
    report: dict[str, Any] = {
        "kind": "hardware-checks",
        "sha": req.get("sha") or source.get("sha"),
        "pr": req.get("pr"),
        "source": source,
        "devices": device_ids,
        "plan": plan,
        "phases": [],
        "checks": [],
        "rig_error": None,
    }
    async with reserve(device_ids):
        t_start = await run_start()
        board_of = await boards(device_ids)
        report["boards"] = board_of
        regions = (
            sorted(
                {
                    req.get("regions") or b.regions
                    for i in device_ids
                    if (b := BOARDS.get(board_of.get(i) or "")) and b.regions
                }
            )
            if "lights" in plan
            else []
        )
        jobs.report(0, "preflight: camera, speaker")
        pre = await preflight(regions, bool({"audio", "response"} & set(plan)))
        report["preflight"] = pre
        if pre["error"]:
            report["rig_error"] = f"preflight: {pre['error']}"
            return await _done(report)
        expected = req.get("expected_version")
        flash_t: float | None = None
        deployed = source["kind"] in ("dev_build", "staging")
        if deployed:
            jobs.report(2, "upload: deploying")
            before = await versions_before(device_ids)
            t_dep = await run_start()
            dep = await _deploy(source, device_ids)
            report["deploy_s"] = round(await run_start() - t_dep, 1)
            result, ev = judge_deploy(dep, before)
            report["deploy"] = {
                k: dep.get(k) for k in ("status", "failed_step", "message", "version")
            }
            if result == "rig":
                report["rig_error"] = f"upload: {ev}"
                report["phases"].append(
                    {"name": "upload", "result": "rig error", "seconds": report["deploy_s"]}
                )
                return await _done(report)
            for i in device_ids:
                report["checks"].append(
                    check(
                        "upload",
                        "flash",
                        i,
                        result,
                        "flashed and verified through the deploy pipeline",
                        ev,
                    )
                )
            if result == NO:
                report["phases"].append(
                    {"name": "upload", "result": NO, "seconds": report["deploy_s"]}
                )
                return await _done(report)
            expected = dep["version"]
            flash_t = float(dep["steps"]["store"]["flash_t"])
        else:
            for i in device_ids:
                report["checks"].append(
                    check(
                        "upload",
                        "flash",
                        i,
                        NA,
                        "flashed and verified through the deploy pipeline",
                        "dev run: the chip as flashed (dev.py flash)",
                    )
                )
        report["version"] = expected
        start = await baseline(device_ids, flash_t if flash_t is not None else t_start)
        async with lit(device_ids, needs_light=False, purpose="the hardware checks"):
            for n, name in enumerate(plan):
                jobs.report(5 + 90 * n // len(plan), f"{name}")
                if name == "upload":
                    res = await upload_checks(device_ids, expected, board_of)
                    # The phase's time includes the flash.
                    res["seconds"] = round(res["seconds"] + report.get("deploy_s", 0.0), 1)
                elif name == "lights":
                    res = await lights(device_ids, board_of, req.get("regions"), brightness)
                elif name == "audio":
                    res = await audio(device_ids)
                elif name == "response":
                    res = await response(device_ids)
                    report["response"] = {"meta": res.get("meta"), "capture": res.get("capture")}
                else:
                    res = await interfaces(device_ids, board_of, start)
                if not _record_phase(report, name, res):
                    break
    return await _done(report)


async def _deploy(source: dict[str, Any], device_ids: list[str]) -> dict[str, Any]:
    from ..api.routes_firmware import _store_dev_hex, _store_staging_hex
    from ..firmware.fleet_workflows import flash_devices

    if source["kind"] == "dev_build":
        dep = await deploy_pipeline.run_deploy(
            _fleet(),
            jobs.Progress(),
            release_tag=None,
            device_ids=device_ids,
            download_dir=Path(firmware_dir() / "dev-download"),
            store_firmware=_store_dev_hex,
            flash=flash_devices,
            dev_build=source["build_id"],
        )
        if dep.get("status") == "ok" and dep.get("sha") != source.get("sha"):
            return {
                **dep,
                "status": "error",
                "failed_step": "release",
                "message": f"build {source['build_id']} is of {str(dep.get('sha'))[:12]}, not {str(source.get('sha'))[:12]}",
            }
        return dep
    return await deploy_pipeline.run_deploy(
        _fleet(),
        jobs.Progress(),
        release_tag=None,
        device_ids=device_ids,
        download_dir=data_dir() / "staging-download",
        store_firmware=_store_staging_hex,
        flash=flash_devices,
        staging_sha=source["sha"],
    )


async def _done(report: dict[str, Any]) -> dict[str, Any]:
    report["passed"] = verdict(report)
    key = f"{report.get('sha') or 'dev'}-{'pass' if report['passed'] else 'rig' if report['rig_error'] else 'fail'}"
    report["path"] = await write_report(key, report)
    jobs.report(
        100, "passed" if report["passed"] else ("rig error" if report["rig_error"] else "FAILED")
    )
    return report
