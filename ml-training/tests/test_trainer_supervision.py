"""train.py under the Spark's gpu_task contract, end to end (#304).

Runs the real trainer on a tiny synthetic dataset with the environment the platform sets
(services/job-service/spark_jobs/workers/gpu_task_worker.py), SIGTERMs it once a checkpoint
exists, and checks what the platform checks: exit 42, then a resume that finishes, STEP lines
throughout, results only inside the declared output, and a refused resume when the run changed.
Needs torch, so it runs in its own CI step.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import pytest

torch = pytest.importorskip("torch")

ML = Path(__file__).resolve().parents[1]
EPOCHS = 40
sys.path.insert(0, str(ML))

from scripts.mel_frontend import RECORD_FILE, frontend_record, write_record  # noqa: E402


@pytest.fixture
def job(tmp_path: Path) -> dict:
    cfg_dir = tmp_path / "configs"
    cfg_dir.mkdir()
    shutil.copy(ML / "configs" / "base.yaml", cfg_dir / "base.yaml")
    (cfg_dir / "tiny.yaml").write_text(
        "model:\n"
        "  type: frame_conv1d\n"
        "  channels: [4]\n"
        "  kernel_sizes: [3]\n"
        "  window_frames: 16\n"
        "  dropout: 0.0\n"
        "  num_output_channels: 0\n"
        "loss:\n  type: bce\n"
        "training:\n"
        "  batch_size: 64\n  epochs: 2\n  subsample: 1.0\n  num_workers: 0\n"
        "  patience: 1000\n  pos_weight: 0\n"
        "augmentation:\n  spec_augment:\n    enabled: false\n"
    )
    data = tmp_path / "inputs" / "data"
    data.mkdir(parents=True)
    rng = np.random.default_rng(0)
    for split, n in (("train", 256), ("val", 64)):
        x = rng.random((n, 128, 50), dtype=np.float32)
        y = (rng.random((n, 128)) < 0.05).astype(np.float32)
        np.save(data / f"X_{split}.npy", x)
        np.save(data / f"Y_{split}.npy", y)
    write_record(data, frontend_record("firmware_table"))  # as prepare_dataset.py does (#291)
    work = tmp_path / "work"
    env = {
        **os.environ,
        "PYTHONUNBUFFERED": "1",
        "SPARK_JOB_ID": "test-job",
        "SPARK_INPUT_DATA": str(data),
        "SPARK_OUTPUT_MODEL": str(work / "outputs" / "model"),
        "SPARK_CHECKPOINT_DIR": str(work / "checkpoints"),
    }
    env.pop("TMUX", None)
    env.pop("STY", None)
    return {"env": env, "config": str(cfg_dir / "tiny.yaml"), "work": work}


def cmd(job: dict, *extra: str) -> list[str]:
    return [sys.executable, "train.py", "--config", job["config"], "--epochs", str(EPOCHS),
            "--device", "cpu", *extra]


def steps(out: str) -> list[tuple[int, int]]:
    return [(int(a), int(b)) for a, b in re.findall(r"^STEP (\d+)/(\d+)$", out, re.M)]


def test_preempt_resume_and_refuse(job: dict) -> None:
    ckpt = job["work"] / "checkpoints" / "training_checkpoint.pt"
    model_dir = job["work"] / "outputs" / "model"

    # 1. SIGTERM once a checkpoint exists: a clean exit 42.
    proc = subprocess.Popen(cmd(job), cwd=ML, env=job["env"], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    deadline = time.monotonic() + 300
    while not ckpt.exists():
        assert proc.poll() is None, proc.communicate()[0][-3000:]
        assert time.monotonic() < deadline, "no checkpoint within 300 s"
        time.sleep(0.05)
    proc.send_signal(signal.SIGTERM)
    out1, _ = proc.communicate(timeout=120)
    assert proc.returncode == 42, out1[-3000:]
    assert "PREEMPTED (SIGTERM)" in out1
    assert steps(out1), "no STEP line before the preemption"
    done_before = torch.load(ckpt, weights_only=False)["epoch"] + 1
    assert done_before < EPOCHS

    # 2. The requeued attempt resumes from the checkpoint and finishes.
    r = subprocess.run(cmd(job), cwd=ML, env=job["env"], capture_output=True, text=True,
                       timeout=600)
    assert r.returncode == 0, (r.stdout + r.stderr)[-3000:]
    assert f"RESUMED from epoch {done_before}/{EPOCHS}" in r.stdout
    s = steps(r.stdout)
    total = s[0][1]
    assert s[0][0] == done_before * total // EPOCHS  # picks up where it left off
    assert s[-1] == (total, total)
    assert [a for a, _ in s] == sorted(a for a, _ in s)
    # Results only inside the declared output; the checkpoint in its own preserved dir.
    for name in ("best_model.pt", "final_model.pt", "model_checkpoint.pt", "training_log.csv"):
        assert (model_dir / name).is_file(), name
    # The dataset's mel front end travels with the model, for export_tflite.py (#291).
    assert json.loads((model_dir / RECORD_FILE).read_text()) == frontend_record("firmware_table")
    assert torch.load(model_dir / "model_checkpoint.pt", weights_only=False)["mel_frontend"] \
        == frontend_record("firmware_table")
    assert not (model_dir / "training_checkpoint.pt").exists()
    assert len((model_dir / "training_log.csv").read_text().splitlines()) == EPOCHS + 1
    # The checkpoint's best weights are the saved best model's, not the latest weights.
    final = torch.load(ckpt, weights_only=False)
    best_file = torch.load(model_dir / "best_model.pt", weights_only=True)
    assert final["best_state"].keys() == best_file.keys()
    assert all(torch.equal(final["best_state"][k], best_file[k]) for k in best_file)
    # A copy, not the live parameters: torch.save keeps shared storage shared, so a
    # best_state that aliased the model would come back sharing model_state's storage.
    for k in best_file:
        ptr = final["best_state"][k].untyped_storage().data_ptr()
        assert ptr != final["model_state"][k].untyped_storage().data_ptr(), k

    # 3. A finished run's checkpoint resumes as finished: no epoch is retrained.
    r = subprocess.run(cmd(job), cwd=ML, env=job["env"], capture_output=True, text=True,
                       timeout=600)
    assert r.returncode == 0, (r.stdout + r.stderr)[-3000:]
    assert "training loop already finished" in r.stdout
    assert not re.search(r"^Epoch \d+/", r.stdout, re.M)

    # 4. A changed run refuses the checkpoint before loading any of it.
    r = subprocess.run(cmd(job, "--lr", "0.01"), cwd=ML, env=job["env"], capture_output=True,
                       text=True, timeout=600)
    assert r.returncode == 1
    assert "belongs to a different run" in r.stderr


def test_missing_declarations_fail_loud(job: dict) -> None:
    env = {k: v for k, v in job["env"].items() if k != "SPARK_INPUT_DATA"}
    r = subprocess.run(cmd(job), cwd=ML, env=env, capture_output=True, text=True, timeout=300)
    assert r.returncode == 1 and "SPARK_INPUT_DATA is unset" in r.stderr
    env = {k: v for k, v in job["env"].items() if k != "SPARK_OUTPUT_MODEL"}
    r = subprocess.run(cmd(job), cwd=ML, env=env, capture_output=True, text=True, timeout=300)
    assert r.returncode == 1 and "SPARK_OUTPUT_MODEL is unset" in r.stderr


def test_unrecorded_or_mismatched_front_end_is_refused(job: dict) -> None:
    """Features with no front-end record, or another front end than the config's, never train:
    the model would carry a front end nothing vouches for into the firmware's header (#291)."""
    data = Path(job["env"]["SPARK_INPUT_DATA"])
    (data / RECORD_FILE).unlink()
    r = subprocess.run(cmd(job), cwd=ML, env=job["env"], capture_output=True, text=True,
                       timeout=300)
    assert r.returncode == 1 and "mel_frontend.json is missing" in r.stderr
    write_record(data, frontend_record("librosa_htk"))
    r = subprocess.run(cmd(job), cwd=ML, env=job["env"], capture_output=True, text=True,
                       timeout=300)
    assert r.returncode == 1 and "holds librosa_htk features" in r.stderr

    # An output dir already holding another front end's model is not relabeled.
    write_record(data, frontend_record("firmware_table"))
    model_dir = Path(job["env"]["SPARK_OUTPUT_MODEL"])
    model_dir.mkdir(parents=True)
    write_record(model_dir, frontend_record("librosa_htk"))
    r = subprocess.run(cmd(job), cwd=ML, env=job["env"], capture_output=True, text=True,
                       timeout=300)
    assert r.returncode == 1 and "another mel front end" in r.stderr
    # ...nor one holding models from before records existed.
    (model_dir / RECORD_FILE).unlink()
    (model_dir / "best_model.pt").write_bytes(b"old")
    r = subprocess.run(cmd(job), cwd=ML, env=job["env"], capture_output=True, text=True,
                       timeout=300)
    assert r.returncode == 1 and "from before #291" in r.stderr
