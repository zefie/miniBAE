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

#ifndef GUI_DEBUG_CONSOLE_H
#define GUI_DEBUG_CONSOLE_H

#ifdef _DEBUG

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_events.h>

// Initialize debug console system
void debug_console_init(void);

// Cleanup debug console system
void debug_console_shutdown(void);

// Note: debug_message() is provided by X_DebugCallback.c in the library
// The GUI registers its internal implementation via BAE_SetDebugOutputCallback()

// Toggle debug console window visibility
void debug_console_toggle(void);

// Show/hide debug console window
void debug_console_show(void);
void debug_console_hide(void);

// Check if debug console is visible
bool debug_console_is_visible(void);

// Handle an event (returns true if consumed, false if should be passed to main window)
bool debug_console_handle_event(SDL_Event *event);

// Update and render debug console (call from main loop after handling events)
void debug_console_render(void);

#endif // _DEBUG

#endif // GUI_DEBUG_CONSOLE_H
