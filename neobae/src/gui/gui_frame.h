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

#ifndef GUI_FRAME_H
#define GUI_FRAME_H

#include "gui_common.h"
#include "gui_panels.h"
#include "NeoBAE.h"

/* Shared per-frame state for immediate-mode panel peels. */
typedef struct GuiFrameCtx
{
    SDL_Renderer *R;
    SDL_Window *win;

    /* Raw mouse for the frame (before modal swallow). */
    int mx, my;
    bool mdown;
    bool *mclick; /* raw frame click; some peels clear mid-frame for dialogs */

    int ui_mx, ui_my;
    bool ui_mdown;
    bool ui_mclick; /* mutable across peels (click consume) */
    bool ui_rclick;

    int transport_mx, transport_my;
    bool transport_mdown, transport_mclick;

    bool modal_block;
    bool modal_block_transport;

    bool *playing;
    int *progress;
    int *duration;
    int *transpose;
    int *tempo;
    int *volume;
    int *reverb_type; /* reverbType in main */
    bool *loop_enabled; /* loopPlay in main */
    bool *ch_enable; /* BAE_MAX_MIDI_CHANNELS */
    int *last_drag_progress;

    Rect channelPanel;
    Rect controlPanel;
    Rect transportPanel;
    Rect keyboardPanel;
    Rect statusPanel;

    SDL_Color labelCol;
    SDL_Color headerCol;
    SDL_Color panelBg;
    SDL_Color panelBorder;

    bool showKeyboard;
    bool showWaveform;
} GuiFrameCtx;

#endif /* GUI_FRAME_H */
