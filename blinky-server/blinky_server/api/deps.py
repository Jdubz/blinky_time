"""Shared dependencies for API routes."""

import hmac
import os

from fastapi import Header, HTTPException

from ..device.manager import FleetManager

_fleet: FleetManager | None = None

# API key for flash/deploy endpoints. Set via BLINKY_API_KEY env var
# or defaults to a generated key written to ~/.blinky-api-key on first run.
_API_KEY_ENV = "BLINKY_API_KEY"
_API_KEY_FILE = os.path.expanduser("~/.blinky-api-key")

_api_key: str | None = None


def _get_api_key() -> str:
    """Lazy-load API key on first use (not at import time)."""
    global _api_key
    if _api_key is not None:
        return _api_key

    key = os.environ.get(_API_KEY_ENV)
    if not key and os.path.isfile(_API_KEY_FILE):
        with open(_API_KEY_FILE) as f:
            key = f.read().strip()
    if not key:
        import secrets

        key = secrets.token_urlsafe(32)
        with open(_API_KEY_FILE, "w") as f:
            f.write(key + "\n")
        os.chmod(_API_KEY_FILE, 0o600)

    _api_key = key
    return key


def set_fleet(fm: FleetManager | None) -> None:
    global _fleet
    _fleet = fm


def get_fleet() -> FleetManager:
    assert _fleet is not None, "Fleet manager not initialized"
    return _fleet


async def require_api_key(x_api_key: str = Header(..., alias="X-API-Key")) -> None:
    """FastAPI dependency that validates the X-API-Key header."""
    if not hmac.compare_digest(x_api_key, _get_api_key()):
        raise HTTPException(403, "Invalid API key")
