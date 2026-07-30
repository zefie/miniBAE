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
 
/*
 * This file contains the implementation of the MobileBAE, ported from RetroDLS
 * Original code available at: https://github.com/Magstic/RetroDLS
 *
 * Used under the MIT License.
 *
 * Copyright (c) 2026 Magstic
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "GenDLS_MobileBAE.h"
#include "X_API.h"
#include "GenPriv.h"
#include "GenDLS_MobileBAE_Tables.h"
#include "X_Assert.h"

#if USE_MPEG_DECODER == TRUE
#include "XMPEG_BAE_API.h"
#endif

void GM_SetMixerDLSMode(bool isDLS) 
{
    GM_Mixer* pMixer = GM_GetCurrentMixer();
    if (pMixer) 
    {
        pMixer->isDLS = isDLS;
    }
}

bool g_use_mobilebae_quirks = true;

static bool dls_bank_quirks(const DLS_Bank* bank) {
    return g_use_mobilebae_quirks || (bank && bank->forceQuirks);
}

static bool dls_bank_spmidi(const DLS_Bank* bank) {
    return !g_use_mobilebae_quirks && !(bank && bank->forceQuirks);
}

static void DLS_ApplyDirectConnection(DLS_Articulation* art, const DLS_Connection* connection, bool quirks);
static void dls_refresh_current_synth_for_mode(void);

void GM_DLS_SetMobileBAEQuirks(bool useQuirks) 
{
    g_use_mobilebae_quirks = useQuirks;
    dls_refresh_current_synth_for_mode();
}

bool GM_DLS_GetMobileBAEQuirks() 
{
    return g_use_mobilebae_quirks;
}

// ============================================================================
// DLS MATH AND RENDER HELPERS
// ============================================================================
// Fixed-point macro helpers
#define DLS_FP_SHIFT 16
#define DLS_FP_ONE (1 << DLS_FP_SHIFT)
#define DLS_FP_MUL(a, b) (int32_t)(((int64_t)(a) * (b)) >> DLS_FP_SHIFT)
#define DLS_FP_DIV(a, b) (int32_t)((((int64_t)(a)) << DLS_FP_SHIFT) / (b))

static int32_t dls_pow2_millibels(int32_t mB) {
    if (mB < -150515) return 0;
    if (mB > 150515) return 0x7FFFFFFF;
    int32_t log2_val = (int32_t)(((int64_t)mB * 217706) >> 16);
    int32_t i_part = log2_val >> 16;
    int32_t f_part = log2_val & 0xFFFF;
    int32_t val = EXP2_TABLE[(f_part * 79) >> 16];
    if (i_part > 0) return val << i_part;
    return val >> (-i_part);
}

static int32_t dls_pow10_millibels(int32_t mB) {
    if (mB < -150515) return 0;
    if (mB > 150515) return 0x7FFFFFFF;
    int32_t log10_val = (int32_t)(((int64_t)mB * 6554) >> 16);
    int32_t i_part = log10_val >> 16;
    int32_t f_part = log10_val & 0xFFFF;
    int32_t val = EXP10_TABLE[(f_part * 79) >> 16];
    if (i_part > 0) return val << i_part;
    return val >> (-i_part);
}

static int32_t dls_pitch_cents_to_ratio_q16(int32_t pitchCents) {
    /* Match RetroDLS pitchRatioQ16(): pitchCents is cents in Q16.16. */
    int32_t pitchSemitones = pitchCents / 100;
    const int32_t octaveSize = 12 << 16;
    int32_t octave = pitchSemitones / octaveSize;
    int32_t inOctave = pitchSemitones % octaveSize;
    if (inOctave < 0) {
        inOctave += octaveSize;
        octave--;
    }

    int32_t semitone = inOctave >> 16;
    int32_t cent = (100 * (inOctave & 0xFFFF)) >> 16;
    int64_t ratio = DLS_FP_MUL(PITCH_SEMITONE[semitone], PITCH_CENT[cent]);
    if (octave >= 0) {
        ratio = octave >= 14 ? INT32_MAX : ratio << octave;
    } else {
        ratio >>= -octave;
    }

    return (int32_t)ratio;
}

static int32_t dls_time_cents_to_samples(int32_t tc, int32_t sampleRate) {
    if (tc == INT32_MIN || sampleRate <= 0) return 0;
    return (int32_t)(((int64_t)dls_pitch_cents_to_ratio_q16(tc) * sampleRate) >> 16);
}

static int32_t dls_pitch_cents_to_increment(int32_t pitchCents, int32_t sampleRate, int32_t sourceSampleRate) {
    if (sampleRate <= 0) return 0;
    return (int32_t)(((int64_t)dls_pitch_cents_to_ratio_q16(pitchCents) * sourceSampleRate) / sampleRate);
}

static inline int32_t dls_clamp(int32_t value, int32_t min, int32_t max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static int32_t dls_log10_q16(int32_t value) {
    int32_t v = value <= 0 ? 1 : value;
    int32_t bias = 0;
    while (v >= 0xA0000) {
        v /= 10;
        bias += 0x10000;
    }
    while (v < 0x10000) {
        v *= 10;
        bias -= 0x10000;
    }
    int32_t x = v >> 7;
    int32_t index = (x - 512) / 9;
    if (index < 0) index = 0;
    if (index > 511) index = 511;
    int32_t fraction = x - 9 * index - 512;
    return bias + LOG10_TABLE[index] + ((LOG10_TABLE[index + 1] - LOG10_TABLE[index]) * fraction) / 9;
}

static int32_t exponent_table_scale(int32_t value, int32_t scale) {
    uint32_t uValue = (uint32_t)value;
    uint32_t uScale = (uint32_t)scale;
    uint32_t low = ((uValue & 0xFFFF) * (uScale & 0xFFFF)) >> 16;
    uint32_t high = (uValue & 0xFFFF) * (uScale >> 16) + uScale * (uValue >> 16);
    return (int32_t)((high + low + 128) >> 8);
}

static int32_t dls_exp2_q16(int32_t x) {
    if (x < -0x100000) return 1;
    int32_t value = x;
    uint32_t scale = 0x1000000u;
    while (value >= 0x10000) { value -= 0x10000; scale <<= 1; }
    while (value < 0 && scale != 0) { value += 0x10000; scale >>= 1; }
    int32_t index = value >> 7;
    int32_t rem = (int32_t)((uint32_t)value & 0x7F);
    int32_t tableValue = EXP2_INTERP_TABLE[index]
        + ((int32_t)(((int64_t)(EXP2_INTERP_TABLE[index + 1] - EXP2_INTERP_TABLE[index]) * rem) >> 7));
    return exponent_table_scale(tableValue, (int32_t)scale);
}

static int32_t dls_exp10_q16(int32_t x) {
    if (x < -315652) return 1;
    int32_t value = x;
    uint32_t scale = 0x1000000u;
    while (value >= 0x10000) { value -= 0x10000; scale *= 10u; }
    while (value < 0 && scale != 0) { value += 0x10000; scale /= 10u; }
    int32_t index = value >> 7;
    int32_t rem = (int32_t)((uint32_t)value & 0x7F);
    int32_t tableValue = EXP10_INTERP_TABLE[index]
        + ((int32_t)(((int64_t)(EXP10_INTERP_TABLE[index + 1] - EXP10_INTERP_TABLE[index]) * rem) >> 7));
    return exponent_table_scale(tableValue, (int32_t)scale);
}

static int32_t dls_pan_scale_q16(int32_t pan, bool quirks) {
    if (quirks) {
        int32_t index = (((500 * (dls_clamp(pan, -0x10000, 0x10000) + 0x10000)) >> 16) & 0xFFFF) >> 1;
        if (index < 0) index = 0;
        if (index >= (int32_t)(sizeof(MOBILEBAE_PAN_TABLE) / sizeof(MOBILEBAE_PAN_TABLE[0]))) {
            index = (int32_t)(sizeof(MOBILEBAE_PAN_TABLE) / sizeof(MOBILEBAE_PAN_TABLE[0])) - 1;
        }
        return MOBILEBAE_PAN_TABLE[index];
    } else {
        /* DLS v2.2 pan law: Left  = 20*log10(cos(pi/2 * (PAN+50%)))
                             Right = 20*log10(sin(pi/2 * (PAN+50%)))
           pan is [-0x10000, 0x10000], and we're computing the scale factor for
           one channel. When pan == 0 (center), both channels are 0.707 (-3dB).
           We use the same MOBILEBAE_PAN_TABLE as an empirical approximation
           of the equal-power pan curve with the correct DLS v2.2 scaling. */
        int32_t index = (((508 * (dls_clamp(pan, -0x10000, 0x10000) + 0x10000)) >> 16) & 0xFFFF) >> 1;
        if (index < 0) index = 0;
        if (index >= (int32_t)(sizeof(MOBILEBAE_PAN_TABLE) / sizeof(MOBILEBAE_PAN_TABLE[0]))) {
            index = (int32_t)(sizeof(MOBILEBAE_PAN_TABLE) / sizeof(MOBILEBAE_PAN_TABLE[0])) - 1;
        }
        return MOBILEBAE_PAN_TABLE[index];
    }
}

static int32_t dls_timecent_to_micros(int32_t scale) {
    if (scale == INT32_MIN) return 0;
    int64_t micros = ((1000LL * dls_exp2_q16(scale / 1200)) >> 16) * 1000LL;
    if (micros < 0) return 0;
    if (micros > 40000000LL) return 40000000;
    return (int32_t)micros;
}

static int32_t dls_plus_lfo_period(int32_t scale) {
    int32_t ratio = dls_exp2_q16(scale / 1200);
    int32_t lo = ratio & 0xFFFF;
    int32_t v = 535809 * (ratio >> 16) + ((11521 * lo) >> 16) + 8 * lo;
    if (v < 6554) v = 6554;
    return 1000 * (DLS_FP_DIV(65536000, v) >> 16);
}

static int32_t dls_micros_to_control_ticks(int32_t micros) {
    if (micros <= 5000) return 0;
    int64_t ticks = micros / 10000LL;
    if (ticks > INT32_MAX) return INT32_MAX;
    return (int32_t)ticks;
}

static int32_t dls_eg1_multiplier(int32_t micros) {
    int32_t ticks = (int32_t)dls_clamp((micros + 9999) / 10000, 0, 0x9C40);
    if (ticks < 2) return 0;
    int32_t band = 13;
    int32_t limit = 4;
    for (int32_t i = 0; i < 13; i++, limit <<= 1) {
        if (ticks < limit) {
            band = i;
            break;
        }
    }
    return (int32_t)(((int64_t)EG1_MULTIPLIER_SLOPE[band] * ticks) >> 16) + EG1_MULTIPLIER_BASE[band];
}

static int64_t dls_eg1_sustain_target(int32_t sustainQ16) {
    int32_t sustain = dls_clamp(sustainQ16, 0, 0x10000);
    int32_t index = 511 - (int32_t)(((int64_t)(sustain & 0xFFFF) * 511) >> 16) - ((sustain >> 16) * 511);
    int32_t tableValue = index >= 511 ? 0 : dls_exp2_q16(-(index << 16) / 32);
    return (int64_t)tableValue * 0xFFFFLL;
}

static int32_t dls_eg1_level(int64_t current) {
    return current >= DLS_EG1_FULL ? 0x10000 : (int32_t)(current >> 16);
}

static int32_t dls_modulated_time_micros(int32_t baseMicros, int32_t valueQ16) {
    if (baseMicros <= 0) return 0;
    int64_t val = ((int64_t)baseMicros * dls_exp2_q16(valueQ16 / 1200)) >> 16;
    if (val < 0) return 0;
    if (val > 40000000LL) return 40000000;
    return (int32_t)val;
}

// ----------------------------------------------------------------------------
// LFO
// ----------------------------------------------------------------------------
static void dls_lfo_init(DLS_Lfo* lfo, int32_t periodMicros, int32_t startDelayMicros, int32_t sampleRate) {
    (void)sampleRate;
    lfo->startDelay = dls_clamp(startDelayMicros, 0, 10000000);
    lfo->period = dls_clamp(periodMicros, 50000, 10000000);
    lfo->phase = lfo->period >> 2;
    lfo->output = 0x8000;
    lfo->active = false;
}

static int32_t dls_lfo_next(DLS_Lfo* lfo, int32_t frames) {
    (void)frames;
    if (!lfo->active) {
        lfo->phase += 10000;
        if ((uint32_t)lfo->phase >= (uint32_t)lfo->startDelay) {
            lfo->active = true;
            lfo->phase = lfo->period >> 2;
        }
        return lfo->output;
    }
    int32_t folded = lfo->phase;
    if ((uint32_t)lfo->phase >= (uint32_t)(lfo->period >> 1)) {
        /* RetroDLS folding: 32-bit overflow matches the original
           MobileBAE Plus sub_11F59B0 ARM multiply:
           folded = period + (0x03FFFFFF * phase)  mod 2^32 */
        folded = (int32_t)((uint32_t)lfo->period + 0x03FFFFFFu * (uint32_t)lfo->phase);
    }
    /* RetroDLS uses Integer.divideUnsigned on the 32-bit wrapped
       product, so shift the unsigned folded value and divide. */
    {
        uint32_t shifted = (uint32_t)(folded << 6);
        uint32_t den = (uint32_t)(lfo->period >> 3);
        lfo->output = (int32_t)(((uint64_t)shifted / (den > 0 ? den : 1)) << 8);
    }
    lfo->phase += 10000;
    if ((uint32_t)lfo->phase >= (uint32_t)lfo->period) {
        lfo->phase -= lfo->period;
    }
    return lfo->output;
}

// ----------------------------------------------------------------------------
// ENVELOPE
// ----------------------------------------------------------------------------
enum {
    DLS_ENV_DELAY = 0,
    DLS_ENV_ATTACK = 1,
    DLS_ENV_HOLD = 2,
    DLS_ENV_DECAY = 3,
    DLS_ENV_SUSTAIN = 4,
    DLS_ENV_RELEASE = 5,
    DLS_ENV_FINISHED = 6,
    DLS_ENV_SHUTDOWN = 7
};

#define DLS_FORCED_FADE_MICROS 15000

static int32_t dls_env_eg2_ramp_step(int32_t elapsedMicros, int32_t durationMicros)
{
    if (durationMicros <= 0)
    {
        return DLS_EG2_FULL;
    }
    {
        int32_t divisor = durationMicros >> 2;
        int64_t step;
        if (divisor <= 0)
        {
            return DLS_EG2_FULL;
        }
        step = ((((int64_t)elapsedMicros) << 6) / divisor) << 8;
        return step >= DLS_EG2_FULL ? DLS_EG2_FULL : (int32_t)step;
    }
}

static void dls_env_init(DLS_Envelope* env, int32_t delayMicros, int32_t attackMicros, int32_t holdMicros,
                         int32_t decayMicros, int32_t sustainQ16, int32_t releaseMicros,
                         bool eg1, int32_t sampleRate) {
    (void)sampleRate;
    env->delayMicros = dls_clamp(delayMicros, 0, 40000000);
    env->attackMicros = dls_clamp(attackMicros, 0, 40000000);
    env->holdMicros = dls_clamp(holdMicros, 0, 40000000);
    env->decayMicros = dls_clamp(decayMicros, 0, 40000000);
    env->attackTicks = dls_micros_to_control_ticks(env->attackMicros);
    env->decayTicks = dls_micros_to_control_ticks(env->decayMicros);
    env->releaseMicros = dls_clamp(releaseMicros, 0, 40000000);
    env->activeReleaseMicros = env->releaseMicros;
    env->sustain = eg1 ? dls_clamp(sustainQ16, 0, 0x10000)
                       : (int32_t)(((int64_t)DLS_EG2_FULL * dls_clamp(sustainQ16, 0, 0x10000)) >> 16);
    env->eg1 = eg1;
    env->eg1Sustain = dls_eg1_sustain_target(sustainQ16);
    env->decayMultiplier = dls_eg1_multiplier(env->decayMicros);
    env->activeReleaseMultiplier = dls_eg1_multiplier(env->releaseMicros);
    env->current = 0;
    env->eg1Current = 0;
    env->stage = env->delayMicros == 0 ? DLS_ENV_ATTACK : DLS_ENV_DELAY;
    env->tickIndex = 0;
    env->shutdownStart = 0;
    env->finished = false;
}

static void dls_env_release(DLS_Envelope* env, int32_t durationMicros) {
    if (!env->finished && env->stage != DLS_ENV_SHUTDOWN) {
        env->activeReleaseMicros = dls_clamp(durationMicros, 0, 40000000);
        env->activeReleaseMultiplier = dls_eg1_multiplier(env->activeReleaseMicros);
        env->tickIndex = env->eg1 ? 0 : (int32_t)(((int64_t)(DLS_EG2_FULL - env->current) * env->activeReleaseMicros) >> 16);
        env->stage = DLS_ENV_RELEASE;
    }
}

static void dls_env_shutdown(DLS_Envelope* env)
{
    if (!env->finished)
    {
        int32_t fadeMicros;
        if (env->forceQuirks) {
            fadeMicros = DLS_FORCED_FADE_MICROS;
        } else {
            fadeMicros = env->shutdownMicros > 0 ? env->shutdownMicros : DLS_FORCED_FADE_MICROS;
        }
        env->activeReleaseMicros = fadeMicros;
        env->activeReleaseMultiplier = dls_eg1_multiplier(fadeMicros);
        env->shutdownStart = env->current;
        env->tickIndex = 0;
        env->stage = DLS_ENV_SHUTDOWN;
    }
}

static int32_t dls_env_next_eg1(DLS_Envelope* env);

static int32_t dls_env_next_linear(DLS_Envelope* env) {
    if (env->finished) return 0;
    if (env->stage == DLS_ENV_DELAY) {
        env->current = 0;
        if (env->tickIndex >= env->delayMicros) {
            env->stage = DLS_ENV_ATTACK;
            env->tickIndex = 10000;
        } else {
            env->tickIndex += 10000;
        }
    } else if (env->stage == DLS_ENV_ATTACK) {
        int32_t step;
        if (env->attackTicks == 0) {
            env->current = DLS_EG2_FULL;
            env->stage = env->holdMicros == 0 ? (env->decayTicks == 0 ? DLS_ENV_SUSTAIN : DLS_ENV_DECAY) : DLS_ENV_HOLD;
            env->tickIndex = 0;
            return dls_env_next_linear(env);
        }
        step = dls_env_eg2_ramp_step(env->tickIndex, env->attackMicros);
        env->current = step;
        if (step >= DLS_EG2_FULL) {
            env->stage = env->holdMicros == 0 ? (env->decayTicks == 0 ? DLS_ENV_SUSTAIN : DLS_ENV_DECAY) : DLS_ENV_HOLD;
            env->tickIndex = env->holdMicros == 0 ? 0 : 10000;
            env->current = DLS_EG2_FULL;
        } else {
            env->tickIndex += 10000;
        }
    } else if (env->stage == DLS_ENV_HOLD) {
        env->current = DLS_EG2_FULL;
        if (env->tickIndex >= env->holdMicros) {
            env->stage = env->decayTicks == 0 ? DLS_ENV_SUSTAIN : DLS_ENV_DECAY;
            env->tickIndex = 10000;
        } else {
            env->tickIndex += 10000;
        }
    } else if (env->stage == DLS_ENV_DECAY) {
        env->current = DLS_EG2_FULL - dls_env_eg2_ramp_step(env->tickIndex, env->decayMicros);
        if (env->current <= env->sustain) {
            env->stage = DLS_ENV_SUSTAIN;
            env->current = env->sustain;
        } else {
            env->tickIndex += 10000;
        }
    } else if (env->stage == DLS_ENV_SUSTAIN) {
        env->current = env->sustain;
    } else if (env->stage == DLS_ENV_RELEASE) {
        int32_t step = dls_env_eg2_ramp_step(env->tickIndex, env->activeReleaseMicros);
        env->current = DLS_EG2_FULL - step;
        if (step >= DLS_EG2_FULL) {
            env->current = 0;
            env->finished = true;
        } else {
            env->tickIndex += 10000;
        }
    } else if (env->stage == DLS_ENV_SHUTDOWN) {
        int32_t fadeMicros;
        if (env->forceQuirks) {
            fadeMicros = DLS_FORCED_FADE_MICROS;
        } else {
            fadeMicros = env->shutdownMicros > 0 ? env->shutdownMicros : DLS_FORCED_FADE_MICROS;
        }
        int32_t remaining = DLS_EG2_FULL - dls_env_eg2_ramp_step(env->tickIndex, fadeMicros);
        env->current = DLS_FP_MUL(env->shutdownStart, remaining);
        if (env->current <= 0 || remaining <= 0) {
            env->current = 0;
            env->finished = true;
            env->stage = DLS_ENV_FINISHED;
        } else {
            env->tickIndex += 10000;
        }
    }
    return dls_clamp(env->current, 0, DLS_EG2_FULL);
}

static int32_t dls_env_next_eg1(DLS_Envelope* env) {
    if (env->finished) return 0;
    int32_t output = 0;
    if (env->stage == DLS_ENV_DELAY) {
        env->eg1Current = 0;
        output = 0;
        if (env->tickIndex >= env->delayMicros) {
            env->stage = DLS_ENV_ATTACK;
            env->tickIndex = 10000;
        } else {
            env->tickIndex += 10000;
        }
    } else if (env->stage == DLS_ENV_ATTACK) {
        if (env->attackMicros == 0) {
            env->stage = env->holdMicros == 0 ? (env->decayMicros == 0 ? DLS_ENV_SUSTAIN : DLS_ENV_DECAY) : DLS_ENV_HOLD;
            env->tickIndex = 0;
            env->eg1Current = DLS_EG1_FULL;
            return dls_env_next_eg1(env);
        }
        int32_t denom = (env->attackMicros >> 2) > 0 ? (env->attackMicros >> 2) : 1;
        int64_t level = (((int64_t)env->tickIndex << 6) / denom) << 8;
        if (level >= 0xFFFFLL) {
            env->stage = env->holdMicros == 0 ? (env->decayMicros == 0 ? DLS_ENV_SUSTAIN : DLS_ENV_DECAY) : DLS_ENV_HOLD;
            env->tickIndex = 10000;
            env->eg1Current = DLS_EG1_FULL;
            output = dls_eg1_level(env->eg1Current);
        } else {
            env->eg1Current = level << 16;
            output = dls_eg1_level(env->eg1Current);
            env->tickIndex += 10000;
        }
    } else if (env->stage == DLS_ENV_HOLD) {
        env->eg1Current = DLS_EG1_FULL;
        output = dls_eg1_level(env->eg1Current);
        if (env->tickIndex >= env->holdMicros) {
            env->stage = env->decayMicros == 0 ? DLS_ENV_SUSTAIN : DLS_ENV_DECAY;
            env->tickIndex = 0;
        } else {
            env->tickIndex += 10000;
        }
    } else if (env->stage == DLS_ENV_DECAY) {
        output = dls_eg1_level(env->eg1Current);
        env->eg1Current = (env->eg1Current * env->decayMultiplier) >> 16;
        if (env->eg1Current <= env->eg1Sustain) {
            env->stage = DLS_ENV_SUSTAIN;
            env->eg1Current = env->eg1Sustain;
        }
    } else if (env->stage == DLS_ENV_SUSTAIN) {
        env->eg1Current = env->eg1Sustain;
        output = dls_eg1_level(env->eg1Current);
    } else if (env->stage == DLS_ENV_RELEASE || env->stage == DLS_ENV_SHUTDOWN) {
        if (env->activeReleaseMicros == 0) {
            env->eg1Current = 0;
            output = 0;
        } else {
            output = dls_eg1_level(env->eg1Current);
            env->eg1Current = (env->eg1Current * env->activeReleaseMultiplier) >> 16;
        }
    } else {
        env->eg1Current = 0;
        output = 0;
    }
    env->current = output;
    if (env->stage > DLS_ENV_ATTACK && output == 0) {
        env->finished = true;
        env->stage = DLS_ENV_FINISHED;
    }
    return output;
}

static int32_t dls_env_next(DLS_Envelope* env, int32_t frames) {
    (void)frames;
    if (env->eg1) return dls_env_next_eg1(env);
    return dls_env_next_linear(env);
}

// ----------------------------------------------------------------------------
// FILTER
// ----------------------------------------------------------------------------

static int32_t filter_mapped_cutoff(int32_t logValue) {
    int32_t lo = logValue & 0xFFFF;
    int32_t hi = logValue >> 16;
    return 12 * (((21098 * lo) >> 16) + 217706 * hi + 3 * lo + 376832);
}

static void dls_filter_update_coeffs(DLS_PlusFilter* f, int32_t cutoff) {
    cutoff = cutoff > f->lowThreshold ? cutoff : f->lowThreshold;
    int32_t norm = 65 * DLS_FP_DIV(cutoff - f->lowThreshold, f->midThreshold - f->lowThreshold);
    int32_t res = f->resonance > 0 ? f->resonance : 0;
    int32_t resIndex = (16 * DLS_FP_DIV(res, FILTER_MAX_RESONANCE)) >> 16;
    if (resIndex > 15) resIndex = 15;
    
    int32_t normIndex = norm >> 16;
    int32_t i0 = normIndex < 64 ? normIndex : 64;
    int32_t i1 = (i0 + 1) < 64 ? (i0 + 1) : 64;
    int32_t fraction = norm - (i0 << 16);
    int32_t t0 = 65 * resIndex + i0;
    int32_t t1 = 65 * resIndex + i1;
    
    f->c1 = FILTER_C1[t0] + (int32_t)(((int64_t)fraction * (FILTER_C1[t1] - FILTER_C1[t0])) >> 16);
    f->c2 = FILTER_C2[t0] + (int32_t)(((int64_t)fraction * (FILTER_C2[t1] - FILTER_C2[t0])) >> 16);
    f->c0 = DLS_FP_MUL(FILTER_C0_SCALE[resIndex], f->c1 + f->c2 + 0x10000);
    
    if (normIndex >= 65) {
        int32_t extra = DLS_FP_DIV(cutoff - f->midThreshold, f->highThreshold - f->midThreshold);
        f->c0 += DLS_FP_MUL(extra, 0x10000 - f->c0);
        f->c1 = DLS_FP_MUL(f->c1, 0x10000 - extra);
        f->c2 = DLS_FP_MUL(f->c2, 0x10000 - extra);
    }
}

static void dls_filter_init(DLS_PlusFilter* f, int32_t sampleRate, int32_t cutoff, int32_t resonance) {
    int64_t sampleRateLike = ((int64_t)sampleRate) << 16;
    int32_t base = (int32_t)(sampleRateLike / 440);
    f->lowThreshold = filter_mapped_cutoff(dls_log10_q16(base / 240));
    f->midThreshold = filter_mapped_cutoff(dls_log10_q16(base / 6));
    f->highThreshold = filter_mapped_cutoff(dls_log10_q16(base / 2));
    f->baseCutoff = cutoff > FILTER_MIN_CUTOFF ? cutoff : FILTER_MIN_CUTOFF;
    f->resonance = dls_clamp(resonance, 0, FILTER_MAX_RESONANCE);
    f->effectiveCutoff = f->baseCutoff;
    
    f->h1Left = 0; f->h2Left = 0;
    f->h1Right = 0; f->h2Right = 0;
    
    if (f->effectiveCutoff < f->highThreshold) {
        dls_filter_update_coeffs(f, f->effectiveCutoff);
    }
}

static bool dls_filter_enabled(DLS_PlusFilter* f) {
    return f->effectiveCutoff < f->highThreshold;
}

static void dls_filter_update(DLS_PlusFilter* f, int32_t runtimeCutoffDelta, int32_t runtimeResonanceDelta) {
    (void)runtimeResonanceDelta;
    int32_t cutoff = f->baseCutoff + runtimeCutoffDelta;
    if (cutoff < 0) cutoff = 0;
    if (cutoff != f->effectiveCutoff) {
        f->effectiveCutoff = cutoff;
        if (f->effectiveCutoff < f->highThreshold) {
            dls_filter_update_coeffs(f, f->effectiveCutoff);
        }
    }
}

static int32_t dls_filtered_raw(DLS_PlusFilter* f, int32_t sample, int32_t h1, int32_t h2) {
    return ((f->c0 * sample) >> 6) - (((f->c2 >> 3) * (h2 >> 9)) >> 4) - (((f->c1 >> 4) * (h1 >> 9)) >> 3);
}

static int32_t dls_filter_next_left(DLS_PlusFilter* f, int32_t sample) {
    int32_t raw = dls_filtered_raw(f, sample, f->h1Left, f->h2Left);
    f->h2Left = f->h1Left;
    f->h1Left = raw;
    return raw >> 10;
}

static int32_t dls_filter_next_right(DLS_PlusFilter* f, int32_t sample) {
    int32_t raw = dls_filtered_raw(f, sample, f->h1Right, f->h2Right);
    f->h2Right = f->h1Right;
    f->h1Right = raw;
    return raw >> 10;
}

// Helper to read Little-Endian values from memory
static uint32_t read_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_chunk_id(const uint8_t* p) {
    // Keep it as a 32-bit integer for fast comparison, assuming Little-Endian chunk IDs 
    // e.g., 'RIFF' -> 0x46464952 (on LE machines)
    // Actually, just read 4 chars into a uint32_t directly without endian swapping 
    // so 'RIFF' string directly matches 0x46464952
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t DLS_Selector(int32_t bankMsb, int32_t bankLsb, int32_t program) {
    return (uint32_t)(program & 0x7F) | ((uint32_t)(bankLsb & 0x7F) << 8) |
           ((uint32_t)(bankMsb & 0x7F) << 16);
}

#define CHUNK_RIFF 0x46464952
#define CHUNK_DLS  0x20534C44
#define CHUNK_LIST 0x5453494C
#define CHUNK_INFO 0x4F464E49
#define CHUNK_INAM 0x4D414E49
#define CHUNK_LINS 0x736E696C
#define CHUNK_INS  0x20736E69
#define CHUNK_INSH 0x68736E69
#define CHUNK_LRGN 0x6E67726C
#define CHUNK_RGNH 0x686E6772
#define CHUNK_LART 0x7472616C
#define CHUNK_ART1 0x31747261
#define CHUNK_WVPL 0x6C707677
#define CHUNK_WAVE 0x45564157
#define CHUNK_FMT  0x20746D66
#define CHUNK_WSMP 0x706D7377
#define CHUNK_DATA 0x61746164
#define CHUNK_PTBL 0x6C627470

typedef struct {
    const uint8_t* start;
    const uint8_t* pos;
    const uint8_t* end;
} DLS_ParserState;

static bool looks_like_wave_chunk(const uint8_t* p, const uint8_t* end) {
    if (p + 12 > end) return false;
    uint32_t id = read_chunk_id(p);
    uint32_t type = read_chunk_id(p + 8);
    return (id == CHUNK_RIFF && type == CHUNK_WAVE) || (id == CHUNK_LIST && type == read_chunk_id((const uint8_t*)"wave"));
}

static int16_t decode_alaw(uint8_t input) {
    int32_t u = (input & 0xFF) ^ 0x55;
    int32_t exponent = (u & 0x70) >> 4;
    int32_t mantissa = u & 0x0F;
    int32_t sample = exponent == 0 ? (mantissa << 4) + 8 : ((mantissa << 4) + 0x108) << (exponent - 1);
    return (int16_t)((u & 0x80) != 0 ? sample : -sample);
}

static int16_t decode_ulaw(uint8_t input) {
    int32_t u = (input & 0xFF) ^ 0xFF;
    int32_t sample = ((u & 0x0F) << 3) + 0x84;
    sample <<= (u & 0x70) >> 4;
    return (int16_t)((u & 0x80) != 0 ? 0x84 - sample : sample - 0x84);
}

static int32_t decode_ima_nibble(int32_t predictor, int32_t index, int32_t nibble) {
    int32_t step = IMA_STEP_TABLE[index];
    int32_t diff = step >> 3;
    if ((nibble & 1) != 0) diff += step >> 2;
    if ((nibble & 2) != 0) diff += step >> 1;
    if ((nibble & 4) != 0) diff += step;
    int32_t result = predictor + ((nibble & 8) == 0 ? diff : -diff);
    return dls_clamp(result, -32768, 32767);
}

static OPErr dls_decode_mpeg_wave(const uint8_t* encoded, uint32_t encodedBytes, DLS_Wave* wave) {
#if USE_MPEG_DECODER != FALSE
    if (!encoded || encodedBytes == 0 || !wave) return BAD_FILE_TYPE;

    OPErr err = NO_ERR;
    XMPEGDecodedData* stream = XOpenMPEGStreamFromMemory((XPTR)encoded, encodedBytes, &err);
    if (!stream || err != NO_ERR) return BAD_FILE_TYPE;

    if (stream->channels != 1 && stream->channels != 2) {
        XCloseMPEGStream(stream);
        return BAD_FILE_TYPE;
    }

    uint64_t decodeBytes64 = (uint64_t)stream->maxFrameBuffers * (uint64_t)stream->frameBufferSize;
    if (decodeBytes64 == 0 || decodeBytes64 > INT32_MAX) {
        XCloseMPEGStream(stream);
        return BAD_FILE_TYPE;
    }

    uint32_t decodeBytes = (uint32_t)decodeBytes64;
    signed char* decoded = (signed char*)XNewPtr(decodeBytes);
    if (!decoded) {
        XCloseMPEGStream(stream);
        return MEMORY_ERR;
    }

    signed char* writePtr = decoded;
    for (uint32_t count = 0; count < stream->maxFrameBuffers; count++) {
        bool done = FALSE;
        err = XFillMPEGStreamBuffer(stream, writePtr, &done);
        if (err != NO_ERR || done) {
            break;
        }
        writePtr += stream->frameBufferSize;
    }

    uint32_t usefulBytes = (uint32_t)(writePtr - decoded);
    if (err != NO_ERR || usefulBytes == 0) {
        XDisposePtr((XPTR)decoded);
        XCloseMPEGStream(stream);
        return BAD_FILE_TYPE;
    }

    wave->channels = stream->channels;
    wave->sampleRate = stream->sampleRate;
    wave->bitsPerSample = 16;
    wave->frames = usefulBytes / (wave->channels * 2);
    wave->pcm = (int16_t*)XNewPtr((wave->frames + 1) * wave->channels * sizeof(int16_t));
    if (!wave->pcm) {
        XDisposePtr((XPTR)decoded);
        XCloseMPEGStream(stream);
        return MEMORY_ERR;
    }

    for (uint32_t i = 0, p = 0; i < wave->frames * wave->channels; i++, p += 2) {
        wave->pcm[i] = (int16_t)((decoded[p] & 0xFF) | (decoded[p + 1] << 8));
    }

    XDisposePtr((XPTR)decoded);
    XCloseMPEGStream(stream);
    return NO_ERR;
#else
    (void)encoded;
    (void)encodedBytes;
    (void)wave;
    return BAD_FILE_TYPE;
#endif
}

static OPErr DLS_Parse_Wave_Data(const uint8_t* chunk_start, const uint8_t* chunk_end, uint32_t index, DLS_Wave* wave) {
    wave->index = index;
    wave->sample.present = false;
    wave->pcm = NULL;
    wave->frames = 0;
    wave->factFrames = -1;
    
    if (chunk_start + 12 > chunk_end) return BAD_FILE_TYPE;
    uint32_t wave_chunk_size = read_le32(chunk_start + 4);
    const uint8_t* wave_end = chunk_start + 8 + wave_chunk_size;
    if (wave_end > chunk_end) return BAD_FILE_TYPE;
    const uint8_t* q = chunk_start + 12; // skip LIST <size> wave
    
    const uint8_t* pcm_data = NULL;
    uint32_t pcm_size = 0;

    while (q + 8 <= wave_end) {
        uint32_t id = read_chunk_id(q);
        uint32_t chunk_size = read_le32(q + 4);
        const uint8_t* body = q + 8;
        uint32_t padded_size = (chunk_size + 1) & ~1;
        if (body + padded_size > wave_end) break;

        if (id == CHUNK_FMT) {
            if (chunk_size >= 16) {
                wave->formatTag = read_le16(body);
                wave->channels = read_le16(body + 2);
                wave->sampleRate = read_le32(body + 4);
                wave->blockAlign = read_le16(body + 12);
                wave->bitsPerSample = read_le16(body + 14);
                if (wave->formatTag == 85) {
                    /* WAVE_FORMAT_MPEGLAYER3: require cbSize=12 and decode to 16-bit PCM. */
                    if (chunk_size < 30 || read_le16(body + 16) != 12) {
                        return BAD_FILE_TYPE;
                    }
                    wave->bitsPerSample = 16;
                }
            }
        } else if (id == CHUNK_DATA) {
            pcm_data = body;
            pcm_size = chunk_size;
        } else if (id == read_chunk_id((const uint8_t*)"fact")) {
            if (chunk_size >= 4) {
                wave->factFrames = read_le32(body);
            }
        } else if (id == CHUNK_WSMP) {
            if (chunk_size >= 20) {
                wave->sample.present = true;
                wave->sample.unityNote = read_le16(body + 4);
                wave->sample.fineTuneCents = (int16_t)read_le16(body + 6);
                wave->sample.attenuation = (int32_t)read_le32(body + 8);
                uint32_t cSampleLoops = read_le32(body + 16);
                wave->sample.loopMode = DLS_LOOP_NONE;
                wave->sample.loopStart = 0;
                wave->sample.loopEndInclusive = -1;
                wave->sample.loopUntilRelease = false;
                if (cSampleLoops > 0 && chunk_size >= 20 + cSampleLoops * 16) {
                    const uint8_t* loop = body + 20;
                    uint32_t loopType = read_le32(loop + 4);
                    uint32_t loopStart = read_le32(loop + 8);
                    uint32_t loopLength = read_le32(loop + 12);
                    if (loopLength != 0) {
                        wave->sample.loopStart = loopStart;
                        wave->sample.loopEndInclusive = (int32_t)(loopStart + loopLength - 1);
                        wave->sample.loopMode = DLS_LOOP_FORWARD;
                        wave->sample.loopUntilRelease = (loopType == 1);
                    }
                }
            }
        } else if (id == read_chunk_id((const uint8_t*)"smpl") && chunk_size >= 36) {
            uint32_t loopCount = read_le32(body + 28);
            wave->sample.present = true;
            wave->sample.unityNote = read_le32(body + 12) & 0xFF;
            if (loopCount > 0 && chunk_size >= 60) {
                uint32_t loopType = read_le32(body + 40);
                uint32_t loopStart = read_le32(body + 44);
                uint32_t loopEnd = read_le32(body + 48);
                if (loopEnd >= loopStart) {
                    wave->sample.loopMode = loopType == 0 ? DLS_LOOP_FORWARD : DLS_LOOP_NONE;
                    wave->sample.loopStart = loopStart;
                    wave->sample.loopEndInclusive = loopEnd;
                    wave->sample.loopUntilRelease = false;
                }
            }
        } else if (id == read_chunk_id((const uint8_t*)"inst") && chunk_size >= 7) {
            wave->sample.present = true;
            wave->sample.unityNote = body[0];
            wave->sample.fineTuneCents = (int8_t)body[1];
            wave->sample.attenuation = (int8_t)body[2] * 655360;
        }
        q = body + padded_size;
    }
    // Decode PCM data
    if (pcm_data && wave->channels > 0) {
        if (wave->formatTag == 1) { // PCM
            if (wave->bitsPerSample == 8) {
                wave->frames = pcm_size / wave->channels;
                wave->pcm = (int16_t*)XNewPtr((wave->frames + 1) * wave->channels * sizeof(int16_t));
                if (wave->pcm) {
                    for (uint32_t i = 0; i < wave->frames * wave->channels; i++) {
                        wave->pcm[i] = (int16_t)(((pcm_data[i] & 0xFF) - 128) << 8);
                    }
                }
            } else if (wave->bitsPerSample == 16) {
                wave->frames = pcm_size / (2 * wave->channels);
                wave->pcm = (int16_t*)XNewPtr((wave->frames + 1) * wave->channels * sizeof(int16_t));
                if (wave->pcm) {
                    for (uint32_t i = 0, p = 0; i < wave->frames * wave->channels; i++, p += 2) {
                        wave->pcm[i] = (int16_t)((pcm_data[p] & 0xFF) | (pcm_data[p + 1] << 8));
                    }
                }
            }
        } else if (wave->formatTag == 6 || wave->formatTag == 7) { // A-Law / u-Law
            wave->frames = pcm_size / wave->channels;
            wave->pcm = (int16_t*)XNewPtr((wave->frames + 1) * wave->channels * sizeof(int16_t));
            if (wave->pcm) {
                for (uint32_t i = 0; i < wave->frames * wave->channels; i++) {
                    wave->pcm[i] = (wave->formatTag == 6) ? decode_alaw(pcm_data[i]) : decode_ulaw(pcm_data[i]);
                }
            }
        } else if (wave->formatTag == 17) { // IMA ADPCM
            if (wave->blockAlign > 0 && wave->blockAlign % wave->channels == 0) {
                int32_t bytesPerChannel = wave->blockAlign / wave->channels;
                int32_t framesPerBlock = 2 * bytesPerChannel - 7;
                if (framesPerBlock > 0) {
                    int32_t dataBytesPerChannel = bytesPerChannel - 4;
                    int32_t blocks = pcm_size / wave->blockAlign;
                    uint32_t decodedFrames = blocks * framesPerBlock;
                    wave->frames = (wave->factFrames >= 0 && (uint32_t)wave->factFrames < decodedFrames) ? (uint32_t)wave->factFrames : decodedFrames;
                    
                    wave->pcm = (int16_t*)XNewPtr((wave->frames + 1) * wave->channels * sizeof(int16_t));
                    if (wave->pcm) {
                        for (int32_t block = 0; block < blocks; block++) {
                            int32_t blockBase = block * wave->blockAlign;
                            int32_t frameBase = block * framesPerBlock;
                            int32_t predictor[2] = {0};
                            int32_t index[2] = {0};
                            
                            for (int32_t ch = 0; ch < wave->channels; ch++) {
                                int32_t header = blockBase + ch * 4;
                                predictor[ch] = (int16_t)((pcm_data[header] & 0xFF) | (pcm_data[header + 1] << 8));
                                index[ch] = pcm_data[header + 2] & 0xFF;
                                if (index[ch] > 88) index[ch] = 88;
                                if ((uint32_t)frameBase < wave->frames) {
                                    wave->pcm[frameBase * wave->channels + ch] = (int16_t)predictor[ch];
                                }
                            }
                            
                            for (int32_t ch = 0; ch < wave->channels; ch++) {
                                int32_t sampleFrame = frameBase + 1;
                                for (int32_t e = 0; e < dataBytesPerChannel && sampleFrame < frameBase + framesPerBlock; e++) {
                                    int32_t src = blockBase + 4 * wave->channels + (e / 4) * wave->channels * 4 + ch * 4 + (e & 3);
                                    int32_t packed = pcm_data[src] & 0xFF;
                                    for (int32_t half = 0; half < 2 && sampleFrame < frameBase + framesPerBlock; half++) {
                                        int32_t nibble = (packed >> (half * 4)) & 0x0F;
                                        predictor[ch] = decode_ima_nibble(predictor[ch], index[ch], nibble);
                                        index[ch] = dls_clamp(index[ch] + IMA_INDEX_DELTA[nibble], 0, 88);
                                        if ((uint32_t)sampleFrame < wave->frames) {
                                            wave->pcm[sampleFrame * wave->channels + ch] = (int16_t)predictor[ch];
                                        }
                                        sampleFrame++;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else if (wave->formatTag == 85) { // MPEG Layer audio
            OPErr decodeErr = dls_decode_mpeg_wave(pcm_data, pcm_size, wave);
            if (decodeErr != NO_ERR) {
                return decodeErr;
            }
        }
    }

    return NO_ERR;
}

static void DLS_InitArticulation(DLS_Articulation* art) {
    XSetMemory(art, sizeof(*art), 0);
    art->lfoFrequency = 200000;
    art->lfoStartDelay = 10000;
    art->vibratoFrequency = 200000;
    art->vibratoStartDelay = 10000;
    art->eg1Delay = INT32_MIN;
    art->eg1Attack = INT32_MIN;
    art->eg1Hold = INT32_MIN;
    art->eg1Decay = INT32_MIN;
    art->eg1Sustain = 0x10000;
    art->eg1Release = INT32_MIN;
    art->eg1Shutdown = INT32_MIN;
    art->eg2Delay = INT32_MIN;
    art->eg2Attack = INT32_MIN;
    art->eg2Hold = INT32_MIN;
    art->eg2Decay = INT32_MIN;
    art->eg2Sustain = 0x10000;
    art->eg2Release = INT32_MIN;
    art->eg2Shutdown = INT32_MIN;
    art->filterCutoff = FILTER_DISABLED_CUTOFF;
}

static void dls_rebuild_articulation_for_mode(DLS_Articulation* art)
{
    DLS_Connection* savedConnections;
    uint16_t savedCount;

    if (!art) return;
    savedConnections = art->runtimeConnections;
    savedCount = art->connectionCount;
    DLS_InitArticulation(art);
    art->runtimeConnections = savedConnections;
    art->connectionCount = savedCount;
    for (uint16_t i = 0; i < savedCount; i++) {
        DLS_ApplyDirectConnection(art, &savedConnections[i], g_use_mobilebae_quirks);
    }
}

static void dls_refresh_current_synth_for_mode(void)
{
    GM_Mixer* mixer = GM_GetCurrentMixer();
    DLS_Synth* synth;
    bool savedQuirks;

    if (!mixer || !mixer->pDLSSynth) return;
    synth = mixer->pDLSSynth;
    savedQuirks = g_use_mobilebae_quirks;

    for (int bankIndex = 0; bankIndex < 2; bankIndex++) {
        DLS_Bank* bank = synth->banks[bankIndex];
        if (!bank) continue;
        g_use_mobilebae_quirks = (bankIndex == 1) ? true : savedQuirks;
        for (uint32_t i = 0; i < bank->instrumentCount; i++) {
            DLS_Instrument* instrument = &bank->instruments[i];
            dls_rebuild_articulation_for_mode(&instrument->articulation);
            for (uint32_t j = 0; j < instrument->regionCount; j++) {
                dls_rebuild_articulation_for_mode(&instrument->regions[j].articulation);
            }
        }
    }
    g_use_mobilebae_quirks = savedQuirks;

    for (int i = 0; i < 256; i++) {
        synth->voices[i].active = false;
    }
    for (int channel = 0; channel < 16; channel++) {
        synth->channels[channel].selectedInstrument = NULL;
        synth->channels[channel].selectedBankSelector = 0;
        synth->channels[channel].programSelected = false;
    }
}

static void DLS_ApplyDirectConnection(DLS_Articulation* art, const DLS_Connection* connection, bool quirks) {
    if (connection->source != 0) return;

    if (quirks) {
        switch (connection->destination) {
            case 3: art->pitch = connection->scale / 100; break;
            case 4:
                art->pan = connection->scale / 500;
                debug_message("DLS Articulation: direct pan source=%u control=%u transform=0x%04X scale=%d pan=%d\n",
                              connection->source, connection->control, connection->transform,
                              connection->scale, art->pan);
                break;
            case 0x80: art->chorus = connection->scale / 1000; break;
            case 0x81: art->reverb = connection->scale / 1000; break;
            case 0x104: art->lfoFrequency = dls_plus_lfo_period(connection->scale); break;
            case 0x105: art->lfoStartDelay = dls_timecent_to_micros(connection->scale); break;
            case 0x114: art->vibratoFrequency = dls_plus_lfo_period(connection->scale); break;
            case 0x115: art->vibratoStartDelay = dls_timecent_to_micros(connection->scale); break;
            case 0x20B: art->eg1Delay = connection->scale; break;
            case 0x206: art->eg1Attack = connection->scale; break;
            case 0x20C: art->eg1Hold = connection->scale; break;
            case 0x207: art->eg1Decay = connection->scale; break;
            case 0x209: art->eg1Release = connection->scale; break;
            case 0x20A: art->eg1Sustain = connection->scale / 1000; break;
            case 0x30F: art->eg2Delay = connection->scale; break;
            case 0x30A: art->eg2Attack = connection->scale; break;
            case 0x310: art->eg2Hold = connection->scale; break;
            case 0x30B: art->eg2Decay = connection->scale; break;
            case 0x30D: art->eg2Release = connection->scale; break;
            case 0x30E: art->eg2Sustain = connection->scale / 1000; break;
            case 0x500:
                art->filterCutoff = connection->scale == 0x7FFFFFFF ? FILTER_DISABLED_CUTOFF : connection->scale / 100;
                break;
            case 0x501: art->filterResonance = connection->scale / 10; break;
        }
    } else {
        switch (connection->destination) {
            case 3: art->pitch = connection->scale / 100; break;
            case 4: art->pan = connection->scale / 500; break;
            case 0x80: art->chorus = connection->scale / 1000; break;
            case 0x81: art->reverb = connection->scale / 1000; break;
            case 0x104: art->lfoFrequency = dls_plus_lfo_period(connection->scale); break;
            case 0x105: art->lfoStartDelay = dls_timecent_to_micros(connection->scale); break;
            case 0x114: art->vibratoFrequency = dls_plus_lfo_period(connection->scale); break;
            case 0x115: art->vibratoStartDelay = dls_timecent_to_micros(connection->scale); break;
            case 0x20B: art->eg1Delay = connection->scale; break;
            case 0x206: art->eg1Attack = connection->scale; break;
            case 0x20C: art->eg1Hold = connection->scale; break;
            case 0x207: art->eg1Decay = connection->scale; break;
            case 0x209: art->eg1Release = connection->scale; break;
            case 0x20A: art->eg1Sustain = connection->scale / 1000; break;
            case 0x20D: art->eg1Shutdown = connection->scale; break;
            case 0x30F: art->eg2Delay = connection->scale; break;
            case 0x30A: art->eg2Attack = connection->scale; break;
            case 0x310: art->eg2Hold = connection->scale; break;
            case 0x30B: art->eg2Decay = connection->scale; break;
            case 0x30D: art->eg2Release = connection->scale; break;
            case 0x30E: art->eg2Sustain = connection->scale / 1000; break;
            case 0x311: art->eg2Shutdown = connection->scale; break;
            case 0x500:
                art->filterCutoff = connection->scale == 0x7FFFFFFF ? FILTER_DISABLED_CUTOFF : connection->scale / 100;
                break;
            case 0x501: art->filterResonance = connection->scale / 10; break;
        }
    }
}

static OPErr DLS_Parse_ArticulationChunk(const uint8_t* body, uint32_t size, DLS_Articulation* art) {
    if (size < 8) return BAD_FILE_TYPE;
    uint32_t count = read_le32(body + 4);
    if (8 + count * 12 > size) return BAD_FILE_TYPE;
    
    if (count > 0) {
        uint32_t new_count = art->connectionCount + count;
        DLS_Connection* connections = (DLS_Connection*)XNewPtr(new_count * sizeof(DLS_Connection));
        if (!connections) return MEMORY_ERR;
        if (art->runtimeConnections && art->connectionCount > 0) {
            XBlockMove(art->runtimeConnections, connections, art->connectionCount * sizeof(DLS_Connection));
            XDisposePtr(art->runtimeConnections);
        }
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* p = body + 8 + i * 12;
            DLS_Connection* connection = &connections[art->connectionCount + i];
            connection->source = read_le16(p);
            connection->control = read_le16(p + 2);
            connection->destination = read_le16(p + 4);
            connection->transform = read_le16(p + 6);
            connection->scale = (int32_t)read_le32(p + 8);
            DLS_ApplyDirectConnection(art, connection, g_use_mobilebae_quirks);
        }
        art->runtimeConnections = connections;
        art->connectionCount = new_count;
    }
    return NO_ERR;
}

static OPErr DLS_Parse_ArticulationList(const uint8_t* start, const uint8_t* end, DLS_Articulation* art) {
    const uint8_t* p = start;
    while (p + 8 <= end) {
        uint32_t id = read_chunk_id(p);
        uint32_t size = read_le32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        
        if (id == CHUNK_ART1 || id == read_chunk_id((const uint8_t*)"art2")) {
            DLS_Parse_ArticulationChunk(body, size, art);
        }
        p += 8 + padded_size;
    }

    return NO_ERR;
}

static OPErr DLS_Parse_Region(const uint8_t* start, const uint8_t* end, bool level2, DLS_Region* region) {
    DLS_InitArticulation(&region->articulation);
    region->level2 = level2;
    region->keyLow = 0;
    region->keyHigh = 127;
    region->velocityLow = 0;
    region->velocityHigh = 127;
    region->tableIndex = -1;
    region->sample.loopMode = DLS_LOOP_NONE;
    region->sample.loopUntilRelease = false;
    
    const uint8_t* p = start;
    while (p + 8 <= end) {
        uint32_t id = read_chunk_id(p);
        uint32_t size = read_le32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        if (body + padded_size > end) break;
        
        if (id == CHUNK_RGNH && size >= 12) {
            region->keyLow = read_le16(body);
            region->keyHigh = read_le16(body + 2);
            region->velocityLow = read_le16(body + 4);
            region->velocityHigh = read_le16(body + 6);
            region->options = read_le16(body + 8);
            region->keyGroup = read_le16(body + 10);
        } else if (id == read_chunk_id((const uint8_t*)"wlnk") && size >= 12) {
            region->channel = read_le32(body + 4);
            region->tableIndex = read_le32(body + 8);
        } else if (id == CHUNK_WSMP) {
            if (size >= 20) {
                region->sample.present = true;
                region->sample.unityNote = read_le16(body + 4);
                region->sample.fineTuneCents = (int16_t)read_le16(body + 6);
                region->sample.attenuation = (int32_t)read_le32(body + 8);
                uint32_t cSampleLoops = read_le32(body + 16);
                region->sample.loopMode = DLS_LOOP_NONE;
                region->sample.loopStart = 0;
                region->sample.loopEndInclusive = -1;
                region->sample.loopUntilRelease = false;
                if (cSampleLoops > 0 && size >= 20 + cSampleLoops * 16) {
                    const uint8_t* loop = body + 20;
                    uint32_t loopType = read_le32(loop + 4);
                    uint32_t loopStart = read_le32(loop + 8);
                    uint32_t loopLength = read_le32(loop + 12);
                    if (loopLength != 0) {
                        region->sample.loopStart = loopStart;
                        region->sample.loopEndInclusive = (int32_t)(loopStart + loopLength - 1);
                        region->sample.loopMode = DLS_LOOP_FORWARD;
                        region->sample.loopUntilRelease = (loopType == 1);
                    }
                }
            }
        } else if (id == CHUNK_LIST && size >= 4 && (read_chunk_id(body) == CHUNK_LART || read_chunk_id(body) == read_chunk_id((const uint8_t*)"lar2"))) {
            region->ownsArticulation = true;
            DLS_Parse_ArticulationList(body + 4, body + size, &region->articulation);
        }
        
        p += 8 + padded_size;
    }
    return NO_ERR;
}

static OPErr DLS_Parse_Regions(const uint8_t* start, const uint8_t* end, DLS_Instrument* inst) {
    uint32_t index = 0;
    const uint8_t* p = start;
    while (p + 8 <= end && index < inst->regionCount) {
        uint32_t id = read_chunk_id(p);
        uint32_t size = read_le32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        if (body + padded_size > end) break;
        
        if (id == CHUNK_LIST && size >= 4) {
            uint32_t list_type = read_chunk_id(body);
            if (list_type == read_chunk_id((const uint8_t*)"rgn ") || list_type == read_chunk_id((const uint8_t*)"rgn2")) {
                DLS_Parse_Region(body + 4, body + size, list_type == read_chunk_id((const uint8_t*)"rgn2"), &inst->regions[index]);
                inst->regions[index].index = index;
                index++;
            }
        }
        p += 8 + padded_size;
    }
    return NO_ERR;
}

static bool DLS_InstrumentUsesRawSelector(DLS_Bank* bank, uint32_t rawBank) {
    int32_t rawLsb = rawBank & 0x7F;
    int32_t rawMsb = (rawBank >> 8) & 0x7F;

    if (bank->isDLSM || rawMsb == 120 || rawMsb == 121 || rawLsb != 0) {
        bank->selectorRawModeActive = true;
        return true;
    }
    if (bank->selectorRawModeActive) {
        return true;
    }
    bank->selectorImplicitModeSeen = true;
    return false;
}

static OPErr DLS_Parse_Instrument(const uint8_t* start, const uint8_t* end, DLS_Bank* bank, DLS_Instrument* inst) {
    uint32_t declaredRegions = 0;

    DLS_InitArticulation(&inst->articulation);
    inst->parentBank = bank;

    const uint8_t* p = start;
    while (p + 8 <= end) {
        uint32_t id = read_chunk_id(p);
        uint32_t size = read_le32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        if (body + padded_size > end) break;
        
        if (id == CHUNK_INSH && size >= 12) {
            declaredRegions = read_le32(body);
            inst->rawBank = read_le32(body + 4);
            inst->rawInstrument = read_le32(body + 8);
            inst->program = inst->rawInstrument & 0x7F;
            inst->drum = (inst->rawBank & 0x80000000) != 0;
            inst->rawMode = DLS_InstrumentUsesRawSelector(bank, inst->rawBank);
            inst->bankLsb = inst->rawMode ? (inst->rawBank & 0x7F) : ((inst->rawBank >> 8) & 0x7F);
            inst->bankMsb = inst->rawMode ? ((inst->rawBank >> 8) & 0x7F) : (inst->drum ? 120 : 121);
            
            debug_message("DLS Parser: loaded instrument bankMsb=%d bankLsb=%d program=%d drum=%d\n", inst->bankMsb, inst->bankLsb, inst->program, inst->drum);
            
            if (declaredRegions > 0) {
                inst->regionCount = declaredRegions;
                inst->regions = (DLS_Region*)XNewPtr(declaredRegions * sizeof(DLS_Region));
                if (inst->regions) {
                    XSetMemory(inst->regions, declaredRegions * sizeof(DLS_Region), 0);
                }
            }
        } else if (id == CHUNK_LIST && size >= 4 && read_chunk_id(body) == CHUNK_LRGN) {
            if (inst->regions) {
                DLS_Parse_Regions(body + 4, body + size, inst);
            }
        } else if (id == CHUNK_LIST && size >= 4 && (read_chunk_id(body) == CHUNK_LART || read_chunk_id(body) == read_chunk_id((const uint8_t*)"lar2"))) {
            DLS_Parse_ArticulationList(body + 4, body + size, &inst->articulation);
        }

        p += 8 + padded_size;
    }

    /* In DLS, a region without its own lart/lar2 list inherits the complete
       instrument articulation.  Deep-copy the connection list because each
       region has independent cleanup ownership. */
    for (uint32_t i = 0; i < inst->regionCount; i++) {
        DLS_Region* region = &inst->regions[i];
        if (!region->ownsArticulation) {
            region->articulation = inst->articulation;
            region->articulation.runtimeConnections = NULL;
            if (inst->articulation.connectionCount > 0) {
                uint32_t bytes = inst->articulation.connectionCount * sizeof(DLS_Connection);
                region->articulation.runtimeConnections = (DLS_Connection*)XNewPtr(bytes);
                if (!region->articulation.runtimeConnections) return MEMORY_ERR;
                XBlockMove(inst->articulation.runtimeConnections,
                           region->articulation.runtimeConnections, bytes);
            }
        }
    }
    return NO_ERR;
}

static OPErr DLS_Parse_Lins(DLS_ParserState* state, DLS_Bank* bank) {
    if (bank->declaredInstrumentCount == 0) return NO_ERR;
    
    uint32_t index = 0;
    const uint8_t* p = state->start;
    while (p + 8 <= state->end && index < bank->declaredInstrumentCount) {
        uint32_t id = read_chunk_id(p);
        uint32_t size = read_le32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        if (body + padded_size > state->end) break;
        
        if (id == CHUNK_LIST && size >= 4 && read_chunk_id(body) == CHUNK_INS) {
            DLS_Parse_Instrument(body + 4, body + size, bank, &bank->instruments[index]);
            index++;
        }
        
        p += 8 + padded_size;
    }
    return NO_ERR;
}

static uint16_t DLS_pgalLegacyBank(uint16_t bank) {
    return (bank & 0x7F) | 0x3C80;
}

static int32_t DLS_pgalBankMsb(uint16_t bank) {
    return ((uint32_t)bank >> 7) & 0x7F;  /* unsigned shift */
}

static int32_t DLS_pgalBankLsb(uint16_t bank) {
    return bank & 0x7F;
}

static OPErr DLS_Parse_Pgal(const uint8_t* body, uint32_t size, DLS_Bank* bank) {
    /* Detect PGAL version by checking marker at offset 0 */
    uint32_t versionMarker = size >= 4 ? read_le32(body) : (uint32_t)-1;
    uint32_t version = 0;
    uint32_t tableOffset = 0;
    uint32_t countOffset = 0;
    uint32_t recordOffset = 0;
    
    if (versionMarker == 1 || versionMarker == 2) {
        version = versionMarker;
        tableOffset = 4;
        countOffset = 132;
        recordOffset = 136;
    } else if (versionMarker == 0x03020100) {
        version = 0;
        tableOffset = 0;
        countOffset = 128;
        recordOffset = 132;
    } else {
        return NO_ERR;  /* Not a recognized PGAL format */
    }
    
    if (recordOffset > size) return NO_ERR;
    
    uint32_t count = read_le32(body + countOffset);
    if (count == 0 || recordOffset + count * 8 != size) return NO_ERR;
    
    /* Parse percussion key aliases table (128 bytes at tableOffset) */
    for (int i = 0; i < 128; i++) {
        bank->percussionKeyAliases[i] = body[tableOffset + i] & 0x7F;
    }
    bank->hasPercussionKeyAliases = true;
    
    /* Parse program aliases */
    bank->programAliases = (DLS_ProgramAlias*)XNewPtr((int32_t)(count * sizeof(DLS_ProgramAlias)));
    if (!bank->programAliases) return MEMORY_ERR;
    bank->programAliasCount = count;

    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* p = body + recordOffset + i * 8;
        uint16_t fromBank = read_le16(p);
        uint16_t fromProgram = read_le16(p + 2) & 0x7F;
        uint16_t toBank = read_le16(p + 4);
        uint16_t toProgram = read_le16(p + 6) & 0x7F;
        
        /* Apply legacy bank conversion for old PGAL formats */
        if (version < 2) {
            fromBank = DLS_pgalLegacyBank(fromBank);
            toBank = DLS_pgalLegacyBank(toBank);
        }
        
        bank->programAliases[i].fromSelector = DLS_Selector(DLS_pgalBankMsb(fromBank), 
                                                              DLS_pgalBankLsb(fromBank), 
                                                              fromProgram);
        bank->programAliases[i].toSelector = DLS_Selector(DLS_pgalBankMsb(toBank), 
                                                            DLS_pgalBankLsb(toBank), 
                                                            toProgram);
    }
    return NO_ERR;
}

static OPErr DLS_Parse_Wvpl(DLS_ParserState* state, DLS_Bank* bank, uint32_t* poolOffsets, uint32_t poolOffsetCount) {
    if (poolOffsetCount > 0) {
        bank->waveCount = poolOffsetCount;
        bank->waves = (DLS_Wave*)XNewPtr(bank->waveCount * sizeof(DLS_Wave));
        if (!bank->waves) return MEMORY_ERR;
        XSetMemory(bank->waves, bank->waveCount * sizeof(DLS_Wave), 0);

        const uint8_t* baseA = state->start;
        const uint8_t* baseB = state->start - 4; // Because chunk_data points to after "LIST" but baseB might be the start of "LIST"
        
        for (uint32_t i = 0; i < poolOffsetCount; i++) {
            const uint8_t* p = baseA + poolOffsets[i];
            if (!looks_like_wave_chunk(p, state->end)) {
                p = baseB + poolOffsets[i];
            }
            if (looks_like_wave_chunk(p, state->end)) {
                DLS_Parse_Wave_Data(p, state->end, i, &bank->waves[i]);
            }
        }
    } else {
        // count chunks
        uint32_t count = 0;
        const uint8_t* p = state->start;
        while (p + 8 <= state->end) {
            if (looks_like_wave_chunk(p, state->end)) count++;
            uint32_t chunk_size = read_le32(p + 4);
            p += 8 + ((chunk_size + 1) & ~1);
        }
        
        if (count > 0) {
            bank->waveCount = count;
            bank->waves = (DLS_Wave*)XNewPtr(bank->waveCount * sizeof(DLS_Wave));
            if (!bank->waves) return MEMORY_ERR;
            XSetMemory(bank->waves, bank->waveCount * sizeof(DLS_Wave), 0);

            p = state->start;
            uint32_t index = 0;
            while (p + 8 <= state->end) {
                if (looks_like_wave_chunk(p, state->end)) {
                    DLS_Parse_Wave_Data(p, state->end, index, &bank->waves[index]);
                    index++;
                }
                uint32_t chunk_size = read_le32(p + 4);
                p += 8 + ((chunk_size + 1) & ~1);
            }
        }
    }
    return NO_ERR;
}

OPErr GM_LoadDLSBankFromMemory(void* pMemory, uint32_t memorySize, DLS_Bank** ppBank) {
    if (!pMemory || !ppBank) return PARAM_ERR;

    *ppBank = (DLS_Bank*)XNewPtr((int32_t)sizeof(DLS_Bank));
    if (!*ppBank) return MEMORY_ERR;

    XSetMemory(*ppBank, (int32_t)sizeof(DLS_Bank), 0);
    DLS_Bank* bank = *ppBank;

    DLS_ParserState root_state;
    root_state.start = (const uint8_t*)pMemory;
    root_state.pos = root_state.start;
    root_state.end = root_state.start + memorySize;

    // Minimum RIFF header size
    if (memorySize < 12) {
        GM_UnloadDLSBank(bank);
        *ppBank = NULL;
        return BAD_FILE_TYPE;
    }

    uint32_t riff_id = read_chunk_id(root_state.pos);
    if (riff_id != CHUNK_RIFF) {
        GM_UnloadDLSBank(bank);
        *ppBank = NULL;
        return BAD_FILE_TYPE;
    }
    
    // uint32_t riff_size = read_le32(root_state.pos + 4);
    uint32_t dls_id = read_chunk_id(root_state.pos + 8);
    if (dls_id != CHUNK_DLS && dls_id != read_chunk_id((const uint8_t*)"DLSM")) {
        GM_UnloadDLSBank(bank);
        *ppBank = NULL;
        return BAD_FILE_TYPE;
    }
    bank->isDLSM = dls_id == read_chunk_id((const uint8_t*)"DLSM");

    root_state.pos += 12;

    DLS_ParserState lins_state = {NULL, NULL, NULL};
    DLS_ParserState wvpl_state = {NULL, NULL, NULL};
    DLS_ParserState ptbl_state = {NULL, NULL, NULL};
    DLS_ParserState pgal_state = {NULL, NULL, NULL};

    while (root_state.pos + 8 <= root_state.end) {
        uint32_t chunk_id = read_chunk_id(root_state.pos);
        uint32_t chunk_size = read_le32(root_state.pos + 4);
        const uint8_t* chunk_data = root_state.pos + 8;
        
        // chunk sizes are padded to even length
        uint32_t padded_size = (chunk_size + 1) & ~1;
        if (chunk_data + padded_size > root_state.end) break;
        
        if (chunk_id == CHUNK_LIST) {
            uint32_t list_type = read_chunk_id(chunk_data);
            if (list_type == CHUNK_LINS) {
                lins_state.start = chunk_data + 4;
                lins_state.pos = chunk_data + 4;
                lins_state.end = chunk_data + chunk_size;
            } else if (list_type == CHUNK_WVPL) {
                wvpl_state.start = chunk_data + 4;
                wvpl_state.pos = chunk_data + 4;
                wvpl_state.end = chunk_data + chunk_size;
            }
        } else if (chunk_id == CHUNK_PTBL) {
            ptbl_state.start = chunk_data;
            ptbl_state.pos = chunk_data;
            ptbl_state.end = chunk_data + chunk_size;
        } else if (chunk_id == read_chunk_id((const uint8_t*)"pgal")) {
            pgal_state.start = chunk_data;
            pgal_state.pos = chunk_data;
            pgal_state.end = chunk_data + chunk_size;
        } else if (chunk_id == read_chunk_id((const uint8_t*)"colh")) {
            bank->declaredInstrumentCount = read_le32(chunk_data);
            if (bank->declaredInstrumentCount > 0) {
                bank->instruments = (DLS_Instrument*)XNewPtr((int32_t)(bank->declaredInstrumentCount * sizeof(DLS_Instrument)));
                if (bank->instruments) {
                    XSetMemory(bank->instruments, (int32_t)(bank->declaredInstrumentCount * sizeof(DLS_Instrument)), 0);
                }
            }
        }
        
        root_state.pos = chunk_data + padded_size;
    }

    uint32_t poolOffsetCount = 0;
    uint32_t* poolOffsets = NULL;

    if (ptbl_state.start && ptbl_state.end - ptbl_state.start >= 8) {
        poolOffsetCount = read_le32(ptbl_state.start + 4);
        if (poolOffsetCount * 4 + 8 <= (uint32_t)(ptbl_state.end - ptbl_state.start)) {
            poolOffsets = (uint32_t*)XNewPtr(poolOffsetCount * sizeof(uint32_t));
            if (poolOffsets) {
                for(uint32_t i=0; i<poolOffsetCount; i++) {
                    poolOffsets[i] = read_le32(ptbl_state.start + 8 + i*4);
                }
            }
        }
    }
    
    if (lins_state.start) {
        DLS_Parse_Lins(&lins_state, bank);
    }
    bank->instrumentCount = bank->declaredInstrumentCount;
    if (pgal_state.start) {
        OPErr pgalErr = DLS_Parse_Pgal(pgal_state.start, (uint32_t)(pgal_state.end - pgal_state.start), bank);
        if (pgalErr != NO_ERR) {
            GM_UnloadDLSBank(bank);
            *ppBank = NULL;
            return pgalErr;
        }
    }
    if (wvpl_state.start) {
        DLS_Parse_Wvpl(&wvpl_state, bank, poolOffsets, poolOffsetCount);
    }

    if (poolOffsets) {
        XDisposePtr(poolOffsets);
    }

    debug_message("DLS Parser: instruments=%u waves=%u ptbl=%u pgal=%u\n",
                  bank->instrumentCount, bank->waveCount, poolOffsetCount,
                  bank->programAliasCount);

    return NO_ERR;
}

OPErr GM_LoadDLSFromMemory(struct GM_Mixer* pMixer, const void* pMemory, uint32_t memorySize) {
    DLS_Bank* bank = NULL;
    OPErr err;

    if (!pMixer || !pMemory || memorySize == 0) {
        return PARAM_ERR;
    }

    err = GM_LoadDLSBankFromMemory((void*)pMemory, memorySize, &bank);
    if (err != NO_ERR) {
        return err;
    }

    if (!pMixer->pDLSSynth) {
        err = GM_InitDLSSynth(&pMixer->pDLSSynth, pMixer->outputRate);
        if (err != NO_ERR) {
            GM_UnloadDLSBank(bank);
            return err;
        }
    }

    if (pMixer->pDLSSynth->banks[0]) {
        GM_UnloadDLSBank(pMixer->pDLSSynth->banks[0]);
    }
    if (pMixer->pDLSSynth->banks[1]) {
        GM_UnloadDLSBank(pMixer->pDLSSynth->banks[1]);
        pMixer->pDLSSynth->banks[1] = NULL;
    }

    pMixer->pDLSSynth->banks[0] = bank;

    /* Bank switch invalidates all cached channel instrument bindings and active voices. */
    for (int i = 0; i < 256; i++) {
        pMixer->pDLSSynth->voices[i].active = false;
    }
    for (int ch = 0; ch < 16; ch++) {
        DLS_ChannelState* channelState = &pMixer->pDLSSynth->channels[ch];
        XSetMemory(channelState, sizeof(*channelState), 0);
        channelState->channel = ch;
        channelState->bankMsb = (ch == 9) ? 120 : 121;
        channelState->bankLsb = 0;
        channelState->program = 0;
        channelState->volume = 100;
        channelState->expression = 127;
        channelState->pan = 64;
        channelState->reverb = 40;
        channelState->pitchBend = 0x2000;
        channelState->rpnValues[0] = 0x0100;
        channelState->rpnValues[1] = 0x2000;
        channelState->rpnValues[2] = 0x2000;
        channelState->rpnMsb = 127;
        channelState->rpnLsb = 127;
        channelState->nrpnMsb = 127;
        channelState->nrpnLsb = 127;
        channelState->selectedInstrument = NULL;
        channelState->selectedBankSelector = 0;
        channelState->programSelected = false;
    }

    pMixer->isDLS = true;
    return NO_ERR;
}

OPErr GM_LoadDLSAsXMFOverlayFromMemory(struct GM_Mixer* pMixer, const void* pMemory, uint32_t memorySize)
{
    DLS_Bank* bank = NULL;
    OPErr err;
    bool savedQuirks;

    if (!pMixer || !pMemory || memorySize == 0) {
        return PARAM_ERR;
    }

    savedQuirks = g_use_mobilebae_quirks;
    g_use_mobilebae_quirks = true;
    err = GM_LoadDLSBankFromMemory((void*)pMemory, memorySize, &bank);
    g_use_mobilebae_quirks = savedQuirks;
    if (err != NO_ERR) {
        return err;
    }
    bank->forceQuirks = true;

    if (!pMixer->pDLSSynth) {
        err = GM_InitDLSSynth(&pMixer->pDLSSynth, pMixer->outputRate);
        if (err != NO_ERR) {
            GM_UnloadDLSBank(bank);
            return err;
        }
    }

    if (pMixer->pDLSSynth->banks[1]) {
        GM_UnloadDLSBank(pMixer->pDLSSynth->banks[1]);
        pMixer->pDLSSynth->banks[1] = NULL;
    }

    pMixer->pDLSSynth->banks[1] = bank;

    /* Overlay changes program resolution priority; force channel/program cache rebuild. */
    for (int i = 0; i < 256; i++) {
        pMixer->pDLSSynth->voices[i].active = false;
    }
    for (int ch = 0; ch < 16; ch++) {
        DLS_ChannelState* channelState = &pMixer->pDLSSynth->channels[ch];
        channelState->selectedInstrument = NULL;
        channelState->selectedBankSelector = 0;
        channelState->programSelected = false;
    }

    pMixer->isDLS = true;
    return NO_ERR;
}

void GM_UnloadXMFDLSOverlay(struct GM_Mixer* pMixer)
{
    if (!pMixer || !pMixer->pDLSSynth) {
        return;
    }

    if (pMixer->pDLSSynth->banks[1]) {
        GM_UnloadDLSBank(pMixer->pDLSSynth->banks[1]);
        pMixer->pDLSSynth->banks[1] = NULL;
    }

    for (int i = 0; i < 256; i++) {
        pMixer->pDLSSynth->voices[i].active = false;
    }
    for (int ch = 0; ch < 16; ch++) {
        DLS_ChannelState* channelState = &pMixer->pDLSSynth->channels[ch];
        channelState->selectedInstrument = NULL;
        channelState->selectedBankSelector = 0;
        channelState->programSelected = false;
    }

    if (!pMixer->pDLSSynth->banks[0]) {
        pMixer->isDLS = false;
    }
}

void GM_UnloadDLSBank(DLS_Bank* pBank) {
    if (!pBank) return;
    
    if (pBank->instruments) {
        for (uint32_t i = 0; i < pBank->declaredInstrumentCount; i++) {
            DLS_Instrument* inst = &pBank->instruments[i];
            if (inst->regions) {
                for (uint32_t j = 0; j < inst->regionCount; j++) {
                    if (inst->regions[j].articulation.runtimeConnections) {
                        XDisposePtr(inst->regions[j].articulation.runtimeConnections);
                    }
                }
                XDisposePtr(inst->regions);
            }
            if (inst->articulation.runtimeConnections) {
                XDisposePtr(inst->articulation.runtimeConnections);
            }
        }
        XDisposePtr(pBank->instruments);
    }
    
    if (pBank->waves) {
        for (uint32_t i = 0; i < pBank->waveCount; i++) {
            if (pBank->waves[i].pcm) XDisposePtr(pBank->waves[i].pcm);
        }
        XDisposePtr(pBank->waves);
    }
    if (pBank->programAliases) XDisposePtr(pBank->programAliases);
    
    XDisposePtr(pBank);
}

// ============================================================================
// SYNTHESIZER
// ============================================================================

static void dls_channel_reset_controllers(DLS_ChannelState* ch);

OPErr GM_InitDLSSynth(DLS_Synth** ppSynth, int32_t sampleRate) {
    if (!ppSynth) return PARAM_ERR;
    *ppSynth = (DLS_Synth*)XNewPtr(sizeof(DLS_Synth));
    if (!*ppSynth) return MEMORY_ERR;
    XSetMemory(*ppSynth, sizeof(DLS_Synth), 0);
    (*ppSynth)->sampleRate = sampleRate;
    for (int32_t channel = 0; channel < 16; channel++) {
        (*ppSynth)->channels[channel].channel = channel;
        (*ppSynth)->channels[channel].bankMsb = channel == 9 ? 120 : 121;
        (*ppSynth)->channels[channel].bankLsb = 0;
        dls_channel_reset_controllers(&(*ppSynth)->channels[channel]);
    }
    return NO_ERR;
}

bool GM_IsDLSSong(GM_Song* pSong) {
    if (pSong) {
        return pSong->isDLSSong;
    }
    return false;
}

bool GM_DLS_HasXmfEmbeddedBank(struct GM_Mixer* pMixer)
{
    if (!pMixer || !pMixer->pDLSSynth) {
        return false;
    }
    return (((DLS_Synth*)pMixer->pDLSSynth)->banks[1] != NULL);
}

uint16_t GM_DLS_GetActiveVoiceCount(struct GM_Mixer* pMixer) {
    if (!pMixer || !pMixer->pDLSSynth) return 0;
    DLS_Synth* synth = (DLS_Synth*)pMixer->pDLSSynth;
    uint16_t active = 0;
    for (int i = 0; i < 256; i++) {
        if (synth->voices[i].active) {
            active++;
        }
    }
    return active;
}

GM_Instrument* GM_DLS_CreateInstrumentStub(int32_t instrument) {
    GM_Instrument* inst = (GM_Instrument*)XNewPtr((int32_t)sizeof(GM_Instrument));
    if (inst) {
        inst->usageReferenceCount = 0;
    }
    return inst;
}

void GM_FinisDLSSynth(DLS_Synth* pSynth) {
    if (pSynth) {
        XDisposePtr(pSynth);
    }
}

void GM_DLS_ResetForSong(GM_Song* pSong)
{
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth)
    {
        return;
    }

    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;

    for (int i = 0; i < 256; i++)
    {
        synth->voices[i].active = false;
    }

    for (int ch = 0; ch < 16; ch++)
    {
        DLS_ChannelState* channelState = &synth->channels[ch];
        XSetMemory(channelState, sizeof(*channelState), 0);
        channelState->channel = ch;
        channelState->bankMsb = (ch == 9) ? 120 : 121;
        channelState->bankLsb = 0;
        channelState->program = 0;
        dls_channel_reset_controllers(channelState);
        channelState->selectedInstrument = NULL;
        channelState->selectedBankSelector = 0;
        channelState->programSelected = false;
    }

    synth->nextVoiceSerial = 0;
}

static DLS_Instrument* DLS_Bank_FindSelector(DLS_Bank* bank, uint32_t selector);

/* SP-MIDI 1.0b Figure 6: each entry lists all program numbers in one
   instrument group, terminated by -1.  When searching for a group match,
   every member is tried in priority order (first = canonical target). */
static const int8_t g_spmidi_melodic_groups[][18] = {
    {0,1,2,3,4,5,6,7,-1},           /* Piano → 0 Acoustic Grand */
    {8,9,10,11,13,14,15,16,46,47,99,109,-1}, /* Chromatic Perc → 12 Vibraphone */
    {17,18,19,20,21,22,23,24,110,-1}, /* Organ → 17 Drawbar */
    {25,26,27,28,29,30,31,32,105,106,107,108,-1}, /* Guitar → 28 Clean */
    {33,34,35,36,37,38,39,40,-1},     /* Bass → 34 Finger */
    {41,42,43,44,111,-1},             /* Strings → 41 Violin */
    {45,49,50,51,52,-1},              /* Ensemble → 49 String Ens1 */
    {48,56,113,114,115,116,117,118,119,-1}, /* Percussive → 115 SteelDrums */
    {53,54,55,89,91,92,93,94,95,96,97,98,100,101,102,103,104,-1}, /* Pad/Synth → 90 WarmPad */
    {57,58,59,60,61,62,63,64,-1},     /* Brass → 57 Trumpet */
    {65,66,67,68,69,70,71,72,112,-1}, /* Reed → 67 TenorSax */
    {73,74,75,76,77,78,79,80,-1},     /* Pipe → 74 Flute */
    {81,82,83,84,85,86,87,88,-1},     /* Synth Lead → 82 Saw */
    {-1}                              /* sentinel */
};

static const int8_t g_spmidi_percussion_groups[][16] = {
    {36,35,-1},       /* Bass Drum → 36 */
    {40,38,39,-1},    /* Snare → 40 */
    {42,44,71,80,-1}, /* Closed HH → 42 */
    {46,55,-1},       /* Open HH → 46 */
    {49,57,-1},       /* Crash → 49 */
    {50,47,48,-1},    /* High Tom → 50 */
    {45,41,43,-1},    /* Low Tom → 45 */
    {51,52,53,59,-1}, /* Ride → 51 */
    {54,-1},          /* Tambourine → 54 */
    {62,60,65,78,-1}, /* Mute Hi Conga → 62 */
    {64,61,63,66,79,-1}, /* Low Conga → 64 */
    {70,69,-1},       /* Maracas → 70 */
    {75,37,56,-1},    /* Claves → 75 */
    {67,68,-1},       /* Agogo → 67 */
    {72,-1},          /* Long Whistle → 72 */
    {73,-1},          /* Short Guiro → 73 */
    {74,-1},          /* Long Guiro → 74 */
    {76,77,-1},       /* Wood Block → 76 */
    {81,-1},          /* Open Triangle → 81 */
    {-1}              /* sentinel */
};

/* SP-MIDI 1.0b Figure 7: percussion key remap.  Each group's first
   member is the minimum-sound target that replaces all later members.
   Returns the input key unchanged if no remap group is found. */
static int32_t DLS_SPMIDI_RemapPercussionKey(int32_t key) {
    if (key < 24 || key > 127) return key;
    for (int32_t g = 0; g_spmidi_percussion_groups[g][0] != -1; g++) {
        for (int32_t m = 1; g_spmidi_percussion_groups[g][m] != -1; m++) {
            if (g_spmidi_percussion_groups[g][m] == key) {
                return g_spmidi_percussion_groups[g][0];
            }
        }
    }
    return key;
}

/* Search each SP-MIDI group for the requested program; if found, iterate
   the group's members and return the first program that exists in the bank. */
static int32_t DLS_SPMIDI_RemapProgram(DLS_Bank* bank, int32_t bankMsb, int32_t program) {
    DLS_Instrument* inst;
    uint32_t selector;
    int32_t candidate;

    if (bankMsb == 121) {
        if (program < 0 || program > 127) return program;
        for (int32_t g = 0; g_spmidi_melodic_groups[g][0] != -1; g++) {
            bool inGroup = false;
            for (int32_t m = 0; g_spmidi_melodic_groups[g][m] != -1; m++) {
                if (g_spmidi_melodic_groups[g][m] == program) { inGroup = true; break; }
            }
            if (!inGroup) continue;
            for (int32_t m = 0; g_spmidi_melodic_groups[g][m] != -1; m++) {
                candidate = g_spmidi_melodic_groups[g][m];
                selector = DLS_Selector(121, 0, candidate);
                inst = DLS_Bank_FindSelector(bank, selector);
                if (inst) return candidate;
            }
            return program;
        }
        return program;
    }

    if (bankMsb == 120) {
        if (program < 24 || program > 127) return program;
        for (int32_t g = 0; g_spmidi_percussion_groups[g][0] != -1; g++) {
            bool inGroup = false;
            for (int32_t m = 0; g_spmidi_percussion_groups[g][m] != -1; m++) {
                if (g_spmidi_percussion_groups[g][m] == program) { inGroup = true; break; }
            }
            if (!inGroup) continue;
            for (int32_t m = 0; g_spmidi_percussion_groups[g][m] != -1; m++) {
                candidate = g_spmidi_percussion_groups[g][m];
                selector = DLS_Selector(120, 0, candidate);
                inst = DLS_Bank_FindSelector(bank, selector);
                if (inst) return candidate;
            }
            return program;
        }
        return program;
    }

    return program;
}

static DLS_Instrument* DLS_Bank_FindSelector(DLS_Bank* bank, uint32_t selector) {
    for (uint32_t i = 0; i < bank->instrumentCount; i++) {
        DLS_Instrument* inst = &bank->instruments[i];
        if (DLS_Selector(inst->bankMsb, inst->bankLsb, inst->program) == selector) return inst;
    }
    return NULL;
}

static DLS_Instrument* DLS_Bank_FindAlias(DLS_Bank* bank, uint32_t selector) {
    for (uint32_t i = 0; i < bank->programAliasCount; i++) {
        if (bank->programAliases[i].fromSelector == selector) {
            return DLS_Bank_FindSelector(bank, bank->programAliases[i].toSelector);
        }
    }
    return NULL;
}

static DLS_Instrument* DLS_Bank_FindSelectorOrAlias(DLS_Bank* bank, uint32_t selector)
{
    DLS_Instrument* inst = DLS_Bank_FindSelector(bank, selector);
    if (inst) {
        return inst;
    }
    return DLS_Bank_FindAlias(bank, selector);
}

bool GM_DLS_XmfOverlayHasBankProgram(struct GM_Mixer* pMixer, int32_t bankMsb, int32_t bankLsb, int32_t program)
{
    if (!pMixer || !pMixer->pDLSSynth) {
        return false;
    }
    DLS_Synth* synth = (DLS_Synth*)pMixer->pDLSSynth;
    DLS_Bank* bank = synth->banks[1];
    if (!bank) {
        return false;
    }
    uint32_t selector = DLS_Selector(bankMsb & 0x7F, bankLsb & 0x7F, program & 0x7F);
    return DLS_Bank_FindSelectorOrAlias(bank, selector) != NULL;
}


static DLS_Instrument* DLS_Bank_FindMidiInstrument(DLS_Bank* bank, int32_t bankId, int32_t program,
                                                   bool allowDrumProgramFallback) {
    int32_t bankMsb = (bankId >> 7) & 0x7F;
    int32_t bankLsb = bankId & 0x7F;
    int32_t rawBank;
    uint32_t selector = DLS_Selector(bankMsb, bankLsb, program);
    DLS_Instrument* inst = DLS_Bank_FindSelectorOrAlias(bank, selector);
    if (inst) return inst;

    /* SF2-style conservative aliasing: translate selector topology explicitly. */

    /* GM-style MSB 0 often maps to melodic family 121 in DLS banks. */
    if (bankMsb == 0) {
        selector = DLS_Selector(121, bankLsb, program);
        inst = DLS_Bank_FindSelectorOrAlias(bank, selector);
        if (inst) return inst;

        if (bankLsb > 0) {
            selector = DLS_Selector(121, 0, program);
            inst = DLS_Bank_FindSelectorOrAlias(bank, selector);
            if (inst) return inst;
        }
    }

    /* HSB-family to raw selector conversion for banks authored outside 120/121 layout. */
    if (bankMsb == 120 || bankMsb == 121) {
        int32_t altRawBank;
        DLS_Instrument* altInst = NULL;
        bool wantDrum = (bankMsb == 120) ? true : false;

        rawBank = (bankLsb << 1) | ((bankMsb == 120) ? 1 : 0);
        selector = DLS_Selector((rawBank >> 7) & 0x7F, rawBank & 0x7F, program);
        inst = DLS_Bank_FindSelectorOrAlias(bank, selector);

        /* Probe sibling topology (parity-swapped raw bank). Some DLS sets encode
           120/121 family opposite to our default parity mapping. */
        altRawBank = rawBank ^ 1;
        selector = DLS_Selector((altRawBank >> 7) & 0x7F, altRawBank & 0x7F, program);
        altInst = DLS_Bank_FindSelectorOrAlias(bank, selector);

        if (wantDrum) {
            int32_t fallbackProgram;

            if (inst && inst->drum) return inst;
            if (altInst && altInst->drum) return altInst;

            if (!allowDrumProgramFallback) return NULL;

            for (fallbackProgram = 0; fallbackProgram < 128; fallbackProgram++) {
                selector = DLS_Selector(bankMsb, bankLsb, fallbackProgram);
                inst = DLS_Bank_FindSelectorOrAlias(bank, selector);
                if (inst) return inst;

                selector = DLS_Selector((rawBank >> 7) & 0x7F, rawBank & 0x7F, fallbackProgram);
                inst = DLS_Bank_FindSelectorOrAlias(bank, selector);
                if (inst && inst->drum) return inst;

                selector = DLS_Selector((altRawBank >> 7) & 0x7F, altRawBank & 0x7F, fallbackProgram);
                altInst = DLS_Bank_FindSelectorOrAlias(bank, selector);
                if (altInst && altInst->drum) return altInst;
            }

            /* Do not substitute melodic instruments for percussion requests.
               Fall through to SP-MIDI remap below as last resort. */
        } else {
            if (inst && !inst->drum) return inst;
            if (altInst && !altInst->drum) return altInst;
            if (inst) return inst;
            if (altInst) return altInst;
            /* Fall through to SP-MIDI remap below as last resort. */
        }
    }

    /* Non-quirks (SP-MIDI compliant): remap missing instruments per
       SP-MIDI 1.0b Figures 6-7 when all other lookup strategies have
       been exhausted for the 120/121 bank families.  Only remap when
       no instrument with the requested program exists in the bank at
       any selector — otherwise the bank has the instrument and a
       different encoding strategy should be found upstream.      */
    if (dls_bank_spmidi(bank)) {
        bool bankHasProgram = false;
        for (uint32_t i = 0; i < bank->instrumentCount; i++) {
            DLS_Instrument* chk = &bank->instruments[i];
            if (chk->program == program && chk->bankMsb == bankMsb) {
                bankHasProgram = true;
                break;
            }
        }
        if (!bankHasProgram) {
            int32_t spRemap = DLS_SPMIDI_RemapProgram(bank, bankMsb, program);
            if (spRemap != program) {
                selector = DLS_Selector(bankMsb, bankLsb, spRemap);
                inst = DLS_Bank_FindSelector(bank, selector);
                if (inst) {
                    debug_message("DLS Synth: SP-MIDI remap program %d -> %d\n", program, spRemap);
                    return inst;
                }
            }
        }
    }

    if ((bankId & 0x3F80) == (121 << 7)) {
        if (bankLsb == 0) return NULL;
        selector = DLS_Selector(121, 0, program);
        inst = DLS_Bank_FindSelectorOrAlias(bank, selector);
        if (inst) return inst;
    }

    if (bankMsb == 120 && bankLsb == 0 && (program & 0x7F) == 0) {
        return NULL;
    }

    return NULL;
}

static DLS_Instrument* DLS_Synth_FindInstrument(DLS_Synth* synth, int32_t bankId, int32_t program) {
    DLS_Instrument* inst = NULL;

    if (synth->banks[1]) {
        inst = DLS_Bank_FindMidiInstrument(synth->banks[1], bankId, program, false);
        if (inst) {
            return inst;
        }
    }

    if (synth->banks[0]) {
        inst = DLS_Bank_FindMidiInstrument(synth->banks[0], bankId, program, false);
        if (inst) {
            return inst;
        }
    }

    if (synth->banks[1]) {
        inst = DLS_Bank_FindMidiInstrument(synth->banks[1], bankId, program, true);
        if (inst) {
            return inst;
        }
    }

    if (synth->banks[0]) {
        return DLS_Bank_FindMidiInstrument(synth->banks[0], bankId, program, true);
    }

    return NULL;
}

static DLS_Region* DLS_Synth_FindRegion(DLS_Instrument* inst, int32_t key, int32_t velocity) {
    for (uint32_t i = 0; i < inst->regionCount; i++) {
        DLS_Region* region = &inst->regions[i];
        if (key >= region->keyLow && key <= region->keyHigh &&
            velocity >= region->velocityLow && velocity <= region->velocityHigh) {
            return region;
        }
    }
    return NULL;
}

/* The DLS Level 1 defaults used by RetroDLS Articulation.addDefaultConnections(). */
static const DLS_Connection g_dls_default_connections[] = {
    { 3,    0,     3, 0x0000,  838860800 },
    { 2,    0,     1, 0x8400,  -31457280 },
    { 6, 0x100,     3, 0x4000,  838860800 },
    { 0x87, 0,      1, 0x8400,  -62914560 },
    { 0x8B, 0,      1, 0x8400,  -62914560 },
    { 0x101, 0,     3, 0x4000,   6553600 },
    { 0x8A, 0,      4, 0x4000,  33292288 },
    { 0xDB, 0,   0x81, 0x0000,  65536000 },
    { 0xDD, 0,   0x80, 0x0000,  65536000 }
};

/* DLS v2.2 default connections per MMA RP-025/Amd2 Tables 5-6.
   These replace the Level 1 defaults when MobileBAE quirks are disabled.
   Only connections identical in function to the DLS1 defaults are included,
   matching the same (source, control, destination) topology but with
   DLS v2.2-correct default scale values for CC10 pan (50.8%) and
   DLS v2.2 additional destinations (EG delay, hold, shutdown). */
static const DLS_Connection g_dlsv2_default_connections[] = {
    /* Key Number to Pitch: 12800 cents (same as DLS1) */
    { 3,    0,     3, 0x0000,  838860800 },
    /* Velocity to Gain: Concave, Inverted, -48 dB (matches quirks behavior;
       DLS2 spec says -96 dB but -48 dB prevents drums from vanishing at
       sub-127 velocities with DLS banks authored for this engine.) */
    { 2,    0,     1, 0x8400,  -31457280 },
    /* Pitch Wheel + RPN0 to Pitch: 12800 cents (same as DLS1) */
    { 6, 0x100,     3, 0x4000,  838860800 },
    /* CC7 to Gain: Concave, Inverted, -96 dB (same as DLS1) */
    { 0x87, 0,      1, 0x8400,  -62914560 },
    /* CC11 to Gain: Concave, Inverted, -96 dB (same as DLS1) */
    { 0x8B, 0,      1, 0x8400,  -62914560 },
    /* RPN1 to Pitch: 100 cents (same as DLS1) */
    { 0x101, 0,     3, 0x4000,   6553600 },
    /* CC10 to Pan: DLS2 uses 50.8% instead of DLS1's 50% */
    { 0x8A, 0,      4, 0x4000,  33292288 },
    /* CC91 to Reverb Send (same as DLS1) */
    { 0xDB, 0,   0x81, 0x0000,  65536000 },
    /* CC93 to Chorus Send (same as DLS1) */
    { 0xDD, 0,   0x80, 0x0000,  65536000 },
};

static const size_t g_dlsv2_default_connections_count = sizeof(g_dlsv2_default_connections) / sizeof(g_dlsv2_default_connections[0]);
static const size_t g_dls_default_connections_count = sizeof(g_dls_default_connections) / sizeof(g_dls_default_connections[0]);

static bool dls_connection_matches(const DLS_Connection* a, const DLS_Connection* b) {
    return a->source == b->source && a->control == b->control && a->destination == b->destination;
}

static bool dls_has_connection(const DLS_Articulation* art, const DLS_Connection* candidate) {
    for (uint32_t i = 0; i < art->connectionCount; i++) {
        if (dls_connection_matches(&art->runtimeConnections[i], candidate)) return true;
    }
    return false;
}

static int32_t dls_transform_source_q16(int32_t source, uint16_t transform) {
    int32_t value = source;
    if (transform & 0x8000) value = 0x10000 - value;
    if (transform & 0x4000) return (value << 1) - 0x10000;
    int32_t type = transform & 0x0F;
    if (type == 0) type = (transform >> 10) & 0x0F;
    if (type == 1) return value > 65275 ? 0x10000 : -5 * dls_log10_q16(0x10000 - value) / 12;
    return value;
}

static int32_t dls_connection_value_q16(const DLS_Connection* connection, int32_t source, int32_t control) {
    return DLS_FP_MUL(DLS_FP_MUL(dls_transform_source_q16(source, connection->transform), control), connection->scale);
}

static int32_t dls_note_on_connection_value_q16(const DLS_Connection* connection, int32_t key,
                                                  int32_t velocity, int32_t unityNote,
                                                  const DLS_ChannelState* ch) {
    int32_t source;
    int32_t control = 0x10000;
    if (connection->source == 2) {
        source = velocity;
    } else if (connection->source == 3) {
        source = connection->destination == 3 ? key - unityNote : key;
    } else {
        return 0;
    }

    bool quirks = ch && ch->selectedInstrument ? dls_bank_quirks(ch->selectedInstrument->parentBank) : g_use_mobilebae_quirks;

    if (!quirks && (connection->transform & 0x8000)) {
        source = velocity;
    } else if (connection->transform & 0x8000) {
        source = 127 - source;
    }
    if (connection->transform & 0x4000) {
        source = (source << 17) / 128 - 0x10000;
    } else {
        int32_t type = connection->transform & 0x0F;
        if (type == 0) type = (connection->transform >> 10) & 0x0F;
        if (type == 0) {
            source <<= 9;
        } else if (type == 1) {
            if (quirks) {
                source = source == 127 ? 0x10000 : -5 * dls_log10_q16(((127 - source) << 16) / 127) / 12;
            } else {
                source = -5 * dls_log10_q16(((source) << 16) / 127) / 12;
            }
        } else {
            return 0;
        }
    }
    if (connection->control == 0x81 && ch) {
        int32_t modulation14 = (((ch->modulation & 0x7F) << 7) | (ch->modulationLsb & 0x7F));
        control = (modulation14 & 0x3FFF) << 2;
    } else if (connection->control == 0x100 && ch) {
        control = (ch->rpnValues[0] & 0x3FFF) << 2;
    }
    return DLS_FP_MUL(DLS_FP_MUL(source, control), connection->scale);
}

static void dls_apply_note_on_connection(const DLS_Connection* connection, int32_t key, int32_t velocity,
                                         int32_t unityNote, const DLS_ChannelState* ch,
                                         int32_t* pitch, int32_t* gainAttenuation,
                                         int32_t* panOffset) {
    int32_t value = dls_note_on_connection_value_q16(connection, key, velocity, unityNote, ch);
    if (value == 0) return;
    if (connection->destination == 1) *gainAttenuation += value;
    else if (connection->destination == 3) *pitch += value / 100;
    else if (connection->destination == 4) *panOffset += value / 500;
}

static void dls_apply_note_on_connections(const DLS_Articulation* art, int32_t key, int32_t velocity,
                                          int32_t unityNote, const DLS_ChannelState* ch,
                                          int32_t* pitch, int32_t* gainAttenuation,
                                          int32_t* panOffset) {
    for (uint32_t i = 0; i < art->connectionCount; i++) {
        dls_apply_note_on_connection(&art->runtimeConnections[i], key, velocity, unityNote, ch,
                                     pitch, gainAttenuation, panOffset);
    }
    bool quirks = dls_bank_quirks(ch->selectedInstrument->parentBank);
    if (quirks) {
        for (uint32_t i = 0; i < g_dls_default_connections_count; i++) {
            if (!dls_has_connection(art, &g_dls_default_connections[i])) {
                dls_apply_note_on_connection(&g_dls_default_connections[i], key, velocity, unityNote, ch,
                                             pitch, gainAttenuation, panOffset);
            }
        }
    } else {
        for (uint32_t i = 0; i < g_dlsv2_default_connections_count; i++) {
            if (!dls_has_connection(art, &g_dlsv2_default_connections[i])) {
                dls_apply_note_on_connection(&g_dlsv2_default_connections[i], key, velocity, unityNote, ch,
                                             pitch, gainAttenuation, panOffset);
            }
        }
    }
}

static int32_t dls_runtime_connection_value_q16(const DLS_Connection* connection, const DLS_Voice* voice) {
    const DLS_ChannelState* ch = voice->channelState;
    int32_t source;
    int32_t control = 0x10000;

#define DLS_CC_14(msb, lsb) ((((msb) & 0x7F) << 7) | ((lsb) & 0x7F))

    switch (connection->source) {
        case 1: source = voice->modulationLfo.output; break;
        case 5: source = voice->eg2Envelope.current; break;
        case 9: source = voice->vibratoLfo.output; break;
        case 7: source = (ch->channelPressure & 0x7F) << 9; break;
        case 8: source = (voice->channelState->keyPressure[voice->key & 0x7F] & 0x7F) << 9; break;
        case 6:
            if (connection->control != 0x100) return 0;
            source = (ch->pitchBend & 0x3FFF) << 2;
            control = (ch->rpnValues[0] & 0x3FFF) << 2;
            break;
        case 0x81: source = DLS_CC_14(ch->modulation, ch->modulationLsb) << 2; break;
        case 0x101: source = (ch->rpnValues[1] & 0x3FFF) << 2; break;
        case 0x87: source = DLS_CC_14(ch->volume, ch->volumeLsb) << 2; break;
        case 0x8B: source = DLS_CC_14(ch->expression, ch->expressionLsb) << 2; break;
        case 0x8A: source = DLS_CC_14(ch->pan, ch->panLsb) << 2; break;
        case 0xDB: source = (ch->reverb & 0x7F) << 9; break;
        case 0xDD: source = (ch->chorus & 0x7F) << 9; break;
        default: return 0;
    }

    if (connection->control == 0x81) control = DLS_CC_14(ch->modulation, ch->modulationLsb) << 2;
    else if (connection->control == 0x100) control = (ch->rpnValues[0] & 0x3FFF) << 2;
    else if (connection->control != 0) return 0;
#undef DLS_CC_14
    return dls_connection_value_q16(connection, source, control);
}

static void dls_apply_runtime_connection(const DLS_Connection* connection, const DLS_Voice* voice,
                                         int32_t* runtimePitch, int32_t* gainAttenuation,
                                         int32_t* panOffset, int32_t* reverbSend, int32_t* chorusSend,
                                         int32_t* filterCutoffDelta, int32_t* filterResonanceDelta) {
    int32_t value = dls_runtime_connection_value_q16(connection, voice);
    if (value == 0) return;
    if (connection->destination == 1) *gainAttenuation += value / 10;
    else if (connection->destination == 3) *runtimePitch += value / 100;
    else if (connection->destination == 4) *panOffset += value / 500;
    else if (connection->destination == 0x81) *reverbSend += value / 1000;
    else if (connection->destination == 0x80) *chorusSend += value / 1000;
    else if (connection->destination == 0x500) *filterCutoffDelta += value / 100;
    else if (connection->destination == 0x501) (void)filterResonanceDelta;
}

static void dls_apply_runtime_connections(const DLS_Articulation* art, const DLS_Voice* voice,
                                          int32_t* runtimePitch, int32_t* gainAttenuation,
                                          int32_t* panOffset, int32_t* reverbSend, int32_t* chorusSend,
                                          int32_t* filterCutoffDelta, int32_t* filterResonanceDelta) {
    for (uint32_t i = 0; i < art->connectionCount; i++) {
        dls_apply_runtime_connection(&art->runtimeConnections[i], voice, runtimePitch, gainAttenuation,
                                     panOffset, reverbSend, chorusSend, filterCutoffDelta, filterResonanceDelta);
    }
    bool quirks = dls_bank_quirks(voice->parentBank);
    if (quirks) {
        for (uint32_t i = 0; i < g_dls_default_connections_count; i++) {
            if (!dls_has_connection(art, &g_dls_default_connections[i])) {
                dls_apply_runtime_connection(&g_dls_default_connections[i], voice, runtimePitch, gainAttenuation,
                                             panOffset, reverbSend, chorusSend, filterCutoffDelta, filterResonanceDelta);
            }
        }
    } else {
        for (uint32_t i = 0; i < g_dlsv2_default_connections_count; i++) {
            if (!dls_has_connection(art, &g_dlsv2_default_connections[i])) {
                dls_apply_runtime_connection(&g_dlsv2_default_connections[i], voice, runtimePitch, gainAttenuation,
                                             panOffset, reverbSend, chorusSend, filterCutoffDelta, filterResonanceDelta);
            }
        }
    }
}

static int32_t dls_channel_value14(int32_t msb, int32_t lsb) {
    return ((msb & 0x7F) << 7) | (lsb & 0x7F);
}

static void dls_channel_reset_controllers(DLS_ChannelState* ch) {
    if (!ch) return;
    ch->modulation = 0;
    ch->modulationLsb = 0;
    ch->foot = 0;
    ch->footLsb = 0;
    ch->volume = 100;
    ch->volumeLsb = 0;
    ch->expression = 127;
    ch->expressionLsb = 0;
    ch->pan = 64;
    ch->panLsb = 0;
    ch->sustain = false;
    ch->reverb = 40;
    ch->chorus = 0;
    ch->pitchBend = 0x2000;
    ch->rpnValues[0] = 0x0100;
    ch->rpnValues[1] = 0x2000;
    ch->rpnValues[2] = 0x2000;
    ch->rpnMsb = 127;
    ch->rpnLsb = 127;
    ch->nrpnMsb = 127;
    ch->nrpnLsb = 127;
    ch->selectorMode = 0;
    ch->channelPressure = 0;
    XSetMemory(ch->keyPressure, sizeof(ch->keyPressure), 0);
}

/* Equivalent to RetroDLS ChannelState.bankSelector() in its default mode. */
static int32_t DLS_ChannelBankSelector(const DLS_ChannelState* ch) {
    int32_t msb = ch->bankMsb & 0x7F;
    int32_t lsb = ch->bankLsb & 0x7F;
    int32_t defaultMsb = ch->channel == 9 ? 120 : 121;

    if (msb == 120 || msb == 121) return (msb << 7) | lsb;
    if (msb != 0) return (defaultMsb << 7) | (lsb == 0 ? msb : lsb);
    return (defaultMsb << 7) | lsb;
}

static void dls_program_change(DLS_Synth* synth, DLS_ChannelState* ch, int32_t program) {
    int32_t encodedBank;

    if (!synth || !ch) return;

    /* GenSeq can pass a combined value: (hsbBank * 128) + program.
       Decode that bank hint here so native DLS resolves the same overlay bank
       choices that SF2/FluidSynth does. */
    encodedBank = (program >> 7) & 0x3FFF;
    if (encodedBank > 0)
    {
        int32_t msbHint = (encodedBank >> 7) & 0x7F;
        int32_t lsbHint = encodedBank & 0x7F;

        if (msbHint == 120 || msbHint == 121)
        {
            /* Already in selector form (MSB/LSB). */
            ch->bankMsb = msbHint;
            ch->bankLsb = lsbHint;
        }
        else
        {
            /* HSB convention: even banks melodic, odd banks percussion. */
            ch->bankMsb = (encodedBank & 1) ? 120 : 121;
            ch->bankLsb = (encodedBank >> 1) & 0x7F;
        }
    }

    ch->program = program & 0x7F;
    ch->selectedBankSelector = DLS_ChannelBankSelector(ch);
    ch->selectedInstrument = DLS_Synth_FindInstrument(synth, ch->selectedBankSelector, ch->program);
    ch->programSelected = true;
}

static void dls_voice_init(DLS_Voice* v, int32_t channel, int32_t key, int32_t velocity, 
                           DLS_Region* region, DLS_Wave* wave, DLS_ChannelState* ch, int32_t sampleRate) {
    v->channel = channel;
    v->key = key;
    v->velocity = velocity;
    v->regionIndex = region->index;
    v->keyGroup = region->keyGroup;
    v->wave = wave;
    v->articulation = &region->articulation;
    v->channelState = ch;
    v->parentBank = ch->selectedInstrument ? ch->selectedInstrument->parentBank : NULL;
    
    DLS_Articulation* art = &region->articulation;
    v->connectionCount = art->connectionCount;
    v->runtimeConnections = art->runtimeConnections;
    
    int32_t notePitch = art->pitch;
    int32_t noteGainAttenuation = 0;
    int32_t notePanOffset = art->pan;
    int32_t filterCutoff = art->filterCutoff == FILTER_DISABLED_CUTOFF
        ? FILTER_DISABLED_CUTOFF
        : (art->filterCutoff > FILTER_MIN_CUTOFF ? art->filterCutoff : FILTER_MIN_CUTOFF);
    DLS_SampleInfo* sample = region->sample.present ? &region->sample : &wave->sample;
    int32_t unityNote = sample->present ? sample->unityNote : 60;
    dls_apply_note_on_connections(art, key, velocity, unityNote, ch, &notePitch,
                                  &noteGainAttenuation, &notePanOffset);

    v->baseGainQ16 = 0x10000;
    v->basePanOffset = notePanOffset;
    v->baseReverbSend = art->reverb;
    v->baseChorusSend = art->chorus;

    v->looping = sample->loopMode == DLS_LOOP_FORWARD;
    v->loopUntilRelease = sample->loopUntilRelease;
    v->loopStart = (int64_t)sample->loopStart << 16;
    v->loopEnd = (int64_t)(sample->loopEndInclusive + 1) << 16;
    if (v->loopStart < 0 || v->loopEnd <= v->loopStart || v->loopEnd > ((int64_t)wave->frames << 16)) {
        v->looping = false;
        v->loopStart = 0;
        v->loopEnd = 0;
    }
    v->baseGainQ16 = dls_exp10_q16(sample->attenuation / 200);
    if (noteGainAttenuation != 0) {
        v->baseGainQ16 = DLS_FP_MUL(v->baseGainQ16, dls_exp10_q16(noteGainAttenuation / 200));
    }
    
    int32_t eg1Delay = dls_timecent_to_micros(art->eg1Delay);
    int32_t eg1Attack = dls_timecent_to_micros(art->eg1Attack);
    int32_t eg1Hold = dls_timecent_to_micros(art->eg1Hold);
    int32_t eg1Decay = dls_timecent_to_micros(art->eg1Decay);
    int32_t eg1Release = dls_timecent_to_micros(art->eg1Release);
    int32_t eg2Delay = dls_timecent_to_micros(art->eg2Delay);
    int32_t eg2Attack = dls_timecent_to_micros(art->eg2Attack);
    int32_t eg2Hold = dls_timecent_to_micros(art->eg2Hold);
    int32_t eg2Decay = dls_timecent_to_micros(art->eg2Decay);
    int32_t eg2Release = dls_timecent_to_micros(art->eg2Release);

    for (uint32_t i = 0; i < art->connectionCount; i++) {
        const DLS_Connection* connection = &art->runtimeConnections[i];
        int32_t value = dls_note_on_connection_value_q16(connection, key, velocity, unityNote, ch);
        if (value == 0) continue;
        if (connection->destination == 0x20B) {
            eg1Delay = dls_modulated_time_micros(eg1Delay, value);
        } else if (connection->destination == 0x206) {
            eg1Attack = dls_modulated_time_micros(eg1Attack, value);
        } else if (connection->destination == 0x20C) {
            eg1Hold = dls_modulated_time_micros(eg1Hold, value);
        } else if (connection->destination == 0x207) {
            eg1Decay = dls_modulated_time_micros(eg1Decay, value);
        } else if (connection->destination == 0x209) {
            eg1Release = dls_modulated_time_micros(eg1Release, value);
        } else if (connection->destination == 0x30F) {
            eg2Delay = dls_modulated_time_micros(eg2Delay, value);
        } else if (connection->destination == 0x30A) {
            eg2Attack = dls_modulated_time_micros(eg2Attack, value);
        } else if (connection->destination == 0x310) {
            eg2Hold = dls_modulated_time_micros(eg2Hold, value);
        } else if (connection->destination == 0x30B) {
            eg2Decay = dls_modulated_time_micros(eg2Decay, value);
        } else if (connection->destination == 0x30D) {
            eg2Release = dls_modulated_time_micros(eg2Release, value);
        } else if (connection->destination == 0x500) {
            /* RetroDLS folds note-on filter modulation into the filter base cutoff. */
            filterCutoff += value / 100;
        }
    }

    /* `art->pitch` is semitones in Q16.16; the resampler expects cents in
       Q16.16.  RetroDLS applies the key-number pitch connection here too. */
    int32_t pitchCentsQ16 = notePitch * 100;
    pitchCentsQ16 += sample->fineTuneCents * (1 << 16);
    v->baseIncrement = dls_pitch_cents_to_increment(pitchCentsQ16, sampleRate, wave->sampleRate);
    if (v->baseIncrement < 1) v->baseIncrement = 1;

    dls_env_init(&v->envelope, eg1Delay, eg1Attack, eg1Hold, eg1Decay,
                 art->eg1Sustain, eg1Release, true, sampleRate);
    dls_env_init(&v->eg2Envelope, eg2Delay, eg2Attack, eg2Hold, eg2Decay,
                 art->eg2Sustain, eg2Release, false, sampleRate);
    v->envelope.forceQuirks = dls_bank_quirks(v->parentBank);
    v->eg2Envelope.forceQuirks = dls_bank_quirks(v->parentBank);
    if (!v->envelope.forceQuirks) {
        v->envelope.shutdownMicros = art->eg1Shutdown > 0 ? dls_timecent_to_micros(art->eg1Shutdown) : DLS_FORCED_FADE_MICROS;
        v->eg2Envelope.shutdownMicros = art->eg2Shutdown > 0 ? dls_timecent_to_micros(art->eg2Shutdown) : DLS_FORCED_FADE_MICROS;
    } else {
        v->envelope.shutdownMicros = DLS_FORCED_FADE_MICROS;
        v->eg2Envelope.shutdownMicros = DLS_FORCED_FADE_MICROS;
    }
    dls_lfo_init(&v->vibratoLfo, art->vibratoFrequency, art->vibratoStartDelay, sampleRate);
    dls_lfo_init(&v->modulationLfo, art->lfoFrequency, art->lfoStartDelay, sampleRate);
    
    if (filterCutoff != FILTER_DISABLED_CUTOFF) {
        v->filterEnabled = true;
        dls_filter_init(&v->filter, sampleRate, filterCutoff, art->filterResonance);
    } else {
        v->filterEnabled = false;
    }
    
    v->startSerial = 0; // Handled by synth
    v->currentIncrement = v->baseIncrement;
    /* RetroDLS updates envelopes, LFOs, and gain targets once every 10 ms. */
    v->controlBlockFrames = (sampleRate * DEFAULT_RENDER_PERIOD_MS) / 1000;
    if (v->controlBlockFrames < 1) v->controlBlockFrames = 1;
    {
        int32_t env1 = dls_env_next(&v->envelope, v->controlBlockFrames);
        dls_env_next(&v->eg2Envelope, v->controlBlockFrames);
        dls_lfo_next(&v->vibratoLfo, v->controlBlockFrames);
        dls_lfo_next(&v->modulationLfo, v->controlBlockFrames);

        int32_t runtimePitch = 0;
        int32_t gainAttenuation = 0;
        int32_t filterCutoffDelta = 0;
        int32_t filterResonanceDelta = 0;
        int32_t panOffset = v->basePanOffset;
        int32_t reverbSend = v->baseReverbSend;
        int32_t chorusSend = v->baseChorusSend;

        dls_apply_runtime_connections(v->articulation, v, &runtimePitch, &gainAttenuation,
                                      &panOffset, &reverbSend, &chorusSend,
                                      &filterCutoffDelta, &filterResonanceDelta);

        if (v->filterEnabled) {
            dls_filter_update(&v->filter, filterCutoffDelta, filterResonanceDelta);
        }

        int32_t gainQ16 = v->baseGainQ16;
        bool voiceQuirks = dls_bank_quirks(v->parentBank);
        if (voiceQuirks) {
            if (gainAttenuation < 0) {
                gainQ16 = DLS_FP_MUL(gainQ16, dls_exp10_q16(gainAttenuation / 20));
            }
        } else if (gainAttenuation != 0) {
            gainQ16 = DLS_FP_MUL(gainQ16, dls_exp10_q16(gainAttenuation / 20));
        }
        gainQ16 = DLS_FP_MUL(gainQ16, env1);

        panOffset = dls_clamp(panOffset, -0x10000, 0x10000);
        v->targetLeftGain = DLS_FP_MUL(gainQ16, dls_pan_scale_q16(-panOffset, voiceQuirks));
        v->targetRightGain = DLS_FP_MUL(gainQ16, dls_pan_scale_q16(panOffset, voiceQuirks));

        if (reverbSend == 0 && ch->reverb > 0) {
            reverbSend = (ch->reverb & 0x7F) << 9;
        }
        if (chorusSend == 0 && ch->chorus > 0) {
            chorusSend = (ch->chorus & 0x7F) << 9;
        }
        v->targetReverbSend = dls_clamp(reverbSend, 0, 0x10000);
        v->targetChorusSend = dls_clamp(chorusSend, 0, 0x10000);

        v->leftGain = v->targetLeftGain;
        v->rightGain = v->targetRightGain;
        v->reverbSend = v->targetReverbSend;
        v->chorusSend = v->targetChorusSend;

        v->rampStartLeftGain = v->leftGain;
        v->rampStartRightGain = v->rightGain;
        v->rampStartReverbSend = v->reverbSend;
        v->rampStartChorusSend = v->chorusSend;
        v->rampSegmentFrame = 0;
        v->rampSegmentFrames = 0;
        v->rampSegmentStartLeftGain = v->leftGain;
        v->rampSegmentStartRightGain = v->rightGain;
        v->rampSegmentStartReverbSend = v->reverbSend;
        v->rampSegmentStartChorusSend = v->chorusSend;
        v->rampInitialized = true;
        v->rampFrame = 0;
        v->currentIncrement = (v->baseIncrement * dls_pitch_cents_to_ratio_q16(runtimePitch * 100)) >> 16;
    }
    v->controlFramesUntilTick = v->controlBlockFrames;
    
    v->position = 0;
    v->keyHeld = true;
    v->sustainSnapshot = false;
    v->active = true;
}

static void dls_voice_fast_kill(DLS_Voice* v) {
    if (!v || !v->active) return;
    v->keyHeld = false;
    v->sustainSnapshot = false;
    dls_env_shutdown(&v->envelope);
    dls_env_shutdown(&v->eg2Envelope);
    v->controlFramesUntilTick = 0;
}

static void dls_kill_exclusive_voices(DLS_Synth* synth, int32_t channel, int32_t key, const DLS_Region* region) {
    if (!synth || !region) return;

    int32_t exclusiveClass = region->keyGroup & 0x0F;
    for (int i = 0; i < 256; i++) {
        DLS_Voice* voice = &synth->voices[i];
        if (!voice->active) continue;

        bool voiceQuirks = dls_bank_quirks(voice->parentBank);
        if (voiceQuirks) {
            if (voice->channel != channel) continue;
        }

        bool sameRegionKey = ((region->options & 0x10) != 0) &&
                             (voice->key == key) &&
                             (voice->regionIndex == region->index);
        bool sameExclusiveClass = (exclusiveClass != 0) &&
                                  ((voice->keyGroup & 0x0F) == exclusiveClass);
        if (sameRegionKey || sameExclusiveClass) {
            dls_voice_fast_kill(voice);
        }
    }
}

static int32_t dls_find_free_voice_index(DLS_Synth* synth) {
    for (int i = 0; i < 256; i++) {
        if (!synth->voices[i].active) return i;
    }
    return -1;
}

static int32_t dls_find_recyclable_voice(DLS_Synth* synth, int32_t newChannel) {
    static const int32_t kQuirksVoiceStealOrder[16] = {15, 14, 13, 12, 11, 10, 8, 7, 6, 5, 4, 3, 2, 1, 0, 9};
    static const int32_t kSpecVoiceStealOrder[16] = {9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15};
    bool useQuirks = dls_bank_quirks(synth->banks[0]) || dls_bank_quirks(synth->banks[1]);
    const int32_t* order = useQuirks ? kQuirksVoiceStealOrder : kSpecVoiceStealOrder;

    for (int ord = 0; ord < 16; ord++) {
        int32_t channel = order[ord];
        if (newChannel != 9 && channel == 9) continue;

        int32_t candidate = -1;
        for (int i = 0; i < 256; i++) {
            DLS_Voice* voice = &synth->voices[i];
            if (voice->active && voice->channel == channel && !voice->keyHeld && !voice->sustainSnapshot) {
                candidate = i;
            }
        }
        if (candidate >= 0) return candidate;
    }
    return -1;
}

static int32_t dls_find_held_percussion_voice(DLS_Synth* synth, int32_t newChannel) {
    static const int32_t kQuirksVoiceStealOrder[16] = {15, 14, 13, 12, 11, 10, 8, 7, 6, 5, 4, 3, 2, 1, 0, 9};
    static const int32_t kSpecVoiceStealOrder[16] = {9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15};
    bool useQuirks = dls_bank_quirks(synth->banks[0]) || dls_bank_quirks(synth->banks[1]);
    const int32_t* order = useQuirks ? kQuirksVoiceStealOrder : kSpecVoiceStealOrder;

    for (int ord = 0; ord < 16; ord++) {
        int32_t channel = order[ord];
        if (newChannel != 9 && channel == 9) continue;

        int32_t candidate = -1;
        int64_t priority = INT64_MAX;
        for (int i = 0; i < 256; i++) {
            DLS_Voice* voice = &synth->voices[i];
            if (useQuirks) {
                if (channel == 9 && voice->channel == channel && voice->keyHeld && voice->startSerial < priority) {
                    candidate = i;
                    priority = voice->startSerial;
                }
            } else {
                if (voice->channel == channel && voice->keyHeld && voice->startSerial < priority) {
                    candidate = i;
                    priority = voice->startSerial;
                }
            }
        }
        if (candidate >= 0) return candidate;
    }
    return -1;
}

static int32_t dls_find_active_voice_by_priority(DLS_Synth* synth, int32_t newChannel) {
    static const int32_t kQuirksVoiceStealOrder[16] = {15, 14, 13, 12, 11, 10, 8, 7, 6, 5, 4, 3, 2, 1, 0, 9};
    static const int32_t kSpecVoiceStealOrder[16] = {9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15};
    bool useQuirks = dls_bank_quirks(synth->banks[0]) || dls_bank_quirks(synth->banks[1]);
    const int32_t* order = useQuirks ? kQuirksVoiceStealOrder : kSpecVoiceStealOrder;

    for (int ord = 0; ord < 16; ord++) {
        int32_t channel = order[ord];
        if (newChannel != 9 && channel == 9) continue;

        int32_t candidate = -1;
        int64_t priority = INT64_MAX;
        for (int i = 0; i < 256; i++) {
            DLS_Voice* voice = &synth->voices[i];
            if (voice->active && voice->channel == channel && voice->startSerial < priority) {
                candidate = i;
                priority = voice->startSerial;
            }
        }
        if (candidate >= 0) return candidate;
    }
    return -1;
}

static int32_t dls_select_voice_index_for_note_on(DLS_Synth* synth, int32_t newChannel) {
    int32_t idx = dls_find_free_voice_index(synth);
    if (idx >= 0) return idx;

    idx = dls_find_recyclable_voice(synth, newChannel);
    if (idx >= 0) return idx;

    idx = dls_find_held_percussion_voice(synth, newChannel);
    if (idx >= 0) return idx;

    idx = dls_find_active_voice_by_priority(synth, newChannel);
    if (!g_use_mobilebae_quirks) return idx;
    return idx >= 0 ? idx : 0;
}

static int32_t dls_channel_coarse_semitones(const DLS_ChannelState* ch) {
    if (!ch) return 0;
    return ((int16_t)((ch->rpnValues[2] & 0x3FFF) - 0x2000)) >> 7;
}

void GM_DLS_ProcessNoteOn(GM_Song* pSong, uint16_t channel, uint16_t note, uint16_t velocity) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    if (velocity == 0) {
        GM_DLS_ProcessNoteOff(pSong, channel, note, 64);
        return;
    }

    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    
    DLS_ChannelState* ch = &synth->channels[channel];
    ch->channel = channel;

    if (!ch->programSelected) {
        dls_program_change(synth, ch, ch->program);
    }

    DLS_Instrument* inst = ch->selectedInstrument;
    if (!inst) {
        debug_message("DLS Synth: no instrument for bank=%d:%d (selector=%d) program=%d\n",
                      ch->bankMsb, ch->bankLsb, ch->selectedBankSelector, ch->program);
        return;
    }

//    debug_message("DLS Synth: bank=%d:%d selector=%d program=%d -> instrument bank=%d:%d program=%d\n",
//                  ch->bankMsb, ch->bankLsb, ch->selectedBankSelector, ch->program,
//                  inst->bankMsb, inst->bankLsb, inst->program);
    
    /* Apply percussion key alias for drum banks to find region,
       but keep original note for voice initialization and pitch */
    int32_t voiceNote = note;
    if ((ch->selectedBankSelector & 0x3F80) == (120 << 7)) {
        if (inst->parentBank && inst->parentBank->hasPercussionKeyAliases) {
            int32_t aliasedNote = inst->parentBank->percussionKeyAliases[note & 0x7F] & 0x7F;
            if (aliasedNote != note) {
                debug_message("DLS Synth: percussion key alias key %d -> %d for region lookup\n", note, aliasedNote);
            }
            voiceNote = aliasedNote;
        } else if (dls_bank_spmidi(inst->parentBank)) {
            /* SP-MIDI percussion key remap (Figure 7): only remap when the
               bank has no PGAL key aliases AND no region exists for the
               original note.  This avoids overriding real instrument regions
               that happen to share a key with a group-defined non-canonical
               member. */
            int32_t spRemap = DLS_SPMIDI_RemapPercussionKey(note & 0x7F);
            if (spRemap != (int32_t)(note & 0x7F)) {
                int32_t clampedNote = dls_clamp(note + dls_channel_coarse_semitones(ch), 0, 127);
                DLS_Region* origRegion = DLS_Synth_FindRegion(inst, clampedNote, velocity);
                if (!origRegion) {
                    debug_message("DLS Synth: SP-MIDI perc key alias %d -> %d for region lookup\n",
                                  note & 0x7F, spRemap);
                    voiceNote = spRemap;
                }
            }
        }
    }

    int32_t regionKey = voiceNote;
    if (!(((ch->selectedBankSelector & 0x3F80) == (120 << 7)) &&
          inst->parentBank && inst->parentBank->hasPercussionKeyAliases)) {
        regionKey = dls_clamp(voiceNote + dls_channel_coarse_semitones(ch), 0, 127);
    }
    
    DLS_Region* region = DLS_Synth_FindRegion(inst, regionKey, velocity);
    if (!region) {
        debug_message("DLS Synth: no region for instrument=%d:%d:%d key=%d velocity=%d\n",
                      inst->bankMsb, inst->bankLsb, inst->program, note, velocity);
        return;
    }
    
//    debug_message("DLS Synth: region found: keyLow=%d keyHigh=%d velLow=%d velHigh=%d tableIndex=%d\n",
//                  region->keyLow, region->keyHigh, region->velocityLow, region->velocityHigh, region->tableIndex);
    
    DLS_Wave* wave = NULL;
    if (inst->parentBank && region->tableIndex >= 0 && region->tableIndex < inst->parentBank->waveCount) {
        wave = &inst->parentBank->waves[region->tableIndex];
    }
    if (!wave || !wave->pcm) {
        debug_message("DLS Synth: no decoded wave for instrument=%d:%d:%d key=%d region=%d table=%d\n",
                      inst->bankMsb, inst->bankLsb, inst->program, note,
                      region->index, region->tableIndex);
        return;
    }

    dls_kill_exclusive_voices(synth, channel, voiceNote, region);
    
    int32_t voiceIdx = dls_select_voice_index_for_note_on(synth, channel);
    if (voiceIdx < 0) {
        return;
    }
    
    DLS_Voice* v = &synth->voices[voiceIdx];
    /* Pass the aliased key to voice init so pitch is calculated correctly */
    dls_voice_init(v, channel, voiceNote, velocity, region, wave, ch, synth->sampleRate);
    v->startSerial = synth->nextVoiceSerial++;
}

void GM_DLS_ProcessNoteOff(GM_Song* pSong, uint16_t channel, uint16_t note, uint16_t velocity) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    DLS_ChannelState* ch = &synth->channels[channel & 0x0F];
    
    /* Apply percussion key alias if this is a drum channel */
    int32_t matchNote = note;
    int32_t bankSelector = ch->programSelected ? ch->selectedBankSelector : DLS_ChannelBankSelector(ch);
    DLS_Bank* aliasBank = (ch->selectedInstrument && ch->selectedInstrument->parentBank)
        ? ch->selectedInstrument->parentBank
        : (synth->banks[1] ? synth->banks[1] : synth->banks[0]);
    if ((bankSelector & 0x3F80) == (120 << 7)) {
        if (aliasBank && aliasBank->hasPercussionKeyAliases) {
            int32_t aliasedNote = aliasBank->percussionKeyAliases[note & 0x7F] & 0x7F;
            if (aliasedNote != note) {
                debug_message("DLS Synth: note-off percussion key alias key %d -> %d\n", note, aliasedNote);
            }
            matchNote = aliasedNote;
        } else if (dls_bank_spmidi(aliasBank)) {
            /* SP-MIDI percussion key remap: try the original note first,
               fall back to the remapped key only if no voice matches.
               This mirrors the note-on behaviour where remap only fires
               after the original note's region is confirmed missing. */
            int32_t spRemap = DLS_SPMIDI_RemapPercussionKey(note & 0x7F);
            if (spRemap != (int32_t)(note & 0x7F)) {
                int32_t altNote = spRemap;
                bool origFound = false;
                for (int i = 0; i < 256; i++) {
                    DLS_Voice* v = &synth->voices[i];
                    if (v->active && v->channel == channel && v->key == note) {
                        origFound = true;
                        break;
                    }
                }
                if (!origFound) {
                    matchNote = altNote;
                }
            }
        }
    }

    for (int i = 0; i < 256; i++) {
        DLS_Voice* v = &synth->voices[i];
        if (v->active && v->channel == channel && v->key == matchNote) {
            v->keyHeld = false;
        }
    }
}

void GM_DLS_AllNotesOff(GM_Song* pSong, int16_t channel, bool immediate) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    for (int i = 0; i < 256; i++) {
        DLS_Voice* v = &synth->voices[i];
        if (!v->active || (channel >= 0 && v->channel != channel)) continue;
        v->keyHeld = false;
        if (immediate) {
            v->sustainSnapshot = false;
            v->active = false;
        }
    }
}

void GM_DLS_ProcessProgramChange(GM_Song* pSong, uint16_t channel, uint16_t program) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    dls_program_change(synth, &synth->channels[channel & 0x0F], program);
}

bool GM_DLS_HasProgram(GM_Song* pSong, uint16_t channel, uint16_t program)
{
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth)
    {
        return FALSE;
    }

    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    DLS_ChannelState* ch = &synth->channels[channel & 0x0F];

    dls_program_change(synth, ch, program);

    return (ch->selectedInstrument != NULL) ? TRUE : FALSE;
}

void GM_DLS_ProcessPitchBend(GM_Song* pSong, uint16_t channel, uint16_t value) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    synth->channels[channel].pitchBend = value & 0x3FFF;
}

void GM_DLS_ProcessKeyPressure(GM_Song* pSong, uint16_t channel, uint16_t key, uint16_t value) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    DLS_ChannelState* ch = &synth->channels[channel & 0x0F];
    ch->keyPressure[key & 0x7F] = value & 0x7F;
}

void GM_DLS_ProcessChannelPressure(GM_Song* pSong, uint16_t channel, uint16_t value) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    DLS_ChannelState* ch = &synth->channels[channel & 0x0F];
    ch->channelPressure = value & 0x7F;
}

void GM_DLS_ProcessController(GM_Song* pSong, uint16_t channel, uint16_t controller, uint16_t value) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    
    DLS_ChannelState* ch = &synth->channels[channel];
    if (controller == 64) {
        ch->sustain = value >= 64;
    } else if (controller == 121) {
        dls_channel_reset_controllers(ch);
        ch->selectedInstrument = NULL;
        ch->selectedBankSelector = 0;
        ch->programSelected = false;
    } else if (controller == 120) {
        GM_DLS_AllNotesOff(pSong, channel, true);
    } else if (controller == 123 || (controller >= 124 && controller <= 127)) {
        GM_DLS_AllNotesOff(pSong, channel, false);
    } else if (controller == 0) {
        ch->bankMsb = value & 0x7F;
        ch->bankLsb = 0;
        ch->selectedInstrument = NULL;
        ch->selectedBankSelector = 0;
        ch->programSelected = false;
    } else if (controller == 32) {
        ch->bankLsb = value & 0x7F;
        ch->selectedInstrument = NULL;
        ch->selectedBankSelector = 0;
        ch->programSelected = false;
    } else if (controller == 1) {
        ch->modulation = value & 0x7F;
    } else if (controller == 33) {
        ch->modulationLsb = value & 0x7F;
    } else if (controller == 4) {
        ch->foot = value & 0x7F;
    } else if (controller == 36) {
        ch->footLsb = value & 0x7F;
    } else if (controller == 7) {
        ch->volume = value & 0x7F;
    } else if (controller == 39) {
        ch->volumeLsb = value & 0x7F;
    } else if (controller == 10) {
        ch->pan = value & 0x7F;
    } else if (controller == 42) {
        ch->panLsb = value & 0x7F;
    } else if (controller == 11) {
        ch->expression = value & 0x7F;
    } else if (controller == 43) {
        ch->expressionLsb = value & 0x7F;
    } else if (controller == 101) {
        ch->rpnMsb = value & 0x7F;
        ch->rpnLsb = 127;
        ch->selectorMode = 1;
    } else if (controller == 100) {
        ch->rpnLsb = value & 0x7F;
        ch->selectorMode = 1;
    } else if (controller == 99) {
        ch->nrpnMsb = value & 0x7F;
        ch->nrpnLsb = 127;
        ch->selectorMode = 2;
    } else if (controller == 98) {
        ch->nrpnLsb = value & 0x7F;
        ch->selectorMode = 2;
    } else if (controller == 6 || controller == 38 || controller == 96 || controller == 97) {
        int32_t rpn = ((ch->rpnMsb & 0x7F) << 7) | (ch->rpnLsb & 0x7F);
        if (ch->selectorMode == 1 && rpn >= 0 && rpn < (int32_t)(sizeof(ch->rpnValues) / sizeof(ch->rpnValues[0]))) {
            if (controller == 6) {
                ch->rpnValues[rpn] = (value & 0x7F) << 7;
            } else if (controller == 38) {
                ch->rpnValues[rpn] = (ch->rpnValues[rpn] & 0xFF80) | (value & 0x7F);
            } else if (controller == 96) {
                ch->rpnValues[rpn] = (ch->rpnValues[rpn] + (value & 0x7F)) & 0xFFFF;
            } else {
                ch->rpnValues[rpn] = (ch->rpnValues[rpn] - (value & 0x7F)) & 0xFFFF;
            }
        }
    } else if (controller == 91) {
        ch->reverb = value & 0x7F;
    } else if (controller == 93) {
        ch->chorus = value & 0x7F;
    }
}

static int32_t dls_get_sample(DLS_Wave* wave, int64_t positionQ16) {
    if (!wave || !wave->pcm) return 0;
    int32_t index = (int32_t)(positionQ16 >> 16);
    int32_t frac = (int32_t)(positionQ16 & 0xFFFF);
    
    if (index >= wave->frames) return 0;
    
    int32_t nextIndex = index + 1 < wave->frames ? index + 1 : index;
    int32_t base = index * wave->channels;
    int32_t nextBase = nextIndex * wave->channels;
    int32_t left0 = wave->pcm[base];
    int32_t left1 = wave->pcm[nextBase];
    int32_t sample;
    if (wave->formatTag == 1 && wave->bitsPerSample == 8) {
        /* Match RetroDLS interpolateSourceSample() for 8-bit source PCM. */
        sample = left0 + (((((left1 - left0) >> 8) * frac) >> 8) & ~0xFF);
    } else {
        sample = left0 + (((frac >> 1) * (left1 - left0)) >> 15);
    }

    /* RetroDLS renders the source to mono before applying region/channel pan. */
    if (wave->channels == 2) {
        int32_t right0 = wave->pcm[base + 1];
        int32_t right1 = wave->pcm[nextBase + 1];
        int32_t right = wave->formatTag == 1 && wave->bitsPerSample == 8
            ? right0 + (((((right1 - right0) >> 8) * frac) >> 8) & ~0xFF)
            : right0 + (((frac >> 1) * (right1 - right0)) >> 15);
        sample = (sample / 2) + (right / 2);
    }
    return sample;
}

void GM_DLS_RenderAudioSlice(GM_Song* pSong, int32_t* pBuffer, int32_t* pReverbBuffer, int32_t* pChorusBuffer, uint32_t frames) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    
    for (uint32_t f = 0; f < frames; f++) {
        int64_t leftOut = 0;
        int64_t rightOut = 0;
        int64_t revOut = 0;
        int64_t choOut = 0;
        
        for (int i = 0; i < 256; i++) {
            DLS_Voice* v = &synth->voices[i];
            if (!v->active) continue;
            
            // Advance state
            if (v->controlFramesUntilTick <= 0) {
                DLS_ChannelState* ch = v->channelState;
                v->sustainSnapshot = ch->sustain;
                if (!v->keyHeld && !v->sustainSnapshot) {
                    dls_env_release(&v->envelope, v->envelope.releaseMicros);
                    dls_env_release(&v->eg2Envelope, v->eg2Envelope.releaseMicros);
                }

                int32_t env1 = dls_env_next(&v->envelope, v->controlBlockFrames);
                dls_env_next(&v->eg2Envelope, v->controlBlockFrames);
                dls_lfo_next(&v->vibratoLfo, v->controlBlockFrames);
                dls_lfo_next(&v->modulationLfo, v->controlBlockFrames);
                if (v->envelope.finished) {
                    v->active = false;
                    continue;
                }
                
                int32_t runtimePitch = 0;
                int32_t gainAttenuation = 0;
                int32_t filterCutoffDelta = 0;
                int32_t filterResonanceDelta = 0;
                int32_t panOffset = v->basePanOffset;
                int32_t reverbSend = v->baseReverbSend;
                int32_t chorusSend = v->baseChorusSend;
                dls_apply_runtime_connections(v->articulation, v, &runtimePitch, &gainAttenuation,
                                              &panOffset, &reverbSend, &chorusSend,
                                              &filterCutoffDelta, &filterResonanceDelta);

                /* Fallback: if DLS-specific bend state never moved, consume legacy channel bend
                   (8.8 semitone units) so pitch wheel still affects DLS voices. */
                if (ch->pitchBend == 0x2000 && v->channel >= 0 && v->channel < MAX_CHANNELS && pSong) {
                    int32_t legacyBend = (int32_t)pSong->channelBend[v->channel];
                    if (legacyBend != 0) {
                        runtimePitch += (legacyBend << 8); /* 8.8 -> 16.16 semitone */
                    }
                }
                
                if (v->filterEnabled) {
                    dls_filter_update(&v->filter, filterCutoffDelta, filterResonanceDelta);
                }
                
                int32_t gainQ16 = v->baseGainQ16;
                bool voiceQuirks = dls_bank_quirks(v->parentBank);
                if (voiceQuirks) {
                    if (gainAttenuation < 0) {
                        gainQ16 = DLS_FP_MUL(gainQ16, dls_exp10_q16(gainAttenuation / 20));
                    }
                } else if (gainAttenuation != 0) {
                    gainQ16 = DLS_FP_MUL(gainQ16, dls_exp10_q16(gainAttenuation / 20));
                }
                gainQ16 = DLS_FP_MUL(gainQ16, env1);
                
                panOffset = dls_clamp(panOffset, -0x10000, 0x10000);
                if (v->rampInitialized) {
                    v->leftGain = v->targetLeftGain;
                    v->rightGain = v->targetRightGain;
                    v->reverbSend = v->targetReverbSend;
                    v->chorusSend = v->targetChorusSend;
                }
                v->targetLeftGain = DLS_FP_MUL(gainQ16, dls_pan_scale_q16(-panOffset, voiceQuirks));
                v->targetRightGain = DLS_FP_MUL(gainQ16, dls_pan_scale_q16(panOffset, voiceQuirks));
                
                // If articulation has no explicit reverb/chorus, apply channel-level CC#91/93 values
                // This allows reverbs 8+ (which read from songBufferReverb) to process DLS audio
                if (reverbSend == 0 && ch->reverb > 0) {
                    reverbSend = (ch->reverb & 0x7F) << 9;  // Convert 0-127 to Q16 (0x0-0x3F80)
                }
                if (chorusSend == 0 && ch->chorus > 0) {
                    chorusSend = (ch->chorus & 0x7F) << 9;
                }
                
                v->targetReverbSend = dls_clamp(reverbSend, 0, 0x10000);
                v->targetChorusSend = dls_clamp(chorusSend, 0, 0x10000);

                if (!v->rampInitialized) {
                    v->leftGain = v->targetLeftGain;
                    v->rightGain = v->targetRightGain;
                    v->reverbSend = v->targetReverbSend;
                    v->chorusSend = v->targetChorusSend;
                    v->rampInitialized = true;
                }

                v->rampStartLeftGain = v->leftGain;
                v->rampStartRightGain = v->rightGain;
                v->rampStartReverbSend = v->reverbSend;
                v->rampStartChorusSend = v->chorusSend;
                v->rampFrame = 0;

                if (v->position == 0) {
                    debug_message("DLS Control: voice=%lld env=%d gain=%d atten=%d vol=%d expr=%d pan=%d panL=%d panR=%d left=%d right=%d\n",
                                  v->startSerial, env1, gainQ16, gainAttenuation, ch->volume,
                                  ch->expression, panOffset,
                                  dls_pan_scale_q16(-panOffset, voiceQuirks), dls_pan_scale_q16(panOffset, voiceQuirks),
                                  v->targetLeftGain, v->targetRightGain);
                }
                
                v->currentIncrement = (v->baseIncrement * dls_pitch_cents_to_ratio_q16(runtimePitch * 100)) >> 16;
                v->controlFramesUntilTick = v->controlBlockFrames;
            }
            v->controlFramesUntilTick--;
            
            // Get sample
            int32_t sample = dls_get_sample(v->wave, v->position);
            
            v->lastFiltered = false;
            if (v->filterEnabled && dls_filter_enabled(&v->filter)) {
                sample = dls_filter_next_left(&v->filter, sample);
                v->lastFiltered = true;
            }
            
            v->lastLeftSample = sample;
            v->lastRightSample = sample;
            
            // Advance position
            v->position += v->currentIncrement;
            if (v->looping && v->position >= v->loopEnd) {
                if (!v->loopUntilRelease || v->keyHeld || v->sustainSnapshot) {
                    v->position = v->loopStart + (v->position - v->loopEnd);
                } else {
                    v->looping = false;
                }
            }
            if (!v->looping && v->position >= ((int64_t)v->wave->frames << 16)) {
                v->active = false;
            }
            
            // Interpolate gain across the control block (matches Java's per-sample ramp).
            {
                int32_t rampFrames = v->controlBlockFrames;
                if (v->rampFrame < rampFrames) {
                    int32_t t = v->rampFrame;
                    int32_t n = rampFrames;
                    v->leftGain  = v->rampStartLeftGain  + (int32_t)(((int64_t)(v->targetLeftGain  - v->rampStartLeftGain)  * t) / n);
                    v->rightGain = v->rampStartRightGain + (int32_t)(((int64_t)(v->targetRightGain - v->rampStartRightGain) * t) / n);
                    v->reverbSend = v->rampStartReverbSend + (int32_t)(((int64_t)(v->targetReverbSend - v->rampStartReverbSend) * t) / n);
                    v->chorusSend = v->rampStartChorusSend + (int32_t)(((int64_t)(v->targetChorusSend - v->rampStartChorusSend) * t) / n);
                    v->rampFrame++;
                } else {
                    v->leftGain = v->targetLeftGain;
                    v->rightGain = v->targetRightGain;
                    v->reverbSend = v->targetReverbSend;
                    v->chorusSend = v->targetChorusSend;
                }

            }
            
            // Mix - keep in int64_t to avoid precision loss from intermediate int32_t cast
            // Calculate unscaled first for effects sends
            int64_t leftSampleUnscaled = ((int64_t)sample * v->leftGain) >> 16;
            int64_t rightSampleUnscaled = ((int64_t)sample * v->rightGain) >> 16;
            
            // Apply OUTPUT_SCALAR per-voice for main output
            int64_t leftSample = leftSampleUnscaled << OUTPUT_SCALAR;
            int64_t rightSample = rightSampleUnscaled << OUTPUT_SCALAR;
            
            leftOut += leftSample;
            rightOut += rightSample;
            
            if (pReverbBuffer && v->reverbSend > 0) {
                // Apply OUTPUT_SCALAR to match the signal level expected by RunNewReverb/RunNeoReverb
                // (those reverb types read from songBufferReverb which expects scaled audio, not raw samples)
                // Average L+R (>> 1) to get mono without doubling the level
                int64_t monoScaled = ((leftSampleUnscaled + rightSampleUnscaled) >> 1) << OUTPUT_SCALAR;
                revOut += (monoScaled * v->reverbSend) >> 16;
            }
            if (pChorusBuffer && v->chorusSend > 0) {
                int64_t monoScaled = ((leftSampleUnscaled + rightSampleUnscaled) >> 1) << OUTPUT_SCALAR;
                choOut += (monoScaled * v->chorusSend) >> 16;
            }
        }
        
        static int DLS_GAIN_FACTOR = 5;
        
        // Dry mix = 50% (32/64). Keep wet paths proportionally below dry for headroom.
        int64_t mixedLeft = (int64_t)pBuffer[f * 2] + ((32 * leftOut) >> DLS_GAIN_FACTOR);
        int64_t mixedRight = (int64_t)pBuffer[f * 2 + 1] + ((32 * rightOut) >> DLS_GAIN_FACTOR);
        pBuffer[f * 2] = (int32_t)(mixedLeft > INT32_MAX ? INT32_MAX : (mixedLeft < INT32_MIN ? INT32_MIN : mixedLeft));
        pBuffer[f * 2 + 1] = (int32_t)(mixedRight > INT32_MAX ? INT32_MAX : (mixedRight < INT32_MIN ? INT32_MIN : mixedRight));
        if (pReverbBuffer) {
            // Write mono reverb send (matching SF2's format: one mono sample per frame)
            int64_t mixedReverb = (int64_t)pReverbBuffer[f] + ((20 * revOut) >> DLS_GAIN_FACTOR);
            pReverbBuffer[f] = (int32_t)(mixedReverb > INT32_MAX ? INT32_MAX : (mixedReverb < INT32_MIN ? INT32_MIN : mixedReverb));
        }
        if (pChorusBuffer) {
            // Write mono chorus send (matching SF2's format: one mono sample per frame)
            int64_t mixedChorus = (int64_t)pChorusBuffer[f] + ((20 * choOut) >> DLS_GAIN_FACTOR);
            pChorusBuffer[f] = (int32_t)(mixedChorus > INT32_MAX ? INT32_MAX : (mixedChorus < INT32_MIN ? INT32_MIN : mixedChorus));
        }
    }
}

