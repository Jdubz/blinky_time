"""Phase-4 tests: ``/api/flash-jobs`` HTTP surface.

Exercises the route layer against the Phase-3 stub orchestrator (which
fails every job with a "not yet wired" error after transport selection).
That's fine — these tests are about the HTTP shape and the FleetManager
integration, not about real flashes.

Real flash behavior is tested live in Phase 7.
"""

from __future__ import annotations

from collections.abc import AsyncGenerator
from pathlib import Path

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from blinky_server.api import deps
from blinky_server.api.app import create_app
from blinky_server.api.deps import require_api_key, require_deploy_tool, set_fleet
from blinky_server.device.manager import FleetManager


@pytest_asyncio.fixture
async def api_client_unauthed() -> AsyncGenerator[tuple[AsyncClient, FleetManager], None]:
    """Client + fleet, with auth dependencies enabled. For testing the gates."""
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    set_fleet(fleet)
    app = create_app()
    transport = ASGITransport(app=app)  # type: ignore[arg-type]
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        yield client, fleet
    set_fleet(None)  # type: ignore[arg-type]


@pytest_asyncio.fixture
async def api_client_authed() -> AsyncGenerator[tuple[AsyncClient, FleetManager], None]:
    """Client + fleet, with auth dependencies overridden to no-op.

    Used for testing route business logic — the auth gates are tested
    separately with the un-overridden ``api_client_unauthed`` fixture.
    """
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    set_fleet(fleet)
    app = create_app()
    # Bypass the API key + deploy-tool gates for route-logic testing.
    app.dependency_overrides[require_api_key] = lambda: None
    app.dependency_overrides[require_deploy_tool] = lambda: None
    transport = ASGITransport(app=app)  # type: ignore[arg-type]
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        yield client, fleet
    set_fleet(None)  # type: ignore[arg-type]


@pytest.fixture
def fw_file(tmp_path: Path) -> Path:
    """A real on-disk file the API can find at body.firmware_path."""
    p = tmp_path / "firmware.hex"
    p.write_text(":00000001FF\n")  # minimal valid-looking Intel HEX
    return p


# --- POST /api/flash-jobs ---------------------------------------------------


async def test_post_creates_job_returns_202(
    api_client_authed: tuple[AsyncClient, FleetManager], fw_file: Path
) -> None:
    client, fleet = api_client_authed
    resp = await client.post(
        "/api/flash-jobs",
        json={"device_id": "dev-1", "firmware_path": str(fw_file)},
    )
    assert resp.status_code == 202, resp.text
    body = resp.json()
    assert body["device_id"] == "dev-1"
    assert body["state"] in ("pending", "selecting_transport", "failed")
    assert "job_id" in body
    # The fleet should now know about this job.
    assert fleet.get_flash_job("dev-1") is not None


async def test_post_rejects_missing_firmware_path(
    api_client_authed: tuple[AsyncClient, FleetManager],
) -> None:
    client, _ = api_client_authed
    resp = await client.post(
        "/api/flash-jobs",
        json={"device_id": "dev-1", "firmware_path": "/nonexistent/path.hex"},
    )
    assert resp.status_code == 400
    assert "does not exist" in resp.json()["detail"]


@pytest.mark.parametrize(
    "body",
    [
        {},  # both fields missing
        {"device_id": "dev"},  # firmware_path missing
        {"firmware_path": "/f"},  # device_id missing
        {"device_id": "", "firmware_path": "/f"},  # empty device_id
    ],
)
async def test_post_rejects_invalid_body(
    api_client_authed: tuple[AsyncClient, FleetManager], body: dict[str, object]
) -> None:
    client, _ = api_client_authed
    resp = await client.post("/api/flash-jobs", json=body)
    assert resp.status_code == 422  # Pydantic validation error


# --- Auth gates -------------------------------------------------------------


async def test_post_requires_api_key(
    api_client_unauthed: tuple[AsyncClient, FleetManager], fw_file: Path
) -> None:
    """No headers at all → fails the API-key gate first (401 or 422 depending
    on FastAPI's header-missing handling — either is acceptable here)."""
    client, _ = api_client_unauthed
    resp = await client.post(
        "/api/flash-jobs",
        json={"device_id": "dev-1", "firmware_path": str(fw_file)},
    )
    # FastAPI returns 422 for missing required Header(...), 401 for invalid.
    # Either confirms the request was rejected before reaching route logic.
    assert resp.status_code in (401, 422)


async def test_post_requires_deploy_tool_header(
    api_client_unauthed: tuple[AsyncClient, FleetManager],
    fw_file: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """With API key but no deploy-tool header → 403 / 422."""
    # Set a known API key and present it; the deploy-tool gate should still
    # reject because the X-Deploy-Tool header is missing.
    monkeypatch.setattr(deps, "_api_key", "test-api-key-abc")
    client, _ = api_client_unauthed
    resp = await client.post(
        "/api/flash-jobs",
        json={"device_id": "dev-1", "firmware_path": str(fw_file)},
        headers={"X-API-Key": "test-api-key-abc"},
    )
    assert resp.status_code in (403, 422)


async def test_post_accepts_valid_auth(
    api_client_unauthed: tuple[AsyncClient, FleetManager],
    fw_file: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Valid API key + deploy.sh-prefixed deploy-tool header → 202."""
    monkeypatch.setattr(deps, "_api_key", "test-api-key-abc")
    client, _ = api_client_unauthed
    resp = await client.post(
        "/api/flash-jobs",
        json={"device_id": "dev-1", "firmware_path": str(fw_file)},
        headers={
            "X-API-Key": "test-api-key-abc",
            "X-Deploy-Tool": "deploy.sh-test1234",
        },
    )
    assert resp.status_code == 202, resp.text


# --- GET /api/flash-jobs/{id} ----------------------------------------------


async def test_get_returns_job_after_post(
    api_client_authed: tuple[AsyncClient, FleetManager], fw_file: Path
) -> None:
    client, _ = api_client_authed
    post = await client.post(
        "/api/flash-jobs",
        json={"device_id": "dev-1", "firmware_path": str(fw_file)},
    )
    job_id = post.json()["job_id"]
    resp = await client.get(f"/api/flash-jobs/{job_id}")
    assert resp.status_code == 200
    body = resp.json()
    assert body["job_id"] == job_id
    assert body["device_id"] == "dev-1"


async def test_get_404_for_unknown_job(
    api_client_authed: tuple[AsyncClient, FleetManager],
) -> None:
    client, _ = api_client_authed
    resp = await client.get("/api/flash-jobs/does-not-exist")
    assert resp.status_code == 404


# --- GET /api/flash-jobs (list) --------------------------------------------


async def test_list_empty(
    api_client_authed: tuple[AsyncClient, FleetManager],
) -> None:
    client, _ = api_client_authed
    resp = await client.get("/api/flash-jobs")
    assert resp.status_code == 200
    body = resp.json()
    assert body["count"] == 0
    assert body["jobs"] == []


async def test_list_returns_jobs_newest_first(
    api_client_authed: tuple[AsyncClient, FleetManager], fw_file: Path
) -> None:
    client, _ = api_client_authed
    a = await client.post(
        "/api/flash-jobs",
        json={"device_id": "dev-1", "firmware_path": str(fw_file)},
    )
    b = await client.post(
        "/api/flash-jobs",
        json={"device_id": "dev-2", "firmware_path": str(fw_file)},
    )
    resp = await client.get("/api/flash-jobs")
    assert resp.status_code == 200
    body = resp.json()
    assert body["count"] == 2
    # Most-recent first — dev-2 was created second.
    assert body["jobs"][0]["device_id"] == "dev-2"
    assert body["jobs"][1]["device_id"] == "dev-1"


async def test_list_active_filter(
    api_client_authed: tuple[AsyncClient, FleetManager], fw_file: Path
) -> None:
    """After the stub orchestrator runs, jobs end FAILED (terminal). The
    ?active=true filter should hide them."""
    client, fleet = api_client_authed
    post = await client.post(
        "/api/flash-jobs",
        json={"device_id": "dev-1", "firmware_path": str(fw_file)},
    )
    # Wait for the stub orchestrator's background task to complete.
    job = fleet.get_flash_job("dev-1")
    assert job is not None
    await job.wait_until_terminal(timeout=2.0)
    # Now ?active=true should return nothing; the default should return the job.
    all_resp = await client.get("/api/flash-jobs")
    assert all_resp.json()["count"] == 1
    active_resp = await client.get("/api/flash-jobs?active=true")
    assert active_resp.json()["count"] == 0
    # Job exists by id but isn't active.
    one = await client.get(f"/api/flash-jobs/{post.json()['job_id']}")
    assert one.json()["is_terminal"] is True
