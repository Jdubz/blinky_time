"""Jobs on the shared volume: the only channel between the api and the worker.

  <jobs>/.incoming/<id>/   being written by the api; renamed into place when complete
  <jobs>/<id>/job.json     {id, sha, repo, source, created_at}           (api, for the worker)
  <jobs>/<id>/src/         a checkout of sha with history                (api; worker removes it)
  <jobs>/<id>/claim        the worker took it (O_EXCL)                   (worker)
  <jobs>/<id>/status.json  {state, version, error, ...}                  (worker, atomic)
  <jobs>/<id>/build.log    the build's output                            (worker)
  <jobs>/<id>/package/     manifest.json, *.hex, *.uf2                   (worker, renamed in)

No status.json means queued; an unreadable one means failed.

The build runs repository code with write access to this volume, so the api treats every
file the worker writes as untrusted data: it keeps its own record of each job (id, sha,
source) in private storage (``meta_dir``), and reads worker files only through
``read_file``, which follows no symlink, opens only regular files and caps the size. The
api runs no git command on this volume (mirror.py builds the checkout in private storage
and copies it in).
"""

import json
import os
import re
import secrets
import shutil
import stat
import time
from pathlib import Path

ID_RE = re.compile(r"^[0-9a-z][0-9a-z-]{0,63}$")
STATES = ("queued", "building", "done", "failed")
LOG_TAIL_BYTES = 6000
LOG_TAIL_LINES = 60
MAX_JSON = 1 << 20
MAX_PACKAGE_FILE = 64 << 20


def _write_json_atomic(path, data):
    path = Path(path)
    tmp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def read_file(root, *parts, limit=MAX_JSON, tail=False):
    """Bytes of root/parts..., walking every component without following a symlink and
    opening only a regular file (never a FIFO, device or /proc link). None if absent or
    anything else. ``tail``: the last ``limit`` bytes instead of refusing a bigger file."""
    fds = []
    try:
        fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
        fds.append(fd)
        for name in parts[:-1]:
            fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            fds.append(fd)
        fd = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=fd)
        fds.append(fd)
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            return None
        if st.st_size > limit:
            if not tail:
                return None
            os.lseek(fd, st.st_size - limit, os.SEEK_SET)
        chunks, left = [], limit
        while left > 0:
            b = os.read(fd, min(left, 1 << 20))
            if not b:
                break
            chunks.append(b)
            left -= len(b)
        return b"".join(chunks)
    except OSError:
        return None
    finally:
        for f in fds:
            os.close(f)


def list_dir(root, *parts):
    """Names in root/parts... (no symlink followed), or None."""
    fds = []
    try:
        fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
        fds.append(fd)
        for name in parts:
            fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            fds.append(fd)
        return sorted(os.listdir(fd))
    except OSError:
        return None
    finally:
        for f in fds:
            os.close(f)


def _json(data):
    if data is None:
        return None
    try:
        v = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None
    return v if isinstance(v, dict) else None


def tail_text(data, cut):
    text = (data or b"").decode("utf-8", errors="replace")
    lines = text.splitlines()
    if cut and lines:
        lines = lines[1:]  # the first line is probably cut
    return "\n".join(lines[-LOG_TAIL_LINES:])


class JobStore:
    """``meta_dir``: the api's private record of jobs. Without it (the worker), job.json on
    the shared volume is the record."""

    def __init__(self, root, meta_dir=None):
        self.root = Path(root)
        self.meta_dir = Path(meta_dir) if meta_dir else None

    # ---------------------------------------------------------------- common ---

    def ids(self):
        """Job ids, oldest first."""
        jobs = []
        if self.meta_dir:
            names = list_dir(self.meta_dir) or []
            names = [n[:-5] for n in names if n.endswith(".json")]
        else:
            names = list_dir(self.root) or []
        for name in names:
            if name.startswith(".") or not ID_RE.match(name):
                continue
            meta = self.meta(name)
            if meta:
                jobs.append((meta.get("created_at", 0), name))
        return [i for _, i in sorted(jobs)]

    def meta(self, job_id):
        if not ID_RE.match(job_id or ""):
            return None
        if self.meta_dir:
            m = _json(read_file(self.meta_dir, f"{job_id}.json"))
        else:
            m = _json(read_file(self.root, job_id, "job.json"))
        if not m or m.get("id") != job_id or not isinstance(m.get("sha"), str):
            return None
        return m

    def status(self, job_id):
        if not self.meta(job_id):
            return None
        raw = read_file(self.root, job_id, "status.json")
        if raw is None:
            exists = list_dir(self.root, job_id)
            if exists is not None and "status.json" in exists:
                return {"state": "failed", "error": "status.json is unreadable"}
            return {"state": "queued"}
        st = _json(raw)
        if not st or st.get("state") not in STATES:
            return {"state": "failed", "error": "status.json is corrupt"}
        return st

    def view(self, job_id):
        """The API's view of a job, or None."""
        meta = self.meta(job_id)
        if not meta:
            return None
        st = self.status(job_id)
        log = read_file(self.root, job_id, "build.log", limit=LOG_TAIL_BYTES, tail=True)
        version = st.get("version")
        error = st.get("error")
        return {
            "id": job_id,
            "state": st["state"],
            "sha": meta["sha"],
            "version": version if isinstance(version, str) else None,
            "error": error if error is None or isinstance(error, str) else str(error),
            "log_tail": tail_text(log, log is not None and len(log) >= LOG_TAIL_BYTES),
            "source": meta.get("source"),
            "created_at": meta.get("created_at"),
            "duration_s": st.get("duration_s"),
        }

    def package_files(self, job_id):
        """{name: bytes} of a done job's package: manifest.json, *.hex, *.uf2, regular files
        only. None if not done or not readable."""
        if (self.status(job_id) or {}).get("state") != "done":
            return None
        names = list_dir(self.root, job_id, "package")
        if names is None:
            return None
        files = {}
        for name in names:
            if name == "manifest.json" or name.endswith((".hex", ".uf2")):
                data = read_file(self.root, job_id, "package", name, limit=MAX_PACKAGE_FILE)
                if data is None:
                    return None
                files[name] = data
        return files

    # ------------------------------------------------------------------- api ---

    def new_id(self, sha):
        return f"{time.strftime('%Y%m%d%H%M%S', time.gmtime())}-{sha[:8]}-{secrets.token_hex(3)}"

    def find_reusable(self, sha):
        """The newest done build of ``sha``, else one still queued or building, else None."""
        done = inflight = None
        for job_id in self.ids():
            meta = self.meta(job_id)
            if not meta or meta.get("sha") != sha:
                continue
            state = (self.status(job_id) or {}).get("state")
            if state == "done":
                done = job_id
            elif state in ("queued", "building"):
                inflight = job_id
        return done or inflight

    def incoming(self, job_id):
        d = self.root / ".incoming" / job_id
        d.mkdir(parents=True)
        return d

    def publish(self, job_id, sha, repo, source):
        """Record the job and move the finished job directory into the queue."""
        meta = {"id": job_id, "sha": sha, "repo": repo, "source": source,
                "created_at": time.time()}
        src = self.root / ".incoming" / job_id
        _write_json_atomic(src / "job.json", meta)
        if self.meta_dir:
            self.meta_dir.mkdir(parents=True, exist_ok=True)
            _write_json_atomic(self.meta_dir / f"{job_id}.json", meta)
        os.replace(src, self.root / job_id)

    def discard_incoming(self, job_id):
        shutil.rmtree(self.root / ".incoming" / job_id, ignore_errors=True)

    def prune(self, keep):
        """Delete the oldest finished jobs beyond the newest ``keep`` (never a queued or
        building one). Returns the ids deleted."""
        ids = self.ids()
        gone = []
        for job_id in ids[: max(0, len(ids) - keep)]:
            if (self.status(job_id) or {}).get("state") in ("done", "failed"):
                shutil.rmtree(self.root / job_id, ignore_errors=True)  # never follows links
                if self.meta_dir:
                    (self.meta_dir / f"{job_id}.json").unlink(missing_ok=True)
                gone.append(job_id)
        return gone

    # ---------------------------------------------------------------- worker ---

    def next_queued(self):
        for job_id in self.ids():
            d = self.root / job_id
            if not (d / "claim").exists() and not (d / "status.json").exists():
                return job_id
        return None

    def claim(self, job_id):
        try:
            fd = os.open(self.root / job_id / "claim", os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        except FileExistsError:
            return False
        with os.fdopen(fd, "w") as f:
            f.write(f"{os.getpid()} {time.time()}\n")
        return True

    def set_status(self, job_id, **fields):
        d = self.root / job_id
        st = _json(read_file(self.root, job_id, "status.json")) or {}
        st.update(fields)
        assert st.get("state") in STATES, st
        _write_json_atomic(d / "status.json", st)
        return st
