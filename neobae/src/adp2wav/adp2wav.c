/****************************************************************************
 *
 * adp2wav.c
 *
 * Convert proprietary ADP (audio/g722 wrapper) into PCM WAV.
 *
 * Observed ADP structure:
 *   bytes 0..13  : "audio/g722    "
 *   bytes 14..15 : sample rate (u16be)
 *   bytes 16..17 : encoder field (observed 2)
 *   bytes 18..19 : legacy/unknown length field (u16be)
 *   bytes 20..21 : mode/block-count packed as (mode << 8) | block_count
 *   bytes 22..23 : repeat delay in ms
 *   bytes 24..   : block_count control blocks, each 12 bytes:
 *                  u32be start_sample, u32be end_sample, u16be count, u16be flags
 *   payload      : remaining bytes, rotate-right(2), decode as raw G.722
 *
 ****************************************************************************/

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef _WIN32
#include <process.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <sys/wait.h>
#endif

#define ADP_BASE_HEADER_SIZE 24
#define ADP_BLOCK_SIZE 12

typedef struct AdpBlock
{
    uint32_t startSample;
    uint32_t endSample;
    uint16_t count;
    uint16_t flags;
} AdpBlock;

typedef struct AdpHeader
{
    uint16_t sampleRate;
    uint16_t encoderField;
    uint16_t legacyLength;
    uint8_t mode;
    uint8_t blockCount;
    uint16_t repeatDelayMs;
    size_t payloadOffset;
} AdpHeader;

typedef struct SampleBuffer
{
    int16_t *data;
    size_t length;
    size_t capacity;
} SampleBuffer;

static uint16_t read_be_u16(unsigned char const *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static uint32_t read_be_u32(unsigned char const *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

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

static unsigned char rotate_right_2(unsigned char value)
{
    return (unsigned char)((value >> 2) | (value << 6));
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

static int write_entire_file(char const *path, unsigned char const *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        fprintf(stderr, "Error: cannot create '%s': %s\n", path, strerror(errno));
        return 1;
    }

    if (size > 0 && fwrite(data, 1, size, file) != size)
    {
        fprintf(stderr, "Error: failed writing '%s'\n", path);
        fclose(file);
        return 1;
    }

    if (fclose(file) != 0)
    {
        fprintf(stderr, "Error: failed closing '%s': %s\n", path, strerror(errno));
        return 1;
    }

    return 0;
}

static int parse_header_and_blocks(unsigned char const *data,
                                   size_t size,
                                   AdpHeader *header,
                                   AdpBlock **outBlocks)
{
    uint16_t modeAndCount;
    size_t blocksBytes;
    size_t i;
    AdpBlock *blocks;

    *outBlocks = NULL;

    if (size < ADP_BASE_HEADER_SIZE)
    {
        fprintf(stderr, "Error: file is too small for ADP base header\n");
        return 1;
    }

    if (memcmp(data, "audio/g722    ", 14) != 0)
    {
        fprintf(stderr, "Error: unsupported ADP header prefix\n");
        return 1;
    }

    header->sampleRate = read_be_u16(data + 14);
    header->encoderField = read_be_u16(data + 16);
    header->legacyLength = read_be_u16(data + 18);
    modeAndCount = read_be_u16(data + 20);
    header->mode = (uint8_t)((modeAndCount >> 8) & 0xFF);
    header->blockCount = (uint8_t)(modeAndCount & 0xFF);
    header->repeatDelayMs = read_be_u16(data + 22);

    if (header->sampleRate == 0)
    {
        fprintf(stderr, "Error: invalid ADP sample rate 0\n");
        return 1;
    }

    blocksBytes = (size_t)header->blockCount * ADP_BLOCK_SIZE;
    if (size < ADP_BASE_HEADER_SIZE + blocksBytes)
    {
        fprintf(stderr, "Error: ADP block section truncated (count=%u)\n",
                (unsigned int)header->blockCount);
        return 1;
    }

    header->payloadOffset = ADP_BASE_HEADER_SIZE + blocksBytes;

    if (header->blockCount == 0)
    {
        return 0;
    }

    blocks = (AdpBlock *)calloc(header->blockCount, sizeof(AdpBlock));
    if (!blocks)
    {
        fprintf(stderr, "Error: out of memory allocating %u blocks\n",
                (unsigned int)header->blockCount);
        return 1;
    }

    for (i = 0; i < (size_t)header->blockCount; ++i)
    {
        size_t off = ADP_BASE_HEADER_SIZE + i * ADP_BLOCK_SIZE;
        blocks[i].startSample = read_be_u32(data + off + 0);
        blocks[i].endSample = read_be_u32(data + off + 4);
        blocks[i].count = read_be_u16(data + off + 8);
        blocks[i].flags = read_be_u16(data + off + 10);
    }

    *outBlocks = blocks;
    return 0;
}

static int write_temp_file(unsigned char const *data, size_t size, char *tempPath, size_t tempPathSize)
{
    int fd;
    size_t totalWritten;

    if (tempPathSize < 18)
    {
        fprintf(stderr, "Error: internal temp path buffer too small\n");
        return 1;
    }

#if defined(_WIN32)
    snprintf(tempPath, tempPathSize, "%s\\adp2wav-XXXXXX", getenv("TEMP") ? getenv("TEMP") : "C:\\Temp");
    if (_mktemp_s(tempPath, tempPathSize) != 0)
    {
        fprintf(stderr, "Error: _mktemp_s failed\n");
        return 1;
    }
    fd = _open(tempPath, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0)
    {
        fprintf(stderr, "Error: failed creating temporary file '%s': %s\n", tempPath, strerror(errno));
        return 1;
    }
#else
    snprintf(tempPath, tempPathSize, "/tmp/adp2wav-XXXXXX");
    fd = mkstemp(tempPath);
    if (fd < 0)
    {
        fprintf(stderr, "Error: mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
#endif
    totalWritten = 0;
    while (totalWritten < size)
    {
        ssize_t written = write(fd, data + totalWritten, size - totalWritten);
        if (written < 0)
        {
            fprintf(stderr, "Error: failed writing temporary file: %s\n", strerror(errno));
            close(fd);
            unlink(tempPath);
            return 1;
        }
        totalWritten += (size_t)written;
    }

    if (close(fd) != 0)
    {
        fprintf(stderr, "Error: failed closing temporary file: %s\n", strerror(errno));
        unlink(tempPath);
        return 1;
    }

    return 0;
}

static int run_ffmpeg_decode_pcm(char const *g722Path, uint32_t sampleRate, char *pcmPath, size_t pcmPathSize)
{
    char sampleRateArg[16];
#if defined(_WIN32)
    snprintf(pcmPath, pcmPathSize, "%s\\adp2wav-pcm-XXXXXX", getenv("TEMP") ? getenv("TEMP") : "C:\\Temp");
    if (_mktemp_s(pcmPath, pcmPathSize) != 0)
    {
        fprintf(stderr, "Error: _mktemp_s failed for PCM output\n");
        return 1;
    }
#else
    snprintf(pcmPath, pcmPathSize, "/tmp/adp2wav-pcm-XXXXXX");
    {
        int fd = mkstemp(pcmPath);
        if (fd < 0)
        {
            fprintf(stderr, "Error: mkstemp for PCM output failed: %s\n", strerror(errno));
            return 1;
        }
        close(fd);
    }
#endif
    snprintf(sampleRateArg, sizeof(sampleRateArg), "%u", (unsigned int)sampleRate);

    #ifdef _WIN32
    {
        intptr_t spawnResult;
        spawnResult = _spawnlp(_P_WAIT,
                               "ffmpeg",
                               "ffmpeg",
                               "-y",
                               "-v",
                               "error",
                               "-f",
                               "g722",
                               "-i",
                               g722Path,
                               "-f",
                               "s16le",
                               "-ac",
                               "1",
                               "-ar",
                               sampleRateArg,
                               pcmPath,
                               NULL);
        if (spawnResult != 0)
        {
            fprintf(stderr, "Error: ffmpeg decode failed (exit=%ld)\n", (long)spawnResult);
            unlink(pcmPath);
            return 1;
        }
    }
    #else
    {
        pid_t pid;
        int status;

        pid = fork();
        if (pid < 0)
        {
            fprintf(stderr, "Error: failed to launch ffmpeg: %s\n", strerror(errno));
            unlink(pcmPath);
            return 1;
        }

        if (pid == 0)
        {
            execlp("ffmpeg",
                   "ffmpeg",
                   "-y",
                   "-v",
                   "error",
                   "-f",
                   "g722",
                   "-i",
                   g722Path,
                   "-f",
                   "s16le",
                   "-ac",
                   "1",
                   "-ar",
                   sampleRateArg,
                   pcmPath,
                   (char *)NULL);
            _exit(127);
        }

        if (waitpid(pid, &status, 0) < 0)
        {
            fprintf(stderr, "Error: failed waiting for ffmpeg: %s\n", strerror(errno));
            unlink(pcmPath);
            return 1;
        }

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            fprintf(stderr, "Error: ffmpeg decode failed\n");
            unlink(pcmPath);
            return 1;
        }
    }
    #endif

    return 0;
}

static int load_pcm16_mono(char const *path, int16_t **outSamples, size_t *outFrames)
{
    unsigned char *bytes;
    size_t size;
    int16_t *samples;

    *outSamples = NULL;
    *outFrames = 0;

    if (read_entire_file(path, &bytes, &size) != 0)
    {
        return 1;
    }

    if ((size % 2) != 0)
    {
        fprintf(stderr, "Error: decoded PCM has odd byte length\n");
        free(bytes);
        return 1;
    }

    samples = (int16_t *)malloc(size);
    if (!samples)
    {
        fprintf(stderr, "Error: out of memory for decoded PCM\n");
        free(bytes);
        return 1;
    }

    memcpy(samples, bytes, size);
    free(bytes);

    *outSamples = samples;
    *outFrames = size / 2;
    return 0;
}

static int ensure_capacity(SampleBuffer *buffer, size_t needed)
{
    size_t newCap;
    int16_t *newData;

    if (needed <= buffer->capacity)
    {
        return 0;
    }

    newCap = (buffer->capacity == 0) ? 4096 : buffer->capacity;
    while (newCap < needed)
    {
        newCap *= 2;
    }

    newData = (int16_t *)realloc(buffer->data, newCap * sizeof(int16_t));
    if (!newData)
    {
        fprintf(stderr, "Error: out of memory while growing sample buffer\n");
        return 1;
    }

    buffer->data = newData;
    buffer->capacity = newCap;
    return 0;
}

static int append_samples(SampleBuffer *buffer, int16_t const *samples, size_t count)
{
    if (count == 0)
    {
        return 0;
    }
    if (ensure_capacity(buffer, buffer->length + count) != 0)
    {
        return 1;
    }
    memcpy(buffer->data + buffer->length, samples, count * sizeof(int16_t));
    buffer->length += count;
    return 0;
}

static int append_silence(SampleBuffer *buffer, size_t count)
{
    if (count == 0)
    {
        return 0;
    }
    if (ensure_capacity(buffer, buffer->length + count) != 0)
    {
        return 1;
    }
    memset(buffer->data + buffer->length, 0, count * sizeof(int16_t));
    buffer->length += count;
    return 0;
}

static int16_t apply_fade_sample(int16_t s, uint16_t flags, size_t index, size_t total)
{
    double db;
    double t;
    double gain;
    int32_t out;

    if (flags == 0 || total == 0)
    {
        return s;
    }

    t = (total > 1) ? ((double)index / (double)(total - 1)) : 1.0;

    if (flags == 0x0100)
    {
        db = -60.0 + 60.0 * t;
    }
    else if (flags == 0x0200)
    {
        db = -60.0 * t;
    }
    else
    {
        return s;
    }

    gain = pow(10.0, db / 20.0);
    out = (int32_t)lrint((double)s * gain);
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    return (int16_t)out;
}

static int append_segment_repeats(SampleBuffer *out,
                                  int16_t const *src,
                                  size_t srcFrames,
                                  AdpBlock const *block)
{
    size_t start;
    size_t end;
    size_t segLen;
    size_t reps;
    size_t r;

    start = (size_t)block->startSample;
    end = (size_t)block->endSample;

    if (start >= srcFrames)
    {
        return 0;
    }
    if (end > srcFrames)
    {
        end = srcFrames;
    }
    if (end <= start)
    {
        return 0;
    }

    segLen = end - start;
    reps = (size_t)block->count + 1; /* count is repeat count */

    for (r = 0; r < reps; ++r)
    {
        size_t i;
        if (block->flags == 0)
        {
            if (append_samples(out, src + start, segLen) != 0)
            {
                return 1;
            }
        }
        else
        {
            if (ensure_capacity(out, out->length + segLen) != 0)
            {
                return 1;
            }
            for (i = 0; i < segLen; ++i)
            {
                out->data[out->length + i] = apply_fade_sample(src[start + i], block->flags, i, segLen);
            }
            out->length += segLen;
        }
    }

    return 0;
}

static int emulate_playback(SampleBuffer *out,
                            int16_t const *src,
                            size_t srcFrames,
                            AdpHeader const *header,
                            AdpBlock const *blocks)
{
    size_t cursor = 0;
    size_t i;

    out->data = NULL;
    out->length = 0;
    out->capacity = 0;

    if (header->mode == 0 || header->blockCount == 0)
    {
        if (append_samples(out, src, srcFrames) != 0)
        {
            return 1;
        }
    }
    else if (header->mode == 1)
    {
        /* Loop mode: keep full timeline, expanding each loop block in-place. */
        for (i = 0; i < (size_t)header->blockCount; ++i)
        {
            size_t start = (size_t)blocks[i].startSample;
            size_t end = (size_t)blocks[i].endSample;

            if (start > srcFrames) start = srcFrames;
            if (end > srcFrames) end = srcFrames;
            if (start < cursor) start = cursor;

            if (start > cursor)
            {
                if (append_samples(out, src + cursor, start - cursor) != 0)
                {
                    return 1;
                }
            }

            if (append_segment_repeats(out, src, srcFrames, &blocks[i]) != 0)
            {
                return 1;
            }

            if (end > cursor)
            {
                cursor = end;
            }
        }

        if (cursor < srcFrames)
        {
            if (append_samples(out, src + cursor, srcFrames - cursor) != 0)
            {
                return 1;
            }
        }
    }
    else if (header->mode == 2)
    {
        /* Play mode: playlist is built from blocks only. */
        for (i = 0; i < (size_t)header->blockCount; ++i)
        {
            if (append_segment_repeats(out, src, srcFrames, &blocks[i]) != 0)
            {
                return 1;
            }
        }
    }
    else
    {
        fprintf(stderr, "Warning: unknown ADP mode %u, using decoded stream without block emulation\n",
                (unsigned int)header->mode);
        if (append_samples(out, src, srcFrames) != 0)
        {
            return 1;
        }
    }

    if (header->repeatDelayMs > 0)
    {
        size_t delayFrames = ((size_t)header->sampleRate * (size_t)header->repeatDelayMs) / 1000U;
        if (append_silence(out, delayFrames) != 0)
        {
            return 1;
        }
    }

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
    AdpHeader header;
    AdpBlock *blocks = NULL;

    unsigned char *rotatedPayload = NULL;
    size_t payloadSize;
    size_t i;

    char tempG722Path[64] = {0};
    char tempPcmPath[64] = {0};

    int16_t *decodedPcm = NULL;
    size_t decodedFrames = 0;
    SampleBuffer emulated;

    int result = 1;

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

    if (parse_header_and_blocks(fileData, fileSize, &header, &blocks) != 0)
    {
        goto cleanup;
    }

    payloadSize = fileSize - header.payloadOffset;
    rotatedPayload = (unsigned char *)malloc(payloadSize);
    if (!rotatedPayload)
    {
        fprintf(stderr, "Error: out of memory allocating %zu payload bytes\n", payloadSize);
        goto cleanup;
    }

    for (i = 0; i < payloadSize; ++i)
    {
        rotatedPayload[i] = rotate_right_2(fileData[header.payloadOffset + i]);
    }

    if (verbose)
    {
        fprintf(stderr,
                "Header: sample_rate=%u encoder=%u legacy_len=%u mode=%u blocks=%u repeat_delay_ms=%u payload_offset=%zu payload_bytes=%zu\n",
                (unsigned int)header.sampleRate,
                (unsigned int)header.encoderField,
                (unsigned int)header.legacyLength,
                (unsigned int)header.mode,
                (unsigned int)header.blockCount,
                (unsigned int)header.repeatDelayMs,
                header.payloadOffset,
                payloadSize);
        for (i = 0; i < (size_t)header.blockCount; ++i)
        {
            fprintf(stderr,
                    "  block[%zu]: start=%u end=%u count=%u flags=0x%04x\n",
                    i,
                    (unsigned int)blocks[i].startSample,
                    (unsigned int)blocks[i].endSample,
                    (unsigned int)blocks[i].count,
                    (unsigned int)blocks[i].flags);
        }
    }

    if (rawOutputPath)
    {
        if (write_entire_file(rawOutputPath, rotatedPayload, payloadSize) != 0)
        {
            goto cleanup;
        }
    }

    if (write_temp_file(rotatedPayload, payloadSize, tempG722Path, sizeof(tempG722Path)) != 0)
    {
        goto cleanup;
    }

    if (run_ffmpeg_decode_pcm(tempG722Path, header.sampleRate, tempPcmPath, sizeof(tempPcmPath)) != 0)
    {
        goto cleanup;
    }

    if (load_pcm16_mono(tempPcmPath, &decodedPcm, &decodedFrames) != 0)
    {
        goto cleanup;
    }

    if (emulate_playback(&emulated, decodedPcm, decodedFrames, &header, blocks) != 0)
    {
        goto cleanup;
    }

    if (write_wav_pcm16_mono(outputPath, header.sampleRate, emulated.data, emulated.length) != 0)
    {
        goto cleanup;
    }

    if (verbose)
    {
        fprintf(stderr, "Decoded frames=%zu, output frames=%zu\n", decodedFrames, emulated.length);
        fprintf(stderr, "Wrote WAV '%s'\n", outputPath);
    }

    result = 0;

cleanup:
    if (tempG722Path[0] != '\0')
    {
        unlink(tempG722Path);
    }
    if (tempPcmPath[0] != '\0')
    {
        unlink(tempPcmPath);
    }
    free(emulated.data);
    free(decodedPcm);
    free(rotatedPayload);
    free(blocks);
    free(fileData);

    return result;
}
