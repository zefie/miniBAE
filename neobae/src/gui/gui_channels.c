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

#include "gui_channels.h"

int g_max_vu_channels = 16;

/* Time-based per-channel VU smoothing. Fixed per-frame alphas flicker at high
 * refresh rates because audio levels update much slower than the UI. Attack is
 * quicker so note-ons still feel responsive; release averages over a short
 * window for a stable meter. */
static const float CHANNEL_VU_ATTACK_MS = 30.0f;
static const float CHANNEL_VU_RELEASE_MS = 140.0f;
static const float CHANNEL_ACTIVITY_RELEASE_MS = 80.0f;
static Uint32 g_channel_vu_last_tick = 0;

static float channel_vu_ema_alpha(float dt_ms, float tau_ms)
{
    if (tau_ms <= 1.0f)
        return 1.0f;
    float a = 1.0f - expf(-dt_ms / tau_ms);
    if (a < 0.0f)
        return 0.0f;
    if (a > 1.0f)
        return 1.0f;
    return a;
}

void gui_channels_draw(GuiFrameCtx *ctx)
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

draw_rect(R, channelPanel, panelBg);
draw_frame(R, channelPanel, panelBorder);
draw_text(R, 20, 20, "MIDI CHANNELS", headerCol);
int chStartX = 20, chStartY = 40;
// Precompute estimated per-channel levels from mixer realtime info when available.
float realtime_channel_level[BAE_MAX_MIDI_CHANNELS];
for (int _i = 0; _i < BAE_MAX_MIDI_CHANNELS; ++_i)
    realtime_channel_level[_i] = 0.0f;
bool have_realtime_levels = false;
#if SUPPORT_MIDI_HW == TRUE
if (g_bae.mixer && !g_exporting && (playing || g_midi_input_enabled))
#else
if (g_bae.mixer && !g_exporting && playing)
#endif
{
    // Get realtime levels when playing files or when MIDI input is active
    // Prefer PCM-derived per-channel estimates when available from the engine.
    float chL[BAE_MAX_MIDI_CHANNELS], chR[BAE_MAX_MIDI_CHANNELS];
    GM_GetRealtimeChannelLevels(chL, chR);
    // Merge stereo channels into a single mono level per MIDI channel
    for (int ch = 0; ch < BAE_MAX_MIDI_CHANNELS; ++ch)
    {
        float lvl = (chL[ch] + chR[ch]) * 0.5f;
        realtime_channel_level[ch] = lvl;
    }
    // Always trust the engine's realtime levels when playing or MIDI input is active
    have_realtime_levels = true;
}

Uint32 nowTick = SDL_GetTicks();
float dt_ms = 16.0f;
if (g_channel_vu_last_tick != 0 && nowTick > g_channel_vu_last_tick)
{
    dt_ms = (float)(nowTick - g_channel_vu_last_tick);
    if (dt_ms < 1.0f)
        dt_ms = 1.0f;
    /* Cap after stalls so a hitch doesn't slam the meters to the new sample. */
    if (dt_ms > 100.0f)
        dt_ms = 100.0f;
}
g_channel_vu_last_tick = nowTick;

for (int i = 0; i < g_max_vu_channels; i++)
{
    int col = i % 8;
    int row = i / 8;
    Rect r = {chStartX + col * 45, chStartY + row * 35, 16, 16};
    // Move checkboxes/labels/VU for channels 9-16 (row == 1) down by 1 pixel
    if (row == 1)
    {
        r.y += 1;
    }
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", i + 1);
    // Handle toggle and clear VU when channel is muted
    // Disable channel toggles when playing audio files (sounds, not songs)
    // and while the reverb dropdown is expanded.
    bool channel_toggle_enabled = !(g_bae.is_audio_file && g_bae.sound) && !g_reverbDropdownOpen;
    bool toggled = ui_toggle(R, r, &ch_enable[i], NULL,
                             channel_toggle_enabled ? ui_mx : -1,
                             channel_toggle_enabled ? ui_my : -1,
                             ui_mclick && !modal_block && channel_toggle_enabled);
    if (toggled && !ch_enable[i])
    {
        // Muted -> immediately empty visible VU
        g_channel_vu[i] = 0.0f;
        // Clear virtual keyboard highlights for this channel
        gui_clear_virtual_keyboard_channel(i);
        // If MIDI-in is active, proactively send NoteOff for any active notes on this channel
        // to prevent stuck notes from live input. Use the current target song (loaded or live).
#if SUPPORT_MIDI_HW == TRUE
        if (g_midi_input_enabled)
        {
            BAESong target = g_bae.song ? g_bae.song : g_live_song;
            if (target)
            {
                // Send per-channel panic to engine
                gui_panic_channel_notes(target, i);
                // Also mirror NoteOff to external MIDI out if enabled
                if (g_midi_output_enabled)
                {
                    for (int n = 0; n < BAE_MAX_NOTES; ++n)
                    {
                        if (g_keyboard_active_notes_by_channel[i][n])
                        {
                            unsigned char msg[3] = {(unsigned char)(0x80 | (i & 0x0F)), (unsigned char)n, 0};
                            midi_output_send(msg, 3);
                        }
                    }
                }
            }
        }
#endif
    }
    else if (toggled && ch_enable[i])
    {
        // Unmuted -> refresh virtual keyboard state from current engine state
        // to avoid showing stale highlights from before the channel was muted
        gui_refresh_virtual_keyboard_channel_from_engine(i);
    }
    int tw = 0, th = 0;
    measure_text(buf, &tw, &th);
    // Reserve a few pixels to the right of checkbox for the VU meter so
    // the number doesn't visually collide with it. Center within checkbox width.
    int cx = r.x + (r.w - tw) / 2;
    int ty = r.y + r.h + 2; // label below box
    // If this is the second row (channels 9-16) we nudged the checkbox down;
    // apply the same 1px offset to the label so it stays aligned.
    if (r.y != chStartY + row * 35)
        ty += 0; // r.y already includes adjustment; keep explicit comment for clarity
    draw_text(R, cx, ty, buf, labelCol);

    // Draw a tiny vertical VU meter immediately to the right of the checkbox.
    // Height = checkbox height + gap (2) + number text height so it aligns with both.
    int meterW = 6; // narrow vertical meter
    int meterH = r.h + 2 + th;
    // Move 3px to the left from previous placement: use +5 instead of +8
    int meterX = r.x + r.w + 5; // slightly closer to checkbox
    int meterY = r.y;           // align top with checkbox
    Rect meterBg = {meterX, meterY, meterW, meterH};
    // Background / frame
    draw_rect(R, meterBg, g_panel_bg);
    draw_frame(R, meterBg, g_panel_border);

    // Prefer realtime estimated per-channel levels when available. Otherwise fall back to
    // the previous activity-driven heuristic (incoming MIDI or engine active notes).

    // Immediately clear VU meters when not playing, regardless of other state
    if (!playing || g_bae.is_audio_file)
    {
        g_channel_vu[i] = 0.0f;
        // Also clear peaks when stopped
        g_channel_peak_level[i] = 0.0f;
        g_channel_peak_hold_until[i] = 0;
    }
    else if (have_realtime_levels)
    {
        float lvl = realtime_channel_level[i];
        if (lvl < 0.f)
            lvl = 0.f;
        if (lvl > 1.f)
            lvl = 1.f;
        float tau = (lvl > g_channel_vu[i]) ? CHANNEL_VU_ATTACK_MS : CHANNEL_VU_RELEASE_MS;
        float alpha = channel_vu_ema_alpha(dt_ms, tau);
        g_channel_vu[i] = g_channel_vu[i] * (1.0f - alpha) + lvl * alpha;
        // update peak from realtime level
        if (lvl > g_channel_peak_level[i])
        {
            g_channel_peak_level[i] = lvl;
            g_channel_peak_hold_until[i] = nowTick + g_channel_peak_hold_ms;
        }
    }
    else
    {
        // Simple activity-based VU: set to full when any active notes on that channel
        // (from incoming MIDI UI array or engine active notes), otherwise decay.
        bool active = false;
        // Check per-channel incoming MIDI UI state
        for (int n = 0; n < BAE_MAX_NOTES && !active; n++)
        {
            if (g_keyboard_active_notes_by_channel[i][n])
                active = true;
        }
        // Also query engine active notes when playing or when no MIDI input.
        // Prevent querying the engine when playback is stopped AND MIDI input
        // is enabled so cleared VUs are not immediately repopulated.
#if SUPPORT_MIDI_HW == TRUE
        if (!active && !g_exporting && (playing || !g_midi_input_enabled))
#else
        if (!active && !g_exporting && playing)
#endif
        {
            BAESong target = g_bae.song ? g_bae.song : g_live_song;
            if (target)
            {
                unsigned char ch_notes[BAE_MAX_NOTES];
                memset(ch_notes, 0, sizeof(ch_notes));
                BAESong_GetActiveNotes(target, (unsigned char)i, ch_notes);
                for (int n = 0; n < BAE_MAX_NOTES; n++)
                {
                    if (ch_notes[n])
                    {
                        active = true;
                        break;
                    }
                }
            }
        }
        // Update channel VU with simple attack / time-based release
        if (active)
        {
            g_channel_vu[i] = 1.0f;
        }
        else
        {
            float alpha = channel_vu_ema_alpha(dt_ms, CHANNEL_ACTIVITY_RELEASE_MS);
            g_channel_vu[i] *= (1.0f - alpha);
            if (g_channel_vu[i] < 0.005f)
                g_channel_vu[i] = 0.0f;
        }
    }

    // Fill level from bottom using per-channel VU value (clamped)
    float lvl = g_channel_vu[i];
    if (lvl < 0.f)
        lvl = 0.f;
    if (lvl > 1.f)
        lvl = 1.f;
    int innerPad = 2;
    int innerH = meterH - (innerPad * 2);
    int fillH = (int)(lvl * innerH);
    if (fillH > 0)
    {
        // Draw a simple vertical gradient: green (low) -> yellow (mid) -> red (high)
        int gx = meterX + innerPad;
        int gw = meterW - (innerPad * 2);
        for (int yoff = 0; yoff < fillH; yoff++)
        {
            // map to gradient from green->yellow->red based on relative height
            float frac = (float)yoff / (float)(innerH > 0 ? innerH : 1);
            SDL_Color col;
            if (frac < 0.5f)
            { // green to yellow
                float p = frac / 0.5f;
                col.r = (Uint8)(g_highlight_color.r * p + 20 * (1.0f - p));
                col.g = (Uint8)(200 * (1.0f - (1.0f - p) * 0.2f));
                col.b = (Uint8)(20);
            }
            else
            { // yellow to red
                float p = (frac - 0.5f) / 0.5f;
                col.r = (Uint8)(200 + (55 * p));
                col.g = (Uint8)(200 * (1.0f - p));
                col.b = 20;
            }
            // Draw one horizontal scanline of the gradient from bottom upwards
            SDL_SetRenderDrawColor(R, col.r, col.g, col.b, 255);
#if USE_SDL2 == TRUE
            SDL_RenderDrawLine(R, gx, meterY + meterH - innerPad - 1 - yoff, gx + gw - 1, meterY + meterH - innerPad - 1 - yoff);
#else
            SDL_RenderLine(R, gx, meterY + meterH - innerPad - 1 - yoff, gx + gw - 1, meterY + meterH - innerPad - 1 - yoff);
#endif
        }
    }
    // Channel peak markers intentionally removed - we only draw the realtime fill.
    // Release smoothing above already decays idle meters in a frame-rate-independent way;
    // the old per-frame *= 0.92 made high-FPS draws collapse the bars between audio updates.
}

// 'All' checkbox: moved to render after the virtual keyboard so it appears on top.

// Channel control buttons in a row
int btnY = chStartY + 75;
// Disable channel controls while the reverb dropdown is expanded.
bool channel_controls_enabled = !(g_bae.is_audio_file && g_bae.sound) && !g_reverbDropdownOpen;
if (ui_button(R, (Rect){20, btnY, 80, 26}, "Invert", channel_controls_enabled ? ui_mx : -1, channel_controls_enabled ? ui_my : -1, channel_controls_enabled ? ui_mdown : false) && ui_mclick && !modal_block && channel_controls_enabled)
{
    for (int i = 0; i < BAE_MAX_MIDI_CHANNELS; i++)
    {
        bool was_enabled = ch_enable[i];
        ch_enable[i] = !ch_enable[i];
        // If channel was enabled and is now muted, clear its virtual keyboard highlights
        if (was_enabled && !ch_enable[i])
        {
            gui_clear_virtual_keyboard_channel(i);
        }
        // If channel was muted and is now enabled, refresh from engine state
        else if (!was_enabled && ch_enable[i])
        {
            gui_refresh_virtual_keyboard_channel_from_engine(i);
        }
    }
}
if (ui_button(R, (Rect){110, btnY, 80, 26}, "Mute All", channel_controls_enabled ? ui_mx : -1, channel_controls_enabled ? ui_my : -1, channel_controls_enabled ? ui_mdown : false) && ui_mclick && !modal_block && channel_controls_enabled)
{
    for (int i = 0; i < BAE_MAX_MIDI_CHANNELS; i++)
        ch_enable[i] = false;
    // Clear all virtual keyboard highlights when muting all channels
    gui_clear_virtual_keyboard_all_channels();
}
if (ui_button(R, (Rect){200, btnY, 90, 26}, "Unmute All", channel_controls_enabled ? ui_mx : -1, channel_controls_enabled ? ui_my : -1, channel_controls_enabled ? ui_mdown : false) && ui_mclick && !modal_block && channel_controls_enabled)
{
    for (int i = 0; i < BAE_MAX_MIDI_CHANNELS; i++)
    {
        ch_enable[i] = true;
        // Refresh virtual keyboard state from engine for all channels
        gui_refresh_virtual_keyboard_channel_from_engine(i);
    }
}

if (!g_exporting) {
    // Voice count VU meter - vertical meter aligned with channel VUs
    BAEAudioInfo audioInfo;
    int voiceCount = 0;
    if (g_bae.mixer && BAEMixer_GetRealtimeStatus(g_bae.mixer, &audioInfo) == BAE_NO_ERROR && playing && !g_bae.is_audio_file)
    {
        voiceCount = audioInfo.voicesActive;
    }

    // Calculate VU position and dimensions to align with channel VUs
    int vuX = 375;      // positioned to the right of "Unmute All" button
    int vuY = chStartY; // align with top of channel grid
    int vuW = 8;        // slightly wider than channel VUs for visibility
    int vuH = 71;       // span both rows: row height + checkbox + gap + text + row spacing adjustment

    // Draw VU background/frame
    Rect vuBg = {vuX, vuY, vuW, vuH};
    draw_rect(R, vuBg, g_panel_bg);
    draw_frame(R, vuBg, g_panel_border);

    // Calculate fill level (0-MAX_VOICES voices mapped to 0-1)
    float voiceFill = (float)voiceCount / (float)MAX_VOICES;
    if (voiceFill > 1.0f)
        voiceFill = 1.0f;
    if (voiceFill < 0.0f)
        voiceFill = 0.0f;

    int innerPad = 2;
    int innerH = vuH - (innerPad * 2);
    int fillH = (int)(voiceFill * innerH);

    if (fillH > 0)
    {
        // Draw voice VU with gradient similar to channel VUs
        int gx = vuX + innerPad;
        int gw = vuW - (innerPad * 2);
        for (int yoff = 0; yoff < fillH; yoff++)
        {
            float frac = (float)yoff / (float)(innerH > 0 ? innerH : 1);
            SDL_Color col;
            if (frac < 0.5f)
            { // green to yellow
                float p = frac / 0.5f;
                col.r = (Uint8)(g_highlight_color.r * p + 20 * (1.0f - p));
                col.g = (Uint8)(200 * (1.0f - (1.0f - p) * 0.2f));
                col.b = (Uint8)(20);
            }
            else
            { // yellow to red
                float p = (frac - 0.5f) / 0.5f;
                col.r = (Uint8)(200 + (55 * p));
                col.g = (Uint8)(200 * (1.0f - p));
                col.b = 20;
            }
            // Draw from bottom upwards
            SDL_SetRenderDrawColor(R, col.r, col.g, col.b, 255);
#if USE_SDL2 == TRUE
            SDL_RenderDrawLine(R, gx, vuY + vuH - innerPad - 1 - yoff, gx + gw - 1, vuY + vuH - innerPad - 1 - yoff);
#else
            SDL_RenderLine(R, gx, vuY + vuH - innerPad - 1 - yoff, gx + gw - 1, vuY + vuH - innerPad - 1 - yoff);
#endif
        }
    }

    // Handle tooltip for Voice VU meter
    if (point_in(ui_mx, ui_my, vuBg) && !modal_block && !g_bae.is_audio_file)
    {
        char tooltip_text[64];
        snprintf(tooltip_text, sizeof(tooltip_text), "Active Voices: %d", voiceCount);

        // Measure actual text width and height
        int text_w, text_h;
        measure_text(tooltip_text, &text_w, &text_h);

        int tooltip_w = text_w + 8; // 4px padding on each side
        int tooltip_h = text_h + 8; // 4px padding top and bottom
        if (tooltip_w > 500)
            tooltip_w = 500; // Maximum width constraint

        int tooltip_x = ui_mx + 10;
        int tooltip_y = ui_my - 30;

        // Keep tooltip on screen
        if (tooltip_x + tooltip_w > WINDOW_W - 4)
            tooltip_x = WINDOW_W - tooltip_w - 4;
        if (tooltip_y < 4)
            tooltip_y = ui_my + 25; // Show below cursor if no room above

        ui_set_tooltip((Rect){tooltip_x, tooltip_y, tooltip_w, tooltip_h}, tooltip_text, &g_voice_tooltip_visible, &g_voice_tooltip_rect, g_voice_tooltip_text, sizeof(g_voice_tooltip_text));
    }
    else
    {
        ui_clear_tooltip(&g_voice_tooltip_visible);
    }
}

// If playing an audio file (sound, not song), dim the MIDI channels panel
if (g_bae.is_audio_file && g_bae.sound)
{
    SDL_Color dim = g_is_dark_mode ? (SDL_Color){0, 0, 0, 160} : (SDL_Color){255, 255, 255, 160};
    draw_rect(R, channelPanel, dim);
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
