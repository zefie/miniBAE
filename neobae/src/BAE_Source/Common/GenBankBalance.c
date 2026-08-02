/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GenBankBalance.c — post-load bank RMS scanning and cross-engine mix scales.
 */

#include "GenBankBalance.h"
#include "GenPriv.h"
#include "X_Formats.h"
#include "X_Assert.h"

#include <math.h>
#include <string.h>

#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
#include "GenSF2_FluidLite.h"
#endif
#if USE_NATIVE_DLS == TRUE
#include "GenDLS_MobileBAE.h"
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Cap work so large banks stay interactive; stride keeps RMS cheap. */
enum
{
    kBankBalanceMaxHsbTrack = 8,
    kBankBalanceMaxRmfTrack = MAX_SONGS,
    kBankBalanceMaxHsbInstruments = 96,
    kBankBalanceMaxValues = 128,
    kBankBalancePcmStride = 8,
    kBankBalanceProbeKey = 60,
    kBankBalanceProbeVel = 64
};

/* Match existing engine insertion gain so sample RMS compares fairly.
 * HSB/RMF: HSB_MAX_OUTPUT_SCALE (0.5) via scaleBackAmount.
 * DLS path gain (0.25 for OUTPUT_SCALAR-2) is applied in GM_DLS_MeasureBankLoudness. */
enum
{
    kHsbPathGainNum = 1,
    kHsbPathGainDen = 2 /* HSB_MAX_OUTPUT_SCALE = 0.5 */
};

static const float kMinLoudness = 1.0e-6f;
static const float kScaleMin = 0.05f; /* ~-26 dB — hot RMF one-shots vs quiet DLS */
static const float kScaleMax = 1.0f;  /* match-quietest: never boost, only attenuate */

static XFILE g_hsbFiles[kBankBalanceMaxHsbTrack];
static int g_hsbCount = 0;

/* RMF/ZMF embedded instruments share the native (HSB) mix path. */
static GM_Song *g_rmfSongs[kBankBalanceMaxRmfTrack];
static float g_rmfSongLoudness[kBankBalanceMaxRmfTrack];
static int g_rmfCount = 0;
static float g_rmfLoudness = 0.0f; /* combined across tracked RMF songs */
static float g_hsbBankLoudness = 0.0f; /* host HSB/ZSB bank only */

static bool g_present[GM_BANK_ENGINE_COUNT];
static bool g_scanned[GM_BANK_ENGINE_COUNT];
static float g_loudness[GM_BANK_ENGINE_COUNT]; /* effective linear loudness */
static float g_scale[GM_BANK_ENGINE_COUNT] = {1.0f, 1.0f, 1.0f, 1.0f};
static bool g_active = FALSE;

static void PV_UpdateHsbPresentAndLoudness(void);

static float PV_Clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static float PV_MedianInPlace(float *values, int count)
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

/* Peak-aware loudness: full-buffer RMS underestimates short hot one-shots
 * padded with silence. Use RMS of samples above 10% of peak, floored by
 * a fraction of peak so transient hits still dominate. */
static float PV_BankBalance_LoudnessInt16(const int16_t *pcm, uint32_t frames, int channels, int stride)
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
            if (s < 0.0)
                s = -s;
            if (s > peak)
                peak = s;
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

static float PV_BankBalance_LoudnessU8(const unsigned char *pcm, uint32_t frames, int channels, int stride)
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
            double s = ((double)pcm[f * (uint32_t)channels + (uint32_t)c] - 128.0) * (1.0 / 128.0);
            if (s < 0.0)
                s = -s;
            if (s > peak)
                peak = s;
        }
    }

    thresh = peak * 0.10;
    for (f = 0; f < frames; f += (uint32_t)stride)
    {
        int c;
        for (c = 0; c < channels; c++)
        {
            double s = ((double)pcm[f * (uint32_t)channels + (uint32_t)c] - 128.0) * (1.0 / 128.0);
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

static void PV_ResetScalesInactive(void)
{
    int i;
    for (i = 0; i < GM_BANK_ENGINE_COUNT; i++)
        g_scale[i] = 1.0f;
    g_active = FALSE;
}

static XPTR PV_LoadHsbSndResource(XFILE bankFile, XShortResourceID sndID, int32_t *outSize)
{
    static const XResourceType sndTypes[] = {ID_SND, ID_CSND, ID_ESND};
    int t;

    if (outSize)
        *outSize = 0;

    for (t = 0; t < 3; t++)
    {
        int32_t sndSize = 0;
        XPTR sndData = XGetFileResource(bankFile, sndTypes[t], (XLongResourceID)sndID, NULL, &sndSize);
        if (!sndData)
            continue;

        if (sndTypes[t] == ID_CSND)
        {
            XPTR decompressed = XDecompressPtr(sndData, (uint32_t)sndSize, FALSE);
            XDisposePtr(sndData);
            sndData = decompressed;
            if (sndData)
                sndSize = (int32_t)XGetPtrSize(sndData);
        }
        else if (sndTypes[t] == ID_ESND)
        {
            XDecryptData(sndData, (uint32_t)sndSize);
        }

        if (sndData && outSize)
            *outSize = sndSize;
        return sndData;
    }
    return NULL;
}

static float PV_MeasureHsbSndLoudness(XFILE bankFile, XShortResourceID sndID, int16_t volumeParam)
{
    int32_t sndSize = 0;
    XPTR sndData;
    SampleDataInfo info;
    XPTR pcm;
    float rms = 0.0f;
    float volScale;

    sndData = PV_LoadHsbSndResource(bankFile, sndID, &sndSize);
    if (!sndData || sndSize <= 0)
        return 0.0f;

    XSetMemory(&info, (int32_t)sizeof(info), 0);
    pcm = XGetSamplePtrFromSnd(sndData, &info);
    if (pcm && info.frames > 0)
    {
        if (info.bitSize == 16)
            rms = PV_BankBalance_LoudnessInt16((const int16_t *)pcm, info.frames, info.channels, kBankBalancePcmStride);
        else
            rms = PV_BankBalance_LoudnessU8((const unsigned char *)pcm, info.frames, info.channels, kBankBalancePcmStride);
    }

    if (info.pMasterPtr && info.pMasterPtr != sndData)
        XDisposePtr(info.pMasterPtr);
    XDisposePtr(sndData);

    volScale = (float)((volumeParam > 0) ? volumeParam : 100) / 100.0f;
    return rms * volScale;
}

static float PV_ScanHsbBankLoudness(XFILE bankFile)
{
    float values[kBankBalanceMaxValues];
    int valueCount = 0;
    int32_t instCount;
    int32_t step;
    int32_t i;

    if (!bankFile)
        return 0.0f;

    instCount = XCountFileResourcesOfType(bankFile, ID_INST);
    if (instCount <= 0)
        return 0.0f;

    step = 1;
    if (instCount > kBankBalanceMaxHsbInstruments)
        step = (instCount + kBankBalanceMaxHsbInstruments - 1) / kBankBalanceMaxHsbInstruments;

    for (i = 0; i < instCount && valueCount < kBankBalanceMaxValues; i += step)
    {
        XLongResourceID instID = 0;
        int32_t instSize = 0;
        XPTR instData;
        InstrumentResource *inst;
        int16_t keySplitCount;
        XShortResourceID sndID;
        int16_t volumeParam;
        float loud;
        int32_t bankNum;

        instData = XGetIndexedFileResource(bankFile, ID_INST, &instID, i, NULL, &instSize);
        if (!instData || instSize < 14)
        {
            if (instData)
                XDisposePtr(instData);
            continue;
        }

        /* Prefer melodic (non-percussion-style) banks for balance reference. */
        bankNum = (int32_t)(instID / 256);
        if (bankNum == 1 || bankNum == 128)
        {
            XDisposePtr(instData);
            continue;
        }

        inst = (InstrumentResource *)instData;
        {
            const unsigned char *ip = (const unsigned char *)instData;
            uint8_t flags2 = ip[6];
            keySplitCount = (int16_t)XGetShort(ip + 12);
            sndID = 0;
            volumeParam = (int16_t)XGetShort(ip + 10);

            if (keySplitCount > 0)
            {
                int s;
                int chosen = -1;
                for (s = 0; s < keySplitCount; s++)
                {
                    KeySplit split;
                    XGetKeySplitFromPtr(inst, (int16_t)s, &split);
                    if (split.lowMidi <= kBankBalanceProbeKey && split.highMidi >= kBankBalanceProbeKey)
                    {
                        chosen = s;
                        break;
                    }
                }
                if (chosen < 0)
                    chosen = keySplitCount / 2;
                {
                    KeySplit split;
                    XGetKeySplitFromPtr(inst, (int16_t)chosen, &split);
                    sndID = split.sndResourceID;
                    /* miscParameter2 is volume unless it's a sound-modifier param. */
                    if ((flags2 & ZBF_enableSoundModifier) && !(flags2 & ZBF_useSoundModifierAsRootKey))
                        volumeParam = 100;
                    else
                        volumeParam = split.miscParameter2;
                }
            }
            else
            {
                sndID = (XShortResourceID)XGetShort(ip + 0);
                if ((flags2 & ZBF_enableSoundModifier) && !(flags2 & ZBF_useSoundModifierAsRootKey))
                    volumeParam = 100;
            }
            if (volumeParam <= 0)
                volumeParam = 100;
        }

        XDisposePtr(instData);

        if (sndID == 0)
            continue;

        loud = PV_MeasureHsbSndLoudness(bankFile, sndID, volumeParam);
        if (loud > kMinLoudness)
            values[valueCount++] = loud;
    }

    if (valueCount <= 0)
        return 0.0f;

    return PV_MedianInPlace(values, valueCount) * ((float)kHsbPathGainNum / (float)kHsbPathGainDen);
}

static float PV_MeasureWaveformLoudness(const GM_Waveform *wave, int16_t volumeParam)
{
    float level = 0.0f;
    float volScale;
    int channels;

    if (!wave || !wave->theWaveform || wave->waveFrames == 0)
        return 0.0f;

    channels = wave->channels > 0 ? (int)wave->channels : 1;
    if (wave->bitSize == 16)
        level = PV_BankBalance_LoudnessInt16((const int16_t *)wave->theWaveform, wave->waveFrames, channels, kBankBalancePcmStride);
    else
        level = PV_BankBalance_LoudnessU8((const unsigned char *)wave->theWaveform, wave->waveFrames, channels, kBankBalancePcmStride);

    volScale = (float)((volumeParam > 0) ? volumeParam : 100) / 100.0f;
    return level * volScale;
}

static float PV_MeasureInstrumentLoudness(GM_Instrument *inst)
{
    int16_t volumeParam;

    if (!inst)
        return 0.0f;

    volumeParam = inst->miscParameter2;
    if (volumeParam <= 0)
        volumeParam = 100;

    if (inst->doKeymapSplit)
    {
        uint16_t splitCount = inst->u.k.KeymapSplitCount;
        int chosen = -1;
        uint16_t s;

        if (splitCount == 0)
            return 0.0f;

        for (s = 0; s < splitCount; s++)
        {
            GM_KeymapSplit *split = &inst->u.k.keySplits[s];
            if (split->lowMidi <= kBankBalanceProbeKey && split->highMidi >= kBankBalanceProbeKey)
            {
                chosen = (int)s;
                break;
            }
        }
        if (chosen < 0)
            chosen = (int)(splitCount / 2);

        {
            GM_KeymapSplit *split = &inst->u.k.keySplits[chosen];
            if (!split->pSplitInstrument)
                return 0.0f;
            if (!(inst->enableSoundModifier && !inst->useSoundModifierAsRootKey))
                volumeParam = split->miscParameter2;
            if (volumeParam <= 0)
                volumeParam = 100;
            return PV_MeasureWaveformLoudness(&split->pSplitInstrument->u.w, volumeParam);
        }
    }

    return PV_MeasureWaveformLoudness(&inst->u.w, volumeParam);
}

static float PV_ScanRmfSongLoudness(GM_Song *pSong)
{
    float values[kBankBalanceMaxValues];
    int valueCount = 0;
    int i;
    uint32_t embeddedCount;

    if (!pSong)
        return 0.0f;

    embeddedCount = pSong->RMFInstrumentIDs[0];
    if (embeddedCount > 0 && embeddedCount <= MAX_INSTRUMENTS)
    {
        /* Prefer the explicit embedded-instrument list when available.
         * Include all banks — RMF one-shots are often on perc/user banks. */
        for (i = 1; i <= (int)embeddedCount && valueCount < kBankBalanceMaxValues; i++)
        {
            uint32_t instId = pSong->RMFInstrumentIDs[i];
            GM_Instrument *inst;
            float loud;

            if (instId >= (uint32_t)(MAX_INSTRUMENTS * MAX_BANKS))
                continue;

            inst = pSong->instrumentData[instId];
            loud = PV_MeasureInstrumentLoudness(inst);
            if (loud > kMinLoudness)
                values[valueCount++] = loud;
        }
    }
    else
    {
        for (i = 0; i < MAX_INSTRUMENTS * MAX_BANKS && valueCount < kBankBalanceMaxValues; i++)
        {
            GM_Instrument *inst = pSong->instrumentData[i];
            float loud;

            if (!inst)
                continue;

            loud = PV_MeasureInstrumentLoudness(inst);
            if (loud > kMinLoudness)
                values[valueCount++] = loud;
        }
    }

    if (valueCount <= 0)
        return 0.0f;

    /* For sparse RMF embeds, the loudest instrument dominates mix perception. */
    if (valueCount <= 4)
    {
        float peak = values[0];
        for (i = 1; i < valueCount; i++)
        {
            if (values[i] > peak)
                peak = values[i];
        }
        return peak * ((float)kHsbPathGainNum / (float)kHsbPathGainDen);
    }

    return PV_MedianInPlace(values, valueCount) * ((float)kHsbPathGainNum / (float)kHsbPathGainDen);
}

static float PV_CombineRmfLoudness(void)
{
    int i;
    int n = 0;
    float logSum = 0.0f;

    for (i = 0; i < g_rmfCount; i++)
    {
        if (g_rmfSongLoudness[i] > kMinLoudness)
        {
            logSum += logf(g_rmfSongLoudness[i]);
            n++;
        }
    }
    if (n <= 0)
        return 0.0f;
    return expf(logSum / (float)n);
}

static void PV_UpdateHsbPresentAndLoudness(void)
{
    float combined;

    g_rmfLoudness = PV_CombineRmfLoudness();

    /* Prefer embedded RMF loudness when present. Averaging with the host HSB
     * bank dilutes a hot one-shot (e.g. Free.rmf) and under-attenuates it. */
    if (g_rmfLoudness > kMinLoudness)
        combined = g_rmfLoudness;
    else
        combined = g_hsbBankLoudness;

    if (g_hsbCount > 0 || g_rmfCount > 0)
    {
        g_present[GM_BANK_ENGINE_HSB] = TRUE;
        g_loudness[GM_BANK_ENGINE_HSB] = combined;
        /* Scanned if we have RMF data and/or already scanned the host bank. */
        g_scanned[GM_BANK_ENGINE_HSB] = (g_rmfCount > 0) || (g_hsbCount > 0 && g_hsbBankLoudness > kMinLoudness);
    }
    else
    {
        g_present[GM_BANK_ENGINE_HSB] = FALSE;
        g_scanned[GM_BANK_ENGINE_HSB] = FALSE;
        g_loudness[GM_BANK_ENGINE_HSB] = 0.0f;
        g_hsbBankLoudness = 0.0f;
    }
}

static void PV_BankBalance_Recalculate(void)
{
    int i;
    int presentCount = 0;
    int measuredCount = 0;
    float target;
    const char *names[GM_BANK_ENGINE_COUNT] = {"HSB/RMF", "SF2", "DLS", "DLS/XMF"};

    PV_UpdateHsbPresentAndLoudness();

    for (i = 0; i < GM_BANK_ENGINE_COUNT; i++)
    {
        if (g_present[i])
            presentCount++;
    }

    if (presentCount < 2)
    {
        PV_ResetScalesInactive();
        return;
    }

    /* Lazy scan host HSB bank when a second engine is present. */
    if (g_hsbCount > 0 && g_hsbBankLoudness <= kMinLoudness)
    {
        XFILE file = g_hsbFiles[g_hsbCount - 1];
        g_hsbBankLoudness = PV_ScanHsbBankLoudness(file);
        debug_message("[BankBalance] HSB bank loudness=%.6f (%d files)\n", g_hsbBankLoudness, g_hsbCount);
        PV_UpdateHsbPresentAndLoudness();
    }

    if (g_present[GM_BANK_ENGINE_HSB])
    {
        g_scanned[GM_BANK_ENGINE_HSB] = TRUE;
        debug_message("[BankBalance] HSB/RMF loudness=%.6f (bank=%.6f rmf=%.6f songs=%d)\n",
                      g_loudness[GM_BANK_ENGINE_HSB], g_hsbBankLoudness, g_rmfLoudness, g_rmfCount);
    }

#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
    if (g_present[GM_BANK_ENGINE_SF2] && !g_scanned[GM_BANK_ENGINE_SF2])
    {
        float loud = GM_SF2_MeasureBankLoudness();
        g_loudness[GM_BANK_ENGINE_SF2] = loud;
        g_scanned[GM_BANK_ENGINE_SF2] = TRUE;
        debug_message("[BankBalance] SF2 loudness=%.6f\n", loud);
    }
#endif

#if USE_NATIVE_DLS == TRUE
    if (g_present[GM_BANK_ENGINE_DLS] && !g_scanned[GM_BANK_ENGINE_DLS])
    {
        g_scanned[GM_BANK_ENGINE_DLS] = TRUE;
        debug_message("[BankBalance] DLS loudness=%.6f\n", g_loudness[GM_BANK_ENGINE_DLS]);
    }
    if (g_present[GM_BANK_ENGINE_DLS_XMF] && !g_scanned[GM_BANK_ENGINE_DLS_XMF])
    {
        g_scanned[GM_BANK_ENGINE_DLS_XMF] = TRUE;
        debug_message("[BankBalance] DLS/XMF loudness=%.6f\n", g_loudness[GM_BANK_ENGINE_DLS_XMF]);
    }
#endif

    target = 0.0f;
    for (i = 0; i < GM_BANK_ENGINE_COUNT; i++)
    {
        if (g_present[i] && g_loudness[i] > kMinLoudness)
        {
            if (measuredCount == 0 || g_loudness[i] < target)
                target = g_loudness[i];
            measuredCount++;
        }
    }

    if (measuredCount < 2)
    {
        PV_ResetScalesInactive();
        return;
    }

    /* Match quieter engine: attenuate louder engines down, never boost. */
    for (i = 0; i < GM_BANK_ENGINE_COUNT; i++)
    {
        if (g_present[i] && g_loudness[i] > kMinLoudness)
            g_scale[i] = PV_Clampf(target / g_loudness[i], kScaleMin, kScaleMax);
        else
            g_scale[i] = 1.0f;
    }
    g_active = TRUE;

    for (i = 0; i < GM_BANK_ENGINE_COUNT; i++)
    {
        if (g_present[i])
        {
            debug_message("[BankBalance] %s scale=%.3f (loud=%.6f target=%.6f)\n",
                          names[i], g_scale[i], g_loudness[i], target);
        }
    }
}

void GM_BankBalance_OnHsbBankAdded(XFILE file)
{
    int i;
    if (!file)
        return;

    for (i = 0; i < g_hsbCount; i++)
    {
        if (g_hsbFiles[i] == file)
        {
            /* Move to end so it becomes the preferred scan target. */
            if (i != g_hsbCount - 1)
            {
                int j;
                for (j = i; j < g_hsbCount - 1; j++)
                    g_hsbFiles[j] = g_hsbFiles[j + 1];
                g_hsbFiles[g_hsbCount - 1] = file;
            }
            g_hsbBankLoudness = 0.0f; /* force rescan */
            g_scanned[GM_BANK_ENGINE_HSB] = FALSE;
            PV_BankBalance_Recalculate();
            return;
        }
    }

    if (g_hsbCount < kBankBalanceMaxHsbTrack)
    {
        g_hsbFiles[g_hsbCount++] = file;
    }
    else
    {
        /* Drop oldest. */
        int j;
        for (j = 0; j < kBankBalanceMaxHsbTrack - 1; j++)
            g_hsbFiles[j] = g_hsbFiles[j + 1];
        g_hsbFiles[kBankBalanceMaxHsbTrack - 1] = file;
    }

    g_hsbBankLoudness = 0.0f;
    g_scanned[GM_BANK_ENGINE_HSB] = FALSE;
    PV_BankBalance_Recalculate();
}

void GM_BankBalance_OnHsbBankRemoved(XFILE file)
{
    int i;
    int j;
    if (!file)
        return;

    for (i = 0; i < g_hsbCount; i++)
    {
        if (g_hsbFiles[i] == file)
        {
            for (j = i; j < g_hsbCount - 1; j++)
                g_hsbFiles[j] = g_hsbFiles[j + 1];
            g_hsbCount--;
            break;
        }
    }

    g_hsbBankLoudness = 0.0f;
    g_scanned[GM_BANK_ENGINE_HSB] = FALSE;
    if (g_hsbCount <= 0 && g_rmfCount <= 0)
        g_scale[GM_BANK_ENGINE_HSB] = 1.0f;
    PV_BankBalance_Recalculate();
}

void GM_BankBalance_OnRmfInstrumentsLoaded(struct GM_Song *pSong)
{
    float loud;
    int i;

    if (!pSong)
        return;

    loud = PV_ScanRmfSongLoudness(pSong);
    if (loud <= kMinLoudness)
    {
        debug_message("[BankBalance] RMF song %p: no measurable embedded instruments\n", (void *)pSong);
        return;
    }

    for (i = 0; i < g_rmfCount; i++)
    {
        if (g_rmfSongs[i] == pSong)
        {
            g_rmfSongLoudness[i] = loud;
            debug_message("[BankBalance] RMF song %p updated loudness=%.6f\n", (void *)pSong, loud);
            PV_BankBalance_Recalculate();
            return;
        }
    }

    if (g_rmfCount < kBankBalanceMaxRmfTrack)
    {
        g_rmfSongs[g_rmfCount] = pSong;
        g_rmfSongLoudness[g_rmfCount] = loud;
        g_rmfCount++;
    }
    else
    {
        /* Replace oldest entry. */
        int j;
        for (j = 0; j < kBankBalanceMaxRmfTrack - 1; j++)
        {
            g_rmfSongs[j] = g_rmfSongs[j + 1];
            g_rmfSongLoudness[j] = g_rmfSongLoudness[j + 1];
        }
        g_rmfSongs[kBankBalanceMaxRmfTrack - 1] = pSong;
        g_rmfSongLoudness[kBankBalanceMaxRmfTrack - 1] = loud;
    }

    debug_message("[BankBalance] RMF song %p loudness=%.6f (tracked=%d)\n", (void *)pSong, loud, g_rmfCount);
    PV_BankBalance_Recalculate();
}

void GM_BankBalance_OnRmfInstrumentsUnloaded(struct GM_Song *pSong)
{
    int i;
    int j;

    if (!pSong)
        return;

    for (i = 0; i < g_rmfCount; i++)
    {
        if (g_rmfSongs[i] == pSong)
        {
            for (j = i; j < g_rmfCount - 1; j++)
            {
                g_rmfSongs[j] = g_rmfSongs[j + 1];
                g_rmfSongLoudness[j] = g_rmfSongLoudness[j + 1];
            }
            g_rmfCount--;
            debug_message("[BankBalance] RMF song %p unloaded (tracked=%d)\n", (void *)pSong, g_rmfCount);
            if (g_hsbCount <= 0 && g_rmfCount <= 0)
                g_scale[GM_BANK_ENGINE_HSB] = 1.0f;
            PV_BankBalance_Recalculate();
            return;
        }
    }
}

void GM_BankBalance_OnSf2Loaded(void)
{
    g_present[GM_BANK_ENGINE_SF2] = TRUE;
    g_scanned[GM_BANK_ENGINE_SF2] = FALSE;
    g_loudness[GM_BANK_ENGINE_SF2] = 0.0f;
    PV_BankBalance_Recalculate();
}

void GM_BankBalance_OnSf2Unloaded(void)
{
    g_present[GM_BANK_ENGINE_SF2] = FALSE;
    g_scanned[GM_BANK_ENGINE_SF2] = FALSE;
    g_loudness[GM_BANK_ENGINE_SF2] = 0.0f;
    g_scale[GM_BANK_ENGINE_SF2] = 1.0f;
    PV_BankBalance_Recalculate();
}

void GM_BankBalance_OnDlsBanksChanged(struct GM_Mixer *pMixer)
{
#if USE_NATIVE_DLS == TRUE
    DLS_Synth *synth;

    g_present[GM_BANK_ENGINE_DLS] = FALSE;
    g_scanned[GM_BANK_ENGINE_DLS] = FALSE;
    g_loudness[GM_BANK_ENGINE_DLS] = 0.0f;
    g_scale[GM_BANK_ENGINE_DLS] = 1.0f;
    g_present[GM_BANK_ENGINE_DLS_XMF] = FALSE;
    g_scanned[GM_BANK_ENGINE_DLS_XMF] = FALSE;
    g_loudness[GM_BANK_ENGINE_DLS_XMF] = 0.0f;
    g_scale[GM_BANK_ENGINE_DLS_XMF] = 1.0f;

    if (!pMixer || !pMixer->pDLSSynth)
    {
        PV_BankBalance_Recalculate();
        return;
    }

    synth = (DLS_Synth *)pMixer->pDLSSynth;

    /* banks[0] = main DLS bank; banks[1] = XMF overlay — tracked separately
     * so each can match against HSB/SF2/each other independently. */
    if (synth->banks[0])
    {
        float l = GM_DLS_MeasureBankLoudness(synth->banks[0]);
        if (l > kMinLoudness)
        {
            g_present[GM_BANK_ENGINE_DLS] = TRUE;
            g_scanned[GM_BANK_ENGINE_DLS] = TRUE;
            g_loudness[GM_BANK_ENGINE_DLS] = l;
        }
    }
    if (synth->banks[1])
    {
        float l = GM_DLS_MeasureBankLoudness(synth->banks[1]);
        if (l > kMinLoudness)
        {
            g_present[GM_BANK_ENGINE_DLS_XMF] = TRUE;
            g_scanned[GM_BANK_ENGINE_DLS_XMF] = TRUE;
            g_loudness[GM_BANK_ENGINE_DLS_XMF] = l;
        }
    }

    PV_BankBalance_Recalculate();
#else
    (void)pMixer;
#endif
}

float GM_BankBalance_GetMixScale(GM_BankEngine engine)
{
    if ((int)engine < 0 || (int)engine >= GM_BANK_ENGINE_COUNT)
        return 1.0f;
    if (!g_active)
        return 1.0f;
    return g_scale[engine];
}

bool GM_BankBalance_IsActive(void)
{
    return g_active;
}

/* ---- MIDI+patch song peak estimate -------------------------------------- */

enum { kEstimateMaxChannels = 16 };

static float s_estAmp[kEstimateMaxChannels][128];
static double s_estEnergy = 0.0;
static float s_estMaxPeak = 0.0f;

void GM_EstimatePeak_Reset(void)
{
    XSetMemory(s_estAmp, sizeof(s_estAmp), 0);
    s_estEnergy = 0.0;
    s_estMaxPeak = 0.0f;
}

float GM_EstimatePeak_GetMax(void)
{
    return s_estMaxPeak;
}

static float PV_MeasureInstrumentLoudnessAtKey(GM_Instrument *inst, int16_t key)
{
    int16_t volumeParam;

    if (!inst)
        return 0.0f;

    volumeParam = inst->miscParameter2;
    if (volumeParam <= 0)
        volumeParam = 100;

    if (inst->doKeymapSplit)
    {
        uint16_t splitCount = inst->u.k.KeymapSplitCount;
        int chosen = -1;
        uint16_t s;

        if (splitCount == 0)
            return 0.0f;

        for (s = 0; s < splitCount; s++)
        {
            GM_KeymapSplit *split = &inst->u.k.keySplits[s];
            if (split->lowMidi <= key && split->highMidi >= key)
            {
                chosen = (int)s;
                break;
            }
        }
        if (chosen < 0)
            chosen = (int)(splitCount / 2);

        {
            GM_KeymapSplit *split = &inst->u.k.keySplits[chosen];
            if (!split->pSplitInstrument)
                return 0.0f;
            if (!(inst->enableSoundModifier && !inst->useSoundModifierAsRootKey))
                volumeParam = split->miscParameter2;
            if (volumeParam <= 0)
                volumeParam = 100;
            return PV_MeasureWaveformLoudness(&split->pSplitInstrument->u.w, volumeParam);
        }
    }

    return PV_MeasureWaveformLoudness(&inst->u.w, volumeParam);
}

static int16_t PV_EstimateConvertPatchBank(GM_Song *pSong, int16_t thePatch, int16_t theChannel)
{
    int16_t theBank = pSong->channelBank[theChannel];

    switch (pSong->channelBankMode[theChannel])
    {
    default:
    case USE_GM_DEFAULT:
        if (theChannel == PERCUSSION_CHANNEL)
            theBank = (theBank * 2) + 1;
        else
            theBank = theBank * 2 + 0;
        if (theBank < MAX_BANKS)
            thePatch = (theBank * 128) + thePatch;
        break;
    case USE_NON_GM_PERC_BANK:
    case USE_GM_PERC_BANK:
        theBank = (theBank * 2) + 1;
        if (theBank < MAX_BANKS)
            thePatch = (theBank * 128) + thePatch;
        break;
    case USE_NORM_BANK:
        theBank = theBank * 2 + 0;
        if (theBank < MAX_BANKS)
            thePatch = (theBank * 128) + thePatch;
        break;
    }
    return thePatch;
}

static int16_t PV_EstimateInstrumentToUse(GM_Song *pSong, int16_t midiNote, int16_t MIDIChannel)
{
    int16_t thePatch = 0;

    if (pSong->defaultPercusionProgram < 0)
    {
        switch (pSong->channelBankMode[MIDIChannel])
        {
        case USE_GM_DEFAULT:
            if (MIDIChannel == PERCUSSION_CHANNEL)
                thePatch = PV_EstimateConvertPatchBank(pSong, midiNote, MIDIChannel);
            else
                thePatch = PV_EstimateConvertPatchBank(pSong, pSong->channelProgram[MIDIChannel], MIDIChannel);
            break;
        case USE_NON_GM_PERC_BANK:
        case USE_NORM_BANK:
            thePatch = PV_EstimateConvertPatchBank(pSong, pSong->channelProgram[MIDIChannel], MIDIChannel);
            break;
        case USE_GM_PERC_BANK:
            thePatch = PV_EstimateConvertPatchBank(pSong, midiNote, MIDIChannel);
            break;
        }
    }
    else
    {
        thePatch = pSong->channelProgram[MIDIChannel];
    }
    return thePatch;
}

float GM_EstimateNoteLoudness(GM_Song *pSong, int16_t channel, int16_t note, int16_t velocity)
{
    float level = 0.0f;
    GM_BankEngine engine = GM_BANK_ENGINE_HSB;
    int16_t thePatch;

    if (!pSong || channel < 0 || channel >= kEstimateMaxChannels)
        return 0.0f;
    if (note < 0)
        note = 0;
    if (note > 127)
        note = 127;
    (void)velocity;

#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
    if ((GM_IsSF2Song(pSong) || pSong->channelType[channel] == CHANNEL_TYPE_SF2) &&
        pSong->channelType[channel] != CHANNEL_TYPE_DLS &&
        pSong->channelType[channel] != CHANNEL_TYPE_RMF)
    {
        int sf2Bank = (int)pSong->channelBank[channel];
        int sf2Prog = pSong->channelProgram[channel];

        if (sf2Prog < 0)
            sf2Prog = 0;
        sf2Prog &= 0x7F;

        if (channel == PERCUSSION_CHANNEL ||
            pSong->channelBankMode[channel] == USE_GM_PERC_BANK)
        {
            if (sf2Bank == 0 || sf2Bank == 121)
                sf2Bank = 128;
        }
        else if (sf2Bank == 121)
        {
            sf2Bank = 0;
        }

        level = GM_SF2_EstimateNoteLoudness(sf2Bank, sf2Prog, note, velocity);
        if (level <= kMinLoudness && sf2Bank != 0 &&
            channel != PERCUSSION_CHANNEL &&
            pSong->channelBankMode[channel] != USE_GM_PERC_BANK)
        {
            level = GM_SF2_EstimateNoteLoudness(0, sf2Prog, note, velocity);
        }
        engine = GM_BANK_ENGINE_SF2;
        if (level > kMinLoudness)
            return level * GM_BankBalance_GetMixScale(engine);
        /* Fall through to HSB/RMF if SF2 zone missing. */
    }
#endif

#if USE_NATIVE_DLS == TRUE
    if ((GM_IsDLSSong(pSong) || pSong->channelType[channel] == CHANNEL_TYPE_DLS) &&
        pSong->channelType[channel] != CHANNEL_TYPE_RMF)
    {
        level = GM_DLS_EstimateNoteLoudness(pSong, channel, note, velocity);
        if (level > kMinLoudness)
        {
            float scaleMain = GM_BankBalance_GetMixScale(GM_BANK_ENGINE_DLS);
            float scaleXmf = GM_BankBalance_GetMixScale(GM_BANK_ENGINE_DLS_XMF);
            /* Overlay lookup is preferred in FindInstrument; use quieter scale when both active. */
            float scale = scaleMain;
            if (g_present[GM_BANK_ENGINE_DLS_XMF] && scaleXmf < scaleMain)
                scale = scaleXmf;
            return level * scale;
        }
        /* Fall through to HSB/RMF if DLS instrument missing. */
    }
#endif

    thePatch = PV_EstimateInstrumentToUse(pSong, note, channel);
    if (thePatch >= 0 && thePatch < (MAX_INSTRUMENTS * MAX_BANKS))
    {
        level = PV_MeasureInstrumentLoudnessAtKey(pSong->instrumentData[thePatch], note);
        level *= ((float)kHsbPathGainNum / (float)kHsbPathGainDen);
    }
    return level * GM_BankBalance_GetMixScale(GM_BANK_ENGINE_HSB);
}

static void PV_EstimatePeak_UpdateMax(void)
{
    float peak = (s_estEnergy > 0.0) ? (float)sqrt(s_estEnergy) : 0.0f;
    if (peak > s_estMaxPeak)
        s_estMaxPeak = peak;
}

void GM_EstimatePeak_NoteOn(GM_Song *pSong, int16_t channel, int16_t note, int16_t velocity)
{
    float L;
    float volScale;
    float exprScale;
    float velScale;
    float amp;
    float prev;

    if (!pSong || channel < 0 || channel >= kEstimateMaxChannels)
        return;
    if (note < 0 || note > 127 || velocity <= 0)
        return;

    L = GM_EstimateNoteLoudness(pSong, channel, note, velocity);
    /* Missing patch/zone: skip rather than floor — a tiny floor stacks into
     * a bogus max peak and drives normalize gain to the clamp. */
    if (L <= kMinLoudness)
        return;

    volScale = (float)pSong->channelVolume[channel] / (float)MAX_NOTE_VOLUME;
    if (volScale < 0.0f)
        volScale = 0.0f;
    if (volScale > 1.0f)
        volScale = 1.0f;

    /* Expression 0 means "unset / unity" in the native mixer path. */
    if (pSong->channelExpression[channel] == 0)
        exprScale = 1.0f;
    else
        exprScale = (float)pSong->channelExpression[channel] / (float)MAX_NOTE_VOLUME;

    velScale = (float)velocity / (float)MAX_NOTE_VOLUME;
    amp = L * velScale * volScale * exprScale;

    prev = s_estAmp[channel][note];
    if (prev > 0.0f)
        s_estEnergy -= (double)prev * (double)prev;
    s_estAmp[channel][note] = amp;
    s_estEnergy += (double)amp * (double)amp;
    if (s_estEnergy < 0.0)
        s_estEnergy = 0.0;
    PV_EstimatePeak_UpdateMax();
}

void GM_EstimatePeak_NoteOff(GM_Song *pSong, int16_t channel, int16_t note)
{
    float prev;

    (void)pSong;
    if (channel < 0 || channel >= kEstimateMaxChannels)
        return;
    if (note < 0 || note > 127)
        return;

    prev = s_estAmp[channel][note];
    if (prev <= 0.0f)
        return;
    s_estEnergy -= (double)prev * (double)prev;
    if (s_estEnergy < 0.0)
        s_estEnergy = 0.0;
    s_estAmp[channel][note] = 0.0f;
}
