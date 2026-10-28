"""Spark Job API transport: upload, submit, read a job. Contract: my-spark docs/architecture/api.md."""

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
                raise SparkError(f"{method} {path} -> HTTP {e.code}: {detail}") from None
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

    def submit(self, spec: dict[str, Any]) -> dict[str, Any]:
        return self._json("POST", "/jobs", spec)

    def job(self, job_id: str) -> dict[str, Any]:
        return self._json("GET", f"/jobs/{urllib.parse.quote(job_id)}")
