"""Config storage roots (#286): ${BLINKY_DATA} / ${BLINKY_WORK} expansion."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

ML = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ML))

from scripts.audio import STORAGE_ROOT_DEFAULTS, expand_roots, load_config  # noqa: E402


def test_defaults_are_the_devtop_layout(monkeypatch):
    monkeypatch.delenv("BLINKY_DATA", raising=False)
    monkeypatch.delenv("BLINKY_WORK", raising=False)
    cfg = expand_roots({"a": "${BLINKY_DATA}/labels", "b": ["${BLINKY_WORK}/processed", 3], "c": "plain"})
    assert cfg == {"a": STORAGE_ROOT_DEFAULTS["BLINKY_DATA"] + "/labels",
                   "b": [STORAGE_ROOT_DEFAULTS["BLINKY_WORK"] + "/processed", 3], "c": "plain"}


def test_environment_wins(monkeypatch):
    monkeypatch.setenv("BLINKY_DATA", "/vol/blinky/")
    monkeypatch.setenv("BLINKY_WORK", "/scratch")
    assert expand_roots({"x": {"y": "${BLINKY_DATA}/audio"}, "z": "${BLINKY_WORK}/out"}) == \
        {"x": {"y": "/vol/blinky/audio"}, "z": "/scratch/out"}


def test_unknown_root_fails_loud():
    with pytest.raises(ValueError, match="BLINKY_DTA"):
        expand_roots({"x": "${BLINKY_DTA}/a"})


def test_no_config_names_a_machine_mount(monkeypatch):
    monkeypatch.setenv("BLINKY_DATA", "/vol/data")
    monkeypatch.setenv("BLINKY_WORK", "/vol/work")
    offenders = [p.name for p in sorted((ML / "configs").glob("*.yaml"))
                 if "/mnt/" in str(load_config(str(p)))]
    assert offenders == [], f"configs still hardcode a mount: {offenders}"
