"""Scene CRUD + apply endpoints.

Endpoints are under ``/api/scenes`` (prefix added in app.py).

Apply semantics: the scene's command sequence is broadcast via
``FleetManager.broadcast`` so every device in range gets the same
configuration. Subset targeting isn't wired yet - BLE broadcasts hit
everything advertising on the radio.

Security: scene names are used to derive on-disk filenames via ``slugify``
(lowercased + non-alphanumerics->'-'). We validate the URL-path ``name``
matches its slug so a crafted path like ``../../etc/passwd`` is rejected at
the API boundary rather than relying on slugify's implicit normalisation.
"""

from __future__ import annotations

import re

from fastapi import APIRouter, HTTPException

from .. import scene_cursor
from ..scenes import (
    Scene,
    delete_scene,
    get_scene,
    list_scenes,
    save_scene,
    scene_to_commands,
)
from .deps import get_fleet

router = APIRouter(tags=["scenes"])


# Accept a superset of slug chars (letters, digits, spaces, hyphen, underscore,
# and a handful of punctuation) at the URL level. The resource identifier on
# disk is always the slug; we reject anything that would slug-differ to force
# clients to use consistent names. This also blocks path traversal (``/``,
# ``..``) and NUL bytes.
_NAME_RE = re.compile(r"^[\w][\w \-]{0,63}$")


def _validate_name_or_404(name: str) -> None:
    """Reject scene-name path params that don't look like scene names.

    A 404 is preferred over 400 for security-sensitive input so probes can't
    distinguish "invalid name" from "scene not found" - both return the same
    shape. Length cap matches ``Scene.name`` (max_length=64) so we never
    route a name the Pydantic model itself would reject.
    """
    if not _NAME_RE.match(name):
        raise HTTPException(404, f"Scene not found: {name}")


@router.get("/scenes")
def scenes_list() -> list[Scene]:
    return list_scenes()


# NOTE: specific /scenes/<word> routes (current, next, previous) MUST be
# declared before the parameterised /scenes/{name} route below. FastAPI
# matches routes in declaration order; without this ordering a request
# for /scenes/current is matched by /scenes/{name} with name="current"
# and returns a 404 "Scene not found: current" instead of cursor state.
# /scenes/next and /scenes/previous don't currently collide (different
# HTTP method than GET /scenes/{name}) but moving them up is the safer
# convention so a future GET /scenes/{name}/... route doesn't surprise us.


@router.get("/scenes/current")
def scenes_current() -> dict[str, object]:
    """Report the cursor state without changing it.

    Useful for the Hub UI to highlight the currently-applied scene
    chip. Returns ``current=null`` if no scene has ever been applied
    via ``/apply`` / ``/next`` / ``/previous`` on this install, or if
    the cursor's scene was deleted."""
    scenes = list_scenes()
    cursor_name = scene_cursor.get_current()
    index = _resolve_cursor_index(scenes) if cursor_name else -1
    return {
        "current": scenes[index].name if index >= 0 else None,
        "index": index if index >= 0 else None,
        "count": len(scenes),
    }


@router.post("/scenes/next")
async def scenes_next() -> dict[str, object]:
    """Advance to the next scene in alphabetical order and apply it.

    The cursor is persisted by scene NAME (not index) in
    ``scenes_dir()/.cursor.json``. Adding, deleting, or renaming scenes
    between presses doesn't randomly shift the cursor relative to the
    operator's expectation — the cursor just re-finds its name in the
    new list, or falls back to the start if its scene was deleted.

    Wraps around the end of the list. 409 if the scene library is empty.
    Designed for hardware-button consumers: one fixed URL, no name
    interpolation, no client-side cursor file."""
    return await _step_and_apply(+1)


@router.post("/scenes/previous")
async def scenes_previous() -> dict[str, object]:
    """Mirror of ``/next`` in the opposite direction."""
    return await _step_and_apply(-1)


@router.get("/scenes/{name}")
def scenes_get(name: str) -> Scene:
    _validate_name_or_404(name)
    scene = get_scene(name)
    if scene is None:
        raise HTTPException(404, f"Scene not found: {name}")
    return scene


@router.put("/scenes/{name}")
def scenes_put(name: str, body: Scene) -> Scene:
    _validate_name_or_404(name)
    # URL name wins over body name - the URL is the resource identifier, and
    # enforcing consistency avoids accidentally saving under the wrong slug.
    scene = body.model_copy(update={"name": name})
    return save_scene(scene)


@router.delete("/scenes/{name}")
def scenes_delete(name: str) -> dict[str, bool]:
    _validate_name_or_404(name)
    return {"deleted": delete_scene(name)}


async def _apply_scene(scene: Scene) -> dict[str, object]:
    """Run a scene's command sequence through the fleet broadcaster.

    Apply doesn't abort on partial failure because a half-applied state is
    worse than retrying — but the response includes per-command per-device
    results so callers can detect a device that rejected an intermediate
    command (e.g. ``gen fire`` succeeds on device A but fails on device B,
    then ``effect hue`` succeeds on both; without the full trace a client
    would only see the successful last step).

    **Cursor semantics: tracks operator intent, NOT delivery.**

    ``scene_cursor.set_current()`` is called unconditionally after the
    broadcast loop, regardless of per-device delivery status. This is
    deliberate: BLE broadcast is fire-and-forget with ~37%-~100% per-
    packet capture depending on conditions, so a strict "advance only on
    100% delivery" rule would routinely fail to update the cursor when
    the operator visibly saw the scene take effect on most devices. The
    "what scene did I last ask for?" model matches operator mental model
    better than "what scene did every device confirm?". A subsequent
    ``/api/scenes/next`` press always advances from the last-requested
    scene, not the last-fully-delivered one. If the policy ever changes,
    gate the ``set_current`` call on inspecting ``results`` for
    per-device acks.
    """
    fleet = get_fleet()
    commands = scene_to_commands(scene)
    results: list[dict[str, object]] = []
    for cmd in commands:
        responses = await fleet.broadcast(cmd)
        results.append({"command": cmd, "responses": responses})
    scene_cursor.set_current(scene.name)
    last_responses = results[-1]["responses"] if results else {}
    return {
        "scene": scene.name,
        "commands": commands,
        "results": results,
        "responses": last_responses,  # backward-compat alias for prior callers
    }


@router.post("/scenes/{name}/apply")
async def scenes_apply(name: str) -> dict[str, object]:
    """Apply a scene to every connected device.

    Response shape: ``{scene, commands, results: [{command, responses:
    {device_id: "ok"|"error: ...", ...}}, ...]}``. ``responses`` is kept
    as an alias for ``results[-1].responses`` for backward compatibility
    with clients written against the prior single-step response.
    """
    _validate_name_or_404(name)
    scene = get_scene(name)
    if scene is None:
        raise HTTPException(404, f"Scene not found: {name}")
    return await _apply_scene(scene)


def _resolve_cursor_index(scenes: list[Scene]) -> int:
    """Return the current cursor's index in ``scenes``.

    Returns ``-1`` (interpreted as "before the start") if there's no
    persisted cursor or if the cursored scene has been deleted - so
    ``/next`` lands on index 0 and ``/previous`` lands on the last
    scene, both of which are the least-surprising behaviours.
    """
    cursor_name = scene_cursor.get_current()
    if not cursor_name:
        return -1
    for i, s in enumerate(scenes):
        if s.name == cursor_name:
            return i
    return -1


def _next_cursor_index(current_idx: int, direction: int, count: int) -> int:
    """Pick the target index for a ``/next`` or ``/previous`` step.

    ``current_idx == -1`` is the "no cursor" sentinel from
    ``_resolve_cursor_index`` (no persisted cursor, or the cursored
    scene was deleted). The naive ``(current_idx + direction) % count``
    misbehaves there: ``(-1 + -1) % N`` evaluates to ``N - 2`` in Python,
    so ``/previous`` would skip the last scene. Special-case the
    no-cursor state to match ``_resolve_cursor_index``'s contract:
    ``/next`` lands on index 0, ``/previous`` lands on the last index.
    """
    if current_idx == -1:
        return 0 if direction == 1 else count - 1
    return (current_idx + direction) % count


async def _step_and_apply(direction: int) -> dict[str, object]:
    """Internal: walk the sorted scene list by ``direction`` (±1) from
    the persisted cursor, apply, and update the cursor. Wraps."""
    scenes = list_scenes()
    if not scenes:
        raise HTTPException(409, "No scenes saved; create one first")
    current_idx = _resolve_cursor_index(scenes)
    next_idx = _next_cursor_index(current_idx, direction, len(scenes))
    response = await _apply_scene(scenes[next_idx])
    # Augment with cursor info so a client can render "scene N of M".
    response["index"] = next_idx
    response["count"] = len(scenes)
    return response
