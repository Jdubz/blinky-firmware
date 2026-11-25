#!/usr/bin/env python3
"""The firmware's mel front end against its contract (blinky_time#295, #291).

The contract is tests/parity/spec/mel_frontend_v1.json and the golden vectors it names
(`_golden`: magnitude frames and the mel this firmware computes on them). blinky_time carries an
identical copy (ml-training/spec/) and checks training's front end against the same vectors, so
neither repo reads the other's source. This checks the firmware's side:
  1. the spec is the firmware's: `bands` is MEL_BANDS[] and the constants are
     SharedSpectralAnalysis's, and `version` is MEL_FRONTEND_VERSION, the number a model header
     must record (FrameOnsetNN.h, tools/check_model_input.py);
  2. the golden file is the one the spec names (sha256), shaped as the spec says;
  3. with --harness, the real SharedSpectralAnalysis.cpp (tests/parity/parity_harness) still
     produces the golden mel within the spec's reference_atol.

Usage (from the repo root):
  cmake -S tests/parity -B tests/parity/build && cmake --build tests/parity/build -j
  python tests/parity/test_mel_contract.py --harness tests/parity/build/parity_harness
  python tests/parity/test_mel_contract.py --harness ... --regenerate   # rewrite the vectors

A deliberate front-end change: bump MEL_FRONTEND_VERSION, write mel_frontend_v<N>.json, run
--regenerate, put the printed sha256 in the spec, then copy both files to blinky_time's
ml-training/spec/ in a PR there.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SPEC = HERE / "spec" / "mel_frontend_v1.json"
CPP = REPO / "blinky-things" / "audio" / "SharedSpectralAnalysis.cpp"
HEADER = REPO / "blinky-things" / "audio" / "SharedSpectralAnalysis.h"


def load_spec() -> dict:
    spec = json.loads(SPEC.read_text(encoding="utf-8"))
    assert len(spec["bands"]) == spec["n_mels"], f"{len(spec['bands'])} bands, n_mels {spec['n_mels']}"
    return spec


def golden_path(spec: dict) -> Path:
    return SPEC.parent / spec["_golden"]["file"]


def firmware_bands() -> list[list[int]]:
    body = re.search(r"static const MelBandDef MEL_BANDS\[[^\]]*\]\s*=\s*\{(.*?)\n\};",
                     CPP.read_text(encoding="utf-8"), re.S)
    if body is None:
        raise AssertionError(f"MEL_BANDS[] not found in {CPP}")
    return [[int(a), int(b), int(c)]
            for a, b, c in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", body.group(1))]


def header_constant(name: str) -> float:
    text = HEADER.read_text(encoding="utf-8")
    if name == "NUM_BINS":  # `NUM_BINS = FFT_SIZE / 2`
        assert re.search(r"constexpr int NUM_BINS = FFT_SIZE / 2;", text), "NUM_BINS changed"
        return header_constant("FFT_SIZE") / 2
    m = re.search(rf"constexpr\s+\w+\s+{name}\s*=\s*([0-9.]+)f?\s*;", text)
    if m is None:
        raise AssertionError(f"{name} not found in {HEADER}")
    return float(m.group(1))


def check_spec_matches_firmware(spec: dict) -> None:
    fw = firmware_bands()
    assert fw == spec["bands"], (
        f"spec bands differ from {CPP.name} MEL_BANDS[] at "
        f"{[i for i, (a, b) in enumerate(zip(fw, spec['bands'])) if a != b] or 'length'}")
    for key, name in (("n_mels", "NUM_MEL_BANDS"), ("fmin", "MEL_MIN_FREQ"),
                      ("fmax", "MEL_MAX_FREQ"), ("db_range", "MEL_DB_RANGE"),
                      ("n_fft", "FFT_SIZE"), ("num_bins", "NUM_BINS")):
        assert float(spec[key]) == header_constant(name), f"spec {key} != firmware {name}"
    cpp = CPP.read_text(encoding="utf-8")
    for value, what in ((spec["silence_threshold"], "silenceThreshold"),
                        (spec["log_epsilon"], "epsilon")):
        m = re.search(rf"const float {what} = ([0-9.e+-]+)f;", cpp)
        assert m and float(m.group(1)) == value, f"spec {what} != firmware"
    # The number a model header records and FrameOnsetNN.h compares (#291).
    assert header_constant("MEL_FRONTEND_VERSION") == spec["version"], (
        f"spec version {spec['version']} != firmware MEL_FRONTEND_VERSION "
        f"{header_constant('MEL_FRONTEND_VERSION'):g}: bump both together")
    assert SPEC.name == f"mel_frontend_v{spec['version']}.json", \
        f"{SPEC.name} holds version {spec['version']}"
    print(f"ok  the spec is the firmware's MEL_BANDS[] ({len(fw)} bands), constants and "
          f"MEL_FRONTEND_VERSION {spec['version']}")


def check_golden_file(spec: dict) -> None:
    g = spec["_golden"]
    path = golden_path(spec)
    assert g["file"] == f"mel_golden_v{spec['version']}.npz", f"{g['file']} for version {spec['version']}"
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    assert digest == g["sha256"], f"{path.name} hashes to {digest}, the spec names {g['sha256']}"
    data = np.load(path)
    assert data["mags"].shape == (g["frames"], spec["num_bins"]), data["mags"].shape
    assert data["mel"].shape == (g["frames"], spec["n_mels"]), data["mel"].shape
    print(f"ok  {path.name} is the spec's ({g['frames']} frames, sha256 {digest[:12]})")


def golden_frames(num_bins: int) -> np.ndarray:
    """Deterministic magnitude frames that pin each part of the front end."""
    frames = [np.zeros(num_bins)]
    # 9.5e-7: below the 1e-6 silence floor, yet above where the log map alone reaches 0.
    for level in (5e-7, 9.5e-7, 3e-6, 1e-4, 1e-2, 1.0, 10.0, 100.0):  # silence .. clipping
        frames.append(np.full(num_bins, level))
    eye = np.eye(num_bins)  # one bin at a time: each band's exact support and weights
    frames.extend(eye[b] * 0.5 for b in range(num_bins))
    rng = np.random.default_rng(295)
    frames.extend(10.0 ** rng.uniform(-6, 1, size=(24, num_bins)))
    # A kick-like frame and a noise burst, through the firmware's window and FFT size.
    n = 2 * (num_bins)
    t = np.arange(n) / 16000
    window = 0.54 - 0.46 * np.cos(2 * np.pi * np.arange(n) / (n - 1))
    for sig in (0.8 * np.sin(2 * np.pi * 55 * t) * np.exp(-t / 0.05),
                0.3 * rng.standard_normal(n)):
        frames.append(np.abs(np.fft.rfft(sig * window))[:num_bins])
    return np.asarray(frames, dtype=np.float32)


def run_harness(harness: Path, mags: np.ndarray) -> np.ndarray:
    with tempfile.TemporaryDirectory() as d:
        inp, out = Path(d) / "mags.bin", Path(d) / "out.csv"
        with open(inp, "wb") as f:
            np.array([mags.shape[1], mags.shape[0]], dtype=np.int32).tofile(f)
            mags.astype(np.float32).tofile(f)
        r = subprocess.run([str(harness), str(inp), str(out)], capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"{harness} exited {r.returncode}: {r.stderr.strip()[-500:]}")
        header = out.read_text().splitlines()[0].split(",")
        cols = [i for i, h in enumerate(header) if h.startswith("mel")]
        data = np.loadtxt(out, delimiter=",", skiprows=1, ndmin=2)
    return data[:, cols].astype(np.float32)


def check_firmware_matches_golden(spec: dict, harness: Path) -> None:
    g = np.load(golden_path(spec))
    fw = run_harness(harness, g["mags"])
    err = np.abs(fw - g["mel"])
    tol = spec["_golden"]["reference_atol"]
    worst = np.unravel_index(int(err.argmax()), err.shape)
    assert err.max() <= tol, (
        f"the firmware's mel no longer matches {golden_path(spec).name} (max |diff| "
        f"{err.max():.3g} > {tol:g}, frame {worst[0]}, band {worst[1]}). If that change is "
        f"intended, it is a new front-end version: see this file's docstring.")
    print(f"ok  SharedSpectralAnalysis.cpp still produces {golden_path(spec).name} "
          f"(max |diff| {err.max():.2g})")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--harness", type=Path, help="the tests/parity build's parity_harness")
    ap.add_argument("--regenerate", action="store_true",
                    help="rewrite the golden file from the firmware and print its sha256")
    a = ap.parse_args(argv)
    spec = load_spec()
    check_spec_matches_firmware(spec)
    if a.regenerate:
        if not a.harness:
            ap.error("--regenerate needs --harness: the golden values come from the firmware")
        mags = golden_frames(spec["num_bins"])
        path = golden_path(spec)
        np.savez_compressed(path, mags=mags, mel=run_harness(a.harness, mags))
        print(f"wrote {path} ({len(mags)} frames): set _golden.frames = {len(mags)} and "
              f"_golden.sha256 = {hashlib.sha256(path.read_bytes()).hexdigest()} in {SPEC.name}")
        return 0
    check_golden_file(spec)
    if a.harness:
        check_firmware_matches_golden(spec, a.harness)
    else:
        print("skip the firmware run (no --harness)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
