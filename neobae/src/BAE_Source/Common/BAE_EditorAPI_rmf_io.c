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
/*****************************************************************************/
/*
**  BAE_EditorAPI_rmf_io.c
**
**  RMF load and sample/INST/song resource writers. Perf: neobae/docs/BAE_EditorAPI_PERF.md
*/
/*****************************************************************************/

#include "BAE_EditorAPI_Internal.h"


BAEResult PV_CaptureOriginalResourcesFromFile(BAERmfEditorDocument *document, XFILE fileRef)
{
    XFILERESOURCEMAP map;
    int32_t nextOffset;
    int32_t resourceCount;
    int32_t resourceIndex;

    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }
    PV_FreeOriginalResources(document);

    if (XFileSetPosition(fileRef, 0L) != 0 ||
        XFileRead(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0 ||
        !XFILERESOURCE_ID_IS_VALID(XGetLong(&map.mapID)) ||
        !XFILERESOURCE_VERSION_IS_VALID(XGetLong(&map.version)))
    {
        return BAE_BAD_FILE;
    }

    nextOffset = (int32_t)sizeof(XFILERESOURCEMAP);
    resourceCount = (int32_t)XGetLong(&map.totalResources);
    for (resourceIndex = 0; resourceIndex < resourceCount; ++resourceIndex)
    {
        int32_t nextHeader;
        int32_t rawType;
        int32_t rawID;
        XResourceType type;
        XLongResourceID id;
        char cName[256];
        BAERmfEditorResourceEntry *entry;
        XPTR resourceData;
        int32_t resourceSize;
        BAEResult growResult;

        if (XFileSetPosition(fileRef, nextOffset) != 0 ||
            XFileRead(fileRef, &nextHeader, (int32_t)sizeof(int32_t)) != 0 ||
            XFileRead(fileRef, &rawType, (int32_t)sizeof(int32_t)) != 0 ||
            XFileRead(fileRef, &rawID, (int32_t)sizeof(int32_t)) != 0)
        {
            PV_FreeOriginalResources(document);
            return BAE_FILE_IO_ERROR;
        }
        nextHeader = (int32_t)XGetLong(&nextHeader);
        type = (XResourceType)XGetLong(&rawType);
        id = (XLongResourceID)XGetLong(&rawID);

        resourceData = XGetFileResource(fileRef, type, id, NULL, &resourceSize);
        if (!resourceData)
        {
            PV_FreeOriginalResources(document);
            return BAE_BAD_FILE;
        }

        growResult = PV_GrowBuffer((void **)&document->originalResources,
                                   &document->originalResourceCapacity,
                                   sizeof(BAERmfEditorResourceEntry),
                                   document->originalResourceCount + 1);
        if (growResult != BAE_NO_ERROR)
        {
            XDisposePtr(resourceData);
            PV_FreeOriginalResources(document);
            return growResult;
        }

        entry = &document->originalResources[document->originalResourceCount];
        XSetMemory(entry, sizeof(*entry), 0);
        entry->type = type;
        entry->id = id;
        entry->data = resourceData;
        entry->size = resourceSize;
        cName[0] = 0;
        if (XGetFileResourceName(fileRef, type, id, cName) != FALSE)
        {
            if (PV_CreatePascalName(cName, (char *)entry->pascalName) != BAE_NO_ERROR)
            {
                entry->pascalName[0] = 0;
            }
        }
        document->originalResourceCount++;

        if (resourceIndex < (resourceCount - 1))
        {
            if (nextHeader <= nextOffset)
            {
                PV_FreeOriginalResources(document);
                return BAE_BAD_FILE;
            }
            nextOffset = nextHeader;
        }
    }
    return BAE_NO_ERROR;
}


BAEResult PV_PopulateSongResourceInfoFromDocument(BAERmfEditorDocument const *document,
                                                         SongResource_Info *songInfo,
                                                         XLongResourceID midiResourceID)
{
    uint32_t infoIndex;
    BAEResult result;
    SongType writeSongType;

    if (!document || !songInfo)
    {
        return BAE_PARAM_ERR;
    }

    XClearSongResourceInfo(songInfo);

    writeSongType = document->songType;
    if (writeSongType == SONG_TYPE_BAD)
    {
        writeSongType = SONG_TYPE_RMF;
    }

    songInfo->songType = writeSongType;
    songInfo->objectResourceID = (XShortResourceID)midiResourceID;
    songInfo->maxMidiNotes = document->maxMidiNotes;
    songInfo->maxEffects = document->maxEffects;
    songInfo->mixLevel = document->mixLevel;
    songInfo->reverbType = (int16_t)document->reverbType;
    songInfo->songVolume = document->songVolume;
    songInfo->songTempo = document->songTempo;
    songInfo->songPitchShift = document->songPitchShift;
    songInfo->songLocked = document->songLocked;
    songInfo->songEmbedded = document->songEmbedded;
    songInfo->engineConfigFlags = document->engineConfigFlags;
    if (document->velocityCurveType != DEFAULT_VELOCITY_CURVE)
    {
        songInfo->engineConfigFlags &= ~(SONG_CONFIG_OVERRIDE_VOLUME_CURVE | SONG_CONFIG_VOLUME_CURVE_TYPE_MASK);
        songInfo->engineConfigFlags |= SONG_CONFIG_OVERRIDE_VOLUME_CURVE;
        songInfo->engineConfigFlags |= ((uint32_t)document->velocityCurveType << SONG_CONFIG_VOLUME_CURVE_TYPE_SHIFT) & SONG_CONFIG_VOLUME_CURVE_TYPE_MASK;
    }
    else
    {
        songInfo->engineConfigFlags &= ~(SONG_CONFIG_OVERRIDE_VOLUME_CURVE | SONG_CONFIG_VOLUME_CURVE_TYPE_MASK);
    }

    for (infoIndex = 0; infoIndex < INFO_TYPE_COUNT; ++infoIndex)
    {
        if (document->info[infoIndex])
        {
            result = PV_AssignSongInfoString(songInfo, (BAEInfoType)infoIndex, document->info[infoIndex]);
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
        }
    }

    return BAE_NO_ERROR;
}


BAEResult PV_GetEmbeddedSampleDisplayName(XFILE fileRef,
                                                 XShortResourceID sndID,
                                                 char outName[256])
{
    static XResourceType const kSampleTypes[] = { ID_CSND, ID_ESND, ID_SND };
    uint32_t typeIndex;
    char rawName[256];

    if (!fileRef || !outName)
    {
        return BAE_PARAM_ERR;
    }

    outName[0] = 0;
    for (typeIndex = 0; typeIndex < (uint32_t)(sizeof(kSampleTypes) / sizeof(kSampleTypes[0])); ++typeIndex)
    {
        rawName[0] = 0;
        if (XGetFileResourceName(fileRef,
                                 kSampleTypes[typeIndex],
                                 (XLongResourceID)sndID,
                                 rawName) != FALSE)
        {
            PV_DecodeResourceName(rawName, outName);
            if (outName[0])
            {
                return BAE_NO_ERROR;
            }
        }
    }

    return BAE_RESOURCE_NOT_FOUND;
}


BAEResult PV_AddEmbeddedSampleVariant(BAERmfEditorDocument *document,
                                             XFILE fileRef,
                                             XLongResourceID instID,
                                             char const *displayName,
                                             unsigned char program,
                                             XShortResourceID sndID,
                                             unsigned char rootKey,
                                             unsigned char lowKey,
                                             unsigned char highKey)
{
    XPTR sndData;
    int32_t sndSize;
    SampleDataInfo sdi;
    XPTR pcmData;
    XPTR pcmOwner;
    XPTR sndCopy;
    GM_Waveform *waveform;
    BAERmfEditorSample *sample;
    BAEResult growResult;

    /* Use the engine's canonical loader for SND/CSND/ESND handling. */
    XFileUseThisResourceFile(fileRef);
    sndData = XGetSoundResourceByID((XLongResourceID)sndID, &sndSize);
    if (!sndData)
    {
        return BAE_BAD_FILE;
    }
    /* Keep the normalized plain SND blob so save can re-wrap it as esnd/csnd/snd later
     * without re-encoding the codec payload inside the SND body. */
    sndCopy = XNewPtr(sndSize);
    if (sndCopy)
    {
        XBlockMove(sndData, sndCopy, sndSize);
    }

    XSetMemory(&sdi, sizeof(sdi), 0);
    pcmData = XGetSamplePtrFromSnd(sndData, &sdi);
    pcmOwner = NULL;
    if (sdi.pMasterPtr && sdi.pMasterPtr != sndData)
    {
        pcmOwner = sdi.pMasterPtr;
    }
    if (!pcmData)
    {
        XDisposePtr(sndData);
        return BAE_BAD_FILE;
    }

    waveform = (GM_Waveform *)XNewPtr((int32_t)sizeof(GM_Waveform));
    if (!waveform)
    {
        XDisposePtr(sndData);
        return BAE_MEMORY_ERR;
    }
    XSetMemory(waveform, sizeof(*waveform), 0);
    XTranslateFromSampleDataToWaveform(&sdi, waveform);
    {
        int32_t pcmSize;

        pcmSize = (int32_t)(sdi.frames * (sdi.bitSize / 8) * sdi.channels);
        if (pcmSize > 0)
        {
            XPTR ownedPcm = XNewPtr(pcmSize);
            if (!ownedPcm)
            {
                XDisposePtr((XPTR)waveform);
                if (pcmOwner)
                {
                    XDisposePtr(pcmOwner);
                }
                XDisposePtr(sndData);
                return BAE_MEMORY_ERR;
            }
            XBlockMove(pcmData, ownedPcm, pcmSize);
            waveform->theWaveform = (signed char *)ownedPcm;
        }
    }
    /* theWaveform now contains raw PCM copy; keep compression metadata aligned with data format */
    waveform->compressionType = C_NONE;
    /* When INST midiRootKey is 0 ("no override"), the engine relies on the SND's baseFrequency
     * for pitch calibration.  Recover the original baseFrequency from sdi.baseKey so that
     * sample->rootKey and the regenerated SND both carry the correct root note. */
    if (rootKey == 0)
    {
        if (lowKey <= 127 && highKey <= 127 && lowKey == highKey)
        {
            /* Single-key split with no explicit root: infer the split key first. */
            rootKey = lowKey;
        }
        else if (sdi.baseKey >= 0 && sdi.baseKey <= 127)
        {
            /* baseKey 0 is valid (C-1); only out-of-range falls back. */
            rootKey = (unsigned char)sdi.baseKey;
        }
        else
        {
            rootKey = 60; /* safe default: middle C */
        }
    }
    waveform->baseMidiPitch = rootKey;
    if (pcmOwner)
    {
        XDisposePtr(pcmOwner);
    }
    XDisposePtr(sndData);

    growResult = PV_GrowBuffer((void **)&document->samples,
                               &document->sampleCapacity,
                               sizeof(BAERmfEditorSample),
                               document->sampleCount + 1);
    if (growResult != BAE_NO_ERROR)
    {
        GM_FreeWaveform(waveform);
        return growResult;
    }
    sample = &document->samples[document->sampleCount];
    XSetMemory(sample, sizeof(*sample), 0);
    sample->waveform = waveform;
    sample->program = program;
    sample->instID = (uint32_t)instID;
    /* SND / asset id 0 is valid — do not remint. */
    sample->sampleAssetID = (uint32_t)sndID;
    PV_NoteSampleAssetID(document, sample->sampleAssetID);
    sample->rootKey = rootKey;
    sample->lowKey = lowKey;
    sample->highKey = highKey;
    sample->sourceCompressionType = sdi.compressionType;
    sample->sourceCompressionSubType = PV_GetStoredCompressionSubTypeFromSnd(sndCopy ? sndCopy : sndData,
                                                                              sndSize,
                                                                              sample->sourceCompressionType);
    sample->originalSndResourceType = ID_ESND;
    if (document)
    {
        BAERmfEditorResourceEntry const *originalSndEntry;

        originalSndEntry = PV_FindOriginalResourceByTypeAndID(document, ID_ESND, (XLongResourceID)sndID);
        if (!originalSndEntry)
        {
            originalSndEntry = PV_FindOriginalResourceByTypeAndID(document, ID_CSND, (XLongResourceID)sndID);
        }
        if (!originalSndEntry)
        {
            originalSndEntry = PV_FindOriginalResourceByTypeAndID(document, ID_SND, (XLongResourceID)sndID);
        }
        if (originalSndEntry)
        {
            sample->originalSndResourceType = originalSndEntry->type;
        }
    }
    sample->targetCompressionType = BAE_EDITOR_COMPRESSION_DONT_CHANGE;
    sample->targetOpusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
    if ((SndCompressionType)sdi.compressionType == C_OPUS && sndCopy != NULL)
    {
        sample->opusUseRoundTripResampling = XGetSoundOpusRoundTripFlag(sndCopy);
    }
#endif
    sample->originalSndData = sndCopy;
    sample->originalSndSize = sndCopy ? sndSize : 0;
    if (displayName && displayName[0])
    {
        sample->displayName = PV_DuplicateString(displayName);
    }
    else
    {
        char buf[32];
        sprintf(buf, "Sample P%u", (unsigned)program);
        sample->displayName = PV_DuplicateString(buf);
    }
    sample->sourcePath = NULL;
    sample->sampleInfo.bitSize = waveform->bitSize;
    sample->sampleInfo.channels = waveform->channels;
    sample->sampleInfo.baseMidiPitch = waveform->baseMidiPitch;
    sample->sampleInfo.waveSize = waveform->waveSize;
    sample->sampleInfo.waveFrames = waveform->waveFrames;
    sample->sampleInfo.startLoop = waveform->startLoop;
    sample->sampleInfo.endLoop = waveform->endLoop;
    sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;
    document->sampleCount++;
    return BAE_NO_ERROR;
}


BAERmfEditorResourceEntry const *PV_FindOriginalResourceByTypeAndID(BAERmfEditorDocument const *document,
                                                                            XResourceType type,
                                                                            XLongResourceID id)
{
    uint32_t i;

    if (!document)
    {
        return NULL;
    }
    for (i = 0; i < document->originalResourceCount; ++i)
    {
        BAERmfEditorResourceEntry const *entry;

        entry = &document->originalResources[i];
        if (entry->type == type && entry->id == id)
        {
            return entry;
        }
    }
    return NULL;
}


void PV_LoadEmbeddedSamplesFromRmf(BAERmfEditorDocument *document, XFILE fileRef)
{
    enum
    {
        kInstHeaderMinSize = 14,
        kInstKeySplitSize = 8
    };
    int32_t instIndex;

    for (instIndex = 0; ; ++instIndex)
    {
        XLongResourceID instID;
        XPTR instData;
        int32_t instSize;
        InstrumentResource *inst;
        int16_t splitCount;
        int16_t splitIndex;
        XShortResourceID baseSndID;
        int16_t baseRootKey;
        unsigned char program;
        char rawName[256];
        char instName[256];

        rawName[0] = 0;
        instData = XGetIndexedFileResource(fileRef, ID_INST, &instID, instIndex, rawName, &instSize);
        if (!instData)
        {
            break;
        }
        /* Some RMFs intentionally override Bank 0 with low INST IDs (<256).
         * Do not filter those out, or their embedded samples become invisible. */
        if (instSize < kInstHeaderMinSize)
        {
            XDisposePtr(instData);
            continue;
        }

        PV_DecodeResourceName(rawName, instName);
        inst = (InstrumentResource *)instData;
        baseSndID = (XShortResourceID)XGetShort(&inst->sndResourceID);
        baseRootKey = (int16_t)XGetShort(&inst->midiRootKey);
        splitCount = (int16_t)XGetShort(&inst->keySplitCount);
        if (splitCount < 0)
        {
            splitCount = 0;
        }
        if (instSize < (kInstHeaderMinSize + (splitCount * kInstKeySplitSize)))
        {
            XDisposePtr(instData);
            continue;
        }
        program = (unsigned char)(instID % 128);

        /* Detect whether this INST uses miscParameter1 as the per-sample root key
         * (useSoundModifierAsRootKey), or whether the root key comes from the SND's
         * own baseFrequency.  The two paths require different loading strategies:
         *   useSoundModifierAsRootKey=TRUE  → miscParameter1 (split or INST) IS the root key
         *   useSoundModifierAsRootKey=FALSE → SND's baseFrequency is the root key
         *     (pass rootKey=0 to PV_AddEmbeddedSampleVariant so it falls back to sdi.baseKey)
         * NOTE: midiRootKey 0 and 60 are both no-ops in the engine (shift by 0 semitones).
         * For non-trivial masterRootKey values the effective root would be
         * masterRootKey + baseMidiPitch - 60, but that edge case is uncommon in practice.
         */
        {
            bool useSoundModifierAsRootKey = TEST_FLAG_VALUE(inst->flags2, ZBF_useSoundModifierAsRootKey);
            int16_t instMiscParam1 = (int16_t)XGetShort(&inst->miscParameter1);

        if (splitCount > 0)
        {
            for (splitIndex = 0; splitIndex < splitCount; ++splitIndex)
            {
                KeySplit split;
                unsigned char splitRootForLoad;
                char sampleName[256];

                XGetKeySplitFromPtr(inst, splitIndex, &split);
                if (useSoundModifierAsRootKey)
                {
                    /* miscParameter1 is the authoritative per-split root key */
                    int16_t splitRoot = split.miscParameter1;
                    if (split.lowMidi == split.highMidi && splitRoot == 0)
                    {
                        /* Single-key split with unset root key: infer from split key.
                         * rootKey=60 is a valid explicit value (sample pitched at middle C)
                         * and must NOT be overridden - doing so would break instruments
                         * where all splits share the same root key for transposition. */
                        splitRoot = (int16_t)split.lowMidi;
                    }
                    if (splitRoot < 0 || splitRoot > 127)
                    {
                        splitRoot = baseRootKey;
                    }
                    splitRootForLoad = PV_ClampMidi7Bit(splitRoot);
                }
                else
                {
                    /* Root key comes from the SND's baseFrequency; pass 0 so
                     * PV_AddEmbeddedSampleVariant falls back to sdi.baseKey. */
                    splitRootForLoad = 0;
                }

                sampleName[0] = 0;
                if (PV_GetEmbeddedSampleDisplayName(fileRef, split.sndResourceID, sampleName) != BAE_NO_ERROR)
                {
                    XStrCpy(sampleName, instName);
                }

                /* If the SND doesn't exist in this file, treat it as a bank alias. */
                if (!PV_SndExistsInOriginalResources(document, split.sndResourceID))
                {
                    if (PV_AddBankAliasSample(document,
                                              NULL,
                                              NULL,
                                              instID,
                                              sampleName,
                                              program,
                                              split.sndResourceID,
                                              splitRootForLoad,
                                              PV_ClampMidi7Bit((int32_t)split.lowMidi),
                                              PV_ClampMidi7Bit((int32_t)split.highMidi)) == BAE_NO_ERROR)
                    {
                        document->samples[document->sampleCount - 1].splitVolume = split.miscParameter2;
                    }
                }
                else if (PV_AddEmbeddedSampleVariant(document,
                                                fileRef,
                                                instID,
                                                sampleName,
                                                program,
                                                split.sndResourceID,
                                                splitRootForLoad,
                                                PV_ClampMidi7Bit((int32_t)split.lowMidi),
                                                PV_ClampMidi7Bit((int32_t)split.highMidi)) != BAE_NO_ERROR)
                {
                    debug_message("[RMF] INST ID=%ld split=%d failed to load sndID=%d\n",
                               (long)instID, (int)splitIndex, (int)split.sndResourceID);
                }
                else
                {
                    /* Store per-split volume (miscParameter2) on the newly added sample */
                    document->samples[document->sampleCount - 1].splitVolume = split.miscParameter2;
                }
            }
        }
        else
        {
            unsigned char nonSplitRootForLoad;
            char sampleName[256];
            if (useSoundModifierAsRootKey)
            {
                /* miscParameter1 holds the root key override for non-split instruments */
                nonSplitRootForLoad = PV_ClampMidi7Bit(instMiscParam1 ? instMiscParam1 : baseRootKey);
            }
            else
            {
                /* Root key comes from the SND's baseFrequency; pass 0 so
                 * PV_AddEmbeddedSampleVariant falls back to sdi.baseKey. */
                nonSplitRootForLoad = 0;
            }

            sampleName[0] = 0;
            if (PV_GetEmbeddedSampleDisplayName(fileRef, baseSndID, sampleName) != BAE_NO_ERROR)
            {
                XStrCpy(sampleName, instName);
            }

            /* If the SND doesn't exist in this file, treat it as a bank alias. */
            if (!PV_SndExistsInOriginalResources(document, baseSndID))
            {
                if (PV_AddBankAliasSample(document,
                                          NULL,
                                          NULL,
                                          instID,
                                          sampleName,
                                          program,
                                          baseSndID,
                                          nonSplitRootForLoad,
                                          0,
                                          127) == BAE_NO_ERROR)
                {
                    document->samples[document->sampleCount - 1].splitVolume =
                        (int16_t)XGetShort(&inst->miscParameter2);
                }
            }
            else if (PV_AddEmbeddedSampleVariant(document,
                                            fileRef,
                                            instID,
                                            sampleName,
                                            program,
                                            baseSndID,
                                            nonSplitRootForLoad,
                                            0,
                                            127) != BAE_NO_ERROR)
            {
                debug_message("[RMF] INST ID=%ld failed to load base sndID=%d\n",
                           (long)instID, (int)baseSndID);
            }
            else
            {
                /* Store header miscParameter2 as the split volume for non-split instruments */
                document->samples[document->sampleCount - 1].splitVolume =
                    (int16_t)XGetShort(&inst->miscParameter2);
            }
        }
        }

        /* Parse and store extended instrument data (ADSR, LPF, LFO, curves) */
        if (!PV_FindInstrumentExt(document, instID))
        {
            BAERmfEditorInstrumentExt extData;
            PV_ParseExtendedInstData(instData, instSize, &extData);
            extData.instID = instID;
            extData.dirty = FALSE;
            extData.displayName = instName[0] ? PV_DuplicateString(instName) : NULL;
            /* Keep raw blob for bit-perfect round-trip of unmodified instruments */
            extData.originalInstData = XNewPtr(instSize);
            if (extData.originalInstData)
            {
                XBlockMove(instData, extData.originalInstData, instSize);
                extData.originalInstSize = instSize;
            }
            if (PV_AddInstrumentExt(document, &extData) != BAE_NO_ERROR)
            {
                PV_FreeString(&extData.displayName);
                if (extData.originalInstData)
                {
                    XDisposePtr(extData.originalInstData);
                    extData.originalInstData = NULL;
                }
            }
        }

        XDisposePtr(instData);
    }
}


BAEResult PV_LoadRmfResourceIntoDocument(BAERmfEditorDocument *document, XFILE fileRef)
{
    SongResource *songResource;
    SongResource_Info *songInfo;
    XPTR midiData;
    int32_t songSize;
    int32_t midiSize;
    BAEResult result;
    XLongResourceID songID;
    XShortResourceID objectResourceID;

    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }
    result = BAE_BAD_FILE;
    songResource = NULL;
    songInfo = NULL;
    midiData = NULL;
    result = PV_CaptureOriginalResourcesFromFile(document, fileRef);
    if (result != BAE_NO_ERROR)
    {
        debug_message("[RMF] Failed to capture original resource map result=%d\n", (int)result);
        return result;
    }
    songResource = (SongResource *)XGetIndexedFileResource(fileRef, ID_SONG, &songID, 0, NULL, &songSize);
    if (!songResource)
    {
        debug_message("[RMF] No SONG resource found\n");
        return BAE_BAD_FILE;
    }
    debug_message("[RMF] SONG resource found, ID=%ld, size=%ld\n", (long)songID, (long)songSize);
    songInfo = XGetSongResourceInfo(songResource, songSize);
    if (!songInfo)
    {
        debug_message("[RMF] XGetSongResourceInfo failed\n");
        XDisposePtr(songResource);
        return BAE_BAD_FILE;
    }
    if (songInfo->songTempo > 0 && songInfo->songTempo <= 500)
    {
        /* Some files store BPM here, while classic RMF stores a master-tempo scalar. */
        document->tempoBPM = (uint32_t)songInfo->songTempo;
    }
    document->songType = songInfo->songType;
    document->songTempo = songInfo->songTempo;
    document->songPitchShift = songInfo->songPitchShift;
    document->songLocked = songInfo->songLocked;
    document->songEmbedded = songInfo->songEmbedded;
    document->maxMidiNotes = songInfo->maxMidiNotes;
    document->maxEffects = songInfo->maxEffects;
    document->mixLevel = songInfo->mixLevel;
    document->songVolume = songInfo->songVolume;
    document->reverbType = (BAEReverbType)songInfo->reverbType;
    document->engineConfigFlags = songInfo->engineConfigFlags;
    document->velocityCurveType = DEFAULT_VELOCITY_CURVE;
    if (document->engineConfigFlags & SONG_CONFIG_OVERRIDE_VOLUME_CURVE)
    {
        document->velocityCurveType = (VelocityCurveType)((document->engineConfigFlags & SONG_CONFIG_VOLUME_CURVE_TYPE_MASK) >> SONG_CONFIG_VOLUME_CURVE_TYPE_SHIFT);
        if (document->velocityCurveType > 5)
            document->velocityCurveType = DEFAULT_VELOCITY_CURVE;
    }
    if (songInfo->title)
    {
        BAERmfEditorDocument_SetInfo(document, TITLE_INFO, songInfo->title);
    }
    if (songInfo->performed)
    {
        BAERmfEditorDocument_SetInfo(document, PERFORMED_BY_INFO, songInfo->performed);
    }
    if (songInfo->composer)
    {
        BAERmfEditorDocument_SetInfo(document, COMPOSER_INFO, songInfo->composer);
    }
    if (songInfo->copyright)
    {
        BAERmfEditorDocument_SetInfo(document, COPYRIGHT_INFO, songInfo->copyright);
    }
    if (songInfo->publisher_contact_info)
    {
        BAERmfEditorDocument_SetInfo(document, PUBLISHER_CONTACT_INFO, songInfo->publisher_contact_info);
    }
    if (songInfo->use_license)
    {
        BAERmfEditorDocument_SetInfo(document, USE_OF_LICENSE_INFO, songInfo->use_license);
    }
    if (songInfo->licensed_to_URL)
    {
        BAERmfEditorDocument_SetInfo(document, LICENSED_TO_URL_INFO, songInfo->licensed_to_URL);
    }
    if (songInfo->license_term)
    {
        BAERmfEditorDocument_SetInfo(document, LICENSE_TERM_INFO, songInfo->license_term);
    }
    if (songInfo->expire_date)
    {
        BAERmfEditorDocument_SetInfo(document, EXPIRATION_DATE_INFO, songInfo->expire_date);
    }
    if (songInfo->compser_notes)
    {
        BAERmfEditorDocument_SetInfo(document, COMPOSER_NOTES_INFO, songInfo->compser_notes);
    }
    if (songInfo->index_number)
    {
        BAERmfEditorDocument_SetInfo(document, INDEX_NUMBER_INFO, songInfo->index_number);
    }
    if (songInfo->genre)
    {
        BAERmfEditorDocument_SetInfo(document, GENRE_INFO, songInfo->genre);
    }
    if (songInfo->sub_genre)
    {
        BAERmfEditorDocument_SetInfo(document, SUB_GENRE_INFO, songInfo->sub_genre);
    }
    if (songInfo->tempo_description)
    {
        BAERmfEditorDocument_SetInfo(document, TEMPO_DESCRIPTION_INFO, songInfo->tempo_description);
    }
    if (songInfo->original_source)
    {
        BAERmfEditorDocument_SetInfo(document, ORIGINAL_SOURCE_INFO, songInfo->original_source);
    }
    objectResourceID = songInfo->objectResourceID;
    document->originalSongID = songID;
    document->originalObjectResourceID = (XLongResourceID)objectResourceID;
    document->originalMidiType = ID_MIDI;
    debug_message("[RMF] objectResourceID=%d, tempo=%ld\n", (int)objectResourceID, (long)songInfo->songTempo);
    midiData = XGetFileResource(fileRef, ID_MIDI, objectResourceID, NULL, &midiSize);
    if (midiData)
    {
        document->originalMidiType = ID_MIDI;
    }
    if (!midiData)
    {
        debug_message("[RMF] No ID_MIDI with objectResourceID=%d, trying ID_MIDI_OLD\n", (int)objectResourceID);
        midiData = XGetFileResource(fileRef, ID_MIDI_OLD, objectResourceID, NULL, &midiSize);
        if (midiData)
        {
            document->originalMidiType = ID_MIDI_OLD;
        }
    }
    if (!midiData)
    {
        debug_message("[RMF] No ID_MIDI_OLD with objectResourceID=%d, trying ID_ECMI\n", (int)objectResourceID);
        midiData = PV_DecodeMidiData(XGetFileResource(fileRef, ID_ECMI, objectResourceID, NULL, &midiSize), ID_ECMI, &midiSize);
        if (midiData)
        {
            document->originalMidiType = ID_ECMI;
        }
    }
    if (!midiData)
    {
        debug_message("[RMF] No ID_ECMI with objectResourceID=%d, trying ID_EMID\n", (int)objectResourceID);
        midiData = PV_DecodeMidiData(XGetFileResource(fileRef, ID_EMID, objectResourceID, NULL, &midiSize), ID_EMID, &midiSize);
        if (midiData)
        {
            document->originalMidiType = ID_EMID;
        }
    }
    if (!midiData)
    {
        debug_message("[RMF] No ID_EMID with objectResourceID=%d, trying ID_CMID\n", (int)objectResourceID);
        midiData = PV_DecodeMidiData(XGetFileResource(fileRef, ID_CMID, objectResourceID, NULL, &midiSize), ID_CMID, &midiSize);
        if (midiData)
        {
            document->originalMidiType = ID_CMID;
        }
    }
    if (midiData)
    {
        debug_message("[RMF] Got MIDI data, size=%ld\n", (long)midiSize);
        result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
        debug_message("[RMF] PV_LoadMidiBytesIntoDocument result=%d, trackCount=%u\n", (int)result, document->trackCount);
        if (result == BAE_NO_ERROR)
        {
            BAEResult copyResult;

            copyResult = PV_SetDebugOriginalMidiData(document,
                                                     (unsigned char const *)midiData,
                                                     (uint32_t)midiSize);
            if (copyResult != BAE_NO_ERROR)
            {
                XDisposeSongResourceInfo(songInfo);
                XDisposePtr(songResource);
                XDisposePtr(midiData);
                return copyResult;
            }
        }
    }
    else
    {
        debug_message("[RMF] No MIDI data found by objectResourceID\n");
    }
    if (result != BAE_NO_ERROR)
    {
        XLongResourceID fallbackID;
        int32_t fallbackIndex;

        debug_message("[RMF] Primary MIDI load failed, trying indexed fallback scan\n");
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = XGetIndexedFileResource(fileRef, ID_MIDI, &fallbackID, fallbackIndex++, NULL, &midiSize);
            if (!midiData)
            {
                debug_message("[RMF] No more indexed ID_MIDI resources\n");
                break;
            }
            debug_message("[RMF] Trying indexed ID_MIDI[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            debug_message("[RMF] indexed ID_MIDI result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_MIDI;
            }
        }
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = XGetIndexedFileResource(fileRef, ID_MIDI_OLD, &fallbackID, fallbackIndex++, NULL, &midiSize);
            if (!midiData)
            {
                debug_message("[RMF] No more indexed ID_MIDI_OLD resources\n");
                break;
            }
            debug_message("[RMF] Trying indexed ID_MIDI_OLD[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            debug_message("[RMF] indexed ID_MIDI_OLD result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_MIDI_OLD;
            }
        }
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = PV_DecodeMidiData(XGetIndexedFileResource(fileRef, ID_ECMI, &fallbackID, fallbackIndex++, NULL, &midiSize), ID_ECMI, &midiSize);
            if (!midiData)
            {
                debug_message("[RMF] No more indexed ID_ECMI resources\n");
                break;
            }
            debug_message("[RMF] Trying indexed ID_ECMI[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            debug_message("[RMF] indexed ID_ECMI result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_ECMI;
            }
        }
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = PV_DecodeMidiData(XGetIndexedFileResource(fileRef, ID_EMID, &fallbackID, fallbackIndex++, NULL, &midiSize), ID_EMID, &midiSize);
            if (!midiData)
            {
                debug_message("[RMF] No more indexed ID_EMID resources\n");
                break;
            }
            debug_message("[RMF] Trying indexed ID_EMID[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            debug_message("[RMF] indexed ID_EMID result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_EMID;
            }
        }
        fallbackIndex = 0;
        while (result != BAE_NO_ERROR)
        {
            if (midiData)
            {
                XDisposePtr(midiData);
                midiData = NULL;
            }
            midiData = PV_DecodeMidiData(XGetIndexedFileResource(fileRef, ID_CMID, &fallbackID, fallbackIndex++, NULL, &midiSize), ID_CMID, &midiSize);
            if (!midiData)
            {
                debug_message("[RMF] No more indexed ID_CMID resources\n");
                break;
            }
            debug_message("[RMF] Trying indexed ID_CMID[%d], ID=%ld, size=%ld\n", (int)(fallbackIndex-1), (long)fallbackID, (long)midiSize);
            result = PV_LoadMidiBytesIntoDocument(document, (unsigned char const *)midiData, (uint32_t)midiSize);
            debug_message("[RMF] indexed ID_CMID result=%d\n", (int)result);
            if (result == BAE_NO_ERROR)
            {
                document->originalObjectResourceID = fallbackID;
                document->originalMidiType = ID_CMID;
            }
        }
    }
    XDisposeSongResourceInfo(songInfo);
    XDisposePtr(songResource);
    if (midiData)
    {
        XDisposePtr(midiData);
    }
    /* Extract embedded SND/INST samples into the document sample list */
    if (result == BAE_NO_ERROR)
    {
        PV_LoadEmbeddedSamplesFromRmf(document, fileRef);
        switch (document->originalMidiType)
        {
            case ID_CMID:
                document->midiStorageType = BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT;
                break;
            case ID_EMID:
                document->midiStorageType = BAE_EDITOR_MIDI_STORAGE_EMID;
                break;
            case ID_MIDI:
            case ID_MIDI_OLD:
                document->midiStorageType = BAE_EDITOR_MIDI_STORAGE_MIDI;
                break;
            case ID_ECMI:
            default:
                document->midiStorageType = BAE_EDITOR_MIDI_STORAGE_ECMI;
                break;
        }
        document->loadedFromRmf = TRUE;
        document->isPristine = TRUE;
    }
    return result;
}


BAEResult PV_LoadRmfFileIntoDocument(BAERmfEditorDocument *document, BAEPathName filePath)
{
    XFILENAME name;
    XFILE fileRef;
    BAEResult result;

    if (!document || !filePath)
    {
        return BAE_PARAM_ERR;
    }
    debug_message("[RMF] Loading RMF file: %s\n", filePath);
    XConvertPathToXFILENAME(filePath, &name);
    fileRef = XFileOpenResource(&name, TRUE);
    if (!fileRef)
    {
        debug_message("[RMF] XFileOpenResource failed\n");
        return BAE_FILE_IO_ERROR;
    }
    result = PV_LoadRmfResourceIntoDocument(document, fileRef);
    XFileClose(fileRef);
    return result;
}


BAEResult PV_LoadRmfMemoryIntoDocument(BAERmfEditorDocument *document,
                                              void const *rmfData,
                                              uint32_t rmfSize)
{
    XFILE fileRef;
    BAEResult result;

    if (!document || !rmfData || rmfSize == 0)
    {
        return BAE_PARAM_ERR;
    }
    fileRef = XFileOpenResourceFromMemory((XPTR)rmfData, rmfSize, TRUE);
    if (!fileRef)
    {
        return BAE_BAD_FILE;
    }
    result = PV_LoadRmfResourceIntoDocument(document, fileRef);
    XFileClose(fileRef);
    return result;
}


BAEResult PV_GetAvailableResourceID(XFILE fileRef,
                                          XResourceType resourceType,
                                          XLongResourceID startingID,
                                          XLongResourceID *outResourceID)
{
    XFILERESOURCEMAP map;
    int32_t nextOffset;
    int32_t resourceCount;
    int32_t resourceIndex;
    XLongResourceID nextID;

    if (!fileRef || !outResourceID)
    {
        return BAE_PARAM_ERR;
    }
    *outResourceID = 0;
    if (XGetUniqueFileResourceID(fileRef, resourceType, outResourceID) == 0 && *outResourceID != 0)
    {
        return BAE_NO_ERROR;
    }
    nextID = (startingID > 0) ? startingID : 1;
    if (XFileSetPosition(fileRef, 0L) != 0 ||
        XFileRead(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0 ||
        !XFILERESOURCE_ID_IS_VALID(XGetLong(&map.mapID)) ||
        !XFILERESOURCE_VERSION_IS_VALID(XGetLong(&map.version)))
    {
        return BAE_FILE_IO_ERROR;
    }
    nextOffset = (int32_t)sizeof(XFILERESOURCEMAP);
    resourceCount = (int32_t)XGetLong(&map.totalResources);
    for (resourceIndex = 0; resourceIndex < resourceCount; ++resourceIndex)
    {
        int32_t headerNext;
        int32_t data;

        if (XFileSetPosition(fileRef, nextOffset) != 0 ||
            XFileRead(fileRef, &headerNext, (int32_t)sizeof(int32_t)) != 0 ||
            XFileRead(fileRef, &data, (int32_t)sizeof(int32_t)) != 0)
        {
            return BAE_FILE_IO_ERROR;
        }
        headerNext = (int32_t)XGetLong(&headerNext);
        if ((XResourceType)XGetLong(&data) == resourceType)
        {
            if (XFileRead(fileRef, &data, (int32_t)sizeof(int32_t)) != 0)
            {
                return BAE_FILE_IO_ERROR;
            }
            data = (int32_t)XGetLong(&data);
            if ((XLongResourceID)data >= nextID)
            {
                if (data == 0x7FFFFFFF)
                {
                    return BAE_FILE_IO_ERROR;
                }
                nextID = (XLongResourceID)(data + 1);
            }
        }
        if (resourceIndex < (resourceCount - 1))
        {
            if (headerNext <= nextOffset)
            {
                return BAE_FILE_IO_ERROR;
            }
            nextOffset = headerNext;
        }
    }
    *outResourceID = nextID;
    return BAE_NO_ERROR;
}


BAEResult PV_EnsureResourceFileReady(XFILE fileRef, int32_t resourceID)
{
    XFILERESOURCEMAP map;

    if (!fileRef)
    {
        return BAE_PARAM_ERR;
    }
    if (XFileSetLength(fileRef, 0) != 0)
    {
        return BAE_FILE_IO_ERROR;
    }
    XFileFreeResourceCache(fileRef);
    XPutLong(&map.mapID, resourceID);
    XPutLong(&map.version, XFILERESOURCE_VERSION_FOR_ID(resourceID));
    XPutLong(&map.totalResources, 0);
    if (XFileSetPosition(fileRef, 0L) != 0)
    {
        return BAE_FILE_IO_ERROR;
    }
    if (XFileWrite(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_FILE_IO_ERROR;
    }
    return BAE_NO_ERROR;
}


BAEResult PV_PrepareResourceFilePath(XFILENAME *name, int32_t resourceID)
{
    XFILE fileRef;
    XFILERESOURCEMAP map;
    bool isValid;

    if (!name)
    {
        return BAE_PARAM_ERR;
    }
    isValid = FALSE;
    fileRef = XFileOpenForRead(name);
    if (fileRef)
    {
        if (XFileRead(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) == 0 &&
            XFILERESOURCE_ID_IS_VALID(XGetLong(&map.mapID)) &&
            XFILERESOURCE_VERSION_IS_VALID(XGetLong(&map.version)))
        {
            isValid = TRUE;
        }
        XFileClose(fileRef);
    }
    if (isValid)
    {
        return BAE_NO_ERROR;
    }
    fileRef = XFileOpenForWrite(name, TRUE);
    if (!fileRef)
    {
        return BAE_FILE_IO_ERROR;
    }
    if (XFileSetLength(fileRef, 0) != 0)
    {
        XFileClose(fileRef);
        return BAE_FILE_IO_ERROR;
    }
    XPutLong(&map.mapID, resourceID);
    XPutLong(&map.version, XFILERESOURCE_VERSION_FOR_ID(resourceID));
    XPutLong(&map.totalResources, 0);
    if (XFileSetPosition(fileRef, 0L) != 0 ||
        XFileWrite(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        XFileClose(fileRef);
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(fileRef);
    return BAE_NO_ERROR;
}


BAEResult PV_WriteOriginalResources(BAERmfEditorDocument const *document, XFILE fileRef)
{
    uint32_t index;

    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }
    for (index = 0; index < document->originalResourceCount; ++index)
    {
        BAERmfEditorResourceEntry const *entry;

        entry = &document->originalResources[index];
        if (!entry->data || entry->size < 0)
        {
            return BAE_BAD_FILE;
        }
        if (entry->type == FOUR_CHAR('C', 'a', 'S', 'd'))
        {
            continue; /* Session CaSd masters stay out of RMF/ZMF writes. */
        }
        if (XAddFileResource(fileRef,
                             entry->type,
                             entry->id,
                             entry->pascalName,
                             entry->data,
                             entry->size) != 0)
        {
            return BAE_FILE_IO_ERROR;
        }
    }
    return BAE_NO_ERROR;
}


#if USE_ZMF_SUPPORT == TRUE
/* Write INST resources for oscillator-only (sample-free) instruments that were
 * not emitted by the sample-driven INST builder above/below. */
BAEResult PV_AddSampleFreeInstrumentResources(BAERmfEditorDocument *document,
                                                     XFILE fileRef)
{
    uint32_t extIndex;

    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }

    for (extIndex = 0; extIndex < document->instrumentExtCount; ++extIndex)
    {
        BAERmfEditorInstrumentExt const *ext = &document->instrumentExts[extIndex];
        XLongResourceID instID;
        uint32_t sampleIndex;
        bool hasSample;
        char pascalName[256];
        BAEResult nameResult;
        XPTR extTail;
        int32_t extTailSize;
        unsigned char *instBytes;
        int32_t instSize;
        int32_t totalSize;

        if (!ext->useOscillator)
        {
            continue;
        }
        instID = ext->instID;
        /* INST id 0 is valid (bank-group 0 / program 0). */
        if (XExistsFileResource(fileRef, ID_INST, instID) != FALSE)
        {
            continue;
        }

        hasSample = FALSE;
        for (sampleIndex = 0; sampleIndex < document->sampleCount; ++sampleIndex)
        {
            if (document->samples[sampleIndex].instID == (uint32_t)instID)
            {
                hasSample = TRUE;
                break;
            }
        }
        if (hasSample)
        {
            continue;
        }

        nameResult = PV_CreatePascalName(
            (ext->displayName && ext->displayName[0]) ? ext->displayName : "Oscillator",
            pascalName);
        if (nameResult != BAE_NO_ERROR)
        {
            return nameResult;
        }

        /* Prefer bit-perfect original INST when oscillator mode was not edited. */
        if (!ext->dirty && ext->originalInstData && ext->originalInstSize >= 14)
        {
            debug_message("[RMF Save] INST id=%ld sample-free oscillator verbatim (%ld bytes)\n",
                          (long)instID, (long)ext->originalInstSize);
            if (XAddFileResource(fileRef,
                                 ID_INST,
                                 instID,
                                 pascalName,
                                 ext->originalInstData,
                                 ext->originalInstSize) != 0)
            {
                return BAE_FILE_IO_ERROR;
            }
            continue;
        }

        extTail = NULL;
        extTailSize = 0;
        if (ext->hasExtendedData || ext->dirty || ext->useOscillator)
        {
            if (!ext->dirty && ext->originalInstData && ext->originalInstSize > 0)
            {
                if (PV_CopyOriginalInstExtendedTail(ext, &extTail, &extTailSize) != BAE_NO_ERROR)
                {
                    extTail = NULL;
                    extTailSize = 0;
                }
            }
            if (!extTail)
            {
                extTail = PV_SerializeExtendedInstTail(ext, &extTailSize);
            }
        }

        /* Non-split INST header + tremolo terminator (same layout as empty bank INST). */
        instSize = (int32_t)sizeof(InstrumentResource);
        totalSize = instSize + extTailSize;
        instBytes = (unsigned char *)XNewPtr(totalSize);
        if (!instBytes)
        {
            if (extTail)
            {
                XDisposePtr(extTail);
            }
            return BAE_MEMORY_ERR;
        }
        XSetMemory(instBytes, totalSize, 0);
        XPutShort(instBytes + 0, 0xFFFFu); /* sndResourceID: no sample */
        XPutShort(instBytes + 2, ext->midiRootKey ? (uint16_t)ext->midiRootKey : 60u);
        instBytes[4] = (unsigned char)ext->panPlacement;
        instBytes[5] = (unsigned char)(ext->flags1 | ZBF_extendedFormat);
        instBytes[6] = ext->flags2;
        XPutShort(instBytes + 8, (uint16_t)ext->miscParameter1);
        XPutShort(instBytes + 10, (uint16_t)(ext->miscParameter2 ? ext->miscParameter2 : 100));
        XPutShort(instBytes + 12, 0); /* keySplitCount */
        XPutShort(instBytes + 14, 0); /* tremoloCount */
        XPutShort(instBytes + 16, 0x8000); /* tremoloEnd */
        if (extTail && extTailSize > 0)
        {
            XBlockMove(extTail, instBytes + instSize, extTailSize);
        }
        if (extTail)
        {
            XDisposePtr(extTail);
        }

        debug_message("[RMF Save] INST id=%ld sample-free oscillator rebuilt (%ld bytes)\n",
                      (long)instID, (long)totalSize);
        if (XAddFileResource(fileRef, ID_INST, instID, pascalName, instBytes, totalSize) != 0)
        {
            XDisposePtr((XPTR)instBytes);
            return BAE_FILE_IO_ERROR;
        }
        XDisposePtr((XPTR)instBytes);
    }

    return BAE_NO_ERROR;
}

#endif /* USE_ZMF_SUPPORT */

BAEResult PV_AddSampleResources(BAERmfEditorDocument *document, XFILE fileRef, bool isZmf)
{
    uint32_t index;
    XShortResourceID *sampleSndIDs;
    XLongResourceID *sampleInstIDs;

    debug_message("[RMF Save] PV_AddSampleResources entered sampleCount=%u\n", document ? (unsigned)document->sampleCount : 0U);
    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }
    if (document->sampleCount == 0)
    {
        debug_message("[RMF Save] PV_AddSampleResources: no samples\n");
#if USE_ZMF_SUPPORT == TRUE
        return PV_AddSampleFreeInstrumentResources(document, fileRef);
#else
        return BAE_NO_ERROR;
#endif
    }

    sampleSndIDs = (XShortResourceID *)XNewPtr((int32_t)(document->sampleCount * sizeof(XShortResourceID)));
    sampleInstIDs = (XLongResourceID *)XNewPtr((int32_t)(document->sampleCount * sizeof(XLongResourceID)));
    if (!sampleSndIDs || !sampleInstIDs)
    {
        if (sampleSndIDs)
        {
            XDisposePtr((XPTR)sampleSndIDs);
        }
        if (sampleInstIDs)
        {
            XDisposePtr((XPTR)sampleInstIDs);
        }
        return BAE_MEMORY_ERR;
    }
    XSetMemory(sampleSndIDs, (int32_t)(document->sampleCount * sizeof(XShortResourceID)), 0);
    XSetMemory(sampleInstIDs, (int32_t)(document->sampleCount * sizeof(XLongResourceID)), 0);

    for (index = 0; index < document->sampleCount; ++index)
    {
        BAERmfEditorSample const *sample;
        uint32_t prior;
        XLongResourceID sndID;
        XPTR sndResource;
        XPTR encodeWaveDataOwner;
        OPErr opErr;
        BAEResult result;
        char pascalName[256];
        GM_Waveform writeWaveform;
        uint32_t writeSampleRate;
        int32_t bytesPerFrame;
        uint32_t maxFramesBySize;
        int32_t loopStart;
        int32_t loopEnd;
        uint32_t loopFrameLimit;
        XResourceType writeSndType;
        int32_t roundTripSourceRate;  /* non-zero when encoding Opus round-trip */
        bool samplePlayAtSampledFreq;
        bool sampleAdvancedInterpolation;
        bool sampleWasEncodedOpus;
        bool sampleWasEncodedMpeg;
        uint32_t decodedFramesForRate;
        uint32_t decodedSampleRateForSnd;
        uint32_t prePadWaveFrames;  /* waveFrames before Opus loop-context pad */

        sample = &document->samples[index];
        roundTripSourceRate = 0;
        samplePlayAtSampledFreq = FALSE;
        sampleAdvancedInterpolation = FALSE;
        sampleWasEncodedOpus = FALSE;
        sampleWasEncodedMpeg = FALSE;
        decodedFramesForRate = 0;
        decodedSampleRateForSnd = 0;
        prePadWaveFrames = 0;

        if (sample->instID != BAE_EDITOR_INST_ID_NONE)
        {
            BAERmfEditorInstrumentExt const *sampleExt;

            sampleExt = PV_FindInstrumentExt((BAERmfEditorDocument *)document,
                                             (XLongResourceID)sample->instID);
            if (sampleExt && TEST_FLAG_VALUE(sampleExt->flags2, ZBF_playAtSampledFreq))
            {
                samplePlayAtSampledFreq = TRUE;
            }
            if (sampleExt && TEST_FLAG_VALUE(sampleExt->flags2, ZBF_advancedInterpolation))
            {
                sampleAdvancedInterpolation = TRUE;
            }
        }

        /* Ghost / bank-alias samples (RMF/ZMF song documents only): reference
         * external bank SND IDs and must not be embedded or participate in
         * local SND dedupe/ID assignment. HSB/ZSB bank save never uses this path. */
        if (sample->isBankAlias)
        {
            sampleSndIDs[index] = sample->aliasSndResourceID;
            sampleInstIDs[index] = (sample->instID != BAE_EDITOR_INST_ID_NONE)
                                    ? (XLongResourceID)sample->instID
                                    : (XLongResourceID)(512 + (uint32_t)sample->program);
            continue;
        }

        /* One SND resource per shared sample asset; additional usages reuse it. */
        for (prior = 0; prior < index; ++prior)
        {
            if (PV_CanReuseSndResourceForSamples(sample,
                                                 &document->samples[prior]) &&
                sampleSndIDs[prior] != 0)
            {
                sampleSndIDs[index] = sampleSndIDs[prior];
                sampleInstIDs[index] = (sample->instID != BAE_EDITOR_INST_ID_NONE)
                                        ? (XLongResourceID)sample->instID
                                        : (XLongResourceID)(512 + (uint32_t)sample->program);
                break;
            }
        }
        if (prior < index)
        {
            continue;
        }

        result = PV_CreatePascalName(sample->displayName ? sample->displayName : sample->sourcePath, pascalName);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        {
            bool usedPreferredID;
            XLongResourceID preferredID;

            usedPreferredID = FALSE;
            preferredID = (XLongResourceID)sample->sampleAssetID;

            /* Keep SND IDs stable across saves by reusing the sample-asset ID
             * whenever it is a valid short resource ID (including 0) and not
             * already used in this save pass by another asset. */
            if (sample->sampleAssetID <= 32767u)
            {
                uint32_t priorIDIndex;

                usedPreferredID = TRUE;
                for (priorIDIndex = 0; priorIDIndex < index; ++priorIDIndex)
                {
                    if (sampleSndIDs[priorIDIndex] == (XShortResourceID)preferredID)
                    {
                        usedPreferredID = FALSE;
                        break;
                    }
                }
                if (usedPreferredID)
                {
                    sndID = preferredID;
                }
            }

            if (!usedPreferredID)
            {
                /* Deterministic fallback: scan IDs starting from 1 and pick the
                 * first one not already claimed by a prior sample in this save pass.
                 * This avoids the random ID picker in XGetUniqueFileResourceID so
                 * every save of the same document produces the same SND IDs. */
                XLongResourceID candidateID;
                uint32_t priorIDIndex;
                bool conflict;

                sndID = 0;
                for (candidateID = 1; candidateID <= 32767; ++candidateID)
                {
                    conflict = FALSE;
                    for (priorIDIndex = 0; priorIDIndex < index; ++priorIDIndex)
                    {
                        if (sampleSndIDs[priorIDIndex] == (XShortResourceID)candidateID)
                        {
                            conflict = TRUE;
                            break;
                        }
                    }
                    if (!conflict)
                    {
                        sndID = candidateID;
                        break;
                    }
                }
                if (sndID == 0)
                {
                    return BAE_FILE_IO_ERROR;
                }
            }
        }
        debug_message("[RMF Save] Sample[%u] program=%u waveform=%p theWaveform=%p waveSize=%ld\n",
                   (unsigned)index, (unsigned)sample->program,
                   (void *)sample->waveform,
                   sample->waveform ? (void *)sample->waveform->theWaveform : NULL,
                   sample->waveform ? (long)sample->waveform->waveSize : 0L);
        if (!sample->waveform)
        {
            return BAE_BAD_FILE;
        }
        encodeWaveDataOwner = NULL;
        if (!sample->waveform->theWaveform)
        {
            return BAE_BAD_FILE;
        }
        writeWaveform = *sample->waveform;

        /* Loop/rate are per-sample properties; do not trust shared waveform metadata. */
        writeWaveform.baseMidiPitch = sample->rootKey;
        writeWaveform.startLoop = sample->sampleInfo.startLoop;
        writeWaveform.endLoop = sample->sampleInfo.endLoop;
        writeWaveform.sampledRate = (int32_t)sample->sampleInfo.sampledRate;

        if ((writeWaveform.bitSize != 8 && writeWaveform.bitSize != 16) ||
            (writeWaveform.channels != 1 && writeWaveform.channels != 2))
        {
            return BAE_BAD_FILE;
        }

        bytesPerFrame = (int32_t)((writeWaveform.bitSize / 8) * writeWaveform.channels);
        if (bytesPerFrame <= 0)
        {
            return BAE_BAD_FILE;
        }

        if (writeWaveform.waveFrames == 0 && writeWaveform.waveSize > 0)
        {
            writeWaveform.waveFrames = (uint32_t)(writeWaveform.waveSize / bytesPerFrame);
        }
        if (writeWaveform.waveSize <= 0 && writeWaveform.waveFrames > 0)
        {
            writeWaveform.waveSize = (int32_t)(writeWaveform.waveFrames * (uint32_t)bytesPerFrame);
        }
        if (writeWaveform.waveFrames == 0 || writeWaveform.waveSize <= 0)
        {
            return BAE_BAD_FILE;
        }

        maxFramesBySize = (uint32_t)(writeWaveform.waveSize / bytesPerFrame);
        if (maxFramesBySize == 0)
        {
            return BAE_BAD_FILE;
        }
        if (writeWaveform.waveFrames > maxFramesBySize)
        {
            writeWaveform.waveFrames = maxFramesBySize;
        }
        writeWaveform.waveSize = (int32_t)(writeWaveform.waveFrames * (uint32_t)bytesPerFrame);

        writeSampleRate = (uint32_t)writeWaveform.sampledRate;
        debug_message("[RMF Save] Sample[%u] sampledRate check: writeRate=0x%08lx\n",
                   (unsigned)index,
                   (unsigned long)writeSampleRate);
        
        /* Handle sample rate normalization for saves.
         * Opus always encodes at 48kHz (handled separately).
         * Other codecs should preserve the original sample rate.
         * If rate is in raw Hz (< 4000<<16), convert to fixed-point if valid.
         * If rate is already fixed-point (>= 4000<<16), keep as-is.
         * Only default to 44100 if rate is 0 or invalid. */
        if (writeSampleRate == 0)
        {
            /* Rate is invalid/uninitialized */
            if (PV_IsOpusCompression(sample->targetCompressionType))
            {
                writeSampleRate = 48000L << 16;
                debug_message("[RMF Save] Sample[%u] defaulting to 48000 Hz for Opus (was 0)\n", (unsigned)index);
            }
            else
            {
                writeSampleRate = 44100L << 16;
                debug_message("[RMF Save] Sample[%u] defaulting to 44100 Hz (was 0)\n", (unsigned)index);
            }
        }
        else if (writeSampleRate < (1000U << 16))
        {
            /* Looks like raw Hz instead of fixed-point */
            if (writeSampleRate >= 1000U && writeSampleRate <= 384000U)
            {
                /* Valid raw Hz range, convert to fixed-point */
                debug_message("[RMF Save] Sample[%u] converting raw Hz to fixed-point: %lu -> ", 
                           (unsigned)index, (unsigned long)writeSampleRate);
                writeSampleRate <<= 16;
                debug_message("0x%08lx\n", (unsigned long)writeSampleRate);
            }
            else
            {
                /* Out of range raw Hz, default */
                debug_message("[RMF Save] Sample[%u] raw Hz out of range (%lu), defaulting\n", 
                           (unsigned)index, (unsigned long)writeSampleRate);
                if (PV_IsOpusCompression(sample->targetCompressionType))
                {
                    writeSampleRate = 48000L << 16;
                }
                else
                {
                    writeSampleRate = 44100L << 16;
                }
            }
        }
        else
        {
            /* Already in fixed-point format, preserve as-is */
            debug_message("[RMF Save] Sample[%u] preserving fixed-point rate 0x%08lx\n", 
                       (unsigned)index, (unsigned long)writeSampleRate);
        }
        writeWaveform.sampledRate = (int32_t)writeSampleRate;

        loopFrameLimit = writeWaveform.waveFrames;
        if (sample->sampleInfo.waveFrames > 0 && sample->sampleInfo.waveFrames < loopFrameLimit)
        {
            loopFrameLimit = sample->sampleInfo.waveFrames;
        }

        loopStart = (int32_t)writeWaveform.startLoop;
        loopEnd = (int32_t)writeWaveform.endLoop;
        if (loopStart < 0 || loopStart >= (int32_t)loopFrameLimit ||
            loopEnd <= loopStart || loopEnd > (int32_t)loopFrameLimit)
        {
            loopStart = 0;
            loopEnd = 0;
        }
        writeWaveform.startLoop = (uint32_t)loopStart;
        writeWaveform.endLoop = (uint32_t)loopEnd;

        debug_message("[RMF Save] Sample[%u] rate raw=0x%08lx write=0x%08lx\n",
                   (unsigned)index,
                   (unsigned long)(uint32_t)sample->waveform->sampledRate,
                   (unsigned long)(uint32_t)writeWaveform.sampledRate);
        debug_message("[RMF Save] Sample[%u] frames=%u size=%ld bits=%u ch=%u loop=%u-%u\n",
                   (unsigned)index,
                   (unsigned)writeWaveform.waveFrames,
                   (long)writeWaveform.waveSize,
                   (unsigned)writeWaveform.bitSize,
                   (unsigned)writeWaveform.channels,
                   (unsigned)writeWaveform.startLoop,
                   (unsigned)writeWaveform.endLoop);
        sndResource = NULL;
        /* DONT_CHANGE: reuse the cached plain SND blob directly, then apply the
         * selected storage wrapper (esnd/csnd/snd) later in this save path. */
        if (sample->targetCompressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE &&
            sample->originalSndData && sample->originalSndSize > 0)
        {
            sndResource = XNewPtr((int32_t)sample->originalSndSize);
            if (!sndResource)
            {
                XDisposePtr((XPTR)sampleSndIDs);
                XDisposePtr((XPTR)sampleInstIDs);
                return BAE_MEMORY_ERR;
            }
            XBlockMove(sample->originalSndData, sndResource, (int32_t)sample->originalSndSize);
            debug_message("[RMF Save] Sample[%u] using cached plain SND blob (%ld bytes)\n",
                       (unsigned)index, (long)sample->originalSndSize);
        }
        else
        {
            /* Map BAERmfEditorCompressionType -> SndCompressionType + sub-type.
             * MPEG bitrates each have their own SndCompressionType constant;
             * Vorbis/Opus use a subtype to select target bitrate tier;
             * FLAC and ADPCM use a single type constant with CS_DEFAULT sub-type. */
            SndCompressionType compType;
            SndCompressionSubType compSubType;
            SndCompressionSubType encodeCompSubType;
            switch (sample->targetCompressionType)
            {
                case BAE_EDITOR_COMPRESSION_DONT_CHANGE:
                    if (PV_IsSupportedPassthroughCompression((SndCompressionType)sample->sourceCompressionType))
                    {
                        compType = (SndCompressionType)sample->sourceCompressionType;
                        compSubType = (SndCompressionSubType)sample->sourceCompressionSubType;
#if USE_MPEG_ENCODER == TRUE || USE_MPEG_DECODER == TRUE
                        if ((compType == C_MPEG_32 || compType == C_MPEG_40 || compType == C_MPEG_48 ||
                             compType == C_MPEG_56 || compType == C_MPEG_64 || compType == C_MPEG_80 ||
                             compType == C_MPEG_96 || compType == C_MPEG_112 || compType == C_MPEG_128 ||
                             compType == C_MPEG_160 || compType == C_MPEG_192 || compType == C_MPEG_224 ||
                             compType == C_MPEG_256 || compType == C_MPEG_320) &&
                            compSubType == CS_DEFAULT)
                        {
                            compSubType = CS_MPEG2;
                        }
#endif
                    }
                    else
                    {
                        compType = C_NONE;
                        compSubType = CS_DEFAULT;
                    }
                    break;
                case BAE_EDITOR_COMPRESSION_ADPCM:
                    compType    = C_IMA4;
                    compSubType = CS_DEFAULT;
                    break;
#if USE_ZMF_SUPPORT == TRUE
                case BAE_EDITOR_COMPRESSION_ADPCM_2BIT:
                    compType    = C_IMA2;
                    compSubType = CS_DEFAULT;
                    break;
#endif
                case BAE_EDITOR_COMPRESSION_MP3_32K:
                    compType    = C_MPEG_32;
                    compSubType = CS_MPEG2;
                    break;
                case BAE_EDITOR_COMPRESSION_MP3_48K:
                    compType    = C_MPEG_48;
                    compSubType = CS_MPEG2;
                    break;
                case BAE_EDITOR_COMPRESSION_MP3_64K:
                    compType    = C_MPEG_64;
                    compSubType = CS_MPEG2;
                    break;
                case BAE_EDITOR_COMPRESSION_MP3_96K:
                    compType    = C_MPEG_96;
                    compSubType = CS_MPEG2;
                    break;
                case BAE_EDITOR_COMPRESSION_MP3_128K:
                    compType    = C_MPEG_128;
                    compSubType = CS_MPEG2;
                    break;
                case BAE_EDITOR_COMPRESSION_MP3_192K:
                    compType    = C_MPEG_192;
                    compSubType = CS_MPEG2;
                    break;
                case BAE_EDITOR_COMPRESSION_MP3_256K:
                    compType    = C_MPEG_256;
                    compSubType = CS_MPEG2;
                    break;
                case BAE_EDITOR_COMPRESSION_MP3_320K:
                    compType    = C_MPEG_320;
                    compSubType = CS_MPEG2;
                    break;
#if USE_VORBIS_ENCODER == TRUE && USE_VORBIS_DECODER == TRUE                    
                case BAE_EDITOR_COMPRESSION_VORBIS_32K:
                    compType    = C_VORBIS;
                    compSubType = CS_VORBIS_32K;
                    break;
                case BAE_EDITOR_COMPRESSION_VORBIS_48K:
                    compType    = C_VORBIS;
                    compSubType = CS_VORBIS_48K;
                    break;
                case BAE_EDITOR_COMPRESSION_VORBIS_64K:
                    compType    = C_VORBIS;
                    compSubType = CS_VORBIS_64K;
                    break;
                case BAE_EDITOR_COMPRESSION_VORBIS_80K:
                    compType    = C_VORBIS;
                    compSubType = CS_VORBIS_80K;
                    break;
                case BAE_EDITOR_COMPRESSION_VORBIS_96K:
                    compType    = C_VORBIS;
                    compSubType = CS_VORBIS_96K;
                    break;
                case BAE_EDITOR_COMPRESSION_VORBIS_128K:
                    compType    = C_VORBIS;
                    compSubType = CS_VORBIS_128K;
                    break;
                case BAE_EDITOR_COMPRESSION_VORBIS_160K:
                    compType    = C_VORBIS;
                    compSubType = CS_VORBIS_160K;
                    break;
                case BAE_EDITOR_COMPRESSION_VORBIS_192K:
                    compType    = C_VORBIS;
                    compSubType = CS_VORBIS_192K;
                    break;
                case BAE_EDITOR_COMPRESSION_VORBIS_256K:
                    compType    = C_VORBIS;
                    compSubType = CS_VORBIS_256K;
                    break;
#endif /* USE_VORBIS_ENCODER && USE_VORBIS_DECODER */                    
#if USE_FLAC_ENCODER == TRUE && USE_FLAC_DECODER == TRUE
                case BAE_EDITOR_COMPRESSION_FLAC:
                    compType    = C_FLAC;
                    compSubType = CS_DEFAULT;
                    break;
#endif /* USE_FLAC_ENCODER && USE_FLAC_DECODER */
#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
                case BAE_EDITOR_COMPRESSION_OPUS_12K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_12K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_16K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_16K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_24K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_24K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_32K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_32K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_48K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_48K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_64K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_64K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_80K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_80K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_96K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_96K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_128K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_128K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_160K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_160K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_192K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_192K;
                    break;
                case BAE_EDITOR_COMPRESSION_OPUS_256K:
                    compType    = C_OPUS;
                    compSubType = CS_OPUS_256K;
                    break;
#endif
#if USE_QOA_SUPPORT == TRUE
                case BAE_EDITOR_COMPRESSION_QOA:
                    compType    = C_QOA;
                    compSubType = CS_DEFAULT;
                    break;
#endif
                case BAE_EDITOR_COMPRESSION_PCM:
                default:
                    compType    = C_NONE;
                    compSubType = CS_DEFAULT;
                    break;
            }
            encodeCompSubType = compSubType;

#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
            if (compType == C_OPUS)
            {
                encodeCompSubType = PV_ComposeOpusEncodeSubType(compSubType, sample->targetOpusMode);
                sampleWasEncodedOpus = TRUE;
            }
            else
#endif            
            if (compType == C_MPEG_32 || compType == C_MPEG_40 || compType == C_MPEG_48 ||
                     compType == C_MPEG_56 || compType == C_MPEG_64 || compType == C_MPEG_80 ||
                     compType == C_MPEG_96 || compType == C_MPEG_112 || compType == C_MPEG_128 ||
                     compType == C_MPEG_160 || compType == C_MPEG_192 || compType == C_MPEG_224 ||
                     compType == C_MPEG_256 || compType == C_MPEG_320)
            {
                sampleWasEncodedMpeg = TRUE;
            }
#if USE_VORBIS_ENCODER == TRUE && USE_VORBIS_DECODER == TRUE            
            else if (compType == C_VORBIS)
            {
                sampleWasEncodedMpeg = TRUE;
            }
#endif            
#if USE_OPUS_ENCODER == TRUE && USE_OPUS_DECODER == TRUE
            if (sample->sourceCompressionType == (uint32_t)C_OPUS &&
                compType != C_OPUS)
            {
                BAE_UNSIGNED_FIXED decodedRate;
                BAE_UNSIGNED_FIXED targetRate;

                decodedRate = PV_NormalizeSampleRateForSave((BAE_UNSIGNED_FIXED)sample->waveform->sampledRate);
                targetRate = PV_NormalizeSampleRateForSave((BAE_UNSIGNED_FIXED)writeWaveform.sampledRate);
                if (decodedRate != 0 && targetRate != 0 && decodedRate != targetRate)
                {
                    result = PV_ResampleWaveformLinear(&writeWaveform,
                                                       targetRate,
                                                       &encodeWaveDataOwner);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return result;
                    }
                    debug_message("[RMF Save] Sample[%u] retimed decoded Opus PCM %luHz -> %luHz for non-Opus encode\n",
                               (unsigned)index,
                               (unsigned long)(decodedRate >> 16),
                               (unsigned long)(targetRate >> 16));
                }
            }

            /*
             * Some editor paths can leave a mono source represented as dual-mono
             * PCM (L == R) while preserving sampleInfo.channels = 1. Keep Opus
             * exports truly mono by collapsing that representation before encode.
             */
            if (compType == C_OPUS &&
                writeWaveform.channels == 2 &&
                sample->sampleInfo.channels == 1 &&
                writeWaveform.theWaveform &&
                writeWaveform.waveFrames > 0 &&
                (writeWaveform.bitSize == 8 || writeWaveform.bitSize == 16))
            {
                bool dualMono;
                uint32_t frame;
                uint32_t monoBytes;
                XPTR monoData;

                dualMono = TRUE;
                if (writeWaveform.bitSize == 16)
                {
                    int16_t const *pcm16;
                    pcm16 = (int16_t const *)writeWaveform.theWaveform;
                    for (frame = 0; frame < writeWaveform.waveFrames; ++frame)
                    {
                        if (pcm16[frame * 2] != pcm16[frame * 2 + 1])
                        {
                            dualMono = FALSE;
                            break;
                        }
                    }
                }
                else
                {
                    unsigned char const *pcm8;
                    pcm8 = (unsigned char const *)writeWaveform.theWaveform;
                    for (frame = 0; frame < writeWaveform.waveFrames; ++frame)
                    {
                        if (pcm8[frame * 2] != pcm8[frame * 2 + 1])
                        {
                            dualMono = FALSE;
                            break;
                        }
                    }
                }

                if (dualMono)
                {
                    monoBytes = writeWaveform.waveFrames * (uint32_t)(writeWaveform.bitSize / 8);
                    monoData = XNewPtr((int32_t)monoBytes);
                    if (!monoData)
                    {
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return BAE_MEMORY_ERR;
                    }

                    if (writeWaveform.bitSize == 16)
                    {
                        int16_t const *src16;
                        int16_t *dst16;
                        src16 = (int16_t const *)writeWaveform.theWaveform;
                        dst16 = (int16_t *)monoData;
                        for (frame = 0; frame < writeWaveform.waveFrames; ++frame)
                        {
                            dst16[frame] = src16[frame * 2];
                        }
                    }
                    else
                    {
                        unsigned char const *src8;
                        unsigned char *dst8;
                        src8 = (unsigned char const *)writeWaveform.theWaveform;
                        dst8 = (unsigned char *)monoData;
                        for (frame = 0; frame < writeWaveform.waveFrames; ++frame)
                        {
                            dst8[frame] = src8[frame * 2];
                        }
                    }

                    writeWaveform.theWaveform = monoData;
                    writeWaveform.channels = 1;
                    writeWaveform.waveSize = (int32_t)monoBytes;
                    encodeWaveDataOwner = monoData;
                    debug_message("[RMF Save] Sample[%u] collapsed dual-mono PCM to mono for Opus encode\n",
                               (unsigned)index);
                }
            }
#endif

            /* MPEG encoders only support a fixed set of sample rates.
             * If the source rate isn't one of those (e.g. 8287 Hz from
             * a MOD file), LAME will silently downsample to the nearest
             * valid rate, causing a pitch shift.  Resample the PCM to
             * the codec-compatible rate first so content and metadata
             * stay in agreement. */
            if (sampleWasEncodedMpeg)
            {
                BAE_UNSIGNED_FIXED sourceRate;
                BAE_UNSIGNED_FIXED targetRate;

                sourceRate = PV_NormalizeSampleRateForSave((BAE_UNSIGNED_FIXED)writeWaveform.sampledRate);
                targetRate = PV_ChooseCodecRateFromSourceHz(sample->targetCompressionType,
                                                            (uint32_t)(sourceRate >> 16));
                if (targetRate == 0)
                {
                    targetRate = (44100U << 16);
                }
                if (sourceRate != targetRate)
                {
                    result = PV_ResampleWaveformLinear(&writeWaveform,
                                                       targetRate,
                                                       &encodeWaveDataOwner);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return result;
                    }
                    debug_message("[RMF Save] Sample[%u] MPEG resampled %uHz -> %uHz (%u -> %u frames)\n",
                               (unsigned)index,
                               (unsigned)(sourceRate >> 16),
                               (unsigned)(targetRate >> 16),
                               (unsigned)sample->waveform->waveFrames,
                               (unsigned)writeWaveform.waveFrames);
                }
            }

#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
            /*
             * Opus encoder accepts multiple source rates; choose a codec-aware
             * target rate (e.g. 24k for ~22k sources, 48k for high-rate sources)
             * to minimize storage while preserving expected playback behavior.
             * Round-trip resampling skips this \u2014 the source PCM is fed to the
             * encoder directly with the rate spoofed to 48000 (done below).
             */
            if (compType == C_OPUS && !sample->opusUseRoundTripResampling)
            {
                BAE_UNSIGNED_FIXED sourceRate;
                BAE_UNSIGNED_FIXED targetRate;

                sourceRate = PV_NormalizeSampleRateForSave((BAE_UNSIGNED_FIXED)writeWaveform.sampledRate);
                targetRate = PV_ChooseCodecRateFromSourceHz(sample->targetCompressionType,
                                                            (uint32_t)(sourceRate >> 16));
                if (targetRate == 0)
                {
                    targetRate = (48000U << 16);
                }
                if (sourceRate != targetRate)
                {
                    result = PV_ResampleWaveformLinear(&writeWaveform,
                                                       targetRate,
                                                       &encodeWaveDataOwner);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return result;
                    }
                    debug_message("[RMF Save] Sample[%u] resampled %uHz -> %uHz (%u -> %u frames)\n",
                               (unsigned)index,
                               (unsigned)(sourceRate >> 16),
                               (unsigned)(targetRate >> 16),
                               (unsigned)sample->waveform->waveFrames,
                               (unsigned)writeWaveform.waveFrames);
                }
            }
            /* Trim leading silence from Opus source PCM.  Many instrument
             * samples start with a handful of zero frames.  Opus's transform
             * codec smears the silence-to-audio transition, making the gap
             * audibly wider than the original.  Removing leading zeros before
             * encoding keeps the attack tight.  Loop points are shifted to
             * compensate so they stay aligned with the trimmed PCM. */
            if (compType == C_OPUS && !sample->opusUseRoundTripResampling && writeWaveform.theWaveform &&
                writeWaveform.waveFrames > 64 &&
                (writeWaveform.bitSize == 8 || writeWaveform.bitSize == 16))
            {
                uint32_t trimFrames = 0;

                if (writeWaveform.bitSize == 16)
                {
                    int16_t const *pcm16 = (int16_t const *)writeWaveform.theWaveform;
                    while (trimFrames < writeWaveform.waveFrames)
                    {
                        uint32_t ch;
                        bool allZero = TRUE;
                        for (ch = 0; ch < writeWaveform.channels; ++ch)
                        {
                            if (pcm16[(trimFrames * writeWaveform.channels) + ch] != 0)
                            {
                                allZero = FALSE;
                                break;
                            }
                        }
                        if (!allZero) break;
                        trimFrames++;
                    }
                }
                else
                {
                    unsigned char const *pcm8 = (unsigned char const *)writeWaveform.theWaveform;
                    while (trimFrames < writeWaveform.waveFrames)
                    {
                        uint32_t ch;
                        bool allZero = TRUE;
                        for (ch = 0; ch < writeWaveform.channels; ++ch)
                        {
                            if (pcm8[(trimFrames * writeWaveform.channels) + ch] != 128)
                            {
                                allZero = FALSE;
                                break;
                            }
                        }
                        if (!allZero) break;
                        trimFrames++;
                    }
                }

                if (trimFrames > 0 && trimFrames < writeWaveform.waveFrames - 32)
                {
                    uint32_t bytesPerFrame = (uint32_t)(writeWaveform.bitSize / 8) * writeWaveform.channels;
                    writeWaveform.theWaveform = (signed char *)writeWaveform.theWaveform + (trimFrames * bytesPerFrame);
                    writeWaveform.waveFrames -= trimFrames;
                    writeWaveform.waveSize = (int32_t)(writeWaveform.waveFrames * bytesPerFrame);

                    if (writeWaveform.startLoop >= trimFrames)
                        writeWaveform.startLoop -= trimFrames;
                    else
                        writeWaveform.startLoop = 0;
                    if (writeWaveform.endLoop >= trimFrames)
                        writeWaveform.endLoop -= trimFrames;
                    else
                        writeWaveform.endLoop = 0;
                    if (writeWaveform.startLoop >= writeWaveform.endLoop)
                    {
                        writeWaveform.startLoop = 0;
                        writeWaveform.endLoop = 0;
                    }

                    debug_message("[RMF Save] Sample[%u] trimmed %u leading silence frames for Opus\n",
                               (unsigned)index, (unsigned)trimFrames);
                }
            }

            /* Round-trip: spoof the encoder input rate to 48000 so the PCM is
             * stored as a sped-up bitstream.  The real source rate is preserved
             * in the SND header and corrected after XCreateSoundObjectFromData. */
            if (compType == C_OPUS && sample->opusUseRoundTripResampling)
            {
                roundTripSourceRate = writeWaveform.sampledRate;
                writeWaveform.sampledRate = (int32_t)(48000u << 16);
                debug_message("[RMF Save] Sample[%u] round-trip: spoofing encoder rate %uHz -> 48000Hz\n",
                           (unsigned)index, (unsigned)(roundTripSourceRate >> 16));
            }

            if (compType == C_OPUS && !sample->opusUseRoundTripResampling)
            {
                prePadWaveFrames = writeWaveform.waveFrames;
                result = PV_ApplyOpusLoopSeamMicroFade(&writeWaveform,
                                                       &encodeWaveDataOwner,
                                                       (uint32_t)index);
                if (result != BAE_NO_ERROR)
                {
                    XDisposePtr((XPTR)sampleSndIDs);
                    XDisposePtr((XPTR)sampleInstIDs);
                    return result;
                }
            }
#endif

            opErr = XCreateSoundObjectFromData(&sndResource,
                                               &writeWaveform,
                                               compType,
                                               encodeCompSubType,
                                               NULL,
                                               NULL);
            debug_message("[RMF Save] Sample[%u] XCreateSoundObjectFromData compType=%d opErr=%d sndResource=%p\n",
                       (unsigned)index, (int)compType, (int)opErr, (void *)sndResource);
            if (encodeWaveDataOwner)
            {
                XDisposePtr(encodeWaveDataOwner);
                encodeWaveDataOwner = NULL;
            }
            if (opErr != NO_ERR || !sndResource)
            {
                XDisposePtr((XPTR)sampleSndIDs);
                XDisposePtr((XPTR)sampleInstIDs);
                return BAE_BAD_FILE;
            }
            PV_StoreCompressionSubTypeInSnd(sndResource,
                                            XGetPtrSize(sndResource),
                                            compType,
                                            compSubType);

            {
                uint32_t encodedFrames;
                SampleDataInfo decodedInfo;
                XPTR decodedOwner;

                XSetMemory(&decodedInfo, (int32_t)sizeof(decodedInfo), 0);
                decodedOwner = NULL;
                (void)XGetSamplePtrFromSnd(sndResource, &decodedInfo);

                encodedFrames = decodedInfo.frames;
                if (encodedFrames == 0)
                {
                    encodedFrames = writeWaveform.waveFrames;
                }

                if (decodedInfo.rate != 0)
                {
                    decodedSampleRateForSnd = (uint32_t)decodedInfo.rate;
                }
                loopStart = (int32_t)writeWaveform.startLoop;
                loopEnd = (int32_t)writeWaveform.endLoop;

#if USE_OPUS_ENCODER == TRUE && USE_OPUS_DECODER == TRUE
                if (compType == C_OPUS && sample->opusUseRoundTripResampling)
                {
                    /* RT mode: Opus pre-skip can make the decoded frame count
                     * slightly shorter than the source.  Shift both loop
                     * points uniformly by that difference so they stay
                     * aligned with the decoded PCM while preserving the
                     * exact loop length (no detuning). */
                    int32_t shift;

                    decodedFramesForRate = encodedFrames;
                    PV_ForceSndDecodedFrameCount(sndResource, encodedFrames);
                    shift = (int32_t)writeWaveform.waveFrames - (int32_t)encodedFrames;
                    if (shift > 0)
                    {
                        if (loopStart >= shift)
                        {
                            loopStart -= shift;
                        }
                        else
                        {
                            loopStart = 0;
                        }
                        loopEnd -= shift;
                        if (loopEnd < 0)
                        {
                            loopEnd = 0;
                        }
                    }
                    if (loopEnd > (int32_t)encodedFrames)
                    {
                        loopEnd = (int32_t)encodedFrames;
                    }
                    if (loopStart >= loopEnd)
                    {
                        loopStart = 0;
                        loopEnd = 0;
                    }
                    writeWaveform.startLoop = (uint32_t)loopStart;
                    writeWaveform.endLoop = (uint32_t)loopEnd;
                    debug_message("[RMF Save] Sample[%u] Opus RT: srcFrames=%u encFrames=%u shift=%d loop %u-%u\n",
                               (unsigned)index,
                               (unsigned)writeWaveform.waveFrames,
                               (unsigned)encodedFrames,
                               (int)shift,
                               (unsigned)writeWaveform.startLoop,
                               (unsigned)writeWaveform.endLoop);
                }
                else
#endif
                {
                    decodedFramesForRate = encodedFrames;
                    PV_ForceSndDecodedFrameCount(sndResource, encodedFrames);
                    /* IMA4 encodes in fixed 64-frame blocks; the decoded frame
                     * count rounds up to a block multiple.  The extra frames
                     * are zero-padded silence at the tail.  Remapping loop
                     * points into that enlarged domain would stretch the loop
                     * region to include the padding, lowering the perceived
                     * pitch.  Skip the remap for IMA4 - the original loop
                     * points are always valid within the decoded buffer. */
                    if (compType != C_IMA4)
                    {
                        uint32_t remapSourceFrames = writeWaveform.waveFrames;
                        /* If the waveform was padded for Opus loop context,
                         * use the pre-pad frame count for proportional remap
                         * so loop points map correctly without the padding
                         * skewing the ratio. */
                        if (prePadWaveFrames > 0 && prePadWaveFrames < remapSourceFrames)
                        {
                            remapSourceFrames = prePadWaveFrames;
                        }
                        PV_RemapLoopPointsToFrameCount(remapSourceFrames,
                                                       encodedFrames,
                                                       &loopStart,
                                                       &loopEnd);
                    }
                    writeWaveform.startLoop = (uint32_t)loopStart;
                    writeWaveform.endLoop = (uint32_t)loopEnd;
                    debug_message("[RMF Save] Sample[%u] loop remap srcFrames=%u encFrames=%u -> %u-%u\n",
                               (unsigned)index,
                               (unsigned)writeWaveform.waveFrames,
                               (unsigned)encodedFrames,
                               (unsigned)writeWaveform.startLoop,
                               (unsigned)writeWaveform.endLoop);
                }

                if (decodedInfo.pMasterPtr && decodedInfo.pMasterPtr != sndResource)
                {
                    decodedOwner = decodedInfo.pMasterPtr;
                }
                if (decodedOwner)
                {
                    XDisposePtr(decodedOwner);
                }
            }
        }
        if (sndResource)
        {
            int32_t sndSampleRate;

            sndSampleRate = writeWaveform.sampledRate;
            if (sampleWasEncodedMpeg && decodedSampleRateForSnd != 0)
            {
                sndSampleRate = (int32_t)decodedSampleRateForSnd;
                debug_message("[RMF Save] Sample[%u] MPEG rate align using decoded stream rate %uHz\n",
                           (unsigned)index,
                           (unsigned)(decodedSampleRateForSnd >> 16));
            }
            XSetSoundBaseKey(sndResource, sample->rootKey);
            XSetSoundSampleRate(sndResource, sndSampleRate);
            XSetSoundLoopPoints(sndResource, (int32_t)writeWaveform.startLoop, (int32_t)writeWaveform.endLoop);
            PV_ForceSndLoopPoints(sndResource, (int32_t)writeWaveform.startLoop, (int32_t)writeWaveform.endLoop);
#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
            if (roundTripSourceRate != 0)
            {
                int32_t roundTripWriteRate;

                roundTripWriteRate = roundTripSourceRate;
                /* Override the spoofed 48000 rate with the true source rate so
                 * the engine can time-stretch correctly on decode. */
                XSetSoundSampleRate(sndResource, roundTripWriteRate);
                XSetSoundOpusRoundTripFlag(sndResource, TRUE);
                debug_message("[RMF Save] Sample[%u] round-trip: SND rate fixed to %uHz + XSOUND_OPUS_ROUNDTRIP_RESAMPLE set\n",
                           (unsigned)index, (unsigned)(((uint32_t)roundTripWriteRate) >> 16));
            }
#endif
            if (isZmf && sampleAdvancedInterpolation && writeWaveform.endLoop > writeWaveform.startLoop)
            {
                XSetSoundAdvancedInterpolationFlag(sndResource, TRUE);
            }
        }
        if (sndResource)
        {
            XSetSoundEmbeddedStatus(sndResource, TRUE);
        }
        writeSndType = sample->originalSndResourceType;
        if (writeSndType != ID_ESND && writeSndType != ID_CSND && writeSndType != ID_SND)
        {
            writeSndType = ID_ESND;
        }
        switch (writeSndType)
        {
            case ID_ESND:
                XEncryptData(sndResource, (uint32_t)XGetPtrSize(sndResource));
                break;
            case ID_CSND:
                {
                    XPTR compressedSnd;
                    int32_t compressedSize;
                    XCOMPRESSION_TYPE csndCompType;

#if USE_LZMA_COMPRESSION == TRUE
                    csndCompType = isZmf ? X_LZMA_RAW : X_RAW;
#else
                    (void)isZmf;
                    csndCompType = X_RAW;
#endif
                    compressedSnd = NULL;
                    compressedSize = XCompressPtr(&compressedSnd,
                                                  sndResource,
                                                  (uint32_t)XGetPtrSize(sndResource),
                                                  csndCompType,
                                                  NULL,
                                                  NULL);
                    if (compressedSize <= 0 || !compressedSnd)
                    {
                        XDisposePtr(sndResource);
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return BAE_BAD_FILE;
                    }
                    XDisposePtr(sndResource);
                    sndResource = compressedSnd;
                }
                break;
            case ID_SND:
                /* plain – no transformation needed */
                break;
            default:
                writeSndType = ID_SND;
                break;
        }
        {
            unsigned char *dbgBytes = (unsigned char *)sndResource;
            int32_t dbgSize = XGetPtrSize(sndResource);
            int16_t dbgFmt = (dbgSize >= 2) ? (int16_t)XGetShort(dbgBytes) : -1;
            debug_message("[RMF Save] SND id=%ld fmt=%d size=%ld first8=",
                       (long)sndID, (int)dbgFmt, (long)dbgSize);
            if (dbgSize >= 8)
            {
                debug_message("%02x %02x %02x %02x %02x %02x %02x %02x",
                           dbgBytes[0], dbgBytes[1], dbgBytes[2], dbgBytes[3],
                           dbgBytes[4], dbgBytes[5], dbgBytes[6], dbgBytes[7]);
            }
            debug_message("\n");
        }
        if (XAddFileResource(fileRef, writeSndType, sndID, pascalName, sndResource, XGetPtrSize(sndResource)) != 0)
        {
            XDisposePtr(sndResource);
            XDisposePtr((XPTR)sampleSndIDs);
            XDisposePtr((XPTR)sampleInstIDs);
            return BAE_FILE_IO_ERROR;
        }
        XDisposePtr(sndResource);
        sampleSndIDs[index] = (XShortResourceID)sndID;
        sampleInstIDs[index] = (sample->instID != BAE_EDITOR_INST_ID_NONE)
                                ? (XLongResourceID)sample->instID
                                : (XLongResourceID)(512 + (uint32_t)sample->program);
    }

    for (index = 0; index < document->sampleCount; ++index)
    {
        uint32_t prior;
        uint32_t splitCount;
        uint32_t sampleIndex;
        uint32_t leaderIndex;
        uint32_t leaderFrames;
        XLongResourceID instID;
        BAERmfEditorSample const *leaderSample;
        BAERmfEditorInstrumentExt const *extForInst;
        char pascalName[256];
        BAEResult result;

        instID = sampleInstIDs[index];
        for (prior = 0; prior < index; ++prior)
        {
            if (sampleInstIDs[prior] == instID)
            {
                break;
            }
        }
        if (prior < index)
        {
            continue;
        }

        splitCount = 0;
        leaderIndex = index;
        leaderFrames = 0;
        for (sampleIndex = 0; sampleIndex < document->sampleCount; ++sampleIndex)
        {
            if (sampleInstIDs[sampleIndex] == instID)
            {
                BAERmfEditorSample const *candidate;
                uint32_t frames;

                splitCount++;
                candidate = &document->samples[sampleIndex];
                frames = 0;
                if (candidate->waveform)
                {
                    frames = candidate->waveform->waveFrames;
                }
                else
                {
                    frames = candidate->sampleInfo.waveFrames;
                }
                if (frames >= leaderFrames)
                {
                    leaderFrames = frames;
                    leaderIndex = sampleIndex;
                }
            }
        }
        if (splitCount == 0)
        {
            continue;
        }

        leaderSample = &document->samples[leaderIndex];
    extForInst = PV_FindInstrumentExt((BAERmfEditorDocument *)document, instID);
        result = PV_CreatePascalName((extForInst && extForInst->displayName && extForInst->displayName[0])
                                        ? extForInst->displayName
                                        : (leaderSample->displayName ? leaderSample->displayName : leaderSample->sourcePath),
                                     pascalName);
        if (result != BAE_NO_ERROR)
        {
            XDisposePtr((XPTR)sampleSndIDs);
            XDisposePtr((XPTR)sampleInstIDs);
            return result;
        }

        {
            InstrumentResource instrument;

            /* Unmodified bank clones: write the original INST bytes with only SND
             * IDs remapped. Rebuilding a single-split INST via XNewInstrumentResource
             * places tremoloEnd=0x8000 before the appended unit block; PV_GetEnvelopeData
             * then latches onto that marker and never finds ADSR/PITC/LPF — Syn Drum
             * and similar instruments play as the raw sample ("tom 2"). */
            if (extForInst &&
                !extForInst->dirty &&
                extForInst->originalInstData &&
                extForInst->originalInstSize >= 14)
            {
                enum
                {
                    kInstOffset_sndResourceID = 0,
                    kInstOffset_keySplitCount = 12,
                    kInstOffset_keySplitData = 14,
                    kKeySplitFileSize = 8
                };
                unsigned char *instBytes;
                int32_t instSize;
                uint16_t origSplitCount;
                uint32_t *orderedSampleIndices;
                uint32_t orderedCount;
                uint32_t si;
                uint32_t splitIdx;
                XShortResourceID newHeaderSnd;

                instSize = extForInst->originalInstSize;
                origSplitCount = (uint16_t)XGetShort((unsigned char const *)extForInst->originalInstData +
                                                     kInstOffset_keySplitCount);
                orderedSampleIndices = (uint32_t *)XNewPtr((int32_t)(splitCount * sizeof(uint32_t)));
                if (!orderedSampleIndices)
                {
                    XDisposePtr((XPTR)sampleSndIDs);
                    XDisposePtr((XPTR)sampleInstIDs);
                    return BAE_MEMORY_ERR;
                }
                orderedCount = 0;
                for (si = 0; si < document->sampleCount; ++si)
                {
                    if (sampleInstIDs[si] == instID)
                    {
                        orderedSampleIndices[orderedCount++] = si;
                    }
                }

                /* Need one remapped SND per original split (or the non-split header). */
                if (orderedCount == 0 ||
                    (origSplitCount > 0 && orderedCount != (uint32_t)origSplitCount) ||
                    (origSplitCount == 0 && orderedCount < 1) ||
                    instSize < (int32_t)(kInstOffset_keySplitData +
                                        ((int32_t)origSplitCount * kKeySplitFileSize) + 10))
                {
                    XDisposePtr((XPTR)orderedSampleIndices);
                    /* Fall through to rebuild paths below. */
                }
                else
                {
                    instBytes = (unsigned char *)XNewPtr(instSize);
                    if (!instBytes)
                    {
                        XDisposePtr((XPTR)orderedSampleIndices);
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return BAE_MEMORY_ERR;
                    }
                    XBlockMove(extForInst->originalInstData, instBytes, instSize);

                    newHeaderSnd = sampleSndIDs[orderedSampleIndices[0]];
                    XPutShort(instBytes + kInstOffset_sndResourceID, (uint16_t)newHeaderSnd);
                    for (splitIdx = 0; splitIdx < (uint32_t)origSplitCount; ++splitIdx)
                    {
                        unsigned char *splitPtr;
                        XShortResourceID newSnd;

                        newSnd = sampleSndIDs[orderedSampleIndices[splitIdx]];
                        splitPtr = instBytes + kInstOffset_keySplitData +
                                   ((int32_t)splitIdx * kKeySplitFileSize);
                        XPutShort(splitPtr + 2, (uint16_t)newSnd);
                    }

                    debug_message("[RMF Save] INST id=%ld verbatim original (%ld bytes) splits=%u\n",
                                  (long)instID, (long)instSize, (unsigned)origSplitCount);
                    if (XAddFileResource(fileRef, ID_INST, instID, pascalName, instBytes, instSize) != 0)
                    {
                        XDisposePtr((XPTR)instBytes);
                        XDisposePtr((XPTR)orderedSampleIndices);
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return BAE_FILE_IO_ERROR;
                    }
                    XDisposePtr((XPTR)instBytes);
                    XDisposePtr((XPTR)orderedSampleIndices);
                    continue;
                }
            }

            if (splitCount > 1)
            {
                enum
                {
                    kInstOffset_sndResourceID = 0,
                    kInstOffset_midiRootKey = 2,
                    kInstOffset_panPlacement = 4,
                    kInstOffset_flags1 = 5,
                    kInstOffset_flags2 = 6,
                    kInstOffset_smodResourceID = 7,
                    kInstOffset_miscParameter1 = 8,
                    kInstOffset_miscParameter2 = 10,
                    kInstOffset_keySplitCount = 12,
                    kInstOffset_keySplitData = 14,
                    kKeySplitFileSize = 8,
                    kInstTailSize = 10
                };
                int32_t instSize;
                unsigned char *instBytes;
                int32_t tailOffset;
                uint32_t *instSampleIndices;
                uint32_t collected;
                uint32_t i;
                unsigned char writeFlags1;
                unsigned char writeFlags2;
                int16_t headerMiscParam1;
                int16_t headerMiscParam2;

                instSampleIndices = (uint32_t *)XNewPtr((int32_t)(splitCount * sizeof(uint32_t)));
                if (!instSampleIndices)
                {
                    XDisposePtr((XPTR)sampleSndIDs);
                    XDisposePtr((XPTR)sampleInstIDs);
                    return BAE_MEMORY_ERR;
                }

                collected = 0;
                for (sampleIndex = 0; sampleIndex < document->sampleCount; ++sampleIndex)
                {
                    if (sampleInstIDs[sampleIndex] == instID)
                    {
                        instSampleIndices[collected++] = sampleIndex;
                    }
                }

                /* Keep split matching deterministic for engine key-range lookup. */
                for (i = 0; i + 1 < collected; ++i)
                {
                    uint32_t j;
                    for (j = i + 1; j < collected; ++j)
                    {
                        BAERmfEditorSample const *a;
                        BAERmfEditorSample const *b;
                        if (instSampleIndices[i] == instSampleIndices[j])
                        {
                            continue;
                        }
                        a = &document->samples[instSampleIndices[i]];
                        b = &document->samples[instSampleIndices[j]];
                        if (b->lowKey < a->lowKey ||
                            (b->lowKey == a->lowKey && b->highKey < a->highKey))
                        {
                            uint32_t t;
                            t = instSampleIndices[i];
                            instSampleIndices[i] = instSampleIndices[j];
                            instSampleIndices[j] = t;
                        }
                    }
                }

                instSize = (int32_t)(kInstOffset_keySplitData + (int32_t)(collected * kKeySplitFileSize) + kInstTailSize);

                /* Build flags: preserve original flags and OR in required bits */
                writeFlags1 = ZBF_useSampleRate;
                writeFlags2 = ZBF_useSoundModifierAsRootKey;
                if (extForInst)
                {
                    writeFlags1 = extForInst->flags1;
                    writeFlags2 = extForInst->flags2;
                }
                /* Opus decodes to 48 kHz which differs from the engine's 22050 Hz
                 * base rate.  The engine must factor in the SND sample rate when
                 * computing pitch, otherwise the resampled data plays too slowly.
                 * Force ZBF_useSampleRate for any instrument that contains an
                 * Opus-compressed split. */
                {
                    uint32_t si;
                    for (si = 0; si < document->sampleCount; ++si)
                    {
                        if (sampleInstIDs[si] == instID &&
                            PV_IsOpusCompression(document->samples[si].targetCompressionType))
                        {
                            writeFlags1 |= ZBF_useSampleRate;
                            break;
                        }
                    }
                }

                headerMiscParam1 = 0;
                headerMiscParam2 = 0;
                if (extForInst && extForInst->originalInstData && extForInst->originalInstSize >= 12)
                {
                    unsigned char const *origBytes = (unsigned char const *)extForInst->originalInstData;
                    headerMiscParam1 = (int16_t)XGetShort((void *)(origBytes + kInstOffset_miscParameter1));
                    headerMiscParam2 = (int16_t)XGetShort((void *)(origBytes + kInstOffset_miscParameter2));
                }

                /* Check if we need to append extended format data */
                {
                    XPTR extTail = NULL;
                    int32_t extTailSize = 0;
                    if (extForInst && (extForInst->hasExtendedData || extForInst->dirty))
                    {
                        if (!extForInst->dirty && extForInst->originalInstData && extForInst->originalInstSize > 0)
                        {
                            if (PV_CopyOriginalInstExtendedTail(extForInst, &extTail, &extTailSize) != BAE_NO_ERROR)
                            {
                                extTail = NULL;
                                extTailSize = 0;
                            }
                        }
                        if (!extTail)
                        {
                            extTail = PV_SerializeExtendedInstTail(extForInst, &extTailSize);
                        }
                        if (extTail && extTailSize > 0)
                        {
                            writeFlags1 |= ZBF_extendedFormat;
                        }
                    }

                    instBytes = (unsigned char *)XNewPtr(instSize + extTailSize);
                    if (!instBytes)
                    {
                        if (extTail) XDisposePtr(extTail);
                        XDisposePtr((XPTR)instSampleIndices);
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return BAE_MEMORY_ERR;
                    }
                    XSetMemory(instBytes, instSize + extTailSize, 0);

                    /* Use the first (lowest-key) split as the INST header sndResourceID.
                     * This matches the original file's convention and is more deterministic
                     * than using the largest-frame split. The header value is stored in
                     * defaultInstrumentID but is not used for split playback. */
                    XPutShort(instBytes + kInstOffset_sndResourceID, (uint16_t)sampleSndIDs[instSampleIndices[0]]);
                    XPutShort(instBytes + kInstOffset_midiRootKey, extForInst ? extForInst->midiRootKey : 60);
                    instBytes[kInstOffset_panPlacement] = extForInst ? (unsigned char)extForInst->panPlacement : 0;
                    instBytes[kInstOffset_flags1] = writeFlags1;
                    instBytes[kInstOffset_flags2] = writeFlags2;
                    instBytes[kInstOffset_smodResourceID] = 0;
                    /* Preserve header misc parameters from the original INST when present.
                     * Some files use these fields for sound-modifier defaults. */
                    XPutShort(instBytes + kInstOffset_miscParameter1, (uint16_t)headerMiscParam1);
                    XPutShort(instBytes + kInstOffset_miscParameter2, (uint16_t)headerMiscParam2);
                    XPutShort(instBytes + kInstOffset_keySplitCount, (uint16_t)collected);

                    for (i = 0; i < collected; ++i)
                    {
                        BAERmfEditorSample const *splitSample;
                        unsigned char *splitPtr;

                        splitSample = &document->samples[instSampleIndices[i]];
                        splitPtr = instBytes + kInstOffset_keySplitData + (i * kKeySplitFileSize);
                        splitPtr[0] = (unsigned char)splitSample->lowKey;
                        splitPtr[1] = (unsigned char)splitSample->highKey;
                        XPutShort(splitPtr + 2, (uint16_t)sampleSndIDs[instSampleIndices[i]]);
                        XPutShort(splitPtr + 4, (uint16_t)splitSample->rootKey);
                        XPutShort(splitPtr + 6, splitSample->splitVolume ? (uint16_t)splitSample->splitVolume : 100);
                    }

                    tailOffset = (int32_t)(kInstOffset_keySplitData + (int32_t)(collected * kKeySplitFileSize));
                    XPutShort(instBytes + tailOffset + 0, 0);      /* tremoloCount */
                    XPutShort(instBytes + tailOffset + 2, 0x8000); /* tremoloEnd */
                    XPutShort(instBytes + tailOffset + 4, 0);      /* reserved_3 */
                    XPutShort(instBytes + tailOffset + 6, 0);      /* descriptorName */
                    XPutShort(instBytes + tailOffset + 8, 0);      /* descriptorFlags */

                    /* Append extended data tail if present */
                    if (extTail && extTailSize > 0)
                    {
                        XBlockMove(extTail, instBytes + instSize, extTailSize);
                        instSize += extTailSize;
                    }
                    if (extTail) XDisposePtr(extTail);
                }

                debug_message("[RMF Save] INST id=%ld splitCount=%u using split map leaderSample=%u leaderFrames=%u\n",
                           (long)instID,
                           (unsigned)collected,
                           (unsigned)leaderIndex,
                           (unsigned)leaderFrames);
                if (XAddFileResource(fileRef, ID_INST, instID, pascalName, instBytes, instSize) != 0)
                {
                    XDisposePtr((XPTR)instBytes);
                    XDisposePtr((XPTR)instSampleIndices);
                    XDisposePtr((XPTR)sampleSndIDs);
                    XDisposePtr((XPTR)sampleInstIDs);
                    return BAE_FILE_IO_ERROR;
                }

                XDisposePtr((XPTR)instBytes);
                XDisposePtr((XPTR)instSampleIndices);
                continue;
            }

            /* Single-sample (no key splits) path */
            {
                unsigned char writeFlags1;
                unsigned char writeFlags2;
                int16_t headerMiscParam1;
                int16_t headerMiscParam2;
                XPTR extTail = NULL;
                int32_t extTailSize = 0;

                writeFlags1 = ZBF_useSampleRate;
                writeFlags2 = ZBF_useSoundModifierAsRootKey;
                if (extForInst)
                {
                    writeFlags1 = extForInst->flags1;
                    writeFlags2 = extForInst->flags2;
                }
                /* Force ZBF_useSampleRate for Opus - see multi-split comment above. */
                if (PV_IsOpusCompression(leaderSample->targetCompressionType))
                {
                    writeFlags1 |= ZBF_useSampleRate;
                }

                if (extForInst && extForInst->originalInstData && extForInst->originalInstSize >= 12)
                {
                    unsigned char const *origBytes = (unsigned char const *)extForInst->originalInstData;
                    headerMiscParam1 = (int16_t)XGetShort((void *)(origBytes + 8));
                    headerMiscParam2 = (int16_t)XGetShort((void *)(origBytes + 10));
                }
                else
                {
                    headerMiscParam1 = (int16_t)leaderSample->rootKey;
                    headerMiscParam2 = leaderSample->splitVolume ? (int16_t)leaderSample->splitVolume : 100;
                }

                /* Only override header misc params when the instrument is explicitly
                 * using miscParameter1/2 for sample-offset-start metadata. For normal
                 * instruments, keep legacy root-key / split-volume behavior. */
                if (extForInst && extForInst->dirty &&
                    TEST_FLAG_VALUE(writeFlags2, ZBF_enableSampleOffsetStart))
                {
                    headerMiscParam1 = extForInst->miscParameter1;
                    headerMiscParam2 = extForInst->miscParameter2;
                }

                if (extForInst && TEST_FLAG_VALUE(writeFlags2, ZBF_useSoundModifierAsRootKey) &&
                    !TEST_FLAG_VALUE(writeFlags2, ZBF_enableSampleOffsetStart))
                {
                    /* If the instrument declares miscParameter1 as root key (and is NOT
                     * using it for sample offset), keep it aligned to the edited root. */
                    headerMiscParam1 = (int16_t)leaderSample->rootKey;
                }
                if (extForInst && (extForInst->hasExtendedData || extForInst->dirty))
                {
                    if (!extForInst->dirty && extForInst->originalInstData && extForInst->originalInstSize > 0)
                    {
                        if (PV_CopyOriginalInstExtendedTail(extForInst, &extTail, &extTailSize) != BAE_NO_ERROR)
                        {
                            extTail = NULL;
                            extTailSize = 0;
                        }
                    }
                    if (!extTail)
                    {
                        extTail = PV_SerializeExtendedInstTail(extForInst, &extTailSize);
                    }
                    if (extTail && extTailSize > 0)
                    {
                        writeFlags1 |= ZBF_extendedFormat;
                    }
                }

                {
                    InstrumentResource *templateInst =
                        XNewInstrumentResource((XShortResourceID)sampleSndIDs[leaderIndex]);
                    if (!templateInst)
                    {
                        if (extTail)
                        {
                            XDisposePtr(extTail);
                        }
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return BAE_MEMORY_ERR;
                    }
                    XBlockMove(templateInst, &instrument, (int32_t)sizeof(instrument));
                    XDisposeInstrumentResource(templateInst);
                }
                XPutShort(&instrument.midiRootKey, extForInst ? extForInst->midiRootKey : 60);
                instrument.panPlacement = extForInst ? extForInst->panPlacement : 0;
                instrument.flags1 = writeFlags1;
                instrument.flags2 = writeFlags2;
                XPutShort(&instrument.miscParameter1, (uint16_t)headerMiscParam1);
                XPutShort(&instrument.miscParameter2, (uint16_t)headerMiscParam2);

                if (extTail && extTailSize > 0)
                {
                    /* Must write as byte buffer to append extended tail */
                    int32_t totalSize = (int32_t)sizeof(instrument) + extTailSize;
                    unsigned char *instBuf = (unsigned char *)XNewPtr(totalSize);
                    if (!instBuf)
                    {
                        XDisposePtr(extTail);
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return BAE_MEMORY_ERR;
                    }
                    XBlockMove(&instrument, instBuf, (int32_t)sizeof(instrument));
                    XBlockMove(extTail, instBuf + sizeof(instrument), extTailSize);
                    XDisposePtr(extTail);
                    debug_message("[RMF Save] INST id=%ld midiRootKey=%d sampleRootKey=%u (extended, %ld bytes)\n",
                               (long)instID, 60, (unsigned)leaderSample->rootKey, (long)totalSize);
                    if (XAddFileResource(fileRef, ID_INST, instID, pascalName, instBuf, totalSize) != 0)
                    {
                        XDisposePtr((XPTR)instBuf);
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return BAE_FILE_IO_ERROR;
                    }
                    XDisposePtr((XPTR)instBuf);
                }
                else
                {
                    if (extTail) XDisposePtr(extTail);
                    debug_message("[RMF Save] INST id=%ld fallback midiRootKey=%d sampleRootKey=%u\n",
                               (long)instID,
                               60,
                               (unsigned)leaderSample->rootKey);
                    if (XAddFileResource(fileRef, ID_INST, instID, pascalName, &instrument, (int32_t)sizeof(instrument)) != 0)
                    {
                        XDisposePtr((XPTR)sampleSndIDs);
                        XDisposePtr((XPTR)sampleInstIDs);
                        return BAE_FILE_IO_ERROR;
                    }
                }
            }
        }
    }

    XDisposePtr((XPTR)sampleSndIDs);
    XDisposePtr((XPTR)sampleInstIDs);
#if USE_ZMF_SUPPORT == TRUE
    {
        BAEResult oscResult = PV_AddSampleFreeInstrumentResources(document, fileRef);
        if (oscResult != BAE_NO_ERROR)
        {
            return oscResult;
        }
    }
#endif
    return BAE_NO_ERROR;
}


BAEResult PV_AddSongResource(BAERmfEditorDocument *document, XFILE fileRef, XLongResourceID midiResourceID)
{
    SongResource_Info *songInfo;
    SongResource *songResource;
    XLongResourceID songID;
    char pascalName[256];
    BAEResult result;

    debug_message("[RMF Save] PV_AddSongResource entered midiID=%ld\n", (long)midiResourceID);
    songInfo = XNewSongResourceInfo();
    if (!songInfo)
    {
        return BAE_MEMORY_ERR;
    }
    result = PV_PopulateSongResourceInfoFromDocument(document, songInfo, midiResourceID);
    if (result != BAE_NO_ERROR)
    {
        XDisposeSongResourceInfo(songInfo);
        return result;
    }
    songResource = XNewSongFromSongResourceInfo(songInfo);
    XDisposeSongResourceInfo(songInfo);
    if (!songResource)
    {
        return BAE_MEMORY_ERR;
    }
    if (PV_GetAvailableResourceID(fileRef, ID_SONG, 1, &songID) != BAE_NO_ERROR)
    {
        XDisposeSongPtr(songResource);
        return BAE_FILE_IO_ERROR;
    }
    PV_CreatePascalName(document->info[TITLE_INFO] ? document->info[TITLE_INFO] : "Untitled RMF", pascalName);
    if (XAddFileResource(fileRef, ID_SONG, songID, pascalName, songResource, XGetPtrSize(songResource)) != 0)
    {
        XDisposeSongPtr(songResource);
        return BAE_FILE_IO_ERROR;
    }
    XDisposeSongPtr(songResource);
    return BAE_NO_ERROR;
}


BAEResult PV_AddSongResourceWithID(BAERmfEditorDocument *document,
                                          XFILE fileRef,
                                          XLongResourceID midiResourceID,
                                          XLongResourceID songID,
                                          unsigned char const *pascalName)
{
    BAERmfEditorResourceEntry const *originalSongEntry;
    uint32_t resourceIndex;
    SongResource_Info *songInfo;
    SongResource *songResource;
    BAEResult result;
    char fallbackPascalName[256];
    unsigned char const *songName;

    originalSongEntry = NULL;
    if (document && document->loadedFromRmf && document->originalResourceCount > 0)
    {
        for (resourceIndex = 0; resourceIndex < document->originalResourceCount; ++resourceIndex)
        {
            BAERmfEditorResourceEntry const *entry;

            entry = &document->originalResources[resourceIndex];
            if (entry->type != ID_SONG || !entry->data || entry->size < (int32_t)sizeof(XShortResourceID))
            {
                continue;
            }
            if (document->originalSongID != 0 && entry->id != document->originalSongID)
            {
                continue;
            }
            originalSongEntry = entry;
            break;
        }
        if (!originalSongEntry)
        {
            for (resourceIndex = 0; resourceIndex < document->originalResourceCount; ++resourceIndex)
            {
                BAERmfEditorResourceEntry const *entry;

                entry = &document->originalResources[resourceIndex];
                if (entry->type == ID_SONG && entry->data && entry->size >= (int32_t)sizeof(XShortResourceID))
                {
                    originalSongEntry = entry;
                    break;
                }
            }
        }
    }

    songInfo = XNewSongResourceInfo();
    if (!songInfo)
    {
        return BAE_MEMORY_ERR;
    }
    result = PV_PopulateSongResourceInfoFromDocument(document, songInfo, midiResourceID);
    if (result != BAE_NO_ERROR)
    {
        XDisposeSongResourceInfo(songInfo);
        return result;
    }
    songResource = XNewSongFromSongResourceInfo(songInfo);
    XDisposeSongResourceInfo(songInfo);
    if (!songResource)
    {
        return BAE_MEMORY_ERR;
    }

    if (pascalName && pascalName[0])
    {
        songName = pascalName;
    }
    else if (originalSongEntry && originalSongEntry->pascalName[0])
    {
        songName = originalSongEntry->pascalName;
    }
    else
    {
        PV_CreatePascalName(document->info[TITLE_INFO] ? document->info[TITLE_INFO] : "Untitled RMF", fallbackPascalName);
        songName = (unsigned char const *)fallbackPascalName;
    }

    if (XAddFileResource(fileRef, ID_SONG, songID, songName, songResource, XGetPtrSize(songResource)) != 0)
    {
        XDisposeSongPtr(songResource);
        return BAE_FILE_IO_ERROR;
    }
    XDisposeSongPtr(songResource);
    return BAE_NO_ERROR;
}


BAEResult PV_VerifyEmbeddedOnlyInstrumentReferences(BAERmfEditorDocument const *document,
                                                           uint16_t embeddedBank)
{
    (void)document;
    (void)embeddedBank;
    return BAE_NO_ERROR;
}


BAEResult PV_VerifyClonedInstrument(BAERmfEditorDocument const *document,
                                           BAEBankToken bankToken,
                                           uint32_t instrumentIndex,
                                           uint32_t targetInstID,
                                           uint32_t firstSampleIndex,
                                           uint32_t *outSampleCount)
{
    uint32_t sourceSampleCount;
    uint32_t sampleOffset;
    uint32_t loadedSampleCount;

    sourceSampleCount = 0;
    if (BAERmfEditorBank_GetInstrumentSampleCount(bankToken,
                                                   instrumentIndex,
                                                   &sourceSampleCount) != BAE_NO_ERROR ||
        sourceSampleCount == 0)
    {
        return BAE_BAD_FILE;
    }

    loadedSampleCount = (document->sampleCount >= firstSampleIndex)
                            ? (document->sampleCount - firstSampleIndex)
                            : 0;

#if USE_ZMF_SUPPORT == TRUE
    /* Oscillator-only instruments clear SND to 0xFFFF and clone with no samples. */
    if (loadedSampleCount == 0)
    {
        BAERmfEditorInstrumentExtInfo extInfo;
        BAERmfEditorInstrumentExt const *docExt;
        uint32_t noSampleSlots;
        uint32_t i;

        XSetMemory(&extInfo, (int32_t)sizeof(extInfo), 0);
        if (BAERmfEditorBank_GetInstrumentExtInfo(bankToken, instrumentIndex, &extInfo) != BAE_NO_ERROR ||
            !extInfo.useOscillator)
        {
            return BAE_BAD_FILE;
        }
        noSampleSlots = 0;
        for (i = 0; i < sourceSampleCount; ++i)
        {
            BAERmfEditorBankSampleInfo sourceSample;
            XSetMemory(&sourceSample, (int32_t)sizeof(sourceSample), 0);
            if (BAERmfEditorBank_GetInstrumentSampleInfo(bankToken,
                                                          instrumentIndex,
                                                          i,
                                                          &sourceSample) != BAE_NO_ERROR)
            {
                return BAE_BAD_FILE;
            }
            if (PV_IsNoSampleSndID(sourceSample.sndResourceID))
            {
                noSampleSlots++;
            }
        }
        if (noSampleSlots != sourceSampleCount)
        {
            return BAE_BAD_FILE;
        }
        docExt = PV_FindInstrumentExt((BAERmfEditorDocument *)document,
                                      (XLongResourceID)targetInstID);
        if (!docExt || !docExt->useOscillator)
        {
            return BAE_BAD_FILE;
        }
        if (outSampleCount)
        {
            *outSampleCount = 0;
        }
        return BAE_NO_ERROR;
    }
#endif

    if (document->sampleCount != firstSampleIndex + sourceSampleCount)
    {
        return BAE_BAD_FILE;
    }

    for (sampleOffset = 0; sampleOffset < sourceSampleCount; ++sampleOffset)
    {
        BAERmfEditorBankSampleInfo sourceSample;
        BAERmfEditorSample const *clonedSample = &document->samples[firstSampleIndex + sampleOffset];

        if (BAERmfEditorBank_GetInstrumentSampleInfo(bankToken,
                                                      instrumentIndex,
                                                      sampleOffset,
                                                      &sourceSample) != BAE_NO_ERROR)
        {
            return BAE_BAD_FILE;
        }
        if (clonedSample->instID != targetInstID ||
            clonedSample->rootKey != sourceSample.rootKey ||
            clonedSample->lowKey != sourceSample.lowKey ||
            clonedSample->highKey != sourceSample.highKey ||
            clonedSample->splitVolume != sourceSample.splitVolume)
        {
            return BAE_BAD_FILE;
        }
    }
    if (outSampleCount)
    {
        *outSampleCount = sourceSampleCount;
    }
    return BAE_NO_ERROR;
}


BAEResult PV_PrepareUsedInstrumentsFromBank(
    BAERmfEditorDocument *document,
    BAEBankToken bankToken,
    BAE_BOOL embedAll,
    uint32_t const *embedResolvedInstIDs,
    uint32_t embedCount,
    BAERmfEditorCloneUsedResult *outResult)
{
    enum { kMaximumPitchedInstruments = 128 };
    PV_UsedInstrumentPair pitchedPairs[kMaximumPitchedInstruments];
    PV_UsedPercussion usedPercussion[kMaximumPitchedInstruments];
    uint32_t pitchedPairCount;
    uint32_t usedPercussionCount;
    uint32_t clonedPitchedCount;
    uint32_t clonedPercussionCount;
    uint16_t trackCount;
    uint16_t trackIndex;
    BAEResult result;

    if (!document || !bankToken)
    {
        return BAE_PARAM_ERR;
    }
    if (outResult)
    {
        XSetMemory(outResult, sizeof(*outResult), 0);
    }
    pitchedPairCount = 0;
    usedPercussionCount = 0;
    clonedPitchedCount = 0;
    clonedPercussionCount = 0;
    trackCount = 0;
    PV_ReserveBankSoundResourceIDs(document, (XFILE)bankToken);
    result = BAERmfEditorDocument_GetTrackCount(document, &trackCount);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    for (trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        uint32_t noteCount;
        uint32_t noteIndex;

        noteCount = 0;
        if (BAERmfEditorDocument_GetNoteCount(document, trackIndex, &noteCount) != BAE_NO_ERROR)
        {
            continue;
        }
        for (noteIndex = 0; noteIndex < noteCount; ++noteIndex)
        {
            BAERmfEditorNoteInfo noteInfo;
            if (BAERmfEditorDocument_GetNoteInfo(document, trackIndex, noteIndex, &noteInfo) != BAE_NO_ERROR)
            {
                continue;
            }
            /* CH10 + bank 0 = classic GM drums (note selects INST 128+n).
               CH10 + bank != 0 may be either:
                 - melodic CloneUsed embeds (bank+program → INST 512+p), or
                 - bank-group kits (group*256 + 128 + note, e.g. DreamTheme bank 2).
               Prefer Resolve of the melodic ID; fall back to kit percussion. */
            if (noteInfo.channel == 9)
            {
                if (!PV_AddUsedPercussion(usedPercussion,
                                          &usedPercussionCount,
                                          kMaximumPitchedInstruments,
                                          noteInfo.bank,
                                          noteInfo.note))
                {
                    return BAE_PARAM_ERR;
                }
            }
            else if (!PV_AddUsedInstrumentPair(pitchedPairs,
                                               &pitchedPairCount,
                                               kMaximumPitchedInstruments,
                                               noteInfo.bank,
                                               noteInfo.program))
            {
                return BAE_PARAM_ERR;
            }
        }
    }

    qsort(pitchedPairs, pitchedPairCount, sizeof(pitchedPairs[0]), PV_CompareUsedInstrumentPairs);
    PV_RemoveDocumentInstrumentAuxEvents(document);
    for (uint32_t pairIndex = 0; pairIndex < pitchedPairCount; ++pairIndex)
    {
        PV_UsedInstrumentPair const *pair = &pitchedPairs[pairIndex];
        uint32_t instrumentCount;
        uint32_t instrumentIndex;
        uint32_t expectedInstID;
        uint32_t resolvedInstID = 0;
        uint32_t firstSampleIndex;
        uint32_t clonedSampleCount;
        BAERmfEditorBankInstrumentInfo sourceInfo;
        bool found;

        instrumentCount = 0;
        expectedInstID = ((uint32_t)PV_BankGroupFromInternalBank(pair->bank) * 256u) + pair->program;
        unsigned char targetProgram;
        targetProgram = (unsigned char)pair->program;

        if (!PV_ExportShouldEmbedInst(expectedInstID, embedAll, embedResolvedInstIDs, embedCount))
        {
            (void)BAERmfEditorDocument_DeleteInstrument(document, expectedInstID);
            (void)BAERmfEditorDocument_DeleteInstrument(document, 512u + targetProgram);
            continue;
        }
        (void)BAERmfEditorDocument_DeleteInstrument(document, expectedInstID);
        (void)BAERmfEditorDocument_DeleteInstrument(document, 512u + targetProgram);

        resolvedInstID = expectedInstID;
        found = FALSE;
        result = BAERmfEditorBank_GetInstrumentCount(bankToken, &instrumentCount);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        for (instrumentIndex = 0; instrumentIndex < instrumentCount; ++instrumentIndex)
        {
            BAERmfEditorBankInstrumentInfo info;
            if (BAERmfEditorBank_GetInstrumentInfo(bankToken, instrumentIndex, &info) == BAE_NO_ERROR &&
                info.instID == expectedInstID)
            {
                found = TRUE;
                break;
            }
        }
        if (!found)
        {
            if (BAERmfEditorBank_ResolveInstID(bankToken,
                                               expectedInstID,
                                               &resolvedInstID,
                                               &instrumentIndex) != BAE_NO_ERROR)
            {
                debug_message("[CloneUsed] Missing pitched INST=%u for bank=%u program=%u, skipping clone\n",
                              (unsigned)expectedInstID,
                              (unsigned)pair->bank,
                              (unsigned)pair->program);
                continue;
            }
            /* Reject alias when it maps a melodic program to a percussion
             * instrument (or vice versa).  e.g. a GM melodic program 118
             * (Synth Drum) aliased to a tom drumkit INST would clone the
             * wrong instrument definition (wrong envelopes / ADSR). */
            if ((expectedInstID & 0x80u) != (resolvedInstID & 0x80u))
            {
                debug_message("[CloneUsed] Alias crosses melodic/drum boundary INST=%u -> INST=%u, not embedding\n",
                              (unsigned)expectedInstID,
                              (unsigned)resolvedInstID);
                continue;
            }
        }
        if (BAERmfEditorBank_GetInstrumentInfo(bankToken,
                                                instrumentIndex,
                                                &sourceInfo) != BAE_NO_ERROR)
        {
            continue;
        }
        if (found)
        {
            resolvedInstID = sourceInfo.instID;
        }
        if (sourceInfo.instID != resolvedInstID)
        {
            return BAE_BAD_FILE;
        }

        uint32_t targetInstID = expectedInstID;
        firstSampleIndex = document->sampleCount;
        if (!PV_ExportShouldEmbedInst(resolvedInstID, embedAll, embedResolvedInstIDs, embedCount) &&
            !PV_ExportShouldEmbedInst(expectedInstID, embedAll, embedResolvedInstIDs, embedCount))
        {
            continue;
        }

        result = BAERmfEditorDocument_CloneInstrumentFromBankToInstID(document,
                                                                      bankToken,
                                                                      instrumentIndex,
                                                                      targetInstID,
                                                                      targetProgram);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        result = PV_VerifyClonedInstrument(document,
                                           bankToken,
                                           instrumentIndex,
                                           targetInstID,
                                           firstSampleIndex,
                                           &clonedSampleCount);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        PV_AddCloneUsedMapping(outResult,
                               pair->bank,
                               pair->program,
                               FALSE,
                               expectedInstID,
                               resolvedInstID,
                               targetInstID,
                               clonedSampleCount,
                               sourceInfo.name);
        debug_message("[CloneUsed] source bank=%u program=%u INST=%u instIndex=%u -> target INST=%u program=%u\n",
                      (unsigned)pair->bank,
                      (unsigned)pair->program,
                      (unsigned)expectedInstID,
                      (unsigned)instrumentIndex,
                      (unsigned)targetInstID,
                      (unsigned)targetProgram);
        /* Do NOT remap notes; preserve user's original bank and program references. */
        clonedPitchedCount++;
    }

    if (outResult)
    {
        outResult->pitchedCount = clonedPitchedCount;
    }

    for (uint32_t percussionIndex = 0; percussionIndex < usedPercussionCount; ++percussionIndex)
    {
        PV_UsedPercussion const *percussion = &usedPercussion[percussionIndex];
        uint32_t note = percussion->note;
        uint32_t bankGroup;
        uint32_t requestedInstID;
        uint32_t instrumentIndex;
        uint32_t resolvedInstID;
        uint32_t firstSampleIndex;
        uint32_t clonedSampleCount;
        unsigned char targetProgram;
        BAERmfEditorBankInstrumentInfo sourceInfo;

        /* Kit encoding: group*256 + 128 + note (e.g. bank-2 kick → INST 676).
         * Clone to that natural ID and leave MIDI bank/note alone — same contract
         * as pitched natural-ID embeds. Do NOT remap into sequential 640+slots. */
        bankGroup = (uint32_t)PV_BankGroupFromInternalBank(percussion->bank);
        requestedInstID = (bankGroup * 256u) + 128u + note;
        targetProgram = (unsigned char)(note & 0x7Fu);

        if (!PV_ExportShouldEmbedInst(requestedInstID, embedAll, embedResolvedInstIDs, embedCount))
        {
            (void)BAERmfEditorDocument_DeleteInstrument(document, requestedInstID);
            continue;
        }
        (void)BAERmfEditorDocument_DeleteInstrument(document, requestedInstID);

        if (BAERmfEditorBank_ResolveInstID(bankToken,
                                           requestedInstID,
                                           &resolvedInstID,
                                           &instrumentIndex) != BAE_NO_ERROR)
        {
            debug_message("[CloneUsed] Missing percussion INST=%u for note=%u, skipping clone\n",
                          (unsigned)requestedInstID,
                          (unsigned)note);
            continue;
        }
        if ((requestedInstID & 0x80u) != (resolvedInstID & 0x80u))
        {
            debug_message("[CloneUsed] Perc alias crosses melodic/drum boundary INST=%u -> INST=%u, not embedding\n",
                          (unsigned)requestedInstID,
                          (unsigned)resolvedInstID);
            continue;
        }

        if (BAERmfEditorBank_GetInstrumentInfo(bankToken,
                                                instrumentIndex,
                                                &sourceInfo) != BAE_NO_ERROR ||
            sourceInfo.instID != resolvedInstID ||
            resolvedInstID < 128 ||
            sourceInfo.instID < 128)
        {
            continue;
        }
        if (!PV_ExportShouldEmbedInst(resolvedInstID, embedAll, embedResolvedInstIDs, embedCount) &&
            !PV_ExportShouldEmbedInst(requestedInstID, embedAll, embedResolvedInstIDs, embedCount))
        {
            continue;
        }

        firstSampleIndex = document->sampleCount;
        result = BAERmfEditorDocument_CloneInstrumentFromBankToInstID(document,
                                                                      bankToken,
                                                                      instrumentIndex,
                                                                      requestedInstID,
                                                                      targetProgram);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        result = PV_VerifyClonedInstrument(document,
                                           bankToken,
                                           instrumentIndex,
                                           requestedInstID,
                                           firstSampleIndex,
                                           &clonedSampleCount);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        /* Preserve original bank + note references on the MIDI (no remap). */
        PV_AddCloneUsedMapping(outResult,
                               percussion->bank,
                               (unsigned char)note,
                               TRUE,
                               requestedInstID,
                               resolvedInstID,
                               requestedInstID,
                               clonedSampleCount,
                               sourceInfo.name);
        clonedPercussionCount++;
    }

    if (outResult)
    {
        outResult->percussionCount = clonedPercussionCount;
    }
    return BAE_NO_ERROR;
}


BAEResult PV_AddRequiredAliases(BAERmfEditorDocument *document, XFILE fileRef, bool isZmf)
{
    uint32_t sampleIndex;
    uint32_t maxProgram;
    uint32_t percussionAliasCount;
    uint32_t pitchedAliasCount;
    uint32_t aliasCount;
    uint32_t aliasBytes;
    XPTR aliasBlob;
    uint32_t w;
    uint32_t trackIndex;
    unsigned char percussionTargetPrograms[128];
    unsigned char percussionUsedNotes[128];
    uint32_t percussionNoteCount;

    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }

    maxProgram = 0;
    for (sampleIndex = 0; sampleIndex < document->sampleCount; ++sampleIndex)
    {
        BAERmfEditorSample const *sample = &document->samples[sampleIndex];
        if (sample->instID != BAE_EDITOR_INST_ID_NONE && sample->instID >= 512)
        {
            uint32_t program = (uint32_t)(sample->instID - 512);
            if (program < 128 && program > maxProgram)
            {
                maxProgram = program;
            }
        }
    }
#if USE_ZMF_SUPPORT == TRUE
    /* Sample-free oscillator embeds still need alias coverage by INST id. */
    {
        uint32_t extIndex;
        for (extIndex = 0; extIndex < document->instrumentExtCount; ++extIndex)
        {
            BAERmfEditorInstrumentExt const *ext = &document->instrumentExts[extIndex];
            if (ext->useOscillator &&
                ext->instID != (XLongResourceID)BAE_EDITOR_INST_ID_NONE &&
                ext->instID >= 512)
            {
                uint32_t program = (uint32_t)ext->instID - 512u;
                if (program < 128 && program > maxProgram)
                {
                    maxProgram = program;
                }
            }
        }
    }
#endif

    if (maxProgram == 0)
    {
        return BAE_NO_ERROR;
    }

    XSetMemory(percussionTargetPrograms, (int32_t)sizeof(percussionTargetPrograms), 0);
    XSetMemory(percussionUsedNotes, (int32_t)sizeof(percussionUsedNotes), 0);
    percussionNoteCount = 0;

    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack const *track = &document->tracks[trackIndex];
        uint32_t noteIndex;

        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote const *note = &track->notes[noteIndex];
            if (note->channel == 9 && note->note < 128)
            {
                if (!percussionUsedNotes[note->note])
                {
                    /* Natural kit embeds live at 640+note. Only emit a remap
                     * alias when MIDI program was rewritten to a different slot. */
                    unsigned char targetProg = note->program;
                    if (targetProg == note->note || targetProg == 0)
                    {
                        percussionUsedNotes[note->note] = 2; /* used, identity — no alias */
                    }
                    else
                    {
                        percussionUsedNotes[note->note] = 1;
                        percussionTargetPrograms[note->note] = targetProg;
                        ++percussionNoteCount;
                    }
                }
            }
        }
    }

    pitchedAliasCount = maxProgram + 1;
    percussionAliasCount = percussionNoteCount;
    aliasCount = pitchedAliasCount + percussionAliasCount;
    aliasBytes = 8 + (aliasCount * 8);
    aliasBlob = XNewPtr((int32_t)aliasBytes);
    if (!aliasBlob)
    {
        return BAE_MEMORY_ERR;
    }
    XSetMemory(aliasBlob, (int32_t)aliasBytes, 0);

    XPutLong(&((unsigned char *)aliasBlob)[0], ALIAS_ID_RESOURCE_VERSION);
    XPutLong(&((unsigned char *)aliasBlob)[4], aliasCount);

    w = 0;

    for (; w < pitchedAliasCount; ++w)
    {
        uint32_t offset = (uint32_t)(8 + (w * 8));
        XPutLong(&((unsigned char *)aliasBlob)[offset],     (uint32_t)w);
        XPutLong(&((unsigned char *)aliasBlob)[offset + 4], 512u + w);
    }

    {
        uint32_t percussionNoteIndex;

        for (percussionNoteIndex = 0; percussionNoteIndex < 128; ++percussionNoteIndex)
        {
            uint32_t offset;
            uint32_t fromId;
            uint32_t toId;

            if (percussionUsedNotes[percussionNoteIndex] != 1)
            {
                continue;
            }

            fromId = ((uint32_t)(2u * 2u + 1u) * 128u) + percussionNoteIndex; /* 640+note */
            toId = (2u * 256u) + 128u + (uint32_t)percussionTargetPrograms[percussionNoteIndex];
            if (fromId == toId)
            {
                continue;
            }
            offset = (uint32_t)(8 + (w * 8));
            XPutLong(&((unsigned char *)aliasBlob)[offset], fromId);
            XPutLong(&((unsigned char *)aliasBlob)[offset + 4], toId);
            ++w;
        }
        /* Rewrite alias count — skipped identity perc aliases shrink the table. */
        XPutLong(&((unsigned char *)aliasBlob)[4], w);
    }

    {
        char pascalName[256];
        PV_CreatePascalName("EmbeddedInstrumentAliases", pascalName);
        if (XAddFileResource(fileRef, ID_ALIAS, (XLongResourceID)DEFAULT_RESOURCE_ALIAS_ID, pascalName, aliasBlob, (int32_t)aliasBytes) != 0)
        {
            XDisposePtr(aliasBlob);
            return BAE_FILE_IO_ERROR;
        }
    }

    XDisposePtr(aliasBlob);
    debug_message("[RMF Save] Added %u alias entries (%u pitched, %u percussion)\n",
                  aliasCount, pitchedAliasCount, percussionAliasCount);
    return BAE_NO_ERROR;
}


BAEResult PV_WriteRmfDocumentToResourceFile(BAERmfEditorDocument *document,
                                                   XFILE fileRef,
                                                   int32_t resourceID,
                                                   bool preserveOriginalMidi,
                                                   bool packForShip)
{
    XLongResourceID midiID;
    ByteBuffer midiData;
    char midiName[256];
    BAEResult result;
    bool isZmf;
    BAERmfEditorResourceEntry const *originalMidiEntry;

    if (!document || !fileRef)
    {
        return BAE_PARAM_ERR;
    }

    isZmf = (resourceID == XFILERESOURCE_ZMF_ID) ? TRUE : FALSE;
    originalMidiEntry = NULL;
    if (preserveOriginalMidi && document->loadedFromRmf && document->originalObjectResourceID != 0)
    {
        originalMidiEntry = PV_FindOriginalResourceByTypeAndID(document,
                                                               document->originalMidiType,
                                                               document->originalObjectResourceID);
    }

    debug_message("[RMF Save] Starting save, trackCount=%u, format=%s\n", document->trackCount, isZmf ? "ZMF" : "RMF");
    result = BAERmfEditorDocument_Validate(document);
    debug_message("[RMF Save] Validate result=%d, trackCount=%u\n", (int)result, document->trackCount);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    
    XSetMemory(&midiData, sizeof(midiData), 0);
    if (!originalMidiEntry)
    {
        result = PV_BuildMidiFile(document, &midiData);
        debug_message("[RMF Save] BuildMidiFile result=%d, size=%u\n", (int)result, midiData.size);
        if (result != BAE_NO_ERROR)
        {
            PV_ByteBufferDispose(&midiData);
            return result;
        }
        PV_DebugReportMidiRoundTripDiff(document, &midiData);
    }
    result = PV_EnsureResourceFileReady(fileRef, resourceID);
    debug_message("[RMF Save] EnsureResourceFileReady result=%d\n", (int)result);
    if (result != BAE_NO_ERROR)
    {
        PV_ByteBufferDispose(&midiData);
        return result;
    }

    if (document->loadedFromRmf && document->originalResourceCount > 0 && document->originalObjectResourceID != 0)
    {
        uint32_t resourceIndex;

        for (resourceIndex = 0; resourceIndex < document->originalResourceCount; ++resourceIndex)
        {
            BAERmfEditorResourceEntry const *entry;

            entry = &document->originalResources[resourceIndex];
            if (entry->type == XFILECACHE_ID)
            {
                continue;
            }
            if (entry->type == ID_SND || entry->type == ID_CSND || entry->type == ID_ESND || entry->type == ID_INST)
            {
                continue;
            }
            if (PV_IsMidiResourceType(entry->type))
            {
                continue;
            }
            if (entry->type == ID_SONG)
            {
                continue;
            }
            /* Session uncompressed PCM masters — never write into export RMF/ZMF. */
            if (entry->type == FOUR_CHAR('C', 'a', 'S', 'd'))
            {
                continue;
            }
            if (XAddFileResource(fileRef,
                                 entry->type,
                                 entry->id,
                                 entry->pascalName,
                                 entry->data,
                                 entry->size) != 0)
            {
                PV_ByteBufferDispose(&midiData);
                return BAE_FILE_IO_ERROR;
            }
        }
        {
            char midiPascalName[256];
            XPTR encodedMidi;
            int32_t encodedMidiSize;

            if (originalMidiEntry && originalMidiEntry->pascalName[0])
            {
                XBlockMove(originalMidiEntry->pascalName, midiPascalName, 256);
            }
            else
            {
                PV_CreatePascalName(document->info[TITLE_INFO] ? document->info[TITLE_INFO] : "Song", midiPascalName);
            }

            if (originalMidiEntry && originalMidiEntry->data && originalMidiEntry->size > 0)
            {
                if (XAddFileResource(fileRef,
                                     originalMidiEntry->type,
                                     document->originalObjectResourceID,
                                     midiPascalName,
                                     originalMidiEntry->data,
                                     originalMidiEntry->size) != 0)
                {
                    PV_ByteBufferDispose(&midiData);
                    return BAE_FILE_IO_ERROR;
                }
            }
            else
            {
                XResourceType usedMidiType;
                result = PV_EncodeMidiForStorageType(document->midiStorageType,
                                                     &midiData,
                                                     &encodedMidi,
                                                     &encodedMidiSize,
                                                     &usedMidiType,
                                                     isZmf);
                if (result != BAE_NO_ERROR)
                {
                    PV_ByteBufferDispose(&midiData);
                    return result;
                }
                if (XAddFileResource(fileRef,
                                     usedMidiType,
                                     document->originalObjectResourceID,
                                     midiPascalName,
                                     encodedMidi,
                                     encodedMidiSize) != 0)
                {
                    XDisposePtr(encodedMidi);
                    PV_ByteBufferDispose(&midiData);
                    return BAE_FILE_IO_ERROR;
                }
                XDisposePtr(encodedMidi);
            }
        }
        result = PV_AddSongResourceWithID(document,
                                          fileRef,
                                          document->originalObjectResourceID,
                                          document->originalSongID ? document->originalSongID : 1,
                                          NULL);
        if (result != BAE_NO_ERROR)
        {
            PV_ByteBufferDispose(&midiData);
            return result;
        }
        result = PV_AddSampleResources(document, fileRef, isZmf);
        debug_message("[RMF Save] loadedFromRmf AddSampleResources result=%d, sampleCount=%u\n",
                   (int)result, document->sampleCount);
        if (result != BAE_NO_ERROR)
        {
            PV_ByteBufferDispose(&midiData);
            return result;
        }
        result = PV_AddRequiredAliases(document, fileRef, isZmf);
        debug_message("[RMF Save] loadedFromRmf AddRequiredAliases result=%d\n", (int)result);
        if (result != BAE_NO_ERROR)
        {
            PV_ByteBufferDispose(&midiData);
            return result;
        }
        if (packForShip)
        {
            if (XPackResourceFileForShip(fileRef) == FALSE)
            {
                PV_ByteBufferDispose(&midiData);
                return BAE_FILE_IO_ERROR;
            }
        }
        else if (XFinalizeEditorResourceFile(fileRef) == FALSE)
        {
            PV_ByteBufferDispose(&midiData);
            return BAE_FILE_IO_ERROR;
        }
        PV_ByteBufferDispose(&midiData);
        return BAE_NO_ERROR;
    }

    PV_CreatePascalName(document->info[TITLE_INFO] ? document->info[TITLE_INFO] : "Song", midiName);
    {
        XPTR encodedMidi;
        int32_t encodedMidiSize;
        XResourceType usedMidiType;

        result = PV_EncodeMidiForStorageType(document->midiStorageType,
                                             &midiData,
                                             &encodedMidi,
                                             &encodedMidiSize,
                                             &usedMidiType,
                                             isZmf);
        if (result != BAE_NO_ERROR)
        {
            debug_message("[RMF Save] MIDI encode failed result=%d\n", (int)result);
            PV_ByteBufferDispose(&midiData);
            return result;
        }
        if (PV_GetAvailableResourceID(fileRef, usedMidiType, 1, &midiID) != BAE_NO_ERROR)
        {
            debug_message("[RMF Save] GetAvailableResourceID for selected MIDI type failed\n");
            XDisposePtr(encodedMidi);
            PV_ByteBufferDispose(&midiData);
            return BAE_FILE_IO_ERROR;
        }
        if (XAddFileResource(fileRef, usedMidiType, midiID, midiName, encodedMidi, encodedMidiSize) != 0)
        {
            XDisposePtr(encodedMidi);
            debug_message("[RMF Save] XAddFileResource(MIDI) failed\n");
            PV_ByteBufferDispose(&midiData);
            return BAE_FILE_IO_ERROR;
        }
        XDisposePtr(encodedMidi);
    }
    result = PV_AddSampleResources(document, fileRef, isZmf);
    debug_message("[RMF Save] AddSampleResources result=%d, sampleCount=%u\n", (int)result, document->sampleCount);
    if (result == BAE_NO_ERROR)
    {
        result = PV_AddRequiredAliases(document, fileRef, isZmf);
        debug_message("[RMF Save] AddRequiredAliases result=%d\n", (int)result);
    }
    if (result == BAE_NO_ERROR)
    {
        result = PV_AddSongResource(document, fileRef, midiID);
        debug_message("[RMF Save] AddSongResource result=%d\n", (int)result);
    }
    if (result == BAE_NO_ERROR)
    {
        if (packForShip)
        {
            if (XPackResourceFileForShip(fileRef) == FALSE)
            {
                debug_message("[RMF Save] XPackResourceFileForShip failed\n");
                result = BAE_FILE_IO_ERROR;
            }
        }
        else if (XFinalizeEditorResourceFile(fileRef) == FALSE)
        {
            debug_message("[RMF Save] XFinalizeEditorResourceFile failed\n");
            result = BAE_FILE_IO_ERROR;
        }
    }
    debug_message("[RMF Save] Final result=%d\n", (int)result);
    PV_ByteBufferDispose(&midiData);
    return result;
}

