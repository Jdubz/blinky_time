"""blinky_server/telemetry.py: what /metrics exposes and how log lines carry trace ids."""

from __future__ import annotations

import logging
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest
from httpx import AsyncClient
from opentelemetry.sdk.trace import TracerProvider
from prometheus_client import CollectorRegistry

from blinky_server import telemetry
from blinky_server.firmware.flash_job import FlashJob, FlashJobState, FlashTransport


def _value(name: str, labels: dict[str, str]) -> float | None:
    return telemetry.REGISTRY.get_sample_value(name, labels)


def test_a_terminal_flash_job_counts_and_times() -> None:
    labels = {"transport": "uf2", "state": "completed", "source": "operator"}
    before = _value("blinky_flash_jobs_total", labels) or 0.0
    job = FlashJob(device_id="dev-1", firmware_path=Path("/tmp/fw.hex"))
    job.transport = FlashTransport.UF2
    job.transition(FlashJobState.SELECTING_TRANSPORT)
    job.transition(FlashJobState.WRITING)
    job.transition(FlashJobState.VERIFYING)
    job.transition(FlashJobState.COMPLETED)
    telemetry.record_flash(job)
    assert _value("blinky_flash_jobs_total", labels) == before + 1
    assert (
        _value("blinky_flash_duration_seconds_count", {"transport": "uf2", "state": "completed"})
        or 0
    ) >= 1


def test_the_staging_report_sets_one_gauge_per_check_and_replaces_the_last_run() -> None:
    telemetry.record_staging(
        {
            "passed": False,
            "devices": ["CHIP"],
            "deploy": {"pass": True},
            "health": {"CHIP": {"fps": {"pass": True}, "frametime": {"pass": False}}},
            "validation": {"pass": True},
        }
    )
    assert _value("blinky_staging_last_passed", {}) == 0
    check = "blinky_staging_check_passed"
    assert _value(check, {"device": "CHIP", "section": "health", "check": "frametime"}) == 0
    assert _value(check, {"device": "CHIP", "section": "health", "check": "fps"}) == 1
    assert _value(check, {"device": "CHIP", "section": "deploy", "check": "deploy"}) == 1
    assert _value(check, {"device": "CHIP", "section": "validation", "check": "played"}) == 1
    telemetry.record_staging({"passed": True, "devices": ["CHIP"], "health": {}})
    assert _value("blinky_staging_last_passed", {}) == 1
    # The previous run's checks are gone, not left stale.
    assert _value(check, {"device": "CHIP", "section": "health", "check": "frametime"}) is None


def _device(**kw: Any) -> SimpleNamespace:
    base = {
        "id": "87DE9B58C90AFE2D",
        "state": SimpleNamespace(value="connected"),
        "device_type": "testbox_grid_v1",
        "version": "b217",
        "platform": "nrf52840",
        "transport": SimpleNamespace(transport_type="serial"),
        "last_info": None,
        "last_info_at": None,
    }
    base.update(kw)
    return SimpleNamespace(**base)


def test_device_health_comes_from_the_last_json_info() -> None:
    reg = CollectorRegistry()
    live = _device(
        last_info={"fps": 190.5, "maxFrameMs": 33, "audioOverruns": 0, "millis": 61_000},
        last_info_at=0.0,
    )
    gone = _device(id="OTHER", state=SimpleNamespace(value="disconnected"))
    reg.register(telemetry.FleetCollector(lambda: [live, gone]))
    v = reg.get_sample_value
    assert v("blinky_device_up", {"device": live.id}) == 1
    assert v("blinky_device_up", {"device": "OTHER"}) == 0
    assert v("blinky_device_fps", {"device": live.id}) == 190.5
    assert v("blinky_device_max_frame_ms", {"device": live.id}) == 33
    assert v("blinky_device_uptime_seconds", {"device": live.id}) == 61.0
    assert v("blinky_device_fps", {"device": "OTHER"}) is None  # never answered: no number
    info = {
        "device": live.id,
        "device_type": "testbox_grid_v1",
        "version": "b217",
        "platform": "nrf52840",
        "transport": "serial",
        "state": "connected",
    }
    assert v("blinky_device_info", info) == 1


def test_log_lines_carry_the_trace_id_only_inside_a_span() -> None:
    f = telemetry.TraceIdFilter()
    rec = logging.LogRecord("t", logging.INFO, __file__, 1, "msg", None, None)
    f.filter(rec)
    assert rec.trace_ctx == ""
    tracer = TracerProvider().get_tracer("test")
    with tracer.start_as_current_span("s") as span:
        f.filter(rec)
        assert rec.trace_ctx == f" trace_id={span.get_span_context().trace_id:032x}"


def test_tracing_stays_off_without_an_endpoint(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("OTEL_EXPORTER_OTLP_ENDPOINT", raising=False)
    assert telemetry.init_tracing(staging=True) is False


async def test_metrics_endpoint_serves_prometheus_text(api_client: AsyncClient) -> None:
    telemetry.register_fleet(lambda: [_device()])  # the lifespan does this on a real start
    resp = await api_client.get("/metrics")
    assert resp.status_code == 200
    assert resp.headers["content-type"].startswith("text/plain")
    assert "blinky_device_up" in resp.text


def test_a_restart_restores_the_staging_gauges_from_the_newest_report(tmp_path: Any) -> None:
    import json as _json
    import os as _os

    old = tmp_path / "old.json"
    old.write_text(_json.dumps({"passed": False, "devices": ["CHIP"]}))
    _os.utime(old, (1_000, 1_000))
    new = tmp_path / "new.json"
    new.write_text(
        _json.dumps(
            {"passed": True, "devices": ["CHIP"], "health": {"CHIP": {"fps": {"pass": True}}}}
        )
    )
    _os.utime(new, (2_000, 2_000))
    telemetry.restore_staging(tmp_path)
    assert _value("blinky_staging_last_passed", {}) == 1
    assert _value("blinky_staging_last_run_timestamp_seconds", {}) == 2_000
    check = {"device": "CHIP", "section": "health", "check": "fps"}
    assert _value("blinky_staging_check_passed", check) == 1


def test_no_reports_leave_the_staging_gauges_alone(tmp_path: Any) -> None:
    telemetry.record_staging({"passed": True, "devices": []}, written_at=5.0)
    telemetry.restore_staging(tmp_path)
    assert _value("blinky_staging_last_run_timestamp_seconds", {}) == 5.0
