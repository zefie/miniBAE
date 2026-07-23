#ifndef ADX2WAV_DECODE_H
#define ADX2WAV_DECODE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct BAEAdxDecodeOptions
{
    int verbose;
    FILE *logFile;
} BAEAdxDecodeOptions;

typedef struct BAEAdxDecodedAudio
{
    int16_t *samples;
    size_t frameCount;
    uint32_t sampleRate;
    uint8_t channels;
    int hasLoop;
    uint32_t loopStart;
    uint32_t loopEnd;
} BAEAdxDecodedAudio;

int BAEAdx_IsFileData(void const *data, size_t size);
int BAEAdx_DecodeMemoryToPCM16(void const *data,
                               size_t size,
                               BAEAdxDecodeOptions const *options,
                               BAEAdxDecodedAudio *outAudio);
void BAEAdx_FreeDecodedAudio(BAEAdxDecodedAudio *audio);

#endif