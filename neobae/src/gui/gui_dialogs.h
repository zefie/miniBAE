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

#ifndef GUI_DIALOGS_H
#define GUI_DIALOGS_H

#include "gui_common.h"
#include "NeoBAE.h"

// Dialog state globals
extern bool g_show_rmf_info_dialog;
extern bool g_rmf_info_loaded;
extern char g_rmf_info_values[INFO_TYPE_COUNT][512];

extern bool g_show_about_dialog;
extern int g_about_page;
extern int g_about_credits_scroll;
extern int g_about_wheel_delta;

// Tooltip state
extern bool g_bank_tooltip_visible;
extern Rect g_bank_tooltip_rect;
extern char g_bank_tooltip_text[520];

extern bool g_file_tooltip_visible;
extern Rect g_file_tooltip_rect;
extern char g_file_tooltip_text[520];

// Reverb button tooltip state
extern bool g_reverb_tooltip_visible;
extern Rect g_reverb_tooltip_rect;
extern char g_reverb_tooltip_text[520];

extern bool g_loop_tooltip_visible;
extern Rect g_loop_tooltip_rect;
extern char g_loop_tooltip_text[520];

extern bool g_voice_tooltip_visible;
extern Rect g_voice_tooltip_rect;
extern char g_voice_tooltip_text[520];

extern bool g_program_tooltip_visible;
extern Rect g_program_tooltip_rect;
extern char g_program_tooltip_text[520];

extern bool g_dls_compat_tooltip_visible;
extern Rect g_dls_compat_tooltip_rect;
extern char g_dls_compat_tooltip_text[520];

// Function declarations
const char *rmf_info_label(BAEInfoType t);
void rmf_info_reset(void);
void rmf_info_load_if_needed(void);

// Platform file dialogs
char *open_file_dialog(void);
char *open_folder_dialog(void); // For folder selection to add all files
char *open_playlist_dialog(void); // For M3U playlist files
char *save_playlist_dialog(void); // For saving M3U playlist files

// Neo reverb preset (.neoreverb) dialogs
char *open_neoreverb_dialog(void);
char *save_neoreverb_dialog(const char *default_name);

// Dialog rendering functions
void render_rmf_info_dialog(SDL_Renderer *R, int mx, int my, bool mclick);
void render_about_dialog(SDL_Renderer *R, int mx, int my, bool mclick);
void render_eq_dialog(SDL_Renderer *R, int mx, int my, bool mclick, bool mdown, int window_h);
int get_eq_preset_count(void);
const char *get_eq_preset_name(int idx);

extern bool g_show_eq_dialog;

// Dialog system initialization/cleanup
void dialogs_init(void);
void dialogs_cleanup(void);

#ifdef _WIN32
// Windows file dialog functions
void show_open_file_dialog(char *result_path, int result_size, const char *filter, const char *title);
void show_save_file_dialog(char *result_path, int result_size, const char *filter, const char *title, const char *default_name);
#endif

#endif // GUI_DIALOGS_H
