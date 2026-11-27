"""Fixtures: a local stand-in for GitHub (a bare repo with branches and a fork PR ref), a
fake firmware tree whose build and package steps are stubs, and a running API server."""

import json
import os
import secrets
import stat
import subprocess
import sys
import textwrap
import threading
import urllib.error
import urllib.request
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from blinky_build import api, toolchain  # noqa: E402

REPO = "Jdubz/blinky-firmware"
TOKEN = "t" * 40

GIT_ENV = {
    "GIT_AUTHOR_NAME": "Owner",
    "GIT_AUTHOR_EMAIL": "owner@example.invalid",
    "GIT_COMMITTER_NAME": "Owner",
    "GIT_COMMITTER_EMAIL": "owner@example.invalid",
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_CONFIG_GLOBAL": "/dev/null",
}


def run_git(cwd, *args):
    env = {**os.environ, **GIT_ENV}
    r = subprocess.run(["git", "-c", "init.defaultBranch=main", *args], cwd=cwd, env=env,
                       capture_output=True, text=True, check=True)
    return r.stdout.strip()


TOOLCHAIN = {
    "_comment": "test pin",
    "board_index_url": "https://example.invalid/index.json",
    "core": "Seeeduino:nrf52@1.1.10",
    "libraries": ["Adafruit NeoPixel@1.15.1", "ArduinoJson@7.4.2"],
    "fqbn": "Seeeduino:nrf52:xiaonRF52840Sense",
    "arduino_cli": "1.3.1",
}

# The stub build.sh does what scripts/build.sh --dev leaves behind: Version.h with
# b<N>-dev-<sha8>, the hex and libraries.json in the output directory.
BUILD_SH = """#!/bin/bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
[ "${1:-}" = "--dev" ] || { echo "want --dev"; exit 2; }
if [ -f FAIL_BUILD ]; then
  mkdir -p "@OUT@"
  printf 'g++ -Werror=return-type x.cpp\nx.cpp:1: the real error\n' > "@OUT@/compile.log"
  echo "error: build broken on purpose"; exit 1
fi
N=$(tr -d '[:space:]' < blinky-things/BUILD_NUMBER)
SHA=$(git rev-parse --short=8 HEAD)
printf '#define FIRMWARE_VERSION "b%s-dev-%s"\\n' "$N" "$SHA" > blinky-things/types/Version.h
mkdir -p "@OUT@"
printf ':00000001FF\\n%s\\n' "$(cat blinky-things/main.cpp)" > "@OUT@/blinky-things.ino.hex"
echo '[{"name": "ArduinoJson", "version": "7.4.2", "folder": "user/libraries/ArduinoJson"}]' \\
  > "@OUT@/libraries.json"
echo "compiled $N $SHA" > "@OUT@/compile.log"
mkdir -p "$XDG_CACHE_HOME/arduino" && echo "$SHA" > "$XDG_CACHE_HOME/arduino/last-build"
echo "=== Build $N complete ==="
"""

RELEASE_PY = '''
import argparse, hashlib, json, shutil
from pathlib import Path
ap = argparse.ArgumentParser()
sub = ap.add_subparsers(dest="cmd")
p = sub.add_parser("package")
for a in ("--hex", "--build", "--sha", "--out", "--toolchain-json", "--libraries-json"):
    p.add_argument(a, required=True)
p.add_argument("--dev", action="store_true")
a = ap.parse_args()
out = Path(a.out); out.mkdir(parents=True)
hx = out / f"blinky-things-b{a.build}.hex"; uf = out / f"blinky-things-b{a.build}.uf2"
shutil.copyfile(a.hex, hx); uf.write_bytes(b"UF2" + Path(a.hex).read_bytes())
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
m = {"build": int(a.build), "version": f"b{a.build}-dev-{a.sha[:8]}", "channel": "dev",
     "git_sha": a.sha, "files": {hx.name: sha(hx), uf.name: sha(uf)},
     "toolchain": json.loads(a.toolchain_json),
     "libraries": json.loads(Path(a.libraries_json).read_text())}
(out / "manifest.json").write_text(json.dumps(m, indent=2) + "\\n")
'''


def write_firmware_tree(root, out_dir, toolchain_pin=None):
    root = Path(root)
    files = {
        "tools/firmware_toolchain.json": json.dumps(toolchain_pin or TOOLCHAIN, indent=2),
        "tools/setup_tflite_lib.sh": textwrap.dedent("""\
            #!/usr/bin/env bash
            TFLITE_REF="${TFLITE_REF:-48a62b6e75862ceca82924e48618f197750b1299}"
            """),
        "tools/firmware_release.py": RELEASE_PY,
        "patches/tinyusb-cdc-patched-marker.patch": "--- a\n+++ b\n",
        "blinky-things/BUILD_NUMBER": "211\n",
        "blinky-things/types/Version.h": "// generated\n",
        "blinky-things/main.cpp": "int main() { return 0; }\n",
        "blinky-things/other.cpp": "int other() { return 1; }\n",
        "scripts/build.sh": BUILD_SH.replace("@OUT@", str(out_dir)),
        ".gitignore": ".arduino\n",
    }
    for rel, text in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    (root / "scripts/build.sh").chmod(0o755)


# A PR Validation workflow in the shape of blinky-firmware's: the two heavy jobs a check may run,
# with download steps the image provides and steps that run (checks.py).
WORKFLOW = """
name: PR Validation
on: [pull_request]
jobs:
  changes:
    runs-on: ubuntu-latest
    steps:
    - run: echo never run by a check
  tracker-harness:
    name: Tracker Harness (UBSan stress + corpus gate)
    runs-on: ubuntu-latest
    timeout-minutes: 30
    needs: changes
    if: ${{ always() }}
    steps:
    - name: Checkout repository
      uses: actions/checkout@v5
    - name: Corpus submodule
      run: git submodule update --init --depth 1 corpus
    - name: Setup Python
      uses: actions/setup-python@v5
      with:
        python-version: '3.11'
    - name: Install Python deps
      run: pip install numpy imageio-ffmpeg pytest
    - name: Corpus regression gate
      run: |
        test -f corpus/edm/track.json
        echo "gate $(cat corpus/edm/track.json)"
        if [ -f FAIL_GATE ]; then echo "drum F below the floor"; exit 1; fi
    - name: After the gate
      run: echo after
  tracker-harness-nn:
    name: Tracker Harness with real onset NN (host TFLite Micro)
    runs-on: ubuntu-latest
    steps:
    - uses: actions/checkout@v5
    - name: Setup Arduino CLI
      uses: arduino/setup-arduino-cli@v2
      with:
        version: 1.3.1
    - name: Install TFLite Micro library
      run: |
        arduino-cli config init
        bash tools/setup_tflite_lib.sh --ci
    - name: Build harness with the real onset NN
      env:
        CXX: the-compiler
      run: echo "built with $CXX"
"""


class Remote:
    """A bare 'GitHub' repo plus a working clone to make commits in, and a corpus repo the
    firmware pins at corpus/ (a gitlink, as a submodule)."""

    def __init__(self, tmp, out_dir):
        self.bare = tmp / "github.git"
        self.work = tmp / "work"
        self.corpus_bare = tmp / "corpus.git"
        corpus_work = tmp / "corpus-work"
        run_git(tmp, "init", "--bare", "-q", str(self.corpus_bare))
        run_git(tmp, "init", "-q", str(corpus_work))
        (corpus_work / "edm").mkdir()
        (corpus_work / "edm/track.json").write_text('{"bpm": 120}\n')
        run_git(corpus_work, "add", "-A")
        run_git(corpus_work, "commit", "-q", "-m", "corpus")
        self.corpus_pin = run_git(corpus_work, "rev-parse", "HEAD")
        run_git(corpus_work, "push", "-q", str(self.corpus_bare), "HEAD:refs/heads/master")
        # A corpus commit on no branch (a fork PR's, served by SHA): never taken.
        (corpus_work / "edm/track.json").write_text('{"bpm": 666}\n')
        run_git(corpus_work, "commit", "-q", "-am", "fork corpus")
        self.corpus_fork = run_git(corpus_work, "rev-parse", "HEAD")
        run_git(corpus_work, "push", "-q", str(self.corpus_bare), "HEAD:refs/pull/1/head")
        run_git(tmp, "init", "--bare", "-q", str(self.bare))
        run_git(tmp, "init", "-q", str(self.work))
        write_firmware_tree(self.work, out_dir)
        p = self.work / ".github/workflows/pr-validation.yml"
        p.parent.mkdir(parents=True)
        p.write_text(WORKFLOW)
        (self.work / "corpus").mkdir()  # an unpopulated submodule: `add -A` keeps the gitlink
        run_git(self.work, "update-index", "--add", "--cacheinfo",
                f"160000,{self.corpus_pin},corpus")
        self.commit("initial")
        self.base = self.head()
        self.commit_file("blinky-things/main.cpp", "int main() { return 2; }\n", "second")
        self.tip = self.head()
        run_git(self.work, "remote", "add", "origin", str(self.bare))
        run_git(self.work, "push", "-q", "origin", "HEAD:refs/heads/staging")
        # A fork PR: GitHub serves its commit from the base repo under refs/pull/N/head.
        run_git(self.work, "checkout", "-q", "-b", "fork")
        self.commit_file("blinky-things/main.cpp", "int main() { return 666; }\n", "fork change")
        self.fork = self.head()
        run_git(self.work, "push", "-q", "origin", "HEAD:refs/pull/1/head")
        run_git(self.work, "checkout", "-q", "main")
        run_git(self.work, "reset", "-q", "--hard", self.tip)

    def commit(self, msg):
        run_git(self.work, "add", "-A")
        run_git(self.work, "commit", "-q", "-m", msg)
        return self.head()

    def commit_file(self, rel, text, msg):
        p = self.work / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
        return self.commit(msg)

    def head(self):
        return run_git(self.work, "rev-parse", "HEAD")

    def push(self, ref="staging", force=False):
        flags = ["-f"] if force else []
        run_git(self.work, "push", "-q", *flags, "origin", f"HEAD:refs/heads/{ref}")

    def bundle(self, path, rev_range):
        run_git(self.work, "bundle", "create", "-q", str(path), *rev_range)
        return path


@pytest.fixture
def out_dir(tmp_path):
    return tmp_path / "blinky-build-out"


@pytest.fixture
def remote(tmp_path, out_dir):
    return Remote(tmp_path, out_dir)


@pytest.fixture
def service(tmp_path, remote):
    cfg = api.Config(
        host="127.0.0.1", port=0, token=TOKEN, data=tmp_path / "data", jobs=tmp_path / "jobs",
        repo=REPO, remote=str(remote.bare), max_bundle=8 * 1024 * 1024, keep=50,
        corpus_remote=str(remote.corpus_bare),
    )
    svc = api.Service(cfg)
    svc.start()
    return svc


class Client:
    def __init__(self, base):
        self.base = base

    def req(self, method, path, body=None, ctype=None, token=TOKEN, raw=False):
        headers = {}
        if token:
            headers["Authorization"] = f"Bearer {token}"
        if ctype:
            headers["Content-Type"] = ctype
        r = urllib.request.Request(self.base + path, data=body, method=method, headers=headers)
        try:
            with urllib.request.urlopen(r, timeout=60) as resp:
                data = resp.read()
                return resp.status, (data if raw else json.loads(data))
        except urllib.error.HTTPError as e:
            data = e.read()
            return e.code, (json.loads(data) if data else None)

    def post_sha(self, sha, repo=REPO, **kw):
        return self.req("POST", "/v1/builds", json.dumps({"repo": repo, "sha": sha}).encode(),
                        "application/json", **kw)

    def post_bundle(self, sha, bundle_bytes, **kw):
        boundary = "x" + secrets.token_hex(12)
        disp = f"--{boundary}\r\nContent-Disposition: form-data; "
        parts = [
            f'{disp}name="sha"\r\n\r\n{sha}\r\n'.encode(),
            (f'{disp}name="bundle"; filename="b.bundle"\r\n'
             "Content-Type: application/octet-stream\r\n\r\n").encode() + bundle_bytes + b"\r\n",
            f"--{boundary}--\r\n".encode(),
        ]
        return self.req("POST", "/v1/builds", b"".join(parts),
                        f"multipart/form-data; boundary={boundary}", **kw)


@pytest.fixture
def client(service):
    srv = api.make_server(service)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    host, port = srv.server_address[:2]
    yield Client(f"http://{host}:{port}")
    srv.shutdown()
    srv.server_close()


@pytest.fixture
def worker_env(tmp_path, monkeypatch, remote):
    """A worker config whose baked toolchain matches the fake tree, with an arduino-cli stub."""
    from blinky_build import worker

    bindir = tmp_path / "bin"
    bindir.mkdir()
    cli = bindir / "arduino-cli"
    cli.write_text("#!/bin/sh\necho 'arduino-cli  Version: 1.3.1 Commit: 08ff7e2b Date: x'\n")
    cli.chmod(cli.stat().st_mode | stat.S_IEXEC)
    monkeypatch.setenv("PATH", f"{bindir}:{os.environ['PATH']}")
    fp = toolchain.fingerprint(remote.work)
    tc_file = tmp_path / "toolchain.json"
    tc_file.write_text(json.dumps({**fp, "id": toolchain.fingerprint_id(fp)}))

    def make(tc=None):
        if tc is not None:
            tc_file.write_text(json.dumps(tc))
        cfg = worker.Config(
            jobs=tmp_path / "jobs", cache=tmp_path / "cache", src=tmp_path / "src",
            toolchain_file=tc_file, output=tmp_path / "blinky-build-out", timeout=120,
            home=tmp_path / "home",
        )
        return worker.Worker(cfg)

    return make
