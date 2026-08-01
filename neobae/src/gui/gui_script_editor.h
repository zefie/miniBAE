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

#ifndef GUI_SCRIPT_EDITOR_H
#define GUI_SCRIPT_EDITOR_H

#if SUPPORT_BAESCRIPT == TRUE

#include <stdbool.h>
#if USE_SDL2 == TRUE
#include <SDL2/SDL_events.h>
#else
#include <SDL3/SDL_events.h>
#endif

// Initialize script editor system
void script_editor_init(void);

// Cleanup script editor system
void script_editor_shutdown(void);

// Toggle script editor window visibility
void script_editor_toggle(void);

// Show/hide script editor window
void script_editor_show(void);
void script_editor_hide(void);

// Check if script editor is visible
bool script_editor_is_visible(void);

// Handle an SDL event (returns true if consumed)
bool script_editor_handle_event(SDL_Event *event);

// Update and render script editor (call from main loop)
void script_editor_render(void);

// Tick the script engine (call from playback loop when enabled)
void script_editor_tick(void);

// Reset exporter-scoped script options before starting an export pass.
void script_editor_reset_exporter_options(void);

// Read exporter.loopcount if set by script; returns true when defined.
bool script_editor_get_exporter_loopcount(int *outLoopCount);

// Check if script processing is enabled
bool script_editor_is_enabled(void);

// Get/set state for settings persistence
const char *script_editor_get_path(void);
const char *script_editor_get_text(void);
bool script_editor_get_enabled(void);
void script_editor_restore_state(const char *path, const char *text, bool enabled);

#endif // SUPPORT_BAESCRIPT
#endif // GUI_SCRIPT_EDITOR_H
