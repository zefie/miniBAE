/* Cache clear confirms and Resource Usage dialogs.
 * Included inside NbEditorApp (after bank_add helpers). */

    static XResourceType kCasdType()
    {
        return FOUR_CHAR('C', 'a', 'S', 'd');
    }

    size_t AppCompressionCacheBytes() const
    {
        size_t total = 0;
        for (const auto &kv : m_app_compression_cache)
        {
            total += kv.second.size();
        }
        return total;
    }

    size_t SessionPcmCacheBytes() const
    {
        size_t total = 0;
        for (const auto &kv : m_session_pcm_cache)
        {
            total += kv.second.pcm.size();
        }
        return total;
    }

    int CountBankCasdResources() const
    {
        if (!m_bank_token)
        {
            return 0;
        }
        const int32_t count =
            XCountFileResourcesOfType(reinterpret_cast<XFILE>(m_bank_token), kCasdType());
        return count > 0 ? static_cast<int>(count) : 0;
    }

    void RequestClearCompressionCacheConfirm()
    {
        m_confirm_clear_cache_open = true;
    }

    void RequestDeleteUncompressedConfirm()
    {
        m_confirm_delete_pcm_open = true;
    }

    void PerformClearCompressionCache()
    {
        const size_t entries = m_app_compression_cache.size();
        const size_t bytes = AppCompressionCacheBytes();
        m_app_compression_cache.clear();
        char status[256];
        std::snprintf(status,
                      sizeof(status),
                      "Cleared compression cache: %zu preview(s), %zu bytes",
                      entries,
                      bytes);
        SetStatus(status);
    }

    void PerformDeleteUncompressedOriginals()
    {
        const size_t pcm_entries = m_session_pcm_cache.size();
        const size_t pcm_bytes = SessionPcmCacheBytes();
        int casd_deleted = 0;

        if (m_bank_token && (pcm_entries > 0 || CountBankCasdResources() > 0))
        {
            if (EnsureActiveBankWritable())
            {
                XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
                /* Delete CaSd IDs tracked in session PCM cache first. */
                for (const auto &kv : m_session_pcm_cache)
                {
                    const XLongResourceID id = static_cast<XLongResourceID>(kv.first);
                    if (XExistsFileResource(bank, kCasdType(), id) != FALSE)
                    {
                        if (XDeleteFileResource(bank, kCasdType(), id, FALSE) != FALSE)
                        {
                            ++casd_deleted;
                        }
                    }
                }
                /* Also remove any remaining CaSd resources (session masters on disk). */
                for (;;)
                {
                    const int32_t remaining = XCountFileResourcesOfType(bank, kCasdType());
                    if (remaining <= 0)
                    {
                        break;
                    }
                    XLongResourceID rid = 0;
                    int32_t size = 0;
                    char name[256];
                    name[0] = 0;
                    XPTR data = XGetIndexedFileResource(bank, kCasdType(), &rid, 0, name, &size);
                    if (data)
                    {
                        XDisposePtr(data);
                    }
                    if (XDeleteFileResource(bank, kCasdType(), rid, FALSE) == FALSE)
                    {
                        break;
                    }
                    ++casd_deleted;
                    if (casd_deleted > 10000)
                    {
                        break; /* safety */
                    }
                }
                (void)XFinalizeEditorResourceFile(bank);
            }
        }

        m_session_pcm_cache.clear();
        m_session_casd_count = 0;

        char status[256];
        std::snprintf(status,
                      sizeof(status),
                      "Deleted uncompressed originals: %zu PCM (%zu bytes), %d CaSd",
                      pcm_entries,
                      pcm_bytes,
                      casd_deleted);
        SetStatus(status);
    }

    void DrawEditConfirmDialogs()
    {
        if (m_confirm_clear_cache_open)
        {
            ImGui::OpenPopup("Clear Compression Cache");
        }
        if (ImGui::BeginPopupModal("Clear Compression Cache",
                                   &m_confirm_clear_cache_open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            const size_t entries = m_app_compression_cache.size();
            const size_t bytes = AppCompressionCacheBytes();
            ImGui::TextUnformatted("Clear the app compression preview cache?");
            ImGui::TextDisabled("Session PCM / CaSd masters are not removed.");
            ImGui::Separator();
            ImGui::Text("%zu preview(s), %zu bytes", entries, bytes);
            ImGui::Separator();
            if (ImGui::Button("Clear", ImVec2(120.0f, 0.0f)))
            {
                PerformClearCompressionCache();
                m_confirm_clear_cache_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
            {
                m_confirm_clear_cache_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (m_confirm_delete_pcm_open)
        {
            ImGui::OpenPopup("Delete Uncompressed Originals");
        }
        if (ImGui::BeginPopupModal("Delete Uncompressed Originals",
                                   &m_confirm_delete_pcm_open,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            const size_t entries = m_session_pcm_cache.size();
            const size_t bytes = SessionPcmCacheBytes();
            const int casd = CountBankCasdResources();
            ImGui::TextUnformatted("Delete uncompressed PCM masters from this session?");
            ImGui::TextDisabled("Committed bank samples (esnd/csnd/snd) stay.");
            ImGui::TextDisabled("App compression preview cache is not cleared.");
            ImGui::Separator();
            ImGui::Text("%zu in-memory PCM (%zu bytes), %d CaSd in bank",
                        entries,
                        bytes,
                        casd);
            ImGui::Separator();
            const bool empty = (entries == 0 && casd == 0);
            if (empty)
            {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Delete", ImVec2(120.0f, 0.0f)))
            {
                PerformDeleteUncompressedOriginals();
                m_confirm_delete_pcm_open = false;
                ImGui::CloseCurrentPopup();
            }
            if (empty)
            {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
            {
                m_confirm_delete_pcm_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    int32_t BankResourceSize(XResourceType type, XLongResourceID id) const
    {
        if (!m_bank_token || id < 0)
        {
            return 0;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        int32_t size = 0;
        XPTR data = XGetFileResource(bank, type, id, nullptr, &size);
        if (data)
        {
            XDisposePtr(data);
            return size > 0 ? size : 0;
        }
        return 0;
    }

    void OpenResourceUsageForInstrument(int instrument_list_index)
    {
        m_resource_usage_lines.clear();
        m_resource_usage_title = "Resource Usage — Instrument";
        if (instrument_list_index < 0 ||
            instrument_list_index >= static_cast<int>(m_instruments.size()))
        {
            m_resource_usage_open = true;
            m_resource_usage_lines.push_back("(no instrument selected)");
            return;
        }
        const InstrumentRow &inst = m_instruments[static_cast<size_t>(instrument_list_index)];
        char header[256];
        std::snprintf(header,
                      sizeof(header),
                      "B%dP%03d  %s  (instID %u)",
                      inst.bank,
                      inst.program,
                      inst.name.c_str(),
                      inst.inst_id);
        m_resource_usage_lines.push_back(header);

        const int32_t inst_size =
            BankResourceSize(ID_INST, static_cast<XLongResourceID>(inst.inst_id));
        {
            char line[128];
            std::snprintf(line, sizeof(line), "INST %u: %d bytes", inst.inst_id, (int)inst_size);
            m_resource_usage_lines.push_back(line);
        }

        if (m_bank_token && !inst.from_song_document)
        {
            uint32_t split_count = 0;
            if (BAERmfEditorBank_GetInstrumentSampleCount(m_bank_token,
                                                          inst.instrument_index,
                                                          &split_count) == BAE_NO_ERROR)
            {
                int64_t snd_total = 0;
                for (uint32_t s = 0; s < split_count; ++s)
                {
                    BAERmfEditorBankSampleInfo info;
                    std::memset(&info, 0, sizeof(info));
                    if (BAERmfEditorBank_GetInstrumentSampleInfo(m_bank_token,
                                                                 inst.instrument_index,
                                                                 s,
                                                                 &info) != BAE_NO_ERROR)
                    {
                        continue;
                    }
                    const uint32_t snd_id = static_cast<uint32_t>(info.sndResourceID);
                    int32_t sz = BankResourceSize(ID_ESND, static_cast<XLongResourceID>(snd_id));
                    if (sz <= 0)
                    {
                        sz = BankResourceSize(ID_CSND, static_cast<XLongResourceID>(snd_id));
                    }
                    if (sz <= 0)
                    {
                        sz = BankResourceSize(ID_SND, static_cast<XLongResourceID>(snd_id));
                    }
                    snd_total += sz;
                    char line[160];
                    std::snprintf(line,
                                  sizeof(line),
                                  "  split %u → SND %u: %d bytes",
                                  s,
                                  snd_id,
                                  (int)sz);
                    m_resource_usage_lines.push_back(line);
                    auto cache_it = m_app_compression_cache.find(snd_id);
                    if (cache_it != m_app_compression_cache.end())
                    {
                        char cline[128];
                        std::snprintf(cline,
                                      sizeof(cline),
                                      "    app compression cache: %zu bytes",
                                      cache_it->second.size());
                        m_resource_usage_lines.push_back(cline);
                    }
                    auto pcm_it = m_session_pcm_cache.find(snd_id);
                    if (pcm_it != m_session_pcm_cache.end())
                    {
                        char pline[128];
                        std::snprintf(pline,
                                      sizeof(pline),
                                      "    session PCM master: %zu bytes",
                                      pcm_it->second.pcm.size());
                        m_resource_usage_lines.push_back(pline);
                    }
                }
                char tot[128];
                std::snprintf(tot,
                              sizeof(tot),
                              "Sample data total: %lld bytes",
                              static_cast<long long>(snd_total));
                m_resource_usage_lines.push_back(tot);
            }
        }
        else if (inst.from_song_document)
        {
            m_resource_usage_lines.push_back("(song-document instrument — sizes via Samples tab)");
        }
        m_resource_usage_open = true;
    }

    void OpenResourceUsageForSample(int sample_list_index)
    {
        m_resource_usage_lines.clear();
        m_resource_usage_title = "Resource Usage — Sample";
        if (sample_list_index < 0 || sample_list_index >= static_cast<int>(m_samples.size()))
        {
            m_resource_usage_open = true;
            m_resource_usage_lines.push_back("(no sample selected)");
            return;
        }
        const SampleRow &sample = m_samples[static_cast<size_t>(sample_list_index)];
        char header[256];
        if (sample.unassigned)
        {
            std::snprintf(header,
                          sizeof(header),
                          "[Unassigned] %s  SND %u",
                          sample.name.c_str(),
                          sample.snd_resource_id);
        }
        else
        {
            std::snprintf(header,
                          sizeof(header),
                          "%s B%dP%03d  %s  SND %u",
                          sample.source == SampleSource::Song ? "[Song]" : "[Bank]",
                          sample.bank,
                          sample.program,
                          sample.name.c_str(),
                          sample.snd_resource_id);
        }
        m_resource_usage_lines.push_back(header);

        if (sample.source == SampleSource::Song && m_document)
        {
            BAERmfEditorSampleInfo info;
            std::memset(&info, 0, sizeof(info));
            if (BAERmfEditorDocument_GetSampleInfo(m_document,
                                                   sample.document_sample_index,
                                                   &info) == BAE_NO_ERROR)
            {
                char line[128];
                std::snprintf(line,
                              sizeof(line),
                              "Document sample size: %u bytes, frames %u",
                              static_cast<unsigned>(info.sampleSize),
                              static_cast<unsigned>(info.sampleInfo.waveFrames));
                m_resource_usage_lines.push_back(line);
            }
        }
        else if (sample.snd_resource_id != kNoSndResourceId)
        {
            const XLongResourceID sid = static_cast<XLongResourceID>(sample.snd_resource_id);
            int32_t sz = BankResourceSize(ID_ESND, sid);
            const char *stype = "esnd";
            if (sz <= 0)
            {
                sz = BankResourceSize(ID_CSND, sid);
                stype = "csnd";
            }
            if (sz <= 0)
            {
                sz = BankResourceSize(ID_SND, sid);
                stype = "snd ";
            }
            char line[128];
            std::snprintf(line, sizeof(line), "%s %u: %d bytes", stype, sample.snd_resource_id, (int)sz);
            m_resource_usage_lines.push_back(line);

            if (sample.unassigned)
            {
                m_resource_usage_lines.push_back("Referenced by: (none — unassigned)");
            }
            else
            {
                char ref[128];
                std::snprintf(ref,
                              sizeof(ref),
                              "Referenced by: B%dP%03d (instrument index %u, split %u)",
                              sample.bank,
                              sample.program,
                              sample.instrument_index,
                              sample.split_index);
                m_resource_usage_lines.push_back(ref);
            }

            auto cache_it = m_app_compression_cache.find(sample.snd_resource_id);
            if (cache_it != m_app_compression_cache.end())
            {
                char cline[128];
                std::snprintf(cline,
                              sizeof(cline),
                              "App compression cache: %zu bytes",
                              cache_it->second.size());
                m_resource_usage_lines.push_back(cline);
            }
            auto pcm_it = m_session_pcm_cache.find(sample.snd_resource_id);
            if (pcm_it != m_session_pcm_cache.end())
            {
                char pline[128];
                std::snprintf(pline,
                              sizeof(pline),
                              "Session PCM master: %zu bytes",
                              pcm_it->second.pcm.size());
                m_resource_usage_lines.push_back(pline);
            }
            const int32_t casd = BankResourceSize(kCasdType(), sid);
            if (casd > 0)
            {
                char cline[128];
                std::snprintf(cline, sizeof(cline), "CaSd %u: %d bytes", sample.snd_resource_id, (int)casd);
                m_resource_usage_lines.push_back(cline);
            }
        }
        m_resource_usage_open = true;
    }

    void OpenResourceUsageForSong(int song_index)
    {
        m_resource_usage_lines.clear();
        m_resource_usage_title = "Resource Usage — Song";
        if (song_index >= 0 && song_index < static_cast<int>(m_session_songs.size()))
        {
            const SessionSongEntry &entry = m_session_songs[static_cast<size_t>(song_index)];
            char header[256];
            std::snprintf(header, sizeof(header), "%s  [session song]", entry.name.c_str());
            m_resource_usage_lines.push_back(header);
            {
                char line[128];
                std::snprintf(line,
                              sizeof(line),
                              "Midi resource %u: %zu bytes (in memory)",
                              entry.midi_resource_id,
                              entry.midi_bytes.size());
                m_resource_usage_lines.push_back(line);
            }
            if (entry.midi_resource_id != 0)
            {
                const int32_t mid =
                    BankResourceSize(ID_MIDI, static_cast<XLongResourceID>(entry.midi_resource_id));
                if (mid > 0)
                {
                    char line[128];
                    std::snprintf(line, sizeof(line), "Midi in bank: %d bytes", (int)mid);
                    m_resource_usage_lines.push_back(line);
                }
            }
            if (entry.song_resource_id != 0 || song_index == 0)
            {
                const int32_t song_sz =
                    BankResourceSize(ID_SONG, static_cast<XLongResourceID>(entry.song_resource_id));
                char line[128];
                std::snprintf(line,
                              sizeof(line),
                              "SONG %u in bank: %d bytes",
                              entry.song_resource_id,
                              (int)song_sz);
                m_resource_usage_lines.push_back(line);
            }
        }
        else if (m_document)
        {
            char header[256];
            std::snprintf(header,
                          sizeof(header),
                          "%s  [%s]",
                          m_song_row.name.empty() ? "(untitled)" : m_song_row.name.c_str(),
                          m_song_row.source_type.empty() ? "?" : m_song_row.source_type.c_str());
            m_resource_usage_lines.push_back(header);
            uint32_t sample_count = 0;
            (void)BAERmfEditorDocument_GetSampleCount(m_document, &sample_count);
            char line[128];
            std::snprintf(line, sizeof(line), "Document samples: %u", sample_count);
            m_resource_usage_lines.push_back(line);
        }
        else
        {
            m_resource_usage_lines.push_back("(no song)");
        }
        m_resource_usage_open = true;
    }

    void DrawResourceUsageDialog()
    {
        if (m_resource_usage_open)
        {
            ImGui::OpenPopup("Resource Usage");
        }
        if (!ImGui::BeginPopupModal("Resource Usage",
                                    &m_resource_usage_open,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::TextUnformatted(m_resource_usage_title.c_str());
        ImGui::Separator();
        ImGui::BeginChild("##ResourceUsageBody", ImVec2(420.0f, 220.0f), true);
        for (const std::string &line : m_resource_usage_lines)
        {
            ImGui::TextUnformatted(line.c_str());
        }
        ImGui::EndChild();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)))
        {
            m_resource_usage_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
