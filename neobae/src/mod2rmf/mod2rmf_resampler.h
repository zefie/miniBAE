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

/* mod2rmf_resampler.h
 *
 * PCM pre-processing for mod2rmf:
 *  - Amiga A500 / A1200 hardware low-pass filter simulation
 *  - High-quality resampling (nearest, linear, cubic Hermite, 8-tap sinc)
 *
 * These mirror the "Resampling" panel found in tracker players such as
 * OpenMPT:
 *   Filter:  Sinc + Low-Pass (8 taps)   -> MOD2RMF_RESAMPLE_SINC_8TAP
 *   Amiga:   A500 Filter                -> MOD2RMF_AMIGA_FILTER_A500
 *            A1200 Filter               -> MOD2RMF_AMIGA_FILTER_A1200
 */
#ifndef MOD2RMF_RESAMPLER_H
#define MOD2RMF_RESAMPLER_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Filter enumerations
 * ----------------------------------------------------------------------- */

/* Amiga hardware output low-pass filter simulation.
 * Applied to PCM data at the source sample rate before any resampling. */
typedef enum Mod2RmfAmigaFilter
{
    MOD2RMF_AMIGA_FILTER_NONE  = 0,  /* No hardware filter (default)        */
    MOD2RMF_AMIGA_FILTER_A500,       /* Amiga 500 passive RC LPF ~3275 Hz   */
    MOD2RMF_AMIGA_FILTER_A1200       /* Amiga 1200 passive RC LPF ~28867 Hz */
} Mod2RmfAmigaFilter;

/* Resampling interpolation method used when converting to a target rate.
 * Has no effect when targetRate == 0 or targetRate == srcRate. */
typedef enum Mod2RmfResampleFilter
{
    MOD2RMF_RESAMPLE_NEAREST  = 0,   /* Nearest-neighbor (fastest)          */
    MOD2RMF_RESAMPLE_LINEAR,         /* Linear interpolation                */
    MOD2RMF_RESAMPLE_CUBIC,          /* Cubic Hermite (Catmull-Rom)         */
    MOD2RMF_RESAMPLE_SINC_8TAP       /* 8-tap Lanczos-4 sinc + anti-alias   */
} Mod2RmfResampleFilter;

/* -----------------------------------------------------------------------
 * Settings bundle
 * ----------------------------------------------------------------------- */

typedef struct Mod2RmfResamplerSettings
{
    Mod2RmfAmigaFilter    amigaFilter;    /* Hardware filter (pre-resample) */
    Mod2RmfResampleFilter resampleFilter; /* Interpolation used when resampling */
    uint32_t              targetRate;     /* 0 = keep native rate           */
} Mod2RmfResamplerSettings;

/* -----------------------------------------------------------------------
 * Initialisation and parsing
 * ----------------------------------------------------------------------- */

/* Fill *s with defaults: no amiga filter, sinc resampling, native rate. */
void mod2rmf_resampler_defaults(Mod2RmfResamplerSettings *s);

/* Parse an amiga-filter name string ("none", "a500", "a1200").
 * Returns 0 on success, -1 on unrecognised name. */
int mod2rmf_resampler_parse_amiga(const char *name, Mod2RmfAmigaFilter *out);

/* Parse a resample-filter name string ("nearest", "linear", "cubic", "sinc").
 * Returns 0 on success, -1 on unrecognised name. */
int mod2rmf_resampler_parse_filter(const char *name, Mod2RmfResampleFilter *out);

/* -----------------------------------------------------------------------
 * Processing primitives (exposed for testing / future use)
 * ----------------------------------------------------------------------- */

/* Apply the selected Amiga hardware LPF to floating-point samples in-place.
 * Samples are expected in [-1.0, 1.0].  srcRate is the sample rate in Hz.
 * No-op when filter == MOD2RMF_AMIGA_FILTER_NONE or count == 0. */
void mod2rmf_apply_amiga_filter(float *samples, uint32_t count,
                                Mod2RmfAmigaFilter filter, uint32_t srcRate);

/* Resample src (srcCount frames at srcRate Hz) to dstRate Hz using the
 * specified interpolation method.  For downsampling a Lanczos-scaled
 * anti-aliasing low-pass is applied automatically with the sinc filter.
 *
 * Returns a malloc'd float buffer; caller must free it.
 * Sets *dstCount to the output frame count.
 * Returns NULL on failure. */
float *mod2rmf_resample_float(const float *src, uint32_t srcCount,
                               uint32_t srcRate, uint32_t dstRate,
                               uint32_t *dstCount,
                               Mod2RmfResampleFilter filter);

/* -----------------------------------------------------------------------
 * High-level entry point
 * ----------------------------------------------------------------------- */

/* Convert MOD signed 8-bit PCM at srcRate Hz, apply Amiga hardware filter,
 * then optionally resample to settings->targetRate using settings->resampleFilter.
 *
 * Output:
 *   Returns a malloc'd int16_t buffer (caller must free).
 *   *outFrames  - number of frames in the returned buffer.
 *   *outRate    - effective stored sample rate (targetRate if resampled,
 *                 otherwise srcRate).  Apply finetune on top of this value.
 *
 * Input pcm8 encoding: unsigned-biased int8 where (uint8)pcm8[i] - 128 gives
 * the signed sample value in [-128, 127], matching the BAE engine convention.
 *
 * Returns NULL on memory or parameter error. */
int16_t *mod2rmf_process_sample(const int8_t *pcm8, uint32_t frames,
                                 uint32_t srcRate,
                                 const Mod2RmfResamplerSettings *settings,
                                 uint32_t *outFrames, uint32_t *outRate);

/* Print available filter/resample options to stderr. */
void mod2rmf_resampler_print_options(void);

#endif /* MOD2RMF_RESAMPLER_H */
