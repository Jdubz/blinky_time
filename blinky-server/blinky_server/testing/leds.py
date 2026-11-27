"""The test box's LEDs: dark unless a test runs (owner, 2026-11-24; docs/TEST_BOX.md).

The box's chip boots and idles dark: its device config (the box's own entry, testing/box_config.py)
has brightness 0. A test run lights it with the firmware's runtime-only ``leds`` override
(blinky-firmware render/LedBrightness.h), and drops the override when it ends, however it ends
(``leds config``: back to the stored config's brightness, what a reboot applies). The override
is never saved, and a reboot (a flash, a deploy) clears it.

Only on the box: every function here does nothing unless the server runs with --staging
(``fleet.staging_enabled``), and only for configured chips. A fleet server never sends ``leds``
(and api/deps refuses it there), so fleet devices keep their configured brightness.

Per run type (``needs_light``):

- health, the serial validation, music and beat pace, sweeps and threshold tuning read the chip
  over serial and don't need light. They are lit anyway (the box is lit while it tests), and on
  firmware without ``leds`` they go on with an ERROR in the log.
- camera reviews (camera_review.py submits validation with ``needs_light``) and the camera
  calibration (camera_calibrate.py) need light: without ``leds`` they fail.

Restarts: lighting is per-run setup in the workflow body (durable.py rule 3), so a recovered
job lights its chips again. A drain stops held workflows through their cleanup (dark); after a
crash, the chip is set dark when it reconnects (``reconcile``) unless a run in this process
holds it lit.
"""

from __future__ import annotations

import collections
import contextlib
import logging
from collections.abc import AsyncIterator, Iterable
from typing import TYPE_CHECKING, Any

from ..device.protocol import json_object_from
from ..work import durable

if TYPE_CHECKING:
    from ..device.device import Device

log = logging.getLogger(__name__)

# The brightness a test run lights the box's grid at. A box setting, not a device one: the
# camera's exposure and gain (camera_capture.py, 1 ms at gain 1.0) were calibrated for the grid
# at 40 (docs/TEST_BOX_CAMERA_HARNESS_PLAN.md, "Levels"), and any chip in the grid's place
# needs the same. camera_calibrate.py's --brightness default must match (a test checks).
TEST_BRIGHTNESS = 40

# Runs in this process holding each chip lit, by device id.
_holders: collections.Counter[str] = collections.Counter()


class LedsError(RuntimeError):
    """A chip couldn't be lit for a run that needs light."""


def on_box() -> bool:
    """The server runs with --staging: the test box, whose chips are all test chips."""
    from ..api.deps import get_fleet

    return bool(get_fleet().staging_enabled)


def parse_reply(raw: str) -> dict[str, Any] | None:
    """The ``leds`` reply: {"brightness", "config", "override"}, or None from firmware without
    the command. Raises ValueError on anything else (an ERROR line, a garbled reply)."""
    if "Unknown command" in raw:
        return None
    obj, _ = json_object_from(raw, "override")
    return obj


async def send(device: Device, arg: str) -> dict[str, Any] | None:
    """``leds <arg>`` on one chip: its status after, or None if its firmware lacks ``leds``."""
    return parse_reply(await device.protocol.send_command(f"leds {arg}"))


async def _light(device: Device, needs_light: bool, purpose: str) -> None:
    try:
        status = await send(device, str(TEST_BRIGHTNESS))
        if status is None:
            raise LedsError(
                f"{device.id[:12]}: firmware {device.version} has no `leds` command, so the "
                "grid stays at its config brightness"
            )
        if status.get("brightness") != TEST_BRIGHTNESS or status.get("override") is not True:
            raise LedsError(
                f"{device.id[:12]}: `leds {TEST_BRIGHTNESS}` left {status}, not brightness "
                f"{TEST_BRIGHTNESS} with the override on"
            )
    except (LedsError, ValueError, OSError, TimeoutError) as e:
        if needs_light:
            raise LedsError(f"{purpose} needs the LEDs lit: {e}") from e
        log.error("LEDs not lit for %s (it goes on: it doesn't need light): %s", purpose, e)


async def _darken(device: Device, purpose: str) -> None:
    """Drop the override. Never raises: a run's own outcome must not be masked by its cleanup."""
    try:
        status = await send(device, "config")
        if status is None:
            log.error(
                "%s: firmware %s has no `leds` command; the grid stays at its config brightness",
                device.id[:12],
                device.version,
            )
        elif status.get("override") is not False:
            log.error("%s: `leds config` after %s left %s", device.id[:12], purpose, status)
    except Exception as e:  # cleanup: report, never raise over the run's result
        log.error("%s: LEDs not set back after %s: %s", device.id[:12], purpose, e)


@contextlib.asynccontextmanager
async def lit(devices: Iterable[Device], *, needs_light: bool, purpose: str) -> AsyncIterator[None]:
    """The box's chips lit for the block, and back to their config brightness after it, on
    success, failure, cancel or a drain's stop. Nested holders (the staging suite around its
    validation session) share it: the last one out darkens. Off the box: does nothing."""
    chips = [d for d in devices if d.configured]
    if not chips or not on_box():
        yield
        return
    held: list[Device] = []
    try:
        # Device I/O outside a step: held, so a drain waits for it (as the session's setup).
        with durable.hold(f"LEDs on for {purpose}"):
            for d in chips:
                _holders[d.id] += 1
                held.append(d)
                if _holders[d.id] == 1:
                    await _light(d, needs_light, purpose)
        yield
    finally:
        with durable.hold(f"LEDs off after {purpose}"):
            for d in held:
                _holders[d.id] -= 1
                if _holders[d.id] <= 0:
                    del _holders[d.id]
                    await _darken(d, purpose)


def refuse_off_box(commands: Iterable[str]) -> None:
    """A test job's own commands: a box-only one (``leds``) off the box fails the job. The API
    refuses them at submission; this covers a job recovered from before that check."""
    from ..api.deps import is_test_box_only_command

    bad = [c for c in commands if is_test_box_only_command(c)]
    if bad and not on_box():
        raise RuntimeError(f"test-box-only commands on a fleet server: {bad}")


async def reconcile(device: Device, *, staging: bool) -> None:
    """On connect (device/manager.py), on the box (``staging``: the fleet manager's
    staging_enabled): a chip no run here holds goes dark (a run cut off by a crash left it lit);
    one a run holds is lit again (it may have rebooted). Never raises: connecting must not fail
    over the LEDs."""
    if not staging or not device.configured:
        return
    try:
        if _holders.get(device.id):
            await _light(device, needs_light=False, purpose="the run holding it (reconnected)")
        else:
            status = await send(device, "config")
            if status is None:
                log.info(
                    "%s: firmware %s has no `leds` command (LEDs at config brightness)",
                    device.id[:12],
                    device.version,
                )
    except Exception as e:
        log.error("%s: LEDs not reconciled on connect: %s", device.id[:12], e)
