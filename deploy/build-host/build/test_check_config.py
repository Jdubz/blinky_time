"""check_config.py: the build service's compose config is refused when it loosens the sandbox."""

from __future__ import annotations

import copy
import importlib.util
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("check_config", HERE / "check_config.py")
cc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cc)


def fenced(**extra):
    return {"user": "10001:10001", "cap_drop": ["ALL"], "security_opt": ["no-new-privileges:true"],
            "cpuset": "0-1", "mem_limit": "1073741824", "memswap_limit": "1073741824", "pids_limit": 256,
            **extra}


def vol(source, target):
    return {"type": "volume", "source": source, "target": target, "volume": {}}


# The shape `docker compose config --format json` gives for compose.yaml (placeholder host values).
GOOD = {
    "services": {
        "api": fenced(read_only=True, volumes=[vol("mirror", "/data"), vol("jobs", "/jobs")],
                      ports=[{"mode": "ingress", "host_ip": "192.0.2.10", "target": 18080,
                              "published": "18080", "protocol": "tcp"}]),
        "worker": fenced(network_mode="none", volumes=[vol("jobs", "/jobs"), vol("cache", "/cache")]),
    },
    "volumes": {"mirror": {"name": "blinky-build-mirror"}, "jobs": {"name": "blinky-build-jobs"},
                "cache": {"name": "blinky-build-cache"}},
}


def with_(path, value):
    cfg = copy.deepcopy(GOOD)
    *parents, last = path
    node = cfg
    for k in parents:
        node = node[k]
    node[last] = value
    return cfg


def test_the_shipped_shape_passes():
    assert cc.problems(GOOD) == []


@pytest.mark.parametrize("path,value", [
    (("services", "worker", "network_mode"), "bridge"),
    (("services", "worker", "user"), "0:0"),
    (("services", "api", "cap_add"), ["NET_ADMIN"]),
    (("services", "worker", "privileged"), True),
    (("services", "worker", "memswap_limit"), "-1"),
    (("services", "api", "read_only"), False),
    (("services", "worker", "volumes"), [vol("jobs", "/jobs"), {"type": "bind", "source": "/", "target": "/cache"}]),
    (("services", "api", "ports"), [{"host_ip": "0.0.0.0", "target": 1, "published": "1"}]),
    # Loosenings the first version let through:
    (("services", "worker", "security_opt"), ["no-new-privileges:true", "seccomp=unconfined"]),
    (("services", "api", "security_opt"), ["no-new-privileges:true", "apparmor=unconfined"]),
    (("services", "worker", "userns_mode"), "host"),
    (("services", "worker", "uts"), "host"),
    (("services", "worker", "cgroup"), "host"),
    (("services", "worker", "volumes_from"), ["api"]),
    (("services", "api", "network_mode"), "host"),
    (("volumes", "cache"), {"name": "c", "driver": "local", "driver_opts": {"type": "none", "o": "bind", "device": "/"}}),
    (("volumes", "jobs"), {"name": "j", "external": True}),
])
def test_a_loosened_config_is_refused(path, value):
    assert cc.problems(with_(path, value)), f"{path}={value!r} was accepted"
