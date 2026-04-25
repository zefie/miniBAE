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

// X_DebugCallback.c - Debug output callback for BAE_PRINTF
// Provides a hook for GUI applications to capture debug output

#ifdef _DEBUG

#include <stdarg.h>
#include <stdio.h>
#include "X_Assert.h"

// Function pointer for debug output callback
static void (*g_debug_output_callback)(const char *message) = BAE_PRINTF;

// Set the debug output callback (called by GUI on init)
void BAE_SetDebugOutputCallback(void (*callback)(const char *message))
{
    g_debug_output_callback = callback;
}

// Send message to debug output callback
void debug_console_append(const char *message)
{
    if (g_debug_output_callback) {
        g_debug_output_callback(message);
    }
    // If no callback registered, do nothing (messages go to stderr via BAE_STDERR)
}

#endif // _DEBUG
