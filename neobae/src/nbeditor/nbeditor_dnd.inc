/* Tab-scoped drag-and-drop + bank 0/1 merge (preserve bank 2+).
 * Included inside NbEditorApp after bank_add / instrument_ops. */

    static bool IsAudioSampleExtension(const std::string &ext)
    {
        return ext == "wav" || ext == "aif" || ext == "aiff" || ext == "mp3" ||
               ext == "mpg" || ext == "mpeg" || ext == "ogg" || ext == "oga" ||
               ext == "flac" || ext == "opus" || ext == "qoa";
    }

    static bool IsSongImportExtension(const std::string &ext)
    {
        return ext == "mid" || ext == "midi" || ext == "kar" || ext == "rmi" ||
               ext == "rmf" || ext == "zmf" || IsModuleExtension(ext) ||
               ext == "bsn" || ext == "zsn" || ext == "nbs";
    }

    void MarkBank01TouchedIfNeeded(int bank)
    {
        if (bank >= 0 && bank < 2)
        {
            m_bank01_touched = true;
        }
    }

    bool Bank01HasInstruments() const
    {
        for (const InstrumentRow &row : m_instruments)
        {
            if (!row.is_alias && row.bank >= 0 && row.bank < 2)
            {
                return true;
            }
        }
        return false;
    }

    bool Bank2PlusHasInstruments() const
    {
        for (const InstrumentRow &row : m_instruments)
        {
            if (!row.is_alias && row.bank >= 2)
            {
                return true;
            }
        }
        return false;
    }

    /* Bank file open/merge should keep Bank 2+ / session customs. If none of that
     * exists (typical New Session = pristine built-in only), fully replace the
     * bank token instead of merging on top of built-in leftovers. */
    bool ShouldPreserveBank2OnBankLoad() const
    {
        return Bank2PlusHasInstruments() || !m_session_custom_snd_ids.empty() ||
               !m_session_custom_inst_ids.empty();
    }

    bool ActiveBankHasContentToOverwrite() const
    {
        if (!m_bank_token)
        {
            return false;
        }
        /* Fresh / New Session state: built-in Bank 0/1 only, never edited.
         * Token differs from a Session file's bank, but nothing user-owned is lost. */
        if (m_loaded_bank_path.empty() && !m_bank01_touched && !Bank2PlusHasInstruments() &&
            m_session_custom_snd_ids.empty() && m_session_custom_inst_ids.empty())
        {
            return false;
        }
        return Bank01HasInstruments() || Bank2PlusHasInstruments() ||
               !m_session_custom_snd_ids.empty() || !m_session_custom_inst_ids.empty() ||
               m_bank01_touched;
    }

    void CollectAllBankSndIds(std::set<uint32_t> &out_all) const
    {
        out_all.clear();
        if (!m_bank_token)
        {
            return;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        static const XResourceType kSndTypes[] = {ID_ESND, ID_CSND, ID_SND, 0};
        for (int t = 0; kSndTypes[t] != 0; ++t)
        {
            const int32_t count = XCountFileResourcesOfType(bank, kSndTypes[t]);
            for (int32_t i = 0; i < count; ++i)
            {
                XLongResourceID rid = 0;
                int32_t size = 0;
                XPTR data = XGetIndexedFileResource(bank, kSndTypes[t], &rid, i, nullptr, &size);
                if (data)
                {
                    XDisposePtr(data);
                }
                /* SND id 0 is valid (BE2 custom samples often use it). */
                out_all.insert(static_cast<uint32_t>(rid));
            }
        }
    }

    /* After Bank 0/1 instruments are removed: drop built-in SNDs not used by Bank 2+. */
    void DeleteOrphanedBuiltinBank01Samples(const std::set<uint32_t> &bank2_plus_snds)
    {
        std::set<uint32_t> all_snds;
        CollectAllBankSndIds(all_snds);
        std::set<uint32_t> protect = bank2_plus_snds;
        protect.insert(m_session_custom_snd_ids.begin(), m_session_custom_snd_ids.end());
        std::set<uint32_t> to_delete;
        for (uint32_t snd_id : all_snds)
        {
            if (protect.count(snd_id) == 0)
            {
                to_delete.insert(snd_id);
            }
        }
        DeleteUnreferencedSndResources(to_delete, protect);
    }

    /* Drop SNDs not referenced by any remaining INST. Keeps session-custom
     * unassigned samples. Catches built-in bank orphans (unused SNDs that
     * UnloadBank01 never sees because no Bank 0/1 INST referenced them). */
    void PurgeOrphanBankSamples()
    {
        if (!m_bank_token)
        {
            return;
        }
        std::set<uint32_t> referenced;
        uint32_t inst_count = 0;
        if (BAERmfEditorBank_GetInstrumentCount(m_bank_token, &inst_count) == BAE_NO_ERROR)
        {
            for (uint32_t i = 0; i < inst_count; ++i)
            {
                uint32_t split_count = 0;
                if (BAERmfEditorBank_GetInstrumentSampleCount(m_bank_token, i, &split_count) !=
                    BAE_NO_ERROR)
                {
                    continue;
                }
                for (uint32_t s = 0; s < split_count; ++s)
                {
                    BAERmfEditorBankSampleInfo sample;
                    std::memset(&sample, 0, sizeof(sample));
                    if (BAERmfEditorBank_GetInstrumentSampleInfo(m_bank_token, i, s, &sample) ==
                        BAE_NO_ERROR)
                    {
                        referenced.insert(static_cast<uint32_t>(sample.sndResourceID));
                    }
                }
            }
        }
        referenced.insert(m_session_custom_snd_ids.begin(), m_session_custom_snd_ids.end());

        std::set<uint32_t> all_snds;
        CollectAllBankSndIds(all_snds);
        std::set<uint32_t> to_delete;
        for (uint32_t snd_id : all_snds)
        {
            if (referenced.count(snd_id) == 0)
            {
                to_delete.insert(snd_id);
            }
        }
        if (!to_delete.empty())
        {
            DeleteUnreferencedSndResources(to_delete, referenced);
        }
    }

    /* SND ids referenced by one INST resource in a bank/RMF file. */
    bool CollectSndIdsForInstInFile(XFILE file,
                                    uint32_t inst_id,
                                    std::vector<XShortResourceID> &out_snd) const
    {
        out_snd.clear();
        if (!file)
        {
            return false;
        }
        int32_t size = 0;
        char name[256];
        name[0] = 0;
        XPTR data = XGetFileResource(file,
                                     ID_INST,
                                     static_cast<XLongResourceID>(inst_id),
                                     name,
                                     &size);
        if (!data || size < static_cast<int32_t>(sizeof(InstrumentResource)))
        {
            if (data)
            {
                XDisposePtr(data);
            }
            return false;
        }

        InstrumentResource *inst = static_cast<InstrumentResource *>(data);
        const int16_t split_count = XGetShort(&inst->keySplitCount);
        if (split_count <= 0)
        {
            const XShortResourceID snd =
                static_cast<XShortResourceID>(XGetShort(&inst->sndResourceID));
            if (snd != static_cast<XShortResourceID>(kNoSndResourceId))
            {
                out_snd.push_back(snd);
            }
        }
        else
        {
            std::set<XShortResourceID> seen;
            for (int16_t s = 0; s < split_count; ++s)
            {
                KeySplit split;
                std::memset(&split, 0, sizeof(split));
                XGetKeySplitFromPtrWithSize(inst, size, s, &split);
                const XShortResourceID snd = split.sndResourceID;
                if (snd != static_cast<XShortResourceID>(kNoSndResourceId) &&
                    seen.insert(snd).second)
                {
                    out_snd.push_back(snd);
                }
            }
        }
        XDisposePtr(data);
        return true;
    }

    void CollectInstAndSndIdsFromBankFile(XFILE file,
                                          std::vector<XShortResourceID> &out_inst,
                                          std::vector<XShortResourceID> &out_snd) const
    {
        out_inst.clear();
        out_snd.clear();
        std::set<XShortResourceID> snd_seen;
        const int32_t count = XCountFileResourcesOfType(file, ID_INST);
        for (int32_t i = 0; i < count; ++i)
        {
            XLongResourceID rid = 0;
            int32_t size = 0;
            char name[256];
            name[0] = 0;
            XPTR data = XGetIndexedFileResource(file, ID_INST, &rid, i, name, &size);
            if (!data || size < static_cast<int32_t>(sizeof(InstrumentResource)))
            {
                if (data)
                {
                    XDisposePtr(data);
                }
                continue;
            }
            const uint32_t bank = static_cast<uint32_t>(rid) / 256u;
            if (bank > 1u || rid >= 512)
            {
                XDisposePtr(data);
                continue;
            }
            out_inst.push_back(static_cast<XShortResourceID>(rid));

            InstrumentResource *inst = static_cast<InstrumentResource *>(data);
            const int16_t split_count = XGetShort(&inst->keySplitCount);
            if (split_count <= 0)
            {
                /* INST blob is still big-endian on disk — XGetShort once. */
                const XShortResourceID snd =
                    static_cast<XShortResourceID>(XGetShort(&inst->sndResourceID));
                if (snd != static_cast<XShortResourceID>(kNoSndResourceId) &&
                    snd_seen.insert(snd).second)
                {
                    out_snd.push_back(snd);
                }
            }
            else
            {
                for (int16_t s = 0; s < split_count; ++s)
                {
                    KeySplit split;
                    std::memset(&split, 0, sizeof(split));
                    /* XGetKeySplitFromPtrWithSize already host-endianizes fields.
                     * Do not XGetShort() again (double-swap → wrong IDs, SND copy
                     * misses, Samples list empty — e.g. NokiaRevBS8a.bsn). */
                    XGetKeySplitFromPtrWithSize(inst, size, s, &split);
                    const XShortResourceID snd = split.sndResourceID;
                    if (snd != static_cast<XShortResourceID>(kNoSndResourceId) &&
                        snd_seen.insert(snd).second)
                    {
                        out_snd.push_back(snd);
                    }
                }
            }
            XDisposePtr(data);
        }
    }

    void DeleteInstrumentsMatchingBankPredicate(bool (*bank_match)(uint16_t bank))
    {
        if (!m_bank_token || !bank_match)
        {
            return;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        std::vector<uint32_t> inst_ids;
        uint32_t inst_count = 0;
        if (BAERmfEditorBank_GetInstrumentCount(m_bank_token, &inst_count) != BAE_NO_ERROR)
        {
            return;
        }
        for (uint32_t i = 0; i < inst_count; ++i)
        {
            BAERmfEditorBankInstrumentInfo info;
            std::memset(&info, 0, sizeof(info));
            if (BAERmfEditorBank_GetInstrumentInfo(m_bank_token, i, &info) != BAE_NO_ERROR)
            {
                continue;
            }
            if (bank_match(info.bank))
            {
                inst_ids.push_back(info.instID);
            }
        }
        /* Hard purge — Unload Bank must not flood Trash (user deletes only). */
        for (uint32_t inst_id : inst_ids)
        {
            (void)XPurgeFileResource(bank, ID_INST, static_cast<XLongResourceID>(inst_id));
            m_session_custom_inst_ids.erase(inst_id);
        }

        /* Drop aliases whose display slot bank matches the predicate. */
        XAliasLinkResource *alias = XGetAliasLinkFromFile(bank);
        if (alias)
        {
            const uint32_t n = static_cast<uint32_t>(XGetLong(&alias->numberOfAliases));
            std::vector<uint32_t> alias_from;
            for (uint32_t a = 0; a < n; ++a)
            {
                const uint32_t from_id = static_cast<uint32_t>(XGetLong(&alias->list[a].aliasFrom));
                if (bank_match(static_cast<uint16_t>(from_id / 256u)))
                {
                    alias_from.push_back(from_id);
                }
            }
            XDisposePtr(reinterpret_cast<XPTR>(alias));
            for (uint32_t from_id : alias_from)
            {
                (void)BAERmfEditorBank_DeleteAlias(m_bank_token, from_id);
            }
        }
    }

    void DeleteBank01InstrumentsFromActive()
    {
        DeleteInstrumentsMatchingBankPredicate([](uint16_t bank) { return bank < 2; });
    }

    void DeleteBank2PlusInstrumentsFromActive()
    {
        DeleteInstrumentsMatchingBankPredicate([](uint16_t bank) { return bank >= 2; });
    }

    /* New Session / RMF session replace: drop Bank 2+ INST and custom/orphan SNDs; keep Bank 0/1. */
    void ClearBank2AndCustomSamples()
    {
        if (!m_bank_token)
        {
            m_session_custom_snd_ids.clear();
            m_session_custom_inst_ids.clear();
            m_session_pcm_cache.clear();
            m_app_compression_cache.clear();
            return;
        }
        if (!EnsureActiveBankWritable())
        {
            return;
        }

        std::set<uint32_t> bank01_snds;
        std::set<uint32_t> bank2_snds;
        CollectBankReferencedSndIds(bank01_snds, bank2_snds);

        DeleteBank2PlusInstrumentsFromActive();

        /* Also remove session-custom instruments that live on Bank 0/1 (RMF promote). */
        if (!m_session_custom_inst_ids.empty())
        {
            std::vector<uint32_t> indices;
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
                    if (m_session_custom_inst_ids.count(info.instID) != 0)
                    {
                        indices.push_back(i);
                    }
                }
                std::sort(indices.begin(), indices.end(), std::greater<uint32_t>());
                for (uint32_t idx : indices)
                {
                    (void)BAERmfEditorBank_DeleteInstrument(m_bank_token, idx);
                }
            }
        }

        std::set<uint32_t> candidates = bank2_snds;
        candidates.insert(m_session_custom_snd_ids.begin(), m_session_custom_snd_ids.end());
        /* Recompute Bank 0/1 refs after deletes so we don't remove shared SNDs. */
        std::set<uint32_t> bank01_after;
        std::set<uint32_t> bank2_after;
        CollectBankReferencedSndIds(bank01_after, bank2_after);
        bank01_after.insert(bank2_after.begin(), bank2_after.end());
        DeleteUnreferencedSndResources(candidates, bank01_after);

        m_session_custom_snd_ids.clear();
        m_session_custom_inst_ids.clear();
        m_session_pcm_cache.clear();
        m_app_compression_cache.clear();
        (void)XCleanResourceFile(reinterpret_cast<XFILE>(m_bank_token));
        RefreshListsFromBank();
    }

    void CollectBankReferencedSndIds(std::set<uint32_t> &out_bank01,
                                     std::set<uint32_t> &out_bank2_plus) const
    {
        out_bank01.clear();
        out_bank2_plus.clear();
        if (!m_bank_token)
        {
            return;
        }
        uint32_t inst_count = 0;
        if (BAERmfEditorBank_GetInstrumentCount(m_bank_token, &inst_count) != BAE_NO_ERROR)
        {
            return;
        }
        for (uint32_t i = 0; i < inst_count; ++i)
        {
            BAERmfEditorBankInstrumentInfo info;
            std::memset(&info, 0, sizeof(info));
            if (BAERmfEditorBank_GetInstrumentInfo(m_bank_token, i, &info) != BAE_NO_ERROR)
            {
                continue;
            }
            uint32_t split_count = 0;
            if (BAERmfEditorBank_GetInstrumentSampleCount(m_bank_token, i, &split_count) !=
                BAE_NO_ERROR)
            {
                continue;
            }
            std::set<uint32_t> &dest = (info.bank < 2) ? out_bank01 : out_bank2_plus;
            for (uint32_t s = 0; s < split_count; ++s)
            {
                BAERmfEditorBankSampleInfo sample;
                std::memset(&sample, 0, sizeof(sample));
                if (BAERmfEditorBank_GetInstrumentSampleInfo(m_bank_token, i, s, &sample) !=
                    BAE_NO_ERROR)
                {
                    continue;
                }
                /* SND id 0 is valid in BE2 session banks. */
                dest.insert(static_cast<uint32_t>(sample.sndResourceID));
            }
        }
    }

    void DeleteUnreferencedSndResources(const std::set<uint32_t> &candidates,
                                        const std::set<uint32_t> &still_referenced)
    {
        if (!m_bank_token || candidates.empty())
        {
            return;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        static const XResourceType kSndTypes[] = {ID_ESND, ID_CSND, ID_SND, 0};
        for (uint32_t snd_id : candidates)
        {
            if (still_referenced.count(snd_id) != 0)
            {
                continue;
            }
            const XLongResourceID rid = static_cast<XLongResourceID>(snd_id);
            for (int t = 0; kSndTypes[t] != 0; ++t)
            {
                if (XExistsFileResource(bank, kSndTypes[t], rid) != FALSE)
                {
                    /* Hard purge (no Trash). Keeps ZSHD/TRSH peers intact. */
                    (void)XPurgeFileResource(bank, kSndTypes[t], rid);
                    break;
                }
            }
            m_session_custom_snd_ids.erase(snd_id);
            m_session_pcm_cache.erase(snd_id);
            m_app_compression_cache.erase(snd_id);
        }
    }

    /* Same cleanup Unload Bank uses; ok if Bank 0/1 is already empty. */
    bool UnloadBank01KeepingBank2(bool quiet_if_empty = false)
    {
        if (!m_bank_token)
        {
            SetStatus("No bank loaded");
            return false;
        }
        if (!EnsureActiveBankWritable())
        {
            return false;
        }

        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        std::set<uint32_t> bank01_snds;
        std::set<uint32_t> bank2_snds;
        CollectBankReferencedSndIds(bank01_snds, bank2_snds);

        /* INST to drop: Bank 0/1 only. Keep Bank 2+ and session-custom INST
         * even if they happen to live on Bank 0/1 (RMF promote). */
        std::vector<XLongResourceID> omit_inst;
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
                if (info.bank >= 2)
                {
                    continue;
                }
                if (m_session_custom_inst_ids.count(info.instID) != 0)
                {
                    continue;
                }
                omit_inst.push_back(static_cast<XLongResourceID>(info.instID));
            }
        }

        if (omit_inst.empty() && !Bank01HasInstruments())
        {
            if (!quiet_if_empty)
            {
                SetStatus("Bank 0/1 already empty");
            }
        }

        /* SND to drop: only those referenced by Bank 0/1 INST being removed.
         * Never use (all SNDs − protect) — if Bank 2 sample collection misses
         * anything, that wiped every sample including Bank 2+. */
        std::set<uint32_t> protect = bank2_snds;
        protect.insert(m_session_custom_snd_ids.begin(), m_session_custom_snd_ids.end());
        for (uint32_t custom_id : m_session_custom_inst_ids)
        {
            uint32_t resolved = 0;
            uint32_t index = 0;
            if (BAERmfEditorBank_ResolveInstID(m_bank_token, custom_id, &resolved, &index) !=
                BAE_NO_ERROR)
            {
                continue;
            }
            uint32_t split_count = 0;
            if (BAERmfEditorBank_GetInstrumentSampleCount(m_bank_token, index, &split_count) !=
                BAE_NO_ERROR)
            {
                continue;
            }
            for (uint32_t s = 0; s < split_count; ++s)
            {
                BAERmfEditorBankSampleInfo sinfo;
                std::memset(&sinfo, 0, sizeof(sinfo));
                if (BAERmfEditorBank_GetInstrumentSampleInfo(m_bank_token, index, s, &sinfo) ==
                    BAE_NO_ERROR)
                {
                    protect.insert(static_cast<uint32_t>(sinfo.sndResourceID));
                }
            }
        }

        std::vector<XLongResourceID> omit_snd;
        for (uint32_t snd_id : bank01_snds)
        {
            if (protect.count(snd_id) == 0)
            {
                omit_snd.push_back(static_cast<XLongResourceID>(snd_id));
            }
        }

        if (omit_inst.empty() && omit_snd.empty())
        {
            m_bank01_touched = false;
            RefreshListsFromBank();
            return true;
        }

        /* INST first (ZINS filter, no expand). Then SND-only omit so a sample
         * purge cannot touch the instrument block. */
        if (!omit_inst.empty())
        {
            if (XPurgeFileInstrumentAndSoundLists(bank,
                                                  omit_inst.data(),
                                                  static_cast<int32_t>(omit_inst.size()),
                                                  nullptr,
                                                  0) == FALSE)
            {
                SetStatus("Unload Bank 0/1 failed (instruments)");
                return false;
            }
        }
        if (!omit_snd.empty())
        {
            if (XPurgeFileInstrumentAndSoundLists(bank,
                                                  nullptr,
                                                  0,
                                                  omit_snd.data(),
                                                  static_cast<int32_t>(omit_snd.size())) == FALSE)
            {
                SetStatus("Unload Bank 0/1 failed (samples)");
                return false;
            }
        }

        /* Drop Bank 0/1 aliases only (not Bank 2+). */
        {
            XAliasLinkResource *alias = XGetAliasLinkFromFile(bank);
            if (alias)
            {
                const uint32_t n = static_cast<uint32_t>(XGetLong(&alias->numberOfAliases));
                std::vector<uint32_t> alias_from;
                for (uint32_t a = 0; a < n; ++a)
                {
                    const uint32_t from_id =
                        static_cast<uint32_t>(XGetLong(&alias->list[a].aliasFrom));
                    if ((from_id / 256u) < 2u &&
                        m_session_custom_inst_ids.count(from_id) == 0)
                    {
                        alias_from.push_back(from_id);
                    }
                }
                XDisposePtr(reinterpret_cast<XPTR>(alias));
                for (uint32_t from_id : alias_from)
                {
                    (void)BAERmfEditorBank_DeleteAlias(m_bank_token, from_id);
                }
            }
        }

        /* Verify Bank 2+ / custom INST and samples survived before refreshing UI. */
        {
            std::vector<uint32_t> must_keep_inst;
            for (const InstrumentRow &row : m_instruments)
            {
                if (row.is_alias)
                {
                    continue;
                }
                if (row.bank >= 2 || m_session_custom_inst_ids.count(row.inst_id) != 0)
                {
                    must_keep_inst.push_back(row.inst_id);
                }
            }
            for (uint32_t id : must_keep_inst)
            {
                int32_t sz = 0;
                char name[256];
                name[0] = 0;
                XPTR data = XGetFileResource(bank, ID_INST, static_cast<XLongResourceID>(id), name, &sz);
                if (!data)
                {
                    SetStatus("Unload Bank 0/1 failed: Bank 2+/custom instrument missing after purge");
                    return false;
                }
                XDisposePtr(data);
            }
            std::set<uint32_t> must_keep_snd = bank2_snds;
            must_keep_snd.insert(m_session_custom_snd_ids.begin(), m_session_custom_snd_ids.end());
            for (uint32_t snd_id : must_keep_snd)
            {
                if (!BankSndResourceExists(snd_id))
                {
                    SetStatus("Unload Bank 0/1 failed: Bank 2+/custom sample missing after purge");
                    return false;
                }
            }
        }

        for (XLongResourceID id : omit_inst)
        {
            m_session_custom_inst_ids.erase(static_cast<uint32_t>(id));
        }
        for (XLongResourceID id : omit_snd)
        {
            m_session_custom_snd_ids.erase(static_cast<uint32_t>(id));
            m_session_pcm_cache.erase(static_cast<uint32_t>(id));
            m_app_compression_cache.erase(static_cast<uint32_t>(id));
        }

        m_bank01_touched = false;
        RefreshListsFromBank();
        SetStatus("Unloaded Bank 0/1 (Bank 2+ kept)");
        return true;
    }

    bool MergeBank01FromDonor(XFILE donor, const char *source_label)
    {
        if (!donor || !m_bank_token)
        {
            SetStatus("Bank merge requires an active session bank");
            return false;
        }
        if (!EnsureActiveBankWritable())
        {
            return false;
        }

        std::vector<XShortResourceID> inst_ids;
        std::vector<XShortResourceID> snd_ids;
        CollectInstAndSndIdsFromBankFile(donor, inst_ids, snd_ids);
        if (inst_ids.empty())
        {
            SetStatus("Donor bank has no Bank 0/1 instruments to import");
            return false;
        }

        /* Safe unload of current Bank 0/1 (same as Unload Bank menu). */
        if (!UnloadBank01KeepingBank2(true))
        {
            return false;
        }

        XFILE dest = reinterpret_cast<XFILE>(m_bank_token);
        if (!snd_ids.empty())
        {
            (void)XCopySndResources(snd_ids.data(),
                                    static_cast<int16_t>(snd_ids.size()),
                                    donor,
                                    dest,
                                    FALSE,
                                    TRUE);
        }
        (void)XCopyInstrumentResources(inst_ids.data(),
                                       static_cast<int16_t>(inst_ids.size()),
                                       donor,
                                       dest,
                                       TRUE);
        /* INST/SND copy does not touch ALIS — merge Bank 0/1 aliases (BE2 does). */
        const size_t alias_count = MergeBank01AliasesFromDonor(donor, dest);
        (void)XCleanResourceFile(dest);

        /* Bank-mode load only merges instruments — drop leftover session/built-in
         * groovoids so the Songs list matches the instrument bank being loaded. */
        (void)StripBankGroovoids(dest, nullptr);
        /* Built-in patches ship unused SNDs; UnloadBank01 only drops samples that
         * were referenced by removed INST — purge true orphans here. */
        PurgeOrphanBankSamples();

        m_bank01_touched = false;
        EnableEditorBankZsCompatibility(m_bank_token);
        RefreshListsFromBank();
        char status[256];
        std::snprintf(status,
                      sizeof(status),
                      "Loaded Bank 0/1 from %s (%zu instruments, %zu aliases, Bank 2+ kept)",
                      source_label && source_label[0] ? source_label : "bank",
                      inst_ids.size(),
                      alias_count);
        SetStatus(status);
        return true;
    }

    /* Append donor Bank 0/1 ALIS entries onto dest (which still has Bank 2+ aliases
     * after UnloadBank01). Returns number of donor aliases imported. */
    size_t MergeBank01AliasesFromDonor(XFILE donor, XFILE dest)
    {
        if (!donor || !dest)
        {
            return 0;
        }

        XAliasLinkResource *donor_alias = XGetAliasLinkFromFile(donor);
        if (!donor_alias)
        {
            return 0;
        }

        struct AliasPair
        {
            uint32_t from_id;
            uint32_t to_id;
        };
        std::vector<AliasPair> merged;
        std::set<uint32_t> from_seen;

        XAliasLinkResource *dest_alias = XGetAliasLinkFromFile(dest);
        if (dest_alias)
        {
            const uint32_t n = static_cast<uint32_t>(XGetLong(&dest_alias->numberOfAliases));
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t from_id =
                    static_cast<uint32_t>(XGetLong(&dest_alias->list[i].aliasFrom));
                const uint32_t to_id =
                    static_cast<uint32_t>(XGetLong(&dest_alias->list[i].aliasTo));
                if (from_seen.insert(from_id).second)
                {
                    merged.push_back({from_id, to_id});
                }
            }
            XDisposePtr(reinterpret_cast<XPTR>(dest_alias));
        }

        size_t imported = 0;
        const uint32_t donor_n =
            static_cast<uint32_t>(XGetLong(&donor_alias->numberOfAliases));
        for (uint32_t i = 0; i < donor_n; ++i)
        {
            const uint32_t from_id =
                static_cast<uint32_t>(XGetLong(&donor_alias->list[i].aliasFrom));
            const uint32_t to_id =
                static_cast<uint32_t>(XGetLong(&donor_alias->list[i].aliasTo));
            /* Same Bank 0/1 slot rule as UnloadBank01 alias deletion. */
            if ((from_id / 256u) >= 2u)
            {
                continue;
            }
            if (!from_seen.insert(from_id).second)
            {
                continue;
            }
            merged.push_back({from_id, to_id});
            ++imported;
        }
        XDisposePtr(reinterpret_cast<XPTR>(donor_alias));

        if (imported == 0 && merged.empty())
        {
            return 0;
        }

        if (merged.empty())
        {
            (void)XDeleteFileResource(dest, ID_ALIAS, DEFAULT_RESOURCE_ALIAS_ID, FALSE);
            return 0;
        }

        const int32_t new_size = static_cast<int32_t>(sizeof(uint32_t) * 2 +
                                                     merged.size() * sizeof(XAliasLink));
        XAliasLinkResource *new_alias =
            static_cast<XAliasLinkResource *>(XNewPtr(new_size));
        if (!new_alias)
        {
            return 0;
        }
        XPutLong(&new_alias->version, ALIAS_ID_RESOURCE_VERSION);
        XPutLong(&new_alias->numberOfAliases, static_cast<uint32_t>(merged.size()));
        for (size_t i = 0; i < merged.size(); ++i)
        {
            XPutLong(&new_alias->list[i].aliasFrom, merged[i].from_id);
            XPutLong(&new_alias->list[i].aliasTo, merged[i].to_id);
        }

        (void)XDeleteFileResource(dest, ID_ALIAS, DEFAULT_RESOURCE_ALIAS_ID, FALSE);
        char empty_name[1] = {0};
        if (XAddFileResource(dest,
                             ID_ALIAS,
                             DEFAULT_RESOURCE_ALIAS_ID,
                             empty_name,
                             new_alias,
                             new_size) != 0)
        {
            imported = 0;
        }
        XDisposePtr(reinterpret_cast<XPTR>(new_alias));
        return imported;
    }

    /* Collect every INST + referenced SND from a resource file (any bank). */
    /* INST ids from donor INST resources; SND ids only from embedded
     * esnd/csnd/snd resources (never from INST references alone).
     * INST may bank-alias SNDs that are not in the file — those must not be
     * deleted from the session bank during promote. Use indexed enumeration
     * rather than XExistsFileResource (uncached Exists is unreliable). */
    void CollectAllInstAndSndIdsFromFile(XFILE file,
                                         std::vector<XShortResourceID> &out_inst,
                                         std::vector<XShortResourceID> &out_snd) const
    {
        out_inst.clear();
        out_snd.clear();
        if (!file)
        {
            return;
        }
        std::set<XShortResourceID> snd_seen;

        const int32_t count = XCountFileResourcesOfType(file, ID_INST);
        for (int32_t i = 0; i < count; ++i)
        {
            XLongResourceID rid = 0;
            int32_t size = 0;
            char name[256];
            name[0] = 0;
            XPTR data = XGetIndexedFileResource(file, ID_INST, &rid, i, name, &size);
            if (data)
            {
                XDisposePtr(data);
            }
            if (rid > 32767)
            {
                continue;
            }
            out_inst.push_back(static_cast<XShortResourceID>(rid));
        }

        static const XResourceType kSndTypes[] = {ID_ESND, ID_CSND, ID_SND, 0};
        for (int t = 0; kSndTypes[t] != 0; ++t)
        {
            const int32_t snd_count = XCountFileResourcesOfType(file, kSndTypes[t]);
            for (int32_t i = 0; i < snd_count; ++i)
            {
                XLongResourceID rid = 0;
                int32_t size = 0;
                XPTR data = XGetIndexedFileResource(file, kSndTypes[t], &rid, i, nullptr, &size);
                if (data)
                {
                    XDisposePtr(data);
                }
                if (rid <= 32767 && snd_seen.insert(static_cast<XShortResourceID>(rid)).second)
                {
                    out_snd.push_back(static_cast<XShortResourceID>(rid));
                }
            }
        }
    }

    /* Promote RMF/ZMF embedded INST/SND into the active bank (IDs preserved; marked custom). */
    bool PromoteEmbeddedResourcesFromRmfXFile(XFILE donor)
    {
        if (!donor)
        {
            return false;
        }
        if (!m_bank_token)
        {
            if (!LoadBuiltinBankAsActive())
            {
                return false;
            }
        }
        if (!EnsureActiveBankWritable())
        {
            return false;
        }

        std::vector<XShortResourceID> inst_ids;
        std::vector<XShortResourceID> snd_ids;
        CollectAllInstAndSndIdsFromFile(donor, inst_ids, snd_ids);
        if (inst_ids.empty() && snd_ids.empty())
        {
            return true; /* song-only RMF; nothing to promote */
        }

        bool all_already_present = true;
        for (XShortResourceID id : inst_ids)
        {
            if (m_session_custom_inst_ids.count(static_cast<uint32_t>(id)) == 0)
            {
                all_already_present = false;
                break;
            }
        }
        if (all_already_present)
        {
            for (XShortResourceID id : snd_ids)
            {
                if (m_session_custom_snd_ids.count(static_cast<uint32_t>(id)) == 0)
                {
                    all_already_present = false;
                    break;
                }
            }
        }
        if (all_already_present)
        {
            return true; /* All embedded resources already promoted into bank */
        }

        /* XDeleteFileResource is a no-op on in-memory (EnsureWritable) banks, so
         * delete+XAddFileResource would append duplicates (EndGame bank0 INST
         * listed twice). Import rebuilds the bank image in one pass. */
        const BAEResult import_r = BAERmfEditorBank_ImportInstAndSndFromFile(
            m_bank_token,
            donor,
            inst_ids.empty() ? nullptr : inst_ids.data(),
            static_cast<uint32_t>(inst_ids.size()),
            snd_ids.empty() ? nullptr : snd_ids.data(),
            static_cast<uint32_t>(snd_ids.size()));
        if (import_r != BAE_NO_ERROR)
        {
            SetStatus(std::string("RMF promote failed: ") + FormatBAEError(import_r));
            return false;
        }

        for (XShortResourceID snd_id : snd_ids)
        {
            m_session_custom_snd_ids.insert(static_cast<uint32_t>(snd_id));
        }
        for (XShortResourceID inst_id : inst_ids)
        {
            m_session_custom_inst_ids.insert(static_cast<uint32_t>(inst_id));
            if ((static_cast<uint32_t>(inst_id) / 256u) < 2u)
            {
                m_bank01_touched = true;
            }
        }
        RefreshListsFromBank();
        return true;
    }

    bool PromoteEmbeddedResourcesFromRmfPath(const char *path)
    {
        if (!path || !path[0])
        {
            return false;
        }
        XFILENAME xname;
        XConvertPathToXFILENAME(const_cast<char *>(path), &xname);
        XFILE donor = XFileOpenResource(&xname, TRUE);
        if (!donor)
        {
            SetStatus("Failed to open RMF for instrument promote");
            return false;
        }
        const bool ok = PromoteEmbeddedResourcesFromRmfXFile(donor);
        XFileClose(donor);
        return ok;
    }

    bool PromoteEmbeddedResourcesFromRmfMemory(const void *data, uint32_t size)
    {
        if (!data || size == 0)
        {
            return false;
        }
        /* allowCopy=TRUE so the memory XFILE owns a private image. */
        XFILE donor = XFileOpenResourceFromMemory((XPTR)const_cast<void *>(data), size, TRUE);
        if (!donor)
        {
            return false;
        }
        const bool ok = PromoteEmbeddedResourcesFromRmfXFile(donor);
        XFileClose(donor);
        return ok;
    }

    /* Modules/RMF songs keep INST/SND in the document until promoted. Session
     * save only persists the bank body + Midi/SONG — without promote, S3M/XM/IT
     * customs vanish on save→reload. */
    bool PromoteAllSessionSongEmbedsIntoBank()
    {
        bool any = false;
        for (const SessionSongEntry &entry : m_session_songs)
        {
            if (entry.rmf_bytes.empty())
            {
                continue;
            }
            if (PromoteEmbeddedResourcesFromRmfMemory(
                    entry.rmf_bytes.data(),
                    static_cast<uint32_t>(entry.rmf_bytes.size())))
            {
                any = true;
            }
        }
        return any;
    }

    bool PromoteActiveSessionSongEmbedsIntoBank()
    {
        if (m_session_song_index < 0 ||
            m_session_song_index >= static_cast<int>(m_session_songs.size()))
        {
            return false;
        }
        const SessionSongEntry &entry =
            m_session_songs[static_cast<size_t>(m_session_song_index)];
        if (entry.rmf_bytes.empty())
        {
            return false;
        }
        return PromoteEmbeddedResourcesFromRmfMemory(
            entry.rmf_bytes.data(),
            static_cast<uint32_t>(entry.rmf_bytes.size()));
    }

    bool MergeBank01FromPath(const char *path)
    {
        if (!path || !path[0] || !m_bank_token)
        {
            SetStatus("Bank merge requires an active session bank");
            return false;
        }

        XFILENAME xname;
        XConvertPathToXFILENAME(const_cast<char *>(path), &xname);
        XFILE donor = XFileOpenResource(&xname, TRUE);
        if (!donor)
        {
            SetStatus("Failed to open bank file for merge");
            return false;
        }

        const bool ok = MergeBank01FromDonor(donor, FileNameFromPath(path).c_str());
        XFileClose(donor);
        return ok;
    }

    bool MergeBank01FromBuiltin()
    {
#if _BUILT_IN_PATCHES == TRUE
        if (!m_mixer || !m_bank_token)
        {
            SetStatus("Bank merge requires an active session bank");
            return false;
        }
        /* Load built-in as a temporary second bank, copy Bank 0/1, then unload it. */
        BAEBankToken donor_token = 0;
        if (!LoadBuiltinBankToMixer(m_mixer, &donor_token) || donor_token == 0)
        {
            SetStatus("Built-in bank unavailable");
            return false;
        }
        const bool ok =
            MergeBank01FromDonor(reinterpret_cast<XFILE>(donor_token), "built-in bank");
        (void)BAEMixer_UnloadBank(m_mixer, donor_token);
        return ok;
#else
        SetStatus("Built-in bank unavailable");
        return false;
#endif
    }

    void RequestBank01MergeConfirm(const std::string &path)
    {
        m_pending_bank_merge_path = path;
        m_confirm_bank_merge_open = true;
    }

    void DrawBankMergeConfirmDialog()
    {
        if (m_confirm_bank_merge_open)
        {
            ImGui::OpenPopup("Replace Bank 0/1");
        }
        if (!ImGui::BeginPopupModal("Replace Bank 0/1",
                                    &m_confirm_bank_merge_open,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }
        ImGui::TextWrapped(
            "Replace Bank 0 and Bank 1 from:\n%s",
            m_pending_bank_merge_path.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled(
            "Uses the same safe unload as File → Unload Bank. Bank 2+ is kept.");
        if (m_bank01_touched)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                               "Bank 0/1 have been edited in this session.");
        }
        ImGui::Separator();
        if (ImGui::Button("Replace Bank 0/1", ImVec2(160.0f, 0.0f)))
        {
            LoadBankFromPath(m_pending_bank_merge_path.c_str(),
                             ShouldPreserveBank2OnBankLoad());
            m_confirm_bank_merge_open = false;
            m_pending_bank_merge_path.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
        {
            m_confirm_bank_merge_open = false;
            m_pending_bank_merge_path.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* Songs-tab / Player / Status drops: append songs; never File→Open session wipe for RMF. */
    void HandleSongsTabDrop(const char *path)
    {
        if (!path || !path[0])
        {
            return;
        }
        std::string lower = ToLower(path);
        std::string ext;
        const size_t dot = lower.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < lower.size())
        {
            ext = lower.substr(dot + 1);
        }

        if (ext == "bsn" || ext == "zsn")
        {
            /* Session documents still open/replace via the normal confirm path. */
            ImportPathIntoSession(path);
            return;
        }
        if (IsBankExtension(ext) && !IsSongImportExtension(ext))
        {
            SetStatus("Drop bank files on the Instruments tab");
            return;
        }
        if (IsAudioSampleExtension(ext) && !IsSongImportExtension(ext))
        {
            SetStatus("Drop audio samples on the Samples tab");
            return;
        }
        if (IsModuleExtension(ext))
        {
#if defined(LIBXMP_STATIC) || defined(LIBXMP_CORE_PLAYER)
            (void)CommitActiveSessionSong();
            if (LoadModuleFromPath(path))
            {
                m_session_song_index = -1;
                if (!SyncSessionSongsFromDocument())
                {
                    SetStatus("Module loaded but failed to add to Session song list");
                }
                else
                {
                    (void)PromoteActiveSessionSongEmbedsIntoBank();
                    char status[256];
                    std::snprintf(status,
                                  sizeof(status),
                                  "Added Session song %d/%zu (module, %zu custom INST)",
                                  m_session_song_index + 1,
                                  m_session_songs.size(),
                                  m_session_custom_inst_ids.size());
                    SetStatus(status);
                }
            }
#else
            SetStatus("Tracker module support not enabled in this build");
#endif
            return;
        }
        if (ext == "mid" || ext == "midi" || ext == "kar" || ext == "rmi")
        {
            (void)ImportSongDocumentAsNewSessionEntry(path, "MIDI");
            return;
        }
        if (ext == "rmf" || ext == "zmf")
        {
            /* Same chooser as File → Open / any non-Instruments/Samples drop. */
            ImportPathIntoSession(path);
            return;
        }
        SetStatus("Songs tab accepts MIDI / RMF / ZMF / modules / Session files");
    }

    void HandleDroppedPathInternal(const char *path)
    {
        if (!path || !path[0])
        {
            return;
        }
        std::string lower = ToLower(path);
        std::string ext;
        const size_t dot = lower.find_last_of('.');
        if (dot != std::string::npos && dot + 1 < lower.size())
        {
            ext = lower.substr(dot + 1);
        }

        /* Song-like files: import into Session unless dropped on Instruments/Samples.
         * Includes hover==None (OS drag often loses ImGui hover before DROP_FILE). */
        if (IsSongImportExtension(ext) &&
            m_dnd_hover_target != SessionDropTarget::Instruments &&
            m_dnd_hover_target != SessionDropTarget::Samples)
        {
            HandleSongsTabDrop(path);
            return;
        }

        /* Route by hovered window/tab — not by whichever Session tab happens to be selected. */
        switch (m_dnd_hover_target)
        {
        case SessionDropTarget::None:
            SetStatus("Drop on Session tabs, Player, or Status");
            return;

        case SessionDropTarget::Songs:
        case SessionDropTarget::FileOpen:
            HandleSongsTabDrop(path);
            return;

        case SessionDropTarget::Samples:
            if (!IsAudioSampleExtension(ext))
            {
                SetStatus("Samples tab accepts WAV/AIFF/MP3/OGG/FLAC/Opus");
                return;
            }
            BeginAddSampleFromPath(path);
            return;

        case SessionDropTarget::Instruments:
            if (!IsBankExtension(ext) && ext != "hsb" && ext != "zsb")
            {
                /* .bsn session-with-bank still OK as donor */
                if (!(ext == "bsn" || ext == "zsn"))
                {
                    SetStatus("Instruments tab accepts bank files (.hsb/.zsb/.bsn)");
                    return;
                }
            }
            if (!m_bank_token)
            {
                /* No session bank yet — full load. */
                LoadBankFromPath(path, false);
                return;
            }
            if (Bank01HasInstruments() || m_bank01_touched)
            {
                RequestBank01MergeConfirm(path);
            }
            else
            {
                LoadBankFromPath(path, ShouldPreserveBank2OnBankLoad());
            }
            return;
        }
    }
