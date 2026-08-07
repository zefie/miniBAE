/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/* mod2rmf_encoder.h -- Codec/compression helpers for mod2rmf. */
#ifndef MOD2RMF_ENCODER_H
#define MOD2RMF_ENCODER_H

#include <NeoBAE.h>

/* ------------------------------------------------------------------ */
/*  Codec identifiers (used on the command line via --codec)           */
/* ------------------------------------------------------------------ */
typedef enum Mod2RmfCodec
{
    MOD2RMF_CODEC_PCM       = 0,
    MOD2RMF_CODEC_ADPCM     = 1,
    MOD2RMF_CODEC_ALAW      = 2,
    MOD2RMF_CODEC_ULAW      = 3,
    MOD2RMF_CODEC_MP3       = 4,
    MOD2RMF_CODEC_VORBIS    = 5,
    MOD2RMF_CODEC_FLAC      = 6,
    MOD2RMF_CODEC_OPUS      = 7,
#if USE_QOA_SUPPORT == TRUE
    MOD2RMF_CODEC_QOA       = 8,
#if USE_ZMF_SUPPORT == TRUE
    MOD2RMF_CODEC_ADPCM_2BIT = 9, /* headerless 2-bit IMA (ZMF v6) */
    MOD2RMF_CODEC_COUNT     = 10
#else
    MOD2RMF_CODEC_COUNT     = 9
#endif
#else
#if USE_ZMF_SUPPORT == TRUE
    MOD2RMF_CODEC_ADPCM_2BIT = 8, /* headerless 2-bit IMA (ZMF v6) */
    MOD2RMF_CODEC_COUNT     = 9
#else
    MOD2RMF_CODEC_COUNT     = 8
#endif
#endif
} Mod2RmfCodec;

/* Parsed compression settings. */
typedef struct Mod2RmfEncoderSettings
{
    Mod2RmfCodec codec;
    uint32_t     bitrateKbps;   /* 0 = use codec default */
} Mod2RmfEncoderSettings;

/* Fill *settings with default values (PCM, no bitrate override). */
void mod2rmf_encoder_defaults(Mod2RmfEncoderSettings *settings);

/* Parse a --codec argument string (e.g. "pcm", "adpcm", "adpcm2", "alaw",
 * "ulaw", "mp3", "vorbis", "flac", "opus", "qoa"). Returns 0 on success,
 * -1 on bad name. */
int mod2rmf_encoder_parse_codec(const char *name, Mod2RmfCodec *outCodec);

/* Parse a --bitrate argument string.  Accepts plain kbps ("128") or bps
 * ("128000") and normalises to kbps.  Returns 0 on success, -1 on error. */
int mod2rmf_encoder_parse_bitrate(const char *str, uint32_t *outKbps);

/* Resolve the parsed settings to a BAERmfEditorCompressionType that can be
 * passed to BAERmfEditorDocument_SetSampleAssetCompression().
 * Returns BAE_EDITOR_COMPRESSION_PCM when codec is PCM or on error. */
BAERmfEditorCompressionType mod2rmf_encoder_resolve(const Mod2RmfEncoderSettings *settings);

/* Return a short human-readable label for the resolved compression type
 * (e.g. "MP3 128 kbps", "Opus 48 kbps", "ADPCM").  The pointer is to
 * static storage and must not be freed. */
const char *mod2rmf_encoder_label(BAERmfEditorCompressionType ct);

/* Apply the chosen compression to every sample in the document.
 * Logs a summary line to stderr.  Returns 1 on success, 0 on failure. */
int mod2rmf_encoder_apply(BAERmfEditorDocument *document,
                          const Mod2RmfEncoderSettings *settings,
                          BAERmfEditorCompressionType ct);

/* Returns non-zero if the chosen codec requires ZMF container
 * (Vorbis, FLAC, Opus, QOA, 2-bit ADPCM). ZMF map version is stamped
 * v5 unless a v6 feature is present (oscillator / 2-bit ADPCM). */
int mod2rmf_encoder_requires_zmf(Mod2RmfCodec codec);

/* Print the list of available codecs and bitrates to stderr. */
void mod2rmf_encoder_print_codecs(void);

#endif /* MOD2RMF_ENCODER_H */
