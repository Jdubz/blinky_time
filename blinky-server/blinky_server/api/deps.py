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
        raise HTTPException(401, "Invalid API key")


# Flash endpoints (/api/fleet/upload, /api/fleet/flash) must be reached only
# via scripts/deploy.sh. The API key alone is not sufficient — direct curl
# (even with the right key) bypasses the compile/upload/verify pipeline that
# deploy.sh runs end-to-end. The v33 onset-model regression (2026-04-27) wasn't
# caused by a bypassed deploy.sh, but during diagnosis we discovered Claude
# was bypassing it routinely; this gate makes that fail loudly. The header
# value is intentionally not a secret — the goal is to stop accidental
# bypass, not to defend against an adversary with a valid API key.
_DEPLOY_TOOL_PREFIX = "deploy.sh-"


async def require_deploy_tool(
    x_deploy_tool: str = Header(..., alias="X-Deploy-Tool"),
) -> None:
    """FastAPI dependency that requires the request comes from scripts/deploy.sh.

    deploy.sh sets `X-Deploy-Tool: deploy.sh-<gitsha>`; any other value (or a
    missing header) is rejected with 403. This is *not* a security mechanism —
    it gates accidental direct-curl bypasses of the compile/upload/verify
    pipeline. See CLAUDE.md "CRITICAL: Upload Safety".
    """
    if not x_deploy_tool.startswith(_DEPLOY_TOOL_PREFIX):
        raise HTTPException(
            403,
            "X-Deploy-Tool header missing or invalid. "
            "Use scripts/deploy.sh — direct curl against /fleet/upload or /fleet/flash "
            "is forbidden (see CLAUDE.md 'CRITICAL: Upload Safety').",
        )


# Commands that mutate persistent device state or device lifecycle. Sending
# these in a loop without proper reboot/reconnection handling leaves devices
# in error states (see fps_sweep.py incident, 2026-05-01). These have zero
# legitimate callers outside of deploy.sh, so the cost of gating them is
# zero and the cost of NOT gating them is real (manual fleet recovery).
#
# Left intentionally open: gen/effect/set/save/load/defaults (legitimate
# UI use from blinky-console) and read-only commands (json info, ping).
_DEPLOY_GATED_COMMAND_PREFIXES = (
    "device upload",  # writes new device config to flash
    "reboot",  # device lifecycle — resets connection state
)


def is_deploy_gated_command(cmd: str) -> bool:
    """True if `cmd` is a device-mutating command that requires X-Deploy-Tool.

    Used by /devices/{id}/command and /fleet/command to gate the dangerous
    subset of free-text commands while keeping UI-driven commands open.
    """
    cmd_stripped = cmd.strip().lower()
    return any(
        cmd_stripped == prefix or cmd_stripped.startswith(prefix + " ")
        for prefix in _DEPLOY_GATED_COMMAND_PREFIXES
    )


def assert_command_allowed(cmd: str, x_deploy_tool: str | None) -> None:
    """Raise 403 if `cmd` is deploy-gated and X-Deploy-Tool is missing/invalid.

    Call from any route that accepts a free-text command string. Read the
    X-Deploy-Tool header optionally (`Header(None)`) so legitimate UI
    callers without the header still reach this check rather than failing
    earlier on a required-header validation.
    """
    if not is_deploy_gated_command(cmd):
        return
    if x_deploy_tool is None or not x_deploy_tool.startswith(_DEPLOY_TOOL_PREFIX):
        raise HTTPException(
            403,
            f"Command '{cmd[:40]}' requires X-Deploy-Tool header. "
            "Device-mutating commands (device upload, reboot) must be issued "
            "by scripts/deploy.sh — direct curl is forbidden. "
            "See CLAUDE.md 'CRITICAL: Upload Safety'.",
        )
