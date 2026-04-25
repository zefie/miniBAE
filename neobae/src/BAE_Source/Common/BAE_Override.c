#include "BAE_Override.h"

#include "GenSnd.h"
#include "X_Formats.h"
#include "X_Assert.h"
#include "sha1mini.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define BAE_OVERRIDE_SHA1_HEX_LEN 40

static void PV_DigestToHex(const unsigned char digest[20], char outHex[BAE_OVERRIDE_SHA1_HEX_LEN + 1])
{
    static const char hexDigits[] = "0123456789abcdef";
    uint32_t index;

    for (index = 0; index < 20; ++index)
    {
        outHex[index * 2] = hexDigits[(digest[index] >> 4) & 0x0F];
        outHex[index * 2 + 1] = hexDigits[digest[index] & 0x0F];
    }
    outHex[BAE_OVERRIDE_SHA1_HEX_LEN] = '\0';
}

static uint32_t PV_FilterOverrideFlags(uint32_t flags)
{
    uint32_t filtered = 0;

    filtered |= flags & SONG_CONFIG_CONTAINER_IS_ZMF;

#if BAE_CLASSIC_CHORUS
    filtered |= flags & (SONG_CONFIG_HAS_CLASSIC_CHORUS | SONG_CONFIG_CLASSIC_CHORUS_ON);
#endif
#if BAE_FIX_SPAN_DC
    filtered |= flags & (SONG_CONFIG_HAS_PANFIX | SONG_CONFIG_PANFIX_ON);
#endif
    filtered |= flags & (SONG_CONFIG_HAS_EXTENDED_PITCH_RANGE | SONG_CONFIG_EXTENDED_PITCH_RANGE_ON);

    return filtered;
}

static uint32_t PV_MergeOverridePair(uint32_t currentFlags, uint32_t overrideFlags, uint32_t hasBit, uint32_t onBit)
{
    if (overrideFlags & hasBit)
    {
        currentFlags &= ~(hasBit | onBit);
        currentFlags |= (overrideFlags & (hasBit | onBit));
    }
    return currentFlags;
}

static uint32_t PV_MergeOverrideFlags(uint32_t currentFlags, uint32_t overrideFlags)
{
    overrideFlags = PV_FilterOverrideFlags(overrideFlags);
    if (overrideFlags == 0)
    {
        return currentFlags;
    }

    currentFlags = PV_MergeOverridePair(currentFlags,
                                        overrideFlags,
                                        SONG_CONFIG_HAS_CLASSIC_CHORUS,
                                        SONG_CONFIG_CLASSIC_CHORUS_ON);
    currentFlags = PV_MergeOverridePair(currentFlags,
                                        overrideFlags,
                                        SONG_CONFIG_HAS_PANFIX,
                                        SONG_CONFIG_PANFIX_ON);
    currentFlags = PV_MergeOverridePair(currentFlags,
                                        overrideFlags,
                                        SONG_CONFIG_HAS_EXTENDED_PITCH_RANGE,
                                        SONG_CONFIG_EXTENDED_PITCH_RANGE_ON);
    if (overrideFlags & SONG_CONFIG_CONTAINER_IS_ZMF)
    {
        currentFlags |= SONG_CONFIG_CONTAINER_IS_ZMF;
    }
    currentFlags &= SONG_CONFIG_VALID_BITS_MASK;
    debug_message("[BAE Override] Merged override flags: 0x%08X\n", (unsigned)currentFlags);
    return currentFlags;
}

static uint32_t PV_FindOverrideFlags(const char *sha1Hex)
{
    uint32_t index;

    if (!sha1Hex || !sha1Hex[0])
    {
        return 0;
    }
    for (index = 0; index < kBAEOverrideCount; ++index)
    {
        if (strcmp(sha1Hex, kBAEOverrides[index].sha1) == 0)
        {
            return kBAEOverrides[index].flags;
        }
    }
    return 0;
}

uint32_t BAE_Override(uint32_t currentFlags, const char *sha1Hex)
{
    return PV_MergeOverrideFlags(currentFlags, PV_FindOverrideFlags(sha1Hex));
}

uint32_t BAE_OverrideFromFile(const void *filePath, uint32_t currentFlags)
{
    char sha1Hex[BAE_OVERRIDE_SHA1_HEX_LEN + 1];

    if (!filePath)
    {
        return currentFlags;
    }
    if (sha1mini_file_hex((const char *)filePath, sha1Hex) == 0)
    {
        debug_message("[BAE Override] SHA1 failed for %s\n", (const char *)filePath);
        return currentFlags;
    }
    debug_message("[BAE Override] SHA1 %s  %s\n", sha1Hex, (const char *)filePath);
    return BAE_Override(currentFlags, sha1Hex);
}

uint32_t BAE_OverrideFromData(const void *data, uint32_t dataSize, uint32_t currentFlags)
{
    unsigned char digest[20];
    char sha1Hex[BAE_OVERRIDE_SHA1_HEX_LEN + 1];

    if (!data || dataSize == 0)
    {
        return currentFlags;
    }
    sha1mini((const unsigned char *)data, (size_t)dataSize, digest);
    PV_DigestToHex(digest, sha1Hex);
    return BAE_Override(currentFlags, sha1Hex);
}

void BAE_OverrideSongFromFile(struct GM_Song *song, const void *filePath)
{
    uint32_t mergedFlags;

    if (!song || !filePath)
    {
        return;
    }
    mergedFlags = BAE_OverrideFromFile(filePath, (uint32_t)song->engineConfigFlags);
    if (mergedFlags != (uint32_t)song->engineConfigFlags)
    {
        debug_message("[Override] Applied runtime flags 0x%08X -> 0x%08X for %s\n",
                   (unsigned)song->engineConfigFlags,
                   (unsigned)mergedFlags,
                   (const char *)filePath);
        song->engineConfigFlags = (int32_t)mergedFlags;
    }
}

void BAE_OverrideSongFromData(struct GM_Song *song, const void *data, uint32_t dataSize)
{
    uint32_t mergedFlags;

    if (!song || !data || dataSize == 0)
    {
        return;
    }
    mergedFlags = BAE_OverrideFromData(data, dataSize, (uint32_t)song->engineConfigFlags);
    if (mergedFlags != (uint32_t)song->engineConfigFlags)
    {
        debug_message("[Override] Applied runtime flags 0x%08X -> 0x%08X from memory blob\n",
                   (unsigned)song->engineConfigFlags,
                   (unsigned)mergedFlags);
        song->engineConfigFlags = (int32_t)mergedFlags;
    }
}
