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

#ifndef GUI_THEME_H
#define GUI_THEME_H

#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL3/SDL.h>
#endif
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#include <winreg.h>

// Windows theme detection functions
typedef struct
{
    bool is_dark_mode;
    bool is_high_contrast;
    SDL_Color accent_color;
    SDL_Color text_color;
    SDL_Color bg_color;
    SDL_Color panel_bg;
    SDL_Color border_color;
} WindowsTheme;

extern WindowsTheme g_theme;

bool get_registry_dword(HKEY hkey, const char *subkey, const char *value, DWORD *result);
#endif

// Theme globals (used by widgets to pick colors for light/dark modes)
extern bool g_is_dark_mode;
extern SDL_Color g_accent_color;
extern SDL_Color g_text_color;
extern SDL_Color g_bg_color;
extern SDL_Color g_panel_bg;
extern SDL_Color g_panel_border;
extern SDL_Color g_header_color;
extern SDL_Color g_highlight_color;
extern SDL_Color g_button_base;
extern SDL_Color g_button_hover;
extern SDL_Color g_button_press;
extern SDL_Color g_button_text;
extern SDL_Color g_button_border;

// Theme detection and initialization
void detect_windows_theme(void);

#endif // GUI_THEME_H
