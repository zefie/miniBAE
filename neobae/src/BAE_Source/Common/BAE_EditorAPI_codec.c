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
**  BAE_EditorAPI_codec.c
**
**  Codec, import typing, resample/loop, ReEncode, PreviewCompress. Perf: neobae/docs/BAE_EditorAPI_PERF.md
*/
/*****************************************************************************/

#include "BAE_EditorAPI_Internal.h"


bool PV_PathHasExtensionIgnoreCase(char const *filePath, char const *ext)
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


BAEFileType PV_DetectOggCodecBySignature(BAEPathName filePath)
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


BAEFileType PV_DetermineEditorImportFileType(BAEPathName filePath)
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


BAEFileType PV_DetermineEditorImportMemoryFileType(void const *data,
                                                           uint32_t dataSize,
                                                           BAEFileType fileTypeHint)
{
    unsigned char const *bytes;

    if (!data || dataSize < 4)
    {
        return (fileTypeHint != BAE_INVALID_TYPE) ? fileTypeHint : BAE_INVALID_TYPE;
    }

    bytes = (unsigned char const *)data;
#if USE_MTHC_SUPPORT == TRUE
    /* Prefer container magic over a .mid-derived MIDI hint. */
    if (bytes[0] == 'M' && bytes[1] == 'T' && bytes[2] == 'h' && bytes[3] == 'c')
    {
        return BAE_MTHC;
    }
#endif
    if (fileTypeHint != BAE_INVALID_TYPE)
    {
        return fileTypeHint;
    }
    if (bytes[0] == 'M' && bytes[1] == 'T' && bytes[2] == 'h' && bytes[3] == 'd')
    {
        return BAE_MIDI_TYPE;
    }
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


uint32_t PV_GetStoredCompressionSubTypeFromSnd(XPTR sndData,
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


void PV_StoreCompressionSubTypeInSnd(XPTR sndData,
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


bool PV_IsValidEditorOpusMode(BAERmfEditorOpusMode opusMode)
{
    return (opusMode == BAE_EDITOR_OPUS_MODE_AUDIO ||
            opusMode == BAE_EDITOR_OPUS_MODE_VOICE) ? TRUE : FALSE;
}


/* Map CS_OPUS_*K FourCC -> small index that fits in 16 bits for transport via
 * SndCompressionSubType.  The CS_OPUS_*K constants are 32-bit FourCCs and
 * cannot be packed into 16 bits directly.  Both PV_ComposeOpusEncodeSubType
 * and the matching switch in SampleTools.c must use this same mapping. */
uint32_t PV_SubTypeToOpusBitrateIndex(SndCompressionSubType subType)
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


SndCompressionSubType PV_ComposeOpusEncodeSubType(SndCompressionSubType baseSubType,
                                                         BAERmfEditorOpusMode opusMode)
{
    uint32_t packed;

    /* Low 16 bits: bitrate index (0-11); high 16 bits: opus mode (0-2). */
    packed = PV_SubTypeToOpusBitrateIndex(baseSubType) & 0xFFFFU;
    packed |= ((uint32_t)(PV_IsValidEditorOpusMode(opusMode) ? opusMode : BAE_EDITOR_OPUS_MODE_AUDIO) & 0xFFFFU) << 16;
    return (SndCompressionSubType)packed;
}

uint32_t PV_AllocateSampleAssetID(BAERmfEditorDocument *document)
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


void PV_NoteSampleAssetID(BAERmfEditorDocument *document, uint32_t assetID)
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


/* Reserve every SND/ESND/CSND ID present in a bank file so newly allocated
 * song-local sampleAssetIDs cannot collide with bank samples. Without this,
 * CloneUsed remaps (e.g. ding → 1455) into IDs still used by the parent bank
 * (splash/ride/etc.); loading the ZMF together with that ZSB/ZSN then resolves
 * the wrong PCM via g_openResourceFiles search order. */
void PV_ReserveBankSoundResourceIDs(BAERmfEditorDocument *document, XFILE bankFile)
{
    static const XResourceType kSndTypes[] = { ID_ESND, ID_CSND, ID_SND };
    uint32_t typeIndex;

    if (!document || !bankFile)
    {
        return;
    }
    for (typeIndex = 0; typeIndex < (uint32_t)(sizeof(kSndTypes) / sizeof(kSndTypes[0])); ++typeIndex)
    {
        int32_t count;
        int32_t index;

        count = XCountFileResourcesOfType(bankFile, kSndTypes[typeIndex]);
        for (index = 0; index < count; ++index)
        {
            XLongResourceID sndID;
            int32_t sndSize;
            char rawName[256];
            XPTR sndData;

            rawName[0] = 0;
            sndData = XGetIndexedFileResource(bankFile,
                                              kSndTypes[typeIndex],
                                              &sndID,
                                              index,
                                              rawName,
                                              &sndSize);
            if (sndData)
            {
                XDisposePtr(sndData);
                if (sndID > 0 && sndID <= 32767)
                {
                    PV_NoteSampleAssetID(document, (uint32_t)sndID);
                }
            }
        }
    }
}


BAERmfEditorSample *PV_FindFirstSampleForAsset(BAERmfEditorDocument *document, uint32_t assetID)
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


uint32_t PV_CountSamplesForAsset(BAERmfEditorDocument const *document, uint32_t assetID)
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


bool PV_IsOpusCompression(BAERmfEditorCompressionType ct)
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


bool PV_AssetSupportsDontChange(BAERmfEditorDocument const *document, uint32_t assetID)
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


BAE_UNSIGNED_FIXED PV_NormalizeSampleRateForSave(BAE_UNSIGNED_FIXED sampleRate)
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


bool PV_IsMpegCompression(BAERmfEditorCompressionType ct)
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


uint32_t PV_ExtractOpusInputRateFromOriginalSnd(BAERmfEditorSample const *sample)
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


uint32_t PV_SampleRateFixedToHz(BAE_UNSIGNED_FIXED fixedRate)
{
    fixedRate = PV_NormalizeSampleRateForSave(fixedRate);
    return (uint32_t)(fixedRate >> 16);
}


uint32_t PV_ChooseUpscaledRateFromTable(uint32_t sourceHz,
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


BAE_UNSIGNED_FIXED PV_ChooseCodecRateFromSourceHz(BAERmfEditorCompressionType compressionType,
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


BAE_UNSIGNED_FIXED PV_RecommendSampleRateForCompression(BAERmfEditorSample const *sample,
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


BAEResult PV_ResampleWaveformLinear(GM_Waveform *waveform,
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


BAEResult PV_EnsureWaveformDataOwned(GM_Waveform *waveform, XPTR *ioWaveDataOwner)
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


BAEResult PV_ApplyOpusLoopSeamMicroFade(GM_Waveform *waveform,
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
void PV_ForceSndLoopPoints(XPTR sndResource, int32_t loopStart, int32_t loopEnd)
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


void PV_ForceSndDecodedFrameCount(XPTR sndResource, uint32_t frameCount)
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
void PV_RemapLoopPointsToFrameCount(uint32_t sourceFrames,
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


uint32_t PV_GetDecodedFrameCountFromSnd(XPTR sndResource)
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
bool PV_CanReuseSndResourceForSamples(BAERmfEditorSample const *left,
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


BAEResult PV_CopyOriginalInstExtendedTail(BAERmfEditorInstrumentExt const *ext,
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


AudioFileType PV_TranslateEditorFileType(BAEFileType fileType)
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


bool PV_IsSupportedPassthroughCompression(SndCompressionType compressionType)
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
        case C_IMA2:
            return TRUE;
        default:
            return FALSE;
    }
}


BAEResult PV_CompressionTypeFromEditorFileType(BAEFileType fileType,
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


BAEResult PV_CreatePassthroughSndFromEncodedData(GM_Waveform const *decodedWaveform,
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


BAEResult PV_CreatePassthroughSndFromCompressedWaveform(GM_Waveform const *decodedWaveform,
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


/* Codecs whose native bitstream can be extracted from an XSndHeader3 blob
 * (FLAC/Vorbis/Opus/MPEG/QOA). IMA/ADPCM/PCM decode to WAV instead. */
bool PV_IsBitstreamPassthroughCodec(SndCompressionType srcCodec)
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
    case C_IMA2:
        return TRUE;
    default:
        return FALSE;
    }
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

