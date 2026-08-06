/* Hardware MIDI input (RtMidi) for live audition.
 * Included inside NbEditorApp. Gated on SUPPORT_MIDI_HW.
 * Requires top-level includes: gui_midi_hw_input.h, rtmidi_c.h (and MidiHwDevice). */

#if SUPPORT_MIDI_HW == TRUE

    static bool MidiHwPortNameOk(const char *name)
    {
        if (!name || !name[0])
        {
            return false;
        }
        /* Reject control chars (WinMM/ghost ports sometimes return junk). */
        for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
        {
            if (*p < 0x20u)
            {
                return false;
            }
        }
        return true;
    }

    void MidiHwShutdownInput()
    {
        if (m_midi_hw_open)
        {
            midi_input_shutdown();
            m_midi_hw_open = false;
        }
        m_midi_hw_open_device_name.clear();
        m_hw_note_held.fill(false);
        for (auto &ch_notes : m_hw_full_ch_note_held)
        {
            ch_notes.fill(false);
        }
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
            if (!r || !r->ok || !r->ptr)
            {
                if (r)
                {
                    rtmidi_in_free(r);
                }
                continue;
            }
            const unsigned int cnt = rtmidi_get_port_count(r);
            for (unsigned int di = 0; di < cnt; ++di)
            {
                int needed = 0;
                if (rtmidi_get_port_name(r, di, NULL, &needed) < 0 || needed <= 1)
                {
                    continue;
                }
                if (needed > 512)
                {
                    needed = 512;
                }
                std::vector<char> buf(static_cast<size_t>(needed), 0);
                int buf_len = needed;
                if (rtmidi_get_port_name(r, di, buf.data(), &buf_len) < 0)
                {
                    continue;
                }
                buf[static_cast<size_t>(needed) - 1u] = '\0';
                if (!MidiHwPortNameOk(buf.data()))
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
            m_midi_hw_device_index = -1;
        }
        if (m_prefs_draft_midi_hw_device_index >= static_cast<int>(m_midi_hw_devices.size()))
        {
            m_prefs_draft_midi_hw_device_index = -1;
        }
    }

    bool MidiHwDeviceNamePresent(const std::string &name) const
    {
        if (name.empty())
        {
            return false;
        }
        for (const MidiHwDevice &dev : m_midi_hw_devices)
        {
            if (name == dev.name)
            {
                return true;
            }
        }
        return false;
    }

    void MidiHwOnDeviceLost()
    {
        PanicAuditionNotes();
        MidiHwShutdownInput();
        m_midi_hw_enabled = false;
        m_prefs_draft_midi_hw_enabled = false;
        m_midi_hw_device_index = -1;
        m_prefs_draft_midi_hw_device_index = -1;
        MidiHwRefreshDeviceList();
        SaveEditorPrefsToDisk();
        SetStatus("MIDI input device disconnected — input disabled");
    }

    void MidiHwCheckDeviceAlive()
    {
        if (!m_midi_hw_enabled)
        {
            return;
        }
        const uint32_t now = SDL_GetTicks();
        if (m_midi_hw_last_alive_check_ms != 0 &&
            (now - m_midi_hw_last_alive_check_ms) < 500u)
        {
            return;
        }
        m_midi_hw_last_alive_check_ms = now;

        MidiHwRefreshDeviceList();
        if (m_midi_hw_devices.empty())
        {
            MidiHwOnDeviceLost();
            return;
        }
        if (m_midi_hw_open && !m_midi_hw_open_device_name.empty() &&
            !MidiHwDeviceNamePresent(m_midi_hw_open_device_name))
        {
            MidiHwOnDeviceLost();
        }
    }

    bool MidiHwOpenSelectedDevice()
    {
        MidiHwShutdownInput();
        if (!m_midi_hw_enabled)
        {
            return false;
        }
        MidiHwRefreshDeviceList();
        if (m_midi_hw_devices.empty())
        {
            SetStatus("MIDI input: no devices available");
            m_midi_hw_enabled = false;
            m_prefs_draft_midi_hw_enabled = false;
            return false;
        }
        int api = -1;
        int port = -1;
        std::string open_name;
        if (m_midi_hw_device_index >= 0 &&
            m_midi_hw_device_index < static_cast<int>(m_midi_hw_devices.size()))
        {
            api = m_midi_hw_devices[static_cast<size_t>(m_midi_hw_device_index)].api_index;
            port = m_midi_hw_devices[static_cast<size_t>(m_midi_hw_device_index)].port_index;
            open_name = m_midi_hw_devices[static_cast<size_t>(m_midi_hw_device_index)].name;
        }
        else
        {
            /* Default: first enumerated device. */
            open_name = m_midi_hw_devices[0].name;
            api = m_midi_hw_devices[0].api_index;
            port = m_midi_hw_devices[0].port_index;
        }
        if (!midi_input_init("nbeditor", api, port))
        {
            SetStatus("MIDI input: failed to open device");
            m_midi_hw_enabled = false;
            m_prefs_draft_midi_hw_enabled = false;
            return false;
        }
        m_midi_hw_open = true;
        m_midi_hw_open_device_name = std::move(open_name);
        m_midi_hw_last_alive_check_ms = SDL_GetTicks();
        SetStatus("MIDI input enabled");
        return true;
    }

    void MidiHwApplyFromPrefs()
    {
        MidiHwRefreshDeviceList();
        if (m_midi_hw_enabled)
        {
            if (!MidiHwOpenSelectedDevice())
            {
                SaveEditorPrefsToDisk();
            }
        }
        else
        {
            MidiHwShutdownInput();
        }
    }

    void PollHardwareMidiInput()
    {
        MidiHwCheckDeviceAlive();
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
            const unsigned char mch = status & 0x0Fu;
            const KeyboardInputTarget focus = ResolveKeyboardTarget();

            /* Main preview: full multi-channel MIDI (zefidi-style). Note Off
             * always uses the full path if that ch/note was started there. */
            const bool full_note_off =
                (mtype == 0x80u || (mtype == 0x90u && sz >= 3 && (buf[2] & 0x7f) == 0)) &&
                sz >= 3 &&
                m_hw_full_ch_note_held[mch][buf[1] & 0x7f];

            if (focus == KeyboardInputTarget::MidiEditorRecord)
            {
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
                else if (mtype == 0xB0u && sz >= 3)
                {
                    MeRecordController(status, buf[1], buf[2]);
                }
                else if (mtype == 0xE0u && sz >= 3)
                {
                    MeRecordController(status, buf[1], buf[2]);
                }
                continue;
            }

            if (focus == KeyboardInputTarget::MainPreview || full_note_off)
            {
                if ((mtype == 0x80u || mtype == 0x90u) && sz >= 3)
                {
                    DispatchMainPreviewFullMidi(status, buf[1], buf[2]);
                }
                else if (mtype == 0xA0u && sz >= 3)
                {
                    DispatchMainPreviewFullMidi(status, buf[1], buf[2]);
                }
                else if (mtype == 0xB0u && sz >= 3)
                {
                    DispatchMainPreviewFullMidi(status, buf[1], buf[2]);
                }
                else if (mtype == 0xC0u && sz >= 2)
                {
                    DispatchMainPreviewFullMidi(status, buf[1], 0);
                }
                else if (mtype == 0xD0u && sz >= 2)
                {
                    DispatchMainPreviewFullMidi(status, buf[1], 0);
                }
                else if (mtype == 0xE0u && sz >= 3)
                {
                    DispatchMainPreviewFullMidi(status, buf[1], buf[2]);
                }
                continue;
            }

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
            else if (mtype == 0xA0u && sz >= 3)
            {
                DispatchAuditionMidiMessage(status, buf[1], buf[2]);
            }
            else if (mtype == 0xB0u && sz >= 3)
            {
                DispatchAuditionMidiMessage(status, buf[1], buf[2]);
            }
            else if (mtype == 0xC0u && sz >= 2)
            {
                DispatchAuditionMidiMessage(status, buf[1], 0);
            }
            else if (mtype == 0xD0u && sz >= 2)
            {
                DispatchAuditionMidiMessage(status, buf[1], 0);
            }
            else if (mtype == 0xE0u && sz >= 3)
            {
                DispatchAuditionMidiMessage(status, buf[1], buf[2]);
            }
        }
    }

    void DrawMidiHwPreferencesSection()
    {
        ImGui::SeparatorText("Hardware MIDI");

        /* Keep the list current while the dialog is open (unplug → empty/(None)). */
        {
            const uint32_t now = SDL_GetTicks();
            if (m_midi_hw_last_prefs_enum_ms == 0 ||
                (now - m_midi_hw_last_prefs_enum_ms) >= 500u)
            {
                m_midi_hw_last_prefs_enum_ms = now;
                MidiHwRefreshDeviceList();
            }
        }

        ImGui::Checkbox("Enable MIDI input (audition)", &m_prefs_draft_midi_hw_enabled);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Main preview: full multi-channel MIDI (like zefidi) —\n"
                "all 16 channels, CC, program change, pitch bend.\n"
                "Instrument/sample editors: remapped single-channel audition.\n"
                "Unplugging the active device disables input automatically.");
        }

        const char *preview = "(None)";
        if (!m_midi_hw_devices.empty())
        {
            if (m_prefs_draft_midi_hw_device_index >= 0 &&
                m_prefs_draft_midi_hw_device_index < static_cast<int>(m_midi_hw_devices.size()))
            {
                preview = m_midi_hw_devices[static_cast<size_t>(m_prefs_draft_midi_hw_device_index)].name;
            }
            else
            {
                preview = "(Default / first device)";
            }
        }
        else
        {
            m_prefs_draft_midi_hw_device_index = -1;
        }

        if (ImGui::BeginCombo("MIDI input device", preview))
        {
            if (m_midi_hw_devices.empty())
            {
                ImGui::BeginDisabled();
                ImGui::Selectable("(None)", true);
                ImGui::EndDisabled();
            }
            else
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
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Refresh devices"))
        {
            MidiHwRefreshDeviceList();
            m_midi_hw_last_prefs_enum_ms = SDL_GetTicks();
        }
        if (m_midi_hw_open)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Open");
        }
        else if (m_midi_hw_devices.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("No devices");
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
