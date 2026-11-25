#pragma once

#include "AudioControl.h"
#include "../hal/PlatformDetect.h"

// Onset NN build switch. OFF on devices: on the edm/ corpus with the hat's
// CPU cost model (tests/tracker), tuned spectral flux scores F1 0.542 at
// 178 fps; the v33 NN scores 0.480 and costs ~12 ms/inference, holding the
// loop at 48 fps. Visual smoothness comes first, so a model is enabled only
// when it beats flux under that same measurement (cheaper export: #174).
// The host NN harness (BLINKY_HOST_NN) keeps the NN path tested in CI.
#ifndef BLINKY_ONSET_NN_ENABLED
#  ifdef BLINKY_HOST_NN
#    define BLINKY_ONSET_NN_ENABLED 1
#  else
#    define BLINKY_ONSET_NN_ENABLED 0
#  endif
#endif

#if ((defined(BLINKY_PLATFORM_NRF52840) || defined(BLINKY_PLATFORM_ESP32S3)) && !defined(BLINKY_HOST_HARNESS))     || defined(BLINKY_HOST_NN)
// Device builds, and host harness builds linked against a host-compiled
// TFLite Micro (BLINKY_HOST_NN, see tests/tracker/README.md, #203).
#include "FrameOnsetNN.h"
#else
// Host builds without TFLite (default tests/tracker/ harness): the tracker
// runs its NN-unavailable path (spectral-flux pulses, mic-level NN buffer).
struct FrameOnsetNN {
    static constexpr int INPUT_MEL_BANDS = 26;
    bool begin()                                    { return false; }
    bool isReady() const                            { return false; }
    float infer(const float*, const float* = nullptr, float = 0.0f, float = 0.0f) { return 0.0f; }
    void pushFrame(const float*, const float* = nullptr, float = 0.0f, float = 0.0f) {}
    float invoke()                                  { return 0.0f; }
    float getLastOnset() const                       { return 0.0f; }
    void setProfileEnabled(bool)                    {}
    template <typename P> void printDiagnostics(P&) const {}
};
#endif

#include "SharedSpectralAnalysis.h"
#include "OnsetDetector.h"
#include "PhaseClock.h"
#include "SlidingAcf.h"
#include "../inputs/AdaptiveMic.h"
#include "../hal/interfaces/IPdmMic.h"
#include "../hal/interfaces/ISystemTime.h"

// Pattern slot cache (v82): caches PLP patterns for instant section recall
static constexpr int SLOT_COUNT = 4;
static constexpr int SLOT_BINS = 16;

// T1.5 — spectral feature snapshot captured at each onset (either detector).
// Emitted in the transient JSON so offline analysis can cluster TP vs FP
// firings by spectral signature without needing persist_raw signal_frames
// replay.
struct PulseFeatureSnapshot {
    float flatness;
    float rawFlux;
    float centroid;
    float crest;
    float rolloff;
    float hfc;
    float bassRatio;
    float plpPulse;
    float plpConfidence;
};


struct PatternSlot {
    float bins[SLOT_BINS];   // Resampled PLP pattern digest
    float confidence;         // PLP confidence when snapshot taken
    uint16_t totalBars;      // Bars observed with this pattern
    uint8_t age;             // LRU counter (0 = most recent)
    bool valid;              // Is this slot populated?
    bool seeded;             // Template-seeded (one-shot per slot)
};

/**
 * AudioTracker - Audio analysis with decoupled tempo/onset architecture
 *
 * ACF + PLP architecture:
 *   Period path:  spectral flux + bass energy → ACF → beat period → bar multipliers
 *   Onset path:   FrameOnsetNN → pulse detection (visual sparks)
 *   Pattern path:  PLP epoch-folds NN-gated flux at detected period → repeating pattern
 *
 * Key design: Period estimation uses multi-source ACF (bass + flux, NN-independent).
 * Beat-level ACF peaks are multiplied (2×/3×/4×) to find bar-level candidates.
 * Epoch-fold variance scoring selects the period with the best pattern contrast.
 * NN onset drives visual pulse and gates the epoch-fold source for pattern quality.
 */
class AudioTracker {
public:
    AudioTracker(IPdmMic& pdm, ISystemTime& time);
    ~AudioTracker();

    bool begin(uint32_t sampleRate = 16000);
    void end();

    const AudioControl& update(float dt);
    const AudioControl& getControl() const { return control_; }

    // === Accessors for SerialConsole / ConfigStorage ===
    AdaptiveMic& getMicForTuning() { return mic_; }
    const AdaptiveMic& getMic() const { return mic_; }
    SharedSpectralAnalysis& getSpectral() { return spectral_; }
    const SharedSpectralAnalysis& getSpectral() const { return spectral_; }
    const FrameOnsetNN& getFrameOnsetNN() const { return nnDetector_.model(); }
    // The onset detector in use (OnsetDetector.h): "spectral-flux" or "onset-nn".
    const char* getOnsetDetectorName() const { return detector_->name(); }
    // Switch detector at runtime by name ("spectral-flux", or "onset-nn" when
    // built in and ready). False for an unknown or unavailable one.
    bool selectOnsetDetector(const char* name);
    float getCurrentBpm() const { return bpm_; }
    int getPlpPeriod() const { return plpBestPeriod_; }  // ACF period in spectral frames (62.5 Hz), integer
    float getPlpPeriodF() const { return plpPeriodF_; }  // Fractional period (frames) — drives phase advance
    int getHwGain() const;

    // === Debug accessors ===
    float getPeriodicityStrength() const { return periodicityStrength_; }
    float getLastPulseStrength() const { return lastPulseStrength_; }
    // T1.4/T1.5 — per-firing diagnostics, populated only when a pulse fires
    // (pulseStrength > 0). Values persist until next firing.
    uint8_t getLastPulseGateMask() const { return lastPulseGateMask_; }
    const PulseFeatureSnapshot& getLastPulseFeatures() const { return lastPulseFeatures_; }
    uint16_t getBeatCount() const { return beatCount_; }
    float getPlpPhase() const { return plpPhase_; }
    float getPlpConfidence() const { return plpConfidence_; }
    float getPlpAccentPhase() const { return plpAccentPhase_; }
    float getPlpPulseValue() const { return plpPulseValue_; }
    int getPlpPatternLen() const { return plpPatternLen_; }
    float getAcfPeakStrength() const { return acfPeakStrength_; }
    int getPlpBestSource() const { return plpBestSource_; }
    float getPlpNNAgreement() const { return plpNNAgreement_; }
    float getPlpReliability() const { return plpMeanReliability_; }
    float getBeatStability() const { return beatStability_; }
    float getOnsetDensity() const { return onsetDensity_; }
    float getBpmMin() const { return bpmMin; }
    float getBpmMax() const { return bpmMax; }
    uint32_t getLastAcfMs() const { return lastAcfDurationMs_; }
    uint32_t getLastPlpMs() const { return lastPlpMs_; }
    // Tempo analysis cost, mean and max in us over the last closed 5 s window (0 until one
    // closes), like SharedSpectralAnalysis's onsetUs. acf = one decision's ACF work (snapshot,
    // pass 1, syncopation candidates, pass 2, refinement, accent fold), summed over the frames
    // it is spread across; plp = updatePlpAnalysis per decision. A decision is spread over the
    // frames of one acfPeriodMs cycle, so what a single loop iteration carries is the slice:
    // all tempo work in one spectral frame (the incremental ACF update plus whatever analysis
    // steps fall in it). `json info` reports these next to maxFrameMs.
    // Device note: micros() on this core is the FreeRTOS tick (1024 Hz, ~977 us steps) unless
    // the DWT is enabled, so a mean is a dithered estimate and a max is quantized to ~1 ms.
    uint32_t getAcfUs() const { return acfCost_.mean; }
    uint32_t getAcfMaxUs() const { return acfCost_.maxClosed; }
    uint32_t getPlpUs() const { return plpCost_.mean; }
    uint32_t getPlpMaxUs() const { return plpCost_.maxClosed; }
    uint32_t getAcfSliceUs() const { return acfSliceCost_.mean; }
    uint32_t getAcfSliceMaxUs() const { return acfSliceCost_.maxClosed; }
    // Per-stage cost (AcfStage order): mean / max over the last closed 5 s window.
    enum AcfStage { ACF_STAGE_LINEARIZE = 0, ACF_STAGE_PASS1, ACF_STAGE_SYNC, ACF_STAGE_PASS2,
                    ACF_STAGE_REFINE, ACF_STAGE_ACCENT, ACF_STAGE_UPDATE, ACF_STAGE_COUNT };
    uint32_t getAcfStageUs(int stage) const { return acfStageCost_[stage].mean; }
    uint32_t getAcfStageMaxUs(int stage) const { return acfStageCost_[stage].maxClosed; }

#ifdef BLINKY_HOST_HARNESS
    // Test hook (tests/tracker, ACF equivalence): each tempo decision when it is applied, with
    // the analysis snapshot it was computed from (linearized, mean-subtracted sources; valid
    // until the next snapshot), so a test can recompute it with the old batch code.
    struct AcfTrace {
        const float* sources[3];
        int counts[3];
        uint32_t snapFrame;      // frame the snapshot was taken (old code decided here)
        uint32_t applyFrame;     // frame the decision was applied
        bool valid;              // false: no candidate (period and phase kept)
        int period;
        float periodF;
        float accentBin;
        float snapTargetPhase;   // phase target at snapFrame (before the elapsed-frame shift)
        int source;
        int multiplier;
        float strength;
    };
    using AcfTraceHook = void (*)(void* ctx, const AcfTrace& trace);
    void setAcfTraceHook(AcfTraceHook hook, void* ctx) { acfTraceHook_ = hook; acfTraceCtx_ = ctx; }
#endif

    // Alias for JSON audio stream ("onset" field)
    float getLastOnsetStrength() const { return lastPulseStrength_; }

    // Onset activation (0-1, either detector) of the last frame with fresh
    // evidence, before peak picking and gates.
    float getOnsetActivation() const { return onsetActivation_; }

    // Absolute mic-sample index (AdaptiveMic ring counter) at which the most
    // recent spectral frame's window ended. Same timeline as record-mode audio.
    uint32_t getLastFrameEndSampleIdx() const { return lastFrameEndSampleIdx_; }
    // Call once at the end of setup(): drops the audio captured while setup
    // ran, uncounted (AdaptiveMic::discardBacklog, #275). Returns samples dropped.
    uint32_t discardSetupBacklog();

    // Pulse strength fired by the frame most recently processed (0 = none).
    // getLastOnsetStrength() is the max over all frames of the last update().
    float getLastFramePulse() const { return lastFramePulse_; }

    // Per-frame observer: called at the end of every processed spectral
    // frame (loop task, inside update()). Record mode uses it to log every
    // frame — not just the last one of each update().
    using FrameHook = void (*)(void* ctx);
    void setFrameHook(FrameHook hook, void* ctx) { frameHook_ = hook; frameHookCtx_ = ctx; }

    // Pattern slot cache (v82)
    int getActiveSlotId() const { return activeSlot_; }
    const struct PatternSlot& getSlot(int i) const { return slots_[i]; }
    static constexpr int getSlotCount() { return SLOT_COUNT; }
    void resetSlots();

    // === Tunable parameters ===
    // Diagnostic: scale all downstream AudioControl values (energy, pulse,
    // plpPulse, rhythmStrength) by this multiplier before generators see
    // them. Default 1.0 = no change. Set to a large value (e.g. 100) to
    // saturate the audio chain and visually distinguish "wired correctly
    // but subtle" from "not wired at all" — every non-zero input clamps
    // to 1.0, so generators should show fully-saturated response on any
    // audio activity. Set to 0 to verify ambient-only baseline.
    float audioAmp = 1.0f;

    // Core tempo
    float bpmMin = 15.0f;              // Captures full-bar patterns (4 beats at 60 BPM = 264 frames)
    float bpmMax = 200.0f;
    float tempoSmoothing = 0.85f;
    float tempoPriorWidth = 0.8f;      // Beat-lag prior width in octaves around 120 BPM (0 = off).
    float syncopatedCandidateScale = 1.0f;  // Bar candidates derived from 1.5/2.5-beat peaks; score scale (0 = off) (#166)
                                       // tests/tracker sweep: 0.8 lifts valid-tempo time 0.60 -> 0.67
                                       // and phase alignment 0.079 -> 0.103 vs off.
    uint16_t acfPeriodMs = 100;

    // Rhythm activation
    float activationThreshold = 0.3f;

    // PLP (Predominant Local Pulse)
    float plpConfAlpha = 0.25f;        // Confidence EMA smoothing rate (was 0.15)
    float plpNovGain = 1.0f;           // Pattern contrast: power-law exponent (>1 sharpens peaks, 1.0=linear)
    float plpSignalFloor = 0.10f;      // Mic level for full confidence activation
    float plpVarianceSens = 0.0f;      // Epoch-fold variance suppression (0=disabled, sweep showed best autoCorr)
    float plpDecayRate = 0.2f;         // Epoch-fold recency decay (sweep: 0.2 best plp@T+autoCorr)
    float plpPhaseGain = 0.3f;         // Fraction of measured phase error removed per analysis (~every 100 ms)

    // NN profiling
    bool nnProfile = false;

    // Spectral flux contrast (power-law sharpening before ACF)
    float odfContrast = 1.25f;

    // Pulse detection
    // Spectral-flux picker delta (FluxOnsetDetector, SuperFlux ODF units).
    float fluxDelta = FluxOnsetDetector::DELTA_DEFAULT;
    // NN activation threshold (the NN's picker delta). With the online
    // picker (no 96 ms window or cooldown) the old 0.20 fired 10/s; edm/
    // host sweep, drum F at 0 dB: 0.4 0.587 | 0.5 0.632 | 0.55 0.641 |
    // 0.6 0.638 | 0.7 0.607 (-20 dB: 0.5 0.656 | 0.55 0.669 | 0.6 0.671).
    static constexpr float PULSE_ONSET_FLOOR_DEFAULT = 0.55f;
    float pulseOnsetFloor = PULSE_ONSET_FLOOR_DEFAULT;
    // Rhythm outputs need recent onsets (see update()): full for
    // RHYTHM_HOLD_S after the last onset, then fade to 0 over RHYTHM_FADE_S.
    static constexpr float RHYTHM_HOLD_S = 2.0f;
    static constexpr float RHYTHM_FADE_S = 2.0f;

    // === Shared onset gate (AudioTracker::pickOnset) ===
    // Applied to every detector's candidates alike (OnsetDetector.h). The
    // bass-ratio gate and the presence gate (pulseMinLevel) were deleted
    // with the online picker (#280): on edm/ the bass gate cost both
    // detectors recall for no drum F (flux 0.649 -> 0.648 off, -20 dB
    // 0.640 -> 0.647; NN 0.641 -> 0.644), and presence never blocked an
    // onset, in music or in noise (the old flux fired 3-6/s on steady
    // noise at -60..-30 dBFS with it on; SuperFlux fires 0).
    //
    // Rhythm-pattern bias: while the PLP pattern is confident, a candidate
    // at a position the pattern predicts no hit needs up to this much more
    // activation (scaled by confidence and by how low the pattern is there;
    // 0 = off). Wanted: triggers prefer the music's pattern. Tune and
    // improve it where the pattern tracker is wrong or too strict (#340);
    // keep it switchable for A/B tests. Was NN-only.
    static constexpr float ONSET_PATTERN_BIAS_DEFAULT = 0.3f;
    float onsetPatternBias = ONSET_PATTERN_BIAS_DEFAULT;

    // Pulse envelope decay (synthesizeOutputs). Shorter than the gaps between
    // drum hits, so each hit shows as its own flash: kick+snare inter-onset
    // intervals on edm/ are p10 112 / p25 157 / p50 240 ms, and at 100 ms the
    // envelope is down to ~21% by the p25 gap. Host, device level, drum hits
    // that make a visible flash (a jump >= 0.2) by tau: 60 ms 79%, 100 ms
    // 77%, 150 ms 74%, 240 ms 68% (#280).
    // 100 ms read as a glitch on hat_v1 (owner): default 150 ms (74%), and
    // runtime-tunable (`pulsedecay`, ms) to set the look by eye.
    static constexpr float PULSE_DECAY_MS_DEFAULT = 150.0f;
    float pulseDecayMs = PULSE_DECAY_MS_DEFAULT;

    // === Shared peak picker (AudioTracker::pickOnset) ===
    // Online peak picking, Boeck, Krebs & Schedl, "Evaluating the online
    // capabilities of onset detection methods" (ISMIR 2012): frame t is an
    // onset if its ODF is the maximum of the last W1 frames and itself, it
    // reaches (mean of the last W3 frames and itself) + delta, and the last
    // onset was more than W5 frames ago. Their online values are 30 / 100 /
    // 30 ms; at 62.5 fps W1 = 2, W3 = 6, W5 = 2. No look-ahead: the onset is
    // reported at the frame that peaks. W3 and delta are per detector
    // (OnsetDetector::averageFrames / delta). This replaces the 96 ms
    // local-max window, the tempo-adaptive 40-150 ms cooldown, the adaptive
    // flux baseline and the crest / beat-grid / re-arm gates: on edm/ the
    // cooldown blocked 61-71% of the kicks the old picker missed (#280).
    // W1 / W3 / W5 are in spectral frames (62.5 fps) and scale by the
    // detector's evidence rate, so their length in time is the same at any
    // rate (OnsetDetector::evidenceRate; SuperFlux 2 per frame, #178).
    static constexpr int PICKER_WINDOW_MAX_FRAMES = 16;  // settings range for W1 / W3
    static constexpr int ONSET_HISTORY_MAX = PICKER_WINDOW_MAX_FRAMES * ONSET_EVIDENCE_MAX;  // picker steps of ODF history
    static constexpr uint8_t ONSET_MAX_FRAMES_DEFAULT = 2;   // W1
    static constexpr uint8_t ONSET_MIN_GAP_FRAMES_DEFAULT = 2;  // W5
    uint8_t onsetMaxFrames = ONSET_MAX_FRAMES_DEFAULT;
    uint8_t onsetMinGapFrames = ONSET_MIN_GAP_FRAMES_DEFAULT;
    uint8_t fluxAverageFrames = FluxOnsetDetector::AVERAGE_FRAMES_DEFAULT;  // W3, flux

    // ODF peak-hold decay
    float odfPeakHoldDecay = 0.85f;    // Peak-hold release rate (~100ms at 62.5Hz)

    // Energy synthesis blend weights (should sum to ~1.0 for expected behavior;
    // not auto-normalized — setting all low under-drives energy, all high over-drives)
    float energyMicWeight = 0.30f;     // Broadband mic level weight
    float energyMelWeight = 0.30f;     // Bass mel energy weight
    float energyOdfWeight = 0.40f;     // ODF peak-hold transient weight

    // Pattern slot cache (v82): caches PLP patterns for instant section recall
    float slotSwitchThreshold = 0.70f;   // Cosine sim to recall cached slot
    float slotNewThreshold = 0.40f;      // Below this: allocate new slot
    float slotUpdateRate = 0.15f;        // EMA rate for active slot reinforcement
    float slotSaveMinConf = 0.25f;       // Min PLP confidence to save/update slots (was 0.50)
    float slotSeedBlend = 0.70f;         // Blend ratio when seeding from cached slot

private:
    // === Audio input ===
    ISystemTime& time_;
    AdaptiveMic mic_;
    SharedSpectralAnalysis spectral_;
    // Onset detectors (OnsetDetector.h). detector_ is the one in use: the NN
    // when built in and ready, otherwise spectral flux (devices: flux).
    FluxOnsetDetector fluxDetector_;
    NNOnsetDetector<FrameOnsetNN> nnDetector_;
    OnsetDetector* detector_ = &fluxDetector_;
    uint32_t lastSpectralFrameCount_ = 0;  // Track new frames via getFrameCount()

    // === OSS buffer (~12 seconds: 792 samples @ ~66 Hz) ===
    // Sized for 3 epochs at MAX_PATTERN_LEN (264 × 3 = 792)
    static constexpr int OSS_BUFFER_SIZE = 792;
    // THE clock for all rhythm analysis: one OSS sample, one NN inference,
    // one pulse-detector step and one phase step per spectral frame.
    // 16 kHz / 256-sample non-overlapping frames = exactly 62.5 Hz.
    // (Was 66.0 and per-loop-iteration — see docs/AUDIO_ARCHITECTURE.md
    // "Frame clock": phase drifted at loopFps/62.5 and skipped frames were
    // invisible to tempo/NN.)
    static constexpr float OSS_FRAME_RATE =
        SpectralConstants::SAMPLE_RATE / static_cast<float>(SpectralConstants::FFT_SIZE);
    static_assert(OSS_FRAME_RATE > 62.49f && OSS_FRAME_RATE < 62.51f, "frame clock must be 62.5 Hz");
    static constexpr float OSS_FRAMES_PER_MIN = OSS_FRAME_RATE * 60.0f;
    float ossBuffer_[OSS_BUFFER_SIZE] = {0};       // Ungated spectral flux (NN-independent, for ACF period detection)
    float ossLinear_[OSS_BUFFER_SIZE] = {0};        // Linearized ungated flux (for period detection)
    // midFluxBuffer_ / highFluxBuffer_ removed — band selection tested in v82,
    // regressed on 8-10/18 tracks. Broadband (ossBuffer_) is the sole PLP source.
    // Saves 6336 bytes RAM (2 × 792 floats).
    int ossWriteIdx_ = 0;
    int ossCount_ = 0;

    // === Tempo state ===
    float bpm_ = 120.0f;
    float periodicityStrength_ = 0.0f;

    // === PLP state (Predominant Local Pulse) ===
    static constexpr int BASS_BUFFER_SIZE = 792;  // Same size as OSS buffer
    static constexpr int NN_BUFFER_SIZE = 792;    // NN onset activation history
    static constexpr int MAX_PATTERN_LEN = 264;   // 1 bar at 60 BPM (4 beats × 66 frames/beat)
    float bassBuffer_[BASS_BUFFER_SIZE] = {0};
    float bassLinear_[BASS_BUFFER_SIZE] = {0};    // Linearized bass (shared by ACF + PLP)
    int bassWriteIdx_ = 0;
    int bassCount_ = 0;
    // Rhythm source 2. Holds MIC LEVEL per frame in both modes (#155), not the
    // NN activation, despite the nn* names: the NN drives onsets only.
    float nnOnsetBuffer_[NN_BUFFER_SIZE] = {0};   // Mic level per frame (source 2)
    float nnLinear_[NN_BUFFER_SIZE] = {0};        // Linearized source 2 (shared by ACF + PLP)
    int nnWriteIdx_ = 0;
    int nnCount_ = 0;

    float plpPattern_[MAX_PATTERN_LEN] = {0};   // Epoch-folded pattern (one period), ACCENT-ALIGNED: bin 0 = accent
    int plpPatternLen_ = 33;                     // Current pattern length (BPM-dependent)
    float plpPhase_ = 0.0f;                     // 0→1 phase since the accent. Advanced 1/plpPeriodF_ per frame,
                                                // pulled toward the measured accent position at each analysis.
    float plpPeriodF_ = 33.0f;                  // Fractional period (frames) refined from full-resolution ACF
    float plpAccentBin_ = 0.0f;                 // Accent position in the end-aligned fold (bin P-1 = newest frame)
    bool phaseLocked_ = false;                  // First analysis snaps phase; later ones correct gradually
    bool skipNextWrap_ = false;                 // Phase stepped back across 0 — that beat was already counted
    float plpConfidence_ = 0.0f;                // Pattern confidence (ACF × signal × NN agreement)
    float plpNNAgreement_ = 0.5f;               // Cross-correlation between flux fold and source-2 (mic level) fold (0-1)
                                                // Starts at 0.5 (neutral) so PLP confidence isn't penalized during warmup
    float plpMeanReliability_ = 0.0f;           // Mean per-bin reliability (low CV = consistent events)
    float plpPulseValue_ = 0.5f;               // Current pulse value (pattern at phase position)
    float cachedBassEnergy_ = 0.0f;            // Cached bass mel energy (shared by PLP + energy synthesis)
    float acfPeakStrength_ = 0.0f;                   // Normalized ACF peak of winning period (0-1)
    int plpBestPeriod_ = 33;                   // Winning period from ACF + epoch-fold scoring (frames)
    float plpAccentPhase_ = 0.0f;             // Epoch-fold accent position (0-1 within period)
    float plpPeakEma_ = 0.0f;                // EMA of PLP peak amplitudes (beat stability tracking)
    float beatStability_ = 0.0f;              // Current PLP peak / peak EMA (0=disrupted, 1=locked)
    uint8_t plpBestSource_ = 0;                // 0=flux, 1=bass, 2=nn (which source won)
    uint16_t beatCount_ = 0;                    // Beat counter (increments on phase wrap)

    // === Onset picking (shared by every detector, see OnsetDetector.h) ===
    // Outputs never know which detector ran: control_.onset is the strength
    // (0-1 activation at the peak), control_.pulse the envelope of the
    // activation, onset density the count of picked onsets.
    float odfPeakHold_ = 0.0f;        // Peak-hold of spectral flux: energy synthesis + presence gate
    float lastPulseStrength_ = 0.0f;  // Max pulse fired by any frame processed in the last update()
    float lastFramePulse_ = 0.0f;     // Pulse fired by the most recent frame
    FrameHook frameHook_ = nullptr;
    void* frameHookCtx_ = nullptr;
    uint32_t lastPulseFrame_ = 0;     // frameClock_ of the last onset (rhythm presence)
    uint32_t pickerStepsSinceOnset_ = UINT32_MAX / 2;  // picker steps since the last onset (minimum gap W5)
    // T1.4/T1.5 — per-onset diagnostics captured by pickOnset at every onset
    // (either detector). Consumed by the TRANSIENT JSON emitter in
    // blinky-things.ino for offline FP/TP class clustering.
    uint8_t lastPulseGateMask_ = 0;
    PulseFeatureSnapshot lastPulseFeatures_ = {0};

    // ODF of the last fresh frames, newest first ([0] = t-1). The picker's
    // max and moving-average windows read it.
    float odfHistory_[ONSET_HISTORY_MAX] = {0};
    float onsetActivation_ = 0.0f;     // Max activation over the last fresh frame's picker steps (getOnsetActivation)

    // Epoch-fold linearized buffer (raw, not mean-subtracted — for pattern extraction)
    float epochFoldLinear_[OSS_BUFFER_SIZE] = {0};

    // === Incremental ACF (SlidingAcf.h) ===
    // Exact running lag products of the three rhythm sources over the OSS window, updated
    // every frame. The decision reads them instead of recomputing the ACF from scratch.
    // Lags read: pass 1 up to ACF_MAX_LAG + ACF_STEP, syncopation bar lags up to
    // MAX_PATTERN_LEN, refinement up to count/2 + ACF_REFINE_MAX_K/2 + 1 (long-lag window
    // and its parabola neighbour).
    static constexpr int ACF_REFINE_MAX_K = 12;
    static constexpr int ACF_TABLE_MAX_LAG = OSS_BUFFER_SIZE / 2 + ACF_REFINE_MAX_K / 2 + 2;
    static_assert(ACF_TABLE_MAX_LAG >= MAX_PATTERN_LEN + 3, "table must cover the syncopation and bar-refine lags");
    // Pushes the table may be held for: from a snapshot to the refinement that reads it
    // (<= 4 frames, see runAcfJob) plus the catch-up after it.
    static constexpr int ACF_TABLE_SLACK = 16;
    using RhythmAcf = SlidingAcf<OSS_BUFFER_SIZE, ACF_TABLE_MAX_LAG, ACF_TABLE_SLACK>;
    RhythmAcf acfTable_[3];                       // 0 = bass flux (OSS), 1 = bass energy, 2 = mic level
    // Window head/tail sums of one source (RhythmAcf::prefixSums), cached per decision.
    int64_t acfHead_[ACF_TABLE_MAX_LAG + 1] = {};
    int64_t acfTail_[ACF_TABLE_MAX_LAG + 1] = {};
    int acfPrefixSrc_ = -1;
    int acfPrefixMaxLag_ = -1;
    double acfSumOverN2_ = 0.0;

    // === Tempo decision, spread over one acfPeriodMs cycle (runAcfJob) ===
    // Frame 0 snapshots the sources (linearized, mean-subtracted) and the ACF tables, and
    // runs pass 1 + candidate generation; pass 2 scores the candidates across the next
    // frames; then refinement + accent fold decide and apply; then PLP. Every step reads
    // the snapshot, so the decision is the one the old inline code made at frame 0; it is
    // applied a few frames later with the phase target moved on by the frames elapsed.
    struct AcfCandidate {
        float acfStrength;
        int period;
        int source;
        int multiplier;   // 1=beat, 2/3/4=bar-level
        bool derived;     // syncopation-derived (#166): score scaled by syncopatedCandidateScale
    };
    static constexpr int ACF_TOP_N = 5;
    static constexpr int ACF_NUM_MULTIPLIERS = 4;
    static constexpr int ACF_SYNC_FRACS = 2;
    static constexpr int ACF_SYNC_MULTS = 2;
    static constexpr int ACF_MAX_CANDIDATES = ACF_TOP_N * (ACF_NUM_MULTIPLIERS + ACF_SYNC_FRACS * ACF_SYNC_MULTS);
    static constexpr int ACF_MAX_PASS2_GROUPS = 3;
    AcfCandidate acfCands_[ACF_MAX_CANDIDATES] = {};
    int acfNumCands_ = 0;
    int acfNextCand_ = 0;
    // Pass 2 running best
    float acfBestScore_ = -1.0f;
    float acfBestStrength_ = 0.0f;
    float acfBestAccentBin_ = 0.0f;
    int acfBestPeriod_ = 0;
    int acfBestSource_ = 0;
    int acfBestMultiplier_ = 1;
    bool acfBestDerived_ = false;
    // Snapshot
    int acfSnapCount_[3] = {0, 0, 0};
    float acfSnapLevelEnv_ = 0.0f;
    uint32_t acfSnapFrame_ = 0;
    // Job schedule (frame offsets from the snapshot frame; see runAcfJob)
    bool acfJobActive_ = false;
    bool acfJobDecided_ = false;   // refinement done: the tables may move again
    int acfJobOffset_ = 0;
    int acfJobGroups_ = 1;
    int acfJobPeriodFrames_ = 1;
    float acfJobPhaseFrames_ = 0.0f;  // phase time (frames) elapsed since the snapshot frame
    uint32_t acfJobPass2Us_ = 0;
    uint32_t acfJobUs_ = 0;         // this decision's ACF work so far (acfCost_)
#ifdef BLINKY_HOST_HARNESS
    AcfTraceHook acfTraceHook_ = nullptr;
    void* acfTraceCtx_ = nullptr;
#endif
    uint32_t lastFrameEndSampleIdx_ = 0;  // See getLastFrameEndSampleIdx()
    bool haveFrameEnd_ = false;           // lastFrameEndSampleIdx_ is a real frame end (PhaseClock)

    // === Onset density ===
    float onsetDensity_ = 0.0f;
    int onsetCountInWindow_ = 0;
    uint32_t onsetDensityWindowStart_ = 0;

    // === Pulse envelope (driven by the onset activation) ===
    float pulseEnvelope_ = 0.0f;     // Decaying envelope triggered by onset events

    // === Frame clock ===
    uint32_t frameClock_ = 0;         // Spectral frames processed since boot
    int framesThisUpdate_ = 0;        // Frames processed by the last update() (0 = no new audio)
    int framesSinceAcf_ = 0;          // ACF/PLP analysis cadence is counted in frames

    // === Smoothed levels (gating must not sample one instant) ===
    float levelEnv_ = 0.0f;           // ~1 s EMA of mic normalized level (PLP confidence gate)

    // === ACF timing ===
    uint32_t lastAcfDurationMs_ = 0;  // Timing: last runAcf()+updatePlpAnalysis() duration
    uint32_t lastPlpMs_ = 0;       // Timing: last updatePlpAnalysis() duration

    // Mean and max of a recurring cost (us) over windows of COST_WINDOW_MS; the getters read
    // the last closed window, so a read is stable within one.
    static constexpr uint32_t COST_WINDOW_MS = 5000;
    struct CostWindow {
        uint32_t sum = 0, count = 0, max = 0;   // the open window
        uint32_t mean = 0, maxClosed = 0;       // the last closed window
        uint32_t startMs = 0;
        void add(uint32_t us, uint32_t nowMs) {
            sum += us;
            count++;
            if (us > max) max = us;
            if (nowMs - startMs >= COST_WINDOW_MS) {
                mean = sum / count;   // count >= 1: this add just counted
                maxClosed = max;
                sum = count = max = 0;
                startMs = nowMs;
            }
        }
    };
    CostWindow acfCost_;
    CostWindow plpCost_;
    CostWindow acfSliceCost_;
    CostWindow acfStageCost_[ACF_STAGE_COUNT];


    // === Pattern slot cache (v82) ===
    PatternSlot slots_[SLOT_COUNT] = {};
    float currentDigest_[SLOT_BINS] = {0};
    int activeSlot_ = -1;
    uint16_t lastSlotCheckBeat_ = 0;
    float prevBpm_ = 120.0f;          // For BPM shift detection

    // === Scratch buffers (loop-task only) ===
    // The Arduino loop task has a 4 KB stack (core LOOP_STACK_SZ = 256*4
    // words) with FreeRTOS overflow checking on. runAcf/updatePlpAnalysis/
    // checkPatternSlots used to put up to four 1 KB arrays on it at once —
    // a stack overflow there is a crash loop on a sealed device. Members
    // cost static RAM (plenty free) instead of stack. Never used re-entrantly:
    // all callers run sequentially inside update() on the loop task.
    float scratchFold_[MAX_PATTERN_LEN];  // runAcf folds, checkPatternSlots resample
    float scratchRaw_[MAX_PATTERN_LEN];   // updatePlpAnalysis patternRaw
    float scratchVar_[MAX_PATTERN_LEN];   // updatePlpAnalysis perBinVariance
    float scratchNN_[MAX_PATTERN_LEN];    // updatePlpAnalysis NN fold

    // === Output ===
    AudioControl control_;

    // === Internal methods ===
    // Everything that must run exactly once per spectral frame. elapsedFrames:
    // real time the frame covers on the sample clock (1, more after an overrun).
    // mayRunExpensive: this frame may run the detector's expensive work.
    void processFrame(bool mayRunExpensive, float elapsedFrames);
    void addOssSample(float ungatedFlux);
    void addBassSample(float bassEnergy);
    void pushRhythmSample(int src, float x);    // incremental ACF table (deferred apply)
    float acfCentered(int src, int lag);         // mean-subtracted lag sum of the snapshot window
    const float* acfSource(int src) const;       // linearized, mean-subtracted snapshot of a source
    void acfSnapshot(int periodFrames);          // frame 0: linearize, pass 1, candidates
    void acfScoreCandidates(int end);            // pass 2 for candidates [acfNextCand_, end)
    void acfDecide();                            // refinement, accent fold, apply period + phase
    void runAcfJob(bool finishAll);              // the steps due at this frame offset
    bool acfPlpDue() const;                      // PLP runs in this frame's job step
    float pickOnset(const OnsetEvidence& ev, const OnsetDetectorParams& params);  // One frame; returns onset strength (0 = none)
    OnsetDetectorParams detectorParams() const;
    void updatePlpAnalysis();       // Epoch-fold + bass ACF + cross-correlate (ACF cadence)
    void advancePlpPhase(float frames);  // Advance phase by `frames` + read pattern value
    float patternValueAt(float phase) const;  // accent-aligned PLP pattern, interpolated
    void correctPlpPhase(float targetPhase, float gain);
    void countBeat();
    void synthesizeOutputs(float dt, uint32_t nowMs);

    // Pattern slot cache methods
    void checkPatternSlots();
    int allocateSlot();
    static void resamplePattern(const float* src, int srcLen, float* dst, int dstLen);
    static float cosineSimilarity(const float* a, const float* b, int len);

};
