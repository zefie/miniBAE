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
        TrackEot,
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

    uint32_t MeComputeFreeRunTick() const
    {
        if (!m_midi_record_free_run)
        {
            return m_midi_playhead_tick;
        }
        const uint32_t bpm = MeSongBpm();
        const uint16_t tpq = MeGetTpq();
        const uint32_t elapsed_ms = SDL_GetTicks() - m_midi_record_clock_start_ms;
        const uint64_t delta_ticks =
            (static_cast<uint64_t>(elapsed_ms) * static_cast<uint64_t>(bpm) *
             static_cast<uint64_t>(tpq)) /
            60000ull;
        const uint64_t tick =
            static_cast<uint64_t>(m_midi_record_clock_origin_tick) + delta_ticks;
        return tick > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(tick);
    }

    uint32_t MePlayheadTick() const
    {
        if (m_midi_record_free_run)
        {
            return MeComputeFreeRunTick();
        }
        if (!m_song || !m_song_started)
        {
            return m_midi_playhead_tick;
        }
        BAE_BOOL paused = FALSE;
        if (BAESong_IsPaused(m_song, &paused) == BAE_NO_ERROR && paused)
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

    uint32_t MeRecordCaptureTick() const
    {
        return MePlayheadTick();
    }

    void MeExtendTrackForRecord(uint32_t tick)
    {
        if (!m_document || m_midi_selected_track < 0)
        {
            return;
        }
        uint32_t eot = 0;
        (void)BAERmfEditorDocument_GetTrackEndOfTrackTick(
            m_document, static_cast<uint16_t>(m_midi_selected_track), &eot);
        const uint32_t need = tick + static_cast<uint32_t>(MeGetTpq()) * 4u; /* +1 bar pad */
        if (need > eot)
        {
            (void)BAERmfEditorDocument_SetTrackEndOfTrackTick(
                m_document, static_cast<uint16_t>(m_midi_selected_track), need);
            m_document_dirty = true;
        }
    }

    void MeStartRecordFreeRun()
    {
        m_midi_record_free_run = true;
        m_midi_record_clock_origin_tick = m_midi_playhead_tick;
        m_midi_record_clock_start_ms = SDL_GetTicks();
        m_midi_metro_last_song_beat = -1;
        m_midi_record_was_playing = false;
    }

    void MeStopRecordFreeRun()
    {
        if (m_midi_record_free_run)
        {
            m_midi_playhead_tick = MeComputeFreeRunTick();
        }
        m_midi_record_free_run = false;
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
        MeCancelCountIn();
        if (m_midi_record_take_open || m_midi_record_armed)
        {
            MeFinishRecordTake();
        }
        MeStopRecordFreeRun();
        m_midi_record_armed = false;
        m_midi_record_held_start.fill(-1);
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

    /* Shift-click: select inclusive index range from primary (or hit alone). */
    void MeSelectNoteRange(int index)
    {
        if (index < 0)
        {
            return;
        }
        if (m_midi_primary_note < 0)
        {
            MeSelectSingleNote(index);
            return;
        }
        const int a = std::min(m_midi_primary_note, index);
        const int b = std::max(m_midi_primary_note, index);
        m_midi_selected_notes.clear();
        for (int i = a; i <= b; ++i)
        {
            m_midi_selected_notes.insert(i);
        }
        m_midi_primary_note = index;
    }

    void MeQuantizeSelectedNotes()
    {
        if (!m_document || m_midi_selected_track < 0 || m_midi_selected_notes.empty())
        {
            return;
        }
        MeBeginUndo();
        const auto &notes = MeNotes();
        for (int idx : m_midi_selected_notes)
        {
            if (idx < 0 || idx >= static_cast<int>(notes.size()))
            {
                continue;
            }
            BAERmfEditorNoteInfo info = notes[static_cast<size_t>(idx)];
            const uint32_t end = info.startTick + std::max(1u, info.durationTicks);
            info.startTick = MeSnapTick(info.startTick);
            uint32_t new_end = MeSnapTick(end);
            if (new_end <= info.startTick)
            {
                new_end = info.startTick + MeSnapTicks();
            }
            info.durationTicks = new_end - info.startTick;
            BAERmfEditorDocument_SetNoteInfo(m_document,
                                             static_cast<uint16_t>(m_midi_selected_track),
                                             static_cast<uint32_t>(idx),
                                             &info);
        }
        m_midi_note_cache_dirty = true;
        MeCommitUndo();
        SetStatus("Quantized selection");
    }

    void MeApplySelectedNoteVelocity(unsigned char velocity)
    {
        if (!m_document || m_midi_selected_track < 0 || m_midi_selected_notes.empty())
        {
            return;
        }
        const auto &notes = MeNotes();
        for (int idx : m_midi_selected_notes)
        {
            if (idx < 0 || idx >= static_cast<int>(notes.size()))
            {
                continue;
            }
            BAERmfEditorNoteInfo info = notes[static_cast<size_t>(idx)];
            info.velocity = velocity;
            BAERmfEditorDocument_SetNoteInfo(m_document,
                                             static_cast<uint16_t>(m_midi_selected_track),
                                             static_cast<uint32_t>(idx),
                                             &info);
        }
        m_midi_note_cache_dirty = true;
    }

    void MeApplySelectedNoteBank(uint16_t bank)
    {
        if (!m_document || m_midi_selected_track < 0 || m_midi_selected_notes.empty())
        {
            return;
        }
        const auto &notes = MeNotes();
        for (int idx : m_midi_selected_notes)
        {
            if (idx < 0 || idx >= static_cast<int>(notes.size()))
            {
                continue;
            }
            BAERmfEditorNoteInfo info = notes[static_cast<size_t>(idx)];
            info.bank = bank;
            BAERmfEditorDocument_SetNoteInfo(m_document,
                                             static_cast<uint16_t>(m_midi_selected_track),
                                             static_cast<uint32_t>(idx),
                                             &info);
        }
        m_midi_note_cache_dirty = true;
    }

    void MeApplySelectedNoteProgram(unsigned char program)
    {
        if (!m_document || m_midi_selected_track < 0 || m_midi_selected_notes.empty())
        {
            return;
        }
        const auto &notes = MeNotes();
        for (int idx : m_midi_selected_notes)
        {
            if (idx < 0 || idx >= static_cast<int>(notes.size()))
            {
                continue;
            }
            BAERmfEditorNoteInfo info = notes[static_cast<size_t>(idx)];
            info.program = program;
            BAERmfEditorDocument_SetNoteInfo(m_document,
                                             static_cast<uint16_t>(m_midi_selected_track),
                                             static_cast<uint32_t>(idx),
                                             &info);
        }
        m_midi_note_cache_dirty = true;
    }

    void MeRecordFlushHeldNotes()
    {
        if (!m_document || m_midi_selected_track < 0)
        {
            m_midi_record_held_start.fill(-1);
            return;
        }
        uint32_t end_tick = MePlayheadTick();
        if (m_midi_snap_enabled)
        {
            end_tick = MeSnapTick(end_tick);
        }
        for (int n = 0; n < 128; ++n)
        {
            const int32_t start = m_midi_record_held_start[static_cast<size_t>(n)];
            if (start < 0)
            {
                continue;
            }
            uint32_t dur = (end_tick > static_cast<uint32_t>(start))
                               ? (end_tick - static_cast<uint32_t>(start))
                               : MeSnapTicks();
            if (dur < 1u)
            {
                dur = MeSnapTicks();
            }
            (void)BAERmfEditorDocument_AddNote(m_document,
                                               static_cast<uint16_t>(m_midi_selected_track),
                                               static_cast<uint32_t>(start),
                                               dur,
                                               static_cast<unsigned char>(n),
                                               m_midi_record_held_vel[static_cast<size_t>(n)]);
            m_midi_record_held_start[static_cast<size_t>(n)] = -1;
        }
        m_midi_note_cache_dirty = true;
    }

    void MeFinishRecordTake()
    {
        MeRecordFlushHeldNotes();
        MeStopRecordFreeRun();
        if (m_midi_record_take_open)
        {
            MeCommitUndo();
            m_midi_record_take_open = false;
            SetStatus("Recording take committed");
        }
    }

    /* Switch free-run capture onto the song clock at the current free-run tick. */
    void MeAttachRecordToSongClock()
    {
        if (!m_midi_record_free_run)
        {
            return;
        }
        const uint32_t tick = MeComputeFreeRunTick();
        MeStopRecordFreeRun();
        m_midi_playhead_tick = tick;
        if (m_song)
        {
            MeSeekToTick(tick);
        }
    }

    bool MeCanRecordCapture() const
    {
        return m_midi_record_armed && m_document && m_midi_selected_track >= 0 &&
               !m_midi_count_in_active &&
               (m_song_started || m_midi_record_free_run);
    }

    void MeEnsureRecordTakeOpen()
    {
        if (!m_midi_record_take_open)
        {
            MeBeginUndo();
            m_midi_record_take_open = true;
        }
        m_document_dirty = true;
    }

    void MeRecordNoteOn(int midi_note, unsigned char velocity)
    {
        /* Always monitor through audition. */
        NoteOn(midi_note, velocity);
        if (!MeCanRecordCapture())
        {
            return;
        }
        if (midi_note < 0 || midi_note > 127)
        {
            return;
        }
        MeEnsureRecordTakeOpen();
        uint32_t tick = MeRecordCaptureTick();
        if (m_midi_snap_enabled)
        {
            tick = MeSnapTick(tick);
        }
        m_midi_record_held_start[static_cast<size_t>(midi_note)] = static_cast<int32_t>(tick);
        m_midi_record_held_vel[static_cast<size_t>(midi_note)] = velocity ? velocity : 100;
        MeExtendTrackForRecord(tick);
    }

    void MeRecordNoteOff(int midi_note)
    {
        NoteOff(midi_note);
        if (midi_note < 0 || midi_note > 127)
        {
            return;
        }
        const int32_t start = m_midi_record_held_start[static_cast<size_t>(midi_note)];
        if (start < 0 || !m_document || m_midi_selected_track < 0)
        {
            m_midi_record_held_start[static_cast<size_t>(midi_note)] = -1;
            return;
        }
        uint32_t end_tick = MeRecordCaptureTick();
        if (m_midi_snap_enabled)
        {
            end_tick = MeSnapTick(end_tick);
        }
        uint32_t dur = (end_tick > static_cast<uint32_t>(start))
                           ? (end_tick - static_cast<uint32_t>(start))
                           : MeSnapTicks();
        if (dur < 1u)
        {
            dur = MeSnapTicks();
        }
        (void)BAERmfEditorDocument_AddNote(m_document,
                                           static_cast<uint16_t>(m_midi_selected_track),
                                           static_cast<uint32_t>(start),
                                           dur,
                                           static_cast<unsigned char>(midi_note),
                                           m_midi_record_held_vel[static_cast<size_t>(midi_note)]);
        m_midi_record_held_start[static_cast<size_t>(midi_note)] = -1;
        m_midi_note_cache_dirty = true;
        MeExtendTrackForRecord(static_cast<uint32_t>(start) + dur);
    }

    /* Live CC / pitch-bend into the selected track (+ monitor). */
    void MeRecordController(unsigned char status, unsigned char data1, unsigned char data2)
    {
        const unsigned char mtype = status & 0xF0u;
        if (mtype != 0xB0u && mtype != 0xE0u)
        {
            return;
        }

        unsigned char ch = 0;
        if (m_document && m_midi_selected_track >= 0)
        {
            BAERmfEditorTrackInfo ti{};
            if (BAERmfEditorDocument_GetTrackInfo(
                    m_document, static_cast<uint16_t>(m_midi_selected_track), &ti) == BAE_NO_ERROR)
            {
                ch = ti.channel;
            }
        }
        BAESong song = GetAuditionSong();
        if (song)
        {
            if (mtype == 0xB0u)
            {
                const unsigned char cc = data1 & 0x7fu;
                if (cc != 120 && cc != 123)
                {
                    BAESong_ControlChange(song, ch, cc, data2 & 0x7fu, 0);
                }
            }
            else
            {
                BAESong_PitchBend(song, ch, data1 & 0x7fu, data2 & 0x7fu, 0);
            }
        }

        if (mtype == 0xB0u && ((data1 & 0x7fu) == 120 || (data1 & 0x7fu) == 123))
        {
            PanicAuditionNotes();
            return;
        }

        if (!MeCanRecordCapture())
        {
            return;
        }

        MeEnsureRecordTakeOpen();
        const uint32_t tick = MeRecordCaptureTick(); /* raw ticks for continuous CC */
        const uint32_t min_gap = std::max(1u, static_cast<uint32_t>(MeGetTpq()) / 32u);

        if (mtype == 0xB0u)
        {
            const unsigned char cc = data1 & 0x7fu;
            const unsigned char val = data2 & 0x7fu;
            const int32_t last_t = m_midi_record_last_cc_tick[cc];
            const int last_v = m_midi_record_last_cc_val[cc];
            if (last_t >= 0)
            {
                const uint32_t dt = (tick >= static_cast<uint32_t>(last_t))
                                       ? (tick - static_cast<uint32_t>(last_t))
                                       : 0u;
                if (last_v == static_cast<int>(val) && dt < min_gap)
                {
                    return;
                }
                if (last_v != static_cast<int>(val) && dt < (min_gap / 4u))
                {
                    return; /* light throttle while sweeping */
                }
            }
            if (BAERmfEditorDocument_AddTrackCCEvent(m_document,
                                                     static_cast<uint16_t>(m_midi_selected_track),
                                                     cc,
                                                     tick,
                                                     val) == BAE_NO_ERROR)
            {
                m_midi_record_last_cc_tick[cc] = static_cast<int32_t>(tick);
                m_midi_record_last_cc_val[cc] = static_cast<int>(val);
                MeExtendTrackForRecord(tick);
            }
            return;
        }

        /* Pitch bend: 14-bit */
        const int pb = static_cast<int>(data1 & 0x7fu) | (static_cast<int>(data2 & 0x7fu) << 7);
        if (m_midi_record_last_pb_tick >= 0 &&
            static_cast<uint32_t>(m_midi_record_last_pb_tick) + min_gap > tick &&
            m_midi_record_last_pb_val == pb)
        {
            return;
        }
        if (BAERmfEditorDocument_AddTrackPitchBendEvent(
                m_document,
                static_cast<uint16_t>(m_midi_selected_track),
                tick,
                static_cast<uint16_t>(pb)) == BAE_NO_ERROR)
        {
            m_midi_record_last_pb_tick = static_cast<int32_t>(tick);
            m_midi_record_last_pb_val = pb;
            MeExtendTrackForRecord(tick);
        }
    }

    void MeSetRecordArmed(bool armed)
    {
        if (m_midi_record_armed == armed)
        {
            return;
        }
        if (m_midi_record_armed && !armed)
        {
            MeFinishRecordTake();
            MeCancelCountIn();
        }
        m_midi_record_armed = armed;
        m_midi_record_was_playing = m_song_started;
        m_midi_record_last_cc_tick.fill(-1);
        m_midi_record_last_cc_val.fill(-1);
        m_midi_record_last_pb_tick = -1;
        m_midi_record_last_pb_val = -1;
        if (armed)
        {
            if (m_song_started)
            {
                MeStopRecordFreeRun();
                SetStatus("Recording armed — synced to Play");
            }
            else if (m_midi_count_in_bars > 0)
            {
                MeStartCountIn();
            }
            else
            {
                MeStartRecordFreeRun();
                SetStatus("Recording — free-run clock (Play optional for monitor)");
            }
        }
    }

    void MePumpRecordTransport()
    {
        if (!m_midi_record_armed)
        {
            return;
        }
        /* Keep UI playhead + track length moving during free-run. */
        if (m_midi_record_free_run && !m_midi_count_in_active)
        {
            m_midi_playhead_tick = MeComputeFreeRunTick();
            MeExtendTrackForRecord(m_midi_playhead_tick);
            if (m_midi_metronome)
            {
                const uint16_t tpq = MeGetTpq();
                const int beat =
                    static_cast<int>(m_midi_playhead_tick / std::max<uint16_t>(1, tpq));
                if (beat != m_midi_metro_last_song_beat)
                {
                    MePlayMetronomeClick((beat % 4) == 0);
                    m_midi_metro_last_song_beat = beat;
                }
            }
        }
        /* Ending Play (song sync) commits the take, then keeps capturing on free-run. */
        if (m_midi_record_was_playing && !m_song_started && !m_midi_count_in_active &&
            !m_midi_record_free_run)
        {
            MeFinishRecordTake();
            if (m_midi_record_armed)
            {
                MeStartRecordFreeRun();
                SetStatus("Recording — free-run clock (Play optional for monitor)");
            }
        }
        m_midi_record_was_playing = m_song_started;
    }

    static void MeFillClickPcm(std::vector<int16_t> &out, double freq_hz, bool accent)
    {
        constexpr int kRate = 22050;
        constexpr int kFrames = 220; /* ~10 ms */
        out.assign(static_cast<size_t>(kFrames), 0);
        const double amp = accent ? 0.55 : 0.32;
        for (int i = 0; i < kFrames; ++i)
        {
            const double t = static_cast<double>(i) / static_cast<double>(kRate);
            const double env = 1.0 - static_cast<double>(i) / static_cast<double>(kFrames);
            const double s = std::sin(2.0 * 3.14159265358979323846 * freq_hz * t) * amp * env * env;
            out[static_cast<size_t>(i)] =
                static_cast<int16_t>(std::clamp(s * 32767.0, -32767.0, 32767.0));
        }
    }

    void MeEnsureMetronomeSounds()
    {
        if (!m_mixer)
        {
            return;
        }
        if (!m_midi_click_hi)
        {
            m_midi_click_hi = BAESound_New(m_mixer);
            MeFillClickPcm(m_midi_click_hi_pcm, 1200.0, true);
            if (m_midi_click_hi && !m_midi_click_hi_pcm.empty())
            {
                (void)BAESound_LoadCustomSample(m_midi_click_hi,
                                                m_midi_click_hi_pcm.data(),
                                                static_cast<uint32_t>(m_midi_click_hi_pcm.size()),
                                                16,
                                                1,
                                                LONG_TO_UNSIGNED_FIXED(22050),
                                                0,
                                                0);
            }
        }
        if (!m_midi_click_lo)
        {
            m_midi_click_lo = BAESound_New(m_mixer);
            MeFillClickPcm(m_midi_click_lo_pcm, 800.0, false);
            if (m_midi_click_lo && !m_midi_click_lo_pcm.empty())
            {
                (void)BAESound_LoadCustomSample(m_midi_click_lo,
                                                m_midi_click_lo_pcm.data(),
                                                static_cast<uint32_t>(m_midi_click_lo_pcm.size()),
                                                16,
                                                1,
                                                LONG_TO_UNSIGNED_FIXED(22050),
                                                0,
                                                0);
            }
        }
    }

    void MePlayMetronomeClick(bool accent)
    {
        if (!m_midi_metronome)
        {
            return;
        }
        MeEnsureMetronomeSounds();
        BAESound snd = accent ? m_midi_click_hi : m_midi_click_lo;
        if (!snd)
        {
            return;
        }
        (void)BAESound_Stop(snd, FALSE);
        (void)BAESound_Start(snd, 0, FLOAT_TO_UNSIGNED_FIXED(accent ? 0.9 : 0.65), 0);
    }

    uint32_t MeSongBpm() const
    {
        uint32_t bpm = 120;
        if (m_document)
        {
            BAERmfEditorDocument_GetTempoBPM(m_document, &bpm);
        }
        if (bpm == 0)
        {
            bpm = 120;
        }
        return bpm;
    }

    void MeCancelCountIn()
    {
        m_midi_count_in_active = false;
        m_midi_count_in_last_beat = -1;
    }

    void MeStartCountIn()
    {
        m_midi_count_in_active = true;
        m_midi_count_in_start_ms = SDL_GetTicks();
        m_midi_count_in_last_beat = -1;
        MeEnsureMetronomeSounds();
        char buf[96];
        std::snprintf(buf,
                      sizeof(buf),
                      "Count-in: %d bar%s…",
                      m_midi_count_in_bars,
                      m_midi_count_in_bars == 1 ? "" : "s");
        SetStatus(buf);
    }

    /* Start transport after count-in (no re-entry into count-in). */
    void MeStartPlaybackForRecord()
    {
        if (!m_song && m_document)
        {
            if (!ReloadPlaybackFromDocument(false))
            {
                return;
            }
        }
        if (!m_song)
        {
            SetStatus("No song to play");
            return;
        }
        BAESong_SetLoops(m_song, m_loop_enabled ? 32767 : 0);
        BAESong_Preroll(m_song);
        BAESong_Start(m_song, 1);
        m_song_started = true;
        m_midi_metro_last_song_beat = -1;
        m_midi_record_was_playing = true;
    }

    void MePumpCountInAndMetronome()
    {
        const uint32_t bpm = MeSongBpm();
        const uint32_t ms_per_beat = std::max(1u, 60000u / bpm);

        if (m_midi_count_in_active)
        {
            const int total_beats = std::max(1, m_midi_count_in_bars) * 4;
            const uint32_t elapsed = SDL_GetTicks() - m_midi_count_in_start_ms;
            const int beat = static_cast<int>(elapsed / ms_per_beat);
            if (beat != m_midi_count_in_last_beat && beat < total_beats)
            {
                MePlayMetronomeClick((beat % 4) == 0);
                m_midi_count_in_last_beat = beat;
            }
            if (beat >= total_beats)
            {
                m_midi_count_in_active = false;
                m_midi_count_in_last_beat = -1;
                MeStartRecordFreeRun();
                SetStatus("Recording — free-run clock (Play optional for monitor)");
            }
            return;
        }

        /* Song metronome (free-run metronome is handled in MePumpRecordTransport). */
        if (!m_midi_metronome || !m_song_started || !m_song || m_midi_record_free_run)
        {
            return;
        }
        BAE_BOOL paused = FALSE;
        if (BAESong_IsPaused(m_song, &paused) == BAE_NO_ERROR && paused)
        {
            return;
        }
        const uint32_t tick = MePlayheadTick();
        const uint16_t tpq = MeGetTpq();
        const int beat = static_cast<int>(tick / std::max<uint16_t>(1, tpq));
        if (beat != m_midi_metro_last_song_beat)
        {
            MePlayMetronomeClick((beat % 4) == 0);
            m_midi_metro_last_song_beat = beat;
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

    uint32_t MeCountNotesAffectedByEot(uint16_t track, uint32_t boundary_tick) const
    {
        if (!m_document)
        {
            return 0;
        }
        uint32_t note_count = 0;
        if (BAERmfEditorDocument_GetNoteCount(m_document, track, &note_count) != BAE_NO_ERROR)
        {
            return 0;
        }
        uint32_t affected = 0;
        for (uint32_t n = 0; n < note_count; ++n)
        {
            BAERmfEditorNoteInfo info{};
            if (BAERmfEditorDocument_GetNoteInfo(m_document, track, n, &info) != BAE_NO_ERROR)
            {
                continue;
            }
            if (info.startTick >= boundary_tick ||
                info.startTick + info.durationTicks > boundary_tick)
            {
                ++affected;
            }
        }
        return affected;
    }

    void MeApplySetEndOfTrack(uint16_t track, uint32_t boundary_tick)
    {
        if (!m_document)
        {
            return;
        }
        MeBeginUndo();

        uint32_t note_count = 0;
        (void)BAERmfEditorDocument_GetNoteCount(m_document, track, &note_count);
        for (int32_t n = static_cast<int32_t>(note_count) - 1; n >= 0; --n)
        {
            BAERmfEditorNoteInfo info{};
            if (BAERmfEditorDocument_GetNoteInfo(
                    m_document, track, static_cast<uint32_t>(n), &info) != BAE_NO_ERROR)
            {
                continue;
            }
            if (info.startTick >= boundary_tick)
            {
                (void)BAERmfEditorDocument_DeleteNote(m_document, track, static_cast<uint32_t>(n));
                continue;
            }
            if (info.startTick + info.durationTicks > boundary_tick)
            {
                const uint32_t new_dur = boundary_tick - info.startTick;
                if (new_dur < 1u)
                {
                    (void)BAERmfEditorDocument_DeleteNote(
                        m_document, track, static_cast<uint32_t>(n));
                }
                else
                {
                    info.durationTicks = new_dur;
                    (void)BAERmfEditorDocument_SetNoteInfo(
                        m_document, track, static_cast<uint32_t>(n), &info);
                }
            }
        }

        for (int cc = 0; cc < 128; ++cc)
        {
            uint32_t cc_count = 0;
            if (BAERmfEditorDocument_GetTrackCCEventCount(
                    m_document, track, static_cast<unsigned char>(cc), &cc_count) != BAE_NO_ERROR)
            {
                continue;
            }
            for (int32_t i = static_cast<int32_t>(cc_count) - 1; i >= 0; --i)
            {
                uint32_t tick = 0;
                unsigned char value = 0;
                if (BAERmfEditorDocument_GetTrackCCEvent(m_document,
                                                        track,
                                                        static_cast<unsigned char>(cc),
                                                        static_cast<uint32_t>(i),
                                                        &tick,
                                                        &value) != BAE_NO_ERROR)
                {
                    continue;
                }
                if (tick >= boundary_tick)
                {
                    (void)BAERmfEditorDocument_DeleteTrackCCEvent(
                        m_document, track, static_cast<unsigned char>(cc), static_cast<uint32_t>(i));
                }
            }
        }

        uint32_t pb_count = 0;
        if (BAERmfEditorDocument_GetTrackPitchBendEventCount(m_document, track, &pb_count) ==
            BAE_NO_ERROR)
        {
            for (int32_t i = static_cast<int32_t>(pb_count) - 1; i >= 0; --i)
            {
                uint32_t tick = 0;
                uint16_t value = 0;
                if (BAERmfEditorDocument_GetTrackPitchBendEvent(
                        m_document, track, static_cast<uint32_t>(i), &tick, &value) != BAE_NO_ERROR)
                {
                    continue;
                }
                if (tick >= boundary_tick)
                {
                    (void)BAERmfEditorDocument_DeleteTrackPitchBendEvent(
                        m_document, track, static_cast<uint32_t>(i));
                }
            }
        }

        (void)BAERmfEditorDocument_SetTrackEndOfTrackTick(m_document, track, boundary_tick);
        if (m_midi_selected_track == static_cast<int>(track))
        {
            m_midi_selected_notes.clear();
            m_midi_primary_note = -1;
            m_midi_note_cache_dirty = true;
        }
        MeCommitUndo();
        char buf[96];
        std::snprintf(buf, sizeof(buf), "End of track set to tick %u", boundary_tick);
        SetStatus(buf);
    }

    uint32_t MeCountNotesAffectedByEotSong(uint32_t boundary_tick) const
    {
        if (!m_document)
        {
            return 0;
        }
        uint16_t track_count = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &track_count) != BAE_NO_ERROR)
        {
            return 0;
        }
        uint32_t affected = 0;
        for (uint16_t t = 0; t < track_count; ++t)
        {
            affected += MeCountNotesAffectedByEot(t, boundary_tick);
        }
        return affected;
    }

    void MeApplySetEndOfSong(uint32_t boundary_tick)
    {
        if (!m_document)
        {
            return;
        }
        MeBeginUndo();
        (void)BAERmfEditorDocument_TrimToTick(m_document, boundary_tick);

        bool loop_on = false;
        uint32_t loop_start = 0;
        uint32_t loop_end = 0;
        int32_t loop_count = 0;
        (void)BAERmfEditorDocument_GetMidiLoopMarkers(
            m_document, &loop_on, &loop_start, &loop_end, &loop_count);
        if (!loop_on)
        {
            loop_start = 0;
            loop_count = 0;
        }
        if (boundary_tick == 0)
        {
            loop_start = 0;
            loop_end = 1;
        }
        else
        {
            if (loop_start >= boundary_tick)
            {
                loop_start = 0;
            }
            loop_end = boundary_tick;
            if (loop_end <= loop_start)
            {
                loop_end = loop_start + 1;
            }
        }
        (void)BAERmfEditorDocument_SetMidiLoopMarkers(
            m_document, true, loop_start, loop_end, loop_count);

        m_midi_selected_notes.clear();
        m_midi_primary_note = -1;
        m_midi_note_cache_dirty = true;
        MeCommitUndo();
        char buf[96];
        std::snprintf(buf, sizeof(buf), "End of song set to tick %u", boundary_tick);
        SetStatus(buf);
    }

    void MeRequestSetEndOfTrack(int track, uint32_t tick)
    {
        if (!m_document || track < 0)
        {
            return;
        }
        uint16_t track_count = 0;
        if (BAERmfEditorDocument_GetTrackCount(m_document, &track_count) != BAE_NO_ERROR ||
            track >= static_cast<int>(track_count))
        {
            return;
        }
        uint32_t boundary = tick;
        if (m_midi_snap_enabled)
        {
            boundary = MeSnapTick(boundary);
        }
        const uint32_t affected =
            MeCountNotesAffectedByEot(static_cast<uint16_t>(track), boundary);
        if (affected == 0)
        {
            MeApplySetEndOfTrack(static_cast<uint16_t>(track), boundary);
            return;
        }
        m_midi_set_eot_is_song = false;
        m_midi_set_eot_track = track;
        m_midi_set_eot_tick = boundary;
        m_midi_set_eot_note_count = affected;
        m_midi_set_eot_confirm_open = true;
    }

    void MeRequestSetEndOfSong(uint32_t tick)
    {
        if (!m_document)
        {
            return;
        }
        uint32_t boundary = tick;
        if (m_midi_snap_enabled)
        {
            boundary = MeSnapTick(boundary);
        }
        const uint32_t affected = MeCountNotesAffectedByEotSong(boundary);
        if (affected == 0)
        {
            MeApplySetEndOfSong(boundary);
            return;
        }
        m_midi_set_eot_is_song = true;
        m_midi_set_eot_track = -1;
        m_midi_set_eot_tick = boundary;
        m_midi_set_eot_note_count = affected;
        m_midi_set_eot_confirm_open = true;
    }

    void MeDrawSetEotConfirmDialog()
    {
        const char *popup_id =
            m_midi_set_eot_is_song ? "Trim Song to Here" : "Trim Track to Here";
        if (m_midi_set_eot_confirm_open)
        {
            ImGui::OpenPopup(popup_id);
        }
        if (!ImGui::BeginPopupModal(popup_id,
                                    &m_midi_set_eot_confirm_open,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        if (m_midi_set_eot_is_song)
        {
            ImGui::TextWrapped(
                "Trim song to tick %u?\n"
                "This will truncate or remove %u note%s across all tracks, "
                "trim events, and move the end-of-song marker.",
                m_midi_set_eot_tick,
                m_midi_set_eot_note_count,
                m_midi_set_eot_note_count == 1 ? "" : "s");
        }
        else
        {
            ImGui::TextWrapped(
                "Trim track to tick %u?\nThis will truncate or remove %u note%s on this track.",
                m_midi_set_eot_tick,
                m_midi_set_eot_note_count,
                m_midi_set_eot_note_count == 1 ? "" : "s");
        }
        ImGui::Spacing();
        ImGui::TextDisabled("CC and pitch-bend events at or after this tick are also removed.");
        ImGui::Separator();
        if (ImGui::Button("Truncate", ImVec2(120.0f, 0.0f)))
        {
            if (m_midi_set_eot_is_song)
            {
                MeApplySetEndOfSong(m_midi_set_eot_tick);
            }
            else if (m_midi_set_eot_track >= 0)
            {
                MeApplySetEndOfTrack(static_cast<uint16_t>(m_midi_set_eot_track),
                                     m_midi_set_eot_tick);
            }
            m_midi_set_eot_confirm_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            m_midi_set_eot_confirm_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* ---- Beatnik / NeoBAE (N)RPN insert & remove ----
     * NRPN select = CC99/98; RPN select = CC101/100. Data = CC6 (+ optional CC38). */

    enum class MeRpPreset : int
    {
        NrpnVelocityCurve = 0,   /* NRPN 4,0 */
        NrpnChannelInstMode = 1, /* NRPN 5,0 (Beatnik NRPN 640) */
        NrpnSampleOffset = 2,    /* NRPN 6,0 (NeoBAE ZMF, 21-bit) */
        NrpnCustom = 3,
        RpnPitchBendSens = 4, /* RPN 0,0 — supported */
        RpnFineTuning = 5,    /* RPN 0,1 — recognized (no engine effect yet) */
        RpnCoarseTuning = 6,  /* RPN 0,2 — recognized (no engine effect yet) */
        RpnCustom = 7,
    };

    struct MeRpCcHit
    {
        uint32_t index = 0;
        uint32_t tick = 0;
        unsigned char value = 0;
    };

    struct MeRpEntry
    {
        uint32_t tick = 0;
        bool is_rpn = false; /* false = NRPN (99/98), true = RPN (101/100) */
        unsigned char msb = 0;
        unsigned char lsb = 0;
        uint32_t data = 0; /* decoded CC6 data (21-bit for NRPN 6,0 sample offset) */
        int data_digits = 1;
        bool has_data_lsb = false;
        bool has_null = false;
        bool is_null_only = false;
        uint32_t idx_sel_msb = 0; /* CC99 or CC101 */
        uint32_t idx_sel_lsb = 0; /* CC98 or CC100 */
        uint32_t idx_cc6[3] = {0, 0, 0};
        uint32_t idx_cc38 = 0;
        uint32_t idx_null_msb = 0;
        uint32_t idx_null_lsb = 0;
    };

    static bool MeRpPresetIsRpn(MeRpPreset p)
    {
        return p >= MeRpPreset::RpnPitchBendSens;
    }

    static const char *MeRpPresetLabel(MeRpPreset p)
    {
        switch (p)
        {
        case MeRpPreset::NrpnVelocityCurve:
            return "NRPN: Velocity Curve (4,0)";
        case MeRpPreset::NrpnChannelInstMode:
            return "NRPN: Channel Instrument Mode (5,0)";
        case MeRpPreset::NrpnSampleOffset:
            return "NRPN: Sample Start Offset (6,0) [ZMF]";
        case MeRpPreset::NrpnCustom:
            return "NRPN: Custom";
        case MeRpPreset::RpnPitchBendSens:
            return "RPN: Pitch Bend Sensitivity (0,0)";
        case MeRpPreset::RpnFineTuning:
            return "RPN: Fine Tuning (0,1)";
        case MeRpPreset::RpnCoarseTuning:
            return "RPN: Coarse Tuning (0,2)";
        case MeRpPreset::RpnCustom:
            return "RPN: Custom";
        }
        return "(N)RPN";
    }

    static const char *MeRpDescribe(bool is_rpn,
                                    unsigned char msb,
                                    unsigned char lsb,
                                    uint32_t data,
                                    bool is_null)
    {
        if (is_null || (msb == 127 && lsb == 127))
        {
            return is_rpn ? "RPN Null" : "NRPN Null";
        }
        if (is_rpn)
        {
            if (msb == 0 && lsb == 0)
            {
                return "Pitch Bend Sensitivity";
            }
            if (msb == 0 && lsb == 1)
            {
                return "Fine Tuning (stub)";
            }
            if (msb == 0 && lsb == 2)
            {
                return "Coarse Tuning (stub)";
            }
            return "RPN";
        }
        if (msb == 4 && lsb == 0)
        {
            switch (data & 0x7Fu)
            {
            case 0:
                return "Velocity Curve: Default S";
            case 1:
                return "Velocity Curve: Peaky S";
            case 2:
                return "Velocity Curve: WebTV";
            case 3:
                return "Velocity Curve: 2× Exp";
            case 4:
                return "Velocity Curve: 2× Linear";
            case 5:
                return "Velocity Curve: Passthrough";
            default:
                return "Velocity Curve";
            }
        }
        if (msb == 5 && lsb == 0)
        {
            switch (data & 0x7Fu)
            {
            case 0:
                return "Inst Mode: GM Default";
            case 1:
                return "Inst Mode: Non-GM Perc Bank";
            case 2:
                return "Inst Mode: GM Perc Bank";
            case 3:
                return "Inst Mode: Melodic (Norm)";
            default:
                return "Channel Instrument Mode";
            }
        }
        if (msb == 6 && lsb == 0)
        {
            return "Sample Start Offset (ZMF)";
        }
        if (msb == 1 && lsb == 8)
        {
            return "GS Vibrato Rate (stub)";
        }
        if (msb == 1 && lsb == 9)
        {
            return "GS Vibrato Depth (stub)";
        }
        if (msb == 1 && lsb == 10)
        {
            return "GS Vibrato Delay (stub)";
        }
        if (msb == 1 && lsb == 32)
        {
            return "GS Cutoff Freq (stub)";
        }
        if (msb == 1 && lsb == 33)
        {
            return "GS Resonance (stub)";
        }
        return "NRPN";
    }

    void MeCollectTrackCc(uint16_t track, unsigned char cc, std::vector<MeRpCcHit> *out) const
    {
        out->clear();
        if (!m_document)
        {
            return;
        }
        uint32_t count = 0;
        if (BAERmfEditorDocument_GetTrackCCEventCount(m_document, track, cc, &count) != BAE_NO_ERROR ||
            count == 0)
        {
            return;
        }
        out->reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t tick = 0;
            unsigned char value = 0;
            if (BAERmfEditorDocument_GetTrackCCEvent(m_document, track, cc, i, &tick, &value) !=
                BAE_NO_ERROR)
            {
                continue;
            }
            MeRpCcHit hit;
            hit.index = i;
            hit.tick = tick;
            hit.value = value;
            out->push_back(hit);
        }
    }

    void MeScanTrackRpFamily(uint16_t track,
                             bool is_rpn,
                             std::vector<bool> *used6,
                             std::vector<bool> *used38,
                             const std::vector<MeRpCcHit> &cc6,
                             const std::vector<MeRpCcHit> &cc38,
                             std::vector<MeRpEntry> *out) const
    {
        const unsigned char sel_msb_cc = is_rpn ? 101 : 99;
        const unsigned char sel_lsb_cc = is_rpn ? 100 : 98;
        std::vector<MeRpCcHit> sel_msb;
        std::vector<MeRpCcHit> sel_lsb;
        MeCollectTrackCc(track, sel_msb_cc, &sel_msb);
        MeCollectTrackCc(track, sel_lsb_cc, &sel_lsb);

        std::vector<bool> used_msb(sel_msb.size(), false);
        std::vector<bool> used_lsb(sel_lsb.size(), false);

        auto find_unused_at = [](const std::vector<MeRpCcHit> &hits,
                                 const std::vector<bool> &used,
                                 uint32_t tick,
                                 int prefer_value = -1) -> int {
            int fallback = -1;
            for (size_t i = 0; i < hits.size(); ++i)
            {
                if (used[i] || hits[i].tick != tick)
                {
                    continue;
                }
                if (prefer_value >= 0 && hits[i].value == static_cast<unsigned char>(prefer_value))
                {
                    return static_cast<int>(i);
                }
                if (fallback < 0)
                {
                    fallback = static_cast<int>(i);
                }
            }
            return fallback;
        };

        for (size_t i_msb = 0; i_msb < sel_msb.size(); ++i_msb)
        {
            if (used_msb[i_msb])
            {
                continue;
            }
            const MeRpCcHit &msb_ev = sel_msb[i_msb];
            const int i_lsb = find_unused_at(sel_lsb, used_lsb, msb_ev.tick, -1);
            if (i_lsb < 0)
            {
                continue;
            }
            used_msb[i_msb] = true;
            used_lsb[static_cast<size_t>(i_lsb)] = true;

            MeRpEntry entry;
            entry.tick = msb_ev.tick;
            entry.is_rpn = is_rpn;
            entry.msb = msb_ev.value;
            entry.lsb = sel_lsb[static_cast<size_t>(i_lsb)].value;
            entry.idx_sel_msb = msb_ev.index;
            entry.idx_sel_lsb = sel_lsb[static_cast<size_t>(i_lsb)].index;

            if (entry.msb == 127 && entry.lsb == 127)
            {
                entry.is_null_only = true;
                out->push_back(entry);
                continue;
            }

            /* NRPN 6,0 uses three CC6 digits; everything else uses one (+ optional CC38). */
            const int want_digits = (!is_rpn && entry.msb == 6 && entry.lsb == 0) ? 3 : 1;
            entry.data_digits = 0;
            entry.data = 0;
            for (int d = 0; d < want_digits; ++d)
            {
                const int i6 = find_unused_at(cc6, *used6, entry.tick, -1);
                if (i6 < 0)
                {
                    break;
                }
                (*used6)[static_cast<size_t>(i6)] = true;
                entry.idx_cc6[d] = cc6[static_cast<size_t>(i6)].index;
                entry.data = (entry.data << 7) | (cc6[static_cast<size_t>(i6)].value & 0x7Fu);
                ++entry.data_digits;
            }

            if (want_digits == 1)
            {
                const int i38 = find_unused_at(cc38, *used38, entry.tick, -1);
                if (i38 >= 0)
                {
                    (*used38)[static_cast<size_t>(i38)] = true;
                    entry.has_data_lsb = true;
                    entry.idx_cc38 = cc38[static_cast<size_t>(i38)].index;
                    /* data stays CC6-only; CC38 is tracked via has_data_lsb/idx_cc38 */
                }
            }

            const int n_msb = find_unused_at(sel_msb, used_msb, entry.tick, 127);
            const int n_lsb = find_unused_at(sel_lsb, used_lsb, entry.tick, 127);
            if (n_msb >= 0 && n_lsb >= 0 &&
                sel_msb[static_cast<size_t>(n_msb)].value == 127 &&
                sel_lsb[static_cast<size_t>(n_lsb)].value == 127)
            {
                used_msb[static_cast<size_t>(n_msb)] = true;
                used_lsb[static_cast<size_t>(n_lsb)] = true;
                entry.has_null = true;
                entry.idx_null_msb = sel_msb[static_cast<size_t>(n_msb)].index;
                entry.idx_null_lsb = sel_lsb[static_cast<size_t>(n_lsb)].index;
            }

            out->push_back(entry);
        }
    }

    void MeScanTrackRps(uint16_t track, std::vector<MeRpEntry> *out) const
    {
        out->clear();
        std::vector<MeRpCcHit> cc6;
        std::vector<MeRpCcHit> cc38;
        MeCollectTrackCc(track, 6, &cc6);
        MeCollectTrackCc(track, 38, &cc38);
        std::vector<bool> used6(cc6.size(), false);
        std::vector<bool> used38(cc38.size(), false);

        /* Scan NRPNs first so they claim shared CC6/CC38 at a tick before RPNs. */
        MeScanTrackRpFamily(track, false, &used6, &used38, cc6, cc38, out);
        MeScanTrackRpFamily(track, true, &used6, &used38, cc6, cc38, out);
        std::sort(out->begin(),
                  out->end(),
                  [](const MeRpEntry &a, const MeRpEntry &b) {
                      if (a.tick != b.tick)
                      {
                          return a.tick < b.tick;
                      }
                      if (a.is_rpn != b.is_rpn)
                      {
                          return !a.is_rpn; /* NRPN before RPN at same tick */
                      }
                      return a.msb < b.msb || (a.msb == b.msb && a.lsb < b.lsb);
                  });
    }

    void MeDeleteRpEntry(uint16_t track, const MeRpEntry &entry)
    {
        if (!m_document)
        {
            return;
        }
        const unsigned char sel_msb_cc = entry.is_rpn ? 101 : 99;
        const unsigned char sel_lsb_cc = entry.is_rpn ? 100 : 98;
        /* Delete high indices first within each CC number. */
        struct Del
        {
            unsigned char cc;
            uint32_t index;
        };
        std::vector<Del> dels;
        dels.push_back({sel_msb_cc, entry.idx_sel_msb});
        dels.push_back({sel_lsb_cc, entry.idx_sel_lsb});
        for (int d = 0; d < entry.data_digits; ++d)
        {
            dels.push_back({6, entry.idx_cc6[d]});
        }
        if (entry.has_data_lsb)
        {
            dels.push_back({38, entry.idx_cc38});
        }
        if (entry.has_null)
        {
            dels.push_back({sel_msb_cc, entry.idx_null_msb});
            dels.push_back({sel_lsb_cc, entry.idx_null_lsb});
        }
        std::sort(dels.begin(),
                  dels.end(),
                  [](const Del &a, const Del &b) {
                      if (a.cc != b.cc)
                      {
                          return a.cc > b.cc;
                      }
                      return a.index > b.index;
                  });
        for (const Del &d : dels)
        {
            (void)BAERmfEditorDocument_DeleteTrackCCEvent(m_document, track, d.cc, d.index);
        }
    }

    bool MeInsertRpSequence(uint16_t track,
                            uint32_t tick,
                            bool is_rpn,
                            unsigned char msb,
                            unsigned char lsb,
                            const unsigned char *data_digits,
                            int data_count,
                            bool write_data_lsb,
                            unsigned char data_lsb_value,
                            bool append_null)
    {
        if (!m_document || !data_digits || data_count <= 0)
        {
            return false;
        }
        const unsigned char sel_msb_cc = is_rpn ? 101 : 99;
        const unsigned char sel_lsb_cc = is_rpn ? 100 : 98;
        if (BAERmfEditorDocument_AddTrackCCEvent(m_document, track, sel_msb_cc, tick, msb) !=
                BAE_NO_ERROR ||
            BAERmfEditorDocument_AddTrackCCEvent(m_document, track, sel_lsb_cc, tick, lsb) !=
                BAE_NO_ERROR)
        {
            return false;
        }
        for (int i = 0; i < data_count; ++i)
        {
            if (BAERmfEditorDocument_AddTrackCCEvent(
                    m_document, track, 6, tick, data_digits[i] & 0x7Fu) != BAE_NO_ERROR)
            {
                return false;
            }
        }
        if (write_data_lsb)
        {
            if (BAERmfEditorDocument_AddTrackCCEvent(
                    m_document, track, 38, tick, data_lsb_value & 0x7Fu) != BAE_NO_ERROR)
            {
                return false;
            }
        }
        if (append_null)
        {
            if (BAERmfEditorDocument_AddTrackCCEvent(
                    m_document, track, sel_msb_cc, tick, 127) != BAE_NO_ERROR ||
                BAERmfEditorDocument_AddTrackCCEvent(
                    m_document, track, sel_lsb_cc, tick, 127) != BAE_NO_ERROR)
            {
                return false;
            }
        }
        return true;
    }

    void OpenNrpnManagerDialog(uint32_t tick_hint, bool use_tick_hint)
    {
        if (!m_document || m_midi_selected_track < 0)
        {
            SetStatus("Select a track to manage (N)RPNs");
            return;
        }
        m_midi_nrpn_track = m_midi_selected_track;
        if (use_tick_hint)
        {
            m_midi_nrpn_tick = static_cast<int>(tick_hint);
        }
        else
        {
            m_midi_nrpn_tick = static_cast<int>(MePlayheadTick());
        }
        if (m_midi_snap_enabled)
        {
            m_midi_nrpn_tick = static_cast<int>(MeSnapTick(static_cast<uint32_t>(std::max(0, m_midi_nrpn_tick))));
        }
        m_midi_nrpn_preset = static_cast<int>(MeRpPreset::NrpnChannelInstMode);
        m_midi_nrpn_curve = 0;
        m_midi_nrpn_mode = 3; /* melodic — common Beatnik use */
        m_midi_nrpn_sample_offset = 0;
        m_midi_nrpn_custom_msb = 5;
        m_midi_nrpn_custom_lsb = 0;
        m_midi_nrpn_custom_data = 3;
        m_midi_rpn_pb_semitones = 2;
        m_midi_rpn_fine_14 = 8192; /* GM center */
        m_midi_rpn_coarse = 64;    /* GM center */
        m_midi_rpn_custom_msb = 0;
        m_midi_rpn_custom_lsb = 0;
        m_midi_rpn_custom_data = 2;
        m_midi_nrpn_append_null = true;
        m_midi_nrpn_open = true;
    }

    void MeDrawNrpnManagerDialog()
    {
        if (m_midi_nrpn_open)
        {
            ImGui::OpenPopup("(N)RPN Manager");
        }
        if (!ImGui::BeginPopupModal("(N)RPN Manager",
                                    &m_midi_nrpn_open,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        if (!m_document || m_midi_nrpn_track < 0)
        {
            ImGui::TextDisabled("No track selected.");
            if (ImGui::Button("Close"))
            {
                m_midi_nrpn_open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            return;
        }

        const uint16_t track = static_cast<uint16_t>(m_midi_nrpn_track);
        ImGui::Text("Track %d", m_midi_nrpn_track + 1);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Tick", &m_midi_nrpn_tick);
        if (m_midi_nrpn_tick < 0)
        {
            m_midi_nrpn_tick = 0;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Playhead"))
        {
            m_midi_nrpn_tick = static_cast<int>(MePlayheadTick());
        }
        if (m_midi_ctx_has_tick && m_midi_ctx_track == m_midi_nrpn_track)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Context"))
            {
                m_midi_nrpn_tick = static_cast<int>(m_midi_ctx_tick);
            }
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Insert");
        ImGui::SetNextItemWidth(340.0f);
        if (ImGui::BeginCombo("Preset", MeRpPresetLabel(static_cast<MeRpPreset>(m_midi_nrpn_preset))))
        {
            for (int p = 0; p <= static_cast<int>(MeRpPreset::RpnCustom); ++p)
            {
                const bool sel = (m_midi_nrpn_preset == p);
                if (ImGui::Selectable(MeRpPresetLabel(static_cast<MeRpPreset>(p)), sel))
                {
                    m_midi_nrpn_preset = p;
                }
                if (sel)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        const MeRpPreset preset = static_cast<MeRpPreset>(m_midi_nrpn_preset);
        const bool is_rpn = MeRpPresetIsRpn(preset);
        unsigned char msb = 0;
        unsigned char lsb = 0;
        unsigned char digits[3] = {0, 0, 0};
        int digit_count = 1;
        bool write_data_lsb = true;
        unsigned char data_lsb_value = 0;
        bool can_insert = true;

        if (preset == MeRpPreset::NrpnVelocityCurve)
        {
            msb = 4;
            lsb = 0;
            static const char *kCurves[] = {
                "0 — Default S",
                "1 — Peaky S",
                "2 — WebTV",
                "3 — 2× Exponential",
                "4 — 2× Linear",
                "5 — Passthrough",
            };
            m_midi_nrpn_curve = std::clamp(m_midi_nrpn_curve, 0, 5);
            ImGui::SetNextItemWidth(280.0f);
            ImGui::Combo("Curve", &m_midi_nrpn_curve, kCurves, IM_ARRAYSIZE(kCurves));
            digits[0] = static_cast<unsigned char>(m_midi_nrpn_curve);
            ImGui::TextDisabled("Song-wide velocity response (Beatnik NRPN 4,0).");
        }
        else if (preset == MeRpPreset::NrpnChannelInstMode)
        {
            msb = 5;
            lsb = 0;
            static const char *kModes[] = {
                "0 — GM Default",
                "1 — Non-GM Percussion Bank",
                "2 — GM Percussion Bank",
                "3 — Melodic / Normal Bank",
            };
            m_midi_nrpn_mode = std::clamp(m_midi_nrpn_mode, 0, 3);
            ImGui::SetNextItemWidth(280.0f);
            ImGui::Combo("Mode", &m_midi_nrpn_mode, kModes, IM_ARRAYSIZE(kModes));
            digits[0] = static_cast<unsigned char>(m_midi_nrpn_mode);
            ImGui::TextDisabled("Beatnik Channel Instrument Mode (NRPN 640 / 5,0).\n"
                                "Mode 3 makes MIDI ch10 melodic.");
        }
        else if (preset == MeRpPreset::NrpnSampleOffset)
        {
            msb = 6;
            lsb = 0;
            digit_count = 3;
            write_data_lsb = false;
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputInt("Offset (frames)", &m_midi_nrpn_sample_offset);
            m_midi_nrpn_sample_offset = std::clamp(m_midi_nrpn_sample_offset, 0, 0x1FFFFF);
            const uint32_t o = static_cast<uint32_t>(m_midi_nrpn_sample_offset) & 0x1FFFFFu;
            digits[0] = static_cast<unsigned char>((o >> 14) & 0x7Fu);
            digits[1] = static_cast<unsigned char>((o >> 7) & 0x7Fu);
            digits[2] = static_cast<unsigned char>(o & 0x7Fu);
            ImGui::TextDisabled("NeoBAE ZMF NRPN 6,0 — three CC6 digits.\n"
                                "0 clears channel NRPN offset. Requires ZMF song.");
        }
        else if (preset == MeRpPreset::NrpnCustom)
        {
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("MSB##nrpn", &m_midi_nrpn_custom_msb);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("LSB##nrpn", &m_midi_nrpn_custom_lsb);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Data##nrpn", &m_midi_nrpn_custom_data);
            m_midi_nrpn_custom_msb = std::clamp(m_midi_nrpn_custom_msb, 0, 127);
            m_midi_nrpn_custom_lsb = std::clamp(m_midi_nrpn_custom_lsb, 0, 127);
            m_midi_nrpn_custom_data = std::clamp(m_midi_nrpn_custom_data, 0, 127);
            msb = static_cast<unsigned char>(m_midi_nrpn_custom_msb);
            lsb = static_cast<unsigned char>(m_midi_nrpn_custom_lsb);
            digits[0] = static_cast<unsigned char>(m_midi_nrpn_custom_data);
            if (msb == 6 && lsb == 0)
            {
                ImGui::TextDisabled("For 21-bit sample offset, use the Sample Start Offset preset.");
            }
        }
        else if (preset == MeRpPreset::RpnPitchBendSens)
        {
            msb = 0;
            lsb = 0;
            m_midi_rpn_pb_semitones = std::clamp(m_midi_rpn_pb_semitones, 0, 24);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputInt("Semitones", &m_midi_rpn_pb_semitones);
            m_midi_rpn_pb_semitones = std::clamp(m_midi_rpn_pb_semitones, 0, 24);
            digits[0] = static_cast<unsigned char>(m_midi_rpn_pb_semitones);
            data_lsb_value = 0; /* cents = 0 */
            write_data_lsb = true;
            ImGui::TextDisabled("GM RPN 0,0 — pitch bend range in semitones (CC6).\n"
                                "CC38 cents set to 0. Engine uses MSB only.");
        }
        else if (preset == MeRpPreset::RpnFineTuning)
        {
            msb = 0;
            lsb = 1;
            m_midi_rpn_fine_14 = std::clamp(m_midi_rpn_fine_14, 0, 16383);
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputInt("Fine (14-bit)", &m_midi_rpn_fine_14);
            m_midi_rpn_fine_14 = std::clamp(m_midi_rpn_fine_14, 0, 16383);
            digits[0] = static_cast<unsigned char>((m_midi_rpn_fine_14 >> 7) & 0x7F);
            data_lsb_value = static_cast<unsigned char>(m_midi_rpn_fine_14 & 0x7F);
            write_data_lsb = true;
            ImGui::TextDisabled("GM RPN 0,1 — 8192 = center. Recognized; no engine effect yet.");
        }
        else if (preset == MeRpPreset::RpnCoarseTuning)
        {
            msb = 0;
            lsb = 2;
            m_midi_rpn_coarse = std::clamp(m_midi_rpn_coarse, 0, 127);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputInt("Coarse (64=center)", &m_midi_rpn_coarse);
            m_midi_rpn_coarse = std::clamp(m_midi_rpn_coarse, 0, 127);
            digits[0] = static_cast<unsigned char>(m_midi_rpn_coarse);
            write_data_lsb = true;
            data_lsb_value = 0;
            ImGui::TextDisabled("GM RPN 0,2 — 64 = concert pitch. Recognized; no engine effect yet.");
        }
        else /* RpnCustom */
        {
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("MSB##rpn", &m_midi_rpn_custom_msb);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("LSB##rpn", &m_midi_rpn_custom_lsb);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Data##rpn", &m_midi_rpn_custom_data);
            m_midi_rpn_custom_msb = std::clamp(m_midi_rpn_custom_msb, 0, 127);
            m_midi_rpn_custom_lsb = std::clamp(m_midi_rpn_custom_lsb, 0, 127);
            m_midi_rpn_custom_data = std::clamp(m_midi_rpn_custom_data, 0, 127);
            msb = static_cast<unsigned char>(m_midi_rpn_custom_msb);
            lsb = static_cast<unsigned char>(m_midi_rpn_custom_lsb);
            digits[0] = static_cast<unsigned char>(m_midi_rpn_custom_data);
            ImGui::TextDisabled("RPN select = CC101/CC100. Supported: 0,0 pitch bend.");
        }

        ImGui::Checkbox(is_rpn ? "Append RPN Null (127,127)" : "Append NRPN Null (127,127)",
                        &m_midi_nrpn_append_null);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Clears (N)RPN selection after data entry so later CC6\n"
                "does not accidentally change the parameter.\n"
                "Uses CC99/98 (NRPN) or CC101/100 (RPN).");
        }

        if (ImGui::Button("Insert", ImVec2(100, 0)) && can_insert)
        {
            uint32_t tick = static_cast<uint32_t>(std::max(0, m_midi_nrpn_tick));
            if (m_midi_snap_enabled)
            {
                tick = MeSnapTick(tick);
                m_midi_nrpn_tick = static_cast<int>(tick);
            }
            const bool do_lsb = write_data_lsb && digit_count == 1;
            MeBeginUndo();
            if (MeInsertRpSequence(track,
                                   tick,
                                   is_rpn,
                                   msb,
                                   lsb,
                                   digits,
                                   digit_count,
                                   do_lsb,
                                   data_lsb_value,
                                   m_midi_nrpn_append_null))
            {
                MeCommitUndo();
                char buf[160];
                std::snprintf(buf,
                              sizeof(buf),
                              "Inserted %s at tick %u",
                              MeRpDescribe(is_rpn, msb, lsb, digits[0], false),
                              tick);
                SetStatus(buf);
            }
            else
            {
                MeCancelUndo();
                SetStatus("(N)RPN insert failed");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Existing on this track");
        ImGui::BeginChild("##rp_list", ImVec2(560, 220), true);
        std::vector<MeRpEntry> entries;
        MeScanTrackRps(track, &entries);
        if (entries.empty())
        {
            ImGui::TextDisabled("No NRPN or RPN sequences found on this track.");
        }
        else
        {
            for (size_t i = 0; i < entries.size(); ++i)
            {
                const MeRpEntry &e = entries[i];
                ImGui::PushID(static_cast<int>(i));
                char label[224];
                const char *kind = e.is_rpn ? "RPN" : "NRPN";
                if (e.is_null_only)
                {
                    std::snprintf(label,
                                  sizeof(label),
                                  "tick %u  %s Null",
                                  e.tick,
                                  kind);
                }
                else
                {
                    /* Trailing (N)RPN Null select (127,127) — same as "Append … Null". */
                    const char *null_tag =
                        e.has_null ? (e.is_rpn ? " (+ RPN Null)" : " (+ NRPN Null)") : "";
                    if (!e.is_rpn && e.msb == 6 && e.lsb == 0)
                    {
                        std::snprintf(label,
                                      sizeof(label),
                                      "tick %u  %s %s = %u%s",
                                      e.tick,
                                      kind,
                                      MeRpDescribe(e.is_rpn, e.msb, e.lsb, e.data, false),
                                      e.data,
                                      null_tag);
                    }
                    else if (e.is_rpn && e.msb == 0 && e.lsb == 0)
                    {
                        const unsigned char semis = static_cast<unsigned char>(e.data & 0x7Fu);
                        /* Build plural in one buffer — avoid stacked %s ternaries
                         * (some CRTs print "(null)" for an empty %s edge case). */
                        char range[48];
                        std::snprintf(range,
                                      sizeof(range),
                                      "%u semitone%s",
                                      semis,
                                      (semis == 1) ? "" : "s");
                        std::snprintf(label,
                                      sizeof(label),
                                      "tick %u  %s %s = %s%s",
                                      e.tick,
                                      kind,
                                      MeRpDescribe(e.is_rpn, e.msb, e.lsb, e.data, false),
                                      range,
                                      null_tag);
                    }
                    else if (e.is_rpn && e.msb == 0 && e.lsb == 1 && e.has_data_lsb)
                    {
                        /* Fine tuning: reconstruct 14-bit from CC6 + CC38 at same tick. */
                        std::snprintf(label,
                                      sizeof(label),
                                      "tick %u  %s %s (14-bit via CC6/CC38)%s",
                                      e.tick,
                                      kind,
                                      MeRpDescribe(e.is_rpn, e.msb, e.lsb, e.data, false),
                                      null_tag);
                    }
                    else
                    {
                        std::snprintf(label,
                                      sizeof(label),
                                      "tick %u  %s %s (MSB=%u LSB=%u data=%u)%s",
                                      e.tick,
                                      kind,
                                      MeRpDescribe(e.is_rpn, e.msb, e.lsb, e.data, false),
                                      e.msb,
                                      e.lsb,
                                      e.data & 0x7Fu,
                                      null_tag);
                    }
                }
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                {
                    MeBeginUndo();
                    MeDeleteRpEntry(track, e);
                    MeCommitUndo();
                    SetStatus(e.is_rpn ? "Removed RPN sequence" : "Removed NRPN sequence");
                    ImGui::PopID();
                    break; /* list rebuilt next frame */
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Go"))
                {
                    m_midi_nrpn_tick = static_cast<int>(e.tick);
                    m_midi_scroll_tick = std::max(0.0f, static_cast<float>(e.tick) - 64.0f);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(100, 0)))
        {
            m_midi_nrpn_open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
        const float lane_h = std::clamp(m_midi_track_lane_h, 28.0f, 120.0f);
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

            /* Note overview: active region vs past end-of-track. */
            const float body_x = win.x + chrome_w;
            const float body_w = std::max(1.0f, content_w - chrome_w);
            const float body_y0 = y + 2.0f;
            const float body_y1 = y + lane_h - 2.0f;
            uint32_t eot_tick = 0;
            (void)BAERmfEditorDocument_GetTrackEndOfTrackTick(m_document, t, &eot_tick);
            const bool eot_dragging =
                (m_midi_drag_mode == MeDragMode::TrackEot && m_midi_eot_drag_track == static_cast<int>(t));
            const uint32_t draw_eot = eot_dragging ? m_midi_eot_drag_tick : eot_tick;
            const float eot_x = MeTickToX(draw_eot, body_x);
            const float body_x1 = body_x + body_w;
            /* Beyond EOT — darker void */
            dl->AddRectFilled(ImVec2(body_x, body_y0),
                              ImVec2(body_x1, body_y1),
                              IM_COL32(16, 17, 20, 255));
            /* In-track region */
            if (eot_x > body_x)
            {
                dl->AddRectFilled(ImVec2(body_x, body_y0),
                                  ImVec2(std::min(eot_x, body_x1), body_y1),
                                  selected ? IM_COL32(34, 40, 52, 255) : IM_COL32(28, 32, 40, 255));
            }
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
                if (x1 < body_x || x0 > body_x1)
                {
                    continue;
                }
                const float ny =
                    y + 4.0f + (1.0f - static_cast<float>(note.note) / 127.0f) * (lane_h - 12.0f);
                dl->AddRectFilled(ImVec2(std::max(x0, body_x), ny),
                                  ImVec2(std::min(x1, body_x1), ny + 3.0f),
                                  IM_COL32(90, 160, 230, 200));
            }
            /* End-of-track marker */
            if (eot_x >= body_x - 2.0f && eot_x <= body_x1 + 2.0f)
            {
                const ImU32 eot_col = eot_dragging ? IM_COL32(255, 220, 120, 255)
                                                  : (selected ? IM_COL32(255, 200, 90, 240)
                                                             : IM_COL32(200, 170, 80, 200));
                dl->AddLine(ImVec2(eot_x, body_y0), ImVec2(eot_x, body_y1), eot_col, 2.0f);
                dl->AddLine(ImVec2(eot_x - 4.0f, body_y0), ImVec2(eot_x + 4.0f, body_y0), eot_col, 2.0f);
                dl->AddLine(ImVec2(eot_x - 4.0f, body_y1), ImVec2(eot_x + 4.0f, body_y1), eot_col, 2.0f);
            }

            /* Click body to select track; EOT drag uses manual hit-test (no overlapping button). */
            ImGui::SetCursorScreenPos(ImVec2(body_x, y));
            ImGui::InvisibleButton("##lane_body", ImVec2(body_w, lane_h));
            const bool lane_hovered = ImGui::IsItemHovered();
            const ImVec2 mp = ImGui::GetIO().MousePos;
            constexpr float kEotHitPx = 6.0f;
            const bool near_eot =
                lane_hovered && std::fabs(mp.x - eot_x) <= kEotHitPx && eot_x >= body_x - 2.0f &&
                eot_x <= body_x1 + 2.0f;
            if (near_eot || eot_dragging)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (near_eot && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !eot_dragging)
            {
                ImGui::SetTooltip("Drag to set end of track");
            }
            if (near_eot && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                m_midi_drag_mode == MeDragMode::None)
            {
                m_midi_selected_track = static_cast<int>(t);
                m_midi_selected_notes.clear();
                m_midi_primary_note = -1;
                m_midi_note_cache_dirty = true;
                m_midi_drag_mode = MeDragMode::TrackEot;
                m_midi_eot_drag_track = static_cast<int>(t);
                m_midi_eot_drag_tick = eot_tick;
            }
            else if (lane_hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
                     m_midi_drag_mode == MeDragMode::None)
            {
                m_midi_selected_track = static_cast<int>(t);
                m_midi_selected_notes.clear();
                m_midi_primary_note = -1;
                m_midi_note_cache_dirty = true;
            }
            if (m_midi_drag_mode == MeDragMode::TrackEot &&
                m_midi_eot_drag_track == static_cast<int>(t))
            {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                {
                    uint32_t tick = MeXToTick(mp.x, body_x);
                    if (m_midi_snap_enabled)
                    {
                        tick = MeSnapTick(tick);
                    }
                    m_midi_eot_drag_tick = tick;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
                else
                {
                    const uint32_t commit_tick = m_midi_eot_drag_tick;
                    const int commit_track = m_midi_eot_drag_track;
                    m_midi_drag_mode = MeDragMode::None;
                    m_midi_eot_drag_track = -1;
                    MeRequestSetEndOfTrack(commit_track, commit_tick);
                }
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

        /* One timeline context menu: resolve track + tick from the right-click. */
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const float time_x = win.x + chrome_w;
            const float lanes_top = win.y + ruler_h + 4.0f;
            int hit_track = -1;
            if (mp.y >= lanes_top)
            {
                const float rel = mp.y - lanes_top;
                const int idx = static_cast<int>(rel / (lane_h + 2.0f));
                if (idx >= 0 && idx < static_cast<int>(track_count) &&
                    rel - static_cast<float>(idx) * (lane_h + 2.0f) <= lane_h)
                {
                    hit_track = idx;
                }
            }
            if (hit_track >= 0)
            {
                m_midi_selected_track = hit_track;
                m_midi_selected_notes.clear();
                m_midi_primary_note = -1;
                m_midi_note_cache_dirty = true;
                m_midi_ctx_track = hit_track;
            }
            else if (m_midi_selected_track >= 0)
            {
                m_midi_ctx_track = m_midi_selected_track;
            }
            else
            {
                m_midi_ctx_track = -1;
            }
            if (mp.x >= time_x)
            {
                m_midi_ctx_tick = MeXToTick(mp.x, time_x);
                m_midi_ctx_has_tick = true;
            }
            else
            {
                /* Chrome: trim at playhead if we have a track. */
                m_midi_ctx_tick = MePlayheadTick();
                m_midi_ctx_has_tick = (m_midi_ctx_track >= 0);
            }
            ImGui::OpenPopup("##me_track_ctx");
        }
        if (ImGui::BeginPopup("##me_track_ctx"))
        {
            if (ImGui::MenuItem("Add Track"))
            {
                MeAddTrack();
            }
            if (ImGui::MenuItem("Delete Track", nullptr, false, track_count > 1 && m_midi_selected_track >= 0))
            {
                MeDeleteSelectedTrack();
            }
            if (m_midi_ctx_has_tick && m_midi_ctx_track >= 0)
            {
                ImGui::Separator();
                char trim_track_label[80];
                char trim_song_label[80];
                uint32_t show_tick = m_midi_ctx_tick;
                if (m_midi_snap_enabled)
                {
                    show_tick = MeSnapTick(show_tick);
                }
                std::snprintf(trim_track_label,
                              sizeof(trim_track_label),
                              "Trim Track to Here (%u)",
                              show_tick);
                std::snprintf(trim_song_label,
                              sizeof(trim_song_label),
                              "Trim Song to Here (%u)",
                              show_tick);
                if (ImGui::MenuItem(trim_track_label))
                {
                    MeRequestSetEndOfTrack(m_midi_ctx_track, m_midi_ctx_tick);
                }
                if (ImGui::MenuItem(trim_song_label))
                {
                    MeRequestSetEndOfSong(m_midi_ctx_tick);
                }
                ImGui::Separator();
                char nrpn_label[80];
                std::snprintf(nrpn_label, sizeof(nrpn_label), "(N)RPN Manager (%u)...", show_tick);
                if (ImGui::MenuItem(nrpn_label, nullptr, false, m_midi_selected_track >= 0))
                {
                    if (m_midi_ctx_track >= 0)
                    {
                        m_midi_selected_track = m_midi_ctx_track;
                    }
                    OpenNrpnManagerDialog(show_tick, true);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                {
                    ImGui::SetTooltip(
                        "Insert or remove NRPNs and RPNs\n"
                        "(velocity curve, instrument mode, sample offset,\n"
                        "pitch bend sensitivity, …).");
                }
            }
            else if (m_midi_selected_track >= 0)
            {
                ImGui::Separator();
                if (ImGui::MenuItem("(N)RPN Manager..."))
                {
                    OpenNrpnManagerDialog(MePlayheadTick(), true);
                }
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

        /* Dim past this track's end-of-track; amber boundary line (live while dragging). */
        uint32_t track_eot = 0;
        float roll_eot_x = -1.0f;
        if (BAERmfEditorDocument_GetTrackEndOfTrackTick(
                m_document, static_cast<uint16_t>(m_midi_selected_track), &track_eot) == BAE_NO_ERROR)
        {
            const bool eot_dragging =
                (m_midi_drag_mode == MeDragMode::TrackEot &&
                 m_midi_eot_drag_track == m_midi_selected_track);
            const uint32_t draw_eot = eot_dragging ? m_midi_eot_drag_tick : track_eot;
            roll_eot_x = MeTickToX(draw_eot, grid_origin.x);
            const float grid_x1 = grid_origin.x + grid_size.x;
            const float grid_y1 = grid_origin.y + grid_size.y;
            if (roll_eot_x < grid_x1)
            {
                const float dim_x0 = std::max(roll_eot_x, grid_origin.x);
                if (dim_x0 < grid_x1)
                {
                    dl->AddRectFilled(ImVec2(dim_x0, grid_origin.y),
                                      ImVec2(grid_x1, grid_y1),
                                      IM_COL32(8, 8, 10, 140));
                }
            }
            if (roll_eot_x >= grid_origin.x && roll_eot_x <= grid_x1)
            {
                const ImU32 eot_col =
                    eot_dragging ? IM_COL32(255, 220, 120, 255) : IM_COL32(255, 200, 90, 220);
                dl->AddLine(ImVec2(roll_eot_x, grid_origin.y),
                            ImVec2(roll_eot_x, grid_y1),
                            eot_col,
                            2.0f);
                dl->AddLine(ImVec2(roll_eot_x - 5.0f, grid_origin.y),
                            ImVec2(roll_eot_x + 5.0f, grid_origin.y),
                            eot_col,
                            2.0f);
                dl->AddLine(ImVec2(roll_eot_x - 5.0f, grid_y1),
                            ImVec2(roll_eot_x + 5.0f, grid_y1),
                            eot_col,
                            2.0f);
            }
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
        const bool grid_left_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        const ImGuiIO &io = ImGui::GetIO();
        const bool eot_dragging =
            (m_midi_drag_mode == MeDragMode::TrackEot &&
             m_midi_eot_drag_track == m_midi_selected_track);
        constexpr float kEotHitPx = 6.0f;
        const bool near_roll_eot =
            hovered && roll_eot_x >= grid_origin.x &&
            roll_eot_x <= grid_origin.x + grid_size.x &&
            std::fabs(io.MousePos.x - roll_eot_x) <= kEotHitPx;
        if (near_roll_eot || eot_dragging)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (near_roll_eot && !eot_dragging && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip("Drag to set end of track");
        }
        if (near_roll_eot && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            m_midi_drag_mode == MeDragMode::None)
        {
            m_midi_drag_mode = MeDragMode::TrackEot;
            m_midi_eot_drag_track = m_midi_selected_track;
            m_midi_eot_drag_tick = track_eot;
        }
        if (eot_dragging)
        {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                uint32_t tick = MeXToTick(io.MousePos.x, grid_origin.x);
                if (m_midi_snap_enabled)
                {
                    tick = MeSnapTick(tick);
                }
                m_midi_eot_drag_tick = tick;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            else
            {
                const uint32_t commit_tick = m_midi_eot_drag_tick;
                const int commit_track = m_midi_eot_drag_track;
                m_midi_drag_mode = MeDragMode::None;
                m_midi_eot_drag_track = -1;
                MeRequestSetEndOfTrack(commit_track, commit_tick);
            }
        }

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

        if (grid_left_clicked && m_midi_drag_mode == MeDragMode::None && !near_roll_eot)
        {
            float edge = 0.0f;
            const int hit = MeHitTestNote(io.MousePos.x, io.MousePos.y, grid_origin.x, grid_origin.y, &edge);
            if (hit >= 0)
            {
                if (io.KeyShift && !io.KeyCtrl)
                {
                    MeSelectNoteRange(hit);
                }
                else if (io.KeyCtrl)
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
                if (!io.KeyCtrl && !io.KeyShift)
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
        /* Flow toolbar widgets across up to 3 lines based on window width. */
        const float region_max_x =
            ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
        const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
        const ImGuiStyle &style = ImGui::GetStyle();
        int wrap_line = 0;
        bool first_on_line = true;
        constexpr int kMaxWrapLines = 3;

        /* InputInt width includes +/- steppers; size for 4 digits so values aren't clipped. */
        const float int_field_w =
            ImGui::CalcTextSize("0000").x + style.FramePadding.x * 2.0f +
            ImGui::GetFrameHeight() * 2.0f + style.ItemInnerSpacing.x;
        auto btn_w = [&](const char *label) -> float {
            return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2.0f;
        };
        auto labeled_w = [&](const char *label, float field_w) -> float {
            return ImGui::CalcTextSize(label, nullptr, true).x + style.ItemInnerSpacing.x + field_w;
        };
        auto place = [&](float approx_w) {
            if (first_on_line)
            {
                first_on_line = false;
                return;
            }
            const float next_max = ImGui::GetItemRectMax().x + spacing_x + approx_w;
            if (wrap_line + 1 < kMaxWrapLines && next_max > region_max_x)
            {
                ++wrap_line;
                first_on_line = false;
                return;
            }
            ImGui::SameLine();
        };

        place(btn_w("Add Track"));
        if (ImGui::Button("Add Track"))
        {
            MeAddTrack();
        }
        place(btn_w("Delete Track"));
        if (ImGui::Button("Delete Track"))
        {
            MeDeleteSelectedTrack();
        }
        {
            const bool rec = m_midi_record_armed;
            const char *rec_label = rec ? "Rec" : "Record";
            place(btn_w(rec_label));
            if (rec)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.25f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.10f, 0.10f, 1.0f));
            }
            if (ImGui::Button(rec ? "Rec###merec" : "Record###merec"))
            {
                MeSetRecordArmed(!m_midi_record_armed);
            }
            if (rec)
            {
                ImGui::PopStyleColor(3);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            {
                ImGui::SetTooltip(
                    "Arm recording into the selected track from the playhead.\n"
                    "Play is optional: free-run clock when stopped, song-sync when playing.\n"
                    "Notes + CC + pitch bend (PC / HW / typing). Track length grows with the take.\n"
                    "Disarm to commit. Stop ends a song-synced take then keeps free-run armed.");
            }
        }
        place(labeled_w("Metronome", ImGui::GetFrameHeight()));
        ImGui::Checkbox("Metronome", &m_midi_metronome);
        place(labeled_w("Count-in", 70.0f));
        ImGui::SetNextItemWidth(70.0f);
        ImGui::Combo("Count-in", &m_midi_count_in_bars, "Off\0" "1 bar\0" "2 bars\0");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Bars of delay before capture when Record is armed while stopped.\n"
                "Clicks only play when Metronome is checked.\n"
                "(Ignored if Play is already running.)");
        }
        if (m_midi_count_in_active)
        {
            place(ImGui::CalcTextSize("Count-in…").x);
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Count-in…");
        }
        place(labeled_w("Snap", ImGui::GetFrameHeight()));
        ImGui::Checkbox("Snap", &m_midi_snap_enabled);
        place(70.0f);
        ImGui::SetNextItemWidth(70.0f);
        ImGui::Combo("##snapdiv",
                     &m_midi_snap_division_index,
                     "1/1\0" "1/2\0" "1/4\0" "1/8\0" "1/16\0");
        static const int kDivs[] = {1, 2, 4, 8, 16};
        m_midi_snap_division =
            kDivs[std::clamp(m_midi_snap_division_index, 0, 4)];
        place(labeled_w("Follow", ImGui::GetFrameHeight()));
        ImGui::Checkbox("Follow", &m_midi_follow_playhead);
        place(labeled_w("Zoom", 100.0f));
        ImGui::SetNextItemWidth(100.0f);
        ImGui::SliderFloat("Zoom", &m_midi_px_per_tick, 0.02f, 1.5f, "%.3f");
        place(btn_w("Undo"));
        if (ImGui::Button("Undo"))
        {
            MeUndo();
        }
        place(btn_w("Redo"));
        if (ImGui::Button("Redo"))
        {
            MeRedo();
        }
        place(btn_w("Copy Notes"));
        if (ImGui::Button("Copy Notes"))
        {
            MeCopySelectedNotes();
        }
        place(btn_w("Paste"));
        if (ImGui::Button("Paste"))
        {
            MePasteNotesAtPlayhead();
        }
        place(btn_w("Delete Notes"));
        if (ImGui::Button("Delete Notes"))
        {
            MeDeleteSelectedNotes();
        }
        place(btn_w("(N)RPN..."));
        ImGui::BeginDisabled(m_midi_selected_track < 0);
        if (ImGui::Button("(N)RPN..."))
        {
            OpenNrpnManagerDialog(MePlayheadTick(), true);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal |
                                 ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip(
                "Insert or remove NRPNs / RPNs on the selected track:\n"
                "NRPN: velocity curve, instrument mode, ZMF sample offset;\n"
                "RPN: pitch bend sensitivity (0,0), fine/coarse tuning.");
        }
        place(btn_w("Quantize"));
        ImGui::BeginDisabled(m_midi_selected_notes.empty());
        if (ImGui::Button("Quantize"))
        {
            MeQuantizeSelectedNotes();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip("Snap start/end of selected notes to the grid (Ctrl+Q).");
        }

        if (m_document && m_midi_selected_track >= 0)
        {
            BAERmfEditorTrackInfo info{};
            if (BAERmfEditorDocument_GetTrackInfo(
                    m_document, static_cast<uint16_t>(m_midi_selected_track), &info) == BAE_NO_ERROR)
            {
                place(ImGui::CalcTextSize("|").x);
                ImGui::TextDisabled("|");
                if (m_midi_rename_track != m_midi_selected_track)
                {
                    m_midi_rename_track = m_midi_selected_track;
                    std::snprintf(m_midi_rename_buf,
                                  sizeof(m_midi_rename_buf),
                                  "%s",
                                  (info.name && info.name[0]) ? info.name : "Track");
                }
                place(labeled_w("Name", 120.0f));
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
                int bank = info.bank;
                int program = info.program;
                int channel = info.channel;
                int vol = info.volume;
                int pan = info.pan;
                place(labeled_w("Ch", int_field_w));
                ImGui::SetNextItemWidth(int_field_w);
                if (ImGui::InputInt("Ch", &channel))
                {
                    MeBeginUndo();
                    info.channel = static_cast<unsigned char>(std::clamp(channel, 0, 15));
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
                place(labeled_w("Bank", int_field_w));
                ImGui::SetNextItemWidth(int_field_w);
                if (ImGui::InputInt("Bank", &bank))
                {
                    MeBeginUndo();
                    info.bank = static_cast<uint16_t>(std::max(0, bank));
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
                place(labeled_w("Prog", int_field_w));
                ImGui::SetNextItemWidth(int_field_w);
                if (ImGui::InputInt("Prog", &program))
                {
                    MeBeginUndo();
                    info.program = static_cast<unsigned char>(std::clamp(program, 0, 127));
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
                place(labeled_w("Vol", int_field_w));
                ImGui::SetNextItemWidth(int_field_w);
                if (ImGui::InputInt("Vol", &vol))
                {
                    MeBeginUndo();
                    info.volume = static_cast<unsigned char>(std::clamp(vol, 0, 127));
                    BAERmfEditorDocument_SetTrackInfo(
                        m_document, static_cast<uint16_t>(m_midi_selected_track), &info);
                    MeCommitUndo();
                }
                place(labeled_w("Pan", int_field_w));
                ImGui::SetNextItemWidth(int_field_w);
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

        if (!m_midi_selected_notes.empty() && m_document && m_midi_selected_track >= 0)
        {
            const auto &notes = MeNotes();
            bool have_vel = false;
            bool have_bank = false;
            bool have_prog = false;
            bool vel_mixed = false;
            bool bank_mixed = false;
            bool prog_mixed = false;
            int vel = 100;
            int bank = 0;
            int program = 0;
            for (int idx : m_midi_selected_notes)
            {
                if (idx < 0 || idx >= static_cast<int>(notes.size()))
                {
                    continue;
                }
                const BAERmfEditorNoteInfo &n = notes[static_cast<size_t>(idx)];
                if (!have_vel)
                {
                    vel = n.velocity;
                    have_vel = true;
                }
                else if (vel != static_cast<int>(n.velocity))
                {
                    vel_mixed = true;
                }
                if (!have_bank)
                {
                    bank = n.bank;
                    have_bank = true;
                }
                else if (bank != static_cast<int>(n.bank))
                {
                    bank_mixed = true;
                }
                if (!have_prog)
                {
                    program = n.program;
                    have_prog = true;
                }
                else if (program != static_cast<int>(n.program))
                {
                    prog_mixed = true;
                }
            }

            ImGui::Separator();
            ImGui::TextDisabled("Selection (%d note%s)",
                                static_cast<int>(m_midi_selected_notes.size()),
                                m_midi_selected_notes.size() == 1 ? "" : "s");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            if (vel_mixed)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.75f, 1.0f));
            }
            if (ImGui::SliderInt(vel_mixed ? "Velocity###selvel" : "Velocity###selvel", &vel, 1, 127))
            {
                if (!m_midi_vel_undo_open)
                {
                    MeBeginUndo();
                    m_midi_vel_undo_open = true;
                }
                MeApplySelectedNoteVelocity(static_cast<unsigned char>(vel));
                vel_mixed = false;
            }
            if (vel_mixed)
            {
                ImGui::PopStyleColor();
            }
            if (m_midi_vel_undo_open && ImGui::IsItemDeactivatedAfterEdit())
            {
                MeCommitUndo();
                m_midi_vel_undo_open = false;
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(int_field_w);
            if (bank_mixed)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.65f);
            }
            if (ImGui::InputInt(bank_mixed ? "Note Bank###selbank" : "Note Bank###selbank", &bank))
            {
                if (!m_midi_note_bank_undo_open)
                {
                    MeBeginUndo();
                    m_midi_note_bank_undo_open = true;
                }
                MeApplySelectedNoteBank(static_cast<uint16_t>(std::max(0, bank)));
            }
            if (bank_mixed)
            {
                ImGui::PopStyleVar();
            }
            if (m_midi_note_bank_undo_open && ImGui::IsItemDeactivatedAfterEdit())
            {
                MeCommitUndo();
                m_midi_note_bank_undo_open = false;
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            {
                ImGui::SetTooltip(
                    "Per-note Beatnik bank (CC0 / instID = bank×256+prog).\n"
                    "0..127 = bank group. Preview/export emit Bank Select when it changes.");
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(int_field_w);
            if (prog_mixed)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.65f);
            }
            if (ImGui::InputInt(prog_mixed ? "Note Prog###selprog" : "Note Prog###selprog", &program))
            {
                if (!m_midi_note_prog_undo_open)
                {
                    MeBeginUndo();
                    m_midi_note_prog_undo_open = true;
                }
                MeApplySelectedNoteProgram(
                    static_cast<unsigned char>(std::clamp(program, 0, 127)));
            }
            if (prog_mixed)
            {
                ImGui::PopStyleVar();
            }
            if (m_midi_note_prog_undo_open && ImGui::IsItemDeactivatedAfterEdit())
            {
                MeCommitUndo();
                m_midi_note_prog_undo_open = false;
            }
        }
    }

    void DrawMidiEditorWindow()
    {
        if (!m_midi_editor_open)
        {
            MeDrawSetEotConfirmDialog();
            MeDrawNrpnManagerDialog();
            return;
        }
        bool open = m_midi_editor_open;
        ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("MIDI Editor", &open, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::End();
            m_midi_editor_open = open;
            MeDrawSetEotConfirmDialog();
            MeDrawNrpnManagerDialog();
            return;
        }
        m_midi_editor_open = open;
        m_midi_editor_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        if (!m_document)
        {
            ImGui::TextDisabled("Load or activate a song to edit MIDI.");
            ImGui::End();
            MeDrawSetEotConfirmDialog();
            MeDrawNrpnManagerDialog();
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
        if (m_midi_follow_playhead && (m_song_started || m_midi_record_free_run))
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

        MePumpRecordTransport();
        MeDrawToolbar();
        ImGui::Separator();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        uint16_t track_count_for_h = 0;
        BAERmfEditorDocument_GetTrackCount(m_document, &track_count_for_h);
        const float lane_h = std::clamp(m_midi_track_lane_h, 28.0f, 120.0f);
        const float timeline_h = std::clamp(
            26.0f + static_cast<float>(std::max<uint16_t>(1, track_count_for_h)) * (lane_h + 2.0f) +
                8.0f,
            100.0f,
            std::max(100.0f, avail.y * 0.45f));
        const float split_h = 6.0f;
        MeDrawTrackTimeline(avail.x, timeline_h);

        ImGui::InvisibleButton("##me_lane_h_split", ImVec2(avail.x, split_h));
        {
            const bool split_hover = ImGui::IsItemHovered();
            const bool split_active = ImGui::IsItemActive();
            if (split_hover || split_active)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            }
            if (split_active)
            {
                m_midi_track_lane_h =
                    std::clamp(m_midi_track_lane_h + ImGui::GetIO().MouseDelta.y, 28.0f, 120.0f);
            }
            const ImVec2 sp0 = ImGui::GetItemRectMin();
            const ImVec2 sp1 = ImGui::GetItemRectMax();
            const float mid_y = (sp0.y + sp1.y) * 0.5f;
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(sp0.x + 8.0f, mid_y),
                ImVec2(sp1.x - 8.0f, mid_y),
                split_active  ? IM_COL32(160, 140, 220, 255)
                : split_hover ? IM_COL32(120, 110, 160, 220)
                              : IM_COL32(70, 74, 90, 200),
                2.0f);
        }

        MeDrawPianoRoll(avail.x, avail.y - timeline_h - split_h - 4.0f);

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
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q, false) &&
                !m_midi_selected_notes.empty())
            {
                MeQuantizeSelectedNotes();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !io.KeyCtrl && !io.WantTextInput)
            {
                MeSetRecordArmed(!m_midi_record_armed);
            }
        }

        ImGui::End();
        MeDrawSetEotConfirmDialog();
        MeDrawNrpnManagerDialog();
    }
