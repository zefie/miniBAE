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

/* Ensure POSIX APIs (popen/strcasecmp/realpath) are visible. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "gui_frame.h"
#include "gui_bae.h"
#include "gui_widgets.h"
#include "gui_text.h"
#include "gui_theme.h"
#include "gui_panels.h"
#include "gui_settings.h"
#include "gui_dialogs.h"
#include "gui_export.h"
#include "gui_midi.h"
#include "gui_playlist.h"
#include "X_Assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#if SUPPORT_MIDI_HW == TRUE
#include "gui_midi_hw.h"
#endif
#if SUPPORT_KARAOKE == TRUE
#include "gui_karaoke.h"
#endif
#if USE_SF2_SUPPORT == TRUE
#if _USING_FLUIDLITE == TRUE
#include "GenSF2_FluidLite.h"
#endif
#endif
#if USE_NATIVE_DLS == TRUE
#include "GenDLS_MobileBAE.h"
#if USE_XMF_SUPPORT == TRUE
#include "GenXMF.h"
#endif
#endif

#include "gui_playback_controls.h"
#include <math.h>

static void draw_io_arrow_icon(SDL_Renderer *R, Rect r, bool up, SDL_Color col)
{
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, col.r, col.g, col.b, col.a);

    int cx = r.x + r.w / 2;
    int top = r.y + 4;
    int bot = r.y + r.h - 4;
    int w = (r.w < r.h ? r.w : r.h) / 3;
    if (w < 3)
        w = 3;

    if (up)
    {
#if USE_SDL2 == TRUE
        SDL_RenderDrawLine(R, cx, bot, cx, top + 2);
        SDL_RenderDrawLine(R, cx, top + 2, cx - w, top + 2 + w);
        SDL_RenderDrawLine(R, cx, top + 2, cx + w, top + 2 + w);
        SDL_RenderDrawLine(R, r.x + 5, bot, r.x + r.w - 6, bot);
#else
        SDL_RenderLine(R, cx, bot, cx, top + 2);
        SDL_RenderLine(R, cx, top + 2, cx - w, top + 2 + w);
        SDL_RenderLine(R, cx, top + 2, cx + w, top + 2 + w);
        SDL_RenderLine(R, r.x + 5, bot, r.x + r.w - 6, bot);
#endif
    }
    else
    {
#if USE_SDL2 == TRUE
        SDL_RenderDrawLine(R, cx, top, cx, bot - 2);
        SDL_RenderDrawLine(R, cx, bot - 2, cx - w, bot - 2 - w);
        SDL_RenderDrawLine(R, cx, bot - 2, cx + w, bot - 2 - w);
        SDL_RenderDrawLine(R, r.x + 5, top, r.x + r.w - 6, top);
#else
        SDL_RenderLine(R, cx, top, cx, bot - 2);
        SDL_RenderLine(R, cx, bot - 2, cx - w, bot - 2 - w);
        SDL_RenderLine(R, cx, bot - 2, cx + w, bot - 2 - w);
        SDL_RenderLine(R, r.x + 5, top, r.x + r.w - 6, top);
#endif
    }
}


void gui_playback_controls_draw(GuiFrameCtx *ctx)
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
    int transport_mx = ctx->transport_mx;
    int transport_my = ctx->transport_my;
    bool transport_mdown = ctx->transport_mdown;
    bool transport_mclick = ctx->transport_mclick;
    bool modal_block = ctx->modal_block;
    bool modal_block_transport = ctx->modal_block_transport;
    bool playing = *ctx->playing;
    int progress = *ctx->progress;
    int duration = *ctx->duration;
    int transpose = *ctx->transpose;
    int tempo = *ctx->tempo;
    int volume = *ctx->volume;
    int reverbType = *ctx->reverb_type;
    bool loopPlay = *ctx->loop_enabled;
    bool *ch_enable = ctx->ch_enable;
    int last_drag_progress = *ctx->last_drag_progress;
    Rect channelPanel = ctx->channelPanel;
    Rect controlPanel = ctx->controlPanel;
    Rect transportPanel = ctx->transportPanel;
    Rect keyboardPanel = ctx->keyboardPanel;
    Rect statusPanel = ctx->statusPanel;
    SDL_Color labelCol = ctx->labelCol;
    SDL_Color headerCol = ctx->headerCol;
    SDL_Color panelBg = ctx->panelBg;
    SDL_Color panelBorder = ctx->panelBorder;
    bool showKeyboard = ctx->showKeyboard;
    bool showWaveform = ctx->showWaveform;
    (void)showKeyboard;
    (void)showWaveform;
    (void)labelCol;
    (void)modal_block_transport;
    (void)transport_mx;
    (void)transport_my;
    (void)transport_mdown;
    (void)transport_mclick;
    (void)channelPanel;
    (void)controlPanel;
    (void)transportPanel;
    (void)keyboardPanel;
    (void)statusPanel;
    (void)panelBg;
    (void)panelBorder;
    (void)headerCol;
    (void)mx;
    (void)my;
    (void)mdown;
    (void)mclick;

// Control panel
draw_rect(R, controlPanel, panelBg);
draw_frame(R, controlPanel, panelBorder);
draw_text(R, 410, 20, "PLAYBACK CONTROLS", headerCol);

#if SUPPORT_MIDI_HW == TRUE
// When external MIDI input is active or playing audio files, dim and disable most playback controls
bool playback_controls_enabled = !g_midi_input_enabled && !(g_bae.is_audio_file && g_bae.sound);
#else
bool playback_controls_enabled = !(g_bae.is_audio_file && g_bae.sound);
#endif

    // While the reverb dropdown is expanded, disable transpose/tempo + reset controls.
    bool pitch_tempo_enabled = playback_controls_enabled && !g_reverbDropdownOpen;

// Transpose control
draw_text(R, 410, 45, "Transpose:", labelCol);
ui_slider(R, (Rect){410, 63, 160, 14}, &transpose, -24, 24, pitch_tempo_enabled ? ui_mx : -1, pitch_tempo_enabled ? ui_my : -1, pitch_tempo_enabled ? ui_mdown : false, pitch_tempo_enabled ? ui_mclick : false);
char tbuf[64];
snprintf(tbuf, sizeof(tbuf), "%+d", transpose);
draw_text(R, 577, 61, tbuf, labelCol);
Rect transposeResetBtn = (Rect){620, 59, 50, 20};
if (!pitch_tempo_enabled)
{
    SDL_Color disabled_bg = {g_button_base.r / 2, g_button_base.g / 2, g_button_base.b / 2, g_button_base.a};
    SDL_Color disabled_txt = {g_button_text.r / 2, g_button_text.g / 2, g_button_text.b / 2, g_button_text.a};
    SDL_Color disabled_border = {g_button_border.r / 2, g_button_border.g / 2, g_button_border.b / 2, g_button_border.a};
    draw_rect(R, transposeResetBtn, disabled_bg);
    draw_frame(R, transposeResetBtn, disabled_border);
    int text_w = 0, text_h = 0;
    measure_text("Reset", &text_w, &text_h);
    int text_x = transposeResetBtn.x + (transposeResetBtn.w - text_w) / 2;
    int text_y = transposeResetBtn.y + (transposeResetBtn.h - text_h) / 2;
    draw_text(R, text_x, text_y, "Reset", disabled_txt);
}
else if (ui_button(R, transposeResetBtn, "Reset", ui_mx, ui_my, ui_mdown) && ui_mclick && !modal_block)
{
    transpose = 0;
    bae_set_transpose(transpose);
}

// Tempo control
draw_text(R, 410, 85, "Tempo:", labelCol);
ui_slider(R, (Rect){410, 103, 160, 14}, &tempo, 25, 200, pitch_tempo_enabled ? ui_mx : -1, pitch_tempo_enabled ? ui_my : -1, pitch_tempo_enabled ? ui_mdown : false, pitch_tempo_enabled ? ui_mclick : false);
snprintf(tbuf, sizeof(tbuf), "%d%%", tempo);
draw_text(R, 577, 101, tbuf, labelCol);
Rect tempoResetBtn = (Rect){620, 99, 50, 20};
if (!pitch_tempo_enabled)
{
    SDL_Color disabled_bg = {g_button_base.r / 2, g_button_base.g / 2, g_button_base.b / 2, g_button_base.a};
    SDL_Color disabled_txt = {g_button_text.r / 2, g_button_text.g / 2, g_button_text.b / 2, g_button_text.a};
    SDL_Color disabled_border = {g_button_border.r / 2, g_button_border.g / 2, g_button_border.b / 2, g_button_border.a};
    draw_rect(R, tempoResetBtn, disabled_bg);
    draw_frame(R, tempoResetBtn, disabled_border);
    int text_w = 0, text_h = 0;
    measure_text("Reset", &text_w, &text_h);
    int text_x = tempoResetBtn.x + (tempoResetBtn.w - text_w) / 2;
    int text_y = tempoResetBtn.y + (tempoResetBtn.h - text_h) / 2;
    draw_text(R, text_x, text_y, "Reset", disabled_txt);
}
else if (ui_button(R, tempoResetBtn, "Reset", ui_mx, ui_my, ui_mdown) && ui_mclick && !modal_block)
{
    int oldTempo = tempo;
    int newTempo = 100;
    if (newTempo != oldTempo)
    {
        tempo = newTempo;
        bae_set_tempo(tempo);
    }
}

// Reverb controls (we always leave Reverb interactive even when MIDI input is enabled)
draw_text(R, 687, 20, "Reverb:", labelCol);
// Removed non-functional 'No Change' option; first entry now 'Default' (engine type 0)
// Removed engine reverb index 0 (NO_CHANGE). UI list now maps i -> engine index (i+1)
int reverbCount = get_reverb_count();
Rect ddRect = {687, 38, 160, 24}; // moved up 5px
// Closed dropdown: use theme globals
SDL_Color dd_bg = g_button_base;
SDL_Color dd_txt = g_button_text;
SDL_Color dd_frame = g_button_border;
bool overMain = point_in(ui_mx, ui_my, ddRect);
// Disable reverb dropdown when playing audio files (sounds, not songs)
bool reverb_enabled = !(g_bae.is_audio_file && g_bae.sound);
if (overMain && reverb_enabled)
    dd_bg = g_button_hover;
if (!reverb_enabled)
{
    dd_bg.a = 180;
    dd_txt.a = 180;
}
draw_rect(R, ddRect, dd_bg);
draw_frame(R, ddRect, dd_frame);
const char *cur = (reverbType >= 1 && reverbType <= reverbCount) ? get_reverb_name(reverbType - 1) : "?";
draw_text(R, ddRect.x + 6, ddRect.y + 3, cur, dd_txt);
draw_text(R, ddRect.x + ddRect.w - 16, ddRect.y + 3, g_reverbDropdownOpen ? "^" : "v", dd_txt);
if (overMain && ui_mclick && reverb_enabled)
{
    g_reverbDropdownOpen = !g_reverbDropdownOpen;
}

// Show "Customize", "+" and "-" buttons when Custom reverb or user preset is selected (below the dropdown)
#if USE_NEO_EFFECTS
g_custom_reverb_button_visible = (reverbType >= BAE_REVERB_TYPE_12 && reverbType != BAE_REVERB_TYPE_17 && reverbType != BAE_REVERB_TYPE_18);
if (g_custom_reverb_button_visible && reverb_enabled)
{
    // When the dropdown list is open, don't allow clicks on the buttons underneath it.
    bool custom_controls_enabled = (!g_reverbDropdownOpen && !modal_block);
    int btnY = ddRect.y + ddRect.h + 2;
    int spacing = 2;
    
    // Customize button
    int customTextW = 0, customTextH = 0;
    measure_text("Custom", &customTextW, &customTextH);
    int customBtnW = customTextW + 12; // padding
    if (customBtnW < 44)
        customBtnW = 44;
    Rect customBtn = {ddRect.x, btnY, customBtnW, 20};
    bool overCustom = custom_controls_enabled && point_in(ui_mx, ui_my, customBtn);
    SDL_Color btn_bg = overCustom ? g_button_hover : g_button_base;
    if (!custom_controls_enabled)
    {
        btn_bg.a = 180;
    }
    draw_rect(R, customBtn, btn_bg);
    draw_frame(R, customBtn, g_button_border);
    SDL_Color custom_txt = g_button_text;
    if (!custom_controls_enabled)
    {
        custom_txt.a = 180;
    }
    draw_text(R, customBtn.x + 4, customBtn.y + 3, "Custom", custom_txt);
    
    if (overCustom && ui_mclick)
    {
        // Force the custom reverb dialog to refresh its cached slider values from current engine state
        extern int g_custom_reverb_dialog_sync_serial;
        g_custom_reverb_dialog_sync_serial++;
        g_show_custom_reverb_dialog = true;
        g_reverbDropdownOpen = false; // Close dropdown when opening custom dialog
    }
    
    // Save preset button (+)
    Rect saveBtn = {customBtn.x + customBtn.w + spacing, btnY, 20, 20};
    bool overSave = custom_controls_enabled && point_in(ui_mx, ui_my, saveBtn);
    SDL_Color save_bg = overSave ? g_button_hover : g_button_base;
    if (!custom_controls_enabled)
    {
        save_bg.a = 180;
    }
    draw_rect(R, saveBtn, save_bg);
    draw_frame(R, saveBtn, g_button_border);
    SDL_Color save_txt = g_button_text;
    if (!custom_controls_enabled)
    {
        save_txt.a = 180;
    }
    draw_text(R, saveBtn.x + 6, saveBtn.y + 2, "+", save_txt);

    if (overSave)
    {
        const char *tip = "Save Preset";
        int tw = 0, th = 0;
        measure_text(tip, &tw, &th);
        int tx = ui_mx + 10;
        int ty = ui_my - 25;
        int tipw = tw + 8;
        int tiph = th + 8;
        if (tx + tipw > WINDOW_W - 4) tx = WINDOW_W - tipw - 4;
        if (ty < 4) ty = ui_my + 25;
        ui_set_tooltip((Rect){tx, ty, tipw, tiph}, tip, &g_reverb_tooltip_visible, &g_reverb_tooltip_rect, g_reverb_tooltip_text, sizeof(g_reverb_tooltip_text));
    }
    
    if (overSave && ui_mclick)
    {
        // Show text input dialog for preset name
        extern bool g_show_preset_name_dialog;
        extern char g_preset_name_input[64];
        extern int g_preset_name_cursor;
        extern bool g_preset_dialog_is_eq;
        
        g_preset_dialog_is_eq = false;
        
        // If we're on an existing custom preset, populate the text field with its name
        if (reverbType > BAE_REVERB_TYPE_19)
        {
            const char *current_name = get_reverb_name(reverbType - 1);
            if (current_name)
            {
                safe_strncpy(g_preset_name_input, current_name, sizeof(g_preset_name_input) - 1);
                g_preset_name_cursor = strlen(g_preset_name_input);
            }
        }
        else
        {
            // Clear the input for new presets
            memset(g_preset_name_input, 0, sizeof(g_preset_name_input));
            g_preset_name_cursor = 0;
        }
        
        g_show_preset_name_dialog = true;
    }
    
    // Delete preset button (-)
    Rect deleteBtn = {saveBtn.x + saveBtn.w + spacing, btnY, 20, 20};
    bool overDelete = custom_controls_enabled && point_in(ui_mx, ui_my, deleteBtn);
    // Only allow delete when a saved custom preset (after "Custom") is selected.
    // "Custom" itself is BAE_REVERB_TYPE_19.
    bool can_delete_preset = (reverbType > BAE_REVERB_TYPE_19);
    SDL_Color delete_bg = (overDelete && can_delete_preset) ? g_button_hover : g_button_base;
    if (!custom_controls_enabled || !can_delete_preset)
    {
        delete_bg.a = 180;
    }
    draw_rect(R, deleteBtn, delete_bg);
    draw_frame(R, deleteBtn, g_button_border);
    SDL_Color delete_txt = g_button_text;
    if (!custom_controls_enabled || !can_delete_preset)
    {
        delete_txt.a = 180;
    }
    draw_text(R, deleteBtn.x + 6, deleteBtn.y + 2, "-", delete_txt);

    if (overDelete)
    {
        const char *tip = "Delete Preset";
        int tw = 0, th = 0;
        measure_text(tip, &tw, &th);
        int tx = ui_mx + 10;
        int ty = ui_my - 25;
        int tipw = tw + 8;
        int tiph = th + 8;
        if (tx + tipw > WINDOW_W - 4) tx = WINDOW_W - tipw - 4;
        if (ty < 4) ty = ui_my + 25;
        ui_set_tooltip((Rect){tx, ty, tipw, tiph}, tip, &g_reverb_tooltip_visible, &g_reverb_tooltip_rect, g_reverb_tooltip_text, sizeof(g_reverb_tooltip_text));
    }
    
    if (overDelete && ui_mclick && can_delete_preset)
    {
        // Delete current custom reverb preset if it's a saved preset
        const char *preset_name = get_reverb_name(reverbType - 1);
        extern bool g_show_preset_delete_confirm_dialog;
        extern char g_preset_delete_name[64];
        extern bool g_preset_dialog_is_eq;
        
        g_preset_dialog_is_eq = false;
        g_show_preset_delete_confirm_dialog = true;
        safe_strncpy(g_preset_delete_name, preset_name, sizeof(g_preset_delete_name) - 1);
        g_preset_delete_name[sizeof(g_preset_delete_name) - 1] = '\0';
    }

    // Import/Export (.neoreverb, .neoreverb.xml)
    Rect importBtn = {deleteBtn.x + deleteBtn.w + spacing, btnY, 20, 20};
    Rect exportBtn = {importBtn.x + importBtn.w + spacing, btnY, 20, 20};

    bool overImport = custom_controls_enabled && point_in(ui_mx, ui_my, importBtn);
    SDL_Color import_bg = overImport ? g_button_hover : g_button_base;
    if (!custom_controls_enabled)
        import_bg.a = 180;
    draw_rect(R, importBtn, import_bg);
    draw_frame(R, importBtn, g_button_border);
    SDL_Color import_fg = g_button_text;
    if (!custom_controls_enabled)
        import_fg.a = 180;
    draw_io_arrow_icon(R, importBtn, false, import_fg);

    if (overImport)
    {
        const char *tip = "Import Preset";
        int tw = 0, th = 0;
        measure_text(tip, &tw, &th);
        int tx = ui_mx + 10;
        int ty = ui_my - 25;
        int tipw = tw + 8;
        int tiph = th + 8;
        if (tx + tipw > WINDOW_W - 4) tx = WINDOW_W - tipw - 4;
        if (ty < 4) ty = ui_my + 25;
        ui_set_tooltip((Rect){tx, ty, tipw, tiph}, tip, &g_reverb_tooltip_visible, &g_reverb_tooltip_rect, g_reverb_tooltip_text, sizeof(g_reverb_tooltip_text));
    }

    extern char g_current_custom_reverb_preset[64];
    bool can_export_preset = (g_current_custom_reverb_preset[0] != '\0');
    bool overExport = custom_controls_enabled && can_export_preset && point_in(ui_mx, ui_my, exportBtn);
    SDL_Color export_bg = (overExport ? g_button_hover : g_button_base);
    if (!custom_controls_enabled || !can_export_preset)
        export_bg.a = 180;
    draw_rect(R, exportBtn, export_bg);
    draw_frame(R, exportBtn, g_button_border);
    SDL_Color export_fg = g_button_text;
    if (!custom_controls_enabled || !can_export_preset)
        export_fg.a = 180;
    draw_io_arrow_icon(R, exportBtn, true, export_fg);

    if (overExport)
    {
        const char *tip = "Export Preset";
        int tw = 0, th = 0;
        measure_text(tip, &tw, &th);
        int tx = ui_mx + 10;
        int ty = ui_my - 25;
        int tipw = tw + 8;
        int tiph = th + 8;
        if (tx + tipw > WINDOW_W - 4) tx = WINDOW_W - tipw - 4;
        if (ty < 4) ty = ui_my + 25;
        ui_set_tooltip((Rect){tx, ty, tipw, tiph}, tip, &g_reverb_tooltip_visible, &g_reverb_tooltip_rect, g_reverb_tooltip_text, sizeof(g_reverb_tooltip_text));
    }

    if (overImport && ui_mclick)
    {
        char *path = open_neoreverb_dialog();
        if (path)
        {
            char imported[64] = {0};
            if (import_custom_reverb_neoreverb(path, imported, sizeof(imported)))
            {
                // Select the imported preset in the dropdown
                int preset_list_idx = get_custom_reverb_preset_index(imported);
                if (preset_list_idx >= 0)
                {
                    load_custom_reverb_preset(imported);
                    extern int g_custom_reverb_preset_count;
                    int base_count2 = get_reverb_count() - g_custom_reverb_preset_count;
                    reverbType = base_count2 + preset_list_idx + 1;
                }
                set_status_message("Imported .neoreverb preset");
            }
            else
            {
                set_status_message("Failed to import .neoreverb or .neoreverb.xml");
            }
            free(path);
        }
        g_reverbDropdownOpen = false;
    }

    if (overExport && ui_mclick && can_export_preset)
    {
        // Suggest filename: <name>.neoreverb
        char default_name[96];
        default_name[0] = '\0';
        {
            char safe[64];
            safe_strncpy(safe, g_current_custom_reverb_preset, sizeof(safe));
            for (size_t i = 0; safe[i]; i++)
            {
                char c = safe[i];
                if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c == '\'')
                    safe[i] = '_';
            }
            snprintf(default_name, sizeof(default_name), "%s.neoreverb", safe);
        }

        char *path = save_neoreverb_dialog(default_name);
        if (path)
        {
            if (export_custom_reverb_neoreverb(g_current_custom_reverb_preset, path))
                set_status_message("Exported .neoreverb preset");
            else
                set_status_message("Failed to export .neoreverb or .neoreverb.xml");
            free(path);
        }
        g_reverbDropdownOpen = false;
    }
}
#endif

// Volume control (aligned with Tempo)
draw_text(R, 687, 85, "Volume:", labelCol);
// Allow volume slider interaction when reverb dropdown is closed. We want
// users to adjust master volume even while external MIDI input is active.
// Disable volume interaction when a modal/dialog is open
bool volume_enabled = !g_reverbDropdownOpen && !modal_block;
ui_slider(R, (Rect){687, 103, ddRect.w, 14}, &volume, 0, NEW_MAX_VOLUME_PCT,
          volume_enabled ? ui_mx : -1, volume_enabled ? ui_my : -1,
          volume_enabled ? ui_mdown : false, volume_enabled ? ui_mclick : false);
char vbuf[32];
snprintf(vbuf, sizeof(vbuf), "%d%%", volume);
int vtxt_x = ddRect.x + ddRect.w + 3;
int vtxt_y = 101;
draw_text(R, vtxt_x, vtxt_y, vbuf, labelCol);
/* Volume value is now non-interactive; clicking the percent label no longer resets to 100% */

// EQ Button
Rect eqBtn = {687, 123, 60, 20};
bool eq_button_enabled = !g_reverbDropdownOpen && !modal_block;
bool overEQ = eq_button_enabled && point_in(ui_mx, ui_my, eqBtn);
SDL_Color eq_btn_bg = overEQ ? g_button_hover : g_button_base;
if (!eq_button_enabled)
{
    eq_btn_bg.a = 180;
}
draw_rect(R, eqBtn, eq_btn_bg);
draw_frame(R, eqBtn, g_button_border);
SDL_Color eq_txt_col = g_button_text;
if (!eq_button_enabled)
{
    eq_txt_col.a = 180;
}
draw_text(R, eqBtn.x + 10, eqBtn.y + 3, "EQ...", eq_txt_col);
if (overEQ && ui_mclick)
{
    g_show_eq_dialog = true;
}

#if SUPPORT_MIDI_HW == TRUE
// If MIDI input is enabled, paint a semi-transparent overlay over the control panel to dim it
if (g_midi_input_enabled)
{
    SDL_Color dim = g_is_dark_mode ? (SDL_Color){0, 0, 0, 160} : (SDL_Color){255, 255, 255, 160};
    draw_rect(R, controlPanel, dim);
    // Redraw Reverb controls on top so they remain active/visible
    draw_rect(R, ddRect, dd_bg);
    draw_frame(R, ddRect, dd_frame);
    draw_text(R, ddRect.x + 6, ddRect.y + 3, cur, dd_txt);
    draw_text(R, ddRect.x + ddRect.w - 16, ddRect.y + 3, g_reverbDropdownOpen ? "^" : "v", dd_txt);
    // Also redraw Volume controls on top so they remain active/visible
    draw_text(R, 687, 85, "Volume:", labelCol);
    ui_slider(R, (Rect){687, 103, ddRect.w, 14}, &volume, 0, NEW_MAX_VOLUME_PCT,
              volume_enabled ? ui_mx : -1, volume_enabled ? ui_my : -1,
              volume_enabled ? ui_mdown : false, volume_enabled ? ui_mclick : false);
    draw_text(R, vtxt_x, vtxt_y, vbuf, labelCol);
    // Also redraw EQ button on top so it remains active/visible
    draw_rect(R, eqBtn, eq_btn_bg);
    draw_frame(R, eqBtn, g_button_border);
    draw_text(R, eqBtn.x + 10, eqBtn.y + 3, "EQ...", eq_txt_col);
    // Draw the external MIDI notice in the bottom-right of the control panel
    const char *notice = "External MIDI Input Enabled";
    int n_w = 0, n_h = 0;
    measure_text(notice, &n_w, &n_h);
    int n_x = controlPanel.x + controlPanel.w - n_w - 8;
    int n_y = controlPanel.y + controlPanel.h - n_h - 6;
    draw_text(R, n_x, n_y, notice, g_highlight_color);
    // Ensure the "Reverb:" label itself is also drawn above the dim layer
    draw_text(R, 687, 20, "Reverb:", labelCol);
}
#endif
// If playing an audio file (sound, not song), dim the control panel except volume-related controls
if (g_bae.is_audio_file && g_bae.sound)
{
    SDL_Color dim = g_is_dark_mode ? (SDL_Color){0, 0, 0, 160} : (SDL_Color){255, 255, 255, 160};
    draw_rect(R, controlPanel, dim);
    // Only redraw Volume controls on top so they remain active/visible
    // (Reverb is disabled when playing audio files)
    // Respect modal_block here so volume cannot be adjusted while a dialog is open
    bool audio_volume_enabled = !g_reverbDropdownOpen && !modal_block; // Volume should work when playing audio files
    draw_text(R, 687, 85, "Volume:", labelCol);
    ui_slider(R, (Rect){687, 103, ddRect.w, 14}, &volume, 0, NEW_MAX_VOLUME_PCT,
              audio_volume_enabled ? ui_mx : -1, audio_volume_enabled ? ui_my : -1,
              audio_volume_enabled ? ui_mdown : false, audio_volume_enabled ? ui_mclick : false);
    draw_text(R, vtxt_x, vtxt_y, vbuf, labelCol);
    // Also redraw EQ button on top so it remains active/visible
    draw_rect(R, eqBtn, eq_btn_bg);
    draw_frame(R, eqBtn, g_button_border);
    draw_text(R, eqBtn.x + 10, eqBtn.y + 3, "EQ...", eq_txt_col);
    // Draw a notice in the bottom-right of the control panel
    const char *notice = "Audio File";
    int n_w = 0, n_h = 0;
    measure_text(notice, &n_w, &n_h);
    int n_x = controlPanel.x + controlPanel.w - n_w - 8;
    int n_y = controlPanel.y + controlPanel.h - n_h - 6;
    draw_text(R, n_x, n_y, notice, g_highlight_color);
}

    *ctx->playing = playing;
    *ctx->progress = progress;
    *ctx->duration = duration;
    *ctx->transpose = transpose;
    *ctx->tempo = tempo;
    *ctx->volume = volume;
    *ctx->reverb_type = reverbType;
    *ctx->loop_enabled = loopPlay;
    *ctx->last_drag_progress = last_drag_progress;
    ctx->ui_mclick = ui_mclick;
    ctx->ui_rclick = ui_rclick;
    ctx->ui_mdown = ui_mdown;
    if (ctx->mclick)
        *ctx->mclick = mclick;

}
