#pragma once
// Reference copy of the tempo decision as AudioTracker::runAcf computed it
// before the incremental ACF (ACF phase 1): the batch ACF over
// the mean-subtracted snapshot buffers, verbatim except that it takes the
// snapshot as arguments and returns the decision instead of applying it.
// Test-only (tests/tracker, --acf-trace): the firmware's spread, table-driven
// decision is compared against it on the same snapshots. Remove it when a
// change to the tempo maths (phase 2) makes the two differ on purpose.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>

struct AcfRefParams {
    float plpDecayRate;
    float syncopatedCandidateScale;
    float tempoPriorWidth;
};

struct AcfRefDecision {
    bool valid;
    int period;
    float periodF;
    float accentBin;
    float targetPhase;  // phase at the snapshot frame
    int source;
    int multiplier;
    float strength;
};

inline AcfRefDecision acfReferenceDecision(const float* const srcIn[3], const int countsIn[3],
                                           const AcfRefParams& prm) {
    using std::min;
    static constexpr int MAX_PATTERN_LEN = 264;
    static constexpr float OSS_FRAMES_PER_MIN = 62.5f * 60.0f;
    auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); };
    const float plpDecayRate = prm.plpDecayRate;
    const float syncopatedCandidateScale = prm.syncopatedCandidateScale;
    const float tempoPriorWidth = prm.tempoPriorWidth;
    static float scratchFold[MAX_PATTERN_LEN];
    const float* sources[3] = { srcIn[0], srcIn[1], srcIn[2] };
    const int sourceCounts[3] = { countsIn[0], countsIn[1], countsIn[2] };
    const int ossCount_ = sourceCounts[0];
    const float* ossLinear_ = sources[0];
    AcfRefDecision out = {};

    // --- Pass 1: Multi-source ACF at beat-level lags ---
    // Scan lags 20-80 (200-49 BPM beat rate) with step=2.
    // Bar-level candidates are generated from multiples, not scanned directly.
    // Cost: ~31 lags × 3 sources × ~700 overlap ≈ 65K MACs (~4ms on M4F).
    // Parabolic interpolation on peaks recovers sub-step resolution (odd lags).
    static constexpr int TOP_N = 5;
    static constexpr int ACF_MIN_LAG = 20;    // ~200 BPM beat rate
    static constexpr int ACF_MAX_LAG = 80;    // ~49 BPM beat rate
    static constexpr int ACF_STEP = 2;

    struct AcfPeak {
        float strength;   // Normalized ACF value at peak (0-1)
        int period;
        int source;
    };
    AcfPeak beatPeaks[TOP_N] = {};

    for (int src = 0; src < 3; src++) {
        int count = sourceCounts[src];
        if (count < ACF_MAX_LAG * 2) continue;
        const float* buf = sources[src];

        // Zero-lag energy for normalization (unbiased ACF)
        float zeroLag = 0.0f;
        for (int i = 0; i < count; i++) zeroLag += buf[i] * buf[i];
        if (zeroLag < 1e-10f) continue;

        // Scan ACF with local-maximum peak detection.
        // Extend one step past ACF_MAX_LAG so a peak AT the boundary is detected
        // (the 3-point detector looks one step back).
        int scanEnd = min(ACF_MAX_LAG + ACF_STEP, count / 2 - 1);
        float prevAcf = 0.0f, prevPrevAcf = 0.0f;
        for (int lag = ACF_MIN_LAG; lag <= scanEnd; lag += ACF_STEP) {
            float sum = 0.0f;
            int overlap = count - lag;
            for (int i = 0; i < overlap; i++) {
                sum += buf[i] * buf[i + lag];
            }
            float acfVal = sum / zeroLag;

            // Peak: previous sample was a local maximum above threshold
            // Only record if the peak lag is within the valid scan range
            if (prevAcf > prevPrevAcf && prevAcf > acfVal && prevAcf > 0.05f
                && (lag - ACF_STEP) >= ACF_MIN_LAG && (lag - ACF_STEP) <= ACF_MAX_LAG) {
                int peakLag = lag - ACF_STEP;
                float peakStrength = prevAcf;

                // Parabolic interpolation: refine peak position and strength
                // from 3 equidistant ACF samples (step=2 → recovers odd lags)
                float denom = prevPrevAcf - 2.0f * prevAcf + acfVal;
                if (fabsf(denom) > 1e-10f) {
                    float delta = 0.5f * (prevPrevAcf - acfVal) / denom;
                    if (delta > -1.0f && delta < 1.0f) {
                        int refined = peakLag + static_cast<int>(roundf(delta * ACF_STEP));
                        if (refined >= ACF_MIN_LAG && refined <= ACF_MAX_LAG) {
                            peakLag = refined;
                        }
                        peakStrength = prevAcf - 0.25f * (prevPrevAcf - acfVal) * delta;
                    }
                }

                // Insert into top-N with 10% minimum period separation
                bool tooClose = false;
                for (int k = 0; k < TOP_N; k++) {
                    if (beatPeaks[k].strength > 0.01f &&
                        abs(peakLag - beatPeaks[k].period) <= beatPeaks[k].period / 10) {
                        if (peakStrength > beatPeaks[k].strength) {
                            beatPeaks[k] = { peakStrength, peakLag, src };
                        }
                        tooClose = true;
                        break;
                    }
                }
                if (!tooClose) {
                    int minIdx = 0;
                    for (int k = 1; k < TOP_N; k++) {
                        if (beatPeaks[k].strength < beatPeaks[minIdx].strength) minIdx = k;
                    }
                    if (peakStrength > beatPeaks[minIdx].strength) {
                        beatPeaks[minIdx] = { peakStrength, peakLag, src };
                    }
                }
            }
            prevPrevAcf = prevAcf;
            prevAcf = acfVal;
        }
    }

    // --- Generate bar-level candidates from beat-peak multiples ---
    // Bar patterns are 2×/3×/4× the beat period. Epoch-fold variance scoring
    // selects bar-length periods over beat-length when the pattern is structured
    // (e.g., kick-snare-kick-snare has higher variance at 4× than at 1×).
    static constexpr int MULTIPLIERS[] = { 1, 2, 3, 4 };
    static constexpr int NUM_MULTIPLIERS = 4;
    // Syncopation-derived candidates (#166): per beat peak, 2 beat hypotheses
    // (L*2/3, L*2/5) x 2 bar multipliers (2x, 4x).
    static constexpr int SYNC_FRACS = 2;
    static constexpr int SYNC_MULTS = 2;
    static constexpr int MAX_CANDIDATES = TOP_N * (NUM_MULTIPLIERS + SYNC_FRACS * SYNC_MULTS);

    struct ScoredCandidate {
        float acfStrength;
        int period;
        int source;
        int multiplier;   // 1=beat, 2/3/4=bar-level
        bool derived;     // syncopation-derived (#166): score scaled by syncopatedCandidateScale
    };
    static_assert(MAX_CANDIDATES == TOP_N * (NUM_MULTIPLIERS + SYNC_FRACS * SYNC_MULTS),
                  "MAX_CANDIDATES invariant");
    ScoredCandidate candidates[MAX_CANDIDATES] = {};
    int numCandidates = 0;

    for (int k = 0; k < TOP_N; k++) {
        if (beatPeaks[k].strength < 0.01f) continue;
        for (int m = 0; m < NUM_MULTIPLIERS; m++) {
            int cPeriod = beatPeaks[k].period * MULTIPLIERS[m];
            if (cPeriod > MAX_PATTERN_LEN) continue;
            if (cPeriod >= ossCount_ / 2) continue;  // Need 2+ epochs

            // Skip near-duplicates
            bool dup = false;
            for (int c = 0; c < numCandidates; c++) {
                if (abs(cPeriod - candidates[c].period) <= cPeriod / 10) { dup = true; break; }
            }
            if (dup) continue;

            if (numCandidates < MAX_CANDIDATES) {
                candidates[numCandidates++] = { beatPeaks[k].strength, cPeriod, beatPeaks[k].source, MULTIPLIERS[m], false };
            }
        }
    }

    // --- Syncopation-derived bar candidates (#166) ---
    // Breakbeat / DnB / 2-step accent patterns (kick on 1 and the "and" of 3,
    // snare on 2 and 4) put the strongest beat-range ACF peaks at 1.5 and 2.5
    // beats, while the true beat lag barely shows up in any source. The bar
    // (4 beats) still correlates strongly. So from each beat peak L derive the
    // beat hypotheses b = 2L/3 (L was 1.5 beats) and 2L/5 (L was 2.5 beats), and
    // add bar-level candidates at 2b and 4b scored with the ACTUAL ACF at that
    // lag, not the parent peak's. Wrong hypotheses get a weak ACF and lose in
    // Pass 2 on their own; nothing is forced. Cost: <= TOP_N*4 single-lag ACFs.
    if (syncopatedCandidateScale > 0.0f) {
        static constexpr int FRAC_NUM[SYNC_FRACS] = { 2, 2 };
        static constexpr int FRAC_DEN[SYNC_FRACS] = { 3, 5 };
        static constexpr int SYNC_MULT[SYNC_MULTS] = { 2, 4 };
        for (int k = 0; k < TOP_N; k++) {
            if (beatPeaks[k].strength < 0.01f) continue;
            const int src = beatPeaks[k].source;
            const float* buf = sources[src];
            const int count = sourceCounts[src];
            float zero = 0.0f;
            for (int i = 0; i < count; i++) zero += buf[i] * buf[i];
            if (zero < 1e-10f) continue;
            const float zeroMean = zero / static_cast<float>(count);
            for (int f = 0; f < SYNC_FRACS; f++) {
                // b is rounded BEFORE the bar multiplier, so 4-beat candidates
                // sit on multiples of 4 frames and can miss the exact bar peak
                // by up to 2 frames (L=56, 2/5 -> 88, true 90). Rounding once
                // after both factors was measured on edm/: tempo 0.729 ->
                // 0.735 but phase lock (excess) 0.094 -> 0.091 (more tracks
                // lock to bar cycles, which phase-lock worse). Phase lock drives
                // the visuals, so the coarser rounding stays until bar-level
                // phase alignment itself improves.
                const int b = (beatPeaks[k].period * FRAC_NUM[f] + FRAC_DEN[f] / 2) / FRAC_DEN[f];
                if (b < ACF_MIN_LAG) continue;
                for (int m = 0; m < SYNC_MULTS; m++) {
                    const int cPeriod = b * SYNC_MULT[m];
                    if (cPeriod > MAX_PATTERN_LEN || cPeriod >= count / 2) continue;
                    // Dedupe within this source only: the same period from a
                    // weak source (e.g. a flat bass ACF) must not hide the
                    // strong-source version, which is the whole point here.
                    //
                    // Same-source ordinary candidates DO suppress a derived one
                    // at the same period, deliberately. Letting derived bar
                    // candidates bypass them was measured on edm/: tempo
                    // 0.729 -> 0.748 but phase lock (excess) 0.094 -> 0.087,
                    // because more tracks then lock to bar-length cycles.
                    // Phase lock is what the visuals animate on, so it wins.
                    bool dup = false;
                    for (int c = 0; c < numCandidates; c++) {
                        if (candidates[c].source == src &&
                            abs(cPeriod - candidates[c].period) <= cPeriod / 10) { dup = true; break; }
                    }
                    if (dup) continue;
                    // Unbiased normalized ACF at the bar lag.
                    const int overlap = count - cPeriod;
                    float sum = 0.0f;
                    for (int i = 0; i < overlap; i++) sum += buf[i] * buf[i + cPeriod];
                    const float acf = sum / static_cast<float>(overlap) / zeroMean;
                    if (acf < 0.05f) continue;
                    if (numCandidates < MAX_CANDIDATES) {
                        candidates[numCandidates++] = { clampf(acf, 0.0f, 1.0f), cPeriod, src, SYNC_MULT[m], true };
                    }
                }
            }
        }
    }

    // --- Pass 2: Score candidates by epoch-fold pattern quality ---
    // Reuses tempogram Pass 2 logic: the period that produces the highest-contrast
    // pattern is the one that creates the best visual pulse. Each candidate is
    // scored using its own source buffer for fair cross-source comparison.
    float bestScore = -1.0f;
    float bestStrength = 0.0f;
    int bestPeriod = static_cast<int>(OSS_FRAMES_PER_MIN / 120.0f);
    float bestAccentBin = 0.0f;
    int bestSource = 0;
    int bestMultiplier = 1;
    bool bestDerived = false;

    for (int c = 0; c < numCandidates; c++) {
        int cPeriod = candidates[c].period;
        int cSource = candidates[c].source;
        const float* cBuf = sources[cSource];
        int cCount = sourceCounts[cSource];

        if (cCount < cPeriod * 2) continue;

        // Recency-weighted epoch fold
        float* fold = scratchFold;
        memset(fold, 0, cPeriod * sizeof(float));
        float totalWeight = 0.0f;
        int epochs = 0;
        for (int offset = cCount - cPeriod; offset >= 0; offset -= cPeriod) {
            float weight = expf(-plpDecayRate * epochs);  // ~3-epoch half-life
            for (int j = 0; j < cPeriod; j++) {
                fold[j] += cBuf[offset + j] * weight;
            }
            totalWeight += weight;
            epochs++;
        }
        if (epochs < 2) continue;
        for (int j = 0; j < cPeriod; j++) fold[j] /= totalWeight;

        // Pattern variance (contrast) — higher = more structured
        float mean = 0.0f;
        for (int j = 0; j < cPeriod; j++) mean += fold[j];
        mean /= cPeriod;
        float variance = 0.0f;
        for (int j = 0; j < cPeriod; j++) {
            float d = fold[j] - mean;
            variance += d * d;
        }
        variance /= cPeriod;

        // Combined score: ACF periodicity × pattern contrast.
        // Multiplier penalty: higher multipliers (2×/3×/4×) must produce
        // proportionally more variance to beat the base period. Without this,
        // 4× bar periods always win because they capture more structure.
        // Penalty: divide by sqrt(multiplier) — 2× needs 1.41× more contrast,
        // 4× needs 2× more contrast to win over the beat-level period.
        float multiplierPenalty = sqrtf(static_cast<float>(candidates[c].multiplier));
        float score = candidates[c].acfStrength * sqrtf(variance) / multiplierPenalty;
        if (candidates[c].derived) score *= syncopatedCandidateScale;

        // Tempo prior on the BEAT lag (Ellis 2007-style log-Gaussian around
        // 120 BPM). ACF peaks at 1.5x / 2x the true beat lag are often as
        // strong as the beat itself on syncopated material; multiplied up to
        // bar length they produced 6-8-beat "cycles". The prior breaks those
        // ties toward musically common tempi without forbidding any.
        if (tempoPriorWidth > 0.0f) {
            float beatLag = static_cast<float>(cPeriod) / static_cast<float>(candidates[c].multiplier);
            float oct = log2f(beatLag / (OSS_FRAMES_PER_MIN / 120.0f)) / tempoPriorWidth;
            score *= expf(-0.5f * oct * oct);
        }

        if (score > bestScore) {
            // Phase from epoch-fold peak position (where the main accent falls).
            // Parabolic interpolation on the 3 bins around the peak gives sub-frame
            // precision (~0.3% vs ~3% integer-bin at 120 BPM). Same technique used
            // in the ACF peak finder. Handles wrap-around for peaks at bin 0.
            int peakBin = 0;
            float peakVal = fold[0];
            for (int j = 1; j < cPeriod; j++) {
                if (fold[j] > peakVal) { peakVal = fold[j]; peakBin = j; }
            }
            // Parabolic interpolation for sub-frame phase
            float refinedBin = static_cast<float>(peakBin);
            if (cPeriod >= 3) {
                int prev = (peakBin - 1 + cPeriod) % cPeriod;  // wrap-around
                int next = (peakBin + 1) % cPeriod;
                float denom = fold[prev] - 2.0f * fold[peakBin] + fold[next];
                if (fabsf(denom) > 1e-6f) {
                    float delta = 0.5f * (fold[prev] - fold[next]) / denom;
                    delta = clampf(delta, -0.5f, 0.5f);
                    refinedBin += delta;
                    if (refinedBin < 0.0f) refinedBin += cPeriod;
                }
            }

            bestScore = score;
            bestStrength = candidates[c].acfStrength;
            bestPeriod = cPeriod;
            bestAccentBin = refinedBin;
            bestSource = cSource;
            bestMultiplier = candidates[c].multiplier;
            bestDerived = candidates[c].derived;
        }
    }

    // (acfPeakStrength_ / periodicityStrength_ updates omitted: outputs only)
    out.strength = bestStrength;
    if (bestScore < 0.0f) { out.valid = false; return out; }

    // --- Fractional period ---
    // Candidate periods are integer frames (the fold needs integer bins), but
    // a real tempo almost never is: 124 BPM = 30.24 frames. Advancing phase
    // by 1/30 would gain ~1% per beat and rely on corrections to claw it
    // back. Refine the beat-level lag at FULL resolution (the scan used
    // step 2) with parabolic interpolation on the winning source, then
    // scale by the multiplier.
    //
    // Syncopation-derived winners (#166) are refined at their BAR lag instead
    // and not rescaled: they exist precisely because the beat-lag ACF peak is
    // weak or absent, so refining at bestPeriod / bestMultiplier would chase
    // an unrelated peak (or keep the rounded integer) and leave period error
    // in the phase advance.
    float periodF = static_cast<float>(bestPeriod);
    {
        const float* buf = sources[bestSource];
        const int count = sourceCounts[bestSource];
        const int refineMultiplier = bestDerived ? 1 : bestMultiplier;
        int lag = bestPeriod / refineMultiplier;
        // UNBIASED estimator (divide by overlap). The plain sum shrinks
        // linearly with lag, which tilts the peak toward shorter lags —
        // measured ~0.7% short (29.07 vs 29.30 frames at 128 BPM), a steady
        // phase drift the corrections had to keep fighting.
        auto acfAt = [&](int L) -> float {
            float sum = 0.0f;
            int overlap = count - L;
            for (int i = 0; i < overlap; i++) sum += buf[i] * buf[i + L];
            return (overlap > 0) ? sum / static_cast<float>(overlap) : 0.0f;
        };
        if (lag > 2 && lag + 1 < count / 2) {
            float a = acfAt(lag - 1), b = acfAt(lag), c = acfAt(lag + 1);
            // Hill-climb up to 2 steps if the step-2 scan landed beside the peak.
            for (int step = 0; step < 2; step++) {
                if (a > b && lag - 2 > 2) { c = b; b = a; lag--; a = acfAt(lag - 1); }
                else if (c > b && lag + 2 < count / 2) { a = b; b = c; lag++; c = acfAt(lag + 1); }
                else break;
            }
            float frac = 0.0f;
            float denom = a - 2.0f * b + c;
            if (b >= a && b >= c && denom < -1e-12f) {
                frac = clampf(0.5f * (a - c) / denom, -0.5f, 0.5f);
            }
            float lagF = static_cast<float>(lag) + frac;

            // Long-lag refinement. Onsets are quantized to whole frames, so
            // the ACF around one period is lopsided (e.g. 0 / .7 / .3 at lags
            // 28/29/30 for a true 29.3) and the parabola lands ~0.15 frame
            // short — a steady phase drift. The K-th repetition of the
            // period carries the same quantization error spread over K
            // periods: refine the peak near K·lagF and divide by K.
            int K = static_cast<int>((count / 2 - 2) / lagF);
            if (K > 12) K = 12;
            if (K >= 3) {
                int center = static_cast<int>(K * lagF + 0.5f);
                int halfWin = K / 2 + 1;  // ±0.5 frame/period uncertainty × K
                int bestL = center;
                float bestV = -1e30f;
                for (int L = center - halfWin; L <= center + halfWin; L++) {
                    if (L < 2 || L + 1 >= count) continue;
                    float v = acfAt(L);
                    if (v > bestV) { bestV = v; bestL = L; }
                }
                float la = acfAt(bestL - 1), lb = bestV, lc = acfAt(bestL + 1);
                float ld = la - 2.0f * lb + lc;
                float lfrac = 0.0f;
                if (lb >= la && lb >= lc && ld < -1e-12f) {
                    lfrac = clampf(0.5f * (la - lc) / ld, -0.5f, 0.5f);
                }
                float refined = (static_cast<float>(bestL) + lfrac) / static_cast<float>(K);
                // Accept only if it agrees with the short-lag estimate (a
                // different peak family at long lag would mean a tempo
                // change inside the buffer).
                if (fabsf(refined - lagF) < 0.75f) lagF = refined;
            }
            periodF = lagF * static_cast<float>(refineMultiplier);
            // The fold's integer period must stay the nearest integer.
            if (fabsf(periodF - static_cast<float>(bestPeriod)) > 1.5f) {
                periodF = static_cast<float>(bestPeriod);
            }
        }
    }

    // --- Accent position from the TRANSIENT source ---
    // The winning period may come from the bass-energy or NN buffer, but
    // those are smooth/level-like: with a long (bar-level) period there are
    // only ~3-4 epochs in the buffer, recency weighting favours the newest,
    // and any loudness trend puts the fold maximum at the newest frames —
    // i.e. the "accent" tracked "now" and phase hovered just below 1.0
    // (measured on techno-dub-groove: 30% of the time in 0.9-1.0). The OSS
    // buffer is half-wave-rectified bass flux (kick onsets, no trend), so
    // its fold peak is where the kick actually lands in the cycle.
    if (ossCount_ >= bestPeriod * 2) {
        float* fold = scratchFold;
        memset(fold, 0, bestPeriod * sizeof(float));
        float totalWeight = 0.0f;
        int epochs = 0;
        for (int offset = ossCount_ - bestPeriod; offset >= 0; offset -= bestPeriod) {
            float weight = expf(-plpDecayRate * epochs);
            for (int j = 0; j < bestPeriod; j++) fold[j] += ossLinear_[offset + j] * weight;
            totalWeight += weight;
            epochs++;
        }
        if (epochs >= 2 && totalWeight > 0.0f) {
            int peakBin = 0;
            float peakVal = fold[0];
            float mean = 0.0f;
            for (int j = 0; j < bestPeriod; j++) {
                mean += fold[j];
                if (fold[j] > peakVal) { peakVal = fold[j]; peakBin = j; }
            }
            mean /= bestPeriod;
            // Only trust a fold with a real peak (flat fold = no kick structure).
            if (peakVal > mean + 1e-6f) {
                float refined = static_cast<float>(peakBin);
                int prev = (peakBin - 1 + bestPeriod) % bestPeriod;
                int next = (peakBin + 1) % bestPeriod;
                float denom = fold[prev] - 2.0f * fold[peakBin] + fold[next];
                if (fabsf(denom) > 1e-9f) {
                    refined += clampf(0.5f * (fold[prev] - fold[next]) / denom, -0.5f, 0.5f);
                    if (refined < 0.0f) refined += bestPeriod;
                }
                bestAccentBin = refined;
            }
        }
    }

    float framesSinceAccent = static_cast<float>(bestPeriod - 1) - bestAccentBin;
    if (framesSinceAccent < 0.0f) framesSinceAccent += static_cast<float>(bestPeriod);
    float targetPhase = framesSinceAccent / periodF;
    targetPhase -= floorf(targetPhase);
    out.valid = true;
    out.period = bestPeriod;
    out.periodF = periodF;
    out.accentBin = bestAccentBin;
    out.targetPhase = targetPhase;
    out.source = bestSource;
    out.multiplier = bestMultiplier;
    return out;
}
