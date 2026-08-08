/* About / RMF info / EQ / custom reverb modals + file dialogs (methods of Zefidi2App). */

    static void SDLCALL OnFileDialogResult(void *userdata, const char *const *filelist, int)
    {
        auto *app = static_cast<Zefidi2App *>(userdata);
        if (!app || !app->m_dialog_mutex)
        {
            return;
        }
        std::string selected;
        bool had_error = false;
        std::string err_msg;
        if (!filelist)
        {
            had_error = true;
            const char *err = SDL_GetError();
            err_msg = err ? err : "Unknown file dialog error";
        }
        else if (filelist[0])
        {
            selected = filelist[0];
        }
        SDL_LockMutex(app->m_dialog_mutex);
        app->m_dialog_result_ready = true;
        app->m_dialog_error = had_error;
        app->m_dialog_path = selected;
        app->m_dialog_error_message = err_msg;
        SDL_UnlockMutex(app->m_dialog_mutex);
    }

    void OpenLoadDialog()
    {
        m_pending_dialog_action = DialogAction::OpenMedia;
        static const SDL_DialogFileFilter filters[] = {
            {"Supported media",
             "mid;midi;kar;rmi;rmf;zmf;xmf;mxmf;wav;aif;aiff;au;mp3;mp2;ogg;opus;flac;qoa;adp;adx;wma"},
            {"MIDI / Karaoke", "mid;midi;kar;rmi"},
            {"RMF / ZMF / XMF", "rmf;zmf;xmf;mxmf"},
            {"Audio", "wav;aif;aiff;au;mp3;ogg;opus;flac"},
            {"All files", "*"},
        };
        SDL_ShowOpenFileDialog(OnFileDialogResult, this, m_main_window, filters,
                               static_cast<int>(SDL_arraysize(filters)), nullptr, false);
    }

    void OpenBankDialog()
    {
        m_pending_dialog_action = DialogAction::OpenBank;
        static const SDL_DialogFileFilter filters[] = {
            {"Banks", "hsb;zsb;sf2;sf3;sfo;dls"},
            {"All files", "*"},
        };
        SDL_ShowOpenFileDialog(OnFileDialogResult, this, m_main_window, filters,
                               static_cast<int>(SDL_arraysize(filters)), nullptr, false);
    }

    void OpenPlaylistAddDialog()
    {
        m_pending_dialog_action = DialogAction::AddPlaylist;
        static const SDL_DialogFileFilter filters[] = {
            {"Supported media",
             "mid;midi;kar;rmi;rmf;zmf;wav;mp3;ogg;opus;flac"},
            {"All files", "*"},
        };
        SDL_ShowOpenFileDialog(OnFileDialogResult, this, m_main_window, filters,
                               static_cast<int>(SDL_arraysize(filters)), nullptr, true);
    }

    void OpenExportSaveDialog()
    {
        m_pending_dialog_action = DialogAction::ExportSave;
        static const SDL_DialogFileFilter filters[] = {
            {"WAV", "wav"},
            {"All files", "*"},
        };
        SDL_ShowSaveFileDialog(OnFileDialogResult, this, m_main_window, filters,
                               static_cast<int>(SDL_arraysize(filters)), "export.wav");
    }

    void ConsumeDialogResult()
    {
        if (!m_dialog_mutex)
        {
            return;
        }
        bool ready = false;
        bool error = false;
        std::string path;
        std::string err;
        DialogAction action = m_pending_dialog_action;
        SDL_LockMutex(m_dialog_mutex);
        if (m_dialog_result_ready)
        {
            ready = true;
            error = m_dialog_error;
            path = m_dialog_path;
            err = m_dialog_error_message;
            m_dialog_result_ready = false;
        }
        SDL_UnlockMutex(m_dialog_mutex);
        if (!ready)
        {
            return;
        }
        if (error)
        {
            SetStatus(std::string("Dialog: ") + err);
            return;
        }
        if (path.empty())
        {
            return;
        }
        switch (action)
        {
        case DialogAction::OpenMedia:
#if SUPPORT_PLAYLIST == TRUE
            if (m_playlist_enabled)
            {
                PlaylistAddFile(path);
            }
#endif
            if (LoadMediaFromPath(path.c_str()))
            {
                m_song_started = false;
                m_playing = false;
                (void)PlayPause();
            }
            break;
        case DialogAction::OpenBank:
            (void)LoadBankFromPath(path.c_str());
            break;
        case DialogAction::AddPlaylist:
#if SUPPORT_PLAYLIST == TRUE
            PlaylistAddFile(path);
            PlaylistSaveAuto();
#endif
            break;
        case DialogAction::ExportSave:
            StartExportToPath(path.c_str());
            break;
        }
    }

    void HandleDroppedPath(const char *path)
    {
        if (!path || !path[0])
        {
            return;
        }
        if (IsBankPath(path))
        {
            (void)LoadBankFromPath(path);
            return;
        }
#if SUPPORT_PLAYLIST == TRUE
        if (m_playlist_enabled)
        {
            PlaylistAddFile(path);
            PlaylistSaveAuto();
        }
#endif
        if (LoadMediaFromPath(path))
        {
            m_song_started = false;
            m_playing = false;
            (void)PlayPause();
        }
    }

    void DrawAboutModal()
    {
        if (m_about_open)
        {
            ImGui::OpenPopup("About##zefidi2");
        }
        if (ImGui::BeginPopupModal("About##zefidi2", &m_about_open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("zefidi2 — slim ImGui NeoBAE player");
            ImGui::Text("Version %s (%s)", _VERSION, BAE_GetCurrentCPUArchitecture());
            ImGui::Separator();
            ImGui::TextWrapped(
                "Compact Dear ImGui player with classic zefidi feature coverage. "
                "Engine: NeoBAE / libneobae. UI: SDL3 + Dear ImGui.");
            ImGui::Spacing();
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                m_about_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    std::string LoadRmfContainerVersion(const std::string &path) const
    {
        /* Match classic zefidi: IREZ/ZREZ header mapID + version dword. */
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            return {};
        }
        unsigned char hdr[12] = {};
        const size_t n = std::fread(hdr, 1, sizeof(hdr), f);
        std::fclose(f);
        if (n != sizeof(hdr))
        {
            return {};
        }
        const uint32_t map_id = (static_cast<uint32_t>(hdr[0]) << 24) |
                                (static_cast<uint32_t>(hdr[1]) << 16) |
                                (static_cast<uint32_t>(hdr[2]) << 8) |
                                static_cast<uint32_t>(hdr[3]);
        const uint32_t version = (static_cast<uint32_t>(hdr[4]) << 24) |
                                 (static_cast<uint32_t>(hdr[5]) << 16) |
                                 (static_cast<uint32_t>(hdr[6]) << 8) |
                                 static_cast<uint32_t>(hdr[7]);
        if (!XFILERESOURCE_ID_IS_VALID(map_id))
        {
            return {};
        }
        char buf[64];
        const char *kind = (map_id == XFILERESOURCE_ZMF_ID || version >= 2u) ? "ZMF" : "RMF";
        std::snprintf(buf, sizeof(buf), "%s Version: %u", kind, static_cast<unsigned>(version));
        return std::string(buf);
    }

    void DrawRmfInfoModal()
    {
        if (m_rmf_info_open)
        {
            ImGui::OpenPopup("RMF Info##zefidi2");
        }
        if (ImGui::BeginPopupModal("RMF Info##zefidi2", &m_rmf_info_open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (m_loaded_media_path.empty() || !m_is_rmf_file)
            {
                ImGui::TextUnformatted("No RMF loaded.");
            }
            else
            {
                const std::string ver = LoadRmfContainerVersion(m_loaded_media_path);
                if (!ver.empty())
                {
                    ImGui::TextUnformatted(ver.c_str());
                    ImGui::Separator();
                }
                static const BAEInfoType kTypes[] = {
                    TITLE_INFO, PERFORMED_BY_INFO, COMPOSER_INFO, COPYRIGHT_INFO,
                    GENRE_INFO, TEMPO_DESCRIPTION_INFO, ORIGINAL_SOURCE_INFO,
                };
                static const char *kLabels[] = {
                    "Title", "Performed by", "Composer", "Copyright",
                    "Genre", "Tempo", "Source",
                };
                bool any = !ver.empty();
                for (size_t i = 0; i < IM_ARRAYSIZE(kTypes); ++i)
                {
                    char buf[512] = {};
                    if (BAEUtil_GetRmfSongInfoFromFile((BAEPathName)m_loaded_media_path.c_str(), 0,
                                                       kTypes[i], buf, sizeof(buf)) == BAE_NO_ERROR &&
                        buf[0])
                    {
                        ImGui::Text("%s: %s", kLabels[i], buf);
                        any = true;
                    }
                }
                if (!any)
                {
                    ImGui::TextDisabled("No embedded info fields.");
                }
            }
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                m_rmf_info_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void DrawEqModal()
    {
        if (m_eq_open)
        {
            ImGui::OpenPopup("EQ##zefidi2");
        }
        if (ImGui::BeginPopupModal("EQ##zefidi2", &m_eq_open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Checkbox("Enabled", &m_eq_enabled);
            static const char *kBands[] = {"60Hz", "230Hz", "910Hz", "3.6k", "14k"};
            for (int i = 0; i < 5; ++i)
            {
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::SliderFloat(kBands[i], &m_eq_gains[i], -12.0f, 12.0f, "%.1f dB"))
                {
                    ApplyEq();
                }
            }
            if (ImGui::Button("Flat"))
            {
                for (float &g : m_eq_gains)
                {
                    g = 0.0f;
                }
                ApplyEq();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                ApplyEq();
                m_eq_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void DrawCustomReverbModal()
    {
        if (m_custom_reverb_open)
        {
            ImGui::OpenPopup("Custom Reverb##zefidi2");
        }
        if (ImGui::BeginPopupModal("Custom Reverb##zefidi2", &m_custom_reverb_open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::SliderInt("Combs", &m_custom_comb_count, 1, 8);
            for (int i = 0; i < m_custom_comb_count && i < 8; ++i)
            {
                ImGui::PushID(i);
                ImGui::Text("Comb %d", i + 1);
                ImGui::SliderInt("Delay ms", &m_custom_comb_delay[i], 1, 200);
                ImGui::SliderInt("Feedback", &m_custom_comb_feedback[i], 0, 127);
                ImGui::SliderInt("Gain", &m_custom_comb_gain[i], 0, 127);
                ImGui::PopID();
            }
            ImGui::SliderInt("Lowpass", &m_custom_lowpass, 0, 127);
            if (ImGui::Button("Apply", ImVec2(100.0f, 0.0f)))
            {
                ApplyCustomReverbParams();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                ApplyCustomReverbParams();
                m_custom_reverb_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
