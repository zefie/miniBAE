/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef GUI_MIDI_H
#define GUI_MIDI_H

#include "gui_midi_vkbd.h"
#include "gui_midi_hw.h"

/* Declaration only - actual definition is provided in gui_bae.c.
	Use extern to avoid multiple-definition linker errors. */
extern double g_last_requested_master_volume;
extern int g_thread_ch_enabled[16];

#endif // GUI_MIDI_H
