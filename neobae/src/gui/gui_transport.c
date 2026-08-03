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

#include "gui_transport.h"

/* Session / WebTV progress state (was static in gui_main.c). */
static int g_total_play_ms = 0;
int g_last_engine_pos_ms = 0;
static uint32_t g_last_play_tick_ms = 0;
static uint32_t g_progress_display_tick_ms = 0;
static int g_progress_stripe_offset = 0;
static const int g_progress_stripe_width = 28;

void gui_transport_reset_session_timer(void)
{
    g_total_play_ms = 0;
    g_last_engine_pos_ms = 0;
    g_last_play_tick_ms = 0;
    g_progress_display_tick_ms = 0;
}

void gui_transport_update_progress(bool playing, int *progress, int *duration, int tempo)
{
    Uint32 now = SDL_GetTicks();
    if (playing)
    {
        const int engine_progress_ms = bae_get_pos_ms();
        *duration = bae_get_len_ms();

        if (g_progress_display_tick_ms == 0)
        {
            *progress = engine_progress_ms;
            g_progress_display_tick_ms = now;
        }
        else
        {
            uint32_t step_ms = now - g_progress_display_tick_ms;
            if (step_ms > 250)
                step_ms = 250;
            *progress += (int)((double)step_ms * ((double)tempo / 100.0));
            g_progress_display_tick_ms = now;

            if (abs(engine_progress_ms - *progress) > 120 || engine_progress_ms < (g_last_engine_pos_ms - 50))
                *progress = engine_progress_ms;
        }

        if (*progress < 0)
            *progress = 0;
        if (*duration > 0 && *progress > *duration)
            *progress = *duration;

        g_last_engine_pos_ms = engine_progress_ms;
    }
    else
    {
        g_progress_display_tick_ms = 0;
    }
}

void gui_transport_draw(GuiFrameCtx *ctx)
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
    bool modal_block_transport = ctx->modal_block_transport;
    bool playing = *ctx->playing;
    int progress = *ctx->progress;
    int duration = *ctx->duration;
    int transpose = *ctx->transpose;
    int tempo = *ctx->tempo;
    int volume = *ctx->volume;
    int reverbType = *ctx->reverb_type;
    bool loopPlay = *ctx->loop_enabled;
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

// Transport panel
draw_rect(R, transportPanel, panelBg);
draw_frame(R, transportPanel, panelBorder);
draw_text(R, 20, 170, "TRANSPORT & PROGRESS", headerCol);

// Progress bar with better styling
Rect bar = {20, 190, 650, 20};
#ifdef _WIN32
draw_rect(R, bar, g_theme.is_dark_mode ? (SDL_Color){25, 25, 30, 255} : (SDL_Color){240, 240, 240, 255});
#else
draw_rect(R, bar, (SDL_Color){25, 25, 30, 255});
#endif
draw_frame(R, bar, panelBorder);
if (duration != bae_get_len_ms())
    duration = bae_get_len_ms();
float pct = (duration > 0) ? (float)progress / duration : 0.f;
if (pct < 0)
    pct = 0;
if (pct > 1)
    pct = 1;
if (pct > 0)
{
    // Animated striped progress fill using accent color
    int fillW = (int)((bar.w - 4) * pct);
    Rect fillRect = {bar.x + 2, bar.y + 2, fillW, bar.h - 4};
    // Background for fill area (darker strip base)
    SDL_Color accent_dark = g_accent_color;
    int tr = (int)accent_dark.r - 36;
    if (tr < 0)
        tr = 0;
    accent_dark.r = (Uint8)tr;
    int tg = (int)accent_dark.g - 36;
    if (tg < 0)
        tg = 0;
    accent_dark.g = (Uint8)tg;
    int tb = (int)accent_dark.b - 36;
    if (tb < 0)
        tb = 0;
    accent_dark.b = (Uint8)tb;
    if (g_disable_webtv_progress_bar)
    {
        // Simple solid accent fill when WebTV style is disabled
        draw_rect(R, fillRect, g_accent_color);
    }
    else
    {
        draw_rect(R, fillRect, accent_dark);

        // Clip drawing to the fill area so stripes don't bleed outside
        SDL_Rect clip = {fillRect.x, fillRect.y, fillRect.w, fillRect.h};
#if USE_SDL2 == TRUE
        SDL_RenderSetClipRect(R, &clip);
#else
        SDL_SetRenderClipRect(R, &clip);
#endif

        // Draw diagonal stripes (leaning down-right) with equal dark/light band sizes.
        SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(R, g_accent_color.r, g_accent_color.g, g_accent_color.b, 220);
        int bandW = (g_progress_stripe_width / 2) + 4; // light band width (widened)
        if (bandW < 6)
            bandW = 6;
        int stripeStep = bandW * 2; // dark+light equal
        int thickness = 18;         // make the light band visibly wider
        int off = g_progress_stripe_offset % stripeStep;
        // Draw slanted bands by drawing multiple parallel lines per band
        // Draw slanted bands leaning up-left (reverse of previous)
        for (int sx = -fillRect.h - bandW - off; sx < fillRect.w + fillRect.h; sx += stripeStep)
        {
            int x0 = fillRect.x + sx;
            int x1 = x0 + bandW;
            for (int t = 0; t < thickness; ++t)
            {
                // Draw from bottom to top so the slant opposes previous direction
#if USE_SDL2 == TRUE
                SDL_RenderDrawLine(R, x0 + t, fillRect.y + fillRect.h, x1 + t, fillRect.y);
#else
                SDL_RenderLine(R, x0 + t, fillRect.y + fillRect.h, x1 + t, fillRect.y);
#endif
            }
        }

        // Restore clip
#if USE_SDL2 == TRUE
        SDL_RenderSetClipRect(R, NULL);
#else
        SDL_SetRenderClipRect(R, NULL);
#endif
        // Advance stripe animation based on elapsed time, not frame count,
        // so speed is consistent regardless of compiler or frame rate.
        static Uint32 g_progress_last_advance_ms = 0;
        const Uint32 stripe_advance_interval_ms = 50; // ms per 1-pixel step
        Uint32 now_stripe = SDL_GetTicks();
        if (now_stripe - g_progress_last_advance_ms >= stripe_advance_interval_ms)
        {
            g_progress_last_advance_ms = now_stripe;
            // Reverse scrolling direction by subtracting a single unit
            g_progress_stripe_offset = (g_progress_stripe_offset - 1) % (stripeStep);
            if (g_progress_stripe_offset < 0)
                g_progress_stripe_offset += stripeStep;
        }
    }
}
// Disable seekbar interaction while the reverb dropdown is expanded.
bool seekbar_enabled = !g_reverbDropdownOpen && !modal_block_transport;
    #if SUPPORT_MIDI_HW == TRUE
seekbar_enabled = seekbar_enabled && !g_midi_input_enabled;
    #endif
if (seekbar_enabled && ui_mdown && point_in(ui_mx, ui_my, bar))
{
    int rel = ui_mx - bar.x;
    if (rel < 0)
        rel = 0;
    if (rel > bar.w)
        rel = bar.w;
    int new_progress = (int)((double)rel / bar.w * duration);
    if (new_progress != last_drag_progress)
    {
        progress = new_progress;
        last_drag_progress = new_progress;
        bae_seek_ms(progress);
        g_last_engine_pos_ms = progress;
        // When the user seeks, ensure any sounding notes on the virtual
        // keyboard are silenced and UI-held state cleared so keys don't
        // remain lit for the wrong position.
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
    }
}
else
{
    // Reset when not dragging
    last_drag_progress = -1;
}

// Time display with fixed milliseconds for both current and total.
int prog_ms = progress % 1000;
int prog_sec = (progress / 1000) % 60;
int prog_min = (progress / 1000) / 60;
char pbuf[64];
snprintf(pbuf, sizeof(pbuf), "%02d:%02d.%03d", prog_min, prog_sec, prog_ms);
int dur_ms = duration % 1000;
int dur_sec = (duration / 1000) % 60;
int dur_min = (duration / 1000) / 60;
char dbuf[64];
snprintf(dbuf, sizeof(dbuf), "%02d:%02d.%03d", dur_min, dur_sec, dur_ms);

int pbuf_w = 0, pbuf_h = 0;
int dbuf_w = 0, dbuf_h = 0;
measure_text("00:00.000", &pbuf_w, &pbuf_h);
measure_text("00:00.000", &dbuf_w, &dbuf_h);
int time_y = 191; // moved up 3px from 194
int pbuf_x = 680;
// Clickable region just around current time text
Rect progressRect = {pbuf_x, time_y, pbuf_w, pbuf_h > 0 ? pbuf_h : 16};
// Transport controls (Play/seek) are disabled when external MIDI input is active.
#if SUPPORT_MIDI_HW == TRUE
bool transport_enabled = !g_midi_input_enabled;
#else
bool transport_enabled = true;
#endif
bool progressInteract = !g_reverbDropdownOpen && transport_enabled && !modal_block_transport;
bool progressHover = progressInteract && point_in(ui_mx, ui_my, progressRect);
if (progressInteract && progressHover && ui_mclick)
{
    progress = 0;
    bae_seek_ms(0);
    g_last_engine_pos_ms = progress;
    // Clear any sounding virtual keyboard notes on user-initiated seek
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
}
SDL_Color progressColor = progressHover ? g_highlight_color : labelCol;
draw_text(R, pbuf_x, time_y, pbuf, progressColor);
int slash_x = pbuf_x + pbuf_w + 6; // gap
draw_text(R, slash_x, time_y, "/", labelCol);
draw_text(R, slash_x + 10, time_y, dbuf, labelCol);

// Update session total-played time using wall-clock deltas while
// playing so loop-point timeline jumps do not inflate totals.
if (playing)
{
    uint32_t now_ms = SDL_GetTicks();
    if (g_last_play_tick_ms == 0)
    {
        g_last_play_tick_ms = now_ms;
    }
    else
    {
        uint32_t delta_ms = now_ms - g_last_play_tick_ms;
        // Ignore implausible spikes (e.g. debugger pause) to keep UI sane.
        if (delta_ms < (uint32_t)(5 * 60 * 1000))
        {
            g_total_play_ms += (int)delta_ms;
        }
        g_last_play_tick_ms = now_ms;
    }
    g_last_engine_pos_ms = bae_get_pos_ms();
}
else if (!playing)
{
    // Reset play tick while paused so we don't count pause time.
    g_last_play_tick_ms = 0;
    g_last_engine_pos_ms = bae_get_pos_ms();
}

// Draw total-played session timer below the progress time
int total_ms = g_total_play_ms;
int t_ms = total_ms % 1000;
int t_sec = (total_ms / 1000) % 60;
int t_min = (total_ms / 1000) / 60;
char total_time_buf[64];
snprintf(total_time_buf, sizeof(total_time_buf), "%02d:%02d.%03d", t_min, t_sec, t_ms);
int total_w = 0, total_h = 0;
measure_text(total_time_buf, &total_w, &total_h);
draw_text(R, pbuf_x, time_y + 18, total_time_buf, labelCol);

// Transport buttons
if (!transport_enabled)
{
    // Draw disabled Play button (no interaction)
    Rect playRect = {20, 215, 60, 22};
    SDL_Color disabledBg = g_panel_bg;
    SDL_Color disabledTxt = g_panel_border;
    draw_rect(R, playRect, disabledBg);
    draw_frame(R, playRect, g_panel_border);
    draw_text(R, playRect.x + 6, playRect.y + 4, playing ? "Pause" : "Play", disabledTxt);
}
else
{
    if (ui_button(R, (Rect){20, 215, 60, 22}, playing ? "Pause" : "Play", transport_mx, transport_my, transport_mdown) && transport_mclick && !modal_block_transport)
    {
        if (bae_play(&playing))
        {
            // If the play call resulted in a pause (playing==false), clear visible notes on the virtual keyboard
            if (!playing)
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
        }
    }
}
// Stop remains active even when transport is dimmed for MIDI input mode
if (ui_button(R, (Rect){90, 215, 60, 22}, "Stop", transport_mx, transport_my, transport_mdown) && transport_mclick && !modal_block_transport)
{
    bae_stop(&playing, &progress);
#if SUPPORT_MIDI_HW == TRUE
    // Ensure engine releases any held notes when user stops playback (panic)
    midi_output_send_all_notes_off(); // silence any external device too
#endif
    if (g_bae.song)
    {
        gui_panic_all_notes(g_bae.song);
    }
    if (g_live_song)
    {
        gui_panic_all_notes(g_live_song);
    }
    // Also ensure the virtual keyboard UI and per-channel note state are cleared
    // immediately after the engine AllNotesOff so the UI can't show lingering notes.
    if (g_show_virtual_keyboard)
    {
        BAESong target = g_bae.song ? g_bae.song : g_live_song;
        if (target)
        {
            // Send NoteOff for every channel/note to be extra-safe
            for (int ch = 0; ch < BAE_MAX_MIDI_CHANNELS; ++ch)
            {
                for (int n = 0; n < BAE_MAX_NOTES; ++n)
                {
                    BAESong_NoteOff(target, (unsigned char)ch, (unsigned char)n, 0, 0);
                }
            }
        }
        // Clear UI-held state so keys render as released immediately
        g_keyboard_mouse_note = -1;
        memset(g_keyboard_active_notes_by_channel, 0, sizeof(g_keyboard_active_notes_by_channel));
        memset(g_keyboard_active_notes, 0, sizeof(g_keyboard_active_notes));
        g_keyboard_suppress_until = SDL_GetTicks() + 33;
    }
    // Reset total-play timer on user Stop
    g_total_play_ms = 0;
    g_last_engine_pos_ms = 0;
    g_last_play_tick_ms = 0;
    g_progress_display_tick_ms = 0;
    // Clear visible virtual keyboard notes on Stop (use live song fallback)
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
    // Also stop export if active
    if (g_exporting)
    {
        bae_stop_wav_export();
    }
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
