/* Trash (TRSH) browser — included inside NbEditorApp. */

    static const char *TrashTypeLabel(XResourceType t)
    {
        switch (t)
        {
        case ID_INST:
            return "INST";
        case ID_ALIAS:
            return "ALIS";
        case ID_SND:
            return "snd ";
        case ID_CSND:
            return "csnd";
        case ID_ESND:
            return "esnd";
        case ID_SONG:
            return "SONG";
        case ID_MIDI:
            return "Midi";
        case ID_MIDI_OLD:
            return "MIDI";
        default:
            return t ? "other" : "unknown";
        }
    }

    int CountBankTrash() const
    {
        if (!m_bank_token)
        {
            return 0;
        }
        return static_cast<int>(
            XCountFileTrash(reinterpret_cast<XFILE>(m_bank_token)));
    }

    void CompactBankTrashDuplicates()
    {
        if (!m_bank_token)
        {
            return;
        }
        if (!EnsureActiveBankWritable())
        {
            return;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        const int before = CountBankTrash();
        int32_t removed = 0;
        if (XDedupeFileTrash(bank, &removed) == FALSE)
        {
            SetStatus("Compact Trash duplicates failed");
            return;
        }
        if (removed > 0)
        {
            RefreshListsFromBank();
            m_trash_selected = -1;
            char status[160];
            std::snprintf(status,
                          sizeof(status),
                          "Removed %d duplicate Trash entr%s (%d → %d)",
                          static_cast<int>(removed),
                          removed == 1 ? "y" : "ies",
                          before,
                          before - static_cast<int>(removed));
            SetStatus(status);
        }
    }

    void OpenTrashWindow()
    {
        m_trash_open = true;
        m_trash_selected = -1;
        /* Legacy soft-delete used id=0 for every TRSH; older pack paths then
         * cloned the first trash body into every slot. Collapse those on open. */
        if (CountBankTrash() > 1)
        {
            CompactBankTrashDuplicates();
        }
    }

    void RestoreTrashAt(int trash_index)
    {
        if (!m_bank_token || trash_index < 0)
        {
            return;
        }
        if (!EnsureActiveBankWritable())
        {
            return;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        char name_before[256] = {};
        XResourceType orig_type = 0;
        XLongResourceID orig_id = 0;
        XLongResourceID trash_id = 0;
        int32_t payload_size = 0;
        bool has_meta = false;
        bool guessed = false;
        (void)XGetIndexedFileTrashInfo(bank,
                                       trash_index,
                                       &trash_id,
                                       &orig_type,
                                       &orig_id,
                                       name_before,
                                       &payload_size,
                                       &has_meta,
                                       &guessed);

        XLongResourceID restored_id = 0;
        if (XRestoreIndexedFileTrash(bank, trash_index, &restored_id) == FALSE)
        {
            SetStatus("Restore from Trash failed (unknown type or I/O error)");
            return;
        }
        if (orig_type == ID_INST ||
            XExistsFileResource(bank, ID_INST, restored_id) != FALSE)
        {
            m_session_custom_inst_ids.insert(static_cast<uint32_t>(restored_id));
        }
        else if (orig_type == ID_SND || orig_type == ID_CSND || orig_type == ID_ESND ||
                 XExistsFileResource(bank, ID_SND, restored_id) != FALSE ||
                 XExistsFileResource(bank, ID_CSND, restored_id) != FALSE ||
                 XExistsFileResource(bank, ID_ESND, restored_id) != FALSE)
        {
            m_session_custom_snd_ids.insert(static_cast<uint32_t>(restored_id));
        }
        RefreshListsFromBank();
        m_trash_selected = -1;
        char status[192];
        if (name_before[0])
        {
            std::snprintf(status,
                          sizeof(status),
                          "Restored \"%s\" as %s id %ld",
                          name_before,
                          TrashTypeLabel(orig_type),
                          static_cast<long>(restored_id));
        }
        else
        {
            std::snprintf(status,
                          sizeof(status),
                          "Restored %s id %ld from Trash",
                          TrashTypeLabel(orig_type),
                          static_cast<long>(restored_id));
        }
        SetStatus(status);
    }

    void PurgeTrashAt(int trash_index)
    {
        if (!m_bank_token || trash_index < 0)
        {
            return;
        }
        if (!EnsureActiveBankWritable())
        {
            return;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        if (XPurgeIndexedFileTrash(bank, trash_index) == FALSE)
        {
            SetStatus("Permanently delete from Trash failed");
            return;
        }
        RefreshListsFromBank();
        m_trash_selected = -1;
        SetStatus("Permanently deleted item from Trash");
    }

    void EmptyBankTrash()
    {
        if (!m_bank_token)
        {
            return;
        }
        if (!EnsureActiveBankWritable())
        {
            return;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        const int before = CountBankTrash();
        if (before <= 0)
        {
            SetStatus("Trash is empty");
            return;
        }
        if (XEmptyFileTrash(bank) == FALSE)
        {
            SetStatus("Empty Trash failed");
            return;
        }
        RefreshListsFromBank();
        m_trash_selected = -1;
        m_confirm_empty_trash_open = false;
        char status[128];
        std::snprintf(status, sizeof(status), "Emptied Trash (%d item%s)", before, before == 1 ? "" : "s");
        SetStatus(status);
    }

    void DrawTrashWindow()
    {
        if (!m_trash_open)
        {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(520, 360), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Trash", &m_trash_open))
        {
            ImGui::End();
            return;
        }

        if (!m_bank_token)
        {
            ImGui::TextDisabled("No bank loaded.");
            ImGui::End();
            return;
        }

        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        const int count = CountBankTrash();
        ImGui::Text("%d item%s in Trash", count, count == 1 ? "" : "s");
        ImGui::SameLine();
        ImGui::TextDisabled("(survives Save; Empty Trash to purge)");
        ImGui::Separator();

        if (ImGui::BeginTable("##trash_table",
                              5,
                              ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchProp,
                              ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2.2f)))
        {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Orig ID", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Meta", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < count; ++i)
            {
                XLongResourceID trash_id = 0;
                XResourceType orig_type = 0;
                XLongResourceID orig_id = 0;
                char name[256] = {};
                int32_t payload_size = 0;
                bool has_meta = false;
                bool guessed = false;
                if (XGetIndexedFileTrashInfo(bank,
                                             i,
                                             &trash_id,
                                             &orig_type,
                                             &orig_id,
                                             name,
                                             &payload_size,
                                             &has_meta,
                                             &guessed) == FALSE)
                {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = (m_trash_selected == i);
                char label[288];
                if (name[0])
                {
                    std::snprintf(label, sizeof(label), "%s##trash%d", name, i);
                }
                else
                {
                    std::snprintf(label, sizeof(label), "(unnamed)##trash%d", i);
                }
                if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    m_trash_selected = i;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                {
                    RestoreTrashAt(i);
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(TrashTypeLabel(orig_type));
                ImGui::TableNextColumn();
                if (has_meta)
                {
                    ImGui::Text("%ld", static_cast<long>(orig_id));
                }
                else
                {
                    ImGui::TextDisabled("—");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d", static_cast<int>(payload_size));
                ImGui::TableNextColumn();
                if (has_meta)
                {
                    ImGui::TextUnformatted("ZTRS");
                }
                else if (guessed)
                {
                    ImGui::TextDisabled("legacy*");
                }
                else
                {
                    ImGui::TextDisabled("legacy");
                }
            }
            ImGui::EndTable();
        }

        const bool has_sel = m_trash_selected >= 0 && m_trash_selected < count;
        if (ImGui::Button("Restore") && has_sel)
        {
            RestoreTrashAt(m_trash_selected);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Permanently...") && has_sel)
        {
            m_confirm_purge_trash_index = m_trash_selected;
            m_confirm_purge_trash_open = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Empty Trash...") && count > 0)
        {
            m_confirm_empty_trash_open = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Compact Duplicates") && count > 1)
        {
            CompactBankTrashDuplicates();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close"))
        {
            m_trash_open = false;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Legacy TRSH (from older saves) has no original type/id metadata.\n"
                "Restore guesses snd/INST from the body when possible.");
        }

        ImGui::End();

        if (m_confirm_empty_trash_open)
        {
            ImGui::OpenPopup("Empty Trash?");
            m_confirm_empty_trash_open = false;
            m_confirm_empty_trash_popup = true;
        }
        if (m_confirm_empty_trash_popup)
        {
            if (ImGui::BeginPopupModal("Empty Trash?",
                                       &m_confirm_empty_trash_popup,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Permanently delete all %d Trash item%s?\nThis cannot be undone.",
                            count,
                            count == 1 ? "" : "s");
                if (ImGui::Button("Empty Trash", ImVec2(140, 0)))
                {
                    EmptyBankTrash();
                    m_confirm_empty_trash_popup = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                {
                    m_confirm_empty_trash_popup = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        if (m_confirm_purge_trash_open)
        {
            ImGui::OpenPopup("Delete Trash Item?");
            m_confirm_purge_trash_open = false;
            m_confirm_purge_trash_popup = true;
        }
        if (m_confirm_purge_trash_popup)
        {
            if (ImGui::BeginPopupModal("Delete Trash Item?",
                                       &m_confirm_purge_trash_popup,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Permanently delete the selected Trash item?\nThis cannot be undone.");
                if (ImGui::Button("Delete", ImVec2(120, 0)))
                {
                    PurgeTrashAt(m_confirm_purge_trash_index);
                    m_confirm_purge_trash_popup = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                {
                    m_confirm_purge_trash_popup = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }
