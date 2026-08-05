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

#include "mod2rmf_common.h"
#include "mod2rmf_rmfcreat.h"
#include <math.h>
#include <string.h>


/* --- Event sorting helpers for channel spreading ------------------------ */

/* Sort pitch bend events by tick. At the same tick, center-value resets
 * (MOD2RMF_PITCH_BEND_CENTER) sort first so that a new note's initial bend
 * at the same tick overrides the previous note's center reset. */
int mod2rmf_compare_pitch_bend_by_tick(const void *a, const void *b)
{
    const ModPitchBendEvent *ea = (const ModPitchBendEvent *)a;
    const ModPitchBendEvent *eb = (const ModPitchBendEvent *)b;

    if (ea->tick != eb->tick)
    {
        return (ea->tick < eb->tick) ? -1 : 1;
    }
    /* Same tick: center resets first */
    {
        int aCenter = (ea->value == MOD2RMF_PITCH_BEND_CENTER) ? 0 : 1;
        int bCenter = (eb->value == MOD2RMF_PITCH_BEND_CENTER) ? 0 : 1;
        return aCenter - bCenter;
    }
}

/* Sort CC events by tick. */
int mod2rmf_compare_cc_by_tick(const void *a, const void *b)
{
    const ModCCEvent *ea = (const ModCCEvent *)a;
    const ModCCEvent *eb = (const ModCCEvent *)b;

    if (ea->tick != eb->tick)
    {
        return (ea->tick < eb->tick) ? -1 : 1;
    }
    return 0;
}


uint16_t mod2rmf_pitchbend_to_midi(int32_t xmpPitchbend,
                                         uint16_t bendRangeSemitones)
{
    double semitoneDelta;
    int32_t bend;

    if (bendRangeSemitones == 0)
    {
        bendRangeSemitones = MOD2RMF_PITCH_BEND_RANGE_ST;
    }

    /* libxmp pitchbend units are in cents (100 = 1 semitone). */
    semitoneDelta = (double)xmpPitchbend / 100.0;
    bend = (int32_t)(MOD2RMF_PITCH_BEND_CENTER +
                     (semitoneDelta / (double)bendRangeSemitones) *
                     (double)MOD2RMF_PITCH_BEND_CENTER);
    if (bend < 0) bend = 0;
    if (bend > 0x3FFF) bend = 0x3FFF;
    return (uint16_t)bend;
}

static int32_t mod2rmf_env_level_from_xmp(uint32_t envY)
{
    return (int32_t)(envY * VOLUME_RANGE / 64u);
}

static uint32_t mod2rmf_env_delta_to_us(uint32_t deltaTicks, double usPerTick)
{
    return (uint32_t)(deltaTicks * usPerTick + 0.5);
}

static double mod2rmf_abs_double(double value)
{
    return (value < 0.0) ? -value : value;
}

static int mod2rmf_select_env_points(const struct xmp_envelope *aei,
                                     int startIdx,
                                     int endIdx,
                                     int maxPoints,
                                     int *selected)
{
    int totalPoints;
    int count;
    int i;
    bool keep[XMP_MAX_ENV_POINTS];

    if (!aei || !selected || maxPoints <= 0 || startIdx < 0 || endIdx < startIdx)
    {
        return 0;
    }

    totalPoints = endIdx - startIdx + 1;
    if (totalPoints <= maxPoints)
    {
        for (i = 0; i < totalPoints; ++i)
        {
            selected[i] = startIdx + i;
        }
        return totalPoints;
    }

    memset(keep, 0, sizeof(keep));
    keep[startIdx] = TRUE;
    keep[endIdx] = TRUE;
    count = 2;

    while (count < maxPoints)
    {
        int segStart;
        int bestIdx;
        double bestError;

        bestIdx = -1;
        bestError = -1.0;
        segStart = startIdx;

        while (segStart < endIdx)
        {
            int segEnd;

            segEnd = segStart + 1;
            while (segEnd <= endIdx && !keep[segEnd])
            {
                ++segEnd;
            }
            if (segEnd > endIdx)
            {
                break;
            }

            if (segEnd - segStart > 1)
            {
                double x0 = (double)aei->data[segStart * 2];
                double y0 = (double)aei->data[segStart * 2 + 1];
                double x1 = (double)aei->data[segEnd * 2];
                double y1 = (double)aei->data[segEnd * 2 + 1];

                for (i = segStart + 1; i < segEnd; ++i)
                {
                    double xi = (double)aei->data[i * 2];
                    double yi = (double)aei->data[i * 2 + 1];
                    double interp;
                    double error;

                    if (x1 > x0)
                    {
                        interp = y0 + ((y1 - y0) * (xi - x0) / (x1 - x0));
                    }
                    else
                    {
                        interp = y0;
                    }

                    error = mod2rmf_abs_double(yi - interp);
                    if (error > bestError)
                    {
                        bestError = error;
                        bestIdx = i;
                    }
                }
            }

            segStart = segEnd;
        }

        if (bestIdx < 0)
        {
            break;
        }

        keep[bestIdx] = TRUE;
        ++count;
    }

    count = 0;
    for (i = startIdx; i <= endIdx; ++i)
    {
        if (keep[i])
        {
            selected[count++] = i;
        }
    }
    return count;
}

static void mod2rmf_store_adsr_stage(ModRawSample *raw,
                                     uint32_t stageIndex,
                                     uint32_t envY,
                                     uint32_t timeUs,
                                     int32_t flags)
{
    raw->adsrStages[stageIndex].level = mod2rmf_env_level_from_xmp(envY);
    raw->adsrStages[stageIndex].timeUs = (int32_t)timeUs;
    raw->adsrStages[stageIndex].flags = flags;
}

static uint32_t mod2rmf_default_release_tail_us(const struct xmp_instrument *inst,
                                                uint32_t envY,
                                                double usPerTick)
{
    uint32_t releaseUs = 50000u;

    if (inst && inst->rls > 0)
    {
        double releaseTicks = (envY > 0)
                                ? ((double)envY * 1024.0 / (double)inst->rls)
                                : 1.0;
        releaseUs = (uint32_t)(releaseTicks * usPerTick + 0.5);
    }

    if (releaseUs > 10000000u)
    {
        releaseUs = 10000000u;
    }
    return releaseUs;
}

/* Map a libxmp instrument's amplitude envelope to BAE ADSR stages.
 * We keep the most significant envelope points within BAE's 8-stage limit,
 * preserve explicit tracker release segments, and only synthesize a release
 * tail when the source envelope does not provide one. */
void mod2rmf_extract_envelope_adsr(const struct xmp_instrument *inst,
                                  uint32_t bpm,
                                  ModRawSample *raw)
{
    const struct xmp_envelope *aei;
    bool hasSustain;
    bool needsTailToZero;
    int sustainIdx;
    int npt;
    int selectedAttack[XMP_MAX_ENV_POINTS];
    int selectedRelease[XMP_MAX_ENV_POINTS];
    int attackPointCount;
    int releasePointCount;
    double usPerTick;
    uint32_t stage;
    uint32_t lastEnvY;

    if (!inst || !raw)
    {
        return;
    }

    aei = &inst->aei;
    if (!(aei->flg & XMP_ENVELOPE_ON) || aei->npt < 2)
    {
        return;
    }

    npt = aei->npt;
    if (npt > XMP_MAX_ENV_POINTS)
    {
        npt = XMP_MAX_ENV_POINTS;
    }

    raw->hasEnvelope = TRUE;
    usPerTick = 2500000.0 / (double)(bpm > 0 ? bpm : 125);
    lastEnvY = (uint32_t)aei->data[(npt - 1) * 2 + 1];

    sustainIdx = (aei->flg & XMP_ENVELOPE_SUS) ? aei->sus : -1;
    hasSustain = (sustainIdx >= 0 && sustainIdx < npt - 1) ? TRUE : FALSE;
    if (!hasSustain)
    {
        sustainIdx = npt - 1;
    }
    needsTailToZero = (lastEnvY > 0u) ? TRUE : FALSE;

    #if _DEBUG == TRUE
    {
        int dbgIdx;
        fprintf(stderr, "[mod2rmf]  ADSR extract: npt=%d flg=0x%02x sus=%d hasSustain=%d rls=%d usPerTick=%.1f\n",
                npt, aei->flg, aei->sus, hasSustain ? 1 : 0, inst->rls, usPerTick);
        for (dbgIdx = 0; dbgIdx < npt; ++dbgIdx)
        {
            fprintf(stderr, "    pt[%d]: X=%d Y=%d%s\n",
                    dbgIdx, aei->data[dbgIdx * 2], aei->data[dbgIdx * 2 + 1],
                    (hasSustain && dbgIdx == sustainIdx) ? " <sustain>" : "");
        }
    }
    #endif

    stage = 0;

    if (hasSustain)
    {
        int reservedStages = 1 + (needsTailToZero ? 1 : 0);
        int availablePointStages = MOD2RMF_MAX_ADSR_STAGES - reservedStages;
        int attackDesired = sustainIdx + 1;
        int releaseDesired = npt - sustainIdx - 1;
        int attackMin = (sustainIdx > 0) ? 2 : 1;
        int releaseMin = 1;
        int attackMax;
        int releaseMax;

        if (availablePointStages < attackMin + releaseMin)
        {
            availablePointStages = attackMin + releaseMin;
        }

        if (attackDesired + releaseDesired <= availablePointStages)
        {
            attackMax = attackDesired;
            releaseMax = releaseDesired;
        }
        else
        {
            int totalDesired = attackDesired + releaseDesired;

            attackMax = (attackDesired * availablePointStages + totalDesired / 2) / totalDesired;
            if (attackMax < attackMin)
            {
                attackMax = attackMin;
            }
            if (attackMax > attackDesired)
            {
                attackMax = attackDesired;
            }

            releaseMax = availablePointStages - attackMax;
            if (releaseMax < releaseMin)
            {
                releaseMax = releaseMin;
                attackMax = availablePointStages - releaseMax;
            }
            if (releaseMax > releaseDesired)
            {
                releaseMax = releaseDesired;
                attackMax = availablePointStages - releaseMax;
            }
            if (attackMax < attackMin)
            {
                attackMax = attackMin;
            }
        }

        attackPointCount = mod2rmf_select_env_points(aei,
                                                     0,
                                                     sustainIdx,
                                                     attackMax,
                                                     selectedAttack);
        releasePointCount = mod2rmf_select_env_points(aei,
                                                      sustainIdx + 1,
                                                      npt - 1,
                                                      releaseMax,
                                                      selectedRelease);

        if (attackPointCount <= 0 || releasePointCount <= 0)
        {
            raw->hasEnvelope = FALSE;
            raw->adsrStageCount = 0;
            return;
        }

        {
            uint32_t prevX = 0;
            int i;

            for (i = 0; i < attackPointCount && stage < MOD2RMF_MAX_ADSR_STAGES; ++i)
            {
                int pointIdx = selectedAttack[i];
                uint32_t ptX = (uint32_t)aei->data[pointIdx * 2];
                uint32_t ptY = (uint32_t)aei->data[pointIdx * 2 + 1];
                uint32_t deltaX = (ptX > prevX) ? (ptX - prevX) : 0u;

                mod2rmf_store_adsr_stage(raw,
                                         stage++,
                                         ptY,
                                         mod2rmf_env_delta_to_us(deltaX, usPerTick),
                                         ADSR_LINEAR_RAMP_LONG);
                prevX = ptX;
            }
        }

        mod2rmf_store_adsr_stage(raw,
                                 stage++,
                                 (uint32_t)aei->data[sustainIdx * 2 + 1],
                                 0u,
                                 ADSR_SUSTAIN_LONG);

        {
            uint32_t prevX = (uint32_t)aei->data[sustainIdx * 2];
            int i;

            for (i = 0; i < releasePointCount && stage < MOD2RMF_MAX_ADSR_STAGES; ++i)
            {
                int pointIdx = selectedRelease[i];
                uint32_t ptX = (uint32_t)aei->data[pointIdx * 2];
                uint32_t ptY = (uint32_t)aei->data[pointIdx * 2 + 1];
                uint32_t deltaX = (ptX > prevX) ? (ptX - prevX) : 0u;
                int32_t flags;

                if (i == 0)
                {
                    flags = ADSR_RELEASE_LONG;
                }
                else if (!needsTailToZero && i == releasePointCount - 1)
                {
                    flags = ADSR_TERMINATE_LONG;
                }
                else
                {
                    flags = ADSR_LINEAR_RAMP_LONG;
                }

                mod2rmf_store_adsr_stage(raw,
                                         stage++,
                                         ptY,
                                         mod2rmf_env_delta_to_us(deltaX, usPerTick),
                                         flags);
                prevX = ptX;
            }
        }

        if (needsTailToZero && stage < MOD2RMF_MAX_ADSR_STAGES)
        {
            mod2rmf_store_adsr_stage(raw,
                                     stage++,
                                     0u,
                                     mod2rmf_default_release_tail_us(inst, lastEnvY, usPerTick),
                                     ADSR_TERMINATE_LONG);
        }
    }
    else
    {
        int availablePointStages = MOD2RMF_MAX_ADSR_STAGES - (needsTailToZero ? 1 : 0);
        int i;
        uint32_t prevX = 0;

        attackPointCount = mod2rmf_select_env_points(aei,
                                                     0,
                                                     npt - 1,
                                                     availablePointStages,
                                                     selectedAttack);
        if (attackPointCount <= 0)
        {
            raw->hasEnvelope = FALSE;
            raw->adsrStageCount = 0;
            return;
        }

        for (i = 0; i < attackPointCount && stage < MOD2RMF_MAX_ADSR_STAGES; ++i)
        {
            int pointIdx = selectedAttack[i];
            uint32_t ptX = (uint32_t)aei->data[pointIdx * 2];
            uint32_t ptY = (uint32_t)aei->data[pointIdx * 2 + 1];
            uint32_t deltaX = (ptX > prevX) ? (ptX - prevX) : 0u;
            int32_t flags;

            if (!needsTailToZero && i == attackPointCount - 1)
            {
                flags = ADSR_TERMINATE_LONG;
            }
            else
            {
                flags = ADSR_LINEAR_RAMP_LONG;
            }

            mod2rmf_store_adsr_stage(raw,
                                     stage++,
                                     ptY,
                                     mod2rmf_env_delta_to_us(deltaX, usPerTick),
                                     flags);
            prevX = ptX;
        }

        if (needsTailToZero && stage < MOD2RMF_MAX_ADSR_STAGES)
        {
            mod2rmf_store_adsr_stage(raw,
                                     stage++,
                                     0u,
                                     mod2rmf_default_release_tail_us(inst, lastEnvY, usPerTick),
                                     ADSR_TERMINATE_LONG);
        }
    }

    raw->adsrStageCount = stage;

    #if _DEBUG == TRUE
    {
        uint32_t s;
        fprintf(stderr, "[mod2rmf]  ADSR result: %u stages\n", stage);
        for (s = 0; s < stage; ++s)
        {
            fprintf(stderr, "    stage[%u]: level=%d time=%dus flags=0x%08x\n",
                    s, raw->adsrStages[s].level, raw->adsrStages[s].timeUs,
                    raw->adsrStages[s].flags);
        }
    }
    #endif
}

/* Emulate a bidirectional (ping-pong) sample loop by appending a reversed
 * copy of the loop body (minus the two endpoints) after loopEnd.  The
 * resulting sample uses a standard forward loop that is (2*loopLen - 2)
 * frames long, producing the same audible oscillation as a true bidi loop
 * without a click at the turnaround point. */
void mod2rmf_emulate_bidi_loop(ModRawSample *raw)
{
    uint32_t loopLen;
    uint32_t reversedLen;
    uint32_t newFrameCount;
    int8_t  *newPcm;
    uint32_t dst;
    uint32_t src;

    if (!raw || !raw->pcm8 || raw->loopEnd <= raw->loopStart)
    {
        return;
    }

    loopLen = raw->loopEnd - raw->loopStart;
    if (loopLen < 3)
    {
        /* Bidi with fewer than 3 frames is identical to forward loop. */
        raw->loopType = MOD2RMF_LOOP_FORWARD;
        return;
    }

    reversedLen = loopLen - 2; /* skip both endpoints to avoid doubling */
    newFrameCount = raw->loopEnd + reversedLen;

    newPcm = (int8_t *)malloc(newFrameCount);
    if (!newPcm)
    {
        return; /* leave sample unchanged on OOM */
    }

    /* Copy everything up to loopEnd. */
    memcpy(newPcm, raw->pcm8, raw->loopEnd);

    /* Append reversed loop body, skipping the endpoint samples. */
    dst = raw->loopEnd;
    for (src = raw->loopEnd - 2; src > raw->loopStart; --src)
    {
        newPcm[dst++] = raw->pcm8[src];
    }

    free(raw->pcm8);
    raw->pcm8 = newPcm;
    raw->frameCount = newFrameCount;
    raw->loopEnd = raw->loopStart + loopLen + reversedLen; /* = loopStart + 2*loopLen - 2 */
    raw->loopType = MOD2RMF_LOOP_FORWARD;
}

/* Emulate a reverse (backwards-only) sample loop by reversing the samples
 * in the loop region in-place.  After this, a standard forward loop will
 * play the region in the originally backward direction. */
void mod2rmf_emulate_reverse_loop(ModRawSample *raw)
{
    uint32_t lo;
    uint32_t hi;
    int8_t   tmp;

    if (!raw || !raw->pcm8 || raw->loopEnd <= raw->loopStart)
    {
        return;
    }

    lo = raw->loopStart;
    hi = raw->loopEnd - 1;
    while (lo < hi)
    {
        tmp = raw->pcm8[lo];
        raw->pcm8[lo] = raw->pcm8[hi];
        raw->pcm8[hi] = tmp;
        lo++;
        hi--;
    }

    raw->loopType = MOD2RMF_LOOP_FORWARD;
}

/* Parse a row's primary and secondary effect columns for retrigger and
 * note-delay commands.  Sets *outRetrigInterval and *outDelayFrames
 * (either may remain unchanged if the effect is not present). */
void mod2rmf_parse_row_effects(const struct xmp_event *ev,
                              uint8_t *outRetrigInterval,
                              uint8_t *outDelayFrames)
{
    int col;

    if (!ev || !outRetrigInterval || !outDelayFrames)
    {
        return;
    }

    /* Check both primary (fxt/fxp) and secondary (f2t/f2p) columns. */
    for (col = 0; col < 2; ++col)
    {
        uint8_t fxType = (col == 0) ? ev->fxt : ev->f2t;
        uint8_t fxParam = (col == 0) ? ev->fxp : ev->f2p;

        if (fxType == MOD2RMF_FX_EXTENDED)
        {
            uint8_t subCmd = (fxParam >> 4) & 0x0F;
            uint8_t subVal = fxParam & 0x0F;

            if (subCmd == MOD2RMF_EX_RETRIG && subVal > 0)
            {
                *outRetrigInterval = subVal;
            }
            else if (subCmd == MOD2RMF_EX_DELAY && subVal > 0)
            {
                *outDelayFrames = subVal;
            }
        }
        else if (fxType == MOD2RMF_FX_MULTI_RETRIG)
        {
            uint8_t interval = fxParam & 0x0F;
            if (interval > 0)
            {
                *outRetrigInterval = interval;
            }
        }
    }
}

/* Decode explicit row volume-set commands (tracker volume column).
 * Returns TRUE only when the row contains an absolute volume command.
 * This allows callers to distinguish "v00" from "no volume command". */
bool mod2rmf_get_row_volume_command(const struct xmp_event *ev,
                                   uint8_t *outVol64)
{
    if (!ev || !outVol64)
    {
        return FALSE;
    }

    /* libxmp may map volume-column set to a tracker FX opcode. */
    if (ev->fxt == MOD2RMF_FX_TRK_VOL)
    {
        *outVol64 = (uint8_t)mod2rmf_clamp_int((int)ev->fxp, 0, 64);
        return TRUE;
    }
    if (ev->f2t == MOD2RMF_FX_TRK_VOL)
    {
        *outVol64 = (uint8_t)mod2rmf_clamp_int((int)ev->f2p, 0, 64);
        return TRUE;
    }

    /* libxmp volume-column set values are encoded as 1..65 for tracker
     * volumes 0..64, with 0 meaning "no volume command". */
    if (ev->vol > 0 && ev->vol <= 65)
    {
        *outVol64 = (uint8_t)(ev->vol - 1u);
        return TRUE;
    }

    return FALSE;
}

/* Return TRUE if tone portamento (effect 3xx or 5xx) is active in either
 * effect column of this row.  When tone portamento is active the note in
 * the pattern is the slide TARGET; the sample must NOT be retriggered. */
bool mod2rmf_row_has_tone_portamento(const struct xmp_event *ev)
{
    if (!ev)
    {
        return FALSE;
    }
    if (ev->fxt == MOD2RMF_FX_TONEPORTA || ev->fxt == MOD2RMF_FX_TONE_VSLIDE)
    {
        return TRUE;
    }
    if (ev->f2t == MOD2RMF_FX_TONEPORTA || ev->f2t == MOD2RMF_FX_TONE_VSLIDE)
    {
        return TRUE;
    }
    return FALSE;
}


bool mod2rmf_is_mod_family(const char *type)
{
    if (!type || !type[0])
    {
        return FALSE;
    }

    if (strstr(type, "MOD") ||
        strstr(type, "ProTracker") ||
        strstr(type, "NoiseTracker") ||
        strstr(type, "Startrekker") ||
        strstr(type, "Fast Tracker"))
    {
        return TRUE;
    }

    return FALSE;
}

bool mod2rmf_is_s3m_family(const char *type)
{
    if (!type || !type[0])
    {
        return FALSE;
    }

    /* libxmp uses "Scream Tracker 3" / variant tracker names for .s3m. */
    if (strstr(type, "Scream Tracker 3") ||
        strstr(type, "ScreamTracker 3") ||
        strstr(type, "S3M"))
    {
        return TRUE;
    }

    return FALSE;
}

bool mod2rmf_format_uses_sample_c5_rate(bool isIt, const char *type)
{
    /* IT/S3M store a true per-sample C5/C2 frequency that libxmp converts
     * into subinstrument xpo/fin relative to 8363 Hz. XM relative-note and
     * MOD finetune are musical offsets, not sample rates. */
    return isIt || mod2rmf_is_s3m_family(type);
}

uint32_t mod2rmf_c2spd_from_xpo_fin(int xpo, int fin)
{
    double cents;
    double rate;

    /* Inverse of libxmp_c2spd_to_note(): c2spd = 8363 * 2^((xpo + fin/128)/12). */
    if (fin < 0)
    {
        fin = 0;
    }
    if (fin > 127)
    {
        fin = 127;
    }

    cents = (double)xpo * 100.0 + ((double)fin * 100.0) / 128.0;
    rate = 8363.0 * pow(2.0, cents / 1200.0);
    if (rate < 100.0)
    {
        rate = 100.0;
    }
    if (rate > 768000.0)
    {
        rate = 768000.0;
    }
    return (uint32_t)(rate + 0.5);
}

int mod2rmf_rate_fin_to_cents(int fin)
{
    if (fin < 0)
    {
        fin = 0;
    }
    if (fin > 127)
    {
        fin = 127;
    }
    /* 128 fin units = 100 cents. */
    return (fin * 100 + 64) / 128;
}

uint32_t mod2rmf_fold_rate_for_fixed(uint32_t rateHz, int16_t *outRootAdjust)
{
    int16_t rootAdjust = 0;

    if (rateHz == 0u)
    {
        rateHz = 1u;
    }
    /* Each octave fold halves the stored rate and drops root by 12 so that
     * note 60 still plays at the original C5 frequency. */
    while (rateHz > MOD2RMF_MAX_FIXED_RATE_HZ)
    {
        rateHz = (rateHz + 1u) / 2u;
        rootAdjust = (int16_t)(rootAdjust - 12);
        if (rootAdjust < -96)
        {
            break;
        }
    }
    if (outRootAdjust)
    {
        *outRootAdjust = rootAdjust;
    }
    return rateHz;
}

int mod2rmf_is_ascii_space(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v');
}

void mod2rmf_trim_copy_ascii(char *dst, size_t dstSize, const char *src)
{
    size_t start;
    size_t end;
    size_t i;
    size_t out;

    if (!dst || dstSize == 0)
    {
        return;
    }
    dst[0] = '\0';
    if (!src)
    {
        return;
    }

    start = 0;
    while (src[start] && mod2rmf_is_ascii_space(src[start]))
    {
        start++;
    }

    end = start;
    while (src[end])
    {
        end++;
    }
    while (end > start && mod2rmf_is_ascii_space(src[end - 1]))
    {
        end--;
    }

    out = 0;
    for (i = start; i < end && out + 1 < dstSize; ++i)
    {
        unsigned char ch;
        ch = (unsigned char)src[i];
        if (ch < 32u)
        {
            continue;
        }
        dst[out++] = (char)ch;
    }
    dst[out] = '\0';
}

void mod2rmf_append_linef(char *dst, size_t dstSize, const char *text)
{
    size_t len;

    if (!dst || !text || !text[0] || dstSize == 0)
    {
        return;
    }

    len = strlen(dst);
    if (len >= dstSize - 1)
    {
        return;
    }

    (void)snprintf(dst + len,
                   dstSize - len,
                   "%s\n",
                   text);
}

uint32_t mod2rmf_fp_ticks_to_int(uint64_t fp)
{
    return (uint32_t)((fp + 0x8000u) >> 16);
}

unsigned char mod2rmf_vol_to_midi(unsigned char vol64)
{
    double linear;
    int midi;

    /* MOD volume 0..64 is linear amplitude.
     * MIDI volume (CC 7) is usually squared by the engine.
     * So we need to apply a square root curve to the MOD volume. */
    if (vol64 >= 64u) return 127u;
    if (vol64 == 0u) return 0u;

    linear = (double)vol64 / 64.0;
    midi = (int)(sqrt(linear) * 127.0 + 0.5);
    if (midi > 127) midi = 127;
    if (midi < 0) midi = 0;
    return (unsigned char)midi;
}

unsigned char mod2rmf_note_velocity_from_volume(unsigned char vol64)
{
    (void)vol64;
    return 127u;
}

int mod2rmf_clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void mod2rmf_compute_channel_map(const ChannelProfile profiles[],
                                uint32_t trackerCount,
                                ChannelMap *map,
                                bool avoidMidiChannel10)
{
    uint32_t i;
    uint8_t preferredMidiChannels[16] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
        10, 11, 12, 13, 14, 15
    };
    uint32_t preferredCount = 16u;
    uint32_t directAssignCount;

    if (avoidMidiChannel10)
    {
        /* Use all melodic-safe channels except MIDI ch10 (index 9). */
        static const uint8_t noCh10[15] = {
            0, 1, 2, 3, 4, 5, 6, 7, 8,
            10, 11, 12, 13, 14, 15
        };
        preferredCount = 15u;
        memcpy(preferredMidiChannels, noCh10, preferredCount * sizeof(uint8_t));
    }

    memset(map, 0, sizeof(*map));
    /* Initialize all mappings to 0xFF (unmapped) */
    memset(map->trackerToMidi, 0xFF, sizeof(map->trackerToMidi));

    /* Pass 1: direct assignment to preferred MIDI channels. */
    directAssignCount = trackerCount;
    if (directAssignCount > preferredCount)
    {
        directAssignCount = preferredCount;
    }

    for (i = 0; i < directAssignCount; ++i)
    {
        uint8_t mappedMidi = preferredMidiChannels[i];
        map->trackerToMidi[i] = mappedMidi;
        if (profiles[i].used)
        {
            map->midiChannelUsed[mappedMidi] = TRUE;
        }
    }

    /* Pass 2: overflow assignment with overlap-minimizing reuse. */
    for (i = directAssignCount; i < trackerCount; ++i)
    {
        uint8_t bestMidi = preferredMidiChannels[0];
        uint32_t bestScore = UINT32_MAX; /* lower = better */
        bool foundEmpty = FALSE;
        uint32_t prefIdx;

        if (!profiles[i].used)
        {
            /* Unused tracker channel - map to ch 0 as placeholder */
            map->trackerToMidi[i] = preferredMidiChannels[0];
            continue;
        }

        for (prefIdx = 0; prefIdx < preferredCount; ++prefIdx)
        {
            ChannelProfile agg;
            uint32_t ovlap;
            uint8_t midiCh;

            midiCh = preferredMidiChannels[prefIdx];

            memset(&agg, 0, sizeof(agg));

            if (!map->midiChannelUsed[midiCh])
            {
                /* Empty MIDI channel - best possible choice */
                bestMidi = midiCh;
                foundEmpty = TRUE;
                break;
            }

            /* Build aggregate profile for this MIDI channel */
            mod2rmf_build_midi_channel_aggregate(profiles, map->trackerToMidi,
                                         trackerCount, midiCh, &agg);

            /* Check overlap between overflow channel and aggregate */
            ovlap = mod2rmf_overlap_ticks(&profiles[i], &agg);
            free(agg.activeRanges);

            if (ovlap < bestScore)
            {
                bestScore = ovlap;
                bestMidi = midiCh;
                if (ovlap == 0) break; /* no overlap = no conflict */
            }
        }

        map->trackerToMidi[i] = bestMidi;
        map->midiChannelUsed[bestMidi] = TRUE;

        if (!foundEmpty && bestScore > 0)
        {
            #if _DEBUG == TRUE
            fprintf(stderr, "[mod2rmf] Channel map: tracker ch %u -> MIDI ch %u (overlap %u ticks)\n",
                    i, bestMidi, bestScore);
            #endif
        }
    }
}

/* Check whether any active range in profile 'a' overlaps with any in 'b'. */
bool mod2rmf_ranges_overlap(const ChannelProfile *a, const ChannelProfile *b)
{
    uint32_t i, j;
    for (i = 0; i < a->rangeCount; ++i)
    {
        for (j = 0; j < b->rangeCount; ++j)
        {
            if (a->activeRanges[i].startTick < b->activeRanges[j].endTick &&
                b->activeRanges[j].startTick < a->activeRanges[i].endTick)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* Count how many ticks of overlap exist between two channel profiles. */
uint32_t mod2rmf_overlap_ticks(const ChannelProfile *a, const ChannelProfile *b)
{
    uint32_t total = 0;
    uint32_t i, j;
    for (i = 0; i < a->rangeCount; ++i)
    {
        for (j = 0; j < b->rangeCount; ++j)
        {
            uint32_t lo = (a->activeRanges[i].startTick > b->activeRanges[j].startTick)
                          ? a->activeRanges[i].startTick : b->activeRanges[j].startTick;
            uint32_t hi = (a->activeRanges[i].endTick < b->activeRanges[j].endTick)
                          ? a->activeRanges[i].endTick : b->activeRanges[j].endTick;
            if (lo < hi) total += (hi - lo);
        }
    }
    return total;
}

const char *mod2rmf_path_basename_ptr(const char *path)
{
    const char *p;
    const char *last;

    if (!path)
    {
        return "";
    }
    last = path;
    for (p = path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            last = p + 1;
        }
    }
    return last;
}

bool mod2rmf_sample_requires_processing(const ModPlayable *playable,
                                                const Mod2RmfResamplerSettings *settings,
                                                uint32_t moduleBaseRateHz)
{
    uint32_t srcRate;

    if (!playable || !settings)
    {
        return FALSE;
    }

    if (settings->amigaFilter != MOD2RMF_AMIGA_FILTER_NONE)
    {
        return TRUE;
    }

    if (settings->targetRate == 0)
    {
        return FALSE;
    }

    srcRate = moduleBaseRateHz;
    if (playable->hasSampleRateOverride && playable->sampleRateOverrideHz > 0u)
    {
        srcRate = playable->sampleRateOverrideHz;
    }

    return settings->targetRate != srcRate;
}

/* Apply stereo separation to a raw libxmp panning value (0..255).
 * For MOD-family formats, uses the Amiga LRRL hard-panning pattern
 * scaled by the separation percentage.  For other formats, scales the
 * native panning distance from center by the separation percentage.
 * Returns an adjusted 0..255 panning value. */
uint8_t mod2rmf_apply_stereo_separation(uint8_t rawPan, uint32_t ch,
                                    bool isMod, uint8_t stereoSep)
{
    if (stereoSep == 0)
    {
        return 128u; /* mono: center */
    }

    if (isMod)
    {
        /* Amiga L-R-R-L hard-panning pattern */
        static const int amigaSide[4] = { -1, 1, 1, -1 };
        int offset = (128 * (int)stereoSep) / 100;
        int pan = 128 + amigaSide[ch % 4u] * offset;
        return (uint8_t)mod2rmf_clamp_int(pan, 0, 255);
    }

    /* Non-MOD: scale native panning around center */
    {
        int centered = (int)rawPan - 128;
        int scaled = (centered * (int)stereoSep) / 100;
        return (uint8_t)mod2rmf_clamp_int(128 + scaled, 0, 255);
    }
}