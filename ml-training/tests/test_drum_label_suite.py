"""The drum label regression suite and the generator's pipeline guards (#284).

The suite is what a new kick/snare generator must pass before it replaces the current one, so
its metrics are checked here on cases with known answers, and the committed baseline is checked
to still be what the current generator and edm/'s labels produce. Needs librosa (the generator
uses it), so it runs in the trainer-contract CI job.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

librosa = pytest.importorskip("librosa")
sf = pytest.importorskip("soundfile")

ML = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ML))

from scripts import drum_label_suite as S  # noqa: E402
from scripts import generate_kick_weighted_targets as G  # noqa: E402

TRUTH = [(1.0, "kick"), (1.5, "snare"), (2.0, "kick"), (2.5, "snare"), (3.0, "kick")]


# -- the metrics -----------------------------------------------------------------------------


def test_perfect_labels_score_perfectly() -> None:
    s = S.score_synthetic(TRUTH, TRUTH)
    for kind in S.TYPES:
        assert s[kind]["f1"] == 1.0 and s[kind]["timing"]["median_ms"] == 0.0
        assert s[kind]["timing"]["within_10ms"] == 1.0 and s[kind]["duplicates"] == 0
    assert s["snare_as_kick"] == s["kick_as_snare"] == s["phantom_snares"] == 0


def test_late_labels_still_match_but_timing_says_late() -> None:
    s = S.score_synthetic(TRUTH, [(t + 0.024, k) for t, k in TRUTH])
    assert s["kick"]["f1"] == 1.0  # inside the 50 ms match tolerance
    assert s["kick"]["timing"]["median_ms"] == 24.0 and s["kick"]["timing"]["within_10ms"] == 0.0


def test_confusion_phantoms_and_duplicates_are_counted() -> None:
    est = [
        (1.0, "kick"), (1.03, "kick"),  # a duplicate kick
        (1.5, "kick"),  # the snare at 1.5 labeled as a kick
        (1.96, "snare"), (2.0, "kick"),  # a phantom snare 40 ms before the kick at 2.0
        (2.5, "snare"), (3.0, "snare"),  # the kick at 3.0 labeled as a snare
    ]
    s = S.score_synthetic(TRUTH, est)
    assert s["kick"]["duplicates"] == 1
    assert s["snare_as_kick"] == 1 and s["kick_as_snare"] == 1
    assert s["phantom_snares"] == 1
    assert s["snare"]["recall"] == 0.5 and s["kick"]["recall"] == pytest.approx(2 / 3, abs=1e-3)


def test_the_gate_knows_which_way_is_better() -> None:
    base = json.loads(S.BASELINE.read_text())
    assert S.compare(base, base) == []
    worse = json.loads(json.dumps(base))
    worse["synthetic"]["kick"]["timing"]["median_ms"] = -30.0  # earlier, but further from 0
    worse["edm"]["clean_retention"] = 0.5
    worse["synthetic"]["snare_as_kick"] -= 1  # better: fewer
    got = S.compare(base, worse)
    assert len(got) == 2 and any("median_ms" in g for g in got) and any("retention" in g for g in got)


# -- the synthetic corpus ----------------------------------------------------------------------


def test_synthetic_corpus_is_deterministic_and_its_truth_is_where_the_sound_starts() -> None:
    a, b = S.synth_corpus(0), S.synth_corpus(0)
    assert all(np.array_equal(x.audio, y.audio) and x.truth == y.truth for x, y in zip(a, b, strict=True))
    tr = a[0]
    kinds = {k for _, k in tr.truth}
    assert kinds == {"kick", "snare", "hihat"}
    floor = np.abs(tr.audio[: int(0.4 * tr.sr)]).max()  # before the first hit at 0.5 s
    first = int(round(tr.truth[0][0] * tr.sr))
    assert np.abs(tr.audio[first : first + int(0.005 * tr.sr)]).max() > 10 * floor
    co_hits = [t for t, k in tr.truth if k == "kick" and (t, "snare") in tr.truth]
    assert co_hits, "patterns must include kick+snare co-hits"


# -- the baseline ------------------------------------------------------------------------------


def test_edm_baseline_reproduces() -> None:
    base = json.loads(S.BASELINE.read_text())
    assert S.run_edm() == base["edm"]  # pure arithmetic on committed files: exact
    # It is the audit's measurement of the committed labels (#284): about 26 ms late.
    assert 20 <= base["edm"]["kick"]["timing_vs_consensus"]["median_ms"] <= 30


def test_synthetic_baseline_reproduces_with_the_same_librosa() -> None:
    base = json.loads(S.BASELINE.read_text())
    got = S.run_synthetic(S.current_generator)
    if librosa.__version__ == base["librosa"]:
        assert got == base["synthetic"]
    # Whatever the version, the suite must see what the audit found in this generator.
    assert got["kick"]["timing"]["median_ms"] > 10  # late
    assert got["snare_as_kick"] > got["snare"]["ref"] / 2  # most snares labeled kicks
    assert got["snare"]["f1"] < 0.5


# -- the generator's pipeline guards -----------------------------------------------------------


def run_gen(*args: str, algorithm: str = "bandpass") -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, "scripts/generate_kick_weighted_targets.py", *args,
                           "--algorithm", algorithm, "--workers", "1"], cwd=ML,
                          capture_output=True, text=True, timeout=600)


@pytest.fixture
def stems(tmp_path: Path) -> Path:
    d = tmp_path / "drums"
    d.mkdir()
    tr = S.synth_corpus(0)[0]
    for name in ("000001", "000002"):
        sf.write(d / f"{name}.wav", tr.audio[: tr.sr * 4], tr.sr)
    sf.write(d / "000003.wav", np.zeros(16000, dtype=np.float32), 16000)  # a silent stem
    return d


def test_generator_writes_versioned_labels_and_resumes_only_its_own(
    stems: Path, tmp_path: Path
) -> None:
    out = tmp_path / "kw"
    r = run_gen("--audio-dir", str(stems), "--output-dir", str(out))
    assert r.returncode == 0, r.stderr
    lab = json.loads((out / "000001.kick_weighted.json").read_text())
    assert lab["generator"] == G.GENERATOR and lab["kick_count"] > 0
    assert json.loads((out / "000003.kick_weighted.json").read_text())["skipped"] is True
    assert json.loads((out / ".provenance.json").read_text())["generator"] == G.GENERATOR

    r = run_gen("--audio-dir", str(stems), "--output-dir", str(out))
    assert r.returncode == 0 and "0 tracks (3 already done" in r.stdout.replace("Processing ", "")

    # A file from another generator in the same dir is redone, not trusted.
    stale = out / "000002.kick_weighted.json"
    stale.write_text(json.dumps({**lab, "generator": {"algorithm": "old", "version": 0}}))
    r = run_gen("--audio-dir", str(stems), "--output-dir", str(out))
    assert "1 tracks (2 already done" in r.stdout.replace("Processing ", "")
    assert json.loads(stale.read_text())["generator"] == G.GENERATOR


def test_generator_refuses_a_dir_from_another_generator_or_source(
    stems: Path, tmp_path: Path
) -> None:
    out = tmp_path / "kw"
    assert run_gen("--audio-dir", str(stems), "--output-dir", str(out)).returncode == 0
    prov = out / ".provenance.json"
    p = json.loads(prov.read_text())
    prov.write_text(json.dumps({**p, "generator": {"algorithm": "bandpass_onset_detect",
                                                     "version": 0}}))
    r = run_gen("--audio-dir", str(stems), "--output-dir", str(out))
    assert r.returncode == 1 and "holds other labels" in r.stderr and "v0" in r.stderr

    legacy = tmp_path / "legacy"  # labels with no provenance at all: unknown origin
    legacy.mkdir()
    (legacy / "000001.kick_weighted.json").write_text("{}")
    r = run_gen("--audio-dir", str(stems), "--output-dir", str(legacy))
    assert r.returncode == 1 and "no .provenance.json" in r.stderr


def test_generator_fails_loudly_when_there_is_nothing_to_label(
    stems: Path, tmp_path: Path
) -> None:
    labels = tmp_path / "onsets_consensus"
    labels.mkdir()
    (labels / "000002.onsets.json").write_text("{}")
    r = run_gen("--audio-dir", str(stems), "--output-dir", str(tmp_path / "a"),
                "--tracks-from", str(labels))
    assert r.returncode == 0 and "Matched 1 audio files" in r.stdout
    labeled = sorted(p.name for p in (tmp_path / "a").glob("*.kick_weighted.json"))
    assert labeled == ["000002.kick_weighted.json"]
    empty = tmp_path / "empty"
    empty.mkdir()
    r = run_gen("--audio-dir", str(stems), "--output-dir", str(tmp_path / "b"),
                "--tracks-from", str(empty))
    assert r.returncode == 1 and "no tracks to label" in r.stderr  # was "Nothing to do!", exit 0


# -- the validator's consensus gate ------------------------------------------------------------


def validate(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, "scripts/validate_kick_weighted.py", *args], cwd=ML,
                          capture_output=True, text=True, timeout=120)


def test_validator_runs_the_consensus_gate_on_the_onset_consensus() -> None:
    r = validate("--label-dir", str(S.EDM / "kick_weighted"), "--consensus-dir", str(S.EDM))
    assert r.returncode == 0, r.stdout + r.stderr
    assert "18 OK, 0 flagged, 0 corrupt, 0 without consensus" in r.stdout


def test_a_wrong_consensus_dir_fails_and_quarantines_nothing(tmp_path: Path) -> None:
    labels = tmp_path / "kw"
    labels.mkdir()
    for f in sorted((S.EDM / "kick_weighted").glob("*.kick_weighted.json"))[:3]:
        (labels / f.name).write_text(f.read_text())
    r = validate("--label-dir", str(labels), "--consensus-dir", str(tmp_path / "nope"),
                 "--quarantine")
    assert r.returncode == 1  # was "All labels pass validation", exit 0
    assert "3 without consensus" in r.stdout and "gate did not run" in r.stderr
    assert "Nothing quarantined" in r.stderr
    assert len(list(labels.glob("*.kick_weighted.json"))) == 3
    assert not (labels / ".quarantined").exists()


# -- ADTOF, the replacement --------------------------------------------------------------------
#
# These need the port (torch), which only CI's adtof-labeler job installs. There,
# BLINKY_REQUIRE_ADTOF=1 turns a missing package into a failure instead of a skip, so the job
# can't pass by testing nothing.


def need_adtof() -> None:
    if os.environ.get("BLINKY_REQUIRE_ADTOF") == "1":
        import adtof_pytorch  # noqa: F401  (ImportError fails the test)
    else:
        pytest.importorskip("adtof_pytorch")


def test_adtof_beats_the_baseline_on_every_synthetic_row() -> None:
    need_adtof()
    base = json.loads(S.BASELINE.read_text())
    got = S.run_synthetic(S.adtof)
    synthetic_rows = [r for r in S.compare({"synthetic": base["synthetic"], "edm": base["edm"]},
                                           {"synthetic": got, "edm": base["edm"]})]
    assert synthetic_rows == [], synthetic_rows
    assert got["snare_as_kick"] == 0 and got["snare"]["f1"] > 0.9 and got["kick"]["f1"] > 0.9
    # The committed candidate evidence says the same, edm/ rows included.
    assert json.loads(S.candidate_path("adtof").read_text())["worse_than_baseline"] == []


def test_adtof_generator_writes_attributed_labels_with_co_hits(tmp_path: Path) -> None:
    need_adtof()
    from scripts import adtof_labels as A

    tr = S.synth_corpus(0)[0]
    audio = tmp_path / "mix"
    audio.mkdir()
    sf.write(audio / "000001.wav", tr.audio, tr.sr)
    out = tmp_path / "kw"
    r = run_gen("--audio-dir", str(audio), "--output-dir", str(out), algorithm="adtof")
    assert r.returncode == 0, r.stderr
    lab = json.loads((out / "000001.kick_weighted.json").read_text())
    assert lab["generator"] == A.GENERATOR and "CC BY-NC-SA" in lab["generator"]["attribution"]
    assert lab["generator"]["params"]["weights_sha256"] == A.WEIGHTS_SHA256
    kicks = {round(o["time"], 2) for o in lab["onsets"] if o["type"] == "kick"}
    snares = {round(o["time"], 2) for o in lab["onsets"] if o["type"] == "snare"}
    assert kicks & snares, "a kick and a snare at one instant are both kept"
    # The band generator's dir and ADTOF's never mix.
    r = run_gen("--audio-dir", str(audio), "--output-dir", str(out), algorithm="bandpass")
    assert r.returncode == 1 and "adtof_frame_rnn v1 != bandpass_onset_detect v1" in r.stderr
