"""Scene CRUD + apply endpoints.

Endpoints are under ``/api/scenes`` (prefix added in app.py).

Apply semantics: the scene's command sequence is sent via ``FleetManager.send_to_all``
so every connected device gets the same configuration. Subset targeting isn't
wired yet - add ``?device_ids=id1,id2`` when needed.

Security: scene names are used to derive on-disk filenames via ``slugify``
(lowercased + non-alphanumerics->'-'). We validate the URL-path ``name``
matches its slug so a crafted path like ``../../etc/passwd`` is rejected at
the API boundary rather than relying on slugify's implicit normalisation.
"""

from __future__ import annotations

import re

from fastapi import APIRouter, HTTPException

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


@router.post("/scenes/{name}/apply")
async def scenes_apply(name: str) -> dict[str, object]:
    """Apply a scene to every connected device.

    Apply doesn't abort on partial failure because a half-applied state is
    worse than retrying — but the response includes per-command per-device
    results so callers can detect a device that rejected an intermediate
    command (e.g. ``gen fire`` succeeds on device A but fails on device B,
    then ``effect hue`` succeeds on both; without the full trace a client
    would only see the successful last step).

    Response shape: ``{scene, commands, results: [{command, responses:
    {device_id: "ok"|"error: ...", ...}}, ...]}``. ``responses`` is kept
    as an alias for ``results[-1].responses`` for backward compatibility
    with clients written against the prior single-step response.
    """
    _validate_name_or_404(name)
    scene = get_scene(name)
    if scene is None:
        raise HTTPException(404, f"Scene not found: {name}")

    fleet = get_fleet()
    commands = scene_to_commands(scene)
    results: list[dict[str, object]] = []
    for cmd in commands:
        responses = await fleet.send_to_all(cmd)
        results.append({"command": cmd, "responses": responses})
    last_responses = results[-1]["responses"] if results else {}
    return {
        "scene": scene.name,
        "commands": commands,
        "results": results,
        "responses": last_responses,  # backward-compat alias for prior callers
    }
