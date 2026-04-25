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
#include <math.h>
#include <NeoBAE.h>
#include <BAE_API.h>

#define MAX_SKIP_PROGRAMS 256
#define MAX_TARGET_INSTRUMENTS 1024
#define MAX_TARGET_SAMPLES 4096
#define MAX_TARGET_SNDS 4096
#define MAX_OVERRIDES 4096
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef struct BankTargetSample
{
    uint32_t instrumentIndex;
    uint32_t sampleIndex;
} BankTargetSample;

typedef struct SndOverride
{
    uint32_t sndID;
    int      codec;    /* -1 = not set */
    uint32_t bitrate;  /* 0  = not set */
} SndOverride;

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
static SndOverride sndOverrides[MAX_OVERRIDES];
static int sndOverrideCount = 0;

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

static int addOrUpdateOverride(uint32_t sndID, int codec, uint32_t bitrate)
{
    int i;

    for (i = 0; i < sndOverrideCount; ++i)
    {
        if (sndOverrides[i].sndID == sndID)
        {
            if (codec >= 0)    sndOverrides[i].codec   = codec;
            if (bitrate > 0)   sndOverrides[i].bitrate = bitrate;
            return 1;
        }
    }
    if (sndOverrideCount >= MAX_OVERRIDES)
    {
        return 0;
    }
    sndOverrides[sndOverrideCount].sndID   = sndID;
    sndOverrides[sndOverrideCount].codec   = codec;
    sndOverrides[sndOverrideCount].bitrate = bitrate;
    ++sndOverrideCount;
    return 1;
}

static const SndOverride *findSndOverride(uint32_t sndID)
{
    int i;

    for (i = 0; i < sndOverrideCount; ++i)
    {
        if (sndOverrides[i].sndID == sndID)
        {
            return &sndOverrides[i];
        }
    }
    return NULL;
}

/* Parse "sndID:codecID[:bitrate]" from the command line */
static int parseOverrideCli(const char *str)
{
    char *end1, *end2, *end3;
    unsigned long sndID, codecID;
    uint32_t bitrate = 0;

    sndID = strtoul(str, &end1, 10);
    if (end1 == str || *end1 != ':')
    {
        return 0;
    }
    codecID = strtoul(end1 + 1, &end2, 10);
    if (end2 == end1 + 1)
    {
        return 0;
    }
    if (*end2 == ':')
    {
        unsigned long br = strtoul(end2 + 1, &end3, 10);
        if (end3 == end2 + 1 || *end3 != '\0')
        {
            return 0;
        }
        bitrate = parseBitrate(end2 + 1);
    }
    else if (*end2 != '\0')
    {
        return 0;
    }
    if ((int)codecID < 0 || (int)codecID >= CODEC_COUNT)
    {
        fprintf(stderr, "Error: invalid codec %lu in override for SndID %lu\n", codecID, sndID);
        return 0;
    }
    return addOrUpdateOverride((uint32_t)sndID, (int)codecID, bitrate);
}

/* Parse a simple INI file:
 *   [sndID]
 *   codec=N
 *   bitrate=N
 */
static int parseOverrideFile(const char *path)
{
    FILE *f;
    char line[512];
    uint32_t curSndID = 0;
    int curCodec = -1;
    uint32_t curBitrate = 0;
    int inSection = 0;

    f = fopen(path, "r");
    if (!f)
    {
        fprintf(stderr, "Error: cannot open override file '%s'\n", path);
        return 0;
    }

    while (fgets(line, (int)sizeof(line), f))
    {
        char *p = line;
        size_t len;

        /* Trim leading whitespace */
        while (*p == ' ' || *p == '\t') ++p;
        /* Trim trailing whitespace/newline */
        len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' ||
                           p[len-1] == ' '  || p[len-1] == '\t'))
        {
            p[--len] = '\0';
        }

        if (*p == '\0' || *p == ';' || *p == '#')
        {
            continue;
        }

        if (*p == '[')
        {
            char *close;
            char *end;

            /* Commit previous section */
            if (inSection && curCodec >= 0)
            {
                if (!addOrUpdateOverride(curSndID, curCodec, curBitrate))
                {
                    fprintf(stderr, "Warning: too many overrides, skipping SndID %u\n", curSndID);
                }
            }

            close = strchr(p + 1, ']');
            if (!close)
            {
                fprintf(stderr, "Error: malformed section in override file: %s\n", p);
                fclose(f);
                return 0;
            }
            *close = '\0';
            curSndID  = (uint32_t)strtoul(p + 1, &end, 10);
            curCodec  = -1;
            curBitrate = 0;
            inSection = 1;
        }
        else if (inSection)
        {
            char *eq = strchr(p, '=');
            char *key, *val;
            size_t klen;

            if (!eq)
            {
                continue;
            }
            *eq = '\0';
            key  = p;
            val  = eq + 1;

            /* Trim key trailing whitespace */
            klen = strlen(key);
            while (klen > 0 && (key[klen-1] == ' ' || key[klen-1] == '\t'))
            {
                key[--klen] = '\0';
            }
            /* Trim val leading whitespace */
            while (*val == ' ' || *val == '\t') ++val;

            if (strcasecmp(key, "codec") == 0)
            {
                int c = atoi(val);
                if (c < 0 || c >= CODEC_COUNT)
                {
                    fprintf(stderr, "Warning: invalid codec %d for SndID %u in override file\n",
                            c, curSndID);
                }
                else
                {
                    curCodec = c;
                }
            }
            else if (strcasecmp(key, "bitrate") == 0)
            {
                curBitrate = parseBitrate(val);
            }
        }
    }

    /* Commit last section */
    if (inSection && curCodec >= 0)
    {
        if (!addOrUpdateOverride(curSndID, curCodec, curBitrate))
        {
            fprintf(stderr, "Warning: too many overrides, skipping SndID %u\n", curSndID);
        }
    }

    fclose(f);
    return 1;
}

/* Dispatch: "help" → print format docs; string with ':' -> CLI format; else -> file */
static int parseOverride(const char *str)
{
    if (strcmp(str, "help") == 0)
    {
        printf("--override help\n\n");
        printf("Override the codec (and optional bitrate) for specific SND resource IDs.\n");
        printf("Can be specified multiple times on the command line or via an INI file.\n\n");
        printf("CLI format:  --override sndID:codecID[:bitrate]\n");
        printf("  Example:   --override 23:4        (SndID 23 -> MP3 default bitrate)\n");
        printf("  Example:   --override 24:0        (SndID 24 -> PCM)\n");
        printf("  Example:   --override 26:7:80     (SndID 26 -> Opus 80 kbps)\n\n");
        printf("File format: --override overrides.ini   (or .txt)\n\n");
        printf("  INI syntax:\n");
        printf("    [sndID]\n");
        printf("    codec=N\n");
        printf("    bitrate=N   ; optional\n\n");
        printf("  Example overrides.ini:\n");
        printf("    [23]\n");
        printf("    codec=4\n\n");
        printf("    [24]\n");
        printf("    codec=0\n\n");
        printf("    [26]\n");
        printf("    codec=7\n");
        printf("    bitrate=80\n\n");
        printf("Codec numbers match --codec values (see --help for the full list).\n");
        return -1; /* sentinel: printed help, caller should exit 0 */
    }

    /* Decide: if the string contains ':' treat as CLI sndID:codec[:bitrate] */
    if (strchr(str, ':'))
    {
        if (!parseOverrideCli(str))
        {
            fprintf(stderr, "Error: invalid --override value '%s' (expected sndID:codecID[:bitrate])\n", str);
            return 0;
        }
        return 1;
    }

    /* Otherwise treat as a file path */
    return parseOverrideFile(str) ? 1 : 0;
}

static void printHelp(const char *progName)
{
    printf("Usage: %s [options] <input.hsb|zsb> <output.hsb|zsb>\n\n", progName);
    printf("Recompresses bank samples and/or applies gain through BAERmfEditor API.\n");
    printf("Without target filters, all samples are processed.\n\n");
    printf("Options:\n");
    printf("  --codec N      Target codec number (see list below)\n");
    printf("  --bitrate N    Target bitrate in kbps (e.g. 128) or bps (e.g. 128000)\n");
    printf("  --sgain DB     Apply sample gain in dB before compression (e.g. -3.0, 6.0)\n");
    printf("  --igain SCALE  Multiply each instrument split's volume by SCALE (0.0-2.0, 1.0 = unchanged)\n");
    printf("  --minframes N  Skip recompression for samples with fewer than N frames\n");
    printf("  --skip P,P,... Skip recompression for listed program numbers\n");
    printf("  --instrument I[,I...]\n");
    printf("                 Process only listed bank instrument indices\n");
    printf("  --sample I:S[,I:S...]\n");
    printf("                 Process only listed instrument sample slots\n");
    printf("  --sndstorage T Output storage for processed samples: ESND, CSND, SND\n");
    printf("  --override V   Per-SND codec override: sndID:codecID[:bitrate], file.ini,\n");
    printf("                 or 'help' for full format documentation\n");
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
    printf("      Without --codec/--sgain/--igain, the bank is resaved unchanged.\n");
}

static int applyPcmGainInPlace(void *pcmData,
                               uint32_t frameCount,
                               uint16_t channels,
                               uint16_t bitSize,
                               double linearGain)
{
    uint32_t totalSamples;
    uint32_t i;

    if (!pcmData)
    {
        return 0;
    }
    if ((bitSize != 8 && bitSize != 16) ||
        (channels != 1 && channels != 2) ||
        frameCount == 0)
    {
        return 0;
    }

    totalSamples = frameCount * (uint32_t)channels;

    if (bitSize == 8)
    {
        uint8_t *data = (uint8_t *)pcmData;
        for (i = 0; i < totalSamples; ++i)
        {
            int centered = (int)data[i] - 128;
            int scaled = (centered >= 0)
                       ? (int)(centered * linearGain + 0.5)
                       : (int)(centered * linearGain - 0.5);
            int out = scaled + 128;
            if (out < 0)
            {
                out = 0;
            }
            if (out > 255)
            {
                out = 255;
            }
            data[i] = (uint8_t)out;
        }
    }
    else
    {
        int16_t *data = (int16_t *)pcmData;
        for (i = 0; i < totalSamples; ++i)
        {
            int32_t scaled;
            if (data[i] >= 0)
            {
                scaled = (int32_t)(data[i] * linearGain + 0.5);
            }
            else
            {
                scaled = (int32_t)(data[i] * linearGain - 0.5);
            }
            if (scaled < -32768)
            {
                scaled = -32768;
            }
            if (scaled > 32767)
            {
                scaled = 32767;
            }
            data[i] = (int16_t)scaled;
        }
    }

    return 1;
}

static int inferCompressionFromSampleInfo(const BAERmfEditorBankSampleInfo *sampleInfo,
                                          BAERmfEditorCompressionType *outCompression)
{
    if (!sampleInfo || !outCompression)
    {
        return 0;
    }

    switch ((uint32_t)sampleInfo->compressionType)
    {
        case 0:
        case FOUR_CHAR('n','o','n','e'):
            *outCompression = BAE_EDITOR_COMPRESSION_PCM;
            return 1;
        case FOUR_CHAR('i','m','a','4'):
        case FOUR_CHAR('i','m','a','W'):
            *outCompression = BAE_EDITOR_COMPRESSION_ADPCM;
            return 1;
        case FOUR_CHAR('u','l','a','w'):
            *outCompression = BAE_EDITOR_COMPRESSION_ULAW;
            return 1;
        case FOUR_CHAR('a','l','a','w'):
            *outCompression = BAE_EDITOR_COMPRESSION_ALAW;
            return 1;
        case FOUR_CHAR('f','L','a','C'):
            *outCompression = BAE_EDITOR_COMPRESSION_FLAC;
            return 1;
#if USE_QOA_SUPPORT == TRUE
        case FOUR_CHAR('q','o','a','f'):
            *outCompression = BAE_EDITOR_COMPRESSION_QOA;
            return 1;
#endif
        case FOUR_CHAR('m','p','g','n'):
            *outCompression = BAE_EDITOR_COMPRESSION_MP3_32K;
            return 1;
        case FOUR_CHAR('m','p','g','b'):
            *outCompression = BAE_EDITOR_COMPRESSION_MP3_48K;
            return 1;
        case FOUR_CHAR('m','p','g','d'):
            *outCompression = BAE_EDITOR_COMPRESSION_MP3_64K;
            return 1;
        case FOUR_CHAR('m','p','g','f'):
            *outCompression = BAE_EDITOR_COMPRESSION_MP3_96K;
            return 1;
        case FOUR_CHAR('m','p','g','h'):
            *outCompression = BAE_EDITOR_COMPRESSION_MP3_128K;
            return 1;
        case FOUR_CHAR('m','p','g','j'):
            *outCompression = BAE_EDITOR_COMPRESSION_MP3_192K;
            return 1;
        case FOUR_CHAR('m','p','g','l'):
            *outCompression = BAE_EDITOR_COMPRESSION_MP3_256K;
            return 1;
        case FOUR_CHAR('m','p','g','m'):
            *outCompression = BAE_EDITOR_COMPRESSION_MP3_320K;
            return 1;
        case FOUR_CHAR('O','g','g','V'):
            switch ((uint32_t)sampleInfo->compressionSubType)
            {
                case FOUR_CHAR('v','0','3','2'): *outCompression = BAE_EDITOR_COMPRESSION_VORBIS_32K;  return 1;
                case FOUR_CHAR('v','0','4','8'): *outCompression = BAE_EDITOR_COMPRESSION_VORBIS_48K;  return 1;
                case FOUR_CHAR('v','0','6','4'): *outCompression = BAE_EDITOR_COMPRESSION_VORBIS_64K;  return 1;
                case FOUR_CHAR('v','0','8','0'): *outCompression = BAE_EDITOR_COMPRESSION_VORBIS_80K;  return 1;
                case FOUR_CHAR('v','0','9','6'): *outCompression = BAE_EDITOR_COMPRESSION_VORBIS_96K;  return 1;
                case FOUR_CHAR('v','1','2','8'): *outCompression = BAE_EDITOR_COMPRESSION_VORBIS_128K; return 1;
                case FOUR_CHAR('v','1','6','0'): *outCompression = BAE_EDITOR_COMPRESSION_VORBIS_160K; return 1;
                case FOUR_CHAR('v','1','9','2'): *outCompression = BAE_EDITOR_COMPRESSION_VORBIS_192K; return 1;
                case FOUR_CHAR('v','2','5','6'): *outCompression = BAE_EDITOR_COMPRESSION_VORBIS_256K; return 1;
                default: return 0;
            }
        case FOUR_CHAR('O','g','g','O'):
            switch ((uint32_t)sampleInfo->compressionSubType)
            {
                case FOUR_CHAR('o','0','1','2'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_12K;  return 1;
                case FOUR_CHAR('o','0','1','6'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_16K;  return 1;
                case FOUR_CHAR('o','0','2','4'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_24K;  return 1;
                case FOUR_CHAR('o','0','3','2'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_32K;  return 1;
                case FOUR_CHAR('o','0','4','8'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_48K;  return 1;
                case FOUR_CHAR('o','0','6','4'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_64K;  return 1;
                case FOUR_CHAR('o','0','8','0'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_80K;  return 1;
                case FOUR_CHAR('o','0','9','6'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_96K;  return 1;
                case FOUR_CHAR('o','1','2','8'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_128K; return 1;
                case FOUR_CHAR('o','1','6','0'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_160K; return 1;
                case FOUR_CHAR('o','1','9','2'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_192K; return 1;
                case FOUR_CHAR('o','2','5','6'): *outCompression = BAE_EDITOR_COMPRESSION_OPUS_256K; return 1;
                default: return 0;
            }
        default:
            return 0;
    }
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
    int hasSampleGain = 0;
    double sampleGainDb = 0.0;
    double sampleGainLinear = 1.0;
    int hasInstrumentGain = 0;
    int hasSndStorage = 0;
    double instrumentGainPercent = 1.0;
    BAERmfEditorCompressionType targetCompression = BAE_EDITOR_COMPRESSION_DONT_CHANGE;
    BAERmfEditorSndStorageType sndStorageType = BAE_EDITOR_SND_STORAGE_ESND;
    BAERmfEditorOpusMode opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
    int doProcess;
    uint32_t instCount;
    uint32_t i;
    int argIdx;
    int instrumentTargetMatched[MAX_TARGET_INSTRUMENTS];
    int sampleTargetMatched[MAX_TARGET_SAMPLES];
    uint32_t processedSndIDs[MAX_TARGET_SNDS];
    int processedSndCount;
    uint32_t reencodedCount;
    uint32_t skippedCount;
    uint32_t instrumentGainCount;

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
        else if (strcmp(argv[argIdx], "--sgain") == 0)
        {
            char *end;

            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --sgain requires a number argument (dB)\n");
                return 1;
            }

            sampleGainDb = strtod(argv[++argIdx], &end);
            if (end == argv[argIdx] || *end != '\0')
            {
                fprintf(stderr, "Error: invalid --sgain value '%s'\n", argv[argIdx]);
                return 1;
            }
            hasSampleGain = 1;
        }
        else if (strcmp(argv[argIdx], "--igain") == 0)
        {
            char *end;

            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --igain requires a number argument (percent scalar)\n");
                return 1;
            }

            instrumentGainPercent = strtod(argv[++argIdx], &end);
            if (end == argv[argIdx] || *end != '\0')
            {
                fprintf(stderr, "Error: invalid --igain value '%s'\n", argv[argIdx]);
                return 1;
            }
            hasInstrumentGain = 1;
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
            hasSndStorage = 1;
        }
        else if (strcmp(argv[argIdx], "--override") == 0)
        {
            int r;

            if (argIdx + 1 >= argc)
            {
                fprintf(stderr, "Error: --override requires a value (try --override help)\n");
                return 1;
            }
            r = parseOverride(argv[++argIdx]);
            if (r == -1)
            {
                return 0; /* help was printed */
            }
            if (!r)
            {
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

    doProcess = (codec >= 0 || hasSampleGain || hasInstrumentGain || hasSndStorage ||
                 sndOverrideCount > 0);

    if (!doProcess && hasTargetFilters())
    {
        fprintf(stderr, "Error: --instrument/--sample require --codec, --sgain, or --igain\n");
        return 1;
    }
    if (minFrames > 0 && !(codec >= 0 || hasSampleGain || hasSndStorage))
    {
        fprintf(stderr, "Error: --minframes requires --codec, --sgain, or --sndstorage\n");
        return 1;
    }
    if (skipProgramCount > 0 && !doProcess)
    {
        fprintf(stderr, "Error: --skip requires --codec, --sgain, or --igain\n");
        return 1;
    }
    if (hasSampleGain)
    {
        sampleGainLinear = pow(10.0, sampleGainDb / 20.0);
        if (sampleGainLinear < 0.0)
        {
            sampleGainLinear = 0.0;
        }
    }
    if (hasInstrumentGain)
    {
        if (instrumentGainPercent < 0.0)
        {
            instrumentGainPercent = 0.0;
        }
        if (instrumentGainPercent > 2.0)
        {
            instrumentGainPercent = 2.0;
        }
    }

    if (codec >= 0)
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
    }
    if (codec >= 0 || hasSndStorage)
    {
        printf("Recompressed SND storage: %s\n", sndStorageName(sndStorageType));
    }
    if (hasSampleGain)
    {
        printf("Sample gain: %+0.2f dB (x%.6f)\n", sampleGainDb, sampleGainLinear);
    }
    if (hasInstrumentGain)
    {
        printf("Instrument gain: x%.3f (scalar)\n", instrumentGainPercent);
    }
    if (sndOverrideCount > 0)
    {
        int oi;
        size_t outLen = outputPath ? strlen(outputPath) : 0;
        int outIsHsb = (outLen >= 4 && strcasecmp(outputPath + outLen - 4, ".hsb") == 0);

        printf("SND overrides: %d entries\n", sndOverrideCount);
        for (oi = 0; oi < sndOverrideCount; ++oi)
        {
            printf("  SndID %-6u -> codec %d (%s)",
                   sndOverrides[oi].sndID,
                   sndOverrides[oi].codec,
                   (sndOverrides[oi].codec >= 0 && sndOverrides[oi].codec < CODEC_COUNT)
                       ? codecNames[sndOverrides[oi].codec] : "?");
            if (sndOverrides[oi].bitrate > 0)
            {
                printf(" @ %u kbps", sndOverrides[oi].bitrate);
            }
            printf("\n");

            if (outIsHsb && sndOverrides[oi].codec >= 0 && codecRequiresZsb(sndOverrides[oi].codec))
            {
                fprintf(stderr,
                        "Error: override for SndID %u uses %s codec which requires ZSB output.\n"
                        "       Please use a .zsb output extension.\n",
                        sndOverrides[oi].sndID, codecNames[sndOverrides[oi].codec]);
                return 1;
            }
        }
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
    instrumentGainCount = 0;

    /* For sample processing (codec/sgain/sndstorage), enable batch SND mode so all re-encodes
       are queued and committed in a single bank rebuild after the loops. */
    if (hasSampleGain || codec >= 0 || hasSndStorage || sndOverrideCount > 0)
    {
        result = BAERmfEditorBank_BeginBatchSnd(bankToken);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: BeginBatchSnd failed (%d)\n", (int)result);
            BAEMixer_Delete(mixer);
            return 1;
        }
    }

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

        /* When only igain is active, show condensed sample lines (SndID + Vol only) */
        int igainOnlyMode = (hasInstrumentGain && !hasSampleGain && codec < 0);

        for (s = 0; s < sampleCount; ++s)
        {
            BAERmfEditorBankSampleInfo sampleInfo;
            int instrumentMatchIdx = -1;
            int sampleMatchIdx = -1;
            int targetHit;
            int skipReason = 0;
            int sharedAlready = 0;
            int si;
            int sampleDataActive;
            int effectiveCodec;
            uint32_t effectiveBitrate;
            BAERmfEditorCompressionType effectiveCompression;
            uint32_t effectiveChosenBitrate;

            result = BAERmfEditorBank_GetInstrumentSampleInfo(bankToken, i, s, &sampleInfo);
            if (result != BAE_NO_ERROR)
            {
                fprintf(stderr, "      Sample[%u] GetInstrumentSampleInfo failed (%d)\n", s, (int)result);
                continue;
            }

            if (igainOnlyMode)
            {
                int16_t origVol = sampleInfo.splitVolume ? sampleInfo.splitVolume : 100;
                int32_t newVol = (int32_t)(origVol * instrumentGainPercent + 0.5);
                if (newVol < 0) newVol = 0;
                if (newVol > 800) newVol = 800;
                printf("      Sample[%u]: SndID=%d Vol=%d->%d/800\n",
                       s,
                       (int)sampleInfo.sndResourceID,
                       (int)origVol,
                       (int)newVol);
            }
            else
            {
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
            }

            if (isTargetInstrument(i, &instrumentMatchIdx))
            {
                instrumentTargetMatched[instrumentMatchIdx] = 1;
            }
            if (isTargetSample(i, s, &sampleMatchIdx))
            {
                sampleTargetMatched[sampleMatchIdx] = 1;
            }

            if (!doProcess)
            {
                continue;
            }

            targetHit = !hasTargetFilters() ||
                        isTargetInstrument(i, NULL) ||
                        isTargetSample(i, s, NULL);

            /* igain is applied per-instrument below (after the sample loop), not per-split */

            /* Per-SND effective codec/bitrate: --override wins over global --codec */
            {
                const SndOverride *ovr = findSndOverride((uint32_t)sampleInfo.sndResourceID);
                if (ovr && ovr->codec >= 0)
                {
                    effectiveCodec   = ovr->codec;
                    effectiveBitrate = (ovr->bitrate > 0) ? ovr->bitrate : bitrateKbps;
                    if (!resolveTargetCompression(effectiveCodec, effectiveBitrate,
                                                  &effectiveCompression, &effectiveChosenBitrate))
                    {
                        effectiveCodec = codec;
                        effectiveBitrate = bitrateKbps;
                        effectiveCompression = targetCompression;
                        effectiveChosenBitrate = chosenBitrate;
                        fprintf(stderr, "Warning: override codec resolve failed for SndID %u, "
                                        "falling back to global codec\n",
                                sampleInfo.sndResourceID);
                    }
                }
                else
                {
                    effectiveCodec         = codec;
                    effectiveBitrate       = bitrateKbps;
                    effectiveCompression   = targetCompression;
                    effectiveChosenBitrate = chosenBitrate;
                }
            }

            sampleDataActive = (effectiveCodec >= 0 || hasSampleGain || hasSndStorage);
            if (!sampleDataActive)
            {
                continue;
            }

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
            else if ((int32_t)sampleInfo.sndResourceID <= 0)
            {
                skipReason = 5;
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
                    case 5:
                    default:
                        if (skipReason == 4)
                        {
                            printf("        SKIPPED (shared SndID already processed)\n");
                        }
                        else
                        {
                            printf("        SKIPPED (no backing SndID/resource)\n");
                        }
                        break;
                }
                continue;
            }

            if (hasSampleGain)
            {
                void *waveData;
                uint32_t frameCount;
                uint16_t bitSize;
                uint16_t channels;
                BAE_UNSIGNED_FIXED sampleRate;
                BAERmfEditorCompressionType sampleCompression;
                BAERmfEditorSndStorageType sampleStorage;
                bool sampleOpusRoundTrip;

                waveData = NULL;
                frameCount = 0;
                bitSize = 0;
                channels = 0;
                sampleRate = 0;

                result = BAERmfEditorBank_GetSampleWaveformData(bankToken,
                                                                 i,
                                                                 s,
                                                                 &waveData,
                                                                 &frameCount,
                                                                 &bitSize,
                                                                 &channels,
                                                                 &sampleRate);
                if (result != BAE_NO_ERROR || !waveData)
                {
                    ++skippedCount;
                    printf("        SKIPPED (decode failed: %d)\n", (int)result);
                    continue;
                }

                if (!applyPcmGainInPlace(waveData,
                                         frameCount,
                                         channels,
                                         bitSize,
                                         sampleGainLinear))
                {
                    BAERmfEditorBank_FreeWaveformData(waveData);
                    ++skippedCount;
                    printf("        SKIPPED (gain unsupported for %dbit/%dch sample)\n",
                           (int)bitSize,
                           (int)channels);
                    continue;
                }

                if (effectiveCodec >= 0)
                {
                    sampleCompression = effectiveCompression;
                    sampleStorage = sndStorageType;
                }
                else
                {
                    if (!inferCompressionFromSampleInfo(&sampleInfo, &sampleCompression))
                    {
                        BAERmfEditorBank_FreeWaveformData(waveData);
                        ++skippedCount;
                        printf("        SKIPPED (cannot infer original compression)\n");
                        continue;
                    }
                    sampleStorage = hasSndStorage ? sndStorageType : sampleInfo.sndStorageType;
                }

                sampleOpusRoundTrip = sampleInfo.opusRoundTripResample;

                result = BAERmfEditorBank_ReEncodeSampleFromMutablePCMEx(bankToken,
                                                                          i,
                                                                          s,
                                                                          sampleCompression,
                                                                          sampleStorage,
                                                                          opusMode,
                                                                          sampleOpusRoundTrip,
                                                                          waveData,
                                                                          frameCount,
                                                                          bitSize,
                                                                          channels,
                                                                          sampleRate);
                BAERmfEditorBank_FreeWaveformData(waveData);
            }
            else
            {
                BAERmfEditorCompressionType sampleCompression;
                BAERmfEditorSndStorageType sampleStorage;

                if (effectiveCodec < 0 && hasSndStorage)
                {
                    result = BAERmfEditorBank_SetSampleSndStorageType(bankToken,
                                                                      i,
                                                                      s,
                                                                      sndStorageType);
                    if (result != BAE_NO_ERROR)
                    {
                        ++skippedCount;
                        if (result == BAE_COMPRESSION_INEFFECTIVE)
                        {
                            int32_t origBytes = (int32_t)(sampleInfo.frameCount *
                                                           sampleInfo.channels *
                                                           (sampleInfo.bitDepth / 8));
                            printf("        SKIPPED (CSND compression ineffective: compressed >= original (%d bytes))\n",
                                   origBytes);
                        }
                        else
                            printf("        SKIPPED (set storage failed: %d)\n", (int)result);
                        continue;
                    }

                    if (!addUniqueUint32(processedSndIDs,
                                         &processedSndCount,
                                         MAX_TARGET_SNDS,
                                         (uint32_t)sampleInfo.sndResourceID))
                    {
                        fprintf(stderr, "Error: too many processed SND IDs\n");
                        BAERmfEditorBank_AbortBatchSnd(bankToken);
                        BAEMixer_Delete(mixer);
                        return 1;
                    }

                    ++reencodedCount;
                    printf("        OK\n");
                    continue;
                }

                if (effectiveCodec >= 0)
                {
                    sampleCompression = effectiveCompression;
                    sampleStorage = sndStorageType;
                }
                else
                {
                    if (!inferCompressionFromSampleInfo(&sampleInfo, &sampleCompression))
                    {
                        ++skippedCount;
                        printf("        SKIPPED (cannot infer original compression)\n");
                        continue;
                    }
                    sampleStorage = hasSndStorage ? sndStorageType : sampleInfo.sndStorageType;
                }

                result = BAERmfEditorBank_ReEncodeSample(bankToken,
                                                         i,
                                                         s,
                                                         sampleCompression,
                                                         sampleStorage,
                                                         opusMode);
            }
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
                BAERmfEditorBank_AbortBatchSnd(bankToken);
                BAEMixer_Delete(mixer);
                return 1;
            }

            ++reencodedCount;
            printf("        OK\n");
        }

        /* Apply igain to all splits of this instrument in a single rebuild */
        if (hasInstrumentGain && doProcess &&
            !(skipProgramCount > 0 && isSkippedProgram(instInfo.program)))
        {
            int instTargetHit = !hasTargetFilters() || isTargetInstrument(i, NULL);
            if (instTargetHit)
            {
                result = BAERmfEditorBank_ScaleAllSplitVolumes(bankToken, i, instrumentGainPercent);
                if (result == BAE_NO_ERROR)
                {
                    instrumentGainCount += (int)sampleCount;
                }
                else
                {
                    ++skippedCount;
                    printf("  SKIPPED instrument %u igain (failed: %d)\n", i, (int)result);
                }
            }
        }
    }

    /* Commit all queued SND replacements in a single bank rebuild */
    if (hasSampleGain || codec >= 0 || hasSndStorage || sndOverrideCount > 0)
    {
        result = BAERmfEditorBank_CommitBatchSnd(bankToken);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Error: CommitBatchSnd failed (%d)\n", (int)result);
            BAEMixer_Delete(mixer);
            return 1;
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

    if (doProcess)
    {
         printf("\nProcessing complete: %u samples updated, %u instrument slots gain-adjusted, %u skipped\n",
               reencodedCount,
             instrumentGainCount,
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
