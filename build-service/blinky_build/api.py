"""The build service's HTTP API. Every git operation happens here; no repository code runs.

  GET  /v1/healthz                 no auth -> {"ok": true}
  POST /v1/builds                  JSON {"repo": REPO, "sha": "<40 hex>"}, or multipart
                                   fields `bundle` (file) and `sha`
                                   -> 202 {"id", "state", "sha"}; an existing done (or
                                   in-flight) build of that sha is returned as it is
                                   400 bad input, 401 no/bad token, 403 not an owner commit,
                                   409 bundle prerequisites not on a branch, 413 too large
  GET  /v1/builds/{id}             -> {"id", "state", "sha", "version", "error", "log_tail", ...}
  GET  /v1/builds/{id}/package     -> .tar.gz of manifest.json, *.hex, *.uf2; 409 until done

Configuration (environment only; there is no default address or port):
  BLINKY_BUILD_HOST, BLINKY_BUILD_PORT    where to listen (required)
  BLINKY_BUILD_TOKEN or BLINKY_BUILD_TOKEN_FILE   the bearer token (required, >= 32 chars)
  BLINKY_BUILD_DATA      the mirror's volume (default /data)
  BLINKY_BUILD_JOBS      the shared job volume (default /jobs)
  BLINKY_BUILD_REPO      the one repo built here (default Jdubz/blinky-firmware)
  BLINKY_BUILD_REMOTE    its fetch URL (default https://github.com/<repo>.git)
  BLINKY_BUILD_MAX_BUNDLE_MB  (default 64)    BLINKY_BUILD_KEEP_JOBS  (default 200)
"""

import hmac
import io
import json
import os
import re
import sys
import tarfile
import tempfile
import threading
import time
from email.message import Message
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from . import SHA_RE
from .jobs import ID_RE, JobStore
from .mirror import BadInput, Mirror, MissingPrerequisites, Untrusted

SHA = re.compile(SHA_RE)
MAX_JSON = 64 * 1024
MIN_TOKEN = 32


def log(msg):
    print(f"[api {time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}] {msg}", file=sys.stderr,
          flush=True)


class HttpError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


class Config:
    def __init__(self, host, port, token, data, jobs, repo, remote, max_bundle, keep):
        self.host, self.port, self.token = host, port, token
        self.data, self.jobs = Path(data), Path(jobs)
        self.repo, self.remote = repo, remote
        self.max_bundle, self.keep = max_bundle, keep

    @classmethod
    def from_env(cls, env=None):
        env = os.environ if env is None else env

        def need(key):
            v = env.get(key, "").strip()
            if not v:
                raise SystemExit(f"{key} is required (no default: see build-service/README.md)")
            return v

        token = env.get("BLINKY_BUILD_TOKEN", "").strip()
        if not token and env.get("BLINKY_BUILD_TOKEN_FILE"):
            token = Path(env["BLINKY_BUILD_TOKEN_FILE"]).read_text(encoding="utf-8").strip()
        if len(token) < MIN_TOKEN:
            raise SystemExit(f"BLINKY_BUILD_TOKEN(_FILE) is required, at least {MIN_TOKEN} chars")
        repo = env.get("BLINKY_BUILD_REPO", "Jdubz/blinky-firmware")
        return cls(
            host=need("BLINKY_BUILD_HOST"),
            port=int(need("BLINKY_BUILD_PORT")),
            token=token,
            data=env.get("BLINKY_BUILD_DATA", "/data"),
            jobs=env.get("BLINKY_BUILD_JOBS", "/jobs"),
            repo=repo,
            remote=env.get("BLINKY_BUILD_REMOTE", f"https://github.com/{repo}.git"),
            max_bundle=int(env.get("BLINKY_BUILD_MAX_BUNDLE_MB", "64")) * 1024 * 1024,
            keep=int(env.get("BLINKY_BUILD_KEEP_JOBS", "200")),
        )


class Service:
    def __init__(self, cfg):
        self.cfg = cfg
        self.mirror = Mirror(cfg.data / "mirror.git", cfg.remote, cfg.repo)
        self.store = JobStore(cfg.jobs)
        self.lock = threading.Lock()

    def start(self):
        self.cfg.jobs.mkdir(parents=True, exist_ok=True)
        (self.cfg.data / "tmp").mkdir(parents=True, exist_ok=True)
        self.mirror.ensure()
        err = self.mirror.fetch_heads()
        log(f"mirror ready ({self.cfg.repo})" + (f"; initial fetch failed: {err}" if err else ""))

    def _brief(self, job_id):
        v = self.store.view(job_id)
        return {"id": v["id"], "state": v["state"], "sha": v["sha"]}

    def _enqueue(self, sha, source, prepare):
        """``prepare(job_id)`` makes the commit available and returns a refs/dev prefix
        or None. Then the checkout is written and the job published."""
        job_id = self.store.new_id(sha)
        incoming = self.store.incoming(job_id)
        try:
            prefix = prepare(job_id)
            self.mirror.write_checkout(sha, incoming / "src", prefix)
            self.store.publish(job_id, sha, self.cfg.repo, source)
        except BaseException:
            self.store.discard_incoming(job_id)
            raise
        self.store.prune(self.cfg.keep)
        log(f"queued {job_id} ({source} {sha[:12]})")
        return 202, {"id": job_id, "state": "queued", "sha": sha}

    def _fetch(self):
        err = self.mirror.fetch_heads()
        if err:
            log(f"fetch failed, using the branches already mirrored: {err}")

    def submit_sha(self, repo, sha):
        if repo != self.cfg.repo:
            raise HttpError(400, f"only {self.cfg.repo} is built here, not {repo!r}")
        if not isinstance(sha, str) or not SHA.match(sha):
            raise HttpError(400, "sha must be a full 40-hex lowercase commit sha")
        with self.lock:
            reuse = self.store.find_reusable(sha)
            if reuse:
                return 202, self._brief(reuse)
            self._fetch()
            try:
                self.mirror.require_branch_commit(sha)
            except Untrusted as e:
                log(f"refused {sha[:12]}: {e}")
                raise HttpError(403, str(e)) from None
            return self._enqueue(sha, "branch", lambda _id: None)

    def submit_bundle(self, sha, bundle_path):
        if not isinstance(sha, str) or not SHA.match(sha):
            raise HttpError(400, "sha must be a full 40-hex lowercase commit sha")
        with self.lock:
            reuse = self.store.find_reusable(sha)
            if reuse:
                return 202, self._brief(reuse)
            self._fetch()
            try:
                return self._enqueue(
                    sha, "bundle",
                    lambda job_id: self.mirror.import_bundle(bundle_path, job_id, sha),
                )
            except BadInput as e:
                raise HttpError(400, str(e)) from None
            except MissingPrerequisites as e:
                raise HttpError(409, str(e)) from None

    def package_tar(self, job_id):
        if not self.store.view(job_id):
            raise HttpError(404, f"no build {job_id}")
        pkg = self.store.package_dir(job_id)
        if not pkg:
            raise HttpError(409, f"build {job_id} is not done")
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w:gz") as tar:
            for p in sorted(pkg.iterdir()):
                if p.is_file() and (p.name == "manifest.json" or p.suffix in (".hex", ".uf2")):
                    info = tar.gettarinfo(str(p), arcname=p.name)
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mode = 0o644
                    with open(p, "rb") as f:
                        tar.addfile(info, f)
        return buf.getvalue()


def parse_multipart(content_type, body):
    """{field: (filename or None, bytes)} from a multipart/form-data body."""
    m = Message()
    m["content-type"] = content_type
    boundary = m.get_param("boundary")
    if m.get_content_type() != "multipart/form-data" or not boundary:
        raise HttpError(400, "expected multipart/form-data with a boundary")
    delim = b"--" + boundary.encode("latin-1")
    chunks = body.split(delim)
    if len(chunks) < 3 or not chunks[-1].startswith(b"--"):
        raise HttpError(400, "malformed multipart body")
    fields = {}
    for chunk in chunks[1:-1]:
        if not chunk.startswith(b"\r\n") or not chunk.endswith(b"\r\n"):
            raise HttpError(400, "malformed multipart part")
        head, sep, data = chunk[2:-2].partition(b"\r\n\r\n")
        if not sep:
            raise HttpError(400, "malformed multipart part headers")
        hm = Message()
        for line in head.decode("latin-1").split("\r\n"):
            k, _, v = line.partition(":")
            if k.strip():
                hm[k.strip()] = v.strip()
        name = hm.get_param("name", header="content-disposition")
        if not name:
            raise HttpError(400, "multipart part without a name")
        if name in fields:
            raise HttpError(400, f"multipart field {name!r} given twice")
        fields[name] = (hm.get_param("filename", header="content-disposition"), data)
    return fields


class Handler(BaseHTTPRequestHandler):
    server_version = "blinky-build"
    sys_version = ""
    protocol_version = "HTTP/1.1"
    timeout = 120
    service = None  # set by make_server

    def log_message(self, fmt, *args):
        log(f"{self.address_string()} {fmt % args}")

    # ------------------------------------------------------------- responses ---

    def _send(self, code, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else (json.dumps(body) + "\n").encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _error(self, code, message, close=False):
        extra = {"WWW-Authenticate": 'Bearer realm="blinky-build"'} if code == 401 else {}
        if close:
            extra["Connection"] = "close"
            self.close_connection = True
        self._send(code, {"error": message}, extra=extra)

    def _authorized(self):
        auth = self.headers.get("Authorization", "")
        scheme, _, token = auth.partition(" ")
        if scheme.lower() != "bearer" or not token:
            return False
        return hmac.compare_digest(token.strip().encode(), self.service.cfg.token.encode())

    def _body(self, limit):
        if self.headers.get("Transfer-Encoding"):
            raise HttpError(411, "send a Content-Length (no chunked bodies)")
        try:
            n = int(self.headers.get("Content-Length", ""))
        except ValueError:
            raise HttpError(411, "Content-Length required") from None
        if n < 0 or n > limit:
            raise HttpError(413, f"body too large ({n} bytes, limit {limit})")
        data = bytearray()
        while len(data) < n:
            chunk = self.rfile.read(min(n - len(data), 1 << 20))
            if not chunk:
                raise HttpError(400, "body shorter than Content-Length")
            data += chunk
        return bytes(data)

    # ---------------------------------------------------------------- routes ---

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/v1/healthz":
            return self._send(200, {"ok": True})
        if not self._authorized():
            return self._error(401, "missing or bad bearer token")
        parts = path.strip("/").split("/")
        try:
            if len(parts) in (3, 4) and parts[:2] == ["v1", "builds"] and ID_RE.match(parts[2]):
                job_id = parts[2]
                if len(parts) == 3:
                    v = self.service.store.view(job_id)
                    if not v:
                        raise HttpError(404, f"no build {job_id}")
                    return self._send(200, v)
                if parts[3] == "package":
                    data = self.service.package_tar(job_id)
                    sha = self.service.store.meta(job_id)["sha"]
                    return self._send(200, data, "application/gzip", {
                        "Content-Disposition": f'attachment; filename="dev-firmware-{sha}.tar.gz"'
                    })
            raise HttpError(404, "not found")
        except HttpError as e:
            return self._error(e.code, e.message)

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if not self._authorized():
            # The body is never read: close rather than leave it in the stream.
            return self._error(401, "missing or bad bearer token", close=True)
        try:
            if path != "/v1/builds":
                raise HttpError(404, "not found")
            ctype = self.headers.get("Content-Type", "")
            if ctype.split(";")[0].strip().lower() == "application/json":
                try:
                    req = json.loads(self._body(MAX_JSON))
                except ValueError:
                    raise HttpError(400, "body is not JSON") from None
                if not isinstance(req, dict) or set(req) - {"repo", "sha"}:
                    raise HttpError(400, 'expected {"repo": ..., "sha": ...}')
                code, body = self.service.submit_sha(req.get("repo"), req.get("sha"))
            elif ctype.split(";")[0].strip().lower() == "multipart/form-data":
                fields = parse_multipart(ctype, self._body(self.service.cfg.max_bundle + 65536))
                if set(fields) - {"bundle", "sha", "repo"} or "bundle" not in fields:
                    raise HttpError(400, "multipart fields: bundle (file), sha, optional repo")
                repo = fields.get("repo", (None, None))[1]
                if repo is not None and repo.decode("utf-8", "replace") != self.service.cfg.repo:
                    raise HttpError(400, f"only {self.service.cfg.repo} is built here")
                sha = fields.get("sha", (None, b""))[1].decode("utf-8", "replace").strip()
                bundle = fields["bundle"][1]
                if len(bundle) > self.service.cfg.max_bundle:
                    raise HttpError(413, "bundle too large")
                fd, tmp = tempfile.mkstemp(suffix=".bundle", dir=self.service.cfg.data / "tmp")
                try:
                    with os.fdopen(fd, "wb") as f:
                        f.write(bundle)
                    code, body = self.service.submit_bundle(sha, tmp)
                finally:
                    os.unlink(tmp)
            else:
                raise HttpError(400, "Content-Type must be application/json or multipart/form-data")
            return self._send(code, body)
        except HttpError as e:
            return self._error(e.code, e.message, close=e.code in (411, 413))
        except Exception as e:  # never a stack trace to the client; the log has it
            log(f"internal error on POST: {e!r}")
            return self._error(500, "internal error (see the service log)")


def make_server(service):
    handler = type("BoundHandler", (Handler,), {"service": service})
    srv = ThreadingHTTPServer((service.cfg.host, service.cfg.port), handler)
    srv.daemon_threads = True
    return srv


def main():
    cfg = Config.from_env()
    service = Service(cfg)
    service.start()
    srv = make_server(service)
    log(f"listening on port {cfg.port}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
