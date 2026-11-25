# Parity harness — firmware vs Python feature math

This directory hosts a native C++ test that links the real firmware
`SharedSpectralAnalysis.cpp` and invokes its shape-feature math on an
externally supplied magnitude spectrum. A Python script computes the same
features on the same magnitudes via `ml-training/analysis/features.py` and
asserts per-frame MAE is below a tight threshold.

Why not re-implement features in Python only? Because any drift between
firmware and Python is then invisible. The harness makes the firmware C++
the authoritative computation and forces the Python reference to track it.

## What's tested

- The firmware's pre-compressor feature chain (`runRawFeaturesForTest()`,
  which includes `computeShapeFeaturesRaw()`) on injected magnitudes:
  centroid, crest, rolloff, HFC, flatness and raw flux. Each must match
  Python within a tight MAE tolerance.
- The harness also writes the mel bands. `test_mel_contract.py` checks them
  against the mel front-end contract (below); `run_parity_test.py` doesn't.

## Mel front-end contract

`spec/mel_frontend_v1.json` specifies the onset NN's mel front end (the
`MEL_BANDS[]` table, constants, arithmetic, and `version` =
`MEL_FRONTEND_VERSION`), and `spec/mel_golden_v1.npz` holds golden vectors:
magnitude frames and the mel this firmware computes on them, named in the
spec's `_golden` with their sha256 and tolerances. blinky_time carries
identical copies (`ml-training/spec/`) and checks training's front end against
the same vectors, so neither repo reads the other's source.

`test_mel_contract.py --harness build/parity_harness` (CI: the `Mel Front End
Contract` job) checks that the spec is this firmware's table, constants and
version, that the golden file is the one the spec names, and that the real
`SharedSpectralAnalysis.cpp` still produces the golden mel. A deliberate
change is a new version: bump `MEL_FRONTEND_VERSION`, write
`mel_frontend_v<N>.json`, run with `--regenerate`, put the printed sha256 in
the spec, then copy both files to blinky_time in a PR there.

Not yet tested (open follow-ups):
- FFT (firmware uses CMSIS-DSP / arduinoFFT; Python uses numpy). Feeding
  the same magnitudes to both code paths bypasses this difference —
  acceptable because the FFT is well-trodden infrastructure, not the part
  we wrote.
- Compressor / whitening on compressed mags.
  Add in future rounds if those features enter the shortlist.

## CI

The `Mel Front End Contract` job builds the harness and runs
`test_mel_contract.py` on every PR. The shape-feature comparison
(`run_parity_test.py`, in blinky_time's `ml-training/`) is not run in CI: run
it locally when you change the feature math on either side.

## Build

```bash
mkdir -p tests/parity/build
cd tests/parity/build
cmake ..
cmake --build . -j
```

Produces `parity_harness`.

## Run via Python wrapper

```bash
cd ml-training
./venv/bin/python -m analysis.run_parity_test \
    --audio ../corpus/edm/breakbeat-drive.mp3 \
    --harness ../tests/parity/build/parity_harness
```

Builds the harness if missing, computes Python mags + features, feeds mags
to the harness, reads harness feature output, compares, prints MAE per
feature. Non-zero exit on MAE above threshold.
