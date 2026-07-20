/*
    CRI ADX decoding for NeoBAE
*/

#include "X_API.h"
#include "X_Formats.h"
#include "X_Assert.h"
#include "NeoBAE.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

// Helper to read big endian
static uint16_t ReadBE16(const unsigned char *data) {
    return (data[0] << 8) | data[1];
}

static uint32_t ReadBE32(const unsigned char *data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

OPErr XExpandADX(GM_Waveform const* src, uint32_t startFrame, GM_Waveform* dst) {
    return PARAM_ERR; // Not supported for block expansion yet
}

GM_Waveform *PV_ReadADXIntoMemoryFromMemory(void *pMemoryFile, uint32_t memoryFileSize, OPErr *pErr) {
    const unsigned char *data = (const unsigned char *)pMemoryFile;
    GM_Waveform *wave = NULL;
    OPErr err = NO_ERR;
    uint16_t magic;
    uint16_t headerSize;
    uint8_t encodingType;
    uint8_t frameSize;
    uint8_t bitDepth;
    uint8_t channels;
    uint32_t sampleRate;
    uint32_t totalSamples;
    int32_t loopStart = 0;
    int32_t loopEnd = 0;
    
    if (!data || memoryFileSize < 16) {
        if (pErr) *pErr = BAD_FILE;
        return NULL;
    }
    
    magic = ReadBE16(data);
    if (magic != 0x8000) {
        if (pErr) *pErr = BAD_FILE_TYPE;
        return NULL;
    }
    
    headerSize = ReadBE16(data + 2);
    if ((uint32_t)headerSize + 4 > memoryFileSize) {
        if (pErr) *pErr = BAD_FILE;
        return NULL;
    }
    
    encodingType = data[4];
    frameSize = data[5];
    bitDepth = data[6];
    channels = data[7];
    sampleRate = ReadBE32(data + 8);
    totalSamples = ReadBE32(data + 12);
    
    if ((encodingType != 0x03 && encodingType != 0x04) || frameSize != 18 || bitDepth != 4 || (channels != 1 && channels != 2)) {
        if (pErr) *pErr = BAD_FILE_TYPE;
        return NULL;
    }
    
    // Check for loop points based on header type at offset 0x12
    uint8_t headerType = data[18];
    if (headerType == 4 && headerSize >= 56) {
        // Type 04 loop structure (fixed offsets starting at 0x24)
        uint32_t loopFlag = ReadBE32(data + 0x24);
        if (loopFlag == 1) {
            loopStart = ReadBE32(data + 0x28);
            loopEnd = ReadBE32(data + 0x30);
        }
    } else if (headerSize >= 36) {
        // Type 03 loop structure (fixed offsets starting at 0x18)
        uint16_t loopFlag = ReadBE16(data + 0x18);
        if (loopFlag == 1 || loopFlag == 2) {
            loopStart = ReadBE32(data + 0x1c);
            loopEnd = ReadBE32(data + 0x20);
        }
    }
    
    if (totalSamples > 0xFFFFFFFFu / (channels * sizeof(int16_t))) {
        if (pErr) *pErr = BAD_FILE;
        return NULL;
    }
    
    int16_t *outSamples = (int16_t *)XNewPtr(totalSamples * channels * sizeof(int16_t));
    if (!outSamples) {
        if (pErr) *pErr = MEMORY_ERR;
        return NULL;
    }
    
    // Calculate coefficients
    double z = cos(2.0 * M_PI * 500.0 / sampleRate);
    double a = M_SQRT2 - z;
    double b = M_SQRT2 - 1.0;
    double c = (a - sqrt((a + b) * (a - b))) / b;
    
    int32_t coef1 = (int32_t)floor(c * 8192);
    int32_t coef2 = (int32_t)floor(c * c * -4096);
    
    int32_t hist1[2] = {0, 0};
    int32_t hist2[2] = {0, 0};
    
    const unsigned char *inData = data + headerSize + 4;
    uint32_t sampleOffset = 0;
    
    // Read frames
    while (sampleOffset < totalSamples) {
        for (int ch = 0; ch < channels; ch++) {
            if (inData + frameSize > data + memoryFileSize) {
                break; // EOF
            }
            
            int32_t scale = ReadBE16(inData);
            const unsigned char *nibbles = inData + 2;
            
            for (int i = 0; i < 32; i++) {
                int nibble = (i % 2 == 0) ? (nibbles[i / 2] >> 4) : (nibbles[i / 2] & 0x0F);
                if (nibble & 0x08) nibble -= 16;
                
                int32_t sample = (scale * nibble) + ((coef1 * hist1[ch] + coef2 * hist2[ch]) >> 12);
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                
                hist2[ch] = hist1[ch];
                hist1[ch] = sample;
                
                if (sampleOffset + i < totalSamples) {
                    outSamples[(sampleOffset + i) * channels + ch] = (int16_t)sample;
                }
            }
            
            inData += frameSize;
        }
        sampleOffset += 32;
    }
    
    wave = GM_ReadRawAudioIntoMemoryFromMemory(outSamples,
                                               totalSamples,
                                               16,
                                               channels,
                                               LONG_TO_UNSIGNED_FIXED(sampleRate),
                                               0,
                                               0,
                                               &err);
    if (wave && loopStart < loopEnd && loopEnd <= (int32_t)totalSamples) {
        wave->waveFrames = totalSamples; // ensure frames are set
        // Support loops. Wait, NeoBAE loop points.
        wave->startLoop = loopStart;
        wave->endLoop = loopEnd;
        wave->numLoops = 0xFFFFFFFF;
        debug_message("ADX: loopStart=%d, loopEnd=%d, totalSamples=%d\n", loopStart, loopEnd, (int32_t)totalSamples);
    } else if (wave) {
        debug_message("ADX: Loop points invalid or not present. loopStart=%d, loopEnd=%d, totalSamples=%d\n", loopStart, loopEnd, (int32_t)totalSamples);
    }
    
    XDisposePtr(outSamples);
    
    if (pErr) *pErr = err;
    return wave;
}
