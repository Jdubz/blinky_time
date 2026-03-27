import logging
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware

from ..device.manager import FleetManager
from .deps import set_fleet

log = logging.getLogger(__name__)


_fleet_kwargs: dict = {}


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    fm = FleetManager(**_fleet_kwargs)
    set_fleet(fm)
    await fm.start()
    yield
    await fm.stop()
    set_fleet(None)


def create_app(
    enable_ble: bool = True,
    wifi_hosts: list[dict] | None = None,
) -> FastAPI:
    global _fleet_kwargs
    _fleet_kwargs = {"enable_ble": enable_ble, "wifi_hosts": wifi_hosts}
    app = FastAPI(
        title="Blinky Server",
        description="Fleet management API for Blinky Time LED art devices",
        version="0.1.0",
        lifespan=lifespan,
    )

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
    from .ws import router as ws_router

    app.include_router(devices_router, prefix="/api")
    app.include_router(commands_router, prefix="/api")
    app.include_router(ws_router)

    return app
