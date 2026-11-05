"""firmware_release.download_staging (#349): only a verified staging build of exactly that commit."""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import firmware_release as fr  # noqa: E402

SHA = "ab" * 20
SLUG = "Jdubz/blinky_time"


def fake_gh(tmp: Path, **over: Any) -> Any:
    """gh as GitHub would answer for a good staging build of SHA; ``over`` breaks one part."""
    hexb, uf2b = b":00000001FF\n", b"UF2"
    manifest = {"build": 216, "version": f"b216-dev-{SHA[:8]}", "channel": "dev", "git_sha": SHA,
                "files": {"blinky-things-b216.hex": hashlib.sha256(hexb).hexdigest(),
                          "blinky-things-b216.uf2": hashlib.sha256(uf2b).hexdigest()}}
    manifest.update(over.get("manifest", {}))
    run = {"path": fr.STAGING_WORKFLOW, "event": "workflow_run", "conclusion": "success",
           "repository": {"full_name": SLUG}, **over.get("run", {})}
    arts = over.get("artifacts", [{"expired": False, "created_at": "2026-11-04T00:00:00Z",
                                   "workflow_run": {"id": 9}}])
    compare = {"status": over.get("compare", "ahead")}

    def gh(*args: str, capture: bool = True) -> str:
        if args[:2] == ("repo", "view"):
            return json.dumps({"nameWithOwner": SLUG})
        if args[0] == "api" and "actions/artifacts" in args[1]:
            assert f"name=staging-firmware-{SHA}" in args[1]
            return json.dumps({"artifacts": arts})
        if args[0] == "api" and "actions/runs/9" in args[1]:
            return json.dumps(run)
        if args[0] == "api" and "/compare/" in args[1]:
            return json.dumps(compare)
        if args[:2] == ("run", "download"):
            out = Path(args[args.index("--dir") + 1])
            (out / "manifest.json").write_text(json.dumps(manifest))
            (out / "blinky-things-b216.hex").write_bytes(over.get("hex", hexb))
            (out / "blinky-things-b216.uf2").write_bytes(uf2b)
            return ""
        raise AssertionError(args)

    return gh


def test_a_good_staging_build_downloads(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(fr, "gh", fake_gh(tmp_path))
    keep = tmp_path / "unrelated.txt"
    keep.write_text("x")
    r = fr.download_staging(SHA, tmp_path)
    assert r["manifest"]["version"] == f"b216-dev-{SHA[:8]}" and r["hex"].is_file()
    assert r["hex"].parent == tmp_path / SHA and keep.exists()  # nothing else under out is touched


@pytest.mark.parametrize("over,why", [
    ({"artifacts": []}, "no staging build"),
    ({"artifacts": [{"expired": True, "created_at": "x", "workflow_run": {"id": 9}}]}, "no staging build"),
    ({"run": {"path": ".github/workflows/release.yml"}}, "not built by"),
    ({"run": {"event": "push"}}, "not built by"),
    ({"run": {"repository": {"full_name": "someone/fork"}}}, "another repository"),
    ({"run": {"conclusion": "failure"}}, "concluded failure"),
    ({"compare": "diverged"}, "not on staging"),
    ({"manifest": {"channel": "release"}}, "not the dev build"),
    ({"manifest": {"git_sha": "cd" * 20}}, "not the dev build"),
    ({"manifest": {"version": "b216-abababab"}}, "not the dev build"),
    ({"hex": b"tampered"}, "sha256 mismatch"),
])
def test_anything_else_is_refused(tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
                                  over: dict[str, Any], why: str) -> None:
    monkeypatch.setattr(fr, "gh", fake_gh(tmp_path, **over))
    with pytest.raises(fr.ReleaseError, match=why):
        fr.download_staging(SHA, tmp_path)


def test_a_short_sha_is_refused() -> None:
    with pytest.raises(fr.ReleaseError, match="full commit sha"):
        fr.download_staging(SHA[:12], "/nonexistent")
