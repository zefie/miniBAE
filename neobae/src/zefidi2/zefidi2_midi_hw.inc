/* Hardware MIDI input via shared gui_midi_hw_input (methods of Zefidi2App). */

#if SUPPORT_MIDI_HW == TRUE

    void StartMidiInput()
    {
        if (m_midi_in_active)
        {
            return;
        }
        if (midi_input_init("zefidi2", -1, m_midi_in_port))
        {
            m_midi_in_active = true;
            SetStatus("MIDI input on");
        }
        else
        {
            SetStatus("MIDI input failed");
        }
    }

    void StopMidiInput()
    {
        if (!m_midi_in_active)
        {
            return;
        }
        midi_input_shutdown();
        m_midi_in_active = false;
    }

    void PollHardwareMidiInput()
    {
        if (!m_midi_in_enabled)
        {
            if (m_midi_in_active)
            {
                StopMidiInput();
            }
            return;
        }
        if (!m_midi_in_active)
        {
            StartMidiInput();
            if (!m_midi_in_active)
            {
                return;
            }
        }

        unsigned char buf[1024];
        unsigned int size = 0;
        double ts = 0.0;
        while (midi_input_poll(buf, &size, &ts))
        {
            if (size < 1)
            {
                continue;
            }
            BAESong song = ActiveNoteSong();
            if (!song)
            {
                continue;
            }
            const unsigned char status = buf[0];
            const unsigned char cmd = status & 0xF0u;
            const unsigned char ch = status & 0x0Fu;
            const int note = (size >= 2) ? static_cast<int>(buf[1]) : -1;
            const bool on_selected = (static_cast<int>(ch) == KeyboardChannel());
            if (cmd == 0x90u && size >= 3)
            {
                if (buf[2])
                {
                    (void)BAESong_NoteOn(song, ch, buf[1], buf[2], 0);
                    if (on_selected && note >= 0 && note < 128)
                    {
                        m_keyboard_midi_lit[static_cast<size_t>(note)] = true;
                    }
                }
                else
                {
                    (void)BAESong_NoteOff(song, ch, buf[1], 0, 0);
                    if (on_selected && note >= 0 && note < 128)
                    {
                        m_keyboard_midi_lit[static_cast<size_t>(note)] = false;
                    }
                }
            }
            else if (cmd == 0x80u && size >= 3)
            {
                (void)BAESong_NoteOff(song, ch, buf[1], buf[2], 0);
                if (on_selected && note >= 0 && note < 128)
                {
                    m_keyboard_midi_lit[static_cast<size_t>(note)] = false;
                }
            }
            else if (cmd == 0xB0u && size >= 3)
            {
                (void)BAESong_ControlChange(song, ch, buf[1], buf[2], 0);
                /* CC0 bank MSB / CC32 bank LSB — refresh UI when selected ch. */
                if (on_selected && (buf[1] == 0 || buf[1] == 32))
                {
                    m_keyboard_prog_dirty = false;
                    SyncKeyboardBankProgramFromEngine();
                }
            }
            else if (cmd == 0xC0u && size >= 2)
            {
                (void)BAESong_ProgramChange(song, ch, buf[1], 0);
                if (on_selected)
                {
                    m_keyboard_program = buf[1];
                    m_keyboard_prog_dirty = true;
                    m_keyboard_prog_hold_until_ms = SDL_GetTicks() + 400;
                }
            }
            else if (cmd == 0xE0u && size >= 3)
            {
                (void)BAESong_PitchBend(song, ch, buf[1], buf[2], 0);
            }
        }
        (void)ts;
    }

    void DrawMidiHwSettings()
    {
        bool en = m_midi_in_enabled;
        if (ImGui::Checkbox("MIDI Input", &en))
        {
            m_midi_in_enabled = en;
            if (!en)
            {
                StopMidiInput();
            }
        }
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("Input port", &m_midi_in_port);
        if (ImGui::Button("Restart MIDI In"))
        {
            StopMidiInput();
            if (m_midi_in_enabled)
            {
                StartMidiInput();
            }
        }
        if (m_midi_in_active)
        {
            ImGui::TextDisabled("drops: %u", midi_input_drops());
        }
    }

#endif /* SUPPORT_MIDI_HW */
