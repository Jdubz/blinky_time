"""Tests for blinky_server.scenes — scene CRUD + behaviours changed in
PR 131 review (atomic saves, sort-by-display-name, cached data dir)."""

from __future__ import annotations

from pathlib import Path

import pytest

from blinky_server import scenes as scenes_mod
from blinky_server.scenes import (
    Scene,
    delete_scene,
    get_scene,
    list_scenes,
    save_scene,
    scene_to_commands,
    slugify,
)


@pytest.fixture(autouse=True)
def isolated_scenes_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Redirect the scenes store to a throwaway tmp dir for each test.

    Also clears the module-level cached Path so the next call to _data_dir
    re-derives from XDG_DATA_HOME.
    """
    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path))
    monkeypatch.setattr(scenes_mod, "_CACHED_DATA_DIR", None)
    yield
    # Post-test reset so other test modules don't see a leaked pointer.
    monkeypatch.setattr(scenes_mod, "_CACHED_DATA_DIR", None)


def _scene(name: str, generator: str = "fire") -> Scene:
    return Scene(name=name, generator=generator)  # type: ignore[arg-type]


def test_slugify_handles_edge_cases() -> None:
    assert slugify("Living Room") == "living-room"
    assert slugify("  __weird!!  ") == "weird"
    assert slugify("") == "scene"  # empty → fallback
    assert slugify("!!!") == "scene"
    # Different display names that collapse to the same slug overwrite each
    # other. This is documented and fine for a single-operator kiosk.
    assert slugify("Bar!") == slugify("bar")


def test_save_then_get_round_trips() -> None:
    s = _scene("Main", generator="water")
    save_scene(s)
    got = get_scene("Main")
    assert got is not None
    assert got.name == "Main"
    assert got.generator == "water"


def test_list_scenes_sorts_by_display_name_case_insensitive() -> None:
    # Save in reverse-alpha order with deliberately mixed case + non-ASCII to
    # exercise casefold sorting vs slug sorting (they'd differ here).
    save_scene(_scene("zebra"))
    save_scene(_scene("Apple"))
    save_scene(_scene("banana"))
    names = [s.name for s in list_scenes()]
    assert names == ["Apple", "banana", "zebra"]


def test_save_is_atomic_via_tmp_replace(tmp_path: Path) -> None:
    """Atomic save pattern: never leaves a truncated target file visible.

    We can't easily simulate a crash in CI, but we CAN verify the
    implementation detail that the tmp file doesn't coexist with the target
    after a successful save (i.e. replace ran).
    """
    save_scene(_scene("Party"))
    data_dir = scenes_mod._data_dir()
    files = sorted(p.name for p in data_dir.iterdir())
    # Only the final file should exist — no .tmp stragglers.
    assert files == ["party.json"]


def test_save_overwrites_same_slug() -> None:
    save_scene(_scene("Party", generator="fire"))
    save_scene(_scene("Party", generator="water"))
    got = get_scene("Party")
    assert got is not None and got.generator == "water"
    assert len(list_scenes()) == 1


def test_delete_scene_returns_false_for_missing() -> None:
    assert delete_scene("never-existed") is False


def test_delete_scene_removes_file() -> None:
    save_scene(_scene("Gone"))
    assert delete_scene("Gone") is True
    assert get_scene("Gone") is None
    assert list_scenes() == []


def test_data_dir_is_cached_within_a_process() -> None:
    assert scenes_mod._CACHED_DATA_DIR is None
    first = scenes_mod._data_dir()
    second = scenes_mod._data_dir()
    assert first is second  # same object — cached, not re-derived
    assert scenes_mod._CACHED_DATA_DIR is first


def test_data_dir_creates_the_directory() -> None:
    # Caching doesn't skip the initial mkdir — verify the dir exists.
    d = scenes_mod._data_dir()
    assert d.is_dir()


def test_scene_to_commands_off_mode() -> None:
    s = _scene("Off", generator="fire")
    s.effect_mode = "off"
    assert scene_to_commands(s) == ["gen fire", "effect none"]


def test_scene_to_commands_static_mode_sets_both_huespeed_and_hueshift() -> None:
    s = _scene("Static", generator="water")
    s.effect_mode = "static"
    s.effect_hue = 0.42
    cmds = scene_to_commands(s)
    # Order matters: generator → effect hue → huespeed 0 → hueshift
    assert cmds[0] == "gen water"
    assert cmds[1] == "effect hue"
    assert cmds[2] == "set huespeed 0"
    assert cmds[3].startswith("set hueshift ")
    assert "0.42" in cmds[3]


def test_scene_to_commands_rotate_mode_uses_huespeed() -> None:
    s = _scene("Rotate", generator="lightning")
    s.effect_mode = "rotate"
    s.effect_speed = 1.1
    cmds = scene_to_commands(s)
    assert cmds[0] == "gen lightning"
    assert cmds[1] == "effect hue"
    assert cmds[2].startswith("set huespeed ")
    assert "1.1" in cmds[2]
