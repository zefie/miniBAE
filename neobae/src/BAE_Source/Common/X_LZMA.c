/*****************************************************************************/
/*
**  X_LZMA.c
**
**  LZMA compression/decompression wrappers for miniBAE.
**  Used by ZMF containers in place of LZSS for ECMI/CMID and CSND resources.
**
**  Uses bundled 7zip SDK (LZMA2 directly) for new payloads and keeps
**  backward compatibility with legacy XZ-wrapped payloads.
**
**  2026.03.21  zefie  Created
*/
/*****************************************************************************/

#include "X_API.h"

#if USE_LZMA_COMPRESSION == TRUE

#include <string.h>

#include "Alloc.h"
#include "Lzma2Dec.h"
#include "Lzma2Enc.h"
#include "Xz.h"
#include "XzCrc64.h"
#include "7zCrc.h"

#define ZMF_LZMA2_MAGIC_0 ((unsigned char)'Z')
#define ZMF_LZMA2_MAGIC_1 ((unsigned char)'L')
#define ZMF_LZMA2_MAGIC_2 ((unsigned char)'2')
#define ZMF_LZMA2_MAGIC_3 ((unsigned char)'1')
#define ZMF_LZMA2_HEADER_SIZE 5

#define ZMF_LZMA2_CHUNK_UNPACK_MAX (1u << 16)

static uint32_t PV_LZMA2PayloadBound(uint32_t srcBytes)
{
    uint64_t chunks;
    uint64_t bound;

    chunks = ((uint64_t)srcBytes + (uint64_t)ZMF_LZMA2_CHUNK_UNPACK_MAX - 1u) /
             (uint64_t)ZMF_LZMA2_CHUNK_UNPACK_MAX;

    /*
     * Conservative bound for LZMA2 stream payload:
     * - raw payload
     * - per-chunk headers / fallbacks
     * - end marker and extra slack
     */
    bound = (uint64_t)srcBytes + (chunks * 32u) + 512u;
    if (bound > 0xFFFFFFFFu)
    {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)bound;
}

static bool PV_IsZMFNativeLZMA2(const unsigned char *src, uint32_t srcBytes)
{
    if (!src || srcBytes < ZMF_LZMA2_HEADER_SIZE)
    {
        return FALSE;
    }

    return (src[0] == ZMF_LZMA2_MAGIC_0 &&
            src[1] == ZMF_LZMA2_MAGIC_1 &&
            src[2] == ZMF_LZMA2_MAGIC_2 &&
            src[3] == ZMF_LZMA2_MAGIC_3) ? TRUE : FALSE;
}

static bool PV_IsXZStream(const unsigned char *src, uint32_t srcBytes)
{
    if (!src || srcBytes < XZ_SIG_SIZE)
    {
        return FALSE;
    }

    return (XMemCmp(src, XZ_SIG, XZ_SIG_SIZE) == 0) ? TRUE : FALSE;
}

static bool PV_DecodeNativeLZMA2(const unsigned char *src, uint32_t srcBytes,
                                 unsigned char *dst, uint32_t dstBytes)
{
    SizeT srcLen;
    SizeT dstLen;
    ELzmaStatus status;
    SRes res;
    Byte prop;

    if (!PV_IsZMFNativeLZMA2(src, srcBytes))
    {
        return FALSE;
    }

    prop = src[4];
    srcLen = (SizeT)(srcBytes - ZMF_LZMA2_HEADER_SIZE);
    dstLen = (SizeT)dstBytes;

    res = Lzma2Decode(dst,
                      &dstLen,
                      src + ZMF_LZMA2_HEADER_SIZE,
                      &srcLen,
                      prop,
                      LZMA_FINISH_END,
                      &status,
                      &g_Alloc);

    return (res == SZ_OK &&
            dstLen == (SizeT)dstBytes &&
            status == LZMA_STATUS_FINISHED_WITH_MARK) ? TRUE : FALSE;
}

static bool PV_DecodeLegacyXZ(const unsigned char *src, uint32_t srcBytes,
                              unsigned char *dst, uint32_t dstBytes)
{
    CXzUnpacker unpacker;
    SizeT srcLen;
    SizeT dstLen;
    ECoderStatus status;
    SRes res;
    BoolInt finished;

    if (!PV_IsXZStream(src, srcBytes))
    {
        return FALSE;
    }

    XzUnpacker_Construct(&unpacker, &g_Alloc);
    XzUnpacker_Init(&unpacker);

    srcLen = (SizeT)srcBytes;
    dstLen = (SizeT)dstBytes;
    res = XzUnpacker_CodeFull(&unpacker,
                              dst,
                              &dstLen,
                              src,
                              &srcLen,
                              CODER_FINISH_END,
                              &status);
    finished = XzUnpacker_IsStreamWasFinished(&unpacker);
    XzUnpacker_Free(&unpacker);

    return (res == SZ_OK && finished && dstLen == (SizeT)dstBytes) ? TRUE : FALSE;
}

/* ---------- single-shot compress ---------- */

/* Compress srcBytes of data from src into dst.
 * dst must be at least LZMACompressBound(srcBytes) bytes.
 * Returns the number of compressed bytes written on success, or -1 on failure. */
int32_t LZMACompress(unsigned char *src, uint32_t srcBytes, unsigned char *dst,
                     XCompressStatusProc proc, void *procData)
{
    CLzma2EncProps props;
    CLzma2EncHandle enc;
    size_t outSize;
    size_t outCap;
    Byte prop;
    SRes res;

    if (!src || !dst || srcBytes == 0)
    {
        return -1;
    }

    (void)proc;
    (void)procData;

    enc = Lzma2Enc_Create(&g_Alloc, &g_BigAlloc);
    if (!enc)
    {
        return -1;
    }

    Lzma2EncProps_Init(&props);
    props.lzmaProps.numThreads = 1;
    props.numTotalThreads = 1;

    res = Lzma2Enc_SetProps(enc, &props);
    if (res != SZ_OK)
    {
        Lzma2Enc_Destroy(enc);
        return -1;
    }

    prop = Lzma2Enc_WriteProperties(enc);
    outCap = (size_t)PV_LZMA2PayloadBound(srcBytes);
    outSize = outCap;

    res = Lzma2Enc_Encode2(enc,
                           NULL,
                           dst + ZMF_LZMA2_HEADER_SIZE,
                           &outSize,
                           NULL,
                           src,
                           (size_t)srcBytes,
                           NULL);
    if (res != SZ_OK)
    {
        Lzma2Enc_Destroy(enc);
        return -1;
    }

    dst[0] = ZMF_LZMA2_MAGIC_0;
    dst[1] = ZMF_LZMA2_MAGIC_1;
    dst[2] = ZMF_LZMA2_MAGIC_2;
    dst[3] = ZMF_LZMA2_MAGIC_3;
    dst[4] = prop;

    Lzma2Enc_Destroy(enc);

    if (outSize > (size_t)0x7FFFFFFF - ZMF_LZMA2_HEADER_SIZE)
    {
        return -1;
    }
    return (int32_t)(outSize + ZMF_LZMA2_HEADER_SIZE);
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
    if (!src || !dst || srcBytes == 0 || dstBytes == 0)
    {
        return;
    }

    CrcGenerateTable();
    Crc64GenerateTable();
    Sha256Prepare();

    if (PV_DecodeNativeLZMA2(src, srcBytes, dst, dstBytes))
    {
        return;
    }

    (void)PV_DecodeLegacyXZ(src, srcBytes, dst, dstBytes);
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
    uint64_t bound;

    bound = (uint64_t)PV_LZMA2PayloadBound(srcBytes) + (uint64_t)ZMF_LZMA2_HEADER_SIZE;
    if (bound > 0xFFFFFFFFu)
    {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)bound;
}

#endif /* USE_LZMA_COMPRESSION == TRUE */
