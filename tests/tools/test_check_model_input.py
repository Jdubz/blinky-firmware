"""tools/check_model_input.py's mel front-end check (blinky_time#291): a model header must record
the front end this firmware runs (MEL_FRONTEND_VERSION), or be the one librosa model waived by
the hash of its bytes, and the header's hash must be the bytes' own.

Moved from blinky_time's ml-training/tests/test_mel_frontend_record.py, which loaded this tool
from its firmware/ submodule; since decoupling step 2 that repo checks only that its exporter
writes the spec's version, and the tool is tested here, beside it. The model-input-check job
also runs the tool itself on the embedded header.
"""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))
import check_model_input  # noqa: E402

FIRMWARE_VERSION = 1


def header(data: bytes, frontend: int | None, hash_: str | None = None) -> str:
    """A model header as blinky_time's export_tflite.py writes it."""
    hash_ = hash_ or hashlib.sha256(data).hexdigest()[:16]
    fe = "" if frontend is None else f"#define FRAME_ONSET_MODEL_DATA_MEL_FRONTEND {frontend}\n"
    hexed = ", ".join(f"0x{b:02x}" for b in data)
    return (f'#define FRAME_ONSET_MODEL_DATA_HASH "{hash_}"\n{fe}'
            f"alignas(8) static const unsigned char frame_onset_model_data[] = {{\n  {hexed},\n}};\n")


def errors(src: str, waived: str = "fa087e0a364640ab") -> list[str]:
    return check_model_input.frontend_errors(src, check_model_input.model_bytes(src),
                                             FIRMWARE_VERSION, waived)


def test_the_firmware_runs_front_end_1():
    """The version these tests assume, and the one the mel contract's spec carries
    (tests/parity/test_mel_contract.py checks the spec against it)."""
    assert check_model_input.const(check_model_input.SPECTRAL_H,
                                   r"MEL_FRONTEND_VERSION = (\d+);") == FIRMWARE_VERSION


def test_header_check_accepts_the_firmware_front_end():
    assert errors(header(b"model", FIRMWARE_VERSION)) == []


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
