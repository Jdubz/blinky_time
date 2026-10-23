# ML Training Storage Layout

**Last updated:** 2026-10-23

This doc is the authoritative reference for where ML training data lives, why it lives there, and how prep + training use disk. The layout is designed so disk-full failures during prep stop being possible at the workloads we currently run.

## Where the data is now (2026-10-23)

**The data is not on devtop.** On 2026-07-10 (Phase 0 of the homelab cutover, `homelab-stacks`), `/mnt/storage` became the my-spark object store (`/mnt/storage/spark-artifacts`). blinky's ML data was archived first, to bignasty's RAID:

`bignasty:/srv/dev-disk-by-uuid-45e47416-96ad-41be-9fc0-582e15241cbd/archive/devtop-2026-07/blinky/`

| Path in the archive | Size | Contents |
|---|---|---|
| `blinky-ml-data/stems/htdemucs` | 172 GB | Demucs stems, 6,752 tracks |
| `blinky-ml-data/audio/fma`, `audio/metadata` | 29 GB, 1.7 GB | training audio |
| `blinky-ml-data/audio/combined` | — | 6,111 symlinks, **all broken**: absolute `/mnt/storage/blinky-ml-data/audio/fma/…` targets. Relink to `audio/fma` when restoring. |
| `blinky-ml-data/labels/` | ~300 MB | `onsets_consensus`, `kick_weighted_drums`, `kick_weighted_clean`, `onsets_librosa`, `fma` (6,111 tracks) |
| `blinky-ml-data/rir`, `noise`, `calibration` | ~360 MB | augmentation inputs |
| `processed_v36` | 217 GB | prepared tensors for the dead v36 (80-mel) config; re-prep instead |
| `blinky-ml-archive/_archive_beats_2026_04_29` | 1 GB | archived beat labels (never a training input) |

bignasty's RAID is production's only redundant backup (homelab-stacks cutover runbook): read from the archive, never write or delete there. Restoring to a training host (the Spark's dataset volume, my-spark#1525) is part of the Spark onboarding (#285).

**Storage roots (#286).** Configs, the Makefile and `train_pipeline.sh` name `${BLINKY_DATA}` (warm tier: corpora, labels, RIR, noise) and `${BLINKY_WORK}` (hot tier: `processed_v*`, mel cache, outputs) instead of mounts. Unset, they default to the devtop layout below (`/mnt/storage/blinky-ml-data`, `/mnt/nvme`); a job sets them. `scripts/audio.py:load_config` expands them and refuses any other `${…}`.

The tier design below is the devtop layout as it was until July 2026.

## TL;DR — where things go

| Tier | Mount | Backing | Capacity | Purpose |
|------|-------|---------|----------|---------|
| **Hot** | `/mnt/nvme` | LVM-linear over 2× USB-NVMe (Samsung 970 EVO Plus 250GB + Samsung MZVLQ256HAJD via 2× RTL9210) | ~462 GB | Active `processed_v*/`, `mel_cache/`, training `outputs/`, prep scratch — anything written or read hot during a run |
| **Warm** | `/mnt/storage` | SATA SSD (Samsung 860 EVO 500GB) | 458 GB | Audio corpora (FMA, GiantSteps, EDM symlinks), labels, RIR, noise, stems archive — large but read-once-per-prep, latency-tolerant |
| **OS** | `/` | Internal NVMe (Samsung 970 EVO 1TB) | 916 GB | Code, virtualenvs, system. **Do not put bulk data here.** |
| **Other** | `/mnt/speedy` | NVMe SSD | 458 GB | Unrelated `imagineer/` project — leave alone |

## Why this split

The pain point we're fixing: dataprep peak disk during the train-shard merge phase is roughly **2× the X_train final size** because all shards still exist on disk while the final `X_train.npy` memmap is being written. For a 6700-track corpus at 30 mel bands and 128 frame chunks, that's:

- X_train final: ~183 GB
- Train shards live: ~183 GB
- **Peak: ~370 GB on the same filesystem**

Add Y arrays, val arrays, and run-specific mel cache and you can hit 400 GB. No single drive in the system was big enough to absorb that without other content getting in the way. The 463 GB pool gives us ~95 GB of headroom over peak with the rest of the system untouched.

## The NVMe pool

Two USB-NVMe enclosures (Realtek RTL9210, USB 3.0) each holding one M.2 NVMe drive, joined as a single LVM-linear volume:

```
/dev/sdc (Samsung MZVLQ256HAJD 256 GB)  ─┐
                                         ├── VG "nvme" ── LV "data" (ext4, ~462 GB) ── /mnt/nvme
/dev/sdd (Samsung 970 EVO Plus 250 GB)  ─┘
```

**Linear, not striped:** both enclosures are on the same USB 3.0 root hub (Bus 006), so striping wouldn't actually parallelize I/O — the controller is the bottleneck. Linear keeps each file on a single PV, so a single drive failure loses only the files that landed on that PV (still bad, but recoverable to "redo prep" rather than "every file is corrupt").

**Drive identity is captured by serial number** in the format script (`/tmp/pool_nvme.sh`, ID_SERIAL_SHORT) so the script aborts if `/dev/sdc` or `/dev/sdd` is something else when run.

### Mount and fstab

```
/etc/fstab:
UUID=<lv-uuid>  /mnt/nvme  ext4  defaults,nofail,x-systemd.device-timeout=10s  0  2
```

`nofail` means a USB drive disconnect at boot won't block the system from coming up. `x-systemd.device-timeout=10s` caps the wait. `defaults` includes `relatime` which is fine for ML workloads.

### USB autosuspend disabled

A udev rule is installed that disables USB autosuspend for the RTL9210 enclosures by VID:PID (0bda:9210):

```
/etc/udev/rules.d/99-blinky-nvme-noautosuspend.rules
```

Without this, the kernel would idle-suspend the bridge during long sequential reads (a common training pattern: large memmap warm-up + steady-state batches), and the bridge sometimes doesn't return cleanly — the device drops, prep/training crashes, and you have to physically replug. Suspended-while-active disconnects on USB-NVMe bridges are well-documented; the rule is the cheapest mitigation.

## Path reference

### Read paths (where prep and training READ from)

| Path | Content | Tier |
|------|---------|------|
| `/mnt/storage/blinky-ml-data/audio/combined/` | Symlinked union of all audio corpora (FMA, GiantSteps, EDM test set excluded) | Warm |
| `/mnt/storage/blinky-ml-data/labels/onsets_consensus/` | 5-system onset consensus labels (`base.yaml` default `labels_dir`; see CLAUDE.md "Task is Onset Detection") | Warm |
| `/mnt/storage/blinky-ml-data/labels/kick_weighted_drums/`, `kick_weighted_clean/` | Kick/snare onset labels from drum stems. v33 trained on `kick_weighted_drums`; v34d and v36 on `kick_weighted_clean` | Warm |
| `/mnt/storage/blinky-ml-data/labels/onsets_human/` | Hand edits from `tools/label-reviewer/`, overlaid on the consensus labels at prep time | Warm |
| `/mnt/storage/blinky-ml-data/labels/_archive_beats_2026_04_29/` | Archived BEAT consensus (`consensus_v*`). Never a training input; `prepare_dataset.py` refuses these paths | Warm |
| `/mnt/storage/blinky-ml-data/rir/processed/` | Room impulse responses for augmentation | Warm |
| `/mnt/storage/blinky-ml-data/noise/` | Background noise clips for SNR mixing | Warm |
| `/mnt/storage/blinky-ml-data/stems_archive/` | Demucs-separated stems (cold archive, was on `/`) | Warm |
| `../blinky-test-player/music/edm/` | 18-track EDM validation corpus (in repo, not on /mnt/storage) | Repo |
| `/mnt/nvme/processed_v*/X_*.npy, Y_*.npy` | Prepared training tensors (memmap-streamed by train.py) | Hot |
| `/mnt/nvme/mel_cache/<config-hash>/<stem>/` | Cached mel spectrograms keyed by config hash | Hot |

### Write paths (where prep and training WRITE)

| Path | Pipeline phase | Peak size | Lifetime |
|------|----------------|-----------|----------|
| `/mnt/nvme/processed_v{N}/` | dataprep output | ~220 GB final + ~180 GB transient shards = ~400 GB peak | Per version, deleted by `--auto-clean-stale` on next prep |
| `/mnt/nvme/processed_v{N}/blinky_train_*/` | dataprep transient shards | ~180 GB | Auto-deleted at end of train merge |
| `/mnt/nvme/processed_v{N}/blinky_val_*/` | dataprep transient shards | ~30 GB | Auto-deleted at end of val merge |
| `/mnt/nvme/processed_v{N}/.prep_progress_*.json` | prep resumption manifest | <1 MB | Deleted on success; persists on crash for resume |
| `/mnt/nvme/mel_cache/<hash>/` | mel cache | ~10 MB × n_files × n_aug_variants | Per config hash; auto-pruned at start of each prep |
| `/mnt/nvme/outputs/<run-name>/` | training checkpoints, eval, export | ~5 MB per run | Permanent (manual cleanup) |
| `/mnt/storage/blinky-ml-data/labels/onsets_consensus/` | label generation (one-time per corpus change) | ~300 MB | Permanent |

## Disk budget — where the failures used to come from

`train_pipeline.sh` runs a preflight that estimates peak disk and aborts if free space is below the estimate. Calibration (from prepare_dataset.py):

```
peak_gb ≈ 150 × (n_mels/30) × (n_files/6732) × (chunk_frames/128) + 30 GB scratch
```

The formula is a deliberately high upper bound. Example: `processed_v36` (80 mel, 128-frame chunks; 4.77M chunks vs v33's 7.18M after clean-label filtering) measured **~217 GB** for the final arrays. Add ~150 GB of train shards on disk during merge: **~370 GB true peak**. For 80 mel at the full 6732 files the formula asks for ~430 GB free, so the precheck may refuse an 80-mel prep on this pool even when the real peak would fit.

The pool's 462 GB capacity is sized for this with ~92 GB headroom for one prep run. **Two concurrent preps will not fit** — that's by design; sequential is fine.

## Operational rules

### Starting a prep run

`train_pipeline.sh` now passes `--auto-clean-stale` to `prepare_dataset.py` by default. That deletes:

- All `processed_v*/` siblings whose name doesn't match the current run
- All `mel_cache/<hash>/` subtrees whose hash doesn't match the current config

**You will lose old processed datasets when you start a new prep.** Re-prep takes 1-2 hours. They're regenerable from audio + labels + config, so this is the right tradeoff against silent disk-full crashes mid-run.

If you need to keep an old version around (e.g. comparing v36 vs v37), move it out of `/mnt/nvme/` first:

```bash
mv /mnt/nvme/processed_v36 /mnt/storage/blinky-ml-data/processed_v36_keep
# then run prep for v37
```

### Where do training outputs go?

`train_pipeline.sh` defaults `OUTPUT_DIR=/mnt/nvme/outputs/<run-name>`. Override with `OUTPUT_DIR=...` env var if you want a different target. Outputs are tiny (~5 MB per run); accumulate freely, sweep manually when you stop caring about old runs.

### Adding a new version

Configs override `data.processed_dir` per version. New configs should set:

```yaml
data:
  processed_dir: "/mnt/nvme/processed_v{N}"
```

`base.yaml` defaults the value if a config doesn't override.

### What if a USB drive disconnects mid-training

The pool goes read-only or unresponsive; training crashes with I/O errors. Recovery:

1. Replug the disconnected enclosure.
2. `sudo lvchange -ay nvme/data && sudo mount /mnt/nvme`
3. Re-run the same `train.py` command with the same `--output-dir`. `train.py` resumes on its own from `outputs/<run-name>/training_checkpoint.pt` when that file exists. Run it in tmux (`train.py` refuses to start outside tmux/screen).

If a drive truly fails (vs. flaky cable/hub), the pool is degraded — `pvs` shows the missing PV. Re-format the replacement, `pvcreate`, `vgextend`, then re-prep (data on the failed PV is regenerable; data on the surviving PV is fine but re-prep is simpler than partial recovery).

### Disk usage check before starting a long run

```bash
df -h /mnt/nvme           # need >400 GB free for a fresh prep
ls /mnt/nvme/processed_v* # any stale versions?
du -sh /mnt/nvme/mel_cache  # any stale caches?
```

`prepare_dataset.py` does this automatically; the manual check is for sanity before starting a prep+train run that you can't easily babysit.

## Migration history (2026-05-02)

Original layout had `processed_v36` (217 GB) on `/mnt/storage` (458 GB SATA SSD with 188 GB free). This was below the 217 GB target with no prep headroom — disk-full crashes were inevitable at peak. The 172 GB stems archive lived on `/` (root NVMe) via a symlink, which obscured the true storage situation.

Migration steps:

1. Two USB-NVMe drives attached via RTL9210 enclosures, formatted, joined as LVM-linear pool.
2. `processed_v36` (217 GB) moved from `/mnt/storage` → `/mnt/nvme`.
3. Stems archive (172 GB) moved from `/home/.../ml-training/data/archive/stems/` → `/mnt/storage/blinky-ml-data/stems_archive/`.
4. `base.yaml`, `Makefile`, all 29 version configs, and `train_pipeline.sh` updated to point at the new layout.
5. `--auto-clean-stale` made default in `train_pipeline.sh`.
6. udev rule installed to disable USB autosuspend on the RTL9210 bridges.

Net effect: `/mnt/storage` now has ~410 GB free for warm-tier growth, `/` has 172 GB more headroom, and `/mnt/nvme` provides 462 GB dedicated to the high-churn workload.
