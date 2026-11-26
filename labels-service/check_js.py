#!/usr/bin/env python3
"""Syntax-check the reviewer page's inline scripts with `node --check` (CI runs this).

The page has no build step and no JS dependencies; this catches a syntax error before
the owner opens the page. Fails loudly if node is missing rather than skipping.
"""

from __future__ import annotations

import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

PAGE = Path(__file__).resolve().parent / "index.html"


def main() -> int:
    node = shutil.which("node")
    if not node:
        print("node not found: install Node.js to check the page", file=sys.stderr)
        return 2
    html = PAGE.read_text()
    if re.search(r"<(script|link)[^>]+(src|href)=[\"']?(https?:)?//", html, re.I):
        print("index.html fetches an external resource: the page must work offline", file=sys.stderr)
        return 1
    scripts = re.findall(r"<script\b[^>]*>(.*?)</script>", html, re.S | re.I)
    if not scripts:
        print("no inline scripts found in index.html", file=sys.stderr)
        return 1
    bad = 0
    with tempfile.TemporaryDirectory() as d:
        for i, src in enumerate(scripts):
            f = Path(d) / f"script{i}.js"
            f.write_text(src)
            r = subprocess.run([node, "--check", str(f)], capture_output=True, text=True)
            if r.returncode:
                bad += 1
                print(f"script #{i}: syntax error\n{r.stderr}", file=sys.stderr)
    print(f"{len(scripts)} inline scripts checked, {bad} with errors")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
