"""Telemetry: Prometheus metrics at ``/metrics`` and OpenTelemetry traces over OTLP.

On the test box both go to the central LGTM stack on bignasty (Josh-Wentworth/observability)
through the box's Alloy agent (scripts/testbox/alloy/blinkytest.alloy). Alloy scrapes
``/metrics`` on loopback and receives OTLP on 127.0.0.1:4318, then pushes to Prometheus and Tempo.

- **Metrics** are read at scrape time. Device health comes from the last ``json info`` each
  device answered (the liveness ping already asks, so scraping adds no device traffic). Flash,
  workflow, step and staging metrics are recorded where those things finish.
- **Traces**: a span per durable workflow and per step (``work/durable.py``), per flash job
  (``device/manager.py``) and per HTTP request. Off unless ``OTEL_EXPORTER_OTLP_ENDPOINT`` is
  set; the server logs which it is at startup.
- **Logs** written inside a span end with `` trace_id=<32 hex>``, which Alloy lifts into Loki
  structured metadata so Grafana can jump from a log line to its trace.

Labels stay low-cardinality: device serials (a handful), workflow and step names (a fixed set),
transports and outcomes. Never job ids or SHAs.
"""

from __future__ import annotations

import json
import logging
import os
import socket
import time
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

from opentelemetry import trace
from prometheus_client import CONTENT_TYPE_LATEST, Counter, Gauge, Histogram, generate_latest
from prometheus_client.core import REGISTRY, GaugeMetricFamily
from prometheus_client.registry import Collector

log = logging.getLogger(__name__)

tracer = trace.get_tracer("blinky_server")

# -- metrics recorded where things finish ------------------------------------------------------

FLASH_JOBS = Counter(
    "blinky_flash_jobs_total",
    "Flash jobs that reached a terminal state.",
    ["transport", "state", "source"],
)
FLASH_SECONDS = Histogram(
    "blinky_flash_duration_seconds",
    "Flash job wall time, start to terminal state.",
    ["transport", "state"],
    buckets=(10, 20, 30, 45, 60, 90, 120, 180, 300, 600),
)
WORKFLOWS = Counter(
    "blinky_workflow_runs_total",
    "Durable workflow runs that ended, by outcome (a run cut off by a drain resumes later).",
    ["workflow", "outcome"],
)
STEP_SECONDS = Histogram(
    "blinky_step_duration_seconds",
    "Durable step wall time, by outcome. A replayed step returns its record almost at once.",
    ["step", "outcome"],
    buckets=(0.1, 0.5, 1, 5, 15, 30, 60, 120, 300, 900, 1800),
)
STAGING_LAST_RUN = Gauge(
    "blinky_staging_last_run_timestamp_seconds", "When the last staging suite report was written."
)
STAGING_LAST_PASSED = Gauge(
    "blinky_staging_last_passed", "1 if the last staging suite passed, else 0."
)
STAGING_CHECK = Gauge(
    "blinky_staging_check_passed",
    "Each check of the last staging suite, per device: 1 passed, 0 failed.",
    ["device", "section", "check"],
)


def record_flash(job: Any) -> None:
    """A terminal FlashJob (manager's audit writer calls this once per job)."""
    transport = job.transport.value if job.transport else "none"
    state = job.state.value
    FLASH_JOBS.labels(
        transport, state, "auto_recovery" if job.is_auto_recovery else "operator"
    ).inc()
    if job.finished_at:  # from creation: pre-write failures and preparation count too
        FLASH_SECONDS.labels(transport, state).observe(job.finished_at - job.created_at)


def record_staging(report: dict[str, Any], written_at: float | None = None) -> None:
    """The staging suite's report, as written. Replaces the previous run's check gauges."""
    STAGING_LAST_RUN.set(time.time() if written_at is None else written_at)
    STAGING_LAST_PASSED.set(1 if report.get("passed") else 0)
    STAGING_CHECK.clear()
    for section in ("health", "music", "final"):
        for device, checks in (report.get(section) or {}).items():
            for name, c in (checks or {}).items():
                if isinstance(c, dict) and "pass" in c:
                    STAGING_CHECK.labels(device, section, name).set(1 if c["pass"] else 0)
    val = report.get("validation") or {}
    if "pass" in val:
        for device in report.get("devices", []):
            STAGING_CHECK.labels(device, "validation", "played").set(1 if val["pass"] else 0)
    dep = report.get("deploy") or {}
    if "pass" in dep:
        for device in report.get("devices", []):
            STAGING_CHECK.labels(device, "deploy", "deploy").set(1 if dep["pass"] else 0)


def restore_staging(reports_dir: Path) -> None:
    """At startup: the newest report on disk sets the staging gauges, with its write time, so
    a restart doesn't make the last suite look failed (or never run) until the next one."""
    reports = sorted(reports_dir.glob("*.json"), key=lambda p: p.stat().st_mtime)
    if not reports:
        return
    newest = reports[-1]
    try:
        report = json.loads(newest.read_text())
    except (OSError, ValueError) as e:
        log.warning("staging metrics not restored from %s: %s", newest, e)
        return
    record_staging(report, written_at=newest.stat().st_mtime)


# -- device health, read at scrape time --------------------------------------------------------

# json info field -> (metric suffix, help, scale)
_INFO_FIELDS: tuple[tuple[str, str, str, float], ...] = (
    ("fps", "fps", "Frames per second over the firmware's last closed 5 s window.", 1.0),
    ("maxFrameMs", "max_frame_ms", "Longest frame in the last closed 5 s window, ms.", 1.0),
    ("minFrameMs", "min_frame_ms", "Shortest frame in the last closed 5 s window, ms.", 1.0),
    ("audioOverruns", "audio_overruns", "Mic buffer overruns since boot.", 1.0),
    ("audioSamplesLost", "audio_samples_lost", "Mic samples lost since boot.", 1.0),
    ("millis", "uptime_seconds", "Device uptime.", 0.001),
    ("heapUsed", "heap_used_bytes", "Heap in use.", 1.0),
    ("loopStackFree", "loop_stack_free_bytes", "Least free loop-task stack ever seen.", 1.0),
    ("onsetUs", "onset_us", "Onset path time per frame, µs.", 1.0),
    ("onsetMaxUs", "onset_max_us", "Longest onset path time per frame, µs.", 1.0),
)


class FleetCollector(Collector):
    """Per-device gauges from the fleet's state and each device's last ``json info``."""

    def __init__(self, get_devices: Callable[[], list[Any]]) -> None:
        self._get_devices = get_devices

    def collect(self) -> Iterator[GaugeMetricFamily]:
        up = GaugeMetricFamily(
            "blinky_device_up", "1 if the server holds a working connection.", labels=["device"]
        )
        info = GaugeMetricFamily(
            "blinky_device_info",
            "Constant 1, carrying the device's identity as labels.",
            labels=["device", "device_type", "version", "platform", "transport", "state"],
        )
        info_age = GaugeMetricFamily(
            "blinky_device_info_age_seconds",
            "Seconds since the device last answered json info.",
            labels=["device"],
        )
        fields = {
            key: GaugeMetricFamily(f"blinky_device_{suffix}", help_, labels=["device"])
            for key, suffix, help_, _ in _INFO_FIELDS
        }
        now = time.monotonic()
        for d in self._get_devices():
            state = getattr(d.state, "value", str(d.state))
            up.add_metric([d.id], 1.0 if state == "connected" else 0.0)
            info.add_metric(
                [
                    d.id,
                    d.device_type or "",
                    d.version or "",
                    d.platform or "",
                    getattr(d.transport, "transport_type", "") or "",
                    state,
                ],
                1.0,
            )
            last = getattr(d, "last_info", None)
            at = getattr(d, "last_info_at", None)
            if at is not None:
                info_age.add_metric([d.id], now - at)
            if not last:
                continue
            for key, _suffix, _help, scale in _INFO_FIELDS:
                v = last.get(key)
                if isinstance(v, (int, float)) and not isinstance(v, bool):
                    fields[key].add_metric([d.id], float(v) * scale)
        yield up
        yield info
        yield info_age
        yield from fields.values()


_fleet_collector: FleetCollector | None = None


def register_fleet(get_devices: Callable[[], list[Any]]) -> None:
    """Expose the fleet's devices at /metrics (once per process)."""
    global _fleet_collector
    if _fleet_collector is not None:
        REGISTRY.unregister(_fleet_collector)
    _fleet_collector = FleetCollector(get_devices)
    REGISTRY.register(_fleet_collector)


# -- the box's mode and the hardware queue behind it (testing/box_mode.py) ---------------------


class BoxCollector(Collector):
    """``blinky_box_mode{mode}`` (1 for the mode the box is in), and the work waiting behind
    dev mode: ``blinky_hw_queue_depth`` and ``blinky_hw_queue_oldest_seconds`` (0 when none).
    A switch to dev still waiting for its suite counts as queue (it is, until the suite ends)."""

    def collect(self) -> Iterator[GaugeMetricFamily]:
        from .testing import box_mode

        try:
            st = box_mode.snapshot()
        except OSError as e:  # the data dir; never fail a scrape over it
            log.warning("box mode metrics: %s", e)
            return
        mode = GaugeMetricFamily(
            "blinky_box_mode",
            "1 for the test box's mode (queue: runners submit; dev: dev builds, runners hold).",
            labels=["mode"],
        )
        for m in box_mode.MODES:
            mode.add_metric([m], 1.0 if st["mode"] == m else 0.0)
        waiting = st["waiting"]
        depth = GaugeMetricFamily(
            "blinky_hw_queue_depth", "Hardware runs waiting behind the box's dev mode."
        )
        depth.add_metric([], float(len(waiting)))
        oldest = GaugeMetricFamily(
            "blinky_hw_queue_oldest_seconds",
            "How long the oldest hardware run has waited behind dev mode (0: none waiting).",
        )
        since = [float(i.get("since") or 0.0) for i in waiting]
        oldest.add_metric([], max(0.0, time.time() - min(since)) if since else 0.0)
        yield mode
        yield depth
        yield oldest


REGISTRY.register(BoxCollector())


def metrics_body() -> tuple[bytes, str]:
    return generate_latest(REGISTRY), CONTENT_TYPE_LATEST


# -- traces ------------------------------------------------------------------------------------


def init_tracing(staging: bool) -> bool:
    """Install the OTLP tracer provider if ``OTEL_EXPORTER_OTLP_ENDPOINT`` is set. True if on.

    The exporter reads the endpoint (and any OTEL_* settings) from the environment, so the unit
    file is the one place it's configured. Unset, spans are no-ops and the server says so."""
    endpoint = os.environ.get("OTEL_EXPORTER_OTLP_ENDPOINT")
    if not endpoint:
        log.info("Tracing off: OTEL_EXPORTER_OTLP_ENDPOINT is not set")
        return False
    from opentelemetry.exporter.otlp.proto.http.trace_exporter import OTLPSpanExporter
    from opentelemetry.sdk.resources import Resource
    from opentelemetry.sdk.trace import TracerProvider
    from opentelemetry.sdk.trace.export import BatchSpanProcessor

    resource = Resource.create(
        {
            "service.name": "blinky-server",
            "service.namespace": "blinky",
            "deployment.environment": "test" if staging else "prod",
            "host.name": socket.gethostname(),
        }
    )
    provider = TracerProvider(resource=resource)
    provider.add_span_processor(BatchSpanProcessor(OTLPSpanExporter()))
    trace.set_tracer_provider(provider)
    log.info("Tracing on: OTLP to %s", endpoint)
    return True


def shutdown_tracing() -> None:
    """Flush buffered spans (the drain is over by the time this runs)."""
    provider = trace.get_tracer_provider()
    shutdown = getattr(provider, "shutdown", None)
    if shutdown is not None:
        shutdown()


class TraceIdFilter(logging.Filter):
    """Adds ``record.trace_ctx``: `` trace_id=<hex>`` inside a recording span, else ""."""

    def filter(self, record: logging.LogRecord) -> bool:
        ctx = trace.get_current_span().get_span_context()
        record.trace_ctx = f" trace_id={ctx.trace_id:032x}" if ctx.is_valid else ""
        return True
