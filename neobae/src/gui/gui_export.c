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

 // gui_export.c - WAV, MP3 and MIDI export functionality

/* Ensure POSIX APIs (popen/strcasecmp/realpath) are visible. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "gui_export.h"
#include "gui_dialogs.h"
#include "gui_frame.h"
#include "gui_transport.h"
#include "gui_widgets.h"
#include "gui_text.h"
#include "gui_theme.h"
#include "gui_panels.h"
#include "gui_midi.h"
#include "gui_settings.h"
#if SUPPORT_MIDI_HW == TRUE
#include "gui_midi_hw.h"
#endif
#include "gui_common.h"
#include "gui_theme.h"
#include "gui_midi.h"
#include "gui_bae.h"
#include "gui_script_editor.h"
#include "NeoBAE.h"
#include "GenPriv.h"
#include "BAE_API.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "X_Assert.h"
#include "gui_midi_hw.h"
#include "gui_settings.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <sys/stat.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#endif

// Forward declare the BAEGUI structure (defined in gui_bae.h, included above)
// External globals
extern BAEGUI g_bae;
extern void set_status_message(const char *msg);
// Export state globals
bool g_exporting = false;
int g_export_progress = 0;      // retained for potential legacy UI, not shown now
uint32_t g_export_last_pos = 0; // track advancement
int g_export_stall_iters = 0;   // stall detection
char g_export_path[1024] = {0}; // path of current export file

// Threading support for export
#ifdef _WIN32
static HANDLE g_export_thread = NULL;
static unsigned int g_export_thread_id = 0;
#else
static pthread_t g_export_thread = 0;
static bool g_export_thread_active = false;
#endif

// Export thread synchronization
static volatile bool g_export_thread_should_stop = false;
static volatile bool g_export_thread_finished = false;

// Forward declaration of export thread function
#ifdef _WIN32
static unsigned __stdcall export_thread_proc(void *param);
#else
static void *export_thread_proc(void *param);
#endif

// Export file tracking
int g_export_file_type = BAE_WAVE_TYPE; // active export type (WAV/FLAC/MP3/Vorbis/Opus)
uint32_t g_export_last_device_samples = 0;
int g_export_stable_loops = 0;
static const uint32_t EXPORT_MPEG_STABLE_THRESHOLD = 8; // matches playbae heuristic
bool g_export_realtime_mode = false;
bool g_export_using_live_song = false;
static unsigned int g_export_target_loops = 0;
static unsigned int g_export_loops_done = 0;
static uint32_t g_export_last_song_pos_us = 0;

// Export tick callback for script engine synchronization
static ExportTickFn g_export_tick_fn = NULL;
static void *g_export_tick_ud = NULL;

static void apply_current_eq_state(void)
{
    if (g_bae.mixer) {
        BAEMixer_SetEQEnabled(g_bae.mixer, g_eq_enabled ? TRUE : FALSE);
        if (g_eq_enabled) {
            for (int i = 0; i < 5; i++) {
                BAEMixer_SetEQGain(g_bae.mixer, i, g_eq_gains[i]);
            }
        }
    }
}

void bae_signal_export_stop(void)
{
    g_export_thread_should_stop = true;
}

void bae_set_export_tick_callback(ExportTickFn fn, void *userdata)
{
    g_export_tick_fn = fn;
    g_export_tick_ud = userdata;
}

// Export dropdown state: controls encoding choice when exporting
bool g_exportDropdownOpen = false;

// Default export codec: prefer 128kbps MP3 if MPEG encoder is available, otherwise FLAC if available
#if USE_MPEG_ENCODER != FALSE
int g_exportCodecIndex =
#if USE_FLAC_ENCODER != FALSE
    5; // 0 = PCM 16 WAV, 1 = FLAC, 2..8 = MP3 bitrates (5 -> 128kbps MP3)
#else
    4; // 0 = PCM 16 WAV, 1..7 = MP3 bitrates (4 -> 128kbps MP3)
#endif
#elif USE_FLAC_ENCODER != FALSE
int g_exportCodecIndex = 1; // 0 = PCM 16 WAV, 1 = FLAC
#else
int g_exportCodecIndex = 0; // fallback to WAV when no encoders present
#endif

const char *g_exportCodecNames[] = {
    "PCM 16 WAV",
#if USE_FLAC_ENCODER != FALSE
    "FLAC Lossless",
#endif
#if USE_MPEG_ENCODER != FALSE
    /* Removed: 64kbps, 96kbps, 160kbps MP3 per request; keep common MP3 options */
    "128kbps MP3",
    "192kbps MP3",
    "256kbps MP3",
    "320kbps MP3",
#endif
#if defined(USE_VORBIS_ENCODER) && USE_VORBIS_ENCODER == TRUE
    "96kbps Vorbis",
    "128kbps Vorbis",
    "256kbps Vorbis",
    "320kbps Vorbis",
#endif
#if defined(USE_OPUS_ENCODER) && USE_OPUS_ENCODER == TRUE
    "64kbps Opus",
    "96kbps Opus",
    "128kbps Opus",
    "256kbps Opus"
#endif
};

const int g_exportCodecCount = (int)(sizeof(g_exportCodecNames) / sizeof(g_exportCodecNames[0]));

// Direct mapping from dropdown index to BAE compression enum, half bitrate for per channel
const BAECompressionType g_exportCompressionMap[] = {
    BAE_COMPRESSION_NONE,
#if USE_FLAC_ENCODER != FALSE
    BAE_COMPRESSION_LOSSLESS,
#endif
#if defined(USE_MPEG_ENCODER) && USE_MPEG_ENCODER == TRUE
    /* Map retained MP3 bitrates */
    BAE_COMPRESSION_MPEG_128,
    BAE_COMPRESSION_MPEG_192,
    BAE_COMPRESSION_MPEG_256,
    BAE_COMPRESSION_MPEG_320,
#endif

#if defined(USE_VORBIS_ENCODER) && USE_VORBIS_ENCODER == TRUE
    /* Vorbis export mappings */
    BAE_COMPRESSION_VORBIS_96,
    BAE_COMPRESSION_VORBIS_128,
    BAE_COMPRESSION_VORBIS_256,
    BAE_COMPRESSION_VORBIS_320,
#endif
#if defined(USE_OPUS_ENCODER) && USE_OPUS_ENCODER == TRUE
    BAE_COMPRESSION_OPUS_64,
    BAE_COMPRESSION_OPUS_96,
    BAE_COMPRESSION_OPUS_128,
    BAE_COMPRESSION_OPUS_256,
#endif
    BAE_COMPRESSION_TYPE_COUNT
};

#if USE_MPEG_ENCODER == TRUE || USE_FLAC_ENCODER == TRUE || USE_VORBIS_ENCODER == TRUE || USE_OPUS_ENCODER == TRUE
const int g_exportCompressionCount = (int)(sizeof(g_exportCompressionMap) / sizeof(g_exportCompressionMap[0]));
#else
const int g_exportCompressionCount = 1; // only WAV
#endif

bool g_midiRecordFormatDropdownOpen = false;
#if SUPPORT_MIDI_HW == TRUE
// MIDI-record format dropdown (visible when MIDI-in is enabled)
int g_midiRecordFormatIndex = 0; // 0 = MIDI, 1 = WAV, 2..n = MP3 bitrates
const char *g_midiRecordFormatNames[] = {
    "MIDI Sequence",
    "PCM 16 WAV",
#if USE_FLAC_ENCODER != FALSE
    "FLAC Lossless",
#endif
#if USE_MPEG_ENCODER != FALSE
    "128kbps MP3",
    "192kbps MP3",
    "256kbps MP3",
    "320kbps MP3",
#endif
#if defined(USE_VORBIS_ENCODER) && USE_VORBIS_ENCODER == TRUE
    "96kbps Vorbis",
    "128kbps Vorbis",
    "256kbps Vorbis",
    "320kbps Vorbis",
#endif
#if defined(USE_OPUS_ENCODER) && USE_OPUS_ENCODER == TRUE
    "64kbps Opus",
    "96kbps Opus",
    "128kbps Opus",
    "256kbps Opus"
#endif
};
const int g_midiRecordFormatCount = sizeof(g_midiRecordFormatNames) / sizeof(g_midiRecordFormatNames[0]);

// Helper function to determine format type and bitrate for MIDI record format index
MidiRecordFormatInfo get_midi_record_format_info(int index) {
    MidiRecordFormatInfo info = {MIDI_RECORD_FORMAT_MIDI, 0, ".mid"};
    
    if (index == 0) {
        info.type = MIDI_RECORD_FORMAT_MIDI;
        info.extension = ".mid";
        return info;
    }
    
    if (index == 1) {
        info.type = MIDI_RECORD_FORMAT_WAV;
        info.extension = ".wav";
        return info;
    }
    
    int current_index = 2;
    
#if USE_FLAC_ENCODER != FALSE
    if (index == current_index) {
        info.type = MIDI_RECORD_FORMAT_FLAC;
        info.extension = ".flac";
        return info;
    }
    current_index++;
#endif

#if USE_MPEG_ENCODER != FALSE
    // MP3 formats: 128, 192, 256, 320
    if (index >= current_index && index < current_index + 4) {
        info.type = MIDI_RECORD_FORMAT_MP3;
        info.extension = ".mp3";
        int mp3_bitrates[] = {128, 192, 256, 320};
        info.bitrate = mp3_bitrates[index - current_index] * 1000; // convert to bps
        return info;
    }
    current_index += 4;
#endif

#if defined(USE_VORBIS_ENCODER) && USE_VORBIS_ENCODER == TRUE
    // Vorbis formats: 96, 128, 256, 320
    if (index >= current_index && index < current_index + 4) {
        info.type = MIDI_RECORD_FORMAT_VORBIS;
        info.extension = ".ogg";
        int vorbis_bitrates[] = {96, 128, 256, 320};
        info.bitrate = vorbis_bitrates[index - current_index] * 1000; // convert to bps
        return info;
    }
    current_index += 4;
#endif

#if defined(USE_OPUS_ENCODER) && USE_OPUS_ENCODER == TRUE
    // Opus formats: 64, 96, 128, 256
    if (index >= current_index && index < current_index + 4) {
        info.type = MIDI_RECORD_FORMAT_OPUS;
        info.extension = ".opus";
        int opus_bitrates[] = {64, 96, 128, 256};
        info.bitrate = opus_bitrates[index - current_index] * 1000; // convert to bps
        return info;
    }
#endif

    // Default fallback
    return info;
}

// External references to virtual keyboard and PCM recording
extern bool g_show_virtual_keyboard;
extern int g_keyboard_mouse_note;
extern int g_keyboard_channel;
extern BAESong g_live_song;
extern bool g_keyboard_active_notes_by_channel[16][128];
extern bool g_keyboard_active_notes[128];
extern bool g_pcm_wav_recording;
extern bool g_midi_input_enabled;
extern void pcm_wav_finalize(void);
#endif

#if SUPPORT_KARAOKE == TRUE
// External karaoke suspend function
extern void karaoke_suspend(bool suspend);
#endif

bool bae_start_export(const char *output_file, int export_type, int compression)
{
    int script_export_loopcount = -1;

    if (!g_bae.song_loaded || g_bae.is_audio_file)
    {
        set_status_message("Cannot export: No MIDI/RMF loaded");
        return false;
    }

    // Save current state so we can restore after export
    uint32_t curPosUs = 0;
    BAESong_GetMicrosecondPosition(g_bae.song, &curPosUs);
    g_bae.position_us_before_export = curPosUs;
    g_bae.was_playing_before_export = g_bae.is_playing;
    g_bae.loop_was_enabled_before_export = g_bae.loop_enabled_gui;

    // Stop current playback if running (we'll always restart for export)
    if (g_bae.is_playing)
    {
        BAESong_Stop(g_bae.song, FALSE);
        g_bae.is_playing = false;
    }

    // Rewind to beginning (export always starts from start)
#if SUPPORT_MIDI_HW == TRUE
    g_midi_output_suppressed_during_seek = true;
#endif    
    BAESong_SetMicrosecondPosition(g_bae.song, 0);
#if SUPPORT_MIDI_HW == TRUE    
    g_midi_output_suppressed_during_seek = false;
#endif    

    // CORRECTED ORDER: Start export FIRST, then start song
    // This is the correct order based on working MBAnsi test code
    BAEResult result = BAEMixer_StartOutputToFile(g_bae.mixer,
                                                  (BAEPathName)output_file,
                                                  (BAEFileType)export_type,
                                                  (BAECompressionType)compression);

    if (result != BAE_NO_ERROR)
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "Export failed to start (%d)", result);
        set_status_message(msg);
        return false;
    }

    // Re-apply EQ settings as StartOutputToFile re-initializes the mixer
    apply_current_eq_state();

    // Auto-start path: preroll then start
    BAESong_Stop(g_bae.song, FALSE);
#if SUPPORT_MIDI_HW == TRUE
    g_midi_output_suppressed_during_seek = true;
#endif    
    BAESong_SetMicrosecondPosition(g_bae.song, 0);
#if SUPPORT_MIDI_HW == TRUE
    g_midi_output_suppressed_during_seek = false;
#endif    
    BAESong_Preroll(g_bae.song);
    /* Match playback: reverb + optional normalize before the first export slice. */
    bae_set_reverb(g_bae.current_reverb_type);
    bae_apply_normalize_for_current_song();
    result = BAESong_Start(g_bae.song, 0);
    if (result != BAE_NO_ERROR)
    {
        BAE_PRINTF("Export: initial BAESong_Start failed (%d), retrying with re-preroll\n", result);
        BAESong_Stop(g_bae.song, FALSE);
#if SUPPORT_MIDI_HW == TRUE
        g_midi_output_suppressed_during_seek = true;
#endif
        BAESong_SetMicrosecondPosition(g_bae.song, 0);
#if SUPPORT_MIDI_HW == TRUE
        g_midi_output_suppressed_during_seek = false;
#endif
        BAESong_Preroll(g_bae.song);
        bae_set_reverb(g_bae.current_reverb_type);
        bae_apply_normalize_for_current_song();
        result = BAESong_Start(g_bae.song, 0);
        if (result != BAE_NO_ERROR)
        {
            char msg[128];
            snprintf(msg, sizeof(msg), "Song start failed during export (%d)", result);
            set_status_message(msg);
            BAEMixer_StopOutputToFile();
            return false;
        }
        else
        {
            // Ensure reverb is applied for export (matches MP3 export behavior)
            bae_set_reverb(g_bae.current_reverb_type);
            g_bae.is_playing = true;
        }
    }
    else
    {
        // Ensure reverb is applied for export (matches MP3 export behavior)
        bae_set_reverb(g_bae.current_reverb_type);
        g_bae.is_playing = true;
    }

    // Export defaults to one-shot; finite looping is handled externally.
    g_export_target_loops = 0;
    g_export_loops_done = 0;
    g_export_last_song_pos_us = 0;
    BAESong_SetLoops(g_bae.song, 0);

    // Tick the script engine before any audio is rendered so a script
    // seek (e.g. midi.position = X) takes effect before priming.
#if SUPPORT_BAESCRIPT == TRUE
    script_editor_reset_exporter_options();
#endif
    if (g_export_tick_fn)
        g_export_tick_fn(g_export_tick_ud);

#if SUPPORT_BAESCRIPT == TRUE
    if (script_editor_get_exporter_loopcount(&script_export_loopcount)) {
        if (script_export_loopcount > 0) {
            g_export_target_loops = (unsigned int)script_export_loopcount;
            BAESong_SetLoops(g_bae.song, 30000);
        }
    }
#endif

    // Apply channel mutes BEFORE any audio service calls so muted channels
    // are silenced from the very first sample of the export
    bool ch_enable[16];
    for (int i = 0; i < 16; i++) {
        ch_enable[i] = g_thread_ch_enabled[i] ? true : false;
    }
    bae_update_channel_mutes(ch_enable);

    // Force reverb to reinit by briefly switching to None and back.
    // This clears all internal reverb delay line buffers that might
    // contain residual tails from prior playback state.
    {
        int saved_reverb = g_bae.current_reverb_type;
        bae_set_reverb(BAE_REVERB_NONE);
        bae_set_reverb(saved_reverb);
    }

    // Give the song a moment to settle and process initial MIDI events
    // This helps prevent note dropping at the beginning of the export
    for (int settle = 0; settle < 10; settle++)
    {
        BAEMixer_ServiceAudioOutputToFile(g_bae.mixer);
        BAE_WaitMicroseconds(1000); // 1ms pause between each service call
    }

    {
        BAEResult perr = BAEMixer_PrimeAudioOutputToFile(g_bae.mixer, g_bae.song);
        if (perr != BAE_NO_ERROR)
        {
            BAE_PRINTF("Export priming failed (BAE Error #%d). Aborting.\n", perr);
            BAEMixer_StopOutputToFile();
            return false;
        }
    }

    g_exporting = true;
    // Record current export file type for MPEG-specific heuristics
    g_export_file_type = BAE_WAVE_TYPE;
    // When called via interactive Record->WAV, prefer realtime pacing
    g_export_realtime_mode = true;
    // Note: this function previously only supported WAV via StartOutputToFile call above.
    // If StartOutputToFile was called with MPEG elsewhere, g_export_file_type will be set there.
#if SUPPORT_MIDI_HW == TRUE
    // Ensure virtual keyboard is reset and any held note is released when export starts
    if (g_show_virtual_keyboard)
    {
        if (g_keyboard_mouse_note != -1)
        {
            BAESong target = g_bae.song ? g_bae.song : g_live_song;
            if (target)
                BAESong_NoteOff(target, (unsigned char)g_keyboard_channel, (unsigned char)g_keyboard_mouse_note, 0, 0);
            g_keyboard_mouse_note = -1;
        }
        // Clear per-channel incoming note flags and UI array
        memset(g_keyboard_active_notes_by_channel, 0, sizeof(g_keyboard_active_notes_by_channel));
        memset(g_keyboard_active_notes, 0, sizeof(g_keyboard_active_notes));
    }
#endif
#if SUPPORT_KARAOKE == TRUE
    karaoke_suspend(true); // disable karaoke during export
#endif
    g_export_progress = 0; // reset (unused for display)
    g_export_last_pos = 0;
    g_export_stall_iters = 0;
    safe_strncpy(g_export_path, output_file ? output_file : "", sizeof(g_export_path) - 1);
    g_export_path[sizeof(g_export_path) - 1] = '\0';

    // Initialize thread synchronization variables
    g_export_thread_should_stop = false;
    g_export_thread_finished = false;

    // Create export thread
#ifdef _WIN32
    g_export_thread = (HANDLE)_beginthreadex(NULL, 0, export_thread_proc, NULL, 0, &g_export_thread_id);
    if (g_export_thread == NULL)
    {
        BAE_PRINTF("Failed to create export thread\n");
        set_status_message("Failed to create export thread");
        BAEMixer_StopOutputToFile();
        g_exporting = false;
        return false;
    }
#else
    g_export_thread_active = true;
    if (pthread_create(&g_export_thread, NULL, export_thread_proc, NULL) != 0)
    {
        BAE_PRINTF("Failed to create export thread\n");
        set_status_message("Failed to create export thread");
        BAEMixer_StopOutputToFile();
        g_exporting = false;
        g_export_thread_active = false;
        return false;
    }
#endif

    set_status_message("WAV export started");
    return true;
}

#if USE_MPEG_ENCODER != FALSE
bool bae_start_mpeg_export(const char *output_file, int codec_index)
{
    int script_export_loopcount = -1;

    if (!g_bae.song_loaded || g_bae.is_audio_file)
    {
        set_status_message("Cannot export: No MIDI/RMF loaded");
        return false;
    }

    if (codec_index < 1 || codec_index >= g_exportCompressionCount)
    {
        set_status_message("Invalid codec index");
        return false;
    }

    // Save current state so we can restore after export
    uint32_t curPosUs = 0;
    BAESong_GetMicrosecondPosition(g_bae.song, &curPosUs);
    g_bae.position_us_before_export = curPosUs;
    g_bae.was_playing_before_export = g_bae.is_playing;
    g_bae.loop_was_enabled_before_export = g_bae.loop_enabled_gui;

    // Stop current playback if running
    if (g_bae.is_playing)
    {
        BAESong_Stop(g_bae.song, FALSE);
        g_bae.is_playing = false;
    }

    // Rewind to beginning
#if SUPPORT_MIDI_HW == TRUE
    g_midi_output_suppressed_during_seek = true;
#endif
    BAESong_SetMicrosecondPosition(g_bae.song, 0);
#if SUPPORT_MIDI_HW == TRUE
    g_midi_output_suppressed_during_seek = false;
#endif

    // Determine output type from selected compression
    BAEFileType outType = BAE_WAVE_TYPE;
    BAECompressionType comp = g_exportCompressionMap[codec_index];

#if USE_FLAC_ENCODER == TRUE
    if (comp == BAE_COMPRESSION_LOSSLESS)
    {
        outType = BAE_FLAC_TYPE;
    }
#endif

#if USE_MPEG_ENCODER == TRUE
    if (comp >= BAE_COMPRESSION_MPEG_32 && comp <= BAE_COMPRESSION_MPEG_320)
    {
        outType = BAE_MPEG_TYPE;
    }
#endif
    
#if USE_VORBIS_ENCODER == TRUE
    if (comp == BAE_COMPRESSION_VORBIS_96 || comp == BAE_COMPRESSION_VORBIS_128 || comp == BAE_COMPRESSION_VORBIS_256 || comp == BAE_COMPRESSION_VORBIS_320)
    {
        outType = BAE_VORBIS_TYPE;
    }
#endif

#if USE_OPUS_ENCODER == TRUE
    if (comp >= BAE_COMPRESSION_OPUS_16 && comp <= BAE_COMPRESSION_OPUS_256)
    {
        outType = BAE_OPUS_TYPE;
    }
#endif

    // Start export
    BAEResult result = BAEMixer_StartOutputToFile(g_bae.mixer,
                                                  (BAEPathName)output_file,
                                                  outType,
                                                  comp);

    if (result != BAE_NO_ERROR)
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "Export failed to start (%d)", result);
        set_status_message(msg);
        return false;
    }

    // Re-apply EQ settings as StartOutputToFile re-initializes the mixer
    apply_current_eq_state();

    // Start the song to drive export
    BAESong_Stop(g_bae.song, FALSE);
#if SUPPORT_MIDI_HW == TRUE
    g_midi_output_suppressed_during_seek = true;
#endif
    BAESong_SetMicrosecondPosition(g_bae.song, 0);
#if SUPPORT_MIDI_HW == TRUE
    g_midi_output_suppressed_during_seek = false;
#endif
    BAESong_Preroll(g_bae.song);
    bae_set_reverb(g_bae.current_reverb_type); // ensure reverb is set for export / normalize padding
    bae_apply_normalize_for_current_song();
    result = BAESong_Start(g_bae.song, 0);
    if (result != BAE_NO_ERROR)
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "Song start failed during export (%d)", result);
        set_status_message(msg);
        BAEMixer_StopOutputToFile();
        return false;
    }
    bae_set_reverb(g_bae.current_reverb_type); // song start can overwrite embedded reverb default

    g_bae.is_playing = true;

    // Export defaults to one-shot; finite looping is handled externally.
    g_export_target_loops = 0;
    g_export_loops_done = 0;
    g_export_last_song_pos_us = 0;
    BAESong_SetLoops(g_bae.song, 0);

    // Tick the script engine before any audio is rendered so a script
    // seek (e.g. midi.position = X) takes effect before priming.
#if SUPPORT_BAESCRIPT == TRUE
    script_editor_reset_exporter_options();
#endif
    if (g_export_tick_fn)
        g_export_tick_fn(g_export_tick_ud);

#if SUPPORT_BAESCRIPT == TRUE
    if (script_editor_get_exporter_loopcount(&script_export_loopcount)) {
        if (script_export_loopcount > 0) {
            g_export_target_loops = (unsigned int)script_export_loopcount;
            BAESong_SetLoops(g_bae.song, 30000);
        }
    }
#endif

    // Apply channel mutes BEFORE any audio service calls so muted channels
    // are silenced from the very first sample of the export
    bool ch_enable[16];
    for (int i = 0; i < 16; i++) {
        ch_enable[i] = g_thread_ch_enabled[i] ? true : false;
    }
    bae_update_channel_mutes(ch_enable);

    // Force reverb to reinit by briefly switching to None and back.
    // This clears all internal reverb delay line buffers that might
    // contain residual tails from prior playback state.
    {
        int saved_reverb = g_bae.current_reverb_type;
        bae_set_reverb(BAE_REVERB_NONE);
        bae_set_reverb(saved_reverb);
    }

    {
        BAEResult perr = BAEMixer_PrimeAudioOutputToFile(g_bae.mixer, g_bae.song);
        if (perr != BAE_NO_ERROR)
        {
            BAE_PRINTF("MP3 export priming failed (BAE Error #%d). Aborting.\n", perr);
            BAEMixer_StopOutputToFile();
            return false;
        }
    }

    g_exporting = true;
    g_export_file_type = outType;
    g_export_realtime_mode = false; // MPEG export typically runs at full speed

#if SUPPORT_MIDI_HW == TRUE
    // Reset virtual keyboard
    if (g_show_virtual_keyboard)
    {
        if (g_keyboard_mouse_note != -1)
        {
            BAESong target = g_bae.song ? g_bae.song : g_live_song;
            if (target)
                BAESong_NoteOff(target, (unsigned char)g_keyboard_channel, (unsigned char)g_keyboard_mouse_note, 0, 0);
            g_keyboard_mouse_note = -1;
        }
        memset(g_keyboard_active_notes_by_channel, 0, sizeof(g_keyboard_active_notes_by_channel));
        memset(g_keyboard_active_notes, 0, sizeof(g_keyboard_active_notes));
    }
#endif

#if SUPPORT_KARAOKE == TRUE
    karaoke_suspend(true); // disable karaoke during export
#endif

    g_export_progress = 0;
    g_export_last_pos = 0;
    g_export_stall_iters = 0;
    safe_strncpy(g_export_path, output_file ? output_file : "", sizeof(g_export_path) - 1);
    g_export_path[sizeof(g_export_path) - 1] = '\0';

    // Initialize thread synchronization variables
    g_export_thread_should_stop = false;
    g_export_thread_finished = false;

    // Create export thread
#ifdef _WIN32
    g_export_thread = (HANDLE)_beginthreadex(NULL, 0, export_thread_proc, NULL, 0, &g_export_thread_id);
    if (g_export_thread == NULL)
    {
        BAE_PRINTF("Failed to create export thread\n");
        set_status_message("Failed to create export thread");
        BAEMixer_StopOutputToFile();
        g_exporting = false;
        return false;
    }
#else
    g_export_thread_active = true;
    if (pthread_create(&g_export_thread, NULL, export_thread_proc, NULL) != 0)
    {
        BAE_PRINTF("Failed to create export thread\n");
        set_status_message("Failed to create export thread");
        BAEMixer_StopOutputToFile();
        g_exporting = false;
        g_export_thread_active = false;
        return false;
    }
#endif

    set_status_message("Export started");
    return true;
}
#endif // USE_MPEG_ENCODER

void bae_stop_wav_export()
{
    if (g_exporting)
    {
        // Signal the export thread to stop
        g_export_thread_should_stop = true;
        
        // Wait for the export thread to finish
#ifdef _WIN32
        if (g_export_thread != NULL)
        {
            WaitForSingleObject(g_export_thread, 5000); // Wait up to 5 seconds
            CloseHandle(g_export_thread);
            g_export_thread = NULL;
            g_export_thread_id = 0;
        }
#else
        if (g_export_thread_active)
        {
            // Simple join - if the thread is stuck, the user can always force-quit the app
            pthread_join(g_export_thread, NULL);
            g_export_thread_active = false;
            g_export_thread = 0;
        }
#endif

        BAEMixer_StopOutputToFile();

        // Stop the song first
        if (g_bae.song)
        {
            BAESong_Stop(g_bae.song, FALSE);
        }

        // Restore looping state
        if (g_bae.song && g_bae.loop_was_enabled_before_export)
        {
            BAESong_SetLoops(g_bae.song, 32767);
        }
        g_bae.loop_was_enabled_before_export = false;

        // Restore original position
        if (g_bae.song)
        {
#if SUPPORT_MIDI_HW == TRUE
            g_midi_output_suppressed_during_seek = true;
#endif            
            BAESong_SetMicrosecondPosition(g_bae.song, g_bae.position_us_before_export);
#if SUPPORT_MIDI_HW == TRUE
            g_midi_output_suppressed_during_seek = false;
#endif            
        }

        // Re-engage hardware audio if we had it before
        // The StartOutputToFile disengages hardware, so we need to re-engage it
        if (g_bae.mixer)
        {
            // Try to re-acquire audio hardware
            BAEResult reacquire_result = BAEMixer_ReengageAudio(g_bae.mixer);
            if (reacquire_result != BAE_NO_ERROR)
            {
                BAE_PRINTF("Warning: Could not re-engage audio hardware after export (%d)\n", reacquire_result);
            }
            else
            {
                // Re-apply EQ settings as hardware re-engagement may have reset the mixer state
                apply_current_eq_state();
            }
        }

        // Restore playback state
        if (g_bae.was_playing_before_export && g_bae.song)
        {
            // Restart song from restored position
            BAESong_Preroll(g_bae.song);
            bae_set_reverb(g_bae.current_reverb_type);
            bae_apply_normalize_for_current_song();
#if SUPPORT_MIDI_HW == TRUE
            g_midi_output_suppressed_during_seek = true;
#endif
            BAESong_SetMicrosecondPosition(g_bae.song, g_bae.position_us_before_export);
#if SUPPORT_MIDI_HW == TRUE
            g_midi_output_suppressed_during_seek = false;
#endif
            if (BAESong_Start(g_bae.song, 0) == BAE_NO_ERROR)
            {
                g_bae.is_playing = true;
            }
            else
            {
                g_bae.is_playing = false;
            }
        }
        else
        {
            g_bae.is_playing = false;
        }
        // Mark UI needs sync (local 'playing' variable)
        // We'll sync just after frame logic by checking mismatch

        g_exporting = false;
        g_export_thread_should_stop = false;
        g_export_thread_finished = false;
        
#if SUPPORT_KARAOKE == TRUE
        karaoke_suspend(false); // re-enable karaoke after export
#endif
        g_export_realtime_mode = false;
        g_export_progress = 0;
        g_export_path[0] = '\0';
        
        const char *msg = g_export_thread_should_stop ? "Export cancelled" : "Export completed";
        set_status_message(msg);
    }
}

void bae_service_wav_export()
{
#if SUPPORT_MIDI_HW == TRUE
    // If using our PCM WAV recorder, service that path separately
    if (g_pcm_wav_recording)
    {
        // Platform recorder writes slices from the audio callback directly.
        // Here we only need to monitor for song completion (when recording a file-backed song)
        // and finalize the recording. Do not pull frames from the mixer to avoid double-consumption.
        if (!g_midi_input_enabled && g_bae.song)
        {
            BAE_BOOL done = FALSE;
            BAESong_IsDone(g_bae.song, &done);
            if (done)
            {
                // Before completing, give the mixer a chance to drain any reverb/FX tail
                // by servicing the output and checking for any non-zero samples in the
                // legacy reverb buffer, new reverb buffers, or Neo reverb state. This helps
                // catch reverb trails that would otherwise be truncated if we stop immediately.

                if (!g_export_thread_should_stop)
                {
                    BAE_PRINTF("Export: checking for reverb tail (will wait up to ~600ms)...\n");
                    int silentConsec = 0;
                    const int needSilentConsec = 4; // require this many consecutive silent checks
                    const int maxLoops = 300;       // safety limit
                    int loopCount = 0;
                    for (loopCount = 0; loopCount < maxLoops && !g_export_thread_should_stop; ++loopCount)
                    {
                        BAEMixer_ServiceAudioOutputToFile(g_bae.mixer);

                        bool foundNonZero = BAEMixer_IsAudioTailActive(g_bae.mixer);
                        if (foundNonZero)
                        {
                            silentConsec = 0; // reverb still present; continue draining
                        }
                        else
                        {
                            silentConsec++;
                            if (silentConsec >= needSilentConsec)
                            {
                                BAE_PRINTF("Export: reverb tail cleared after %d iterations\n", loopCount + 1);
                                break;
                            }
                        }

                        // small pause to allow effects to decay
                        BAE_WaitMicroseconds(2000);
                    }

                    if (loopCount >= maxLoops)
                    {
                        BAE_PRINTF("Export: reverb drain timed out after %d iterations\n", loopCount);
                    }
                    
                    pcm_wav_finalize();
                    if (g_bae.loop_was_enabled_before_export && g_bae.song)
                    {
                        BAESong_SetLoops(g_bae.song, 32767);
                    }
                    g_bae.loop_was_enabled_before_export = false;
                    if (g_bae.song)
                    {
                        g_midi_output_suppressed_during_seek = true;
                        BAESong_SetMicrosecondPosition(g_bae.song, g_bae.position_us_before_export);
                        g_midi_output_suppressed_during_seek = false;
                    }
                    g_bae.is_playing = g_bae.was_playing_before_export;
                    gui_panic_all_notes(g_bae.song);
                }
            }
        }
        return;
    }
#endif
    
    // If we're using threaded export, just check if the thread is done
    if (g_exporting && g_export_thread_finished)
    {
        bae_stop_wav_export();
        return;
    }
    
    // Non-threaded export is no longer used - all export is now threaded
    return;
}

// Export thread function - this runs the actual export loop
#ifdef _WIN32
static unsigned __stdcall export_thread_proc(void *param)
#else
static void *export_thread_proc(void *param)
#endif
{
    BAE_PRINTF("Export thread started\n");
    
    // Apply channel mutes at the start of export to ensure they're respected
    bool ch_enable[16];
    for (int i = 0; i < 16; i++) {
        ch_enable[i] = g_thread_ch_enabled[i] ? true : false;
    }
    bae_update_channel_mutes(ch_enable);

    // Tick the script engine once before rendering any audio so it can
    // seek/configure before the first buffer is written.
    if (g_export_tick_fn)
        g_export_tick_fn(g_export_tick_ud);

    while (!g_export_thread_should_stop && g_exporting)
    {
        // First service call (matching main loop service in playbae)
        BAEResult r = BAEMixer_ServiceAudioOutputToFile(g_bae.mixer);
        if (r != BAE_NO_ERROR)
        {
            BAE_PRINTF("ServiceAudioOutputToFile error: %d\n", r);
            g_export_thread_should_stop = true;
            break;
        }

        BAE_BOOL is_done = FALSE;
        uint32_t current_pos = 0;
        BAESong_GetMicrosecondPosition(g_bae.song, &current_pos);
        BAESong_IsDone(g_bae.song, &is_done);

        if (g_export_target_loops > 0 && g_export_last_song_pos_us > 0)
        {
            if (current_pos < g_export_last_song_pos_us &&
                (g_export_last_song_pos_us - current_pos) > 1000000U)
            {
                g_export_loops_done++;
                BAE_PRINTF("Export loop wrap detected (%u/%u)\n",
                    g_export_loops_done, g_export_target_loops);
                if (g_export_loops_done >= g_export_target_loops)
                {
                    BAESong_Stop(g_bae.song, FALSE);
                }
            }
        }
        g_export_last_song_pos_us = current_pos;

        // Tick the script engine so it can react to position changes
        // during non-realtime export (otherwise it only ticks per GUI frame)
        if (g_export_tick_fn)
            g_export_tick_fn(g_export_tick_ud);

        if (!is_done)
        {
            // Second service call (matching PV_Idle service in playbae)
            r = BAEMixer_ServiceAudioOutputToFile(g_bae.mixer);
            if (r != BAE_NO_ERROR)
            {
                BAE_PRINTF("ServiceAudioOutputToFile error: %d\n", r);
                g_export_thread_should_stop = true;
                break;
            }
        }

        if (is_done)
        {
            BAE_PRINTF("Song finished at position %u\n", current_pos);


            // If exporting MPEG, wait for device-samples to stabilize before stopping (encoder drain)
            if (g_exporting && g_export_file_type == BAE_MPEG_TYPE && !g_export_thread_should_stop)
            {
                uint32_t lastSamples = 0;
                int stableLoops = 0;
                while (stableLoops < (int)EXPORT_MPEG_STABLE_THRESHOLD && !g_export_thread_should_stop)
                {
                    BAEMixer_ServiceAudioOutputToFile(g_bae.mixer);
                    BAE_WaitMicroseconds(11000);
                    uint32_t curSamples = BAE_GetDeviceSamplesPlayedPosition();
                    if (curSamples == lastSamples)
                    {
                        stableLoops++;
                    }
                    else
                    {
                        stableLoops = 0;
                        lastSamples = curSamples;
                    }
                }
            }

            if (!g_export_thread_should_stop)
            {
                BAE_PRINTF("Export: checking for reverb tail (will wait up to ~600ms)...\n");
                int silentConsec = 0;
                const int needSilentConsec = 4; // require this many consecutive silent checks
                const int maxLoops = 300;       // safety limit
                int loopCount = 0;
                for (loopCount = 0; loopCount < maxLoops && !g_export_thread_should_stop; ++loopCount)
                {
                    BAEMixer_ServiceAudioOutputToFile(g_bae.mixer);

                    bool foundNonZero = BAEMixer_IsAudioTailActive(g_bae.mixer);
                    if (foundNonZero)
                    {
                        silentConsec = 0; // reverb still present; continue draining
                    }
                    else
                    {
                        silentConsec++;
                        if (silentConsec >= needSilentConsec)
                        {
                            BAE_PRINTF("Export: reverb tail cleared after %d iterations\n", loopCount + 1);
                            break;
                        }
                    }

                    // small pause to allow effects to decay
                    BAE_WaitMicroseconds(2000);
                }

                if (loopCount >= maxLoops)
                {
                    BAE_PRINTF("Export: reverb drain timed out after %d iterations\n", loopCount);
                }
                gui_panic_all_notes(g_bae.song);
            }

            // Export completed successfully
            g_export_thread_finished = true;
            BAE_PRINTF("Export thread completed normally\n");
#ifdef _WIN32
            return 0;
#else
            return NULL;
#endif
        }
    }

    // If we get here, export was cancelled or failed
    BAE_PRINTF("Export thread stopped (cancelled or failed)\n");
    g_export_thread_finished = true;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

// File dialog for save export
char *save_export_dialog(int export_type) // 0=WAV, 1=FLAC, 2=MP3, 3=OGG, 4=OPUS
{
#ifdef _WIN32
    char fileBuf[1024] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    if (export_type == 1)
    {
        ofn.lpstrFilter = "FLAC Files\0*.flac\0All Files\0*.*\0";
        ofn.lpstrDefExt = "flac";
    }
    else if (export_type == 2)
    {
        ofn.lpstrFilter = "MP3 Files\0*.mp3\0All Files\0*.*\0";
        ofn.lpstrDefExt = "mp3";
    }
    else if (export_type == 3)
    {
        ofn.lpstrFilter = "OGG Files\0*.ogg\0All Files\0*.*\0";
        ofn.lpstrDefExt = "ogg";
    } 
    else if (export_type == 4)
    {
        ofn.lpstrFilter = "OPUS Files\0*.opus\0All Files\0*.*\0";
        ofn.lpstrDefExt = "opus";
    }        
    else
    {
        ofn.lpstrFilter = "WAV Files\0*.wav\0All Files\0*.*\0";
        ofn.lpstrDefExt = "wav";
    }
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameA(&ofn))
    {
        size_t len = strlen(fileBuf);
        char *ret = (char *)malloc(len + 1);
        if (ret)
        {
            memcpy(ret, fileBuf, len + 1);
        }
        return ret;
    }
    return NULL;
#elif defined(__APPLE__)
    const char *ext;
    const char *title;
    if (export_type == 1)      { ext = "flac"; title = "Save FLAC Export"; }
    else if (export_type == 2) { ext = "mp3";  title = "Save MP3 Export"; }
    else if (export_type == 3) { ext = "ogg";  title = "Save OGG Export"; }
    else if (export_type == 4) { ext = "opus"; title = "Save Opus Export"; }
    else                       { ext = "wav";  title = "Save WAV Export"; }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'POSIX path of (choose file name with prompt \"%s\" default name \"output.%s\")' 2>/dev/null",
        title, ext);
    FILE *fp = popen(cmd, "r");
    if (fp)
    {
        char buf[1024];
        if (fgets(buf, sizeof(buf), fp))
        {
            pclose(fp);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                    memcpy(ret, buf, l + 1);
                return ret;
            }
        }
        else
            pclose(fp);
    }
    return NULL;
#else
    const char *cmds_wav[] = {
        "zenity --file-selection --save --title='Save WAV Export' --file-filter='WAV Files | *.wav' 2>/dev/null",
        "kdialog --getsavefilename . '*.wav' 2>/dev/null",
        "yad --file-selection --save --title='Save WAV Export' 2>/dev/null",
        NULL};
    const char *cmds_flac[] = {
        "zenity --file-selection --save --title='Save FLAC Export' --file-filter='FLAC Files | *.flac' 2>/dev/null",
        "kdialog --getsavefilename . '*.flac' 2>/dev/null",
        "yad --file-selection --save --title='Save FLAC Export' 2>/dev/null",
        NULL};
    const char *cmds_mp3[] = {
        "zenity --file-selection --save --title='Save MP3 Export' --file-filter='MP3 Files | *.mp3' 2>/dev/null",
        "kdialog --getsavefilename . '*.mp3' 2>/dev/null",
        "yad --file-selection --save --title='Save MP3 Export' 2>/dev/null",
        NULL};
    const char *cmds_ogg[] = {
        "zenity --file-selection --save --title='Save OGG Export' --file-filter='OGG Files | *.ogg' 2>/dev/null",
        "kdialog --getsavefilename . '*.ogg' 2>/dev/null",
        "yad --file-selection --save --title='Save OGG Export' 2>/dev/null",
        NULL};
    const char *cmds_opus[] = {
        "zenity --file-selection --save --title='Save Opus Export' --file-filter='Opus Files | *.opus' 2>/dev/null",
        "kdialog --getsavefilename . '*.opus' 2>/dev/null",
        "yad --file-selection --save --title='Save Opus Export' 2>/dev/null",
        NULL};
    const char **use_cmds;
    if (export_type == 1)
    {
        use_cmds = cmds_flac;
    }
    else if (export_type == 2)
    {
        use_cmds = cmds_mp3;
    }
    else if (export_type == 3)
    {
        use_cmds = cmds_ogg;
    }
    else if (export_type == 4)
    {
        use_cmds = cmds_opus;
    }
    else
    {
        use_cmds = cmds_wav;
    }
    for (int i = 0; use_cmds[i]; ++i)
    {
        FILE *p = popen(use_cmds[i], "r");
        if (!p)
            continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p))
        {
            pclose(p);
            // strip newline
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                {
                    memcpy(ret, buf, l + 1);
                }
                return ret;
            }
        }
        else
        {
            pclose(p);
        }
    }
    return NULL;
#endif
}

// Initialize export subsystem
void export_init(void)
{
    g_exporting = false;
    g_export_progress = 0;
    g_export_last_pos = 0;
    g_export_stall_iters = 0;
    g_export_path[0] = '\0';
    g_export_file_type = BAE_WAVE_TYPE;
    g_export_last_device_samples = 0;
    g_export_stable_loops = 0;
    g_export_realtime_mode = false;
    g_export_using_live_song = false;
    g_exportDropdownOpen = false;
    
    // Initialize threading variables
#ifdef _WIN32
    g_export_thread = NULL;
    g_export_thread_id = 0;
#else
    g_export_thread = 0;
    g_export_thread_active = false;
#endif
    g_export_thread_should_stop = false;
    g_export_thread_finished = false;
}

// Cleanup export subsystem
void export_cleanup(void)
{
    if (g_exporting)
    {
        bae_stop_wav_export();
    }
}

// Save dialog specifically for MIDI (.mid)
char *save_midi_dialog()
{
#ifdef _WIN32
    char fileBuf[1024] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "MIDI Files\0*.mid\0All Files\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "mid";
    if (GetSaveFileNameA(&ofn))
    {
        size_t len = strlen(fileBuf);
        char *ret = (char *)malloc(len + 1);
        if (ret)
        {
            memcpy(ret, fileBuf, len + 1);
        }
        return ret;
    }
    return NULL;
#else
    const char *cmds[] = {
        "zenity --file-selection --save --title='Save MIDI' --file-filter='MIDI Files | *.mid' 2>/dev/null",
        "kdialog --getsavefilename . '*.mid' 2>/dev/null",
        "yad --file-selection --save --title='Save MIDI' 2>/dev/null",
        NULL};
    for (int i = 0; cmds[i]; ++i)
    {
        FILE *p = popen(cmds[i], "r");
        if (!p)
            continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p))
        {
            pclose(p);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                {
                    memcpy(ret, buf, l + 1);
                }
                return ret;
            }
        }
        else
        {
            pclose(p);
        }
    }
    BAE_PRINTF("No GUI file chooser available for saving MIDI.\n");
    return NULL;
#endif
}

void gui_export_draw_controls(GuiFrameCtx *ctx)
{
    SDL_Renderer *R = ctx->R;
    int mx = ctx->mx;
    int my = ctx->my;
    bool mdown = ctx->mdown;
    bool mclick = ctx->mclick ? *ctx->mclick : false;
    int ui_mx = ctx->ui_mx;
    int ui_my = ctx->ui_my;
    bool ui_mdown = ctx->ui_mdown;
    bool ui_mclick = ctx->ui_mclick;
    bool ui_rclick = ctx->ui_rclick;
    bool modal_block = ctx->modal_block;
    bool playing = *ctx->playing;
    int progress = *ctx->progress;
    int duration = *ctx->duration;
    int transpose = *ctx->transpose;
    int tempo = *ctx->tempo;
    int volume = *ctx->volume;
    int reverbType = *ctx->reverb_type;
    bool loopPlay = *ctx->loop_enabled;
    bool *ch_enable = ctx->ch_enable;
    SDL_Color panelBg = ctx->panelBg;
    SDL_Color panelBorder = ctx->panelBorder;
    Rect transportPanel = ctx->transportPanel;
    (void)ui_mdown;
    (void)ui_rclick;
    (void)transpose;
    (void)tempo;
    (void)volume;
    (void)reverbType;
    (void)ch_enable;
    (void)panelBg;
    (void)panelBorder;

// Export controls (only for MIDI/RMF files) or MIDI-in recording when enabled
#if SUPPORT_MIDI_HW == TRUE
if (g_midi_input_enabled || (!g_bae.is_audio_file && g_bae.song_loaded))
#else
if (!g_bae.is_audio_file && g_bae.song_loaded)
#endif
{
    // If MIDI input is enabled, show Record/Stop instead of Export
#if SUPPORT_MIDI_HW == TRUE
    if (g_midi_input_enabled)
    {
        // When MIDI-in is enabled the overlay provides recording controls. Draw a disabled placeholder here.
        SDL_Color disabledBg = g_panel_bg;
        SDL_Color disabledTxt = g_panel_border;
        Rect placeholder = {348, 215, 80, 22}; // Moved from 320 to 348
        draw_rect(R, placeholder, disabledBg);
        draw_frame(R, placeholder, g_panel_border);
        int text_w = 0, text_h = 0;
        measure_text("Record", &text_w, &text_h);
        int text_x = placeholder.x + (placeholder.w - text_w) / 2;
        int text_y = placeholder.y + (placeholder.h - text_h) / 2;
        draw_text(R, text_x, text_y, "Record", disabledTxt);
    }
    else
    {
#endif
        // Export button: mutually exclusive with external MIDI Output. When MIDI Output
        // is enabled the Export button is shown disabled and does not accept clicks.
#if SUPPORT_MIDI_HW == TRUE
        bool export_allowed = !g_midi_output_enabled && !g_exporting && !modal_block && !g_reverbDropdownOpen;
#else
    bool export_allowed = !g_exporting && !modal_block && !g_reverbDropdownOpen;
#endif
        if (export_allowed)
        {
            if (ui_button(R, (Rect){340, 215, 80, 22}, "Export", ui_mx, ui_my, ui_mdown) && ui_mclick)
            {
#if SUPPORT_MIDI_HW == TRUE
                /* export_allowed already ensures g_midi_output_enabled == false */
#endif

                // Single song export
                // When export button clicked, open save dialog using extension depending on codec
                int export_dialog_type = 0; // 0=WAV, 1=FLAC, 2=MP3, 3=OGG/Opus
                BAECompressionType compression = BAE_COMPRESSION_NONE;

                if (g_exportCodecIndex >= 0 && g_exportCodecIndex < g_exportCompressionCount)
                {
                    compression = g_exportCompressionMap[g_exportCodecIndex];
                }

                if (compression == BAE_COMPRESSION_NONE)
                {
                    export_dialog_type = 0;
                }
#if USE_FLAC_ENCODER == TRUE
                else if (compression == BAE_COMPRESSION_LOSSLESS)
                {
                    export_dialog_type = 1;
                }
#endif
#if USE_MPEG_ENCODER == TRUE
                else if (compression >= BAE_COMPRESSION_MPEG_32 && compression <= BAE_COMPRESSION_MPEG_320)
                {
                    export_dialog_type = 2;
                }
#endif
#if USE_VORBIS_ENCODER == TRUE
                else if (compression >= BAE_COMPRESSION_VORBIS_96 && compression <= BAE_COMPRESSION_VORBIS_320)
                {
                    export_dialog_type = 3;
                }
#endif
#if USE_OPUS_ENCODER == TRUE
                else if (compression >= BAE_COMPRESSION_OPUS_16 && compression <= BAE_COMPRESSION_OPUS_256)
                {
                    export_dialog_type = 4;
                }
#endif
                else {
                    // Default to WAV if something's wrong with the mapping
                    export_dialog_type = 0;
                }

                char *export_file = save_export_dialog(export_dialog_type);
                if (export_file)
                {
                    // Ensure correct file extension based on export type
                    size_t L = strlen(export_file);
                    const char *expected_ext = NULL;

                    if (compression == BAE_COMPRESSION_LOSSLESS)
                    {
                        expected_ext = ".flac";
                    }
                    else if (compression >= BAE_COMPRESSION_MPEG_32 && compression <= BAE_COMPRESSION_MPEG_320)
                    {
                        expected_ext = ".mp3";
                    }
#if USE_OPUS_ENCODER == TRUE
                    else if (compression >= BAE_COMPRESSION_OPUS_16 && compression <= BAE_COMPRESSION_OPUS_256)
                    {
                        expected_ext = ".opus";
                    }
#endif
                    else if (compression >= BAE_COMPRESSION_VORBIS_96 && compression <= BAE_COMPRESSION_VORBIS_320)
                    {
                        expected_ext = ".ogg";
                    }
                    else
                    {
                        expected_ext = ".wav";
                    }

                    int ext_len = strlen(expected_ext);
                    if (L < ext_len || strcasecmp(export_file + L - ext_len, expected_ext) != 0)
                    {
                        // Append the correct extension
                        size_t n = L + ext_len + 1;
                        char *tmp = malloc(n);
                        if (tmp)
                        {
                            snprintf(tmp, n, "%s%s", export_file, expected_ext);
                            free(export_file);
                            export_file = tmp;
                        }
                    }

                    // Start export using selected codec mapping
                    // Map our index to BAEMixer compression enums using table
                    if (!g_bae.song_loaded || g_bae.is_audio_file)
                    {
                        set_status_message("Cannot export: No MIDI/RMF loaded");
                    }
                    else
                    {
                        // Save current state
                        BAEFileType export_file_type = BAE_WAVE_TYPE;

                        // Determine file type based on compression type
                        if (compression == BAE_COMPRESSION_NONE)
                        {
                            export_file_type = BAE_WAVE_TYPE;
                        }
#if USE_FLAC_ENCODER == TRUE
                        else if (compression == BAE_COMPRESSION_LOSSLESS)
                        {
                            export_file_type = BAE_FLAC_TYPE;
                        }
#endif
#if USE_MPEG_ENCODER == TRUE
                        else if (compression >= BAE_COMPRESSION_MPEG_64 && compression <= BAE_COMPRESSION_MPEG_320)
                        {
                            export_file_type = BAE_MPEG_TYPE;
                        }
#endif
#if USE_VORBIS_ENCODER == TRUE
                        else if (compression >= BAE_COMPRESSION_VORBIS_96 && compression <= BAE_COMPRESSION_VORBIS_320)
                        {
                            export_file_type = BAE_VORBIS_TYPE;
                        }
#endif
#if USE_OPUS_ENCODER == TRUE
                        else if (compression >= BAE_COMPRESSION_OPUS_16 && compression <= BAE_COMPRESSION_OPUS_256)
                        {
                            export_file_type = BAE_OPUS_TYPE;
                        }
#endif

                        // Determine export file type so service loop can apply MPEG heuristics
                        g_export_file_type = export_file_type;
                        bae_start_export(export_file, export_file_type, compression);
                    }
                    free(export_file);
                }
                ui_mclick = false; // consume click
            }
        }
        else
        {
            // Draw disabled Export button (no interaction)
            Rect disr = {340, 215, 80, 22}; // Moved from 320 to 340
            SDL_Color disabledBg = g_panel_bg;
            SDL_Color disabledTxt = g_panel_border;
            disabledBg.a = 200;
            disabledTxt.a = 200;
            draw_rect(R, disr, disabledBg);
            draw_frame(R, disr, g_panel_border);
            int text_w = 0, text_h = 0;
            measure_text("Export", &text_w, &text_h);
            int text_x = disr.x + (disr.w - text_w) / 2;
            int text_y = disr.y + (disr.h - text_h) / 2;
            draw_text(R, text_x, text_y, "Export", disabledTxt);
        }

#if SUPPORT_MIDI_HW == TRUE
    }
#endif
    // RMF Info button (only for RMF files)
    if (g_bae.is_rmf_file)
    {
        // RMF Info button position (moved from 440 to 640 to avoid overlap with Record Format button)
        int rmf_x_pos = 798;

        if (ui_button(R, (Rect){rmf_x_pos, 215, 80, 22}, "RMF Info", ui_mx, ui_my, ui_mdown) && ui_mclick && !modal_block)
        {
            if (g_show_rmf_info_dialog)
            {
                g_show_rmf_info_dialog = false;
            }
            else
            {
                g_show_rmf_info_dialog = true;
                rmf_info_load_if_needed();
            }
        }
    }
}
#if SUPPORT_MIDI_HW == TRUE
// If MIDI input is enabled, paint a semi-transparent overlay over the transport panel
// to dim it and disable interactions except the Stop button (which we keep active).
if (g_midi_input_enabled)
{
    SDL_Color dim = g_is_dark_mode ? (SDL_Color){0, 0, 0, 160} : (SDL_Color){255, 255, 255, 160};
    draw_rect(R, transportPanel, dim);
    // Redraw Stop button on top of the dim overlay so the user can stop
    Rect stopRect = {90, 215, 60, 22};
    // Use raw mouse coords so the Stop button remains clickable even when modal_block is true
    // When MIDI-in is active the Stop button actually stops external playback (external/"♪s"), so label it accordingly.
    const char *stop_label = g_midi_input_enabled ? "Stop ♪s" : "Stop";
    if (ui_button(R, stopRect, stop_label, mx, my, mdown) && mclick)
    {
        bae_stop(&playing, &progress);
        // Ensure engine releases any held notes when user stops playback (panic)
        midi_output_send_all_notes_off(); // silence any external device too
        if (g_bae.song)
        {
            gui_panic_all_notes(g_bae.song);
        }
        if (g_live_song)
        {
            gui_panic_all_notes(g_live_song);
        }
        if (g_show_virtual_keyboard)
        {
            BAESong target = g_bae.song ? g_bae.song : g_live_song;
            if (target)
            {
                for (int n = 0; n < BAE_MAX_NOTES; n++)
                {
                    BAESong_NoteOff(target, (unsigned char)g_keyboard_channel, (unsigned char)n, 0, 0);
                }
            }
            g_keyboard_mouse_note = -1;
            memset(g_keyboard_active_notes, 0, sizeof(g_keyboard_active_notes));
            g_keyboard_suppress_until = SDL_GetTicks() + 33;
        }
        // Reset total-play timer on user Stop
                gui_transport_reset_session_timer();
        // Also stop export if active
        if (g_exporting)
        {
            bae_stop_wav_export();
        }
        // consume the click so underlying UI doesn't react to the same event
        mclick = false;
    }
    // Also draw Record/Stop in the Export slot so MIDI users can record even when panel is dimmed
    Rect recRect = {348, 215, 80, 22}; // Moved from 320 to 348
    if (!g_midi_recording)
    {
        // Disable Record button when a dialog/modal is open
        if (!modal_block && ui_button(R, recRect, "Record", mx, my, mdown) && mclick)
        {
            // Behavior depends on selected format
            if (g_midiRecordFormatIndex == 0)
            {
                // MIDI: existing flow
                char *export_file = save_midi_dialog();
                if (export_file)
                {
                    // ensure .mid extension
                    size_t L = strlen(export_file);
                    if (L < 4 || strcasecmp(export_file + L - 4, ".mid") != 0)
                    {
                        size_t n = L + 5;
                        char *tmp = malloc(n);
                        if (tmp)
                        {
                            snprintf(tmp, n, "%s.mid", export_file);
                            free(export_file);
                            export_file = tmp;
                        }
                    }
                    if (export_file)
                    {
                        bool started = midi_record_start(export_file);
                        free(export_file);
                        if (!started)
                            set_status_message("Failed to start MIDI record");
                    }
                }
            }
            else
            {
                // WAV or MP3: start mixer export to file
                // If no file is loaded, we can still record from MIDI-in using the lightweight live song.
                if (!g_bae.song_loaded && !g_live_song)
                {
                    set_status_message("Cannot record audio: No MIDI/RMF loaded and no live song available");
                }
                else
                {
                    // Choose save dialog and extension based on format using helper function
                    MidiRecordFormatInfo format_info = get_midi_record_format_info(g_midiRecordFormatIndex);
                    int export_dialog_type = 0; // Default to WAV

                    switch (format_info.type)
                    {
                    case MIDI_RECORD_FORMAT_WAV:
                        export_dialog_type = 0; // WAV
                        break;
                    case MIDI_RECORD_FORMAT_FLAC:
                        export_dialog_type = 1; // FLAC
                        break;
                    case MIDI_RECORD_FORMAT_MP3:
                        export_dialog_type = 2; // MP3
                        break;
                    case MIDI_RECORD_FORMAT_VORBIS:
                        export_dialog_type = 3; // OGG/Vorbis
                        break;
                    case MIDI_RECORD_FORMAT_OPUS:
                        export_dialog_type = 4; // OPUS
                        break;                                
                    default:
                        export_dialog_type = 0; // fallback to WAV
                        break;
                    }
                    char *export_file = save_export_dialog(export_dialog_type);
                    if (export_file)
                    {
                        // ensure correct extension
                        size_t L = strlen(export_file);
                        if (g_midiRecordFormatIndex == 1)
                        { // WAV
                            if (L < 4 || strcasecmp(export_file + L - 4, ".wav") != 0)
                            {
                                size_t n = L + 5;
                                char *tmp = malloc(n);
                                if (tmp)
                                {
                                    snprintf(tmp, n, "%s.wav", export_file);
                                    free(export_file);
                                    export_file = tmp;
                                }
                            }
                        }
#if USE_FLAC_ENCODER != FALSE
                        else if (g_midiRecordFormatIndex == 2)
                        { // FLAC
                            if (L < 5 || strcasecmp(export_file + L - 5, ".flac") != 0)
                            {
                                size_t n = L + 6;
                                char *tmp = malloc(n);
                                if (tmp)
                                {
                                    snprintf(tmp, n, "%s.flac", export_file);
                                    free(export_file);
                                    export_file = tmp;
                                }
                            }
                        }
#endif
                        else if (g_midiRecordFormatIndex >= 3)
                        {
                            // Use helper function to determine format and extension
                            MidiRecordFormatInfo format_info = get_midi_record_format_info(g_midiRecordFormatIndex);
                            const char *ext = format_info.extension;
                            int ext_len = strlen(ext);

                            if (L < ext_len || strcasecmp(export_file + L - ext_len, ext) != 0)
                            {
                                size_t n = L + ext_len + 1;
                                char *tmp = malloc(n);
                                if (tmp)
                                {
                                    snprintf(tmp, n, "%s%s", export_file, ext);
                                    free(export_file);
                                    export_file = tmp;
                                }
                            }
                        }

                        // Save current state for restore. Use live song if no file-loaded song present.
                        BAESong target = g_bae.song ? g_bae.song : g_live_song;
                        uint32_t curPosUs = 0;
                        // If we're in MIDI-in mode, do not modify song playback (do not stop/seek/start).
                        if (!g_midi_input_enabled)
                        {
                            if (target)
                                BAESong_GetMicrosecondPosition(target, &curPosUs);
                        }
                        g_bae.position_us_before_export = curPosUs;
                        g_bae.was_playing_before_export = g_bae.is_playing;
                        g_bae.loop_was_enabled_before_export = g_bae.loop_enabled_gui;
                        if (!g_midi_input_enabled && g_bae.is_playing && target)
                        {
                            BAESong_Stop(target, FALSE);
                            g_bae.is_playing = false;
                        }

                        // Map selected format to BAE types using helper function
                        BAECompressionType compression = BAE_COMPRESSION_NONE;
                        MidiRecordFormatInfo format_info = get_midi_record_format_info(g_midiRecordFormatIndex);

                        if (format_info.type == MIDI_RECORD_FORMAT_WAV)
                        {
                            compression = BAE_COMPRESSION_NONE;
                        }
#if USE_FLAC_ENCODER != FALSE
                        else if (format_info.type == MIDI_RECORD_FORMAT_FLAC)
                        {
                            compression = BAE_COMPRESSION_LOSSLESS;
                        }
#endif
#if USE_MPEG_ENCODER != FALSE
                        else if (format_info.type == MIDI_RECORD_FORMAT_MP3)
                        {
                            // Map MP3 bitrate to compression type
                            switch (format_info.bitrate)
                            {
                            case 128000:
                                compression = BAE_COMPRESSION_MPEG_128;
                                break;
                            case 192000:
                                compression = BAE_COMPRESSION_MPEG_192;
                                break;
                            case 256000:
                                compression = BAE_COMPRESSION_MPEG_256;
                                break;
                            case 320000:
                                compression = BAE_COMPRESSION_MPEG_320;
                                break;
                            default:
                                compression = BAE_COMPRESSION_MPEG_128;
                                break;
                            }
                        }
#endif
#if USE_VORBIS_ENCODER == TRUE
                        else if (format_info.type == MIDI_RECORD_FORMAT_VORBIS)
                        {
                            // Map Vorbis bitrate to compression type
                            switch (format_info.bitrate)
                            {
                            case 96000:
                                compression = BAE_COMPRESSION_VORBIS_96;
                                break;
                            case 128000:
                                compression = BAE_COMPRESSION_VORBIS_128;
                                break;
                            case 256000:
                                compression = BAE_COMPRESSION_VORBIS_256;
                                break;
                            case 320000:
                                compression = BAE_COMPRESSION_VORBIS_320;
                                break;
                            default:
                                compression = BAE_COMPRESSION_VORBIS_128;
                                break;
                            }
                        }
#endif
#if USE_OPUS_ENCODER == TRUE
                        else if (format_info.type == MIDI_RECORD_FORMAT_OPUS)
                        {
                            // Map Opus bitrate to compression type
                            switch (format_info.bitrate)
                            {
                            case 16000:
                                compression = BAE_COMPRESSION_OPUS_16;
                                break;
                            case 32000:
                                compression = BAE_COMPRESSION_OPUS_32;
                                break;
                            case 64000:
                                compression = BAE_COMPRESSION_OPUS_64;
                                break;
                            case 96000:
                                compression = BAE_COMPRESSION_OPUS_96;
                                break;
                            case 128000:
                                compression = BAE_COMPRESSION_OPUS_128;
                                break;
                            case 256000:
                                compression = BAE_COMPRESSION_OPUS_256;
                                break;
                            default:
                                compression = BAE_COMPRESSION_OPUS_128;
                                break;
                            }
                        }
#endif

                        if (export_file)
                        {
                            // If WAV or FLAC selected, use our PCM capture path instead of BAEMixer file output
                            if (g_midiRecordFormatIndex == 1) // WAV
                            {
                                // start our own PCM WAV writer and start song/live-song to drive audio
                                int wav_channels = 2;
                                int wav_sr = g_sample_rate_hz > 0 ? g_sample_rate_hz : 44100;
                                bool started = pcm_wav_start(export_file, wav_channels, wav_sr, 16);
                                if (!started)
                                {
                                    set_status_message("Failed to open WAV file for recording");
                                    free(export_file);
                                }
                                else
                                {
                                    // If not in MIDI-in mode, start/seek/preroll the target song to drive engine audio.
                                    if (!g_midi_input_enabled)
                                    {
                                        if (target)
                                        {
                                            BAESong_Stop(target, FALSE);
                                            BAESong_SetMicrosecondPosition(target, 0);
                                            BAESong_Preroll(target);
                                        }
                                        BAEResult rs = target ? BAESong_Start(target, 0) : BAE_NO_ERROR;
                                        if (rs != BAE_NO_ERROR)
                                        {
                                            set_status_message("Failed to start song for WAV recording");
                                            pcm_wav_finalize();
                                            free(export_file);
                                        }
                                        else
                                        {
                                            g_bae.is_playing = true;
                                            g_pcm_wav_recording = true;
                                            g_exporting = false; // use our own writer
                                            safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                            g_export_path[sizeof(g_export_path) - 1] = '\0';
                                            set_status_message("WAV recording started");
                                            free(export_file);
                                        }
                                    }
                                    else
                                    {
                                        // MIDI-in mode: do not change engine playback; just enable PCM writer.
                                        g_pcm_wav_recording = true;
                                        g_exporting = false; // use our own writer
                                        safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                        g_export_path[sizeof(g_export_path) - 1] = '\0';
                                        set_status_message("WAV recording started");
                                        free(export_file);
                                    }
                                }
                            }
#if USE_FLAC_ENCODER != FALSE
                            else if (g_midiRecordFormatIndex == 2) // FLAC
                            {
                                // start our own PCM FLAC writer and start song/live-song to drive audio
                                int flac_channels = 2;
                                int flac_sr = g_sample_rate_hz > 0 ? g_sample_rate_hz : 44100;
                                set_status_message("Attempting FLAC recording...");
                                BAE_PRINTF("GUI: Attempting FLAC recording - channels=%d, sr=%d, file=%s\n", flac_channels, flac_sr, export_file);
                                bool started = pcm_flac_start(export_file, flac_channels, flac_sr, 16);
                                if (!started)
                                {
                                    set_status_message("Failed to open FLAC file for recording");
                                    free(export_file);
                                }
                                else
                                {
                                    // If not in MIDI-in mode, start/seek/preroll the target song to drive engine audio.
                                    if (!g_midi_input_enabled)
                                    {
                                        if (target)
                                        {
                                            BAESong_Stop(target, FALSE);
                                            BAESong_SetMicrosecondPosition(target, 0);
                                            BAESong_Preroll(target);
                                        }
                                        BAEResult rs = target ? BAESong_Start(target, 0) : BAE_NO_ERROR;
                                        if (rs != BAE_NO_ERROR)
                                        {
                                            set_status_message("Failed to start song for FLAC recording");
                                            pcm_flac_finalize();
                                            free(export_file);
                                        }
                                        else
                                        {
                                            g_bae.is_playing = true;
                                            g_pcm_flac_recording = true;
                                            g_midi_recording = true;
                                            g_exporting = false; // use our own writer
                                            safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                            g_export_path[sizeof(g_export_path) - 1] = '\0';
                                            set_status_message("FLAC recording started");
                                            free(export_file);
                                        }
                                    }
                                    else
                                    {
                                        // MIDI-in mode: do not change engine playback; just enable PCM writer.
                                        g_pcm_flac_recording = true;
                                        g_midi_recording = true;
                                        g_exporting = false; // use our own writer
                                        safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                        g_export_path[sizeof(g_export_path) - 1] = '\0';
                                        set_status_message("FLAC recording started");
                                        free(export_file);
                                    }
                                }
                            }
#endif
                            else
                            {
                                // MP3, Vorbis, or Opus selected - check format type
                                if (format_info.type == MIDI_RECORD_FORMAT_MP3)
                                {
#if USE_MPEG_ENCODER != FALSE
                                    if (g_midi_input_enabled)
                                    {
                                        // Use platform MP3 recorder for MIDI-in; do not start BAESong
                                        int mp3_channels = 2;
                                        int mp3_sr = g_sample_rate_hz > 0 ? g_sample_rate_hz : 44100;
                                        int bitrate = format_info.bitrate;
                                        int rc = BAE_Platform_MP3Recorder_Start(export_file, (uint32_t)mp3_channels, (uint32_t)mp3_sr, 16, (uint32_t)bitrate);
                                        if (rc != 0)
                                        {
                                            set_status_message("Failed to start MP3 recorder");
                                            free(export_file);
                                        }
                                        else
                                        {
                                            g_pcm_mp3_recording = true;
                                            g_midi_recording = true;
                                            g_exporting = false;
                                            safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                            g_export_path[sizeof(g_export_path) - 1] = '\0';
                                            set_status_message("MP3 recording started");
                                            free(export_file);
                                        }
                                    }
                                    else
                                    {
                                        // Fallback to BAEMixer path for normal song export to MP3
                                        BAEResult result = BAEMixer_StartOutputToFile(g_bae.mixer, (BAEPathName)export_file,
                                                                                      BAE_MPEG_TYPE,
                                                                                      compression);
                                        if (result != BAE_NO_ERROR)
                                        {
                                            char msg[128];
                                            snprintf(msg, sizeof(msg), "MP3 export failed to start (%d)", result);
                                            set_status_message(msg);
                                            free(export_file);
                                        }
                                        else
                                        {
                                            g_exporting = true;
                                            g_export_file_type = BAE_MPEG_TYPE;
                                            safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                            g_export_path[sizeof(g_export_path) - 1] = '\0';
                                            set_status_message("MP3 export started");
                                            free(export_file);
                                        }
                                    }
#else
            set_status_message("MP3 export not supported in this build");
            free(export_file);
#endif
                                }
                                else if (format_info.type == MIDI_RECORD_FORMAT_VORBIS)
                                {
#if USE_VORBIS_ENCODER == TRUE
                                    // Use our own PCM Vorbis writer for real-time recording
                                    int vorbis_channels = 2;
                                    int vorbis_sr = g_sample_rate_hz > 0 ? g_sample_rate_hz : 44100;
                                    int bitrate = format_info.bitrate;
                                    bool started = pcm_vorbis_start(export_file, vorbis_channels, vorbis_sr, 16, bitrate);
                                    if (!started)
                                    {
                                        set_status_message("Failed to open Vorbis file for recording");
                                        free(export_file);
                                    }
                                    else
                                    {
                                        // If not in MIDI-in mode, start/seek/preroll the target song to drive engine audio.
                                        if (!g_midi_input_enabled)
                                        {
                                            if (target)
                                            {
                                                BAESong_Stop(target, FALSE);
                                                BAESong_SetMicrosecondPosition(target, 0);
                                                BAESong_Preroll(target);
                                            }
                                            BAEResult rs = target ? BAESong_Start(target, 0) : BAE_NO_ERROR;
                                            if (rs != BAE_NO_ERROR)
                                            {
                                                set_status_message("Failed to start song for Vorbis recording");
                                                pcm_vorbis_finalize();
                                                free(export_file);
                                            }
                                            else
                                            {
                                                g_bae.is_playing = true;
                                                g_pcm_vorbis_recording = true;
                                                g_midi_recording = true;
                                                g_exporting = false; // use our own writer
                                                safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                                g_export_path[sizeof(g_export_path) - 1] = '\0';
                                                set_status_message("Vorbis recording started");
                                                free(export_file);
                                            }
                                        }
                                        else
                                        {
                                            // MIDI-in mode: do not change engine playback; just enable PCM writer.
                                            g_pcm_vorbis_recording = true;
                                            g_midi_recording = true;
                                            g_exporting = false; // use our own writer
                                            safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                            g_export_path[sizeof(g_export_path) - 1] = '\0';
                                            set_status_message("Vorbis recording started");
                                            free(export_file);
                                        }
                                    }
#else
                                set_status_message("Vorbis export not supported in this build");
                                free(export_file);
#endif
                                }
                                else if (format_info.type == MIDI_RECORD_FORMAT_OPUS)
                                {
#if USE_OPUS_ENCODER == TRUE
                                    // Use our own PCM Opus writer for real-time recording
                                    int opus_channels = 2;
                                    int opus_sr = g_sample_rate_hz > 0 ? g_sample_rate_hz : 44100;
                                    int bitrate = format_info.bitrate;
                                    bool started = pcm_opus_start(export_file, opus_channels, opus_sr, 16, bitrate);
                                    if (!started)
                                    {
                                        set_status_message("Failed to open Opus file for recording");
                                        free(export_file);
                                    }
                                    else
                                    {
                                        // If not in MIDI-in mode, start/seek/preroll the target song to drive engine audio.
                                        if (!g_midi_input_enabled)
                                        {
                                            if (target)
                                            {
                                                BAESong_Stop(target, FALSE);
                                                BAESong_SetMicrosecondPosition(target, 0);
                                                BAESong_Preroll(target);
                                            }
                                            BAEResult rs = target ? BAESong_Start(target, 0) : BAE_NO_ERROR;
                                            if (rs != BAE_NO_ERROR)
                                            {
                                                set_status_message("Failed to start song for Opus recording");
                                                pcm_opus_finalize();
                                                free(export_file);
                                            }
                                            else
                                            {
                                                g_bae.is_playing = true;
                                                g_pcm_opus_recording = true;
                                                g_midi_recording = true;
                                                g_exporting = false; // use our own writer
                                                safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                                g_export_path[sizeof(g_export_path) - 1] = '\0';
                                                set_status_message("Opus recording started");
                                                free(export_file);
                                            }
                                        }
                                        else
                                        {
                                            // MIDI-in mode: do not change engine playback; just enable PCM writer.
                                            g_pcm_opus_recording = true;
                                            g_midi_recording = true;
                                            g_exporting = false; // use our own writer
                                            safe_strncpy(g_export_path, export_file, sizeof(g_export_path) - 1);
                                            g_export_path[sizeof(g_export_path) - 1] = '\0';
                                            set_status_message("Opus recording started");
                                            free(export_file);
                                        }
                                    }
#else
                                set_status_message("Opus export not supported in this build");
                                free(export_file);
#endif
                                }
                                else
                                {
                                    set_status_message("Unsupported export format");
                                    free(export_file);
                                }
                            }
                        }
                    }
                }
            }
            mclick = false;
        }
        else if (modal_block)
        {
            // Draw disabled Record button (no interaction)
            SDL_Color disabledBg = g_panel_bg;
            SDL_Color disabledTxt = g_panel_border;
            disabledBg.a = 200;
            disabledTxt.a = 200;
            draw_rect(R, recRect, disabledBg);
            draw_frame(R, recRect, g_panel_border);
            int text_w = 0, text_h = 0;
            measure_text("Record", &text_w, &text_h);
            int text_x = recRect.x + (recRect.w - text_w) / 2;
            int text_y = recRect.y + (recRect.h - text_h) / 2;
            draw_text(R, text_x, text_y, "Record", disabledTxt);
        }
    }
    else
    {
        // Stop either MIDI recording or active export
        if (ui_button(R, recRect, "Stop", mx, my, mdown) && mclick)
        {
            // If MIDI recording is active, stop and save - use helper function
            MidiRecordFormatInfo stop_format_info = get_midi_record_format_info(g_midiRecordFormatIndex);

            if (stop_format_info.type == MIDI_RECORD_FORMAT_MIDI)
            {
                if (!midi_record_stop())
                {
                    set_status_message("Failed to finalize MIDI file");
                }
            }
            else if (stop_format_info.type == MIDI_RECORD_FORMAT_WAV)
            {
                // WAV: if using our PCM writer, finalize it
                if (g_pcm_wav_recording)
                {
                    pcm_wav_finalize();
                }
                else if (g_exporting)
                {
                    bae_stop_wav_export();
                }
                else
                {
                    set_status_message("No WAV export in progress");
                }
            }
#if USE_FLAC_ENCODER == TRUE
            else if (stop_format_info.type == MIDI_RECORD_FORMAT_FLAC)
            {
                // FLAC: if using our PCM writer, finalize it
                if (g_pcm_flac_recording)
                {
                    pcm_flac_finalize();
                }
                else if (g_exporting)
                {
                    bae_stop_wav_export(); // Falls back to normal export stop
                }
                else
                {
                    set_status_message("No FLAC export in progress");
                }
            }
#endif
#if USE_MPEG_ENCODER == TRUE
            else if (stop_format_info.type == MIDI_RECORD_FORMAT_MP3)
            {
                // MP3 format
                if (g_midi_input_enabled && g_pcm_mp3_recording)
                {
                    BAE_Platform_MP3Recorder_Stop();
                    g_pcm_mp3_recording = false;
                    g_midi_recording = false;
                    set_status_message("MP3 recording saved");
                }
                else if (g_exporting)
                {
                    // BAEMixer-based export
                    bae_stop_wav_export();
                }
                else
                {
                    set_status_message("No MP3 export in progress");
                }
            }
#endif                    
            else if (stop_format_info.type == MIDI_RECORD_FORMAT_VORBIS)
            {
#if USE_VORBIS_ENCODER == TRUE
                // Vorbis: if using our PCM writer, finalize it
                if (g_pcm_vorbis_recording)
                {
                    pcm_vorbis_finalize();
                }
                else if (g_exporting)
                {
                    bae_stop_wav_export(); // Generic export stop function
                }
                else
                {
                    set_status_message("No Vorbis export in progress");
                }
#else
    set_status_message("Vorbis not supported in this build");
#endif
            }
            else if (stop_format_info.type == MIDI_RECORD_FORMAT_OPUS)
            {
#if USE_OPUS_ENCODER == TRUE
                // Opus: if using our PCM writer, finalize it
                if (g_pcm_opus_recording)
                {
                    pcm_opus_finalize();
                }
                else if (g_exporting)
                {
                    bae_stop_wav_export(); // Generic export stop function
                }
                else
                {
                    set_status_message("No Opus export in progress");
                }
#else
    set_status_message("Opus not supported in this build");
#endif
            }
            else
            {
                // Unknown format - fallback
                if (g_exporting)
                {
                    bae_stop_wav_export();
                }
                else
                {
                    set_status_message("No export in progress");
                }
            }
            mclick = false;
        }
    }
}
#endif

    *ctx->playing = playing;
    *ctx->progress = progress;
    *ctx->duration = duration;
    *ctx->loop_enabled = loopPlay;
    ctx->ui_mclick = ui_mclick;
    if (ctx->mclick)
        *ctx->mclick = mclick;

}
