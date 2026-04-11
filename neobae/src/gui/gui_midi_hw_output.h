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

// Lightweight MIDI output wrapper using RtMidi C API

#ifndef MIDI_OUTPUT_H
#define MIDI_OUTPUT_H

#include <stdbool.h>

// Initialize MIDI output. Returns true on success.
// client_name: optional display name for virtual port, may be NULL.
// api_index: if >=0, attempts to use that RtMidi compiled API. Use -1 for default.
// port_index: if >=0, open that device port for the chosen API. Use -1 to open the first available or virtual port.
bool midi_output_init(const char *client_name, int api_index, int port_index);

// Shutdown MIDI output and free resources.
void midi_output_shutdown(void);

// Send a short MIDI message out the opened output device. Returns true on success.
bool midi_output_send(const unsigned char *msg, int len);

// Send All Notes Off / All Sound Off across all 16 MIDI channels. Safe to call
// from any thread when midi_output is initialized.
void midi_output_send_all_notes_off(void);

#endif // MIDI_OUTPUT_H
