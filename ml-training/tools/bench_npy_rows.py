#!/usr/bin/env python3
"""Benchmark #287: memmap vs NpyRows (pread + POSIX_FADV_DONTNEED) under the
training DataLoader's access pattern (shuffled rows, batch 4096, workers 4).

Reports rows/s and how much the page cache grew (Cached in /proc/meminfo)
over the run. Linux only. Generates a synthetic X of --gb gigabytes of
float32 rows shaped like training chunks (128 x 50) unless --x is given.

usage: python tools/bench_npy_rows.py --dir /mnt/nvme/scratch --gb 12 [--batches 40]
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np
import torch
from torch.utils.data import DataLoader, Dataset

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from npy_rows import NpyRows  # noqa: E402


def cached_gib() -> float:
    for line in open("/proc/meminfo"):
        if line.startswith("Cached:"):
            return int(line.split()[1]) / 2**20
    raise RuntimeError("no Cached in /proc/meminfo")


class MM(Dataset):
    def __init__(self, p):
        self.X = np.load(p, mmap_mode="r")

    def __len__(self):
        return len(self.X)

    def __getitem__(self, i):
        return torch.from_numpy(self.X[i].copy()).float()


class NR(Dataset):
    def __init__(self, p):
        self.X = NpyRows(p)

    def __len__(self):
        return len(self.X)

    def __getitem__(self, i):
        return torch.from_numpy(self.X[i]).float()


def run(ds, batches, batch, workers, max_s):
    g = torch.Generator().manual_seed(0)
    dl = DataLoader(ds, batch_size=batch, shuffle=True, num_workers=workers, generator=g)
    c0, t0, n = cached_gib(), time.time(), 0
    for k, x in enumerate(dl):
        n += x.shape[0]
        if k + 1 >= batches or time.time() - t0 > max_s:
            break
    return n / (time.time() - t0), cached_gib() - c0, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--x")
    ap.add_argument("--gb", type=float, default=12)
    ap.add_argument("--batches", type=int, default=40)
    ap.add_argument("--batch", type=int, default=4096)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--max-s", type=float, default=300, help="cap per variant (rows/s is still exact)")
    a = ap.parse_args()
    p = Path(a.x) if a.x else Path(a.dir) / "bench_X.npy"
    if not p.exists():
        rows = int(a.gb * 2**30 / (128 * 50 * 4))
        print(f"writing {p} ({rows} rows x 128 x 50 float32, {a.gb:.0f} GiB)", flush=True)
        mm = np.lib.format.open_memmap(p, mode="w+", dtype=np.float32, shape=(rows, 128, 50))
        step = 20000
        rng = np.random.default_rng(0)
        for i in range(0, rows, step):
            mm[i:i + step] = rng.standard_normal((min(step, rows - i), 128, 50), dtype=np.float32)
        mm.flush()
        del mm
        print("written; drop the file from cache before measuring", flush=True)
        NpyRows(p)  # header check
        import os
        fd = os.open(p, os.O_RDONLY)
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        os.close(fd)
    for name, ds in (("NpyRows", NR(p)), ("memmap", MM(p)), ("NpyRows", NR(p))):
        rps, dc, n = run(ds, a.batches, a.batch, a.workers, a.max_s)
        print(f"{name:8s} {rps:10.0f} rows/s  page cache +{dc:6.2f} GiB over {n} rows", flush=True)
        # Leave the box as we found it: drop whatever the run cached of this file.
        import os
        fd = os.open(p, os.O_RDONLY)
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        os.close(fd)


if __name__ == "__main__":
    main()
