"""Server-side fleet deploy (#269): deploy.sh's hardened pipeline, step by step.

The old POST /api/fleet/deploy compiled whatever source the server had and
flashed every eligible device with only the API key. These tests pin the
replacement: stable releases only (via firmware_release.py, the exact
command deploy.sh runs), explicit scope, the single flash path, settings
restore + save, and the post-deploy assertion. Any failed step stops the
deploy.
"""

from __future__ import annotations

import json
import subprocess
from collections.abc import AsyncGenerator
from pathlib import Path
from typing import Any

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from blinky_server.api.app import create_app
from blinky_server.api.deps import require_api_key, require_deploy_tool, set_fleet
from blinky_server.device.device import DeviceState
from blinky_server.device.manager import FleetManager
from blinky_server.firmware import deploy_pipeline as dp
from blinky_server.firmware.deploy_pipeline import DeployError

STABLE_VERSION = "b215-39df3912"


class FakeProtocol:
    def __init__(self, info: dict[str, Any] | Exception) -> None:
        self.info = info
        self.sent: list[str] = []

    async def send_command(self, command: str, timeout: float | None = None) -> str:
        self.sent.append(command)
        if isinstance(self.info, Exception):
            raise self.info
        return json.dumps(self.info)


class FakeDevice:
    def __init__(
        self,
        device_id: str,
        state: DeviceState = DeviceState.CONNECTED,
        platform: str = "nrf52840",
        info: dict[str, Any] | Exception | None = None,
    ) -> None:
        self.id = device_id
        self.state = state
        self.platform = platform
        self.device_name = device_id
        self.protocol = FakeProtocol(
            info
            if info is not None
            else {
                "version": STABLE_VERSION,
                "fps": 140.0,
                "audioOverruns": 0,
                "audioSamplesLost": 0,
            }
        )


class FakeFleet:
    def __init__(self, devices: list[FakeDevice]) -> None:
        self.devices = {d.id: d for d in devices}
        self.broadcasts: list[str] = []
        self.broadcast_result: dict[str, str] = {"broadcast": "OK"}
        self.recovery: tuple[str, list[str]] | None = None

    def get_all_devices(self) -> list[FakeDevice]:
        return list(self.devices.values())

    def get_device(self, device_id: str) -> FakeDevice | None:
        return self.devices.get(device_id)

    async def broadcast(self, command: str) -> dict[str, str]:
        self.broadcasts.append(command)
        return dict(self.broadcast_result)

    def set_recovery_firmware(self, path: str, device_ids: list[str]) -> None:
        self.recovery = (path, device_ids)


class FakeJob:
    progress = 0
    progress_message = ""


def _fleet() -> FakeFleet:
    return FakeFleet(
        [
            FakeDevice("hat_v1"),
            FakeDevice("ble_lamp", state=DeviceState.PRESENT),
            FakeDevice("stuck", state=DeviceState.DFU_RECOVERY),
            FakeDevice("gone", state=DeviceState.DISCONNECTED),
            FakeDevice("esp", platform="esp32s3"),
        ]
    )


# ── Targets ─────────────────────────────────────────────────────────────


def test_all_means_connected_or_present_nrf52840_only() -> None:
    ids = [d.id for d in dp.resolve_targets(_fleet(), "all")]
    assert ids == ["hat_v1", "ble_lamp"]


def test_explicit_list_may_name_a_device_in_its_bootloader() -> None:
    ids = [d.id for d in dp.resolve_targets(_fleet(), ["stuck", "hat_v1", "hat_v1"])]
    assert ids == ["stuck", "hat_v1"]  # de-duplicated, order kept


@pytest.mark.parametrize(
    ("ids", "match"),
    [
        (["typo"], "unknown"),
        (["gone"], "not flashable"),
        (["esp"], "not flashable"),
        ([], "empty device_ids"),
        ("everything", "list of IDs"),
    ],
)
def test_bad_scope_fails_loudly(ids: Any, match: str) -> None:
    with pytest.raises(DeployError, match=match):
        dp.resolve_targets(_fleet(), ids)


# ── Release: stable only, through firmware_release.py ─────────────────


def _fake_run(stdout: str = "", returncode: int = 0, stderr: str = "") -> Any:
    calls: list[list[str]] = []

    def run(args: list[str], **_: Any) -> subprocess.CompletedProcess[str]:
        calls.append(args)
        return subprocess.CompletedProcess(args, returncode, stdout, stderr)

    run.calls = calls  # type: ignore[attr-defined]
    return run


def test_release_runs_firmware_release_download_like_deploy_sh(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    hex_path = tmp_path / "blinky-things-b215.hex"
    hex_path.write_text(":00000001FF\n")
    out = {
        "tag": "b215",
        "stable": True,
        "hex": str(hex_path),
        "uf2": "x",
        "version": STABLE_VERSION,
    }
    run = _fake_run(json.dumps(out))
    monkeypatch.setattr(dp.subprocess, "run", run)
    rel = dp.fetch_stable_release("b215", tmp_path)
    assert rel["version"] == STABLE_VERSION
    args = run.calls[0]
    assert args[1].endswith("tools/firmware_release.py") or args[1].endswith(
        "tools\\firmware_release.py"
    )
    assert args[2:] == ["download", "--out", str(tmp_path), "--tag", "b215"]


def test_release_refuses_a_non_stable_release(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    hex_path = tmp_path / "x.hex"
    hex_path.write_text("x")
    out = {"tag": "b216", "stable": False, "hex": str(hex_path), "uf2": "x", "version": "b216-aaaa"}
    monkeypatch.setattr(dp.subprocess, "run", _fake_run(json.dumps(out)))
    with pytest.raises(DeployError, match="not a stable release"):
        dp.fetch_stable_release(None, tmp_path)


def test_release_download_failure_is_reported(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(
        dp.subprocess,
        "run",
        _fake_run(returncode=1, stderr="ERROR: b215: blinky-things-b215.hex sha256 mismatch"),
    )
    with pytest.raises(DeployError, match="sha256 mismatch"):
        dp.fetch_stable_release("b215", tmp_path)


# ── Settings ────────────────────────────────────────────────────────────


def test_broadcast_rule_matches_deploy_sh() -> None:
    assert (
        dp.broadcast_failures(
            {"broadcast": "OK", "abc": "OK saved", "def": "skipped: state=present"}
        )
        == []
    )
    assert dp.broadcast_failures({"broadcast": "FAILED: adapter busy"}) == [
        "broadcast: FAILED: adapter busy"
    ]
    assert dp.broadcast_failures({"abc": "ERR unknown command"}) == ["abc: ERR unknown command"]


async def test_restore_then_save_and_stop_on_failure() -> None:
    fleet = _fleet()
    await dp.restore_and_save(fleet)
    assert fleet.broadcasts == ["restore_runtime_settings", "save"]

    fleet = _fleet()
    fleet.broadcast_result = {"broadcast": "OK", "hat_v1": "ERR"}
    with pytest.raises(DeployError, match="restore_runtime_settings did not land"):
        await dp.restore_and_save(fleet)
    assert fleet.broadcasts == ["restore_runtime_settings"]  # never saved half-restored state


# ── Post-deploy assertion ───────────────────────────────────────────────


async def test_assertion_passes_on_the_right_version() -> None:
    rows = await dp.assert_post_deploy(_fleet(), ["hat_v1", "ble_lamp"], STABLE_VERSION)
    assert rows["hat_v1"]["ok"] and rows["ble_lamp"]["ok"]


@pytest.mark.parametrize(
    ("device", "match"),
    [
        (
            FakeDevice(
                "hat_v1",
                info={"version": "b214-7a4664d6", "audioOverruns": 0, "audioSamplesLost": 0},
            ),
            "version",
        ),
        (FakeDevice("hat_v1", info={"version": STABLE_VERSION, "fps": 140.0}), "overrun counters"),
        (FakeDevice("hat_v1", state=DeviceState.DFU_RECOVERY), "dfu_recovery"),
        (FakeDevice("hat_v1", info=TimeoutError("no reply")), "unreachable"),
    ],
)
async def test_assertion_fails_loudly(
    device: FakeDevice, match: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(dp, "JSON_INFO_RETRY_S", 0.0)
    with pytest.raises(DeployError) as e:
        await dp.assert_post_deploy(FakeFleet([device]), ["hat_v1"], STABLE_VERSION)
    assert match in json.dumps(e.value.detail)


# ── The pipeline ────────────────────────────────────────────────────────


async def _deploy(
    fleet: FakeFleet,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    *,
    stable: bool = True,
    flash_status: str = "ok",
) -> tuple[dict[str, Any], list[Any]]:
    hex_path = tmp_path / "blinky-things-b215.hex"
    hex_path.write_text(":00000001FF\n")
    out = {
        "tag": "b215",
        "stable": stable,
        "hex": str(hex_path),
        "uf2": "x",
        "version": STABLE_VERSION,
    }
    monkeypatch.setattr(dp.subprocess, "run", _fake_run(json.dumps(out)))
    flashed: list[Any] = []

    async def flash(_fleet: Any, ids: list[str], fw: Path, _job: Any) -> dict[str, Any]:
        flashed.append((ids, fw))
        return {"status": flash_status, "message": f"{len(ids)}/{len(ids)} devices flashed"}

    stored = tmp_path / "firmware.hex"
    result = await dp.run_deploy(
        fleet,
        FakeJob(),
        release_tag=None,
        device_ids=["hat_v1"],
        download_dir=tmp_path,
        store_firmware=lambda p, tag: stored,
        flash=flash,
        settle_s=0.0,
    )
    return result, flashed


async def test_deploy_runs_every_step(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    fleet = _fleet()
    result, flashed = await _deploy(fleet, monkeypatch, tmp_path)
    assert result["status"] == "ok", result
    assert list(result["steps"]) == ["targets", "release", "store", "flash", "settings", "verify"]
    assert flashed == [(["hat_v1"], tmp_path / "firmware.hex")]
    assert fleet.recovery == (
        str(tmp_path / "firmware.hex"),
        ["hat_v1"],
    )  # armed for exactly the targets
    assert fleet.broadcasts == ["restore_runtime_settings", "save"]


async def test_deploy_never_flashes_a_non_stable_release(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    fleet = _fleet()
    result, flashed = await _deploy(fleet, monkeypatch, tmp_path, stable=False)
    assert result["status"] == "error" and result["failed_step"] == "release"
    assert flashed == [] and fleet.recovery is None and fleet.broadcasts == []


async def test_deploy_stops_after_a_failed_flash(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    fleet = _fleet()
    result, _ = await _deploy(fleet, monkeypatch, tmp_path, flash_status="error")
    assert result["status"] == "error" and result["failed_step"] == "flash"
    assert fleet.broadcasts == []  # no settings writes after a failed flash


# ── Route ───────────────────────────────────────────────────────────────


@pytest_asyncio.fixture
async def client() -> AsyncGenerator[AsyncClient, None]:
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    set_fleet(fleet)
    app = create_app()
    app.dependency_overrides[require_api_key] = lambda: None
    app.dependency_overrides[require_deploy_tool] = lambda: None
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:  # type: ignore[arg-type]
        yield c
    set_fleet(None)  # type: ignore[arg-type]


async def test_route_requires_an_explicit_scope(client: AsyncClient) -> None:
    r = await client.post("/api/fleet/deploy", json={})
    assert r.status_code == 422  # device_ids is required


async def test_route_rejects_unknown_devices_before_starting(client: AsyncClient) -> None:
    r = await client.post("/api/fleet/deploy", json={"device_ids": ["nope"]})
    assert r.status_code == 400 and "unknown" in r.text


async def test_route_rejects_a_malformed_release(client: AsyncClient) -> None:
    r = await client.post("/api/fleet/deploy", json={"device_ids": "all", "release": "main"})
    assert r.status_code == 422


async def test_route_needs_the_api_key() -> None:
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    set_fleet(fleet)
    app = create_app()
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:  # type: ignore[arg-type]
        r = await c.post("/api/fleet/deploy", json={"device_ids": "all"})
    set_fleet(None)  # type: ignore[arg-type]
    assert r.status_code in (401, 422)  # 422: missing X-API-Key header; 401: wrong key


def test_route_no_longer_compiles() -> None:
    src = (Path(dp.__file__).parent.parent / "api" / "routes_firmware.py").read_text(
        encoding="utf-8"
    )
    assert "compile_firmware" not in src and "generate_dfu_package" not in src
