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

#include "mod2rmf_rmfcreat.h"
#include "mod2rmf_common.h"
#include "mod2rmf_song.h"
#include "X_Formats.h"
#include "X_Assert.h"
#include <math.h>
#include <string.h>
#include <xmp.h>

#ifndef FOUR_CHAR
#include "X_API.h"
#endif
#define MOD2RMF_FCC(a,b,c,d) FOUR_CHAR((a),(b),(c),(d))

/* Default: keep native Amiga/XM rate and middle-C root. Non-zero shift
 * (via --down-octave-range) still lowers root + rate together for low-note
 * headroom under BAE's default -24 semitone pitch floor; prefer --ext-pitch. */
#define MOD2RMF_DEFAULT_ROOT_SHIFT_ST 0u
#define MOD2RMF_LOGICAL_ROOT_KEY 60u
/* BAE pitch LFO depth: cents * 41 (same as sf2-hsb). */
#define MOD2RMF_PITCH_CENTS_TO_LFO 41

static int32_t mod2rmf_it_vib_wave_shape(int vwf)
{
    switch (vwf & 3)
    {
    case 1: return (int32_t)MOD2RMF_FCC('S', 'A', 'W', 'T');
    case 2: return (int32_t)MOD2RMF_FCC('S', 'Q', 'U', 'A');
    case 3: return (int32_t)MOD2RMF_FCC('T', 'R', 'I', 'A');
    case 0:
    default: return (int32_t)MOD2RMF_FCC('S', 'I', 'N', 'E');
    }
}

static void mod2rmf_capture_it_lpf(ModRawSample *raw, int ifc, int ifr)
{
    if (!raw || raw->hasLpf)
    {
        return;
    }
    if (!(ifc & 0x80))
    {
        return;
    }
    raw->hasLpf = TRUE;
    raw->lpfFrequency = (int32_t)(ifc & 0x7f) * 256;
    if (raw->lpfFrequency < 0x200 && raw->lpfFrequency > 0)
    {
        raw->lpfFrequency = 0x200;
    }
    if (ifr & 0x80)
    {
        raw->lpfResonance = (int32_t)(ifr & 0x7f) * 2;
    }
    else
    {
        raw->lpfResonance = 0;
    }
    /* Engage mono LPF path even when resonance is 0. */
    raw->lpfAmount = 255;
}

/* IT NNA Cut/Fade on note replace does not play the full post-sustain
 * envelope release. Collapse that tail so MIDI note-off (our flush on
 * retrigger / DCT) does not smear the previous phrase under the next. */
/* IT duplicate check: when a new note starts, apply DCA to other active
 * notes that match DCT (note / sample / instrument).
 * DCT=Instrument must use the IT instrument index — NOT program/sample.
 * (M)TRANC pattern 9 layers ins7+ins27 on the same sample; matching by
 * program cut the undelayed voice after SDx (~1 frame). */
static int mod2rmf_flush_dct_duplicates(ModSongModel *song,
                                        ActiveNote *activeNotes,
                                        uint32_t channelCount,
                                        uint16_t newChannel,
                                        uint8_t newProgram,
                                        uint8_t newInstrument,
                                        unsigned char newNote,
                                        int dct,
                                        uint64_t tickFP)
{
    uint32_t ch;

    if (!song || !activeNotes || dct == XMP_INST_DCT_OFF)
    {
        return 1;
    }

    for (ch = 0; ch < channelCount; ++ch)
    {
        ActiveNote *other = &activeNotes[ch];
        int match = 0;

        if (ch == (uint32_t)newChannel || !other->active)
        {
            continue;
        }

        switch (dct)
        {
        case XMP_INST_DCT_NOTE:
            match = (other->note == newNote) ? 1 : 0;
            break;
        case XMP_INST_DCT_SMP:
            /* One sample → one MIDI program in mod2rmf. */
            match = (other->program == newProgram) ? 1 : 0;
            break;
        case XMP_INST_DCT_INST:
            match = (other->instrument == newInstrument &&
                     newInstrument != 255u) ? 1 : 0;
            break;
        default:
            break;
        }

        if (match)
        {
            if (!mod2rmf_flush_active_note(song, (uint16_t)ch, other, tickFP))
            {
                return 0;
            }
        }
    }
    return 1;
}

/* IT instruments with fadeout but no volume envelope: put release in INST
 * volumeADSR instead of letting libxmp bake it into per-frame CC7. */
static void mod2rmf_synthesize_fadeout_adsr(ModRawSample *raw,
                                           const struct xmp_instrument *inst,
                                           uint32_t bpm)
{
    double usPerTick;
    uint32_t releaseUs;
    double ticks;

    if (!raw || !inst || raw->hasEnvelope || inst->rls <= 0)
    {
        return;
    }

    usPerTick = 2500000.0 / (double)(bpm > 0 ? bpm : 125);
    /* libxmp: fadeout -= rls each tick from ~65536. */
    ticks = 65536.0 / (double)inst->rls;
    releaseUs = (uint32_t)(ticks * usPerTick + 0.5);
    if (releaseUs < 1000u)
    {
        releaseUs = 1000u;
    }
    if (releaseUs > 10000000u)
    {
        releaseUs = 10000000u;
    }

    raw->hasEnvelope = TRUE;
    raw->adsrStageCount = 2u;
    raw->adsrStages[0].level = VOLUME_RANGE;
    raw->adsrStages[0].timeUs = 0;
    raw->adsrStages[0].flags = ADSR_SUSTAIN_LONG;
    raw->adsrStages[1].level = 0;
    raw->adsrStages[1].timeUs = (int32_t)releaseUs;
    raw->adsrStages[1].flags = ADSR_TERMINATE_LONG;
}

static void mod2rmf_clamp_adsr_for_nna(ModRawSample *raw, uint32_t bpm)
{
    uint32_t i;
    uint32_t sustainIdx;
    uint32_t shortUs;
    double usPerTick;
    int32_t fadeout;

    if (!raw || !raw->hasEnvelope || raw->adsrStageCount < 2 || !raw->hasNotePolicy)
    {
        return;
    }

    if (raw->nna != XMP_INST_NNA_CUT && raw->nna != XMP_INST_NNA_FADE)
    {
        return;
    }

    usPerTick = 2500000.0 / (double)(bpm > 0 ? bpm : 125);
    fadeout = raw->fadeout;
    if (raw->nna == XMP_INST_NNA_CUT)
    {
        shortUs = 15000u; /* ~15ms hard cut */
    }
    else if (fadeout <= 0)
    {
        /* NNA Fade with zero IT fadeout still needs a short declick tail.
         * Keep it well under a row so DCT/NNA replaces do not smear. */
        shortUs = 80000u; /* 80ms */
    }
    else
    {
        /* libxmp: fadeout -= rls each tick from ~65536. */
        double ticks = 65536.0 / (double)fadeout;
        shortUs = (uint32_t)(ticks * usPerTick + 0.5);
        if (shortUs < 40000u)
        {
            shortUs = 40000u;
        }
        if (shortUs > 500000u)
        {
            shortUs = 500000u; /* cap 0.5s — still much shorter than env tails */
        }
    }

    sustainIdx = raw->adsrStageCount;
    for (i = 0; i < raw->adsrStageCount; ++i)
    {
        if (raw->adsrStages[i].flags == ADSR_SUSTAIN_LONG)
        {
            sustainIdx = i;
            break;
        }
    }
    if (sustainIdx >= raw->adsrStageCount)
    {
        return;
    }

    /* Keep attack+sustain; replace release with a short terminate. */
    raw->adsrStageCount = sustainIdx + 2u;
    raw->adsrStages[sustainIdx + 1u].level = 0;
    raw->adsrStages[sustainIdx + 1u].timeUs = (int32_t)shortUs;
    raw->adsrStages[sustainIdx + 1u].flags = ADSR_TERMINATE_LONG;
}

static void mod2rmf_capture_note_policy(ModRawSample *raw,
                                        const struct xmp_instrument *inst,
                                        const struct xmp_subinstrument *sub)
{
    if (!raw || !inst || !sub || raw->hasNotePolicy)
    {
        return;
    }
    raw->hasNotePolicy = TRUE;
    raw->nna = (int8_t)sub->nna;
    raw->dct = (int8_t)sub->dct;
    raw->dca = (int8_t)sub->dca;
    raw->fadeout = inst->rls;
}

static void mod2rmf_capture_sample_vibrato(ModRawSample *raw,
                                          const struct xmp_subinstrument *sub,
                                          uint32_t bpm)
{
    int rate;
    double usPerTick;
    int32_t cents;
    int32_t periodUs;
    int32_t sweepFrames;

    if (!raw || !sub || raw->hasVibrato)
    {
        return;
    }
    if (sub->vde <= 0 || sub->vra <= 0)
    {
        return;
    }

    /* libxmp: rate = (vra + 2) >> 2, 64-phase table advanced once per tick. */
    rate = (sub->vra + 2) >> 2;
    if (rate < 1)
    {
        rate = 1;
    }
    usPerTick = 2500000.0 / (double)(bpm > 0 ? bpm : 125);
    periodUs = (int32_t)((64.0 / (double)rate) * usPerTick + 0.5);
    if (periodUs < 10000)
    {
        periodUs = 10000;
    }
    if (periodUs > 10000000)
    {
        periodUs = 10000000;
    }

    /* IT sample vibrato depth is in 1/64 semitone; libxmp stores vid<<1. */
    cents = (int32_t)(((int64_t)sub->vde * 50) / 64);
    if (cents < 1)
    {
        cents = 1;
    }

    raw->hasVibrato = TRUE;
    raw->vibPeriodUs = periodUs;
    raw->vibLevel = cents * MOD2RMF_PITCH_CENTS_TO_LFO;
    if (raw->vibLevel < 1)
    {
        raw->vibLevel = 1;
    }
    if (raw->vibLevel > 524288)
    {
        raw->vibLevel = 524288;
    }
    raw->vibWaveShape = mod2rmf_it_vib_wave_shape(sub->vwf);

    /* libxmp sweep counts down by 2 each tick from vsw. Approximate as a
     * linear delay-to-full LFO ADSR over that many ticks. */
    sweepFrames = sub->vsw;
    if (sweepFrames > 1)
    {
        raw->vibSweepUs = (int32_t)((double)sweepFrames * usPerTick + 0.5);
        if (raw->vibSweepUs < 0)
        {
            raw->vibSweepUs = 0;
        }
    }
    else
    {
        raw->vibSweepUs = 0;
    }
}

static unsigned char mod2rmf_shifted_root_key(unsigned char rootKey, uint8_t shiftSemitones)
{
    return (rootKey > shiftSemitones) ? (unsigned char)(rootKey - shiftSemitones) : 0;
}

static BAE_UNSIGNED_FIXED mod2rmf_compensated_sample_rate(uint32_t baseRateHz, uint8_t shiftSemitones)
{
    double rateScale;
    double shiftedRate;

    if (shiftSemitones == 0u)
    {
        return (BAE_UNSIGNED_FIXED)((double)baseRateHz * 65536.0 + 0.5);
    }

    rateScale = pow(2.0, -((double)shiftSemitones / 12.0));
    shiftedRate = (double)baseRateHz * rateScale;
    if (shiftedRate < 1.0)
    {
        shiftedRate = 1.0;
    }

    return (BAE_UNSIGNED_FIXED)(shiftedRate * 65536.0 + 0.5);
}

static bool mod2rmf_is_it_family(const char *type)
{
    if (!type || !type[0])
    {
        return FALSE;
    }

    return (strstr(type, "Impulse Tracker") ||
            strstr(type, "Compressed Impulse Tracker") ||
            strstr(type, "OpenMPT") && strstr(type, "IT")) ? TRUE : FALSE;
}

bool mod2rmf_path_is_it(const char *path)
{
    const char *ext;

    if (!path)
    {
        return FALSE;
    }

    ext = strrchr(path, '.');
    if (!ext)
    {
        return FALSE;
    }

    return (!strcmp(ext, ".it") || !strcmp(ext, ".IT") ||
            !strcmp(ext, ".itz") || !strcmp(ext, ".ITZ")) ? TRUE : FALSE;
}

static int mod2rmf_tracker_note_bias(const Mod2RmfConverter *conv)
{
    (void)conv;
    /* Logical root is middle C (60); libxmp notes map 1:1 with no octave bias.
     * (Former root 72 + bias +12 was pitch-equivalent but pushed MIDI into
     * octaves 5–7 and paired with a default -24 st rate/root fake-out.) */
    return 0;
}

static void mod2rmf_apply_sample_gain(ModRawSample *raw, double gainDb)
{
    double scale;
    uint32_t i;

    if (!raw || raw->frameCount == 0)
    {
        return;
    }
    if (gainDb > -0.0001 && gainDb < 0.0001)
    {
        return;
    }

    scale = pow(10.0, gainDb / 20.0);
    if (raw->pcm16)
    {
        for (i = 0; i < raw->frameCount; ++i)
        {
            double v = (double)raw->pcm16[i] * scale;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            raw->pcm16[i] = (int16_t)((v >= 0.0) ? (v + 0.5) : (v - 0.5));
        }
    }
    if (!raw->pcm8)
    {
        return;
    }
    for (i = 0; i < raw->frameCount; ++i)
    {
        int sampleU8;
        int centered;
        int scaled;

        sampleU8 = (int)((uint8_t)raw->pcm8[i]);
        centered = sampleU8 - 128;

        if (centered >= 0)
        {
            scaled = (int)(centered * scale + 0.5);
        }
        else
        {
            scaled = (int)(centered * scale - 0.5);
        }

        sampleU8 = scaled + 128;
        if (sampleU8 < 0)
        {
            sampleU8 = 0;
        }
        if (sampleU8 > 255)
        {
            sampleU8 = 255;
        }

        raw->pcm8[i] = (int8_t)sampleU8;
    }
}

/* Resolve per-note transpose from the active instrument mapping.
 * Some formats (notably IT) can reuse the same sample across multiple
 * instruments with different transpose settings, so a global per-sample
 * transpose cache can detune only certain notes.
 *
 * IT/S3M: C5/C2 rate is stored on the sample; strip that rate-encoding
 * xpo so MIDI notes stay in the pattern-key domain (avoids note>127 clamp
 * on high-C5 samples like AVP2.it). */
static int mod2rmf_resolve_note_transpose(const struct xmp_module *mod,
                                          const struct xmp_channel_info *ci,
                                          int sid,
                                          const int16_t sampleTranspose[MOD2RMF_MAX_SAMPLES],
                                          const ModRawSample *rawSamples,
                                          uint32_t rawSampleCount)
{
    int noteXpo = 0;
    int instIndex = -1;

    /* libxmp exports xc->ins as a 0-based instrument index (or 255 if none). */
    if (ci && mod && (int)ci->instrument >= 0 && (int)ci->instrument < mod->ins)
    {
        instIndex = (int)ci->instrument;
    }
    if (mod && instIndex >= 0 && instIndex < mod->ins && mod->xxi)
    {
        const struct xmp_instrument *inst = &mod->xxi[instIndex];
        int sub;
        bool foundSub = FALSE;

        if (inst->nsm > 0 && inst->sub && sid >= 0)
        {
            for (sub = 0; sub < inst->nsm; ++sub)
            {
                if (inst->sub[sub].sid == sid)
                {
                    noteXpo += inst->sub[sub].xpo;
                    foundSub = TRUE;
                    break;
                }
            }
        }

        if (!foundSub && sid >= 0 && sid < MOD2RMF_MAX_SAMPLES)
        {
            noteXpo += (int)sampleTranspose[sid];
        }
    }
    else if (sid >= 0 && sid < MOD2RMF_MAX_SAMPLES)
    {
        noteXpo = (int)sampleTranspose[sid];
    }

    if (rawSamples && sid >= 0 && (uint32_t)sid < rawSampleCount &&
        rawSamples[sid].hasRateMapping)
    {
        noteXpo -= (int)rawSamples[sid].rateXpo;
    }

    return noteXpo;
}

/* Sample baseKey for IT/S3M: logical middle C plus octave-fold adjust so a
 * 16.16-safe stored rate still plays the true C5 pitch. Instrument
 * midiRootKey must stay at logical middle C — applying the fold on both
 * would double-compensate. */
static unsigned char mod2rmf_sample_root_key_for_raw(const ModRawSample *raw,
                                                    uint8_t rootShiftSemitones)
{
    int root;

    root = (int)MOD2RMF_LOGICAL_ROOT_KEY;
    if (raw && raw->hasRateMapping)
    {
        root += (int)raw->rateRootAdjust;
    }
    if (root < 0)
    {
        root = 0;
    }
    if (root > 127)
    {
        root = 127;
    }
    return mod2rmf_shifted_root_key((unsigned char)root, rootShiftSemitones);
}

/* Read the raw row event directly from the module pattern/track tables.
 * This preserves original tracker column intent (e.g. IT volume column)
 * before libxmp frame processing mutates channel state. */
static const struct xmp_event *mod2rmf_get_raw_row_event(const struct xmp_module *mod,
                                                         int pattern,
                                                         int row,
                                                         uint32_t ch)
{
    struct xmp_pattern *pat;
    struct xmp_track *trk;
    int trackIndex;

    if (!mod || !mod->xxp || !mod->xxt)
    {
        return NULL;
    }
    if (pattern < 0 || pattern >= mod->pat || row < 0 || ch >= (uint32_t)mod->chn)
    {
        return NULL;
    }

    pat = mod->xxp[pattern];
    if (!pat || row >= pat->rows)
    {
        return NULL;
    }

    trackIndex = pat->index[ch];
    if (trackIndex < 0 || trackIndex >= mod->trk)
    {
        return NULL;
    }

    trk = mod->xxt[trackIndex];
    if (!trk || row >= trk->rows)
    {
        return NULL;
    }

    return &trk->event[row];
}

int mod2rmf_load_source_data(Mod2RmfConverter *conv, const char *sourcePath)
{
    FILE *file;
    size_t fileSize;
    size_t bytesRead;

    if (!conv || !sourcePath)
    {
        return 0;
    }

    file = fopen(sourcePath, "rb");
    if (!file)
    {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return 0;
    }
    {
        long rawSize;
        rawSize = ftell(file);
        if (rawSize < 0)
        {
            fclose(file);
            return 0;
        }
        fileSize = (size_t)rawSize;
    }
    if (fseek(file, 0, SEEK_SET) != 0 || fileSize == 0)
    {
        fclose(file);
        return 0;
    }

    conv->sourceData = malloc(fileSize);
    if (!conv->sourceData)
    {
        fclose(file);
        return 0;
    }
    conv->sourceSize = fileSize;

    bytesRead = fread(conv->sourceData, 1, fileSize, file);
    fclose(file);
    return bytesRead == fileSize;
}

int mod2rmf_setup_document(Mod2RmfConverter *conv,
                          const ModSongModel *song,
                          const char *sourcePath)
{
    BAEResult result;
    BAERmfEditorTrackSetup setup;
    char conductorName[] = "Conductor";

    if (!conv || !song)
    {
        return 0;
    }

    conv->document = BAERmfEditorDocument_New();
    if (!conv->document)
    {
        return 0;
    }

    BAERmfEditorDocument_SetTempoBPM(conv->document, song->bpm);
    BAERmfEditorDocument_AddTempoEvent(conv->document, 0, 60000000UL / song->bpm);
    BAERmfEditorDocument_SetTicksPerQuarter(conv->document, 480);
    if (song->moduleName[0])
    {
        BAERmfEditorDocument_SetInfo(conv->document, TITLE_INFO, song->moduleName);
        BAERmfEditorDocument_SetInfo(conv->document, COPYRIGHT_INFO, "Converted by mod2rmf");
    }
    if (song->composerNotes[0])
    {
        BAERmfEditorDocument_SetInfo(conv->document, COMPOSER_NOTES_INFO, song->composerNotes);
    }
    if (sourcePath && sourcePath[0])
    {
        BAERmfEditorDocument_SetInfo(conv->document,
                                     ORIGINAL_SOURCE_INFO,
                                     mod2rmf_path_basename_ptr(sourcePath));
    }

    memset(&setup, 0, sizeof(setup));
    setup.channel = 0;
    setup.bank = 0;
    setup.program = 0;
    setup.name = conductorName;

    result = BAERmfEditorDocument_AddTrack(conv->document, &setup, NULL);
    return result == BAE_NO_ERROR;
}

int mod2rmf_setup_samples(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;
    uint32_t baseAssetBySourceSlot[MOD2RMF_MAX_SAMPLES];

    if (!conv || !song)
    {
        return 0;
    }

    for (i = 0; i < MOD2RMF_MAX_SAMPLES; ++i)
    {
        baseAssetBySourceSlot[i] = 0xFFFFFFFFu;
    }

    for (i = 0; i < song->playableCount; ++i)
    {
        BAERmfEditorSampleSetup setup;
        BAESampleInfo sampleInfo;
        BAEResult result;
        uint32_t sampleIndex;
        uint32_t chosenLoopStart;
        uint32_t chosenLoopEnd;
        uint32_t sourceSlot;
        bool usedSharedAsset;
        const ModPlayable *playable;
        ModRawSample *raw;

        playable = &song->playables[i];
        raw = playable->rawSample;
        sourceSlot = playable->sourceSlot;
        usedSharedAsset = FALSE;

        if (!raw || !raw->valid || !raw->pcm8 || raw->frameCount == 0)
        {
            continue;
        }

        memset(&setup, 0, sizeof(setup));
        setup.program = playable->program;
        
        /* Sample baseKey stays at logical middle C (editor-friendly). Song
         * pitch compensation for IT/S3M rates is applied on instrument
         * midiRootKey in setup_instrument_ext. */
        setup.rootKey = mod2rmf_shifted_root_key(playable->rootKey, conv->rootShiftSemitones);
        
        setup.lowKey = 0;
        setup.highKey = 127;
        setup.displayName = (char *)playable->displayName;

        sampleIndex = 0;
        chosenLoopStart = 0;
        chosenLoopEnd = 0;

        result = BAERmfEditorDocument_AddEmptySample(conv->document,
                                                     &setup,
                                                     &sampleIndex,
                                                     &sampleInfo);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "[mod2rmf] Warning: failed to add sample for program %u (%d)\n", (unsigned)setup.program, (int)result);
            continue;
        }

        {
            uint32_t sampleInstID = 512u + (uint32_t)playable->program;
            BAERmfEditorDocument_SetSampleInstID(conv->document, sampleIndex, sampleInstID);
        }

        if (raw && raw->valid && raw->pcm8 && raw->frameCount > 0)
        {
            uint32_t pcmFrames;
            uint32_t loopStart;
            uint32_t loopEnd;

            pcmFrames = raw->frameCount;
            loopStart = 0;
            loopEnd = 0;

            if (raw->loopEnd > raw->loopStart)
            {
                loopStart = raw->loopStart;
                loopEnd = raw->loopEnd;
            }

            if (playable->offsetVariant && sourceSlot < MOD2RMF_MAX_SAMPLES &&
                baseAssetBySourceSlot[sourceSlot] != 0xFFFFFFFFu)
            {
                result = BAERmfEditorDocument_SetSampleAssetForSample(conv->document,
                                                                       sampleIndex,
                                                                       baseAssetBySourceSlot[sourceSlot]);
                if (result == BAE_NO_ERROR)
                {
                    usedSharedAsset = TRUE;
                }
                else
                {
                    fprintf(stderr, "[mod2rmf] Warning: failed to share sample asset for program %u (%d); falling back to PCM copy\n",
                            (unsigned)setup.program,
                            (int)result);
                }
            }

            if (!usedSharedAsset)
            {
                bool forceOriginal;
                bool requiresFilterResample;
                bool useNative16;

                chosenLoopStart = loopStart;
                chosenLoopEnd = loopEnd;
                forceOriginal = conv->forceOriginalSamples ? TRUE : FALSE;
                requiresFilterResample = mod2rmf_sample_requires_processing(playable,
                                                                           &conv->resamplerSettings,
                                                                           conv->moduleBaseRateHz);
                /* Prefer untouched IT/XM 16-bit PCM when no filter/resample. */
                useNative16 = (!forceOriginal && raw->pcm16 != NULL && !requiresFilterResample);

                 if (forceOriginal)
                 {
                     BAE_UNSIGNED_FIXED sampledRate;
                     double baseRate = playable->hasSampleRateOverride ? 
                                         (double)playable->sampleRateOverrideHz : 
                                         (double)conv->moduleBaseRateHz;
                     sampledRate = mod2rmf_compensated_sample_rate((uint32_t)(baseRate + 0.5),
                                                                   conv->rootShiftSemitones);

                     result = BAERmfEditorDocument_ReplaceSampleFromPCM(conv->document,
                                                                        sampleIndex,
                                                                        raw->pcm8,
                                                                        pcmFrames,
                                                                        8,
                                                                        1,
                                                                        sampledRate,
                                                                        loopStart,
                                                                        loopEnd,
                                                                        &sampleInfo);
                     if (result != BAE_NO_ERROR)
                     {
                         fprintf(stderr, "[mod2rmf] Warning: failed to inject raw 8-bit PCM for program %u (%d)\n",
                                 (unsigned)setup.program,
                                 (int)result);
                     }
                 }
                 else if (useNative16)
                 {
                     BAE_UNSIGNED_FIXED sampledRate;
                     double baseRate = playable->hasSampleRateOverride ?
                                         (double)playable->sampleRateOverrideHz :
                                         (double)conv->moduleBaseRateHz;
                     sampledRate = mod2rmf_compensated_sample_rate((uint32_t)(baseRate + 0.5),
                                                                   conv->rootShiftSemitones);

                     result = BAERmfEditorDocument_ReplaceSampleFromPCM(conv->document,
                                                                        sampleIndex,
                                                                        raw->pcm16,
                                                                        pcmFrames,
                                                                        16,
                                                                        1,
                                                                        sampledRate,
                                                                        loopStart,
                                                                        loopEnd,
                                                                        &sampleInfo);
                     if (result != BAE_NO_ERROR)
                     {
                         fprintf(stderr, "[mod2rmf] Warning: failed to inject native 16-bit PCM for program %u (%d)\n",
                                 (unsigned)setup.program,
                                 (int)result);
                     }
                 }
                else
                {
                    int16_t *pcm16;
                    uint32_t outFrames;
                    uint32_t outRate;
                    uint32_t srcRateForProcessing;
                    uint32_t scaledLoopStart;
                    uint32_t scaledLoopEnd;
                    BAE_UNSIGNED_FIXED sampledRate;

                    /* Apply Amiga hardware filter and/or resampling.  The
                     * returned int16 buffer is already 16-bit so the engine can
                     * use the advancedInterpolation (PV_LoopWrapSample16) path. */
                     srcRateForProcessing = playable->hasSampleRateOverride && playable->sampleRateOverrideHz > 0u ?
                                             playable->sampleRateOverrideHz : conv->moduleBaseRateHz;

                    pcm16 = mod2rmf_process_sample(raw->pcm8, pcmFrames,
                                                   srcRateForProcessing,
                                                   &conv->resamplerSettings,
                                                   &outFrames, &outRate);
                    if (!pcm16)
                    {
                        fprintf(stderr,
                                "[mod2rmf] Warning: failed to process sample for program %u\n",
                                (unsigned)setup.program);
                        continue;
                    }

                    sampledRate = mod2rmf_compensated_sample_rate(outRate,
                                                                  conv->rootShiftSemitones);

                    /* Scale loop end-points to the output frame domain so that
                     * ReplaceSampleFromPCM receives indices within the buffer.
                     * The subsequent remapping block recomputes them from
                     * sampleInfo.waveFrames, which handles any codec re-framing. */
                    scaledLoopStart = loopStart;
                    scaledLoopEnd   = loopEnd;
                    if (outFrames != pcmFrames && pcmFrames > 0)
                    {
                        scaledLoopStart = (uint32_t)(((uint64_t)loopStart * outFrames
                                                      + pcmFrames / 2u) / pcmFrames);
                        scaledLoopEnd   = (uint32_t)(((uint64_t)loopEnd   * outFrames
                                                      + pcmFrames / 2u) / pcmFrames);
                    }

                    result = BAERmfEditorDocument_ReplaceSampleFromPCM(conv->document,
                                                                       sampleIndex,
                                                                       pcm16,
                                                                       outFrames,
                                                                       16,
                                                                       1,
                                                                       sampledRate,
                                                                       scaledLoopStart,
                                                                       scaledLoopEnd,
                                                                       &sampleInfo);
                    free(pcm16);
                    if (result != BAE_NO_ERROR)
                    {
                        fprintf(stderr,
                                "[mod2rmf] Warning: failed to inject processed PCM for program %u (%d)\n",
                                (unsigned)setup.program,
                                (int)result);
                    }
                }

                if (result == BAE_NO_ERROR && !playable->offsetVariant && sourceSlot < MOD2RMF_MAX_SAMPLES)
                {
                    uint32_t assetID;

                    if (BAERmfEditorDocument_GetSampleAssetIDForSample(conv->document,
                                                                       sampleIndex,
                                                                       &assetID) == BAE_NO_ERROR)
                    {
                        baseAssetBySourceSlot[sourceSlot] = assetID;
                    }
                }
            }
            else
            {
                chosenLoopStart = loopStart;
                chosenLoopEnd = loopEnd;
            }
        }

        if (raw && raw->valid && chosenLoopEnd > chosenLoopStart)
        {
            BAERmfEditorSampleInfo editorSampleInfo;
            BAEResult infoResult;
            uint32_t dstFrames;
            uint32_t srcFrames;
            uint32_t mappedStart;
            uint32_t mappedEnd;
            uint32_t srcLoopStart;
            uint32_t srcLoopEnd;

            dstFrames = sampleInfo.waveFrames;
            srcFrames = raw->frameCount;
            srcLoopStart = chosenLoopStart;
            srcLoopEnd = chosenLoopEnd;
            mappedStart = srcLoopStart;
            mappedEnd = srcLoopEnd;

            if (srcFrames > 0 && dstFrames > 0 && srcFrames != dstFrames)
            {
                mappedStart = (uint32_t)(((uint64_t)srcLoopStart * (uint64_t)dstFrames + (uint64_t)(srcFrames / 2u)) / (uint64_t)srcFrames);
                mappedEnd = (uint32_t)(((uint64_t)srcLoopEnd * (uint64_t)dstFrames + (uint64_t)(srcFrames / 2u)) / (uint64_t)srcFrames);
            }
            if (mappedStart > dstFrames)
            {
                mappedStart = dstFrames;
            }
            if (mappedEnd > dstFrames)
            {
                mappedEnd = dstFrames;
            }
            if (mappedEnd <= mappedStart)
            {
                mappedStart = 0;
                mappedEnd = 0;
            }

            infoResult = BAERmfEditorDocument_GetSampleInfo(conv->document, sampleIndex, &editorSampleInfo);
            if (infoResult == BAE_NO_ERROR)
            {
                editorSampleInfo.sampleInfo.startLoop = mappedStart;
                editorSampleInfo.sampleInfo.endLoop = mappedEnd;
                infoResult = BAERmfEditorDocument_SetSampleInfo(conv->document, sampleIndex, &editorSampleInfo);
            }

            if (infoResult != BAE_NO_ERROR)
            {
                fprintf(stderr,
                        "[mod2rmf] Warning: failed to apply loop points for program %u (%d)\n",
                        (unsigned)setup.program,
                        (int)infoResult);
            }
        }
    }

    return 1;
}

int mod2rmf_build_song_model(Mod2RmfConverter *conv, ModSongModel *song)
{
    xmp_context ctx;
    struct xmp_module_info mi;
    struct xmp_module *mod;
    struct xmp_frame_info fi;
    ActiveNote activeNotes[MOD2RMF_MAX_CHANNELS];
    ChannelEffectState chEffects[MOD2RMF_MAX_CHANNELS];
    uint8_t v00CutArmed[MOD2RMF_MAX_CHANNELS];
    uint16_t v00SilentRows[MOD2RMF_MAX_CHANNELS];
    uint8_t chOffsetMemory[MOD2RMF_MAX_CHANNELS];
    uint8_t chHiOffsetMemory[MOD2RMF_MAX_CHANNELS];
    uint8_t chLastVol[MOD2RMF_MAX_CHANNELS];
    uint8_t chLastPan[MOD2RMF_MAX_CHANNELS];
    uint16_t chLastBend[MOD2RMF_MAX_CHANNELS];
    int16_t sampleTranspose[MOD2RMF_MAX_SAMPLES];  /* sub-instrument xpo per sample */
    bool sampleHasEnvelope[MOD2RMF_MAX_SAMPLES];   /* instrument has amplitude envelope */
    bool sampleHasPitchEnv[MOD2RMF_MAX_SAMPLES];
    bool sampleHasFilterEnv[MOD2RMF_MAX_SAMPLES];
    uint64_t positionTickFP[XMP_MAX_MOD_LENGTH]; /* MIDI tick at start of each order position */
    int positionSeen[XMP_MAX_MOD_LENGTH];        /* whether we've recorded the tick for this pos */
    int lastPos;                                 /* last seen order position */
    int *sampleToProgram;
    uint64_t currentTickFP;
    uint64_t tickPerFrameFP;
    uint32_t frameGuard;
    uint32_t nextProgram;
    uint16_t lastBpm;
    int playerStarted;
    uint32_t i;

    if (!conv || !song || !conv->sourceData || conv->sourceSize == 0)
    {
        return 0;
    }

    ctx = xmp_create_context();
    if (!ctx)
    {
        return 0;
    }
    if (xmp_load_module_from_memory(ctx, conv->sourceData, (long)conv->sourceSize) != 0)
    {
        xmp_free_context(ctx);
        return 0;
    }

    memset(&mi, 0, sizeof(mi));
    xmp_get_module_info(ctx, &mi);
    mod = mi.mod;
    if (!mod)
    {
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return 0;
    }

    mod2rmf_trim_copy_ascii(song->moduleName, sizeof(song->moduleName), mod->name);
    if (mi.comment)
    {
        mod2rmf_trim_copy_ascii(song->composerNotes, sizeof(song->composerNotes), mi.comment);
    }

    conv->isMod = mod2rmf_is_mod_family(mod->type);
    conv->isIt = conv->isIt || mod2rmf_is_it_family(mod->type);
    conv->moduleBaseRateHz = conv->isMod ? MOD2RMF_SAMPLE_RATE : 8363u;

    song->channelCount = (mod->chn > MOD2RMF_MAX_CHANNELS) ? MOD2RMF_MAX_CHANNELS : (uint32_t)mod->chn;
    lastBpm = (mod->bpm > 0) ? (uint16_t)mod->bpm : 125u;
    song->bpm = lastBpm;
    song->pitchBendRangeSemitones = MOD2RMF_PITCH_BEND_RANGE_ST;
    (void)mod2rmf_song_model_append_tempo_change(song, 0, lastBpm);

    free(conv->rawSamples);
    conv->rawSamples = NULL;
    conv->rawSampleCount = (mod->smp > 0) ? (uint32_t)mod->smp : 0u;
    if (conv->rawSampleCount > 0)
    {
        conv->rawSamples = (ModRawSample *)calloc(conv->rawSampleCount, sizeof(ModRawSample));
        if (!conv->rawSamples)
        {
            xmp_release_module(ctx);
            xmp_free_context(ctx);
            return 0;
        }
    }

    /* Capture per-sample instrument transpose (sub->xpo) so we can
     * reconstruct the transposed note for MIDI output.  libxmp's public
     * ci->note (= xc->key) is the raw pattern key WITHOUT transpose,
     * while ci->pitchbend is relative to the TRANSPOSED note. */
    memset(sampleTranspose, 0, sizeof(sampleTranspose));
    memset(sampleHasEnvelope, 0, sizeof(sampleHasEnvelope));
    memset(sampleHasPitchEnv, 0, sizeof(sampleHasPitchEnv));
    memset(sampleHasFilterEnv, 0, sizeof(sampleHasFilterEnv));
    if (mod->ins > 0 && mod->xxi)
    {
        for (i = 0; i < (uint32_t)mod->ins; ++i)
        {
            const struct xmp_instrument *inst;
            int sub;

            inst = &mod->xxi[i];
            if (inst->nsm <= 0 || !inst->sub)
            {
                continue;
            }

            for (sub = 0; sub < inst->nsm; ++sub)
            {
                int sid;
                sid = inst->sub[sub].sid;
                if (sid < 0 || sid >= (int)conv->rawSampleCount || sid >= MOD2RMF_MAX_SAMPLES)
                {
                    continue;
                }
                if (sampleTranspose[sid] == 0)
                {
                    sampleTranspose[sid] = (int16_t)inst->sub[sub].xpo;
                }
            }
        }
    }

    for (i = 0; i < conv->rawSampleCount; ++i)
    {
        const struct xmp_sample *s;
        ModRawSample *raw;
        int len;

        s = &mod->xxs[i];
        raw = &conv->rawSamples[i];
        memset(raw, 0, sizeof(*raw));
        mod2rmf_trim_copy_ascii(raw->name, sizeof(raw->name), s->name);
        raw->rootKey = MOD2RMF_LOGICAL_ROOT_KEY;
        raw->defaultVolume = 64;
        raw->defaultPan = -1; /* unset; will be filled from sub-instrument */
        raw->finetune = 0;

        len = s->len;
        if (len <= 0 || !s->data || (s->flg & XMP_SAMPLE_SYNTH))
        {
            continue;
        }

        if (s->flg & XMP_SAMPLE_16BIT)
        {
            const int16_t *src16;
            uint32_t outFrames;
            uint32_t f;

            outFrames = (uint32_t)len;
            raw->pcm16 = (int16_t *)malloc(outFrames * sizeof(int16_t));
            raw->pcm8 = (int8_t *)malloc(outFrames);
            if (!raw->pcm16 || !raw->pcm8)
            {
                free(raw->pcm16);
                free(raw->pcm8);
                raw->pcm16 = NULL;
                raw->pcm8 = NULL;
                xmp_release_module(ctx);
                xmp_free_context(ctx);
                return 0;
            }
            src16 = (const int16_t *)s->data;
            memcpy(raw->pcm16, src16, outFrames * sizeof(int16_t));
            for (f = 0; f < outFrames; ++f)
            {
                int v;
                v = (int)src16[f] >> 8;
                v += 128;
                raw->pcm8[f] = (int8_t)mod2rmf_clamp_int(v, 0, 255);
            }
            raw->frameCount = outFrames;
        }
        else
        {
            const uint8_t *src8;
            uint32_t outFrames;
            uint32_t f;

            outFrames = (uint32_t)len;
            raw->pcm8 = (int8_t *)malloc(outFrames);
            if (!raw->pcm8)
            {
                xmp_release_module(ctx);
                xmp_free_context(ctx);
                return 0;
            }
            src8 = (const uint8_t *)s->data;
            for (f = 0; f < outFrames; ++f)
            {
                raw->pcm8[f] = (int8_t)(src8[f] ^ 0x80u);
            }
            raw->frameCount = outFrames;
        }

        mod2rmf_apply_sample_gain(raw, conv->sampleGainDb);

        raw->loopStart = 0;
        raw->loopEnd = 0;

        /* Only set loop points if libxmp reports the sample as looping.
         * Many tracker formats store lps=0 lpe=2 as a "no loop" sentinel;
         * without the flag check those would produce a tiny invalid loop. */
        if (s->flg & XMP_SAMPLE_LOOP)
        {
            raw->loopStart = (s->lps > 0) ? (uint32_t)s->lps : 0u;
            raw->loopEnd = (s->lpe > s->lps) ? (uint32_t)s->lpe : 0u;
            if (raw->loopStart > raw->frameCount) raw->loopStart = 0;
            if (raw->loopEnd > raw->frameCount) raw->loopEnd = raw->frameCount;
            /* Discard degenerate loops (< 3 frames) */
            if (raw->loopEnd - raw->loopStart < 3)
            {
                raw->loopStart = 0;
                raw->loopEnd = 0;
            }

            /* Detect loop type from libxmp flags (only meaningful with a valid loop). */
            if (raw->loopEnd > raw->loopStart)
            {
                if (s->flg & XMP_SAMPLE_LOOP_BIDIR)
                {
                    raw->loopType = MOD2RMF_LOOP_BIDIR;
                }
                else if (s->flg & XMP_SAMPLE_LOOP_REVERSE)
                {
                    raw->loopType = MOD2RMF_LOOP_REVERSE;
                }
            }
        }

        raw->valid = TRUE;

        /* Emulate non-forward loop types by transforming the PCM data
         * so the BAE engine's forward-only loop player can reproduce
         * the correct audible result. */
        if (raw->loopType == MOD2RMF_LOOP_BIDIR)
        {
            mod2rmf_emulate_bidi_loop(raw);
        }
        else if (raw->loopType == MOD2RMF_LOOP_REVERSE)
        {
            mod2rmf_emulate_reverse_loop(raw);
        }

        /* Default rate until IT/S3M C5/C2 mapping below overrides it. */
        raw->sampleRateHz = conv->moduleBaseRateHz;
        raw->hasRateMapping = FALSE;
        raw->rateXpo = 0;
        raw->rateFin = 0;
        raw->rateRootAdjust = 0;
    }

    /* Extract amplitude envelope ADSR and (for IT/S3M) per-sample C5/C2
     * rates from the first instrument that references each sample.
     * Runs AFTER PCM extraction so memset() above cannot clobber them. */
    if (mod->ins > 0 && mod->xxi)
    {
        const bool useSampleC5Rate =
            mod2rmf_format_uses_sample_c5_rate(conv->isIt, mod->type);

        for (i = 0; i < (uint32_t)mod->ins; ++i)
        {
            struct xmp_instrument *inst = &mod->xxi[i];
            int sub;
            bool instrumentGotVolumeAdsr = FALSE;
            bool instrumentGotPitchOrFilter = FALSE;
            const bool instrumentHadAei = (inst->aei.flg & XMP_ENVELOPE_ON) != 0;
            const bool instrumentHadFei = (inst->fei.flg & XMP_ENVELOPE_ON) != 0;
            uint32_t bpm = (uint32_t)(mod->bpm > 0 ? mod->bpm : 125);

            if (inst->nsm <= 0 || !inst->sub)
            {
                continue;
            }

            for (sub = 0; sub < inst->nsm; ++sub)
            {
                int sid;
                sid = inst->sub[sub].sid;
                if (sid < 0 || sid >= (int)conv->rawSampleCount || sid >= MOD2RMF_MAX_SAMPLES)
                {
                    continue;
                }
                if (useSampleC5Rate && !conv->rawSamples[sid].hasRateMapping)
                {
                    uint32_t trueRate;
                    int16_t rootAdjust = 0;

                    conv->rawSamples[sid].rateXpo = (int16_t)inst->sub[sub].xpo;
                    conv->rawSamples[sid].rateFin = (int16_t)inst->sub[sub].fin;
                    trueRate = mod2rmf_c2spd_from_xpo_fin(inst->sub[sub].xpo, inst->sub[sub].fin);
                    /* AVP2.it etc. use C5 > 65535 Hz — fold into 16.16-safe range. */
                    conv->rawSamples[sid].sampleRateHz =
                        mod2rmf_fold_rate_for_fixed(trueRate, &rootAdjust);
                    conv->rawSamples[sid].rateRootAdjust = rootAdjust;
                    conv->rawSamples[sid].hasRateMapping = TRUE;
                }
                /* Keep aei enabled for the whole sub loop so multi-sample
                 * instruments can stamp volumeADSR onto every mapped sample. */
                if (!sampleHasEnvelope[sid] &&
                    instrumentHadAei && inst->aei.npt >= 2)
                {
                    mod2rmf_extract_envelope_adsr(inst, bpm,
                                          &conv->rawSamples[sid],
                                          conv->maxAdsrStages);
                    sampleHasEnvelope[sid] = conv->rawSamples[sid].hasEnvelope;
                }
                /* Fadeout-only IT instruments: INST release, not MIDI CC7. */
                if (!sampleHasEnvelope[sid] && inst->rls > 0)
                {
                    mod2rmf_synthesize_fadeout_adsr(&conv->rawSamples[sid], inst, bpm);
                    sampleHasEnvelope[sid] = conv->rawSamples[sid].hasEnvelope;
                }

                mod2rmf_capture_note_policy(&conv->rawSamples[sid], inst, &inst->sub[sub]);
                if (sampleHasEnvelope[sid])
                {
                    instrumentGotVolumeAdsr = TRUE;
                    mod2rmf_clamp_adsr_for_nna(&conv->rawSamples[sid], bpm);
                }

                /* Pitch / filter envelope (fei). FLT bit selects filter vs pitch. */
                if (instrumentHadFei && inst->fei.npt >= 2)
                {
                    ModEnvelopeAdsr extracted;

                    if ((inst->fei.flg & XMP_ENVELOPE_FLT) && !sampleHasFilterEnv[sid])
                    {
                        mod2rmf_extract_envelope_to_adsr(&inst->fei, inst, bpm,
                                                         MOD2RMF_ENV_SCALE_FILTER,
                                                         &extracted,
                                                         conv->maxAdsrStages);
                        if (extracted.valid && extracted.stageCount > 0)
                        {
                            ModRawSample *raw = &conv->rawSamples[sid];
                            raw->hasFilterEnv = TRUE;
                            raw->filterEnvStageCount = extracted.stageCount;
                            raw->filterEnvPeakAbsY = extracted.peakAbsY;
                            memcpy(raw->filterEnvStages, extracted.stages,
                                   extracted.stageCount * sizeof(ModAdsrStage));
                            sampleHasFilterEnv[sid] = TRUE;
                            instrumentGotPitchOrFilter = TRUE;
                        }
                    }
                    else if (!(inst->fei.flg & XMP_ENVELOPE_FLT) && !sampleHasPitchEnv[sid])
                    {
                        mod2rmf_extract_envelope_to_adsr(&inst->fei, inst, bpm,
                                                         MOD2RMF_ENV_SCALE_PITCH,
                                                         &extracted,
                                                         conv->maxAdsrStages);
                        if (extracted.valid && extracted.stageCount > 0)
                        {
                            ModRawSample *raw = &conv->rawSamples[sid];
                            raw->hasPitchEnv = TRUE;
                            raw->pitchEnvStageCount = extracted.stageCount;
                            raw->pitchEnvPeakAbsY = extracted.peakAbsY;
                            memcpy(raw->pitchEnvStages, extracted.stages,
                                   extracted.stageCount * sizeof(ModAdsrStage));
                            sampleHasPitchEnv[sid] = TRUE;
                            instrumentGotPitchOrFilter = TRUE;
                        }
                    }
                }

                /* IT initial filter cutoff / resonance (first assignment wins). */
                mod2rmf_capture_it_lpf(&conv->rawSamples[sid],
                                      inst->sub[sub].ifc,
                                      inst->sub[sub].ifr);

                /* Sample vibrato → INST PITC LFO (first assignment wins). */
                mod2rmf_capture_sample_vibrato(&conv->rawSamples[sid],
                                               &inst->sub[sub],
                                               bpm);
                /* Always clear so libxmp does not bake vibrato into pitchbend. */
                inst->sub[sub].vde = 0;
                inst->sub[sub].vra = 0;
                inst->sub[sub].vsw = 0;

                /* Capture default pan from sub-instrument (first assignment wins) */
                if (conv->rawSamples[sid].defaultPan < 0)
                {
                    conv->rawSamples[sid].defaultPan = (int16_t)inst->sub[sub].pan;
                }
            }

            /* INST volumeADSR owns envelope + fadeout — zero rls so libxmp
             * does not fold release into ci->volume / CC7. Only clear aei/fei
             * after a successful extract so a failed extract keeps libxmp. */
            if (instrumentGotVolumeAdsr)
            {
                inst->rls = 0;
                if (instrumentHadAei)
                {
                    inst->aei.flg &= ~XMP_ENVELOPE_ON;
                }
            }
            if (instrumentGotPitchOrFilter && instrumentHadFei)
            {
                inst->fei.flg &= ~XMP_ENVELOPE_ON;
            }
        }
    }

    sampleToProgram = NULL;
    if (conv->rawSampleCount > 0)
    {
        sampleToProgram = (int *)malloc(conv->rawSampleCount * sizeof(int));
        if (!sampleToProgram)
        {
            xmp_release_module(ctx);
            xmp_free_context(ctx);
            return 0;
        }
        for (i = 0; i < conv->rawSampleCount; ++i)
        {
            sampleToProgram[i] = -1;
        }
    }
    nextProgram = 0;

    memset(activeNotes, 0, sizeof(activeNotes));
    memset(chEffects, 0, sizeof(chEffects));
    memset(v00CutArmed, 0, sizeof(v00CutArmed));
    memset(v00SilentRows, 0, sizeof(v00SilentRows));
    memset(chOffsetMemory, 0, sizeof(chOffsetMemory));
    memset(chHiOffsetMemory, 0, sizeof(chHiOffsetMemory));
    for (i = 0; i < MOD2RMF_MAX_CHANNELS; ++i)
    {
        chLastVol[i] = 0xFF;
        chLastPan[i] = 0xFF;
        chLastBend[i] = MOD2RMF_PITCH_BEND_CENTER;
    }

    currentTickFP = 0;
    /* MOD2RMF_ROW_TICKS (120) is defined as MIDI ticks per row at speed 6.
     * Dividing by 6 gives 20 MIDI ticks per tracker frame (tick).
     * Speed changes are handled naturally: fewer frames per row at lower
     * speed means fewer MIDI ticks per row = rows play faster.  The /6
     * constant must NOT be replaced with the current speed. */
    tickPerFrameFP = ((uint64_t)MOD2RMF_ROW_TICKS << 16) / 6u;
    frameGuard = 0;
    playerStarted = 0;
    memset(positionTickFP, 0, sizeof(positionTickFP));
    memset(positionSeen, 0, sizeof(positionSeen));
    lastPos = -1;

    if (xmp_start_player(ctx, 44100, 0) != 0)
    {
        free(sampleToProgram);
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return 0;
    }
    playerStarted = 1;

    while (frameGuard < 4000000u)
    {
        uint32_t ch;
        int playRc;
        uint32_t tick;

        playRc = xmp_play_frame(ctx);
        if (playRc != 0)
        {
            break;
        }

        xmp_get_frame_info(ctx, &fi);
        tick = mod2rmf_fp_ticks_to_int(currentTickFP);

        /* Check for loop BEFORE processing this frame's events.
         * When loop_count > 0 libxmp has already jumped back, so this
         * frame is the first frame of the repeated section.  We must
         * not emit its events a second time (they were already recorded
         * during the first playthrough), and the loop end tick is the
         * current tick (the first tick that would repeat). */
        if (fi.loop_count > 0)
        {
            song->loopEnabled = TRUE;
            /* fi.pos is the position libxmp jumped back to (Bxx target).
             * Use the recorded tick for that position as the loop start. */
            if (fi.pos >= 0 && fi.pos < XMP_MAX_MOD_LENGTH && positionSeen[fi.pos])
            {
                song->loopStartTick = mod2rmf_fp_ticks_to_int(positionTickFP[fi.pos]);
            }
            else
            {
                song->loopStartTick = 0;
            }
            song->loopEndTick = tick;
            break;
        }

        /* Track the MIDI tick at the start of each order position so we
         * can map Bxx loop targets back to MIDI ticks. */
        if (fi.pos >= 0 && fi.pos < XMP_MAX_MOD_LENGTH && fi.pos != lastPos)
        {
            lastPos = fi.pos;
            if (!positionSeen[fi.pos])
            {
                positionSeen[fi.pos] = 1;
                positionTickFP[fi.pos] = currentTickFP;
            }
        }

        if (fi.bpm > 0 && (uint16_t)fi.bpm != lastBpm)
        {
            lastBpm = (uint16_t)fi.bpm;
            song->bpm = lastBpm;
            (void)mod2rmf_song_model_append_tempo_change(song, tick, lastBpm);
        }

        for (ch = 0; ch < song->channelCount; ++ch)
        {
            const struct xmp_channel_info *ci;
            const struct xmp_event *rowEvent;
            uint8_t evNote;
            uint8_t sampleNum;
            int sid;
            int program;

            ci = &fi.channel_info[ch];
            rowEvent = &ci->event;
            if (fi.frame == 0)
            {
                const struct xmp_event *rawEv;
                rawEv = mod2rmf_get_raw_row_event(mod, fi.pattern, fi.row, ch);
                if (rawEv)
                {
                    rowEvent = rawEv;
                }
            }
            evNote = rowEvent->note;
            sampleNum = ci->sample;
            sid = (int)sampleNum; /* 0-based index into mod->xxs[] */

            /* Volume, panning, and pitch bend update on every frame
             * so that slides/vibrato/tremolo are captured.
             *
             * For enveloped instruments the BAE ADSR handles the volume
             * shape, so we only emit CC7 at row boundaries (frame 0)
             * where ci->volume reflects the channel volume before the
             * envelope has modulated it.  This avoids a flood of CC7
             * events that just approximate the envelope curve.
             *
             * Snapshot the CC event count so we can fix program tags
             * below if a note-on changes the active program this frame.
             * CC7/CC10 emitted here use the *previous* note's program,
             * but the volume/pan actually belongs to the incoming note. */
            {
                uint32_t preNoteCCIdx = song->ccCount;
                uint32_t preNoteBendIdx = song->pitchBendCount;
                uint8_t  preNoteProg  = activeNotes[ch].program;
                bool hasRowVolumeCmd = FALSE;
                uint8_t rowVolumeCmd64 = 0;
                int itV00CutRows = (int)conv->itV00CutRows;
                int itV00Cut = (itV00CutRows > 0) ? 1 : 0;

                if (fi.frame == 0)
                {
                    hasRowVolumeCmd = mod2rmf_get_row_volume_command(rowEvent,
                                                                     &rowVolumeCmd64);
                }

                {
                    uint8_t adjPan;
                    adjPan = mod2rmf_apply_stereo_separation((uint8_t)ci->pan, ch,
                                                     conv->isMod, conv->stereoSeparation);
                    if (adjPan != chLastPan[ch])
                    {
                        chLastPan[ch] = adjPan;
                        (void)mod2rmf_song_model_append_cc_event(song, (uint16_t)ch, tick, 10,
                                                         (unsigned char)(adjPan >> 1),
                                                         activeNotes[ch].program);
                    }
                }

                /* CC7 = tracker channel/slide volume intent only.
                 * Instrument envelope + IT fadeout live in INST volumeADSR
                 * (rls zeroed after extract). Do not park the MIDI channel
                 * from libxmp finalvol between notes or after note-off. */
                {
                    uint8_t vol64;
                    bool rowHasVolSlide;
                    const struct xmp_event *slideEv;
                    bool intentionalRowVol;
                    /* Prefer the raw pattern row so Dxx/vol-col slides stay
                     * visible on every frame of the row (ci->event may clear). */
                    slideEv = mod2rmf_get_raw_row_event(mod, fi.pattern, fi.row, ch);
                    if (!slideEv)
                    {
                        slideEv = rowEvent;
                    }
                    rowHasVolSlide = mod2rmf_row_has_volume_slide(slideEv);
                    intentionalRowVol = (fi.frame == 0 && hasRowVolumeCmd) || rowHasVolSlide;

                    if (!activeNotes[ch].active && !intentionalRowVol)
                    {
                        /* Idle / post-flush: leave last CC7 alone. */
                    }
                    else
                    {
                        vol64 = (fi.frame == 0 && hasRowVolumeCmd)
                                  ? rowVolumeCmd64
                                  : (uint8_t)mod2rmf_clamp_int((int)ci->volume, 0, 64);
                        if (vol64 != chLastVol[ch])
                        {
                            bool intentionalZero =
                                (fi.frame == 0 && hasRowVolumeCmd && rowVolumeCmd64 == 0) ||
                                rowHasVolSlide;
                            bool emitVol;
                            if (vol64 == 0 && activeNotes[ch].active && !intentionalZero)
                            {
                                /* oneshot/mixer silence — keep last emitted CC7. */
                                emitVol = FALSE;
                            }
                            else
                            {
                                emitVol = (vol64 != 0) ||
                                          intentionalZero ||
                                          (fi.frame == 0 && hasRowVolumeCmd);
                                chLastVol[ch] = vol64;
                            }
                            if (emitVol)
                            {
                                (void)mod2rmf_song_model_append_cc_event(song, (uint16_t)ch, tick, 7,
                                                                mod2rmf_vol_to_midi(vol64),
                                                                activeNotes[ch].program);
                            }
                        }
                    }
                }
                if (activeNotes[ch].active)
                {
                    uint16_t bend;
                    bend = mod2rmf_pitchbend_to_midi(ci->pitchbend -
                                                        activeNotes[ch].rateFinCents +
                                                        activeNotes[ch].bendOffsetCents,
                                                    song->pitchBendRangeSemitones);
                    if (bend != chLastBend[ch])
                    {
                        chLastBend[ch] = bend;
                        (void)mod2rmf_song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                        activeNotes[ch].program);
                    }
                }

                /* ---- Row boundary (frame 0): parse effects, handle note events ---- */
                if (fi.frame == 0)
                {
                    uint8_t retrigInterval = 0;
                    uint8_t noteDelayFrames = 0;
                    bool hasSampleOffset = FALSE;
                    uint32_t sampleOffsetFrames = 0;

                    /* Reset per-row effect state. */
                    memset(&chEffects[ch], 0, sizeof(chEffects[ch]));

                    /* Parse retrigger and note-delay effects from both effect columns. */
                    mod2rmf_parse_row_effects(rowEvent, &retrigInterval, &noteDelayFrames);
                    chEffects[ch].retrigInterval = retrigInterval;
                    chEffects[ch].noteDelayFrames = noteDelayFrames;

                    /* Sample offset (9xx / HIOFFSET): memory is sticky; apply only
                     * when FX_OFFSET is present on this row with a note-on. */
                    mod2rmf_parse_sample_offset(rowEvent,
                                               &chOffsetMemory[ch],
                                               &chHiOffsetMemory[ch],
                                               &hasSampleOffset,
                                               &sampleOffsetFrames);
                    if (!hasSampleOffset)
                    {
                        sampleOffsetFrames = 0;
                    }

                    /* Smart v00-cut arming: arm on explicit v00, disarm on
                     * non-zero row volume or a fresh row note. */
                    if (hasRowVolumeCmd && rowVolumeCmd64 == 0)
                    {
                        v00CutArmed[ch] = 1;
                        v00SilentRows[ch] = 0;
                    }
                    else if ((hasRowVolumeCmd && rowVolumeCmd64 > 0) ||
                             (evNote > 0 && evNote <= 120))
                    {
                        v00CutArmed[ch] = 0;
                        v00SilentRows[ch] = 0;
                    }

                    /* Key-off events are always processed immediately, even with delay. */
                    if (evNote == XMP_KEY_OFF || evNote == XMP_KEY_CUT || evNote == XMP_KEY_FADE)
                    {
                        if (!mod2rmf_flush_active_note(song, (uint16_t)ch, &activeNotes[ch], currentTickFP))
                        {
                            if (playerStarted) xmp_end_player(ctx);
                            free(sampleToProgram);
                            xmp_release_module(ctx);
                            xmp_free_context(ctx);
                            return 0;
                        }
                    }
                    else if (itV00Cut && v00CutArmed[ch] && evNote == 0 && activeNotes[ch].active)
                    {
                        if (ci->volume == 0)
                        {
                            if (v00SilentRows[ch] < 0xFFFFu)
                            {
                                v00SilentRows[ch]++;
                            }
                        }
                        else
                        {
                            v00CutArmed[ch] = 0;
                            v00SilentRows[ch] = 0;
                        }

                        if (v00CutArmed[ch] && itV00CutRows > 0 && v00SilentRows[ch] >= (uint16_t)itV00CutRows)
                        {
                            if (!mod2rmf_flush_active_note(song, (uint16_t)ch, &activeNotes[ch], currentTickFP))
                            {
                                if (playerStarted) xmp_end_player(ctx);
                                free(sampleToProgram);
                                xmp_release_module(ctx);
                                xmp_free_context(ctx);
                                return 0;
                            }
                            v00CutArmed[ch] = 0;
                            v00SilentRows[ch] = 0;
                        }
                    }
                    else if (evNote > 0 && evNote <= 120 && sid >= 0 && sid < (int)conv->rawSampleCount)
                    {
                        if (conv->rawSamples[sid].valid)
                        {
                            if (noteDelayFrames > 0)
                            {
                                /* Note delay: defer the note-on to a later frame. */
                                chEffects[ch].hasDelayedNote = TRUE;
                                chEffects[ch].delayedEvNote = evNote;
                                chEffects[ch].delayedSid = sid;
                                chEffects[ch].delayedInstrument =
                                    (uint8_t)((ci->instrument <= 255u) ? ci->instrument : 255u);
                                chEffects[ch].delayedVolume = hasRowVolumeCmd
                                                              ? rowVolumeCmd64
                                                              : (uint8_t)mod2rmf_clamp_int((int)ci->volume, 0, 64);
                                chEffects[ch].delayedSampleOffsetFrames = sampleOffsetFrames;
                            }
                            else
                            {
                                /* Normal note-on (or retrigger tick 0). */
                                int baseNote;
                                int midiNote;
                                int noteXpo;

                                if (sampleToProgram[sid] < 0)
                                {
                                    if (nextProgram >= MOD2RMF_MAX_SAMPLES)
                                    {
                                        if (playerStarted) xmp_end_player(ctx);
                                        free(sampleToProgram);
                                        xmp_release_module(ctx);
                                        xmp_free_context(ctx);
                                        return 0;
                                    }
                                    sampleToProgram[sid] = (int)nextProgram;
                                    nextProgram++;
                                }

                                program = sampleToProgram[sid];

                                if (!mod2rmf_flush_active_note(song, (uint16_t)ch, &activeNotes[ch], currentTickFP))
                                {
                                    if (playerStarted) xmp_end_player(ctx);
                                    free(sampleToProgram);
                                    xmp_release_module(ctx);
                                    xmp_free_context(ctx);
                                    return 0;
                                }

                                baseNote = ((int)ci->note > 0) ? (int)ci->note : (int)evNote;
                                noteXpo = mod2rmf_resolve_note_transpose(mod,
                                                                         ci,
                                                                         sid,
                                                                         sampleTranspose,
                                                                         conv->rawSamples,
                                                                         conv->rawSampleCount);
                                midiNote = baseNote + mod2rmf_tracker_note_bias(conv) + noteXpo;

                                /* IT DCT: cut/off/fade duplicates on other channels
                                 * before this note starts (e.g. singing DCT=INST). */
                                if (conv->rawSamples[sid].hasNotePolicy &&
                                    conv->rawSamples[sid].dct != XMP_INST_DCT_OFF)
                                {
                                    unsigned char dctNote = (unsigned char)mod2rmf_clamp_int(midiNote, 0, 127);
                                    uint8_t dctInst = (uint8_t)((ci->instrument <= 255u) ? ci->instrument : 255u);
                                    if (!mod2rmf_flush_dct_duplicates(song,
                                                                      activeNotes,
                                                                      song->channelCount,
                                                                      (uint16_t)ch,
                                                                      (uint8_t)program,
                                                                      dctInst,
                                                                      dctNote,
                                                                      conv->rawSamples[sid].dct,
                                                                      currentTickFP))
                                    {
                                        if (playerStarted) xmp_end_player(ctx);
                                        free(sampleToProgram);
                                        xmp_release_module(ctx);
                                        xmp_free_context(ctx);
                                        return 0;
                                    }
                                }

                                activeNotes[ch].active = TRUE;
                                activeNotes[ch].startTickFP = currentTickFP;
                                activeNotes[ch].note = (unsigned char)mod2rmf_clamp_int(midiNote, 0, 127);
                                activeNotes[ch].velocity = mod2rmf_note_velocity_from_volume(hasRowVolumeCmd
                                                                                               ? rowVolumeCmd64
                                                                                               : (uint8_t)mod2rmf_clamp_int((int)ci->volume, 0, 64));
                                activeNotes[ch].program = (uint8_t)program;
                                activeNotes[ch].instrument =
                                    (uint8_t)((ci->instrument <= 255u) ? ci->instrument : 255u);
                                activeNotes[ch].bendOffsetCents = (midiNote - (int)activeNotes[ch].note) * 100;
                                activeNotes[ch].rateFinCents =
                                    (sid >= 0 && (uint32_t)sid < conv->rawSampleCount &&
                                     conv->rawSamples[sid].hasRateMapping)
                                        ? mod2rmf_rate_fin_to_cents(conv->rawSamples[sid].rateFin)
                                        : 0;
                                activeNotes[ch].sampleOffsetFrames = sampleOffsetFrames;
                                chLastBend[ch] = 0xFFFFu;
                                {
                                    uint16_t bend;
                                    bend = mod2rmf_pitchbend_to_midi(ci->pitchbend -
                                                                        activeNotes[ch].rateFinCents +
                                                                        activeNotes[ch].bendOffsetCents,
                                                                    song->pitchBendRangeSemitones);
                                    chLastBend[ch] = bend;
                                    (void)mod2rmf_song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                                        activeNotes[ch].program);
#if _DEBUG == TRUE
                                    if (ch == 9u)
                                    {
                                        fprintf(stderr,
                                                "[mod2rmf][dbg][srcch9] note-on tick=%u evNote=%u ciNote=%d sid=%d inst=%u base=%d xpo=%d midi=%d clamp=%u bendOff=%d ciBend=%d bend=0x%04X prog=%u\n",
                                                (unsigned)tick,
                                                (unsigned)evNote,
                                                (int)ci->note,
                                                sid,
                                                (unsigned)ci->instrument,
                                                baseNote,
                                                noteXpo,
                                                midiNote,
                                                (unsigned)activeNotes[ch].note,
                                                activeNotes[ch].bendOffsetCents,
                                                (int)ci->pitchbend,
                                                (unsigned)bend,
                                                (unsigned)activeNotes[ch].program);
                                    }
#endif
                                }
                            }
                        }
                    }
                }

                /* ---- Non-zero frames: handle note delay trigger and retrigger ---- */
                if (fi.frame > 0)
                {
                    /* Note delay: trigger the deferred note-on at the correct frame. */
                    if (chEffects[ch].hasDelayedNote &&
                        (uint8_t)fi.frame == chEffects[ch].noteDelayFrames)
                    {
                        int delaySid = chEffects[ch].delayedSid;
                        if (delaySid >= 0 && delaySid < (int)conv->rawSampleCount &&
                            conv->rawSamples[delaySid].valid)
                        {
                            int baseNote;
                            int midiNote;
                            int noteXpo;

                            if (sampleToProgram[delaySid] < 0)
                            {
                                if (nextProgram >= MOD2RMF_MAX_SAMPLES)
                                {
                                    if (playerStarted) xmp_end_player(ctx);
                                    free(sampleToProgram);
                                    xmp_release_module(ctx);
                                    xmp_free_context(ctx);
                                    return 0;
                                }
                                sampleToProgram[delaySid] = (int)nextProgram;
                                nextProgram++;
                            }

                            program = sampleToProgram[delaySid];

                            if (!mod2rmf_flush_active_note(song, (uint16_t)ch, &activeNotes[ch], currentTickFP))
                            {
                                if (playerStarted) xmp_end_player(ctx);
                                free(sampleToProgram);
                                xmp_release_module(ctx);
                                xmp_free_context(ctx);
                                return 0;
                            }

                            baseNote = ((int)ci->note > 0)
                                            ? (int)ci->note
                                            : (int)chEffects[ch].delayedEvNote;
                            noteXpo = mod2rmf_resolve_note_transpose(mod,
                                                                     ci,
                                                                     delaySid,
                                                                     sampleTranspose,
                                                                     conv->rawSamples,
                                                                     conv->rawSampleCount);
                            midiNote = baseNote + mod2rmf_tracker_note_bias(conv) + noteXpo;

                            if (delaySid >= 0 && (uint32_t)delaySid < conv->rawSampleCount &&
                                conv->rawSamples[delaySid].hasNotePolicy &&
                                conv->rawSamples[delaySid].dct != XMP_INST_DCT_OFF)
                            {
                                unsigned char dctNote = (unsigned char)mod2rmf_clamp_int(midiNote, 0, 127);
                                if (!mod2rmf_flush_dct_duplicates(song,
                                                                  activeNotes,
                                                                  song->channelCount,
                                                                  (uint16_t)ch,
                                                                  (uint8_t)program,
                                                                  chEffects[ch].delayedInstrument,
                                                                  dctNote,
                                                                  conv->rawSamples[delaySid].dct,
                                                                  currentTickFP))
                                {
                                    if (playerStarted) xmp_end_player(ctx);
                                    free(sampleToProgram);
                                    xmp_release_module(ctx);
                                    xmp_free_context(ctx);
                                    return 0;
                                }
                            }

                            activeNotes[ch].active = TRUE;
                            activeNotes[ch].startTickFP = currentTickFP;
                            activeNotes[ch].note = (unsigned char)mod2rmf_clamp_int(midiNote, 0, 127);
                            activeNotes[ch].velocity = mod2rmf_note_velocity_from_volume(chEffects[ch].delayedVolume);
                            activeNotes[ch].program = (uint8_t)program;
                            activeNotes[ch].instrument = chEffects[ch].delayedInstrument;
                            activeNotes[ch].bendOffsetCents = (midiNote - (int)activeNotes[ch].note) * 100;
                            activeNotes[ch].rateFinCents =
                                (delaySid >= 0 && (uint32_t)delaySid < conv->rawSampleCount &&
                                 conv->rawSamples[delaySid].hasRateMapping)
                                    ? mod2rmf_rate_fin_to_cents(conv->rawSamples[delaySid].rateFin)
                                    : 0;
                            activeNotes[ch].sampleOffsetFrames = chEffects[ch].delayedSampleOffsetFrames;
                            chLastBend[ch] = 0xFFFFu;
                            {
                                uint16_t bend;
                                bend = mod2rmf_pitchbend_to_midi(ci->pitchbend -
                                                                    activeNotes[ch].rateFinCents +
                                                                    activeNotes[ch].bendOffsetCents,
                                                                song->pitchBendRangeSemitones);
                                chLastBend[ch] = bend;
                                (void)mod2rmf_song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                                    activeNotes[ch].program);
#if _DEBUG == TRUE
                                if (ch == 9u)
                                {
                                    fprintf(stderr,
                                            "[mod2rmf][dbg][srcch9] delayed-note tick=%u evNote=%u ciNote=%d sid=%d inst=%u base=%d xpo=%d midi=%d clamp=%u bendOff=%d ciBend=%d bend=0x%04X prog=%u delay=%u\n",
                                            (unsigned)tick,
                                            (unsigned)chEffects[ch].delayedEvNote,
                                            (int)ci->note,
                                            delaySid,
                                            (unsigned)ci->instrument,
                                            baseNote,
                                            noteXpo,
                                            midiNote,
                                            (unsigned)activeNotes[ch].note,
                                            activeNotes[ch].bendOffsetCents,
                                            (int)ci->pitchbend,
                                            (unsigned)bend,
                                            (unsigned)activeNotes[ch].program,
                                            (unsigned)chEffects[ch].noteDelayFrames);
                                }
#endif
                            }
                        }
                        chEffects[ch].hasDelayedNote = FALSE;
                    }

                    /* Retrigger: fire a new note-on at each interval boundary. */
                    if (chEffects[ch].retrigInterval > 0 &&
                        ((uint8_t)fi.frame % chEffects[ch].retrigInterval) == 0 &&
                        activeNotes[ch].active)
                    {
                        uint32_t savedOffsetFrames = activeNotes[ch].sampleOffsetFrames;

                        if (!mod2rmf_flush_active_note(song, (uint16_t)ch, &activeNotes[ch], currentTickFP))
                        {
                            if (playerStarted) xmp_end_player(ctx);
                            free(sampleToProgram);
                            xmp_release_module(ctx);
                            xmp_free_context(ctx);
                            return 0;
                        }

                        /* Re-trigger the same note with current volume from libxmp
                        * (which already applies Rxy volume table changes). */
                        activeNotes[ch].active = TRUE;
                        activeNotes[ch].startTickFP = currentTickFP;
                        /* note, program, and sample offset stay the same from the row's note-on */
                        activeNotes[ch].sampleOffsetFrames = savedOffsetFrames;
                        activeNotes[ch].velocity = mod2rmf_note_velocity_from_volume((uint8_t)mod2rmf_clamp_int((int)ci->volume, 0, 64));
                        chLastBend[ch] = 0xFFFFu;
                        {
                            uint16_t bend;
                            bend = mod2rmf_pitchbend_to_midi(ci->pitchbend -
                                                                activeNotes[ch].rateFinCents +
                                                                activeNotes[ch].bendOffsetCents,
                                                            song->pitchBendRangeSemitones);
                            chLastBend[ch] = bend;
                            (void)mod2rmf_song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                                activeNotes[ch].program);
                        }
                    }
                }

                /* If the note-on changed the active program, patch the CC
                 * and pitch bend events we emitted earlier this frame so
                 * they carry the correct program tag for spread-mode
                 * routing. */
                if (activeNotes[ch].program != preNoteProg)
                {
                    uint32_t j;
                    for (j = preNoteCCIdx; j < song->ccCount; ++j)
                    {
                        song->ccEvents[j].program = activeNotes[ch].program;
                    }
                    for (j = preNoteBendIdx; j < song->pitchBendCount; ++j)
                    {
                        song->pitchBendEvents[j].program = activeNotes[ch].program;
                    }
                }
            } /* end preNoteCCIdx scope */
        }

        currentTickFP += tickPerFrameFP;
        frameGuard++;
    }

    if (playerStarted)
    {
        xmp_end_player(ctx);
    }

    for (i = 0; i < song->channelCount; ++i)
    {
        if (!mod2rmf_flush_active_note(song, (uint16_t)i, &activeNotes[i], currentTickFP))
        {
            free(sampleToProgram);
            xmp_release_module(ctx);
            xmp_free_context(ctx);
            return 0;
        }
    }

    /* Programs are assigned densely as samples are used, so playableCount is exact. */
    song->playableCount = nextProgram;
    if (song->playableCount > 0)
    {
        song->playables = (ModPlayable *)calloc(song->playableCount, sizeof(ModPlayable));
        if (!song->playables)
        {
            free(sampleToProgram);
            xmp_release_module(ctx);
            xmp_free_context(ctx);
            return 0;
        }
    }
    for (i = 0; i < conv->rawSampleCount; ++i)
    {
        ModPlayable *p;
        int program;
        char tmp[64];

        program = sampleToProgram ? sampleToProgram[i] : -1;
        if (program < 0 || (uint32_t)program >= song->playableCount)
        {
            continue;
        }
        p = &song->playables[program];
        p->sourceSlot = i;
        p->program = (unsigned char)program;
        /* Includes octave-fold root adjust for high-C5 IT/S3M samples. */
        p->rootKey = mod2rmf_sample_root_key_for_raw(&conv->rawSamples[i], 0u);
        p->hasSampleRateOverride = TRUE;
        p->sampleRateOverrideHz =
            (conv->rawSamples[i].sampleRateHz > 0u) ? conv->rawSamples[i].sampleRateHz
                                                    : conv->moduleBaseRateHz;
        
        p->sampleOffsetBytes = 0;
        p->offsetVariant = FALSE;
        p->rawSample = &conv->rawSamples[i];

        /* If the instrument has an amplitude envelope, map it to a BAE
         * ADSR so the engine produces smooth volume shaping instead of
         * relying on per-frame CC7 events.  The per-frame CC7 tracking
         * is correspondingly suppressed for enveloped channels. */
        if (conv->rawSamples[i].hasEnvelope &&
            conv->rawSamples[i].adsrStageCount > 0)
        {
            p->hasVolumeAdsr = TRUE;
            p->adsrStageCount = conv->rawSamples[i].adsrStageCount;
            memcpy(p->adsrStages, conv->rawSamples[i].adsrStages,
                   conv->rawSamples[i].adsrStageCount * sizeof(ModAdsrStage));
        }

        if (conv->rawSamples[i].hasPitchEnv &&
            conv->rawSamples[i].pitchEnvStageCount > 0)
        {
            p->hasPitchEnv = TRUE;
            p->pitchEnvStageCount = conv->rawSamples[i].pitchEnvStageCount;
            p->pitchEnvPeakAbsY = conv->rawSamples[i].pitchEnvPeakAbsY;
            memcpy(p->pitchEnvStages, conv->rawSamples[i].pitchEnvStages,
                   conv->rawSamples[i].pitchEnvStageCount * sizeof(ModAdsrStage));
        }
        if (conv->rawSamples[i].hasFilterEnv &&
            conv->rawSamples[i].filterEnvStageCount > 0)
        {
            p->hasFilterEnv = TRUE;
            p->filterEnvStageCount = conv->rawSamples[i].filterEnvStageCount;
            p->filterEnvPeakAbsY = conv->rawSamples[i].filterEnvPeakAbsY;
            memcpy(p->filterEnvStages, conv->rawSamples[i].filterEnvStages,
                   conv->rawSamples[i].filterEnvStageCount * sizeof(ModAdsrStage));
        }
        if (conv->rawSamples[i].hasLpf)
        {
            p->hasLpf = TRUE;
            p->lpfFrequency = conv->rawSamples[i].lpfFrequency;
            p->lpfResonance = conv->rawSamples[i].lpfResonance;
            p->lpfAmount = conv->rawSamples[i].lpfAmount;
        }
        if (conv->rawSamples[i].hasVibrato)
        {
            p->hasVibrato = TRUE;
            p->vibPeriodUs = conv->rawSamples[i].vibPeriodUs;
            p->vibLevel = conv->rawSamples[i].vibLevel;
            p->vibWaveShape = conv->rawSamples[i].vibWaveShape;
            p->vibSweepUs = conv->rawSamples[i].vibSweepUs;
        }

        /* Map sub-instrument default pan to BAE panPlacement */
        if (conv->rawSamples[i].defaultPan >= 0)
        {
            p->panPlacement = (int8_t)((int)conv->rawSamples[i].defaultPan - 128);
        }

        if (conv->rawSamples[i].name[0])
        {
            snprintf(p->displayName, sizeof(p->displayName), "%s", conv->rawSamples[i].name);
        }
        else
        {
            snprintf(tmp, sizeof(tmp), "Sample %u", (unsigned)(i + 1u));
            mod2rmf_trim_copy_ascii(p->displayName, sizeof(p->displayName), tmp);
        }
    }

    free(sampleToProgram);
    xmp_release_module(ctx);
    xmp_free_context(ctx);
    return 1;
}

int mod2rmf_save_document(Mod2RmfConverter *conv, const char *destPath)
{
    FILE *outFile;
    unsigned char *rmfData;
    uint32_t rmfSize;
    BAEResult result;
    const char *ext;
    bool useZmfContainer;
    bool requiresZmf;

    if (!conv || !conv->document || !destPath)
    {
        return 0;
    }

    ext = strrchr(destPath, '.');
    useZmfContainer = (ext && (!strcmp(ext, ".zmf") || !strcmp(ext, ".ZMF"))) ? TRUE : FALSE;
    uint32_t reason;
    requiresZmf = BAERmfEditorDocument_RequiresZmf(conv->document, &reason);
#if USE_ZMF_SUPPORT == TRUE
    if (requiresZmf && !useZmfContainer)
    {
        char reasonBuf[256];
        BAEZMFReasonCodeToString(reason, reasonBuf, sizeof(reasonBuf));
        fprintf(stderr,
                "[mod2rmf] Error: document requires ZMF format due to RMF-incompatible options \n"
                "[mod2rmf] Reason(s): %s\n"
                "[mod2rmf] Please use a .zmf output extension.\n",
                reasonBuf);
        return 0;
    }
#else
    if (requiresZmf)
    {
        fprintf(stderr,
                "[mod2rmf] Error: document requires ZMF format but ZMF support is not compiled in.\n"
                "[mod2rmf] Please re-compile with ZMF support to be able to support this document.\n");
        return 0;
    }
#endif
    rmfData = NULL;
    rmfSize = 0;
    result = BAERmfEditorDocument_SaveAsRmfToMemory(conv->document,
                                                    useZmfContainer,
                                                    &rmfData,
                                                    &rmfSize);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "[mod2rmf] Error: save failed (%d): %s\n", (int)result, destPath);
        return 0;
    }

    outFile = fopen(destPath, "wb");
    if (!outFile)
    {
        XDisposePtr((XPTR)rmfData);
        return 0;
    }
    if (fwrite(rmfData, 1, rmfSize, outFile) != rmfSize)
    {
        fclose(outFile);
        XDisposePtr((XPTR)rmfData);
        return 0;
    }
    fclose(outFile);
    XDisposePtr((XPTR)rmfData);

    return 1;
}

Mod2RmfConverter *mod2rmf_converter_create(void)
{
    Mod2RmfConverter *conv;
    conv = (Mod2RmfConverter *)malloc(sizeof(Mod2RmfConverter));
    if (conv)
    {
        memset(conv, 0, sizeof(*conv));
        conv->moduleBaseRateHz = MOD2RMF_SAMPLE_RATE;
        conv->isMod = FALSE;
        conv->sampleGainDb = 0.0;
        conv->rootShiftSemitones = MOD2RMF_DEFAULT_ROOT_SHIFT_ST;
        conv->itV00CutRows = 6;
        conv->stereoSeparation = 75;
        conv->maxAdsrStages = (uint8_t)BAE_RMF_MAX_ADSR_STAGES;
        mod2rmf_resampler_defaults(&conv->resamplerSettings);
    }
    return conv;
}

void mod2rmf_converter_delete(Mod2RmfConverter *conv)
{
    uint32_t i;
    if (!conv)
    {
        return;
    }

    if (conv->document)
    {
        BAERmfEditorDocument_Delete(conv->document);
    }
    free(conv->sourceData);
    free(conv->channelToTrackIndex);

    if (conv->rawSamples)
    {
        for (i = 0; i < conv->rawSampleCount; ++i)
        {
            free(conv->rawSamples[i].pcm8);
            free(conv->rawSamples[i].pcm16);
        }
        free(conv->rawSamples);
    }

    free(conv);
}

int mod2rmf_flush_active_note(ModSongModel *song,
                             uint16_t sourceChannel,
                             ActiveNote *note,
                             uint64_t endTickFP)
{
    uint32_t startTick;
    uint32_t endTick;
    uint32_t duration;

    if (!note || !note->active)
    {
        return 1;
    }

    startTick = mod2rmf_fp_ticks_to_int(note->startTickFP);
    endTick = mod2rmf_fp_ticks_to_int(endTickFP);
    duration = (endTick > startTick) ? (endTick - startTick) : 1u;

    if (!mod2rmf_song_model_append_note(song,
                                sourceChannel,
                                startTick,
                                duration,
                                note->note,
                                note->velocity,
                                note->program,
                                note->sampleOffsetFrames))
    {
        return 0;
    }

    note->active = FALSE;
    return 1;
}

int mod2rmf_write_song_tempo_events(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;

    if (!conv || !song)
    {
        return 0;
    }

    for (i = 0; i < song->tempoChangeCount; ++i)
    {
        const ModTempoChange *tc;
        uint32_t microsecondsPerBeat;

        tc = &song->tempoChanges[i];
        if (tc->bpm == 0)
        {
            continue;
        }
        microsecondsPerBeat = 60000000UL / tc->bpm;
        BAERmfEditorDocument_AddTempoEvent(conv->document, tc->tick, microsecondsPerBeat);
    }

    return 1;
}

int mod2rmf_write_song_cc_events(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;
    /* Last emitted CC values: [source track][cc] - 0xFFFF = not yet emitted */
    uint16_t lastCC[MOD2RMF_MAX_CHANNELS][128];

    if (!conv || !song)
    {
        return 0;
    }

    bool ccDedupReset = FALSE;
    memset(lastCC, 0xFF, sizeof(lastCC));

    for (i = 0; i < song->ccCount; ++i)
    {
        const ModCCEvent *ev;
        uint16_t trackIndex;
        uint8_t midiCh;
        uint32_t j;
        bool superseded = FALSE;

        ev = &song->ccEvents[i];
        if (ev->sourceChannel >= song->channelCount) continue;

        /* When we reach the loop start tick, reset dedup so every CC
         * value is re-emitted.  The engine's channel state at loop-back
         * comes from the end of the song, which may differ from what
         * was active at the loop start on the first playthrough. */
        if (song->loopEnabled && !ccDedupReset &&
            ev->tick >= song->loopStartTick)
        {
            memset(lastCC, 0xFF, sizeof(lastCC));
            ccDedupReset = TRUE;
        }

        trackIndex = conv->channelToTrackIndex[ev->sourceChannel];
        if (trackIndex == (uint16_t)0xFFFF) continue;

        midiCh = conv->channelMap.trackerToMidi[ev->sourceChannel];
        if (midiCh >= MOD2RMF_MAX_MIDI_CHANNELS) continue;

        /* When multiple source tracks share a MIDI channel, GenSeq applies
         * controllers in track order at the same tick.  Keep only the last
         * CC of each type for that MIDI channel (mirrors pitch-bend write). */
        for (j = i + 1; j < song->ccCount; ++j)
        {
            const ModCCEvent *nextEv = &song->ccEvents[j];
            if (nextEv->tick > ev->tick) break;
            if (nextEv->tick == ev->tick &&
                nextEv->cc == ev->cc &&
                nextEv->sourceChannel < song->channelCount &&
                conv->channelMap.trackerToMidi[nextEv->sourceChannel] == midiCh &&
                conv->channelToTrackIndex[nextEv->sourceChannel] != (uint16_t)0xFFFF)
            {
                superseded = TRUE;
                break;
            }
        }
        if (superseded)
        {
            continue;
        }

        /* Skip if this CC value was already emitted for this source track */
        if (lastCC[ev->sourceChannel][ev->cc] == (uint16_t)ev->value)
        {
            continue;
        }
        lastCC[ev->sourceChannel][ev->cc] = (uint16_t)ev->value;

        (void)BAERmfEditorDocument_AddTrackCCEvent(conv->document,
                                                   trackIndex,
                                                   ev->cc,
                                                   ev->tick,
                                                   ev->value);
    }

    return 1;
}

int mod2rmf_write_song_pitch_bend_events(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;
    /* Deduplication per source track */
    uint16_t lastBend[MOD2RMF_MAX_CHANNELS];

    if (!conv || !song)
    {
        return 0;
    }

#if _DEBUG == TRUE
    {
        uint32_t ch;
        for (ch = 0; ch < song->channelCount; ++ch)
        {
            if (conv->channelMap.trackerToMidi[ch] == 9u)
            {
                fprintf(stderr,
                        "[mod2rmf][dbg][ch10] tracker ch %u routes to MIDI ch10, track=%u\n",
                        (unsigned)ch,
                        (unsigned)conv->channelToTrackIndex[ch]);
            }
        }
    }
#endif

    /* 0xFFFF = not yet emitted */
    bool bendDedupReset = FALSE;
    memset(lastBend, 0xFF, sizeof(lastBend));

    for (i = 0; i < song->pitchBendCount; ++i)
    {
        const ModPitchBendEvent *ev;
        uint8_t midiCh;
        uint16_t trackIndex;
        uint32_t j;
        bool superseded = FALSE;

        ev = &song->pitchBendEvents[i];
        if (ev->sourceChannel >= song->channelCount) continue;

        /* Reset dedup at loop start so bend state is re-established */
        if (song->loopEnabled && !bendDedupReset &&
            ev->tick >= song->loopStartTick)
        {
            memset(lastBend, 0xFF, sizeof(lastBend));
            bendDedupReset = TRUE;
        }

        midiCh = conv->channelMap.trackerToMidi[ev->sourceChannel];
        if (midiCh >= MOD2RMF_MAX_MIDI_CHANNELS) continue;

        trackIndex = conv->channelToTrackIndex[ev->sourceChannel];
        if (trackIndex == (uint16_t)0xFFFF) continue;

        /* Look ahead to see if there's another bend for this MIDI channel at the same tick.
         * If there is, we skip this one and let the later one take effect. */
        for (j = i + 1; j < song->pitchBendCount; ++j)
        {
            const ModPitchBendEvent *nextEv = &song->pitchBendEvents[j];
            if (nextEv->tick > ev->tick) break; /* Events are strictly sorted by tick */
            if (nextEv->tick == ev->tick)
            {
                if (nextEv->sourceChannel < song->channelCount &&
                    conv->channelMap.trackerToMidi[nextEv->sourceChannel] == midiCh)
                {
#if _DEBUG == TRUE
                    if (midiCh == 9u)
                    {
                        fprintf(stderr,
                                "[mod2rmf][dbg][ch10] supersede bend tick=%u srcCh=%u val=0x%04X by srcCh=%u val=0x%04X\n",
                                (unsigned)ev->tick,
                                (unsigned)ev->sourceChannel,
                                (unsigned)ev->value,
                                (unsigned)nextEv->sourceChannel,
                                (unsigned)nextEv->value);
                    }
#endif
                    superseded = TRUE;
                    break;
                }
            }
        }
        
        if (superseded) {
            continue;
        }

        /* Skip if bend value hasn't changed for this source track */
        if (lastBend[ev->sourceChannel] == ev->value) {
            continue;
        }
        lastBend[ev->sourceChannel] = ev->value;

    #if _DEBUG == TRUE
        if (midiCh == 9u)
        {
            fprintf(stderr,
                "[mod2rmf][dbg][ch10] emit bend tick=%u srcCh=%u track=%u val=0x%04X\n",
                (unsigned)ev->tick,
                (unsigned)ev->sourceChannel,
                (unsigned)trackIndex,
                (unsigned)ev->value);
        }
    #endif

        (void)BAERmfEditorDocument_AddTrackPitchBendEvent(conv->document,
                                                           trackIndex,
                                                           ev->tick,
                                                           ev->value);
    }

    return 1;
}

int mod2rmf_add_programmed_note(Mod2RmfConverter *conv,
                               uint16_t trackIndex,
                               uint32_t startTick,
                               uint32_t durationTicks,
                               unsigned char note,
                               unsigned char velocity,
                               unsigned char program)
{
    BAEResult result;
    uint32_t noteCount;
    BAERmfEditorNoteInfo info;

    if (!conv || trackIndex == (uint16_t)0xFFFF)
    {
        return 0;
    }

    result = BAERmfEditorDocument_AddNote(conv->document,
                                          trackIndex,
                                          startTick,
                                          durationTicks,
                                          note,
                                          velocity);
    if (result != BAE_NO_ERROR)
    {
        return 0;
    }

    noteCount = 0;
    if (BAERmfEditorDocument_GetNoteCount(conv->document, trackIndex, &noteCount) != BAE_NO_ERROR || noteCount == 0)
    {
        return 0;
    }
    if (BAERmfEditorDocument_GetNoteInfo(conv->document, trackIndex, noteCount - 1, &info) != BAE_NO_ERROR)
    {
        return 0;
    }

    info.bank = MOD2RMF_EMBEDDED_BANK;
    info.program = program;
    return BAERmfEditorDocument_SetNoteInfo(conv->document, trackIndex, noteCount - 1, &info) == BAE_NO_ERROR;
}

/* Emit ZMF-only NRPN 6,0 + three CC6 digits for a 21-bit frame offset. */
static void mod2rmf_emit_sample_offset_nrpn(Mod2RmfConverter *conv,
                                           uint16_t trackIndex,
                                           uint32_t tick,
                                           uint32_t offsetFrames)
{
    uint32_t o;
    unsigned char b0, b1, b2;

    if (!conv || trackIndex == (uint16_t)0xFFFF)
    {
        return;
    }

    o = offsetFrames & MOD2RMF_SAMPLE_OFFSET_MAX_FRAMES;
    b0 = (unsigned char)((o >> 14) & 0x7Fu);
    b1 = (unsigned char)((o >> 7) & 0x7Fu);
    b2 = (unsigned char)(o & 0x7Fu);

    (void)BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 99, tick, 6);
    (void)BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 98, tick, 0);
    (void)BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 6, tick, b0);
    (void)BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 6, tick, b1);
    (void)BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 6, tick, b2);
}

int mod2rmf_write_loop_program_resets(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t ch;

    if (!conv || !song)
    {
        return 0;
    }
    if (!song->loopEnabled)
    {
        return 1;
    }

    /* Emit before notes so aux PC eventOrder sorts ahead of note-ons at the
     * same tick.  Explicit aux PCs are always serialized (unlike per-note
     * program injection, which skips when currentProgram already matches). */
    for (ch = 0; ch < song->channelCount && ch < MOD2RMF_MAX_CHANNELS; ++ch)
    {
        uint16_t trackIndex;
        uint8_t program;

        program = song->loopProgramReset[ch];
        if (program == 0xFF)
        {
            continue;
        }
        trackIndex = conv->channelToTrackIndex[ch];
        if (trackIndex == (uint16_t)0xFFFF)
        {
            continue;
        }
        if (BAERmfEditorDocument_AddTrackProgramChange(conv->document,
                                                       trackIndex,
                                                       song->loopStartTick,
                                                       program) != BAE_NO_ERROR)
        {
            return 0;
        }
    }

    return 1;
}

int mod2rmf_write_song_notes(Mod2RmfConverter *conv, const ModSongModel *song, bool useZmfContainer)
{
    uint32_t i;
    uint32_t lastEmittedOffset[MOD2RMF_MAX_CHANNELS];

    if (!conv || !song)
    {
        return 0;
    }

    memset(lastEmittedOffset, 0, sizeof(lastEmittedOffset));

    for (i = 0; i < song->noteCount; ++i)
    {
        const ModNoteEvent *note;
        uint16_t trackIndex;
        uint8_t midiCh;
        uint32_t wantOffset;

        note = &song->notes[i];
        if (note->sourceChannel >= song->channelCount)
        {
            continue;
        }
        midiCh = conv->channelMap.trackerToMidi[note->sourceChannel];
        trackIndex = conv->channelToTrackIndex[note->sourceChannel];
        if (trackIndex == (uint16_t)0xFFFF)
        {
            continue;
        }

        wantOffset = note->sampleOffsetFrames & MOD2RMF_SAMPLE_OFFSET_MAX_FRAMES;
        if (useZmfContainer &&
            note->sourceChannel < MOD2RMF_MAX_CHANNELS &&
            lastEmittedOffset[note->sourceChannel] != wantOffset)
        {
            mod2rmf_emit_sample_offset_nrpn(conv, trackIndex, note->startTick, wantOffset);
            lastEmittedOffset[note->sourceChannel] = wantOffset;
        }

#if _DEBUG == TRUE
        if (midiCh == 9u && note->startTick == 0u)
        {
            fprintf(stderr,
                    "[mod2rmf][dbg][ch10] tick0 note srcCh=%u track=%u note=%u dur=%u prog=%u\n",
                    (unsigned)note->sourceChannel,
                    (unsigned)trackIndex,
                    (unsigned)note->note,
                    (unsigned)note->durationTicks,
                    (unsigned)note->program);
        }
#endif

        (void)mod2rmf_add_programmed_note(conv,
                                  trackIndex,
                                  note->startTick,
                                  note->durationTicks,
                                  note->note,
                                  note->velocity,
                                  note->program);
    }

    return 1;
}

int mod2rmf_setup_instrument_ext(Mod2RmfConverter *conv, const ModSongModel *song, bool useZmfContainer)
{
    uint32_t i;

    if (!conv || !conv->document || !song)
    {
        return 0;
    }

    for (i = 0; i < song->playableCount; ++i)
    {
        BAERmfEditorInstrumentExtInfo extInfo;
        BAEResult result;
        uint32_t instID;
        const ModPlayable *playable;
        unsigned char instRootKey;

        playable = &song->playables[i];
        if (!playable->rawSample || !playable->rawSample->valid ||
            !playable->rawSample->pcm8 || playable->rawSample->frameCount == 0)
        {
            continue;
        }
        instID = 512u + (uint32_t)playable->program;
        /* Keep instrument masterRootKey at logical middle C; sample baseKey
         * carries any high-C5 octave-fold adjust. */
        instRootKey = mod2rmf_shifted_root_key(MOD2RMF_LOGICAL_ROOT_KEY,
                                               conv->rootShiftSemitones);

        memset(&extInfo, 0, sizeof(extInfo));
        result = BAERmfEditorDocument_GetInstrumentExtInfo(conv->document, instID, &extInfo);
        if (result != BAE_NO_ERROR)
        {
            /* Continue with safe defaults if the get call fails. */
            memset(&extInfo, 0, sizeof(extInfo));
            extInfo.instID = instID;
            extInfo.flags1 = MOD2RMF_ZBF_USE_SAMPLE_RATE;
            extInfo.flags2 = 0;
            extInfo.midiRootKey = instRootKey;
            extInfo.miscParameter2 = 100;
            /* 2-stage ADSR: sustain at max, then immediate release to 0.
             * This activates the "new style" ADSR path in the engine,
             * which frees voices immediately on note-off instead of
             * lingering for 8 render cycles (the "old style" NoteDecay
             * path).  Without this, CC#7 changes during the 8-cycle
             * decay bleed into the releasing voice. */
            extInfo.volumeADSR.stageCount = 2;
            extInfo.volumeADSR.stages[0].level = VOLUME_RANGE;
            extInfo.volumeADSR.stages[0].time = 0;
            extInfo.volumeADSR.stages[0].flags = ADSR_SUSTAIN_LONG;
            extInfo.volumeADSR.stages[1].level = 0;
            extInfo.volumeADSR.stages[1].time = 0;
            extInfo.volumeADSR.stages[1].flags = ADSR_TERMINATE_LONG;
        }

        extInfo.instID = instID;
        extInfo.displayName = playable->displayName;
        extInfo.midiRootKey = instRootKey;

        /* Apply per-instrument pan placement from tracker sub-instrument */
        extInfo.panPlacement = playable->panPlacement;

        if (playable->hasVolumeAdsr && playable->adsrStageCount > 0)
        {
            /* Multi-stage envelope: each stage was pre-built by extract_envelope_adsr
             * with level, timeUs, and flags already in BAE format. */
            uint32_t s;
            extInfo.volumeADSR.stageCount = playable->adsrStageCount;
            for (s = 0; s < playable->adsrStageCount && s < BAE_EDITOR_MAX_ADSR_STAGES; ++s)
            {
                extInfo.volumeADSR.stages[s].level = playable->adsrStages[s].level;
                extInfo.volumeADSR.stages[s].time = playable->adsrStages[s].timeUs;
                extInfo.volumeADSR.stages[s].flags = playable->adsrStages[s].flags;
            }
        }
        else
        {
            /* MOD fallback: sustain at max then immediate release. */
            extInfo.volumeADSR.stageCount = 2;
            extInfo.volumeADSR.stages[0].level = VOLUME_RANGE;
            extInfo.volumeADSR.stages[0].time = 0;
            extInfo.volumeADSR.stages[0].flags = ADSR_SUSTAIN_LONG;
            extInfo.volumeADSR.stages[1].level = 0;
            extInfo.volumeADSR.stages[1].time = 0;
            extInfo.volumeADSR.stages[1].flags = ADSR_TERMINATE_LONG;
        }

        extInfo.flags1 |= MOD2RMF_ZBF_USE_SAMPLE_RATE;
        /* RMF uses normal interpolation; advanced interpolation is ZMF-only
         * and only valid for the processed 16-bit sample path. */
        if (useZmfContainer && !conv->forceOriginalSamples)
        {
            extInfo.flags1 &= (unsigned char)~MOD2RMF_ZBF_ENABLE_INTERPOLATE;
            extInfo.flags2 |= MOD2RMF_ZBF_ADVANCED_INTERPOLATION;
        }
        else
        {
            extInfo.flags1 |= MOD2RMF_ZBF_ENABLE_INTERPOLATE;
            extInfo.flags2 &= (unsigned char)~MOD2RMF_ZBF_ADVANCED_INTERPOLATION;
        }
        extInfo.flags2 &= (unsigned char)~MOD2RMF_ZBF_PLAY_AT_SAMPLED_FREQ;
        extInfo.flags1 &= (unsigned char)~(MOD2RMF_ZBF_DISABLE_SND_LOOPING | MOD2RMF_ZBF_SAMPLE_AND_HOLD);
        /* Match what the instrument editor produces on OK for plain samples:
         * do not force miscParameter1 as root key. */
        extInfo.flags2 &= (unsigned char)~MOD2RMF_ZBF_USE_SMOD_AS_ROOTKEY;

        if (useZmfContainer && playable->offsetVariant)
        {
            uint32_t offsetFrames;
            offsetFrames = playable->sampleOffsetBytes;
            extInfo.flags2 |= MOD2RMF_ZBF_ENABLE_SAMPLE_OFFSET_START;
            extInfo.miscParameter1 = (int16_t)((offsetFrames >> 16) & 0xFFFFu);
            extInfo.miscParameter2 = (int16_t)(offsetFrames & 0xFFFFu);
        }
        else
        {
            extInfo.flags2 &= (unsigned char)~MOD2RMF_ZBF_ENABLE_SAMPLE_OFFSET_START;
            extInfo.miscParameter1 = 0;
            if (extInfo.miscParameter2 == 0)
            {
                extInfo.miscParameter2 = 100;
            }
        }

        /* Static LPF from IT ifc/ifr. Filter-env-only instruments still need
         * a base cutoff so LPFR modulation has something to work against. */
        if (playable->hasLpf)
        {
            extInfo.LPF_frequency = playable->lpfFrequency;
            extInfo.LPF_resonance = playable->lpfResonance;
            extInfo.LPF_lowpassAmount = playable->lpfAmount;
        }
        else if (playable->hasFilterEnv)
        {
            extInfo.LPF_frequency = 127 * 256;
            extInfo.LPF_resonance = 0;
            extInfo.LPF_lowpassAmount = 255;
        }

        /* INST LFOs: prefer filter env, pitch env, then vibrato (max 6). */
        extInfo.lfoCount = 0;

        if (playable->hasFilterEnv &&
            playable->filterEnvStageCount > 0 &&
            extInfo.lfoCount < BAE_EDITOR_MAX_LFOS)
        {
            BAERmfEditorLFOInfo *lfo = &extInfo.lfos[extInfo.lfoCount++];
            int32_t baseFreq = extInfo.LPF_frequency > 0 ? extInfo.LPF_frequency : (127 * 256);
            int32_t peak = playable->filterEnvPeakAbsY > 0 ? playable->filterEnvPeakAbsY : 256;

            memset(lfo, 0, sizeof(*lfo));
            lfo->destination = (int32_t)MOD2RMF_FCC('L', 'P', 'F', 'R');
            lfo->period = 0;
            lfo->waveShape = (int32_t)MOD2RMF_FCC('S', 'I', 'N', 'E');
            lfo->level = 0;
            /* Negative DC_feed: high ADSR brightens (matches SF2 mod-env→filter). */
            lfo->DC_feed = -((int32_t)(((int64_t)baseFreq * (int64_t)peak) / 256));
            if (lfo->DC_feed > -1)
            {
                lfo->DC_feed = -baseFreq;
            }
            mod2rmf_copy_adsr_to_editor(&lfo->adsr,
                                        playable->filterEnvStages,
                                        playable->filterEnvStageCount);
        }

        if (playable->hasPitchEnv &&
            playable->pitchEnvStageCount > 0 &&
            extInfo.lfoCount < BAE_EDITOR_MAX_LFOS)
        {
            BAERmfEditorLFOInfo *lfo = &extInfo.lfos[extInfo.lfoCount++];
            int32_t peak = playable->pitchEnvPeakAbsY > 0 ? playable->pitchEnvPeakAbsY : 1;

            memset(lfo, 0, sizeof(*lfo));
            lfo->destination = (int32_t)MOD2RMF_FCC('P', 'I', 'T', 'C');
            lfo->period = 0;
            lfo->waveShape = (int32_t)MOD2RMF_FCC('S', 'I', 'N', 'E');
            lfo->level = 0;
            /* libxmp fei Y ≈ cents after IT *50 scaling. */
            lfo->DC_feed = peak * MOD2RMF_PITCH_CENTS_TO_LFO;
            if (lfo->DC_feed < 1)
            {
                lfo->DC_feed = 1;
            }
            if (lfo->DC_feed > 524288)
            {
                lfo->DC_feed = 524288;
            }
            mod2rmf_copy_adsr_to_editor(&lfo->adsr,
                                        playable->pitchEnvStages,
                                        playable->pitchEnvStageCount);
        }

        if (playable->hasVibrato && extInfo.lfoCount < BAE_EDITOR_MAX_LFOS)
        {
            BAERmfEditorLFOInfo *lfo = &extInfo.lfos[extInfo.lfoCount++];

            memset(lfo, 0, sizeof(*lfo));
            lfo->destination = (int32_t)MOD2RMF_FCC('P', 'I', 'T', 'C');
            lfo->period = playable->vibPeriodUs;
            lfo->waveShape = playable->vibWaveShape
                                 ? playable->vibWaveShape
                                 : (int32_t)MOD2RMF_FCC('S', 'I', 'N', 'E');
            lfo->DC_feed = 0;
            lfo->level = playable->vibLevel;
            if (lfo->level < 1)
            {
                lfo->level = 1;
            }
            if (playable->vibSweepUs > 0)
            {
                lfo->adsr.stageCount = 2;
                lfo->adsr.stages[0].level = 0;
                lfo->adsr.stages[0].time = playable->vibSweepUs;
                lfo->adsr.stages[0].flags = (int32_t)MOD2RMF_FCC('L', 'I', 'N', 'E');
                lfo->adsr.stages[1].level = VOLUME_RANGE;
                lfo->adsr.stages[1].time = 0;
                lfo->adsr.stages[1].flags = (int32_t)MOD2RMF_FCC('S', 'U', 'S', 'T');
            }
            else
            {
                lfo->adsr.stageCount = 1;
                lfo->adsr.stages[0].level = VOLUME_RANGE;
                lfo->adsr.stages[0].time = 0;
                lfo->adsr.stages[0].flags = (int32_t)MOD2RMF_FCC('S', 'U', 'S', 'T');
            }
        }

        /* MOD/IT files don't use mod-wheel vibrato; suppress the engine's
         * automatic pitch LFO injection. */
        extInfo.hasDefaultMod = TRUE;

        (void)BAERmfEditorDocument_SetInstrumentExtInfo(conv->document, instID, &extInfo);
    }

    return 1;
}

int mod2rmf_setup_tracks(Mod2RmfConverter *conv, const ModSongModel *song, const ChannelMap *chMap)
{
    uint32_t i;
    uint32_t channelsToAdd;
    bool midiChInitialized[MOD2RMF_MAX_MIDI_CHANNELS];

    if (!conv || !song || !chMap)
    {
        return 0;
    }

    channelsToAdd = song->channelCount;
    conv->channelToTrackIndex = (uint16_t *)malloc(song->channelCount * sizeof(uint16_t));
    if (!conv->channelToTrackIndex)
    {
        return 0;
    }
    memset(conv->channelToTrackIndex, 0xFF, song->channelCount * sizeof(uint16_t));
    memset(midiChInitialized, 0, sizeof(midiChInitialized));

    for (i = 0; i < channelsToAdd; ++i)
    {
        BAERmfEditorTrackSetup setup;
        BAEResult result;
        uint16_t trackIndex;
        char trackName[64];

        memset(&setup, 0, sizeof(setup));
        setup.channel = chMap->trackerToMidi[i];
        /* Skip unused/unmapped tracker channels — empty tracks that share a
         * MIDI channel emit CC7=0 and silence real notes on that channel. */
        if (setup.channel >= MOD2RMF_MAX_MIDI_CHANNELS)
        {
            continue;
        }
        setup.bank = MOD2RMF_EMBEDDED_BANK;
        setup.program = 0;
        snprintf(trackName, sizeof(trackName), "Ch %u", i + 1);
        setup.name = trackName;

        result = BAERmfEditorDocument_AddTrack(conv->document, &setup, &trackIndex);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "[mod2rmf] Warning: failed to add track for channel %u (%d)\n", i, (int)result);
            continue;
        }

        conv->channelToTrackIndex[i] = trackIndex;
        BAERmfEditorDocument_SetTrackDefaultInstrument(conv->document,
                                   trackIndex,
                                   MOD2RMF_EMBEDDED_BANK,
                                   0);

        /* Per-MIDI-channel initialization (pitch bend range, ch10 melodic mode)
         * - only emit once per MIDI channel even if multiple tracks share it. */
        if (!midiChInitialized[setup.channel])
        {
            midiChInitialized[setup.channel] = TRUE;

            /* Set pitch bend range to ±N semitones via RPN */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 101, 0, 0); /* RPN MSB */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 100, 0, 0); /* RPN LSB */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,   6, 0,
                                                 song->pitchBendRangeSemitones);         /* Data Entry */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,  38, 0, 0); /* Data LSB */
            /* Null RPN selection so subsequent Data Entry does not accidentally
             * modify bend range on synths with sticky parameter selection. */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 101, 0, 127); /* RPN Null MSB */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 100, 0, 127); /* RPN Null LSB */

            /* MIDI channel 10 (zero-based 9) defaults to percussion in many synths.
             * NRPN 5,0 with data 3 switches it to melodic playback. */
            if (setup.channel == 9u)
            {
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 99, 0, 5); /* NRPN MSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 98, 0, 0); /* NRPN LSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,  6, 0, 3); /* Data Entry MSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 38, 0, 0); /* Data Entry LSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 99, 0, 127); /* NRPN Null MSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 98, 0, 127); /* NRPN Null LSB */

                /* Re-assert bend range after ch10 melodic-mode setup. Some devices
                 * ignore NRPN select and may interpret CC6=3 as bend-range data. */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 101, 0, 0); /* RPN MSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 100, 0, 0); /* RPN LSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,   6, 0,
                                                     song->pitchBendRangeSemitones);         /* Data Entry */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,  38, 0, 0); /* Data LSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 101, 0, 127); /* RPN Null MSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 100, 0, 127); /* RPN Null LSB */
#if _DEBUG == TRUE
                fprintf(stderr,
                        "[mod2rmf][dbg][ch10] init melodic mode on track=%u at tick=0 (NRPN 5,0 -> 3)\n",
                        (unsigned)trackIndex);
#endif
            }
        }
    }

    return 1;
}

/* Build aggregate profile for a MIDI channel (union of all tracker channels
 * already assigned to it). Only considers active ranges for overlap testing. */
void mod2rmf_build_midi_channel_aggregate(const ChannelProfile trackerProfiles[],
                                         const uint8_t trackerToMidi[],
                                         uint32_t trackerCount,
                                         uint8_t midiCh,
                                         ChannelProfile *agg)
{
    uint32_t i, j;
    memset(agg, 0, sizeof(*agg));
    for (i = 0; i < trackerCount; ++i)
    {
        if (trackerToMidi[i] != midiCh) continue;
        if (!trackerProfiles[i].used) continue;

        for (j = 0; j < trackerProfiles[i].rangeCount; ++j)
        {
            mod2rmf_channel_profile_add_range(agg, trackerProfiles[i].activeRanges[j].startTick,
                                           trackerProfiles[i].activeRanges[j].endTick);
        }
        agg->noteCount += trackerProfiles[i].noteCount;
        agg->used = TRUE;
    }
}

void mod2rmf_load_options_defaults(Mod2RmfLoadOptions *opts)
{
    if (!opts)
    {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->useZmfContainer = TRUE;
    opts->useExtendedPitchRange = FALSE;
    opts->useExtendedAdsr = FALSE;
    mod2rmf_resampler_defaults(&opts->resamplerSettings);
    opts->stereoSeparation = 75;
}

BAEResult mod2rmf_load_module_to_document(BAERmfEditorDocument **doc, const char *sourcePath, bool useZmfContainer)
{
    Mod2RmfLoadOptions opts;
    mod2rmf_load_options_defaults(&opts);
    opts.useZmfContainer = useZmfContainer ? TRUE : FALSE;
    return mod2rmf_load_module_to_document_ex(doc, sourcePath, &opts);
}

BAEResult mod2rmf_load_module_to_document_ex(BAERmfEditorDocument **doc,
                                             const char *sourcePath,
                                             const Mod2RmfLoadOptions *opts)
{
    Mod2RmfLoadOptions localOpts;
    Mod2RmfConverter *conv;
    ModSongModel song;
    bool useZmfContainer;

    if (!doc || !sourcePath || !sourcePath[0])
    {
        return BAE_PARAM_ERR;
    }
    *doc = NULL;

    if (opts)
    {
        localOpts = *opts;
    }
    else
    {
        mod2rmf_load_options_defaults(&localOpts);
    }
    if (localOpts.stereoSeparation > 100u)
    {
        localOpts.stereoSeparation = 100u;
    }

    conv = mod2rmf_converter_create();
    if (!conv)
    {
        return BAE_MEMORY_ERR;
    }
    mod2rmf_song_model_init(&song);

    conv->resamplerSettings = localOpts.resamplerSettings;
    conv->stereoSeparation = localOpts.stereoSeparation;
    conv->useExtendedPitchRange = localOpts.useExtendedPitchRange ? TRUE : FALSE;
    conv->maxAdsrStages = localOpts.useExtendedAdsr
                              ? (uint8_t)BAE_EDITOR_MAX_ADSR_STAGES
                              : (uint8_t)BAE_RMF_MAX_ADSR_STAGES;
    /* Extended ADSR / pitch features require ZMF. */
    useZmfContainer = localOpts.useZmfContainer ||
                      localOpts.useExtendedAdsr ||
                      localOpts.useExtendedPitchRange;
    conv->isIt = mod2rmf_path_is_it(sourcePath);

    if (!mod2rmf_load_source_data(conv, sourcePath))
    {
        BAE_STDERR("Error: failed to read source file\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_FILE_IO_ERROR;
    }

    {
        struct xmp_test_info testInfo;
        memset(&testInfo, 0, sizeof(testInfo));
        if (xmp_test_module_from_memory(conv->sourceData, (long)conv->sourceSize, &testInfo) != 0)
        {
            BAE_STDERR("Error: unsupported or invalid tracker module\n");
            mod2rmf_song_model_dispose(&song);
            mod2rmf_converter_delete(conv);
            return BAE_UNSUPPORTED_FORMAT;
        }
        BAE_PRINTF("Module detected by libxmp: %s (%s)\n",
                testInfo.name[0] ? testInfo.name : "(untitled)",
                testInfo.type[0] ? testInfo.type : "unknown");
    }

    if (!mod2rmf_build_song_model(conv, &song))
    {
        BAE_STDERR("Error: failed to build song model\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_BAD_FILE_TYPE;
    }

    if (!mod2rmf_ensure_loop_cc_resets(&song))
    {
        fprintf(stderr, "Error: loop CC reset failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_MEMORY_ERR;
    }

    /* Same for pitch bend - engine keeps bend state across loop-back. */
    if (!mod2rmf_ensure_loop_pitch_bend_resets(&song))
    {
        fprintf(stderr, "Error: loop pitch bend reset failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_MEMORY_ERR;
    }

    /* Same for program - engine keeps channelProgram across loop-back. */
    if (!mod2rmf_ensure_loop_program_resets(&song))
    {
        fprintf(stderr, "Error: loop program reset failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_MEMORY_ERR;
    }

    if (!mod2rmf_setup_document(conv, &song, sourcePath))
    {
        fprintf(stderr, "Error: document setup failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_GENERAL_ERR;
    }

    /* Emit MIDI loop markers if the song has an infinite loop */
    if (song.loopEnabled)
    {
        BAERmfEditorDocument_SetMidiLoopMarkers(conv->document,
                                                TRUE,
                                                song.loopStartTick,
                                                song.loopEndTick,
                                                -1); /* -1 = loop forever */
        fprintf(stderr, "Loop detected: start=%u end=%u ticks\n",
                (unsigned)song.loopStartTick, (unsigned)song.loopEndTick);
    }

    if (!mod2rmf_setup_samples(conv, &song))
    {
        fprintf(stderr, "Error: sample setup failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_GENERAL_ERR;
    }

    /* Analyze channel usage and compute tracker→MIDI channel mapping */
    {
        ChannelProfile profiles[MOD2RMF_MAX_CHANNELS];

        mod2rmf_analyze_channel_usage(&song, profiles, song.channelCount);
        mod2rmf_compute_channel_map(profiles,
                        song.channelCount,
                        &conv->channelMap,
                        conv->avoidMidiChannel10);

        #if _DEBUG == TRUE
        {
            uint32_t ci;
            for (ci = 0; ci < song.channelCount; ++ci)
            {
                if (profiles[ci].used)
                {
                    fprintf(stderr, "[mod2rmf] Channel map: tracker ch %u -> MIDI ch %u (%u notes, %u ranges)\n",
                            ci, conv->channelMap.trackerToMidi[ci], profiles[ci].noteCount, profiles[ci].rangeCount);
                }
            }
        }
        #endif

        mod2rmf_channel_profile_cleanup(profiles, song.channelCount);
    }

    if (conv->useExtendedPitchRange && conv->document)
    {
        int32_t engineConfig = 0;
        (void)BAERmfEditorDocument_GetEngineConfig(conv->document, &engineConfig);
        engineConfig |= (int32_t)(SONG_CONFIG_HAS_EXTENDED_PITCH_RANGE | SONG_CONFIG_EXTENDED_PITCH_RANGE_ON);
        BAERmfEditorDocument_SetEngineConfig(conv->document, engineConfig);
    }

    if (!mod2rmf_setup_tracks(conv, &song, &conv->channelMap) ||
        !mod2rmf_setup_instrument_ext(conv, &song, useZmfContainer) ||
        !mod2rmf_write_song_cc_events(conv, &song) ||
        !mod2rmf_write_song_pitch_bend_events(conv, &song) ||
        !mod2rmf_write_loop_program_resets(conv, &song) ||
        !mod2rmf_write_song_notes(conv, &song, useZmfContainer) ||
        !mod2rmf_write_song_tempo_events(conv, &song))
    {
        BAE_STDERR("Error: conversion failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_MEMORY_ERR;
    }

    *doc = conv->document; /* Return the created document via output parameter */

    conv->document = NULL;
    mod2rmf_song_model_dispose(&song);
    mod2rmf_converter_delete(conv);
    return BAE_NO_ERROR;
}