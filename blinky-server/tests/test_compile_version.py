"""compile_firmware regenerates Version.h first and refuses to build without it (#213)."""

from __future__ import annotations

from pathlib import Path

from blinky_server.firmware import compile as compile_mod


def test_missing_build_number_is_an_error(tmp_path: Path) -> None:
    err = compile_mod._regenerate_version_header(tmp_path)
    assert err is not None and "BUILD_NUMBER" in err


def test_non_integer_build_number_is_an_error(tmp_path: Path) -> None:
    (tmp_path / compile_mod.SKETCH_DIR).mkdir()
    (tmp_path / compile_mod.SKETCH_DIR / "BUILD_NUMBER").write_text("b12\n", encoding="utf-8")
    err = compile_mod._regenerate_version_header(tmp_path)
    assert err is not None and "not an integer" in err


def test_compile_refuses_when_version_generation_fails(monkeypatch, tmp_path: Path) -> None:
    monkeypatch.setattr(compile_mod, "find_arduino_cli", lambda: "arduino-cli")
    monkeypatch.setattr(compile_mod, "find_project_root", lambda: str(tmp_path))
    ran = []
    monkeypatch.setattr(compile_mod.subprocess, "run", lambda *a, **k: ran.append(a))
    result = compile_mod.compile_firmware()
    assert result["status"] == "error"
    assert "BUILD_NUMBER" in result["message"]
    assert ran == []  # never reached arduino-cli
