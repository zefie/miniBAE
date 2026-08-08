/* Offline export modal (methods of Zefidi2App). */

    void DrawExportModal()
    {
        if (m_export_open)
        {
            ImGui::OpenPopup("Export##zefidi2");
        }
        if (ImGui::BeginPopupModal("Export##zefidi2", &m_export_open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (m_is_audio_file)
            {
                ImGui::TextWrapped("Export is available for MIDI/RMF songs, not raw audio files.");
            }
            else if (!m_song_loaded)
            {
                ImGui::TextWrapped("Load a song first.");
            }
            else
            {
                static const char *kFormats[] = {
                    "WAV (PCM)",
#if USE_FLAC_ENCODER == TRUE
                    "FLAC",
#endif
#if USE_LAME_ENCODER == TRUE || USE_MPEG_ENCODER == TRUE
                    "MP3 128k",
                    "MP3 192k",
#endif
#if USE_VORBIS_ENCODER == TRUE
                    "Vorbis 128k",
#endif
#if USE_OPUS_ENCODER == TRUE
                    "Opus 96k",
#endif
                };
                ImGui::Combo("Format", &m_export_format, kFormats, IM_ARRAYSIZE(kFormats));
                if (ImGui::Button("Export…", ImVec2(120.0f, 0.0f)))
                {
                    OpenExportSaveDialog();
                }
                if (m_exporting)
                {
                    ImGui::Text("Exporting… %d%%", m_export_progress);
                    if (ImGui::Button("Cancel"))
                    {
                        m_export_cancel = true;
                    }
                }
            }
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                m_export_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void StartExportToPath(const char *path)
    {
        if (!m_mixer || !m_song || m_is_audio_file || !path || !path[0] || m_exporting)
        {
            return;
        }

        BAEFileType file_type = BAE_WAVE_TYPE;
        BAECompressionType compression = BAE_COMPRESSION_NONE;
        int fmt = m_export_format;
        int idx = 0;
        auto match = [&](int want_type, BAECompressionType c) {
            if (fmt == idx)
            {
                file_type = static_cast<BAEFileType>(want_type);
                compression = c;
            }
            ++idx;
        };
        match(BAE_WAVE_TYPE, BAE_COMPRESSION_NONE);
#if USE_FLAC_ENCODER == TRUE
        match(BAE_FLAC_TYPE, BAE_COMPRESSION_LOSSLESS);
#endif
#if USE_LAME_ENCODER == TRUE || USE_MPEG_ENCODER == TRUE
        match(BAE_MPEG_TYPE, BAE_COMPRESSION_MPEG_128);
        match(BAE_MPEG_TYPE, BAE_COMPRESSION_MPEG_192);
#endif
#if USE_VORBIS_ENCODER == TRUE
        match(BAE_VORBIS_TYPE, BAE_COMPRESSION_VORBIS_128);
#endif
#if USE_OPUS_ENCODER == TRUE
        match(BAE_OPUS_TYPE, BAE_COMPRESSION_OPUS_96);
#endif
        (void)idx;

        const bool was_playing = m_playing;
        const uint32_t saved_ms = GetPosMs();
        Stop();

        m_exporting = true;
        m_export_cancel = false;
        m_export_progress = 0;

        if (BAEMixer_StartOutputToFile(m_mixer, (BAEPathName)path, file_type, compression) !=
            BAE_NO_ERROR)
        {
            m_exporting = false;
            SetStatus("Export start failed");
            return;
        }

        (void)BAESong_SetMicrosecondPosition(m_song, 0);
        (void)BAESong_Preroll(m_song);
        (void)BAESong_SetLoops(m_song, 0);
        if (BAESong_Start(m_song, 0) != BAE_NO_ERROR)
        {
            BAEMixer_StopOutputToFile();
            m_exporting = false;
            SetStatus("Export song start failed");
            return;
        }

        const uint32_t len_ms = GetLenMs();
        while (!m_export_cancel)
        {
            BAE_BOOL done = FALSE;
            (void)BAESong_IsDone(m_song, &done);
            if (done)
            {
                break;
            }
            if (BAEMixer_ServiceAudioOutputToFile(m_mixer) != BAE_NO_ERROR)
            {
                break;
            }
            const uint32_t pos = GetPosMs();
            if (len_ms > 0)
            {
                m_export_progress = static_cast<int>(std::min(100u, (pos * 100u) / len_ms));
            }
            /* Keep UI alive lightly during long exports. */
            SDL_PumpEvents();
        }

        BAEMixer_StopOutputToFile();
        (void)BAESong_Stop(m_song, FALSE);
        SeekMs(saved_ms);
        (void)BAESong_Preroll(m_song);
        ApplyLoop();
        m_exporting = false;
        m_export_progress = 100;
        SetStatus(m_export_cancel ? "Export cancelled" : std::string("Exported: ") + PathBasename(path));
        if (was_playing)
        {
            m_song_started = false;
            m_playing = false;
            (void)PlayPause();
        }
    }
