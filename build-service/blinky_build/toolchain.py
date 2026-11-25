"""The firmware toolchain pin a commit asks for, and the one baked into the worker image.

The worker image installs the toolchain at build time (blinky-firmware's
scripts/setup_toolchain.sh at FIRMWARE_TOOLCHAIN_REF) and records this fingerprint of it. A
commit builds only if its own fingerprint is equal; otherwise the build fails with
"toolchain mismatch: rebuild the worker image". The fingerprint is every pin
setup_toolchain.sh installs from:
  tools/firmware_toolchain.json   core, libraries, fqbn, arduino-cli (minus "_" comment keys)
  tools/setup_tflite_lib.sh       TFLITE_REF
  patches/*.patch                 the core patches, by sha256

  python -m blinky_build.toolchain <firmware checkout>    (prints the fingerprint JSON)
"""

import hashlib
import json
import re
import sys
from pathlib import Path

# The same pattern staging-build.yml's sed reads the pin with.
TFLITE_RE = re.compile(r'^TFLITE_REF="\$\{TFLITE_REF:-([0-9a-f]*)\}"$', re.M)


class ToolchainError(Exception):
    pass


def _sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def tflite_ref(repo):
    p = Path(repo) / "tools/setup_tflite_lib.sh"
    try:
        m = TFLITE_RE.search(p.read_text(encoding="utf-8"))
    except OSError:
        m = None
    if not m or not m.group(1):
        raise ToolchainError("TFLITE_REF pin not found in tools/setup_tflite_lib.sh")
    return m.group(1)


def toolchain_json(repo):
    p = Path(repo) / "tools/firmware_toolchain.json"
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise ToolchainError(f"tools/firmware_toolchain.json unreadable: {e}") from None


def fingerprint(repo):
    repo = Path(repo)
    tc = {k: v for k, v in toolchain_json(repo).items() if not k.startswith("_")}
    patches = {p.name: _sha256(p) for p in sorted((repo / "patches").glob("*.patch"))}
    return {"toolchain": tc, "tflite_ref": tflite_ref(repo), "patches": patches}


def fingerprint_id(fp):
    blob = json.dumps(fp, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(blob).hexdigest()[:16]


def differences(want, have):
    """Readable differences between a commit's fingerprint (want) and the image's (have)."""
    diffs = []
    wt, ht = want.get("toolchain", {}), have.get("toolchain", {})
    for k in sorted(set(wt) | set(ht)):
        if wt.get(k) != ht.get(k):
            diffs.append(f"{k}: commit {wt.get(k)!r}, image {ht.get(k)!r}")
    if want.get("tflite_ref") != have.get("tflite_ref"):
        diffs.append(f"tflite_ref: commit {want.get('tflite_ref')}, image {have.get('tflite_ref')}")
    wp, hp = want.get("patches", {}), have.get("patches", {})
    for name in sorted(set(wp) | set(hp)):
        if wp.get(name) != hp.get(name):
            w = (wp.get(name) or "absent")[:12]
            h = (hp.get(name) or "absent")[:12]
            diffs.append(f"patches/{name}: commit {w}, image {h}")
    return diffs


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    fp = fingerprint(argv[1])
    print(json.dumps({**fp, "id": fingerprint_id(fp)}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
