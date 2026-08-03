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
#include "g72x.h"
#include "NeoBAE.h"
#include "GenBankBalance.h"
#include <math.h>

#if USE_MPEG_DECODER == TRUE
#include "XMPEG_BAE_API.h"
#endif
#if USE_WMA_SUPPORT == TRUE
#include "wma_decoder.h"
#endif

#if defined(__GNUC__) || defined(__clang__)
#define DLS_UNUSED_FN __attribute__((unused))
#else
#define DLS_UNUSED_FN
#endif

void GM_SetMixerDLSMode(bool isDLS) 
{
    GM_Mixer* pMixer = GM_GetCurrentMixer();
    if (pMixer) 
    {
        pMixer->isDLS = isDLS;
    }
}

bool GM_GetMixerDLSMode(void) 
{
    GM_Mixer* pMixer = GM_GetCurrentMixer();
    if (pMixer) 
    {
        return pMixer->isDLS;
    }
    return false;
}

bool g_use_mobilebae_quirks = true; /* default for newly created synths */

static bool dls_bank_quirks(const DLS_Bank* bank) {
    /* Prefer bank-local forceQuirks; otherwise use current mixer synth mode. */
    if (bank && bank->forceQuirks) return true;
    GM_Mixer* mixer = GM_GetCurrentMixer();
    if (mixer && mixer->pDLSSynth) {
        return mixer->pDLSSynth->useQuirks;
    }
    return g_use_mobilebae_quirks;
}

/* -dlscompat uses DLS2 implied defaults only for Level-2 banks. Quirks /
 * MobileBAE / pgal / eggs / XMF overlay keep the MobileBAE default path. */
static bool dls_bank_use_dls2_defaults(const DLS_Bank* bank) {
    if (!bank || dls_bank_quirks(bank) || bank->eggsArticulators) return false;
    return bank->isDLS2;
}

/* MobileBAE quirks and eggs keep historical default connections (-48 dB vel,
 * CC91/93, 50.8% pan scale). Pure compat uses true DLS1/DLS2 tables. */
static bool dls_bank_use_mobilebae_defaults(const DLS_Bank* bank) {
    if (dls_bank_quirks(bank)) return true;
    return bank && bank->eggsArticulators;
}

static bool dls_synth_has_eggs_bank(const DLS_Synth* synth) {
    if (!synth) return false;
    if (synth->banks[0] && synth->banks[0]->eggsArticulators) return true;
    if (synth->banks[1] && synth->banks[1]->eggsArticulators) return true;
    return false;
}

static bool dls_bank_spmidi(const DLS_Bank* bank) {
    /* microQ/eggs banks are incomplete GM sets; SP-MIDI substitute patches
     * (e.g. Glint PC1→0, PC24→17) change arrangement vs MobileBAE silence. */
    if (bank && bank->eggsArticulators) return false;
    return !dls_bank_quirks(bank);
}

static bool dls_synth_quirks(const DLS_Synth* synth) {
    if (!synth) return g_use_mobilebae_quirks;
    return synth->useQuirks;
}

/* Eggs under -dlscompat only. Quirks mode must keep using dls_synth_quirks()
 * / dls_bank_quirks() directly — this helper must not alter quirks paths. */
static bool dls_synth_eggs_compat_voice_mgmt(const DLS_Synth* synth) {
    if (!synth || dls_synth_quirks(synth)) return false;
    return dls_synth_has_eggs_bank(synth);
}

static DLS_UNUSED_FN int32_t dls_synth_voice_limit(const DLS_Synth* synth) {
    int32_t limit;
    if (!synth) return DLS_MAX_VOICE_POOL;
    limit = synth->maxVoices > 0 ? synth->maxVoices : DLS_MAX_VOICE_POOL;
    if (limit > DLS_MAX_VOICE_POOL) limit = DLS_MAX_VOICE_POOL;
    if (limit < 1) limit = 1;
    return limit;
}

static void dls_mip_rebuild_mask(DLS_Synth* synth);

static void dls_synth_sync_voice_limit(DLS_Synth* synth) {
    GM_Mixer* mixer;
    int32_t limit = DLS_MAX_VOICE_POOL;
    if (!synth) return;
    mixer = GM_GetCurrentMixer();
    if (mixer) {
        if (mixer->MaxNotes > 0) limit = mixer->MaxNotes;
    }
    if (limit > DLS_MAX_VOICE_POOL) limit = DLS_MAX_VOICE_POOL;
    if (limit < 1) limit = 1;
    synth->maxVoices = limit;
    dls_mip_rebuild_mask(synth);
}

static uint16_t dls_channel_index(uint16_t channel) {
    return (uint16_t)(channel & 0x0F);
}

static bool dls_mul_u32_ok(uint32_t a, uint32_t b, uint32_t* out) {
    if (a != 0 && b > UINT32_MAX / a) return false;
    if (out) *out = a * b;
    return true;
}

static void DLS_ApplyDirectConnection(DLS_Articulation* art, const DLS_Connection* connection, bool quirks);
static void dls_refresh_current_synth_for_mode(void);
static void dls_channel_reset_controllers(DLS_ChannelState* ch, bool quirks);

void GM_DLS_SetMobileBAEQuirks(bool useQuirks) 
{
    GM_Mixer* mixer = GM_GetCurrentMixer();
    g_use_mobilebae_quirks = useQuirks;
    if (mixer && mixer->pDLSSynth) {
        mixer->pDLSSynth->useQuirks = useQuirks;
    }
    dls_refresh_current_synth_for_mode();
}

bool GM_DLS_GetMobileBAEQuirks(void) 
{
    GM_Mixer* mixer = GM_GetCurrentMixer();
    if (mixer && mixer->pDLSSynth) {
        return mixer->pDLSSynth->useQuirks;
    }
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

static DLS_UNUSED_FN int32_t dls_pow2_millibels(int32_t mB) {
    if (mB < -150515) return 0;
    if (mB > 150515) return 0x7FFFFFFF;
    int32_t log2_val = (int32_t)(((int64_t)mB * 217706) >> 16);
    int32_t i_part = log2_val >> 16;
    int32_t f_part = log2_val & 0xFFFF;
    int32_t val = EXP2_TABLE[(f_part * 79) >> 16];
    if (i_part > 0) return val << i_part;
    return val >> (-i_part);
}

static DLS_UNUSED_FN int32_t dls_pow10_millibels(int32_t mB) {
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

static DLS_UNUSED_FN int32_t dls_time_cents_to_samples(int32_t tc, int32_t sampleRate) {
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

static int32_t dls_modulated_time_micros(int32_t baseMicros, int32_t valueQ16, bool eggs) {
    /* MobileBAE/quirks: unset direct EG time (0 µs) keeps modulators inert.
     * microQ/eggs (TouchWiz): many regions define attack/decay only via
     * VEL/KEY→EG modulators; treat 0 as DLS 0-timecent (= 1 s) base so those
     * modulators apply. Do not change the quirks path. */
    if (baseMicros <= 0) {
        if (!eggs) return 0;
        baseMicros = 1000000;
    }
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
    /* Match delay→attack handoff: EG1 attack math uses tickIndex in 10 ms
       units and treats the first attack sample as tickIndex=10000. Starting
       at 0 made short non-zero attacks (e.g. Windows GM Celesta ~2 ms) begin
       at gain 0 for a full control block (~10 ms soft fade-in). */
    env->tickIndex = (env->stage == DLS_ENV_ATTACK) ? 10000 : 0;
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
        /* attackTicks==0 covers authored 0 and sub-5 ms attacks (same threshold
           as EG2 / dls_micros_to_control_ticks). Checking only attackMicros==0
           left Celesta-style ~2 ms attacks on the slow ramp path. */
        if (env->attackMicros == 0 || env->attackTicks == 0) {
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
    f->baseResonance = dls_clamp(resonance, 0, FILTER_MAX_RESONANCE);
    f->resonance = f->baseResonance;
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
    int32_t cutoff = f->baseCutoff + runtimeCutoffDelta;
    int32_t resonance = dls_clamp(f->baseResonance + runtimeResonanceDelta, 0, FILTER_MAX_RESONANCE);
    if (cutoff < 0) cutoff = 0;
    if (cutoff != f->effectiveCutoff || resonance != f->resonance) {
        f->effectiveCutoff = cutoff;
        f->resonance = resonance;
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
#define CHUNK_LAR2 0x3272616C
#define CHUNK_ART1 0x31747261
#define CHUNK_ART2 0x32747261
#define CHUNK_RGN2 0x326E6772
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
    uint32_t id = PV_ReadLE32(p);
    uint32_t type = PV_ReadLE32(p + 8);
    return (id == CHUNK_RIFF && type == CHUNK_WAVE) || (id == CHUNK_LIST && type == PV_ReadLE32((const uint8_t*)"wave"));
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

#if USE_WMA_SUPPORT == TRUE
static OPErr dls_decode_wma_wave(const uint8_t* encoded, uint32_t encodedBytes, DLS_Wave* wave) {
    WMADecodeContext *ctx = NULL;
    WMAFormatInfo info;
    float *frame_out = NULL;
    int16_t *pcm = NULL;
    uint32_t pcm_cap = 0, pcm_n = 0;
    uint32_t offset;
    int block_align;
    int frame_len = 0;

    if (!encoded || encodedBytes == 0 || !wave) return BAD_FILE_TYPE;
    if (wave->blockAlign <= 0 || wave->channels <= 0 || wave->sampleRate <= 0)
        return BAD_FILE_TYPE;

    ctx = (WMADecodeContext *)XNewPtr(sizeof(WMADecodeContext));
    if (!ctx)
        return MEMORY_ERR;

    XSetMemory(&info, sizeof(info), 0);
    info.codec_id = (uint16_t)wave->formatTag;
    info.channels = (uint16_t)wave->channels;
    info.rate = (uint32_t)wave->sampleRate;
    info.bitrate = (uint32_t)((wave->avgBytesPerSec > 0)
                              ? wave->avgBytesPerSec
                              : (wave->blockAlign * wave->sampleRate / 1024));
    info.blockalign = (uint16_t)wave->blockAlign;
    info.bitspersample = 16;
    info.datalen = wave->wmaExtraLen;
    info.data = (wave->wmaExtraLen > 0) ? wave->wmaExtra : NULL;

    if (wma_decode_init(ctx, &info) < 0) {
        XDisposePtr((XPTR)ctx);
        return BAD_FILE_TYPE;
    }

    /*
     * FFmpeg skips 2 priming frames for general WMA, but DLS wsmp loop
     * points in WMA banks (e.g. WinCE GM) are authored against a 1-frame
     * skip. Using 2 leaves sustain loops spanning the silent flush frame
     * (audible gap); program 80 C4 is a clear example.
     */
    ctx->skip_frames = 1;

    block_align = ctx->block_align;
    frame_len = ctx->frame_len;
    /* Bit-reservoir packets can emit up to 15 frames. */
    frame_out = (float *)XNewPtr((uint32_t)(16 * frame_len) *
                                 (uint32_t)ctx->nb_channels * sizeof(float));
    pcm_cap = (encodedBytes / (uint32_t)block_align + 3) *
              (uint32_t)frame_len * (uint32_t)ctx->nb_channels * 4u;
    pcm = (int16_t *)XNewPtr(pcm_cap * sizeof(int16_t));
    if (!frame_out || !pcm) {
        if (pcm) XDisposePtr((XPTR)pcm);
        if (frame_out) XDisposePtr((XPTR)frame_out);
        wma_decode_close(ctx);
        XDisposePtr((XPTR)ctx);
        return MEMORY_ERR;
    }

    for (offset = 0; offset + (uint32_t)block_align <= encodedBytes; offset += (uint32_t)block_align) {
        int samples, i, ch, skip = 0;
        samples = wma_decode_superframe(ctx, encoded + offset, block_align,
                                        frame_out, 16 * frame_len);
        if (samples < 0)
            continue;
        if (samples == 0)
            continue;

        while (skip < samples && ctx->skip_frames > 0) {
            skip += frame_len;
            ctx->skip_frames--;
        }
        if (skip >= samples)
            continue;

        samples -= skip;
        if (pcm_n + (uint32_t)(samples * ctx->nb_channels) > pcm_cap) {
            uint32_t ncap = pcm_cap * 2 + (uint32_t)(samples * ctx->nb_channels);
            int16_t *np = (int16_t *)XNewPtr(ncap * sizeof(int16_t));
            if (!np) break;
            XBlockMove(pcm, np, pcm_n * sizeof(int16_t));
            XDisposePtr((XPTR)pcm);
            pcm = np;
            pcm_cap = ncap;
        }
        for (i = 0; i < samples; i++) {
            for (ch = 0; ch < ctx->nb_channels; ch++) {
                float s = frame_out[(skip + i) * ctx->nb_channels + ch] * 32767.0f;
                if (s > 32767.0f) s = 32767.0f;
                if (s < -32768.0f) s = -32768.0f;
                pcm[pcm_n++] = (int16_t)s;
            }
        }
    }

    /* Flush the overlap buffer (matches FFmpeg draining one final frame). */
    if (ctx->skip_frames <= 0) {
        int i, ch;
        uint32_t need = (uint32_t)(ctx->frame_len * ctx->nb_channels);
        if (pcm_n + need > pcm_cap) {
            uint32_t ncap = pcm_n + need;
            int16_t *np = (int16_t *)XNewPtr(ncap * sizeof(int16_t));
            if (np) {
                XBlockMove(pcm, np, pcm_n * sizeof(int16_t));
                XDisposePtr((XPTR)pcm);
                pcm = np;
                pcm_cap = ncap;
            }
        }
        if (pcm_n + need <= pcm_cap) {
            for (i = 0; i < ctx->frame_len; i++) {
                for (ch = 0; ch < ctx->nb_channels; ch++) {
                    float s = ctx->frame_out[ch][i] * 32767.0f;
                    if (s > 32767.0f) s = 32767.0f;
                    if (s < -32768.0f) s = -32768.0f;
                    pcm[pcm_n++] = (int16_t)s;
                }
            }
        }
    }

    wma_decode_close(ctx);
    XDisposePtr((XPTR)ctx);
    XDisposePtr((XPTR)frame_out);

    if (pcm_n == 0) {
        XDisposePtr((XPTR)pcm);
        return BAD_FILE_TYPE;
    }

    wave->channels = (int32_t)info.channels;
    wave->sampleRate = (int32_t)info.rate;
    wave->bitsPerSample = 16;
    wave->frames = pcm_n / (uint32_t)wave->channels;
    if (wave->factFrames >= 0 && (uint32_t)wave->factFrames < wave->frames)
        wave->frames = (uint32_t)wave->factFrames;
    wave->pcm = pcm;
    return NO_ERR;
}
#endif

static OPErr DLS_Parse_Wave_Data(const uint8_t* chunk_start, const uint8_t* chunk_end, uint32_t index, DLS_Wave* wave) {
    wave->index = index;
    wave->sample.present = false;
    wave->pcm = NULL;
    wave->frames = 0;
    wave->factFrames = -1;
    
    if (chunk_start + 12 > chunk_end) return BAD_FILE_TYPE;
    uint32_t wave_chunk_size = PV_ReadLE32(chunk_start + 4);
    const uint8_t* wave_end = chunk_start + 8 + wave_chunk_size;
    if (wave_end > chunk_end) return BAD_FILE_TYPE;
    const uint8_t* q = chunk_start + 12; // skip LIST <size> wave
    
    const uint8_t* pcm_data = NULL;
    uint32_t pcm_size = 0;

    while (q + 8 <= wave_end) {
        uint32_t id = PV_ReadLE32(q);
        uint32_t chunk_size = PV_ReadLE32(q + 4);
        const uint8_t* body = q + 8;
        uint32_t padded_size = (chunk_size + 1) & ~1;
        if (body + padded_size > wave_end) break;

        if (id == CHUNK_FMT) {
            if (chunk_size >= 16) {
                wave->formatTag = PV_ReadLE16(body);
                wave->channels = PV_ReadLE16(body + 2);
                wave->sampleRate = PV_ReadLE32(body + 4);
                wave->avgBytesPerSec = (int32_t)PV_ReadLE32(body + 8);
                wave->blockAlign = PV_ReadLE16(body + 12);
                wave->bitsPerSample = PV_ReadLE16(body + 14);
                wave->wmaExtraLen = 0;
                if (wave->formatTag == 85) {
                    /* WAVE_FORMAT_MPEGLAYER3: require cbSize=12 and decode to 16-bit PCM. */
                    if (chunk_size < 30 || PV_ReadLE16(body + 16) != 12) {
                        return BAD_FILE_TYPE;
                    }
                    wave->bitsPerSample = 16;
#if USE_WMA_SUPPORT == TRUE
                } else if (wave->formatTag == 0x0160 || wave->formatTag == 0x0161) {
                    /* MSAUDIO1/2: keep WAVEFORMATEX extradata for decoder flags. */
                    if (chunk_size >= 18) {
                        uint16_t cb = PV_ReadLE16(body + 16);
                        if (cb > 0 && chunk_size >= 18U + cb) {
                            uint16_t copy = cb;
                            if (copy > sizeof(wave->wmaExtra))
                                copy = (uint16_t)sizeof(wave->wmaExtra);
                            XBlockMove((XPTR)(body + 18), wave->wmaExtra, copy);
                            wave->wmaExtraLen = copy;
                        }
                    }
                    wave->bitsPerSample = 16;
#endif
                }
            }
        } else if (id == CHUNK_DATA) {
            pcm_data = body;
            pcm_size = chunk_size;
        } else if (id == PV_ReadLE32((const uint8_t*)"fact")) {
            if (chunk_size >= 4) {
                wave->factFrames = PV_ReadLE32(body);
            }
        } else if (id == CHUNK_WSMP) {
            if (chunk_size >= 20) {
                wave->sample.present = true;
                wave->sample.unityNote = PV_ReadLE16(body + 4);
                wave->sample.fineTuneCents = (int16_t)PV_ReadLE16(body + 6);
                wave->sample.attenuation = (int32_t)PV_ReadLE32(body + 8);
                uint32_t cSampleLoops = PV_ReadLE32(body + 16);
                wave->sample.loopMode = DLS_LOOP_NONE;
                wave->sample.loopStart = 0;
                wave->sample.loopEndInclusive = -1;
                wave->sample.loopUntilRelease = false;
                if (cSampleLoops > 0 && chunk_size >= 20 + cSampleLoops * 16) {
                    const uint8_t* loop = body + 20;
                    uint32_t loopType = PV_ReadLE32(loop + 4);
                    uint32_t loopStart = PV_ReadLE32(loop + 8);
                    uint32_t loopLength = PV_ReadLE32(loop + 12);
                    if (loopLength != 0) {
                        wave->sample.loopStart = loopStart;
                        wave->sample.loopEndInclusive = (int32_t)(loopStart + loopLength - 1);
                        wave->sample.loopMode = DLS_LOOP_FORWARD;
                        wave->sample.loopUntilRelease = (loopType == 1);
                    }
                }
            }
        } else if (id == PV_ReadLE32((const uint8_t*)"smpl") && chunk_size >= 36) {
            uint32_t loopCount = PV_ReadLE32(body + 28);
            wave->sample.present = true;
            wave->sample.unityNote = PV_ReadLE32(body + 12) & 0xFF;
            if (loopCount > 0 && chunk_size >= 60) {
                uint32_t loopType = PV_ReadLE32(body + 40);
                uint32_t loopStart = PV_ReadLE32(body + 44);
                uint32_t loopEnd = PV_ReadLE32(body + 48);
                if (loopEnd >= loopStart) {
                    wave->sample.loopMode = loopType == 0 ? DLS_LOOP_FORWARD : DLS_LOOP_NONE;
                    wave->sample.loopStart = loopStart;
                    wave->sample.loopEndInclusive = loopEnd;
                    wave->sample.loopUntilRelease = false;
                }
            }
        } else if (id == PV_ReadLE32((const uint8_t*)"inst") && chunk_size >= 7) {
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
                    wave->pcm[i] = (int16_t)((wave->formatTag == 6) ? alaw2linear(pcm_data[i]) : ulaw2linear(pcm_data[i]));
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
#if USE_WMA_SUPPORT == TRUE
        } else if (wave->formatTag == 0x0160 || wave->formatTag == 0x0161) { // WMA v1/v2
            OPErr decodeErr = dls_decode_wma_wave(pcm_data, pcm_size, wave);
            if (decodeErr != NO_ERR) {
                return decodeErr;
            }
#endif
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

static void dls_rebuild_articulation_for_mode(DLS_Articulation* art, bool quirks)
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
        DLS_ApplyDirectConnection(art, &savedConnections[i], quirks);
    }
}

static void dls_refresh_current_synth_for_mode(void)
{
    GM_Mixer* mixer = GM_GetCurrentMixer();
    DLS_Synth* synth;

    if (!mixer || !mixer->pDLSSynth) return;
    synth = mixer->pDLSSynth;
    dls_synth_sync_voice_limit(synth);

    for (int bankIndex = 0; bankIndex < 2; bankIndex++) {
        DLS_Bank* bank = synth->banks[bankIndex];
        bool quirks;
        if (!bank) continue;
        /* XMF overlays always force MobileBAE quirks; main bank follows synth mode. */
        quirks = (bankIndex == 1) || bank->forceQuirks || synth->useQuirks;
        for (uint32_t i = 0; i < bank->instrumentCount; i++) {
            DLS_Instrument* instrument = &bank->instruments[i];
            dls_rebuild_articulation_for_mode(&instrument->articulation, quirks);
            for (uint32_t j = 0; j < instrument->regionCount; j++) {
                dls_rebuild_articulation_for_mode(&instrument->regions[j].articulation, quirks);
            }
        }
    }

    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
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

/* microQ stores art1 usDestination (16-bit) and lScale (32-bit) bit-reversed
 * when the ptbl header contains the 'eggs' marker (see sub_43459D84). */
static uint8_t dls_bitrev8(uint8_t x)
{
    x = (uint8_t)(((x & 0xAAu) >> 1) | ((x & 0x55u) << 1));
    x = (uint8_t)(((x & 0xCCu) >> 2) | ((x & 0x33u) << 2));
    x = (uint8_t)(((x & 0xF0u) >> 4) | ((x & 0x0Fu) << 4));
    return x;
}

static uint16_t dls_bitrev16(uint16_t x)
{
    return (uint16_t)(dls_bitrev8((uint8_t)(x & 0xFFu)) |
                      ((uint16_t)dls_bitrev8((uint8_t)(x >> 8)) << 8));
}

static uint32_t dls_bitrev32(uint32_t x)
{
    x = ((x & 0xAAAAAAAAu) >> 1) | ((x & 0x55555555u) << 1);
    x = ((x & 0xCCCCCCCCu) >> 2) | ((x & 0x33333333u) << 2);
    x = ((x & 0xF0F0F0F0u) >> 4) | ((x & 0x0F0F0F0Fu) << 4);
    x = ((x & 0xFF00FF00u) >> 8) | ((x & 0x00FF00FFu) << 8);
    x = (x >> 16) | (x << 16);
    return x;
}

/* Eggs wlnk.ulTableIndex: bit-reverse each byte, preserve byte order
 * (unlike dls_bitrev32, which reverses the whole word). */
static uint32_t dls_bitrev32_per_byte(uint32_t x)
{
    return (uint32_t)dls_bitrev8((uint8_t)(x)) |
           ((uint32_t)dls_bitrev8((uint8_t)(x >> 8)) << 8) |
           ((uint32_t)dls_bitrev8((uint8_t)(x >> 16)) << 16) |
           ((uint32_t)dls_bitrev8((uint8_t)(x >> 24)) << 24);
}

#define DLS_EGGS_MARKER 0x73676765u /* 'eggs' LE */

static OPErr DLS_Parse_ArticulationChunk(const uint8_t* body, uint32_t size, DLS_Articulation* art, bool eggsArticulators) {
    if (size < 8) return BAD_FILE_TYPE;
    uint32_t count = PV_ReadLE32(body + 4);
    uint32_t connBytes;
    if (!dls_mul_u32_ok(count, 12, &connBytes)) return BAD_FILE_TYPE;
    if (connBytes > size - 8) return BAD_FILE_TYPE;
    
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
            connection->source = PV_ReadLE16(p);
            connection->control = PV_ReadLE16(p + 2);
            connection->destination = PV_ReadLE16(p + 4);
            connection->transform = PV_ReadLE16(p + 6);
            connection->scale = (int32_t)PV_ReadLE32(p + 8);
            if (eggsArticulators) {
                connection->destination = dls_bitrev16(connection->destination);
                connection->scale = (int32_t)dls_bitrev32((uint32_t)connection->scale);
            }
            DLS_ApplyDirectConnection(art, connection, g_use_mobilebae_quirks);
            /* refreshed later per-synth via dls_refresh_current_synth_for_mode() */
        }
        art->runtimeConnections = connections;
        art->connectionCount = new_count;
    }
    return NO_ERR;
}

static OPErr DLS_Parse_ArticulationList(const uint8_t* start, const uint8_t* end, DLS_Articulation* art,
                                         bool eggsArticulators, DLS_Bank* bank) {
    const uint8_t* p = start;
    while (p + 8 <= end) {
        uint32_t id = PV_ReadLE32(p);
        uint32_t size = PV_ReadLE32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        
        if (id == CHUNK_ART1 || id == CHUNK_ART2) {
            if (id == CHUNK_ART2 && bank) {
                bank->isDLS2 = true;
            }
            DLS_Parse_ArticulationChunk(body, size, art, eggsArticulators);
        }
        p += 8 + padded_size;
    }

    return NO_ERR;
}

static OPErr DLS_Parse_Region(const uint8_t* start, const uint8_t* end, bool level2, DLS_Region* region,
                               bool eggsArticulators, DLS_Bank* bank) {
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
        uint32_t id = PV_ReadLE32(p);
        uint32_t size = PV_ReadLE32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        if (body + padded_size > end) break;
        
        if (id == CHUNK_RGNH && size >= 12) {
            region->keyLow = PV_ReadLE16(body);
            region->keyHigh = PV_ReadLE16(body + 2);
            region->velocityLow = PV_ReadLE16(body + 4);
            region->velocityHigh = PV_ReadLE16(body + 6);
            region->options = PV_ReadLE16(body + 8);
            region->keyGroup = PV_ReadLE16(body + 10);
        } else if (id == PV_ReadLE32((const uint8_t*)"wlnk") && size >= 12) {
            uint32_t tableIndex = PV_ReadLE32(body + 8);
            region->waveLinkOptions = PV_ReadLE16(body);
            region->phaseGroup = PV_ReadLE16(body + 2);
            region->channel = PV_ReadLE32(body + 4);
            /* microQ eggs: ulTableIndex is stored with per-byte bitrev and is a
             * direct ptbl cue / wave index (sub_4345A1F8). */
            if (eggsArticulators) {
                tableIndex = dls_bitrev32_per_byte(tableIndex);
            }
            region->tableIndex = (int32_t)tableIndex;
        } else if (id == CHUNK_WSMP) {
            if (size >= 20) {
                region->sample.present = true;
                region->sample.unityNote = PV_ReadLE16(body + 4);
                region->sample.fineTuneCents = (int16_t)PV_ReadLE16(body + 6);
                region->sample.attenuation = (int32_t)PV_ReadLE32(body + 8);
                uint32_t cSampleLoops = PV_ReadLE32(body + 16);
                region->sample.loopMode = DLS_LOOP_NONE;
                region->sample.loopStart = 0;
                region->sample.loopEndInclusive = -1;
                region->sample.loopUntilRelease = false;
                if (cSampleLoops > 0 && size >= 20 + cSampleLoops * 16) {
                    const uint8_t* loop = body + 20;
                    uint32_t loopType = PV_ReadLE32(loop + 4);
                    uint32_t loopStart = PV_ReadLE32(loop + 8);
                    uint32_t loopLength = PV_ReadLE32(loop + 12);
                    if (loopLength != 0) {
                        region->sample.loopStart = loopStart;
                        region->sample.loopEndInclusive = (int32_t)(loopStart + loopLength - 1);
                        region->sample.loopMode = DLS_LOOP_FORWARD;
                        region->sample.loopUntilRelease = (loopType == 1);
                    }
                }
            }
        } else if (id == CHUNK_LIST && size >= 4 &&
                   (PV_ReadLE32(body) == CHUNK_LART || PV_ReadLE32(body) == CHUNK_LAR2)) {
            if (PV_ReadLE32(body) == CHUNK_LAR2 && bank) {
                bank->isDLS2 = true;
            }
            region->ownsArticulation = true;
            DLS_Parse_ArticulationList(body + 4, body + size, &region->articulation, eggsArticulators, bank);
        }
        
        p += 8 + padded_size;
    }
    return NO_ERR;
}

static OPErr DLS_Parse_Regions(const uint8_t* start, const uint8_t* end, DLS_Instrument* inst, bool eggsArticulators) {
    uint32_t index = 0;
    const uint8_t* p = start;
    DLS_Bank* bank = inst ? inst->parentBank : NULL;
    while (p + 8 <= end && index < inst->regionCount) {
        uint32_t id = PV_ReadLE32(p);
        uint32_t size = PV_ReadLE32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        if (body + padded_size > end) break;
        
        if (id == CHUNK_LIST && size >= 4) {
            uint32_t list_type = PV_ReadLE32(body);
            if (list_type == PV_ReadLE32((const uint8_t*)"rgn ") || list_type == CHUNK_RGN2) {
                if (list_type == CHUNK_RGN2 && bank) {
                    bank->isDLS2 = true;
                }
                DLS_Parse_Region(body + 4, body + size, list_type == CHUNK_RGN2, &inst->regions[index],
                                 eggsArticulators, bank);
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

static bool dls_parse_connection_same_key(const DLS_Connection* a, const DLS_Connection* b)
{
    return a->source == b->source &&
           a->control == b->control &&
           a->destination == b->destination;
}

/* Build the effective region articulation.  Instrument connections are
   inherited; a region connection with the same source/control/destination
   replaces the instrument connection.  This is both DLS semantics and the
   merge flow seen around MobileBAE sub_11F4670/sub_11F4720. */
static OPErr dls_merge_region_articulation(const DLS_Articulation* instrumentArt,
                                           DLS_Region* region)
{
    DLS_Connection* regionConnections = region->articulation.runtimeConnections;
    uint16_t regionCount = region->articulation.connectionCount;
    uint32_t capacity = (uint32_t)instrumentArt->connectionCount + regionCount;
    DLS_Connection* merged = NULL;
    uint32_t mergedCount = 0;

    if (capacity > UINT16_MAX) return BAD_FILE_TYPE;
    if (capacity > 0) {
        merged = (DLS_Connection*)XNewPtr(capacity * sizeof(*merged));
        if (!merged) return MEMORY_ERR;
    }

    for (uint32_t i = 0; i < instrumentArt->connectionCount; i++) {
        bool overridden = false;
        for (uint32_t j = 0; j < regionCount; j++) {
            if (dls_parse_connection_same_key(&instrumentArt->runtimeConnections[i],
                                              &regionConnections[j])) {
                overridden = true;
                break;
            }
        }
        if (!overridden) merged[mergedCount++] = instrumentArt->runtimeConnections[i];
    }
    for (uint32_t i = 0; i < regionCount; i++) {
        merged[mergedCount++] = regionConnections[i];
    }

    if (regionConnections) XDisposePtr(regionConnections);
    DLS_InitArticulation(&region->articulation);
    region->articulation.runtimeConnections = merged;
    region->articulation.connectionCount = (uint16_t)mergedCount;
    for (uint32_t i = 0; i < mergedCount; i++) {
        DLS_ApplyDirectConnection(&region->articulation, &merged[i], g_use_mobilebae_quirks);
    }
    return NO_ERR;
}

static OPErr DLS_Parse_Instrument(const uint8_t* start, const uint8_t* end, DLS_Bank* bank, DLS_Instrument* inst) {
    uint32_t declaredRegions = 0;

    DLS_InitArticulation(&inst->articulation);
    inst->parentBank = bank;

    const uint8_t* p = start;
    while (p + 8 <= end) {
        uint32_t id = PV_ReadLE32(p);
        uint32_t size = PV_ReadLE32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        if (body + padded_size > end) break;
        
        if (id == CHUNK_INSH && size >= 12) {
            declaredRegions = PV_ReadLE32(body);
            inst->rawBank = PV_ReadLE32(body + 4);
            inst->rawInstrument = PV_ReadLE32(body + 8);
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
        } else if (id == CHUNK_LIST && size >= 4 && PV_ReadLE32(body) == CHUNK_LRGN) {
            if (inst->regions) {
                DLS_Parse_Regions(body + 4, body + size, inst, bank->eggsArticulators);
            }
        } else if (id == CHUNK_LIST && size >= 4 &&
                   (PV_ReadLE32(body) == CHUNK_LART || PV_ReadLE32(body) == CHUNK_LAR2)) {
            if (PV_ReadLE32(body) == CHUNK_LAR2) {
                bank->isDLS2 = true;
            }
            DLS_Parse_ArticulationList(body + 4, body + size, &inst->articulation, bank->eggsArticulators, bank);
        }

        p += 8 + padded_size;
    }

    /* Every region receives an independent effective articulation. */
    for (uint32_t i = 0; i < inst->regionCount; i++) {
        DLS_Region* region = &inst->regions[i];
        OPErr mergeErr = dls_merge_region_articulation(&inst->articulation, region);
        if (mergeErr != NO_ERR) return mergeErr;
        region->ownsArticulation = true;
    }
    return NO_ERR;
}

static OPErr DLS_Parse_Lins(DLS_ParserState* state, DLS_Bank* bank) {
    if (bank->declaredInstrumentCount == 0) return NO_ERR;
    
    uint32_t index = 0;
    const uint8_t* p = state->start;
    while (p + 8 <= state->end && index < bank->declaredInstrumentCount) {
        uint32_t id = PV_ReadLE32(p);
        uint32_t size = PV_ReadLE32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded_size = (size + 1) & ~1;
        if (body + padded_size > state->end) break;
        
        if (id == CHUNK_LIST && size >= 4 && PV_ReadLE32(body) == CHUNK_INS) {
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
    uint32_t versionMarker = size >= 4 ? PV_ReadLE32(body) : (uint32_t)-1;
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
    
    uint32_t count = PV_ReadLE32(body + countOffset);
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
        uint16_t fromBank = PV_ReadLE16(p);
        uint16_t fromProgram = PV_ReadLE16(p + 2) & 0x7F;
        uint16_t toBank = PV_ReadLE16(p + 4);
        uint16_t toProgram = PV_ReadLE16(p + 6) & 0x7F;
        
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

/* microQ ROM percussion key gap-fill (fls_seg1 VA 0x4585D7F5, keys 27..87).
 * sub_43459CE8 fills empty drum-key slots from this table after rgnh map build.
 * Only missing keys are aliased; present keys stay identity (unlike always-on PGAL). */
static const uint8_t g_microq_perc_key_remap_27_87[61] = {
    42, 40, 42, 75, 42, 75, 42, 51, /* 27-34 */
    36, 36, 75, 40, 54, 40, 45, 42, /* 35-42 */
    45, 42, 45, 46, 45, 50, 49, 50, /* 43-50 */
    51, 51, 51, 54, 46, 75, 49, 46, /* 51-58 */
    51, 62, 64, 62, 64, 64, 62, 64, /* 59-66 */
    75, 75, 70, 70, 42, 46, 70, 46, /* 67-74 */
    75, 75, 75, 62, 64, 42, 46, 70, /* 75-82 */
    51, 51, 75, 45, 45              /* 83-87 */
};

static void DLS_Install_MicroQPercKeyAliases(DLS_Bank* bank) {
    bool hasRegion[128];
    bool anyAlias = false;
    int i;

    if (!bank || !bank->eggsArticulators || bank->hasPercussionKeyAliases) {
        return;
    }

    for (i = 0; i < 128; i++) {
        hasRegion[i] = false;
        bank->percussionKeyAliases[i] = i;
    }

    for (uint32_t ii = 0; ii < bank->instrumentCount; ii++) {
        DLS_Instrument* inst = &bank->instruments[ii];
        if (!inst->drum && (inst->bankMsb & 0x7F) != 120) {
            continue;
        }
        for (uint32_t r = 0; r < inst->regionCount; r++) {
            DLS_Region* region = &inst->regions[r];
            int32_t lo = region->keyLow & 0x7F;
            int32_t hi = region->keyHigh & 0x7F;
            int32_t k;
            if (lo > hi) {
                int32_t tmp = lo;
                lo = hi;
                hi = tmp;
            }
            for (k = lo; k <= hi; k++) {
                hasRegion[k] = true;
            }
        }
    }

    for (i = 27; i <= 87; i++) {
        if (!hasRegion[i]) {
            int32_t alias = g_microq_perc_key_remap_27_87[i - 27] & 0x7F;
            bank->percussionKeyAliases[i] = alias;
            if (alias != i) {
                anyAlias = true;
            }
        }
    }

    if (anyAlias) {
        bank->hasPercussionKeyAliases = true;
        debug_message("DLS Parser: installed microQ percussion key aliases (eggs)\n");
    }
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
            uint32_t chunk_size = PV_ReadLE32(p + 4);
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
                uint32_t chunk_size = PV_ReadLE32(p + 4);
                p += 8 + ((chunk_size + 1) & ~1);
            }
        }
    }
    return NO_ERR;
}

/* Walk RIFF/LIST structure; any art2/lar2/rgn2 marks the bank as Level 2. */
static void DLS_DetectLevel2Chunks(const uint8_t* start, const uint8_t* end, DLS_Bank* bank)
{
    const uint8_t* p = start;

    if (!bank || bank->isDLS2 || !start || start >= end) return;

    while (p + 8 <= end && !bank->isDLS2) {
        uint32_t id = PV_ReadLE32(p);
        uint32_t size = PV_ReadLE32(p + 4);
        const uint8_t* body = p + 8;
        uint32_t padded = (size + 1) & ~1;

        if (body + padded > end) break;

        if (id == CHUNK_ART2 || id == CHUNK_LAR2 || id == CHUNK_RGN2) {
            bank->isDLS2 = true;
            return;
        }
        if (id == CHUNK_LIST && size >= 4) {
            uint32_t listType = PV_ReadLE32(body);
            if (listType == CHUNK_LAR2 || listType == CHUNK_RGN2) {
                bank->isDLS2 = true;
                return;
            }
            DLS_DetectLevel2Chunks(body + 4, body + size, bank);
        }
        p = body + padded;
    }
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

    uint32_t riff_id = PV_ReadLE32(root_state.pos);
    if (riff_id != CHUNK_RIFF) {
        GM_UnloadDLSBank(bank);
        *ppBank = NULL;
        return BAD_FILE_TYPE;
    }
    
    // uint32_t riff_size = PV_ReadLE32(root_state.pos + 4);
    uint32_t dls_id = PV_ReadLE32(root_state.pos + 8);
    if (dls_id != CHUNK_DLS && dls_id != PV_ReadLE32((const uint8_t*)"DLSM")) {
        GM_UnloadDLSBank(bank);
        *ppBank = NULL;
        return BAD_FILE_TYPE;
    }
    bank->isDLSM = dls_id == PV_ReadLE32((const uint8_t*)"DLSM");

    root_state.pos += 12;
    DLS_DetectLevel2Chunks(root_state.pos, root_state.end, bank);
    debug_message("DLS Parser: bank level=%s\n", bank->isDLS2 ? "DLS2" : "DLS1");

    DLS_ParserState lins_state = {NULL, NULL, NULL};
    DLS_ParserState wvpl_state = {NULL, NULL, NULL};
    DLS_ParserState ptbl_state = {NULL, NULL, NULL};
    DLS_ParserState pgal_state = {NULL, NULL, NULL};

    while (root_state.pos + 8 <= root_state.end) {
        uint32_t chunk_id = PV_ReadLE32(root_state.pos);
        uint32_t chunk_size = PV_ReadLE32(root_state.pos + 4);
        const uint8_t* chunk_data = root_state.pos + 8;
        
        // chunk sizes are padded to even length
        uint32_t padded_size = (chunk_size + 1) & ~1;
        if (chunk_data + padded_size > root_state.end) break;
        
        if (chunk_id == CHUNK_LIST) {
            uint32_t list_type = PV_ReadLE32(chunk_data);
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
        } else if (chunk_id == PV_ReadLE32((const uint8_t*)"pgal")) {
            pgal_state.start = chunk_data;
            pgal_state.pos = chunk_data;
            pgal_state.end = chunk_data + chunk_size;
            /* pgal marks a MobileBAE bank: force quirks even under -dlscompat. */
            bank->hasPgal = true;
            bank->isMobileBAE = true;
            bank->forceQuirks = true;
        } else if (chunk_id == PV_ReadLE32((const uint8_t*)"colh")) {
            bank->declaredInstrumentCount = PV_ReadLE32(chunk_data);
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
        uint32_t ptblSize = (uint32_t)(ptbl_state.end - ptbl_state.start);
        uint32_t cbSize = PV_ReadLE32(ptbl_state.start);
        uint32_t cueBytes = 0;

        /* DLS: cues begin at +cbSize. microQ/QSound may use cbSize>8 with an
         * 'eggs' marker (bit-reversed) in the extension — see KM380 microQ. */
        if (cbSize < 8 || cbSize > ptblSize) {
            cbSize = 8;
        }
        for (uint32_t off = 8; off + 4 <= cbSize && off + 4 <= ptblSize; off += 4) {
            if (dls_bitrev32(PV_ReadLE32(ptbl_state.start + off)) == DLS_EGGS_MARKER) {
                bank->eggsArticulators = true;
                /* QSound hid the microQ layout in a bit-reversed "eggs" tag — scrambled eggs. */
                debug_message("DLS Parser: scrambled eggs detected (microQ)\n");
                break;
            }
        }

        poolOffsetCount = PV_ReadLE32(ptbl_state.start + 4);
        if (!dls_mul_u32_ok(poolOffsetCount, 4, &cueBytes)) {
            poolOffsetCount = 0;
        }
        if (poolOffsetCount > 0 &&
            cbSize + cueBytes <= ptblSize) {
            poolOffsets = (uint32_t*)XNewPtr(poolOffsetCount * sizeof(uint32_t));
            if (poolOffsets) {
                for (uint32_t i = 0; i < poolOffsetCount; i++) {
                    poolOffsets[i] = PV_ReadLE32(ptbl_state.start + cbSize + i * 4);
                }
            }
        }
    }
    
    /* Bake articulations with MobileBAE quirks when pgal is present, matching
     * XMF-overlay load (forceQuirks banks must not inherit -dlscompat bake). */
    {
        bool savedQuirks = g_use_mobilebae_quirks;
        if (bank->forceQuirks) {
            g_use_mobilebae_quirks = true;
        }
        if (lins_state.start) {
            DLS_Parse_Lins(&lins_state, bank);
        }
        bank->instrumentCount = bank->declaredInstrumentCount;
        if (pgal_state.start) {
            OPErr pgalErr = DLS_Parse_Pgal(pgal_state.start, (uint32_t)(pgal_state.end - pgal_state.start), bank);
            if (pgalErr != NO_ERR) {
                g_use_mobilebae_quirks = savedQuirks;
                GM_UnloadDLSBank(bank);
                *ppBank = NULL;
                return pgalErr;
            }
        }
        if (wvpl_state.start) {
            DLS_Parse_Wvpl(&wvpl_state, bank, poolOffsets, poolOffsetCount);
        }
        g_use_mobilebae_quirks = savedQuirks;
    }

    /* Eggs/microQ: gap-fill missing drum keys 27..87 (no-op if pgal already set). */
    DLS_Install_MicroQPercKeyAliases(bank);

    if (poolOffsets) {
        XDisposePtr(poolOffsets);
    }

    /* Non-eggs banks only: if wlnk IDs are sparse OOR values with a unique
     * count matching waveCount, remap sorted IDs → dense indices.
     * Eggs banks already decode ulTableIndex via per-byte bitrev to a direct
     * cue index — do not remap (sorted-unique was wrong for TouchWiz). */
    if (!bank->eggsArticulators) {
        uint32_t uniqueCount = 0;
        int32_t* uniqueIds = NULL;
        bool needsRemap = false;

        for (uint32_t i = 0; i < bank->instrumentCount; i++) {
            DLS_Instrument* inst = &bank->instruments[i];
            for (uint32_t r = 0; r < inst->regionCount; r++) {
                int32_t id = inst->regions[r].tableIndex;
                if (id < 0) continue;
                if ((uint32_t)id >= bank->waveCount) needsRemap = true;
            }
        }

        if (needsRemap && bank->waveCount > 0) {
            uniqueIds = (int32_t*)XNewPtr((int32_t)(bank->waveCount * sizeof(int32_t)));
            if (uniqueIds) {
                for (uint32_t i = 0; i < bank->instrumentCount; i++) {
                    DLS_Instrument* inst = &bank->instruments[i];
                    for (uint32_t r = 0; r < inst->regionCount; r++) {
                        int32_t id = inst->regions[r].tableIndex;
                        uint32_t u;
                        if (id < 0) continue;
                        for (u = 0; u < uniqueCount; u++) {
                            if (uniqueIds[u] == id) break;
                        }
                        if (u == uniqueCount) {
                            if (uniqueCount >= bank->waveCount) {
                                uniqueCount = 0; /* abort: too many ids */
                                goto sparse_done;
                            }
                            uniqueIds[uniqueCount++] = id;
                        }
                    }
                }

                if (uniqueCount == bank->waveCount) {
                    for (uint32_t i = 1; i < uniqueCount; i++) {
                        int32_t key = uniqueIds[i];
                        uint32_t j = i;
                        while (j > 0 && uniqueIds[j - 1] > key) {
                            uniqueIds[j] = uniqueIds[j - 1];
                            j--;
                        }
                        uniqueIds[j] = key;
                    }
                    for (uint32_t i = 0; i < bank->instrumentCount; i++) {
                        DLS_Instrument* inst = &bank->instruments[i];
                        for (uint32_t r = 0; r < inst->regionCount; r++) {
                            int32_t id = inst->regions[r].tableIndex;
                            uint32_t lo = 0, hi = uniqueCount;
                            if (id < 0) continue;
                            while (lo < hi) {
                                uint32_t mid = lo + (hi - lo) / 2;
                                if (uniqueIds[mid] < id) lo = mid + 1;
                                else hi = mid;
                            }
                            if (lo < uniqueCount && uniqueIds[lo] == id) {
                                inst->regions[r].tableIndex = (int32_t)lo;
                            }
                        }
                    }
                    debug_message("DLS Parser: remapped sparse wlnk IDs (%u waves)\n", uniqueCount);
                }
            }
        }
    sparse_done:
        if (uniqueIds) XDisposePtr(uniqueIds);
    }

    debug_message("DLS Parser: instruments=%u waves=%u ptbl=%u pgal=%u eggs=%d\n",
                  bank->instrumentCount, bank->waveCount, poolOffsetCount,
                  bank->programAliasCount, (int)bank->eggsArticulators);

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
    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
        pMixer->pDLSSynth->voices[i].active = false;
    }
    for (int ch = 0; ch < 16; ch++) {
        DLS_ChannelState* channelState = &pMixer->pDLSSynth->channels[ch];
        XSetMemory(channelState, sizeof(*channelState), 0);
        channelState->channel = ch;
        channelState->bankMsb = (ch == 9) ? 120 : 121;
        channelState->bankLsb = 0;
        channelState->program = 0;
        dls_channel_reset_controllers(channelState, pMixer->pDLSSynth->useQuirks);
        channelState->selectedInstrument = NULL;
        channelState->selectedBankSelector = 0;
        channelState->programSelected = false;
    }

    pMixer->isDLS = true;
    GM_BankBalance_OnDlsBanksChanged(pMixer);
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
    /* XMF/MXMF embedded DLS is MobileBAE-era content — badge + quirks. */
    bank->isMobileBAE = true;

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
    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
        pMixer->pDLSSynth->voices[i].active = false;
    }
    for (int ch = 0; ch < 16; ch++) {
        DLS_ChannelState* channelState = &pMixer->pDLSSynth->channels[ch];
        channelState->selectedInstrument = NULL;
        channelState->selectedBankSelector = 0;
        channelState->programSelected = false;
    }

    pMixer->isDLS = true;
    GM_BankBalance_OnDlsBanksChanged(pMixer);
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

    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
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
    GM_BankBalance_OnDlsBanksChanged(pMixer);
}

static float PV_DLS_LoudnessInt16(const int16_t *pcm, uint32_t frames, int channels, int stride)
{
    double sumSq = 0.0;
    double peak = 0.0;
    uint32_t n = 0;
    uint32_t f;
    double thresh;

    if (!pcm || frames == 0 || channels <= 0)
        return 0.0f;
    if (stride < 1)
        stride = 1;

    for (f = 0; f < frames; f += (uint32_t)stride)
    {
        int c;
        for (c = 0; c < channels; c++)
        {
            double s = (double)pcm[f * (uint32_t)channels + (uint32_t)c] * (1.0 / 32768.0);
            double a = (s < 0.0) ? -s : s;
            if (a > peak)
                peak = a;
        }
    }

    thresh = peak * 0.10;
    for (f = 0; f < frames; f += (uint32_t)stride)
    {
        int c;
        for (c = 0; c < channels; c++)
        {
            double s = (double)pcm[f * (uint32_t)channels + (uint32_t)c] * (1.0 / 32768.0);
            double a = (s < 0.0) ? -s : s;
            if (a >= thresh)
            {
                sumSq += s * s;
                n++;
            }
        }
    }

    if (n == 0)
        return (float)peak;
    {
        float activeRms = (float)sqrt(sumSq / (double)n);
        float peakFloor = (float)(peak * 0.35);
        return (activeRms > peakFloor) ? activeRms : peakFloor;
    }
}

/* Peak sample amplitude for MIDI normalize estimates. BankBalance keeps the
 * RMS-biased metric above; peaky DLS banks (e.g. WinCE/Windows GM) otherwise
 * under-read and the normalize gain overshoots into clipping. */
static float PV_DLS_SamplePeakInt16(const int16_t *pcm, uint32_t frames, int channels, int stride)
{
    double peak = 0.0;
    uint32_t f;

    if (!pcm || frames == 0 || channels <= 0)
        return 0.0f;
    if (stride < 1)
        stride = 1;

    for (f = 0; f < frames; f += (uint32_t)stride)
    {
        int c;
        for (c = 0; c < channels; c++)
        {
            double s = (double)pcm[f * (uint32_t)channels + (uint32_t)c] * (1.0 / 32768.0);
            double a = (s < 0.0) ? -s : s;
            if (a > peak)
                peak = a;
        }
    }
    return (float)peak;
}

static float PV_DLS_MedianInPlace(float *values, int count)
{
    int i, j;
    if (count <= 0)
        return 0.0f;
    for (i = 1; i < count; i++)
    {
        float key = values[i];
        j = i - 1;
        while (j >= 0 && values[j] > key)
        {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = key;
    }
    if (count & 1)
        return values[count / 2];
    return 0.5f * (values[count / 2 - 1] + values[count / 2]);
}

float GM_DLS_MeasureBankLoudness(DLS_Bank* bank)
{
    enum { kMaxValues = 128, kProbeKey = 60, kProbeVel = 64, kStride = 8 };
    float values[kMaxValues];
    int valueCount = 0;
    uint32_t i;

    if (!bank || !bank->instruments || bank->instrumentCount == 0)
        return 0.0f;

    for (i = 0; i < bank->instrumentCount && valueCount < kMaxValues; i++)
    {
        DLS_Instrument *inst = &bank->instruments[i];
        float best = 0.0f;
        uint32_t r;

        if (inst->drum)
            continue;

        for (r = 0; r < inst->regionCount; r++)
        {
            DLS_Region *region = &inst->regions[r];
            DLS_Wave *wave;
            DLS_SampleInfo *sample;
            float rms;
            float attenLin;
            float loud;

            if (region->keyLow > kProbeKey || region->keyHigh < kProbeKey)
                continue;
            if (region->velocityLow > kProbeVel || region->velocityHigh < kProbeVel)
                continue;
            if (region->tableIndex < 0 || (uint32_t)region->tableIndex >= bank->waveCount)
                continue;

            wave = &bank->waves[region->tableIndex];
            if (!wave->pcm || wave->frames == 0)
                continue;

            sample = region->sample.present ? &region->sample : &wave->sample;
            /* Match voice path: dls_exp10_q16(attenuation / 200) ≈ 10^((att/200)/65536). */
            attenLin = (float)pow(10.0, (double)sample->attenuation / (200.0 * 65536.0));
            if (attenLin < 1.0e-6f)
                attenLin = 1.0e-6f;

            rms = PV_DLS_LoudnessInt16(wave->pcm, wave->frames, wave->channels > 0 ? wave->channels : 1, kStride);
            loud = rms * attenLin;
            if (loud > best)
                best = loud;
        }

        if (best > 1.0e-6f)
            values[valueCount++] = best;
    }

    if (valueCount <= 0)
    {
        /* Fallback: wave-only scan when region probes miss (odd bank layouts). */
        for (i = 0; i < bank->waveCount && valueCount < kMaxValues; i++)
        {
            DLS_Wave *wave = &bank->waves[i];
            float rms;
            float attenLin;
            if (!wave->pcm || wave->frames == 0)
                continue;
            attenLin = (float)pow(10.0, (double)wave->sample.attenuation / (200.0 * 65536.0));
            if (attenLin < 1.0e-6f)
                attenLin = 1.0e-6f;
            rms = PV_DLS_LoudnessInt16(wave->pcm, wave->frames, wave->channels > 0 ? wave->channels : 1, kStride);
            if (rms * attenLin > 1.0e-6f)
                values[valueCount++] = rms * attenLin;
        }
    }

    if (valueCount <= 0)
        return 0.0f;
    /* DLS bus inserts at OUTPUT_SCALAR-2 (see GM_DLS_RenderAudioSlice
     * scalar_modifier), ~12 dB below native full-scale sample RMS. */
    return PV_DLS_MedianInPlace(values, valueCount) * 0.25f;
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

/* mBAE_plus14 byte_1239164 — SP-MIDI default channel priority. */
static const uint8_t kDlsMipDefaultPriority[16] = {
    9, 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15
};

static void dls_mip_rebuild_mask(DLS_Synth* synth)
{
    if (!synth) return;
    if (!synth->mipActive || synth->mipPairCount == 0) {
        synth->mipChannelMask = 0xFFFF;
        return;
    }
    /* mBAE sub_11FDC60: enable channels whose MIP level <= device polyphony. */
    int32_t budget = synth->maxVoices > 0 ? synth->maxVoices : DLS_MAX_VOICE_POOL;
    uint16_t mask = 0;
    for (uint8_t i = 0; i < synth->mipPairCount; i++) {
        uint8_t level = synth->mipLevel[i];
        if (level == 0) break;
        if ((int32_t)level <= budget) {
            mask = (uint16_t)(mask | (uint16_t)(1u << (synth->mipChannel[i] & 15)));
        }
    }
    synth->mipChannelMask = mask ? mask : 0xFFFF;
}

static void dls_mip_reset(DLS_Synth* synth)
{
    if (!synth) return;
    XBlockMove((XPTR)kDlsMipDefaultPriority, synth->channelPriority, 16);
    synth->mipPairCount = 0;
    synth->mipActive = false;
    synth->mipChannelMask = 0xFFFF;
    XSetMemory(synth->mipChannel, 16, 0);
    XSetMemory(synth->mipLevel, 16, 0);
}

static void dls_mip_apply_message(DLS_Synth* synth, const uint8_t* pairs, uint32_t pairBytes)
{
    uint8_t ordered[16];
    uint8_t count = 0;
    uint32_t remaining;
    const uint8_t* p;
    uint8_t ch;

    if (!synth || !pairs) return;

    remaining = pairBytes;
    p = pairs;
    synth->mipPairCount = 0;
    while (remaining >= 2 && count < 16 && synth->mipPairCount < 16) {
        uint8_t channel = p[0];
        uint8_t level = p[1];
        if (channel < 16) {
            /* Priority order = MIP channel order (sub_11F5990). */
            ordered[count++] = channel;
            if (synth->mipPairCount > 0 && level < synth->mipLevel[synth->mipPairCount - 1]) {
                /* mBAE rejects non-increasing MIP levels (sub_11FDB90). */
                dls_mip_reset(synth);
                return;
            }
            synth->mipChannel[synth->mipPairCount] = channel;
            synth->mipLevel[synth->mipPairCount] = level;
            synth->mipPairCount++;
        }
        p += 2;
        remaining -= 2;
    }

    /* Append channels not listed (sub_11F5990). */
    for (ch = 0; ch < 16 && count < 16; ch++) {
        uint8_t i;
        bool present = false;
        for (i = 0; i < count; i++) {
            if (ordered[i] == ch) { present = true; break; }
        }
        if (!present) ordered[count++] = ch;
    }
    XBlockMove(ordered, synth->channelPriority, 16);
    synth->mipActive = true;
    dls_mip_rebuild_mask(synth);
}

OPErr GM_InitDLSSynth(DLS_Synth** ppSynth, int32_t sampleRate) {
    if (!ppSynth) return PARAM_ERR;
    *ppSynth = (DLS_Synth*)XNewPtr(sizeof(DLS_Synth));
    if (!*ppSynth) return MEMORY_ERR;
    XSetMemory(*ppSynth, sizeof(DLS_Synth), 0);
    (*ppSynth)->sampleRate = sampleRate;
    (*ppSynth)->useQuirks = g_use_mobilebae_quirks;
    (*ppSynth)->maxVoices = DLS_MAX_VOICE_POOL;
    (*ppSynth)->limiterGainQ16 = 0x10000;
    dls_mip_reset(*ppSynth);
    dls_synth_sync_voice_limit(*ppSynth);
    for (int32_t channel = 0; channel < 16; channel++) {
        (*ppSynth)->channels[channel].channel = channel;
        (*ppSynth)->channels[channel].bankMsb = channel == 9 ? 120 : 121;
        (*ppSynth)->channels[channel].bankLsb = 0;
        dls_channel_reset_controllers(&(*ppSynth)->channels[channel], (*ppSynth)->useQuirks);
    }
    return NO_ERR;
}

bool GM_IsDLSSong(GM_Song* pSong) {
    if (pSong) {
        return pSong->isDLSSong;
    }
    return false;
}

bool GM_DLS_SongNeedsRender(GM_Song* pSong) {
    int16_t i;

    if (!pSong) {
        return false;
    }
    if (pSong->isDLSSong) {
        return true;
    }
    /* Hybrid RMF/ZMF + external DLS: isDLSSong stays false so embedded INST
       routing is preserved, but DLS hole-fill channels still need a mix slice. */
    for (i = 0; i < MAX_CHANNELS; i++) {
        if (pSong->channelType[i] == CHANNEL_TYPE_DLS) {
            return true;
        }
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

bool GM_DLS_HasEggsBank(struct GM_Mixer* pMixer)
{
    DLS_Synth* synth;
    if (!pMixer || !pMixer->pDLSSynth) {
        return false;
    }
    synth = (DLS_Synth*)pMixer->pDLSSynth;
    if (synth->banks[0] && synth->banks[0]->eggsArticulators) return true;
    if (synth->banks[1] && synth->banks[1]->eggsArticulators) return true;
    return false;
}

bool GM_DLS_HasMobileBAEBank(struct GM_Mixer* pMixer)
{
    DLS_Synth* synth;
    if (!pMixer || !pMixer->pDLSSynth) {
        return false;
    }
    synth = (DLS_Synth*)pMixer->pDLSSynth;
    if (synth->banks[0] && synth->banks[0]->isMobileBAE) return true;
    if (synth->banks[1] && synth->banks[1]->isMobileBAE) return true;
    return false;
}

bool GM_DLS_HasMobileBAEMainBank(struct GM_Mixer* pMixer)
{
    DLS_Synth* synth;
    if (!pMixer || !pMixer->pDLSSynth) {
        return false;
    }
    synth = (DLS_Synth*)pMixer->pDLSSynth;
    return (synth->banks[0] && synth->banks[0]->isMobileBAE) ? true : false;
}

int GM_DLS_GetBankLevel(struct GM_Mixer* pMixer)
{
    DLS_Synth* synth;
    DLS_Bank* bank;
    if (!pMixer || !pMixer->pDLSSynth) {
        return 0;
    }
    synth = (DLS_Synth*)pMixer->pDLSSynth;
    bank = synth->banks[0] ? synth->banks[0] : synth->banks[1];
    if (!bank) {
        return 0;
    }
    return bank->isDLS2 ? 2 : 1;
}

static bool g_forced_quirks_load_active = false;
static bool g_forced_quirks_load_saved = false;

void GM_DLS_BeginForcedQuirksLoad(void)
{
    if (g_forced_quirks_load_active) {
        return;
    }
    g_forced_quirks_load_saved = g_use_mobilebae_quirks;
    g_use_mobilebae_quirks = true;
    g_forced_quirks_load_active = true;
}

void GM_DLS_EndForcedQuirksLoad(void)
{
    if (!g_forced_quirks_load_active) {
        return;
    }
    g_use_mobilebae_quirks = g_forced_quirks_load_saved;
    g_forced_quirks_load_active = false;
}

void GM_DLS_MarkMainBankMobileBAE(struct GM_Mixer* pMixer)
{
    DLS_Synth* synth;
    DLS_Bank* bank;
    if (!pMixer || !pMixer->pDLSSynth) {
        return;
    }
    synth = (DLS_Synth*)pMixer->pDLSSynth;
    bank = synth->banks[0];
    if (!bank) {
        return;
    }
    bank->isMobileBAE = true;
    bank->forceQuirks = true;
    dls_refresh_current_synth_for_mode();
}

uint16_t GM_DLS_GetActiveVoiceCount(struct GM_Mixer* pMixer) {
    if (!pMixer || !pMixer->pDLSSynth) return 0;
    DLS_Synth* synth = (DLS_Synth*)pMixer->pDLSSynth;
    uint16_t active = 0;
    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
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

    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++)
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
        dls_channel_reset_controllers(channelState, synth->useQuirks);
        channelState->selectedInstrument = NULL;
        channelState->selectedBankSelector = 0;
        channelState->programSelected = false;
    }

    synth->nextVoiceSerial = 0;
    synth->limiterGainQ16 = 0x10000;
    dls_mip_reset(synth);
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

            /* mBAE sub_11DD160 under quirks: drum miss → program 0 only. */
            {
                int32_t progStart = 0;
                int32_t progEnd = dls_bank_quirks(bank) ? 1 : 128;
                for (fallbackProgram = progStart; fallbackProgram < progEnd; fallbackProgram++) {
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

        /*
         * Mobile Sound Builder / MobileBAE custom banks in rawMode use two
         * INSH layouts; channel select normalizes both to 121:X:N:
         *   CC0=X, CC32=0 → INSH bits 8-14 = X → selector X:0:N
         *   CC0=0, CC32=X → INSH bits 0-6  = X → selector 0:X:N
         * Try both only when LSB != 0 so GM 121:0:N is not stolen.
         */
        if (bankLsb > 0) {
            selector = DLS_Selector(bankLsb, 0, program);
            inst = DLS_Bank_FindSelectorOrAlias(bank, selector);
            if (inst) {
                bool isDrum = (inst->drum || ((inst->bankMsb & 0x7F) == 120));
                if (wantDrum == isDrum) {
                    return inst;
                }
            }

            /* DLS-spec / MSB-authored LSB form (e.g. Woodland Wood Marimba). */
            selector = DLS_Selector(0, bankLsb, program);
            inst = DLS_Bank_FindSelectorOrAlias(bank, selector);
            if (inst) {
                bool isDrum = (inst->drum || ((inst->bankMsb & 0x7F) == 120));
                if (wantDrum == isDrum) {
                    return inst;
                }
            }
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

    /*
     * Do not scan the XMF/DLS overlay by program number alone. That let a
     * custom preset at 121:2:0 (MIDI CC0=2) hijack GM requests for 121:0:0.
     * Bank-aware lookup above (including MobileBAE raw CC0 fallback) is enough
     * for overlay banks; missing presets fall through to the main bank / GM.
     */

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

/* MobileBAE / quirks / eggs historical defaults. Do not change for authenticity. */
static const DLS_Connection g_dls_default_connections[] = {
    { 3,    0,     3, 0x0000,  838860800 },
    { 2,    0,     1, 0x8400,  -31457280 }, /* -48 dB (MobileBAE) */
    { 6, 0x100,     3, 0x4000,  838860800 },
    { 0x87, 0,      1, 0x8400,  -62914560 },
    { 0x8B, 0,      1, 0x8400,  -62914560 },
    { 0x101, 0,     3, 0x4000,   6553600 },
    { 0x8A, 0,      4, 0x4000,  33292288 },
    { 0xDB, 0,   0x81, 0x0000,  65536000 },
    { 0xDD, 0,   0x80, 0x0000,  65536000 }
};

/* True DLS Level 1 implied defaults for -dlscompat DLS1 banks.
 * Velocity −96 dB; CC10 50%; no CC91/93 (Level 2 only).
 * RPN2 coarse tune is applied via key-number shift (not a separate pitch row). */
static const DLS_Connection g_dlsv1_default_connections[] = {
    { 3,    0,     3, 0x0000,  838860800 }, /* Key Number to Pitch */
    { 2,    0,     1, 0x8400,  -62914560 }, /* Velocity to Gain −96 dB */
    { 6, 0x100,     3, 0x4000,  838860800 }, /* Pitch Wheel + RPN0 */
    { 0x87, 0,      1, 0x8400,  -62914560 }, /* CC7 to Gain */
    { 0x8B, 0,      1, 0x8400,  -62914560 }, /* CC11 to Gain */
    { 0x101, 0,     3, 0x4000,   6553600 }, /* RPN1 to Pitch */
    { 0x8A, 0,      4, 0x4000,  32768000 }, /* CC10 to Pan 50% */
};

/* DLS v2.2 default connections per MMA RP-025 Tables 3–6.
   Used under -dlscompat only when the loaded bank is Level 2 (isDLS2). */
static const DLS_Connection g_dlsv2_default_connections[] = {
    { 3,    0,     3, 0x0000,  838860800 }, /* Key Number to Pitch */
    { 2,    0,     1, 0x8400,  -62914560 }, /* Velocity to Gain −96 dB */
    { 6, 0x100,     3, 0x4000,  838860800 }, /* Pitch Wheel + RPN0 */
    { 0x87, 0,      1, 0x8400,  -62914560 }, /* CC7 to Gain */
    { 0x8B, 0,      1, 0x8400,  -62914560 }, /* CC11 to Gain */
    { 0x101, 0,     3, 0x4000,   6553600 }, /* RPN1 to Pitch */
    { 0x8A, 0,      4, 0x4000,  33292288 }, /* CC10 to Pan 50.8% */
    { 0xDB, 0,   0x81, 0x0000,  65536000 }, /* CC91 to Reverb */
    { 0xDD, 0,   0x80, 0x0000,  65536000 }, /* CC93 to Chorus */
};

static const size_t g_dlsv2_default_connections_count = sizeof(g_dlsv2_default_connections) / sizeof(g_dlsv2_default_connections[0]);
static const size_t g_dlsv1_default_connections_count = sizeof(g_dlsv1_default_connections) / sizeof(g_dlsv1_default_connections[0]);
static const size_t g_dls_default_connections_count = sizeof(g_dls_default_connections) / sizeof(g_dls_default_connections[0]);

/* microQ/eggs: force true 50% CC10 even when using the MobileBAE table. */
static const DLS_Connection g_eggs_cc10_pan_50 = { 0x8A, 0, 4, 0x4000, 32768000 };

static const DLS_Connection* dls_default_connection_at(const DLS_Bank* bank,
                                                        const DLS_Connection* table, size_t i) {
    const DLS_Connection* c = &table[i];
    if (bank && bank->eggsArticulators && c->source == 0x8A && c->destination == 4) {
        return &g_eggs_cc10_pan_50;
    }
    return c;
}

static bool dls_has_connection(const DLS_Articulation* art, const DLS_Connection* candidate) {
    for (uint32_t i = 0; i < art->connectionCount; i++) {
        if (dls_parse_connection_same_key(&art->runtimeConnections[i], candidate)) return true;
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
    } else if (connection->control == 8 && ch) {
        control = (ch->channelPressure & 0x7F) << 9;
    }
    return DLS_FP_MUL(DLS_FP_MUL(source, control), connection->scale);
}

static void dls_apply_note_on_connection(const DLS_Connection* connection, int32_t key, int32_t velocity,
                                         int32_t unityNote, const DLS_ChannelState* ch,
                                         int32_t* pitch, int32_t* gainMultiplier,
                                         int32_t* panOffset) {
    int32_t value = dls_note_on_connection_value_q16(connection, key, velocity, unityNote, ch);
    if (value == 0) return;
    if (connection->destination == 1) {
        *gainMultiplier = DLS_FP_MUL(*gainMultiplier, dls_exp10_q16(value / 200));
    }
    else if (connection->destination == 3) *pitch += value / 100;
    else if (connection->destination == 4) *panOffset += value / 500;
}

static void dls_apply_default_note_on_connections(const DLS_Articulation* art, const DLS_Bank* bank,
                                                   int32_t key, int32_t velocity, int32_t unityNote,
                                                   const DLS_ChannelState* ch, int32_t* pitch,
                                                   int32_t* gainMultiplier, int32_t* panOffset) {
    const DLS_Connection* table;
    size_t count;
    if (dls_bank_use_mobilebae_defaults(bank)) {
        table = g_dls_default_connections;
        count = g_dls_default_connections_count;
    } else if (dls_bank_use_dls2_defaults(bank)) {
        table = g_dlsv2_default_connections;
        count = g_dlsv2_default_connections_count;
    } else {
        table = g_dlsv1_default_connections;
        count = g_dlsv1_default_connections_count;
    }
    for (size_t i = 0; i < count; i++) {
        const DLS_Connection* def = dls_default_connection_at(bank, table, i);
        if (!dls_has_connection(art, def)) {
            dls_apply_note_on_connection(def, key, velocity, unityNote, ch,
                                         pitch, gainMultiplier, panOffset);
        }
    }
}

static void dls_apply_note_on_connections(const DLS_Articulation* art, int32_t key, int32_t velocity,
                                          int32_t unityNote, const DLS_ChannelState* ch,
                                          int32_t* pitch, int32_t* gainMultiplier,
                                          int32_t* panOffset) {
    for (uint32_t i = 0; i < art->connectionCount; i++) {
        dls_apply_note_on_connection(&art->runtimeConnections[i], key, velocity, unityNote, ch,
                                     pitch, gainMultiplier, panOffset);
    }
    const DLS_Bank* bank = ch->selectedInstrument ? ch->selectedInstrument->parentBank : NULL;
    dls_apply_default_note_on_connections(art, bank, key, velocity, unityNote, ch,
                                          pitch, gainMultiplier, panOffset);
}

static int32_t dls_runtime_connection_value_q16(const DLS_Connection* connection, const DLS_Voice* voice) {
    const DLS_ChannelState* ch = voice->channelState;
    bool voiceQuirks = dls_bank_quirks(voice->parentBank);
    int32_t source;
    int32_t control = 0x10000;

#define DLS_CC_14(msb, lsb) ((((msb) & 0x7F) << 7) | ((lsb) & 0x7F))

    switch (connection->source) {
        case 1: source = voice->modulationLfo.output; break;
        case 5: source = voice->eg2Envelope.current; break;
        case 9: source = voice->vibratoLfo.output; break;
        case 7:
            /* Plus14 quirks path does not consume source 7 in runtime routing. */
            if (voiceQuirks) return 0;
            source = (ch->channelPressure & 0x7F) << 9;
            break;
        case 8: source = (voice->channelState->keyPressure[voice->key & 0x7F] & 0x7F) << 9; break;
        case 6:
            source = (ch->pitchBend & 0x3FFF) << 2;
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
    else if (connection->control == 5) control = voice->eg2Envelope.current;
    else if (connection->control == 8) control = (voice->channelState->keyPressure[voice->key & 0x7F] & 0x7F) << 9;
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
    else if (connection->destination == 0x501) *filterResonanceDelta += value / 10;
}

static void dls_apply_runtime_connections(const DLS_Articulation* art, const DLS_Voice* voice,
                                          int32_t* runtimePitch, int32_t* gainAttenuation,
                                          int32_t* panOffset, int32_t* reverbSend, int32_t* chorusSend,
                                          int32_t* filterCutoffDelta, int32_t* filterResonanceDelta) {
    for (uint32_t i = 0; i < art->connectionCount; i++) {
        dls_apply_runtime_connection(&art->runtimeConnections[i], voice, runtimePitch, gainAttenuation,
                                     panOffset, reverbSend, chorusSend, filterCutoffDelta, filterResonanceDelta);
    }
    const DLS_Bank* bank = voice->parentBank;
    const DLS_Connection* table;
    size_t count;
    if (dls_bank_use_mobilebae_defaults(bank)) {
        table = g_dls_default_connections;
        count = g_dls_default_connections_count;
    } else if (dls_bank_use_dls2_defaults(bank)) {
        table = g_dlsv2_default_connections;
        count = g_dlsv2_default_connections_count;
    } else {
        table = g_dlsv1_default_connections;
        count = g_dlsv1_default_connections_count;
    }
    for (size_t i = 0; i < count; i++) {
        const DLS_Connection* def = dls_default_connection_at(bank, table, i);
        if (!dls_has_connection(art, def)) {
            dls_apply_runtime_connection(def, voice, runtimePitch, gainAttenuation,
                                         panOffset, reverbSend, chorusSend,
                                         filterCutoffDelta, filterResonanceDelta);
        }
    }
}

static DLS_UNUSED_FN int32_t dls_channel_value14(int32_t msb, int32_t lsb) {
    return ((msb & 0x7F) << 7) | (lsb & 0x7F);
}

static void dls_channel_reset_controllers(DLS_ChannelState* ch, bool quirks) {
    if (!ch) return;
    ch->modulation = 0;
    ch->modulationLsb = 0;
    ch->foot = 0;
    ch->footLsb = 0;
    ch->volume = quirks ? 127 : 100;
    ch->volumeLsb = quirks ? 127 : 0;
    ch->expression = 127;
    ch->expressionLsb = quirks ? 127 : 0;
    ch->pan = 64;
    ch->panLsb = 0;
    ch->sustain = false;
    ch->reverb = 40;
    ch->chorus = 0;
    ch->pitchBend = 0x2000;
    ch->portamentoTime = 0;
    ch->portamentoEnabled = false;
    ch->rpnValues[0] = 0x0100;
    ch->rpnValues[1] = 0x2000;
    ch->rpnValues[2] = 0x2000;
    ch->rpnMsb = 127;
    ch->rpnLsb = 127;
    ch->nrpnMsb = 127;
    ch->nrpnLsb = 127;
    ch->selectorMode = 0;
    ch->channelPressure = 0;
    ch->lastNote = -1;
    XSetMemory(ch->keyPressure, sizeof(ch->keyPressure), 0);
}

/* DLS/MIDI CC121 data=0: reset all controllers except Volume and Pan. */
static void dls_channel_reset_controllers_keep_vol_pan(DLS_ChannelState* ch, bool quirks) {
    int32_t volume, volumeLsb, pan, panLsb;
    if (!ch) return;
    volume = ch->volume;
    volumeLsb = ch->volumeLsb;
    pan = ch->pan;
    panLsb = ch->panLsb;
    dls_channel_reset_controllers(ch, quirks);
    ch->volume = volume;
    ch->volumeLsb = volumeLsb;
    ch->pan = pan;
    ch->panLsb = panLsb;
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

float GM_DLS_EstimateNoteLoudness(struct GM_Song* pSong, int16_t channel,
                                  int16_t note, int16_t velocity)
{
    enum { kStride = 8 };
    DLS_Synth* synth;
    DLS_ChannelState probe;
    DLS_Instrument* inst;
    int32_t bankSelector;
    int32_t program;
    int32_t regionKey;
    float sum = 0.0f;
    uint32_t r;

    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth)
        return 0.0f;
    if (channel < 0 || channel >= MAX_CHANNELS)
        return 0.0f;
    if (note < 0)
        note = 0;
    if (note > 127)
        note = 127;
    if (velocity < 1)
        velocity = 1;
    if (velocity > 127)
        velocity = 127;

    synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    XSetMemory(&probe, sizeof(probe), 0);
    probe.channel = channel;
    probe.bankMsb = pSong->channelBank[channel] & 0x7F;
    probe.bankLsb = 0;
    if (((channel == PERCUSSION_CHANNEL && pSong->channelBankMode[channel] == USE_GM_DEFAULT) ||
         pSong->channelBankMode[channel] == USE_GM_PERC_BANK) &&
        probe.bankMsb == 121)
    {
        probe.bankMsb = 120;
    }
    bankSelector = DLS_ChannelBankSelector(&probe);
    program = pSong->channelProgram[channel];
    if (program < 0)
        program = 0;
    program &= 0x7F;

    inst = DLS_Synth_FindInstrument(synth, bankSelector, program);
    if (!inst || !inst->parentBank)
        return 0.0f;

    regionKey = note;
    if ((bankSelector & 0x3F80) == (120 << 7) &&
        inst->parentBank->hasPercussionKeyAliases)
    {
        regionKey = inst->parentBank->percussionKeyAliases[note & 0x7F] & 0x7F;
    }

    for (r = 0; r < inst->regionCount; r++)
    {
        DLS_Region* region = &inst->regions[r];
        DLS_Wave* wave;
        DLS_SampleInfo* sample;
        float peak;
        float attenLin;
        float loud;

        if (regionKey < region->keyLow || regionKey > region->keyHigh)
            continue;
        if (velocity < region->velocityLow || velocity > region->velocityHigh)
            continue;
        if (region->tableIndex < 0 ||
            (uint32_t)region->tableIndex >= inst->parentBank->waveCount)
            continue;

        wave = &inst->parentBank->waves[region->tableIndex];
        if (!wave->pcm || wave->frames == 0)
            continue;

        sample = region->sample.present ? &region->sample : &wave->sample;
        attenLin = (float)pow(10.0, (double)sample->attenuation / (200.0 * 65536.0));
        if (attenLin < 1.0e-6f)
            attenLin = 1.0e-6f;
        /* Peak (not RMS): normalize cares about clip peaks; crest-heavy GM
         * DLS banks otherwise under-estimate and over-boost. Scale < 1
         * because EG/filter/path rarely hit the raw sample peak in-mix. */
        peak = PV_DLS_SamplePeakInt16(wave->pcm, wave->frames,
                                       wave->channels > 0 ? wave->channels : 1, kStride);
        loud = peak * 0.70f * attenLin;
        /* Layered regions stack (MobileBAE starts every match). */
        sum += loud;
    }

    /* DLS bus inserts at OUTPUT_SCALAR-2 (~0.25 vs full-scale native). */
    return sum * 0.25f;
}

/* midiKey: note-off / exclusivity match key.
 * pitchKey: key used for Key→Pitch and note-on EG/filter key scaling
 *           (includes RPN2 coarse tune under pure compat). */
static void dls_voice_init(DLS_Voice* v, int32_t channel, int32_t midiKey, int32_t pitchKey,
                           int32_t velocity, DLS_Region* region, DLS_Wave* wave,
                           DLS_ChannelState* ch, int32_t sampleRate) {
    /* A pool slot may be recycled directly from a killed/active voice.  Reset
       every transient (especially rampInitialized, prior gain targets, filter
       history, and control counters) so no previous note can impose a false
       fade or modulation on this note.  Voice pointers are borrowed, so no
       owned allocation is lost here. */
    XSetMemory(v, sizeof(*v), 0);

    v->channel = channel;
    v->key = midiKey;
    v->velocity = velocity;
    v->regionIndex = region->index;
    v->keyGroup = region->keyGroup;
    v->phaseGroup = region->phaseGroup;
    v->waveLinkOptions = region->waveLinkOptions;
    v->phaseLockMaster = -1;
    v->wave = wave;
    v->articulation = &region->articulation;
    v->channelState = ch;
    v->parentBank = ch->selectedInstrument ? ch->selectedInstrument->parentBank : NULL;
    
    DLS_Articulation* art = &region->articulation;
    v->connectionCount = art->connectionCount;
    v->runtimeConnections = art->runtimeConnections;
    
    int32_t notePitch = art->pitch;
    int32_t noteGainMultiplier = 0x10000;
    int32_t notePanOffset = art->pan;
    int32_t filterCutoff = art->filterCutoff == FILTER_DISABLED_CUTOFF
        ? FILTER_DISABLED_CUTOFF
        : (art->filterCutoff > FILTER_MIN_CUTOFF ? art->filterCutoff : FILTER_MIN_CUTOFF);
    DLS_SampleInfo* sample = region->sample.present ? &region->sample : &wave->sample;
    int32_t unityNote = sample->present ? sample->unityNote : 60;
    dls_apply_note_on_connections(art, pitchKey, velocity, unityNote, ch, &notePitch,
                                  &noteGainMultiplier, &notePanOffset);

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
    v->baseGainQ16 = DLS_FP_MUL(dls_exp10_q16(sample->attenuation / 200), noteGainMultiplier);
    
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
    bool eggsEg = v->parentBank && v->parentBank->eggsArticulators;

    for (uint32_t i = 0; i < art->connectionCount; i++) {
        const DLS_Connection* connection = &art->runtimeConnections[i];
        int32_t value = dls_note_on_connection_value_q16(connection, pitchKey, velocity, unityNote, ch);
        if (value == 0) continue;
        if (connection->destination == 0x20B) {
            eg1Delay = dls_modulated_time_micros(eg1Delay, value, eggsEg);
        } else if (connection->destination == 0x206) {
            eg1Attack = dls_modulated_time_micros(eg1Attack, value, eggsEg);
        } else if (connection->destination == 0x20C) {
            eg1Hold = dls_modulated_time_micros(eg1Hold, value, eggsEg);
        } else if (connection->destination == 0x207) {
            eg1Decay = dls_modulated_time_micros(eg1Decay, value, eggsEg);
        } else if (connection->destination == 0x209) {
            eg1Release = dls_modulated_time_micros(eg1Release, value, eggsEg);
        } else if (connection->destination == 0x30F) {
            eg2Delay = dls_modulated_time_micros(eg2Delay, value, eggsEg);
        } else if (connection->destination == 0x30A) {
            eg2Attack = dls_modulated_time_micros(eg2Attack, value, eggsEg);
        } else if (connection->destination == 0x310) {
            eg2Hold = dls_modulated_time_micros(eg2Hold, value, eggsEg);
        } else if (connection->destination == 0x30B) {
            eg2Decay = dls_modulated_time_micros(eg2Decay, value, eggsEg);
        } else if (connection->destination == 0x30D) {
            eg2Release = dls_modulated_time_micros(eg2Release, value, eggsEg);
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
        /* DLS1 / quirks / eggs: 50% pan table. Compat+DLS2 only: 50.8%. */
        {
            bool useDls1Pan = !dls_bank_use_dls2_defaults(v->parentBank);
            v->targetLeftGain = DLS_FP_MUL(gainQ16, dls_pan_scale_q16(-panOffset, useDls1Pan));
            v->targetRightGain = DLS_FP_MUL(gainQ16, dls_pan_scale_q16(panOffset, useDls1Pan));
        }

        /* Channel CC91/93 → NeoBAE FX sends when the articulation graph left
         * sends at 0. DLS1 has no CC91/93 default connections, but NeoBAE's
         * reverb/chorus buses still consume these sends (default CC91=40). */
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

/* MobileBAE quirks and eggs keep historical exclusivity/steal behavior.
 * Pure -dlscompat follows DLS1/DLS2 note exclusivity + static channel priority. */
static bool dls_bank_mobilebae_exclusivity(const DLS_Bank* bank) {
    if (dls_bank_quirks(bank)) return true;
    return bank && bank->eggsArticulators;
}

static bool dls_synth_use_spec_voice_alloc(const DLS_Synth* synth) {
    return synth && !dls_synth_quirks(synth) && !dls_synth_eggs_compat_voice_mgmt(synth);
}

/* channelPriority[0] = highest priority. Returns 0..15. */
static int32_t dls_channel_priority_rank(const DLS_Synth* synth, int32_t channel) {
    int32_t ch = channel & 15;
    if (!synth) return 15;
    for (int i = 0; i < 16; i++) {
        if ((synth->channelPriority[i] & 15) == ch) return i;
    }
    return 15;
}

static void dls_kill_exclusive_voices(DLS_Synth* synth, int32_t channel, int32_t key,
                                      const DLS_Region* region, const DLS_Bank* newBank) {
    if (!synth || !region) return;

    /* DLS F_RGN_OPTION_SELFNONEXCLUSIVE = 0x0001: when clear, new notes choke
     * prior same-key voices. microQ matches this (fusOptions bit0). usKeyGroup
     * is compared in full (not nibble-masked). */
    bool selfNonExclusive = (region->options & 0x0001) != 0;
    int32_t exclusiveClass = region->keyGroup;
    bool mobilebaeExcl = dls_bank_mobilebae_exclusivity(newBank);
    /* Compat Level-2 banks: exclusivity uses EG1_SHUTDOWNTIME. DLS1: Note-Off. */
    bool useDls2Excl = !mobilebaeExcl && dls_bank_use_dls2_defaults(newBank);

    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
        DLS_Voice* voice = &synth->voices[i];
        if (!voice->active) continue;

        if (mobilebaeExcl) {
            /* Quirks: always channel-scoped. Eggs+compat: same (microQ). */
            bool channelScoped = dls_bank_quirks(voice->parentBank);
            if (!channelScoped && voice->parentBank && voice->parentBank->eggsArticulators) {
                channelScoped = true;
            }
            if (channelScoped && voice->channel != channel) continue;
        } else {
            /* DLS1/DLS2: exclusivity is always MIDI-channel-scoped. */
            if (voice->channel != channel) continue;
        }

        bool sameKey = !selfNonExclusive && (voice->key == key);
        if (sameKey && !mobilebaeExcl && !voice->keyHeld) {
            /* Spec: only oscillators that have not received a Note-Off. */
            sameKey = false;
        }

        bool sameExclusiveClass = (exclusiveClass != 0) &&
                                  (voice->keyGroup == exclusiveClass);
        if (sameExclusiveClass && !mobilebaeExcl && !useDls2Excl && channel != 9) {
            /* DLS1: mutually exclusive key groups limited to channel 10. */
            sameExclusiveClass = false;
        }

        if (!(sameKey || sameExclusiveClass)) continue;

        if (mobilebaeExcl || useDls2Excl) {
            /* MobileBAE / DLS2: shutdown via EG1_SHUTDOWNTIME. */
            dls_voice_fast_kill(voice);
        } else {
            /* DLS1: exclusivity issues a Note-Off (normal EG1 release). */
            voice->keyHeld = false;
        }
    }
}

static int32_t dls_find_free_voice_index(DLS_Synth* synth) {
    /* maxVoices limits logical notes; layered regions still need independent
       physical slots in the full pool. */
    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
        if (!synth->voices[i].active) return i;
    }
    return -1;
}

/* mBAE sub_11F5A10: walk channelPriority from lowest priority (index 15) to
 * highest (index 0). Default table yields steal order 15…0 then 9 last.
 * Spec path additionally refuses to steal from strictly higher-priority channels. */
static int32_t dls_find_recyclable_voice(DLS_Synth* synth, int32_t newChannel) {
    bool mipMode = synth->mipActive;
    bool specAlloc = dls_synth_use_spec_voice_alloc(synth);
    int32_t newRank = dls_channel_priority_rank(synth, newChannel);
    int32_t voiceLimit = DLS_MAX_VOICE_POOL;

    for (int ord = 15; ord >= 0; ord--) {
        int32_t channel = synth->channelPriority[ord] & 15;
        /* DLS: notes on lower-priority channels cannot steal from higher. */
        if (specAlloc && ord < newRank) continue;
        /* Protect drums unless new note is drums, or MIP mode is active. */
        if (newChannel != 9 && channel == 9 && !mipMode) continue;

        int32_t candidate = -1;
        for (int i = 0; i < voiceLimit; i++) {
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
    /* Quirks unchanged; eggs+compat adopts the same Plus14 ch9-only pass. */
    bool quirksHeldSteal = dls_synth_quirks(synth) || dls_synth_eggs_compat_voice_mgmt(synth);
    bool specAlloc = dls_synth_use_spec_voice_alloc(synth);
    bool mipMode = synth->mipActive;
    int32_t newRank = dls_channel_priority_rank(synth, newChannel);
    int32_t voiceLimit = DLS_MAX_VOICE_POOL;

    for (int ord = 15; ord >= 0; ord--) {
        int32_t channel = synth->channelPriority[ord] & 15;
        if (specAlloc && ord < newRank) continue;
        if (newChannel != 9 && channel == 9 && !mipMode) continue;

        int32_t candidate = -1;
        int64_t priority = INT64_MAX;
        for (int i = 0; i < voiceLimit; i++) {
            DLS_Voice* voice = &synth->voices[i];
            if (quirksHeldSteal) {
                /* Plus14: only steal held percussion in this pass. */
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
    bool mipMode = synth->mipActive;
    bool specAlloc = dls_synth_use_spec_voice_alloc(synth);
    int32_t newRank = dls_channel_priority_rank(synth, newChannel);
    int32_t voiceLimit = DLS_MAX_VOICE_POOL;

    for (int ord = 15; ord >= 0; ord--) {
        int32_t channel = synth->channelPriority[ord] & 15;
        if (specAlloc && ord < newRank) continue;
        if (newChannel != 9 && channel == 9 && !mipMode) continue;

        int32_t candidate = -1;
        int64_t priority = INT64_MAX;
        for (int i = 0; i < voiceLimit; i++) {
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
    /* Quirks (and eggs+compat): never drop the note — force a slot. */
    if (!dls_synth_quirks(synth) && !dls_synth_eggs_compat_voice_mgmt(synth)) return idx;
    return idx >= 0 ? idx : 0;
}

static int32_t dls_channel_coarse_semitones(const DLS_ChannelState* ch, const DLS_Bank* bank) {
    if (!ch) return 0;
    /* DLS1: coarse tune is not supported on channel 10. */
    if (bank && !dls_bank_use_mobilebae_defaults(bank) &&
        !dls_bank_use_dls2_defaults(bank) && ch->channel == 9) {
        return 0;
    }
    return ((int16_t)((ch->rpnValues[2] & 0x3FFF) - 0x2000)) >> 7;
}

static void dls_voice_link_phase(DLS_Synth* synth, DLS_Voice* v, int32_t voiceIdx) {
    int32_t master = -1;
    if (!synth || !v || v->phaseGroup == 0) return;
    if (!dls_bank_use_dls2_defaults(v->parentBank)) return;
    /* F_WAVELINK_PHASE_MASTER: this voice is the phase reference. */
    if (v->waveLinkOptions & 0x0001) {
        v->phaseLockMaster = -1;
        return;
    }
    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
        DLS_Voice* o;
        if (i == voiceIdx) continue;
        o = &synth->voices[i];
        if (!o->active || o->noteInstanceId != v->noteInstanceId) continue;
        if (o->phaseGroup != v->phaseGroup) continue;
        if (o->waveLinkOptions & 0x0001) {
            master = i;
            break;
        }
        if (master < 0) master = i;
    }
    v->phaseLockMaster = master;
    if (master >= 0) {
        v->position = synth->voices[master].position;
        v->currentIncrement = synth->voices[master].currentIncrement;
    }
}

void GM_DLS_ProcessSysEx(GM_Song* pSong, const unsigned char* message, int32_t length)
{
    DLS_Synth* synth;
    const unsigned char* p;
    int32_t n;

    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth || !message || length < 2) {
        return;
    }
    if (message[0] != 0xF0) {
        return;
    }

    synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    p = message + 1;
    n = length - 1;
    if (n > 0 && p[n - 1] == 0xF7) {
        n--;
    }
    if (n < 1) {
        return;
    }

    /* Beatnik / mBAE: F0 00 01 0D 01 00 … — clear MIP mode, restore default
     * priority (sub_11F78D0). */
    if (n >= 5 && p[0] == 0x00 && p[1] == 0x01 && p[2] == 0x0D && p[3] == 0x01 && p[4] == 0x00) {
        dls_mip_reset(synth);
        return;
    }

    /* Universal Realtime MIP: F0 7F 7F 0B 01 [ch,mip]* … F7 (sub_11F78D0 /
     * sub_11FDB90). Device ID 7F = all-call; also accept any device ID. */
    if (n >= 4 && p[0] == 0x7F && p[2] == 0x0B && p[3] == 0x01) {
        dls_mip_apply_message(synth, p + 4, (uint32_t)(n - 4));
    }
}

void GM_DLS_ProcessNoteOn(GM_Song* pSong, uint16_t channel, uint16_t note, uint16_t velocity) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    if (velocity == 0) {
        GM_DLS_ProcessNoteOff(pSong, channel, note, 64);
        return;
    }

    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    
    channel = dls_channel_index(channel);
    /* mBAE sub_11FAC40: MIP scaleable mute — drop notes on channels outside mask. */
    if (synth->mipActive && (synth->mipChannelMask & (uint16_t)(1u << channel)) == 0) {
        return;
    }
    DLS_ChannelState* ch = &synth->channels[channel];
    ch->channel = channel;

    if (((channel == PERCUSSION_CHANNEL && pSong->channelBankMode[channel] == USE_GM_DEFAULT) || pSong->channelBankMode[channel] == USE_GM_PERC_BANK) && ch->bankMsb == 121) {
        ch->bankMsb = 120;
        ch->selectedInstrument = NULL;
        ch->programSelected = false;
    }

    if (!ch->programSelected) {
        dls_program_change(synth, ch, ch->program);
    }

    DLS_Instrument* inst = ch->selectedInstrument;
    if (!inst) {
        debug_message("DLS Synth: no instrument for bank=%d:%d (selector=%d) program=%d channel=%d\n",
                      ch->bankMsb, ch->bankLsb, ch->selectedBankSelector, ch->program, channel);
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
                int32_t coarse = dls_channel_coarse_semitones(ch, inst->parentBank);
                int32_t clampedNote = dls_clamp(note + coarse, 0, 127);
                DLS_Region* origRegion = DLS_Synth_FindRegion(inst, clampedNote, velocity);
                if (!origRegion) {
                    debug_message("DLS Synth: SP-MIDI perc key alias %d -> %d for region lookup\n",
                                  note & 0x7F, spRemap);
                    voiceNote = spRemap;
                }
            }
        }
    }

    {
        int32_t coarse = dls_channel_coarse_semitones(ch, inst->parentBank);
        int32_t regionKey = voiceNote;
        int32_t pitchKey = voiceNote;
        /* MobileBAE / eggs / DLS2: layer every match. DLS1 compat: first only. */
        bool allowLayers = dls_bank_use_mobilebae_defaults(inst->parentBank) ||
                           dls_bank_use_dls2_defaults(inst->parentBank);
        bool allowPortamento = dls_bank_use_mobilebae_defaults(inst->parentBank);
        int64_t noteInstance = ++synth->nextNoteInstance;
        int32_t startedVoices = 0;

        if (!(((ch->selectedBankSelector & 0x3F80) == (120 << 7)) &&
              inst->parentBank && inst->parentBank->hasPercussionKeyAliases)) {
            regionKey = dls_clamp(voiceNote + coarse, 0, 127);
        }
        /* Spec: RPN2 shifts the key used for articulation/pitch as well as
         * region select. Quirks/eggs keep historical pitch-from-MIDI-note. */
        if (!dls_bank_use_mobilebae_defaults(inst->parentBank)) {
            pitchKey = regionKey;
        }

        for (uint32_t regionIdx = 0; regionIdx < inst->regionCount; regionIdx++) {
            DLS_Region* region = &inst->regions[regionIdx];
            DLS_Wave* regionWave = NULL;
            int32_t voiceIdx;
            DLS_Voice* v;

            if (regionKey < region->keyLow || regionKey > region->keyHigh) continue;
            if (velocity < region->velocityLow || velocity > region->velocityHigh) continue;

            if (inst->parentBank && region->tableIndex >= 0 &&
                (uint32_t)region->tableIndex < inst->parentBank->waveCount) {
                regionWave = &inst->parentBank->waves[region->tableIndex];
            }
            if (!regionWave || !regionWave->pcm) {
                debug_message("DLS Synth: no decoded wave for instrument=%d:%d:%d key=%d region=%d table=%d\n",
                              inst->bankMsb, inst->bankLsb, inst->program, note,
                              region->index, region->tableIndex);
                continue;
            }

            /* Match sub_11F7110 exactly: exclusive handling occurs before
               allocating each matching region, including regions started by
               an earlier iteration of this same note-on. */
            dls_kill_exclusive_voices(synth, channel, voiceNote, region, inst->parentBank);

            voiceIdx = dls_select_voice_index_for_note_on(synth, channel);
            if (voiceIdx < 0) {
                break;
            }

            v = &synth->voices[voiceIdx];
            dls_voice_init(v, channel, voiceNote, pitchKey, velocity, region, regionWave,
                           ch, synth->sampleRate);
            v->startSerial = synth->nextVoiceSerial++;
            v->noteInstanceId = noteInstance;
            dls_voice_link_phase(synth, v, voiceIdx);

            /* Portamento is a MobileBAE/eggs extension, not a DLS articulator. */
            if (allowPortamento && ch->portamentoEnabled &&
                ch->lastNote >= 0 && ch->lastNote != voiceNote) {
                v->portamentoActive = true;
                /* start/target as cent offsets from the voice's base note in Q16.16 */
                int32_t centsPerSemitone = 100;
                v->portamentoStartPitch = (int64_t)(ch->lastNote - voiceNote) * centsPerSemitone * (1 << 16);
                v->portamentoTargetPitch = 0;  /* arrive at the voice's native base pitch */

                /* Calculate glide duration in samples: CC5 0 = 10 ms, CC5 127 = 2000 ms */
                int32_t glideTimeMs = 10 + (ch->portamentoTime * 1990 / 127);
                int32_t totalSamples = (glideTimeMs * synth->sampleRate) / 1000;
                if (totalSamples < 1) totalSamples = 1;
                v->portamentoTotalFrames = (int64_t)totalSamples;
                v->portamentoFramesRemaining = totalSamples;
            }

            startedVoices++;
            if (!allowLayers) break;
        }

        if (startedVoices == 0) {
            debug_message("DLS Synth: no region/wave for instrument=%d:%d:%d key=%d velocity=%d\n",
                          inst->bankMsb, inst->bankLsb, inst->program, note, velocity);
        }

        if (allowPortamento && ch->portamentoEnabled) {
            ch->lastNote = voiceNote;
        }

        return;
    }
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
                for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
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

    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
        DLS_Voice* v = &synth->voices[i];
        if (v->active && v->channel == channel && v->key == matchNote) {
            v->keyHeld = false;
        }
    }
}

void GM_DLS_AllNotesOff(GM_Song* pSong, int16_t channel, bool immediate) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    for (int i = 0; i < DLS_MAX_VOICE_POOL; i++) {
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
    DLS_ChannelState* ch = &synth->channels[channel & 0x0F];

    if (pSong->channelBankMode[channel] == USE_GM_PERC_BANK) {
        if (program == 0) {
            debug_message("DLS: PERC_BANK force 120:0 ch=%d (was bankMsb=%d)\n", channel, ch->bankMsb);
            ch->bankMsb = 120;
            ch->bankLsb = 0;
        } else {
            debug_message("DLS: PERC_BANK cleared ch=%d (program=%d)\n", channel, program);
            pSong->channelBankMode[channel] = USE_GM_DEFAULT;
        }
    } else if (((channel == PERCUSSION_CHANNEL && pSong->channelBankMode[channel] == USE_GM_DEFAULT) || pSong->channelBankMode[channel] == USE_GM_PERC_BANK) && ch->bankMsb == 121) {
        debug_message("DLS: percussion ch=%d with melodic bank 121, forcing to 120\n", channel);
        ch->bankMsb = 120;
    }

    dls_program_change(synth, ch, program);
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

/* Force active voices on the given channel to recompute runtime connections
   on the next sample instead of waiting for the next 10 ms control tick. */
static void dls_invalidate_channel_voices(DLS_Synth* synth, int32_t channelIndex) {
    for (int32_t i = 0; i < synth->maxVoices; i++) {
        DLS_Voice* voice = &synth->voices[i];
        if (voice->active && voice->channel == channelIndex) {
            voice->controlFramesUntilTick = 0;
        }
    }
}

void GM_DLS_ProcessPitchBend(GM_Song* pSong, uint16_t channel, uint16_t value) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    int32_t channelIndex = dls_channel_index(channel);
    synth->channels[channelIndex].pitchBend = value & 0x3FFF;
    dls_invalidate_channel_voices(synth, channelIndex);
}

void GM_DLS_ProcessKeyPressure(GM_Song* pSong, uint16_t channel, uint16_t key, uint16_t value) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    int32_t channelIndex = dls_channel_index(channel);
    synth->channels[channelIndex].keyPressure[key & 0x7F] = value & 0x7F;
    dls_invalidate_channel_voices(synth, channelIndex);
}

void GM_DLS_ProcessChannelPressure(GM_Song* pSong, uint16_t channel, uint16_t value) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    int32_t channelIndex = dls_channel_index(channel);
    synth->channels[channelIndex].channelPressure = value & 0x7F;
    dls_invalidate_channel_voices(synth, channelIndex);
}

void GM_DLS_ProcessController(GM_Song* pSong, uint16_t channel, uint16_t controller, uint16_t value) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    
    DLS_ChannelState* ch = &synth->channels[dls_channel_index(channel)];
    if (controller == 64) {
        ch->sustain = value >= 64;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 65) {
        /* Portamento on/off */
        ch->portamentoEnabled = (value >= 64);
        if (!ch->portamentoEnabled) {
            /* When disabling portamento, clear the last note so the next note doesn't glide */
            ch->lastNote = -1;
        }
    } else if (controller == 84) {
        /* Portamento source note number */
        if (ch->portamentoEnabled) {
            ch->lastNote = value & 0x7F;
        }
    } else if (controller == 5) {
        /* Portamento time */
        ch->portamentoTime = value & 0x7F;
    } else if (controller == 121) {
        int32_t resetVal = value & 0x7F;
        /* MobileBAE/eggs: only the internal reset sentinel 127. */
        if (dls_synth_quirks(synth) || dls_synth_eggs_compat_voice_mgmt(synth)) {
            if (resetVal == 127) {
                dls_channel_reset_controllers(ch, dls_synth_quirks(synth));
                dls_invalidate_channel_voices(synth, dls_channel_index(channel));
            }
        } else if (resetVal == 0) {
            /* DLS/MIDI: data 0 resets all except Volume and Pan. */
            dls_channel_reset_controllers_keep_vol_pan(ch, false);
            dls_invalidate_channel_voices(synth, dls_channel_index(channel));
        } else if (resetVal == 127) {
            /* Power-on defaults. */
            dls_channel_reset_controllers(ch, false);
            dls_invalidate_channel_voices(synth, dls_channel_index(channel));
        }
    } else if (controller == 120) {
        GM_DLS_AllNotesOff(pSong, channel, true);
    } else if (controller == 123) {
        GM_DLS_AllNotesOff(pSong, channel, false);
    } else if (controller >= 124 && controller <= 127) {
        /* Omni Off/On (124/125) and Mono/Poly (126/127) also imply All Notes Off. */
        if (controller == 126) {
            ch->monoMode = true;
        } else if (controller == 127) {
            ch->monoMode = false;
        }
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
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 33) {
        ch->modulationLsb = value & 0x7F;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 4) {
        ch->foot = value & 0x7F;
    } else if (controller == 36) {
        ch->footLsb = value & 0x7F;
    } else if (controller == 7) {
        ch->volume = value & 0x7F;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 39) {
        ch->volumeLsb = value & 0x7F;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 10) {
        ch->pan = value & 0x7F;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 42) {
        ch->panLsb = value & 0x7F;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 11) {
        ch->expression = value & 0x7F;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 43) {
        ch->expressionLsb = value & 0x7F;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
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
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 91) {
        ch->reverb = value & 0x7F;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    } else if (controller == 93) {
        ch->chorus = value & 0x7F;
        dls_invalidate_channel_voices(synth, dls_channel_index(channel));
    }
}

static DLS_UNUSED_FN int32_t dls_get_sample(DLS_Wave* wave, int64_t positionQ16) {
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

static void dls_get_stereo_sample(DLS_Wave* wave, int64_t positionQ16, int32_t* leftOut, int32_t* rightOut) {
    if (!wave || !wave->pcm || !leftOut || !rightOut) {
        if (leftOut) *leftOut = 0;
        if (rightOut) *rightOut = 0;
        return;
    }
    
    int32_t index = (int32_t)(positionQ16 >> 16);
    int32_t frac = (int32_t)(positionQ16 & 0xFFFF);
    
    if (index >= wave->frames) {
        *leftOut = 0;
        *rightOut = 0;
        return;
    }
    
    int32_t nextIndex = index + 1 < wave->frames ? index + 1 : index;
    int32_t base = index * wave->channels;
    int32_t nextBase = nextIndex * wave->channels;
    
    if (wave->channels == 2) {
        /* Stereo wave - interpolate left and right separately */
        int32_t left0 = wave->pcm[base];
        int32_t left1 = wave->pcm[nextBase];
        int32_t right0 = wave->pcm[base + 1];
        int32_t right1 = wave->pcm[nextBase + 1];
        
        if (wave->formatTag == 1 && wave->bitsPerSample == 8) {
            *leftOut = left0 + (((((left1 - left0) >> 8) * frac) >> 8) & ~0xFF);
            *rightOut = right0 + (((((right1 - right0) >> 8) * frac) >> 8) & ~0xFF);
        } else {
            *leftOut = left0 + (((frac >> 1) * (left1 - left0)) >> 15);
            *rightOut = right0 + (((frac >> 1) * (right1 - right0)) >> 15);
        }
    } else {
        /* Mono wave - same sample for both channels */
        int32_t left0 = wave->pcm[base];
        int32_t left1 = wave->pcm[nextBase];
        int32_t sample;
        
        if (wave->formatTag == 1 && wave->bitsPerSample == 8) {
            sample = left0 + (((((left1 - left0) >> 8) * frac) >> 8) & ~0xFF);
        } else {
            sample = left0 + (((frac >> 1) * (left1 - left0)) >> 15);
        }
        
        *leftOut = sample;
        *rightOut = sample;
    }
}

void GM_DLS_RenderAudioSlice(GM_Song* pSong, int32_t* pBuffer, int32_t* pReverbBuffer, int32_t* pChorusBuffer, uint32_t frames) {
    if (!pSong || !pSong->pMixer || !pSong->pMixer->pDLSSynth) return;
    DLS_Synth* synth = (DLS_Synth*)pSong->pMixer->pDLSSynth;
    int32_t voiceLimit = DLS_MAX_VOICE_POOL;
    int scalar_modifier = -2;
    const int dlsGainFactor = 5;
    /* Main bank and XMF overlay can have independent balance scales. */
    int32_t balMainQ16 = (int32_t)(GM_BankBalance_GetMixScale(GM_BANK_ENGINE_DLS) * 65536.0f);
    int32_t balXmfQ16 = (int32_t)(GM_BankBalance_GetMixScale(GM_BANK_ENGINE_DLS_XMF) * 65536.0f);
    if (balMainQ16 < 1) balMainQ16 = 1;
    if (balXmfQ16 < 1) balXmfQ16 = 1;

    for (uint32_t f = 0; f < frames; f++) {
        int64_t leftOut = 0;
        int64_t rightOut = 0;
        int64_t revOut = 0;
        int64_t choOut = 0;
        
        for (int i = 0; i < voiceLimit; i++) {
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
                {
                    bool useDls1Pan = !dls_bank_use_dls2_defaults(v->parentBank);
                    v->targetLeftGain = DLS_FP_MUL(gainQ16, dls_pan_scale_q16(-panOffset, useDls1Pan));
                    v->targetRightGain = DLS_FP_MUL(gainQ16, dls_pan_scale_q16(panOffset, useDls1Pan));
                }

                /* Channel CC91/93 → NeoBAE FX sends when articulator send is 0.
                 * Needed for DLS1 compat banks (no CC91/93 default connections). */
                if (reverbSend == 0 && ch->reverb > 0) {
                    reverbSend = (ch->reverb & 0x7F) << 9;
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
                    int32_t prog = ch->program;
                    int32_t msb = ch->bankMsb;
                    int32_t lsb = ch->bankLsb;
                    if (ch->selectedInstrument) {
                        prog = ch->selectedInstrument->program;
                        msb = ch->selectedInstrument->bankMsb;
                        lsb = ch->selectedInstrument->bankLsb;
                    }
                    debug_message("DLS Control: voice=%lld ch=%d key=%d vel=%d bank=%d:%d prog=%d env=%d base=%d gain=%d atten=%d vol=%d expr=%d pan=%d pitch=%d left=%d right=%d\n",
                                  v->startSerial, v->channel, v->key, v->velocity,
                                  msb, lsb, prog, env1, v->baseGainQ16, gainQ16, gainAttenuation,
                                  ch->volume, ch->expression, panOffset, runtimePitch,
                                  v->targetLeftGain, v->targetRightGain);
                }
                
                v->currentIncrement = (v->baseIncrement * dls_pitch_cents_to_ratio_q16(runtimePitch * 100)) >> 16;
                v->controlFramesUntilTick = v->controlBlockFrames;
            }
            v->controlFramesUntilTick--;
            
            /* Apply portamento glide if active.  Interpolate base increment from
               start-pitch to target-pitch, advancing one sample per frame.  Store
               the total glide duration in portamentoFramesRemaining at init so we
               can compute the linear fraction on each frame. */
            if (v->portamentoActive && v->portamentoFramesRemaining > 0) {
                int64_t elapsed = (int64_t)(v->portamentoTotalFrames - v->portamentoFramesRemaining);
                if (elapsed < 0) elapsed = 0;
                if (elapsed >= v->portamentoTotalFrames) {
                    elapsed = v->portamentoTotalFrames;
                    v->portamentoActive = false;
                }
                int64_t pitchDelta = v->portamentoTargetPitch - v->portamentoStartPitch;
                int64_t currentPitch = v->portamentoStartPitch +
                    (pitchDelta * elapsed / v->portamentoTotalFrames);

                /* currentPitch is cents in Q16.16.  Pass it through the full
                   pitch-cents-to-ratio pipeline so fractional cents are preserved. */
                v->currentIncrement = (v->baseIncrement *
                    dls_pitch_cents_to_ratio_q16((int32_t)(currentPitch))) >> 16;
                v->portamentoFramesRemaining--;
            }
            
            bool phaseSlave = false;
            int32_t leftSample, rightSample;

            /* DLS2 phase-lock: slaves follow the master's oscillator phase. */
            if (v->phaseLockMaster >= 0 && v->phaseLockMaster < DLS_MAX_VOICE_POOL) {
                DLS_Voice* master = &synth->voices[v->phaseLockMaster];
                if (master->active && master->noteInstanceId == v->noteInstanceId) {
                    v->position = master->position;
                    v->currentIncrement = master->currentIncrement;
                    phaseSlave = true;
                } else {
                    v->phaseLockMaster = -1;
                }
            }

            dls_get_stereo_sample(v->wave, v->position, &leftSample, &rightSample);

            v->lastFiltered = false;
            if (v->filterEnabled && dls_filter_enabled(&v->filter)) {
                leftSample = dls_filter_next_left(&v->filter, leftSample);
                rightSample = dls_filter_next_right(&v->filter, rightSample);
                v->lastFiltered = true;
            }

            v->lastLeftSample = leftSample;
            v->lastRightSample = rightSample;

            if (!phaseSlave) {
                v->position += v->currentIncrement;
                if (v->looping && v->position >= v->loopEnd) {
                    if (!v->loopUntilRelease || v->keyHeld || v->sustainSnapshot) {
                        v->position = v->loopStart + (v->position - v->loopEnd);
                    } else {
                        v->looping = false;
                    }
                }
            }
            if (!v->looping && v->position >= ((int64_t)v->wave->frames << 16)) {
                v->active = false;
            }
            
            /* Interpolate controller/envelope targets across the control
               block to avoid discontinuities.  Note-on itself is initialized
               directly to its first corrected EG target above, so this does
               not reintroduce the former initial soft attack. */
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
            int64_t leftSampleUnscaled = ((int64_t)leftSample * v->leftGain) >> 16;
            int64_t rightSampleUnscaled = ((int64_t)rightSample * v->rightGain) >> 16;
            
            // Apply OUTPUT_SCALAR per-voice for main output
            int64_t leftSampleScaled = leftSampleUnscaled << (OUTPUT_SCALAR + scalar_modifier);
            int64_t rightSampleScaled = rightSampleUnscaled << (OUTPUT_SCALAR + scalar_modifier);

            /* Per-voice bank balance: XMF overlay (banks[1]) vs main (banks[0]). */
            {
                int32_t voiceBalQ16 = (v->parentBank && v->parentBank == synth->banks[1])
                    ? balXmfQ16 : balMainQ16;
                leftSampleScaled = (leftSampleScaled * voiceBalQ16) >> 16;
                rightSampleScaled = (rightSampleScaled * voiceBalQ16) >> 16;
            }

            if (pSong->pMixer->channelCaptureEnabled && v->channel >= 0 && v->channel < 16) {
                int32_t sampleIndex = (int32_t)(f * 2);
                if (sampleIndex + 1 < pSong->pMixer->channelCaptureBufSamples) {
                    int32_t* channelBuffer = pSong->pMixer->channelCaptureBuf[v->channel];
                    if (channelBuffer) {
                        channelBuffer[sampleIndex] += (int32_t)((32 * leftSampleScaled) >> dlsGainFactor);
                        channelBuffer[sampleIndex + 1] += (int32_t)((32 * rightSampleScaled) >> dlsGainFactor);
                    }
                }
            }
            
            leftOut += leftSampleScaled;
            rightOut += rightSampleScaled;
            
            if (pReverbBuffer && v->reverbSend > 0) {
                // Apply OUTPUT_SCALAR to match the signal level expected by RunNewReverb/RunNeoReverb
                // (those reverb types read from songBufferReverb which expects scaled audio, not raw samples)
                // Average L+R (>> 1) to get mono without doubling the level
                int64_t monoScaled = ((leftSampleUnscaled + rightSampleUnscaled) >> 1) << (OUTPUT_SCALAR + scalar_modifier);
                {
                    int32_t voiceBalQ16 = (v->parentBank && v->parentBank == synth->banks[1])
                        ? balXmfQ16 : balMainQ16;
                    monoScaled = (monoScaled * voiceBalQ16) >> 16;
                }
                revOut += (monoScaled * v->reverbSend) >> 16;
            }
            if (pChorusBuffer && v->chorusSend > 0) {
                int64_t monoScaled = ((leftSampleUnscaled + rightSampleUnscaled) >> 1) << (OUTPUT_SCALAR + scalar_modifier);
                {
                    int32_t voiceBalQ16 = (v->parentBank && v->parentBank == synth->banks[1])
                        ? balXmfQ16 : balMainQ16;
                    monoScaled = (monoScaled * voiceBalQ16) >> 16;
                }
                choOut += (monoScaled * v->chorusSend) >> 16;
            }
        }
        
        int64_t mixedLeft = (int64_t)pBuffer[f * 2] + ((32 * leftOut) >> dlsGainFactor);
        int64_t mixedRight = (int64_t)pBuffer[f * 2 + 1] + ((32 * rightOut) >> dlsGainFactor);

        {
            const int64_t limit = (int64_t)32767 << OUTPUT_SCALAR;
            int64_t absLeft = mixedLeft < 0 ? -mixedLeft : mixedLeft;
            int64_t absRight = mixedRight < 0 ? -mixedRight : mixedRight;
            int64_t peak = absLeft > absRight ? absLeft : absRight;
            int32_t targetGain = peak > limit ? (int32_t)((limit << 16) / peak) : 0x10000;
            if (targetGain < synth->limiterGainQ16) {
                int32_t attackFrames = synth->sampleRate / 1000;
                int32_t delta;
                if (attackFrames < 1) attackFrames = 1;
                delta = (synth->limiterGainQ16 - targetGain) / attackFrames;
                if (delta < 1) delta = 1;
                synth->limiterGainQ16 -= delta;
                if (synth->limiterGainQ16 < targetGain) synth->limiterGainQ16 = targetGain;
            } else if (synth->limiterGainQ16 < 0x10000) {
                int32_t releaseFrames = synth->sampleRate / 20;
                int32_t delta;
                if (releaseFrames < 1) releaseFrames = 1;
                delta = (0x10000 - synth->limiterGainQ16) / releaseFrames;
                if (delta < 1) delta = 1;
                synth->limiterGainQ16 += delta;
                if (synth->limiterGainQ16 > 0x10000) synth->limiterGainQ16 = 0x10000;
            }
            mixedLeft = (mixedLeft * synth->limiterGainQ16) >> 16;
            mixedRight = (mixedRight * synth->limiterGainQ16) >> 16;
            if (mixedLeft > limit) mixedLeft = limit;
            else if (mixedLeft < -limit) mixedLeft = -limit;
            if (mixedRight > limit) mixedRight = limit;
            else if (mixedRight < -limit) mixedRight = -limit;
        }
        pBuffer[f * 2] = (int32_t)mixedLeft;
        pBuffer[f * 2 + 1] = (int32_t)mixedRight;
        if (pReverbBuffer) {
            int64_t mixedReverb = (int64_t)pReverbBuffer[f] + ((20 * revOut) >> dlsGainFactor);
            pReverbBuffer[f] = (int32_t)(mixedReverb > INT32_MAX ? INT32_MAX : (mixedReverb < INT32_MIN ? INT32_MIN : mixedReverb));
        }
        if (pChorusBuffer) {
            int64_t mixedChorus = (int64_t)pChorusBuffer[f] + ((20 * choOut) >> dlsGainFactor);
            pChorusBuffer[f] = (int32_t)(mixedChorus > INT32_MAX ? INT32_MAX : (mixedChorus < INT32_MIN ? INT32_MIN : mixedChorus));
        }
    }
}
