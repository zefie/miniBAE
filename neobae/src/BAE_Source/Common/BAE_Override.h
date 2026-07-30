#ifndef MINIBAE_BAE_OVERRIDE_H
#define MINIBAE_BAE_OVERRIDE_H

#include <stdint.h>

#include "X_Formats.h"

struct GM_Song;

typedef struct BAEOverrideEntry
{
    const char *sha1;
    uint32_t flags;
} BAEOverrideEntry;

#if BAE_CLASSIC_CHORUS
#define BAE_OVERRIDE_FLAG_CLASSIC_CHORUS_ON \
    (SONG_CONFIG_HAS_CLASSIC_CHORUS | SONG_CONFIG_CLASSIC_CHORUS_ON)
#define BAE_OVERRIDE_FLAG_CLASSIC_CHORUS_OFF \
    (SONG_CONFIG_HAS_CLASSIC_CHORUS)
#endif

#define BAE_OVERRIDE_FLAG_FORCE_ZMF \
    (SONG_CONFIG_CONTAINER_IS_ZMF)

#if BAE_FIX_SPAN_DC
#define BAE_OVERRIDE_FLAG_PANFIX_ON \
    (SONG_CONFIG_HAS_PANFIX | SONG_CONFIG_PANFIX_ON)
#define BAE_OVERRIDE_FLAG_PANFIX_OFF \
    (SONG_CONFIG_HAS_PANFIX)
#endif

#define BAE_OVERRIDE_FLAG_EXTENDED_PITCH_RANGE_ON \
    (SONG_CONFIG_HAS_EXTENDED_PITCH_RANGE | SONG_CONFIG_EXTENDED_PITCH_RANGE_ON)
#define BAE_OVERRIDE_FLAG_EXTENDED_PITCH_RANGE_OFF \
    (SONG_CONFIG_HAS_EXTENDED_PITCH_RANGE)

#define BAE_OVERRIDE_VOLUME_CURVE(curveType) \
    ((uint32_t)(SONG_CONFIG_OVERRIDE_VOLUME_CURVE | (((curveType) << SONG_CONFIG_VOLUME_CURVE_TYPE_SHIFT) & SONG_CONFIG_VOLUME_CURVE_TYPE_MASK)))

/*
 * Hash-based runtime overrides.
 *
 * Add new entries here using lowercase SHA1 hex for the full file contents.
 * Flags are merged into the song's runtime engine config when the file hash matches.
 */
static const BAEOverrideEntry kBAEOverrides[] = {
#if BAE_CLASSIC_CHORUS
    { "d48b578b13759fecceb6a53d038704484f8fc993", BAE_OVERRIDE_FLAG_CLASSIC_CHORUS_ON | BAE_OVERRIDE_FLAG_FORCE_ZMF },
#endif
};

static const uint32_t kBAEOverrideCount = (uint32_t)(sizeof(kBAEOverrides) / sizeof(kBAEOverrides[0]));

uint32_t BAE_Override(uint32_t currentFlags, const char *sha1Hex);
uint32_t BAE_OverrideFromFile(const void *filePath, uint32_t currentFlags);
uint32_t BAE_OverrideFromData(const void *data, uint32_t dataSize, uint32_t currentFlags);
void BAE_OverrideSongFromFile(struct GM_Song *song, const void *filePath);
void BAE_OverrideSongFromData(struct GM_Song *song, const void *data, uint32_t dataSize);
void BAE_OverrideBAESongFromFile(void *songObject, const void *filePath);

#endif