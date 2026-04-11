/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/* mod2rmf_encoder.c -- Codec/compression helpers for mod2rmf.
 *
 * This keeps all codec-selection and bitrate-resolution logic in one place
 * so that mod2rmf.c only needs to call mod2rmf_encoder_resolve() and
 * mod2rmf_encoder_apply().
 */
#include "mod2rmf_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Bitrate table entry                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t                    kbps;
    BAERmfEditorCompressionType ct;
} BitrateEntry;

/* ------------------------------------------------------------------ */
/*  Per-codec bitrate tables (sorted ascending by kbps)                */
/* ------------------------------------------------------------------ */
static const BitrateEntry mp3Bitrates[] = {
    {  32, BAE_EDITOR_COMPRESSION_MP3_32K  },
    {  48, BAE_EDITOR_COMPRESSION_MP3_48K  },
    {  64, BAE_EDITOR_COMPRESSION_MP3_64K  },
    {  96, BAE_EDITOR_COMPRESSION_MP3_96K  },
    { 128, BAE_EDITOR_COMPRESSION_MP3_128K },
    { 192, BAE_EDITOR_COMPRESSION_MP3_192K },
    { 256, BAE_EDITOR_COMPRESSION_MP3_256K },
    { 320, BAE_EDITOR_COMPRESSION_MP3_320K },
};

static const BitrateEntry vorbisBitrates[] = {
    {  32, BAE_EDITOR_COMPRESSION_VORBIS_32K  },
    {  48, BAE_EDITOR_COMPRESSION_VORBIS_48K  },
    {  64, BAE_EDITOR_COMPRESSION_VORBIS_64K  },
    {  80, BAE_EDITOR_COMPRESSION_VORBIS_80K  },
    {  96, BAE_EDITOR_COMPRESSION_VORBIS_96K  },
    { 128, BAE_EDITOR_COMPRESSION_VORBIS_128K },
    { 160, BAE_EDITOR_COMPRESSION_VORBIS_160K },
    { 192, BAE_EDITOR_COMPRESSION_VORBIS_192K },
    { 256, BAE_EDITOR_COMPRESSION_VORBIS_256K },
};

static const BitrateEntry opusBitrates[] = {
    {  12, BAE_EDITOR_COMPRESSION_OPUS_12K  },
    {  16, BAE_EDITOR_COMPRESSION_OPUS_16K  },
    {  24, BAE_EDITOR_COMPRESSION_OPUS_24K  },
    {  32, BAE_EDITOR_COMPRESSION_OPUS_32K  },
    {  48, BAE_EDITOR_COMPRESSION_OPUS_48K  },
    {  64, BAE_EDITOR_COMPRESSION_OPUS_64K  },
    {  80, BAE_EDITOR_COMPRESSION_OPUS_80K  },
    {  96, BAE_EDITOR_COMPRESSION_OPUS_96K  },
    { 128, BAE_EDITOR_COMPRESSION_OPUS_128K },
    { 160, BAE_EDITOR_COMPRESSION_OPUS_160K },
    { 192, BAE_EDITOR_COMPRESSION_OPUS_192K },
    { 256, BAE_EDITOR_COMPRESSION_OPUS_256K },
};

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Find the entry whose kbps is closest to the requested value. */
static BAERmfEditorCompressionType find_closest(const BitrateEntry *table,
                                                 size_t count,
                                                 uint32_t kbps)
{
    size_t i;
    size_t best;
    uint32_t bestDist;

    best = 0;
    bestDist = (kbps > table[0].kbps) ? (kbps - table[0].kbps)
                                       : (table[0].kbps - kbps);
    for (i = 1; i < count; ++i)
    {
        uint32_t d;
        d = (kbps > table[i].kbps) ? (kbps - table[i].kbps)
                                    : (table[i].kbps - kbps);
        if (d < bestDist)
        {
            bestDist = d;
            best = i;
        }
    }
    return table[best].ct;
}

/* ------------------------------------------------------------------ */
/*  Codec name table                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    const char  *name;
    Mod2RmfCodec codec;
} CodecName;

static const CodecName codecNames[] = {
    { "pcm",     MOD2RMF_CODEC_PCM     },
    { "adpcm",   MOD2RMF_CODEC_ADPCM   },
    { "alaw",    MOD2RMF_CODEC_ALAW    },
    { "ulaw",    MOD2RMF_CODEC_ULAW    },
    { "mp3",     MOD2RMF_CODEC_MP3     },
    { "vorbis",  MOD2RMF_CODEC_VORBIS  },
    { "flac",    MOD2RMF_CODEC_FLAC    },
    { "opus",    MOD2RMF_CODEC_OPUS    },
#if USE_QOA_SUPPORT == TRUE
    { "qoa",     MOD2RMF_CODEC_QOA     },
#endif
};

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void mod2rmf_encoder_defaults(Mod2RmfEncoderSettings *settings)
{
    if (!settings) return;
    settings->codec = MOD2RMF_CODEC_PCM;
    settings->bitrateKbps = 0;
}

int mod2rmf_encoder_parse_codec(const char *name, Mod2RmfCodec *outCodec)
{
    size_t i;
    char *end;
    unsigned long num;

    if (!name || !outCodec) return -1;

    /* Try numeric first (matches bankrecomp convention). */
    num = strtoul(name, &end, 10);
    if (end != name && *end == '\0' && num < MOD2RMF_CODEC_COUNT)
    {
        *outCodec = (Mod2RmfCodec)num;
        return 0;
    }

    /* Fall back to name lookup. */
    for (i = 0; i < ARRAY_COUNT(codecNames); ++i)
    {
        if (strcmp(name, codecNames[i].name) == 0)
        {
            *outCodec = codecNames[i].codec;
            return 0;
        }
    }
    return -1;
}

int mod2rmf_encoder_parse_bitrate(const char *str, uint32_t *outKbps)
{
    unsigned long val;
    char *end;

    if (!str || !outKbps) return -1;
    val = strtoul(str, &end, 10);
    if (end == str || val == 0) return -1;
    /* If the value looks like bps (>= 1000), convert to kbps. */
    if (val >= 1000)
    {
        val = (val + 500) / 1000;
    }
    *outKbps = (uint32_t)val;
    return 0;
}

BAERmfEditorCompressionType mod2rmf_encoder_resolve(const Mod2RmfEncoderSettings *settings)
{
    uint32_t br;

    if (!settings) return BAE_EDITOR_COMPRESSION_PCM;

    switch (settings->codec)
    {
        case MOD2RMF_CODEC_PCM:
            return BAE_EDITOR_COMPRESSION_PCM;

        case MOD2RMF_CODEC_ADPCM:
            return BAE_EDITOR_COMPRESSION_ADPCM;

        case MOD2RMF_CODEC_ALAW:
            return BAE_EDITOR_COMPRESSION_ALAW;

        case MOD2RMF_CODEC_ULAW:
            return BAE_EDITOR_COMPRESSION_ULAW;

        case MOD2RMF_CODEC_FLAC:
            return BAE_EDITOR_COMPRESSION_FLAC;

        case MOD2RMF_CODEC_MP3:
            br = settings->bitrateKbps ? settings->bitrateKbps : 128;
            return find_closest(mp3Bitrates, ARRAY_COUNT(mp3Bitrates), br);

        case MOD2RMF_CODEC_VORBIS:
            br = settings->bitrateKbps ? settings->bitrateKbps : 128;
            return find_closest(vorbisBitrates, ARRAY_COUNT(vorbisBitrates), br);

        case MOD2RMF_CODEC_OPUS:
            br = settings->bitrateKbps ? settings->bitrateKbps : 48;
            return find_closest(opusBitrates, ARRAY_COUNT(opusBitrates), br);

#if USE_QOA_SUPPORT == TRUE
        case MOD2RMF_CODEC_QOA:
            return BAE_EDITOR_COMPRESSION_QOA;
#endif

        default:
            break;
    }
    return BAE_EDITOR_COMPRESSION_PCM;
}

const char *mod2rmf_encoder_label(BAERmfEditorCompressionType ct)
{
    switch (ct)
    {
        case BAE_EDITOR_COMPRESSION_PCM:         return "PCM (uncompressed)";
        case BAE_EDITOR_COMPRESSION_ADPCM:       return "ADPCM";
        case BAE_EDITOR_COMPRESSION_ALAW:        return "A-law";
        case BAE_EDITOR_COMPRESSION_ULAW:        return "u-law";
        case BAE_EDITOR_COMPRESSION_FLAC:        return "FLAC (lossless)";

        case BAE_EDITOR_COMPRESSION_MP3_32K:     return "MP3 32 kbps";
        case BAE_EDITOR_COMPRESSION_MP3_48K:     return "MP3 48 kbps";
        case BAE_EDITOR_COMPRESSION_MP3_64K:     return "MP3 64 kbps";
        case BAE_EDITOR_COMPRESSION_MP3_96K:     return "MP3 96 kbps";
        case BAE_EDITOR_COMPRESSION_MP3_128K:    return "MP3 128 kbps";
        case BAE_EDITOR_COMPRESSION_MP3_192K:    return "MP3 192 kbps";
        case BAE_EDITOR_COMPRESSION_MP3_256K:    return "MP3 256 kbps";
        case BAE_EDITOR_COMPRESSION_MP3_320K:    return "MP3 320 kbps";

        case BAE_EDITOR_COMPRESSION_VORBIS_32K:  return "Vorbis 32 kbps";
        case BAE_EDITOR_COMPRESSION_VORBIS_48K:  return "Vorbis 48 kbps";
        case BAE_EDITOR_COMPRESSION_VORBIS_64K:  return "Vorbis 64 kbps";
        case BAE_EDITOR_COMPRESSION_VORBIS_80K:  return "Vorbis 80 kbps";
        case BAE_EDITOR_COMPRESSION_VORBIS_96K:  return "Vorbis 96 kbps";
        case BAE_EDITOR_COMPRESSION_VORBIS_128K: return "Vorbis 128 kbps";
        case BAE_EDITOR_COMPRESSION_VORBIS_160K: return "Vorbis 160 kbps";
        case BAE_EDITOR_COMPRESSION_VORBIS_192K: return "Vorbis 192 kbps";
        case BAE_EDITOR_COMPRESSION_VORBIS_256K: return "Vorbis 256 kbps";

        case BAE_EDITOR_COMPRESSION_OPUS_12K:    return "Opus 12 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_16K:    return "Opus 16 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_24K:    return "Opus 24 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_32K:    return "Opus 32 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_48K:    return "Opus 48 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_64K:    return "Opus 64 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_80K:    return "Opus 80 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_96K:    return "Opus 96 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_128K:   return "Opus 128 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_160K:   return "Opus 160 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_192K:   return "Opus 192 kbps";
        case BAE_EDITOR_COMPRESSION_OPUS_256K:   return "Opus 256 kbps";

    #if USE_QOA_SUPPORT == TRUE
        case BAE_EDITOR_COMPRESSION_QOA:         return "QOA";
    #endif

        default: break;
    }
    return "Unknown";
}

int mod2rmf_encoder_apply(BAERmfEditorDocument *document,
                          const Mod2RmfEncoderSettings *settings,
                          BAERmfEditorCompressionType ct)
{
    uint32_t sampleCount;
    uint32_t i;
    uint32_t applied;
    bool usingOpusRoundTrip;

    if (!document || !settings) return 0;
    if (ct == BAE_EDITOR_COMPRESSION_PCM) return 1;

    usingOpusRoundTrip = (settings->codec == MOD2RMF_CODEC_OPUS) ? TRUE : FALSE;
    sampleCount = 0;
    BAERmfEditorDocument_GetSampleCount(document, &sampleCount);

    applied = 0;
    for (i = 0; i < sampleCount; ++i)
    {
        uint32_t assetID;
        BAEResult result;

        result = BAERmfEditorDocument_GetSampleAssetIDForSample(document, i, &assetID);
        if (result != BAE_NO_ERROR) continue;

        result = BAERmfEditorDocument_SetSampleAssetCompression(document, assetID, ct);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "Warning: failed to set compression for sample %u (asset %u): %d\n",
                    (unsigned)i, (unsigned)assetID, (int)result);
            continue;
        }

        /* For Opus round-trip mode, set the flag on the sample info so
         * the save path encodes at 48 kHz and preserves the original rate. */
        if (usingOpusRoundTrip)
        {
            BAERmfEditorSampleInfo sInfo;
            result = BAERmfEditorDocument_GetSampleInfo(document, i, &sInfo);
            if (result == BAE_NO_ERROR)
            {
                sInfo.opusMode = BAE_EDITOR_OPUS_MODE_AUDIO;
                sInfo.opusRoundTripResample = TRUE;
                result = BAERmfEditorDocument_SetSampleInfo(document, i, &sInfo);
                if (result != BAE_NO_ERROR)
                {
                    fprintf(stderr,
                            "Warning: failed to apply Opus RT sample settings for sample %u: %d\n",
                            (unsigned)i,
                            (int)result);
                    continue;
                }
            }
            else
            {
                fprintf(stderr,
                        "Warning: failed to read sample info for Opus RT sample %u: %d\n",
                        (unsigned)i,
                        (int)result);
            }
        }

        ++applied;
    }

        fprintf(stderr, "Compression: %s%s applied to %u/%u samples\n",
            mod2rmf_encoder_label(ct),
            usingOpusRoundTrip ? " (round-trip)" : "",
            (unsigned)applied, (unsigned)sampleCount);
    return 1;
}

int mod2rmf_encoder_requires_zmf(Mod2RmfCodec codec)
{
    return (codec == MOD2RMF_CODEC_VORBIS ||
            codec == MOD2RMF_CODEC_FLAC   ||
            codec == MOD2RMF_CODEC_OPUS
#if USE_QOA_SUPPORT == TRUE
            || codec == MOD2RMF_CODEC_QOA
#endif
            );
}

void mod2rmf_encoder_print_codecs(void)
{
    fprintf(stderr, "Available codecs (use number or name with --codec):\n");
    fprintf(stderr, "  0  pcm        Uncompressed PCM (default)\n");
    fprintf(stderr, "  1  adpcm      IMA ADPCM 4:1\n");
    fprintf(stderr, "  2  alaw       G.711 A-law\n");
    fprintf(stderr, "  3  ulaw       G.711 u-law\n");
    fprintf(stderr, "  4  mp3        MP3          --bitrate: 32 48 64 96 128* 192 256 320\n");
    fprintf(stderr, "  5  vorbis     Ogg Vorbis   --bitrate: 32 48 64 80 96 128* 160 192 256\n");
    fprintf(stderr, "  6  flac       FLAC lossless\n");
    fprintf(stderr, "  7  opus       Ogg Opus (round-trip) --bitrate: 12 16 24 32 48* 64 80 96 128 160 192 256\n");
#if USE_QOA_SUPPORT == TRUE
    fprintf(stderr, "  8  qoa        Quite OK Audio (sample codec, no bitrate option)\n");
#endif
    fprintf(stderr, "                * = default bitrate when --bitrate is omitted\n");
}
