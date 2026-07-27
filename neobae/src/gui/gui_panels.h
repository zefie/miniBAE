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

// Declarations for extracted UI panel helpers
#ifndef GUI_PANELS_H
#define GUI_PANELS_H

#include "gui_common.h" // Rect
// Forward declare SDL_Renderer so we can accept it in helper declarations
typedef struct SDL_Renderer SDL_Renderer;

typedef struct
{
    Rect transportPanel;
    Rect chanDD;
    Rect ddRect;
    Rect keyboardPanel;
    Rect bankRect;
    Rect programRect;
    Rect playlistPanel;
    int playlistPanelHeight;
} UiLayout;

void compute_ui_layout(UiLayout *L);
int get_reverb_count(void);
const char *get_reverb_name(int idx);

// Custom reverb modal state
extern bool g_show_custom_reverb_dialog;
extern bool g_custom_reverb_button_visible;

// Accumulates mouse wheel ticks while the custom reverb dialog is open.
// Positive = wheel up, negative = wheel down. Consumed by render_custom_reverb_dialog().
extern int g_custom_reverb_wheel_delta;

// Increment this to force the custom reverb dialog to re-sync its cached slider values
// from the backend (e.g. when a preset is loaded).
extern int g_custom_reverb_dialog_sync_serial;

// Custom reverb dialog rendering
void render_custom_reverb_dialog(SDL_Renderer *R, int mx, int my, bool mclick, bool mdown, int window_h);

// Bank/MSB/LSB helper
void change_bank_value_for_current_channel(bool is_msb, int delta);

// Modal gating helper used by event handlers
bool ui_modal_blocking(void);

// Tooltip helpers
void ui_set_tooltip(Rect r, const char *text, bool *visible_ptr, Rect *rect_ptr, char *text_buf, size_t text_buf_len);
void ui_clear_tooltip(bool *visible_ptr);
// Draw a tooltip rectangle with shadow, background, border and text.
// center_vertically: if true center the text vertically inside tipRect; otherwise draw with 4px top padding.
// use_panel_border: toggles which border/shade style to use (file/bank style vs loop/voice style).
void ui_draw_tooltip(SDL_Renderer *R, Rect tipRect, const char *text, bool center_vertically, bool use_panel_border);

// Slider helpers
bool ui_adjust_transpose(int mx, int my, int delta, bool playback_controls_enabled_local, int *transpose_ptr);
bool ui_adjust_tempo(int mx, int my, int delta, bool playback_controls_enabled_local, int *tempo_ptr, int *duration_out, int *progress_out);
bool ui_adjust_volume(int mx, int my, int delta, bool volume_enabled_local, int *volume_ptr);

#endif // GUI_PANELS_H
