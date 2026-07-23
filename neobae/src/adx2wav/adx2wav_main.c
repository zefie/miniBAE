/****************************************************************************
 *
 * adx2wav_main.c
 *
 * Convert CRI ADX to PCM WAV.
 *
 ****************************************************************************/

#include "adx2wav_decode.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct RenderConfig
{
    bool hasDuration;
    uint32_t durationSeconds;
    bool hasLoopCount;
    uint32_t loopCount;
} RenderConfig;

static void write_le_u16(unsigned char *dst, uint16_t value)
{
    dst[0] = (unsigned char)(value & 0xFF);
    dst[1] = (unsigned char)((value >> 8) & 0xFF);
}

static void write_le_u32(unsigned char *dst, uint32_t value)
{
    dst[0] = (unsigned char)(value & 0xFF);
    dst[1] = (unsigned char)((value >> 8) & 0xFF);
    dst[2] = (unsigned char)((value >> 16) & 0xFF);
    dst[3] = (unsigned char)((value >> 24) & 0xFF);
}

static int parse_u32(char const *text, uint32_t *outValue)
{
    char *end;
    unsigned long parsed;

    if (!text || !*text)
    {
        return 1;
    }

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed > 0xFFFFFFFFUL)
    {
        return 1;
    }

    *outValue = (uint32_t)parsed;
    return 0;
}

static void print_usage(char const *programName)
{
    fprintf(stderr,
            "ADX to WAV Converter\n"
            "Usage: %s [-v] [--duration seconds] [--loopcount count] <input.adx> <output.wav>\n"
            "\n"
            "Options:\n"
            "  -v             Print parsed ADX metadata\n"
            "  --duration     Limit output to x seconds (takes priority over --loopcount)\n"
            "  --loopcount    Repeat ADX loop region x additional times\n",
            programName);
}

static int read_entire_file(char const *path, unsigned char **outData, size_t *outSize)
{
    FILE *file;
    long fileSize;
    unsigned char *data;

    *outData = NULL;
    *outSize = 0;

    file = fopen(path, "rb");
    if (!file)
    {
        fprintf(stderr, "Error: cannot open '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fprintf(stderr, "Error: cannot seek '%s': %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }
    fileSize = ftell(file);
    if (fileSize < 0)
    {
        fprintf(stderr, "Error: cannot determine size of '%s': %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }
    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fprintf(stderr, "Error: cannot rewind '%s': %s\n", path, strerror(errno));
        fclose(file);
        return 1;
    }

    data = (unsigned char *)malloc((size_t)fileSize);
    if (!data)
    {
        fprintf(stderr, "Error: out of memory allocating %ld bytes\n", fileSize);
        fclose(file);
        return 1;
    }
    if (fileSize > 0 && fread(data, 1, (size_t)fileSize, file) != (size_t)fileSize)
    {
        fprintf(stderr, "Error: failed reading '%s'\n", path);
        free(data);
        fclose(file);
        return 1;
    }

    fclose(file);
    *outData = data;
    *outSize = (size_t)fileSize;
    return 0;
}

static int write_wav_pcm16(char const *path,
                           uint32_t sampleRate,
                           uint16_t channels,
                           int16_t const *samples,
                           size_t frameCount)
{
    FILE *file;
    uint64_t totalSampleCount;
    uint64_t dataSize64;
    uint32_t dataSize;
    uint32_t riffSize;
    uint32_t byteRate;
    uint16_t blockAlign;
    unsigned char hdr[44];

    if (channels == 0)
    {
        fprintf(stderr, "Error: WAV channel count must be non-zero\n");
        return 1;
    }

    totalSampleCount = (uint64_t)frameCount * (uint64_t)channels;
    dataSize64 = totalSampleCount * sizeof(int16_t);
    if (dataSize64 > 0xFFFFFFFFULL)
    {
        fprintf(stderr, "Error: WAV output is too large for RIFF/WAV\n");
        return 1;
    }

    dataSize = (uint32_t)dataSize64;
    riffSize = 36U + dataSize;
    blockAlign = (uint16_t)(channels * 2U);
    byteRate = sampleRate * (uint32_t)blockAlign;

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, "RIFF", 4);
    write_le_u32(hdr + 4, riffSize);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    write_le_u32(hdr + 16, 16);
    write_le_u16(hdr + 20, 1);
    write_le_u16(hdr + 22, channels);
    write_le_u32(hdr + 24, sampleRate);
    write_le_u32(hdr + 28, byteRate);
    write_le_u16(hdr + 32, blockAlign);
    write_le_u16(hdr + 34, 16);
    memcpy(hdr + 36, "data", 4);
    write_le_u32(hdr + 40, dataSize);

    file = fopen(path, "wb");
    if (!file)
    {
        fprintf(stderr, "Error: cannot create '%s': %s\n", path, strerror(errno));
        return 1;
    }
    if (fwrite(hdr, 1, sizeof(hdr), file) != sizeof(hdr))
    {
        fprintf(stderr, "Error: failed writing WAV header\n");
        fclose(file);
        return 1;
    }
    if (dataSize > 0 && fwrite(samples, 1, dataSize, file) != dataSize)
    {
        fprintf(stderr, "Error: failed writing WAV data\n");
        fclose(file);
        return 1;
    }
    if (fclose(file) != 0)
    {
        fprintf(stderr, "Error: failed closing WAV output: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int append_frames(int16_t *dst,
                         size_t *dstOffset,
                         int16_t const *src,
                         size_t srcStartFrame,
                         size_t frameCount,
                         uint16_t channels)
{
    size_t sampleCount = frameCount * channels;
    memcpy(dst + (*dstOffset * channels), src + (srcStartFrame * channels), sampleCount * sizeof(int16_t));
    *dstOffset += frameCount;
    return 0;
}

static int render_with_options(BAEAdxDecodedAudio const *decoded,
                               RenderConfig const *cfg,
                               BAEAdxDecodedAudio *rendered)
{
    bool loopValid;
    size_t frameCount;
    size_t outFrames;
    int16_t *outSamples;

    rendered->samples = NULL;
    rendered->frameCount = 0;
    rendered->sampleRate = decoded->sampleRate;
    rendered->channels = decoded->channels;
    rendered->hasLoop = decoded->hasLoop;
    rendered->loopStart = decoded->loopStart;
    rendered->loopEnd = decoded->loopEnd;

    loopValid = decoded->hasLoop && decoded->loopEnd > decoded->loopStart && decoded->loopEnd <= decoded->frameCount;
    frameCount = decoded->frameCount;

    if (cfg->hasDuration)
    {
        uint64_t targetFrames64 = (uint64_t)cfg->durationSeconds * (uint64_t)decoded->sampleRate;
        size_t targetFrames;

        if (targetFrames64 > (uint64_t)SIZE_MAX)
        {
            fprintf(stderr, "Error: requested duration is too large\n");
            return 1;
        }
        targetFrames = (size_t)targetFrames64;

        if (!loopValid)
        {
            outFrames = frameCount < targetFrames ? frameCount : targetFrames;
        }
        else
        {
            outFrames = targetFrames;
        }
    }
    else if (cfg->hasLoopCount && loopValid)
    {
        size_t loopLen = decoded->loopEnd - decoded->loopStart;
        uint64_t outFrames64 = (uint64_t)frameCount + (uint64_t)cfg->loopCount * (uint64_t)loopLen;
        if (outFrames64 > (uint64_t)SIZE_MAX)
        {
            fprintf(stderr, "Error: loop-expanded output is too large\n");
            return 1;
        }
        outFrames = (size_t)outFrames64;
    }
    else
    {
        outFrames = frameCount;
    }

    if (outFrames == 0)
    {
        rendered->samples = NULL;
        rendered->frameCount = 0;
        return 0;
    }

    if ((uint64_t)outFrames > (uint64_t)SIZE_MAX / ((uint64_t)decoded->channels * sizeof(int16_t)))
    {
        fprintf(stderr, "Error: output sample count overflow\n");
        return 1;
    }

    outSamples = (int16_t *)malloc(outFrames * decoded->channels * sizeof(int16_t));
    if (!outSamples)
    {
        fprintf(stderr, "Error: out of memory allocating rendered audio\n");
        return 1;
    }

    if (cfg->hasDuration)
    {
        size_t dstFrame = 0;

        if (!loopValid)
        {
            append_frames(outSamples, &dstFrame, decoded->samples, 0, outFrames, decoded->channels);
        }
        else
        {
            size_t introFrames = decoded->loopEnd;
            size_t loopLen = decoded->loopEnd - decoded->loopStart;
            if (introFrames > outFrames)
            {
                introFrames = outFrames;
            }

            append_frames(outSamples, &dstFrame, decoded->samples, 0, introFrames, decoded->channels);

            while (dstFrame < outFrames)
            {
                size_t remain = outFrames - dstFrame;
                size_t chunk = remain < loopLen ? remain : loopLen;
                append_frames(outSamples, &dstFrame, decoded->samples, decoded->loopStart, chunk, decoded->channels);
            }
        }
    }
    else if (cfg->hasLoopCount && loopValid)
    {
        size_t dstFrame = 0;
        size_t loopLen = decoded->loopEnd - decoded->loopStart;
        uint32_t i;

        append_frames(outSamples, &dstFrame, decoded->samples, 0, decoded->loopEnd, decoded->channels);
        for (i = 0; i < cfg->loopCount; ++i)
        {
            append_frames(outSamples, &dstFrame, decoded->samples, decoded->loopStart, loopLen, decoded->channels);
        }
        if (decoded->loopEnd < decoded->frameCount)
        {
            append_frames(outSamples,
                          &dstFrame,
                          decoded->samples,
                          decoded->loopEnd,
                          decoded->frameCount - decoded->loopEnd,
                          decoded->channels);
        }
    }
    else
    {
        append_frames(outSamples, &rendered->frameCount, decoded->samples, 0, outFrames, decoded->channels);
        rendered->samples = outSamples;
        return 0;
    }

    rendered->samples = outSamples;
    rendered->frameCount = outFrames;
    return 0;
}

int main(int argc, char *argv[])
{
    char const *inputPath = NULL;
    char const *outputPath = NULL;
    bool verbose = false;
    unsigned char *fileData = NULL;
    size_t fileSize = 0;
    RenderConfig renderCfg;
    BAEAdxDecodeOptions decodeOpts;
    BAEAdxDecodedAudio decoded;
    BAEAdxDecodedAudio rendered;
    size_t i;
    int result = 1;

    memset(&renderCfg, 0, sizeof(renderCfg));
    memset(&decodeOpts, 0, sizeof(decodeOpts));
    memset(&decoded, 0, sizeof(decoded));
    memset(&rendered, 0, sizeof(rendered));

    for (i = 1; i < (size_t)argc; ++i)
    {
        char const *arg = argv[i];
        if (strcmp(arg, "-v") == 0)
        {
            verbose = true;
        }
        else if (strcmp(arg, "--duration") == 0)
        {
            if (i + 1 >= (size_t)argc || parse_u32(argv[i + 1], &renderCfg.durationSeconds) != 0)
            {
                fprintf(stderr, "Error: --duration requires a non-negative integer value\n");
                print_usage(argv[0]);
                return 1;
            }
            renderCfg.hasDuration = true;
            ++i;
        }
        else if (strcmp(arg, "--loopcount") == 0)
        {
            if (i + 1 >= (size_t)argc || parse_u32(argv[i + 1], &renderCfg.loopCount) != 0)
            {
                fprintf(stderr, "Error: --loopcount requires a non-negative integer value\n");
                print_usage(argv[0]);
                return 1;
            }
            renderCfg.hasLoopCount = true;
            ++i;
        }
        else if (!inputPath)
        {
            inputPath = arg;
        }
        else if (!outputPath)
        {
            outputPath = arg;
        }
        else
        {
            fprintf(stderr, "Error: unexpected argument '%s'\n", arg);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!inputPath || !outputPath)
    {
        print_usage(argv[0]);
        return 1;
    }

    if (read_entire_file(inputPath, &fileData, &fileSize) != 0)
    {
        goto cleanup;
    }

    decodeOpts.verbose = verbose ? 1 : 0;
    decodeOpts.logFile = stderr;
    if (BAEAdx_DecodeMemoryToPCM16(fileData, fileSize, &decodeOpts, &decoded) != 0)
    {
        goto cleanup;
    }

    if ((renderCfg.hasLoopCount || renderCfg.hasDuration) && !decoded.hasLoop && verbose)
    {
        fprintf(stderr, "Warning: ADX has no loop points; loop-based options have limited effect\n");
    }

    if (render_with_options(&decoded, &renderCfg, &rendered) != 0)
    {
        goto cleanup;
    }

    if (write_wav_pcm16(outputPath,
                        rendered.sampleRate,
                        rendered.channels,
                        rendered.samples,
                        rendered.frameCount) != 0)
    {
        goto cleanup;
    }

    if (verbose)
    {
        fprintf(stderr,
                "Wrote WAV '%s' (%zu frames @ %u Hz, %u channels)\n",
                outputPath,
                rendered.frameCount,
                (unsigned int)rendered.sampleRate,
                (unsigned int)rendered.channels);
    }

    result = 0;

cleanup:
    BAEAdx_FreeDecodedAudio(&rendered);
    BAEAdx_FreeDecodedAudio(&decoded);
    free(fileData);
    return result;
}