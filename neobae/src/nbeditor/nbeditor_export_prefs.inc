/* Editor Preferences, Export Bank, Export as Audio, Song Settings.
 * Included inside NbEditorApp. */

    static std::string EditorPrefsFilePath()
    {
        const char *xdg = std::getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0])
        {
            return std::string(xdg) + "/neobae/nbeditor_prefs";
        }
        const char *home = std::getenv("HOME");
        if (home && home[0])
        {
            return std::string(home) + "/.config/neobae/nbeditor_prefs";
        }
        /* Windows builds often lack HOME; USERPROFILE is the usual stand-in. */
        const char *userprofile = std::getenv("USERPROFILE");
        if (userprofile && userprofile[0])
        {
            return std::string(userprofile) + "/.config/neobae/nbeditor_prefs";
        }
        return std::string("nbeditor_prefs");
    }

    static void PrefEnsureParentDirs(const std::string &path)
    {
        /* Create each path segment so ~/.config/neobae exists on first save. */
        std::string dir = path;
        for (char &c : dir)
        {
            if (c == '\\')
            {
                c = '/';
            }
        }
        const size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
        {
            return;
        }
        dir.resize(slash);
        std::string cur;
        cur.reserve(dir.size());
        for (size_t i = 0; i < dir.size(); ++i)
        {
            const char ch = dir[i];
            cur.push_back(ch);
            if (ch == '/' || i + 1 == dir.size())
            {
                if (cur.size() <= 1)
                {
                    continue; /* "/" or drive-root skip handled below */
                }
                /* Skip "C:" style roots on Windows (no trailing slash yet). */
                if (cur.size() == 2 && cur[1] == ':')
                {
                    continue;
                }
#if defined(_WIN32)
                CreateDirectoryA(cur.c_str(), nullptr);
#else
                ::mkdir(cur.c_str(), 0755);
#endif
            }
        }
    }

    static void PrefAssignCString(char *dst, size_t n, const std::string &value)
    {
        if (!dst || n == 0)
        {
            return;
        }
        std::snprintf(dst, n, "%s", value.c_str());
    }

    static bool PrefParseBool(const std::string &value, bool default_value)
    {
        if (value == "1" || value == "true" || value == "TRUE" || value == "yes")
        {
            return true;
        }
        if (value == "0" || value == "false" || value == "FALSE" || value == "no")
        {
            return false;
        }
        return default_value;
    }

    void LoadEditorPrefsFromDisk()
    {
        m_pref_reopen_last_session = false;
        m_pref_save_session_layout = true;
        m_pref_ignore_session_layout = false;
        m_pref_show_tips_at_startup = true;
        m_tips_seen.clear();
        m_tips_next = 0;
        m_pref_adsr_default_valid = false;
        std::memset(&m_pref_adsr_default, 0, sizeof(m_pref_adsr_default));
        m_pref_mod_ext_pitch = false;
        m_pref_mod_ext_adsr = true;
        m_pref_mod_resample_filter = static_cast<int>(MOD2RMF_RESAMPLE_SINC_8TAP);
        m_pref_mod_resample_rate = 0;
        m_pref_mod_amiga_filter = static_cast<int>(MOD2RMF_AMIGA_FILTER_NONE);
        m_pref_mod_stereo_sep = 75;
#if SUPPORT_MIDI_HW == TRUE
        m_midi_hw_enabled = false;
        m_midi_hw_device_index = -1;
#endif
        std::ifstream in(EditorPrefsFilePath());
        if (!in)
        {
            return;
        }
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            {
                line.pop_back();
            }
            if (line.empty() || line[0] == '#')
            {
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            const std::string key = line.substr(0, eq);
            const std::string value = line.substr(eq + 1);
            if (key == "reopen_last_session")
            {
                m_pref_reopen_last_session = PrefParseBool(value, false);
            }
            else if (key == "save_session_layout")
            {
                m_pref_save_session_layout = PrefParseBool(value, true);
            }
            else if (key == "ignore_session_layout")
            {
                m_pref_ignore_session_layout = PrefParseBool(value, false);
            }
            else if (key == "show_tips_at_startup")
            {
                m_pref_show_tips_at_startup = PrefParseBool(value, true);
            }
            else if (key == "tips_seen")
            {
                m_tips_seen.clear();
                size_t start = 0;
                while (start < value.size())
                {
                    size_t comma = value.find(',', start);
                    if (comma == std::string::npos)
                    {
                        comma = value.size();
                    }
                    const std::string token = value.substr(start, comma - start);
                    if (!token.empty())
                    {
                        const int idx = std::atoi(token.c_str());
                        if (idx >= 0)
                        {
                            if (idx >= static_cast<int>(m_tips_seen.size()))
                            {
                                m_tips_seen.resize(static_cast<size_t>(idx) + 1u, 0);
                            }
                            m_tips_seen[static_cast<size_t>(idx)] = 1;
                        }
                    }
                    start = comma + (comma < value.size() ? 1u : 0u);
                    if (comma >= value.size())
                    {
                        break;
                    }
                }
            }
            else if (key == "tips_next")
            {
                m_tips_next = std::max(0, std::atoi(value.c_str()));
            }
            else if (key == "mod_import_ext_pitch")
            {
                m_pref_mod_ext_pitch = PrefParseBool(value, false);
            }
            else if (key == "mod_import_ext_adsr")
            {
                m_pref_mod_ext_adsr = PrefParseBool(value, false);
            }
            else if (key == "mod_import_resample_filter")
            {
                m_pref_mod_resample_filter = std::clamp(std::atoi(value.c_str()), 0, 3);
            }
            else if (key == "mod_import_resample_rate")
            {
                const int rate = std::atoi(value.c_str());
                m_pref_mod_resample_rate =
                    (rate == 0 || (rate >= 1000 && rate <= 384000)) ? rate : 0;
            }
            else if (key == "mod_import_amiga_filter")
            {
                m_pref_mod_amiga_filter = std::clamp(std::atoi(value.c_str()), 0, 2);
            }
            else if (key == "mod_import_stereo_separation")
            {
                m_pref_mod_stereo_sep = std::clamp(std::atoi(value.c_str()), 0, 100);
            }
#if SUPPORT_MIDI_HW == TRUE
            else if (key == "midi_hw_enabled")
            {
                m_midi_hw_enabled = PrefParseBool(value, false);
            }
            else if (key == "midi_hw_device_index")
            {
                m_midi_hw_device_index = std::atoi(value.c_str());
            }
#endif
            else if (key == "song_title")
            {
                PrefAssignCString(m_pref_song_title, sizeof(m_pref_song_title), value);
            }
            else if (key == "song_composer")
            {
                PrefAssignCString(m_pref_song_composer, sizeof(m_pref_song_composer), value);
            }
            else if (key == "song_copyright")
            {
                PrefAssignCString(m_pref_song_copyright, sizeof(m_pref_song_copyright), value);
            }
            else if (key == "song_performed")
            {
                PrefAssignCString(m_pref_song_performed, sizeof(m_pref_song_performed), value);
            }
            else if (key == "song_publisher")
            {
                PrefAssignCString(m_pref_song_publisher, sizeof(m_pref_song_publisher), value);
            }
            else if (key == "song_license_url")
            {
                PrefAssignCString(m_pref_song_license_url, sizeof(m_pref_song_license_url), value);
            }
            else if (key == "song_genre")
            {
                PrefAssignCString(m_pref_song_genre, sizeof(m_pref_song_genre), value);
            }
            else if (key == "song_subgenre")
            {
                PrefAssignCString(m_pref_song_subgenre, sizeof(m_pref_song_subgenre), value);
            }
            else if (key == "song_notes")
            {
                PrefAssignCString(m_pref_song_notes, sizeof(m_pref_song_notes), value);
            }
            else if (key == "adsr_default_stages")
            {
                const int n = std::atoi(value.c_str());
                m_pref_adsr_default.stageCount =
                    static_cast<uint32_t>(std::clamp(n, 0, BAE_EDITOR_MAX_ADSR_STAGES));
                m_pref_adsr_default_valid = (m_pref_adsr_default.stageCount > 0);
            }
            else if (key.rfind("adsr_default_stage_", 0) == 0)
            {
                /* adsr_default_stage_N=level,time,flags */
                const int stage = std::atoi(key.c_str() + 19);
                if (stage >= 0 && stage < BAE_EDITOR_MAX_ADSR_STAGES)
                {
                    int level = 0, time = 0, flags = 0;
                    if (std::sscanf(value.c_str(), "%d,%d,%d", &level, &time, &flags) == 3)
                    {
                        m_pref_adsr_default.stages[stage].level = level;
                        m_pref_adsr_default.stages[stage].time = time;
                        m_pref_adsr_default.stages[stage].flags = flags;
                        m_pref_adsr_default_valid = true;
                    }
                }
            }
        }
    }

    void SaveEditorPrefsToDisk() const
    {
        const std::string path = EditorPrefsFilePath();
        PrefEnsureParentDirs(path);
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            return;
        }
        out << "reopen_last_session=" << (m_pref_reopen_last_session ? "1" : "0") << '\n';
        out << "save_session_layout=" << (m_pref_save_session_layout ? "1" : "0") << '\n';
        out << "ignore_session_layout=" << (m_pref_ignore_session_layout ? "1" : "0") << '\n';
        out << "show_tips_at_startup=" << (m_pref_show_tips_at_startup ? "1" : "0") << '\n';
        out << "tips_next=" << m_tips_next << '\n';
        {
            out << "tips_seen=";
            bool first = true;
            for (size_t i = 0; i < m_tips_seen.size(); ++i)
            {
                if (!m_tips_seen[i])
                {
                    continue;
                }
                if (!first)
                {
                    out << ',';
                }
                out << i;
                first = false;
            }
            out << '\n';
        }
        out << "mod_import_ext_pitch=" << (m_pref_mod_ext_pitch ? "1" : "0") << '\n';
        out << "mod_import_ext_adsr=" << (m_pref_mod_ext_adsr ? "1" : "0") << '\n';
        out << "mod_import_resample_filter=" << m_pref_mod_resample_filter << '\n';
        out << "mod_import_resample_rate=" << m_pref_mod_resample_rate << '\n';
        out << "mod_import_amiga_filter=" << m_pref_mod_amiga_filter << '\n';
        out << "mod_import_stereo_separation=" << m_pref_mod_stereo_sep << '\n';
#if SUPPORT_MIDI_HW == TRUE
        out << "midi_hw_enabled=" << (m_midi_hw_enabled ? "1" : "0") << '\n';
        out << "midi_hw_device_index=" << m_midi_hw_device_index << '\n';
#endif
        out << "song_title=" << m_pref_song_title << '\n';
        out << "song_composer=" << m_pref_song_composer << '\n';
        out << "song_copyright=" << m_pref_song_copyright << '\n';
        out << "song_performed=" << m_pref_song_performed << '\n';
        out << "song_publisher=" << m_pref_song_publisher << '\n';
        out << "song_license_url=" << m_pref_song_license_url << '\n';
        out << "song_genre=" << m_pref_song_genre << '\n';
        out << "song_subgenre=" << m_pref_song_subgenre << '\n';
        out << "song_notes=" << m_pref_song_notes << '\n';
        if (m_pref_adsr_default_valid)
        {
            out << "adsr_default_stages=" << m_pref_adsr_default.stageCount << '\n';
            for (uint32_t i = 0; i < m_pref_adsr_default.stageCount && i < BAE_EDITOR_MAX_ADSR_STAGES;
                 ++i)
            {
                out << "adsr_default_stage_" << i << '='
                    << m_pref_adsr_default.stages[i].level << ','
                    << m_pref_adsr_default.stages[i].time << ','
                    << m_pref_adsr_default.stages[i].flags << '\n';
            }
        }
    }

    void OpenEditorPreferencesDialog()
    {
        m_prefs_draft_reopen = m_pref_reopen_last_session;
        m_prefs_draft_save_layout = m_pref_save_session_layout;
        m_prefs_draft_ignore_layout = m_pref_ignore_session_layout;
        m_prefs_draft_show_tips = m_pref_show_tips_at_startup;
#if SUPPORT_MIDI_HW == TRUE
        m_prefs_draft_midi_hw_enabled = m_midi_hw_enabled;
        m_prefs_draft_midi_hw_device_index = m_midi_hw_device_index;
        MidiHwRefreshDeviceList();
#endif
        m_prefs_open = true;
    }

    void DrawEditorPreferencesDialog()
    {
        if (!m_prefs_open)
        {
            return;
        }
        ImGui::OpenPopup("Preferences");
        if (!ImGui::BeginPopupModal("Preferences", &m_prefs_open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::Checkbox("Reopen last session on startup", &m_prefs_draft_reopen);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "BE2 does this by default. Off by default in nbeditor (clean slate).");
        }
        ImGui::Checkbox("Save session layout", &m_prefs_draft_save_layout);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "When saving a Session, store dock/editor layout and master window size.\n"
                "(Master window position is saved for off-screen recovery only.)\n"
                "Enabled by default.");
        }
        ImGui::Checkbox("Ignore session layout on load", &m_prefs_draft_ignore_layout);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "When opening a Session, skip applying saved size/dock/editor layout.\n"
                "Disabled by default.");
        }
        ImGui::Checkbox("Show tips at startup", &m_prefs_draft_show_tips);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Show a Tip dialog when NBEditor starts.\n"
                "Also available from Help → Tips…\n"
                "Enabled by default.");
        }
        DrawMidiHwPreferencesSection();
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(80, 0)))
        {
            m_pref_reopen_last_session = m_prefs_draft_reopen;
            m_pref_save_session_layout = m_prefs_draft_save_layout;
            m_pref_ignore_session_layout = m_prefs_draft_ignore_layout;
            m_pref_show_tips_at_startup = m_prefs_draft_show_tips;
#if SUPPORT_MIDI_HW == TRUE
            m_midi_hw_enabled = m_prefs_draft_midi_hw_enabled;
            m_midi_hw_device_index = m_prefs_draft_midi_hw_device_index;
            MidiHwApplyFromPrefs();
#endif
            SaveEditorPrefsToDisk();
            m_prefs_open = false;
            ImGui::CloseCurrentPopup();
            SetStatus("Preferences saved");
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)))
        {
            m_prefs_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void OpenModuleImportSettingsDialog()
    {
        m_mod_import_draft_ext_pitch = m_pref_mod_ext_pitch;
        m_mod_import_draft_ext_adsr = m_pref_mod_ext_adsr;
        m_mod_import_draft_resample_filter = m_pref_mod_resample_filter;
        m_mod_import_draft_resample_rate = m_pref_mod_resample_rate;
        m_mod_import_draft_amiga_filter = m_pref_mod_amiga_filter;
        m_mod_import_draft_stereo_sep = m_pref_mod_stereo_sep;
        m_mod_import_prefs_open = true;
    }

    void DrawModuleImportSettingsDialog()
    {
        if (!m_mod_import_prefs_open)
        {
            return;
        }
        ImGui::OpenPopup("Module Import Settings");
        if (!ImGui::BeginPopupModal("Module Import Settings",
                                    &m_mod_import_prefs_open,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        ImGui::TextUnformatted("ZMF features");
        ImGui::Checkbox("Extended pitch range (-96 st)", &m_mod_import_draft_ext_pitch);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Allow pitch down to -96 semitones (vs default -24).\n"
                "Matches mod2rmf --ext-pitch. Implies ZMF.");
        }
        ImGui::Checkbox("Extended ADSR (up to 32 stages)", &m_mod_import_draft_ext_adsr);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Preserve more tracker envelope points (up to 32 stages).\n"
                "Off = classic 8-stage RMF cap. Matches mod2rmf --ext-adsr.\n"
                "Implies ZMF when envelopes exceed 8 stages.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        static const char *kResampleLabels[] = {
            "Nearest",
            "Linear",
            "Cubic",
            "Sinc (8-tap)",
        };
        m_mod_import_draft_resample_filter =
            std::clamp(m_mod_import_draft_resample_filter, 0, 3);
        ImGui::Combo("Resample filter",
                     &m_mod_import_draft_resample_filter,
                     kResampleLabels,
                     IM_ARRAYSIZE(kResampleLabels));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Interpolation used when resampling module samples.\n"
                "Matches mod2rmf --resample-filter. Default: Sinc.\n"
                "Only applied when Resample rate is non-zero.");
        }
        ImGui::InputInt("Resample rate (Hz)", &m_mod_import_draft_resample_rate);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "0 = keep native module rate. Otherwise 1000–384000 Hz.\n"
                "Matches mod2rmf --resample-rate.");
        }
        if (m_mod_import_draft_resample_rate < 0)
        {
            m_mod_import_draft_resample_rate = 0;
        }

        static const char *kAmigaLabels[] = {
            "None",
            "A500 (~3275 Hz LPF)",
            "A1200 (~28867 Hz LPF)",
        };
        m_mod_import_draft_amiga_filter = std::clamp(m_mod_import_draft_amiga_filter, 0, 2);
        ImGui::Combo("Amiga filter",
                     &m_mod_import_draft_amiga_filter,
                     kAmigaLabels,
                     IM_ARRAYSIZE(kAmigaLabels));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Bake Amiga hardware low-pass into samples before import.\n"
                "Matches mod2rmf --amiga-filter.");
        }

        ImGui::Spacing();
        ImGui::SliderInt("Stereo separation", &m_mod_import_draft_stereo_sep, 0, 100, "%d%%");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Stereo width: 0 = mono (center), 75 = default (OpenMPT-like),\n"
                "100 = hard L/R. Matches mod2rmf --stereo-separation.");
        }

        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(80, 0)))
        {
            m_pref_mod_ext_pitch = m_mod_import_draft_ext_pitch;
            m_pref_mod_ext_adsr = m_mod_import_draft_ext_adsr;
            m_pref_mod_resample_filter = std::clamp(m_mod_import_draft_resample_filter, 0, 3);
            {
                int rate = m_mod_import_draft_resample_rate;
                if (rate != 0 && (rate < 1000 || rate > 384000))
                {
                    rate = 0;
                }
                m_pref_mod_resample_rate = std::max(0, rate);
            }
            m_pref_mod_amiga_filter = std::clamp(m_mod_import_draft_amiga_filter, 0, 2);
            m_pref_mod_stereo_sep = std::clamp(m_mod_import_draft_stereo_sep, 0, 100);
            SaveEditorPrefsToDisk();
            m_mod_import_prefs_open = false;
            ImGui::CloseCurrentPopup();
            SetStatus("Module import settings saved");
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)))
        {
            m_mod_import_prefs_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* ---- Export Bank ---- */

    void OpenExportBankDialog()
    {
        if (!m_bank_token)
        {
            SetStatus("No bank loaded");
            return;
        }
        m_export_bank_zsb_reason = 0;
        m_export_bank_needs_zsb = BankRequiresZsb(&m_export_bank_zsb_reason);
        m_export_bank_open = true;
    }

    void DrawExportBankDialog()
    {
        if (!m_export_bank_open)
        {
            return;
        }
        ImGui::OpenPopup("Export Bank");
        if (!ImGui::BeginPopupModal("Export Bank", &m_export_bank_open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::TextWrapped(
            "Export a copy of the current bank. The session bank is not modified.");
        ImGui::Checkbox("Encrypt samples (ESND)", &m_export_bank_encrypt);
        ImGui::Checkbox("Include groovoids", &m_export_bank_include_groovoids);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Off = strip groovoids and Session songs from the export.\n"
                "Session songs belong in .zsn; use Convert… below to embed them.");
        }
        ImGui::Checkbox("Drop unused samples", &m_export_bank_drop_unref);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Omit bank SNDs not referenced by any instrument (recommended).");
        }
        ImGui::Checkbox("Convert Session songs to groovoids", &m_export_bank_songs_to_groovoids);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Adds each Session MIDI song as SONG→emid named like the song.\n"
                "Otherwise Session songs are omitted from bank export.");
        }
        ImGui::Spacing();
        if (m_export_bank_needs_zsb)
        {
            char reason_buf[256] = {0};
            BAEZMFReasonCodeToString(m_export_bank_zsb_reason, reason_buf, sizeof(reason_buf));
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Requires .zsb: %s",
                               reason_buf);
        }
        if (ImGui::Button("Export...", ImVec2(100, 0)))
        {
            m_export_bank_open = false;
            ImGui::CloseCurrentPopup();
            m_pending_dialog_action = DialogAction::ExportBank;
            const bool want_zsb = m_export_bank_needs_zsb;
            static const SDL_DialogFileFilter filters_any[] = {
                {"Bank (*.hsb;*.zsb)", "hsb;zsb"},
                {"All files (*.*)", "*"},
            };
            static const SDL_DialogFileFilter filters_zsb[] = {
                {"NeoBAE Bank (*.zsb)", "zsb"},
                {"All files (*.*)", "*"},
            };
            static char default_path[256];
            const std::string stem = SuggestedBankExportFileStem();
            std::snprintf(default_path,
                          sizeof(default_path),
                          "%s.%s",
                          stem.c_str(),
                          want_zsb ? "zsb" : "hsb");
            SDL_ShowSaveFileDialog(OnFileDialogResult,
                                   this,
                                   m_main_window,
                                   want_zsb ? filters_zsb : filters_any,
                                   want_zsb ? static_cast<int>(SDL_arraysize(filters_zsb))
                                            : static_cast<int>(SDL_arraysize(filters_any)),
                                   default_path);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)))
        {
            m_export_bank_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void ExportBankCloneToPath(const std::string &path)
    {
        if (!m_mixer || !m_bank_token || path.empty())
        {
            SetStatus("Export Bank failed: no bank");
            return;
        }

        bool remapped = false;
        const std::string out_path = EnforceBankSavePath(path, &remapped);
        const bool want_zsb = EndsWith(out_path, ".zsb");
        const int32_t target_map =
            want_zsb ? XFILERESOURCE_ZMF_ID : XFILERESOURCE_ID;

        UpdateBusyProgress(0.02f, "Scanning bank…");
        XFILE src_bank = reinterpret_cast<XFILE>(m_bank_token);
        const bool has_song_payloads = BankFileHasSongPayloads(src_bank);
        /* Any song payloads: strip all (groovoids off) or Session-only (on). */
        const bool need_song_strip = has_song_payloads;
        const bool need_casd_strip =
            XCountFileResourcesOfType(src_bank, FOUR_CHAR('C', 'a', 'S', 'd')) > 0;
        const bool need_drop =
            m_export_bank_drop_unref && CountUnreferencedBankSamples(m_bank_token) > 0;
        const bool need_encrypt = m_export_bank_encrypt;
        const bool need_add_groovoids = m_export_bank_songs_to_groovoids && !m_session_songs.empty();
        const bool needs_mutate =
            need_song_strip || need_casd_strip || need_drop || need_encrypt || need_add_groovoids;

        /* Fast path: serialize once and write (no clone / rebuild). */
        if (!needs_mutate)
        {
            UpdateBusyProgress(0.3f, want_zsb ? "Building ZSB…" : "Building HSB…");
            unsigned char *out_bytes = nullptr;
            uint32_t out_size = 0;
            const BAEResult sr =
                BAERmfEditorBank_SaveToMemoryEx(m_bank_token, target_map, &out_bytes, &out_size);
            if (sr != BAE_NO_ERROR || !out_bytes || out_size == 0)
            {
                if (out_bytes)
                {
                    XDisposePtr(out_bytes);
                }
                SetStatus(std::string("Export Bank failed: ") + FormatBAEError(sr));
                return;
            }
            UpdateBusyProgress(0.9f, "Writing bank file…");
            XFILENAME xf_out;
            XConvertPathToXFILENAME(const_cast<char *>(out_path.c_str()), &xf_out);
            XFILE disk = XFileOpenForWrite(&xf_out, TRUE);
            if (!disk ||
                XFileSetLength(disk, 0) != 0 ||
                XFileSetPosition(disk, 0L) != 0 ||
                XFileWrite(disk, out_bytes, static_cast<int32_t>(out_size)) != 0)
            {
                if (disk)
                {
                    XFileClose(disk);
                }
                XDisposePtr(out_bytes);
                SetStatus("Export Bank failed: write");
                return;
            }
            XFileClose(disk);
            XDisposePtr(out_bytes);
            SetStatus(std::string("Exported bank: ") + FileNameFromPath(out_path));
            return;
        }

        UpdateBusyProgress(0.08f, "Snapshotting bank…");
        unsigned char *bank_bytes = nullptr;
        uint32_t bank_size = 0;
        BAEResult sr =
            BAERmfEditorBank_SaveToMemoryEx(m_bank_token, target_map, &bank_bytes, &bank_size);
        if (sr != BAE_NO_ERROR || !bank_bytes || bank_size == 0)
        {
            if (bank_bytes)
            {
                XDisposePtr(bank_bytes);
            }
            SetStatus(std::string("Export Bank failed: snapshot: ") + FormatBAEError(sr));
            return;
        }

        /* Writable clone in one copy — skip mixer AddBank + EnsureWritable
         * (those kept a second full image of large banks like zpatches). */
        UpdateBusyProgress(0.15f, "Opening writable clone…");
        XFILE temp_file = XFileOpenWritableResourceFromMemory(bank_bytes, bank_size);
        XDisposePtr(bank_bytes);
        bank_bytes = nullptr;
        if (!temp_file)
        {
            SetStatus("Export Bank failed: could not open writable clone");
            return;
        }
        BAEBankToken temp_token = reinterpret_cast<BAEBankToken>(temp_file);

        if (need_song_strip)
        {
            UpdateBusyProgress(0.25f,
                               m_export_bank_include_groovoids
                                   ? "Removing Session songs…"
                                   : "Removing songs…");
            const char *strip_err = nullptr;
            bool strip_ok = true;
            if (!m_export_bank_include_groovoids)
            {
                /* One omit rebuild for SONG/ZSNG/Midi/emid/DATe. */
                strip_ok = StripAllSongsFromBankFile(temp_file, &strip_err);
            }
            else
            {
                strip_ok = StripBankSessionSongs(temp_file, &strip_err);
            }
            if (!strip_ok)
            {
                SetStatus(std::string("Export Bank failed: ") +
                          (strip_err ? strip_err : "could not remove songs"));
                XFileClose(temp_file);
                return;
            }
        }

        if (need_casd_strip)
        {
            UpdateBusyProgress(0.28f, "Removing CaSd masters…");
            const char *casd_err = nullptr;
            if (!StripCasdFromBankFile(temp_file, &casd_err))
            {
                SetStatus(std::string("Export Bank failed: ") +
                          (casd_err ? casd_err : "could not remove CaSd"));
                XFileClose(temp_file);
                return;
            }
        }

        if (need_add_groovoids)
        {
            UpdateBusyProgress(0.35f, "Adding session songs as groovoids…");
            for (size_t i = 0; i < m_session_songs.size(); ++i)
            {
                const SessionSongEntry &entry = m_session_songs[i];
                if (entry.midi_bytes.empty())
                {
                    continue;
                }
                std::vector<unsigned char> emid = entry.midi_bytes;
                XEncryptData(emid.data(), static_cast<uint32_t>(emid.size()));
                const XLongResourceID emid_id = static_cast<XLongResourceID>(20000 + i);
                const XLongResourceID song_id = static_cast<XLongResourceID>(30000 + i);
                char pascal_name[256];
                BsnMakePascalName(entry.name.empty() ? "Session Song" : entry.name.c_str(), pascal_name);
                if (XAddFileResource(temp_file,
                                     ID_EMID,
                                     emid_id,
                                     pascal_name,
                                     emid.data(),
                                     static_cast<int32_t>(emid.size())) != 0)
                {
                    continue;
                }
                SongResource_Info *song_info = XNewSongResourceInfo();
                if (!song_info)
                {
                    continue;
                }
                XClearSongResourceInfo(song_info);
                song_info->songType = SONG_TYPE_RMF;
                song_info->objectResourceID = static_cast<XShortResourceID>(emid_id);
                song_info->maxMidiNotes = 56;
                song_info->maxEffects = 4;
                song_info->mixLevel = 8;
                song_info->songVolume = 127;
                song_info->reverbType = 1;
                {
                    const size_t tlen = entry.name.size();
                    char *title_copy = static_cast<char *>(XNewPtr(static_cast<int32_t>(tlen + 1)));
                    if (title_copy)
                    {
                        std::memcpy(title_copy, entry.name.c_str(), tlen);
                        title_copy[tlen] = 0;
                        song_info->title = title_copy;
                    }
                }
                SongResource *song_res = XNewSongFromSongResourceInfo(song_info);
                XDisposeSongResourceInfo(song_info);
                if (!song_res)
                {
                    continue;
                }
                (void)XAddFileResource(temp_file,
                                       ID_SONG,
                                       song_id,
                                       pascal_name,
                                       song_res,
                                       XGetPtrSize(song_res));
                XDisposeSongPtr(song_res);
            }
        }

        if (need_encrypt)
        {
            const int32_t plain_snd = XCountFileResourcesOfType(temp_file, ID_SND);
            const int32_t csnd = XCountFileResourcesOfType(temp_file, ID_CSND);
            if (plain_snd > 0 || csnd > 0)
            {
                UpdateBusyProgress(0.45f, "Encrypting samples (batch)…");
                (void)BAERmfEditorBank_BeginBatchSnd(temp_token);
                uint32_t inst_count = 0;
                if (BAERmfEditorBank_GetInstrumentCount(temp_token, &inst_count) == BAE_NO_ERROR)
                {
                    for (uint32_t ii = 0; ii < inst_count; ++ii)
                    {
                        if ((ii & 31u) == 0u && inst_count > 0u)
                        {
                            UpdateBusyProgress(0.45f + 0.30f * (static_cast<float>(ii) /
                                                                static_cast<float>(inst_count)),
                                               "Encrypting samples (batch)…");
                        }
                        uint32_t split_count = 0;
                        if (BAERmfEditorBank_GetInstrumentSampleCount(temp_token, ii, &split_count) !=
                            BAE_NO_ERROR)
                        {
                            continue;
                        }
                        for (uint32_t si = 0; si < split_count; ++si)
                        {
                            BAERmfEditorBankSampleInfo sinfo;
                            std::memset(&sinfo, 0, sizeof(sinfo));
                            if (BAERmfEditorBank_GetInstrumentSampleInfo(temp_token, ii, si, &sinfo) !=
                                BAE_NO_ERROR)
                            {
                                continue;
                            }
                            if (sinfo.frameCount == 0 && sinfo.bitDepth == 0)
                            {
                                continue;
                            }
                            if (sinfo.sndStorageType == BAE_EDITOR_SND_STORAGE_ESND)
                            {
                                continue;
                            }
                            (void)BAERmfEditorBank_SetSampleSndStorageType(
                                temp_token, ii, si, BAE_EDITOR_SND_STORAGE_ESND);
                        }
                    }
                }
                UpdateBusyProgress(0.78f, "Committing encrypted samples…");
                (void)BAERmfEditorBank_CommitBatchSnd(temp_token);
            }
        }

        if (need_drop)
        {
            UpdateBusyProgress(0.85f, "Dropping unused samples…");
            (void)DropUnreferencedSamplesFromBankFile(temp_file, temp_token);
        }

        UpdateBusyProgress(0.92f, "Writing bank file…");
        /* Direct image write — avoids SaveToFile's second full serialize. */
        const bool wrote = WriteBankFileImageToPath(temp_file, out_path);
        XFileClose(temp_file);
        if (wrote)
        {
            if (remapped)
            {
                uint32_t reason = 0;
                (void)BankRequiresZsb(&reason);
                char reason_buf[256] = {0};
                BAEZMFReasonCodeToString(reason, reason_buf, sizeof(reason_buf));
                SetStatus(std::string("Exported bank as .zsb (") + reason_buf + "): " +
                          FileNameFromPath(out_path));
            }
            else
            {
                SetStatus(std::string("Exported bank: ") + FileNameFromPath(out_path));
            }
        }
        else
        {
            SetStatus("Export Bank failed: write");
        }
    }

    /* ---- Export as Audio ---- */

    struct AudioExportCodecChoice
    {
        const char *label;
        BAEFileType file_type;
        BAECompressionType compression;
        const char *extension;
    };

    static const AudioExportCodecChoice *AudioExportCodecTable(int *out_count)
    {
        static const AudioExportCodecChoice kTable[] = {
            {"PCM 16 WAV", BAE_WAVE_TYPE, BAE_COMPRESSION_NONE, ".wav"},
            {"AIFF", BAE_AIFF_TYPE, BAE_COMPRESSION_NONE, ".aiff"},
            {"Raw PCM", BAE_RAW_PCM, BAE_COMPRESSION_NONE, ".raw"},
#if USE_FLAC_ENCODER != FALSE
            {"FLAC Lossless", BAE_FLAC_TYPE, BAE_COMPRESSION_LOSSLESS, ".flac"},
#endif
#if USE_QOA_SUPPORT == TRUE
            {"QOA", BAE_QOA_TYPE, BAE_COMPRESSION_NONE, ".qoa"},
#endif
#if USE_MPEG_ENCODER != FALSE
            {"128kbps MP3", BAE_MPEG_TYPE, BAE_COMPRESSION_MPEG_128, ".mp3"},
            {"192kbps MP3", BAE_MPEG_TYPE, BAE_COMPRESSION_MPEG_192, ".mp3"},
            {"256kbps MP3", BAE_MPEG_TYPE, BAE_COMPRESSION_MPEG_256, ".mp3"},
            {"320kbps MP3", BAE_MPEG_TYPE, BAE_COMPRESSION_MPEG_320, ".mp3"},
#endif
#if defined(USE_VORBIS_ENCODER) && USE_VORBIS_ENCODER == TRUE
            {"96kbps Vorbis", BAE_VORBIS_TYPE, BAE_COMPRESSION_VORBIS_96, ".ogg"},
            {"128kbps Vorbis", BAE_VORBIS_TYPE, BAE_COMPRESSION_VORBIS_128, ".ogg"},
            {"256kbps Vorbis", BAE_VORBIS_TYPE, BAE_COMPRESSION_VORBIS_256, ".ogg"},
            {"320kbps Vorbis", BAE_VORBIS_TYPE, BAE_COMPRESSION_VORBIS_320, ".ogg"},
#endif
#if defined(USE_OPUS_ENCODER) && USE_OPUS_ENCODER == TRUE
            {"64kbps Opus", BAE_OPUS_TYPE, BAE_COMPRESSION_OPUS_64, ".opus"},
            {"96kbps Opus", BAE_OPUS_TYPE, BAE_COMPRESSION_OPUS_96, ".opus"},
            {"128kbps Opus", BAE_OPUS_TYPE, BAE_COMPRESSION_OPUS_128, ".opus"},
            {"256kbps Opus", BAE_OPUS_TYPE, BAE_COMPRESSION_OPUS_256, ".opus"},
#endif
        };
        if (out_count)
        {
            *out_count = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
        }
        return kTable;
    }

    void OpenExportAudioDialog()
    {
        if (!m_document && !m_song)
        {
            SetStatus("No song to export as audio");
            return;
        }
        m_export_audio_codec_index = 0;
        m_export_audio_open = true;
    }

    void DrawExportAudioDialog()
    {
        if (!m_export_audio_open)
        {
            return;
        }
        ImGui::OpenPopup("Export as Audio");
        if (!ImGui::BeginPopupModal("Export as Audio",
                                    &m_export_audio_open,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        int codec_count = 0;
        const AudioExportCodecChoice *codecs = AudioExportCodecTable(&codec_count);
        if (m_export_audio_codec_index < 0 || m_export_audio_codec_index >= codec_count)
        {
            m_export_audio_codec_index = 0;
        }
        if (ImGui::BeginCombo("Codec", codecs[m_export_audio_codec_index].label))
        {
            for (int i = 0; i < codec_count; ++i)
            {
                const bool selected = (i == m_export_audio_codec_index);
                if (ImGui::Selectable(codecs[i].label, selected))
                {
                    m_export_audio_codec_index = i;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Offline render (no speaker playback). Loop follows Player setting.");
        ImGui::Spacing();
        if (ImGui::Button("Export...", ImVec2(100, 0)))
        {
            const char *ext = codecs[m_export_audio_codec_index].extension;
            char default_name[256];
            const std::string stem = SuggestedExportFileStem();
            std::snprintf(default_name, sizeof(default_name), "%s%s", stem.c_str(), ext);
            m_export_audio_open = false;
            ImGui::CloseCurrentPopup();
            m_pending_dialog_action = DialogAction::ExportAudio;
            static const SDL_DialogFileFilter filters[] = {
                {"Audio files", "wav;aiff;aif;raw;flac;qoa;mp3;ogg;opus"},
                {"All files (*.*)", "*"},
            };
            SDL_ShowSaveFileDialog(OnFileDialogResult,
                                   this,
                                   m_main_window,
                                   filters,
                                   static_cast<int>(SDL_arraysize(filters)),
                                   default_name);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0)))
        {
            m_export_audio_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void ExportSessionSongAsAudioToPath(std::string path)
    {
        if (!m_mixer)
        {
            SetStatus("Export as Audio failed: no mixer");
            return;
        }
        if (!m_song)
        {
            if (!ReloadPlaybackFromDocument(false))
            {
                SetStatus("Export as Audio failed: could not prepare song");
                return;
            }
        }
        if (!m_song)
        {
            SetStatus("Export as Audio failed: no song");
            return;
        }

        int codec_count = 0;
        const AudioExportCodecChoice *codecs = AudioExportCodecTable(&codec_count);
        if (m_export_audio_codec_index < 0 || m_export_audio_codec_index >= codec_count)
        {
            m_export_audio_codec_index = 0;
        }
        const AudioExportCodecChoice &choice = codecs[m_export_audio_codec_index];
        if (!EndsWith(path, choice.extension))
        {
            /* Replace trailing extension if present. */
            const size_t dot = path.find_last_of('.');
            if (dot != std::string::npos && path.find_last_of("/\\") < dot)
            {
                path = path.substr(0, dot);
            }
            path += choice.extension;
        }

        const bool was_playing = m_song_started;
        BAESong_Stop(m_song, FALSE);
        m_song_started = false;
        BAESong_SetMicrosecondPosition(m_song, 0);
        /* Offline render is always one pass (ignore Player loop for duration). */
        BAESong_SetLoops(m_song, 0);

        BAEResult start = BAEMixer_StartOutputToFile(m_mixer,
                                                     (BAEPathName)path.c_str(),
                                                     choice.file_type,
                                                     choice.compression);
        if (start != BAE_NO_ERROR)
        {
            SetStatus(std::string("Export as Audio start failed: ") + FormatBAEError(start));
            return;
        }

        BAEMixer_SetDefaultReverb(m_mixer, static_cast<BAEReverbType>(m_reverb_type));
        BAEMixer_SetOutputGain(m_mixer, m_volume_percent);
        const float tempo_ratio =
            static_cast<float>(std::max(1, m_song_settings_tempo_percent)) / 100.0f;
        (void)BAESong_SetMasterTempo(m_song, FLOAT_TO_UNSIGNED_FIXED(tempo_ratio));
        BAESong_Preroll(m_song);
        if (BAESong_Start(m_song, 0) != BAE_NO_ERROR)
        {
            BAEMixer_StopOutputToFile();
            SetStatus("Export as Audio failed: song start");
            return;
        }
        m_song_started = true;

        for (int settle = 0; settle < 8; ++settle)
        {
            (void)BAEMixer_ServiceAudioOutputToFile(m_mixer);
            BAE_WaitMicroseconds(1000);
        }
        if (BAEMixer_PrimeAudioOutputToFile(m_mixer, m_song) != BAE_NO_ERROR)
        {
            BAESong_Stop(m_song, FALSE);
            m_song_started = false;
            BAEMixer_StopOutputToFile();
            SetStatus("Export as Audio failed: prime");
            return;
        }

        uint32_t song_len_us = 0;
        (void)BAESong_GetMicrosecondLength(m_song, &song_len_us);
        int stall = 0;
        uint32_t last_pos = 0;
        for (;;)
        {
            BAE_BOOL done = FALSE;
            (void)BAESong_IsDone(m_song, &done);
            if (done)
            {
                break;
            }
            const BAEResult svc = BAEMixer_ServiceAudioOutputToFile(m_mixer);
            if (svc != BAE_NO_ERROR)
            {
                break;
            }
            uint32_t pos = 0;
            (void)BAESong_GetMicrosecondPosition(m_song, &pos);
            if (song_len_us > 0 && pos >= song_len_us)
            {
                break;
            }
            if (pos == last_pos)
            {
                if (++stall > 2000)
                {
                    break;
                }
            }
            else
            {
                stall = 0;
                last_pos = pos;
            }
        }

        BAESong_Stop(m_song, FALSE);
        m_song_started = false;
        BAEMixer_StopOutputToFile();
        BAESong_SetLoops(m_song, m_loop_enabled ? 32767 : 0);
        (void)was_playing;
        SetStatus(std::string("Exported audio: ") + FileNameFromPath(path));
    }

    /* ---- Song Settings ---- */

    void OpenSongSettingsDialog()
    {
        m_song_settings_open = true;
    }

    void DrawSongSettingsDialog()
    {
        if (!m_song_settings_open)
        {
            return;
        }
        bool open = m_song_settings_open;
        if (ImGui::Begin("Song Settings", &open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            static const char *reverb_labels[] = {
                "None", "Type 1", "Type 2", "Type 3", "Type 4", "Type 5", "Type 6",
                "Type 7", "Type 8", "Type 9", "Type 10", "Type 11",
#if USE_NEO_EFFECTS == TRUE
                "Type 12", "Type 13", "Type 14", "Type 15", "Type 16",
                "Type 17", "Type 18", "Custom",
#endif
            };
            int reverb_choice =
                std::clamp(m_reverb_type - static_cast<int>(BAE_REVERB_TYPE_1),
                           0,
                           static_cast<int>(IM_ARRAYSIZE(reverb_labels)) - 1);
            if (ImGui::Combo("Reverb", &reverb_choice, reverb_labels, IM_ARRAYSIZE(reverb_labels)))
            {
                m_reverb_type = static_cast<int>(BAE_REVERB_TYPE_1) + reverb_choice;
                if (m_mixer)
                {
                    BAEMixer_SetDefaultReverb(m_mixer, static_cast<BAEReverbType>(m_reverb_type));
                }
            }
            if (ImGui::SliderInt("Tempo %", &m_song_settings_tempo_percent, 25, 200))
            {
                if (m_song)
                {
                    const float ratio =
                        static_cast<float>(std::max(1, m_song_settings_tempo_percent)) / 100.0f;
                    (void)BAESong_SetMasterTempo(m_song, FLOAT_TO_UNSIGNED_FIXED(ratio));
                }
            }
            if (ImGui::SliderInt("Volume %", &m_volume_percent, 0, 150))
            {
                if (m_mixer)
                {
                    BAEMixer_SetOutputGain(m_mixer, m_volume_percent);
                }
            }
            if (ImGui::InputInt("Max MIDI voices", &m_song_settings_voices))
            {
                m_song_settings_voices = std::clamp(m_song_settings_voices, 8, 128);
            }
            if (ImGui::Button("Apply Voices (reopen mixer)"))
            {
                if (CreateMixer(false))
                {
                    SetStatus("Mixer reopened with new voice count");
                }
            }
            ImGui::Spacing();
            if (ImGui::Button("Re-Import MIDI..."))
            {
                m_pending_dialog_action = DialogAction::ReImportMidi;
                static const SDL_DialogFileFilter filters[] = {
                    {"MIDI files (*.mid;*.midi;*.kar;*.rmi)", "mid;midi;kar;rmi"},
                    {"All files (*.*)", "*"},
                };
                SDL_ShowOpenFileDialog(OnFileDialogResult,
                                       this,
                                       m_main_window,
                                       filters,
                                       static_cast<int>(SDL_arraysize(filters)),
                                       nullptr,
                                       false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Close"))
            {
                open = false;
            }
        }
        ImGui::End();
        m_song_settings_open = open;
    }

    void ReImportMidiFromPath(const std::string &path)
    {
        if (path.empty())
        {
            return;
        }
        std::vector<unsigned char> bytes;
        if (!ReadEntireFileBytes(path.c_str(), bytes) || bytes.empty())
        {
            SetStatus("Re-Import MIDI failed: read");
            return;
        }
        BAERmfEditorDocument *new_doc =
            BAERmfEditorDocument_LoadFromMemory(bytes.data(),
                                               static_cast<uint32_t>(bytes.size()),
                                               BAE_MIDI_TYPE);
        if (!new_doc)
        {
            SetStatus("Re-Import MIDI failed: parse");
            return;
        }
        if (m_document)
        {
            /* Preserve info fields from the previous document when present. */
            for (int t = 0; t < static_cast<int>(INFO_TYPE_COUNT); ++t)
            {
                const char *v =
                    BAERmfEditorDocument_GetInfo(m_document, static_cast<BAEInfoType>(t));
                if (v && v[0])
                {
                    BAERmfEditorDocument_SetInfo(new_doc, static_cast<BAEInfoType>(t), v);
                }
            }
            BAERmfEditorDocument_Delete(m_document);
        }
        m_document = new_doc;
        m_loaded_doc_path = path;
        m_document_dirty = true;
        (void)SyncSessionSongsFromDocument();
        RefreshSongOverridesFromDocument();
        UpdateSongRowFromDocument(path, "MIDI");
        (void)ReloadPlaybackFromDocument(true);
        SetStatus(std::string("Re-imported MIDI: ") + FileNameFromPath(path));
    }
