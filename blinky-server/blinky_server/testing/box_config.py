"""The test box's chips take their device config from the registry (docs/TEST_BOX.md).

On the box (a server started with --staging), every time a chip connects, the server checks it
against its registry entry, ``devices/registry/<entry>.json``, and fixes it if it differs:

1. Compare. ``device show`` (the stored config) and ``json info`` (the running one) against
   the entry, with the firmware's defaults for the fields the entry leaves out. A match, the
   normal boot, costs one ``device show`` and writes nothing.
2. Upload. If the stored config differs, ``device upload <entry>``. Every chip on the box is
   a test chip (#382), so this needs no deploy header.
3. Reboot to apply it, once the chip has been up ``MIN_UPTIME_S`` (the firmware's 60 s rule
   between resets; markStable fires at 60 s) and only while no work uses it: no test job, no
   flash, no run holding its LEDs. Busy, it is left for its next reboot, which every run's
   flash brings anyway.

Loops: each reboot is recorded against the entry's hash (``state_path``). A chip that still
differs after a reboot for the same entry is logged as an ERROR and left alone until the entry
changes or the chip matches. A chip that isn't in ``BOX_CHIPS`` is never touched (logged).

Enrolling a chip: its registry entry, and a line in ``BOX_CHIPS``. The deployer restarts the
server for either (``devices/registry/`` is in its SERVER_PATHS), and the chip is configured
when it reconnects.
"""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import json
import logging
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ..work import durable

if TYPE_CHECKING:
    from ..device.device import Device
    from ..device.manager import FleetManager

log = logging.getLogger(__name__)

# The box's chips, by USB serial: the registry entry each one runs.
BOX_CHIPS = {
    "87DE9B58C90AFE2D": "testbox_grid_v1",  # the permanent test chip, the 8x8 grid
    # The ESP32-S3 (testbox_esp32) is added once the box can flash current ESP32 firmware: its
    # 1.0.1 build refuses the entry (orientation 5), so it would only log a refused upload.
}

REGISTRY_DIR = Path(__file__).resolve().parents[3] / "devices" / "registry"

# Up this long before the server reboots a chip: past markStable (60 s), as deploy_local.py.
MIN_UPTIME_S = 75.0

# What the firmware stores for a field an upload leaves out (SerialConsole::uploadDeviceConfig),
# so an entry compares equal to the full config `device show` prints.
FIRMWARE_DEFAULTS: dict[str, Any] = {
    "deviceId": "unknown",
    "deviceName": "Unnamed Device",
    "ledWidth": 0,
    "ledHeight": 1,
    "ledPin": 10,
    "ledPin2": 0,
    "brightness": 100,
    "ledType": 82,
    "orientation": 0,
    "layoutType": 0,
    "battery": False,
    "upVectorX": 0.0,
    "upVectorY": 0.0,
    "upVectorZ": 1.0,
    "rotationDegrees": 0.0,
    "invertZ": False,
    "swapXY": False,
    "invertX": False,
    "invertY": False,
    "baudRate": 115200,
    "initTimeoutMs": 2000,
    "sampleRate": 16000,
    "bufferSize": 32,
    "buttonPin": 0,
}

# The pending reboot per chip, so a reconnect doesn't queue a second one.
_pending: dict[str, asyncio.Task[None]] = {}


def state_path() -> Path:
    """{chip id: hash of the entry the server last rebooted it to apply}."""
    from ..paths import data_dir

    return data_dir() / "box-config.json"


def _load_state() -> dict[str, str]:
    try:
        return dict(json.loads(state_path().read_text()))
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as e:
        log.error("[FALLBACK] %s unreadable (%s): starting with no reboot record", state_path(), e)
        return {}


def _save_state(state: dict[str, str]) -> None:
    state_path().write_text(json.dumps(state, indent=1, sort_keys=True) + "\n")


def entry_for(device: Device) -> dict[str, Any] | None:
    """The chip's registry entry, or None for a chip that isn't enrolled."""
    name = BOX_CHIPS.get(device.id) or BOX_CHIPS.get(device.hardware_sn or "")
    if name is None:
        return None
    return dict(json.loads((REGISTRY_DIR / f"{name}.json").read_text()))


def parse_show(raw: str) -> dict[str, Any]:
    """``device show``'s pretty-printed object (several lines). Raises ValueError."""
    start, end = raw.find("{"), raw.rfind("}")
    if start < 0 or end < start:
        raise ValueError(f"no JSON object in the `device show` reply: {raw[:200]!r}")
    obj = json.loads(raw[start : end + 1])
    if not isinstance(obj, dict):
        raise ValueError(f"`device show` replied {obj!r}")
    return obj


def stored_diff(shown: dict[str, Any], entry: dict[str, Any]) -> list[str]:
    """The fields where the stored config (``device show``) differs from the entry. An
    unconfigured chip differs in all of them. A field the firmware doesn't print (older builds)
    isn't compared."""
    if "deviceId" not in shown:
        return ["unconfigured"]
    out = []
    for key, default in FIRMWARE_DEFAULTS.items():
        if key not in shown:
            continue
        want, have = entry.get(key, default), shown[key]
        if isinstance(default, float):
            same = abs(float(have) - float(want)) < 0.005  # printed with 2 decimals
        else:
            same = have == want
        if not same:
            out.append(f"{key}: {have!r} -> {want!r}")
    return out


def running_diff(info: dict[str, Any], entry: dict[str, Any]) -> list[str]:
    """Where the running config (``json info``'s device) differs from the entry: an upload
    applies at the next reboot."""
    dev = info.get("device") or {}
    want = {
        "configured": True,
        "id": entry["deviceId"],
        "width": entry["ledWidth"],
        "height": entry.get("ledHeight", FIRMWARE_DEFAULTS["ledHeight"]),
    }
    return [f"running {k}: {dev.get(k)!r} -> {v!r}" for k, v in want.items() if dev.get(k) != v]


def _entry_hash(entry: dict[str, Any]) -> str:
    return hashlib.sha256(json.dumps(entry, sort_keys=True).encode()).hexdigest()[:16]


async def reconcile(fleet: FleetManager, device: Device) -> None:
    """On connect (device/manager.py), on the box only. Never raises: connecting must not fail
    over the config."""
    if not fleet.staging_enabled:
        return
    try:
        await _reconcile(fleet, device)
    except Exception as e:
        log.error("%s: device config not checked against the registry: %s", device.id[:12], e)


async def _reconcile(fleet: FleetManager, device: Device) -> None:
    entry = entry_for(device)
    if entry is None:
        log.info(
            "%s (%s): no registry entry for this chip in box_config.BOX_CHIPS; its config is "
            "left alone",
            device.id[:12],
            device.platform,
        )
        return
    name, digest = entry["deviceId"], _entry_hash(entry)
    shown = parse_show(await device.protocol.send_command("device show"))
    stored = stored_diff(shown, entry)
    running = running_diff(getattr(device, "last_info", {}) or {}, entry)
    state = _load_state()
    if not stored and not running:
        if state.pop(device.id, None) is not None:
            _save_state(state)
        log.info("%s: device config matches its registry entry %s", device.id[:12], name)
        return
    if state.get(device.id) == digest:
        log.error(
            "%s: still differs from its registry entry %s after the server rebooted it to apply "
            "it (%s); left alone until the entry changes",
            device.id[:12],
            name,
            "; ".join(stored + running),
        )
        return
    if stored:
        log.warning(
            "%s: uploading its registry entry %s (%s)", device.id[:12], name, "; ".join(stored)
        )
        reply = await device.protocol.send_command(
            "device upload " + json.dumps(entry, separators=(",", ":"))
        )
        if "ERROR" in reply or "saved" not in reply:
            log.error("%s: `device upload` of %s refused: %s", device.id[:12], name, reply[:300])
            return
    task = _pending.get(device.id)
    if task is None or task.done():
        _pending[device.id] = asyncio.create_task(_reboot_when_idle(fleet, device, entry, digest))


async def _busy(fleet: FleetManager, device: Device) -> str | None:
    """What uses the chip now, or None."""
    from ..work import jobs
    from . import leds
    from .workflows import TEST_JOBS

    if leds._holders.get(device.id):
        return "a run holds its LEDs lit"
    job = fleet.get_flash_job(device.id)
    if job is not None and job.is_active:
        return "a flash"
    if durable.draining() or durable.in_flight():
        return "work in flight"
    if durable.launched():
        for kind in TEST_JOBS.kinds:
            if await jobs.active(TEST_JOBS, kind):
                return f"a {kind} job"
    return None


async def _reboot_when_idle(
    fleet: FleetManager, device: Device, entry: dict[str, Any], digest: str
) -> None:
    """Reboot the chip to apply its entry, once it has been up MIN_UPTIME_S and if no work
    uses it then. Otherwise its next reboot applies it."""
    from ..device.device import DeviceState
    from ..device.protocol import json_object_from

    name = entry["deviceId"]
    try:
        up_ms = float((getattr(device, "last_info", {}) or {}).get("millis", 0))
        await asyncio.sleep(max(0.0, MIN_UPTIME_S - up_ms / 1000.0) + 1.0)
        if device.state != DeviceState.CONNECTED:
            log.info("%s: disconnected; %s applies at its next boot", device.id[:12], name)
            return
        info, _ = json_object_from(await device.protocol.send_command("json info"), "version")
        if float(info.get("millis", 0)) < max(up_ms, MIN_UPTIME_S * 1000):
            # It reset meanwhile (a flash): it booted with the stored config, which its
            # reconnect checked again.
            return
        why = await _busy(fleet, device)
        if why is not None:
            log.info("%s: not rebooted to apply %s (%s)", device.id[:12], name, why)
            return
        state = _load_state()
        state[device.id] = digest
        _save_state(state)
        log.warning("%s: rebooting to apply its registry entry %s", device.id[:12], name)
        # The chip resets as it replies: the reply may be cut off.
        with (
            durable.hold(f"reboot {device.id[:12]} to apply {name}", owner="box-config"),
            contextlib.suppress(Exception),
        ):
            await device.protocol.send_command("reboot", timeout=3.0)
    except Exception as e:
        log.error("%s: reboot to apply %s failed: %s", device.id[:12], name, e)
