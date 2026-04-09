/*
    XQOAFiles.c
    QOA sample encode/decode helpers for SND Type3 resources.
*/

#include "X_Formats.h"

#if USE_QOA_SUPPORT == TRUE

#include <stdlib.h>

#define QOA_IMPLEMENTATION
#include "../../thirdparty/qoa/qoa.h"

static unsigned int PV_FixedToRate(XFIXED rate)
{
    if (rate <= 0)
    {
        return 44100U;
    }
    return (unsigned int)(rate >> 16);
}

OPErr XExpandQOA(GM_Waveform const* src, uint32_t startFrame, GM_Waveform* dst)
{
    qoa_desc desc;
    short *decoded;
    uint32_t totalFrames;
    uint32_t outFrames;
    uint32_t outSamples;
    uint32_t outBytes;

    if (!src || !dst || !src->theWaveform || src->waveSize == 0)
    {
        return PARAM_ERR;
    }

    XSetMemory(&desc, sizeof(desc), 0);
    decoded = qoa_decode((const unsigned char *)src->theWaveform, (int)src->waveSize, &desc);
    if (!decoded)
    {
        return BAD_FILE;
    }

    if (desc.channels == 0 || desc.channels > 2)
    {
        free(decoded);
        return BAD_FILE;
    }

    totalFrames = desc.samples;
    if (startFrame > totalFrames)
    {
        free(decoded);
        return BAD_FILE;
    }

    outFrames = totalFrames - startFrame;
    if (src->waveFrames > 0 && outFrames > src->waveFrames)
    {
        outFrames = src->waveFrames;
    }

    outSamples = outFrames * desc.channels;
    outBytes = outSamples * sizeof(short);

    dst->channels = (int16_t)desc.channels;
    dst->bitSize = 16;
    dst->sampledRate = (XFIXED)((uint32_t)desc.samplerate << 16);
    dst->waveFrames = outFrames;
    dst->waveSize = outBytes;
    dst->compressionType = C_NONE;
    dst->theWaveform = XNewPtr((int32_t)outBytes);
    if (!dst->theWaveform)
    {
        free(decoded);
        return MEMORY_ERR;
    }

    XBlockMove((XPTR)(decoded + (startFrame * desc.channels)), dst->theWaveform, (int32_t)outBytes);
    free(decoded);
    return NO_ERR;
}

OPErr XEncodeQOAToMemory(GM_Waveform const *src, XPTR *outData, uint32_t *outSize)
{
    qoa_desc desc;
    unsigned int encodedLen;
    void *encoded;

    if (!src || !outData || !outSize || !src->theWaveform)
    {
        return PARAM_ERR;
    }

    *outData = NULL;
    *outSize = 0;

    if (src->compressionType != (uint32_t)C_NONE)
    {
        return PARAM_ERR;
    }
    if (src->bitSize != 16)
    {
        return NOT_SETUP;
    }
    if (src->channels < 1 || src->channels > 2)
    {
        return NOT_SETUP;
    }
    if (src->waveFrames == 0)
    {
        return BAD_FILE;
    }

    XSetMemory(&desc, sizeof(desc), 0);
    desc.channels = (unsigned int)src->channels;
    desc.samplerate = PV_FixedToRate(src->sampledRate);
    desc.samples = src->waveFrames;

    encodedLen = 0;
    encoded = qoa_encode((const short *)src->theWaveform, &desc, &encodedLen);
    if (!encoded || encodedLen == 0)
    {
        if (encoded)
        {
            free(encoded);
        }
        return BAD_FILE;
    }

    *outData = XNewPtr((int32_t)encodedLen);
    if (!*outData)
    {
        free(encoded);
        return MEMORY_ERR;
    }

    XBlockMove((XPTR)encoded, *outData, (int32_t)encodedLen);
    *outSize = (uint32_t)encodedLen;
    free(encoded);
    return NO_ERR;
}

#endif
