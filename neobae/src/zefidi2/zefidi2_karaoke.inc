/* Karaoke lyric capture + foldable panel (methods of Zefidi2App). */

#if SUPPORT_KARAOKE == TRUE

    static void KaraokeLyricThunk(struct GM_Song * /*song*/,
                                  const char *lyric,
                                  uint32_t /*t_us*/,
                                  void *ref)
    {
        auto *app = static_cast<Zefidi2App *>(ref);
        if (!app || !lyric)
        {
            return;
        }
        app->OnKaraokeFragment(lyric);
    }

    void KaraokeReset()
    {
        m_karaoke_line.clear();
        m_karaoke_prev.clear();
        m_karaoke_current.clear();
    }

    void OnKaraokeFragment(const char *lyric)
    {
        if (!lyric || !m_karaoke_enabled)
        {
            return;
        }
        for (const char *p = lyric; *p; ++p)
        {
            if (*p == '/' || *p == '\\')
            {
                if (!m_karaoke_line.empty())
                {
                    m_karaoke_prev = m_karaoke_current;
                    m_karaoke_current = m_karaoke_line;
                    m_karaoke_line.clear();
                }
            }
            else if (*p == '\r')
            {
                /* skip */
            }
            else
            {
                m_karaoke_line.push_back(*p);
            }
        }
    }

    void SetupKaraokeCallbacks()
    {
        KaraokeReset();
        if (!m_karaoke_enabled || !m_song)
        {
            return;
        }
        if (BAESong_SetLyricCallback(m_song, KaraokeLyricThunk, this) != BAE_NO_ERROR)
        {
            /* Meta fallback omitted (needs static app ptr); lyric path is preferred. */
            (void)BAESong_SetMetaEventCallback(m_song, nullptr, nullptr);
        }
    }

    void ClearKaraokeCallbacks()
    {
        if (m_song)
        {
            (void)BAESong_SetLyricCallback(m_song, nullptr, nullptr);
            (void)BAESong_SetMetaEventCallback(m_song, nullptr, nullptr);
        }
    }

    void DrawKaraokeSection()
    {
        if (!m_karaoke_enabled)
        {
            return;
        }
        if (!ImGui::CollapsingHeader("Karaoke"))
        {
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.12f, 0.12f, 1.0f));
        ImGui::BeginChild("##kara", ImVec2(0.0f, 52.0f), ImGuiChildFlags_Borders);
        if (!m_karaoke_prev.empty())
        {
            ImGui::TextDisabled("%s", m_karaoke_prev.c_str());
        }
        const std::string show =
            m_karaoke_current.empty()
                ? (m_karaoke_line.empty() ? std::string("…") : m_karaoke_line)
                : m_karaoke_current;
        ImGui::TextColored(Zefidi2Theme::Accent(), "%s", show.c_str());
        if (!m_karaoke_current.empty() && !m_karaoke_line.empty())
        {
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled("%s", m_karaoke_line.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

#endif /* SUPPORT_KARAOKE */
