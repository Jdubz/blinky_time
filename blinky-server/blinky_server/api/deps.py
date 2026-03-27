"""Shared dependencies for API routes."""

from ..device.manager import FleetManager

_fleet: FleetManager | None = None


def set_fleet(fm: FleetManager | None) -> None:
    global _fleet
    _fleet = fm


def get_fleet() -> FleetManager:
    assert _fleet is not None, "Fleet manager not initialized"
    return _fleet
