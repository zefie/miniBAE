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

// gui_dialogs.c - GUI Dialogs

#include "gui_dialogs.h"
#include "gui_common.h"
#include "gui_widgets.h"
#include "gui_text.h"
#include "gui_theme.h"
#include "gui_panels.h"
#include "gui_bae.h"
#include "gui_settings.h"
#include "NeoBAE.h"
#include "BAE_API.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Dialog state
bool g_show_rmf_info_dialog = false;
bool g_rmf_info_loaded = false;
char g_rmf_info_values[INFO_TYPE_COUNT][512];
char g_rmf_container_version[64];

bool g_show_about_dialog = false;
int g_about_page = 0;
bool g_show_eq_dialog = false;

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

#ifdef SUPPORT_MIDI_HW
#include "rtmidi_c.h"
#include "gui_midi_hw.h"
#endif

// External globals
extern BAEGUI g_bae;
extern int g_window_h;

// Tooltip state
bool g_bank_tooltip_visible = false;
Rect g_bank_tooltip_rect;
char g_bank_tooltip_text[520];

bool g_file_tooltip_visible = false;
Rect g_file_tooltip_rect;
char g_file_tooltip_text[520];

bool g_reverb_tooltip_visible = false;
Rect g_reverb_tooltip_rect;
char g_reverb_tooltip_text[520];

bool g_loop_tooltip_visible = false;
Rect g_loop_tooltip_rect;
char g_loop_tooltip_text[520];

bool g_voice_tooltip_visible = false;
Rect g_voice_tooltip_rect;
char g_voice_tooltip_text[520];

bool g_program_tooltip_visible = false;
Rect g_program_tooltip_rect;
char g_program_tooltip_text[520];

// External references
extern bool g_exporting;
extern bool g_exportDropdownOpen;
extern int g_exportCodecIndex;
extern const char *g_exportCodecNames[];

#ifdef SUPPORT_MIDI_HW
extern bool g_midi_input_enabled;
extern bool g_midi_output_enabled;
extern bool g_midi_input_device_dd_open;
extern bool g_midi_output_device_dd_open;
extern int g_midi_input_device_index;
extern int g_midi_output_device_index;
extern int g_midi_input_device_count;
extern int g_midi_output_device_count;
extern char g_midi_device_name_cache[64][128];
extern bool g_master_muted_for_midi_out;
extern BAESong g_live_song;
#endif

// RMF info functions
const char *rmf_info_label(BAEInfoType t)
{
    switch (t)
    {
    case TITLE_INFO:
        return "Title";
    case PERFORMED_BY_INFO:
        return "Performed By";
    case COMPOSER_INFO:
        return "Composer";
    case COPYRIGHT_INFO:
        return "Copyright";
    case PUBLISHER_CONTACT_INFO:
        return "Publisher";
    case USE_OF_LICENSE_INFO:
        return "Use Of License";
    case LICENSED_TO_URL_INFO:
        return "Licensed URL";
    case LICENSE_TERM_INFO:
        return "License Term";
    case EXPIRATION_DATE_INFO:
        return "Expiration";
    case COMPOSER_NOTES_INFO:
        return "Composer Notes";
    case INDEX_NUMBER_INFO:
        return "Index Number";
    case GENRE_INFO:
        return "Genre";
    case SUB_GENRE_INFO:
        return "Sub-Genre";
    case TEMPO_DESCRIPTION_INFO:
        return "Tempo";
    case ORIGINAL_SOURCE_INFO:
        return "Source";
    default:
        return "Unknown";
    }
}

void rmf_info_reset(void)
{
    for (int i = 0; i < INFO_TYPE_COUNT; i++)
    {
        g_rmf_info_values[i][0] = '\0';
    }
    g_rmf_container_version[0] = '\0';
    g_rmf_info_loaded = false;
}

static void rmf_info_load_container_version(void)
{
    FILE *f;
    unsigned char hdr[12];
    uint32_t mapID;
    uint32_t version;
    const char *kind;

    g_rmf_container_version[0] = '\0';
    if (!g_bae.loaded_path[0])
    {
        return;
    }

    f = fopen(g_bae.loaded_path, "rb");
    if (!f)
    {
        return;
    }
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
    {
        fclose(f);
        return;
    }
    fclose(f);

    mapID = ((uint32_t)hdr[0] << 24) |
            ((uint32_t)hdr[1] << 16) |
            ((uint32_t)hdr[2] << 8) |
            (uint32_t)hdr[3];
    version = ((uint32_t)hdr[4] << 24) |
              ((uint32_t)hdr[5] << 16) |
              ((uint32_t)hdr[6] << 8) |
              (uint32_t)hdr[7];

    if (mapID != XFILERESOURCE_ID && mapID != XFILERESOURCE_ZMF_ID)
    {
        return;
    }

    kind = (version >= 2u) ? "ZMF" : "RMF";
    snprintf(g_rmf_container_version, sizeof(g_rmf_container_version), "%s Version: %u", kind, (unsigned)version);
}

void rmf_info_load_if_needed(void)
{
    if (!g_bae.is_rmf_file || !g_bae.song_loaded)
        return;
    if (g_rmf_info_loaded)
        return;
    // Iterate all known info types, fetch if fits
    for (int i = 0; i < INFO_TYPE_COUNT; i++)
    {
        BAEInfoType it = (BAEInfoType)i;
        char buf[512];
        buf[0] = '\0';
        if (BAEUtil_GetRmfSongInfoFromFile((BAEPathName)g_bae.loaded_path, 0, it, buf, sizeof(buf)) == BAE_NO_ERROR)
        {
            // Only store if non-empty and printable
            if (buf[0] != '\0')
            {
                safe_strncpy(g_rmf_info_values[i], buf, sizeof(g_rmf_info_values[i]));
            }
        }
    }
    rmf_info_load_container_version();
    g_rmf_info_loaded = true;
}

// Platform file dialog abstraction
char *open_file_dialog(void)
{
#ifdef _WIN32
/* Compile-time built extension list, Windows Style*/
static const char AUDIO_EXT_FILTER[] =    
#if USE_FLAC_DECODER == TRUE
    "*.flac;"
#else
    ""
#endif
#if USE_MPEG_DECODER == TRUE
    "*.mp2;*.mp3;"
#else
    ""
#endif
#if USE_VORBIS_DECODER == TRUE && SUPPORT_OGG_FORMAT == TRUE
    "*.ogg;"
#else
    ""
#endif
#if USE_OPUS_DECODER == TRUE && SUPPORT_OGG_FORMAT == TRUE
    "*.opus;"
#else
    ""
#endif
#if USE_XMF_SUPPORT == TRUE && _USING_FLUIDSYNTH == TRUE
    "*.xmf;*.mxmf;"
#else
    ""
#endif
#if USE_RETRO_RINGTONE_SUPPORT == TRUE
    "*.imy;*.rng;*.rtx;"
#else
    ""    
#endif
#if USE_QOA_SUPPORT == TRUE
    "*.qoa;"
#else
    ""
#endif
#if USE_ZMF_SUPPORT == TRUE
    "*.zmf;"
#else
    ""
#endif
    "*.wav;*.aif;*.aiff;*.au;";
    char fileBuf[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    /* Build a proper Windows multi-string filter using compiled-in extensions.
       We must provide sequences of: DisplayName\0Pattern\0...\0 (double-null terminated).
       AUDIO_EXT_FILTER here contains semicolon-separated patterns (e.g. "*.flac;*.mp3;...")
       so we insert it into the appropriate pattern slots. */
    char filterBuf[1024];
    char *p = filterBuf;
    size_t rem = sizeof(filterBuf);

    #define APPEND_STR(s) do { size_t _l = strlen(s); if (_l + 1 > rem) break; memcpy(p, (s), _l); p += _l; *p++ = '\0'; rem -= (_l + 1); } while(0)

    APPEND_STR("All Supported");
    {
        char pattern[512];
        /* include the always-available patterns plus the compiled-in ones */
        snprintf(pattern, sizeof(pattern), "*.mid;*.midi;*.kar;*.rmi;*.rmf;%s", AUDIO_EXT_FILTER);
        /* Remove possible duplicate separators if AUDIO_EXT_FILTER is empty */
        APPEND_STR(pattern);
    }
    APPEND_STR("MIDI Files"); APPEND_STR("*.mid;*.midi;*.kar;*.rmi");
#if USE_XMF_SUPPORT == TRUE && _USING_FLUIDSYNTH == TRUE
    APPEND_STR("XMF Files"); APPEND_STR("*.xmf;*.mxmf");
#endif
    APPEND_STR("RMF Files"); APPEND_STR("*.rmf");
#if USE_ZMF_SUPPORT == TRUE
    APPEND_STR("ZMF Files"); APPEND_STR("*.zmf");
#endif    
    APPEND_STR("Audio Files");
    {
        char audioPattern[512];
        snprintf(audioPattern, sizeof(audioPattern), "%s", AUDIO_EXT_FILTER);
        APPEND_STR(audioPattern);
    }
    APPEND_STR("All Files"); APPEND_STR("*.*");
    /* double-null terminate */
    if (rem > 0)
        *p++ = '\0';
    ofn.lpstrFilter = filterBuf;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn))
    {
        size_t len = strlen(fileBuf);
        char *ret = (char *)malloc(len + 1);
        if (ret)
        {
            memcpy(ret, fileBuf, len + 1);
        }
        return ret;
    }
    return NULL;
#elif defined(__APPLE__)
    static const char AUDIO_TYPE_LIST[] =
        "\"mid\", \"midi\", \"kar\", \"rmi\", \"rmf\", \"zmf\", \"imy\", \"rng\", \"rtx\""
#if USE_FLAC_DECODER == TRUE
        ", \"flac\""
#endif
#if USE_MPEG_DECODER == TRUE
        ", \"mp2\", \"mp3\""
#endif
#if USE_VORBIS_DECODER == TRUE && SUPPORT_OGG_FORMAT == TRUE
        ", \"ogg\""
#endif
#if USE_OPUS_DECODER == TRUE && SUPPORT_OGG_FORMAT == TRUE
        ", \"opus\""
#endif
#if USE_XMF_SUPPORT == TRUE && _USING_FLUIDSYNTH == TRUE
        ", \"xmf\", \"mxmf\""
#endif
        ", \"wav\", \"aif\", \"aiff\", \"au\"";
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'POSIX path of (choose file with prompt \"Open Media File\" of type {%s})' 2>/dev/null",
        AUDIO_TYPE_LIST);
    FILE *fp = popen(cmd, "r");
    if (fp)
    {
        char buf[1024];
        if (fgets(buf, sizeof(buf), fp))
        {
            pclose(fp);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                    memcpy(ret, buf, l + 1);
                return ret;
            }
        }
        else
            pclose(fp);
    }
    return NULL;
#else
/* Compile-time built extension list, Linux Style */
static const char AUDIO_EXT_FILTER[] =
    "*.mid *.midi *.kar *.rmi *.rmf *.imy *.rng *.rtx "
#if USE_ZMF_SUPPORT == TRUE
    "*.zmf "
#else
    ""
#endif    
#if USE_FLAC_DECODER == TRUE
    "*.flac "
#else
    ""
#endif
#if USE_MPEG_DECODER == TRUE
    "*.mp2 *.mp3 "
#else
    ""
#endif
#if USE_VORBIS_DECODER == TRUE && SUPPORT_OGG_FORMAT == TRUE
    "*.ogg "
#else
    ""
#endif
#if USE_OPUS_DECODER == TRUE && SUPPORT_OGG_FORMAT == TRUE
    "*.opus  "
#else
    ""
#endif
#if USE_XMF_SUPPORT == TRUE && _USING_FLUIDSYNTH == TRUE
    "*.xmf *.mxmf";
#else
    ""
#endif
    "*.wav *.aif *.aiff *.au";

     /* Build desktop file-chooser commands using the compiled-in extension list.
         AUDIO_EXT_FILTER on Unix is a space-separated list like "*.flac *.mp3 ...". */
     char cmd_zenity[1024];
     char cmd_kdialog[1024];
     snprintf(cmd_zenity, sizeof(cmd_zenity), "zenity --file-selection --title='Open Media File' --file-filter='Supported Files | %s' --file-filter='All Files | *' 2>/dev/null", AUDIO_EXT_FILTER);
     snprintf(cmd_kdialog, sizeof(cmd_kdialog), "kdialog --getopenfilename . '%s' 2>/dev/null", AUDIO_EXT_FILTER);
     const char *cmds[] = { cmd_zenity, cmd_kdialog, "yad --file-selection --title='Open Media File' 2>/dev/null", NULL };
    for (int i = 0; cmds[i]; ++i)
    {
        FILE *p = popen(cmds[i], "r");
        if (!p)
            continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p))
        {
            pclose(p);
            // strip newline
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                {
                    memcpy(ret, buf, l + 1);
                }
                return ret;
            }
        }
        else
        {
            pclose(p);
        }
    }
    BAE_PRINTF("No GUI file chooser available (zenity/kdialog/yad). Drag & drop still works for media and bank files.\n");
    return NULL;
#endif
}


#if SUPPORT_PLAYLIST == TRUE
char *open_playlist_dialog(void)
{
#ifdef _WIN32
    char fileBuf[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "M3U Playlist Files\0*.m3u;*.m3u8\0"
                      "All Files\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn))
    {
        size_t len = strlen(fileBuf);
        char *ret = (char *)malloc(len + 1);
        if (ret)
        {
            memcpy(ret, fileBuf, len + 1);
        }
        return ret;
    }
    return NULL;
#elif defined(__APPLE__)
    FILE *fp = popen("osascript -e 'POSIX path of (choose file with prompt \"Open Playlist File\" of type {\"m3u\", \"m3u8\"})' 2>/dev/null", "r");
    if (fp)
    {
        char buf[1024];
        if (fgets(buf, sizeof(buf), fp))
        {
            pclose(fp);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                    memcpy(ret, buf, l + 1);
                return ret;
            }
        }
        else
            pclose(fp);
    }
    return NULL;
#else
    const char *cmds[] = {
        "zenity --file-selection --title='Open Playlist File' --file-filter='M3U Playlist Files | *.m3u *.m3u8' --file-filter='All Files | *' 2>/dev/null",
        "kdialog --getopenfilename . '*.m3u *.m3u8' 2>/dev/null",
        "yad --file-selection --title='Open Playlist File' 2>/dev/null",
        NULL};
    for (int i = 0; cmds[i]; ++i)
    {
        FILE *p = popen(cmds[i], "r");
        if (!p)
            continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p))
        {
            pclose(p);
            // strip newline
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                {
                    memcpy(ret, buf, l + 1);
                }
                return ret;
            }
        }
        pclose(p);
    }
    BAE_PRINTF("No GUI file chooser available (zenity/kdialog/yad). Drag & drop still works for playlist files.\n");
    return NULL;
#endif
}

char *save_playlist_dialog(void)
{
#ifdef _WIN32
    char fileBuf[MAX_PATH] = {0};
    strcpy(fileBuf, "playlist.m3u"); // Default filename
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "M3U Playlist Files\0*.m3u;*.m3u8\0"
                      "All Files\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrDefExt = "m3u";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameA(&ofn))
    {
        size_t len = strlen(fileBuf);
        char *ret = (char *)malloc(len + 1);
        if (ret)
        {
            memcpy(ret, fileBuf, len + 1);
        }
        return ret;
    }
    return NULL;
#elif defined(__APPLE__)
    FILE *fp = popen("osascript -e 'POSIX path of (choose file name with prompt \"Save Playlist As\" default name \"playlist.m3u\")' 2>/dev/null", "r");
    if (fp)
    {
        char buf[1024];
        if (fgets(buf, sizeof(buf), fp))
        {
            pclose(fp);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                    memcpy(ret, buf, l + 1);
                return ret;
            }
        }
        else
            pclose(fp);
    }
    // Fallback to default filename
    {
        char *ret = (char *)malloc(strlen("playlist.m3u") + 1);
        if (ret)
            strcpy(ret, "playlist.m3u");
        return ret;
    }
#else
    const char *cmds[] = {
        "zenity --file-selection --save --confirm-overwrite --title='Save Playlist As' --filename='playlist.m3u' --file-filter='M3U Playlist Files | *.m3u *.m3u8' --file-filter='All Files | *' 2>/dev/null",
        "kdialog --getsavefilename 'playlist.m3u' '*.m3u *.m3u8' 2>/dev/null",
        "yad --file-selection --save --confirm-overwrite --title='Save Playlist As' --filename='playlist.m3u' 2>/dev/null",
        NULL};
    for (int i = 0; cmds[i]; ++i)
    {
        FILE *p = popen(cmds[i], "r");
        if (!p)
            continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p))
        {
            pclose(p);
            // strip newline
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                {
                    memcpy(ret, buf, l + 1);
                }
                return ret;
            }
        }
        else
        {
            pclose(p);
        }
    }
    BAE_PRINTF("No GUI file chooser available (zenity/kdialog/yad). Using default filename 'playlist.m3u'.\n");
    // Fallback to default filename
    char *ret = (char *)malloc(strlen("playlist.m3u") + 1);
    if (ret)
    {
        strcpy(ret, "playlist.m3u");
    }
    return ret;
#endif
}
#endif // SUPPORT_PLAYLIST

char *open_neoreverb_dialog(void)
{
#ifdef _WIN32
    char fileBuf[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "Neo Reverb Preset Files\0*.neoreverb;*.neoreverb.xml\0"
                      "All Files\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn))
    {
        size_t len = strlen(fileBuf);
        char *ret = (char *)malloc(len + 1);
        if (ret)
        {
            memcpy(ret, fileBuf, len + 1);
        }
        return ret;
    }
    return NULL;
#elif defined(__APPLE__)
    FILE *fp = popen("osascript -e 'POSIX path of (choose file with prompt \"Import Neo Reverb Preset\" of type {\"neoreverb\"})' 2>/dev/null", "r");
    if (fp)
    {
        char buf[1024];
        if (fgets(buf, sizeof(buf), fp))
        {
            pclose(fp);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                    memcpy(ret, buf, l + 1);
                return ret;
            }
        }
        else
            pclose(fp);
    }
    return NULL;
#else
    const char *cmds[] = {
        "zenity --file-selection --title='Import Neo Reverb Preset' --file-filter='Neo Reverb Preset | *.neoreverb *.neoreverb.xml' --file-filter='All Files | *' 2>/dev/null",
        "kdialog --getopenfilename . '*.neoreverb *.neoreverb.xml' 2>/dev/null",
        "yad --file-selection --title='Import Neo Reverb Preset' 2>/dev/null",
        NULL};
    for (int i = 0; cmds[i]; ++i)
    {
        FILE *p = popen(cmds[i], "r");
        if (!p)
            continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p))
        {
            pclose(p);
            // strip newline
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                {
                    memcpy(ret, buf, l + 1);
                }
                return ret;
            }
        }
        pclose(p);
    }
    BAE_PRINTF("No GUI file chooser available (zenity/kdialog/yad).\n");
    return NULL;
#endif
}

char *save_neoreverb_dialog(const char *default_name)
{
#ifdef _WIN32
    char fileBuf[MAX_PATH] = {0};
    if (default_name && default_name[0])
    {
        safe_strncpy(fileBuf, default_name, sizeof(fileBuf));
    }
    else
    {
        strcpy(fileBuf, "preset.neoreverb");
    }
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "Neo Reverb Preset Files\0*.neoreverb;*.neoreverb.xml\0"
                      "All Files\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrDefExt = "neoreverb";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameA(&ofn))
    {
        size_t len = strlen(fileBuf);
        char *ret = (char *)malloc(len + 1);
        if (ret)
        {
            memcpy(ret, fileBuf, len + 1);
        }
        return ret;
    }
    return NULL;
#elif defined(__APPLE__)
    char fname[256];
    if (default_name && default_name[0])
        safe_strncpy(fname, default_name, sizeof(fname));
    else
        strcpy(fname, "preset.neoreverb");
    // Sanitize characters that would break the AppleScript string quoting
    for (size_t i = 0; fname[i]; i++)
    {
        if (fname[i] == '\'' || fname[i] == '"' || fname[i] == '\\')
            fname[i] = '_';
    }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'POSIX path of (choose file name with prompt \"Export Neo Reverb Preset\" default name \"%s\")' 2>/dev/null",
        fname);
    FILE *fp = popen(cmd, "r");
    if (fp)
    {
        char buf[1024];
        if (fgets(buf, sizeof(buf), fp))
        {
            pclose(fp);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                    memcpy(ret, buf, l + 1);
                return ret;
            }
        }
        else
            pclose(fp);
    }
    return NULL;
#else
    char fname[256];
    if (default_name && default_name[0])
        safe_strncpy(fname, default_name, sizeof(fname));
    else
        strcpy(fname, "preset.neoreverb");

    // These commands use single quotes; ensure fname doesn't contain single quotes.
    for (size_t i = 0; fname[i]; i++)
    {
        if (fname[i] == '\'')
            fname[i] = '_';
    }

    char cmd_zenity[1024];
    char cmd_kdialog[1024];
    char cmd_yad[1024];
    snprintf(cmd_zenity, sizeof(cmd_zenity),
             "zenity --file-selection --save --confirm-overwrite --title='Export Neo Reverb Preset' --filename='%s' --file-filter='Neo Reverb Preset | *.neoreverb *.neoreverb.xml' --file-filter='All Files | *' 2>/dev/null",
             fname);
    snprintf(cmd_kdialog, sizeof(cmd_kdialog), "kdialog --getsavefilename '%s' '*.neoreverb *.neoreverb.xml' 2>/dev/null", fname);
    snprintf(cmd_yad, sizeof(cmd_yad),
             "yad --file-selection --save --confirm-overwrite --title='Export Neo Reverb Preset' --filename='%s' 2>/dev/null",
             fname);
    const char *cmds[] = {cmd_zenity, cmd_kdialog, cmd_yad, NULL};

    for (int i = 0; cmds[i]; ++i)
    {
        FILE *p = popen(cmds[i], "r");
        if (!p)
            continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p))
        {
            pclose(p);
            // strip newline
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                {
                    memcpy(ret, buf, l + 1);
                }
                return ret;
            }
        }
        else
        {
            pclose(p);
        }
    }
    BAE_PRINTF("No GUI file chooser available (zenity/kdialog/yad).\n");
    return NULL;
#endif
}

#if SUPPORT_PLAYLIST == TRUE
char *open_folder_dialog(void)
{
#ifdef _WIN32
    char folderBuf[1024] = {0};
    BROWSEINFOA bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = NULL;
    bi.lpszTitle = "Select Folder to Add All Media Files";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl)
    {
        if (SHGetPathFromIDListA(pidl, folderBuf))
        {
            size_t len = strlen(folderBuf);
            char *ret = (char *)malloc(len + 1);
            if (ret)
            {
                memcpy(ret, folderBuf, len + 1);
            }
            // Free the pidl
            CoTaskMemFree(pidl);
            return ret;
        }
        CoTaskMemFree(pidl);
    }
    return NULL;
#elif defined(__APPLE__)
    FILE *fp = popen("osascript -e 'POSIX path of (choose folder with prompt \"Select Folder to Add All Media Files\")' 2>/dev/null", "r");
    if (fp)
    {
        char buf[1024];
        if (fgets(buf, sizeof(buf), fp))
        {
            pclose(fp);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                    memcpy(ret, buf, l + 1);
                return ret;
            }
        }
        else
            pclose(fp);
    }
    return NULL;
#else
    const char *cmds[] = {
        "zenity --file-selection --directory --title='Select Folder to Add All Media Files' 2>/dev/null",
        "kdialog --getexistingdirectory . 2>/dev/null",
        "yad --file-selection --directory --title='Select Folder to Add All Media Files' 2>/dev/null",
        NULL};
    for (int i = 0; cmds[i]; ++i)
    {
        FILE *p = popen(cmds[i], "r");
        if (!p)
            continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p))
        {
            pclose(p);
            // strip newline
            size_t l = strlen(buf);
            while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
                buf[--l] = '\0';
            if (l > 0)
            {
                char *ret = (char *)malloc(l + 1);
                if (ret)
                {
                    memcpy(ret, buf, l + 1);
                }
                return ret;
            }
        }
        pclose(p);
    }
    BAE_PRINTF("No GUI folder chooser available (zenity/kdialog/yad). Drag & drop individual files still works.\n");
    return NULL;
#endif
}
#endif

// RMF Info dialog rendering
void render_rmf_info_dialog(SDL_Renderer *R, int mx, int my, bool mclick)
{
    if (!g_show_rmf_info_dialog || !g_bae.is_rmf_file)
        return;

    // Dim entire background first (drawn before dialog contents)
    SDL_Color dim = g_is_dark_mode ? (SDL_Color){0, 0, 0, 120} : (SDL_Color){0, 0, 0, 90};
    draw_rect(R, (Rect){0, 0, WINDOW_W, g_window_h}, dim);

    rmf_info_load_if_needed();

    int pad = 8;
    int lineH = 16;
    // Determine inner content width needed so the longest metadata line does not wrap (within limits)
    int minOuterW = 340; // previous base width
    int maxOuterW = WINDOW_W - 20;
    if (maxOuterW < minOuterW)
        maxOuterW = minOuterW; // keep on-screen
    int longestInner = 0;      // width without wrapping (text only)
    // Measure title too so dialog is never narrower than it
    int titleW = 0, titleH = 0;
    measure_text("RMF Metadata", &titleW, &titleH);
    if (titleW > longestInner)
        longestInner = titleW;
    if (g_rmf_container_version[0])
    {
        int w = 0, h = 0;
        measure_text(g_rmf_container_version, &w, &h);
        if (w > longestInner)
            longestInner = w;
    }
    for (int i = 0; i < INFO_TYPE_COUNT; i++)
    {
        if (g_rmf_info_values[i][0])
        {
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s: %s", rmf_info_label((BAEInfoType)i), g_rmf_info_values[i]);
            int w = 0, h = 0;
            measure_text(tmp, &w, &h);
            if (w > longestInner)
                longestInner = w;
        }
    }
    // Convert inner width (text) to outer dialog width used by existing wrapping helpers
    // inner width passed to draw_wrapped_text is (dlgW - pad*2 - 8)
    int desiredOuterW = longestInner + pad * 2 + 8;
    if (desiredOuterW < minOuterW)
        desiredOuterW = minOuterW;
    if (desiredOuterW > maxOuterW)
        desiredOuterW = maxOuterW;
    int dlgW = desiredOuterW;

    // Now compute total wrapped lines for chosen width
    int totalLines = 0;
    if (g_rmf_container_version[0])
    {
        int count = count_wrapped_lines(g_rmf_container_version, dlgW - pad * 2 - 8);
        if (count <= 0)
            count = 1;
        totalLines += count;
    }
    for (int i = 0; i < INFO_TYPE_COUNT; i++)
    {
        if (g_rmf_info_values[i][0])
        {
            char tmp[1024];
            snprintf(tmp, sizeof(tmp), "%s: %s", rmf_info_label((BAEInfoType)i), g_rmf_info_values[i]);
            int count = count_wrapped_lines(tmp, dlgW - pad * 2 - 8);
            if (count <= 0)
                count = 1;
            totalLines += count;
        }
    }
    if (totalLines == 0)
        totalLines = 1;                                // placeholder
    int dlgH = pad * 2 + 24 + totalLines * lineH + 10; // title + fields

    // If dialog would exceed window height, attempt one more widening (if possible) to reduce wrapping
    if (dlgH > g_window_h - 20 && dlgW < maxOuterW)
    {
        int extra = maxOuterW - dlgW; // try expanding to max
        int newDlgW = dlgW + extra;
        int newTotalLines = 0;
        for (int i = 0; i < INFO_TYPE_COUNT; i++)
        {
            if (g_rmf_info_values[i][0])
            {
                char tmp[1024];
                snprintf(tmp, sizeof(tmp), "%s: %s", rmf_info_label((BAEInfoType)i), g_rmf_info_values[i]);
                int count = count_wrapped_lines(tmp, newDlgW - pad * 2 - 8);
                if (count <= 0)
                    count = 1;
                newTotalLines += count;
            }
        }
        int newDlgH = pad * 2 + 24 + newTotalLines * lineH + 10;
        if (newDlgH < dlgH)
        { // only adopt if improves
            dlgW = newDlgW;
            totalLines = newTotalLines;
            dlgH = newDlgH;
        }
    }

    Rect dlg = {WINDOW_W - dlgW - 10, 10, dlgW, dlgH};
    // Theme-aware dialog background and border (keep slight translucency)
    SDL_Color dlgBg = g_panel_bg;
    dlgBg.a = 230;
    SDL_Color dlgBorder = g_panel_border;
    draw_rect(R, dlg, dlgBg);
    draw_frame(R, dlg, dlgBorder);

    // Title uses header color
    draw_text(R, dlg.x + 10, dlg.y + 8, "RMF Metadata", g_header_color);

    // Close button (simple X) styled with button colors so it fits theme
    Rect closeBtn = {dlg.x + dlg.w - 22, dlg.y + 6, 16, 16};
    bool overClose = point_in(mx, my, closeBtn);
    SDL_Color cbg = overClose ? g_button_hover : g_button_base;
    draw_rect(R, closeBtn, cbg);
    draw_frame(R, closeBtn, g_button_border);
    // Nudge X up ~3px for better visual alignment
    draw_text(R, closeBtn.x + 4, closeBtn.y - 1, "X", g_button_text);
    if (mclick && overClose)
    {
        g_show_rmf_info_dialog = false;
    }

    // Render wrapped fields
    int y = dlg.y + 32;
    int rendered = 0;
    if (g_rmf_container_version[0])
    {
        int drawn = draw_wrapped_text(R,
                                      dlg.x + 10,
                                      y,
                                      g_rmf_container_version,
                                      g_text_color,
                                      dlgW - pad * 2 - 8,
                                      lineH);
        y += drawn * lineH;
        rendered += drawn;
    }
    for (int i = 0; i < INFO_TYPE_COUNT; i++)
    {
        if (g_rmf_info_values[i][0])
        {
            char full[1024];
            snprintf(full, sizeof(full), "%s: %s", rmf_info_label((BAEInfoType)i), g_rmf_info_values[i]);
            // Use theme text color for wrapped fields
            int drawn = draw_wrapped_text(R, dlg.x + 10, y, full, g_text_color, dlgW - pad * 2 - 8, lineH);
            y += drawn * lineH;
            rendered += drawn;
        }
    }
    if (rendered == 0)
    {
        SDL_Color placeholder = g_is_dark_mode ? (SDL_Color){160, 160, 170, 255} : (SDL_Color){100, 100, 100, 255};
        draw_text(R, dlg.x + 10, y, "(No metadata fields present)", placeholder);
    }

    // Clicking outside dialog (and not on its opener button) closes it.
    // NOTE: The RMF Info button was repositioned in gui_main.c to x=798; keep this in sync.
    // Ideally this would be passed in, but for now we mirror the hard-coded location.
    Rect rmfOpener = {798, 215, 80, 22};
    if (mclick && !point_in(mx, my, dlg) && !point_in(mx, my, rmfOpener))
    {
        g_show_rmf_info_dialog = false;
    }
}

// About dialog rendering
void render_about_dialog(SDL_Renderer *R, int mx, int my, bool mclick)
{
    if (!g_show_about_dialog)
        return;

    SDL_Color dim = g_is_dark_mode ? (SDL_Color){0, 0, 0, 120} : (SDL_Color){0, 0, 0, 90};
    draw_rect(R, (Rect){0, 0, WINDOW_W, g_window_h}, dim);
    int dlgW = 560;
    int dlgH = 380;
    int pad = 10;
    Rect dlg = {(WINDOW_W - dlgW) / 2, (g_window_h - dlgH) / 2, dlgW, dlgH};
    SDL_Color dlgBg = g_panel_bg;
    dlgBg.a = 240;
    draw_rect(R, dlg, dlgBg);
    draw_frame(R, dlg, g_panel_border);
    draw_text(R, dlg.x + pad, dlg.y + 8, "About", g_header_color);
    // Close X (slightly larger for better hit/visibility)
    Rect closeBtn = {dlg.x + dlg.w - 22, dlg.y + 6, 16, 16};
    bool overClose = point_in(mx, my, closeBtn);
    draw_rect(R, closeBtn, overClose ? g_button_hover : g_button_base);
    draw_frame(R, closeBtn, g_button_border);
    // Nudge X up ~3px for better visual alignment
    draw_text(R, closeBtn.x + 4, closeBtn.y - 1, "X", g_button_text);
    if (mclick && overClose)
    {
        g_show_about_dialog = false;
    }

    // About dialog content: paged. Page 0 = main info, Page 1 = credits/licenses
    // Navigation controls drawn bottom-right
    const char *cpuArch = BAE_GetCurrentCPUArchitecture();
    const char *baeFeatures = BAE_GetFeatureString();
    char *baeVersion = (char *)BAE_GetVersion();   /* malloc'd by engine */
    char *compInfo = (char *)BAE_GetCompileInfo(); /* malloc'd by engine */

    char line1[256];
    if (baeVersion && cpuArch)
        snprintf(line1, sizeof(line1), "zefidi Media Player (%s) %s", cpuArch, baeVersion);
    else if (baeVersion)
        snprintf(line1, sizeof(line1), "zefidi Media Player %s", baeVersion);
    else if (cpuArch)
        snprintf(line1, sizeof(line1), "zefidi Media Player (%s)", cpuArch);
    else
        snprintf(line1, sizeof(line1), "zefidi Media Player");

    char line2[256];
    if (compInfo && compInfo[0])
        snprintf(line2, sizeof(line2), "built with %s", compInfo);
    else
        line2[0] = '\0';

    char line3[512];
    if (baeFeatures && baeFeatures[0]) {
        snprintf(line3, sizeof(line3), "features: %s", baeFeatures);
    }
    else
        line3[0] = '\0';

    int y = dlg.y + 40;
    // Page 0: main info
    if (g_about_page == 0)
    {
        // Make version text clickable and link to GitHub (commit or tree)
        int vw = 0, vh = 0;
        measure_text(line1, &vw, &vh);
        Rect verLinkRect = {dlg.x + pad, y, vw, vh > 0 ? vh : 14};
        bool overVerLink = point_in(mx, my, verLinkRect);
        SDL_Color verLinkCol = overVerLink ? g_accent_color : g_text_color;
        draw_text(R, verLinkRect.x, verLinkRect.y, line1, verLinkCol);
        if (overVerLink)
        {
            SDL_SetRenderDrawColor(R, verLinkCol.r, verLinkCol.g, verLinkCol.b, verLinkCol.a);
#if defined(USE_SDL2)
            SDL_RenderDrawLine(R, verLinkRect.x, verLinkRect.y + verLinkRect.h - 2, verLinkRect.x + verLinkRect.w, verLinkRect.y + verLinkRect.h - 2);
#else
            SDL_RenderLine(R, verLinkRect.x, verLinkRect.y + verLinkRect.h - 2, verLinkRect.x + verLinkRect.w, verLinkRect.y + verLinkRect.h - 2);
#endif
        }
        if (mclick && overVerLink)
        {
            const char *raw = baeVersion ? baeVersion : _VERSION;
            char url[256];
            url[0] = '\0';
            if (strstr(raw, "-dirty"))
            {
                size_t len = strlen(raw);
                if (len > 6)
                {
                    size_t copylen = (len > 6) ? len - 6 : 0;
                    if (copylen >= sizeof(url)) copylen = sizeof(url) - 1;
                    safe_strncpy(url, raw, copylen);
                    url[copylen] = '\0';
                }
                else
                {
                    snprintf(url, sizeof(url), "%s", raw);
                }
            }
            else
            {
                snprintf(url, sizeof(url), "%s", raw);
            }
            if (strncmp(raw, "git-", 4) == 0)
            {
                const char *sha = raw + 4;
                char shortSha[64];
                int i = 0;
                while (sha[i] && sha[i] != '-' && i < (int)sizeof(shortSha) - 1)
                {
                    shortSha[i] = sha[i];
                    i++;
                }
                shortSha[i] = '\0';
                snprintf(url, sizeof(url), "https://github.com/zefie/NeoBAE/commit/%s", shortSha);
            }
            else if (strncmp(raw, "built", 5) != 0 && !strstr(raw, "dirty"))
            {
                snprintf(url, sizeof(url), "https://github.com/zefie/NeoBAE/tree/%s", raw);
            }

            if (url[0])
            {
#ifdef _WIN32
                ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
                char cmd[512];
                snprintf(cmd, sizeof(cmd), "(xdg-open '%s' || open '%s') >/dev/null 2>&1 &", url, url);
                system(cmd);
#endif
            }
        }
        y += 20;
        if (line2[0])
        {
            draw_text(R, dlg.x + pad, y, line2, g_text_color);
            y += 20;
        }
        // Insert features line as third line on About page (with wrapping)
        if (line3[0])
        {
            int wrapWidth = dlgW - pad * 2 - 8;
            int featureLineH = 18; // line height for wrapped features
            int wrapCount = count_wrapped_lines(line3, wrapWidth);
            if (wrapCount <= 0)
                wrapCount = 1;
            draw_wrapped_text(R, dlg.x + pad, y, line3, g_text_color, wrapWidth, featureLineH);
            y += wrapCount * featureLineH;
        }
        draw_text(R, dlg.x + pad, y, "", g_text_color); /* spacer */
        y += dlg.h - (y - dlg.y) - 78;          // move down to near bottom for links
#if defined(USE_SDL2)
        SDL_RendererInfo sdl2_renderer_info;
        const char *renderer_name = (SDL_GetRendererInfo(R, &sdl2_renderer_info) == 0) ? sdl2_renderer_info.name : NULL;
#else
        const char *renderer_name = SDL_GetRendererName(R);
#endif
        if (renderer_name && renderer_name[0]) {
            char renderer[128];
            snprintf(renderer, sizeof(renderer), "SDL Graphics Layer: %s", renderer_name);
            draw_text(R, dlg.x + pad, y, renderer, g_text_color);
            y += 18;
        }
        draw_text(R, dlg.x + pad, y, "(C) 2021-2026 Zefie Networks", g_text_color);
        y += 18;
        const char *urls[] = {"https://www.soundmusicsys.com/", "https://github.com/zefie/NeoBAE/", NULL};
        for (int i = 0; urls[i]; ++i)
        {
            const char *u = urls[i];
            int tw = 0, th = 0;
            measure_text(u, &tw, &th);
            Rect r = {dlg.x + pad, y, tw, th > 0 ? th : 14};
            bool over = point_in(mx, my, r);
            SDL_Color col = over ? g_accent_color : g_highlight_color;
            draw_text(R, r.x, r.y, u, col);
            if (over)
            {
                SDL_SetRenderDrawColor(R, col.r, col.g, col.b, col.a);
#if defined(USE_SDL2)
                SDL_RenderDrawLine(R, r.x, r.y + r.h - 2, r.x + r.w, r.y + r.h - 2);
#else
                SDL_RenderLine(R, r.x, r.y + r.h - 2, r.x + r.w, r.y + r.h - 2);
#endif
            }
            if (mclick && over)
            {
                if (strncmp(u, "http", 4) == 0)
                {
#ifdef _WIN32
                    ShellExecuteA(NULL, "open", u, NULL, NULL, SW_SHOWNORMAL);
#else
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd), "(xdg-open '%s' || open '%s') >/dev/null 2>&1 &", u, u);
                    system(cmd);
#endif
                }
            }
            y += 18;
        }
    }
    // Page 1: credits/licenses (part 1)
    else if (g_about_page == 1)
    {
        draw_text(R, dlg.x + pad, y, "This software makes use of the following software:", g_text_color);
        y += 18;
        const char *credits_page1[] = {
            // NeoBAE is obviously required
            "",
            "NeoBAE",
            "Copyright (c) 2026 Zefie Networks",
            "Based on miniBAE, Copyright (c) 2009 Beatnik, Inc.",
            "Original miniBAE source code available at:",
            "https://github.com/heyigor/miniBAE/",
            // SDL is also required for this GUI so no #ifdef is necessary
#if defined(USE_SDL2)            
            "",
            "SDL2 & SDL2_ttf",
#elif X_PLATFORM == X_SDL3
            "",
            "SDL3 & SDL3_ttf",
#endif
#if defined(USE_SDL2) || X_PLATFORM == X_SDL3
            "Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>",
            "https://www.libsdl.org/",
#endif      
#if USE_MPEG_DECODER == TRUE
            "",
            "minimp3",
            "Licensed under the CC0",
            "http://creativecommons.org/publicdomain/zero/1.0/",
#endif
#if USE_XMF_SUPPORT == TRUE
            "",
            "zlib",
            "Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler",
            "http://www.zlib.net/",
#endif
            "",
            NULL};
        for (int i = 0; credits_page1[i]; ++i)
        {
            const char *txt = credits_page1[i];
            if (strncmp(txt, "http", 4) == 0)
            {
                int tw = 0, th = 0;
                measure_text(txt, &tw, &th);
                Rect r = {dlg.x + pad + 8, y, tw, th > 0 ? th : 14};
                bool over = point_in(mx, my, r);
                SDL_Color col = over ? g_accent_color : g_highlight_color;
                draw_text(R, r.x, r.y, txt, col);
                if (over)
                {
                    SDL_SetRenderDrawColor(R, col.r, col.g, col.b, col.a);
#if defined(USE_SDL2)
                    SDL_RenderDrawLine(R, r.x, r.y + r.h - 2, r.x + r.w, r.y + r.h - 2);
#else
                    SDL_RenderLine(R, r.x, r.y + r.h - 2, r.x + r.w, r.y + r.h - 2);
#endif
                }
                if (mclick && over)
                {
#ifdef _WIN32
                    ShellExecuteA(NULL, "open", txt, NULL, NULL, SW_SHOWNORMAL);
#else
                    char cmd[512];
                    snprintf(cmd, sizeof(cmd), "(xdg-open '%s' || open '%s') >/dev/null 2>&1 &", txt, txt);
                    system(cmd);
#endif
                }
            }
            else
            {
                draw_text(R, dlg.x + pad + 8, y, txt, g_text_color);
            }
            y += 16;
            if (y > dlg.y + dlg.h - 36)
                break;
        }
    }

#if (USE_MPEG_DECODER == TRUE) || (USE_MPEG_ENCODER == TRUE) || (SUPPORT_MIDI_HW == TRUE) || (SUPPORT_OGG_FORMAT == TRUE) || (USE_VORBIS_DECODER == TRUE) || (USE_VORBIS_ENCODER == TRUE) || (USE_FLAC_DECODER == TRUE) || (USE_FLAC_ENCODER == TRUE) || (_USING_FLUIDSYNTH == TRUE)
    // Page 2 & 3: credits/licenses (part 2)
    else if (g_about_page == 2 || g_about_page == 3 || g_about_page == 4)
    {
        const char *credits_page2[] = {
#if USE_MPEG_ENCODER == TRUE
            "",
            "libmp3lame",
            "https://lame.sourceforge.io/",
#endif            
#if SUPPORT_MIDI_HW == TRUE
            "",
            "RtMidi: realtime MIDI i/o C++ classes",
            "Copyright (c) 2003-2023 Gary P. Scavone",
            "https://github.com/thestk/rtmidi",
#endif
#if SUPPORT_OGG_FORMAT == TRUE
            "",
            "libogg",
            "Copyright (c) 2002, Xiph.org Foundation",
            "https://www.xiph.org/ogg/",
#endif
#if (USE_VORBIS_DECODER == TRUE) || (USE_VORBIS_ENCODER == TRUE)
            "",
            "libvorbis",
            "Copyright (c) 2002-2020 Xiph.org Foundation",
            "https://www.xiph.org/vorbis/",
#endif
#if (USE_OPUS_DECODER == TRUE) || (USE_OPUS_ENCODER == TRUE)
            "",
            "libopus",
            "Copyright (c) 2007-2026 Xiph.org Foundation",
            "https://www.xiph.org/opus/",
#endif
#if (USE_FLAC_DECODER == TRUE) || (USE_FLAC_ENCODER == TRUE)
            "",
            "libFLAC",
            "Copyright (C) 2000-2009 Josh Coalson, (C) 2011-2025 Xiph.Org Foundation",
            "https://www.xiph.org/flac/",
#endif
#if _USING_FLUIDSYNTH == TRUE
            "",
            "FluidSynth",
            "Copyright (C) 2004-2026 FluidSynth Team",
            "https://www.fluidsynth.org/",
#endif
#if USE_QOA_SUPPORT == TRUE
            "",
            "Quite OK Audio Codec (QOA)",
            "Copyright (C) 2023 Dominic Szablewski",
            "https://github.com/phoboslab/qoa",
#endif
#if USE_ADP_SUPPORT == TRUE
            "",
            "libg722",
            "Copyright (c) 2014-2025 Sippy Software, Inc.",
            "http://www.sippysoft.com",
#endif
#if USE_LZMA_COMPRESSION == TRUE
             "",
             "LZMA SDK",
             "Copyright (C) 1999-2026 Igor Pavlov",
             "https://www.7-zip.org/sdk.html",
#endif

            "",
            NULL
        };
        
        // Check if credits_page2 has any meaningful content (not just empty strings)
        bool has_credits_content = false;
        for (int i = 0; credits_page2[i]; ++i)
        {
            if (credits_page2[i][0] != '\0')
            {
                has_credits_content = true;
                break;
            }
        }
        
        // Only render if there's actual content
        if (has_credits_content)
        {
            // Calculate how many items fit on page 2
            int page2_max_items = 0;
            int temp_y = dlg.y + 40;
            for (int i = 0; credits_page2[i]; ++i)
            {
                temp_y += 16;
                if (temp_y > dlg.y + dlg.h - 36)
                {
                    page2_max_items = i;
                    break;
                }
            }
            
            // If we're on page 2, render up to page2_max_items
            // If we're on page 3, render from page2_max_items onward
            int start_index = (g_about_page == 2) ? 0 : page2_max_items;
            int end_index = (g_about_page == 2) ? page2_max_items : -1;
            
            for (int i = start_index; credits_page2[i] && (end_index == -1 || i < end_index); ++i)
            {
                const char *txt = credits_page2[i];
                if (strncmp(txt, "http", 4) == 0)
                {
                    int tw = 0, th = 0;
                    measure_text(txt, &tw, &th);
                    Rect r = {dlg.x + pad + 8, y, tw, th > 0 ? th : 14};
                    bool over = point_in(mx, my, r);
                    SDL_Color col = over ? g_accent_color : g_highlight_color;
                    draw_text(R, r.x, r.y, txt, col);
                    if (over)
                    {
                        SDL_SetRenderDrawColor(R, col.r, col.g, col.b, col.a);
#if defined(USE_SDL2)
                        SDL_RenderDrawLine(R, r.x, r.y + r.h - 2, r.x + r.w, r.y + r.h - 2);
#else
                        SDL_RenderLine(R, r.x, r.y + r.h - 2, r.x + r.w, r.y + r.h - 2);
#endif
                    }
                    if (mclick && over)
                    {
#ifdef _WIN32
                        ShellExecuteA(NULL, "open", txt, NULL, NULL, SW_SHOWNORMAL);
#else
                        char cmd[512];
                        snprintf(cmd, sizeof(cmd), "(xdg-open '%s' || open '%s') >/dev/null 2>&1 &", txt, txt);
                        system(cmd);
#endif
                    }
                }
                else
                {
                    draw_text(R, dlg.x + pad + 8, y, txt, g_text_color);
                }
                y += 16;
                if (y > dlg.y + dlg.h - 36)
                    break;
            }
        }
    }
#endif
    // Page navigation controls (bottom-right)
    // Calculate max pages dynamically based on available features and content overflow
    int max_pages = 2; // Always have pages 0 and 1
#if (USE_MPEG_DECODER == TRUE) || (USE_MPEG_ENCODER == TRUE) || (SUPPORT_MIDI_HW == TRUE) || (SUPPORT_OGG_FORMAT == TRUE) || (USE_VORBIS_DECODER == TRUE) || (USE_VORBIS_ENCODER == TRUE) || (USE_FLAC_DECODER == TRUE) || (USE_FLAC_ENCODER == TRUE)
    // Check if page 2 content would overflow and require page 3
    const char *credits_page2[] = {
#if USE_MPEG_DECODER == TRUE
        "", "minimp3", "Licensed under the CC0", "http://creativecommons.org/publicdomain/zero/1.0/",
#endif
#if SUPPORT_MIDI_HW == TRUE
        "", "RtMidi: realtime MIDI i/o C++ classes", "Copyright (c) 2003-2023 Gary P. Scavone", "https://github.com/thestk/rtmidi",
#endif
#if SUPPORT_OGG_FORMAT == TRUE
        "", "libogg", "Copyright (c) 2002, Xiph.org Foundation", "https://www.xiph.org/ogg/",
#endif
#if (USE_VORBIS_DECODER == TRUE) || (USE_VORBIS_ENCODER == TRUE)
        "", "libvorbis", "Copyright (c) 2002-2020 Xiph.org Foundation", "https://www.xiph.org/vorbis/",
#endif
#if (USE_OPUS_DECODER == TRUE) || (USE_OPUS_ENCODER == TRUE)
        "", "libopus", "Copyright (c) 2007-2026 Xiph.org Foundation", "https://www.xiph.org/opus/",
#endif
#if (USE_FLAC_DECODER == TRUE) || (USE_FLAC_ENCODER == TRUE)
        "", "libFLAC", "Copyright (C) 2000-2009  Josh Coalson", "Copyright (C) 2011-2025  Xiph.Org Foundation", "https://www.xiph.org/flac/",
#endif
        "", NULL};
    
    // Check if credits_page2 has any meaningful content (not just empty strings)
    bool has_credits_content = false;
    for (int i = 0; credits_page2[i]; ++i)
    {
        if (credits_page2[i][0] != '\0')
        {
            has_credits_content = true;
            break;
        }
    }
    
    if (has_credits_content)
    {
        max_pages = 3; // Add page 2 if features are available and have content
        
        // Simulate rendering to see if all items fit on page 2
        int temp_y = dlg.y + 40;
        bool page3_needed = false;
        for (int i = 0; credits_page2[i]; ++i)
        {
            temp_y += 16;
            if (temp_y > dlg.y + dlg.h - 36)
            {
                // Check if there are more items after this one
                if (credits_page2[i + 1] != NULL)
                {
                    page3_needed = true;
                }
                break;
            }
        }
        
        if (page3_needed)
        {
            max_pages = 4; // Add page 3 if content overflows
        }
    }
#endif
    
    Rect navPrev = {dlg.x + dlg.w - 70, dlg.y + dlg.h - 34, 24, 20};
    Rect navNext = {dlg.x + dlg.w - 34, dlg.y + dlg.h - 34, 24, 20};
    bool overPrev = point_in(mx, my, navPrev);
    bool overNext = point_in(mx, my, navNext);
    draw_rect(R, navPrev, overPrev ? g_button_hover : g_button_base);
    draw_frame(R, navPrev, g_button_border);
    draw_text(R, navPrev.x + 6, navPrev.y, "<", g_button_text);
    draw_rect(R, navNext, overNext ? g_button_hover : g_button_base);
    draw_frame(R, navNext, g_button_border);
    draw_text(R, navNext.x + 6, navNext.y, ">", g_button_text);
    // Page indicator
    char pg[32];
    snprintf(pg, sizeof(pg), "%d / %d", g_about_page + 1, max_pages);
    int pw = 0, ph = 0;
    measure_text(pg, &pw, &ph);
    draw_text(R, dlg.x + dlg.w - 100 - pw / 2, dlg.y + dlg.h - 32, pg, g_text_color);
    if (mclick)
    {
        if (overPrev && g_about_page > 0)
        {
            g_about_page--;
        }
        else if (overNext && g_about_page < (max_pages - 1))
        {
            g_about_page++;
        }
    }

    if (baeVersion)
        free(baeVersion);
    if (compInfo)
        free(compInfo);

    // Note: deliberately do NOT close About dialog when clicking outside to
    // avoid immediate close when the About button (outside the dialog) is clicked.
}

void render_eq_dialog(SDL_Renderer *R, int mx, int my, bool mclick, bool mdown, int window_h)
{
    if (!g_show_eq_dialog)
        return;

    extern bool g_show_preset_name_dialog;
    extern bool g_show_preset_delete_confirm_dialog;
    bool sub_dialog_active = g_show_preset_name_dialog || g_show_preset_delete_confirm_dialog;

    int input_mx = sub_dialog_active ? -1 : mx;
    int input_my = sub_dialog_active ? -1 : my;
    bool input_mclick = sub_dialog_active ? false : mclick;
    bool input_mdown = sub_dialog_active ? false : mdown;

    static bool initialized = false;
    static bool preset_dropdown_open = false;

    const int eq_preset_gains[7][5] = {
        {0, 0, 0, 0, 0},        // Flat
        {8, 4, 0, 0, 0},        // Bass Boost
        {4, 2, 0, 2, 4},        // Acoustic
        {6, 2, -2, 2, 6},       // Rock
        {-2, 4, 6, 4, -2},      // Pop
        {6, 4, 0, 2, 4},        // Classical
        {-4, 0, 6, 4, -2}       // Vocal
    };

    if (!initialized && g_bae.mixer)
    {
        BAE_BOOL eq_on = FALSE;
        BAEMixer_GetEQEnabled(g_bae.mixer, &eq_on);
        g_eq_enabled = eq_on ? true : false;

        for (int i = 0; i < 5; i++)
        {
            float g = 0.0f;
            BAEMixer_GetEQGain(g_bae.mixer, i, &g);
            g_eq_gains[i] = g;
        }

        if (g_selected_eq_preset < 0 || g_selected_eq_preset >= get_eq_preset_count())
        {
            g_selected_eq_preset = 7; // Custom
        }
        initialized = true;
    }

    // Dim background
    SDL_Color dim = g_is_dark_mode ? (SDL_Color){0, 0, 0, 160} : (SDL_Color){0, 0, 0, 120};
    draw_rect(R, (Rect){0, 0, WINDOW_W, window_h}, dim);

    // Dialog dimensions
    int dlgW = 400;
    int dlgH = 370;
    int pad = 10;
    Rect dlg = {(WINDOW_W - dlgW) / 2, (window_h - dlgH) / 2, dlgW, dlgH};

    SDL_Color dlgBg = g_panel_bg;
    dlgBg.a = 250;
    SDL_Color dlgFrame = g_panel_border;

    draw_rect(R, dlg, dlgBg);
    draw_frame(R, dlg, dlgFrame);

    // Title
    draw_text(R, dlg.x + pad, dlg.y + 8, "Equalizer Settings", g_header_color);

    // Close button
    Rect closeBtn = {dlg.x + dlg.w - 22, dlg.y + 6, 16, 16};
    bool overClose = point_in(input_mx, input_my, closeBtn);
    SDL_Color close_bg = overClose ? g_button_hover : g_button_base;
    SDL_Color close_txt = g_button_text;
    if (sub_dialog_active)
    {
        close_bg.a = 120;
        close_txt.a = 120;
    }
    draw_rect(R, closeBtn, close_bg);
    draw_frame(R, closeBtn, g_button_border);
    draw_text(R, closeBtn.x + 4, closeBtn.y - 1, "X", close_txt);

    if (input_mclick && overClose)
    {
        g_show_eq_dialog = false;
        initialized = false;
        preset_dropdown_open = false;
        return;
    }

    // Enable/Disable EQ toggle
    bool old_enabled = g_eq_enabled;
    ui_toggle(R, (Rect){dlg.x + 20, dlg.y + 48, 18, 18}, &g_eq_enabled, "Enable EQ", input_mx, input_my, input_mclick);
    if (g_eq_enabled != old_enabled)
    {
        if (g_bae.mixer)
        {
            BAEMixer_SetEQEnabled(g_bae.mixer, g_eq_enabled ? TRUE : FALSE);
            save_settings(g_current_bank_path[0] ? g_current_bank_path : NULL, g_bae.current_reverb_type, g_bae.loop_enabled_gui);
        }
    }

    // Preset dropdown and save/delete buttons
    Rect dropdownRect = {dlg.x + dlg.w - 180, dlg.y + 45, 110, 24};
    SDL_Color lblCol = g_eq_enabled ? g_text_color : (SDL_Color){g_text_color.r, g_text_color.g, g_text_color.b, 120};
    draw_text(R, dropdownRect.x - 60, dropdownRect.y + 4, "Preset:", lblCol);

    Rect saveBtn = {dlg.x + dlg.w - 65, dlg.y + 47, 20, 20};
    Rect deleteBtn = {dlg.x + dlg.w - 40, dlg.y + 47, 20, 20};

    bool overSave = g_eq_enabled && !preset_dropdown_open && point_in(input_mx, input_my, saveBtn);
    bool overDelete = g_eq_enabled && !preset_dropdown_open && point_in(input_mx, input_my, deleteBtn);
    bool can_delete = (g_selected_eq_preset >= 8); // Only custom presets (8+) can be deleted

    SDL_Color save_bg = overSave ? g_button_hover : g_button_base;
    SDL_Color delete_bg = (overDelete && can_delete) ? g_button_hover : g_button_base;

    if (!g_eq_enabled)
    {
        save_bg.a = 120;
        delete_bg.a = 120;
    }
    else if (!can_delete)
    {
        delete_bg.a = 120;
    }

    draw_rect(R, saveBtn, save_bg);
    draw_frame(R, saveBtn, g_button_border);
    SDL_Color btn_txt = g_button_text;
    if (!g_eq_enabled) btn_txt.a = 120;
    draw_text(R, saveBtn.x + 5, saveBtn.y + 2, "+", btn_txt);

    draw_rect(R, deleteBtn, delete_bg);
    draw_frame(R, deleteBtn, g_button_border);
    SDL_Color del_txt = g_button_text;
    if (!g_eq_enabled || !can_delete) del_txt.a = 120;
    draw_text(R, deleteBtn.x + 6, deleteBtn.y + 2, "-", del_txt);

    if (input_mclick && g_eq_enabled && !preset_dropdown_open)
    {
        if (overSave)
        {
            extern bool g_show_preset_name_dialog;
            extern char g_preset_name_input[64];
            extern int g_preset_name_cursor;
            
            g_preset_dialog_is_eq = true;
            if (g_selected_eq_preset >= 8)
            {
                const char *current_name = get_eq_preset_name(g_selected_eq_preset);
                if (current_name)
                {
                    safe_strncpy(g_preset_name_input, current_name, sizeof(g_preset_name_input) - 1);
                    g_preset_name_cursor = strlen(g_preset_name_input);
                }
            }
            else
            {
                memset(g_preset_name_input, 0, sizeof(g_preset_name_input));
                g_preset_name_cursor = 0;
            }
            g_show_preset_name_dialog = true;
        }
        else if (overDelete && can_delete)
        {
            extern bool g_show_preset_delete_confirm_dialog;
            extern char g_preset_delete_name[64];
            
            g_preset_dialog_is_eq = true;
            g_show_preset_delete_confirm_dialog = true;
            const char *preset_name = get_eq_preset_name(g_selected_eq_preset);
            safe_strncpy(g_preset_delete_name, preset_name, sizeof(g_preset_delete_name) - 1);
            g_preset_delete_name[sizeof(g_preset_delete_name) - 1] = '\0';
        }
    }

    int old_preset = g_selected_eq_preset;
    // Disable preset dropdown selection if EQ is disabled
    int dd_mx = g_eq_enabled ? input_mx : -1;
    int dd_my = g_eq_enabled ? input_my : -1;
    bool dd_mdown = g_eq_enabled ? input_mdown : false;
    bool dd_mclick = g_eq_enabled ? input_mclick : false;

    // Sliders layout
    const char *band_labels[] = {
        "100 Hz (Bass):",
        "300 Hz (Low Mid):",
        "1 kHz (Mid):",
        "3 kHz (High Mid):",
        "10 kHz (Treble):"
    };

    int startY = dlg.y + 90;
    int labelX = dlg.x + 20;
    int sliderX = dlg.x + 150;
    int sliderW = dlg.w - 170 - 60;
    int sliderH = 14;

    int check_mx = g_eq_enabled && !preset_dropdown_open ? input_mx : -1;
    int check_my = g_eq_enabled && !preset_dropdown_open ? input_my : -1;
    bool check_mdown = g_eq_enabled && !preset_dropdown_open ? input_mdown : false;
    bool check_mclick = g_eq_enabled && !preset_dropdown_open ? input_mclick : false;

    for (int i = 0; i < 5; i++)
    {
        int y = startY + i * 50;

        // Label
        draw_text(R, labelX, y + 2, band_labels[i], lblCol);

        // Slider
        Rect sliderRect = {sliderX, y, sliderW, sliderH};
        int val_int = (int)g_eq_gains[i];
        int old_val = val_int;
        ui_slider(R, sliderRect, &val_int, -12, 12, check_mx, check_my, check_mdown, check_mclick);
        g_eq_gains[i] = (float)val_int;

        if (val_int != old_val)
        {
            if (g_bae.mixer)
            {
                BAEMixer_SetEQGain(g_bae.mixer, i, g_eq_gains[i]);
            }
            g_selected_eq_preset = 7; // Custom
            g_current_custom_eq_preset[0] = '\0';
            save_settings(g_current_bank_path[0] ? g_current_bank_path : NULL, g_bae.current_reverb_type, g_bae.loop_enabled_gui);
        }

        // Value text
        char valBuf[32];
        snprintf(valBuf, sizeof(valBuf), "%+d dB", val_int);
        draw_text(R, sliderX + sliderW + 8, y + 2, valBuf, lblCol);
    }

    // Render dropdown last so it draws on top of sliders
    int total_presets = get_eq_preset_count();
    const char **eq_preset_names = (const char **)malloc(sizeof(char *) * (size_t)total_presets);
    if (eq_preset_names)
    {
        for (int i = 0; i < total_presets; i++)
        {
            eq_preset_names[i] = get_eq_preset_name(i);
        }

        bool dropdown_changed = ui_dropdown(R, dropdownRect, &g_selected_eq_preset, eq_preset_names, total_presets, &preset_dropdown_open, dd_mx, dd_my, dd_mdown, dd_mclick);
        if (dropdown_changed && g_selected_eq_preset != old_preset)
        {
            if (g_selected_eq_preset < 7)
            {
                for (int i = 0; i < 5; i++)
                {
                    g_eq_gains[i] = (float)eq_preset_gains[g_selected_eq_preset][i];
                    if (g_bae.mixer)
                    {
                        BAEMixer_SetEQGain(g_bae.mixer, i, g_eq_gains[i]);
                    }
                }
                g_current_custom_eq_preset[0] = '\0';
                save_settings(g_current_bank_path[0] ? g_current_bank_path : NULL, g_bae.current_reverb_type, g_bae.loop_enabled_gui);
            }
            else if (g_selected_eq_preset == 7)
            {
                g_current_custom_eq_preset[0] = '\0';
                save_settings(g_current_bank_path[0] ? g_current_bank_path : NULL, g_bae.current_reverb_type, g_bae.loop_enabled_gui);
            }
            else
            {
                int custom_idx = g_selected_eq_preset - 8;
                if (custom_idx >= 0 && custom_idx < g_custom_eq_preset_count)
                {
                    load_custom_eq_preset(g_custom_eq_presets[custom_idx].name);
                }
                save_settings(g_current_bank_path[0] ? g_current_bank_path : NULL, g_bae.current_reverb_type, g_bae.loop_enabled_gui);
            }
        }
        free(eq_preset_names);
    }

    if (sub_dialog_active)
    {
        SDL_Color sub_dim = {0, 0, 0, 160};
        draw_rect(R, dlg, sub_dim);
        draw_frame(R, dlg, g_panel_border);
    }
}

// Dialog initialization
void dialogs_init(void)
{
    g_show_rmf_info_dialog = false;
    g_rmf_info_loaded = false;
    g_show_about_dialog = false;
    g_about_page = 0;
    g_show_eq_dialog = false;
    ui_clear_tooltip(&g_bank_tooltip_visible);
    ui_clear_tooltip(&g_file_tooltip_visible);
    ui_clear_tooltip(&g_loop_tooltip_visible);
    ui_clear_tooltip(&g_program_tooltip_visible);
    ui_clear_tooltip(&g_bank_tooltip_visible);
}

// Dialog cleanup
void dialogs_cleanup(void)
{
    g_show_rmf_info_dialog = false;
    g_show_about_dialog = false;
    g_show_eq_dialog = false;
    ui_clear_tooltip(&g_bank_tooltip_visible);
    ui_clear_tooltip(&g_file_tooltip_visible);
    ui_clear_tooltip(&g_loop_tooltip_visible);
    ui_clear_tooltip(&g_program_tooltip_visible);
    ui_clear_tooltip(&g_bank_tooltip_visible);
}

int get_eq_preset_count(void)
{
    extern int g_custom_eq_preset_count;
    return 8 + g_custom_eq_preset_count;
}

const char *get_eq_preset_name(int idx)
{
    static const char *eq_standard_names[] = {
        "Flat", "Bass Boost", "Acoustic", "Rock", "Pop", "Classical", "Vocal", "Custom"
    };
    if (idx < 0) return "?";
    if (idx < 8) return eq_standard_names[idx];
    
    extern int g_custom_eq_preset_count;
    extern CustomEQPreset *g_custom_eq_presets;
    if (idx < 8 + g_custom_eq_preset_count && g_custom_eq_presets)
    {
        return g_custom_eq_presets[idx - 8].name;
    }
    return "?";
}
