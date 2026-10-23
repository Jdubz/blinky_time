"""NpyRows must return exactly what a memmap returns (#287)."""

from __future__ import annotations

import pickle
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

from npy_rows import NpyRows  # noqa: E402


@pytest.mark.parametrize("shape,dtype", [((37, 128, 50), np.float32), ((11, 128), np.float16), ((5,), np.int64)])
def test_rows_match_memmap(tmp_path, shape, dtype):
    rng = np.random.default_rng(0)
    a = (rng.standard_normal(shape) * 100).astype(dtype)
    p = tmp_path / "a.npy"
    np.save(p, a)
    r = NpyRows(p)
    mm = np.load(p, mmap_mode="r")
    assert len(r) == shape[0]
    for i in [0, 1, shape[0] - 1, -1] + list(rng.integers(0, shape[0], 10)):
        row = r[i]
        assert row.dtype == a.dtype and row.shape == a.shape[1:]
        np.testing.assert_array_equal(row, mm[i])
        row_copy = np.array(row)
        row_copy[...] = 0  # fresh, writable buffer; never a view into the file
        np.testing.assert_array_equal(r[i], mm[i])


def test_rows_are_writable(tmp_path):
    p = tmp_path / "w.npy"
    np.save(p, np.arange(12, dtype=np.float32).reshape(3, 4))
    row = NpyRows(p)[1]
    row += 1  # torch.from_numpy needs a writable array
    assert row.tolist() == [5, 6, 7, 8]


def test_out_of_range_and_truncated(tmp_path):
    p = tmp_path / "t.npy"
    np.save(p, np.zeros((4, 8), np.float32))
    r = NpyRows(p)
    with pytest.raises(IndexError):
        r[4]
    with pytest.raises(IndexError):
        r[-5]
    data = p.read_bytes()
    p.write_bytes(data[:-16])
    with pytest.raises(ValueError, match="truncated"):
        NpyRows(p)


def test_fortran_order_refused(tmp_path):
    p = tmp_path / "f.npy"
    np.save(p, np.asfortranarray(np.zeros((4, 8), np.float32)))
    with pytest.raises(ValueError, match="Fortran"):
        NpyRows(p)


def test_pickles_without_handles(tmp_path):
    p = tmp_path / "p.npy"
    np.save(p, np.arange(20, dtype=np.float32).reshape(5, 4))
    r = NpyRows(p)
    _ = r[2]
    r2 = pickle.loads(pickle.dumps(r))
    assert r2._fd is None
    np.testing.assert_array_equal(r2[3], [12, 13, 14, 15])
