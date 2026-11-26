"""Runs the real ``test.validate`` workflow against a fake fleet in its own process, so
test_durable_work.py can kill it for real and start it again (#325). WORKFLOW names another
test job instead; ``staging`` runs the staging suite (#349) with the deploy, json info, the
stream and json rhythm faked, its validation playbacks counted like validate's; ``dev-run`` runs
the dev loop's targeted checks the same way, and ``pr-smoke`` the PR hardware queue's smoke test.

    python -m tests.durable_harness start  OUT KILL   # KILL: none | crash:N | drain:N
    python -m tests.durable_harness resume OUT [late]  # late: devices reconnect 1.5 s in

Every device command and playback is appended to OUT/effects.log. ``crash:N`` exits the
process in the middle of the Nth playback; ``drain:N`` starts the shutdown drain in the middle
of it, as SIGTERM does, then exits once drained. ``resume`` starts again and prints the job's
result. The data dir (the work store) comes from XDG_DATA_HOME.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
import time
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from blinky_server.api.deps import set_fleet
from blinky_server.device.device import DeviceState
from blinky_server.firmware import deploy_pipeline
from blinky_server.paths import data_dir
from blinky_server.testing import dev_run, hardware_checks, staging, workflows
from blinky_server.testing.audio_player import PlaybackResult
from blinky_server.work import durable

TRACKS = ["alpha", "bravo", "charlie"]
RUNS = 2
DEVICES = ["dev-a", "dev-b"]
WORKFLOW = os.environ.get("WORKFLOW", "validate")
REQ = {
    "device_ids": DEVICES,
    "track_dir": "/fake",
    "track_names": None,
    "duration_ms": 1000,
    "seek_sec": None,
    "settle_ms": 0,
    "num_runs": RUNS,
    "commands": ["set x 1"],
    "per_device_commands": None,
    "persist_raw": False,
}

mode, out = sys.argv[1], Path(sys.argv[2])
kill = sys.argv[3] if len(sys.argv) > 3 else "none"
effects = out / "effects.log"


def effect(line: str) -> None:
    with open(effects, "a") as f:
        f.write(line + "\n")


class Protocol:
    def __init__(self, dev: str) -> None:
        self.dev = dev

    async def send_command(self, cmd: str) -> str:
        global setup_drain
        if cmd == "json rhythm":  # the staging suite's beat pace: 120 bpm, counted exactly
            return json.dumps({"bpm": 120.0, "beatCount": int(2 * time.time()) % 65536})
        effect(f"{self.dev} cmd {cmd}")
        if WORKFLOW == "hardware-checks":
            return hw_reply(self.dev, cmd)
        if kill == "drain-setup" and cmd == "restore_runtime_settings" and not setup_drain:
            setup_drain = True  # SIGTERM in the middle of the session's device setup
            asyncio.get_running_loop().create_task(sigterm())
        return "{}"

    async def send_command_timed(self, cmd: str) -> tuple[str, float, list[float]]:
        t = time.time()
        return await self.send_command(cmd), t, [t + 0.003]

    async def start_stream(self, mode: str) -> None:
        effect(f"{self.dev} stream {mode}")
        for q in live_queues:  # the hardware checks' stream check reads what follows
            for _ in range(5):
                q.put_nowait({"type": "audio", "data": {"a": {"l": 0.3}, "m": {"p": 0.0}}})

    async def stop_stream(self) -> None:
        effect(f"{self.dev} stream off")


live_queues: list[asyncio.Queue[dict[str, Any] | None]] = []


def subscribe_stream() -> asyncio.Queue[dict[str, Any] | None]:
    """The staging suite's music check: a loud stream with a pulse every other line."""
    q: asyncio.Queue[dict[str, Any] | None] = asyncio.Queue()
    live_queues.append(q)
    for n in range(200):
        q.put_nowait({"type": "audio", "data": {"a": {"l": 0.3}, "m": {"p": float(n % 2)}}})
    return q


devices = {
    d: SimpleNamespace(
        id=d,
        state=DeviceState.CONNECTED,
        protocol=Protocol(d),
        alert_count=0,
        alerts=[],
        subscribe_stream=subscribe_stream,
        unsubscribe_stream=lambda q: None,
        device_type="testbox_grid_v1",
        ble_address="",
    )
    for d in DEVICES
}
# Not the test box: no LED lifecycle here (tests/test_testbox_leds.py covers it).
set_fleet(
    SimpleNamespace(
        get_device=devices.get,
        get_all_devices=lambda: list(devices.values()),
        staging_enabled=False,
    )
)

plays = 0
setup_drain = False


async def fake_play(device_ids: list[str], track: dict[str, Any], duration_ms: float) -> Any:
    global plays
    plays += 1
    effect(f"play {track['name']} start")
    if kill == f"crash:{plays}":
        os._exit(9)
    if kill in (f"drain:{plays}", f"two-drain:{plays}"):
        asyncio.get_running_loop().create_task(sigterm())
    await asyncio.sleep(0.3)
    effect(f"play {track['name']} end")
    devs = [devices[d] for d in device_ids]
    return (
        devs,
        PlaybackResult(success=True, audio_start_time_ms=0.0, duration_ms=300.0),
        {d: "data" for d in device_ids},
    )


async def sigterm() -> None:
    """What the app's lifespan does on SIGTERM, then the process ends."""
    await durable.drain(lambda s: effect(f"status {s}"), lambda s: None)
    effect("drained")
    await durable.shutdown()
    effect("exited")
    os._exit(0)


async def noop(*a: Any, **k: Any) -> None:
    return None


workflows.discover_tracks = lambda d: [
    {"name": t, "audio_file": f"/fake/{t}.mp3", "ground_truth": None} for t in TRACKS
]
workflows.load_track_manifest = lambda d: {}
workflows._ground_truth = lambda track, duration_ms: None
staging.track_seconds = lambda t: 100.0  # every fake track has room for music and pace
workflows._play = fake_play
workflows.score_device_run = lambda data, start, gt, **k: {"data": data}
workflows.format_score_summary = lambda score: {"ok": True, "drums": {"drumF": 0.5}}


async def fake_capture(device: Any, duration_ms: float, output: str) -> Any:
    global plays
    plays += 1
    effect("play capture start")
    if kill == f"crash:{plays}":
        os._exit(9)
    await asyncio.sleep(0.3)
    effect("play capture end")
    return SimpleNamespace(
        frames=10,
        duration_sec=1.0,
        frame_rate=62.5,
        output_path=output,
        nn_active=True,
        onset_stats={},
        level_stats={},
    )


workflows.capture_nn_stream = fake_capture
workflows.INTER_RUN_GAP_S = 0
workflows.kill_orphan_audio = noop
workflows.stop_audio = noop
workflows.acquire_audio_lock = lambda ids: True
workflows.release_audio_lock = lambda: None


STAGING_VERSION = "b300-dev-abababab"


async def fake_deploy(*a: Any, **k: Any) -> dict[str, Any]:
    out = {"status": "ok", "version": STAGING_VERSION, "steps": {"store": {"flash_t": 0.0}}}
    if k.get("dev_build"):  # the hardware checks' build-service package, of its commit
        effect(f"deploy build {k['dev_build']}")
        return {**out, "build_id": k["dev_build"], "sha": HW_REQ["sha"]}
    effect(f"deploy {k['staging_sha'][:8]}")
    return out


async def fake_info(device_id: str) -> dict[str, Any]:
    # Wall-clock millis: uptime keeps rising across this process's restart, as on a chip
    # that the server's restart doesn't reset.
    return {
        "version": STAGING_VERSION,
        "millis": int(time.time() * 1000),
        "fps": 120.0,
        "maxFrameMs": 20,
        "audioOverruns": 0,
    }


async def fake_play_audio(*a: Any, **k: Any) -> Any:
    await asyncio.sleep(0.01)
    return SimpleNamespace(success=True)


deploy_pipeline.run_deploy = fake_deploy  # type: ignore[assignment]
staging._info = fake_info
staging.play_audio = fake_play_audio
staging.stop_audio = noop
dev_run.kill_orphan_audio = noop
dev_run.stop_audio = noop
staging.HEALTH_POLL_S = 0.05
staging.HEALTH_SETTLE_S = 0.0
STAGING_REQ = {
    "sha": "ab" * 20,
    "device_ids": DEVICES,
    "track_dir": "/fake",
    "limits": {
        "stable_minutes": 0.005,
        "music_seconds": 0.3,
        "pace_seconds": 2.5,
        "validation_ms": 1000,
    },
}


# The dev loop's targeted checks (docs/DEV_LOOP.md): every kind, in order.
DEV_RUN_REQ = {
    "device_ids": DEVICES,
    "checks": [
        {"kind": "info"},
        {"kind": "commands", "commands": ["set x 3"]},
        {"kind": "health", "minutes": 0.005},
        {"kind": "validate", "tracks": TRACKS},
        {"kind": "pace"},
    ],
    "track_dir": "/fake",
    "limits": STAGING_REQ["limits"],
}


# The hardware checks (testing/hardware_checks.py): a PR head's build, every phase.
HW_REQ = {
    "sha": "cd" * 20,
    "device_ids": DEVICES,
    "source": {"kind": "dev_build", "build_id": "b-1", "sha": "cd" * 20},
    "pr": 7,
    "runner": "pr",
}
hw_settings: dict[str, str] = {}


def hw_reply(dev: str, cmd: str) -> str:
    """What the box's chip answers to the hardware checks' commands."""
    words = cmd.split()
    if words[0] == "leds" and words[1].isdigit():
        return json.dumps({"brightness": int(words[1]), "config": 0, "override": True})
    if words[0] == "testpattern":
        return f"OK testpattern {words[1]}"
    if words[0] == "set":
        hw_settings[f"{dev}:{words[1]}"] = words[2]
        return f"{words[1]} = {words[2]}  [effect]"
    if words[0] == "get":
        return f"{words[1]} = {hw_settings.get(f'{dev}:{words[1]}', '0.000')}  [effect]"
    if cmd == "ble":
        return "[BLE] NUS: disconnected\n[BLE] advertising=yes\n[BLE] role=scanner state=active"
    return "{}"


class FakeCamera:
    """camera_lights.py's replies for a grid whose every LED works."""

    @classmethod
    async def open(cls, regions: str) -> tuple[Any, dict[str, Any]]:
        return cls(), {"ready": True, "regions": 64}

    async def ask(self, req: dict[str, Any]) -> dict[str, Any]:
        if req["op"] == "dark":
            return {"frame_mean": 27.8, "dark_mean": [5000.0] * 64}
        if req["op"] == "lit":
            if "key" not in req:
                return {"lit": list(range(64)), "frame_rise": 0.5}
            x, y = (int(v) for v in req["key"].split(","))
            return {"lit": [y * 8 + x], "frame_rise": 0.01}
        return {"ok": True, "dead": [], "unseen": [], "ambiguous": [], "shared": {}}

    async def close(self) -> None:
        pass


async def fake_capture(devs: list[Any], wav: str | None, seconds: float) -> dict[str, Any]:
    """The stream while a stimulus plays: the noise and the response stimulus are playbacks
    (counted, and a crash point), the silence isn't."""
    global plays
    from tests import hw_synth

    t0 = time.time() * 1000
    if wav is None:
        cap = hw_synth.level(t0, seconds, 0.002)
    else:
        kind = "noise" if "noise" in wav else "response"
        plays += 1
        effect(f"play {kind} start")
        if kill == f"crash:{plays}":
            os._exit(9)
        if kill in (f"drain:{plays}", f"two-drain:{plays}"):
            asyncio.get_running_loop().create_task(sigterm())
        await asyncio.sleep(0.3)
        effect(f"play {kind} end")
        cap = hw_synth.level(t0, seconds, 0.08) if kind == "noise" else hw_synth.response(t0)
    one = cap["devices"]["grid"]
    cap["devices"] = {d.id: one for d in devs}
    return cap


async def hw_info(device: Any) -> dict[str, Any]:
    return {
        "version": STAGING_VERSION,
        "platform": "nrf52840",
        "millis": int(time.time() * 1000),
        "fps": 120.0,
        "maxFrameMs": 20,
        "audioOverruns": 0,
        "audioSamplesLost": 0,
        "device": {
            "id": "testbox_grid_v1",
            "width": 8,
            "height": 8,
            "leds": 64,
            "configured": True,
        },
    }


hardware_checks.Camera = FakeCamera  # type: ignore[misc,assignment]
hardware_checks.capture = fake_capture
hardware_checks._info = hw_info
hardware_checks.ffplay = lambda: "/usr/bin/ffplay"
hardware_checks.host_bluetooth = lambda: None


async def reconnect_late() -> None:
    """As after a real restart: discovery connects the devices a while after the start."""
    for d in devices.values():
        d.state = DeviceState.PRESENT
    await asyncio.sleep(1.5)
    for d in devices.values():
        d.state = DeviceState.CONNECTED
        effect(f"{d.id} connected")


async def main() -> None:
    if kill == "newschema":  # this restart's code declares the recorded steps incompatible
        durable.SCHEMA_VERSION = "blinky-work-next"
    if kill == "late":
        asyncio.get_running_loop().create_task(reconnect_late())
        await asyncio.sleep(0)
    durable.configure(data_dir())
    await durable.launch()
    wid_file = out / "workflow_id"
    if mode == "start" and WORKFLOW != "validate":
        jobs = {
            "param-sweep": (
                workflows.param_sweep,
                {**REQ, "param_name": "odfgate", "values": [0.1, 0.2, 0.3, 0.4]},
            ),
            "tune-threshold": (
                workflows.tune_threshold,
                {
                    **REQ,
                    "device_id": "dev-a",
                    "param_name": "odfgate",
                    "low": 0.1,
                    "high": 0.5,
                    "max_steps": 4,
                    "target_metric": "drumF",
                },
            ),
        }
        if WORKFLOW == "staging":
            wid = await durable.submit(staging.staging_suite, STAGING_REQ)
        elif WORKFLOW == "dev-run":
            wid = await durable.submit(dev_run.dev_run, DEV_RUN_REQ)
        elif WORKFLOW == "hardware-checks":
            wid = await durable.submit(hardware_checks.hardware_checks, HW_REQ)
        elif WORKFLOW == "capture-nn":
            wid = await durable.submit(workflows.capture_nn, "dev-a", 1000.0, str(out / "c.jsonl"))
        else:
            fn, req = jobs[WORKFLOW]
            wid = await durable.submit(fn, req)
        wid_file.write_text(wid)
    elif mode == "start":
        wid = await durable.submit(workflows.validate, REQ)
        wid_file.write_text(wid)
        if kill.startswith("two-"):
            # A second job, queued behind the first on the audio lock.
            second = await durable.submit(workflows.validate, {**REQ, "commands": ["set x 2"]})
            (out / "second_id").write_text(second)
    else:
        wid = wid_file.read_text()
    from dbos import DBOS

    if kill == "newschema":
        st = await DBOS.get_workflow_status_async(wid)
        print(json.dumps({"status": st.status, "app_version": st.app_version}))
        await durable.shutdown()
        return
    handle = await DBOS.retrieve_workflow_async(wid)
    print(json.dumps(await handle.get_result()))
    if (out / "second_id").exists():
        second = await DBOS.retrieve_workflow_async((out / "second_id").read_text())
        print(json.dumps(await second.get_result()))
    await durable.shutdown()


asyncio.run(main())
