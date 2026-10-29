"""The durable work store's state (#325): whether the server is draining, and what's in flight.

During a restart the same thing is in systemd's status (``systemctl status blinky-server``).
"""

from __future__ import annotations

from typing import Any

from fastapi import APIRouter

from ..work import durable

router = APIRouter(tags=["work"])


@router.get("/work")
async def work_status() -> dict[str, Any]:
    return {
        "running": durable.launched(),
        "draining": durable.draining(),
        "in_flight": durable.in_flight(),
    }
