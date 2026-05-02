from __future__ import annotations

from fastapi import APIRouter, Header, HTTPException

from ..device.device import Device, DeviceState
from .deps import assert_command_allowed, get_fleet
from .models import CommandRequest, CommandResponse, SettingValueRequest

router = APIRouter(tags=["commands"])


def _get_connected_device(device_id: str) -> Device:
    device = get_fleet().get_device(device_id)
    if not device:
        raise HTTPException(404, f"Device not found: {device_id}")
    if device.state != DeviceState.CONNECTED:
        raise HTTPException(
            409,
            f"Device {device_id[:12]} is not connected (state={device.state.value}). "
            + (
                "Use POST /devices/{id}/flash to recover."
                if device.state == DeviceState.DFU_RECOVERY
                else ""
            ),
        )
    return device


# ── Per-device commands ──


@router.post("/devices/{device_id}/command")
async def send_command(
    device_id: str,
    body: CommandRequest,
    x_deploy_tool: str | None = Header(None, alias="X-Deploy-Tool"),
) -> CommandResponse:
    """Send a raw command to a device.

    Device-mutating commands (`device upload`, `reboot`) require the
    X-Deploy-Tool header set by scripts/deploy.sh; everything else is
    open. See deps.assert_command_allowed for the gated list.
    """
    assert_command_allowed(body.command, x_deploy_tool)
    device = _get_connected_device(device_id)
    resp = await device.protocol.send_command(body.command)
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/generator/{name}")
async def set_generator(device_id: str, name: str) -> CommandResponse:
    device = _get_connected_device(device_id)
    resp = await device.protocol.set_generator(name)
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/effect/{name}")
async def set_effect(device_id: str, name: str) -> CommandResponse:
    device = _get_connected_device(device_id)
    resp = await device.protocol.set_effect(name)
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/stream/{mode}")
async def control_stream(device_id: str, mode: str) -> CommandResponse:
    """Start or stop streaming. mode: on, off, fast, debug, nn."""
    device = _get_connected_device(device_id)
    if mode == "off":
        resp = await device.protocol.stop_stream()
    else:
        resp = await device.protocol.start_stream(mode)
    return CommandResponse(response=resp)


# ── Fleet-wide commands ──


@router.post("/fleet/command")
async def fleet_command(
    body: CommandRequest,
    x_deploy_tool: str | None = Header(None, alias="X-Deploy-Tool"),
) -> dict[str, str]:
    """Send a command to all connected devices.

    Same deploy-gate as /devices/{id}/command for fleet-wide raw commands.
    """
    assert_command_allowed(body.command, x_deploy_tool)
    return await get_fleet().send_to_all(body.command)


@router.post("/fleet/generator/{name}")
async def fleet_generator(name: str) -> dict[str, str]:
    """Switch all devices to a generator."""
    return await get_fleet().send_to_all(f"gen {name}")


@router.post("/fleet/effect/{name}")
async def fleet_effect(name: str) -> dict[str, str]:
    """Switch all devices to an effect."""
    return await get_fleet().send_to_all(f"effect {name}")


@router.put("/fleet/settings/{name}")
async def fleet_set_setting(name: str, body: SettingValueRequest) -> dict[str, str]:
    """Set a setting on all devices."""
    return await get_fleet().send_to_all(f"set {name} {body.value}")


@router.post("/fleet/settings/save")
async def fleet_save_settings() -> dict[str, str]:
    """Save settings on all connected devices to flash."""
    return await get_fleet().send_to_all("save")


@router.post("/fleet/settings/load")
async def fleet_load_settings() -> dict[str, str]:
    """Load settings from flash on all connected devices."""
    return await get_fleet().send_to_all("load")


@router.post("/fleet/settings/defaults")
async def fleet_restore_defaults() -> dict[str, str]:
    """Restore runtime tunables on all connected devices.

    Resets soft settings (mic params, audio tracker config, generators).
    Does NOT wipe device identity or matrix size. To wipe identity,
    POST /fleet/command with body `{"command": "wipe_device_identity"}`
    and the X-Deploy-Tool header. The wipe command (and its deprecated
    `factory`/`reset` aliases, plus `device upload`/`reboot`) is in the
    deploy-gated set — see deps._DEPLOY_GATED_COMMAND_PREFIXES for the
    canonical list. No dedicated /fleet/wipe endpoint exists; deploy.sh
    is the canonical caller.

    Endpoint path retained for blinky-console compatibility; firmware
    command name updated to `restore_runtime_settings` per #141.
    """
    return await get_fleet().send_to_all("restore_runtime_settings")
