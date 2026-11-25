"""Shared dependencies for API routes."""

import hmac
import os
import re

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


# The low-level flash endpoints (/api/fleet/upload, /api/fleet/flash) are
# steps of the hardened deploy pipeline, not a way to deploy: direct curl
# (even with the right key) skips the rest of it (stable-only release,
# verify, restore + save, post-deploy assertion). scripts/deploy.sh drives
# them end-to-end; POST /api/fleet/deploy runs the same pipeline server-side
# (firmware/deploy_pipeline.py) and so doesn't need this header. The v33 onset-model regression (2026-04-27) wasn't
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
# Prefix-form gated commands: gate exact match AND `prefix <args>` form.
# These commands take arguments (`device upload {json}`) or could (`reboot`
# is no-arg today but reasonable to gate any future arg form too).
_DEPLOY_GATED_PREFIXES = (
    "device upload",  # writes new device config to flash
    "reboot",  # device lifecycle — resets connection state
    "wipe_device_identity",  # heavy reset; self-documenting name (#141)
)

# Exact-match-only gated commands: deprecated aliases for wipe_device_identity.
# Kept as exact-match-only (not prefix) so a future safe command like a
# hypothetical `reset_session <args>` isn't accidentally caught — `reset`
# the alias is no-arg and won't conflict with itself. Per PR 138 round-8
# review.
_DEPLOY_GATED_EXACT = (
    "factory",  # deprecated alias
    "reset",  # deprecated alias
)

# Combined heterogeneous list for documentation cross-references (CLAUDE.md,
# error messages). Some entries are prefix-form, some are exact-match-only.
# Do NOT iterate this directly with a prefix matcher; call
# is_deploy_gated_command() which dispatches to the right matching rule per
# entry. Per PR 138 round-10 review: name reflects the heterogeneous nature
# (the previous _COMMAND_PREFIXES name implied prefix-form, which was wrong
# for the factory/reset entries).
_DEPLOY_GATED_COMMAND_LIST = _DEPLOY_GATED_PREFIXES + _DEPLOY_GATED_EXACT

# Module-level compiled regex — keeps the gate hot-path independent of
# Python's regex cache size. Per PR 138 round-12 review.
_WHITESPACE_RE = re.compile(r"\s+")


def is_deploy_gated_command(cmd: str) -> bool:
    """True if `cmd` is a device-mutating command that requires X-Deploy-Tool.

    Used by /devices/{id}/command and /fleet/command to gate the dangerous
    subset of free-text commands while keeping UI-driven commands open.

    Whitespace is collapsed before matching so `"device  upload"` (double
    space) and `" device\tupload"` are gated identically to canonical
    `"device upload"`. The firmware also wouldn't recognize the malformed
    form, but normalizing here keeps the gate's contract independent of
    the firmware's tokenizer.

    The deprecated `factory`/`reset` aliases are gated as exact-match only,
    so a hypothetical future `reset_session <args>` safe command isn't
    accidentally caught. Per PR 138 round-8 review.
    """
    cmd_normalized = _WHITESPACE_RE.sub(" ", cmd.strip().lower())
    if cmd_normalized in _DEPLOY_GATED_EXACT:
        return True
    return any(
        cmd_normalized == prefix or cmd_normalized.startswith(prefix + " ")
        for prefix in _DEPLOY_GATED_PREFIXES
    )


# Test-box-only commands: refused on any server not started with --staging, and in fleet
# broadcasts everywhere. `leds` overrides the LED brightness at runtime (testing/leds.py); only
# the box's chips, whose config brightness is 0, are lit and darkened with it. Every other
# device keeps its configured brightness (owner, 2026-11-24).
_TEST_BOX_ONLY_PREFIXES = ("leds",)


def is_test_box_only_command(cmd: str) -> bool:
    """True if `cmd` may only go to a test box chip (see _TEST_BOX_ONLY_PREFIXES)."""
    cmd_normalized = _WHITESPACE_RE.sub(" ", cmd.strip().lower())
    return any(
        cmd_normalized == prefix or cmd_normalized.startswith(prefix + " ")
        for prefix in _TEST_BOX_ONLY_PREFIXES
    )


def assert_test_box_rules(cmd: str, *, test_box: bool) -> None:
    """Raise if `cmd` holds more than one line (a newline would reach the device as a second
    command no gate saw: ``gen fire\nleds 0``), or is test-box-only off the box. For every path
    that forwards a free-text command: the command routes (assert_command_allowed), WebSocket
    commands and test jobs' command lists."""
    if "\n" in cmd or "\r" in cmd:
        raise HTTPException(400, "A command is one line: it must not contain CR or LF.")
    if not test_box and is_test_box_only_command(cmd):
        raise HTTPException(
            403,
            f"Command '{cmd.strip()[:60]}' is for the test box's chips only (a server started "
            "with --staging); it is never sent to fleet devices.",
        )


def assert_command_allowed(cmd: str, x_deploy_tool: str | None, *, test_box: bool = False) -> None:
    """Raise 403 if `cmd` is deploy-gated and X-Deploy-Tool is missing/invalid.

    Call from any route that accepts a free-text command string. Read the
    X-Deploy-Tool header optionally (`Header(None)`) so legitimate UI
    callers without the header still reach this check rather than failing
    earlier on a required-header validation.

    ``test_box``: the command targets one chip on the test box (a server started
    with --staging). Every chip there is a test chip, iterated on directly, so
    the deploy.sh gate (which protects the fleet) doesn't apply. The firmware's
    60 s rule between resets still does: that is on the caller. Test-box-only
    commands (``leds``) are refused unless ``test_box``, and so is any command
    with a CR or LF in it (assert_test_box_rules).
    """
    assert_test_box_rules(cmd, test_box=test_box)
    if test_box or not is_deploy_gated_command(cmd):
        return
    if x_deploy_tool is None or not x_deploy_tool.startswith(_DEPLOY_TOOL_PREFIX):
        # `device upload <large JSON>` is the realistic worst case: the
        # JSON body can be hundreds of chars, and truncating mid-payload
        # (the prior 40-char cap) made the message confusing. 60 chars
        # + ellipsis surfaces enough to identify the command without
        # spilling the full body. Per PR 138 round-14 review (LOW #1).
        cmd_display = cmd if len(cmd) <= 60 else cmd[:60] + "…"
        raise HTTPException(
            403,
            f"Command '{cmd_display}' requires X-Deploy-Tool header. "
            "Device-mutating commands must be issued by scripts/deploy.sh "
            "— direct curl is forbidden. See CLAUDE.md 'CRITICAL: Upload "
            "Safety' or _DEPLOY_GATED_COMMAND_LIST in this module for the "
            "full gated list.",
        )
