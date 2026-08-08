/* Karaoke lyric editor — included inside NbEditorApp. */

    struct LyFragment
    {
        std::string text;
        uint32_t tick = 0;
        bool synced = false;
        bool starts_new_line = false;
        int line_index = 0;
    };

#if USE_LIB_HYPHEN == TRUE
#include "nbeditor_hyphen.inc"
#endif

    static void LyAppendParsedMetaText(std::vector<LyFragment> *out,
                                       uint32_t tick,
                                       const unsigned char *data,
                                       uint32_t size,
                                       bool *pending_newline)
    {
        if (!out || !pending_newline)
        {
            return;
        }
        if (!data || size == 0)
        {
            *pending_newline = true;
            return;
        }

        uint32_t i = 0;
        if (data[0] == '/' || data[0] == '\\')
        {
            *pending_newline = true;
            i = 1;
        }

        std::string seg;
        auto flush_seg = [&]() {
            if (seg.empty())
            {
                return;
            }
            LyFragment frag;
            frag.text = seg;
            frag.tick = tick;
            frag.synced = true;
            frag.starts_new_line = *pending_newline && !out->empty();
            *pending_newline = false;
            out->push_back(std::move(frag));
            seg.clear();
        };

        for (; i < size; ++i)
        {
            const char c = static_cast<char>(data[i]);
            if (c == '/' || c == '\\' || c == '\r' || c == '\n')
            {
                flush_seg();
                *pending_newline = true;
                continue;
            }
            seg.push_back(c);
        }
        flush_seg();
    }

    static void LyNormalizeFragments(std::vector<LyFragment> *frags)
    {
        if (!frags)
        {
            return;
        }
        /* Drop empty placeholders except those that only mark newlines — merge into next. */
        std::vector<LyFragment> cleaned;
        cleaned.reserve(frags->size());
        bool force_next_nl = false;
        for (const LyFragment &f : *frags)
        {
            if (f.text.empty())
            {
                force_next_nl = true;
                continue;
            }
            LyFragment copy = f;
            if (force_next_nl)
            {
                copy.starts_new_line = true;
                force_next_nl = false;
            }
            cleaned.push_back(std::move(copy));
        }
        int line = 0;
        for (size_t i = 0; i < cleaned.size(); ++i)
        {
            if (i == 0)
            {
                cleaned[i].starts_new_line = false;
                cleaned[i].line_index = 0;
                continue;
            }
            if (cleaned[i].starts_new_line)
            {
                ++line;
            }
            cleaned[i].line_index = line;
        }
        *frags = std::move(cleaned);
    }

    void OpenLyricsEditor()
    {
        m_lyrics_open = true;
        if (m_document)
        {
            LyEnsureTrack();
            LyReloadFromDocument();
        }
        SetStatus("Lyrics editor open");
    }

    int LyFindBestLyricTrack() const
    {
        if (!m_document)
        {
            return -1;
        }
        uint16_t track_count = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &track_count) != BAE_NO_ERROR ||
            track_count == 0)
        {
            return -1;
        }
        int best = -1;
        uint32_t best_count = 0;
        for (uint16_t t = 0; t < track_count; ++t)
        {
            uint32_t lyric_count = 0;
            BAERmfEditorDocument_GetLyricEventCount(m_document, t, &lyric_count);
            if (lyric_count > best_count)
            {
                best_count = lyric_count;
                best = static_cast<int>(t);
            }
        }
        if (best >= 0)
        {
            return best;
        }
        /* Fallback: track with KAR-style 0x01 text. */
        for (uint16_t t = 0; t < track_count; ++t)
        {
            uint32_t meta_count = 0;
            BAERmfEditorDocument_GetTrackMetaEventCount(m_document, t, 0x01, &meta_count);
            uint32_t usable = 0;
            for (uint32_t i = 0; i < meta_count; ++i)
            {
                uint32_t tick = 0;
                unsigned char type = 0;
                unsigned char data[8] = {};
                uint32_t size = 0;
                if (BAERmfEditorDocument_GetTrackMetaEvent(m_document,
                                                          t,
                                                          0x01,
                                                          i,
                                                          &tick,
                                                          &type,
                                                          data,
                                                          sizeof(data),
                                                          &size) != BAE_NO_ERROR)
                {
                    continue;
                }
                if (size > 0 && data[0] == '@')
                {
                    continue;
                }
                ++usable;
            }
            if (usable > best_count)
            {
                best_count = usable;
                best = static_cast<int>(t);
            }
        }
        if (best >= 0)
        {
            return best;
        }
        return m_midi_selected_track >= 0 ? m_midi_selected_track : 0;
    }

    void LyEnsureTrack()
    {
        if (!m_document)
        {
            m_lyrics_track = -1;
            return;
        }
        if (m_lyrics_track < 0)
        {
            m_lyrics_track = LyFindBestLyricTrack();
        }
        uint16_t track_count = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &track_count);
        if (m_lyrics_track < 0 || m_lyrics_track >= static_cast<int>(track_count))
        {
            m_lyrics_track = track_count > 0 ? 0 : -1;
        }
    }

    void LyCreateLyricsTrack()
    {
        if (!m_document)
        {
            return;
        }
        MeBeginUndo();
        BAERmfEditorTrackSetup setup{};
        setup.channel = 0;
        setup.bank = 0;
        setup.program = 0;
        char name_buf[] = "Lyrics";
        setup.name = name_buf;
        uint16_t out_index = 0;
        if (BAERmfEditorDocument_AddTrack(m_document, &setup, &out_index) == BAE_NO_ERROR)
        {
            m_lyrics_track = static_cast<int>(out_index);
            m_midi_selected_track = m_lyrics_track;
            MeEnsureTrackMuteSize();
            MeCommitUndo();
            LyReloadFromDocument();
            SetStatus("Created Lyrics track");
        }
        else
        {
            MeCancelUndo();
        }
    }

    void LyReloadFromDocument()
    {
        m_lyrics_fragments.clear();
        m_lyrics_selected.clear();
        m_lyrics_text_buf[0] = 0;
        m_lyrics_dirty = false;
        m_lyrics_sync_cursor = 0;
        if (!m_document)
        {
            return;
        }
        LyEnsureTrack();
        if (m_lyrics_track < 0)
        {
            return;
        }
        const uint16_t track = static_cast<uint16_t>(m_lyrics_track);
        uint32_t lyric_count = 0;
        BAERmfEditorDocument_GetLyricEventCount(m_document, track, &lyric_count);

        bool pending_newline = false;
        if (lyric_count > 0)
        {
            for (uint32_t i = 0; i < lyric_count; ++i)
            {
                uint32_t tick = 0;
                unsigned char type = 0;
                unsigned char data[512] = {};
                uint32_t size = 0;
                if (BAERmfEditorDocument_GetTrackMetaEvent(m_document,
                                                          track,
                                                          0x05,
                                                          i,
                                                          &tick,
                                                          &type,
                                                          data,
                                                          sizeof(data),
                                                          &size) != BAE_NO_ERROR)
                {
                    continue;
                }
                if (size > sizeof(data))
                {
                    size = sizeof(data);
                }
                LyAppendParsedMetaText(
                    &m_lyrics_fragments, tick, data, size, &pending_newline);
            }
        }
        else
        {
            /* KAR-style 0x01 fallback when no true lyrics. */
            uint32_t meta_count = 0;
            BAERmfEditorDocument_GetTrackMetaEventCount(m_document, track, 0x01, &meta_count);
            for (uint32_t i = 0; i < meta_count; ++i)
            {
                uint32_t tick = 0;
                unsigned char type = 0;
                unsigned char data[512] = {};
                uint32_t size = 0;
                if (BAERmfEditorDocument_GetTrackMetaEvent(m_document,
                                                          track,
                                                          0x01,
                                                          i,
                                                          &tick,
                                                          &type,
                                                          data,
                                                          sizeof(data),
                                                          &size) != BAE_NO_ERROR)
                {
                    continue;
                }
                if (size > 0 && data[0] == '@')
                {
                    continue;
                }
                if (size > sizeof(data))
                {
                    size = sizeof(data);
                }
                LyAppendParsedMetaText(
                    &m_lyrics_fragments, tick, data, size, &pending_newline);
            }
        }
        LyNormalizeFragments(&m_lyrics_fragments);
        LyRebuildTextBufferFromFragments();
        m_lyrics_sync_cursor = LyNextUnsyncedIndex();
    }

    void LyRebuildTextBufferFromFragments()
    {
        std::string text;
        for (size_t i = 0; i < m_lyrics_fragments.size(); ++i)
        {
            const LyFragment &f = m_lyrics_fragments[i];
            if (i > 0 && f.starts_new_line)
            {
                text.push_back('\n');
            }
            text += f.text;
        }
        if (text.size() >= sizeof(m_lyrics_text_buf))
        {
            text.resize(sizeof(m_lyrics_text_buf) - 1);
        }
        std::memcpy(m_lyrics_text_buf, text.c_str(), text.size() + 1);
    }

    int LyNextUnsyncedIndex() const
    {
        for (int i = 0; i < static_cast<int>(m_lyrics_fragments.size()); ++i)
        {
            if (!m_lyrics_fragments[static_cast<size_t>(i)].synced)
            {
                return i;
            }
        }
        return static_cast<int>(m_lyrics_fragments.size());
    }

    int LySyncedCount() const
    {
        int n = 0;
        for (const LyFragment &f : m_lyrics_fragments)
        {
            if (f.synced)
            {
                ++n;
            }
        }
        return n;
    }

    void LyApplyTextPreserveTimings()
    {
        /* Split text into words (or syllables); rematch synced ticks by order. */
        std::vector<LyFragment> old = m_lyrics_fragments;
        std::vector<LyFragment> neu;
        const char *p = m_lyrics_text_buf;
        int line = 0;
        bool at_line_start = true;
#if USE_LIB_HYPHEN == TRUE
        if (!m_hyph_langs_scanned)
        {
            LyHyphRescanLanguages();
        }
#endif
        while (*p)
        {
            while (*p == '\r')
            {
                ++p;
            }
            if (*p == '\n')
            {
                ++line;
                at_line_start = true;
                ++p;
                continue;
            }
            /* Collect whitespace-prefixed word. */
            std::string word;
            bool lead_space = false;
            if (!at_line_start && (*p == ' ' || *p == '\t'))
            {
                lead_space = true;
                while (*p == ' ' || *p == '\t')
                {
                    word.push_back(*p++);
                }
            }
            else
            {
                while (*p == ' ' || *p == '\t')
                {
                    ++p;
                }
            }
            while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t')
            {
                word.push_back(*p++);
            }
            if (word.empty())
            {
                continue;
            }
            const bool starts_nl = at_line_start && !neu.empty();
#if USE_LIB_HYPHEN == TRUE
            if (m_lyrics_split_mode == LySplit_Syllables)
            {
                LySyllabifyWord(word, lead_space, starts_nl, line, &neu);
            }
            else
#endif
            {
                LyFragment frag;
                frag.text = word;
                frag.starts_new_line = starts_nl;
                frag.line_index = line;
                frag.synced = false;
                frag.tick = 0;
                neu.push_back(std::move(frag));
            }
            at_line_start = false;
        }
        /* Rematch timings by order. */
        const size_t n = std::min(old.size(), neu.size());
        for (size_t i = 0; i < n; ++i)
        {
            if (old[i].synced)
            {
                neu[i].tick = old[i].tick;
                neu[i].synced = true;
            }
        }
        /* Fix line_index / starts_new_line consistency. */
        LyNormalizeFragments(&neu);
        /* Preserve rematched sync after normalize (normalize may reorder flags). */
        for (size_t i = 0; i < neu.size() && i < old.size(); ++i)
        {
            if (old[i].synced && i < n)
            {
                neu[i].tick = old[i].tick;
                neu[i].synced = true;
            }
        }
        m_lyrics_fragments = std::move(neu);
        m_lyrics_selected.clear();
        m_lyrics_sync_cursor = LyNextUnsyncedIndex();
        m_lyrics_dirty = true;
    }

    void LyBuildSyncedApiFragments(std::vector<BAERmfEditorLyricFragment> *api,
                                    std::vector<std::string> *text_storage) const
    {
        if (!api || !text_storage)
        {
            return;
        }
        api->clear();
        text_storage->clear();
        api->reserve(m_lyrics_fragments.size());
        text_storage->reserve(m_lyrics_fragments.size());
        int prev_line = -1;
        for (const LyFragment &f : m_lyrics_fragments)
        {
            if (!f.synced)
            {
                continue;
            }
            text_storage->push_back(f.text);
            BAERmfEditorLyricFragment lf{};
            lf.tick = f.tick;
            lf.text = text_storage->back().c_str();
            lf.startsNewLine =
                (prev_line >= 0 && f.line_index != prev_line) ? TRUE : FALSE;
            prev_line = f.line_index;
            api->push_back(lf);
        }
    }

    bool LyCommitToDocument(bool /*reload_playback*/)
    {
        if (!m_document || m_lyrics_track < 0)
        {
            return false;
        }
        /* Only write synced fragments so unsynced words do not fire at tick 0
         * while tap-to-sync is in progress. Text for unsynced words stays in
         * the editor buffer / fragment list until stamped. */
        std::vector<BAERmfEditorLyricFragment> api;
        std::vector<std::string> text_storage;
        LyBuildSyncedApiFragments(&api, &text_storage);
        MeBeginUndo();
        const BAEResult result = BAERmfEditorDocument_ReplaceTrackLyrics(
            m_document,
            static_cast<uint16_t>(m_lyrics_track),
            api.empty() ? nullptr : api.data(),
            static_cast<uint32_t>(api.size()));
        if (result != BAE_NO_ERROR)
        {
            MeCancelUndo();
            SetStatus(std::string("Lyric commit failed: ") + FormatBAEError(result));
            return false;
        }
        MeCommitUndo();
        m_lyrics_dirty = false;
        LyWirePreviewCallback();
        SetStatus("Lyrics committed");
        return true;
    }

    void LyStampNextAtPlayhead()
    {
        if (!m_document || m_lyrics_track < 0)
        {
            return;
        }
        const int idx = LyNextUnsyncedIndex();
        if (idx >= static_cast<int>(m_lyrics_fragments.size()))
        {
            SetStatus("All lyric fragments synced");
            return;
        }
        uint32_t tick = MePlayheadTick();
        if (m_midi_snap_enabled)
        {
            tick = MeSnapTick(tick);
        }
        m_lyrics_fragments[static_cast<size_t>(idx)].tick = tick;
        m_lyrics_fragments[static_cast<size_t>(idx)].synced = true;
        m_lyrics_sync_cursor = LyNextUnsyncedIndex();
        m_lyrics_selected.clear();
        m_lyrics_selected.insert(idx);
        LyCommitToDocument(true);
    }

    void LyClearLastStamp()
    {
        for (int i = static_cast<int>(m_lyrics_fragments.size()) - 1; i >= 0; --i)
        {
            if (m_lyrics_fragments[static_cast<size_t>(i)].synced)
            {
                m_lyrics_fragments[static_cast<size_t>(i)].synced = false;
                m_lyrics_fragments[static_cast<size_t>(i)].tick = 0;
                m_lyrics_sync_cursor = i;
                m_lyrics_selected.clear();
                m_lyrics_selected.insert(i);
                LyCommitToDocument(true);
                return;
            }
        }
    }

    void LyClearAllTimings()
    {
        if (m_lyrics_fragments.empty())
        {
            return;
        }
        for (LyFragment &f : m_lyrics_fragments)
        {
            f.synced = false;
            f.tick = 0;
        }
        m_lyrics_sync_cursor = 0;
        LyCommitToDocument(true);
    }

    void LyDeleteSelectedFragments()
    {
        if (m_lyrics_selected.empty())
        {
            return;
        }
        std::vector<int> indices(m_lyrics_selected.begin(), m_lyrics_selected.end());
        std::sort(indices.begin(), indices.end(), std::greater<int>());
        for (int idx : indices)
        {
            if (idx >= 0 && idx < static_cast<int>(m_lyrics_fragments.size()))
            {
                m_lyrics_fragments.erase(m_lyrics_fragments.begin() + idx);
            }
        }
        LyNormalizeFragments(&m_lyrics_fragments);
        LyRebuildTextBufferFromFragments();
        m_lyrics_selected.clear();
        m_lyrics_sync_cursor = LyNextUnsyncedIndex();
        LyCommitToDocument(true);
    }

    void LyQuantizeFragments(bool selection_only)
    {
        if (m_lyrics_fragments.empty())
        {
            return;
        }
        bool changed = false;
        for (int i = 0; i < static_cast<int>(m_lyrics_fragments.size()); ++i)
        {
            if (selection_only && m_lyrics_selected.find(i) == m_lyrics_selected.end())
            {
                continue;
            }
            LyFragment &f = m_lyrics_fragments[static_cast<size_t>(i)];
            if (!f.synced)
            {
                continue;
            }
            const uint32_t q = MeSnapTick(f.tick);
            if (q != f.tick)
            {
                f.tick = q;
                changed = true;
            }
        }
        if (changed)
        {
            LyCommitToDocument(true);
            SetStatus("Quantized lyrics");
        }
        else
        {
            SetStatus("Lyrics already on grid");
        }
    }

    void LyNudgeSelected(int32_t tick_delta)
    {
        if (m_lyrics_selected.empty() || tick_delta == 0)
        {
            return;
        }
        for (int idx : m_lyrics_selected)
        {
            if (idx < 0 || idx >= static_cast<int>(m_lyrics_fragments.size()))
            {
                continue;
            }
            LyFragment &f = m_lyrics_fragments[static_cast<size_t>(idx)];
            if (!f.synced)
            {
                continue;
            }
            const int32_t t = static_cast<int32_t>(f.tick) + tick_delta;
            f.tick = static_cast<uint32_t>(std::max(0, t));
        }
        LyCommitToDocument(true);
    }

#if SUPPORT_KARAOKE == TRUE
    static void LyPreviewCallbackThunk(struct GM_Song * /*song*/,
                                       const char *lyric,
                                       uint32_t /*t_us*/,
                                       void *ref)
    {
        auto *self = static_cast<NbEditorApp *>(ref);
        if (!self)
        {
            return;
        }
        self->LyOnPreviewLyric(lyric);
    }
#endif

    void LyOnPreviewLyric(const char *lyric)
    {
        if (!lyric)
        {
            return;
        }
        if (lyric[0] == '\0' || lyric[0] == '/' || lyric[0] == '\\')
        {
            if (m_lyrics_preview_current[0])
            {
                std::snprintf(m_lyrics_preview_previous,
                              sizeof(m_lyrics_preview_previous),
                              "%s",
                              m_lyrics_preview_current);
            }
            m_lyrics_preview_current[0] = 0;
            m_lyrics_preview_current_len = 0;
            if (lyric[0] == '/' || lyric[0] == '\\')
            {
                const char *rest = lyric + 1;
                if (*rest)
                {
                    LyPreviewAddFragment(rest);
                }
            }
            return;
        }
        /* Embedded delimiters */
        const char *p = lyric;
        const char *seg = p;
        while (true)
        {
            if (*p == '/' || *p == '\\' || *p == '\0')
            {
                if (p > seg)
                {
                    char tmp[192];
                    size_t len = static_cast<size_t>(p - seg);
                    if (len >= sizeof(tmp))
                    {
                        len = sizeof(tmp) - 1;
                    }
                    std::memcpy(tmp, seg, len);
                    tmp[len] = 0;
                    LyPreviewAddFragment(tmp);
                }
                if (*p == '/' || *p == '\\')
                {
                    if (m_lyrics_preview_current[0])
                    {
                        std::snprintf(m_lyrics_preview_previous,
                                      sizeof(m_lyrics_preview_previous),
                                      "%s",
                                      m_lyrics_preview_current);
                    }
                    m_lyrics_preview_current[0] = 0;
                    m_lyrics_preview_current_len = 0;
                    ++p;
                    seg = p;
                    continue;
                }
                break;
            }
            ++p;
        }
    }

    void LyPreviewAddFragment(const char *frag)
    {
        if (!frag || !frag[0])
        {
            return;
        }
        const size_t frag_len = std::strlen(frag);
        const size_t space =
            sizeof(m_lyrics_preview_current) - 1 - m_lyrics_preview_current_len;
        const size_t copy = frag_len < space ? frag_len : space;
        if (copy > 0)
        {
            std::memcpy(m_lyrics_preview_current + m_lyrics_preview_current_len, frag, copy);
            m_lyrics_preview_current_len += copy;
            m_lyrics_preview_current[m_lyrics_preview_current_len] = 0;
        }
    }

    void LyResetPreview()
    {
        m_lyrics_preview_current[0] = 0;
        m_lyrics_preview_previous[0] = 0;
        m_lyrics_preview_current_len = 0;
#if SUPPORT_KARAOKE == TRUE
        if (m_song)
        {
            (void)BAESong_ResetLyricState(m_song);
        }
#endif
    }

    void LyWirePreviewCallback()
    {
#if SUPPORT_KARAOKE == TRUE
        if (m_song && (m_lyrics_open || m_midi_editor_open))
        {
            (void)BAESong_SetLyricCallback(m_song, LyPreviewCallbackThunk, this);
        }
#endif
    }

    void LyDrawPreviewStrip()
    {
        ImGui::BeginChild("##ly_preview",
                          ImVec2(0.0f, 52.0f),
                          true,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::TextDisabled("Preview");
        ImGui::TextWrapped("%s",
                           m_lyrics_preview_previous[0] ? m_lyrics_preview_previous : " ");
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f),
                           "%s",
                           m_lyrics_preview_current[0] ? m_lyrics_preview_current : "—");
        ImGui::EndChild();
    }

    void LyHandleTapShortcuts(bool allow)
    {
        if (!allow || !m_lyrics_tap_sync || !m_document)
        {
            return;
        }
        const ImGuiIO &io = ImGui::GetIO();
        if (io.WantTextInput)
        {
            return;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
        {
            LyStampNextAtPlayhead();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
        {
            LyClearLastStamp();
        }
    }

    void LyDrawLane(float width, float height, float key_w)
    {
        if (!m_midi_lyrics_lane_visible || !m_document || m_lyrics_track < 0)
        {
            return;
        }
        if (m_lyrics_fragments.empty() && m_lyrics_open == false)
        {
            /* Keep lane usable; reload if editor not open yet. */
            LyEnsureTrack();
            if (m_lyrics_fragments.empty())
            {
                LyReloadFromDocument();
            }
        }

        ImGui::BeginChild("##me_lyrics_lane", ImVec2(width, height), true,
                          ImGuiWindowFlags_NoScrollbar);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const float plot_x = origin.x + key_w;
        const float plot_w = std::max(1.0f, size.x - key_w);

        dl->AddRectFilled(origin,
                          ImVec2(origin.x + size.x, origin.y + size.y),
                          IM_COL32(24, 28, 34, 255));
        dl->AddText(ImVec2(origin.x + 4.0f, origin.y + 2.0f),
                    IM_COL32(220, 190, 90, 255),
                    "Lyrics");

        /* Beat grid */
        {
            const uint16_t tpq = MeGetTpq();
            const uint32_t view0 =
                static_cast<uint32_t>(std::max(0.0f, m_midi_scroll_tick));
            const uint32_t view1 = MeXToTick(plot_x + plot_w, plot_x);
            for (uint32_t t = (view0 / tpq) * tpq; t <= view1; t += tpq)
            {
                const float x = MeTickToX(t, plot_x);
                dl->AddLine(ImVec2(x, origin.y + 16.0f),
                            ImVec2(x, origin.y + size.y - 2.0f),
                            IM_COL32(50, 54, 64, 180));
            }
        }

        const float chip_y = origin.y + 18.0f;
        const float chip_h = std::max(16.0f, size.y - 22.0f);
        int hit = -1;
        for (int i = 0; i < static_cast<int>(m_lyrics_fragments.size()); ++i)
        {
            const LyFragment &f = m_lyrics_fragments[static_cast<size_t>(i)];
            if (!f.synced)
            {
                continue;
            }
            const float x = MeTickToX(f.tick, plot_x);
            if (x < plot_x - 40.0f || x > plot_x + plot_w + 40.0f)
            {
                continue;
            }
            char label[64];
            std::snprintf(label, sizeof(label), "%s", f.text.c_str());
            const ImVec2 ts = ImGui::CalcTextSize(label);
            const float chip_w = std::max(18.0f, ts.x + 10.0f);
            const ImVec2 c0(x, chip_y);
            const ImVec2 c1(x + chip_w, chip_y + chip_h);
            const bool selected = m_lyrics_selected.count(i) > 0;
            const ImU32 fill = selected ? IM_COL32(210, 160, 50, 220)
                                        : (f.starts_new_line ? IM_COL32(90, 120, 170, 210)
                                                             : IM_COL32(70, 100, 150, 200));
            dl->AddRectFilled(c0, c1, fill, 3.0f);
            dl->AddRect(c0, c1, IM_COL32(230, 210, 120, 255), 3.0f);
            dl->AddText(ImVec2(c0.x + 4.0f, c0.y + 2.0f), IM_COL32(245, 245, 245, 255), label);

            if (ImGui::IsMouseHoveringRect(c0, c1))
            {
                hit = i;
            }
        }

        /* Playhead */
        {
            const float px = MeTickToX(MePlayheadTick(), plot_x);
            dl->AddLine(ImVec2(px, origin.y),
                        ImVec2(px, origin.y + size.y),
                        IM_COL32(255, 80, 80, 220),
                        1.5f);
        }

        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("##ly_lane_hit", size);
        const bool lane_hovered = ImGui::IsItemHovered();
        const ImGuiIO &io = ImGui::GetIO();

        if (lane_hovered && io.KeyCtrl && io.MouseWheel != 0.0f)
        {
            MeZoomTimeAt(io.MouseWheel, io.MousePos.x, plot_x, plot_w);
        }
        else if (lane_hovered && io.MouseWheelH != 0.0f)
        {
            MeScrollTimeByWheel(io.MouseWheelH, plot_w);
        }
        else if (lane_hovered && io.KeyShift && io.MouseWheel != 0.0f)
        {
            MeScrollTimeByWheel(io.MouseWheel, plot_w);
        }

        if (lane_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (hit >= 0)
            {
                if (!io.KeyCtrl)
                {
                    m_lyrics_selected.clear();
                }
                m_lyrics_selected.insert(hit);
                m_lyrics_drag_active = true;
                m_lyrics_drag_origin_tick = m_lyrics_fragments[static_cast<size_t>(hit)].tick;
                m_lyrics_drag_last_delta = 0;
                MeBeginUndo();
                m_lyrics_drag_undo_open = true;
            }
            else if (!io.KeyCtrl)
            {
                m_lyrics_selected.clear();
            }
        }

        if (m_lyrics_drag_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const uint32_t cur = MeXToTick(io.MousePos.x, plot_x);
            int32_t delta =
                static_cast<int32_t>(cur) - static_cast<int32_t>(m_lyrics_drag_origin_tick);
            if (m_midi_snap_enabled)
            {
                const int32_t snap = static_cast<int32_t>(MeSnapTicks());
                if (snap > 0)
                {
                    delta = (delta / snap) * snap;
                }
            }
            if (delta != m_lyrics_drag_last_delta)
            {
                const int32_t step = delta - m_lyrics_drag_last_delta;
                for (int idx : m_lyrics_selected)
                {
                    if (idx < 0 || idx >= static_cast<int>(m_lyrics_fragments.size()))
                    {
                        continue;
                    }
                    LyFragment &f = m_lyrics_fragments[static_cast<size_t>(idx)];
                    if (!f.synced)
                    {
                        continue;
                    }
                    const int32_t t = static_cast<int32_t>(f.tick) + step;
                    f.tick = static_cast<uint32_t>(std::max(0, t));
                }
                m_lyrics_drag_last_delta = delta;
            }
        }

        if (m_lyrics_drag_active && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            m_lyrics_drag_active = false;
            if (m_lyrics_drag_undo_open)
            {
                /* Commit via Replace without nested MeBeginUndo. */
                m_lyrics_drag_undo_open = false;
                std::vector<BAERmfEditorLyricFragment> api;
                std::vector<std::string> text_storage;
                LyBuildSyncedApiFragments(&api, &text_storage);
                if (BAERmfEditorDocument_ReplaceTrackLyrics(
                        m_document,
                        static_cast<uint16_t>(m_lyrics_track),
                        api.empty() ? nullptr : api.data(),
                        static_cast<uint32_t>(api.size())) == BAE_NO_ERROR)
                {
                    MeCommitUndo();
                }
                else
                {
                    MeCancelUndo();
                }
            }
        }

        if (lane_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            ImGui::OpenPopup("##ly_lane_ctx");
        }
        if (ImGui::BeginPopup("##ly_lane_ctx"))
        {
            if (ImGui::MenuItem("Quantize selected", nullptr, false, !m_lyrics_selected.empty()))
            {
                LyQuantizeFragments(true);
            }
            if (ImGui::MenuItem("Quantize all synced"))
            {
                LyQuantizeFragments(false);
            }
            if (ImGui::MenuItem("Delete selected", nullptr, false, !m_lyrics_selected.empty()))
            {
                LyDeleteSelectedFragments();
            }
            if (ImGui::MenuItem("Seek playhead to selection",
                                nullptr,
                                false,
                                !m_lyrics_selected.empty()))
            {
                const int idx = *m_lyrics_selected.begin();
                if (idx >= 0 && idx < static_cast<int>(m_lyrics_fragments.size()) &&
                    m_lyrics_fragments[static_cast<size_t>(idx)].synced)
                {
                    MeSeekToTick(m_lyrics_fragments[static_cast<size_t>(idx)].tick);
                }
            }
            if (ImGui::MenuItem("Open Lyrics editor…"))
            {
                OpenLyricsEditor();
            }
            ImGui::EndPopup();
        }

        ImGui::EndChild();
    }

    void DrawLyricsEditorWindow()
    {
        if (!m_lyrics_open)
        {
            return;
        }
        bool open = m_lyrics_open;
        ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Lyrics", &open))
        {
            ImGui::End();
            m_lyrics_open = open;
            return;
        }
        m_lyrics_open = open;
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        if (!m_document)
        {
            ImGui::TextDisabled("Load or activate a song to edit lyrics.");
            ImGui::End();
            return;
        }

        LyEnsureTrack();
        if (m_lyrics_fragments.empty() && m_lyrics_text_buf[0] == 0 && !m_lyrics_dirty)
        {
            LyReloadFromDocument();
        }

        /* Track picker */
        {
            uint16_t track_count = 0;
            BAERmfEditorDocument_GetTrackCount(m_document, &track_count);
            char track_preview[160] = "(none)";
            if (m_lyrics_track >= 0 && m_lyrics_track < static_cast<int>(track_count))
            {
                BAERmfEditorTrackInfo info{};
                uint32_t lc = 0;
                BAERmfEditorDocument_GetLyricEventCount(
                    m_document, static_cast<uint16_t>(m_lyrics_track), &lc);
                if (BAERmfEditorDocument_GetTrackInfo(
                        m_document, static_cast<uint16_t>(m_lyrics_track), &info) ==
                    BAE_NO_ERROR)
                {
                    std::snprintf(track_preview,
                                  sizeof(track_preview),
                                  "%u — %s (%u lyrics)",
                                  static_cast<unsigned>(m_lyrics_track + 1),
                                  (info.name && info.name[0]) ? info.name : "Track",
                                  static_cast<unsigned>(lc));
                }
                else
                {
                    std::snprintf(track_preview,
                                  sizeof(track_preview),
                                  "Track %u",
                                  static_cast<unsigned>(m_lyrics_track + 1));
                }
            }
            ImGui::SetNextItemWidth(280.0f);
            if (ImGui::BeginCombo("Track", track_preview))
            {
                for (uint16_t t = 0; t < track_count; ++t)
                {
                    BAERmfEditorTrackInfo info{};
                    char label[160];
                    if (BAERmfEditorDocument_GetTrackInfo(m_document, t, &info) == BAE_NO_ERROR)
                    {
                        uint32_t lc = 0;
                        BAERmfEditorDocument_GetLyricEventCount(m_document, t, &lc);
                        std::snprintf(label,
                                      sizeof(label),
                                      "%u — %s (%u lyrics)",
                                      static_cast<unsigned>(t + 1),
                                      (info.name && info.name[0]) ? info.name : "Track",
                                      static_cast<unsigned>(lc));
                    }
                    else
                    {
                        std::snprintf(label, sizeof(label), "Track %u", static_cast<unsigned>(t + 1));
                    }
                    const bool sel = (m_lyrics_track == static_cast<int>(t));
                    if (ImGui::Selectable(label, sel))
                    {
                        m_lyrics_track = static_cast<int>(t);
                        LyReloadFromDocument();
                    }
                    if (sel)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Create Lyrics track"))
            {
                LyCreateLyricsTrack();
            }
            ImGui::SameLine();
            if (ImGui::Button("Reload"))
            {
                LyReloadFromDocument();
            }
        }

        ImGui::Checkbox("Tap-to-sync", &m_lyrics_tap_sync);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "While enabled: Space/Enter stamps the playhead onto the next\n"
                "unsynced word. Backspace clears the last stamp.\n"
                "Then use Quantize or the Lyrics lane to align.");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%d / %d synced",
                            LySyncedCount(),
                            static_cast<int>(m_lyrics_fragments.size()));
        if (m_lyrics_tap_sync && m_lyrics_sync_cursor < static_cast<int>(m_lyrics_fragments.size()))
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f),
                               "Next: \"%s\"",
                               m_lyrics_fragments[static_cast<size_t>(m_lyrics_sync_cursor)]
                                   .text.c_str());
        }

        LyDrawPreviewStrip();

        ImGui::Separator();
        ImGui::TextUnformatted("Lyric text (one line per verse line; words split on spaces)");
#if USE_LIB_HYPHEN == TRUE
        {
            if (!m_hyph_langs_scanned)
            {
                LyHyphRescanLanguages();
            }
            const char *mode_label =
                (m_lyrics_split_mode == LySplit_Syllables) ? "Syllables" : "Words";
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::BeginCombo("Split", mode_label))
            {
                if (ImGui::Selectable("Words", m_lyrics_split_mode == LySplit_Words))
                {
                    m_lyrics_split_mode = LySplit_Words;
                }
                if (ImGui::Selectable("Syllables", m_lyrics_split_mode == LySplit_Syllables))
                {
                    m_lyrics_split_mode = LySplit_Syllables;
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            {
                ImGui::SetTooltip(
                    "Words: one fragment per whitespace-separated word.\n"
                    "Syllables: break with hyphenation patterns (EN/RU/…).");
            }
            if (m_lyrics_split_mode == LySplit_Syllables)
            {
                ImGui::SameLine();
                char lang_preview[64];
                if (std::strcmp(m_lyrics_hyph_lang, "auto") == 0)
                {
                    std::snprintf(lang_preview, sizeof(lang_preview), "Auto (Cyr→RU)");
                }
                else
                {
                    std::snprintf(lang_preview, sizeof(lang_preview), "%s", m_lyrics_hyph_lang);
                }
                ImGui::SetNextItemWidth(160.0f);
                if (ImGui::BeginCombo("Lang", lang_preview))
                {
                    if (ImGui::Selectable("Auto (Cyrillic→RU, else EN)",
                                          std::strcmp(m_lyrics_hyph_lang, "auto") == 0))
                    {
                        std::snprintf(m_lyrics_hyph_lang, sizeof(m_lyrics_hyph_lang), "auto");
                    }
                    for (const LyHyphLang &lang : m_hyph_langs)
                    {
                        const bool sel = (lang.id == m_lyrics_hyph_lang);
                        if (ImGui::Selectable(lang.name.c_str(), sel))
                        {
                            std::snprintf(m_lyrics_hyph_lang,
                                          sizeof(m_lyrics_hyph_lang),
                                          "%s",
                                          lang.id.c_str());
                        }
                        if (sel)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Get more languages…"))
                {
                    m_hyph_get_more_open = true;
                }
            }
            LyDrawHyphGetMorePopup();
            LyDrawHyphGetMoreModal();
        }
#endif
        if (ImGui::InputTextMultiline("##ly_text",
                                      m_lyrics_text_buf,
                                      sizeof(m_lyrics_text_buf),
                                      ImVec2(-1.0f, 120.0f)))
        {
            m_lyrics_dirty = true;
        }
        if (ImGui::Button("Apply text"))
        {
            LyApplyTextPreserveTimings();
            LyCommitToDocument(true);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Rebuild fragments from the text. Already-synced timings\n"
                "are kept by fragment order when possible.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Stamp playhead"))
        {
            LyStampNextAtPlayhead();
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear timings"))
        {
            LyClearAllTimings();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_lyrics_selected.empty());
        if (ImGui::Button("Quantize sel"))
        {
            LyQuantizeFragments(true);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Quantize all"))
        {
            LyQuantizeFragments(false);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_lyrics_selected.empty());
        if (ImGui::Button("Delete sel"))
        {
            LyDeleteSelectedFragments();
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextUnformatted("Fragments");
        if (ImGui::BeginTable("##ly_table",
                              4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                              ImVec2(0.0f, -1.0f)))
        {
            ImGui::TableSetupColumn("Tick", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupColumn("Sync", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(m_lyrics_fragments.size()); ++i)
            {
                LyFragment &f = m_lyrics_fragments[static_cast<size_t>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool selected = m_lyrics_selected.count(i) > 0;
                if (i == m_lyrics_sync_cursor && m_lyrics_tap_sync && !f.synced)
                {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           IM_COL32(90, 70, 20, 120));
                }
                char tick_buf[32];
                std::snprintf(tick_buf,
                              sizeof(tick_buf),
                              "%u",
                              static_cast<unsigned>(f.synced ? f.tick : 0));
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##tick",
                                     tick_buf,
                                     sizeof(tick_buf),
                                     ImGuiInputTextFlags_CharsDecimal |
                                         ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    const int v = std::atoi(tick_buf);
                    f.tick = static_cast<uint32_t>(std::max(0, v));
                    f.synced = true;
                    LyCommitToDocument(true);
                }
                if (ImGui::IsItemClicked())
                {
                    if (!ImGui::GetIO().KeyCtrl)
                    {
                        m_lyrics_selected.clear();
                    }
                    m_lyrics_selected.insert(i);
                }
                ImGui::TableSetColumnIndex(1);
                char text_buf[256];
                std::snprintf(text_buf, sizeof(text_buf), "%s", f.text.c_str());
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##txt", text_buf, sizeof(text_buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue))
                {
                    f.text = text_buf;
                    LyRebuildTextBufferFromFragments();
                    LyCommitToDocument(true);
                }
                if (ImGui::IsItemClicked())
                {
                    if (!ImGui::GetIO().KeyCtrl)
                    {
                        m_lyrics_selected.clear();
                    }
                    m_lyrics_selected.insert(i);
                }
                if (selected)
                {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                           IM_COL32(60, 80, 120, 100));
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", f.line_index + 1);
                ImGui::TableSetColumnIndex(3);
                if (ImGui::Checkbox("##syn", &f.synced))
                {
                    if (f.synced && f.tick == 0)
                    {
                        f.tick = MePlayheadTick();
                    }
                    m_lyrics_sync_cursor = LyNextUnsyncedIndex();
                    LyCommitToDocument(true);
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    /* fallthrough */
                }
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && selected && f.synced)
                {
                    MeSeekToTick(f.tick);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        LyHandleTapShortcuts(focused);
        LyWirePreviewCallback();
        ImGui::End();
    }
