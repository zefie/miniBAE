#ifndef ADP2WAV_DECODE_H
#define ADP2WAV_DECODE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct BAEAdpDecodeOptions
{
    int verbose;
    char const *rawG722OutputPath;
    FILE *logFile;
} BAEAdpDecodeOptions;

typedef struct BAEAdpDecodedAudio
{
    int16_t *samples;
    size_t frameCount;
    uint32_t sampleRate;
} BAEAdpDecodedAudio;

int BAEAdp_IsFileData(void const *data, size_t size);
int BAEAdp_DecodeMemoryToPCM16Mono(void const *data,
                                   size_t size,
                                   BAEAdpDecodeOptions const *options,
                                   BAEAdpDecodedAudio *outAudio);
void BAEAdp_FreeDecodedAudio(BAEAdpDecodedAudio *audio);

#endif