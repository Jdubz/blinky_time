"""Scene persistence for the LemonCart fleet UI.

A *scene* is a named snapshot of {generator, effect} that the console can save
and replay across every connected device. Scenes live as JSON files on disk
so they survive reinstalls and are inspectable by hand.

Storage: ``$XDG_DATA_HOME/blinky-server/scenes/`` (defaults to
``~/.local/share/blinky-server/scenes``). Each scene is ``<slug>.json``.
Slug is derived from the name (lowercased, non-alphanumerics → '-').

The control surface is intentionally tiny — generator + hue effect mode. The
MVP fleet UI doesn't expose per-setting sliders, so scenes don't either. Extra
fields can be added later without breaking existing files (forward-compatible
JSON — unknown keys ignored).
"""

from __future__ import annotations

import json
import logging
import os
import re
from pathlib import Path
from typing import Literal

from pydantic import BaseModel, Field

log = logging.getLogger(__name__)

EffectMode = Literal["off", "rotate", "static"]
GeneratorName = Literal["fire", "water", "lightning", "audio"]


class Scene(BaseModel):
    """A named fleet configuration."""

    name: str = Field(..., min_length=1, max_length=64)
    generator: GeneratorName
    effect_mode: EffectMode = "off"
    # Hue rotation speed in cycles/sec (firmware `huespeed`), 0–2. Only
    # meaningful when effect_mode == "rotate".
    effect_speed: float = Field(default=0.5, ge=0.0, le=2.0)
    # Static hue offset (firmware `hueshift`), 0–1. Only meaningful when
    # effect_mode == "static".
    effect_hue: float = Field(default=0.0, ge=0.0, le=1.0)


def _data_dir() -> Path:
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    d = Path(base) / "blinky-server" / "scenes"
    d.mkdir(parents=True, exist_ok=True)
    return d


_SLUG_RE = re.compile(r"[^a-z0-9]+")


def slugify(name: str) -> str:
    """Slug used for the on-disk filename. Lossy — two names with the same
    slug clobber each other, which is fine for a single-operator station."""
    s = _SLUG_RE.sub("-", name.lower()).strip("-")
    return s or "scene"


def _scene_path(name: str) -> Path:
    return _data_dir() / f"{slugify(name)}.json"


def list_scenes() -> list[Scene]:
    """Return every saved scene, sorted by name."""
    scenes: list[Scene] = []
    for p in sorted(_data_dir().glob("*.json")):
        try:
            scenes.append(Scene.model_validate_json(p.read_text()))
        except Exception as e:
            log.warning("Skipping unreadable scene file %s: %s", p.name, e)
    return scenes


def get_scene(name: str) -> Scene | None:
    p = _scene_path(name)
    if not p.is_file():
        return None
    try:
        return Scene.model_validate_json(p.read_text())
    except Exception as e:
        log.warning("Scene %s unreadable: %s", p.name, e)
        return None


def save_scene(scene: Scene) -> Scene:
    """Persist a scene. Overwrites any existing scene with the same slug."""
    p = _scene_path(scene.name)
    p.write_text(scene.model_dump_json(indent=2))
    log.info("Saved scene %s → %s", scene.name, p)
    return scene


def delete_scene(name: str) -> bool:
    p = _scene_path(name)
    if not p.is_file():
        return False
    p.unlink()
    log.info("Deleted scene %s", name)
    return True


def scene_to_commands(scene: Scene) -> list[str]:
    """Translate a Scene into the firmware command sequence that realises it.

    Order matters: generator first so the effect doesn't briefly paint over
    the wrong pattern; effect next; effect params last because setting
    `huespeed`/`hueshift` with no hue effect active is a no-op.
    """
    cmds: list[str] = [f"gen {scene.generator}"]
    if scene.effect_mode == "off":
        cmds.append("effect none")
        return cmds
    cmds.append("effect hue")
    if scene.effect_mode == "static":
        cmds.append("set huespeed 0")
        cmds.append(f"set hueshift {scene.effect_hue}")
    else:  # rotate
        cmds.append(f"set huespeed {scene.effect_speed}")
    return cmds
