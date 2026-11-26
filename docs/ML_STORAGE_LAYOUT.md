# ML data: where it lives

Where blinky's ML training data is, and how configs find it. Training is paused (`ML_IMPROVEMENT_PLAN.md`); when it runs, it runs on the DGX Spark as a job, never on the build host or the test box.

## The archive (the NAS)

On 2026-07-10 the build host's `/mnt/storage` became the my-spark object store, and the old build-host data layout (the `/mnt/nvme` pool, `/mnt/storage/blinky-ml-data`) no longer exists. blinky's ML data was archived first, to the archive on the NAS's RAID. Its path is in the owner's private notes: this repo is public, and it names no host path or address. The archive's `blinky/` directory holds:

| Path in the archive | Size | Contents |
|---|---|---|
| `blinky-ml-data/stems/htdemucs` | 172 GB | Demucs stems, 6,752 tracks |
| `blinky-ml-data/audio/fma`, `audio/metadata` | 29 GB, 1.7 GB | training audio |
| `blinky-ml-data/audio/combined` | — | 6,111 symlinks, **all broken** (absolute `/mnt/storage/blinky-ml-data/audio/fma/…` targets). Relink to `audio/fma` when restoring. |
| `blinky-ml-data/labels/` | ~300 MB | `onsets_consensus`, `kick_weighted_drums`, `kick_weighted_clean`, `onsets_librosa`, `fma` (6,111 tracks) |
| `blinky-ml-data/rir`, `noise`, `calibration` | ~360 MB | augmentation inputs |
| `processed_v36` | 217 GB | prepared tensors for the dead v36 (80-mel) config; re-prep instead |
| `blinky-ml-archive/_archive_beats_2026_04_29` | 1 GB | archived beat labels (never a training input) |

The NAS's RAID is production's only redundant backup: read from the archive, never write or delete there.

## The Spark's object store (what jobs read)

`gpu_task` inputs are object-store key prefixes, never host paths, and nothing reads the data over ssh.

- `datasets/blinky-corpus/`: `audio/fma` (6,093 mp3s), the label sets, and `rir`/`noise`/`calibration` (my-spark #1545). A job that declares it gets a copy in its workspace.
- `datasets/blinky-stems/htdemucs/`: the 172 GB archive stems, copied as-is with a manifest (my-spark #1558), laid out `<track>/<stem>.wav`. Not re-separated: that would spend about 9 h of GPU and replace the stems the existing `kick_weighted` labels came from.
- `blinky/stems/<model>/`: output of the Spark `separate` job, for tracks the archive lacks or a new separator. Never reaped.

A host copy also sits on the Spark (6.8 GB); jobs don't read it.

Prepared training tensors (about 200 GB at full size) are made inside the job's checkpoint directory by `ml-training/spark_entry.py` and never go to the store.

## Storage roots (#286)

Configs, scripts, the Makefile and `train_pipeline.sh` name `${BLINKY_DATA}` (corpora, labels, RIR, noise) and `${BLINKY_WORK}` (`processed_v*`, mel cache, outputs) instead of mounts. Set them to wherever a local copy lives.

**There are no defaults.** Until 2026-11-25 an unset root fell back to the old build-host paths (`/mnt/storage/blinky-ml-data`, `/mnt/nvme`), which exist on no machine. A run then failed late, as a missing file under a dead mount, or wrote a fresh tree there. Now an unset or empty root fails at once, naming the variable:
- `scripts/audio.py` (`storage_root`, `load_config`) raises `StorageRootUnset`.
- A script's directory default (`type=root_path`, for example `--kw-dir` defaulting to `${BLINKY_DATA}/labels/kick_weighted_drums`) stops with an argparse error naming the option and the variable. It does so only when the default is used, so a run that passes the directory needs no root.
- The Makefile's targets, `train_pipeline.sh` and the other shell scripts stop with `BLINKY_DATA is unset` or `BLINKY_WORK is unset`.
- The label reviewer skips its `training` corpus and says why. Its review state then needs `--review-state`.
- `scripts/disk-audit.sh` skips its ML section and says why.

`load_config` expands every root a config names, so even a script that reads only a config's model or mel settings needs both roots set. Any value will do for those; the CI tests that do this set placeholder values.

**What sets them where:**
- **On the Spark,** `spark_entry.py` (the `gpu_task` entry point that `spark_jobs.py train` submits) sets `BLINKY_DATA` to the corpus input and `BLINKY_WORK` to the checkpoint dir's `work/` before it loads the config. Nothing there relied on the old defaults.
- **The CI trainer-contract test** runs `train.py` directly. It did rely on them, silently: its config paths expanded to the dead mounts, and `train.py` read its data from `$SPARK_INPUT_DATA` instead. The test now sets the roots as `spark_entry.py` does.

`load_config` refuses any `${…}` other than the two roots. `tests/test_storage_roots.py` fails in four cases:
- a root has a default;
- a config names a machine mount;
- a tracked file outside the archives names the retired layout (`ml-training/archive/` is dead code, left as it was, and nothing live imports it);
- a script default doesn't fail as described above.

New configs set paths through the roots:

```yaml
data:
  processed_dir: "${BLINKY_WORK}/processed_v{N}"
```

A local prep needs a lot of disk: peak is roughly twice the final `X_train` size while shards merge (about 370 GB for the full corpus at 80 mel). `prepare_dataset.py` estimates the peak and checks free space before it starts, and `train_pipeline.sh` passes it `--auto-clean-stale`, which deletes other `processed_v*` and stale mel caches under `${BLINKY_WORK}`.
