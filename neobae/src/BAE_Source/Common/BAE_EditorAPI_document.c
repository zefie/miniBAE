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
**  BAE_EditorAPI_document.c
**
**  Document lifecycle, samples/assets, INST ext/ghost, Validate, SaveAsRmf. Perf: neobae/docs/BAE_EditorAPI_PERF.md
*/
/*****************************************************************************/

#include "BAE_EditorAPI_Internal.h"


/* ---------- Instrument extended data helpers ---------- */

#define EDITOR_KEY_SPLIT_FILE_SIZE 8  /* matches KEY_SPLIT_FILE_SIZE in GenPatch.c */

BAERmfEditorInstrumentExt *PV_FindInstrumentExt(BAERmfEditorDocument *document, XLongResourceID instID)
{
    uint32_t i;
    for (i = 0; i < document->instrumentExtCount; i++)
    {
        if (document->instrumentExts[i].instID == instID)
        {
            return &document->instrumentExts[i];
        }
    }
    return NULL;
}


BAEResult PV_AddInstrumentExt(BAERmfEditorDocument *document, BAERmfEditorInstrumentExt const *ext)
{
    BAEResult result;
    result = PV_GrowBuffer((void **)&document->instrumentExts,
                           &document->instrumentExtCapacity,
                           sizeof(BAERmfEditorInstrumentExt),
                           document->instrumentExtCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    document->instrumentExts[document->instrumentExtCount] = *ext;
    document->instrumentExtCount++;
    return BAE_NO_ERROR;
}


void PV_EnsureInstrumentExtForRemappedID(BAERmfEditorDocument *document,
                                                XLongResourceID oldInstID,
                                                XLongResourceID newInstID)
{
    BAERmfEditorInstrumentExt *oldExt;
    BAERmfEditorInstrumentExt clone;
    BAEResult addResult;

    if (!document || oldInstID == (XLongResourceID)BAE_EDITOR_INST_ID_NONE || newInstID == (XLongResourceID)BAE_EDITOR_INST_ID_NONE || oldInstID == newInstID)
    {
        return;
    }
    if (PV_FindInstrumentExt(document, newInstID))
    {
        return;
    }
    oldExt = PV_FindInstrumentExt(document, oldInstID);
    if (!oldExt)
    {
        return;
    }

    clone = *oldExt;
    clone.instID = newInstID;
    clone.dirty = TRUE;
    clone.displayName = NULL;
    clone.originalInstData = NULL;
    clone.originalInstSize = 0;
    if (oldExt->displayName)
    {
        clone.displayName = PV_DuplicateString(oldExt->displayName);
        if (!clone.displayName)
        {
            return;
        }
    }

    addResult = PV_AddInstrumentExt(document, &clone);
    if (addResult != BAE_NO_ERROR && clone.displayName)
    {
        XDisposePtr(clone.displayName);
    }
}


void PV_ClearInstrumentExts(BAERmfEditorDocument *document)
{
    uint32_t i;
    if (!document)
    {
        return;
    }
    for (i = 0; i < document->instrumentExtCount; i++)
    {
        PV_FreeString(&document->instrumentExts[i].displayName);
        if (document->instrumentExts[i].originalInstData)
        {
            XDisposePtr(document->instrumentExts[i].originalInstData);
            document->instrumentExts[i].originalInstData = NULL;
        }
    }
    if (document->instrumentExts)
    {
        XDisposePtr(document->instrumentExts);
        document->instrumentExts = NULL;
    }
    document->instrumentExtCount = 0;
    document->instrumentExtCapacity = 0;
}


/* Build XInstrumentData from Ext unit fields (no edit padding). */
void PV_FillXFromInstrumentExt(XInstrumentData *x, BAERmfEditorInstrumentExt const *ext)
{
    uint32_t i;
    int32_t count2;
    int32_t unitIndex;

    if (!x || !ext)
    {
        return;
    }

    XSetMemory(x, (int32_t)sizeof(*x), 0);
    unitIndex = 0;

    if (ext->volumeADSR.stageCount > 0)
    {
        XEnvelopeData *env = &x->units[unitIndex].u.envelopeADSR;
        int32_t stageCount = (int32_t)ext->volumeADSR.stageCount;
        if (stageCount > ADSR_STAGES)
        {
            stageCount = ADSR_STAGES;
        }
        x->units[unitIndex].unitType = INST_ADSR_ENVELOPE;
        x->units[unitIndex].unitID = (uint32_t)unitIndex;
        env->stageCount = stageCount;
        for (count2 = 0; count2 < stageCount; count2++)
        {
            env->level[count2] = ext->volumeADSR.stages[count2].level;
            env->time[count2] = ext->volumeADSR.stages[count2].time;
            env->flags[count2] = ext->volumeADSR.stages[count2].flags;
        }
        unitIndex++;
    }

    if (ext->hasDefaultMod)
    {
        x->units[unitIndex].unitType = INST_DEFAULT_MOD;
        x->units[unitIndex].unitID = (uint32_t)unitIndex;
        x->units[unitIndex].u.useDefaultModwheelAction = TRUE;
        unitIndex++;
    }

    if (ext->LPF_frequency != 0 || ext->LPF_resonance != 0 || ext->LPF_lowpassAmount != 0)
    {
        x->units[unitIndex].unitType = INST_LOW_PASS_FILTER;
        x->units[unitIndex].unitID = (uint32_t)unitIndex;
        x->units[unitIndex].u.lpf.LPF_frequency = ext->LPF_frequency;
        x->units[unitIndex].u.lpf.LPF_resonance = ext->LPF_resonance;
        x->units[unitIndex].u.lpf.LPF_lowpassAmount = ext->LPF_lowpassAmount;
        unitIndex++;
    }

    if (ext->defaultReverbSend > 0)
    {
        x->units[unitIndex].unitType = INST_REVERB_SEND;
        x->units[unitIndex].unitID = (uint32_t)unitIndex;
        x->units[unitIndex].u.sendAmount = (int32_t)ext->defaultReverbSend;
        unitIndex++;
    }

    if (ext->defaultChorusSend > 0)
    {
        x->units[unitIndex].unitType = INST_CHORUS_SEND;
        x->units[unitIndex].unitID = (uint32_t)unitIndex;
        x->units[unitIndex].u.sendAmount = (int32_t)ext->defaultChorusSend;
        unitIndex++;
    }

#if USE_ZMF_SUPPORT == TRUE
    if (ext->useOscillator && unitIndex < 256)
    {
        x->units[unitIndex].unitType = INST_OSCILLATOR;
        x->units[unitIndex].unitID = (uint32_t)unitIndex;
        x->units[unitIndex].u.osc.waveShape = ext->oscWaveShape ? ext->oscWaveShape : SINE_WAVE_LONG;
        x->units[unitIndex].u.osc.pulseWidth = ext->oscPulseWidth;
        if (x->units[unitIndex].u.osc.pulseWidth <= 0)
            x->units[unitIndex].u.osc.pulseWidth = 32768;
        x->units[unitIndex].u.osc.volume = ext->oscVolume;
        if (x->units[unitIndex].u.osc.volume < 0)
            x->units[unitIndex].u.osc.volume = 0;
        if (x->units[unitIndex].u.osc.volume > 65536)
            x->units[unitIndex].u.osc.volume = 65536;
        unitIndex++;
    }
#endif

    for (i = 0; i < ext->lfoCount && unitIndex < 256; i++)
    {
        EditorLFO const *lfo = &ext->lfos[i];
        XLFOData *dst = &x->units[unitIndex].u.lfo;
        int32_t stageCount = (int32_t)lfo->adsr.stageCount;
        if (stageCount > ADSR_STAGES)
        {
            stageCount = ADSR_STAGES;
        }
        x->units[unitIndex].unitType = (XUnitType)lfo->destination;
        x->units[unitIndex].unitID = (uint32_t)unitIndex;
        dst->envelopeLFO.stageCount = stageCount;
        for (count2 = 0; count2 < stageCount; count2++)
        {
            dst->envelopeLFO.level[count2] = lfo->adsr.stages[count2].level;
            dst->envelopeLFO.time[count2] = lfo->adsr.stages[count2].time;
            dst->envelopeLFO.flags[count2] = lfo->adsr.stages[count2].flags;
        }
        dst->period = lfo->period;
        dst->waveShape = lfo->waveShape;
        dst->DC_feed = lfo->DC_feed;
        dst->depth = lfo->level;
        unitIndex++;
    }

    for (i = 0; i < ext->curveCount && unitIndex < 256; i++)
    {
        EditorCurve const *curve = &ext->curves[i];
        XTieToData *dst = &x->units[unitIndex].u.curve;
        int32_t curvePoints = curve->curveCount;
        if (curvePoints > X_INSTRUMENT_MAX_CURVE_POINTS)
        {
            curvePoints = X_INSTRUMENT_MAX_CURVE_POINTS;
        }
        if (curvePoints > EDITOR_MAX_ADSR_STAGES)
        {
            curvePoints = EDITOR_MAX_ADSR_STAGES;
        }
        x->units[unitIndex].unitType = INST_EXPONENTIAL_CURVE;
        x->units[unitIndex].unitID = (uint32_t)unitIndex;
        dst->tieFrom = curve->tieFrom;
        dst->tieTo = curve->tieTo;
        dst->curveCount = (int16_t)curvePoints;
        for (count2 = 0; count2 < curvePoints; count2++)
        {
            dst->from_Value[count2] = curve->from_Value[count2];
            dst->to_Scalar[count2] = curve->to_Scalar[count2];
        }
        unitIndex++;
    }

    x->unitCount = unitIndex;
    x->unitBlockPresent = (unitIndex > 0) ? TRUE : FALSE;
}


/* Parse an InstrumentResource blob into BAERmfEditorInstrumentExt via
 * XCreateXInstrumentEx (lossless; no edit padding). */
void PV_ParseExtendedInstData(XPTR instData, int32_t instSize, BAERmfEditorInstrumentExt *ext)
{
    unsigned char const *pBase;
    XInstrumentData *x;

    XSetMemory(ext, (int32_t)sizeof(*ext), 0);

    if (!instData || instSize < 14)
    {
        return;
    }

    pBase = (unsigned char const *)instData;

    /* Read header fields via byte offsets (same as save path enum) */
    ext->panPlacement = (char)pBase[4];
    ext->flags1 = pBase[5];
    ext->flags2 = pBase[6];
    ext->midiRootKey = (int16_t)XGetShort((void *)(pBase + 2));
    ext->miscParameter1 = (int16_t)XGetShort((void *)(pBase + 8));
    ext->miscParameter2 = (int16_t)XGetShort((void *)(pBase + 10));
#if USE_ZMF_SUPPORT == TRUE
    ext->oscVolume = OSC_VOLUME_DEFAULT;
    ext->oscPulseWidth = 32768;
#endif

    /* Default ADSR until units override it */
    ext->volumeADSR.stageCount = 1;
    ext->volumeADSR.stages[0].level = VOLUME_RANGE;
    ext->volumeADSR.stages[0].time = 0;
    ext->volumeADSR.stages[0].flags = ADSR_TERMINATE_LONG;

    if (!(ext->flags1 & ZBF_extendedFormat))
    {
        return;
    }

    x = XCreateXInstrumentEx((InstrumentResource *)instData, (uint32_t)instSize, FALSE);
    if (!x)
    {
        return;
    }

    if (x->unitBlockPresent)
    {
        ext->hasExtendedData = TRUE;
        PV_FillExtFromXInstrument(ext, x);
    }

    XDisposePtr((XPTR)x);
}


/* Serialize extended units via XSerializeInstrumentUnits.
 * Uses 10 reserved bytes: caller appends after tremolo/name/descriptorFlags,
 * and the INST parser treats descriptorFlags(2) + these 10 as the 12-byte reserved. */
XPTR PV_SerializeExtendedInstTail(BAERmfEditorInstrumentExt const *ext, int32_t *outSize)
{
    XInstrumentData x;

    if (!ext || !outSize)
    {
        if (outSize)
        {
            *outSize = 0;
        }
        return NULL;
    }

    PV_FillXFromInstrumentExt(&x, ext);
    return XSerializeInstrumentUnits(&x, 10, outSize);
}


BAERmfEditorDocument *BAERmfEditorDocument_New(void)
{
    BAERmfEditorDocument *document;

    document = (BAERmfEditorDocument *)XNewPtr(sizeof(BAERmfEditorDocument));
    if (document)
    {
        XSetMemory(document, sizeof(BAERmfEditorDocument), 0);
        document->tempoBPM = 120;
        document->ticksPerQuarter = 480;
        document->songType = SONG_TYPE_RMF;
        document->songTempo = 16667;
        document->songPitchShift = 0;
        document->songLocked = FALSE;
        document->songEmbedded = FALSE;
        document->maxMidiNotes = 24;
        document->maxEffects = 4;
        document->mixLevel = 8;
        document->songVolume = 127;
        document->reverbType = BAE_REVERB_TYPE_1;
        document->originalSongID = 0;
        document->originalObjectResourceID = 0;
        document->originalMidiType = 0;
        document->midiStorageType = BAE_EDITOR_MIDI_STORAGE_ECMI;
        document->loadedFromRmf = FALSE;
        document->isPristine = FALSE;
        document->nextSampleAssetID = 1;
        document->engineConfigFlags = 0;
        document->velocityCurveType = DEFAULT_VELOCITY_CURVE;
    }
    return document;
}


BAERmfEditorDocument *BAERmfEditorDocument_LoadFromFile(BAEPathName filePath)
{
    BAERmfEditorDocument *document;
    BAEFileType fileType;
    BAEResult result;

    if (!filePath)
    {
        return NULL;
    }
    fileType = PV_DetermineEditorImportFileType(filePath);
    document = BAERmfEditorDocument_New();
    if (!document)
    {
        return NULL;
    }
#if USE_MTHC_SUPPORT == TRUE
    if (fileType == BAE_MIDI_TYPE || fileType == BAE_MTHC)
#else
    if (fileType == BAE_MIDI_TYPE)
#endif
    {
        unsigned char *data;
        uint32_t dataSize;

        result = PV_ReadWholeFile(filePath, &data, &dataSize);
        if (result == BAE_NO_ERROR)
        {
            /* .mid extension reports BAE_MIDI_TYPE even for MThc payloads. */
            result = PV_LoadMidiOrMthcBytesIntoDocument(document, data, dataSize);
            XDisposePtr(data);
        }
    }
    else if (fileType == BAE_RMF)
    {
        result = PV_LoadRmfFileIntoDocument(document, filePath);
    }
    else if (fileType == BAE_RMI)
    {
        unsigned char *rmiData;
        uint32_t rmiDataSize;

        result = PV_ReadWholeFile(filePath, &rmiData, &rmiDataSize);
        if (result == BAE_NO_ERROR)
        {
            /* Inline RIFF MIDI extraction: find 'data' chunk inside RIFF/RMID, ignore any DLS */
            const unsigned char *midiStart = NULL;
            uint32_t midiLen = 0;

            if (rmiDataSize >= 20 &&
                rmiData[0]=='R' && rmiData[1]=='I' && rmiData[2]=='F' && rmiData[3]=='F' &&
                rmiData[8]=='R' && rmiData[9]=='M' && rmiData[10]=='I' && rmiData[11]=='D')
            {
                uint32_t pos = 12;
                while (pos + 8 <= rmiDataSize)
                {
                    uint32_t chunkSize = (uint32_t)rmiData[pos+4]
                                      | ((uint32_t)rmiData[pos+5] << 8)
                                      | ((uint32_t)rmiData[pos+6] << 16)
                                      | ((uint32_t)rmiData[pos+7] << 24);
                    if (rmiData[pos]=='d' && rmiData[pos+1]=='a' && rmiData[pos+2]=='t' && rmiData[pos+3]=='a'
                        && pos + 8 + chunkSize <= rmiDataSize)
                    {
                        midiStart = rmiData + pos + 8;
                        midiLen = chunkSize;
                        break;
                    }
                    pos += 8 + ((chunkSize + 1) & ~1U);
                }
            }

            if (midiStart && midiLen >= 4 &&
                midiStart[0]=='M' && midiStart[1]=='T' && midiStart[2]=='h' && midiStart[3]=='d')
            {
                result = PV_LoadMidiBytesIntoDocument(document, midiStart, midiLen);
                if (result == BAE_NO_ERROR)
                {
                    result = PV_SetDebugOriginalMidiData(document, midiStart, midiLen);
                }
                if (result == BAE_NO_ERROR)
                {
                    document->isPristine = TRUE;
                }
            }
            else
            {
                result = BAE_BAD_FILE;
            }
            XDisposePtr(rmiData);
        }
    }
    else
    {
        result = BAE_BAD_FILE_TYPE;
    }
    if (result != BAE_NO_ERROR)
    {
        BAERmfEditorDocument_Delete(document);
        return NULL;
    }
    return document;
}


BAERmfEditorDocument *BAERmfEditorDocument_LoadFromMemory(void const *data,
                                                          uint32_t dataSize,
                                                          BAEFileType fileTypeHint)
{
    BAERmfEditorDocument *document;
    BAEFileType fileType;
    BAEResult result;

    if (!data || dataSize == 0)
    {
        return NULL;
    }

    fileType = PV_DetermineEditorImportMemoryFileType(data, dataSize, fileTypeHint);
    document = BAERmfEditorDocument_New();
    if (!document)
    {
        return NULL;
    }

#if USE_MTHC_SUPPORT == TRUE
    if (fileType == BAE_MIDI_TYPE || fileType == BAE_MTHC)
#else
    if (fileType == BAE_MIDI_TYPE)
#endif
    {
        result = PV_LoadMidiOrMthcBytesIntoDocument(document,
                                                    (unsigned char const *)data,
                                                    dataSize);
    }
    else if (fileType == BAE_RMF)
    {
        result = PV_LoadRmfMemoryIntoDocument(document, data, dataSize);
    }
    else if (fileType == BAE_RMI)
    {
        unsigned char const *rmiData;
        uint32_t rmiDataSize;
        unsigned char const *midiStart;
        uint32_t midiLen;

        rmiData = (unsigned char const *)data;
        rmiDataSize = dataSize;
        midiStart = NULL;
        midiLen = 0;

        if (rmiDataSize >= 20 &&
            rmiData[0]=='R' && rmiData[1]=='I' && rmiData[2]=='F' && rmiData[3]=='F' &&
            rmiData[8]=='R' && rmiData[9]=='M' && rmiData[10]=='I' && rmiData[11]=='D')
        {
            uint32_t pos;

            pos = 12;
            while (pos + 8 <= rmiDataSize)
            {
                uint32_t chunkSize;

                chunkSize = (uint32_t)rmiData[pos+4]
                         | ((uint32_t)rmiData[pos+5] << 8)
                         | ((uint32_t)rmiData[pos+6] << 16)
                         | ((uint32_t)rmiData[pos+7] << 24);
                if (rmiData[pos]=='d' && rmiData[pos+1]=='a' && rmiData[pos+2]=='t' && rmiData[pos+3]=='a'
                    && pos + 8 + chunkSize <= rmiDataSize)
                {
                    midiStart = rmiData + pos + 8;
                    midiLen = chunkSize;
                    break;
                }
                pos += 8 + ((chunkSize + 1) & ~1U);
            }
        }

        if (midiStart && midiLen >= 4 &&
            midiStart[0]=='M' && midiStart[1]=='T' && midiStart[2]=='h' && midiStart[3]=='d')
        {
            result = PV_LoadMidiOrMthcBytesIntoDocument(document, midiStart, midiLen);
        }
        else
        {
            result = BAE_BAD_FILE;
        }
    }
    else
    {
        result = BAE_BAD_FILE_TYPE;
    }

    if (result != BAE_NO_ERROR)
    {
        BAERmfEditorDocument_Delete(document);
        return NULL;
    }
    return document;
}


BAEResult BAERmfEditorDocument_Delete(BAERmfEditorDocument *document)
{
    uint32_t index;

    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    for (index = 0; index < INFO_TYPE_COUNT; ++index)
    {
        PV_FreeString(&document->info[index]);
    }
    for (index = 0; index < document->trackCount; ++index)
    {
        PV_FreeString(&document->tracks[index].name);
        if (document->tracks[index].notes)
        {
            XDisposePtr(document->tracks[index].notes);
        }
        if (document->tracks[index].ccEvents)
        {
            XDisposePtr(document->tracks[index].ccEvents);
        }
        PV_FreeTrackSysExEvents(&document->tracks[index]);
        PV_FreeTrackAuxEvents(&document->tracks[index]);
        PV_FreeTrackMetaEvents(&document->tracks[index]);
    }
    if (document->tracks)
    {
        XDisposePtr(document->tracks);
    }
    for (index = 0; index < document->sampleCount; ++index)
    {
        PV_FreeString(&document->samples[index].displayName);
        PV_FreeString(&document->samples[index].sourcePath);
        if (document->samples[index].waveform)
        {
            GM_FreeWaveform(document->samples[index].waveform);
        }
        if (document->samples[index].originalSndData)
        {
            XDisposePtr(document->samples[index].originalSndData);
            document->samples[index].originalSndData = NULL;
            document->samples[index].originalSndSize = 0;
        }
    }
    if (document->samples)
    {
        XDisposePtr(document->samples);
    }
    PV_ClearTempoEvents(document);
    PV_ClearInstrumentExts(document);
    PV_FreeOriginalResources(document);
    PV_FreeDebugOriginalMidiData(document);
    XDisposePtr(document);
    return BAE_NO_ERROR;
}


int PV_CompareAuxOrderRef(void const *left, void const *right)
{
    PV_AuxOrderRef const *a;
    PV_AuxOrderRef const *b;

    a = (PV_AuxOrderRef const *)left;
    b = (PV_AuxOrderRef const *)right;
    if (a->tick < b->tick)
    {
        return -1;
    }
    if (a->tick > b->tick)
    {
        return 1;
    }
    if (a->eventOrder < b->eventOrder)
    {
        return -1;
    }
    if (a->eventOrder > b->eventOrder)
    {
        return 1;
    }
    return 0;
}


void PV_ShiftTrackEventOrders(BAERmfEditorTrack *track,
                                     uint32_t startOrder,
                                     uint32_t delta)
{
    uint32_t i;

    if (!track || delta == 0)
    {
        return;
    }

    for (i = 0; i < track->noteCount; ++i)
    {
        if (track->notes[i].noteOnOrder >= startOrder)
        {
            track->notes[i].noteOnOrder += delta;
        }
        if (track->notes[i].noteOffOrder >= startOrder)
        {
            track->notes[i].noteOffOrder += delta;
        }
    }
    for (i = 0; i < track->ccEventCount; ++i)
    {
        if (track->ccEvents[i].eventOrder >= startOrder)
        {
            track->ccEvents[i].eventOrder += delta;
        }
    }
    for (i = 0; i < track->sysexEventCount; ++i)
    {
        if (track->sysexEvents[i].eventOrder >= startOrder)
        {
            track->sysexEvents[i].eventOrder += delta;
        }
    }
    for (i = 0; i < track->auxEventCount; ++i)
    {
        if (track->auxEvents[i].eventOrder >= startOrder)
        {
            track->auxEvents[i].eventOrder += delta;
        }
    }
    for (i = 0; i < track->metaEventCount; ++i)
    {
        if (track->metaEvents[i].eventOrder >= startOrder)
        {
            track->metaEvents[i].eventOrder += delta;
        }
    }
    track->nextEventOrder += delta;
}


BAEResult PV_RemapTrackInstrumentReferences(BAERmfEditorTrack *track,
                                                   uint16_t sourceBank,
                                                   unsigned char sourceProgram,
                                                   uint16_t targetBank,
                                                   unsigned char targetProgram,
                                                   uint16_t remapChannelMask,
                                                   bool *outChanged)
{
    bool changed;
    bool restartScan;
    uint16_t sourceMsb;
    uint16_t sourceLsb;
    uint16_t targetMsb;
    uint16_t targetLsb;
    uint16_t sourceBankMidi;
    uint16_t targetBankMidi;

    if (!track)
    {
        return BAE_PARAM_ERR;
    }

    /* Normalize to MIDI (MSB<<7)|LSB so short Beatnik groups and imported
     * MIDI banks compare / rewrite correctly. */
    PV_BankToMidiMsbLsb(sourceBank, &sourceMsb, &sourceLsb);
    PV_BankToMidiMsbLsb(targetBank, &targetMsb, &targetLsb);
    sourceBankMidi = (uint16_t)((sourceMsb << 7) | sourceLsb);
    targetBankMidi = (uint16_t)((targetMsb << 7) | targetLsb);

    changed = FALSE;
    do
    {
        uint16_t currentBank[BAE_MAX_MIDI_CHANNELS];
        uint32_t lastBankMsbIndex[BAE_MAX_MIDI_CHANNELS];
        uint32_t lastBankLsbIndex[BAE_MAX_MIDI_CHANNELS];
        PV_AuxOrderRef *refs;
        uint32_t i;

        restartScan = FALSE;

        if (track->auxEventCount == 0)
        {
            break;
        }

        refs = (PV_AuxOrderRef *)XNewPtr((int32_t)(sizeof(PV_AuxOrderRef) * track->auxEventCount));
        if (!refs)
        {
            return BAE_MEMORY_ERR;
        }

        for (i = 0; i < BAE_MAX_MIDI_CHANNELS; ++i)
        {
            currentBank[i] = 0;
            lastBankMsbIndex[i] = 0xFFFFFFFFu;
            lastBankLsbIndex[i] = 0xFFFFFFFFu;
        }

        /* Freeze the scan length: inserts grow auxEventCount and must not
         * extend this loop past the allocated refs[] array. */
        {
            uint32_t scanCount = track->auxEventCount;

            for (i = 0; i < scanCount; ++i)
            {
                refs[i].index = i;
                refs[i].tick = track->auxEvents[i].tick;
                refs[i].eventOrder = track->auxEvents[i].eventOrder;
            }
            qsort(refs, scanCount, sizeof(PV_AuxOrderRef), PV_CompareAuxOrderRef);

            for (i = 0; i < scanCount; ++i)
            {
                BAERmfEditorAuxEvent *aux;
                unsigned char eventType;
                unsigned char channel;

                aux = &track->auxEvents[refs[i].index];
                eventType = (unsigned char)(aux->status & 0xF0);
                channel = (unsigned char)(aux->status & 0x0F);

                if (eventType == CONTROL_CHANGE && aux->dataBytes >= 2)
                {
                    unsigned char sourceData2;

                    sourceData2 = aux->data2;
                    if (aux->data1 == BANK_MSB)
                    {
                        currentBank[channel] = (uint16_t)((((uint16_t)sourceData2) << 7) | (currentBank[channel] & 0x7F));
                        lastBankMsbIndex[channel] = refs[i].index;
                    }
                    else if (aux->data1 == BANK_LSB)
                    {
                        currentBank[channel] = (uint16_t)((currentBank[channel] & 0x3F80) | ((uint16_t)sourceData2 & 0x7F));
                        lastBankLsbIndex[channel] = refs[i].index;
                    }

                    if (sourceBankMidi != targetBankMidi &&
                        (remapChannelMask & (uint16_t)(1u << channel)) != 0)
                    {
                        if (aux->data1 == BANK_MSB && sourceData2 == (unsigned char)sourceMsb)
                        {
                            aux->data2 = (unsigned char)targetMsb;
                            changed = TRUE;
                        }
                        else if (aux->data1 == BANK_LSB && sourceData2 == (unsigned char)sourceLsb)
                        {
                            aux->data2 = (unsigned char)targetLsb;
                            changed = TRUE;
                        }
                    }
                }
                else if (eventType == PROGRAM_CHANGE && aux->dataBytes >= 1)
                {
                    bool channelFallbackAllowed;

                    channelFallbackAllowed = ((((remapChannelMask & (uint16_t)(1u << channel)) != 0)
                                               && (lastBankMsbIndex[channel] == 0xFFFFFFFFu)
                                               && (lastBankLsbIndex[channel] == 0xFFFFFFFFu))
                                              ? TRUE
                                              : FALSE);

                    if (aux->data1 == sourceProgram
                        && (PV_BanksEquivalent(currentBank[channel], sourceBankMidi)
                            || channelFallbackAllowed))
                    {
                        aux->data1 = targetProgram;
                        changed = TRUE;

                        if (sourceBankMidi != targetBankMidi &&
                            !PV_BanksEquivalent(currentBank[channel], targetBankMidi))
                        {
                            BAEResult insertResult;
                            bool needMsb;
                            bool needLsb;

                            needMsb = (lastBankMsbIndex[channel] == 0xFFFFFFFFu) ? TRUE : FALSE;
                            needLsb = (lastBankLsbIndex[channel] == 0xFFFFFFFFu) ? TRUE : FALSE;

                            if (!needMsb)
                            {
                                track->auxEvents[lastBankMsbIndex[channel]].data2 = (unsigned char)targetMsb;
                                changed = TRUE;
                            }
                            if (!needLsb)
                            {
                                track->auxEvents[lastBankLsbIndex[channel]].data2 = (unsigned char)targetLsb;
                                changed = TRUE;
                            }

                            if (needMsb || needLsb)
                            {
                                insertResult = PV_InsertBankSelectBeforeAuxEvent(track,
                                                                                  refs[i].index,
                                                                                  channel,
                                                                                  targetBankMidi,
                                                                                  needMsb,
                                                                                  needLsb);
                                if (insertResult != BAE_NO_ERROR)
                                {
                                    XDisposePtr(refs);
                                    return insertResult;
                                }
                                changed = TRUE;
                                /* Indices/orders shifted — rescan from scratch. */
                                restartScan = TRUE;
                                break;
                            }
                        }
                    }
                }
            }
        }

        XDisposePtr(refs);
    } while (restartScan);

    if (outChanged)
    {
        *outChanged = changed;
    }
    return BAE_NO_ERROR;
}


/* Keep short (0..127 group) vs MIDI ((MSB<<7)|LSB) encoding of the old value. */
uint16_t PV_RemapBankPreserveForm(uint16_t oldBank, uint16_t targetBank)
{
    uint16_t targetMsb;
    uint16_t targetLsb;

    if (oldBank < 128)
    {
        return PV_BankGroupFromInternalBank(targetBank);
    }
    PV_BankToMidiMsbLsb(targetBank, &targetMsb, &targetLsb);
    return (uint16_t)((targetMsb << 7) | targetLsb);
}


BAEResult PV_AddTrimHardStopEventsToTrack(BAERmfEditorTrack *track,
                                                 uint32_t stopTick)
{
    unsigned char usedChannels[16];
    uint32_t i;

    if (!track)
    {
        return BAE_PARAM_ERR;
    }

    XSetMemory(usedChannels, sizeof(usedChannels), 0);

    if (track->channel < 16)
    {
        usedChannels[track->channel] = 1;
    }

    for (i = 0; i < track->noteCount; ++i)
    {
        if (track->notes[i].channel < 16)
        {
            usedChannels[track->notes[i].channel] = 1;
        }
    }

    for (i = 0; i < track->auxEventCount; ++i)
    {
        unsigned char status;
        unsigned char type;
        unsigned char channel;

        status = track->auxEvents[i].status;
        type = (unsigned char)(status & 0xF0);
        channel = (unsigned char)(status & 0x0F);

        if ((type >= 0x80 && type <= 0xE0) && channel < 16)
        {
            usedChannels[channel] = 1;
        }
    }

    for (i = 0; i < 16; ++i)
    {
        BAEResult result;
        unsigned char status;

        if (!usedChannels[i])
        {
            continue;
        }

        status = (unsigned char)(0xB0 | (unsigned char)i);

        result = PV_AddAuxEventToTrack(track, stopTick, status, 64, 0, 2); /* sustain off */
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        result = PV_AddAuxEventToTrack(track, stopTick, status, 66, 0, 2); /* sostenuto off */
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        result = PV_AddAuxEventToTrack(track, stopTick, status, 67, 0, 2); /* soft pedal off */
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        result = PV_AddAuxEventToTrack(track, stopTick, status, 123, 0, 2); /* all notes off */
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        result = PV_AddAuxEventToTrack(track, stopTick, status, 120, 0, 2); /* all sound off */
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        result = PV_AddAuxEventToTrack(track, stopTick, status, 121, 0, 2); /* reset all controllers */
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }

    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_AddSampleFromFile(BAERmfEditorDocument *document,
                                                 BAEPathName filePath,
                                                 BAERmfEditorSampleSetup const *setup,
                                                 BAESampleInfo *outSampleInfo)
{
    BAERmfEditorSample *sample;
    BAEFileType fileType;
    AudioFileType audioFileType;
    XFILENAME fileName;
    GM_Waveform *waveform;
    GM_Waveform *compressedWaveform;
    bool isCompressedImport;
    SndCompressionType sourceCompressionType;
    XPTR encodedData;
    int32_t encodedSize;
    XPTR passthroughSndData;
    int32_t passthroughSndSize;
    OPErr opErr;
    BAEResult result;
    uint32_t index;

    if (!document || !filePath || !setup)
    {
        return BAE_PARAM_ERR;
    }
    if (setup->program >= 128)
    {
        return BAE_PARAM_ERR;
    }
    for (index = 0; index < document->sampleCount; ++index)
    {
        if (document->samples[index].program == setup->program)
        {
            return BAE_ALREADY_EXISTS;
        }
    }
    fileType = PV_DetermineEditorImportFileType(filePath);
    if (PV_TranslateEditorFileType(fileType) == FILE_INVALID_TYPE)
    {
        return BAE_BAD_FILE_TYPE;
    }
    audioFileType = PV_TranslateEditorFileType(fileType);
    if (audioFileType == FILE_INVALID_TYPE)
    {
        return BAE_BAD_FILE_TYPE;
    }
    XConvertPathToXFILENAME(filePath, &fileName);
    waveform = GM_ReadFileIntoMemory(&fileName, audioFileType, TRUE, &opErr);
    if (!waveform || opErr != NO_ERR)
    {
        if (waveform)
        {
            GM_FreeWaveform(waveform);
        }
        return BAE_BAD_FILE;
    }
    waveform->baseMidiPitch = setup->rootKey;

    compressedWaveform = NULL;
    isCompressedImport = PV_IsEditorCompressedImportType(fileType);
    sourceCompressionType = C_NONE;
    encodedData = NULL;
    encodedSize = 0;
    passthroughSndData = NULL;
    passthroughSndSize = 0;
    if (isCompressedImport)
    {
        if (PV_CompressionTypeFromEditorFileType(fileType, &sourceCompressionType) != BAE_NO_ERROR)
        {
            GM_FreeWaveform(waveform);
            return BAE_BAD_FILE_TYPE;
        }

#if USE_MPEG_DECODER == TRUE || USE_MPEG_ENCODER == TRUE
        if (fileType == BAE_MPEG_TYPE)
        {
            compressedWaveform = GM_ReadFileIntoMemory(&fileName, audioFileType, FALSE, &opErr);
            if (!compressedWaveform || opErr != NO_ERR)
            {
                if (compressedWaveform)
                {
                    GM_FreeWaveform(compressedWaveform);
                }
                GM_FreeWaveform(waveform);
                return BAE_BAD_FILE;
            }
            result = PV_CreatePassthroughSndFromCompressedWaveform(waveform,
                                                                    compressedWaveform,
                                                                    CS_DEFAULT,
                                                                    &passthroughSndData,
                                                                    &passthroughSndSize);
            GM_FreeWaveform(compressedWaveform);
        }
        else
#endif
        {
            result = PV_ReadFileIntoMemory(&fileName, &encodedData, &encodedSize);
            if (result == BAE_NO_ERROR)
            {
                result = PV_CreatePassthroughSndFromEncodedData(waveform,
                                                                encodedData,
                                                                encodedSize,
                                                                sourceCompressionType,
                                                                CS_DEFAULT,
                                                                &passthroughSndData,
                                                                &passthroughSndSize);
            }
            if (encodedData)
            {
                XDisposePtr(encodedData);
                encodedData = NULL;
            }
        }
        if (result != BAE_NO_ERROR)
        {
            GM_FreeWaveform(waveform);
            return result;
        }
    }

    result = PV_GrowBuffer((void **)&document->samples,
                           &document->sampleCapacity,
                           sizeof(BAERmfEditorSample),
                           document->sampleCount + 1);
    if (result != BAE_NO_ERROR)
    {
        if (passthroughSndData)
        {
            XDisposePtr(passthroughSndData);
        }
        GM_FreeWaveform(waveform);
        return result;
    }
    sample = &document->samples[document->sampleCount];
    XSetMemory(sample, sizeof(*sample), 0);
    sample->waveform = waveform;
    sample->program = setup->program;
    sample->sampleAssetID = PV_AllocateSampleAssetID(document);
    sample->rootKey = setup->rootKey;
    sample->lowKey = setup->lowKey;
    sample->highKey = setup->highKey;
    sample->sourceCompressionType = isCompressedImport ? (uint32_t)sourceCompressionType
                                                       : waveform->compressionType;
    sample->sourceCompressionSubType = CS_DEFAULT;
    sample->targetCompressionType = isCompressedImport ? BAE_EDITOR_COMPRESSION_DONT_CHANGE : BAE_EDITOR_COMPRESSION_PCM;
    sample->originalSndData = passthroughSndData;
    sample->originalSndSize = passthroughSndSize;
    sample->displayName = PV_DuplicateString(setup->displayName ? setup->displayName : filePath);
    sample->sourcePath = PV_DuplicateString(filePath);
    if (!sample->displayName || !sample->sourcePath)
    {
        if (sample->originalSndData)
        {
            XDisposePtr(sample->originalSndData);
            sample->originalSndData = NULL;
            sample->originalSndSize = 0;
        }
        PV_FreeString(&sample->displayName);
        PV_FreeString(&sample->sourcePath);
        GM_FreeWaveform(waveform);
        XSetMemory(sample, sizeof(*sample), 0);
        return BAE_MEMORY_ERR;
    }
    sample->sampleInfo.bitSize = waveform->bitSize;
    sample->sampleInfo.channels = waveform->channels;
    sample->sampleInfo.baseMidiPitch = waveform->baseMidiPitch;
    sample->sampleInfo.waveSize = waveform->waveSize;
    sample->sampleInfo.waveFrames = waveform->waveFrames;
    sample->sampleInfo.startLoop = waveform->startLoop;
    sample->sampleInfo.endLoop = waveform->endLoop;
    sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;
    if (outSampleInfo)
    {
        *outSampleInfo = sample->sampleInfo;
    }
    document->sampleCount++;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_AddEmptySample(BAERmfEditorDocument *document,
                                              BAERmfEditorSampleSetup const *setup,
                                              uint32_t *outSampleIndex,
                                              BAESampleInfo *outSampleInfo)
{
    BAERmfEditorSample *sample;
    BAEResult result;
    GM_Waveform *waveform;
    int16_t *pcm;

    if (!document || !setup)
    {
        return BAE_PARAM_ERR;
    }
    if (setup->program >= 128 || setup->rootKey > 127 || setup->lowKey > 127 || setup->highKey > 127 || setup->lowKey > setup->highKey)
    {
        return BAE_PARAM_ERR;
    }

    waveform = (GM_Waveform *)XNewPtr((int32_t)sizeof(GM_Waveform));
    if (!waveform)
    {
        return BAE_MEMORY_ERR;
    }
    XSetMemory(waveform, sizeof(*waveform), 0);
    pcm = (int16_t *)XNewPtr((int32_t)sizeof(int16_t));
    if (!pcm)
    {
        XDisposePtr((XPTR)waveform);
        return BAE_MEMORY_ERR;
    }
    pcm[0] = 0;
    waveform->theWaveform = (signed char *)pcm;
    waveform->waveFrames = 1;
    waveform->waveSize = sizeof(int16_t);
    waveform->bitSize = 16;
    waveform->channels = 1;
    waveform->sampledRate = 22050L << 16;
    waveform->baseMidiPitch = setup->rootKey;
    waveform->startLoop = 0;
    waveform->endLoop = 0;
    waveform->compressionType = C_NONE;

    result = PV_GrowBuffer((void **)&document->samples,
                           &document->sampleCapacity,
                           sizeof(BAERmfEditorSample),
                           document->sampleCount + 1);
    if (result != BAE_NO_ERROR)
    {
        GM_FreeWaveform(waveform);
        return result;
    }

    sample = &document->samples[document->sampleCount];
    XSetMemory(sample, sizeof(*sample), 0);
    sample->waveform = waveform;
    sample->program = setup->program;
    sample->sampleAssetID = PV_AllocateSampleAssetID(document);
    sample->rootKey = setup->rootKey;
    sample->lowKey = setup->lowKey;
    sample->highKey = setup->highKey;
    sample->sourceCompressionType = C_NONE;
    sample->sourceCompressionSubType = CS_DEFAULT;
    sample->targetCompressionType = BAE_EDITOR_COMPRESSION_PCM;
    sample->targetOpusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
    sample->originalSndData = NULL;
    sample->originalSndSize = 0;
    sample->instID = BAE_EDITOR_INST_ID_NONE;
    sample->displayName = PV_DuplicateString(setup->displayName ? setup->displayName : "New Instrument");
    sample->sourcePath = NULL;
    if (!sample->displayName)
    {
        GM_FreeWaveform(waveform);
        XSetMemory(sample, sizeof(*sample), 0);
        return BAE_MEMORY_ERR;
    }

    sample->sampleInfo.bitSize = waveform->bitSize;
    sample->sampleInfo.channels = waveform->channels;
    sample->sampleInfo.baseMidiPitch = waveform->baseMidiPitch;
    sample->sampleInfo.waveSize = waveform->waveSize;
    sample->sampleInfo.waveFrames = waveform->waveFrames;
    sample->sampleInfo.startLoop = waveform->startLoop;
    sample->sampleInfo.endLoop = waveform->endLoop;
    sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;

    if (outSampleInfo)
    {
        *outSampleInfo = sample->sampleInfo;
    }
    if (outSampleIndex)
    {
        *outSampleIndex = document->sampleCount;
    }
    document->sampleCount++;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetSampleCount(BAERmfEditorDocument const *document,
                                              uint32_t *outSampleCount)
{
    if (!document || !outSampleCount)
    {
        return BAE_PARAM_ERR;
    }
    *outSampleCount = document->sampleCount;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetSampleInfo(BAERmfEditorDocument const *document,
                                             uint32_t sampleIndex,
                                             BAERmfEditorSampleInfo *outSampleInfo)
{
    BAERmfEditorSample const *sample;

    if (!document || !outSampleInfo || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    outSampleInfo->displayName = (sample->displayName && sample->displayName[0] != '\0') ? sample->displayName : "";
    outSampleInfo->sourcePath = sample->sourcePath;
    outSampleInfo->program = sample->program;
    outSampleInfo->rootKey = sample->rootKey;
    outSampleInfo->lowKey = sample->lowKey;
    outSampleInfo->highKey = sample->highKey;
    outSampleInfo->splitVolume = sample->splitVolume;
    outSampleInfo->sampleInfo = sample->sampleInfo;
    outSampleInfo->sampleSize = (uint32_t)sample->originalSndSize;
    outSampleInfo->compressionType = sample->targetCompressionType;
    outSampleInfo->hasOriginalData = (sample->originalSndData != NULL) ? TRUE : FALSE;
    outSampleInfo->opusMode = sample->targetOpusMode;
    outSampleInfo->opusRoundTripResample = sample->opusUseRoundTripResampling;
    switch (sample->originalSndResourceType)
    {
        case ID_CSND: outSampleInfo->sndStorageType = BAE_EDITOR_SND_STORAGE_CSND; break;
        case ID_SND:  outSampleInfo->sndStorageType = BAE_EDITOR_SND_STORAGE_SND;  break;
        default:      outSampleInfo->sndStorageType = BAE_EDITOR_SND_STORAGE_ESND; break;
    }
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetRecommendedSampleRate(BAERmfEditorDocument const *document,
                                                        uint32_t sampleIndex,
                                                        BAERmfEditorCompressionType compressionType,
                                                        BAE_UNSIGNED_FIXED *outSampleRate)
{
    BAERmfEditorSample const *sample;

    if (!document || !outSampleRate || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }

    sample = &document->samples[sampleIndex];
    *outSampleRate = PV_RecommendSampleRateForCompression(sample, compressionType);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetSampleAssetIDForSample(BAERmfEditorDocument const *document,
                                                         uint32_t sampleIndex,
                                                         uint32_t *outAssetID)
{
    if (!document || !outAssetID || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    *outAssetID = document->samples[sampleIndex].sampleAssetID;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetSampleInstID(BAERmfEditorDocument *document,
                                               uint32_t sampleIndex,
                                               uint32_t instID)
{
    if (!document || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    document->samples[sampleIndex].instID = instID;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetSampleAssetCount(BAERmfEditorDocument const *document,
                                                   uint32_t *outAssetCount)
{
    uint32_t i;
    uint32_t count;

    if (!document || !outAssetCount)
    {
        return BAE_PARAM_ERR;
    }
    count = 0;
    for (i = 0; i < document->sampleCount; ++i)
    {
        uint32_t prior;
        uint32_t assetID;

        assetID = document->samples[i].sampleAssetID;
        if (assetID == BAE_EDITOR_SAMPLE_ASSET_ID_NONE)
        {
            continue;
        }
        for (prior = 0; prior < i; ++prior)
        {
            if (document->samples[prior].sampleAssetID == assetID)
            {
                break;
            }
        }
        if (prior == i)
        {
            ++count;
        }
    }
    *outAssetCount = count;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetSampleAssetInfo(BAERmfEditorDocument const *document,
                                                  uint32_t assetIndex,
                                                  BAERmfEditorSampleAssetInfo *outAssetInfo)
{
    uint32_t i;
    uint32_t uniqueIndex;

    if (!document || !outAssetInfo)
    {
        return BAE_PARAM_ERR;
    }

    uniqueIndex = 0;
    for (i = 0; i < document->sampleCount; ++i)
    {
        uint32_t prior;
        BAERmfEditorSample const *sample;

        sample = &document->samples[i];
        if (sample->sampleAssetID == BAE_EDITOR_SAMPLE_ASSET_ID_NONE)
        {
            continue;
        }
        for (prior = 0; prior < i; ++prior)
        {
            if (document->samples[prior].sampleAssetID == sample->sampleAssetID)
            {
                break;
            }
        }
        if (prior < i)
        {
            continue;
        }
        if (uniqueIndex == assetIndex)
        {
            outAssetInfo->assetID = sample->sampleAssetID;
            outAssetInfo->displayName = sample->displayName;
            outAssetInfo->sourcePath = sample->sourcePath;
            outAssetInfo->compressionType = sample->targetCompressionType;
            outAssetInfo->opusMode = sample->targetOpusMode;
            outAssetInfo->hasOriginalData = PV_AssetSupportsDontChange(document, sample->sampleAssetID);
            outAssetInfo->usageCount = PV_CountSamplesForAsset(document, sample->sampleAssetID);
            return BAE_NO_ERROR;
        }
        ++uniqueIndex;
    }

    return BAE_PARAM_ERR;
}


BAEResult BAERmfEditorDocument_GetSampleAssetUsageCount(BAERmfEditorDocument const *document,
                                                        uint32_t assetID,
                                                        uint32_t *outUsageCount)
{
    uint32_t usageCount;

    if (!document || !outUsageCount || assetID == BAE_EDITOR_SAMPLE_ASSET_ID_NONE)
    {
        return BAE_PARAM_ERR;
    }
    usageCount = PV_CountSamplesForAsset(document, assetID);
    if (usageCount == 0)
    {
        return BAE_PARAM_ERR;
    }
    *outUsageCount = usageCount;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetSampleAssetSampleIndex(BAERmfEditorDocument const *document,
                                                         uint32_t assetID,
                                                         uint32_t usageIndex,
                                                         uint32_t *outSampleIndex)
{
    uint32_t i;
    uint32_t hit;

    if (!document || !outSampleIndex || assetID == BAE_EDITOR_SAMPLE_ASSET_ID_NONE)
    {
        return BAE_PARAM_ERR;
    }
    hit = 0;
    for (i = 0; i < document->sampleCount; ++i)
    {
        if (document->samples[i].sampleAssetID != assetID)
        {
            continue;
        }
        if (hit == usageIndex)
        {
            *outSampleIndex = i;
            return BAE_NO_ERROR;
        }
        ++hit;
    }
    return BAE_PARAM_ERR;
}


BAEResult BAERmfEditorDocument_SetSampleAssetCompression(BAERmfEditorDocument *document,
                                                         uint32_t assetID,
                                                         BAERmfEditorCompressionType compressionType)
{
    uint32_t i;
    BAERmfEditorCompressionType resolvedType;
    bool touched;

    if (!document || assetID == BAE_EDITOR_SAMPLE_ASSET_ID_NONE)
    {
        return BAE_PARAM_ERR;
    }

    resolvedType = compressionType;
    if (compressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE &&
        !PV_AssetSupportsDontChange(document, assetID))
    {
        resolvedType = BAE_EDITOR_COMPRESSION_PCM;
    }

    touched = FALSE;
    for (i = 0; i < document->sampleCount; ++i)
    {
        BAERmfEditorSample *sample;

        sample = &document->samples[i];
        if (sample->sampleAssetID != assetID)
        {
            continue;
        }
        sample->targetCompressionType = resolvedType;
        touched = TRUE;
    }
    if (!touched)
    {
        return BAE_PARAM_ERR;
    }

    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetSampleAssetForSample(BAERmfEditorDocument *document,
                                                       uint32_t sampleIndex,
                                                       uint32_t assetID)
{
    BAERmfEditorSample *sample;
    BAERmfEditorSample *sourceAssetSample;
    BAERmfEditorCompressionType sourceCompression;

    if (!document || sampleIndex >= document->sampleCount ||
        assetID == BAE_EDITOR_SAMPLE_ASSET_ID_NONE)
    {
        return BAE_PARAM_ERR;
    }

    sourceAssetSample = PV_FindFirstSampleForAsset(document, assetID);
    if (!sourceAssetSample)
    {
        return BAE_PARAM_ERR;
    }

    sample = &document->samples[sampleIndex];
    sample->sampleAssetID = assetID;
    sourceCompression = sourceAssetSample->targetCompressionType;
    if (sourceCompression == BAE_EDITOR_COMPRESSION_DONT_CHANGE &&
        !PV_AssetSupportsDontChange(document, assetID))
    {
        sourceCompression = BAE_EDITOR_COMPRESSION_PCM;
    }
    sample->targetCompressionType = sourceCompression;
    sample->targetOpusMode = sourceAssetSample->targetOpusMode;
    sample->opusUseRoundTripResampling = sourceAssetSample->opusUseRoundTripResampling;

    BAERmfEditorDocument_SetSampleAssetCompression(document, assetID, sourceCompression);

    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_CloneSampleAssetForSample(BAERmfEditorDocument *document,
                                                         uint32_t sampleIndex,
                                                         uint32_t *outNewAssetID)
{
    BAERmfEditorSample *sample;
    uint32_t newAssetID;

    if (!document || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }

    sample = &document->samples[sampleIndex];
    newAssetID = PV_AllocateSampleAssetID(document);
    sample->sampleAssetID = newAssetID;

    if (outNewAssetID)
    {
        *outNewAssetID = newAssetID;
    }
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetSampleInfo(BAERmfEditorDocument *document,
                                             uint32_t sampleIndex,
                                             BAERmfEditorSampleInfo const *sampleInfo)
{
    BAERmfEditorSample *sample;
    BAEResult result;
    bool loopChanged;
    BAE_UNSIGNED_FIXED newSampleRate;
    BAE_UNSIGNED_FIXED incomingSampleRate;
    BAE_UNSIGNED_FIXED oldSampleRate;
    unsigned char oldProgram;
    uint32_t oldInstID;

    if (!document || !sampleInfo || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    if (sampleInfo->program >= 128 ||
        sampleInfo->rootKey > 127 ||
        sampleInfo->lowKey > 127 ||
        sampleInfo->highKey > 127 ||
        sampleInfo->lowKey > sampleInfo->highKey)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    oldProgram = sample->program;
    oldInstID = sample->instID;
    loopChanged = (sample->sampleInfo.startLoop != sampleInfo->sampleInfo.startLoop) ||
                  (sample->sampleInfo.endLoop != sampleInfo->sampleInfo.endLoop);
    result = PV_SetDocumentString(&sample->displayName, sampleInfo->displayName);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    sample->program = sampleInfo->program;

    if (oldInstID != BAE_EDITOR_INST_ID_NONE && sampleInfo->program != oldProgram)
    {
        uint32_t newInstID;
        uint32_t i;

        /* Preserve the instrument bank namespace and swap only the low 7-bit program. */
        newInstID = (oldInstID & ~127U) | (uint32_t)sampleInfo->program;
        if (newInstID != oldInstID)
        {
            PV_EnsureInstrumentExtForRemappedID(document,
                                               (XLongResourceID)oldInstID,
                                               (XLongResourceID)newInstID);
            for (i = 0; i < document->sampleCount; ++i)
            {
                if (document->samples[i].instID == oldInstID)
                {
                    document->samples[i].instID = newInstID;
                    if (document->samples[i].program == oldProgram)
                    {
                        document->samples[i].program = sampleInfo->program;
                    }
                }
            }
            sample = &document->samples[sampleIndex];
        }
    }

    sample->rootKey = sampleInfo->rootKey;
    sample->lowKey = sampleInfo->lowKey;
    sample->highKey = sampleInfo->highKey;
    sample->splitVolume = sampleInfo->splitVolume;
    if (sampleInfo->sndStorageType == ID_SND ||
        sampleInfo->sndStorageType == ID_ESND ||
        sampleInfo->sndStorageType == ID_CSND)
    {
        sample->originalSndResourceType = sampleInfo->sndStorageType;
    }

    incomingSampleRate = sampleInfo->sampleInfo.sampledRate;
    oldSampleRate = sample->sampleInfo.sampledRate;
    newSampleRate = incomingSampleRate;
    if (newSampleRate == 0)
    {
        newSampleRate = oldSampleRate;
    }
    else if (newSampleRate >= 1000U && newSampleRate <= 384000U)
    {
        /* Accept plain-Hz values from UI callers and normalize to 16.16 fixed. */
        newSampleRate <<= 16;
    }
    /* Otherwise assume caller already provided 16.16 fixed-point rate and keep it as-is. */

    /* Keep per-sample metadata in sampleInfo. The waveform may be shared by
     * multiple splits, so writing loop/rate there can leak edits across samples. */
    if (sample->waveform)
    {
        sample->waveform->baseMidiPitch = sampleInfo->rootKey;
    }
    sample->sampleInfo.baseMidiPitch = sampleInfo->rootKey;
    sample->sampleInfo.startLoop     = sampleInfo->sampleInfo.startLoop;
    sample->sampleInfo.endLoop       = sampleInfo->sampleInfo.endLoop;
    sample->sampleInfo.sampledRate   = newSampleRate;

    if (loopChanged && sample->originalSndData)
    {
        XDisposePtr(sample->originalSndData);
        sample->originalSndData = NULL;
        sample->originalSndSize = 0;
    }

    /* Validate and store compression type.
     * DONT_CHANGE is only legal when we have the original compressed blob. */
    if (sampleInfo->compressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE &&
        !PV_AssetSupportsDontChange(document, sample->sampleAssetID))
    {
        BAERmfEditorDocument_SetSampleAssetCompression(document,
                                                       sample->sampleAssetID,
                                                       BAE_EDITOR_COMPRESSION_PCM);
    }
    else
    {
        BAERmfEditorDocument_SetSampleAssetCompression(document,
                                                       sample->sampleAssetID,
                                                       sampleInfo->compressionType);
    }

    {
        BAERmfEditorOpusMode resolvedOpusMode;
        uint32_t i;

        resolvedOpusMode = sampleInfo->opusMode;
        if (!PV_IsValidEditorOpusMode(resolvedOpusMode))
        {
            resolvedOpusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
        }
        for (i = 0; i < document->sampleCount; ++i)
        {
            if (document->samples[i].sampleAssetID == sample->sampleAssetID)
            {
                document->samples[i].targetOpusMode = resolvedOpusMode;
                document->samples[i].opusUseRoundTripResampling = sampleInfo->opusRoundTripResample;
            }
        }
    }

    switch (sampleInfo->sndStorageType)
    {
        case BAE_EDITOR_SND_STORAGE_CSND: sample->originalSndResourceType = ID_CSND; break;
        case BAE_EDITOR_SND_STORAGE_SND:  sample->originalSndResourceType = ID_SND;  break;
        default:                          sample->originalSndResourceType = ID_ESND; break;
    }
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_DeleteSample(BAERmfEditorDocument *document,
                                            uint32_t sampleIndex)
{
    BAERmfEditorSample *sample;

    if (!document || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    PV_FreeString(&sample->displayName);
    PV_FreeString(&sample->sourcePath);
    if (sample->waveform)
    {
        GM_FreeWaveform(sample->waveform);
        sample->waveform = NULL;
    }
    if (sample->originalSndData)
    {
        XDisposePtr(sample->originalSndData);
        sample->originalSndData = NULL;
        sample->originalSndSize = 0;
    }
    if (sampleIndex + 1 < document->sampleCount)
    {
        XBlockMove(&document->samples[sampleIndex + 1],
                   &document->samples[sampleIndex],
                   (int32_t)((document->sampleCount - (sampleIndex + 1)) * sizeof(BAERmfEditorSample)));
    }
    document->sampleCount--;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_DeleteInstrument(BAERmfEditorDocument *document, uint32_t instID)
{
    uint32_t i;
    bool foundAny = FALSE;

    /* INST id 0 is a valid resource id on some banks — do not reject it. */
    if (!document)
    {
        return BAE_PARAM_ERR;
    }

    /* Delete samples matching instID */
    for (i = 0; i < document->sampleCount; )
    {
        if (document->samples[i].instID == instID)
        {
            (void)BAERmfEditorDocument_DeleteSample(document, i);
            foundAny = TRUE;
        }
        else
        {
            ++i;
        }
    }

    /* Delete instrumentExt matching instID */
    for (i = 0; i < document->instrumentExtCount; )
    {
        if ((uint32_t)document->instrumentExts[i].instID == instID)
        {
            PV_FreeString(&document->instrumentExts[i].displayName);
            if (document->instrumentExts[i].originalInstData)
            {
                XDisposePtr(document->instrumentExts[i].originalInstData);
                document->instrumentExts[i].originalInstData = NULL;
                document->instrumentExts[i].originalInstSize = 0;
            }
            if (i + 1 < document->instrumentExtCount)
            {
                XBlockMove(&document->instrumentExts[i + 1],
                           &document->instrumentExts[i],
                           (int32_t)((document->instrumentExtCount - (i + 1)) * sizeof(BAERmfEditorInstrumentExt)));
            }
            document->instrumentExtCount--;
            foundAny = TRUE;
        }
        else
        {
            ++i;
        }
    }

    /* Delete originalResource ID_INST entry matching instID */
    for (i = 0; i < document->originalResourceCount; )
    {
        if (document->originalResources[i].type == ID_INST &&
            (uint32_t)document->originalResources[i].id == instID)
        {
            if (document->originalResources[i].data)
            {
                XDisposePtr(document->originalResources[i].data);
            }
            if (i + 1 < document->originalResourceCount)
            {
                XBlockMove(&document->originalResources[i + 1],
                           &document->originalResources[i],
                           (int32_t)((document->originalResourceCount - (i + 1)) * sizeof(BAERmfEditorResourceEntry)));
            }
            document->originalResourceCount--;
            foundAny = TRUE;
        }
        else
        {
            ++i;
        }
    }

    if (foundAny)
    {
        PV_MarkDocumentDirty(document);
    }
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_ReplaceSampleFromFile(BAERmfEditorDocument *document,
                                                     uint32_t sampleIndex,
                                                     BAEPathName filePath,
                                                     BAESampleInfo *outSampleInfo)
{
    BAERmfEditorSample *sample;
    BAEFileType fileType;
    AudioFileType audioFileType;
    XFILENAME fileName;
    GM_Waveform *waveform;
    GM_Waveform *compressedWaveform;
    bool isCompressedImport;
    SndCompressionType sourceCompressionType;
    XPTR encodedData;
    int32_t encodedSize;
    XPTR passthroughSndData;
    int32_t passthroughSndSize;
    OPErr opErr;
    BAEResult result;
    char *pathCopy;

    if (!document || !filePath || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    fileType = PV_DetermineEditorImportFileType(filePath);
    if (PV_TranslateEditorFileType(fileType) == FILE_INVALID_TYPE)
    {
        return BAE_BAD_FILE_TYPE;
    }
    audioFileType = PV_TranslateEditorFileType(fileType);
    if (audioFileType == FILE_INVALID_TYPE)
    {
        return BAE_BAD_FILE_TYPE;
    }
    XConvertPathToXFILENAME(filePath, &fileName);
    waveform = GM_ReadFileIntoMemory(&fileName, audioFileType, TRUE, &opErr);
    if (!waveform || opErr != NO_ERR)
    {
        if (waveform)
        {
            GM_FreeWaveform(waveform);
        }
        return BAE_BAD_FILE;
    }

    compressedWaveform = NULL;
    isCompressedImport = PV_IsEditorCompressedImportType(fileType);
    sourceCompressionType = C_NONE;
    encodedData = NULL;
    encodedSize = 0;
    passthroughSndData = NULL;
    passthroughSndSize = 0;
    if (isCompressedImport)
    {
        if (PV_CompressionTypeFromEditorFileType(fileType, &sourceCompressionType) != BAE_NO_ERROR)
        {
            GM_FreeWaveform(waveform);
            return BAE_BAD_FILE_TYPE;
        }

#if USE_MPEG_DECODER == TRUE || USE_MPEG_ENCODER == TRUE
        if (fileType == BAE_MPEG_TYPE)
        {
            compressedWaveform = GM_ReadFileIntoMemory(&fileName, audioFileType, FALSE, &opErr);
            if (!compressedWaveform || opErr != NO_ERR)
            {
                if (compressedWaveform)
                {
                    GM_FreeWaveform(compressedWaveform);
                }
                GM_FreeWaveform(waveform);
                return BAE_BAD_FILE;
            }
            result = PV_CreatePassthroughSndFromCompressedWaveform(waveform,
                                                                    compressedWaveform,
                                                                    CS_DEFAULT,
                                                                    &passthroughSndData,
                                                                    &passthroughSndSize);
            GM_FreeWaveform(compressedWaveform);
        }
        else
#endif
        {
            result = PV_ReadFileIntoMemory(&fileName, &encodedData, &encodedSize);
            if (result == BAE_NO_ERROR)
            {
                result = PV_CreatePassthroughSndFromEncodedData(waveform,
                                                                encodedData,
                                                                encodedSize,
                                                                sourceCompressionType,
                                                                CS_DEFAULT,
                                                                &passthroughSndData,
                                                                &passthroughSndSize);
            }
            if (encodedData)
            {
                XDisposePtr(encodedData);
                encodedData = NULL;
            }
        }
        if (result != BAE_NO_ERROR)
        {
            GM_FreeWaveform(waveform);
            return result;
        }
    }

    pathCopy = PV_DuplicateString(filePath);
    if (!pathCopy)
    {
        if (passthroughSndData)
        {
            XDisposePtr(passthroughSndData);
        }
        GM_FreeWaveform(waveform);
        return BAE_MEMORY_ERR;
    }

    sample = &document->samples[sampleIndex];
    /* Preserve the existing rootKey into the new waveform before swapping, so
     * the save path and preview always use the instrument's assigned root key
     * rather than whatever baseMidiPitch happened to be in the new audio file. */
    waveform->baseMidiPitch = sample->rootKey;
    if (sample->waveform)
    {
        GM_FreeWaveform(sample->waveform);
    }
    sample->waveform = waveform;
    PV_FreeString(&sample->sourcePath);
    sample->sourcePath = pathCopy;
    /* Replaced sample: clear original blob and default to RAW PCM. */
    if (sample->originalSndData)
    {
        XDisposePtr(sample->originalSndData);
        sample->originalSndData = NULL;
        sample->originalSndSize = 0;
    }
    sample->targetCompressionType = isCompressedImport ? BAE_EDITOR_COMPRESSION_DONT_CHANGE : BAE_EDITOR_COMPRESSION_PCM;
    sample->targetOpusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
    sample->originalSndData = passthroughSndData;
    sample->originalSndSize = passthroughSndSize;

    sample->sampleInfo.bitSize = waveform->bitSize;
    sample->sampleInfo.channels = waveform->channels;
    sample->sampleInfo.baseMidiPitch = sample->rootKey;
    sample->sampleInfo.waveSize = waveform->waveSize;
    sample->sampleInfo.waveFrames = waveform->waveFrames;
    sample->sampleInfo.startLoop = waveform->startLoop;
    sample->sampleInfo.endLoop = waveform->endLoop;
    sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;
    sample->sourceCompressionType = isCompressedImport ? (uint32_t)sourceCompressionType
                                                       : waveform->compressionType;
    sample->sourceCompressionSubType = CS_DEFAULT;
    if (outSampleInfo)
    {
        *outSampleInfo = sample->sampleInfo;
    }
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_ReplaceSampleFromPCM(BAERmfEditorDocument *document,
                                                    uint32_t sampleIndex,
                                                    void const *pcmData,
                                                    uint32_t frameCount,
                                                    uint16_t bitSize,
                                                    uint16_t channels,
                                                    BAE_UNSIGNED_FIXED sampledRate,
                                                    uint32_t startLoop,
                                                    uint32_t endLoop,
                                                    BAESampleInfo *outSampleInfo)
{
    BAERmfEditorSample *sample;
    GM_Waveform *waveform;
    XPTR pcmCopy;
    uint32_t bytesPerFrame;
    uint32_t waveSize;

    if (!document || sampleIndex >= document->sampleCount || !pcmData || frameCount == 0)
    {
        return BAE_PARAM_ERR;
    }
    if (!((bitSize == 8) || (bitSize == 16)) || !((channels == 1) || (channels == 2)))
    {
        return BAE_PARAM_ERR;
    }
    if (sampledRate == 0)
    {
        return BAE_PARAM_ERR;
    }

    bytesPerFrame = (uint32_t)channels * ((uint32_t)bitSize / 8u);
    if (bytesPerFrame == 0)
    {
        return BAE_PARAM_ERR;
    }
    waveSize = frameCount * bytesPerFrame;
    if (waveSize / bytesPerFrame != frameCount)
    {
        return BAE_PARAM_ERR;
    }

    waveform = (GM_Waveform *)XNewPtr((int32_t)sizeof(GM_Waveform));
    if (!waveform)
    {
        return BAE_MEMORY_ERR;
    }
    XSetMemory(waveform, sizeof(*waveform), 0);

    pcmCopy = XNewPtr((int32_t)waveSize);
    if (!pcmCopy)
    {
        XDisposePtr((XPTR)waveform);
        return BAE_MEMORY_ERR;
    }
    XBlockMove(pcmData, pcmCopy, (int32_t)waveSize);

    sample = &document->samples[sampleIndex];
    waveform->theWaveform = (signed char *)pcmCopy;
    waveform->waveFrames = frameCount;
    waveform->waveSize = (int32_t)waveSize;
    waveform->bitSize = bitSize;
    waveform->channels = channels;
    waveform->sampledRate = (int32_t)sampledRate;
    waveform->baseMidiPitch = sample->rootKey;
    waveform->compressionType = C_NONE;

    if (startLoop < endLoop && endLoop <= frameCount)
    {
        waveform->startLoop = startLoop;
        waveform->endLoop = endLoop;
    }
    else
    {
        waveform->startLoop = 0;
        waveform->endLoop = 0;
    }

    if (sample->waveform)
    {
        GM_FreeWaveform(sample->waveform);
    }
    sample->waveform = waveform;

    PV_FreeString(&sample->sourcePath);
    sample->sourcePath = NULL;

    if (sample->originalSndData)
    {
        XDisposePtr(sample->originalSndData);
        sample->originalSndData = NULL;
        sample->originalSndSize = 0;
    }

    sample->sourceCompressionType = C_NONE;
    sample->sourceCompressionSubType = CS_DEFAULT;
    sample->targetCompressionType = BAE_EDITOR_COMPRESSION_PCM;
    sample->targetOpusMode = BAE_EDITOR_OPUS_MODE_AUDIO;

    sample->sampleInfo.bitSize = waveform->bitSize;
    sample->sampleInfo.channels = waveform->channels;
    sample->sampleInfo.baseMidiPitch = sample->rootKey;
    sample->sampleInfo.waveSize = (uint32_t)waveform->waveSize;
    sample->sampleInfo.waveFrames = waveform->waveFrames;
    sample->sampleInfo.startLoop = waveform->startLoop;
    sample->sampleInfo.endLoop = waveform->endLoop;
    sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;

    if (outSampleInfo)
    {
        *outSampleInfo = sample->sampleInfo;
    }

    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_PropagateReplacementToAsset(BAERmfEditorDocument *document,
                                                           uint32_t sourceSampleIndex)
{
    BAERmfEditorSample *source;
    uint32_t assetID;
    uint32_t i;

    if (!document || sourceSampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    source = &document->samples[sourceSampleIndex];
    assetID = source->sampleAssetID;
    if (assetID == BAE_EDITOR_SAMPLE_ASSET_ID_NONE)
    {
        return BAE_NO_ERROR;
    }

    for (i = 0; i < document->sampleCount; ++i)
    {
        BAERmfEditorSample *dest;
        GM_Waveform *dupWaveform;
        XPTR waveData;

        if (i == sourceSampleIndex)
        {
            continue;
        }
        dest = &document->samples[i];
        if (dest->sampleAssetID != assetID)
        {
            continue;
        }

        /* Duplicate the waveform data */
        dupWaveform = GM_NewWaveform();
        if (!dupWaveform)
        {
            return BAE_MEMORY_ERR;
        }
        *dupWaveform = *source->waveform;
        /* Preserve the destination sample's own root key */
        dupWaveform->baseMidiPitch = dest->rootKey;
        if (source->waveform->theWaveform && source->waveform->waveSize > 0)
        {
            waveData = XNewPtr(source->waveform->waveSize);
            if (!waveData)
            {
                XDisposePtr((XPTR)dupWaveform);
                return BAE_MEMORY_ERR;
            }
            XBlockMove(source->waveform->theWaveform, waveData, source->waveform->waveSize);
            dupWaveform->theWaveform = (int16_t *)waveData;
        }
        else
        {
            dupWaveform->theWaveform = NULL;
        }

        /* Free old waveform and assign new */
        if (dest->waveform)
        {
            GM_FreeWaveform(dest->waveform);
        }
        dest->waveform = dupWaveform;

        /* Update source path */
        PV_FreeString(&dest->sourcePath);
        dest->sourcePath = PV_DuplicateString(source->sourcePath);

        /* Update audio properties (preserve per-split root/key/volume) */
        dest->sampleInfo.bitSize = source->sampleInfo.bitSize;
        dest->sampleInfo.channels = source->sampleInfo.channels;
        dest->sampleInfo.waveSize = source->sampleInfo.waveSize;
        dest->sampleInfo.waveFrames = source->sampleInfo.waveFrames;
        dest->sampleInfo.startLoop = source->sampleInfo.startLoop;
        dest->sampleInfo.endLoop = source->sampleInfo.endLoop;
        dest->sampleInfo.sampledRate = source->sampleInfo.sampledRate;

        /* Clear old SND data and copy compression settings */
        if (dest->originalSndData)
        {
            XDisposePtr(dest->originalSndData);
            dest->originalSndData = NULL;
            dest->originalSndSize = 0;
        }
        if (source->originalSndData && source->originalSndSize > 0)
        {
            dest->originalSndData = XNewPtr(source->originalSndSize);
            if (dest->originalSndData)
            {
                XBlockMove(source->originalSndData, dest->originalSndData, source->originalSndSize);
                dest->originalSndSize = source->originalSndSize;
            }
        }
        dest->targetCompressionType = source->targetCompressionType;
        dest->targetOpusMode = source->targetOpusMode;
        dest->opusUseRoundTripResampling = source->opusUseRoundTripResampling;
        dest->sourceCompressionType = source->sourceCompressionType;
        dest->sourceCompressionSubType = source->sourceCompressionSubType;
    }
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetSampleWaveformData(BAERmfEditorDocument const *document,
                                                     uint32_t sampleIndex,
                                                     void const **outWaveData,
                                                     uint32_t *outFrameCount,
                                                     uint16_t *outBitSize,
                                                     uint16_t *outChannels,
                                                     BAE_UNSIGNED_FIXED *outSampleRate)
{
    BAERmfEditorSample const *sample;
    GM_Waveform const *waveform;
    uint32_t frameCount;
    uint32_t bytesPerFrame;

    if (!document || !outWaveData || !outFrameCount || !outBitSize || !outChannels || !outSampleRate)
    {
        return BAE_PARAM_ERR;
    }
    if (sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }

    sample = &document->samples[sampleIndex];
    waveform = sample->waveform;
    if (!waveform || !waveform->theWaveform || waveform->bitSize == 0 || waveform->channels == 0)
    {
        return BAE_BAD_FILE;
    }

    bytesPerFrame = (uint32_t)(waveform->channels * (waveform->bitSize / 8));
    frameCount = waveform->waveFrames;
    if (frameCount == 0 && bytesPerFrame > 0)
    {
        frameCount = (uint32_t)(waveform->waveSize / bytesPerFrame);
    }

    *outWaveData = (void const *)waveform->theWaveform;
    *outFrameCount = frameCount;
    *outBitSize = waveform->bitSize;
    *outChannels = waveform->channels;
    *outSampleRate = (BAE_UNSIGNED_FIXED)waveform->sampledRate;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetSampleCodecDescription(BAERmfEditorDocument const *document,
                                                         uint32_t sampleIndex,
                                                         char *outCodec,
                                                         uint32_t outCodecSize)
{
    BAERmfEditorSample const *sample;
    uint16_t bitSize;

    if (!document || !outCodec || outCodecSize == 0 || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    bitSize = (sample->waveform) ? sample->waveform->bitSize : 0;
    outCodec[0] = 0;
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE    
    if (sample->sourceCompressionType == (uint32_t)C_VORBIS)
    {
        switch ((SndCompressionSubType)sample->sourceCompressionSubType)
        {
            case CS_VORBIS_32K:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis 32k");
                break;
            case CS_VORBIS_48K:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis 48k");
                break;
            case CS_VORBIS_64K:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis 64k");
                break;
            case CS_VORBIS_80K:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis 80k");
                break;
            case CS_VORBIS_96K:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis 96k");
                break;
            case CS_VORBIS_128K:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis 128k");
                break;
            case CS_VORBIS_160K:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis 160k");
                break;
            case CS_VORBIS_192K:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis 192k");
                break;
            case CS_VORBIS_256K:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis 256k");
                break;
            default:
                PV_CopyStringBounded(outCodec, outCodecSize, "Ogg Vorbis");
                break;
        }
    } else
#endif    
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
    if (sample->sourceCompressionType == (uint32_t)C_OPUS)
    {
        const char *rt = (sample->opusUseRoundTripResampling) ? " RT" : "";
        switch ((SndCompressionSubType)sample->sourceCompressionSubType)
        {
            case CS_OPUS_12K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 12k", rt);
                break;
            case CS_OPUS_16K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 16k", rt);
                break;
            case CS_OPUS_24K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 24k", rt);
                break;
            case CS_OPUS_32K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 32k", rt);
                break;
            case CS_OPUS_48K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 48k", rt);
                break;
            case CS_OPUS_64K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 64k", rt);
                break;
            case CS_OPUS_80K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 80k", rt);
                break;
            case CS_OPUS_96K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 96k", rt);
                break;
            case CS_OPUS_128K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 128k", rt);
                break;
            case CS_OPUS_160K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 160k", rt);
                break;
            case CS_OPUS_192K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 192k", rt);
                break;
            case CS_OPUS_256K:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s 256k", rt);
                break;
            default:
                snprintf(outCodec, outCodecSize, "Ogg Opus%s", rt);
                break;
        }
    } else
#endif
#if USE_QOA_SUPPORT == TRUE
    if (sample->sourceCompressionType == (uint32_t)C_QOA)
    {
        PV_CopyStringBounded(outCodec, outCodecSize, "QOA");
    } else
#endif
    if (sample->sourceCompressionType == (uint32_t)C_IMA2)
    {
        PV_CopyStringBounded(outCodec, outCodecSize, "2-bit ADPCM");
    } else
    {
        if ((sample->sourceCompressionType == (uint32_t)C_NONE || sample->sourceCompressionType == 0)
            && (bitSize == 8 || bitSize == 16))
        {
            snprintf(outCodec, outCodecSize, "PCM %u-bit", (unsigned)bitSize);
        }
        else
        {
            XGetCompressionName((int32_t)sample->sourceCompressionType, outCodec);
            if ((bitSize == 8 || bitSize == 16)
                && (!XStrCmp(outCodec, "PCM") || !XStrCmp(outCodec, "PCM (raw)")))
            {
                snprintf(outCodec, outCodecSize, "PCM %u-bit", (unsigned)bitSize);
            }
        }
    }
    if (outCodec[0] == 0)
    {
        PV_CopyStringBounded(outCodec, outCodecSize, "Unknown");
    }
    if (XStrLen(outCodec) >= (int32_t)outCodecSize)
    {
        outCodec[outCodecSize - 1] = 0;
    }
    return BAE_NO_ERROR;
}


/* Write the compressed payload from an XSndHeader3 SND blob, or BAE_NOT_SETUP
 * if the blob is not a passthrough Type-3 compressed sample. */
BAEResult PV_ExportSndBitstreamToFile(XPTR sndData,
                                             int32_t sndSize,
                                             SndCompressionType srcCodec,
                                             BAEPathName filePath)
{
    XSndHeader3 const *hdr3;
    int32_t bitstreamSize;
    unsigned char const *bitstream;
    unsigned char const *blobEnd;
    XFILENAME fileName;
    XFILE outFile;
    int32_t writeErr;

    if (!sndData || !filePath || sndSize <= (int32_t)sizeof(XSndHeader3))
    {
        return BAE_PARAM_ERR;
    }
    if (!PV_IsBitstreamPassthroughCodec(srcCodec))
    {
        return BAE_NOT_SETUP;
    }

    hdr3 = (XSndHeader3 const *)sndData;
    if (XGetShort(&hdr3->type) != XThirdSoundFormat)
    {
        return BAE_NOT_SETUP;
    }

    bitstreamSize = XGetLong(&hdr3->sndBuffer.encodedBytes);
    bitstream = (unsigned char const *)&hdr3->sndBuffer.sampleArea[0];
    blobEnd = (unsigned char const *)sndData + sndSize;
    if (bitstreamSize <= 0 || bitstream + bitstreamSize > blobEnd)
    {
        return BAE_BAD_FILE;
    }

    XConvertPathToXFILENAME(filePath, &fileName);
    outFile = XFileOpenForWrite(&fileName, TRUE);
    if (!outFile)
    {
        return BAE_FILE_IO_ERROR;
    }
    writeErr = XFileWrite(outFile, (XPTRC)bitstream, bitstreamSize);
    XFileClose(outFile);
    return (writeErr == 0) ? BAE_NO_ERROR : BAE_FILE_IO_ERROR;
}


BAEResult BAERmfEditorDocument_ExportSampleToFile(BAERmfEditorDocument const *document,
                                                  uint32_t sampleIndex,
                                                  BAEPathName filePath)
{
    BAERmfEditorSample const *sample;
    XFILENAME fileName;
    AudioFileType outType;
    char const *ext;
    GM_Waveform waveCopy;
    OPErr opErr;
    BAEResult passResult;

    if (!document || !filePath || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    if (!sample->waveform || !sample->waveform->theWaveform)
    {
        return BAE_BAD_FILE;
    }

    /* For compressed formats with an original SND blob, extract and write the
     * raw bitstream directly: FLAC -> .flac, Vorbis/Opus -> .ogg, MPEG -> .mp3,
     * QOA -> .qoa. The blob is laid out as XSndHeader3 (int16_t format tag +
     * XSoundHeader3), with the compressed bitstream at sndBuffer.sampleArea[0]. */
    if (sample->originalSndData && sample->originalSndSize > (int32_t)sizeof(XSndHeader3))
    {
        passResult = PV_ExportSndBitstreamToFile(sample->originalSndData,
                                                 sample->originalSndSize,
                                                 (SndCompressionType)sample->sourceCompressionType,
                                                 filePath);
        if (passResult != BAE_NOT_SETUP)
        {
            return passResult;
        }
    }

    /* PCM/ADPCM/IMA or no original blob: export decoded waveform as WAV or AIFF. */
    ext = strrchr(filePath, '.');
    if (ext && (!XStrCmp(ext, ".aif") || !XStrCmp(ext, ".aiff") ||
               !XStrCmp(ext, ".AIF") || !XStrCmp(ext, ".AIFF")))
    {
        outType = FILE_AIFF_TYPE;
    }
    else
    {
        outType = FILE_WAVE_TYPE;
    }

    waveCopy = *sample->waveform;
    XConvertPathToXFILENAME(filePath, &fileName);
    opErr = GM_WriteFileFromMemory(&fileName, &waveCopy, outType);
    return (opErr == NO_ERR) ? BAE_NO_ERROR : BAE_FILE_IO_ERROR;
}


/* ---------- Extended instrument data API ---------- */

BAEResult BAERmfEditorDocument_GetInstIDForSample(BAERmfEditorDocument const *document,
                                                  uint32_t sampleIndex,
                                                  uint32_t *outInstID)
{
    if (!document || !outInstID || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    *outInstID = (uint32_t)document->samples[sampleIndex].instID;
    return BAE_NO_ERROR;
}


void PV_CopyEditorADSRToInfo(EditorADSR const *src, BAERmfEditorADSRInfo *dst)
{
    uint32_t i;
    dst->stageCount = src->stageCount;
    for (i = 0; i < src->stageCount && i < EDITOR_MAX_ADSR_STAGES; i++)
    {
        dst->stages[i].level = src->stages[i].level;
        dst->stages[i].time = src->stages[i].time;
        dst->stages[i].flags = src->stages[i].flags;
    }
}


void PV_CopyInfoToEditorADSR(BAERmfEditorADSRInfo const *src, EditorADSR *dst)
{
    uint32_t i;
    dst->stageCount = src->stageCount;
    if (dst->stageCount > EDITOR_MAX_ADSR_STAGES)
    {
        dst->stageCount = EDITOR_MAX_ADSR_STAGES;
    }
    for (i = 0; i < dst->stageCount; i++)
    {
        dst->stages[i].level = src->stages[i].level;
        dst->stages[i].time = src->stages[i].time;
        dst->stages[i].flags = src->stages[i].flags;
    }
}


BAEResult BAERmfEditorDocument_GetInstrumentExtInfo(BAERmfEditorDocument const *document,
                                                    uint32_t instID,
                                                    BAERmfEditorInstrumentExtInfo *outInfo)
{
    BAERmfEditorInstrumentExt *ext;
    uint32_t i;

    if (!document || !outInfo)
    {
        return BAE_PARAM_ERR;
    }
    XSetMemory(outInfo, (int32_t)sizeof(*outInfo), 0);
    outInfo->instID = instID;

    ext = PV_FindInstrumentExt((BAERmfEditorDocument *)document, (XLongResourceID)instID);
    if (!ext)
    {
        /* No ext data stored - return defaults.
         * flags1/flags2 must match what PV_AddSampleResources uses when no
         * ext record exists, otherwise the INST resource written during
         * preview serialisation loses ZBF_useSampleRate and the engine
         * plays the sample at the wrong pitch (one octave low for 44 kHz). */
        outInfo->displayName = NULL;
        outInfo->hasExtendedData = FALSE;
        outInfo->flags1 = ZBF_useSampleRate;
        outInfo->flags2 = ZBF_useSoundModifierAsRootKey;
        outInfo->midiRootKey = 60;
        outInfo->miscParameter1 = 0;
        outInfo->miscParameter2 = 100;
        outInfo->volumeADSR.stageCount = 1;
        outInfo->volumeADSR.stages[0].level = VOLUME_RANGE;
        outInfo->volumeADSR.stages[0].time = 0;
        outInfo->volumeADSR.stages[0].flags = ADSR_TERMINATE_LONG;
        return BAE_NO_ERROR;
    }

    outInfo->displayName = ext->displayName;
    outInfo->hasExtendedData = ext->hasExtendedData;
    outInfo->flags1 = ext->flags1;
    outInfo->flags2 = ext->flags2;
    outInfo->panPlacement = ext->panPlacement;
    outInfo->midiRootKey = ext->midiRootKey;
    outInfo->miscParameter1 = ext->miscParameter1;
    outInfo->miscParameter2 = ext->miscParameter2;
    outInfo->hasDefaultMod = ext->hasDefaultMod;
    outInfo->LPF_frequency = ext->LPF_frequency;
    outInfo->LPF_resonance = ext->LPF_resonance;
    outInfo->LPF_lowpassAmount = ext->LPF_lowpassAmount;
    outInfo->defaultReverbSend = ext->defaultReverbSend;
    outInfo->defaultChorusSend = ext->defaultChorusSend;
#if USE_ZMF_SUPPORT == TRUE
    outInfo->useOscillator = ext->useOscillator ? TRUE : FALSE;
    outInfo->oscWaveShape = ext->oscWaveShape ? ext->oscWaveShape : (int32_t)SINE_WAVE_LONG;
    outInfo->oscPulseWidth = ext->oscPulseWidth > 0 ? ext->oscPulseWidth : 32768;
    outInfo->oscVolume = ext->oscVolume;
    if (outInfo->oscVolume < 0)
        outInfo->oscVolume = 0;
    if (outInfo->oscVolume > 65536)
        outInfo->oscVolume = 65536;
#endif
    PV_CopyEditorADSRToInfo(&ext->volumeADSR, &outInfo->volumeADSR);
    outInfo->lfoCount = ext->lfoCount;
    for (i = 0; i < ext->lfoCount && i < EDITOR_MAX_LFOS; i++)
    {
        outInfo->lfos[i].destination = ext->lfos[i].destination;
        outInfo->lfos[i].period = ext->lfos[i].period;
        outInfo->lfos[i].waveShape = ext->lfos[i].waveShape;
        outInfo->lfos[i].DC_feed = ext->lfos[i].DC_feed;
        outInfo->lfos[i].level = ext->lfos[i].level;
        PV_CopyEditorADSRToInfo(&ext->lfos[i].adsr, &outInfo->lfos[i].adsr);
    }
    outInfo->curveCount = ext->curveCount;
    for (i = 0; i < ext->curveCount && i < EDITOR_MAX_CURVES; i++)
    {
        int32_t j;
        outInfo->curves[i].tieFrom = ext->curves[i].tieFrom;
        outInfo->curves[i].tieTo = ext->curves[i].tieTo;
        outInfo->curves[i].curveCount = ext->curves[i].curveCount;
        for (j = 0; j < ext->curves[i].curveCount && j < EDITOR_MAX_ADSR_STAGES; j++)
        {
            outInfo->curves[i].from_Value[j] = ext->curves[i].from_Value[j];
            outInfo->curves[i].to_Scalar[j] = ext->curves[i].to_Scalar[j];
        }
    }
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetInstrumentExtInfo(BAERmfEditorDocument *document,
                                                    uint32_t instID,
                                                    BAERmfEditorInstrumentExtInfo const *info)
{
    BAERmfEditorInstrumentExt *ext;
    uint32_t i;

    if (!document || !info)
    {
        return BAE_PARAM_ERR;
    }

    ext = PV_FindInstrumentExt(document, (XLongResourceID)instID);
    if (!ext)
    {
        /* Create a new entry */
        BAERmfEditorInstrumentExt newExt;
        BAEResult result;
        XSetMemory(&newExt, (int32_t)sizeof(newExt), 0);
        newExt.instID = (XLongResourceID)instID;
        result = PV_AddInstrumentExt(document, &newExt);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        ext = PV_FindInstrumentExt(document, (XLongResourceID)instID);
        if (!ext)
        {
            return BAE_GENERAL_ERR;
        }
    }

    {
        BAEResult nameResult;
        nameResult = PV_SetDocumentString(&ext->displayName, info->displayName);
        if (nameResult != BAE_NO_ERROR)
        {
            return nameResult;
        }
    }

    ext->hasExtendedData = info->hasExtendedData;
    ext->dirty = TRUE;
    ext->flags1 = info->flags1;
    ext->flags2 = info->flags2;
    ext->panPlacement = info->panPlacement;
    ext->midiRootKey = info->midiRootKey;
    ext->miscParameter1 = info->miscParameter1;
    ext->miscParameter2 = info->miscParameter2;
    ext->hasDefaultMod = info->hasDefaultMod;
    ext->LPF_frequency = info->LPF_frequency;
    ext->LPF_resonance = info->LPF_resonance;
    ext->LPF_lowpassAmount = info->LPF_lowpassAmount;
    ext->defaultReverbSend = info->defaultReverbSend;
    ext->defaultChorusSend = info->defaultChorusSend;
#if USE_ZMF_SUPPORT == TRUE
    ext->useOscillator = info->useOscillator ? TRUE : FALSE;
    ext->oscWaveShape = info->oscWaveShape ? info->oscWaveShape : (int32_t)SINE_WAVE_LONG;
    ext->oscPulseWidth = info->oscPulseWidth > 0 ? info->oscPulseWidth : 32768;
    ext->oscVolume = info->oscVolume;
    if (ext->oscVolume < 0)
        ext->oscVolume = 0;
    if (ext->oscVolume > 65536)
        ext->oscVolume = 65536;
    if (ext->useOscillator)
    {
        ext->flags1 |= ZBF_extendedFormat;
        ext->hasExtendedData = TRUE;
    }
#endif
    PV_CopyInfoToEditorADSR(&info->volumeADSR, &ext->volumeADSR);
    ext->lfoCount = info->lfoCount;
    if (ext->lfoCount > EDITOR_MAX_LFOS)
    {
        ext->lfoCount = EDITOR_MAX_LFOS;
    }
    for (i = 0; i < ext->lfoCount; i++)
    {
        ext->lfos[i].destination = info->lfos[i].destination;
        ext->lfos[i].period = info->lfos[i].period;
        if (ext->lfos[i].period != 0 && ext->lfos[i].period <= 512)
            ext->lfos[i].period = 513; // engine requires period == 0 or > 512
        ext->lfos[i].waveShape = info->lfos[i].waveShape;
        ext->lfos[i].DC_feed = info->lfos[i].DC_feed;
        ext->lfos[i].level = info->lfos[i].level;
        PV_CopyInfoToEditorADSR(&info->lfos[i].adsr, &ext->lfos[i].adsr);
    }
    ext->curveCount = info->curveCount;
    if (ext->curveCount > EDITOR_MAX_CURVES)
    {
        ext->curveCount = EDITOR_MAX_CURVES;
    }
    for (i = 0; i < ext->curveCount; i++)
    {
        int32_t j;
        ext->curves[i].tieFrom = info->curves[i].tieFrom;
        ext->curves[i].tieTo = info->curves[i].tieTo;
        ext->curves[i].curveCount = info->curves[i].curveCount;
        if (ext->curves[i].curveCount > EDITOR_MAX_ADSR_STAGES)
        {
            ext->curves[i].curveCount = EDITOR_MAX_ADSR_STAGES;
        }
        for (j = 0; j < ext->curves[i].curveCount; j++)
        {
            ext->curves[i].from_Value[j] = info->curves[i].from_Value[j];
            ext->curves[i].to_Scalar[j] = info->curves[i].to_Scalar[j];
        }
    }

    /* Discard raw blob since we've been modified */
    if (ext->originalInstData)
    {
        XDisposePtr(ext->originalInstData);
        ext->originalInstData = NULL;
        ext->originalInstSize = 0;
    }

    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult PV_ResolveBankInstrumentIndexForGhost(BAEBankToken bankToken,
                                                       uint32_t instID,
                                                       uint32_t *outInstrumentIndex)
{
    uint32_t instrumentCount;
    uint32_t instrumentIndex;
    uint32_t resolvedInstID;

    if (!bankToken || !outInstrumentIndex)
    {
        return BAE_PARAM_ERR;
    }
    if (BAERmfEditorBank_GetInstrumentCount(bankToken, &instrumentCount) != BAE_NO_ERROR)
    {
        return BAE_BAD_FILE;
    }
    for (instrumentIndex = 0; instrumentIndex < instrumentCount; ++instrumentIndex)
    {
        BAERmfEditorBankInstrumentInfo info;
        if (BAERmfEditorBank_GetInstrumentInfo(bankToken, instrumentIndex, &info) == BAE_NO_ERROR &&
            info.instID == instID)
        {
            *outInstrumentIndex = instrumentIndex;
            return BAE_NO_ERROR;
        }
    }
    if (BAERmfEditorBank_ResolveInstID(bankToken, instID, &resolvedInstID, outInstrumentIndex) == BAE_NO_ERROR)
    {
        return BAE_NO_ERROR;
    }
    return BAE_BAD_FILE;
}


/* Returns TRUE and writes *outSndID when resolved. sndID 0 is a valid resource id. */
bool PV_ResolveGhostAliasSndID(BAEBankToken bankToken,
                                      uint32_t bankInstrumentIndex,
                                      uint32_t splitIndex,
                                      BAERmfEditorSample const *sample,
                                      XShortResourceID *outSndID)
{
    BAERmfEditorBankSampleInfo bankSample;
    XFILE bankFile;
    XPTR sndData;
    int32_t sndSize;

    if (!sample || !outSndID)
    {
        return FALSE;
    }
    if (sample->isBankAlias)
    {
        *outSndID = sample->aliasSndResourceID;
        return TRUE;
    }

    XSetMemory(&bankSample, sizeof(bankSample), 0);
    if (BAERmfEditorBank_GetInstrumentSampleInfo(bankToken,
                                                 bankInstrumentIndex,
                                                 splitIndex,
                                                 &bankSample) == BAE_NO_ERROR)
    {
        *outSndID = bankSample.sndResourceID;
        return TRUE;
    }

    if (sample->sampleAssetID <= 32767u)
    {
        bankFile = (XFILE)bankToken;
        XFileUseThisResourceFile(bankFile);
        sndData = XGetSoundResourceByID((XLongResourceID)sample->sampleAssetID, &sndSize);
        if (sndData)
        {
            XDisposePtr(sndData);
            *outSndID = (XShortResourceID)sample->sampleAssetID;
            return TRUE;
        }
    }
    return FALSE;
}


BAEResult PV_ConvertDocumentSampleToBankAlias(BAERmfEditorSample *sample,
                                                     BAEBankToken bankToken,
                                                     XShortResourceID sndID)
{
    if (!sample || !bankToken)
    {
        return BAE_PARAM_ERR;
    }
    if (sample->waveform)
    {
        GM_FreeWaveform(sample->waveform);
        sample->waveform = NULL;
    }
    if (sample->originalSndData)
    {
        XDisposePtr(sample->originalSndData);
        sample->originalSndData = NULL;
        sample->originalSndSize = 0;
    }
    sample->isBankAlias = TRUE;
    sample->aliasBankToken = bankToken;
    sample->aliasSndResourceID = sndID;
    sample->sampleAssetID = (uint32_t)sndID;
    return BAE_NO_ERROR;
}


BAEResult PV_HydrateDocumentSampleFromBankAlias(BAERmfEditorDocument *document,
                                                       uint32_t sampleIndex,
                                                       BAEBankToken bankToken)
{
    BAERmfEditorSample *sample;
    BAERmfEditorSample saved;
    XShortResourceID sndID;
    uint32_t beforeCount;
    BAEResult result;

    if (!document || !bankToken || sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    sample = &document->samples[sampleIndex];
    if (!sample->isBankAlias)
    {
        return BAE_NO_ERROR;
    }
    sndID = sample->aliasSndResourceID;

    saved = *sample;
    beforeCount = document->sampleCount;
    result = PV_AddEmbeddedSampleVariant(document,
                                         (XFILE)bankToken,
                                         (XLongResourceID)saved.instID,
                                         saved.displayName,
                                         saved.program,
                                         sndID,
                                         saved.rootKey,
                                         saved.lowKey,
                                         saved.highKey);
    if (result != BAE_NO_ERROR || document->sampleCount != beforeCount + 1)
    {
        return (result != BAE_NO_ERROR) ? result : BAE_GENERAL_ERR;
    }

    {
        BAERmfEditorSample *hydrated = &document->samples[document->sampleCount - 1];
        hydrated->splitVolume = saved.splitVolume;
        hydrated->instID = saved.instID;
        hydrated->program = saved.program;
        hydrated->rootKey = saved.rootKey;
        hydrated->lowKey = saved.lowKey;
        hydrated->highKey = saved.highKey;

        /* Replace the alias slot with the hydrated sample, then drop the temp. */
        if (sample->displayName)
        {
            PV_FreeString(&sample->displayName);
        }
        if (sample->sourcePath)
        {
            PV_FreeString(&sample->sourcePath);
        }
        *sample = *hydrated;
        XSetMemory(hydrated, sizeof(*hydrated), 0);
        document->sampleCount--;
    }
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_IsInstrumentGhost(BAERmfEditorDocument const *document,
                                                 uint32_t instID,
                                                 BAE_BOOL *outGhost)
{
    BAERmfEditorInstrumentExt *ext;
    uint32_t i;
    uint32_t sampleCountForInst;
    uint32_t aliasCount;

    if (!document || !outGhost)
    {
        return BAE_PARAM_ERR;
    }
    *outGhost = FALSE;

    ext = PV_FindInstrumentExt((BAERmfEditorDocument *)document, (XLongResourceID)instID);
    if (ext && TEST_FLAG_VALUE(ext->flags1, ZBF_ghostInstrument))
    {
        *outGhost = TRUE;
        return BAE_NO_ERROR;
    }

    sampleCountForInst = 0;
    aliasCount = 0;
    for (i = 0; i < document->sampleCount; ++i)
    {
        if (document->samples[i].instID != instID)
        {
            continue;
        }
        sampleCountForInst++;
        if (document->samples[i].isBankAlias)
        {
            aliasCount++;
        }
    }
    if (sampleCountForInst > 0 && aliasCount == sampleCountForInst)
    {
        *outGhost = TRUE;
    }
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetInstrumentGhost(BAERmfEditorDocument *document,
                                                  uint32_t instID,
                                                  BAEBankToken bankToken,
                                                  BAE_BOOL ghost)
{
    BAERmfEditorInstrumentExtInfo info;
    BAEResult result;
    uint32_t bankInstrumentIndex;
    uint32_t sampleIndex;
    uint32_t splitIndex;
    BAE_BOOL currentlyGhost;

    if (!document || !bankToken)
    {
        return BAE_PARAM_ERR;
    }

    currentlyGhost = FALSE;
    (void)BAERmfEditorDocument_IsInstrumentGhost(document, instID, &currentlyGhost);

    result = BAERmfEditorDocument_GetInstrumentExtInfo(document, instID, &info);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    if (ghost)
    {
        info.flags1 = (unsigned char)(info.flags1 | ZBF_ghostInstrument);
    }
    else
    {
        info.flags1 = (unsigned char)(info.flags1 & (unsigned char)~ZBF_ghostInstrument);
    }
    result = BAERmfEditorDocument_SetInstrumentExtInfo(document, instID, &info);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

#if USE_ZMF_SUPPORT == TRUE
    /* Oscillator instruments are sample-free (SND 0xFFFF). Ghost aliases are
     * meaningless here and inventing a 0xFFFF bank-alias row makes the RMF
     * writer treat the INST as sample-backed, so the OSCL-only INST path is
     * skipped and playback is silent without a host bank sample. */
    if (info.useOscillator)
    {
        uint32_t si = 0;
        while (si < document->sampleCount)
        {
            if (document->samples[si].instID == instID)
            {
                (void)BAERmfEditorDocument_DeleteSample(document, si);
            }
            else
            {
                ++si;
            }
        }
        /* Drop the ghost bit we may have just set — sample-free OSCL embeds
         * fully; there is nothing to resolve from a host bank. */
        if (TEST_FLAG_VALUE(info.flags1, ZBF_ghostInstrument))
        {
            info.flags1 = (unsigned char)(info.flags1 & (unsigned char)~ZBF_ghostInstrument);
            (void)BAERmfEditorDocument_SetInstrumentExtInfo(document, instID, &info);
        }
        PV_MarkDocumentDirty(document);
        return BAE_NO_ERROR;
    }
#endif

    if (ghost == currentlyGhost)
    {
        /* Still ensure sample alias state matches when enabling. */
        if (!ghost)
        {
            return BAE_NO_ERROR;
        }
    }

    bankInstrumentIndex = 0;
    {
        BAEResult resolveResult;

        resolveResult = PV_ResolveBankInstrumentIndexForGhost(bankToken, instID, &bankInstrumentIndex);
        if (resolveResult != BAE_NO_ERROR)
        {
            /* Song-only INST with no bank counterpart: allow flag-only ghost when
             * samples already carry usable bank SND IDs via sampleAssetID. */
            if (!ghost)
            {
                return BAE_BAD_FILE;
            }
            result = resolveResult;
        }
        else
        {
            result = BAE_NO_ERROR;
        }
    }

    splitIndex = 0;
    for (sampleIndex = 0; sampleIndex < document->sampleCount; ++sampleIndex)
    {
        BAERmfEditorSample *sample = &document->samples[sampleIndex];
        if (sample->instID != instID)
        {
            continue;
        }
        if (ghost)
        {
            XShortResourceID sndID = 0;
            /* Bank index resolve may fail for song-only INSTs; still allow
             * sampleAssetID / existing alias fallbacks inside the resolver. */
            if (!PV_ResolveGhostAliasSndID(bankToken,
                                           bankInstrumentIndex,
                                           splitIndex,
                                           sample,
                                           &sndID))
            {
                return (result != BAE_NO_ERROR) ? result : BAE_PARAM_ERR;
            }
            result = PV_ConvertDocumentSampleToBankAlias(sample, bankToken, sndID);
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
        }
        else
        {
            result = PV_HydrateDocumentSampleFromBankAlias(document, sampleIndex, bankToken);
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
        }
        splitIndex++;
    }

    /* Enabling ghost with no document samples yet (e.g. session reload):
     * materialize bank-alias splits from the bank INST so RMF export can
     * omit embedding without the user re-checking Ghost. */
    if (ghost && splitIndex == 0 && result == BAE_NO_ERROR)
    {
        uint32_t bankSampleCount;
        uint32_t s;
        BAERmfEditorBankInstrumentInfo bankInfo;
        BAERmfEditorInstrumentExtInfo bankExt;

        bankSampleCount = 0;
        XSetMemory(&bankInfo, sizeof(bankInfo), 0);
        XSetMemory(&bankExt, sizeof(bankExt), 0);
        if (BAERmfEditorBank_GetInstrumentSampleCount(bankToken,
                                                      bankInstrumentIndex,
                                                      &bankSampleCount) != BAE_NO_ERROR ||
            bankSampleCount == 0 ||
            BAERmfEditorBank_GetInstrumentInfo(bankToken, bankInstrumentIndex, &bankInfo) != BAE_NO_ERROR)
        {
            return BAE_BAD_FILE;
        }
        {
            uint32_t aliasesAdded = 0;
            for (s = 0; s < bankSampleCount; ++s)
            {
                BAERmfEditorBankSampleInfo bankSample;
                XSetMemory(&bankSample, sizeof(bankSample), 0);
                if (BAERmfEditorBank_GetInstrumentSampleInfo(bankToken,
                                                            bankInstrumentIndex,
                                                            s,
                                                            &bankSample) != BAE_NO_ERROR)
                {
                    return BAE_BAD_FILE;
                }
                /* 0xFFFF = no sample (oscillator / empty INST). Do not invent aliases. */
                if (PV_IsNoSampleSndID(bankSample.sndResourceID))
                {
                    continue;
                }
                if (BAERmfEditorDocument_AddBankAliasSample(document,
                                                           bankToken,
                                                           instID,
                                                           bankInfo.program,
                                                           bankSample.sndResourceID,
                                                           bankInfo.name,
                                                           bankSample.rootKey,
                                                           bankSample.lowKey,
                                                           bankSample.highKey,
                                                           NULL,
                                                           NULL) != BAE_NO_ERROR)
                {
                    return BAE_GENERAL_ERR;
                }
                document->samples[document->sampleCount - 1].splitVolume = bankSample.splitVolume;
                aliasesAdded++;
            }
            if (aliasesAdded == 0)
            {
#if USE_ZMF_SUPPORT == TRUE
                if (BAERmfEditorBank_GetInstrumentExtInfo(bankToken, bankInstrumentIndex, &bankExt) ==
                        BAE_NO_ERROR &&
                    bankExt.useOscillator)
                {
                    bankExt.displayName = bankInfo.name[0] ? bankInfo.name : NULL;
                    bankExt.flags1 = (unsigned char)(bankExt.flags1 | ZBF_ghostInstrument);
                    bankExt.instID = instID;
                    (void)BAERmfEditorDocument_SetInstrumentExtInfo(document, instID, &bankExt);
                    PV_MarkDocumentDirty(document);
                    return BAE_NO_ERROR;
                }
#endif
                return BAE_BAD_FILE;
            }
        }
        if (BAERmfEditorBank_GetInstrumentExtInfo(bankToken, bankInstrumentIndex, &bankExt) == BAE_NO_ERROR)
        {
            /* Bank GetInstrumentExtInfo displayName points at a locals buffer;
             * retarget to bankInfo.name which remains valid in this scope. */
            bankExt.displayName = bankInfo.name[0] ? bankInfo.name : NULL;
            bankExt.flags1 = (unsigned char)(bankExt.flags1 | ZBF_ghostInstrument);
            bankExt.instID = instID;
            (void)BAERmfEditorDocument_SetInstrumentExtInfo(document, instID, &bankExt);
        }
    }

    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


/* --------------------------------------------------------------- */

BAEResult BAERmfEditorDocument_CopySamplesFrom(BAERmfEditorDocument *dest,
                                               BAERmfEditorDocument const *src)
{
    return BAERmfEditorDocument_CopySamplesForPrograms(dest, src, NULL, NULL);
}


BAEResult PV_CopySampleEntry(BAERmfEditorDocument *dest,
                                    BAERmfEditorSample const *srcSample)
{
    BAEResult result;
    BAERmfEditorSample *dstSample;
    GM_Waveform *waveform;

    result = PV_GrowBuffer((void **)&dest->samples,
                           &dest->sampleCapacity,
                           sizeof(BAERmfEditorSample),
                           dest->sampleCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    dstSample = &dest->samples[dest->sampleCount];
    XSetMemory(dstSample, sizeof(*dstSample), 0);
    dstSample->displayName = PV_DuplicateString(srcSample->displayName);
    dstSample->sourcePath = PV_DuplicateString(srcSample->sourcePath);
    dstSample->program = srcSample->program;
    dstSample->instID = srcSample->instID;
    dstSample->sampleAssetID = srcSample->sampleAssetID;
    PV_NoteSampleAssetID(dest, dstSample->sampleAssetID);
    dstSample->rootKey = srcSample->rootKey;
    dstSample->lowKey = srcSample->lowKey;
    dstSample->highKey = srcSample->highKey;
    dstSample->splitVolume = srcSample->splitVolume;
    dstSample->sourceCompressionType = srcSample->sourceCompressionType;
    dstSample->sourceCompressionSubType = srcSample->sourceCompressionSubType;
    dstSample->targetCompressionType = srcSample->targetCompressionType;
    dstSample->targetOpusMode = srcSample->targetOpusMode;
    dstSample->opusUseRoundTripResampling = srcSample->opusUseRoundTripResampling;
    dstSample->isBankAlias = srcSample->isBankAlias;
    dstSample->aliasBankToken = srcSample->aliasBankToken;
    dstSample->aliasSndResourceID = srcSample->aliasSndResourceID;
    dstSample->sampleInfo = srcSample->sampleInfo;
    dstSample->originalSndData = NULL;
    dstSample->originalSndSize = 0;
    if (srcSample->originalSndData && srcSample->originalSndSize > 0)
    {
        XPTR sndCopy = XNewPtr((int32_t)srcSample->originalSndSize);
        if (!sndCopy)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(srcSample->originalSndData, sndCopy, (int32_t)srcSample->originalSndSize);
        dstSample->originalSndData = sndCopy;
        dstSample->originalSndSize = srcSample->originalSndSize;
    }
    waveform = NULL;
    if (srcSample->waveform)
    {
        waveform = (GM_Waveform *)XNewPtr((int32_t)sizeof(GM_Waveform));
        if (!waveform)
        {
            return BAE_MEMORY_ERR;
        }
        *waveform = *srcSample->waveform;
        waveform->theWaveform = NULL;
        if (srcSample->waveform->theWaveform && srcSample->waveform->waveSize > 0)
        {
            XPTR pcmCopy = XNewPtr((int32_t)srcSample->waveform->waveSize);
            if (!pcmCopy)
            {
                XDisposePtr((XPTR)waveform);
                return BAE_MEMORY_ERR;
            }
            XBlockMove(srcSample->waveform->theWaveform, pcmCopy, (int32_t)srcSample->waveform->waveSize);
            waveform->theWaveform = (signed char *)pcmCopy;
        }
    }
    dstSample->waveform = waveform;
    dest->sampleCount++;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_CopySamplesForPrograms(BAERmfEditorDocument *dest,
                                                      BAERmfEditorDocument const *src,
                                                      unsigned char const *programFlags128,
                                                      uint32_t *outCopiedCount)
{
    uint32_t i;
    uint32_t copiedCount;
    BAEResult result;

    if (!dest || !src)
    {
        return BAE_PARAM_ERR;
    }
    copiedCount = 0;
    for (i = 0; i < src->sampleCount; i++)
    {
        BAERmfEditorSample const *srcSample;

        srcSample = &src->samples[i];
        if (programFlags128)
        {
            if (srcSample->program >= 128)
            {
                continue;
            }
            if (!programFlags128[srcSample->program])
            {
                continue;
            }
        }
        result = PV_CopySampleEntry(dest, srcSample);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        copiedCount++;
    }
    if (outCopiedCount)
    {
        *outCopiedCount = copiedCount;
    }
    PV_MarkDocumentDirty(dest);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_Validate(BAERmfEditorDocument *document)
{
    uint32_t trackIndex;

    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (document->trackCount == 0)
    {
        return BAE_PARAM_ERR;
    }
    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack const *track;

        track = &document->tracks[trackIndex];
        if (track->channel >= BAE_MAX_MIDI_CHANNELS ||
            track->bank > 16383 ||
            track->program >= 128 ||
            track->pan > 127 ||
            track->volume > 127 ||
            track->transpose < -127 ||
            track->transpose > 127)
        {
            return BAE_PARAM_ERR;
        }
    }
    return BAE_NO_ERROR;
}



// begin ZMF requirements checking
// we leave these enabled even if ZMF support is not compiled in, to allow detection of potential ZMF requirements
// and to prevent saving RMF with ZMF features (ZMF required + no ZMF Support = fail)

/* Classic HSB/RMF has no supported LPF/resonance path in NeoBAE — treat as zs*. */
bool PV_LfoDestinationIsLpf(int32_t destination)
{
    return destination == (int32_t)FOUR_CHAR('L', 'P', 'F', 'R') ||
           destination == (int32_t)FOUR_CHAR('L', 'P', 'R', 'E') ||
           destination == (int32_t)FOUR_CHAR('L', 'P', 'A', 'M');
}


bool PV_InstrumentUsesLpfFilter(int32_t lpfFrequency,
                                       int32_t lpfResonance,
                                       int32_t lpfLowpassAmount,
                                       bool hasFilterLfoDestination)
{
    return (lpfFrequency != 0 || lpfResonance != 0 || lpfLowpassAmount != 0 ||
            hasFilterLfoDestination) ? TRUE : FALSE;
}


void BAEZMFReasonCodeToString(uint32_t reason, char *outBuffer, uint32_t bufferSize)
{
    outBuffer[0] = '\0';

    size_t cap = bufferSize;
    size_t len = 0;

    // helper lambda-style macro for bounded append
    #define APPEND(text) do { \
        const char* t = (text); \
        size_t tlen = strlen(t); \
        if (len + tlen < cap) { \
            memcpy(outBuffer + len, t, tlen); \
            len += tlen; \
            outBuffer[len] = '\0'; \
        } \
    } while (0)

    if (reason & BAEZMF_ALREADY_ZMF)
        APPEND("Already a ZMF file; ");

    if (reason & BAEZMF_REASON_LOOP_TOO_SHORT)
        APPEND("An instrument's loop is too short; ");

    if (reason & BAEZMF_REASON_MODERN_CODEC)
        APPEND("Using a modern codec; ");

    if (reason & BAEZMF_REASON_CUBIC_INTERPOLATION)
        APPEND("Using advanced interpolation; ");

    if (reason & BAEZMF_REASON_CLASSIC_CHORUS)
        APPEND("Classic chorus song flag enabled; ");

    if (reason & BAEZMF_REASON_PANFIX)
        APPEND("Panfix song flag enabled; ");

    if (reason & BAEZMF_REASON_EXTENDED_ADSR)
        APPEND("Extended ADSR envelope (>8 stages); ");

    if (reason & BAEZMF_REASON_EXTENDED_PITCH_RANGE)
        APPEND("Extended pitch range enabled; ");

    if (reason & BAEZMF_REASON_LPF_FILTER)
        APPEND("Stereo sample with low-pass filter / resonance; ");

    if (reason & BAEZMF_REASON_OSCILLATOR)
        APPEND("Instrument oscillator (sample-free generator); ");

    if (reason & BAEZMF_REASON_ADPCM_2BIT)
        APPEND("Using 2-bit ADPCM (ZMF v6); ");

    if (reason & BAEZMF_REASON_OTHER)
        APPEND("Other engine flags set; ");        

    if (len == 0)
    {
        snprintf(outBuffer, bufferSize, "Unknown");
    }

    // remove trailing "; " safely
    if (len >= 2) {
        outBuffer[len - 2] = '\0';
    }
}


BAE_BOOL BAERmfEditorDocument_RequiresZmf(BAERmfEditorDocument const *document, uint32_t *outReason)
{
    uint32_t i;
    uint32_t reason = 0;

    if (!document)
    {
        return FALSE;
    }
    for (i = 0; i < document->sampleCount; ++i)
    {
        BAERmfEditorSample const *sample = &document->samples[i];

        if (sample->sampleInfo.endLoop > sample->sampleInfo.startLoop &&
            (sample->sampleInfo.endLoop - sample->sampleInfo.startLoop) < MIN_LOOP_SIZE_RMF &&
            (reason & BAEZMF_REASON_LOOP_TOO_SHORT) == 0)
        {
            reason |= BAEZMF_REASON_LOOP_TOO_SHORT;
        }

        switch (sample->targetCompressionType)
        {
            case BAE_EDITOR_COMPRESSION_VORBIS_32K:
            case BAE_EDITOR_COMPRESSION_VORBIS_48K:
            case BAE_EDITOR_COMPRESSION_VORBIS_64K:
            case BAE_EDITOR_COMPRESSION_VORBIS_80K:
            case BAE_EDITOR_COMPRESSION_VORBIS_96K:
            case BAE_EDITOR_COMPRESSION_VORBIS_128K:
            case BAE_EDITOR_COMPRESSION_VORBIS_160K:
            case BAE_EDITOR_COMPRESSION_VORBIS_192K:
            case BAE_EDITOR_COMPRESSION_VORBIS_256K:
            case BAE_EDITOR_COMPRESSION_FLAC:
            case BAE_EDITOR_COMPRESSION_OPUS_12K:
            case BAE_EDITOR_COMPRESSION_OPUS_16K:
            case BAE_EDITOR_COMPRESSION_OPUS_24K:
            case BAE_EDITOR_COMPRESSION_OPUS_32K:
            case BAE_EDITOR_COMPRESSION_OPUS_48K:
            case BAE_EDITOR_COMPRESSION_OPUS_64K:
            case BAE_EDITOR_COMPRESSION_OPUS_80K:
            case BAE_EDITOR_COMPRESSION_OPUS_96K:
            case BAE_EDITOR_COMPRESSION_OPUS_128K:
            case BAE_EDITOR_COMPRESSION_OPUS_160K:
            case BAE_EDITOR_COMPRESSION_OPUS_192K:
            case BAE_EDITOR_COMPRESSION_OPUS_256K:
#if USE_QOA_SUPPORT == TRUE
            case BAE_EDITOR_COMPRESSION_QOA:
#endif
                if ((reason & BAEZMF_REASON_MODERN_CODEC) == 0)
                {
                    reason |= BAEZMF_REASON_MODERN_CODEC;
                }
                /* fall through — also inspect original/source compression */

            case BAE_EDITOR_COMPRESSION_DONT_CHANGE:
                /* Original data may contain a modern codec */
                if (
#if USE_FLAC_DECODER == TRUE || USE_FLAC_ENCODER == TRUE                    
                    sample->sourceCompressionType == (uint32_t)C_FLAC ||
#endif
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE                    
                    sample->sourceCompressionType == (uint32_t)C_VORBIS ||
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
                    sample->sourceCompressionType == (uint32_t)C_OPUS ||
#endif
#if USE_QOA_SUPPORT == TRUE
                    sample->sourceCompressionType == (uint32_t)C_QOA ||
#endif
                    sample->sourceCompressionType == (uint32_t)C_IMA2 ||
                false)
                {
                    reason |= BAEZMF_REASON_MODERN_CODEC;
                    if (sample->sourceCompressionType == (uint32_t)C_IMA2)
                    {
                        reason |= BAEZMF_REASON_ADPCM_2BIT;
                    }
                }
                break;
#if USE_ZMF_SUPPORT == TRUE
            case BAE_EDITOR_COMPRESSION_ADPCM_2BIT:
                reason |= BAEZMF_REASON_ADPCM_2BIT;
                reason |= BAEZMF_REASON_MODERN_CODEC;
                break;
#endif
            default:
                break;
        }
    }
    for (i = 0; i < document->instrumentExtCount; ++i)
    {
        BAERmfEditorInstrumentExt const *ext = &document->instrumentExts[i];
        uint32_t lfoIdx;

        if (TEST_FLAG_VALUE(ext->flags2, ZBF_advancedInterpolation) && (reason & BAEZMF_REASON_CUBIC_INTERPOLATION) == 0)
        {            
            reason |= BAEZMF_REASON_CUBIC_INTERPOLATION;
        }

        if ((reason & BAEZMF_REASON_EXTENDED_ADSR) == 0)
        {
            if (ext->volumeADSR.stageCount > BAE_RMF_MAX_ADSR_STAGES)
            {
                reason |= BAEZMF_REASON_EXTENDED_ADSR;
            }
            else
            {
                for (lfoIdx = 0; lfoIdx < ext->lfoCount; lfoIdx++)
                {
                    if (ext->lfos[lfoIdx].adsr.stageCount > BAE_RMF_MAX_ADSR_STAGES)
                    {
                        reason |= BAEZMF_REASON_EXTENDED_ADSR;
                        break;
                    }
                }
            }
        }

        if ((reason & BAEZMF_REASON_LPF_FILTER) == 0)
        {
            bool hasFilterLfo = FALSE;
            for (lfoIdx = 0; lfoIdx < ext->lfoCount && lfoIdx < EDITOR_MAX_LFOS; ++lfoIdx)
            {
                if (PV_LfoDestinationIsLpf(ext->lfos[lfoIdx].destination))
                {
                    hasFilterLfo = TRUE;
                    break;
                }
            }
            /* Classic Beatnik supports mono LPF; stereo filter needs ZMF/ZSB. */
            if (PV_InstrumentUsesLpfFilter(ext->LPF_frequency,
                                           ext->LPF_resonance,
                                           ext->LPF_lowpassAmount,
                                           hasFilterLfo))
            {
                uint32_t si;
                for (si = 0; si < document->sampleCount; ++si)
                {
                    BAERmfEditorSample const *sample = &document->samples[si];
                    uint16_t channels;

                    if (sample->instID != (uint32_t)ext->instID)
                    {
                        continue;
                    }
                    channels = sample->sampleInfo.channels;
                    if (sample->waveform && sample->waveform->channels)
                    {
                        channels = sample->waveform->channels;
                    }
                    if (channels > 1)
                    {
                        reason |= BAEZMF_REASON_LPF_FILTER;
                        break;
                    }
                }
            }
        }
#if USE_ZMF_SUPPORT == TRUE
        if (ext->useOscillator)
        {
            reason |= BAEZMF_REASON_OSCILLATOR;
        }
#endif
    }
    int32_t engineFlags;
    BAERmfEditorDocument_GetEngineConfig(document, &engineFlags);
    if (engineFlags & SONG_CONFIG_EXTENDED_PITCH_RANGE_ON)
    {
        engineFlags &= ~SONG_CONFIG_EXTENDED_PITCH_RANGE_ON;
        reason |= BAEZMF_REASON_EXTENDED_PITCH_RANGE;
    }
    if (engineFlags & SONG_CONFIG_CLASSIC_CHORUS_ON)
    {
        engineFlags &= ~SONG_CONFIG_CLASSIC_CHORUS_ON;
        reason |= BAEZMF_REASON_CLASSIC_CHORUS;        
    }
    if (engineFlags & SONG_CONFIG_PANFIX_ON)
    {
        engineFlags &= ~SONG_CONFIG_PANFIX_ON;
        reason |= BAEZMF_REASON_PANFIX;        
    }    
    if (engineFlags & SONG_CONFIG_CONTAINER_IS_ZMF)
    {
        engineFlags &= ~SONG_CONFIG_CONTAINER_IS_ZMF;
        reason |= BAEZMF_ALREADY_ZMF;
    }
    if (engineFlags != SONG_CONFIG_UNUSED_INDEX)
    {        
        reason |= BAEZMF_REASON_OTHER;
    }
    if (outReason)
    {
        *outReason = reason;
    }
    return (reason != BAEZMF_REASON_NONE) ? TRUE : FALSE;
}


// end ZMF Requirements detection

#if USE_ZMF_SUPPORT == TRUE
/* Stamp ZREZ map version: v6 when OSCL or 2-bit ADPCM is present, otherwise v5
 * so older players can open the file. No-op for IREZ / short buffers. */
void PV_StampZmfMapVersionForOscillator(unsigned char *data,
                                               uint32_t size,
                                               bool needsV6)
{
    XFILERESOURCEMAP map;
    int32_t version;

    if (!data || size < (uint32_t)sizeof(XFILERESOURCEMAP))
    {
        return;
    }
    XBlockMove(data, &map, (int32_t)sizeof(map));
    if ((int32_t)XGetLong(&map.mapID) != XFILERESOURCE_ZMF_ID)
    {
        return;
    }
    version = needsV6 ? XFILERESOURCE_VERSION_ZMF
                      : XFILERESOURCE_VERSION_ZMF_NO_OSC;
    XPutLong(&map.version, (uint32_t)version);
    XBlockMove(&map, data, (int32_t)sizeof(map));
}


bool PV_DocumentNeedsZmfV6(BAERmfEditorDocument const *document)
{
    uint32_t reason = 0;

    if (!document)
    {
        return FALSE;
    }
    (void)BAERmfEditorDocument_RequiresZmf(document, &reason);
    return ((reason & (BAEZMF_REASON_OSCILLATOR | BAEZMF_REASON_ADPCM_2BIT)) != 0)
               ? TRUE
               : FALSE;
}


/* Keep old helper names as wrappers for call-site compatibility. */
bool PV_DocumentHasOscillator(BAERmfEditorDocument const *document)
{
    return PV_DocumentNeedsZmfV6(document);
}
#endif /* USE_ZMF_SUPPORT */


BAEResult BAERmfEditorDocument_CloneInstrumentFromBank(BAERmfEditorDocument *document,
                                                       BAEBankToken bankToken,
                                                       uint32_t instrumentIndex,
                                                       unsigned char targetProgram)
{
    enum
    {
        kInstHeaderMinSize = 14,
        kInstKeySplitSize = 8
    };
    XFILE bankFile;
    XPTR instData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    char instName[256];
    InstrumentResource *inst;
    XShortResourceID baseSndID;
    int16_t baseRootKey;
    int16_t splitCount;
    int16_t splitIndex;
    bool useSoundModifierAsRootKey;
    int16_t instMiscParam1;
    XLongResourceID targetInstID;
    uint32_t initialSampleCount;

    if (!document || !bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;
    initialSampleCount = document->sampleCount;

    rawName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, rawName, &instSize);
    if (!instData)
    {
        return BAE_BAD_FILE;
    }
    if (instSize < kInstHeaderMinSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
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
        return BAE_BAD_FILE;
    }

    useSoundModifierAsRootKey = TEST_FLAG_VALUE(inst->flags2, ZBF_useSoundModifierAsRootKey);
    instMiscParam1 = (int16_t)XGetShort(&inst->miscParameter1);

    /* Target INST ID: deterministic clone namespace (512 + program). */
    targetInstID = (XLongResourceID)(512 + (uint32_t)targetProgram);

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
                int16_t splitRoot = split.miscParameter1;
                if (split.lowMidi == split.highMidi && splitRoot == 0)
                {
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
                splitRootForLoad = 0;
            }

            sampleName[0] = 0;
            if (PV_GetEmbeddedSampleDisplayName(bankFile, split.sndResourceID, sampleName) != BAE_NO_ERROR)
            {
                XStrCpy(sampleName, instName);
            }

            /* Oscillator / sample-free splits use snd=0xFFFF — skip payload. */
            if (PV_IsNoSampleSndID(split.sndResourceID))
            {
                continue;
            }

            if (PV_AddEmbeddedSampleVariant(document,
                                            bankFile,
                                            targetInstID,
                                            sampleName,
                                            targetProgram,
                                            split.sndResourceID,
                                            splitRootForLoad,
                                            PV_ClampMidi7Bit((int32_t)split.lowMidi),
                                            PV_ClampMidi7Bit((int32_t)split.highMidi)) != BAE_NO_ERROR)
            {
                debug_message("[CloneFromBank] INST ID=%ld split=%d failed to load sndID=%d\n",
                           (long)instID, (int)splitIndex, (int)split.sndResourceID);
                while (document->sampleCount > initialSampleCount)
                {
                    BAERmfEditorDocument_DeleteSample(document, document->sampleCount - 1);
                }
                XDisposePtr(instData);
                return BAE_BAD_FILE;
            }
            else
            {
                BAERmfEditorSample *newSample = &document->samples[document->sampleCount - 1];
                /* Keep the bank SND ID from PV_AddEmbeddedSampleVariant so the
                 * embedded song can share the bank's ID space without remap
                 * collisions when the ZSB/ZSN is also loaded. */
                newSample->splitVolume = split.miscParameter2;
                if (splitCount == 1 && instName[0])
                {
                    if (newSample->displayName)
                    {
                        XDisposePtr(newSample->displayName);
                    }
                    newSample->displayName = PV_DuplicateString(instName);
                }
            }
        }
    }
    else if (!PV_IsNoSampleSndID(baseSndID))
    {
        unsigned char nonSplitRootForLoad;
        char sampleName[256];

        if (useSoundModifierAsRootKey)
        {
            nonSplitRootForLoad = PV_ClampMidi7Bit(instMiscParam1 ? instMiscParam1 : baseRootKey);
        }
        else
        {
            nonSplitRootForLoad = 0;
        }

        if (instName[0])
        {
            XStrCpy(sampleName, instName);
        }
        else
        {
            sampleName[0] = 0;
            if (PV_GetEmbeddedSampleDisplayName(bankFile, baseSndID, sampleName) != BAE_NO_ERROR)
            {
                sampleName[0] = 0;
            }
        }

        if (PV_AddEmbeddedSampleVariant(document,
                                        bankFile,
                                        targetInstID,
                                        sampleName,
                                        targetProgram,
                                        baseSndID,
                                        nonSplitRootForLoad,
                                        0,
                                        127) != BAE_NO_ERROR)
        {
            debug_message("[CloneFromBank] INST ID=%ld failed to load base sndID=%d\n",
                       (long)instID, (int)baseSndID);
            XDisposePtr(instData);
            return BAE_GENERAL_ERR;
        }
        else
        {
            document->samples[document->sampleCount - 1].splitVolume =
                (int16_t)XGetShort(&inst->miscParameter2);
        }
    }
    /* else: non-split sample-free (oscillator) — INST body only, added below. */

    /* Parse and store extended instrument data (ADSR, LFO, LPF, curves) */
    if (!PV_FindInstrumentExt(document, targetInstID))
    {
        BAERmfEditorInstrumentExt extData;
        PV_ParseExtendedInstData(instData, instSize, &extData);
        extData.instID = targetInstID;
        extData.dirty = FALSE;
        extData.displayName = instName[0] ? PV_DuplicateString(instName) : NULL;
        /* Keep raw blob for bit-perfect round-trip */
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
            }
        }
    }

    /* Sample-free clone must carry an oscillator (or other sample-less) INST. */
    if (document->sampleCount == initialSampleCount)
    {
        BAERmfEditorInstrumentExt *ext = PV_FindInstrumentExt(document, targetInstID);
#if USE_ZMF_SUPPORT == TRUE
        if (!ext || !ext->useOscillator)
#else
        if (!ext)
#endif
        {
            debug_message("[CloneFromBank] INST ID=%ld has no samples and is not oscillator\n",
                          (long)instID);
            XDisposePtr(instData);
            return BAE_BAD_INSTRUMENT;
        }
    }

    XDisposePtr(instData);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_AliasInstrumentFromBank(BAERmfEditorDocument *document,
                                                       BAEBankToken bankToken,
                                                       uint32_t instrumentIndex,
                                                       unsigned char targetProgram)
{
    enum
    {
        kInstHeaderMinSize = 14,
        kInstKeySplitSize = 8
    };
    XFILE bankFile;
    XPTR instData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    char instName[256];
    InstrumentResource *inst;
    XShortResourceID baseSndID;
    int16_t baseRootKey;
    int16_t splitCount;
    int16_t splitIndex;
    bool useSoundModifierAsRootKey;
    int16_t instMiscParam1;
    XLongResourceID targetInstID;
    uint32_t initialSampleCount;

    if (!document || !bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;
    initialSampleCount = document->sampleCount;

    rawName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, rawName, &instSize);
    if (!instData)
    {
        return BAE_BAD_FILE;
    }
    if (instSize < kInstHeaderMinSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
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
        return BAE_BAD_FILE;
    }

    useSoundModifierAsRootKey = TEST_FLAG_VALUE(inst->flags2, ZBF_useSoundModifierAsRootKey);
    instMiscParam1 = (int16_t)XGetShort(&inst->miscParameter1);

    targetInstID = (XLongResourceID)(512 + (uint32_t)targetProgram);

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
                int16_t splitRoot = split.miscParameter1;
                if (split.lowMidi == split.highMidi && splitRoot == 0)
                {
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
                splitRootForLoad = 0;
            }

            sampleName[0] = 0;
            if (PV_GetEmbeddedSampleDisplayName(bankFile, split.sndResourceID, sampleName) != BAE_NO_ERROR)
            {
                XStrCpy(sampleName, instName);
            }

            if (PV_IsNoSampleSndID(split.sndResourceID))
            {
                continue;
            }

            if (PV_AddBankAliasSample(document,
                                      bankFile,
                                      bankToken,
                                      targetInstID,
                                      sampleName,
                                      targetProgram,
                                      split.sndResourceID,
                                      splitRootForLoad,
                                      PV_ClampMidi7Bit((int32_t)split.lowMidi),
                                      PV_ClampMidi7Bit((int32_t)split.highMidi)) != BAE_NO_ERROR)
            {
                debug_message("[AliasFromBank] INST ID=%ld split=%d failed sndID=%d\n",
                           (long)instID, (int)splitIndex, (int)split.sndResourceID);
            }
            else
            {
                document->samples[document->sampleCount - 1].splitVolume = split.miscParameter2;
            }
        }
    }
    else if (!PV_IsNoSampleSndID(baseSndID))
    {
        unsigned char nonSplitRootForLoad;
        char sampleName[256];

        if (useSoundModifierAsRootKey)
        {
            nonSplitRootForLoad = PV_ClampMidi7Bit(instMiscParam1 ? instMiscParam1 : baseRootKey);
        }
        else
        {
            nonSplitRootForLoad = 0;
        }

        sampleName[0] = 0;
        if (PV_GetEmbeddedSampleDisplayName(bankFile, baseSndID, sampleName) != BAE_NO_ERROR)
        {
            XStrCpy(sampleName, instName);
        }

        if (PV_AddBankAliasSample(document,
                                  bankFile,
                                  bankToken,
                                  targetInstID,
                                  sampleName,
                                  targetProgram,
                                  baseSndID,
                                  nonSplitRootForLoad,
                                  0,
                                  127) != BAE_NO_ERROR)
        {
            debug_message("[AliasFromBank] INST ID=%ld failed base sndID=%d\n",
                       (long)instID, (int)baseSndID);
            XDisposePtr(instData);
            return BAE_GENERAL_ERR;
        }
        else
        {
            document->samples[document->sampleCount - 1].splitVolume =
                (int16_t)XGetShort(&inst->miscParameter2);
        }
    }
    /* else: sample-free oscillator — INST body only. */

    /* Parse and store extended instrument data (ADSR, LFO, LPF, curves) */
    if (!PV_FindInstrumentExt(document, targetInstID))
    {
        BAERmfEditorInstrumentExt extData;
        PV_ParseExtendedInstData(instData, instSize, &extData);
        extData.instID = targetInstID;
        extData.dirty = FALSE;
        extData.displayName = instName[0] ? PV_DuplicateString(instName) : NULL;
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
            }
        }
    }

    if (document->sampleCount == initialSampleCount)
    {
        BAERmfEditorInstrumentExt *ext = PV_FindInstrumentExt(document, targetInstID);
#if USE_ZMF_SUPPORT == TRUE
        if (!ext || !ext->useOscillator)
#else
        if (!ext)
#endif
        {
            debug_message("[AliasFromBank] INST ID=%ld has no samples and is not oscillator\n",
                          (long)instID);
            XDisposePtr(instData);
            return BAE_BAD_INSTRUMENT;
        }
    }

    XDisposePtr(instData);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_AddBankAliasSample(BAERmfEditorDocument *document,
                                                  BAEBankToken bankToken,
                                                  uint32_t targetInstID,
                                                  unsigned char targetProgram,
                                                  XShortResourceID sndID,
                                                  char const *displayName,
                                                  unsigned char rootKey,
                                                  unsigned char lowKey,
                                                  unsigned char highKey,
                                                  uint32_t *outSampleIndex,
                                                  BAERmfEditorSampleInfo *outSampleInfo)
{
    BAEResult result;
    uint32_t sampleIndex;

    if (!document || !bankToken)
    {
        return BAE_PARAM_ERR;
    }
    if (targetProgram > 127 || lowKey > 127 || highKey > 127 || lowKey > highKey)
    {
        return BAE_PARAM_ERR;
    }

    result = PV_AddBankAliasSample(document,
                                   (XFILE)bankToken,
                                   bankToken,
                                   (XLongResourceID)targetInstID,
                                   displayName,
                                   targetProgram,
                                   sndID,
                                   rootKey,
                                   lowKey,
                                   highKey);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    sampleIndex = document->sampleCount - 1;
    if (outSampleIndex)
    {
        *outSampleIndex = sampleIndex;
    }
    if (outSampleInfo)
    {
        BAERmfEditorDocument_GetSampleInfo(document, sampleIndex, outSampleInfo);
    }

    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_IsSampleBankAlias(BAERmfEditorDocument const *document,
                                                  uint32_t sampleIndex,
                                                  bool *outIsAlias)
{
    if (!document || !outIsAlias)
    {
        return BAE_PARAM_ERR;
    }
    if (sampleIndex >= document->sampleCount)
    {
        return BAE_PARAM_ERR;
    }
    *outIsAlias = document->samples[sampleIndex].isBankAlias;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_CloneInstrumentFromBankToInstID(
    BAERmfEditorDocument *document,
    BAEBankToken bankToken,
    uint32_t instrumentIndex,
    uint32_t targetInstID,
    unsigned char targetProgram)
{
    enum
    {
        kInstHeaderMinSize = 14,
        kInstKeySplitSize = 8
    };
    XFILE bankFile;
    XPTR instData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    char instName[256];
    InstrumentResource *inst;
    XShortResourceID baseSndID;
    int16_t baseRootKey;
    int16_t splitCount;
    int16_t splitIndex;
    bool useSoundModifierAsRootKey;
    int16_t instMiscParam1;
    uint32_t initialSampleCount;

    if (!document || !bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;
    initialSampleCount = document->sampleCount;

    rawName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, rawName, &instSize);
    if (!instData)
    {
        return BAE_BAD_FILE;
    }
    if (instSize < kInstHeaderMinSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
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
        return BAE_BAD_FILE;
    }

    useSoundModifierAsRootKey = TEST_FLAG_VALUE(inst->flags2, ZBF_useSoundModifierAsRootKey);
    instMiscParam1 = (int16_t)XGetShort(&inst->miscParameter1);

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
                int16_t splitRoot = split.miscParameter1;
                if (split.lowMidi == split.highMidi && splitRoot == 0)
                {
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
                /* Match RMF load: root comes from SND baseFrequency, not INST midiRootKey. */
                splitRootForLoad = 0;
            }

            sampleName[0] = 0;
            if (PV_GetEmbeddedSampleDisplayName(bankFile, split.sndResourceID, sampleName) != BAE_NO_ERROR)
            {
                XStrCpy(sampleName, instName);
            }

            if (PV_IsNoSampleSndID(split.sndResourceID))
            {
                continue;
            }

            if (PV_AddEmbeddedSampleVariant(document,
                                            bankFile,
                                            (XLongResourceID)targetInstID,
                                            sampleName,
                                            targetProgram,
                                            split.sndResourceID,
                                            splitRootForLoad,
                                            PV_ClampMidi7Bit((int32_t)split.lowMidi),
                                            PV_ClampMidi7Bit((int32_t)split.highMidi)) != BAE_NO_ERROR)
            {
                debug_message("[CloneToInstID] INST ID=%ld split=%d failed to load sndID=%d\n",
                           (long)instID, (int)splitIndex, (int)split.sndResourceID);
                while (document->sampleCount > initialSampleCount)
                {
                    BAERmfEditorDocument_DeleteSample(document, document->sampleCount - 1);
                }
                XDisposePtr(instData);
                return BAE_BAD_FILE;
            }
            else
            {
                BAERmfEditorSample *newSample = &document->samples[document->sampleCount - 1];
                /* Keep bank SND ID (see CloneFromBank) — avoid remap collisions
                 * with the parent ZSB/ZSN when both are open. */
                newSample->splitVolume = split.miscParameter2;
                /* Single-split bank instruments: surface the INST name (e.g. Syn Drum)
                 * rather than the underlying SND name (e.g. tom 2). */
                if (splitCount == 1 && instName[0])
                {
                    if (newSample->displayName)
                    {
                        XDisposePtr(newSample->displayName);
                    }
                    newSample->displayName = PV_DuplicateString(instName);
                }
            }
        }
    }
    else if (!PV_IsNoSampleSndID(baseSndID))
    {
        unsigned char nonSplitRootForLoad;
        char sampleName[256];

        if (useSoundModifierAsRootKey)
        {
            nonSplitRootForLoad = PV_ClampMidi7Bit(instMiscParam1 ? instMiscParam1 : baseRootKey);
        }
        else
        {
            nonSplitRootForLoad = 0;
        }

        /* Prefer INST name for the lone sample so editors don't show the SND name. */
        if (instName[0])
        {
            XStrCpy(sampleName, instName);
        }
        else
        {
            sampleName[0] = 0;
            if (PV_GetEmbeddedSampleDisplayName(bankFile, baseSndID, sampleName) != BAE_NO_ERROR)
            {
                sampleName[0] = 0;
            }
        }

        if (PV_AddEmbeddedSampleVariant(document,
                                        bankFile,
                                        (XLongResourceID)targetInstID,
                                        sampleName,
                                        targetProgram,
                                        baseSndID,
                                        nonSplitRootForLoad,
                                        0,
                                        127) != BAE_NO_ERROR)
        {
            debug_message("[CloneToInstID] INST ID=%ld failed to load base sndID=%d\n",
                       (long)instID, (int)baseSndID);
            XDisposePtr(instData);
            return BAE_GENERAL_ERR;
        }
        else
        {
            document->samples[document->sampleCount - 1].splitVolume =
                (int16_t)XGetShort(&inst->miscParameter2);
        }
    }
    /* else: non-split sample-free (oscillator) — INST body only, added below. */

    /* Parse and store extended instrument data (ADSR, LFO, LPF, curves) */
    if (!PV_FindInstrumentExt(document, (XLongResourceID)targetInstID))
    {
        BAERmfEditorInstrumentExt extData;
        PV_ParseExtendedInstData(instData, instSize, &extData);
        extData.instID = (XLongResourceID)targetInstID;
        extData.dirty = FALSE;
        extData.displayName = instName[0] ? PV_DuplicateString(instName) : NULL;
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
            }
        }
    }

    if (document->sampleCount == initialSampleCount)
    {
        BAERmfEditorInstrumentExt *ext =
            PV_FindInstrumentExt(document, (XLongResourceID)targetInstID);
#if USE_ZMF_SUPPORT == TRUE
        if (!ext || !ext->useOscillator)
#else
        if (!ext)
#endif
        {
            debug_message("[CloneToInstID] INST ID=%ld has no samples and is not oscillator\n",
                          (long)instID);
            XDisposePtr(instData);
            return BAE_BAD_INSTRUMENT;
        }
    }

    XDisposePtr(instData);
    return BAE_NO_ERROR;
}


bool PV_AddUsedInstrumentPair(PV_UsedInstrumentPair *pairs,
                                     uint32_t *pairCount,
                                     uint32_t pairCapacity,
                                     uint16_t bank,
                                     unsigned char program)
{
    uint32_t pairIndex;

    for (pairIndex = 0; pairIndex < *pairCount; ++pairIndex)
    {
        if (pairs[pairIndex].bank == bank && pairs[pairIndex].program == program)
        {
            return TRUE;
        }
    }
    if (*pairCount >= pairCapacity)
    {
        return FALSE;
    }
    pairs[*pairCount].bank = bank;
    pairs[*pairCount].program = program;
    (*pairCount)++;
    return TRUE;
}


int PV_CompareUsedInstrumentPairs(void const *left, void const *right)
{
    PV_UsedInstrumentPair const *leftPair = (PV_UsedInstrumentPair const *)left;
    PV_UsedInstrumentPair const *rightPair = (PV_UsedInstrumentPair const *)right;

    if (leftPair->bank != rightPair->bank)
    {
        return leftPair->bank < rightPair->bank ? -1 : 1;
    }
    if (leftPair->program != rightPair->program)
    {
        return leftPair->program < rightPair->program ? -1 : 1;
    }
    return 0;
}


bool PV_AddUsedPercussion(PV_UsedPercussion *entries,
                                 uint32_t *entryCount,
                                 uint32_t entryCapacity,
                                 uint16_t bank,
                                 unsigned char note)
{
    uint32_t entryIndex;

    for (entryIndex = 0; entryIndex < *entryCount; ++entryIndex)
    {
        if (entries[entryIndex].bank == bank && entries[entryIndex].note == note)
        {
            return TRUE;
        }
    }
    if (*entryCount >= entryCapacity)
    {
        return FALSE;
    }
    entries[*entryCount].bank = bank;
    entries[*entryCount].note = note;
    (*entryCount)++;
    return TRUE;
}


void PV_RemapPercussionNoteReferences(BAERmfEditorDocument *document,
                                              uint16_t sourceBank,
                                              unsigned char sourceNote,
                                              uint16_t targetBank,
                                              unsigned char targetProgram)
{
    uint32_t trackIndex;

    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack *track = &document->tracks[trackIndex];
        uint32_t noteIndex;

        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote *note = &track->notes[noteIndex];
            if (note->channel == 9 && note->bank == sourceBank && note->note == sourceNote)
            {
                note->bank = targetBank;
                note->program = targetProgram;
            }
        }
    }
}


void PV_RemapPitchedNoteReferences(BAERmfEditorDocument *document,
                                           uint16_t sourceBank,
                                           unsigned char sourceProgram,
                                           uint16_t targetBank,
                                           unsigned char targetProgram)
{
    uint32_t trackIndex;

    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack *track = &document->tracks[trackIndex];
        uint32_t noteIndex;

        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote *note = &track->notes[noteIndex];
            /* Classic GM drums (ch10, bank 0) use note→INST; melodic ch10
               (CloneUsed bank-2 embeds) follows bank+program like pitched. */
            if (note->channel == 9 && note->bank == 0)
            {
                continue;
            }
            if (note->bank == sourceBank ||
                PV_BankGroupFromInternalBank(note->bank) == PV_BankGroupFromInternalBank(sourceBank))
            {
                if (note->program == sourceProgram)
                {
                    note->bank = targetBank;
                    note->program = targetProgram;
                }
            }
        }
    }
}


void PV_SynchronizeTrackInstrumentDefaults(BAERmfEditorDocument *document)
{
    uint32_t trackIndex;

    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack *track = &document->tracks[trackIndex];
        uint32_t noteIndex;
        BAERmfEditorNote const *firstNote = NULL;

        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote const *note = &track->notes[noteIndex];
            if (!firstNote || note->startTick < firstNote->startTick ||
                (note->startTick == firstNote->startTick && note->noteOnOrder < firstNote->noteOnOrder))
            {
                firstNote = note;
            }
        }
        if (firstNote)
        {
            track->channel = firstNote->channel;
            if (firstNote->channel == 9 && firstNote->bank == 0)
            {
                /* Classic GM drums: note selects INST; keep track bank/program 0. */
                track->bank = 0;
                track->program = 0;
            }
            else
            {
                /* Melodic tracks, or CH10 kit banks (non-zero bank group). */
                track->bank = firstNote->bank;
                track->program = firstNote->program;
            }
        }
        else
        {
            track->bank = 0;
            track->program = 0;
        }
    }
}


void PV_AddCloneUsedMapping(BAERmfEditorCloneUsedResult *outResult,
                                   uint16_t sourceBank,
                                   unsigned char sourceProgram,
                                   bool isPercussion,
                                   uint32_t requestedInstID,
                                   uint32_t resolvedInstID,
                                   uint32_t targetInstID,
                                   uint32_t sampleCount,
                                   char const *resolvedName)
{
    BAERmfEditorCloneUsedMapping *mapping;

    if (!outResult || outResult->mappingCount >= BAE_RMF_EDITOR_MAX_CLONE_MAPPINGS)
    {
        return;
    }
    mapping = &outResult->mappings[outResult->mappingCount++];
    mapping->sourceBank = sourceBank;
    mapping->sourceProgram = sourceProgram;
    mapping->isPercussion = isPercussion ? 1 : 0;
    mapping->requestedInstID = requestedInstID;
    mapping->resolvedInstID = resolvedInstID;
    mapping->targetInstID = targetInstID;
    mapping->sampleCount = sampleCount;
    XStrCpy(mapping->resolvedName, resolvedName ? resolvedName : "");
}


BAE_BOOL PV_ExportShouldEmbedInst(uint32_t resolvedInstID,
                                         BAE_BOOL embedAll,
                                         uint32_t const *embedResolvedInstIDs,
                                         uint32_t embedCount)
{
    uint32_t i;

    if (embedAll)
    {
        return TRUE;
    }
    if (!embedResolvedInstIDs || embedCount == 0)
    {
        return FALSE;
    }
    for (i = 0; i < embedCount; ++i)
    {
        if (embedResolvedInstIDs[i] == resolvedInstID)
        {
            return TRUE;
        }
    }
    return FALSE;
}


bool PV_DocumentHasEmbeddedInstID(BAERmfEditorDocument const *document, uint32_t instID)
{
    uint32_t i;
    /* INST id 0 is valid — only reject a null document. */
    if (!document)
    {
        return FALSE;
    }
    for (i = 0; i < document->originalResourceCount; ++i)
    {
        if (document->originalResources[i].type == ID_INST &&
            (uint32_t)document->originalResources[i].id == instID)
        {
            return TRUE;
        }
    }
    for (i = 0; i < document->instrumentExtCount; ++i)
    {
        if ((uint32_t)document->instrumentExts[i].instID == instID)
        {
            return TRUE;
        }
    }
    for (i = 0; i < document->sampleCount; ++i)
    {
        if (document->samples[i].instID == instID)
        {
            return TRUE;
        }
    }
    return FALSE;
}


BAEResult BAERmfEditorDocument_CloneUsedInstrumentsFromBank(
    BAERmfEditorDocument *document,
    BAEBankToken bankToken,
    BAERmfEditorCloneUsedResult *outResult)
{
    return PV_PrepareUsedInstrumentsFromBank(document,
                                             bankToken,
                                             TRUE,
                                             NULL,
                                             0,
                                             outResult);
}


BAEResult BAERmfEditorDocument_ExportUsedInstrumentsFromBank(
    BAERmfEditorDocument *document,
    BAEBankToken bankToken,
    uint32_t const *embedResolvedInstIDs,
    uint32_t embedCount,
    BAERmfEditorCloneUsedResult *outResult)
{
    return PV_PrepareUsedInstrumentsFromBank(document,
                                             bankToken,
                                             FALSE,
                                             embedResolvedInstIDs,
                                             embedCount,
                                             outResult);
}


static BAEResult PV_OpenSongFileWritableAdopt(BAEPathName songPath,
                                              XFILENAME *outSongName,
                                              XFILE *outSongFile)
{
    XFILE songFile;
    XPTR songImage;
    int32_t songImageSize;

    if (!songPath || !outSongName || !outSongFile)
    {
        return BAE_PARAM_ERR;
    }
    *outSongFile = NULL;
    XConvertPathToXFILENAME(songPath, outSongName);
    /* Disk-backed XFileOpenResource has no pResourceData — GetMemoryFileAsData
     * always fails. Read bytes, then adopt into a writable memory resource. */
    songImage = NULL;
    songImageSize = 0;
    if (XGetFileAsData(outSongName, &songImage, &songImageSize) != 0 ||
        !songImage || songImageSize <= 0)
    {
        if (songImage)
        {
            XDisposePtr(songImage);
        }
        return BAE_FILE_IO_ERROR;
    }
    songFile = XFileOpenWritableResourceAdoptMemory(songImage, (uint32_t)songImageSize);
    if (!songFile)
    {
        XDisposePtr(songImage);
        return BAE_MEMORY_ERR;
    }
    *outSongFile = songFile;
    return BAE_NO_ERROR;
}

static BAEResult PV_WriteBackAdoptedSongFile(XFILENAME *songName, XFILE *songFile)
{
    XPTR outData;
    int32_t outSize;
    XFILE outFile;

    if (!songName || !songFile || !*songFile)
    {
        return BAE_PARAM_ERR;
    }
    if (XPackResourceFileForShip(*songFile) == FALSE)
    {
        return BAE_FILE_IO_ERROR;
    }
    outData = NULL;
    outSize = 0;
    if (XFileGetMemoryFileAsData(*songFile, &outData, &outSize) != 0 ||
        !outData || outSize <= 0)
    {
        if (outData)
        {
            XDisposePtr(outData);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(*songFile);
    *songFile = NULL;
    outFile = XFileOpenForWrite(songName, TRUE);
    if (!outFile)
    {
        XDisposePtr(outData);
        return BAE_FILE_IO_ERROR;
    }
    if (XFileSetLength(outFile, 0) != 0 ||
        XFileSetPosition(outFile, 0L) != 0 ||
        XFileWrite(outFile, outData, outSize) != 0)
    {
        XFileClose(outFile);
        XDisposePtr(outData);
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(outFile);
    XDisposePtr(outData);
    return BAE_NO_ERROR;
}

static BAEResult PV_InjectOneInstFromBank(XFILE songFile,
                                          BAEBankToken bankToken,
                                          uint32_t wantID)
{
    XFILE bankFile;
    XLongResourceID bankInstID;
    XPTR instData;
    int32_t instSize;
    char rawName[256];
    uint32_t resolvedID;
    uint32_t instrumentIndex;

    if (!songFile || !bankToken || wantID == BAE_EDITOR_INST_ID_NONE)
    {
        return BAE_PARAM_ERR;
    }
    if (XExistsFileResource(songFile, ID_INST, (XLongResourceID)wantID) != FALSE)
    {
        return BAE_NO_ERROR;
    }
    resolvedID = wantID;
    instrumentIndex = 0;
    if (BAERmfEditorBank_ResolveInstID(bankToken, wantID, &resolvedID, &instrumentIndex) !=
        BAE_NO_ERROR)
    {
        return BAE_BAD_INSTRUMENT;
    }
    bankFile = (XFILE)bankToken;
    bankInstID = (XLongResourceID)resolvedID;
    rawName[0] = 0;
    instData = XGetFileResource(bankFile, ID_INST, bankInstID, rawName, &instSize);
    if (!instData || instSize < 14)
    {
        if (instData)
        {
            XDisposePtr(instData);
        }
        /* Indexed fallback when id lookup misses but Resolve gave an index. */
        bankInstID = (XLongResourceID)resolvedID;
        instData = XGetIndexedFileResource(bankFile,
                                           ID_INST,
                                           &bankInstID,
                                           (int32_t)instrumentIndex,
                                           rawName,
                                           &instSize);
        if (!instData || instSize < 14)
        {
            if (instData)
            {
                XDisposePtr(instData);
            }
            return BAE_BAD_INSTRUMENT;
        }
    }
    if (XAddFileResource(songFile,
                         ID_INST,
                         (XLongResourceID)wantID,
                         rawName,
                         instData,
                         instSize) != 0)
    {
        XDisposePtr(instData);
        return BAE_FILE_IO_ERROR;
    }
    XDisposePtr(instData);
    debug_message("[InjectInst] wrote INST id=%lu (%ld bytes) from bank into song\n",
                  (unsigned long)wantID, (long)instSize);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditor_InjectMissingInstFromBankIntoSongFile(
    BAEPathName songPath,
    BAEBankToken bankToken,
    uint32_t const *instIDs,
    uint32_t instCount)
{
    XFILENAME songName;
    XFILE songFile;
    uint32_t i;
    uint32_t injected;
    BAEResult result;

    if (!songPath || !bankToken || !instIDs || instCount == 0)
    {
        return BAE_PARAM_ERR;
    }

    result = PV_OpenSongFileWritableAdopt(songPath, &songName, &songFile);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    injected = 0;
    for (i = 0; i < instCount; ++i)
    {
        if (instIDs[i] == BAE_EDITOR_INST_ID_NONE)
        {
            continue;
        }
        if (XExistsFileResource(songFile, ID_INST, (XLongResourceID)instIDs[i]) != FALSE)
        {
            continue;
        }
        result = PV_InjectOneInstFromBank(songFile, bankToken, instIDs[i]);
        if (result != BAE_NO_ERROR)
        {
            break;
        }
        injected++;
    }

    if (result == BAE_NO_ERROR && injected > 0)
    {
        result = PV_WriteBackAdoptedSongFile(&songName, &songFile);
    }

    if (songFile)
    {
        XFileClose(songFile);
    }
    return result;
}

BAEResult BAERmfEditorDocument_PatchMissingInstIntoSongFile(
    BAERmfEditorDocument *document,
    BAEPathName songPath,
    BAEBankToken bankToken,
    uint32_t const *instIDs,
    uint32_t instCount)
{
    XFILENAME songName;
    XFILE songFile;
    uint32_t i;
    uint32_t patched;
    BAEResult result;

    if (!document || !songPath || !instIDs || instCount == 0)
    {
        return BAE_PARAM_ERR;
    }

    result = PV_OpenSongFileWritableAdopt(songPath, &songName, &songFile);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    patched = 0;
#if USE_ZMF_SUPPORT == TRUE
    {
        /* Rebuild any still-missing sample-free OSCL INST from live ExtInfo. */
        uint32_t beforeMissing = 0;
        for (i = 0; i < instCount; ++i)
        {
            if (instIDs[i] != BAE_EDITOR_INST_ID_NONE &&
                XExistsFileResource(songFile, ID_INST, (XLongResourceID)instIDs[i]) == FALSE)
            {
                beforeMissing++;
            }
        }
        result = PV_AddSampleFreeInstrumentResources(document, songFile);
        if (result != BAE_NO_ERROR)
        {
            XFileClose(songFile);
            return result;
        }
        for (i = 0; i < instCount; ++i)
        {
            if (instIDs[i] != BAE_EDITOR_INST_ID_NONE &&
                XExistsFileResource(songFile, ID_INST, (XLongResourceID)instIDs[i]) != FALSE)
            {
                /* count reductions below */
            }
        }
        {
            uint32_t afterMissing = 0;
            for (i = 0; i < instCount; ++i)
            {
                if (instIDs[i] != BAE_EDITOR_INST_ID_NONE &&
                    XExistsFileResource(songFile, ID_INST, (XLongResourceID)instIDs[i]) == FALSE)
                {
                    afterMissing++;
                }
            }
            if (afterMissing < beforeMissing)
            {
                patched += (beforeMissing - afterMissing);
            }
        }
    }
#endif

    for (i = 0; i < instCount; ++i)
    {
        BAERmfEditorInstrumentExt *ext;

        if (instIDs[i] == BAE_EDITOR_INST_ID_NONE)
        {
            continue;
        }
        if (XExistsFileResource(songFile, ID_INST, (XLongResourceID)instIDs[i]) != FALSE)
        {
            continue;
        }
        ext = PV_FindInstrumentExt(document, (XLongResourceID)instIDs[i]);
        if (ext && ext->originalInstData && ext->originalInstSize >= 14)
        {
            char pascalName[256];
            BAEResult nameResult = PV_CreatePascalName(
                (ext->displayName && ext->displayName[0]) ? ext->displayName : "Instrument",
                pascalName);
            if (nameResult != BAE_NO_ERROR)
            {
                XFileClose(songFile);
                return nameResult;
            }
            if (XAddFileResource(songFile,
                                 ID_INST,
                                 (XLongResourceID)instIDs[i],
                                 pascalName,
                                 ext->originalInstData,
                                 ext->originalInstSize) != 0)
            {
                XFileClose(songFile);
                return BAE_FILE_IO_ERROR;
            }
            patched++;
            debug_message("[PatchInst] wrote INST id=%lu verbatim (%ld bytes) from document\n",
                          (unsigned long)instIDs[i], (long)ext->originalInstSize);
            continue;
        }
        if (bankToken)
        {
            result = PV_InjectOneInstFromBank(songFile, bankToken, instIDs[i]);
            if (result != BAE_NO_ERROR)
            {
                XFileClose(songFile);
                return result;
            }
            patched++;
        }
    }

    for (i = 0; i < instCount; ++i)
    {
        if (instIDs[i] == BAE_EDITOR_INST_ID_NONE)
        {
            continue;
        }
        if (XExistsFileResource(songFile, ID_INST, (XLongResourceID)instIDs[i]) == FALSE)
        {
            debug_message("[PatchInst] INST id=%lu still missing after patch — refusing silent export\n",
                          (unsigned long)instIDs[i]);
            XFileClose(songFile);
            return BAE_BAD_INSTRUMENT;
        }
    }

    if (patched > 0)
    {
        result = PV_WriteBackAdoptedSongFile(&songName, &songFile);
    }
    if (songFile)
    {
        XFileClose(songFile);
    }
    return result;
}


BAEResult BAERmfEditorDocument_SaveAsRmfToMemoryEx(BAERmfEditorDocument *document,
                                                   bool useZmfContainer,
                                                   bool packForShip,
                                                   unsigned char **outData,
                                                   uint32_t *outSize)
{
    XFILE fileRef;
    XPTR data;
    int32_t size;
    BAEResult result;
    int32_t resourceID;

    if (!document || !outData || !outSize)
    {
        return BAE_PARAM_ERR;
    }

    *outData = NULL;
    *outSize = 0;

#if USE_ZMF_SUPPORT != TRUE
    if (useZmfContainer)
    {
        return BAE_UNSUPPORTED_FORMAT;
    }
#endif

    resourceID = useZmfContainer ? XFILERESOURCE_ZMF_ID : XFILERESOURCE_ID;

    fileRef = XFileOpenVirtualResource(resourceID);
    if (!fileRef)
    {
        return BAE_FILE_IO_ERROR;
    }

    result = PV_WriteRmfDocumentToResourceFile(document, fileRef, resourceID, FALSE, packForShip);
    if (result != BAE_NO_ERROR)
    {
        XFileClose(fileRef);
        return result;
    }

    data = NULL;
    size = 0;
    if (XFileGetMemoryFileAsData(fileRef, &data, &size) != 0 || !data || size <= 0)
    {
        XFileClose(fileRef);
        if (data)
        {
            XDisposePtr(data);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(fileRef);

    *outData = (unsigned char *)data;
    *outSize = (uint32_t)size;
#if USE_ZMF_SUPPORT == TRUE
    if (useZmfContainer)
    {
        PV_StampZmfMapVersionForOscillator(*outData, *outSize,
                                           PV_DocumentHasOscillator(document));
    }
#endif
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SaveAsRmfToMemory(BAERmfEditorDocument *document,
                                                 bool useZmfContainer,
                                                 unsigned char **outData,
                                                 uint32_t *outSize)
{
    /* Default = ship pack (export / converters). Preview uses SaveAsRmfToMemoryEx(..., FALSE). */
    return BAERmfEditorDocument_SaveAsRmfToMemoryEx(document,
                                                    useZmfContainer,
                                                    TRUE,
                                                    outData,
                                                    outSize);
}


BAEResult BAERmfEditorDocument_SaveAsRmf(BAERmfEditorDocument *document,
                                         BAEPathName filePath)
{
    unsigned char *rmfData;
    uint32_t rmfSize;
    XFILENAME name;
    XFILE fileRef;
    BAEResult result;
    bool useZmfContainer;
    const char *ext;

    if (!document || !filePath)
    {
        return BAE_PARAM_ERR;
    }

    /* Choose ZREZ header for .zmf files, IREZ for everything else */
    ext = strrchr(filePath, '.');
    if (ext && (strcmp(ext, ".zmf") == 0 || strcmp(ext, ".ZMF") == 0))
    {
#if USE_ZMF_SUPPORT == TRUE
        useZmfContainer = TRUE;
#else
        return BAE_UNSUPPORTED_FORMAT;
#endif
    }
    else
    {
        uint32_t zmfReason = 0;
        if (BAERmfEditorDocument_RequiresZmf(document, &zmfReason) != FALSE)
        {
            /* Ignore "already ZMF" container bit — only content incompatibilities block RMF. */
            zmfReason &= ~(uint32_t)BAEZMF_ALREADY_ZMF;
            if (zmfReason != BAEZMF_REASON_NONE)
            {
                return BAE_UNSUPPORTED_FORMAT;
            }
        }
        useZmfContainer = FALSE;
    }

    rmfData = NULL;
    rmfSize = 0;
    result = BAERmfEditorDocument_SaveAsRmfToMemory(document,
                                                    useZmfContainer,
                                                    &rmfData,
                                                    &rmfSize);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    XConvertPathToXFILENAME(filePath, &name);
    fileRef = XFileOpenForWrite(&name, TRUE);
    if (!fileRef)
    {
        XDisposePtr((XPTR)rmfData);
        return BAE_FILE_IO_ERROR;
    }

    if (XFileSetLength(fileRef, 0) != 0 ||
        XFileSetPosition(fileRef, 0L) != 0 ||
        XFileWrite(fileRef, rmfData, (int32_t)rmfSize) != 0)
    {
        XFileClose(fileRef);
        XDisposePtr((XPTR)rmfData);
        return BAE_FILE_IO_ERROR;
    }

    XFileClose(fileRef);
    XDisposePtr((XPTR)rmfData);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SaveAsRmfPreserveMidi(BAERmfEditorDocument *document,
                                                     BAEPathName filePath)
{
    unsigned char *rmfData;
    uint32_t rmfSize;
    XFILENAME name;
    XFILE fileRef;
    BAEResult result;
    bool useZmfContainer;
    const char *ext;
    XPTR data;
    int32_t size;

    if (!document || !filePath)
    {
        return BAE_PARAM_ERR;
    }

    ext = strrchr(filePath, '.');
    if (ext && (strcmp(ext, ".zmf") == 0 || strcmp(ext, ".ZMF") == 0))
    {
#if USE_ZMF_SUPPORT == TRUE
        useZmfContainer = TRUE;
#else
        return BAE_UNSUPPORTED_FORMAT;
#endif
    }
    else
    {
        useZmfContainer = FALSE;
    }

    fileRef = XFileOpenVirtualResource(useZmfContainer ? XFILERESOURCE_ZMF_ID : XFILERESOURCE_ID);
    if (!fileRef)
    {
        return BAE_FILE_IO_ERROR;
    }

    result = PV_WriteRmfDocumentToResourceFile(document,
                                               fileRef,
                                               useZmfContainer ? XFILERESOURCE_ZMF_ID : XFILERESOURCE_ID,
                                               TRUE,
                                               TRUE);
    if (result != BAE_NO_ERROR)
    {
        XFileClose(fileRef);
        return result;
    }

    data = NULL;
    size = 0;
    if (XFileGetMemoryFileAsData(fileRef, &data, &size) != 0 || !data || size <= 0)
    {
        XFileClose(fileRef);
        if (data)
        {
            XDisposePtr(data);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(fileRef);

    rmfData = (unsigned char *)data;
    rmfSize = (uint32_t)size;
#if USE_ZMF_SUPPORT == TRUE
    if (useZmfContainer)
    {
        PV_StampZmfMapVersionForOscillator(rmfData, rmfSize,
                                           PV_DocumentHasOscillator(document));
    }
#endif

    XConvertPathToXFILENAME(filePath, &name);
    fileRef = XFileOpenForWrite(&name, TRUE);
    if (!fileRef)
    {
        XDisposePtr((XPTR)rmfData);
        return BAE_FILE_IO_ERROR;
    }

    if (XFileSetLength(fileRef, 0) != 0 ||
        XFileSetPosition(fileRef, 0L) != 0 ||
        XFileWrite(fileRef, rmfData, (int32_t)rmfSize) != 0)
    {
        XFileClose(fileRef);
        XDisposePtr((XPTR)rmfData);
        return BAE_FILE_IO_ERROR;
    }

    XFileClose(fileRef);
    XDisposePtr((XPTR)rmfData);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetFileVersion(BAEPathName filePath, int32_t *outVersion)
{
    XFILENAME name;
    XFILE fileRef;
    XFILERESOURCEMAP map;

    if (!filePath || !outVersion)
    {
        return BAE_PARAM_ERR;
    }

    XConvertPathToXFILENAME(filePath, &name);
    fileRef = XFileOpenResource(&name, TRUE);
    if (!fileRef)
    {
        return BAE_FILE_NOT_FOUND;
    }

    if (XFileSetPosition(fileRef, 0L) != 0 ||
        XFileRead(fileRef, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        XFileClose(fileRef);
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(fileRef);

    if (!XFILERESOURCE_ID_IS_VALID(XGetLong(&map.mapID)))
    {
        return BAE_BAD_FILE;
    }

    *outVersion = (int32_t)XGetLong(&map.version);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_UpgradeFile(BAEPathName srcPath,
                                           BAEPathName dstPath,
                                           int32_t *outFromVersion)
{
    BAERmfEditorDocument *document;
    int32_t fromVersion;
    BAEResult result;

    if (!srcPath || !dstPath)
    {
        return BAE_PARAM_ERR;
    }

    result = BAERmfEditorDocument_GetFileVersion(srcPath, &fromVersion);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    if (outFromVersion)
    {
        *outFromVersion = fromVersion;
    }

    if (fromVersion == XFILERESOURCE_VERSION_ZMF)
    {
        return BAE_ALREADY_EXISTS;
    }

    document = BAERmfEditorDocument_LoadFromFile(srcPath);
    if (!document)
    {
        return BAE_BAD_FILE;
    }

    result = BAERmfEditorDocument_SaveAsRmf(document, dstPath);
    BAERmfEditorDocument_Delete(document);
    return result;
}

