"""The box's client for the build service on the build host (docs/DEV_LOOP.md): fetch a dev package
and verify it as a staging artifact is verified.

The service builds owner commits of blinky-firmware into the same dev package as
staging-build.yml (``firmware_release.py package --dev``: ``manifest.json``, hex, UF2). The box
asks it by build id (the box's ``scripts/testbox/dev.py`` submitted the build), then:

1. ``GET /v1/builds/{id}``: the build must be ``done``, of a full commit sha;
2. ``GET /v1/builds/{id}/package``: a ``.tar.gz`` of flat regular files only (``manifest.json``,
   ``*.hex``, ``*.uf2``), unpacked into its own emptied directory, size-capped;
3. ``hosttools/firmware_release.py verify-dev-package``: the verifier download_staging uses
   (the dev-channel manifest of exactly that sha, version ``b<N>-dev-<sha8>``, the sha256 of
   every file, nothing unlisted).

Configuration (environment, else ``~/.config/blinky/build.env``, as the box's dev.py reads it):
  BLINKY_BUILD_URL    the service's base URL (no default: the repo is public and names no host)
  BLINKY_BUILD_TOKEN  its bearer token
Unconfigured, ``POST /api/dev/deploy`` answers 503 and nothing else changes.
"""

from __future__ import annotations

import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from ..paths import hosttools_dir

ENV_FILE = Path.home() / ".config" / "blinky" / "build.env"
BUILD_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
# A package is a few hundred KB (hex + UF2 of a 1 MB flash image). Anything near this is wrong.
MAX_PACKAGE_BYTES = 32 * 1024 * 1024
MAX_PACKAGE_FILES = 16  # manifest, hex, UF2, and room for the signed release's bin/otam
PACKAGE_FILE_RE = re.compile(r"^(manifest\.json|[A-Za-z0-9._-]+\.(hex|uf2))$")
HTTP_TIMEOUT_S = 60.0
VERIFY_TIMEOUT_S = 60.0


class BuildServiceError(RuntimeError):
    """The build service can't be used, or its answer isn't a verified dev package. ``code``:
    the service's HTTP status when it answered with an error (404: it has no such build)."""

    def __init__(self, msg: str, code: int | None = None) -> None:
        super().__init__(msg)
        self.code = code


def _env_file(path: Path) -> dict[str, str]:
    """KEY=VALUE lines (``export`` and quotes allowed), as a shell would source them."""
    out: dict[str, str] = {}
    try:
        text = path.read_text()
    except OSError:
        return out
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.removeprefix("export ").partition("=")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        out[key.strip()] = value
    return out


def config() -> tuple[str, str] | None:
    """(base URL, token), or None when the service isn't configured on this machine."""
    file = _env_file(ENV_FILE)
    url = os.environ.get("BLINKY_BUILD_URL") or file.get("BLINKY_BUILD_URL", "")
    token = os.environ.get("BLINKY_BUILD_TOKEN") or file.get("BLINKY_BUILD_TOKEN", "")
    if not url or not token:
        return None
    return url.rstrip("/"), token


def disabled_reason() -> str:
    return (
        "the build service isn't configured on this server: set BLINKY_BUILD_URL and "
        f"BLINKY_BUILD_TOKEN (environment or {ENV_FILE})"
    )


def _get(path: str, *, raw: bool = False) -> Any:
    cfg = config()
    if cfg is None:
        raise BuildServiceError(disabled_reason())
    url, token = cfg
    req = urllib.request.Request(url + path, headers={"Authorization": f"Bearer {token}"})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_S) as r:
            data = r.read(MAX_PACKAGE_BYTES + 1)
    except urllib.error.HTTPError as e:
        raise BuildServiceError(f"build service: GET {path} -> {e.code} {e.reason}", e.code) from e
    except (urllib.error.URLError, OSError) as e:
        raise BuildServiceError(f"build service unreachable: {e}") from e
    if len(data) > MAX_PACKAGE_BYTES:
        raise BuildServiceError(f"build service: {path} is over {MAX_PACKAGE_BYTES} bytes")
    if raw:
        return data
    try:
        return json.loads(data)
    except ValueError as e:
        raise BuildServiceError(f"build service: {path} isn't JSON") from e


def build(build_id: str) -> dict[str, Any]:
    """The build's record (``GET /v1/builds/{id}``)."""
    if not BUILD_ID_RE.match(build_id):
        raise BuildServiceError(f"not a build id: {build_id!r}")
    rec = _get(f"/v1/builds/{build_id}")
    if not isinstance(rec, dict):
        raise BuildServiceError(f"build service: build {build_id} isn't an object")
    return rec


def unpack(data: bytes, out_dir: Path) -> list[str]:
    """Unpack a package into ``out_dir`` (emptied first): flat regular files whose names are a
    manifest, hex or UF2 only; a ``./`` prefix is allowed. Anything else refuses the package."""
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    names: list[str] = []
    total = 0
    try:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
            for m in tar:  # streamed: a bad member stops it before the rest is read
                name = m.name.removeprefix("./")
                if m.isdir() and name in ("", "."):
                    continue
                if not m.isfile() or not PACKAGE_FILE_RE.match(name) or name in names:
                    raise BuildServiceError(f"the package holds {m.name!r}: refusing it")
                total += m.size
                if total > MAX_PACKAGE_BYTES or len(names) >= MAX_PACKAGE_FILES:
                    # A gzip bomb or a pile of files, not a firmware package.
                    raise BuildServiceError(
                        f"the package unpacks to over {MAX_PACKAGE_BYTES} bytes or "
                        f"{MAX_PACKAGE_FILES} files: refusing it"
                    )
                f = tar.extractfile(m)
                if f is None:
                    raise BuildServiceError(f"the package's {m.name!r} can't be read")
                (out_dir / name).write_bytes(f.read())
                names.append(name)
    except (tarfile.TarError, EOFError, OSError) as e:
        raise BuildServiceError(f"the package isn't a readable .tar.gz: {e}") from e
    if "manifest.json" not in names:
        raise BuildServiceError("the package has no manifest.json")
    return names


def verify(pkg_dir: Path, sha: str) -> dict[str, Any]:
    """``hosttools/firmware_release.py verify-dev-package``: the checks download_staging runs on
    a staging artifact (one verifier for both), every file in the package included."""
    tools = hosttools_dir()
    args = [sys.executable, str(tools / "firmware_release.py"), "verify-dev-package"]
    args += ["--sha", sha, "--dir", str(pkg_dir)]
    try:
        r = subprocess.run(
            args, cwd=tools, capture_output=True, text=True, timeout=VERIFY_TIMEOUT_S
        )
    except subprocess.TimeoutExpired as e:
        raise BuildServiceError("verify-dev-package timed out") from e
    if r.returncode != 0:
        msg = (r.stderr or r.stdout).strip().splitlines()
        raise BuildServiceError(msg[-1] if msg else f"verify-dev-package exited {r.returncode}")
    try:
        out: dict[str, Any] = json.loads(r.stdout)
    except ValueError as e:
        raise BuildServiceError(f"verify-dev-package printed no JSON: {r.stdout[:200]!r}") from e
    if out.get("sha") != sha or not out.get("version") or not out.get("hex"):
        raise BuildServiceError(f"verify-dev-package's answer isn't this package: {out!r}")
    # Verified as the dev-channel build of exactly sha: a dev image, never stable.
    return {**out, "tag": f"dev-{sha[:8]}", "channel": "dev", "stable": False}


def fetch(build_id: str, out_dir: Path) -> dict[str, Any]:
    """The verified dev package of a finished build, unpacked under ``out_dir/<build_id>``.
    Returns deploy_pipeline's release record (tag, version, hex, sha, channel, stable False)."""
    rec = build(build_id)
    if rec.get("state") != "done":
        why = f": {rec.get('error')}" if rec.get("error") else ""
        raise BuildServiceError(f"build {build_id} is {rec.get('state')!r}, not done{why}")
    sha = str(rec.get("sha") or "")
    if not SHA_RE.match(sha):
        raise BuildServiceError(f"build {build_id} names no full commit sha: {sha!r}")
    data = _get(f"/v1/builds/{build_id}/package", raw=True)
    pkg_dir = out_dir / build_id
    unpack(data, pkg_dir)
    rel = verify(pkg_dir, sha)
    if rel.get("channel") != "dev" or rel.get("stable") is not False or rel.get("sha") != sha:
        raise BuildServiceError(f"build {build_id} is not a dev package of {sha[:12]}: {rel!r}")
    if rec.get("version") and rec["version"] != rel.get("version"):
        raise BuildServiceError(
            f"build {build_id}: the service says {rec['version']}, its manifest "
            f"{rel.get('version')}"
        )
    return {**rel, "build_id": build_id}
