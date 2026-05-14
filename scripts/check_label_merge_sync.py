#!/usr/bin/env python3
"""Enforce parity between the two copies of ``onset_label_merge.py``.

Two copies exist intentionally:

* ``ml-training/scripts/onset_label_merge.py`` — used by the training pipeline.
* ``blinky-server/blinky_server/testing/onset_label_merge.py`` — used by the
  blinky-server validation harness, which runs on blinkyhost (a Raspberry Pi)
  where the ml-training package is intentionally not installed.

The two files MUST stay byte-identical except for their module docstring
(which is allowed to differ so each can describe itself as the canonical
copy for its own consumer).

This check enforces that constraint at commit/pre-push time. Run with no
arguments; exit code is 0 on parity and 1 on divergence (with a unified diff
printed to stderr).
"""
from __future__ import annotations

import ast
import difflib
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FILE_A = REPO / "ml-training" / "scripts" / "onset_label_merge.py"
FILE_B = REPO / "blinky-server" / "blinky_server" / "testing" / "onset_label_merge.py"


def _strip_module_docstring(src: str) -> str:
    """Return ``src`` with its leading module docstring AND any directly-
    following blank lines removed. The two source files we're comparing may
    have differently-sized docstrings (one declares itself canonical, the
    other the mirror) plus differently-sized blank-line padding after them
    — we only care about whether the rest of the code matches.
    """
    tree = ast.parse(src)
    if not tree.body:
        return src
    first = tree.body[0]
    if not (
        isinstance(first, ast.Expr)
        and isinstance(first.value, ast.Constant)
        and isinstance(first.value.value, str)
    ):
        # No module docstring — pass through unchanged.
        return src

    lines = src.splitlines(keepends=True)
    # Drop the docstring lines AND any blank lines immediately after.
    drop_until = first.end_lineno  # one past last docstring line, 0-indexed
    while drop_until < len(lines) and not lines[drop_until].strip():
        drop_until += 1
    remaining = lines[drop_until:]
    return "".join(remaining)


def main() -> int:
    for p in (FILE_A, FILE_B):
        if not p.exists():
            print(f"ERROR: {p} does not exist", file=sys.stderr)
            return 2

    a_src = FILE_A.read_text(encoding="utf-8")
    b_src = FILE_B.read_text(encoding="utf-8")

    a_stripped = _strip_module_docstring(a_src)
    b_stripped = _strip_module_docstring(b_src)

    if a_stripped == b_stripped:
        print(f"OK: {FILE_A.relative_to(REPO)} and {FILE_B.relative_to(REPO)} match (module docstring ignored)")
        return 0

    print(
        f"ERROR: {FILE_A.relative_to(REPO)} and {FILE_B.relative_to(REPO)} have diverged.",
        file=sys.stderr,
    )
    print(
        "Two copies must stay byte-identical except for the module docstring.",
        file=sys.stderr,
    )
    print(
        "Background: the duplication is deliberate (blinky-server on the Pi can't",
        file=sys.stderr,
    )
    print(
        "carry ml-training's heavy deps). Any logic change must be applied to BOTH",
        file=sys.stderr,
    )
    print("files in the same commit. Unified diff follows:", file=sys.stderr)
    print("", file=sys.stderr)
    for line in difflib.unified_diff(
        a_stripped.splitlines(keepends=True),
        b_stripped.splitlines(keepends=True),
        fromfile=str(FILE_A.relative_to(REPO)),
        tofile=str(FILE_B.relative_to(REPO)),
        n=3,
    ):
        sys.stderr.write(line)
    return 1


if __name__ == "__main__":
    sys.exit(main())
