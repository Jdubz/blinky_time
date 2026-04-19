import contextlib
import logging
import os
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse

from ..device.manager import FleetManager
from .deps import set_fleet

log = logging.getLogger(__name__)


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    # Clean up orphan audio and stale locks from previous session
    from ..testing.audio_lock import LOCK_PATH, is_audio_locked, release_audio_lock
    from ..testing.audio_player import kill_orphan_audio, stop_audio

    await kill_orphan_audio()
    locked, _info = is_audio_locked()
    if locked:
        log.warning("Stale audio lock from previous session — releasing")
        release_audio_lock()
        with contextlib.suppress(OSError):
            os.unlink(LOCK_PATH)

    fm = FleetManager(**app.state.fleet_kwargs)
    set_fleet(fm)
    await fm.start()
    yield
    # Shutdown: kill any playing audio, stop fleet
    await stop_audio()
    await kill_orphan_audio()
    await fm.stop()
    set_fleet(None)


def create_app(
    enable_ble: bool = True,
    enable_serial: bool = True,
    wifi_hosts: list[dict[str, Any]] | None = None,
) -> FastAPI:
    app = FastAPI(
        title="Blinky Server",
        description="Fleet management API for Blinky Time LED art devices",
        version="0.1.0",
        lifespan=lifespan,
    )
    app.state.fleet_kwargs = {
        "enable_ble": enable_ble,
        "enable_serial": enable_serial,
        "wifi_hosts": wifi_hosts,
    }

    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # Import routers here to avoid circular imports
    from .routes_commands import router as commands_router
    from .routes_devices import router as devices_router
    from .routes_firmware import router as firmware_router
    from .routes_testing import router as testing_router
    from .ws import router as ws_router

    app.include_router(devices_router, prefix="/api")
    app.include_router(commands_router, prefix="/api")
    app.include_router(firmware_router, prefix="/api")
    app.include_router(testing_router, prefix="/api")
    app.include_router(ws_router)

    # Mount the built blinky-console as a SPA (after API routers so they
    # take precedence). Skipped silently if the static dir isn't present,
    # so the server runs fine in dev without a console build.
    _mount_frontend(app)

    return app


def _resolve_static_dir() -> Path:
    """Path to the built blinky-console assets. Configurable via BLINKY_STATIC_DIR."""
    if env := os.environ.get("BLINKY_STATIC_DIR"):
        # Resolve so a relative BLINKY_STATIC_DIR doesn't depend on CWD.
        return Path(env).resolve()
    # Default: blinky-server/web/ (Vite's build.outDir target, set in M4)
    return Path(__file__).resolve().parent.parent.parent / "web"


# Top-level paths whose namespaces must not be shadowed by the SPA fallback:
# `/api/*` and the WebSocket `/ws/*`. The FastAPI docs routes (/docs,
# /openapi.json, /redoc) aren't listed here — they're registered on the
# app before this catch-all, so a real request hits them first; a typo
# like `/documents` should reach the SPA, not 404.
_RESERVED_NAMES = frozenset({"api", "ws"})


def _is_reserved(path: str) -> bool:
    """True if `path` is exactly a reserved name or lives inside one."""
    if path in _RESERVED_NAMES:
        return True
    return any(path.startswith(name + "/") for name in _RESERVED_NAMES)


def _mount_frontend(app: FastAPI) -> None:
    """Serve the blinky-console SPA from the resolved static dir, if present.

    Adds a catch-all GET that serves real files from the static dir when they
    exist, and falls back to index.html for any other path — the standard SPA
    deep-link pattern. Reserved API and WebSocket namespaces are left alone
    so genuine 404s there still surface as 404s instead of an HTML page.
    """
    static_dir = _resolve_static_dir()
    if not static_dir.is_dir():
        log.info("Frontend static dir %s not present — SPA mount skipped", static_dir)
        return

    index_html = static_dir / "index.html"
    if not index_html.is_file():
        log.warning(
            "Frontend static dir %s exists but has no index.html — SPA mount skipped",
            static_dir,
        )
        return

    log.info("Serving blinky-console SPA from %s", static_dir)
    static_root = static_dir.resolve()

    @app.get("/{full_path:path}", include_in_schema=False)
    async def spa_fallback(full_path: str) -> FileResponse:
        if _is_reserved(full_path):
            raise HTTPException(status_code=404)
        # Real file in the static dir → serve it; guard against path traversal.
        try:
            target = (static_dir / full_path).resolve()
            target.relative_to(static_root)
            if target.is_file():
                return FileResponse(target)
        except ValueError:
            pass
        # no-cache ensures browsers fetch a fresh shell after UI deploys.
        # Static assets (JS/CSS with content hashes) are cached normally.
        return FileResponse(index_html, headers={"Cache-Control": "no-cache"})
