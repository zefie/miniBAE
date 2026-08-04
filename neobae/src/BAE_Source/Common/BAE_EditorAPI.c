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
**  BAE_EditorAPI.c
**
**  RMF / bank Creation & Editor API implementation.
**  Public declarations: BAE_EditorAPI.h (also included via NeoBAE.h).
**  Formerly BAERmfEditor.c.
*/
/*****************************************************************************/

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

#define BAE_RMF_EDITOR_SUBTYPE_TAG FOUR_CHAR('b','q','s','t')

static void PV_CopyStringBounded(char *dst, uint32_t dstSize, char const *src)
{
    uint32_t i;

    if (!dst || dstSize == 0)
    {
        return;
    }
    if (!src)
    {
        dst[0] = 0;
        return;
    }
    i = 0;
    while (i + 1 < dstSize && src[i] != 0)
    {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static bool PV_PathHasExtensionIgnoreCase(char const *filePath, char const *ext)
{
    char const *dot;
    uint32_t i;

    if (!filePath || !ext)
    {
        return FALSE;
    }
    dot = strrchr(filePath, '.');
    if (!dot)
    {
        return FALSE;
    }

    i = 0;
    while (dot[i] != 0 && ext[i] != 0)
    {
        char a = dot[i];
        char b = ext[i];

        if (a >= 'A' && a <= 'Z')
        {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z')
        {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b)
        {
            return FALSE;
        }
        i++;
    }
    return (dot[i] == 0 && ext[i] == 0) ? TRUE : FALSE;
}

static BAEFileType PV_DetectOggCodecBySignature(BAEPathName filePath)
{
    XFILENAME fileName;
    XFILE file;
    int32_t fileSize;
    int32_t probeSize;
    unsigned char probe[4096];
    int32_t i;

    if (!filePath)
    {
        return BAE_INVALID_TYPE;
    }

    XConvertPathToXFILENAME(filePath, &fileName);
    file = XFileOpenForRead(&fileName);
    if (!file)
    {
        return BAE_INVALID_TYPE;
    }

    fileSize = XFileGetLength(file);
    if (fileSize <= 0)
    {
        XFileClose(file);
        return BAE_INVALID_TYPE;
    }

    probeSize = fileSize;
    if (probeSize > (int32_t)sizeof(probe))
    {
        probeSize = (int32_t)sizeof(probe);
    }

    if (XFileSetPosition(file, 0) != 0 || XFileRead(file, (XPTR)probe, probeSize) != 0)
    {
        XFileClose(file);
        return BAE_INVALID_TYPE;
    }
    XFileClose(file);

#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
    for (i = 0; i + 8 <= probeSize; ++i)
    {
        if (memcmp(&probe[i], "OpusHead", 8) == 0)
        {
            return BAE_OPUS_TYPE;
        }
    }
#endif

#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
    for (i = 0; i + 7 <= probeSize; ++i)
    {
        if (probe[i] == 0x01 && memcmp(&probe[i + 1], "vorbis", 6) == 0)
        {
            return BAE_VORBIS_TYPE;
        }
    }
    for (i = 0; i + 6 <= probeSize; ++i)
    {
        if (memcmp(&probe[i], "vorbis", 6) == 0)
        {
            return BAE_VORBIS_TYPE;
        }
    }
#endif

    return BAE_INVALID_TYPE;
}

static BAEFileType PV_DetermineEditorImportFileType(BAEPathName filePath)
{
    BAEFileType fileType;

    fileType = X_DetermineFileType(filePath);
    if (fileType != BAE_INVALID_TYPE)
    {
        return fileType;
    }

    fileType = PV_DetermineEditorImportFileType(filePath);
    if (fileType != BAE_INVALID_TYPE)
    {
        return fileType;
    }

    if (PV_PathHasExtensionIgnoreCase(filePath, ".ogg") ||
        PV_PathHasExtensionIgnoreCase(filePath, ".oga"))
    {
        fileType = PV_DetectOggCodecBySignature(filePath);
        if (fileType != BAE_INVALID_TYPE)
        {
            return fileType;
        }
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
        return BAE_VORBIS_TYPE;
#elif USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        return BAE_OPUS_TYPE;
#endif
    }

    return BAE_INVALID_TYPE;
}

static BAEFileType PV_DetermineEditorImportMemoryFileType(void const *data,
                                                           uint32_t dataSize,
                                                           BAEFileType fileTypeHint)
{
    unsigned char const *bytes;

    if (fileTypeHint != BAE_INVALID_TYPE)
    {
        return fileTypeHint;
    }
    if (!data || dataSize < 4)
    {
        return BAE_INVALID_TYPE;
    }

    bytes = (unsigned char const *)data;
    if (bytes[0] == 'M' && bytes[1] == 'T' && bytes[2] == 'h' && bytes[3] == 'd')
    {
        return BAE_MIDI_TYPE;
    }
#if USE_MTHC_SUPPORT == TRUE
    if (bytes[0] == 'M' && bytes[1] == 'T' && bytes[2] == 'h' && bytes[3] == 'c')
    {
        return BAE_MTHC;
    }
#endif
    if ((bytes[0] == 'I' && bytes[1] == 'R' && bytes[2] == 'E' && bytes[3] == 'Z') ||
        (bytes[0] == 'Z' && bytes[1] == 'R' && bytes[2] == 'E' && bytes[3] == 'Z'))
    {
        return BAE_RMF;
    }
    if (dataSize >= 12 &&
        bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
        bytes[8] == 'R' && bytes[9] == 'M' && bytes[10] == 'I' && bytes[11] == 'D')
    {
        return BAE_RMI;
    }
    return BAE_INVALID_TYPE;
}

static uint32_t PV_GetStoredCompressionSubTypeFromSnd(XPTR sndData,
                                                       int32_t sndSize,
                                                       uint32_t compressionType)
{
    XSndHeader3 const *header3;
    uint32_t marker;
    uint32_t subType;

    if (!sndData || sndSize < (int32_t)sizeof(XSndHeader3))
    {
        return (uint32_t)CS_DEFAULT;
    }
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
    if (compressionType != (uint32_t)C_VORBIS
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        && compressionType != (uint32_t)C_OPUS
#endif
    )
    {
        return (uint32_t)CS_DEFAULT;
    }
#endif    
    header3 = (XSndHeader3 const *)sndData;
    if (XGetShort(&header3->type) != XThirdSoundFormat)
    {
        return (uint32_t)CS_DEFAULT;
    }
    if ((uint32_t)XGetLong(&header3->sndBuffer.subType) != (uint32_t)compressionType)
    {
        return (uint32_t)CS_DEFAULT;
    }

    marker = (uint32_t)XGetLong(&header3->sndBuffer.reserved3[0]);
    if (marker != (uint32_t)BAE_RMF_EDITOR_SUBTYPE_TAG)
    {
        return (uint32_t)CS_DEFAULT;
    }

    subType = (uint32_t)XGetLong(&header3->sndBuffer.reserved3[1]);
    switch ((SndCompressionSubType)subType)
    {
        case CS_VORBIS_32K:
        case CS_VORBIS_48K:
        case CS_VORBIS_64K:
        case CS_VORBIS_80K:
        case CS_VORBIS_96K:
        case CS_VORBIS_128K:
        case CS_VORBIS_160K:
        case CS_VORBIS_192K:
        case CS_VORBIS_256K:
        case CS_OPUS_12K:
        case CS_OPUS_16K:
        case CS_OPUS_24K:
        case CS_OPUS_32K:
        case CS_OPUS_48K:
        case CS_OPUS_64K:
        case CS_OPUS_80K:
        case CS_OPUS_96K:
        case CS_OPUS_128K:
        case CS_OPUS_160K:
        case CS_OPUS_192K:
        case CS_OPUS_256K:
            return subType;
        default:
            break;
    }
    return (uint32_t)CS_DEFAULT;
}

static void PV_StoreCompressionSubTypeInSnd(XPTR sndData,
                                            int32_t sndSize,
                                            SndCompressionType compressionType,
                                            SndCompressionSubType compressionSubType)
{
    XSndHeader3 *header3;

    if (!sndData || sndSize < (int32_t)sizeof(XSndHeader3))
    {
        return;
    }
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
    if (compressionType != C_VORBIS
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        && compressionType != C_OPUS
#endif
    )
    {
        return;
    }
#endif
    header3 = (XSndHeader3 *)sndData;
    if (XGetShort(&header3->type) != XThirdSoundFormat)
    {
        return;
    }
    if ((uint32_t)XGetLong(&header3->sndBuffer.subType) != (uint32_t)compressionType)
    {
        return;
    }

    XPutLong(&header3->sndBuffer.reserved3[0], (uint32_t)BAE_RMF_EDITOR_SUBTYPE_TAG);
    XPutLong(&header3->sndBuffer.reserved3[1], (uint32_t)compressionSubType);
}

static bool PV_IsValidEditorOpusMode(BAERmfEditorOpusMode opusMode)
{
    return (opusMode == BAE_EDITOR_OPUS_MODE_AUDIO ||
            opusMode == BAE_EDITOR_OPUS_MODE_VOICE) ? TRUE : FALSE;
}

/* Map CS_OPUS_*K FourCC -> small index that fits in 16 bits for transport via
 * SndCompressionSubType.  The CS_OPUS_*K constants are 32-bit FourCCs and
 * cannot be packed into 16 bits directly.  Both PV_ComposeOpusEncodeSubType
 * and the matching switch in SampleTools.c must use this same mapping. */
static uint32_t PV_SubTypeToOpusBitrateIndex(SndCompressionSubType subType)
{
    switch (subType)
    {
        case CS_OPUS_12K:  return 0;
        case CS_OPUS_16K:  return 1;
        case CS_OPUS_24K:  return 2;
        case CS_OPUS_32K:  return 3;
        case CS_OPUS_48K:  return 4;
        case CS_OPUS_64K:  return 5;
        case CS_OPUS_80K:  return 6;
        case CS_OPUS_96K:  return 7;
        case CS_OPUS_128K: return 8;
        case CS_OPUS_160K: return 9;
        case CS_OPUS_192K: return 10;
        case CS_OPUS_256K: return 11;
        default:           return 7;
    }
}

static SndCompressionSubType PV_ComposeOpusEncodeSubType(SndCompressionSubType baseSubType,
                                                         BAERmfEditorOpusMode opusMode)
{
    uint32_t packed;

    /* Low 16 bits: bitrate index (0-11); high 16 bits: opus mode (0-2). */
    packed = PV_SubTypeToOpusBitrateIndex(baseSubType) & 0xFFFFU;
    packed |= ((uint32_t)(PV_IsValidEditorOpusMode(opusMode) ? opusMode : BAE_EDITOR_OPUS_MODE_AUDIO) & 0xFFFFU) << 16;
    return (SndCompressionSubType)packed;
}

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
    uint32_t sampleAssetID;              /* shared audio asset identifier */
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

static uint32_t PV_AllocateSampleAssetID(BAERmfEditorDocument *document)
{
    uint32_t newID;
    uint32_t i;

    if (!document)
    {
        return 0;
    }
    if (document->nextSampleAssetID == 0)
    {
        document->nextSampleAssetID = 1;
    }
    newID = document->nextSampleAssetID;

    /* Some legacy INST entries use sndResourceID=0. In that case we synthesize
       an internal asset ID, but it must never collide with real SND IDs present
       in the loaded resource map (or already assigned sample assets), otherwise
       unrelated instruments become grouped under the same asset in the editor. */
    for (;;)
    {
        bool reserved;

        reserved = FALSE;
        for (i = 0; i < document->originalResourceCount; ++i)
        {
            XResourceType type;

            type = document->originalResources[i].type;
            if ((type == ID_ESND || type == ID_CSND || type == ID_SND) &&
                (uint32_t)document->originalResources[i].id == newID)
            {
                reserved = TRUE;
                break;
            }
        }
        if (!reserved)
        {
            for (i = 0; i < document->sampleCount; ++i)
            {
                if (document->samples[i].sampleAssetID == newID)
                {
                    reserved = TRUE;
                    break;
                }
            }
        }
        if (!reserved)
        {
            break;
        }
        newID++;
        if (newID == 0)
        {
            return 0;
        }
    }

    document->nextSampleAssetID = newID + 1;
    return newID;
}

static void PV_NoteSampleAssetID(BAERmfEditorDocument *document, uint32_t assetID)
{
    if (!document || assetID == 0)
    {
        return;
    }
    if (document->nextSampleAssetID <= assetID)
    {
        document->nextSampleAssetID = assetID + 1;
    }
}

static BAERmfEditorSample *PV_FindFirstSampleForAsset(BAERmfEditorDocument *document, uint32_t assetID)
{
    uint32_t i;

    if (!document || assetID == 0)
    {
        return NULL;
    }
    for (i = 0; i < document->sampleCount; ++i)
    {
        if (document->samples[i].sampleAssetID == assetID)
        {
            return &document->samples[i];
        }
    }
    return NULL;
}

static uint32_t PV_CountSamplesForAsset(BAERmfEditorDocument const *document, uint32_t assetID)
{
    uint32_t i;
    uint32_t count;

    if (!document || assetID == 0)
    {
        return 0;
    }
    count = 0;
    for (i = 0; i < document->sampleCount; ++i)
    {
        if (document->samples[i].sampleAssetID == assetID)
        {
            ++count;
        }
    }
    return count;
}

static bool PV_IsOpusCompression(BAERmfEditorCompressionType ct)
{
    switch (ct)
    {
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
            return TRUE;
        default:
            return FALSE;
    }
}

static bool PV_AssetSupportsDontChange(BAERmfEditorDocument const *document, uint32_t assetID)
{
    uint32_t i;
    bool sawAny;

    if (!document || assetID == 0)
    {
        return FALSE;
    }
    sawAny = FALSE;
    for (i = 0; i < document->sampleCount; ++i)
    {
        BAERmfEditorSample const *sample;

        sample = &document->samples[i];
        if (sample->sampleAssetID != assetID)
        {
            continue;
        }
        sawAny = TRUE;
        if (!sample->originalSndData)
        {
            return FALSE;
        }
    }
    return sawAny;
}

static BAE_UNSIGNED_FIXED PV_NormalizeSampleRateForSave(BAE_UNSIGNED_FIXED sampleRate)
{
    if (sampleRate < (1000U << 16))
    {
        if (sampleRate >= 1000U && sampleRate <= 384000U)
        {
            return sampleRate << 16;
        }
        return (44100U << 16);
    }
    return sampleRate;
}

static bool PV_IsMpegCompression(BAERmfEditorCompressionType ct)
{
    switch (ct)
    {
        case BAE_EDITOR_COMPRESSION_MP3_32K:
        case BAE_EDITOR_COMPRESSION_MP3_48K:
        case BAE_EDITOR_COMPRESSION_MP3_64K:
        case BAE_EDITOR_COMPRESSION_MP3_96K:
        case BAE_EDITOR_COMPRESSION_MP3_128K:
        case BAE_EDITOR_COMPRESSION_MP3_192K:
        case BAE_EDITOR_COMPRESSION_MP3_256K:
        case BAE_EDITOR_COMPRESSION_MP3_320K:
            return TRUE;
        default:
            return FALSE;
    }
}

static uint32_t PV_ExtractOpusInputRateFromOriginalSnd(BAERmfEditorSample const *sample)
{
    XSndHeader3 const *hdr3;
    int32_t bitstreamSize;
    unsigned char const *bitstream;
    unsigned char const *bitstreamEnd;
    unsigned char const *p;

    if (!sample || !sample->originalSndData || sample->originalSndSize <= (int32_t)sizeof(XSndHeader3))
    {
        return 0;
    }

    hdr3 = (XSndHeader3 const *)sample->originalSndData;
    bitstreamSize = XGetLong(&hdr3->sndBuffer.encodedBytes);
    bitstream = (unsigned char const *)&hdr3->sndBuffer.sampleArea[0];
    bitstreamEnd = (unsigned char const *)sample->originalSndData + sample->originalSndSize;
    if (bitstreamSize > 0 && bitstream + bitstreamSize <= bitstreamEnd)
    {
        for (p = bitstream; p + 19 <= bitstream + bitstreamSize; ++p)
        {
            if (memcmp(p, "OpusHead", 8) == 0)
            {
                uint32_t hz;
                hz = PV_ReadLE32(p + 12);
                if (hz >= 1000U && hz <= 384000U)
                {
                    return hz;
                }
                return 0;
            }
        }
    }

    /* Fallback: some legacy SND wrappers can report encodedBytes inconsistently.
     * Scan the full stored blob for OpusHead so we can still recover original Hz. */
    for (p = (unsigned char const *)sample->originalSndData;
         p + 19 <= (unsigned char const *)sample->originalSndData + sample->originalSndSize;
         ++p)
    {
        if (memcmp(p, "OpusHead", 8) == 0)
        {
            uint32_t hz;
            hz = PV_ReadLE32(p + 12);
            if (hz >= 1000U && hz <= 384000U)
            {
                return hz;
            }
            return 0;
        }
    }
    return 0;
}

static uint32_t PV_SampleRateFixedToHz(BAE_UNSIGNED_FIXED fixedRate)
{
    fixedRate = PV_NormalizeSampleRateForSave(fixedRate);
    return (uint32_t)(fixedRate >> 16);
}

static uint32_t PV_ChooseUpscaledRateFromTable(uint32_t sourceHz,
                                               uint32_t const *table,
                                               uint32_t count)
{
    uint32_t i;

    if (sourceHz == 0)
    {
        sourceHz = 44100;
    }
    for (i = 0; i < count; ++i)
    {
        if (sourceHz <= table[i])
        {
            return table[i];
        }
    }
    return table[count - 1];
}

static BAE_UNSIGNED_FIXED PV_ChooseCodecRateFromSourceHz(BAERmfEditorCompressionType compressionType,
                                                          uint32_t sourceHz)
{
    static uint32_t const kMpegRatesHz[] = { 8000, 11025, 12000, 16000, 22050, 32000, 44100, 48000 };

    if (PV_IsOpusCompression(compressionType))
    {
        if (sourceHz <= 8000U)  return (8000U << 16);
        if (sourceHz <= 12000U) return (12000U << 16);
        if (sourceHz <= 16000U) return (16000U << 16);
        if (sourceHz <= 24000U) return (24000U << 16);
        return (48000U << 16);
    }
    if (PV_IsMpegCompression(compressionType))
    {
        uint32_t chosen = PV_ChooseUpscaledRateFromTable(sourceHz,
                                                         kMpegRatesHz,
                                                         (uint32_t)(sizeof(kMpegRatesHz) / sizeof(kMpegRatesHz[0])));
        return (BAE_UNSIGNED_FIXED)(chosen << 16);
    }

    if (sourceHz == 0)
    {
        sourceHz = 44100;
    }
    return (BAE_UNSIGNED_FIXED)(sourceHz << 16);
}

static BAE_UNSIGNED_FIXED PV_RecommendSampleRateForCompression(BAERmfEditorSample const *sample,
                                                               BAERmfEditorCompressionType compressionType)
{
    uint32_t sourceHz;

    if (!sample)
    {
        return (44100U << 16);
    }

    sourceHz = 0;
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE    
    if (sample->sourceCompressionType == (uint32_t)C_OPUS)
    {
        sourceHz = PV_ExtractOpusInputRateFromOriginalSnd(sample);
    }
 #endif    
    /* For live editor sessions, waveform carries the true source domain for
     * uncompressed imports and recent edits. Prefer it when available. */
    if (sourceHz == 0 && sample->waveform)
    {
        sourceHz = PV_SampleRateFixedToHz((BAE_UNSIGNED_FIXED)sample->waveform->sampledRate);
    }

    if (sourceHz == 0)
    {
        sourceHz = PV_SampleRateFixedToHz(sample->sampleInfo.sampledRate);
    }
    return PV_ChooseCodecRateFromSourceHz(compressionType, sourceHz);
}

static BAEResult PV_ResampleWaveformLinear(GM_Waveform *waveform,
                                           BAE_UNSIGNED_FIXED targetRate,
                                           XPTR *ioWaveDataOwner)
{
    BAE_UNSIGNED_FIXED sourceRate;
    uint32_t srcRateHz;
    uint32_t dstRateHz;
    uint32_t channels;
    uint32_t srcFrames;
    uint32_t dstFrames;
    uint32_t dstBytes;
    XPTR resampledData;

    if (!waveform || !waveform->theWaveform || !ioWaveDataOwner)
    {
        return BAE_PARAM_ERR;
    }

    sourceRate = PV_NormalizeSampleRateForSave((BAE_UNSIGNED_FIXED)waveform->sampledRate);
    targetRate = PV_NormalizeSampleRateForSave(targetRate);
    if (sourceRate == 0 || targetRate == 0)
    {
        return BAE_PARAM_ERR;
    }
    if (sourceRate == targetRate)
    {
        waveform->sampledRate = (int32_t)targetRate;
        return BAE_NO_ERROR;
    }
    if ((waveform->bitSize != 8 && waveform->bitSize != 16) ||
        (waveform->channels != 1 && waveform->channels != 2) ||
        waveform->waveFrames == 0)
    {
        return BAE_UNSUPPORTED_FORMAT;
    }

    srcRateHz = (uint32_t)(sourceRate >> 16);
    dstRateHz = (uint32_t)(targetRate >> 16);
    if (srcRateHz == 0 || dstRateHz == 0)
    {
        return BAE_PARAM_ERR;
    }

    srcFrames = waveform->waveFrames;
    channels = waveform->channels;
    dstFrames = (uint32_t)((((uint64_t)srcFrames * (uint64_t)dstRateHz) + ((uint64_t)srcRateHz / 2ULL)) /
                           (uint64_t)srcRateHz);
    if (dstFrames == 0)
    {
        dstFrames = 1;
    }

    dstBytes = dstFrames * channels * (uint32_t)(waveform->bitSize / 8);
    resampledData = XNewPtr((int32_t)dstBytes);
    if (!resampledData)
    {
        return BAE_MEMORY_ERR;
    }

    if (waveform->bitSize == 16)
    {
        int16_t const *srcPcm;
        int16_t *dstPcm;
        double step;
        double pos;
        uint32_t frameIndex;

        srcPcm = (int16_t const *)waveform->theWaveform;
        dstPcm = (int16_t *)resampledData;
        step = (double)srcRateHz / (double)dstRateHz;
        pos = 0.0;
        for (frameIndex = 0; frameIndex < dstFrames; ++frameIndex)
        {
            uint32_t srcIndex = (uint32_t)pos;
            double frac = pos - (double)srcIndex;
            uint32_t channelIndex;

            if (srcIndex + 1 < srcFrames)
            {
                for (channelIndex = 0; channelIndex < channels; ++channelIndex)
                {
                    double s0 = (double)srcPcm[srcIndex * channels + channelIndex];
                    double s1 = (double)srcPcm[(srcIndex + 1) * channels + channelIndex];
                    double value = s0 + (s1 - s0) * frac;
                    if (value > 32767.0) value = 32767.0;
                    if (value < -32768.0) value = -32768.0;
                    dstPcm[frameIndex * channels + channelIndex] = (int16_t)value;
                }
            }
            else
            {
                for (channelIndex = 0; channelIndex < channels; ++channelIndex)
                {
                    dstPcm[frameIndex * channels + channelIndex] =
                        (srcIndex < srcFrames) ? srcPcm[srcIndex * channels + channelIndex] : 0;
                }
            }
            pos += step;
        }
    }
    else
    {
        unsigned char const *srcPcm;
        unsigned char *dstPcm;
        double step;
        double pos;
        uint32_t frameIndex;

        srcPcm = (unsigned char const *)waveform->theWaveform;
        dstPcm = (unsigned char *)resampledData;
        step = (double)srcRateHz / (double)dstRateHz;
        pos = 0.0;
        for (frameIndex = 0; frameIndex < dstFrames; ++frameIndex)
        {
            uint32_t srcIndex = (uint32_t)pos;
            double frac = pos - (double)srcIndex;
            uint32_t channelIndex;

            if (srcIndex + 1 < srcFrames)
            {
                for (channelIndex = 0; channelIndex < channels; ++channelIndex)
                {
                    double s0 = (double)srcPcm[srcIndex * channels + channelIndex];
                    double s1 = (double)srcPcm[(srcIndex + 1) * channels + channelIndex];
                    double value = s0 + (s1 - s0) * frac;
                    if (value > 255.0) value = 255.0;
                    if (value < 0.0) value = 0.0;
                    dstPcm[frameIndex * channels + channelIndex] = (unsigned char)value;
                }
            }
            else
            {
                for (channelIndex = 0; channelIndex < channels; ++channelIndex)
                {
                    dstPcm[frameIndex * channels + channelIndex] =
                        (srcIndex < srcFrames) ? srcPcm[srcIndex * channels + channelIndex] : 128;
                }
            }
            pos += step;
        }
    }

    if (waveform->startLoop != 0 || waveform->endLoop != 0)
    {
        waveform->startLoop = (uint32_t)((((uint64_t)waveform->startLoop * (uint64_t)dstRateHz) +
                                          ((uint64_t)srcRateHz / 2ULL)) /
                                         (uint64_t)srcRateHz);
        waveform->endLoop = (uint32_t)((((uint64_t)waveform->endLoop * (uint64_t)dstRateHz) +
                                        ((uint64_t)srcRateHz / 2ULL)) /
                                       (uint64_t)srcRateHz);
        if (waveform->endLoop > dstFrames)
        {
            waveform->endLoop = dstFrames;
        }
        if (waveform->startLoop >= waveform->endLoop)
        {
            waveform->startLoop = 0;
            waveform->endLoop = 0;
        }
    }

    if (*ioWaveDataOwner)
    {
        XDisposePtr(*ioWaveDataOwner);
    }
    *ioWaveDataOwner = resampledData;
    waveform->theWaveform = resampledData;
    waveform->waveFrames = dstFrames;
    waveform->waveSize = (int32_t)dstBytes;
    waveform->sampledRate = (int32_t)targetRate;
    return BAE_NO_ERROR;
}

static BAEResult PV_EnsureWaveformDataOwned(GM_Waveform *waveform, XPTR *ioWaveDataOwner)
{
    uint32_t bytes;
    XPTR copy;

    if (!waveform || !waveform->theWaveform || !ioWaveDataOwner)
    {
        return BAE_PARAM_ERR;
    }

    if (*ioWaveDataOwner == waveform->theWaveform)
    {
        return BAE_NO_ERROR;
    }

    bytes = waveform->waveFrames * waveform->channels * (uint32_t)(waveform->bitSize / 8);
    if (bytes == 0)
    {
        return BAE_PARAM_ERR;
    }

    copy = XNewPtr((int32_t)bytes);
    if (!copy)
    {
        return BAE_MEMORY_ERR;
    }
    XBlockMove(waveform->theWaveform, copy, (int32_t)bytes);

    if (*ioWaveDataOwner)
    {
        XDisposePtr(*ioWaveDataOwner);
    }
    *ioWaveDataOwner = copy;
    waveform->theWaveform = (signed char *)copy;
    waveform->waveSize = (int32_t)bytes;
    return BAE_NO_ERROR;
}

static BAEResult PV_ApplyOpusLoopSeamMicroFade(GM_Waveform *waveform,
                                               XPTR *ioWaveDataOwner,
                                               uint32_t sampleIndex)
{
    uint32_t loopStart;
    uint32_t loopEnd;
    uint32_t loopSpan;
    uint32_t seamFrames;
    uint32_t padFrames;
    uint32_t maxDelta;
    uint32_t i;
    uint32_t ch;
    uint32_t bytesPerFrame;
    uint32_t newTotalFrames;
    uint32_t newTotalBytes;
    XPTR newData;

    if (!waveform || !waveform->theWaveform || !ioWaveDataOwner)
    {
        return BAE_PARAM_ERR;
    }
    if ((waveform->bitSize != 8 && waveform->bitSize != 16) ||
        (waveform->channels != 1 && waveform->channels != 2) ||
        waveform->waveFrames < 64)
    {
        return BAE_NO_ERROR;
    }

    loopStart = waveform->startLoop;
    loopEnd = waveform->endLoop;
    if (loopEnd > waveform->waveFrames)
    {
        loopEnd = waveform->waveFrames;
    }
    if (loopEnd <= loopStart)
    {
        return BAE_NO_ERROR;
    }
    loopSpan = loopEnd - loopStart;
    if (loopSpan < 64)
    {
        return BAE_NO_ERROR;
    }

    bytesPerFrame = (uint32_t)(waveform->bitSize / 8) * waveform->channels;

    /* --- Step 1: Crossfade the tail of the loop toward the head ---------- */
    maxDelta = 0;
    if (waveform->bitSize == 16)
    {
        int16_t const *pcm16;
        pcm16 = (int16_t const *)waveform->theWaveform;
        for (ch = 0; ch < waveform->channels; ++ch)
        {
            int32_t tail = pcm16[((loopEnd - 1) * waveform->channels) + ch];
            int32_t head = pcm16[(loopStart * waveform->channels) + ch];
            uint32_t delta = (tail >= head) ? (uint32_t)(tail - head) : (uint32_t)(head - tail);
            if (delta > maxDelta)
            {
                maxDelta = delta;
            }
        }
    }
    else
    {
        unsigned char const *pcm8;
        pcm8 = (unsigned char const *)waveform->theWaveform;
        for (ch = 0; ch < waveform->channels; ++ch)
        {
            int32_t tail = (int32_t)pcm8[((loopEnd - 1) * waveform->channels) + ch] - 128;
            int32_t head = (int32_t)pcm8[(loopStart * waveform->channels) + ch] - 128;
            uint32_t delta = (tail >= head) ? (uint32_t)(tail - head) : (uint32_t)(head - tail);
            if (delta > maxDelta)
            {
                maxDelta = delta;
            }
        }
    }

    seamFrames = loopSpan / 64;
    if (seamFrames < 64) seamFrames = 64;
    if (seamFrames > 480) seamFrames = 480;
    if (seamFrames >= loopSpan) seamFrames = loopSpan - 1;

    /* --- Step 2: Append loop-start audio after loopEnd ------------------- *
     * Opus MDCT uses a ~960 frame transform window.  The decoded output at
     * loopEnd is influenced by audio AFTER loopEnd in the encoded stream.
     * During playback the engine jumps back to loopStart, but the decoder saw
     * whatever happened to follow loopEnd (silence, post-loop tail, etc.).
     * By appending a copy of the loop-start region after loopEnd we give the
     * encoder the same context the listener hears on loop restart, so the
     * decoded values at loopEnd transition cleanly into loopStart. */
    padFrames = 480;
    if (padFrames > loopSpan) padFrames = loopSpan;

    newTotalFrames = waveform->waveFrames + padFrames;
    newTotalBytes = newTotalFrames * bytesPerFrame;
    newData = XNewPtr((int32_t)newTotalBytes);
    if (!newData)
    {
        return BAE_MEMORY_ERR;
    }

    /* Copy original PCM */
    XBlockMove(waveform->theWaveform, newData, (int32_t)(waveform->waveFrames * bytesPerFrame));

    /* Append loop-start content after the original data */
    {
        uint32_t srcOffset = loopStart * bytesPerFrame;
        uint32_t dstOffset = waveform->waveFrames * bytesPerFrame;
        XBlockMove((char *)waveform->theWaveform + srcOffset,
                   (char *)newData + dstOffset,
                   (int32_t)(padFrames * bytesPerFrame));
    }

    if (*ioWaveDataOwner)
    {
        XDisposePtr(*ioWaveDataOwner);
    }
    *ioWaveDataOwner = newData;
    waveform->theWaveform = (signed char *)newData;
    waveform->waveFrames = newTotalFrames;
    waveform->waveSize = (int32_t)newTotalBytes;

    /* Apply the crossfade on the owned buffer */
    if (maxDelta >= (waveform->bitSize == 16 ? 2048U : 16U))
    {
        if (waveform->bitSize == 16)
        {
            int16_t *pcm16;
            pcm16 = (int16_t *)waveform->theWaveform;
            for (i = 0; i < seamFrames; ++i)
            {
                uint32_t tailFrame = (loopEnd - seamFrames) + i;
                uint32_t headFrame = loopStart + i;
                uint32_t wHead = i + 1;
                uint32_t wTail = seamFrames - i;
                uint32_t wSum = wHead + wTail;

                for (ch = 0; ch < waveform->channels; ++ch)
                {
                    int32_t tailS = pcm16[(tailFrame * waveform->channels) + ch];
                    int32_t headS = pcm16[(headFrame * waveform->channels) + ch];
                    int32_t mixed = (tailS * (int32_t)wTail + headS * (int32_t)wHead) / (int32_t)wSum;
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    pcm16[(tailFrame * waveform->channels) + ch] = (int16_t)mixed;
                }
            }
        }
        else
        {
            unsigned char *pcm8;
            pcm8 = (unsigned char *)waveform->theWaveform;
            for (i = 0; i < seamFrames; ++i)
            {
                uint32_t tailFrame = (loopEnd - seamFrames) + i;
                uint32_t headFrame = loopStart + i;
                uint32_t wHead = i + 1;
                uint32_t wTail = seamFrames - i;
                uint32_t wSum = wHead + wTail;

                for (ch = 0; ch < waveform->channels; ++ch)
                {
                    int32_t tailS = (int32_t)pcm8[(tailFrame * waveform->channels) + ch] - 128;
                    int32_t headS = (int32_t)pcm8[(headFrame * waveform->channels) + ch] - 128;
                    int32_t mixed = (tailS * (int32_t)wTail + headS * (int32_t)wHead) / (int32_t)wSum;
                    int32_t out = mixed + 128;
                    if (out > 255) out = 255;
                    if (out < 0) out = 0;
                    pcm8[(tailFrame * waveform->channels) + ch] = (unsigned char)out;
                }
            }
        }
    }

    debug_message("[RMF Save] Sample[%u] Opus loop seam: crossfade=%u pad=%u delta=%u\n",
               (unsigned)sampleIndex,
               (unsigned)seamFrames,
               (unsigned)padFrames,
               (unsigned)maxDelta);
    return BAE_NO_ERROR;
}

/* Some encoded SND variants can report unusual channel metadata at write time.
 * Always stamp loop points directly in the SND payload so split-specific loops
 * survive regardless of which helper path produced the resource blob. */
static void PV_ForceSndLoopPoints(XPTR sndResource, int32_t loopStart, int32_t loopEnd)
{
    XSndHeader3 *snd;
    uint16_t channels;
    uint16_t ch;

    if (!sndResource)
    {
        return;
    }

    snd = (XSndHeader3 *)sndResource;
    if (XGetShort(&snd->type) != XThirdSoundFormat)
    {
        return;
    }

    channels = snd->sndBuffer.channels;
    if (channels == 0)
    {
        channels = 1;
    }
    if (channels > 2)
    {
        channels = 2;
    }
    for (ch = 0; ch < channels; ++ch)
    {
        XPutLong(&snd->sndBuffer.loopStart[ch], (uint32_t)loopStart);
        XPutLong(&snd->sndBuffer.loopEnd[ch], (uint32_t)loopEnd);
    }
}

static void PV_ForceSndDecodedFrameCount(XPTR sndResource, uint32_t frameCount)
{
    XSndHeader3 *snd;
    uint32_t bytesPerFrame;

    if (!sndResource)
    {
        return;
    }

    snd = (XSndHeader3 *)sndResource;
    if (XGetShort(&snd->type) != XThirdSoundFormat)
    {
        return;
    }

    XPutLong(&snd->sndBuffer.frameCount, frameCount);

    bytesPerFrame = (uint32_t)snd->sndBuffer.channels * ((uint32_t)snd->sndBuffer.bitSize / 8U);
    if (bytesPerFrame > 0)
    {
        XPutLong(&snd->sndBuffer.decodedBytes, frameCount * bytesPerFrame);
    }
}

/* Lossy encoders can alter decoded frame count (resampling, padding/trimming).
 * Keep loop points valid by mapping them to the encoded stream's frame domain. */
static void PV_RemapLoopPointsToFrameCount(uint32_t sourceFrames,
                                           uint32_t targetFrames,
                                           int32_t *ioLoopStart,
                                           int32_t *ioLoopEnd)
{
    int32_t loopStart;
    int32_t loopEnd;

    if (!ioLoopStart || !ioLoopEnd)
    {
        return;
    }

    loopStart = *ioLoopStart;
    loopEnd = *ioLoopEnd;

    if (sourceFrames == 0 || targetFrames == 0 ||
        loopStart < 0 || loopEnd <= loopStart ||
        (uint32_t)loopStart >= sourceFrames)
    {
        *ioLoopStart = 0;
        *ioLoopEnd = 0;
        return;
    }

    if ((uint32_t)loopEnd > sourceFrames)
    {
        loopEnd = (int32_t)sourceFrames;
    }
    if (loopEnd <= loopStart)
    {
        *ioLoopStart = 0;
        *ioLoopEnd = 0;
        return;
    }

    if (sourceFrames != targetFrames)
    {
        uint32_t mappedStart;
        uint32_t mappedEnd;

        /* Loop points are a half-open interval [start, end). Preserve the
         * covered region by flooring the start boundary and ceiling the end
         * boundary instead of rounding both inward. */
        mappedStart = (uint32_t)(((uint64_t)(uint32_t)loopStart * (uint64_t)targetFrames) /
                                 (uint64_t)sourceFrames);
        mappedEnd = (uint32_t)((((uint64_t)(uint32_t)loopEnd * (uint64_t)targetFrames) +
                                ((uint64_t)sourceFrames - 1ULL)) /
                               (uint64_t)sourceFrames);

        if (mappedStart > targetFrames)
        {
            mappedStart = targetFrames;
        }
        if (mappedEnd > targetFrames)
        {
            mappedEnd = targetFrames;
        }

        if (mappedEnd <= mappedStart)
        {
            if (mappedStart < targetFrames)
            {
                mappedEnd = mappedStart + 1;
            }
            else if (targetFrames > 0)
            {
                mappedStart = targetFrames - 1;
                mappedEnd = targetFrames;
            }
            else
            {
                mappedStart = 0;
                mappedEnd = 0;
            }
        }

        loopStart = (int32_t)mappedStart;
        loopEnd = (int32_t)mappedEnd;
    }

    if (loopStart < 0 || loopEnd <= loopStart || loopEnd > (int32_t)targetFrames)
    {
        *ioLoopStart = 0;
        *ioLoopEnd = 0;
        return;
    }

    *ioLoopStart = loopStart;
    *ioLoopEnd = loopEnd;
}

static uint32_t PV_GetDecodedFrameCountFromSnd(XPTR sndResource)
{
    SampleDataInfo info;

    if (!sndResource)
    {
        return 0;
    }

    XSetMemory(&info, (int32_t)sizeof(info), 0);
    if (!XGetSamplePtrFromSnd(sndResource, &info))
    {
        if (info.pMasterPtr && info.pMasterPtr != sndResource)
        {
            XDisposePtr(info.pMasterPtr);
        }
        return 0;
    }

    if (info.pMasterPtr && info.pMasterPtr != sndResource)
    {
        XDisposePtr(info.pMasterPtr);
    }
    return info.frames;
}

/* SND dedup is only safe when all SND-header-significant parameters that are
 * not overridden by INST data match. Root key is controlled by INST/split data
 * in this editor flow, so shared-asset dedupe may ignore root-key differences.
 * Loop/rate must still match to preserve playback behavior. */
static bool PV_CanReuseSndResourceForSamples(BAERmfEditorSample const *left,
                                              BAERmfEditorSample const *right)
{
    if (!left || !right)
    {
        return FALSE;
    }
    /* Never dedupe bank-alias samples with embedded samples.
     * Alias samples must keep their bank SND resource IDs verbatim. */
    if (left->isBankAlias || right->isBankAlias)
    {
        return FALSE;
    }
    if (left->sampleAssetID == 0 || left->sampleAssetID != right->sampleAssetID)
    {
        return FALSE;
    }
    if (left->originalSndResourceType != right->originalSndResourceType)
    {
        return FALSE;
    }
    if (left->targetOpusMode != right->targetOpusMode)
    {
        return FALSE;
    }
    if (left->opusUseRoundTripResampling != right->opusUseRoundTripResampling)
    {
        return FALSE;
    }
    /* For passthrough RMF samples, keep original shared SND topology even when
     * per-instrument root/loop interpretation differs. */
    if (left->targetCompressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE &&
        right->targetCompressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE &&
        left->originalSndData && right->originalSndData)
    {
        return TRUE;
    }
    if (left->sampleInfo.startLoop != right->sampleInfo.startLoop ||
        left->sampleInfo.endLoop != right->sampleInfo.endLoop)
    {
        return FALSE;
    }
    if (PV_NormalizeSampleRateForSave(left->sampleInfo.sampledRate) !=
        PV_NormalizeSampleRateForSave(right->sampleInfo.sampledRate))
    {
        return FALSE;
    }
    return TRUE;
}

static BAEResult PV_CopyOriginalInstExtendedTail(BAERmfEditorInstrumentExt const *ext,
                                                 XPTR *outTail,
                                                 int32_t *outTailSize)
{
    enum
    {
        kInstOffset_keySplitCount = 12,
        kInstOffset_keySplitData = 14,
        kKeySplitFileSize = 8,
        kInstTailSize = 10
    };
    uint16_t oldSplitCount;
    int32_t tailOffset;
    int32_t tailSize;
    XPTR tailCopy;

    if (!outTail || !outTailSize)
    {
        return BAE_PARAM_ERR;
    }
    *outTail = NULL;
    *outTailSize = 0;

    if (!ext || !ext->originalInstData || ext->originalInstSize <= 0)
    {
        return BAE_PARAM_ERR;
    }
    if (ext->originalInstSize < (kInstOffset_keySplitData + kInstTailSize))
    {
        return BAE_BAD_FILE;
    }

    oldSplitCount = (uint16_t)XGetShort(((unsigned char const *)ext->originalInstData) + kInstOffset_keySplitCount);
    tailOffset = (int32_t)(kInstOffset_keySplitData + ((int32_t)oldSplitCount * kKeySplitFileSize) + kInstTailSize);
    if (tailOffset < 0 || tailOffset > ext->originalInstSize)
    {
        return BAE_BAD_FILE;
    }

    tailSize = ext->originalInstSize - tailOffset;
    if (tailSize <= 0)
    {
        return BAE_NO_ERROR;
    }

    tailCopy = XNewPtr(tailSize);
    if (!tailCopy)
    {
        return BAE_MEMORY_ERR;
    }
    XBlockMove(((unsigned char const *)ext->originalInstData) + tailOffset, tailCopy, tailSize);
    *outTail = tailCopy;
    *outTailSize = tailSize;
    return BAE_NO_ERROR;
}

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

static BAEResult PV_CreatePascalName(char const *source, char outName[256]);
static BAEResult PV_GrowBuffer(void **buffer, uint32_t *capacity, uint32_t elementSize, uint32_t minimumCount);
static void PV_ClearTempoEvents(BAERmfEditorDocument *document);
static BAEResult PV_AddTempoEvent(BAERmfEditorDocument *document, uint32_t tick, uint32_t microsecondsPerQuarter);
static BAEResult PV_ReadVLQ(unsigned char const *data, uint32_t dataSize, uint32_t *ioOffset, uint32_t *outValue);
static BAEResult PV_AddCCEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char cc, unsigned char value, unsigned char data2);
static BAEResult PV_AddSysExEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char status, unsigned char const *data, uint32_t size);
static void PV_FreeTrackSysExEvents(BAERmfEditorTrack *track);
static BAEResult PV_PopulateSongResourceInfoFromDocument(BAERmfEditorDocument const *document,
                                                         SongResource_Info *songInfo,
                                                         XLongResourceID midiResourceID);
static BAEResult PV_AddRequiredAliases(BAERmfEditorDocument *document, XFILE fileRef, bool isZmf);
static BAERmfEditorResourceEntry const *PV_FindOriginalResourceByTypeAndID(BAERmfEditorDocument const *document,
                                                                            XResourceType type,
                                                                            XLongResourceID id);
static BAEResult PV_AddAuxEventToTrack(BAERmfEditorTrack *track,
                                       uint32_t tick,
                                       unsigned char status,
                                       unsigned char data1,
                                       unsigned char data2,
                                       unsigned char dataBytes);
static void PV_FreeTrackAuxEvents(BAERmfEditorTrack *track);
static BAEResult PV_AddMetaEventToTrack(BAERmfEditorTrack *track,
                                        uint32_t tick,
                                        unsigned char type,
                                        unsigned char const *data,
                                        uint32_t size);
static void PV_FreeTrackMetaEvents(BAERmfEditorTrack *track);
static BAEResult PV_SetDebugOriginalMidiData(BAERmfEditorDocument *document,
                                             unsigned char const *midiData,
                                             uint32_t midiDataSize);
static void PV_FreeDebugOriginalMidiData(BAERmfEditorDocument *document);
static void PV_DebugFreeMidiStats(BAEDebugMidiStats *stats);
static BAEResult PV_DebugCollectMidiStats(unsigned char const *data,
                                         uint32_t dataSize,
                                         BAEDebugMidiStats *outStats);
static void PV_DebugReportMidiRoundTripDiff(BAERmfEditorDocument const *document,
                                            ByteBuffer const *generatedMidi);
static void PV_ParseExtendedInstData(XPTR instData, int32_t instSize, BAERmfEditorInstrumentExt *ext);
static BAERmfEditorInstrumentExt *PV_FindInstrumentExt(BAERmfEditorDocument *document, XLongResourceID instID);
static BAEResult PV_AddInstrumentExt(BAERmfEditorDocument *document, BAERmfEditorInstrumentExt const *ext);
static void PV_ClearInstrumentExts(BAERmfEditorDocument *document);
static XPTR PV_SerializeExtendedInstTail(BAERmfEditorInstrumentExt const *ext, int32_t *outSize);
static int PV_CompareCCEvents(void const *left, void const *right);
static bool PV_TrackHasMetaType(BAERmfEditorTrack const *track, unsigned char type);
static BAERmfEditorCCEvent *PV_FindTrackCCEvent(BAERmfEditorTrack *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex);
static BAERmfEditorCCEvent const *PV_FindTrackCCEventConst(BAERmfEditorTrack const *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex);
static unsigned char PV_ToLowerAscii(unsigned char c);
static bool PV_IsLoopStartMarkerText(unsigned char const *data, uint32_t size, int32_t *outLoopCount);
static bool PV_IsLoopEndMarkerText(unsigned char const *data, uint32_t size);
static void PV_RemoveLoopMarkersFromTrack(BAERmfEditorTrack *track);

static char *PV_DuplicateString(char const *source)
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

static void PV_FreeString(char **target)
{
    if (target && *target)
    {
        XDisposePtr(*target);
        *target = NULL;
    }
}

static void PV_MarkDocumentDirty(BAERmfEditorDocument *document)
{
    if (document)
    {
        document->isPristine = FALSE;
        document->loopMarkersOnlyDirty = FALSE;
    }
}

static BAEResult PV_SetDebugOriginalMidiData(BAERmfEditorDocument *document,
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

static void PV_FreeDebugOriginalMidiData(BAERmfEditorDocument *document)
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

static void PV_DebugFreeMidiStats(BAEDebugMidiStats *stats)
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

static void PV_DebugHashByte(uint32_t *hash, unsigned char value)
{
    *hash ^= (uint32_t)value;
    *hash *= 16777619UL;
}

static void PV_DebugHashU32(uint32_t *hash, uint32_t value)
{
    PV_DebugHashByte(hash, (unsigned char)(value & 0xFF));
    PV_DebugHashByte(hash, (unsigned char)((value >> 8) & 0xFF));
    PV_DebugHashByte(hash, (unsigned char)((value >> 16) & 0xFF));
    PV_DebugHashByte(hash, (unsigned char)((value >> 24) & 0xFF));
}

static BAEResult PV_DebugCollectMidiStats(unsigned char const *data,
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

static void PV_DebugDumpMidiTrack(const char *label, unsigned char const *data, uint32_t dataSize, uint16_t trackIndex)
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

static void PV_DebugReportMidiRoundTripDiff(BAERmfEditorDocument const *document,
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

static void PV_FreeOriginalResources(BAERmfEditorDocument *document)
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

static void PV_ClearTempoEvents(BAERmfEditorDocument *document)
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

static BAEResult PV_AddTempoEvent(BAERmfEditorDocument *document, uint32_t tick, uint32_t microsecondsPerQuarter)
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

static BAEResult PV_AddCCEventToTrack(BAERmfEditorTrack *track, uint32_t tick, unsigned char cc, unsigned char value, unsigned char data2)
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

static BAEResult PV_AddSysExEventToTrack(BAERmfEditorTrack *track,
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

static void PV_FreeTrackSysExEvents(BAERmfEditorTrack *track)
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

static BAEResult PV_AddAuxEventToTrack(BAERmfEditorTrack *track,
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

static void PV_FreeTrackAuxEvents(BAERmfEditorTrack *track)
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

static BAEResult PV_AddMetaEventToTrack(BAERmfEditorTrack *track,
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

static void PV_FreeTrackMetaEvents(BAERmfEditorTrack *track)
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

static unsigned char PV_ToLowerAscii(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static bool PV_MarkerStartsWith(unsigned char const *data, uint32_t size, char const *text)
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

static bool PV_IsLoopStartMarkerText(unsigned char const *data, uint32_t size, int32_t *outLoopCount)
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

static bool PV_IsLoopEndMarkerText(unsigned char const *data, uint32_t size)
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

static void PV_RemoveLoopMarkersFromTrack(BAERmfEditorTrack *track)
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

static int PV_CompareCCEvents(void const *left, void const *right)
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

static BAERmfEditorCCEvent *PV_FindTrackCCEventAtTick(BAERmfEditorTrack *track, unsigned char cc, uint32_t atTick)
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

static BAERmfEditorCCEvent *PV_FindTrackCCEvent(BAERmfEditorTrack *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex)
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

static BAERmfEditorCCEvent const *PV_FindTrackCCEventConst(BAERmfEditorTrack const *track, unsigned char cc, uint32_t eventIndex, uint32_t *outActualIndex)
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

static BAEResult PV_CaptureOriginalResourcesFromFile(BAERmfEditorDocument *document, XFILE fileRef)
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

static BAEResult PV_GrowBuffer(void **buffer, uint32_t *capacity, uint32_t elementSize, uint32_t minimumCount)
{
    void *nextBuffer;
    uint32_t nextCapacity;
    if (*capacity >= minimumCount)
    {
        return BAE_NO_ERROR;
    }
    nextCapacity = *capacity ? (*capacity * 2) : 4;
    while (nextCapacity < minimumCount)
    {
        nextCapacity *= 2;
    }
    XPI_Memblock *mb = XGetHeaderIfOurs(*buffer);
    if (!mb) {
        nextBuffer = XNewPtr((int32_t)(nextCapacity * elementSize));
        if (!nextBuffer)
        {
            return BAE_MEMORY_ERR;
        }
        if (*buffer && *capacity)
        {
            XBlockMove(*buffer, nextBuffer, (int32_t)(*capacity * elementSize));
            XDisposePtr(*buffer);
        }
    } else {
        nextBuffer = XResizePtr(*buffer, (int32_t)(nextCapacity * elementSize));
        if (!nextBuffer) {
            return BAE_MEMORY_ERR;
        }
    }
    *buffer = nextBuffer;
    *capacity = nextCapacity;
    return BAE_NO_ERROR;
}

static BAEResult PV_ByteBufferReserve(ByteBuffer *buffer, uint32_t extraBytes)
{
    uint32_t required;

    required = buffer->size + extraBytes;
    return PV_GrowBuffer((void **)&buffer->data, &buffer->capacity, sizeof(unsigned char), required);
}

static BAEResult PV_ByteBufferAppend(ByteBuffer *buffer, void const *data, uint32_t length)
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

static BAEResult PV_ByteBufferAppendByte(ByteBuffer *buffer, unsigned char value)
{
    return PV_ByteBufferAppend(buffer, &value, 1);
}

static BAEResult PV_ByteBufferAppendBE16(ByteBuffer *buffer, uint16_t value)
{
    unsigned char bytes[2];

    bytes[0] = (unsigned char)((value >> 8) & 0xFF);
    bytes[1] = (unsigned char)(value & 0xFF);
    return PV_ByteBufferAppend(buffer, bytes, 2);
}

static BAEResult PV_ByteBufferAppendBE32(ByteBuffer *buffer, uint32_t value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)((value >> 24) & 0xFF);
    bytes[1] = (unsigned char)((value >> 16) & 0xFF);
    bytes[2] = (unsigned char)((value >> 8) & 0xFF);
    bytes[3] = (unsigned char)(value & 0xFF);
    return PV_ByteBufferAppend(buffer, bytes, 4);
}

static BAEResult PV_ByteBufferAppendVLQ(ByteBuffer *buffer, uint32_t value)
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

static void PV_ByteBufferDispose(ByteBuffer *buffer)
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

static BAEResult PV_SetDocumentString(char **target, char const *value)
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

static AudioFileType PV_TranslateEditorFileType(BAEFileType fileType)
{
    switch (fileType)
    {
        case BAE_WAVE_TYPE:
            return FILE_WAVE_TYPE;
        case BAE_AIFF_TYPE:
            return FILE_AIFF_TYPE;
#if USE_MPEG_DECODER == TRUE || USE_MPEG_ENCODER == TRUE
        case BAE_MPEG_TYPE:
            return FILE_MPEG_TYPE;
#endif
#if USE_FLAC_DECODER == TRUE || USE_FLAC_ENCODER == TRUE
        case BAE_FLAC_TYPE:
            return FILE_FLAC_TYPE;
#endif
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
        case BAE_VORBIS_TYPE:
            return FILE_VORBIS_TYPE;
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        case BAE_OPUS_TYPE:
            return FILE_OPUS_TYPE;
#endif
#if USE_QOA_SUPPORT == TRUE
        case BAE_QOA_TYPE:
            return FILE_QOA_TYPE;
#endif
        default:
            return FILE_INVALID_TYPE;
    }
}

static bool PV_IsEditorCompressedImportType(BAEFileType fileType)
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

static bool PV_IsSupportedPassthroughCompression(SndCompressionType compressionType)
{
    switch (compressionType)
    {
#if USE_MPEG_DECODER == TRUE || USE_MPEG_ENCODER == TRUE
        case C_MPEG_32:
        case C_MPEG_40:
        case C_MPEG_48:
        case C_MPEG_56:
        case C_MPEG_64:
        case C_MPEG_80:
        case C_MPEG_96:
        case C_MPEG_112:
        case C_MPEG_128:
        case C_MPEG_160:
        case C_MPEG_192:
        case C_MPEG_224:
        case C_MPEG_256:
        case C_MPEG_320:
            return TRUE;
#endif
#if USE_FLAC_DECODER == TRUE || USE_FLAC_ENCODER == TRUE
        case C_FLAC:
            return TRUE;
#endif
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
        case C_VORBIS:
            return TRUE;
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        case C_OPUS:
            return TRUE;
#endif
#if USE_QOA_SUPPORT == TRUE
        case C_QOA:
            return TRUE;
#endif
        default:
            return FALSE;
    }
}

static BAEResult PV_CompressionTypeFromEditorFileType(BAEFileType fileType,
                                                      SndCompressionType *outCompressionType)
{
    if (!outCompressionType)
    {
        return BAE_PARAM_ERR;
    }

    switch (fileType)
    {
#if USE_MPEG_DECODER == TRUE || USE_MPEG_ENCODER == TRUE
        case BAE_MPEG_TYPE:
            *outCompressionType = C_MPEG_128;
            return BAE_NO_ERROR;
#endif
#if USE_FLAC_DECODER == TRUE || USE_FLAC_ENCODER == TRUE
        case BAE_FLAC_TYPE:
            *outCompressionType = C_FLAC;
            return BAE_NO_ERROR;
#endif
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
        case BAE_VORBIS_TYPE:
            *outCompressionType = C_VORBIS;
            return BAE_NO_ERROR;
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        case BAE_OPUS_TYPE:
            *outCompressionType = C_OPUS;
            return BAE_NO_ERROR;
#endif
#if USE_QOA_SUPPORT == TRUE
    case BAE_QOA_TYPE:
        *outCompressionType = C_QOA;
        return BAE_NO_ERROR;
#endif
        default:
            break;
    }

    return BAE_BAD_FILE_TYPE;
}

static BAEResult PV_ReadFileIntoMemory(XFILENAME const *fileName,
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

static BAEResult PV_CreatePassthroughSndFromEncodedData(GM_Waveform const *decodedWaveform,
                                                        XPTR encodedData,
                                                        int32_t encodedSize,
                                                        SndCompressionType compressionType,
                                                        SndCompressionSubType compressionSubType,
                                                        XPTR *outSndData,
                                                        int32_t *outSndSize)
{
    XPTR sndData;
    int32_t sndSize;

    if (!decodedWaveform || !encodedData || encodedSize <= 0 || !outSndData || !outSndSize)
    {
        return BAE_PARAM_ERR;
    }
    if (!PV_IsSupportedPassthroughCompression(compressionType))
    {
        return BAE_BAD_FILE_TYPE;
    }

    *outSndData = NULL;
    *outSndSize = 0;

    sndSize = (int32_t)(sizeof(XSndHeader3) + encodedSize);
    sndData = XNewPtr(sndSize);
    if (!sndData)
    {
        return BAE_MEMORY_ERR;
    }

    {
        XSndHeader3 *snd = (XSndHeader3 *)sndData;
        uint32_t decodedBytes;

        XSetMemory(snd, sizeof(XSndHeader3), 0);
        XPutShort(&snd->type, XThirdSoundFormat);
        XPutLong(&snd->sndBuffer.subType, (uint32_t)compressionType);
        XPutLong(&snd->sndBuffer.sampleRate, (uint32_t)decodedWaveform->sampledRate);
        XPutLong(&snd->sndBuffer.frameCount, decodedWaveform->waveFrames);
        XPutLong(&snd->sndBuffer.encodedBytes, (uint32_t)encodedSize);

        decodedBytes = (uint32_t)(decodedWaveform->waveFrames * decodedWaveform->channels * (decodedWaveform->bitSize / 8));
        if (decodedBytes == 0 && decodedWaveform->waveSize > 0)
        {
            decodedBytes = (uint32_t)decodedWaveform->waveSize;
        }
        XPutLong(&snd->sndBuffer.decodedBytes, decodedBytes);
        XPutLong(&snd->sndBuffer.blockBytes, 0);
        XPutLong(&snd->sndBuffer.startFrame, 0);
        XPutLong(&snd->sndBuffer.loopStart[0], decodedWaveform->startLoop);
        XPutLong(&snd->sndBuffer.loopEnd[0], decodedWaveform->endLoop);
        snd->sndBuffer.baseKey = (unsigned char)decodedWaveform->baseMidiPitch;
        snd->sndBuffer.channels = (unsigned char)decodedWaveform->channels;
        snd->sndBuffer.bitSize = (unsigned char)decodedWaveform->bitSize;
        snd->sndBuffer.isEmbedded = TRUE;
        XBlockMove(encodedData, snd->sndBuffer.sampleArea, encodedSize);
    }

    PV_StoreCompressionSubTypeInSnd(sndData,
                                    sndSize,
                                    compressionType,
                                    compressionSubType);

    *outSndData = sndData;
    *outSndSize = sndSize;
    return BAE_NO_ERROR;
}

static BAEResult PV_CreatePassthroughSndFromCompressedWaveform(GM_Waveform const *decodedWaveform,
                                                               GM_Waveform const *compressedWaveform,
                                                               SndCompressionSubType compressionSubType,
                                                               XPTR *outSndData,
                                                               int32_t *outSndSize)
{
    SndCompressionType compressionType;
    XPTR sndData;
    int32_t sndSize;

    if (!decodedWaveform || !compressedWaveform || !compressedWaveform->theWaveform ||
        compressedWaveform->waveSize <= 0 || !outSndData || !outSndSize)
    {
        return BAE_PARAM_ERR;
    }

    *outSndData = NULL;
    *outSndSize = 0;

    compressionType = (SndCompressionType)compressedWaveform->compressionType;
    if (!PV_IsSupportedPassthroughCompression(compressionType))
    {
        return BAE_BAD_FILE_TYPE;
    }

#if USE_MPEG_DECODER == TRUE || USE_MPEG_ENCODER == TRUE
    if (compressionType == C_MPEG_32 || compressionType == C_MPEG_40 || compressionType == C_MPEG_48 ||
        compressionType == C_MPEG_56 || compressionType == C_MPEG_64 || compressionType == C_MPEG_80 ||
        compressionType == C_MPEG_96 || compressionType == C_MPEG_112 || compressionType == C_MPEG_128 ||
        compressionType == C_MPEG_160 || compressionType == C_MPEG_192 || compressionType == C_MPEG_224 ||
        compressionType == C_MPEG_256 || compressionType == C_MPEG_320)
    {
        OPErr opErr;

        opErr = XCreateSoundObjectFromData(outSndData,
                                           compressedWaveform,
                                           compressionType,
                                           compressionSubType,
                                           NULL,
                                           NULL);
        if (opErr != NO_ERR || !*outSndData)
        {
            return BAE_BAD_FILE;
        }
        *outSndSize = XGetPtrSize(*outSndData);
        return BAE_NO_ERROR;
    }
#endif

    sndSize = (int32_t)(sizeof(XSndHeader3) + compressedWaveform->waveSize);
    sndData = XNewPtr(sndSize);
    if (!sndData)
    {
        return BAE_MEMORY_ERR;
    }

    {
        XSndHeader3 *snd = (XSndHeader3 *)sndData;
        uint32_t decodedBytes;

        XSetMemory(snd, sizeof(XSndHeader3), 0);
        XPutShort(&snd->type, XThirdSoundFormat);
        XPutLong(&snd->sndBuffer.subType, (uint32_t)compressionType);
        XPutLong(&snd->sndBuffer.sampleRate, (uint32_t)decodedWaveform->sampledRate);
        XPutLong(&snd->sndBuffer.frameCount, decodedWaveform->waveFrames);
        XPutLong(&snd->sndBuffer.encodedBytes, (uint32_t)compressedWaveform->waveSize);

        decodedBytes = (uint32_t)(decodedWaveform->waveFrames * decodedWaveform->channels * (decodedWaveform->bitSize / 8));
        if (decodedBytes == 0 && decodedWaveform->waveSize > 0)
        {
            decodedBytes = (uint32_t)decodedWaveform->waveSize;
        }
        XPutLong(&snd->sndBuffer.decodedBytes, decodedBytes);

        XPutLong(&snd->sndBuffer.blockBytes, 0);
        XPutLong(&snd->sndBuffer.startFrame, 0);
        XPutLong(&snd->sndBuffer.loopStart[0], decodedWaveform->startLoop);
        XPutLong(&snd->sndBuffer.loopEnd[0], decodedWaveform->endLoop);
        snd->sndBuffer.baseKey = (unsigned char)decodedWaveform->baseMidiPitch;
        snd->sndBuffer.channels = (unsigned char)decodedWaveform->channels;
        snd->sndBuffer.bitSize = (unsigned char)decodedWaveform->bitSize;
        snd->sndBuffer.isEmbedded = TRUE;
        XBlockMove(compressedWaveform->theWaveform, snd->sndBuffer.sampleArea, compressedWaveform->waveSize);
    }

    PV_StoreCompressionSubTypeInSnd(sndData,
                                    sndSize,
                                    compressionType,
                                    compressionSubType);

    *outSndData = sndData;
    *outSndSize = sndSize;
    return BAE_NO_ERROR;
}

static BAEResult PV_AssignSongInfoString(SongResource_Info *songInfo, BAEInfoType infoType, char const *value)
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

static BAEResult PV_PopulateSongResourceInfoFromDocument(BAERmfEditorDocument const *document,
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

static BAEResult PV_CreatePascalName(char const *source, char outName[256])
{
    uint32_t length;

    if (!outName)
    {
        return BAE_PARAM_ERR;
    }
    if (!source)
    {
        outName[0] = 0;
        return BAE_NO_ERROR;
    }
    length = (uint32_t)strlen(source);
    if (length > 255)
    {
        length = 255;
    }
    outName[0] = (char)length;
    if (length)
    {
        XBlockMove(source, outName + 1, (int32_t)length);
    }
    return BAE_NO_ERROR;
}

static BAEResult PV_ReadVLQ(unsigned char const *data, uint32_t dataSize, uint32_t *ioOffset, uint32_t *outValue)
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

static unsigned char PV_ClampMidi7Bit(int32_t value)
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

static BAERmfEditorTrack *PV_GetTrack(BAERmfEditorDocument *document, uint16_t trackIndex)
{
    if (!document || trackIndex >= document->trackCount)
    {
        return NULL;
    }
    return &document->tracks[trackIndex];
}

static BAERmfEditorTrack const *PV_GetTrackConst(BAERmfEditorDocument const *document, uint16_t trackIndex)
{
    if (!document || trackIndex >= document->trackCount)
    {
        return NULL;
    }
    return &document->tracks[trackIndex];
}

static BAEResult PV_AddNoteToTrack(BAERmfEditorTrack *track,
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

static BAEResult PV_SetTrackName(BAERmfEditorTrack *track, char const *name)
{
    if (!track)
    {
        return BAE_PARAM_ERR;
    }
    return PV_SetDocumentString(&track->name, name);
}

static BAEResult PV_ReadWholeFile(BAEPathName filePath, unsigned char **outData, uint32_t *outSize)
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

/* Scan forward from peekOffset over events at the same tick (zero-delta VLQs),
   updating *bank and *program with the final values seen for the given channel.
   This is used when a NoteOn is parsed before a same-tick Program Change or
   Bank Select - a common authoring pattern where the instrument is set just
   after the first note at the same tick. */
static void PV_PeekSameTickBankProgram(unsigned char const *trackData,
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

static BAERmfEditorActiveNote *PV_PushActiveNote(BAERmfEditorActiveNote **head,
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

static BAERmfEditorActiveNote *PV_PopActiveNote(BAERmfEditorActiveNote **head,
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

static void PV_DisposeActiveNotes(BAERmfEditorActiveNote **head)
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

static BAEResult PV_FinalizeActiveNotes(BAERmfEditorTrack *track,
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

static int PV_CompareChannelStateEvents(void const *left, void const *right)
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

/* Reconcile imported note bank/program against a merged (format-1 style)
   channel-state timeline across all tracks.  Each track's notes prefer
   bank/program from that track's OWN aux events so that round-trip fidelity
   is preserved.  However, when a track has NO bank-select or program-change
   events for a given channel, the global (cross-track) state is used instead
   so that the classic MIDI pattern of "setup track + note track" works. */
static BAEResult PV_ReconcileImportedMidiNotePrograms(BAERmfEditorDocument *document)
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

static BAEResult PV_LoadMidiTrackIntoDocument(BAERmfEditorDocument *document,
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

static BAEResult PV_LoadMidiBytesIntoDocument(BAERmfEditorDocument *document,
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

static void PV_DecodeResourceName(char const *rawName, char outName[256])
{
    uint8_t len;

    if (!outName)
    {
        return;
    }
    outName[0] = 0;
    if (!rawName)
    {
        return;
    }
    /* Some paths return Pascal strings, others C strings. */
    len = (uint8_t)rawName[0];
    if (len > 0 && len < 64 && rawName[1] >= 32)
    {
        uint32_t copyLen = len;
        if (copyLen > 255)
        {
            copyLen = 255;
        }
        XBlockMove(rawName + 1, outName, (int32_t)copyLen);
        outName[copyLen] = 0;
        return;
    }
    XStrCpy(outName, rawName);
}

static BAEResult PV_GetEmbeddedSampleDisplayName(XFILE fileRef,
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

static BAEResult PV_AddEmbeddedSampleVariant(BAERmfEditorDocument *document,
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
        else if (sdi.baseKey > 0 && sdi.baseKey <= 127)
        {
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

/* Add a bank alias sample: stores only metadata + bank/SND reference, no PCM or SND blob.
 * If fileRef is non-NULL and the SND can be found, sample metadata (rate, channels, etc.)
 * is populated from the SND header.  Otherwise a minimal entry is created. */
static BAEResult PV_AddBankAliasSample(BAERmfEditorDocument *document,
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

/* ---------- Instrument extended data helpers ---------- */

#define EDITOR_KEY_SPLIT_FILE_SIZE 8  /* matches KEY_SPLIT_FILE_SIZE in GenPatch.c */

static BAERmfEditorInstrumentExt *PV_FindInstrumentExt(BAERmfEditorDocument *document, XLongResourceID instID)
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

static BAEResult PV_AddInstrumentExt(BAERmfEditorDocument *document, BAERmfEditorInstrumentExt const *ext)
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

static void PV_EnsureInstrumentExtForRemappedID(BAERmfEditorDocument *document,
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

static void PV_ClearInstrumentExts(BAERmfEditorDocument *document)
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

/* Map XInstrumentData units into BAERmfEditorInstrumentExt unit fields. */
static void PV_FillExtFromXInstrument(BAERmfEditorInstrumentExt *ext, XInstrumentData const *x)
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

/* Build XInstrumentData from Ext unit fields (no edit padding). */
static void PV_FillXFromInstrumentExt(XInstrumentData *x, BAERmfEditorInstrumentExt const *ext)
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
static void PV_ParseExtendedInstData(XPTR instData, int32_t instSize, BAERmfEditorInstrumentExt *ext)
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
static XPTR PV_SerializeExtendedInstTail(BAERmfEditorInstrumentExt const *ext, int32_t *outSize)
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

/* Extract INST + SND resources from an open RMF resource file and add them
   as editable samples in the document. Includes all key-split variants. */
/* Check whether a SND resource ID exists among the document's captured original resources. */
static bool PV_SndExistsInOriginalResources(BAERmfEditorDocument const *document, XShortResourceID sndID)
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

static BAERmfEditorResourceEntry const *PV_FindOriginalResourceByTypeAndID(BAERmfEditorDocument const *document,
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

static void PV_LoadEmbeddedSamplesFromRmf(BAERmfEditorDocument *document, XFILE fileRef)
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

/* Decode raw MIDI resource data for encrypted/compressed types (ecmi, emid, cmid).
 * Takes ownership of 'raw'. Returns decoded/decompressed MIDI data or NULL on failure.
 * Updates *ioSize with the final decoded size. */
static XPTR PV_DecodeMidiData(XPTR raw, XResourceType rtype, int32_t *ioSize)
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

static BAEResult PV_LoadRmfResourceIntoDocument(BAERmfEditorDocument *document, XFILE fileRef)
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

static BAEResult PV_LoadRmfFileIntoDocument(BAERmfEditorDocument *document, BAEPathName filePath)
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

static BAEResult PV_LoadRmfMemoryIntoDocument(BAERmfEditorDocument *document,
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

static BAEResult PV_GetAvailableResourceID(XFILE fileRef,
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

static BAEResult PV_EnsureResourceFileReady(XFILE fileRef, int32_t resourceID)
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

static BAEResult PV_PrepareResourceFilePath(XFILENAME *name, int32_t resourceID)
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

static BAEResult PV_WriteOriginalResources(BAERmfEditorDocument const *document, XFILE fileRef)
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

static BAEResult PV_EncodeMidiForResourceType(XResourceType resourceType,
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
static BAEResult PV_EncodeMidiBestEffort(ByteBuffer const *plainMidi,
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

static BAERmfEditorMidiStorageType PV_NormalizeMidiStorageType(BAERmfEditorMidiStorageType storageType)
{
    if (storageType < BAE_EDITOR_MIDI_STORAGE_CMID_BEST_EFFORT ||
        storageType > BAE_EDITOR_MIDI_STORAGE_MIDI)
    {
        return BAE_EDITOR_MIDI_STORAGE_ECMI;
    }
    return storageType;
}

static BAEResult PV_EncodeMidiForStorageType(BAERmfEditorMidiStorageType storageType,
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

static bool PV_IsMidiResourceType(XResourceType resourceType)
{
    return (resourceType == ID_ECMI ||
            resourceType == ID_EMID ||
            resourceType == ID_CMID ||
            resourceType == ID_MIDI ||
            resourceType == ID_MIDI_OLD) ? TRUE : FALSE;
}

static int PV_CompareMidiEvents(void const *left, void const *right)
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

static BAEResult PV_AppendMetaEvent(ByteBuffer *buffer, uint32_t delta, unsigned char type, void const *data, uint32_t length)
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

static BAEResult PV_CompactMidiRunningStatus(ByteBuffer *trackData)
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

static BAEResult PV_BuildTempoTrack(BAERmfEditorDocument *document, ByteBuffer *trackData)
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

static BAEResult PV_BuildConductorTrack(BAERmfEditorDocument *document,
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

static bool PV_IsMetaOnlyConductorTrack(BAERmfEditorTrack const *track)
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

static bool PV_TrackHasMetaType(BAERmfEditorTrack const *track, unsigned char type)
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

static BAEResult PV_BuildTrackData(BAERmfEditorTrack const *track,
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
     * present in aux events for the channel. Keep bank as canonical 14-bit
     * (MSB: bits 7-13, LSB: bits 0-6) to avoid mutating authored LSB-only banks. */
    if (track->bank != 0 || track->program != 0)
    {
        unsigned char channel = track->channel & 0x0F;
        uint16_t bankMsb = (track->bank >> 7) & 0x7F;
        uint16_t bankLsb = track->bank & 0x7F;

        /* Emit Bank MSB (CC 0) */
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

        /* Only advance currentBank to track->bank when we actually emitted
           the bank value (or part of it).  When the initial bank emit was
           suppressed because explicit aux events already carry it, leave
           currentBank at 0 so the aux events update it when they are
           processed in the event loop below. */
        if (!explicitBankMsb[channel] && !explicitBankLsb[channel])
        {
            currentBank[channel] = track->bank;
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
            noteOn->applyProgram = (explicitBankMsb[note->channel] || explicitBankLsb[note->channel] || explicitProgram[note->channel]) ? 0 : 1;

            noteOff->tick = note->startTick + note->durationTicks;
            noteOff->sequence = note->noteOffOrder;
            noteOff->order = 0;
            if ((note->noteOffStatus & 0xF0) == NOTE_ON || (note->noteOffStatus & 0xF0) == NOTE_OFF)
            {
                noteOff->status = note->noteOffStatus;
            }
            else
            {
                noteOff->status = (unsigned char)(NOTE_OFF | (note->channel & 0x0F));
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
                bankChanged = (event->bank != currentBank[eventChannel]);
                if (bankChanged)
                {
                    uint16_t bankMsb;
                    uint16_t bankLsb;
                    uint16_t prevBankLsb;

                    bankMsb = (uint16_t)((event->bank >> 7) & 0x7F);
                    bankLsb = (uint16_t)(event->bank & 0x7F);

                    prevBankLsb = (uint16_t)(currentBank[eventChannel] & 0x7F);

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
                    if (bankLsb != 0 || prevBankLsb != 0)
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

static BAEResult PV_BuildMidiFile(BAERmfEditorDocument *document, ByteBuffer *output)
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

static BAEResult PV_BuildMidiWithAppendedLoopTrack(BAERmfEditorDocument const *document,
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

static BAEResult PV_AddSampleResources(BAERmfEditorDocument *document, XFILE fileRef, bool isZmf)
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
        debug_message("[RMF Save] PV_AddSampleResources: no samples, returning OK\n");
        return BAE_NO_ERROR;
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
             * whenever it is a valid short resource ID and not already used in
             * this save pass by another asset. */
            if (preferredID > 0 && preferredID <= 32767)
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

            /* If we have the original INST blob and no edits were made, write it
             * verbatim for bit-perfect round-trip preservation. We must still update
             * the SND resource IDs and root keys in the blob since those may have
             * been reassigned during save. Skip this shortcut for now and always
             * rebuild so that SND ID remapping stays correct. If the ext data is
             * unmodified, we still append the serialized extended tail. */

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
    return BAE_NO_ERROR;
}

static BAEResult PV_AddSongResource(BAERmfEditorDocument *document, XFILE fileRef, XLongResourceID midiResourceID)
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

static BAEResult PV_AddSongResourceWithID(BAERmfEditorDocument *document,
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
    if (fileType == BAE_MIDI_TYPE)
    {
        unsigned char *data;
        uint32_t dataSize;

        result = PV_ReadWholeFile(filePath, &data, &dataSize);
        if (result == BAE_NO_ERROR)
        {
            result = PV_LoadMidiBytesIntoDocument(document, data, dataSize);
            if (result == BAE_NO_ERROR)
            {
                result = PV_SetDebugOriginalMidiData(document, data, dataSize);
            }
            XDisposePtr(data);
            if (result == BAE_NO_ERROR)
            {
                document->isPristine = TRUE;
            }
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

    if (fileType == BAE_MIDI_TYPE)
    {
        result = PV_LoadMidiBytesIntoDocument(document,
                                              (unsigned char const *)data,
                                              dataSize);
        if (result == BAE_NO_ERROR)
        {
            result = PV_SetDebugOriginalMidiData(document,
                                                 (unsigned char const *)data,
                                                 dataSize);
        }
        if (result == BAE_NO_ERROR)
        {
            document->isPristine = TRUE;
        }
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
    }
#if USE_MTHC_SUPPORT == TRUE
    else if (fileType == BAE_MTHC)
    {
        unsigned char *decodedMthcMidi = NULL;
        uint32_t decodedMthcMidiLen = 0;

        if (mthc_decompress_memory((unsigned char const *)data,
                                    dataSize,
                                    &decodedMthcMidi,
                                    &decodedMthcMidiLen) != 0)
        {
            result = BAE_BAD_FILE;
        }
        else if (decodedMthcMidiLen >= 4 &&
                 decodedMthcMidi[0]=='M' && decodedMthcMidi[1]=='T' && decodedMthcMidi[2]=='h' && decodedMthcMidi[3]=='d')
        {
            result = PV_LoadMidiBytesIntoDocument(document, decodedMthcMidi, decodedMthcMidiLen);
            if (result == BAE_NO_ERROR)
            {
                result = PV_SetDebugOriginalMidiData(document, decodedMthcMidi, decodedMthcMidiLen);
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

        if (decodedMthcMidi)
        {
            free(decodedMthcMidi);
        }
    }
#endif
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

typedef struct PV_AuxOrderRef
{
    uint32_t index;
    uint32_t tick;
    uint32_t eventOrder;
} PV_AuxOrderRef;

static int PV_CompareAuxOrderRef(void const *left, void const *right)
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

static void PV_ShiftTrackEventOrders(BAERmfEditorTrack *track,
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

static BAEResult PV_InsertBankSelectBeforeAuxEvent(BAERmfEditorTrack *track,
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

static BAEResult PV_RemapTrackInstrumentReferences(BAERmfEditorTrack *track,
                                                   uint16_t sourceBank,
                                                   unsigned char sourceProgram,
                                                   uint16_t targetBank,
                                                   unsigned char targetProgram,
                                                   uint16_t remapChannelMask,
                                                   bool *outChanged)
{
    bool changed;
    bool restartScan;

    if (!track)
    {
        return BAE_PARAM_ERR;
    }

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

        for (i = 0; i < track->auxEventCount; ++i)
        {
            refs[i].index = i;
            refs[i].tick = track->auxEvents[i].tick;
            refs[i].eventOrder = track->auxEvents[i].eventOrder;
        }
        qsort(refs, track->auxEventCount, sizeof(PV_AuxOrderRef), PV_CompareAuxOrderRef);

        for (i = 0; i < track->auxEventCount; ++i)
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

                if (sourceBank != targetBank && (remapChannelMask & (uint16_t)(1u << channel)) != 0)
                {
                    if (aux->data1 == BANK_MSB && sourceData2 == (unsigned char)((sourceBank >> 7) & 0x7F))
                    {
                        aux->data2 = (unsigned char)((targetBank >> 7) & 0x7F);
                        changed = TRUE;
                    }
                    else if (aux->data1 == BANK_LSB && sourceData2 == (unsigned char)(sourceBank & 0x7F))
                    {
                        aux->data2 = (unsigned char)(targetBank & 0x7F);
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
                    && (currentBank[channel] == sourceBank
                        || channelFallbackAllowed))
                {
                    aux->data1 = targetProgram;
                    changed = TRUE;

                    if (sourceBank != targetBank && currentBank[channel] != targetBank)
                    {
                        BAEResult insertResult;
                        bool needMsb;
                        bool needLsb;

                        needMsb = (lastBankMsbIndex[channel] == 0xFFFFFFFFu) ? TRUE : FALSE;
                        needLsb = (lastBankLsbIndex[channel] == 0xFFFFFFFFu) ? TRUE : FALSE;

                        if (!needMsb)
                        {
                            track->auxEvents[lastBankMsbIndex[channel]].data2 = (unsigned char)((targetBank >> 7) & 0x7F);
                            changed = TRUE;
                        }
                        if (!needLsb)
                        {
                            track->auxEvents[lastBankLsbIndex[channel]].data2 = (unsigned char)(targetBank & 0x7F);
                            changed = TRUE;
                        }

                        if (needMsb || needLsb)
                        {
                            insertResult = PV_InsertBankSelectBeforeAuxEvent(track,
                                                                              refs[i].index,
                                                                              channel,
                                                                              targetBank,
                                                                              needMsb,
                                                                              needLsb);
                            if (insertResult != BAE_NO_ERROR)
                            {
                                XDisposePtr(refs);
                                return insertResult;
                            }
                            changed = TRUE;
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
    if (sourceBank == targetBank && sourceProgram == targetProgram)
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
        if (track->bank == sourceBank && track->program == sourceProgram)
        {
            documentRemapChannelMask |= (uint16_t)(1u << (track->channel & 0x0F));
        }
        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote const *note = &track->notes[noteIndex];
            if (note->bank == sourceBank && note->program == sourceProgram)
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

        if (track->bank == sourceBank && track->program == sourceProgram)
        {
            track->bank = targetBank;
            track->program = targetProgram;
            changed = TRUE;
        }

        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            BAERmfEditorNote *note;

            note = &track->notes[noteIndex];
            if (note->bank == sourceBank && note->program == sourceProgram)
            {
                note->bank = targetBank;
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

static BAEResult PV_AddTrimHardStopEventsToTrack(BAERmfEditorTrack *track,
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
        if (assetID == 0)
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
        if (sample->sampleAssetID == 0)
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

    if (!document || !outUsageCount || assetID == 0)
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

    if (!document || !outSampleIndex || assetID == 0)
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

    if (!document || assetID == 0)
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

    if (!document || sampleIndex >= document->sampleCount || assetID == 0)
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
    if (assetID == 0)
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

/* Codecs whose native bitstream can be extracted from an XSndHeader3 blob
 * (FLAC/Vorbis/Opus/MPEG/QOA). IMA/ADPCM/PCM decode to WAV instead. */
static bool PV_IsBitstreamPassthroughCodec(SndCompressionType srcCodec)
{
    switch (srcCodec)
    {
#if USE_FLAC_DECODER == TRUE
    case C_FLAC:
        return TRUE;
#endif
#if USE_VORBIS_DECODER == TRUE
    case C_VORBIS:
    case CS_VORBIS_32K:
    case CS_VORBIS_48K:
    case CS_VORBIS_64K:
    case CS_VORBIS_80K:
    case CS_VORBIS_96K:
    case CS_VORBIS_128K:
    case CS_VORBIS_160K:
    case CS_VORBIS_192K:
    case CS_VORBIS_256K:
        return TRUE;
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
    case C_OPUS:
    case CS_OPUS_12K:
    case CS_OPUS_16K:
    case CS_OPUS_24K:
    case CS_OPUS_32K:
    case CS_OPUS_48K:
    case CS_OPUS_64K:
    case CS_OPUS_80K:
    case CS_OPUS_96K:
    case CS_OPUS_128K:
    case CS_OPUS_160K:
    case CS_OPUS_192K:
    case CS_OPUS_256K:
        return TRUE;
#endif
#if USE_MPEG_DECODER != 0
    case C_MPEG_32:  case C_MPEG_40:  case C_MPEG_48:  case C_MPEG_56:
    case C_MPEG_64:  case C_MPEG_80:  case C_MPEG_96:  case C_MPEG_112:
    case C_MPEG_128: case C_MPEG_160: case C_MPEG_192: case C_MPEG_224:
    case C_MPEG_256: case C_MPEG_320:
        return TRUE;
#endif
#if USE_QOA_SUPPORT == TRUE
    case C_QOA:
        return TRUE;
#endif
    default:
        return FALSE;
    }
}

/* Write the compressed payload from an XSndHeader3 SND blob, or BAE_NOT_SETUP
 * if the blob is not a passthrough Type-3 compressed sample. */
static BAEResult PV_ExportSndBitstreamToFile(XPTR sndData,
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

static void PV_CopyEditorADSRToInfo(EditorADSR const *src, BAERmfEditorADSRInfo *dst)
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

static void PV_CopyInfoToEditorADSR(BAERmfEditorADSRInfo const *src, EditorADSR *dst)
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

static BAEResult PV_ResolveBankInstrumentIndexForGhost(BAEBankToken bankToken,
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
static bool PV_ResolveGhostAliasSndID(BAEBankToken bankToken,
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

static BAEResult PV_ConvertDocumentSampleToBankAlias(BAERmfEditorSample *sample,
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

static BAEResult PV_HydrateDocumentSampleFromBankAlias(BAERmfEditorDocument *document,
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

static BAEResult PV_CopySampleEntry(BAERmfEditorDocument *dest,
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
                false)
                {
                    reason |= BAEZMF_REASON_MODERN_CODEC;
                }
                break;
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

BAE_BOOL BAERmfEditorBank_RequiresZsb(BAEBankToken bankToken, uint32_t *outReason)
{
    uint32_t instrumentCount;
    uint32_t instrumentIndex;
    uint32_t reason = 0;

    if (!bankToken)
    {
        return FALSE;
    }

    instrumentCount = 0;
    if (BAERmfEditorBank_GetInstrumentCount(bankToken, &instrumentCount) != BAE_NO_ERROR)
    {
        return FALSE;
    }

    for (instrumentIndex = 0; instrumentIndex < instrumentCount; ++instrumentIndex)
    {
        BAERmfEditorInstrumentExtInfo extInfo;
        BAERmfEditorBankSampleInfo firstSampleInfo;
        uint32_t sampleCount;
        uint32_t sampleIndex;

        if (BAERmfEditorBank_GetInstrumentExtInfo(bankToken, instrumentIndex, &extInfo) == BAE_NO_ERROR)
        {
            if ((reason & BAEZMF_REASON_EXTENDED_ADSR) == 0)
            {
                if (extInfo.volumeADSR.stageCount > BAE_RMF_MAX_ADSR_STAGES)
                {
                    reason |= BAEZMF_REASON_EXTENDED_ADSR;
                }
                else
                {
                    uint32_t lfoIdx;
                    for (lfoIdx = 0; lfoIdx < extInfo.lfoCount && lfoIdx < BAE_EDITOR_MAX_LFOS; ++lfoIdx)
                    {
                        if (extInfo.lfos[lfoIdx].adsr.stageCount > BAE_RMF_MAX_ADSR_STAGES)
                        {
                            reason |= BAEZMF_REASON_EXTENDED_ADSR;
                            break;
                        }
                    }
                }
            }
            if (TEST_FLAG_VALUE(extInfo.flags2, ZBF_advancedInterpolation))
            {
#if defined(_DEBUG) && (_DEBUG != 0)
                if ((reason & BAEZMF_REASON_CUBIC_INTERPOLATION) == 0)
                {
                    uint32_t debugSndID = 0;
                    if (BAERmfEditorBank_GetInstrumentSampleInfo(bankToken,
                                                                 instrumentIndex,
                                                                 0,
                                                                 &firstSampleInfo) == BAE_NO_ERROR)
                    {
                        debugSndID = (uint32_t)(uint16_t)firstSampleInfo.sndResourceID;
                    }
                    debug_message("[BankRequiresZsb] TRIP reason=advanced-interpolation instIndex=%u sndID=%u flags2=0x%02X\n",
                               (unsigned)instrumentIndex,
                               (unsigned)debugSndID,
                               (unsigned)extInfo.flags2);
                }
#endif
                reason |= BAEZMF_REASON_CUBIC_INTERPOLATION;
            }
        }

        sampleCount = 0;
        if (BAERmfEditorBank_GetInstrumentSampleCount(bankToken, instrumentIndex, &sampleCount) != BAE_NO_ERROR)
        {
            continue;
        }

        for (sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            BAERmfEditorBankSampleInfo sampleInfo;
            uint32_t compressionType;

            if (BAERmfEditorBank_GetInstrumentSampleInfo(bankToken,
                                                         instrumentIndex,
                                                         sampleIndex,
                                                         &sampleInfo) != BAE_NO_ERROR)
            {
                continue;
            }

            if (sampleInfo.loopEnd > sampleInfo.loopStart)
            {
                uint32_t loopLength = sampleInfo.loopEnd - sampleInfo.loopStart;
                if (loopLength >= MIN_LOOP_SIZE_ZMF && loopLength < MIN_LOOP_SIZE_RMF)
                {
#if defined(_DEBUG) && (_DEBUG != 0)
                    if ((reason & BAEZMF_REASON_LOOP_TOO_SHORT) == 0)
                    {
                        debug_message("[BankRequiresZsb] TRIP reason=short-loop-rmf-window instIndex=%u sndID=%u loopStart=%u loopEnd=%u loopLen=%u minZmf=%u minRmf=%u\n",
                                   (unsigned)instrumentIndex,
                                   (unsigned)(uint16_t)sampleInfo.sndResourceID,
                                   (unsigned)sampleInfo.loopStart,
                                   (unsigned)sampleInfo.loopEnd,
                                   (unsigned)loopLength,
                                   (unsigned)MIN_LOOP_SIZE_ZMF,
                                   (unsigned)MIN_LOOP_SIZE_RMF);
                    }
#endif
                    reason |= BAEZMF_REASON_LOOP_TOO_SHORT;
                }
            }

            compressionType = (uint32_t)sampleInfo.compressionType;
            if (
#if USE_FLAC_DECODER == TRUE || USE_FLAC_ENCODER == TRUE
                compressionType == (uint32_t)C_FLAC ||
#endif                
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
                compressionType == (uint32_t)C_VORBIS ||
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
                compressionType == (uint32_t)C_OPUS ||
#endif
#if USE_QOA_SUPPORT == TRUE
                compressionType == (uint32_t)C_QOA ||
#endif
            false)
            {
#if defined(_DEBUG) && (_DEBUG != 0)
                if ((reason & BAEZMF_REASON_MODERN_CODEC) == 0)
                {
                    debug_message("[BankRequiresZsb] TRIP reason=modern-codec instIndex=%u sndID=%u compressionType=0x%08X\n",
                               (unsigned)instrumentIndex,
                               (unsigned)(uint16_t)sampleInfo.sndResourceID,
                               (unsigned)compressionType);
                }
#endif
                reason |= BAEZMF_REASON_MODERN_CODEC;
            }
        }
    }
    if (outReason)
    {
        *outReason = reason;
    }
    return (reason != BAEZMF_REASON_NONE) ? TRUE : FALSE;
}

// end ZMF Requirements detection

/* ---------- Bank instrument enumeration and cloning ---------- */

BAEResult BAERmfEditorBank_GetInstrumentCount(BAEBankToken bankToken,
                                              uint32_t *outCount)
{
    XFILE bankFile;
    int32_t count;

    if (!outCount)
    {
        return BAE_PARAM_ERR;
    }
    *outCount = 0;
    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;
    count = XCountFileResourcesOfType(bankFile, ID_INST);
    if (count < 0)
    {
        count = 0;
    }
    *outCount = (uint32_t)count;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorBank_GetInstrumentInfo(BAEBankToken bankToken,
                                             uint32_t instrumentIndex,
                                             BAERmfEditorBankInstrumentInfo *outInfo)
{
    enum
    {
        kInstHeaderMinSize = 14
    };
    XFILE bankFile;
    XPTR instData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    InstrumentResource *inst;

    if (!outInfo)
    {
        return BAE_PARAM_ERR;
    }
    XSetMemory(outInfo, (int32_t)sizeof(*outInfo), 0);
    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

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
    inst = (InstrumentResource *)instData;

    outInfo->instID = (uint32_t)instID;
    PV_DecodeResourceName(rawName, outInfo->name);
    outInfo->program = (unsigned char)(instID % 128);
    outInfo->bank = (uint16_t)(instID / 256);
    outInfo->keySplitCount = (int16_t)XGetShort(&inst->keySplitCount);
    outInfo->flags1 = inst->flags1;
    outInfo->flags2 = inst->flags2;

    XDisposePtr(instData);
    return BAE_NO_ERROR;
}

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
                splitRootForLoad = PV_ClampMidi7Bit(baseRootKey);
            }

            sampleName[0] = 0;
            if (PV_GetEmbeddedSampleDisplayName(bankFile, split.sndResourceID, sampleName) != BAE_NO_ERROR)
            {
                XStrCpy(sampleName, instName);
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
                newSample->sampleAssetID = PV_AllocateSampleAssetID(document);
                newSample->splitVolume = split.miscParameter2;
            }
        }
    }
    else
    {
        unsigned char nonSplitRootForLoad;
        char sampleName[256];

        if (useSoundModifierAsRootKey)
        {
            nonSplitRootForLoad = PV_ClampMidi7Bit(instMiscParam1 ? instMiscParam1 : baseRootKey);
        }
        else
        {
            nonSplitRootForLoad = PV_ClampMidi7Bit(baseRootKey);
        }

        sampleName[0] = 0;
        if (PV_GetEmbeddedSampleDisplayName(bankFile, baseSndID, sampleName) != BAE_NO_ERROR)
        {
            XStrCpy(sampleName, instName);
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
            BAERmfEditorSample *newSample = &document->samples[document->sampleCount - 1];
            newSample->sampleAssetID = PV_AllocateSampleAssetID(document);
            newSample->splitVolume = (int16_t)XGetShort(&inst->miscParameter2);
        }
    }

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

    if (!document || !bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

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
                splitRootForLoad = PV_ClampMidi7Bit(baseRootKey);
            }

            sampleName[0] = 0;
            if (PV_GetEmbeddedSampleDisplayName(bankFile, split.sndResourceID, sampleName) != BAE_NO_ERROR)
            {
                XStrCpy(sampleName, instName);
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
    else
    {
        unsigned char nonSplitRootForLoad;
        char sampleName[256];

        if (useSoundModifierAsRootKey)
        {
            nonSplitRootForLoad = PV_ClampMidi7Bit(instMiscParam1 ? instMiscParam1 : baseRootKey);
        }
        else
        {
            nonSplitRootForLoad = PV_ClampMidi7Bit(baseRootKey);
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

BAEResult BAERmfEditorBank_ResolveInstID(BAEBankToken bankToken,
                                         uint32_t instID,
                                         uint32_t *outResolvedInstID,
                                         uint32_t *outInstrumentIndex)
{
    XFILE bankFile;
    int32_t totalInst;
    int32_t i;
    uint32_t resolvedID;

    if (!outResolvedInstID || !outInstrumentIndex)
    {
        return BAE_PARAM_ERR;
    }
    *outResolvedInstID = instID;
    *outInstrumentIndex = 0;
    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

    /* Try the requested ID first, then fall back to alias resolution. */
    resolvedID = instID;
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 1)
        {
            /* Second pass: check alias table. */
            XAliasLinkResource *pAlias;
            XLongResourceID aliasTarget;

            pAlias = XGetAliasLinkFromFile(bankFile);
            if (!pAlias)
            {
                return BAE_BAD_FILE;
            }
            if (XLookupAlias(pAlias, (XLongResourceID)instID, &aliasTarget) != 0)
            {
                XDisposePtr((XPTR)pAlias);
                return BAE_BAD_FILE;
            }
            XDisposePtr((XPTR)pAlias);
            resolvedID = (uint32_t)aliasTarget;
        }

        totalInst = XCountFileResourcesOfType(bankFile, ID_INST);
        for (i = 0; i < totalInst; ++i)
        {
            XLongResourceID thisID;
            int32_t size;
            XPTR data;

            data = XGetIndexedFileResource(bankFile, ID_INST, &thisID,
                                           i, NULL, &size);
            if (data)
            {
                XDisposePtr(data);
                if ((uint32_t)thisID == resolvedID)
                {
                    *outResolvedInstID = resolvedID;
                    *outInstrumentIndex = (uint32_t)i;
                    return BAE_NO_ERROR;
                }
            }
        }
    }
    return BAE_BAD_FILE;
}

/* ---------- Bank sample enumeration and editing ---------- */

BAEResult BAERmfEditorBank_GetInstrumentSampleCount(BAEBankToken bankToken,
                                                     uint32_t instrumentIndex,
                                                     uint32_t *outCount)
{
    enum
    {
        kInstHeaderMinSize = 14
    };
    XFILE bankFile;
    XPTR instData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    InstrumentResource *inst;
    int16_t splitCount;

    if (!outCount)
    {
        return BAE_PARAM_ERR;
    }
    *outCount = 0;
    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

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
    inst = (InstrumentResource *)instData;
    splitCount = (int16_t)XGetShort(&inst->keySplitCount);
    if (splitCount < 0)
    {
        splitCount = 0;
    }

    /* Non-split instruments return 1 (the base sample), split instruments return the split count */
    *outCount = (splitCount > 0) ? (uint32_t)splitCount : 1;

    XDisposePtr(instData);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorBank_GetInstrumentSampleInfo(BAEBankToken bankToken,
                                                    uint32_t instrumentIndex,
                                                    uint32_t sampleIndex,
                                                    BAERmfEditorBankSampleInfo *outInfo)
{
    enum
    {
        kInstHeaderMinSize = 14,
        kInstKeySplitSize = 8
    };
    XFILE bankFile;
    XPTR instData;
    XPTR sndData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    InstrumentResource *inst;
    KeySplit split;
    XShortResourceID sndID;
    SampleDataInfo sampleInfo;
    int16_t splitCount;
    int16_t baseRootKey;
    int16_t baseVolume;
    bool useSoundModifierAsRootKey;
    int16_t miscParam1;

    if (!outInfo)
    {
        return BAE_PARAM_ERR;
    }
    XSetMemory(outInfo, (int32_t)sizeof(*outInfo), 0);
    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

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
    inst = (InstrumentResource *)instData;

    splitCount = (int16_t)XGetShort(&inst->keySplitCount);
    if (splitCount < 0)
    {
        splitCount = 0;
    }

    /* Validate sampleIndex */
    if (splitCount > 0)
    {
        if (sampleIndex >= (uint32_t)splitCount)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }
    }
    else
    {
        if (sampleIndex != 0)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }
    }

    outInfo->instID = (uint32_t)instID;
    outInfo->sampleIndex = sampleIndex;
    baseRootKey = (int16_t)XGetShort(&inst->midiRootKey);
    baseVolume = (int16_t)XGetShort(&inst->miscParameter2);
    useSoundModifierAsRootKey = TEST_FLAG_VALUE(inst->flags2, ZBF_useSoundModifierAsRootKey);
    miscParam1 = (int16_t)XGetShort(&inst->miscParameter1);

    if (splitCount > 0)
    {
        /* This is a split instrument - get info from the key split */
        XGetKeySplitFromPtr(inst, (int16_t)sampleIndex, &split);
        sndID = split.sndResourceID;

        outInfo->lowKey = (unsigned char)split.lowMidi;
        outInfo->highKey = (unsigned char)split.highMidi;
        outInfo->splitVolume = split.miscParameter2;

        /* Determine root key */
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
            outInfo->rootKey = (unsigned char)splitRoot;
        }
        else
        {
            outInfo->rootKey = (unsigned char)baseRootKey;
        }
    }
    else
    {
        /* Non-split instrument - use base sample */
        sndID = (XShortResourceID)XGetShort(&inst->sndResourceID);
        outInfo->lowKey = 0;
        outInfo->highKey = 127;
        outInfo->splitVolume = baseVolume;

        /* Determine root key */
        if (useSoundModifierAsRootKey)
        {
            if (miscParam1 > 0 && miscParam1 <= 127)
            {
                outInfo->rootKey = (unsigned char)miscParam1;
            }
            else
            {
                outInfo->rootKey = (unsigned char)baseRootKey;
            }
        }
        else
        {
            outInfo->rootKey = (unsigned char)baseRootKey;
        }
    }

    outInfo->sndResourceID = sndID;

    /* Get SND resource info — prefer ESND/CSND over plain SND (promote collisions). */
    {
        int32_t sndSize;
        static const XResourceType sndTypes[] = { ID_ESND, ID_CSND, ID_SND, 0 };
        int32_t typeIdx;
        XResourceType foundSndType;

        sndData = NULL;
        foundSndType = 0;
        for (typeIdx = 0; sndTypes[typeIdx] != 0; ++typeIdx)
        {
            sndData = XGetFileResource(bankFile, sndTypes[typeIdx], (XLongResourceID)sndID, NULL, &sndSize);
            if (sndData)
            {
                foundSndType = sndTypes[typeIdx];
                /* Decompress CSND (LZSS) or decrypt ESND before reading header */
                if (sndTypes[typeIdx] == ID_CSND)
                {
                    XPTR decompressed = XDecompressPtr(sndData, (uint32_t)sndSize, FALSE);
                    XDisposePtr(sndData);
                    sndData = decompressed;
                    if (sndData)
                    {
                        sndSize = XGetPtrSize(sndData);
                    }
                }
                else if (sndTypes[typeIdx] == ID_ESND)
                {
                    XDecryptData(sndData, (uint32_t)sndSize);
                }
                break;
            }
        }
        if (sndData && sndSize > 0)
        {
            /* Validate the SND resource has a recognizable sound header before
             * calling XGetSampleInfoFromSnd, which asserts on invalid data.
             * A valid SND resource starts with a format type at offset 0:
             * 1 = XFirstSoundFormat, 2 = XSecondSoundFormat, 3 = XThirdSoundFormat */
            int16_t soundFormat = (int16_t)XGetShort(sndData);
            if (soundFormat == 1 || soundFormat == 2 || soundFormat == 3)
            {
                XSetMemory(&sampleInfo, (int32_t)sizeof(sampleInfo), 0);
                if (XGetSampleInfoFromSnd(sndData, &sampleInfo) == 0)
                {
                    outInfo->sampleRate = (uint32_t)XFIXED_TO_UNSIGNED_LONG(sampleInfo.rate);
                    outInfo->frameCount = sampleInfo.frames;
                    outInfo->bitDepth = sampleInfo.bitSize;
                    outInfo->channels = sampleInfo.channels;
                    outInfo->loopStart = sampleInfo.loopStart;
                    outInfo->loopEnd = sampleInfo.loopEnd;
                    outInfo->compressionType = sampleInfo.compressionType;
                    outInfo->compressionSubType = PV_GetStoredCompressionSubTypeFromSnd(
                        sndData, sndSize, (uint32_t)sampleInfo.compressionType);
                    outInfo->opusRoundTripResample = XGetSoundOpusRoundTripFlag(sndData);
                }
            }
            XDisposePtr(sndData);
        }

        /* Report the container type so callers can reflect it in the UI */
        if (foundSndType == ID_CSND)
        {
            outInfo->sndStorageType = BAE_EDITOR_SND_STORAGE_CSND;
        }
        else if (foundSndType == ID_SND)
        {
            outInfo->sndStorageType = BAE_EDITOR_SND_STORAGE_SND;
        }
        else
        {
            outInfo->sndStorageType = BAE_EDITOR_SND_STORAGE_ESND; /* default / ESND */
        }
    }

    XDisposePtr(instData);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorBank_GetInstrumentExtInfo(BAEBankToken bankToken,
                                                 uint32_t instrumentIndex,
                                                 BAERmfEditorInstrumentExtInfo *outInfo)
{
    enum
    {
        kInstHeaderMinSize = 14
    };
    XFILE bankFile;
    XPTR instData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    char instName[256];
    uint32_t i;

    if (!outInfo)
    {
        return BAE_PARAM_ERR;
    }
    XSetMemory(outInfo, (int32_t)sizeof(*outInfo), 0);
    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

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

    /* Get basic INST header info */
    PV_DecodeResourceName(rawName, instName);
    outInfo->instID = (uint32_t)instID;
    outInfo->displayName = instName[0] ? instName : NULL;
    outInfo->flags1 = ((unsigned char *)instData)[5];
    /* Ghost is RMF/ZMF document-only; ignore if present on bank INST bytes. */
    outInfo->flags1 = (unsigned char)(outInfo->flags1 & (unsigned char)~ZBF_ghostInstrument);
    outInfo->flags2 = ((unsigned char *)instData)[6];
    outInfo->panPlacement = ((unsigned char *)instData)[4];
    outInfo->midiRootKey = (int16_t)XGetShort(((unsigned char *)instData) + 2);
    outInfo->miscParameter1 = (int16_t)XGetShort(((unsigned char *)instData) + 8);
    outInfo->miscParameter2 = (int16_t)XGetShort(((unsigned char *)instData) + 10);

    /* Parse extended data using existing function */
    {
        BAERmfEditorInstrumentExt extData;
        PV_ParseExtendedInstData(instData, instSize, &extData);
        outInfo->hasExtendedData = extData.hasExtendedData;
        outInfo->hasDefaultMod = extData.hasDefaultMod;
        outInfo->LPF_frequency = extData.LPF_frequency;
        outInfo->LPF_resonance = extData.LPF_resonance;
        outInfo->LPF_lowpassAmount = extData.LPF_lowpassAmount;
        outInfo->defaultReverbSend = extData.defaultReverbSend;
        outInfo->defaultChorusSend = extData.defaultChorusSend;
        PV_CopyEditorADSRToInfo(&extData.volumeADSR, &outInfo->volumeADSR);
        outInfo->lfoCount = extData.lfoCount;
        for (i = 0; i < extData.lfoCount && i < BAE_EDITOR_MAX_LFOS; i++)
        {
            outInfo->lfos[i].destination = extData.lfos[i].destination;
            outInfo->lfos[i].period = extData.lfos[i].period;
            outInfo->lfos[i].waveShape = extData.lfos[i].waveShape;
            outInfo->lfos[i].DC_feed = extData.lfos[i].DC_feed;
            outInfo->lfos[i].level = extData.lfos[i].level;
            PV_CopyEditorADSRToInfo(&extData.lfos[i].adsr, &outInfo->lfos[i].adsr);
        }
        outInfo->curveCount = extData.curveCount;
        for (i = 0; i < extData.curveCount && i < BAE_EDITOR_MAX_CURVES; i++)
        {
            uint32_t j;
            outInfo->curves[i].tieFrom = extData.curves[i].tieFrom;
            outInfo->curves[i].tieTo = extData.curves[i].tieTo;
            outInfo->curves[i].curveCount = extData.curves[i].curveCount;
            for (j = 0; j < extData.curves[i].curveCount && j < BAE_EDITOR_MAX_ADSR_STAGES; j++)
            {
                outInfo->curves[i].from_Value[j] = extData.curves[i].from_Value[j];
                outInfo->curves[i].to_Scalar[j] = extData.curves[i].to_Scalar[j];
            }
        }
    }

    XDisposePtr(instData);
    return BAE_NO_ERROR;
}

/* Fast in-place INST update: skips SND/CSND/ESND iteration and rebuilds only INST resources.
   This is ~100x faster than PV_BankReplaceResource for metadata-only changes since we avoid
   copying non-INST resources (which are expensive). */
static BAEResult PV_BankReplaceInstResourceInPlace(XFILE bankFile,
                                                   XLongResourceID instID,
                                                   char const *pascalName,
                                                   XPTR instData,
                                                   int32_t instSize)
{
    XFILERESOURCEMAP map;
    int32_t resourceID;
    XFILE outFile;
    int32_t instIndex = 0;
    int32_t instCount = 0;
    XPTR packedData;
    int32_t packedSize;
    bool replaced;

    if (XFileSetPosition(bankFile, 0L) != 0 ||
        XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_BAD_FILE;
    }
    resourceID = (int32_t)XGetLong(&map.mapID);
    if (!XFILERESOURCE_ID_IS_VALID(resourceID))
    {
        return BAE_BAD_FILE;
    }

    outFile = XFileOpenVirtualResource(resourceID);
    if (!outFile)
    {
        return BAE_MEMORY_ERR;
    }

    /* Only iterate INST resources (skip SND/CSND/ESND/etc) */
    instCount = XCountFileResourcesOfType(bankFile, ID_INST);
    replaced = FALSE;
    for (instIndex = 0; instIndex < instCount; ++instIndex)
    {
        XLongResourceID resID;
        int32_t resSize;
        XPTR resData;
        char resName[256];

        resName[0] = 0;
        resData = XGetIndexedFileResource(bankFile,
                                          ID_INST,
                                          &resID,
                                          instIndex,
                                          resName,
                                          &resSize);
        if (!resData)
        {
            continue;
        }

        if (resID == instID)
        {
            if (!replaced)
            {
                char const *replaceName = pascalName ? pascalName : resName;
                if (XAddFileResource(outFile,
                                     ID_INST,
                                     instID,
                                     replaceName,
                                     instData,
                                     instSize) != 0)
                {
                    XDisposePtr(resData);
                    XFileClose(outFile);
                    return BAE_FILE_IO_ERROR;
                }
                replaced = TRUE;
            }
            XDisposePtr(resData);
            continue;
        }

        /* Copy unchanged INST resources */
        if (XAddFileResource(outFile, ID_INST, resID, resName, resData, resSize) != 0)
        {
            XDisposePtr(resData);
            XFileClose(outFile);
            return BAE_FILE_IO_ERROR;
        }
        XDisposePtr(resData);
    }

    if (!replaced)
    {
        if (XAddFileResource(outFile,
                             ID_INST,
                             instID,
                             pascalName,
                             instData,
                             instSize) != 0)
        {
            XFileClose(outFile);
            return BAE_FILE_IO_ERROR;
        }
    }

    /* Copy all non-INST resources (SND/CSND/ESND/ALIAS/BANK/etc.) verbatim */
    {
        static const XResourceType nonInstTypes[] = {
            ID_SND, ID_CSND, ID_ESND, ID_ALIAS, ID_BANK,
            ID_SONG, ID_MIDI, ID_MIDI_OLD, ID_CMID,
            ID_EMID, ID_ECMI, ID_RMF, ID_TEXT, ID_VERS, 0
        };
        int niIdx;
        for (niIdx = 0; nonInstTypes[niIdx] != 0; ++niIdx)
        {
            XResourceType resType = nonInstTypes[niIdx];
            int32_t resCount = XCountFileResourcesOfType(bankFile, resType);
            int32_t resIndex;

            for (resIndex = 0; resIndex < resCount; ++resIndex)
            {
                XLongResourceID resID;
                int32_t resSize;
                XPTR resData;
                char resName[256];

                resName[0] = 0;
                resData = XGetIndexedFileResource(bankFile,
                                                  resType,
                                                  &resID,
                                                  resIndex,
                                                  resName,
                                                  &resSize);
                if (!resData)
                {
                    continue;
                }

                if (XAddFileResource(outFile, resType, resID, resName, resData, resSize) != 0)
                {
                    XDisposePtr(resData);
                    XFileClose(outFile);
                    return BAE_FILE_IO_ERROR;
                }
                XDisposePtr(resData);
            }
        }
    }

    if (XCleanResourceFile(outFile) == FALSE)
    {
        XFileClose(outFile);
        return BAE_FILE_IO_ERROR;
    }

    packedData = NULL;
    packedSize = 0;
    if (XFileGetMemoryFileAsData(outFile, &packedData, &packedSize) != 0 ||
        !packedData || packedSize <= 0)
    {
        XFileClose(outFile);
        if (packedData)
        {
            XDisposePtr(packedData);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(outFile);

    if (bankFile->pCache)
    {
        XDisposePtr(bankFile->pCache);
        bankFile->pCache = NULL;
    }
    if (bankFile->pResourceData && bankFile->ownsResourceData)
    {
        XDisposePtr(bankFile->pResourceData);
    }

    bankFile->pResourceData = packedData;
    bankFile->resMemLength = packedSize;
    bankFile->resMemOffset = 0;
    bankFile->ownsResourceData = TRUE;
    bankFile->resizeResourceData = TRUE;
    bankFile->readOnly = FALSE;
    bankFile->allowMemCopy = TRUE;
    return BAE_NO_ERROR;
}

/* Fast in-place SND update: skips INST/ALIAS/BANK/etc iteration and rebuilds only SND/CSND/ESND.
   This is ~100x faster than PV_BankReplaceResource for sgain since we avoid copying
   non-audio resources (especially large INST resources). 
   Handles type changes (e.g., SND -> CSND) just like PV_BankReplaceResourceEx.
   When the bank is in batch mode (pendingSndBatch != NULL), the replacement is
   queued and no rebuild occurs until BAERmfEditorBank_CommitBatchSnd is called. */
static BAEResult PV_BankReplaceSndResourceInPlace(XFILE bankFile,
                                                  XResourceType oldSndType,
                                                  XResourceType newSndType,
                                                  XShortResourceID sndID,
                                                  char const *pascalName,
                                                  XPTR sndData,
                                                  int32_t sndSize)
{
    static const XResourceType sndTypes[] = { ID_SND, ID_CSND, ID_ESND, 0 };
    XFILERESOURCEMAP map;
    int32_t resourceID;
    XFILE outFile;
    XPTR packedData;
    int32_t packedSize;
    bool replaced;
    int typeIdx;

    /* Batch mode: queue the replacement instead of rebuilding now */
    if (bankFile->pendingSndBatch != NULL)
    {
        struct XFilePendingSnd *entry;
        XPTR dataCopy;

        /* Grow the array if needed */
        if (bankFile->pendingSndCount >= bankFile->pendingSndCapacity)
        {
            int32_t newCap = bankFile->pendingSndCapacity ? bankFile->pendingSndCapacity * 2 : 64;
            struct XFilePendingSnd *newArr = (struct XFilePendingSnd *)XNewPtr(
                (int32_t)(newCap * (int32_t)sizeof(struct XFilePendingSnd)));
            if (!newArr)
            {
                return BAE_MEMORY_ERR;
            }
            if (bankFile->pendingSndBatch)
            {
                XBlockMove(bankFile->pendingSndBatch, newArr,
                           bankFile->pendingSndCount * (int32_t)sizeof(struct XFilePendingSnd));
                XDisposePtr(bankFile->pendingSndBatch);
            }
            bankFile->pendingSndBatch = newArr;
            bankFile->pendingSndCapacity = newCap;
        }

        /* Copy the SND data so the caller can free their copy */
        dataCopy = XNewPtr(sndSize);
        if (!dataCopy)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(sndData, dataCopy, sndSize);

        entry = &bankFile->pendingSndBatch[bankFile->pendingSndCount++];
        entry->oldType = (int32_t)oldSndType;
        entry->newType = (int32_t)newSndType;
        entry->sndID   = (int16_t)sndID;
        entry->data    = dataCopy;
        entry->size    = sndSize;
        entry->name[0] = 0;
        if (pascalName)
        {
            int32_t i;
            for (i = 0; i < 255 && pascalName[i]; ++i)
            {
                entry->name[i] = pascalName[i];
            }
            entry->name[i] = 0;
        }
        return BAE_NO_ERROR;
    }

    if (XFileSetPosition(bankFile, 0L) != 0 ||
        XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_BAD_FILE;
    }
    resourceID = (int32_t)XGetLong(&map.mapID);
    if (!XFILERESOURCE_ID_IS_VALID(resourceID))
    {
        return BAE_BAD_FILE;
    }

    outFile = XFileOpenVirtualResource(resourceID);
    if (!outFile)
    {
        return BAE_MEMORY_ERR;
    }

    /* Only iterate SND/CSND/ESND resources (skip INST/ALIAS/BANK/etc) */
    replaced = FALSE;
    for (typeIdx = 0; sndTypes[typeIdx] != 0; ++typeIdx)
    {
        XResourceType resType = sndTypes[typeIdx];
        int32_t resCount = XCountFileResourcesOfType(bankFile, resType);
        int32_t resIndex;

        for (resIndex = 0; resIndex < resCount; ++resIndex)
        {
            XLongResourceID resID;
            int32_t resSize;
            XPTR resData;
            char resName[256];

            resName[0] = 0;
            resData = XGetIndexedFileResource(bankFile,
                                              resType,
                                              &resID,
                                              resIndex,
                                              resName,
                                              &resSize);
            if (!resData)
            {
                continue;
            }

            if (resType == oldSndType && resID == sndID)
            {
                if (!replaced)
                {
                    char const *replaceName = pascalName ? pascalName : resName;
                    /* Use newSndType to allow type changes (SND->CSND, etc) */
                    if (XAddFileResource(outFile,
                                         newSndType,
                                         sndID,
                                         replaceName,
                                         sndData,
                                         sndSize) != 0)
                    {
                        XDisposePtr(resData);
                        XFileClose(outFile);
                        return BAE_FILE_IO_ERROR;
                    }
                    replaced = TRUE;
                }
                XDisposePtr(resData);
                continue;
            }

            if (XAddFileResource(outFile, resType, resID, resName, resData, resSize) != 0)
            {
                XDisposePtr(resData);
                XFileClose(outFile);
                return BAE_FILE_IO_ERROR;
            }
            XDisposePtr(resData);
        }
    }

    if (!replaced)
    {
        if (XAddFileResource(outFile,
                             newSndType,
                             sndID,
                             pascalName,
                             sndData,
                             sndSize) != 0)
        {
            XFileClose(outFile);
            return BAE_FILE_IO_ERROR;
        }
    }

    /* Copy all non-SND resources directly without iterating them individually */
    static const XResourceType nonSndTypes[] = {
        ID_INST, ID_ALIAS, ID_BANK, ID_SONG, ID_MIDI, ID_MIDI_OLD, ID_CMID,
        ID_EMID, ID_ECMI, ID_RMF, ID_TEXT, ID_VERS, 0
    };
    for (typeIdx = 0; nonSndTypes[typeIdx] != 0; ++typeIdx)
    {
        XResourceType resType = nonSndTypes[typeIdx];
        int32_t resCount = XCountFileResourcesOfType(bankFile, resType);
        int32_t resIndex;

        for (resIndex = 0; resIndex < resCount; ++resIndex)
        {
            XLongResourceID resID;
            int32_t resSize;
            XPTR resData;
            char resName[256];

            resName[0] = 0;
            resData = XGetIndexedFileResource(bankFile,
                                              resType,
                                              &resID,
                                              resIndex,
                                              resName,
                                              &resSize);
            if (!resData)
            {
                continue;
            }

            if (XAddFileResource(outFile, resType, resID, resName, resData, resSize) != 0)
            {
                XDisposePtr(resData);
                XFileClose(outFile);
                return BAE_FILE_IO_ERROR;
            }
            XDisposePtr(resData);
        }
    }

    if (XCleanResourceFile(outFile) == FALSE)
    {
        XFileClose(outFile);
        return BAE_FILE_IO_ERROR;
    }

    packedData = NULL;
    packedSize = 0;
    if (XFileGetMemoryFileAsData(outFile, &packedData, &packedSize) != 0 ||
        !packedData || packedSize <= 0)
    {
        XFileClose(outFile);
        if (packedData)
        {
            XDisposePtr(packedData);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(outFile);

    if (bankFile->pCache)
    {
        XDisposePtr(bankFile->pCache);
        bankFile->pCache = NULL;
    }
    if (bankFile->pResourceData && bankFile->ownsResourceData)
    {
        XDisposePtr(bankFile->pResourceData);
    }

    bankFile->pResourceData = packedData;
    bankFile->resMemLength = packedSize;
    bankFile->resMemOffset = 0;
    bankFile->ownsResourceData = TRUE;
    bankFile->resizeResourceData = TRUE;
    bankFile->readOnly = FALSE;
    bankFile->allowMemCopy = TRUE;
    return BAE_NO_ERROR;
}

/* Fast in-place SND update: skips INST/ALIAS/BANK/etc iteration and rebuilds only SND/CSND/ESND.
   This is ~100x faster than PV_BankReplaceResource for sgain since we avoid copying
   non-audio resources (especially large INST resources). */
/* Batch SND replacement: replaces multiple SND/CSND/ESND resources in a single
   bank rebuild. This is the key optimization for sgain - encode all samples first,
   then commit once instead of once per sample. */
typedef struct
{
    XResourceType   oldType;
    XResourceType   newType;
    XShortResourceID sndID;
    char            name[256];
    XPTR            data;
    int32_t         size;
} PV_SndReplacement;

static BAEResult PV_BankReplaceMultipleSndResources(XFILE bankFile,
                                                     PV_SndReplacement const *replacements,
                                                     int32_t replacementCount)
{
    static const XResourceType sndTypes[] = { ID_SND, ID_CSND, ID_ESND, 0 };
    static const XResourceType nonSndTypes[] = {
        ID_INST, ID_ALIAS, ID_BANK, ID_SONG, ID_MIDI, ID_MIDI_OLD, ID_CMID,
        ID_EMID, ID_ECMI, ID_RMF, ID_TEXT, ID_VERS, 0
    };
    XFILERESOURCEMAP map;
    int32_t resourceID;
    XFILE outFile;
    XPTR packedData;
    int32_t packedSize;
    int typeIdx;
    int32_t ri;

    if (!bankFile || !replacements || replacementCount <= 0)
    {
        return BAE_PARAM_ERR;
    }

    if (XFileSetPosition(bankFile, 0L) != 0 ||
        XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_BAD_FILE;
    }
    resourceID = (int32_t)XGetLong(&map.mapID);
    if (!XFILERESOURCE_ID_IS_VALID(resourceID))
    {
        return BAE_BAD_FILE;
    }

    outFile = XFileOpenVirtualResource(resourceID);
    if (!outFile)
    {
        return BAE_MEMORY_ERR;
    }

    /* Walk SND resources; for each one check if it's in the replacement list */
    for (typeIdx = 0; sndTypes[typeIdx] != 0; ++typeIdx)
    {
        XResourceType resType = sndTypes[typeIdx];
        int32_t resCount = XCountFileResourcesOfType(bankFile, resType);
        int32_t resIndex;

        for (resIndex = 0; resIndex < resCount; ++resIndex)
        {
            XLongResourceID resID;
            int32_t resSize;
            XPTR resData;
            char resName[256];
            PV_SndReplacement const *match = NULL;

            resName[0] = 0;
            resData = XGetIndexedFileResource(bankFile, resType, &resID,
                                              resIndex, resName, &resSize);
            if (!resData)
            {
                continue;
            }

            /* Check if this resource is being replaced */
            for (ri = 0; ri < replacementCount; ++ri)
            {
                if (replacements[ri].oldType == resType &&
                    (XLongResourceID)replacements[ri].sndID == resID)
                {
                    match = &replacements[ri];
                    break;
                }
            }

            if (match)
            {
                char const *useName = match->name[0] ? match->name : resName;
                if (XAddFileResource(outFile, match->newType, resID,
                                     useName, match->data, match->size) != 0)
                {
                    XDisposePtr(resData);
                    XFileClose(outFile);
                    return BAE_FILE_IO_ERROR;
                }
            }
            else
            {
                if (XAddFileResource(outFile, resType, resID, resName,
                                     resData, resSize) != 0)
                {
                    XDisposePtr(resData);
                    XFileClose(outFile);
                    return BAE_FILE_IO_ERROR;
                }
            }
            XDisposePtr(resData);
        }
    }

    /* Copy all non-SND resources unchanged */
    for (typeIdx = 0; nonSndTypes[typeIdx] != 0; ++typeIdx)
    {
        XResourceType resType = nonSndTypes[typeIdx];
        int32_t resCount = XCountFileResourcesOfType(bankFile, resType);
        int32_t resIndex;

        for (resIndex = 0; resIndex < resCount; ++resIndex)
        {
            XLongResourceID resID;
            int32_t resSize;
            XPTR resData;
            char resName[256];

            resName[0] = 0;
            resData = XGetIndexedFileResource(bankFile, resType, &resID,
                                              resIndex, resName, &resSize);
            if (!resData)
            {
                continue;
            }
            if (XAddFileResource(outFile, resType, resID, resName,
                                 resData, resSize) != 0)
            {
                XDisposePtr(resData);
                XFileClose(outFile);
                return BAE_FILE_IO_ERROR;
            }
            XDisposePtr(resData);
        }
    }

    if (XCleanResourceFile(outFile) == FALSE)
    {
        XFileClose(outFile);
        return BAE_FILE_IO_ERROR;
    }

    packedData = NULL;
    packedSize = 0;
    if (XFileGetMemoryFileAsData(outFile, &packedData, &packedSize) != 0 ||
        !packedData || packedSize <= 0)
    {
        XFileClose(outFile);
        if (packedData)
        {
            XDisposePtr(packedData);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(outFile);

    if (bankFile->pCache)
    {
        XDisposePtr(bankFile->pCache);
        bankFile->pCache = NULL;
    }
    if (bankFile->pResourceData && bankFile->ownsResourceData)
    {
        XDisposePtr(bankFile->pResourceData);
    }

    bankFile->pResourceData = packedData;
    bankFile->resMemLength = packedSize;
    bankFile->resMemOffset = 0;
    bankFile->ownsResourceData = TRUE;
    bankFile->resizeResourceData = TRUE;
    bankFile->readOnly = FALSE;
    bankFile->allowMemCopy = TRUE;
    return BAE_NO_ERROR;
}

static BAEResult PV_BankReplaceResource(XFILE bankFile,
                                        XResourceType type,
                                        XLongResourceID id,
                                        char const *pascalName,
                                        XPTR data,
                                        int32_t size)
{
    static const XResourceType bankResourceTypes[] = {
        ID_INST,
        ID_SND,
        ID_CSND,
        ID_ESND,
        ID_ALIAS,
        ID_BANK,
        ID_SONG,
        ID_MIDI,
        ID_MIDI_OLD,
        ID_CMID,
        ID_EMID,
        ID_ECMI,
        ID_RMF,
        ID_TEXT,
        ID_VERS,
        0
    };
    XFILERESOURCEMAP map;
    int32_t resourceID;
    XFILE outFile;
    int32_t typeIdx;
    XPTR packedData;
    int32_t packedSize;
    bool replaced;
    uint32_t type_u32 = (uint32_t)type;
    debug_message("[PV_BankReplaceResource] start: type=%c%c%c%c id=%d\n", (type_u32>>24)&0xFF, (type_u32>>16)&0xFF, (type_u32>>8)&0xFF, type_u32&0xFF, (int)id);

    debug_message("[PV_BankReplaceResource] step 1: Write map...\n");

    if (XFileSetPosition(bankFile, 0L) != 0 ||
        XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_BAD_FILE;
    }
    resourceID = (int32_t)XGetLong(&map.mapID);
    if (!XFILERESOURCE_ID_IS_VALID(resourceID))
    {
        return BAE_BAD_FILE;
    }

    debug_message("[PV_BankReplaceResource] step 2: XFileOpenVirtualResource...\n");
    outFile = XFileOpenVirtualResource(resourceID);
    if (!outFile)
    {
        return BAE_MEMORY_ERR;
    }

    debug_message("[PV_BankReplaceResource] step 3: Loop bankResourceTypes...\n");
    replaced = FALSE;
    for (typeIdx = 0; bankResourceTypes[typeIdx] != 0; ++typeIdx)
    {
        XResourceType resType;
        int32_t resCount = 0;
        int32_t resIndex = 0;

        resType = bankResourceTypes[typeIdx];
        resCount = XCountFileResourcesOfType(bankFile, resType);
        debug_message("[PV_BankReplaceResource] Loop over resIndex %d of %d (type=%c%c%c%c)...\n", (int)resIndex, (int)resCount, ((uint32_t)resType>>24)&0xFF, ((uint32_t)resType>>16)&0xFF, ((uint32_t)resType>>8)&0xFF, (uint32_t)resType&0xFF);
        for (resIndex = 0; resIndex < resCount; ++resIndex)
        {
            XLongResourceID resID;
            int32_t resSize;
            XPTR resData;
            char resName[256];
            uint32_t startTicks = XMicroseconds();

            resName[0] = 0;
            resData = XGetIndexedFileResource(bankFile,
                                              resType,
                                              &resID,
                                              resIndex,
                                              resName,
                                              &resSize);
            uint32_t midTicks = XMicroseconds();
            if (!resData)
            {
                continue;
            }

            if (resType == type && resID == id)
            {
                if (!replaced)
                {
                    char const *replaceName;

                    replaceName = pascalName ? pascalName : resName;
                    if (XAddFileResource(outFile,
                                         type,
                                         id,
                                         replaceName,
                                         data,
                                         size) != 0)
                    {
                        XDisposePtr(resData);
                        XFileClose(outFile);
                        return BAE_FILE_IO_ERROR;
                    }
                    replaced = TRUE;
                }
                XDisposePtr(resData);
                continue;
            }

            if (XAddFileResource(outFile, resType, resID, resName, resData, resSize) != 0)
            {
                XDisposePtr(resData);
                XFileClose(outFile);
                return BAE_FILE_IO_ERROR;
            }
            uint32_t endTicks = XMicroseconds();
            if (endTicks - startTicks > 50000) {
                debug_message("[PV_BankReplaceResource] Slow resource copy: resIndex=%d resType=%c%c%c%c resID=%d size=%d XGetIndexedFileResource=%uus XAddFileResource=%uus\n", (int)resIndex, ((uint32_t)resType>>24)&0xFF, ((uint32_t)resType>>16)&0xFF, ((uint32_t)resType>>8)&0xFF, (uint32_t)resType&0xFF, (int)resID, (int)resSize, (unsigned)(midTicks - startTicks), (unsigned)(endTicks - midTicks));
            }
            XDisposePtr(resData);
        }
    }

    debug_message("[PV_BankReplaceResource] step 4: Out of loop. Replace if needed...\n");
    if (!replaced)
    {
        if (XAddFileResource(outFile,
                             type,
                             id,
                             pascalName,
                             data,
                             size) != 0)
        {
            XFileClose(outFile);
            return BAE_FILE_IO_ERROR;
        }
    }

    debug_message("[PV_BankReplaceResource] step 5: XCleanResourceFile...\n");
    if (XCleanResourceFile(outFile) == FALSE)
    {
        XFileClose(outFile);
        return BAE_FILE_IO_ERROR;
    }

    debug_message("[PV_BankReplaceResource] step 6: XFileGetMemoryFileAsData...\n");
    packedData = NULL;
    packedSize = 0;
    if (XFileGetMemoryFileAsData(outFile, &packedData, &packedSize) != 0 ||
        !packedData || packedSize <= 0)
    {
        XFileClose(outFile);
        if (packedData)
        {
            XDisposePtr(packedData);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(outFile);

    if (bankFile->pCache)
    {
        XDisposePtr(bankFile->pCache);
        bankFile->pCache = NULL;
    }
    if (bankFile->pResourceData && bankFile->ownsResourceData)
    {
        XDisposePtr(bankFile->pResourceData);
    }

    bankFile->pResourceData = packedData;
    bankFile->resMemLength = packedSize;
    bankFile->resMemOffset = 0;
    bankFile->ownsResourceData = TRUE;
    bankFile->resizeResourceData = TRUE;
    bankFile->readOnly = FALSE;
    bankFile->allowMemCopy = TRUE;
    return BAE_NO_ERROR;
}

/* outName is a Pascal string (same form XGetFileResource / XAddFileResource use).
   Do not fill it via XGetFileResourceName — that returns a C string, and writing
   it back through XAddFileResource treats the first character as a length byte
   (each save drops another leading character). */
static BAEResult PV_BankFindSndResource(XFILE bankFile,
                                        XShortResourceID sndID,
                                        XResourceType *outType,
                                        XPTR *outData,
                                        int32_t *outSize,
                                        char outName[256])
{
    static const XResourceType sndTypes[] = { ID_ESND, ID_CSND, ID_SND, 0 };
    int32_t typeIdx;

    if (!bankFile || !outType || !outData || !outSize)
    {
        return BAE_PARAM_ERR;
    }
    *outType = 0;
    *outData = NULL;
    *outSize = 0;
    if (outName)
    {
        outName[0] = 0;
    }

    for (typeIdx = 0; sndTypes[typeIdx] != 0; ++typeIdx)
    {
        XPTR data;
        int32_t size;
        char pascalName[256];

        pascalName[0] = 0;
        data = XGetFileResource(bankFile,
                                sndTypes[typeIdx],
                                (XLongResourceID)sndID,
                                pascalName,
                                &size);
        if (data)
        {
            if (outName)
            {
                XBlockMove(pascalName, outName, (int32_t)((unsigned char)pascalName[0]) + 1);
            }
            *outType = sndTypes[typeIdx];
            *outData = data;
            *outSize = size;
            return BAE_NO_ERROR;
        }
    }

    return BAE_BAD_FILE;
}

static BAEResult PV_BankRewrapSndForType(XFILE bankFile,
                                         XResourceType sndType,
                                         XPTR plainSnd,
                                         int32_t plainSndSize,
                                         XPTR *outWrapped,
                                         int32_t *outWrappedSize)
{
    XPTR wrapped;
    int32_t wrappedSize;

    if (!bankFile || !plainSnd || plainSndSize <= 0 || !outWrapped || !outWrappedSize)
    {
        return BAE_PARAM_ERR;
    }
    *outWrapped = NULL;
    *outWrappedSize = 0;

    if (sndType == ID_SND)
    {
        wrapped = XNewPtr(plainSndSize);
        if (!wrapped)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(plainSnd, wrapped, plainSndSize);
        *outWrapped = wrapped;
        *outWrappedSize = plainSndSize;
        return BAE_NO_ERROR;
    }

    if (sndType == ID_ESND)
    {
        wrapped = XNewPtr(plainSndSize);
        if (!wrapped)
        {
            return BAE_MEMORY_ERR;
        }
        XBlockMove(plainSnd, wrapped, plainSndSize);
        XEncryptData(wrapped, (uint32_t)plainSndSize);
        *outWrapped = wrapped;
        *outWrappedSize = plainSndSize;
        return BAE_NO_ERROR;
    }

    if (sndType == ID_CSND)
    {
        XFILERESOURCEMAP map;
        int32_t mapID;
        XCOMPRESSION_TYPE compType;

        if (XFileSetPosition(bankFile, 0L) != 0 ||
            XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
        {
            return BAE_BAD_FILE;
        }
        mapID = (int32_t)XGetLong(&map.mapID);
#if USE_LZMA_COMPRESSION == TRUE
        compType = (mapID == XFILERESOURCE_ZMF_ID) ? X_LZMA_RAW : X_RAW;
#else
        compType = X_RAW;
#endif
        wrapped = NULL;
        wrappedSize = XCompressPtr(&wrapped,
                                   plainSnd,
                                   (uint32_t)plainSndSize,
                                   compType,
                                   NULL,
                                   NULL);
        if (wrappedSize <= 0 || !wrapped)
        {
            if (wrapped)
            {
                XDisposePtr(wrapped);
            }
            return BAE_COMPRESSION_INEFFECTIVE;
        }
        *outWrapped = wrapped;
        *outWrappedSize = wrappedSize;
        return BAE_NO_ERROR;
    }

    return BAE_PARAM_ERR;
}

/* Like PV_BankReplaceResource, but allows the container type to change.
 * Scans the bank and when a resource matching (oldType, id) is found it is
 * replaced with (newType, id, data).  When oldType == newType this is
 * identical to PV_BankReplaceResource. */
static BAEResult PV_BankReplaceResourceEx(XFILE bankFile,
                                           XResourceType oldType,
                                           XResourceType newType,
                                           XLongResourceID id,
                                           char const *pascalName,
                                           XPTR data,
                                           int32_t size)
{
    static const XResourceType bankResourceTypes[] = {
        ID_INST,
        ID_SND,
        ID_CSND,
        ID_ESND,
        ID_ALIAS,
        ID_BANK,
        ID_SONG,
        ID_MIDI,
        ID_MIDI_OLD,
        ID_CMID,
        ID_EMID,
        ID_ECMI,
        ID_RMF,
        ID_TEXT,
        ID_VERS,
        0
    };
    XFILERESOURCEMAP map;
    int32_t resourceID;
    XFILE outFile;
    int32_t typeIdx;
    XPTR packedData;
    int32_t packedSize;
    bool replaced;

    if (!bankFile || !data || size <= 0)
    {
        return BAE_PARAM_ERR;
    }
    if (XFileSetPosition(bankFile, 0L) != 0 ||
        XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_BAD_FILE;
    }
    resourceID = (int32_t)XGetLong(&map.mapID);
    if (!XFILERESOURCE_ID_IS_VALID(resourceID))
    {
        return BAE_BAD_FILE;
    }

    outFile = XFileOpenVirtualResource(resourceID);
    if (!outFile)
    {
        return BAE_MEMORY_ERR;
    }

    replaced = FALSE;
    for (typeIdx = 0; bankResourceTypes[typeIdx] != 0; ++typeIdx)
    {
        XResourceType resType;
        int32_t resCount;
        int32_t resIndex;

        resType = bankResourceTypes[typeIdx];
        resCount = XCountFileResourcesOfType(bankFile, resType);
        for (resIndex = 0; resIndex < resCount; ++resIndex)
        {
            XLongResourceID resID;
            int32_t resSize;
            XPTR resData;
            char resName[256];

            resName[0] = 0;
            resData = XGetIndexedFileResource(bankFile,
                                              resType,
                                              &resID,
                                              resIndex,
                                              resName,
                                              &resSize);
            if (!resData)
            {
                continue;
            }

            if (resType == oldType && resID == id)
            {
                if (!replaced)
                {
                    char const *replaceName;

                    replaceName = pascalName ? pascalName : resName;
                    /* Write with newType instead of oldType (handles container change) */
                    if (XAddFileResource(outFile,
                                         newType,
                                         id,
                                         replaceName,
                                         data,
                                         size) != 0)
                    {
                        XDisposePtr(resData);
                        XFileClose(outFile);
                        return BAE_FILE_IO_ERROR;
                    }
                    replaced = TRUE;
                }
                XDisposePtr(resData);
                continue;
            }

            if (XAddFileResource(outFile, resType, resID, resName, resData, resSize) != 0)
            {
                XDisposePtr(resData);
                XFileClose(outFile);
                return BAE_FILE_IO_ERROR;
            }
            XDisposePtr(resData);
        }
    }

    if (!replaced)
    {
        if (XAddFileResource(outFile, newType, id, pascalName, data, size) != 0)
        {
            XFileClose(outFile);
            return BAE_FILE_IO_ERROR;
        }
    }

    if (XCleanResourceFile(outFile) == FALSE)
    {
        XFileClose(outFile);
        return BAE_FILE_IO_ERROR;
    }

    packedData = NULL;
    packedSize = 0;
    if (XFileGetMemoryFileAsData(outFile, &packedData, &packedSize) != 0 ||
        !packedData || packedSize <= 0)
    {
        XFileClose(outFile);
        if (packedData)
        {
            XDisposePtr(packedData);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(outFile);

    if (bankFile->pCache)
    {
        XDisposePtr(bankFile->pCache);
        bankFile->pCache = NULL;
    }
    if (bankFile->pResourceData && bankFile->ownsResourceData)
    {
        XDisposePtr(bankFile->pResourceData);
    }

    bankFile->pResourceData = packedData;
    bankFile->resMemLength = packedSize;
    bankFile->resMemOffset = 0;
    bankFile->ownsResourceData = TRUE;
    bankFile->resizeResourceData = TRUE;
    bankFile->readOnly = FALSE;
    bankFile->allowMemCopy = TRUE;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorBank_SetInstrumentExtInfo(BAEBankToken bankToken,
                                                uint32_t instrumentIndex,
                                                BAERmfEditorInstrumentExtInfo const *info)
{
    enum
    {
        kInstHeaderMinSize = 14,
        kInstKeySplitSize = 8,
        kInstTailSize = 10
    };
    XFILE bankFile;
    XPTR instData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    unsigned char *instBytes;
    int16_t keySplitCount;
    int32_t baseSize;
    int32_t extTailSize;
    XPTR extTail;
    BAERmfEditorInstrumentExt ext;
    uint32_t i;
    BAEResult replaceResult;

    if (!bankToken || !info)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

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

    keySplitCount = (int16_t)XGetShort((unsigned char *)instData + 12);
    if (keySplitCount < 0)
    {
        keySplitCount = 0;
    }
    baseSize = kInstHeaderMinSize + (keySplitCount * kInstKeySplitSize) + kInstTailSize;
    if (instSize < baseSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
    }

    XSetMemory(&ext, (int32_t)sizeof(ext), 0);
    ext.instID = (XLongResourceID)info->instID;
    ext.hasExtendedData = info->hasExtendedData;
    ext.flags1 = info->flags1;
    /* HSB/ZSB banks always embed their own SNDs — never persist ghost. */
    ext.flags1 = (unsigned char)(ext.flags1 & (unsigned char)~ZBF_ghostInstrument);
    ext.flags2 = info->flags2;
    ext.panPlacement = info->panPlacement;
    ext.midiRootKey = info->midiRootKey;
    ext.miscParameter1 = info->miscParameter1;
    ext.miscParameter2 = info->miscParameter2;
    ext.hasDefaultMod = info->hasDefaultMod;
    ext.LPF_frequency = info->LPF_frequency;
    ext.LPF_resonance = info->LPF_resonance;
    ext.LPF_lowpassAmount = info->LPF_lowpassAmount;
    PV_CopyInfoToEditorADSR(&info->volumeADSR, &ext.volumeADSR);
    ext.lfoCount = info->lfoCount;
    if (ext.lfoCount > EDITOR_MAX_LFOS)
    {
        ext.lfoCount = EDITOR_MAX_LFOS;
    }
    for (i = 0; i < ext.lfoCount; ++i)
    {
        ext.lfos[i].destination = info->lfos[i].destination;
        ext.lfos[i].period = info->lfos[i].period;
        if (ext.lfos[i].period != 0 && ext.lfos[i].period <= 512)
        {
            ext.lfos[i].period = 513;
        }
        ext.lfos[i].waveShape = info->lfos[i].waveShape;
        ext.lfos[i].DC_feed = info->lfos[i].DC_feed;
        ext.lfos[i].level = info->lfos[i].level;
        PV_CopyInfoToEditorADSR(&info->lfos[i].adsr, &ext.lfos[i].adsr);
    }
    ext.curveCount = info->curveCount;
    if (ext.curveCount > EDITOR_MAX_CURVES)
    {
        ext.curveCount = EDITOR_MAX_CURVES;
    }
    for (i = 0; i < ext.curveCount; ++i)
    {
        uint32_t j;
        ext.curves[i].tieFrom = info->curves[i].tieFrom;
        ext.curves[i].tieTo = info->curves[i].tieTo;
        ext.curves[i].curveCount = info->curves[i].curveCount;
        if (ext.curves[i].curveCount > EDITOR_MAX_ADSR_STAGES)
        {
            ext.curves[i].curveCount = EDITOR_MAX_ADSR_STAGES;
        }
        for (j = 0; j < ext.curves[i].curveCount; ++j)
        {
            ext.curves[i].from_Value[j] = info->curves[i].from_Value[j];
            ext.curves[i].to_Scalar[j] = info->curves[i].to_Scalar[j];
        }
    }

    extTail = PV_SerializeExtendedInstTail(&ext, &extTailSize);

    instBytes = (unsigned char *)XNewPtr(baseSize + extTailSize);
    if (!instBytes)
    {
        if (extTail)
        {
            XDisposePtr(extTail);
        }
        XDisposePtr(instData);
        return BAE_MEMORY_ERR;
    }
    XSetMemory(instBytes, baseSize + extTailSize, 0);
    XBlockMove(instData, instBytes, baseSize);

    XPutShort(instBytes + 2, (uint16_t)info->midiRootKey);
    instBytes[4] = (unsigned char)info->panPlacement;
    instBytes[5] = ext.flags1;
    instBytes[6] = info->flags2;
    XPutShort(instBytes + 8, (uint16_t)info->miscParameter1);
    XPutShort(instBytes + 10, (uint16_t)info->miscParameter2);
    if (extTail && extTailSize > 0)
    {
        instBytes[5] |= ZBF_extendedFormat;
    }
    else
    {
        instBytes[5] &= (unsigned char)(~ZBF_extendedFormat);
    }

    if (extTail && extTailSize > 0)
    {
        XBlockMove(extTail, instBytes + baseSize, extTailSize);
    }

    {
        char pascalName[256];
        char const *nameForReplace = rawName;
        if (info->displayName && info->displayName[0] &&
            PV_CreatePascalName(info->displayName, pascalName) == BAE_NO_ERROR)
        {
            nameForReplace = pascalName;
        }
        replaceResult = PV_BankReplaceResource(bankFile,
                                               ID_INST,
                                               instID,
                                               nameForReplace,
                                               instBytes,
                                               baseSize + extTailSize);
    }

    if (extTail)
    {
        XDisposePtr(extTail);
    }
    XDisposePtr(instBytes);
    XDisposePtr(instData);
    return replaceResult;
}

BAEResult BAERmfEditorBank_RenameSampleResource(BAEBankToken bankToken,
                                                XShortResourceID sndID,
                                                char const *displayName)
{
    XFILE bankFile;
    XResourceType sndType = 0;
    XPTR sndData = NULL;
    int32_t sndSize = 0;
    char pascalName[256];
    BAEResult r;

    /* sndID 0 is a valid resource id on some legacy banks — do not reject it. */
    if (!bankToken || !displayName || !displayName[0])
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;
    r = PV_BankFindSndResource(bankFile, sndID, &sndType, &sndData, &sndSize, NULL);
    if (r != BAE_NO_ERROR || !sndData)
    {
        return (r != BAE_NO_ERROR) ? r : BAE_BAD_FILE;
    }
    if (PV_CreatePascalName(displayName, pascalName) != BAE_NO_ERROR)
    {
        XDisposePtr(sndData);
        return BAE_PARAM_ERR;
    }
    r = PV_BankReplaceResource(bankFile, sndType, (XLongResourceID)sndID, pascalName, sndData, sndSize);
    XDisposePtr(sndData);
    return r;
}

BAEResult BAERmfEditorBank_BeginBatchSnd(BAEBankToken bankToken)
{
    XFILE bankFile;

    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;
    if (bankFile->pendingSndBatch != NULL)
    {
        return BAE_PARAM_ERR; /* already in batch mode */
    }

    /* Allocate initial capacity - will grow as needed in PV_BankReplaceSndResourceInPlace */
    bankFile->pendingSndBatch = (struct XFilePendingSnd *)XNewPtr(
        64 * (int32_t)sizeof(struct XFilePendingSnd));
    if (!bankFile->pendingSndBatch)
    {
        return BAE_MEMORY_ERR;
    }
    bankFile->pendingSndCount = 0;
    bankFile->pendingSndCapacity = 64;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorBank_CommitBatchSnd(BAEBankToken bankToken)
{
    XFILE bankFile;
    PV_SndReplacement *replacements;
    int32_t i;
    BAEResult result;

    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;
    if (!bankFile->pendingSndBatch || bankFile->pendingSndCount == 0)
    {
        /* Nothing to flush; just clear batch mode */
        if (bankFile->pendingSndBatch)
        {
            XDisposePtr(bankFile->pendingSndBatch);
            bankFile->pendingSndBatch = NULL;
        }
        bankFile->pendingSndCount = 0;
        bankFile->pendingSndCapacity = 0;
        return BAE_NO_ERROR;
    }

    /* Build PV_SndReplacement array from pending list */
    replacements = (PV_SndReplacement *)XNewPtr(
        bankFile->pendingSndCount * (int32_t)sizeof(PV_SndReplacement));
    if (!replacements)
    {
        return BAE_MEMORY_ERR;
    }
    for (i = 0; i < bankFile->pendingSndCount; ++i)
    {
        struct XFilePendingSnd *p = &bankFile->pendingSndBatch[i];
        replacements[i].oldType = (XResourceType)p->oldType;
        replacements[i].newType = (XResourceType)p->newType;
        replacements[i].sndID   = (XShortResourceID)p->sndID;
        replacements[i].data    = p->data;
        replacements[i].size    = p->size;
        XBlockMove(p->name, replacements[i].name, 256);
    }

    result = PV_BankReplaceMultipleSndResources(bankFile, replacements,
                                                bankFile->pendingSndCount);

    /* Free pending data */
    for (i = 0; i < bankFile->pendingSndCount; ++i)
    {
        XDisposePtr(bankFile->pendingSndBatch[i].data);
    }
    XDisposePtr(bankFile->pendingSndBatch);
    bankFile->pendingSndBatch = NULL;
    bankFile->pendingSndCount = 0;
    bankFile->pendingSndCapacity = 0;
    XDisposePtr(replacements);
    return result;
}

void BAERmfEditorBank_AbortBatchSnd(BAEBankToken bankToken)
{
    XFILE bankFile;
    int32_t i;

    if (!bankToken)
    {
        return;
    }
    bankFile = (XFILE)bankToken;
    if (!bankFile->pendingSndBatch)
    {
        return;
    }
    for (i = 0; i < bankFile->pendingSndCount; ++i)
    {
        XDisposePtr(bankFile->pendingSndBatch[i].data);
    }
    XDisposePtr(bankFile->pendingSndBatch);
    bankFile->pendingSndBatch = NULL;
    bankFile->pendingSndCount = 0;
    bankFile->pendingSndCapacity = 0;
}

BAEResult BAERmfEditorBank_ScaleAllSplitVolumes(BAEBankToken bankToken,
                                                uint32_t instrumentIndex,
                                                double scalar)
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
    char instName[256];
    int16_t splitCount;
    int32_t s;

    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

    instName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, instName, &instSize);
    if (!instData)
    {
        return BAE_BAD_FILE;
    }
    if (instSize < kInstHeaderMinSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
    }

    splitCount = (int16_t)XGetShort((unsigned char *)instData + 12);
    if (splitCount < 0)
    {
        splitCount = 0;
    }

    if (splitCount > 0)
    {
        /* Scale every split's miscParameter2 (volume) by scalar */
        for (s = 0; s < splitCount; ++s)
        {
            unsigned char *splitPtr = (unsigned char *)instData + 14 + (s * kInstKeySplitSize);
            int16_t curVol = (int16_t)XGetShort(splitPtr + 6);
            /* 0 means default (100) */
            if (curVol == 0) curVol = 100;
            {
                int32_t scaled = (int32_t)(curVol * scalar + 0.5);
                if (scaled < 0) scaled = 0;
                if (scaled > 800) scaled = 800;
                /* Store 0 to mean default only when exactly 100 */
                XPutShort(splitPtr + 6, (uint16_t)(scaled == 100 ? 0 : scaled));
            }
        }
    }
    else
    {
        /* Non-split instrument: scale header miscParameter2 */
        int16_t curVol = (int16_t)XGetShort((unsigned char *)instData + 10);
        if (curVol == 0) curVol = 100;
        {
            int32_t scaled = (int32_t)(curVol * scalar + 0.5);
            if (scaled < 0) scaled = 0;
            if (scaled > 800) scaled = 800;
            XPutShort((unsigned char *)instData + 10, (uint16_t)(scaled == 100 ? 0 : scaled));
        }
    }

    /* One rebuild for the whole instrument */
    {
        BAEResult r = PV_BankReplaceInstResourceInPlace(bankFile, instID, instName,
                                                        instData, instSize);
        XDisposePtr(instData);
        return r;
    }
}

BAEResult BAERmfEditorBank_SetInstrumentSampleInfo(BAEBankToken bankToken,
                                                    uint32_t instrumentIndex,
                                                    uint32_t sampleIndex,
                                                    BAERmfEditorBankSampleInfo const *info)
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
    char instName[256];
    int16_t splitCount;
    XShortResourceID sndID;
    XResourceType sndType;
    XPTR sndRawData;
    int32_t sndRawSize;
    char sndName[256];
    XPTR sndPlain;
    int32_t sndPlainSize;
    XPTR sndWrapped;
    int32_t sndWrappedSize;
    BAEResult result;
    BAEResult replaceResult;
    BAERmfEditorBankSampleInfo currentInfo;
    int needsSndHeaderUpdate;

    if (!bankToken || !info)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

    instName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, instName, &instSize);
    if (!instData)
    {
        return BAE_BAD_FILE;
    }
    if (instSize < kInstHeaderMinSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
    }

    splitCount = (int16_t)XGetShort((unsigned char *)instData + 12);
    if (splitCount < 0)
    {
        splitCount = 0;
    }

    if (splitCount > 0)
    {
        unsigned char *splitPtr;
        unsigned char flags2;

        if (sampleIndex >= (uint32_t)splitCount)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }

        splitPtr = (unsigned char *)instData + 14 + (sampleIndex * kInstKeySplitSize);
        splitPtr[0] = info->lowKey;
        splitPtr[1] = info->highKey;
        XPutShort(splitPtr + 6, (uint16_t)info->splitVolume);
        /* sndResourceID 0 is a valid SND id — always write the caller's value. */
        XPutShort(splitPtr + 2, (uint16_t)info->sndResourceID);
        sndID = info->sndResourceID;

        /* Root key storage depends on ZBF_useSoundModifierAsRootKey.
         * When set, each split stores its own root in miscParameter1.
         * When clear, all splits share the instrument-level midiRootKey. */
        flags2 = ((unsigned char *)instData)[6];
        if (TEST_FLAG_VALUE(flags2, ZBF_useSoundModifierAsRootKey))
        {
            XPutShort(splitPtr + 4, (uint16_t)info->rootKey);
        }
        else
        {
            /* Write to the shared midiRootKey - this affects all splits,
             * which is correct because the format has no per-split root
             * key in this mode. */
            XPutShort((unsigned char *)instData + 2, (uint16_t)info->rootKey);
        }
    }
    else
    {
        unsigned char flags2;

        if (sampleIndex != 0)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }
        XPutShort((unsigned char *)instData + 2, (uint16_t)info->rootKey);
        XPutShort((unsigned char *)instData + 10, (uint16_t)info->splitVolume);
        flags2 = ((unsigned char *)instData)[6];
        if (TEST_FLAG_VALUE(flags2, ZBF_useSoundModifierAsRootKey))
        {
            XPutShort((unsigned char *)instData + 8, (uint16_t)info->rootKey);
        }
        /* sndResourceID 0 is a valid SND id — always write the caller's value. */
        XPutShort((unsigned char *)instData + 0, (uint16_t)info->sndResourceID);
        sndID = info->sndResourceID;
    }

    /* Check early if this is a metadata-only edit (splitVolume/rootKey only, no SND changes) */
    needsSndHeaderUpdate = 1;
    XSetMemory(&currentInfo, (int32_t)sizeof(currentInfo), 0);
    if (BAERmfEditorBank_GetInstrumentSampleInfo(bankToken,
                                                 instrumentIndex,
                                                 sampleIndex,
                                                 &currentInfo) == BAE_NO_ERROR)
    {
        if (currentInfo.sampleRate == info->sampleRate &&
            currentInfo.loopStart == info->loopStart &&
            currentInfo.loopEnd == info->loopEnd)
        {
            needsSndHeaderUpdate = 0;
        }
    }

    /* Use fast in-place INST-only update when no SND changes needed.
       This is ~100x faster than the full resource replacement. */
    if (!needsSndHeaderUpdate)
    {
        result = PV_BankReplaceInstResourceInPlace(bankFile,
                                                   instID,
                                                   instName,
                                                   instData,
                                                   instSize);
        XDisposePtr(instData);
        return result;
    }

    /* Full resource replacement when SND data might need updating */
    result = PV_BankReplaceResource(bankFile,
                                    ID_INST,
                                    instID,
                                    instName,
                                    instData,
                                    instSize);
    if (result != BAE_NO_ERROR)
    {
        XDisposePtr(instData);
        return result;
    }
    XDisposePtr(instData);

    /* Process SND header updates if needed */
    if (!needsSndHeaderUpdate)
    {
        return BAE_NO_ERROR;
    }

    sndName[0] = 0;
    result = PV_BankFindSndResource(bankFile,
                                    sndID,
                                    &sndType,
                                    &sndRawData,
                                    &sndRawSize,
                                    sndName);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    sndPlain = sndRawData;
    sndPlainSize = sndRawSize;
    if (sndType == ID_CSND)
    {
        sndPlain = XDecompressPtr(sndRawData, (uint32_t)sndRawSize, FALSE);
        XDisposePtr(sndRawData);
        if (!sndPlain)
        {
            return BAE_BAD_FILE;
        }
        sndPlainSize = XGetPtrSize(sndPlain);
    }
    else if (sndType == ID_ESND)
    {
        XDecryptData(sndPlain, (uint32_t)sndPlainSize);
    }

    {
        uint32_t hz;
        BAE_UNSIGNED_FIXED sampleRate;

        hz = info->sampleRate;
        sampleRate = (BAE_UNSIGNED_FIXED)(hz << 16);
        /* Do NOT overwrite the SND's baseFrequency/baseMidiPitch here.
         * That field represents the sample's recorded pitch (used by the
         * engine's baseMidiPitch calculation) and is a property of the
         * audio data itself.  The instrument root key is stored in the
         * INST resource (midiRootKey / split miscParameter1) above. */
        XSetSoundSampleRate(sndPlain, sampleRate);
        XSetSoundLoopPoints(sndPlain, (int32_t)info->loopStart, (int32_t)info->loopEnd);
    }

    sndWrapped = NULL;
    sndWrappedSize = 0;
    result = PV_BankRewrapSndForType(bankFile,
                                     sndType,
                                     sndPlain,
                                     sndPlainSize,
                                     &sndWrapped,
                                     &sndWrappedSize);
    XDisposePtr(sndPlain);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    replaceResult = PV_BankReplaceResource(bankFile,
                                           sndType,
                                           (XLongResourceID)sndID,
                                           sndName,
                                           sndWrapped,
                                           sndWrappedSize);
    XDisposePtr(sndWrapped);
    return replaceResult;
}

BAEResult BAERmfEditorBank_SetInstrumentSampleSndID(BAEBankToken bankToken,
                                                     uint32_t instrumentIndex,
                                                     uint32_t sampleIndex,
                                                     XShortResourceID sndResourceID)
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
    char instName[256];
    int16_t splitCount;

    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

    instName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, instName, &instSize);
    if (!instData)
    {
        return BAE_BAD_FILE;
    }
    if (instSize < kInstHeaderMinSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
    }

    splitCount = (int16_t)XGetShort((unsigned char *)instData + 12);
    if (splitCount < 0)
    {
        splitCount = 0;
    }

    if (splitCount > 0)
    {
        unsigned char *splitPtr;

        if (sampleIndex >= (uint32_t)splitCount)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }

        splitPtr = (unsigned char *)instData + 14 + (sampleIndex * kInstKeySplitSize);
        XPutShort(splitPtr + 2, (uint16_t)sndResourceID);

        /* Keep header sndResourceID synchronized with split 0 for compatibility. */
        XPutShort((unsigned char *)instData + 0,
                  (uint16_t)XGetShort((unsigned char *)instData + 14 + 2));
    }
    else
    {
        if (sampleIndex != 0)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }
        XPutShort((unsigned char *)instData + 0, (uint16_t)sndResourceID);
    }

    {
        BAEResult result = PV_BankReplaceResource(bankFile,
                                                  ID_INST,
                                                  instID,
                                                  instName,
                                                  instData,
                                                  instSize);
        XDisposePtr(instData);
        return result;
    }
}

BAEResult BAERmfEditorBank_SetSampleSndStorageType(BAEBankToken bankToken,
                                                    uint32_t instrumentIndex,
                                                    uint32_t sampleIndex,
                                                    BAERmfEditorSndStorageType sndStorageType)
{
    XFILE bankFile;
    BAERmfEditorBankSampleInfo sampleInfo;
    XResourceType oldType;
    XResourceType newType;
    XPTR sndRawData;
    int32_t sndRawSize;
    XPTR sndPlain;
    int32_t sndPlainSize;
    XPTR sndWrapped;
    int32_t sndWrappedSize;
    char resName[256];
    BAEResult result;

    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }

    bankFile = (XFILE)bankToken;

    /* Resolve current sample metadata to locate the backing SND resource. */
    if (BAERmfEditorBank_GetInstrumentSampleInfo(bankToken,
                                                 instrumentIndex,
                                                 sampleIndex,
                                                 &sampleInfo) != BAE_NO_ERROR)
    {
        return BAE_BAD_FILE;
    }

    switch (sampleInfo.sndStorageType)
    {
        case BAE_EDITOR_SND_STORAGE_CSND:
            oldType = ID_CSND;
            break;
        case BAE_EDITOR_SND_STORAGE_SND:
            oldType = ID_SND;
            break;
        case BAE_EDITOR_SND_STORAGE_ESND:
        default:
            oldType = ID_ESND;
            break;
    }

    switch (sndStorageType)
    {
        case BAE_EDITOR_SND_STORAGE_CSND:
            newType = ID_CSND;
            break;
        case BAE_EDITOR_SND_STORAGE_SND:
            newType = ID_SND;
            break;
        case BAE_EDITOR_SND_STORAGE_ESND:
        default:
            newType = ID_ESND;
            break;
    }

    if (oldType == newType)
    {
        return BAE_NO_ERROR;
    }

    /* Load existing payload, decode container wrapper, then rewrap for target type.
       This avoids PCM decode/re-encode while still honoring CSND compression/encryption. */
    resName[0] = 0;
    result = PV_BankFindSndResource(bankFile,
                                    (XShortResourceID)sampleInfo.sndResourceID,
                                    &oldType,
                                    &sndRawData,
                                    &sndRawSize,
                                    resName);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    sndPlain = sndRawData;
    sndPlainSize = sndRawSize;
    {
        int16_t soundFormat = 0;
        if (sndPlain && sndPlainSize >= 2)
        {
            soundFormat = (int16_t)XGetShort(sndPlain);
        }

        /* Some loaders return already-rebuilt plain SND data even for CSND/ESND
           entries. Only unwrap when data does not already look like SND. */
        if (!(soundFormat == 1 || soundFormat == 2 || soundFormat == 3))
        {
            if (oldType == ID_CSND)
            {
                XPTR decompressed = XDecompressPtr(sndRawData, (uint32_t)sndRawSize, FALSE);
                XDisposePtr(sndRawData);
                sndPlain = decompressed;
                if (!sndPlain)
                {
                    return BAE_BAD_FILE;
                }
                sndPlainSize = XGetPtrSize(sndPlain);
            }
            else if (oldType == ID_ESND)
            {
                XDecryptData(sndPlain, (uint32_t)sndPlainSize);
            }
        }
    }

    sndWrapped = NULL;
    sndWrappedSize = 0;
    result = PV_BankRewrapSndForType(bankFile,
                                     newType,
                                     sndPlain,
                                     sndPlainSize,
                                     &sndWrapped,
                                     &sndWrappedSize);
    XDisposePtr(sndPlain);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    result = PV_BankReplaceSndResourceInPlace(bankFile,
                                              oldType,
                                              newType,
                                              (XShortResourceID)sampleInfo.sndResourceID,
                                              resName,
                                              sndWrapped,
                                              sndWrappedSize);
    XDisposePtr(sndWrapped);
    return result;
}

static BAEResult PV_BankDeleteResource(XFILE bankFile,
                                       XResourceType type,
                                       XLongResourceID id)
{
    static const XResourceType bankResourceTypes[] = {
        ID_INST,
        ID_SND,
        ID_CSND,
        ID_ESND,
        ID_ALIAS,
        ID_BANK,
        ID_SONG,
        ID_MIDI,
        ID_MIDI_OLD,
        ID_CMID,
        ID_EMID,
        ID_ECMI,
        ID_RMF,
        ID_TEXT,
        ID_VERS,
        0
    };
    XFILERESOURCEMAP map;
    int32_t resourceID;
    XFILE outFile;
    int32_t typeIdx;
    XPTR packedData;
    int32_t packedSize;

    if (!bankFile)
    {
        return BAE_PARAM_ERR;
    }
    if (XFileSetPosition(bankFile, 0L) != 0 ||
        XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_BAD_FILE;
    }
    resourceID = (int32_t)XGetLong(&map.mapID);
    if (!XFILERESOURCE_ID_IS_VALID(resourceID))
    {
        return BAE_BAD_FILE;
    }

    outFile = XFileOpenVirtualResource(resourceID);
    if (!outFile)
    {
        return BAE_MEMORY_ERR;
    }

    for (typeIdx = 0; bankResourceTypes[typeIdx] != 0; ++typeIdx)
    {
        XResourceType resType;
        int32_t resCount;
        int32_t resIndex;

        resType = bankResourceTypes[typeIdx];
        resCount = XCountFileResourcesOfType(bankFile, resType);
        for (resIndex = 0; resIndex < resCount; ++resIndex)
        {
            XLongResourceID resID;
            int32_t resSize;
            XPTR resData;
            char resName[256];

            resName[0] = 0;
            resData = XGetIndexedFileResource(bankFile,
                                              resType,
                                              &resID,
                                              resIndex,
                                              resName,
                                              &resSize);
            if (!resData)
            {
                continue;
            }

            if (resType == type && resID == id)
            {
                XDisposePtr(resData);
                continue;
            }

            if (XAddFileResource(outFile, resType, resID, resName, resData, resSize) != 0)
            {
                XDisposePtr(resData);
                XFileClose(outFile);
                return BAE_FILE_IO_ERROR;
            }
            XDisposePtr(resData);
        }
    }

    if (XCleanResourceFile(outFile) == FALSE)
    {
        XFileClose(outFile);
        return BAE_FILE_IO_ERROR;
    }

    packedData = NULL;
    packedSize = 0;
    if (XFileGetMemoryFileAsData(outFile, &packedData, &packedSize) != 0 ||
        !packedData || packedSize <= 0)
    {
        XFileClose(outFile);
        if (packedData)
        {
            XDisposePtr(packedData);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(outFile);

    if (bankFile->pCache)
    {
        XDisposePtr(bankFile->pCache);
        bankFile->pCache = NULL;
    }
    if (bankFile->pResourceData && bankFile->ownsResourceData)
    {
        XDisposePtr(bankFile->pResourceData);
    }

    bankFile->pResourceData = packedData;
    bankFile->resMemLength = packedSize;
    bankFile->resMemOffset = 0;
    bankFile->ownsResourceData = TRUE;
    bankFile->resizeResourceData = TRUE;
    bankFile->readOnly = FALSE;
    bankFile->allowMemCopy = TRUE;
    return BAE_NO_ERROR;
}

// Public API to delete an instrument from a bank
BAEResult BAERmfEditorBank_DeleteInstrument(BAEBankToken bankToken,
                                            uint32_t instrumentIndex)
{
    XFILE bankFile;
    XLongResourceID instID;
    XPTR instData;
    int32_t instSize;

    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }

    bankFile = (XFILE)bankToken;

    // Get the instrument ID at this index
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, NULL, &instSize);
    if (!instData)
    {
        return BAE_BAD_FILE;
    }

    XDisposePtr(instData);

    // Use internal function to properly delete the resource from memory-based bank
    {
        BAEResult r = PV_BankDeleteResource(bankFile, ID_INST, instID);
        if (r != BAE_NO_ERROR)
            return r;
    }

    // Also remove any alias entries that pointed at this instID
    {
        XAliasLinkResource *oldAlias = XGetAliasLinkFromFile(bankFile);
        if (oldAlias)
        {
            uint32_t oldCount = (uint32_t)XGetLong(&oldAlias->numberOfAliases);
            uint32_t newCount = 0;
            int32_t newSize;
            XAliasLinkResource *newAlias;

            for (uint32_t i = 0; i < oldCount; ++i)
                if ((uint32_t)XGetLong(&oldAlias->list[i].aliasTo) != (uint32_t)instID)
                    ++newCount;

            if (newCount < oldCount)
            {
                if (newCount == 0)
                {
                    // No aliases left - remove the ID_ALIAS resource entirely
                    PV_BankDeleteResource(bankFile, ID_ALIAS, DEFAULT_RESOURCE_ALIAS_ID);
                }
                else
                {
                    newSize  = (int32_t)(sizeof(uint32_t) * 2 +
                                         newCount * sizeof(XAliasLink));
                    newAlias = (XAliasLinkResource *)XNewPtr(newSize);
                    if (newAlias)
                    {
                        uint32_t dst = 0;
                        XPutLong(&newAlias->version, ALIAS_ID_RESOURCE_VERSION);
                        XPutLong(&newAlias->numberOfAliases, newCount);
                        for (uint32_t i = 0; i < oldCount; ++i)
                        {
                            if ((uint32_t)XGetLong(&oldAlias->list[i].aliasTo) != (uint32_t)instID)
                            {
                                XPutLong(&newAlias->list[dst].aliasFrom,
                                         XGetLong(&oldAlias->list[i].aliasFrom));
                                XPutLong(&newAlias->list[dst].aliasTo,
                                         XGetLong(&oldAlias->list[i].aliasTo));
                                ++dst;
                            }
                        }
                        char emptyName[1] = {0};
                        PV_BankReplaceResource(bankFile, ID_ALIAS,
                                               DEFAULT_RESOURCE_ALIAS_ID,
                                               emptyName, newAlias, newSize);
                        XDisposePtr((XPTR)newAlias);
                    }
                }
            }
            XDisposePtr((XPTR)oldAlias);
        }
    }
    return BAE_NO_ERROR;
}

// Remove one alias entry (by aliasFrom ID) from the bank's ID_ALIAS resource.
// Does NOT delete the underlying INST resource.
BAEResult BAERmfEditorBank_DeleteAlias(BAEBankToken bankToken,
                                       uint32_t aliasFromInstID)
{
    XFILE bankFile;
    XAliasLinkResource *oldAlias;
    uint32_t oldCount;
    uint32_t newCount;
    int32_t newSize;
    XAliasLinkResource *newAlias;
    BAEResult result;

    if (!bankToken)
        return BAE_PARAM_ERR;

    bankFile = (XFILE)bankToken;
    oldAlias = XGetAliasLinkFromFile(bankFile);
    oldCount = oldAlias ? (uint32_t)XGetLong(&oldAlias->numberOfAliases) : 0u;

    if (oldCount == 0)
    {
        XDisposePtr((XPTR)oldAlias);
        return BAE_PARAM_ERR;  // nothing to remove
    }

    // Count entries that will survive
    newCount = 0;
    for (uint32_t i = 0; i < oldCount; ++i)
        if ((uint32_t)XGetLong(&oldAlias->list[i].aliasFrom) != aliasFromInstID)
            ++newCount;

    if (newCount == oldCount)
    {
        // Entry not found
        XDisposePtr((XPTR)oldAlias);
        return BAE_PARAM_ERR;
    }

    if (newCount == 0)
    {
        XDisposePtr((XPTR)oldAlias);
        return PV_BankDeleteResource(bankFile, ID_ALIAS, DEFAULT_RESOURCE_ALIAS_ID);
    }

    newSize  = (int32_t)(sizeof(uint32_t) * 2 + newCount * sizeof(XAliasLink));
    newAlias = (XAliasLinkResource *)XNewPtr(newSize);
    if (!newAlias)
    {
        XDisposePtr((XPTR)oldAlias);
        return BAE_MEMORY_ERR;
    }

    XPutLong(&newAlias->version, ALIAS_ID_RESOURCE_VERSION);
    XPutLong(&newAlias->numberOfAliases, newCount);
    {
        uint32_t dst = 0;
        for (uint32_t i = 0; i < oldCount; ++i)
        {
            if ((uint32_t)XGetLong(&oldAlias->list[i].aliasFrom) != aliasFromInstID)
            {
                XPutLong(&newAlias->list[dst].aliasFrom,
                         XGetLong(&oldAlias->list[i].aliasFrom));
                XPutLong(&newAlias->list[dst].aliasTo,
                         XGetLong(&oldAlias->list[i].aliasTo));
                ++dst;
            }
        }
    }
    XDisposePtr((XPTR)oldAlias);

    {
        char emptyName[1] = {0};
        result = PV_BankReplaceResource(bankFile, ID_ALIAS,
                                        DEFAULT_RESOURCE_ALIAS_ID,
                                        emptyName, newAlias, newSize);
    }
    XDisposePtr((XPTR)newAlias);
    return result;
}

static bool PV_BankIdInList(XShortResourceID const *ids, uint32_t count, XLongResourceID id)
{
    uint32_t i;

    if (!ids || count == 0 || id > 32767)
    {
        return FALSE;
    }
    for (i = 0; i < count; ++i)
    {
        if ((XLongResourceID)ids[i] == id)
        {
            return TRUE;
        }
    }
    return FALSE;
}

/* True if file has a decodable SND/CSND/ESND for sid (handles encrypted ESND).
 * ID 0 is a valid resource id; only 0xFFFF (-1) means "no sample". */
static bool PV_BankFileHasValidSndResource(XFILE file, XShortResourceID sid)
{
    static const XResourceType sndTypes[] = {ID_ESND, ID_CSND, ID_SND, 0};
    int t;

    if (!file || sid == (XShortResourceID)-1)
    {
        return FALSE;
    }
    for (t = 0; sndTypes[t] != 0; ++t)
    {
        int32_t size = 0;
        /* Zero-extend: sign-extending negative shorts breaks ID lookups. */
        XPTR data = XGetFileResource(file,
                                     sndTypes[t],
                                     (XLongResourceID)(uint16_t)sid,
                                     NULL,
                                     &size);
        if (!data || size < 4)
        {
            if (data)
            {
                XDisposePtr(data);
            }
            continue;
        }
        if (sndTypes[t] == ID_ESND)
        {
            XPTR copy = XNewPtr(size);
            int16_t fmt;
            bool ok;

            if (!copy)
            {
                XDisposePtr(data);
                return FALSE;
            }
            XBlockMove(data, copy, size);
            XDisposePtr(data);
            XDecryptData(copy, (uint32_t)size);
            fmt = (int16_t)XGetShort(copy);
            ok = (fmt == 1 || fmt == 2 || fmt == 3);
            XDisposePtr(copy);
            if (ok)
            {
                return TRUE;
            }
        }
        else if (sndTypes[t] == ID_CSND)
        {
            XPTR decoded = XDecompressPtr(data, (uint32_t)size, FALSE);
            int16_t fmt;
            bool ok;

            XDisposePtr(data);
            if (!decoded)
            {
                continue;
            }
            fmt = (int16_t)XGetShort(decoded);
            ok = (fmt == 1 || fmt == 2 || fmt == 3);
            XDisposePtr(decoded);
            if (ok)
            {
                return TRUE;
            }
        }
        else
        {
            int16_t fmt = (int16_t)XGetShort(data);
            XDisposePtr(data);
            if (fmt == 1 || fmt == 2 || fmt == 3)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

BAEResult BAERmfEditorBank_ImportInstAndSndFromFile(BAEBankToken bankToken,
                                                    void *sourceFile,
                                                    XShortResourceID const *instIds,
                                                    uint32_t instCount,
                                                    XShortResourceID const *sndIds,
                                                    uint32_t sndCount)
{
    static const XResourceType bankResourceTypes[] = {
        ID_INST,
        ID_SND,
        ID_CSND,
        ID_ESND,
        ID_ALIAS,
        ID_BANK,
        ID_SONG,
        ID_MIDI,
        ID_MIDI_OLD,
        ID_CMID,
        ID_EMID,
        ID_ECMI,
        ID_RMF,
        ID_TEXT,
        ID_VERS,
        0
    };
    static const XResourceType sndTypes[] = {ID_ESND, ID_CSND, ID_SND, 0};
    XFILE bankFile;
    XFILE donor;
    XFILE flatDonor;
    XFILERESOURCEMAP map;
    int32_t resourceID;
    XFILE outFile;
    int32_t typeIdx;
    XPTR packedData;
    int32_t packedSize;
    uint32_t i;
    XShortResourceID *sndCopy = NULL;
    XShortResourceID *instCopy = NULL;

    if (!bankToken || !sourceFile)
    {
        return BAE_PARAM_ERR;
    }
    if ((instCount > 0 && !instIds) || (sndCount > 0 && !sndIds))
    {
        return BAE_PARAM_ERR;
    }
    if (instCount == 0 && sndCount == 0)
    {
        return BAE_NO_ERROR;
    }

    bankFile = (XFILE)bankToken;
    donor = (XFILE)sourceFile;

    /* Expand ZREZ donor packages (ZSHD payload-refs / ZINS) into a flat IREZ
     * staging file before merge. XGetFileResource reassembles refs; writing
     * into IREZ + Clean does not re-pack, so overlays are always full SND/INST. */
    flatDonor = XFileOpenVirtualResource(XFILERESOURCE_ID);
    if (!flatDonor)
    {
        return BAE_MEMORY_ERR;
    }
    if (sndCount > 0)
    {
        sndCopy = (XShortResourceID *)XNewPtr((int32_t)(sndCount * sizeof(XShortResourceID)));
        if (!sndCopy)
        {
            XFileClose(flatDonor);
            return BAE_MEMORY_ERR;
        }
        XBlockMove((void *)sndIds, sndCopy, (int32_t)(sndCount * sizeof(XShortResourceID)));
        (void)XCopySndResources(sndCopy,
                                (int16_t)sndCount,
                                donor,
                                flatDonor,
                                FALSE,
                                TRUE);
        for (i = 0; i < sndCount; ++i)
        {
            if (!PV_BankFileHasValidSndResource(flatDonor, sndCopy[i]))
            {
                XDisposePtr(sndCopy);
                XFileClose(flatDonor);
                return BAE_BAD_FILE;
            }
        }
    }
    if (instCount > 0)
    {
        instCopy = (XShortResourceID *)XNewPtr((int32_t)(instCount * sizeof(XShortResourceID)));
        if (!instCopy)
        {
            if (sndCopy)
            {
                XDisposePtr(sndCopy);
            }
            XFileClose(flatDonor);
            return BAE_MEMORY_ERR;
        }
        XBlockMove((void *)instIds, instCopy, (int32_t)(instCount * sizeof(XShortResourceID)));
        (void)XCopyInstrumentResources(instCopy,
                                       (int16_t)instCount,
                                       donor,
                                       flatDonor,
                                       TRUE);
        for (i = 0; i < instCount; ++i)
        {
            int32_t size = 0;
            XPTR data = XGetFileResource(flatDonor,
                                         ID_INST,
                                         (XLongResourceID)instCopy[i],
                                         NULL,
                                         &size);
            if (!data || size < 14)
            {
                if (data)
                {
                    XDisposePtr(data);
                }
                XDisposePtr(instCopy);
                if (sndCopy)
                {
                    XDisposePtr(sndCopy);
                }
                XFileClose(flatDonor);
                return BAE_BAD_FILE;
            }
            XDisposePtr(data);
        }
    }
    /* Keep SNDs flat in the staging image — ZSHD refs are a paste footgun. */
    if (XCleanResourceFileEx(flatDonor, FALSE) == FALSE)
    {
        if (instCopy)
        {
            XDisposePtr(instCopy);
        }
        if (sndCopy)
        {
            XDisposePtr(sndCopy);
        }
        XFileClose(flatDonor);
        return BAE_FILE_IO_ERROR;
    }
    donor = flatDonor;

    if (XFileSetPosition(bankFile, 0L) != 0 ||
        XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        if (instCopy)
        {
            XDisposePtr(instCopy);
        }
        if (sndCopy)
        {
            XDisposePtr(sndCopy);
        }
        XFileClose(flatDonor);
        return BAE_BAD_FILE;
    }
    resourceID = (int32_t)XGetLong(&map.mapID);
    if (!XFILERESOURCE_ID_IS_VALID(resourceID))
    {
        if (instCopy)
        {
            XDisposePtr(instCopy);
        }
        if (sndCopy)
        {
            XDisposePtr(sndCopy);
        }
        XFileClose(flatDonor);
        return BAE_BAD_FILE;
    }

    outFile = XFileOpenVirtualResource(resourceID);
    if (!outFile)
    {
        if (instCopy)
        {
            XDisposePtr(instCopy);
        }
        if (sndCopy)
        {
            XDisposePtr(sndCopy);
        }
        XFileClose(flatDonor);
        return BAE_MEMORY_ERR;
    }

    /* Copy dest resources, omitting ids that will be replaced. */
    for (typeIdx = 0; bankResourceTypes[typeIdx] != 0; ++typeIdx)
    {
        XResourceType resType = bankResourceTypes[typeIdx];
        int32_t resCount = XCountFileResourcesOfType(bankFile, resType);
        int32_t resIndex;

        for (resIndex = 0; resIndex < resCount; ++resIndex)
        {
            XLongResourceID resID = 0;
            int32_t resSize = 0;
            char resName[256];
            XPTR resData;

            resName[0] = 0;
            resData = XGetIndexedFileResource(bankFile,
                                              resType,
                                              &resID,
                                              resIndex,
                                              resName,
                                              &resSize);
            if (!resData)
            {
                continue;
            }

            if (resType == ID_INST && PV_BankIdInList(instIds, instCount, resID))
            {
                XDisposePtr(resData);
                continue;
            }
            if ((resType == ID_SND || resType == ID_CSND || resType == ID_ESND) &&
                PV_BankIdInList(sndIds, sndCount, resID))
            {
                XDisposePtr(resData);
                continue;
            }

            if (XAddFileResource(outFile, resType, resID, resName, resData, resSize) != 0)
            {
                XDisposePtr(resData);
                XFileClose(outFile);
                if (instCopy)
                {
                    XDisposePtr(instCopy);
                }
                if (sndCopy)
                {
                    XDisposePtr(sndCopy);
                }
                XFileClose(flatDonor);
                return BAE_FILE_IO_ERROR;
            }
            XDisposePtr(resData);
        }
    }

    /* Overlay SND containers from expanded donor (ESND/CSND/SND). */
    for (i = 0; i < sndCount; ++i)
    {
        XLongResourceID rid = (XLongResourceID)sndIds[i];
        int t;

        for (t = 0; sndTypes[t] != 0; ++t)
        {
            char name[256];
            int32_t size = 0;
            XPTR data;

            name[0] = 0;
            data = XGetFileResource(donor, sndTypes[t], rid, name, &size);
            if (!data || size <= 0)
            {
                if (data)
                {
                    XDisposePtr(data);
                }
                continue;
            }
            if (XAddFileResource(outFile, sndTypes[t], rid, name, data, size) != 0)
            {
                XDisposePtr(data);
                XFileClose(outFile);
                if (instCopy)
                {
                    XDisposePtr(instCopy);
                }
                if (sndCopy)
                {
                    XDisposePtr(sndCopy);
                }
                XFileClose(flatDonor);
                return BAE_FILE_IO_ERROR;
            }
            XDisposePtr(data);
        }
    }

    /* Overlay INST resources from expanded donor. */
    for (i = 0; i < instCount; ++i)
    {
        XLongResourceID rid = (XLongResourceID)instIds[i];
        char name[256];
        int32_t size = 0;
        XPTR data;

        name[0] = 0;
        data = XGetFileResource(donor, ID_INST, rid, name, &size);
        if (!data || size <= 0)
        {
            if (data)
            {
                XDisposePtr(data);
            }
            continue;
        }
        if (XAddFileResource(outFile, ID_INST, rid, name, data, size) != 0)
        {
            XDisposePtr(data);
            XFileClose(outFile);
            if (instCopy)
            {
                XDisposePtr(instCopy);
            }
            if (sndCopy)
            {
                XDisposePtr(sndCopy);
            }
            XFileClose(flatDonor);
            return BAE_FILE_IO_ERROR;
        }
        XDisposePtr(data);
    }

    if (instCopy)
    {
        XDisposePtr(instCopy);
        instCopy = NULL;
    }
    if (sndCopy)
    {
        XDisposePtr(sndCopy);
        sndCopy = NULL;
    }
    XFileClose(flatDonor);
    flatDonor = NULL;

    /* Do not ZSHD-pack during merge — keep complete ESND/SND payloads so the
     * editor can decode immediately after cross-process paste. */
    if (XCleanResourceFileEx(outFile, FALSE) == FALSE)
    {
        XFileClose(outFile);
        return BAE_FILE_IO_ERROR;
    }

    packedData = NULL;
    packedSize = 0;
    if (XFileGetMemoryFileAsData(outFile, &packedData, &packedSize) != 0 ||
        !packedData || packedSize <= 0)
    {
        XFileClose(outFile);
        if (packedData)
        {
            XDisposePtr(packedData);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(outFile);

    if (bankFile->pCache)
    {
        XDisposePtr(bankFile->pCache);
        bankFile->pCache = NULL;
    }
    if (bankFile->pResourceData && bankFile->ownsResourceData)
    {
        XDisposePtr(bankFile->pResourceData);
    }

    bankFile->pResourceData = packedData;
    bankFile->resMemLength = packedSize;
    bankFile->resMemOffset = 0;
    bankFile->ownsResourceData = TRUE;
    bankFile->resizeResourceData = TRUE;
    bankFile->readOnly = FALSE;
    bankFile->allowMemCopy = TRUE;
    /* Match EnsureWritable — Exists/name lookups need a live cache. */
    bankFile->pCache = XCreateAccessCache(bankFile);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorBank_EnsureWritable(BAEBankToken bankToken)
{
    static const XResourceType bankResourceTypes[] = {
        ID_INST,
        ID_SND,
        ID_CSND,
        ID_ESND,
        ID_ALIAS,
        ID_BANK,
        ID_SONG,
        ID_MIDI,
        ID_MIDI_OLD,
        ID_CMID,
        ID_EMID,
        ID_ECMI,
        ID_RMF,
        ID_TEXT,
        ID_VERS,
        0
    };
    XFILE bankFile;
    XFILERESOURCEMAP map;
    int32_t resourceID;
    XFILE outFile;
    int32_t typeIdx;
    XPTR packedData;
    int32_t packedSize;

    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;
    if (bankFile->pResourceData &&
        bankFile->readOnly == FALSE &&
        bankFile->resizeResourceData != FALSE)
    {
        return BAE_NO_ERROR;
    }

    if (XFileSetPosition(bankFile, 0L) != 0 ||
        XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_BAD_FILE;
    }
    resourceID = (int32_t)XGetLong(&map.mapID);
    if (!XFILERESOURCE_ID_IS_VALID(resourceID))
    {
        return BAE_BAD_FILE;
    }

    outFile = XFileOpenVirtualResource(resourceID);
    if (!outFile)
    {
        return BAE_MEMORY_ERR;
    }

    for (typeIdx = 0; bankResourceTypes[typeIdx] != 0; ++typeIdx)
    {
        XResourceType resType = bankResourceTypes[typeIdx];
        int32_t resCount = XCountFileResourcesOfType(bankFile, resType);
        int32_t resIndex;

        for (resIndex = 0; resIndex < resCount; ++resIndex)
        {
            XLongResourceID resID = 0;
            int32_t resSize = 0;
            char resName[256];
            XPTR resData;

            resName[0] = 0;
            resData = XGetIndexedFileResource(bankFile,
                                              resType,
                                              &resID,
                                              resIndex,
                                              resName,
                                              &resSize);
            if (!resData)
            {
                continue;
            }
            if (XAddFileResource(outFile, resType, resID, resName, resData, resSize) != 0)
            {
                XDisposePtr(resData);
                XFileClose(outFile);
                return BAE_FILE_IO_ERROR;
            }
            XDisposePtr(resData);
        }
    }

    if (XCleanResourceFile(outFile) == FALSE)
    {
        XFileClose(outFile);
        return BAE_FILE_IO_ERROR;
    }

    packedData = NULL;
    packedSize = 0;
    if (XFileGetMemoryFileAsData(outFile, &packedData, &packedSize) != 0 ||
        !packedData || packedSize <= 0)
    {
        XFileClose(outFile);
        if (packedData)
        {
            XDisposePtr(packedData);
        }
        return BAE_FILE_IO_ERROR;
    }
    XFileClose(outFile);

    if (bankFile->pCache)
    {
        XDisposePtr(bankFile->pCache);
        bankFile->pCache = NULL;
    }
    if (bankFile->pResourceData && bankFile->ownsResourceData)
    {
        XDisposePtr(bankFile->pResourceData);
    }

    bankFile->pResourceData = packedData;
    bankFile->resMemLength = packedSize;
    bankFile->resMemOffset = 0;
    bankFile->ownsResourceData = TRUE;
    bankFile->resizeResourceData = TRUE;
    bankFile->readOnly = FALSE;
    bankFile->allowMemCopy = TRUE;

    /* Rebuild access cache so Exists/name lookups work (uncached path has a
     * not-found bug that reports success for missing IDs). */
    bankFile->pCache = XCreateAccessCache(bankFile);
    return BAE_NO_ERROR;
}

// Clone an INST resource to a new instID.  If deepClone is TRUE, all SND/CSND/ESND
// resources referenced by the instrument's key-splits are also duplicated with new IDs.
BAEResult BAERmfEditorBank_CloneInstrument(BAEBankToken bankToken,
                                           uint32_t instrumentIndex,
                                           uint32_t destInstID,
                                           bool deepClone)
{
    enum { kInstHeaderMinSize = 14, kInstKeySplitSize = 8 };
    XFILE bankFile;
    XLongResourceID srcInstID;
    XPTR instData;
    int32_t instSize;
    char instName[256];
    int16_t splitCount;
    BAEResult result;
    uint32_t resolvedInstID;
    uint32_t resolvedIndex;

    if (!bankToken)
        return BAE_PARAM_ERR;

    bankFile = (XFILE)bankToken;

    /* Duplicate guard: if destInstID already resolves to an existing INST
       (directly or through ID_ALIAS), report it explicitly. */
    result = BAERmfEditorBank_ResolveInstID(bankToken,
                                            destInstID,
                                            &resolvedInstID,
                                            &resolvedIndex);
    if (result == BAE_NO_ERROR)
    {
        return BAE_ALREADY_EXISTS;
    }

    instName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &srcInstID,
                                       (int32_t)instrumentIndex, instName, &instSize);
    if (!instData || instSize < kInstHeaderMinSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
    }

    splitCount = (int16_t)XGetShort((unsigned char *)instData + 12);
    if (splitCount < 0) splitCount = 0;

    if (deepClone && splitCount > 0)
    {
        // For each split, copy the SND resource to a new ID and patch the clone
        unsigned char *cloneData = (unsigned char *)XNewPtr(instSize);
        if (!cloneData)
        {
            XDisposePtr(instData);
            return BAE_MEMORY_ERR;
        }
        XBlockMove(instData, cloneData, instSize);

        for (int s = 0; s < splitCount; ++s)
        {
            unsigned char *splitPtr = cloneData + 14 + s * kInstKeySplitSize;
            XShortResourceID oldSndID = (XShortResourceID)XGetShort(splitPtr + 2);
            if (oldSndID != 0)
            {
                // Find a free SND ID
                XShortResourceID newSndID = 0;
                {
                    int32_t sndCount = XCountFileResourcesOfType(bankFile, ID_SND);
                    // Collect used IDs across SND/CSND/ESND
                    bool usedIDs[65536];
                    XSetMemory(usedIDs, sizeof(usedIDs), 0);
                    static const XResourceType sndTypes[] = { ID_SND, ID_CSND, ID_ESND, 0 };
                    for (int t = 0; sndTypes[t] != 0; ++t)
                    {
                        int32_t cnt = XCountFileResourcesOfType(bankFile, sndTypes[t]);
                        for (int k = 0; k < cnt; ++k)
                        {
                            XLongResourceID rid;
                            XPTR tmp = XGetIndexedFileResource(bankFile, sndTypes[t], &rid, k, NULL, NULL);
                            if (tmp) { XDisposePtr(tmp); if (rid >= 1 && rid < 65536) usedIDs[rid] = TRUE; }
                        }
                    }
                    (void)sndCount;
                    for (int k = 1; k < 65536; ++k)
                        if (!usedIDs[k]) { newSndID = (XShortResourceID)k; break; }
                }
                if (newSndID == 0)
                {
                    XDisposePtr(cloneData);
                    XDisposePtr(instData);
                    return BAE_FILE_IO_ERROR;
                }

                // Try each SND type to find the source
                static const XResourceType sndTypes[] = { ID_SND, ID_CSND, ID_ESND, 0 };
                for (int t = 0; sndTypes[t] != 0; ++t)
                {
                    int32_t sndSize;
                    XPTR sndData = XGetFileResource(bankFile, sndTypes[t],
                                                    (XLongResourceID)oldSndID, NULL, &sndSize);
                    if (sndData)
                    {
                        // Add under new ID
                        char emptyName[1] = {0};
                        if (XAddFileResource(bankFile, sndTypes[t],
                                             (XLongResourceID)newSndID,
                                             emptyName, sndData, sndSize) != 0)
                        {
                            XDisposePtr(sndData);
                            XDisposePtr(cloneData);
                            XDisposePtr(instData);
                            return BAE_FILE_IO_ERROR;
                        }
                        XDisposePtr(sndData);
                        // Patch the split to point to the new SND ID
                        XPutShort(splitPtr + 2, (uint16_t)newSndID);
                        break;
                    }
                }
            }
        }
        // Keep header sndResourceID in sync with split 0
        XPutShort(cloneData, (uint16_t)XGetShort(cloneData + 14 + 2));

        result = PV_BankReplaceResource(bankFile, ID_INST,
                                        (XLongResourceID)destInstID,
                                        instName, cloneData, instSize);
        XDisposePtr(cloneData);
    }
    else
    {
        // Shallow clone: add INST resource under new ID, same SND references
        result = PV_BankReplaceResource(bankFile, ID_INST,
                                        (XLongResourceID)destInstID,
                                        instName, instData, instSize);
    }

    XDisposePtr(instData);
    return result;
}

// Add an alias entry (aliasFrom -> srcInstID) to the bank's ID_ALIAS resource.
BAEResult BAERmfEditorBank_AliasInstrument(BAEBankToken bankToken,
                                           uint32_t instrumentIndex,
                                           uint32_t aliasInstID)
{
    XFILE bankFile;
    XLongResourceID srcInstID;
    XPTR instData;
    int32_t instSize;
    XAliasLinkResource *oldAlias;
    uint32_t oldCount;
    uint32_t newCount;
    int32_t newSize;
    XAliasLinkResource *newAlias;
    BAEResult result;

    if (!bankToken)
        return BAE_PARAM_ERR;

    bankFile = (XFILE)bankToken;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &srcInstID,
                                       (int32_t)instrumentIndex, NULL, &instSize);
    if (!instData)
        return BAE_BAD_FILE;
    XDisposePtr(instData);

    oldAlias = XGetAliasLinkFromFile(bankFile);
    oldCount = oldAlias ? (uint32_t)XGetLong(&oldAlias->numberOfAliases) : 0u;

    // Check the alias doesn't already exist
    for (uint32_t i = 0; i < oldCount; ++i)
    {
        if ((uint32_t)XGetLong(&oldAlias->list[i].aliasFrom) == aliasInstID)
        {
            XDisposePtr((XPTR)oldAlias);
            return BAE_PARAM_ERR;  // slot already aliased
        }
    }

    newCount = oldCount + 1;
    newSize  = (int32_t)(sizeof(uint32_t) * 2 +
                         newCount * sizeof(XAliasLink));
    newAlias = (XAliasLinkResource *)XNewPtr(newSize);
    if (!newAlias)
    {
        XDisposePtr((XPTR)oldAlias);
        return BAE_MEMORY_ERR;
    }

    XPutLong(&newAlias->version, ALIAS_ID_RESOURCE_VERSION);
    XPutLong(&newAlias->numberOfAliases, newCount);

    for (uint32_t i = 0; i < oldCount; ++i)
    {
        XPutLong(&newAlias->list[i].aliasFrom, XGetLong(&oldAlias->list[i].aliasFrom));
        XPutLong(&newAlias->list[i].aliasTo,   XGetLong(&oldAlias->list[i].aliasTo));
    }
    XPutLong(&newAlias->list[oldCount].aliasFrom, aliasInstID);
    XPutLong(&newAlias->list[oldCount].aliasTo,   (uint32_t)srcInstID);

    XDisposePtr((XPTR)oldAlias);

    // Write the updated alias resource back
    {
        char emptyName[1] = {0};
        result = PV_BankReplaceResource(bankFile, ID_ALIAS,
                                        DEFAULT_RESOURCE_ALIAS_ID,
                                        emptyName, newAlias, newSize);
    }
    XDisposePtr((XPTR)newAlias);
    return result;
}

static uint32_t PV_BankCountSndReferences(XFILE bankFile, XShortResourceID sndID)
{
    enum
    {
        kInstHeaderMinSize = 14,
        kInstKeySplitSize = 8
    };
    uint32_t refs;
    int32_t instCount;
    int32_t instIndex;

    if (!bankFile)
    {
        return 0;
    }

    refs = 0;
    instCount = XCountFileResourcesOfType(bankFile, ID_INST);
    for (instIndex = 0; instIndex < instCount; ++instIndex)
    {
        XLongResourceID instID;
        int32_t instSize;
        XPTR instData;
        int16_t splitCount;

        instData = XGetIndexedFileResource(bankFile, ID_INST, &instID, instIndex, NULL, &instSize);
        if (!instData || instSize < kInstHeaderMinSize)
        {
            if (instData)
            {
                XDisposePtr(instData);
            }
            continue;
        }

        splitCount = (int16_t)XGetShort((unsigned char *)instData + 12);
        if (splitCount < 0)
        {
            splitCount = 0;
        }

        if (splitCount > 0)
        {
            int16_t splitIndex;
            if (instSize >= (kInstHeaderMinSize + (splitCount * kInstKeySplitSize)))
            {
                for (splitIndex = 0; splitIndex < splitCount; ++splitIndex)
                {
                    unsigned char *splitPtr;
                    XShortResourceID splitSndID;

                    splitPtr = (unsigned char *)instData + 14 + (splitIndex * kInstKeySplitSize);
                    splitSndID = (XShortResourceID)XGetShort(splitPtr + 2);
                    if (splitSndID == sndID)
                    {
                        ++refs;
                    }
                }
            }
        }
        else
        {
            XShortResourceID baseSndID = (XShortResourceID)XGetShort((unsigned char *)instData + 0);
            if (baseSndID == sndID)
            {
                ++refs;
            }
        }

        XDisposePtr(instData);
    }

    return refs;
}

BAEResult BAERmfEditorBank_GrowInstrumentSampleSlots(BAEBankToken bankToken,
                                                      uint32_t instrumentIndex,
                                                      uint32_t desiredSampleCount)
{
    enum
    {
        kInstHeaderMinSize = 14
    };
    XFILE bankFile;
    XLongResourceID instID;
    XPTR instData;
    int32_t instSize;
    char instName[256];
    int16_t splitCount;
    uint32_t currentCount;
    InstrumentResource *grown;
    int16_t howMany;
    uint32_t startInit;
    uint32_t i;
    unsigned char defaultRoot;
    int16_t defaultSplitVolume;
    KeySplit newSplit;
    BAEResult replaceResult;

    if (!bankToken || desiredSampleCount == 0)
    {
        return BAE_PARAM_ERR;
    }
    if (desiredSampleCount > 32767u)
    {
        return BAE_PARAM_ERR;
    }

    bankFile = (XFILE)bankToken;
    instName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, instName, &instSize);
    if (!instData)
    {
        return BAE_BAD_FILE;
    }
    if (instSize < kInstHeaderMinSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
    }

    splitCount = (int16_t)XGetShort((unsigned char *)instData + 12);
    if (splitCount < 0)
    {
        splitCount = 0;
    }
    currentCount = (splitCount > 0) ? (uint32_t)splitCount : 1u;
    if (desiredSampleCount <= currentCount)
    {
        XDisposePtr(instData);
        return BAE_NO_ERROR;
    }

    if (splitCount > 0)
    {
        howMany = (int16_t)(desiredSampleCount - (uint32_t)splitCount);
        grown = XAddKeySplit((InstrumentResource *)instData, howMany);
        startInit = (uint32_t)splitCount;
    }
    else
    {
        /* Convert legacy non-split INST to split form, then grow to desired count. */
        howMany = (int16_t)desiredSampleCount;
        grown = XAddKeySplit((InstrumentResource *)instData, howMany);
        startInit = 0;
    }

    if (!grown)
    {
        XDisposePtr(instData);
        return BAE_MEMORY_ERR;
    }

    defaultRoot = (unsigned char)XGetShort((unsigned char *)grown + 2);
    defaultSplitVolume = (int16_t)XGetShort((unsigned char *)grown + 10);
    if (defaultSplitVolume == 0)
    {
        defaultSplitVolume = 100;
    }

    if (splitCount == 0)
    {
        /* Slot 0 preserves legacy header sample mapping. */
        XShortResourceID baseSnd = (XShortResourceID)XGetShort((unsigned char *)instData + 0);
        int16_t baseRoot = (int16_t)XGetShort((unsigned char *)instData + 2);
        int16_t miscParam1 = (int16_t)XGetShort((unsigned char *)instData + 8);
        int16_t baseVolume = (int16_t)XGetShort((unsigned char *)instData + 10);
        bool useSoundModifierAsRootKey = TEST_FLAG_VALUE(((unsigned char *)instData)[6], ZBF_useSoundModifierAsRootKey);
        int16_t splitRoot = baseRoot;

        if (useSoundModifierAsRootKey)
        {
            if (miscParam1 > 0 && miscParam1 <= 127)
            {
                splitRoot = miscParam1;
            }
        }

        XSetMemory(&newSplit, (int32_t)sizeof(newSplit), 0);
        newSplit.lowMidi = 0;
        newSplit.highMidi = 127;
        newSplit.sndResourceID = baseSnd;
        newSplit.miscParameter1 = splitRoot;
        newSplit.miscParameter2 = baseVolume;
        XSetKeySplitFromPtr(grown, 0, &newSplit);
        startInit = 1;
    }

    for (i = startInit; i < desiredSampleCount; ++i)
    {
        XSetMemory(&newSplit, (int32_t)sizeof(newSplit), 0);
        newSplit.lowMidi = 0;
        newSplit.highMidi = 127;
        newSplit.sndResourceID = 0;
        newSplit.miscParameter1 = (int16_t)defaultRoot;
        newSplit.miscParameter2 = defaultSplitVolume;
        XSetKeySplitFromPtr(grown, (int16_t)i, &newSplit);
    }

    /* Keep header sndResourceID aligned with split 0 for compatibility. */
    {
        KeySplit split0;
        XGetKeySplitFromPtr(grown, 0, &split0);
        XPutShort((unsigned char *)grown + 0, (uint16_t)split0.sndResourceID);
    }

    replaceResult = PV_BankReplaceResource(bankFile,
                                           ID_INST,
                                           instID,
                                           instName,
                                           grown,
                                           XGetPtrSize((XPTR)grown));

    XDisposePtr((XPTR)grown);
    XDisposePtr(instData);
    return replaceResult;
}

BAEResult BAERmfEditorBank_DeleteInstrumentSample(BAEBankToken bankToken,
                                                   uint32_t instrumentIndex,
                                                   uint32_t sampleIndex,
                                                   bool deleteSndIfUnreferenced)
{
    enum
    {
        kInstHeaderMinSize = 14,
        kInstKeySplitSize = 8
    };
    XFILE bankFile;
    XLongResourceID instID;
    XPTR instData;
    int32_t instSize;
    char instName[256];
    int16_t splitCount;
    XShortResourceID removedSndID;
    BAEResult result;

    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

    instName[0] = 0;
    instData = XGetIndexedFileResource(bankFile, ID_INST, &instID,
                                       (int32_t)instrumentIndex, instName, &instSize);
    if (!instData)
    {
        return BAE_BAD_FILE;
    }
    if (instSize < kInstHeaderMinSize)
    {
        XDisposePtr(instData);
        return BAE_BAD_FILE;
    }

    splitCount = (int16_t)XGetShort((unsigned char *)instData + 12);
    if (splitCount < 0)
    {
        splitCount = 0;
    }

    removedSndID = 0;
    if (splitCount > 0)
    {
        InstrumentResource *shrunk;
        KeySplit removedSplit;
        int16_t newSplitCount;

        if (sampleIndex >= (uint32_t)splitCount)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }

        XGetKeySplitFromPtr((InstrumentResource *)instData, (int16_t)sampleIndex, &removedSplit);
        removedSndID = removedSplit.sndResourceID;

        shrunk = XRemoveThisKeySplit((InstrumentResource *)instData, (int16_t)sampleIndex);
        if (!shrunk)
        {
            XDisposePtr(instData);
            return BAE_MEMORY_ERR;
        }

        newSplitCount = (int16_t)XGetShort(&shrunk->keySplitCount);
        if (newSplitCount > 0)
        {
            KeySplit split0;
            XGetKeySplitFromPtr(shrunk, 0, &split0);
            XPutShort((unsigned char *)shrunk + 0, (uint16_t)split0.sndResourceID);
        }
        else
        {
            XPutShort((unsigned char *)shrunk + 0, 0);
        }

        result = PV_BankReplaceResource(bankFile,
                                        ID_INST,
                                        instID,
                                        instName,
                                        shrunk,
                                        XGetPtrSize((XPTR)shrunk));
        XDisposePtr((XPTR)shrunk);
        XDisposePtr(instData);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }
    else
    {
        if (sampleIndex != 0)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }

        removedSndID = (XShortResourceID)XGetShort((unsigned char *)instData + 0);
        XPutShort((unsigned char *)instData + 0, 0);

        result = PV_BankReplaceResource(bankFile,
                                        ID_INST,
                                        instID,
                                        instName,
                                        instData,
                                        instSize);
        XDisposePtr(instData);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
    }

    /* removedSndID 0 is a valid SND id — probe by resource existence, not != 0. */
    if (deleteSndIfUnreferenced)
    {
        if (PV_BankCountSndReferences(bankFile, removedSndID) == 0)
        {
            XResourceType sndType;
            XPTR sndData;
            int32_t sndSize;

            result = PV_BankFindSndResource(bankFile,
                                            removedSndID,
                                            &sndType,
                                            &sndData,
                                            &sndSize,
                                            NULL);
            if (result == BAE_NO_ERROR)
            {
                XDisposePtr(sndData);
                (void)PV_BankDeleteResource(bankFile, sndType, (XLongResourceID)removedSndID);
            }
        }
    }

    return BAE_NO_ERROR;
}

/* Serialize all resources from a loaded bank file into a new in-memory
 * resource image.  The output is a complete IREZ or ZREZ blob that can
 * be written to disk or loaded via BAEMixer_AddBankFromMemory.
 *
 * The format is preserved from the source bank (IREZ stays IREZ, ZREZ
 * stays ZREZ) unless overrideResourceID is non-zero, in which case that
 * format is used instead.  All resource types are copied verbatim. */
static BAEResult PV_BankSaveToMemory(BAEBankToken bankToken,
                                     int32_t overrideResourceID,
                                     unsigned char **outData,
                                     uint32_t *outSize)
{
    XFILE bankFile;
    XFILE outFile;
    XFILERESOURCEMAP map;
    int32_t resourceID;
    XPTR data;
    int32_t size;

    if (!outData || !outSize)
    {
        return BAE_PARAM_ERR;
    }
    *outData = NULL;
    *outSize = 0;
    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

    /* Read the source bank's map header to determine IREZ vs ZREZ */
    if (XFileSetPosition(bankFile, 0L) != 0 ||
        XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
    {
        return BAE_BAD_FILE;
    }
    resourceID = (int32_t)XGetLong(&map.mapID);
    if (!XFILERESOURCE_ID_IS_VALID(resourceID))
    {
        return BAE_BAD_FILE;
    }

    /* Override format if requested (e.g. .zsb extension forces ZREZ) */
    if (overrideResourceID != 0)
    {
        resourceID = overrideResourceID;
    }

    /* Create a virtual (in-memory) resource file with the same format */
    outFile = XFileOpenVirtualResource(resourceID);
    if (!outFile)
    {
        return BAE_MEMORY_ERR;
    }

    /* Enumerate all known bank resource types and copy each resource.
     * Bank files typically contain: INST, SND/CSND/ESND, ALIAS, and
     * possibly other types.  We iterate over a fixed list of known types
     * rather than using XCountTypes/XGetIndexedType (which are unimplemented). */
    {
        static const XResourceType bankResourceTypes[] = {
            ID_INST,
            ID_SND,
            ID_CSND,
            ID_ESND,
            ID_ALIAS,
            ID_BANK,
            ID_SONG,
            ID_MIDI,
            ID_MIDI_OLD,
            ID_CMID,
            ID_EMID,
            ID_ECMI,
            ID_RMF,
            ID_TEXT,
            ID_VERS,
            0  /* sentinel */
        };
        int32_t typeIdx;

        int32_t soundCount = 0;
        for (typeIdx = 0; bankResourceTypes[typeIdx] != 0; ++typeIdx)
        {
            XResourceType resType;
            int32_t resCount;
            int32_t resIndex;

            resType = bankResourceTypes[typeIdx];
            resCount = XCountFileResourcesOfType(bankFile, resType);
            for (resIndex = 0; resIndex < resCount; ++resIndex)
            {
                if (typeIdx == ID_SND || typeIdx == ID_CSND || typeIdx == ID_ESND)
                {
                    ++soundCount;
                }
                if (soundCount > MAX_SAMPLES)
                {
                    XFileClose(outFile);
                    return BAE_TOO_MANY_SAMPLES;  // too many sound resources for 16-bit IDs
                }
                XLongResourceID resID;
                int32_t resSize;
                XPTR resData;
                char resName[256];

                resName[0] = 0;
                resData = XGetIndexedFileResource(bankFile, resType, &resID,
                                                  resIndex, resName, &resSize);
                if (!resData)
                {
                    continue;
                }
                int32_t result = XAddFileResource(outFile, resType, resID, resName, resData, resSize);
                if (result != 0)
                {
                    XDisposePtr(resData);
                    XFileClose(outFile);
                    return result;
                }
                XDisposePtr(resData);
            }
        }
    }

    /* Finalize the resource file */
    if (XCleanResourceFile(outFile) == FALSE)
    {
        XFileClose(outFile);
        return BAE_FILE_IO_ERROR;
    }

    /* Extract the serialized data */
    data = NULL;
    size = 0;
    if (XFileGetMemoryFileAsData(outFile, &data, &size) != 0 || !data || size <= 0)
    {
        XFileClose(outFile);
        if (data)
        {
            XDisposePtr(data);
        }
        return BAE_MEMORY_ERR;
    }
    XFileClose(outFile);

    *outData = (unsigned char *)data;
    *outSize = (uint32_t)size;
    return BAE_NO_ERROR;
}

/* Public wrapper - preserves source bank format (no override). */
BAEResult BAERmfEditorBank_SaveToMemory(BAEBankToken bankToken,
                                        unsigned char **outData,
                                        uint32_t *outSize)
{
    return PV_BankSaveToMemory(bankToken, 0, outData, outSize);
}

BAEResult BAERmfEditorBank_SaveToFile(BAEBankToken bankToken,
                                      BAEPathName filePath)
{
    unsigned char *bankData;
    uint32_t bankSize;
    XFILENAME name;
    XFILE fileRef;
    BAEResult result;
    int32_t overrideResourceID;
    const char *ext;

    if (!bankToken || !filePath)
    {
        return BAE_PARAM_ERR;
    }

    /* Choose ZREZ format for .zsb files, like ZMF does for .zmf */
    overrideResourceID = 0;
    ext = strrchr(filePath, '.');
    if (ext && (strcmp(ext, ".zsb") == 0 || strcmp(ext, ".ZSB") == 0))
    {
#if USE_ZMF_SUPPORT == TRUE
        overrideResourceID = XFILERESOURCE_ZMF_ID;
#else
        return BAE_UNSUPPORTED_FORMAT;
#endif
    }
    else if (ext && (strcmp(ext, ".hsb") == 0 || strcmp(ext, ".HSB") == 0))
    {
        uint32_t zsbReason = 0;
        if (BAERmfEditorBank_RequiresZsb(bankToken, &zsbReason) != FALSE)
        {
            /* Modern codecs / extended ADSR / etc. cannot be written as classic HSB. */
            return BAE_UNSUPPORTED_FORMAT;
        }
        overrideResourceID = XFILERESOURCE_ID;
    }

    bankData = NULL;
    bankSize = 0;
    result = PV_BankSaveToMemory(bankToken, overrideResourceID, &bankData, &bankSize);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    XConvertPathToXFILENAME(filePath, &name);
    fileRef = XFileOpenForWrite(&name, TRUE);
    if (!fileRef)
    {
        XDisposePtr((XPTR)bankData);
        return BAE_FILE_IO_ERROR;
    }

    if (XFileSetLength(fileRef, 0) != 0 ||
        XFileSetPosition(fileRef, 0L) != 0 ||
        XFileWrite(fileRef, bankData, (int32_t)bankSize) != 0)
    {
        XFileClose(fileRef);
        XDisposePtr((XPTR)bankData);
        return BAE_FILE_IO_ERROR;
    }

    XFileClose(fileRef);
    XDisposePtr((XPTR)bankData);
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorBank_GetSampleWaveformData(BAEBankToken bankToken,
                                                  uint32_t instrumentIndex,
                                                  uint32_t sampleIndex,
                                                  void **outWaveData,
                                                  uint32_t *outFrameCount,
                                                  uint16_t *outBitSize,
                                                  uint16_t *outChannels,
                                                  BAE_UNSIGNED_FIXED *outSampleRate)
{
    enum
    {
        kInstHeaderMinSize = 14
    };
    XFILE bankFile;
    XPTR instData;
    XLongResourceID instID;
    int32_t instSize;
    char rawName[256];
    InstrumentResource *inst;
    KeySplit split;
    XShortResourceID sndID;
    int16_t splitCount;
    XPTR sndData;
    int32_t sndSize;
    SampleDataInfo sdi;
    XPTR pcmData;
    XPTR pcmOwner;
    int32_t pcmSize;
    XPTR ownedPcm;
    /* Prefer encrypted/compressed containers — session promote can leave a
     * colliding plain GM SND alongside the intended ESND/CSND. */
    static const XResourceType sndTypes[] = { ID_ESND, ID_CSND, ID_SND, 0 };
    int32_t typeIdx;

    if (!outWaveData || !outFrameCount || !outBitSize || !outChannels || !outSampleRate)
    {
        return BAE_PARAM_ERR;
    }
    *outWaveData = NULL;
    *outFrameCount = 0;
    *outBitSize = 0;
    *outChannels = 0;
    *outSampleRate = 0;
    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

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
    inst = (InstrumentResource *)instData;
    splitCount = (int16_t)XGetShort(&inst->keySplitCount);
    if (splitCount < 0)
    {
        splitCount = 0;
    }

    /* Resolve the SND resource ID from the instrument or key split */
    if (splitCount > 0)
    {
        if (sampleIndex >= (uint32_t)splitCount)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }
        XGetKeySplitFromPtr(inst, (int16_t)sampleIndex, &split);
        sndID = split.sndResourceID;
    }
    else
    {
        if (sampleIndex != 0)
        {
            XDisposePtr(instData);
            return BAE_PARAM_ERR;
        }
        sndID = (XShortResourceID)XGetShort(&inst->sndResourceID);
    }
    XDisposePtr(instData);

    /* Load the SND resource from the bank file */
    sndData = NULL;
    sndSize = 0;
    for (typeIdx = 0; sndTypes[typeIdx] != 0; ++typeIdx)
    {
        sndData = XGetFileResource(bankFile, sndTypes[typeIdx], (XLongResourceID)sndID, NULL, &sndSize);
        if (sndData)
        {
            /* Match sample-info path: preprocess container payload before
             * parsing/decoding the SND structure. */
            if (sndTypes[typeIdx] == ID_CSND)
            {
                XPTR decompressed = XDecompressPtr(sndData, (uint32_t)sndSize, FALSE);
                XDisposePtr(sndData);
                sndData = decompressed;
                if (sndData)
                {
                    sndSize = XGetPtrSize(sndData);
                }
            }
            else if (sndTypes[typeIdx] == ID_ESND)
            {
                XDecryptData(sndData, (uint32_t)sndSize);
            }
            break;
        }
    }
    if (!sndData || sndSize <= 0)
    {
        return BAE_BAD_FILE;
    }

    /* Validate the SND format */
    {
        int16_t soundFormat = (int16_t)XGetShort(sndData);
        if (soundFormat != 1 && soundFormat != 2 && soundFormat != 3)
        {
            XDisposePtr(sndData);
            return BAE_BAD_FILE;
        }
    }

    /* Decode the SND to PCM */
    XSetMemory(&sdi, (int32_t)sizeof(sdi), 0);
    pcmData = XGetSamplePtrFromSnd(sndData, &sdi);
    pcmOwner = NULL;
    if (sdi.pMasterPtr && sdi.pMasterPtr != sndData)
    {
        pcmOwner = sdi.pMasterPtr;
    }
    if (!pcmData || sdi.bitSize == 0 || sdi.channels == 0)
    {
        if (pcmOwner)
        {
            XDisposePtr(pcmOwner);
        }
        XDisposePtr(sndData);
        return BAE_BAD_FILE;
    }

    /* Copy the PCM data so we can free the SND resource */
    pcmSize = (int32_t)(sdi.frames * (sdi.bitSize / 8) * sdi.channels);
    if (pcmSize <= 0)
    {
        if (pcmOwner)
        {
            XDisposePtr(pcmOwner);
        }
        XDisposePtr(sndData);
        return BAE_BAD_FILE;
    }
    ownedPcm = XNewPtr(pcmSize);
    if (!ownedPcm)
    {
        if (pcmOwner)
        {
            XDisposePtr(pcmOwner);
        }
        XDisposePtr(sndData);
        return BAE_MEMORY_ERR;
    }
    XBlockMove(pcmData, ownedPcm, pcmSize);

    if (pcmOwner)
    {
        XDisposePtr(pcmOwner);
    }
    XDisposePtr(sndData);

    *outWaveData = ownedPcm;
    *outFrameCount = sdi.frames;
    *outBitSize = sdi.bitSize;
    *outChannels = sdi.channels;
    *outSampleRate = sdi.rate;
    return BAE_NO_ERROR;
}

BAEResult BAERmfEditorBank_GetSndWaveformData(BAEBankToken bankToken,
                                               uint32_t sndResourceID,
                                               void **outWaveData,
                                               uint32_t *outFrameCount,
                                               uint16_t *outBitSize,
                                               uint16_t *outChannels,
                                               BAE_UNSIGNED_FIXED *outSampleRate)
{
    XFILE bankFile;
    XPTR sndData;
    int32_t sndSize;
    static const XResourceType sndTypes[] = { ID_ESND, ID_CSND, ID_SND, 0 };
    int32_t typeIdx;
    SampleDataInfo sdi;
    XPTR pcmData;
    XPTR pcmOwner;
    int32_t pcmSize;
    XPTR ownedPcm;

    if (!outWaveData || !outFrameCount || !outBitSize || !outChannels || !outSampleRate)
    {
        return BAE_PARAM_ERR;
    }
    *outWaveData = NULL;
    *outFrameCount = 0;
    *outBitSize = 0;
    *outChannels = 0;
    *outSampleRate = 0;
    if (!bankToken || sndResourceID > 32767u)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

    sndData = NULL;
    sndSize = 0;
    for (typeIdx = 0; sndTypes[typeIdx] != 0; ++typeIdx)
    {
        sndData = XGetFileResource(bankFile,
                                   sndTypes[typeIdx],
                                   (XLongResourceID)sndResourceID,
                                   NULL,
                                   &sndSize);
        if (sndData)
        {
            if (sndTypes[typeIdx] == ID_CSND)
            {
                XPTR decompressed = XDecompressPtr(sndData, (uint32_t)sndSize, FALSE);
                XDisposePtr(sndData);
                sndData = decompressed;
                if (sndData)
                {
                    sndSize = XGetPtrSize(sndData);
                }
            }
            else if (sndTypes[typeIdx] == ID_ESND)
            {
                XDecryptData(sndData, (uint32_t)sndSize);
            }
            break;
        }
    }
    if (!sndData || sndSize <= 0)
    {
        return BAE_BAD_FILE;
    }
    {
        int16_t soundFormat = (int16_t)XGetShort(sndData);
        if (soundFormat != 1 && soundFormat != 2 && soundFormat != 3)
        {
            XDisposePtr(sndData);
            return BAE_BAD_FILE;
        }
    }

    XSetMemory(&sdi, (int32_t)sizeof(sdi), 0);
    pcmData = XGetSamplePtrFromSnd(sndData, &sdi);
    pcmOwner = NULL;
    if (sdi.pMasterPtr && sdi.pMasterPtr != sndData)
    {
        pcmOwner = sdi.pMasterPtr;
    }
    if (!pcmData || sdi.bitSize == 0 || sdi.channels == 0)
    {
        if (pcmOwner)
        {
            XDisposePtr(pcmOwner);
        }
        XDisposePtr(sndData);
        return BAE_BAD_FILE;
    }
    pcmSize = (int32_t)(sdi.frames * (sdi.bitSize / 8) * sdi.channels);
    if (pcmSize <= 0)
    {
        if (pcmOwner)
        {
            XDisposePtr(pcmOwner);
        }
        XDisposePtr(sndData);
        return BAE_BAD_FILE;
    }
    ownedPcm = XNewPtr(pcmSize);
    if (!ownedPcm)
    {
        if (pcmOwner)
        {
            XDisposePtr(pcmOwner);
        }
        XDisposePtr(sndData);
        return BAE_MEMORY_ERR;
    }
    XBlockMove(pcmData, ownedPcm, pcmSize);
    if (pcmOwner)
    {
        XDisposePtr(pcmOwner);
    }
    XDisposePtr(sndData);

    *outWaveData = ownedPcm;
    *outFrameCount = sdi.frames;
    *outBitSize = sdi.bitSize;
    *outChannels = sdi.channels;
    *outSampleRate = sdi.rate;
    return BAE_NO_ERROR;
}

void BAERmfEditorBank_FreeWaveformData(void *waveData)
{
    if (waveData)
    {
        XDisposePtr((XPTR)waveData);
    }
}

BAEResult BAERmfEditorBank_ExportSndResourceToFile(BAEBankToken bankToken,
                                                   uint32_t sndResourceID,
                                                   BAEPathName filePath)
{
    XFILE bankFile;
    XPTR sndData;
    int32_t sndSize;
    static const XResourceType sndTypes[] = { ID_SND, ID_CSND, ID_ESND, 0 };
    int32_t typeIdx;
    SampleDataInfo sdi;
    BAEResult passResult;
    XPTR pcmData;
    XPTR pcmOwner;
    int32_t pcmSize;
    GM_Waveform wave;
    XFILENAME fileName;
    OPErr opErr;
    AudioFileType outType;
    char const *ext;

    if (!bankToken || !filePath)
    {
        return BAE_PARAM_ERR;
    }
    bankFile = (XFILE)bankToken;

    sndData = NULL;
    sndSize = 0;
    for (typeIdx = 0; sndTypes[typeIdx] != 0; ++typeIdx)
    {
        sndData = XGetFileResource(bankFile, sndTypes[typeIdx],
                                   (XLongResourceID)sndResourceID, NULL, &sndSize);
        if (sndData)
        {
            if (sndTypes[typeIdx] == ID_CSND)
            {
                XPTR decompressed = XDecompressPtr(sndData, (uint32_t)sndSize, FALSE);
                XDisposePtr(sndData);
                sndData = decompressed;
                if (sndData)
                {
                    sndSize = XGetPtrSize(sndData);
                }
            }
            else if (sndTypes[typeIdx] == ID_ESND)
            {
                XDecryptData(sndData, (uint32_t)sndSize);
            }
            break;
        }
    }
    if (!sndData || sndSize <= 0)
    {
        return BAE_BAD_FILE;
    }

    {
        int16_t soundFormat = (int16_t)XGetShort(sndData);
        if (soundFormat != 1 && soundFormat != 2 && soundFormat != 3)
        {
            XDisposePtr(sndData);
            return BAE_BAD_FILE;
        }
    }

    XSetMemory(&sdi, (int32_t)sizeof(sdi), 0);
    if (XGetSampleInfoFromSnd(sndData, &sdi) == 0)
    {
        passResult = PV_ExportSndBitstreamToFile(sndData,
                                                 sndSize,
                                                 (SndCompressionType)sdi.compressionType,
                                                 filePath);
        if (passResult != BAE_NOT_SETUP)
        {
            XDisposePtr(sndData);
            return passResult;
        }
    }

    /* PCM / IMA / ADPCM (or failed bitstream extract): decode and write WAV/AIFF. */
    XSetMemory(&sdi, (int32_t)sizeof(sdi), 0);
    pcmData = XGetSamplePtrFromSnd(sndData, &sdi);
    pcmOwner = NULL;
    if (sdi.pMasterPtr && sdi.pMasterPtr != sndData)
    {
        pcmOwner = sdi.pMasterPtr;
    }
    if (!pcmData || sdi.bitSize == 0 || sdi.channels == 0 || sdi.frames == 0)
    {
        if (pcmOwner)
        {
            XDisposePtr(pcmOwner);
        }
        XDisposePtr(sndData);
        return BAE_BAD_FILE;
    }

    pcmSize = (int32_t)(sdi.frames * (sdi.bitSize / 8) * sdi.channels);
    if (pcmSize <= 0)
    {
        if (pcmOwner)
        {
            XDisposePtr(pcmOwner);
        }
        XDisposePtr(sndData);
        return BAE_BAD_FILE;
    }

    XSetMemory(&wave, (int32_t)sizeof(wave), 0);
    wave.theWaveform = (XPTR)pcmData;
    wave.waveSize = (uint32_t)pcmSize;
    wave.waveFrames = sdi.frames;
    wave.bitSize = (unsigned char)sdi.bitSize;
    wave.channels = (unsigned char)sdi.channels;
    wave.sampledRate = sdi.rate;
    wave.baseMidiPitch = (uint16_t)sdi.baseKey;
    wave.startLoop = sdi.loopStart;
    wave.endLoop = sdi.loopEnd;

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

    XConvertPathToXFILENAME(filePath, &fileName);
    opErr = GM_WriteFileFromMemory(&fileName, &wave, outType);

    if (pcmOwner)
    {
        XDisposePtr(pcmOwner);
    }
    XDisposePtr(sndData);
    return (opErr == NO_ERR) ? BAE_NO_ERROR : BAE_FILE_IO_ERROR;
}

BAEResult BAERmfEditorBank_ExportSampleToFile(BAEBankToken bankToken,
                                              uint32_t instrumentIndex,
                                              uint32_t sampleIndex,
                                              BAEPathName filePath)
{
    BAERmfEditorBankSampleInfo info;
    BAEResult result;

    result = BAERmfEditorBank_GetInstrumentSampleInfo(bankToken, instrumentIndex,
                                                     sampleIndex, &info);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    return BAERmfEditorBank_ExportSndResourceToFile(bankToken,
                                                    (uint32_t)(uint16_t)info.sndResourceID,
                                                    filePath);
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
                splitRootForLoad = PV_ClampMidi7Bit(baseRootKey);
            }

            sampleName[0] = 0;
            if (PV_GetEmbeddedSampleDisplayName(bankFile, split.sndResourceID, sampleName) != BAE_NO_ERROR)
            {
                XStrCpy(sampleName, instName);
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
                newSample->sampleAssetID = PV_AllocateSampleAssetID(document);
                newSample->splitVolume = split.miscParameter2;
            }
        }
    }
    else
    {
        unsigned char nonSplitRootForLoad;
        char sampleName[256];

        if (useSoundModifierAsRootKey)
        {
            nonSplitRootForLoad = PV_ClampMidi7Bit(instMiscParam1 ? instMiscParam1 : baseRootKey);
        }
        else
        {
            nonSplitRootForLoad = PV_ClampMidi7Bit(baseRootKey);
        }

        sampleName[0] = 0;
        if (PV_GetEmbeddedSampleDisplayName(bankFile, baseSndID, sampleName) != BAE_NO_ERROR)
        {
            XStrCpy(sampleName, instName);
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
            BAERmfEditorSample *newSample = &document->samples[document->sampleCount - 1];
            newSample->sampleAssetID = PV_AllocateSampleAssetID(document);
            newSample->splitVolume = (int16_t)XGetShort(&inst->miscParameter2);
        }
    }

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

    XDisposePtr(instData);
    return BAE_NO_ERROR;
}

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

static bool PV_AddUsedInstrumentPair(PV_UsedInstrumentPair *pairs,
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

static int PV_CompareUsedInstrumentPairs(void const *left, void const *right)
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

static bool PV_AddUsedPercussion(PV_UsedPercussion *entries,
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

static void PV_RemapPercussionNoteReferences(BAERmfEditorDocument *document,
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

static void PV_RemapPitchedNoteReferences(BAERmfEditorDocument *document,
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
            if (note->bank == sourceBank && note->program == sourceProgram)
            {
                note->bank = targetBank;
                note->program = targetProgram;
            }
        }
    }
}

static uint16_t PV_BankGroupFromInternalBank(uint16_t internalBank)
{
    return internalBank >= 128 ? (uint16_t)((internalBank >> 7) & 0x7F) : internalBank;
}

static void PV_RemoveDocumentInstrumentAuxEvents(BAERmfEditorDocument *document)
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

static void PV_SynchronizeTrackInstrumentDefaults(BAERmfEditorDocument *document)
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
                /* Classic GM drums: note selects the hit; keep track defaults clear. */
                track->bank = 0;
                track->program = 0;
            }
            else
            {
                /* Melodic percussion (CloneUsed bank-2 embeds) and pitched tracks
                   keep the remapped bank/program so MIDI emit matches note state. */
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

static BAEResult PV_VerifyEmbeddedOnlyInstrumentReferences(BAERmfEditorDocument const *document,
                                                           uint16_t embeddedBank)
{
    uint32_t trackIndex;

    for (trackIndex = 0; trackIndex < document->trackCount; ++trackIndex)
    {
        BAERmfEditorTrack const *track = &document->tracks[trackIndex];
        uint32_t noteIndex;

        for (noteIndex = 0; noteIndex < track->noteCount; ++noteIndex)
        {
            if (track->notes[noteIndex].bank != embeddedBank)
            {
                debug_message("[CloneUsed] Unmapped note track=%u note=%u bank=%u program=%u\n",
                              (unsigned)trackIndex,
                              (unsigned)noteIndex,
                              (unsigned)track->notes[noteIndex].bank,
                              (unsigned)track->notes[noteIndex].program);
                return BAE_BAD_FILE;
            }
        }
    }
    return BAE_NO_ERROR;
}

static BAEResult PV_VerifyClonedInstrument(BAERmfEditorDocument const *document,
                                           BAEBankToken bankToken,
                                           uint32_t instrumentIndex,
                                           uint32_t targetInstID,
                                           uint32_t firstSampleIndex,
                                           uint32_t *outSampleCount)
{
    uint32_t sourceSampleCount;
    uint32_t sampleOffset;

    sourceSampleCount = 0;
    if (BAERmfEditorBank_GetInstrumentSampleCount(bankToken,
                                                   instrumentIndex,
                                                   &sourceSampleCount) != BAE_NO_ERROR ||
        sourceSampleCount == 0 ||
        document->sampleCount != firstSampleIndex + sourceSampleCount)
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
                                                      &sourceSample) != BAE_NO_ERROR ||
            clonedSample->instID != targetInstID ||
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

static void PV_AddCloneUsedMapping(BAERmfEditorCloneUsedResult *outResult,
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

static BAE_BOOL PV_ExportShouldEmbedInst(uint32_t resolvedInstID,
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

static BAEResult PV_PrepareUsedInstrumentsFromBank(
    BAERmfEditorDocument *document,
    BAEBankToken bankToken,
    BAE_BOOL embedAll,
    uint32_t const *embedResolvedInstIDs,
    uint32_t embedCount,
    BAERmfEditorCloneUsedResult *outResult)
{
    enum { kMaximumPitchedInstruments = 128 };
    static const uint16_t kClonedInstrumentBank = (2u << 7);
    PV_UsedInstrumentPair pitchedPairs[kMaximumPitchedInstruments];
    PV_UsedPercussion usedPercussion[kMaximumPitchedInstruments];
    uint32_t clonedPercussionInstIDs[kMaximumPitchedInstruments];
    unsigned char clonedPercussionPrograms[kMaximumPitchedInstruments];
    uint32_t clonedPercussionSampleCounts[kMaximumPitchedInstruments];
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
               CH10 + bank != 0 = melodic percussion from a prior CloneUsed
               (bank+program → INST 512+p); treat as pitched or Resolve fails. */
            if (noteInfo.channel == 9 && noteInfo.bank == 0)
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
        uint32_t resolvedInstID;
        uint32_t firstSampleIndex;
        uint32_t clonedSampleCount;
        BAERmfEditorBankInstrumentInfo sourceInfo;
        bool found;

        instrumentCount = 0;
        expectedInstID = ((uint32_t)PV_BankGroupFromInternalBank(pair->bank) * 256u) + pair->program;
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
                debug_message("[CloneUsed] Missing pitched INST=%u for bank=%u program=%u\n",
                              (unsigned)expectedInstID,
                              (unsigned)pair->bank,
                              (unsigned)pair->program);
                return BAE_BAD_FILE;
            }
        }
        if (BAERmfEditorBank_GetInstrumentInfo(bankToken,
                                                instrumentIndex,
                                                &sourceInfo) != BAE_NO_ERROR)
        {
            return BAE_BAD_FILE;
        }
        if (found)
        {
            resolvedInstID = sourceInfo.instID;
        }
        if (sourceInfo.instID != resolvedInstID)
        {
            return BAE_BAD_FILE;
        }

        firstSampleIndex = document->sampleCount;
        if (PV_ExportShouldEmbedInst(resolvedInstID, embedAll, embedResolvedInstIDs, embedCount))
        {
            result = BAERmfEditorDocument_CloneInstrumentFromBank(document,
                                                                   bankToken,
                                                                   instrumentIndex,
                                                                   (unsigned char)clonedPitchedCount);
        }
        else
        {
            result = BAERmfEditorDocument_AliasInstrumentFromBank(document,
                                                                   bankToken,
                                                                   instrumentIndex,
                                                                   (unsigned char)clonedPitchedCount);
            debug_message("[CloneUsed] alias (ghost) pitched INST=%u -> program=%u\n",
                          (unsigned)resolvedInstID,
                          (unsigned)clonedPitchedCount);
        }
        if (result != BAE_NO_ERROR)
        {
            return result;
        }
        result = PV_VerifyClonedInstrument(document,
                                           bankToken,
                                           instrumentIndex,
                                           512u + clonedPitchedCount,
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
                               512u + clonedPitchedCount,
                               clonedSampleCount,
                               sourceInfo.name);
        debug_message("[CloneUsed] source bank=%u program=%u INST=%u instIndex=%u -> target INST=%u program=%u\n",
                      (unsigned)pair->bank,
                      (unsigned)pair->program,
                      (unsigned)expectedInstID,
                      (unsigned)instrumentIndex,
                      (unsigned)(512u + clonedPitchedCount),
                      (unsigned)clonedPitchedCount);
        PV_RemapPitchedNoteReferences(document,
                                      pair->bank,
                                      pair->program,
                                      kClonedInstrumentBank,
                                      (unsigned char)clonedPitchedCount);
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
        uint32_t requestedInstID;
        uint32_t instrumentIndex;
        uint32_t resolvedInstID;
        uint32_t firstSampleIndex;
        uint32_t clonedSampleCount;
        uint32_t clonedIndex;
        unsigned char targetProgram;
        BAERmfEditorBankInstrumentInfo sourceInfo;

        requestedInstID = ((uint32_t)PV_BankGroupFromInternalBank(percussion->bank) * 256u) + 128u + note;
        if (BAERmfEditorBank_ResolveInstID(bankToken,
                                           requestedInstID,
                                           &resolvedInstID,
                                           &instrumentIndex) != BAE_NO_ERROR)
        {
            debug_message("[CloneUsed] Missing percussion INST=%u for note=%u\n",
                          (unsigned)requestedInstID,
                          (unsigned)note);
            return BAE_BAD_FILE;
        }
        if (BAERmfEditorBank_GetInstrumentInfo(bankToken,
                                                instrumentIndex,
                                                &sourceInfo) != BAE_NO_ERROR ||
            sourceInfo.instID != resolvedInstID)
        {
            return BAE_BAD_FILE;
        }
        for (clonedIndex = 0; clonedIndex < clonedPercussionCount; ++clonedIndex)
        {
            if (clonedPercussionInstIDs[clonedIndex] == resolvedInstID)
            {
                break;
            }
        }
        if (clonedIndex < clonedPercussionCount)
        {
            targetProgram = clonedPercussionPrograms[clonedIndex];
            clonedSampleCount = clonedPercussionSampleCounts[clonedIndex];
        }
        else
        {
            if (clonedPitchedCount + clonedPercussionCount >= 128)
            {
                return BAE_PARAM_ERR;
            }
            targetProgram = (unsigned char)(clonedPitchedCount + clonedPercussionCount);
            firstSampleIndex = document->sampleCount;
            if (PV_ExportShouldEmbedInst(resolvedInstID, embedAll, embedResolvedInstIDs, embedCount))
            {
                result = BAERmfEditorDocument_CloneInstrumentFromBankToInstID(document,
                                                                              bankToken,
                                                                              instrumentIndex,
                                                                              512u + targetProgram,
                                                                              targetProgram);
            }
            else
            {
                /* Alias path always lands at INST 512+program. */
                result = BAERmfEditorDocument_AliasInstrumentFromBank(document,
                                                                       bankToken,
                                                                       instrumentIndex,
                                                                       targetProgram);
                debug_message("[CloneUsed] alias (ghost) percussion INST=%u -> program=%u\n",
                              (unsigned)resolvedInstID,
                              (unsigned)targetProgram);
            }
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
            result = PV_VerifyClonedInstrument(document,
                                               bankToken,
                                               instrumentIndex,
                                               512u + targetProgram,
                                               firstSampleIndex,
                                               &clonedSampleCount);
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
            clonedPercussionInstIDs[clonedPercussionCount] = resolvedInstID;
            clonedPercussionPrograms[clonedPercussionCount] = targetProgram;
            clonedPercussionSampleCounts[clonedPercussionCount] = clonedSampleCount;
            clonedPercussionCount++;
        }
        PV_RemapPercussionNoteReferences(document,
                                          percussion->bank,
                                          (unsigned char)note,
                                          kClonedInstrumentBank,
                                          targetProgram);
        PV_AddCloneUsedMapping(outResult,
                               percussion->bank,
                               (unsigned char)note,
                               TRUE,
                               requestedInstID,
                               resolvedInstID,
                               512u + targetProgram,
                               clonedSampleCount,
                               sourceInfo.name);
    }

    if (outResult)
    {
        outResult->percussionCount = clonedPercussionCount;
    }
    PV_SynchronizeTrackInstrumentDefaults(document);
    return PV_VerifyEmbeddedOnlyInstrumentReferences(document, kClonedInstrumentBank);
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

static BAEResult PV_AddRequiredAliases(BAERmfEditorDocument *document, XFILE fileRef, bool isZmf)
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
            if (program > maxProgram)
            {
                maxProgram = program;
            }
        }
    }

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
                    percussionUsedNotes[note->note] = 1;
                    percussionTargetPrograms[note->note] = note->program;
                    ++percussionNoteCount;
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

            if (!percussionUsedNotes[percussionNoteIndex])
            {
                continue;
            }

            offset = (uint32_t)(8 + (w * 8));
            XPutLong(&((unsigned char *)aliasBlob)[offset],
                     ((uint32_t)(2u * 2u + 1u) * 128u) + percussionNoteIndex);
            XPutLong(&((unsigned char *)aliasBlob)[offset + 4],
                     512u + (uint32_t)percussionTargetPrograms[percussionNoteIndex]);
            ++w;
        }
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

static BAEResult PV_WriteRmfDocumentToResourceFile(BAERmfEditorDocument *document,
                                                   XFILE fileRef,
                                                   int32_t resourceID,
                                                   bool preserveOriginalMidi)
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
        if (XCleanResourceFile(fileRef) == FALSE)
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
        if (XCleanResourceFile(fileRef) == FALSE)
        {
            debug_message("[RMF Save] XCleanResourceFile failed\n");
            result = BAE_FILE_IO_ERROR;
        }
    }
    debug_message("[RMF Save] Final result=%d\n", (int)result);
    PV_ByteBufferDispose(&midiData);
    return result;
}

BAEResult BAERmfEditorDocument_SaveAsRmfToMemory(BAERmfEditorDocument *document,
                                                 bool useZmfContainer,
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

    result = PV_WriteRmfDocumentToResourceFile(document, fileRef, resourceID, FALSE);
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
    return BAE_NO_ERROR;
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

/* Core encode logic shared by bank re-encode and in-memory compression preview.
 * waveData is borrowed - caller owns it (may be modified in place by codecs).
 * When outPreviewPcm is non-NULL, encode+decode only (no bank I/O). */
static BAEResult PV_BankReEncodeSampleCore(XFILE bankFile,
                                            BAERmfEditorBankSampleInfo *pSampleInfo,
                                            void *waveData,
                                            uint32_t frameCount,
                                            uint16_t bitSize,
                                            uint16_t channels,
                                            BAE_UNSIGNED_FIXED sampleRate,
                                            BAERmfEditorCompressionType compressionType,
                                            BAERmfEditorSndStorageType sndStorageType,
                                            BAERmfEditorOpusMode opusMode,
                                            bool opusRoundTripResample,
                                            void **outPreviewPcm,
                                            uint32_t *outPreviewFrames,
                                            uint16_t *outPreviewBitSize,
                                            uint16_t *outPreviewChannels,
                                            BAE_UNSIGNED_FIXED *outPreviewRate,
                                            uint32_t *outEncodedBytes)
{
    BAEResult result;
    SndCompressionType compType;
    SndCompressionSubType compSubType;
    SndCompressionSubType encodeCompSubType;
    GM_Waveform writeWaveform;
    XResourceType oldSndType;
    XResourceType newSndType;
    XPTR oldSndRawData;
    int32_t oldSndRawSize;
    XPTR oldSndPlain;
    int32_t oldSndPlainSize;
    bool oldSndPlainOwned;
    bool needInspectOldSndHeader;
    bool previewOnly;
    SampleDataInfo oldSndInfo;
    int16_t preservedBaseKey;
    char sndName[256];
    XPTR sndResource;
    XPTR encodeWaveDataOwner;
    XPTR wrappedSnd;
    int32_t wrappedSndSize;
    OPErr opErr;
    bool sampleWasEncodedMpeg;
    int32_t normalizedLoopStart;
    int32_t normalizedLoopEnd;
    uint32_t inputPcmRateHz;
    uint32_t decodedSampleRateForSnd;
    uint32_t decodedFramesForRate;
    int32_t roundTripSourceRate;
    int32_t loopStart;
    int32_t loopEnd;

    previewOnly = (outPreviewPcm != NULL);
    if (previewOnly)
    {
        *outPreviewPcm = NULL;
        if (outPreviewFrames) { *outPreviewFrames = 0; }
        if (outPreviewBitSize) { *outPreviewBitSize = 0; }
        if (outPreviewChannels) { *outPreviewChannels = 0; }
        if (outPreviewRate) { *outPreviewRate = 0; }
        if (outEncodedBytes) { *outEncodedBytes = 0; }
    }
    else if (!bankFile)
    {
        return BAE_PARAM_ERR;
    }

    /* Map editor compression type to SND compression type / sub-type */
    sampleWasEncodedMpeg = FALSE;
    normalizedLoopStart = (int32_t)pSampleInfo->loopStart;
    normalizedLoopEnd = (int32_t)pSampleInfo->loopEnd;
    inputPcmRateHz = 0;
    decodedSampleRateForSnd = 0;
    decodedFramesForRate = 0;
    roundTripSourceRate = 0;
    loopStart = 0;
    loopEnd = 0;
    oldSndPlain = NULL;
    oldSndPlainSize = 0;
    oldSndPlainOwned = FALSE;
    needInspectOldSndHeader = FALSE;
    oldSndType = ID_SND;
    XSetMemory(&oldSndInfo, (int32_t)sizeof(oldSndInfo), 0);
    preservedBaseKey = (int16_t)pSampleInfo->rootKey;
    compType = C_NONE;
    compSubType = CS_DEFAULT;
    switch (compressionType)
    {
        case BAE_EDITOR_COMPRESSION_ADPCM:
            compType = C_IMA4;
            compSubType = CS_DEFAULT;
            break;
        case BAE_EDITOR_COMPRESSION_ALAW:
            compType = C_ALAW;
            compSubType = CS_DEFAULT;
            break;
        case BAE_EDITOR_COMPRESSION_ULAW:
            compType = C_ULAW;
            compSubType = CS_DEFAULT;
            break;
        case BAE_EDITOR_COMPRESSION_MP3_32K:
            compType = C_MPEG_32;  compSubType = CS_MPEG2; sampleWasEncodedMpeg = TRUE; break;
        case BAE_EDITOR_COMPRESSION_MP3_48K:
            compType = C_MPEG_48;  compSubType = CS_MPEG2; sampleWasEncodedMpeg = TRUE; break;
        case BAE_EDITOR_COMPRESSION_MP3_64K:
            compType = C_MPEG_64;  compSubType = CS_MPEG2; sampleWasEncodedMpeg = TRUE; break;
        case BAE_EDITOR_COMPRESSION_MP3_96K:
            compType = C_MPEG_96;  compSubType = CS_MPEG2; sampleWasEncodedMpeg = TRUE; break;
        case BAE_EDITOR_COMPRESSION_MP3_128K:
            compType = C_MPEG_128; compSubType = CS_MPEG2; sampleWasEncodedMpeg = TRUE; break;
        case BAE_EDITOR_COMPRESSION_MP3_192K:
            compType = C_MPEG_192; compSubType = CS_MPEG2; sampleWasEncodedMpeg = TRUE; break;
        case BAE_EDITOR_COMPRESSION_MP3_256K:
            compType = C_MPEG_256; compSubType = CS_MPEG2; sampleWasEncodedMpeg = TRUE; break;
        case BAE_EDITOR_COMPRESSION_MP3_320K:
            compType = C_MPEG_320; compSubType = CS_MPEG2; sampleWasEncodedMpeg = TRUE; break;
#if USE_VORBIS_ENCODER == TRUE && USE_VORBIS_DECODER == TRUE
        case BAE_EDITOR_COMPRESSION_VORBIS_32K:
            compType = C_VORBIS; compSubType = CS_VORBIS_32K; break;
        case BAE_EDITOR_COMPRESSION_VORBIS_48K:
            compType = C_VORBIS; compSubType = CS_VORBIS_48K; break;
        case BAE_EDITOR_COMPRESSION_VORBIS_64K:
            compType = C_VORBIS; compSubType = CS_VORBIS_64K; break;
        case BAE_EDITOR_COMPRESSION_VORBIS_80K:
            compType = C_VORBIS; compSubType = CS_VORBIS_80K; break;
        case BAE_EDITOR_COMPRESSION_VORBIS_96K:
            compType = C_VORBIS; compSubType = CS_VORBIS_96K; break;
        case BAE_EDITOR_COMPRESSION_VORBIS_128K:
            compType = C_VORBIS; compSubType = CS_VORBIS_128K; break;
        case BAE_EDITOR_COMPRESSION_VORBIS_160K:
            compType = C_VORBIS; compSubType = CS_VORBIS_160K; break;
        case BAE_EDITOR_COMPRESSION_VORBIS_192K:
            compType = C_VORBIS; compSubType = CS_VORBIS_192K; break;
        case BAE_EDITOR_COMPRESSION_VORBIS_256K:
            compType = C_VORBIS; compSubType = CS_VORBIS_256K; break;
#endif
#if USE_FLAC_ENCODER == TRUE && USE_FLAC_DECODER == TRUE
        case BAE_EDITOR_COMPRESSION_FLAC:
            compType = C_FLAC; compSubType = CS_DEFAULT; break;
#endif
#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
        case BAE_EDITOR_COMPRESSION_OPUS_12K:
            compType = C_OPUS; compSubType = CS_OPUS_12K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_16K:
            compType = C_OPUS; compSubType = CS_OPUS_16K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_24K:
            compType = C_OPUS; compSubType = CS_OPUS_24K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_32K:
            compType = C_OPUS; compSubType = CS_OPUS_32K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_48K:
            compType = C_OPUS; compSubType = CS_OPUS_48K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_64K:
            compType = C_OPUS; compSubType = CS_OPUS_64K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_80K:
            compType = C_OPUS; compSubType = CS_OPUS_80K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_96K:
            compType = C_OPUS; compSubType = CS_OPUS_96K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_128K:
            compType = C_OPUS; compSubType = CS_OPUS_128K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_160K:
            compType = C_OPUS; compSubType = CS_OPUS_160K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_192K:
            compType = C_OPUS; compSubType = CS_OPUS_192K; break;
        case BAE_EDITOR_COMPRESSION_OPUS_256K:
            compType = C_OPUS; compSubType = CS_OPUS_256K; break;
#endif
#if USE_QOA_SUPPORT == TRUE
        case BAE_EDITOR_COMPRESSION_QOA:
            compType = C_QOA; compSubType = CS_DEFAULT; break;
#endif
        case BAE_EDITOR_COMPRESSION_PCM:
        default:
            compType = C_NONE;
            compSubType = CS_DEFAULT;
            break;
    }

    /* Find and inspect the existing SND resource so we can preserve
     * source playback metadata (notably base key) across bank re-encodes.
     * Preview path skips all bank I/O and uses the caller's rootKey. */
    sndName[0] = 0;
    oldSndRawData = NULL;
    oldSndRawSize = 0;
    if (!previewOnly)
    {
        result = PV_BankFindSndResource(bankFile, pSampleInfo->sndResourceID,
                                        &oldSndType, &oldSndRawData, &oldSndRawSize, sndName);
        if (result != BAE_NO_ERROR)
        {
            return result;
        }

        if (preservedBaseKey < 0 || preservedBaseKey > 127)
        {
            needInspectOldSndHeader = TRUE;
        }

        if (needInspectOldSndHeader)
        {
            oldSndPlain = oldSndRawData;
            oldSndPlainSize = oldSndRawSize;
            if (oldSndType == ID_CSND)
            {
                oldSndPlain = XDecompressPtr(oldSndRawData, (uint32_t)oldSndRawSize, FALSE);
                oldSndPlainSize = oldSndPlain ? XGetPtrSize(oldSndPlain) : 0;
                oldSndPlainOwned = TRUE;
            }
            else if (oldSndType == ID_ESND)
            {
                XDecryptData(oldSndPlain, (uint32_t)oldSndPlainSize);
            }

            if (oldSndPlain && oldSndPlainSize > 0)
            {
                if (XGetSampleInfoFromSnd(oldSndPlain, &oldSndInfo) == 0 &&
                    oldSndInfo.baseKey >= 0 && oldSndInfo.baseKey <= 127)
                {
                    preservedBaseKey = oldSndInfo.baseKey;
                }
            }
        }
    }
    else if (preservedBaseKey < 0 || preservedBaseKey > 127)
    {
        preservedBaseKey = 60;
    }

    /* Build the source waveform descriptor */
    XSetMemory(&writeWaveform, (int32_t)sizeof(writeWaveform), 0);
    writeWaveform.theWaveform   = (signed char *)waveData;
    writeWaveform.waveFrames    = frameCount;
    writeWaveform.waveSize      = (int32_t)(frameCount * (uint32_t)(bitSize / 8u) * (uint32_t)channels);
    writeWaveform.bitSize       = bitSize;
    writeWaveform.channels      = channels;
    writeWaveform.sampledRate   = (int32_t)sampleRate;
    writeWaveform.baseMidiPitch = (uint16_t)preservedBaseKey;
    writeWaveform.compressionType = C_NONE;

    /* Bank sample metadata loop points are stored in sample-rate domain.
     * For non-RT Opus re-encode, incoming PCM may be decoded at 48kHz while
     * metadata/sampleRate remains at playback/header rate; remap loops to the
     * actual PCM frame domain before validation. */
#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
    if (compType == C_OPUS && !opusRoundTripResample)
    {
        if ((uint32_t)sampleRate >= (1000u << 16))
        {
            inputPcmRateHz = (uint32_t)sampleRate >> 16;
        }
        else if ((uint32_t)sampleRate >= 1000u && (uint32_t)sampleRate <= 384000u)
        {
            inputPcmRateHz = (uint32_t)sampleRate;
        }

        if (inputPcmRateHz > 0 && pSampleInfo->sampleRate > 0 && inputPcmRateHz != pSampleInfo->sampleRate)
        {
            uint32_t mappedStart;
            uint32_t mappedEnd;

            mappedStart = (uint32_t)(((uint64_t)(uint32_t)normalizedLoopStart * (uint64_t)inputPcmRateHz) /
                                     (uint64_t)pSampleInfo->sampleRate);
            mappedEnd = (uint32_t)((((uint64_t)(uint32_t)normalizedLoopEnd * (uint64_t)inputPcmRateHz) +
                                    ((uint64_t)pSampleInfo->sampleRate - 1ULL)) /
                                   (uint64_t)pSampleInfo->sampleRate);

            if (mappedStart > frameCount)
            {
                mappedStart = frameCount;
            }
            if (mappedEnd > frameCount)
            {
                mappedEnd = frameCount;
            }

            normalizedLoopStart = (int32_t)mappedStart;
            normalizedLoopEnd = (int32_t)mappedEnd;
        }
    }
#endif

    /* Validate/clamp loop points */
    if (normalizedLoopStart >= 0 &&
        normalizedLoopEnd > normalizedLoopStart &&
        (uint32_t)normalizedLoopEnd <= frameCount)
    {
        writeWaveform.startLoop = (uint32_t)normalizedLoopStart;
        writeWaveform.endLoop   = (uint32_t)normalizedLoopEnd;
    }

    /* Normalize sample rate to fixed-point */
    if ((uint32_t)writeWaveform.sampledRate == 0u)
    {
        writeWaveform.sampledRate = PV_IsOpusCompression(compressionType)
                                    ? (int32_t)(48000u << 16)
                                    : (int32_t)(44100u << 16);
    }
    else if ((uint32_t)writeWaveform.sampledRate < (1000u << 16))
    {
        if ((uint32_t)writeWaveform.sampledRate >= 1000u &&
            (uint32_t)writeWaveform.sampledRate <= 384000u)
        {
            writeWaveform.sampledRate <<= 16;
        }
        else
        {
            writeWaveform.sampledRate = PV_IsOpusCompression(compressionType)
                                        ? (int32_t)(48000u << 16)
                                        : (int32_t)(44100u << 16);
        }
    }

    encodeWaveDataOwner = NULL;

    /* MPEG: resample to a codec-compatible rate if needed */
    if (sampleWasEncodedMpeg)
    {
        BAE_UNSIGNED_FIXED sourceRate;
        BAE_UNSIGNED_FIXED targetRate;

        sourceRate = PV_NormalizeSampleRateForSave((BAE_UNSIGNED_FIXED)writeWaveform.sampledRate);
        targetRate = PV_ChooseCodecRateFromSourceHz(compressionType, (uint32_t)(sourceRate >> 16));
        if (targetRate == 0)
        {
            targetRate = (44100u << 16);
        }
        if (sourceRate != targetRate)
        {
            result = PV_ResampleWaveformLinear(&writeWaveform, targetRate, &encodeWaveDataOwner);
            if (result != BAE_NO_ERROR)
            {
                return result;
            }
        }
    }

#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
    /* Opus: resample to optimal rate for the chosen bitrate tier */
    if (compType == C_OPUS)
    {
        BAE_UNSIGNED_FIXED sourceRate;
        BAE_UNSIGNED_FIXED targetRate;

        sourceRate = PV_NormalizeSampleRateForSave((BAE_UNSIGNED_FIXED)writeWaveform.sampledRate);
        targetRate = sourceRate;
        if (!opusRoundTripResample)
        {
            targetRate = PV_ChooseCodecRateFromSourceHz(compressionType, (uint32_t)(sourceRate >> 16));
            if (targetRate == 0)
            {
                targetRate = (48000u << 16);
            }
        }
        if (sourceRate != targetRate)
        {
            result = PV_ResampleWaveformLinear(&writeWaveform, targetRate, &encodeWaveDataOwner);
            if (result != BAE_NO_ERROR)
            {
                if (encodeWaveDataOwner) { XDisposePtr(encodeWaveDataOwner); }
                return result;
            }
        }
        encodeCompSubType = PV_ComposeOpusEncodeSubType(compSubType, opusMode);
        if (opusRoundTripResample)
        {
            roundTripSourceRate = writeWaveform.sampledRate;
            writeWaveform.sampledRate = (int32_t)(48000u << 16);
        }
    }
    else
#endif
    {
        encodeCompSubType = compSubType;
    }

    /* Encode */
    sndResource = NULL;
    opErr = XCreateSoundObjectFromData(&sndResource, &writeWaveform, compType, encodeCompSubType, NULL, NULL);

    if (encodeWaveDataOwner)
    {
        XDisposePtr(encodeWaveDataOwner);
        encodeWaveDataOwner = NULL;
    }

    if (opErr != NO_ERR || !sndResource)
    {
        return BAE_BAD_FILE;
    }

    /* Store the compression sub-type in the SND header */
    PV_StoreCompressionSubTypeInSnd(sndResource, XGetPtrSize(sndResource), compType, compSubType);

    /* Update sample rate from the decoded stream (important for MPEG) */
    {
        int32_t sndSampleRate = writeWaveform.sampledRate;
        SampleDataInfo decodedInfo;
        XPTR decodedOwner;

        XSetMemory(&decodedInfo, (int32_t)sizeof(decodedInfo), 0);
        decodedOwner = NULL;
        (void)XGetSamplePtrFromSnd(sndResource, &decodedInfo);
        if (sampleWasEncodedMpeg && decodedInfo.rate != 0)
        {
            sndSampleRate = (int32_t)decodedInfo.rate;
            decodedSampleRateForSnd = (uint32_t)decodedInfo.rate;
        }

        decodedFramesForRate = decodedInfo.frames ? decodedInfo.frames : writeWaveform.waveFrames;
        loopStart = (int32_t)writeWaveform.startLoop;
        loopEnd   = (int32_t)writeWaveform.endLoop;

#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
        if (compType == C_OPUS)
        {
            if (opusRoundTripResample)
            {
                /* Match document save path: preserve source frame domain in RT mode,
                 * but clamp loop end to the actual encoded decode capacity. */
                PV_ForceSndDecodedFrameCount(sndResource, writeWaveform.waveFrames);
                decodedFramesForRate = writeWaveform.waveFrames;
                if (decodedInfo.frames > 0 && loopEnd > (int32_t)decodedInfo.frames)
                {
                    int32_t delta;

                    delta = loopEnd - (int32_t)decodedInfo.frames;
                    loopEnd = (int32_t)decodedInfo.frames;
                    if (delta > 0)
                    {
                        if (loopStart > delta)
                        {
                            loopStart -= delta;
                        }
                        else
                        {
                            loopStart = 0;
                        }
                    }
                    if (loopStart >= loopEnd)
                    {
                        loopStart = 0;
                        loopEnd = 0;
                    }
                }
            }
            else
            {
                /* Match document save non-RT path: keep decoder-probed frame domain
                 * and remap loops from writeWaveform domain into final decoded domain. */
                decodedFramesForRate = decodedInfo.frames ? decodedInfo.frames : writeWaveform.waveFrames;
                PV_ForceSndDecodedFrameCount(sndResource, decodedFramesForRate);
                PV_RemapLoopPointsToFrameCount(writeWaveform.waveFrames,
                                               decodedFramesForRate,
                                               &loopStart,
                                               &loopEnd);
            }
        }
        else
#endif
        {
            PV_ForceSndDecodedFrameCount(sndResource, decodedFramesForRate);
            if (compType != C_IMA4)
            {
                PV_RemapLoopPointsToFrameCount(writeWaveform.waveFrames,
                                               decodedFramesForRate,
                                               &loopStart,
                                               &loopEnd);
            }
        }

        writeWaveform.startLoop = (uint32_t)loopStart;
        writeWaveform.endLoop   = (uint32_t)loopEnd;

#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
        if (roundTripSourceRate != 0)
        {
            sndSampleRate = roundTripSourceRate;
        }
#endif

        XSetSoundBaseKey(sndResource, preservedBaseKey);
        XSetSoundSampleRate(sndResource, sndSampleRate);
        XSetSoundLoopPoints(sndResource,
                            (int32_t)writeWaveform.startLoop,
                            (int32_t)writeWaveform.endLoop);
        PV_ForceSndLoopPoints(sndResource,
                              (int32_t)writeWaveform.startLoop,
                              (int32_t)writeWaveform.endLoop);
#if USE_OPUS_ENCODER == TRUE || USE_OPUS_DECODER == TRUE
        if (roundTripSourceRate != 0)
        {
            XSetSoundOpusRoundTripFlag(sndResource, TRUE);
        }
#endif
        XSetSoundEmbeddedStatus(sndResource, TRUE);

        if (decodedInfo.pMasterPtr && decodedInfo.pMasterPtr != sndResource)
        {
            decodedOwner = decodedInfo.pMasterPtr;
        }
        if (decodedOwner)
        {
            XDisposePtr(decodedOwner);
        }
    }
    (void)decodedSampleRateForSnd; /* unused after rate-update block */

    if (oldSndPlainOwned && oldSndPlain)
    {
        XDisposePtr(oldSndPlain);
    }
    if (oldSndRawData)
    {
        XDisposePtr(oldSndRawData);
        oldSndRawData = NULL;
    }

    /* In-memory compression preview: decode the freshly encoded SND and return
     * PCM without touching the bank (avoids full-session SND rebuilds). */
    if (previewOnly)
    {
        SampleDataInfo previewInfo;
        XPTR previewPcm;
        XPTR previewOwner;
        XPTR ownedPcm;
        int32_t pcmSize;

        if (outEncodedBytes)
        {
            *outEncodedBytes = (uint32_t)XGetPtrSize(sndResource);
        }

        XSetMemory(&previewInfo, (int32_t)sizeof(previewInfo), 0);
        previewPcm = XGetSamplePtrFromSnd(sndResource, &previewInfo);
        previewOwner = NULL;
        if (previewInfo.pMasterPtr && previewInfo.pMasterPtr != sndResource)
        {
            previewOwner = previewInfo.pMasterPtr;
        }
        if (!previewPcm || previewInfo.bitSize == 0 || previewInfo.channels == 0 ||
            previewInfo.frames == 0)
        {
            if (previewOwner)
            {
                XDisposePtr(previewOwner);
            }
            XDisposePtr(sndResource);
            return BAE_BAD_FILE;
        }

        pcmSize = (int32_t)(previewInfo.frames *
                            (previewInfo.bitSize / 8) *
                            previewInfo.channels);
        if (pcmSize <= 0)
        {
            if (previewOwner)
            {
                XDisposePtr(previewOwner);
            }
            XDisposePtr(sndResource);
            return BAE_BAD_FILE;
        }

        ownedPcm = XNewPtr(pcmSize);
        if (!ownedPcm)
        {
            if (previewOwner)
            {
                XDisposePtr(previewOwner);
            }
            XDisposePtr(sndResource);
            return BAE_MEMORY_ERR;
        }
        XBlockMove(previewPcm, ownedPcm, pcmSize);
        if (previewOwner)
        {
            XDisposePtr(previewOwner);
        }
        XDisposePtr(sndResource);

        *outPreviewPcm = ownedPcm;
        if (outPreviewFrames) { *outPreviewFrames = previewInfo.frames; }
        if (outPreviewBitSize) { *outPreviewBitSize = previewInfo.bitSize; }
        if (outPreviewChannels) { *outPreviewChannels = previewInfo.channels; }
        if (outPreviewRate) { *outPreviewRate = previewInfo.rate; }
        (void)sndStorageType;
        return BAE_NO_ERROR;
    }

    /* Map sndStorageType to XResourceType */
    switch (sndStorageType)
    {
        case BAE_EDITOR_SND_STORAGE_CSND: newSndType = ID_CSND; break;
        case BAE_EDITOR_SND_STORAGE_SND:  newSndType = ID_SND;  break;
        case BAE_EDITOR_SND_STORAGE_ESND:
        default:                          newSndType = ID_ESND; break;
    }

    /* Wrap the encoded SND in the target container */
    wrappedSnd = NULL;
    wrappedSndSize = 0;
    result = PV_BankRewrapSndForType(bankFile, newSndType, sndResource,
                                     XGetPtrSize(sndResource), &wrappedSnd, &wrappedSndSize);
    XDisposePtr(sndResource);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    /* Replace the SND resource in the bank using the fast SND-only path
       (handles container type change if oldSndType != newSndType) */
    result = PV_BankReplaceSndResourceInPlace(bankFile, oldSndType, newSndType,
                                              (XShortResourceID)pSampleInfo->sndResourceID,
                                              sndName, wrappedSnd, wrappedSndSize);
    XDisposePtr(wrappedSnd);
    return result;
}

BAEResult BAERmfEditorBank_ReEncodeSample(BAEBankToken bankToken,
                                           uint32_t instrumentIndex,
                                           uint32_t sampleIndex,
                                           BAERmfEditorCompressionType compressionType,
                                           BAERmfEditorSndStorageType sndStorageType,
                                           BAERmfEditorOpusMode opusMode)
{
    XFILE bankFile;
    BAERmfEditorBankSampleInfo sampleInfo;
    void *waveData;
    uint32_t frameCount;
    uint16_t bitSize;
    uint16_t channels;
    BAE_UNSIGNED_FIXED sampleRate;
    BAEResult result;

    if (!bankToken)
    {
        return BAE_PARAM_ERR;
    }
    if (compressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE)
    {
        return BAE_NO_ERROR;
    }

    bankFile = (XFILE)bankToken;

    /* Decode the sample to PCM */
    waveData = NULL;
    frameCount = 0;
    bitSize = 0;
    channels = 0;
    sampleRate = 0;
    result = BAERmfEditorBank_GetSampleWaveformData(bankToken, instrumentIndex, sampleIndex,
                                                    &waveData, &frameCount, &bitSize, &channels, &sampleRate);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }
    if (!waveData || frameCount == 0 || bitSize == 0 || channels == 0)
    {
        if (waveData) { XDisposePtr(waveData); }
        return BAE_BAD_FILE;
    }

    /* Get sample metadata (loop points, root key, current container type) */
    result = BAERmfEditorBank_GetInstrumentSampleInfo(bankToken, instrumentIndex, sampleIndex, &sampleInfo);
    if (result != BAE_NO_ERROR)
    {
        XDisposePtr(waveData);
        return result;
    }

    result = PV_BankReEncodeSampleCore(bankFile, &sampleInfo,
                                       waveData, frameCount, bitSize, channels, sampleRate,
                                       compressionType, sndStorageType, opusMode, FALSE,
                                       NULL, NULL, NULL, NULL, NULL, NULL);
    XDisposePtr(waveData);
    return result;
}

BAEResult BAERmfEditorBank_ReEncodeSampleFromPCM(BAEBankToken bankToken,
                                                  uint32_t instrumentIndex,
                                                  uint32_t sampleIndex,
                                                  BAERmfEditorCompressionType compressionType,
                                                  BAERmfEditorSndStorageType sndStorageType,
                                                  BAERmfEditorOpusMode opusMode,
                                                  const void *sourcePcm,
                                                  uint32_t frameCount,
                                                  uint16_t bitSize,
                                                  uint16_t channels,
                                                  BAE_UNSIGNED_FIXED sampleRate)
{
        return BAERmfEditorBank_ReEncodeSampleFromPCMEx(bankToken,
                                                                                                         instrumentIndex,
                                                                                                         sampleIndex,
                                                                                                         compressionType,
                                                                                                         sndStorageType,
                                                                                                         opusMode,
                                                                                                         FALSE,
                                                                                                         sourcePcm,
                                                                                                         frameCount,
                                                                                                         bitSize,
                                                                                                         channels,
                                                                                                         sampleRate);
}

BAEResult BAERmfEditorBank_ReEncodeSampleFromPCMEx(BAEBankToken bankToken,
                                                                                                        uint32_t instrumentIndex,
                                                                                                        uint32_t sampleIndex,
                                                                                                        BAERmfEditorCompressionType compressionType,
                                                                                                        BAERmfEditorSndStorageType sndStorageType,
                                                                                                        BAERmfEditorOpusMode opusMode,
                                                                                                        bool opusRoundTripResample,
                                                                                                        const void *sourcePcm,
                                                                                                        uint32_t frameCount,
                                                                                                        uint16_t bitSize,
                                                                                                        uint16_t channels,
                                                                                                        BAE_UNSIGNED_FIXED sampleRate)
{
    XFILE bankFile;
    BAERmfEditorBankSampleInfo sampleInfo;
    BAEResult result;
    void *pcmCopy;
    int32_t pcmBytes;

    if (!bankToken || !sourcePcm || frameCount == 0 || bitSize == 0 || channels == 0)
    {
        return BAE_PARAM_ERR;
    }
    if (compressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE)
    {
        return BAE_NO_ERROR;
    }

    bankFile = (XFILE)bankToken;

    result = BAERmfEditorBank_GetInstrumentSampleInfo(bankToken, instrumentIndex, sampleIndex, &sampleInfo);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    /* Make a writable copy of the caller's PCM so PV_BankReEncodeSampleCore can pass it to
       XCreateSoundObjectFromData (which may modify the buffer in place for some codecs). */
    pcmBytes = (int32_t)(frameCount * (uint32_t)(bitSize / 8u) * (uint32_t)channels);
    pcmCopy = XNewPtr(pcmBytes);
    if (!pcmCopy)
    {
        return BAE_MEMORY_ERR;
    }
    XBlockMove((XPTR)(uintptr_t)sourcePcm, pcmCopy, pcmBytes);

    result = PV_BankReEncodeSampleCore(bankFile, &sampleInfo,
                                       pcmCopy, frameCount, bitSize, channels, sampleRate,
                                       compressionType, sndStorageType, opusMode,
                                       opusRoundTripResample,
                                       NULL, NULL, NULL, NULL, NULL, NULL);
    XDisposePtr(pcmCopy);
    return result;
}

BAEResult BAERmfEditorBank_ReEncodeSampleFromMutablePCMEx(BAEBankToken bankToken,
                                                           uint32_t instrumentIndex,
                                                           uint32_t sampleIndex,
                                                           BAERmfEditorCompressionType compressionType,
                                                           BAERmfEditorSndStorageType sndStorageType,
                                                           BAERmfEditorOpusMode opusMode,
                                                           bool opusRoundTripResample,
                                                           void *mutablePcm,
                                                           uint32_t frameCount,
                                                           uint16_t bitSize,
                                                           uint16_t channels,
                                                           BAE_UNSIGNED_FIXED sampleRate)
{
    XFILE bankFile;
    BAERmfEditorBankSampleInfo sampleInfo;
    BAEResult result;

    if (!bankToken || !mutablePcm || frameCount == 0 || bitSize == 0 || channels == 0)
    {
        return BAE_PARAM_ERR;
    }
    if (compressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE)
    {
        return BAE_NO_ERROR;
    }

    bankFile = (XFILE)bankToken;

    result = BAERmfEditorBank_GetInstrumentSampleInfo(bankToken, instrumentIndex, sampleIndex, &sampleInfo);
    if (result != BAE_NO_ERROR)
    {
        return result;
    }

    return PV_BankReEncodeSampleCore(bankFile, &sampleInfo,
                                     mutablePcm, frameCount, bitSize, channels, sampleRate,
                                     compressionType, sndStorageType, opusMode,
                                     opusRoundTripResample,
                                     NULL, NULL, NULL, NULL, NULL, NULL);
}

/* In-memory encode→decode for sample-editor compression preview.
 * Does not modify any bank. Free outWaveData with BAERmfEditorBank_FreeWaveformData. */
BAEResult BAERmfEditor_PreviewCompressPCM(const void *sourcePcm,
                                          uint32_t frameCount,
                                          uint16_t bitSize,
                                          uint16_t channels,
                                          BAE_UNSIGNED_FIXED sampleRate,
                                          uint32_t loopStart,
                                          uint32_t loopEnd,
                                          int16_t rootKey,
                                          uint32_t metadataSampleRateHz,
                                          BAERmfEditorCompressionType compressionType,
                                          BAERmfEditorOpusMode opusMode,
                                          bool opusRoundTripResample,
                                          void **outWaveData,
                                          uint32_t *outFrameCount,
                                          uint16_t *outBitSize,
                                          uint16_t *outChannels,
                                          BAE_UNSIGNED_FIXED *outSampleRate,
                                          uint32_t *outEncodedBytes)
{
    BAERmfEditorBankSampleInfo sampleInfo;
    void *pcmCopy;
    int32_t pcmBytes;
    BAEResult result;

    if (!sourcePcm || !outWaveData || frameCount == 0 || bitSize == 0 || channels == 0)
    {
        return BAE_PARAM_ERR;
    }
    if (compressionType == BAE_EDITOR_COMPRESSION_DONT_CHANGE ||
        compressionType == BAE_EDITOR_COMPRESSION_PCM)
    {
        return BAE_PARAM_ERR;
    }

    XSetMemory(&sampleInfo, (int32_t)sizeof(sampleInfo), 0);
    sampleInfo.loopStart = loopStart;
    sampleInfo.loopEnd = loopEnd;
    sampleInfo.rootKey = (unsigned char)((rootKey < 0) ? 0 : ((rootKey > 127) ? 127 : rootKey));
    if (metadataSampleRateHz > 0)
    {
        sampleInfo.sampleRate = metadataSampleRateHz;
    }
    else if ((uint32_t)sampleRate >= (1000u << 16))
    {
        sampleInfo.sampleRate = (uint32_t)sampleRate >> 16;
    }
    else
    {
        sampleInfo.sampleRate = (uint32_t)sampleRate;
    }

    pcmBytes = (int32_t)(frameCount * (uint32_t)(bitSize / 8u) * (uint32_t)channels);
    pcmCopy = XNewPtr(pcmBytes);
    if (!pcmCopy)
    {
        return BAE_MEMORY_ERR;
    }
    XBlockMove((XPTR)(uintptr_t)sourcePcm, pcmCopy, pcmBytes);

    result = PV_BankReEncodeSampleCore(NULL, &sampleInfo,
                                       pcmCopy, frameCount, bitSize, channels, sampleRate,
                                       compressionType, BAE_EDITOR_SND_STORAGE_SND, opusMode,
                                       opusRoundTripResample,
                                       outWaveData, outFrameCount, outBitSize, outChannels,
                                       outSampleRate, outEncodedBytes);
    XDisposePtr(pcmCopy);
    return result;
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
