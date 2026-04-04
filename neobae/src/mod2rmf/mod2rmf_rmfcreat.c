#include "mod2rmf_rmfcreat.h"
#include "mod2rmf_common.h"
#include "mod2rmf_song.h"
#include "X_Formats.h"
#include "X_Assert.h"
#include <xmp.h>

/* Resolve per-note transpose from the active instrument mapping.
 * Some formats (notably IT) can reuse the same sample across multiple
 * instruments with different transpose settings, so a global per-sample
 * transpose cache can detune only certain notes. */
static int mod2rmf_resolve_note_transpose(const struct xmp_module *mod,
                                          const struct xmp_channel_info *ci,
                                          int sid,
                                          const int16_t sampleTranspose[MOD2RMF_MAX_SAMPLES])
{
    int noteXpo = 0;
    int instIndex = -1;

    if (ci && ci->instrument > 0)
    {
        instIndex = (int)ci->instrument - 1;
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

    return noteXpo;
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
        
        /* BAE limits downward pitch shifts to -24 semitones from rootKey.
         * To allow tracker modules to play extremely low notes (e.g. laugh samples),
         * we virtually shift the rootKey down by 2 octaves (24 semitones) and 
         * correspondingly divide the sample rate by 4 prior to saving. */
        setup.rootKey = (playable->rootKey >= 24) ? (playable->rootKey - 24) : 0;
        
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
                bool needsProcessing;

                chosenLoopStart = loopStart;
                chosenLoopEnd = loopEnd;
                needsProcessing = conv->forceOriginalSamples ? FALSE : TRUE;

                 if (!needsProcessing)
                 {
                     BAE_UNSIGNED_FIXED sampledRate;
                     double baseRate = playable->hasSampleRateOverride ? 
                                         (double)playable->sampleRateOverrideHz : 
                                         (double)conv->moduleBaseRateHz;
                     /* Divide the physical baseRate by 4 to complement the -24 rootKey shift */
                     sampledRate = (BAE_UNSIGNED_FIXED)((double)(baseRate / 4.0) * 65536.0 + 0.5);
                     if (sampledRate < 65536u)
                     {
                         sampledRate = 65536u;
                     }

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

                    if (playable->hasSampleRateOverride)
                    {
                        sampledRate = (BAE_UNSIGNED_FIXED)((outRate / 4u) << 16);
                    }
                    else
                    {
                        sampledRate = (BAE_UNSIGNED_FIXED)((double)(outRate / 4.0) * 65536.0 + 0.5);
                        if (sampledRate < 65536u)
                        {
                            sampledRate = 65536u;
                        }
                    }

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
    uint8_t chLastVol[MOD2RMF_MAX_CHANNELS];
    uint8_t chLastPan[MOD2RMF_MAX_CHANNELS];
    uint16_t chLastBend[MOD2RMF_MAX_CHANNELS];
    int16_t sampleTranspose[MOD2RMF_MAX_SAMPLES];  /* sub-instrument xpo per sample */
    bool sampleHasEnvelope[MOD2RMF_MAX_SAMPLES];   /* instrument has amplitude envelope */
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
        raw->rootKey = 72;
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
            raw->pcm8 = (int8_t *)malloc(outFrames);
            if (!raw->pcm8)
            {
                xmp_release_module(ctx);
                xmp_free_context(ctx);
                return 0;
            }
            src16 = (const int16_t *)s->data;
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
    }

    /* Extract amplitude envelope ADSR for each sample from the first
     * instrument that references it (same "first wins" policy as
     * sampleTranspose).  This runs AFTER the sample extraction loop
     * above so the memset() on each raw sample doesn't clobber the
     * envelope data. */
    if (mod->ins > 0 && mod->xxi)
    {
        for (i = 0; i < (uint32_t)mod->ins; ++i)
        {
            struct xmp_instrument *inst;
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
                if (!sampleHasEnvelope[sid] &&
                    (inst->aei.flg & XMP_ENVELOPE_ON) && inst->aei.npt >= 2)
                {
                    mod2rmf_extract_envelope_adsr(inst, (uint32_t)(mod->bpm > 0 ? mod->bpm : 125),
                                          &conv->rawSamples[sid]);
                    sampleHasEnvelope[sid] = conv->rawSamples[sid].hasEnvelope;
                }
                /* Capture default pan from sub-instrument (first assignment wins) */
                if (conv->rawSamples[sid].defaultPan < 0)
                {
                    conv->rawSamples[sid].defaultPan = (int16_t)inst->sub[sub].pan;
                }
            }

            /* Now that we have captured the ADSR parameters, disable the
             * amplitude envelope so libxmp no longer folds it into
             * ci->volume.  This lets us emit CC7 from the raw channel
             * volume (including volume slides) while BAE handles the
             * envelope shape through its own ADSR engine. */
            if (inst->aei.flg & XMP_ENVELOPE_ON)
            {
                inst->aei.flg &= ~XMP_ENVELOPE_ON;
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

                /* Use row-start volume only; per-frame ci->volume is post-processed
                 * (envelope/fade/runtime), not the raw channel volume intent. */
                if (fi.frame == 0)
                {
                    uint8_t vol64;
                    vol64 = hasRowVolumeCmd
                              ? rowVolumeCmd64
                              : (uint8_t)mod2rmf_clamp_int((int)ci->volume, 0, 64);
                    if (vol64 != chLastVol[ch])
                    {
                        chLastVol[ch] = vol64;
                        (void)mod2rmf_song_model_append_cc_event(song, (uint16_t)ch, tick, 7,
                                                        mod2rmf_vol_to_midi(vol64),
                                                        activeNotes[ch].program);
                    }
                }
                if (activeNotes[ch].active)
                {
                    uint16_t bend;
                    bend = mod2rmf_pitchbend_to_midi(ci->pitchbend + activeNotes[ch].bendOffsetCents,
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

                    /* Reset per-row effect state. */
                    memset(&chEffects[ch], 0, sizeof(chEffects[ch]));

                    /* Parse retrigger and note-delay effects from both effect columns. */
                    mod2rmf_parse_row_effects(rowEvent, &retrigInterval, &noteDelayFrames);
                    chEffects[ch].retrigInterval = retrigInterval;
                    chEffects[ch].noteDelayFrames = noteDelayFrames;

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
                                chEffects[ch].delayedVolume = hasRowVolumeCmd
                                                              ? rowVolumeCmd64
                                                              : (uint8_t)mod2rmf_clamp_int((int)ci->volume, 0, 64);
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
                                                                         sampleTranspose);
                                midiNote = baseNote + 12 + noteXpo;

                                activeNotes[ch].active = TRUE;
                                activeNotes[ch].startTickFP = currentTickFP;
                                activeNotes[ch].note = (unsigned char)mod2rmf_clamp_int(midiNote, 0, 127);
                                activeNotes[ch].velocity = mod2rmf_note_velocity_from_volume(hasRowVolumeCmd
                                                                                               ? rowVolumeCmd64
                                                                                               : (uint8_t)mod2rmf_clamp_int((int)ci->volume, 0, 64));
                                activeNotes[ch].program = (uint8_t)program;
                                activeNotes[ch].bendOffsetCents = (midiNote - (int)activeNotes[ch].note) * 100;
                                chLastBend[ch] = 0xFFFFu;
                                {
                                    uint16_t bend;
                                    bend = mod2rmf_pitchbend_to_midi(ci->pitchbend + activeNotes[ch].bendOffsetCents,
                                                                    song->pitchBendRangeSemitones);
                                    chLastBend[ch] = bend;
                                    (void)mod2rmf_song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                                        activeNotes[ch].program);
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
                                                                     sampleTranspose);
                            midiNote = baseNote + 12 + noteXpo;

                            activeNotes[ch].active = TRUE;
                            activeNotes[ch].startTickFP = currentTickFP;
                            activeNotes[ch].note = (unsigned char)mod2rmf_clamp_int(midiNote, 0, 127);
                            activeNotes[ch].velocity = mod2rmf_note_velocity_from_volume(chEffects[ch].delayedVolume);
                            activeNotes[ch].program = (uint8_t)program;
                            activeNotes[ch].bendOffsetCents = (midiNote - (int)activeNotes[ch].note) * 100;
                            chLastBend[ch] = 0xFFFFu;
                            {
                                uint16_t bend;
                                bend = mod2rmf_pitchbend_to_midi(ci->pitchbend + activeNotes[ch].bendOffsetCents,
                                                                song->pitchBendRangeSemitones);
                                chLastBend[ch] = bend;
                                (void)mod2rmf_song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                                    activeNotes[ch].program);
                            }
                        }
                        chEffects[ch].hasDelayedNote = FALSE;
                    }

                    /* Retrigger: fire a new note-on at each interval boundary. */
                    if (chEffects[ch].retrigInterval > 0 &&
                        ((uint8_t)fi.frame % chEffects[ch].retrigInterval) == 0 &&
                        activeNotes[ch].active)
                    {
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
                        /* note and program stay the same from the row's note-on */
                        activeNotes[ch].velocity = mod2rmf_note_velocity_from_volume((uint8_t)mod2rmf_clamp_int((int)ci->volume, 0, 64));
                        chLastBend[ch] = 0xFFFFu;
                        {
                            uint16_t bend;
                            bend = mod2rmf_pitchbend_to_midi(ci->pitchbend + activeNotes[ch].bendOffsetCents,
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
        p->rootKey = 72;
        p->hasSampleRateOverride = TRUE;
        p->sampleRateOverrideHz = conv->moduleBaseRateHz;
        
        p->sampleOffsetBytes = 0;
        p->offsetVariant = FALSE;
        p->rawSample = &conv->rawSamples[i];

        /* If the instrument has an amplitude envelope, map it to a BAE
         * ADSR so the engine produces smooth volume shaping instead of
         * relying on per-frame CC7 events.  The per-frame CC7 tracking
         * is correspondingly suppressed for enveloped channels. */
        if (conv->rawSamples[i].hasEnvelope && conv->rawSamples[i].adsrStageCount > 0)
        {
            p->hasVolumeAdsr = TRUE;
            p->adsrStageCount = conv->rawSamples[i].adsrStageCount;
            memcpy(p->adsrStages, conv->rawSamples[i].adsrStages,
                   conv->rawSamples[i].adsrStageCount * sizeof(ModAdsrStage));
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
        conv->itV00CutRows = 6;
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
                                note->program))
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
    /* For deduplication: track the primary RMF track per MIDI channel,
     * and the last emitted value per (midiCh, cc#) to avoid redundant events. */
    uint16_t midiChPrimaryTrack[MOD2RMF_MAX_MIDI_CHANNELS];
    /* Last emitted CC values: [midiCh][cc] — 0xFFFF = not yet emitted */
    uint16_t lastCC[MOD2RMF_MAX_MIDI_CHANNELS][128];

    if (!conv || !song)
    {
        return 0;
    }

    bool ccDedupReset = FALSE;
    memset(lastCC, 0xFF, sizeof(lastCC));

    /* Find primary (first) RMF track for each MIDI channel */
    memset(midiChPrimaryTrack, 0xFF, sizeof(midiChPrimaryTrack));
    for (i = 0; i < song->channelCount; ++i)
    {
        uint8_t midiCh = conv->channelMap.trackerToMidi[i];
        if (midiCh < MOD2RMF_MAX_MIDI_CHANNELS &&
            midiChPrimaryTrack[midiCh] == (uint16_t)0xFFFF &&
            conv->channelToTrackIndex[i] != (uint16_t)0xFFFF)
        {
            midiChPrimaryTrack[midiCh] = conv->channelToTrackIndex[i];
        }
    }

    for (i = 0; i < song->ccCount; ++i)
    {
        const ModCCEvent *ev;
        uint8_t midiCh;
        uint16_t trackIndex;

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

        midiCh = conv->channelMap.trackerToMidi[ev->sourceChannel];
        if (midiCh >= MOD2RMF_MAX_MIDI_CHANNELS) continue;

        /* Route through the primary track for this MIDI channel */
        trackIndex = midiChPrimaryTrack[midiCh];
        if (trackIndex == (uint16_t)0xFFFF) continue;

        /* Skip if this CC value was already emitted for this MIDI channel */
        if (lastCC[midiCh][ev->cc] == (uint16_t)ev->value)
        {
            continue;
        }
        lastCC[midiCh][ev->cc] = (uint16_t)ev->value;

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
    /* Deduplication: primary track and last value per MIDI channel */
    uint16_t midiChPrimaryTrack[MOD2RMF_MAX_MIDI_CHANNELS];
    uint16_t lastBend[MOD2RMF_MAX_MIDI_CHANNELS];

    if (!conv || !song)
    {
        return 0;
    }

    /* 0xFFFF = not yet emitted */
    bool bendDedupReset = FALSE;
    memset(lastBend, 0xFF, sizeof(lastBend));

    /* Find primary (first) RMF track for each MIDI channel */
    memset(midiChPrimaryTrack, 0xFF, sizeof(midiChPrimaryTrack));
    for (i = 0; i < song->channelCount; ++i)
    {
        uint8_t midiCh = conv->channelMap.trackerToMidi[i];
        if (midiCh < MOD2RMF_MAX_MIDI_CHANNELS &&
            midiChPrimaryTrack[midiCh] == (uint16_t)0xFFFF &&
            conv->channelToTrackIndex[i] != (uint16_t)0xFFFF)
        {
            midiChPrimaryTrack[midiCh] = conv->channelToTrackIndex[i];
        }
    }

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

        /* Route through the primary track for this MIDI channel */
        trackIndex = midiChPrimaryTrack[midiCh];
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
                    superseded = TRUE;
                    break;
                }
            }
        }
        
        if (superseded) {
            continue;
        }

        /* Skip if bend value hasn't changed for this MIDI channel */
        if (lastBend[midiCh] == ev->value) {
            continue;
        }
        lastBend[midiCh] = ev->value;

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

int mod2rmf_write_song_notes(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;

    if (!conv || !song)
    {
        return 0;
    }

    for (i = 0; i < song->noteCount; ++i)
    {
        const ModNoteEvent *note;
        uint16_t trackIndex;

        note = &song->notes[i];
        trackIndex = conv->channelToTrackIndex[note->sourceChannel];
        if (trackIndex == (uint16_t)0xFFFF)
        {
            continue;
        }

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

        playable = &song->playables[i];
        if (!playable->rawSample || !playable->rawSample->valid ||
            !playable->rawSample->pcm8 || playable->rawSample->frameCount == 0)
        {
            continue;
        }
        instID = 512u + (uint32_t)playable->program;

        memset(&extInfo, 0, sizeof(extInfo));
        result = BAERmfEditorDocument_GetInstrumentExtInfo(conv->document, instID, &extInfo);
        if (result != BAE_NO_ERROR)
        {
            /* Continue with safe defaults if the get call fails. */
            memset(&extInfo, 0, sizeof(extInfo));
            extInfo.instID = instID;
            extInfo.flags1 = MOD2RMF_ZBF_USE_SAMPLE_RATE;
            extInfo.flags2 = 0;
            extInfo.midiRootKey = 60;
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
        /* Always force root key to 60 (C-4).  GetInstrumentExtInfo may
         * return a different default depending on the sample rate the
         * engine inferred during ReplaceSampleFromPCM; that mismatch
         * causes some S3M samples to play at the wrong pitch. */
        extInfo.midiRootKey = 60;

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

        /* MOD files don't use mod-wheel vibrato; suppress the engine's
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
         * — only emit once per MIDI channel even if multiple tracks share it. */
        if (!midiChInitialized[setup.channel])
        {
            midiChInitialized[setup.channel] = TRUE;

            /* Set pitch bend range to ±N semitones via RPN */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 101, 0, 0); /* RPN MSB */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 100, 0, 0); /* RPN LSB */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,   6, 0,
                                                 song->pitchBendRangeSemitones);         /* Data Entry */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,  38, 0, 0); /* Data LSB */

            /* MIDI channel 10 (zero-based 9) defaults to percussion in many synths.
             * NRPN 5,0 with data 3 switches it to melodic playback. */
            if (setup.channel == 9u)
            {
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 99, 0, 5); /* NRPN MSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 98, 0, 0); /* NRPN LSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,  6, 0, 3); /* Data Entry MSB */
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

BAEResult mod2rmf_load_module_to_document(BAERmfEditorDocument **doc, const char *sourcePath, bool useZmfContainer) {
    Mod2RmfResamplerSettings resamplerSettings;
    Mod2RmfConverter *conv = mod2rmf_converter_create();
    ModSongModel song;
    mod2rmf_song_model_init(&song);    
    mod2rmf_resampler_defaults(&resamplerSettings);

    if (!mod2rmf_load_source_data(conv, sourcePath))
    {
        BAE_STDERR("Error: failed to read source file\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_FILE_IO_ERROR; 
    }

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

    if (!mod2rmf_build_song_model(conv, &song))
    {
        BAE_STDERR("Error: failed to build song model\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_INVALID_TYPE;
    }

    if (!mod2rmf_ensure_loop_cc_resets(&song))
    {
        fprintf(stderr, "Error: loop CC reset failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_MEMORY_ERR;
    }

    /* Same for pitch bend — engine keeps bend state across loop-back. */
    if (!mod2rmf_ensure_loop_pitch_bend_resets(&song))
    {
        fprintf(stderr, "Error: loop pitch bend reset failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return BAE_MEMORY_ERR;
    }

    if (!mod2rmf_setup_document(conv, &song, sourcePath))
    {
        fprintf(stderr, "Error: document setup failed\n");
        mod2rmf_song_model_dispose(&song);
        mod2rmf_converter_delete(conv);
        return 1;
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
        return 1;
    }

    /* Analyze channel usage and compute tracker→MIDI channel mapping */
    {
        ChannelProfile profiles[MOD2RMF_MAX_CHANNELS];

        mod2rmf_analyze_channel_usage(&song, profiles, song.channelCount);
        mod2rmf_compute_channel_map(profiles, song.channelCount, &conv->channelMap);

        #ifdef _DEBUG
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

    if (!mod2rmf_setup_tracks(conv, &song, &conv->channelMap) ||
        !mod2rmf_setup_instrument_ext(conv, &song, useZmfContainer) ||
        !mod2rmf_write_song_cc_events(conv, &song) ||
        !mod2rmf_write_song_pitch_bend_events(conv, &song) ||
        !mod2rmf_write_song_notes(conv, &song) ||
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