/* BAEScript editor modal (methods of Zefidi2App). */

#if SUPPORT_BAESCRIPT == TRUE

    void FreeScript()
    {
        if (m_script_ctx)
        {
            BAEScript_Free(m_script_ctx);
            m_script_ctx = nullptr;
        }
    }

    void CompileScript()
    {
        FreeScript();
        if (!m_script_text_buf[0])
        {
            m_script_status = "Empty script";
            return;
        }
        m_script_ctx = BAEScript_LoadString(m_script_text_buf);
        if (!m_script_ctx)
        {
            m_script_status = "Compile failed";
            return;
        }
        BAEScript_SetOutputCallback(
            m_script_ctx,
            [](const char *text, void *userdata) {
                auto *app = static_cast<Zefidi2App *>(userdata);
                if (app && text)
                {
                    app->m_script_console.append(text);
                    if (app->m_script_console.size() > 8000)
                    {
                        app->m_script_console.erase(0, app->m_script_console.size() - 6000);
                    }
                }
            },
            this);
        BindScriptToSong();
        m_script_status = "Compiled OK";
        m_script_dirty = false;
    }

    void BindScriptToSong()
    {
        if (m_script_ctx && m_song)
        {
            BAEScript_SetSong(m_script_ctx, m_song);
        }
    }

    void TickScript()
    {
        if (!m_script_enabled || !m_script_ctx || !m_song || m_is_audio_file)
        {
            return;
        }
        BAEScript_SetSong(m_script_ctx, m_song);
        BAEScript_SetExporting(m_script_ctx, m_exporting ? 1 : 0);
        BAEScript_Tick(m_script_ctx, GetPosMs(), GetLenMs());
    }

    void LoadScriptFromDisk()
    {
        const std::string path = ConfigPath("zefidi2_script.txt");
        FILE *fp = std::fopen(path.c_str(), "r");
        if (!fp)
        {
            return;
        }
        m_script_text_buf[0] = 0;
        char buf[1024];
        size_t used = 0;
        while (std::fgets(buf, sizeof(buf), fp))
        {
            const size_t n = std::strlen(buf);
            if (used + n + 1 >= sizeof(m_script_text_buf))
            {
                break;
            }
            std::memcpy(m_script_text_buf + used, buf, n);
            used += n;
            m_script_text_buf[used] = 0;
        }
        std::fclose(fp);
        m_script_dirty = true;
    }

    void SaveScriptToDisk()
    {
        const std::string path = ConfigPath("zefidi2_script.txt");
        FILE *fp = std::fopen(path.c_str(), "w");
        if (!fp)
        {
            return;
        }
        std::fputs(m_script_text_buf, fp);
        std::fclose(fp);
    }

    void DrawScriptModal()
    {
        if (m_script_open)
        {
            ImGui::OpenPopup("BAEScript##zefidi2");
        }
        ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("BAEScript##zefidi2", &m_script_open))
        {
            ImGui::Checkbox("Enabled", &m_script_enabled);
            ImGui::SameLine();
            if (ImGui::Button("Compile"))
            {
                CompileScript();
            }
            ImGui::SameLine();
            if (ImGui::Button("Save"))
            {
                SaveScriptToDisk();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", m_script_status.c_str());

            if (ImGui::InputTextMultiline("##script", m_script_text_buf, sizeof(m_script_text_buf),
                                          ImVec2(-1.0f, -80.0f)))
            {
                m_script_dirty = true;
            }
            ImGui::TextUnformatted("Console");
            ImGui::BeginChild("##scriptout", ImVec2(-1.0f, 50.0f), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(m_script_console.c_str());
            ImGui::EndChild();
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                m_script_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

#endif /* SUPPORT_BAESCRIPT */
