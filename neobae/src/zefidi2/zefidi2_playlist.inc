/* Foldable playlist panel (methods of Zefidi2App). */

#if SUPPORT_PLAYLIST == TRUE

    void PlaylistClear()
    {
        m_playlist.clear();
        m_playlist_index = -1;
        m_playlist_order.clear();
    }

    void PlaylistAddFile(const std::string &path)
    {
        if (path.empty())
        {
            return;
        }
        for (const auto &e : m_playlist)
        {
            if (e == path)
            {
                return;
            }
        }
        m_playlist.push_back(path);
        RebuildPlaylistOrder();
    }

    void PlaylistRemove(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_playlist.size()))
        {
            return;
        }
        m_playlist.erase(m_playlist.begin() + index);
        if (m_playlist_index >= static_cast<int>(m_playlist.size()))
        {
            m_playlist_index = static_cast<int>(m_playlist.size()) - 1;
        }
        RebuildPlaylistOrder();
    }

    void RebuildPlaylistOrder()
    {
        m_playlist_order.resize(m_playlist.size());
        for (size_t i = 0; i < m_playlist.size(); ++i)
        {
            m_playlist_order[i] = static_cast<int>(i);
        }
        if (m_playlist_shuffle && m_playlist_order.size() > 1)
        {
            for (size_t i = m_playlist_order.size() - 1; i > 0; --i)
            {
                const size_t j = static_cast<size_t>(std::rand()) % (i + 1);
                std::swap(m_playlist_order[i], m_playlist_order[j]);
            }
        }
    }

    int PlaylistNextForEndOfSong()
    {
        if (m_playlist.empty())
        {
            return -1;
        }
        if (m_playlist_repeat == 2 && m_playlist_index >= 0)
        {
            return m_playlist_index; /* track */
        }
        if (m_playlist_order.empty())
        {
            RebuildPlaylistOrder();
        }
        int pos_in_order = 0;
        for (size_t i = 0; i < m_playlist_order.size(); ++i)
        {
            if (m_playlist_order[i] == m_playlist_index)
            {
                pos_in_order = static_cast<int>(i);
                break;
            }
        }
        const int next_pos = pos_in_order + 1;
        if (next_pos < static_cast<int>(m_playlist_order.size()))
        {
            return m_playlist_order[static_cast<size_t>(next_pos)];
        }
        if (m_playlist_repeat == 1)
        {
            if (m_playlist_shuffle)
            {
                RebuildPlaylistOrder();
            }
            return m_playlist_order.empty() ? -1 : m_playlist_order[0];
        }
        return -1;
    }

    void PlaylistPlayIndex(int index, bool auto_play)
    {
        if (index < 0 || index >= static_cast<int>(m_playlist.size()))
        {
            return;
        }
        m_playlist_index = index;
        if (LoadMediaFromPath(m_playlist[static_cast<size_t>(index)].c_str()) && auto_play)
        {
            m_playing = false;
            m_song_started = false;
            (void)PlayPause();
        }
    }

    void PlaylistSaveAuto()
    {
        const std::string path = ConfigPath("zefidi2_playlist.m3u");
        FILE *fp = std::fopen(path.c_str(), "w");
        if (!fp)
        {
            return;
        }
        std::fprintf(fp, "#EXTM3U\n");
        for (const auto &e : m_playlist)
        {
            std::fprintf(fp, "%s\n", e.c_str());
        }
        std::fclose(fp);
    }

    void PlaylistLoadAuto()
    {
        const std::string path = ConfigPath("zefidi2_playlist.m3u");
        FILE *fp = std::fopen(path.c_str(), "r");
        if (!fp)
        {
            return;
        }
        char line[2048];
        while (std::fgets(line, sizeof(line), fp))
        {
            char *nl = std::strchr(line, '\n');
            if (nl)
            {
                *nl = 0;
            }
            if (!line[0] || line[0] == '#')
            {
                continue;
            }
            PlaylistAddFile(line);
        }
        std::fclose(fp);
    }

    void DrawPlaylistSection()
    {
        if (!m_playlist_enabled)
        {
            return;
        }
        if (!ImGui::CollapsingHeader("Playlist", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }
        if (ImGui::Button("Add…"))
        {
            OpenPlaylistAddDialog();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            PlaylistClear();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Shuffle", &m_playlist_shuffle);
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            RebuildPlaylistOrder();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        static const char *kRepeat[] = {"None", "All", "Track"};
        ImGui::Combo("##repeat", &m_playlist_repeat, kRepeat, 3);

        ImGui::BeginChild("##plist", ImVec2(0.0f, 120.0f), ImGuiChildFlags_Borders);
        for (int i = 0; i < static_cast<int>(m_playlist.size()); ++i)
        {
            ImGui::PushID(i);
            const bool selected = (i == m_playlist_index);
            const std::string label = PathBasename(m_playlist[static_cast<size_t>(i)]);
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick))
            {
                if (ImGui::IsMouseDoubleClicked(0))
                {
                    PlaylistPlayIndex(i, true);
                }
                else
                {
                    m_playlist_index = i;
                }
            }
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Play"))
                {
                    PlaylistPlayIndex(i, true);
                }
                if (ImGui::MenuItem("Remove"))
                {
                    PlaylistRemove(i);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

#endif /* SUPPORT_PLAYLIST */
