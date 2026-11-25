"""The worker against a stub firmware tree: package, toolchain check, persistence, recovery."""

import hashlib
import json
import time

from conftest import TOOLCHAIN, run_git

from blinky_build import toolchain


def test_build_produces_a_staging_style_package(client, service, remote, worker_env):
    w = worker_env()
    _, b = client.post_sha(remote.tip)
    assert w.run_once()
    code, view = client.req("GET", f"/v1/builds/{b['id']}")
    assert view["state"] == "done", view
    assert view["version"] == f"b211-dev-{remote.tip[:8]}"
    assert view["error"] is None and "done in" in view["log_tail"]
    pkg = service.cfg.jobs / b["id"] / "package"
    assert sorted(p.name for p in pkg.iterdir()) == [
        "blinky-things-b211.hex", "blinky-things-b211.uf2", "manifest.json"]
    m = json.loads((pkg / "manifest.json").read_text())
    assert m["channel"] == "dev" and m["git_sha"] == remote.tip
    # The toolchain JSON is the staging build's: these keys, in this order.
    assert list(m["toolchain"]) == ["core", "libraries", "fqbn", "tflite_ref", "arduino_cli"]
    assert m["toolchain"]["arduino_cli"].startswith("arduino-cli  Version: 1.3.1")
    assert m["toolchain"]["tflite_ref"] == "48a62b6e75862ceca82924e48618f197750b1299"
    for name, digest in m["files"].items():
        assert hashlib.sha256((pkg / name).read_bytes()).hexdigest() == digest
    assert not (service.cfg.jobs / b["id"] / "src").exists()  # removed after the build
    assert not w.run_once()  # nothing left


def test_toolchain_mismatch_fails_clearly(client, service, remote, worker_env):
    other = toolchain.fingerprint(remote.work)
    other["toolchain"] = {**other["toolchain"], "core": "Seeeduino:nrf52@1.1.9"}
    w = worker_env(other)
    _, b = client.post_sha(remote.tip)
    w.run_once()
    view = client.req("GET", f"/v1/builds/{b['id']}")[1]
    assert view["state"] == "failed"
    assert view["error"].startswith("toolchain mismatch: rebuild the worker image")
    assert "core: commit 'Seeeduino:nrf52@1.1.10', image 'Seeeduino:nrf52@1.1.9'" in view["error"]
    assert not (service.cfg.jobs / b["id"] / "package").exists()


def test_toolchain_mismatch_on_patch_or_tflite(remote):
    fp = toolchain.fingerprint(remote.work)
    changed = json.loads(json.dumps(fp))
    changed["patches"]["tinyusb-cdc-patched-marker.patch"] = "0" * 64
    changed["tflite_ref"] = "1" * 40
    diffs = toolchain.differences(fp, changed)
    assert any(d.startswith("patches/tinyusb-cdc-patched-marker.patch") for d in diffs)
    assert any(d.startswith("tflite_ref") for d in diffs)
    # The comment key is not part of the pin.
    pin = dict(TOOLCHAIN, _comment="reworded")
    (remote.work / "tools/firmware_toolchain.json").write_text(json.dumps(pin))
    assert toolchain.differences(toolchain.fingerprint(remote.work), fp) == []


def test_build_failure_reports_log(client, service, remote, worker_env):
    new = remote.commit_file("FAIL_BUILD", "1\n", "broken")
    remote.push()
    w = worker_env()
    _, b = client.post_sha(new)
    w.run_once()
    view = client.req("GET", f"/v1/builds/{b['id']}")[1]
    assert view["state"] == "failed" and "scripts/build.sh --dev failed" in view["error"]
    assert "build broken on purpose" in view["log_tail"]
    assert "x.cpp:1: the real error" in view["log_tail"]  # compile.log's tail, not build.sh's grep
    assert (service.cfg.jobs / b["id"] / "compile.log").is_file()


def test_incremental_tree_keeps_unchanged_mtimes(client, service, remote, worker_env, tmp_path):
    w = worker_env()
    _, b1 = client.post_sha(remote.tip)
    w.run_once()
    src = tmp_path / "src"
    other = src / "blinky-things/other.cpp"
    main = src / "blinky-things/main.cpp"
    other_mtime, main_mtime = other.stat().st_mtime_ns, main.stat().st_mtime_ns
    time.sleep(0.05)
    new = remote.commit_file("blinky-things/main.cpp", "int main() { return 42; }\n", "change")
    remote.push()
    _, b2 = client.post_sha(new)
    w.run_once()
    assert client.req("GET", f"/v1/builds/{b2['id']}")[1]["state"] == "done"
    assert other.stat().st_mtime_ns == other_mtime  # untouched: arduino-cli reuses its object
    assert main.stat().st_mtime_ns > main_mtime  # changed: rebuilt
    assert run_git(src, "rev-parse", "HEAD") == new


def test_build_cannot_persist_git_config(client, service, remote, worker_env, tmp_path):
    w = worker_env()
    _, b1 = client.post_sha(remote.tip)
    w.run_once()
    cfg = tmp_path / "src/.git/config"
    cfg.write_text(cfg.read_text() + "[core]\n\thooksPath = scripts\n")
    (tmp_path / "src/.git/info").mkdir(exist_ok=True)
    (tmp_path / "src/.git/info/attributes").write_text("* filter=evil\n")
    _, b2 = client.post_sha(remote.base)
    w.run_once()
    assert client.req("GET", f"/v1/builds/{b2['id']}")[1]["state"] == "done"
    assert "hooksPath = scripts" not in cfg.read_text()
    assert not (tmp_path / "src/.git/info/attributes").exists()


def test_build_cache_is_per_toolchain_and_stale_ones_pruned(client, remote, worker_env, tmp_path):
    w = worker_env()
    _, b = client.post_sha(remote.tip)
    w.run_once()
    mine = f"tc-{toolchain.fingerprint_id(w.baked)}"
    assert (tmp_path / "cache" / mine / "tmp").is_dir()
    # arduino-cli's build cache ($XDG_CACHE_HOME/arduino) is on the cache volume, not in HOME.
    marker = tmp_path / "cache" / mine / "xdg-cache" / "arduino" / "last-build"
    assert marker.read_text().strip() == remote.tip[:8]
    (tmp_path / "cache" / "tc-0123456789abcdef").mkdir()
    w.prune_caches()
    assert sorted(p.name for p in (tmp_path / "cache").iterdir()) == [mine]


def test_recover_marks_interrupted_builds_failed(client, service, remote, worker_env):
    w = worker_env()
    _, b = client.post_sha(remote.tip)
    service.store.claim(b["id"])
    service.store.set_status(b["id"], state="building")
    w.recover()
    view = client.req("GET", f"/v1/builds/{b['id']}")[1]
    assert view["state"] == "failed" and "restarted" in view["error"]
    # A resubmit builds again.
    _, again = client.post_sha(remote.tip)
    assert again["id"] != b["id"]
    assert w.run_once()
    assert client.req("GET", f"/v1/builds/{again['id']}")[1]["state"] == "done"


def test_bundle_build_end_to_end(client, service, remote, worker_env, tmp_path):
    new = remote.commit_file("blinky-things/main.cpp", "int main() { return 77; }\n", "dev")
    b = remote.bundle(tmp_path / "e.bundle", [f"{remote.tip}..HEAD"])
    _, job = client.post_bundle(new, b.read_bytes())
    worker_env().run_once()
    view = client.req("GET", f"/v1/builds/{job['id']}")[1]
    assert view["state"] == "done" and view["version"] == f"b211-dev-{new[:8]}"
