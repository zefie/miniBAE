/* Hardware MIDI input (RtMidi) for live audition.
 * Included inside NbEditorApp. Gated on SUPPORT_MIDI_HW.
 * Requires top-level includes: gui_midi_hw_input.h, rtmidi_c.h (and MidiHwDevice). */

#if SUPPORT_MIDI_HW == TRUE

    void MidiHwShutdownInput()
    {
        if (m_midi_hw_open)
        {
            midi_input_shutdown();
            m_midi_hw_open = false;
        }
        m_hw_note_held.fill(false);
        m_keyboard_note_target.fill(KeyboardInputTarget::None);
    }

    void MidiHwRefreshDeviceList()
    {
        m_midi_hw_devices.clear();
        enum RtMidiApi apis[16];
        const int api_count =
            rtmidi_get_compiled_api(apis, (unsigned int)(sizeof(apis) / sizeof(apis[0])));
        for (int ai = 0; ai < api_count; ++ai)
        {
            RtMidiInPtr r = rtmidi_in_create(apis[ai], "nbeditor-enum", 100);
            if (!r)
            {
                continue;
            }
            const unsigned int cnt = rtmidi_get_port_count(r);
            for (unsigned int di = 0; di < cnt; ++di)
            {
                int needed = 0;
                rtmidi_get_port_name(r, di, NULL, &needed);
                if (needed <= 0)
                {
                    continue;
                }
                std::vector<char> buf(static_cast<size_t>(needed) + 1u, 0);
                int buf_len = needed;
                if (rtmidi_get_port_name(r, di, buf.data(), &buf_len) < 0)
                {
                    continue;
                }
                MidiHwDevice dev;
                std::snprintf(dev.name, sizeof(dev.name), "%s", buf.data());
                dev.api_index = ai;
                dev.port_index = static_cast<int>(di);
                m_midi_hw_devices.push_back(dev);
            }
            rtmidi_in_free(r);
        }
        if (m_midi_hw_device_index >= static_cast<int>(m_midi_hw_devices.size()))
        {
            m_midi_hw_device_index = m_midi_hw_devices.empty() ? -1 : 0;
        }
    }

    bool MidiHwOpenSelectedDevice()
    {
        MidiHwShutdownInput();
        if (!m_midi_hw_enabled)
        {
            return false;
        }
        int api = -1;
        int port = -1;
        if (m_midi_hw_device_index >= 0 &&
            m_midi_hw_device_index < static_cast<int>(m_midi_hw_devices.size()))
        {
            api = m_midi_hw_devices[static_cast<size_t>(m_midi_hw_device_index)].api_index;
            port = m_midi_hw_devices[static_cast<size_t>(m_midi_hw_device_index)].port_index;
        }
        if (!midi_input_init("nbeditor", api, port))
        {
            SetStatus("MIDI input: failed to open device");
            return false;
        }
        m_midi_hw_open = true;
        SetStatus("MIDI input enabled");
        return true;
    }

    void MidiHwApplyFromPrefs()
    {
        if (m_midi_hw_devices.empty())
        {
            MidiHwRefreshDeviceList();
        }
        if (m_midi_hw_enabled)
        {
            MidiHwOpenSelectedDevice();
        }
        else
        {
            MidiHwShutdownInput();
        }
    }

    void PollHardwareMidiInput()
    {
        if (!m_midi_hw_open || !m_midi_hw_enabled)
        {
            return;
        }
        unsigned char buf[1024];
        unsigned int sz = 0;
        double ts = 0.0;
        while (midi_input_poll(buf, &sz, &ts))
        {
            (void)ts;
            if (sz < 1)
            {
                continue;
            }
            const unsigned char status = buf[0];
            const unsigned char mtype = status & 0xF0u;
            if (mtype == 0x80u && sz >= 3)
            {
                const int note = buf[1] & 0x7f;
                DispatchKeyboardNoteOff(note);
                m_hw_note_held[static_cast<size_t>(note)] = false;
            }
            else if (mtype == 0x90u && sz >= 3)
            {
                const int note = buf[1] & 0x7f;
                const unsigned char vel = buf[2] & 0x7f;
                if (vel != 0)
                {
                    DispatchKeyboardNoteOn(note, vel);
                    m_hw_note_held[static_cast<size_t>(note)] = true;
                }
                else
                {
                    DispatchKeyboardNoteOff(note);
                    m_hw_note_held[static_cast<size_t>(note)] = false;
                }
            }
        }
    }

    void DrawMidiHwPreferencesSection()
    {
        ImGui::SeparatorText("Hardware MIDI");
        ImGui::Checkbox("Enable MIDI input (audition)", &m_prefs_draft_midi_hw_enabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Route USB/hardware Note On/Off to the focused editor sink:\n"
                "sample editor preview, instrument editor, or main audition.\n"
                "Piano-roll recording will use the same input when armed.");
        }
        if (m_midi_hw_devices.empty())
        {
            MidiHwRefreshDeviceList();
        }
        const char *preview = "(Default / first device)";
        if (m_prefs_draft_midi_hw_device_index >= 0 &&
            m_prefs_draft_midi_hw_device_index < static_cast<int>(m_midi_hw_devices.size()))
        {
            preview = m_midi_hw_devices[static_cast<size_t>(m_prefs_draft_midi_hw_device_index)].name;
        }
        if (ImGui::BeginCombo("MIDI input device", preview))
        {
            if (ImGui::Selectable("(Default / first device)",
                                  m_prefs_draft_midi_hw_device_index < 0))
            {
                m_prefs_draft_midi_hw_device_index = -1;
            }
            for (int i = 0; i < static_cast<int>(m_midi_hw_devices.size()); ++i)
            {
                const bool selected = (i == m_prefs_draft_midi_hw_device_index);
                if (ImGui::Selectable(m_midi_hw_devices[static_cast<size_t>(i)].name, selected))
                {
                    m_prefs_draft_midi_hw_device_index = i;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Refresh devices"))
        {
            MidiHwRefreshDeviceList();
        }
        if (m_midi_hw_open)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Open");
        }
    }

#else /* SUPPORT_MIDI_HW */

    void MidiHwShutdownInput() {}
    void MidiHwApplyFromPrefs() {}
    void PollHardwareMidiInput() {}
    void DrawMidiHwPreferencesSection()
    {
        ImGui::SeparatorText("Hardware MIDI");
        ImGui::TextDisabled("Built without SUPPORT_MIDI_HW (RtMidi / ALSA).");
    }

#endif /* SUPPORT_MIDI_HW */
