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
#if SUPPORT_BAESCRIPT == TRUE
#include "gui_script_editor.h"
#endif
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

#include "gui_status_bank.h"

#define VU_SCOPE_BUF_MAX 2048
static float g_vu_left_level = 0.0f;
static float g_vu_right_level = 0.0f;
static int g_vu_peak_left = 0;
static int g_vu_peak_right = 0;
static Uint32 g_vu_peak_hold_until = 0;
static float g_vu_gain = 6.0f;
static bool g_vu_waveform_mode = false;
static float g_vu_fps_estimate = 60.0f;
static Uint32 g_vu_last_frame_tick = 0;
static int16_t g_scope_buf_l[VU_SCOPE_BUF_MAX];
static int16_t g_scope_buf_r[VU_SCOPE_BUF_MAX];
static int g_scope_buf_count = 0;
static const float MAIN_VU_ALPHA = 0.12f;

void gui_status_bank_draw(GuiFrameCtx *ctx)
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

// Status panel
draw_rect(R, statusPanel, panelBg);
draw_frame(R, statusPanel, panelBorder);
int statusBaseY = statusPanel.y + 10;
draw_text(R, 20, statusBaseY, "STATUS & BANK", headerCol);
/* Bank flavor chips sit next to the panel title (not the bank name). */
if (g_bae.bank_loaded)
{
    const char *chips[4];
    SDL_Color chipFills[4];
    SDL_Color chipTexts[4];
    int chipCount = 0;
    int has_mobile_chip = 0;
    int rmi_embed = g_bae.has_rmi_embedded_soundbank ? 1 : 0;
    const char *bank_ext = strrchr(g_current_bank_path, '.');
    int is_hsb = (strcmp(g_current_bank_path, "__builtin__") == 0);
    int is_zsb = 0;
#if USE_NATIVE_DLS == TRUE
    int has_xmf = (!rmi_embed && g_bae.mixer &&
                   BAEMixer_HasXMFDLSOverlayBank(g_bae.mixer)) ? 1 : 0;
#else
    int has_xmf = 0;
#endif
    if (bank_ext)
    {
#ifdef _WIN32
        is_hsb = is_hsb || (_stricmp(bank_ext, ".hsb") == 0);
        is_zsb = (_stricmp(bank_ext, ".zsb") == 0);
#else
        is_hsb = is_hsb || (strcasecmp(bank_ext, ".hsb") == 0);
        is_zsb = (strcasecmp(bank_ext, ".zsb") == 0);
#endif
    }
    /* RMI embed replaces the host bank — classify from mixer only.
     * Prefer DLS over SF2 (RMI DLS load may leave a prior SF2 flag). */
    if (rmi_embed)
    {
#if USE_NATIVE_DLS == TRUE
        if (g_bae.mixer && GM_GetMixerDLSMode())
        {
            extern bool g_use_dls_compatiblity_mode;
            int has_mobile = BAEMixer_HasMobileBAEMainBank(g_bae.mixer) ? 1 : 0;
            int has_eggs = BAEMixer_HasEggsDLSBank(g_bae.mixer) ? 1 : 0;
            int dls_level = BAEMixer_GetDLSBankLevel(g_bae.mixer);
            const char *neobae_dls_chip = (dls_level == 2) ? "NeoBAE DLS 2" : "NeoBAE DLS 1";
            if (has_eggs)
            {
                chips[chipCount] = "microQ";
                chipFills[chipCount] = g_is_dark_mode
                    ? (SDL_Color){40, 40, 40, 220}
                    : (SDL_Color){230, 230, 230, 230};
                chipTexts[chipCount] = g_is_dark_mode
                    ? (SDL_Color){235, 235, 235, 255}
                    : (SDL_Color){25, 25, 25, 255};
                chipCount++;
            }
            else if (has_mobile)
            {
                chips[chipCount] = "mobileBAE";
                chipFills[chipCount] = g_is_dark_mode
                    ? (SDL_Color){30, 70, 95, 220}
                    : (SDL_Color){190, 225, 245, 230};
                chipTexts[chipCount] = g_is_dark_mode
                    ? (SDL_Color){160, 220, 255, 255}
                    : (SDL_Color){20, 70, 110, 255};
                chipCount++;
            }
            else
            {
                if (!g_use_dls_compatiblity_mode)
                {
                    chips[chipCount] = "mobileBAE";
                    chipFills[chipCount] = g_is_dark_mode
                        ? (SDL_Color){30, 70, 95, 220}
                        : (SDL_Color){190, 225, 245, 230};
                    chipTexts[chipCount] = g_is_dark_mode
                        ? (SDL_Color){160, 220, 255, 255}
                        : (SDL_Color){20, 70, 110, 255};
                    chipCount++;
                }
                chips[chipCount] = neobae_dls_chip;
                chipFills[chipCount] = g_is_dark_mode
                    ? (SDL_Color){85, 50, 25, 220}
                    : (SDL_Color){255, 205, 145, 230};
                chipTexts[chipCount] = g_is_dark_mode
                    ? (SDL_Color){255, 185, 110, 255}
                    : (SDL_Color){100, 50, 15, 255};
                chipCount++;
            }
        }
        else
#endif
#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
        if (GM_GetMixerSF2Mode())
        {
            chips[chipCount] = "FluidBAE";
            chipFills[chipCount] = g_is_dark_mode
                ? (SDL_Color){20, 70, 65, 220}
                : (SDL_Color){175, 235, 225, 230};
            chipTexts[chipCount] = g_is_dark_mode
                ? (SDL_Color){130, 235, 215, 255}
                : (SDL_Color){15, 90, 80, 255};
            chipCount++;
        }
#endif
        ; /* rmi_embed handled */
    }
    else if (is_hsb)
    {
        /* miniBAE — dark blue */
        chips[chipCount] = "miniBAE";
        chipFills[chipCount] = g_is_dark_mode
            ? (SDL_Color){20, 35, 85, 220}
            : (SDL_Color){30, 50, 120, 230};
        chipTexts[chipCount] = g_is_dark_mode
            ? (SDL_Color){150, 180, 255, 255}
            : (SDL_Color){220, 230, 255, 255};
        chipCount++;
    }
    else if (is_zsb)
    {
        /* NeoBAE — gold (former microQ colors) */
        chips[chipCount] = "NeoBAE";
        chipFills[chipCount] = g_is_dark_mode
            ? (SDL_Color){90, 70, 30, 220}
            : (SDL_Color){255, 230, 160, 230};
        chipTexts[chipCount] = g_is_dark_mode
            ? (SDL_Color){255, 220, 120, 255}
            : (SDL_Color){120, 80, 20, 255};
        chipCount++;
    }
#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
    else if (GM_GetMixerSF2Mode())
    {
        /* FluidBAE — teal */
        chips[chipCount] = "FluidBAE";
        chipFills[chipCount] = g_is_dark_mode
            ? (SDL_Color){20, 70, 65, 220}
            : (SDL_Color){175, 235, 225, 230};
        chipTexts[chipCount] = g_is_dark_mode
            ? (SDL_Color){130, 235, 215, 255}
            : (SDL_Color){15, 90, 80, 255};
        chipCount++;
    }
#endif
#if USE_NATIVE_DLS == TRUE
    else if (g_bae.mixer && GM_GetMixerDLSMode())
    {
        extern bool g_use_dls_compatiblity_mode;
        /* Main bank only — XMF overlay is always MobileBAE and would
         * otherwise steal the host chip. */
        int has_mobile = BAEMixer_HasMobileBAEMainBank(g_bae.mixer) ? 1 : 0;
        int has_eggs = BAEMixer_HasEggsDLSBank(g_bae.mixer) ? 1 : 0;
        int dls_level = BAEMixer_GetDLSBankLevel(g_bae.mixer);
        const char *neobae_dls_chip = (dls_level == 2) ? "NeoBAE DLS 2" : "NeoBAE DLS 1";
        if (has_eggs)
        {
            /* microQ — black & white (own lane; ignore compat). */
            chips[chipCount] = "microQ";
            chipFills[chipCount] = g_is_dark_mode
                ? (SDL_Color){40, 40, 40, 220}
                : (SDL_Color){230, 230, 230, 230};
            chipTexts[chipCount] = g_is_dark_mode
                ? (SDL_Color){235, 235, 235, 255}
                : (SDL_Color){25, 25, 25, 255};
            chipCount++;
        }
        else if (has_mobile)
        {
            chips[chipCount] = "mobileBAE";
            chipFills[chipCount] = g_is_dark_mode
                ? (SDL_Color){30, 70, 95, 220}
                : (SDL_Color){190, 225, 245, 230};
            chipTexts[chipCount] = g_is_dark_mode
                ? (SDL_Color){160, 220, 255, 255}
                : (SDL_Color){20, 70, 110, 255};
            chipCount++;
            has_mobile_chip = 1;
        }
        else
        {
            /* Generic DLS: quirks → mobileBAE + NeoBAE DLS #; compat → NeoBAE DLS # only. */
            if (!g_use_dls_compatiblity_mode)
            {
                chips[chipCount] = "mobileBAE";
                chipFills[chipCount] = g_is_dark_mode
                    ? (SDL_Color){30, 70, 95, 220}
                    : (SDL_Color){190, 225, 245, 230};
                chipTexts[chipCount] = g_is_dark_mode
                    ? (SDL_Color){160, 220, 255, 255}
                    : (SDL_Color){20, 70, 110, 255};
                chipCount++;
                has_mobile_chip = 1;
            }
            chips[chipCount] = neobae_dls_chip;
            chipFills[chipCount] = g_is_dark_mode
                ? (SDL_Color){85, 50, 25, 220}
                : (SDL_Color){255, 205, 145, 230};
            chipTexts[chipCount] = g_is_dark_mode
                ? (SDL_Color){255, 185, 110, 255}
                : (SDL_Color){100, 50, 15, 255};
            chipCount++;
        }
    }
#endif
    /* XMF/MXMF embedded DLS: add mobileBAE alongside the host chip.
     * Skip if the host bank already contributed a mobileBAE chip.
     * RMI embeds are not overlays — do not stack a second chip. */
    if (has_xmf && !has_mobile_chip && chipCount < 4)
    {
        chips[chipCount] = "mobileBAE";
        chipFills[chipCount] = g_is_dark_mode
            ? (SDL_Color){30, 70, 95, 220}
            : (SDL_Color){190, 225, 245, 230};
        chipTexts[chipCount] = g_is_dark_mode
            ? (SDL_Color){160, 220, 255, 255}
            : (SDL_Color){20, 70, 110, 255};
        chipCount++;
    }
    if (chipCount > 0)
    {
        int title_w = 0, title_h_unused = 0;
        measure_text("STATUS & BANK", &title_w, &title_h_unused);
        (void)title_h_unused;
        int chip_x = 20 + title_w + 12;
        for (int ci = 0; ci < chipCount; ci++)
        {
            int chip_w = 0, chip_h = 0;
            measure_text(chips[ci], &chip_w, &chip_h);
            int chip_y = statusBaseY - 1;
            Rect chipBg = {chip_x - 4, chip_y - 1, chip_w + 8, (chip_h > 0 ? chip_h : 12) + 2};
            draw_rect(R, chipBg, chipFills[ci]);
            draw_frame(R, chipBg, chipTexts[ci]);
            draw_text(R, chip_x, statusBaseY, chips[ci], chipTexts[ci]);
            chip_x += chipBg.w + 8;
        }
    }
}
int lineY1 = statusBaseY + 20;
int lineY2 = statusBaseY + 40;

// Current file
draw_text(R, 20, lineY1, "File:", labelCol);
if (g_bae.song_loaded)
{
    // Show just filename, not full path
    const char *fn = g_bae.loaded_path;
    const char *base = fn;
    for (const char *p = fn; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    draw_text(R, 60, lineY1, base, g_highlight_color);
    // Tooltip hover region approximate width (mono 8px * len) like bank tooltip
    int textLen = (int)strlen(base);
    if (textLen < 1)
        textLen = 1;
    int approxW = textLen * 8;
    if (approxW > 480)
        approxW = 480;
    Rect fileTextRect = {60, lineY1, approxW, 16};
    if (!g_keyboard_channel_dd_open && point_in(ui_mx, ui_my, fileTextRect))
    {
        // Use full path as tooltip; if path equals base then show clarifying label
        char tip[512];
        if (strcmp(base, fn) == 0)
        {
            snprintf(tip, sizeof(tip), "File: %s", fn);
        }
        else
        {
            snprintf(tip, sizeof(tip), "%s", fn);
        }
        int tipLen = (int)strlen(tip);
        if (tipLen > 0)
        {
            // Measure actual text width and height
            int text_w, text_h;
            measure_text(tip, &text_w, &text_h);

            int tw = text_w + 8; // 4px padding on each side
            int th = text_h + 8; // 4px padding top and bottom
            if (tw > 480)
                tw = 480; // Maximum width constraint

            int tx = mx + 12;
            int ty = my + 12;
            if (tx + tw > WINDOW_W - 4)
                tx = WINDOW_W - tw - 4;
            if (ty + th > g_window_h - 4)
                ty = g_window_h - th - 4;
            ui_set_tooltip((Rect){tx, ty, tw, th}, tip, &g_file_tooltip_visible, &g_file_tooltip_rect, g_file_tooltip_text, sizeof(g_file_tooltip_text));
        }
    }
    else
    {
        ui_clear_tooltip(&g_file_tooltip_visible);
    }
}
else
{
    // muted text for empty file
    SDL_Color muted = g_is_dark_mode ? (SDL_Color){150, 150, 150, 255} : (SDL_Color){120, 120, 120, 255};
    draw_text(R, 60, lineY1, "<none>", muted);
}

// Bank info with tooltip (friendly name shown, filename/path on hover)
draw_text(R, 20, lineY2, "Bank:", labelCol);
if (g_bae.bank_loaded)
{
    // g_bae.bank_name now contains the display name (friendly name or filename)
    draw_text(R, 60, lineY2, g_bae.bank_name, g_highlight_color);
    // Simple tooltip region (approx width based on char count * 8px mono font)
    int textLen = (int)strlen(g_bae.bank_name);
    int approxW = textLen * 8;
    if (approxW < 8)
        approxW = 8;
    if (approxW > 400)
        approxW = 400; // crude clamp
    Rect bankTextRect = {60, lineY2, approxW, 16};
    // Prepare deferred tooltip drawing at end of frame (post status text)
    if (!g_keyboard_channel_dd_open && point_in(ui_mx, ui_my, bankTextRect))
    {
        char tip[512];
        // Show the full path in tooltip (RMI embed: active bank, not host path)
        if (g_bae.has_rmi_embedded_soundbank)
        {
            const char *flavor = NULL;
#if USE_NATIVE_DLS == TRUE
            if (g_bae.mixer && GM_GetMixerDLSMode())
            {
                extern bool g_use_dls_compatiblity_mode;
                int tip_mobile = BAEMixer_HasMobileBAEMainBank(g_bae.mixer) ? 1 : 0;
                int tip_eggs = BAEMixer_HasEggsDLSBank(g_bae.mixer) ? 1 : 0;
                int tip_level = BAEMixer_GetDLSBankLevel(g_bae.mixer);
                if (tip_eggs)
                    flavor = "scrambled eggs / microQ";
                else if (tip_mobile)
                    flavor = "mobileBAE";
                else if (!g_use_dls_compatiblity_mode)
                    flavor = (tip_level == 2)
                        ? "mobileBAE + NeoBAE DLS 2"
                        : "mobileBAE + NeoBAE DLS 1";
                else
                    flavor = (tip_level == 2) ? "NeoBAE DLS 2" : "NeoBAE DLS 1";
            }
#endif
#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
            if (!flavor && GM_GetMixerSF2Mode())
                flavor = "FluidBAE";
#endif
            if (flavor)
                snprintf(tip, sizeof(tip), "RMI embedded bank  (%s)", flavor);
            else
                snprintf(tip, sizeof(tip), "RMI embedded bank");
        }
        else if (strcmp(g_current_bank_path, "__builtin__") == 0)
        {
            snprintf(tip, sizeof(tip), "Built-in patches");
        }
        else
        {
            const char *flavor = NULL;
            const char *tip_ext = strrchr(g_current_bank_path, '.');
            if (tip_ext)
            {
#ifdef _WIN32
                if (_stricmp(tip_ext, ".hsb") == 0)
                    flavor = "miniBAE";
                else if (_stricmp(tip_ext, ".zsb") == 0)
                    flavor = "NeoBAE";
#else
                if (strcasecmp(tip_ext, ".hsb") == 0)
                    flavor = "miniBAE";
                else if (strcasecmp(tip_ext, ".zsb") == 0)
                    flavor = "NeoBAE";
#endif
            }
#if USE_SF2_SUPPORT == TRUE && _USING_FLUIDLITE == TRUE
            if (!flavor && GM_GetMixerSF2Mode())
                flavor = "FluidBAE";
#endif
#if USE_NATIVE_DLS == TRUE
            if (!flavor && g_bae.mixer && GM_GetMixerDLSMode())
            {
                extern bool g_use_dls_compatiblity_mode;
                int tip_mobile = BAEMixer_HasMobileBAEMainBank(g_bae.mixer) ? 1 : 0;
                int tip_eggs = BAEMixer_HasEggsDLSBank(g_bae.mixer) ? 1 : 0;
                int tip_level = BAEMixer_GetDLSBankLevel(g_bae.mixer);
                const char *tip_neobae = (tip_level == 2) ? "NeoBAE DLS 2" : "NeoBAE DLS 1";
                if (tip_eggs)
                    flavor = "scrambled eggs / microQ";
                else if (tip_mobile)
                    flavor = "mobileBAE";
                else if (!g_use_dls_compatiblity_mode)
                    flavor = (tip_level == 2)
                        ? "mobileBAE + NeoBAE DLS 2"
                        : "mobileBAE + NeoBAE DLS 1";
                else
                    flavor = tip_neobae;
            }
            if (g_bae.mixer && BAEMixer_HasXMFDLSOverlayBank(g_bae.mixer))
            {
                if (flavor && strcmp(flavor, "mobileBAE") != 0 &&
                    strncmp(flavor, "mobileBAE", 9) != 0)
                {
                    static char tip_flavor_buf[96];
                    snprintf(tip_flavor_buf, sizeof(tip_flavor_buf), "%s + mobileBAE", flavor);
                    flavor = tip_flavor_buf;
                }
                else if (!flavor)
                {
                    flavor = "mobileBAE";
                }
            }
#endif
            if (flavor)
                snprintf(tip, sizeof(tip), "%s  (%s)", g_current_bank_path, flavor);
            else
                snprintf(tip, sizeof(tip), "%s", g_current_bank_path);
        }
        int tipLen = (int)strlen(tip);
        if (tipLen > 0)
        {
            // Measure actual text width and height
            int text_w, text_h;
            measure_text(tip, &text_w, &text_h);

            int tw = text_w + 8; // 4px padding on each side
            int th = text_h + 8; // 4px padding top and bottom
            if (tw > 450)
                tw = 450; // Maximum width constraint

            int tx = mx + 12;
            int ty = my + 12; // initial placement near cursor
            if (tx + tw > WINDOW_W - 4)
                tx = WINDOW_W - tw - 4;
            if (ty + th > g_window_h - 4)
                ty = g_window_h - th - 4;
            ui_set_tooltip((Rect){tx, ty, tw, th}, tip, &g_bank_tooltip_visible, &g_bank_tooltip_rect, g_bank_tooltip_text, sizeof(g_bank_tooltip_text));
        }
    }
    else
    {
        ui_clear_tooltip(&g_bank_tooltip_visible);
    }
}
else
{
    // Muted text: slightly darker in light mode for better contrast on pale panels
    SDL_Color muted = g_is_dark_mode ? (SDL_Color){150, 150, 150, 255} : (SDL_Color){80, 80, 80, 255};
    draw_text(R, 60, lineY2, "<none>", muted);
}

{
    int pad_local = 4;
    int btnH_local = 30;
    int metersW = 300; // leave room for buttons and labels at right
    int vuX = statusPanel.x + statusPanel.w - metersW - 20;
    if (metersW < 40)
        metersW = 40;
    int meterH = 12; // each meter height
    int spacing = 6;
    int vuY = statusPanel.y + statusPanel.h - pad_local - btnH_local - 12 - (meterH + spacing) * 2 + 3;
    Rect vuToggleRect = {vuX, vuY - 10, metersW, meterH * 2 + spacing + 20};

    {
        Uint32 nowTick = SDL_GetTicks();
        if (g_vu_last_frame_tick != 0 && nowTick > g_vu_last_frame_tick)
        {
            float dt_ms = (float)(nowTick - g_vu_last_frame_tick);
            float instFps = 1000.0f / dt_ms;
            // Smooth FPS estimate for stable waveform window sizing.
            g_vu_fps_estimate = g_vu_fps_estimate * 0.90f + instFps * 0.10f;
        }
        g_vu_last_frame_tick = nowTick;
    }

    if (ui_mclick && !modal_block && point_in(ui_mx, ui_my, vuToggleRect))
    {
        g_vu_waveform_mode = !g_vu_waveform_mode;
    }

    // stacked meters (reuse existing sampling logic)
    // Avoid pulling frames from the mixer for VU sampling while we're
    // using the PCM WAV recorder (it consumes the same rendered frames).
    // Only call BAEMixer_GetAudioSampleFrame here when not exporting and
    // when the PCM writer is inactive to prevent draining audio.
#if SUPPORT_MIDI_HW == TRUE
    if (!g_exporting && !g_pcm_wav_recording && g_bae.mixer)
#else
    if (!g_exporting && g_bae.mixer)
#endif
    {
        static int16_t vuLeftBuf[16384];
        static int16_t vuRightBuf[16384];
        int16_t out = 0;
        if (BAEMixer_GetAudioSampleFrame(g_bae.mixer, vuLeftBuf, vuRightBuf, &out) == BAE_NO_ERROR)
        {
            const int bufCount = (int)(sizeof(vuLeftBuf) / sizeof(vuLeftBuf[0]));
            int sampleIndex = 0;
            if (out > 0 && out < (int16_t)bufCount)
            {
                sampleIndex = out / 2;
            }

            float rawL = 0.0f;
            float rawR = 0.0f;
            if (g_vu_waveform_mode)
            {
                // Oscilloscope mode: snapshot only the freshly-written samples
                // (index 0..sampleIndex) so the renderer can stretch them across
                // the full width without old zero-padding on the left.
                int snapCount = sampleIndex + 1;
                if (snapCount > VU_SCOPE_BUF_MAX) snapCount = VU_SCOPE_BUF_MAX;
                if (snapCount < 1) snapCount = 1;
                memcpy(g_scope_buf_l, vuLeftBuf, (size_t)snapCount * sizeof(int16_t));
                memcpy(g_scope_buf_r, vuRightBuf, (size_t)snapCount * sizeof(int16_t));
                g_scope_buf_count = snapCount;
                rawL = 0.0f;
                rawR = 0.0f;
            }
            else
            {
                // RMS mode: track loudness instead of waveform phase.
                const int rmsWindow = 512;
                float sumSqL = 0.0f;
                float sumSqR = 0.0f;
                for (int i = 0; i < rmsWindow; ++i)
                {
                    int idx = sampleIndex - i;
                    if (idx < 0)
                        idx += bufCount;

                    float nL = (float)vuLeftBuf[idx] / 32768.0f;
                    float nR = (float)vuRightBuf[idx] / 32768.0f;
                    sumSqL += nL * nL;
                    sumSqR += nR * nR;
                }
                float rmsL = sqrtf(sumSqL / (float)rmsWindow);
                float rmsR = sqrtf(sumSqR / (float)rmsWindow);
                rawL = rmsL * g_vu_gain;
                rawR = rmsR * g_vu_gain;
            }

            float fL = sqrtf(MIN(1.0f, rawL));
            float fR = sqrtf(MIN(1.0f, rawR));
            const float alpha = MAIN_VU_ALPHA;
            g_vu_left_level = g_vu_left_level * (1.0f - alpha) + fL * alpha;
            g_vu_right_level = g_vu_right_level * (1.0f - alpha) + fR * alpha;
            Uint32 now = SDL_GetTicks();
            int il = (int)(g_vu_left_level * 100.0f);
            int ir = (int)(g_vu_right_level * 100.0f);
            if (il > g_vu_peak_left)
            {
                g_vu_peak_left = il;
                g_vu_peak_hold_until = now + 600;
            }
            if (ir > g_vu_peak_right)
            {
                g_vu_peak_right = ir;
                g_vu_peak_hold_until = now + 600;
            }
            if (now > g_vu_peak_hold_until)
            {
                g_vu_peak_left = (int)(g_vu_left_level * 100.0f);
                g_vu_peak_right = (int)(g_vu_right_level * 100.0f);
            }
        }
    }
    else if (g_exporting)
    {
        const float decay = (1.0f - MAIN_VU_ALPHA); // keep exporting decay consistent with main smoothing (smoother)
        g_vu_left_level = g_vu_left_level * (1.0f - decay);
        g_vu_right_level = g_vu_right_level * (1.0f - decay);
        if (g_vu_left_level < 0.001f)
            g_vu_left_level = 0.0f;
        if (g_vu_right_level < 0.001f)
            g_vu_right_level = 0.0f;
    }

// Render stacked meters (same visuals as before)
#define METER_COLOR_FROM_LEVEL(v, outcol)  \
    do                                     \
    {                                      \
float _t = (v);                    \
if (_t < 0.f)                      \
    _t = 0.f;                      \
if (_t > 1.f)                      \
    _t = 1.f;                      \
SDL_Color _c;                      \
_c.a = 255;                        \
if (_t <= 0.6f)                    \
{                                  \
    float u = _t / 0.6f;           \
    _c.r = (Uint8)(0 + u * 255);   \
    _c.g = (Uint8)(200);           \
    _c.b = 0;                      \
}                                  \
else                               \
{                                  \
    float u = (_t - 0.6f) / 0.4f;  \
    _c.r = (Uint8)(255 - u * 55);  \
    _c.g = (Uint8)(200 - u * 160); \
    _c.b = 0;                      \
}                                  \
(outcol) = _c;                     \
    } while (0)
    SDL_Color trackBg = g_panel_bg;
    trackBg.a = 220;
    if (g_vu_waveform_mode)
    {
        // Waveform mode: one beveled stereo scope panel replaces L/R bars and labels.
        Rect scope = {vuX + 15, vuY - 13, metersW, meterH * 2 + spacing + 20};
        SDL_Color outerBg = g_bg_color;
        outerBg.a = 240;
        draw_rect(R, scope, outerBg);
        draw_frame(R, scope, g_panel_border);

        Rect inner = {scope.x + 2, scope.y + 2, scope.w - 4, scope.h - 4};
        SDL_Color innerBg = g_panel_bg;
        innerBg.a = 230;
        draw_rect(R, inner, innerBg);

        // Bevel effect (top/left highlight, bottom/right shadow)
        SDL_Color bevelHi = (SDL_Color){255, 255, 255, 70};
        SDL_Color bevelLo = (SDL_Color){0, 0, 0, 90};
        // Inset/pressed look: dark top-left, light bottom-right.
        draw_rect(R, (Rect){inner.x, inner.y, inner.w, 1}, bevelLo);
        draw_rect(R, (Rect){inner.x, inner.y, 1, inner.h}, bevelLo);
        draw_rect(R, (Rect){inner.x, inner.y + inner.h - 1, inner.w, 1}, bevelHi);
        draw_rect(R, (Rect){inner.x + inner.w - 1, inner.y, 1, inner.h}, bevelHi);

        int halfH = inner.h / 2;
        int midL = inner.y + halfH - (halfH / 8);
        int midR = inner.y + halfH + (halfH / 8);
        SDL_Color centerLine = g_panel_border;
        centerLine.a = 120;
        draw_rect(R, (Rect){inner.x + 1, midL, inner.w - 2, 1}, centerLine);
        draw_rect(R, (Rect){inner.x + 1, midR, inner.w - 2, 1}, centerLine);

        SDL_Color colL = g_highlight_color; // primary/intent
        SDL_Color colR = g_accent_color;    // secondary

        if (g_scope_buf_count > 0)
        {
            int drawW = inner.w - 2;
            // Map all g_scope_buf_count samples (oldest=left, newest=right) across drawW pixels.
            float samplesPerPixel = (float)g_scope_buf_count / (float)(drawW > 0 ? drawW : 1);

            int ampRange = (halfH / 2) - 1;
            if (ampRange < 1)
                ampRange = 1;

            for (int x = 0; x < drawW; ++x)
            {
                // Map pixel column directly to a sample bucket - no ring wrapping.
                int iStart = (int)((float)x * samplesPerPixel);
                int iEnd   = (int)((float)(x + 1) * samplesPerPixel) + 1;
                if (iEnd > g_scope_buf_count) iEnd = g_scope_buf_count;
                if (iStart >= iEnd) iStart = iEnd > 0 ? iEnd - 1 : 0;

                // Use one representative point per column to avoid solid min/max fills.
                float sumL = 0.0f;
                float sumR = 0.0f;
                int count = iEnd - iStart;
                if (count < 1)
                    count = 1;
                for (int s = iStart; s < iEnd; ++s)
                {
                    float sL = (float)g_scope_buf_l[s] / 32768.0f * g_vu_gain;
                    float sR = (float)g_scope_buf_r[s] / 32768.0f * g_vu_gain;
                    sumL += sL;
                    sumR += sR;
                }

                float avgL = sumL / (float)count;
                float avgR = sumR / (float)count;
                // Clamp to [-1, 1].
                if (avgL < -1.0f) avgL = -1.0f;
                if (avgL > 1.0f) avgL = 1.0f;
                if (avgR < -1.0f) avgR = -1.0f;
                if (avgR > 1.0f) avgR = 1.0f;

                // Positive sample → above center; negative → below.
                int yL = midL - (int)(avgL * (float)ampRange);
                int yR = midR - (int)(avgR * (float)ampRange);
                // Clamp to respective half-panels.
                if (yL < inner.y + 1)
                    yL = inner.y + 1;
                if (yL > inner.y + halfH - 1)
                    yL = inner.y + halfH - 1;
                if (yR < inner.y + halfH + 1)
                    yR = inner.y + halfH + 1;
                if (yR > inner.y + inner.h - 2)
                    yR = inner.y + inner.h - 2;

                int px = inner.x + 1 + x;
                SDL_SetRenderDrawColor(R, colL.r, colL.g, colL.b, 220);
#if USE_SDL2 == TRUE
                SDL_RenderDrawPoint(R, px, yL);
#else
                SDL_RenderPoint(R, px, yL);
#endif
                SDL_SetRenderDrawColor(R, colR.r, colR.g, colR.b, 210);
#if USE_SDL2 == TRUE
                SDL_RenderDrawPoint(R, px, yR);
#else
                SDL_RenderPoint(R, px, yR);
#endif
            }
        }
    }
    else
    {
        draw_rect(R, (Rect){vuX, vuY, metersW, meterH}, trackBg);
        draw_frame(R, (Rect){vuX, vuY, metersW, meterH}, g_panel_border);
        int leftFill = (int)(g_vu_left_level * (metersW - 6));
        if (leftFill < 0)
            leftFill = 0;
        if (leftFill > metersW - 6)
            leftFill = metersW - 6;
        // Draw a left-to-right gradient fill (green -> yellow -> red) for the left meter
        int innerX = vuX + 3;
        int innerW = metersW - 6;
        int innerY = vuY + 3;
        int innerH = meterH - 6;
        if (leftFill > 0)
        {
            for (int xoff = 0; xoff < leftFill; ++xoff)
            {
                float frac = (float)xoff / (float)(innerW > 0 ? innerW : 1); // 0..1 left->right
                SDL_Color col;
                if (frac < 0.5f)
                { // green -> yellow
                    float p = frac / 0.5f;
                    col.r = (Uint8)(g_highlight_color.r * p + 20 * (1.0f - p));
                    col.g = (Uint8)(200 * (1.0f - (1.0f - p) * 0.2f));
                    col.b = 20;
                }
                else
                { // yellow -> red
                    float p = (frac - 0.5f) / 0.5f;
                    col.r = (Uint8)(200 + (55 * p));
                    col.g = (Uint8)(200 * (1.0f - p));
                    col.b = 20;
                }
                SDL_SetRenderDrawColor(R, col.r, col.g, col.b, 255);
#if USE_SDL2 == TRUE
                SDL_RenderDrawLine(R, innerX + xoff, innerY, innerX + xoff, innerY + innerH - 1);
#else
                SDL_RenderLine(R, innerX + xoff, innerY, innerX + xoff, innerY + innerH - 1);
#endif
            }
        }
        int pL = vuX + 3 + (int)((g_vu_peak_left / 100.0f) * (metersW - 6));
        if (pL < vuX + 3)
            pL = vuX + 3;
        if (pL > vuX + 3 + metersW - 6)
            pL = vuX + 3 + metersW - 6;
        // Draw white-ish peak marker similar to per-channel meters
        draw_rect(R, (Rect){pL - 1, vuY + 1, 2, meterH - 2}, (SDL_Color){255, 255, 255, 200});
        int vuY2 = vuY + meterH + spacing;
        draw_rect(R, (Rect){vuX, vuY2, metersW, meterH}, trackBg);
        draw_frame(R, (Rect){vuX, vuY2, metersW, meterH}, g_panel_border);
        int rightFill = (int)(g_vu_right_level * (metersW - 6));
        if (rightFill < 0)
            rightFill = 0;
        if (rightFill > metersW - 6)
            rightFill = metersW - 6;
        // Right meter gradient (same mapping as left)
        int innerX2 = vuX + 3;
        int innerW2 = metersW - 6;
        int innerY2 = vuY2 + 3;
        int innerH2 = meterH - 6;
        if (rightFill > 0)
        {
            for (int xoff = 0; xoff < rightFill; ++xoff)
            {
                float frac = (float)xoff / (float)(innerW2 > 0 ? innerW2 : 1);
                SDL_Color col;
                if (frac < 0.5f)
                {
                    float p = frac / 0.5f;
                    col.r = (Uint8)(g_highlight_color.r * p + 20 * (1.0f - p));
                    col.g = (Uint8)(200 * (1.0f - (1.0f - p) * 0.2f));
                    col.b = 20;
                }
                else
                {
                    float p = (frac - 0.5f) / 0.5f;
                    col.r = (Uint8)(200 + (55 * p));
                    col.g = (Uint8)(200 * (1.0f - p));
                    col.b = 20;
                }
                SDL_SetRenderDrawColor(R, col.r, col.g, col.b, 255);
#if USE_SDL2 == TRUE
                SDL_RenderDrawLine(R, innerX2 + xoff, innerY2, innerX2 + xoff, innerY2 + innerH2 - 1);
#else
                SDL_RenderLine(R, innerX2 + xoff, innerY2, innerX2 + xoff, innerY2 + innerH2 - 1);
#endif
            }
        }
        int pR = vuX + 3 + (int)((g_vu_peak_right / 100.0f) * (metersW - 6));
        if (pR < vuX + 3)
            pR = vuX + 3;
        if (pR > vuX + 3 + metersW - 6)
            pR = vuX + 3 + metersW - 6;
        draw_rect(R, (Rect){pR - 1, vuY2 + 1, 2, meterH - 2}, (SDL_Color){255, 255, 255, 200});
        int labelX = vuX + metersW + 6;
        SDL_Color labelCol = g_text_color;
        draw_text(R, labelX, vuY - 3, "L", labelCol);
        draw_text(R, labelX, vuY2 - 3, "R", labelCol);
    }
#undef METER_COLOR_FROM_LEVEL
}

{
    int pad = 4; // panel-relative padding
    int btnW = 90;
    int btnH = 30;            // fixed size (uniform for Settings and bank buttons)
    int builtinW = btnW + 10; // make Default Bank button width appropriate for text
    // Anchor buttons to bottom-right corner of statusPanel
    int baseX = statusPanel.x + statusPanel.w - pad - btnW;
    int baseY = statusPanel.y + statusPanel.h - pad - btnH;
    // Settings button sits at baseX, baseY
    Rect settingsBtn = {baseX, baseY, btnW, btnH};
    // Spacing between buttons
    int gap = 8;
    // Default Bank immediately left of Settings (wider)
    Rect builtinBtn = {baseX - gap - builtinW, baseY, builtinW, btnH};
    // Load Bank to the left of Default Bank
    Rect loadBankBtn = {builtinBtn.x - gap - btnW, baseY, btnW, btnH};
    bool settingsEnabled = !g_reverbDropdownOpen;
    bool overSettings = settingsEnabled && point_in(ui_mx, ui_my, settingsBtn);
    SDL_Color sbg = settingsEnabled ? (overSettings ? g_button_hover : g_button_base) : g_button_base;
    if (!settingsEnabled)
    {
        sbg.a = 180;
    }
    if (g_show_settings_dialog)
        sbg = g_button_base;
    draw_rect(R, settingsBtn, sbg);
    draw_frame(R, settingsBtn, g_button_border);
    int tw = 0, th = 0;
    measure_text("Settings", &tw, &th);
    draw_text(R, settingsBtn.x + (settingsBtn.w - tw) / 2, settingsBtn.y + (settingsBtn.h - th) / 2, "Settings", g_button_text);
    if (settingsEnabled && !modal_block && ui_mclick && overSettings)
    {
        g_show_settings_dialog = !g_show_settings_dialog;
        if (g_show_settings_dialog)
        {
            g_volumeCurveDropdownOpen = false;
            g_show_rmf_info_dialog = false;
        }
    }

    // About button (left of Load Bank) - same size as settings
    Rect aboutBtn = {loadBankBtn.x - gap - btnW, baseY, btnW, btnH};
    draw_rect(R, aboutBtn, g_button_base);
    draw_frame(R, aboutBtn, g_button_border);
    int abtw = 0, abth = 0;
    measure_text("About", &abtw, &abth);
    draw_text(R, aboutBtn.x + (aboutBtn.w - abtw) / 2, aboutBtn.y + (aboutBtn.h - abth) / 2, "About", g_button_text);
    if (point_in(ui_mx, ui_my, aboutBtn) && ui_mclick && !modal_block)
    {
        g_show_about_dialog = !g_show_about_dialog;
        if (g_show_about_dialog)
        {
            g_show_settings_dialog = false;
            g_show_rmf_info_dialog = false;
            g_about_page = 0;
            g_about_credits_scroll = 0;
            g_about_wheel_delta = 0;
        }
    }

#if SUPPORT_BAESCRIPT == TRUE
    // Script button (left of About)
    Rect scriptBtn = {aboutBtn.x - gap - btnW, baseY, btnW, btnH};
    {
        bool overScript = point_in(ui_mx, ui_my, scriptBtn);
        SDL_Color scbg = overScript ? g_button_hover : g_button_base;
        if (script_editor_is_visible()) scbg = g_button_press;
        draw_rect(R, scriptBtn, scbg);
        draw_frame(R, scriptBtn, g_button_border);
        int sctw = 0, scth = 0;
        measure_text("Script", &sctw, &scth);
        draw_text(R, scriptBtn.x + (scriptBtn.w - sctw) / 2, scriptBtn.y + (scriptBtn.h - scth) / 2, "Script", g_button_text);
        if (overScript && ui_mclick && !modal_block)
        {
            script_editor_toggle();
        }
    }
#endif

    // Load Bank button (left of Settings). Label trimmed to "Load Bank" per request.
    if (ui_button(R, loadBankBtn, "Load Bank", ui_mx, ui_my, ui_mdown) && ui_mclick && !modal_block)
    {
        char fileBuf[1024] = {0};

#ifdef _WIN32
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter =
#if USE_SF2_SUPPORT == TRUE
    #if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0 && USE_NATIVE_DLS == TRUE
            "Bank Files (*.hsb;*.zsb;*.sf2;*.sf3;*.sfo;*.dls)\0*.hsb;*.zsb;*.sf2;*.sf3;*.sfo;*.dls\0HSB/ZSB Banks\0*.hsb;*.zsb\0SF2 SoundFonts\0*.sf2\0SF3 SoundFonts\0*.sf3\0SFO SoundFonts\0*.sfo\0DLS Banks\0*.dls\0All Files\0*.*\0"
    #else
#if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0
            "Bank Files (*.hsb;*.zsb;*.sf2;*.sf3;*.sfo)\0*.hsb;*.zsb;*.sf2;*.sf3;*.sfo\0HSB/ZSB Banks\0*.hsb;*.zsb\0SF2 SoundFonts\0*.sf2\0SF3 SoundFonts\0*.sf3\0SFO SoundFonts\0*.sfo\0All Files\0*.*\0"
#else
            "Bank Files (*.hsb;*.zsb;*.sf2)\0*.hsb;*.zsb;*.sf2\0HSB/ZSB Banks\0*.hsb;*.zsb\0SF2 SoundFonts\0*.sf2\0All Files\0*.*\0"
#endif
    #endif
#elif USE_NATIVE_DLS == TRUE
            "Bank Files (*.hsb;*.zsb;*.sf2;*.dls)\0*.hsb;*.zsb;*.dls\0HSB/ZSB Banks\0*.hsb;*.zsb\0DLS Banks\0*.dls\0All Files\0*.*\0"
#else
            "Bank Files (*.hsb;*.zsb)\0*.hsb;*.zsb\0HSB/ZSB Banks\0*.hsb;*.zsb\0All Files\0*.*\0"
#endif
            ;
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = sizeof(fileBuf);
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        ofn.lpstrDefExt = "hsb";
        if (GetOpenFileNameA(&ofn))
            load_bank(fileBuf, playing, transpose, tempo, volume, loopPlay, reverbType, ch_enable, true);
#elif defined(__APPLE__)
        {
            static const char BANK_TYPE_LIST[] =
                "\"hsb\", \"zsb\""
#if USE_SF2_SUPPORT == TRUE
                ", \"sf2\""
#if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0
                ", \"sf3\", \"sfo\""
#endif
#if _USING_FLUIDLITE == TRUE || USE_NATIVE_DLS == TRUE
                ", \"dls\""
#endif
#endif
                ;
            char cmd[1024];
            snprintf(cmd, sizeof(cmd),
                "osascript -e 'POSIX path of (choose file with prompt \"Load Patch Bank\" of type {%s})' 2>/dev/null",
                BANK_TYPE_LIST);
            FILE *fp = popen(cmd, "r");
            if (fp)
            {
                if (fgets(fileBuf, sizeof(fileBuf), fp))
                {
                    pclose(fp);
                    size_t l = strlen(fileBuf);
                    while (l > 0 && (fileBuf[l - 1] == '\n' || fileBuf[l - 1] == '\r'))
                        fileBuf[--l] = '\0';
                    if (l > 0)
                        load_bank(fileBuf, playing, transpose, tempo, volume, loopPlay, reverbType, ch_enable, true);
                }
                else
                    pclose(fp);
            }
        }
#else
const char *cmds[] = {
#if USE_SF2_SUPPORT == TRUE
    #if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0 && USE_NATIVE_DLS == TRUE
    "zenity --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb *.sf2 *.sf3 *.sfo *.dls' 2>/dev/null",
    "kdialog --getopenfilename . '*.hsb *.zsb *.sf2 *.sf3 *.sfo *.dls' 2>/dev/null",
    "yad --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb *.sf2 *.sf3 *.sfo *.dls' 2>/dev/null",
    #else
#if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0
    "zenity --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb *.sf2 *.sf3 *.sfo' 2>/dev/null",
    "kdialog --getopenfilename . '*.hsb *.zsb *.sf2 *.sf3 *.sfo' 2>/dev/null",
    "yad --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb *.sf2 *.sf3 *.sfo' 2>/dev/null",
#else
    "zenity --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb *.sf2' 2>/dev/null",
    "kdialog --getopenfilename . '*.hsb *.zsb *.sf2' 2>/dev/null",
    "yad --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb *.sf2' 2>/dev/null",
#endif
    #endif
#elif USE_NATIVE_DLS == TRUE
    "zenity --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb *.dls' 2>/dev/null",
    "kdialog --getopenfilename . '*.hsb *.zsb *.dls' 2>/dev/null",
    "yad --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb *.dls' 2>/dev/null",
#else
    "zenity --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb' 2>/dev/null",
    "kdialog --getopenfilename . '*.hsb *.zsb' 2>/dev/null",
    "yad --file-selection --title='Load Patch Bank' --file-filter='Bank Files | *.hsb *.zsb' 2>/dev/null",
#endif
    NULL};            
for (int ci = 0; cmds[ci]; ++ci)
{
    FILE *p = popen(cmds[ci], "r");
    if (!p)
        continue;
    if (fgets(fileBuf, sizeof(fileBuf), p))
    {
        pclose(p);
        size_t l = strlen(fileBuf);
        while (l > 0 && (fileBuf[l - 1] == '\n' || fileBuf[l - 1] == '\r'))
            fileBuf[--l] = '\0';
        if (l > 0)
        {
            if ((l > 4 && strcasecmp(fileBuf + l - 4, ".hsb") == 0)
                || (l > 4 && strcasecmp(fileBuf + l - 4, ".zsb") == 0)
#if USE_SF2_SUPPORT == TRUE
                || (l > 4 && strcasecmp(fileBuf + l - 4, ".sf2") == 0)
#if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0
                || (l > 4 && strcasecmp(fileBuf + l - 4, ".sf3") == 0)
                || (l > 4 && strcasecmp(fileBuf + l - 4, ".sfo") == 0)
#endif                        
#endif
#if _USING_FLUIDLITE == TRUE || USE_NATIVE_DLS == TRUE
                || (l > 4 && strcasecmp(fileBuf + l - 4, ".dls") == 0)
#endif

            )
            {
                load_bank(fileBuf, playing, transpose, tempo, volume, loopPlay, reverbType, ch_enable, true);
            }
            else
            {
#if USE_SF2_SUPPORT == TRUE
    #if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0 && USE_NATIVE_DLS == TRUE
                BAE_PRINTF("Not a bank file (.hsb, .zsb, .sf2, .sf3, .sfo, or .dls): %s\n", fileBuf);
    #else
#if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0                  
                BAE_PRINTF("Not a bank file (.hsb, .zsb, .sf2, .sf3, or .sfo): %s\n", fileBuf);
#else
                BAE_PRINTF("Not a bank file (.hsb, .zsb, or .sf2): %s\n", fileBuf);
#endif
    #endif
#elif USE_NATIVE_DLS == TRUE
                BAE_PRINTF("Not a bank file (.hsb, .zsb, or .dls): %s\n", fileBuf);
#else
                BAE_PRINTF("Not a bank file (.hsb or .zsb): %s\n", fileBuf);
#endif
            }
        }
        break;
    }
    pclose(p);
}
#endif
    }

    // Default Bank button (right of Load Bank)
    bool defaultBankExists = false;
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    // Check if we should show the Default Bank button

    // Check if patches.hsb or patches.zsb exists in executable directory

    char exe_dir[512];
    char patches_path[1024];
    get_executable_directory(exe_dir, sizeof(exe_dir));
    {
        static const char *default_names[] = {"patches.hsb", "patches.zsb", NULL};
        for (int di = 0; default_names[di]; ++di)
        {
#ifdef _WIN32
            snprintf(patches_path, sizeof(patches_path), "%s\\%s", exe_dir, default_names[di]);
#else
            snprintf(patches_path, sizeof(patches_path), "%s/%s", exe_dir, default_names[di]);
#endif
            FILE *test_file = fopen(patches_path, "r");
            if (test_file)
            {
                fclose(test_file);
                defaultBankExists = true;
                break;
            }
        }
    }

#endif

    bool default_loaded = false;
    bool builtin_loaded = false;
    if (defaultBankExists)
    {
        default_loaded = (g_current_bank_path[0] && strcmp(g_current_bank_path, patches_path) == 0);
    }
#if _BUILT_IN_PATCHES == TRUE
    builtin_loaded = (g_current_bank_path[0] && strcmp(g_current_bank_path, "__builtin__") == 0);
#endif

    bool builtinEnabled = !modal_block && !g_reverbDropdownOpen;
    bool overBuiltin = builtinEnabled && point_in(ui_mx, ui_my, builtinBtn);
    SDL_Color bbg = builtinEnabled ? (overBuiltin ? g_button_hover : g_button_base) : g_button_base;
    if (!builtinEnabled)
        bbg.a = 180;
    draw_rect(R, builtinBtn, bbg);
    draw_frame(R, builtinBtn, g_button_border);
    int btw = 0, bth = 0;
    measure_text("Default Bank", &btw, &bth);
    draw_text(R, builtinBtn.x + (builtinBtn.w - btw) / 2, builtinBtn.y + (builtinBtn.h - bth) / 2, "Default Bank", g_button_text);
    if (builtinEnabled && ui_mclick && overBuiltin)
    {
        if (defaultBankExists && !default_loaded)
        {
            if (!load_bank(patches_path, playing, transpose, tempo, volume, loopPlay, reverbType, ch_enable, true))
            {
                set_status_message("Failed to load default bank");
            }
#if _BUILT_IN_PATCHES == TRUE
        }
        else if (!builtin_loaded && !defaultBankExists)
        {
            if (!load_bank("__builtin__", playing, transpose, tempo, volume, loopPlay, reverbType, ch_enable, true))
            {
                set_status_message("Failed to load default bank");
            }
#endif
        }
    }
    // Right-click loads built-in bank (if available) regardless of default bank existence
#if _BUILT_IN_PATCHES == TRUE
    if (overBuiltin && ui_rclick && !builtin_loaded)
    {
        if (!load_bank("__builtin__", playing, transpose, tempo, volume, loopPlay, reverbType, ch_enable, true))
        {
            set_status_message("Failed to load built-in bank");
        }
    }
#endif
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
