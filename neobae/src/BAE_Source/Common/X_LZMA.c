/*****************************************************************************/
/*
**  X_LZMA.c
**
**  LZMA compression/decompression wrappers for miniBAE.
**  Used by ZMF containers in place of LZSS for ECMI/CMID and CSND resources,
**  and by NBS session files in place of zlib.
**
**  Requires liblzma (xz-utils).
**
**  2026.03.21  zefie  Created
*/
/*****************************************************************************/

#include "X_API.h"

#if USE_LZMA_COMPRESSION == TRUE

#include <lzma.h>
#include <string.h>

/* ---------- single-shot compress ---------- */

/* Compress srcBytes of data from src into dst.
 * dst must be at least LZMACompressBound(srcBytes) bytes.
 * Returns the number of compressed bytes written on success, or -1 on failure. */
int32_t LZMACompress(unsigned char *src, uint32_t srcBytes, unsigned char *dst,
                     XCompressStatusProc proc, void *procData)
{
    size_t      outPos;
    lzma_ret    ret;

    if (!src || !dst || srcBytes == 0)
    {
        return -1;
    }

    (void)proc;
    (void)procData;

    outPos = 0;
    ret = lzma_easy_buffer_encode(LZMA_PRESET_DEFAULT,   /* preset 6 */
                                  LZMA_CHECK_CRC64,
                                  NULL,                   /* default allocator */
                                  src, (size_t)srcBytes,
                                  dst, &outPos,
                                  lzma_stream_buffer_bound((size_t)srcBytes));
    if (ret != LZMA_OK)
    {
        return -1;
    }
    return (int32_t)outPos;
}

/* ---------- delta + compress variants ---------- */

/* These mirror the LZSS delta helpers: apply delta encoding in-place,
 * compress, then undo delta encoding so the caller's buffer is unchanged. */

/* --- 8-bit mono delta --- */
static void PV_DeltaMono8(unsigned char *buf, uint32_t bytes)
{
    uint32_t i;
    unsigned char prev = 0;
    unsigned char cur;

    for (i = 0; i < bytes; i++)
    {
        cur = buf[i];
        buf[i] = (unsigned char)(cur - prev);
        prev = cur;
    }
}

static void PV_UnDeltaMono8(unsigned char *buf, uint32_t bytes)
{
    uint32_t i;
    unsigned char prev = 0;

    for (i = 0; i < bytes; i++)
    {
        buf[i] = (unsigned char)(buf[i] + prev);
        prev = buf[i];
    }
}

int32_t LZMACompressDeltaMono8(unsigned char *src, uint32_t srcBytes, unsigned char *dst,
                               XCompressStatusProc proc, void *procData)
{
    int32_t result;

    PV_DeltaMono8(src, srcBytes);
    result = LZMACompress(src, srcBytes, dst, proc, procData);
    PV_UnDeltaMono8(src, srcBytes);
    return result;
}

/* --- 8-bit stereo delta --- */
static void PV_DeltaStereo8(unsigned char *buf, uint32_t frameCount)
{
    uint32_t i;
    unsigned char prevL = 0, prevR = 0;
    unsigned char curL, curR;

    for (i = 0; i < frameCount; i++)
    {
        curL = buf[i * 2];
        curR = buf[i * 2 + 1];
        buf[i * 2]     = (unsigned char)(curL - prevL);
        buf[i * 2 + 1] = (unsigned char)(curR - prevR);
        prevL = curL;
        prevR = curR;
    }
}

static void PV_UnDeltaStereo8(unsigned char *buf, uint32_t frameCount)
{
    uint32_t i;
    unsigned char prevL = 0, prevR = 0;

    for (i = 0; i < frameCount; i++)
    {
        buf[i * 2]     = (unsigned char)(buf[i * 2] + prevL);
        buf[i * 2 + 1] = (unsigned char)(buf[i * 2 + 1] + prevR);
        prevL = buf[i * 2];
        prevR = buf[i * 2 + 1];
    }
}

int32_t LZMACompressDeltaStereo8(unsigned char *src, uint32_t srcBytes, unsigned char *dst,
                                 XCompressStatusProc proc, void *procData)
{
    uint32_t frameCount = srcBytes / 2;
    int32_t result;

    PV_DeltaStereo8(src, frameCount);
    result = LZMACompress(src, srcBytes, dst, proc, procData);
    PV_UnDeltaStereo8(src, frameCount);
    return result;
}

/* --- 16-bit mono delta --- */
static void PV_DeltaMono16(int16_t *buf, uint32_t frameCount)
{
    uint32_t i;
    int16_t prev = 0, cur;

    for (i = 0; i < frameCount; i++)
    {
        cur = buf[i];
        buf[i] = (int16_t)(cur - prev);
        prev = cur;
    }
}

static void PV_UnDeltaMono16(int16_t *buf, uint32_t frameCount)
{
    uint32_t i;
    int16_t prev = 0;

    for (i = 0; i < frameCount; i++)
    {
        buf[i] = (int16_t)(buf[i] + prev);
        prev = buf[i];
    }
}

int32_t LZMACompressDeltaMono16(int16_t *src, uint32_t srcBytes, unsigned char *dst,
                                XCompressStatusProc proc, void *procData)
{
    uint32_t frameCount = srcBytes / 2;
    int32_t result;

    PV_DeltaMono16(src, frameCount);
    result = LZMACompress((unsigned char *)src, srcBytes, dst, proc, procData);
    PV_UnDeltaMono16(src, frameCount);
    return result;
}

/* --- 16-bit stereo delta --- */
static void PV_DeltaStereo16(int16_t *buf, uint32_t frameCount)
{
    uint32_t i;
    int16_t prevL = 0, prevR = 0;
    int16_t curL, curR;

    for (i = 0; i < frameCount; i++)
    {
        curL = buf[i * 2];
        curR = buf[i * 2 + 1];
        buf[i * 2]     = (int16_t)(curL - prevL);
        buf[i * 2 + 1] = (int16_t)(curR - prevR);
        prevL = curL;
        prevR = curR;
    }
}

static void PV_UnDeltaStereo16(int16_t *buf, uint32_t frameCount)
{
    uint32_t i;
    int16_t prevL = 0, prevR = 0;

    for (i = 0; i < frameCount; i++)
    {
        buf[i * 2]     = (int16_t)(buf[i * 2] + prevL);
        buf[i * 2 + 1] = (int16_t)(buf[i * 2 + 1] + prevR);
        prevL = buf[i * 2];
        prevR = buf[i * 2 + 1];
    }
}

int32_t LZMACompressDeltaStereo16(int16_t *src, uint32_t srcBytes, unsigned char *dst,
                                  XCompressStatusProc proc, void *procData)
{
    uint32_t frameCount = srcBytes / 4;
    int32_t result;

    PV_DeltaStereo16(src, frameCount);
    result = LZMACompress((unsigned char *)src, srcBytes, dst, proc, procData);
    PV_UnDeltaStereo16(src, frameCount);
    return result;
}

/* ---------- single-shot decompress ---------- */

void LZMAUncompress(unsigned char *src, uint32_t srcBytes,
                    unsigned char *dst, uint32_t dstBytes)
{
    uint64_t    memlimit;
    size_t      inPos;
    size_t      outPos;

    if (!src || !dst || srcBytes == 0 || dstBytes == 0)
    {
        return;
    }

    memlimit = UINT64_MAX;   /* no memory limit */
    inPos  = 0;
    outPos = 0;
    lzma_ret ret = lzma_stream_buffer_decode(&memlimit,
                              0,        /* flags */
                              NULL,     /* default allocator */
                              src, &inPos, (size_t)srcBytes,
                              dst, &outPos, (size_t)dstBytes);
    (void)ret;
}

/* ---------- delta + decompress variants ---------- */

void LZMAUncompressDeltaMono8(unsigned char *src, uint32_t srcBytes,
                              unsigned char *dst, uint32_t dstBytes)
{
    LZMAUncompress(src, srcBytes, dst, dstBytes);
    PV_UnDeltaMono8(dst, dstBytes);
}

void LZMAUncompressDeltaStereo8(unsigned char *src, uint32_t srcBytes,
                                unsigned char *dst, uint32_t dstBytes)
{
    LZMAUncompress(src, srcBytes, dst, dstBytes);
    PV_UnDeltaStereo8(dst, dstBytes / 2);
}

void LZMAUncompressDeltaMono16(unsigned char *src, uint32_t srcBytes,
                               int16_t *dst, uint32_t dstBytes)
{
    LZMAUncompress(src, srcBytes, (unsigned char *)dst, dstBytes);
    PV_UnDeltaMono16(dst, dstBytes / 2);
}

void LZMAUncompressDeltaStereo16(unsigned char *src, uint32_t srcBytes,
                                 int16_t *dst, uint32_t dstBytes)
{
    LZMAUncompress(src, srcBytes, (unsigned char *)dst, dstBytes);
    PV_UnDeltaStereo16(dst, dstBytes / 4);
}

/* ---------- bound helper ---------- */

uint32_t LZMACompressBound(uint32_t srcBytes)
{
    return (uint32_t)lzma_stream_buffer_bound((size_t)srcBytes);
}

#endif /* USE_LZMA_COMPRESSION == TRUE */
