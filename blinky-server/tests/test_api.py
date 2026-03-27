"""Tests for the REST API endpoints."""

from __future__ import annotations

from httpx import AsyncClient


async def test_list_devices(api_client: AsyncClient) -> None:
    resp = await api_client.get("/api/devices")
    assert resp.status_code == 200
    devices = resp.json()
    assert len(devices) == 2
    assert devices[0]["id"] == "MOCK_DEVICE_000"
    assert devices[0]["platform"] == "nrf52840"
    assert devices[0]["state"] == "connected"
    assert devices[1]["id"] == "MOCK_DEVICE_001"
    assert devices[1]["platform"] == "esp32s3"


async def test_get_device(api_client: AsyncClient) -> None:
    resp = await api_client.get("/api/devices/MOCK_DEVICE_000")
    assert resp.status_code == 200
    device = resp.json()
    assert device["device_name"] == "Long Tube"
    assert device["leds"] == 240


async def test_get_device_not_found(api_client: AsyncClient) -> None:
    resp = await api_client.get("/api/devices/NONEXISTENT")
    assert resp.status_code == 404


async def test_get_settings(api_client: AsyncClient) -> None:
    resp = await api_client.get("/api/devices/MOCK_DEVICE_000/settings")
    assert resp.status_code == 200
    settings = resp.json()
    assert len(settings) == 2
    assert settings[0]["name"] == "basespawnchance"


async def test_get_settings_by_category(api_client: AsyncClient) -> None:
    resp = await api_client.get("/api/devices/MOCK_DEVICE_000/settings/fire")
    assert resp.status_code == 200
    settings = resp.json()
    assert all(s["cat"] == "fire" for s in settings)


async def test_set_setting(api_client: AsyncClient) -> None:
    resp = await api_client.put(
        "/api/devices/MOCK_DEVICE_000/settings/basespawnchance",
        json={"value": 0.8},
    )
    assert resp.status_code == 200
    assert "OK" in resp.json()["response"]


async def test_set_setting_missing_value(api_client: AsyncClient) -> None:
    resp = await api_client.put(
        "/api/devices/MOCK_DEVICE_000/settings/basespawnchance",
        json={},
    )
    assert resp.status_code == 422  # Pydantic validation error


async def test_save_settings(api_client: AsyncClient) -> None:
    resp = await api_client.post("/api/devices/MOCK_DEVICE_000/settings/save")
    assert resp.status_code == 200
    assert "OK" in resp.json()["response"]


async def test_send_command(api_client: AsyncClient) -> None:
    resp = await api_client.post(
        "/api/devices/MOCK_DEVICE_000/command",
        json={"command": "ble"},
    )
    assert resp.status_code == 200
    assert "[BLE]" in resp.json()["response"]


async def test_send_command_empty_rejected(api_client: AsyncClient) -> None:
    resp = await api_client.post(
        "/api/devices/MOCK_DEVICE_000/command",
        json={"command": ""},
    )
    assert resp.status_code == 422  # min_length=1 validation


async def test_set_generator(api_client: AsyncClient) -> None:
    resp = await api_client.post("/api/devices/MOCK_DEVICE_000/generator/water")
    assert resp.status_code == 200
    assert "Water" in resp.json()["response"]


async def test_set_effect(api_client: AsyncClient) -> None:
    resp = await api_client.post("/api/devices/MOCK_DEVICE_000/effect/hue")
    assert resp.status_code == 200
    assert "hue" in resp.json()["response"]


async def test_control_stream(api_client: AsyncClient) -> None:
    resp = await api_client.post("/api/devices/MOCK_DEVICE_000/stream/on")
    assert resp.status_code == 200

    resp = await api_client.post("/api/devices/MOCK_DEVICE_000/stream/off")
    assert resp.status_code == 200


async def test_fleet_command(api_client: AsyncClient) -> None:
    resp = await api_client.post(
        "/api/fleet/command",
        json={"command": "ble"},
    )
    assert resp.status_code == 200
    results = resp.json()
    assert len(results) == 2


async def test_fleet_generator(api_client: AsyncClient) -> None:
    resp = await api_client.post("/api/fleet/generator/lightning")
    assert resp.status_code == 200
    results = resp.json()
    assert all("Lightning" in v for v in results.values())


async def test_fleet_set_setting(api_client: AsyncClient) -> None:
    resp = await api_client.put(
        "/api/fleet/settings/basespawnchance",
        json={"value": 0.3},
    )
    assert resp.status_code == 200
    results = resp.json()
    assert all("OK" in v for v in results.values())


async def test_release_and_reconnect(api_client: AsyncClient) -> None:
    # Release
    resp = await api_client.post("/api/devices/MOCK_DEVICE_000/release")
    assert resp.status_code == 200
    assert resp.json()["status"] == "released"

    # Verify disconnected
    resp = await api_client.get("/api/devices/MOCK_DEVICE_000")
    assert resp.json()["state"] == "disconnected"
