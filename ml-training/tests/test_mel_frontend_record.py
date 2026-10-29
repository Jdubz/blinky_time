"""The mel front end travels from the dataset to the model header (#291).

prepare_dataset.py records it, train.py carries it, export_tflite.py writes it into the header,
and FrameOnsetNN.h / tools/check_model_input.py refuse a front end the firmware doesn't run.
These test the record and the header check without torch or tflite; the end-to-end flow
through prep and train is asserted in test_spark_entry.py.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
from pathlib import Path

import pytest

ML = Path(__file__).resolve().parents[1]
REPO = ML.parent
sys.path.insert(0, str(ML))

from scripts.mel_frontend import (  # noqa: E402
    RECORD_FILE,
    frontend_record,
    load_spec,
    model_record,
    read_record,
    write_record,
)

_spec = importlib.util.spec_from_file_location("check_model_input",
                                               REPO / "tools" / "check_model_input.py")
check_model_input = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(check_model_input)

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


def header(data: bytes, frontend: int | None, hash_: str | None = None) -> str:
    """A model header as export_tflite.py writes it."""
    hash_ = hash_ or hashlib.sha256(data).hexdigest()[:16]
    fe = "" if frontend is None else f"#define FRAME_ONSET_MODEL_DATA_MEL_FRONTEND {frontend}\n"
    hexed = ", ".join(f"0x{b:02x}" for b in data)
    return (f'#define FRAME_ONSET_MODEL_DATA_HASH "{hash_}"\n{fe}'
            f"alignas(8) static const unsigned char frame_onset_model_data[] = {{\n  {hexed},\n}};\n")


def errors(src: str, waived: str = "fa087e0a364640ab") -> list[str]:
    return check_model_input.frontend_errors(src, check_model_input.model_bytes(src), 1, waived)


def test_header_check_accepts_the_firmware_front_end():
    assert errors(header(b"model", 1)) == []


def test_header_check_refuses_others():
    assert "no FRAME_ONSET_MODEL_DATA_MEL_FRONTEND" in errors(header(b"model", None))[0]
    assert "firmware runs 1" in errors(header(b"model", 2))[0]
    # librosa (0) is waived only for the one model, by the hash of its bytes.
    assert "waived" in errors(header(b"model", 0))[0]
    waived = hashlib.sha256(b"model").hexdigest()[:16]
    assert errors(header(b"model", 0), waived=waived) == []
    # Claiming the waived hash for other bytes is caught: the hash is recomputed.
    forged = errors(header(b"other", 0, hash_=waived), waived=waived)
    assert any("bytes hash to" in e for e in forged) and any("waived" in e for e in forged)


def test_the_embedded_model_passes():
    """The live header is v33 on the librosa front end, waived by its real hash."""
    src = check_model_input.MODEL_H.read_text(encoding="utf-8")
    nn_h = check_model_input.NN_H.read_text(encoding="utf-8")
    assert "#define FRAME_ONSET_MODEL_DATA_MEL_FRONTEND 0" in src
    assert 'WAIVED_LIBROSA_MODEL = "fa087e0a364640ab"' in nn_h
    assert errors(src) == []


def test_export_header_passes_the_check(tmp_path):
    """What export_tflite.py writes is what the check reads. The writer is compiled on its own,
    since importing export_tflite needs TensorFlow."""
    import ast

    src = (ML / "scripts" / "export_tflite.py").read_text(encoding="utf-8")
    fn = next(n for n in ast.parse(src).body
              if isinstance(n, ast.FunctionDef) and n.name == "tflite_to_c_header")
    ns: dict = {"Path": Path}
    exec(compile(ast.Module(body=[fn], type_ignores=[]), "export_tflite.py", "exec"), ns)
    out = tmp_path / "frame_onset_model_data.h"
    for record, want in ((TABLE, []), (LIBROSA, ["waived"])):
        ns["tflite_to_c_header"](b"\x01\x02model", "frame_onset_model_data", str(out), record)
        text = out.read_text()
        assert f"#define FRAME_ONSET_MODEL_DATA_MEL_FRONTEND {record['version']}" in text
        got = errors(text)
        assert len(got) == len(want) and all(w in g for w, g in zip(want, got)), got
