/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "sf2_hsb_converter.h"
#include "sf2_parser.h"

#include <BAE_API.h>
#include <X_API.h>
#include "X_Formats.h"
#include "mod2rmf_encoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FCC
#define FCC(a,b,c,d) FOUR_CHAR((a),(b),(c),(d))
#endif

#define SF2HSB_VOLUME_RANGE 4096
#define SF2HSB_MAX_PENDING_EXTS 512

static void set_error(char *buf, size_t sz, const char *msg)
{
    if (buf && sz > 0) {
        snprintf(buf, sz, "%s", msg ? msg : "Unknown error");
    }
}

static int ends_with_ci(const char *path, const char *suffix)
{
    size_t pl;
    size_t sl;
    size_t i;

    if (!path || !suffix) {
        return 0;
    }

    pl = strlen(path);
    sl = strlen(suffix);
    if (pl < sl) {
        return 0;
    }

    for (i = 0; i < sl; ++i) {
        char a = path[pl - sl + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }

    return 1;
}

static int32_t tc_to_us(int tc)
{
    double secs;
    int32_t us;

    if (tc <= -12000) return 0;
    if (tc >= 8000) return 100000000;

    secs = pow(2.0, tc / 1200.0);
    us = (int32_t)(secs * 1000000.0);

    if (us < 1000) us = 1000;
    if (us > 100000000) us = 100000000;
    return us;
}

static double tc_to_seconds(int tc)
{
    if (tc <= -12000) return 0.0;
    if (tc >= 8000) return 100.0;
    return pow(2.0, tc / 1200.0);
}

static int32_t seconds_to_us_clamped(double seconds)
{
    double micros;

    if (!(seconds > 0.0)) {
        return 0;
    }

    micros = seconds * 1000000.0;
    if (micros >= 100000000.0) {
        return 100000000;
    }

    return (int32_t)(micros + 0.5);
}

static double linear_amp_decay_time_to_lin_db_decay_time(double secondsToFullAtten)
{
    double targetDbLeastSquares = 70.0;
    double targetDbInitialSlope = 140.0;
    double ln10;
    double kShort;
    double kLong;
    double tKnee;
    double p;
    double x;
    double w;

    if (secondsToFullAtten <= 0.0) return 0.0;

    ln10 = 2.302585092994046;
    kShort = targetDbInitialSlope / (20.0 / ln10);
    kLong  = targetDbLeastSquares * ln10 / 45.0;
    tKnee = 0.12;
    p = 2.0;

    x = secondsToFullAtten / tKnee;
    w = 1.0 / (1.0 + pow(x, p));

    return secondsToFullAtten * (w * kShort + (1.0 - w) * kLong);
}

static double approximate_linear_amp_decay_seconds(double convertedSeconds)
{
    double low;
    double high;
    int iteration;

    if (!(convertedSeconds > 0.0)) {
        return 0.0;
    }

    low = 0.0;
    high = convertedSeconds;
    while (linear_amp_decay_time_to_lin_db_decay_time(high) < convertedSeconds) {
        low = high;
        high *= 2.0;
        if (high >= 60.0) {
            break;
        }
    }

    for (iteration = 0; iteration < 32; ++iteration) {
        double mid = (low + high) * 0.5;
        if (linear_amp_decay_time_to_lin_db_decay_time(mid) < convertedSeconds) {
            low = mid;
        } else {
            high = mid;
        }
    }

    return high;
}

/* SF2 volume envelope sustain is always amplitude centibels (÷200), independent
 * of --attn-div (which only tweaks initialAttenuation → split volume). */
static int32_t cb_to_level4096_env(int cb)
{
    double ratio;
    int32_t level;

    if (cb >= 96000) return 0;
    if (cb <= 0) return SF2HSB_VOLUME_RANGE;

    ratio = pow(10.0, -cb / 200.0);
    level = (int32_t)((double)SF2HSB_VOLUME_RANGE * ratio + 0.5);

    if (level < 0) level = 0;
    if (level > SF2HSB_VOLUME_RANGE) level = SF2HSB_VOLUME_RANGE;
    return level;
}

static double level_to_atten_db(int32_t level)
{
    if (level <= 0) {
        return 100.0;
    }
    if (level >= SF2HSB_VOLUME_RANGE) {
        return 0.0;
    }
    return -20.0 * log10((double)level / (double)SF2HSB_VOLUME_RANGE);
}

static int apply_keynum_to_tc(int baseTc, int keynumToTcPerKey, int key)
{
    long v;

    if (keynumToTcPerKey == 0) {
        return baseTc;
    }
    if (key < 0) {
        key = 0;
    } else if (key > 127) {
        key = 127;
    }

    /* SF2: tc' = tc + keynumTo*(keynum-60) */
    v = (long)baseTc + (long)keynumToTcPerKey * (long)(key - 60);
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    return (int)v;
}

static int zone_envelope_bake_key(const SF2Zone *zone, int drumNote)
{
    if (drumNote >= 0 && drumNote <= 127) {
        return drumNote;
    }

    /* Melodic INST ADSR is shared across all key splits. Baking keynumTo* at a
     * zone midpoint warps the envelope for the whole program — e.g. GeneralUser
     * Muted Guitar (keynumToVolEnvDecay=84) at mid-key 23 collapses a ~1.75s SF2
     * decay to ~0.29s and sounds like harsh attack clicks. SF2 applies keynumTo
     * per played note; with one ADSR, keep the SF2 reference key (60). */
    (void)zone;
    return 60;
}

static double resolve_attack_seconds_from_sf2_tc(int attackTc, int extAdsr)
{
    double attackSeconds = tc_to_seconds(attackTc);

    if (!(attackSeconds > 0.0)) {
        return 0.0;
    }

    /* Sub-10ms is effectively instantaneous on both paths. */
    if (attackSeconds <= 0.010) {
        return 0.0;
    }

    /* Extended/ZSB: keep true SF2 attack time (linear amplitude ramp). */
    if (extAdsr) {
        return attackSeconds;
    }

    /* Classic single-segment path: short attacks read as clicks if fully preserved. */
    if (attackSeconds <= 0.050) {
        return 0.0;
    }
    if (attackSeconds <= 0.200) {
        return attackSeconds * 0.35;
    }

    return attackSeconds;
}

static double resolve_ext_decay_seconds(double fullDropSeconds, int32_t sustainLevel)
{
    double sustain_db;
    double dbScaledSeconds;

    if (!(fullDropSeconds > 0.0)) {
        return 0.0;
    }

    /* SF2 decay TC = time for a full 100 dB drop. Decay only travels peak→sustain.
     *
     * Extended ADSR approximates that path with piecewise lin-dB (exponential amp)
     * segments, so use the true SF2 peak→sustain duration — including sustain≈0
     * plucks (Muted Guitar, etc.). The old linear-amp blend + approx*1.2 cap
     * collapsed multi-second SF2 decays to ~20ms and produced harsh attack clicks. */
    sustain_db = level_to_atten_db(sustainLevel);
    if (sustain_db < 0.0) sustain_db = 0.0;
    if (sustain_db > 100.0) sustain_db = 100.0;
    dbScaledSeconds = fullDropSeconds * sustain_db / 100.0;

    return dbScaledSeconds;
}

static double resolve_ext_release_seconds(double fullDropSeconds, int32_t sustainLevel)
{
    double sustain_db;
    double remaining_db;

    if (!(fullDropSeconds > 0.0)) {
        return 0.0;
    }

    /* SF2 release TC = time for a full 100 dB change at the release rate.
     * From sustain attenuation S to silence (100 dB), duration is
     * T * (100 - S) / 100. Loud sustains keep nearly the full tail (e.g. 39s);
     * the old T * S / 100 formula wrongly collapsed those. */
    sustain_db = level_to_atten_db(sustainLevel);
    if (sustain_db < 0.0) sustain_db = 0.0;
    if (sustain_db > 100.0) sustain_db = 100.0;
    remaining_db = 100.0 - sustain_db;
    if (remaining_db < 0.5) {
        remaining_db = 0.5; /* avoid zero-length release when already near silence */
    }
    return fullDropSeconds * remaining_db / 100.0;
}

static double resolve_decay_seconds_from_sf2_tc(int decayTc, int32_t sustainLevel, int extAdsr)
{
    double decaySeconds = tc_to_seconds(decayTc);

    if (!(decaySeconds > 0.0)) {
        return 0.0;
    }

    if (extAdsr) {
        return resolve_ext_decay_seconds(decaySeconds, sustainLevel);
    }

    /* Classic single-segment path */
    if (sustainLevel <= 4) {
        return approximate_linear_amp_decay_seconds(decaySeconds);
    }
    return decaySeconds;
}

static double resolve_release_seconds_from_sf2_tc(int releaseTc, int32_t sustainLevel, int extAdsr)
{
    double releaseSeconds = tc_to_seconds(releaseTc);

    if (!(releaseSeconds > 0.0)) {
        return 0.0;
    }

    if (extAdsr) {
        return resolve_ext_release_seconds(releaseSeconds, sustainLevel);
    }

    /* Classic single-segment path */
    if (sustainLevel <= 4) {
        return approximate_linear_amp_decay_seconds(releaseSeconds);
    }
    return releaseSeconds;
}

static int32_t pm_to_level4096(int pm)
{
    int32_t clamped = pm;
    if (clamped < 0) clamped = 0;
    if (clamped > 1000) clamped = 1000;
    return (SF2HSB_VOLUME_RANGE * (1000 - clamped)) / 1000;
}

static int16_t rd_s16le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t sf2_sample_type_base(uint16_t sampleType)
{
    return (uint16_t)(sampleType & 0x7FFFu);
}

static int sf2_zone_is_lr_pair_compatible(SF2Zone const *a, SF2Zone const *b)
{
    if (!a || !b) {
        return 0;
    }

    return a->loKey == b->loKey &&
           a->hiKey == b->hiKey &&
           a->loVel == b->loVel &&
           a->hiVel == b->hiVel &&
           a->overrideRootKey == b->overrideRootKey &&
           a->coarseTune == b->coarseTune &&
           a->fineTune == b->fineTune &&
           a->sampleModes == b->sampleModes &&
           a->scaleTuning == b->scaleTuning &&
           a->startAddrsOffset == b->startAddrsOffset &&
           a->startAddrsCoarse == b->startAddrsCoarse &&
           a->endAddrsOffset == b->endAddrsOffset &&
           a->endAddrsCoarse == b->endAddrsCoarse &&
           a->startloopAddrsOffset == b->startloopAddrsOffset &&
           a->startloopAddrsCoarse == b->startloopAddrsCoarse &&
           a->endloopAddrsOffset == b->endloopAddrsOffset &&
           a->endloopAddrsCoarse == b->endloopAddrsCoarse;
}

static void interleave_stereo_pcm16(const uint8_t *left,
                                    const uint8_t *right,
                                    uint32_t frameCount,
                                    uint8_t *dstInterleaved)
{
    uint32_t f;
    for (f = 0; f < frameCount; ++f) {
        uint32_t leftOfs = f * 2u;
        uint32_t dstOfs = f * 4u;

        dstInterleaved[dstOfs + 0u] = left[leftOfs + 0u];
        dstInterleaved[dstOfs + 1u] = left[leftOfs + 1u];
        dstInterleaved[dstOfs + 2u] = right[leftOfs + 0u];
        dstInterleaved[dstOfs + 3u] = right[leftOfs + 1u];
    }
}

static uint64_t score_loop_boundary_mono(const uint8_t *pcmData,
                                         uint32_t frameCount,
                                         uint32_t loopStart,
                                         uint32_t loopEnd)
{
    uint32_t lastFrame;
    uint32_t prevLastFrame;
    uint32_t nextFrame;
    int16_t first;
    int16_t last;
    int16_t prevLast;
    int16_t next;
    uint64_t ampJump;
    long long slopeLeft;
    long long slopeRight;
    uint64_t slopeJump;

    if (!pcmData || frameCount < 3 || loopEnd <= loopStart + 1 || loopEnd > frameCount) {
        return (uint64_t)-1;
    }

    lastFrame = loopEnd - 1;
    prevLastFrame = loopEnd - 2;
    nextFrame = (loopStart + 1 < loopEnd) ? (loopStart + 1) : loopStart;

    first = rd_s16le(pcmData + (loopStart * 2u));
    last = rd_s16le(pcmData + (lastFrame * 2u));
    prevLast = rd_s16le(pcmData + (prevLastFrame * 2u));
    next = rd_s16le(pcmData + (nextFrame * 2u));

    ampJump = (uint64_t)llabs((long long)last - (long long)first);
    slopeLeft = (long long)last - (long long)prevLast;
    slopeRight = (long long)next - (long long)first;
    slopeJump = (uint64_t)llabs(slopeLeft - slopeRight);

    return (ampJump * 4u) + slopeJump;
}

static void refine_loop_points_from_pcm(const uint8_t *pcmData,
                                        uint32_t frameCount,
                                        int32_t *ioLoopStart,
                                        int32_t *ioLoopEnd)
{
    uint32_t baseStart;
    uint32_t baseEnd;
    uint32_t loopLen;
    uint32_t bestStart;
    uint32_t bestEnd;
    uint64_t bestScore;
    int delta;

    if (!pcmData || !ioLoopStart || !ioLoopEnd) {
        return;
    }
    if (*ioLoopStart < 0 || *ioLoopEnd <= *ioLoopStart || (uint32_t)*ioLoopEnd > frameCount || frameCount < 3) {
        return;
    }

    baseStart = (uint32_t)*ioLoopStart;
    baseEnd = (uint32_t)*ioLoopEnd;
    if (baseEnd <= baseStart + 2) {
        return;
    }

    loopLen = baseEnd - baseStart;
    bestStart = baseStart;
    bestEnd = baseEnd;
    bestScore = score_loop_boundary_mono(pcmData, frameCount, baseStart, baseEnd);

    /* Test inclusive->exclusive interpretation mismatch (end - 1). */
    if (baseEnd > baseStart + 2) {
        uint64_t endMinusOneScore = score_loop_boundary_mono(pcmData, frameCount, baseStart, baseEnd - 1);
        if (endMinusOneScore < bestScore) {
            bestScore = endMinusOneScore;
            bestStart = baseStart;
            bestEnd = baseEnd - 1;
        }
    }

    /* Slide the whole loop (preserving length) within a window of +/-32 frames
       to find the position where the loop seam discontinuity is minimised.
       This covers byte-to-frame rounding errors from compressed source formats. */
    for (delta = -32; delta <= 32; ++delta) {
        long long shiftedStartLL = (long long)baseStart + delta;
        uint32_t shiftedStart;
        uint32_t shiftedEnd;
        uint64_t score;

        if (shiftedStartLL < 0) {
            continue;
        }
        shiftedStart = (uint32_t)shiftedStartLL;
        shiftedEnd = shiftedStart + loopLen;
        if (shiftedEnd > frameCount || shiftedEnd <= shiftedStart + 1) {
            continue;
        }

        score = score_loop_boundary_mono(pcmData, frameCount, shiftedStart, shiftedEnd);
        if (score < bestScore) {
            bestScore = score;
            bestStart = shiftedStart;
            bestEnd = shiftedEnd;
        }

        /* Also test end - 1 at this shifted position. */
        if (shiftedEnd > shiftedStart + 2) {
            score = score_loop_boundary_mono(pcmData, frameCount, shiftedStart, shiftedEnd - 1);
            if (score < bestScore) {
                bestScore = score;
                bestStart = shiftedStart;
                bestEnd = shiftedEnd - 1;
            }
        }
    }

    *ioLoopStart = (int32_t)bestStart;
    *ioLoopEnd = (int32_t)bestEnd;
}

/* Allocate ZSB polyline segments across decay/release only.
 * SF2 volume attack is linear in amplitude (convex only in dB space), so attack
 * always uses a single Beatnik LINE stage and does not consume this budget.
 */
static void allocate_decay_release_segments(int budget,
                                            double decaySec,
                                            double releaseSec,
                                            int *outDecay,
                                            int *outRelease)
{
    double wD = (decaySec > 0.001) ? (1.0 + decaySec * 1.5) : 0.0;
    double wR = (releaseSec > 0.001) ? (1.5 + releaseSec * 2.5) : 0.0;
    double sum = wD + wR;
    int nD = 0, nR = 0;
    int used;
    int leftover;

    if (budget < 1) {
        budget = 1;
    }

    if (sum <= 0.0) {
        *outDecay = (decaySec > 0.0) ? 1 : 0;
        *outRelease = (releaseSec > 0.0) ? 1 : 0;
        return;
    }

    if (wD > 0.0) nD = (int)((budget * wD / sum) + 0.5);
    if (wR > 0.0) nR = (int)((budget * wR / sum) + 0.5);
    if (wD > 0.0 && nD < 1) nD = 1;
    if (wR > 0.0 && nR < 1) nR = 1;

    used = nD + nR;
    while (used > budget) {
        if (nR >= nD && nR > 1) {
            nR--;
        } else if (nD > 1) {
            nD--;
        } else {
            break;
        }
        used = nD + nR;
    }

    leftover = budget - used;
    if (leftover > 0) {
        if (nR >= nD) {
            nR += leftover;
        } else {
            nD += leftover;
        }
    }

    *outDecay = nD;
    *outRelease = nR;
}

/* SF2 decay/release are linear in dB (= exponential in Beatnik linear amplitude).
 * Place equal-time knots in log-amplitude space so each LINE segment tracks the
 * SF2 lin-dB trajectory. Returns the new stage index.
 */
static uint32_t adsr_push_decay_piecewise(BAERmfEditorADSRInfo *adsr,
                                          uint32_t st,
                                          int32_t startLevel,
                                          int32_t endLevel,
                                          int32_t decayUs,
                                          int32_t finalFlags,
                                          int nSegments)
{
    int N = (nSegments > 1) ? nSegments : 1;
    int i;

    if (decayUs <= 0) {
        if (st < BAE_EDITOR_MAX_ADSR_STAGES) {
            adsr->stages[st].level = endLevel;
            adsr->stages[st].time = 0;
            adsr->stages[st].flags = finalFlags;
            st++;
        }
        return st;
    }
    if (startLevel <= endLevel
            || (int32_t)(st + (uint32_t)N) > BAE_EDITOR_MAX_ADSR_STAGES
            || decayUs < N * 1000) {
        if (st < BAE_EDITOR_MAX_ADSR_STAGES) {
            adsr->stages[st].level = endLevel;
            adsr->stages[st].time = decayUs;
            adsr->stages[st].flags = finalFlags;
            st++;
        }
        return st;
    }

    {
        /* Silence floor ~-96 dB relative to start when targeting 0. */
        double floorLevel = (endLevel > 0) ? (double)endLevel
                                           : ((double)startLevel * pow(10.0, -96.0 / 20.0));
        double ln_start;
        double ln_end;
        int32_t segTime;
        int32_t lastTime;
        uint32_t begin = st;

        if (endLevel <= 0 && floorLevel < 1.0) {
            floorLevel = 1.0;
        }
        ln_start = log((double)startLevel);
        ln_end = log(floorLevel);
        segTime = decayUs / N;
        lastTime = decayUs - segTime * (N - 1);

        for (i = 1; i <= N; i++) {
            double t = (double)i / (double)N;
            int32_t level = (i == N) ? endLevel
                          : (int32_t)(exp(ln_start + t * (ln_end - ln_start)) + 0.5);
            int32_t time = (i == N) ? lastTime : segTime;
            int32_t flags = (i == N) ? finalFlags : FCC('L','I','N','E');

            if (time < 1) time = 1;
            if (level < 0) level = 0;
            if (endLevel > 0 && level < endLevel) level = endLevel;
            if (level > startLevel) level = startLevel;

            if (i < N && st > 0
                    && adsr->stages[st - 1].flags == FCC('L','I','N','E')
                    && adsr->stages[st - 1].level == level) {
                adsr->stages[st - 1].time += time;
            } else {
                if (st >= BAE_EDITOR_MAX_ADSR_STAGES) break;
                adsr->stages[st].level = level;
                adsr->stages[st].time = time;
                adsr->stages[st].flags = flags;
                st++;
            }
        }

        {
            int32_t sum = 0;
            uint32_t j;
            for (j = begin; j < st; j++) {
                sum += adsr->stages[j].time;
            }
            if (st > begin && sum != decayUs) {
                adsr->stages[st - 1].time += (decayUs - sum);
                if (adsr->stages[st - 1].time < 1) {
                    adsr->stages[st - 1].time = 1;
                }
            }
        }
    }
    return st;
}

/* Build BAE ADSR stages from SF2 envelope parameters.
 *
 * SF2 volume envelope (important for Beatnik's linear-amplitude ADSR):
 *   - Attack is convex in dB space so that amplitude rises LINEARLY.
 *   - Decay/release are linear in dB (= exponential in amplitude).
 *
 * extAdsr: spend ZSB's extra stages approximating lin-dB decay/release.
 * Classic mode: one LINE segment per phase (HSB, max 8 stages).
 */
static void build_adsr_from_sf2(BAERmfEditorADSRInfo *adsr,
                                int delayTc,
                                int attackTc,
                                int holdTc,
                                int decayTc,
                                int32_t sustainLevel,
                                int releaseTc,
                                int extAdsr)
{
    uint32_t st = 0;
    double attackSec = resolve_attack_seconds_from_sf2_tc(attackTc, extAdsr);
    double decaySec = resolve_decay_seconds_from_sf2_tc(decayTc, sustainLevel, extAdsr);
    double releaseSec = resolve_release_seconds_from_sf2_tc(releaseTc, sustainLevel, extAdsr);
    int32_t attackUs = seconds_to_us_clamped(attackSec);
    int32_t holdUs = tc_to_us(holdTc);
    int32_t decayUs = seconds_to_us_clamped(decaySec);
    int32_t releaseUs = seconds_to_us_clamped(releaseSec);
    int32_t delayUs = tc_to_us(delayTc);
    int maxAdsrStages = (extAdsr) ? BAE_EDITOR_MAX_ADSR_STAGES : 8;
    int nDecay = 1;
    int nRelease = 1;

    memset(adsr, 0, sizeof(*adsr));

    /* SF2: sustain at full level implies zero-length decay regardless of decay TC. */
    if (sustainLevel >= SF2HSB_VOLUME_RANGE - 1) {
        decayUs = 0;
        decaySec = 0.0;
        sustainLevel = SF2HSB_VOLUME_RANGE;
    }

    if (extAdsr) {
        int fixedStages = 1 /* sustain */
                        + 1 /* linear attack */
                        + (delayUs > 0 ? 1 : 0)
                        + (holdUs > 0 ? 1 : 0);
        int budget = BAE_EDITOR_MAX_ADSR_STAGES - fixedStages;
        if (budget < 2) budget = 2;
        allocate_decay_release_segments(budget, decaySec, releaseSec, &nDecay, &nRelease);
        if (nDecay < 1) nDecay = 1;
        if (nRelease < 1) nRelease = 1;
    }

    if (delayUs > 0 && st < (uint32_t)maxAdsrStages) {
        adsr->stages[st].level = 0;
        adsr->stages[st].time = delayUs;
        adsr->stages[st].flags = FCC('L','I','N','E');
        st++;
    }

    /* Attack: always a single linear amplitude ramp (SF2-correct for Beatnik). */
    if (st < (uint32_t)maxAdsrStages) {
        adsr->stages[st].level = SF2HSB_VOLUME_RANGE;
        adsr->stages[st].time = attackUs;
        adsr->stages[st].flags = FCC('L','I','N','E');
        st++;
    }

    if (holdUs > 0 && st < (uint32_t)maxAdsrStages) {
        adsr->stages[st].level = SF2HSB_VOLUME_RANGE;
        adsr->stages[st].time = holdUs;
        adsr->stages[st].flags = FCC('L','I','N','E');
        st++;
    }

    if (extAdsr) {
        st = adsr_push_decay_piecewise(adsr, st, SF2HSB_VOLUME_RANGE, sustainLevel, decayUs,
                                       FCC('L','I','N','E'), nDecay);
    } else if (st < (uint32_t)maxAdsrStages) {
        adsr->stages[st].level = sustainLevel;
        adsr->stages[st].time = decayUs;
        adsr->stages[st].flags = FCC('L','I','N','E');
        st++;
    }

    if (st < (uint32_t)maxAdsrStages) {
        adsr->stages[st].level = sustainLevel;
        adsr->stages[st].time = 0;
        adsr->stages[st].flags = FCC('S','U','S','T');
        st++;
    }

    if (releaseUs < 1000) {
        releaseUs = 1000;
    }

    if (extAdsr) {
        st = adsr_push_decay_piecewise(adsr, st, sustainLevel, 0, releaseUs,
                                       FCC('L','A','S','T'), nRelease);
    } else if (st < (uint32_t)maxAdsrStages) {
        adsr->stages[st].level = 0;
        adsr->stages[st].time = releaseUs;
        adsr->stages[st].flags = FCC('L','A','S','T');
        st++;
    }

    adsr->stageCount = st;
}

static void build_lfo_delay_adsr(BAERmfEditorADSRInfo *adsr, int delayTc)
{
    int32_t delayUs = tc_to_us(delayTc);
    memset(adsr, 0, sizeof(*adsr));
    if (delayUs > 0) {
        adsr->stageCount = 2;
        adsr->stages[0].level = 0;
        adsr->stages[0].time = delayUs;
        adsr->stages[0].flags = FCC('L','I','N','E');
        adsr->stages[1].level = SF2HSB_VOLUME_RANGE;
        adsr->stages[1].time = 0;
        adsr->stages[1].flags = FCC('S','U','S','T');
    } else {
        adsr->stageCount = 1;
        adsr->stages[0].level = SF2HSB_VOLUME_RANGE;
        adsr->stages[0].time = 0;
        adsr->stages[0].flags = FCC('S','U','S','T');
    }
}

static int16_t cb_to_split_volume(int cb, int attnDiv)
{
    /* initialAttenuation is in centibels (0.1 dB); 0 = full scale.
     * BAE split volume: miscParameter2, 100 = unity
     *   Volume = (Volume * miscParameter2) / 100
     *
     * attnDiv selects the dB→amplitude curve:
     *   200 = IEEE SF2 literal amplitude (10^(-dB/20)). Sounds too extreme in
     *         Beatnik: soft layers collapse to vol≈1..15 and vanish next to hot ones.
     *   500 = Creative/AWE-compatible (~0.4 dB applied per SF2 dB). Default.
     *         Most GM banks were authored against this hardware quirk.
     *   400 = legacy half-range compromise.
     *
     * Result is further adjusted by finalize_split_volume() using PCM peak so
     * quiet-recorded samples (typical strings) aren't buried under hot ones.
     */
    double amp;
    int32_t v;

    if (cb <= 0) {
        return 100;
    }
    if (attnDiv <= 0) {
        attnDiv = 500;
    }

    amp = pow(10.0, -cb / (double)attnDiv);
    /* Compress into a Beatnik-friendly band (~35..100) so attenuation still
     * ranks instruments without driving soft layers into the noise floor. */
    v = (int32_t)(35.0 + 65.0 * pow(amp, 0.75) + 0.5);
    if (v < 20) {
        v = 20;
    }
    if (v > 127) {
        v = 127;
    }
    return (int16_t)v;
}

/* Peak of int16 PCM (mono or interleaved stereo). */
static int32_t pcm16_peak_abs(const uint8_t *pcm, uint32_t frames, int channels)
{
    const int16_t *s;
    uint32_t n;
    uint32_t i;
    int32_t peak = 0;

    if (!pcm || frames == 0u || channels < 1) {
        return 0;
    }
    s = (const int16_t *)(const void *)pcm;
    n = frames * (uint32_t)channels;
    for (i = 0; i < n; ++i) {
        int32_t a = (int32_t)s[i];
        if (a < 0) {
            a = -a;
        }
        if (a > peak) {
            peak = a;
        }
    }
    return peak;
}

/* Combine SF2 attenuation volume with sample-peak makeup. SF2 banks often mix
 * full-scale plucks with quietly recorded sustains; Beatnik applies split volume
 * as linear gain on whatever PCM we stored, so without makeup the quiet samples
 * stay inaudible even after AWE-style attenuation mapping. */
static int16_t finalize_split_volume(int16_t attenVol, int32_t peakAbs)
{
    double peakNorm;
    double makeup;
    int32_t v;

    if (attenVol < 1) {
        attenVol = 1;
    }
    if (peakAbs < 1) {
        peakAbs = 1;
    }

    peakNorm = (double)peakAbs / 32767.0;
    if (peakNorm < 0.03) {
        peakNorm = 0.03; /* don't insane-boost near-silence / pads of zeros */
    }

    /* Target ~0.65 FS contribution at attenVol=100. */
    makeup = 0.65 / peakNorm;
    if (makeup > 5.0) {
        makeup = 5.0;
    }
    if (makeup < 0.40) {
        makeup = 0.40;
    }

    v = (int32_t)((double)attenVol * makeup + 0.5);
    if (v < 18) {
        v = 18;
    }
    if (v > 127) {
        v = 127;
    }
    return (int16_t)v;
}

static int16_t sf2_pan_to_inst_pan(int pan)
{
    int32_t p = (pan * 127) / 500;
    if (p < -128) p = -128;
    if (p > 127) p = 127;
    return (int16_t)p;
}

static uint32_t sf2_preset_to_inst_id(uint16_t sf2Bank, uint16_t sf2Preset)
{
    uint32_t bankGroup;
    uint32_t base;

    bankGroup = (uint32_t)(sf2Bank);
    base = bankGroup * 256u;
    return base + (uint32_t)sf2Preset;
}

static uint32_t sf2_drum_note_to_inst_id(int drumKitSlot, int note)
{
    if (note < 0) note = 0;
    if (note > 127) note = 127;

    if (drumKitSlot <= 0) {
        return 128u + (uint32_t)note;   /* perc bank 0 -> 128-255 */
    }
    return 384u + (uint32_t)note;       /* perc bank 1 -> 384-511 */
}

typedef struct {
    int sampleIdx;
    uint32_t frameStart;
    uint32_t frameCount;
    uint32_t sampleRateFixed;
    int rootKey;
    uint32_t loopStart;
    uint32_t loopEnd;
    int16_t splitVolume;
    uint64_t pcmHash;
    uint32_t assetID;
} CachedSampleAsset;

static uint64_t hash_pcm_fnv1a64(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ULL;
    size_t i;

    if (!p || size == 0) {
        return 0;
    }

    for (i = 0; i < size; ++i) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }

    return h;
}

typedef struct {
    uint32_t instID;
    SF2Zone zone;
    const char *presetName;
    int isDrumPreset;
    int enableSampleAndHold;
    int32_t panSum;      /* sum of pan values at the current best specificity level */
    uint32_t panCount;   /* number of zones contributing to panSum */
    int panBestRange;    /* drums: smallest (zone->hiKey - zone->loKey) seen so far;
                          * -1 for melodic (all zones averaged equally) */
} PendingInstrumentExt;

static int should_enable_sample_and_hold_for_zone(SF2Zone const *zone, int attnDiv)
{
    (void)attnDiv;
    if (!zone) {
        return 0;
    }
    return (zone->sampleModes & 0x1) != 0;
}

static int prefer_zone_for_instrument_ext(SF2Zone const *candidate,
                                          SF2Zone const *current,
                                          int attnDiv)
{
    int32_t candidateSustain;
    int32_t currentSustain;
    double candidateHold;
    double currentHold;
    double candidateDecay;
    double currentDecay;
    double candidateRelease;
    double currentRelease;

    if (!candidate) {
        return 0;
    }
    if (!current) {
        return 1;
    }

    (void)attnDiv;
    candidateSustain = cb_to_level4096_env(candidate->volSustainCb);
    currentSustain = cb_to_level4096_env(current->volSustainCb);
    if (candidateSustain != currentSustain) {
        return candidateSustain > currentSustain;
    }

    candidateHold = tc_to_seconds(candidate->volHoldTc);
    currentHold = tc_to_seconds(current->volHoldTc);
    if (candidateHold != currentHold) {
        return candidateHold > currentHold;
    }

    candidateDecay = resolve_decay_seconds_from_sf2_tc(candidate->volDecayTc, candidateSustain, 0);
    currentDecay = resolve_decay_seconds_from_sf2_tc(current->volDecayTc, currentSustain, 0);
    if (candidateDecay != currentDecay) {
        return candidateDecay > currentDecay;
    }

    candidateRelease = resolve_release_seconds_from_sf2_tc(candidate->volReleaseTc, candidateSustain, 0);
    currentRelease = resolve_release_seconds_from_sf2_tc(current->volReleaseTc, currentSustain, 0);
    return candidateRelease > currentRelease;
}

static int prefer_zone_for_drum_note_at_key(SF2Zone const *candidate,
                                            SF2Zone const *current,
                                            int note,
                                            SF2SampleHdr const *samples,
                                            uint32_t sampleCount)
{
    int candKeySpan;
    int currKeySpan;
    int candVelSpan;
    int currVelSpan;
    int candRoot = 60;
    int currRoot = 60;
    int candRootDist;
    int currRootDist;
    int candEdgeScore;
    int currEdgeScore;
    double candRelease;
    double currRelease;
    double candDecay;
    double currDecay;

    if (!candidate) {
        return 0;
    }
    if (!current) {
        return 1;
    }

    /* Prefer more specific key mapping first. */
    candKeySpan = (int)candidate->hiKey - (int)candidate->loKey;
    currKeySpan = (int)current->hiKey - (int)current->loKey;
    if (candKeySpan != currKeySpan) {
        return candKeySpan < currKeySpan;
    }

    /* For overlapping same-span ranges, prefer the zone whose range starts at this
     * note, then one that ends at this note. This avoids pairwise note cloning at
     * boundaries (e.g. 65/66 both picking the same adjacent range). */
    candEdgeScore = (note == (int)candidate->loKey) ? 0 : ((note == (int)candidate->hiKey) ? 1 : 2);
    currEdgeScore = (note == (int)current->loKey) ? 0 : ((note == (int)current->hiKey) ? 1 : 2);
    if (candEdgeScore != currEdgeScore) {
        return candEdgeScore < currEdgeScore;
    }

    /* Prefer the zone whose natural/root pitch is closest to the requested note.
     * This avoids adjacent-note cloning when overlapping ranges exist. */
    if (samples) {
        if (candidate->sampleIdx >= 0 && (uint32_t)candidate->sampleIdx < sampleCount) {
            SF2SampleHdr const *s = &samples[candidate->sampleIdx];
            candRoot = (candidate->overrideRootKey >= 0 && candidate->overrideRootKey <= 127)
                     ? candidate->overrideRootKey
                     : ((s->originalPitch != 255) ? s->originalPitch : 60);
            candRoot -= candidate->coarseTune;
        }
        if (current->sampleIdx >= 0 && (uint32_t)current->sampleIdx < sampleCount) {
            SF2SampleHdr const *s = &samples[current->sampleIdx];
            currRoot = (current->overrideRootKey >= 0 && current->overrideRootKey <= 127)
                     ? current->overrideRootKey
                     : ((s->originalPitch != 255) ? s->originalPitch : 60);
            currRoot -= current->coarseTune;
        }
    }
    if (candRoot < 0) candRoot = 0;
    if (candRoot > 127) candRoot = 127;
    if (currRoot < 0) currRoot = 0;
    if (currRoot > 127) currRoot = 127;

    candRootDist = abs(candRoot - note);
    currRootDist = abs(currRoot - note);

    /* If one candidate has an effectively instant tail and another has a real
     * release, prefer the one with the real tail before root-distance tie-break. */
    candRelease = tc_to_seconds(candidate->volReleaseTc);
    currRelease = tc_to_seconds(current->volReleaseTc);
    if ((candRelease > 0.020 && currRelease <= 0.002) ||
        (currRelease > 0.020 && candRelease <= 0.002)) {
        return candRelease > currRelease;
    }

    if (candRootDist != currRootDist) {
        return candRootDist < currRootDist;
    }

    /* Preserve natural tails: prefer zones with longer SF2 release/decay when
     * key/root matching is otherwise equivalent. */
    if (candRelease != currRelease) {
        return candRelease > currRelease;
    }

    candDecay = tc_to_seconds(candidate->volDecayTc);
    currDecay = tc_to_seconds(current->volDecayTc);
    if (candDecay != currDecay) {
        return candDecay > currDecay;
    }

    /* With no velocity gating in this split path, prefer wider velocity coverage
     * after key/root matching so the chosen zone behaves well across dynamics. */
    candVelSpan = (int)candidate->hiVel - (int)candidate->loVel;
    currVelSpan = (int)current->hiVel - (int)current->loVel;
    if (candVelSpan != currVelSpan) {
        return candVelSpan > currVelSpan;
    }

    if (candidate->hiVel != current->hiVel) {
        return candidate->hiVel > current->hiVel;
    }

    if (candidate->initialAttenuation != current->initialAttenuation) {
        return candidate->initialAttenuation < current->initialAttenuation;
    }

    return candidate->sampleIdx < current->sampleIdx;
}

static int prefer_zone_for_melodic_layer(SF2Zone const *candidate,
                                         SF2Zone const *current)
{
    const int targetVel = 100;
    int candVelSpan;
    int currVelSpan;
    int candCoversTarget;
    int currCoversTarget;
    int candMid;
    int currMid;
    int candDist;
    int currDist;

    if (!candidate) {
        return 0;
    }
    if (!current) {
        return 1;
    }

    candVelSpan = (int)candidate->hiVel - (int)candidate->loVel;
    currVelSpan = (int)current->hiVel - (int)current->loVel;

    candCoversTarget = (candidate->loVel <= targetVel && targetVel <= candidate->hiVel) ? 1 : 0;
    currCoversTarget = (current->loVel <= targetVel && targetVel <= current->hiVel) ? 1 : 0;
    if (candCoversTarget != currCoversTarget) {
        return candCoversTarget > currCoversTarget;
    }

    candMid = ((int)candidate->loVel + (int)candidate->hiVel) / 2;
    currMid = ((int)current->loVel + (int)current->hiVel) / 2;
    candDist = abs(candMid - targetVel);
    currDist = abs(currMid - targetVel);
    if (candDist != currDist) {
        return candDist < currDist;
    }

    if (candVelSpan != currVelSpan) {
        return candVelSpan < currVelSpan;
    }

    if (candidate->initialAttenuation != current->initialAttenuation) {
        return candidate->initialAttenuation < current->initialAttenuation;
    }

    return candidate->sampleIdx < current->sampleIdx;
}

/* Beatnik keymap splits are first-match only (GenSynth breaks on the first
 * hit). SF2 presets often layer different instruments on overlapping keys
 * (e.g. GeneralUser FM EP: DX7 Strike + DX7 Wave). Emitting both as overlapping
 * splits makes neighbouring notes flip between layers — "wrong sample" at
 * boundaries like G#5. Resolve by mixing layered zones into one split per
 * atomic key range. */
#define SF2HSB_MAX_LAYER_ZONES 4
#define SF2HSB_MAX_MELODIC_JOBS 512

typedef struct {
    int loKey;
    int hiKey;
    int nLayers;
    int layerZoneIdx[SF2HSB_MAX_LAYER_ZONES];
    uint8_t *mixedPcm;
    uint32_t mixedFrames;
    uint32_t mixedSampleRate;
    int mixedRootKey;
    int32_t mixedLoopStart;
    int32_t mixedLoopEnd;
    int16_t mixedSplitVolume;
} MelodicEmitJob;

static int melodic_zone_kept(SF2Zone const *zones, uint32_t zoneCount, uint32_t z)
{
    uint32_t zk;
    SF2Zone const *zone = &zones[z];

    if (zone->sampleIdx < 0) {
        return 0;
    }
    for (zk = 0; zk < zoneCount; ++zk) {
        SF2Zone const *other = &zones[zk];
        if (zk == z || other->sampleIdx < 0) {
            continue;
        }
        if (other->loKey == zone->loKey && other->hiKey == zone->hiKey) {
            if (prefer_zone_for_melodic_layer(other, zone)) {
                return 0;
            }
        }
    }
    return 1;
}

static int zone_effective_root(SF2Zone const *zone, SF2SampleHdr const *sample)
{
    int rootKey = (zone->overrideRootKey >= 0 && zone->overrideRootKey <= 127)
                      ? zone->overrideRootKey
                      : ((sample->originalPitch != 255) ? sample->originalPitch : 60);
    rootKey -= zone->coarseTune;
    if (rootKey < 0) rootKey = 0;
    if (rootKey > 127) rootKey = 127;
    return rootKey;
}

static int16_t *pitch_resample_mono_linear(const int16_t *src,
                                           uint32_t srcFrames,
                                           uint32_t srcRate,
                                           int srcRoot,
                                           int dstNote,
                                           uint32_t dstRate,
                                           double fineTuneCents,
                                           uint32_t *outFrames)
{
    double pitchRatio;
    double outFramesF;
    uint32_t outCount;
    int16_t *out;
    uint32_t i;

    if (!src || srcFrames == 0u || !outFrames || srcRate < 1u || dstRate < 1u) {
        return NULL;
    }

    pitchRatio = pow(2.0, ((double)(dstNote - srcRoot) + fineTuneCents / 100.0) / 12.0);
    if (!(pitchRatio > 1.0e-6)) {
        pitchRatio = 1.0e-6;
    }
    outFramesF = (double)srcFrames * (double)dstRate / ((double)srcRate * pitchRatio);
    if (outFramesF < 1.0) {
        outFramesF = 1.0;
    }
    if (outFramesF > 8.0 * 44100.0) {
        outFramesF = 8.0 * 44100.0; /* cap mixed layer length */
    }
    outCount = (uint32_t)(outFramesF + 0.5);
    out = (int16_t *)malloc((size_t)outCount * sizeof(int16_t));
    if (!out) {
        return NULL;
    }

    for (i = 0; i < outCount; ++i) {
        double srcPos = ((double)i * (double)srcFrames) / (double)outCount;
        uint32_t i0 = (uint32_t)srcPos;
        double frac = srcPos - (double)i0;
        int16_t s0;
        int16_t s1;
        double y;

        if (i0 >= srcFrames) {
            i0 = srcFrames - 1u;
        }
        s0 = src[i0];
        s1 = (i0 + 1u < srcFrames) ? src[i0 + 1u] : s0;
        y = (1.0 - frac) * (double)s0 + frac * (double)s1;
        if (y > 32767.0) y = 32767.0;
        if (y < -32768.0) y = -32768.0;
        out[i] = (int16_t)(y + (y >= 0.0 ? 0.5 : -0.5));
    }

    *outFrames = outCount;
    return out;
}

static int mix_melodic_layers_to_job(SF2Bank const *sf2,
                                     SF2Zone const *zones,
                                     MelodicEmitJob *job,
                                     int attnDiv)
{
    int mid;
    int li;
    uint32_t dstRate = 44100u;
    uint32_t maxFrames = 0u;
    int16_t *layerPcm[SF2HSB_MAX_LAYER_ZONES];
    uint32_t layerFrames[SF2HSB_MAX_LAYER_ZONES];
    double layerGain[SF2HSB_MAX_LAYER_ZONES];
    int loopLayer = -1;
    int32_t loopStart = 0;
    int32_t loopEnd = 0;
    float *mix;
    uint32_t i;
    double peak = 1.0;
    int16_t *out;
    int bestAttn = 0;
    int32_t peakAbs = 1;

    memset(layerPcm, 0, sizeof(layerPcm));
    if (!sf2 || !zones || !job || job->nLayers < 2) {
        return -1;
    }

    mid = (job->loKey + job->hiKey) / 2;
    if (mid < 0) mid = 0;
    if (mid > 127) mid = 127;
    job->mixedRootKey = mid;

    for (li = 0; li < job->nLayers; ++li) {
        SF2Zone const *zone = &zones[job->layerZoneIdx[li]];
        SF2SampleHdr const *sample;
        int32_t finalStart;
        int32_t finalEnd;
        int32_t finalLoopStart;
        int32_t finalLoopEnd;
        uint32_t srcFrames;
        int rootKey;
        const int16_t *src;
        uint32_t outFrames = 0;
        double fineCents;

        if (zone->sampleIdx < 0 || (uint32_t)zone->sampleIdx >= sf2->sampleCount) {
            goto fail;
        }
        sample = &sf2->samples[zone->sampleIdx];
        finalStart = (int32_t)sample->start + zone->startAddrsOffset + (zone->startAddrsCoarse * 32768);
        finalEnd = (int32_t)sample->end + zone->endAddrsOffset + (zone->endAddrsCoarse * 32768);
        finalLoopStart = (int32_t)sample->loopStart + zone->startloopAddrsOffset + (zone->startloopAddrsCoarse * 32768);
        finalLoopEnd = (int32_t)sample->loopEnd + zone->endloopAddrsOffset + (zone->endloopAddrsCoarse * 32768);
        if (finalStart < 0) finalStart = 0;
        if (finalEnd <= finalStart) {
            goto fail;
        }
        srcFrames = (uint32_t)(finalEnd - finalStart);
        rootKey = zone_effective_root(zone, sample);
        fineCents = (double)zone->fineTune + (double)sample->pitchCorrection;
        src = (const int16_t *)(const void *)(sf2->smplData + ((uint32_t)finalStart * 2u));
        layerPcm[li] = pitch_resample_mono_linear(src,
                                                  srcFrames,
                                                  sample->sampleRate ? sample->sampleRate : 44100u,
                                                  rootKey,
                                                  mid,
                                                  dstRate,
                                                  fineCents,
                                                  &outFrames);
        if (!layerPcm[li]) {
            goto fail;
        }
        layerFrames[li] = outFrames;
        if (outFrames > maxFrames) {
            maxFrames = outFrames;
        }
        /* SF2 layer amplitude from initialAttenuation (IEEE centibels). */
        layerGain[li] = (zone->initialAttenuation <= 0)
                            ? 1.0
                            : pow(10.0, -(double)zone->initialAttenuation / 200.0);
        if (li == 0 || zone->initialAttenuation < bestAttn) {
            bestAttn = zone->initialAttenuation;
        }
        if ((zone->sampleModes & 0x1) != 0 && finalLoopEnd > finalLoopStart + 1) {
            double ratio = (double)outFrames / (double)srcFrames;
            int32_t ls = (int32_t)(((double)(finalLoopStart - finalStart) * ratio) + 0.5);
            int32_t le = (int32_t)(((double)(finalLoopEnd - finalStart) * ratio) + 0.5);
            if (ls < 0) ls = 0;
            if (le > (int32_t)outFrames) le = (int32_t)outFrames;
            if (le > ls + 1 && (loopLayer < 0 || zone->volSustainCb < zones[job->layerZoneIdx[loopLayer]].volSustainCb)) {
                loopLayer = li;
                loopStart = ls;
                loopEnd = le;
            }
        }
    }

    mix = (float *)calloc((size_t)maxFrames, sizeof(float));
    if (!mix) {
        goto fail;
    }
    for (li = 0; li < job->nLayers; ++li) {
        uint32_t n = layerFrames[li];
        double g = layerGain[li];
        for (i = 0; i < n; ++i) {
            mix[i] += (float)((double)layerPcm[li][i] * g);
        }
        free(layerPcm[li]);
        layerPcm[li] = NULL;
    }
    for (i = 0; i < maxFrames; ++i) {
        double a = fabs((double)mix[i]);
        if (a > peak) {
            peak = a;
        }
    }
    /* Leave a little headroom; split volume still applies later. */
    if (peak < 1.0) {
        peak = 1.0;
    }
    out = (int16_t *)malloc((size_t)maxFrames * sizeof(int16_t));
    if (!out) {
        free(mix);
        goto fail;
    }
    {
        double scale = 30000.0 / peak;
        for (i = 0; i < maxFrames; ++i) {
            double y = (double)mix[i] * scale;
            if (y > 32767.0) y = 32767.0;
            if (y < -32768.0) y = -32768.0;
            out[i] = (int16_t)(y + (y >= 0.0 ? 0.5 : -0.5));
        }
    }
    free(mix);

    peakAbs = pcm16_peak_abs((const uint8_t *)(const void *)out, maxFrames, 1);
    job->mixedPcm = (uint8_t *)(void *)out;
    job->mixedFrames = maxFrames;
    job->mixedSampleRate = dstRate;
    job->mixedLoopStart = (loopLayer >= 0) ? loopStart : 0;
    job->mixedLoopEnd = (loopLayer >= 0) ? loopEnd : 0;
    job->mixedSplitVolume = finalize_split_volume(cb_to_split_volume(bestAttn, attnDiv), peakAbs);
    return 0;

fail:
    for (li = 0; li < SF2HSB_MAX_LAYER_ZONES; ++li) {
        free(layerPcm[li]);
    }
    return -1;
}

static void free_melodic_jobs(MelodicEmitJob *jobs, uint32_t jobCount)
{
    uint32_t i;
    if (!jobs) {
        return;
    }
    for (i = 0; i < jobCount; ++i) {
        free(jobs[i].mixedPcm);
        jobs[i].mixedPcm = NULL;
    }
    free(jobs);
}

static int same_layer_set(int const *a, int na, int const *b, int nb)
{
    int i;
    int j;
    if (na != nb) {
        return 0;
    }
    for (i = 0; i < na; ++i) {
        int found = 0;
        for (j = 0; j < nb; ++j) {
            if (a[i] == b[j]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return 0;
        }
    }
    return 1;
}

static MelodicEmitJob *build_melodic_emit_jobs(SF2Bank const *sf2,
                                               SF2Zone const *zones,
                                               uint32_t zoneCount,
                                               int attnDiv,
                                               uint32_t *outJobCount)
{
    uint8_t kept[1024];
    int noteLayers[128][SF2HSB_MAX_LAYER_ZONES];
    int noteLayerCount[128];
    MelodicEmitJob *jobs;
    uint32_t jobCount = 0;
    uint32_t z;
    int n;

    if (!sf2 || !zones || !outJobCount) {
        return NULL;
    }
    *outJobCount = 0;
    if (zoneCount > sizeof(kept)) {
        return NULL;
    }

    memset(kept, 0, sizeof(kept));
    memset(noteLayerCount, 0, sizeof(noteLayerCount));
    for (z = 0; z < zoneCount; ++z) {
        kept[z] = (uint8_t)melodic_zone_kept(zones, zoneCount, z);
    }

    for (z = 0; z < zoneCount; ++z) {
        int lo;
        int hi;
        int note;
        if (!kept[z]) {
            continue;
        }
        lo = zones[z].loKey;
        hi = zones[z].hiKey;
        if (lo < 0) lo = 0;
        if (hi > 127) hi = 127;
        if (lo > hi) {
            int tmp = lo;
            lo = hi;
            hi = tmp;
        }
        for (note = lo; note <= hi; ++note) {
            int c = noteLayerCount[note];
            int dup = 0;
            int k;
            for (k = 0; k < c; ++k) {
                if (noteLayers[note][k] == (int)z ||
                    zones[noteLayers[note][k]].sampleIdx == zones[z].sampleIdx) {
                    /* Same sample already present for this note — keep quieter/louder by atten. */
                    if (zones[z].initialAttenuation < zones[noteLayers[note][k]].initialAttenuation) {
                        noteLayers[note][k] = (int)z;
                    }
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if (c >= SF2HSB_MAX_LAYER_ZONES) {
                /* Prefer keeping lower attenuation layers. */
                int worst = 0;
                for (k = 1; k < c; ++k) {
                    if (zones[noteLayers[note][k]].initialAttenuation >
                        zones[noteLayers[note][worst]].initialAttenuation) {
                        worst = k;
                    }
                }
                if (zones[z].initialAttenuation < zones[noteLayers[note][worst]].initialAttenuation) {
                    noteLayers[note][worst] = (int)z;
                }
                continue;
            }
            noteLayers[note][c] = (int)z;
            noteLayerCount[note] = c + 1;
        }
    }

    jobs = (MelodicEmitJob *)calloc(SF2HSB_MAX_MELODIC_JOBS, sizeof(MelodicEmitJob));
    if (!jobs) {
        return NULL;
    }

    for (n = 0; n < 128; ) {
        int lo;
        int hi;
        MelodicEmitJob *job;
        if (noteLayerCount[n] <= 0) {
            n++;
            continue;
        }
        lo = n;
        hi = n;
        while (hi + 1 < 128 &&
               same_layer_set(noteLayers[lo], noteLayerCount[lo],
                              noteLayers[hi + 1], noteLayerCount[hi + 1])) {
            hi++;
        }
        if (jobCount >= SF2HSB_MAX_MELODIC_JOBS) {
            free_melodic_jobs(jobs, jobCount);
            return NULL;
        }
        job = &jobs[jobCount++];
        job->loKey = lo;
        job->hiKey = hi;
        job->nLayers = noteLayerCount[lo];
        memcpy(job->layerZoneIdx, noteLayers[lo], (size_t)job->nLayers * sizeof(int));
        if (job->nLayers >= 2) {
            if (mix_melodic_layers_to_job(sf2, zones, job, attnDiv) != 0) {
                /* Fall back to single loudest layer. */
                int best = 0;
                int k;
                for (k = 1; k < job->nLayers; ++k) {
                    if (zones[job->layerZoneIdx[k]].initialAttenuation <
                        zones[job->layerZoneIdx[best]].initialAttenuation) {
                        best = k;
                    }
                }
                job->layerZoneIdx[0] = job->layerZoneIdx[best];
                job->nLayers = 1;
            }
        }
        n = hi + 1;
    }

    *outJobCount = jobCount;
    return jobs;
}

static BAEResult stage_instrument_ext(PendingInstrumentExt *pendingExts,
                                      uint32_t *pendingExtCount,
                                      uint32_t maxPendingExts,
                                      uint32_t instID,
                                      SF2Zone const *zone,
                                      const char *presetName,
                                      int isDrumPreset,
                                      int attnDiv)
{
    uint32_t i;
    int enableSampleAndHold;

    if (!pendingExts || !pendingExtCount || !zone) {
        return BAE_PARAM_ERR;
    }

    enableSampleAndHold = should_enable_sample_and_hold_for_zone(zone, attnDiv);

    for (i = 0; i < *pendingExtCount; ++i) {
        if (pendingExts[i].instID != instID) {
            continue;
        }

        if (prefer_zone_for_instrument_ext(zone, &pendingExts[i].zone, attnDiv)) {
            pendingExts[i].zone = *zone;
            pendingExts[i].presetName = presetName;
            pendingExts[i].isDrumPreset = isDrumPreset;
        }
        pendingExts[i].enableSampleAndHold = pendingExts[i].enableSampleAndHold || enableSampleAndHold;

        if (isDrumPreset) {
            /* For drums: prefer the most specific (narrowest key-range) zone's pan.
             * A single-note zone (loKey==hiKey) beats a wide catch-all zone.
             * Zones with equal specificity are averaged (handles velocity-layer pairs). */
            int zoneRange = zone->hiKey - zone->loKey;
            if (zoneRange < pendingExts[i].panBestRange) {
                /* More specific: reset the running average to just this zone */
                pendingExts[i].panSum = zone->pan;
                pendingExts[i].panCount = 1;
                pendingExts[i].panBestRange = zoneRange;
            } else if (zoneRange == pendingExts[i].panBestRange) {
                /* Same specificity: include in the average */
                pendingExts[i].panSum += zone->pan;
                pendingExts[i].panCount++;
            }
            /* else: wider zone, ignore for pan */
        } else {
            /* For melodic: average all zones equally (handles stereo L/R pairs). */
            pendingExts[i].panSum += zone->pan;
            pendingExts[i].panCount++;
        }
        return BAE_NO_ERROR;
    }

    if (*pendingExtCount >= maxPendingExts) {
        return BAE_MEMORY_ERR;
    }

    pendingExts[*pendingExtCount].instID = instID;
    pendingExts[*pendingExtCount].zone = *zone;
    pendingExts[*pendingExtCount].presetName = presetName;
    pendingExts[*pendingExtCount].isDrumPreset = isDrumPreset;
    pendingExts[*pendingExtCount].enableSampleAndHold = enableSampleAndHold;
    pendingExts[*pendingExtCount].panSum = zone->pan;
    pendingExts[*pendingExtCount].panCount = 1;
    pendingExts[*pendingExtCount].panBestRange = isDrumPreset ? (zone->hiKey - zone->loKey) : -1;
    (*pendingExtCount)++;
    return BAE_NO_ERROR;
}

static int sample_cache_find(CachedSampleAsset const *cache,
                             uint32_t cacheCount,
                             int sampleIdx,
                             uint32_t frameStart,
                             uint32_t frameCount,
                             uint32_t sampleRateFixed,
                             int rootKey,
                             uint32_t loopStart,
                             uint32_t loopEnd,
                             int16_t splitVolume,
                             uint64_t pcmHash,
                             uint32_t *outAssetID)
{
    uint32_t i;
    for (i = 0; i < cacheCount; ++i) {
        if (cache[i].frameCount == frameCount &&
            cache[i].sampleRateFixed == sampleRateFixed &&
            cache[i].rootKey == rootKey &&
            cache[i].loopStart == loopStart &&
            cache[i].loopEnd == loopEnd &&
            cache[i].splitVolume == splitVolume &&
            ((cache[i].sampleIdx == sampleIdx && cache[i].frameStart == frameStart) ||
             (pcmHash != 0 && cache[i].pcmHash == pcmHash))) {
            *outAssetID = cache[i].assetID;
            return 1;
        }
    }
    return 0;
}

static BAEResult sample_cache_add(CachedSampleAsset **cache,
                                  uint32_t *cacheCount,
                                  uint32_t *cacheCap,
                                  int sampleIdx,
                                  uint32_t frameStart,
                                  uint32_t frameCount,
                                  uint32_t sampleRateFixed,
                                  int rootKey,
                                  uint32_t loopStart,
                                  uint32_t loopEnd,
                                  int16_t splitVolume,
                                  uint64_t pcmHash,
                                  uint32_t assetID)
{
    CachedSampleAsset *grown;

    if (*cacheCount >= *cacheCap) {
        uint32_t nextCap = (*cacheCap == 0u) ? 256u : (*cacheCap * 2u);
        grown = (CachedSampleAsset *)realloc(*cache, nextCap * sizeof(CachedSampleAsset));
        if (!grown) {
            return BAE_MEMORY_ERR;
        }
        *cache = grown;
        *cacheCap = nextCap;
    }

    (*cache)[*cacheCount].sampleIdx = sampleIdx;
    (*cache)[*cacheCount].frameStart = frameStart;
    (*cache)[*cacheCount].frameCount = frameCount;
    (*cache)[*cacheCount].sampleRateFixed = sampleRateFixed;
    (*cache)[*cacheCount].rootKey = rootKey;
    (*cache)[*cacheCount].loopStart = loopStart;
    (*cache)[*cacheCount].loopEnd = loopEnd;
    (*cache)[*cacheCount].splitVolume = splitVolume;
    (*cache)[*cacheCount].pcmHash = pcmHash;
    (*cache)[*cacheCount].assetID = assetID;
    (*cacheCount)++;

    return BAE_NO_ERROR;
}

static BAEResult ensure_conductor_track(BAERmfEditorDocument *document)
{
    BAEResult result;
    BAERmfEditorTrackSetup setup;
    char name[] = "Conductor";

    if (!document) {
        return BAE_PARAM_ERR;
    }

    memset(&setup, 0, sizeof(setup));
    setup.channel = 0;
    setup.bank = 0;
    setup.program = 0;
    setup.name = name;

    result = BAERmfEditorDocument_AddTrack(document, &setup, NULL);
    return result;
}

static BAEResult apply_ext_info_for_zone(BAERmfEditorDocument *document,
                                         uint32_t instID,
                                         SF2Zone const *zone,
                                         const char *presetName,
                                         int isDrumPreset,
                                         int enableSampleAndHold,
                                         int extAdsr,
                                         int attnDiv,
                                         int32_t avgPan,
                                         int verbose)
{
    BAEResult result;
    BAERmfEditorInstrumentExtInfo ext;
    int32_t sustainLevel;
    int effectiveVolReleaseTc;

    int bakeKey;
    int volHoldTc;
    int volDecayTc;
    int modHoldTc;
    int modDecayTc;

    memset(&ext, 0, sizeof(ext));
    result = BAERmfEditorDocument_GetInstrumentExtInfo(document, instID, &ext);
    if (result != BAE_NO_ERROR && result != BAE_BAD_FILE) {
        return BAE_NO_ERROR;
    }

    ext.instID = instID;
    ext.displayName = (char *)presetName;
    ext.hasExtendedData = TRUE;
    sustainLevel = cb_to_level4096_env(zone->volSustainCb);
    (void)attnDiv; /* envelope sustain uses SF2 amp centibels; attnDiv is split volume only */

    bakeKey = zone_envelope_bake_key(zone, -1);
    if (isDrumPreset) {
        if (instID >= 384u && instID <= 511u) {
            bakeKey = (int)(instID - 384u);
        } else if (instID >= 128u && instID <= 255u) {
            bakeKey = (int)(instID - 128u);
        }
    }
    volHoldTc = apply_keynum_to_tc(zone->volHoldTc, zone->keynumToVolEnvHold, bakeKey);
    volDecayTc = apply_keynum_to_tc(zone->volDecayTc, zone->keynumToVolEnvDecay, bakeKey);
    modHoldTc = apply_keynum_to_tc(zone->modHoldTc, zone->keynumToModEnvHold, bakeKey);
    modDecayTc = apply_keynum_to_tc(zone->modDecayTc, zone->keynumToModEnvDecay, bakeKey);
    /*
    if (isDrumPreset) {
        ext.flags2 |= ZBF_playAtSampledFreq;
    }
    */
    if (enableSampleAndHold) {
        ext.flags1 |= ZBF_sampleAndHold;
    } else {
        ext.flags1 &= (unsigned char)~ZBF_sampleAndHold;
    }
    if ((zone->sampleModes & 0x1) != 0) {
        uint32_t reason = 0;
        bool requiresZmf = BAERmfEditorDocument_RequiresZmf(document, &reason);
#if USE_ZMF_SUPPORT == TRUE
        if (requiresZmf) {
            ext.flags2 |= ZBF_advancedInterpolation;
        } else
        {
            ext.flags2 &= (unsigned char)~ZBF_advancedInterpolation;
        }
#else
        if (requiresZmf) {
            return BAE_UNSUPPORTED_FORMAT;
        }
#endif
    }
    ext.midiRootKey = 60; /* Master root key should always be 60; individual splits handle their own rootKey */
    ext.panPlacement = (char)sf2_pan_to_inst_pan(avgPan);

    effectiveVolReleaseTc = zone->volReleaseTc;
    if (isDrumPreset && effectiveVolReleaseTc <= -12000) {
        /* Some drum zones resolve to default/no release in SF2 metadata, which maps to
         * a 1ms TERMINATE in BAE and sounds unnaturally chopped. Use decay as fallback
         * when available, otherwise apply a short practical tail (~125ms). */
        if (volDecayTc > -12000) {
            effectiveVolReleaseTc = volDecayTc;
        } else {
            effectiveVolReleaseTc = -3600;
        }
    }

    if (zone->initialFilterFc > 0) {
        ext.LPF_frequency = zone->initialFilterFc * 256 / 100;
    }
    if (zone->initialFilterQ != 0) {
        ext.LPF_resonance = zone->initialFilterQ;
    }

    /* Map SF2 reverb/chorus send (0-1000 permilles) -> BAE 0-127 */
    if (zone->reverbEffectsSend > 0) {
        int32_t rv = (127 * zone->reverbEffectsSend) / 1000;
        if (rv > 127) rv = 127;
        ext.defaultReverbSend = (int16_t)rv;
    }
    if (zone->chorusEffectsSend > 0) {
        int32_t ch = (127 * zone->chorusEffectsSend) / 1000;
        if (ch > 127) ch = 127;
        ext.defaultChorusSend = (int16_t)ch;
    }

    /* Volume ADSR mapped from SF2 to BAE stage model (VOLUME_RANGE=4096).
     * Hold/decay TCs are baked with keynumTo* at key 60 (melodic) or drum note. */
    build_adsr_from_sf2(&ext.volumeADSR,
                        zone->volDelayTc,
                        zone->volAttackTc,
                        volHoldTc,
                        volDecayTc,
                        sustainLevel,
                        effectiveVolReleaseTc,
                        extAdsr);

    if (verbose && isDrumPreset) {
        int drumNote = -1;
        if (instID >= 384u && instID <= 511u) {
            drumNote = (int)(instID - 384u);
        } else if (instID >= 128u && instID <= 255u) {
            drumNote = (int)(instID - 128u);
        }
        if (drumNote >= 0) {
            int32_t releaseUs = seconds_to_us_clamped(resolve_release_seconds_from_sf2_tc(effectiveVolReleaseTc,
                                                                                           sustainLevel,
                                                                                           extAdsr));
            if (releaseUs < 1000) {
                releaseUs = 1000;
            }
            printf("  [drum adsr] note %d relTc=%d effRelTc=%d decTc=%d sustain=%d relUs=%.3f\n",
                   drumNote,
                   zone->volReleaseTc,
                   effectiveVolReleaseTc,
                   zone->volDecayTc,
                   (int)sustainLevel,
                   (double)releaseUs / 1000000.0);
        }
    }

    /* LFOs */
    ext.lfoCount = 0;

    if (zone->vibLfoPitchCents != 0 && ext.lfoCount < BAE_EDITOR_MAX_LFOS) {
        BAERmfEditorLFOInfo *lfo = &ext.lfos[ext.lfoCount++];
        memset(lfo, 0, sizeof(*lfo));
        lfo->destination = FCC('P','I','T','C');
        lfo->period = (int32_t)(1000000.0 / (8.176 * pow(2.0, zone->vibLfoFreqCh / 1200.0)));
        if (lfo->period < 10000) lfo->period = 10000;
        if (lfo->period > 10000000) lfo->period = 10000000;
        lfo->waveShape = FCC('S','I','N','E');
        lfo->DC_feed = 0;
        lfo->level = abs(zone->vibLfoPitchCents) * 41;
        if (lfo->level < 1) lfo->level = 1;
        if (lfo->level > 524288) lfo->level = 524288;
        build_lfo_delay_adsr(&lfo->adsr, zone->vibLfoDelayTc);
    }

    if (zone->modLfoPitchCents != 0 && ext.lfoCount < BAE_EDITOR_MAX_LFOS) {
        BAERmfEditorLFOInfo *lfo = &ext.lfos[ext.lfoCount++];
        memset(lfo, 0, sizeof(*lfo));
        lfo->destination = FCC('P','I','T','C');
        lfo->period = (int32_t)(1000000.0 / (8.176 * pow(2.0, zone->modLfoFreqCh / 1200.0)));
        if (lfo->period < 10000) lfo->period = 10000;
        if (lfo->period > 10000000) lfo->period = 10000000;
        lfo->waveShape = FCC('S','I','N','E');
        lfo->DC_feed = 0;
        lfo->level = abs(zone->modLfoPitchCents) * 41;
        if (lfo->level < 1) lfo->level = 1;
        if (lfo->level > 524288) lfo->level = 524288;
        build_lfo_delay_adsr(&lfo->adsr, zone->modLfoDelayTc);
    }

    if (zone->modLfoVolCb != 0 && ext.lfoCount < BAE_EDITOR_MAX_LFOS) {
        BAERmfEditorLFOInfo *lfo = &ext.lfos[ext.lfoCount++];
        memset(lfo, 0, sizeof(*lfo));
        lfo->destination = FCC('V','O','L','U');
        lfo->period = (int32_t)(1000000.0 / (8.176 * pow(2.0, zone->modLfoFreqCh / 1200.0)));
        if (lfo->period < 10000) lfo->period = 10000;
        if (lfo->period > 10000000) lfo->period = 10000000;
        lfo->waveShape = FCC('S','I','N','E');
        lfo->DC_feed = 0;
        lfo->level = (int32_t)(65536.0 * (pow(10.0, abs(zone->modLfoVolCb) / 200.0) - 1.0));
        if (lfo->level < 1) lfo->level = 1;
        if (lfo->level > 524288) lfo->level = 524288;
        build_lfo_delay_adsr(&lfo->adsr, zone->modLfoDelayTc);
    }

    if (zone->modLfoFilterCents != 0 && ext.lfoCount < BAE_EDITOR_MAX_LFOS) {
        BAERmfEditorLFOInfo *lfo = &ext.lfos[ext.lfoCount++];
        memset(lfo, 0, sizeof(*lfo));
        lfo->destination = FCC('L','P','F','R');
        lfo->period = (int32_t)(1000000.0 / (8.176 * pow(2.0, zone->modLfoFreqCh / 1200.0)));
        if (lfo->period < 10000) lfo->period = 10000;
        if (lfo->period > 10000000) lfo->period = 10000000;
        lfo->waveShape = FCC('S','I','N','E');
        lfo->DC_feed = 0;
        lfo->level = abs(zone->modLfoFilterCents) * 41;
        if (lfo->level < 1) lfo->level = 1;
        if (lfo->level > 524288) lfo->level = 524288;
        build_lfo_delay_adsr(&lfo->adsr, zone->modLfoDelayTc);
    }

    if (zone->modEnvToPitchCents != 0 && ext.lfoCount < BAE_EDITOR_MAX_LFOS) {
        BAERmfEditorLFOInfo *lfo = &ext.lfos[ext.lfoCount++];
        int attackTc = zone->modAttackTc;
        memset(lfo, 0, sizeof(*lfo));
        lfo->destination = FCC('P','I','T','C');
        lfo->period = 0;
        lfo->waveShape = FCC('S','I','N','E');
        lfo->level = 0;
        lfo->DC_feed = zone->modEnvToPitchCents * 41;
        
        build_adsr_from_sf2(&lfo->adsr,
                            zone->modDelayTc,
                            attackTc,
                            modHoldTc,
                            modDecayTc,
                            pm_to_level4096(zone->modSustainPm),
                            zone->modReleaseTc,
                            extAdsr);
    }

    if (zone->modEnvToFilterCents != 0 && ext.lfoCount < BAE_EDITOR_MAX_LFOS) {
        BAERmfEditorLFOInfo *lfo = &ext.lfos[ext.lfoCount++];
        int attackTc = zone->modAttackTc;
        memset(lfo, 0, sizeof(*lfo));
        lfo->destination = FCC('L','P','F','R');
        lfo->period = 0;
        lfo->waveShape = FCC('S','I','N','E');
        lfo->level = 0;
        lfo->DC_feed = -zone->modEnvToFilterCents * 41;

        build_adsr_from_sf2(&lfo->adsr,
                            zone->modDelayTc,
                            attackTc,
                            modHoldTc,
                            modDecayTc,
                            pm_to_level4096(zone->modSustainPm),
                            zone->modReleaseTc,
                            extAdsr);
    }

    return BAERmfEditorDocument_SetInstrumentExtInfo(document, instID, &ext);
}

static BAEResult SF2HSB_ConvertParsedBank(BAEMixer mixer,
                                          SF2Bank *sf2Ptr,
                                          const char *outputPath,
                                          const SF2HSBConvertOptions *options,
                                          SF2HSBConvertReport *report,
                                          char *errorBuffer,
                                          size_t errorBufferSize,
                                          unsigned char **outBankData,
                                          uint32_t *outBankSize)
{
    BAERmfEditorDocument *document;
    BAEBankToken bankToken;
    SF2HSBConvertOptions localOptions;
    SF2HSBConvertReport localReport;
    BAEResult result;
    unsigned char *rmfData;
    uint32_t rmfSize;
    uint32_t i;
    uint16_t drumPresets[2];
    CachedSampleAsset *sampleCache;
    uint32_t sampleCacheCount;
    uint32_t sampleCacheCap;
    BAERmfEditorCompressionType resolvedCompression;
    int usingOpusRoundTrip;
    PendingInstrumentExt pendingExts[SF2HSB_MAX_PENDING_EXTS];
    uint32_t pendingExtCount;
    int outputIsZsb;

    memset(&localReport, 0, sizeof(localReport));
    document = NULL;
    bankToken = NULL;
    rmfData = NULL;

    if (outBankData) {
        *outBankData = NULL;
    }
    if (outBankSize) {
        *outBankSize = 0;
    }

    if (!mixer || !sf2Ptr || !options) {
        set_error(errorBuffer, errorBufferSize, "Invalid converter arguments.");
        return BAE_PARAM_ERR;
    }

    localOptions = *options;
    localOptions.conflateStereo = 1;

    if (localOptions.attnDiv <= 0) {
        /* Creative/AWE-compatible default. Use --attn-div 200 for literal SF2 amp. */
        localOptions.attnDiv = 500;
    }

    resolvedCompression = mod2rmf_encoder_resolve(&localOptions.encoderSettings);
    usingOpusRoundTrip = (localOptions.encoderSettings.codec == MOD2RMF_CODEC_OPUS) ? 1 : 0;

    if (mod2rmf_encoder_requires_zmf(localOptions.encoderSettings.codec)) {
        if (localOptions.forceHsb) {
            set_error(errorBuffer, errorBufferSize, "Selected codec requires ZSB output and cannot be used with --force-hsb.");
            return BAE_PARAM_ERR;
        }
        localOptions.forceZsb = 1;
    }

    if (localOptions.extendedAdsr && localOptions.classicAdsr) {
        set_error(errorBuffer, errorBufferSize, "--extended-adsr and --classic-adsr cannot be used together.");
        return BAE_PARAM_ERR;
    }
    if (localOptions.extendedAdsr && localOptions.forceHsb) {
        set_error(errorBuffer, errorBufferSize, "--extended-adsr cannot be used with --force-hsb.");
        return BAE_PARAM_ERR;
    }
    if (localOptions.extendedAdsr) {
        localOptions.forceZsb = 1;
    }

    if (localOptions.forceHsb && localOptions.forceZsb) {
        set_error(errorBuffer, errorBufferSize, "--force-hsb and --force-zsb cannot be used together.");
        return BAE_PARAM_ERR;
    }

    if (!localOptions.dryRun && !outputPath && !(outBankData && outBankSize)) {
        set_error(errorBuffer, errorBufferSize, "Either an output path or in-memory output buffer is required unless --dry-run is used.");
        return BAE_PARAM_ERR;
    }

    if (!localOptions.dryRun && outputPath != NULL) {
        if (!ends_with_ci(outputPath, ".hsb") && !ends_with_ci(outputPath, ".zsb")) {
            set_error(errorBuffer, errorBufferSize, "Output file must end in .hsb or .zsb.");
            return BAE_PARAM_ERR;
        }
    }

    localReport.presetCount = sf2Ptr->presetCount;

    if (localOptions.dryRun) {
        localReport.sampleCount = sf2Ptr->sampleCount;
        if (report) *report = localReport;
        SF2Bank_Free(sf2Ptr);
        return BAE_NO_ERROR;
    }

    document = BAERmfEditorDocument_New();
    if (!document) {
        set_error(errorBuffer, errorBufferSize, "Failed to create RMF authoring document.");
        SF2Bank_Free(sf2Ptr);
        return BAE_MEMORY_ERR;
    }

    (void)BAERmfEditorDocument_SetTempoBPM(document, 120);
    (void)BAERmfEditorDocument_AddTempoEvent(document, 0, 500000);
    (void)BAERmfEditorDocument_SetTicksPerQuarter(document, 480);
    (void)BAERmfEditorDocument_SetInfo(document, TITLE_INFO, "SF2 Converted Bank");
    result = ensure_conductor_track(document);
    if (result != BAE_NO_ERROR) {
        set_error(errorBuffer, errorBufferSize, "Failed to initialize document track state.");
        BAERmfEditorDocument_Delete(document);
        SF2Bank_Free(sf2Ptr);
        return result;
    }

    sampleCache = NULL;
    sampleCacheCount = 0;
    sampleCacheCap = 0;
    pendingExtCount = 0;
    outputIsZsb = 0;
    drumPresets[0] = 0xFFFFu;
    drumPresets[1] = 0xFFFFu;

    if (localOptions.forceZsb) {
        outputIsZsb = 1;
    } else if (localOptions.forceHsb) {
        outputIsZsb = 0;
    } else if (outputPath && ends_with_ci(outputPath, ".zsb")) {
        outputIsZsb = 1;
    }

    /* ZSB can hold up to 32 linear ADSR stages — use them by default to approximate
     * SF2 convex attack / lin-dB decay+release. Opt out with --classic-adsr. */
    if (outputIsZsb && !localOptions.classicAdsr) {
        localOptions.extendedAdsr = 1;
    } else if (!outputIsZsb) {
        localOptions.extendedAdsr = 0;
    }

    if (!localOptions.dryRun && outputPath != NULL) {
        if (outputIsZsb && ends_with_ci(outputPath, ".hsb")) {
            set_error(errorBuffer, errorBufferSize, "Selected ZSB output cannot be written to a .hsb path.");
            BAERmfEditorDocument_Delete(document);
            SF2Bank_Free(sf2Ptr);
            return BAE_PARAM_ERR;
        }
        if (!outputIsZsb && ends_with_ci(outputPath, ".zsb")) {
            set_error(errorBuffer, errorBufferSize, "Selected HSB output cannot be written to a .zsb path.");
            BAERmfEditorDocument_Delete(document);
            SF2Bank_Free(sf2Ptr);
            return BAE_PARAM_ERR;
        }
    }

    for (i = 0; i < sf2Ptr->presetCount; ++i) {
        SF2PresetHdr const *preset = &sf2Ptr->presets[i];
        SF2Zone *zones = NULL;
        uint32_t zoneCount = 0;
        uint32_t outInstID;
        int drumKitSlot;
        int isDrumPreset;
        uint32_t z;
        uint8_t drumNoteSeen[128];
        int bestDrumZoneForNote[128];
        int drumNote;

        memset(drumNoteSeen, 0, sizeof(drumNoteSeen));
        for (drumNote = 0; drumNote < 128; ++drumNote) {
            bestDrumZoneForNote[drumNote] = -1;
        }

        if (localOptions.verbose) {
            printf("Preset %u: %s (bank %u, program %u)\n",
                   (unsigned)i,
                   preset->name,
                   (unsigned)preset->bank,
                   (unsigned)preset->preset);
        }

        if (preset->bank != 0 && preset->bank != 1 && preset->bank != 128) {
            localReport.skippedCount++;
            continue;
        }

        if (SF2Bank_GetPresetZones(sf2Ptr, i, &zones, &zoneCount) < 0 || zoneCount == 0) {
            localReport.skippedCount++;
            free(zones);
            continue;
        }


        outInstID = sf2_preset_to_inst_id(preset->bank, preset->preset);
        drumKitSlot = -1;
        isDrumPreset = (preset->bank == 128u) ? 1 : 0;
        {
        MelodicEmitJob *melodicJobs = NULL;
        uint32_t melodicJobCount = 0;
        uint32_t emitCount;
        uint32_t emitIdx;

        if (isDrumPreset) {
            if (drumPresets[0] == preset->preset) {
                drumKitSlot = 0;
            } else if (drumPresets[1] == preset->preset) {
                drumKitSlot = 1;
            } else if (drumPresets[0] == 0xFFFFu) {
                drumPresets[0] = preset->preset;
                drumKitSlot = 0;
            } else if (drumPresets[1] == 0xFFFFu) {
                drumPresets[1] = preset->preset;
                drumKitSlot = 1;
            } else {
                if (localOptions.verbose) {
                    printf("Skipping drum preset %u: only first two SF2 drum kits are supported.\n",
                           (unsigned)preset->preset);
                }
                localReport.skippedCount++;
                free(zones);
                continue;
            }

            /* Choose one representative source zone per drum note. This prevents
             * overlapping SF2 velocity layers from stacking as duplicate hits in
             * RMF, which has no per-split velocity range fields here. */
            for (z = 0; z < zoneCount; ++z) {
                SF2Zone const *dz = &zones[z];
                int lo = dz->loKey;
                int hi = dz->hiKey;
                int n;

                if (dz->sampleIdx < 0 || (uint32_t)dz->sampleIdx >= sf2Ptr->sampleCount) {
                    continue;
                }

                if (lo < 0) lo = 0;
                if (lo > 127) lo = 127;
                if (hi < 0) hi = 0;
                if (hi > 127) hi = 127;
                if (lo > hi) {
                    int tmp = lo;
                    lo = hi;
                    hi = tmp;
                }

                for (n = lo; n <= hi; ++n) {
                    int bestIdx = bestDrumZoneForNote[n];
                    SF2Zone const *best = (bestIdx >= 0) ? &zones[bestIdx] : NULL;
                    if (prefer_zone_for_drum_note_at_key(dz,
                                                         best,
                                                         n,
                                                         sf2Ptr->samples,
                                                         sf2Ptr->sampleCount)) {
                        bestDrumZoneForNote[n] = (int)z;
                    }
                }
            }
        } else {
            melodicJobs = build_melodic_emit_jobs(sf2Ptr,
                                                  zones,
                                                  zoneCount,
                                                  localOptions.attnDiv,
                                                  &melodicJobCount);
            if (!melodicJobs && zoneCount > 0u) {
                set_error(errorBuffer, errorBufferSize, "Failed to build melodic key-split plan.");
                free(zones);
                free(sampleCache);
                BAERmfEditorDocument_Delete(document);
                SF2Bank_Free(sf2Ptr);
                return BAE_MEMORY_ERR;
            }
        }

        emitCount = isDrumPreset ? zoneCount : melodicJobCount;
        for (emitIdx = 0; emitIdx < emitCount; ++emitIdx) {
            MelodicEmitJob *job = (!isDrumPreset) ? &melodicJobs[emitIdx] : NULL;
            SF2Zone jobZoneStorage;
            SF2Zone const *zone;
            SF2Zone stageZone;
            SF2Zone const *stageZonePtr;
            SF2SampleHdr const *sample;
            SF2SampleHdr const *linkedSample;
            BAERmfEditorSampleSetup setup;
            BAERmfEditorSampleInfo editorSampleInfo;
            BAESampleInfo sampleInfo;
            uint32_t sampleIndex;
            uint32_t frameCount;
            uint32_t linkedFrameCount;
            int rootKey;
            uint32_t zoneInstID;
            uint32_t sampleRateFixed;
            uint32_t linkedSampleRateFixed;
            uint32_t assetID;
            uint32_t assetSampleIndex;
            const void *pcmData;
            const uint8_t *pcmBytes;
            const uint8_t *linkedPcmBytes;
            uint64_t pcmHash;
            uint8_t *stereoPcmData;
            BAEResult cacheResult;
            int32_t finalStart, finalEnd, finalLoopStart, finalLoopEnd;
            int32_t linkedFinalStart, linkedFinalEnd, linkedFinalLoopStart, linkedFinalLoopEnd;
            int32_t localLoopStart, localLoopEnd;
            int32_t linkedLocalLoopStart, linkedLocalLoopEnd;
            int importChannels;
            uint32_t importWaveSize;
            int useStereoPair;
            int16_t splitVolume;
            uint16_t sampleTypeBase;
            uint16_t linkedSampleTypeBase;
            int usingMixedLayers = 0;

            z = isDrumPreset ? emitIdx : (uint32_t)job->layerZoneIdx[0];
            zone = &zones[z];
            if (job) {
                int li;
                /* Prefer sustain/body layer params for shared ADSR when mixed. */
                for (li = 1; li < job->nLayers; ++li) {
                    if (prefer_zone_for_instrument_ext(&zones[job->layerZoneIdx[li]], zone, localOptions.attnDiv)) {
                        z = (uint32_t)job->layerZoneIdx[li];
                        zone = &zones[z];
                    }
                }
                jobZoneStorage = *zone;
                jobZoneStorage.loKey = job->loKey;
                jobZoneStorage.hiKey = job->hiKey;
                zone = &jobZoneStorage;
                usingMixedLayers = (job->mixedPcm != NULL && job->nLayers >= 2) ? 1 : 0;
            }
            stageZonePtr = zone;

            if (zone->sampleIdx < 0 || (uint32_t)zone->sampleIdx >= sf2Ptr->sampleCount) {
                continue;
            }

            sample = &sf2Ptr->samples[zone->sampleIdx];
            linkedSample = NULL;
            linkedPcmBytes = NULL;
            stereoPcmData = NULL;
            useStereoPair = 0;
            importChannels = 1;
            importWaveSize = 0;

            sampleTypeBase = sf2_sample_type_base(sample->sampleType);
            linkedSampleTypeBase = 0;

            if (usingMixedLayers) {
                /* Pre-pitched mono mix of overlapping SF2 layers for this key range. */
                useStereoPair = 0;
                linkedSample = NULL;
                frameCount = job->mixedFrames;
                if (frameCount == 0u) {
                    continue;
                }
                finalStart = 0;
                finalEnd = (int32_t)frameCount;
                localLoopStart = job->mixedLoopStart;
                localLoopEnd = job->mixedLoopEnd;
                if (localLoopEnd <= localLoopStart) {
                    localLoopStart = 0;
                    localLoopEnd = 0;
                }
                rootKey = job->mixedRootKey;
                sampleRateFixed = job->mixedSampleRate * 65536u;
                splitVolume = job->mixedSplitVolume;
                importChannels = 1;
                importWaveSize = frameCount * 2u;
                pcmHash = hash_pcm_fnv1a64(job->mixedPcm, (size_t)frameCount * 2u);
            } else {
            if (localOptions.conflateStereo &&
                (sampleTypeBase == 2u || sampleTypeBase == 4u) &&
                sample->sampleLink < sf2Ptr->sampleCount) {
                linkedSample = &sf2Ptr->samples[sample->sampleLink];
                linkedSampleTypeBase = sf2_sample_type_base(linkedSample->sampleType);
                if ((sampleTypeBase == 2u && linkedSampleTypeBase == 4u) ||
                    (sampleTypeBase == 4u && linkedSampleTypeBase == 2u)) {
                    if ((uint32_t)zone->sampleIdx > sample->sampleLink) {
                        /* Right-channel-first zone. Do not drop it outright, because some SF2
                         * files map key ranges asymmetrically across linked samples and this can
                         * remove specific notes (common in percussion kits). Import as mono. */
                        useStereoPair = 0;
                    } else {
                        useStereoPair = 1;
                    }
                }
            }

            finalStart = (int32_t)sample->start + zone->startAddrsOffset + (zone->startAddrsCoarse * 32768);
            finalEnd = (int32_t)sample->end + zone->endAddrsOffset + (zone->endAddrsCoarse * 32768);
            finalLoopStart = (int32_t)sample->loopStart + zone->startloopAddrsOffset + (zone->startloopAddrsCoarse * 32768);
            finalLoopEnd = (int32_t)sample->loopEnd + zone->endloopAddrsOffset + (zone->endloopAddrsCoarse * 32768);

            if (finalStart < 0) finalStart = 0;
            if (finalStart * 2u > sf2Ptr->smplSize) finalStart = sf2Ptr->smplSize / 2u;
            if (finalEnd < finalStart) finalEnd = finalStart;
            if (finalEnd * 2u > sf2Ptr->smplSize) finalEnd = sf2Ptr->smplSize / 2u;
            
            frameCount = (uint32_t)(finalEnd - finalStart);
            if (frameCount == 0) {
                continue;
            }

            localLoopStart = (finalLoopStart > finalStart) ? (finalLoopStart - finalStart) : 0;
            localLoopEnd = (finalLoopEnd > finalStart) ? (finalLoopEnd - finalStart) : 0;
            if ((zone->sampleModes & 0x1) == 0 || localLoopEnd <= localLoopStart) {
                localLoopStart = 0;
                localLoopEnd = 0;
            } else {
                if ((uint32_t)localLoopEnd > frameCount) {
                    localLoopEnd = (int32_t)frameCount;
                }
                if ((uint32_t)localLoopStart >= frameCount || localLoopEnd <= localLoopStart) {
                    localLoopStart = 0;
                    localLoopEnd = 0;
                }
            }

            if (useStereoPair) {
                linkedFinalStart = (int32_t)linkedSample->start + zone->startAddrsOffset + (zone->startAddrsCoarse * 32768);
                linkedFinalEnd = (int32_t)linkedSample->end + zone->endAddrsOffset + (zone->endAddrsCoarse * 32768);
                linkedFinalLoopStart = (int32_t)linkedSample->loopStart + zone->startloopAddrsOffset + (zone->startloopAddrsCoarse * 32768);
                linkedFinalLoopEnd = (int32_t)linkedSample->loopEnd + zone->endloopAddrsOffset + (zone->endloopAddrsCoarse * 32768);

                if (linkedFinalStart < 0) linkedFinalStart = 0;
                if (linkedFinalStart * 2u > sf2Ptr->smplSize) linkedFinalStart = sf2Ptr->smplSize / 2u;
                if (linkedFinalEnd < linkedFinalStart) linkedFinalEnd = linkedFinalStart;
                if (linkedFinalEnd * 2u > sf2Ptr->smplSize) linkedFinalEnd = sf2Ptr->smplSize / 2u;

                linkedFrameCount = (uint32_t)(linkedFinalEnd - linkedFinalStart);
                if (linkedFrameCount != frameCount || linkedFrameCount == 0u) {
                    useStereoPair = 0;
                } else {
                    linkedLocalLoopStart = (linkedFinalLoopStart > linkedFinalStart) ? (linkedFinalLoopStart - linkedFinalStart) : 0;
                    linkedLocalLoopEnd = (linkedFinalLoopEnd > linkedFinalStart) ? (linkedFinalLoopEnd - linkedFinalStart) : 0;
                    if ((zone->sampleModes & 0x1) == 0 || linkedLocalLoopEnd <= linkedLocalLoopStart) {
                        linkedLocalLoopStart = 0;
                        linkedLocalLoopEnd = 0;
                    } else {
                        if ((uint32_t)linkedLocalLoopEnd > linkedFrameCount) {
                            linkedLocalLoopEnd = (int32_t)linkedFrameCount;
                        }
                        if ((uint32_t)linkedLocalLoopStart >= linkedFrameCount || linkedLocalLoopEnd <= linkedLocalLoopStart) {
                            linkedLocalLoopStart = 0;
                            linkedLocalLoopEnd = 0;
                        }
                    }

                    if (linkedLocalLoopStart != localLoopStart || linkedLocalLoopEnd != localLoopEnd) {
                        useStereoPair = 0;
                    }
                }
            }

            if (!useStereoPair && localLoopEnd > localLoopStart) {
                pcmBytes = sf2Ptr->smplData + ((uint32_t)finalStart * 2u);
                refine_loop_points_from_pcm(pcmBytes,
                                            frameCount,
                                            &localLoopStart,
                                            &localLoopEnd);
            }

            {
                double baseRate = (double)sample->sampleRate;
                double fineTuneRatio;
                double adjustedRate;
                if (!(sample->sampleRate >= 1000u && sample->sampleRate <= 384000u)) {
                    baseRate = 44100.0;
                }
                /* Combine zone fineTune + sample pitchCorrection (both in cents) */
                fineTuneRatio = pow(2.0, ((double)zone->fineTune + (double)sample->pitchCorrection) / 1200.0);
                adjustedRate = baseRate * fineTuneRatio;
                sampleRateFixed = (uint32_t)(adjustedRate * 65536.0 + 0.5);
            }

            if (useStereoPair) {
                double linkedBaseRate = (double)linkedSample->sampleRate;
                double linkedFineTuneRatio;
                double linkedAdjustedRate;
                if (!(linkedSample->sampleRate >= 1000u && linkedSample->sampleRate <= 384000u)) {
                    linkedBaseRate = 44100.0;
                }
                linkedFineTuneRatio = pow(2.0, ((double)zone->fineTune + (double)linkedSample->pitchCorrection) / 1200.0);
                linkedAdjustedRate = linkedBaseRate * linkedFineTuneRatio;
                linkedSampleRateFixed = (uint32_t)(linkedAdjustedRate * 65536.0 + 0.5);
                if (linkedSampleRateFixed != sampleRateFixed) {
                    useStereoPair = 0;
                }
            }

            rootKey = (zone->overrideRootKey >= 0 && zone->overrideRootKey <= 127)
                      ? zone->overrideRootKey
                      : ((sample->originalPitch != 255) ? sample->originalPitch : 60);
                      
            rootKey -= zone->coarseTune;

            if (rootKey < 0) rootKey = 0;
            if (rootKey > 127) rootKey = 127;

            importChannels = useStereoPair ? 2 : 1;
            importWaveSize = frameCount * (uint32_t)(importChannels * 2);
            pcmHash = 0;
            if (useStereoPair) {
                stageZone = *zone;
                stageZone.pan = 0;
                stageZonePtr = &stageZone;
            }
            } /* !usingMixedLayers */

            /* Determine iteration strategy:
             *   - Drums: per-key split (one split per note) while preserving SF2 root/tuning pitch mapping
             *   - scaleTuning==0: per-key split, no pitch tracking (splitRootKey=n)
             *   - scaleTuning!=100: per-key split with adjusted rootKey to match SF2 pitch scaling
             *   - scaleTuning==100 (default): single split, splitRootKey = naturalRoot (rootKey)
             */
            {
            int needPerKeySplits = isDrumPreset || (zone->scaleTuning != 100);
            int zoneLo = zone->loKey;
            int zoneHi = zone->hiKey;
            int nStart;
            int nEnd;
            int n;

            if (zoneLo < 0) zoneLo = 0;
            if (zoneLo > 127) zoneLo = 127;
            if (zoneHi < 0) zoneHi = 0;
            if (zoneHi > 127) zoneHi = 127;
            if (zoneLo > zoneHi) {
                int tmp = zoneLo;
                zoneLo = zoneHi;
                zoneHi = tmp;
            }

            nStart = needPerKeySplits ? zoneLo : 0;
            nEnd   = needPerKeySplits ? zoneHi : 0;

            /* Peak-compensate split volume once per zone sample (not per key). */
            if (!usingMixedLayers) {
                int16_t attenVol = cb_to_split_volume(zone->initialAttenuation, localOptions.attnDiv);
                int32_t peakAbs = 0;
                const uint8_t *peakPcm = sf2Ptr->smplData + ((uint32_t)finalStart * 2u);
                peakAbs = pcm16_peak_abs(peakPcm, frameCount, 1);
                if (useStereoPair) {
                    int32_t linkedPeak = pcm16_peak_abs(
                        sf2Ptr->smplData + ((uint32_t)linkedFinalStart * 2u), frameCount, 1);
                    if (linkedPeak > peakAbs) {
                        peakAbs = linkedPeak;
                    }
                }
                splitVolume = finalize_split_volume(attenVol, peakAbs);
            } else {
                /* Mixed PCM is already pitched to mixedRootKey for this key span. */
                zoneLo = job->loKey;
                zoneHi = job->hiKey;
                needPerKeySplits = 0;
                nStart = 0;
                nEnd = 0;
            }

            for (n = nStart; n <= nEnd; ++n) {
            if (isDrumPreset && bestDrumZoneForNote[n] != (int)z) {
                continue;
            }
            int splitRootKey;
            if (zone->scaleTuning == 0) {
                /* No pitch tracking: each key plays at sample pitch */
                splitRootKey = n;
            } else if (zone->scaleTuning != 100) {
                /* Non-standard pitch scaling: compute per-key rootKey so that the BAE
                 * engine shift (note - rootKey) matches SF2's (note - naturalRoot) * scale/100 */
                splitRootKey = n - (int)round((double)(n - rootKey) * zone->scaleTuning / 100.0);
                if (splitRootKey < 0)   splitRootKey = 0;
                if (splitRootKey > 127) splitRootKey = 127;
            } else {
                /* Default SF2 tracking: preserve zone/sample natural root key. */
                splitRootKey = rootKey;
            }

            zoneInstID = isDrumPreset
                         ? sf2_drum_note_to_inst_id(drumKitSlot, n)
                         : outInstID;

            if (isDrumPreset) {
                drumNoteSeen[(uint8_t)n] = 1;
            }

            memset(&setup, 0, sizeof(setup));
            setup.program = (unsigned char)(zoneInstID & 0x7Fu);
            setup.rootKey = (unsigned char)splitRootKey;
            setup.lowKey = needPerKeySplits ? (unsigned char)n : (unsigned char)zoneLo;
            setup.highKey = needPerKeySplits ? (unsigned char)n : (unsigned char)zoneHi;
            setup.displayName = (char *)sample->name;

            sampleIndex = 0;
            result = BAERmfEditorDocument_AddEmptySample(document,
                                                         &setup,
                                                         &sampleIndex,
                                                         &sampleInfo);
            if (result != BAE_NO_ERROR) {
                set_error(errorBuffer, errorBufferSize, "Failed to add sample slot to document.");
                free(zones);
                free(sampleCache);
                BAERmfEditorDocument_Delete(document);
                SF2Bank_Free(sf2Ptr);
                return result;
            }

            result = BAERmfEditorDocument_SetSampleInstID(document, sampleIndex, zoneInstID);
            if (result != BAE_NO_ERROR) {
                set_error(errorBuffer, errorBufferSize, "Failed to bind sample to target instrument ID.");
                free(zones);
                free(sampleCache);
                BAERmfEditorDocument_Delete(document);
                SF2Bank_Free(sf2Ptr);
                return result;
            }

            if (!useStereoPair) {
                const uint8_t *monoPcm = sf2Ptr->smplData + ((uint32_t)finalStart * 2u);
                pcmHash = hash_pcm_fnv1a64(monoPcm, (size_t)frameCount * 2u);
            }

            if (!useStereoPair &&
                sample_cache_find(sampleCache,
                                  sampleCacheCount,
                                  zone->sampleIdx,
                                  (uint32_t)finalStart,
                                  frameCount,
                                  sampleRateFixed,
                                  rootKey,
                                  (localLoopStart > 0) ? (uint32_t)localLoopStart : 0u,
                                  (localLoopEnd > 0) ? (uint32_t)localLoopEnd : 0u,
                                  splitVolume,
                                  pcmHash,
                                  &assetID)) {
                result = BAERmfEditorDocument_SetSampleAssetForSample(document, sampleIndex, assetID);
                if (result != BAE_NO_ERROR) {
                    set_error(errorBuffer, errorBufferSize, "Failed to attach cached sample asset.");
                    free(zones);
                    free(sampleCache);
                    BAERmfEditorDocument_Delete(document);
                    SF2Bank_Free(sf2Ptr);
                    return result;
                }

                result = BAERmfEditorDocument_GetSampleAssetSampleIndex(document,
                                                                        assetID,
                                                                        0,
                                                                        &assetSampleIndex);
                if (result != BAE_NO_ERROR) {
                    set_error(errorBuffer, errorBufferSize, "Failed to resolve cached asset sample index.");
                    free(zones);
                    free(sampleCache);
                    BAERmfEditorDocument_Delete(document);
                    SF2Bank_Free(sf2Ptr);
                    return result;
                }

                result = BAERmfEditorDocument_PropagateReplacementToAsset(document,
                                                                          assetSampleIndex);
                if (result != BAE_NO_ERROR) {
                    set_error(errorBuffer, errorBufferSize, "Failed to propagate cached asset waveform data.");
                    free(zones);
                    free(sampleCache);
                    BAERmfEditorDocument_Delete(document);
                    SF2Bank_Free(sf2Ptr);
                    return result;
                }

                result = BAERmfEditorDocument_GetSampleInfo(document,
                                                            sampleIndex,
                                                            &editorSampleInfo);
                if (result != BAE_NO_ERROR) {
                    set_error(errorBuffer, errorBufferSize, "Failed to read cached sample metadata.");
                    free(zones);
                    free(sampleCache);
                    BAERmfEditorDocument_Delete(document);
                    SF2Bank_Free(sf2Ptr);
                    return result;
                }
            } else {
                if (useStereoPair) {
                    const uint8_t *leftPcm;
                    const uint8_t *rightPcm;

                    pcmBytes = sf2Ptr->smplData + ((uint32_t)finalStart * 2u);
                    linkedPcmBytes = sf2Ptr->smplData + ((uint32_t)linkedFinalStart * 2u);

                    if (sampleTypeBase == 4u) {
                        leftPcm = pcmBytes;
                        rightPcm = linkedPcmBytes;
                    } else {
                        leftPcm = linkedPcmBytes;
                        rightPcm = pcmBytes;
                    }

                    stereoPcmData = (uint8_t *)malloc(frameCount * 4u);
                    if (!stereoPcmData) {
                        set_error(errorBuffer, errorBufferSize, "Out of memory creating stereo sample buffer.");
                        free(zones);
                        free(sampleCache);
                        BAERmfEditorDocument_Delete(document);
                        SF2Bank_Free(sf2Ptr);
                        return BAE_MEMORY_ERR;
                    }

                    interleave_stereo_pcm16(leftPcm, rightPcm, frameCount, stereoPcmData);
                    pcmData = stereoPcmData;
                } else if (usingMixedLayers) {
                    pcmData = job->mixedPcm;
                } else {
                    pcmData = sf2Ptr->smplData + ((uint32_t)finalStart * 2u);
                }

                result = BAERmfEditorDocument_ReplaceSampleFromPCM(document,
                                                                   sampleIndex,
                                                                   pcmData,
                                                                   frameCount,
                                                                   16,
                                                                   importChannels,
                                                                   (BAE_UNSIGNED_FIXED)sampleRateFixed,
                                                                   (localLoopStart > 0) ? (uint32_t)localLoopStart : 0u,
                                                                   (localLoopEnd > 0) ? (uint32_t)localLoopEnd : 0u,
                                                                   &sampleInfo);
                if (stereoPcmData) {
                    free(stereoPcmData);
                    stereoPcmData = NULL;
                }
                if (result != BAE_NO_ERROR) {
                    set_error(errorBuffer, errorBufferSize, "Failed to encode SF2 PCM into bank sample.");
                    free(zones);
                    free(sampleCache);
                    BAERmfEditorDocument_Delete(document);
                    SF2Bank_Free(sf2Ptr);
                    return result;
                }

                result = BAERmfEditorDocument_GetSampleAssetIDForSample(document, sampleIndex, &assetID);
                if (result != BAE_NO_ERROR) {
                    set_error(errorBuffer, errorBufferSize, "Failed to resolve sample asset after PCM import.");
                    free(zones);
                    free(sampleCache);
                    BAERmfEditorDocument_Delete(document);
                    SF2Bank_Free(sf2Ptr);
                    return result;
                }

                if (!useStereoPair) {
                    cacheResult = sample_cache_add(&sampleCache,
                                                   &sampleCacheCount,
                                                   &sampleCacheCap,
                                                   zone->sampleIdx,
                                                   (uint32_t)finalStart,
                                                   frameCount,
                                                   sampleRateFixed,
                                                   rootKey,
                                                   (localLoopStart > 0) ? (uint32_t)localLoopStart : 0u,
                                                   (localLoopEnd > 0) ? (uint32_t)localLoopEnd : 0u,
                                                   splitVolume,
                                                   pcmHash,
                                                   assetID);
                    if (cacheResult != BAE_NO_ERROR) {
                        set_error(errorBuffer, errorBufferSize, "Out of memory building sample cache.");
                        free(zones);
                        free(sampleCache);
                        BAERmfEditorDocument_Delete(document);
                        SF2Bank_Free(sf2Ptr);
                        return cacheResult;
                    }
                }

                localReport.sampleCount++;
                if (localReport.sampleCount > MAX_SAMPLES) {
                    set_error(errorBuffer, errorBufferSize, "There are too many samples in this file to convert to HSB (Max 2048).");
                    free(zones);
                    free(sampleCache);
                    BAERmfEditorDocument_Delete(document);
                    SF2Bank_Free(sf2Ptr);
                    return BAE_TOO_MANY_SAMPLES;
                }

                result = BAERmfEditorDocument_GetSampleInfo(document, sampleIndex, &editorSampleInfo);
                if (result != BAE_NO_ERROR) {
                    set_error(errorBuffer, errorBufferSize, "Failed to inspect newly encoded sample metadata.");
                    free(zones);
                    free(sampleCache);
                    BAERmfEditorDocument_Delete(document);
                    SF2Bank_Free(sf2Ptr);
                    return result;
                }
            }

            editorSampleInfo.lowKey = needPerKeySplits ? (unsigned char)n : (unsigned char)zoneLo;
            editorSampleInfo.highKey = needPerKeySplits ? (unsigned char)n : (unsigned char)zoneHi;
            editorSampleInfo.rootKey = (unsigned char)splitRootKey;
            editorSampleInfo.program = setup.program;
            editorSampleInfo.splitVolume = splitVolume;
            editorSampleInfo.compressionType = resolvedCompression;
            editorSampleInfo.sndStorageType = BAE_EDITOR_SND_STORAGE_ESND;
            editorSampleInfo.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
            editorSampleInfo.opusRoundTripResample = usingOpusRoundTrip ? TRUE : FALSE;

            editorSampleInfo.sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)sampleRateFixed;
            editorSampleInfo.sampleInfo.bitSize = 16;
            editorSampleInfo.sampleInfo.channels = (unsigned char)importChannels;
            editorSampleInfo.sampleInfo.waveFrames = frameCount;
            editorSampleInfo.sampleInfo.waveSize = importWaveSize;
            editorSampleInfo.sampleInfo.startLoop = (localLoopStart > 0) ? (uint32_t)localLoopStart : 0u;
            editorSampleInfo.sampleInfo.endLoop = (localLoopEnd > 0) ? (uint32_t)localLoopEnd : 0u;
            if (editorSampleInfo.sampleInfo.endLoop <= editorSampleInfo.sampleInfo.startLoop || (zone->sampleModes & 0x1) == 0) {
                editorSampleInfo.sampleInfo.startLoop = 0;
                editorSampleInfo.sampleInfo.endLoop = 0;
            }

            result = BAERmfEditorDocument_SetSampleInfo(document, sampleIndex, &editorSampleInfo);
            if (result != BAE_NO_ERROR) {
                set_error(errorBuffer, errorBufferSize, "Failed to set instrument sample metadata.");
                free(zones);
                free(sampleCache);
                BAERmfEditorDocument_Delete(document);
                SF2Bank_Free(sf2Ptr);
                return result;
            }

            result = stage_instrument_ext(pendingExts,
                                          &pendingExtCount,
                                          SF2HSB_MAX_PENDING_EXTS,
                                          zoneInstID,
                                          stageZonePtr,
                                          preset->name,
                                          isDrumPreset,
                                          localOptions.attnDiv);
            if (result != BAE_NO_ERROR) {
                set_error(errorBuffer, errorBufferSize, "Failed to stage instrument ADSR/LFO extension data.");
                free(zones);
                free(sampleCache);
                BAERmfEditorDocument_Delete(document);
                SF2Bank_Free(sf2Ptr);
                return result;
            }
            } /* end per-key split loop */
            } /* end split strategy block */
        }

        if (localOptions.verbose && isDrumPreset) {
            int drumCovered = 0;
            int note;
            for (note = 0; note < 128; ++note) {
                if (drumNoteSeen[note]) {
                    drumCovered++;
                }
            }
            printf("  [drum coverage] preset %u slot %d: %d/128 notes mapped\n",
                   (unsigned)preset->preset,
                   drumKitSlot,
                   drumCovered);

            for (note = 0; note < 128; ++note) {
                int zi = bestDrumZoneForNote[note];
                if (zi >= 0) {
                    SF2Zone const *dz = &zones[zi];
                    printf("    [drum layer] note %d -> sample %d vel %u-%u key %u-%u\n",
                           note,
                           dz->sampleIdx,
                           (unsigned)dz->loVel,
                           (unsigned)dz->hiVel,
                           (unsigned)dz->loKey,
                           (unsigned)dz->hiKey);
                }
            }
        }

        free_melodic_jobs(melodicJobs, melodicJobCount);
        free(zones);
        } /* melodicJobs scope */
    }

    if (!mod2rmf_encoder_apply(document, &localOptions.encoderSettings, resolvedCompression)) {
        set_error(errorBuffer, errorBufferSize, "Failed to apply selected sample compression settings.");
        free(sampleCache);
        BAERmfEditorDocument_Delete(document);
        SF2Bank_Free(sf2Ptr);
        return BAE_GENERAL_ERR;
    }

    for (i = 0; i < pendingExtCount; ++i) {
        int32_t avgPan = (pendingExts[i].panCount > 0)
                         ? (pendingExts[i].panSum / (int32_t)pendingExts[i].panCount)
                         : 0;
        if (localOptions.verbose && avgPan != 0) {
            int16_t baePan = sf2_pan_to_inst_pan(avgPan);
            if (pendingExts[i].isDrumPreset) {
                uint32_t id = pendingExts[i].instID;
                int drumNote = (id >= 384u) ? (int)(id - 384u) : (int)(id - 128u);
                printf("  [pan] %s drum note %d: SF2 pan=%d (zones=%u, bestRange=%d) -> BAE pan=%d\n",
                       pendingExts[i].presetName ? pendingExts[i].presetName : "?",
                       drumNote, (int)avgPan,
                       (unsigned)pendingExts[i].panCount,
                       (int)pendingExts[i].panBestRange,
                       (int)baePan);
            } else {
                printf("  [pan] %s instID=%u: SF2 pan=%d (zones=%u) -> BAE pan=%d\n",
                       pendingExts[i].presetName ? pendingExts[i].presetName : "?",
                       (unsigned)pendingExts[i].instID,
                       (int)avgPan,
                       (unsigned)pendingExts[i].panCount,
                       (int)baePan);
            }
        }
        result = apply_ext_info_for_zone(document,
                                         pendingExts[i].instID,
                                         &pendingExts[i].zone,
                                         pendingExts[i].presetName,
                                         pendingExts[i].isDrumPreset,
                                         pendingExts[i].enableSampleAndHold,
                                         localOptions.extendedAdsr,
                                         localOptions.attnDiv,
                                         avgPan,
                                         localOptions.verbose);
        if (result != BAE_NO_ERROR) {
            set_error(errorBuffer, errorBufferSize, "Failed to apply staged instrument ADSR/LFO extension data.");
            free(sampleCache);
            BAERmfEditorDocument_Delete(document);
            SF2Bank_Free(sf2Ptr);
            return result;
        }
    }

    printf("Serializing authored document to %s format...\n", outputIsZsb ? "ZSB" : "HSB");
    result = BAERmfEditorDocument_SaveAsRmfToMemory(document,
                                                    outputIsZsb ? TRUE : FALSE,
                                                    &rmfData,
                                                    &rmfSize);
    if (result != BAE_NO_ERROR) {
        set_error(errorBuffer, errorBufferSize, "Failed to serialize authored document.");
        free(sampleCache);
        BAERmfEditorDocument_Delete(document);
        SF2Bank_Free(sf2Ptr);
        return result;
    }

    printf("Validating serialized bank data...\n");
    result = BAEMixer_AddBankFromMemory(mixer, rmfData, rmfSize, &bankToken);
    if (result != BAE_NO_ERROR) {
        set_error(errorBuffer, errorBufferSize, "Failed to load authored document as bank data.");
        free(sampleCache);
        BAERmfEditorDocument_Delete(document);
        XDisposePtr((XPTR)rmfData);
        SF2Bank_Free(sf2Ptr);
        return result;
    }

    if (outBankData && outBankSize) {
        *outBankData = rmfData;
        *outBankSize = rmfSize;
        rmfData = NULL;
    }
    else {
        printf("Saving serialized bank to output path...\n");
        result = BAERmfEditorBank_SaveToFile(bankToken, (BAEPathName)outputPath);
        if (result != BAE_NO_ERROR) {
            set_error(errorBuffer, errorBufferSize, "Failed to save converted bank to output path.");
            free(sampleCache);
            BAEMixer_UnloadBank(mixer, bankToken);
            BAERmfEditorDocument_Delete(document);
            XDisposePtr((XPTR)rmfData);
            SF2Bank_Free(sf2Ptr);
            return result;
        }
    }

    free(sampleCache);
    BAEMixer_UnloadBank(mixer, bankToken);
    BAERmfEditorDocument_Delete(document);
    XDisposePtr((XPTR)rmfData);
    SF2Bank_Free(sf2Ptr);

    if (report) {
        *report = localReport;
    }

    return BAE_NO_ERROR;
}

BAEResult SF2HSB_ConvertBankFile(BAEMixer mixer,
                                 const char *inputPath,
                                 const char *outputPath,
                                 const SF2HSBConvertOptions *options,
                                 SF2HSBConvertReport *report,
                                 char *errorBuffer,
                                 size_t errorBufferSize)
{
    SF2Bank sf2;

    memset(&sf2, 0, sizeof(sf2));

    if (!inputPath) {
        set_error(errorBuffer, errorBufferSize, "Invalid converter arguments.");
        return BAE_PARAM_ERR;
    }

    if (!ends_with_ci(inputPath, ".sf2")) {
        set_error(errorBuffer, errorBufferSize, "Input file must have .sf2 extension.");
        return BAE_BAD_FILE;
    }


    {
        char parseError[256];
        parseError[0] = '\0';
        if (SF2Bank_Load(inputPath, &sf2, parseError, sizeof(parseError)) < 0) {
            set_error(errorBuffer, errorBufferSize, parseError[0] ? parseError : "Failed to parse SF2 file.");
            return BAE_BAD_FILE;
        }
    }

    return SF2HSB_ConvertParsedBank(mixer,
                                    &sf2,
                                    outputPath,
                                    options,
                                    report,
                                    errorBuffer,
                                    errorBufferSize,
                                    NULL,
                                    NULL);
}

BAEResult SF2HSB_ConvertBankMemory(BAEMixer mixer,
                                   const void *inputData,
                                   size_t inputSize,
                                   const SF2HSBConvertOptions *options,
                                   SF2HSBConvertReport *report,
                                   char *errorBuffer,
                                   size_t errorBufferSize,
                                   unsigned char **outBankData,
                                   uint32_t *outBankSize)
{
    SF2Bank sf2;

    memset(&sf2, 0, sizeof(sf2));

    if (!inputData || inputSize == 0) {
        set_error(errorBuffer, errorBufferSize, "Invalid in-memory SF2 input.");
        return BAE_PARAM_ERR;
    }

    {
        char parseError[256];
        parseError[0] = '\0';
        if (SF2Bank_LoadMemory(inputData, inputSize, &sf2, parseError, sizeof(parseError)) < 0) {
            set_error(errorBuffer, errorBufferSize, parseError[0] ? parseError : "Failed to parse in-memory SF2 data.");
            return BAE_BAD_FILE;
        }
    }

    return SF2HSB_ConvertParsedBank(mixer,
                                    &sf2,
                                    NULL,
                                    options,
                                    report,
                                    errorBuffer,
                                    errorBufferSize,
                                    outBankData,
                                    outBankSize);
}
