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

// X_DebugCallback.c - Debug output callback for debug_message
// Provides a hook for GUI applications to capture debug output

#include <stdarg.h>
#include <stdio.h>
#include "X_Assert.h"

// Function pointer for debug output callback
static void (*g_debug_output_callback)(const char *message) = NULL;

// Set the debug output callback (called by GUI on init)
void BAE_SetDebugOutputCallback(void (*callback)(const char *message))
{
    g_debug_output_callback = callback;
}

// Send message to debug output callback
void debug_message(const char *format, ...)
{
    char message[2048];
    va_list args;

    if (!format)
    {
        return;
    }

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (g_debug_output_callback)
    {
        g_debug_output_callback(message);
    }
    else
    {
#if _DEBUG == TRUE
        // Default to stderr if no callback is set (e.g. in non-GUI debug builds)
        BAE_STDERR("%s", message);
#endif
    }
}
