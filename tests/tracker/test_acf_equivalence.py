"""acf_equivalence.py's gate: it must fail when it compared nothing.

A stand-in harness writes the trace; the corpus is two empty stems, decoding is a no-op. An
empty trace (the --acf-trace hook unwired) used to pass with "ALL: 0 decisions", and a one-row
trace crashed on a 0-d array.
"""
from __future__ import annotations

import importlib
import stat
import sys
import types
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
COLS = ("snap_frame,apply_frame,new_valid,ref_valid,new_period,ref_period,new_mult,ref_mult,"
        "new_src,ref_src,new_period_f,ref_period_f,new_phase,ref_phase,new_accent,ref_accent,"
        "new_strength,ref_strength")
ROW = "10,14,1,1,30,30,1,1,0,0,30.1,30.1,0.2,0.2,5,5,0.5,0.5"
OTHER = "16,20,1,1,30,32,1,1,0,0,30.1,32.0,0.2,0.2,5,5,0.5,0.5"  # a different period


@pytest.fixture
def equiv(tmp_path, monkeypatch):
    corpus = tmp_path / "edm"
    corpus.mkdir()
    for stem in ("a", "b"):
        (corpus / f"{stem}.onsets_consensus.json").write_text("{}")
    fake = types.ModuleType("run_tracker_eval")
    fake.CORPUS = corpus
    fake.DEVICE_LEVEL_DB = -50.0
    fake.decode = lambda *a, **k: None
    fake.find_ffmpeg = lambda explicit: "ffmpeg"
    monkeypatch.setitem(sys.modules, "run_tracker_eval", fake)
    monkeypatch.syspath_prepend(str(HERE))
    sys.modules.pop("acf_equivalence", None)
    mod = importlib.import_module("acf_equivalence")

    def run(*rows: str) -> int:
        harness = tmp_path / "harness.sh"
        body = "\\n".join((COLS,) + rows)
        harness.write_text(
            "#!/bin/sh\n"
            f"for a; do case \"$a\" in --acf-trace=*) printf '{body}\\n' > \"${{a#--acf-trace=}}\";; esac; done\n")
        harness.chmod(harness.stat().st_mode | stat.S_IEXEC)
        monkeypatch.setattr(sys, "argv", ["acf_equivalence.py", "--harness", str(harness)])
        return mod.main()

    return run


def test_identical_decisions_pass(equiv):
    assert equiv(ROW, ROW) == 0


def test_one_decision_per_track_passes(equiv):
    assert equiv(ROW) == 0


def test_a_different_decision_fails(equiv):
    assert equiv(ROW, OTHER) == 1


def test_no_decision_traced_fails(equiv):
    assert equiv() == 1


def test_no_valid_decision_fails(equiv):
    assert equiv(ROW.replace("10,14,1,1", "10,14,0,0", 1)) == 1
