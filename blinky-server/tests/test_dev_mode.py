"""The box's dev loop (docs/DEV_LOOP.md): box modes, the build-service client, dev deploys
through the staging pipeline, and the dev endpoints' refusals."""

from __future__ import annotations

import hashlib
import io
import json
import tarfile
import threading
import time
from collections.abc import AsyncGenerator, Iterator
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from blinky_server.api import routes_dev, routes_firmware, routes_testing
from blinky_server.api.app import create_app
from blinky_server.api.deps import require_api_key, set_fleet
from blinky_server.firmware import build_service
from blinky_server.firmware import deploy_pipeline as dp
from blinky_server.testing import box_mode

from .test_deploy_pipeline import FakeDevice, FakeFleet, FakeJob

SHA = "cd" * 20
VERSION = f"b220-dev-{SHA[:8]}"
HEX = b":00000001FF\n" * 100
UF2 = b"UF2" * 100


def tar_gz(files: dict[str, bytes], extra: list[tarfile.TarInfo] | None = None) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, data in files.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        for info in extra or []:
            tar.addfile(info)
    return buf.getvalue()


def good_files(**over: Any) -> dict[str, bytes]:
    files = {"blinky-things-b220.hex": HEX, "blinky-things-b220.uf2": UF2}
    manifest = {
        "build": 220,
        "version": VERSION,
        "channel": "dev",
        "git_sha": SHA,
        "files": {n: hashlib.sha256(b).hexdigest() for n, b in files.items()},
        **over,
    }
    return {"manifest.json": json.dumps(manifest).encode(), **files}


class FakeBuildService:
    """The build service's contract (DEV_LOOP.md): GET /v1/builds/{id} and its package."""

    def __init__(self) -> None:
        self.builds: dict[str, dict[str, Any]] = {}
        self.packages: dict[str, bytes] = {}
        self.auth: list[str | None] = []
        svc = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *a: Any) -> None:
                pass

            def do_GET(self) -> None:
                svc.auth.append(self.headers.get("Authorization"))
                if self.headers.get("Authorization") != "Bearer tok":
                    self.send_response(401)
                    self.end_headers()
                    return
                parts = self.path.strip("/").split("/")
                body: bytes | None = None
                if len(parts) == 3 and parts[2] in svc.builds:
                    body = json.dumps(svc.builds[parts[2]]).encode()
                elif len(parts) == 4 and parts[3] == "package" and parts[2] in svc.packages:
                    body = svc.packages[parts[2]]
                if body is None:
                    self.send_response(404)
                    self.end_headers()
                    return
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def add(self, build_id: str, package: bytes, **rec: Any) -> None:
        self.builds[build_id] = {
            "id": build_id,
            "state": "done",
            "sha": SHA,
            "version": VERSION,
            "error": None,
            "log_tail": "",
            **rec,
        }
        self.packages[build_id] = package


@pytest.fixture
def service(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> Iterator[FakeBuildService]:
    svc = FakeBuildService()
    env = tmp_path / "build.env"
    env.write_text(
        f"# the box's private config\nBLINKY_BUILD_URL={svc.url}/\nexport BLINKY_BUILD_TOKEN='tok'\n"
    )
    monkeypatch.setattr(build_service, "ENV_FILE", env)
    monkeypatch.delenv("BLINKY_BUILD_URL", raising=False)
    monkeypatch.delenv("BLINKY_BUILD_TOKEN", raising=False)
    yield svc
    svc.server.shutdown()


# -- the build-service client ------------------------------------------------------------------


def test_config_comes_from_the_private_env_file_and_has_no_default(
    service: FakeBuildService, monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    assert build_service.config() == (service.url, "tok")
    monkeypatch.setattr(build_service, "ENV_FILE", tmp_path / "missing.env")
    assert build_service.config() is None  # no default address: the repo names no host
    monkeypatch.setenv("BLINKY_BUILD_URL", "http://example.invalid")
    monkeypatch.setenv("BLINKY_BUILD_TOKEN", "t2")
    assert build_service.config() == ("http://example.invalid", "t2")


def test_a_good_package_is_fetched_and_verified(service: FakeBuildService, tmp_path: Path) -> None:
    service.add("b1", tar_gz(good_files()))
    rel = build_service.fetch("b1", tmp_path / "dl")
    assert rel["version"] == VERSION and rel["sha"] == SHA and rel["stable"] is False
    assert Path(rel["hex"]).read_bytes() == HEX and rel["build_id"] == "b1"
    assert set(service.auth) == {"Bearer tok"}


def test_a_package_with_a_dot_slash_prefix_is_fine(
    service: FakeBuildService, tmp_path: Path
) -> None:
    dot = tarfile.TarInfo(".")
    dot.type = tarfile.DIRTYPE
    service.add("b1", tar_gz({f"./{n}": b for n, b in good_files().items()}, [dot]))
    assert build_service.fetch("b1", tmp_path / "dl")["version"] == VERSION


def _link(name: str, target: str) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.type = tarfile.SYMTYPE
    info.linkname = target
    return info


@pytest.mark.parametrize(
    ("package", "rec", "why"),
    [
        (tar_gz(good_files()), {"state": "building"}, "not done"),
        (tar_gz(good_files()), {"state": "failed", "error": "compile"}, "compile"),
        (tar_gz(good_files()), {"sha": "abc"}, "no full commit sha"),
        (tar_gz(good_files()), {"version": "b221-dev-cdcdcdcd"}, "the service says"),
        (tar_gz(good_files(channel="release")), {}, "not the dev build"),
        (tar_gz(good_files(git_sha="ab" * 20)), {}, "not the dev build"),
        (tar_gz({**good_files(), "blinky-things-b220.uf2": b"tampered"}), {}, "sha256 mismatch"),
        (tar_gz({**good_files(), "../evil.hex": b"x"}), {}, "refusing it"),
        (tar_gz({**good_files(), "sub/x.hex": b"x"}), {}, "refusing it"),
        (tar_gz({**good_files(), "run.sh": b"x"}), {}, "refusing it"),
        (tar_gz(good_files(), [_link("evil.hex", "/etc/passwd")]), {}, "refusing it"),
        (tar_gz({**good_files(), "extra.hex": b"x"}), {}, "not in the manifest"),
        (b"not a tarball", {}, "readable .tar.gz"),
    ],
)
def test_anything_but_a_verified_dev_package_is_refused(
    service: FakeBuildService, tmp_path: Path, package: bytes, rec: dict[str, Any], why: str
) -> None:
    service.add("b1", package, **rec)
    with pytest.raises(build_service.BuildServiceError, match=why):
        build_service.fetch("b1", tmp_path / "dl")
    assert not (tmp_path / "evil.hex").exists() and not (tmp_path / "dl" / "evil.hex").exists()


def test_a_bad_build_id_never_reaches_the_service(service: FakeBuildService) -> None:
    with pytest.raises(build_service.BuildServiceError, match="not a build id"):
        build_service.build("../../etc")
    assert service.auth == []


# -- the dev deploy: the staging pipeline with a build-service package -------------------------


async def test_a_dev_deploy_runs_every_staging_step_and_never_arms_recovery(
    service: FakeBuildService, tmp_path: Path
) -> None:
    service.add("b1", tar_gz(good_files()))
    fleet = FakeFleet([FakeDevice("grid"), FakeDevice("other")])
    grid = fleet.devices["grid"]
    grid.protocol.info = {**grid.protocol.info, "version": VERSION}  # type: ignore[dict-item]
    flashed: list[Any] = []
    stored: list[str] = []

    async def flash(_f: Any, ids: list[str], fw: Path, _j: Any) -> dict[str, Any]:
        flashed.append((ids, fw))
        return {"status": "ok"}

    def store(p: Path, version: str) -> Path:
        assert p.read_bytes() == HEX
        stored.append(version)
        return tmp_path / "dev.hex"

    result = await dp.run_deploy(
        fleet,
        FakeJob(),
        release_tag=None,
        device_ids=["grid"],
        download_dir=tmp_path / "dl",
        store_firmware=store,
        flash=flash,
        settle_s=0.0,
        dev_build="b1",
    )
    assert result["status"] == "ok", result
    assert result["version"] == VERSION and result["sha"] == SHA and result["build_id"] == "b1"
    assert list(result["steps"]) == ["targets", "release", "store", "flash", "settings", "verify"]
    assert flashed == [(["grid"], tmp_path / "dev.hex")] and stored == [VERSION]
    assert fleet.recovery is None and fleet.broadcasts == []  # #251; settings on the target only
    assert fleet.devices["other"].protocol.sent == []


async def test_a_dev_deploy_refuses_off_the_box_or_all(
    service: FakeBuildService, tmp_path: Path
) -> None:
    service.add("b1", tar_gz(good_files()))
    for staging_enabled, ids in ((False, ["grid"]), (True, "all")):
        fleet = FakeFleet([FakeDevice("grid")])
        fleet.staging_enabled = staging_enabled
        result = await dp.run_deploy(
            fleet,
            FakeJob(),
            release_tag=None,
            device_ids=ids,
            download_dir=tmp_path,
            store_firmware=lambda p, v: p,
            flash=None,  # type: ignore[arg-type]
            dev_build="b1",
        )
        assert result["status"] == "error" and result["failed_step"] == "targets", result


async def test_a_bad_package_stops_the_deploy_before_anything_is_stored(
    service: FakeBuildService, tmp_path: Path
) -> None:
    service.add("b1", tar_gz({**good_files(), "blinky-things-b220.hex": b"x" * 2000}))
    fleet = FakeFleet([FakeDevice("grid")])
    result = await dp.run_deploy(
        fleet,
        FakeJob(),
        release_tag=None,
        device_ids=["grid"],
        download_dir=tmp_path,
        store_firmware=lambda p, v: pytest.fail("stored"),  # type: ignore[arg-type,return-value]
        flash=None,  # type: ignore[arg-type]
        dev_build="b1",
    )
    assert result["failed_step"] == "release" and "sha256 mismatch" in result["message"]


# -- the box's mode ----------------------------------------------------------------------------


@pytest.fixture
def suites(monkeypatch: pytest.MonkeyPatch) -> list[str]:
    """The staging suites running now, as box_mode sees them."""
    running: list[str] = []

    async def active() -> list[str]:
        return list(running)

    monkeypatch.setattr(box_mode, "_active_suites", active)
    return running


async def test_the_default_is_queue_and_dev_has_no_expiry(suites: list[str]) -> None:
    assert (await box_mode.current())["mode"] == "queue" and not box_mode.holds_queue()
    st = await box_mode.set_mode("dev", "trying the ACF fix")
    assert st["mode"] == "dev" and st["note"] == "trying the ACF fix" and box_mode.in_dev()
    box_mode._cache = None  # a restart: read back from the data dir
    assert (await box_mode.current())["mode"] == "dev" and box_mode.holds_queue()
    st = await box_mode.set_mode("queue")
    assert st["mode"] == "queue" and not box_mode.holds_queue()


async def test_a_switch_waits_for_the_running_suite(suites: list[str]) -> None:
    suites.append("suite-1")
    st = await box_mode.set_mode("dev")
    assert st["mode"] == "queue" and st["switching"]["waiting_for"] == ["suite-1"]
    assert box_mode.holds_queue() and not box_mode.in_dev()  # runners hold; no dev jobs yet
    assert (await box_mode.current())["mode"] == "queue"  # still running
    suites.clear()
    st = await box_mode.current()
    assert st["mode"] == "dev" and st["switching"] is None


async def test_switching_back_to_queue_cancels_a_pending_switch(suites: list[str]) -> None:
    suites.append("suite-1")
    await box_mode.set_mode("dev")
    st = await box_mode.set_mode("queue")
    suites.clear()
    assert (await box_mode.current())["mode"] == "queue" and st["switching"] is None


async def test_preempt_cancels_the_suite_and_queues_its_commit(
    suites: list[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    from dbos import DBOS

    cancelled: list[str] = []

    async def cancel(job: str) -> None:
        cancelled.append(job)

    async def sha(job: str) -> tuple[str, str]:
        return SHA, "staging"

    monkeypatch.setattr(DBOS, "cancel_workflow_async", cancel)
    monkeypatch.setattr(box_mode, "_suite_of", sha)
    suites.append("suite-1")
    st = await box_mode.set_mode("dev", preempt=True)
    assert cancelled == ["suite-1"] and st["mode"] == "dev"
    assert st["preempted"][0]["job"] == "suite-1" and st["preempted"][0]["sha"] == SHA
    assert [(w["runner"], w["sha"], w["preempted"]) for w in st["waiting"]] == [
        ("staging", SHA, True)
    ]


def test_the_queue_is_each_runners_list_oldest_first() -> None:
    box_mode.set_waiting("staging", [{"sha": "a" * 40, "since": 20.0}])
    st = box_mode.set_waiting("pr", [{"sha": "b" * 40, "since": 10.0}])
    assert [w["sha"][0] for w in st["waiting"]] == ["b", "a"]
    st = box_mode.set_waiting("pr", [])
    assert [w["runner"] for w in st["waiting"]] == ["staging"]


def test_a_corrupt_state_file_is_queue_mode(tmp_path: Path) -> None:
    box_mode._path().write_text("{not json")
    box_mode._cache = None
    assert box_mode.snapshot()["mode"] == "queue"


def test_the_metrics_show_the_mode_and_the_work_waiting_behind_it() -> None:
    from blinky_server import telemetry

    st = box_mode._state()
    st.update(mode="dev", since=time.time())
    box_mode._save(st)
    box_mode.set_waiting("staging", [{"sha": "a" * 40, "since": time.time() - 600}])
    body = telemetry.metrics_body()[0].decode()
    assert 'blinky_box_mode{mode="dev"} 1.0' in body
    assert 'blinky_box_mode{mode="queue"} 0.0' in body
    assert "blinky_hw_queue_depth 1.0" in body
    oldest = next(ln for ln in body.splitlines() if ln.startswith("blinky_hw_queue_oldest_seconds"))
    assert 599 <= float(oldest.split()[1]) < 700
    box_mode.set_waiting("staging", [])
    body = telemetry.metrics_body()[0].decode()
    assert "blinky_hw_queue_depth 0.0" in body and "blinky_hw_queue_oldest_seconds 0.0" in body


# -- the routes --------------------------------------------------------------------------------


@pytest_asyncio.fixture
async def box(suites: list[str]) -> AsyncGenerator[tuple[AsyncClient, FakeFleet], None]:
    fleet = FakeFleet([FakeDevice("grid")])
    fleet.devices["grid"].last_info = {"millis": 3_600_000}  # type: ignore[attr-defined]
    fleet.devices["grid"].last_info_at = time.monotonic()  # type: ignore[attr-defined]
    set_fleet(fleet)  # type: ignore[arg-type]
    app = create_app()
    app.dependency_overrides[require_api_key] = lambda: None
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:  # type: ignore[arg-type]
        yield c, fleet
    set_fleet(None)  # type: ignore[arg-type]


@pytest.fixture
def submitted(monkeypatch: pytest.MonkeyPatch) -> list[tuple[Any, ...]]:
    got: list[tuple[Any, ...]] = []

    async def fleet_submit(fn: Any, *args: Any) -> str:
        got.append((fn.__name__, *args))
        return "job-1"

    async def test_submit(fn: Any, *args: Any) -> dict[str, Any]:
        got.append((fn.__name__, *args))
        return {"job_id": "job-2", "status": "submitted"}

    async def inactive() -> bool:
        return False

    monkeypatch.setattr(routes_firmware, "_submit", fleet_submit)
    monkeypatch.setattr(routes_firmware, "_deploy_active", inactive)
    monkeypatch.setattr(routes_testing, "_submit", test_submit)
    return got


async def test_mode_routes(box: tuple[AsyncClient, FakeFleet]) -> None:
    c, fleet = box
    r = await c.get("/api/box/mode")
    assert r.json()["mode"] == "queue" and r.json()["summary"] == "box in queue mode (0 waiting)"
    r = await c.put("/api/box/mode", json={"mode": "dev", "note": "acf"})
    assert r.status_code == 200 and r.json()["mode"] == "dev"
    assert r.json()["summary"].startswith("box in dev mode since")
    assert (await c.put("/api/box/mode", json={"mode": "later"})).status_code == 422
    r = await c.put("/api/box/queue/staging", json={"waiting": [{"sha": SHA, "since": 1.0}]})
    assert (
        r.status_code == 200 and (await c.get("/api/box/mode")).json()["waiting"][0]["sha"] == SHA
    )
    assert (await c.put("/api/box/queue/Bad Name", json={"waiting": []})).status_code in (404, 422)
    fleet.staging_enabled = False
    await c.put("/api/box/mode", json={"mode": "queue"})
    assert (await c.put("/api/box/mode", json={"mode": "dev"})).status_code == 403


async def test_mode_changes_need_the_api_key() -> None:
    set_fleet(FakeFleet([]))  # type: ignore[arg-type]
    app = create_app()
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:  # type: ignore[arg-type]
        assert (await c.put("/api/box/mode", json={"mode": "dev"})).status_code in (401, 422)
        r = await c.put("/api/box/queue/staging", json={"waiting": []})
        assert r.status_code in (401, 422)
        assert (await c.get("/api/box/mode")).status_code == 200  # reading needs none
    set_fleet(None)  # type: ignore[arg-type]


async def test_dev_endpoints_answer_409_outside_dev_mode(
    box: tuple[AsyncClient, FakeFleet],
    service: FakeBuildService,
    suites: list[str],
    submitted: list[Any],
) -> None:
    c, _ = box
    deploy = {"build_id": "b1", "device_ids": ["grid"]}
    run = {"device_ids": ["grid"], "checks": [{"kind": "info"}]}
    for r in (await c.post("/api/dev/deploy", json=deploy), await c.post("/api/dev/run", json=run)):
        assert r.status_code == 409 and "queue mode" in r.json()["detail"]
    suites.append("suite-1")
    await c.put("/api/box/mode", json={"mode": "dev"})
    r = await c.post("/api/dev/deploy", json=deploy)
    assert r.status_code == 409 and "switching" in r.json()["detail"]
    assert submitted == []


async def test_the_staging_suite_route_refuses_in_dev_mode(
    box: tuple[AsyncClient, FakeFleet], submitted: list[Any]
) -> None:
    c, _ = box
    await c.put("/api/box/mode", json={"mode": "dev"})
    body = {"sha": SHA, "device_ids": ["grid"], "track_dir": "/tmp"}
    r = await c.post("/api/test/staging", json=body)
    assert r.status_code == 409 and "dev mode" in r.json()["detail"] and submitted == []


async def test_every_route_that_flashes_the_box_keeps_the_60_s_rule(
    box: tuple[AsyncClient, FakeFleet], submitted: list[Any]
) -> None:
    """The staging suite and a staging deploy start with a flash, like a dev deploy and the
    hardware checks: a chip that reset 20 s ago (a dev flash just before the box went back to
    queue mode, or hardware checks cancelled mid-flash) is refused with how long to wait, and
    the runner tries again next pass (409)."""
    c, fleet = box
    fleet.devices["grid"].last_info = {"millis": 20_000}  # type: ignore[attr-defined]
    calls = [
        ("/api/test/staging", {"sha": SHA, "device_ids": ["grid"], "track_dir": "/tmp"}),
        ("/api/fleet/deploy-staging", {"sha": SHA, "device_ids": ["grid"]}),
        ("/api/test/hardware-checks", {"sha": SHA, "device_ids": ["grid"], "source": "staging"}),
    ]
    for path, body in calls:
        r = await c.post(path, json=body)
        assert r.status_code == 409, (path, r.text)
        assert "60 s between resets" in r.json()["detail"]
        assert 39 <= int(r.headers["Retry-After"]) <= 41
    assert submitted == []
    fleet.devices["grid"].last_info = {"millis": 3_600_000}  # type: ignore[attr-defined]
    for path, body in calls:
        assert (await c.post(path, json=body)).status_code == 200, path
    assert len(submitted) == 3


async def test_a_dev_deploy_in_dev_mode(
    box: tuple[AsyncClient, FakeFleet],
    service: FakeBuildService,
    submitted: list[Any],
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    c, fleet = box
    await c.put("/api/box/mode", json={"mode": "dev"})
    r = await c.post("/api/dev/deploy", json={"build_id": "b1", "device_ids": ["grid"]})
    assert r.status_code == 200 and r.json()["job_id"] == "job-1"
    assert submitted[0][:3] == ("fleet_deploy_dev", "b1", ["grid"])
    assert (
        await c.post("/api/dev/deploy", json={"build_id": "../x", "device_ids": ["grid"]})
    ).status_code == 422
    r = await c.post("/api/dev/deploy", json={"build_id": "b1", "device_ids": ["nope"]})
    assert r.status_code == 400
    # The 60 s rule: a chip that reset 20 s ago is refused with how long to wait.
    fleet.devices["grid"].last_info = {"millis": 20_000}  # type: ignore[attr-defined]
    r = await c.post("/api/dev/deploy", json={"build_id": "b1", "device_ids": ["grid"]})
    assert r.status_code == 409 and "60 s between resets" in r.json()["detail"]
    assert 39 <= int(r.headers["Retry-After"]) <= 41
    monkeypatch.setattr(build_service, "ENV_FILE", tmp_path / "none.env")
    r = await c.post("/api/dev/deploy", json={"build_id": "b1", "device_ids": ["grid"]})
    assert r.status_code == 503 and "BLINKY_BUILD_URL" in r.json()["detail"]
    assert len(submitted) == 1


async def test_a_dev_run_in_dev_mode(
    box: tuple[AsyncClient, FakeFleet],
    submitted: list[Any],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    c, _ = box
    await c.put("/api/box/mode", json={"mode": "dev"})
    monkeypatch.setattr(routes_dev, "default_track_dir", lambda: tmp_path)
    monkeypatch.setattr(
        "blinky_scoring.track_discovery.discover_tracks",
        lambda d: [{"name": "techno"}, {"name": "dnb"}],
    )
    checks = [
        {"kind": "info"},
        {"kind": "commands", "commands": ["set odfgate 0.3", "leds status"]},
        {"kind": "health", "minutes": 2},
        {"kind": "validate", "tracks": ["techno"]},
        {"kind": "pace"},
    ]
    r = await c.post("/api/dev/run", json={"device_ids": ["grid"], "checks": checks})
    assert r.status_code == 200, r.text
    name, req = submitted[0]
    assert name == "dev_run" and req["track_dir"] == str(tmp_path)
    assert [ch["kind"] for ch in req["checks"]] == [
        "info",
        "commands",
        "health",
        "validate",
        "pace",
    ]
    bad = [
        ({"kind": "validate"}, 422),  # no tracks
        ({"kind": "commands"}, 422),  # no commands
        ({"kind": "health", "minutes": 0}, 422),
        ({"kind": "camera"}, 422),
        ({"kind": "validate", "tracks": ["missing"]}, 400),
        ({"kind": "commands", "commands": ["reboot"]}, 400),  # no resets from a run
        ({"kind": "commands", "commands": ["device upload {}"]}, 400),  # no config writes
        ({"kind": "commands", "commands": ["gen fire\nreboot"]}, 400),  # one line each
        ({"kind": "commands", "commands": ["save"]}, 400),  # runtime-only: no settings write
        ({"kind": "commands", "commands": ["bootloader"]}, 400),  # a reset
        ({"kind": "commands", "commands": ["ota commit"]}, 400),
    ]
    for check, code in bad:
        r = await c.post("/api/dev/run", json={"device_ids": ["grid"], "checks": [check]})
        assert r.status_code == code, (check, r.text)
    r = await c.post(
        "/api/dev/run",
        json={"device_ids": ["grid"], "checks": [{"kind": "info"}], "track_dir": "/etc"},
    )
    assert r.status_code == 400
    assert len(submitted) == 1


def test_reset_wait_reads_the_uptime_since_the_last_json_info() -> None:
    d = FakeDevice("grid")
    assert routes_dev.reset_wait(d) == 0.0  # unknown: the flash path has its own guards
    d.last_info, d.last_info_at = {"millis": 30_000}, 100.0  # type: ignore[attr-defined]
    assert routes_dev.reset_wait(d, now=110.0) == pytest.approx(20.0)
    assert routes_dev.reset_wait(d, now=200.0) == 0.0


def test_the_verdict_gates_on_health_and_asserts_only() -> None:
    from blinky_server.testing.dev_run import timers, verdict

    report: dict[str, Any] = {
        "checks": [
            {"kind": "health", "devices": {"g": {"fps": {"pass": True}}}},
            {"kind": "pace", "devices": {"g": {"beatpace": {"pass": False}}}},  # a score
        ],
        "final": {"g": {"no_reboot": {"pass": True}, "alerts": {"pass": True}}},
    }
    assert verdict(report) is True
    report["final"]["g"]["alerts"]["pass"] = False
    assert verdict(report) is False
    report["final"]["g"]["alerts"]["pass"] = True
    report["checks"][0]["devices"]["g"]["fps"]["pass"] = False
    assert verdict(report) is False
    info = {
        "version": "x",
        "acfUs": 1,
        "acfStageMaxUs": [2],
        "maxFrameMs": 3,
        "heapPeak": 4,
        "millis": 5,
        "device": {},
    }
    assert timers(info) == {"acfUs": 1, "acfStageMaxUs": [2], "maxFrameMs": 3, "heapPeak": 4}


def test_a_package_of_too_many_files_or_bytes_is_refused(
    service: FakeBuildService, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    many = {**good_files(), **{f"x{i}.hex": b"x" for i in range(20)}}
    service.add("b1", tar_gz(many))
    with pytest.raises(build_service.BuildServiceError, match="files: refusing it"):
        build_service.fetch("b1", tmp_path / "dl")
    monkeypatch.setattr(build_service, "MAX_PACKAGE_BYTES", 1000)  # under the hex + UF2 total
    service.add("b2", tar_gz(good_files()))
    with pytest.raises(build_service.BuildServiceError, match="bytes or"):
        build_service.fetch("b2", tmp_path / "dl")


async def test_a_switch_called_off_while_its_suite_check_runs_stays_off(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Review: GET /api/box/mode awaits the suite check; a PUT queue meanwhile must win."""
    running = ["suite-1"]

    async def active() -> list[str]:
        if not running:  # the GET's check: a PUT queue lands while it awaits
            await box_mode.set_mode("queue")
        return list(running)

    monkeypatch.setattr(box_mode, "_active_suites", active)
    await box_mode.set_mode("dev")
    running.clear()
    st = await box_mode.current()
    assert st["mode"] == "queue" and st["switching"] is None


async def test_a_dev_deploy_rechecks_the_mode_under_the_deploy_lock(
    box: tuple[AsyncClient, FakeFleet], service: FakeBuildService, submitted: list[Any]
) -> None:
    """Review: a switch to queue that holds the lock while the deploy waits for it wins."""
    import asyncio

    c, _ = box
    await c.put("/api/box/mode", json={"mode": "dev"})
    async with routes_firmware._deploy_lock:
        req = asyncio.create_task(
            c.post("/api/dev/deploy", json={"build_id": "b1", "device_ids": ["grid"]})
        )
        await asyncio.sleep(0.2)  # the request is past its first check, waiting for the lock
        await box_mode.set_mode("queue")
    r = await req
    assert r.status_code == 409 and "queue mode" in r.json()["detail"] and submitted == []


async def test_the_info_check_waits_out_a_metrics_window(monkeypatch: pytest.MonkeyPatch) -> None:
    """A dev run starts right after a deploy (a reboot, then the settings restore + save): the
    info check must read a 5 s metrics window that began after it."""
    from blinky_server.testing import dev_run

    events: list[Any] = []

    async def fake_sleep(s: float) -> None:
        events.append(("sleep", s))

    async def fake_info(device_id: str) -> dict[str, Any]:
        events.append(("info", device_id))
        return {"version": "b1-dev-x", "maxFrameMs": 15}

    monkeypatch.setattr(dev_run.asyncio, "sleep", fake_sleep)
    monkeypatch.setattr(dev_run.staging, "_info", fake_info)
    fn = getattr(dev_run.info, "__wrapped__", dev_run.info)
    out = await fn(["grid"])
    assert events[0] == ("sleep", dev_run.INFO_SETTLE_S) and dev_run.INFO_SETTLE_S > 5
    assert events[1] == ("info", "grid") and out["grid"]["version"] == "b1-dev-x"


# -- the PR hardware queue (docs/DEV_LOOP.md, phase 2) ----------------------------------------


async def test_preempt_queues_a_pr_head_under_the_pr_runner(
    suites: list[str], monkeypatch: pytest.MonkeyPatch
) -> None:
    from dbos import DBOS

    async def cancel(job: str) -> None:
        pass

    async def of(job: str) -> tuple[str, str]:
        return SHA, "pr"

    monkeypatch.setattr(DBOS, "cancel_workflow_async", cancel)
    monkeypatch.setattr(box_mode, "_suite_of", of)
    suites.append("smoke-1")
    st = await box_mode.set_mode("dev", preempt=True)
    assert st["preempted"][0]["job"] == "smoke-1"
    assert [(w["runner"], w["sha"]) for w in st["waiting"]] == [("pr", SHA)]


def test_each_waiting_item_carries_when_its_runner_last_reported() -> None:
    box_mode._reported_at.clear()
    st = box_mode.set_waiting("pr", [{"sha": "b" * 40, "since": 10.0, "ready": True}])
    (w,) = st["waiting"]
    assert w["ready"] is True and time.time() - w["reported_at"] < 5
    box_mode._reported_at.clear()  # a restart: unknown until the runner's next pass
    assert "reported_at" not in box_mode.snapshot()["waiting"][0]


async def test_the_queue_route_keeps_the_ready_flag(box: tuple[AsyncClient, FakeFleet]) -> None:
    c, _ = box
    item = {"sha": SHA, "since": 1.0, "what": "PR #5", "ready": True}
    await c.put("/api/box/queue/pr", json={"waiting": [item]})
    (w,) = (await c.get("/api/box/mode")).json()["waiting"]
    assert w["ready"] is True and w["what"] == "PR #5" and w["runner"] == "pr"


async def test_hardware_checks_in_queue_mode(
    box: tuple[AsyncClient, FakeFleet],
    service: FakeBuildService,
    submitted: list[Any],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    c, fleet = box
    service.add("b1", b"")
    body = {"sha": SHA, "build_id": "b1", "device_ids": ["grid"], "pr": 5, "source": "dev_build"}
    r = await c.post("/api/test/hardware-checks", json=body)
    assert r.status_code == 200, r.text
    name, req = submitted[0]
    assert name == "hardware_checks" and req["runner"] == "pr" and req["pr"] == 5
    assert req["source"] == {"kind": "dev_build", "sha": SHA, "build_id": "b1"}
    # The staging runner's: staging's artifact, no build service involved.
    r = await c.post(
        "/api/test/hardware-checks", json={"sha": SHA, "device_ids": ["grid"], "source": "staging"}
    )
    assert r.status_code == 200, r.text
    assert submitted[1][1]["source"] == {"kind": "staging", "sha": SHA}
    assert submitted[1][1]["runner"] == "staging"
    service.add("b2", b"", sha="ef" * 20)
    service.add("b3", b"", state="building")
    bad = [
        ({"build_id": "b2"}, 400),  # a build of another commit
        ({"build_id": "b3"}, 409),  # not built yet
        ({"build_id": "nope"}, 410),  # the service doesn't know it (pruned): rebuild
        ({"device_ids": ["nope"]}, 400),
        ({"build_id": "../x"}, 422),
        ({"build_id": None}, 422),  # a dev build names its build
        ({"source": "release"}, 422),
    ]
    for over, code in bad:
        r = await c.post("/api/test/hardware-checks", json={**body, **over})
        assert r.status_code == code, (over, r.text)
    # The 60 s rule between resets, as a dev deploy.
    fleet.devices["grid"].last_info = {"millis": 20_000}  # type: ignore[attr-defined]
    r = await c.post("/api/test/hardware-checks", json=body)
    assert r.status_code == 409 and "60 s between resets" in r.json()["detail"]
    assert "Retry-After" in r.headers
    fleet.devices["grid"].last_info = {"millis": 3_600_000}  # type: ignore[attr-defined]

    # Busy: another deploy or hardware test holds the box.
    async def active() -> bool:
        return True

    monkeypatch.setattr(routes_firmware, "_deploy_active", active)
    r = await c.post("/api/test/hardware-checks", json=body)
    assert r.status_code == 409 and "already running" in r.json()["detail"]
    assert len(submitted) == 2


async def test_hardware_checks_are_refused_in_dev_mode_and_off_the_box(
    box: tuple[AsyncClient, FakeFleet],
    service: FakeBuildService,
    submitted: list[Any],
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    c, fleet = box
    service.add("b1", b"")
    body = {"sha": SHA, "build_id": "b1", "device_ids": ["grid"], "source": "dev_build"}
    await c.put("/api/box/mode", json={"mode": "dev"})
    r = await c.post("/api/test/hardware-checks", json=body)
    assert r.status_code == 409 and "dev mode" in r.json()["detail"]
    await c.put("/api/box/mode", json={"mode": "queue"})
    monkeypatch.setattr(build_service, "ENV_FILE", tmp_path / "none.env")
    assert (await c.post("/api/test/hardware-checks", json=body)).status_code == 503
    fleet.staging_enabled = False
    assert (await c.post("/api/test/hardware-checks", json=body)).status_code == 403
    assert submitted == []


async def test_dev_hardware_checks_run_the_chip_as_it_is_in_dev_mode_only(
    box: tuple[AsyncClient, FakeFleet], submitted: list[Any]
) -> None:
    c, _ = box
    body = {"device_ids": ["grid"], "expected_version": VERSION, "lights_brightness": 0}
    assert (await c.post("/api/dev/hardware-checks", json=body)).status_code == 409  # queue mode
    await c.put("/api/box/mode", json={"mode": "dev"})
    r = await c.post("/api/dev/hardware-checks", json={**body, "phases": ["upload", "lights"]})
    assert r.status_code == 200, r.text
    name, req = submitted[0]
    assert name == "hardware_checks" and req["source"] == {"kind": "none"}
    assert req["runner"] == "dev" and req["phases"] == ["upload", "lights"]
    assert req["expected_version"] == VERSION and req["lights_brightness"] == 0
    for over, code in [
        ({"regions": "/etc/passwd"}, 400),
        ({"lights_brightness": 256}, 422),
        ({"phases": ["quality"]}, 422),
        ({"device_ids": ["nope"]}, 400),
    ]:
        r = await c.post("/api/dev/hardware-checks", json={**body, **over})
        assert r.status_code == code, (over, r.text)
    assert len(submitted) == 1


async def test_preempting_hardware_checks_queues_them_under_their_runner(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A staging runner's hardware checks go back in the staging queue, a PR head's in the
    PR queue: the runner is in the request."""
    from types import SimpleNamespace

    from dbos import DBOS

    async def listed(**k: Any) -> list[Any]:
        req = {"sha": SHA, "runner": "staging"}
        return [SimpleNamespace(name="test.hardware-checks", input={"args": [req]})]

    monkeypatch.setattr(DBOS, "list_workflows_async", listed)
    assert await box_mode._suite_of("hw-1") == (SHA, "staging")


async def test_only_a_hardware_checks_job_is_cancelled_by_its_route(
    box: tuple[AsyncClient, FakeFleet], monkeypatch: pytest.MonkeyPatch
) -> None:
    from dbos import DBOS

    from blinky_server.work import jobs

    c, _ = box
    got: dict[str, dict[str, Any]] = {
        "hw-1": {"kind": "hardware-checks", "status": "running"},
        "hw-2": {"kind": "hardware-checks", "status": "complete"},
        "suite-1": {"kind": "staging", "status": "running"},
    }
    cancelled: list[str] = []

    async def get(family: Any, job_id: str) -> dict[str, Any] | None:
        return got.get(job_id)

    async def cancel(job_id: str) -> None:
        cancelled.append(job_id)

    monkeypatch.setattr(jobs, "get", get)
    monkeypatch.setattr(DBOS, "cancel_workflow_async", cancel)
    monkeypatch.setattr(routes_testing, "_require_store", lambda: None)
    r = await c.delete("/api/test/hardware-checks/hw-1")
    assert r.status_code == 200 and r.json()["status"] == "cancelled"
    r = await c.delete("/api/test/hardware-checks/hw-2")
    assert r.status_code == 200 and r.json()["status"] == "complete"
    assert (await c.delete("/api/test/hardware-checks/suite-1")).status_code == 404
    assert (await c.delete("/api/test/hardware-checks/none")).status_code == 404
    assert cancelled == ["hw-1"]
