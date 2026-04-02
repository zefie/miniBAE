/****************************************************************************
 *
 * adp2wav_main.c
 *
 * Convert proprietary ADP (audio/g722 wrapper) into PCM WAV.
 *
 ****************************************************************************/

#include "adp2wav_decode.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void print_usage(char const *programName)
{
    fprintf(stderr,
            "ADP to WAV Converter\n"
            "Usage: %s [-v] [--raw-g722-output path] <input.adp> <output.wav>\n"
            "\n"
            "Options:\n"
            "  -v                  Print parsed header and block metadata\n"
            "  --raw-g722-output   Write rotated G.722 payload to this path\n",
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

static int write_wav_pcm16_mono(char const *path,
                                uint32_t sampleRate,
                                int16_t const *samples,
                                size_t frameCount)
{
    FILE *file;
    uint32_t dataSize;
    uint32_t riffSize;
    unsigned char hdr[44];

    if (frameCount > (SIZE_MAX / 2))
    {
        fprintf(stderr, "Error: frame count overflow\n");
        return 1;
    }

    dataSize = (uint32_t)(frameCount * 2);
    riffSize = 36U + dataSize;

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr + 0, "RIFF", 4);
    write_le_u32(hdr + 4, riffSize);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    write_le_u32(hdr + 16, 16);
    write_le_u16(hdr + 20, 1);
    write_le_u16(hdr + 22, 1);
    write_le_u32(hdr + 24, sampleRate);
    write_le_u32(hdr + 28, sampleRate * 2U);
    write_le_u16(hdr + 32, 2);
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
    if (frameCount > 0 && fwrite(samples, sizeof(int16_t), frameCount, file) != frameCount)
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

int main(int argc, char *argv[])
{
    char const *inputPath = NULL;
    char const *outputPath = NULL;
    char const *rawOutputPath = NULL;
    bool verbose = false;
    unsigned char *fileData = NULL;
    size_t fileSize = 0;
    BAEAdpDecodeOptions options;
    BAEAdpDecodedAudio audio;
    size_t i;
    int result = 1;

    memset(&options, 0, sizeof(options));
    memset(&audio, 0, sizeof(audio));

    for (i = 1; i < (size_t)argc; ++i)
    {
        char const *arg = argv[i];
        if (strcmp(arg, "-v") == 0)
        {
            verbose = true;
        }
        else if (strcmp(arg, "--raw-g722-output") == 0)
        {
            if (i + 1 >= (size_t)argc)
            {
                fprintf(stderr, "Error: --raw-g722-output requires a path\n");
                print_usage(argv[0]);
                return 1;
            }
            rawOutputPath = argv[++i];
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

    options.verbose = verbose ? 1 : 0;
    options.rawG722OutputPath = rawOutputPath;
    options.logFile = stderr;
    if (BAEAdp_DecodeMemoryToPCM16Mono(fileData, fileSize, &options, &audio) != 0)
    {
        goto cleanup;
    }
    if (write_wav_pcm16_mono(outputPath, audio.sampleRate, audio.samples, audio.frameCount) != 0)
    {
        goto cleanup;
    }
    if (verbose)
    {
        fprintf(stderr, "Wrote WAV '%s'\n", outputPath);
    }

    result = 0;

cleanup:
    BAEAdp_FreeDecodedAudio(&audio);
    free(fileData);
    return result;
}