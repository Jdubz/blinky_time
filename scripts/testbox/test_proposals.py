"""Score-ranked stable proposals (proposals.py; docs/DEV_LOOP.md, "Stable candidates from
scores"): the comparison, its noise, health, the runs it asks for, and its one issue."""

from __future__ import annotations

import importlib.util
import json
import sys
import urllib.error
from pathlib import Path
from typing import Any

import pytest

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE))


def _load(name: str) -> Any:
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


proposals = _load("proposals")
staging = _load("staging")

PIN, OLD_PIN = "c" * 40, "o" * 40
CAND_SHA, STABLE_SHA, TAG_SHA = "a" * 40, "e" * 40, "m" * 40
SETUP = {"devices": ["CHIP"], "limits": {"stable_minutes": 5.0}}
TRACKS = [f"t{i:02d}" for i in range(18)]


def line(
    sha: str,
    values: dict[str, float] | float,
    *,
    run: str,
    corpus: str = PIN,
    release: str | None = None,
    build: str | None = None,
    lag: str = "measured",
    health: dict[str, Any] | None = None,
    tracks: list[str] = TRACKS,
) -> dict[str, Any]:
    """A score line: every metric of every track at ``values`` (a number: every metric; a dict:
    per metric), plus a per-track offset so tracks differ from each other."""
    rows = []
    for i, t in enumerate(tracks):
        v = (
            values
            if isinstance(values, dict)
            else dict.fromkeys(("drumF", "kickR50", "snareR50", "onDrum", "tempoErr"), values)
        )
        rows.append(
            {
                "track": t,
                "device": "CHIP",
                **{k: x + 0.01 * (i % 5) for k, x in v.items()},
                "lagSource": lag,
                "lagAmbiguous": False,
            }
        )
    rec: dict[str, Any] = {
        "sha": sha,
        "run": run,
        "version": f"v-{run}",
        "corpus": corpus,
        **SETUP,
        "at": 1795700000.0,
        "tracks": rows,
        "health": health or {"passed": True, "rebooted": False, "strict": {}},
    }
    if release:
        rec["release"] = release
    if build:
        rec["build"] = build
    return rec


def jitter(rec: dict[str, Any], d: float) -> dict[str, Any]:
    """The same build again, every score moved by +-d alternately (run-to-run noise)."""
    out = json.loads(json.dumps(rec))
    out["run"] += "-again"
    for i, r in enumerate(out["tracks"]):
        for k in ("drumF", "kickR50", "snareR50", "onDrum", "tempoErr"):
            if k in r:
                r[k] += d if i % 2 else -d
    return out


# -- noise and comparison ----------------------------------------------------------------------


def test_noise_is_the_pooled_within_build_spread_over_every_pin() -> None:
    a = line("x" * 40, 0.5, run="1", corpus=OLD_PIN, build="b")
    b = line("y" * 40, 0.7, run="2")
    recs = [a, jitter(a, 0.02), b, jitter(b, 0.02), line("z" * 40, 0.9, run="3")]
    nz = proposals.noise(recs)["drumF"]
    # Pairs differing by 0.02 per track: each run 0.01 from the pair's mean, 1 dof per pair.
    assert nz.dof == 36 and nz.builds == 2
    assert nz.sd == pytest.approx(0.02 / 2**0.5)


def test_a_build_on_two_pins_is_not_a_repeat() -> None:
    a = line("x" * 40, 0.5, run="1", corpus=OLD_PIN)
    b = line("x" * 40, 0.6, run="2")  # same build, another pin: its scores may differ for real
    assert proposals.noise([a, b])["drumF"].dof == 0


def noisy(sd: float = 0.01, dof: int = 30) -> dict[str, Any]:
    return {m.key: proposals.Noise(sd, dof, 1) for m in proposals.METRICS}


def verdicts(results: list[Any]) -> dict[str, str]:
    return {r.metric.key: r.verdict for r in results}


def test_a_lead_beyond_the_noise_is_better_and_one_inside_it_is_not() -> None:
    base = [line(STABLE_SHA, 0.50, run="s", release="b217")]
    cand = [
        line(
            CAND_SHA,
            {"drumF": 0.60, "kickR50": 0.505, "snareR50": 0.5, "onDrum": 0.5, "tempoErr": 0.5},
            run="c",
        )
    ]
    res = proposals.compare(cand, base, noisy(0.05))
    v = verdicts(res)
    assert v["drumF"] == "better" and v["kickR50"] == "within noise"
    assert v["repS"] == "not measured"  # the box doesn't measure it: counts neither way
    drum = next(r for r in res if r.metric.key == "drumF")
    assert drum.diff == pytest.approx(0.10) and drum.tracks == 18
    # K standard errors of a mean over 18 tracks, one run a side: 2 * 0.05 * sqrt(2/18).
    assert drum.margin == pytest.approx(2 * 0.05 * (2 / 18) ** 0.5)


def test_tempo_error_is_better_when_lower() -> None:
    base = [line(STABLE_SHA, {"tempoErr": 0.20}, run="s", release="b217")]
    cand = [line(CAND_SHA, {"tempoErr": 0.05}, run="c")]
    assert verdicts(proposals.compare(cand, base, noisy()))["tempoErr"] == "better"
    assert verdicts(proposals.compare(base, cand, noisy()))["tempoErr"] == "worse"


def test_tracks_with_incomparable_lags_or_one_sided_scores_are_left_out_and_listed() -> None:
    base = [line(STABLE_SHA, 0.5, run="s", release="b217")]
    cand = line(CAND_SHA, 0.5, run="c")
    cand["tracks"][0]["lagSource"] = "acoustic"  # the scorer searched: not the measured lag
    cand["tracks"][1]["drumF"] = None
    res = next(r for r in proposals.compare([cand], base, noisy()) if r.metric.key == "drumF")
    assert res.tracks == 16
    assert res.left_out == {
        "t00": "lag sources differ (acoustic, measured)",
        "t01": "no score from the candidate",
    }


def test_the_body_lists_left_out_tracks_per_metric() -> None:
    gh, st = GitHub(), {}
    recs = leading()
    for t in recs[0]["tracks"]:
        t["tempoErr"] = None  # the candidate's line predates the tempo score
    out = run(gh, st, recs)
    assert (
        "- tempo error (octave-tolerant; lower is better): t00 (no score from the candidate)"
        in (out.text or "")
    )
    assert "- drum F:" not in (out.text or "")


def test_without_measured_noise_a_metric_cannot_be_judged() -> None:
    base = [line(STABLE_SHA, 0.5, run="s", release="b217")]
    cand = [line(CAND_SHA, 0.9, run="c")]
    assert verdicts(proposals.compare(cand, base, noisy(dof=3)))["drumF"] == "noise unknown"
    d = proposals.decide(proposals.compare(cand, base, noisy(dof=3)), [])
    assert not d.qualifies and "noise not measured" in d.why


def test_qualifies_only_with_no_regression_and_at_least_one_gain() -> None:
    base = [line(STABLE_SHA, 0.5, run="s", release="b217")]
    up = {"drumF": 0.6, "kickR50": 0.5, "snareR50": 0.5, "onDrum": 0.5, "tempoErr": 0.5}
    d = proposals.decide(proposals.compare([line(CAND_SHA, up, run="c")], base, noisy()), [])
    assert d.qualifies and "drum F" in d.why
    mixed = {**up, "onDrum": 0.4}
    d = proposals.decide(proposals.compare([line(CAND_SHA, mixed, run="c")], base, noisy()), [])
    assert not d.qualifies and d.why == "worse beyond its noise: on-drum share"
    same = proposals.compare([line(CAND_SHA, 0.5, run="c")], base, noisy())
    assert proposals.decide(same, []).why == "no metric better beyond its noise"


# -- health ------------------------------------------------------------------------------------


def test_health_rules_a_candidate_out() -> None:
    ok = {"rebooted": False, "strict": {"frame_time": {"status": "pass", "worst": 18}}}
    assert proposals.health_problems([ok], "success", {"frame_time"}) == []
    assert proposals.health_problems([ok], "failure", {"frame_time"}) == [
        "`Hardware test (staging)` is failure on its staging commit"
    ]
    rebooted = {**ok, "rebooted": True, "version": "b300-dev-x"}
    assert proposals.health_problems([rebooted], "success", set()) == [
        "a run rebooted (b300-dev-x)"
    ]
    slow = {"rebooted": False, "strict": {"frame_time": {"status": "warn", "worst": 72}}}
    # A rule that blocks now rules it out even if it only warned when the run was made...
    assert proposals.health_problems([slow], "success", {"frame_time"}) == [
        "strict rule `frame_time` (blocking) failed: worst 72"
    ]
    # ...and a warn rule doesn't.
    assert proposals.health_problems([slow], "success", set()) == []
    d = proposals.decide([], ["a run rebooted (x)"])
    assert not d.qualifies and d.why == "health: a run rebooted (x)"


def test_health_comes_from_the_report_for_lines_written_before_it_was_recorded(
    tmp_path: Path,
) -> None:
    report = {
        "passed": True,
        "health": {"CHIP": {"stability": {"rebooted": False}}},
        "final": {"CHIP": {"no_reboot": {"pass": False}}},
        "strict": {"rules": [{"rule": "fps", "status": "pass", "worst": 95.9, "mode": "warn"}]},
    }
    (tmp_path / f"{CAND_SHA}.json").write_text(json.dumps(report))
    rec = {"sha": CAND_SHA}
    got = proposals.health_of(rec, tmp_path)
    assert got == {
        "passed": True,
        "rebooted": True,
        "strict": {"fps": {"status": "pass", "worst": 95.9, "mode": "warn"}},
    }
    assert proposals.health_of({"sha": "q" * 40}, tmp_path) is None


# -- the runs it asks for ----------------------------------------------------------------------


def make_plan(records: list[dict[str, Any]], failed: dict[str, int] | None = None) -> Any:
    return proposals.plan(
        records,
        "b217",
        STABLE_SHA,
        "b228",
        CAND_SHA,
        PIN,
        proposals.noise(records),
        failed or {},
    )


def test_a_candidate_scored_only_under_an_older_pin_is_run_again_as_the_same_build() -> None:
    p = make_plan([line(CAND_SHA, 0.6, run="c", corpus=OLD_PIN, build="h" * 40)])
    assert p.need == {
        "key": f"cand:{CAND_SHA}:{PIN}",
        "what": f"b228's staging commit under corpus {PIN[:8]}",
        "body": {"sha": CAND_SHA, "build_of": "h" * 40},
    }


def test_a_stable_with_no_scores_under_the_pin_is_run() -> None:
    old = line(STABLE_SHA, 0.5, run="s0", release="b217", corpus=OLD_PIN)
    p = make_plan([line(CAND_SHA, 0.6, run="c"), old])
    assert p.need is not None and p.need["key"] == f"base:b217:{PIN}:1"
    assert p.need["body"] == {"sha": STABLE_SHA, "release": "b217"}


def test_a_stable_from_another_setup_doesnt_count() -> None:
    base = line(STABLE_SHA, 0.5, run="s", release="b217")
    base["limits"] = {"stable_minutes": 1.0}
    p = make_plan([line(CAND_SHA, 0.6, run="c"), base])
    assert p.base == [] and p.need is not None and p.need["key"].startswith("base:b217")


def test_without_repeat_runs_the_stable_is_run_again_for_the_noise_up_to_a_cap() -> None:
    cand, base = line(CAND_SHA, 0.6, run="c"), line(STABLE_SHA, 0.5, run="s", release="b217")
    p = make_plan([cand, base])
    assert p.need is not None and p.need["key"] == f"base:b217:{PIN}:2"
    assert "noise" in p.need["what"]
    # Two stable runs: a repeat pair, the noise is measured, nothing more to run.
    p = make_plan([cand, base, jitter(base, 0.01)])
    assert p.need is None and len(p.base) == 2
    # Never more than MAX_BASELINE_RUNS a pin, even if the noise stays unmeasured.
    few = ["t00", "t01"]  # too few tracks for the dof
    bases = [line(STABLE_SHA, 0.5, run=f"s{i}", release="b217", tracks=few) for i in range(3)]
    p = make_plan([line(CAND_SHA, 0.6, run="c", tracks=few), *bases])
    assert p.need is None


def test_a_metric_the_stable_doesnt_report_never_waits_for_repeats() -> None:
    cand = line(CAND_SHA, 0.6, run="c")
    base = line(STABLE_SHA, 0.5, run="s", release="b217")
    old = [base, jitter(base, 0.01)]
    for r in old:
        for t in r["tracks"]:
            t["tempoErr"] = None  # an older firmware: no tempo
    p = make_plan([cand, *old])
    assert p.need is None
    res = proposals.compare(p.cand, p.base, proposals.noise([cand, *old]))
    assert verdicts(res)["tempoErr"] == "not measured"


def test_a_run_that_keeps_ending_without_scores_is_given_up_on() -> None:
    p = make_plan([], failed={f"cand:{CAND_SHA}:{PIN}": 2})
    assert p.need is None and p.blocked and "given up" in p.blocked


# -- the issue and the runner ------------------------------------------------------------------


class GitHub:
    """gh as a fake for the proposals: releases, commits, statuses, and blinky_time issues."""

    def __init__(self, candidate: str = "b228", stable: str = "b217", hw: str = "success"):
        self.rels = [
            {"tag_name": candidate, "prerelease": True},
            {"tag_name": "b227", "prerelease": True},
            {"tag_name": stable, "prerelease": False},
            {"tag_name": "b216", "prerelease": False},
        ]
        self.hw = hw
        self.issues: dict[int, dict[str, Any]] = {}
        self.comments: list[tuple[int, str]] = []
        self.calls: list[str] = []

    def __call__(self, *args: str, stdin: str | None = None) -> str:
        a = " ".join(args)
        self.calls.append(a)
        if "releases?per_page" in a:
            return json.dumps(self.rels)
        if "search/issues" in a:
            return "".join(
                json.dumps({"title": i["title"], "number": n, "url": i["url"], "state": i["state"]})
                + "\n"
                for n, i in self.issues.items()
            )
        if a.endswith("/status"):
            return json.dumps(
                {"statuses": [{"context": "Hardware test (staging)", "state": self.hw}]}
            )
        if f"repos/{proposals.FW_REPO}/commits/" in a:
            ref = a.rsplit("/", 1)[1]
            if ref.startswith("b2"):  # a tag: the merge of CAND_SHA (or the stable's commit)
                if ref == "b217":
                    return json.dumps({"sha": STABLE_SHA, "commit": {"tree": {"sha": "ts"}}})
                parents = [{"sha": "p" * 40}, {"sha": CAND_SHA}]
                return json.dumps(
                    {"sha": TAG_SHA, "commit": {"tree": {"sha": "tc"}}, "parents": parents}
                )
            return json.dumps({"sha": ref, "commit": {"tree": {"sha": "tc"}}, "parents": []})
        if "/issues" in a and "-X POST" in a and "/comments" in a:
            n = int(a.split("/issues/")[1].split("/")[0])
            self.comments.append((n, json.loads(stdin or "{}")["body"]))
            return "{}"
        if "/issues" in a and "-X POST" in a:
            n = 600 + len(self.issues)
            body = json.loads(stdin or "{}")
            url = f"https://github.com/{proposals.TOOLING_REPO}/issues/{n}"
            self.issues[n] = {**body, "url": url, "state": "open"}
            return json.dumps({"number": n, "html_url": url})
        if "-X PATCH" in a:
            n = int(a.split("/issues/")[1].split()[0])
            self.issues[n].update(json.loads(stdin or "{}"))
            return "{}"
        raise AssertionError(f"unexpected gh call: {a}")


def leading() -> list[dict[str, Any]]:
    base = line(STABLE_SHA, 0.5, run="s", release="b217")
    up = {"drumF": 0.6, "kickR50": 0.5, "snareR50": 0.5, "onDrum": 0.5, "tempoErr": 0.5}
    return [line(CAND_SHA, up, run="c"), base, jitter(base, 0.01)]


def run(gh: GitHub, st: dict[str, Any], records: list[dict[str, Any]]) -> Any:
    out = proposals.evaluate(gh, st, records, PIN, {"frame_time"})
    proposals.apply(gh, st, out)
    return out


def test_a_leading_candidate_gets_one_issue_updated_in_place_never_a_second() -> None:
    gh, st = GitHub(), {}
    out = run(gh, st, leading())
    assert out.decision.qualifies
    assert [i["title"] for i in gh.issues.values()] == ["Propose b228 for stable"]
    body = gh.issues[600]["body"]
    assert "| drum F | 0.518 | 0.618 | +0.100 |" in body and "✅ better" in body
    assert "repS" in body and "release_promote.py" in body
    n_calls = len(gh.calls)
    run(gh, st, leading())  # nothing changed: no write
    assert not any("-X PATCH" in c or "-X POST" in c for c in gh.calls[n_calls:])
    # The state lost: the title search finds the issue; it is updated, not opened again.
    recs = leading()
    recs[0]["tracks"][3]["drumF"] = 0.9
    run(gh, {}, recs)
    assert len(gh.issues) == 1 and gh.issues[600]["body"] != body


def test_a_candidate_that_stops_qualifying_is_closed_and_reopened_if_it_qualifies_again() -> None:
    gh, st = GitHub(), {}
    run(gh, st, leading())
    gh.hw = "failure"
    out = run(gh, st, leading())
    assert not out.decision.qualifies and gh.issues[600]["state"] == "closed"
    assert "doesn't qualify now" in gh.issues[600]["body"]
    gh.hw = "success"
    run(gh, st, leading())
    assert gh.issues[600]["state"] == "open" and len(gh.issues) == 1


def test_a_candidate_that_never_qualified_gets_no_issue() -> None:
    gh = GitHub(hw="failure")
    run(gh, {}, leading())
    assert gh.issues == {}


def test_a_newer_candidates_proposal_closes_the_older_one_pointing_to_it() -> None:
    gh, st = GitHub(candidate="b227"), {}
    gh.rels = [r for r in gh.rels if r["tag_name"] != "b228"]
    run(gh, st, leading())
    gh2 = GitHub()
    gh2.issues, gh2.comments = gh.issues, gh.comments
    run(gh2, st, leading())
    assert gh.issues[600]["state"] == "closed" and gh.issues[601]["state"] == "open"
    assert gh.comments == [(600, f"Superseded by the proposal of b228: {gh.issues[601]['url']}")]


def test_a_proposal_whose_candidate_became_stable_is_closed() -> None:
    gh, st = GitHub(), {}
    run(gh, st, leading())
    promoted = GitHub(candidate="b229", stable="b228")
    promoted.issues, promoted.comments = gh.issues, gh.comments
    run(promoted, st, [])
    assert gh.issues[600]["state"] == "closed"
    assert gh.comments[-1] == (600, "b228 is the stable release now.")


def test_a_promoted_proposal_closes_even_before_a_newer_candidate_exists() -> None:
    # Review of #453: right after promotion there is no candidate newer than the stable.
    gh, st = GitHub(), {}
    run(gh, st, leading())
    promoted = GitHub(stable="b228")
    promoted.rels = [
        {"tag_name": "b228", "prerelease": False},
        {"tag_name": "b227", "prerelease": True},
    ]
    promoted.issues, promoted.comments = gh.issues, gh.comments
    out = run(promoted, st, leading())
    assert out.plan is None and out.stable == "b228"
    assert gh.issues[600]["state"] == "closed"
    assert gh.comments[-1] == (600, "b228 is the stable release now.")


def test_an_older_open_proposal_closes_when_a_newer_release_becomes_stable() -> None:
    gh, st = GitHub(candidate="b227"), {}
    gh.rels = [r for r in gh.rels if r["tag_name"] != "b228"]
    run(gh, st, leading())
    promoted = GitHub(stable="b228")
    promoted.rels = [{"tag_name": "b228", "prerelease": False}]
    promoted.issues, promoted.comments = gh.issues, gh.comments
    run(promoted, st, [])
    assert gh.issues[600]["state"] == "closed"
    assert gh.comments[-1] == (600, "b228, newer than b227, is the stable release now.")


def test_the_candidates_staging_commit_must_have_its_tree() -> None:
    gh = GitHub()
    real = gh.__call__

    def other_tree(*args: str, stdin: str | None = None) -> str:
        if args[-1].endswith(f"/commits/{CAND_SHA}"):
            return json.dumps(
                {"sha": CAND_SHA, "commit": {"tree": {"sha": "other"}}, "parents": []}
            )
        return real(*args, stdin=stdin)

    out = proposals.evaluate(other_tree, {}, leading(), PIN, set())
    assert out.plan is None and "tree isn't its staging parent" in out.message


class Server:
    def __init__(self, mode: str = "queue", waiting: list[dict[str, Any]] | None = None):
        self.mode, self.waiting = mode, waiting or []
        self.posts: list[dict[str, Any]] = []
        self.refuse: int | None = None

    def __call__(self, method: str, path: str, body: Any = None) -> Any:
        if path == "/api/box/mode":
            return {"mode": self.mode, "waiting": self.waiting, "preempted": []}
        if path == "/api/devices":
            return [
                {"id": "CHIP", "state": "connected", "platform": "nrf52840", "device_type": "g"}
            ]
        if path == "/api/test/strict-rules":
            return {"readiness": {"frame_time": {"mode": "block"}, "beat_pace": {"mode": "warn"}}}
        if method == "POST" and path == "/api/test/staging":
            if self.refuse:
                raise urllib.error.HTTPError(path, self.refuse, "no", {}, None)  # type: ignore[arg-type]
            self.posts.append(body)
            return {"job_id": f"job{len(self.posts)}"}
        raise AssertionError(f"unexpected {method} {path}")


@pytest.fixture
def box(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.setattr(staging, "STATE", tmp_path / "staging.json")
    monkeypatch.setattr(staging, "LINK", tmp_path / "deploy")
    monkeypatch.setattr(proposals, "corpus_pin", lambda p: PIN)
    return tmp_path


def write_records(d: Path, recs: list[dict[str, Any]]) -> None:
    (d / "staging-scores.jsonl").write_text("".join(json.dumps(r) + "\n" for r in recs))


def test_the_runner_submits_the_baseline_only_when_the_box_is_free(box: Path) -> None:
    write_records(box, [line(CAND_SHA, 0.6, run="c")])
    gh, state = GitHub(), {}
    busy = Server(waiting=[{"runner": "pr", "sha": "q" * 40, "reported_at": 9e12}])
    staging.propose(gh, busy, state)
    assert busy.posts == [] and "running" not in state
    dev = Server(mode="dev")
    staging.propose(gh, dev, state)
    assert dev.posts == []
    free = Server()
    staging.propose(gh, free, state)
    assert free.posts == [
        {
            "sha": STABLE_SHA,
            "release": "b217",
            "track_dir": str(box / "deploy" / staging.CORPUS),
            "device_ids": ["CHIP"],
        }
    ]
    assert state["running"]["kind"] == "proposal"


def test_a_proposal_run_that_ends_without_scores_counts_towards_giving_up(box: Path) -> None:
    state = {"running": {"sha": STABLE_SHA, "job": "j", "kind": "proposal", "key": "base:k"}}
    staging.finish_proposal_run(state, {"status": "error", "error": "artifact gone"})
    assert state["proposal"]["failed"] == {"base:k": 1} and "running" not in state
    state["running"] = {"sha": STABLE_SHA, "job": "j", "kind": "proposal", "key": "base:k"}
    staging.finish_proposal_run(
        state, {"status": "complete", "result": {"validation": {"results": [{}]}}}
    )
    assert state["proposal"]["failed"] == {"base:k": 1}


def test_a_refused_proposal_run_counts_too_and_a_busy_server_doesnt(box: Path) -> None:
    write_records(box, [line(CAND_SHA, 0.6, run="c")])
    state: dict[str, Any] = {}
    s = Server()
    s.refuse = 409
    staging.propose(GitHub(), s, state)
    assert (state["proposal"].get("failed") or {}) == {} and "running" not in state
    s.refuse = 400
    staging.propose(GitHub(), s, state)
    assert state["proposal"]["failed"] == {f"base:b217:{PIN}:1": 1}
