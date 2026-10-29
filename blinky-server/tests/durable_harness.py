"""Runs the real ``test.validate`` workflow against a fake fleet in its own process, so
test_durable_work.py can kill it for real and start it again (#325).

    python -m tests.durable_harness start  OUT KILL   # KILL: none | crash:N | drain:N
    python -m tests.durable_harness resume OUT

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
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from blinky_server.api.deps import set_fleet
from blinky_server.device.device import DeviceState
from blinky_server.paths import data_dir
from blinky_server.testing import workflows
from blinky_server.work import durable

TRACKS = ["alpha", "bravo", "charlie"]
RUNS = 2
DEVICES = ["dev-a", "dev-b"]
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
        effect(f"{self.dev} cmd {cmd}")
        return "{}"

    async def start_stream(self, mode: str) -> None:
        effect(f"{self.dev} stream {mode}")

    async def stop_stream(self) -> None:
        effect(f"{self.dev} stream off")


devices = {
    d: SimpleNamespace(id=d, state=DeviceState.CONNECTED, protocol=Protocol(d)) for d in DEVICES
}
set_fleet(SimpleNamespace(get_device=devices.get, get_all_devices=lambda: list(devices.values())))

plays = 0


async def fake_play(device_ids: list[str], track: dict[str, Any], duration_ms: float) -> Any:
    global plays
    plays += 1
    effect(f"play {track['name']} start")
    if kill == f"crash:{plays}":
        os._exit(9)
    if kill == f"drain:{plays}":
        asyncio.get_running_loop().create_task(sigterm())
    await asyncio.sleep(0.3)
    effect(f"play {track['name']} end")
    devs = [devices[d] for d in device_ids]
    return (
        devs,
        SimpleNamespace(success=True, audio_start_time_ms=0.0),
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
workflows._play = fake_play
workflows.score_device_run = lambda data, start, gt: {"data": data}
workflows.format_score_summary = lambda score: {"ok": True}
workflows.INTER_RUN_GAP_S = 0
workflows.kill_orphan_audio = noop
workflows.stop_audio = noop
workflows.acquire_audio_lock = lambda ids: True
workflows.release_audio_lock = lambda: None


async def main() -> None:
    durable.configure(data_dir())
    await durable.launch()
    wid_file = out / "workflow_id"
    if mode == "start":
        wid = await durable.submit(workflows.validate, REQ)
        wid_file.write_text(wid)
    else:
        wid = wid_file.read_text()
    from dbos import DBOS

    handle = await DBOS.retrieve_workflow_async(wid)
    print(json.dumps(await handle.get_result()))
    await durable.shutdown()


asyncio.run(main())
