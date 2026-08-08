/* Session Info + Recent Sessions — included inside NbEditorApp. */

    static constexpr int kMaxRecentSessions = 10;

    static std::string RecentSessionsLegacyFilePath()
    {
        const char *userprofile = std::getenv("USERPROFILE");
        if (userprofile && userprofile[0])
        {
            return std::string(userprofile) + "/.config/neobae/nbeditor_recent_sessions.txt";
        }
        return std::string();
    }

    static std::string RecentSessionsFilePath()
    {
        char path[1100];
        if (BAE_GetConfigFilePath(path, sizeof(path), "nbeditor_recent_sessions.txt") == 0)
        {
            return std::string(path);
        }
        return std::string("nbeditor_recent_sessions.txt");
    }

    static void RecentSessionsPrepareOnce()
    {
        static bool prepared = false;
        if (prepared)
        {
            return;
        }
        const std::string legacy = RecentSessionsLegacyFilePath();
        (void)BAE_PrepareConfigFile("nbeditor_recent_sessions.txt",
                                    legacy.empty() ? nullptr : legacy.c_str());
        prepared = true;
    }

    void LoadRecentSessionsFromDisk()
    {
        RecentSessionsPrepareOnce();
        m_recent_sessions.clear();
        std::ifstream in(RecentSessionsFilePath());
        if (!in)
        {
            return;
        }
        std::string line;
        while (std::getline(in, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }
            m_recent_sessions.push_back(line);
            if (static_cast<int>(m_recent_sessions.size()) >= kMaxRecentSessions)
            {
                break;
            }
        }
    }

    void SaveRecentSessionsToDisk() const
    {
        RecentSessionsPrepareOnce();
        const std::string path = RecentSessionsFilePath();
        (void)BAE_EnsureConfigDirectory();
        std::ofstream out(path, std::ios::trunc);
        if (!out)
        {
            return;
        }
        for (const std::string &entry : m_recent_sessions)
        {
            out << entry << '\n';
        }
    }

    void RememberRecentSession(const char *path)
    {
        if (!path || !path[0])
        {
            return;
        }
        std::string p(path);
        m_recent_sessions.erase(std::remove(m_recent_sessions.begin(), m_recent_sessions.end(), p),
                                m_recent_sessions.end());
        m_recent_sessions.insert(m_recent_sessions.begin(), p);
        if (static_cast<int>(m_recent_sessions.size()) > kMaxRecentSessions)
        {
            m_recent_sessions.resize(static_cast<size_t>(kMaxRecentSessions));
        }
        SaveRecentSessionsToDisk();
    }

    void OpenSessionInfoDialog()
    {
        m_session_info_open = true;
    }

    void DrawSessionInfoDialog()
    {
        if (!m_session_info_open)
        {
            return;
        }
        ImGui::OpenPopup("Session Info");
        if (!ImGui::BeginPopupModal("Session Info",
                                    &m_session_info_open,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        ImGui::TextUnformatted("Session");
        ImGui::Separator();
        ImGui::Text("Path: %s",
                    m_loaded_session_path.empty() ? "(unsaved)" : m_loaded_session_path.c_str());
        ImGui::Text("Songs: %zu", m_session_songs.size());
        if (m_document || !m_song_row.name.empty())
        {
            ImGui::Text("Active song: %s [%s]",
                        m_song_row.name.empty() ? "(untitled)" : m_song_row.name.c_str(),
                        m_song_row.source_type.empty() ? "?" : m_song_row.source_type.c_str());
        }
        ImGui::Text("CaSd (uncompressed masters): %d", m_session_casd_count);
        ImGui::Text("Session PCM cache: %zu (%zu bytes)",
                    m_session_pcm_cache.size(),
                    SessionPcmCacheBytes());
        ImGui::Text("App compression cache: %zu (%zu bytes)",
                    m_app_compression_cache.size(),
                    AppCompressionCacheBytes());

        ImGui::Spacing();
        ImGui::TextUnformatted("Bank");
        ImGui::Separator();
        ImGui::Text("Bank path: %s",
                    m_loaded_bank_path.empty() ? "(none / builtin)" : m_loaded_bank_path.c_str());
        if (!m_loaded_bank_display_name.empty())
        {
            ImGui::Text("Bank name: %s", m_loaded_bank_display_name.c_str());
        }
        ImGui::Text("Instruments: %zu", m_instruments.size());
        ImGui::Text("Samples: %zu", m_samples.size());

        int alias_count = 0;
        for (const InstrumentRow &row : m_instruments)
        {
            if (row.is_alias)
            {
                ++alias_count;
            }
        }
        ImGui::Text("Aliases: %d", alias_count);

        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
        {
            m_session_info_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
