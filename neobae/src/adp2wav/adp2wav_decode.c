/****************************************************************************
 *
 * adp2wav_decode.c
 *
 * Shared ADP (audio/g722 wrapper) decoder used by the adp2wav CLI and
 * directly by NeoBAE when loading ADP files.
 *
 ****************************************************************************/

#include "adp2wav_decode.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "../thirdparty/libg722/g722_decoder.h"

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

static void adp_log(BAEAdpDecodeOptions const *options, char const *fmt, ...)
{
    va_list args;

    if (!options || !options->logFile)
    {
        return;
    }

    va_start(args, fmt);
    vfprintf(options->logFile, fmt, args);
    va_end(args);
}

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

static unsigned char rotate_right_2(unsigned char value)
{
    return (unsigned char)((value >> 2) | (value << 6));
}

static int write_entire_file(char const *path,
                             unsigned char const *data,
                             size_t size,
                             BAEAdpDecodeOptions const *options)
{
    FILE *file = fopen(path, "wb");
    if (!file)
    {
        adp_log(options, "Error: cannot create '%s'\n", path);
        return 1;
    }
    if (size > 0 && fwrite(data, 1, size, file) != size)
    {
        adp_log(options, "Error: failed writing '%s'\n", path);
        fclose(file);
        return 1;
    }
    if (fclose(file) != 0)
    {
        adp_log(options, "Error: failed closing '%s'\n", path);
        return 1;
    }
    return 0;
}

int BAEAdp_IsFileData(void const *data, size_t size)
{
    unsigned char const *bytes = (unsigned char const *)data;

    if (!bytes || size < ADP_BASE_HEADER_SIZE)
    {
        return 0;
    }
    return memcmp(bytes, "audio/g722", 10) == 0;
}

static int parse_header_and_blocks(unsigned char const *data,
                                   size_t size,
                                   AdpHeader *header,
                                   AdpBlock **outBlocks,
                                   BAEAdpDecodeOptions const *options)
{
    uint16_t modeAndCount;
    size_t blocksBytes;
    size_t i;
    AdpBlock *blocks;

    *outBlocks = NULL;

    if (size < ADP_BASE_HEADER_SIZE)
    {
        adp_log(options, "Error: file is too small for ADP base header\n");
        return 1;
    }
    if (!BAEAdp_IsFileData(data, size))
    {
        adp_log(options, "Error: unsupported ADP header prefix\n");
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
        adp_log(options, "Error: invalid ADP sample rate 0\n");
        return 1;
    }

    blocksBytes = (size_t)header->blockCount * ADP_BLOCK_SIZE;
    if (size < ADP_BASE_HEADER_SIZE + blocksBytes)
    {
        adp_log(options,
                "Error: ADP block section truncated (count=%u)\n",
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
        adp_log(options,
                "Error: out of memory allocating %u blocks\n",
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

static int decode_g722_to_pcm(unsigned char const *g722Data,
                              size_t g722Size,
                              int16_t **outPcm,
                              size_t *outFrames,
                              BAEAdpDecodeOptions const *options)
{
    G722_DEC_CTX *decoder;
    int16_t *pcmBuffer;
    size_t maxSamples;
    int decodedCount;

    *outPcm = NULL;
    *outFrames = 0;

    if (!g722Data || g722Size == 0)
    {
        return 1;
    }

    maxSamples = g722Size * 2;
    pcmBuffer = (int16_t *)malloc(maxSamples * sizeof(int16_t));
    if (!pcmBuffer)
    {
        adp_log(options, "Error: out of memory allocating %zu samples\n", maxSamples);
        return 1;
    }

    decoder = g722_decoder_new(64000, G722_DEFAULT);
    if (!decoder)
    {
        adp_log(options, "Error: failed to create G.722 decoder\n");
        free(pcmBuffer);
        return 1;
    }

    decodedCount = g722_decode(decoder, g722Data, (int)g722Size, pcmBuffer);
    g722_decoder_destroy(decoder);
    if (decodedCount < 0)
    {
        adp_log(options, "Error: G.722 decode failed\n");
        free(pcmBuffer);
        return 1;
    }

    *outPcm = pcmBuffer;
    *outFrames = (size_t)decodedCount;
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
    reps = (size_t)block->count + 1;
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
                            AdpBlock const *blocks,
                            BAEAdpDecodeOptions const *options)
{
    size_t cursor = 0;
    size_t i;

    out->data = NULL;
    out->length = 0;
    out->capacity = 0;

    if (header->mode == 0 || header->blockCount == 0)
    {
        return append_samples(out, src, srcFrames);
    }
    if (header->mode == 1)
    {
        for (i = 0; i < (size_t)header->blockCount; ++i)
        {
            size_t start = (size_t)blocks[i].startSample;
            size_t end = (size_t)blocks[i].endSample;

            if (start > srcFrames) start = srcFrames;
            if (end > srcFrames) end = srcFrames;
            if (start < cursor) start = cursor;
            if (start > cursor && append_samples(out, src + cursor, start - cursor) != 0)
            {
                return 1;
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
        if (cursor < srcFrames && append_samples(out, src + cursor, srcFrames - cursor) != 0)
        {
            return 1;
        }
    }
    else if (header->mode == 2)
    {
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
        adp_log(options,
                "Warning: unknown ADP mode %u, using decoded stream without block emulation\n",
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

int BAEAdp_DecodeMemoryToPCM16Mono(void const *data,
                                   size_t size,
                                   BAEAdpDecodeOptions const *options,
                                   BAEAdpDecodedAudio *outAudio)
{
    unsigned char const *fileData = (unsigned char const *)data;
    AdpHeader header;
    AdpBlock *blocks = NULL;
    unsigned char *rotatedPayload = NULL;
    size_t payloadSize;
    size_t i;
    int16_t *decodedPcm = NULL;
    size_t decodedFrames = 0;
    SampleBuffer emulated;
    int result = 1;

    if (!outAudio)
    {
        return 1;
    }

    outAudio->samples = NULL;
    outAudio->frameCount = 0;
    outAudio->sampleRate = 0;
    emulated.data = NULL;
    emulated.length = 0;
    emulated.capacity = 0;

    if (parse_header_and_blocks(fileData, size, &header, &blocks, options) != 0)
    {
        goto cleanup;
    }

    payloadSize = size - header.payloadOffset;
    rotatedPayload = (unsigned char *)malloc(payloadSize);
    if (!rotatedPayload)
    {
        adp_log(options, "Error: out of memory allocating %zu payload bytes\n", payloadSize);
        goto cleanup;
    }

    for (i = 0; i < payloadSize; ++i)
    {
        rotatedPayload[i] = rotate_right_2(fileData[header.payloadOffset + i]);
    }

    if (options && options->verbose)
    {
        adp_log(options,
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
            adp_log(options,
                    "  block[%zu]: start=%u end=%u count=%u flags=0x%04x\n",
                    i,
                    (unsigned int)blocks[i].startSample,
                    (unsigned int)blocks[i].endSample,
                    (unsigned int)blocks[i].count,
                    (unsigned int)blocks[i].flags);
        }
    }

    if (options && options->rawG722OutputPath)
    {
        if (write_entire_file(options->rawG722OutputPath, rotatedPayload, payloadSize, options) != 0)
        {
            goto cleanup;
        }
    }

    if (decode_g722_to_pcm(rotatedPayload, payloadSize, &decodedPcm, &decodedFrames, options) != 0)
    {
        goto cleanup;
    }

    if (options && options->verbose)
    {
        adp_log(options, "G.722 decode: %zu bytes -> %zu samples\n", payloadSize, decodedFrames);
    }

    if (emulate_playback(&emulated, decodedPcm, decodedFrames, &header, blocks, options) != 0)
    {
        goto cleanup;
    }

    if (options && options->verbose)
    {
        adp_log(options, "Decoded frames=%zu, output frames=%zu\n", decodedFrames, emulated.length);
    }

    outAudio->samples = emulated.data;
    outAudio->frameCount = emulated.length;
    outAudio->sampleRate = header.sampleRate;
    emulated.data = NULL;
    result = 0;

cleanup:
    free(emulated.data);
    free(decodedPcm);
    free(rotatedPayload);
    free(blocks);
    return result;
}

void BAEAdp_FreeDecodedAudio(BAEAdpDecodedAudio *audio)
{
    if (!audio)
    {
        return;
    }
    free(audio->samples);
    audio->samples = NULL;
    audio->frameCount = 0;
    audio->sampleRate = 0;
}