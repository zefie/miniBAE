/* Shared keyboard / hardware-MIDI dispatch + PC musical typing.
 * Included inside NbEditorApp (before midi_hw / player note paths). */

    enum class KeyboardInputTarget : uint8_t
    {
        None = 0,
        SampleEditor,
        InstrumentEditor,
        MidiEditorRecord, /* piano roll armed — §2 recording sink */
        MainPreview,
    };

    KeyboardInputTarget ResolveKeyboardTarget() const
    {
        /* Blocking modal: drop new notes (Note Off still uses per-note memory). */
        if (ImGui::GetTopMostPopupModal() != nullptr)
        {
            return KeyboardInputTarget::None;
        }
        if (m_sample_editor_open)
        {
            return KeyboardInputTarget::SampleEditor;
        }
        if (m_instrument_editor_open)
        {
            return KeyboardInputTarget::InstrumentEditor;
        }
        /* Recording arm comes later (§2); focused piano roll monitors via main for now. */
        if (m_midi_editor_open && m_midi_editor_focused)
        {
            /* if (m_midi_record_armed) return KeyboardInputTarget::MidiEditorRecord; */
            return KeyboardInputTarget::MainPreview;
        }
        return KeyboardInputTarget::MainPreview;
    }

    static int PcKeyboardScancodeToOffset(SDL_Scancode sc)
    {
        /* zefidi map: a w s e d f t g y h u j k o → C..C# (+octave). */
        switch (sc)
        {
        case SDL_SCANCODE_A: return 0;
        case SDL_SCANCODE_W: return 1;
        case SDL_SCANCODE_S: return 2;
        case SDL_SCANCODE_E: return 3;
        case SDL_SCANCODE_D: return 4;
        case SDL_SCANCODE_F: return 5;
        case SDL_SCANCODE_T: return 6;
        case SDL_SCANCODE_G: return 7;
        case SDL_SCANCODE_Y: return 8;
        case SDL_SCANCODE_H: return 9;
        case SDL_SCANCODE_U: return 10;
        case SDL_SCANCODE_J: return 11;
        case SDL_SCANCODE_K: return 12;
        case SDL_SCANCODE_O: return 13;
        default: return -1;
        }
    }

    bool PcKeyboardTypingAllowed() const
    {
        const ImGuiIO &io = ImGui::GetIO();
        if (io.WantTextInput)
        {
            return false;
        }
        if (io.KeyCtrl || io.KeyAlt || io.KeySuper)
        {
            return false;
        }
        return ResolveKeyboardTarget() != KeyboardInputTarget::None;
    }

    void DispatchKeyboardNoteOn(int midi_note, unsigned char velocity = 127)
    {
        if (midi_note < 0 || midi_note > 127)
        {
            return;
        }
        if (velocity == 0)
        {
            DispatchKeyboardNoteOff(midi_note);
            return;
        }

        const KeyboardInputTarget target = ResolveKeyboardTarget();
        m_keyboard_note_target[static_cast<size_t>(midi_note)] = target;

        switch (target)
        {
        case KeyboardInputTarget::None:
            m_keyboard_note_target[static_cast<size_t>(midi_note)] = KeyboardInputTarget::None;
            return;
        case KeyboardInputTarget::SampleEditor:
#if NBEDITOR_MVP
            StartSampleEditorNote(midi_note, velocity);
#endif
            return;
        case KeyboardInputTarget::MidiEditorRecord:
            /* §2: append to record buffer + optional monitor. */
            return;
        case KeyboardInputTarget::InstrumentEditor:
        case KeyboardInputTarget::MainPreview:
            NoteOn(midi_note, velocity);
            return;
        }
    }

    void DispatchKeyboardNoteOff(int midi_note)
    {
        if (midi_note < 0 || midi_note > 127)
        {
            return;
        }
        const KeyboardInputTarget target = m_keyboard_note_target[static_cast<size_t>(midi_note)];
        m_keyboard_note_target[static_cast<size_t>(midi_note)] = KeyboardInputTarget::None;

        switch (target)
        {
        case KeyboardInputTarget::None:
            NoteOff(midi_note);
#if NBEDITOR_MVP
            StopSampleEditorNote(midi_note);
#endif
            return;
        case KeyboardInputTarget::SampleEditor:
#if NBEDITOR_MVP
            StopSampleEditorNote(midi_note);
#endif
            return;
        case KeyboardInputTarget::MidiEditorRecord:
            return;
        case KeyboardInputTarget::InstrumentEditor:
        case KeyboardInputTarget::MainPreview:
            NoteOff(midi_note);
            return;
        }
    }

    void HandlePcKeyboardEventInternal(const SDL_Event &event)
    {
        if (event.type != SDL_EVENT_KEY_DOWN && event.type != SDL_EVENT_KEY_UP)
        {
            return;
        }
        if (event.key.repeat)
        {
            return;
        }

        const SDL_Scancode sc = event.key.scancode;
        const int sc_i = static_cast<int>(sc);
        if (sc_i < 0 || sc_i >= static_cast<int>(m_pc_key_note.size()))
        {
            return;
        }

        const bool is_down = (event.type == SDL_EVENT_KEY_DOWN);

        /* Always release tracked notes (avoids stuck notes when focus enters a text field). */
        if (!is_down)
        {
            const int held = m_pc_key_note[static_cast<size_t>(sc_i)];
            if (held >= 0)
            {
                m_pc_key_note[static_cast<size_t>(sc_i)] = -1;
                m_hw_note_held[static_cast<size_t>(held)] = false;
                DispatchKeyboardNoteOff(held);
            }
            return;
        }

        if (!PcKeyboardTypingAllowed())
        {
            return;
        }

        if (sc == SDL_SCANCODE_COMMA)
        {
            m_pc_keyboard_octave = std::max(0, m_pc_keyboard_octave - 1);
            SetStatus(std::string("Keyboard octave: ") + std::to_string(m_pc_keyboard_octave));
            return;
        }
        if (sc == SDL_SCANCODE_PERIOD)
        {
            m_pc_keyboard_octave = std::min(8, m_pc_keyboard_octave + 1);
            SetStatus(std::string("Keyboard octave: ") + std::to_string(m_pc_keyboard_octave));
            return;
        }

        const int offset = PcKeyboardScancodeToOffset(sc);
        if (offset < 0)
        {
            return;
        }

        int midi = 60 + (m_pc_keyboard_octave - 4) * 12 + offset;
        midi = std::clamp(midi, 0, 127);
        if (m_pc_key_note[static_cast<size_t>(sc_i)] == midi)
        {
            return;
        }
        if (m_pc_key_note[static_cast<size_t>(sc_i)] >= 0)
        {
            DispatchKeyboardNoteOff(m_pc_key_note[static_cast<size_t>(sc_i)]);
        }
        m_pc_key_note[static_cast<size_t>(sc_i)] = midi;
        m_hw_note_held[static_cast<size_t>(midi)] = true;
        DispatchKeyboardNoteOn(midi, 100);
    }

    void PanicPcKeyboardNotes()
    {
        for (int &n : m_pc_key_note)
        {
            if (n >= 0)
            {
                DispatchKeyboardNoteOff(n);
                n = -1;
            }
        }
    }
