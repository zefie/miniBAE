/*
 * © 2021–2026 zefie
 *
 * zefidi2 — slim Dear ImGui + SDL3 NeoBAE media player.
 */

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "zefidi2_theme.inc"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include "NeoBAE.h"
#include "NeoBAEConfigPath.h"
#include "X_API.h"
#include "GenPriv.h"
#include "GenSnd.h"
#if USE_XMF_SUPPORT == TRUE
#include "GenXMF.h"
#endif

#ifndef SUPPORT_MIDI_HW
#define SUPPORT_MIDI_HW 0
#endif
#ifndef SUPPORT_KARAOKE
#define SUPPORT_KARAOKE 0
#endif
#ifndef SUPPORT_PLAYLIST
#define SUPPORT_PLAYLIST 0
#endif
#ifndef SUPPORT_BAESCRIPT
#define SUPPORT_BAESCRIPT 0
#endif

#if SUPPORT_MIDI_HW == TRUE
#include "gui_midi_hw_input.h"
#endif
#if SUPPORT_BAESCRIPT == TRUE
#include "baescript.h"
#endif

#if defined(ZEFIDI2_EMBED_FONTS)
#include "zefidi2_embedded_fonts.h"
#endif

#ifndef _VERSION
#define _VERSION "unknown"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

enum class DialogAction
{
    OpenMedia = 0,
    OpenBank,
    AddPlaylist,
    ExportSave,
};

class Zefidi2App
{
public:
    SDL_Window *m_main_window = nullptr;
    SDL_Renderer *m_renderer = nullptr;
    SDL_Mutex *m_dialog_mutex = nullptr;

    bool m_bae_initialized = false;
    bool m_want_quit = false;

    BAEMixer m_mixer = nullptr;
    BAESong m_song = nullptr;
    BAESong m_live_song = nullptr;
    BAESound m_sound = nullptr;
    BAEBankToken m_bank_token = 0;

    bool m_song_loaded = false;
    bool m_is_audio_file = false;
    bool m_is_rmf_file = false;
    bool m_song_started = false;
    bool m_playing = false;

    int m_sample_rate_hz = 44100;
    bool m_stereo = true;
    int m_midi_voices = 64;
    int m_volume_percent = 100;
    int m_tempo_percent = 100;
    int m_transpose = 0;
    int m_reverb_type = static_cast<int>(BAE_REVERB_TYPE_1);
    int m_velocity_curve = 1;
    bool m_loop_enabled = false;
    bool m_webtv_style_bar = false;
    bool m_show_keyboard = true;
    bool m_normalize_playback = false;
    bool m_eq_enabled = false;
    std::array<float, 5> m_eq_gains = {};

    int m_custom_comb_count = 4;
    int m_custom_comb_delay[8] = {29, 37, 43, 53, 61, 67, 73, 79};
    int m_custom_comb_feedback[8] = {90, 88, 86, 84, 82, 80, 78, 76};
    int m_custom_comb_gain[8] = {100, 95, 90, 85, 80, 75, 70, 65};
    int m_custom_lowpass = 80;

    std::array<bool, 16> m_channel_muted = {};
    int m_channel_solo = -1;
    std::array<float, 16> m_channel_vu = {};
    uint32_t m_channel_vu_last_tick_ms = 0;
    std::array<float, 128> m_scope_history = {};
    int m_scope_write_index = 0;
    float m_master_vu_l = 0.0f;
    float m_master_vu_r = 0.0f;
    float m_seek_fraction = 0.0f;

    std::string m_status = "Ready";
    std::string m_loaded_media_path;
    std::string m_song_title;
    std::string m_loaded_bank_path;
    std::string m_loaded_bank_display;
    std::string m_settings_last_bank;

    bool m_settings_open = false;
    bool m_about_open = false;
    bool m_rmf_info_open = false;
    bool m_eq_open = false;
    bool m_custom_reverb_open = false;
    bool m_export_open = false;
    bool m_script_open = false;

    bool m_exporting = false;
    bool m_export_cancel = false;
    int m_export_progress = 0;
    int m_export_format = 0;

    int m_keyboard_octave = 4;          /* PC-keyboard transpose (C of octave) */
    int m_keyboard_channel = 0;         /* 0..15 — selected channel (classic zefidi) */
    int m_keyboard_program = 0;         /* 0..127 — live display / edit */
    int m_keyboard_bank = 0;            /* 0..127 — live display / edit */
    bool m_keyboard_prog_dirty = false; /* user editing bank/prog; pause engine sync briefly */
    uint32_t m_keyboard_prog_hold_until_ms = 0;
    std::array<bool, 128> m_keyboard_note_on = {}; /* local mouse / PC keys held */
    std::array<bool, 128> m_keyboard_midi_lit = {}; /* hardware MIDI notes on selected ch */

#if SUPPORT_PLAYLIST == TRUE
    bool m_playlist_enabled = true;
    bool m_playlist_shuffle = false;
    int m_playlist_repeat = 0; /* 0 none, 1 all, 2 track */
    std::vector<std::string> m_playlist;
    std::vector<int> m_playlist_order;
    int m_playlist_index = -1;
#else
    bool m_playlist_enabled = false;
#endif

#if SUPPORT_KARAOKE == TRUE
    bool m_karaoke_enabled = true;
    std::string m_karaoke_line;
    std::string m_karaoke_prev;
    std::string m_karaoke_current;
#else
    bool m_karaoke_enabled = false;
#endif

#if SUPPORT_MIDI_HW == TRUE
    bool m_midi_in_enabled = false;
    bool m_midi_in_active = false;
    int m_midi_in_port = -1;
#else
    bool m_midi_in_enabled = false;
    int m_midi_in_port = -1;
#endif

#if SUPPORT_BAESCRIPT == TRUE
    bool m_script_enabled = false;
    bool m_script_dirty = true;
    BAEScript_Context *m_script_ctx = nullptr;
    char m_script_text_buf[65536] = {};
    std::string m_script_status;
    std::string m_script_console;
#else
    bool m_script_enabled = false;
#endif

    DialogAction m_pending_dialog_action = DialogAction::OpenMedia;
    bool m_dialog_result_ready = false;
    bool m_dialog_error = false;
    std::string m_dialog_path;
    std::string m_dialog_error_message;

    void SetMainWindow(SDL_Window *w)
    {
        m_main_window = w;
    }
    void SetRenderer(SDL_Renderer *r)
    {
        m_renderer = r;
    }
    void RequestQuit()
    {
        m_want_quit = true;
    }
    bool WantsQuit() const
    {
        return m_want_quit;
    }

    bool Init()
    {
        m_dialog_mutex = SDL_CreateMutex();
        if (!m_dialog_mutex)
        {
            m_status = "SDL_CreateMutex failed";
            return false;
        }
        (void)BAE_Setup();
        m_bae_initialized = true;
        LoadSettings();
#if SUPPORT_BAESCRIPT == TRUE
        LoadScriptFromDisk();
#endif
        if (!CreateMixer())
        {
            return false;
        }
        (void)TryLoadDefaultBank();
#if SUPPORT_PLAYLIST == TRUE
        PlaylistLoadAuto();
#endif
        SetStatus("Ready");
        return true;
    }

    void Shutdown()
    {
        SaveSettings();
#if SUPPORT_PLAYLIST == TRUE
        PlaylistSaveAuto();
#endif
#if SUPPORT_BAESCRIPT == TRUE
        SaveScriptToDisk();
        FreeScript();
#endif
#if SUPPORT_MIDI_HW == TRUE
        StopMidiInput();
#endif
        DestroyMixer();
        if (m_bae_initialized)
        {
            (void)BAE_Cleanup();
            m_bae_initialized = false;
        }
        if (m_dialog_mutex)
        {
            SDL_DestroyMutex(m_dialog_mutex);
            m_dialog_mutex = nullptr;
        }
    }

    void OpenCommandLineArgs(int argc, char **argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (!argv[i] || !argv[i][0] || argv[i][0] == '-')
            {
                continue;
            }
            HandleDroppedPath(argv[i]);
        }
    }

    void DrawUI()
    {
        ConsumeDialogResult();
        DrawPlayerWindow();
        DrawSettingsModal();
        DrawAboutModal();
        DrawRmfInfoModal();
        DrawEqModal();
        DrawCustomReverbModal();
        DrawExportModal();
#if SUPPORT_BAESCRIPT == TRUE
        DrawScriptModal();
#endif
        if (m_mixer)
        {
            (void)BAEMixer_Idle(m_mixer);
        }
    }

    /* Method bodies */
#include "zefidi2_bae.inc"
#include "zefidi2_dialogs.inc"
#include "zefidi2_settings.inc"
#include "zefidi2_keyboard.inc"
#include "zefidi2_playlist.inc"
#include "zefidi2_karaoke.inc"
#include "zefidi2_export.inc"
#include "zefidi2_midi_hw.inc"
#include "zefidi2_script.inc"
#include "zefidi2_player.inc"
};

int main(int argc, char **argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const SDL_WindowFlags window_flags =
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    char title[160];
    std::snprintf(title, sizeof(title), "zefidi2 - %s - %s", BAE_GetCurrentCPUArchitecture(),
                  _VERSION);
    SDL_Window *window = SDL_CreateWindow(title, static_cast<int>(720 * main_scale),
                                          static_cast<int>(520 * main_scale), window_flags);
    if (!window)
    {
        std::printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
    {
        std::printf("SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    Zefidi2Theme::DetectSystemAccent(); /* Windows accent, else NeoBAE purple */
    Zefidi2Theme::ApplyImGuiStyle();
    ImGuiStyle &style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    const float font_size = 13.0f * main_scale;
    ImFont *font_regular = nullptr;
#if defined(ZEFIDI2_EMBED_FONTS)
    {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        font_regular = io.Fonts->AddFontFromMemoryTTF(
            const_cast<unsigned char *>(liberation_sans_regular_data),
            static_cast<int>(liberation_sans_regular_size), font_size, &cfg);
    }
#else
    auto try_font = [&](const char *path) -> ImFont * {
        if (!path || !path[0])
        {
            return nullptr;
        }
        FILE *fp = std::fopen(path, "rb");
        if (!fp)
        {
            return nullptr;
        }
        std::fclose(fp);
        return io.Fonts->AddFontFromFileTTF(path, font_size);
    };
    font_regular = try_font("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    if (!font_regular)
    {
        font_regular = try_font("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    }
#if defined(_WIN32)
    if (!font_regular)
    {
        font_regular = try_font("C:\\Windows\\Fonts\\segoeui.ttf");
    }
#endif
#endif
    if (!font_regular)
    {
        font_regular = io.Fonts->AddFontDefault();
    }
    io.FontDefault = font_regular;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    Zefidi2App app;
    app.SetMainWindow(window);
    app.SetRenderer(renderer);
    if (!app.Init())
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "zefidi2", app.GetStatus(), nullptr);
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    app.OpenCommandLineArgs(argc, argv);
    SDL_ShowWindow(window);

    bool done = false;
    while (!done)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
            {
                app.RequestQuit();
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                event.window.windowID == SDL_GetWindowID(window))
            {
                app.RequestQuit();
            }
            if (event.type == SDL_EVENT_DROP_FILE)
            {
                const char *dropped = event.drop.data;
                if (dropped && dropped[0])
                {
                    app.HandleDroppedPath(dropped);
                }
            }
            if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
            {
                app.HandlePcKeyboardEvent(event);
            }
        }

        if (app.WantsQuit())
        {
            done = true;
            break;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.DrawUI();
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 14, 18, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    app.Shutdown();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    return main(__argc, __argv);
}
#endif
