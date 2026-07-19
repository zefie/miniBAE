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

/*****************************************************************************/
/*
** "GenReverbNeo.c"
**
**  Roland MT-32 style reverb implementation for miniBAE
**
**  Written by: NeoBAE Contributors
**  Created: 2025
**
**  This file implements a Roland MT-32 inspired reverb system with three modes:
**  - Room: Short early reflections with moderate decay
**  - Hall: Longer reverb with smoother decay
**  - Tap Delay: Multiple discrete echoes (characteristic MT-32 effect)
**
**  The MT-32 used a relatively simple but distinctive reverb algorithm based
**  on delay lines and feedback. This implementation captures that character
**  while adapting to miniBAE's architecture.
**
** Modification History:
**  12/24/25    Initial implementation of MT-32 style reverb
*/
/*****************************************************************************/

#include "GenSnd.h"
#include "GenPriv.h"
#include "BAE_API.h"
#include "X_API.h"
#include <stdint.h>
#include <math.h>

#if USE_NEO_EFFECTS     // Conditionally compile this file

// Fixed-point shift amounts for precision
#define NEO_COEFF_SHIFT         16
#define NEO_COEFF_MULTIPLY      (1L << NEO_COEFF_SHIFT)

// Match the internal scaling used by GenReverbNew.c so the mono send buffer
// (songBufferReverb) is interpreted in the same domain and the wet output is
// added back to the dry mix consistently.
#define NEO_INPUTSHIFT          10

// Neo reverb tends to be perceptually quieter than the legacy/"new" reverb path
// at the same controller sends. Boost the wet addback slightly so Room/Hall/Tap
// are clearly audible vs. None.
#define NEO_WETSHIFT            (NEO_INPUTSHIFT + 1)

// Feedback coefficient limits (Q16.16).
// Keeping these conservative avoids "build-up" behavior where long feedback
// approaches unity and the tail becomes effectively non-decaying.
#define NEO_FEEDBACK_MIN_Q16    19661   // ~0.30
#define NEO_ROOM_FEEDBACK_MAX_Q16 45875 // ~0.70
#define NEO_HALL_FEEDBACK_MAX_Q16 51118 // ~0.78

// Fixed-point limit-cycle killer: once the feedback loop falls below this
// magnitude, snap to zero so the tail actually dies out.
// (This avoids the classic "infinite sustain / buzzing" artifact in IIR delay
// networks implemented with truncating fixed-point math.)
// Keep this very low to avoid audible quantization artifacts
#define NEO_SILENCE_THRESHOLD   2

// Output-side idle shutoff:
// When the input send is silent and the computed wet output is very small
// for a short hold period, clear filter state and delay buffers.
// This prevents "never finishes" low-level limit-cycle noise without putting
// a harsh gate in the feedback path.
#define NEO_IDLE_INPUT_THRESHOLD        1
#define NEO_IDLE_WET_THRESHOLD          8
#define NEO_IDLE_HOLD_FRAMES_MIN        11025
#define NEO_IDLE_HOLD_FRAMES_MAX        44100

// Default reverb time used when the host doesn't provide one.
// This must be < 1.0 feedback (via SetNeoReverbTime) to avoid non-decaying tails.
#define NEO_DEFAULT_REVERB_TIME 100

// NOTE:
// In miniBAE, MusicGlobals->songBufferReverb is a MONO send buffer with
// length == One_Loop (frames). The destination dry buffer is interleaved
// stereo (L,R,L,R...).
// This implementation keeps internal delay lines interleaved stereo for
// a wider image, but consumes mono input.

// Buffer sizes for MT-32 style delays (must be power of 2)
// Tap delay needs to hold up to ~400ms @ 44.1kHz in *stereo-interleaved* samples:
// 400ms is 17640 frames => 35280 interleaved samples, so 32768 would wrap.
#define NEO_TAP_BUFFER_SIZE     65536
// Custom reverb needs to hold up to 500ms at common output rates.
// Buffer size is in *stereo-interleaved* samples, so max frames is (size/2)-2.
// At 48kHz, 500ms is 24000 frames -> 48000 interleaved samples, so use 65536.
#define NEO_CUSTOM_BUFFER_SIZE  65536

#define NEO_TAP_BUFFER_MASK     (NEO_TAP_BUFFER_SIZE - 1)
#define NEO_CUSTOM_BUFFER_MASK  (NEO_CUSTOM_BUFFER_SIZE - 1)

// MT-32 Tap Delay mode: Multiple discrete echoes
// Delays for rhythmic echoes at ~100ms, 200ms, 300ms, 400ms at 44.1kHz
#define NEO_TAP_COUNT           4
static const int32_t neo_tap_delays[] = {4410, 8820, 13230, 17640};
static const int32_t neo_tap_gains[] = {XFIXED_1, 52428, 39321, 26214};  // Descending gains


// ==============================================================================
// MobileBAE Types
// ==============================================================================
#define MOBILE_DELAY_LENGTH 8192
#define MOBILE_TYPE_SCALE 0x00010000
#define MOBILE_REVERB_TIME 85197

typedef struct {
    int16_t inputDelay[MOBILE_DELAY_LENGTH];
    int16_t comb[6][MOBILE_DELAY_LENGTH];
    int inputIndex[8];
    int combRead[6];
    int combWrite[6];
    int combFeedback[6];
    int16_t early[256];
    int16_t stereoL[512];
    int16_t stereoR[512];
    int sampleRate;
    int wetSmoothingGain;
    int16_t wetSmoothingState;
    int earlyRead;
    int earlyWrite;
    int stereoRead;
    int stereoWrite;
    bool initialized;
} MobileReverbState;

static MobileReverbState gMobileReverb;

// Global reverb parameters
typedef struct NeoReverbParams
{
    bool       mIsInitialized;
    Rate        mSampleRate;
    int32_t     mReverbMode;  // Which MT-32 mode (Room/Hall/Tap)
    
    // Tap delay buffer
    int32_t       *mTapBuffer;
    int         mTapWriteIdx;
    int         mTapReadIdx[NEO_TAP_COUNT];
    int         mTapDelayFrames[NEO_TAP_COUNT];
    
    // Low-pass filter for smoothing
    int32_t       mFilterMemoryL;
    int32_t       mFilterMemoryR;
    int32_t       mLopassK;
    
    // Wet/dry mix
    int32_t       mWetGain;
    int32_t       mDryGain;
    
    // Custom mode buffers and parameters
    int32_t       *mCustomBuffer[NEO_CUSTOM_MAX_COMBS];
    int         mCustomWriteIdx[NEO_CUSTOM_MAX_COMBS];
    int         mCustomReadIdx[NEO_CUSTOM_MAX_COMBS];
    int         mCustomDelayFrames[NEO_CUSTOM_MAX_COMBS];
    int32_t       mCustomFeedback[NEO_CUSTOM_MAX_COMBS];
    int32_t       mCustomGain[NEO_CUSTOM_MAX_COMBS];
    int         mCustomCombCount;
    bool       mCustomParamsDirty;  // Need to rebuild delays/indices

    // Tail shutoff state (shared by Custom + Tap)
    int         mIdleFrames;
    bool       mWasActive;
    
} NeoReverbParams;

NeoReverbParams gNeoReverbParams;

static INLINE int32_t PV_Clamp32From64(int64_t v)
{
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}

static INLINE int32_t PV_ScaleReverbSend(int32_t sendSample)
{
    // Match RunNewReverb(): convert engine mix domain to a smaller internal domain.
    // The +1 keeps headroom (mirrors the historical implementation).
    return sendSample >> (NEO_INPUTSHIFT + 1);
}

static INLINE int32_t PV_Abs32(int32_t v)
{
    if (v == INT32_MIN) return INT32_MAX;
    return (v < 0) ? -v : v;
}

static INLINE int PV_ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static INLINE int32_t PV_MulQ16_Trunc(int32_t a, int32_t b)
{
    int64_t p = (int64_t)a * (int64_t)b;
    
    // If negative, add (2^Shift - 1) before shifting.
    // This forces small negative numbers (e.g. -0.5) to become 0 instead of -1.
    if (p < 0) {
        p += (1LL << NEO_COEFF_SHIFT) - 1; 
    }
    
    return PV_Clamp32From64(p >> NEO_COEFF_SHIFT);
}

static INLINE int32_t PV_MulQ16_Round(int32_t a, int32_t b)
{
    int64_t p = (int64_t)a * (int64_t)b;
    p += (p >= 0) ? (1LL << (NEO_COEFF_SHIFT - 1)) : -(1LL << (NEO_COEFF_SHIFT - 1));
    return PV_Clamp32From64(p >> NEO_COEFF_SHIFT);
}

static INLINE int32_t PV_ZapSmall(int32_t v)
{
    if (v > -NEO_SILENCE_THRESHOLD && v < NEO_SILENCE_THRESHOLD)
        return 0;
    return v;
}

static INLINE int PV_ClampDelayFramesForBuffer(int frames, int interleavedBufferSize)
{
    // Interleaved stereo buffer holds (size/2) frames.
    const int maxFrames = (interleavedBufferSize / 2) - 2;
    if (frames < 1) return 1;
    if (frames > maxFrames) return maxFrames;
    return frames;
}

static void PV_UpdateNeoDelayTables(NeoReverbParams *params)
{
    // The constants are expressed in frames @ 44.1kHz. Scale to the actual output rate.
    // This keeps the perceived time constants consistent across sample rates.
    const int32_t refRate = 44100;
    int i;

    if (!params || params->mSampleRate <= 0)
        return;

    for (i = 0; i < NEO_TAP_COUNT; i++)
    {
        int64_t scaled = ((int64_t)neo_tap_delays[i] * (int64_t)params->mSampleRate + (refRate / 2)) / refRate;
        params->mTapDelayFrames[i] = PV_ClampDelayFramesForBuffer((int)scaled, NEO_TAP_BUFFER_SIZE);
    }
}


//++------------------------------------------------------------------------------
//  GetNeoReverbPresetParams()
//
//  Get the preset parameters for a Neo reverb type (for UI customization)
//++------------------------------------------------------------------------------
void GetNeoReverbPresetParams(int reverbType, int *combCount, int *delaysMs, int *feedback, int *gain, int *lowpass, int *mix)
{
    if (!combCount || !delaysMs || !feedback || !gain || !lowpass || !mix)
        return;

    // Initialize with defaults
    *combCount = 4;
    for (int i = 0; i < NEO_CUSTOM_MAX_COMBS; i++)
    {
        delaysMs[i] = 50 + (i * 25);  // Default delays
        feedback[i] = 90;
        gain[i] = 127;
    }
    *lowpass = 64;

    switch (reverbType)
    {
        case REVERB_TYPE_12: // Room
            *combCount = 3;
            delaysMs[0] = 35; delaysMs[1] = 43; delaysMs[2] = 52;
            feedback[0] = 70; feedback[1] = 70; feedback[2] = 70;
            gain[0] = 127; gain[1] = 127; gain[2] = 127;
            *lowpass = 50;
            *mix = 96;
            break;
        case REVERB_TYPE_13: // Hall
            *combCount = 4;
            delaysMs[0] = 52; delaysMs[1] = 65; delaysMs[2] = 79; delaysMs[3] = 93;
            feedback[0] = 85; feedback[1] = 85; feedback[2] = 85; feedback[3] = 85;
            gain[0] = 127; gain[1] = 127; gain[2] = 127; gain[3] = 127;
            *lowpass = 40;
            *mix = 88;
            break;
        case REVERB_TYPE_14: // Cavern
            *combCount = 4;
            delaysMs[0] = 75; delaysMs[1] = 125; delaysMs[2] = 175; delaysMs[3] = 200;
            feedback[0] = 107; feedback[1] = 107; feedback[2] = 107; feedback[3] = 107;
            gain[0] = 127; gain[1] = 127; gain[2] = 127; gain[3] = 127;
            *lowpass = 64;
            *mix = 127;
            break;
        case REVERB_TYPE_15: // Dungeon
            *combCount = 4;
            delaysMs[0] = 175; delaysMs[1] = 250; delaysMs[2] = 325; delaysMs[3] = 450;
            feedback[0] = 107; feedback[1] = 107; feedback[2] = 107; feedback[3] = 107;
            gain[0] = 127; gain[1] = 127; gain[2] = 127; gain[3] = 127;
            *lowpass = 64;
            *mix = 160;
            break;
        case REVERB_TYPE_16: // SC55-style
            *combCount = 4;
            delaysMs[0] = 40; delaysMs[1] = 56; delaysMs[2] = 79; delaysMs[3] = 122;
            feedback[0] = 116; feedback[1] = 101; feedback[2] = 127; feedback[3] = 122;
            gain[0] = 127; gain[1] = 127; gain[2] = 127; gain[3] = 127;
            *lowpass = 17;
            *mix = 152;
            break;
        case REVERB_TYPE_17: // Nokia-style
            *combCount = 4;
            delaysMs[0] = 16; delaysMs[1] = 14; delaysMs[2] = 25; delaysMs[3] = 20;
            feedback[0] = 127; feedback[1] = 127; feedback[2] = 127; feedback[3] = 127;
            gain[0] = 142; gain[1] = 136; gain[2] = 130; gain[3] = 254;
            *lowpass = 127;
            *mix = 110;
            break;            
        case REVERB_TYPE_18: // Tap delay - doesn't use custom reverb params
            *combCount = 4;
            *lowpass = 13107; // ~0.20
            *mix = 104;
            // Tap delay doesn't set custom params, so use defaults
            break;
        default:
            // For custom or unknown types, use current engine values
            *combCount = GetNeoCustomReverbCombCount();
            for (int i = 0; i < NEO_CUSTOM_MAX_COMBS; i++)
            {
                delaysMs[i] = GetNeoCustomReverbCombDelay(i);
                feedback[i] = GetNeoCustomReverbCombFeedback(i);
                gain[i] = GetNeoCustomReverbCombGain(i);
            }
            *lowpass = 64;
            *mix = 110;
            // Note: lowpass is not directly accessible, use default
            break;
    }
}

static void PV_ApplyNeoReverbDefaults(NeoReverbParams *params)
{
    if (!params)
        return;

    int combCount, delaysMs[NEO_CUSTOM_MAX_COMBS], feedback[NEO_CUSTOM_MAX_COMBS], gain[NEO_CUSTOM_MAX_COMBS], lowpass, mix;    

    if (params->mReverbMode < REVERB_TYPE_18) {
        GetNeoReverbPresetParams(params->mReverbMode, &combCount, delaysMs, feedback, gain, &lowpass, &mix);
        if (combCount > NEO_CUSTOM_MAX_COMBS)
            combCount = NEO_CUSTOM_MAX_COMBS;
        SetNeoCustomReverbCombCount(combCount);
        for (int i = 0; i < combCount; i++)
        {
            SetNeoCustomReverbCombDelay(i, delaysMs[i]);
            SetNeoCustomReverbCombFeedback(i, feedback[i]);
            SetNeoCustomReverbCombGain(i, gain[i]);
        }
        SetNeoCustomReverbLowpass(lowpass); 
        SetNeoReverbMix(mix);
    } else {
        SetNeoCustomReverbLowpass(50);
        SetNeoReverbMix(110);  // More aggressive wet mix
    }
}

//++------------------------------------------------------------------------------
//  GetNeoReverbParams()
//
//  Returns pointer to global Neo reverb parameters
//++------------------------------------------------------------------------------
NeoReverbParams* GetNeoReverbParams(void)
{
    return &gNeoReverbParams;
}

// Return TRUE if Neo reverb internal state indicates activity or an active tail
bool BAENeoReverb_IsActive(void)
{
    NeoReverbParams* params = GetNeoReverbParams();
    if (!params || !params->mIsInitialized)
        return FALSE;

    // Quick checks: active flag or filter memory non-zero
    if (params->mWasActive)
        return TRUE;
    if (params->mFilterMemoryL != 0 || params->mFilterMemoryR != 0)
        return TRUE;

    // Check recent tap buffer samples (last 2 frames) if present
    if (params->mTapBuffer && params->mTapWriteIdx >= 2)
    {
        int idx1 = (params->mTapWriteIdx - 1) & NEO_TAP_BUFFER_MASK;
        int idx2 = (params->mTapWriteIdx - 2) & NEO_TAP_BUFFER_MASK;
        if (params->mTapBuffer[idx1] != 0 || params->mTapBuffer[idx2] != 0)
            return TRUE;
    }

    // Check a couple of samples from custom comb buffers
    for (int ci = 0; ci < NEO_CUSTOM_MAX_COMBS; ++ci)
    {
        if (params->mCustomBuffer[ci] && params->mCustomWriteIdx[ci] >= 2)
        {
            int wi = params->mCustomWriteIdx[ci];
            int idx1 = (wi - 1) & NEO_CUSTOM_BUFFER_MASK;
            int idx2 = (wi - 2) & NEO_CUSTOM_BUFFER_MASK;
            if (params->mCustomBuffer[ci][idx1] != 0 || params->mCustomBuffer[ci][idx2] != 0)
                return TRUE;
        }
    }

    // Check MobileBAE reverb tail
    if (gMobileReverb.initialized)
    {
        // Use a threshold to prevent hanging on near-silent IIR limit cycles
        if (gMobileReverb.wetSmoothingState > 4 || gMobileReverb.wetSmoothingState < -4)
            return TRUE;
        
        int sRead = gMobileReverb.stereoRead;
        int idx1 = (sRead - 1) & 0x1FF;
        int idx2 = (sRead - 2) & 0x1FF;
        if (gMobileReverb.stereoL[idx1] > 4 || gMobileReverb.stereoL[idx1] < -4 ||
            gMobileReverb.stereoL[idx2] > 4 || gMobileReverb.stereoL[idx2] < -4 ||
            gMobileReverb.stereoR[idx1] > 4 || gMobileReverb.stereoR[idx1] < -4 ||
            gMobileReverb.stereoR[idx2] > 4 || gMobileReverb.stereoR[idx2] < -4)
            return TRUE;
    }

    return FALSE;
}
//++------------------------------------------------------------------------------
//  InitNeoReverb()
//
//  Initialize the MT-32 style reverb system
//++------------------------------------------------------------------------------
bool InitNeoReverb(void)
{
    int i;
    NeoReverbParams* params = GetNeoReverbParams();
    
    params->mIsInitialized = FALSE;
    
    // Allocate tap delay buffer
    params->mTapBuffer = (int32_t*)XNewPtr(sizeof(int32_t) * NEO_TAP_BUFFER_SIZE);
    if (params->mTapBuffer == NULL)
    {
        ShutdownNeoReverb();
        return FALSE;
    }
    XSetMemory(params->mTapBuffer, sizeof(int32_t) * NEO_TAP_BUFFER_SIZE, 0);
    params->mTapWriteIdx = 0;
    
    // Allocate custom mode buffers
    for (i = 0; i < NEO_CUSTOM_MAX_COMBS; i++)
    {
        params->mCustomBuffer[i] = (int32_t*)XNewPtr(sizeof(int32_t) * NEO_CUSTOM_BUFFER_SIZE);
        if (params->mCustomBuffer[i] == NULL)
        {
            ShutdownNeoReverb();
            return FALSE;
        }
        XSetMemory(params->mCustomBuffer[i], sizeof(int32_t) * NEO_CUSTOM_BUFFER_SIZE, 0);
        params->mCustomWriteIdx[i] = 0;
        params->mCustomFeedback[i] = (int32_t)(NEO_COEFF_MULTIPLY * 0.75);  // Default 0.75 feedback
        params->mCustomGain[i] = NEO_COEFF_MULTIPLY;  // Default full gain
        // Varied delay times for richer texture
        params->mCustomDelayFrames[i] = 1000 + (i * 300);  // 22ms, 29ms, 36ms, etc. @ 44.1kHz
    }
    params->mCustomCombCount = 4;  // Default to 4 combs
    params->mCustomParamsDirty = FALSE;
    
    // Setup delay tables and read indices based on delay times
    // Multiply delays by 2 for stereo interleaving (L,R,L,R...)
    params->mSampleRate = MusicGlobals->outputRate;
    PV_UpdateNeoDelayTables(params);
    for (i = 0; i < NEO_TAP_COUNT; i++)
    {
        params->mTapReadIdx[i] = (NEO_TAP_BUFFER_SIZE - (params->mTapDelayFrames[i] * 2)) & NEO_TAP_BUFFER_MASK;
    }
    
    for (i = 0; i < NEO_CUSTOM_MAX_COMBS; i++)
    {
        params->mCustomReadIdx[i] = (NEO_CUSTOM_BUFFER_SIZE - (params->mCustomDelayFrames[i] * 2)) & NEO_CUSTOM_BUFFER_MASK;
    }
    
    // Initialize filter state
    params->mFilterMemoryL = 0;
    params->mFilterMemoryR = 0;
    params->mLopassK = 13107;  // ~0.2 filter coefficient (gentle smoothing)
    
    // Default wet/dry mix (MT-32 style: strong wet signal for obvious effect)
    params->mWetGain = 98304;   // ~1.5 (very strong for obvious reverb)
    params->mDryGain = 52428;   // ~0.8
    
    params->mReverbMode = -1;  // Will be set by CheckNeoReverbType
    params->mIdleFrames = 0;
    params->mWasActive = FALSE;
    params->mIsInitialized = TRUE;
    
    return TRUE;
}

//++------------------------------------------------------------------------------
//  ShutdownNeoReverb()
//
//  Clean up and deallocate Neo reverb resources
//++------------------------------------------------------------------------------
void ShutdownNeoReverb(void)
{
    int i;
    NeoReverbParams* params = GetNeoReverbParams();
    
    params->mIsInitialized = FALSE;
    
    // Deallocate tap buffer
    if (params->mTapBuffer)
    {
        XDisposePtr(params->mTapBuffer);
        params->mTapBuffer = NULL;
    }
    
    // Deallocate custom buffers
    for (i = 0; i < NEO_CUSTOM_MAX_COMBS; i++)
    {
        if (params->mCustomBuffer[i])
        {
            XDisposePtr(params->mCustomBuffer[i]);
            params->mCustomBuffer[i] = NULL;
        }
    }
}

//++------------------------------------------------------------------------------
//  CheckNeoReverbType()
//
//  Check if reverb type has changed and clear buffers if needed
//++------------------------------------------------------------------------------
bool CheckNeoReverbType(void)
{
    NeoReverbParams* params = GetNeoReverbParams();
    bool changed = FALSE;
    int i;
    
    if (!params->mIsInitialized)
        return FALSE;
    
    if (params->mReverbMode != MusicGlobals->reverbUnitType)
    {
        changed = TRUE;
        params->mReverbMode = MusicGlobals->reverbUnitType;

        // If the output rate changes, keep the time constants stable.
        if (params->mSampleRate != MusicGlobals->outputRate)
        {
            params->mSampleRate = MusicGlobals->outputRate;
            PV_UpdateNeoDelayTables(params);
        }
        
        // Clear all buffers when changing modes
        if (params->mTapBuffer)
            XSetMemory(params->mTapBuffer, sizeof(int32_t) * NEO_TAP_BUFFER_SIZE, 0);
        params->mTapWriteIdx = 0;
        
        for (i = 0; i < NEO_CUSTOM_MAX_COMBS; i++)
        {
            if (params->mCustomBuffer[i])
                XSetMemory(params->mCustomBuffer[i], sizeof(int32_t) * NEO_CUSTOM_BUFFER_SIZE, 0);
            params->mCustomWriteIdx[i] = 0;
        }
        params->mCustomParamsDirty = FALSE;
        
        // Reset filter memory
        params->mFilterMemoryL = 0;
        params->mFilterMemoryR = 0;

        params->mIdleFrames = 0;
        params->mWasActive = FALSE;

        // Apply MT-32-ish defaults per mode.
        PV_ApplyNeoReverbDefaults(params);
    }
    
    return changed;
}

//++------------------------------------------------------------------------------
//  PV_ProcessNeoTapReverb()
//
//  MT-32 Tap Delay mode: Multiple discrete echoes
//  Characteristic MT-32 rhythmic echo effect
//++------------------------------------------------------------------------------
static void PV_ProcessNeoTapReverb(int32_t *sourceP, int32_t *destP, int numFrames)
{
    NeoReverbParams* params = GetNeoReverbParams();
    int32_t inputL, inputR, outputL, outputR;
    int32_t tapL, tapR;
    int i, frame, readPos;

    const int idleHoldFrames = PV_ClampInt(params->mSampleRate / 50, NEO_IDLE_HOLD_FRAMES_MIN, NEO_IDLE_HOLD_FRAMES_MAX);
    
    for (frame = 0; frame < numFrames; frame++)
    {
        // Get mono input from reverb send buffer
        int32_t input = PV_ScaleReverbSend(sourceP[frame]);
        inputL = input;
        inputR = input;
        
        // Write input to delay buffer first
        params->mTapBuffer[params->mTapWriteIdx] = inputL;
        params->mTapBuffer[(params->mTapWriteIdx + 1) & NEO_TAP_BUFFER_MASK] = inputR;
        
        outputL = 0;
        outputR = 0;
        
        // Sum all tap delays with decreasing gains
        for (i = 0; i < NEO_TAP_COUNT; i++)
        {
            // Calculate read position: delay samples back from write position
            readPos = (params->mTapWriteIdx - (params->mTapDelayFrames[i] * 2)) & NEO_TAP_BUFFER_MASK;
            
            tapL = params->mTapBuffer[readPos];
            tapR = params->mTapBuffer[(readPos + 1) & NEO_TAP_BUFFER_MASK];
            
            outputL = PV_Clamp32From64((int64_t)outputL + (((int64_t)tapL * (int64_t)neo_tap_gains[i]) >> NEO_COEFF_SHIFT));
            outputR = PV_Clamp32From64((int64_t)outputR + (((int64_t)tapR * (int64_t)neo_tap_gains[i]) >> NEO_COEFF_SHIFT));
        }
        
        // Advance write index
        params->mTapWriteIdx = (params->mTapWriteIdx + 2) & NEO_TAP_BUFFER_MASK;
        
        // Apply light filtering to taps
        {
            int32_t dL = (int32_t)(outputL - params->mFilterMemoryL);
            int32_t dR = (int32_t)(outputR - params->mFilterMemoryR);
            params->mFilterMemoryL = PV_Clamp32From64((int64_t)params->mFilterMemoryL + (int64_t)PV_MulQ16_Round(dL, params->mLopassK));
            params->mFilterMemoryR = PV_Clamp32From64((int64_t)params->mFilterMemoryR + (int64_t)PV_MulQ16_Round(dR, params->mLopassK));
        }

        // If we're effectively silent for long enough, hard reset state/buffers.
        // This stops low-level limit-cycle noise from keeping the VU alive.
        {
            int32_t wetTestL = PV_MulQ16_Round(params->mFilterMemoryL, params->mWetGain);
            int32_t wetTestR = PV_MulQ16_Round(params->mFilterMemoryR, params->mWetGain);
            const bool isIdle = (PV_Abs32(input) < NEO_IDLE_INPUT_THRESHOLD) &&
                                (PV_Abs32(wetTestL) < NEO_IDLE_WET_THRESHOLD) &&
                                (PV_Abs32(wetTestR) < NEO_IDLE_WET_THRESHOLD);
            if (isIdle)
            {
                params->mIdleFrames++;
                if (params->mWasActive && params->mIdleFrames >= idleHoldFrames)
                {
                    XSetMemory(params->mTapBuffer, sizeof(int32_t) * NEO_TAP_BUFFER_SIZE, 0);
                    params->mTapWriteIdx = 0;
                    params->mFilterMemoryL = 0;
                    params->mFilterMemoryR = 0;
                    params->mWasActive = FALSE;
                }
            }
            else
            {
                params->mIdleFrames = 0;
                params->mWasActive = TRUE;
            }
        }
        
        // Mix wet reverb signal into destination (dry) buffer
        {
            int32_t wetL = PV_MulQ16_Round(params->mFilterMemoryL, params->mWetGain);
            int32_t wetR = PV_MulQ16_Round(params->mFilterMemoryR, params->mWetGain);
            destP[frame * 2] += (int32_t)(((int64_t)wetL) << NEO_WETSHIFT);
            destP[frame * 2 + 1] += (int32_t)(((int64_t)wetR) << NEO_WETSHIFT);
        }
    }
}

//++------------------------------------------------------------------------------
//  PV_RebuildCustomDelayIndices()
//
//  Rebuild read indices for custom reverb when parameters change
//++------------------------------------------------------------------------------
static void PV_RebuildCustomDelayIndices(NeoReverbParams *params)
{
    int i;
    for (i = 0; i < params->mCustomCombCount; i++)
    {
        int clampedDelay = PV_ClampDelayFramesForBuffer(params->mCustomDelayFrames[i], NEO_CUSTOM_BUFFER_SIZE);
        params->mCustomDelayFrames[i] = clampedDelay;
        params->mCustomReadIdx[i] = (NEO_CUSTOM_BUFFER_SIZE - (clampedDelay * 2)) & NEO_CUSTOM_BUFFER_MASK;
    }
    params->mCustomParamsDirty = FALSE;
}

//++------------------------------------------------------------------------------
//  PV_ProcessNeoCustomReverb()
//
//  Custom reverb mode: User-configurable parallel comb filters
//  Allows full control over delay times, feedback, and gain per comb
//++------------------------------------------------------------------------------
static void PV_ProcessNeoCustomReverb(int32_t *sourceP, int32_t *destP, int numFrames)
{
    NeoReverbParams* params = GetNeoReverbParams();
    int32_t inputL, inputR, combOutL, combOutR;
    int64_t outputL, outputR;
    int32_t delayedL, delayedR, feedback, gain = 0;
    int i, frame, readPos;

    const int idleHoldFrames = PV_ClampInt(params->mSampleRate / 50, NEO_IDLE_HOLD_FRAMES_MIN, NEO_IDLE_HOLD_FRAMES_MAX);
       
    // Rebuild delay indices if parameters have changed
    if (params->mCustomParamsDirty)
    {
        PV_RebuildCustomDelayIndices(params);
    }
    
    for (frame = 0; frame < numFrames; frame++)
    {
        // Get mono input from reverb send buffer
        int32_t input = PV_ScaleReverbSend(sourceP[frame]);
        inputL = input;
        inputR = input;
        
        outputL = 0;
        outputR = 0;
        
        // Process parallel comb filters (up to user-defined count)
        for (i = 0; i < params->mCustomCombCount; i++)
        {
            // Calculate read position: delay samples back from write position
            readPos = (params->mCustomWriteIdx[i] - (params->mCustomDelayFrames[i] * 2)) & NEO_CUSTOM_BUFFER_MASK;
            
            // Read delayed samples
            delayedL = params->mCustomBuffer[i][readPos];
            delayedR = params->mCustomBuffer[i][(readPos + 1) & NEO_CUSTOM_BUFFER_MASK];
            
            // Compute comb filter output: input + delayed * feedback
            // Truncation needed to prevent infinite growth
            feedback = params->mCustomFeedback[i];
            int32_t feedbackL = PV_ZapSmall(PV_MulQ16_Trunc(delayedL, feedback));
            int32_t feedbackR = PV_ZapSmall(PV_MulQ16_Trunc(delayedR, feedback));
            
            combOutL = PV_ZapSmall(PV_Clamp32From64((int64_t)inputL + (int64_t)feedbackL));
            combOutR = PV_ZapSmall(PV_Clamp32From64((int64_t)inputR + (int64_t)feedbackR));

            // Write to current position
            params->mCustomBuffer[i][params->mCustomWriteIdx[i]] = combOutL;
            params->mCustomBuffer[i][(params->mCustomWriteIdx[i] + 1) & NEO_CUSTOM_BUFFER_MASK] = combOutR;
            
            // Accumulate output with per-comb gain (use delayed values for output)
            gain = params->mCustomGain[i];
            outputL += (int64_t)PV_MulQ16_Trunc(delayedL, gain);
            outputR += (int64_t)PV_MulQ16_Trunc(delayedR, gain);
            
            // Advance write index
            params->mCustomWriteIdx[i] = (params->mCustomWriteIdx[i] + 2) & NEO_CUSTOM_BUFFER_MASK;
        }
        
        // Average the output from all combs to prevent clipping
        if (params->mCustomCombCount > 0)
        {
            outputL = outputL / params->mCustomCombCount;
            outputR = outputR / params->mCustomCombCount;
        }
        
        // Apply low-pass filtering for smoothing
        {
            int32_t out32L = PV_Clamp32From64(outputL);
            int32_t out32R = PV_Clamp32From64(outputR);
            int32_t dL = (int32_t)(out32L - params->mFilterMemoryL);
            int32_t dR = (int32_t)(out32R - params->mFilterMemoryR);
            params->mFilterMemoryL = PV_Clamp32From64((int64_t)params->mFilterMemoryL + (int64_t)PV_MulQ16_Round(dL, params->mLopassK));
            params->mFilterMemoryR = PV_Clamp32From64((int64_t)params->mFilterMemoryR + (int64_t)PV_MulQ16_Round(dR, params->mLopassK));
        }

        // Output-side idle shutoff: once wet is very small for long enough (and no input),
        // clear filter state and delay lines to fully stop the tail.
        {
            int32_t wetTestL = PV_MulQ16_Round(params->mFilterMemoryL, params->mWetGain);
            int32_t wetTestR = PV_MulQ16_Round(params->mFilterMemoryR, params->mWetGain);
            bool isIdle = FALSE;
            if (gain > 127) {
                isIdle = (PV_Abs32(wetTestL) < NEO_IDLE_WET_THRESHOLD * 1.5f) &&
                         (PV_Abs32(wetTestR) < NEO_IDLE_WET_THRESHOLD * 1.5f);
            } else {
                isIdle = (PV_Abs32(wetTestL) < NEO_IDLE_WET_THRESHOLD) &&
                         (PV_Abs32(wetTestR) < NEO_IDLE_WET_THRESHOLD);
            }
            if (isIdle)
            {
                params->mIdleFrames++;
                if (params->mWasActive && params->mIdleFrames >= idleHoldFrames)
                {
                    for (i = 0; i < params->mCustomCombCount; i++)
                    {
                        XSetMemory(params->mCustomBuffer[i], sizeof(int32_t) * NEO_CUSTOM_BUFFER_SIZE, 0);
                        params->mCustomWriteIdx[i] = 0;
                    }
                    params->mFilterMemoryL = 0;
                    params->mFilterMemoryR = 0;
                    params->mWasActive = FALSE;
                }
            }
            else
            {
                params->mIdleFrames = 0;
                params->mWasActive = TRUE;
            }
        }
        
        // Mix wet reverb signal into destination (dry) buffer
        {
            int32_t wetL = PV_MulQ16_Round(params->mFilterMemoryL, params->mWetGain);
            int32_t wetR = PV_MulQ16_Round(params->mFilterMemoryR, params->mWetGain);
            destP[frame * 2] += (int32_t)(((int64_t)wetL) << NEO_WETSHIFT);
            destP[frame * 2 + 1] += (int32_t)(((int64_t)wetR) << NEO_WETSHIFT);
        }
    }
}

//++------------------------------------------------------------------------------
//  RunNeoReverb()
//
//  Main entry point for Neo reverb processing
//  Dispatches to appropriate MT-32 mode
//++------------------------------------------------------------------------------
void RunNeoReverb(int32_t *sourceP, int32_t *destP, int numFrames)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    
    if (!params->mIsInitialized)
    {
        return;
    }
    
    CheckNeoReverbType();
    
    // Dispatch to appropriate reverb mode
    switch (params->mReverbMode)
    {
        case REVERB_TYPE_12:  // Neo Room (uses Custom preset)
        case REVERB_TYPE_13:  // Neo Hall (uses Custom preset)
        case REVERB_TYPE_14:  // Neo Cavern (uses Custom preset)
        case REVERB_TYPE_15:  // Neo Dungeon (uses Custom preset)
        case REVERB_TYPE_16:  // Neo SC55-style (uses Custom preset)
        case REVERB_TYPE_17:  // Neo Nokia (uses Custom preset)
            PV_ProcessNeoCustomReverb(sourceP, destP, numFrames);
            break;
            
        case REVERB_TYPE_18:  // Neo Tap Delay
            PV_ProcessNeoTapReverb(sourceP, destP, numFrames);
            break;
            
        default:
            if (params->mReverbMode > REVERB_TYPE_18)
            {
                // Treat unknown custom modes as Custom reverb
                PV_ProcessNeoCustomReverb(sourceP, destP, numFrames);
            }
            // No reverb or unsupported type
            break;
    }
}

//++------------------------------------------------------------------------------
//  SetNeoReverbMix()
//
//  Set the wet/dry mix for the reverb
//  wetLevel: 0-255 (extended range for wetter reverb)
//++------------------------------------------------------------------------------
void SetNeoReverbMix(int wetLevel)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    if (wetLevel < 0) wetLevel = 0;
    if (wetLevel > 255) wetLevel = 255;
    
    // Convert extended level (0-255) to fixed-point gain
    // At 255, wet gain is 2.0x for extra wetness
    params->mWetGain = (wetLevel * NEO_COEFF_MULTIPLY) / 128;
    params->mDryGain = ((255 - (wetLevel / 2)) * NEO_COEFF_MULTIPLY) / 255;  // Reduce dry less aggressively
}

//++------------------------------------------------------------------------------
//  SetNeoCustomReverbCombCount()
//
//  Set the number of active comb filters for custom reverb
//  combCount: 1-8 (number of parallel comb filters)
//++------------------------------------------------------------------------------
void SetNeoCustomReverbCombCount(int combCount)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    if (combCount < 1) combCount = 1;
    if (combCount > NEO_CUSTOM_MAX_COMBS) combCount = NEO_CUSTOM_MAX_COMBS;
    
    if (params->mCustomCombCount != combCount)
    {
        params->mCustomCombCount = combCount;
        params->mCustomParamsDirty = TRUE;
    }
}

//++------------------------------------------------------------------------------
//  SetNeoCustomReverbCombDelay()
//
//  Set the delay time in milliseconds for a specific comb filter
//  combIndex: 0-3 (which comb filter to configure) (max of 4 or value of NEO_CUSTOM_MAX_COMBS)
//  delayMs: delay time in milliseconds (1-500ms) (or value of NEO_CUSTOM_MAX_DELAY_MS)
//++------------------------------------------------------------------------------
void SetNeoCustomReverbCombDelay(int combIndex, int delayMs)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    if (combIndex < 0 || combIndex >= NEO_CUSTOM_MAX_COMBS)
        return;
    
    if (delayMs < 1) delayMs = 1;
    if (delayMs > NEO_CUSTOM_MAX_DELAY_MS) delayMs = NEO_CUSTOM_MAX_DELAY_MS;
    
    // Convert milliseconds to frames at current sample rate
    // delayMs * sampleRate / 1000
    int delayFrames = (delayMs * params->mSampleRate) / 1000;
    
    if (params->mCustomDelayFrames[combIndex] != delayFrames)
    {
        params->mCustomDelayFrames[combIndex] = delayFrames;
        params->mCustomParamsDirty = TRUE;
    }
}

//++------------------------------------------------------------------------------
//  SetNeoCustomReverbCombFeedback()
//
//  Set the feedback coefficient for a specific comb filter
//  combIndex: 0-7 (which comb filter to configure)
//  feedback: 0-127 (MIDI style, maps to ~0.0-0.85 feedback)
//++------------------------------------------------------------------------------
void SetNeoCustomReverbCombFeedback(int combIndex, int feedback)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    if (combIndex < 0 || combIndex >= NEO_CUSTOM_MAX_COMBS)
        return;
    
    if (feedback < 0) feedback = 0;
    if (feedback > NEO_CUSTOM_MAX_FEEDBACK) feedback = NEO_CUSTOM_MAX_FEEDBACK;
    
    // Map 0-127 to feedback range (0.0 to ~0.85)
    // Use a safe max to avoid runaway feedback
    const int32_t maxFeedback = (int32_t)(NEO_COEFF_MULTIPLY * 0.85);
    params->mCustomFeedback[combIndex] = (feedback * maxFeedback) / NEO_CUSTOM_MAX_FEEDBACK;
}

//++------------------------------------------------------------------------------
//  SetNeoCustomReverbCombGain()
//
//  Set the output gain for a specific comb filter
//  combIndex: 0-7 (which comb filter to configure)
//  gain: 0-127 (MIDI style, maps to 0.0-2.0 gain)
//++------------------------------------------------------------------------------
void SetNeoCustomReverbCombGain(int combIndex, int gain)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    if (combIndex < 0 || combIndex >= NEO_CUSTOM_MAX_COMBS)
        return;
    
    if (gain < 0) gain = 0;
    if (gain > NEO_CUSTOM_MAX_GAIN) gain = NEO_CUSTOM_MAX_GAIN;
    
    // Map 0-255 to gain range (0.0 to 2.0)
    params->mCustomGain[combIndex] = (gain * NEO_COEFF_MULTIPLY) / 127;
}

//++------------------------------------------------------------------------------
//  SetNeoCustomReverbLowpass()
//
//  Set the low-pass filter coefficient for custom reverb
//  lowpass: 0-127 (MIDI style, maps to filter coefficient 0.0-0.5)
//          Lower values = more filtering (darker sound)
//          Higher values = less filtering (brighter sound)
//++------------------------------------------------------------------------------
void SetNeoCustomReverbLowpass(int lowpass)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    if (lowpass < 0) lowpass = 0;
    if (lowpass > NEO_CUSTOM_MAX_LOWPASS) lowpass = NEO_CUSTOM_MAX_LOWPASS;
    
    // Map 0-255 to lowpass coefficient range (0.0 to 0.5)
    // This controls how much of the new signal blends with the filtered memory
    params->mLopassK = (lowpass * NEO_COEFF_MULTIPLY / 2) / 127;
}

//++------------------------------------------------------------------------------
//  GetNeoCustomReverbCombCount()
//
//  Get the current number of active comb filters
//++------------------------------------------------------------------------------
int GetNeoCustomReverbCombCount(void)
{
    NeoReverbParams* params = GetNeoReverbParams();
    return params->mCustomCombCount;
}

//++------------------------------------------------------------------------------
//  GetNeoCustomReverbCombDelay()
//
//  Get the delay time in milliseconds for a specific comb filter
//++------------------------------------------------------------------------------
int GetNeoCustomReverbCombDelay(int combIndex)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    if (combIndex < 0 || combIndex >= NEO_CUSTOM_MAX_COMBS)
        return 0;
    
    // Convert frames back to milliseconds
    return (params->mCustomDelayFrames[combIndex] * 1000) / params->mSampleRate;
}

//++------------------------------------------------------------------------------
//  GetNeoCustomReverbCombFeedback()
//
//  Get the feedback coefficient for a specific comb filter (0-127)
//++------------------------------------------------------------------------------
int GetNeoCustomReverbCombFeedback(int combIndex)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    if (combIndex < 0 || combIndex >= NEO_CUSTOM_MAX_COMBS)
        return 0;
    
    // Map feedback back to 0-127 range
    const int32_t maxFeedback = (int32_t)(NEO_COEFF_MULTIPLY * 0.85);
    return (int)((params->mCustomFeedback[combIndex] * 127) / maxFeedback);
}

//++------------------------------------------------------------------------------
//  GetNeoCustomReverbCombGain()
//
//  Get the output gain for a specific comb filter (0-127)
//++------------------------------------------------------------------------------
int GetNeoCustomReverbCombGain(int combIndex)
{
    NeoReverbParams* params = GetNeoReverbParams();
    
    if (combIndex < 0 || combIndex >= NEO_CUSTOM_MAX_COMBS)
        return 0;
    
    // Map gain back to 0-127 range
    return (int)((params->mCustomGain[combIndex] * 127) / NEO_COEFF_MULTIPLY);
}

//++------------------------------------------------------------------------------
//  GetNeoCustomLowpass()
//
//  Get the current low-pass filter coefficient (0-127)
//++------------------------------------------------------------------------------
int GetNeoCustomReverbLowpass()
{
    NeoReverbParams* params = GetNeoReverbParams();
    return (int)((params->mLopassK * 254) / NEO_COEFF_MULTIPLY) + 1;
}

//++------------------------------------------------------------------------------
//  GetNeoReverbMix()
//
//  Get the current wet/dry mix level (0-255)
//++------------------------------------------------------------------------------
int GetNeoReverbMix(void)
{
    NeoReverbParams* params = GetNeoReverbParams();
    // Convert fixed-point gain back to extended level (0-255)
    return (int)((params->mWetGain * 128) / NEO_COEFF_MULTIPLY);
}

// ==============================================================================
// MobileBAE Reverb (Ported from Effects.java)
// ==============================================================================

static const int MOBILE_COMB_RATIO[] = {0x596, 0x642, 0x74B, 0x828, 0x93C, 0xA19};
static const int MOBILE_INPUT_RATIO[] = {0x31C, 0x3FE, 0x441, 0x527, 0x5BF, 0x77A, 0x1F9};
static const int MOBILE_DIFFUSION_GAIN[] = {
    0x0001F39C, 0x0001575E, 0x000138A5, 0x0000EA31, 0x0000C6E6, 0x00008630, 0x0000C000
};
static const int MOBILE_SMOOTHING[] = {
    0x10000, 0xD439, 0xCA7F, 0xBB64, 0xAC08, 0x9DF4, 0x91AA, 0x86A8, 0x7CEE, 0x747B, 0x6CCD
};

static INLINE int32_t mobile_mulShift(int32_t a, int32_t b, int shift) {
    // Java uses 32-bit signed multiply which wraps on overflow. We must replicate this exactly for 1:1 behavior.
    int32_t prod = (int32_t)((uint32_t)a * (uint32_t)b);
    return prod >> shift;
}

static INLINE int32_t mobile_fixedMul16_16(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> 16);
}

static INLINE int32_t mobile_fixedDiv16_16(int32_t a, int32_t b) {
    return (int32_t)((((int64_t)a) << 16) / b);
}

static INLINE int mobile_delayIndex(int sampleRate, int ratio) {
    int delayBase = mobile_fixedMul16_16(ratio, MOBILE_TYPE_SCALE);
    int samples = (int)(((int64_t)sampleRate * delayBase) >> 16);
    return (MOBILE_DELAY_LENGTH - samples) & (MOBILE_DELAY_LENGTH - 1);
}

static int mobile_smoothingGain(int sampleRate) {
    if (sampleRate <= 8000) return 0xD439;
    if (sampleRate >= 48000) return 0x6666;
    int pos = (sampleRate - 8000) / 4000;
    int rem = (sampleRate - 8000) % 4000;
    int a = MOBILE_SMOOTHING[pos + 1];
    int b = (pos + 2 < (sizeof(MOBILE_SMOOTHING)/sizeof(MOBILE_SMOOTHING[0]))) ? MOBILE_SMOOTHING[pos + 2] : 0x6666;
    return a + mobile_fixedMul16_16(b - a, (rem << 16) / 4000);
}

void CheckMobileReverbType(void) {
    int sampleRate;
    if (MusicGlobals && MusicGlobals->outputRate) {
        sampleRate = MusicGlobals->outputRate;
    } else {
        sampleRate = 44100;
    }

    if (!gMobileReverb.initialized || gMobileReverb.sampleRate != sampleRate) {
        gMobileReverb.sampleRate = sampleRate;
        for (int i = 0; i < 7; i++) {
            gMobileReverb.inputIndex[i] = mobile_delayIndex(sampleRate, MOBILE_INPUT_RATIO[i]);
        }
        gMobileReverb.inputIndex[7] = 0;

        for (int i = 0; i < 6; i++) {
            int delayBase = mobile_fixedMul16_16(MOBILE_COMB_RATIO[i], MOBILE_TYPE_SCALE);
            gMobileReverb.combRead[i] = mobile_delayIndex(sampleRate, MOBILE_COMB_RATIO[i]);
            int value = mobile_fixedDiv16_16(delayBase, MOBILE_REVERB_TIME);
            
            // Java: -exp10Q16(-3 * value)
            // exp10Q16(x) = 10^(x / 65536.0) * 65536
            double x = -3.0 * (double)value / 65536.0;
            double expVal = pow(10.0, x) * 65536.0;
            gMobileReverb.combFeedback[i] = (int)-expVal;
        }

        gMobileReverb.earlyRead = (256 - (int)(((int64_t)sampleRate * 0x0126) >> 16)) & 0xFF;
        gMobileReverb.stereoRead = (512 - (int)(((int64_t)sampleRate * 456) >> 16)) & 0x1FF;
        gMobileReverb.wetSmoothingGain = mobile_smoothingGain(sampleRate);
        gMobileReverb.initialized = true;
    }
}

static void addStereoWet(int32_t *stereoMix, int wet) {
    int oldL = gMobileReverb.stereoL[gMobileReverb.stereoRead];
    int oldR = gMobileReverb.stereoR[gMobileReverb.stereoRead];
    int deltaL = mobile_mulShift(26214, wet - oldL, 16);
    int deltaR = mobile_mulShift(26214, oldR - wet, 16);
    
    gMobileReverb.stereoL[gMobileReverb.stereoWrite] = (int16_t)(wet + deltaL);
    gMobileReverb.stereoR[gMobileReverb.stereoWrite] = (int16_t)(wet + deltaR);
    
    int32_t outL = oldL + deltaL;
    int32_t outR = oldR + deltaR;
    
    // Apply the master Neo Reverb mix level
    NeoReverbParams* params = GetNeoReverbParams();
    outL = PV_MulQ16_Round(outL, params->mWetGain);
    outR = PV_MulQ16_Round(outR, params->mWetGain);
    
    // Add to stereo output
    // The Java original uses << 10, but miniBAE's mix headroom is different.
    // Shift << 8 tames the hotness (-12dB) and integrates better with the dry mix.
    stereoMix[0] += outL << 8;
    stereoMix[1] += outR << 8;
    
    gMobileReverb.stereoRead = (gMobileReverb.stereoRead + 1) & 0x1FF;
    gMobileReverb.stereoWrite = (gMobileReverb.stereoWrite + 1) & 0x1FF;
}

void RunMobileReverb(int32_t *sourceP, int32_t *destP, int numFrames) {
    for (int frame = 0; frame < numFrames; frame++) {
        // sourceP is already >> (NEO_INPUTSHIFT+1) from PV_ScaleReverbSend
        // Wait, no. RunMobileReverb takes songBufferReverb directly.
        // Let's use the same shifting as MobileBAE: >> 11
        gMobileReverb.inputDelay[gMobileReverb.inputIndex[7]] = (int16_t)(sourceP[frame] >> 11);
        
        int diffusionSum = 0;
        for (int i = 0; i < 6; i++) {
            diffusionSum += mobile_mulShift(MOBILE_DIFFUSION_GAIN[i], gMobileReverb.inputDelay[gMobileReverb.inputIndex[i]], 16);
        }
        int combFeed = mobile_mulShift(MOBILE_DIFFUSION_GAIN[6], gMobileReverb.inputDelay[gMobileReverb.inputIndex[6]], 16);
        
        for (int i = 0; i < 8; i++) {
            gMobileReverb.inputIndex[i] = (gMobileReverb.inputIndex[i] + 1) & (MOBILE_DELAY_LENGTH - 1);
        }

        int combSum = 0;
        for (int i = 0; i < 6; i++) {
            int old = gMobileReverb.comb[i][gMobileReverb.combRead[i]];
            gMobileReverb.comb[i][gMobileReverb.combWrite[i]] = (int16_t)(combFeed + mobile_mulShift(gMobileReverb.combFeedback[i], old, 16));
            gMobileReverb.combRead[i] = (gMobileReverb.combRead[i] + 1) & (MOBILE_DELAY_LENGTH - 1);
            gMobileReverb.combWrite[i] = (gMobileReverb.combWrite[i] + 1) & (MOBILE_DELAY_LENGTH - 1);
            combSum += old;
        }

        int earlyOld = gMobileReverb.early[gMobileReverb.earlyRead];
        int earlyValue = mobile_mulShift(22937, combSum - 2 * earlyOld, 15);
        gMobileReverb.early[gMobileReverb.earlyWrite] = (int16_t)((combSum + earlyValue) >> 1);
        
        gMobileReverb.earlyRead = (gMobileReverb.earlyRead + 1) & 0xFF;
        gMobileReverb.earlyWrite = (gMobileReverb.earlyWrite + 1) & 0xFF;
        
        int target = diffusionSum + earlyValue + 2 * earlyOld;
        gMobileReverb.wetSmoothingState = (int16_t)(gMobileReverb.wetSmoothingState + mobile_mulShift(gMobileReverb.wetSmoothingGain, target - gMobileReverb.wetSmoothingState, 16));
        
        addStereoWet(&destP[frame * 2], gMobileReverb.wetSmoothingState);
    }
}

#endif  // USE_NEO_EFFECTS
