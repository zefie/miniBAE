/*
    CRI ADX decoding for NeoBAE
*/

#include "X_API.h"
#include "X_Formats.h"
#include "X_Assert.h"
#include "NeoBAE.h"
#include "../../adx2wav/adx2wav_decode.h"
#include <string.h>

OPErr XExpandADX(GM_Waveform const* src, uint32_t startFrame, GM_Waveform* dst) {
    return PARAM_ERR; // Not supported for block expansion yet
}

GM_Waveform *PV_ReadADXIntoMemoryFromMemory(void *pMemoryFile, uint32_t memoryFileSize, OPErr *pErr) {
    GM_Waveform *wave = NULL;
    OPErr err = NO_ERR;
    BAEAdxDecodedAudio decoded;
    memset(&decoded, 0, sizeof(decoded));

    if (!pMemoryFile || memoryFileSize == 0)
    {
        if (pErr) *pErr = BAD_FILE;
        return NULL;
    }

    if (BAEAdx_DecodeMemoryToPCM16(pMemoryFile, memoryFileSize, NULL, &decoded) != 0)
    {
        if (pErr) *pErr = BAD_FILE_TYPE;
        return NULL;
    }

    if (decoded.frameCount > 0xFFFFFFFFu)
    {
        BAEAdx_FreeDecodedAudio(&decoded);
        if (pErr) *pErr = BAD_FILE;
        return NULL;
    }

    wave = GM_ReadRawAudioIntoMemoryFromMemory(decoded.samples,
                                               (uint32_t)decoded.frameCount,
                                               16,
                                               decoded.channels,
                                               LONG_TO_UNSIGNED_FIXED(decoded.sampleRate),
                                               0,
                                               0,
                                               &err);

    if (wave && decoded.hasLoop)
    {
        wave->waveFrames = (uint32_t)decoded.frameCount;
        wave->startLoop = (int32_t)decoded.loopStart;
        wave->endLoop = (int32_t)decoded.loopEnd;
        wave->numLoops = 0xFFFFFFFF;
    }

    BAEAdx_FreeDecodedAudio(&decoded);

    if (pErr) *pErr = err;
    return wave;
}
