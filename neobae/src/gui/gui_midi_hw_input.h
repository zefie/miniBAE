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

// Lightweight MIDI input wrapper using RtMidi C API

#ifndef MIDI_INPUT_H
#define MIDI_INPUT_H

#include <stdbool.h>

// Initialize MIDI input. Returns true on success.
// client_name: optional display name for virtual port, may be NULL.
// Initialize MIDI input. Returns true on success.
// client_name: optional display name for virtual port, may be NULL.
// api_index: if >=0, attempts to use that RtMidi compiled API (see rtmidi_get_compiled_api). Use -1 for default.
// port_index: if >=0, open that device port for the chosen API. Use -1 to open the first available or virtual port.
bool midi_input_init(const char *client_name, int api_index, int port_index);

// Shutdown MIDI input and free resources.
void midi_input_shutdown(void);

// Poll for a pending MIDI message. Returns true if a message was returned.
// buffer should be at least 3 bytes for short messages; size_out will contain
// the message length (1..1024). timestamp is seconds since some epoch (as double).
bool midi_input_poll(unsigned char *buffer, unsigned int *size_out, double *timestamp);

// Return number of messages that were dropped due to full input queue since init
unsigned int midi_input_drops(void);

#endif // MIDI_INPUT_H
