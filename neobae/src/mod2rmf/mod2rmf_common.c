#include "mod2rmf_common.h"
#include "mod2rmf_rmfcreat.h"


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

/* Map a libxmp instrument's amplitude envelope directly to BAE ADSR stages.
 * Each pair of consecutive envelope points becomes one LINEAR_RAMP stage,
 * preserving the multi-segment shape.  BAE supports up to 8 stages; with
 * sustain + terminate that leaves room for up to 6 envelope segments (7 points).
 *
 * Envelope x-coordinates are in ticks (one per tracker frame), converted
 * to microseconds via (2500/bpm)*1000.  y-coordinates are 0..64, scaled to
 * 0..VOLUME_RANGE for BAE. */
void mod2rmf_extract_envelope_adsr(const struct xmp_instrument *inst,
                                  uint32_t bpm,
                                  ModRawSample *raw)
{
    const struct xmp_envelope *aei;
    int sustainIdx;
    int npt;
    int idx;
    double usPerTick; /* microseconds per envelope tick */
    uint32_t stage;
    /* Max segments before sustain+terminate = 8-2 = 6, needing up to 7 points */
    int maxSegments;

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

    /* Find sustain point (or last point if no sustain flag). */
    sustainIdx = (aei->flg & XMP_ENVELOPE_SUS) ? aei->sus : npt - 1;
    if (sustainIdx < 0)
    {
        sustainIdx = 0;
    }
    if (sustainIdx >= npt)
    {
        sustainIdx = npt - 1;
    }

    #ifdef _DEBUG
    {
        int dbgIdx;
        fprintf(stderr, "[mod2rmf]  ADSR extract: npt=%d flg=0x%02x sus=%d rls=%d usPerTick=%.1f\n",
                npt, aei->flg, aei->sus, inst->rls, usPerTick);
        for (dbgIdx = 0; dbgIdx < npt; ++dbgIdx)
        {
            fprintf(stderr, "    pt[%d]: X=%d Y=%d%s\n",
                    dbgIdx, aei->data[dbgIdx * 2], aei->data[dbgIdx * 2 + 1],
                    (dbgIdx == sustainIdx) ? " <sustain>" : "");
        }
    }
    #endif

    /* Map each envelope segment (pair of consecutive points) up to and including
     * the sustain point as a LINEAR_RAMP stage.  Reserve 2 stages for
     * sustain-hold and terminate. */
    maxSegments = MOD2RMF_MAX_ADSR_STAGES - 2;
    stage = 0;

    /* If the first envelope point is not at Y=0 and X=0, insert an initial
     * ramp to the first point's level (the envelope starts at pt[0] level). */
    for (idx = 0; idx <= sustainIdx && (int)stage < maxSegments; ++idx)
    {
        uint32_t ptX = (uint32_t)aei->data[idx * 2];
        uint32_t ptY = (uint32_t)aei->data[idx * 2 + 1];
        uint32_t prevX = (idx > 0) ? (uint32_t)aei->data[(idx - 1) * 2] : 0;
        uint32_t deltaX = ptX - prevX;

        raw->adsrStages[stage].level = (int32_t)(ptY * VOLUME_RANGE / 64u);
        raw->adsrStages[stage].timeUs = (int32_t)(deltaX * usPerTick + 0.5);
        raw->adsrStages[stage].flags = ADSR_LINEAR_RAMP_LONG;
        stage++;
    }

    /* Sustain-hold stage at the sustain point's level */
    {
        uint32_t susY = (uint32_t)aei->data[sustainIdx * 2 + 1];
        raw->adsrStages[stage].level = (int32_t)(susY * VOLUME_RANGE / 64u);
        raw->adsrStages[stage].timeUs = 0;
        raw->adsrStages[stage].flags = ADSR_SUSTAIN_LONG;
        stage++;
    }

    /* Terminate/release stage */
    {
        uint32_t susY = (uint32_t)aei->data[sustainIdx * 2 + 1];
        uint32_t releaseUs = 50000; /* 50ms default */

        if (inst->rls > 0)
        {
            /* rls is fadeout per tick (XM: 0..65535).  Time to fade from
             * sustain amplitude to silence = susY / rls * 1024 ticks. */
            double releaseTicks = (susY > 0)
                                    ? ((double)susY * 1024.0 / (double)inst->rls)
                                    : 1.0;
            releaseUs = (uint32_t)(releaseTicks * usPerTick + 0.5);
        }
        else if (sustainIdx + 1 < npt)
        {
            uint32_t lastX = (uint32_t)aei->data[(npt - 1) * 2];
            uint32_t susX = (uint32_t)aei->data[sustainIdx * 2];
            releaseUs = (lastX > susX)
                          ? (uint32_t)((lastX - susX) * usPerTick + 0.5)
                          : 50000;
        }

        if (releaseUs > 10000000u) /* 10 second cap */
        {
            releaseUs = 10000000u;
        }

        raw->adsrStages[stage].level = 0;
        raw->adsrStages[stage].timeUs = (int32_t)releaseUs;
        raw->adsrStages[stage].flags = ADSR_TERMINATE_LONG;
        stage++;
    }

    raw->adsrStageCount = stage;

    #ifdef _DEBUG
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
                                ChannelMap *map)
{
    uint32_t i;
    uint8_t midiCh;

    memset(map, 0, sizeof(*map));
    /* Initialize all mappings to 0xFF (unmapped) */
    memset(map->trackerToMidi, 0xFF, sizeof(map->trackerToMidi));

    /* Pass 1: Direct assignment for first min(trackerCount, 16) channels */
    for (i = 0; i < trackerCount && i < MOD2RMF_MAX_MIDI_CHANNELS; ++i)
    {
        map->trackerToMidi[i] = (uint8_t)i;
        if (profiles[i].used)
        {
            map->midiChannelUsed[i] = TRUE;
        }
    }

    /* Pass 2: Overflow assignment for channels 16+ */
    for (i = MOD2RMF_MAX_MIDI_CHANNELS; i < trackerCount; ++i)
    {
        uint8_t bestMidi = 0;
        uint32_t bestScore = UINT32_MAX; /* lower = better */
        bool foundEmpty = FALSE;

        if (!profiles[i].used)
        {
            /* Unused tracker channel — map to ch 0 as placeholder */
            map->trackerToMidi[i] = 0;
            continue;
        }

        for (midiCh = 0; midiCh < MOD2RMF_MAX_MIDI_CHANNELS; ++midiCh)
        {
            ChannelProfile agg;
            uint32_t ovlap;

            memset(&agg, 0, sizeof(agg));

            if (!map->midiChannelUsed[midiCh])
            {
                /* Empty MIDI channel — best possible choice */
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
            #ifdef _DEBUG
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