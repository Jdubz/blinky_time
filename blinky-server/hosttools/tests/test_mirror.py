"""firmware_release.mirror: the console's web-flasher mirror (blinky-console-deploy.yml), run from
hosttools/ since decoupling step 2 instead of the firmware checkout's copy. Only stable releases,
every file verified against its release manifest, and every gh call names the repo."""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import firmware_release as fr  # noqa: E402

SLUG = fr.FIRMWARE_REPO


def release(n: int, signed: bool = True) -> dict[str, Any]:
    """A release's assets and manifest, as release.yml and sign-release.yml publish them."""
    assets = {f"blinky-things-b{n}.hex": f":hex{n}\n".encode(),
              f"blinky-things-b{n}.uf2": f"UF2-{n}".encode()}
    if signed:
        assets[f"blinky-things-b{n}.bin"] = f"BIN-{n}".encode()
        assets[f"blinky-things-b{n}.otam"] = f"OTAM-{n}".encode()
    manifest = {"build": n, "version": f"b{n}-{'0' * 8}", "channel": "release", "git_sha": "0" * 40,
                "settings_version": 3, "device_version": 2,
                "files": {k: hashlib.sha256(v).hexdigest() for k, v in assets.items()}}
    return {"assets": assets, "manifest": manifest}


def fake_gh(rows: list[dict[str, Any]], rels: dict[str, dict[str, Any]],
            calls: list[tuple[str, ...]], tamper: str | None = None) -> Any:
    def gh(*args: str, capture: bool = True) -> str:
        calls.append(args)
        if args[:2] == ("release", "list"):
            return json.dumps(rows)
        if args[:2] == ("release", "view"):
            return json.dumps({"tagName": args[2], "isPrerelease": False, "isDraft": False,
                               "assets": [], "url": f"https://github.com/{SLUG}/releases/tag/{args[2]}"})
        if args[:2] == ("release", "download"):
            rel = rels[args[2]]
            out = Path(args[args.index("--dir") + 1])
            pats = [args[i + 1] for i, a in enumerate(args) if a == "--pattern"]
            for name in pats:
                body = (json.dumps(rel["manifest"]).encode() if name == "manifest.json"
                        else rel["assets"][name])
                if name == tamper:
                    body += b"!"
                (out / name).write_bytes(body)
            return ""
        raise AssertionError(args)

    return gh


def row(n: int, latest: bool = False, pre: bool = False, draft: bool = False) -> dict[str, Any]:
    return {"tagName": f"b{n}", "isPrerelease": pre, "isLatest": latest, "isDraft": draft,
            "publishedAt": f"2026-11-{n - 200:02d}T00:00:00Z"}


@pytest.fixture
def world(monkeypatch: pytest.MonkeyPatch) -> dict[str, Any]:
    rows = [row(214), row(215), row(216), row(217, latest=True), row(218, pre=True),
            row(219, draft=True), {**row(213), "tagName": "stable-b213"}, row(212)]
    rels = {f"b{n}": release(n, signed=n != 214) for n in (212, 214, 215, 216, 217, 218, 219)}
    stable = {"b214", "b215", "b216", "b217"}  # b212: a full release never promoted
    calls: list[tuple[str, ...]] = []
    monkeypatch.setattr(fr, "tag_exists", lambda tag: tag.removeprefix("stable-") in stable
                        and tag.startswith("stable-"))
    w = {"rows": rows, "rels": rels, "stable": stable, "calls": calls}
    monkeypatch.setattr(fr, "gh", fake_gh(rows, rels, calls))
    return w


def test_mirrors_the_newest_stable_releases(world: dict[str, Any], tmp_path: Path) -> None:
    out = tmp_path / "firmware"
    (out / "b100").mkdir(parents=True)  # a release no longer stable: never served again
    index = fr.mirror(out, keep=3)
    assert index["schema"] == fr.MIRROR_INDEX_SCHEMA and index["latest"] == "b217"
    assert [e["tag"] for e in index["releases"]] == ["b217", "b216", "b215"]
    assert sorted(p.name for p in out.iterdir()) == ["b215", "b216", "b217", "index.json"]
    assert json.loads((out / "index.json").read_text()) == index
    e = index["releases"][0]
    m = world["rels"]["b217"]["manifest"]
    assert e["url"].endswith("/releases/tag/b217") and e["published_at"] == "2026-11-17T00:00:00Z"
    assert {k: e[k] for k in ("build", "version", "git_sha", "settings_version", "device_version")} \
        == {k: m[k] for k in ("build", "version", "git_sha", "settings_version", "device_version")}
    for kind, f in e["files"].items():
        name = f"blinky-things-b217.{kind}"
        assert f == {"path": f"b217/{name}", "sha256": m["files"][name]}
        assert hashlib.sha256((out / f["path"]).read_bytes()).hexdigest() == f["sha256"]
    for args in world["calls"]:
        assert args[-2:] == ("-R", SLUG), args


def test_a_release_without_signed_ota_has_no_ota_files(world: dict[str, Any], tmp_path: Path) -> None:
    index = fr.mirror(tmp_path / "fw", keep=5)
    assert [e["tag"] for e in index["releases"]] == ["b217", "b216", "b215", "b214"]
    assert set(index["releases"][-1]["files"]) == {"hex", "uf2"}
    assert set(index["releases"][0]["files"]) == {"hex", "uf2", "bin", "otam"}


def test_a_bad_hash_aborts(world: dict[str, Any], tmp_path: Path,
                           monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(fr, "gh", fake_gh(world["rows"], world["rels"], world["calls"], tamper="blinky-things-b216.uf2"))
    with pytest.raises(fr.ReleaseError, match="sha256 mismatch"):
        fr.mirror(tmp_path / "fw", keep=5)


def test_latest_must_be_mirrored(world: dict[str, Any], tmp_path: Path) -> None:
    world["rows"][:] = [row(214, latest=True), row(215), row(216), row(217)]
    with pytest.raises(fr.ReleaseError, match="raise --keep"):
        fr.mirror(tmp_path / "fw", keep=2)


def test_no_stable_release_is_latest_null(world: dict[str, Any], tmp_path: Path) -> None:
    world["stable"].clear()
    index = fr.mirror(tmp_path / "fw", keep=5)
    assert index == {"schema": 1, "latest": None, "releases": []}


def test_keep_must_be_positive(tmp_path: Path) -> None:
    with pytest.raises(fr.ReleaseError, match="--keep"):
        fr.mirror(tmp_path / "fw", keep=0)


def test_a_release_withdrawn_mid_mirror_aborts(world: dict[str, Any], tmp_path: Path,
                                               monkeypatch: pytest.MonkeyPatch) -> None:
    """Listed as stable, then its marker is gone by the time download() re-checks."""
    seen: list[str] = []

    def tag_exists(tag: str) -> bool:
        seen.append(tag)
        return tag in ("stable-b216", "stable-b217") and seen.count(tag) == 1

    monkeypatch.setattr(fr, "tag_exists", tag_exists)
    with pytest.raises(fr.ReleaseError, match="b217 stopped being stable"):
        fr.mirror(tmp_path / "fw", keep=5)
