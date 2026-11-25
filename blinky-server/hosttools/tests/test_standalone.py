"""blinky-server's host tools stand alone: nothing in them reaches for a blinky-firmware checkout
(2026-11-24 decoupling). The server runs them from blinky-server/hosttools/ with no firmware/
submodule present."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

HOSTTOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HOSTTOOLS))
import firmware_release as fr  # noqa: E402
import pre_upload_check  # noqa: E402
import uf2_upload  # noqa: E402

TOOLS = sorted(p for p in HOSTTOOLS.glob("*.py"))


def test_the_tools_are_all_here() -> None:
    names = {p.name for p in TOOLS}
    assert {"firmware_release.py", "uf2_upload.py", "pre_upload_check.py", "serial_lock.py",
            "usb_reset.py"} <= names
    assert (HOSTTOOLS / "uf2conv" / "uf2conv.py").is_file()
    assert (HOSTTOOLS / "uf2conv" / "LICENSE.txt").is_file()


@pytest.mark.parametrize("path", TOOLS, ids=lambda p: p.name)
def test_no_tool_resolves_paths_above_hosttools(path: Path) -> None:
    """A firmware checkout used to sit at <repo>/ next to tools/: parent.parent lookups were how
    the tools found its sketch, build folder and toolchain. None may remain."""
    code = path.read_text(encoding="utf-8")
    assert not re.search(r"\.parent\.parent|parents\[[1-9]\]", code), path.name


def test_uf2conv_is_the_vendored_copy() -> None:
    assert uf2_upload.UF2CONV_PATH == HOSTTOOLS / "uf2conv" / "uf2conv.py"


def test_the_source_constant_check_says_it_was_skipped() -> None:
    """With no firmware source here, the CLI's SafetyTest.h comparison is reported as SKIPPED,
    not as a quiet pass. The flash path (uf2_upload.validate_hex) never runs it."""
    results = pre_upload_check.validate_source_constants()
    assert len(results) == 1 and results[0][1] is True and results[0][2].startswith("SKIPPED")


def test_gh_runs_without_a_cwd_and_every_release_call_names_the_repo(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[tuple[list[str], dict[str, Any]]] = []

    def fake_run(cmd: list[str], **kw: Any) -> subprocess.CompletedProcess[str]:
        calls.append((cmd, kw))
        if cmd[1:3] == ["release", "list"]:
            out = json.dumps([{"tagName": "b217", "isPrerelease": False, "isLatest": True,
                               "isDraft": False}])
        elif cmd[1:3] == ["release", "view"]:
            out = json.dumps({"tagName": "b217", "isPrerelease": False, "isDraft": False,
                              "assets": [], "url": "u"})
        else:  # gh api .../git/ref/tags/stable-b217
            out = "{}"
        return subprocess.CompletedProcess(cmd, 0, out, "")

    monkeypatch.setattr(fr.shutil, "which", lambda _: "/usr/bin/gh")
    monkeypatch.setattr(fr.subprocess, "run", fake_run)
    assert fr.latest_stable_tag() == "b217"
    assert calls and all("cwd" not in kw for _, kw in calls)
    for cmd, _ in calls:
        if cmd[1] == "api":
            assert cmd[2].startswith(f"repos/{fr.FIRMWARE_REPO}/")
        else:
            assert cmd[-2:] == ["-R", fr.FIRMWARE_REPO]


def test_the_repo_defaults_to_blinky_firmware() -> None:
    if "BLINKY_FIRMWARE_REPO" not in os.environ:
        assert fr.FIRMWARE_REPO == "Jdubz/blinky-firmware"
