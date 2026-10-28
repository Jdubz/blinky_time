#!/usr/bin/env python3
"""The Spark `gpu_task` entry point (#305): prep the dataset from the corpus, then train.

blinky-server bundles this tree and submits it (blinky_server/spark/, `spark_jobs.py train`). The
platform runs `python3 spark_entry.py <args>` in its runtime image with:

  SPARK_INPUT_CORPUS    the raw corpus (`datasets/blinky-corpus/`), read-only
  SPARK_OUTPUT_MODEL    where train.py writes the model; published on success
  SPARK_CHECKPOINT_DIR  preserved across a requeue; holds the prepared tensors and train.py's checkpoint

The prepared tensors are far too big for the store (about 200 GB at v36's size), so they are made
here, once per job, into the checkpoint dir. A requeued attempt finds them (or prep's own resume
manifest) and carries on. Prep is plain `scripts/prepare_dataset.py` and training is plain
`train.py`, run as children; this file only wires paths, progress and SIGTERM between them.

usage: spark_entry.py --config configs/X.yaml [--audio-dir audio] [--labels-dir DIR]
                      [--no-augment] [--prep-arg=ARG]... [-- TRAIN_ARG...]
Corpus directories are relative to the corpus input. Left out, the config's `${BLINKY_DATA}` paths
apply, with BLINKY_DATA set to the corpus.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import select
import signal
import subprocess
import sys
import time
from pathlib import Path

from scripts.supervision import PREEMPTED_EXIT, Preemption, Progress, fingerprint

HERE = Path(__file__).resolve().parent

# Not in the platform's runtime image yet: my-spark #1550. Checked first, so a job on an image
# without them fails in seconds instead of after staging and prep.
REQUIRED_MODULES = ("yaml", "librosa", "scipy", "tqdm")

PREP_DONE = ".prep_done.json"

# Prep reports through tqdm, which only writes when a bar moves. One line a minute keeps the log
# readable; any output from prep counts as progress for the platform's watchdog (1800 s).
TQDM_MININTERVAL_S = "60"


def fail(msg: str) -> int:
    print(f"ERROR: {msg}", file=sys.stderr, flush=True)
    return 1


def parse(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    train_args: list[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, train_args = argv[:i], argv[i + 1 :]
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--config", required=True, help="config path inside the bundle")
    ap.add_argument("--audio-dir", default=None, help="flat audio dir, relative to the corpus")
    ap.add_argument("--labels-dir", default=None, help="labels dir, relative to the corpus")
    ap.add_argument("--no-augment", action="store_true", help="prep without acoustic augmentation")
    ap.add_argument("--prep-arg", action="append", default=[],
                    help="passed to prepare_dataset.py verbatim (repeatable; --prep-arg=--seed)")
    return ap.parse_args(argv), train_args


def describe(corpus: Path) -> None:
    """Print the corpus's top two levels with file counts: the first thing to read when a path
    in the config doesn't match what was staged."""
    print(f"corpus {corpus}:", flush=True)
    for d in sorted(p for p in corpus.iterdir() if p.is_dir()):
        subs = sorted(p for p in d.iterdir() if p.is_dir())
        files = sum(1 for p in d.iterdir() if p.is_file())
        print(f"  {d.name}/ {files} files" + (f", dirs: {', '.join(s.name for s in subs)}"
                                              if subs else ""), flush=True)


def corpus_path(corpus: Path, rel: str | None, configured: str | None) -> Path | None:
    if rel is not None:
        return corpus / rel
    return Path(configured) if configured else None


def supervise_prep(cmd: list[str], env: dict[str, str], preempt: Preemption) -> int:
    """Run prep, copy its output through, and turn that output into STEP lines. Prep has no
    SIGTERM handling of its own: on one, stop it and exit 42. Its resume manifest is in the
    checkpoint dir, so the requeued attempt carries on from the last completed file."""
    progress = Progress(1)
    progress.report(0, force=True)
    proc = subprocess.Popen(cmd, cwd=HERE, env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    assert proc.stdout is not None
    fd = proc.stdout.fileno()
    while True:
        if preempt.requested:
            proc.terminate()
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            preempt.exit_if_requested("during prep")
        ready, _, _ = select.select([fd], [], [], 1.0)
        if not ready:
            continue
        chunk = os.read(fd, 65536)
        if not chunk:
            break
        sys.stdout.buffer.write(chunk)
        sys.stdout.flush()
        progress.report(0)
    return proc.wait()


def run_train(cmd: list[str], env: dict[str, str]) -> int:
    """train.py speaks the contract itself (STEP lines, exit 42). Forward SIGTERM to it and pass
    its exit code on."""
    proc = subprocess.Popen(cmd, cwd=HERE, env=env)
    signal.signal(signal.SIGTERM, lambda s, f: proc.send_signal(signal.SIGTERM))
    return proc.wait()


def main(argv: list[str] | None = None) -> int:
    args, train_args = parse(sys.argv[1:] if argv is None else argv)
    preempt = Preemption().install()

    env_paths = {}
    for var in ("SPARK_INPUT_CORPUS", "SPARK_OUTPUT_MODEL", "SPARK_CHECKPOINT_DIR"):
        if not os.environ.get(var):
            return fail(f"{var} is unset: submit with the `corpus` input and the `model` output")
        env_paths[var] = Path(os.environ[var])
    corpus = env_paths["SPARK_INPUT_CORPUS"]
    model_dir = env_paths["SPARK_OUTPUT_MODEL"]
    ckpt = env_paths["SPARK_CHECKPOINT_DIR"]

    missing = [m for m in REQUIRED_MODULES if importlib.util.find_spec(m) is None]
    if missing:
        return fail(f"the runtime image lacks {', '.join(missing)}, which prep and train.py "
                    f"import (requested in my-spark #1550)")

    config = HERE / args.config
    if not config.is_file():
        return fail(f"config {args.config} is not in the bundle")
    describe(corpus)

    env = dict(os.environ)
    env.update({
        "BLINKY_DATA": str(corpus),
        "BLINKY_WORK": str(ckpt / "work"),
        "PYTORCH_CUDA_ALLOC_CONF": "expandable_segments:True",  # as train_pipeline.sh
        "TQDM_MININTERVAL": TQDM_MININTERVAL_S,
        "NUMBA_CACHE_DIR": str(ckpt / "work" / "numba"),  # librosa's numba; /code is read-only
        "PYTHONUNBUFFERED": "1",
    })
    os.environ.update({"BLINKY_DATA": env["BLINKY_DATA"], "BLINKY_WORK": env["BLINKY_WORK"]})
    sys.path.insert(0, str(HERE))
    from scripts.audio import load_config

    cfg = load_config(str(config))
    data = cfg.get("data", {})
    augment = not args.no_augment
    dirs = {
        "audio": corpus_path(corpus, args.audio_dir, data.get("audio_dir")),
        "labels": corpus_path(corpus, args.labels_dir, data.get("labels_dir")),
        "rir": Path(data["rir_dir"]) if augment and data.get("rir_dir") else None,
        "noise": Path(data["noise_dir"]) if augment and data.get("noise_dir") else None,
    }
    # Prep silently skips a missing RIR or noise dir. On the Spark that would train on a
    # different augmentation than the config says, so every named dir must exist.
    absent = {k: str(v) for k, v in dirs.items() if v is not None and not v.is_dir()}
    if dirs["audio"] is None or dirs["labels"] is None:
        return fail("the config names no audio_dir/labels_dir; pass --audio-dir/--labels-dir")
    if absent:
        return fail(f"not in the corpus input: {absent}. Pass the dirs relative to the corpus "
                    f"(see the listing above), or --no-augment")

    processed = ckpt / "processed"
    processed.mkdir(parents=True, exist_ok=True)
    (ckpt / "work").mkdir(parents=True, exist_ok=True)
    prep_cmd = [sys.executable, "scripts/prepare_dataset.py", "--config", str(config),
                "--output-dir", str(processed),
                "--audio-dir", str(dirs["audio"]), "--labels-dir", str(dirs["labels"])]
    if augment:
        prep_cmd.append("--augment")
        if dirs["rir"] is not None:
            prep_cmd += ["--rir-dir", str(dirs["rir"])]
        if dirs["noise"] is not None:
            prep_cmd += ["--noise-dir", str(dirs["noise"])]
    prep_cmd += args.prep_arg
    prep_fp = fingerprint({"cmd": prep_cmd[1:], "cfg": cfg})

    done = processed / PREP_DONE
    if done.is_file():
        got = json.loads(done.read_text()).get("fingerprint")
        if got != prep_fp:
            return fail(f"{done} was written by a different prep ({got} != {prep_fp})")
        print(f"PREP already done ({done}); training", flush=True)
    else:
        print(f"PREP: {' '.join(prep_cmd[1:])}", flush=True)
        t0 = time.time()
        rc = supervise_prep(prep_cmd, env, preempt)
        if rc != 0:
            return fail(f"prepare_dataset.py exited {rc}")
        if not (processed / "X_train.npy").is_file():
            return fail("prepare_dataset.py exited 0 but wrote no X_train.npy")
        tmp = done.with_suffix(".tmp")
        tmp.write_text(json.dumps({"fingerprint": prep_fp, "cmd": prep_cmd[1:],
                                   "seconds": round(time.time() - t0)}, indent=2))
        tmp.replace(done)
    preempt.exit_if_requested("after prep")

    env["SPARK_INPUT_DATA"] = str(processed)
    rc = run_train([sys.executable, "train.py", "--config", str(config), *train_args], env)
    if rc != 0:
        if rc != PREEMPTED_EXIT:
            print(f"ERROR: train.py exited {rc}", file=sys.stderr, flush=True)
        return rc
    (model_dir / "spark_run.json").write_text(json.dumps({
        "config": args.config, "train_args": train_args, "prep": json.loads(done.read_text()),
        "job_id": os.environ.get("SPARK_JOB_ID"),
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
