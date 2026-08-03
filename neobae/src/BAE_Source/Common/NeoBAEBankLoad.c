/*
 * Shared client helpers used by playbae, zefidi, foobar2000, Android:
 * bank load, normalize apply, offline-render priming.
 * © 2026 zefie — LGPL-3.0-or-later
 */

#include "NeoBAE.h"
#include "GenSnd.h"
#include "X_API.h"
#include "BAE_API.h"

#include <string.h>

enum
{
    BAE_EXPORT_PRIME_SLICES = 8,
    BAE_EXPORT_PRIME_MAX_WAIT_SLICES = 32,
    BAE_EXPORT_PRIME_WAIT_US = 2000
};

#if USE_SF2_SUPPORT == TRUE
#include "GenSF2_FluidLite.h"
#endif
#if USE_NATIVE_DLS == TRUE
#include "GenDLS_MobileBAE.h"
#endif

#if defined(_WIN32) || defined(WIN32)
#define PV_BANK_STRICMP _stricmp
#else
#define PV_BANK_STRICMP strcasecmp
#endif

static void PV_BankLoadInfoClear(BAEBankLoadInfo *info)
{
    if (info)
    {
        info->token = 0;
        info->kind = BAE_BANK_KIND_UNKNOWN;
    }
}

static const char *PV_PathExtension(const char *path)
{
    const char *ext;
    if (!path)
        return NULL;
    ext = strrchr(path, '.');
    return ext ? ext : NULL;
}

static BAE_BOOL PV_ExtIsSF2(const char *ext)
{
    if (!ext)
        return FALSE;
    if (PV_BANK_STRICMP(ext, ".sf2") == 0)
        return TRUE;
#if USE_SF2_SUPPORT == TRUE
    /* Accept SF3/SFO aliases; load fails cleanly if SF3 support is disabled. */
    if (PV_BANK_STRICMP(ext, ".sf3") == 0 || PV_BANK_STRICMP(ext, ".sfo") == 0)
        return TRUE;
#endif
    return FALSE;
}

static BAE_BOOL PV_ExtIsDLS(const char *ext)
{
    return (ext && PV_BANK_STRICMP(ext, ".dls") == 0) ? TRUE : FALSE;
}

static BAEBankKind PV_DetectBankKindFromMemory(const unsigned char *data, uint32_t size, const char *filenameHint)
{
    const char *ext = PV_PathExtension(filenameHint);

    if (data && size >= 12 &&
        data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F')
    {
        if (data[8] == 's' && data[9] == 'f' && data[10] == 'b' && data[11] == 'k')
            return BAE_BANK_KIND_SF2;
        if (data[8] == 'D' && data[9] == 'L' && data[10] == 'S' && data[11] == ' ')
            return BAE_BANK_KIND_DLS;
    }

    if (PV_ExtIsSF2(ext))
        return BAE_BANK_KIND_SF2;
    if (PV_ExtIsDLS(ext))
        return BAE_BANK_KIND_DLS;
    return BAE_BANK_KIND_HSB;
}

static void PV_MaybeLoadBuiltinBehindSF2(BAEMixer mixer)
{
#if _BUILT_IN_PATCHES == TRUE && _LOAD_BUILTIN_PATCHES_FOR_SF2 == TRUE
    BAEBankToken token = 0;
    if (BAEMixer_LoadBuiltinBank(mixer, &token) == BAE_NO_ERROR && token)
        BAEMixer_SendBankToBack(mixer, token);
#else
    (void)mixer;
#endif
}

static void PV_MaybeLoadBuiltinBehindDLS(BAEMixer mixer)
{
#if _BUILT_IN_PATCHES == TRUE && _LOAD_BUILTIN_PATCHES_FOR_DLS == TRUE
    BAEBankToken token = 0;
    if (BAEMixer_LoadBuiltinBank(mixer, &token) == BAE_NO_ERROR && token)
        BAEMixer_SendBankToBack(mixer, token);
#else
    (void)mixer;
#endif
}

BAEResult BAEMixer_UnloadAllInstrumentBanks(BAEMixer mixer)
{
    if (!mixer)
        return BAE_NULL_OBJECT;

    BAEMixer_UnloadBanks(mixer);
#if USE_NATIVE_DLS == TRUE
    GM_SetMixerDLSMode(FALSE);
    BAEMixer_UnloadXMFDLSOverlayBank(mixer);
    BAEMixer_UnloadDLSBank(mixer);
#endif
#if USE_SF2_SUPPORT == TRUE
    GM_UnloadSF2Soundfont();
    GM_SetMixerSF2Mode(FALSE);
#endif
    return BAE_NO_ERROR;
}

BAEResult BAEMixer_LoadBankFromPath(BAEMixer mixer, BAEPathName path, BAEBankLoadInfo *outInfo)
{
    const char *cpath = (const char *)path;
    const char *ext;

    PV_BankLoadInfoClear(outInfo);
    if (!mixer)
        return BAE_NULL_OBJECT;
    if (!cpath || !cpath[0])
        return BAE_PARAM_ERR;

    BAEMixer_UnloadAllInstrumentBanks(mixer);
    ext = PV_PathExtension(cpath);

#if USE_SF2_SUPPORT == TRUE
    if (PV_ExtIsSF2(ext))
    {
        OPErr oerr;
        PV_MaybeLoadBuiltinBehindSF2(mixer);
        oerr = GM_LoadSF2Soundfont(cpath);
        if (oerr != NO_ERR)
            return BAE_BAD_FILE;
        GM_SetMixerSF2Mode(TRUE);
        if (outInfo)
        {
            outInfo->token = 0;
            outInfo->kind = BAE_BANK_KIND_SF2;
        }
        return BAE_NO_ERROR;
    }
#endif

#if USE_NATIVE_DLS == TRUE
    if (PV_ExtIsDLS(ext))
    {
        BAEResult dls;
        PV_MaybeLoadBuiltinBehindDLS(mixer);
        dls = BAEMixer_LoadDLSBank(mixer, cpath);
        if (dls != BAE_NO_ERROR)
            return dls;
        GM_SetMixerDLSMode(TRUE);
        if (outInfo)
        {
            outInfo->token = 0;
            outInfo->kind = BAE_BANK_KIND_DLS;
        }
        return BAE_NO_ERROR;
    }
#endif

    {
        BAEBankToken tok = 0;
        BAEResult err = BAEMixer_AddBankFromFile(mixer, path, &tok);
        if (err != BAE_NO_ERROR)
            return err;
        if (outInfo)
        {
            outInfo->token = tok;
            outInfo->kind = BAE_BANK_KIND_HSB;
        }
        return BAE_NO_ERROR;
    }
}

BAEResult BAEMixer_LoadBankFromMemory(BAEMixer mixer,
                                      void const *data,
                                      uint32_t size,
                                      char const *filenameHint,
                                      BAEBankLoadInfo *outInfo)
{
    BAEBankKind kind;

    PV_BankLoadInfoClear(outInfo);
    if (!mixer)
        return BAE_NULL_OBJECT;
    if (!data || size == 0)
        return BAE_PARAM_ERR;

    BAEMixer_UnloadAllInstrumentBanks(mixer);
    kind = PV_DetectBankKindFromMemory((const unsigned char *)data, size, filenameHint);

#if USE_SF2_SUPPORT == TRUE
    if (kind == BAE_BANK_KIND_SF2)
    {
        OPErr oerr;
        PV_MaybeLoadBuiltinBehindSF2(mixer);
        oerr = GM_LoadSF2SoundfontFromMemory((const unsigned char *)data, (size_t)size);
        if (oerr != NO_ERR)
            return BAE_BAD_FILE;
        GM_SetMixerSF2Mode(TRUE);
        if (outInfo)
        {
            outInfo->token = 0;
            outInfo->kind = BAE_BANK_KIND_SF2;
        }
        return BAE_NO_ERROR;
    }
#endif

#if USE_NATIVE_DLS == TRUE
    if (kind == BAE_BANK_KIND_DLS)
    {
        BAEResult dls;
        PV_MaybeLoadBuiltinBehindDLS(mixer);
        dls = BAEMixer_LoadDLSBankFromMemory(mixer, (void *)data, size);
        if (dls != BAE_NO_ERROR)
            return dls;
        GM_SetMixerDLSMode(TRUE);
        if (outInfo)
        {
            outInfo->token = 0;
            outInfo->kind = BAE_BANK_KIND_DLS;
        }
        return BAE_NO_ERROR;
    }
#endif

    {
        BAEBankToken tok = 0;
        BAEResult err = BAEMixer_AddBankFromMemory(mixer, (void *)data, size, &tok);
        if (err != BAE_NO_ERROR)
            return err;
        if (outInfo)
        {
            outInfo->token = tok;
            outInfo->kind = BAE_BANK_KIND_HSB;
        }
        return BAE_NO_ERROR;
    }
}

#if _BUILT_IN_PATCHES == TRUE
BAEResult BAEMixer_LoadBankBuiltinOnly(BAEMixer mixer, BAEBankLoadInfo *outInfo)
{
    BAEBankToken tok = 0;
    BAEResult err;

    PV_BankLoadInfoClear(outInfo);
    if (!mixer)
        return BAE_NULL_OBJECT;

    BAEMixer_UnloadAllInstrumentBanks(mixer);
    err = BAEMixer_LoadBuiltinBank(mixer, &tok);
    if (err != BAE_NO_ERROR)
        return err;
    if (outInfo)
    {
        outInfo->token = tok;
        outInfo->kind = BAE_BANK_KIND_BUILTIN;
    }
    return BAE_NO_ERROR;
}
#endif

BAEResult BAESong_ApplyNormalizeFromMidiEstimate(BAESong song,
                                                 BAEMixer mixer,
                                                 BAE_BOOL enable,
                                                 int32_t targetPeakPct,
                                                 int32_t *outAppliedGainPct)
{
    int32_t gainPct = 100;
    BAEResult err;

    if (outAppliedGainPct)
        *outAppliedGainPct = 100;

    if (!mixer)
        return BAE_NULL_OBJECT;

    if (!enable)
    {
        BAEMixer_SetSongNormalizeGain(mixer, 100);
        return BAE_NO_ERROR;
    }

    if (!song)
        return BAE_NULL_OBJECT;

    if (targetPeakPct <= 0)
        targetPeakPct = BAE_NORMALIZE_DEFAULT_TARGET_PEAK_PCT;

    err = BAESong_NormalizeFromMidiEstimate(song, targetPeakPct, &gainPct);
    if (err != BAE_NO_ERROR)
    {
        BAEMixer_SetSongNormalizeGain(mixer, 100);
        if (outAppliedGainPct)
            *outAppliedGainPct = 100;
        return err;
    }

    /* Estimate already applied gain; re-assert for hosts that churn mixer state. */
    BAEMixer_SetSongNormalizeGain(mixer, gainPct);
    if (outAppliedGainPct)
        *outAppliedGainPct = gainPct;
    return BAE_NO_ERROR;
}

BAEResult BAEMixer_PrimeAudioOutputToFile(BAEMixer mixer, BAESong song)
{
    int i;
    BAE_BOOL preDone;
    int safety;

    if (!mixer)
        return BAE_NULL_OBJECT;

    for (i = 0; i < BAE_EXPORT_PRIME_SLICES; i++)
    {
        BAEResult serr = BAEMixer_ServiceAudioOutputToFile(mixer);
        if (serr != BAE_NO_ERROR)
            return serr;
    }

    if (!song)
        return BAE_NO_ERROR;

    preDone = TRUE;
    for (safety = 0; preDone && safety < BAE_EXPORT_PRIME_MAX_WAIT_SLICES; safety++)
    {
        if (BAESong_IsDone(song, &preDone) != BAE_NO_ERROR)
            break;
        if (!preDone)
            break;

        {
            BAEResult serr = BAEMixer_ServiceAudioOutputToFile(mixer);
            if (serr != BAE_NO_ERROR)
                return serr;
        }
        BAE_WaitMicroseconds(BAE_EXPORT_PRIME_WAIT_US);
    }
    return BAE_NO_ERROR;
}
