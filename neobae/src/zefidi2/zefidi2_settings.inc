/* Settings modal + zefidi2.ini persistence (methods of Zefidi2App). */

    std::string ConfigPath(const char *name) const
    {
        char buf[1024] = {};
        (void)BAE_PrepareConfigFile(name, nullptr);
        if (BAE_GetConfigFilePath(buf, sizeof(buf), name) == 0 && buf[0])
        {
            return std::string(buf);
        }
        return std::string(name);
    }

    void LoadSettings()
    {
        const std::string path = ConfigPath("zefidi2.ini");
        FILE *fp = std::fopen(path.c_str(), "r");
        if (!fp)
        {
            return;
        }
        char line[1024];
        while (std::fgets(line, sizeof(line), fp))
        {
            char *nl = std::strchr(line, '\n');
            if (nl)
            {
                *nl = 0;
            }
            char *eq = std::strchr(line, '=');
            if (!eq)
            {
                continue;
            }
            *eq = 0;
            const char *key = line;
            const char *val = eq + 1;
            auto as_int = [&](int &dst) {
                dst = std::atoi(val);
            };
            auto as_bool = [&](bool &dst) {
                dst = (std::atoi(val) != 0);
            };
            if (std::strcmp(key, "volume") == 0)
                as_int(m_volume_percent);
            else if (std::strcmp(key, "tempo") == 0)
                as_int(m_tempo_percent);
            else if (std::strcmp(key, "transpose") == 0)
                as_int(m_transpose);
            else if (std::strcmp(key, "reverb") == 0)
                as_int(m_reverb_type);
            else if (std::strcmp(key, "loop") == 0)
                as_bool(m_loop_enabled);
            else if (std::strcmp(key, "sample_rate") == 0)
                as_int(m_sample_rate_hz);
            else if (std::strcmp(key, "stereo") == 0)
                as_bool(m_stereo);
            else if (std::strcmp(key, "velocity_curve") == 0)
                as_int(m_velocity_curve);
            else if (std::strcmp(key, "midi_voices") == 0)
                as_int(m_midi_voices);
            else if (std::strcmp(key, "webtv_bar") == 0)
                as_bool(m_webtv_style_bar);
            else if (std::strcmp(key, "show_keyboard") == 0)
                as_bool(m_show_keyboard);
            else if (std::strcmp(key, "playlist_enabled") == 0)
                as_bool(m_playlist_enabled);
            else if (std::strcmp(key, "shuffle") == 0)
                as_bool(m_playlist_shuffle);
            else if (std::strcmp(key, "repeat") == 0)
                as_int(m_playlist_repeat);
            else if (std::strcmp(key, "karaoke") == 0)
                as_bool(m_karaoke_enabled);
            else if (std::strcmp(key, "eq_enabled") == 0)
                as_bool(m_eq_enabled);
            else if (std::strcmp(key, "midi_in") == 0)
                as_bool(m_midi_in_enabled);
            else if (std::strcmp(key, "midi_in_port") == 0)
                as_int(m_midi_in_port);
            else if (std::strcmp(key, "last_bank") == 0)
                m_settings_last_bank = val;
            else if (std::strncmp(key, "eq", 2) == 0 && key[2] >= '0' && key[2] <= '4')
            {
                m_eq_gains[key[2] - '0'] = static_cast<float>(std::atof(val));
            }
            else if (std::strcmp(key, "script_enabled") == 0)
                as_bool(m_script_enabled);
            else if (std::strcmp(key, "normalize") == 0)
                as_bool(m_normalize_playback);
        }
        std::fclose(fp);
    }

    void SaveSettings()
    {
        (void)BAE_EnsureConfigDirectory();
        const std::string path = ConfigPath("zefidi2.ini");
        FILE *fp = std::fopen(path.c_str(), "w");
        if (!fp)
        {
            return;
        }
        if (!m_loaded_bank_path.empty())
        {
            m_settings_last_bank = m_loaded_bank_path;
        }
        std::fprintf(fp, "volume=%d\n", m_volume_percent);
        std::fprintf(fp, "tempo=%d\n", m_tempo_percent);
        std::fprintf(fp, "transpose=%d\n", m_transpose);
        std::fprintf(fp, "reverb=%d\n", m_reverb_type);
        std::fprintf(fp, "loop=%d\n", m_loop_enabled ? 1 : 0);
        std::fprintf(fp, "sample_rate=%d\n", m_sample_rate_hz);
        std::fprintf(fp, "stereo=%d\n", m_stereo ? 1 : 0);
        std::fprintf(fp, "velocity_curve=%d\n", m_velocity_curve);
        std::fprintf(fp, "midi_voices=%d\n", m_midi_voices);
        std::fprintf(fp, "webtv_bar=%d\n", m_webtv_style_bar ? 1 : 0);
        std::fprintf(fp, "show_keyboard=%d\n", m_show_keyboard ? 1 : 0);
        std::fprintf(fp, "playlist_enabled=%d\n", m_playlist_enabled ? 1 : 0);
        std::fprintf(fp, "shuffle=%d\n", m_playlist_shuffle ? 1 : 0);
        std::fprintf(fp, "repeat=%d\n", m_playlist_repeat);
        std::fprintf(fp, "karaoke=%d\n", m_karaoke_enabled ? 1 : 0);
        std::fprintf(fp, "eq_enabled=%d\n", m_eq_enabled ? 1 : 0);
        std::fprintf(fp, "midi_in=%d\n", m_midi_in_enabled ? 1 : 0);
        std::fprintf(fp, "midi_in_port=%d\n", m_midi_in_port);
        std::fprintf(fp, "script_enabled=%d\n", m_script_enabled ? 1 : 0);
        std::fprintf(fp, "normalize=%d\n", m_normalize_playback ? 1 : 0);
        std::fprintf(fp, "last_bank=%s\n", m_settings_last_bank.c_str());
        for (int i = 0; i < 5; ++i)
        {
            std::fprintf(fp, "eq%d=%.2f\n", i, static_cast<double>(m_eq_gains[i]));
        }
        std::fclose(fp);
    }

    void DrawSettingsModal()
    {
        if (m_settings_open)
        {
            ImGui::OpenPopup("Settings##zefidi2");
        }
        ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Settings##zefidi2", &m_settings_open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Audio");
            ImGui::Separator();
            ImGui::SetNextItemWidth(120.0f);
            static const int kRates[] = {11025, 22050, 44100, 48000};
            static const char *kRateLabels[] = {"11025", "22050", "44100", "48000"};
            int rate_idx = 2;
            for (int i = 0; i < 4; ++i)
            {
                if (kRates[i] == m_sample_rate_hz)
                {
                    rate_idx = i;
                }
            }
            if (ImGui::Combo("Sample rate", &rate_idx, kRateLabels, 4))
            {
                m_sample_rate_hz = kRates[rate_idx];
                SetStatus("Sample rate applies on next restart");
            }
            ImGui::Checkbox("Stereo", &m_stereo);
            ImGui::SetNextItemWidth(100.0f);
            ImGui::SliderInt("MIDI voices", &m_midi_voices, 8, 128);
            ImGui::Checkbox("Normalize playback", &m_normalize_playback);
            ImGui::Checkbox("WebTV style seek bar", &m_webtv_style_bar);

            ImGui::Spacing();
            ImGui::TextUnformatted("UI");
            ImGui::Separator();
            ImGui::Checkbox("Show virtual keyboard", &m_show_keyboard);
#if SUPPORT_PLAYLIST == TRUE
            ImGui::Checkbox("Enable playlist", &m_playlist_enabled);
#endif
#if SUPPORT_KARAOKE == TRUE
            ImGui::Checkbox("Enable karaoke", &m_karaoke_enabled);
#endif

#if SUPPORT_MIDI_HW == TRUE
            ImGui::Spacing();
            ImGui::TextUnformatted("MIDI Hardware");
            ImGui::Separator();
            DrawMidiHwSettings();
#endif

            ImGui::Spacing();
            if (ImGui::Button("Save", ImVec2(100.0f, 0.0f)))
            {
                SaveSettings();
                ApplyAllMixSettings();
                m_settings_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                m_settings_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        else if (!m_settings_open)
        {
            if (ImGui::IsPopupOpen("Settings##zefidi2"))
            {
                ImGui::ClosePopupToLevel(0, true);
            }
        }
    }
