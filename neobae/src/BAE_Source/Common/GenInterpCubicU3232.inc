/* Shared U3232 cubic Hermite helpers for advancedInterpolation (ZMF).
 * Include only inside #if LOOPS_USED == U3232_LOOPS translation units. */

#ifndef GEN_INTERP_CUBIC_U3232_INC
#define GEN_INTERP_CUBIC_U3232_INC

#include <stdint.h>

static inline int32_t PV_CubicHermiteInterpU3232_Shared(int32_t s0, int32_t s1, int32_t s2, int32_t s3, U32 frac)
{
    int32_t t = (int32_t)(frac >> 17); /* 0..32767 */
    int32_t A = -s0 + 3 * s1 - 3 * s2 + s3;
    int32_t B = 2 * s0 - 5 * s1 + 4 * s2 - s3;
    int32_t C = s2 - s0;
    int32_t r;
    r = (int32_t)(((int64_t)A * t) >> 15) + B;
    r = (int32_t)(((int64_t)r * t) >> 15) + C;
    r = (int32_t)(((int64_t)r * t) >> 16);
    return s1 + r;
}

static inline int32_t PV_LoopWrapSample16U3232_Shared(int16_t *source, int32_t idx, int32_t loopStart, int32_t loopEnd)
{
    int32_t loopLen = loopEnd - loopStart;
    if (loopLen <= 0)
        return (int32_t)source[loopStart];
    if (idx < loopStart)
    {
        int32_t d = (loopStart - idx) % loopLen;
        idx = (d == 0) ? loopStart : (loopEnd - d);
    }
    else if (idx >= loopEnd)
    {
        idx = loopStart + ((idx - loopStart) % loopLen);
    }
    return (int32_t)source[idx];
}

#include "GenInterpSincU3232.inc"

/* Mono 16-bit fetch: linear, cubic, or mixer-wide sinc; scaled >> 6 for LPF. */
static inline int32_t PV_FilterFetchInterp16(GM_Voice *this_voice,
                                             int16_t *source,
                                             U32 cur_wave_i,
                                             U32 cur_wave_f,
                                             bool looping)
{
    int32_t b, c;

    if (PV_U3232UseSincInterp())
    {
        return PV_SincInterp16Stride(this_voice, source, (int32_t)cur_wave_i, cur_wave_f,
                                     looping ? 1 : 0, 1, 0) >> 6;
    }
    if (!this_voice->advancedInterpolation)
    {
        b = source[cur_wave_i];
        c = source[cur_wave_i + 1];
        return ((((int32_t)(cur_wave_f >> 17) * (int32_t)(c - b)) >> 15) + b) >> 6;
    }

    {
        int32_t pos = (int32_t)cur_wave_i;
        int32_t s0, s1, s2, s3;
        int32_t loopStartIdx = (int32_t)(this_voice->NoteLoopPtr - this_voice->NotePtr);
        int32_t loopEndIdx = (int32_t)(this_voice->NoteLoopEnd - this_voice->NotePtr);
        int32_t totalFrames = (int32_t)(this_voice->NotePtrEnd - this_voice->NotePtr);

        s1 = source[pos];
        if (looping && loopEndIdx > loopStartIdx)
        {
            s0 = PV_LoopWrapSample16U3232_Shared(source, pos - 1, loopStartIdx, loopEndIdx);
            s2 = PV_LoopWrapSample16U3232_Shared(source, pos + 1, loopStartIdx, loopEndIdx);
            s3 = PV_LoopWrapSample16U3232_Shared(source, pos + 2, loopStartIdx, loopEndIdx);
        }
        else
        {
            s0 = source[(pos > 0) ? pos - 1 : 0];
            s2 = source[(pos + 1 < totalFrames) ? pos + 1 : pos];
            s3 = source[(pos + 2 < totalFrames) ? pos + 2 : ((pos + 1 < totalFrames) ? pos + 1 : pos)];
        }
        return PV_CubicHermiteInterpU3232_Shared(s0, s1, s2, s3, cur_wave_f) >> 6;
    }
}

/* Stereo interleaved 16-bit fetch (frame index), scaled >> 6 for LPF paths. */
static inline void PV_FilterFetchInterp16Stereo(GM_Voice *this_voice,
                                                int16_t *source,
                                                U32 cur_wave_i,
                                                U32 cur_wave_f,
                                                bool looping,
                                                int32_t *outL,
                                                int32_t *outR)
{
    if (PV_U3232UseSincInterp())
    {
        *outL = PV_SincInterp16Stride(this_voice, source, (int32_t)cur_wave_i, cur_wave_f,
                                      looping ? 1 : 0, 2, 0) >> 6;
        *outR = PV_SincInterp16Stride(this_voice, source, (int32_t)cur_wave_i, cur_wave_f,
                                      looping ? 1 : 0, 2, 1) >> 6;
        return;
    }
    if (!this_voice->advancedInterpolation)
    {
        int16_t *cs = source + cur_wave_i * 2;
        int32_t b, c;

        b = cs[0];
        c = cs[2];
        *outL = ((((int32_t)(cur_wave_f >> 17) * (int32_t)(c - b)) >> 15) + b) >> 6;
        b = cs[1];
        c = cs[3];
        *outR = ((((int32_t)(cur_wave_f >> 17) * (int32_t)(c - b)) >> 15) + b) >> 6;
        return;
    }

    {
        int32_t pos = (int32_t)cur_wave_i;
        int32_t idx0, idx2, idx3;
        int32_t loopStartIdx = (int32_t)(this_voice->NoteLoopPtr - this_voice->NotePtr);
        int32_t loopEndIdx = (int32_t)(this_voice->NoteLoopEnd - this_voice->NotePtr);
        int32_t totalFrames = (int32_t)(this_voice->NotePtrEnd - this_voice->NotePtr);

        if (looping && loopEndIdx > loopStartIdx)
        {
            int32_t loopLen = loopEndIdx - loopStartIdx;
            idx0 = pos - 1;
            if (idx0 < loopStartIdx)
                idx0 += loopLen;
            idx2 = pos + 1;
            if (idx2 >= loopEndIdx)
                idx2 -= loopLen;
            idx3 = pos + 2;
            while (idx3 >= loopEndIdx)
                idx3 -= loopLen;
        }
        else
        {
            idx0 = (pos > 0) ? pos - 1 : 0;
            idx2 = (pos + 1 < totalFrames) ? pos + 1 : pos;
            idx3 = (pos + 2 < totalFrames) ? pos + 2 : idx2;
        }

        *outL = PV_CubicHermiteInterpU3232_Shared(source[idx0 * 2],
                                                  source[pos * 2],
                                                  source[idx2 * 2],
                                                  source[idx3 * 2],
                                                  cur_wave_f) >>
                6;
        *outR = PV_CubicHermiteInterpU3232_Shared(source[idx0 * 2 + 1],
                                                  source[pos * 2 + 1],
                                                  source[idx2 * 2 + 1],
                                                  source[idx3 * 2 + 1],
                                                  cur_wave_f) >>
                6;
    }
}

#endif /* GEN_INTERP_CUBIC_U3232_INC */
