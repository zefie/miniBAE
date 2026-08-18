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

#include "gui_keyboard_panel.h"
#include "gui_transport.h"

void gui_keyboard_panel_draw(GuiFrameCtx *ctx)
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

// Virtual MIDI Keyboard Panel (shown for songs) or waveform for audio files
if (showKeyboard || (g_bae.is_audio_file && g_bae.sound))
{
    draw_rect(R, keyboardPanel, panelBg);
    draw_frame(R, keyboardPanel, panelBorder);
    if (g_bae.is_audio_file && g_bae.sound)
    {
        draw_text(R, keyboardPanel.x + 10, keyboardPanel.y + 8, "WAVEFORM", headerCol);
        // Waveform area
        int wfX = keyboardPanel.x + 10;
        int wfY = keyboardPanel.y + 32;
        int wfW = keyboardPanel.w - 20;
        int wfH = keyboardPanel.h - 46;
        Rect wfRect = {wfX, wfY, wfW, wfH};
        draw_rect(R, wfRect, g_panel_bg);
        draw_frame(R, wfRect, g_panel_border);
        // Inset inner drawing rect by 1px on each side so waveform lines don't overwrite the border
        int inWfX = wfX + 1;
        int inWfY = wfY + 1;
        int inWfW = wfW - 2;
        int inWfH = wfH - 2;
        if (inWfW < 1)
            inWfW = 1;
        if (inWfH < 1)
            inWfH = 1;

        // Try to get raw sample pointer and length (in frames)
        uint32_t frames = 0;
        void *raw = BAESound_GetSamplePlaybackPointer(g_bae.sound, &frames);
        if (raw && frames > 0)
        {
            // Determine bit depth/channels via BAESound_GetInfo
            BAESampleInfo info;
            if (BAESound_GetInfo(g_bae.sound, &info) == BAE_NO_ERROR)
            {
                int channels = info.channels;
                int bitDepth = info.bitSize;
                // We'll treat samples as 16-bit signed if bitDepth>=16, else 8-bit unsigned
                if (bitDepth >= 16)
                {
                    int16_t *s16 = (int16_t *)raw; // interleaved if stereo
                    // Compute samples per pixel (use frames -> pixels)
                    uint32_t frames_per_px = frames / (uint32_t)inWfW;
                    if (frames_per_px < 1)
                        frames_per_px = 1;
                    for (int x = 0; x < inWfW; x++)
                    {
                        uint32_t start = (uint32_t)x * frames_per_px;
                        uint32_t end = start + frames_per_px;
                        if (end > frames)
                            end = frames;
                        int16_t minv = 0, maxv = 0;
                        bool inited = false;
                        for (uint32_t f = start; f < end; f++)
                        {
                            int idx = (int)f * channels; // take first channel for visualization
                            int16_t v = s16[idx];
                            if (!inited)
                            {
                                minv = maxv = v;
                                inited = true;
                            }
                            if (v < minv)
                                minv = v;
                            if (v > maxv)
                                maxv = v;
                        }
                        // Map min/max to pixel Y
                        float minf = (float)minv / 32768.0f;
                        float maxf = (float)maxv / 32768.0f;
                        int y0 = inWfY + (int)((1.0f - (maxf * 0.5f + 0.5f)) * (inWfH - 2));
                        int y1 = inWfY + (int)((1.0f - (minf * 0.5f + 0.5f)) * (inWfH - 2));
                        if (y0 < wfY)
                            y0 = inWfY;
                        if (y1 > inWfY + inWfH - 1)
                            y1 = inWfY + inWfH - 1;
                        SDL_SetRenderDrawColor(R, g_accent_color.r, g_accent_color.g, g_accent_color.b, 255);
#if USE_SDL2 == TRUE
                        SDL_RenderDrawLine(R, inWfX + x, y0, inWfX + x, y1);
#else
                        SDL_RenderLine(R, inWfX + x, y0, inWfX + x, y1);
#endif
                    }
                }
                else
                {
                    // 8-bit samples (unsigned)
                    uint8_t *s8 = (uint8_t *)raw;
                    uint32_t frames_per_px = frames / (uint32_t)inWfW;
                    if (frames_per_px < 1)
                        frames_per_px = 1;
                    for (int x = 0; x < inWfW; x++)
                    {
                        uint32_t start = (uint32_t)x * frames_per_px;
                        uint32_t end = start + frames_per_px;
                        if (end > frames)
                            end = frames;
                        uint8_t minv = 128, maxv = 128;
                        for (uint32_t f = start; f < end; f++)
                        {
                            uint8_t v = s8[f * info.channels];
                            if (v < minv)
                                minv = v;
                            if (v > maxv)
                                maxv = v;
                        }
                        float minf = ((float)minv - 128.0f) / 128.0f;
                        float maxf = ((float)maxv - 128.0f) / 128.0f;
                        int y0 = inWfY + (int)((1.0f - (maxf * 0.5f + 0.5f)) * (inWfH - 2));
                        int y1 = inWfY + (int)((1.0f - (minf * 0.5f + 0.5f)) * (inWfH - 2));
                        if (y0 < inWfY)
                            y0 = inWfY;
                        if (y1 > inWfY + inWfH - 1)
                            y1 = inWfY + inWfH - 1;
                        SDL_SetRenderDrawColor(R, g_accent_color.r, g_accent_color.g, g_accent_color.b, 255);
#if USE_SDL2 == TRUE
                        SDL_RenderDrawLine(R, inWfX + x, y0, inWfX + x, y1);
#else
                        SDL_RenderLine(R, inWfX + x, y0, inWfX + x, y1);
#endif
                    }
                }
            }
        }

        // Waveform click-to-seek handling: compute target ms for hover/drag preview
        // and only perform the actual engine seek on mouse-up. This avoids
        // repeated seeks while the user holds the mouse in place.
        int wf_relx = -1;
        // Use inner waveform rect for hit-testing so clicks near the border
        // don't map onto waveform pixels that would overdraw the frame.
        if (ui_mx >= inWfX && ui_mx < inWfX + inWfW && ui_my >= inWfY && ui_my < inWfY + inWfH)
        {
            wf_relx = ui_mx - inWfX;
            if (wf_relx < 0)
                wf_relx = 0;
            if (wf_relx > inWfW - 1)
                wf_relx = inWfW - 1;
        }

        // Helper to map pixel->ms
        int computed_ms = -1;
        if (wf_relx >= 0)
        {
            BAESampleInfo info2;
            if (BAESound_GetInfo(g_bae.sound, &info2) == BAE_NO_ERROR)
            {
                double sampleRate = (double)(info2.sampledRate >> 16) + (double)(info2.sampledRate & 0xFFFF) / 65536.0;
                if (sampleRate > 0 && audio_total_frames > 0)
                {
                    double frac = (double)wf_relx / (double)(inWfW);
                    if (frac < 0.0)
                        frac = 0.0;
                    if (frac > 1.0)
                        frac = 1.0;
                    uint32_t frame_position = (uint32_t)((double)audio_total_frames * frac);
                    if (frame_position >= audio_total_frames)
                        frame_position = audio_total_frames - 1;
                    computed_ms = (int)((double)frame_position * 1000.0 / sampleRate);
                }
            }
        }

        // Dragging: update preview but don't call engine seek until mouse-up
        static int waveform_preview_ms = -1;
        const int WAVEFORM_SEEK_DEADZONE_MS = 40; // avoid tiny-seek jitter while holding
        if (ui_mdown && computed_ms >= 0)
        {
            // Only update preview/progress if it changed beyond deadzone
            if (waveform_preview_ms < 0 || abs(computed_ms - waveform_preview_ms) > WAVEFORM_SEEK_DEADZONE_MS)
            {
                waveform_preview_ms = computed_ms;
                progress = waveform_preview_ms; // show preview position in UI
            }
        }
        else if (!mdown)
        {
            // If mouse released over waveform, perform actual seek on mouse-up
            if (ui_mclick && computed_ms >= 0)
            {
                int target_ms = computed_ms;
                // prefer preview if it exists
                if (waveform_preview_ms >= 0)
                    target_ms = waveform_preview_ms;
                bae_seek_ms(target_ms);
                progress = target_ms;
                g_last_engine_pos_ms = target_ms;
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
            // Reset preview state when not dragging
            waveform_preview_ms = -1;
        }

        // Draw a playhead indicator for the preview or current position
        int playhead_ms = (waveform_preview_ms >= 0) ? waveform_preview_ms : progress;
        if (playhead_ms >= 0 && audio_total_frames > 0)
        {
            BAESampleInfo info3;
            if (BAESound_GetInfo(g_bae.sound, &info3) == BAE_NO_ERROR)
            {
                double sampleRate = (double)(info3.sampledRate >> 16) + (double)(info3.sampledRate & 0xFFFF) / 65536.0;
                if (sampleRate > 0)
                {
                    uint32_t frame_pos = (uint32_t)((double)playhead_ms * sampleRate / 1000.0);
                    if (frame_pos >= audio_total_frames)
                        frame_pos = audio_total_frames - 1;
                    double frac = (double)frame_pos / (double)audio_total_frames;
                    int phx = inWfX + (int)(frac * inWfW);
                    SDL_SetRenderDrawColor(R, g_highlight_color.r, g_highlight_color.g, g_highlight_color.b, 220);
#if USE_SDL2 == TRUE
                    SDL_RenderDrawLine(R, phx, inWfY, phx, inWfY + inWfH - 1);
#else
                    SDL_RenderLine(R, phx, inWfY, phx, inWfY + inWfH - 1);
#endif
                }
            }
        }
        // If we're showing waveform only, skip rest of keyboard rendering
        if (g_bae.is_audio_file && g_bae.sound)
        {
            goto SKIP_KEYBOARD_RENDER;
        }
    }
    else
    {
        draw_text(R, keyboardPanel.x + 10, keyboardPanel.y + 8, "VIRTUAL MIDI KEYBOARD", headerCol);
    }
    // When waveform is shown we don't render the virtual keyboard controls
    if (!(g_bae.is_audio_file && g_bae.sound))
    {
        // Channel dropdown
        const char *chanItems[BAE_MAX_MIDI_CHANNELS];
        char chanBuf[BAE_MAX_MIDI_CHANNELS][8];
        for (int i = 0; i < BAE_MAX_MIDI_CHANNELS; i++)
        {
            snprintf(chanBuf[i], sizeof(chanBuf[i]), "Ch %d", i + 1);
            chanItems[i] = chanBuf[i];
        }
        Rect chanDD = {keyboardPanel.x + 10, keyboardPanel.y + 28, 90, 22};
        // Render main dropdown box
        SDL_Color ddBg = g_button_base;
        SDL_Color ddTxt = g_button_text;
        SDL_Color ddFrame = g_button_border;
        bool overDD = point_in(ui_mx, ui_my, chanDD);
        if (overDD)
            ddBg = g_button_hover;
        draw_rect(R, chanDD, ddBg);
        draw_frame(R, chanDD, ddFrame);
        draw_text(R, chanDD.x + 6, chanDD.y + 2, chanItems[g_keyboard_channel], ddTxt);
        draw_text(R, chanDD.x + chanDD.w - 16, chanDD.y + 1, g_keyboard_channel_dd_open ? "^" : "v", ddTxt);
        if (!modal_block && ui_mclick && overDD)
        {
            g_keyboard_channel_dd_open = !g_keyboard_channel_dd_open;
        }
        // (Dropdown list itself drawn later for proper z-order)
        if (!g_keyboard_channel_dd_open && ui_mclick && !overDD)
        { /* no-op */
        }

        // Program/Bank number pickers - compact layout below channel dropdown
        int picker_y = keyboardPanel.y + 56; // below channel dropdown
        int picker_w = 35;                   // compact width for 3-digit numbers
        int picker_h = 18;
        int spacing = 5;

        // Bank picker (now first)
        Rect bankRect = {keyboardPanel.x + 10, picker_y, picker_w, picker_h};
        bool bankHover = !g_keyboard_channel_dd_open && point_in(ui_mx, ui_my, bankRect);
        SDL_Color bankBg = bankHover ? g_button_hover : g_button_base;
        if (g_keyboard_channel_dd_open)
            bankBg.a = 180; // Make it appear disabled when channel dropdown is open
        draw_rect(R, bankRect, bankBg);
        draw_frame(R, bankRect, g_button_border);

        char bankText[8];
        snprintf(bankText, sizeof(bankText), "%d", g_keyboard_bank);
        int bank_tw = 0, bank_th = 0;
        measure_text(bankText, &bank_tw, &bank_th);
        SDL_Color bankTextColor = g_button_text;
        if (g_keyboard_channel_dd_open)
            bankTextColor.a = 180; // Dim text when disabled
        draw_text(R, bankRect.x + (bankRect.w - bank_tw) / 2, bankRect.y + (bankRect.h - bank_th) / 2, bankText, bankTextColor);

        // Program picker (now second)
        Rect programRect = {bankRect.x + picker_w + spacing, picker_y, picker_w, picker_h};
        bool programHover = !g_keyboard_channel_dd_open && point_in(ui_mx, ui_my, programRect);
        SDL_Color programBg = programHover ? g_button_hover : g_button_base;
        if (g_keyboard_channel_dd_open)
            programBg.a = 180; // Make it appear disabled when channel dropdown is open
        draw_rect(R, programRect, programBg);
        draw_frame(R, programRect, g_button_border);

        char programText[8];
        snprintf(programText, sizeof(programText), "%d", g_keyboard_program);
        int program_tw = 0, program_th = 0;
        measure_text(programText, &program_tw, &program_th);
        SDL_Color programTextColor = g_button_text;
        if (g_keyboard_channel_dd_open)
            programTextColor.a = 180; // Dim text when disabled
        draw_text(R, programRect.x + (programRect.w - program_tw) / 2, programRect.y + (programRect.h - program_th) / 2, programText, programTextColor);

        // Handle tooltips (disabled when channel dropdown is open)
        if (!g_keyboard_channel_dd_open)
        {
            if (programHover)
            {
                ui_set_tooltip((Rect){ui_mx + 10, ui_my - 25, 0, 0}, "Program", &g_program_tooltip_visible, &g_program_tooltip_rect, g_program_tooltip_text, sizeof(g_program_tooltip_text));
            }
            else if (bankHover)
            {
                ui_set_tooltip((Rect){ui_mx + 10, ui_my - 25, 0, 0}, "Bank", &g_bank_tooltip_visible, &g_bank_tooltip_rect, g_bank_tooltip_text, sizeof(g_bank_tooltip_text));
            }
        }

        // Handle clicks (disabled when channel dropdown is open)
        if (!modal_block && !g_keyboard_channel_dd_open)
        {
            if (bankHover)
            {
                if (ui_mclick) // Left click increments
                {
                    change_bank_value_for_current_channel(true, +1);
                }
                else if (ui_rclick) // Right click decrements
                {
                    change_bank_value_for_current_channel(true, -1);
                }
            }
            else if (programHover)
            {
                if (ui_mclick) // Left click increments
                {
                    change_bank_value_for_current_channel(false, +1);
                }
                else if (ui_rclick) // Right click decrements
                {
                    change_bank_value_for_current_channel(false, -1);
                }
            }
        }
    }
    // Merge engine-driven active notes with notes coming from external
    // MIDI input, QWERTY typing, and mouse clicks so all sources light
    // the virtual keys regardless of playback state.
    unsigned char merged_notes[BAE_MAX_NOTES];
    memset(merged_notes, 0, sizeof(merged_notes));

    // Always include per-channel UI/MIDI-input notes first so QWERTY
    // typing and mouse-click notes show immediately every frame.
    if (g_keyboard_show_all_channels)
    {
        for (int ch = 0; ch < BAE_MAX_MIDI_CHANNELS; ++ch)
        {
            for (int n = 0; n < BAE_MAX_NOTES; ++n)
                merged_notes[n] |= g_keyboard_active_notes_by_channel[ch][n] ? 1 : 0;
        }
    }
    else
    {
        for (int n = 0; n < BAE_MAX_NOTES; ++n)
            merged_notes[n] |= g_keyboard_active_notes_by_channel[g_keyboard_channel][n] ? 1 : 0;
    }

    // Also query engine active notes and OR them in so MIDI-file
    // playback lights the keys.  Skip during export and briefly after
    // seek/stop (suppress_until) while engine note state settles.
    if (!g_exporting)
    {
        BAESong target = g_bae.song ? g_bae.song : g_live_song;
        if (target && (g_bae.is_playing || g_keyboard_mouse_note != -1))
        {
            Uint32 nowms = SDL_GetTicks();
            if (nowms >= g_keyboard_suppress_until)
            {
                if (g_keyboard_show_all_channels)
                {
                    unsigned char ch_notes[BAE_MAX_MIDI_CHANNELS][BAE_MAX_NOTES];
                    BAESong_GetAllActiveNotes(target, ch_notes);
                    for (int ch = 0; ch < BAE_MAX_MIDI_CHANNELS; ++ch)
                    {
                        if (ch_enable[ch])
                        {
                            for (int i = 0; i < BAE_MAX_NOTES; i++)
                                merged_notes[i] |= ch_notes[ch][i];
                        }
                    }
                }
                else
                {
                    if (ch_enable[g_keyboard_channel])
                    {
                        unsigned char engine_notes[BAE_MAX_NOTES];
                        memset(engine_notes, 0, sizeof(engine_notes));
                        BAESong_GetActiveNotes(target, (unsigned char)g_keyboard_channel, engine_notes);
                        for (int i = 0; i < BAE_MAX_NOTES; i++)
                            merged_notes[i] |= engine_notes[i];
                    }
                }
            }
        }
    }
    // Normalize velocity bytes into bool: memcpy would bypass bool conversion
    // and leave raw velocity bytes in place. Clang reads bool as bit 0 only,
    // so an even velocity like 100 (0x64) would appear as false. Normalize explicitly.
    for (int _n = 0; _n < BAE_MAX_NOTES; _n++)
        g_keyboard_active_notes[_n] = (merged_notes[_n] != 0);
    // Build a quick lookup of notes triggered by typed (qwerty) keys so
    // we can render them with the highlight color instead of the
    // accent color used for incoming MIDI/engine activity.
    unsigned char typed_notes[BAE_MAX_NOTES];
    memset(typed_notes, 0, sizeof(typed_notes));
    for (int sc = 0; sc < 512; ++sc)
    {
        int mn = g_keyboard_pressed_note[sc];
        if (mn >= 0 && mn < BAE_MAX_NOTES)
            typed_notes[mn] = 1;
    }
    // Keyboard drawing region
    int kbX = keyboardPanel.x + 110;
    int kbY = keyboardPanel.y + 28;
    int kbW = keyboardPanel.w - 120;
    int kbH = keyboardPanel.h - 38;
    // Define note range (61-key C2..C7)
    int firstNote = 36;
    int lastNote = 96; // inclusive
    // Count white keys
    int whiteCount = 0;
    for (int n = firstNote; n <= lastNote; n++)
    {
        int m = n % 12;
        if (m == 0 || m == 2 || m == 4 || m == 5 || m == 7 || m == 9 || m == 11)
            whiteCount++;
    }
    if (whiteCount < 1)
        whiteCount = 1;
    float whiteWf = (float)kbW / whiteCount;
    // First pass draw white keys
    int wIndex = 0;
    int mouseNoteCandidateWhite = -1;
    int mouseNoteCandidateBlack = -1; // track hover note (black wins)
    for (int n = firstNote; n <= lastNote; n++)
    {
        int m = n % 12;
        bool isWhite = (m == 0 || m == 2 || m == 4 || m == 5 || m == 7 || m == 9 || m == 11);
        if (isWhite)
        {
            int x = kbX + (int)(wIndex * whiteWf);
            int nextX = kbX + (int)((wIndex + 1) * whiteWf);
            int w = nextX - x - 1;
            if (w < 4)
                w = 4;
            SDL_Color keyCol = g_is_dark_mode ? (SDL_Color){200, 200, 205, 255} : (SDL_Color){245, 245, 245, 255};
            // Default: engine/MIDI-driven active notes use accent color.
            if (g_keyboard_active_notes[n])
                keyCol = g_accent_color;
            // Override: typed (qwerty) notes or mouse-held notes use highlight.
            if (typed_notes[n] || g_keyboard_mouse_note == n)
                keyCol = g_highlight_color;
            draw_rect(R, (Rect){x, kbY, w, kbH}, keyCol);
            draw_frame(R, (Rect){x, kbY, w, kbH}, g_panel_border);
            // Optional note name for C notes
            if (m == 0)
            {
                char nb[8];
                int octave = (n / 12) - 1;
                snprintf(nb, sizeof(nb), "C%d", octave);
                int tw, th;
                measure_text(nb, &tw, &th);
                draw_text(R, x + 2, kbY + kbH - (th + 2), nb, g_is_dark_mode ? (SDL_Color){20, 20, 25, 255} : (SDL_Color){30, 30, 30, 255});
            }
            if (!g_keyboard_channel_dd_open && !modal_block && !g_reverbDropdownOpen &&
                 !g_exporting && ui_mx >= x && ui_mx < x + w && ui_my >= kbY && ui_my < kbY + kbH)
            {
                mouseNoteCandidateWhite = n;
            }
            wIndex++;
        }
    }
    // Second pass draw black keys
    wIndex = 0; // re-evaluate positions
    // Build array mapping note->x base for white key underneath (use float to reduce truncation)
    float whitePosF[128];
    for (int i = 0; i < 128; ++i)
        whitePosF[i] = 0.0f;
    for (int n = firstNote; n <= lastNote; n++)
    {
        int m = n % 12;
        bool isWhite = (m == 0 || m == 2 || m == 4 || m == 5 || m == 7 || m == 9 || m == 11);
        if (isWhite)
        {
            whitePosF[n] = (float)kbX + (wIndex * whiteWf);
            wIndex++;
        }
    }
    for (int n = firstNote; n <= lastNote; n++)
    {
        int m = n % 12;
        bool isBlack = (m == 1 || m == 3 || m == 6 || m == 8 || m == 10);
        if (isBlack)
        {
            // position relative to previous white key
            int prevWhite = n - 1;
            while (prevWhite >= firstNote)
            {
                int mm = prevWhite % 12;
                if (mm == 0 || mm == 2 || mm == 4 || mm == 5 || mm == 7 || mm == 9 || mm == 11)
                    break;
                prevWhite--;
            }
            if (prevWhite < firstNote)
                continue;
            float wxf = whitePosF[prevWhite];
            float wxNextf = wxf + whiteWf;
            // Compute black key width first, then center it between wxf and wxNextf
            int bw = (int)(whiteWf * 0.6f);
            if (bw < 4)
                bw = 4;
            int bx = (int)(wxf + ((wxNextf - wxf - (float)bw) * 0.5f));
            // Shift every black key right by 12 pixels (reduced by 6px)
            bx += 10;
            // Intentionally do not clamp bx to wxNext so black keys hover over whites
            int bh = (int)(kbH * 0.62f);
            SDL_Color keyCol = g_is_dark_mode ? (SDL_Color){40, 40, 45, 255} : (SDL_Color){50, 50, 60, 255};
            // Engine/MIDI active notes use accent by default for contrast
            if (g_keyboard_active_notes[n])
                keyCol = g_accent_color;
            // But typed/mouse-held notes should appear highlighted
            if (typed_notes[n] || g_keyboard_mouse_note == n)
                keyCol = g_highlight_color;
            draw_rect(R, (Rect){bx, kbY, bw, bh}, keyCol);
            draw_frame(R, (Rect){bx, kbY, bw, bh}, g_panel_border);
            if (!g_keyboard_channel_dd_open && !modal_block && !g_reverbDropdownOpen && 
#if SUPPORT_MIDI_HW == TRUE
#endif                        
                !g_exporting && ui_mx >= bx && ui_mx < bx + bw && ui_my >= kbY && ui_my < kbY + bh)
            {
                mouseNoteCandidateBlack = n;
            }
        }
    }
    // Determine hovered note (black takes precedence over white)
    int mouseNote = (mouseNoteCandidateBlack != -1) ? mouseNoteCandidateBlack : mouseNoteCandidateWhite;
    // Interaction: monophonic click-n-drag play (velocity varies by vertical position)
    if (!modal_block && !g_keyboard_channel_dd_open && !g_reverbDropdownOpen && 
#if SUPPORT_MIDI_HW == TRUE
#endif
        !g_exporting)
    {
        if (ui_mdown)
        {
            if (mouseNote != -1 && mouseNote != g_keyboard_mouse_note)
            {
                // Release previous
                if (g_keyboard_mouse_note != -1)
                {
                    BAESong target = g_bae.song ? g_bae.song : g_live_song;
                    if (target)
                    {
                        BAESong_NoteOff(target, (unsigned char)g_keyboard_channel, (unsigned char)g_keyboard_mouse_note, 0, 0);
                    }
                }
                // Compute velocity based on Y position inside the key: quiet near top, loud near bottom.
                // Bottom 15 pixels always map to max velocity (127).
                int keyHeight = kbH; // default white key height
                int mod = mouseNote % 12;
                bool isBlack = (mod == 1 || mod == 3 || mod == 6 || mod == 8 || mod == 10);
                if (isBlack)
                {
                    keyHeight = (int)(kbH * 0.62f);
                }
                int relY = ui_my - kbY;
                if (relY < 0)
                    relY = 0;
                if (relY >= keyHeight)
                    relY = keyHeight - 1;
                int fromBottom = keyHeight - 1 - relY;
                int vel;
                if (fromBottom < 15)
                {
                    vel = 127; // bottom 15px -> max
                }
                else
                {
                    int effectiveRange = keyHeight - 15;
                    if (effectiveRange < 1)
                        effectiveRange = 1;
                    float t = (float)relY / (float)effectiveRange; // 0 (top) .. 1 (just above bottom zone)
                    if (t < 0.f)
                        t = 0.f;
                    if (t > 1.f)
                        t = 1.f;
                    vel = (int)(t * 112.0f); // map into 0..112
                    if (vel < 8)
                        vel = 8; // floor so very top still audible
                    if (vel > 112)
                        vel = 112;
                }
                {
                    BAESong target = g_bae.song ? g_bae.song : g_live_song;
                    if (target)
                    {
                        BAESong_NoteOnWithLoad(target, (unsigned char)g_keyboard_channel, (unsigned char)mouseNote, (unsigned char)vel, 0);
                    }
                }
#if SUPPORT_MIDI_HW == TRUE
                if (g_midi_output_enabled)
                {
                    unsigned char m[3];
                    m[0] = (unsigned char)(0x90 | (g_keyboard_channel & 0x0F));
                    m[1] = (unsigned char)mouseNote;
                    m[2] = (unsigned char)vel;
                    midi_output_send(m, 3);
                }
#endif
                g_keyboard_mouse_note = mouseNote;
            }
            else if (mouseNote == -1 && g_keyboard_mouse_note != -1)
            {
                // Dragged outside – stop sounding note
                {
                    BAESong target = g_bae.song ? g_bae.song : g_live_song;
                    if (target)
                    {
                        BAESong_NoteOff(target, (unsigned char)g_keyboard_channel, (unsigned char)g_keyboard_mouse_note, 0, 0);
                    }
                }
#if SUPPORT_MIDI_HW == TRUE
                if (g_midi_output_enabled && g_keyboard_mouse_note != -1)
                {
                    unsigned char m[3];
                    m[0] = (unsigned char)(0x80 | (g_keyboard_channel & 0x0F));
                    m[1] = (unsigned char)g_keyboard_mouse_note;
                    m[2] = 0;
                    midi_output_send(m, 3);
                }
#endif
                g_keyboard_mouse_note = -1;
            }
        }
        else
        {
            // Mouse released anywhere
            if (g_keyboard_mouse_note != -1)
            {
                BAESong target = g_bae.song ? g_bae.song : g_live_song;
                if (target)
                {
                    BAESong_NoteOff(target, (unsigned char)g_keyboard_channel, (unsigned char)g_keyboard_mouse_note, 0, 0);
                }
#if SUPPORT_MIDI_HW == TRUE
                if (g_midi_output_enabled)
                {
                    unsigned char m[3];
                    m[0] = (unsigned char)(0x80 | (g_keyboard_channel & 0x0F));
                    m[1] = (unsigned char)g_keyboard_mouse_note;
                    m[2] = 0;
                    midi_output_send(m, 3);
                }
                g_keyboard_mouse_note = -1;
#endif
            }
        }
    }
    else
    {
        // If dropdown/modal opens while holding a note, release it
        if (g_keyboard_mouse_note != -1)
        {
            BAESong target = g_bae.song ? g_bae.song : g_live_song;
            if (target)
            {
                BAESong_NoteOff(target, (unsigned char)g_keyboard_channel, (unsigned char)g_keyboard_mouse_note, 0, 0);
            }
#if SUPPORT_MIDI_HW == TRUE
            if (g_midi_output_enabled)
            {
                unsigned char m[3];
                m[0] = (unsigned char)(0x80 | (g_keyboard_channel & 0x0F));
                m[1] = (unsigned char)g_keyboard_mouse_note;
                m[2] = 0;
                midi_output_send(m, 3);
            }
#endif
            g_keyboard_mouse_note = -1;
        }
    }
}
    SKIP_KEYBOARD_RENDER:

    {
Rect loopR = {160, 215, 20, 20};
// Create tooltip area that includes checkbox and label
Rect loopTooltipArea = {160, 215, 70, 20}; // Wider to include "BAE Loop" text
bool clicked = false;
#if SUPPORT_MIDI_HW == TRUE
// When MIDI input is enabled, render a disabled Loop checkbox (no interaction) so it appears under the dim overlay
if (g_midi_input_enabled)
{
    SDL_Color disabledBg = g_panel_bg;
    SDL_Color disabledTxt = g_panel_border;
    // Draw checkbox background and border
    draw_rect(R, loopR, disabledBg);
    draw_frame(R, loopR, g_panel_border);
    Rect inner = {loopR.x + 3, loopR.y + 3, loopR.w - 6, loopR.h - 6};
    if (loopPlay)
    {
        draw_rect(R, inner, g_accent_color);
        draw_frame(R, inner, g_button_text);
    }
    else
    {
        draw_rect(R, inner, g_panel_bg);
        draw_frame(R, inner, g_panel_border);
    }
    // Label
    draw_text(R, loopR.x + loopR.w + 6, loopR.y + 2, "BAE Loop", disabledTxt);
}
else
{
#endif
    if (!modal_block)
    {
        if (ui_toggle(R, loopR, &loopPlay, "BAE Loop", ui_mx, ui_my, ui_mclick))
            clicked = true;
    }
    else if (g_exporting)
    {
        // While exporting allow loop toggle using real mouse coords so user can uncheck loop
        if (ui_toggle(R, loopR, &loopPlay, "BAE Loop", mx, my, mclick))
            clicked = true;
    }
    else
    {
        // When modal is open, render disabled checkbox (same as MIDI input disabled style)
        SDL_Color disabledBg = g_panel_bg;
        SDL_Color disabledTxt = g_panel_border;
        // Draw checkbox background and border
        draw_rect(R, loopR, disabledBg);
        draw_frame(R, loopR, g_panel_border);
        Rect inner = {loopR.x + 3, loopR.y + 3, loopR.w - 6, loopR.h - 6};
        if (loopPlay)
        {
            draw_rect(R, inner, g_accent_color);
            draw_frame(R, inner, g_button_text);
        }
        else
        {
            draw_rect(R, inner, g_panel_bg);
            draw_frame(R, inner, g_panel_border);
        }
        // Label
        draw_text(R, loopR.x + loopR.w + 6, loopR.y + 2, "BAE Loop", disabledTxt);
    }

    // Handle tooltip for BAE Loop
    if (point_in(ui_mx, ui_my, loopTooltipArea) && !modal_block)
    {
        const char *tooltip_text = "BAE Loop will interfere with the playlist, disable it to use the playlist.";

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

        ui_set_tooltip((Rect){tooltip_x, tooltip_y, tooltip_w, tooltip_h}, tooltip_text, &g_loop_tooltip_visible, &g_loop_tooltip_rect, g_loop_tooltip_text, sizeof(g_loop_tooltip_text));
    }
    else
    {
        ui_clear_tooltip(&g_loop_tooltip_visible);
    }

    if (clicked)
    {
        bae_set_loop(loopPlay);
        g_bae.loop_enabled_gui = loopPlay;

        // Update loop count on currently loaded audio file if any
        if (g_bae.is_audio_file && g_bae.sound)
        {
            uint32_t loopCount = loopPlay ? 0xFFFFFFFF : 0;
            BAESound_SetLoopCount(g_bae.sound, loopCount);
        }

        // Save settings when loop is changed
        if (g_current_bank_path[0] != '\0')
        {
            save_settings(g_current_bank_path, reverbType, loopPlay);
        }
    }
#if SUPPORT_MIDI_HW == TRUE
}
#endif
    }
#if SUPPORT_MIDI_HW == TRUE
if (g_midi_input_enabled)
{
    // Draw disabled Open... button (no interaction)
    Rect openRect = {258, 215, 80, 22}; // Moved from 230 to 258 to accommodate "BAE Loop"
    SDL_Color disabledBg = g_panel_bg;
    SDL_Color disabledTxt = g_panel_border;
    draw_rect(R, openRect, disabledBg);
    draw_frame(R, openRect, g_panel_border);
    int text_w = 0, text_h = 0;
    measure_text("Open...", &text_w, &text_h);
    int text_x = openRect.x + (openRect.w - text_w) / 2;
    int text_y = openRect.y + (openRect.h - text_h) / 2;
    draw_text(R, text_x, text_y, "Open...", disabledTxt);
}
else
{
#endif
    if (g_reverbDropdownOpen)
    {
        // Draw disabled Open... button (no interaction)
        Rect openRect = {258, 215, 80, 22};
        SDL_Color disabledBg = g_panel_bg;
        SDL_Color disabledTxt = g_panel_border;
        disabledBg.a = 200;
        disabledTxt.a = 200;
        draw_rect(R, openRect, disabledBg);
        draw_frame(R, openRect, g_panel_border);
        int text_w = 0, text_h = 0;
        measure_text("Open...", &text_w, &text_h);
        int text_x = openRect.x + (openRect.w - text_w) / 2;
        int text_y = openRect.y + (openRect.h - text_h) / 2;
        draw_text(R, text_x, text_y, "Open...", disabledTxt);
    }
    else if (ui_button(R, (Rect){258, 215, 80, 22}, "Open...", ui_mx, ui_my, ui_mdown) && ui_mclick && !modal_block) // Moved from 230 to 258
    {
        char *sel = open_file_dialog();
        if (sel)
        {
            if (bae_load_song_with_settings(sel, transpose, tempo, volume, loopPlay, reverbType, ch_enable, true))
            {
#if SUPPORT_PLAYLIST == TRUE
                if (g_playlist.enabled) {
                    // Add file to playlist and set as current
                    playlist_update_current_file(sel);
                }
#endif
                duration = bae_get_len_ms();
                progress = 0;
                playing = false; // force toggle logic
                if (!bae_play(&playing))
                {
                    BAE_PRINTF("Autoplay after Open failed for '%s'\n", sel);
                }
                if (playing && g_bae.mixer)
                {
                    for (int i = 0; i < 3; i++)
                    {
                        BAEMixer_Idle(g_bae.mixer);
                    }
                }
            }
            free(sel);
        }
    }
#if SUPPORT_MIDI_HW == TRUE
}
#endif

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
