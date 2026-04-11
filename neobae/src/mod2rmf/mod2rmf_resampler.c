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

/* mod2rmf_resampler.c
 *
 * PCM pre-processing for mod2rmf:
 *  - Amiga A500 / A1200 hardware low-pass filter simulation
 *  - Sample resampling: nearest, linear, cubic Hermite, 8-tap Lanczos sinc
 *
 * Filter/resampler design notes
 * --------------------------------
 * Amiga A500 passive RC LPF
 *   The original Amiga 500 Paula chip's audio output ran through a passive
 *   RC low-pass filter with a cutoff of ~3275 Hz before reaching the DAC.
 *   Modelled here as a first-order IIR:
 *     alpha = 1 / (fs / (2*pi*fc) + 1)
 *     y[n] = alpha * x[n] + (1 - alpha) * y[n-1]
 *
 * Amiga A1200 passive RC LPF
 *   The A1200 output filter has a much higher cutoff (~28867 Hz).  At
 *   native 8287 Hz sample rate this is above Nyquist and has essentially
 *   no effect; it becomes audible only when samples are upsampled first.
 *
 * 8-tap windowed-sinc resampler (Lanczos-4)
 *   Corresponds to "Sinc + Low-Pass (8 taps)" in tracker players such as
 *   OpenMPT.  Uses a Lanczos window with parameter a=4, giving 8 taps
 *   (source samples floor-3 through floor+4 for each output sample).
 *   When downsampling, the kernel is stretched by (srcRate/dstRate) to
 *   apply automatic anti-aliasing at the output Nyquist.
 */

#include "mod2rmf_resampler.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

/* -----------------------------------------------------------------------
 * Amiga hardware filter cutoff constants (Hz)
 * ----------------------------------------------------------------------- */
#define AMIGA_A500_CUTOFF_HZ    3275.0
#define AMIGA_A1200_CUTOFF_HZ   28867.0

/* -----------------------------------------------------------------------
 * Defaults and parsers
 * ----------------------------------------------------------------------- */

void mod2rmf_resampler_defaults(Mod2RmfResamplerSettings *s)
{
    if (!s) return;
    s->amigaFilter    = MOD2RMF_AMIGA_FILTER_NONE;
    s->resampleFilter = MOD2RMF_RESAMPLE_SINC_8TAP; /* best quality default */
    s->targetRate     = 0;
}

int mod2rmf_resampler_parse_amiga(const char *name, Mod2RmfAmigaFilter *out)
{
    if (!name || !out) return -1;
    if (!strcmp(name, "none") || !strcmp(name, "0"))
        { *out = MOD2RMF_AMIGA_FILTER_NONE;  return 0; }
    if (!strcmp(name, "a500") || !strcmp(name, "500"))
        { *out = MOD2RMF_AMIGA_FILTER_A500;  return 0; }
    if (!strcmp(name, "a1200") || !strcmp(name, "1200"))
        { *out = MOD2RMF_AMIGA_FILTER_A1200; return 0; }
    return -1;
}

int mod2rmf_resampler_parse_filter(const char *name, Mod2RmfResampleFilter *out)
{
    if (!name || !out) return -1;
    if (!strcmp(name, "nearest") || !strcmp(name, "0"))
        { *out = MOD2RMF_RESAMPLE_NEAREST;  return 0; }
    if (!strcmp(name, "linear") || !strcmp(name, "1"))
        { *out = MOD2RMF_RESAMPLE_LINEAR;   return 0; }
    if (!strcmp(name, "cubic") || !strcmp(name, "2"))
        { *out = MOD2RMF_RESAMPLE_CUBIC;    return 0; }
    if (!strcmp(name, "sinc") || !strcmp(name, "sinc8") || !strcmp(name, "3"))
        { *out = MOD2RMF_RESAMPLE_SINC_8TAP; return 0; }
    return -1;
}

/* -----------------------------------------------------------------------
 * Amiga hardware filter
 * ----------------------------------------------------------------------- */

void mod2rmf_apply_amiga_filter(float *samples, uint32_t count,
                                Mod2RmfAmigaFilter filter, uint32_t srcRate)
{
    double cutoff, alpha, y;
    uint32_t i;

    if (!samples || count == 0 || filter == MOD2RMF_AMIGA_FILTER_NONE || srcRate == 0)
        return;

    cutoff = (filter == MOD2RMF_AMIGA_FILTER_A500) ? AMIGA_A500_CUTOFF_HZ
                                                    : AMIGA_A1200_CUTOFF_HZ;

    /* First-order RC IIR (bilinear-free EWM form):
     *   alpha = 1 / (fs / (2*pi*fc) + 1)
     *   y[n]  = alpha * x[n] + (1 - alpha) * y[n-1]
     * Initialise to the first sample to avoid a step-response transient. */
    alpha = 1.0 / ((double)srcRate / (2.0 * M_PI * cutoff) + 1.0);
    y = samples[0];
    for (i = 0; i < count; i++)
    {
        y = alpha * (double)samples[i] + (1.0 - alpha) * y;
        samples[i] = (float)y;
    }
}

/* -----------------------------------------------------------------------
 * Resampling kernel helpers
 * ----------------------------------------------------------------------- */

/* Normalised sinc: sinc(x) = sin(pi*x) / (pi*x), sinc(0) = 1. */
static double sinc_norm(double x)
{
    double px;
    if (fabs(x) < 1e-10) return 1.0;
    px = M_PI * x;
    return sin(px) / px;
}

/* Lanczos-4 kernel: sinc(x) * sinc(x/4) for |x| < 4, else 0.
 * "8 taps" = 4 lobes (a=4), accessing samples floor-3 .. floor+4. */
static double lanczos4(double x)
{
    if (fabs(x) >= 4.0) return 0.0;
    return sinc_norm(x) * sinc_norm(x / 4.0);
}

/* Safe array read clamped to valid range. */
static float read_clamped(const float *src, int32_t count, int32_t i)
{
    if (i < 0)      return src[0];
    if (i >= count) return src[count - 1];
    return src[i];
}

/* Catmull-Rom cubic interpolation at fractional t in [0,1].
 * s0=p[-1]  s1=p[0]  s2=p[+1]  s3=p[+2] */
static float catmull_rom(float s0, float s1, float s2, float s3, double t)
{
    double t2 = t * t;
    double t3 = t2 * t;
    double v  = 0.5 * ((2.0 * s1)
                     + (-s0 + s2) * t
                     + (2.0*s0 - 5.0*s1 + 4.0*s2 - s3) * t2
                     + (-s0 + 3.0*s1 - 3.0*s2 + s3) * t3);
    if (v < -1.0) v = -1.0;
    if (v >  1.0) v =  1.0;
    return (float)v;
}

/* -----------------------------------------------------------------------
 * Core float resampler
 * ----------------------------------------------------------------------- */

float *mod2rmf_resample_float(const float *src, uint32_t srcCount,
                               uint32_t srcRate, uint32_t dstRate,
                               uint32_t *dstCount,
                               Mod2RmfResampleFilter filter)
{
    float  *dst;
    uint32_t n, i;
    double   ratio;   /* source steps per output sample */
    double   cutoff;  /* Lanczos anti-alias cutoff scale (1.0 = no scaling) */
    int32_t  scount;

    if (!src || srcCount == 0 || srcRate == 0 || dstRate == 0 || !dstCount)
        return NULL;

    /* Output length: round to nearest */
    n = (uint32_t)(((uint64_t)srcCount * dstRate + srcRate / 2u) / srcRate);
    if (n == 0) n = 1;

    dst = (float *)malloc((size_t)n * sizeof(float));
    if (!dst) return NULL;

    ratio   = (double)srcRate / (double)dstRate;
    /* When downsampling, scale the sinc kernel to avoid aliasing. */
    cutoff  = (dstRate < srcRate) ? ((double)dstRate / (double)srcRate) : 1.0;
    scount  = (int32_t)srcCount;

    for (i = 0; i < n; i++)
    {
        double   pos  = (double)i * ratio;
        int32_t  base = (int32_t)pos;
        double   frac = pos - (double)base;

        switch (filter)
        {
            case MOD2RMF_RESAMPLE_NEAREST:
            {
                int32_t idx = (int32_t)(pos + 0.5);
                if (idx < 0)      idx = 0;
                if (idx >= scount) idx = scount - 1;
                dst[i] = src[idx];
                break;
            }

            case MOD2RMF_RESAMPLE_LINEAR:
            {
                float s0 = read_clamped(src, scount, base);
                float s1 = read_clamped(src, scount, base + 1);
                dst[i] = s0 + (float)frac * (s1 - s0);
                break;
            }

            case MOD2RMF_RESAMPLE_CUBIC:
            {
                float s0 = read_clamped(src, scount, base - 1);
                float s1 = read_clamped(src, scount, base);
                float s2 = read_clamped(src, scount, base + 1);
                float s3 = read_clamped(src, scount, base + 2);
                dst[i] = catmull_rom(s0, s1, s2, s3, frac);
                break;
            }

            default: /* MOD2RMF_RESAMPLE_SINC_8TAP */
            {
                /* 8-tap Lanczos-4 windowed sinc with anti-aliasing.
                 *
                 * Tap layout around fractional position p = base + frac:
                 *   k = -3 .. +4  →  kernel arg = (frac - k) * cutoff
                 *
                 * Normalise the computed weights so that small rounding
                 * errors in the Lanczos kernel don't cause DC drift. */
                double weights[8];
                double wsum = 0.0;
                double sum  = 0.0;
                int k;

                for (k = -3; k <= 4; k++)
                {
                    double x = (frac - (double)k) * cutoff;
                    weights[k + 3] = lanczos4(x);
                    wsum += weights[k + 3];
                }

                if (wsum < 1e-10)
                {
                    dst[i] = read_clamped(src, scount, base);
                    break;
                }

                for (k = -3; k <= 4; k++)
                    sum += (double)read_clamped(src, scount, base + k)
                           * (weights[k + 3] / wsum);

                if (sum < -1.0) sum = -1.0;
                if (sum >  1.0) sum =  1.0;
                dst[i] = (float)sum;
                break;
            }
        }
    }

    *dstCount = n;
    return dst;
}

/* -----------------------------------------------------------------------
 * High-level entry point
 * ----------------------------------------------------------------------- */

int16_t *mod2rmf_process_sample(const int8_t *pcm8, uint32_t frames,
                                 uint32_t srcRate,
                                 const Mod2RmfResamplerSettings *settings,
                                 uint32_t *outFrames, uint32_t *outRate)
{
    float   *flt = NULL;
    int16_t *result;
    uint32_t i;
    uint32_t resultFrames;
    uint32_t resultRate;
    int      needResample;

    if (!pcm8 || frames == 0 || !settings || !outFrames || !outRate)
        return NULL;

    /* --- Stage 1: int8 MOD PCM → float32 [-1.0, 1.0] ---
     * pcm8 uses unsigned-biased encoding: (uint8_t)pcm8[i] - 128 gives
     * signed values in [-128, 127] where 0 = silence, matching the BAE
     * engine's "val - 0x80" convention. */
    flt = (float *)malloc((size_t)frames * sizeof(float));
    if (!flt) return NULL;

    for (i = 0; i < frames; i++)
        flt[i] = (float)((int32_t)(uint8_t)pcm8[i] - 128) / 128.0f;

    /* --- Stage 2: Amiga hardware LPF (applied at source rate) --- */
    if (settings->amigaFilter != MOD2RMF_AMIGA_FILTER_NONE)
        mod2rmf_apply_amiga_filter(flt, frames, settings->amigaFilter, srcRate);

    /* --- Stage 3: Optional resampling --- */
    resultFrames = frames;
    resultRate   = srcRate;
    needResample = (settings->targetRate != 0 &&
                    settings->targetRate != srcRate);

    if (needResample)
    {
        float   *resampled;
        uint32_t dstCount = 0;

        resampled = mod2rmf_resample_float(flt, frames, srcRate,
                                           settings->targetRate, &dstCount,
                                           settings->resampleFilter);
        free(flt);
        flt = NULL;

        if (!resampled || dstCount == 0)
        {
            free(resampled);
            return NULL;
        }

        flt          = resampled;
        resultFrames = dstCount;
        resultRate   = settings->targetRate;
    }

    /* --- Stage 4: float32 → int16 --- */
    result = (int16_t *)malloc((size_t)resultFrames * sizeof(int16_t));
    if (!result)
    {
        free(flt);
        return NULL;
    }

    for (i = 0; i < resultFrames; i++)
    {
        double v = flt[i];
        if (v < -1.0) v = -1.0;
        if (v >  1.0) v =  1.0;
        /* Scale to signed 16-bit.  Match XConvert8BitTo16Bit convention
         * (positive peak = 32767, negative peak = -32768). */
        result[i] = (v >= 0.0) ? (int16_t)(v * 32767.0)
                                : (int16_t)(v * 32768.0);
    }

    free(flt);
    *outFrames = resultFrames;
    *outRate   = resultRate;
    return result;
}

/* -----------------------------------------------------------------------
 * Help text
 * ----------------------------------------------------------------------- */

void mod2rmf_resampler_print_options(void)
{
    fprintf(stderr,
        "Amiga filter options (--amiga-filter):\n"
        "  none      No hardware filter simulation (default)\n"
        "  a500      Amiga 500 passive RC LPF ~3275 Hz (classic warm sound)\n"
        "  a1200     Amiga 1200 passive RC LPF ~28867 Hz (nearly transparent)\n"
        "\n"
        "Resample filter options (--resample-filter):\n"
        "  nearest   Nearest-neighbor (fastest, lowest quality)\n"
        "  linear    Linear interpolation\n"
        "  cubic     Cubic Hermite / Catmull-Rom\n"
        "  sinc      8-tap Lanczos-4 sinc + auto anti-alias (default)\n"
        "\n"
        "Resample rate (--resample-rate <Hz>):\n"
        "  0         Keep native MOD rate ~8287 Hz (default)\n"
        "  22050     CD half-rate; good for most lossy codecs\n"
        "  44100     CD rate; best for lossless / high-bitrate codecs\n"
        "\n"
        "Examples:\n"
        "  --amiga-filter a500                  Bake in classic A500 sound\n"
        "  --resample-rate 44100                Upsample to 44100 Hz (sinc)\n"
        "  --amiga-filter a500 --resample-rate 44100  Both together\n");
}
