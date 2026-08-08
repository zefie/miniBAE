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
**  BAE_EditorAPI_Internal.h
**
**  Private shared types and PV_* declarations for BAE_EditorAPI_*.c modules.
**  Public API: BAE_EditorAPI.h
**  Perf notes: neobae/docs/BAE_EditorAPI_PERF.md
*/
/*****************************************************************************/

#ifndef BAE_EDITOR_API_INTERNAL_H
#define BAE_EDITOR_API_INTERNAL_H

#include "NeoBAE.h"
#include "BAE_EditorAPI.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "X_API.h"
#include "X_Formats.h"
#include "X_Assert.h"
#include "X_Instruments.h"
#include "X_EditorTools.h"

#if USE_MTHC_SUPPORT == TRUE
#include "../../mthc/mthc_decomp.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BAE_RMF_EDITOR_SUBTYPE_TAG FOUR_CHAR('b','q','s','t')

/* ---- document / editor private types ---- */
typedef struct BAERmfEditorNote
{
    uint32_t startTick;
    uint32_t durationTicks;
    unsigned char note;
    unsigned char velocity;
    unsigned char channel;
    uint16_t bank;
    unsigned char program;
    unsigned char noteOffVelocity;
    unsigned char noteOffStatus;
    uint32_t noteOnOrder;
    uint32_t noteOffOrder;
} BAERmfEditorNote;

typedef struct BAERmfEditorCCEvent
{
    uint32_t tick;
    uint32_t eventOrder;
    unsigned char cc;    /* 0-127 = CC number; 0xFF = pitch bend sentinel */
    unsigned char value; /* CC value, or pitch bend LSB when cc == 0xFF */
    unsigned char data2; /* 0 for CC events; pitch bend MSB when cc == 0xFF */
} BAERmfEditorCCEvent;

typedef struct BAERmfEditorSysExEvent
{
    uint32_t tick;
    uint32_t eventOrder;
    unsigned char status; /* 0xF0 or 0xF7 */
    unsigned char *data;
    uint32_t size;
} BAERmfEditorSysExEvent;

typedef struct BAERmfEditorAuxEvent
{
    uint32_t tick;
    uint32_t eventOrder;
    unsigned char status;
    unsigned char data1;
    unsigned char data2;
    unsigned char dataBytes;
} BAERmfEditorAuxEvent;

typedef struct BAERmfEditorMetaEvent
{
    uint32_t tick;
    uint32_t eventOrder;
    unsigned char type;
    unsigned char *data;
    uint32_t size;
} BAERmfEditorMetaEvent;

typedef struct BAERmfEditorTrack
{
    char *name;
    unsigned char channel;
    uint16_t bank;
    unsigned char program;
    unsigned char pan;
    unsigned char volume;
    int16_t transpose;
    BAERmfEditorNote *notes;
    uint32_t noteCount;
    uint32_t noteCapacity;
    BAERmfEditorCCEvent *ccEvents;
    uint32_t ccEventCount;
    uint32_t ccEventCapacity;
    BAERmfEditorSysExEvent *sysexEvents;
    uint32_t sysexEventCount;
    uint32_t sysexEventCapacity;
    BAERmfEditorAuxEvent *auxEvents;
    uint32_t auxEventCount;
    uint32_t auxEventCapacity;
    BAERmfEditorMetaEvent *metaEvents;
    uint32_t metaEventCount;
    uint32_t metaEventCapacity;
    uint32_t nextEventOrder;
    uint32_t endOfTrackTick;   /* tick of the original 0x2F end-of-track, 0 = unknown */
} BAERmfEditorTrack;

typedef struct BAERmfEditorSample
{
    char *displayName;
    char *sourcePath;
    unsigned char program;
    uint32_t instID;       /* original INST resource ID (e.g. 562 for bank-2 prog 50) */
    unsigned char rootKey;
    unsigned char lowKey;
    unsigned char highKey;
    int16_t splitVolume;       /* per-split miscParameter2 (volume), 0 = use default */
    uint32_t sourceCompressionType;
    uint32_t sourceCompressionSubType;
    XResourceType originalSndResourceType;
    BAESampleInfo sampleInfo;
    GM_Waveform *waveform;
    uint32_t sampleAssetID;              /* shared audio asset id; 0 valid; NONE=0xFFFFFFFF */
    /* Compression control */
    BAERmfEditorCompressionType targetCompressionType; /* desired output codec */
    BAERmfEditorOpusMode targetOpusMode;
    bool opusUseRoundTripResampling;  /* for Opus: encode at 48kHz, play back time-stretched at source rate */
    XPTR    originalSndData;   /* normalized plain SND blob (ESND/CSND already unwrapped) */
    int32_t originalSndSize;   /* byte count of originalSndData */
    /* Bank alias fields: sample references a loaded bank's SND without PCM decode */
    bool   isBankAlias;       /* TRUE if this sample is a bank alias (no waveform) */
    BAEBankToken aliasBankToken;       /* bank that owns the SND resource */
    XShortResourceID aliasSndResourceID;  /* SND resource ID within the bank */
} BAERmfEditorSample;

/* ---------- Extended instrument data (ADSR, LFO, LPF, curves) ---------- */

#define EDITOR_MAX_ADSR_STAGES 32  /* matches ADSR_STAGES from GenSnd.h; RMF/HSB capped at 8 at write time */
#define EDITOR_MAX_LFOS        6  /* matches MAX_LFOS */
#define EDITOR_MAX_CURVES      4  /* matches MAX_CURVES */

typedef struct EditorADSRStage
{
    int32_t level;
    int32_t time;
    int32_t flags;  /* FOUR_CHAR form: 'LINE', 'SUST', 'LAST', 'GOTO', 'GOST', 'RELS', or 0 */
} EditorADSRStage;

typedef struct EditorADSR
{
    uint32_t stageCount;
    EditorADSRStage stages[EDITOR_MAX_ADSR_STAGES];
} EditorADSR;

typedef struct EditorLFO
{
    int32_t destination;  /* FOUR_CHAR: 'VOLU','PITC','SPAN','PAN ','LPFR','LPRE','LPAM' */
    int32_t period;
    int32_t waveShape;    /* FOUR_CHAR: 'SINE','TRIA','SQUA','SQU2','SAWT','SAW2' */
    int32_t DC_feed;
    int32_t level;
    EditorADSR adsr;      /* per-LFO envelope */
} EditorLFO;

typedef struct EditorCurve
{
    int32_t tieFrom;
    int32_t tieTo;
    int16_t curveCount;
    uint8_t from_Value[EDITOR_MAX_ADSR_STAGES];  /* MIDI 0-127 */
    int16_t to_Scalar[EDITOR_MAX_ADSR_STAGES];
} EditorCurve;

typedef struct BAERmfEditorInstrumentExt
{
    XLongResourceID instID;
    char           *displayName;      /* INST resource name */
    bool           hasExtendedData;  /* TRUE if loaded from an extended-format INST */
    bool           dirty;            /* TRUE if user modified via Set API */
    unsigned char   flags1;           /* ZBF_ bitmask from InstrumentResource */
    unsigned char   flags2;           /* ZBF_ bitmask from InstrumentResource */
    char            panPlacement;     /* stereo pan from INST header */
    int16_t         midiRootKey;      /* master root key from INST header */
    int16_t         miscParameter1;   /* offset high-word when ZBF_enableSampleOffsetStart, else root key or 0 */
    int16_t         miscParameter2;   /* volume level (100 = default) */
    bool           hasDefaultMod;    /* TRUE if INST_DEFAULT_MOD unit was present */
    int32_t         LPF_frequency;
    int32_t         LPF_resonance;
    int32_t         LPF_lowpassAmount;
    int16_t         defaultReverbSend;
    int16_t         defaultChorusSend;
    EditorADSR      volumeADSR;
    uint32_t        lfoCount;
    EditorLFO       lfos[EDITOR_MAX_LFOS];
    uint32_t        curveCount;
    EditorCurve     curves[EDITOR_MAX_CURVES];
#if USE_ZMF_SUPPORT == TRUE
    bool            useOscillator;
    int32_t         oscWaveShape;
    int32_t         oscPulseWidth;
    int32_t         oscVolume;
#endif
    /* Raw INST resource blob for unmodified round-trip */
    XPTR            originalInstData;
    int32_t         originalInstSize;
} BAERmfEditorInstrumentExt;

/* ----------------------------------------------------------------------- */

typedef struct BAERmfEditorResourceEntry
{
    XResourceType type;
    XLongResourceID id;
    unsigned char pascalName[256];
    XPTR data;
    int32_t size;
} BAERmfEditorResourceEntry;

typedef struct BAERmfEditorTempoEvent
{
    uint32_t tick;
    uint32_t microsecondsPerQuarter;
} BAERmfEditorTempoEvent;

struct BAERmfEditorDocument
{
    uint32_t tempoBPM;
    uint16_t ticksPerQuarter;
    SongType songType;
    int32_t songTempo;
    int16_t songPitchShift;
    bool songLocked;
    bool songEmbedded;
    int16_t maxMidiNotes;
    int16_t maxEffects;
    int16_t mixLevel;
    int16_t songVolume;
    BAEReverbType reverbType;
    char *info[INFO_TYPE_COUNT];
    BAERmfEditorTrack *tracks;
    uint32_t trackCount;
    uint32_t trackCapacity;
    BAERmfEditorSample *samples;
    uint32_t sampleCount;
    uint32_t sampleCapacity;
    uint32_t nextSampleAssetID;
    BAERmfEditorTempoEvent *tempoEvents;
    uint32_t tempoEventCount;
    uint32_t tempoEventCapacity;
    BAERmfEditorResourceEntry *originalResources;
    uint32_t originalResourceCount;
    uint32_t originalResourceCapacity;
    XLongResourceID originalSongID;
    XLongResourceID originalObjectResourceID;
    XResourceType originalMidiType;
    BAERmfEditorMidiStorageType midiStorageType;
    unsigned char *debugOriginalMidiData;
    uint32_t debugOriginalMidiDataSize;
    bool loadedFromRmf;
    bool isPristine;
    bool loopMarkersOnlyDirty;
    BAERmfEditorInstrumentExt *instrumentExts;
    uint32_t instrumentExtCount;
    uint32_t instrumentExtCapacity;
    int32_t engineConfigFlags;  // per-song engine config (SONG_CONFIG_* bits)
    VelocityCurveType velocityCurveType;  // per-song velocity curve (0-5)
};


/* ---- MIDI / buffer helpers ---- */
typedef struct ByteBuffer
{
    unsigned char *data;
    uint32_t size;
    uint32_t capacity;
} ByteBuffer;

typedef struct MidiEventRecord
{
    uint32_t tick;
    uint32_t sequence;
    unsigned char order;
    unsigned char status;
    unsigned char data1;
    unsigned char data2;
    unsigned char dataBytes;
    unsigned char const *blob;
    uint32_t blobSize;
    uint16_t bank;
    unsigned char program;
    unsigned char applyProgram;
} MidiEventRecord;

typedef struct BAEDebugMidiTrackStats
{
    uint32_t eventCount;
    uint32_t eventHash;
    uint32_t noteOnCount;
    uint32_t noteOffCount;
    uint32_t controlChangeCount;
    uint32_t programChangeCount;
    uint32_t channelAftertouchCount;
    uint32_t polyAftertouchCount;
    uint32_t pitchBendCount;
    uint32_t sysexCount;
    uint32_t tempoMetaCount;
    uint32_t otherMetaCount;
    uint32_t ccCount[128];
    uint32_t firstCCTick[128];
} BAEDebugMidiTrackStats;

typedef struct BAEDebugMidiStats
{
    uint16_t trackCount;
    BAEDebugMidiTrackStats *tracks;
} BAEDebugMidiStats;

#define BAE_EDITOR_CC_PITCH_BEND_SENTINEL        0xFF
#define BAE_EDITOR_CC_CHANNEL_AFTERTOUCH_SENTINEL 0xFE
#define BAE_EDITOR_CC_POLY_AFTERTOUCH_SENTINEL    0xFD

typedef struct BAERmfEditorActiveNote
{
    struct BAERmfEditorActiveNote *next;
    uint32_t startTick;
    uint32_t noteOnOrder;
    unsigned char channel;
    unsigned char note;
    unsigned char velocity;
    uint16_t bank;
    unsigned char program;
} BAERmfEditorActiveNote;


/* ---- private helper typedefs ---- */
typedef struct PV_ChannelStateEvent
{
    uint32_t tick;
    uint16_t trackIndex;
    uint32_t sequence;
    unsigned char kind;      /* 0=bank msb, 1=bank lsb, 2=program, 3=note-on */
    unsigned char channel;
    unsigned char value;
    uint32_t noteIndex;      /* valid when kind==3 */
} PV_ChannelStateEvent;

typedef struct PV_AuxOrderRef
{
    uint32_t index;
    uint32_t tick;
    uint32_t eventOrder;
} PV_AuxOrderRef;

typedef struct
{
    XResourceType   oldType;
    XResourceType   newType;
    XShortResourceID sndID;
    char            name[256];
    XPTR            data;
    int32_t         size;
} PV_SndReplacement;

typedef struct PV_UsedInstrumentPair
{
    uint16_t bank;
    unsigned char program;
} PV_UsedInstrumentPair;

typedef struct PV_UsedPercussion
{
    uint16_t bank;
    unsigned char note;
} PV_UsedPercussion;


/* ---- shared PV_* across translation units ---- */
void PV_CopyStringBounded(char *dst, uint32_t dstSize, char const *src);
bool PV_PathHasExtensionIgnoreCase(char const *filePath, char const *ext);
BAEFileType PV_DetectOggCodecBySignature(BAEPathName filePath);
BAEFileType PV_DetermineEditorImportFileType(BAEPathName filePath);
BAEFileType PV_DetermineEditorImportMemoryFileType(void const *data, uint32_t dataSize, BAEFileType fileTypeHint);
uint32_t PV_GetStoredCompressionSubTypeFromSnd(XPTR sndData, int32_t sndSize, uint32_t compressionType);
void PV_StoreCompressionSubTypeInSnd(XPTR sndData, int32_t sndSize, SndCompressionType compressionType, SndCompressionSubType compressionSubType);
bool PV_IsValidEditorOpusMode(BAERmfEditorOpusMode opusMode);
uint32_t PV_SubTypeToOpusBitrateIndex(SndCompressionSubType subType);
SndCompressionSubType PV_ComposeOpusEncodeSubType(SndCompressionSubType baseSubType, BAERmfEditorOpusMode opusMode);
uint32_t PV_AllocateSampleAssetID(BAERmfEditorDocument *document);
void PV_NoteSampleAssetID(BAERmfEditorDocument *document, uint32_t assetID);
void PV_ReserveBankSoundResourceIDs(BAERmfEditorDocument *document, XFILE bankFile);
BAERmfEditorSample *PV_FindFirstSampleForAsset(BAERmfEditorDocument *document, uint32_t assetID);
uint32_t PV_CountSamplesForAsset(BAERmfEditorDocument const *document, uint32_t assetID);
bool PV_IsOpusCompression(BAERmfEditorCompressionType ct);
bool PV_AssetSupportsDontChange(BAERmfEditorDocument const *document, uint32_t assetID);
BAE_UNSIGNED_FIXED PV_NormalizeSampleRateForSave(BAE_UNSIGNED_FIXED sampleRate);
bool PV_IsMpegCompression(BAERmfEditorCompressionType ct);
uint32_t PV_ExtractOpusInputRateFromOriginalSnd(BAERmfEditorSample const *sample);
uint32_t PV_SampleRateFixedToHz(BAE_UNSIGNED_FIXED fixedRate);
uint32_t PV_ChooseUpscaledRateFromTable(uint32_t sourceHz, uint32_t const *table, uint32_t count);
BAE_UNSIGNED_FIXED PV_ChooseCodecRateFromSourceHz(BAERmfEditorCompressionType compressionType, uint32_t sourceHz);
BAE_UNSIGNED_FIXED PV_RecommendSampleRateForCompression(BAERmfEditorSample const *sample, BAERmfEditorCompressionType compressionType);
BAEResult PV_ResampleWaveformLinear(GM_Waveform *waveform, BAE_UNSIGNED_FIXED targetRate, XPTR *ioWaveDataOwner);
BAEResult PV_EnsureWaveformDataOwned(GM_Waveform *waveform, XPTR *ioWaveDataOwner);
BAEResult PV_ApplyOpusLoopSeamMicroFade(GM_Waveform *waveform, XPTR *ioWaveDataOwner, uint32_t sampleIndex);
void PV_ForceSndLoopPoints(XPTR sndResource, int32_t loopStart, int32_t loopEnd);
void PV_ForceSndDecodedFrameCount(XPTR sndResource, uint32_t frameCount);
void PV_RemapLoopPointsToFrameCount(uint32_t sourceFrames, uint32_t targetFrames, int32_t *ioLoopStart, int32_t *ioLoopEnd);
uint32_t PV_GetDecodedFrameCountFromSnd(XPTR sndResource);
bool PV_CanReuseSndResourceForSamples(BAERmfEditorSample const *left, BAERmfEditorSample const *right);
BAEResult PV_CopyOriginalInstExtendedTail(BAERmfEditorInstrumentExt const *ext, XPTR *outTail, int32_t *outTailSize);
bool PV_IsNoSampleSndID(XShortResourceID sid);
char *PV_DuplicateString(char const *source);
void PV_FreeString(char **target);
void PV_MarkDocumentDirty(BAERmfEditorDocument *document);
BAEResult PV_SetDebugOriginalMidiData(BAERmfEditorDocument *document, unsigned char const *midiData, uint32_t midiDataSize);
void PV_FreeDebugOriginalMidiData(BAERmfEditorDocument *document);
void PV_DebugFreeMidiStats(BAEDebugMidiStats *stats);
void PV_DebugHashByte(uint32_t *hash, unsigned char value);
void PV_DebugHashU32(uint32_t *hash, uint32_t value);
BAEResult PV_DebugCollectMidiStats(unsigned char const *data, uint32_t dataSize, BAEDebugMidiStats *outStats);
void PV_DebugDumpMidiTrack(const char *label, unsigned char const *data, uint32_t dataSize, uint16_t trackIndex);
void PV_DebugReportMidiRoundTripDiff(BAERmfEditorDocument const *document, ByteBuffer const *generatedMidi);
void PV_FreeOriginalResources(BAERmfEditorDocument *document);
void PV_ClearTempoEvents(BAERmfEditorDocument *document);
BAEResult PV_AddTempoEvent(BAERmfEditorDocument *document, uint32_t tick, uint32_t microsecondsPerQuarter);
BAEResult PV_AddCCEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char cc, unsigned char value, unsigned char data2);
BAEResult PV_AddSysExEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char status, unsigned char const *data, uint32_t size);
void PV_FreeTrackSysExEvents(BAERmfEditorTrack *track);
BAEResult PV_AddAuxEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char status, unsigned char data1, unsigned char data2, unsigned char dataBytes);
void PV_FreeTrackAuxEvents(BAERmfEditorTrack *track);
BAEResult PV_AddMetaEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char type, unsigned char const *data, uint32_t size);
void PV_FreeTrackMetaEvents(BAERmfEditorTrack *track);
unsigned char PV_ToLowerAscii(unsigned char c);
bool PV_MarkerStartsWith(unsigned char const *data, uint32_t size, char const *text);
bool PV_IsLoopStartMarkerText(unsigned char const *data, uint32_t size, int32_t *outLoopCount);
bool PV_IsLoopEndMarkerText(unsigned char const *data, uint32_t size);
void PV_RemoveLoopMarkersFromTrack(BAERmfEditorTrack *track);
int PV_CompareCCEvents(void const *left, void const *right);
BAERmfEditorCCEvent *PV_FindTrackCCEventAtTick(BAERmfEditorTrack *track, unsigned char cc, uint32_t atTick);
BAERmfEditorCCEvent *PV_FindTrackCCEvent(BAERmfEditorTrack *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex);
BAERmfEditorCCEvent const *PV_FindTrackCCEventConst(BAERmfEditorTrack const *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex);
BAEResult PV_CaptureOriginalResourcesFromFile(BAERmfEditorDocument *document, XFILE fileRef);
BAEResult PV_GrowBuffer(void **buffer, uint32_t *capacity, uint32_t elementSize, uint32_t minimumCount);
BAEResult PV_ByteBufferReserve(ByteBuffer *buffer, uint32_t extraBytes);
BAEResult PV_ByteBufferAppend(ByteBuffer *buffer, void const *data, uint32_t length);
BAEResult PV_ByteBufferAppendByte(ByteBuffer *buffer, unsigned char value);
BAEResult PV_ByteBufferAppendBE16(ByteBuffer *buffer, uint16_t value);
BAEResult PV_ByteBufferAppendBE32(ByteBuffer *buffer, uint32_t value);
BAEResult PV_ByteBufferAppendVLQ(ByteBuffer *buffer, uint32_t value);
void PV_ByteBufferDispose(ByteBuffer *buffer);
BAEResult PV_SetDocumentString(char **target, char const *value);
AudioFileType PV_TranslateEditorFileType(BAEFileType fileType);
bool PV_IsEditorCompressedImportType(BAEFileType fileType);
bool PV_IsSupportedPassthroughCompression(SndCompressionType compressionType);
BAEResult PV_CompressionTypeFromEditorFileType(BAEFileType fileType, SndCompressionType *outCompressionType);
BAEResult PV_ReadFileIntoMemory(XFILENAME const *fileName, XPTR *outData, int32_t *outSize);
BAEResult PV_CreatePassthroughSndFromEncodedData(GM_Waveform const *decodedWaveform, XPTR encodedData, int32_t encodedSize, SndCompressionType compressionType, SndCompressionSubType compressionSubType, XPTR *outSndData, int32_t *outSndSize);
BAEResult PV_CreatePassthroughSndFromCompressedWaveform(GM_Waveform const *decodedWaveform, GM_Waveform const *compressedWaveform, SndCompressionSubType compressionSubType, XPTR *outSndData, int32_t *outSndSize);
BAEResult PV_AssignSongInfoString(SongResource_Info *songInfo, BAEInfoType infoType, char const *value);
BAEResult PV_PopulateSongResourceInfoFromDocument(BAERmfEditorDocument const *document, SongResource_Info *songInfo, XLongResourceID midiResourceID);
BAEResult PV_CreatePascalName(char const *source, char outName[256]);
BAEResult PV_ReadVLQ(unsigned char const *data, uint32_t dataSize, uint32_t *ioOffset, uint32_t *outValue);
unsigned char PV_ClampMidi7Bit(int32_t value);
BAERmfEditorTrack *PV_GetTrack(BAERmfEditorDocument *document, uint16_t trackIndex);
BAERmfEditorTrack const *PV_GetTrackConst(BAERmfEditorDocument const *document, uint16_t trackIndex);
BAEResult PV_AddNoteToTrack(BAERmfEditorTrack *track, uint32_t startTick, uint32_t durationTicks, unsigned char note, unsigned char velocity, unsigned char channel, uint16_t bank, unsigned char program, unsigned char noteOffStatus, unsigned char noteOffVelocity, uint32_t noteOnOrder, uint32_t noteOffOrder);
BAEResult PV_SetTrackName(BAERmfEditorTrack *track, char const *name);
BAEResult PV_ReadWholeFile(BAEPathName filePath, unsigned char **outData, uint32_t *outSize);
void PV_PeekSameTickBankProgram(unsigned char const *trackData, uint32_t trackSize, uint32_t peekOffset, unsigned char channel, unsigned char peekRunningStatus, uint16_t *bank, unsigned char *program);
BAERmfEditorActiveNote *PV_PushActiveNote(BAERmfEditorActiveNote **head, uint32_t startTick, uint32_t noteOnOrder, unsigned char channel, unsigned char note, unsigned char velocity, uint16_t bank, unsigned char program);
BAERmfEditorActiveNote *PV_PopActiveNote(BAERmfEditorActiveNote **head, unsigned char channel, unsigned char note);
void PV_DisposeActiveNotes(BAERmfEditorActiveNote **head);
BAEResult PV_FinalizeActiveNotes(BAERmfEditorTrack *track, BAERmfEditorActiveNote **activeNotes, uint32_t finalTick);
int PV_CompareChannelStateEvents(void const *left, void const *right);
BAEResult PV_ReconcileImportedMidiNotePrograms(BAERmfEditorDocument *document);
BAEResult PV_LoadMidiTrackIntoDocument(BAERmfEditorDocument *document, unsigned char const *trackData, uint32_t trackSize);
BAEResult PV_LoadMidiBytesIntoDocument(BAERmfEditorDocument *document, unsigned char const *data, uint32_t dataSize);
BAEResult PV_LoadMidiOrMthcBytesIntoDocument(BAERmfEditorDocument *document, unsigned char const *data, uint32_t dataSize);
void PV_DecodeResourceName(char const *rawName, char outName[256]);
BAEResult PV_GetEmbeddedSampleDisplayName(XFILE fileRef, XShortResourceID sndID, char outName[256]);
BAEResult PV_AddEmbeddedSampleVariant(BAERmfEditorDocument *document, XFILE fileRef, XLongResourceID instID, char const *displayName, unsigned char program, XShortResourceID sndID, unsigned char rootKey, unsigned char lowKey, unsigned char highKey);
BAEResult PV_AddBankAliasSample(BAERmfEditorDocument *document, XFILE fileRef, BAEBankToken bankToken, XLongResourceID instID, char const *displayName, unsigned char program, XShortResourceID sndID, unsigned char rootKey, unsigned char lowKey, unsigned char highKey);
BAERmfEditorInstrumentExt *PV_FindInstrumentExt(BAERmfEditorDocument *document, XLongResourceID instID);
BAEResult PV_AddInstrumentExt(BAERmfEditorDocument *document, BAERmfEditorInstrumentExt const *ext);
void PV_EnsureInstrumentExtForRemappedID(BAERmfEditorDocument *document, XLongResourceID oldInstID, XLongResourceID newInstID);
void PV_ClearInstrumentExts(BAERmfEditorDocument *document);
void PV_FillExtFromXInstrument(BAERmfEditorInstrumentExt *ext, XInstrumentData const *x);
void PV_FillXFromInstrumentExt(XInstrumentData *x, BAERmfEditorInstrumentExt const *ext);
void PV_ParseExtendedInstData(XPTR instData, int32_t instSize, BAERmfEditorInstrumentExt *ext);
XPTR PV_SerializeExtendedInstTail(BAERmfEditorInstrumentExt const *ext, int32_t *outSize);
bool PV_SndExistsInOriginalResources(BAERmfEditorDocument const *document, XShortResourceID sndID);
BAERmfEditorResourceEntry const *PV_FindOriginalResourceByTypeAndID(BAERmfEditorDocument const *document, XResourceType type, XLongResourceID id);
void PV_LoadEmbeddedSamplesFromRmf(BAERmfEditorDocument *document, XFILE fileRef);
XPTR PV_DecodeMidiData(XPTR raw, XResourceType rtype, int32_t *ioSize);
BAEResult PV_LoadRmfResourceIntoDocument(BAERmfEditorDocument *document, XFILE fileRef);
BAEResult PV_LoadRmfFileIntoDocument(BAERmfEditorDocument *document, BAEPathName filePath);
BAEResult PV_LoadRmfMemoryIntoDocument(BAERmfEditorDocument *document, void const *rmfData, uint32_t rmfSize);
BAEResult PV_GetAvailableResourceID(XFILE fileRef, XResourceType resourceType, XLongResourceID startingID, XLongResourceID *outResourceID);
BAEResult PV_EnsureResourceFileReady(XFILE fileRef, int32_t resourceID);
BAEResult PV_PrepareResourceFilePath(XFILENAME *name, int32_t resourceID);
BAEResult PV_WriteOriginalResources(BAERmfEditorDocument const *document, XFILE fileRef);
BAEResult PV_EncodeMidiForResourceType(XResourceType resourceType, ByteBuffer const *plainMidi, XPTR *outData, int32_t *outSize, bool isZmf);
BAEResult PV_EncodeMidiBestEffort(ByteBuffer const *plainMidi, XPTR *outData, int32_t *outSize, XResourceType *outUsedType, bool isZmf);
BAERmfEditorMidiStorageType PV_NormalizeMidiStorageType(BAERmfEditorMidiStorageType storageType);
BAEResult PV_EncodeMidiForStorageType(BAERmfEditorMidiStorageType storageType, ByteBuffer const *plainMidi, XPTR *outData, int32_t *outSize, XResourceType *outUsedType, bool isZmf);
bool PV_IsMidiResourceType(XResourceType resourceType);
int PV_CompareMidiEvents(void const *left, void const *right);
BAEResult PV_AppendMetaEvent(ByteBuffer *buffer, uint32_t delta, unsigned char type, void const *data, uint32_t length);
BAEResult PV_CompactMidiRunningStatus(ByteBuffer *trackData);
BAEResult PV_BuildTempoTrack(BAERmfEditorDocument *document, ByteBuffer *trackData);
BAEResult PV_BuildConductorTrack(BAERmfEditorDocument *document, BAERmfEditorTrack const *track, ByteBuffer *trackData);
bool PV_IsMetaOnlyConductorTrack(BAERmfEditorTrack const *track);
bool PV_TrackHasMetaType(BAERmfEditorTrack const *track, unsigned char type);
BAEResult PV_BuildTrackData(BAERmfEditorTrack const *track, ByteBuffer *trackData);
BAEResult PV_BuildMidiFile(BAERmfEditorDocument *document, ByteBuffer *output);
BAEResult PV_BuildMidiWithAppendedLoopTrack(BAERmfEditorDocument const *document, ByteBuffer *output);
#if USE_ZMF_SUPPORT == TRUE
BAEResult PV_AddSampleFreeInstrumentResources(BAERmfEditorDocument *document, XFILE fileRef);
#endif
BAEResult PV_AddSampleResources(BAERmfEditorDocument *document, XFILE fileRef, bool isZmf);
BAEResult PV_AddSongResource(BAERmfEditorDocument *document, XFILE fileRef, XLongResourceID midiResourceID);
BAEResult PV_AddSongResourceWithID(BAERmfEditorDocument *document, XFILE fileRef, XLongResourceID midiResourceID, XLongResourceID songID, unsigned char const *pascalName);
int PV_CompareAuxOrderRef(void const *left, void const *right);
void PV_ShiftTrackEventOrders(BAERmfEditorTrack *track, uint32_t startOrder, uint32_t delta);
BAEResult PV_InsertBankSelectBeforeAuxEvent(BAERmfEditorTrack *track, uint32_t auxIndex, unsigned char channel, uint16_t targetBank, bool insertMsb, bool insertLsb);
BAEResult PV_RemapTrackInstrumentReferences(BAERmfEditorTrack *track, uint16_t sourceBank, unsigned char sourceProgram, uint16_t targetBank, unsigned char targetProgram, uint16_t remapChannelMask, bool *outChanged);
uint16_t PV_RemapBankPreserveForm(uint16_t oldBank, uint16_t targetBank);
BAEResult PV_AddTrimHardStopEventsToTrack(BAERmfEditorTrack *track, uint32_t stopTick);
bool PV_IsBitstreamPassthroughCodec(SndCompressionType srcCodec);
BAEResult PV_ExportSndBitstreamToFile(XPTR sndData, int32_t sndSize, SndCompressionType srcCodec, BAEPathName filePath);
void PV_CopyEditorADSRToInfo(EditorADSR const *src, BAERmfEditorADSRInfo *dst);
void PV_CopyInfoToEditorADSR(BAERmfEditorADSRInfo const *src, EditorADSR *dst);
BAEResult PV_ResolveBankInstrumentIndexForGhost(BAEBankToken bankToken, uint32_t instID, uint32_t *outInstrumentIndex);
bool PV_ResolveGhostAliasSndID(BAEBankToken bankToken, uint32_t bankInstrumentIndex, uint32_t splitIndex, BAERmfEditorSample const *sample, XShortResourceID *outSndID);
BAEResult PV_ConvertDocumentSampleToBankAlias(BAERmfEditorSample *sample, BAEBankToken bankToken, XShortResourceID sndID);
BAEResult PV_HydrateDocumentSampleFromBankAlias(BAERmfEditorDocument *document, uint32_t sampleIndex, BAEBankToken bankToken);
BAEResult PV_CopySampleEntry(BAERmfEditorDocument *dest, BAERmfEditorSample const *srcSample);
bool PV_LfoDestinationIsLpf(int32_t destination);
bool PV_InstrumentUsesLpfFilter(int32_t lpfFrequency, int32_t lpfResonance, int32_t lpfLowpassAmount, bool hasFilterLfoDestination);
#if USE_ZMF_SUPPORT == TRUE
void PV_StampZmfMapVersionForOscillator(unsigned char *data, uint32_t size, bool needsV6);
bool PV_DocumentNeedsZmfV6(BAERmfEditorDocument const *document);
bool PV_BankNeedsZmfV6(BAEBankToken bankToken);
bool PV_DocumentHasOscillator(BAERmfEditorDocument const *document);
bool PV_BankHasOscillator(BAEBankToken bankToken);
#endif
BAEResult PV_BankReplaceInstResourceInPlace(XFILE bankFile, XLongResourceID instID, char const *pascalName, XPTR instData, int32_t instSize);
BAEResult PV_BankReplaceSndResourceInPlace(XFILE bankFile, XResourceType oldSndType, XResourceType newSndType, XShortResourceID sndID, char const *pascalName, XPTR sndData, int32_t sndSize);
BAEResult PV_BankCopyIndexedResourcesOfType(XFILE srcFile, XFILE dstFile, XResourceType resType);
BAEResult PV_BankReplaceMultipleSndResources(XFILE bankFile, PV_SndReplacement const *replacements, int32_t replacementCount);
BAEResult PV_BankReplaceResource(XFILE bankFile, XResourceType type, XLongResourceID id, char const *pascalName, XPTR data, int32_t size);
BAEResult PV_BankFindSndResource(XFILE bankFile, XShortResourceID sndID, XResourceType *outType, XPTR *outData, int32_t *outSize, char outName[256]);
BAEResult PV_BankRewrapSndForType(XFILE bankFile, XResourceType sndType, XPTR plainSnd, int32_t plainSndSize, XPTR *outWrapped, int32_t *outWrappedSize);
BAEResult PV_BankReplaceResourceEx(XFILE bankFile, XResourceType oldType, XResourceType newType, XLongResourceID id, char const *pascalName, XPTR data, int32_t size);
BAEResult PV_BankDeleteResource(XFILE bankFile, XResourceType type, XLongResourceID id);
bool PV_BankIdInList(XShortResourceID const *ids, uint32_t count, XLongResourceID id);
bool PV_BankFileHasValidSndResource(XFILE file, XShortResourceID sid);
uint32_t PV_BankCountSndReferences(XFILE bankFile, XShortResourceID sndID);
BAEResult PV_BankSaveToMemory(BAEBankToken bankToken,
                              int32_t overrideResourceID,
                              bool packForShip,
                              unsigned char **outData,
                              uint32_t *outSize);
bool PV_AddUsedInstrumentPair(PV_UsedInstrumentPair *pairs, uint32_t *pairCount, uint32_t pairCapacity, uint16_t bank, unsigned char program);
int PV_CompareUsedInstrumentPairs(void const *left, void const *right);
bool PV_AddUsedPercussion(PV_UsedPercussion *entries, uint32_t *entryCount, uint32_t entryCapacity, uint16_t bank, unsigned char note);
void PV_RemapPercussionNoteReferences(BAERmfEditorDocument *document, uint16_t sourceBank, unsigned char sourceNote, uint16_t targetBank, unsigned char targetProgram);
void PV_RemapPitchedNoteReferences(BAERmfEditorDocument *document, uint16_t sourceBank, unsigned char sourceProgram, uint16_t targetBank, unsigned char targetProgram);
uint16_t PV_BankGroupFromInternalBank(uint16_t internalBank);
void PV_BankToMidiMsbLsb(uint16_t internalBank, uint16_t *outMsb, uint16_t *outLsb);
bool PV_BanksEquivalent(uint16_t a, uint16_t b);
void PV_RemoveDocumentInstrumentAuxEvents(BAERmfEditorDocument *document);
void PV_SynchronizeTrackInstrumentDefaults(BAERmfEditorDocument *document);
BAEResult PV_VerifyEmbeddedOnlyInstrumentReferences(BAERmfEditorDocument const *document, uint16_t embeddedBank);
BAEResult PV_VerifyClonedInstrument(BAERmfEditorDocument const *document, BAEBankToken bankToken, uint32_t instrumentIndex, uint32_t targetInstID, uint32_t firstSampleIndex, uint32_t *outSampleCount);
void PV_AddCloneUsedMapping(BAERmfEditorCloneUsedResult *outResult, uint16_t sourceBank, unsigned char sourceProgram, bool isPercussion, uint32_t requestedInstID, uint32_t resolvedInstID, uint32_t targetInstID, uint32_t sampleCount, char const *resolvedName);
BAE_BOOL PV_ExportShouldEmbedInst(uint32_t resolvedInstID, BAE_BOOL embedAll, uint32_t const *embedResolvedInstIDs, uint32_t embedCount);
bool PV_DocumentHasEmbeddedInstID(BAERmfEditorDocument const *document, uint32_t instID);
BAEResult PV_PrepareUsedInstrumentsFromBank( BAERmfEditorDocument *document, BAEBankToken bankToken, BAE_BOOL embedAll, uint32_t const *embedResolvedInstIDs, uint32_t embedCount, BAERmfEditorCloneUsedResult *outResult);
BAEResult PV_AddRequiredAliases(BAERmfEditorDocument *document, XFILE fileRef, bool isZmf);
BAEResult PV_WriteRmfDocumentToResourceFile(BAERmfEditorDocument *document,
                                            XFILE fileRef,
                                            int32_t resourceID,
                                            bool preserveOriginalMidi,
                                            bool packForShip);
BAEResult PV_BankReEncodeSampleCore(XFILE bankFile, BAERmfEditorBankSampleInfo *pSampleInfo, void *waveData, uint32_t frameCount, uint16_t bitSize, uint16_t channels, BAE_UNSIGNED_FIXED sampleRate, BAERmfEditorCompressionType compressionType, BAERmfEditorSndStorageType sndStorageType, BAERmfEditorOpusMode opusMode, bool opusRoundTripResample, void **outPreviewPcm, uint32_t *outPreviewFrames, uint16_t *outPreviewBitSize, uint16_t *outPreviewChannels, BAE_UNSIGNED_FIXED *outPreviewRate, uint32_t *outEncodedBytes);

#ifdef __cplusplus
}
#endif

#endif /* BAE_EDITOR_API_INTERNAL_H */
