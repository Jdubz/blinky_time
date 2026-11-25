"""The build worker: no network, non-root, the pinned toolchain baked into its image.

It takes queued jobs from the shared volume one at a time and runs the firmware's own
steps exactly as blinky-firmware's .github/workflows/staging-build.yml does:
  scripts/build.sh --dev
  tools/firmware_release.py package --dev  (the same toolchain JSON, build number and sha)
  the Version.h check
and writes package/ (manifest.json, blinky-things-b<N>.hex, .uf2), build.log and
status.json back into the job.

The checkout it builds in is one persistent tree at a fixed path (BLINKY_BUILD_SRC), moved
between commits with `git checkout`, so arduino-cli's sketch build directory (keyed by the
sketch path) stays warm and unchanged files keep their mtimes: an incremental build
recompiles only what changed. The pinned toolchain sits at <src>/.arduino, root-owned and
read-only to the build: arduino-cli canonicalizes library paths and the firmware's
tools/check_build_libs.py requires them under <checkout>/.arduino, so it can't be a symlink.

Configuration (environment):
  BLINKY_BUILD_JOBS       the shared job volume (default /jobs)
  BLINKY_BUILD_CACHE      the warm build cache volume (default /cache)
  BLINKY_BUILD_SRC        the persistent checkout, holding .arduino (default /opt/blinky/src)
  BLINKY_BUILD_TOOLCHAIN  the image's toolchain fingerprint (default /opt/blinky/toolchain.json)
  BLINKY_BUILD_OUTPUT     where scripts/build.sh writes the hex (default /tmp/blinky-build)
  BLINKY_BUILD_TIMEOUT    seconds per step (default 1800)
"""

import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from . import toolchain
from .gitutil import SAFE_CONFIG, safe_env
from .jobs import JobStore

HEARTBEAT_S = 10


def log(msg):
    print(f"[worker {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}] {msg}", file=sys.stderr,
          flush=True)


class BuildFailed(Exception):
    pass


class Config:
    def __init__(self, jobs, cache, src, toolchain_file, output, timeout, poll=1.0, home=None):
        self.jobs, self.cache, self.src = Path(jobs), Path(cache), Path(src)
        self.toolchain_file, self.output = Path(toolchain_file), Path(output)
        self.timeout, self.poll = timeout, poll
        self.home = Path(home) if home else Path(os.environ.get("HOME", "/tmp"))

    @classmethod
    def from_env(cls, env=None):
        env = os.environ if env is None else env
        return cls(
            jobs=env.get("BLINKY_BUILD_JOBS", "/jobs"),
            cache=env.get("BLINKY_BUILD_CACHE", "/cache"),
            src=env.get("BLINKY_BUILD_SRC", "/opt/blinky/src"),
            toolchain_file=env.get("BLINKY_BUILD_TOOLCHAIN", "/opt/blinky/toolchain.json"),
            output=env.get("BLINKY_BUILD_OUTPUT", "/tmp/blinky-build"),
            timeout=int(env.get("BLINKY_BUILD_TIMEOUT", "1800")),
            poll=float(env.get("BLINKY_BUILD_POLL", "1.0")),
        )


# Written into the persistent checkout's .git/config before every sync: whatever a build
# changed there (it runs repo code with write access to its own tree) is discarded.
GIT_CONFIG = """[core]
\trepositoryformatversion = 0
\tfilemode = true
\tbare = false
\thooksPath = /dev/null
\tfsmonitor = false
[gc]
\tautoDetach = false
"""


class Worker:
    def __init__(self, cfg):
        self.cfg = cfg
        self.store = JobStore(cfg.jobs)
        baked = json.loads(cfg.toolchain_file.read_text(encoding="utf-8"))
        baked.pop("id", None)
        self.baked = baked
        self._stop = threading.Event()

    # ------------------------------------------------------------------ loop ---

    def heartbeat(self):
        d = self.cfg.jobs / ".worker"
        d.mkdir(parents=True, exist_ok=True)
        (d / "heartbeat").write_text(f"{time.time()}\n", encoding="utf-8")

    def _heartbeats(self):
        while not self._stop.wait(HEARTBEAT_S):
            try:
                self.heartbeat()
            except OSError as e:
                log(f"heartbeat failed: {e}")

    def recover(self):
        """A job left `building` (or claimed and never started) by a dead worker failed.
        One worker per job volume."""
        for job_id in self.store.ids():
            d = self.cfg.jobs / job_id
            st = self.store.status(job_id) or {}
            if st.get("state") == "building" or (
                (d / "claim").exists() and not (d / "status.json").exists()
            ):
                self.store.set_status(job_id, state="failed",
                                      error="the worker restarted during this build; submit again")
                shutil.rmtree(d / "src", ignore_errors=True)
                log(f"{job_id}: marked failed (worker restarted)")

    def prune_caches(self):
        """Drop build caches of other toolchains: this image builds only its own."""
        keep = f"tc-{toolchain.fingerprint_id(self.baked)}"
        if not self.cfg.cache.is_dir():
            return
        for d in self.cfg.cache.iterdir():
            if d.name.startswith("tc-") and d.name != keep:
                shutil.rmtree(d, ignore_errors=True)
                log(f"removed the build cache of another toolchain: {d.name}")

    def run_once(self):
        job_id = self.store.next_queued()
        if not job_id or not self.store.claim(job_id):
            return False
        self.build(job_id)
        return True

    def run_forever(self):
        self.recover()
        self.prune_caches()
        self.heartbeat()
        threading.Thread(target=self._heartbeats, daemon=True).start()
        log(f"ready; toolchain {toolchain.fingerprint_id(self.baked)}")
        while True:
            if not self.run_once():
                time.sleep(self.cfg.poll)

    # ----------------------------------------------------------------- build ---

    def build(self, job_id):
        d = self.cfg.jobs / job_id
        sha = self.store.meta(job_id)["sha"]
        t0 = time.monotonic()
        self.store.set_status(job_id, state="building", started_at=time.time())
        log(f"{job_id}: building {sha[:12]}")
        with open(d / "build.log", "a", encoding="utf-8") as logf:
            try:
                version = self._build(d, sha, logf)
            except BuildFailed as e:
                err, version = str(e), None
            except Exception as e:  # noqa: BLE001 - every failure ends the job, with a reason
                err, version = f"internal error: {e!r}", None
            else:
                err = None
            dur = round(time.monotonic() - t0, 1)
            if err:
                logf.write(f"\n=== FAILED after {dur}s: {err}\n")
            else:
                logf.write(f"\n=== done in {dur}s: {version}\n")
        shutil.rmtree(d / "src", ignore_errors=True)
        if err:
            self.store.set_status(job_id, state="failed", error=err, finished_at=time.time(),
                                  duration_s=dur)
            log(f"{job_id}: failed after {dur}s: {err}")
        else:
            self.store.set_status(job_id, state="done", version=version, error=None,
                                  finished_at=time.time(), duration_s=dur)
            log(f"{job_id}: done in {dur}s: {version}")

    def _git(self, logf, *args):
        cmd = ["git"]
        for c in SAFE_CONFIG:
            cmd += ["-c", c]
        cmd += ["-C", str(self.cfg.src), *map(str, args)]
        r = subprocess.run(cmd, env=safe_env(), capture_output=True, text=True, timeout=600)
        if r.returncode != 0:
            logf.write(r.stdout + r.stderr)
            raise BuildFailed(f"git {args[0]} failed: {r.stderr.strip()[:300]}")
        return r.stdout.strip()

    def sync(self, job_src, sha, logf):
        """Move the persistent checkout to ``sha`` from the job's checkout (local, no network)."""
        src = self.cfg.src
        src.mkdir(parents=True, exist_ok=True)
        if not (src / ".git").is_dir():
            self._git(logf, "init", "--quiet")
        (src / ".git/config").write_text(GIT_CONFIG, encoding="utf-8")
        for stray in ("info/attributes", "hooks"):
            p = src / ".git" / stray
            if p.is_dir():
                shutil.rmtree(p, ignore_errors=True)
            elif p.exists():
                p.unlink()
        self._git(logf, "fetch", "--quiet", "--no-tags", "--no-recurse-submodules",
                  "--no-write-fetch-head", str(Path(job_src).resolve()), "+HEAD:refs/build/head")
        got = self._git(logf, "rev-parse", "refs/build/head")
        if got != sha:
            raise BuildFailed(f"job checkout is at {got}, not {sha}")
        self._git(logf, "checkout", "--quiet", "--force", "--detach", sha)
        # .arduino (the baked toolchain) is ignored by the repo and must survive -x.
        self._git(logf, "clean", "-ffdxq", "-e", "/.arduino")
        if self._git(logf, "rev-parse", "HEAD") != sha:
            raise BuildFailed("checkout did not land on the job's commit")

    def _run(self, logf, cmd, env, what):
        logf.write(f"\n=== {what}: {' '.join(map(str, cmd))}\n")
        logf.flush()
        t0 = time.monotonic()
        p = subprocess.Popen([str(c) for c in cmd], cwd=self.cfg.src, env=env, stdout=logf,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             start_new_session=True)
        try:
            rc = p.wait(timeout=self.cfg.timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            raise BuildFailed(f"{what} timed out after {self.cfg.timeout}s") from None
        logf.write(f"=== {what}: exit {rc} in {time.monotonic() - t0:.1f}s\n")
        logf.flush()
        if rc != 0:
            raise BuildFailed(f"{what} failed (exit {rc}); see the log")

    def _build(self, d, sha, logf):
        logf.write(f"=== job {d.name}: {sha}\n")
        logf.flush()
        self.sync(d / "src", sha, logf)
        src = self.cfg.src

        try:
            want = toolchain.fingerprint(src)
        except toolchain.ToolchainError as e:
            raise BuildFailed(f"cannot read this commit's toolchain pin: {e}") from None
        diffs = toolchain.differences(want, self.baked)
        if diffs:
            raise BuildFailed(
                "toolchain mismatch: rebuild the worker image (FIRMWARE_TOOLCHAIN_REF at a commit "
                "with this toolchain). " + "; ".join(diffs)
            )
        tc_id = toolchain.fingerprint_id(want)
        logf.write(f"=== toolchain {tc_id} (matches the image)\n")

        tmp = self.cfg.cache / f"tc-{tc_id}" / "tmp"
        tmp.mkdir(parents=True, exist_ok=True)
        self.cfg.home.mkdir(parents=True, exist_ok=True)
        env = {
            "PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"),
            "HOME": str(self.cfg.home),
            # arduino-cli's build directory and core cache live under TMPDIR: on the cache
            # volume, one tree per toolchain.
            "TMPDIR": str(tmp),
            "LANG": "C.UTF-8",
            "PYTHON": sys.executable,
            "PYTHONDONTWRITEBYTECODE": "1",
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": "/dev/null",
        }
        out = self.cfg.output
        shutil.rmtree(out, ignore_errors=True)
        self._run(logf, ["bash", "scripts/build.sh", "--dev"], env, "scripts/build.sh --dev")
        if (out / "compile.log").is_file():
            shutil.copyfile(out / "compile.log", d / "compile.log")

        # staging-build.yml "Package and validate", step for step.
        n = (src / "blinky-things/BUILD_NUMBER").read_text(encoding="utf-8").strip()
        if not n.isdigit():
            raise BuildFailed(f"BUILD_NUMBER is not a number: {n!r}")
        tcj = toolchain.toolchain_json(src)
        cli = subprocess.run(["arduino-cli", "version"], env=env, capture_output=True, text=True,
                             timeout=60)
        cli_line = (cli.stdout.splitlines() or [""])[0]
        tc = json.dumps(
            {
                "core": tcj.get("core"),
                "libraries": tcj.get("libraries"),
                "fqbn": tcj.get("fqbn"),
                "tflite_ref": toolchain.tflite_ref(src),
                "arduino_cli": cli_line,
            },
            separators=(",", ":"),
        )
        partial = d / "package.partial"
        shutil.rmtree(partial, ignore_errors=True)
        self._run(
            logf,
            [sys.executable, "tools/firmware_release.py", "package", "--dev",
             "--hex", out / "blinky-things.ino.hex", "--build", n, "--sha", sha,
             "--out", partial, "--toolchain-json", tc,
             "--libraries-json", out / "libraries.json"],
            env,
            "firmware_release.py package --dev",
        )
        version = f"b{n}-dev-{sha[:8]}"
        self._check_package(partial, sha, int(n), version)
        version_h = (src / "blinky-things/types/Version.h").read_text(encoding="utf-8")
        if f'"{version}"' not in version_h:
            raise BuildFailed(f"Version.h does not carry {version}")
        shutil.rmtree(d / "package", ignore_errors=True)
        os.replace(partial, d / "package")
        return version

    @staticmethod
    def _check_package(pkg, sha, n, version):
        import hashlib

        m = json.loads((pkg / "manifest.json").read_text(encoding="utf-8"))
        if (m.get("channel"), m.get("git_sha"), m.get("version"), m.get("build")) != (
            "dev", sha, version, n
        ):
            raise BuildFailed(f"package manifest is not the dev build of {sha[:12]}: {m}")
        names = {f"blinky-things-b{n}.hex", f"blinky-things-b{n}.uf2"}
        files = m.get("files") or {}
        if set(files) != names or {p.name for p in pkg.iterdir()} != names | {"manifest.json"}:
            raise BuildFailed(f"package files are not {sorted(names)} + manifest.json")
        for name in names:
            if hashlib.sha256((pkg / name).read_bytes()).hexdigest() != files[name]:
                raise BuildFailed(f"{name}: sha256 differs from the manifest")


def main():
    cfg = Config.from_env()
    Worker(cfg).run_forever()


if __name__ == "__main__":
    main()
