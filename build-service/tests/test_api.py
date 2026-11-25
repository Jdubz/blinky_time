"""Auth, the build cache, job views and the package endpoint."""

import io
import json
import tarfile

import pytest
from conftest import TOKEN

from blinky_build import api


def fake_done(service, job_id, n=211, sha=None):
    sha = sha or service.store.meta(job_id)["sha"]
    pkg = service.cfg.jobs / job_id / "package"
    pkg.mkdir()
    (pkg / "manifest.json").write_text(json.dumps({"version": f"b{n}-dev-{sha[:8]}",
                                                    "git_sha": sha}))
    (pkg / f"blinky-things-b{n}.hex").write_text(":00000001FF\n")
    (pkg / f"blinky-things-b{n}.uf2").write_bytes(b"UF2")
    service.store.set_status(job_id, state="done", version=f"b{n}-dev-{sha[:8]}")


def test_healthz_needs_no_auth(client):
    assert client.req("GET", "/v1/healthz", token=None) == (200, {"ok": True})


@pytest.mark.parametrize("token", [None, "wrong" * 10, TOKEN + "x", TOKEN[:-1]])
def test_auth_required(client, remote, token):
    assert client.post_sha(remote.tip, token=token)[0] == 401
    assert client.req("GET", "/v1/builds/abc", token=token)[0] == 401
    assert client.req("GET", "/v1/builds/abc/package", token=token)[0] == 401


def test_auth_scheme_must_be_bearer(client, service, remote):
    import urllib.error
    import urllib.request

    r = urllib.request.Request(client.base + "/v1/builds/abc", headers={"Authorization": TOKEN})
    with pytest.raises(urllib.error.HTTPError) as e:
        urllib.request.urlopen(r)
    assert e.value.code == 401


def test_no_jobs_before_auth(client, service, remote):
    client.post_sha(remote.tip, token=None)
    assert service.store.ids() == []


def test_config_has_no_default_address():
    with pytest.raises(SystemExit):
        api.Config.from_env({"BLINKY_BUILD_TOKEN": "x" * 40, "BLINKY_BUILD_PORT": "1"})
    with pytest.raises(SystemExit):
        api.Config.from_env({"BLINKY_BUILD_TOKEN": "x" * 40, "BLINKY_BUILD_HOST": "h"})
    with pytest.raises(SystemExit):  # token too short
        api.Config.from_env({"BLINKY_BUILD_TOKEN": "short", "BLINKY_BUILD_HOST": "h",
                             "BLINKY_BUILD_PORT": "1"})
    cfg = api.Config.from_env({"BLINKY_BUILD_TOKEN": "x" * 40, "BLINKY_BUILD_HOST": "h",
                               "BLINKY_BUILD_PORT": "1"})
    assert cfg.repo == "Jdubz/blinky-firmware"
    assert cfg.remote == "https://github.com/Jdubz/blinky-firmware.git"


def test_view_and_unknown(client, remote):
    code, body = client.post_sha(remote.tip)
    code, view = client.req("GET", f"/v1/builds/{body['id']}")
    assert code == 200
    assert {k: view[k] for k in ("id", "state", "sha", "version", "error", "log_tail")} == {
        "id": body["id"], "state": "queued", "sha": remote.tip, "version": None, "error": None,
        "log_tail": "",
    }
    assert client.req("GET", "/v1/builds/nope")[0] == 404
    assert client.req("GET", "/v1/builds/..%2f..%2fetc")[0] == 404


def test_cache_returns_done_build(client, service, remote):
    _, first = client.post_sha(remote.tip)
    fake_done(service, first["id"])
    code, again = client.post_sha(remote.tip)
    assert code == 202 and again == {"id": first["id"], "state": "done", "sha": remote.tip}
    assert len(service.store.ids()) == 1


def test_inflight_build_is_returned_not_duplicated(client, service, remote):
    _, first = client.post_sha(remote.tip)
    _, again = client.post_sha(remote.tip)
    assert again["id"] == first["id"] and again["state"] == "queued"


def test_failed_build_is_retried(client, service, remote):
    _, first = client.post_sha(remote.tip)
    service.store.set_status(first["id"], state="failed", error="toolchain mismatch")
    code, again = client.post_sha(remote.tip)
    assert code == 202 and again["id"] != first["id"] and again["state"] == "queued"


def test_cache_spans_paths(client, service, remote, tmp_path):
    _, first = client.post_sha(remote.tip)
    fake_done(service, first["id"])
    b = remote.bundle(tmp_path / "c.bundle", [f"{remote.base}..main"])
    code, again = client.post_bundle(remote.tip, b.read_bytes())
    assert code == 202 and again["id"] == first["id"] and again["state"] == "done"


def test_package_409_until_done_then_tar(client, service, remote):
    _, b = client.post_sha(remote.tip)
    code, body = client.req("GET", f"/v1/builds/{b['id']}/package")
    assert code == 409
    fake_done(service, b["id"])
    (service.cfg.jobs / b["id"] / "package" / "stray.txt").write_text("not shipped")
    code, data = client.req("GET", f"/v1/builds/{b['id']}/package", raw=True)
    assert code == 200
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        names = sorted(tar.getnames())
    assert names == ["blinky-things-b211.hex", "blinky-things-b211.uf2", "manifest.json"]


def test_prune_keeps_inflight(service, remote):
    store = service.store
    ids = []
    for state in ("done", "failed", None):
        job_id = store.new_id(remote.tip) + (state or "q")
        store.incoming(job_id)
        store.publish(job_id, remote.tip, "r", "branch")
        if state:
            store.set_status(job_id, state=state)
        ids.append(job_id)
    assert store.prune(0) == ids[:2]
    assert store.ids() == [ids[2]]
    assert not (service.cfg.data / "jobs" / f"{ids[0]}.json").exists()


def test_pruned_bundle_refs_are_deleted(client, service, remote, tmp_path):
    from blinky_build import gitutil

    new = remote.commit_file("blinky-things/main.cpp", "int main() { return 21; }\n", "dev")
    b = remote.bundle(tmp_path / "p.bundle", [f"{remote.tip}..HEAD"])
    _, first = client.post_bundle(new, b.read_bytes())
    service.store.set_status(first["id"], state="failed", error="x")
    service.cfg.keep = 1
    client.post_sha(remote.tip)  # a new job prunes the old one, and its refs/dev
    refs = gitutil.out("--git-dir", service.mirror.path, "for-each-ref", "--format=%(refname)",
                       "refs/dev/")
    assert refs == ""
    assert first["id"] not in service.store.ids()


def test_package_must_be_of_the_jobs_commit(client, service, remote):
    _, b = client.post_sha(remote.tip)
    fake_done(service, b["id"], sha=remote.base)  # the worker claims another commit
    assert client.req("GET", f"/v1/builds/{b['id']}/package")[0] == 500


def test_api_ignores_worker_symlinks_and_specials(client, service, remote):
    """Build code can write the job volume: the api must not follow a symlink there (a
    build.log linked to /proc/self/environ would leak the token) or read a FIFO."""
    import os

    _, b = client.post_sha(remote.tip)
    d = service.cfg.jobs / b["id"]
    (d / "build.log").symlink_to("/proc/self/environ")
    secret = d.parent / "secret.json"
    secret.write_text(json.dumps({"state": "done", "version": "TOKEN-LEAK"}))
    (d / "status.json").symlink_to(secret)
    code, view = client.req("GET", f"/v1/builds/{b['id']}")
    assert code == 200 and "TOKEN-LEAK" not in json.dumps(view) and TOKEN not in view["log_tail"]
    assert view["state"] == "failed"  # a status.json it can't read is not "queued" forever
    (d / "status.json").unlink()
    os.mkfifo(d / "status.json")
    assert client.req("GET", f"/v1/builds/{b['id']}")[1]["state"] == "failed"


def test_worker_cannot_rewrite_the_apis_record(client, service, remote):
    _, b = client.post_sha(remote.tip)
    job_json = service.cfg.jobs / b["id"] / "job.json"
    job_json.write_text(json.dumps({"id": b["id"], "sha": remote.base, "created_at": 0}))
    fake_done(service, b["id"], sha=remote.tip)
    # The cache still files this build under remote.tip, not the sha the worker wrote.
    assert client.post_sha(remote.base)[1]["id"] != b["id"]
    assert client.req("GET", f"/v1/builds/{b['id']}")[1]["sha"] == remote.tip


def test_corrupt_status_is_failed_and_retried(client, service, remote):
    _, b = client.post_sha(remote.tip)
    (service.cfg.jobs / b["id"] / "status.json").write_text("{not json")
    assert client.req("GET", f"/v1/builds/{b['id']}")[1]["state"] == "failed"
    assert client.post_sha(remote.tip)[1]["id"] != b["id"]


def test_post_errors_close_the_connection(client, remote):
    import http.client
    from urllib.parse import urlparse

    u = urlparse(client.base)
    conn = http.client.HTTPConnection(u.hostname, u.port, timeout=30)
    conn.request("POST", "/v1/nope", body=b"GET /v1/healthz HTTP/1.1\r\n\r\n",
                 headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "text/plain"})
    r = conn.getresponse()
    r.read()
    assert r.status == 404 and r.getheader("Connection") == "close"


def test_job_checkout_is_not_made_on_the_shared_volume(service, remote):
    # Staged privately (next to the mirror), then copied: nothing left behind in either.
    code, _ = service.submit_sha("Jdubz/blinky-firmware", remote.tip)
    assert code == 202
    assert not list((service.cfg.data / "stage").iterdir())


def test_multipart_boundary_inside_content_is_data():
    body = (b"--B\r\nContent-Disposition: form-data; name=\"bundle\"; filename=\"x\"\r\n\r\n"
            b"data--Bmore\n--B not a delimiter\r\n--B--\r\n")
    f = api.parse_multipart("multipart/form-data; boundary=B", body)
    assert f["bundle"] == ("x", b"data--Bmore\n--B not a delimiter")


def test_multipart_parser():
    body = (b"--B\r\nContent-Disposition: form-data; name=\"sha\"\r\n\r\nabc\r\n"
            b"--B\r\nContent-Disposition: form-data; name=\"bundle\"; filename=\"x\"\r\n\r\n"
            b"\r\n--\x00\xff\r\n\r\n--B--\r\n")
    f = api.parse_multipart("multipart/form-data; boundary=B", body)
    assert f["sha"] == (None, b"abc")
    assert f["bundle"] == ("x", b"\r\n--\x00\xff\r\n")
    with pytest.raises(api.HttpError):
        api.parse_multipart("multipart/form-data; boundary=B", b"--B\r\nno end")
    with pytest.raises(api.HttpError):
        api.parse_multipart("multipart/form-data", body)
