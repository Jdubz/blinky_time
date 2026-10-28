"""spark_entry.py end to end (#305): real prep, then real training, under the gpu_task env.

Builds an eight-track synthetic corpus laid out like the store's `datasets/blinky-corpus/` (flat
`audio/`, labels, `rir/processed`, `noise/<category>`), copies the bundle read-only as the
platform mounts /code, and checks: SIGTERM during prep exits 42, the requeued attempt preps and
trains, a third attempt skips both, and a bad corpus path fails before any work. Needs torch and
librosa, so it runs in the trainer-contract CI job.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import signal
import stat
import subprocess
import sys
from collections.abc import Iterator
from pathlib import Path

import numpy as np
import pytest

pytest.importorskip("torch")
pytest.importorskip("librosa")
sf = pytest.importorskip("soundfile")

ML = Path(__file__).resolve().parents[1]
SR = 22050


def write_corpus(root: Path) -> None:
    rng = np.random.default_rng(0)
    (root / "audio").mkdir(parents=True)
    labels = root / "labels" / "onsets_consensus"
    labels.mkdir(parents=True)
    for i in range(8):
        x = 0.01 * rng.standard_normal(SR * 8)
        times = np.arange(0.25, 7.8, 0.5) + 0.01 * i
        for t in times:
            k = int(t * SR)
            x[k : k + 400] += np.hanning(400) * 0.8 * rng.standard_normal(400)
        sf.write(root / "audio" / f"{i:06d}.wav", x.astype(np.float32), SR)
        onsets = [{"time": float(t), "systems": 5, "strength": 1.0} for t in times]
        (labels / f"{i:06d}.onsets.json").write_text(json.dumps({"onsets": onsets}))
    (root / "rir" / "processed").mkdir(parents=True)
    rir = rng.standard_normal(1600) * np.exp(-np.arange(1600) / 200)
    sf.write(root / "rir" / "processed" / "room.wav", rir.astype(np.float32), 16000)
    (root / "noise" / "hum").mkdir(parents=True)
    sf.write(root / "noise" / "hum" / "hum.wav",
             (0.1 * np.sin(np.arange(SR * 3) * 0.05)).astype(np.float32), SR)


@pytest.fixture
def job(tmp_path: Path) -> Iterator[dict]:
    code = tmp_path / "code"
    tracked = subprocess.run(["git", "ls-files"], cwd=ML, capture_output=True, text=True,
                             check=True).stdout.split()
    for rel in tracked + ["spark_entry.py"]:  # the entry may be uncommitted while developing
        if (ML / rel).is_file():
            (code / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ML / rel, code / rel)
    (code / "configs" / "tiny.yaml").write_text(
        "model:\n  type: frame_conv1d\n  channels: [4]\n  kernel_sizes: [3]\n"
        "  window_frames: 16\n  dropout: 0.0\n  num_output_channels: 0\n"
        "loss:\n  type: bce\n"
        "labels:\n  labels_type: onset_consensus\n"
        '  onset_consensus_dir: "${BLINKY_DATA}/labels/onsets_consensus"\n'
        "training:\n  batch_size: 64\n  epochs: 3\n  subsample: 1.0\n  num_workers: 0\n"
        "  pos_weight: 0\n"
        "augmentation:\n  spec_augment:\n    enabled: false\n"
    )
    for p in [code, *code.rglob("*")]:  # /code is mounted read-only
        p.chmod(p.stat().st_mode & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))
    corpus = tmp_path / "inputs" / "corpus"
    write_corpus(corpus)
    work = tmp_path / "work"
    env = {
        **os.environ,
        "PYTHONUNBUFFERED": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
        "SPARK_JOB_ID": "test-job",
        "SPARK_INPUT_CORPUS": str(corpus),
        "SPARK_OUTPUT_MODEL": str(work / "outputs" / "model"),
        "SPARK_CHECKPOINT_DIR": str(work / "checkpoints"),
        "HOME": str(work),
    }
    env.pop("TMUX", None)
    env.pop("STY", None)
    yield {"code": code, "env": env, "work": work}
    for p in [code, *code.rglob("*")]:
        p.chmod(p.stat().st_mode | stat.S_IWUSR)


def cmd(job: dict, *extra: str) -> list[str]:
    return [sys.executable, str(job["code"] / "spark_entry.py"), "--config", "configs/tiny.yaml",
            "--audio-dir", "audio", "--prep-arg=--seed", "--prep-arg=7", *extra, "--",
            "--device", "cpu"]


def test_preempt_in_prep_then_prep_and_train(job: dict) -> None:
    model = job["work"] / "outputs" / "model"
    processed = job["work"] / "checkpoints" / "processed"

    # 1. SIGTERM while prep runs: exit 42, nothing published.
    proc = subprocess.Popen(cmd(job), cwd=job["work"].parent, env=job["env"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert proc.stdout is not None
    seen = []
    for line in proc.stdout:
        seen.append(line)
        if line.startswith("STEP 0/1"):
            proc.send_signal(signal.SIGTERM)
            break
    out = "".join(seen) + proc.communicate(timeout=120)[0]
    assert proc.returncode == 42, out[-3000:]
    assert "PREEMPTED (SIGTERM) during prep" in out
    assert not (processed / ".prep_done.json").exists()

    # 2. The requeued attempt preps with augmentation and trains.
    r = subprocess.run(cmd(job), cwd=job["work"].parent, env=job["env"], capture_output=True,
                       text=True, timeout=900)
    assert r.returncode == 0, (r.stdout + r.stderr)[-4000:]
    assert "--augment" in r.stdout and "--rir-dir" in r.stdout and "--noise-dir" in r.stdout
    assert "--seed 7" in r.stdout
    for name in ("X_train.npy", "Y_train.npy", "X_val.npy", "Y_val.npy", ".prep_done.json"):
        assert (processed / name).is_file(), name
    for name in ("best_model.pt", "final_model.pt", "training_log.csv", "spark_run.json"):
        assert (model / name).is_file(), name
    assert not any(p.suffix == ".npy" for p in model.rglob("*"))  # tensors are never published
    run = json.loads((model / "spark_run.json").read_text())
    assert run["config"] == "configs/tiny.yaml" and run["train_args"] == ["--device", "cpu"]
    steps = re.findall(r"^STEP (\d+)/(\d+)$", r.stdout, re.M)
    assert steps and steps[-1][0] == steps[-1][1]  # training reported its end

    # 3. Another attempt: prep is done and the training loop is finished.
    r = subprocess.run(cmd(job), cwd=job["work"].parent, env=job["env"], capture_output=True,
                       text=True, timeout=600)
    assert r.returncode == 0, (r.stdout + r.stderr)[-3000:]
    assert "PREP already done" in r.stdout
    assert "training loop already finished" in r.stdout


def test_bad_corpus_paths_fail_before_any_work(job: dict) -> None:
    r = subprocess.run(cmd(job)[:4] + ["--audio-dir", "nope"],
                       cwd=job["work"].parent, env=job["env"], capture_output=True, text=True,
                       timeout=120)
    assert r.returncode == 1 and "not in the corpus input" in r.stderr
    shutil.rmtree(Path(job["env"]["SPARK_INPUT_CORPUS"]) / "noise")
    r = subprocess.run(cmd(job), cwd=job["work"].parent, env=job["env"], capture_output=True,
                       text=True, timeout=120)
    assert r.returncode == 1 and "'noise'" in r.stderr  # augmentation can't silently shrink
    env = {k: v for k, v in job["env"].items() if k != "SPARK_INPUT_CORPUS"}
    r = subprocess.run(cmd(job), cwd=job["work"].parent, env=env, capture_output=True,
                       text=True, timeout=120)
    assert r.returncode == 1 and "SPARK_INPUT_CORPUS is unset" in r.stderr
    assert not (job["work"] / "checkpoints" / "processed").exists()
