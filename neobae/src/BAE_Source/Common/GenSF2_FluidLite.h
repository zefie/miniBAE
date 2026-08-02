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

/****************************************************************************
 *
 * GenSF2_FluidLite.h
 *
 * FluidSynth integration for NeoBAE
 * Provides SF2 soundfont support through FluidSynth when USE_SF2_SUPPORT is enabled
 *
 ****************************************************************************/

#ifndef G_FLUIDSYNTH_H
#define G_FLUIDSYNTH_H

#include "X_API.h"
#include "GenSnd.h"
#include "NeoBAE.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
// Include FluidSynth headers
#include "fluidlite.h"

// FluidSynth integration types
typedef struct GM_SF2Info
{
    fluid_synth_t*      sf2_synth;         // FluidSynth synthesizer handle
    fluid_settings_t*   sf2_settings;     // FluidSynth settings handle
    int                 sf2_soundfont_id; // FluidSynth soundfont ID
    bool               sf2_active;        // TRUE if SF2 is handling this song
    char                sf2_path[256];     // path to loaded SF2 file
    float               sf2_master_volume; // master volume scaling
    int16_t             sf2_sample_rate;   // sample rate for SF2 rendering
    int16_t             sf2_max_voices;    // voice limit for SF2
    // Per-channel volume & expression (0..127); initialized to default GM values
    uint8_t             channelVolume[16];
    uint8_t             channelExpression[16];
    // Per-channel reverb & chorus send levels (0..127); CC 91 and CC 93
    uint8_t             channelReverb[16];
    uint8_t             channelChorus[16];
    // Channel mute states
    bool               channelMuted[16];
} GM_SF2Info;

// Initialize FluidSynth support for the mixer
OPErr GM_InitializeSF2(void);
void GM_CleanupSF2(void);

// FluidSynth mixer mode management
void GM_SetMixerSF2Mode(bool isSF2);
bool GM_GetMixerSF2Mode(void);

// Load SF2 soundfont for FluidSynth rendering
OPErr GM_LoadSF2SoundfontFromMemory(const unsigned char *data, size_t size);
OPErr GM_LoadSF2Soundfont(const char* sf2_path);
void GM_UnloadSF2Soundfont(void);

bool is_libinstpatch_loaded(void);

// Check if a song should use FluidSynth rendering
bool GM_IsSF2Song(GM_Song* pSong);

// Enable/disable FluidSynth rendering for a song
OPErr GM_EnableSF2ForSong(GM_Song* pSong, bool enable);

void GM_SF2_SetGain(float volume);
float GM_SF2_GetGain();

// FluidSynth MIDI event processing (called from existing MIDI processors)
void GM_SF2_ProcessNoteOn(GM_Song* pSong, int16_t channel, int16_t note, int16_t velocity);
void GM_SF2_ProcessNoteOff(GM_Song* pSong, int16_t channel, int16_t note, int16_t velocity);
void GM_SF2_ProcessProgramChange(GM_Song* pSong, int16_t channel, int32_t program);
void GM_SF2_ProcessController(GM_Song* pSong, int16_t channel, int16_t controller, int16_t value);
void GM_SF2_ProcessPitchBend(GM_Song* pSong, int16_t channel, int16_t bendMSB, int16_t bendLSB);
void GM_SF2_ProcessSysEx(GM_Song* pSong, const unsigned char* message, int32_t length);

// FluidSynth audio rendering (called during mixer slice processing)
void GM_SF2_RenderAudioSlice(GM_Song* pSong, int32_t* mixBuffer, int32_t* reverbBuffer, int32_t* chorusBuffer, int32_t frameCount);

// FluidSynth channel management (respects NeoBAE mute/solo states)
void GM_SF2_MuteChannel(GM_Song* pSong, int16_t channel);
void GM_SF2_UnmuteChannel(GM_Song* pSong, int16_t channel);
void GM_SF2_KillChannelNotes(int16_t channel);
void GM_SF2_AllNotesOff(GM_Song* pSong);
void GM_SF2_AllNotesOffChannel(GM_Song* pSong, int16_t channel);
void GM_SF2_SilenceSong(GM_Song* pSong);

// FluidSynth configuration
float GM_SF2_GetMasterVolume(void);
int16_t GM_SF2_GetMaxVoices(void);
void GM_SF2_SetStereoMode(bool stereo, bool applyNow);
void GM_SF2_SetSampleRate(int32_t sampleRate);
void GM_SF2_KillAllNotes(void);

// FluidSynth status queries
uint16_t GM_SF2_GetActiveVoiceCount(void);
bool GM_SF2_IsActive(void);

// FluidSynth reset
void GM_ResetSF2(void);
void GM_SoftResetSF2(void);
void GM_SF2_CheckAndDisableSF2ForRMFEmbedded(GM_Song* pSong);

// FluidSynth channel amplitude monitoring
void sf2_get_channel_amplitudes(float channelAmplitudes[16][2]);

// Private helper functions
void GM_SF2_SetDefaultControllers();
void PV_SF2_SetBankPreset(GM_Song* pSong, int16_t channel, int16_t bank, int16_t preset);

// Query whether the currently loaded soundfont exposes any presets at all.
// Optionally returns the counted number of presets.
bool GM_SF2_CurrentFontHasAnyPreset(int *outPresetCount);

/* Median effective sample loudness (linear) for the loaded SF2, including
 * GEN_ATTENUATION and the current synth.gain path factor. 0 if unavailable. */
float GM_SF2_MeasureBankLoudness(void);

/* Effective sample loudness for one preset note (bank/program/key/vel). */
float GM_SF2_EstimateNoteLoudness(int bank, int program, int key, int velocity);

void GM_SF2_SetChannelMode(int16_t channel, int16_t mode);
#endif // USE_SF2_SUPPORT

#ifdef __cplusplus
}
#endif

#endif // G_FLUIDSYNTH_H
