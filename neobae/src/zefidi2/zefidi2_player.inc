/* Single docked Player window + custom meters/seek (methods of Zefidi2App). */

    void DrawChannelVuMeter(int ch, float height)
    {
        const ImVec2 size(5.0f, height);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (!dl)
        {
            return;
        }
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);
        dl->AddRectFilled(p0, p1, IM_COL32(22, 28, 30, 255));
        dl->AddRect(p0, p1, IM_COL32(50, 60, 64, 255));

        float lvl = std::clamp(m_channel_vu[static_cast<size_t>(ch)], 0.0f, 1.0f);
        const float pad = 1.0f;
        const float inner_h = size.y - pad * 2.0f;
        const int fill_h = static_cast<int>(lvl * inner_h);
        if (fill_h <= 0 || inner_h <= 0.0f)
        {
            return;
        }
        const float gx0 = p0.x + pad;
        const float gx1 = p0.x + size.x - pad;
        for (int yoff = 0; yoff < fill_h; ++yoff)
        {
            const float frac = static_cast<float>(yoff) / inner_h;
            ImU32 col;
            if (frac < 0.55f)
            {
                const float p = frac / 0.55f;
                col = IM_COL32(static_cast<int>(20.0f + 40.0f * p),
                               static_cast<int>(160.0f + 60.0f * p),
                               static_cast<int>(140.0f + 40.0f * p), 255);
            }
            else
            {
                const float p = (frac - 0.55f) / 0.45f;
                col = IM_COL32(static_cast<int>(60.0f + 180.0f * p),
                               static_cast<int>(220.0f * (1.0f - p * 0.5f)),
                               40, 255);
            }
            const float y = p0.y + size.y - pad - 1.0f - static_cast<float>(yoff);
            dl->AddLine(ImVec2(gx0, y), ImVec2(gx1, y), col);
        }
    }

    bool DrawCustomSeekBar(const char *id, float fraction, float height)
    {
        ImGuiWindow *window = ImGui::GetCurrentWindow();
        if (!window || window->SkipItems)
        {
            return false;
        }
        const ImGuiID widget_id = window->GetID(id);
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 pos = window->DC.CursorPos;
        const ImRect bb(pos, ImVec2(pos.x + width, pos.y + height));
        ImGui::ItemSize(bb);
        if (!ImGui::ItemAdd(bb, widget_id))
        {
            return false;
        }

        bool changed = false;
        bool hovered = false;
        bool held = false;
        const bool pressed = ImGui::ButtonBehavior(bb, widget_id, &hovered, &held);
        float f = std::clamp(fraction, 0.0f, 1.0f);
        if (held || pressed)
        {
            const float x = ImGui::GetIO().MousePos.x;
            f = std::clamp((x - bb.Min.x) / (bb.Max.x - bb.Min.x), 0.0f, 1.0f);
            changed = true;
        }

        ImDrawList *dl = window->DrawList;
        dl->AddRectFilled(bb.Min, bb.Max, Zefidi2Theme::SeekTrack(), 3.0f);
        if (m_webtv_style_bar)
        {
            /* Segmented WebTV-style candy. */
            const int segs = 32;
            const float seg_w = (bb.Max.x - bb.Min.x) / static_cast<float>(segs);
            const int lit = static_cast<int>(f * segs + 0.5f);
            for (int i = 0; i < lit; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(segs);
                const ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    0.15f + 0.55f * t, 0.75f - 0.2f * t, 0.55f + 0.3f * t, 1.0f));
                const float x0 = bb.Min.x + seg_w * static_cast<float>(i) + 1.0f;
                const float x1 = bb.Min.x + seg_w * static_cast<float>(i + 1) - 1.0f;
                dl->AddRectFilled(ImVec2(x0, bb.Min.y + 2.0f), ImVec2(x1, bb.Max.y - 2.0f), col, 1.0f);
            }
        }
        else
        {
            const float fill_x = bb.Min.x + (bb.Max.x - bb.Min.x) * f;
            if (fill_x > bb.Min.x + 1.0f)
            {
                dl->AddRectFilledMultiColor(bb.Min, ImVec2(fill_x, bb.Max.y),
                                            Zefidi2Theme::SeekFillLo(), Zefidi2Theme::SeekFillHi(),
                                            Zefidi2Theme::SeekFillHi(), Zefidi2Theme::SeekFillLo());
            }
            const float hx = fill_x;
            dl->AddRectFilled(ImVec2(hx - 2.0f, bb.Min.y - 1.0f),
                              ImVec2(hx + 2.0f, bb.Max.y + 1.0f),
                              IM_COL32(230, 245, 240, 230), 2.0f);
        }
        if (hovered || held)
        {
            dl->AddRect(bb.Min, bb.Max, IM_COL32(90, 200, 190, 160), 3.0f);
        }

        if (changed)
        {
            m_seek_fraction = f;
        }
        return changed;
    }

    void DrawMasterScope(float width, float height)
    {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, height));
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (!dl)
        {
            return;
        }
        const ImVec2 p1(p0.x + width, p0.y + height);
        dl->AddRectFilled(p0, p1, Zefidi2Theme::ScopeBg(), 3.0f);
        dl->AddRect(p0, p1, Zefidi2Theme::ScopeBorder(), 3.0f);

        const int n = static_cast<int>(m_scope_history.size());
        if (n < 2)
        {
            return;
        }
        const float mid = (p0.y + p1.y) * 0.5f;
        const float amp = (height * 0.42f);
        for (int i = 1; i < n; ++i)
        {
            const int idx0 = (m_scope_write_index + i - 1) % n;
            const int idx1 = (m_scope_write_index + i) % n;
            const float v0 = m_scope_history[static_cast<size_t>(idx0)];
            const float v1 = m_scope_history[static_cast<size_t>(idx1)];
            const float x0 = p0.x + (width * static_cast<float>(i - 1) / static_cast<float>(n - 1));
            const float x1 = p0.x + (width * static_cast<float>(i) / static_cast<float>(n - 1));
            dl->AddLine(ImVec2(x0, mid - v0 * amp), ImVec2(x1, mid - v1 * amp),
                        Zefidi2Theme::ScopeWave(), 1.5f);
        }

        /* Stereo peak ticks */
        const float bar_w = 4.0f;
        const float l_h = m_master_vu_l * (height - 4.0f);
        const float r_h = m_master_vu_r * (height - 4.0f);
        dl->AddRectFilled(ImVec2(p1.x - bar_w * 2.0f - 4.0f, p1.y - 2.0f - l_h),
                          ImVec2(p1.x - bar_w - 4.0f, p1.y - 2.0f), IM_COL32(70, 210, 180, 220));
        dl->AddRectFilled(ImVec2(p1.x - bar_w - 2.0f, p1.y - 2.0f - r_h),
                          ImVec2(p1.x - 2.0f, p1.y - 2.0f), IM_COL32(90, 190, 220, 220));
    }

    void DrawPlayerWindow()
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
        ImGui::SetNextWindowSizeConstraints(ImVec2(480.0f, 280.0f), ImVec2(4096.0f, 4096.0f));
        if (!ImGui::Begin("Player", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        PollPlaybackFinished();
        UpdateChannelVuMeters();
#if SUPPORT_BAESCRIPT == TRUE
        TickScript();
#endif
#if SUPPORT_MIDI_HW == TRUE
        PollHardwareMidiInput();
#endif

        /* Header */
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.13f, 0.14f, 1.0f));
        ImGui::BeginChild("##Header", ImVec2(0.0f, 46.0f), ImGuiChildFlags_Borders);
        {
            ImGui::SetWindowFontScale(1.15f);
            ImGui::TextUnformatted(m_song_title.empty() ? "No song" : m_song_title.c_str());
            ImGui::SetWindowFontScale(1.0f);
            ImGui::SameLine(0.0f, 10.0f);
            if (ImGui::SmallButton("Open…"))
            {
                OpenLoadDialog();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Bank…"))
            {
                OpenBankDialog();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Default Bank"))
            {
                (void)TryLoadDefaultBank();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Export…"))
            {
                m_export_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Settings"))
            {
                m_settings_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("About"))
            {
                m_about_open = true;
            }
            if (!m_loaded_bank_display.empty() || m_is_rmf_file)
            {
                ImGui::TextDisabled("%s%s%s",
                                    m_loaded_bank_display.c_str(),
                                    (m_is_rmf_file && !m_loaded_bank_display.empty()) ? " · " : "",
                                    m_is_rmf_file ? "RMF" : "");
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        /* Transport */
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.11f, 0.12f, 1.0f));
        ImGui::BeginChild("##Transport", ImVec2(0.0f, 72.0f), ImGuiChildFlags_Borders);
        {
            BAE_BOOL paused = FALSE;
            if (m_song && !m_is_audio_file)
            {
                (void)BAESong_IsPaused(m_song, &paused);
            }
            const bool show_play = (!m_song_started || !m_playing || paused);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 6.0f));
            if (ImGui::Button(show_play ? "Play" : "Pause", ImVec2(72.0f, 0.0f)))
            {
                (void)PlayPause();
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop", ImVec2(56.0f, 0.0f)))
            {
                Stop();
            }
            ImGui::PopStyleVar();
            ImGui::SameLine();
            if (m_loop_enabled)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.42f, 0.36f, 1.0f));
            }
            if (ImGui::Button(m_loop_enabled ? "Loop On" : "Loop Off", ImVec2(72.0f, 0.0f)))
            {
                m_loop_enabled = !m_loop_enabled;
                ApplyLoop();
            }
            if (m_loop_enabled)
            {
                ImGui::PopStyleColor();
            }

            const uint32_t pos_ms = GetPosMs();
            const uint32_t len_ms = GetLenMs();
            const uint32_t pos_s = pos_ms / 1000u;
            const uint32_t len_s = len_ms / 1000u;
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4.0f);
            ImGui::Text("%u:%02u / %u:%02u", pos_s / 60u, pos_s % 60u, len_s / 60u, len_s % 60u);

            const float frac = (len_ms > 0) ? static_cast<float>(pos_ms) / static_cast<float>(len_ms) : 0.0f;
            if (DrawCustomSeekBar("##seek", frac, 16.0f))
            {
                SeekMs(static_cast<uint32_t>(m_seek_fraction * static_cast<float>(len_ms)));
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        /* Mix row + scope */
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.09f, 0.11f, 0.12f, 1.0f));
        ImGui::BeginChild("##Mix", ImVec2(0.0f, 88.0f), ImGuiChildFlags_Borders);
        {
            const float scope_w = 140.0f;
            ImGui::BeginGroup();
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::SliderInt("##vol", &m_volume_percent, 0, 150))
            {
                ApplyVolume();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Vol");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            if (ImGui::SliderInt("##tempo", &m_tempo_percent, 25, 200))
            {
                ApplyTempo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("T100"))
            {
                m_tempo_percent = 100;
                ApplyTempo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::SliderInt("##xpose", &m_transpose, -24, 24))
            {
                ApplyTranspose();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X0"))
            {
                m_transpose = 0;
                ApplyTranspose();
            }

            static const char *kReverbNames[] = {
                "1 None", "2 Closet", "3 Garage", "4 Acoustic", "5 Cavern", "6 Dungeon",
                "7 WebTV", "8 Early", "9 Basement", "10 Banquet", "11 Catacombs",
                "12 Neo Room", "13 Neo Hall", "14 Neo Cavern", "15 Neo Dungeon",
                "16 Neo Nokia", "17 MobileBAE", "18 Tap Delay", "19 Custom",
            };
            int reverb_idx = std::clamp(m_reverb_type - 1, 0, 18);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("##reverb", &reverb_idx, kReverbNames, IM_ARRAYSIZE(kReverbNames)))
            {
                m_reverb_type = reverb_idx + 1;
                ApplyReverb();
                if (m_reverb_type == static_cast<int>(BAE_REVERB_TYPE_19))
                {
                    ApplyCustomReverbParams();
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("EQ"))
            {
                m_eq_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Custom Verb") && m_reverb_type == static_cast<int>(BAE_REVERB_TYPE_19))
            {
                m_custom_reverb_open = true;
            }
            ImGui::SameLine();
            if (m_is_rmf_file && ImGui::SmallButton("RMF Info"))
            {
                m_rmf_info_open = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Script"))
            {
                m_script_open = true;
            }

            ImGui::SetNextItemWidth(110.0f);
            static const char *kCurves[] = {
                "Neo S", "Peaky S", "WebTV", "2x Exp", "2x Lin", "None",
            };
            if (ImGui::Combo("##vcurve", &m_velocity_curve, kCurves, IM_ARRAYSIZE(kCurves)))
            {
                ApplyVelocityCurve();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Vel curve");
            ImGui::EndGroup();

            ImGui::SameLine();
            DrawMasterScope(scope_w, 70.0f);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        /* Channels */
        ImGui::TextDisabled("Channels  (LMB mute · RMB solo)");
        const float ch_h = 36.0f;
        for (int ch = 0; ch < 16; ++ch)
        {
            if (ch > 0)
            {
                ImGui::SameLine(0.0f, 3.0f);
            }
            ImGui::PushID(ch);
            ImGui::BeginGroup();
            const bool muted = ChannelEffectivelyMuted(ch);
            const bool solo = (m_channel_solo == ch);
            if (solo)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, Zefidi2Theme::ChannelSolo());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Zefidi2Theme::ChannelSoloHover());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Zefidi2Theme::ChannelSoloActive());
            }
            else if (m_channel_muted[static_cast<size_t>(ch)])
            {
                ImGui::PushStyleColor(ImGuiCol_Button, Zefidi2Theme::ChannelMuted());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Zefidi2Theme::ChannelMutedHover());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Zefidi2Theme::ChannelMutedActive());
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, Zefidi2Theme::ChannelOpen());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Zefidi2Theme::ChannelOpenHover());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Zefidi2Theme::ChannelOpenActive());
            }
            char label[8];
            std::snprintf(label, sizeof(label), "%02d", ch + 1);
            if (ImGui::Button(label, ImVec2(28.0f, 22.0f)))
            {
                m_channel_muted[static_cast<size_t>(ch)] = !m_channel_muted[static_cast<size_t>(ch)];
                if (m_channel_solo == ch)
                {
                    m_channel_solo = -1;
                }
                ApplyChannelMutes();
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                m_channel_solo = (m_channel_solo == ch) ? -1 : ch;
                ApplyChannelMutes();
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0.0f, 1.0f);
            DrawChannelVuMeter(ch, ch_h);
            (void)muted;
            ImGui::EndGroup();
            ImGui::PopID();
        }

        ImGui::Separator();
        DrawKeyboardSection();
#if SUPPORT_KARAOKE == TRUE
        DrawKaraokeSection();
#endif
#if SUPPORT_PLAYLIST == TRUE
        DrawPlaylistSection();
#endif

        ImGui::Separator();
        ImGui::TextDisabled("%s", m_status.c_str());

        ImGui::End();
    }

    void DrawDockspace()
    {
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGuiWindowFlags host_flags =
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("zefidi2_desktop", nullptr, host_flags);
        ImGui::PopStyleVar(3);

        const ImGuiID dockspace_id = ImGui::GetID("zefidi2_dock");
        if (!m_dock_built)
        {
            m_dock_built = true;
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, vp->WorkSize);
            ImGui::DockBuilderDockWindow("Player", dockspace_id);
            ImGui::DockBuilderFinish(dockspace_id);
        }
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::End();
    }
