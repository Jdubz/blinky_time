"""The strict rules' section of a run's report (blinky-server testing/strict_rules.py; docs/DEV_LOOP.md,
"Strict rules"), for the staging runner, the PR queue and dev.py. Stdlib only.

A report's ``strict`` is {"rules": [one row per rule], "devices": {...}}; each row has its rule,
mode (warn | block), status (pass | warn | fail | no data), worst value and where it was read,
the threshold, the blinky-firmware issues it waits on, and the windows it excluded (a settings
save the server sent) or, for detection_latency, the tracks it left out (``left_out``: too few
kicks detected for the music's check). Warn rows never change a status; the description may say
how many.
"""

from __future__ import annotations

from typing import Any

FW_ISSUES = "https://github.com/Jdubz/blinky-firmware/issues/"
MARK = {"pass": "✅ pass", "warn": "⚠️ warn", "fail": "❌ FAIL (blocks)", "no data": "— no data"}
TEXT = {"pass": "pass", "warn": "WARN", "fail": "FAIL (blocks)", "no data": "no data"}


def rows(strict: dict[str, Any] | None) -> list[dict[str, Any]]:
    return list((strict or {}).get("rules") or [])


def warnings(strict: dict[str, Any] | None) -> int:
    return sum(r.get("status") == "warn" for r in rows(strict))


def suffix(strict: dict[str, Any] | None) -> str:
    """For a commit status's description: " · 2 strict warnings" (nothing without any)."""
    n = warnings(strict)
    return f" · {n} strict warning{'' if n == 1 else 's'}" if n else ""


def _num(v: Any) -> str:
    return f"{float(v):g}" if isinstance(v, (int, float)) else "?"


def limit(r: dict[str, Any]) -> str:
    op = {"max": "≤", "min": "≥", "abs": "±"}.get(str(r.get("worst_is")), "≥")
    sep = "" if op == "±" else " "
    return f"{op}{sep}{_num(r.get('threshold'))} {r.get('unit') or ''}".rstrip()


def worst(r: dict[str, Any]) -> str:
    if r.get("worst") is None:
        return ""
    v = r["worst"]
    # A signed offset (an "abs" rule) keeps its sign: +3 ms is later, -3 sooner.
    signed = r.get("worst_is") == "abs" and isinstance(v, (int, float))
    shown = f"{float(v):+g}" if signed else _num(v)
    return f"{shown} {r.get('unit') or ''}".rstrip()


def where(r: dict[str, Any]) -> str:
    parts = [str(r[k]) for k in ("where", "at") if r.get(k)]
    if r.get("samples"):
        parts.append(f"{r['samples']} reads")
    if r.get("device") and len(str(r["device"])) > 0:
        parts.append(str(r["device"])[:8])
    return ", ".join(parts)


def title(strict: dict[str, Any] | None) -> str:
    blocking = [r["rule"] for r in rows(strict) if r.get("mode") == "block"]
    if not blocking:
        return "⚠ strict rules (non-blocking)"
    return f"strict rules ({', '.join(blocking)} block; the rest warn)"


def excluded_lines(strict: dict[str, Any] | None) -> list[str]:
    """Every excluded window, said: "frame_time: the window read at hh:mm:ss.mmm (where, N ms)
    excluded: settings save at hh:mm:ss.mmm (blinky-firmware#49)"; and every track a rule left
    out: "detection_latency: dubstep-edm-halftime (+57.2 ms) left out: kickR50 0.25 < 0.5: ..."."""
    out = []
    for r in rows(strict):
        for e in r.get("excluded") or []:
            out.append(
                f"{r['rule']}: the window read at {e.get('read_at')} ({e.get('where')}, "
                f"{_num(e.get('max_frame_ms'))} ms) excluded: {e.get('reason')}"
            )
        for e in r.get("left_out") or []:
            off = e.get("offset_ms")
            shown = f" ({float(off):+g} ms)" if isinstance(off, (int, float)) else ""
            out.append(f"{r['rule']}: {e.get('track')}{shown} left out: {e.get('why')}")
    return out


def md_lines(strict: dict[str, Any] | None) -> list[str]:
    """The section as markdown: a table (rule, result, worst, where, limit, waits on), then
    the exclusions, never silent."""
    rs = rows(strict)
    if not rs:
        return []
    lines = [
        f"**{title(strict)}**",
        "",
        "| rule | | worst | where | limit | waits on |",
        "|---|---|---|---|---|---|",
    ]
    for r in rs:
        issues = ", ".join(f"[blinky-firmware#{n}]({FW_ISSUES}{n})" for n in r.get("issues") or [])
        lines.append(
            f"| {r['rule']} | {MARK.get(r['status'], r['status'])} | {worst(r)} | "
            f"{where(r).replace('|', '/')} | {limit(r)} | {issues or '—'} |"
        )
    lines.append("")
    ex = excluded_lines(strict)
    if ex:
        lines += [*(f"- {e}" for e in ex), ""]
    lines.append(
        "A warning never changes a status (owner, 2026-11-25); a rule blocks only once the owner "
        "sets its mode to block in blinky-server `testing/strict_rules.py`."
    )
    return lines


def text_lines(strict: dict[str, Any] | None) -> list[str]:
    """The section as plain text, one line per rule (dev.py)."""
    rs = rows(strict)
    if not rs:
        return []
    lines = [f"{title(strict)}:"]
    for r in rs:
        issues = ", ".join(f"blinky-firmware#{n}" for n in r.get("issues") or [])
        detail = ", ".join(x for x in (worst(r), where(r)) if x)
        lines.append(
            f"  {r['rule']}: {TEXT.get(r['status'], r['status'])}"
            + (f" ({detail})" if detail else "")
            + f", limit {limit(r)}"
            + (f", waits on {issues}" if issues else "")
        )
    lines += [f"  {e}" for e in excluded_lines(strict)]
    return lines


def readiness_text(got: dict[str, Any]) -> list[str]:
    """GET /api/test/strict-rules as plain text: each rule's mode and streak (dev.py status)."""
    n = got.get("ready_after")
    out = [f"strict rules (ready to block after {n} builds in a row; the owner switches them):"]
    for rule, r in (got.get("readiness") or {}).items():
        tail = ": READY to block" if r.get("ready") and r.get("mode") == "warn" else ""
        broke = r.get("broken_by") or {}
        last = (
            f", last warning {str(broke.get('version') or broke.get('sha') or '?')[:24]}"
            if broke
            else ""
        )
        out.append(f"  {rule}: {r.get('mode')}, streak {r.get('streak')}/{n}{last}{tail}")
    return out
