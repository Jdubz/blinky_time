"""The mel front end travels from the dataset to the model header (#291).

prepare_dataset.py records it, train.py carries it, and export_tflite.py writes it into the
header as FRAME_ONSET_MODEL_DATA_MEL_FRONTEND, next to FRAME_ONSET_MODEL_DATA_HASH (the first 16
hex digits of the model bytes' sha256). blinky-firmware refuses a header whose front end isn't its
MEL_FRONTEND_VERSION (FrameOnsetNN.h, tools/check_model_input.py, tested there), and checks that
number against its copy of spec/mel_frontend_v1.json. So the contract between the repos is the
spec's `version`: these tests hold the record and the header to it, without torch, tflite or any
firmware source. The end-to-end flow through prep and train is asserted in test_spark_entry.py.
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

import pytest

ML = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ML))

from scripts.mel_frontend import (  # noqa: E402
    RECORD_FILE,
    frontend_record,
    load_spec,
    model_record,
    read_record,
    write_record,
)

TABLE = frontend_record("firmware_table")
LIBROSA = frontend_record("librosa_htk")


def test_records_name_the_spec():
    assert TABLE["version"] == load_spec()["version"] == 1
    assert len(TABLE["spec_sha256"]) == 64
    assert LIBROSA == {"mel_filterbank": "librosa_htk", "version": 0}
    with pytest.raises(ValueError):
        frontend_record("htk")


def test_read_record_refuses_missing_and_foreign(tmp_path):
    with pytest.raises(FileNotFoundError, match="prepare_dataset.py"):
        read_record(tmp_path)
    write_record(tmp_path, TABLE)
    assert read_record(tmp_path) == TABLE
    # Features made from another spec's table: same name and version, different content.
    (tmp_path / RECORD_FILE).write_text(json.dumps({**TABLE, "spec_sha256": "0" * 64}))
    with pytest.raises(ValueError, match="re-run prepare_dataset.py"):
        read_record(tmp_path)
    (tmp_path / RECORD_FILE).write_text(json.dumps({"mel_filterbank": "slaney"}))
    with pytest.raises(ValueError, match="not a mel front-end record"):
        read_record(tmp_path)


def test_model_record_sources_must_exist_and_agree(tmp_path):
    model = tmp_path / "best_model.pt"
    with pytest.raises(ValueError, match="--mel-frontend"):
        model_record(model, None, None)
    # A model trained before records existed: stated explicitly.
    assert model_record(model, None, "librosa_htk") == LIBROSA
    write_record(tmp_path, TABLE)  # train.py's record beside the model
    assert model_record(model, None, None) == TABLE
    assert model_record(model, TABLE, "firmware_table") == TABLE
    with pytest.raises(ValueError, match="--mel-frontend says"):
        model_record(model, None, "librosa_htk")
    with pytest.raises(ValueError):
        model_record(model, LIBROSA, None)


def read_header(src: str) -> tuple[int, str, bytes]:
    """(front end, declared hash, model bytes) from a model header, read the way the firmware's
    tools/check_model_input.py reads one."""
    fe = re.search(r"#define FRAME_ONSET_MODEL_DATA_MEL_FRONTEND (\d+)", src)
    hash_ = re.search(r'#define FRAME_ONSET_MODEL_DATA_HASH "([0-9a-f]+)"', src)
    assert fe and hash_, src[:300]
    start = src.index("{", src.index("frame_onset_model_data[]"))
    body = src[start:src.index("};", start)]
    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))
    return int(fe.group(1)), hash_.group(1), data


def test_export_header_records_the_spec_version(tmp_path):
    """What export_tflite.py writes carries the spec's version (the number the firmware compares
    with its MEL_FRONTEND_VERSION) and the hash of exactly the bytes it embeds. The writer is
    compiled on its own, since importing export_tflite needs TensorFlow."""
    import ast

    src = (ML / "scripts" / "export_tflite.py").read_text(encoding="utf-8")
    fn = next(n for n in ast.parse(src).body
              if isinstance(n, ast.FunctionDef) and n.name == "tflite_to_c_header")
    ns: dict = {"Path": Path}
    exec(compile(ast.Module(body=[fn], type_ignores=[]), "export_tflite.py", "exec"), ns)
    out = tmp_path / "frame_onset_model_data.h"
    data = b"\x01\x02model"
    for record, want in ((TABLE, load_spec()["version"]), (LIBROSA, 0)):
        ns["tflite_to_c_header"](data, "frame_onset_model_data", str(out), record)
        fe, hash_, embedded = read_header(out.read_text())
        assert fe == want
        assert embedded == data
        assert hash_ == hashlib.sha256(data).hexdigest()[:16]
