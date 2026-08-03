/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cross-engine bank loudness balance.
 * Scans instrument/sample RMS (with format attenuation) once after load,
 * then derives per-engine mix scales so SF2 / DLS / XMF match the HSB/RMF
 * reference level when two or more sources are present. Native HSB/RMF is
 * left at unity (scaling that path alters timbre); other engines are
 * boosted or attenuated toward it.
 */

#ifndef GEN_BANK_BALANCE_H
#define GEN_BANK_BALANCE_H

#include "X_API.h"
#include "GenSnd.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    GM_BANK_ENGINE_HSB = 0,     /* host HSB/ZSB bank and/or RMF embeds */
    GM_BANK_ENGINE_SF2 = 1,
    GM_BANK_ENGINE_DLS = 2,     /* DLS banks[0] — main bank */
    GM_BANK_ENGINE_DLS_XMF = 3, /* DLS banks[1] — XMF overlay */
    GM_BANK_ENGINE_COUNT = 4
} GM_BankEngine;

/* Notify when an HSB/ZSB patch bank is added or removed from the mixer. */
void GM_BankBalance_OnHsbBankAdded(XFILE file);
void GM_BankBalance_OnHsbBankRemoved(XFILE file);

/* Notify when an SF2 soundfont is loaded or unloaded. */
void GM_BankBalance_OnSf2Loaded(void);
void GM_BankBalance_OnSf2Unloaded(void);

/* Notify when DLS main/overlay banks change. Pass the mixer that owns them. */
void GM_BankBalance_OnDlsBanksChanged(struct GM_Mixer *pMixer);

/* Notify when an RMF/ZMF song's embedded instruments are loaded or unloaded.
 * Measured against loaded GM_Instrument waveforms (same native mix path as HSB). */
void GM_BankBalance_OnRmfInstrumentsLoaded(struct GM_Song *pSong);
void GM_BankBalance_OnRmfInstrumentsUnloaded(struct GM_Song *pSong);

/* Mix multiplier for an engine (1.0 when balance is inactive). */
float GM_BankBalance_GetMixScale(GM_BankEngine engine);

/* TRUE when two or more engines have measured loudness and scales are applied. */
bool GM_BankBalance_IsActive(void);

/* TRUE when this song should apply the HSB/RMF BankBalance mix scale on note-on
   and in the MIDI peak estimate. Pure RMF/ZMF with an unused external DLS/SF2
   bank loaded must stay at 1.0 so normalize matches unscaled playback. */
bool GM_BankBalance_SongAppliesHsbMixScale(struct GM_Song *pSong);

/* --- MIDI+patch song peak estimate (SCAN_ESTIMATE_PEAK) --- */

/* Reset concurrent-note energy tracker before a scan. */
void GM_EstimatePeak_Reset(void);

/* Note-on/off during SCAN_ESTIMATE_PEAK: update concurrent energy + max peak. */
void GM_EstimatePeak_NoteOn(GM_Song *pSong, int16_t channel, int16_t note, int16_t velocity);
void GM_EstimatePeak_NoteOff(GM_Song *pSong, int16_t channel, int16_t note);

/* Max concurrent peak so far, in linear full-scale units (1.0 ≈ 0 dBFS sample). */
float GM_EstimatePeak_GetMax(void);

/* Sample/zone loudness for one note (includes engine path + bank-balance scale). */
float GM_EstimateNoteLoudness(GM_Song *pSong, int16_t channel, int16_t note, int16_t velocity);

#ifdef __cplusplus
}
#endif

#endif /* GEN_BANK_BALANCE_H */
