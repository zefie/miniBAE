/* Phase 5 — BE2-inspired Player transport (included inside EditorApp). */

    void ApplyVelocityCurveToSongs()
    {
        m_velocity_curve = std::clamp(m_velocity_curve, 0, 5);
        if (m_song)
        {
            (void)BAESong_SetVelocityCurve(m_song, m_velocity_curve);
        }
        if (m_preview_song)
        {
            (void)BAESong_SetVelocityCurve(m_preview_song, m_velocity_curve);
        }
    }

    bool FindNrpnAtTick0(unsigned char msb,
                         unsigned char lsb,
                         uint16_t *out_track,
                         MeRpEntry *out_entry) const
    {
        if (!m_document || !out_track || !out_entry)
        {
            return false;
        }
        uint16_t track_count = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &track_count) != BAE_NO_ERROR ||
            track_count == 0)
        {
            return false;
        }
        for (uint16_t t = 0; t < track_count; ++t)
        {
            std::vector<MeRpEntry> entries;
            MeScanTrackRps(t, &entries);
            for (const MeRpEntry &e : entries)
            {
                if (!e.is_rpn && !e.is_null_only && e.msb == msb && e.lsb == lsb && e.tick == 0 &&
                    e.data_digits > 0)
                {
                    *out_track = t;
                    *out_entry = e;
                    return true;
                }
            }
        }
        return false;
    }

    void SyncPlayerMixNrpnsFromDocument()
    {
        if (!m_document)
        {
            return;
        }
        uint16_t track = 0;
        MeRpEntry entry;
        if (FindNrpnAtTick0(4, 0, &track, &entry))
        {
            m_velocity_curve = std::clamp(static_cast<int>(entry.data & 0x7Fu), 0, 5);
        }
        else
        {
            int doc_curve = m_velocity_curve;
            if (BAERmfEditorDocument_GetVelocityCurve(m_document, &doc_curve) == BAE_NO_ERROR)
            {
                m_velocity_curve = std::clamp(doc_curve, 0, 5);
            }
        }
        if (FindNrpnAtTick0(7, 0, &track, &entry))
        {
            const int verb = static_cast<int>(entry.data & 0x7Fu);
            if (verb >= static_cast<int>(BAE_REVERB_TYPE_1) &&
                verb <= static_cast<int>(BAE_REVERB_TYPE_19))
            {
                m_reverb_type = verb;
            }
        }
    }

    /* Replace/insert a single NRPN at tick 0 (caller owns MeBeginUndo/MeCommitUndo). */
    bool ReplaceNrpnAtTick0(unsigned char msb, unsigned char lsb, unsigned char data)
    {
        if (!m_document)
        {
            return false;
        }
        uint16_t target_track = 0;
        MeRpEntry existing;
        if (FindNrpnAtTick0(msb, lsb, &target_track, &existing))
        {
            MeDeleteRpEntry(target_track, existing);
        }
        const unsigned char digit = static_cast<unsigned char>(data & 0x7Fu);
        return MeInsertRpSequence(target_track, 0, false, msb, lsb, &digit, 1, true, 0, true);
    }

    bool WritePlayerMixNrpnsToSong()
    {
        if (!m_document)
        {
            return false;
        }
        m_velocity_curve = std::clamp(m_velocity_curve, 0, 5);
        m_reverb_type = std::clamp(m_reverb_type,
                                   static_cast<int>(BAE_REVERB_TYPE_1),
                                   static_cast<int>(BAE_REVERB_TYPE_19));
        uint16_t track_count = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &track_count) != BAE_NO_ERROR ||
            track_count == 0)
        {
            SetStatus("Save to Song: song has no tracks");
            return false;
        }

        MeBeginUndo();
        if (!ReplaceNrpnAtTick0(4, 0, static_cast<unsigned char>(m_velocity_curve)) ||
            !ReplaceNrpnAtTick0(7, 0, static_cast<unsigned char>(m_reverb_type)))
        {
            MeCancelUndo();
            SetStatus("Save to Song: NRPN write failed");
            return false;
        }
        MeCommitUndo();
        SetStatus("Saved velocity curve + reverb NRPNs at tick 0");
        return true;
    }

    void UpdateChannelVuMeters()
    {
        const uint32_t now = SDL_GetTicks();
        float dt_ms = 16.0f;
        if (m_channel_vu_last_tick_ms != 0 && now >= m_channel_vu_last_tick_ms)
        {
            dt_ms = static_cast<float>(now - m_channel_vu_last_tick_ms);
            if (dt_ms < 1.0f)
            {
                dt_ms = 1.0f;
            }
            if (dt_ms > 100.0f)
            {
                dt_ms = 100.0f;
            }
        }
        m_channel_vu_last_tick_ms = now;

        float ch_l[16] = {};
        float ch_r[16] = {};
        if (m_mixer)
        {
            GM_GetRealtimeChannelLevels(ch_l, ch_r);
        }

        /* Match zefidi: time-based EMA so high refresh rates don't flicker. */
        constexpr float k_attack_ms = 30.0f;
        constexpr float k_release_ms = 140.0f;
        auto ema_alpha = [](float dt, float tau) -> float {
            if (tau <= 1.0f)
            {
                return 1.0f;
            }
            const float a = 1.0f - std::exp(-dt / tau);
            if (a < 0.0f)
            {
                return 0.0f;
            }
            if (a > 1.0f)
            {
                return 1.0f;
            }
            return a;
        };

        for (int ch = 0; ch < 16; ++ch)
        {
            float &vu = m_channel_vu[static_cast<size_t>(ch)];
            if (ChannelEffectivelyMuted(ch))
            {
                vu = 0.0f;
                continue;
            }
            float lvl = 0.0f;
            if (m_mixer)
            {
                lvl = (ch_l[ch] + ch_r[ch]) * 0.5f;
                if (lvl < 0.0f)
                {
                    lvl = 0.0f;
                }
                if (lvl > 1.0f)
                {
                    lvl = 1.0f;
                }
            }
            const float tau = (lvl > vu) ? k_attack_ms : k_release_ms;
            const float alpha = ema_alpha(dt_ms, tau);
            vu = vu * (1.0f - alpha) + lvl * alpha;
            if (vu < 0.005f && lvl <= 0.0f)
            {
                vu = 0.0f;
            }
        }
    }

    void DrawChannelVuMeter(int ch, float height)
    {
        const ImVec2 size(6.0f, height);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (!dl)
        {
            return;
        }
        const ImVec2 p1(p0.x + size.x, p0.y + size.y);
        dl->AddRectFilled(p0, p1, IM_COL32(28, 30, 34, 255));
        dl->AddRect(p0, p1, IM_COL32(60, 64, 72, 255));

        float lvl = m_channel_vu[static_cast<size_t>(ch)];
        if (lvl < 0.0f)
        {
            lvl = 0.0f;
        }
        if (lvl > 1.0f)
        {
            lvl = 1.0f;
        }
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
            if (frac < 0.5f)
            {
                const float p = frac / 0.5f;
                col = IM_COL32(static_cast<int>(20.0f + 180.0f * p), 200, 20, 255);
            }
            else
            {
                const float p = (frac - 0.5f) / 0.5f;
                col = IM_COL32(static_cast<int>(200.0f + 55.0f * p),
                               static_cast<int>(200.0f * (1.0f - p)),
                               20,
                               255);
            }
            const float y = p0.y + size.y - pad - 1.0f - static_cast<float>(yoff);
            dl->AddLine(ImVec2(gx0, y), ImVec2(gx1, y), col);
        }
    }

    void DrawPlayerWindow()
    {
        ImGui::SetNextWindowSizeConstraints(ImVec2(780.0f, 420.0f), ImVec2(4096.0f, 4096.0f));
        if (!ImGui::Begin("Player", nullptr, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        PollSessionSongFinished();

        auto path_basename = [](const std::string &path) -> std::string {
            if (path.empty())
            {
                return {};
            }
            const size_t slash = path.find_last_of("/\\");
            return (slash == std::string::npos) ? path : path.substr(slash + 1);
        };

        std::string song_title = "No song";
        std::string song_subtitle;
        if (m_session_song_index >= 0 &&
            m_session_song_index < static_cast<int>(m_session_songs.size()))
        {
            const SessionSongEntry &entry = m_session_songs[static_cast<size_t>(m_session_song_index)];
            song_title = entry.name.empty() ? "Untitled song" : entry.name;
            if (!entry.source_type.empty())
            {
                song_subtitle = entry.source_type;
            }
        }
        else if (!m_loaded_song_path.empty())
        {
            song_title = path_basename(m_loaded_song_path);
            if (song_title.empty())
            {
                song_title = m_loaded_song_path;
            }
        }
        else if (m_song_info_title[0] != '\0')
        {
            song_title = m_song_info_title;
        }

        if (!m_loaded_bank_display_name.empty())
        {
            if (!song_subtitle.empty())
            {
                song_subtitle += " · ";
            }
            song_subtitle += m_loaded_bank_display_name;
        }
        else if (!m_loaded_bank_path.empty())
        {
            if (!song_subtitle.empty())
            {
                song_subtitle += " · ";
            }
            song_subtitle += path_basename(m_loaded_bank_path);
        }

        {
            char audition_line[96];
            std::snprintf(audition_line,
                          sizeof(audition_line),
                          "Audition B%dP%03d",
                          m_selected_bank,
                          m_selected_program);
            if (!song_subtitle.empty())
            {
                song_subtitle += " · ";
            }
            song_subtitle += audition_line;
        }

        const bool doc_dirty = m_document_dirty || m_bank01_touched;

        /* ---- Header band ---- */
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.15f, 1.0f));
        ImGui::BeginChild("##PlayerHeader", ImVec2(0.0f, 56.0f), true);
        {
            ImGui::SetWindowFontScale(1.25f);
            ImGui::TextUnformatted(song_title.c_str());
            ImGui::SetWindowFontScale(1.0f);
            if (doc_dirty)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "*");
            }
            ImGui::SameLine(0.0f, 12.0f);
            if (ImGui::SmallButton("Open…"))
            {
                OpenLoadDialog();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Song Info"))
            {
                OpenSongInfoDialog();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("MIDI Editor"))
            {
                OpenMidiEditor();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Export…"))
            {
                OpenExportRmfInstrumentDialog();
            }
            if (!song_subtitle.empty())
            {
                ImGui::TextDisabled("%s", song_subtitle.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        /* ---- Transport band ---- */
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.11f, 0.13f, 1.0f));
        ImGui::BeginChild("##PlayerTransport", ImVec2(0.0f, 96.0f), true);
        {
            bool paused = false;
            bool song_done = false;
            if (m_song)
            {
                BAE_BOOL paused_flag = FALSE;
                BAE_BOOL done_flag = FALSE;
                if (BAESong_IsPaused(m_song, &paused_flag) == BAE_NO_ERROR)
                {
                    paused = (paused_flag != FALSE);
                }
                if (BAESong_IsDone(m_song, &done_flag) == BAE_NO_ERROR)
                {
                    song_done = (done_flag != FALSE);
                }
            }
            const char *play_pause_label =
                (!m_song_started || paused || song_done) ? "Play" : "Pause";

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18.0f, 10.0f));
            if (ImGui::Button(play_pause_label, ImVec2(96.0f, 0.0f)))
            {
                PlaySessionSong();
            }
            ImGui::SameLine();
            if (ImGui::Button("Stop", ImVec2(72.0f, 0.0f)))
            {
                StopSessionSong();
            }
            ImGui::PopStyleVar();

            ImGui::SameLine();
            {
                const bool loop_on = m_loop_enabled;
                if (loop_on)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.35f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.55f, 0.42f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.22f, 0.40f, 0.32f, 1.0f));
                }
                if (ImGui::Button(loop_on ? "Loop On" : "Loop Off", ImVec2(84.0f, 0.0f)))
                {
                    m_loop_enabled = !m_loop_enabled;
                    if (m_song)
                    {
                        BAESong_SetLoops(m_song, m_loop_enabled ? 32767 : 0);
                    }
                }
                if (loop_on)
                {
                    ImGui::PopStyleColor(3);
                }
            }

            uint32_t pos_us = 0;
            uint32_t len_us = 0;
            if (m_song)
            {
                BAESong_GetMicrosecondPosition(m_song, &pos_us);
                BAESong_GetMicrosecondLength(m_song, &len_us);
            }
            const uint32_t pos_s = pos_us / 1000000U;
            const uint32_t len_s = len_us / 1000000U;
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8.0f);
            ImGui::Text("%u:%02u / %u:%02u", pos_s / 60U, pos_s % 60U, len_s / 60U, len_s % 60U);

            ImGui::SetNextItemWidth(-1.0f);
            if (m_song && len_us > 0)
            {
                uint32_t seek_us = pos_us;
                if (ImGui::SliderScalar("##Position",
                                       ImGuiDataType_U32,
                                       &seek_us,
                                       &m_seek_min_us,
                                       &len_us,
                                       ""))
                {
                    BAESong_SetMicrosecondPosition(m_song, seek_us);
                }
                if (ImGui::IsItemActive())
                {
                    const uint32_t scrub_s = seek_us / 1000000U;
                    ImGui::SetTooltip("Seek %u:%02u", scrub_s / 60U, scrub_s % 60U);
                }
            }
            else
            {
                float dummy = 0.0f;
                ImGui::BeginDisabled();
                ImGui::SliderFloat("##PositionEmpty", &dummy, 0.0f, 1.0f, "");
                ImGui::EndDisabled();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        /* ---- Mix band ---- */
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.12f, 0.14f, 1.0f));
        ImGui::BeginChild("##PlayerMix", ImVec2(0.0f, 104.0f), true);
        {
            const float label_gap = ImGui::GetStyle().ItemInnerSpacing.x;

            /* Row 1: Volume | Transpose | Tempo */
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::SliderInt("##Volume", &m_volume_percent, 0, 150))
            {
                if (m_mixer)
                {
                    BAEMixer_SetOutputGain(m_mixer, m_volume_percent);
                }
            }
            ImGui::SameLine(0.0f, label_gap);
            ImGui::TextUnformatted("Volume");
            if (ImGui::IsItemClicked())
            {
                m_volume_percent = 100;
                if (m_mixer)
                {
                    BAEMixer_SetOutputGain(m_mixer, m_volume_percent);
                }
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            m_transpose_semitones = std::clamp(m_transpose_semitones, -24, 24);
            if (ImGui::SliderInt("##Transpose", &m_transpose_semitones, -24, 24, "%+d"))
            {
                if (m_song)
                {
                    (void)BAESong_SetTranspose(m_song, m_transpose_semitones);
                }
            }
            ImGui::SameLine(0.0f, label_gap);
            ImGui::TextUnformatted("Transpose");
            if (ImGui::IsItemClicked())
            {
                m_transpose_semitones = 0;
                if (m_song)
                {
                    (void)BAESong_SetTranspose(m_song, 0);
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Song transpose in semitones (−24…+24).\nClick label to reset to 0.");
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::SliderInt("##Tempo", &m_song_settings_tempo_percent, 25, 200))
            {
                if (m_song)
                {
                    const float ratio =
                        static_cast<float>(std::max(1, m_song_settings_tempo_percent)) / 100.0f;
                    (void)BAESong_SetMasterTempo(m_song, FLOAT_TO_UNSIGNED_FIXED(ratio));
                }
            }
            ImGui::SameLine(0.0f, label_gap);
            ImGui::TextUnformatted("Tempo %");
            if (ImGui::IsItemClicked())
            {
                m_song_settings_tempo_percent = 100;
                if (m_song)
                {
                    (void)BAESong_SetMasterTempo(m_song, FLOAT_TO_UNSIGNED_FIXED(1.0f));
                }
            }

            /* Row 2: Reverb | Velocity Curve | Save to Song */
            static const char *reverb_labels[] = {
                "None",
                "Igor's Closet",
                "Igor's Garage",
                "Igor's Acoustic Lab",
                "Igor's Cavern",
                "Igor's Dungeon",
                "Small Reflections",
                "Early Reflections",
                "Basement",
                "Banquet Hall",
                "Catacombs",
                "Neo Room",
                "Neo Hall",
                "Neo Cavern",
                "Neo Dungeon",
                "Neo Nokia",
                "MobileBAE",
                "Neo Tap Delay",
                "Custom",
            };
            const int reverb_max = static_cast<int>(IM_ARRAYSIZE(reverb_labels)) - 1;
            int reverb_choice =
                std::clamp(m_reverb_type - static_cast<int>(BAE_REVERB_TYPE_1), 0, reverb_max);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Reverb", &reverb_choice, reverb_labels, IM_ARRAYSIZE(reverb_labels)))
            {
                m_reverb_type = static_cast<int>(BAE_REVERB_TYPE_1) + reverb_choice;
                if (m_mixer)
                {
                    BAEMixer_SetDefaultReverb(m_mixer, static_cast<BAEReverbType>(m_reverb_type));
                }
                if (m_reverb_type == static_cast<int>(BAE_REVERB_TYPE_19))
                {
                    OpenCustomReverbDialog();
                }
                if (m_velocity_curve_save_to_song)
                {
                    (void)WritePlayerMixNrpnsToSong();
                }
            }
#if USE_NEO_EFFECTS == TRUE
            const bool neo_reverb_editable =
                (m_reverb_type >= static_cast<int>(BAE_REVERB_TYPE_12) &&
                 m_reverb_type != static_cast<int>(BAE_REVERB_TYPE_17) &&
                 m_reverb_type != static_cast<int>(BAE_REVERB_TYPE_18));
            if (neo_reverb_editable)
            {
                ImGui::SameLine();
                if (ImGui::Button("Edit…"))
                {
                    OpenCustomReverbDialog();
                }
            }
#endif

            static const char *velocity_curve_labels[] = {
                "0 — Default S",
                "1 — Peaky S",
                "2 — WebTV",
                "3 — 2× Exponential",
                "4 — 2× Linear",
                "5 — Passthrough",
            };
            m_velocity_curve = std::clamp(m_velocity_curve, 0, 5);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::Combo("Velocity Curve",
                             &m_velocity_curve,
                             velocity_curve_labels,
                             IM_ARRAYSIZE(velocity_curve_labels)))
            {
                ApplyVelocityCurveToSongs();
                if (m_velocity_curve_save_to_song)
                {
                    (void)WritePlayerMixNrpnsToSong();
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(m_document == nullptr);
            if (ImGui::Checkbox("Save to Song", &m_velocity_curve_save_to_song))
            {
                if (m_velocity_curve_save_to_song)
                {
                    if (!WritePlayerMixNrpnsToSong())
                    {
                        m_velocity_curve_save_to_song = false;
                    }
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(
                    "When enabled, writes tick-0 NRPNs into the song:\n"
                    "NRPN 4,0 velocity curve and NRPN 7,0 reverb type.");
            }

            BAEAudioInfo info;
            std::memset(&info, 0, sizeof(info));
            if (m_mixer)
            {
                BAEMixer_GetRealtimeStatus(m_mixer, &info);
            }
            UpdateKeyboardVoiceActivity(info);
            const float voices_ratio =
                std::min(1.0f, static_cast<float>(info.voicesActive) / 128.0f);
            char voices_label[32];
            std::snprintf(voices_label, sizeof(voices_label), "Voices %d", static_cast<int>(info.voicesActive));
            ImGui::ProgressBar(voices_ratio, ImVec2(-1.0f, 14.0f), voices_label);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        /* ---- Channels mute pad (LMB mute, RMB solo) + per-channel VU ---- */
        ImGui::SeparatorText("Channels");
        UpdateChannelVuMeters();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
        constexpr float k_ch_btn_h = 28.0f;
        for (int ch = 0; ch < 16; ++ch)
        {
            ImGui::PushID(ch);
            char label[8];
            std::snprintf(label, sizeof(label), "%02d", ch + 1);
            const bool soloed = (m_channel_solo == ch);
            const bool muted = ChannelEffectivelyMuted(ch);
            if (soloed)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, NeoBaeTheme::ChannelSolo());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, NeoBaeTheme::ChannelSoloHover());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, NeoBaeTheme::ChannelSoloActive());
            }
            else if (muted)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, NeoBaeTheme::ChannelMuted());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, NeoBaeTheme::ChannelMutedHover());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, NeoBaeTheme::ChannelMutedActive());
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, NeoBaeTheme::ChannelOpen());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, NeoBaeTheme::ChannelOpenHover());
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, NeoBaeTheme::ChannelOpenActive());
            }
            ImGui::Button(label, ImVec2(36.0f, k_ch_btn_h));
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            {
                /* Any other channel ends solo; then toggle this channel's mute. */
                if (m_channel_solo >= 0 && m_channel_solo != ch)
                {
                    m_channel_solo = -1;
                }
                m_channel_muted[static_cast<size_t>(ch)] = !m_channel_muted[static_cast<size_t>(ch)];
                ApplyChannelMutes();
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            {
                if (m_channel_solo == ch)
                {
                    m_channel_solo = -1; /* RMB again clears solo */
                }
                else
                {
                    m_channel_solo = ch; /* replaces any previous solo */
                }
                ApplyChannelMutes();
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
            {
                const char *state = soloed ? "Solo"
                                           : (m_channel_muted[static_cast<size_t>(ch)] ? "Muted" : "Open");
                ImGui::SetTooltip("Channel %d — %s\nLeft: mute  Right: solo", ch + 1, state);
            }
            ImGui::SameLine(0.0f, 2.0f);
            DrawChannelVuMeter(ch, k_ch_btn_h);
            ImGui::PopID();
            if (ch < 15)
            {
                ImGui::SameLine();
            }
        }
        ImGui::PopStyleVar(2);

        /* ---- Audition ---- */
        ImGui::SeparatorText("Audition");
        std::string selected_instrument_name;
        for (const InstrumentRow &inst : m_instruments)
        {
            if (inst.bank == m_selected_bank && inst.program == m_selected_program && !inst.name.empty())
            {
                selected_instrument_name = inst.name;
                break;
            }
        }
        if (!selected_instrument_name.empty())
        {
            ImGui::Text("B%dP%03d  %s%s",
                        m_selected_bank,
                        m_selected_program,
                        selected_instrument_name.c_str(),
                        SelectedInstrumentIsPercussion() ? "  (drum / Ch 10)" : "");
        }
        else
        {
            ImGui::Text("B%dP%03d%s",
                        m_selected_bank,
                        m_selected_program,
                        SelectedInstrumentIsPercussion() ? "  (drum / Ch 10)" : "");
        }
        DrawKeyboard();

        ImGui::Spacing();
        UpdateOscilloscopeFromMixer();
        DrawOscilloscope();

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows))
        {
            /* Drops on Player add songs (same as Songs tab). */
            m_dnd_hover_target = SessionDropTarget::Songs;
        }

        ImGui::End();
    }
