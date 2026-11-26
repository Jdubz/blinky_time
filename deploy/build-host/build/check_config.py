#!/usr/bin/env python3
"""Refuse a build-service compose config that loosens the sandbox (docs/DEV_LOOP.md, "Trust").

Reads `docker compose config --format json` on stdin. deploy.sh runs it before every deploy (with a
placeholder token, so the config it reads holds no secret); CI runs it on the same file with
placeholder host values. Prints the api's bind address and port on success. Stdlib only.

The rules: the worker has no network, no published port, no host mount (only its jobs and cache
volumes), no docker socket, no capabilities and no new privileges, runs as uid 10001 and is capped on
memory (swap included), PIDs and CPUs. The api runs read-only as uid 10001 with no capabilities and
publishes exactly one port, on a specific address (never a wildcard).
"""

from __future__ import annotations

import json
import sys

USER = "10001:10001"
WILDCARD = {"", "0.0.0.0", "::", "[::]"}


def problems(cfg: dict) -> list[str]:
    out: list[str] = []
    svcs = cfg.get("services", {})
    if set(svcs) != {"api", "worker"}:
        return [f"services are {sorted(svcs)}, expected api and worker"]
    for name, s in svcs.items():
        if s.get("user") != USER:
            out.append(f"{name}: user {s.get('user')!r}, expected {USER}")
        if s.get("cap_drop") != ["ALL"] or s.get("cap_add"):
            out.append(f"{name}: capabilities changed (cap_drop {s.get('cap_drop')}, cap_add {s.get('cap_add')})")
        if "no-new-privileges:true" not in (s.get("security_opt") or []):
            out.append(f"{name}: no-new-privileges missing")
        if s.get("privileged"):
            out.append(f"{name}: privileged")
        for v in s.get("volumes") or []:
            if v.get("type") != "volume":
                out.append(f"{name}: a {v.get('type')} mount ({v.get('source')} -> {v.get('target')})")
        if not s.get("cpuset"):
            out.append(f"{name}: no cpuset")
        mem, swap = s.get("mem_limit"), s.get("memswap_limit")
        if not mem or mem != swap:
            out.append(f"{name}: memory cap {mem!r} / swap {swap!r} (must be set and equal)")
        if not s.get("pids_limit"):
            out.append(f"{name}: no pids_limit")
        if s.get("devices") or s.get("pid") == "host" or s.get("ipc") == "host":
            out.append(f"{name}: host devices or namespaces")

    w = svcs["worker"]
    if w.get("network_mode") != "none":
        out.append(f"worker: network_mode {w.get('network_mode')!r}, must be none")
    if w.get("ports") or w.get("networks"):
        out.append("worker: has ports or networks")
    if sorted(v.get("target") for v in w.get("volumes") or []) != ["/cache", "/jobs"]:
        out.append(f"worker: volumes {[v.get('target') for v in w.get('volumes') or []]}, expected /jobs and /cache")
    if "BLINKY_BUILD_TOKEN" in (w.get("environment") or {}):
        out.append("worker: holds the api token")

    a = svcs["api"]
    if not a.get("read_only"):
        out.append("api: root filesystem is writable")
    ports = a.get("ports") or []
    if len(ports) != 1:
        out.append(f"api: publishes {len(ports)} ports, expected 1")
    elif ports[0].get("host_ip", "") in WILDCARD:
        out.append(f"api: binds {ports[0].get('host_ip', '')!r}, a wildcard (a specific address only)")
    return out


def main() -> int:
    cfg = json.load(sys.stdin)
    bad = problems(cfg)
    if bad:
        for p in bad:
            print(f"refusing: {p}", file=sys.stderr)
        return 1
    p = cfg["services"]["api"]["ports"][0]
    print(p["host_ip"], p["published"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
