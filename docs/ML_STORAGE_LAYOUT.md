# ML data: where it lives

Where blinky's ML training data is, and how configs find it. Training is paused (`ML_IMPROVEMENT_PLAN.md`); when it runs, it runs on the DGX Spark as a job, never on devtop or the test box.

## The archive (bignasty)

On 2026-07-10 devtop's `/mnt/storage` became the my-spark object store, and the old devtop data layout (the `/mnt/nvme` pool, `/mnt/storage/blinky-ml-data`) no longer exists. blinky's ML data was archived first, to bignasty's RAID:

`bignasty:/srv/dev-disk-by-uuid-45e47416-96ad-41be-9fc0-582e15241cbd/archive/devtop-2026-07/blinky/`

| Path in the archive | Size | Contents |
|---|---|---|
| `blinky-ml-data/stems/htdemucs` | 172 GB | Demucs stems, 6,752 tracks |
| `blinky-ml-data/audio/fma`, `audio/metadata` | 29 GB, 1.7 GB | training audio |
| `blinky-ml-data/audio/combined` | — | 6,111 symlinks, **all broken** (absolute `/mnt/storage/blinky-ml-data/audio/fma/…` targets). Relink to `audio/fma` when restoring. |
| `blinky-ml-data/labels/` | ~300 MB | `onsets_consensus`, `kick_weighted_drums`, `kick_weighted_clean`, `onsets_librosa`, `fma` (6,111 tracks) |
| `blinky-ml-data/rir`, `noise`, `calibration` | ~360 MB | augmentation inputs |
| `processed_v36` | 217 GB | prepared tensors for the dead v36 (80-mel) config; re-prep instead |
| `blinky-ml-archive/_archive_beats_2026_04_29` | 1 GB | archived beat labels (never a training input) |

bignasty's RAID is production's only redundant backup: read from the archive, never write or delete there.

## The Spark's object store (what jobs read)

`gpu_task` inputs are object-store key prefixes, never host paths, and nothing reads the data over ssh.

- `datasets/blinky-corpus/`: `audio/fma` (6,093 mp3s), the label sets, and `rir`/`noise`/`calibration` (my-spark #1545). A job that declares it gets a copy in its workspace.
- `datasets/blinky-stems/htdemucs/`: the 172 GB archive stems, copied as-is with a manifest (my-spark #1558), laid out `<track>/<stem>.wav`. Not re-separated: that would spend about 9 h of GPU and replace the stems the existing `kick_weighted` labels came from.
- `blinky/stems/<model>/`: output of the Spark `separate` job, for tracks the archive lacks or a new separator. Never reaped.

A host copy also sits at `/home/blinky/corpus` on the Spark (6.8 GB); jobs don't read it.

Prepared training tensors (about 200 GB at full size) are made inside the job's checkpoint directory by `ml-training/spark_entry.py` and never go to the store.

## Storage roots (#286)

Configs, the Makefile and `train_pipeline.sh` name `${BLINKY_DATA}` (corpora, labels, RIR, noise) and `${BLINKY_WORK}` (`processed_v*`, mel cache, outputs) instead of mounts. Set them to wherever a local copy lives. Unset, they default to the old devtop paths (`/mnt/storage/blinky-ml-data`, `/mnt/nvme`), which no longer exist. `scripts/audio.py:load_config` expands them and refuses any other `${…}`, and `tests/test_storage_roots.py` fails on a config that names a machine mount.

New configs set paths through the roots:

```yaml
data:
  processed_dir: "${BLINKY_WORK}/processed_v{N}"
```

A local prep needs a lot of disk: peak is roughly twice the final `X_train` size while shards merge (about 370 GB for the full corpus at 80 mel). `prepare_dataset.py` estimates the peak and checks free space before it starts, and `train_pipeline.sh` passes it `--auto-clean-stale`, which deletes other `processed_v*` and stale mel caches under `${BLINKY_WORK}`.
