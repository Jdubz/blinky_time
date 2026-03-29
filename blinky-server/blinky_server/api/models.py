"""Pydantic models for API request/response types."""

from __future__ import annotations

from pydantic import BaseModel, Field

# ── Request Models ──


class CommandRequest(BaseModel):
    command: str = Field(..., min_length=1, description="Command string to send to device")


class SettingValueRequest(BaseModel):
    value: float | int | str = Field(..., description="Setting value to apply")


class ReleaseRequest(BaseModel):
    hold_seconds: int | None = Field(None, ge=0, description="Don't auto-reconnect for N seconds")


class OtaRequest(BaseModel):
    firmware_path: str = Field(..., description="Path to .hex or .uf2 firmware file on the server")


class OtaResponse(BaseModel):
    status: str
    message: str
    elapsed_s: float = 0


# ── Response Models ──


class CommandResponse(BaseModel):
    response: str


class StatusResponse(BaseModel):
    status: str


class DeviceResponse(BaseModel):
    id: str
    port: str
    platform: str
    transport: str
    state: str
    version: str | None = None
    device_type: str | None = None
    device_name: str | None = None
    width: int | None = None
    height: int | None = None
    leds: int | None = None
    configured: bool = False
    safe_mode: bool = False
    streaming: bool = False


class SettingResponse(BaseModel):
    name: str
    value: float | int | bool
    type: str = Field(..., alias="type")
    cat: str
    min: float
    max: float
    desc: str | None = None

    model_config = {"populate_by_name": True}


# ── WebSocket Message Models ──


class WsCommandMessage(BaseModel):
    type: str = "command"
    command: str
    device_id: str | None = None


class WsStreamControlMessage(BaseModel):
    type: str = "stream_control"
    enabled: bool
    mode: str = "on"
    device_id: str | None = None


class WsStreamData(BaseModel):
    type: str
    device_id: str
    data: dict[str, object]


class WsCommandResponseMessage(BaseModel):
    type: str = "response"
    device_id: str
    command: str
    response: str


class WsFleetResponseMessage(BaseModel):
    type: str = "fleet_response"
    command: str
    results: dict[str, str]
