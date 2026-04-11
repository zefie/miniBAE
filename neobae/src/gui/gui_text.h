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

#ifndef GUI_TEXT_H
#define GUI_TEXT_H

#include "gui_common.h"
#if defined(USE_SDL2)
#include <SDL2/SDL_ttf.h>
#else
#include <SDL3_ttf/SDL_ttf.h>
#endif

// Font management
extern TTF_Font *g_font;
extern int g_bitmap_font_scale;

// Text rendering functions
void gui_set_font_scale(int scale);
void measure_text(const char *text, int *w, int *h);
void draw_text(SDL_Renderer *R, int x, int y, const char *text, SDL_Color col);

// Word wrapping utilities
int count_wrapped_lines(const char *text, int max_w);
int draw_wrapped_text(SDL_Renderer *R, int x, int y, const char *text, SDL_Color col, int max_w, int lineH);

// Bitmap font fallback
void bitmap_draw(SDL_Renderer *R, int x, int y, const char *text, SDL_Color col);

#endif // GUI_TEXT_H
