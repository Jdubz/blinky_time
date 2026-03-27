from __future__ import annotations

from fastapi import APIRouter, HTTPException

from ..device.device import Device
from .deps import get_fleet
from .models import CommandRequest, CommandResponse, SettingValueRequest

router = APIRouter(tags=["commands"])


def _get_device_or_404(device_id: str) -> Device:
    device = get_fleet().get_device(device_id)
    if not device:
        raise HTTPException(404, f"Device not found: {device_id}")
    return device


# ── Per-device commands ──


@router.post("/devices/{device_id}/command")
async def send_command(device_id: str, body: CommandRequest) -> CommandResponse:
    """Send a raw command to a device."""
    device = _get_device_or_404(device_id)
    resp = await device.protocol.send_command(body.command)
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/generator/{name}")
async def set_generator(device_id: str, name: str) -> CommandResponse:
    device = _get_device_or_404(device_id)
    resp = await device.protocol.set_generator(name)
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/effect/{name}")
async def set_effect(device_id: str, name: str) -> CommandResponse:
    device = _get_device_or_404(device_id)
    resp = await device.protocol.set_effect(name)
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/stream/{mode}")
async def control_stream(device_id: str, mode: str) -> CommandResponse:
    """Start or stop streaming. mode: on, off, fast, debug, nn."""
    device = _get_device_or_404(device_id)
    if mode == "off":
        resp = await device.protocol.stop_stream()
    else:
        resp = await device.protocol.start_stream(mode)
    return CommandResponse(response=resp)


# ── Fleet-wide commands ──


@router.post("/fleet/command")
async def fleet_command(body: CommandRequest) -> dict[str, str]:
    """Send a command to all connected devices."""
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
