"""Shared fixtures for the labels service's tests: a tiny corpus (fake mp3s, seed labels, a manifest)
and the service's modules on the path. No network; git only for the publisher tests."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import pytest

SERVICE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SERVICE))
sys.path.insert(0, str(Path(__file__).resolve().parent))

DUR = 10.0
KICKS = [0.5 + i for i in range(10)]
SNARES = [1.0 + i for i in range(9)]


def write_json(path: Path, doc) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc))
    return path


def kw(kicks, snares, **extra):
    ons = [{"time": t, "type": "kick", "weight": 1.0} for t in kicks]
    ons += [{"time": t, "type": "snare", "weight": 1.0} for t in snares]
    return {"onsets": sorted(ons, key=lambda o: o["time"]), **extra}


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def git(cwd, *args, input=None):
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", *args],
                          cwd=cwd, check=True, capture_output=True, text=True, input=input).stdout


@pytest.fixture
def edm(tmp_path):
    d = tmp_path / "corpus" / "edm"
    d.mkdir(parents=True)
    for stem in ("a", "b", "c"):
        (d / f"{stem}.mp3").write_bytes(f"fake mp3 {stem}".encode() * 50)
        write_json(d / "drums_adtof" / f"{stem}.kick_weighted.json",
                   kw(KICKS, SNARES, generator={"algorithm": "adtof_frame_rnn"}))
        write_json(d / "kick_weighted" / f"{stem}.kick_weighted.json", kw(KICKS + SNARES, []))
    write_json(d / "track_manifest.json", {s: {"duration": DUR} for s in ("a", "b", "c")})
    return d
