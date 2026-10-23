"""Row reads from a .npy file that leave nothing in the page cache (#287).

Training samples rows of X_train.npy (~217 GB) in shuffled order. Through a
memmap every row read fills the OS page cache, which has no reuse value: a
shuffled epoch over an array larger than RAM evicts each page long before it
is read again. On the DGX Spark that cache is worse than useless, because
its 128 GB is unified CPU/GPU memory: the platform reads MemAvailable minus
MemFree (i.e. the page cache) as memory a cold GPU job cannot get, counts
refault stalls as memory pressure, and drops caches under a per-tenant
reclaim-flap breaker (Josh-Wentworth/my-spark#1525). A memmap-streaming
trainer generates exactly that.

`NpyRows` reads one row with `pread` and immediately tells the kernel to
drop that range (`posix_fadvise(..., POSIX_FADV_DONTNEED)`, rounded out to
whole pages). Same results as `np.load(path, mmap_mode='r')[i]`, as a fresh
writable array.

Linux-only by design (training runs on Linux). On a platform without
`os.pread` / `os.posix_fadvise` it falls back to a memmap and says so once on
stderr, so a dev box can still run the code path.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import numpy as np

_PAGE = os.sysconf("SC_PAGE_SIZE") if hasattr(os, "sysconf") else 4096
_HAVE_FADVISE = hasattr(os, "pread") and hasattr(os, "posix_fadvise")
_warned = False


class NpyRows:
    """Random access to the rows (first axis) of a C-ordered .npy file."""

    def __init__(self, path: str | Path):
        self.path = str(path)
        with open(self.path, "rb") as f:
            version = np.lib.format.read_magic(f)
            if version == (1, 0):
                shape, fortran, dtype = np.lib.format.read_array_header_1_0(f)
            elif version in ((2, 0), (3, 0)):
                shape, fortran, dtype = np.lib.format.read_array_header_2_0(f)
            else:
                raise ValueError(f"{self.path}: unsupported .npy format version {version}")
            self.data_offset = f.tell()
        if fortran:
            raise ValueError(f"{self.path}: Fortran-ordered arrays are not supported")
        if dtype.hasobject:
            raise ValueError(f"{self.path}: object arrays are not supported")
        if len(shape) < 1:
            raise ValueError(f"{self.path}: 0-d array has no rows")
        self.shape = tuple(shape)
        self.dtype = dtype
        self.row_shape = self.shape[1:]
        self.row_bytes = int(np.prod(self.row_shape, dtype=np.int64)) * dtype.itemsize
        expected = self.data_offset + self.shape[0] * self.row_bytes
        actual = os.path.getsize(self.path)
        if actual < expected:
            raise ValueError(f"{self.path}: truncated ({actual} bytes, header implies {expected})")
        self._fd: int | None = None
        self._pid: int | None = None
        self._mm: np.ndarray | None = None

    def __len__(self) -> int:
        return self.shape[0]

    def _open(self) -> int:
        # One descriptor per process: DataLoader workers fork after __init__,
        # and a descriptor shared across a fork shares its file offset (pread
        # doesn't use it, but keep each worker's own anyway).
        if self._fd is None or self._pid != os.getpid():
            self._fd = os.open(self.path, os.O_RDONLY)
            self._pid = os.getpid()
        return self._fd

    def __getitem__(self, i: int) -> np.ndarray:
        global _warned
        n = self.shape[0]
        i = int(i)
        if i < 0:
            i += n
        if not 0 <= i < n:
            raise IndexError(f"row {i} out of range for {n} rows in {self.path}")
        if not _HAVE_FADVISE:
            if not _warned:
                print(f"WARNING: npy_rows: no os.pread/posix_fadvise on {sys.platform}; reading through a "
                      "memmap, so row reads DO fill the page cache here", file=sys.stderr)
                _warned = True
            if self._mm is None:
                self._mm = np.load(self.path, mmap_mode="r")
            return np.array(self._mm[i])
        fd = self._open()
        off = self.data_offset + i * self.row_bytes
        buf = bytearray(self.row_bytes)
        got = os.preadv(fd, [buf], off)
        if got != self.row_bytes:
            raise OSError(f"{self.path}: short read at row {i} ({got} of {self.row_bytes} bytes)")
        start = off - off % _PAGE
        end = -(-(off + self.row_bytes) // _PAGE) * _PAGE
        os.posix_fadvise(fd, start, end - start, os.POSIX_FADV_DONTNEED)
        return np.frombuffer(buf, dtype=self.dtype).reshape(self.row_shape)

    def __getstate__(self):
        # Picklable for spawn-based DataLoader workers: drop process-local handles.
        state = self.__dict__.copy()
        state["_fd"] = None
        state["_pid"] = None
        state["_mm"] = None
        return state
