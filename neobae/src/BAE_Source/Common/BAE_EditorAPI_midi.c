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
**  BAE_EditorAPI_midi.c
**
**  MIDI load/build and document track/note/tempo APIs, SaveAsMidi. Perf: neobae/docs/BAE_EditorAPI_PERF.md
*/
/*****************************************************************************/

#include "BAE_EditorAPI_Internal.h"


char *PV_DuplicateString(char const *source)
{
    char *copy;
    uint32_t length;

    if (!source)
    {
        return NULL;
    }
    length = (uint32_t)strlen(source);
    copy = (char *)XNewPtr((int32_t)(length + 1));
    if (copy)
    {
        XBlockMove(source, copy, (int32_t)(length + 1));
    }
    return copy;
}


void PV_FreeString(char **target)
{
    if (target && *target)
    {
        XDisposePtr(*target);
        *target = NULL;
    }
}


void PV_MarkDocumentDirty(BAERmfEditorDocument *document)
{
    if (document)
    {
        document->isPristine = FALSE;
        document->loopMarkersOnlyDirty = FALSE;
        PV_FreeDebugOriginalMidiData(document);
    }
}


BAEResult PV_SetDebugOriginalMidiData(BAERmfEditorDocument *document,
                                             unsigned char const *midiData,
                                             uint32_t midiDataSize)
{
    unsigned char *copy;

    if (!document)
    {
        return BAE_PARAM_ERR;
    }

    PV_FreeDebugOriginalMidiData(document);

    if (!midiData || midiDataSize == 0)
    {
        return BAE_NO_ERROR;
    }

    copy = (unsigned char *)XNewPtr((int32_t)midiDataSize);
    if (!copy)
    {
        return BAE_MEMORY_ERR;
    }
    XBlockMove(midiData, copy, (int32_t)midiDataSize);
    document->debugOriginalMidiData = copy;
    document->debugOriginalMidiDataSize = midiDataSize;
    return BAE_NO_ERROR;
}


void PV_FreeDebugOriginalMidiData(BAERmfEditorDocument *document)
{
    if (!document)
    {
        return;
    }
    if (document->debugOriginalMidiData)
    {
        XDisposePtr(document->debugOriginalMidiData);
        document->debugOriginalMidiData = NULL;
    }
    document->debugOriginalMidiDataSize = 0;
}


void PV_DebugFreeMidiStats(BAEDebugMidiStats *stats)
{
    if (!stats)
    {
        return;
    }
    if (stats->tracks)
    {
        XDisposePtr(stats->tracks);
        stats->tracks = NULL;
    }
    stats->trackCount = 0;
}


void PV_DebugHashByte(uint32_t *hash, unsigned char value)
{
    *hash ^= (uint32_t)value;
    *hash *= 16777619UL;
}


void PV_DebugHashU32(uint32_t *hash, uint32_t value)
{
    PV_DebugHashByte(hash, (unsigned char)(value & 0xFF));
    PV_DebugHashByte(hash, (unsigned char)((value >> 8) & 0xFF));
    PV_DebugHashByte(hash, (unsigned char)((value >> 16) & 0xFF));
    PV_DebugHashByte(hash, (unsigned char)((value >> 24) & 0xFF));
}


BAEResult PV_DebugCollectMidiStats(unsigned char const *data,
                                         uint32_t dataSize,
                                         BAEDebugMidiStats *outStats)
{
    uint32_t headerLength;
    uint16_t trackCount;
    uint32_t offset;
    uint16_t trackIndex;

    if (!data || !outStats || dataSize < 14)
    {
        return BAE_BAD_FILE;
    }
    XSetMemory(outStats, sizeof(*outStats), 0);
    if (memcmp(data, "MThd", 4) != 0)
    {
        return BAE_BAD_FILE;
    }
    headerLength = PV_ReadBE32(data + 4);
    if (headerLength < 6 || dataSize < 8 + headerLength)
    {
        return BAE_BAD_FILE;
    }
    trackCount = PV_ReadBE16(data + 10);
    if (trackCount == 0)
    {
        return BAE_BAD_FILE;
    }
    outStats->tracks = (BAEDebugMidiTrackStats *)XNewPtr((int32_t)(trackCount * sizeof(BAEDebugMidiTrackStats)));
    if (!outStats->tracks)
    {
        return BAE_MEMORY_ERR;
    }
    outStats->trackCount = trackCount;
    XSetMemory(outStats->tracks, (int32_t)(trackCount * sizeof(BAEDebugMidiTrackStats)), 0);
    for (trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        uint32_t cc;
        outStats->tracks[trackIndex].eventHash = 2166136261UL;
        for (cc = 0; cc < 128; ++cc)
        {
            outStats->tracks[trackIndex].firstCCTick[cc] = 0xFFFFFFFFUL;
        }
    }

    offset = 8 + headerLength;
    for (trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        uint32_t trackLength;
        uint32_t trackEnd;
        uint32_t currentTick;
        unsigned char runningStatus;
        BAEDebugMidiTrackStats *stats;

        if (offset + 8 > dataSize || memcmp(data + offset, "MTrk", 4) != 0)
        {
            PV_DebugFreeMidiStats(outStats);
            return BAE_BAD_FILE;
        }
        trackLength = PV_ReadBE32(data + offset + 4);
        offset += 8;
        if (offset + trackLength > dataSize)
        {
            PV_DebugFreeMidiStats(outStats);
            return BAE_BAD_FILE;
        }
        trackEnd = offset + trackLength;
        currentTick = 0;
        runningStatus = 0;
        stats = &outStats->tracks[trackIndex];

        while (offset < trackEnd)
        {
            uint32_t delta;
            unsigned char status;
            BAEResult result;

            result = PV_ReadVLQ(data, trackEnd, &offset, &delta);
            if (result != BAE_NO_ERROR)
            {
                PV_DebugFreeMidiStats(outStats);
                return result;
            }
            currentTick += delta;
            if (offset >= trackEnd)
            {
                break;
            }

            status = data[offset++];
            if (status < 0x80)
            {
                if (runningStatus == 0)
                {
                    PV_DebugFreeMidiStats(outStats);
                    return BAE_BAD_FILE;
                }
                offset--;
                status = runningStatus;
            }
            else if (status < 0xF0)
            {
                runningStatus = status;
            }
            else
            {
                runningStatus = 0;
            }

            if (status == 0xFF)
            {
                unsigned char metaType;
                uint32_t metaLength;

                if (offset >= trackEnd)
                {
                    PV_DebugFreeMidiStats(outStats);
                    return BAE_BAD_FILE;
                }
                metaType = data[offset++];
                result = PV_ReadVLQ(data, trackEnd, &offset, &metaLength);
                if (result != BAE_NO_ERROR || offset + metaLength > trackEnd)
                {
                    PV_DebugFreeMidiStats(outStats);
                    return BAE_BAD_FILE;
                }
                if (metaType == 0x51)
                {
                    stats->tempoMetaCount++;
                }
                else if (metaType != 0x2F)
                {
                    stats->otherMetaCount++;
                }
                stats->eventCount++;
                PV_DebugHashByte(&stats->eventHash, 0xFF);
                PV_DebugHashByte(&stats->eventHash, metaType);
                PV_DebugHashU32(&stats->eventHash, currentTick);
                PV_DebugHashU32(&stats->eventHash, metaLength);
                if (metaLength > 0)
                {
                    uint32_t k;
                    for (k = 0; k < metaLength; ++k)
                    {
                        PV_DebugHashByte(&stats->eventHash, data[offset + k]);
                    }
                }
                offset += metaLength;
                continue;
            }

            if (status == 0xF0 || status == 0xF7)
            {
                uint32_t sysexLength;

                result = PV_ReadVLQ(data, trackEnd, &offset, &sysexLength);
                if (result != BAE_NO_ERROR || offset + sysexLength > trackEnd)
                {
                    PV_DebugFreeMidiStats(outStats);
                    return BAE_BAD_FILE;
                }
                stats->sysexCount++;
                stats->eventCount++;
                PV_DebugHashByte(&stats->eventHash, status);
                PV_DebugHashU32(&stats->eventHash, currentTick);
                PV_DebugHashU32(&stats->eventHash, sysexLength);
                if (sysexLength > 0)
                {
                    uint32_t k;
                    for (k = 0; k < sysexLength; ++k)
                    {
                        PV_DebugHashByte(&stats->eventHash, data[offset + k]);
                    }
                }
                offset += sysexLength;
                continue;
            }

            switch (status & 0xF0)
            {
                case NOTE_OFF:
                    if (offset + 2 > trackEnd)
                    {
                        PV_DebugFreeMidiStats(outStats);
                        return BAE_BAD_FILE;
                    }
                    stats->noteOffCount++;
                    stats->eventCount++;
                    PV_DebugHashByte(&stats->eventHash, status);
                    PV_DebugHashU32(&stats->eventHash, currentTick);
                    PV_DebugHashByte(&stats->eventHash, data[offset]);
                    PV_DebugHashByte(&stats->eventHash, data[offset + 1]);
                    offset += 2;
                    break;
                case NOTE_ON:
                {
                    unsigned char vel;
                    if (offset + 2 > trackEnd)
                    {
                        PV_DebugFreeMidiStats(outStats);
                        return BAE_BAD_FILE;
                    }
                    vel = data[offset + 1];
                    if (vel == 0)
                    {
                        stats->noteOffCount++;
                    }
                    else
                    {
                        stats->noteOnCount++;
                    }
                    stats->eventCount++;
                    PV_DebugHashByte(&stats->eventHash, status);
                    PV_DebugHashU32(&stats->eventHash, currentTick);
                    PV_DebugHashByte(&stats->eventHash, data[offset]);
                    PV_DebugHashByte(&stats->eventHash, data[offset + 1]);
                    offset += 2;
                    break;
                }
                case POLY_AFTERTOUCH:
                    if (offset + 2 > trackEnd)
                    {
                        PV_DebugFreeMidiStats(outStats);
                        return BAE_BAD_FILE;
                    }
                    stats->polyAftertouchCount++;
                    stats->eventCount++;
                    PV_DebugHashByte(&stats->eventHash, status);
                    PV_DebugHashU32(&stats->eventHash, currentTick);
                    PV_DebugHashByte(&stats->eventHash, data[offset]);
                    PV_DebugHashByte(&stats->eventHash, data[offset + 1]);
                    offset += 2;
                    break;
                case CONTROL_CHANGE:
                {
                    unsigned char cc;
                    if (offset + 2 > trackEnd)
                    {
                        PV_DebugFreeMidiStats(outStats);
                        return BAE_BAD_FILE;
                    }
                    cc = data[offset];
                    if (cc < 128)
                    {
                        stats->ccCount[cc]++;
                        if (currentTick < stats->firstCCTick[cc])
                        {
                            stats->firstCCTick[cc] = currentTick;
                        }
                    }
                    stats->controlChangeCount++;
                    stats->eventCount++;
                    PV_DebugHashByte(&stats->eventHash, status);
                    PV_DebugHashU32(&stats->eventHash, currentTick);
                    PV_DebugHashByte(&stats->eventHash, data[offset]);
                    PV_DebugHashByte(&stats->eventHash, data[offset + 1]);
                    offset += 2;
                    break;
                }
                case PROGRAM_CHANGE:
                    if (offset + 1 > trackEnd)
                    {
                        PV_DebugFreeMidiStats(outStats);
                        return BAE_BAD_FILE;
                    }
                    stats->programChangeCount++;
                    stats->eventCount++;
                    PV_DebugHashByte(&stats->eventHash, status);
                    PV_DebugHashU32(&stats->eventHash, currentTick);
                    PV_DebugHashByte(&stats->eventHash, data[offset]);
                    offset += 1;
                    break;
                case CHANNEL_AFTERTOUCH:
                    if (offset + 1 > trackEnd)
                    {
                        PV_DebugFreeMidiStats(outStats);
                        return BAE_BAD_FILE;
                    }
                    stats->channelAftertouchCount++;
                    stats->eventCount++;
                    PV_DebugHashByte(&stats->eventHash, status);
                    PV_DebugHashU32(&stats->eventHash, currentTick);
                    PV_DebugHashByte(&stats->eventHash, data[offset]);
                    offset += 1;
                    break;
                case PITCH_BEND:
                    if (offset + 2 > trackEnd)
                    {
                        PV_DebugFreeMidiStats(outStats);
                        return BAE_BAD_FILE;
                    }
                    stats->pitchBendCount++;
                    stats->eventCount++;
                    PV_DebugHashByte(&stats->eventHash, status);
                    PV_DebugHashU32(&stats->eventHash, currentTick);
                    PV_DebugHashByte(&stats->eventHash, data[offset]);
                    PV_DebugHashByte(&stats->eventHash, data[offset + 1]);
                    offset += 2;
                    break;
                default:
                    PV_DebugFreeMidiStats(outStats);
                    return BAE_BAD_FILE;
            }
        }
    }

    return BAE_NO_ERROR;
}


void PV_DebugDumpMidiTrack(const char *label, unsigned char const *data, uint32_t dataSize, uint16_t trackIndex)
{
    uint32_t offset;
    uint32_t headerLength;
    uint16_t trackCount;
    uint32_t targetTrackOffset;
    uint32_t targetTrackLength;
    uint32_t currentTick;
    unsigned char runningStatus;
    uint32_t evtIndex;

    if (!data || !label || dataSize < 14) return;
    if (memcmp(data, "MThd", 4) != 0) return;
    headerLength = PV_ReadBE32(data + 4);
    trackCount = PV_ReadBE16(data + 10);
    if (trackIndex >= trackCount || dataSize < 8 + headerLength) return;

    offset = 8 + headerLength;
    {
        uint16_t t;
        for (t = 0; t < trackIndex; ++t)
        {
            uint32_t tlen;
            if (offset + 8 > dataSize || memcmp(data + offset, "MTrk", 4) != 0) return;
            tlen = PV_ReadBE32(data + offset + 4);
            offset += 8 + tlen;
        }
    }
    if (offset + 8 > dataSize || memcmp(data + offset, "MTrk", 4) != 0) return;
    targetTrackLength = PV_ReadBE32(data + offset + 4);
    targetTrackOffset = offset + 8;
    if (targetTrackOffset + targetTrackLength > dataSize) return;

    debug_message("[MIDI DUMP] %s Track %u:\n", label, (unsigned)trackIndex);
    currentTick = 0;
    runningStatus = 0;
    offset = targetTrackOffset;
    evtIndex = 0;
    while (offset < targetTrackOffset + targetTrackLength)
    {
        uint32_t delta;
        unsigned char status;

        if (PV_ReadVLQ(data, targetTrackOffset + targetTrackLength, &offset, &delta) != BAE_NO_ERROR) break;
        currentTick += delta;
        if (offset >= targetTrackOffset + targetTrackLength) break;
        status = data[offset++];
        if (status < 0x80)
        {
            if (runningStatus == 0) break;
            offset--;
            status = runningStatus;
        }
        else if (status < 0xF0)
        {
            runningStatus = status;
        }
        else
        {
            runningStatus = 0;
        }

        if (status == 0xFF)
        {
            unsigned char mt; uint32_t ml;
            if (offset >= targetTrackOffset + targetTrackLength) break;
            mt = data[offset++];
            if (PV_ReadVLQ(data, targetTrackOffset + targetTrackLength, &offset, &ml) != BAE_NO_ERROR) break;
            if (ml == 3 && mt == 0x51)
            {
                uint32_t us = ((uint32_t)data[offset] << 16) | ((uint32_t)data[offset+1] << 8) | data[offset+2];
                debug_message("[MIDI DUMP]   [%u] tick=%-6u META%02X bpm=%u\n", evtIndex, currentTick, (unsigned)mt, (unsigned)(60000000UL / us));
            }
            else
            {
                debug_message("[MIDI DUMP]   [%u] tick=%-6u META%02X len=%u\n", evtIndex, currentTick, (unsigned)mt, (unsigned)ml);
            }
            offset += ml;
        }
        else if (status == 0xF0 || status == 0xF7)
        {
            uint32_t sl;
            if (PV_ReadVLQ(data, targetTrackOffset + targetTrackLength, &offset, &sl) != BAE_NO_ERROR) break;
            debug_message("[MIDI DUMP]   [%u] tick=%-6u SYSEX len=%u\n", evtIndex, currentTick, (unsigned)sl);
            offset += sl;
        }
        else
        {
            unsigned char evtype = (unsigned char)(status & 0xF0);
            unsigned char ch = (unsigned char)(status & 0x0F);
            if (evtype == NOTE_ON || evtype == NOTE_OFF || evtype == CONTROL_CHANGE || evtype == PITCH_BEND || evtype == POLY_AFTERTOUCH)
            {
                unsigned char d1, d2;
                if (offset + 2 > targetTrackOffset + targetTrackLength) break;
                d1 = data[offset++]; d2 = data[offset++];
                if (evtype == NOTE_ON)
                    debug_message("[MIDI DUMP]   [%u] tick=%-6u NON  ch=%u note=%u vel=%u\n", evtIndex, currentTick, (unsigned)ch, (unsigned)d1, (unsigned)d2);
                else if (evtype == NOTE_OFF)
                    debug_message("[MIDI DUMP]   [%u] tick=%-6u NOFF ch=%u note=%u vel=%u\n", evtIndex, currentTick, (unsigned)ch, (unsigned)d1, (unsigned)d2);
                else if (evtype == CONTROL_CHANGE)
                    debug_message("[MIDI DUMP]   [%u] tick=%-6u CC   ch=%u cc=%u val=%u\n", evtIndex, currentTick, (unsigned)ch, (unsigned)d1, (unsigned)d2);
                else if (evtype == PITCH_BEND)
                    debug_message("[MIDI DUMP]   [%u] tick=%-6u PB   ch=%u lsb=%u msb=%u\n", evtIndex, currentTick, (unsigned)ch, (unsigned)d1, (unsigned)d2);
                else
                    debug_message("[MIDI DUMP]   [%u] tick=%-6u PA   ch=%u note=%u val=%u\n", evtIndex, currentTick, (unsigned)ch, (unsigned)d1, (unsigned)d2);
            }
            else if (evtype == PROGRAM_CHANGE || evtype == CHANNEL_AFTERTOUCH)
            {
                unsigned char d1;
                if (offset + 1 > targetTrackOffset + targetTrackLength) break;
                d1 = data[offset++];
                if (evtype == PROGRAM_CHANGE)
                    debug_message("[MIDI DUMP]   [%u] tick=%-6u PC   ch=%u prog=%u\n", evtIndex, currentTick, (unsigned)ch, (unsigned)d1);
                else
                    debug_message("[MIDI DUMP]   [%u] tick=%-6u CA   ch=%u val=%u\n", evtIndex, currentTick, (unsigned)ch, (unsigned)d1);
            }
            else
            {
                debug_message("[MIDI DUMP]   [%u] tick=%-6u UNK  status=%02X\n", evtIndex, currentTick, (unsigned)status);
                break;
            }
        }
        evtIndex++;
    }
}


void PV_DebugReportMidiRoundTripDiff(BAERmfEditorDocument const *document,
                                            ByteBuffer const *generatedMidi)
{
    BAEDebugMidiStats originalStats;
    BAEDebugMidiStats generatedStats;
    BAEResult resultOriginal;
    BAEResult resultGenerated;
    uint16_t i;
    uint16_t compareCount;
    uint16_t genOffset;
    uint16_t maxTracks;

    if (!document || !generatedMidi || !document->debugOriginalMidiData || document->debugOriginalMidiDataSize == 0)
    {
        return;
    }

    XSetMemory(&originalStats, sizeof(originalStats), 0);
    XSetMemory(&generatedStats, sizeof(generatedStats), 0);
    resultOriginal = PV_DebugCollectMidiStats(document->debugOriginalMidiData,
                                              document->debugOriginalMidiDataSize,
                                              &originalStats);
    resultGenerated = PV_DebugCollectMidiStats(generatedMidi->data,
                                               generatedMidi->size,
                                               &generatedStats);

    if (resultOriginal != BAE_NO_ERROR || resultGenerated != BAE_NO_ERROR)
    {
        debug_message("[MIDI DIFF] Unable to parse original/generated MIDI for diff (orig=%d gen=%d)\n",
                   (int)resultOriginal,
                   (int)resultGenerated);
        PV_DebugFreeMidiStats(&originalStats);
        PV_DebugFreeMidiStats(&generatedStats);
        return;
    }

    debug_message("[MIDI DIFF] Original tracks=%u Generated tracks=%u\n",
               (unsigned)originalStats.trackCount,
               (unsigned)generatedStats.trackCount);

    genOffset = 0;
    if (generatedStats.trackCount == (uint16_t)(originalStats.trackCount + 1))
    {
        BAEDebugMidiTrackStats const *t0 = &generatedStats.tracks[0];
        if (t0->noteOnCount == 0 && t0->noteOffCount == 0 && t0->controlChangeCount == 0 &&
            t0->programChangeCount == 0 && t0->pitchBendCount == 0 && t0->channelAftertouchCount == 0 &&
            t0->polyAftertouchCount == 0 && t0->sysexCount == 0)
        {
            genOffset = 1;
        }
    }

    compareCount = originalStats.trackCount;
    if ((uint16_t)(generatedStats.trackCount - genOffset) < compareCount)
    {
        compareCount = (uint16_t)(generatedStats.trackCount - genOffset);
    }

    for (i = 0; i < compareCount; ++i)
    {
        BAEDebugMidiTrackStats const *orig = &originalStats.tracks[i];
        BAEDebugMidiTrackStats const *gen = &generatedStats.tracks[(uint16_t)(i + genOffset)];
        uint32_t cc;
        bool printedTrackHeader;

        printedTrackHeader = FALSE;
        if (orig->noteOnCount != gen->noteOnCount ||
            orig->noteOffCount != gen->noteOffCount ||
            orig->controlChangeCount != gen->controlChangeCount ||
            orig->programChangeCount != gen->programChangeCount ||
            orig->pitchBendCount != gen->pitchBendCount ||
            orig->channelAftertouchCount != gen->channelAftertouchCount ||
            orig->polyAftertouchCount != gen->polyAftertouchCount ||
            orig->sysexCount != gen->sysexCount ||
            orig->tempoMetaCount != gen->tempoMetaCount ||
            orig->otherMetaCount != gen->otherMetaCount ||
            orig->eventCount != gen->eventCount ||
            orig->eventHash != gen->eventHash)
        {
            printedTrackHeader = TRUE;
            debug_message("[MIDI DIFF] Track %u: ON %u->%u OFF %u->%u CC %u->%u PC %u->%u PB %u->%u CA %u->%u PA %u->%u SX %u->%u TMP %u->%u META %u->%u EVT %u->%u HASH %08lx->%08lx\n",
                       (unsigned)i,
                       (unsigned)orig->noteOnCount,
                       (unsigned)gen->noteOnCount,
                       (unsigned)orig->noteOffCount,
                       (unsigned)gen->noteOffCount,
                       (unsigned)orig->controlChangeCount,
                       (unsigned)gen->controlChangeCount,
                       (unsigned)orig->programChangeCount,
                       (unsigned)gen->programChangeCount,
                       (unsigned)orig->pitchBendCount,
                       (unsigned)gen->pitchBendCount,
                       (unsigned)orig->channelAftertouchCount,
                       (unsigned)gen->channelAftertouchCount,
                       (unsigned)orig->polyAftertouchCount,
                       (unsigned)gen->polyAftertouchCount,
                       (unsigned)orig->sysexCount,
                       (unsigned)gen->sysexCount,
                       (unsigned)orig->tempoMetaCount,
                       (unsigned)gen->tempoMetaCount,
                       (unsigned)orig->otherMetaCount,
                       (unsigned)gen->otherMetaCount,
                       (unsigned)orig->eventCount,
                       (unsigned)gen->eventCount,
                       (unsigned long)orig->eventHash,
                       (unsigned long)gen->eventHash);
            /* Dump full event details for both when only the hash differs (same counts). */
            if (orig->eventCount == gen->eventCount &&
                orig->noteOnCount == gen->noteOnCount &&
                orig->noteOffCount == gen->noteOffCount &&
                orig->controlChangeCount == gen->controlChangeCount &&
                orig->programChangeCount == gen->programChangeCount &&
                orig->tempoMetaCount == gen->tempoMetaCount &&
                orig->otherMetaCount == gen->otherMetaCount)
            {
                PV_DebugDumpMidiTrack("ORIG", document->debugOriginalMidiData, document->debugOriginalMidiDataSize, i);
                PV_DebugDumpMidiTrack("GEN", generatedMidi->data, generatedMidi->size, (uint16_t)(i + genOffset));
            }
        }

        for (cc = 0; cc < 128; ++cc)
        {
            if (orig->ccCount[cc] != gen->ccCount[cc] || orig->firstCCTick[cc] != gen->firstCCTick[cc])
            {
                if (!printedTrackHeader)
                {
                    printedTrackHeader = TRUE;
                    debug_message("[MIDI DIFF] Track %u controller deltas:\n", (unsigned)i);
                }
                debug_message("[MIDI DIFF]   CC%u count %u->%u firstTick %ld->%ld\n",
                           (unsigned)cc,
                           (unsigned)orig->ccCount[cc],
                           (unsigned)gen->ccCount[cc],
                           (long)((orig->firstCCTick[cc] == 0xFFFFFFFFUL) ? -1L : (long)orig->firstCCTick[cc]),
                           (long)((gen->firstCCTick[cc] == 0xFFFFFFFFUL) ? -1L : (long)gen->firstCCTick[cc]));
            }
        }
    }

    maxTracks = originalStats.trackCount > generatedStats.trackCount ? originalStats.trackCount : generatedStats.trackCount;
    if (compareCount < maxTracks)
    {
        debug_message("[MIDI DIFF] Warning: unmatched track tail (compare=%u max=%u offset=%u)\n",
                   (unsigned)compareCount,
                   (unsigned)maxTracks,
                   (unsigned)genOffset);
    }

    PV_DebugFreeMidiStats(&originalStats);
    PV_DebugFreeMidiStats(&generatedStats);
}


void PV_FreeOriginalResources(BAERmfEditorDocument *document)
{
    uint32_t index;

    if (!document)
    {
        return;
    }
    for (index = 0; index < document->originalResourceCount; ++index)
    {
        if (document->originalResources[index].data)
        {
            XDisposePtr(document->originalResources[index].data);
            document->originalResources[index].data = NULL;
        }
    }
    if (document->originalResources)
    {
        XDisposePtr(document->originalResources);
        document->originalResources = NULL;
    }
    document->originalResourceCount = 0;
    document->originalResourceCapacity = 0;
}


void PV_ClearTempoEvents(BAERmfEditorDocument *document)
{
    if (!document)
    {
        return;
    }
    if (document->tempoEvents)
    {
        XDisposePtr(document->tempoEvents);
        document->tempoEvents = NULL;
    }
    document->tempoEventCount = 0;
    document->tempoEventCapacity = 0;
}


BAEResult PV_AddTempoEvent(BAERmfEditorDocument *document, uint32_t tick, uint32_t microsecondsPerQuarter)
{
    uint32_t insertIndex;
    BAEResult result;

    if (!document || microsecondsPerQuarter == 0)
    {
        return BAE_PARAM_ERR;
    }

    insertIndex = 0;
    while (insertIndex < document->tempoEventCount && document->tempoEvents[insertIndex].tick < tick)
    {
        insertIndex++;
    }

    if (insertIndex < document->tempoEventCount && document->tempoEvents[insertIndex].tick == tick)
    {
        document->tempoEvents[insertIndex].microsecondsPerQuarter = microsecondsPerQuarter;
        return BAE_NO_ERROR;
    }

    result = PV_GrowBuffer((void **)&document->tempoEvents,
                           &document->tempoEventCapacity,
                           sizeof(BAERmfEditorTempoEvent),
                           document->tempoEventCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    if (insertIndex < document->tempoEventCount)
    {
        XBlockMove(&document->tempoEvents[insertIndex],
                   &document->tempoEvents[insertIndex + 1],
                   (int32_t)((document->tempoEventCount - insertIndex) * sizeof(BAERmfEditorTempoEvent)));
    }

    document->tempoEvents[insertIndex].tick = tick;
    document->tempoEvents[insertIndex].microsecondsPerQuarter = microsecondsPerQuarter;
    document->tempoEventCount++;
    return BAE_NO_ERROR;
}


BAEResult PV_AddCCEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char cc, unsigned char value, unsigned char data2)
{
    BAEResult result;
    BAERmfEditorCCEvent *event;

    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_GrowBuffer((void **)&track->ccEvents,
                           &track->ccEventCapacity,
                           sizeof(BAERmfEditorCCEvent),
                           track->ccEventCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    event = &track->ccEvents[track->ccEventCount];
    event->tick = tick;
    event->eventOrder = track->nextEventOrder++;
    event->cc = cc;
    event->value = value;
    event->data2 = data2;
    track->ccEventCount++;
    return BAE_NO_ERROR;
}


BAEResult PV_AddSysExEventToTrack(BAERmfEditorTrack *track,
                                         uint32_t tick,
                                         unsigned char status,
                                         unsigned char const *data,
                                         uint32_t size)
{
    BAEResult result;
    BAERmfEditorSysExEvent *event;

    if (!track || (status != 0xF0 && status != 0xF7))
    {
        return BAE_PARAM_ERR;
    }

    result = PV_GrowBuffer((void **)&track->sysexEvents,
                           &track->sysexEventCapacity,
                           sizeof(BAERmfEditorSysExEvent),
                           track->sysexEventCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    event = &track->sysexEvents[track->sysexEventCount];
    XSetMemory(event, sizeof(*event), 0);
    event->tick = tick;
    event->eventOrder = track->nextEventOrder++;
    event->status = status;
    if (size > 0)
    {
        event->data = (unsigned char *)XNewPtr((int32_t)size);
        if (!event->data)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(data, event->data, (int32_t)size);
        event->size = size;
    }
    track->sysexEventCount++;
    return BAE_NO_ERROR;
}


void PV_FreeTrackSysExEvents(BAERmfEditorTrack *track)
{
    uint32_t index;

    if (!track)
    {
        return;
    }
    for (index = 0; index < track->sysexEventCount; ++index)
    {
        if (track->sysexEvents[index].data)
        {
            XDisposePtr(track->sysexEvents[index].data);
            track->sysexEvents[index].data = NULL;
        }
    }
    if (track->sysexEvents)
    {
        XDisposePtr(track->sysexEvents);
        track->sysexEvents = NULL;
    }
    track->sysexEventCount = 0;
    track->sysexEventCapacity = 0;
}


BAEResult PV_AddAuxEventToTrack(BAERmfEditorTrack *track,
                                       uint32_t tick,
                                       unsigned char status,
                                       unsigned char data1,
                                       unsigned char data2,
                                       unsigned char dataBytes)
{
    BAEResult result;
    BAERmfEditorAuxEvent *event;

    if (!track || dataBytes == 0 || dataBytes > 2)
    {
        return BAE_PARAM_ERR;
    }

    result = PV_GrowBuffer((void **)&track->auxEvents,
                           &track->auxEventCapacity,
                           sizeof(BAERmfEditorAuxEvent),
                           track->auxEventCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    event = &track->auxEvents[track->auxEventCount];
    event->tick = tick;
    event->eventOrder = track->nextEventOrder++;
    event->status = status;
    event->data1 = data1;
    event->data2 = data2;
    event->dataBytes = dataBytes;
    track->auxEventCount++;
    return BAE_NO_ERROR;
}


void PV_FreeTrackAuxEvents(BAERmfEditorTrack *track)
{
    if (!track)
    {
        return;
    }
    if (track->auxEvents)
    {
        XDisposePtr(track->auxEvents);
        track->auxEvents = NULL;
    }
    track->auxEventCount = 0;
    track->auxEventCapacity = 0;
}


BAEResult PV_AddMetaEventToTrack(BAERmfEditorTrack *track,
                                        uint32_t tick,
                                        unsigned char type,
                                        unsigned char const *data,
                                        uint32_t size)
{
    BAEResult result;
    BAERmfEditorMetaEvent *event;

    if (!track)
    {
        return BAE_PARAM_ERR;
    }

    result = PV_GrowBuffer((void **)&track->metaEvents,
                           &track->metaEventCapacity,
                           sizeof(BAERmfEditorMetaEvent),
                           track->metaEventCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    event = &track->metaEvents[track->metaEventCount];
    XSetMemory(event, sizeof(*event), 0);
    event->tick = tick;
    event->eventOrder = track->nextEventOrder++;
    event->type = type;
    if (size > 0)
    {
        event->data = (unsigned char *)XNewPtr((int32_t)size);
        if (!event->data)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(data, event->data, (int32_t)size);
        event->size = size;
    }
    track->metaEventCount++;
    return BAE_NO_ERROR;
}


void PV_FreeTrackMetaEvents(BAERmfEditorTrack *track)
{
    uint32_t index;

    if (!track)
    {
        return;
    }

    for (index = 0; index < track->metaEventCount; ++index)
    {
        if (track->metaEvents[index].data)
        {
            XDisposePtr(track->metaEvents[index].data);
            track->metaEvents[index].data = NULL;
        }
    }
    if (track->metaEvents)
    {
        XDisposePtr(track->metaEvents);
        track->metaEvents = NULL;
    }
    track->metaEventCount = 0;
    track->metaEventCapacity = 0;
}


unsigned char PV_ToLowerAscii(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}


bool PV_MarkerStartsWith(unsigned char const *data, uint32_t size, char const *text)
{
    uint32_t i;

    if (!data || !text)
    {
        return FALSE;
    }
    for (i = 0; text[i] != 0; ++i)
    {
        if (i >= size)
        {
            return FALSE;
        }
        if (PV_ToLowerAscii(data[i]) != (unsigned char)text[i])
        {
            return FALSE;
        }
    }
    return TRUE;
}


bool PV_IsLoopStartMarkerText(unsigned char const *data, uint32_t size, int32_t *outLoopCount)
{
    if (outLoopCount)
    {
        *outLoopCount = -1;
    }
    if (!data || size == 0)
    {
        return FALSE;
    }

    if (PV_MarkerStartsWith(data, size, "[") || PV_MarkerStartsWith(data, size, "start"))
    {
        return TRUE;
    }
    if (PV_MarkerStartsWith(data, size, "loopstart"))
    {
        if (size > 9 && data[9] == '=')
        {
            int32_t count;
            uint32_t i;

            count = 0;
            for (i = 10; i < size; ++i)
            {
                unsigned char ch;

                ch = data[i];
                if (ch < '0' || ch > '9')
                {
                    break;
                }
                count = (count * 10) + (int32_t)(ch - '0');
            }
            if (outLoopCount && count > 0)
            {
                *outLoopCount = count;
            }
        }
        return TRUE;
    }
    return FALSE;
}


bool PV_IsLoopEndMarkerText(unsigned char const *data, uint32_t size)
{
    if (!data || size == 0)
    {
        return FALSE;
    }
    if (PV_MarkerStartsWith(data, size, "]") ||
        PV_MarkerStartsWith(data, size, "end") ||
        PV_MarkerStartsWith(data, size, "loopend"))
    {
        return TRUE;
    }
    return FALSE;
}


void PV_RemoveLoopMarkersFromTrack(BAERmfEditorTrack *track)
{
    uint32_t readIndex;
    uint32_t writeIndex;

    if (!track || track->metaEventCount == 0)
    {
        return;
    }
    writeIndex = 0;
    for (readIndex = 0; readIndex < track->metaEventCount; ++readIndex)
    {
        BAERmfEditorMetaEvent *event;

        event = &track->metaEvents[readIndex];
        if (event->type == 0x06 &&
            (PV_IsLoopStartMarkerText(event->data, event->size, NULL) ||
             PV_IsLoopEndMarkerText(event->data, event->size)))
        {
            if (event->data)
            {
                XDisposePtr(event->data);
                event->data = NULL;
            }
            continue;
        }
        if (writeIndex != readIndex)
        {
            track->metaEvents[writeIndex] = *event;
        }
        writeIndex++;
    }
    track->metaEventCount = writeIndex;
}


int PV_CompareCCEvents(void const *left, void const *right)
{
    BAERmfEditorCCEvent const *a;
    BAERmfEditorCCEvent const *b;

    a = (BAERmfEditorCCEvent const *)left;
    b = (BAERmfEditorCCEvent const *)right;
    if (a->tick < b->tick)
    {
        return -1;
    }
    if (a->tick > b->tick)
    {
        return 1;
    }
    if (a->cc < b->cc)
    {
        return -1;
    }
    if (a->cc > b->cc)
    {
        return 1;
    }
    return 0;
}


BAERmfEditorCCEvent *PV_FindTrackCCEventAtTick(BAERmfEditorTrack *track, unsigned char cc, uint32_t atTick)
{
    uint32_t index;

    if (!track)
    {
        return NULL;
    }
    for (index = 0; index < track->ccEventCount; ++index)
    {
        if (track->ccEvents[index].cc == cc &&
            track->ccEvents[index].tick == atTick)
        {
            return &track->ccEvents[index];
        }
    }
    return NULL;
}


BAERmfEditorCCEvent *PV_FindTrackCCEvent(BAERmfEditorTrack *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex)
{
    uint32_t index;
    uint32_t count;

    if (!track)
    {
        return NULL;
    }
    count = 0;
    for (index = 0; index < track->ccEventCount; ++index)
    {
        if (track->ccEvents[index].cc == cc)
        {
            if (count == eventIndex)
            {
                if (outActualIndex)
                {
                    *outActualIndex = index;
                }
                return &track->ccEvents[index];
            }
            count++;
        }
    }
    return NULL;
}


BAERmfEditorCCEvent const *PV_FindTrackCCEventConst(BAERmfEditorTrack const *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex)
{
    uint32_t index;
    uint32_t count;

    if (!track)
    {
        return NULL;
    }
    count = 0;
    for (index = 0; index < track->ccEventCount; ++index)
    {
        if (track->ccEvents[index].cc == cc)
        {
            if (count == eventIndex)
            {
                if (outActualIndex)
                {
                    *outActualIndex = index;
                }
                return &track->ccEvents[index];
            }
            count++;
        }
    }
    return NULL;
}


BAEResult PV_ByteBufferReserve(ByteBuffer *buffer, uint32_t extraBytes)
{
    uint32_t required;

    required = buffer->size + extraBytes;
    return PV_GrowBuffer((void **)&buffer->data, &buffer->capacity, sizeof(unsigned char), required);
}


BAEResult PV_ByteBufferAppend(ByteBuffer *buffer, void const *data, uint32_t length)
{
    BAEResult result;

    result = PV_ByteBufferReserve(buffer, length);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    if (length)
    {
        XBlockMove(data, buffer->data + buffer->size, (int32_t)length);
        buffer->size += length;
    }
    return BAE_NO_ERROR;
}


BAEResult PV_ByteBufferAppendByte(ByteBuffer *buffer, unsigned char value)
{
    return PV_ByteBufferAppend(buffer, &value, 1);
}


BAEResult PV_ByteBufferAppendBE16(ByteBuffer *buffer, uint16_t value)
{
    unsigned char bytes[2];

    bytes[0] = (unsigned char)((value >> 8) & 0xFF);
    bytes[1] = (unsigned char)(value & 0xFF);
    return PV_ByteBufferAppend(buffer, bytes, 2);
}


BAEResult PV_ByteBufferAppendBE32(ByteBuffer *buffer, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)((value >> 24) & 0xFF);
    bytes[1] = (unsigned char)((value >> 16) & 0xFF);
    bytes[2] = (unsigned char)((value >> 8) & 0xFF);
    bytes[3] = (unsigned char)(value & 0xFF);
    return PV_ByteBufferAppend(buffer, bytes, 4);
}


BAEResult PV_ByteBufferAppendVLQ(ByteBuffer *buffer, uint32_t value)
{
    unsigned char encoded[5];
    int index;
    int outIndex;

    encoded[0] = (unsigned char)(value & 0x7F);
    index = 1;
    value >>= 7;
    while (value)
    {
        encoded[index++] = (unsigned char)((value & 0x7F) | 0x80);
        value >>= 7;
    }
    for (outIndex = index - 1; outIndex >= 0; --outIndex)
    {
        BAEResult result;

        result = PV_ByteBufferAppendByte(buffer, encoded[outIndex]);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    return BAE_NO_ERROR;
}


void PV_ByteBufferDispose(ByteBuffer *buffer)
{
    debug_message("[RMF Save] PV_ByteBufferDispose: data=%p size=%u capacity=%u\n", 
                buffer->data, (unsigned)buffer->size, (unsigned)buffer->capacity);
    if (buffer->data)
    {
        XDisposePtr(buffer->data);
        buffer->data = NULL;
    }
    buffer->size = 0;
    buffer->capacity = 0;
}


BAEResult PV_SetDocumentString(char **target, char const *value)
{
    char *copy;

    copy = NULL;
    if (value && value[0])
    {
        copy = PV_DuplicateString(value);
        if (!copy)
        {
            return BAE_MEMORY_ERR;
        }
    }
    PV_FreeString(target);
    *target = copy;
    return BAE_NO_ERROR;
}


bool PV_IsEditorCompressedImportType(BAEFileType fileType)
{
    switch (fileType)
    {
#if USE_MPEG_DECODER == TRUE || USE_MPEG_ENCODER == TRUE
        case BAE_MPEG_TYPE:
            return TRUE;
#endif
#if USE_FLAC_DECODER == TRUE || USE_FLAC_ENCODER == TRUE
        case BAE_FLAC_TYPE:
            return TRUE;
#endif
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
        case BAE_VORBIS_TYPE:
            return TRUE;
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        case BAE_OPUS_TYPE:
            return TRUE;
#endif
#if USE_QOA_SUPPORT == TRUE
        case BAE_QOA_TYPE:
            return TRUE;
#endif
        default:
            return FALSE;
    }
}


BAEResult PV_ReadFileIntoMemory(XFILENAME const *fileName,
                                       XPTR *outData,
                                       int32_t *outSize)
{
    XFILE file;
    int32_t fileSize;
    XPTR fileData;

    if (!fileName || !outData || !outSize)
    {
        return BAE_PARAM_ERR;
    }

    *outData = NULL;
    *outSize = 0;

    file = XFileOpenForRead((XFILENAME *)fileName);
    if (!file)
    {
        return BAE_FILE_NOT_FOUND;
    }

    fileSize = XFileGetLength(file);
    if (fileSize <= 0)
    {
        XFileClose(file);
        return BAE_BAD_FILE;
    }

    fileData = XNewPtr(fileSize);
    if (!fileData)
    {
        XFileClose(file);
        return BAE_MEMORY_ERR;
    }

    if (XFileSetPosition(file, 0) != 0 || XFileRead(file, fileData, fileSize) != 0)
    {
        XFileClose(file);
        XDisposePtr(fileData);
        return BAE_BAD_FILE;
    }

    XFileClose(file);
    *outData = fileData;
    *outSize = fileSize;
    return BAE_NO_ERROR;
}


BAEResult PV_AssignSongInfoString(SongResource_Info *songInfo, BAEInfoType infoType, char const *value)
{
    char *copy;

    copy = NULL;
    if (value && value[0])
    {
        copy = PV_DuplicateString(value);
        if (!copy)
        {
            return BAE_MEMORY_ERR;
        }
    }
    switch (infoType)
    {
        case TITLE_INFO:
            songInfo->title = copy;
            return BAE_NO_ERROR;
        case PERFORMED_BY_INFO:
            songInfo->performed = copy;
            return BAE_NO_ERROR;
        case COMPOSER_INFO:
            songInfo->composer = copy;
            return BAE_NO_ERROR;
        case COPYRIGHT_INFO:
            songInfo->copyright = copy;
            return BAE_NO_ERROR;
        case PUBLISHER_CONTACT_INFO:
            songInfo->publisher_contact_info = copy;
            return BAE_NO_ERROR;
        case USE_OF_LICENSE_INFO:
            songInfo->use_license = copy;
            return BAE_NO_ERROR;
        case LICENSED_TO_URL_INFO:
            songInfo->licensed_to_URL = copy;
            return BAE_NO_ERROR;
        case LICENSE_TERM_INFO:
            songInfo->license_term = copy;
            return BAE_NO_ERROR;
        case EXPIRATION_DATE_INFO:
            songInfo->expire_date = copy;
            return BAE_NO_ERROR;
        case COMPOSER_NOTES_INFO:
            songInfo->compser_notes = copy;
            return BAE_NO_ERROR;
        case INDEX_NUMBER_INFO:
            songInfo->index_number = copy;
            return BAE_NO_ERROR;
        case GENRE_INFO:
            songInfo->genre = copy;
            return BAE_NO_ERROR;
        case SUB_GENRE_INFO:
            songInfo->sub_genre = copy;
            return BAE_NO_ERROR;
        case TEMPO_DESCRIPTION_INFO:
            songInfo->tempo_description = copy;
            return BAE_NO_ERROR;
        case ORIGINAL_SOURCE_INFO:
            songInfo->original_source = copy;
            return BAE_NO_ERROR;
        default:
            if (copy)
            {
                XDisposePtr(copy);
            }
            return BAE_PARAM_ERR;
    }
}


BAEResult PV_ReadVLQ(unsigned char const *data, uint32_t dataSize, uint32_t *ioOffset, uint32_t *outValue)
{
    uint32_t offset;
    uint32_t value;
    int count;

    if (!data || !ioOffset || !outValue)
    {
        return BAE_PARAM_ERR;
    }
    offset = *ioOffset;
    value = 0;
    for (count = 0; count < 4; ++count)
    {
        unsigned char byteValue;

        if (offset >= dataSize)
        {
            return BAE_BAD_FILE;
        }
        byteValue = data[offset++];
        value = (value << 7) | (uint32_t)(byteValue & 0x7F);
        if ((byteValue & 0x80) == 0)
        {
            *ioOffset = offset;
            *outValue = value;
            return BAE_NO_ERROR;
        }
    }
    return BAE_BAD_FILE;
}


unsigned char PV_ClampMidi7Bit(int32_t value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 127)
    {
        return 127;
    }
    return (unsigned char)value;
}


BAERmfEditorTrack *PV_GetTrack(BAERmfEditorDocument *document, uint16_t trackIndex)
{
    if (!document || trackIndex >= document->trackCount)
    {
        return NULL;
    }
    return &document->tracks[trackIndex];
}


BAERmfEditorTrack const *PV_GetTrackConst(BAERmfEditorDocument const *document, uint16_t trackIndex)
{
    if (!document || trackIndex >= document->trackCount)
    {
        return NULL;
    }
    return &document->tracks[trackIndex];
}


BAEResult PV_AddNoteToTrack(BAERmfEditorTrack *track,
                                   uint32_t startTick,
                                   uint32_t durationTicks,
                                   unsigned char note,
                                   unsigned char velocity,
                                   unsigned char channel,
                                   uint16_t bank,
                                   unsigned char program,
                                   unsigned char noteOffStatus,
                                   unsigned char noteOffVelocity,
                                   uint32_t noteOnOrder,
                                   uint32_t noteOffOrder)
{
    BAEResult result;

    if (!track || durationTicks == 0)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_GrowBuffer((void **)&track->notes,
                           &track->noteCapacity,
                           sizeof(BAERmfEditorNote),
                           track->noteCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    track->notes[track->noteCount].startTick = startTick;
    track->notes[track->noteCount].durationTicks = durationTicks;
    track->notes[track->noteCount].note = note;
    track->notes[track->noteCount].velocity = velocity ? velocity : 96;
    track->notes[track->noteCount].channel = channel;
    track->notes[track->noteCount].bank = bank;
    track->notes[track->noteCount].program = program;
    track->notes[track->noteCount].noteOffStatus = noteOffStatus;
    track->notes[track->noteCount].noteOffVelocity = noteOffVelocity;
    track->notes[track->noteCount].noteOnOrder = noteOnOrder;
    track->notes[track->noteCount].noteOffOrder = noteOffOrder;
    track->noteCount++;
    return BAE_NO_ERROR;
}


BAEResult PV_SetTrackName(BAERmfEditorTrack *track, char const *name)
{
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    return PV_SetDocumentString(&track->name, name);
}


BAEResult PV_ReadWholeFile(BAEPathName filePath, unsigned char **outData, uint32_t *outSize)
{
    XFILENAME fileName;
    XFILE fileRef;
    unsigned char *data;
    int32_t length;

    if (!filePath || !outData || !outSize)
    {
        return BAE_PARAM_ERR;
    }
    *outData = NULL;
    *outSize = 0;
    XConvertPathToXFILENAME(filePath, &fileName);
    fileRef = XFileOpenForRead(&fileName);
    if (!fileRef)
    {
        return BAE_FILE_IO_ERROR;
    }
    length = XFileGetLength(fileRef);
    if (length <= 0)
    {
        XFileClose(fileRef);
        return BAE_BAD_FILE;
    }
    data = (unsigned char *)XNewPtr(length);
    if (!data)
    {
        XFileClose(fileRef);
        return BAE_MEMORY_ERR;
    }
    if (XFileSetPosition(fileRef, 0L) != 0 || XFileRead(fileRef, data, length) != 0)
    {
        XDisposePtr(data);
        XFileClose(fileRef);
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(fileRef);
    *outData = data;
    *outSize = (uint32_t)length;
    return BAE_NO_ERROR;
}

void PV_PeekSameTickBankProgram(unsigned char const *trackData,
                                       uint32_t trackSize,
                                       uint32_t peekOffset,
                                       unsigned char channel,
                                       unsigned char peekRunningStatus,
                                       uint16_t *bank,
                                       unsigned char *program)
{
    while (peekOffset < trackSize)
    {
        uint32_t delta;
        unsigned char s;
        unsigned char evtype;
        unsigned char evch;
        int twoBytes;
        int oneByte;

        /* Read VLQ delta; stop if this is a future tick */
        delta = 0;
        while (peekOffset < trackSize)
        {
            unsigned char vb = trackData[peekOffset++];
            delta = (delta << 7) | (vb & 0x7F);
            if (!(vb & 0x80))
            {
                break;
            }
        }
        if (delta != 0)
        {
            break;
        }
        if (peekOffset >= trackSize)
        {
            break;
        }

        s = trackData[peekOffset];
        if (s == 0xFF || s == 0xF0 || s == 0xF7)
        {
            break; /* meta/sysex: stop lookahead */
        }
        if (s >= 0x80)
        {
            peekRunningStatus = s;
            peekOffset++;
        }
        else
        {
            if (!peekRunningStatus)
            {
                break;
            }
        }

        evtype = (unsigned char)(peekRunningStatus & 0xF0);
        evch   = (unsigned char)(peekRunningStatus & 0x0F);
        twoBytes = (evtype == NOTE_OFF || evtype == NOTE_ON  ||
                    evtype == 0xA0    || evtype == CONTROL_CHANGE ||
                    evtype == PITCH_BEND);
        oneByte  = (evtype == PROGRAM_CHANGE || evtype == CHANNEL_AFTERTOUCH);

        if (peekOffset + (uint32_t)(twoBytes ? 2 : oneByte ? 1 : 0) > trackSize)
        {
            break;
        }

        if (evch == channel)
        {
            if (evtype == CONTROL_CHANGE)
            {
                unsigned char cc  = trackData[peekOffset];
                unsigned char val = trackData[peekOffset + 1];
                if (cc == BANK_MSB)
                {
                    *bank = (uint16_t)(((uint16_t)val << 7) | (*bank & 0x7F));
                }
                else if (cc == BANK_LSB)
                {
                    *bank = (uint16_t)((*bank & 0x3F80) | (val & 0x7F));
                }
                peekOffset += 2;
            }
            else if (evtype == PROGRAM_CHANGE)
            {
                *program = trackData[peekOffset];
                peekOffset += 1;
            }
            else
            {
                peekOffset += (unsigned)(twoBytes ? 2 : oneByte ? 1 : 0);
            }
        }
        else
        {
            peekOffset += (unsigned)(twoBytes ? 2 : oneByte ? 1 : 0);
        }
    }
}


BAERmfEditorActiveNote *PV_PushActiveNote(BAERmfEditorActiveNote **head,
                                                 uint32_t startTick,
                                                 uint32_t noteOnOrder,
                                                 unsigned char channel,
                                                 unsigned char note,
                                                 unsigned char velocity,
                                                 uint16_t bank,
                                                 unsigned char program)
{
    BAERmfEditorActiveNote *activeNote;

    activeNote = (BAERmfEditorActiveNote *)XNewPtr(sizeof(BAERmfEditorActiveNote));
    if (!activeNote)
    {
        return NULL;
    }
    XSetMemory(activeNote, sizeof(*activeNote), 0);
    activeNote->startTick = startTick;
    activeNote->noteOnOrder = noteOnOrder;
    activeNote->channel = channel;
    activeNote->note = note;
    activeNote->velocity = velocity;
    activeNote->bank = bank;
    activeNote->program = program;
    activeNote->next = *head;
    *head = activeNote;
    return activeNote;
}


BAERmfEditorActiveNote *PV_PopActiveNote(BAERmfEditorActiveNote **head,
                                                unsigned char channel,
                                                unsigned char note)
{
    BAERmfEditorActiveNote *current;
    BAERmfEditorActiveNote *previous;
    BAERmfEditorActiveNote *oldestMatch;
    BAERmfEditorActiveNote *oldestMatchPrevious;

    if (!head)
    {
        return NULL;
    }
    previous = NULL;
    current = *head;
    oldestMatch = NULL;
    oldestMatchPrevious = NULL;
    while (current)
    {
        if (current->channel == channel && current->note == note)
        {
            oldestMatch = current;
            oldestMatchPrevious = previous;
        }
        previous = current;
        current = current->next;
    }
    if (!oldestMatch)
    {
        return NULL;
    }
    if (oldestMatchPrevious)
    {
        oldestMatchPrevious->next = oldestMatch->next;
    }
    else
    {
        *head = oldestMatch->next;
    }
    oldestMatch->next = NULL;
    return oldestMatch;
}


void PV_DisposeActiveNotes(BAERmfEditorActiveNote **head)
{
    BAERmfEditorActiveNote *current;

    if (!head)
    {
        return;
    }
    current = *head;
    while (current)
    {
        BAERmfEditorActiveNote *next;

        next = current->next;
        XDisposePtr(current);
        current = next;
    }
    *head = NULL;
}


BAEResult PV_FinalizeActiveNotes(BAERmfEditorTrack *track,
                                        BAERmfEditorActiveNote **activeNotes,
                                        uint32_t finalTick)
{
    BAEResult result;

    result = BAE_NO_ERROR;
    while (activeNotes && *activeNotes)
    {
        BAERmfEditorActiveNote *activeNote;
        uint32_t durationTicks;

        activeNote = *activeNotes;
        *activeNotes = activeNote->next;
        durationTicks = (finalTick > activeNote->startTick) ? (finalTick - activeNote->startTick) : 1;
        if (result == BAE_NO_ERROR)
        {
            result = PV_AddNoteToTrack(track,
                                       activeNote->startTick,
                                       durationTicks,
                                       activeNote->note,
                                       activeNote->velocity,
                                       activeNote->channel,
                                       activeNote->bank,
                                       activeNote->program,
                                       (unsigned char)(NOTE_OFF | (activeNote->channel & 0x0F)),
                                       0,
                                       activeNote->noteOnOrder,
                                       track->nextEventOrder++);
        }
        XDisposePtr(activeNote);
    }
    return result;
}


int PV_CompareChannelStateEvents(void const *left, void const *right)
{
    PV_ChannelStateEvent const *a;
    PV_ChannelStateEvent const *b;

    a = (PV_ChannelStateEvent const *)left;
    b = (PV_ChannelStateEvent const *)right;
    if (a->tick < b->tick)
    {
        return -1;
    }
    if (a->tick > b->tick)
    {
        return 1;
    }
    if (a->trackIndex < b->trackIndex)
    {
        return -1;
    }
    if (a->trackIndex > b->trackIndex)
    {
        return 1;
    }
    if (a->sequence < b->sequence)
    {
        return -1;
    }
    if (a->sequence > b->sequence)
    {
        return 1;
    }
    if (a->kind < b->kind)
    {
        return -1;
    }
    if (a->kind > b->kind)
    {
        return 1;
    }
    return 0;
}

BAEResult PV_ReconcileImportedMidiNotePrograms(BAERmfEditorDocument *document)
{
    uint32_t eventCount;
    uint32_t trackIndex;
    uint32_t trackCount;
    uint16_t *perTrackBank;     /* [trackCount * BAE_MAX_MIDI_CHANNELS] */
    unsigned char *perTrackProgram; /* [trackCount * BAE_MAX_MIDI_CHANNELS] */
    unsigned char *trackHasBank;    /* [trackCount * BAE_MAX_MIDI_CHANNELS] - set when track has CC0/CC32 for ch */
    unsigned char *trackHasProgram; /* [trackCount * BAE_MAX_MIDI_CHANNELS] - set when track has PC for ch */
    uint16_t globalBank[BAE_MAX_MIDI_CHANNELS];
    unsigned char globalProgram[BAE_MAX_MIDI_CHANNELS];
    PV_ChannelStateEvent *events;

    if (!document)
    {
        return BAE_PARAM_ERR;
    }

    trackCount = document->trackCount;
    eventCount = 0;
    for (trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        BAERmfEditorTrack const *track;
        uint32_t i;

        track = &document->tracks[trackIndex];
        eventCount += track->noteCount;
        for (i = 0; i < track->auxEventCount; ++i)
        {
            BAERmfEditorAuxEvent const *aux;
            unsigned char eventType;

            aux = &track->auxEvents[i];
            eventType = (unsigned char)(aux->status & 0xF0);
            if (eventType == PROGRAM_CHANGE && aux->dataBytes >= 1)
            {
                eventCount++;
            }
            else if (eventType == CONTROL_CHANGE && aux->dataBytes >= 2 &&
                     (aux->data1 == BANK_MSB || aux->data1 == BANK_LSB))
            {
                eventCount++;
            }
        }
    }

    if (eventCount == 0)
    {
        return BAE_NO_ERROR;
    }

    events = (PV_ChannelStateEvent *)XNewPtr((int32_t)(sizeof(PV_ChannelStateEvent) * eventCount));
    if (!events)
    {
        return BAE_MEMORY_ERR;
    }

    /* Allocate per-track channel state arrays. */
    perTrackBank    = (uint16_t *)XNewPtr((int32_t)(trackCount * BAE_MAX_MIDI_CHANNELS * (int32_t)sizeof(uint16_t)));
    perTrackProgram = (unsigned char *)XNewPtr((int32_t)(trackCount * BAE_MAX_MIDI_CHANNELS));
    trackHasBank    = (unsigned char *)XNewPtr((int32_t)(trackCount * BAE_MAX_MIDI_CHANNELS));
    trackHasProgram = (unsigned char *)XNewPtr((int32_t)(trackCount * BAE_MAX_MIDI_CHANNELS));
    if (!perTrackBank || !perTrackProgram || !trackHasBank || !trackHasProgram)
    {
        XDisposePtr(events);
        if (perTrackBank)    XDisposePtr(perTrackBank);
        if (perTrackProgram) XDisposePtr(perTrackProgram);
        if (trackHasBank)    XDisposePtr(trackHasBank);
        if (trackHasProgram) XDisposePtr(trackHasProgram);
        return BAE_MEMORY_ERR;
    }
    XSetMemory(perTrackBank,    (int32_t)(trackCount * BAE_MAX_MIDI_CHANNELS * (int32_t)sizeof(uint16_t)), 0);
    XSetMemory(perTrackProgram, (int32_t)(trackCount * BAE_MAX_MIDI_CHANNELS), 0);
    XSetMemory(trackHasBank,    (int32_t)(trackCount * BAE_MAX_MIDI_CHANNELS), 0);
    XSetMemory(trackHasProgram, (int32_t)(trackCount * BAE_MAX_MIDI_CHANNELS), 0);
    XSetMemory(globalBank,      (int32_t)sizeof(globalBank), 0);
    XSetMemory(globalProgram,   (int32_t)sizeof(globalProgram), 0);

    {
        uint32_t writeIndex;

        writeIndex = 0;
        for (trackIndex = 0; trackIndex < trackCount; ++trackIndex)
        {
            BAERmfEditorTrack const *track;
            uint32_t i;

            track = &document->tracks[trackIndex];

            for (i = 0; i < track->auxEventCount; ++i)
            {
                BAERmfEditorAuxEvent const *aux;
                unsigned char eventType;
                PV_ChannelStateEvent *ev;

                aux = &track->auxEvents[i];
                eventType = (unsigned char)(aux->status & 0xF0);
                if (eventType == PROGRAM_CHANGE && aux->dataBytes >= 1)
                {
                    ev = &events[writeIndex++];
                    ev->tick = aux->tick;
                    ev->trackIndex = (uint16_t)trackIndex;
                    ev->sequence = aux->eventOrder;
                    ev->kind = 2;
                    ev->channel = (unsigned char)(aux->status & 0x0F);
                    ev->value = aux->data1;
                    ev->noteIndex = 0;
                    trackHasProgram[trackIndex * BAE_MAX_MIDI_CHANNELS + ev->channel] = 1;
                }
                else if (eventType == CONTROL_CHANGE && aux->dataBytes >= 2 &&
                         (aux->data1 == BANK_MSB || aux->data1 == BANK_LSB))
                {
                    ev = &events[writeIndex++];
                    ev->tick = aux->tick;
                    ev->trackIndex = (uint16_t)trackIndex;
                    ev->sequence = aux->eventOrder;
                    ev->kind = (aux->data1 == BANK_MSB) ? 0 : 1;
                    ev->channel = (unsigned char)(aux->status & 0x0F);
                    ev->value = aux->data2;
                    ev->noteIndex = 0;
                    trackHasBank[trackIndex * BAE_MAX_MIDI_CHANNELS + ev->channel] = 1;
                }
            }

            for (i = 0; i < track->noteCount; ++i)
            {
                BAERmfEditorNote const *note;
                PV_ChannelStateEvent *ev;

                note = &track->notes[i];
                ev = &events[writeIndex++];
                ev->tick = note->startTick;
                ev->trackIndex = (uint16_t)trackIndex;
                ev->sequence = note->noteOnOrder;
                ev->kind = 3;
                ev->channel = note->channel;
                ev->value = 0;
                ev->noteIndex = i;
            }
        }
        eventCount = writeIndex;
    }

    qsort(events, eventCount, sizeof(PV_ChannelStateEvent), PV_CompareChannelStateEvents);

#define PV_PTB(t, ch)  perTrackBank[(t) * BAE_MAX_MIDI_CHANNELS + (ch)]
#define PV_PTP(t, ch)  perTrackProgram[(t) * BAE_MAX_MIDI_CHANNELS + (ch)]
#define PV_THB(t, ch)  trackHasBank[(t) * BAE_MAX_MIDI_CHANNELS + (ch)]
#define PV_THP(t, ch)  trackHasProgram[(t) * BAE_MAX_MIDI_CHANNELS + (ch)]

    {
        uint32_t i;

        for (i = 0; i < eventCount; ++i)
        {
            PV_ChannelStateEvent const *ev;
            unsigned char ch;
            uint16_t ti;

            ev = &events[i];
            ch = ev->channel;
            ti = ev->trackIndex;

            if (ev->kind == 0)
            {
                PV_PTB(ti, ch) = (uint16_t)((((uint16_t)ev->value) << 7) | (PV_PTB(ti, ch) & 0x7F));
                globalBank[ch] = (uint16_t)((((uint16_t)ev->value) << 7) | (globalBank[ch] & 0x7F));
            }
            else if (ev->kind == 1)
            {
                PV_PTB(ti, ch) = (uint16_t)((PV_PTB(ti, ch) & 0x3F80) | ((uint16_t)ev->value & 0x7F));
                globalBank[ch] = (uint16_t)((globalBank[ch] & 0x3F80) | ((uint16_t)ev->value & 0x7F));
            }
            else if (ev->kind == 2)
            {
                PV_PTP(ti, ch) = ev->value;
                globalProgram[ch] = ev->value;
            }
            else if (ev->kind == 3)
            {
                BAERmfEditorTrack *track;
                BAERmfEditorNote *note;
                uint16_t noteBank;
                unsigned char noteProgram;
                uint32_t j;

                track = &document->tracks[ti];
                if (ev->noteIndex >= track->noteCount)
                {
                    continue;
                }
                note = &track->notes[ev->noteIndex];

                /* Use per-track state when the track has its own bank/program
                   events for this channel; fall back to global state when
                   the track has none (cross-track setup pattern). */
                noteBank    = PV_THB(ti, ch) ? PV_PTB(ti, ch) : globalBank[ch];
                noteProgram = PV_THP(ti, ch) ? PV_PTP(ti, ch) : globalProgram[ch];

                /* Keep compatibility with same-tick setup that appears after the
                   note-on in the same track by scanning forward at the same tick. */
                for (j = i + 1; j < eventCount; ++j)
                {
                    PV_ChannelStateEvent const *next;

                    next = &events[j];
                    if (next->tick != ev->tick)
                    {
                        break;
                    }
                    if (next->channel != ch)
                    {
                        continue;
                    }
                    /* Accept same-tick bank/program from any track when this
                       track has no events of its own for this channel, otherwise
                       restrict to the same track. */
                    if (PV_THB(ti, ch) && next->trackIndex != ti)
                    {
                        continue;
                    }
                    if (next->kind == 0)
                    {
                        noteBank = (uint16_t)((((uint16_t)next->value) << 7) | (noteBank & 0x7F));
                    }
                    else if (next->kind == 1)
                    {
                        noteBank = (uint16_t)((noteBank & 0x3F80) | ((uint16_t)next->value & 0x7F));
                    }
                    else if (next->kind == 2)
                    {
                        noteProgram = next->value;
                    }
                }

                note->bank    = noteBank;
                note->program = noteProgram;
            }
        }
    }

#undef PV_PTB
#undef PV_PTP
#undef PV_THB
#undef PV_THP

    XDisposePtr(perTrackBank);
    XDisposePtr(perTrackProgram);
    XDisposePtr(trackHasBank);
    XDisposePtr(trackHasProgram);
    XDisposePtr(events);
    return BAE_NO_ERROR;
}


BAEResult PV_LoadMidiTrackIntoDocument(BAERmfEditorDocument *document,
                                              unsigned char const *trackData,
                                              uint32_t trackSize)
{
    BAERmfEditorTrackSetup setup;
    BAERmfEditorTrack *track;
    BAERmfEditorActiveNote *activeNotes;
    uint16_t trackIndex;
    uint32_t offset;
    uint32_t currentTick;
    unsigned char runningStatus;
    BAEResult result;
    bool sawChannel;
    uint16_t channelBank[BAE_MAX_MIDI_CHANNELS];
    unsigned char channelProgram[BAE_MAX_MIDI_CHANNELS];
    uint16_t initChannel;

    XSetMemory(&setup, sizeof(setup), 0);
    setup.channel = 0;
    setup.bank = 0;
    setup.program = 0;
    setup.name = NULL;
    result = BAERmfEditorDocument_AddTrack(document, &setup, &trackIndex);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    track = &document->tracks[trackIndex];
    track->pan = 64;
    track->volume = 127;
    track->transpose = 0;
    activeNotes = NULL;
    offset = 0;
    currentTick = 0;
    runningStatus = 0;
    sawChannel = FALSE;
    for (initChannel = 0; initChannel < BAE_MAX_MIDI_CHANNELS; ++initChannel)
    {
        channelBank[initChannel] = 0;
        channelProgram[initChannel] = 0;
    }

    while (offset < trackSize)
    {
        uint32_t delta;
        unsigned char status;
        unsigned char eventType;
        unsigned char channel;

        result = PV_ReadVLQ(trackData, trackSize, &offset, &delta);
        if (result != BAE_NO_ERROR)
        {
            PV_DisposeActiveNotes(&activeNotes);
            return result;
        }
        currentTick += delta;
        if (offset >= trackSize)
        {
            break;
        }
        status = trackData[offset++];
        if (status < 0x80)
        {
            if (runningStatus == 0)
            {
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
            }
            offset--;
            status = runningStatus;
        }
        else if (status < 0xF0)
        {
            runningStatus = status;
        }
        else
        {
            runningStatus = 0;
        }

        if (status == 0xFF)
        {
            unsigned char metaType;
            uint32_t metaLength;

            if (offset >= trackSize)
            {
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
            }
            metaType = trackData[offset++];
            result = PV_ReadVLQ(trackData, trackSize, &offset, &metaLength);
            if (result != BAE_NO_ERROR || offset + metaLength > trackSize)
            {
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
            }
            if (metaType == 0x03)
            {
                char *nameCopy;

                nameCopy = (char *)XNewPtr((int32_t)(metaLength + 1));
                if (!nameCopy)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return BAE_MEMORY_ERR;
                }
                if (metaLength)
                {
                    XBlockMove(trackData + offset, nameCopy, (int32_t)metaLength);
                }
                nameCopy[metaLength] = 0;
                PV_FreeString(&track->name);
                track->name = nameCopy;

                result = PV_AddMetaEventToTrack(track, currentTick, metaType, trackData + offset, metaLength);
                if (result != BAE_NO_ERROR)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return result;
                }
            }
            else if (metaType == 0x51 && metaLength == 3)
            {
                uint32_t microsecondsPerQuarter;

                microsecondsPerQuarter = ((uint32_t)trackData[offset] << 16) |
                                         ((uint32_t)trackData[offset + 1] << 8) |
                                         (uint32_t)trackData[offset + 2];
                if (microsecondsPerQuarter > 0)
                {
                    result = PV_AddTempoEvent(document, currentTick, microsecondsPerQuarter);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                    if (document->tempoEventCount == 1)
                    {
                        document->tempoBPM = 60000000UL / microsecondsPerQuarter;
                    }
                }

                result = PV_AddMetaEventToTrack(track, currentTick, metaType, trackData + offset, metaLength);
                if (result != BAE_NO_ERROR)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return result;
                }
            }
            else if (metaType != 0x2F)
            {
                result = PV_AddMetaEventToTrack(track, currentTick, metaType, trackData + offset, metaLength);
                if (result != BAE_NO_ERROR)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return result;
                }
            }
            else if (metaType == 0x2F)
            {
                offset += metaLength;
                track->endOfTrackTick = currentTick;
                break;
            }
            offset += metaLength;
            continue;
        }
        if (status == 0xF0 || status == 0xF7)
        {
            uint32_t sysexLength;

            result = PV_ReadVLQ(trackData, trackSize, &offset, &sysexLength);
            if (result != BAE_NO_ERROR || offset + sysexLength > trackSize)
            {
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
            }
            result = PV_AddSysExEventToTrack(track, currentTick, status, trackData + offset, sysexLength);
            if (result != BAE_NO_ERROR)
            {
                PV_DisposeActiveNotes(&activeNotes);
                return result;
            }
            offset += sysexLength;
            continue;
        }

        eventType = (unsigned char)(status & 0xF0);
        channel = (unsigned char)(status & 0x0F);
        if (sawChannel == FALSE)
        {
            track->channel = channel;
            sawChannel = TRUE;
        }

        switch (eventType)
        {
            case NOTE_OFF:
            case NOTE_ON:
            {
                unsigned char noteValue;
                unsigned char velocity;
                BAERmfEditorActiveNote *activeNote;

                if (offset + 2 > trackSize)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return BAE_BAD_FILE;
                }
                noteValue = trackData[offset++];
                velocity = trackData[offset++];
                if (eventType == NOTE_ON && velocity > 0)
                {
                    uint32_t noteOnOrder;
                    uint16_t noteBank;
                    unsigned char noteProgram;

                    /* Look ahead over any remaining zero-delta events at this
                       tick so that a Program Change or Bank Select that follows
                       the NoteOn at the same tick is picked up.  This is a
                       common authoring pattern (e.g. Rhodium.rmf) where the
                       instrument is finalised just after the first note. */
                    noteBank    = channelBank[channel];
                    noteProgram = channelProgram[channel];
                    PV_PeekSameTickBankProgram(trackData, trackSize, offset,
                                              channel, runningStatus,
                                              &noteBank, &noteProgram);

                    noteOnOrder = track->nextEventOrder++;
                    activeNote = PV_PushActiveNote(&activeNotes,
                                                   currentTick,
                                                   noteOnOrder,
                                                   channel,
                                                   noteValue,
                                                   velocity,
                                                   noteBank,
                                                   noteProgram);
                    if (!activeNote)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return BAE_MEMORY_ERR;
                    }
                }
                else
                {
                    uint32_t noteOffOrder;

                    noteOffOrder = track->nextEventOrder++;
                    activeNote = PV_PopActiveNote(&activeNotes, channel, noteValue);
                    if (activeNote)
                    {
                        uint32_t durationTicks;

                        durationTicks = (currentTick > activeNote->startTick) ?
                                        (currentTick - activeNote->startTick) : 1;
                        result = PV_AddNoteToTrack(track,
                                                   activeNote->startTick,
                                                   durationTicks,
                                                   activeNote->note,
                                                   activeNote->velocity,
                                                   activeNote->channel,
                                                   activeNote->bank,
                                                   activeNote->program,
                                                   status,
                                                   velocity,
                                                   activeNote->noteOnOrder,
                                                   noteOffOrder);
                        XDisposePtr(activeNote);
                        if (result != BAE_NO_ERROR)
                        {
                            PV_DisposeActiveNotes(&activeNotes);
                            return result;
                        }
                    }
                }
                break;
            }
            case POLY_AFTERTOUCH:
            case CONTROL_CHANGE:
            case PITCH_BEND:
            {
                unsigned char data1;
                unsigned char data2;

                if (offset + 2 > trackSize)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return BAE_BAD_FILE;
                }
                data1 = trackData[offset++];
                data2 = trackData[offset++];
                if (eventType == CONTROL_CHANGE)
                {
                    if (data1 == BANK_MSB)
                    {
                        channelBank[channel] = (uint16_t)(((uint16_t)data2 << 7) | (channelBank[channel] & 0x7F));
                        result = PV_AddAuxEventToTrack(track,
                                                       currentTick,
                                                       status,
                                                       data1,
                                                       data2,
                                                       2);
                        if (result != BAE_NO_ERROR)
                        {
                            PV_DisposeActiveNotes(&activeNotes);
                            return result;
                        }
                        if (channel == track->channel)
                        {
                            track->bank = (uint16_t)(((uint16_t)data2 << 7) | (track->bank & 0x7F));
                        }
                    }
                    else if (data1 == BANK_LSB)
                    {
                        channelBank[channel] = (uint16_t)((channelBank[channel] & 0x3F80) | (uint16_t)(data2 & 0x7F));
                        result = PV_AddAuxEventToTrack(track,
                                                       currentTick,
                                                       status,
                                                       data1,
                                                       data2,
                                                       2);
                        if (result != BAE_NO_ERROR)
                        {
                            PV_DisposeActiveNotes(&activeNotes);
                            return result;
                        }
                        if (channel == track->channel)
                        {
                            track->bank = (uint16_t)((track->bank & 0x3F80) | (uint16_t)(data2 & 0x7F));
                        }
                    }
                    else if (data1 == 7 && channel == track->channel)
                    {
                        track->volume = data2;
                    }
                    else if (data1 == 10 && channel == track->channel)
                    {
                        track->pan = data2;
                    }
                    /* Store all CCs for this channel except bank selects (handled via per-note bank tracking). */
                    if (channel == track->channel && data1 != BANK_MSB && data1 != BANK_LSB)
                    {
                        result = PV_AddCCEventToTrack(track, currentTick, data1, data2, 0);
                        if (result != BAE_NO_ERROR)
                        {
                            PV_DisposeActiveNotes(&activeNotes);
                            return result;
                        }
                    }
                    else if (channel != track->channel && data1 != BANK_MSB && data1 != BANK_LSB)
                    {
                        result = PV_AddAuxEventToTrack(track,
                                                       currentTick,
                                                       status,
                                                       data1,
                                                       data2,
                                                       2);
                        if (result != BAE_NO_ERROR)
                        {
                            PV_DisposeActiveNotes(&activeNotes);
                            return result;
                        }
                    }
                }
                else if (eventType == PITCH_BEND && channel == track->channel)
                {
                    /* Pitch bend: data1=LSB, data2=MSB. */
                    result = PV_AddCCEventToTrack(track,
                                                  currentTick,
                                                  BAE_EDITOR_CC_PITCH_BEND_SENTINEL,
                                                  data1,
                                                  data2);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                }
                else if (eventType == PITCH_BEND)
                {
                    result = PV_AddAuxEventToTrack(track,
                                                   currentTick,
                                                   status,
                                                   data1,
                                                   data2,
                                                   2);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                }
                else if (eventType == POLY_AFTERTOUCH && channel == track->channel)
                {
                    result = PV_AddCCEventToTrack(track,
                                                  currentTick,
                                                  BAE_EDITOR_CC_POLY_AFTERTOUCH_SENTINEL,
                                                  data1,
                                                  data2);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                }
                else if (eventType == POLY_AFTERTOUCH)
                {
                    result = PV_AddAuxEventToTrack(track,
                                                   currentTick,
                                                   status,
                                                   data1,
                                                   data2,
                                                   2);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                }
                break;
            }
            case PROGRAM_CHANGE:
            case CHANNEL_AFTERTOUCH:
            {
                unsigned char data1;

                if (offset + 1 > trackSize)
                {
                    PV_DisposeActiveNotes(&activeNotes);
                    return BAE_BAD_FILE;
                }
                data1 = trackData[offset++];
                if (eventType == PROGRAM_CHANGE)
                {
                    channelProgram[channel] = data1;
                    result = PV_AddAuxEventToTrack(track,
                                                   currentTick,
                                                   status,
                                                   data1,
                                                   0,
                                                   1);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                    if (channel == track->channel)
                    {
                        track->program = data1;
                    }
                }
                else if (eventType == CHANNEL_AFTERTOUCH && channel == track->channel)
                {
                    result = PV_AddCCEventToTrack(track,
                                                  currentTick,
                                                  BAE_EDITOR_CC_CHANNEL_AFTERTOUCH_SENTINEL,
                                                  data1,
                                                  0);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                }
                else if (eventType == CHANNEL_AFTERTOUCH)
                {
                    result = PV_AddAuxEventToTrack(track,
                                                   currentTick,
                                                   status,
                                                   data1,
                                                   0,
                                                   1);
                    if (result != BAE_NO_ERROR)
                    {
                        PV_DisposeActiveNotes(&activeNotes);
                        return result;
                    }
                }
                break;
            }
            default:
                PV_DisposeActiveNotes(&activeNotes);
                return BAE_BAD_FILE;
        }
    }

    result = PV_FinalizeActiveNotes(track, &activeNotes, currentTick);
    PV_DisposeActiveNotes(&activeNotes);
    return result;
}


BAEResult PV_LoadMidiBytesIntoDocument(BAERmfEditorDocument *document,
                                              unsigned char const *data,
                                              uint32_t dataSize)
{
    uint32_t headerLength;
    uint16_t trackCount;
    uint16_t division;
    uint32_t offset;
    uint16_t trackIndex;

    if (!document || !data || dataSize < 14)
    {
        return BAE_BAD_FILE;
    }
    if (memcmp(data, "MThd", 4) != 0)
    {
        return BAE_BAD_FILE;
    }
    headerLength = PV_ReadBE32(data + 4);
    if (headerLength < 6 || dataSize < 8 + headerLength)
    {
        return BAE_BAD_FILE;
    }
    trackCount = PV_ReadBE16(data + 10);
    division = PV_ReadBE16(data + 12);
    if ((division & 0x8000) == 0 && division != 0)
    {
        document->ticksPerQuarter = division;
    }
    PV_ClearTempoEvents(document);
    offset = 8 + headerLength;
    for (trackIndex = 0; trackIndex < trackCount; ++trackIndex)
    {
        uint32_t trackLength;
        BAEResult result;

        if (offset + 8 > dataSize || memcmp(data + offset, "MTrk", 4) != 0)
        {
            return BAE_BAD_FILE;
        }
        trackLength = PV_ReadBE32(data + offset + 4);
        offset += 8;
        if (offset + trackLength > dataSize)
        {
            return BAE_BAD_FILE;
        }
        result = PV_LoadMidiTrackIntoDocument(document, data + offset, trackLength);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        offset += trackLength;
    }

    {
        BAEResult reconcileResult;

        reconcileResult = PV_ReconcileImportedMidiNotePrograms(document);
        if (reconcileResult != BAE_NO_ERROR)
        {
            return reconcileResult;
        }
    }

    if (document->tempoEventCount > 0 && document->tempoEvents[0].microsecondsPerQuarter > 0)
    {
        document->tempoBPM = 60000000UL / document->tempoEvents[0].microsecondsPerQuarter;
    }
    return (document->trackCount > 0) ? BAE_NO_ERROR : BAE_BAD_FILE;
}


/* Load standard MIDI, or transparently decompress Nokia MThc containers first
 * (same behavior as BAESong_LoadMidiFromMemory). Always stores decoded SMF as
 * the document's original MIDI bytes when successful. */
BAEResult PV_LoadMidiOrMthcBytesIntoDocument(BAERmfEditorDocument *document,
                                                    unsigned char const *data,
                                                    uint32_t dataSize)
{
    unsigned char const *midiData;
    uint32_t midiSize;
    BAEResult result;
#if USE_MTHC_SUPPORT == TRUE
    unsigned char *decodedMthcMidi = NULL;
    uint32_t decodedMthcMidiLen = 0;
#endif

    if (!document || !data || dataSize == 0)
    {
        return BAE_BAD_FILE;
    }

    midiData = data;
    midiSize = dataSize;

#if USE_MTHC_SUPPORT == TRUE
    if (dataSize >= 4 &&
        data[0] == 'M' && data[1] == 'T' && data[2] == 'h' && data[3] == 'c')
    {
        if (mthc_decompress_memory(data, dataSize, &decodedMthcMidi, &decodedMthcMidiLen) != 0 ||
            !decodedMthcMidi ||
            decodedMthcMidiLen < 4 ||
            decodedMthcMidi[0] != 'M' || decodedMthcMidi[1] != 'T' ||
            decodedMthcMidi[2] != 'h' || decodedMthcMidi[3] != 'd')
        {
            if (decodedMthcMidi)
            {
                free(decodedMthcMidi);
            }
            return BAE_BAD_FILE;
        }
        midiData = decodedMthcMidi;
        midiSize = decodedMthcMidiLen;
    }
#endif

    result = PV_LoadMidiBytesIntoDocument(document, midiData, midiSize);
    if (result == BAE_NO_ERROR)
    {
        result = PV_SetDebugOriginalMidiData(document, midiData, midiSize);
    }
    if (result == BAE_NO_ERROR)
    {
        document->isPristine = TRUE;
    }

#if USE_MTHC_SUPPORT == TRUE
    if (decodedMthcMidi)
    {
        free(decodedMthcMidi);
    }
#endif
    return result;
}


/* Add a bank alias sample: stores only metadata + bank/SND reference, no PCM or SND blob.
 * If fileRef is non-NULL and the SND can be found, sample metadata (rate, channels, etc.)
 * is populated from the SND header.  Otherwise a minimal entry is created. */
BAEResult PV_AddBankAliasSample(BAERmfEditorDocument *document,
                                       XFILE fileRef,
                                       BAEBankToken bankToken,
                                       XLongResourceID instID,
                                       char const *displayName,
                                       unsigned char program,
                                       XShortResourceID sndID,
                                       unsigned char rootKey,
                                       unsigned char lowKey,
                                       unsigned char highKey)
{
    BAERmfEditorSample *sample;
    BAEResult growResult;
    SampleDataInfo sdi;
    bool haveSdi;

    XSetMemory(&sdi, sizeof(sdi), 0);
    haveSdi = FALSE;

    /* Try to read the SND header for metadata when a file reference is available. */
    if (fileRef)
    {
        XPTR sndData;
        int32_t sndSize;
        XPTR pcmData;

        XFileUseThisResourceFile(fileRef);
        sndData = XGetSoundResourceByID((XLongResourceID)sndID, &sndSize);
        if (sndData)
        {
            pcmData = XGetSamplePtrFromSnd(sndData, &sdi);
            if (sdi.pMasterPtr && sdi.pMasterPtr != sndData)
            {
                XDisposePtr(sdi.pMasterPtr);
            }
            XDisposePtr(sndData);
            if (pcmData)
            {
                haveSdi = TRUE;
            }
        }
    }

    /* Resolve rootKey from SND header if not provided by INST. */
    if (rootKey == 0)
    {
        if (lowKey <= 127 && highKey <= 127 && lowKey == highKey)
        {
            rootKey = lowKey;
        }
        else if (haveSdi && sdi.baseKey > 0 && sdi.baseKey <= 127)
        {
            rootKey = (unsigned char)sdi.baseKey;
        }
        else
        {
            rootKey = 60;
        }
    }

    growResult = PV_GrowBuffer((void **)&document->samples,
                               &document->sampleCapacity,
                               sizeof(BAERmfEditorSample),
                               document->sampleCount + 1);
    if (growResult != BAE_NO_ERROR)
    {
        return growResult;
    }
    sample = &document->samples[document->sampleCount];
    XSetMemory(sample, sizeof(*sample), 0);
    sample->waveform = NULL;
    sample->originalSndData = NULL;
    sample->originalSndSize = 0;
    sample->targetOpusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
    sample->isBankAlias = TRUE;
    sample->aliasBankToken = bankToken;
    sample->aliasSndResourceID = sndID;
    sample->program = program;
    sample->instID = (uint32_t)instID;
    sample->sampleAssetID = (uint32_t)sndID;
    if (sample->sampleAssetID == 0)
    {
        sample->sampleAssetID = PV_AllocateSampleAssetID(document);
    }
    else
    {
        PV_NoteSampleAssetID(document, sample->sampleAssetID);
    }
    sample->rootKey = rootKey;
    sample->lowKey = lowKey;
    sample->highKey = highKey;
    sample->targetCompressionType = BAE_EDITOR_COMPRESSION_DONT_CHANGE;
    if (haveSdi)
    {
        sample->sourceCompressionType = sdi.compressionType;
        sample->sampleInfo.bitSize = (uint16_t)sdi.bitSize;
        sample->sampleInfo.channels = (uint16_t)sdi.channels;
        sample->sampleInfo.baseMidiPitch = rootKey;
        sample->sampleInfo.waveSize = (uint32_t)(sdi.frames * (sdi.bitSize / 8) * sdi.channels);
        sample->sampleInfo.waveFrames = sdi.frames;
        sample->sampleInfo.startLoop = sdi.loopStart;
        sample->sampleInfo.endLoop = sdi.loopEnd;
        sample->sampleInfo.sampledRate = (BAE_UNSIGNED_FIXED)sdi.rate;
    }
    else
    {
        sample->sampleInfo.baseMidiPitch = rootKey;
    }
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
    document->sampleCount++;
    return BAE_NO_ERROR;
}


/* Map XInstrumentData units into BAERmfEditorInstrumentExt unit fields. */
void PV_FillExtFromXInstrument(BAERmfEditorInstrumentExt *ext, XInstrumentData const *x)
{
    int32_t count;
    int32_t count2;
    bool haveVolumeADSR;

    if (!ext || !x)
    {
        return;
    }

    haveVolumeADSR = FALSE;
    for (count = 0; count < x->unitCount; count++)
    {
        XUnitData const *unit = &x->units[count];
        switch (unit->unitType)
        {
            case INST_ADSR_ENVELOPE:
            {
                XEnvelopeData const *env = &unit->u.envelopeADSR;
                int32_t stageCount = env->stageCount;
                if (stageCount > EDITOR_MAX_ADSR_STAGES)
                {
                    stageCount = EDITOR_MAX_ADSR_STAGES;
                }
                ext->volumeADSR.stageCount = (uint32_t)stageCount;
                for (count2 = 0; count2 < stageCount; count2++)
                {
                    ext->volumeADSR.stages[count2].level = env->level[count2];
                    ext->volumeADSR.stages[count2].time = env->time[count2];
                    ext->volumeADSR.stages[count2].flags = env->flags[count2];
                }
                haveVolumeADSR = TRUE;
                break;
            }

            case INST_DEFAULT_MOD:
                ext->hasDefaultMod = TRUE;
                break;

            case INST_REVERB_SEND:
                ext->defaultReverbSend = (int16_t)unit->u.sendAmount;
                break;

            case INST_CHORUS_SEND:
                ext->defaultChorusSend = (int16_t)unit->u.sendAmount;
                break;

            case INST_LOW_PASS_FILTER:
                ext->LPF_frequency = unit->u.lpf.LPF_frequency;
                ext->LPF_resonance = unit->u.lpf.LPF_resonance;
                ext->LPF_lowpassAmount = unit->u.lpf.LPF_lowpassAmount;
                break;

#if USE_ZMF_SUPPORT == TRUE
            case INST_OSCILLATOR:
                ext->useOscillator = TRUE;
                ext->oscWaveShape = unit->u.osc.waveShape;
                ext->oscPulseWidth = unit->u.osc.pulseWidth;
                ext->oscVolume = unit->u.osc.volume;
                if (ext->oscVolume < 0)
                    ext->oscVolume = 0;
                if (ext->oscVolume > 65536)
                    ext->oscVolume = 65536;
                break;
#endif

            case INST_EXPONENTIAL_CURVE:
                if (ext->curveCount >= EDITOR_MAX_CURVES)
                {
                    break;
                }
                {
                    EditorCurve *pCurve = &ext->curves[ext->curveCount];
                    XTieToData const *src = &unit->u.curve;
                    int32_t curvePoints = src->curveCount;
                    if (curvePoints > EDITOR_MAX_ADSR_STAGES)
                    {
                        curvePoints = EDITOR_MAX_ADSR_STAGES;
                    }
                    if (curvePoints > X_INSTRUMENT_MAX_CURVE_POINTS)
                    {
                        curvePoints = X_INSTRUMENT_MAX_CURVE_POINTS;
                    }
                    pCurve->tieFrom = (int32_t)src->tieFrom;
                    pCurve->tieTo = (int32_t)src->tieTo;
                    pCurve->curveCount = (int16_t)curvePoints;
                    for (count2 = 0; count2 < curvePoints; count2++)
                    {
                        pCurve->from_Value[count2] = src->from_Value[count2];
                        pCurve->to_Scalar[count2] = src->to_Scalar[count2];
                    }
                    ext->curveCount++;
                }
                break;

            case INST_PITCH_LFO:
            case INST_VOLUME_LFO:
            case INST_STEREO_PAN_LFO:
            case INST_STEREO_PAN_NAME2:
            case INST_LOW_PASS_AMOUNT:
            case INST_LPF_DEPTH:
            case INST_LPF_FREQUENCY:
#if USE_ZMF_SUPPORT == TRUE
            case INST_PULSE_WIDTH_LFO:
            case INST_WAVE_INDEX_LFO:
#endif
                if (ext->lfoCount >= EDITOR_MAX_LFOS)
                {
                    break;
                }
                {
                    EditorLFO *pLFO = &ext->lfos[ext->lfoCount];
                    XLFOData const *src = &unit->u.lfo;
                    int32_t stageCount = src->envelopeLFO.stageCount;
                    if (stageCount > EDITOR_MAX_ADSR_STAGES)
                    {
                        stageCount = EDITOR_MAX_ADSR_STAGES;
                    }
                    pLFO->destination = (int32_t)unit->unitType;
                    pLFO->period = src->period;
                    pLFO->waveShape = src->waveShape;
                    pLFO->DC_feed = src->DC_feed;
                    pLFO->level = src->depth;
                    pLFO->adsr.stageCount = (uint32_t)stageCount;
                    for (count2 = 0; count2 < stageCount; count2++)
                    {
                        pLFO->adsr.stages[count2].level = src->envelopeLFO.level[count2];
                        pLFO->adsr.stages[count2].time = src->envelopeLFO.time[count2];
                        pLFO->adsr.stages[count2].flags = src->envelopeLFO.flags[count2];
                    }
                    ext->lfoCount++;
                }
                break;

            default:
                break;
        }
    }

    if (!haveVolumeADSR)
    {
        /* Default ADSR: one TERMINATE stage at VOLUME_RANGE (4096) */
        ext->volumeADSR.stageCount = 1;
        ext->volumeADSR.stages[0].level = VOLUME_RANGE;
        ext->volumeADSR.stages[0].time = 0;
        ext->volumeADSR.stages[0].flags = ADSR_TERMINATE_LONG;
    }
}

/* Check whether a SND resource ID exists among the document's captured original resources. */
bool PV_SndExistsInOriginalResources(BAERmfEditorDocument const *document, XShortResourceID sndID)
{
    uint32_t i;
    for (i = 0; i < document->originalResourceCount; ++i)
    {
        if (document->originalResources[i].type == ID_SND ||
            document->originalResources[i].type == ID_CSND ||
            document->originalResources[i].type == ID_ESND)
        {
            if (document->originalResources[i].id == (XLongResourceID)sndID)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}


/* Decode raw MIDI resource data for encrypted/compressed types (ecmi, emid, cmid).
 * Takes ownership of 'raw'. Returns decoded/decompressed MIDI data or NULL on failure.
 * Updates *ioSize with the final decoded size. */
XPTR PV_DecodeMidiData(XPTR raw, XResourceType rtype, int32_t *ioSize)
{
    XPTR dec;

    if (!raw)
    {
        return NULL;
    }
    if (rtype == ID_ECMI)
    {
        debug_message("[RMF] ecmi: raw size=%ld, first bytes: %02x %02x %02x %02x\n",
                   (long)*ioSize,
                   ((unsigned char*)raw)[0], ((unsigned char*)raw)[1],
                   ((unsigned char*)raw)[2], ((unsigned char*)raw)[3]);
        XDecryptData(raw, (uint32_t)*ioSize);
        debug_message("[RMF] ecmi: after decrypt first bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   ((unsigned char*)raw)[0], ((unsigned char*)raw)[1],
                   ((unsigned char*)raw)[2], ((unsigned char*)raw)[3],
                   ((unsigned char*)raw)[4], ((unsigned char*)raw)[5],
                   ((unsigned char*)raw)[6], ((unsigned char*)raw)[7]);
        dec = XDecompressPtr(raw, (uint32_t)*ioSize, TRUE);
        debug_message("[RMF] ecmi: XDecompressPtr returned %s, size=%ld\n",
                   dec ? "non-NULL" : "NULL", dec ? (long)XGetPtrSize(dec) : 0L);
        XDisposePtr(raw);
        if (dec)
        {
            *ioSize = (int32_t)XGetPtrSize(dec);
            debug_message("[RMF] ecmi: first 4 decompressed bytes: %02x %02x %02x %02x\n",
                       ((unsigned char*)dec)[0], ((unsigned char*)dec)[1],
                       ((unsigned char*)dec)[2], ((unsigned char*)dec)[3]);
        }
        return dec;
    }
    if (rtype == ID_EMID)
    {
        XDecryptData(raw, (uint32_t)*ioSize);
        return raw;
    }
    if (rtype == ID_CMID)
    {
        dec = XDecompressPtr(raw, (uint32_t)*ioSize, TRUE);
        XDisposePtr(raw);
        if (dec)
        {
            *ioSize = (int32_t)XGetPtrSize(dec);
        }
        return dec;
    }
    return raw;
}


BAEResult PV_EncodeMidiForResourceType(XResourceType resourceType,
                                              ByteBuffer const *plainMidi,
                                              XPTR *outData,
                                              int32_t *outSize,
                                              bool isZmf)
{
    XPTR encoded;
    XCOMPRESSION_TYPE compType;

    if (!plainMidi || !outData || !outSize)
    {
        return BAE_PARAM_ERR;
    }
    *outData = NULL;
    *outSize = 0;

    if (resourceType == ID_MIDI || resourceType == ID_MIDI_OLD)
    {
        encoded = XNewPtr((int32_t)plainMidi->size);
        if (!encoded)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(plainMidi->data, encoded, (int32_t)plainMidi->size);
        *outData = encoded;
        *outSize = (int32_t)plainMidi->size;
        return BAE_NO_ERROR;
    }

    if (resourceType == ID_CMID || resourceType == ID_ECMI)
    {
        int32_t compressedSize;

        encoded = NULL;
#if USE_LZMA_COMPRESSION == TRUE
        compType = isZmf ? X_LZMA_RAW : X_RAW;
#else
        (void)isZmf;
        compType = X_RAW;
#endif
        if (resourceType == ID_ECMI)
        {
            compressedSize = XCompressAndEncryptWithType(&encoded,
                                                         (XPTR)plainMidi->data,
                                                         plainMidi->size,
                                                         compType,
                                                         NULL,
                                                         NULL);
        }
        else
        {
            compressedSize = XCompressPtr(&encoded,
                                          (XPTR)plainMidi->data,
                                          plainMidi->size,
                                          compType,
                                          NULL,
                                          NULL);
        }
        if (compressedSize <= 0 || !encoded)
        {
            if (encoded)
            {
                XDisposePtr(encoded);
            }
            return BAE_BAD_FILE;
        }
        if ((uint32_t)compressedSize >= plainMidi->size)
        {
            XDisposePtr(encoded);
            return BAE_BAD_FILE;
        }
        *outData = encoded;
        *outSize = compressedSize;
        return BAE_NO_ERROR;
    }

    if (resourceType == ID_EMID)
    {
        encoded = XNewPtr((int32_t)plainMidi->size);
        if (!encoded)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(plainMidi->data, encoded, (int32_t)plainMidi->size);
        XEncryptData(encoded, plainMidi->size);
        *outData = encoded;
        *outSize = (int32_t)plainMidi->size;
        return BAE_NO_ERROR;
    }

    return BAE_PARAM_ERR;
}


/* Like PV_EncodeMidiForResourceType(ID_ECMI, ...) but falls back to EMID
 * (encrypted, uncompressed) when compression fails on small or
 * incompressible MIDI.  Returns the resource type actually used. */
BAEResult PV_EncodeMidiBestEffort(ByteBuffer const *plainMidi,
                                         XPTR *outData,
                                         int32_t *outSize,
                                         XResourceType *outUsedType,
                                         bool isZmf)
{
    BAEResult result;

    if (!outUsedType)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_EncodeMidiForResourceType(ID_ECMI, plainMidi, outData, outSize, isZmf);
    if (result == BAE_NO_ERROR)
    {
        *outUsedType = ID_ECMI;
        return BAE_NO_ERROR;
    }
    /* ECMI failed (compression couldn't compress): fall back to EMID */
    result = PV_EncodeMidiForResourceType(ID_EMID, plainMidi, outData, outSize, isZmf);
    if (result == BAE_NO_ERROR)
    {
        debug_message("[RMF Save] ECMI compression failed; using EMID (encrypted uncompressed) fallback\n");
        *outUsedType = ID_EMID;
        return BAE_NO_ERROR;
    }
    return result;
}


BAERmfEditorMidiStorageType PV_NormalizeMidiStorageType(BAERmfEditorMidiStorageType storageType)
{
    if (storageType < BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT ||
        storageType > BAE_EDITOR_MIDI_STORAGE_MIDI)
    {
        return BAE_EDITOR_MIDI_STORAGE_ECMI;
    }
    return storageType;
}


BAEResult PV_EncodeMidiForStorageType(BAERmfEditorMidiStorageType storageType,
                                             ByteBuffer const *plainMidi,
                                             XPTR *outData,
                                             int32_t *outSize,
                                             XResourceType *outUsedType,
                                             bool isZmf)
{
    storageType = PV_NormalizeMidiStorageType(storageType);
    if (!outUsedType)
    {
        return BAE_PARAM_ERR;
    }

    switch (storageType)
    {
        case BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT:
        {
            BAEResult result;

            result = PV_EncodeMidiForResourceType(ID_CMID, plainMidi, outData, outSize, isZmf);
            if (result == BAE_NO_ERROR)
            {
                *outUsedType = ID_CMID;
                return BAE_NO_ERROR;
            }
            result = PV_EncodeMidiForResourceType(ID_MIDI, plainMidi, outData, outSize, isZmf);
            if (result == BAE_NO_ERROR)
            {
                debug_message("[RMF Save] CMID compression failed; using MIDI fallback\n");
                *outUsedType = ID_MIDI;
                return BAE_NO_ERROR;
            }
            return result;
        }

        case BAE_EDITOR_MIDI_STORAGE_ECMI:
            return PV_EncodeMidiBestEffort(plainMidi, outData, outSize, outUsedType, isZmf);

        case BAE_EDITOR_MIDI_STORAGE_EMID:
        {
            BAEResult result;

            result = PV_EncodeMidiForResourceType(ID_EMID, plainMidi, outData, outSize, isZmf);
            if (result == BAE_NO_ERROR)
            {
                *outUsedType = ID_EMID;
            }
            return result;
        }

        case BAE_EDITOR_MIDI_STORAGE_MIDI:
        {
            BAEResult result;

            result = PV_EncodeMidiForResourceType(ID_MIDI, plainMidi, outData, outSize, isZmf);
            if (result == BAE_NO_ERROR)
            {
                *outUsedType = ID_MIDI;
            }
            return result;
        }

        default:
            return BAE_PARAM_ERR;
    }
}


bool PV_IsMidiResourceType(XResourceType resourceType)
{
    return (resourceType == ID_ECMI ||
            resourceType == ID_EMID ||
            resourceType == ID_CMID ||
            resourceType == ID_MIDI ||
            resourceType == ID_MIDI_OLD) ? TRUE : FALSE;
}


int PV_CompareMidiEvents(void const *left, void const *right)
{
    MidiEventRecord const *a;
    MidiEventRecord const *b;

    a = (MidiEventRecord const *)left;
    b = (MidiEventRecord const *)right;
    if (a->tick < b->tick)
    {
        return -1;
    }
    if (a->tick > b->tick)
    {
        return 1;
    }
    if (a->sequence < b->sequence)
    {
        return -1;
    }
    if (a->sequence > b->sequence)
    {
        return 1;
    }
    return 0;
}


BAEResult PV_AppendMetaEvent(ByteBuffer *buffer, uint32_t delta, unsigned char type, void const *data, uint32_t length)
{
    BAEResult result;

    result = PV_ByteBufferAppendVLQ(buffer, delta);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(buffer, 0xFF);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendByte(buffer, type);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendVLQ(buffer, length);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    return PV_ByteBufferAppend(buffer, data, length);
}


BAEResult PV_CompactMidiRunningStatus(ByteBuffer *trackData)
{
    uint32_t readOffset;
    uint32_t writeOffset;
    unsigned char runningStatus;

    if (!trackData || (!trackData->data && trackData->size > 0))
    {
        return BAE_PARAM_ERR;
    }

    readOffset = 0;
    writeOffset = 0;
    runningStatus = 0;
    while (readOffset < trackData->size)
    {
        unsigned char status;
        uint32_t length;
        uint32_t lengthOffset;
        uint32_t dataBytes;

        do
        {
            unsigned char deltaByte;

            deltaByte = trackData->data[readOffset++];
            trackData->data[writeOffset++] = deltaByte;
            if (!(deltaByte & 0x80))
            {
                break;
            }
        } while (readOffset < trackData->size);
        if (readOffset >= trackData->size)
        {
            return BAE_BAD_FILE;
        }

        status = trackData->data[readOffset++];
        if (status >= 0x80 && status < 0xF0)
        {
            if (status != runningStatus)
            {
                trackData->data[writeOffset++] = status;
                runningStatus = status;
            }
            dataBytes = ((status & 0xF0) == PROGRAM_CHANGE ||
                         (status & 0xF0) == CHANNEL_AFTERTOUCH) ? 1 : 2;
            if (readOffset + dataBytes > trackData->size)
            {
                return BAE_BAD_FILE;
            }
            while (dataBytes-- > 0)
            {
                trackData->data[writeOffset++] = trackData->data[readOffset++];
            }
            continue;
        }

        trackData->data[writeOffset++] = status;
        runningStatus = 0;
        if (status == 0xFF)
        {
            if (readOffset >= trackData->size)
            {
                return BAE_BAD_FILE;
            }
            trackData->data[writeOffset++] = trackData->data[readOffset++];
        }
        else if (status != 0xF0 && status != 0xF7)
        {
            return BAE_BAD_FILE;
        }

        length = 0;
        lengthOffset = readOffset;
        do
        {
            unsigned char lengthByte;

            if (readOffset >= trackData->size)
            {
                return BAE_BAD_FILE;
            }
            lengthByte = trackData->data[readOffset++];
            length = (length << 7) | (uint32_t)(lengthByte & 0x7F);
            if (!(lengthByte & 0x80))
            {
                break;
            }
        } while (TRUE);
        if (readOffset + length > trackData->size)
        {
            return BAE_BAD_FILE;
        }
        while (lengthOffset < readOffset)
        {
            trackData->data[writeOffset++] = trackData->data[lengthOffset++];
        }
        while (length-- > 0)
        {
            trackData->data[writeOffset++] = trackData->data[readOffset++];
        }
    }
    trackData->size = writeOffset;
    return BAE_NO_ERROR;
}


BAEResult PV_BuildTempoTrack(BAERmfEditorDocument *document, ByteBuffer *trackData)
{
    uint32_t eventIndex;
    uint32_t previousTick;
    BAEResult result;

    previousTick = 0;
    if (document->tempoEventCount > 0)
    {
        for (eventIndex = 0; eventIndex < document->tempoEventCount; ++eventIndex)
        {
            unsigned char tempoBytes[3];
            BAERmfEditorTempoEvent const *tempoEvent;
            uint32_t delta;

            tempoEvent = &document->tempoEvents[eventIndex];
            if (tempoEvent->microsecondsPerQuarter == 0)
            {
                continue;
            }
            delta = tempoEvent->tick - previousTick;
            tempoBytes[0] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 16) & 0xFF);
            tempoBytes[1] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 8) & 0xFF);
            tempoBytes[2] = (unsigned char)(tempoEvent->microsecondsPerQuarter & 0xFF);
            result = PV_AppendMetaEvent(trackData, delta, 0x51, tempoBytes, 3);
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
            previousTick = tempoEvent->tick;
        }
    }
    else
    {
        unsigned char tempoBytes[3];
        uint32_t microsecondsPerQuarter;

        microsecondsPerQuarter = 60000000UL / document->tempoBPM;
        tempoBytes[0] = (unsigned char)((microsecondsPerQuarter >> 16) & 0xFF);
        tempoBytes[1] = (unsigned char)((microsecondsPerQuarter >> 8) & 0xFF);
        tempoBytes[2] = (unsigned char)(microsecondsPerQuarter & 0xFF);
        result = PV_AppendMetaEvent(trackData, 0, 0x51, tempoBytes, 3);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    return PV_AppendMetaEvent(trackData, 0, 0x2F, NULL, 0);
}


BAEResult PV_BuildConductorTrack(BAERmfEditorDocument *document,
                                        BAERmfEditorTrack const *track,
                                        ByteBuffer *trackData)
{
    uint32_t tempoIndex;
    uint32_t metaIndex;
    uint32_t previousTick;
    BAEResult result;

    if (!document || !track || !trackData)
    {
        return BAE_PARAM_ERR;
    }

    previousTick = 0;
    if (track->name && track->name[0] && !PV_TrackHasMetaType(track, 0x03))
    {
        result = PV_AppendMetaEvent(trackData, 0, 0x03, track->name, (uint32_t)strlen(track->name));
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }

    /* Skip past track-local tempo meta events. Tempo is regenerated from
     * document->tempoEvents so 0x51 entries in the track meta list are ignored. */
    metaIndex = 0;
    while (metaIndex < track->metaEventCount &&
           track->metaEvents[metaIndex].type == 0x51)
    {
        metaIndex++;
    }

    if (document->tempoEventCount > 0)
    {
        /* Preserve original conductor meta ordering and replace only 0x51 events
         * with document tempo data in-place. This keeps same-tick ordering stable
         * (e.g. META03/META54/META51/META58) while still honoring edited tempo. */
        tempoIndex = 0;
        for (; metaIndex < track->metaEventCount; ++metaIndex)
        {
            BAERmfEditorMetaEvent const *metaEvent;

            metaEvent = &track->metaEvents[metaIndex];

            while (tempoIndex < document->tempoEventCount &&
                   document->tempoEvents[tempoIndex].microsecondsPerQuarter == 0)
            {
                tempoIndex++;
            }

            while (tempoIndex < document->tempoEventCount &&
                   document->tempoEvents[tempoIndex].tick < metaEvent->tick)
            {
                unsigned char tempoBytes[3];
                BAERmfEditorTempoEvent const *tempoEvent;
                uint32_t delta;

                tempoEvent = &document->tempoEvents[tempoIndex];
                delta = (tempoEvent->tick >= previousTick) ? (tempoEvent->tick - previousTick) : 0;
                tempoBytes[0] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 16) & 0xFF);
                tempoBytes[1] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 8) & 0xFF);
                tempoBytes[2] = (unsigned char)(tempoEvent->microsecondsPerQuarter & 0xFF);
                result = PV_AppendMetaEvent(trackData, delta, 0x51, tempoBytes, 3);
                if (result != BAE_NO_ERROR)
                {
                    return result;
                }
                previousTick = tempoEvent->tick;
                tempoIndex++;
            }

            if (metaEvent->type == 0x51)
            {
                if (tempoIndex < document->tempoEventCount &&
                    document->tempoEvents[tempoIndex].tick == metaEvent->tick)
                {
                    unsigned char tempoBytes[3];
                    BAERmfEditorTempoEvent const *tempoEvent;
                    uint32_t delta;

                    tempoEvent = &document->tempoEvents[tempoIndex];
                    delta = (tempoEvent->tick >= previousTick) ? (tempoEvent->tick - previousTick) : 0;
                    tempoBytes[0] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 16) & 0xFF);
                    tempoBytes[1] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 8) & 0xFF);
                    tempoBytes[2] = (unsigned char)(tempoEvent->microsecondsPerQuarter & 0xFF);
                    result = PV_AppendMetaEvent(trackData, delta, 0x51, tempoBytes, 3);
                    if (result != BAE_NO_ERROR)
                    {
                        return result;
                    }
                    previousTick = tempoEvent->tick;
                    tempoIndex++;
                }
                continue;
            }

            {
                uint32_t delta;
                delta = (metaEvent->tick >= previousTick) ? (metaEvent->tick - previousTick) : 0;
                result = PV_AppendMetaEvent(trackData, delta, metaEvent->type, metaEvent->data, metaEvent->size);
                if (result != BAE_NO_ERROR)
                {
                    return result;
                }
                previousTick = metaEvent->tick;
            }
        }

        while (tempoIndex < document->tempoEventCount)
        {
            unsigned char tempoBytes[3];
            BAERmfEditorTempoEvent const *tempoEvent;
            uint32_t delta;

            while (tempoIndex < document->tempoEventCount &&
                   document->tempoEvents[tempoIndex].microsecondsPerQuarter == 0)
            {
                tempoIndex++;
            }
            if (tempoIndex >= document->tempoEventCount)
            {
                break;
            }

            tempoEvent = &document->tempoEvents[tempoIndex];
            delta = (tempoEvent->tick >= previousTick) ? (tempoEvent->tick - previousTick) : 0;
            tempoBytes[0] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 16) & 0xFF);
            tempoBytes[1] = (unsigned char)((tempoEvent->microsecondsPerQuarter >> 8) & 0xFF);
            tempoBytes[2] = (unsigned char)(tempoEvent->microsecondsPerQuarter & 0xFF);
            result = PV_AppendMetaEvent(trackData, delta, 0x51, tempoBytes, 3);
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
            previousTick = tempoEvent->tick;
            tempoIndex++;
        }
    }
    else
    {
        unsigned char tempoBytes[3];
        uint32_t microsecondsPerQuarter;

        microsecondsPerQuarter = 60000000UL / (document->tempoBPM ? document->tempoBPM : 120);
        tempoBytes[0] = (unsigned char)((microsecondsPerQuarter >> 16) & 0xFF);
        tempoBytes[1] = (unsigned char)((microsecondsPerQuarter >> 8) & 0xFF);
        tempoBytes[2] = (unsigned char)(microsecondsPerQuarter & 0xFF);
        result = PV_AppendMetaEvent(trackData, 0, 0x51, tempoBytes, 3);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        /* Output any remaining non-0x51 track meta events. */
        for (; metaIndex < track->metaEventCount; ++metaIndex)
        {
            BAERmfEditorMetaEvent const *metaEvent;
            uint32_t delta;

            metaEvent = &track->metaEvents[metaIndex];
            if (metaEvent->type == 0x51)
            {
                continue;
            }
            delta = (metaEvent->tick >= previousTick) ? (metaEvent->tick - previousTick) : 0;
            result = PV_AppendMetaEvent(trackData, delta, metaEvent->type, metaEvent->data, metaEvent->size);
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
            previousTick = metaEvent->tick;
        }
    }

    return PV_AppendMetaEvent(trackData, 0, 0x2F, NULL, 0);
}


bool PV_IsMetaOnlyConductorTrack(BAERmfEditorTrack const *track)
{
    if (!track)
    {
        return FALSE;
    }
    return (track->noteCount == 0 &&
            track->ccEventCount == 0 &&
            track->sysexEventCount == 0 &&
            track->auxEventCount == 0) ? TRUE : FALSE;
}


bool PV_TrackHasMetaType(BAERmfEditorTrack const *track, unsigned char type)
{
    uint32_t index;

    if (!track)
    {
        return FALSE;
    }
    for (index = 0; index < track->metaEventCount; ++index)
    {
        if (track->metaEvents[index].type == type)
        {
            return TRUE;
        }
    }
    return FALSE;
}


BAEResult PV_BuildTrackData(BAERmfEditorTrack const *track,
                                   ByteBuffer *trackData)
{
    MidiEventRecord *events;
    uint32_t eventCount;
    uint32_t eventIndex;
    uint32_t noteIndex;
    uint32_t previousTick = 0;
    uint16_t currentBank[BAE_MAX_MIDI_CHANNELS];
    unsigned char currentProgram[BAE_MAX_MIDI_CHANNELS];
    unsigned char explicitBankMsb[BAE_MAX_MIDI_CHANNELS];
    unsigned char explicitBankLsb[BAE_MAX_MIDI_CHANNELS];
    unsigned char explicitProgram[BAE_MAX_MIDI_CHANNELS];
    uint16_t initChannel;
    BAEResult result;

    events = NULL;
    eventCount = (track->noteCount * 2) + track->ccEventCount + track->sysexEventCount + track->auxEventCount + track->metaEventCount;
    if (track->name && track->name[0] && !PV_TrackHasMetaType(track, 0x03))
    {
        result = PV_AppendMetaEvent(trackData, 0, 0x03, track->name, (uint32_t)strlen(track->name));
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    for (initChannel = 0; initChannel < BAE_MAX_MIDI_CHANNELS; ++initChannel)
    {
        currentBank[initChannel] = 0;
        currentProgram[initChannel] = 0;
        explicitBankMsb[initChannel] = 0;
        explicitBankLsb[initChannel] = 0;
        explicitProgram[initChannel] = 0;
    }
    
    for (eventIndex = 0; eventIndex < track->auxEventCount; ++eventIndex)
    {
        BAERmfEditorAuxEvent const *aux;
        unsigned char channel;
        unsigned char eventType;

        aux = &track->auxEvents[eventIndex];
        channel = (unsigned char)(aux->status & 0x0F);
        eventType = (unsigned char)(aux->status & 0xF0);
        if (eventType == CONTROL_CHANGE && aux->dataBytes >= 2)
        {
            if (aux->data1 == BANK_MSB)
            {
                explicitBankMsb[channel] = 1;
            }
            else if (aux->data1 == BANK_LSB)
            {
                explicitBankLsb[channel] = 1;
            }
        }
        else if (eventType == PROGRAM_CHANGE && aux->dataBytes >= 1)
        {
            explicitProgram[channel] = 1;
        }
    }

    /* Emit the track's bank/program at tick 0 only when not already explicitly
     * present in aux events for the channel. Editor banks 0..127 are Beatnik
     * groups (CC0); values >= 128 are MIDI (MSB<<7)|LSB. */
    if (track->bank != 0 || track->program != 0)
    {
        unsigned char channel = track->channel & 0x0F;
        uint16_t bankMsb = 0;
        uint16_t bankLsb = 0;
        int emittedBankMsb = 0;
        int emittedBankLsb = 0;

        PV_BankToMidiMsbLsb(track->bank, &bankMsb, &bankLsb);

        /* Emit Bank MSB (CC 0). Channel 10 still needs non-zero MSB for Beatnik
         * kit banks (group*256+128+note). Only skip when MSB is already explicit
         * in aux, or is zero (standard GM drums). */
        if (!explicitBankMsb[channel] && bankMsb != 0)
        {
            result = PV_ByteBufferAppendVLQ(trackData, 0);
            if (result == BAE_NO_ERROR)
                result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | channel));
            if (result == BAE_NO_ERROR)
                result = PV_ByteBufferAppendByte(trackData, BANK_MSB);
            if (result == BAE_NO_ERROR)
                result = PV_ByteBufferAppendByte(trackData, (unsigned char)bankMsb);
            if (result != BAE_NO_ERROR)
                return result;
            emittedBankMsb = 1;
        }

        /* Emit Bank LSB (CC 32) only when non-zero and not explicitly authored. */
        if (!explicitBankLsb[channel] && bankLsb != 0)
        {
            result = PV_ByteBufferAppendVLQ(trackData, 0);
            if (result == BAE_NO_ERROR)
                result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | channel));
            if (result == BAE_NO_ERROR)
                result = PV_ByteBufferAppendByte(trackData, BANK_LSB);
            if (result == BAE_NO_ERROR)
                result = PV_ByteBufferAppendByte(trackData, (unsigned char)bankLsb);
            if (result != BAE_NO_ERROR)
                return result;
            emittedBankLsb = 1;
        }

        /* Emit Program Change */
        if (!explicitProgram[channel])
        {
            result = PV_ByteBufferAppendVLQ(trackData, 0);
            if (result == BAE_NO_ERROR)
                result = PV_ByteBufferAppendByte(trackData, (unsigned char)(PROGRAM_CHANGE | channel));
            if (result == BAE_NO_ERROR)
                result = PV_ByteBufferAppendByte(trackData, track->program);
            if (result != BAE_NO_ERROR)
                return result;
            currentProgram[channel] = track->program;
        }

        /* Only advance currentBank for parts we actually emitted (or that are
           already present as explicit aux). Never pretend a suppressed bank is
           current — that blocks per-note bank injection on CH10 kit tracks. */
        if (!explicitBankMsb[channel] && !explicitBankLsb[channel])
        {
            if (emittedBankMsb || emittedBankLsb)
            {
                currentBank[channel] = track->bank;
            }
        }
        else if (!explicitBankMsb[channel])
        {
            /* MSB emitted, LSB suppressed: track the MSB portion only */
            currentBank[channel] = (uint16_t)((track->bank & 0x3F80));
        }
        else if (!explicitBankLsb[channel])
        {
            /* MSB suppressed, LSB emitted: track the LSB portion only */
            currentBank[channel] = (uint16_t)((track->bank & 0x7F));
        }
        /* else: both suppressed - currentBank stays 0, aux events will set it */
    }

    if (eventCount)
    {
        events = (MidiEventRecord *)XNewPtr((int32_t)(sizeof(MidiEventRecord) * eventCount));
        if (!events)
        {
            return BAE_MEMORY_ERR;
        }
        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote const *note;
            MidiEventRecord *noteOn;
            MidiEventRecord *noteOff;
            unsigned char transposedNote;

            note = &track->notes[noteIndex];
            noteOn = &events[noteIndex * 2];
            noteOff = &events[(noteIndex * 2) + 1];
            transposedNote = PV_ClampMidi7Bit((int32_t)note->note + (int32_t)track->transpose);

            noteOn->tick = note->startTick;
            noteOn->sequence = note->noteOnOrder;
            noteOn->order = 2;
            noteOn->status = (unsigned char)(NOTE_ON | (note->channel & 0x0F));
            noteOn->data1 = transposedNote;
            noteOn->data2 = note->velocity;
            noteOn->dataBytes = 2;
            noteOn->blob = NULL;
            noteOn->blobSize = 0;
            noteOn->bank = note->bank;
            noteOn->program = note->program;
            /* Always allow per-note bank/program injection. Explicit aux events
             * still run first (order); injection only emits when the note's
             * bank/program differs from the channel's current state. */
            noteOn->applyProgram = 1;

            noteOff->tick = note->startTick + note->durationTicks;
            noteOff->sequence = note->noteOffOrder;
            noteOff->order = 0;
            /* Keep the original note-off type (NOTE_OFF vs NOTE_ON vel 0), but always
             * emit on the note's current channel. SetNoteInfo can move notes across
             * channels without rewriting noteOffStatus; using the stale status byte
             * leaves note-ons stranded (e.g. percussion remapped to ch10). */
            {
                unsigned char noteOffType = (unsigned char)(note->noteOffStatus & 0xF0);

                if (noteOffType != NOTE_ON && noteOffType != NOTE_OFF)
                {
                    noteOffType = NOTE_OFF;
                }
                noteOff->status = (unsigned char)(noteOffType | (note->channel & 0x0F));
            }
            noteOff->data1 = transposedNote;
            noteOff->data2 = note->noteOffVelocity;
            noteOff->dataBytes = 2;
            noteOff->blob = NULL;
            noteOff->blobSize = 0;
            noteOff->bank = note->bank;
            noteOff->program = note->program;
            noteOff->applyProgram = 0;
        }
        for (eventIndex = 0; eventIndex < track->ccEventCount; ++eventIndex)
        {
            MidiEventRecord *ccEvent;

            ccEvent = &events[(track->noteCount * 2) + eventIndex];
            ccEvent->tick = track->ccEvents[eventIndex].tick;
            ccEvent->sequence = track->ccEvents[eventIndex].eventOrder;
            ccEvent->order = 1;
            if (track->ccEvents[eventIndex].cc == BAE_EDITOR_CC_PITCH_BEND_SENTINEL)
            {
                /* Pitch bend: sentinel cc=0xFF, value=LSB, data2=MSB */
                ccEvent->status = (unsigned char)(PITCH_BEND | (track->channel & 0x0F));
                ccEvent->data1 = track->ccEvents[eventIndex].value;
                ccEvent->data2 = track->ccEvents[eventIndex].data2;
                ccEvent->dataBytes = 2;
            }
            else if (track->ccEvents[eventIndex].cc == BAE_EDITOR_CC_CHANNEL_AFTERTOUCH_SENTINEL)
            {
                ccEvent->status = (unsigned char)(CHANNEL_AFTERTOUCH | (track->channel & 0x0F));
                ccEvent->data1 = track->ccEvents[eventIndex].value;
                ccEvent->data2 = 0;
                ccEvent->dataBytes = 1;
            }
            else if (track->ccEvents[eventIndex].cc == BAE_EDITOR_CC_POLY_AFTERTOUCH_SENTINEL)
            {
                ccEvent->status = (unsigned char)(POLY_AFTERTOUCH | (track->channel & 0x0F));
                ccEvent->data1 = track->ccEvents[eventIndex].value;
                ccEvent->data2 = track->ccEvents[eventIndex].data2;
                ccEvent->dataBytes = 2;
            }
            else
            {
                ccEvent->status = (unsigned char)(CONTROL_CHANGE | (track->channel & 0x0F));
                ccEvent->data1 = track->ccEvents[eventIndex].cc;
                ccEvent->data2 = track->ccEvents[eventIndex].value;
                ccEvent->dataBytes = 2;
            }
            ccEvent->blob = NULL;
            ccEvent->blobSize = 0;
            ccEvent->bank = track->bank;
            ccEvent->program = track->program;
            ccEvent->applyProgram = 0;
        }
        for (eventIndex = 0; eventIndex < track->sysexEventCount; ++eventIndex)
        {
            MidiEventRecord *sysEvent;

            sysEvent = &events[(track->noteCount * 2) + track->ccEventCount + eventIndex];
            sysEvent->tick = track->sysexEvents[eventIndex].tick;
            sysEvent->sequence = track->sysexEvents[eventIndex].eventOrder;
            sysEvent->order = 1;
            sysEvent->status = track->sysexEvents[eventIndex].status;
            sysEvent->data1 = 0;
            sysEvent->data2 = 0;
            sysEvent->dataBytes = 0;
            sysEvent->blob = track->sysexEvents[eventIndex].data;
            sysEvent->blobSize = track->sysexEvents[eventIndex].size;
            sysEvent->bank = track->bank;
            sysEvent->program = track->program;
            sysEvent->applyProgram = 0;
        }
        for (eventIndex = 0; eventIndex < track->auxEventCount; ++eventIndex)
        {
            MidiEventRecord *auxEvent;

            auxEvent = &events[(track->noteCount * 2) + track->ccEventCount + track->sysexEventCount + eventIndex];
            auxEvent->tick = track->auxEvents[eventIndex].tick;
            auxEvent->sequence = track->auxEvents[eventIndex].eventOrder;
            auxEvent->order = 1;
            auxEvent->status = track->auxEvents[eventIndex].status;
            auxEvent->data1 = track->auxEvents[eventIndex].data1;
            auxEvent->data2 = track->auxEvents[eventIndex].data2;
            auxEvent->dataBytes = track->auxEvents[eventIndex].dataBytes;
            auxEvent->blob = NULL;
            auxEvent->blobSize = 0;
            auxEvent->bank = track->bank;
            auxEvent->program = track->program;
            auxEvent->applyProgram = 0;
        }
        for (eventIndex = 0; eventIndex < track->metaEventCount; ++eventIndex)
        {
            MidiEventRecord *metaEvent;

            metaEvent = &events[(track->noteCount * 2) + track->ccEventCount + track->sysexEventCount + track->auxEventCount + eventIndex];
            metaEvent->tick = track->metaEvents[eventIndex].tick;
            metaEvent->sequence = track->metaEvents[eventIndex].eventOrder;
            metaEvent->order = 1;
            metaEvent->status = 0xFF;
            metaEvent->data1 = track->metaEvents[eventIndex].type;
            metaEvent->data2 = 0;
            metaEvent->dataBytes = 0;
            metaEvent->blob = track->metaEvents[eventIndex].data;
            metaEvent->blobSize = track->metaEvents[eventIndex].size;
            metaEvent->bank = track->bank;
            metaEvent->program = track->program;
            metaEvent->applyProgram = 0;
        }
        qsort(events, eventCount, sizeof(MidiEventRecord), PV_CompareMidiEvents);
        previousTick = 0;
        for (eventIndex = 0; eventIndex < eventCount; ++eventIndex)
        {
            MidiEventRecord const *event;
            uint32_t delta;

            event = &events[eventIndex];
            if (track->endOfTrackTick > 0 && event->tick > track->endOfTrackTick)
            {
                break;
            }
            delta = event->tick - previousTick;
            if (event->applyProgram)
            {
                unsigned char eventChannel;
                int bankChanged;

                eventChannel = (unsigned char)(event->status & 0x0F);
                bankChanged = !PV_BanksEquivalent(event->bank, currentBank[eventChannel]);
                if (bankChanged)
                {
                    uint16_t bankMsb;
                    uint16_t bankLsb;
                    uint16_t prevMsb;
                    uint16_t prevLsb;

                    PV_BankToMidiMsbLsb(event->bank, &bankMsb, &bankLsb);
                    PV_BankToMidiMsbLsb(currentBank[eventChannel], &prevMsb, &prevLsb);

                    result = PV_ByteBufferAppendVLQ(trackData, delta);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | eventChannel));
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, BANK_MSB);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, (unsigned char)bankMsb);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    /* CC0 consumed the delta; all subsequent injected events at the
                       same tick (CC32, PC) must use delta=0. */
                    delta = 0;
                    if (bankLsb != 0 || prevLsb != 0)
                    {
                        result = PV_ByteBufferAppendVLQ(trackData, 0);
                        if (result != BAE_NO_ERROR)
                        {
                            XDisposePtr(events);
                            return result;
                        }
                        result = PV_ByteBufferAppendByte(trackData, (unsigned char)(CONTROL_CHANGE | eventChannel));
                        if (result != BAE_NO_ERROR)
                        {
                            XDisposePtr(events);
                            return result;
                        }
                        result = PV_ByteBufferAppendByte(trackData, BANK_LSB);
                        if (result != BAE_NO_ERROR)
                        {
                            XDisposePtr(events);
                            return result;
                        }
                        result = PV_ByteBufferAppendByte(trackData, (unsigned char)bankLsb);
                        if (result != BAE_NO_ERROR)
                        {
                            XDisposePtr(events);
                            return result;
                        }
                    }
                    currentBank[eventChannel] = event->bank;
                }
                if (event->program != currentProgram[eventChannel] || bankChanged)
                {
                    result = PV_ByteBufferAppendVLQ(trackData, delta);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, (unsigned char)(PROGRAM_CHANGE | eventChannel));
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    result = PV_ByteBufferAppendByte(trackData, event->program);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                    delta = 0;
                    currentProgram[eventChannel] = event->program;
                }
            }
            result = PV_ByteBufferAppendVLQ(trackData, delta);
            if (result != BAE_NO_ERROR)
            {
                XDisposePtr(events);
                return result;
            }
            result = PV_ByteBufferAppendByte(trackData, event->status);
            if (result != BAE_NO_ERROR)
            {
                XDisposePtr(events);
                return result;
            }
            if (event->status == 0xFF)
            {
                result = PV_ByteBufferAppendByte(trackData, event->data1);
                if (result != BAE_NO_ERROR)
                {
                    XDisposePtr(events);
                    return result;
                }
                result = PV_ByteBufferAppendVLQ(trackData, event->blobSize);
                if (result != BAE_NO_ERROR)
                {
                    XDisposePtr(events);
                    return result;
                }
                if (event->blobSize > 0)
                {
                    result = PV_ByteBufferAppend(trackData, event->blob, event->blobSize);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                }
            }
            else if (event->status == 0xF0 || event->status == 0xF7)
            {
                result = PV_ByteBufferAppendVLQ(trackData, event->blobSize);
                if (result != BAE_NO_ERROR)
                {
                    XDisposePtr(events);
                    return result;
                }
                if (event->blobSize > 0)
                {
                    result = PV_ByteBufferAppend(trackData, event->blob, event->blobSize);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                }
            }
            else
            {
                result = PV_ByteBufferAppendByte(trackData, event->data1);
                if (result != BAE_NO_ERROR)
                {
                    XDisposePtr(events);
                    return result;
                }
                if (event->dataBytes > 1)
                {
                    result = PV_ByteBufferAppendByte(trackData, event->data2);
                    if (result != BAE_NO_ERROR)
                    {
                        XDisposePtr(events);
                        return result;
                    }
                }
            }
            previousTick = event->tick;
                    /* Track channel bank/program state so per-note applyProgram comparisons
                       remain accurate after mid-track aux program change or bank select events.
                       Skip meta (0xFF) and sysex (0xF0/0xF7) messages. */
                    if (event->status != 0xFF && event->status != 0xF0 && event->status != 0xF7 &&
                        !event->applyProgram)
                    {
                        unsigned char evType = (unsigned char)(event->status & 0xF0);
                        unsigned char evCh  = (unsigned char)(event->status & 0x0F);
                        if (evType == PROGRAM_CHANGE && event->dataBytes >= 1)
                        {
                            currentProgram[evCh] = event->data1;
                        }
                        else if (evType == CONTROL_CHANGE && event->dataBytes >= 2)
                        {
                            if (event->data1 == BANK_MSB)
                            {
                                currentBank[evCh] = (uint16_t)((((uint16_t)event->data2) << 7) | (currentBank[evCh] & 0x7F));
                            }
                            else if (event->data1 == BANK_LSB)
                            {
                                currentBank[evCh] = (uint16_t)((currentBank[evCh] & 0x3F80) | (uint16_t)(event->data2 & 0x7F));
                            }
                        }
                    }
        }
        XDisposePtr(events);
    }

    /* Place end-of-track at the original tick if it was later than the last event. */
    {
        uint32_t eotDelta = 0;
        if (track->endOfTrackTick > 0 && previousTick > track->endOfTrackTick)
        {
            previousTick = track->endOfTrackTick;
        }
        if (track->endOfTrackTick > previousTick)
        {
            eotDelta = track->endOfTrackTick - previousTick;
        }
        return PV_AppendMetaEvent(trackData, eotDelta, 0x2F, NULL, 0);
    }
}


BAEResult PV_BuildMidiFile(BAERmfEditorDocument *document, ByteBuffer *output)
{
    ByteBuffer tempoTrack;
    ByteBuffer trackData;
    BAEResult result;
    uint32_t trackIndex;
    uint16_t trackCount;
    bool useTrack0AsConductor;
    bool hasTempoMetaInTracks;
    bool addTempoTrack;

    XSetMemory(&tempoTrack, sizeof(tempoTrack), 0);
    XSetMemory(&trackData, sizeof(trackData), 0);
    useTrack0AsConductor = (document->trackCount > 0 && PV_IsMetaOnlyConductorTrack(&document->tracks[0])) ? TRUE : FALSE;
    hasTempoMetaInTracks = FALSE;
    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        if (PV_TrackHasMetaType(&document->tracks[trackIndex], 0x51))
        {
            hasTempoMetaInTracks = TRUE;
            break;
        }
    }
    addTempoTrack = (!useTrack0AsConductor && !hasTempoMetaInTracks &&
                     !(document->debugOriginalMidiData && document->tempoEventCount == 0)) ? TRUE : FALSE;
    result = PV_ByteBufferAppend(output, "MThd", 4);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendBE32(output, 6);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendBE16(output, 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    trackCount = (uint16_t)(document->trackCount + (addTempoTrack ? 1 : 0));
    result = PV_ByteBufferAppendBE16(output, trackCount);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_ByteBufferAppendBE16(output, document->ticksPerQuarter);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    if (addTempoTrack)
    {
        result = PV_BuildTempoTrack(document, &tempoTrack);
        if (result != BAE_NO_ERROR)
        {
            PV_ByteBufferDispose(&tempoTrack);
            return result;
        }
        result = PV_ByteBufferAppend(output, "MTrk", 4);
        if (result == BAE_NO_ERROR)
        {
            result = PV_ByteBufferAppendBE32(output, tempoTrack.size);
        }
        if (result == BAE_NO_ERROR)
        {
            result = PV_ByteBufferAppend(output, tempoTrack.data, tempoTrack.size);
        }
        PV_ByteBufferDispose(&tempoTrack);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        XSetMemory(&trackData, sizeof(trackData), 0);
        if (useTrack0AsConductor && trackIndex == 0)
        {
            result = PV_BuildConductorTrack(document, &document->tracks[trackIndex], &trackData);
        }
        else
        {
            result = PV_BuildTrackData(&document->tracks[trackIndex], &trackData);
        }
        if (result != BAE_NO_ERROR)
        {
            PV_ByteBufferDispose(&trackData);
            return result;
        }
        result = PV_CompactMidiRunningStatus(&trackData);
        if (result != BAE_NO_ERROR)
        {
            PV_ByteBufferDispose(&trackData);
            return result;
        }
        result = PV_ByteBufferAppend(output, "MTrk", 4);
        if (result == BAE_NO_ERROR)
        {
            result = PV_ByteBufferAppendBE32(output, trackData.size);
        }
        if (result == BAE_NO_ERROR)
        {
            result = PV_ByteBufferAppend(output, trackData.data, trackData.size);
        }
        PV_ByteBufferDispose(&trackData);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    return BAE_NO_ERROR;
}


BAEResult PV_BuildMidiWithAppendedLoopTrack(BAERmfEditorDocument const *document,
                                                    ByteBuffer *output)
{
    ByteBuffer trackData;
    BAEResult result;
    bool enabled;
    uint32_t startTick;
    uint32_t endTick;
    int32_t loopCount;
    uint16_t trackCount;
    char loopStartText[32];
    char const *startText;

    if (!document || !output || !document->debugOriginalMidiData ||
        document->debugOriginalMidiDataSize < 14)
    {
        return BAE_PARAM_ERR;
    }

    enabled = FALSE;
    startTick = 0;
    endTick = 0;
    loopCount = -1;
    result = BAERmfEditorDocument_GetMidiLoopMarkers(document,
                                                      &enabled,
                                                      &startTick,
                                                      &endTick,
                                                      &loopCount);
    if (result != BAE_NO_ERROR || !enabled)
    {
        return (result != BAE_NO_ERROR) ? result : BAE_PARAM_ERR;
    }

    trackCount = PV_ReadBE16(document->debugOriginalMidiData + 10);
    if (trackCount == 0xFFFF)
    {
        return BAE_PARAM_ERR;
    }

    XSetMemory(&trackData, sizeof(trackData), 0);
    startText = "loopstart";
    if (loopCount > 0)
    {
        if (loopCount > 99)
        {
            loopCount = 99;
        }
        XSetMemory(loopStartText, (int32_t)sizeof(loopStartText), 0);
        sprintf(loopStartText, "loopstart=%ld", (long)loopCount);
        startText = loopStartText;
    }
    result = PV_AppendMetaEvent(&trackData,
                                startTick,
                                0x06,
                                startText,
                                (uint32_t)strlen(startText));
    if (result == BAE_NO_ERROR)
    {
        result = PV_AppendMetaEvent(&trackData,
                                    endTick - startTick,
                                    0x06,
                                    "loopend",
                                    7);
    }
    if (result == BAE_NO_ERROR)
    {
        result = PV_AppendMetaEvent(&trackData, 0, 0x2F, NULL, 0);
    }
    if (result == BAE_NO_ERROR)
    {
        result = PV_ByteBufferAppend(output,
                                     document->debugOriginalMidiData,
                                     document->debugOriginalMidiDataSize);
    }
    if (result == BAE_NO_ERROR)
    {
        output->data[10] = (unsigned char)((trackCount + 1) >> 8);
        output->data[11] = (unsigned char)((trackCount + 1) & 0xFF);
        result = PV_ByteBufferAppend(output, "MTrk", 4);
    }
    if (result == BAE_NO_ERROR)
    {
        result = PV_ByteBufferAppendBE32(output, trackData.size);
    }
    if (result == BAE_NO_ERROR)
    {
        result = PV_ByteBufferAppend(output, trackData.data, trackData.size);
    }
    PV_ByteBufferDispose(&trackData);
    return result;
}


BAEResult BAERmfEditorDocument_SetTempoBPM(BAERmfEditorDocument *document, uint32_t bpm)
{
    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (bpm == 0 || bpm > 960)
    {
        return BAE_PARAM_ERR;
    }
    /* Explicit tempo edit means use a fixed tempo unless a new MIDI map is loaded/copied. */
    PV_ClearTempoEvents(document);
    document->tempoBPM = bpm;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetMidiLoopMarkers(BAERmfEditorDocument const *document,
                                                  bool *outEnabled,
                                                  uint32_t *outStartTick,
                                                  uint32_t *outEndTick,
                                                  int32_t *outLoopCount)
{
    bool hasStart;
    bool hasEnd;
    uint32_t startTick;
    uint32_t endTick;
    int32_t loopCount;
    uint16_t trackIndex;

    if (!document || !outEnabled || !outStartTick || !outEndTick || !outLoopCount)
    {
        return BAE_PARAM_ERR;
    }

    hasStart = FALSE;
    hasEnd = FALSE;
    startTick = 0;
    endTick = 0;
    loopCount = -1;

    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack const *track;
        uint32_t metaIndex;

        track = &document->tracks[trackIndex];
        for (metaIndex = 0; metaIndex < track->metaEventCount; ++metaIndex)
        {
            BAERmfEditorMetaEvent const *event;
            int32_t markerLoopCount;

            event = &track->metaEvents[metaIndex];
            if (event->type != 0x06)
            {
                continue;
            }

            markerLoopCount = -1;
            if (PV_IsLoopStartMarkerText(event->data, event->size, &markerLoopCount))
            {
                if (!hasStart || event->tick < startTick)
                {
                    startTick = event->tick;
                    hasStart = TRUE;
                    if (markerLoopCount > 0)
                    {
                        loopCount = markerLoopCount;
                    }
                }
            }
            else if (PV_IsLoopEndMarkerText(event->data, event->size))
            {
                if (!hasEnd || event->tick > endTick)
                {
                    endTick = event->tick;
                    hasEnd = TRUE;
                }
            }
        }
    }

    *outEnabled = (hasStart && hasEnd && endTick > startTick) ? TRUE : FALSE;
    *outStartTick = startTick;
    *outEndTick = endTick;
    *outLoopCount = loopCount;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetMidiLoopMarkers(BAERmfEditorDocument *document,
                                                  bool enabled,
                                                  uint32_t startTick,
                                                  uint32_t endTick,
                                                  int32_t loopCount)
{
    uint16_t trackIndex;
    BAEResult result;
    BAERmfEditorTrack *track;
    bool canPreserveOriginalMidi;
    char loopStartText[32];
    char const *startText;
    char const *endText;

    if (!document)
    {
        return BAE_PARAM_ERR;
    }
    if (enabled && endTick <= startTick)
    {
        return BAE_PARAM_ERR;
    }

    canPreserveOriginalMidi = document->loopMarkersOnlyDirty;
    if (!canPreserveOriginalMidi && enabled && document->isPristine &&
        document->debugOriginalMidiData && document->debugOriginalMidiDataSize >= 14 &&
        PV_ReadBE16(document->debugOriginalMidiData + 8) == 1)
    {
        bool hasOriginalLoopMarker;

        hasOriginalLoopMarker = FALSE;
        for (trackIndex = 0; trackIndex < document->trackCount && !hasOriginalLoopMarker; ++trackIndex)
        {
            uint32_t metaIndex;

            for (metaIndex = 0; metaIndex < document->tracks[trackIndex].metaEventCount; ++metaIndex)
            {
                BAERmfEditorMetaEvent const *event;

                event = &document->tracks[trackIndex].metaEvents[metaIndex];
                if (event->type == 0x06 &&
                    (PV_IsLoopStartMarkerText(event->data, event->size, NULL) ||
                     PV_IsLoopEndMarkerText(event->data, event->size)))
                {
                    hasOriginalLoopMarker = TRUE;
                    break;
                }
            }
        }
        canPreserveOriginalMidi = hasOriginalLoopMarker ? FALSE : TRUE;
    }

    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        PV_RemoveLoopMarkersFromTrack(&document->tracks[trackIndex]);
    }

    if (!enabled)
    {
        PV_MarkDocumentDirty(document);
        return BAE_NO_ERROR;
    }

    if (document->trackCount == 0)
    {
        BAERmfEditorTrackSetup setup;

        XSetMemory(&setup, sizeof(setup), 0);
        result = BAERmfEditorDocument_AddTrack(document, &setup, &trackIndex);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }

    track = &document->tracks[0];
    startText = "loopstart";
    if (loopCount > 0)
    {
        if (loopCount > 99)
        {
            loopCount = 99;
        }
        XSetMemory(loopStartText, (int32_t)sizeof(loopStartText), 0);
        sprintf(loopStartText, "loopstart=%ld", (long)loopCount);
        startText = loopStartText;
    }
    endText = "loopend";

    result = PV_AddMetaEventToTrack(track,
                                    startTick,
                                    0x06,
                                    (unsigned char const *)startText,
                                    (uint32_t)strlen(startText));
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    result = PV_AddMetaEventToTrack(track,
                                    endTick,
                                    0x06,
                                    (unsigned char const *)endText,
                                    (uint32_t)strlen(endText));
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    /* Loop marker meta events must be <= track end tick for normal track
     * serialization path; otherwise they are skipped during write. */
    if (track->endOfTrackTick < endTick)
    {
        track->endOfTrackTick = endTick;
    }

    if (canPreserveOriginalMidi)
    {
        document->isPristine = FALSE;
        document->loopMarkersOnlyDirty = TRUE;
    }
    else
    {
        PV_MarkDocumentDirty(document);
    }
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_CopyTempoMapFrom(BAERmfEditorDocument *dest,
                                                BAERmfEditorDocument const *src)
{
    BAEResult result;
    uint32_t eventIndex;

    if (!dest || !src)
    {
        return BAE_PARAM_ERR;
    }

    PV_ClearTempoEvents(dest);
    dest->tempoBPM = src->tempoBPM;

    if (src->tempoEventCount == 0)
    {
        return BAE_NO_ERROR;
    }

    result = PV_GrowBuffer((void **)&dest->tempoEvents,
                           &dest->tempoEventCapacity,
                           sizeof(BAERmfEditorTempoEvent),
                           src->tempoEventCount);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    for (eventIndex = 0; eventIndex < src->tempoEventCount; ++eventIndex)
    {
        dest->tempoEvents[eventIndex] = src->tempoEvents[eventIndex];
    }
    dest->tempoEventCount = src->tempoEventCount;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetTempoEventCount(BAERmfEditorDocument const *document,
                                                  uint32_t *outCount)
{
    if (!document || !outCount)
    {
        return BAE_PARAM_ERR;
    }
    *outCount = document->tempoEventCount;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetTempoEvent(BAERmfEditorDocument const *document,
                                             uint32_t eventIndex,
                                             uint32_t *outTick,
                                             uint32_t *outMicrosecondsPerQuarter)
{
    if (!document || !outTick || !outMicrosecondsPerQuarter)
    {
        return BAE_PARAM_ERR;
    }
    if (eventIndex >= document->tempoEventCount)
    {
        return BAE_PARAM_ERR;
    }
    *outTick = document->tempoEvents[eventIndex].tick;
    *outMicrosecondsPerQuarter = document->tempoEvents[eventIndex].microsecondsPerQuarter;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_AddTempoEvent(BAERmfEditorDocument *document,
                                             uint32_t tick,
                                             uint32_t microsecondsPerQuarter)
{
    BAEResult result;

    if (!document || microsecondsPerQuarter == 0)
    {
        return BAE_PARAM_ERR;
    }
    if (document->tempoEventCount == 0 && tick > 0)
    {
        uint32_t baseBpm;

        baseBpm = document->tempoBPM ? document->tempoBPM : 120;
        result = PV_AddTempoEvent(document, 0, 60000000UL / baseBpm);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    result = PV_AddTempoEvent(document, tick, microsecondsPerQuarter);
    if (result == BAE_NO_ERROR && tick == 0)
    {
        document->tempoBPM = 60000000UL / microsecondsPerQuarter;
    }
    if (result == BAE_NO_ERROR)
    {
        PV_MarkDocumentDirty(document);
    }
    return result;
}


BAEResult BAERmfEditorDocument_SetTempoEvent(BAERmfEditorDocument *document,
                                             uint32_t eventIndex,
                                             uint32_t tick,
                                             uint32_t microsecondsPerQuarter)
{
    BAEResult result;

    if (!document || microsecondsPerQuarter == 0 || eventIndex >= document->tempoEventCount)
    {
        return BAE_PARAM_ERR;
    }
    document->tempoEvents[eventIndex].tick = tick;
    document->tempoEvents[eventIndex].microsecondsPerQuarter = microsecondsPerQuarter;
    while (eventIndex > 0 && document->tempoEvents[eventIndex - 1].tick > document->tempoEvents[eventIndex].tick)
    {
        BAERmfEditorTempoEvent tempEvent;

        tempEvent = document->tempoEvents[eventIndex - 1];
        document->tempoEvents[eventIndex - 1] = document->tempoEvents[eventIndex];
        document->tempoEvents[eventIndex] = tempEvent;
        eventIndex--;
    }
    while ((eventIndex + 1) < document->tempoEventCount &&
           document->tempoEvents[eventIndex + 1].tick < document->tempoEvents[eventIndex].tick)
    {
        BAERmfEditorTempoEvent tempEvent;

        tempEvent = document->tempoEvents[eventIndex + 1];
        document->tempoEvents[eventIndex + 1] = document->tempoEvents[eventIndex];
        document->tempoEvents[eventIndex] = tempEvent;
        eventIndex++;
    }
    if (document->tempoEventCount > 0 && document->tempoEvents[0].tick == 0 && document->tempoEvents[0].microsecondsPerQuarter > 0)
    {
        document->tempoBPM = 60000000UL / document->tempoEvents[0].microsecondsPerQuarter;
    }
    result = BAE_NO_ERROR;
    PV_MarkDocumentDirty(document);
    return result;
}


BAEResult BAERmfEditorDocument_DeleteTempoEvent(BAERmfEditorDocument *document,
                                                uint32_t eventIndex)
{
    if (!document || eventIndex >= document->tempoEventCount)
    {
        return BAE_PARAM_ERR;
    }
    if (eventIndex + 1 < document->tempoEventCount)
    {
        XBlockMove(&document->tempoEvents[eventIndex + 1],
                   &document->tempoEvents[eventIndex],
                   (int32_t)((document->tempoEventCount - (eventIndex + 1)) * sizeof(BAERmfEditorTempoEvent)));
    }
    document->tempoEventCount--;
    if (document->tempoEventCount > 0 && document->tempoEvents[0].tick == 0 && document->tempoEvents[0].microsecondsPerQuarter > 0)
    {
        document->tempoBPM = 60000000UL / document->tempoEvents[0].microsecondsPerQuarter;
    }
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetTrackCCEventCount(BAERmfEditorDocument const *document,
                                                    uint16_t trackIndex,
                                                    unsigned char cc,
                                                    uint32_t *outCount)
{
    BAERmfEditorTrack const *track;
    uint32_t index;
    uint32_t count;

    if (!document || !outCount)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    count = 0;
    for (index = 0; index < track->ccEventCount; ++index)
    {
        if (track->ccEvents[index].cc == cc)
        {
            count++;
        }
    }
    *outCount = count;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetTrackCCEvent(BAERmfEditorDocument const *document,
                                               uint16_t trackIndex,
                                               unsigned char cc,
                                               uint32_t eventIndex,
                                               uint32_t *outTick,
                                               unsigned char *outValue)
{
    BAERmfEditorTrack const *track;
    BAERmfEditorCCEvent const *event;

    if (!document || !outTick || !outValue)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    event = PV_FindTrackCCEventConst(track, cc, eventIndex, NULL);
    if (!event)
    {
        return BAE_PARAM_ERR;
    }
    *outTick = event->tick;
    *outValue = event->value;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_AddTrackCCEvent(BAERmfEditorDocument *document,
                                               uint16_t trackIndex,
                                               unsigned char cc,
                                               uint32_t tick,
                                               unsigned char value)
{
    BAERmfEditorTrack *track;
    BAEResult result;

    if (!document || value > 127)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_AddCCEventToTrack(track, tick, cc, value, 0);
    if (result == BAE_NO_ERROR)
    {
        if (cc == 7 && tick == 0)
        {
            track->volume = value;
        }
        if (cc == 10 && tick == 0)
        {
            track->pan = value;
        }
        PV_MarkDocumentDirty(document);
    }
    return result;
}


BAEResult BAERmfEditorDocument_SetTrackCCEvent(BAERmfEditorDocument *document,
                                               uint16_t trackIndex,
                                               unsigned char cc,
                                               uint32_t eventIndex,
                                               uint32_t tick,
                                               unsigned char value)
{
    BAERmfEditorTrack *track;
    BAERmfEditorCCEvent *event;

    if (!document || value > 127)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    event = PV_FindTrackCCEvent(track, cc, eventIndex, NULL);
    if (!event)
    {
        return BAE_PARAM_ERR;
    }
    event->tick = tick;
    event->value = value;
    qsort(track->ccEvents, track->ccEventCount, sizeof(BAERmfEditorCCEvent), PV_CompareCCEvents);
    if (cc == 7 && tick == 0)
    {
        track->volume = value;
    }
    if (cc == 10 && tick == 0)
    {
        track->pan = value;
    }
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_DeleteTrackCCEvent(BAERmfEditorDocument *document,
                                                  uint16_t trackIndex,
                                                  unsigned char cc,
                                                  uint32_t eventIndex)
{
    BAERmfEditorTrack *track;
    uint32_t actualIndex;

    if (!document)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    if (!PV_FindTrackCCEvent(track, cc, eventIndex, &actualIndex))
    {
        return BAE_PARAM_ERR;
    }
    if (actualIndex + 1 < track->ccEventCount)
    {
        XBlockMove(&track->ccEvents[actualIndex + 1],
                   &track->ccEvents[actualIndex],
                   (int32_t)((track->ccEventCount - (actualIndex + 1)) * sizeof(BAERmfEditorCCEvent)));
    }
    track->ccEventCount--;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetTrackPitchBendEventCount(BAERmfEditorDocument const *document,
                                                           uint16_t trackIndex,
                                                           uint32_t *outCount)
{
    return BAERmfEditorDocument_GetTrackCCEventCount(document,
                                                     trackIndex,
                                                     BAE_EDITOR_CC_PITCH_BEND_SENTINEL,
                                                     outCount);
}


BAEResult BAERmfEditorDocument_GetTrackPitchBendEvent(BAERmfEditorDocument const *document,
                                                      uint16_t trackIndex,
                                                      uint32_t eventIndex,
                                                      uint32_t *outTick,
                                                      uint16_t *outValue)
{
    BAERmfEditorTrack const *track;
    BAERmfEditorCCEvent const *event;

    if (!document || !outTick || !outValue)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    event = PV_FindTrackCCEventConst(track,
                                     BAE_EDITOR_CC_PITCH_BEND_SENTINEL,
                                     eventIndex,
                                     NULL);
    if (!event)
    {
        return BAE_PARAM_ERR;
    }
    *outTick = event->tick;
    *outValue = (uint16_t)(((uint16_t)(event->data2 & 0x7F) << 7) | (uint16_t)(event->value & 0x7F));
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_AddTrackPitchBendEvent(BAERmfEditorDocument *document,
                                                      uint16_t trackIndex,
                                                      uint32_t tick,
                                                      uint16_t value)
{
    BAERmfEditorTrack *track;
    BAEResult result;

    if (!document || value > 16383)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_AddCCEventToTrack(track,
                                  tick,
                                  BAE_EDITOR_CC_PITCH_BEND_SENTINEL,
                                  (unsigned char)(value & 0x7F),
                                  (unsigned char)((value >> 7) & 0x7F));
    if (result == BAE_NO_ERROR)
    {
        PV_MarkDocumentDirty(document);
    }
    return result;
}


BAEResult BAERmfEditorDocument_AddTrackProgramChange(BAERmfEditorDocument *document,
                                                     uint16_t trackIndex,
                                                     uint32_t tick,
                                                     unsigned char program)
{
    BAERmfEditorTrack *track;
    BAEResult result;

    if (!document || program > 127)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_AddAuxEventToTrack(track,
                                   tick,
                                   (unsigned char)(PROGRAM_CHANGE | (track->channel & 0x0F)),
                                   program,
                                   0,
                                   1);
    if (result == BAE_NO_ERROR)
    {
        PV_MarkDocumentDirty(document);
    }
    return result;
}


BAEResult BAERmfEditorDocument_SetTrackPitchBendEvent(BAERmfEditorDocument *document,
                                                      uint16_t trackIndex,
                                                      uint32_t eventIndex,
                                                      uint32_t tick,
                                                      uint16_t value)
{
    BAERmfEditorTrack *track;
    BAERmfEditorCCEvent *event;

    if (!document || value > 16383)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    event = PV_FindTrackCCEvent(track,
                                BAE_EDITOR_CC_PITCH_BEND_SENTINEL,
                                eventIndex,
                                NULL);
    if (!event)
    {
        return BAE_PARAM_ERR;
    }
    event->tick = tick;
    event->value = (unsigned char)(value & 0x7F);
    event->data2 = (unsigned char)((value >> 7) & 0x7F);
    qsort(track->ccEvents, track->ccEventCount, sizeof(BAERmfEditorCCEvent), PV_CompareCCEvents);
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_DeleteTrackPitchBendEvent(BAERmfEditorDocument *document,
                                                         uint16_t trackIndex,
                                                         uint32_t eventIndex)
{
    return BAERmfEditorDocument_DeleteTrackCCEvent(document,
                                                   trackIndex,
                                                   BAE_EDITOR_CC_PITCH_BEND_SENTINEL,
                                                   eventIndex);
}


BAEResult BAERmfEditorDocument_GetTempoBPM(BAERmfEditorDocument const *document, uint32_t *outBpm)
{
    if (!document || !outBpm)
    {
        return BAE_PARAM_ERR;
    }
    *outBpm = document->tempoBPM;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetTicksPerQuarter(BAERmfEditorDocument *document, uint16_t ticksPerQuarter)
{
    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (ticksPerQuarter == 0)
    {
        return BAE_PARAM_ERR;
    }
    document->ticksPerQuarter = ticksPerQuarter;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetTicksPerQuarter(BAERmfEditorDocument const *document, uint16_t *outTicksPerQuarter)
{
    if (!document || !outTicksPerQuarter)
    {
        return BAE_PARAM_ERR;
    }
    *outTicksPerQuarter = document->ticksPerQuarter;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetInfo(BAERmfEditorDocument *document, BAEInfoType infoType, char const *value)
{
    BAEResult result;

    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (infoType < 0 || infoType >= INFO_TYPE_COUNT)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_SetDocumentString(&document->info[infoType], value);
    if (result == BAE_NO_ERROR)
    {
        PV_MarkDocumentDirty(document);
    }
    return result;
}


char const *BAERmfEditorDocument_GetInfo(BAERmfEditorDocument const *document, BAEInfoType infoType)
{
    if (!document || infoType < 0 || infoType >= INFO_TYPE_COUNT)
    {
        return NULL;
    }
    return document->info[infoType];
}


BAEResult BAERmfEditorDocument_SetEngineConfig(BAERmfEditorDocument *document, int32_t flags)
{
    if (!document)
        return BAE_PARAM_ERR;
    document->engineConfigFlags = flags;
    document->velocityCurveType = DEFAULT_VELOCITY_CURVE;
    if (flags & SONG_CONFIG_OVERRIDE_VOLUME_CURVE)
    {
        document->velocityCurveType = (VelocityCurveType)((flags & SONG_CONFIG_VOLUME_CURVE_TYPE_MASK) >> SONG_CONFIG_VOLUME_CURVE_TYPE_SHIFT);
        if (document->velocityCurveType > 5)
            document->velocityCurveType = DEFAULT_VELOCITY_CURVE;
    }
    document->isPristine = FALSE;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetEngineConfig(BAERmfEditorDocument const *document, int32_t *outFlags)
{
    if (!document || !outFlags)
        return BAE_PARAM_ERR;
    *outFlags = document->engineConfigFlags;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetVelocityCurve(BAERmfEditorDocument *document, int curveType)
{
    if (!document)
        return BAE_PARAM_ERR;
    if (curveType < 0)
        curveType = 0;
    if (curveType > 5)
        curveType = 5;
    document->velocityCurveType = (VelocityCurveType)curveType;
    document->isPristine = FALSE;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetVelocityCurve(BAERmfEditorDocument const *document, int *outCurveType)
{
    if (!document || !outCurveType)
        return BAE_PARAM_ERR;
    *outCurveType = (int)document->velocityCurveType;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_AddTrack(BAERmfEditorDocument *document,
                                        BAERmfEditorTrackSetup const *setup,
                                        uint16_t *outTrackIndex)
{
    BAEResult result;
    BAERmfEditorTrack *track;

    if (!document || !setup)
    {
        return BAE_PARAM_ERR;
    }
    if (setup->channel >= BAE_MAX_MIDI_CHANNELS || setup->bank > 16383 || setup->program >= 128)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_GrowBuffer((void **)&document->tracks,
                           &document->trackCapacity,
                           sizeof(BAERmfEditorTrack),
                           document->trackCount + 1);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    track = &document->tracks[document->trackCount];
    XSetMemory(track, sizeof(*track), 0);
    track->channel = setup->channel;
    track->bank = setup->bank;
    track->program = setup->program;
    track->pan = 64;
    track->volume = 127;
    track->transpose = 0;
    if (setup->name)
    {
        track->name = PV_DuplicateString(setup->name);
        if (!track->name)
        {
            return BAE_MEMORY_ERR;
        }
    }
    if (outTrackIndex)
    {
        *outTrackIndex = (uint16_t)document->trackCount;
    }
    document->trackCount++;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetTrackCount(BAERmfEditorDocument const *document,
                                             uint16_t *outTrackCount)
{
    if (!document || !outTrackCount)
    {
        return BAE_PARAM_ERR;
    }
    *outTrackCount = (uint16_t)document->trackCount;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetTrackInfo(BAERmfEditorDocument const *document,
                                            uint16_t trackIndex,
                                            BAERmfEditorTrackInfo *outTrackInfo)
{
    BAERmfEditorTrack const *track;

    if (!outTrackInfo)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    outTrackInfo->name = track->name;
    outTrackInfo->channel = track->channel;
    outTrackInfo->bank = track->bank;
    outTrackInfo->program = track->program;
    outTrackInfo->pan = track->pan;
    outTrackInfo->volume = track->volume;
    outTrackInfo->transpose = track->transpose;
    outTrackInfo->noteCount = track->noteCount;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetTrackInfo(BAERmfEditorDocument *document,
                                            uint16_t trackIndex,
                                            BAERmfEditorTrackInfo const *trackInfo)
{
    BAERmfEditorTrack *track;
    BAERmfEditorCCEvent *ccEvent;
    BAEResult result;

    if (!trackInfo)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    if (trackInfo->channel >= BAE_MAX_MIDI_CHANNELS ||
        trackInfo->bank > 16383 ||
        trackInfo->program >= 128 ||
        trackInfo->pan > 127 ||
        trackInfo->volume > 127 ||
        trackInfo->transpose < -127 ||
        trackInfo->transpose > 127)
    {
        return BAE_PARAM_ERR;
    }
    result = PV_SetTrackName(track, trackInfo->name);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    /* Propagate channel, bank, and program changes to notes and aux events so
       that PV_BuildTrackData (which reads from note/aux fields, not track fields)
       emits the correct MIDI events during preview and export. */
    if (track->channel != trackInfo->channel ||
        track->bank != trackInfo->bank ||
        track->program != trackInfo->program)
    {
        uint32_t i;
        unsigned char oldChannelNibble;

        oldChannelNibble = (unsigned char)(track->channel & 0x0F);

        /* Update notes that matched the old track instrument */
        for (i = 0; i < track->noteCount; ++i)
        {
            BAERmfEditorNote *note;

            note = &track->notes[i];
            if (note->channel == track->channel &&
                note->bank == track->bank &&
                note->program == track->program)
            {
                if (note->channel != trackInfo->channel)
                {
                    unsigned char noteOffType = (unsigned char)(note->noteOffStatus & 0xF0);

                    if (noteOffType == NOTE_ON || noteOffType == NOTE_OFF)
                    {
                        note->noteOffStatus =
                            (unsigned char)(noteOffType | (trackInfo->channel & 0x0F));
                    }
                }
                note->channel = trackInfo->channel;
                note->bank = trackInfo->bank;
                note->program = trackInfo->program;
            }
        }
        /* Remove bank select (CC0/CC32) and program change aux events that
           target the old channel. This forces PV_BuildTrackData to use the
           auto-insertion path (applyProgram=1) which reads bank/program from
           the note fields - already updated above. Without this, stale aux
           events would override the per-note values, and partial coverage
           (e.g. program change without bank select) would leave the bank
           stuck at its old value. */
        {
            uint32_t dst;

            dst = 0;
            for (i = 0; i < track->auxEventCount; ++i)
            {
                BAERmfEditorAuxEvent *aux;
                unsigned char auxChannel;
                unsigned char auxType;
                int remove;

                aux = &track->auxEvents[i];
                auxChannel = (unsigned char)(aux->status & 0x0F);
                auxType = (unsigned char)(aux->status & 0xF0);
                remove = 0;
                if (auxChannel == oldChannelNibble)
                {
                    if (auxType == PROGRAM_CHANGE)
                    {
                        remove = 1;
                    }
                    else if (auxType == CONTROL_CHANGE && aux->dataBytes >= 2 &&
                             (aux->data1 == BANK_MSB || aux->data1 == BANK_LSB))
                    {
                        remove = 1;
                    }
                }
                if (!remove)
                {
                    if (dst != i)
                    {
                        track->auxEvents[dst] = track->auxEvents[i];
                    }
                    dst++;
                }
            }
            track->auxEventCount = dst;
        }
    }
    track->channel = trackInfo->channel;
    track->bank = trackInfo->bank;
    track->program = trackInfo->program;
    track->pan = trackInfo->pan;
    track->volume = trackInfo->volume;
    track->transpose = trackInfo->transpose;

    /* Ensure volume (CC7) and pan (CC10) settings are reflected as CC events
       so they appear in the MIDI output during preview and export. Add a CC7
       at tick 0 only when no CC7 event already exists at tick 0 - never
       overwrite an existing CC7 at any tick. */
    ccEvent = PV_FindTrackCCEventAtTick(track, 7, 0);
    if (!ccEvent)
    {
        result = PV_AddCCEventToTrack(track, 0, 7, trackInfo->volume, 0);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    ccEvent = PV_FindTrackCCEventAtTick(track, 10, 0);
    if (!ccEvent)
    {
        result = PV_AddCCEventToTrack(track, 0, 10, trackInfo->pan, 0);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }

    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetTrackEndOfTrackTick(BAERmfEditorDocument const *document,
                                                      uint16_t trackIndex,
                                                      uint32_t *outTick)
{
    BAERmfEditorTrack const *track;

    if (!outTick)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    *outTick = track->endOfTrackTick;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetTrackEndOfTrackTick(BAERmfEditorDocument *document,
                                                      uint16_t trackIndex,
                                                      uint32_t tick)
{
    BAERmfEditorTrack *track;

    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    track->endOfTrackTick = tick;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetTrackDefaultInstrument(BAERmfEditorDocument *document,
                                                         uint16_t trackIndex,
                                                         uint16_t bank,
                                                         unsigned char program)
{
    BAERmfEditorTrack *track;

    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    if (bank > 16383 || program >= 128)
    {
        return BAE_PARAM_ERR;
    }

    /* Only update track defaults for new notes; do not rewrite existing note instruments. */
    track->bank = bank;
    track->program = program;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult PV_InsertBankSelectBeforeAuxEvent(BAERmfEditorTrack *track,
                                                   uint32_t auxIndex,
                                                   unsigned char channel,
                                                   uint16_t targetBank,
                                                   bool insertMsb,
                                                   bool insertLsb)
{
    uint32_t insertCount;
    uint32_t targetOrder;
    uint32_t tick;
    uint32_t writeOffset;
    BAEResult result;

    if (!track || auxIndex >= track->auxEventCount)
    {
        return BAE_PARAM_ERR;
    }

    insertCount = 0;
    if (insertMsb)
    {
        insertCount++;
    }
    if (insertLsb)
    {
        insertCount++;
    }
    if (insertCount == 0)
    {
        return BAE_NO_ERROR;
    }

    targetOrder = track->auxEvents[auxIndex].eventOrder;
    tick = track->auxEvents[auxIndex].tick;

    PV_ShiftTrackEventOrders(track, targetOrder, insertCount);

    writeOffset = 0;
    if (insertMsb)
    {
        result = PV_AddAuxEventToTrack(track,
                                       tick,
                                       (unsigned char)(CONTROL_CHANGE | (channel & 0x0F)),
                                       BANK_MSB,
                                       (unsigned char)((targetBank >> 7) & 0x7F),
                                       2);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        track->auxEvents[track->auxEventCount - 1].eventOrder = targetOrder + writeOffset;
        writeOffset++;
    }
    if (insertLsb)
    {
        result = PV_AddAuxEventToTrack(track,
                                       tick,
                                       (unsigned char)(CONTROL_CHANGE | (channel & 0x0F)),
                                       BANK_LSB,
                                       (unsigned char)(targetBank & 0x7F),
                                       2);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        track->auxEvents[track->auxEventCount - 1].eventOrder = targetOrder + writeOffset;
    }
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_RemapInstrumentReferences(BAERmfEditorDocument *document,
                                                         uint16_t sourceBank,
                                                         unsigned char sourceProgram,
                                                         uint16_t targetBank,
                                                         unsigned char targetProgram)
{
    uint32_t trackIndex;
    bool changed;
    uint16_t documentRemapChannelMask;

    if (!document || sourceBank > 16383 || targetBank > 16383 || sourceProgram >= 128 || targetProgram >= 128)
    {
        return BAE_PARAM_ERR;
    }
    if (PV_BanksEquivalent(sourceBank, targetBank) && sourceProgram == targetProgram)
    {
        return BAE_NO_ERROR;
    }

    changed = FALSE;
    documentRemapChannelMask = 0;
    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack const *track;
        uint32_t noteIndex;

        track = &document->tracks[trackIndex];
        /* CH10 kit hits select INST by note number, not program — exclude from
         * pitched bank:program remap (matches nbeditor song→INST resolution). */
        if (track->channel != 9 &&
            PV_BanksEquivalent(track->bank, sourceBank) &&
            track->program == sourceProgram)
        {
            documentRemapChannelMask |= (uint16_t)(1u << (track->channel & 0x0F));
        }
        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote const *note = &track->notes[noteIndex];
            if (note->channel != 9 &&
                PV_BanksEquivalent(note->bank, sourceBank) &&
                note->program == sourceProgram)
            {
                documentRemapChannelMask |= (uint16_t)(1u << (note->channel & 0x0F));
            }
        }
    }

    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack *track;
        uint32_t noteIndex;
        bool trackChanged;
        BAEResult remapResult;

        track = &document->tracks[trackIndex];

        if (track->channel != 9 &&
            PV_BanksEquivalent(track->bank, sourceBank) &&
            track->program == sourceProgram)
        {
            track->bank = PV_RemapBankPreserveForm(track->bank, targetBank);
            track->program = targetProgram;
            changed = TRUE;
        }

        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote *note;

            note = &track->notes[noteIndex];
            if (note->channel != 9 &&
                PV_BanksEquivalent(note->bank, sourceBank) &&
                note->program == sourceProgram)
            {
                note->bank = PV_RemapBankPreserveForm(note->bank, targetBank);
                note->program = targetProgram;
                changed = TRUE;
            }
        }

        trackChanged = FALSE;
        remapResult = PV_RemapTrackInstrumentReferences(track,
                                                        sourceBank,
                                                        sourceProgram,
                                                        targetBank,
                                                        targetProgram,
                                                        documentRemapChannelMask,
                                                        &trackChanged);
        if (remapResult != BAE_NO_ERROR)
        {
            return remapResult;
        }
        if (trackChanged)
        {
            changed = TRUE;
        }
    }

    if (changed)
    {
        PV_MarkDocumentDirty(document);
    }
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_DeleteTrack(BAERmfEditorDocument *document,
                                           uint16_t trackIndex)
{
    BAERmfEditorTrack *track;

    if (!document || trackIndex >= document->trackCount)
    {
        return BAE_PARAM_ERR;
    }
    track = &document->tracks[trackIndex];
    PV_FreeString(&track->name);
    if (track->notes)
    {
        XDisposePtr(track->notes);
        track->notes = NULL;
    }
    if (track->ccEvents)
    {
        XDisposePtr(track->ccEvents);
        track->ccEvents = NULL;
    }
    PV_FreeTrackSysExEvents(track);
    PV_FreeTrackAuxEvents(track);
    PV_FreeTrackMetaEvents(track);
    if (trackIndex + 1 < document->trackCount)
    {
        XBlockMove(&document->tracks[trackIndex + 1],
                   &document->tracks[trackIndex],
                   (int32_t)((document->trackCount - (trackIndex + 1)) * sizeof(BAERmfEditorTrack)));
    }
    document->trackCount--;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_AddNote(BAERmfEditorDocument *document,
                                       uint16_t trackIndex,
                                       uint32_t startTick,
                                       uint32_t durationTicks,
                                       unsigned char note,
                                       unsigned char velocity)
{
    BAERmfEditorTrack *track;
    uint32_t noteOnOrder;
    uint32_t noteOffOrder;

    if (!document)
    {
        return BAE_NULL_OBJECT;
    }
    if (note > 127 || velocity > 127)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    {
        BAEResult result;

        noteOnOrder = track->nextEventOrder;
        noteOffOrder = track->nextEventOrder + 1;
        track->nextEventOrder += 2;

        result = PV_AddNoteToTrack(track,
                       startTick,
                       durationTicks,
                       note,
                       velocity,
                       track->channel,
                       track->bank,
                                   track->program,
                                   (unsigned char)(NOTE_OFF | (track->channel & 0x0F)),
                                   0,
                                   noteOnOrder,
                                   noteOffOrder);
        if (result == BAE_NO_ERROR)
        {
            PV_MarkDocumentDirty(document);
        }
        return result;
    }
}


BAEResult BAERmfEditorDocument_GetNoteCount(BAERmfEditorDocument const *document,
                                            uint16_t trackIndex,
                                            uint32_t *outNoteCount)
{
    BAERmfEditorTrack const *track;

    if (!outNoteCount)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    *outNoteCount = track->noteCount;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetNoteInfo(BAERmfEditorDocument const *document,
                                           uint16_t trackIndex,
                                           uint32_t noteIndex,
                                           BAERmfEditorNoteInfo *outNoteInfo)
{
    BAERmfEditorTrack const *track;
    BAERmfEditorNote const *note;

    if (!outNoteInfo)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrackConst(document, trackIndex);
    if (!track || noteIndex >= track->noteCount)
    {
        return BAE_PARAM_ERR;
    }
    note = &track->notes[noteIndex];
    outNoteInfo->startTick = note->startTick;
    outNoteInfo->durationTicks = note->durationTicks;
    outNoteInfo->note = note->note;
    outNoteInfo->velocity = note->velocity;
    outNoteInfo->channel = note->channel;
    outNoteInfo->bank = note->bank;
    outNoteInfo->program = note->program;
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SetNoteInfo(BAERmfEditorDocument *document,
                                           uint16_t trackIndex,
                                           uint32_t noteIndex,
                                           BAERmfEditorNoteInfo const *noteInfo)
{
    BAERmfEditorTrack *track;

    if (!noteInfo || noteInfo->durationTicks == 0 || noteInfo->note > 127 || noteInfo->velocity > 127 || noteInfo->channel > 15 || noteInfo->program > 127 || noteInfo->bank > 16383)
    {
        return BAE_PARAM_ERR;
    }
    track = PV_GetTrack(document, trackIndex);
    if (!track || noteIndex >= track->noteCount)
    {
        return BAE_PARAM_ERR;
    }
    track->notes[noteIndex].startTick = noteInfo->startTick;
    track->notes[noteIndex].durationTicks = noteInfo->durationTicks;
    track->notes[noteIndex].note = noteInfo->note;
    track->notes[noteIndex].velocity = noteInfo->velocity;
    if (track->notes[noteIndex].channel != noteInfo->channel)
    {
        unsigned char noteOffType = (unsigned char)(track->notes[noteIndex].noteOffStatus & 0xF0);

        if (noteOffType == NOTE_ON || noteOffType == NOTE_OFF)
        {
            track->notes[noteIndex].noteOffStatus =
                (unsigned char)(noteOffType | (noteInfo->channel & 0x0F));
        }
    }
    track->notes[noteIndex].channel = noteInfo->channel;
    track->notes[noteIndex].bank = noteInfo->bank;
    track->notes[noteIndex].program = noteInfo->program;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_DeleteNote(BAERmfEditorDocument *document,
                                          uint16_t trackIndex,
                                          uint32_t noteIndex)
{
    BAERmfEditorTrack *track;

    track = PV_GetTrack(document, trackIndex);
    if (!track || noteIndex >= track->noteCount)
    {
        return BAE_PARAM_ERR;
    }
    if (noteIndex + 1 < track->noteCount)
    {
        XBlockMove(&track->notes[noteIndex + 1],
                   &track->notes[noteIndex],
                   (int32_t)((track->noteCount - (noteIndex + 1)) * sizeof(BAERmfEditorNote)));
    }
    track->noteCount--;
    PV_MarkDocumentDirty(document);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_TrimToTick(BAERmfEditorDocument *document,
                                          uint32_t boundaryTick)
{
    uint32_t trackIndex;
    bool changed;

    if (!document)
    {
        return BAE_NULL_OBJECT;
    }

    changed = FALSE;
    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack *track;
        uint32_t readIndex;
        uint32_t writeIndex;

        track = &document->tracks[trackIndex];

        writeIndex = 0;
        for (readIndex = 0; readIndex < track->noteCount; ++readIndex)
        {
            BAERmfEditorNote note;
            uint64_t noteEnd;

            note = track->notes[readIndex];
            if (note.startTick >= boundaryTick)
            {
                changed = TRUE;
                continue;
            }

            noteEnd = (uint64_t)note.startTick + (uint64_t)note.durationTicks;
            if (noteEnd > (uint64_t)boundaryTick)
            {
                note.durationTicks = boundaryTick - note.startTick;
                changed = TRUE;
                if (note.durationTicks == 0)
                {
                    continue;
                }
            }

            if (writeIndex != readIndex)
            {
                track->notes[writeIndex] = note;
            }
            writeIndex++;
        }
        track->noteCount = writeIndex;

        writeIndex = 0;
        for (readIndex = 0; readIndex < track->ccEventCount; ++readIndex)
        {
            BAERmfEditorCCEvent event;

            event = track->ccEvents[readIndex];
            if (event.tick >= boundaryTick)
            {
                changed = TRUE;
                continue;
            }
            if (writeIndex != readIndex)
            {
                track->ccEvents[writeIndex] = event;
            }
            writeIndex++;
        }
        track->ccEventCount = writeIndex;

        writeIndex = 0;
        for (readIndex = 0; readIndex < track->sysexEventCount; ++readIndex)
        {
            BAERmfEditorSysExEvent event;

            event = track->sysexEvents[readIndex];
            if (event.tick >= boundaryTick)
            {
                if (event.data)
                {
                    XDisposePtr(event.data);
                }
                changed = TRUE;
                continue;
            }
            if (writeIndex != readIndex)
            {
                track->sysexEvents[writeIndex] = event;
            }
            writeIndex++;
        }
        track->sysexEventCount = writeIndex;

        writeIndex = 0;
        for (readIndex = 0; readIndex < track->auxEventCount; ++readIndex)
        {
            BAERmfEditorAuxEvent event;

            event = track->auxEvents[readIndex];
            if (event.tick >= boundaryTick)
            {
                changed = TRUE;
                continue;
            }
            if (writeIndex != readIndex)
            {
                track->auxEvents[writeIndex] = event;
            }
            writeIndex++;
        }
        track->auxEventCount = writeIndex;

        writeIndex = 0;
        for (readIndex = 0; readIndex < track->metaEventCount; ++readIndex)
        {
            BAERmfEditorMetaEvent event;

            event = track->metaEvents[readIndex];
            if (event.tick >= boundaryTick)
            {
                if (event.data)
                {
                    XDisposePtr(event.data);
                }
                changed = TRUE;
                continue;
            }
            if (writeIndex != readIndex)
            {
                track->metaEvents[writeIndex] = event;
            }
            writeIndex++;
        }
        track->metaEventCount = writeIndex;

        if (boundaryTick > 0)
        {
            BAEResult stopResult;

            stopResult = PV_AddTrimHardStopEventsToTrack(track, boundaryTick - 1);
            if (stopResult != BAE_NO_ERROR)
            {
                return stopResult;
            }
            changed = TRUE;
        }

        if (track->endOfTrackTick != boundaryTick)
        {
            track->endOfTrackTick = boundaryTick;
            changed = TRUE;
        }
    }

    {
        uint32_t readIndex;
        uint32_t writeIndex;

        writeIndex = 0;
        for (readIndex = 0; readIndex < document->tempoEventCount; ++readIndex)
        {
            BAERmfEditorTempoEvent event;

            event = document->tempoEvents[readIndex];
            if (event.tick >= boundaryTick)
            {
                changed = TRUE;
                continue;
            }
            if (writeIndex != readIndex)
            {
                document->tempoEvents[writeIndex] = event;
            }
            writeIndex++;
        }
        document->tempoEventCount = writeIndex;
    }

    if (changed)
    {
        PV_MarkDocumentDirty(document);
    }

    return BAE_NO_ERROR;
}


void PV_RemoveDocumentInstrumentAuxEvents(BAERmfEditorDocument *document)
{
    uint32_t trackIndex;

    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack *track = &document->tracks[trackIndex];
        uint32_t readIndex;
        uint32_t writeIndex = 0;

        for (readIndex = 0; readIndex < track->auxEventCount; ++readIndex)
        {
            BAERmfEditorAuxEvent const *event = &track->auxEvents[readIndex];
            unsigned char eventType = (unsigned char)(event->status & 0xF0);
            bool isInstrumentState = eventType == PROGRAM_CHANGE ||
                (eventType == CONTROL_CHANGE && event->dataBytes >= 2 &&
                 (event->data1 == BANK_MSB || event->data1 == BANK_LSB));

            if (isInstrumentState)
            {
                continue;
            }
            if (writeIndex != readIndex)
            {
                track->auxEvents[writeIndex] = track->auxEvents[readIndex];
            }
            writeIndex++;
        }
        track->auxEventCount = writeIndex;
    }
}


BAEResult BAERmfEditorDocument_SetMidiStorageType(BAERmfEditorDocument *document,
                                                  BAERmfEditorMidiStorageType storageType)
{
    if (!document)
    {
        return BAE_PARAM_ERR;
    }
    document->midiStorageType = PV_NormalizeMidiStorageType(storageType);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_GetMidiStorageType(BAERmfEditorDocument const *document,
                                                  BAERmfEditorMidiStorageType *outStorageType)
{
    if (!document || !outStorageType)
    {
        return BAE_PARAM_ERR;
    }
    *outStorageType = PV_NormalizeMidiStorageType(document->midiStorageType);
    return BAE_NO_ERROR;
}


BAE_BOOL BAERmfEditorDocument_CanSaveAsMidi(BAERmfEditorDocument const *document)
{
    if (!document)
    {
        return FALSE;
    }
    /* Raw MIDI export is valid only for documents without custom sample/instrument data. */
    return (document->sampleCount == 0) ? TRUE : FALSE;
}


BAEResult BAERmfEditorDocument_SaveAsMidiToMemory(BAERmfEditorDocument *document,
                                                  unsigned char **outData,
                                                  uint32_t *outSize)
{
    ByteBuffer midiData;
    BAEResult result;
    XPTR copy;

    if (!document || !outData || !outSize)
    {
        return BAE_PARAM_ERR;
    }
    *outData = NULL;
    *outSize = 0;
    result = BAERmfEditorDocument_Validate(document);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    XSetMemory(&midiData, sizeof(midiData), 0);
    if (document->loopMarkersOnlyDirty)
    {
        result = PV_BuildMidiWithAppendedLoopTrack(document, &midiData);
    }
    else
    {
        result = PV_BuildMidiFile(document, &midiData);
    }
    if (result != BAE_NO_ERROR)
    {
        PV_ByteBufferDispose(&midiData);
        return result;
    }
    if (!midiData.data || midiData.size == 0)
    {
        PV_ByteBufferDispose(&midiData);
        return BAE_BAD_FILE;
    }
    {
        uint32_t midiSize = (uint32_t)midiData.size;

        copy = XNewPtr((int32_t)midiSize);
        if (!copy)
        {
            PV_ByteBufferDispose(&midiData);
            return BAE_MEMORY_ERR;
        }
        XBlockMove(midiData.data, copy, (int32_t)midiSize);
        PV_ByteBufferDispose(&midiData);
        *outData = (unsigned char *)copy;
        *outSize = midiSize;
    }
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_SaveAsMidi(BAERmfEditorDocument *document,
                                          BAEPathName filePath)
{
    XFILENAME name;
    XFILE fileRef;
    ByteBuffer midiData;
    BAEResult result;

    if (!document || !filePath)
    {
        return BAE_PARAM_ERR;
    }
    if (!BAERmfEditorDocument_CanSaveAsMidi(document))
    {
        return BAE_UNSUPPORTED_FORMAT;
    }
    result = BAERmfEditorDocument_Validate(document);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    XSetMemory(&midiData, sizeof(midiData), 0);
    if (document->loopMarkersOnlyDirty)
    {
        result = PV_BuildMidiWithAppendedLoopTrack(document, &midiData);
    }
    else
    {
        result = PV_BuildMidiFile(document, &midiData);
    }
    if (result != BAE_NO_ERROR)
    {
        PV_ByteBufferDispose(&midiData);
        return result;
    }
    PV_DebugReportMidiRoundTripDiff(document, &midiData);
    XConvertPathToXFILENAME(filePath, &name);
    fileRef = XFileOpenForWrite(&name, TRUE);
    if (!fileRef)
    {
        PV_ByteBufferDispose(&midiData);
        return BAE_FILE_IO_ERROR;
    }
    if (XFileSetLength(fileRef, 0) != 0 ||
        XFileSetPosition(fileRef, 0L) != 0 ||
        XFileWrite(fileRef, midiData.data, (int32_t)midiData.size) != 0)
    {
        XFileClose(fileRef);
        PV_ByteBufferDispose(&midiData);
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(fileRef);
    PV_ByteBufferDispose(&midiData);
    return BAE_NO_ERROR;
}


BAEResult BAERmfEditorDocument_DebugReportMidiRoundTripDiff(BAERmfEditorDocument *document)
{
    ByteBuffer midiData;
    BAEResult result;

    if (!document)
    {
        return BAE_PARAM_ERR;
    }
    XSetMemory(&midiData, sizeof(midiData), 0);
    result = PV_BuildMidiFile(document, &midiData);
    if (result == BAE_NO_ERROR)
    {
        PV_DebugReportMidiRoundTripDiff(document, &midiData);
    }
    PV_ByteBufferDispose(&midiData);
    return result;
}

