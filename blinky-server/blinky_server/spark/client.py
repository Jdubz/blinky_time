"""Spark Job API transport: upload, fetch an artifact, submit, read a job. Contract: my-spark docs/architecture/api.md."""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path
from typing import Any

# Retried: the request may not have reached a verdict. Never retried: a job that FAILED.
# The platform already spent its own retries on a retryable failure, and a client retry
# would stack on top of them (api.md, "retryable on a terminal failed job").
TRANSPORT_RETRIES = 3
RETRY_STATUS = {502, 503, 504}


class SparkError(RuntimeError):
    pass


class NotFound(SparkError):
    """A 404: for a job, its record was pruned (24 h after it finished) or never existed."""


class Refused(SparkError):
    """Any other 4xx: the Job API looked at the request and said no. Sending it again gets the
    same answer, unlike a transport failure or a 5xx."""


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args: Any, **kwargs: Any) -> None:
        return None  # surface the 3xx as an HTTPError; get_artifact follows it itself


class Client:
    def __init__(self, base_url: str, api_key: str, timeout: float = 60.0) -> None:
        self.base = base_url.rstrip("/")
        self.key = api_key
        self.timeout = timeout

    def _request(
        self, method: str, path: str, body: bytes | None = None, content_type: str | None = None
    ) -> bytes:
        headers = {"X-API-Key": self.key}
        if content_type:
            headers["Content-Type"] = content_type
        req = urllib.request.Request(self.base + path, data=body, method=method, headers=headers)
        for attempt in range(TRANSPORT_RETRIES + 1):
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as r:
                    data: bytes = r.read()
                    return data
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    raise NotFound(f"{method} {path} -> HTTP 404") from None
                if e.code in RETRY_STATUS and attempt < TRANSPORT_RETRIES:
                    time.sleep(2**attempt)
                    continue
                detail = e.read().decode("utf-8", "replace")[:500]
                kind = Refused if 400 <= e.code < 500 else SparkError
                raise kind(f"{method} {path} -> HTTP {e.code}: {detail}") from None
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                if attempt < TRANSPORT_RETRIES:
                    time.sleep(2**attempt)
                    continue
                raise SparkError(f"{method} {path}: {e}") from None
        raise AssertionError("unreachable")

    def _json(self, method: str, path: str, obj: dict[str, Any] | None = None) -> dict[str, Any]:
        body = None if obj is None else json.dumps(obj).encode()
        ctype = "application/json" if body is not None else None
        out: dict[str, Any] = json.loads(self._request(method, path, body, ctype))
        return out

    def upload(self, path: Path) -> str:
        """POST /artifacts (multipart field `file`) -> the artifact key (inputs/ is never reaped)."""
        boundary = uuid.uuid4().hex
        head = (
            f'--{boundary}\r\nContent-Disposition: form-data; name="file"; '
            f'filename="{path.name}"\r\nContent-Type: application/octet-stream\r\n\r\n'
        ).encode()
        body = head + path.read_bytes() + f"\r\n--{boundary}--\r\n".encode()
        data = self._request(
            "POST", "/artifacts", body, f"multipart/form-data; boundary={boundary}"
        )
        uri = json.loads(data).get("uri")
        if not uri:
            raise SparkError(f"upload {path}: response has no uri: {data[:200]!r}")
        return str(uri)

    def get_artifact(self, key: str, max_bytes: int = 64 * 1024 * 1024) -> bytes:
        """GET /artifacts/{key}. The Job API answers 307 to a short-lived presigned URL on the
        store. That URL is followed WITHOUT our API key: urllib would otherwise copy the
        X-API-Key header to the store host."""
        path = "/artifacts/" + urllib.parse.quote(key)
        req = urllib.request.Request(self.base + path, headers={"X-API-Key": self.key})
        opener = urllib.request.build_opener(_NoRedirect)
        try:
            with opener.open(req, timeout=self.timeout) as r:
                data: bytes = r.read(max_bytes + 1)  # a 200 with no redirect
        except urllib.error.HTTPError as e:
            if e.code == 404:
                raise NotFound(f"GET {path} -> HTTP 404") from None
            location = e.headers.get("Location") if e.code in (301, 302, 303, 307, 308) else None
            if not location:
                detail = e.read().decode("utf-8", "replace")[:500]
                raise SparkError(f"GET {path} -> HTTP {e.code}: {detail}") from None
            try:
                with urllib.request.urlopen(location, timeout=self.timeout) as r:
                    data = r.read(max_bytes + 1)
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e2:
                raise SparkError(f"GET {key} from the store: {e2}") from None
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            raise SparkError(f"GET {path}: {e}") from None
        if len(data) > max_bytes:
            raise SparkError(f"{key} is over {max_bytes} bytes")
        return data

    def submit(self, spec: dict[str, Any]) -> dict[str, Any]:
        return self._json("POST", "/jobs", spec)

    def job(self, job_id: str) -> dict[str, Any]:
        return self._json("GET", f"/jobs/{urllib.parse.quote(job_id)}")
