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

#ifndef SDL2_COMPAT_H
#define SDL2_COMPAT_H

// SDL2 compatibility shims for SDL3 API differences.
// Include this after SDL headers when building with USE_SDL2.

#if defined(USE_SDL2)

// SDL3 uses uppercase single-letter SDLK constants (e.g. SDLK_A).
// SDL2 uses lowercase (e.g. SDLK_a). Map them here.
#define SDLK_A SDLK_a
#define SDLK_B SDLK_b
#define SDLK_C SDLK_c
#define SDLK_D SDLK_d
#define SDLK_E SDLK_e
#define SDLK_F SDLK_f
#define SDLK_G SDLK_g
#define SDLK_H SDLK_h
#define SDLK_I SDLK_i
#define SDLK_J SDLK_j
#define SDLK_K SDLK_k
#define SDLK_L SDLK_l
#define SDLK_M SDLK_m
#define SDLK_N SDLK_n
#define SDLK_O SDLK_o
#define SDLK_P SDLK_p
#define SDLK_Q SDLK_q
#define SDLK_R SDLK_r
#define SDLK_S SDLK_s
#define SDLK_T SDLK_t
#define SDLK_U SDLK_u
#define SDLK_V SDLK_v
#define SDLK_W SDLK_w
#define SDLK_X SDLK_x
#define SDLK_Y SDLK_y
#define SDLK_Z SDLK_z

// SDL3 uses SDL_Mutex (capital M); SDL2 uses SDL_mutex (lowercase).
// Provide a transparent alias so code can use SDL_Mutex in both.
#define SDL_Mutex SDL_mutex

#endif // USE_SDL2

#endif // SDL2_COMPAT_H
