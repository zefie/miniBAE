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

#ifndef GUI_EXPORT_H
#define GUI_EXPORT_H

#include "gui_common.h"
#include "gui_frame.h"
#include "NeoBAE.h"

// Export state globals
extern bool g_exporting;
extern int g_export_progress;
extern uint32_t g_export_last_pos;
extern int g_export_stall_iters;
extern char g_export_path[1024];
extern int g_export_file_type;
extern uint32_t g_export_last_device_samples;
extern int g_export_stable_loops;
extern bool g_export_realtime_mode;
extern bool g_export_using_live_song;

// Export codec settings
extern bool g_exportDropdownOpen;
extern int g_exportCodecIndex;
extern const char *g_exportCodecNames[];
extern const int g_exportCodecCount;

extern bool g_midiRecordFormatDropdownOpen;
#if SUPPORT_MIDI_HW == TRUE
extern int g_midiRecordFormatIndex;
extern const char *g_midiRecordFormatNames[];
extern const int g_midiRecordFormatCount;

// Helper types and functions for MIDI record format handling
typedef enum {
    MIDI_RECORD_FORMAT_MIDI = 0,
    MIDI_RECORD_FORMAT_WAV = 1,
    MIDI_RECORD_FORMAT_FLAC = 2,
    MIDI_RECORD_FORMAT_MP3 = 3,
    MIDI_RECORD_FORMAT_VORBIS = 4,
    MIDI_RECORD_FORMAT_OPUS = 5
} MidiRecordFormatType;

typedef struct {
    MidiRecordFormatType type;
    int bitrate;  // for MP3/Vorbis/Opus, ignored for MIDI/WAV/FLAC
    const char* extension;
} MidiRecordFormatInfo;

MidiRecordFormatInfo get_midi_record_format_info(int index);
#endif

extern const BAECompressionType g_exportCompressionMap[];
extern const int g_exportCompressionCount;

// Export tick callback - called from the export thread between service calls
// so scripts can track position during non-realtime export
typedef void (*ExportTickFn)(void *userdata);
void bae_set_export_tick_callback(ExportTickFn fn, void *userdata);

// Function declarations
void export_init(void);
void export_cleanup(void);
bool bae_start_export(const char *output_file, int export_type, int compression);
#if USE_MPEG_ENCODER != FALSE
bool bae_start_mpeg_export(const char *output_file, int codec_index);
#endif
void bae_stop_wav_export(void);
void bae_signal_export_stop(void);
void bae_service_wav_export(void);
char *save_export_dialog(int export_type); // 0=WAV, 1=FLAC, 2=MP3, 3=OGG, 4=OPUS
char *save_midi_dialog(void);

void gui_export_draw_controls(GuiFrameCtx *ctx);

#endif // GUI_EXPORT_H
