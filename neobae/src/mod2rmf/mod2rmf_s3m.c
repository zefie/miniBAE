/****************************************************************************
 *
 * mod2rmf_s3m.c
 *
 * Scream Tracker 3 (.S3M) file parser for mod2rmf.
 *
 * Reference: "Scream Tracker 3.2x TECH.DOC" by PSI / Future Crew
 *
 ****************************************************************************/

#include "mod2rmf_s3m.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Little-endian readers
 * ----------------------------------------------------------------------- */

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* -----------------------------------------------------------------------
 * Signature detection
 * ----------------------------------------------------------------------- */

int mod2rmf_s3m_is_signature(const unsigned char *data, size_t size)
{
    if (!data || size < 0x30)
        return 0;
    /* "SCRM" at offset 0x2C */
    return (data[0x2C] == 'S' && data[0x2D] == 'C' &&
            data[0x2E] == 'R' && data[0x2F] == 'M');
}

/* -----------------------------------------------------------------------
 * Pattern unpacking
 *
 * S3M patterns are packed:
 *   [uint16_t packedSize]  (not counting this word)
 *   then packed row data for 64 rows.
 *
 * Each row:
 *   byte == 0  → end of row
 *   byte != 0  →
 *     channel = byte & 0x1F
 *     if (byte & 0x20): read note (1 byte) + instrument (1 byte)
 *     if (byte & 0x40): read volume (1 byte)
 *     if (byte & 0x80): read command (1 byte) + info (1 byte)
 * ----------------------------------------------------------------------- */

static int unpack_pattern(const unsigned char *data, size_t dataSize,
                          uint16_t channelCount, Mod2RmfS3mPattern *pat)
{
    uint16_t packedSize;
    const unsigned char *p;
    const unsigned char *end;
    uint16_t row;

    if (dataSize < 2)
        return 0;

    packedSize = read_le16(data);
    p = data + 2;
    end = data + 2 + packedSize;
    if ((size_t)(end - data) > dataSize)
        end = data + dataSize;

    pat->rows = 64;
    pat->channelCount = channelCount;
    pat->cells = (Mod2RmfS3mCell *)calloc((size_t)64 * channelCount,
                                           sizeof(Mod2RmfS3mCell));
    if (!pat->cells)
        return 0;

    /* Initialise all cells to "empty" */
    {
        uint32_t c;
        for (c = 0; c < (uint32_t)64 * channelCount; c++)
        {
            pat->cells[c].note = 255;   /* 255 = no note */
            pat->cells[c].volume = 255; /* 255 = no volume column */
        }
    }

    row = 0;
    while (p < end && row < 64)
    {
        uint8_t flag = *p++;
        if (flag == 0)
        {
            row++;
            continue;
        }

        {
            uint8_t ch = flag & 0x1Fu;
            Mod2RmfS3mCell *cell = NULL;

            if (ch < channelCount)
                cell = &pat->cells[(uint32_t)row * channelCount + ch];

            if (flag & 0x20u)
            {
                if (p + 2 > end) break;
                if (cell)
                {
                    cell->note = p[0];
                    cell->instrument = p[1];
                }
                p += 2;
            }
            if (flag & 0x40u)
            {
                if (p + 1 > end) break;
                if (cell)
                    cell->volume = p[0];
                p += 1;
            }
            if (flag & 0x80u)
            {
                if (p + 2 > end) break;
                if (cell)
                {
                    cell->command = p[0];
                    cell->info = p[1];
                }
                p += 2;
            }
        }
    }

    return 1;
}

/* -----------------------------------------------------------------------
 * Sample loading
 *
 * S3M instrument header (type 1 = PCM sample):
 *   offset 0x00: type (1 byte)
 *   offset 0x01: DOS filename (12 bytes)
 *   offset 0x0D: sample data pointer (3 bytes, parapointer hi:lo)
 *   offset 0x10: length (uint32_t)
 *   offset 0x14: loop start (uint32_t)
 *   offset 0x18: loop end (uint32_t)
 *   offset 0x1C: volume (1 byte)
 *   offset 0x1E: packing (1 byte, 0=unpacked)
 *   offset 0x1F: flags (1 byte: bit0=loop, bit2=16-bit, bit3=stereo)
 *   offset 0x20: C2Spd (uint32_t)
 *   offset 0x30: sample name (28 bytes)
 *   offset 0x4C: "SCRS" signature
 * ----------------------------------------------------------------------- */

static int load_sample(const unsigned char *fileData, size_t fileSize,
                       size_t instOffset, Mod2RmfS3mSample *smp)
{
    const unsigned char *ih;
    uint32_t dataPtr;
    uint32_t length;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint8_t flags;
    uint8_t packing;
    int is16bit;
    size_t dataOffset;
    size_t byteLen;
    uint32_t i;

    if (instOffset + 0x50 > fileSize)
        return 0;

    ih = fileData + instOffset;
    smp->type = ih[0];

    /* Copy name */
    memcpy(smp->name, ih + 0x30, 28);
    smp->name[28] = '\0';

    if (smp->type != 1)
    {
        /* Not a PCM sample (empty or Adlib) */
        smp->pcm8 = NULL;
        smp->length = 0;
        return 1; /* not an error, just skip */
    }

    /* Sample data parapointer: [0x0D]=hi byte, [0x0E..0x0F]=lo word (LE) */
    dataPtr = ((uint32_t)ih[0x0D] << 16) | (uint32_t)read_le16(ih + 0x0E);
    dataOffset = (size_t)dataPtr << 4; /* parapointer → byte offset */

    length    = read_le32(ih + 0x10);
    loopStart = read_le32(ih + 0x14);
    loopEnd   = read_le32(ih + 0x18);
    smp->volume = ih[0x1C];
    packing   = ih[0x1E];
    flags     = ih[0x1F];
    smp->c2spd = read_le32(ih + 0x20);
    smp->flags = flags;

    is16bit = (flags & 0x04) ? 1 : 0;
    smp->bits16 = (uint8_t)is16bit;

    /* We only support unpacked PCM */
    if (packing != 0)
    {
        smp->pcm8 = NULL;
        smp->length = 0;
        return 1;
    }

    /* Clamp length */
    if (length > 0x100000u)
        length = 0x100000u;

    byteLen = (size_t)length * (is16bit ? 2u : 1u);
    if (dataOffset + byteLen > fileSize)
    {
        /* Truncate to available data */
        if (dataOffset >= fileSize)
        {
            smp->pcm8 = NULL;
            smp->length = 0;
            return 1;
        }
        byteLen = fileSize - dataOffset;
        length = (uint32_t)(byteLen / (is16bit ? 2u : 1u));
    }

    if (length == 0)
    {
        smp->pcm8 = NULL;
        smp->length = 0;
        return 1;
    }

    smp->length = length;
    smp->loopStart = loopStart;
    smp->loopEnd = loopEnd;

    /* Clamp loop points */
    if (smp->loopEnd > length)
        smp->loopEnd = length;
    if (smp->loopStart >= smp->loopEnd)
    {
        smp->loopStart = 0;
        smp->loopEnd = 0;
    }
    if (!(flags & 0x01))
    {
        smp->loopStart = 0;
        smp->loopEnd = 0;
    }

    /* Convert to unsigned-biased 8-bit PCM (BAE engine convention).
     * S3M PCM is unsigned 8-bit or unsigned 16-bit. */
    smp->pcm8 = (int8_t *)malloc(length);
    if (!smp->pcm8)
        return 0;

    if (is16bit)
    {
        const unsigned char *src = fileData + dataOffset;
        for (i = 0; i < length; i++)
        {
            /* S3M 16-bit: unsigned, little-endian.
             * Take the high byte as unsigned-biased 8-bit. */
            uint16_t s = (uint16_t)src[i * 2] | ((uint16_t)src[i * 2 + 1] << 8);
            smp->pcm8[i] = (int8_t)(s >> 8);
        }
    }
    else
    {
        /* S3M 8-bit: unsigned. Already in the right encoding. */
        memcpy(smp->pcm8, fileData + dataOffset, length);
    }

    return 1;
}

/* -----------------------------------------------------------------------
 * Module parsing
 * ----------------------------------------------------------------------- */

int mod2rmf_s3m_parse_module(const unsigned char *data, size_t size,
                             Mod2RmfS3mModule *mod, char *errBuf,
                             size_t errBufSize)
{
    uint16_t orderCount;
    uint16_t instCount;
    uint16_t patCount;
    const unsigned char *instPtrs;
    const unsigned char *patPtrs;
    uint16_t channelCount;
    uint16_t i;

    if (!data || !mod)
    {
        if (errBuf && errBufSize > 0)
            snprintf(errBuf, errBufSize, "null input");
        return 0;
    }

    memset(mod, 0, sizeof(*mod));

    {
        uint32_t pi;
        mod->hasDefaultPanning = 0;
        for (pi = 0; pi < 32u; ++pi)
            mod->defaultPanning[pi] = 255u;
    }

    if (!mod2rmf_s3m_is_signature(data, size))
    {
        if (errBuf && errBufSize > 0)
            snprintf(errBuf, errBufSize, "not an S3M file");
        return 0;
    }

    if (size < 0x60)
    {
        if (errBuf && errBufSize > 0)
            snprintf(errBuf, errBufSize, "truncated S3M header");
        return 0;
    }

    /* Header fields */
    memcpy(mod->name, data, 28);
    mod->name[28] = '\0';

    orderCount = read_le16(data + 0x20);
    instCount  = read_le16(data + 0x22);
    patCount   = read_le16(data + 0x24);
    mod->flags = read_le16(data + 0x26);
    mod->trackerVersion    = read_le16(data + 0x28);
    mod->fileFormatVersion = read_le16(data + 0x2A);
    mod->globalVolume  = data[0x30];
    mod->initialSpeed  = data[0x31];
    mod->initialTempo  = data[0x32];
    mod->masterVolume  = data[0x33];

    if (orderCount > MOD2RMF_S3M_MAX_ORDERS)
        orderCount = MOD2RMF_S3M_MAX_ORDERS;

    mod->orderCount      = orderCount;
    mod->instrumentCount = instCount;
    mod->patternCount    = patCount;

    /* Channel settings at offset 0x40..0x5F */
    memcpy(mod->channelSettings, data + 0x40, 32);

    /* Determine actual channel count from channel settings.
     * Channels with setting >= 0x80 are disabled. */
    channelCount = 0;
    for (i = 0; i < 32; i++)
    {
        if (mod->channelSettings[i] < 0x80)
        {
            if (i + 1 > channelCount)
                channelCount = i + 1;
        }
    }
    if (channelCount == 0)
        channelCount = 1;
    mod->channelCount = channelCount;

    /* Orders at offset 0x60 */
    {
        size_t ordersOff = 0x60;
        if (ordersOff + orderCount > size)
        {
            if (errBuf && errBufSize > 0)
                snprintf(errBuf, errBufSize, "truncated orders");
            return 0;
        }
        memcpy(mod->orders, data + ordersOff, orderCount);
    }

    /* Instrument and pattern parapointers follow orders */
    {
        size_t ptrsOff = 0x60 + orderCount;
        size_t instPtrsSize = (size_t)instCount * 2;
        size_t patPtrsSize  = (size_t)patCount * 2;

        if (ptrsOff + instPtrsSize + patPtrsSize > size)
        {
            if (errBuf && errBufSize > 0)
                snprintf(errBuf, errBufSize, "truncated pointer table");
            return 0;
        }

        instPtrs = data + ptrsOff;
        patPtrs  = data + ptrsOff + instPtrsSize;

        /* Optional default panning table (32 bytes) follows parapointers
         * when header byte 0x35 is 0xFC. Each entry uses bit5 as "set"
         * and low nibble 0..15 as pan from left to right. */
        if (data[0x35] == 0xFC)
        {
            size_t panOff = ptrsOff + instPtrsSize + patPtrsSize;
            if (panOff + 32u <= size)
            {
                for (i = 0; i < 32u; ++i)
                {
                    uint8_t pv;
                    pv = data[panOff + i];
                    if (pv & 0x20u)
                    {
                        mod->defaultPanning[i] = (uint8_t)((pv & 0x0Fu) * 17u);
                    }
                    else
                    {
                        mod->defaultPanning[i] = 255u;
                    }
                }
                mod->hasDefaultPanning = 1;
            }
        }
    }

    /* --- Load samples --- */
    if (instCount > 0)
    {
        mod->samples = (Mod2RmfS3mSample *)calloc(instCount,
                                                    sizeof(Mod2RmfS3mSample));
        if (!mod->samples)
        {
            if (errBuf && errBufSize > 0)
                snprintf(errBuf, errBufSize, "out of memory (samples)");
            return 0;
        }

        for (i = 0; i < instCount; i++)
        {
            uint16_t paraPtr = read_le16(instPtrs + (size_t)i * 2);
            size_t instOff = (size_t)paraPtr << 4;

            if (instOff == 0 || instOff + 0x50 > size)
            {
                mod->samples[i].type = 0;
                continue;
            }

            if (!load_sample(data, size, instOff, &mod->samples[i]))
            {
                if (errBuf && errBufSize > 0)
                    snprintf(errBuf, errBufSize, "sample %u load failed", (unsigned)i);
                mod2rmf_s3m_free_module(mod);
                return 0;
            }
        }
    }

    /* --- Load patterns --- */
    if (patCount > 0)
    {
        mod->patterns = (Mod2RmfS3mPattern *)calloc(patCount,
                                                      sizeof(Mod2RmfS3mPattern));
        if (!mod->patterns)
        {
            if (errBuf && errBufSize > 0)
                snprintf(errBuf, errBufSize, "out of memory (patterns)");
            mod2rmf_s3m_free_module(mod);
            return 0;
        }

        for (i = 0; i < patCount; i++)
        {
            uint16_t paraPtr = read_le16(patPtrs + (size_t)i * 2);
            size_t patOff = (size_t)paraPtr << 4;

            if (paraPtr == 0 || patOff >= size)
            {
                /* Empty pattern — allocate 64 empty rows */
                mod->patterns[i].rows = 64;
                mod->patterns[i].channelCount = channelCount;
                mod->patterns[i].cells = (Mod2RmfS3mCell *)calloc(
                    (size_t)64 * channelCount, sizeof(Mod2RmfS3mCell));
                if (mod->patterns[i].cells)
                {
                    uint32_t c;
                    for (c = 0; c < (uint32_t)64 * channelCount; c++)
                        mod->patterns[i].cells[c].volume = 255;
                }
                continue;
            }

            if (!unpack_pattern(data + patOff, size - patOff,
                                channelCount, &mod->patterns[i]))
            {
                if (errBuf && errBufSize > 0)
                    snprintf(errBuf, errBufSize, "pattern %u unpack failed", (unsigned)i);
                mod2rmf_s3m_free_module(mod);
                return 0;
            }
        }
    }

    return 1;
}

/* -----------------------------------------------------------------------
 * Cleanup
 * ----------------------------------------------------------------------- */

void mod2rmf_s3m_free_module(Mod2RmfS3mModule *mod)
{
    uint16_t i;
    if (!mod) return;

    if (mod->samples)
    {
        for (i = 0; i < mod->instrumentCount; i++)
            free(mod->samples[i].pcm8);
        free(mod->samples);
        mod->samples = NULL;
    }

    if (mod->patterns)
    {
        for (i = 0; i < mod->patternCount; i++)
            free(mod->patterns[i].cells);
        free(mod->patterns);
        mod->patterns = NULL;
    }
}
