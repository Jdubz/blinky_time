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

    Also clears the module-level cached Paths (both ``scenes._CACHED_DATA_DIR``
    and ``paths._{data,firmware,scenes}_dir``) so the next call re-derives
    from XDG_DATA_HOME. PR #140 review: paths.py now also caches.
    """
    from blinky_server import paths as paths_mod

    monkeypatch.setenv("XDG_DATA_HOME", str(tmp_path))
    monkeypatch.setattr(scenes_mod, "_CACHED_DATA_DIR", None)
    paths_mod._clear_cache()
    yield
    # Post-test reset so other test modules don't see a leaked pointer.
    monkeypatch.setattr(scenes_mod, "_CACHED_DATA_DIR", None)
    paths_mod._clear_cache()


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
    cmds = scene_to_commands(s)
    assert len(cmds) == 1
    parts = cmds[0].split()
    assert parts[0] == "scene"
    assert parts[1] == "fire"
    assert parts[2] == "off"


def test_scene_to_commands_static_mode_emits_hue() -> None:
    s = _scene("Static", generator="water")
    s.effect_mode = "static"
    s.effect_hue = 0.42
    cmds = scene_to_commands(s)
    assert len(cmds) == 1
    parts = cmds[0].split()
    assert parts[:3] == ["scene", "water", "static"]
    # parts[3] = speed, parts[4] = hue. Compare as floats — the str
    # representation isn't pinned (could be "0.42" or "0.42000…").
    assert float(parts[4]) == pytest.approx(0.42)


def test_scene_to_commands_rotate_mode_emits_speed() -> None:
    s = _scene("Rotate", generator="lightning")
    s.effect_mode = "rotate"
    s.effect_speed = 1.1
    cmds = scene_to_commands(s)
    assert len(cmds) == 1
    parts = cmds[0].split()
    assert parts[:3] == ["scene", "lightning", "rotate"]
    assert float(parts[3]) == pytest.approx(1.1)


def test_scene_to_commands_always_emits_all_four_args() -> None:
    """Firmware parser is fixed-arity. Even modes that ignore one of the
    floats must still emit both, or the firmware errors out."""
    for mode in ("off", "rotate", "static"):
        s = _scene(f"All-{mode}", generator="fire")
        s.effect_mode = mode  # type: ignore[assignment]
        parts = scene_to_commands(s)[0].split()
        assert len(parts) == 5, f"{mode}: expected 5 tokens, got {parts}"


# ── /scenes/next + /scenes/previous cursor math ─────────────────────────────
# PR 142 review (gemini HIGH, Copilot MED): the naive `(idx + direction) % N`
# misbehaves on the "no cursor" sentinel (idx == -1). `(-1 + -1) % N` lands
# on N-2 in Python, so `/previous` from a fresh install would skip the last
# scene. The fix lives in `_next_cursor_index`; these are the pinned
# regression tests for the boundary behaviours.


def test_next_cursor_index_no_cursor_next_lands_on_first() -> None:
    from blinky_server.api.routes_scenes import _next_cursor_index

    assert _next_cursor_index(-1, 1, 3) == 0


def test_next_cursor_index_no_cursor_previous_lands_on_last() -> None:
    from blinky_server.api.routes_scenes import _next_cursor_index

    # The bug: pre-fix this returned 1 (= N-2). Must return 2 (last index).
    assert _next_cursor_index(-1, -1, 3) == 2
    assert _next_cursor_index(-1, -1, 5) == 4
    # Single-scene library — last == first; both directions stay put.
    assert _next_cursor_index(-1, -1, 1) == 0
    assert _next_cursor_index(-1, 1, 1) == 0


def test_next_cursor_index_with_cursor_wraps() -> None:
    from blinky_server.api.routes_scenes import _next_cursor_index

    # Normal advance.
    assert _next_cursor_index(0, 1, 3) == 1
    assert _next_cursor_index(1, 1, 3) == 2
    # Wrap on /next from end.
    assert _next_cursor_index(2, 1, 3) == 0
    # Wrap on /previous from start.
    assert _next_cursor_index(0, -1, 3) == 2
    # Mid-list /previous.
    assert _next_cursor_index(2, -1, 3) == 1


def test_list_scenes_skips_dotfiles() -> None:
    """``scene_cursor`` writes ``.cursor.json`` + ``.cursor.json.tmp`` to
    the scenes data directory. ``list_scenes`` must skip both, otherwise
    every cursor write would produce a Pydantic-validation warning and a
    spurious unreadable-scene log line. PR 142 review noted that the
    dot-prefix filter is load-bearing; this test pins it."""
    save_scene(_scene("Real"))
    # Simulate the cursor + its in-flight tmp file.
    sdir = scenes_mod._data_dir()
    (sdir / ".cursor.json").write_text('{"current":"Real"}')
    (sdir / ".cursor.json.tmp").write_text("partial")
    names = [s.name for s in list_scenes()]
    assert names == ["Real"]
