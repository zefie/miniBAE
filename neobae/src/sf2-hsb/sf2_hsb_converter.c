/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "sf2_hsb_converter.h"
#include "sf2_parser.h"

#include <BAE_API.h>
#include <X_API.h>
#include "X_Formats.h"

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

static double resolve_attack_seconds_from_sf2_tc(int attackTc)
{
    double attackSeconds = tc_to_seconds(attackTc);

    if (!(attackSeconds > 0.0)) {
        return 0.0;
    }
    if (attackSeconds <= 0.050) {
        return 0.0;
    }
    if (attackSeconds <= 0.200) {
        return attackSeconds * 0.35;
    }

    return attackSeconds;
}

static double resolve_decay_seconds_from_sf2_tc(int decayTc, int32_t sustainLevel)
{
    double decaySeconds = tc_to_seconds(decayTc);

    if (!(decaySeconds > 0.0)) {
        return 0.0;
    }
    if (sustainLevel <= 4) {
        return approximate_linear_amp_decay_seconds(decaySeconds);
    }

    return decaySeconds;
}

static double resolve_release_seconds_from_sf2_tc(int releaseTc, int32_t sustainLevel)
{
    double releaseSeconds = tc_to_seconds(releaseTc);

    if (!(releaseSeconds > 0.0)) {
        return 0.0;
    }
    if (sustainLevel <= 4) {
        return approximate_linear_amp_decay_seconds(releaseSeconds);
    }

    return releaseSeconds;
}

static int32_t cb_to_level4096(int cb)
{
    double ratio;
    int32_t level;

    if (cb >= 96000) return 0;
    if (cb <= 0) return SF2HSB_VOLUME_RANGE;

    // Use /400 consistent with cb_to_split_volume so ADSR sustain level
    // and split volume use the same compressed dB scale.
    ratio = pow(10.0, -cb / 400.0);
    level = (int32_t)((double)SF2HSB_VOLUME_RANGE * ratio);

    if (level < 0) level = 0;
    if (level > SF2HSB_VOLUME_RANGE) level = SF2HSB_VOLUME_RANGE;
    return level;
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

/* Add an 8-segment piecewise linear attack approximation to an ADSR stage array.
 * SF2 volume attack is convex in amplitude (fast initial rise, slowing near peak),
 * modelled as level(t) = peak * (1 - exp(-4*t/T)), normalised so segment 8 = peak.
 * Falls back to a single linear segment when there are fewer than 8 free slots
 * or the attack time is too short to bother subdividing.
 * Returns the new stage index.
 */
static uint32_t adsr_push_attack_piecewise(BAERmfEditorADSRInfo *adsr,
                                           uint32_t st,
                                           int32_t targetLevel,
                                           int32_t attackUs)
{
    static const int N = 8;
    static const double k = 4.0;
    int i;
    if (attackUs <= 0) {
        if (st < BAE_EDITOR_MAX_ADSR_STAGES) {
            adsr->stages[st].level = targetLevel;
            adsr->stages[st].time = 0;
            adsr->stages[st].flags = FCC('L','I','N','E');
            st++;
        }
        return st;
    }
    if ((int32_t)(st + (uint32_t)N) > BAE_EDITOR_MAX_ADSR_STAGES || attackUs < N * 1000) {
        /* Not enough room or attack too short: single linear ramp */
        if (st < BAE_EDITOR_MAX_ADSR_STAGES) {
            adsr->stages[st].level = targetLevel;
            adsr->stages[st].time = attackUs;
            adsr->stages[st].flags = FCC('L','I','N','E');
            st++;
        }
        return st;
    }
    {
        /* Normalisation constant so that i==N yields exactly targetLevel */
        double norm = 1.0 - exp(-k);
        int32_t segTime = attackUs / N;
        int32_t lastTime = attackUs - segTime * (N - 1);
        for (i = 1; i <= N; i++) {
            double t = (double)i / (double)N;
            int32_t level = (i == N) ? targetLevel
                          : (int32_t)((double)targetLevel * (1.0 - exp(-k * t)) / norm + 0.5);
            int32_t time  = (i == N) ? lastTime : segTime;
            adsr->stages[st].level = level;
            adsr->stages[st].time  = time;
            adsr->stages[st].flags = FCC('L','I','N','E');
            st++;
        }
    }
    return st;
}

/* Add an 8-segment piecewise linear decay/release approximation to an ADSR stage array.
 * SF2 decay/release is exponential (fast initial drop, slower approach to target),
 * modelled as level(t) = endLevel + range * exp(-4*t/T).
 * The last segment always carries finalFlags (LINE for decay, LAST for release).
 * Falls back to a single linear segment when there are fewer than 8 free slots
 * or the decay time is too short.
 * Returns the new stage index.
 */
static uint32_t adsr_push_decay_piecewise(BAERmfEditorADSRInfo *adsr,
                                          uint32_t st,
                                          int32_t startLevel,
                                          int32_t endLevel,
                                          int32_t decayUs,
                                          int32_t finalFlags)
{
    static const int N = 8;
    static const double k = 4.0;
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
    if ((int32_t)(st + (uint32_t)N) > BAE_EDITOR_MAX_ADSR_STAGES || decayUs < N * 1000) {
        /* Not enough room or decay too short: single linear ramp */
        if (st < BAE_EDITOR_MAX_ADSR_STAGES) {
            adsr->stages[st].level = endLevel;
            adsr->stages[st].time = decayUs;
            adsr->stages[st].flags = finalFlags;
            st++;
        }
        return st;
    }
    {
        int32_t range    = startLevel - endLevel;
        int32_t segTime  = decayUs / N;
        int32_t lastTime = decayUs - segTime * (N - 1);
        for (i = 1; i <= N; i++) {
            double t = (double)i / (double)N;
            int32_t level = (i == N) ? endLevel
                          : (int32_t)((double)endLevel + (double)range * exp(-k * t) + 0.5);
            int32_t time  = (i == N) ? lastTime : segTime;
            int32_t flags = (i == N) ? finalFlags : FCC('L','I','N','E');
            adsr->stages[st].level = level;
            adsr->stages[st].time  = time;
            adsr->stages[st].flags = flags;
            st++;
        }
    }
    return st;
}

/* Build BAE ADSR stages from SF2 envelope parameters.
 * extAdsr: when non-zero, use 8-segment piecewise linear approximation for
 *          attack, decay, and release to emulate the SF2 exponential curves.
 *          Requires --extended-adsr / --ext-adsr CLI flag (implies ZSB output).
 *          When zero, single linear segments are used (classic HSB behaviour).
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
    int32_t attackUs = seconds_to_us_clamped(resolve_attack_seconds_from_sf2_tc(attackTc));
    int32_t holdUs = tc_to_us(holdTc);
    int32_t decayUs = seconds_to_us_clamped(resolve_decay_seconds_from_sf2_tc(decayTc, sustainLevel));
    int32_t releaseUs = seconds_to_us_clamped(resolve_release_seconds_from_sf2_tc(releaseTc, sustainLevel));
    int32_t delayUs = tc_to_us(delayTc);

    memset(adsr, 0, sizeof(*adsr));

    const int maxAdsrStages = (extAdsr) ? BAE_EDITOR_MAX_ADSR_STAGES : 8;

    /* Delay: always a single linear stage (delay is linear in SF2 too) */
    if (delayUs > 0 && st < maxAdsrStages) {
        adsr->stages[st].level = 0;
        adsr->stages[st].time = delayUs;
        adsr->stages[st].flags = FCC('L','I','N','E');
        st++;
    }

    /* Attack */
    if (extAdsr) {
        st = adsr_push_attack_piecewise(adsr, st, SF2HSB_VOLUME_RANGE, attackUs);
    } else {
        if (st < maxAdsrStages) {
            adsr->stages[st].level = SF2HSB_VOLUME_RANGE;
            adsr->stages[st].time = attackUs;
            adsr->stages[st].flags = FCC('L','I','N','E');
            st++;
        }
    }

    /* Hold: always a single flat stage */
    if (holdUs > 0 && st < maxAdsrStages) {
        adsr->stages[st].level = SF2HSB_VOLUME_RANGE;
        adsr->stages[st].time = holdUs;
        adsr->stages[st].flags = FCC('L','I','N','E');
        st++;
    }

    /* Decay */
    if (extAdsr) {
        st = adsr_push_decay_piecewise(adsr, st, SF2HSB_VOLUME_RANGE, sustainLevel, decayUs, FCC('L','I','N','E'));
    } else {
        if (st < maxAdsrStages) {
            adsr->stages[st].level = sustainLevel;
            adsr->stages[st].time = decayUs;
            adsr->stages[st].flags = FCC('L','I','N','E');
            st++;
        }
    }

    /* Sustain hold point */
    if (st < maxAdsrStages) {
        adsr->stages[st].level = sustainLevel;
        adsr->stages[st].time = 0;
        adsr->stages[st].flags = FCC('S','U','S','T');
        st++;
    }

    if (releaseUs < 1000) {
        releaseUs = 1000;
    }

    /* Release */
    if (extAdsr) {
        st = adsr_push_decay_piecewise(adsr, st, sustainLevel, 0, releaseUs, FCC('L','A','S','T'));
    } else {
        if (st < maxAdsrStages) {
            adsr->stages[st].level = 0;
            adsr->stages[st].time = releaseUs;
            adsr->stages[st].flags = FCC('L','A','S','T');
            st++;
        }
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

static int16_t cb_to_split_volume(int cb)
{
    // initialAttenuation is in centibels (0.1 dB) where 0 = no attenuation (full volume).
    // BAE uses miscParameter2 = 100 as unity gain: Volume = (Volume * miscParameter2) / 100.
    // Use /400 (half-dB) instead of spec-exact /200 because BAE's integer mixing pipeline
    // has a narrower effective dynamic range than a floating-point SF2 engine; the /200
    // curve makes anything above ~200cb (20 dB) effectively inaudible.
    int32_t v;
    if (cb <= 0) return 100;
    v = (int32_t)(100.0 * pow(10.0, -cb / 400.0) + 0.5);
    if (v < 1) v = 1;
    if (v > 127) v = 127;
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
    if (drumKitSlot <= 0) {
        if (note < 0) note = 0;
        if (note > 125) note = 125;
        return 128u + (uint32_t)note;   /* 128-253 */
    }

    if (note < 0) note = 0;
    if (note > 127) note = 127;
    return 384u + (uint32_t)note;       /* 384-511 */
}

typedef struct {
    int sampleIdx;
    uint32_t frameStart;
    uint32_t frameCount;
    uint32_t sampleRateFixed;
    int rootKey;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint32_t assetID;
} CachedSampleAsset;

typedef struct {
    uint32_t instID;
    SF2Zone zone;
    const char *presetName;
    int isDrumPreset;
    int enableSampleAndHold;
} PendingInstrumentExt;

static int should_enable_sample_and_hold_for_zone(SF2Zone const *zone)
{
    int32_t sustainLevel;

    if (!zone) {
        return 0;
    }

    sustainLevel = cb_to_level4096(zone->volSustainCb);
    return ((zone->sampleModes & 0x1) != 0) &&
           (zone->volSustainCb <= 0 || sustainLevel > (SF2HSB_VOLUME_RANGE / 100));
}

static int prefer_zone_for_instrument_ext(SF2Zone const *candidate,
                                          SF2Zone const *current)
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

    candidateSustain = cb_to_level4096(candidate->volSustainCb);
    currentSustain = cb_to_level4096(current->volSustainCb);
    if (candidateSustain != currentSustain) {
        return candidateSustain > currentSustain;
    }

    candidateHold = tc_to_seconds(candidate->volHoldTc);
    currentHold = tc_to_seconds(current->volHoldTc);
    if (candidateHold != currentHold) {
        return candidateHold > currentHold;
    }

    candidateDecay = resolve_decay_seconds_from_sf2_tc(candidate->volDecayTc, candidateSustain);
    currentDecay = resolve_decay_seconds_from_sf2_tc(current->volDecayTc, currentSustain);
    if (candidateDecay != currentDecay) {
        return candidateDecay > currentDecay;
    }

    candidateRelease = resolve_release_seconds_from_sf2_tc(candidate->volReleaseTc, candidateSustain);
    currentRelease = resolve_release_seconds_from_sf2_tc(current->volReleaseTc, currentSustain);
    return candidateRelease > currentRelease;
}

static BAEResult stage_instrument_ext(PendingInstrumentExt *pendingExts,
                                      uint32_t *pendingExtCount,
                                      uint32_t maxPendingExts,
                                      uint32_t instID,
                                      SF2Zone const *zone,
                                      const char *presetName,
                                      int isDrumPreset)
{
    uint32_t i;
    int enableSampleAndHold;

    if (!pendingExts || !pendingExtCount || !zone) {
        return BAE_PARAM_ERR;
    }

    enableSampleAndHold = should_enable_sample_and_hold_for_zone(zone);

    for (i = 0; i < *pendingExtCount; ++i) {
        if (pendingExts[i].instID != instID) {
            continue;
        }

        if (prefer_zone_for_instrument_ext(zone, &pendingExts[i].zone)) {
            pendingExts[i].zone = *zone;
            pendingExts[i].presetName = presetName;
            pendingExts[i].isDrumPreset = isDrumPreset;
        }
        pendingExts[i].enableSampleAndHold = pendingExts[i].enableSampleAndHold || enableSampleAndHold;
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
                             uint32_t *outAssetID)
{
    uint32_t i;
    for (i = 0; i < cacheCount; ++i) {
        if (cache[i].sampleIdx == sampleIdx &&
            cache[i].frameStart == frameStart &&
            cache[i].frameCount == frameCount &&
            cache[i].sampleRateFixed == sampleRateFixed &&
            cache[i].rootKey == rootKey &&
            cache[i].loopStart == loopStart &&
            cache[i].loopEnd == loopEnd) {
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
                                         int extAdsr)
{
    BAEResult result;
    BAERmfEditorInstrumentExtInfo ext;
    int32_t sustainLevel;

    memset(&ext, 0, sizeof(ext));
    result = BAERmfEditorDocument_GetInstrumentExtInfo(document, instID, &ext);
    if (result != BAE_NO_ERROR && result != BAE_BAD_FILE) {
        return BAE_NO_ERROR;
    }

    ext.instID = instID;
    ext.displayName = (char *)presetName;
    ext.hasExtendedData = TRUE;
    sustainLevel = cb_to_level4096(zone->volSustainCb);
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
        if (BAERmfEditorDocument_RequiresZmf(document, &reason)) {
            ext.flags2 |= ZBF_advancedInterpolation;
        } else {
            ext.flags2 &= (unsigned char)~ZBF_advancedInterpolation;
        }
    }
    ext.midiRootKey = 60; /* Master root key should always be 60; individual splits handle their own rootKey */
    ext.panPlacement = (char)sf2_pan_to_inst_pan(zone->pan);
    ext.miscParameter2 = cb_to_split_volume(zone->initialAttenuation);

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

    /* Volume ADSR mapped from SF2 to BAE stage model (VOLUME_RANGE=4096). */
    build_adsr_from_sf2(&ext.volumeADSR,
                        zone->volDelayTc,
                        zone->volAttackTc,
                        zone->volHoldTc,
                        zone->volDecayTc,
                        sustainLevel,
                        zone->volReleaseTc,
                        extAdsr);

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
                            zone->modHoldTc,
                            zone->modDecayTc,
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
                            zone->modHoldTc,
                            zone->modDecayTc,
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
    uint16_t firstDrumPreset;
    uint16_t secondDrumPreset;
    CachedSampleAsset *sampleCache;
    uint32_t sampleCacheCount;
    uint32_t sampleCacheCap;
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
    firstDrumPreset = 0xFFFFu;
    secondDrumPreset = 0xFFFFu;

    if (localOptions.forceZsb) {
        outputIsZsb = 1;
    } else if (localOptions.forceHsb) {
        outputIsZsb = 0;
    } else if (outputPath && ends_with_ci(outputPath, ".zsb")) {
        outputIsZsb = 1;
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

        if (isDrumPreset) {
            if (firstDrumPreset == preset->preset) {
                drumKitSlot = 0;
            } else if (secondDrumPreset == preset->preset) {
                drumKitSlot = 1;
            } else if (firstDrumPreset == 0xFFFFu) {
                firstDrumPreset = preset->preset;
                drumKitSlot = 0;
            } else if (secondDrumPreset == 0xFFFFu) {
                secondDrumPreset = preset->preset;
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
        }

        for (z = 0; z < zoneCount; ++z) {
            SF2Zone const *zone = &zones[z];
            SF2SampleHdr const *sample;
            BAERmfEditorSampleSetup setup;
            BAERmfEditorSampleInfo editorSampleInfo;
            BAESampleInfo sampleInfo;
            uint32_t sampleIndex;
            uint32_t frameCount;
            int rootKey;
            uint32_t zoneInstID;
            uint32_t sampleRateFixed;
            uint32_t assetID;
            uint32_t assetSampleIndex;
            const void *pcmData;
            const uint8_t *pcmBytes;
            BAEResult cacheResult;
            int32_t finalStart, finalEnd, finalLoopStart, finalLoopEnd;
            int32_t localLoopStart, localLoopEnd;

            if (zone->sampleIdx < 0 || (uint32_t)zone->sampleIdx >= sf2Ptr->sampleCount) {
                continue;
            }

            sample = &sf2Ptr->samples[zone->sampleIdx];

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

            if (localLoopEnd > localLoopStart) {
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

            rootKey = (zone->overrideRootKey >= 0 && zone->overrideRootKey <= 127)
                      ? zone->overrideRootKey
                      : ((sample->originalPitch != 255) ? sample->originalPitch : 60);
                      
            rootKey -= zone->coarseTune;

            if (rootKey < 0) rootKey = 0;
            if (rootKey > 127) rootKey = 127;

            /* Determine iteration strategy:
             *   - Drums: per-key split, each key plays sample at its natural pitch (splitRootKey=n)
             *   - scaleTuning==0: per-key split, no pitch tracking (splitRootKey=n)
             *   - scaleTuning!=100: per-key split with adjusted rootKey to match SF2 pitch scaling
             *   - scaleTuning==100 (default): single split, splitRootKey = naturalRoot (rootKey)
             */
            {
            int needPerKeySplits = isDrumPreset || (zone->scaleTuning != 100);
            int nStart = needPerKeySplits ? zone->loKey : 0;
            int nEnd   = needPerKeySplits ? zone->hiKey : 0;
            int n;
            for (n = nStart; n <= nEnd; ++n) {
            int splitRootKey;
            if (isDrumPreset || zone->scaleTuning == 0) {
                /* Each key plays the sample at its natural pitch — no pitch shift */
                splitRootKey = n;
            } else if (zone->scaleTuning != 100) {
                /* Non-standard pitch scaling: compute per-key rootKey so that the BAE
                 * engine shift (note - rootKey) matches SF2's (note - naturalRoot) * scale/100 */
                splitRootKey = n - (int)round((double)(n - rootKey) * zone->scaleTuning / 100.0);
                if (splitRootKey < 0)   splitRootKey = 0;
                if (splitRootKey > 127) splitRootKey = 127;
            } else {
                splitRootKey = rootKey;
            }

            zoneInstID = isDrumPreset
                         ? sf2_drum_note_to_inst_id(drumKitSlot, n)
                         : outInstID;

            memset(&setup, 0, sizeof(setup));
            setup.program = (unsigned char)(zoneInstID & 0x7Fu);
            setup.rootKey = (unsigned char)splitRootKey;
            setup.lowKey = needPerKeySplits ? (unsigned char)n : (unsigned char)zone->loKey;
            setup.highKey = needPerKeySplits ? (unsigned char)n : (unsigned char)zone->hiKey;
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

            if (sample_cache_find(sampleCache,
                                  sampleCacheCount,
                                  zone->sampleIdx,
                                  (uint32_t)finalStart,
                                  frameCount,
                                  sampleRateFixed,
                                  rootKey,
                                  (localLoopStart > 0) ? (uint32_t)localLoopStart : 0u,
                                  (localLoopEnd > 0) ? (uint32_t)localLoopEnd : 0u,
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
                pcmData = sf2Ptr->smplData + ((uint32_t)finalStart * 2u);
                result = BAERmfEditorDocument_ReplaceSampleFromPCM(document,
                                                                   sampleIndex,
                                                                   pcmData,
                                                                   frameCount,
                                                                   16,
                                                                   1,
                                                                   (BAE_UNSIGNED_FIXED)sampleRateFixed,
                                                                   (localLoopStart > 0) ? (uint32_t)localLoopStart : 0u,
                                                                   (localLoopEnd > 0) ? (uint32_t)localLoopEnd : 0u,
                                                                   &sampleInfo);
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
                                               assetID);
                if (cacheResult != BAE_NO_ERROR) {
                    set_error(errorBuffer, errorBufferSize, "Out of memory building sample cache.");
                    free(zones);
                    free(sampleCache);
                    BAERmfEditorDocument_Delete(document);
                    SF2Bank_Free(sf2Ptr);
                    return cacheResult;
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

            editorSampleInfo.lowKey = needPerKeySplits ? (unsigned char)n : (unsigned char)zone->loKey;
            editorSampleInfo.highKey = needPerKeySplits ? (unsigned char)n : (unsigned char)zone->hiKey;
            editorSampleInfo.rootKey = (unsigned char)splitRootKey;
            editorSampleInfo.program = setup.program;
            editorSampleInfo.splitVolume = cb_to_split_volume(zone->initialAttenuation);
            editorSampleInfo.compressionType = BAE_EDITOR_COMPRESSION_PCM;
            editorSampleInfo.sndStorageType = BAE_EDITOR_SND_STORAGE_ESND;
            editorSampleInfo.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
            editorSampleInfo.opusRoundTripResample = FALSE;

            editorSampleInfo.sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)sampleRateFixed;
            editorSampleInfo.sampleInfo.bitSize = 16;
            editorSampleInfo.sampleInfo.channels = 1;
            editorSampleInfo.sampleInfo.waveFrames = frameCount;
            editorSampleInfo.sampleInfo.waveSize = frameCount * 2u;
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
                                          zone,
                                          preset->name,
                                          isDrumPreset);
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

        free(zones);
    }

    for (i = 0; i < pendingExtCount; ++i) {
        result = apply_ext_info_for_zone(document,
                                         pendingExts[i].instID,
                                         &pendingExts[i].zone,
                                         pendingExts[i].presetName,
                                         pendingExts[i].isDrumPreset,
                                         pendingExts[i].enableSampleAndHold,
                                         localOptions.extendedAdsr);
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
