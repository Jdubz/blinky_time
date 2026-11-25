"""Jobs on the shared volume: the only channel between the api and the worker.

  <jobs>/.incoming/<id>/   being written by the api; renamed into place when complete
  <jobs>/<id>/job.json     {id, sha, repo, source, created_at}           (api)
  <jobs>/<id>/src/         a checkout of sha with history                (api; worker removes it)
  <jobs>/<id>/claim        the worker took it (O_EXCL)                   (worker)
  <jobs>/<id>/status.json  {state, version, error, ...}                  (worker, atomic)
  <jobs>/<id>/build.log    the build's output                            (worker)
  <jobs>/<id>/package/     manifest.json, *.hex, *.uf2                   (worker, renamed in)

No status.json means queued.
"""

import json
import os
import re
import secrets
import shutil
import time
from pathlib import Path

ID_RE = re.compile(r"^[0-9a-z][0-9a-z-]{0,63}$")
STATES = ("queued", "building", "done", "failed")
LOG_TAIL_BYTES = 6000
LOG_TAIL_LINES = 60


def _write_json_atomic(path, data):
    path = Path(path)
    tmp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    tmp.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


def _read_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def log_tail(path):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - LOG_TAIL_BYTES))
            data = f.read().decode("utf-8", errors="replace")
    except OSError:
        return ""
    lines = data.splitlines()
    if size > LOG_TAIL_BYTES and lines:
        lines = lines[1:]  # the first line is probably cut
    return "\n".join(lines[-LOG_TAIL_LINES:])


class JobStore:
    def __init__(self, root):
        self.root = Path(root)

    # ---------------------------------------------------------------- common ---

    def dir(self, job_id):
        if not ID_RE.match(job_id or ""):
            return None
        d = self.root / job_id
        return d if (d / "job.json").is_file() else None

    def ids(self):
        """Job ids, oldest first."""
        jobs = []
        try:
            entries = list(self.root.iterdir())
        except FileNotFoundError:
            return []
        for d in entries:
            if d.name.startswith(".") or not ID_RE.match(d.name):
                continue
            meta = _read_json(d / "job.json")
            if meta:
                jobs.append((meta.get("created_at", 0), d.name))
        return [i for _, i in sorted(jobs)]

    def meta(self, job_id):
        d = self.dir(job_id)
        return _read_json(d / "job.json") if d else None

    def status(self, job_id):
        d = self.dir(job_id)
        if not d:
            return None
        return _read_json(d / "status.json") or {"state": "queued"}

    def view(self, job_id):
        """The API's view of a job, or None."""
        d = self.dir(job_id)
        meta = self.meta(job_id)
        if not d or not meta:
            return None
        st = self.status(job_id)
        return {
            "id": job_id,
            "state": st.get("state", "queued"),
            "sha": meta["sha"],
            "version": st.get("version"),
            "error": st.get("error"),
            "log_tail": log_tail(d / "build.log"),
            "source": meta.get("source"),
            "created_at": meta.get("created_at"),
            "duration_s": st.get("duration_s"),
        }

    def package_dir(self, job_id):
        d = self.dir(job_id)
        if not d or (self.status(job_id) or {}).get("state") != "done":
            return None
        p = d / "package"
        return p if p.is_dir() else None

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
        """Write job.json and move the finished job directory into the queue."""
        src = self.root / ".incoming" / job_id
        _write_json_atomic(
            src / "job.json",
            {"id": job_id, "sha": sha, "repo": repo, "source": source, "created_at": time.time()},
        )
        os.replace(src, self.root / job_id)

    def discard_incoming(self, job_id):
        shutil.rmtree(self.root / ".incoming" / job_id, ignore_errors=True)

    def prune(self, keep):
        """Delete the oldest finished jobs beyond the newest ``keep``. Never a queued or
        building one."""
        ids = self.ids()
        for job_id in ids[: max(0, len(ids) - keep)]:
            if (self.status(job_id) or {}).get("state") in ("done", "failed"):
                shutil.rmtree(self.root / job_id, ignore_errors=True)

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
        st = _read_json(d / "status.json") or {}
        st.update(fields)
        assert st.get("state") in STATES, st
        _write_json_atomic(d / "status.json", st)
        return st
