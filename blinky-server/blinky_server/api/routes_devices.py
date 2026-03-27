from __future__ import annotations

from typing import Any

from fastapi import APIRouter, HTTPException

from ..device.device import Device
from .deps import get_fleet
from .models import CommandResponse, DeviceResponse, SettingValueRequest, StatusResponse

router = APIRouter(tags=["devices"])


def _get_device_or_404(device_id: str) -> Device:
    device = get_fleet().get_device(device_id)
    if not device:
        raise HTTPException(404, f"Device not found: {device_id}")
    return device


@router.get("/devices")
async def list_devices() -> list[DeviceResponse]:
    """List all known devices."""
    return [DeviceResponse(**d.to_dict()) for d in get_fleet().get_all_devices()]


@router.get("/devices/{device_id}")
async def get_device(device_id: str) -> DeviceResponse:
    """Get info for a specific device."""
    device = _get_device_or_404(device_id)
    return DeviceResponse(**device.to_dict())


@router.get("/devices/{device_id}/settings")
async def get_settings(device_id: str) -> list[dict[str, Any]]:
    """Get all settings for a device."""
    device = _get_device_or_404(device_id)
    return await device.protocol.get_settings()


@router.get("/devices/{device_id}/settings/{category}")
async def get_settings_by_category(device_id: str, category: str) -> list[dict[str, Any]]:
    """Get settings for a device filtered by category."""
    device = _get_device_or_404(device_id)
    all_settings = await device.protocol.get_settings()
    return [s for s in all_settings if s.get("cat") == category]


@router.put("/devices/{device_id}/settings/{name}")
async def set_setting(device_id: str, name: str, body: SettingValueRequest) -> CommandResponse:
    """Set a single setting value."""
    device = _get_device_or_404(device_id)
    resp = await device.protocol.set_setting(name, body.value)
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/settings/save")
async def save_settings(device_id: str) -> CommandResponse:
    device = _get_device_or_404(device_id)
    resp = await device.protocol.save_settings()
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/settings/load")
async def load_settings(device_id: str) -> CommandResponse:
    device = _get_device_or_404(device_id)
    resp = await device.protocol.load_settings()
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/settings/defaults")
async def restore_defaults(device_id: str) -> CommandResponse:
    device = _get_device_or_404(device_id)
    resp = await device.protocol.restore_defaults()
    return CommandResponse(response=resp)


@router.post("/devices/{device_id}/release")
async def release_device(device_id: str) -> StatusResponse:
    """Release a device (e.g., for firmware flashing)."""
    ok = await get_fleet().release_device(device_id)
    if not ok:
        raise HTTPException(404, f"Device not found: {device_id}")
    return StatusResponse(status="released")


@router.post("/devices/{device_id}/reconnect")
async def reconnect_device(device_id: str) -> StatusResponse:
    """Reconnect a previously released device."""
    ok = await get_fleet().reconnect_device(device_id)
    if not ok:
        raise HTTPException(500, f"Failed to reconnect: {device_id}")
    return StatusResponse(status="connected")
