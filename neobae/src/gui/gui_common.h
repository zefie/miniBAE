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

#ifndef GUI_COMMON_H
#define GUI_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include "X_Assert.h"
#include "X_API.h"

#if USE_SDL2 == TRUE
#include <SDL2/SDL.h>
#include "SDL2_Compat.h"
#else
#include <SDL3/SDL.h>
#endif
// Forward declarations for internal types used in meta callback
struct GM_Song;       // opaque
typedef short int16_t; // 16-bit signed used by engine for track index

// Common macros
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

// Window constants
#define WINDOW_W 900
#define WINDOW_BASE_H 320

// Window state globals
extern int g_window_h;
extern bool g_disable_webtv_progress_bar;

// Common types
typedef struct
{
    int x, y, w, h;
} Rect;

typedef struct
{
    int dummy;
} TextCtx; // placeholder if we extend later

// Utility functions
bool point_in(int mx, int my, Rect r);
char *get_absolute_path(const char *path);
void set_status_message(const char *msg);
void safe_strncpy(char *dst, const char *src, size_t size);
#endif // GUI_COMMON_H
