"""Bundles: prerequisites must be branch commits; contents land in refs/dev/<id>/ only."""

import pytest
from conftest import run_git

from blinky_build import gitutil
from blinky_build.bundle import BundleError, parse_header


def refs(service, prefix=""):
    text = gitutil.out("--git-dir", service.mirror.path, "for-each-ref", "--format=%(refname)",
                       *( [prefix] if prefix else []))
    return text.splitlines()


def test_bundle_on_a_branch_builds(client, service, remote, tmp_path):
    run_git(remote.work, "checkout", "-q", "-b", "feature")
    new = remote.commit_file("blinky-things/main.cpp", "int main() { return 4; }\n", "dev work")
    b = remote.bundle(tmp_path / "f.bundle", [f"{remote.tip}..feature"])
    heads_before = refs(service, "refs/heads/")
    code, body = client.post_bundle(new, b.read_bytes())
    assert code == 202, body
    assert body == {"id": body["id"], "state": "queued", "sha": new}
    # Unbundled into refs/dev/<id>/, never into refs/heads/ (no new branch, nothing moved).
    assert refs(service, f"refs/dev/{body['id']}/") == [f"refs/dev/{body['id']}/0"]
    assert refs(service, "refs/heads/") == heads_before
    assert not service.mirror.branches_containing(new)
    src = service.cfg.jobs / body["id"] / "src"
    assert run_git(src, "rev-parse", "HEAD") == new
    assert run_git(src, "rev-list", "--count", "HEAD") == "3"


def test_bundle_of_head_ref(client, service, remote, tmp_path):
    new = remote.commit_file("blinky-things/main.cpp", "int main() { return 5; }\n", "dev")
    b = remote.bundle(tmp_path / "h.bundle", [f"{remote.tip}..HEAD"])
    assert parse_header(b).refs[0][1] == "HEAD"
    code, body = client.post_bundle(new, b.read_bytes())
    assert code == 202, body


def test_bundle_prerequisite_missing_is_409(client, service, remote, tmp_path):
    local = remote.commit_file("blinky-things/main.cpp", "int main() { return 6; }\n", "local")
    new = remote.commit_file("blinky-things/other.cpp", "int other() { return 6; }\n", "on top")
    b = remote.bundle(tmp_path / "m.bundle", [f"{local}..HEAD"])  # needs `local`, never pushed
    code, body = client.post_bundle(new, b.read_bytes())
    assert code == 409 and "not in the mirror" in body["error"] and local[:12] in body["error"]
    assert refs(service, "refs/dev/") == []


def test_bundle_prerequisite_only_from_another_bundle_is_409(client, service, remote, tmp_path):
    first = remote.commit_file("blinky-things/main.cpp", "int main() { return 7; }\n", "one")
    b1 = remote.bundle(tmp_path / "1.bundle", [f"{remote.tip}..HEAD"])
    assert client.post_bundle(first, b1.read_bytes())[0] == 202
    second = remote.commit_file("blinky-things/other.cpp", "int other() { return 7; }\n", "two")
    b2 = remote.bundle(tmp_path / "2.bundle", [f"{first}..HEAD"])
    code, body = client.post_bundle(second, b2.read_bytes())
    assert code == 409 and "on no branch" in body["error"]


def test_fork_commit_as_prerequisite_is_409(client, service, remote, tmp_path):
    run_git(remote.work, "checkout", "-q", "fork")
    new = remote.commit_file("blinky-things/other.cpp", "int other() { return 8; }\n", "on fork")
    b = remote.bundle(tmp_path / "fk.bundle", [f"{remote.fork}..HEAD"])
    code, body = client.post_bundle(new, b.read_bytes())
    assert code == 409
    assert not service.mirror.has_commit(remote.fork)


def test_garbage_bundle_is_400(client, remote):
    code, body = client.post_bundle(remote.tip[:39] + "0", b"# v2 git bundle\n")
    assert code == 400
    code, body = client.post_bundle("a" * 40, b"\x00\x01 not a bundle at all")
    assert code == 400 and "bundle" in body["error"]


def test_truncated_pack_is_400(client, service, remote, tmp_path):
    new = remote.commit_file("blinky-things/main.cpp", "int main() { return 10; }\n", "dev")
    data = remote.bundle(tmp_path / "t.bundle", [f"{remote.tip}..HEAD"]).read_bytes()
    code, body = client.post_bundle(new, data[: len(data) - 30])
    assert code == 400
    assert refs(service, "refs/dev/") == []


def test_sha_in_neither_bundle_nor_branch_is_400(client, service, remote, tmp_path):
    remote.commit_file("blinky-things/main.cpp", "int main() { return 11; }\n", "dev")
    b = remote.bundle(tmp_path / "n.bundle", [f"{remote.tip}..HEAD"])
    code, body = client.post_bundle("b" * 40, b.read_bytes())
    assert code == 400 and "neither the bundle nor a branch" in body["error"]
    assert refs(service, "refs/dev/") == []


def test_bundle_sha_may_be_a_branch_commit(client, remote, tmp_path):
    remote.commit_file("blinky-things/main.cpp", "int main() { return 12; }\n", "dev")
    b = remote.bundle(tmp_path / "s.bundle", [f"{remote.tip}..HEAD"])
    code, body = client.post_bundle(remote.tip, b.read_bytes())
    assert code == 202 and body["sha"] == remote.tip


def test_bundle_too_large_is_413(client, service, remote):
    service.cfg.max_bundle = 1000
    code, _ = client.post_bundle(remote.tip, b"x" * 70_000)
    assert code == 413


def test_parse_header_refuses_filters_and_bad_lines(tmp_path):
    def hdr(text):
        p = tmp_path / "x.bundle"
        p.write_bytes(text.encode() + b"\n\nPACK")
        return p

    with pytest.raises(BundleError):
        parse_header(hdr("# v3 git bundle\n@filter=blob:none\n" + "a" * 40 + " HEAD"))
    with pytest.raises(BundleError):
        parse_header(hdr("# v3 git bundle\n@object-format=sha256\n" + "a" * 40 + " HEAD"))
    with pytest.raises(BundleError):
        parse_header(hdr("# v2 git bundle\n" + "a" * 40 + " refs/heads/x:refs/heads/y"))
    with pytest.raises(BundleError):
        parse_header(hdr("# v2 git bundle\n-" + "a" * 39))
    with pytest.raises(BundleError):
        parse_header(hdr("# v2 git bundle"))  # no refs
    b = parse_header(hdr("# v3 git bundle\n@object-format=sha1\n-" + "c" * 40 + " msg\n"
                         + "d" * 40 + " refs/heads/main"))
    assert b.prerequisites == ["c" * 40] and b.refs == [("d" * 40, "refs/heads/main")]
