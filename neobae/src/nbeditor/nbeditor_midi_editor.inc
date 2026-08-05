/* GarageBand-style MIDI Editor — included inside NbEditorApp. */

    enum class MeDragMode
    {
        None = 0,
        Marquee,
        MoveNotes,
        ResizeNotes,
        Seek,
        LoopStart,
        LoopEnd,
        AutoNode,
        Pan
    };

    enum class MeAutoKind
    {
        Tempo = 0,
        Controller,
        PitchBend
    };

    struct MeAutoLaneDesc
    {
        const char *label;
        MeAutoKind kind;
        unsigned char cc;
        ImU32 color;
        int max_value;
        bool bipolar;
    };

    struct MeNoteClip
    {
        BAERmfEditorNoteInfo info{};
    };

    struct MeMidiUndoState
    {
        std::vector<unsigned char> serialized;
    };

    static const MeAutoLaneDesc *MeAutoLanes(int *out_count)
    {
        static const MeAutoLaneDesc kLanes[] = {
            {"Tempo", MeAutoKind::Tempo, 0, IM_COL32(202, 128, 52, 255), 240, false},
            {"Mod Wheel (CC1)", MeAutoKind::Controller, 1, IM_COL32(74, 136, 208, 255), 127, false},
            {"Breath (CC2)", MeAutoKind::Controller, 2, IM_COL32(166, 96, 196, 255), 127, false},
            {"Volume (CC7)", MeAutoKind::Controller, 7, IM_COL32(74, 136, 208, 255), 127, false},
            {"Pan (CC10)", MeAutoKind::Controller, 10, IM_COL32(76, 170, 92, 255), 127, true},
            {"Expression (CC11)", MeAutoKind::Controller, 11, IM_COL32(228, 148, 56, 255), 127, false},
            {"Sustain (CC64)", MeAutoKind::Controller, 64, IM_COL32(82, 172, 170, 255), 127, false},
            {"Resonance (CC71)", MeAutoKind::Controller, 71, IM_COL32(76, 170, 92, 255), 127, false},
            {"Release (CC72)", MeAutoKind::Controller, 72, IM_COL32(228, 148, 56, 255), 127, false},
            {"Attack (CC73)", MeAutoKind::Controller, 73, IM_COL32(82, 172, 170, 255), 127, false},
            {"Brightness (CC74)", MeAutoKind::Controller, 74, IM_COL32(210, 96, 118, 255), 127, false},
            {"Reverb Send (CC91)", MeAutoKind::Controller, 91, IM_COL32(74, 136, 208, 255), 127, false},
            {"Chorus Send (CC93)", MeAutoKind::Controller, 93, IM_COL32(166, 96, 196, 255), 127, false},
            {"Pitch Bend", MeAutoKind::PitchBend, 0, IM_COL32(188, 120, 214, 255), 16383, true},
        };
        if (out_count)
        {
            *out_count = static_cast<int>(sizeof(kLanes) / sizeof(kLanes[0]));
        }
        return kLanes;
    }

    void OpenMidiEditor()
    {
        m_midi_editor_open = true;
        if (m_document && m_midi_selected_track < 0)
        {
            uint16_t track_count = 0;
            if (BAERmfEditorDocument_GetTrackCount(m_document, &track_count) == BAE_NO_ERROR &&
                track_count > 0)
            {
                m_midi_selected_track = 0;
            }
        }
        MeEnsureTrackMuteSize();
        SetStatus("MIDI Editor open");
    }

    void MeEnsureTrackMuteSize()
    {
        uint16_t track_count = 0;
        if (m_document)
        {
            BAERmfEditorDocument_GetTrackCount(m_document, &track_count);
        }
        if (m_midi_track_muted.size() < track_count)
        {
            m_midi_track_muted.resize(track_count, false);
        }
        if (m_midi_track_solo.size() < track_count)
        {
            m_midi_track_solo.resize(track_count, false);
        }
    }

    uint16_t MeGetTpq() const
    {
        uint16_t tpq = 480;
        if (m_document)
        {
            BAERmfEditorDocument_GetTicksPerQuarter(m_document, &tpq);
        }
        if (tpq == 0)
        {
            tpq = 480;
        }
        return tpq;
    }

    uint32_t MeSnapTicks() const
    {
        const uint16_t tpq = MeGetTpq();
        const int div = std::max(1, m_midi_snap_division);
        return std::max(1u, static_cast<uint32_t>(tpq) / static_cast<uint32_t>(div));
    }

    uint32_t MeSnapTick(uint32_t tick) const
    {
        if (!m_midi_snap_enabled)
        {
            return tick;
        }
        const uint32_t snap = MeSnapTicks();
        return ((tick + snap / 2u) / snap) * snap;
    }

    uint64_t MeTicksToMicroseconds(uint32_t tick) const
    {
        if (!m_document)
        {
            return 0;
        }
        const uint16_t tpq = MeGetTpq();
        uint32_t base_bpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &base_bpm);
        if (base_bpm == 0)
        {
            base_bpm = 120;
        }
        uint32_t tempo_count = 0;
        uint32_t current_tick = 0;
        uint32_t us_per_q = 60000000UL / base_bpm;
        uint64_t usec = 0;
        if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempo_count) != BAE_NO_ERROR)
        {
            return (static_cast<uint64_t>(tick) * us_per_q) / tpq;
        }
        for (uint32_t i = 0; i < tempo_count; ++i)
        {
            uint32_t event_tick = 0;
            uint32_t event_us = 0;
            if (BAERmfEditorDocument_GetTempoEvent(m_document, i, &event_tick, &event_us) != BAE_NO_ERROR)
            {
                continue;
            }
            if (event_tick > tick)
            {
                break;
            }
            if (event_tick > current_tick)
            {
                usec += (static_cast<uint64_t>(event_tick - current_tick) * us_per_q) / tpq;
            }
            current_tick = event_tick;
            if (event_us > 0)
            {
                us_per_q = event_us;
            }
        }
        if (tick > current_tick)
        {
            usec += (static_cast<uint64_t>(tick - current_tick) * us_per_q) / tpq;
        }
        return usec;
    }

    uint32_t MeMicrosecondsToTicks(uint32_t usec) const
    {
        if (!m_document)
        {
            return 0;
        }
        const uint16_t tpq = MeGetTpq();
        uint32_t base_bpm = 120;
        BAERmfEditorDocument_GetTempoBPM(m_document, &base_bpm);
        if (base_bpm == 0)
        {
            base_bpm = 120;
        }
        uint32_t tempo_count = 0;
        uint32_t current_tick = 0;
        uint32_t us_per_q = 60000000UL / base_bpm;
        uint64_t remaining = usec;
        if (BAERmfEditorDocument_GetTempoEventCount(m_document, &tempo_count) != BAE_NO_ERROR)
        {
            return static_cast<uint32_t>((remaining * tpq) / us_per_q);
        }
        for (uint32_t i = 0; i < tempo_count; ++i)
        {
            uint32_t event_tick = 0;
            uint32_t event_us = 0;
            if (BAERmfEditorDocument_GetTempoEvent(m_document, i, &event_tick, &event_us) != BAE_NO_ERROR)
            {
                continue;
            }
            if (event_tick <= current_tick)
            {
                if (event_us > 0)
                {
                    us_per_q = event_us;
                }
                continue;
            }
            const uint32_t segment_ticks = event_tick - current_tick;
            const uint64_t segment_us =
                (static_cast<uint64_t>(segment_ticks) * us_per_q) / tpq;
            if (remaining < segment_us)
            {
                return current_tick +
                       static_cast<uint32_t>((remaining * tpq) / us_per_q);
            }
            remaining -= segment_us;
            current_tick = event_tick;
            if (event_us > 0)
            {
                us_per_q = event_us;
            }
        }
        return current_tick + static_cast<uint32_t>((remaining * tpq) / us_per_q);
    }

    uint32_t MeSongLengthTicks() const
    {
        if (!m_document)
        {
            return MeGetTpq() * 16u;
        }
        uint16_t track_count = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &track_count);
        uint32_t max_tick = MeGetTpq() * 16u;
        for (uint16_t t = 0; t < track_count; ++t)
        {
            uint32_t eot = 0;
            if (BAERmfEditorDocument_GetTrackEndOfTrackTick(m_document, t, &eot) == BAE_NO_ERROR)
            {
                max_tick = std::max(max_tick, eot);
            }
            uint32_t note_count = 0;
            if (BAERmfEditorDocument_GetNoteCount(m_document, t, &note_count) != BAE_NO_ERROR)
            {
                continue;
            }
            for (uint32_t n = 0; n < note_count; ++n)
            {
                BAERmfEditorNoteInfo info{};
                if (BAERmfEditorDocument_GetNoteInfo(m_document, t, n, &info) == BAE_NO_ERROR)
                {
                    max_tick = std::max(max_tick, info.startTick + info.durationTicks);
                }
            }
        }
        return max_tick + MeGetTpq() * 4u;
    }

    /* Shared left gutter for track chrome / piano keys so time (and playheads) line up. */
    static constexpr float kMeTimeGutter = 168.0f;

    float MeTickToX(uint32_t tick, float origin_x) const
    {
        return origin_x + (static_cast<float>(tick) - m_midi_scroll_tick) * m_midi_px_per_tick;
    }

    uint32_t MeXToTick(float x, float origin_x) const
    {
        const float ticks = m_midi_scroll_tick + (x - origin_x) / std::max(0.001f, m_midi_px_per_tick);
        return static_cast<uint32_t>(std::max(0.0f, ticks));
    }

    float MePitchToY(int pitch, float origin_y) const
    {
        const int top = 127 - pitch;
        return origin_y + (static_cast<float>(top) - m_midi_scroll_pitch) * m_midi_px_per_pitch;
    }

    int MeYToPitch(float y, float origin_y) const
    {
        const float row = m_midi_scroll_pitch + (y - origin_y) / std::max(0.001f, m_midi_px_per_pitch);
        const int pitch = 127 - static_cast<int>(std::floor(row));
        return std::clamp(pitch, 0, 127);
    }

    uint32_t MePlayheadTick() const
    {
        if (!m_song)
        {
            return m_midi_playhead_tick;
        }
        uint32_t pos_us = 0;
        if (BAESong_GetMicrosecondPosition(m_song, &pos_us) != BAE_NO_ERROR)
        {
            return m_midi_playhead_tick;
        }
        return MeMicrosecondsToTicks(pos_us);
    }

    void MeSeekToTick(uint32_t tick)
    {
        m_midi_playhead_tick = tick;
        if (!m_song)
        {
            return;
        }
        const uint64_t usec = MeTicksToMicroseconds(tick);
        const uint32_t clamped =
            usec > 0xffffffffULL ? 0xffffffffu : static_cast<uint32_t>(usec);
        BAESong_SetMicrosecondPosition(m_song, clamped);
    }

    void MeApplyTrackMutes()
    {
        if (!m_song)
        {
            return;
        }
        MeEnsureTrackMuteSize();
        bool any_solo = false;
        for (bool s : m_midi_track_solo)
        {
            if (s)
            {
                any_solo = true;
                break;
            }
        }
        const size_t n = m_midi_track_muted.size();
        for (size_t i = 0; i < n && i < 64; ++i)
        {
            const bool muted =
                any_solo ? !m_midi_track_solo[i] : m_midi_track_muted[i];
            if (muted)
            {
                BAESong_MuteTrack(m_song, static_cast<uint16_t>(i));
            }
            else
            {
                BAESong_UnmuteTrack(m_song, static_cast<uint16_t>(i));
            }
        }
    }

    void MeCommitDocumentEdit(bool reload_playback)
    {
        m_document_dirty = true;
        (void)SyncSessionSongsFromDocument();
        if (reload_playback && m_document)
        {
            uint32_t restore_us = 0;
            bool was_paused = true;
            if (m_song)
            {
                (void)BAESong_GetMicrosecondPosition(m_song, &restore_us);
                BAE_BOOL paused = TRUE;
                BAESong_IsPaused(m_song, &paused);
                was_paused = (paused != FALSE) || !m_song_started;
            }
            (void)ReloadPlaybackFromDocument(was_paused);
            m_document_dirty = true;
            if (m_song && restore_us > 0)
            {
                BAESong_SetMicrosecondPosition(m_song, restore_us);
            }
            MeApplyTrackMutes();
        }
        m_midi_note_cache_dirty = true;
    }

    bool MeCaptureUndo(MeMidiUndoState *out) const
    {
        if (!out || !m_document)
        {
            return false;
        }
        unsigned char *data = nullptr;
        uint32_t size = 0;
        uint32_t reason = 0;
        const bool use_zmf = BAERmfEditorDocument_RequiresZmf(m_document, &reason) != FALSE;
        if (BAERmfEditorDocument_SaveAsRmfToMemory(m_document, use_zmf, &data, &size) != BAE_NO_ERROR ||
            !data || size == 0)
        {
            if (data)
            {
                XDisposePtr((XPTR)data);
            }
            return false;
        }
        out->serialized.assign(data, data + size);
        XDisposePtr((XPTR)data);
        return true;
    }

    bool MeRestoreUndo(const MeMidiUndoState &state)
    {
        if (state.serialized.empty())
        {
            return false;
        }
        BAERmfEditorDocument *loaded = BAERmfEditorDocument_LoadFromMemory(
            state.serialized.data(),
            static_cast<uint32_t>(state.serialized.size()),
            BAE_RMF);
        if (!loaded)
        {
            return false;
        }
        if (m_document)
        {
            BAERmfEditorDocument_Delete(m_document);
        }
        m_document = loaded;
        m_midi_selected_notes.clear();
        m_midi_primary_note = -1;
        m_midi_note_cache_dirty = true;
        MeCommitDocumentEdit(true);
        return true;
    }

    void MeBeginUndo()
    {
        if (!m_document || m_midi_undo_restoring)
        {
            return;
        }
        m_midi_undo_pending_valid = MeCaptureUndo(&m_midi_undo_pending);
    }

    void MeCommitUndo()
    {
        if (!m_document || !m_midi_undo_pending_valid || m_midi_undo_restoring)
        {
            m_midi_undo_pending_valid = false;
            return;
        }
        MeMidiUndoState after;
        if (!MeCaptureUndo(&after) || after.serialized == m_midi_undo_pending.serialized)
        {
            m_midi_undo_pending_valid = false;
            return;
        }
        m_midi_undo.push_back(std::move(m_midi_undo_pending));
        while (m_midi_undo.size() > 64u)
        {
            m_midi_undo.pop_front();
        }
        m_midi_redo.clear();
        m_midi_undo_pending_valid = false;
        MeCommitDocumentEdit(true);
    }

    void MeCancelUndo()
    {
        m_midi_undo_pending_valid = false;
    }

    void MeUndo()
    {
        if (m_midi_undo.empty() || !m_document)
        {
            return;
        }
        MeMidiUndoState current;
        if (!MeCaptureUndo(&current))
        {
            return;
        }
        m_midi_undo_restoring = true;
        const MeMidiUndoState before = m_midi_undo.back();
        m_midi_undo.pop_back();
        if (MeRestoreUndo(before))
        {
            m_midi_redo.push_back(std::move(current));
            while (m_midi_redo.size() > 64u)
            {
                m_midi_redo.pop_front();
            }
            SetStatus("MIDI Editor: undo");
        }
        m_midi_undo_restoring = false;
    }

    void MeRedo()
    {
        if (m_midi_redo.empty() || !m_document)
        {
            return;
        }
        MeMidiUndoState current;
        if (!MeCaptureUndo(&current))
        {
            return;
        }
        m_midi_undo_restoring = true;
        const MeMidiUndoState next = m_midi_redo.back();
        m_midi_redo.pop_back();
        if (MeRestoreUndo(next))
        {
            m_midi_undo.push_back(std::move(current));
            SetStatus("MIDI Editor: redo");
        }
        m_midi_undo_restoring = false;
    }

    void MeClearMidiEditorState()
    {
        m_midi_selected_track = -1;
        m_midi_selected_notes.clear();
        m_midi_primary_note = -1;
        m_midi_undo.clear();
        m_midi_redo.clear();
        m_midi_undo_pending_valid = false;
        m_midi_note_clipboard.clear();
        m_midi_track_muted.clear();
        m_midi_track_solo.clear();
        m_midi_drag_mode = MeDragMode::None;
        m_midi_note_cache_dirty = true;
        m_midi_cached_notes.clear();
    }

    void MeRebuildNoteCache()
    {
        m_midi_cached_notes.clear();
        m_midi_note_cache_dirty = false;
        if (!m_document || m_midi_selected_track < 0)
        {
            return;
        }
        uint32_t note_count = 0;
        if (BAERmfEditorDocument_GetNoteCount(
                m_document, static_cast<uint16_t>(m_midi_selected_track), &note_count) != BAE_NO_ERROR)
        {
            return;
        }
        m_midi_cached_notes.reserve(note_count);
        for (uint32_t i = 0; i < note_count; ++i)
        {
            BAERmfEditorNoteInfo info{};
            if (BAERmfEditorDocument_GetNoteInfo(
                    m_document, static_cast<uint16_t>(m_midi_selected_track), i, &info) == BAE_NO_ERROR)
            {
                m_midi_cached_notes.push_back(info);
            }
        }
    }

    const std::vector<BAERmfEditorNoteInfo> &MeNotes()
    {
        if (m_midi_note_cache_dirty)
        {
            MeRebuildNoteCache();
        }
        return m_midi_cached_notes;
    }

    bool MeIsNoteSelected(int index) const
    {
        return m_midi_selected_notes.find(index) != m_midi_selected_notes.end();
    }

    void MeSelectSingleNote(int index)
    {
        m_midi_selected_notes.clear();
        if (index >= 0)
        {
            m_midi_selected_notes.insert(index);
        }
        m_midi_primary_note = index;
    }

    void MeToggleNoteSelection(int index)
    {
        if (index < 0)
        {
            return;
        }
        if (MeIsNoteSelected(index))
        {
            m_midi_selected_notes.erase(index);
            if (m_midi_primary_note == index)
            {
                m_midi_primary_note =
                    m_midi_selected_notes.empty() ? -1 : *m_midi_selected_notes.begin();
            }
        }
        else
        {
            m_midi_selected_notes.insert(index);
            m_midi_primary_note = index;
        }
    }

    int MeHitTestNote(float mx, float my, float grid_x, float grid_y, float *out_edge_dist = nullptr)
    {
        const auto &notes = MeNotes();
        int hit = -1;
        float best = 1e9f;
        for (int i = static_cast<int>(notes.size()) - 1; i >= 0; --i)
        {
            const BAERmfEditorNoteInfo &n = notes[static_cast<size_t>(i)];
            const float x0 = MeTickToX(n.startTick, grid_x);
            const float x1 = MeTickToX(n.startTick + n.durationTicks, grid_x);
            const float y0 = MePitchToY(n.note, grid_y);
            const float y1 = y0 + m_midi_px_per_pitch;
            if (mx < x0 || mx > x1 || my < y0 || my > y1)
            {
                continue;
            }
            const float dist = std::min(mx - x0, x1 - mx);
            if (dist < best)
            {
                best = dist;
                hit = i;
                if (out_edge_dist)
                {
                    *out_edge_dist = x1 - mx;
                }
            }
        }
        return hit;
    }

    void MeDeleteSelectedNotes()
    {
        if (!m_document || m_midi_selected_track < 0 || m_midi_selected_notes.empty())
        {
            return;
        }
        MeBeginUndo();
        std::vector<int> indices(m_midi_selected_notes.begin(), m_midi_selected_notes.end());
        std::sort(indices.begin(), indices.end(), std::greater<int>());
        for (int idx : indices)
        {
            BAERmfEditorDocument_DeleteNote(
                m_document, static_cast<uint16_t>(m_midi_selected_track), static_cast<uint32_t>(idx));
        }
        m_midi_selected_notes.clear();
        m_midi_primary_note = -1;
        m_midi_note_cache_dirty = true;
        MeCommitUndo();
    }

    void MeCopySelectedNotes()
    {
        m_midi_note_clipboard.clear();
        if (!m_document || m_midi_selected_track < 0)
        {
            return;
        }
        const auto &notes = MeNotes();
        uint32_t min_tick = 0xffffffffu;
        for (int idx : m_midi_selected_notes)
        {
            if (idx < 0 || idx >= static_cast<int>(notes.size()))
            {
                continue;
            }
            min_tick = std::min(min_tick, notes[static_cast<size_t>(idx)].startTick);
        }
        if (min_tick == 0xffffffffu)
        {
            return;
        }
        for (int idx : m_midi_selected_notes)
        {
            if (idx < 0 || idx >= static_cast<int>(notes.size()))
            {
                continue;
            }
            MeNoteClip clip;
            clip.info = notes[static_cast<size_t>(idx)];
            clip.info.startTick -= min_tick;
            m_midi_note_clipboard.push_back(clip);
        }
        SetStatus("MIDI notes copied");
    }

    void MePasteNotesAtPlayhead()
    {
        if (!m_document || m_midi_selected_track < 0 || m_midi_note_clipboard.empty())
        {
            return;
        }
        MeBeginUndo();
        const uint32_t base = MeSnapTick(MePlayheadTick());
        m_midi_selected_notes.clear();
        for (const MeNoteClip &clip : m_midi_note_clipboard)
        {
            BAERmfEditorNoteInfo info = clip.info;
            info.startTick = base + clip.info.startTick;
            if (BAERmfEditorDocument_AddNote(m_document,
                                             static_cast<uint16_t>(m_midi_selected_track),
                                             info.startTick,
                                             info.durationTicks,
                                             info.note,
                                             info.velocity) != BAE_NO_ERROR)
            {
                continue;
            }
            uint32_t count = 0;
            BAERmfEditorDocument_GetNoteCount(
                m_document, static_cast<uint16_t>(m_midi_selected_track), &count);
            if (count > 0)
            {
                const uint32_t idx = count - 1;
                BAERmfEditorDocument_SetNoteInfo(
                    m_document, static_cast<uint16_t>(m_midi_selected_track), idx, &info);
                m_midi_selected_notes.insert(static_cast<int>(idx));
                m_midi_primary_note = static_cast<int>(idx);
            }
        }
        m_midi_note_cache_dirty = true;
        MeCommitUndo();
        SetStatus("MIDI notes pasted");
    }

    void MeAddTrack()
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
        char name_buf[64];
        uint16_t count = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &count);
        std::snprintf(name_buf, sizeof(name_buf), "Track %u", static_cast<unsigned>(count + 1));
        setup.name = name_buf;
        uint16_t out_index = 0;
        if (BAERmfEditorDocument_AddTrack(m_document, &setup, &out_index) == BAE_NO_ERROR)
        {
            m_midi_selected_track = static_cast<int>(out_index);
            m_midi_selected_notes.clear();
            MeEnsureTrackMuteSize();
            MeCommitUndo();
        }
        else
        {
            MeCancelUndo();
        }
    }

    void MeDeleteSelectedTrack()
    {
        if (!m_document || m_midi_selected_track < 0)
        {
            return;
        }
        uint16_t count = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &count);
        if (count <= 1)
        {
            SetStatus("Cannot delete the last track");
            return;
        }
        MeBeginUndo();
        if (BAERmfEditorDocument_DeleteTrack(
                m_document, static_cast<uint16_t>(m_midi_selected_track)) == BAE_NO_ERROR)
        {
            m_midi_selected_track =
                std::min(m_midi_selected_track, static_cast<int>(count - 2));
            m_midi_selected_notes.clear();
            m_midi_primary_note = -1;
            MeEnsureTrackMuteSize();
            MeCommitUndo();
        }
        else
        {
            MeCancelUndo();
        }
    }

    void MeDrawRuler(ImDrawList *dl, ImVec2 origin, ImVec2 size, uint32_t length_ticks)
    {
        /* Left gutter matches track chrome / piano keys; time starts at time_x. */
        const float time_x = origin.x + kMeTimeGutter;
        const float time_w = std::max(1.0f, size.x - kMeTimeGutter);
        dl->AddRectFilled(origin,
                          ImVec2(time_x, origin.y + size.y),
                          IM_COL32(28, 30, 36, 255));
        dl->AddRectFilled(ImVec2(time_x, origin.y),
                          ImVec2(origin.x + size.x, origin.y + size.y),
                          IM_COL32(32, 34, 40, 255));
        const uint16_t tpq = MeGetTpq();
        const uint32_t beat = tpq;
        const uint32_t bar = beat * 4u;
        const uint32_t start = static_cast<uint32_t>(std::max(0.0f, m_midi_scroll_tick));
        const uint32_t end = MeXToTick(time_x + time_w, time_x) + beat;
        for (uint32_t t = (start / beat) * beat; t <= end && t <= length_ticks + bar; t += beat)
        {
            const float x = MeTickToX(t, time_x);
            if (x < time_x || x > time_x + time_w)
            {
                continue;
            }
            const bool is_bar = (t % bar) == 0;
            dl->AddLine(ImVec2(x, origin.y + (is_bar ? 0.0f : size.y * 0.45f)),
                        ImVec2(x, origin.y + size.y),
                        is_bar ? IM_COL32(180, 180, 190, 220) : IM_COL32(100, 100, 110, 160));
            if (is_bar)
            {
                char label[32];
                std::snprintf(label, sizeof(label), "%u", (t / bar) + 1);
                dl->AddText(ImVec2(x + 3.0f, origin.y + 2.0f), IM_COL32(210, 210, 220, 255), label);
            }
        }

        bool loop_on = false;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
        int32_t loop_count = 0;
        BAERmfEditorDocument_GetMidiLoopMarkers(
            m_document, &loop_on, &loop_start, &loop_end, &loop_count);
        if (loop_on && loop_end > loop_start)
        {
            const float x0 = MeTickToX(loop_start, time_x);
            const float x1 = MeTickToX(loop_end, time_x);
            dl->AddRectFilled(ImVec2(std::max(x0, time_x), origin.y),
                              ImVec2(std::min(x1, time_x + time_w), origin.y + size.y),
                              IM_COL32(80, 160, 220, 40));
            if (x0 >= time_x && x0 <= time_x + time_w)
            {
                dl->AddLine(ImVec2(x0, origin.y),
                            ImVec2(x0, origin.y + size.y),
                            IM_COL32(80, 180, 255, 220),
                            2.0f);
            }
            if (x1 >= time_x && x1 <= time_x + time_w)
            {
                dl->AddLine(ImVec2(x1, origin.y),
                            ImVec2(x1, origin.y + size.y),
                            IM_COL32(80, 180, 255, 220),
                            2.0f);
            }
        }

        const uint32_t play_tick = MePlayheadTick();
        const float play_x = MeTickToX(play_tick, time_x);
        if (play_x >= time_x && play_x <= time_x + time_w)
        {
            dl->AddLine(ImVec2(play_x, origin.y),
                        ImVec2(play_x, origin.y + size.y),
                        IM_COL32(255, 90, 70, 255),
                        2.0f);
        }

        ImGui::SetCursorScreenPos(ImVec2(time_x, origin.y));
        ImGui::InvisibleButton("##me_ruler", ImVec2(time_w, size.y));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            if (m_midi_drag_mode == MeDragMode::None)
            {
                if (loop_on)
                {
                    const float x0 = MeTickToX(loop_start, time_x);
                    const float x1 = MeTickToX(loop_end, time_x);
                    if (std::fabs(mp.x - x0) < 6.0f)
                    {
                        m_midi_drag_mode = MeDragMode::LoopStart;
                        MeBeginUndo();
                    }
                    else if (std::fabs(mp.x - x1) < 6.0f)
                    {
                        m_midi_drag_mode = MeDragMode::LoopEnd;
                        MeBeginUndo();
                    }
                    else
                    {
                        m_midi_drag_mode = MeDragMode::Seek;
                    }
                }
                else
                {
                    m_midi_drag_mode = MeDragMode::Seek;
                }
            }
            const uint32_t tick = MeSnapTick(MeXToTick(mp.x, time_x));
            if (m_midi_drag_mode == MeDragMode::Seek)
            {
                MeSeekToTick(tick);
            }
            else if (m_midi_drag_mode == MeDragMode::LoopStart)
            {
                loop_start = std::min(tick, loop_end > 0 ? loop_end - 1 : tick);
                BAERmfEditorDocument_SetMidiLoopMarkers(
                    m_document, true, loop_start, loop_end, loop_count);
            }
            else if (m_midi_drag_mode == MeDragMode::LoopEnd)
            {
                loop_end = std::max(tick, loop_start + 1);
                BAERmfEditorDocument_SetMidiLoopMarkers(
                    m_document, true, loop_start, loop_end, loop_count);
            }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && m_midi_drag_mode == MeDragMode::None)
        {
            MeSeekToTick(MeSnapTick(MeXToTick(ImGui::GetIO().MousePos.x, time_x)));
        }
        if (ImGui::IsItemDeactivated())
        {
            if (m_midi_drag_mode == MeDragMode::LoopStart || m_midi_drag_mode == MeDragMode::LoopEnd)
            {
                MeCommitUndo();
            }
            m_midi_drag_mode = MeDragMode::None;
        }
    }

    void MeDrawTrackTimeline(float width, float height)
    {
        if (!m_document)
        {
            ImGui::TextDisabled("No active song document.");
            return;
        }
        MeEnsureTrackMuteSize();
        uint16_t track_count = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &track_count);
        const uint32_t length_ticks = MeSongLengthTicks();
        const float chrome_w = kMeTimeGutter;
        const float lane_h = 48.0f;
        const float ruler_h = 22.0f;

        ImGui::BeginChild("##me_timeline", ImVec2(width, height), true);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 win = ImGui::GetCursorScreenPos();
        /* Content width must match piano-roll child so time origins share screen X. */
        const float content_w = ImGui::GetContentRegionAvail().x;

        MeDrawRuler(dl, win, ImVec2(content_w, ruler_h), length_ticks);

        float y = win.y + ruler_h + 4.0f;
        for (uint16_t t = 0; t < track_count; ++t)
        {
            BAERmfEditorTrackInfo info{};
            BAERmfEditorDocument_GetTrackInfo(m_document, t, &info);
            const bool selected = (static_cast<int>(t) == m_midi_selected_track);
            const ImVec2 lane0(win.x, y);
            const ImVec2 lane1(win.x + content_w, y + lane_h);
            dl->AddRectFilled(lane0,
                              lane1,
                              selected ? IM_COL32(48, 56, 72, 255) : IM_COL32(28, 30, 36, 255));
            dl->AddRect(lane0, lane1, IM_COL32(70, 74, 86, 255));

            ImGui::SetCursorScreenPos(ImVec2(win.x + 4.0f, y + 4.0f));
            ImGui::PushID(static_cast<int>(t) + 9000);
            char label[128];
            std::snprintf(label,
                          sizeof(label),
                          "%s##trk%d",
                          (info.name && info.name[0]) ? info.name : "Track",
                          static_cast<int>(t));
            if (ImGui::Selectable(label, selected, 0, ImVec2(chrome_w - 12.0f, 18.0f)))
            {
                m_midi_selected_track = static_cast<int>(t);
                m_midi_selected_notes.clear();
                m_midi_primary_note = -1;
                m_midi_note_cache_dirty = true;
            }
            ImGui::SetCursorScreenPos(ImVec2(win.x + 4.0f, y + 24.0f));
            bool muted = t < m_midi_track_muted.size() ? m_midi_track_muted[t] : false;
            bool solo = t < m_midi_track_solo.size() ? m_midi_track_solo[t] : false;
            if (ImGui::Checkbox("M", &muted))
            {
                if (t < m_midi_track_muted.size())
                {
                    m_midi_track_muted[t] = muted;
                }
                MeApplyTrackMutes();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("S", &solo))
            {
                if (t < m_midi_track_solo.size())
                {
                    m_midi_track_solo[t] = solo;
                }
                MeApplyTrackMutes();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%u:%03u", info.bank, info.program);

            /* Note overview silhouette */
            const float body_x = win.x + chrome_w;
            const float body_w = std::max(1.0f, content_w - chrome_w);
            dl->AddRectFilled(ImVec2(body_x, y + 2.0f),
                              ImVec2(body_x + body_w, y + lane_h - 2.0f),
                              IM_COL32(22, 24, 28, 255));
            uint32_t note_count = 0;
            BAERmfEditorDocument_GetNoteCount(m_document, t, &note_count);
            for (uint32_t n = 0; n < note_count; ++n)
            {
                BAERmfEditorNoteInfo note{};
                if (BAERmfEditorDocument_GetNoteInfo(m_document, t, n, &note) != BAE_NO_ERROR)
                {
                    continue;
                }
                const float x0 = MeTickToX(note.startTick, body_x);
                const float x1 = MeTickToX(note.startTick + note.durationTicks, body_x);
                if (x1 < body_x || x0 > body_x + body_w)
                {
                    continue;
                }
                const float ny =
                    y + 4.0f + (1.0f - static_cast<float>(note.note) / 127.0f) * (lane_h - 12.0f);
                dl->AddRectFilled(ImVec2(std::max(x0, body_x), ny),
                                  ImVec2(std::min(x1, body_x + body_w), ny + 3.0f),
                                  IM_COL32(90, 160, 230, 200));
            }

            /* Click body to select track */
            ImGui::SetCursorScreenPos(ImVec2(body_x, y));
            if (ImGui::InvisibleButton("##lane_body", ImVec2(body_w, lane_h)))
            {
                m_midi_selected_track = static_cast<int>(t);
                m_midi_selected_notes.clear();
                m_midi_primary_note = -1;
                m_midi_note_cache_dirty = true;
            }
            ImGui::PopID();
            y += lane_h + 2.0f;
        }

        /* Shared playhead over lanes (same time origin as ruler / piano roll). */
        {
            const float time_x = win.x + chrome_w;
            const float play_x = MeTickToX(MePlayheadTick(), time_x);
            if (play_x >= time_x && play_x <= win.x + content_w)
            {
                dl->AddLine(ImVec2(play_x, win.y + ruler_h),
                            ImVec2(play_x, y),
                            IM_COL32(255, 90, 70, 220),
                            2.0f);
            }
        }

        if (ImGui::BeginPopupContextWindow("##me_track_ctx"))
        {
            if (ImGui::MenuItem("Add Track"))
            {
                MeAddTrack();
            }
            if (ImGui::MenuItem("Delete Track", nullptr, false, track_count > 1))
            {
                MeDeleteSelectedTrack();
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }

    void MeDrawPianoRoll(float width, float height)
    {
        if (!m_document || m_midi_selected_track < 0)
        {
            ImGui::BeginChild("##me_roll_empty", ImVec2(width, height), true);
            ImGui::TextDisabled("Select a track to edit notes.");
            ImGui::EndChild();
            return;
        }

        const uint32_t length_ticks = MeSongLengthTicks();
        const float key_w = kMeTimeGutter; /* same as timeline chrome — keeps playheads aligned */
        const float key_draw_w = 48.0f;
        const float auto_header_h = 22.0f;
        const float auto_splitter_h = 5.0f;
        const float min_lane_h = 52.0f;
        int auto_count = 0;
        const MeAutoLaneDesc *auto_lanes = MeAutoLanes(&auto_count);
        int visible_auto = 0;
        for (int i = 0; i < auto_count; ++i)
        {
            if (m_midi_auto_lane_visible[static_cast<size_t>(i)])
            {
                ++visible_auto;
            }
        }
        const float min_roll_h = 120.0f;
        const float min_auto_h = auto_header_h + min_lane_h + 8.0f;
        const float max_auto_h = std::max(min_auto_h, height - min_roll_h - auto_splitter_h - 4.0f);
        float auto_h = auto_header_h;
        if (!m_midi_auto_collapsed && visible_auto > 0)
        {
            /* Clamp display only — keep preferred height if the window is temporarily small. */
            auto_h = std::clamp(m_midi_auto_panel_h, min_auto_h, max_auto_h);
        }
        else if (!m_midi_auto_collapsed)
        {
            auto_h = auto_header_h;
        }
        const bool show_auto_split = !m_midi_auto_collapsed && visible_auto > 0;
        const float split_h = show_auto_split ? auto_splitter_h : 0.0f;
        const float roll_h = std::max(min_roll_h, height - auto_h - split_h - 4.0f);

        ImGui::BeginChild("##me_roll", ImVec2(width, roll_h), true, ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        const ImVec2 grid_origin(origin.x + key_w, origin.y);
        const ImVec2 grid_size(std::max(1.0f, size.x - key_w), size.y);

        /* Gutter fill + piano keys — leave a hard edge so the roll never paints over keys. */
        const float key_x1 = grid_origin.x - 2.0f;
        dl->AddRectFilled(origin,
                          ImVec2(key_x1, origin.y + size.y),
                          IM_COL32(28, 30, 36, 255));
        const float key_x0 = key_x1 - key_draw_w;
        for (int pitch = 0; pitch < 128; ++pitch)
        {
            const float y0 = MePitchToY(pitch, grid_origin.y);
            const float y1 = y0 + m_midi_px_per_pitch;
            if (y1 < origin.y || y0 > origin.y + size.y)
            {
                continue;
            }
            const int pc = pitch % 12;
            const bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
            dl->AddRectFilled(ImVec2(key_x0, y0),
                              ImVec2(key_x1, y1),
                              black ? IM_COL32(40, 42, 48, 255) : IM_COL32(220, 220, 225, 255));
            /* Black separators between keys (stronger on white keys so they don't merge). */
            dl->AddLine(ImVec2(key_x0, y1 - 0.5f),
                        ImVec2(key_x1, y1 - 0.5f),
                        black ? IM_COL32(20, 20, 24, 255) : IM_COL32(0, 0, 0, 255));
            if (pc == 0)
            {
                char lab[16];
                std::snprintf(lab, sizeof(lab), "C%d", (pitch / 12) - 1);
                dl->AddText(ImVec2(key_x0 + 4.0f, y0 + 1.0f),
                            IM_COL32(30, 30, 35, 255),
                            lab);
            }
        }
        /* Separator strip between keyboard and time grid. */
        dl->AddRectFilled(ImVec2(key_x1, origin.y),
                          ImVec2(grid_origin.x, origin.y + size.y),
                          IM_COL32(12, 12, 16, 255));

        /* Clip all roll content so notes/grid/playhead cannot overlap the keys. */
        dl->PushClipRect(grid_origin,
                         ImVec2(grid_origin.x + grid_size.x, grid_origin.y + grid_size.y),
                         true);

        /* Grid background */
        dl->AddRectFilled(grid_origin,
                          ImVec2(grid_origin.x + grid_size.x, grid_origin.y + grid_size.y),
                          IM_COL32(24, 26, 32, 255));
        const uint16_t tpq = MeGetTpq();
        const uint32_t beat = tpq;
        const uint32_t bar = beat * 4u;
        const uint32_t start_tick = static_cast<uint32_t>(std::max(0.0f, m_midi_scroll_tick));
        const uint32_t end_tick = MeXToTick(grid_origin.x + grid_size.x, grid_origin.x);
        for (uint32_t t = (start_tick / beat) * beat; t <= end_tick + beat; t += beat)
        {
            const float x = MeTickToX(t, grid_origin.x);
            if (x < grid_origin.x || x > grid_origin.x + grid_size.x)
            {
                continue;
            }
            const bool is_bar = (t % bar) == 0;
            dl->AddLine(ImVec2(x, grid_origin.y),
                        ImVec2(x, grid_origin.y + grid_size.y),
                        is_bar ? IM_COL32(70, 74, 90, 255) : IM_COL32(40, 44, 54, 255));
        }
        for (int pitch = 0; pitch < 128; ++pitch)
        {
            const float y = MePitchToY(pitch, grid_origin.y);
            if (y < grid_origin.y || y > grid_origin.y + grid_size.y)
            {
                continue;
            }
            const int pc = pitch % 12;
            const bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
            if (black)
            {
                dl->AddRectFilled(ImVec2(grid_origin.x, y),
                                  ImVec2(grid_origin.x + grid_size.x, y + m_midi_px_per_pitch),
                                  IM_COL32(30, 32, 40, 180));
            }
            dl->AddLine(ImVec2(grid_origin.x, y),
                        ImVec2(grid_origin.x + grid_size.x, y),
                        IM_COL32(36, 38, 46, 255));
        }

        /* Notes — single draw path for selected and unselected */
        const auto &notes = MeNotes();
        for (int i = 0; i < static_cast<int>(notes.size()); ++i)
        {
            BAERmfEditorNoteInfo n = notes[static_cast<size_t>(i)];
            /* Live drag preview */
            if ((m_midi_drag_mode == MeDragMode::MoveNotes ||
                 m_midi_drag_mode == MeDragMode::ResizeNotes) &&
                MeIsNoteSelected(i))
            {
                if (m_midi_drag_mode == MeDragMode::MoveNotes)
                {
                    const int32_t st = static_cast<int32_t>(n.startTick) + m_midi_drag_tick_delta;
                    n.startTick = static_cast<uint32_t>(std::max(0, st));
                    const int pitch = static_cast<int>(n.note) + m_midi_drag_pitch_delta;
                    n.note = static_cast<unsigned char>(std::clamp(pitch, 0, 127));
                }
                else if (m_midi_drag_mode == MeDragMode::ResizeNotes)
                {
                    const int32_t dur =
                        static_cast<int32_t>(n.durationTicks) + m_midi_drag_tick_delta;
                    n.durationTicks = static_cast<uint32_t>(std::max(static_cast<int32_t>(MeSnapTicks()), dur));
                }
            }
            const float x0 = MeTickToX(n.startTick, grid_origin.x);
            const float x1 = MeTickToX(n.startTick + std::max(1u, n.durationTicks), grid_origin.x);
            const float y0 = MePitchToY(n.note, grid_origin.y);
            const float y1 = y0 + m_midi_px_per_pitch - 1.0f;
            if (x1 < grid_origin.x || x0 > grid_origin.x + grid_size.x ||
                y1 < grid_origin.y || y0 > grid_origin.y + grid_size.y)
            {
                continue;
            }
            const bool selected = MeIsNoteSelected(i);
            const float vel_t = static_cast<float>(n.velocity) / 127.0f;
            const ImU32 fill =
                selected ? IM_COL32(255, 170, 70, 255)
                         : IM_COL32(static_cast<int>(60 + 120 * vel_t),
                                    static_cast<int>(120 + 80 * vel_t),
                                    static_cast<int>(200 - 40 * vel_t),
                                    255);
            const ImU32 border = selected ? IM_COL32(255, 230, 160, 255) : IM_COL32(20, 30, 50, 255);
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill, 2.0f);
            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), border, 2.0f);
        }

        /* Marquee */
        if (m_midi_drag_mode == MeDragMode::Marquee)
        {
            const ImVec2 a = m_midi_marquee_a;
            const ImVec2 b = ImGui::GetIO().MousePos;
            const ImVec2 min_p(std::min(a.x, b.x), std::min(a.y, b.y));
            const ImVec2 max_p(std::max(a.x, b.x), std::max(a.y, b.y));
            dl->AddRectFilled(min_p, max_p, IM_COL32(100, 160, 255, 40));
            dl->AddRect(min_p, max_p, IM_COL32(120, 180, 255, 200));
        }

        /* Playhead */
        {
            const float play_x = MeTickToX(MePlayheadTick(), grid_origin.x);
            if (play_x >= grid_origin.x && play_x <= grid_origin.x + grid_size.x)
            {
                dl->AddLine(ImVec2(play_x, grid_origin.y),
                            ImVec2(play_x, grid_origin.y + grid_size.y),
                            IM_COL32(255, 90, 70, 255),
                            1.5f);
            }
        }

        dl->PopClipRect();

        ImGui::SetCursorScreenPos(grid_origin);
        ImGui::InvisibleButton("##me_grid", grid_size);
        const bool hovered = ImGui::IsItemHovered();
        const ImGuiIO &io = ImGui::GetIO();

        if (hovered && io.MouseWheel != 0.0f)
        {
            if (io.KeyCtrl)
            {
                const float factor = (io.MouseWheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
                m_midi_px_per_tick = std::clamp(m_midi_px_per_tick * factor, 0.02f, 2.5f);
            }
            else if (io.KeyShift)
            {
                m_midi_scroll_tick =
                    std::max(0.0f, m_midi_scroll_tick - io.MouseWheel * 40.0f / m_midi_px_per_tick);
            }
            else
            {
                m_midi_scroll_pitch =
                    std::clamp(m_midi_scroll_pitch - io.MouseWheel * 2.0f, 0.0f, 127.0f);
            }
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            float edge = 0.0f;
            const int hit = MeHitTestNote(io.MousePos.x, io.MousePos.y, grid_origin.x, grid_origin.y, &edge);
            if (hit >= 0)
            {
                if (io.KeyCtrl)
                {
                    MeToggleNoteSelection(hit);
                }
                else if (!MeIsNoteSelected(hit))
                {
                    MeSelectSingleNote(hit);
                }
                const bool resize = edge < 8.0f;
                m_midi_drag_mode = resize ? MeDragMode::ResizeNotes : MeDragMode::MoveNotes;
                m_midi_drag_origin_tick = MeXToTick(io.MousePos.x, grid_origin.x);
                m_midi_drag_origin_pitch = MeYToPitch(io.MousePos.y, grid_origin.y);
                m_midi_drag_tick_delta = 0;
                m_midi_drag_pitch_delta = 0;
                MeBeginUndo();
            }
            else
            {
                if (!io.KeyCtrl)
                {
                    m_midi_selected_notes.clear();
                    m_midi_primary_note = -1;
                }
                m_midi_drag_mode = MeDragMode::Marquee;
                m_midi_marquee_a = io.MousePos;
            }
        }

        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
            (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)))
        {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                const uint32_t tick = MeSnapTick(MeXToTick(io.MousePos.x, grid_origin.x));
                const int pitch = MeYToPitch(io.MousePos.y, grid_origin.y);
                MeBeginUndo();
                const uint32_t dur = MeSnapTicks() * 2u;
                if (BAERmfEditorDocument_AddNote(m_document,
                                                 static_cast<uint16_t>(m_midi_selected_track),
                                                 tick,
                                                 dur,
                                                 static_cast<unsigned char>(pitch),
                                                 100) == BAE_NO_ERROR)
                {
                    m_midi_note_cache_dirty = true;
                    uint32_t count = 0;
                    BAERmfEditorDocument_GetNoteCount(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &count);
                    MeSelectSingleNote(static_cast<int>(count) - 1);
                    MeCommitUndo();
                    /* Audition */
                    if (m_preview_song || m_song)
                    {
                        BAESong dest = m_preview_song ? m_preview_song : m_song;
                        BAERmfEditorTrackInfo ti{};
                        BAERmfEditorDocument_GetTrackInfo(
                            m_document, static_cast<uint16_t>(m_midi_selected_track), &ti);
                        BAESong_NoteOn(dest,
                                       ti.channel,
                                       static_cast<unsigned char>(pitch),
                                       100,
                                       0);
                    }
                }
                else
                {
                    MeCancelUndo();
                }
            }
        }

        if (m_midi_drag_mode == MeDragMode::MoveNotes || m_midi_drag_mode == MeDragMode::ResizeNotes)
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                const uint32_t cur_tick = MeXToTick(io.MousePos.x, grid_origin.x);
                const int cur_pitch = MeYToPitch(io.MousePos.y, grid_origin.y);
                int32_t dt = static_cast<int32_t>(cur_tick) -
                             static_cast<int32_t>(m_midi_drag_origin_tick);
                if (m_midi_snap_enabled)
                {
                    const int32_t snap = static_cast<int32_t>(MeSnapTicks());
                    dt = (dt / snap) * snap;
                }
                m_midi_drag_tick_delta = dt;
                m_midi_drag_pitch_delta = (m_midi_drag_mode == MeDragMode::MoveNotes)
                                             ? (cur_pitch - m_midi_drag_origin_pitch)
                                             : 0;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                const auto &cur = MeNotes();
                for (int idx : m_midi_selected_notes)
                {
                    if (idx < 0 || idx >= static_cast<int>(cur.size()))
                    {
                        continue;
                    }
                    BAERmfEditorNoteInfo info = cur[static_cast<size_t>(idx)];
                    if (m_midi_drag_mode == MeDragMode::MoveNotes)
                    {
                        const int32_t st =
                            static_cast<int32_t>(info.startTick) + m_midi_drag_tick_delta;
                        info.startTick = static_cast<uint32_t>(std::max(0, st));
                        const int pitch = static_cast<int>(info.note) + m_midi_drag_pitch_delta;
                        info.note = static_cast<unsigned char>(std::clamp(pitch, 0, 127));
                    }
                    else
                    {
                        const int32_t dur =
                            static_cast<int32_t>(info.durationTicks) + m_midi_drag_tick_delta;
                        info.durationTicks = static_cast<uint32_t>(
                            std::max(static_cast<int32_t>(MeSnapTicks()), dur));
                    }
                    BAERmfEditorDocument_SetNoteInfo(m_document,
                                                    static_cast<uint16_t>(m_midi_selected_track),
                                                    static_cast<uint32_t>(idx),
                                                    &info);
                }
                m_midi_note_cache_dirty = true;
                m_midi_drag_tick_delta = 0;
                m_midi_drag_pitch_delta = 0;
                m_midi_drag_mode = MeDragMode::None;
                MeCommitUndo();
            }
        }
        else if (m_midi_drag_mode == MeDragMode::Marquee)
        {
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            {
                const ImVec2 b = io.MousePos;
                const float min_x = std::min(m_midi_marquee_a.x, b.x);
                const float max_x = std::max(m_midi_marquee_a.x, b.x);
                const float min_y = std::min(m_midi_marquee_a.y, b.y);
                const float max_y = std::max(m_midi_marquee_a.y, b.y);
                if (!io.KeyCtrl)
                {
                    m_midi_selected_notes.clear();
                }
                const auto &cur = MeNotes();
                for (int i = 0; i < static_cast<int>(cur.size()); ++i)
                {
                    const auto &n = cur[static_cast<size_t>(i)];
                    const float x0 = MeTickToX(n.startTick, grid_origin.x);
                    const float x1 = MeTickToX(n.startTick + n.durationTicks, grid_origin.x);
                    const float y0 = MePitchToY(n.note, grid_origin.y);
                    const float y1 = y0 + m_midi_px_per_pitch;
                    if (x1 >= min_x && x0 <= max_x && y1 >= min_y && y0 <= max_y)
                    {
                        m_midi_selected_notes.insert(i);
                        m_midi_primary_note = i;
                    }
                }
                m_midi_drag_mode = MeDragMode::None;
            }
        }

        ImGui::EndChild();

        /* Drag strip between piano roll and automation. */
        if (show_auto_split)
        {
            ImGui::InvisibleButton("##me_auto_split", ImVec2(width, split_h));
            const bool split_hover = ImGui::IsItemHovered();
            const bool split_active = ImGui::IsItemActive();
            if (split_hover || split_active)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }
            if (split_active)
            {
                m_midi_auto_panel_h = std::clamp(m_midi_auto_panel_h - ImGui::GetIO().MouseDelta.y,
                                                 min_auto_h,
                                                 max_auto_h);
            }
            {
                const ImVec2 sp0 = ImGui::GetItemRectMin();
                const ImVec2 sp1 = ImGui::GetItemRectMax();
                const float mid_y = (sp0.y + sp1.y) * 0.5f;
                ImGui::GetWindowDrawList()->AddLine(
                    ImVec2(sp0.x + 8.0f, mid_y),
                    ImVec2(sp1.x - 8.0f, mid_y),
                    split_active   ? IM_COL32(160, 140, 220, 255)
                    : split_hover  ? IM_COL32(120, 110, 160, 220)
                                   : IM_COL32(70, 74, 90, 200),
                    2.0f);
            }
        }

        /* Automation lanes — resizable against the piano roll, scrollable when many. */
        ImGui::BeginChild("##me_auto", ImVec2(width, auto_h), true);
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
            if (ImGui::ArrowButton("##me_auto_toggle",
                                  m_midi_auto_collapsed ? ImGuiDir_Right : ImGuiDir_Down))
            {
                m_midi_auto_collapsed = !m_midi_auto_collapsed;
            }
            ImGui::SameLine();
            ImGui::Text("Automation (%d lane%s)",
                        visible_auto,
                        visible_auto == 1 ? "" : "s");
            ImGui::SameLine();
            ImGui::TextDisabled(show_auto_split ? "— drag bar above to resize"
                                                : "— enable lanes in toolbar");
            ImGui::PopStyleVar();

            if (m_midi_auto_collapsed)
            {
                ImGui::EndChild();
                return;
            }

            const float lanes_view_h = std::max(0.0f, auto_h - auto_header_h - 6.0f);
            /* Fill the panel when few lanes are visible; scroll when many. */
            const float auto_lane_h =
                (visible_auto > 0)
                    ? std::max(min_lane_h, lanes_view_h / static_cast<float>(visible_auto))
                    : min_lane_h;
            ImGui::BeginChild("##me_auto_scroll",
                              ImVec2(0.0f, lanes_view_h),
                              false,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImDrawList *adl = ImGui::GetWindowDrawList();
            ImVec2 aorigin = ImGui::GetCursorScreenPos();
            float ay = aorigin.y;
            int drawn = 0;
            const float lane_draw_w = ImGui::GetContentRegionAvail().x;
            for (int li = 0; li < auto_count; ++li)
            {
                if (!m_midi_auto_lane_visible[static_cast<size_t>(li)])
                {
                    continue;
                }
                const MeAutoLaneDesc &lane = auto_lanes[li];
                const ImVec2 l0(aorigin.x, ay);
                const ImVec2 l1(aorigin.x + lane_draw_w, ay + auto_lane_h - 2.0f);
                adl->AddRectFilled(l0, l1, IM_COL32(26, 28, 34, 255));
                adl->AddRect(l0, l1, IM_COL32(60, 64, 76, 255));
                adl->AddText(ImVec2(l0.x + 4.0f, l0.y + 1.0f), lane.color, lane.label);

                const float plot_x = l0.x + key_w;
                const float plot_w = std::max(1.0f, (l1.x - l0.x) - key_w);
                const float plot_y = l0.y + 16.0f;
                const float plot_h = std::max(12.0f, auto_lane_h - 20.0f);

                struct Node
                {
                    uint32_t tick;
                    int value;
                    uint32_t index;
                };
                std::vector<Node> nodes;
                if (lane.kind == MeAutoKind::Tempo)
                {
                    uint32_t count = 0;
                    BAERmfEditorDocument_GetTempoEventCount(m_document, &count);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        uint32_t tick = 0;
                        uint32_t uspq = 0;
                        if (BAERmfEditorDocument_GetTempoEvent(m_document, i, &tick, &uspq) !=
                            BAE_NO_ERROR)
                        {
                            continue;
                        }
                        const int bpm =
                            uspq > 0 ? static_cast<int>(60000000UL / uspq) : 120;
                        nodes.push_back({tick, bpm, i});
                    }
                }
                else if (lane.kind == MeAutoKind::Controller)
                {
                    uint32_t count = 0;
                    BAERmfEditorDocument_GetTrackCCEventCount(
                        m_document,
                        static_cast<uint16_t>(m_midi_selected_track),
                        lane.cc,
                        &count);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        uint32_t tick = 0;
                        unsigned char value = 0;
                        if (BAERmfEditorDocument_GetTrackCCEvent(
                                m_document,
                                static_cast<uint16_t>(m_midi_selected_track),
                                lane.cc,
                                i,
                                &tick,
                                &value) == BAE_NO_ERROR)
                        {
                            nodes.push_back({tick, value, i});
                        }
                    }
                }
                else
                {
                    uint32_t count = 0;
                    BAERmfEditorDocument_GetTrackPitchBendEventCount(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &count);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        uint32_t tick = 0;
                        uint16_t value = 0;
                        if (BAERmfEditorDocument_GetTrackPitchBendEvent(
                                m_document,
                                static_cast<uint16_t>(m_midi_selected_track),
                                i,
                                &tick,
                                &value) == BAE_NO_ERROR)
                        {
                            nodes.push_back({tick, static_cast<int>(value), i});
                        }
                    }
                }

                auto value_to_y = [&](int value) {
                    float norm = static_cast<float>(value) / static_cast<float>(lane.max_value);
                    if (lane.bipolar && lane.kind == MeAutoKind::PitchBend)
                    {
                        norm = static_cast<float>(value) / 16383.0f;
                    }
                    else if (lane.bipolar)
                    {
                        norm = static_cast<float>(value) / 127.0f;
                    }
                    norm = std::clamp(norm, 0.0f, 1.0f);
                    return plot_y + plot_h * (1.0f - norm);
                };

                for (size_t i = 1; i < nodes.size(); ++i)
                {
                    const float x0 = MeTickToX(nodes[i - 1].tick, plot_x);
                    const float x1 = MeTickToX(nodes[i].tick, plot_x);
                    adl->AddLine(ImVec2(x0, value_to_y(nodes[i - 1].value)),
                                 ImVec2(x1, value_to_y(nodes[i].value)),
                                 lane.color,
                                 1.5f);
                }
                for (const Node &node : nodes)
                {
                    const float nx = MeTickToX(node.tick, plot_x);
                    const float ny = value_to_y(node.value);
                    adl->AddCircleFilled(ImVec2(nx, ny), 4.0f, lane.color);
                    adl->AddCircle(ImVec2(nx, ny), 4.0f, IM_COL32(255, 255, 255, 180));
                }

                ImGui::SetCursorScreenPos(ImVec2(plot_x, plot_y));
                ImGui::PushID(li + 12000);
                ImGui::InvisibleButton("##auto_lane", ImVec2(plot_w, plot_h));
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                {
                    const uint32_t tick = MeSnapTick(MeXToTick(io.MousePos.x, plot_x));
                    float hit_node = -1.0f;
                    int hit_index = -1;
                    for (const Node &node : nodes)
                    {
                        const float nx = MeTickToX(node.tick, plot_x);
                        const float ny = value_to_y(node.value);
                        const float d = std::hypot(io.MousePos.x - nx, io.MousePos.y - ny);
                        if (d < 7.0f && (hit_node < 0.0f || d < hit_node))
                        {
                            hit_node = d;
                            hit_index = static_cast<int>(node.index);
                        }
                    }
                    MeBeginUndo();
                    if (hit_index >= 0)
                    {
                        m_midi_drag_mode = MeDragMode::AutoNode;
                        m_midi_auto_drag_lane = li;
                        m_midi_auto_drag_index = hit_index;
                    }
                    else
                    {
                        const float norm =
                            std::clamp(1.0f - (io.MousePos.y - plot_y) / plot_h, 0.0f, 1.0f);
                        if (lane.kind == MeAutoKind::Tempo)
                        {
                            const int bpm = std::clamp(static_cast<int>(norm * 240.0f), 20, 240);
                            BAERmfEditorDocument_AddTempoEvent(
                                m_document, tick, 60000000UL / static_cast<uint32_t>(bpm));
                            MeCommitUndo();
                        }
                        else if (lane.kind == MeAutoKind::Controller)
                        {
                            const unsigned char value =
                                static_cast<unsigned char>(std::clamp(norm * 127.0f, 0.0f, 127.0f));
                            BAERmfEditorDocument_AddTrackCCEvent(
                                m_document,
                                static_cast<uint16_t>(m_midi_selected_track),
                                lane.cc,
                                tick,
                                value);
                            MeCommitUndo();
                        }
                        else
                        {
                            const uint16_t value = static_cast<uint16_t>(
                                std::clamp(norm * 16383.0f, 0.0f, 16383.0f));
                            BAERmfEditorDocument_AddTrackPitchBendEvent(
                                m_document,
                                static_cast<uint16_t>(m_midi_selected_track),
                                tick,
                                value);
                            MeCommitUndo();
                        }
                    }
                }
                if (m_midi_drag_mode == MeDragMode::AutoNode && m_midi_auto_drag_lane == li &&
                    ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                {
                    const uint32_t tick = MeSnapTick(MeXToTick(io.MousePos.x, plot_x));
                    const float norm =
                        std::clamp(1.0f - (io.MousePos.y - plot_y) / plot_h, 0.0f, 1.0f);
                    if (lane.kind == MeAutoKind::Tempo)
                    {
                        const int bpm = std::clamp(static_cast<int>(norm * 240.0f), 20, 240);
                        BAERmfEditorDocument_SetTempoEvent(
                            m_document,
                            static_cast<uint32_t>(m_midi_auto_drag_index),
                            tick,
                            60000000UL / static_cast<uint32_t>(bpm));
                    }
                    else if (lane.kind == MeAutoKind::Controller)
                    {
                        const unsigned char value =
                            static_cast<unsigned char>(std::clamp(norm * 127.0f, 0.0f, 127.0f));
                        BAERmfEditorDocument_SetTrackCCEvent(
                            m_document,
                            static_cast<uint16_t>(m_midi_selected_track),
                            lane.cc,
                            static_cast<uint32_t>(m_midi_auto_drag_index),
                            tick,
                            value);
                    }
                    else
                    {
                        const uint16_t value =
                            static_cast<uint16_t>(std::clamp(norm * 16383.0f, 0.0f, 16383.0f));
                        BAERmfEditorDocument_SetTrackPitchBendEvent(
                            m_document,
                            static_cast<uint16_t>(m_midi_selected_track),
                            static_cast<uint32_t>(m_midi_auto_drag_index),
                            tick,
                            value);
                    }
                }
                if (m_midi_drag_mode == MeDragMode::AutoNode && m_midi_auto_drag_lane == li &&
                    ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                {
                    m_midi_drag_mode = MeDragMode::None;
                    MeCommitUndo();
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                {
                    float hit_node = -1.0f;
                    int hit_index = -1;
                    for (const Node &node : nodes)
                    {
                        const float nx = MeTickToX(node.tick, plot_x);
                        const float ny = value_to_y(node.value);
                        const float d = std::hypot(io.MousePos.x - nx, io.MousePos.y - ny);
                        if (d < 7.0f && (hit_node < 0.0f || d < hit_node))
                        {
                            hit_node = d;
                            hit_index = static_cast<int>(node.index);
                        }
                    }
                    if (hit_index >= 0)
                    {
                        MeBeginUndo();
                        if (lane.kind == MeAutoKind::Tempo)
                        {
                            BAERmfEditorDocument_DeleteTempoEvent(
                                m_document, static_cast<uint32_t>(hit_index));
                        }
                        else if (lane.kind == MeAutoKind::Controller)
                        {
                            BAERmfEditorDocument_DeleteTrackCCEvent(
                                m_document,
                                static_cast<uint16_t>(m_midi_selected_track),
                                lane.cc,
                                static_cast<uint32_t>(hit_index));
                        }
                        else
                        {
                            BAERmfEditorDocument_DeleteTrackPitchBendEvent(
                                m_document,
                                static_cast<uint16_t>(m_midi_selected_track),
                                static_cast<uint32_t>(hit_index));
                        }
                        MeCommitUndo();
                    }
                }
                ImGui::PopID();
                ay += auto_lane_h;
                ++drawn;
            }
            if (drawn == 0)
            {
                ImGui::TextDisabled("No automation lanes visible (enable in toolbar).");
            }
            else
            {
                /* Reserve vertical space so the scroll region covers all lanes. */
                ImGui::Dummy(ImVec2(1.0f, static_cast<float>(drawn) * auto_lane_h));
            }
            (void)length_ticks;
            ImGui::EndChild(); /* ##me_auto_scroll */
        }
        ImGui::EndChild();
    }

    void MeDrawToolbar()
    {
        if (ImGui::Button("Add Track"))
        {
            MeAddTrack();
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Track"))
        {
            MeDeleteSelectedTrack();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Snap", &m_midi_snap_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::Combo("##snapdiv",
                     &m_midi_snap_division_index,
                     "1/1\0" "1/2\0" "1/4\0" "1/8\0" "1/16\0");
        static const int kDivs[] = {1, 2, 4, 8, 16};
        m_midi_snap_division =
            kDivs[std::clamp(m_midi_snap_division_index, 0, 4)];
        ImGui::SameLine();
        ImGui::Checkbox("Follow", &m_midi_follow_playhead);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        ImGui::SliderFloat("Zoom", &m_midi_px_per_tick, 0.02f, 1.5f, "%.3f");
        ImGui::SameLine();
        if (ImGui::Button("Undo"))
        {
            MeUndo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Redo"))
        {
            MeRedo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy Notes"))
        {
            MeCopySelectedNotes();
        }
        ImGui::SameLine();
        if (ImGui::Button("Paste"))
        {
            MePasteNotesAtPlayhead();
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Notes"))
        {
            MeDeleteSelectedNotes();
        }

        if (m_document && m_midi_selected_track >= 0)
        {
            BAERmfEditorTrackInfo info{};
            if (BAERmfEditorDocument_GetTrackInfo(
                    m_document, static_cast<uint16_t>(m_midi_selected_track), &info) == BAE_NO_ERROR)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                if (m_midi_rename_track != m_midi_selected_track)
                {
                    m_midi_rename_track = m_midi_selected_track;
                    std::snprintf(m_midi_rename_buf,
                                  sizeof(m_midi_rename_buf),
                                  "%s",
                                  (info.name && info.name[0]) ? info.name : "Track");
                }
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::InputText("Name", m_midi_rename_buf, sizeof(m_midi_rename_buf)))
                {
                    /* live buffer only */
                }
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    MeBeginUndo();
                    info.name = m_midi_rename_buf;
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
                ImGui::SameLine();
                int bank = info.bank;
                int program = info.program;
                int channel = info.channel;
                int vol = info.volume;
                int pan = info.pan;
                ImGui::SetNextItemWidth(50.0f);
                if (ImGui::InputInt("Ch", &channel))
                {
                    MeBeginUndo();
                    info.channel = static_cast<unsigned char>(std::clamp(channel, 0, 15));
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::InputInt("Bank", &bank))
                {
                    MeBeginUndo();
                    info.bank = static_cast<uint16_t>(std::max(0, bank));
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::InputInt("Prog", &program))
                {
                    MeBeginUndo();
                    info.program = static_cast<unsigned char>(std::clamp(program, 0, 127));
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::InputInt("Vol", &vol))
                {
                    MeBeginUndo();
                    info.volume = static_cast<unsigned char>(std::clamp(vol, 0, 127));
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.0f);
                if (ImGui::InputInt("Pan", &pan))
                {
                    MeBeginUndo();
                    info.pan = static_cast<unsigned char>(std::clamp(pan, 0, 127));
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
            }
        }

        if (ImGui::TreeNode("Automation lanes"))
        {
            int auto_count = 0;
            const MeAutoLaneDesc *lanes = MeAutoLanes(&auto_count);
            for (int i = 0; i < auto_count; ++i)
            {
                bool vis = m_midi_auto_lane_visible[static_cast<size_t>(i)];
                if (ImGui::Checkbox(lanes[i].label, &vis))
                {
                    m_midi_auto_lane_visible[static_cast<size_t>(i)] = vis;
                }
            }
            ImGui::TreePop();
        }

        if (m_midi_primary_note >= 0 && m_document && m_midi_selected_track >= 0)
        {
            const auto &notes = MeNotes();
            if (m_midi_primary_note < static_cast<int>(notes.size()))
            {
                BAERmfEditorNoteInfo info = notes[static_cast<size_t>(m_midi_primary_note)];
                int vel = info.velocity;
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::SliderInt("Velocity", &vel, 1, 127))
                {
                    if (!m_midi_vel_undo_open)
                    {
                        MeBeginUndo();
                        m_midi_vel_undo_open = true;
                    }
                    info.velocity = static_cast<unsigned char>(vel);
                    BAERmfEditorDocument_SetNoteInfo(
                        m_document,
                        static_cast<uint16_t>(m_midi_selected_track),
                        static_cast<uint32_t>(m_midi_primary_note),
                        &info);
                    m_midi_note_cache_dirty = true;
                }
                if (m_midi_vel_undo_open && ImGui::IsItemDeactivatedAfterEdit())
                {
                    MeCommitUndo();
                    m_midi_vel_undo_open = false;
                }
            }
        }
    }

    void DrawMidiEditorWindow()
    {
        if (!m_midi_editor_open)
        {
            return;
        }
        bool open = m_midi_editor_open;
        ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("MIDI Editor", &open, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::End();
            m_midi_editor_open = open;
            return;
        }
        m_midi_editor_open = open;
        m_midi_editor_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        if (!m_document)
        {
            ImGui::TextDisabled("Load or activate a song to edit MIDI.");
            ImGui::End();
            return;
        }

        if (m_midi_selected_track < 0)
        {
            uint16_t track_count = 0;
            if (BAERmfEditorDocument_GetTrackCount(m_document, &track_count) == BAE_NO_ERROR &&
                track_count > 0)
            {
                m_midi_selected_track = 0;
                m_midi_note_cache_dirty = true;
            }
        }
        MeEnsureTrackMuteSize();

        /* Follow playhead (use time-grid width, not the left gutter). */
        if (m_midi_follow_playhead && m_song_started)
        {
            const uint32_t play = MePlayheadTick();
            const float view_ticks =
                std::max(1.0f, ImGui::GetContentRegionAvail().x - kMeTimeGutter) /
                std::max(0.001f, m_midi_px_per_tick);
            if (play < static_cast<uint32_t>(m_midi_scroll_tick) ||
                play > static_cast<uint32_t>(m_midi_scroll_tick + view_ticks * 0.85f))
            {
                m_midi_scroll_tick = std::max(0.0f, static_cast<float>(play) - view_ticks * 0.25f);
            }
        }
        m_midi_playhead_tick = MePlayheadTick();

        MeDrawToolbar();
        ImGui::Separator();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float timeline_h = std::clamp(avail.y * 0.22f, 120.0f, 200.0f);
        MeDrawTrackTimeline(avail.x, timeline_h);
        MeDrawPianoRoll(avail.x, avail.y - timeline_h - 4.0f);

        /* Shortcuts when focused */
        if (m_midi_editor_focused)
        {
            const ImGuiIO &io = ImGui::GetIO();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            {
                if (io.KeyShift)
                {
                    MeRedo();
                }
                else
                {
                    MeUndo();
                }
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
            {
                MeRedo();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
            {
                MeCopySelectedNotes();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false))
            {
                MeCopySelectedNotes();
                MeDeleteSelectedNotes();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
            {
                MePasteNotesAtPlayhead();
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
            {
                m_midi_selected_notes.clear();
                const auto &notes = MeNotes();
                for (int i = 0; i < static_cast<int>(notes.size()); ++i)
                {
                    m_midi_selected_notes.insert(i);
                }
                m_midi_primary_note = notes.empty() ? -1 : 0;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
            {
                MeDeleteSelectedNotes();
            }
        }

        ImGui::End();
    }
