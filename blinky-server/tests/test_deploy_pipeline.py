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
from blinky_server.paths import hosttools_dir

STABLE_VERSION = "b215-39df3912"


class FakeProtocol:
    def __init__(self, info: dict[str, Any] | Exception) -> None:
        self.info = info
        self.sent: list[str] = []

    async def send_command(self, command: str, timeout: float | None = None) -> str:
        self.sent.append(command)
        if isinstance(self.info, Exception):
            raise self.info
        if command in ("restore_runtime_settings", "save"):
            return "OK"
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
        self.device_type = device_id  # configured: a staging target must be (require_test_devices)
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
        self.staging_enabled = True  # the test box (--staging); staging deploys need it

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
            FakeDevice("porkpie_hat"),
            FakeDevice("ble_lamp", state=DeviceState.PRESENT),
            FakeDevice("stuck", state=DeviceState.DFU_RECOVERY),
            FakeDevice("gone", state=DeviceState.DISCONNECTED),
            FakeDevice("esp", platform="esp32s3"),
            FakeDevice("esp_crashed", state=DeviceState.ERROR, platform="esp32s3"),
            FakeDevice("odd", platform="unknown"),
        ]
    )


# ── Targets ─────────────────────────────────────────────────────────────


def test_all_means_connected_or_present_nrf52840_only() -> None:
    ids = [d.id for d in dp.resolve_targets(_fleet(), "all")]
    assert ids == ["porkpie_hat", "ble_lamp"]


def test_explicit_list_may_name_an_esp32s3_even_one_not_answering() -> None:
    """The box's ESP32-S3: esptool reaches its download mode whatever its firmware does."""
    ids = [d.id for d in dp.resolve_targets(_fleet(), ["porkpie_hat", "esp", "esp_crashed"])]
    assert ids == ["porkpie_hat", "esp", "esp_crashed"]


def test_explicit_list_may_name_a_device_in_its_bootloader() -> None:
    ids = [d.id for d in dp.resolve_targets(_fleet(), ["stuck", "porkpie_hat", "porkpie_hat"])]
    assert ids == ["stuck", "porkpie_hat"]  # de-duplicated, order kept


@pytest.mark.parametrize(
    ("ids", "match"),
    [
        (["typo"], "unknown"),
        (["gone"], "not flashable"),
        (["odd"], "not flashable"),
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
    kwargs: list[dict[str, Any]] = []

    def run(args: list[str], **kw: Any) -> subprocess.CompletedProcess[str]:
        calls.append(args)
        kwargs.append(kw)
        return subprocess.CompletedProcess(args, returncode, stdout, stderr)

    run.calls = calls  # type: ignore[attr-defined]
    run.kwargs = kwargs  # type: ignore[attr-defined]
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
    # blinky-server's own release client, run outside any firmware checkout (decoupling).
    assert args[1] == str(hosttools_dir() / "firmware_release.py")
    assert run.kwargs[0]["cwd"] == hosttools_dir()
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
    fleet.broadcast_result = {"broadcast": "OK", "porkpie_hat": "ERR"}
    with pytest.raises(DeployError, match="restore_runtime_settings did not land"):
        await dp.restore_and_save(fleet)
    assert fleet.broadcasts == ["restore_runtime_settings"]  # never saved half-restored state


# ── Post-deploy assertion ───────────────────────────────────────────────


async def test_assertion_passes_on_the_right_version() -> None:
    rows = await dp.assert_post_deploy(_fleet(), ["porkpie_hat", "ble_lamp"], STABLE_VERSION)
    assert rows["porkpie_hat"]["ok"] and rows["ble_lamp"]["ok"]


@pytest.mark.parametrize(
    ("device", "match"),
    [
        (
            FakeDevice(
                "porkpie_hat",
                info={"version": "b214-7a4664d6", "audioOverruns": 0, "audioSamplesLost": 0},
            ),
            "version",
        ),
        (
            FakeDevice("porkpie_hat", info={"version": STABLE_VERSION, "fps": 140.0}),
            "overrun counters",
        ),
        (FakeDevice("porkpie_hat", state=DeviceState.DFU_RECOVERY), "dfu_recovery"),
        (FakeDevice("porkpie_hat", info=TimeoutError("no reply")), "unreachable"),
    ],
)
async def test_assertion_fails_loudly(
    device: FakeDevice, match: str, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(dp, "JSON_INFO_RETRY_S", 0.0)
    with pytest.raises(DeployError) as e:
        await dp.assert_post_deploy(FakeFleet([device]), ["porkpie_hat"], STABLE_VERSION)
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
        device_ids=["porkpie_hat"],
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
    assert flashed == [(["porkpie_hat"], tmp_path / "firmware.hex")]
    assert fleet.recovery == (
        str(tmp_path / "firmware.hex"),
        ["porkpie_hat"],
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


# ── Staging (#349): the staging build of a commit, test devices only ────

STAGING_SHA = "ab" * 20
STAGING_VERSION = f"b215-dev-{STAGING_SHA[:8]}"


def _staging_run(tmp_path: Path, rel: dict[str, Any] | None = None) -> Any:
    """firmware_release.py as subprocess: porkpie_hat is the only test device; download-staging
    answers with ``rel``."""
    hex_path = tmp_path / "blinky-things-b215.hex"
    hex_path.write_text(":00000001FF\n")
    good = {
        "tag": f"staging-{STAGING_SHA[:8]}",
        "stable": False,
        "channel": "dev",
        "sha": STAGING_SHA,
        "hex": str(hex_path),
        "version": STAGING_VERSION,
    }
    calls: list[list[str]] = []

    def run(args: list[str], **_: Any) -> subprocess.CompletedProcess[str]:
        calls.append(args)
        assert args[2:5] == ["download-staging", "--sha", STAGING_SHA]
        return subprocess.CompletedProcess(args, 0, json.dumps({**good, **(rel or {})}), "")

    run.calls = calls  # type: ignore[attr-defined]
    return run


async def _staging_deploy(
    fleet: FakeFleet,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    *,
    device_ids: Any = ("porkpie_hat",),
    rel: dict[str, Any] | None = None,
) -> tuple[dict[str, Any], list[Any], list[Any]]:
    monkeypatch.setattr(dp.subprocess, "run", _staging_run(tmp_path, rel))
    for d in fleet.devices.values():
        if d.id == "porkpie_hat":
            d.protocol.info = {**d.protocol.info, "version": STAGING_VERSION}  # type: ignore[dict-item]
    flashed: list[Any] = []
    stored: list[Any] = []

    async def flash(_fleet: Any, ids: list[str], fw: Path, _job: Any) -> dict[str, Any]:
        flashed.append((ids, fw))
        return {"status": "ok", "message": "flashed"}

    def store(p: Path, version: str) -> Path:
        stored.append(version)
        return tmp_path / "staging.hex"

    result = await dp.run_deploy(
        fleet,
        FakeJob(),
        release_tag=None,
        device_ids=list(device_ids) if not isinstance(device_ids, str) else device_ids,
        download_dir=tmp_path,
        store_firmware=store,
        flash=flash,
        settle_s=0.0,
        staging_sha=STAGING_SHA,
    )
    return result, flashed, stored


async def test_staging_deploy_runs_every_step_and_never_arms_recovery(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    fleet = _fleet()
    result, flashed, stored = await _staging_deploy(fleet, monkeypatch, tmp_path)
    assert result["status"] == "ok" and result["version"] == STAGING_VERSION, result
    assert result["steps"]["store"]["flash_t"] > 0  # the staging suite's alert window opens here
    assert list(result["steps"]) == ["targets", "release", "store", "flash", "settings", "verify"]
    assert flashed == [(["porkpie_hat"], tmp_path / "staging.hex")] and stored == [STAGING_VERSION]
    assert fleet.recovery is None  # #251: only a stable image arms auto-recovery
    # Settings on the target only: nothing is broadcast to the rest of the fleet.
    assert fleet.broadcasts == []
    hat = fleet.devices["porkpie_hat"].protocol.sent
    assert [c for c in hat if c != "json info"] == ["restore_runtime_settings", "save"]
    assert all(
        c == "json info"
        for d in fleet.devices.values()
        if d.id != "porkpie_hat"
        for c in d.protocol.sent
    )


async def test_staging_deploy_refuses_unless_the_server_is_the_test_box(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    fleet = _fleet()
    fleet.staging_enabled = False
    result, flashed, stored = await _staging_deploy(fleet, monkeypatch, tmp_path)
    assert result["status"] == "error" and result["failed_step"] == "targets"
    assert "test box" in result["message"] and flashed == [] and stored == []


@pytest.mark.parametrize(
    ("device_ids", "match"),
    [
        (["unconfigured"], "unconfigured chips"),
        (["porkpie_hat", "unconfigured"], "unconfigured chips"),
        ("all", "explicit list of test devices"),
    ],
)
async def test_staging_deploy_refuses_unconfigured_chips_and_all(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, device_ids: Any, match: str
) -> None:
    fleet = _fleet()
    bare = FakeDevice("unconfigured")
    bare.device_type = None  # no device config uploaded yet
    fleet.devices[bare.id] = bare
    result, flashed, stored = await _staging_deploy(
        fleet, monkeypatch, tmp_path, device_ids=device_ids
    )
    assert result["status"] == "error" and result["failed_step"] == "targets"
    assert match in result["message"] and flashed == [] and stored == []


async def test_staging_deploy_takes_any_configured_chip_on_the_box(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    # No test-device list on the box: a chip configured as anything is a test chip there.
    fleet = _fleet()
    grid = FakeDevice("testbox_grid")
    fleet.devices[grid.id] = grid
    grid.protocol.info = {**grid.protocol.info, "version": STAGING_VERSION}  # type: ignore[dict-item]
    result, flashed, _ = await _staging_deploy(
        fleet, monkeypatch, tmp_path, device_ids=["testbox_grid"]
    )
    assert result["status"] == "ok", result
    assert flashed == [(["testbox_grid"], tmp_path / "staging.hex")]


@pytest.mark.parametrize(
    "rel",
    [{"channel": "release"}, {"stable": True}, {"sha": "cd" * 20}],
)
async def test_staging_deploy_refuses_anything_but_that_commits_build(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path, rel: dict[str, Any]
) -> None:
    fleet = _fleet()
    result, flashed, _ = await _staging_deploy(fleet, monkeypatch, tmp_path, rel=rel)
    assert result["status"] == "error" and result["failed_step"] == "release"
    assert flashed == [] and fleet.broadcasts == []


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


async def test_route_refuses_a_second_deploy_while_one_runs(
    client: AsyncClient, monkeypatch: pytest.MonkeyPatch
) -> None:
    from blinky_server.api import routes_firmware

    async def active() -> bool:
        return True  # a deploy hasn't finished (running, or held for a restart)

    monkeypatch.setattr(routes_firmware, "_deploy_active", active)
    r = await client.post("/api/fleet/deploy", json={"device_ids": "all"})
    assert r.status_code == 409


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


async def test_staging_route_is_off_unless_the_server_runs_with_staging(
    client: AsyncClient,
) -> None:
    r = await client.post(
        "/api/fleet/deploy-staging", json={"sha": STAGING_SHA, "device_ids": ["porkpie_hat"]}
    )
    assert r.status_code == 403 and "--staging" in r.text


async def test_staging_route_validates_its_request(client: AsyncClient) -> None:
    from blinky_server.api.deps import get_fleet

    get_fleet().staging_enabled = True
    for body in (
        {"sha": STAGING_SHA[:12], "device_ids": ["porkpie_hat"]},  # not a full sha
        {"sha": STAGING_SHA, "device_ids": []},  # no scope
        {"sha": STAGING_SHA, "device_ids": "all"},  # never "all"
    ):
        r = await client.post("/api/fleet/deploy-staging", json=body)
        assert r.status_code == 422, body
    r = await client.post(
        "/api/fleet/deploy-staging", json={"sha": STAGING_SHA, "device_ids": ["nope"]}
    )
    assert r.status_code == 400 and "unknown" in r.text


# ── ESP32-S3 targets: the build's ESP32-S3 image, armed for them alone ──


def _s3_bytes() -> bytes:
    img = bytearray(b"\x22" * 4096)
    img[0] = 0xE9
    img[12:14] = b"\x09\x00"  # chip id: ESP32-S3
    return bytes(img)


async def test_mixed_deploy_stores_and_links_both_images_and_arms_each_platform(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    from blinky_server.firmware import images

    hex_path = tmp_path / "blinky-things-b215.hex"
    hex_path.write_bytes(b":00000001FF\n" * 200)
    s3_path = tmp_path / "blinky-things-b215-esp32s3.bin"
    s3_path.write_bytes(_s3_bytes())
    out = {"tag": "b215", "stable": True, "hex": str(hex_path), "uf2": "x",
           "esp32s3": str(s3_path), "version": STABLE_VERSION}  # fmt: skip
    monkeypatch.setattr(dp.subprocess, "run", _fake_run(json.dumps(out)))
    fleet = _fleet()
    flashed: list[Any] = []

    async def flash(_fleet: Any, ids: list[str], fw: Path, _job: Any) -> dict[str, Any]:
        flashed.append((ids, fw))
        return {"status": "ok", "message": "flashed"}

    def store(p: Path, tag: str) -> Path:
        return images.store(p.read_bytes(), version=tag, stable=True, source="deploy").path

    result = await dp.run_deploy(
        fleet,
        FakeJob(),
        release_tag=None,
        device_ids=["porkpie_hat", "esp"],
        download_dir=tmp_path,
        store_firmware=store,
        flash=flash,
        settle_s=0.0,
    )
    assert result["status"] == "ok", result
    named = Path(result["steps"]["store"]["firmware_path"])
    assert named.suffix == ".hex" and flashed == [(["porkpie_hat", "esp"], named)]
    s3 = images.for_platform(named, "esp32s3")
    assert s3 is not None and s3.path.suffix == ".bin"
    assert fleet.recovery == (str(s3.path), ["esp"])  # the last arm: the S3's own


async def test_esp32_targets_and_no_esp32_image_is_refused_before_flashing(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    fleet = _fleet()
    hex_path = tmp_path / "blinky-things-b215.hex"
    hex_path.write_text(":00000001FF\n")
    out = {"tag": "b215", "stable": True, "hex": str(hex_path), "uf2": "x",
           "version": STABLE_VERSION}  # fmt: skip
    monkeypatch.setattr(dp.subprocess, "run", _fake_run(json.dumps(out)))
    flashed: list[Any] = []

    async def flash(_fleet: Any, ids: list[str], fw: Path, _job: Any) -> dict[str, Any]:
        flashed.append(ids)
        return {"status": "ok"}

    result = await dp.run_deploy(
        fleet,
        FakeJob(),
        release_tag=None,
        device_ids=["esp"],
        download_dir=tmp_path,
        store_firmware=lambda p, tag: tmp_path / "x.hex",
        flash=flash,
        settle_s=0.0,
    )
    assert result["failed_step"] == "release" and "no ESP32-S3 image" in result["message"]
    assert flashed == [] and fleet.recovery is None
