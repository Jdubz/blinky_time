"""Heavy CI jobs as checks (checks.py; docs/DEV_LOOP.md section 1, "Later (phase 3)"): the
plan of a job's steps, the api's trust for the commit and its corpus pin, and the worker."""

import json

import pytest
import yaml
from conftest import WORKFLOW, run_git

from blinky_build import checks

IMAGE = {
    "python": "3.11",
    "arduino_cli": "1.3.1",
    "pip": {"numpy", "imageio-ffmpeg", "pytest"},
    "tools": {"the-compiler"},
}


def wf(text=WORKFLOW):
    return yaml.safe_load(text)


# -- the plan ----------------------------------------------------------------------------------


def test_download_steps_are_the_images_and_the_rest_runs_in_order():
    steps = checks.plan(wf(), "tracker-harness", IMAGE)
    assert [(s.name, bool(s.provided)) for s in steps] == [
        ("Checkout repository", True),
        ("Corpus submodule", True),
        ("Setup Python", True),
        ("Install Python deps", True),
        ("Corpus regression gate", False),
        ("After the gate", False),
    ]
    assert "numpy, imageio-ffmpeg, pytest" in steps[3].provided


def test_step_env_reaches_the_step_and_the_toolchain_steps_are_provided():
    steps = checks.plan(wf(), "tracker-harness-nn", IMAGE)
    assert [s.name for s in steps if s.provided] == [
        "actions/checkout@v5", "Setup Arduino CLI", "Install TFLite Micro library"]
    assert steps[-1].env == {"CXX": "the-compiler"}


@pytest.mark.parametrize(
    "edit,why",
    [
        (("run: git submodule update --init --depth 1 corpus",
          "run: |\n        git submodule update --init --depth 1 corpus\n        echo x"),
         "a download step mixed"),
        (("python-version: '3.11'", "python-version: '3.12'"), "Python '3.12'"),
        (("echo after", "echo ${{ github.sha }}"), "uses an expression"),
        (("    - name: After the gate\n", "    - name: After the gate\n      if: always()\n"),
         "if not supported"),
        (("uses: actions/setup-python@v5", "uses: someone/else@v1"), "action someone/else"),
    ],
)
def test_a_step_the_worker_cant_run_as_written_is_refused_never_dropped(edit, why):
    text = WORKFLOW.replace(*edit)
    assert text != WORKFLOW
    with pytest.raises(checks.Unsupported, match=why):
        checks.plan(wf(text), "tracker-harness", IMAGE)


def test_a_pip_package_the_image_lacks_runs_and_fails_loudly_instead_of_passing():
    # Alone on its line: it is not provided, so it runs (and, with no network, fails).
    steps = checks.plan(wf(WORKFLOW.replace("numpy imageio-ffmpeg pytest", "scipy")),
                        "tracker-harness", IMAGE)
    assert steps[3].provided is None and steps[3].script.strip() == "pip install scipy"


def test_a_compiler_the_image_lacks_is_refused_not_failed():
    with pytest.raises(checks.Unsupported, match="CXX=the-compiler, which the image doesn't"):
        checks.plan(wf(), "tracker-harness-nn", {**IMAGE, "tools": set()})


def test_only_the_heavy_jobs_are_checks():
    with pytest.raises(checks.Unsupported, match="not a check"):
        checks.plan(wf(), "changes", IMAGE)
    with pytest.raises(checks.Unsupported, match="has no job"):
        checks.plan({"jobs": {}}, "tracker-harness", IMAGE)


# -- the api -----------------------------------------------------------------------------------


def post_check(client, sha, check="tracker-harness", repo="Jdubz/blinky-firmware"):
    body = json.dumps({"repo": repo, "sha": sha, "check": check}).encode()
    return client.req("POST", "/v1/checks", body, "application/json")


def test_a_branch_commit_gets_a_check_with_the_corpus_it_pins(client, service, remote):
    code, b = post_check(client, remote.tip)
    assert code == 202 and b["state"] == "queued" and b["check"] == "tracker-harness"
    job = service.cfg.jobs / b["id"]
    assert (job / "corpus/edm/track.json").read_text() == '{"bpm": 120}\n'
    assert not (job / "corpus/.git").exists()  # the tree only
    view = client.req("GET", f"/v1/checks/{b['id']}")[1]
    assert view["corpus"] == remote.corpus_pin and view["conclusion"] is None
    # The same check of the same commit is that job; the other check is its own.
    assert post_check(client, remote.tip)[1]["id"] == b["id"]
    assert post_check(client, remote.tip, "tracker-harness-nn")[1]["id"] != b["id"]
    # A check is not a build, and a build of the commit is not this check.
    assert client.req("GET", f"/v1/builds/{b['id']}")[0] == 404
    assert client.post_sha(remote.tip)[1]["id"] != b["id"]


def test_a_fork_commit_or_a_fork_corpus_pin_is_refused(client, remote):
    code, b = post_check(client, remote.fork)
    assert code == 403 and "not on any branch" in b["error"]
    run_git(remote.work, "update-index", "--cacheinfo", f"160000,{remote.corpus_fork},corpus")
    run_git(remote.work, "commit", "-q", "-m", "pin a fork's corpus commit")
    remote.push()
    code, b = post_check(client, remote.head())
    assert code == 403 and b["error"].startswith("its corpus pin:")


def test_bad_check_requests(client, remote):
    assert post_check(client, remote.tip, "compile")[0] == 400
    assert post_check(client, remote.tip, repo="someone/fork")[0] == 400
    run_git(remote.work, "rm", "-q", "--cached", "corpus")
    run_git(remote.work, "commit", "-q", "-m", "no corpus")
    remote.push()
    code, b = post_check(client, remote.head())
    assert code == 400 and "pins no corpus" in b["error"]


# -- the worker --------------------------------------------------------------------------------


@pytest.fixture
def checker(worker_env, monkeypatch):
    w = worker_env()
    monkeypatch.setattr(type(w), "image", lambda self: IMAGE)
    return w


def test_a_passing_check_reports_each_step(client, service, remote, checker):
    _, b = post_check(client, remote.tip)
    assert checker.run_once()
    v = client.req("GET", f"/v1/checks/{b['id']}")[1]
    assert v["state"] == "done" and v["conclusion"] == "success", v
    assert [(s["name"], s["outcome"]) for s in v["steps"]] == [
        ("Checkout repository", "provided"),
        ("Corpus submodule", "provided"),
        ("Setup Python", "provided"),
        ("Install Python deps", "provided"),
        ("Corpus regression gate", "success"),
        ("After the gate", "success"),
    ]
    assert 'gate {"bpm": 120}' in v["log_tail"]
    assert not (service.cfg.jobs / b["id"] / "corpus").exists()


def test_a_failing_step_fails_the_check_and_skips_the_rest(client, remote, checker):
    remote.commit_file("FAIL_GATE", "1\n", "break the gate")
    remote.push()
    _, b = post_check(client, remote.head())
    checker.run_once()
    v = client.req("GET", f"/v1/checks/{b['id']}")[1]
    assert v["state"] == "done" and v["conclusion"] == "failure"
    assert [s["outcome"] for s in v["steps"]][-2:] == ["failure", "skipped"]
    assert "drum F below the floor" in v["log_tail"]


def test_a_job_the_image_cant_run_is_an_infrastructure_error_not_a_verdict(
    client, remote, checker
):
    remote.commit_file(".github/workflows/pr-validation.yml",
                       WORKFLOW.replace("python-version: '3.11'", "python-version: '3.13'"),
                       "newer python")
    remote.push()
    _, b = post_check(client, remote.head())
    checker.run_once()
    v = client.req("GET", f"/v1/checks/{b['id']}")[1]
    assert v["state"] == "failed" and v["conclusion"] is None
    assert v["error"].startswith("can't run as written:") and "3.13" in v["error"]


def test_the_step_env_reaches_the_step(client, remote, checker):
    _, b = post_check(client, remote.tip, "tracker-harness-nn")
    checker.run_once()
    v = client.req("GET", f"/v1/checks/{b['id']}")[1]
    assert v["conclusion"] == "success" and "built with the-compiler" in v["log_tail"]


def test_untrusted_worker_fields_are_typed_and_capped(service, remote):
    _, b = service.submit_check("Jdubz/blinky-firmware", remote.tip, "tracker-harness")
    service.store.set_status(b["id"], state="done", conclusion="maybe",
                             steps=[{"name": "x" * 500, "outcome": "pwned", "seconds": "a"}, 7])
    v = service.store.view(b["id"])
    assert v["conclusion"] == "failure"
    assert v["steps"] == [{"name": "x" * 200, "outcome": "failure", "seconds": None}]
