/* Instrument alias / clone / move / copy-paste — included inside NbEditorApp
 * (after nbeditor_bank_add.inc). */

    enum class InstDestMode : int
    {
        Alias = 0,
        Clone = 1,
        Move = 2,
        Paste = 3,
    };

    void BuildUsedInstIdMap(bool used[512]) const
    {
        std::memset(used, 0, 512 * sizeof(bool));
        if (!m_bank_token)
        {
            return;
        }
        uint32_t inst_count = 0;
        if (BAERmfEditorBank_GetInstrumentCount(m_bank_token, &inst_count) == BAE_NO_ERROR)
        {
            for (uint32_t i = 0; i < inst_count; ++i)
            {
                BAERmfEditorBankInstrumentInfo info;
                std::memset(&info, 0, sizeof(info));
                if (BAERmfEditorBank_GetInstrumentInfo(m_bank_token, i, &info) != BAE_NO_ERROR)
                {
                    continue;
                }
                if (info.instID < 512u)
                {
                    used[info.instID] = true;
                }
            }
        }
        XAliasLinkResource *alias =
            XGetAliasLinkFromFile(reinterpret_cast<XFILE>(m_bank_token));
        if (alias)
        {
            const uint32_t n = static_cast<uint32_t>(XGetLong(&alias->numberOfAliases));
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t from_id = static_cast<uint32_t>(XGetLong(&alias->list[i].aliasFrom));
                if (from_id < 512u)
                {
                    used[from_id] = true;
                }
            }
            XDisposePtr(reinterpret_cast<XPTR>(alias));
        }
    }

    int FindInstrumentIndexByInstId(uint32_t inst_id) const
    {
        uint32_t resolved = 0;
        uint32_t index = 0;
        if (m_bank_token &&
            BAERmfEditorBank_ResolveInstID(m_bank_token, inst_id, &resolved, &index) == BAE_NO_ERROR)
        {
            return static_cast<int>(index);
        }
        for (const InstrumentRow &row : m_instruments)
        {
            if (!row.is_alias && row.target_inst_id == inst_id)
            {
                return static_cast<int>(row.instrument_index);
            }
        }
        return -1;
    }

    void OpenInstDestDialog(InstDestMode mode, int list_index)
    {
        if (!m_bank_token)
        {
            SetStatus("No bank loaded");
            return;
        }
        m_inst_dest_clip_path.clear();
        m_inst_dest_clip_delete = false;
        m_inst_dest_clip_inst_count = 0;
        m_inst_dest_is_clip_package = false;

        if (mode != InstDestMode::Paste)
        {
            if (list_index < 0 || list_index >= static_cast<int>(m_instruments.size()))
            {
                return;
            }
            const InstrumentRow &src = m_instruments[static_cast<size_t>(list_index)];
            if (src.from_song_document)
            {
                SetStatus("Song-document instruments cannot be aliased/cloned in the bank");
                return;
            }
            if (mode == InstDestMode::Move && src.is_alias)
            {
                SetStatus("Cannot move an alias — remove it or move the source instrument");
                return;
            }
            m_inst_dest_source_index = src.instrument_index;
            m_inst_dest_source_inst_id = src.target_inst_id;
            m_inst_dest_source_display_id = src.inst_id;
            m_inst_dest_bank = (mode == InstDestMode::Move) ? 2 : src.bank;
            m_inst_dest_percussion = src.percussion;
        }
        else
        {
            if (!m_inst_clipboard_valid)
            {
                SetStatus("Instrument clipboard is empty");
                return;
            }
            m_inst_dest_source_index = 0;
            m_inst_dest_source_inst_id = m_inst_clipboard_inst_id;
            m_inst_dest_source_display_id = m_inst_clipboard_inst_id;
            /* Default to the source bank:program (user may remap to another free slot). */
            DecodeInstSlot(m_inst_clipboard_inst_id,
                           &m_inst_dest_bank,
                           &m_inst_dest_percussion,
                           &m_inst_dest_program);
        }

        if (mode != InstDestMode::Paste)
        {
            bool used[512];
            BuildUsedInstIdMap(used);
            int free_prog = -1;
            const uint32_t base =
                BankInstIdFromSlot(m_inst_dest_bank, m_inst_dest_percussion, 0);
            for (int p = 0; p < 128; ++p)
            {
                if (base + static_cast<uint32_t>(p) < 512u && !used[base + static_cast<uint32_t>(p)])
                {
                    free_prog = p;
                    break;
                }
            }
            m_inst_dest_program = free_prog >= 0 ? free_prog : 0;
        }
        m_inst_dest_deep_clone = false;
        m_inst_dest_mode = mode;
        m_inst_dest_open = true;
    }

    void OpenInstDestDialogForClipPackage(const std::string &path,
                                          bool delete_after,
                                          uint32_t source_inst_id,
                                          int inst_count)
    {
        if (!m_bank_token && !LoadBuiltinBankAsActive())
        {
            SetStatus("No bank loaded");
            return;
        }
        if (path.empty())
        {
            SetStatus("Instrument clipboard is empty");
            return;
        }
        m_inst_dest_clip_path = path;
        m_inst_dest_clip_delete = delete_after;
        m_inst_dest_clip_inst_count = inst_count > 0 ? inst_count : 1;
        m_inst_dest_is_clip_package = true;
        m_inst_dest_source_index = 0;
        m_inst_dest_source_inst_id = source_inst_id;
        m_inst_dest_source_display_id = source_inst_id;
        DecodeInstSlot(source_inst_id,
                       &m_inst_dest_bank,
                       &m_inst_dest_percussion,
                       &m_inst_dest_program);
        m_inst_dest_deep_clone = false;
        m_inst_dest_mode = InstDestMode::Paste;
        m_inst_dest_open = true;
    }

    void CopyInstrumentAt(int list_index)
    {
        if (list_index < 0 || list_index >= static_cast<int>(m_instruments.size()))
        {
            return;
        }
        const InstrumentRow &src = m_instruments[static_cast<size_t>(list_index)];
        if (src.from_song_document)
        {
            SetStatus("Cannot copy a song-document instrument to the bank clipboard");
            return;
        }
        m_inst_clipboard_valid = true;
        m_inst_clipboard_inst_id = src.target_inst_id;
        m_inst_clipboard_name = src.name;
        char status[192];
        std::snprintf(status,
                      sizeof(status),
                      "Copied instrument %s (instID %u)",
                      src.name.c_str(),
                      src.target_inst_id);
        SetStatus(status);
    }

    void PerformInstDestAction()
    {
        if (!EnsureActiveBankWritable())
        {
            return;
        }

        const uint32_t dest_id =
            BankInstIdFromSlot(m_inst_dest_bank, m_inst_dest_percussion, m_inst_dest_program);
        bool used_programs[128];
        CollectUsedProgramsForGroup(m_inst_dest_bank, m_inst_dest_percussion, used_programs);
        if (used_programs[m_inst_dest_program])
        {
            SetStatus("Destination instrument slot is already in use");
            return;
        }

        /* Cross-process / OS clip package paste: remap INST + colliding SNDs. */
        if (m_inst_dest_mode == InstDestMode::Paste && m_inst_dest_is_clip_package)
        {
            const std::string path = m_inst_dest_clip_path;
            const bool delete_after = m_inst_dest_clip_delete;
            if (!RemapAndImportInstrumentClip(path, dest_id, delete_after))
            {
                return;
            }
            m_inst_dest_open = false;
            m_inst_dest_clip_path.clear();
            m_inst_dest_is_clip_package = false;
            m_inst_dest_clip_delete = false;
            m_inst_dest_clip_inst_count = 0;
            MarkBank01TouchedIfNeeded(m_inst_dest_bank);
            return;
        }

        int src_index = FindInstrumentIndexByInstId(m_inst_dest_source_inst_id);
        if (src_index < 0)
        {
            SetStatus("Source instrument no longer found in bank");
            m_inst_dest_open = false;
            return;
        }

        BAEResult r = BAE_NO_ERROR;
        char status[256];
        switch (m_inst_dest_mode)
        {
        case InstDestMode::Alias:
            r = BAERmfEditorBank_AliasInstrument(m_bank_token,
                                                 static_cast<uint32_t>(src_index),
                                                 dest_id);
            if (r == BAE_NO_ERROR)
            {
                std::snprintf(status, sizeof(status), "Created alias instID %u", dest_id);
                SetStatus(status);
            }
            break;
        case InstDestMode::Clone:
        case InstDestMode::Paste:
            r = BAERmfEditorBank_CloneInstrument(m_bank_token,
                                                 static_cast<uint32_t>(src_index),
                                                 dest_id,
                                                 m_inst_dest_deep_clone ? TRUE : FALSE);
            if (r == BAE_NO_ERROR)
            {
                std::snprintf(status,
                              sizeof(status),
                              "%s instrument to instID %u (%s)",
                              m_inst_dest_mode == InstDestMode::Paste ? "Pasted" : "Cloned",
                              dest_id,
                              m_inst_dest_deep_clone ? "deep" : "shared samples");
                SetStatus(status);
            }
            break;
        case InstDestMode::Move:
        {
            const uint32_t source_id = m_inst_dest_source_inst_id;
            r = BAERmfEditorBank_CloneInstrument(m_bank_token,
                                                 static_cast<uint32_t>(src_index),
                                                 dest_id,
                                                 FALSE);
            if (r == BAE_NO_ERROR)
            {
                const int del_index = FindInstrumentIndexByInstId(source_id);
                if (del_index >= 0)
                {
                    r = BAERmfEditorBank_DeleteInstrument(m_bank_token,
                                                          static_cast<uint32_t>(del_index));
                }
                else
                {
                    r = BAE_NOT_SETUP;
                }
            }
            if (r == BAE_NO_ERROR)
            {
                std::snprintf(status, sizeof(status), "Moved instrument to instID %u", dest_id);
                SetStatus(status);
            }
            break;
        }
        }

        if (r != BAE_NO_ERROR)
        {
            SetStatus(std::string("Instrument operation failed: ") + FormatBAEError(r));
            return;
        }
        m_inst_dest_open = false;
        MarkBank01TouchedIfNeeded(m_inst_dest_bank);
        {
            const int src_bank = static_cast<int>(m_inst_dest_source_inst_id / 256u);
            MarkBank01TouchedIfNeeded(src_bank);
        }
        RefreshListsFromBank();
    }

    void RemoveAliasAt(int list_index)
    {
        if (list_index < 0 || list_index >= static_cast<int>(m_instruments.size()))
        {
            return;
        }
        const InstrumentRow &row = m_instruments[static_cast<size_t>(list_index)];
        if (!row.is_alias)
        {
            SetStatus("Selected instrument is not an alias");
            return;
        }
        if (!EnsureActiveBankWritable())
        {
            return;
        }
        const BAEResult r = BAERmfEditorBank_DeleteAlias(m_bank_token, row.inst_id);
        if (r != BAE_NO_ERROR)
        {
            SetStatus(std::string("Remove alias failed: ") + FormatBAEError(r));
            return;
        }
        char status[128];
        std::snprintf(status, sizeof(status), "Removed alias instID %u", row.inst_id);
        SetStatus(status);
        RefreshListsFromBank();
    }

    void DeleteInstrumentAt(int list_index)
    {
        if (list_index < 0 || list_index >= static_cast<int>(m_instruments.size()))
        {
            return;
        }
        const InstrumentRow &row = m_instruments[static_cast<size_t>(list_index)];
        if (row.from_song_document || row.has_song_override)
        {
            if (m_document)
            {
                (void)BAERmfEditorDocument_DeleteInstrument(m_document, row.inst_id);
                (void)BAERmfEditorDocument_DeleteInstrument(m_document, row.target_inst_id);
                m_session_custom_inst_ids.erase(row.inst_id);
                m_session_custom_inst_ids.erase(row.target_inst_id);
                RefreshListsFromBank();
                SetStatus(std::string("Deleted song instrument ") + row.name);
                return;
            }
            SetStatus("Cannot delete a song-document instrument");
            return;
        }
        if (row.is_alias)
        {
            RemoveAliasAt(list_index);
            return;
        }
        if (!EnsureActiveBankWritable())
        {
            return;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        const XLongResourceID iid = static_cast<XLongResourceID>(row.inst_id);
        if (XTrashFileResource(bank, ID_INST, iid) == FALSE)
        {
            /* Fallback for edge cases where ZTRS trash rebuild fails. */
            const BAEResult r =
                BAERmfEditorBank_DeleteInstrument(m_bank_token, row.instrument_index);
            if (r != BAE_NO_ERROR)
            {
                SetStatus(std::string("Move instrument to Trash failed: ") + FormatBAEError(r));
                return;
            }
            char status[128];
            std::snprintf(status, sizeof(status), "Deleted instrument instID %u", row.inst_id);
            SetStatus(status);
            RefreshListsFromBank();
            return;
        }
        m_session_custom_inst_ids.erase(row.inst_id);
        char status[128];
        std::snprintf(status, sizeof(status), "Moved instrument instID %u to Trash", row.inst_id);
        SetStatus(status);
        RefreshListsFromBank();
    }

    void DrawInstDestDialog()
    {
        if (!m_inst_dest_open)
        {
            return;
        }
        const char *title = "Instrument Destination";
        switch (m_inst_dest_mode)
        {
        case InstDestMode::Alias:
            title = "Create Alias…";
            break;
        case InstDestMode::Clone:
            title = "Clone Instrument…";
            break;
        case InstDestMode::Move:
            title = "Move Instrument…";
            break;
        case InstDestMode::Paste:
            title = "Paste Instrument…";
            break;
        }
        ImGui::OpenPopup(title);
        bool popup_open = m_inst_dest_open;
        if (!ImGui::BeginPopupModal(title, &popup_open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (!popup_open)
            {
                m_inst_dest_open = false;
                m_inst_dest_clip_path.clear();
                m_inst_dest_is_clip_package = false;
            }
            return;
        }
        if (!popup_open)
        {
            m_inst_dest_open = false;
            m_inst_dest_clip_path.clear();
            m_inst_dest_is_clip_package = false;
            ImGui::EndPopup();
            return;
        }

        if (m_inst_dest_mode == InstDestMode::Paste)
        {
            int src_bank = 0;
            bool src_perc = false;
            int src_prog = 0;
            DecodeInstSlot(m_inst_dest_source_display_id, &src_bank, &src_perc, &src_prog);
            ImGui::Text("Source slot: B%d%s P%03d (instID %u)",
                        src_bank,
                        src_perc ? " Perc" : "",
                        src_prog,
                        m_inst_dest_source_display_id);
            if (m_inst_dest_is_clip_package && m_inst_dest_clip_inst_count > 1)
            {
                ImGui::TextDisabled(
                    "%d instruments in clipboard — first → selected slot; others → next free.",
                    m_inst_dest_clip_inst_count);
            }
            ImGui::Separator();
        }

        ImGui::Text("Destination slot");
        const int prev_bank = m_inst_dest_bank;
        const bool prev_perc = m_inst_dest_percussion;
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("Bank", &m_inst_dest_bank);
        m_inst_dest_bank = std::clamp(m_inst_dest_bank, 0, 127);
        if (m_inst_dest_mode == InstDestMode::Move && m_inst_dest_bank != 2)
        {
            ImGui::TextDisabled("BE2 Move targets Custom (Bank 2); other banks allowed here.");
        }
        ImGui::Checkbox("Percussion", &m_inst_dest_percussion);
        if (m_inst_dest_bank != prev_bank || m_inst_dest_percussion != prev_perc)
        {
            const int free_prog = FirstFreeProgramForGroup(m_inst_dest_bank, m_inst_dest_percussion);
            if (free_prog >= 0)
            {
                m_inst_dest_program = free_prog;
            }
        }
        ImGui::SetNextItemWidth(80.0f);
        ImGui::InputInt("Program", &m_inst_dest_program);
        m_inst_dest_program = std::clamp(m_inst_dest_program, 0, 127);

        bool used[128];
        CollectUsedProgramsForGroup(m_inst_dest_bank, m_inst_dest_percussion, used);
        const uint32_t dest_id =
            BankInstIdFromSlot(m_inst_dest_bank, m_inst_dest_percussion, m_inst_dest_program);
        if (used[m_inst_dest_program])
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "in use");
            ImGui::TextDisabled("Choose a free program slot — Apply is disabled while occupied.");
        }
        else
        {
            ImGui::SameLine();
            ImGui::TextDisabled("instID %u", dest_id);
        }

        if ((m_inst_dest_mode == InstDestMode::Clone || m_inst_dest_mode == InstDestMode::Paste) &&
            !m_inst_dest_is_clip_package)
        {
            ImGui::Separator();
            ImGui::Checkbox("Deep clone (duplicate samples)", &m_inst_dest_deep_clone);
            if (!m_inst_dest_deep_clone)
            {
                ImGui::TextDisabled("Shares sample resources with the source.");
            }
        }
        else if (m_inst_dest_is_clip_package)
        {
            ImGui::Separator();
            ImGui::TextDisabled("Samples with IDs already in use are remapped to free SND IDs.");
        }

        ImGui::Separator();
        const char *ok_label = "OK";
        switch (m_inst_dest_mode)
        {
        case InstDestMode::Alias:
            ok_label = "Create Alias";
            break;
        case InstDestMode::Clone:
            ok_label = "Clone";
            break;
        case InstDestMode::Move:
            ok_label = "Move";
            break;
        case InstDestMode::Paste:
            ok_label = "Paste";
            break;
        }
        const bool can_apply = !used[m_inst_dest_program] && m_bank_token != 0;
        if (!can_apply)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(ok_label, ImVec2(140.0f, 0.0f)))
        {
            PerformInstDestAction();
            if (!m_inst_dest_open)
            {
                ImGui::CloseCurrentPopup();
            }
        }
        if (!can_apply)
        {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            m_inst_dest_open = false;
            m_inst_dest_clip_path.clear();
            m_inst_dest_is_clip_package = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
