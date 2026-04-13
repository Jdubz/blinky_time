"""Shared dependencies for API routes."""

import os

from fastapi import Header, HTTPException

from ..device.manager import FleetManager

_fleet: FleetManager | None = None

# API key for flash/deploy endpoints. Set via BLINKY_API_KEY env var
# or defaults to a generated key written to ~/.blinky-api-key on first run.
_API_KEY_ENV = "BLINKY_API_KEY"
_API_KEY_FILE = os.path.expanduser("~/.blinky-api-key")


def _load_api_key() -> str:
    """Load API key from env or file, generating one if neither exists."""
    key = os.environ.get(_API_KEY_ENV)
    if key:
        return key
    if os.path.isfile(_API_KEY_FILE):
        return open(_API_KEY_FILE).read().strip()
    # Generate and persist
    import secrets

    key = secrets.token_urlsafe(32)
    with open(_API_KEY_FILE, "w") as f:
        f.write(key + "\n")
    os.chmod(_API_KEY_FILE, 0o600)
    return key


API_KEY = _load_api_key()


def set_fleet(fm: FleetManager | None) -> None:
    global _fleet
    _fleet = fm


def get_fleet() -> FleetManager:
    assert _fleet is not None, "Fleet manager not initialized"
    return _fleet


async def require_api_key(x_api_key: str = Header(..., alias="X-API-Key")) -> None:
    """FastAPI dependency that validates the X-API-Key header."""
    if x_api_key != API_KEY:
        raise HTTPException(403, "Invalid API key")
