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

#ifndef GUI_TRANSPORT_H
#define GUI_TRANSPORT_H

#include "gui_frame.h"

extern int g_last_engine_pos_ms;

void gui_transport_reset_session_timer(void);
void gui_transport_update_progress(bool playing, int *progress, int *duration, int tempo);
void gui_transport_draw(GuiFrameCtx *ctx);

#endif
