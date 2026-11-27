"""The build worker: no network, non-root, the pinned toolchain baked into its image.

It takes queued jobs from the shared volume one at a time and runs the firmware's own
steps exactly as blinky-firmware's .github/workflows/staging-build.yml does:
  scripts/build.sh --dev
  tools/firmware_release.py package --dev  (the same toolchain JSON, build number and sha)
  the Version.h check
and writes package/ (manifest.json, blinky-things-b<N>.hex, .uf2), build.log and
status.json back into the job.

A check job (kind ``check:<job>``, checks.py) runs one heavy job of the commit's own
.github/workflows/pr-validation.yml instead, step for step, in the same persistent checkout
with the corpus the commit pins (from the job) at corpus/, and writes its ``conclusion``
(success | failure) and each step's outcome and time into status.json. ``failed`` stays an
infrastructure error: a job the image can't run as written, a toolchain mismatch, a timeout.

The checkout it builds in is one persistent tree at a fixed path (BLINKY_BUILD_SRC), moved
between commits with `git checkout`, so arduino-cli's sketch build directory (keyed by the
sketch path) stays warm and unchanged files keep their mtimes: an incremental build
recompiles only what changed. The build cache itself is on the cache volume. The pinned
toolchain sits at <src>/.arduino, root-owned and read-only to the build: arduino-cli
canonicalizes library paths and the firmware's tools/check_build_libs.py requires them
under <checkout>/.arduino, so it can't be a symlink.

Configuration (environment):
  BLINKY_BUILD_JOBS       the shared job volume (default /jobs)
  BLINKY_BUILD_CACHE      the warm build cache volume (default /cache): per toolchain,
                          arduino-cli's build cache ($XDG_CACHE_HOME) and TMPDIR
  BLINKY_BUILD_SRC        the persistent checkout, holding .arduino (default /opt/blinky/src)
  BLINKY_BUILD_TOOLCHAIN  the image's toolchain fingerprint (default /opt/blinky/toolchain.json)
  BLINKY_BUILD_OUTPUT     where scripts/build.sh writes the hex (default /tmp/blinky-build)
  BLINKY_BUILD_TIMEOUT    seconds per step (default 1800)
"""

import importlib.metadata
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from . import checks, toolchain
from .gitutil import SAFE_CONFIG, safe_env
from .jobs import JobStore

HEARTBEAT_S = 10


def log(msg):
    print(f"[worker {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}] {msg}", file=sys.stderr,
          flush=True)


class BuildFailed(Exception):
    pass


class StepFailed(BuildFailed):
    """A command exited non-zero or timed out: for a build, the build failed; for a check,
    its verdict (a timeout fails a job on GitHub too)."""


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
                shutil.rmtree(d / "corpus", ignore_errors=True)
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
        kind = self.store.kind(self.store.meta(job_id))
        if kind.startswith("check:"):
            self.check(job_id, kind.split(":", 1)[1])
        else:
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

    # ----------------------------------------------------------------- check ---

    def image(self):
        """What this image provides for a check's download steps (checks.plan)."""
        names = set()
        for dist in importlib.metadata.distributions():
            name = dist.metadata.get("Name")
            if name:
                names.add(checks._norm(name))
        return {
            "python": f"{sys.version_info.major}.{sys.version_info.minor}",
            "arduino_cli": str((self.baked.get("toolchain") or {}).get("arduino_cli", "")),
            "pip": names & checks.PROVIDED_PIP,
        }

    def check(self, job_id, job):
        d = self.cfg.jobs / job_id
        sha = self.store.meta(job_id)["sha"]
        t0 = time.monotonic()
        self.store.set_status(job_id, state="building", started_at=time.time())
        log(f"{job_id}: check {job} of {sha[:12]}")
        steps, conclusion, err = [], None, None
        with open(d / "build.log", "a", encoding="utf-8") as logf:
            try:
                conclusion = self._check(d, sha, job, logf, steps)
            except BuildFailed as e:
                err = str(e)
            except Exception as e:  # noqa: BLE001 - every failure ends the job, with a reason
                err = f"internal error: {e!r}"
            dur = round(time.monotonic() - t0, 1)
            logf.write(f"\n=== {'FAILED: ' + err if err else conclusion} after {dur}s\n")
        shutil.rmtree(d / "src", ignore_errors=True)
        shutil.rmtree(d / "corpus", ignore_errors=True)
        if err:
            self.store.set_status(job_id, state="failed", error=err, steps=steps,
                                  finished_at=time.time(), duration_s=dur)
            log(f"{job_id}: check failed to run after {dur}s: {err}")
        else:
            self.store.set_status(job_id, state="done", conclusion=conclusion, error=None,
                                  steps=steps, finished_at=time.time(), duration_s=dur)
            log(f"{job_id}: check {job} {conclusion} in {dur}s")

    def _check(self, d, sha, job, logf, steps):
        import yaml  # the image's PyYAML (requirements-worker.txt); data only: safe_load

        logf.write(f"=== check {job} of {sha} (job {d.name})\n")
        logf.flush()
        self.sync(d / "src", sha, logf)
        src = self.cfg.src
        try:
            want = toolchain.fingerprint(src)
        except toolchain.ToolchainError as e:
            raise BuildFailed(f"cannot read this commit's toolchain pin: {e}") from None
        diffs = toolchain.differences(want, self.baked)
        if diffs:
            raise BuildFailed("toolchain mismatch: rebuild the worker image. " + "; ".join(diffs))
        corpus = d / "corpus"
        if not corpus.is_dir():
            raise BuildFailed("the job has no corpus")
        shutil.rmtree(src / "corpus", ignore_errors=True)
        shutil.copytree(corpus, src / "corpus", symlinks=True)
        try:
            wf = yaml.safe_load((src / checks.WORKFLOW).read_text(encoding="utf-8"))
            plan = checks.plan(wf, job, self.image())
        except (OSError, yaml.YAMLError) as e:
            raise BuildFailed(f"{checks.WORKFLOW} unreadable: {e}") from None
        except checks.Unsupported as e:
            raise BuildFailed(f"can't run as written: {e}") from None
        deadline = time.monotonic() + checks.job_timeout_s(wf, job, self.cfg.timeout)
        tmp = self.cfg.cache / "check-tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True)
        self.cfg.home.mkdir(parents=True, exist_ok=True)
        base = {
            "PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"),
            "HOME": str(self.cfg.home),
            "TMPDIR": str(tmp),
            "LANG": "C.UTF-8",
            "CI": "true",
            "PYTHONDONTWRITEBYTECODE": "1",
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": "/dev/null",
        }
        failed = False
        for step in plan:
            if step.provided:
                steps.append({"name": step.name, "outcome": "provided", "seconds": 0.0})
                logf.write(f"\n=== {step.name}: provided by the image ({step.provided})\n")
                continue
            if failed:
                steps.append({"name": step.name, "outcome": "skipped", "seconds": None})
                continue
            left = deadline - time.monotonic()
            if left <= 0:  # as on GitHub: the job timed out, a failure
                logf.write(f"\n=== {step.name}: the job's timeout passed\n")
                steps.append({"name": step.name, "outcome": "failure", "seconds": 0.0})
                failed = True
                continue
            t = time.monotonic()
            try:
                self._run(logf, ["bash", "--noprofile", "--norc", "-eo", "pipefail", "-c",
                                 step.script], {**base, **step.env}, step.name,
                          timeout=min(left, step.timeout_s or left))
                outcome = "success"
            except StepFailed:
                outcome, failed = "failure", True
            steps.append({"name": step.name, "outcome": outcome,
                          "seconds": round(time.monotonic() - t, 1)})
        return "failure" if failed else "success"

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

    def _run(self, logf, cmd, env, what, timeout=None):
        timeout = self.cfg.timeout if timeout is None else timeout
        logf.write(f"\n=== {what}: {' '.join(map(str, cmd))}\n")
        logf.flush()
        t0 = time.monotonic()
        p = subprocess.Popen([str(c) for c in cmd], cwd=self.cfg.src, env=env, stdout=logf,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             start_new_session=True)
        try:
            rc = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            raise StepFailed(f"{what} timed out after {timeout:.0f}s") from None
        logf.write(f"=== {what}: exit {rc} in {time.monotonic() - t0:.1f}s\n")
        logf.flush()
        if rc != 0:
            raise StepFailed(f"{what} failed (exit {rc}); see the log")

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

        tc_cache = self.cfg.cache / f"tc-{tc_id}"
        tmp = tc_cache / "tmp"
        xdg = tc_cache / "xdg-cache"
        tmp.mkdir(parents=True, exist_ok=True)
        xdg.mkdir(parents=True, exist_ok=True)
        self.cfg.home.mkdir(parents=True, exist_ok=True)
        env = {
            "PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"),
            "HOME": str(self.cfg.home),
            # arduino-cli 1.3.1 keeps its sketch build directories and core cache in
            # build_cache.path, which defaults to os.UserCacheDir()/arduino, i.e.
            # $XDG_CACHE_HOME/arduino: on the cache volume, one tree per toolchain.
            "XDG_CACHE_HOME": str(xdg),
            "TMPDIR": str(tmp),
            "LANG": "C.UTF-8",
            "PYTHON": sys.executable,
            "PYTHONDONTWRITEBYTECODE": "1",
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": "/dev/null",
        }
        out = self.cfg.output
        shutil.rmtree(out, ignore_errors=True)
        try:
            self._run(logf, ["bash", "scripts/build.sh", "--dev"], env, "scripts/build.sh --dev")
        except BuildFailed:
            # build.sh shows only `grep error | head -40` of its compile log, which the
            # compiler's -Werror flags can fill: the log's own tail has the real error.
            self._keep_compile_log(out, d, logf, tail=True)
            raise
        self._keep_compile_log(out, d, logf, tail=False)

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
    def _keep_compile_log(out, d, logf, tail):
        log_path = out / "compile.log"
        if not log_path.is_file():
            return
        shutil.copyfile(log_path, d / "compile.log")
        if tail:
            lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
            logf.write(f"\n=== compile.log, last {min(len(lines), 40)} lines:\n")
            logf.write("\n".join(line[:400] for line in lines[-40:]) + "\n")

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
