"""Tests for the SPA mount in blinky_server.api.app."""

from __future__ import annotations

from collections.abc import AsyncIterator
from pathlib import Path

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from blinky_server.api.app import _is_reserved, create_app
from blinky_server.api.deps import set_fleet

# --- _is_reserved unit tests -------------------------------------------------


def test_is_reserved_exact_api() -> None:
    assert _is_reserved("api") is True


def test_is_reserved_api_subpath() -> None:
    assert _is_reserved("api/devices") is True


def test_is_reserved_exact_ws() -> None:
    assert _is_reserved("ws") is True


def test_is_reserved_ws_subpath() -> None:
    assert _is_reserved("ws/MOCK_DEVICE_000") is True


def test_is_reserved_ignores_similar_prefixes() -> None:
    # Would previously 404 because of an over-broad `startswith("docs")` check.
    assert _is_reserved("documents") is False
    assert _is_reserved("apiclient") is False
    assert _is_reserved("websocket-info") is False


def test_is_reserved_ignores_fastapi_docs_paths() -> None:
    # These are registered by FastAPI itself and never reach the catch-all,
    # so they should not be in the reserved list — a SPA route named `/docs`
    # (or similar) should not collide.
    assert _is_reserved("docs") is False
    assert _is_reserved("redoc") is False
    assert _is_reserved("openapi.json") is False


def test_is_reserved_empty_path() -> None:
    assert _is_reserved("") is False


# --- SPA fallback integration tests ------------------------------------------


@pytest_asyncio.fixture
async def spa_client(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> AsyncIterator[AsyncClient]:
    """An app wired to a temp static dir with a known layout."""
    (tmp_path / "index.html").write_text("<html>INDEX</html>")
    (tmp_path / "favicon.svg").write_text("SVG")
    (tmp_path / "assets").mkdir()
    (tmp_path / "assets" / "app-abc123.js").write_text("// JS")

    monkeypatch.setenv("BLINKY_STATIC_DIR", str(tmp_path))
    # No fleet needed for these routes; API routes aren't exercised.
    app = create_app(enable_ble=False, enable_serial=False)
    set_fleet(None)  # type: ignore[arg-type]

    transport = ASGITransport(app=app)  # type: ignore[arg-type]
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        yield client


async def test_spa_root_returns_index(spa_client: AsyncClient) -> None:
    resp = await spa_client.get("/")
    assert resp.status_code == 200
    assert "INDEX" in resp.text


async def test_spa_deep_link_returns_index(spa_client: AsyncClient) -> None:
    # SPA route the server doesn't know about → must return index.html
    # so client-side routing can handle it after a hard refresh.
    resp = await spa_client.get("/devices/abc/settings")
    assert resp.status_code == 200
    assert "INDEX" in resp.text


async def test_spa_serves_real_file(spa_client: AsyncClient) -> None:
    resp = await spa_client.get("/favicon.svg")
    assert resp.status_code == 200
    assert resp.text == "SVG"


async def test_spa_serves_nested_asset(spa_client: AsyncClient) -> None:
    resp = await spa_client.get("/assets/app-abc123.js")
    assert resp.status_code == 200
    assert "// JS" in resp.text


async def test_api_404_not_masked_by_spa(spa_client: AsyncClient) -> None:
    # Unknown API paths must 404 as JSON, not serve index.html.
    resp = await spa_client.get("/api/nonexistent")
    assert resp.status_code == 404
    assert resp.headers["content-type"].startswith("application/json")


async def test_api_root_not_masked_by_spa(spa_client: AsyncClient) -> None:
    # `/api` exactly (no trailing path) also must 404, not serve index.html.
    # This was the bug in the initial M3 implementation.
    resp = await spa_client.get("/api")
    assert resp.status_code == 404


async def test_ws_path_over_http_is_404(spa_client: AsyncClient) -> None:
    # A regular HTTP GET to a WebSocket endpoint path should 404, not
    # silently return the SPA shell.
    resp = await spa_client.get("/ws/MOCK_DEVICE_000")
    assert resp.status_code == 404


async def test_path_traversal_falls_back_to_index(spa_client: AsyncClient) -> None:
    # Attempts to escape the static root must not leak outside files.
    resp = await spa_client.get("/../../../../../etc/passwd")
    # httpx may normalize this; the server's catch-all must not return
    # filesystem contents regardless.
    assert "root:" not in resp.text


async def test_missing_static_dir_skips_mount(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    nonexistent = tmp_path / "does-not-exist"
    monkeypatch.setenv("BLINKY_STATIC_DIR", str(nonexistent))
    app = create_app(enable_ble=False, enable_serial=False)
    set_fleet(None)  # type: ignore[arg-type]

    transport = ASGITransport(app=app)  # type: ignore[arg-type]
    async with AsyncClient(transport=transport, base_url="http://test") as client:
        # With no static dir, the catch-all isn't registered — GET / should
        # 404 because nothing handles the root path.
        resp = await client.get("/")
        assert resp.status_code == 404
