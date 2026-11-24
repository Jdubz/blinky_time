"""Staging reports in the Spark artifact store, and stable box links to artifacts."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest
from httpx import AsyncClient

from blinky_server.spark import service as spark_service
from blinky_server.spark.client import Client, NotFound, SparkError
from blinky_server.testing import staging

store_report = staging.store_report.__wrapped__  # type: ignore[attr-defined]  # the step's body


def test_presign_returns_urls_and_refuses_a_missing_key(monkeypatch: pytest.MonkeyPatch) -> None:
    c = Client("http://spark", "k")
    sent: list[Any] = []

    def fake_json(method: str, path: str, obj: Any = None) -> dict[str, Any]:
        sent.append((method, path, obj))
        keys = obj["keys"]
        return {
            "urls": {k: f"https://store/{k}?sig" for k in keys if k != "gone"},
            "missing": [k for k in keys if k == "gone"],
        }

    monkeypatch.setattr(c, "_json", fake_json)
    assert c.presign(["inputs/a.json"], 600) == {"inputs/a.json": "https://store/inputs/a.json?sig"}
    assert sent[0] == ("POST", "/artifacts/presign", {"keys": ["inputs/a.json"], "expiry_s": 600})
    with pytest.raises(NotFound):
        c.presign(["gone"])


class _FakeClient:
    def __init__(self, upload: Any = None, presign: Any = None) -> None:
        self._upload, self._presign = upload, presign

    def upload(self, path: Path) -> str:
        return self._upload(path)  # type: ignore[no-any-return]

    def presign(self, keys: list[str], expiry_s: int = 3600) -> dict[str, str]:
        return self._presign(keys)  # type: ignore[no-any-return]


@pytest.fixture
def spark(monkeypatch: pytest.MonkeyPatch) -> Any:
    def install(client: _FakeClient | None) -> None:
        svc = SimpleNamespace(client=client) if client is not None else None
        monkeypatch.setattr(spark_service, "_service", svc)

    yield install
    spark_service.set_service(None)


async def test_a_report_is_archived_and_its_key_returned(spark: Any, tmp_path: Path) -> None:
    report = tmp_path / "r.json"
    report.write_text("{}")
    spark(_FakeClient(upload=lambda p: f"inputs/x/{p.name}"))
    assert await store_report(str(report)) == "inputs/x/r.json"


async def test_archiving_never_fails_a_report(spark: Any, tmp_path: Path) -> None:
    report = tmp_path / "r.json"
    report.write_text("{}")
    spark(None)  # the Spark client isn't configured
    assert await store_report(str(report)) is None

    def boom(p: Path) -> str:
        raise SparkError("POST /artifacts -> HTTP 503")

    spark(_FakeClient(upload=boom))
    assert await store_report(str(report)) is None


async def test_the_artifact_link_redirects_to_a_fresh_signed_url(
    api_client: AsyncClient, spark: Any
) -> None:
    def presign(keys: list[str]) -> dict[str, str]:
        if keys == ["inputs/gone.json"]:
            raise NotFound("not in the artifact store: ['inputs/gone.json']")
        return {keys[0]: f"https://artifacts.example/{keys[0]}?sig"}

    spark(_FakeClient(presign=presign))
    r = await api_client.get("/api/spark/artifacts/inputs/abc/report.json", follow_redirects=False)
    assert r.status_code == 307
    assert r.headers["location"] == "https://artifacts.example/inputs/abc/report.json?sig"
    r = await api_client.get("/api/spark/artifacts/inputs/gone.json", follow_redirects=False)
    assert r.status_code == 404
    spark(None)
    r = await api_client.get("/api/spark/artifacts/inputs/abc/report.json", follow_redirects=False)
    assert r.status_code == 503
