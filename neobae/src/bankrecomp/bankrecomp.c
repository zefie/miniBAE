/****************************************************************************
 *
 * bankrecomp.c
 *
 * Bank recompression utility.
 * Loads a bank file (.hsb/.zsb), optionally recompresses all sample data
 * to a specified codec/bitrate, and writes the result to a new file.
 *
 * Usage: bankresave [options] <input.hsb|zsb> <output.hsb|zsb>
 *
 ****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <NeoBAE.h>
#include <BAE_API.h>
#include <X_API.h>
#include <X_Formats.h>
#include <GenSnd.h>

/* ── Skip-programs list ─────────────────────────────────────────── */

#define MAX_SKIP_PROGRAMS 256
#define MAX_TARGET_INSTRUMENTS 1024
#define MAX_TARGET_SAMPLES 4096
#define MAX_TARGET_SNDS 4096

static uint32_t skipPrograms[MAX_SKIP_PROGRAMS];
static int      skipProgramCount = 0;
static uint32_t targetInstruments[MAX_TARGET_INSTRUMENTS];
static int      targetInstrumentCount = 0;

typedef struct BankTargetSample
{
    uint32_t instrumentIndex;
    uint32_t sampleIndex;
} BankTargetSample;

static BankTargetSample targetSamples[MAX_TARGET_SAMPLES];
static int              targetSampleCount = 0;

/* Parse a comma-separated list of program numbers into skipPrograms[]. */
static void parseSkipPrograms(const char *str)
{
    const char *p = str;
    skipProgramCount = 0;
    while (*p && skipProgramCount < MAX_SKIP_PROGRAMS)
    {
        while (*p == ',' || *p == ' ') p++;
        if (*p == '\0') break;
        skipPrograms[skipProgramCount++] = (uint32_t)atol(p);
        while (*p && *p != ',') p++;
    }
}

static int isSkippedProgram(uint32_t program)
{
    int i;
    for (i = 0; i < skipProgramCount; i++)
    {
        if (skipPrograms[i] == program)
            return 1;
    }
    return 0;
}

static int addUniqueUint32(uint32_t *values, int *count, int maxCount, uint32_t value)
{
    int i;

    if (!values || !count)
    {
        return 0;
    }

    for (i = 0; i < *count; ++i)
    {
        if (values[i] == value)
        {
            return 1;
        }
    }
    if (*count >= maxCount)
    {
        return 0;
    }
    values[*count] = value;
    ++(*count);
    return 1;
}

static int addUniqueTargetSample(uint32_t instrumentIndex, uint32_t sampleIndex)
{
    int i;

    for (i = 0; i < targetSampleCount; ++i)
    {
        if (targetSamples[i].instrumentIndex == instrumentIndex &&
            targetSamples[i].sampleIndex == sampleIndex)
        {
            return 1;
        }
    }
    if (targetSampleCount >= MAX_TARGET_SAMPLES)
    {
        return 0;
    }
    targetSamples[targetSampleCount].instrumentIndex = instrumentIndex;
    targetSamples[targetSampleCount].sampleIndex = sampleIndex;
    ++targetSampleCount;
    return 1;
}

static int parseTargetInstruments(const char *str)
{
    const char *p;

    if (!str)
    {
        return 0;
    }

    p = str;
    while (*p)
    {
        char *end;
        unsigned long value;

        while (*p == ',' || *p == ' ')
        {
            ++p;
        }
        if (*p == '\0')
        {
            break;
        }

        value = strtoul(p, &end, 10);
        if (end == p)
        {
            return 0;
        }
        if (!addUniqueUint32(targetInstruments,
                             &targetInstrumentCount,
                             MAX_TARGET_INSTRUMENTS,
                             (uint32_t)value))
        {
            return 0;
        }

        p = end;
        while (*p == ' ')
        {
            ++p;
        }
        if (*p == ',')
        {
            ++p;
            continue;
        }
        if (*p != '\0')
        {
            return 0;
        }
    }

    return 1;
}

static int parseTargetSamples(const char *str)
{
    const char *p;

    if (!str)
    {
        return 0;
    }

    p = str;
    while (*p)
    {
        char *endInstrument;
        char *endSample;
        unsigned long instrumentIndex;
        unsigned long sampleIndex;

        while (*p == ',' || *p == ' ')
        {
            ++p;
        }
        if (*p == '\0')
        {
            break;
        }

        instrumentIndex = strtoul(p, &endInstrument, 10);
        if (endInstrument == p || *endInstrument != ':')
        {
            return 0;
        }

        sampleIndex = strtoul(endInstrument + 1, &endSample, 10);
        if (endSample == endInstrument + 1)
        {
            return 0;
        }
        if (!addUniqueTargetSample((uint32_t)instrumentIndex, (uint32_t)sampleIndex))
        {
            return 0;
        }

        p = endSample;
        while (*p == ' ')
        {
            ++p;
        }
        if (*p == ',')
        {
            ++p;
            continue;
        }
        if (*p != '\0')
        {
            return 0;
        }
    }

    return 1;
}

static int hasTargetFilters(void)
{
    return (targetInstrumentCount > 0 || targetSampleCount > 0);
}

/* ── Codec table ────────────────────────────────────────────────── */

enum
{
    CODEC_PCM           = 0,
    CODEC_ADPCM         = 1,
    CODEC_ALAW          = 2,
    CODEC_ULAW          = 3,
    CODEC_MP3           = 4,
    CODEC_VORBIS        = 5,
    CODEC_FLAC          = 6,
    CODEC_OPUS          = 7,
#if USE_QOA_SUPPORT == TRUE
    CODEC_QOA           = 8,
    CODEC_COUNT         = 9
#else
    CODEC_COUNT         = 8
#endif
};

static const char *codecNames[CODEC_COUNT] =
{
    "PCM",
    "ADPCM",
    "A-law",
    "u-law",
    "MP3",
    "VORBIS",
    "FLAC",
#if USE_QOA_SUPPORT == TRUE
    "OPUS",
    "QOA"
#else
    "OPUS"
#endif
};

/* Returns TRUE if the chosen codec requires ZSB (ZREZ) container */
static int codecRequiresZsb(int codec)
{
    return (codec == CODEC_VORBIS || codec == CODEC_FLAC ||
            codec == CODEC_OPUS
#if USE_QOA_SUPPORT == TRUE
            || codec == CODEC_QOA
#endif
            );
}

/* ── Bitrate tables per codec ───────────────────────────────────── */

typedef struct
{
    uint32_t kbps;
    SndCompressionType compType;
    SndCompressionSubType compSubType;
} BitrateEntry;

static const BitrateEntry mp3Bitrates[] =
{
    {  32, C_MPEG_32,  CS_MPEG2 },
    {  48, C_MPEG_48,  CS_MPEG2 },
    {  64, C_MPEG_64,  CS_MPEG2 },
    {  96, C_MPEG_96,  CS_MPEG2 },
    { 128, C_MPEG_128, CS_MPEG2 },
    { 192, C_MPEG_192, CS_MPEG2 },
    { 256, C_MPEG_256, CS_MPEG2 },
    { 320, C_MPEG_320, CS_MPEG2 }
};

static const BitrateEntry vorbisBitrates[] =
{
    {  32, C_VORBIS, CS_VORBIS_32K  },
    {  48, C_VORBIS, CS_VORBIS_48K  },
    {  64, C_VORBIS, CS_VORBIS_64K  },
    {  80, C_VORBIS, CS_VORBIS_80K  },
    {  96, C_VORBIS, CS_VORBIS_96K  },
    { 128, C_VORBIS, CS_VORBIS_128K },
    { 160, C_VORBIS, CS_VORBIS_160K },
    { 192, C_VORBIS, CS_VORBIS_192K },
    { 256, C_VORBIS, CS_VORBIS_256K }
};

static const BitrateEntry opusBitrates[] =
{
    {  12, C_OPUS, CS_OPUS_12K  },
    {  16, C_OPUS, CS_OPUS_16K  },
    {  24, C_OPUS, CS_OPUS_24K  },
    {  32, C_OPUS, CS_OPUS_32K  },
    {  48, C_OPUS, CS_OPUS_48K  },
    {  64, C_OPUS, CS_OPUS_64K  },
    {  80, C_OPUS, CS_OPUS_80K  },
    {  96, C_OPUS, CS_OPUS_96K  },
    { 128, C_OPUS, CS_OPUS_128K },
    { 160, C_OPUS, CS_OPUS_160K },
    { 192, C_OPUS, CS_OPUS_192K },
    { 256, C_OPUS, CS_OPUS_256K }
};

#define ARRAY_COUNT(a) (sizeof(a)/sizeof((a)[0]))

/* ── Compression type name helper ───────────────────────────────── */

static const char *compressionName(XResourceType ct, uint32_t subType)
{
    static char combined[48];
    const char *base = NULL;
    const char *bitrate = NULL;

    switch ((uint32_t)ct)
    {
        case 0:                                     return "PCM";
        case FOUR_CHAR('n','o','n','e'):            return "PCM";
        case FOUR_CHAR('i','m','a','4'):            return "IMA4/ADPCM";
        case FOUR_CHAR('i','m','a','W'):            return "IMA4/ADPCM";
        case FOUR_CHAR('i','m','a','3'):            return "IMA3/ADPCM";
        case FOUR_CHAR('M','A','C','3'):            return "MACE3";
        case FOUR_CHAR('M','A','C','6'):            return "MACE6";
        case FOUR_CHAR('u','l','a','w'):            return "uLaw";
        case FOUR_CHAR('a','l','a','w'):            return "aLaw";
        case FOUR_CHAR('m','p','g','n'):            return "MP3 (32k)";
        case FOUR_CHAR('m','p','g','a'):            return "MP3 (40k)";
        case FOUR_CHAR('m','p','g','b'):            return "MP3 (48k)";
        case FOUR_CHAR('m','p','g','c'):            return "MP3 (56k)";
        case FOUR_CHAR('m','p','g','d'):            return "MP3 (64k)";
        case FOUR_CHAR('m','p','g','e'):            return "MP3 (80k)";
        case FOUR_CHAR('m','p','g','f'):            return "MP3 (96k)";
        case FOUR_CHAR('m','p','g','g'):            return "MP3 (112k)";
        case FOUR_CHAR('m','p','g','h'):            return "MP3 (128k)";
        case FOUR_CHAR('m','p','g','i'):            return "MP3 (160k)";
        case FOUR_CHAR('m','p','g','j'):            return "MP3 (192k)";
        case FOUR_CHAR('m','p','g','k'):            return "MP3 (224k)";
        case FOUR_CHAR('m','p','g','l'):            return "MP3 (256k)";
        case FOUR_CHAR('m','p','g','m'):            return "MP3 (320k)";
        case FOUR_CHAR('f','L','a','C'):            return "FLAC";
    #if USE_QOA_SUPPORT == TRUE
        case FOUR_CHAR('q','o','a','f'):            return "QOA";
    #endif
        case FOUR_CHAR('O','g','g','V'):            base = "Vorbis"; break;
        case FOUR_CHAR('O','g','g','O'):            base = "Opus"; break;
        default:                                    return "Unknown";
    }

    /* Vorbis/Opus — append bitrate from sub-type if available */
    switch ((uint32_t)subType)
    {
        case FOUR_CHAR('v','0','3','2'): bitrate = " (32k)"; break;
        case FOUR_CHAR('v','0','4','8'): bitrate = " (48k)"; break;
        case FOUR_CHAR('v','0','6','4'): bitrate = " (64k)"; break;
        case FOUR_CHAR('v','0','8','0'): bitrate = " (80k)"; break;
        case FOUR_CHAR('v','0','9','6'): bitrate = " (96k)"; break;
        case FOUR_CHAR('v','1','2','8'): bitrate = " (128k)"; break;
        case FOUR_CHAR('v','1','6','0'): bitrate = " (160k)"; break;
        case FOUR_CHAR('v','1','9','2'): bitrate = " (192k)"; break;
        case FOUR_CHAR('v','2','5','6'): bitrate = " (256k)"; break;
        case FOUR_CHAR('o','0','1','2'): bitrate = " (12k)"; break;
        case FOUR_CHAR('o','0','1','6'): bitrate = " (16k)"; break;
        case FOUR_CHAR('o','0','2','4'): bitrate = " (24k)"; break;
        case FOUR_CHAR('o','0','3','2'): bitrate = " (32k)"; break;
        case FOUR_CHAR('o','0','4','8'): bitrate = " (48k)"; break;
        case FOUR_CHAR('o','0','6','4'): bitrate = " (64k)"; break;
        case FOUR_CHAR('o','0','8','0'): bitrate = " (80k)"; break;
        case FOUR_CHAR('o','0','9','6'): bitrate = " (96k)"; break;
        case FOUR_CHAR('o','1','2','8'): bitrate = " (128k)"; break;
        case FOUR_CHAR('o','1','6','0'): bitrate = " (160k)"; break;
        case FOUR_CHAR('o','1','9','2'): bitrate = " (192k)"; break;
        case FOUR_CHAR('o','2','5','6'): bitrate = " (256k)"; break;
        default: break;
    }
    if (bitrate)
    {
        snprintf(combined, sizeof(combined), "%s%s", base, bitrate);
        return combined;
    }
    return base;
}

/* ── Opus sub-type packing ──────────────────────────────────────── */
/* The Opus encoder in SampleTools.c expects a packed uint32 where low
 * 16 bits are a bitrate index (0-8) and high 16 bits are the opus mode
 * (0=Audio).  Map CS_OPUS_*K FourCCs to the small index. */

static uint32_t opusSubTypeToIndex(SndCompressionSubType subType)
{
    switch (subType)
    {
        case CS_OPUS_12K:  return 0;
        case CS_OPUS_16K:  return 1;
        case CS_OPUS_24K:  return 2;
        case CS_OPUS_32K:  return 3;
        case CS_OPUS_48K:  return 4;
        case CS_OPUS_64K:  return 5;
        case CS_OPUS_80K:  return 9;
        case CS_OPUS_96K:  return 6;
        case CS_OPUS_128K: return 7;
        case CS_OPUS_160K: return 10;
        case CS_OPUS_192K: return 11;
        case CS_OPUS_256K: return 8;
        default:           return 7;
    }
}

static SndCompressionSubType packOpusSubType(SndCompressionSubType baseSubType)
{
    /* Audio mode = 0 in high 16 bits */
    return (SndCompressionSubType)(opusSubTypeToIndex(baseSubType) & 0xFFFFU);
}

/* Store the compression sub-type tag in a Type3 SND resource so it can be
 * read back by PV_GetStoredCompressionSubTypeFromSnd (in BAERmfEditor.c).
 * Only meaningful for Vorbis and Opus. */
#define BANKRECOMP_SUBTYPE_TAG  FOUR_CHAR('b','q','s','t')

static void storeCompressionSubType(XPTR sndData, int32_t sndSize,
                                    SndCompressionType compressionType,
                                    SndCompressionSubType compressionSubType)
{
    XSndHeader3 *header3;

    if (!sndData || sndSize < (int32_t)sizeof(XSndHeader3))
    {
        return;
    }
    if (compressionType != C_VORBIS && compressionType != C_OPUS)
    {
        return;
    }
    header3 = (XSndHeader3 *)sndData;
    if (XGetShort(&header3->type) != XThirdSoundFormat)
    {
        return;
    }
    XPutLong(&header3->sndBuffer.reserved3[0], (uint32_t)BANKRECOMP_SUBTYPE_TAG);
    XPutLong(&header3->sndBuffer.reserved3[1], (uint32_t)compressionSubType);
}

/* ── Help output ────────────────────────────────────────────────── */

static void printHelp(const char *progName)
{
    printf("Usage: %s [options] <input.hsb|zsb> <output.hsb|zsb>\n\n", progName);
    printf("Recompresses samples in a bank file to a target codec.\n");
    printf("Without target filters, all samples are processed.\n\n");
    printf("Options:\n");
    printf("  --codec N      Target codec number (see list below)\n");
    printf("  --bitrate N    Target bitrate in kbps (e.g. 128) or bps (e.g. 128000)\n");
    printf("  --minframes N  Skip recompression for samples with fewer than N frames\n");
    printf("  --skip P,P,... Skip recompression for listed program numbers\n");
    printf("  --instrument I[,I...]\n");
    printf("                 Recompress only listed bank instrument indices\n");
    printf("  --sample I:S[,I:S...]\n");
    printf("                 Recompress only listed instrument sample slots\n");
    printf("  --help         Show this help message\n\n");
    printf("Codecs:\n");
    printf("  %d  %-22s  (no bitrate option)\n", CODEC_PCM, codecNames[CODEC_PCM]);
    printf("  %d  %-22s  (no bitrate option)\n", CODEC_ADPCM, codecNames[CODEC_ADPCM]);
        printf("  %d  %-22s  (no bitrate option)\n", CODEC_ALAW, codecNames[CODEC_ALAW]);
        printf("  %d  %-22s  (no bitrate option)\n", CODEC_ULAW, codecNames[CODEC_ULAW]);
    printf("  %d  %-22s  bitrates: 32, 48, 64, 96, 128, 192, 256, 320 kbps\n",
           CODEC_MP3, codecNames[CODEC_MP3]);
    printf("  %d  %-22s  bitrates: 32, 48, 64, 80, 96, 128, 160, 192, 256 kbps\n",
           CODEC_VORBIS, codecNames[CODEC_VORBIS]);
    printf("  %d  %-22s  (no bitrate option, lossless)\n", CODEC_FLAC, codecNames[CODEC_FLAC]);
    printf("  %d  %-22s  bitrates: 12, 16, 24, 32, 48, 64, 80, 96, 128, 160, 192, 256 kbps\n",
           CODEC_OPUS, codecNames[CODEC_OPUS]);
#if USE_QOA_SUPPORT == TRUE
        printf("  %d  %-22s  (no bitrate option)\n", CODEC_QOA, codecNames[CODEC_QOA]);
#endif
#if USE_QOA_SUPPORT == TRUE
    printf("\nNote: VORBIS, FLAC, OPUS, QOA codecs force ZSB (ZREZ) output format.\n");
#else
    printf("\nNote: VORBIS, FLAC, OPUS codecs force ZSB (ZREZ) output format.\n");
#endif
    printf("      Without --codec, the bank is resaved with original compression.\n");
    printf("      Instrument/sample indices are the ones printed in the bank listing.\n");
}

/* ── Bitrate lookup ─────────────────────────────────────────────── */

/* Parse a bitrate value that can be in kbps (e.g. 128) or bps (e.g. 128000).
 * Returns the value in kbps. */
static uint32_t parseBitrate(const char *str)
{
    uint32_t val = (uint32_t)atol(str);
    if (val >= 1000)
    {
        val /= 1000;
    }
    return val;
}

/* Find the closest matching bitrate entry for a given codec and kbps value. */
static int findBitrateEntry(int codec, uint32_t kbps,
                            SndCompressionType *outComp,
                            SndCompressionSubType *outSub)
{
    const BitrateEntry *table;
    size_t count;
    size_t i;
    size_t best;
    int32_t bestDiff;

    switch (codec)
    {
        case CODEC_MP3:
            table = mp3Bitrates;
            count = ARRAY_COUNT(mp3Bitrates);
            break;
        case CODEC_VORBIS:
            table = vorbisBitrates;
            count = ARRAY_COUNT(vorbisBitrates);
            break;
        case CODEC_OPUS:
            table = opusBitrates;
            count = ARRAY_COUNT(opusBitrates);
            break;
        default:
            return -1;
    }

    best = 0;
    bestDiff = (int32_t)abs((int32_t)(table[0].kbps - kbps));
    for (i = 1; i < count; i++)
    {
        int32_t diff = (int32_t)abs((int32_t)(table[i].kbps - kbps));
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = i;
        }
    }

    *outComp = table[best].compType;
    *outSub = table[best].compSubType;
    if (kbps != table[best].kbps)
    {
        fprintf(stderr, "Note: using closest available bitrate: %u kbps\n",
                table[best].kbps);
    }
    return 0;
}

/* Get default bitrate for a codec (used when --bitrate is not specified). */
static void getDefaultBitrate(int codec,
                              SndCompressionType *outComp,
                              SndCompressionSubType *outSub)
{
    switch (codec)
    {
        case CODEC_PCM:
            *outComp = C_NONE;
            *outSub = CS_DEFAULT;
            break;
        case CODEC_ADPCM:
            *outComp = C_IMA4;
            *outSub = CS_DEFAULT;
            break;
        case CODEC_ALAW:
            *outComp = C_ALAW;
            *outSub = CS_DEFAULT;
            break;
        case CODEC_ULAW:
            *outComp = C_ULAW;
            *outSub = CS_DEFAULT;
            break;
        case CODEC_MP3:
            *outComp = C_MPEG_128;
            *outSub = CS_MPEG2;
            break;
#if USE_VORBIS_ENCODER == TRUE            
        case CODEC_VORBIS:
            *outComp = C_VORBIS;
            *outSub = CS_VORBIS_128K;
            break;
#endif
#if USE_FLAC_ENCODER == TRUE            
        case CODEC_FLAC:
            *outComp = C_FLAC;
            *outSub = CS_DEFAULT;
            break;
#endif
#if USE_OPUS_ENCODER == TRUE            
        case CODEC_OPUS:
            *outComp = C_OPUS;
            *outSub = CS_OPUS_48K;
            break;
#endif            
#if USE_QOA_SUPPORT == TRUE
        case CODEC_QOA:
            *outComp = C_QOA;
            *outSub = CS_DEFAULT;
            break;
#endif
        default:
            *outComp = C_NONE;
            *outSub = CS_DEFAULT;
            break;
    }
}

/* ── Recompression logic ─────────────────────────────────────────── */

/* Recompress a single SND resource blob.  Returns a newly allocated SND
 * resource with the target codec, or NULL on failure.  Caller must free
 * the returned pointer with XDisposePtr. */
static XPTR recompressSnd(XPTR sndData, int32_t sndSize,
                          SndCompressionType targetComp,
                          SndCompressionSubType targetSub,
                          int32_t *outSize)
{
    SampleDataInfo sampleInfo;
    XPTR samplePtr;
    GM_Waveform waveform;
    OPErr err;
    XPTR newSndData;
    SndCompressionSubType encodeSub;
    XFIXED sourceRate;
    XResourceType origCompression;

    (void)sndSize;
    *outSize = 0;

    /* Decode the SND resource to PCM.
     * XGetSamplePtrFromSnd handles ALL compression types internally —
     * it decodes compressed data and returns a PCM pointer.  For PCM
     * input it returns a pointer into sndData; for compressed input
     * it allocates a new buffer.  Note: sampleInfo.compressionType is
     * preserved as the ORIGINAL codec (not C_NONE) even though the
     * returned data is decoded PCM. */
    XSetMemory(&sampleInfo, (int32_t)sizeof(sampleInfo), 0);
    samplePtr = XGetSamplePtrFromSnd(sndData, &sampleInfo);
    if (!samplePtr || sampleInfo.frames == 0)
    {
        fprintf(stderr, "      Warning: failed to decode SND resource, skipping\n");
        return NULL;
    }

    /* Remember original compression and source rate before we modify anything */
    origCompression = sampleInfo.compressionType;
    sourceRate = sampleInfo.rate;

    /* Build a GM_Waveform from the decoded data.
     * Force compressionType to C_NONE since XGetSamplePtrFromSnd already
     * decoded any compressed data — do NOT call XDecodeSampleData again. */
    XSetMemory(&waveform, (int32_t)sizeof(waveform), 0);
    XTranslateFromSampleDataToWaveform(&sampleInfo, &waveform);
    waveform.theWaveform = samplePtr;
    waveform.compressionType = C_NONE;
    waveform.waveSize = sampleInfo.frames * sampleInfo.channels * (sampleInfo.bitSize / 8);

    /* Opus is round-trip-only in this tool: spoof 48kHz so encoder keeps
     * original sample-domain timing, then restore source rate metadata. */
    if (targetComp == C_OPUS)
    {
        waveform.sampledRate = (XFIXED)(48000U << 16);
    }

    /* For Opus, pack sub-type into the index format that
     * XCreateSoundObjectFromData expects. */
    encodeSub = targetSub;
    if (targetComp == C_OPUS)
    {
        encodeSub = packOpusSubType(targetSub);
    }

    /* Encode to the target format */
    newSndData = NULL;
    err = XCreateSoundObjectFromData(&newSndData, &waveform,
                                     targetComp, encodeSub,
                                     NULL, NULL);

    /* Free the decoded PCM buffer if XGetSamplePtrFromSnd allocated it.
     * Compressed sources → newly allocated buffer (must free).
     * PCM sources → pointer into sndData (must NOT free). */
    if (origCompression != (XResourceType)C_NONE && samplePtr != NULL)
    {
        XDisposePtr(samplePtr);
    }

    if (err != NO_ERR || !newSndData)
    {
        fprintf(stderr, "      Warning: encode failed (err=%d), skipping\n", (int)err);
        return NULL;
    }

    /* Store compression sub-type tag so bitrate can be read back (Vorbis/Opus) */
    storeCompressionSubType(newSndData, XGetPtrSize(newSndData),
                            targetComp, targetSub);

    /* ── Post-encode fixup (mirrors BAERmfEditor.c save path) ──────────
     * Re-decode the freshly encoded SND to discover the actual decoded
     * frame count and sample rate.  Lossy codecs can change both (e.g.
     * Opus resamples to 48kHz, MPEG snaps to closest valid rate, and
     * all codecs may add/remove frames due to padding). */
    {
        uint32_t    encodedFrames;
        SampleDataInfo decodedInfo;
        XPTR        decodedOwner;
        XFIXED      sndSampleRate;
        int32_t     loopStart;
        int32_t     loopEnd;
        bool       isMpeg;

        XSetMemory(&decodedInfo, (int32_t)sizeof(decodedInfo), 0);
        decodedOwner = NULL;
        (void)XGetSamplePtrFromSnd(newSndData, &decodedInfo);

        encodedFrames = decodedInfo.frames;
        if (encodedFrames == 0)
        {
            encodedFrames = sampleInfo.frames;
        }

        loopStart = (int32_t)sampleInfo.loopStart;
        loopEnd   = (int32_t)sampleInfo.loopEnd;

        isMpeg = (((uint32_t)targetComp & 0xFFFFFF00U) == FOUR_CHAR('m','p','g','\0'))
                 || (targetComp == (SndCompressionType)C_VORBIS);

        if (targetComp == C_OPUS)
        {
            /* RT mode: PCM was fed to the encoder at its native rate with no
             * resampling.  Force frameCount back to source frame count (Opus
             * pre-skip can make the probed decode slightly shorter).  Clamp
             * loopEnd to actual Opus decode capacity. */
            XSndHeader3 *snd = (XSndHeader3 *)newSndData;
            if (XGetShort(&snd->type) == XThirdSoundFormat)
            {
                uint32_t bpf = (uint32_t)snd->sndBuffer.channels *
                               ((uint32_t)snd->sndBuffer.bitSize / 8U);
                XPutLong(&snd->sndBuffer.frameCount, sampleInfo.frames);
                if (bpf > 0)
                {
                    XPutLong(&snd->sndBuffer.decodedBytes,
                             sampleInfo.frames * bpf);
                }
            }
            if (encodedFrames > 0 && loopEnd > (int32_t)encodedFrames)
            {
                loopEnd = (int32_t)encodedFrames;
                if (loopStart >= loopEnd)
                {
                    loopStart = 0;
                    loopEnd   = 0;
                }
            }

            /* Restore the real source rate (was spoofed to 48kHz for the
             * encoder) and set the round-trip flag. */
            sndSampleRate = sourceRate;
            XSetSoundSampleRate(newSndData, sndSampleRate);
            XSetSoundOpusRoundTripFlag(newSndData, TRUE);
        }
        else
        {
            /* Non-RT lossy codecs (MPEG, Vorbis, standard Opus):
             * Update SND frameCount/decodedBytes to actual decoded frames
             * and remap loop points into the new frame domain. */
            XSndHeader3 *snd = (XSndHeader3 *)newSndData;
            if (XGetShort(&snd->type) == XThirdSoundFormat)
            {
                uint32_t bpf = (uint32_t)snd->sndBuffer.channels *
                               ((uint32_t)snd->sndBuffer.bitSize / 8U);
                XPutLong(&snd->sndBuffer.frameCount, encodedFrames);
                if (bpf > 0)
                {
                    XPutLong(&snd->sndBuffer.decodedBytes,
                             encodedFrames * bpf);
                }
            }

            /* Remap loop points from source frame domain to encoded frame
             * domain (inline PV_RemapLoopPointsToFrameCount logic). */
            if (sampleInfo.frames > 0 && encodedFrames > 0 &&
                loopStart >= 0 && loopEnd > loopStart &&
                (uint32_t)loopStart < sampleInfo.frames)
            {
                if ((uint32_t)loopEnd > sampleInfo.frames)
                {
                    loopEnd = (int32_t)sampleInfo.frames;
                }
                if (loopEnd > loopStart && sampleInfo.frames != encodedFrames)
                {
                    uint32_t mappedStart, mappedEnd;

                    mappedStart = (uint32_t)(((uint64_t)(uint32_t)loopStart *
                                              (uint64_t)encodedFrames) /
                                             (uint64_t)sampleInfo.frames);
                    mappedEnd   = (uint32_t)((((uint64_t)(uint32_t)loopEnd *
                                               (uint64_t)encodedFrames) +
                                              ((uint64_t)sampleInfo.frames - 1ULL)) /
                                             (uint64_t)sampleInfo.frames);
                    if (mappedEnd > encodedFrames)
                    {
                        mappedEnd = encodedFrames;
                    }
                    if (mappedStart >= mappedEnd)
                    {
                        mappedStart = 0;
                        mappedEnd   = 0;
                    }
                    loopStart = (int32_t)mappedStart;
                    loopEnd   = (int32_t)mappedEnd;
                }
            }
            else
            {
                loopStart = 0;
                loopEnd   = 0;
            }

            /* Set sample rate:
             * MPEG/Vorbis: use the decoded stream rate (encoder may have
             *   snapped to nearest valid MPEG rate).
             * Standard Opus: use the original source rate (the SND currently
             *   says 48kHz from XCreateSoundObjectFromData, but the engine
             *   overwrites info->rate with the decoded rate during playback;
             *   storing the original rate lets the engine compute correct
             *   pitch). */
            sndSampleRate = sourceRate;
            if (isMpeg && decodedInfo.rate != 0)
            {
                sndSampleRate = decodedInfo.rate;
            }
            XSetSoundSampleRate(newSndData, sndSampleRate);
        }

        XSetSoundLoopPoints(newSndData, loopStart, loopEnd);

        /* Free decoded buffer if XGetSamplePtrFromSnd allocated one */
        if (decodedInfo.pMasterPtr && decodedInfo.pMasterPtr != newSndData)
        {
            decodedOwner = decodedInfo.pMasterPtr;
        }
        if (decodedOwner)
        {
            XDisposePtr(decodedOwner);
        }
    }

    *outSize = XGetPtrSize(newSndData);
    return newSndData;
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    BAEMixer mixer;
    BAEBankToken bankToken;
    BAEResult result;
    uint32_t instCount;
    uint32_t i;
    const char *inputPath = NULL;
    const char *outputPath = NULL;
    int codec = -1;           /* -1 = no recompression */
    uint32_t bitrateKbps = 0; /* 0 = use default for codec */
    uint32_t minFrames = 0;   /* 0 = compress all samples */
    int argIdx;
    SndCompressionType targetComp;
    SndCompressionSubType targetSub;
    int doRecompress;
    int instrumentTargetMatched[MAX_TARGET_INSTRUMENTS];
    int sampleTargetMatched[MAX_TARGET_SAMPLES];

    /* Parse arguments */
    for (argIdx = 1; argIdx < argc; argIdx++)
    {
        if (strcmp(argv[argIdx], "--help") == 0 || strcmp(argv[argIdx], "-h") == 0)
        {
            printHelp(argv[0]);
            return 0;
        }
        else if (strcmp(argv[argIdx], "--codec") == 0)
        {
            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --codec requires a number argument\n");
                return 1;
            }
            codec = atoi(argv[++argIdx]);
            if (codec < 0 || codec >= CODEC_COUNT)
            {
                fprintf(stderr, "Error: invalid codec number %d (valid: 0-%d)\n",
                        codec, CODEC_COUNT - 1);
                return 1;
            }
        }
        else if (strcmp(argv[argIdx], "--bitrate") == 0)
        {
            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --bitrate requires a number argument\n");
                return 1;
            }
            bitrateKbps = parseBitrate(argv[++argIdx]);
        }
        else if (strcmp(argv[argIdx], "--minframes") == 0)
        {
            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --minframes requires a number argument\n");
                return 1;
            }
            minFrames = (uint32_t)atol(argv[++argIdx]);
        }
        else if (strcmp(argv[argIdx], "--skip") == 0)
        {
            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --skip requires a comma-separated list of program numbers\n");
                return 1;
            }
            parseSkipPrograms(argv[++argIdx]);
        }
        else if (strcmp(argv[argIdx], "--instrument") == 0)
        {
            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --instrument requires a comma-separated list of indices\n");
                return 1;
            }
            if (!parseTargetInstruments(argv[++argIdx]))
            {
                fprintf(stderr, "Error: invalid --instrument list '%s'\n", argv[argIdx]);
                return 1;
            }
        }
        else if (strcmp(argv[argIdx], "--sample") == 0)
        {
            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --sample requires I:S selectors\n");
                return 1;
            }
            if (!parseTargetSamples(argv[++argIdx]))
            {
                fprintf(stderr, "Error: invalid --sample list '%s' (expected I:S[,I:S...])\n",
                        argv[argIdx]);
                return 1;
            }
        }
        else if (argv[argIdx][0] == '-')
        {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[argIdx]);
            fprintf(stderr, "Use --help for usage information.\n");
            return 1;
        }
        else if (!inputPath)
        {
            inputPath = argv[argIdx];
        }
        else if (!outputPath)
        {
            outputPath = argv[argIdx];
        }
        else
        {
            fprintf(stderr, "Error: unexpected argument '%s'\n", argv[argIdx]);
            return 1;
        }
    }

    if (!inputPath || !outputPath)
    {
        fprintf(stderr, "Error: input and output file paths required.\n");
        fprintf(stderr, "Use --help for usage information.\n");
        return 1;
    }

    /* Resolve compression type and sub-type */
    doRecompress = (codec >= 0);
    targetComp = C_NONE;
    targetSub = CS_DEFAULT;

    if (!doRecompress && hasTargetFilters())
    {
        fprintf(stderr, "Error: --instrument/--sample require --codec\n");
        return 1;
    }

    if (doRecompress)
    {
        if (bitrateKbps > 0 &&
            (codec == CODEC_MP3 || codec == CODEC_VORBIS ||
             codec == CODEC_OPUS))
        {
            if (findBitrateEntry(codec, bitrateKbps, &targetComp, &targetSub) != 0)
            {
                fprintf(stderr, "Error: bitrate lookup failed for codec %d\n", codec);
                return 1;
            }
        }
        else
        {
            getDefaultBitrate(codec, &targetComp, &targetSub);
        }

        /* Warn if bitrate was specified for a codec that doesn't use it */
        if (bitrateKbps > 0 &&
            (codec == CODEC_PCM || codec == CODEC_ADPCM || codec == CODEC_ALAW ||
             codec == CODEC_ULAW || codec == CODEC_FLAC
#if USE_QOA_SUPPORT == TRUE
             || codec == CODEC_QOA
#endif
             ))
        {
            fprintf(stderr, "Note: --bitrate is ignored for %s codec\n", codecNames[codec]);
        }
    }

    /* Warn/force ZSB if output extension is .hsb but codec requires ZSB */
    if (doRecompress && codecRequiresZsb(codec))
    {
        size_t len = strlen(outputPath);
        if (len >= 4)
        {
            const char *ext = outputPath + len - 4;
            if (strcasecmp(ext, ".hsb") == 0)
            {
                fprintf(stderr,
                    "Error: %s codec requires ZSB format. Please use a .zsb output extension.\n",
                    codecNames[codec]);
                return 1;
            }
        }
    }

    if (doRecompress)
    {
        printf("Recompression target: %s", codecNames[codec]);
        if (codec == CODEC_MP3 || codec == CODEC_VORBIS ||
            codec == CODEC_OPUS)
        {
            /* Find the actual kbps we're using */
            const BitrateEntry *table = NULL;
            size_t count = 0, j;
            switch (codec)
            {
                case CODEC_MP3: table = mp3Bitrates; count = ARRAY_COUNT(mp3Bitrates); break;
                case CODEC_VORBIS: table = vorbisBitrates; count = ARRAY_COUNT(vorbisBitrates); break;
                case CODEC_OPUS: table = opusBitrates; count = ARRAY_COUNT(opusBitrates); break;
            }
            if (table)
            {
                for (j = 0; j < count; j++)
                {
                    if (table[j].compType == targetComp && table[j].compSubType == targetSub)
                    {
                        printf(" @ %u kbps", table[j].kbps);
                        break;
                    }
                }
            }
        }
        printf("\n");
        if (targetInstrumentCount > 0)
        {
            int ti;

            printf("Target instruments:");
            for (ti = 0; ti < targetInstrumentCount; ++ti)
            {
                printf(" %u", targetInstruments[ti]);
            }
            printf("\n");
        }
        if (targetSampleCount > 0)
        {
            int ts;

            printf("Target samples:");
            for (ts = 0; ts < targetSampleCount; ++ts)
            {
                printf(" %u:%u",
                       targetSamples[ts].instrumentIndex,
                       targetSamples[ts].sampleIndex);
            }
            printf("\n");
        }
        if (minFrames > 0)
        {
            printf("Minimum frames: %u (samples below this are kept as-is)\n", minFrames);
        }
        if (skipProgramCount > 0)
        {
            int si;
            printf("Skipping programs:");
            for (si = 0; si < skipProgramCount; si++)
                printf(" %u", skipPrograms[si]);
            printf("\n");
        }
    }

    /* Create a minimal mixer just to load the bank */
    mixer = BAEMixer_New();
    if (!mixer)
    {
        fprintf(stderr, "Error: BAEMixer_New failed\n");
        return 1;
    }
    result = BAEMixer_Open(mixer, BAE_RATE_22K, BAE_LINEAR_INTERPOLATION,
                           BAE_USE_16, 8, 0, 8, FALSE);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAEMixer_Open failed (%d)\n", (int)result);
        BAEMixer_Delete(mixer);
        return 1;
    }

    /* Load the bank */
    result = BAEMixer_AddBankFromFile(mixer, (BAEPathName)inputPath, &bankToken);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAEMixer_AddBankFromFile failed (%d) for '%s'\n",
                (int)result, inputPath);
        BAEMixer_Delete(mixer);
        return 1;
    }
    printf("Loaded bank: %s\n", inputPath);

    /* Enumerate instruments */
    result = BAERmfEditorBank_GetInstrumentCount(bankToken, &instCount);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: GetInstrumentCount failed (%d)\n", (int)result);
        BAEMixer_Delete(mixer);
        return 1;
    }
    printf("Instrument count: %u\n\n", instCount);

    /* Collect SND resource IDs belonging to skipped programs */
#define MAX_SKIP_SNDS 4096
    int32_t skipSndIDs[MAX_SKIP_SNDS];
    int     skipSndCount = 0;
    int32_t selectedSndIDs[MAX_TARGET_SNDS];
    int     selectedSndCount = 0;

    XSetMemory(instrumentTargetMatched,
               (int32_t)sizeof(instrumentTargetMatched),
               0);
    XSetMemory(sampleTargetMatched,
               (int32_t)sizeof(sampleTargetMatched),
               0);

    for (i = 0; i < instCount; i++)
    {
        BAERmfEditorBankInstrumentInfo instInfo;
        uint32_t sampleCount;
        uint32_t s;

        result = BAERmfEditorBank_GetInstrumentInfo(bankToken, i, &instInfo);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "  [%u] GetInstrumentInfo failed (%d)\n", i, (int)result);
            continue;
        }
        printf("[%3u] ID=%u Bank=%u Prog=%u Splits=%d Name=\"%s\"\n",
               i, instInfo.instID, instInfo.bank, instInfo.program,
               instInfo.keySplitCount, instInfo.name);

        /* Enumerate samples */
        result = BAERmfEditorBank_GetInstrumentSampleCount(bankToken, i, &sampleCount);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "      GetInstrumentSampleCount failed (%d)\n", (int)result);
            continue;
        }

        for (s = 0; s < sampleCount; s++)
        {
            BAERmfEditorBankSampleInfo sampleInfo;
            result = BAERmfEditorBank_GetInstrumentSampleInfo(bankToken, i, s, &sampleInfo);
            if (result != BAE_NO_ERROR)
            {
                fprintf(stderr, "      Sample[%u] GetInstrumentSampleInfo failed (%d)\n",
                        s, (int)result);
                continue;
            }
            printf("      Sample[%u]: SndID=%d Keys=%u-%u Root=%u Rate=%uHz "
                   "Frames=%u %dbit %dch Loop=%u-%u Codec=%s\n",
                   s, (int)sampleInfo.sndResourceID,
                   sampleInfo.lowKey, sampleInfo.highKey, sampleInfo.rootKey,
                   sampleInfo.sampleRate, sampleInfo.frameCount,
                   sampleInfo.bitDepth, sampleInfo.channels,
                   sampleInfo.loopStart, sampleInfo.loopEnd,
                   compressionName(sampleInfo.compressionType,
                                   sampleInfo.compressionSubType));

            /* Track SND IDs belonging to skipped programs */
            if (skipProgramCount > 0 && isSkippedProgram(instInfo.program) &&
                skipSndCount < MAX_SKIP_SNDS)
            {
                int already = 0, si;
                for (si = 0; si < skipSndCount; si++)
                {
                    if (skipSndIDs[si] == sampleInfo.sndResourceID)
                    {
                        already = 1;
                        break;
                    }
                }
                if (!already)
                    skipSndIDs[skipSndCount++] = sampleInfo.sndResourceID;
            }

            if (targetInstrumentCount > 0)
            {
                int ti;

                for (ti = 0; ti < targetInstrumentCount; ++ti)
                {
                    if (targetInstruments[ti] == i)
                    {
                        instrumentTargetMatched[ti] = 1;
                        if (!addUniqueUint32((uint32_t *)selectedSndIDs,
                                             &selectedSndCount,
                                             MAX_TARGET_SNDS,
                                             (uint32_t)sampleInfo.sndResourceID))
                        {
                            fprintf(stderr, "Error: too many targeted SND resources\n");
                            BAEMixer_Delete(mixer);
                            return 1;
                        }
                        break;
                    }
                }
            }

            if (targetSampleCount > 0)
            {
                int ts;

                for (ts = 0; ts < targetSampleCount; ++ts)
                {
                    if (targetSamples[ts].instrumentIndex == i &&
                        targetSamples[ts].sampleIndex == s)
                    {
                        sampleTargetMatched[ts] = 1;
                        if (!addUniqueUint32((uint32_t *)selectedSndIDs,
                                             &selectedSndCount,
                                             MAX_TARGET_SNDS,
                                             (uint32_t)sampleInfo.sndResourceID))
                        {
                            fprintf(stderr, "Error: too many targeted SND resources\n");
                            BAEMixer_Delete(mixer);
                            return 1;
                        }
                        break;
                    }
                }
            }
        }

        /* Show extended info summary */
        {
            BAERmfEditorInstrumentExtInfo extInfo;
            result = BAERmfEditorBank_GetInstrumentExtInfo(bankToken, i, &extInfo);
            if (result == BAE_NO_ERROR && extInfo.hasExtendedData)
            {
                printf("      ExtData: ADSR stages=%u LFOs=%u Curves=%u "
                       "LPF freq=%d res=%d amt=%d\n",
                       extInfo.volumeADSR.stageCount,
                       extInfo.lfoCount, extInfo.curveCount,
                       extInfo.LPF_frequency, extInfo.LPF_resonance,
                       extInfo.LPF_lowpassAmount);
            }
        }
    }

    if (hasTargetFilters())
    {
        int ti;
        int ts;

        for (ti = 0; ti < targetInstrumentCount; ++ti)
        {
            if (!instrumentTargetMatched[ti])
            {
                fprintf(stderr, "Error: instrument index %u was not found in the bank\n",
                        targetInstruments[ti]);
                BAEMixer_Delete(mixer);
                return 1;
            }
        }
        for (ts = 0; ts < targetSampleCount; ++ts)
        {
            if (!sampleTargetMatched[ts])
            {
                fprintf(stderr, "Error: sample target %u:%u was not found in the bank\n",
                        targetSamples[ts].instrumentIndex,
                        targetSamples[ts].sampleIndex);
                BAEMixer_Delete(mixer);
                return 1;
            }
        }
        printf("Selected SND resources: %d\n\n", selectedSndCount);
    }

    /* ── Save / Recompress ──────────────────────────────────────── */
    if (!doRecompress)
    {
        /* No recompression — simple resave using existing API */
        printf("\nSaving bank (no recompression) to: %s\n", outputPath);
        result = BAERmfEditorBank_SaveToFile(bankToken, (BAEPathName)outputPath);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: BAERmfEditorBank_SaveToFile failed (%d)\n", (int)result);
            BAEMixer_Delete(mixer);
            return 1;
        }
    }
    else
    {
        /* Recompression: manually iterate resources, recompress SND data,
         * and build a new resource file. */
        XFILE bankFile;
        XFILE outFile;
        XFILERESOURCEMAP map;
        int32_t resourceID;
        int32_t useZmf;
        uint32_t sndRecompressed;
        uint32_t sndSkipped;

        static const XResourceType sndTypes[] = { ID_SND, ID_CSND, ID_ESND, 0 };
        static const XResourceType nonSndTypes[] = {
            ID_INST, ID_ALIAS, ID_BANK, ID_SONG, ID_MIDI, ID_MIDI_OLD,
            ID_CMID, ID_EMID, ID_ECMI, ID_RMF, ID_TEXT, ID_VERS, 0
        };

        bankFile = (XFILE)bankToken;
        sndRecompressed = 0;
        sndSkipped = 0;

        /* Read source map header */
        if (XFileSetPosition(bankFile, 0L) != 0 ||
            XFileRead(bankFile, &map, (int32_t)sizeof(XFILERESOURCEMAP)) != 0)
        {
            fprintf(stderr, "Error: failed to read bank map header\n");
            BAEMixer_Delete(mixer);
            return 1;
        }

        /* Determine output format */
        useZmf = codecRequiresZsb(codec);
        if (!useZmf)
        {
            /* Preserve original format if not forced to ZSB */
            resourceID = (int32_t)XGetLong(&map.mapID);
            if (!XFILERESOURCE_ID_IS_VALID(resourceID))
            {
                fprintf(stderr, "Error: invalid bank header\n");
                BAEMixer_Delete(mixer);
                return 1;
            }
        }
        else
        {
            resourceID = XFILERESOURCE_ZMF_ID;
        }

        /* Create virtual output file */
        outFile = XFileOpenVirtualResource(resourceID);
        if (!outFile)
        {
            fprintf(stderr, "Error: failed to create virtual resource file\n");
            BAEMixer_Delete(mixer);
            return 1;
        }

        /* Copy non-SND resources verbatim */
        {
            int32_t typeIdx;
            for (typeIdx = 0; nonSndTypes[typeIdx] != 0; typeIdx++)
            {
                int32_t resCount = XCountFileResourcesOfType(bankFile, nonSndTypes[typeIdx]);
                int32_t resIdx;
                for (resIdx = 0; resIdx < resCount; resIdx++)
                {
                    XLongResourceID resID;
                    int32_t resSize;
                    char resName[256];
                    XPTR resData;

                    resName[0] = 0;
                    resData = XGetIndexedFileResource(bankFile, nonSndTypes[typeIdx],
                                                      &resID, resIdx, resName, &resSize);
                    if (!resData)
                    {
                        continue;
                    }
                    XAddFileResource(outFile, nonSndTypes[typeIdx], resID, resName,
                                     resData, resSize);
                    XDisposePtr(resData);
                }
            }
        }

        /* Process SND resources — recompress each one */
        {
            int32_t typeIdx;
            for (typeIdx = 0; sndTypes[typeIdx] != 0; typeIdx++)
            {
                int32_t resCount = XCountFileResourcesOfType(bankFile, sndTypes[typeIdx]);
                int32_t resIdx;
                for (resIdx = 0; resIdx < resCount; resIdx++)
                {
                    XLongResourceID resID;
                    int32_t resSize;
                    char resName[256];
                    XPTR resData;

                    resName[0] = 0;
                    resData = XGetIndexedFileResource(bankFile, sndTypes[typeIdx],
                                                      &resID, resIdx, resName, &resSize);
                    if (!resData)
                    {
                        continue;
                    }

                    {
                        int32_t newSize;
                        XPTR newSnd;
                        int skipMinFrames = 0;
                        int skipProgram = 0;
                        int skipTarget = 0;

                        /* Check --skip programs */
                        if (skipSndCount > 0)
                        {
                            int si;
                            for (si = 0; si < skipSndCount; si++)
                            {
                                if (skipSndIDs[si] == (int32_t)resID)
                                {
                                    skipProgram = 1;
                                    break;
                                }
                            }
                        }

                        if (selectedSndCount > 0)
                        {
                            int si;

                            skipTarget = 1;
                            for (si = 0; si < selectedSndCount; ++si)
                            {
                                if (selectedSndIDs[si] == (int32_t)resID)
                                {
                                    skipTarget = 0;
                                    break;
                                }
                            }
                        }

                        /* Check --minframes threshold before recompressing */
                        if (minFrames > 0)
                        {
                            SampleDataInfo peekInfo;
                            XSetMemory(&peekInfo, (int32_t)sizeof(peekInfo), 0);
                            if (XGetSampleInfoFromSnd(resData, &peekInfo) == 0 &&
                                peekInfo.frames < minFrames)
                            {
                                skipMinFrames = 1;
                            }
                        }

                        {
                            SampleDataInfo nameInfo;
                            XSetMemory(&nameInfo, (int32_t)sizeof(nameInfo), 0);
                            XGetSampleInfoFromSnd(resData, &nameInfo);
                            printf("  Recompressing SND ID=%d (%s)...",
                                   (int)resID, compressionName(nameInfo.compressionType, 0));
                        }
                        fflush(stdout);

                        if (skipMinFrames || skipProgram || skipTarget)
                        {
                            newSnd = NULL;
                        }
                        else
                        {
                            newSnd = recompressSnd(resData, resSize,
                                                  targetComp, targetSub,
                                                  &newSize);
                        }
                        if (newSnd)
                        {
                            /* Write recompressed SND as ID_SND (canonical type) */
                            XAddFileResource(outFile, ID_SND, resID, resName,
                                             newSnd, newSize);
                            XDisposePtr(newSnd);
                            sndRecompressed++;
                            printf(" OK (%d -> %d bytes)\n", resSize, newSize);
                        }
                        else
                        {
                            /* Failed to recompress or skipped — keep original */
                            XAddFileResource(outFile, sndTypes[typeIdx], resID, resName,
                                             resData, resSize);
                            sndSkipped++;
                            if (skipProgram)
                            {
                                printf(" SKIPPED (program excluded)\n");
                            }
                            else if (skipTarget)
                            {
                                printf(" SKIPPED (not targeted)\n");
                            }
                            else if (skipMinFrames)
                            {
                                printf(" SKIPPED (frames < %u)\n", minFrames);
                            }
                            else
                            {
                                printf(" SKIPPED (kept original)\n");
                            }
                        }
                    }

                    XDisposePtr(resData);
                }
            }
        }

        /* Finalize */
        if (XCleanResourceFile(outFile) == FALSE)
        {
            fprintf(stderr, "Error: XCleanResourceFile failed\n");
            XFileClose(outFile);
            BAEMixer_Delete(mixer);
            return 1;
        }

        /* Extract and write to disk */
        {
            XPTR outData = NULL;
            int32_t outSize = 0;
            XFILENAME xname;
            XFILE diskFile;

            if (XFileGetMemoryFileAsData(outFile, &outData, &outSize) != 0 ||
                !outData || outSize <= 0)
            {
                fprintf(stderr, "Error: failed to serialize output bank\n");
                XFileClose(outFile);
                if (outData)
                {
                    XDisposePtr(outData);
                }
                BAEMixer_Delete(mixer);
                return 1;
            }
            XFileClose(outFile);

            XConvertPathToXFILENAME((BAEPathName)outputPath, &xname);
            diskFile = XFileOpenForWrite(&xname, TRUE);
            if (!diskFile)
            {
                fprintf(stderr, "Error: failed to open output file '%s' for writing\n",
                        outputPath);
                XDisposePtr(outData);
                BAEMixer_Delete(mixer);
                return 1;
            }
            if (XFileSetLength(diskFile, 0) != 0 ||
                XFileSetPosition(diskFile, 0L) != 0 ||
                XFileWrite(diskFile, outData, outSize) != 0)
            {
                fprintf(stderr, "Error: failed to write output file\n");
                XFileClose(diskFile);
                XDisposePtr(outData);
                BAEMixer_Delete(mixer);
                return 1;
            }
            XFileClose(diskFile);
            XDisposePtr(outData);
        }

        printf("\nRecompression complete: %u samples recompressed, %u skipped\n",
               sndRecompressed, sndSkipped);
    }

    printf("Bank saved to: %s\n", outputPath);

    /* Verify: load the saved bank and check instrument count */
    {
        BAEBankToken verifyToken;
        uint32_t verifyCount;

        result = BAEMixer_AddBankFromFile(mixer, (BAEPathName)outputPath, &verifyToken);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: Could not reload saved bank for verification (%d)\n",
                    (int)result);
        }
        else
        {
            result = BAERmfEditorBank_GetInstrumentCount(verifyToken, &verifyCount);
            if (result == BAE_NO_ERROR)
            {
                printf("Verification: saved bank has %u instruments (original: %u) %s %s\n",
                       verifyCount, instCount,
                       (verifyCount == instCount) ? "PASS" : "MISMATCH",
                       (verifyCount == instCount) ? "" : "!!!");
            }
        }
    }

    BAEMixer_Delete(mixer);
    return 0;
}
