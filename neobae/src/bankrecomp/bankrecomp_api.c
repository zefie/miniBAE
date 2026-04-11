/****************************************************************************
 *
 * bankrecomp_api.c
 *
 * Bank recompression utility (BAERmfEditor API path).
 * Loads a bank file (.hsb/.zsb), optionally recompresses sample data
 * through BAERmfEditorBank_ReEncodeSample, and writes the result.
 *
 ****************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <NeoBAE.h>
#include <BAE_API.h>

#define MAX_SKIP_PROGRAMS 256
#define MAX_TARGET_INSTRUMENTS 1024
#define MAX_TARGET_SAMPLES 4096
#define MAX_TARGET_SNDS 4096
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct BankTargetSample
{
    uint32_t instrumentIndex;
    uint32_t sampleIndex;
} BankTargetSample;

enum
{
    CODEC_PCM = 0,
    CODEC_ADPCM = 1,
    CODEC_ALAW = 2,
    CODEC_ULAW = 3,
    CODEC_MP3 = 4,
    CODEC_VORBIS = 5,
    CODEC_FLAC = 6,
    CODEC_OPUS = 7,
#if USE_QOA_SUPPORT == TRUE
    CODEC_QOA = 8,
    CODEC_COUNT = 9
#else
    CODEC_COUNT = 8
#endif
};

static const char *codecNames[CODEC_COUNT] = {
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

typedef struct CompressionEntry
{
    uint32_t kbps;
    BAERmfEditorCompressionType compression;
} CompressionEntry;

static const CompressionEntry mp3CompressionTable[] = {
    {32, BAE_EDITOR_COMPRESSION_MP3_32K},
    {48, BAE_EDITOR_COMPRESSION_MP3_48K},
    {64, BAE_EDITOR_COMPRESSION_MP3_64K},
    {96, BAE_EDITOR_COMPRESSION_MP3_96K},
    {128, BAE_EDITOR_COMPRESSION_MP3_128K},
    {192, BAE_EDITOR_COMPRESSION_MP3_192K},
    {256, BAE_EDITOR_COMPRESSION_MP3_256K},
    {320, BAE_EDITOR_COMPRESSION_MP3_320K}
};

static const CompressionEntry vorbisCompressionTable[] = {
    {32, BAE_EDITOR_COMPRESSION_VORBIS_32K},
    {48, BAE_EDITOR_COMPRESSION_VORBIS_48K},
    {64, BAE_EDITOR_COMPRESSION_VORBIS_64K},
    {80, BAE_EDITOR_COMPRESSION_VORBIS_80K},
    {96, BAE_EDITOR_COMPRESSION_VORBIS_96K},
    {128, BAE_EDITOR_COMPRESSION_VORBIS_128K},
    {160, BAE_EDITOR_COMPRESSION_VORBIS_160K},
    {192, BAE_EDITOR_COMPRESSION_VORBIS_192K},
    {256, BAE_EDITOR_COMPRESSION_VORBIS_256K}
};

static const CompressionEntry opusCompressionTable[] = {
    {12, BAE_EDITOR_COMPRESSION_OPUS_12K},
    {16, BAE_EDITOR_COMPRESSION_OPUS_16K},
    {24, BAE_EDITOR_COMPRESSION_OPUS_24K},
    {32, BAE_EDITOR_COMPRESSION_OPUS_32K},
    {48, BAE_EDITOR_COMPRESSION_OPUS_48K},
    {64, BAE_EDITOR_COMPRESSION_OPUS_64K},
    {80, BAE_EDITOR_COMPRESSION_OPUS_80K},
    {96, BAE_EDITOR_COMPRESSION_OPUS_96K},
    {128, BAE_EDITOR_COMPRESSION_OPUS_128K},
    {160, BAE_EDITOR_COMPRESSION_OPUS_160K},
    {192, BAE_EDITOR_COMPRESSION_OPUS_192K},
    {256, BAE_EDITOR_COMPRESSION_OPUS_256K}
};

static uint32_t skipPrograms[MAX_SKIP_PROGRAMS];
static int skipProgramCount = 0;
static uint32_t targetInstruments[MAX_TARGET_INSTRUMENTS];
static int targetInstrumentCount = 0;
static BankTargetSample targetSamples[MAX_TARGET_SAMPLES];
static int targetSampleCount = 0;

static void parseSkipPrograms(const char *str)
{
    const char *p = str;

    skipProgramCount = 0;
    while (*p && skipProgramCount < MAX_SKIP_PROGRAMS)
    {
        while (*p == ',' || *p == ' ')
        {
            ++p;
        }
        if (*p == '\0')
        {
            break;
        }
        skipPrograms[skipProgramCount++] = (uint32_t)atol(p);
        while (*p && *p != ',')
        {
            ++p;
        }
    }
}

static int isSkippedProgram(uint32_t program)
{
    int i;

    for (i = 0; i < skipProgramCount; ++i)
    {
        if (skipPrograms[i] == program)
        {
            return 1;
        }
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

static uint32_t parseBitrate(const char *str)
{
    uint32_t val = (uint32_t)atol(str);

    if (val >= 1000)
    {
        val /= 1000;
    }
    return val;
}

static int codecRequiresZsb(int codec)
{
    return (codec == CODEC_VORBIS || codec == CODEC_FLAC ||
            codec == CODEC_OPUS
#if USE_QOA_SUPPORT == TRUE
            || codec == CODEC_QOA
#endif
            );
}

static const char *sndStorageName(BAERmfEditorSndStorageType sndStorageType)
{
    switch (sndStorageType)
    {
        case BAE_EDITOR_SND_STORAGE_SND:
            return "SND";
        case BAE_EDITOR_SND_STORAGE_CSND:
            return "CSND";
        case BAE_EDITOR_SND_STORAGE_ESND:
        default:
            return "ESND";
    }
}

static int parseSndStorageType(const char *str, BAERmfEditorSndStorageType *outType)
{
    if (!str || !outType)
    {
        return 0;
    }

    if (strcasecmp(str, "SND") == 0)
    {
        *outType = BAE_EDITOR_SND_STORAGE_SND;
        return 1;
    }
    if (strcasecmp(str, "CSND") == 0)
    {
        *outType = BAE_EDITOR_SND_STORAGE_CSND;
        return 1;
    }
    if (strcasecmp(str, "ESND") == 0)
    {
        *outType = BAE_EDITOR_SND_STORAGE_ESND;
        return 1;
    }

    return 0;
}

static const char *compressionName(XResourceType ct, uint32_t subType)
{
    static char combined[48];
    const char *base = NULL;
    const char *bitrate = NULL;

    switch ((uint32_t)ct)
    {
        case 0:
            return "PCM";
        case FOUR_CHAR('n','o','n','e'):
            return "PCM";
        case FOUR_CHAR('i','m','a','4'):
        case FOUR_CHAR('i','m','a','W'):
            return "IMA4/ADPCM";
        case FOUR_CHAR('u','l','a','w'):
            return "uLaw";
        case FOUR_CHAR('a','l','a','w'):
            return "aLaw";
        case FOUR_CHAR('m','p','g','n'):
            return "MP3 (32k)";
        case FOUR_CHAR('m','p','g','b'):
            return "MP3 (48k)";
        case FOUR_CHAR('m','p','g','d'):
            return "MP3 (64k)";
        case FOUR_CHAR('m','p','g','f'):
            return "MP3 (96k)";
        case FOUR_CHAR('m','p','g','h'):
            return "MP3 (128k)";
        case FOUR_CHAR('m','p','g','j'):
            return "MP3 (192k)";
        case FOUR_CHAR('m','p','g','l'):
            return "MP3 (256k)";
        case FOUR_CHAR('m','p','g','m'):
            return "MP3 (320k)";
        case FOUR_CHAR('f','L','a','C'):
            return "FLAC";
#if USE_QOA_SUPPORT == TRUE
        case FOUR_CHAR('q','o','a','f'):
            return "QOA";
#endif
        case FOUR_CHAR('O','g','g','V'):
            base = "Vorbis";
            break;
        case FOUR_CHAR('O','g','g','O'):
            base = "Opus";
            break;
        default:
            return "Unknown";
    }

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

static void printHelp(const char *progName)
{
    printf("Usage: %s [options] <input.hsb|zsb> <output.hsb|zsb>\n\n", progName);
    printf("Recompresses bank samples through BAERmfEditor API.\n");
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
    printf("  --sndstorage T Output storage for recompressed samples: ESND, CSND, SND\n");
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
    printf("\nNote: VORBIS, FLAC, OPUS, QOA codecs force ZSB (ZREZ) output format.\n");
#else
    printf("\nNote: VORBIS, FLAC, OPUS codecs force ZSB (ZREZ) output format.\n");
#endif
    printf("      Without --codec, the bank is resaved with original compression.\n");
}

static BAERmfEditorCompressionType chooseClosestBitrate(const CompressionEntry *table,
                                                        size_t tableCount,
                                                        uint32_t targetKbps,
                                                        uint32_t *outChosenKbps)
{
    size_t i;
    size_t best = 0;
    uint32_t bestDiff;

    bestDiff = (table[0].kbps > targetKbps) ? (table[0].kbps - targetKbps) : (targetKbps - table[0].kbps);
    for (i = 1; i < tableCount; ++i)
    {
        uint32_t diff = (table[i].kbps > targetKbps) ? (table[i].kbps - targetKbps) : (targetKbps - table[i].kbps);
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = i;
        }
    }

    if (outChosenKbps)
    {
        *outChosenKbps = table[best].kbps;
    }
    return table[best].compression;
}

static int resolveTargetCompression(int codec,
                                    uint32_t bitrateKbps,
                                    BAERmfEditorCompressionType *outCompression,
                                    uint32_t *outChosenKbps)
{
    if (!outCompression)
    {
        return 0;
    }

    switch (codec)
    {
        case CODEC_PCM:
            *outCompression = BAE_EDITOR_COMPRESSION_PCM;
            if (outChosenKbps) *outChosenKbps = 0;
            return 1;
        case CODEC_ADPCM:
            *outCompression = BAE_EDITOR_COMPRESSION_ADPCM;
            if (outChosenKbps) *outChosenKbps = 0;
            return 1;
        case CODEC_ALAW:
            *outCompression = BAE_EDITOR_COMPRESSION_ALAW;
            if (outChosenKbps) *outChosenKbps = 0;
            return 1;
        case CODEC_ULAW:
            *outCompression = BAE_EDITOR_COMPRESSION_ULAW;
            if (outChosenKbps) *outChosenKbps = 0;
            return 1;
        case CODEC_FLAC:
            *outCompression = BAE_EDITOR_COMPRESSION_FLAC;
            if (outChosenKbps) *outChosenKbps = 0;
            return 1;
#if USE_QOA_SUPPORT == TRUE
        case CODEC_QOA:
            *outCompression = BAE_EDITOR_COMPRESSION_QOA;
            if (outChosenKbps) *outChosenKbps = 0;
            return 1;
#endif
        case CODEC_MP3:
            *outCompression = chooseClosestBitrate(mp3CompressionTable,
                                                   ARRAY_COUNT(mp3CompressionTable),
                                                   (bitrateKbps > 0) ? bitrateKbps : 128,
                                                   outChosenKbps);
            return 1;
        case CODEC_VORBIS:
            *outCompression = chooseClosestBitrate(vorbisCompressionTable,
                                                   ARRAY_COUNT(vorbisCompressionTable),
                                                   (bitrateKbps > 0) ? bitrateKbps : 128,
                                                   outChosenKbps);
            return 1;
        case CODEC_OPUS:
            *outCompression = chooseClosestBitrate(opusCompressionTable,
                                                   ARRAY_COUNT(opusCompressionTable),
                                                   (bitrateKbps > 0) ? bitrateKbps : 48,
                                                   outChosenKbps);
            return 1;
        default:
            return 0;
    }
}

static int isTargetInstrument(uint32_t instrumentIndex, int *matchIndex)
{
    int i;

    for (i = 0; i < targetInstrumentCount; ++i)
    {
        if (targetInstruments[i] == instrumentIndex)
        {
            if (matchIndex)
            {
                *matchIndex = i;
            }
            return 1;
        }
    }

    return 0;
}

static int isTargetSample(uint32_t instrumentIndex, uint32_t sampleIndex, int *matchIndex)
{
    int i;

    for (i = 0; i < targetSampleCount; ++i)
    {
        if (targetSamples[i].instrumentIndex == instrumentIndex &&
            targetSamples[i].sampleIndex == sampleIndex)
        {
            if (matchIndex)
            {
                *matchIndex = i;
            }
            return 1;
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    BAEMixer mixer;
    BAEBankToken bankToken;
    BAEResult result;
    const char *inputPath = NULL;
    const char *outputPath = NULL;
    int codec = -1;
    uint32_t bitrateKbps = 0;
    uint32_t chosenBitrate = 0;
    uint32_t minFrames = 0;
    BAERmfEditorCompressionType targetCompression = BAE_EDITOR_COMPRESSION_DONT_CHANGE;
    BAERmfEditorSndStorageType sndStorageType = BAE_EDITOR_SND_STORAGE_ESND;
    BAERmfEditorOpusMode opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
    int doRecompress;
    uint32_t instCount;
    uint32_t i;
    int argIdx;
    int instrumentTargetMatched[MAX_TARGET_INSTRUMENTS];
    int sampleTargetMatched[MAX_TARGET_SAMPLES];
    uint32_t processedSndIDs[MAX_TARGET_SNDS];
    int processedSndCount;
    uint32_t reencodedCount;
    uint32_t skippedCount;

    for (argIdx = 1; argIdx < argc; ++argIdx)
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
                fprintf(stderr, "Error: invalid codec number %d (valid: 0-%d)\n", codec, CODEC_COUNT - 1);
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
                fprintf(stderr, "Error: invalid --sample list '%s' (expected I:S[,I:S...])\n", argv[argIdx]);
                return 1;
            }
        }
        else if (strcmp(argv[argIdx], "--sndstorage") == 0)
        {
            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --sndstorage requires a value: ESND, CSND, or SND\n");
                return 1;
            }
            if (!parseSndStorageType(argv[++argIdx], &sndStorageType))
            {
                fprintf(stderr, "Error: invalid --sndstorage value '%s' (expected ESND, CSND, or SND)\n", argv[argIdx]);
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

    doRecompress = (codec >= 0);

    if (!doRecompress && hasTargetFilters())
    {
        fprintf(stderr, "Error: --instrument/--sample require --codec\n");
        return 1;
    }
    if (!doRecompress && sndStorageType != BAE_EDITOR_SND_STORAGE_ESND)
    {
        fprintf(stderr, "Error: --sndstorage requires --codec\n");
        return 1;
    }

    if (doRecompress)
    {
        if (!resolveTargetCompression(codec, bitrateKbps, &targetCompression, &chosenBitrate))
        {
            fprintf(stderr, "Error: failed to resolve target compression for codec %d\n", codec);
            return 1;
        }

        if (codecRequiresZsb(codec))
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

        printf("Recompression target: %s", codecNames[codec]);
        if (chosenBitrate > 0)
        {
            printf(" @ %u kbps", chosenBitrate);
        }
        printf("\n");
        printf("Recompressed SND storage: %s\n", sndStorageName(sndStorageType));
    }

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

    result = BAEMixer_AddBankFromFile(mixer, (BAEPathName)inputPath, &bankToken);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAEMixer_AddBankFromFile failed (%d) for '%s'\n", (int)result, inputPath);
        BAEMixer_Delete(mixer);
        return 1;
    }

    printf("Loaded bank: %s\n", inputPath);

    result = BAERmfEditorBank_GetInstrumentCount(bankToken, &instCount);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: GetInstrumentCount failed (%d)\n", (int)result);
        BAEMixer_Delete(mixer);
        return 1;
    }

    printf("Instrument count: %u\n\n", instCount);

    XSetMemory(instrumentTargetMatched, (int32_t)sizeof(instrumentTargetMatched), 0);
    XSetMemory(sampleTargetMatched, (int32_t)sizeof(sampleTargetMatched), 0);
    processedSndCount = 0;
    reencodedCount = 0;
    skippedCount = 0;

    for (i = 0; i < instCount; ++i)
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
               i,
               instInfo.instID,
               instInfo.bank,
               instInfo.program,
               instInfo.keySplitCount,
               instInfo.name);

        result = BAERmfEditorBank_GetInstrumentSampleCount(bankToken, i, &sampleCount);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "      GetInstrumentSampleCount failed (%d)\n", (int)result);
            continue;
        }

        for (s = 0; s < sampleCount; ++s)
        {
            BAERmfEditorBankSampleInfo sampleInfo;
            int instrumentMatchIdx = -1;
            int sampleMatchIdx = -1;
            int targetHit;
            int skipReason = 0;
            int sharedAlready = 0;
            int si;

            result = BAERmfEditorBank_GetInstrumentSampleInfo(bankToken, i, s, &sampleInfo);
            if (result != BAE_NO_ERROR)
            {
                fprintf(stderr, "      Sample[%u] GetInstrumentSampleInfo failed (%d)\n", s, (int)result);
                continue;
            }

            printf("      Sample[%u]: SndID=%d Keys=%u-%u Root=%u Rate=%uHz "
                   "Frames=%u %dbit %dch Loop=%u-%u Codec=%s\n",
                   s,
                   (int)sampleInfo.sndResourceID,
                   sampleInfo.lowKey,
                   sampleInfo.highKey,
                   sampleInfo.rootKey,
                   sampleInfo.sampleRate,
                   sampleInfo.frameCount,
                   sampleInfo.bitDepth,
                   sampleInfo.channels,
                   sampleInfo.loopStart,
                   sampleInfo.loopEnd,
                   compressionName(sampleInfo.compressionType, sampleInfo.compressionSubType));

            if (isTargetInstrument(i, &instrumentMatchIdx))
            {
                instrumentTargetMatched[instrumentMatchIdx] = 1;
            }
            if (isTargetSample(i, s, &sampleMatchIdx))
            {
                sampleTargetMatched[sampleMatchIdx] = 1;
            }

            if (!doRecompress)
            {
                continue;
            }

            targetHit = !hasTargetFilters() ||
                        isTargetInstrument(i, NULL) ||
                        isTargetSample(i, s, NULL);

            if (skipProgramCount > 0 && isSkippedProgram(instInfo.program))
            {
                skipReason = 1;
            }
            else if (!targetHit)
            {
                skipReason = 2;
            }
            else if (minFrames > 0 && sampleInfo.frameCount < minFrames)
            {
                skipReason = 3;
            }

            for (si = 0; si < processedSndCount; ++si)
            {
                if (processedSndIDs[si] == (uint32_t)sampleInfo.sndResourceID)
                {
                    sharedAlready = 1;
                    break;
                }
            }

            if (!skipReason && sharedAlready)
            {
                skipReason = 4;
            }

            if (skipReason)
            {
                ++skippedCount;
                switch (skipReason)
                {
                    case 1:
                        printf("        SKIPPED (program excluded)\n");
                        break;
                    case 2:
                        printf("        SKIPPED (not targeted)\n");
                        break;
                    case 3:
                        printf("        SKIPPED (frames < %u)\n", minFrames);
                        break;
                    case 4:
                    default:
                        printf("        SKIPPED (shared SndID already processed)\n");
                        break;
                }
                continue;
            }

            result = BAERmfEditorBank_ReEncodeSample(bankToken,
                                                     i,
                                                     s,
                                                     targetCompression,
                                                     sndStorageType,
                                                     opusMode);
            if (result != BAE_NO_ERROR)
            {
                ++skippedCount;
                printf("        SKIPPED (re-encode failed: %d)\n", (int)result);
                continue;
            }

            if (!addUniqueUint32(processedSndIDs,
                                 &processedSndCount,
                                 MAX_TARGET_SNDS,
                                 (uint32_t)sampleInfo.sndResourceID))
            {
                fprintf(stderr, "Error: too many processed SND IDs\n");
                BAEMixer_Delete(mixer);
                return 1;
            }

            ++reencodedCount;
            printf("        OK\n");
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
                fprintf(stderr, "Error: instrument index %u was not found in the bank\n", targetInstruments[ti]);
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
    }

    if (doRecompress)
    {
        printf("\nRe-encode complete: %u samples recompressed, %u skipped\n",
               reencodedCount,
               skippedCount);
    }

    printf("Saving bank to: %s\n", outputPath);
    result = BAERmfEditorBank_SaveToFile(bankToken, (BAEPathName)outputPath);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAERmfEditorBank_SaveToFile failed (%d)\n", (int)result);
        BAEMixer_Delete(mixer);
        return 1;
    }

    printf("Bank saved to: %s\n", outputPath);

    {
        BAEBankToken verifyToken;
        uint32_t verifyCount;

        result = BAEMixer_AddBankFromFile(mixer, (BAEPathName)outputPath, &verifyToken);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: Could not reload saved bank for verification (%d)\n", (int)result);
        }
        else
        {
            result = BAERmfEditorBank_GetInstrumentCount(verifyToken, &verifyCount);
            if (result == BAE_NO_ERROR)
            {
                printf("Verification: saved bank has %u instruments (original: %u) %s %s\n",
                       verifyCount,
                       instCount,
                       (verifyCount == instCount) ? "PASS" : "MISMATCH",
                       (verifyCount == instCount) ? "" : "!!!");
            }
        }
    }

    BAEMixer_Delete(mixer);
    return 0;
}
