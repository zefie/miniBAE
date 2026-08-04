/* Session Info + Recent Sessions — included inside NbEditorApp. */

    static constexpr int kMaxRecentSessions = 10;

    static std::string RecentSessionsFilePath()
    {
        const char *home = std::getenv("HOME");
        if (home && home[0])
        {
            return std::string(home) + "/.config/neobae/nbeditor_recent_sessions.txt";
        }
        return std::string("nbeditor_recent_sessions.txt");
    }

    void LoadRecentSessionsFromDisk()
    {
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
        const std::string path = RecentSessionsFilePath();
        const size_t slash = path.find_last_of('/');
        if (slash != std::string::npos)
        {
#if !defined(_WIN32)
            const std::string dir = path.substr(0, slash);
            /* ~/.config then ~/.config/neobae — ignore errors if present. */
            const size_t mid = dir.find_last_of('/');
            if (mid != std::string::npos && mid > 0)
            {
                ::mkdir(dir.substr(0, mid).c_str(), 0755);
            }
            ::mkdir(dir.c_str(), 0755);
#endif
        }
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
