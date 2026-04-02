import contextlib
import logging
import os
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

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

    return app
