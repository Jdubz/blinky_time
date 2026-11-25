"""The trust check: only commits on the repo's own branches build (docs/DEV_LOOP.md, "Trust")."""

from conftest import REPO, run_git

from blinky_build import gitutil


def test_branch_tip_and_ancestor_are_buildable(client, remote):
    code, body = client.post_sha(remote.tip)
    assert code == 202 and body["state"] == "queued" and body["sha"] == remote.tip
    code, body = client.post_sha(remote.base)
    assert code == 202 and body["sha"] == remote.base


def test_fork_pr_commit_is_refused_and_never_fetched(client, service, remote):
    # The fork commit exists on "GitHub", reachable through refs/pull/1/head only.
    assert run_git(remote.bare, "cat-file", "-t", remote.fork) == "commit"
    code, body = client.post_sha(remote.fork)
    assert code == 403
    assert "not on any branch" in body["error"] and "refs/pull" in body["error"]
    # The mirror fetched branch heads only: the fork's object never came in.
    assert not service.mirror.has_commit(remote.fork)
    refs = gitutil.out("--git-dir", service.mirror.path, "for-each-ref", "--format=%(refname)")
    assert all(r.startswith("refs/heads/") for r in refs.splitlines())


def test_mirror_fetches_only_branch_heads(service, remote):
    git_dir = ("--git-dir", service.mirror.path)
    cfg = gitutil.out(*git_dir, "config", "--get-all", "remote.origin.fetch")
    assert cfg == "+refs/heads/*:refs/heads/*"
    hooks = gitutil.out("--git-dir", service.mirror.path, "config", "core.hooksPath")
    assert hooks == "/dev/null"


def test_unknown_sha_is_refused(client):
    code, body = client.post_sha("0123456789abcdef0123456789abcdef01234567")
    assert code == 403 and "not on any branch" in body["error"]


def test_commit_known_only_from_a_bundle_is_refused_by_sha(client, service, remote, tmp_path):
    new = remote.commit_file("blinky-things/main.cpp", "int main() { return 3; }\n", "dev")
    b = remote.bundle(tmp_path / "d.bundle", [f"{remote.tip}..HEAD"])
    code, _ = client.post_bundle(new, b.read_bytes())
    assert code == 202
    assert service.mirror.has_commit(new)
    # Present in the mirror (refs/dev/*), but not on a branch: the SHA path refuses it
    # unless a build of it already exists (here: the bundle's, in flight, is returned).
    for job_id in service.store.ids():
        service.store.set_status(job_id, state="failed", error="x")
    code, body = client.post_sha(new)
    assert code == 403 and "known here but is not on any branch" in body["error"]


def test_force_pushed_away_commit_is_refused(client, service, remote):
    run_git(remote.work, "reset", "-q", "--hard", remote.base)
    remote.commit_file("blinky-things/main.cpp", "int main() { return 9; }\n", "rewrite")
    remote.push(force=True)
    code, body = client.post_sha(remote.tip)  # was on staging, no longer on any branch
    assert code == 403


def test_new_push_is_seen_without_restart(client, remote):
    new = remote.commit_file("blinky-things/other.cpp", "int other() { return 5; }\n", "more")
    assert client.post_sha(new)[0] == 403
    remote.push()
    code, body = client.post_sha(new)
    assert code == 202 and body["sha"] == new


def test_only_the_one_repo(client, remote):
    code, body = client.post_sha(remote.tip, repo="someone/blinky-firmware")
    assert code == 400 and REPO in body["error"]


def test_bad_sha_and_body(client, remote):
    assert client.post_sha(remote.tip[:12])[0] == 400
    assert client.post_sha(remote.tip.upper())[0] == 400
    assert client.req("POST", "/v1/builds", b"not json", "application/json")[0] == 400
    assert client.req("POST", "/v1/builds", b'{"sha": 1, "repo": "x", "extra": 2}',
                      "application/json")[0] == 400
    assert client.req("POST", "/v1/builds", b"x", "text/plain")[0] == 400


def test_job_checkout_has_history_and_no_hooks(client, service, remote):
    code, body = client.post_sha(remote.tip)
    src = service.cfg.jobs / body["id"] / "src"
    assert run_git(src, "rev-parse", "HEAD") == remote.tip
    assert run_git(src, "rev-list", "--count", "HEAD") == "2"
    assert run_git(src, "config", "core.hooksPath") == "/dev/null"
    assert run_git(src, "status", "--porcelain") == ""
