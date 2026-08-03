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

#ifndef GUI_FILE_OPEN_H
#define GUI_FILE_OPEN_H

#include "gui_common.h"
#include "NeoBAE.h"
#include <stdbool.h>

typedef struct GuiFileOpenCtx
{
    bool *playing;
    int *progress;
    int *duration;
    int transpose;
    int tempo;
    int volume;
    int reverb_type;
    bool loop_enabled;
    bool *ch_enable; /* BAE_MAX_MIDI_CHANNELS */
} GuiFileOpenCtx;

typedef enum
{
    GUI_FILE_OPEN_EXTERNAL_IPC = 0,
    GUI_FILE_OPEN_DROP = 1
} GuiFileOpenSource;

/* Returns false if this process should exit (second instance forwarded args). */
bool gui_file_open_single_instance_or_forward(int argc, char **argv);

#ifdef _WIN32
void gui_file_open_install_wndproc(SDL_Window *win);
#endif

/* Handle a path from DROPFILE or USEREVENT (external open).
 * For DROP, pass drop_mx/drop_my for playlist hit-testing; ignored for IPC. */
void gui_file_open_path(const char *path, GuiFileOpenSource src,
                        GuiFileOpenCtx *ctx, int drop_mx, int drop_my);

#endif
