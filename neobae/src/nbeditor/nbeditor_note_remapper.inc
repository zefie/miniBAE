/* MIDI Note Remapper — Song menu dialog (included inside NbEditorApp). */

    const char *InstrumentSlotDisplayName(int bank, int program, bool percussion) const
    {
        for (const InstrumentRow &inst : m_instruments)
        {
            if (inst.bank == bank && inst.program == program && inst.percussion == percussion)
            {
                return inst.name.empty() ? "(None)" : inst.name.c_str();
            }
        }
        return "(None)";
    }

    void InvalidateMidiNoteRemapScan()
    {
        m_note_remap_scanned = false;
        m_note_remap_count = 0;
    }

    void ScanMidiNoteRemapper()
    {
        if (!m_document)
        {
            InvalidateMidiNoteRemapScan();
            return;
        }
        m_note_remap_count = CountNotesUsingInstSlot(m_document,
                                                     m_note_remap_src_bank,
                                                     m_note_remap_src_program,
                                                     m_note_remap_src_percussion);
        m_note_remap_scanned = true;
    }

    void OpenMidiNoteRemapperDialog()
    {
        if (!m_document)
        {
            return;
        }
        m_note_remap_src_bank = 0;
        m_note_remap_src_program = 0;
        m_note_remap_src_percussion = false;
        m_note_remap_dst_bank = 0;
        m_note_remap_dst_program = 0;
        m_note_remap_dst_percussion = false;
        if (m_selected_instrument >= 0 &&
            m_selected_instrument < static_cast<int>(m_instruments.size()))
        {
            const InstrumentRow &sel = m_instruments[static_cast<size_t>(m_selected_instrument)];
            m_note_remap_src_bank = sel.bank;
            m_note_remap_src_program = sel.program;
            m_note_remap_src_percussion = sel.percussion;
            m_note_remap_dst_bank = sel.bank;
            m_note_remap_dst_program = sel.program;
            m_note_remap_dst_percussion = sel.percussion;
        }
        else
        {
            m_note_remap_src_bank = m_selected_bank;
            m_note_remap_src_program = m_selected_program;
            m_note_remap_dst_bank = m_selected_bank;
            m_note_remap_dst_program = m_selected_program;
        }
        InvalidateMidiNoteRemapScan();
        m_note_remap_open = true;
    }

    void ApplyMidiNoteRemapper()
    {
        if (!m_document)
        {
            SetStatus("No song document open");
            return;
        }
        if (m_note_remap_src_bank == m_note_remap_dst_bank &&
            m_note_remap_src_program == m_note_remap_dst_program &&
            m_note_remap_src_percussion == m_note_remap_dst_percussion)
        {
            SetStatus("Source and destination are the same");
            return;
        }

        ScanMidiNoteRemapper();
        const uint32_t n = m_note_remap_count;
        if (n == 0)
        {
            SetStatus("No notes use the source bank/program");
            return;
        }

        if (!RemapDocumentInstSlot(m_document,
                                   m_note_remap_src_bank,
                                   m_note_remap_src_program,
                                   m_note_remap_src_percussion,
                                   m_note_remap_dst_bank,
                                   m_note_remap_dst_program,
                                   m_note_remap_dst_percussion))
        {
            SetStatus("MIDI note remap failed");
            return;
        }

        m_document_dirty = true;
        MeInvalidateMidiDocCaches();
        (void)CommitActiveSessionSong();
        (void)ReloadPlaybackFromDocument(true);

        char status[192];
        std::snprintf(status,
                      sizeof(status),
                      "Remapped %u note%s from B%d%s P%03d → B%d%s P%03d",
                      n,
                      n == 1 ? "" : "s",
                      m_note_remap_src_bank,
                      m_note_remap_src_percussion ? " Perc" : "",
                      m_note_remap_src_program,
                      m_note_remap_dst_bank,
                      m_note_remap_dst_percussion ? " Perc" : "",
                      m_note_remap_dst_program);
        SetStatus(status);

        /* Source slot should now be empty; rescans to 0. */
        ScanMidiNoteRemapper();
    }

    void DrawMidiNoteRemapSlotPickCombo(const char *label,
                                        int *bank,
                                        int *program,
                                        bool *percussion)
    {
        char preview[256];
        std::snprintf(preview,
                      sizeof(preview),
                      "B%d%s P%03d  %s",
                      *bank,
                      *percussion ? " Perc" : "",
                      *program,
                      InstrumentSlotDisplayName(*bank, *program, *percussion));
        ImGui::SetNextItemWidth(360.0f);
        if (ImGui::BeginCombo(label, preview))
        {
            for (size_t i = 0; i < m_instruments.size(); ++i)
            {
                const InstrumentRow &inst = m_instruments[i];
                if (inst.is_alias)
                {
                    continue;
                }
                char row[256];
                std::snprintf(row,
                              sizeof(row),
                              "B%d%s P%03d  %s##note_remap_%s_%zu",
                              inst.bank,
                              inst.percussion ? " Perc" : "",
                              inst.program,
                              inst.name.empty() ? "(None)" : inst.name.c_str(),
                              label,
                              i);
                const bool selected = (inst.bank == *bank && inst.program == *program &&
                                       inst.percussion == *percussion);
                if (ImGui::Selectable(row, selected))
                {
                    *bank = inst.bank;
                    *program = inst.program;
                    *percussion = inst.percussion;
                    InvalidateMidiNoteRemapScan();
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    void DrawMidiNoteRemapperDialog()
    {
        if (!m_note_remap_open)
        {
            return;
        }
        if (!m_document)
        {
            m_note_remap_open = false;
            return;
        }

        bool open = m_note_remap_open;
        if (ImGui::Begin("MIDI Note Remapper", &open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped(
                "Find every MIDI note using the source bank/program and remap it to the "
                "destination. Melodic notes match bank+program (not channel 10); percussion "
                "matches drum note number on channel 10.");

            ImGui::SeparatorText("Source");
            const int prev_src_bank = m_note_remap_src_bank;
            const int prev_src_prog = m_note_remap_src_program;
            const bool prev_src_perc = m_note_remap_src_percussion;
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Bank##note_remap_src", &m_note_remap_src_bank);
            m_note_remap_src_bank = std::clamp(m_note_remap_src_bank, 0, 127);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Program##note_remap_src", &m_note_remap_src_program);
            m_note_remap_src_program = std::clamp(m_note_remap_src_program, 0, 127);
            ImGui::SameLine();
            ImGui::Checkbox("Percussion##note_remap_src", &m_note_remap_src_percussion);
            ImGui::TextDisabled("%s",
                                InstrumentSlotDisplayName(m_note_remap_src_bank,
                                                          m_note_remap_src_program,
                                                          m_note_remap_src_percussion));
            DrawMidiNoteRemapSlotPickCombo("Pick source…",
                                           &m_note_remap_src_bank,
                                           &m_note_remap_src_program,
                                           &m_note_remap_src_percussion);
            if (m_note_remap_src_bank != prev_src_bank ||
                m_note_remap_src_program != prev_src_prog ||
                m_note_remap_src_percussion != prev_src_perc)
            {
                InvalidateMidiNoteRemapScan();
            }

            ImGui::SeparatorText("Destination");
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Bank##note_remap_dst", &m_note_remap_dst_bank);
            m_note_remap_dst_bank = std::clamp(m_note_remap_dst_bank, 0, 127);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Program##note_remap_dst", &m_note_remap_dst_program);
            m_note_remap_dst_program = std::clamp(m_note_remap_dst_program, 0, 127);
            ImGui::SameLine();
            ImGui::Checkbox("Percussion##note_remap_dst", &m_note_remap_dst_percussion);
            ImGui::TextDisabled("%s",
                                InstrumentSlotDisplayName(m_note_remap_dst_bank,
                                                          m_note_remap_dst_program,
                                                          m_note_remap_dst_percussion));
            DrawMidiNoteRemapSlotPickCombo("Pick destination…",
                                           &m_note_remap_dst_bank,
                                           &m_note_remap_dst_program,
                                           &m_note_remap_dst_percussion);

            ImGui::Separator();
            if (ImGui::Button("Scan", ImVec2(100.0f, 0.0f)))
            {
                ScanMidiNoteRemapper();
            }
            ImGui::SameLine();
            if (m_note_remap_scanned)
            {
                ImGui::Text("%u note%s found / to be remapped",
                            m_note_remap_count,
                            m_note_remap_count == 1 ? "" : "s");
            }
            else
            {
                ImGui::TextDisabled("Scan to count notes using the source slot.");
            }

            ImGui::Spacing();
            const bool same_slot =
                m_note_remap_src_bank == m_note_remap_dst_bank &&
                m_note_remap_src_program == m_note_remap_dst_program &&
                m_note_remap_src_percussion == m_note_remap_dst_percussion;
            if (same_slot)
            {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Remap", ImVec2(100.0f, 0.0f)))
            {
                ApplyMidiNoteRemapper();
            }
            if (same_slot)
            {
                ImGui::EndDisabled();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
            {
                open = false;
            }
            if (same_slot)
            {
                ImGui::TextDisabled("Choose a different destination slot to enable Remap.");
            }
        }
        ImGui::End();
        m_note_remap_open = open;
    }
