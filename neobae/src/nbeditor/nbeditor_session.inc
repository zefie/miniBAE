/* Session shell, Song Info, Instrument Editor, and export helpers — included inside NbEditorApp. */

    void DrawMainMenuBar()
    {
        if (!ImGui::BeginMainMenuBar())
        {
            return;
        }
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Session"))
            {
                NewSession();
            }
            if (ImGui::MenuItem("Open / Import..."))
            {
                OpenLoadDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Song Info...", nullptr, false, m_document != nullptr))
            {
                OpenSongInfoDialog();
            }
            if (ImGui::MenuItem("Export RMF/ZMF...", nullptr, false, m_document != nullptr))
            {
                OpenExportSongDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Bank", nullptr, false, m_bank_token != 0 && !m_loaded_bank_path.empty()))
            {
                if (m_bank_token && !m_loaded_bank_path.empty())
                {
                    const BAEResult r = BAERmfEditorBank_SaveToFile(
                        m_bank_token, const_cast<char *>(m_loaded_bank_path.c_str()));
                    SetStatus(r == BAE_NO_ERROR ? "Bank saved" : std::string("Bank save failed: ") + FormatBAEError(r));
                }
            }
            if (ImGui::MenuItem("Save Bank As...", nullptr, false, m_bank_token != 0))
            {
                OpenSaveBankDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit"))
            {
                m_request_quit = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Song"))
        {
            if (ImGui::MenuItem("Play", nullptr, false, m_document != nullptr || m_song != nullptr))
            {
                PlaySessionSong();
            }
            if (ImGui::MenuItem("Stop", nullptr, false, m_song != nullptr))
            {
                StopSessionSong();
            }
            if (ImGui::MenuItem("Song Info...", nullptr, false, m_document != nullptr))
            {
                OpenSongInfoDialog();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    void NewSession()
    {
        if (m_song)
        {
            BAESong_Stop(m_song, FALSE);
            BAESong_Delete(m_song);
            m_song = nullptr;
        }
        if (m_document)
        {
            BAERmfEditorDocument_Delete(m_document);
            m_document = nullptr;
        }
        m_loaded_song_path.clear();
        m_loaded_doc_path.clear();
        m_song_row = SongRow{};
        m_document_dirty = false;
        m_song_started = false;
        ClearSongOverrides();
        RefreshSongSamplesFromDocument();
        m_instrument_editor_open = false;
        m_song_info_open = false;
        SetStatus("New Session");
    }

    void ImportPathIntoSession(const std::string &selected_path)
    {
        std::string lower = ToLower(selected_path);
        std::string ext;
        const size_t dot = lower.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < lower.size())
        {
            ext = lower.substr(dot + 1);
        }

        if (ext == "nbs")
        {
            SetStatus("Session files (.nbs) are not supported in nbeditor yet");
            return;
        }
        if (IsModuleExtension(ext))
        {
            LoadModuleFromPath(selected_path.c_str());
            return;
        }
        if (ext == "mid" || ext == "midi" || ext == "kar" || ext == "rmi")
        {
            if (LoadDocumentFromPath(selected_path.c_str()))
            {
                UpdateSongRowFromDocument(selected_path, "MIDI");
                ReloadPlaybackFromDocument(true);
                SetStatus("MIDI imported into Session song");
            }
            return;
        }
        if (IsBankExtension(ext))
        {
            LoadBankFromPath(selected_path.c_str());
            return;
        }

        const bool doc_ok = LoadDocumentFromPath(selected_path.c_str());
        if (doc_ok)
        {
            const char *stype = EndsWith(selected_path, ".zmf") ? "ZMF" : "RMF";
            UpdateSongRowFromDocument(selected_path, stype);
            ReloadPlaybackFromDocument(true);
            SetStatus("Song document loaded into Session");
            return;
        }

        if (LoadSongFromPath(selected_path.c_str()))
        {
            SetStatus("Song loaded (no editable document)");
            return;
        }
        SetStatus("Selected file could not be loaded");
    }

    void UpdateSongRowFromDocument(const std::string &path, const char *source_type)
    {
        m_song_row.path = path;
        m_song_row.source_type = source_type ? source_type : "";
        m_song_row.name = FileNameFromPath(path);
        if (m_document)
        {
            const char *title = BAERmfEditorDocument_GetInfo(m_document, TITLE_INFO);
            if (title && title[0])
            {
                m_song_row.name = title;
            }
        }
        m_loaded_song_path = path;
        m_loaded_doc_path = path;
        m_document_dirty = false;
        RefreshSongSamplesFromDocument();
    }

    bool ReloadPlaybackFromDocument(bool pause_after_start)
    {
        if (!m_mixer || !m_document)
        {
            return false;
        }

        unsigned char *rmf_data = nullptr;
        uint32_t rmf_size = 0;
        uint32_t zmf_reason = 0;
        const bool use_zmf = BAERmfEditorDocument_RequiresZmf(m_document, &zmf_reason) != FALSE;
        const BAEResult save_result = BAERmfEditorDocument_SaveAsRmfToMemory(m_document,
                                                                              use_zmf,
                                                                              &rmf_data,
                                                                              &rmf_size);
        if (save_result != BAE_NO_ERROR || !rmf_data || rmf_size == 0)
        {
            if (rmf_data)
            {
                XDisposePtr((XPTR)rmf_data);
            }
            SetStatus(std::string("Serialize song failed: ") + FormatBAEError(save_result));
            return false;
        }

        if (m_song)
        {
            BAESong_Stop(m_song, FALSE);
            BAESong_Delete(m_song);
            m_song = nullptr;
        }

        BAESong song = BAESong_New(m_mixer);
        if (!song)
        {
            XDisposePtr((XPTR)rmf_data);
            SetStatus("BAESong_New failed");
            return false;
        }

        const BAEResult load_result = BAESong_LoadRmfFromMemory(song, rmf_data, rmf_size, 0, TRUE);
        XDisposePtr((XPTR)rmf_data);
        if (load_result != BAE_NO_ERROR)
        {
            BAESong_Delete(song);
            SetStatus(std::string("Playback load failed: ") + FormatBAEError(load_result));
            return false;
        }

        BAESong_Preroll(song);
        BAESong_SetLoops(song, m_loop_enabled ? 32767 : 0);
        BAEMixer_SetOutputGain(m_mixer, m_volume_percent);
        BAESong_Start(song, 0);
        if (pause_after_start)
        {
            BAESong_Pause(song);
        }
        m_song = song;
        m_song_started = true;
        m_document_dirty = false;
        ApplyChannelMutes();
        RebuildPreviewSongFromDocument();
        return true;
    }

    void RebuildPreviewSongFromDocument()
    {
        if (!m_mixer || !m_document)
        {
            return;
        }
        if (m_preview_song)
        {
            BAESong_Stop(m_preview_song, FALSE);
            BAESong_Delete(m_preview_song);
            m_preview_song = nullptr;
        }
        BAESong preview = BAESong_New(m_mixer);
        if (!preview)
        {
            return;
        }
        unsigned char *rmf_data = nullptr;
        uint32_t rmf_size = 0;
        uint32_t zmf_reason = 0;
        const bool use_zmf = BAERmfEditorDocument_RequiresZmf(m_document, &zmf_reason) != FALSE;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(m_document, use_zmf, &rmf_data, &rmf_size) != BAE_NO_ERROR ||
            !rmf_data)
        {
            BAESong_Delete(preview);
            return;
        }
        const BAEResult lr = BAESong_LoadRmfFromMemory(preview, rmf_data, rmf_size, 0, TRUE);
        XDisposePtr((XPTR)rmf_data);
        if (lr != BAE_NO_ERROR)
        {
            BAESong_Delete(preview);
            return;
        }
        BAESong_Preroll(preview);
        m_preview_song = preview;
    }

    void PlaySessionSong()
    {
        if (m_document && (m_document_dirty || !m_song))
        {
            if (!ReloadPlaybackFromDocument(false))
            {
                return;
            }
            SetStatus("Playing Session song");
            return;
        }
        if (!m_song)
        {
            SetStatus("No song to play");
            return;
        }
        BAE_BOOL paused_flag = FALSE;
        BAESong_IsPaused(m_song, &paused_flag);
        if (!m_song_started)
        {
            BAESong_SetLoops(m_song, m_loop_enabled ? 32767 : 0);
            BAESong_Preroll(m_song);
            BAESong_Start(m_song, 0);
            m_song_started = true;
        }
        else if (paused_flag)
        {
            BAESong_Resume(m_song);
        }
        else
        {
            BAESong_Pause(m_song);
        }
    }

    void StopSessionSong()
    {
        if (!m_song)
        {
            return;
        }
        BAESong_Stop(m_song, FALSE);
        BAESong_SetMicrosecondPosition(m_song, 0);
        BAESong_SetLoops(m_song, m_loop_enabled ? 32767 : 0);
        BAESong_Start(m_song, 0);
        BAESong_Pause(m_song);
        m_song_started = true;
    }

    void ExportSessionSongToPath(const std::string &path)
    {
        if (!m_document)
        {
            SetStatus("No song document to export");
            return;
        }
        const BAEResult validate = BAERmfEditorDocument_Validate(m_document);
        if (validate != BAE_NO_ERROR)
        {
            SetStatus(std::string("Validate failed: ") + FormatBAEError(validate));
            return;
        }

        std::string out_path = path;
        const bool want_midi = EndsWith(out_path, ".mid") || EndsWith(out_path, ".midi");
        if (want_midi)
        {
            if (!BAERmfEditorDocument_CanSaveAsMidi(m_document))
            {
                SetStatus("Document cannot be saved as MIDI");
                return;
            }
            const BAEResult r = BAERmfEditorDocument_SaveAsMidi(m_document, const_cast<char *>(out_path.c_str()));
            SetStatus(r == BAE_NO_ERROR ? "Exported MIDI" : std::string("MIDI export failed: ") + FormatBAEError(r));
            return;
        }

        uint32_t reason = 0;
        const bool requires_zmf = BAERmfEditorDocument_RequiresZmf(m_document, &reason) != FALSE;
        if (requires_zmf && !EndsWith(out_path, ".zmf"))
        {
            // Force .zmf when required.
            const size_t dot = out_path.find_last_of('.');
            if (dot != std::string::npos)
            {
                out_path = out_path.substr(0, dot) + ".zmf";
            }
            else
            {
                out_path += ".zmf";
            }
            char reason_buf[256] = {0};
            BAEZMFReasonCodeToString(reason, reason_buf, sizeof(reason_buf));
            SetStatus(std::string("ZMF required (") + reason_buf + "); exporting as .zmf");
        }

        const BAEResult r = BAERmfEditorDocument_SaveAsRmf(m_document, const_cast<char *>(out_path.c_str()));
        if (r == BAE_NO_ERROR)
        {
            SetStatus(std::string("Exported ") + FileNameFromPath(out_path));
        }
        else
        {
            SetStatus(std::string("Export failed: ") + FormatBAEError(r));
        }
    }

    void RefreshSongSamplesFromDocument()
    {
        // Drop previous song-sourced sample rows, keep bank rows.
        m_samples.erase(std::remove_if(m_samples.begin(),
                                       m_samples.end(),
                                       [](const SampleRow &s) { return s.source == SampleSource::Song; }),
                        m_samples.end());

        if (!m_document)
        {
            return;
        }

        uint32_t sample_count = 0;
        if (BAERmfEditorDocument_GetSampleCount(m_document, &sample_count) != BAE_NO_ERROR)
        {
            return;
        }

        uint32_t row_index = static_cast<uint32_t>(m_samples.size());
        for (uint32_t i = 0; i < sample_count; ++i)
        {
            BAERmfEditorSampleInfo info;
            std::memset(&info, 0, sizeof(info));
            if (BAERmfEditorDocument_GetSampleInfo(m_document, i, &info) != BAE_NO_ERROR)
            {
                continue;
            }
            SampleRow row;
            row.index = row_index++;
            row.document_sample_index = i;
            row.source = SampleSource::Song;
            row.is_custom = true;
            row.program = static_cast<int>(info.program);
            row.root_key = static_cast<int>(info.rootKey);
            row.low_key = static_cast<int>(info.lowKey);
            row.high_key = static_cast<int>(info.highKey);
            row.frame_count = info.sampleInfo.waveFrames;
            row.loop_start = info.sampleInfo.startLoop;
            row.loop_end = info.sampleInfo.endLoop;
            row.bit_depth = info.sampleInfo.bitSize ? info.sampleInfo.bitSize : 16;
            row.channels = info.sampleInfo.channels ? info.sampleInfo.channels : 1;
            row.sample_rate_hz = info.sampleInfo.sampledRate
                                     ? static_cast<uint32_t>(info.sampleInfo.sampledRate >> 16)
                                     : 44100;
            row.snd_resource_id = 0;
            if (info.displayName && info.displayName[0])
            {
                row.name = std::string("[Song] ") + info.displayName;
            }
            else
            {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "[Song] Sample %u", i);
                row.name = buf;
            }
            m_samples.push_back(row);
        }
    }

    void DrawSessionWindow()
    {
        if (!ImGui::Begin("Session"))
        {
            ImGui::End();
            return;
        }

        if (ImGui::BeginTabBar("##SessionTabs"))
        {
            if (ImGui::BeginTabItem("Songs"))
            {
                DrawSongsListWindow();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Instruments"))
            {
                DrawInstrumentListWindow();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Samples"))
            {
                DrawSampleListWindow();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    void DrawSongsListWindow()
    {
        ImGui::Text("Show:");
        ImGui::SameLine();
        ImGui::TextUnformatted("All");
        ImGui::Separator();
        if (m_song_row.name.empty() && !m_document)
        {
            ImGui::TextDisabled("(no song in Session — File → Open / Import)");
            return;
        }

        const char *label = m_song_row.name.empty() ? "(untitled song)" : m_song_row.name.c_str();
        char line[512];
        std::snprintf(line,
                      sizeof(line),
                      "%s  [%s]",
                      label,
                      m_song_row.source_type.empty() ? "?" : m_song_row.source_type.c_str());
        const bool selected = true;
        if (ImGui::Selectable(line, selected))
        {
            /* single song selection */
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            PlaySessionSong();
        }
        if (ImGui::BeginPopupContextItem("song_ctx"))
        {
            if (ImGui::MenuItem("Play"))
            {
                PlaySessionSong();
            }
            if (ImGui::MenuItem("Song Info...", nullptr, false, m_document != nullptr))
            {
                OpenSongInfoDialog();
            }
            if (ImGui::MenuItem("Export RMF/ZMF...", nullptr, false, m_document != nullptr))
            {
                OpenExportSongDialog();
            }
            ImGui::EndPopup();
        }
        if (m_document_dirty)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Document modified — play/export will re-serialize");
        }
    }

    void OpenSongInfoDialog()
    {
        if (!m_document)
        {
            return;
        }
        auto copy_info = [this](BAEInfoType t, char *buf, size_t n) {
            const char *v = BAERmfEditorDocument_GetInfo(m_document, t);
            std::snprintf(buf, n, "%s", v ? v : "");
        };
        copy_info(TITLE_INFO, m_song_info_title, sizeof(m_song_info_title));
        copy_info(COMPOSER_INFO, m_song_info_composer, sizeof(m_song_info_composer));
        copy_info(COPYRIGHT_INFO, m_song_info_copyright, sizeof(m_song_info_copyright));
        copy_info(PERFORMED_BY_INFO, m_song_info_performed, sizeof(m_song_info_performed));
        uint32_t bpm = 120;
        uint16_t tpq = 480;
        BAERmfEditorDocument_GetTempoBPM(m_document, &bpm);
        BAERmfEditorDocument_GetTicksPerQuarter(m_document, &tpq);
        m_song_info_bpm = static_cast<int>(bpm);
        m_song_info_tpq = static_cast<int>(tpq);
        bool loop_enabled = false;
        uint32_t loop_start = 0, loop_end = 0;
        int32_t loop_count = 0;
        BAERmfEditorDocument_GetMidiLoopMarkers(m_document, &loop_enabled, &loop_start, &loop_end, &loop_count);
        m_song_info_loop_enabled = loop_enabled;
        m_song_info_loop_start = static_cast<int>(loop_start);
        m_song_info_loop_end = static_cast<int>(loop_end);
        m_song_info_loop_count = static_cast<int>(loop_count);
        BAERmfEditorMidiStorageType storage = BAE_EDITOR_MIDI_STORAGE_ECMI;
        BAERmfEditorDocument_GetMidiStorageType(m_document, &storage);
        m_song_info_storage = static_cast<int>(storage);
        m_song_info_open = true;
    }

    void DrawSongInfoDialog()
    {
        if (!m_song_info_open)
        {
            return;
        }
        bool open = m_song_info_open;
        if (ImGui::Begin("Song Info", &open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::InputText("Title", m_song_info_title, sizeof(m_song_info_title));
            ImGui::InputText("Composer", m_song_info_composer, sizeof(m_song_info_composer));
            ImGui::InputText("Copyright", m_song_info_copyright, sizeof(m_song_info_copyright));
            ImGui::InputText("Performed by", m_song_info_performed, sizeof(m_song_info_performed));
            ImGui::InputInt("Tempo BPM", &m_song_info_bpm);
            ImGui::InputInt("Ticks/Quarter", &m_song_info_tpq);
            ImGui::Checkbox("Loop markers", &m_song_info_loop_enabled);
            ImGui::InputInt("Loop start tick", &m_song_info_loop_start);
            ImGui::InputInt("Loop end tick", &m_song_info_loop_end);
            ImGui::InputInt("Loop count", &m_song_info_loop_count);
            static const char *storage_labels[] = {"CMID", "ECMI", "EMID", "MIDI"};
            ImGui::Combo("MIDI storage", &m_song_info_storage, storage_labels, IM_ARRAYSIZE(storage_labels));
            if (ImGui::Button("Apply"))
            {
                if (m_document)
                {
                    BAERmfEditorDocument_SetInfo(m_document, TITLE_INFO, m_song_info_title);
                    BAERmfEditorDocument_SetInfo(m_document, COMPOSER_INFO, m_song_info_composer);
                    BAERmfEditorDocument_SetInfo(m_document, COPYRIGHT_INFO, m_song_info_copyright);
                    BAERmfEditorDocument_SetInfo(m_document, PERFORMED_BY_INFO, m_song_info_performed);
                    BAERmfEditorDocument_SetTempoBPM(m_document, static_cast<uint32_t>(std::max(1, m_song_info_bpm)));
                    BAERmfEditorDocument_SetTicksPerQuarter(m_document, static_cast<uint16_t>(std::max(1, m_song_info_tpq)));
                    BAERmfEditorDocument_SetMidiLoopMarkers(m_document,
                                                             m_song_info_loop_enabled,
                                                             static_cast<uint32_t>(std::max(0, m_song_info_loop_start)),
                                                             static_cast<uint32_t>(std::max(0, m_song_info_loop_end)),
                                                             static_cast<int32_t>(m_song_info_loop_count));
                    BAERmfEditorDocument_SetMidiStorageType(
                        m_document, static_cast<BAERmfEditorMidiStorageType>(m_song_info_storage));
                    m_document_dirty = true;
                    UpdateSongRowFromDocument(m_loaded_doc_path.empty() ? m_song_row.path : m_loaded_doc_path,
                                              m_song_row.source_type.c_str());
                    SetStatus("Song Info applied");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Close"))
            {
                open = false;
            }
        }
        ImGui::End();
        m_song_info_open = open;
    }

    void OpenInstrumentEditorForSelection()
    {
        if (m_selected_instrument < 0 || m_selected_instrument >= static_cast<int>(m_instruments.size()))
        {
            return;
        }
        const InstrumentRow &inst = m_instruments[static_cast<size_t>(m_selected_instrument)];
        m_ie_inst_id = inst.inst_id;
        m_ie_instrument_index = inst.instrument_index;
        m_ie_from_song = inst.from_song_document || inst.has_song_override;
        m_ie_bank = inst.bank;
        m_ie_program = inst.program;
        std::memset(&m_ie_ext, 0, sizeof(m_ie_ext));
        BAEResult r = BAE_PARAM_ERR;
        if (m_ie_from_song && m_document && m_ie_inst_id != 0)
        {
            r = BAERmfEditorDocument_GetInstrumentExtInfo(m_document, m_ie_inst_id, &m_ie_ext);
        }
        else if (m_bank_token)
        {
            r = BAERmfEditorBank_GetInstrumentExtInfo(m_bank_token, m_ie_instrument_index, &m_ie_ext);
            if (r == BAE_NO_ERROR)
            {
                m_ie_inst_id = m_ie_ext.instID ? m_ie_ext.instID : inst.inst_id;
            }
        }
        if (r != BAE_NO_ERROR)
        {
            SetStatus(std::string("Instrument Ext load failed: ") + FormatBAEError(r));
            return;
        }
        m_ie_dirty = false;
        m_ie_page = 0;
        m_instrument_editor_open = true;
        SetStatus(std::string("Editing instrument ") + inst.name);
    }

    void ApplyInstrumentEditor(bool live_patch)
    {
        BAEResult r = BAE_PARAM_ERR;
        if (m_ie_from_song && m_document)
        {
            r = BAERmfEditorDocument_SetInstrumentExtInfo(m_document, m_ie_inst_id, &m_ie_ext);
            if (r == BAE_NO_ERROR)
            {
                m_document_dirty = true;
            }
        }
        else if (m_bank_token)
        {
            r = BAERmfEditorBank_SetInstrumentExtInfo(m_bank_token, m_ie_instrument_index, &m_ie_ext);
        }
        if (r != BAE_NO_ERROR)
        {
            SetStatus(std::string("Instrument apply failed: ") + FormatBAEError(r));
            return;
        }
        m_ie_dirty = false;
        if (live_patch)
        {
            BAESong audition = GetAuditionSong();
            if (audition && m_ie_inst_id != 0)
            {
                BAESong_LoadInstrument(audition, static_cast<BAE_INSTRUMENT>(m_ie_inst_id));
                BAESong_PatchLoadedInstrumentExtInfo(audition,
                                                     static_cast<BAE_INSTRUMENT>(m_ie_inst_id),
                                                     &m_ie_ext);
            }
        }
        SetStatus("Instrument applied");
    }

    void DrawInstrumentEditorDialog()
    {
        if (!m_instrument_editor_open)
        {
            return;
        }
        bool open = m_instrument_editor_open;
        ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Instrument Editor", &open))
        {
            ImGui::Text("%s  instID=%u  %s",
                        m_ie_ext.displayName ? m_ie_ext.displayName : "(instrument)",
                        static_cast<unsigned>(m_ie_inst_id),
                        m_ie_from_song ? "[Song]" : "[Bank]");
            if (m_ie_dirty)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "modified");
            }

            if (ImGui::BeginTabBar("##IEPages"))
            {
                if (ImGui::BeginTabItem("Keymap"))
                {
                    m_ie_page = 0;
                    DrawInstrumentEditorKeymapPage();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Filter"))
                {
                    m_ie_page = 1;
                    int freq = m_ie_ext.LPF_frequency;
                    int res = m_ie_ext.LPF_resonance;
                    int amt = m_ie_ext.LPF_lowpassAmount;
                    if (ImGui::InputInt("LPF frequency", &freq))
                    {
                        m_ie_ext.LPF_frequency = freq;
                        m_ie_dirty = true;
                    }
                    if (ImGui::InputInt("LPF resonance", &res))
                    {
                        m_ie_ext.LPF_resonance = res;
                        m_ie_dirty = true;
                    }
                    if (ImGui::InputInt("LPF lowpass amount", &amt))
                    {
                        m_ie_ext.LPF_lowpassAmount = amt;
                        m_ie_dirty = true;
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Volume"))
                {
                    m_ie_page = 2;
                    int pan = m_ie_ext.panPlacement;
                    int vol = m_ie_ext.miscParameter2;
                    int rev = m_ie_ext.defaultReverbSend;
                    int cho = m_ie_ext.defaultChorusSend;
                    if (ImGui::SliderInt("Pan", &pan, -63, 63))
                    {
                        m_ie_ext.panPlacement = static_cast<char>(pan);
                        m_ie_dirty = true;
                    }
                    if (ImGui::InputInt("Volume (misc2)", &vol))
                    {
                        m_ie_ext.miscParameter2 = static_cast<int16_t>(vol);
                        m_ie_dirty = true;
                    }
                    if (ImGui::InputInt("Default reverb send", &rev))
                    {
                        m_ie_ext.defaultReverbSend = static_cast<int16_t>(rev);
                        m_ie_dirty = true;
                    }
                    if (ImGui::InputInt("Default chorus send", &cho))
                    {
                        m_ie_ext.defaultChorusSend = static_cast<int16_t>(cho);
                        m_ie_dirty = true;
                    }
                    ImGui::SeparatorText("Volume ADSR");
                    int stages = static_cast<int>(m_ie_ext.volumeADSR.stageCount);
                    if (ImGui::InputInt("Stage count", &stages))
                    {
                        stages = std::clamp(stages, 0, BAE_EDITOR_MAX_ADSR_STAGES);
                        m_ie_ext.volumeADSR.stageCount = static_cast<uint32_t>(stages);
                        m_ie_dirty = true;
                    }
                    if (ImGui::BeginTable("adsr", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                    {
                        ImGui::TableSetupColumn("Level");
                        ImGui::TableSetupColumn("Time");
                        ImGui::TableSetupColumn("Flags");
                        ImGui::TableSetupColumn("#");
                        ImGui::TableHeadersRow();
                        for (int i = 0; i < stages; ++i)
                        {
                            ImGui::TableNextRow();
                            ImGui::PushID(i);
                            ImGui::TableSetColumnIndex(0);
                            if (ImGui::InputInt("##lvl", &m_ie_ext.volumeADSR.stages[i].level))
                            {
                                m_ie_dirty = true;
                            }
                            ImGui::TableSetColumnIndex(1);
                            if (ImGui::InputInt("##tm", &m_ie_ext.volumeADSR.stages[i].time))
                            {
                                m_ie_dirty = true;
                            }
                            ImGui::TableSetColumnIndex(2);
                            if (ImGui::InputInt("##fl", &m_ie_ext.volumeADSR.stages[i].flags))
                            {
                                m_ie_dirty = true;
                            }
                            ImGui::TableSetColumnIndex(3);
                            ImGui::Text("%d", i);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Modulation"))
                {
                    m_ie_page = 3;
                    bool def_mod = m_ie_ext.hasDefaultMod != FALSE;
                    if (ImGui::Checkbox("Has DEFAULT_MOD", &def_mod))
                    {
                        m_ie_ext.hasDefaultMod = def_mod ? TRUE : FALSE;
                        m_ie_dirty = true;
                    }
                    ImGui::Text("LFOs: %u", static_cast<unsigned>(m_ie_ext.lfoCount));
                    for (uint32_t i = 0; i < m_ie_ext.lfoCount && i < BAE_EDITOR_MAX_LFOS; ++i)
                    {
                        ImGui::PushID(static_cast<int>(i) + 1000);
                        ImGui::Separator();
                        ImGui::Text("LFO %u", static_cast<unsigned>(i));
                        if (ImGui::InputInt("destination", &m_ie_ext.lfos[i].destination))
                        {
                            m_ie_dirty = true;
                        }
                        if (ImGui::InputInt("period", &m_ie_ext.lfos[i].period))
                        {
                            m_ie_dirty = true;
                        }
                        if (ImGui::InputInt("waveShape", &m_ie_ext.lfos[i].waveShape))
                        {
                            m_ie_dirty = true;
                        }
                        if (ImGui::InputInt("DC_feed", &m_ie_ext.lfos[i].DC_feed))
                        {
                            m_ie_dirty = true;
                        }
                        if (ImGui::InputInt("level", &m_ie_ext.lfos[i].level))
                        {
                            m_ie_dirty = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::Text("Curves: %u", static_cast<unsigned>(m_ie_ext.curveCount));
                    for (uint32_t i = 0; i < m_ie_ext.curveCount && i < BAE_EDITOR_MAX_CURVES; ++i)
                    {
                        ImGui::PushID(static_cast<int>(i) + 2000);
                        if (ImGui::InputInt("tieFrom", &m_ie_ext.curves[i].tieFrom))
                        {
                            m_ie_dirty = true;
                        }
                        if (ImGui::InputInt("tieTo", &m_ie_ext.curves[i].tieTo))
                        {
                            m_ie_dirty = true;
                        }
                        int cc = m_ie_ext.curves[i].curveCount;
                        if (ImGui::InputInt("points", &cc))
                        {
                            m_ie_ext.curves[i].curveCount = static_cast<int16_t>(
                                std::clamp(cc, 0, BAE_EDITOR_MAX_ADSR_STAGES));
                            m_ie_dirty = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Output"))
                {
                    m_ie_page = 4;
                    int root = m_ie_ext.midiRootKey;
                    int misc1 = m_ie_ext.miscParameter1;
                    if (ImGui::InputInt("MIDI root key", &root))
                    {
                        m_ie_ext.midiRootKey = static_cast<int16_t>(root);
                        m_ie_dirty = true;
                    }
                    if (ImGui::InputInt("miscParameter1", &misc1))
                    {
                        m_ie_ext.miscParameter1 = static_cast<int16_t>(misc1);
                        m_ie_dirty = true;
                    }
                    auto flag_bit = [this](const char *label, unsigned char &flags, unsigned char bit) {
                        bool on = (flags & bit) != 0;
                        if (ImGui::Checkbox(label, &on))
                        {
                            if (on)
                            {
                                flags = static_cast<unsigned char>(flags | bit);
                            }
                            else
                            {
                                flags = static_cast<unsigned char>(flags & ~bit);
                            }
                            m_ie_dirty = true;
                        }
                    };
                    flag_bit("Use sample rate", m_ie_ext.flags1, ZBF_useSampleRate);
                    flag_bit("Sample and hold", m_ie_ext.flags1, ZBF_sampleAndHold);
                    flag_bit("Avoid reverb", m_ie_ext.flags1, ZBF_avoidReverb);
                    flag_bit("Disable SND looping", m_ie_ext.flags1, ZBF_disableSndLooping);
                    flag_bit("Play at sampled freq", m_ie_ext.flags2, ZBF_playAtSampledFreq);
#if USE_ZMF_SUPPORT == TRUE
                    flag_bit("Advanced interpolation (ZMF)", m_ie_ext.flags2, ZBF_advancedInterpolation);
                    flag_bit("Sample offset start (ZMF)", m_ie_ext.flags2, ZBF_enableSampleOffsetStart);
#endif
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            if (ImGui::Button("Apply"))
            {
                ApplyInstrumentEditor(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply + Live Patch"))
            {
                ApplyInstrumentEditor(true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Close"))
            {
                open = false;
            }
        }
        ImGui::End();
        m_instrument_editor_open = open;
    }

    void DrawInstrumentEditorKeymapPage()
    {
        if (!m_bank_token || m_ie_from_song)
        {
            ImGui::TextWrapped("Keymap split editing is available for bank instruments. Song-embedded instruments: use Volume/Filter/Modulation/Output pages.");
            return;
        }
        uint32_t split_count = 0;
        if (BAERmfEditorBank_GetInstrumentSampleCount(m_bank_token, m_ie_instrument_index, &split_count) != BAE_NO_ERROR)
        {
            ImGui::TextDisabled("No splits");
            return;
        }
        if (ImGui::Button("Add split"))
        {
            (void)BAERmfEditorBank_GrowInstrumentSampleSlots(m_bank_token,
                                                             m_ie_instrument_index,
                                                             split_count + 1);
            RefreshListsFromBank();
            RefreshSongSamplesFromDocument();
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete selected split") && m_ie_selected_split >= 0)
        {
            (void)BAERmfEditorBank_DeleteInstrumentSample(m_bank_token,
                                                          m_ie_instrument_index,
                                                          static_cast<uint32_t>(m_ie_selected_split),
                                                          false);
            m_ie_selected_split = -1;
            RefreshListsFromBank();
            RefreshSongSamplesFromDocument();
        }

        if (ImGui::BeginTable("keymap", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 260)))
        {
            ImGui::TableSetupColumn("Sel");
            ImGui::TableSetupColumn("Low");
            ImGui::TableSetupColumn("High");
            ImGui::TableSetupColumn("Root");
            ImGui::TableSetupColumn("SND");
            ImGui::TableSetupColumn("Vol");
            ImGui::TableHeadersRow();
            for (uint32_t s = 0; s < split_count; ++s)
            {
                BAERmfEditorBankSampleInfo info;
                std::memset(&info, 0, sizeof(info));
                if (BAERmfEditorBank_GetInstrumentSampleInfo(m_bank_token, m_ie_instrument_index, s, &info) != BAE_NO_ERROR)
                {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(s));
                ImGui::TableSetColumnIndex(0);
                bool sel = (m_ie_selected_split == static_cast<int>(s));
                if (ImGui::Selectable("##sel", sel, ImGuiSelectableFlags_SpanAllColumns))
                {
                    m_ie_selected_split = static_cast<int>(s);
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    // Open sample editor for this bank split.
                    for (size_t i = 0; i < m_samples.size(); ++i)
                    {
                        if (m_samples[i].source == SampleSource::Bank &&
                            m_samples[i].instrument_index == m_ie_instrument_index &&
                            m_samples[i].split_index == s)
                        {
                            m_selected_sample = static_cast<int>(i);
                            OpenSampleEditorForSelection();
                            break;
                        }
                    }
                }
                int low = info.lowKey, high = info.highKey, root = info.rootKey, vol = info.splitVolume;
                int snd = info.sndResourceID;
                ImGui::TableSetColumnIndex(1);
                if (ImGui::InputInt("##low", &low))
                {
                    info.lowKey = static_cast<unsigned char>(std::clamp(low, 0, 127));
                    BAERmfEditorBank_SetInstrumentSampleInfo(m_bank_token, m_ie_instrument_index, s, &info);
                }
                ImGui::TableSetColumnIndex(2);
                if (ImGui::InputInt("##high", &high))
                {
                    info.highKey = static_cast<unsigned char>(std::clamp(high, 0, 127));
                    BAERmfEditorBank_SetInstrumentSampleInfo(m_bank_token, m_ie_instrument_index, s, &info);
                }
                ImGui::TableSetColumnIndex(3);
                if (ImGui::InputInt("##root", &root))
                {
                    info.rootKey = static_cast<unsigned char>(std::clamp(root, 0, 127));
                    BAERmfEditorBank_SetInstrumentSampleInfo(m_bank_token, m_ie_instrument_index, s, &info);
                }
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%d", snd);
                ImGui::TableSetColumnIndex(5);
                if (ImGui::InputInt("##vol", &vol))
                {
                    info.splitVolume = static_cast<int16_t>(vol);
                    BAERmfEditorBank_SetInstrumentSampleInfo(m_bank_token, m_ie_instrument_index, s, &info);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Double-click a split row to open Sample Editor");
    }
