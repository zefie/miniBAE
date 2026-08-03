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

/* Ensure POSIX APIs (popen/strcasecmp/realpath) are visible. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "gui_file_open.h"
#include "gui_bae.h"
#include "gui_playlist.h"
#include "X_Assert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if SUPPORT_MIDI_HW == TRUE
#include "gui_midi_hw.h"
#endif
#if SUPPORT_KARAOKE == TRUE
#include "gui_karaoke.h"
#endif
#include "gui_midi.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>
#if USE_SDL2 == TRUE
#include <SDL2/SDL_syswm.h>
#endif

// Single-instance support: mutex name must be stable across runs.
static const char *g_single_instance_mutex_name = "zefidi_single_instance_mutex_v1";
// Previous window proc so we can chain messages we don't handle
static WNDPROC g_prev_wndproc = NULL;

// Helper for EnumWindows: find window with title containing desired substring
struct EnumCtx
{
    const char *want;
    HWND found;
};
static BOOL CALLBACK zefidi_EnumProc(HWND hwnd, LPARAM lparam)
{
    struct EnumCtx *ctx = (struct EnumCtx *)lparam;
    char title[512];
    if (GetWindowTextA(hwnd, title, sizeof(title)) > 0)
    {
        if (strstr(title, ctx->want))
        {
            ctx->found = hwnd;
            return FALSE; // stop enumeration
        }
    }
    return TRUE; // continue
}

// Custom window proc to receive WM_COPYDATA and forward to SDL event queue.
static LRESULT CALLBACK zefidi_WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_COPYDATA)
    {
        PCOPYDATASTRUCT cds = (PCOPYDATASTRUCT)lParam;
        if (cds && cds->lpData && cds->cbData > 0)
        {
            // Allocate a null-terminated copy and push it as an SDL user event.
            char *s = (char *)malloc(cds->cbData + 1);
            if (s)
            {
                memcpy(s, cds->lpData, cds->cbData);
                s[cds->cbData] = '\0';
                SDL_Event ev;
                memset(&ev, 0, sizeof(ev));
#if USE_SDL2 == TRUE
                ev.type = SDL_USEREVENT;
#else
                ev.type = SDL_EVENT_USER;
#endif
                ev.user.code = 1; // code 1 == external file open
                ev.user.data1 = s;
                ev.user.data2 = NULL;
                SDL_PushEvent(&ev);
                // Bring window to foreground and restore if minimized
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                return 1; // handled
            }
        }
    }
    // Chain to previous proc for unhandled messages
    if (g_prev_wndproc)
        return CallWindowProc(g_prev_wndproc, hwnd, uMsg, wParam, lParam);
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

bool gui_file_open_single_instance_or_forward(int argc, char **argv)
{
    HANDLE singleMutex = CreateMutexA(NULL, FALSE, g_single_instance_mutex_name);
    if (singleMutex)
    {
        if (GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (argc > 1)
            {
                const char *pathToSend = argv[1];
                HWND found = NULL;
                const char *want = "zefidi Media Player";
                struct EnumCtx ctx;
                ctx.want = want;
                ctx.found = NULL;
                EnumWindows(zefidi_EnumProc, (LPARAM)&ctx);
                found = ctx.found;
                if (found)
                {
                    COPYDATASTRUCT cds;
                    cds.dwData = 0xBAE1;
                    cds.cbData = (DWORD)(strlen(pathToSend) + 1);
                    cds.lpData = (PVOID)pathToSend;
                    SendMessageA(found, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cds);
                }
            }
            CloseHandle(singleMutex);
            return false; /* exit second instance */
        }
    }
    return true;
}

void gui_file_open_install_wndproc(SDL_Window *win)
{
#if USE_SDL2 == TRUE
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(win, &wmInfo)) {
        HWND hwnd = wmInfo.info.win.window;
#else
    SDL_PropertiesID props = SDL_GetWindowProperties(win);
    if (props) {
        void *hw = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hw) {
            HWND hwnd = (HWND)hw;
#endif
            g_prev_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)zefidi_WndProc);
            BAE_PRINTF("Installed zefidi_WndProc chain (prev=%p)\n", (void *)g_prev_wndproc);
        }
#if USE_SDL2 != TRUE
    }
#endif
}

#else /* !_WIN32 */

bool gui_file_open_single_instance_or_forward(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return true;
}

#endif /* _WIN32 */

static bool path_is_bank_file(const char *ext)
{
    if (!ext)
        return false;
#ifdef _WIN32
    bool is_bank_file = _stricmp(ext, ".hsb") == 0 || _stricmp(ext, ".zsb") == 0;
#if USE_SF2_SUPPORT == TRUE
    if (!is_bank_file)
        is_bank_file = _stricmp(ext, ".sf2") == 0;
#if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0
    if (!is_bank_file)
        is_bank_file = _stricmp(ext, ".sf3") == 0;
    if (!is_bank_file)
        is_bank_file = _stricmp(ext, ".sfo") == 0;
#if _USING_FLUIDLITE == TRUE || USE_NATIVE_DLS == TRUE
    if (!is_bank_file)
        is_bank_file = _stricmp(ext, ".dls") == 0;
#endif
#endif
#endif
#else
    bool is_bank_file = strcasecmp(ext, ".hsb") == 0 || strcasecmp(ext, ".zsb") == 0;
#if USE_SF2_SUPPORT == TRUE
    if (!is_bank_file)
        is_bank_file = strcasecmp(ext, ".sf2") == 0;
#if USE_VORBIS_DECODER == TRUE && SF3_SUPPORT > 0
    if (!is_bank_file)
        is_bank_file = strcasecmp(ext, ".sf3") == 0;
    if (!is_bank_file)
        is_bank_file = strcasecmp(ext, ".sfo") == 0;
#if _USING_FLUIDLITE == TRUE || USE_NATIVE_DLS == TRUE
    if (!is_bank_file)
        is_bank_file = strcasecmp(ext, ".dls") == 0;
#endif
#endif
#endif
#endif
    return is_bank_file;
}

static bool path_is_playlist_file(const char *ext)
{
    if (!ext)
        return false;
#ifdef _WIN32
    return _stricmp(ext, ".m3u") == 0;
#else
    return strcasecmp(ext, ".m3u") == 0;
#endif
}

void gui_file_open_path(const char *path, GuiFileOpenSource src,
                        GuiFileOpenCtx *ctx, int drop_mx, int drop_my)
{
    if (!path || !path[0] || !ctx)
        return;

    const char *ext = strrchr(path, '.');
    bool is_bank_file = path_is_bank_file(ext);
    bool is_playlist_file = path_is_playlist_file(ext);

    if (src == GUI_FILE_OPEN_EXTERNAL_IPC)
        BAE_PRINTF("Received external open request: %s\n", path);
    else
        BAE_PRINTF("Drag and drop: path=%s\n", path);

    if (is_bank_file)
    {
        if (src == GUI_FILE_OPEN_DROP)
            BAE_PRINTF("Drag and drop: Loading bank file: %s\n", path);
        if (load_bank(path, *ctx->playing, ctx->transpose, ctx->tempo, ctx->volume,
                      ctx->loop_enabled, ctx->reverb_type, ctx->ch_enable, true))
        {
            if (src == GUI_FILE_OPEN_EXTERNAL_IPC)
                set_status_message("Loaded bank from external request");
            else
                BAE_PRINTF("Successfully loaded dropped bank: %s\n", path);
        }
        else
        {
            if (src == GUI_FILE_OPEN_EXTERNAL_IPC)
                set_status_message("Failed to load external bank file");
            else {
                BAE_PRINTF("Failed to load dropped bank: %s\n", path);
                set_status_message("Failed to load dropped bank file");
            }
        }
        return;
    }

#if SUPPORT_PLAYLIST == TRUE
    if (src == GUI_FILE_OPEN_DROP && is_playlist_file)
    {
        BAE_PRINTF("Drag and drop: Loading playlist file: %s\n", path);
        playlist_load(path);
        set_status_message("Playlist loaded");
        return;
    }
#else
    (void)is_playlist_file;
#endif

#if SUPPORT_MIDI_HW == TRUE
    if (g_midi_input_enabled)
    {
        if (src == GUI_FILE_OPEN_EXTERNAL_IPC)
        {
            BAE_PRINTF("External open request: MIDI input enabled - ignoring: %s\n", path);
            set_status_message("MIDI input enabled: external open ignored");
        }
        else
        {
            BAE_PRINTF("Drag and drop: MIDI input enabled - ignoring dropped media: %s\n", path);
            set_status_message("MIDI input enabled: media drop ignored");
        }
        return;
    }
#endif

#if SUPPORT_PLAYLIST == TRUE
    if (src == GUI_FILE_OPEN_DROP && g_playlist.enabled)
    {
        int playlistPanelHeight = 300;
        Rect transportPanel_local = {10, 160, 880, 85};
        int keyboardPanelY_local = transportPanel_local.y + transportPanel_local.h + 10;
        Rect keyboardPanel_local = {10, keyboardPanelY_local, 880, 110};
#if SUPPORT_MIDI_HW == TRUE
        bool showKeyboard_local = g_show_virtual_keyboard && (g_midi_input_enabled || (g_bae.song_loaded && !g_bae.is_audio_file));
#else
        bool showKeyboard_local = g_show_virtual_keyboard && (g_bae.song_loaded && !g_bae.is_audio_file);
#endif
        bool showWaveform_local = g_bae.is_audio_file && g_bae.sound;
        if (showWaveform_local)
            showKeyboard_local = false;
#if SUPPORT_KARAOKE == TRUE
        bool showKaraoke_local = g_karaoke_enabled && !g_karaoke_suspended &&
                                (g_lyric_count > 0 || g_karaoke_line_current[0] || g_karaoke_line_previous[0]) &&
                                g_bae.song_loaded && !g_bae.is_audio_file;
        int karaokePanelHeight_local = 40;
#endif
        int statusY_local = ((showKeyboard_local || showWaveform_local) ? (keyboardPanel_local.y + keyboardPanel_local.h + 10) : (transportPanel_local.y + transportPanel_local.h + 10));
#if SUPPORT_KARAOKE == TRUE
        if (showKaraoke_local)
            statusY_local = statusY_local + karaokePanelHeight_local + 5;
#endif
        int playlistPanelY = statusY_local;
        Rect playlistPanel = {10, playlistPanelY, 880, playlistPanelHeight};

        if (point_in(drop_mx, drop_my, playlistPanel))
        {
            BAE_PRINTF("Drag and drop: Adding media file to playlist: %s\n", path);
            playlist_add_file(path);
            set_status_message("File added to playlist");
            return;
        }
    }
#else
    (void)drop_mx;
    (void)drop_my;
#endif

    if (src == GUI_FILE_OPEN_DROP)
        BAE_PRINTF("Drag and drop: Loading media file: %s\n", path);

    if (bae_load_song_with_settings(path, ctx->transpose, ctx->tempo, ctx->volume,
                                    ctx->loop_enabled, ctx->reverb_type, ctx->ch_enable, true))
    {
#if SUPPORT_PLAYLIST == TRUE
        if (g_playlist.enabled) {
            playlist_update_current_file(path);
        }
#endif
        *ctx->duration = bae_get_len_ms();
        *ctx->progress = 0;
        *ctx->playing = false;
        bae_play(ctx->playing);
        if (src == GUI_FILE_OPEN_DROP)
            BAE_PRINTF("Successfully loaded dropped media: %s\n", path);
    }
    else
    {
        if (src == GUI_FILE_OPEN_EXTERNAL_IPC)
            set_status_message("Failed to load external media file");
        else {
            BAE_PRINTF("Failed to load dropped media: %s\n", path);
            set_status_message("Failed to load dropped media file");
        }
    }
}
