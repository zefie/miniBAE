/****************************************************************************
 *
 * adx2wav_decode.c
 *
 * Shared ADX decoder used by the adx2wav CLI.
 *
 ****************************************************************************/

#include "adx2wav_decode.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

#define ADX_MAGIC 0x8000
#define ADX_FRAME_SIZE 18
#define ADX_SAMPLES_PER_FRAME 32

static void adx_log(BAEAdxDecodeOptions const *options, char const *fmt, ...)
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

int BAEAdx_IsFileData(void const *data, size_t size)
{
    unsigned char const *bytes = (unsigned char const *)data;
    uint16_t magic;

    if (!bytes || size < 16)
    {
        return 0;
    }

    magic = read_be_u16(bytes);
    return magic == ADX_MAGIC;
}

int BAEAdx_DecodeMemoryToPCM16(void const *data,
                               size_t size,
                               BAEAdxDecodeOptions const *options,
                               BAEAdxDecodedAudio *outAudio)
{
    unsigned char const *bytes = (unsigned char const *)data;
    uint16_t headerSize;
    uint8_t encodingType;
    uint8_t frameSize;
    uint8_t bitDepth;
    uint8_t channels;
    uint8_t headerType;
    uint32_t sampleRate;
    uint32_t totalSamples;
    int hasLoop = 0;
    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    int16_t *samples = NULL;
    int32_t hist1[2] = {0, 0};
    int32_t hist2[2] = {0, 0};
    int32_t coef1;
    int32_t coef2;
    double z;
    double a;
    double b;
    double c;
    unsigned char const *inData;
    unsigned char const *endData;
    uint32_t frameBase;
    uint32_t producedFrames = 0;
    uint32_t ch;

    if (!outAudio)
    {
        return 1;
    }

    outAudio->samples = NULL;
    outAudio->frameCount = 0;
    outAudio->sampleRate = 0;
    outAudio->channels = 0;
    outAudio->hasLoop = 0;
    outAudio->loopStart = 0;
    outAudio->loopEnd = 0;

    if (!BAEAdx_IsFileData(bytes, size))
    {
        adx_log(options, "Error: unsupported ADX header\n");
        return 1;
    }

    headerSize = read_be_u16(bytes + 2);
    if ((size_t)headerSize + 4 > size)
    {
        adx_log(options, "Error: ADX header size is invalid\n");
        return 1;
    }

    encodingType = bytes[4];
    frameSize = bytes[5];
    bitDepth = bytes[6];
    channels = bytes[7];
    sampleRate = read_be_u32(bytes + 8);
    totalSamples = read_be_u32(bytes + 12);

    if ((encodingType != 0x03 && encodingType != 0x04) ||
        frameSize != ADX_FRAME_SIZE ||
        bitDepth != 4 ||
        (channels != 1 && channels != 2) ||
        sampleRate == 0)
    {
        adx_log(options,
                "Error: unsupported ADX format (enc=%u frame=%u bits=%u channels=%u rate=%u)\n",
                (unsigned int)encodingType,
                (unsigned int)frameSize,
                (unsigned int)bitDepth,
                (unsigned int)channels,
                (unsigned int)sampleRate);
        return 1;
    }

    headerType = (size > 18) ? bytes[18] : 0;
    if (headerType == 4 && headerSize >= 56 && size >= 0x34)
    {
        uint32_t loopFlag = read_be_u32(bytes + 0x24);
        if (loopFlag == 1)
        {
            loopStart = read_be_u32(bytes + 0x28);
            loopEnd = read_be_u32(bytes + 0x30);
            hasLoop = 1;
        }
    }
    else if (headerSize >= 40 && size >= 0x2c)
    {
        uint32_t loopFlag32 = read_be_u32(bytes + 0x18);
        if (loopFlag32 == 1)
        {
            loopStart = read_be_u32(bytes + 0x1c);
            loopEnd = read_be_u32(bytes + 0x24);
            hasLoop = 1;
        }
    }
    else if (headerSize >= 36 && size >= 0x24)
    {
        uint16_t loopFlag16 = read_be_u16(bytes + 0x18);
        if (loopFlag16 == 1 || loopFlag16 == 2)
        {
            loopStart = read_be_u32(bytes + 0x1c);
            loopEnd = read_be_u32(bytes + 0x20);
            hasLoop = 1;
        }
    }

    if (hasLoop)
    {
        if (loopStart >= loopEnd || loopEnd > totalSamples)
        {
            hasLoop = 0;
            loopStart = 0;
            loopEnd = 0;
        }
    }

    if (totalSamples == 0)
    {
        adx_log(options, "Error: ADX contains zero samples\n");
        return 1;
    }

    if ((uint64_t)totalSamples > (uint64_t)SIZE_MAX / ((uint64_t)channels * sizeof(int16_t)))
    {
        adx_log(options, "Error: ADX sample count is too large\n");
        return 1;
    }

    samples = (int16_t *)calloc((size_t)totalSamples * channels, sizeof(int16_t));
    if (!samples)
    {
        adx_log(options, "Error: out of memory allocating PCM buffer\n");
        return 1;
    }

    z = cos(2.0 * M_PI * 500.0 / (double)sampleRate);
    a = M_SQRT2 - z;
    b = M_SQRT2 - 1.0;
    c = (a - sqrt((a + b) * (a - b))) / b;
    coef1 = (int32_t)floor(c * 8192.0);
    coef2 = (int32_t)floor(c * c * -4096.0);

    inData = bytes + headerSize + 4;
    endData = bytes + size;
    frameBase = 0;
    while (frameBase < totalSamples)
    {
        uint32_t frameIndex;
        for (ch = 0; ch < channels; ++ch)
        {
            int32_t scale;
            unsigned char const *nibbles;
            if (inData + frameSize > endData)
            {
                goto done_decode;
            }

            scale = (int32_t)read_be_u16(inData);
            nibbles = inData + 2;
            for (frameIndex = 0; frameIndex < ADX_SAMPLES_PER_FRAME; ++frameIndex)
            {
                uint32_t outFrame = frameBase + frameIndex;
                int nibble = (frameIndex & 1U) == 0U
                                 ? (int)(nibbles[frameIndex >> 1] >> 4)
                                 : (int)(nibbles[frameIndex >> 1] & 0x0F);
                int32_t sample;

                if (nibble & 0x08)
                {
                    nibble -= 16;
                }

                sample = scale * nibble + ((coef1 * hist1[ch] + coef2 * hist2[ch]) >> 12);
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;

                hist2[ch] = hist1[ch];
                hist1[ch] = sample;

                if (outFrame < totalSamples)
                {
                    samples[(size_t)outFrame * channels + ch] = (int16_t)sample;
                    if (outFrame + 1 > producedFrames)
                    {
                        producedFrames = outFrame + 1;
                    }
                }
            }

            inData += frameSize;
        }

        frameBase += ADX_SAMPLES_PER_FRAME;
    }

done_decode:
    if (producedFrames == 0)
    {
        adx_log(options, "Error: ADX did not decode any audio frames\n");
        free(samples);
        return 1;
    }

    if (hasLoop && loopEnd > producedFrames)
    {
        hasLoop = 0;
        loopStart = 0;
        loopEnd = 0;
    }

    if (options && options->verbose)
    {
        adx_log(options,
                "Header: enc=%u frame=%u bits=%u channels=%u sample_rate=%u total_samples=%u decoded=%u loop=%s [%u, %u)\n",
                (unsigned int)encodingType,
                (unsigned int)frameSize,
                (unsigned int)bitDepth,
                (unsigned int)channels,
                (unsigned int)sampleRate,
                (unsigned int)totalSamples,
                (unsigned int)producedFrames,
                hasLoop ? "yes" : "no",
                (unsigned int)loopStart,
                (unsigned int)loopEnd);
    }

    outAudio->samples = samples;
    outAudio->frameCount = (size_t)producedFrames;
    outAudio->sampleRate = sampleRate;
    outAudio->channels = channels;
    outAudio->hasLoop = hasLoop;
    outAudio->loopStart = loopStart;
    outAudio->loopEnd = loopEnd;
    return 0;
}

void BAEAdx_FreeDecodedAudio(BAEAdxDecodedAudio *audio)
{
    if (!audio)
    {
        return;
    }

    free(audio->samples);
    audio->samples = NULL;
    audio->frameCount = 0;
    audio->sampleRate = 0;
    audio->channels = 0;
    audio->hasLoop = 0;
    audio->loopStart = 0;
    audio->loopEnd = 0;
}