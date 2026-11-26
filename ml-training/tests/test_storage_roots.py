"""Config storage roots (#286): ${BLINKY_DATA} / ${BLINKY_WORK} expansion, and no defaults.

The roots once fell back to the build host's layout (/mnt/storage/blinky-ml-data, /mnt/nvme),
retired on 2026-07-10 and present on no machine (docs/ML_STORAGE_LAYOUT.md). An unset root now
fails, naming the variable, instead of becoming a missing file under a dead mount.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import pytest

ML = Path(__file__).resolve().parents[1]
REPO = ML.parent
sys.path.insert(0, str(ML))

from scripts.audio import (  # noqa: E402
    STORAGE_ROOTS,
    StorageRootUnset,
    expand_roots,
    load_config,
    root_path,
    storage_root,
)


@pytest.fixture
def roots(monkeypatch):
    monkeypatch.setenv("BLINKY_DATA", "/vol/data")
    monkeypatch.setenv("BLINKY_WORK", "/vol/work")


@pytest.mark.parametrize("name", sorted(STORAGE_ROOTS))
@pytest.mark.parametrize("value", [None, ""])
def test_an_unset_root_fails_naming_it(monkeypatch, name, value):
    for n in STORAGE_ROOTS:
        monkeypatch.setenv(n, "/vol/x")
    if value is None:
        monkeypatch.delenv(name)
    else:
        monkeypatch.setenv(name, value)
    with pytest.raises(StorageRootUnset, match=rf"\${name} is unset"):
        storage_root(name)
    with pytest.raises(StorageRootUnset, match=rf"\${name} is unset"):
        expand_roots({"a": [f"${{{name}}}/labels"]})


def test_a_config_naming_an_unset_root_fails_to_load(monkeypatch):
    monkeypatch.delenv("BLINKY_DATA", raising=False)
    monkeypatch.setenv("BLINKY_WORK", "/vol/work")
    with pytest.raises(StorageRootUnset, match=r"\$BLINKY_DATA is unset"):
        load_config(str(ML / "configs" / "base.yaml"))


def test_environment_wins(monkeypatch):
    monkeypatch.setenv("BLINKY_DATA", "/vol/blinky/")
    monkeypatch.setenv("BLINKY_WORK", "/scratch")
    assert expand_roots({"x": {"y": "${BLINKY_DATA}/audio"}, "z": "${BLINKY_WORK}/out", "n": 3}) == \
        {"x": {"y": "/vol/blinky/audio"}, "z": "/scratch/out", "n": 3}


def test_unknown_root_fails_loud(roots):
    with pytest.raises(ValueError, match="BLINKY_DTA"):
        expand_roots({"x": "${BLINKY_DTA}/a"})


def test_a_script_default_fails_only_when_used(monkeypatch, capsys):
    """root_path: a default naming a root resolves when the option is left out, and an unset
    root stops the script naming the option and the variable; a given option needs no root."""
    p = argparse.ArgumentParser()
    p.add_argument("--labels", type=root_path, default="${BLINKY_DATA}/labels")
    monkeypatch.delenv("BLINKY_DATA", raising=False)
    assert p.parse_args(["--labels", "/given"]).labels == Path("/given")
    with pytest.raises(SystemExit):
        p.parse_args([])
    err = capsys.readouterr().err
    assert "--labels" in err and "$BLINKY_DATA is unset" in err
    monkeypatch.setenv("BLINKY_DATA", "/vol/data")
    assert p.parse_args([]).labels == Path("/vol/data/labels")


def test_no_config_names_a_machine_mount(roots):
    offenders = [p.name for p in sorted((ML / "configs").glob("*.yaml"))
                 if "/mnt/" in str(load_config(str(p)))]
    assert offenders == [], f"configs still hardcode a mount: {offenders}"


def test_nothing_live_names_the_retired_layout():
    """Tracked scripts, configs and docs outside the archives name the roots, never the
    retired mounts. ML_STORAGE_LAYOUT.md and this file tell their history and are the
    exceptions."""
    retired = ("/mnt/" + "storage", "/mnt/" + "nvme")
    tracked = subprocess.run(
        ["git", "ls-files", "ml-training", "scripts/disk-audit.sh", ".gitignore"],
        cwd=REPO, capture_output=True, text=True, check=True,
    ).stdout.split()
    assert "ml-training/scripts/audio.py" in tracked  # the listing worked
    allowed = {"docs/ML_STORAGE_LAYOUT.md", "ml-training/tests/test_storage_roots.py"}
    offenders = []
    for rel in tracked:
        if rel in allowed or rel.startswith("ml-training/archive/"):
            continue
        try:
            text = (REPO / rel).read_text()
        except (UnicodeDecodeError, FileNotFoundError):
            continue
        if any(r in text for r in retired):
            offenders.append(rel)
    assert offenders == [], f"files name the retired build-host layout: {offenders}"


def test_no_config_exports_outside_its_run(roots):
    """export_tflite.py writes the header into its --output-dir, named by export.header_name.
    A config names a file, never a path: the old export.output_header pointed into the
    firmware tree, and the repos share no source now (2026-11-24 decoupling)."""
    offenders = []
    for p in sorted((ML / "configs").glob("*.yaml")):
        export = load_config(str(p)).get("export", {})
        name = export.get("header_name", "")
        if "output_header" in export or not name.endswith(".h") or Path(name).name != name:
            offenders.append(p.name)
    assert offenders == [], f"configs export outside the run: {offenders}"
