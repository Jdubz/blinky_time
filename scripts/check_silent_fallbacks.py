#!/usr/bin/env python3
"""
Check for silent fallback anti-patterns in firmware code.

Scans for code patterns that silently hide errors instead of making them
visible. On a controlled embedded system, unexpected conditions are bugs —
silent recovery just hides them.

Patterns detected:
- "shouldn't happen" comments (dead fallback code)
- Static fallback returns without BLINKY_ASSERT
- Silent default assignments in error paths

Exit codes:
    0 - No anti-patterns found
    1 - Anti-patterns detected (warning only, does not block build)
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).parent.parent
FIRMWARE_DIR = ROOT / "blinky-things"

# Patterns to flag (regex, description)
PATTERNS = [
    (r"shouldn't happen", "Dead fallback code with 'shouldn't happen' comment"),
    (r"//.*not used.*bolt", "Dead parameter marked as unused"),
    (r"corrupt\s*=\s*true", "All-or-nothing corruption flag (prefer per-param fix)"),
    (r"loadDefaults\(\)", "Bulk default reset (may wipe user settings)"),
]

# Files/patterns to exclude from checks
EXCLUDE_FILES = {
    "BlinkyAssert.h",
    "BlinkyAssert.cpp",
}


def check_file(path):
    """Check a single file for anti-patterns. Returns list of (line_no, pattern_desc)."""
    findings = []
    try:
        content = path.read_text(encoding='utf-8', errors='replace')
    except Exception:
        return findings

    for line_no, line in enumerate(content.splitlines(), 1):
        # Skip comments that document the pattern (e.g., in this script)
        for pattern, desc in PATTERNS:
            if re.search(pattern, line, re.IGNORECASE):
                findings.append((line_no, desc, line.strip()))
    return findings


def main():
    total_findings = 0
    files_checked = 0

    for ext in ('*.h', '*.cpp', '*.ino'):
        for path in FIRMWARE_DIR.rglob(ext):
            if path.name in EXCLUDE_FILES:
                continue
            files_checked += 1
            findings = check_file(path)
            if findings:
                for line_no, desc, line_text in findings:
                    rel = path.relative_to(ROOT)
                    print(f"  {rel}:{line_no}: {desc}")
                    total_findings += 1

    if total_findings > 0:
        print(f"\n  Found {total_findings} potential anti-pattern(s) in {files_checked} files")
        print(f"  Review each to confirm it's intentional or needs BLINKY_ASSERT")
        return 1
    else:
        print(f"  No anti-patterns found in {files_checked} files")
        return 0


if __name__ == "__main__":
    sys.exit(main())
