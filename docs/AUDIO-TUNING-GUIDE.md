# Audio Tuning Guide

**Last checked against the code:** 2026-11-23 (SETTINGS_VERSION 100). Defaults below are the factory values in `blinky-things/config/ConfigStorage.cpp`; the tracker's own member defaults are in `blinky-things/audio/AudioTracker.h`. Verify there before tuning.

> The task is **drum-hit detection**: kicks and snares, on the beat or not. Hi-hats, cymbals and non-percussive changes are false positives. Score kick/snare recall and the on-drum share against the drum labels, plus repetition consistency (`repS`, below), on `corpus/edm/` (the only validation corpus). All-onset F1 is a secondary number at most. Don't use beat ground truth. See CLAUDE.md.

This document covers testing and tuning of the audio side of the Blinky audio-reactive LED system, and is the one place the tunable parameters are listed. For how the pipeline works, see `docs/AUDIO_ARCHITECTURE.md`.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Testing Infrastructure](#testing-infrastructure)
3. [All Tunable Parameters](#all-tunable-parameters)
4. [Current Best Settings](#current-best-settings)
5. [Comprehensive Test Plan](#comprehensive-test-plan)
6. [Troubleshooting](#troubleshooting)

---

## Architecture Overview

### Audio Signal Flow

```
PDM Microphone (16kHz, mono)
        |
   AdaptiveMic (fixed gain + window/range normalization; no AGC, no noise gate)
        |
   SharedSpectralAnalysis (FFT-256, 62.5 Hz frames)
   ├── Soft-knee compressor (Giannoulis 2012)
   ├── 50 mel bands (30-8000 Hz) + per-band whitening
   ├── SuperFlux on its own onset STFT (Hann 512, hop 128 = 125 fps, #20;
   │   pre-compressor, 101-band log filterbank, unweighted sum <= 5 kHz): onsets
   └── Band-weighted spectral flux (bass/mid/high): energy synthesis
        |
        ├── [ONSET PATH]  (devices: spectral flux; the onset NN is compiled out)
        │   SuperFlux ODF (audio/OnsetDetector.h)
        │   → shared online picker (Böck 2012): max of 30 ms, ≥ mean of 100 ms + fluxdelta
        │     (× pattern bias), 30 ms minimum gap (AudioTracker::pickOnset)
        │   → activation 0-1 (same scale as the NN)
        │   → onset event, pulse envelope (onset strength, 150 ms decay), onset density
        │   → Energy synthesis (mic level + bass mel + flux peak-hold)
        │
        └── [RHYTHM PATH]  (every 100 ms)
            Multi-source ACF (beat lags 20-80 frames, 3 sources)
            Sources: bass flux, bass mel energy, mic level
            → Bar multipliers (2×/3×/4×) + syncopation-derived candidates
            → Epoch-fold scoring + tempo prior → period, accent, phase
            → Accent-aligned pattern → plpPulse
            → Pattern slot cache (4-slot LRU, instant section recall)
        |
   AudioControl { energy, level, pulse, onset, phase, beatCount, plpPulse, rhythmStrength, onsetDensity }
        |
   Fire / Water / PlasmaGlobe ("lightning") / Audio generators
```

### Key Design Decisions

1. **Hits and rhythm use different signals.** Onset detection fires on every kick and snare, on-beat or not. The rhythm sources are tuned for periodicity and never include the NN (#155).
2. **SuperFlux onsets on devices; the NN is shelved on the nRF52840.** `BLINKY_ONSET_NN_ENABLED=0`. On the drum metrics the NN at best ties SuperFlux, and a tie goes to flux, which also costs far less CPU (no dropped LED frames). The NN path stays in the host NN harness so it keeps compiling and passing CI. Changes are judged on kick/snare recall, the on-drum share and `repS` (#8).
3. **Spectral conditioning.** The soft-knee compressor normalizes gross signal level; mel bands get per-band whitening (per-bin FFT whitening was removed, #182). Hardware gain is fixed (nRF52840: 32).
4. **No noise gate, no silence detection.** This mic is too weak for a fixed gate. Instead, `rhythmStrength` and `plpPulse` fade out 2-4 s after the last onset.
5. **Pattern slot cache** (v82): every 4 beats the current pattern is compared with 4 cached 16-bin digests. Similarity above `slotswitchthresh` (0.70) recalls the cached pattern.

---

## Testing Infrastructure

### Components

| Component | Location | Purpose |
|-----------|----------|---------|
| **Tracker harness** | `tests/tracker/` | Real tracker code on edm/ audio on the host; deterministic, runs in CI. Reports drum metrics, onset F1 with the device scorer, and repetition consistency |
| **Device eval (workstation)** | blinky_time [`tools/device_eval.py`](https://github.com/Jdubz/blinky_time/blob/master/tools/device_eval.py) | A device on this machine's USB: plays edm/ through the speakers, scores its onsets with the server's scorer. Windows or Linux |
| **blinky-server** | blinky_time [`blinky-server/`](https://github.com/Jdubz/blinky_time/blob/master/blinky-server) | Fleet management, on-device test orchestration, scoring (REST API, port 8420) |
| **blinky-serial-mcp** | blinky_time [`blinky-serial-mcp/`](https://github.com/Jdubz/blinky_time/blob/master/blinky-serial-mcp) | MCP tools for Claude (thin HTTP client for blinky-server) |
| **Test audio** | `corpus/edm/` (the blinky-corpus submodule) | 18 tracks, drum and onset labels, `track_manifest.json` |

### Quick Commands

```bash
# Host: real tracker on the corpus (see tests/tracker/README.md for the build line)
python tests/tracker/run_tracker_eval.py --harness build/tracker_harness

# Host: compare two builds, or try other loop rates
python tests/tracker/run_tracker_eval.py --harness build/tracker_harness \
    --compare build/tracker_harness_old --fps 35 60 90

# On device, from a workstation (device on this machine's USB, music from its speakers; blinky_time's tools/)
python tools/device_eval.py --port COM3                       # whole corpus, ~13 min
python tools/device_eval.py --port COM3 --tracks trance-party --commands "set fluxdelta 10"

# On device: validation suite via the server API (bench test devices only)
curl -X POST http://blinkyhost.local:8420/api/test/validate \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["DEVICE_ID"], "track_dir": "/path/to/corpus/edm"}'

# On device: parameter sweep via the server API
curl -X POST http://blinkyhost.local:8420/api/test/param-sweep \
  -H 'Content-Type: application/json' \
  -d '{"device_ids": ["D1"], "param_name": "fluxdelta", "values": [6, 8, 10], "track_dir": "/path/to/corpus/edm"}'

# Via MCP tools (poll results with check_test_result(job_id))
run_test(port: "DEVICE_ID")
run_validation_suite(ports: ["DEVICE_ID_1", "DEVICE_ID_2"])

# Monitor audio levels and transient detections
monitor_audio(port: "DEVICE_ID", duration_ms: 3000)
monitor_transients(port: "DEVICE_ID", duration_ms: 5000)
```

The harness takes optional overrides as positional arguments (`plpPhaseGain`, `tempoPriorWidth`, `syncopatedCandidateScale`, `pulseOnsetFloor`, `fluxDelta`); pass them through `run_tracker_eval.py --harness-args`. Picker options go through `--harness-opt` (`--onset-max-frames`, `--onset-min-gap`, `--flux-avg-frames`, `--flux-lambda`, `--onset-pattern-bias`). The harness starts from the `AudioTracker.h` member defaults; the `ConfigStorage.cpp` factory values use the same constants.

### Repetition consistency (`repS`), the primary metric for #8

`tests/tracker/consistency.py` scores whether repeated audio gets repeated light, with no labels: the reference is the track's own audio. It finds the track's repeat lag L (a bar or a few), keeps only regions whose audio really matches the audio one lag later, and matches detections there one-to-one with detections one lag later (±32 ms).
- `rep`: the share of hits that fire in both repeats. 1 = every hit that fires in one repeat fires in the next.
- `repS`: strength agreement, `1 − Σ|s_a − s_b| / Σ max(s_a, s_b)`, with an unmatched detection paired with 0. 1 = the same hits at the same strengths.
- `coverage`: the share of time that is a repeated region; `n`: detections scored (below 20, `rep` and `repS` are NaN).

`repS` measures the owner's complaint directly ("somewhat random which hits trigger"), so it is #8's primary number. It doesn't replace the drum metrics: a detector that fires on a fixed grid would score well on `repS` and badly on recall. A consistency change ships only if `repS` goes up without lowering kick recall or on-drum share, or raising noise triggers (`tests/tracker/README.md`). CI runs its unit tests (`tests/tracker/test_consistency.py`).

### Testing Methodology

**Start on the host.** The tracker harness is silent, deterministic and fast. Move to devices only for what the host can't model (the real mic and room).

**On devices, seek past intros.** EDM tracks often have 15-30 s intros with no drums. The server uses `track_manifest.json` seek offsets to jump into the track, and drops the first 12 s of readings (`SETTLE_MS`, ACF convergence).

### Test Tracks (18 Total)

| Category | Tracks |
|----------|--------|
| **Trance** | trance-party, trance-infected-vibes, trance-goa-mantra |
| **Techno** | techno-minimal-01, techno-minimal-emotion, techno-deep-ambience, techno-machine-drum, techno-dub-groove |
| **EDM/Trap** | edm-trap-electro, dubstep-edm-halftime |
| **DnB** | dnb-energetic-breakbeat, dnb-liquid-jungle |
| **World/Urban** | afrobeat-feelgood-groove, amapiano-vibez, reggaeton-fuego-lento, garage-uk-2step |
| **Breakbeat** | breakbeat-drive, breakbeat-background |

blinky_time's [`blinky-test-player/src/patterns.ts`](https://github.com/Jdubz/blinky_time/blob/master/blinky-test-player/src/patterns.ts) still defines older synthetic patterns (strong-beats, hat-rejection, etc.). They aren't part of the validation flow.

---

## All Tunable Parameters

Every parameter below is a serial setting: `set <name> <value>`, `get <name>`, `show [category]`, `categories`, `settings` (help), or `json settings [category]`. `save` writes the persisted ones to flash. **Only use `set`/`save` on a bench test device.** Fleet defaults change in code and ship in a release (CLAUDE.md).

"Saved" = stored in flash by `save`. "Runtime" = tunable but lost on reboot.

### Category: `tracker` - Onset detection, tempo, energy

| Command | Default | Range | Saved | Description |
|---------|---------|-------|-------|-------------|
| `fluxdelta` | 8 | 0.5-60 | yes | Flux onset threshold δ: SuperFlux must exceed its 96 ms mean by this (device onset path) |
| `fluxavgframes` | 6 | 0-16 | runtime | Flux picker moving-average frames (W3, 96 ms) |
| `onsetmaxframes` | 2 | 0-16 | runtime | Picker: onset must be the max of this many previous frames (W1, 32 ms) |
| `onsetmingap` | 2 | 0-30 | runtime | Picker: minimum frames between onsets (W5, 32 ms) |
| `onsetpatternbias` | 0.3 | 0.0-2.0 | runtime | Rhythm-pattern bias: max δ boost where the pattern predicts no hit, 0 = off. Wanted; tune it, don't remove it |
| `pulsedecay` | 150 | 30-600 ms | runtime | Pulse envelope decay time constant after each onset |
| `odfpkdecay` | 0.85 | 0.5-0.99 | yes | Flux peak-hold decay per frame (energy synthesis) |
| `pulseonsetfloor` | 0.55 | 0.0-1.0 | yes | NN activation threshold (NN builds only) |
| `bassflux` | 0.5 | 0.0-1.0 | yes | Weighted flux bass weight (62-375 Hz; energy synthesis only) |
| `midflux` | 0.2 | 0.0-1.0 | yes | Weighted flux mid weight (437-2000 Hz; energy synthesis only) |
| `highflux` | 0.3 | 0.0-1.0 | yes | Weighted flux high weight (2-8 kHz; energy synthesis only) |
| `odfcontrast` | 1.25 | 0.1-4.0 | yes | Exponent on bass flux before the rhythm buffer |
| `bpmmin` | 15 | 10-120 | yes | Lowest reported BPM |
| `bpmmax` | 200 | 120-240 | yes | Highest reported BPM |
| `temposmooth` | 0.85 | 0.5-0.99 | yes | BPM EMA smoothing (higher = slower) |
| `plpconfalpha` | 0.25 | 0.01-0.5 | yes | PLP confidence EMA rate |
| `plpnovgain` | 1.0 | 0.1-5.0 | yes | Pattern power-law exponent (>1 sharpens) |
| `plpsigfloor` | 0.10 | 0.01-0.5 | yes | Smoothed mic level for full PLP confidence |
| `plpdecay` | 0.2 | 0.05-1.0 | yes | Epoch-fold recency decay |
| `plpvarsens` | 0.0 | 0.0-50.0 | yes | Not read by the current tracker code |
| `activationthreshold` | 0.3 | 0.0-1.0 | yes | Not read by the current tracker code |
| `emicweight` | 0.30 | 0.0-1.0 | yes | Energy: mic level weight |
| `emelweight` | 0.30 | 0.0-1.0 | yes | Energy: bass mel weight |
| `eodfweight` | 0.40 | 0.0-1.0 | yes | Energy: flux peak-hold weight |
| `audio_amp` | 1.0 | 0-1000 | runtime | Diagnostic: scales every AudioControl signal (100 = saturate, 0 = mute) |
| `nnprofile` | off | bool | runtime | NN per-inference timing output (NN builds only) |

Code-only (no serial command): `acfPeriodMs` 100 (saved), `plpActivation` 0.3 (saved, not read), `plpPhaseGain` 0.3, `tempoPriorWidth` 0.8, `syncopatedCandidateScale` 1.0, SuperFlux λ 32768 (`--flux-lambda` in the harness).

### Category: `slots` - Pattern slot cache

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `slotswitchthresh` | 0.70 | 0.50-0.95 | Cosine similarity to recall a cached slot |
| `slotnewthresh` | 0.40 | 0.20-0.60 | Below this similarity: allocate a new slot |
| `slotupdaterate` | 0.15 | 0.05-0.40 | EMA rate for reinforcing the active slot |
| `slotsaveconf` | 0.25 | 0.20-0.80 | Min PLP confidence to save/update slots |
| `slotseedblend` | 0.70 | 0.30-0.95 | Blend ratio when seeding from a cached slot or template |

### Category: `audio` (2 parameters) - Window/Range Normalization

| Command | Default | Range | Description |
|---------|---------|-------|-------------|
| `peaktau` | 2.0 | 0.5-10.0 s | Peak adaptation speed |
| `releasetau` | 5.0 | 1.0-30.0 s | Peak release speed |

The compressor (-30 dB threshold, 3:1, 15 dB knee, +6 dB makeup, 2 s release) and noise estimation (off) are code constants in `SharedSpectralAnalysis.h`, not serial settings.

### Generator categories

Each generator has its own category. List them on a device with `show <category>` or `json settings <category>`.

| Category | Generator (`gen <name>`) | Settings |
|----------|--------------------------|----------|
| `fire` | `fire` | `basespawnchance`, `audiospawnboost`, `burstsparks`, `windvariation`, `drag`, `sparkvelmin`, `sparkvelmax`, `sparkspread`, `defaultlifespan`, `intensitymin`, `intensitymax`, `organictransmin`, `thermalforce`, `gridcoolrate`, `buoyancycoupling`, `pressurecoupling` (saved); `burstthreshold` (0.45, min onset strength for a burst), `burstvelmult`, `burstlifemult`, `burstsizebase`, `burstsizegain`, `audiobright`, `smolderheight` (0.15), `maxflameheight` (0.50), `audioheightboost` (0.30), `noiseamplitude`, `noisespatialscale`, `noisebasespeed`, `noiseaudiospeedmult`, `maxvel` (400) (runtime) |
| `water` | `water` | `w_spawnchance`, `w_audioboost`, `w_gravity`, `w_windbase`, `w_windvar`, `w_drag`, `w_dropvelmin`, `w_dropvelmax`, `w_dropspread`, `w_splashparts`, `w_splashvelmin`, `w_splashvelmax`, `w_splashint`, `w_maxparts`, `w_lifespan`, `w_intmin`, `w_intmax`, `w_transmin`, `w_bgintensity` (all saved) |
| `plasma` | `lightning` or `plasma` (PlasmaGlobe) | `p_bgdim` (0.01), `p_orbbright` (0.75), `p_orbradius` (0.06), `p_driftspeed` (0.012), `p_pulsedecay` (0.88), `p_pulsebright` (0.5), `p_pulseexpand` (0.3) (all saved) |
| `audiovis` | `audio` | `transientrowfrac`, `transientdecay`, `transientbright`, `levelbright`, `levelsmooth`, `phasebright`, `musicmodethresh`, `beatpulsebright`, `beatpulsedecay`, `beatpulsewidth`, `bgbright` (runtime) |
| `effect` | `effect hue` / `effect none` | `hueshift` (0.0), `huespeed` (0.0 cycles/s) (runtime; `scene` sets them too) |

### One scorer, three runners

The harness, `tools/device_eval.py` and the server's `/api/test/validate` all score all-onset F1 with blinky_time's [`blinky-server/blinky_server/testing/scoring.py`](https://github.com/Jdubz/blinky_time/blob/master/blinky-server/blinky_server/testing/scoring.py) (a secondary number; the drum metrics and `repS` come from `tests/tracker/`):
- **Reference:** `onsets_consensus` onsets with strength ≥ 0.6, deduplicated at 70 ms.
- **Latency:** estimated and removed per run.
- **Matching:** greedy, at ±100 ms (`f1`), with `f1_50ms` and `f1_70ms` reported alongside.

Their numbers are directly comparable. The harness's `recall` column is a separate, older metric: strong onsets (≥ 0.8) within ±50 ms.

**Segments, not whole tracks.** On a device, each track plays from its `track_manifest.json` `seekOffset` for 35 s, and its ground truth is cut to that segment (`segment_ground_truth`). Before 2026-10-21 the server scored those runs against the track's first 35 s. 12 of the 18 tracks have a non-zero seek, so **on-device F1 from blinkyhost before then is chance-level on those tracks; don't compare against it.**

**Timing in `device_eval.py`.** Each segment is preceded by three sync clicks at irregular spacing. The device's onsets on the clicks measure that run's lag: playback start, speaker and detection delay together. Two cheaper approaches fail on the bench:
- **The ffplay process start as audio start:** its delay depended on the seek position (165 ms vs 390 ms).
- **Estimating the lag from the music:** dense drums alias by a grid step.

The click-measured lag is the latency correction (the scorer's `latency_ms`). The scorer's own nearest-onset estimate is printed as `est` but not applied: on the b216 run it was off by up to 89 ms, where a fine lag scan agreed with the clicks. A run whose clicks weren't detected is reported and left out of the means.

**b216 baseline on hat_v1** (`device_eval.py`, 18 tracks × 35 s, stream off, ±100 ms): **F1 0.653, P 0.581, R 0.800**, F1@50 0.466. Weakest: edm-trap-electro 0.45, trance-infected-vibes 0.45 (both precision ≈ 0.3), techno-machine-drum 0.52.

---

## Current Best Settings

### Onset Detection (2026-10)

**SuperFlux on the onset STFT + the online picker, NN off on devices (#280, #20).** Drum metrics on edm/: kick + snare labels aligned per track, ±50 ms, host harness. Snare labels are provisional (#284). See `docs/AUDIO_ARCHITECTURE.md` "Onset Detection" for the table.

**Test at the device's level.** hat_v1 with music peaks at about 0.0028 FS, roughly −50 dB below the mastered corpus. Evaluate with the corpus scaled to that level and re-quantized to int16, not at file level. λ = 100, chosen from a −20 to +10 dB test, produced no onsets at all on the hat.

`fluxdelta` = 8 was chosen for the onset STFT at device level (−50 dB, 35/60/90 fps) as the largest δ that keeps kick recall at the previous baseline (0.789): δ 8 gives kick R 0.799, snare R 0.629, drum F 0.640, lone hats 25 %, `repS` 0.715; δ 7 gives 0.830 / 0.684 / 0.652 / 32 % / 0.717. Higher δ trades recall for on-drum share. The full sweep is in `OnsetDetector.h`.

### Rhythm

Factory defaults above. The CI corpus gate (`tracker-harness` job in `.github/workflows/pr-validation.yml`) holds `lock`, tempo and `recall` floors just under the current baseline; raise them when the tracker improves.

### Known Limitations

- **Bar-length periods phase-lock worse than beat-length ones.** Syncopation-derived candidates (#166) trade a little tempo accuracy for phase lock on purpose.
- **Breakdowns without percussion:** rhythm outputs fade (by design, "rhythm needs hits").
- **Weak mic:** music can sit near the mic's noise floor; that's why there is no gate.
- Open issues: this repo's GitHub issues (#8 is the consistency umbrella; #20, #26).

---

## Comprehensive Test Plan

### Overview

**Goal:** check a tracker or onset change on edm/, first on the host, then on a bench device.

### Phase 1: Host baseline

1. Build the harness for the old and new code (`tests/tracker/README.md`).
2. Run `run_tracker_eval.py --harness <new> --compare <old>` on the whole corpus.
3. Check kick/snare recall, on-drum share, `repS`, `lock`, `worst20s` and `bpm` per track, at several `--fps` values. The numbers should barely move with fps.
4. For onset changes, run with the CPU cost model too (`--harness-args --nn-cost-us=... --frame-cost-us=3200 --render-cost-us=4500`) so frame-rate cost shows up.

### Phase 2: Sweep (if needed)

- **Host:** pass overrides through `--harness-args` (positional, `-` keeps the default).
- **Device:** `POST /api/test/param-sweep` on a bench test device. One device at a time per room: all devices hear the same speaker.

**Always validate combined defaults after independent sweeps.** Interaction effects are real.

### Phase 3: Visual check

Watch the fire generator on a bench device with music:
- one burst per kick or snare, the same hits firing each time a bar repeats, hi-hat-dense passages not overly busy,
- breathing (`plpPulse`) in time,
- no flicker or jarring jumps.

### Phase 4: Ship

1. Change the default in `config/ConfigStorage.cpp` (and the member default in `audio/AudioTracker.h`, so the harness matches).
2. If a stored struct changed, bump `SETTINGS_VERSION` and its `static_assert`.
3. Merge, then cut and gate a release (`docs/RELEASE_PROCESS.md`). Don't `set`/`save` on fleet devices.

---

## Troubleshooting

### No Audio Detection

1. `stream on` (or `monitor_audio`) and check the level moves with sound.
2. `show spectral` for the spectral state; `show beat` for pulse and onset density.
3. `show nn` should say the NN is disabled by build on devices; that's expected.
4. `show errors` for the assert-fail count.

### Too Many False Positives (pulse/spark effects)

1. Raise `fluxdelta` on a bench device and re-run the edm/ validation.

### Missing Transients

1. Lower `fluxdelta`.
2. Check the mic level isn't near zero (weak mic or blocked enclosure holes).

### Rhythm Not Locking

1. Verify the audio has percussion: rhythm outputs fade 2-4 s after the last onset.
2. Allow ~10 s: the ACF needs 1.6 s of warm-up and several epochs for a good fold.
3. Check `show beat` (BPM, periodicity, PLP confidence, source).

### BPM Detected at Half- or Double-Time

Expected and often fine: the tracker picks the period with the best pattern, not the "true" BPM. For DnB (170+ BPM), half-time is normal.

### Serial Commands Reference

```bash
show beat          # Tracker state (BPM, phase, periodicity, PLP, pulse, onset density)
show nn            # NN status (disabled by build on devices)
show spectral      # Spectral analysis state
json beat          # Tracker state as JSON (alias: json rhythm)
json settings      # All settings as JSON
stream on          # Start audio streaming (~20 Hz)
stream fast        # Fast audio streaming (~100 Hz)
stream off         # Stop streaming
restore_runtime_settings  # Factory settings (old name: defaults)
```

**MCP Tools:**
- `get_music_status` — rhythm/music state
- `monitor_audio` — audio levels over a duration
- `monitor_transients` — transient detection stats
- `run_test` / `run_validation_suite` — track evaluation via blinky-server (async; poll `check_test_result`)
- `get_settings` / `set_setting` / `reset_defaults` — settings (bench devices)
