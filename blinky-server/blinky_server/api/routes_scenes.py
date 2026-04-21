"""Scene CRUD + apply endpoints.

Endpoints are under ``/api/scenes`` (prefix added in app.py).

Apply semantics: the scene's command sequence is sent via ``FleetManager.send_to_all``
so every connected device gets the same configuration. Subset targeting isn't
wired yet — add ``?device_ids=id1,id2`` when needed.
"""

from __future__ import annotations

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


@router.get("/scenes")
def scenes_list() -> list[Scene]:
    return list_scenes()


@router.get("/scenes/{name}")
def scenes_get(name: str) -> Scene:
    scene = get_scene(name)
    if scene is None:
        raise HTTPException(404, f"Scene not found: {name}")
    return scene


@router.put("/scenes/{name}")
def scenes_put(name: str, body: Scene) -> Scene:
    # URL name wins over body name — the URL is the resource identifier, and
    # enforcing consistency avoids accidentally saving under the wrong slug.
    scene = body.model_copy(update={"name": name})
    return save_scene(scene)


@router.delete("/scenes/{name}")
def scenes_delete(name: str) -> dict[str, bool]:
    return {"deleted": delete_scene(name)}


@router.post("/scenes/{name}/apply")
async def scenes_apply(name: str) -> dict[str, object]:
    """Apply a scene to every connected device.

    Returns the per-device responses for the last command in the sequence
    (callers already know the sequence — they only need to know which devices
    reacted). Any earlier-command failures show up as 'error:' strings in
    intermediate responses; apply doesn't abort on partial failure because a
    half-applied state is worse than retrying.
    """
    scene = get_scene(name)
    if scene is None:
        raise HTTPException(404, f"Scene not found: {name}")

    fleet = get_fleet()
    commands = scene_to_commands(scene)
    final_result: dict[str, str] = {}
    for cmd in commands:
        final_result = await fleet.send_to_all(cmd)
    return {"scene": scene.name, "commands": commands, "responses": final_result}
