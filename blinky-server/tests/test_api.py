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


async def test_send_command_device_upload_blocked_without_deploy_tool(
    api_client: AsyncClient,
) -> None:
    """`device upload` requires X-Deploy-Tool header (CLAUDE.md upload safety)."""
    resp = await api_client.post(
        "/api/devices/MOCK_DEVICE_000/command",
        json={"command": 'device upload {"deviceId":"x","ledWidth":1,"ledHeight":1}'},
    )
    assert resp.status_code == 403
    assert "X-Deploy-Tool" in resp.json()["detail"]


async def test_send_command_reboot_blocked_without_deploy_tool(
    api_client: AsyncClient,
) -> None:
    """`reboot` requires X-Deploy-Tool header."""
    resp = await api_client.post(
        "/api/devices/MOCK_DEVICE_000/command",
        json={"command": "reboot"},
    )
    assert resp.status_code == 403
    assert "X-Deploy-Tool" in resp.json()["detail"]


async def test_send_command_device_upload_allowed_with_deploy_tool(
    api_client: AsyncClient,
) -> None:
    """deploy.sh's X-Deploy-Tool header passes the gate."""
    resp = await api_client.post(
        "/api/devices/MOCK_DEVICE_000/command",
        json={"command": 'device upload {"deviceId":"x","ledWidth":1,"ledHeight":1}'},
        headers={"X-Deploy-Tool": "deploy.sh-abc1234"},
    )
    # 200 (mock device responds) — the gate is what we're testing here.
    assert resp.status_code == 200


async def test_fleet_command_reboot_blocked_without_deploy_tool(
    api_client: AsyncClient,
) -> None:
    """Fleet-wide raw command endpoint has the same gate."""
    resp = await api_client.post(
        "/api/fleet/command",
        json={"command": "reboot"},
    )
    assert resp.status_code == 403
    assert "X-Deploy-Tool" in resp.json()["detail"]


async def test_fleet_command_reboot_allowed_with_deploy_tool(
    api_client: AsyncClient,
) -> None:
    """Fleet-wide raw command also passes the gate when deploy tool is set."""
    resp = await api_client.post(
        "/api/fleet/command",
        json={"command": "reboot"},
        headers={"X-Deploy-Tool": "deploy.sh-abc1234"},
    )
    assert resp.status_code == 200


async def test_send_command_wipe_device_identity_blocked(
    api_client: AsyncClient,
) -> None:
    """`wipe_device_identity` (heavy reset) requires X-Deploy-Tool."""
    resp = await api_client.post(
        "/api/devices/MOCK_DEVICE_000/command",
        json={"command": "wipe_device_identity"},
    )
    assert resp.status_code == 403
    assert "X-Deploy-Tool" in resp.json()["detail"]


async def test_send_command_wipe_device_identity_allowed_with_deploy_tool(
    api_client: AsyncClient,
) -> None:
    """`wipe_device_identity` passes the gate when deploy tool is set; mock
    transport accepts it as part of the renamed command set (#141)."""
    resp = await api_client.post(
        "/api/devices/MOCK_DEVICE_000/command",
        json={"command": "wipe_device_identity"},
        headers={"X-Deploy-Tool": "deploy.sh-abc1234"},
    )
    assert resp.status_code == 200


async def test_send_command_factory_alias_blocked(
    api_client: AsyncClient,
) -> None:
    """Old `factory` alias still gated (deprecated but accepted)."""
    resp = await api_client.post(
        "/api/devices/MOCK_DEVICE_000/command",
        json={"command": "factory"},
    )
    assert resp.status_code == 403
    assert "X-Deploy-Tool" in resp.json()["detail"]


async def test_send_command_reset_alias_blocked(
    api_client: AsyncClient,
) -> None:
    """Old `reset` alias still gated."""
    resp = await api_client.post(
        "/api/devices/MOCK_DEVICE_000/command",
        json={"command": "reset"},
    )
    assert resp.status_code == 403
    assert "X-Deploy-Tool" in resp.json()["detail"]


# Unit-level edge cases for is_deploy_gated_command itself. The prefix-match
# logic does case folding + strip + adjacent-word check; non-trivial enough
# that regressions deserve direct coverage rather than only round-tripping
# through the API.
def test_is_deploy_gated_case_insensitive() -> None:
    from blinky_server.api.deps import is_deploy_gated_command

    assert is_deploy_gated_command("REBOOT")
    assert is_deploy_gated_command("Reboot")


def test_is_deploy_gated_strips_whitespace() -> None:
    from blinky_server.api.deps import is_deploy_gated_command

    assert is_deploy_gated_command("  reboot  ")
    assert is_deploy_gated_command("\treboot")


def test_is_deploy_gated_no_substring_match() -> None:
    """Adjacent-word match must not gate `rebooting`, `reset_gain`, etc."""
    from blinky_server.api.deps import is_deploy_gated_command

    assert not is_deploy_gated_command("rebooting")
    assert not is_deploy_gated_command("reset_gain")
    assert not is_deploy_gated_command("factory_install")


def test_is_deploy_gated_prefix_with_args() -> None:
    """`device upload <json>` form is gated."""
    from blinky_server.api.deps import is_deploy_gated_command

    assert is_deploy_gated_command('device upload {"deviceId":"x"}')
    assert is_deploy_gated_command("device upload arbitrary args")


def test_is_deploy_gated_prefix_bare_no_args() -> None:
    """Bare `device upload` (no args) hits the `cmd_normalized == prefix`
    branch of is_deploy_gated_command's any() — the prefix-with-args test
    only exercises the `startswith(prefix + " ")` branch. Per PR 138
    round-13 review."""
    from blinky_server.api.deps import is_deploy_gated_command

    assert is_deploy_gated_command("device upload")


def test_is_deploy_gated_passthrough_safe_commands() -> None:
    """Safe commands aren't gated."""
    from blinky_server.api.deps import is_deploy_gated_command

    assert not is_deploy_gated_command("json info")
    assert not is_deploy_gated_command("ping")
    assert not is_deploy_gated_command("restore_runtime_settings")
    assert not is_deploy_gated_command("save")
    assert not is_deploy_gated_command("gen fire")
    assert not is_deploy_gated_command("set foo 1")


def test_is_deploy_gated_collapses_whitespace() -> None:
    """`device  upload` (double space), `\\tdevice\\tupload` etc. all gated."""
    from blinky_server.api.deps import is_deploy_gated_command

    assert is_deploy_gated_command("device  upload")
    assert is_deploy_gated_command("device\tupload")
    assert is_deploy_gated_command("device\t upload {}")
    assert is_deploy_gated_command("device\t\tupload x")
    # Sanity: still doesn't false-positive on adjacent words after normalization.
    assert not is_deploy_gated_command("device_upload")
    assert not is_deploy_gated_command("undevice upload")


def test_is_deploy_gated_deprecated_aliases_exact_only() -> None:
    """Deprecated `factory`/`reset` are gated as exact-match only.

    Prefix-form match would also gate hypothetical safe future commands
    like `reset_session <args>` or `factory_install <package>`. PR 138
    round-8 split the aliases into _DEPLOY_GATED_EXACT to prevent that.
    Case-folding and whitespace strip apply equally to the exact-match
    path (per PR 138 round-11 review — explicitly testing both aliases).
    """
    from blinky_server.api.deps import is_deploy_gated_command

    # Exact match: gated.
    assert is_deploy_gated_command("factory")
    assert is_deploy_gated_command("reset")
    # Case-folding applies to BOTH deprecated aliases:
    assert is_deploy_gated_command("FACTORY")
    assert is_deploy_gated_command("Factory")
    assert is_deploy_gated_command("RESET")
    assert is_deploy_gated_command("Reset")
    # Whitespace strip applies to both:
    assert is_deploy_gated_command("  reset  ")
    assert is_deploy_gated_command("\tfactory\n")
    # Prefix form (with args): NOT gated, because these aliases are no-arg.
    assert not is_deploy_gated_command("factory install")
    assert not is_deploy_gated_command("reset session")
    assert not is_deploy_gated_command("reset to defaults")


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
