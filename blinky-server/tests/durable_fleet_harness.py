"""Runs the real ``fleet.flash`` workflow against a fake fleet in its own process, so
test_durable_flash.py can kill it for real and start it again (#325).

    python -m tests.durable_fleet_harness start  OUT KILL   # none | before:N | after:N
    python -m tests.durable_fleet_harness resume OUT [late|never|dfu]

Every flash is appended to OUT/effects.log. ``before:N`` exits in the Nth device's flash before
its bytes land; ``after:N`` exits after they land (the device already reports the new version)
but before the flash finished. ``resume`` starts again and prints the job's result. The data
dir (the work store) comes from XDG_DATA_HOME.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
from collections.abc import AsyncIterator
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from blinky_server.api.deps import set_fleet
from blinky_server.device.device import DeviceState
from blinky_server.firmware import fleet_workflows
from blinky_server.firmware.flash_job import FlashJobState, FlashTransport
from blinky_server.paths import data_dir
from blinky_server.work import durable

DEVICES = ["dev-a", "dev-b", "dev-c"]
NEW, OLD = "b300", "b299"

mode, out = sys.argv[1], Path(sys.argv[2])
kill = sys.argv[3] if len(sys.argv) > 3 else "none"
effects = out / "effects.log"
state_file = out / "versions.json"  # the devices' firmware survives the process, as on hardware


def effect(line: str) -> None:
    with open(effects, "a") as f:
        f.write(line + "\n")


versions = (
    json.loads(state_file.read_text()) if state_file.exists() else dict.fromkeys(DEVICES, OLD)
)


def set_version(dev: str, v: str) -> None:
    versions[dev] = v
    devices[dev].version = v
    state_file.write_text(json.dumps(versions))


devices = {
    d: SimpleNamespace(
        id=d,
        state=DeviceState.CONNECTED,
        version=versions[d],
        device_name=d,
        transport=SimpleNamespace(transport_type="serial"),
    )
    for d in DEVICES
}
flashes = 0


async def flash_fleet(device_ids: list[str], firmware_path: Path, **kw: Any) -> AsyncIterator[Any]:
    global flashes
    for dev in device_ids:
        flashes += 1
        effect(f"flash {dev} start")
        if kill == f"before:{flashes}":
            os._exit(9)
        await asyncio.sleep(0.2)
        set_version(dev, NEW)  # the bytes landed
        if kill == f"after:{flashes}":
            os._exit(9)
        effect(f"flash {dev} end")
        yield (
            dev,
            SimpleNamespace(
                state=FlashJobState.COMPLETED,
                duration_s=0.2,
                transport=FlashTransport.UF2,
                error=None,
            ),
            None,
        )


async def usb_settle() -> None:
    effect("settle")


set_fleet(
    SimpleNamespace(
        get_device=devices.get,
        get_all_devices=lambda: list(devices.values()),
        flash_fleet=flash_fleet,
        usb_settle=usb_settle,
    )
)
fleet_workflows.images.load = lambda path: SimpleNamespace(version=NEW)
fleet_workflows.VERSION_SETTLE_S = 0
fleet_workflows.VERSION_POLL_S = 0
fleet_workflows.DEVICE_WAIT_S = 4


async def come_back(never: str | None) -> None:
    """As after a real restart: devices are registered before they're connected, with no
    version, and connect a while later (``never``: that one stays PRESENT, like BLE)."""
    for d in devices.values():
        d.state, d.version = DeviceState.DISCONNECTED, None
    await asyncio.sleep(1.5)
    for d in devices.values():
        if d.id == never:
            d.state = DeviceState.DFU_RECOVERY if kill == "dfu" else DeviceState.PRESENT
            continue
        d.state, d.version = DeviceState.CONNECTED, versions[d.id]
        effect(f"{d.id} connected")


async def main() -> None:
    if mode == "resume" and kill in ("late", "never", "dfu"):
        asyncio.get_running_loop().create_task(come_back(None if kill == "late" else "dev-b"))
        await asyncio.sleep(0)
    durable.configure(data_dir())
    await durable.launch()
    wid_file = out / "workflow_id"
    if mode == "start":
        wid = await durable.submit(fleet_workflows.fleet_flash, DEVICES, "/fake/fw.hex")
        wid_file.write_text(wid)
    else:
        wid = wid_file.read_text()
    from dbos import DBOS

    handle = await DBOS.retrieve_workflow_async(wid)
    print(json.dumps(await handle.get_result()))
    await durable.shutdown()


asyncio.run(main())
