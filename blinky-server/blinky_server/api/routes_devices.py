from __future__ import annotations

import asyncio
from typing import Any

from fastapi import APIRouter, HTTPException

from ..device.device import Device, DeviceState
from .deps import get_fleet
from .models import (
    CommandResponse, DeviceResponse, OtaRequest, OtaResponse,
    ReleaseRequest, SettingValueRequest, StatusResponse,
)

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
async def release_device(device_id: str, body: ReleaseRequest | None = None) -> StatusResponse:
    """Release a device (e.g., for firmware flashing)."""
    hold_seconds = body.hold_seconds if body else None
    ok = await get_fleet().release_device(device_id, hold_seconds=hold_seconds)
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


@router.post("/devices/{device_id}/ota")
async def ota_upload(device_id: str, body: OtaRequest) -> OtaResponse:
    """Upload firmware to a device via OTA (UF2 for serial, BLE DFU for BLE).

    The server handles the entire flow:
    1. Sends bootloader command via its existing transport
    2. Disconnects the transport
    3. Detects UF2 drive / establishes BLE DFU connection
    4. Copies firmware / performs DFU transfer
    5. Waits for device to reboot
    6. Reconnects automatically via fleet manager

    No port contention — the server owns the connection throughout.
    """
    from pathlib import Path

    device = _get_device_or_404(device_id)

    if device.state not in (DeviceState.CONNECTED, DeviceState.DFU_RECOVERY):
        raise HTTPException(409, f"Device not connected (state={device.state.value})")

    # Validate firmware path — restrict to allowed directories
    firmware = Path(body.firmware_path).resolve()
    allowed_dirs = [Path("/tmp"), Path.home()]
    if not any(firmware.is_relative_to(d) for d in allowed_dirs):
        raise HTTPException(400, f"Firmware path not in allowed directory: {firmware}")
    if not firmware.is_file():
        raise HTTPException(400, f"Firmware file not found: {firmware}")

    platform = device.platform

    if platform != "nrf52840":
        raise HTTPException(400, f"OTA not yet supported for platform: {platform}")

    fleet = get_fleet()

    # Device is stuck in BLE DFU bootloader (SafeBoot crash recovery).
    # Push firmware directly — no bootloader entry needed.
    if device.state == DeviceState.DFU_RECOVERY:
        from ..ota.ble_dfu import upload_ble_dfu
        from ..ota.compile import ensure_dfu_zip

        # Use ble_address (app address) — NOT device.port which may be the
        # bootloader address for placeholder devices.
        app_ble_addr = device.ble_address
        if not app_ble_addr:
            raise HTTPException(400, "Device in DFU recovery but BLE app address unknown")

        try:
            dfu_zip = await asyncio.to_thread(ensure_dfu_zip, str(firmware))
        except ValueError as e:
            raise HTTPException(400, str(e))

        fleet.hold_reconnect(device.id, 180)
        fleet.pause_discovery()
        try:
            result = await upload_ble_dfu(
                app_ble_address=app_ble_addr,
                dfu_zip_path=dfu_zip,
                # No bootloader entry — device is already in DFU bootloader
            )
        finally:
            device.state = DeviceState.DISCONNECTED
            fleet.resume_discovery()
            fleet.resume_reconnect(device.id)

        if result["status"] == "ok":
            return OtaResponse(**result)
        else:
            raise HTTPException(500, result["message"])

    transport_type = device.transport.transport_type

    if transport_type == "serial":
        from ..ota import upload_with_ble_fallback

        # Blackout auto-reconnect while we do the upload
        fleet.hold_reconnect(device_id, 120)
        fleet.pause_discovery()  # Prevent BleakScanner conflict if BLE fallback triggers
        try:
            result = await upload_with_ble_fallback(
                serial_port=device.port,
                firmware_path=str(firmware),
                transport=device.transport,
                protocol=device.protocol,
                ble_address=device.ble_address,
            )
        finally:
            device.state = DeviceState.DISCONNECTED
            fleet.resume_discovery()
            fleet.resume_reconnect(device_id)

        if result["status"] == "ok":
            return OtaResponse(**result)
        else:
            raise HTTPException(500, result["message"])

    elif transport_type == "ble":
        from ..ota.ble_dfu import upload_ble_dfu
        from ..ota.compile import ensure_dfu_zip

        # Ensure we have a DFU zip (auto-converts .hex if needed)
        try:
            dfu_zip = await asyncio.to_thread(ensure_dfu_zip, str(firmware))
        except ValueError as e:
            raise HTTPException(400, str(e))

        # Capture transport write_line before hold (which may disconnect)
        ble_transport = device.transport

        async def enter_bootloader_via_ble(cmd: str):
            """Send bootloader command over BLE NUS, then disconnect."""
            await ble_transport.write_line(cmd)
            await asyncio.sleep(0.5)  # Let command transmit before disconnect
            await ble_transport.disconnect()

        fleet.hold_reconnect(device_id, 180)
        fleet.pause_discovery()  # Prevent BleakScanner conflict during DFU
        try:
            result = await upload_ble_dfu(
                app_ble_address=device.port,  # BLE address is stored as port
                dfu_zip_path=dfu_zip,
                enter_bootloader_via_ble=enter_bootloader_via_ble,
            )
        finally:
            # Mark device as disconnected so auto-reconnect picks it up.
            # The old transport was disconnected during DFU — device is
            # rebooting with new firmware and will re-advertise on BLE.
            device.state = DeviceState.DISCONNECTED
            fleet.resume_discovery()
            fleet.resume_reconnect(device_id)

        if result["status"] == "ok":
            return OtaResponse(**result)
        else:
            raise HTTPException(500, result["message"])

    else:
        raise HTTPException(400, f"OTA not supported for transport: {transport_type}")
