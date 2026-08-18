/* Mixer-wide windowed sinc (Lanczos-3, 6-tap) for U3232 inner loops.
 * Include only inside #if LOOPS_USED == U3232_LOOPS translation units. */

#ifndef GEN_INTERP_SINC_U3232_INC
#define GEN_INTERP_SINC_U3232_INC

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PV_SINC_FRAC_BITS 8
#define PV_SINC_TABLE_SIZE (1 << PV_SINC_FRAC_BITS)

static int16_t s_lanczos3_q14[PV_SINC_TABLE_SIZE][6];
static int s_lanczos3_ready = 0;

static void PV_InitLanczos3Table(void)
{
    int i, k;
    if (s_lanczos3_ready)
    {
        return;
    }
    for (i = 0; i < PV_SINC_TABLE_SIZE; i++)
    {
        double t = (double)i / (double)PV_SINC_TABLE_SIZE;
        double sum = 0.0;
        double w[6];
        for (k = 0; k < 6; k++)
        {
            double x = (double)(k - 2) - t;
            if (x == 0.0)
            {
                w[k] = 1.0;
            }
            else if (x <= -3.0 || x >= 3.0)
            {
                w[k] = 0.0;
            }
            else
            {
                double pix = M_PI * x;
                double sinc = sin(pix) / pix;
                double pix3 = pix / 3.0;
                double sinc3 = sin(pix3) / pix3;
                w[k] = sinc * sinc3;
            }
            sum += w[k];
        }
        if (sum == 0.0)
        {
            sum = 1.0;
        }
        for (k = 0; k < 6; k++)
        {
            int v = (int)floor(w[k] / sum * 16384.0 + 0.5);
            if (v > 32767)
                v = 32767;
            if (v < -32768)
                v = -32768;
            s_lanczos3_q14[i][k] = (int16_t)v;
        }
    }
    s_lanczos3_ready = 1;
}

static inline int PV_U3232UseSincInterp(void)
{
    return (MusicGlobals && MusicGlobals->interpolationMode == E_SINC_INTERPOLATION_U3232);
}

static inline int32_t PV_SincWrapIndex(int32_t idx, int32_t loopStart, int32_t loopEnd,
                                       int32_t totalFrames, int looping)
{
    if (looping && loopEnd > loopStart)
    {
        int32_t loopLen = loopEnd - loopStart;
        if (idx < loopStart)
        {
            int32_t d = (loopStart - idx) % loopLen;
            return (d == 0) ? loopStart : (loopEnd - d);
        }
        if (idx >= loopEnd)
        {
            return loopStart + ((idx - loopStart) % loopLen);
        }
        return idx;
    }
    if (idx < 0)
        return 0;
    if (totalFrames > 0 && idx >= totalFrames)
        return totalFrames - 1;
    return idx;
}

static inline int32_t PV_SincInterp16Stride(GM_Voice *this_voice, int16_t *source, int32_t pos,
                                            U32 frac, int looping, int stride, int offset)
{
    int32_t loopStart, loopEnd, totalFrames;
    int tab;
    int k;
    int32_t acc;

    PV_InitLanczos3Table();
    loopStart = (int32_t)(this_voice->NoteLoopPtr - this_voice->NotePtr);
    loopEnd = (int32_t)(this_voice->NoteLoopEnd - this_voice->NotePtr);
    totalFrames = (int32_t)(this_voice->NotePtrEnd - this_voice->NotePtr);
    if (stride < 1)
        stride = 1;
    tab = (int)(frac >> (32 - PV_SINC_FRAC_BITS));
    if (tab >= PV_SINC_TABLE_SIZE)
        tab = PV_SINC_TABLE_SIZE - 1;
    acc = 0;
    for (k = 0; k < 6; k++)
    {
        int32_t idx = PV_SincWrapIndex(pos + (k - 2), loopStart, loopEnd, totalFrames, looping);
        acc += ((int32_t)source[idx * stride + offset] * (int32_t)s_lanczos3_q14[tab][k]);
    }
    return acc >> 14;
}

static inline int32_t PV_SincInterp16Auto(GM_Voice *this_voice, int16_t *source, int32_t pos, U32 frac,
                                          int looping)
{
    if (this_voice->channels > 1)
    {
        return PV_SincInterp16Stride(this_voice, source, pos, frac, looping, 2, 0) +
               PV_SincInterp16Stride(this_voice, source, pos, frac, looping, 2, 1);
    }
    return PV_SincInterp16Stride(this_voice, source, pos, frac, looping, 1, 0);
}

#endif /* GEN_INTERP_SINC_U3232_INC */
