/* Add Instrument / Add Sample / Make Instrument Using — included inside NbEditorApp. */

    enum class BankAddMode : int
    {
        Instrument = 0,       /* empty INST */
        MakeInstrument = 1,   /* INST from existing unassigned SND */
        Alias = 2,            /* alias existing INST into a free slot */
    };

    static uint32_t BankInstIdFromSlot(int bank, bool percussion, int program)
    {
        if (bank < 0)
        {
            bank = 0;
        }
        if (program < 0)
        {
            program = 0;
        }
        if (program > 127)
        {
            program = 127;
        }
        return (static_cast<uint32_t>(bank) * 256u) +
               (percussion ? 128u : 0u) +
               static_cast<uint32_t>(program);
    }

    static void DecodeInstSlot(uint32_t inst_id, int *out_bank, bool *out_percussion, int *out_program)
    {
        const int bank = static_cast<int>(inst_id / 256u);
        const uint32_t within = inst_id % 256u;
        const bool percussion = (within >= 128u);
        const int program = static_cast<int>(within % 128u);
        if (out_bank)
        {
            *out_bank = bank;
        }
        if (out_percussion)
        {
            *out_percussion = percussion;
        }
        if (out_program)
        {
            *out_program = program;
        }
    }

    static void MakeResourcePascalName(const char *c_name, char out_pascal[256])
    {
        if (!out_pascal)
        {
            return;
        }
        if (!c_name || !c_name[0])
        {
            out_pascal[0] = 0;
            return;
        }
        size_t len = std::strlen(c_name);
        if (len > 255)
        {
            len = 255;
        }
        out_pascal[0] = static_cast<char>(len);
        std::memcpy(out_pascal + 1, c_name, len);
    }

    static AudioFileType AudioFileTypeFromPath(const char *path)
    {
        if (!path || !path[0])
        {
            return FILE_INVALID_TYPE;
        }
        if (EndsWith(path, ".wav"))
        {
            return FILE_WAVE_TYPE;
        }
        if (EndsWith(path, ".aif") || EndsWith(path, ".aiff"))
        {
            return FILE_AIFF_TYPE;
        }
#if USE_MPEG_DECODER == TRUE || USE_MPEG_ENCODER == TRUE
        if (EndsWith(path, ".mp3") || EndsWith(path, ".mpg") || EndsWith(path, ".mpeg"))
        {
            return FILE_MPEG_TYPE;
        }
#endif
#if USE_FLAC_DECODER == TRUE || USE_FLAC_ENCODER == TRUE
        if (EndsWith(path, ".flac"))
        {
            return FILE_FLAC_TYPE;
        }
#endif
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
        if (EndsWith(path, ".ogg") || EndsWith(path, ".oga"))
        {
            return FILE_VORBIS_TYPE;
        }
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        if (EndsWith(path, ".opus"))
        {
            return FILE_OPUS_TYPE;
        }
#endif
#if USE_QOA_SUPPORT == TRUE
        if (EndsWith(path, ".qoa"))
        {
            return FILE_QOA_TYPE;
        }
#endif
        return FILE_INVALID_TYPE;
    }

    bool EnsureActiveBankWritable()
    {
        if (!m_bank_token)
        {
            SetStatus("No bank loaded");
            return false;
        }
        const BAEResult r = BAERmfEditorBank_EnsureWritable(m_bank_token);
        if (r != BAE_NO_ERROR)
        {
            SetStatus(std::string("Bank not writable: ") + FormatBAEError(r));
            return false;
        }
        return true;
    }

    void CollectUsedProgramsForGroup(int bank, bool percussion, bool used_programs[128]) const
    {
        std::memset(used_programs, 0, 128 * sizeof(bool));
        if (!m_bank_token)
        {
            return;
        }
        const uint32_t base = BankInstIdFromSlot(bank, percussion, 0);
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
            if (info.instID >= base && info.instID < base + 128u)
            {
                used_programs[info.instID - base] = true;
            }
        }
        XAliasLinkResource *alias =
            XGetAliasLinkFromFile(reinterpret_cast<XFILE>(m_bank_token));
        if (alias)
        {
            const uint32_t n = static_cast<uint32_t>(XGetLong(&alias->numberOfAliases));
            for (uint32_t a = 0; a < n; ++a)
            {
                const uint32_t from_id = static_cast<uint32_t>(XGetLong(&alias->list[a].aliasFrom));
                if (from_id >= base && from_id < base + 128u)
                {
                    used_programs[from_id - base] = true;
                }
            }
            XDisposePtr(reinterpret_cast<XPTR>(alias));
        }
    }

    int FirstFreeProgramForGroup(int bank, bool percussion) const
    {
        bool used[128];
        CollectUsedProgramsForGroup(bank, percussion, used);
        for (int p = 0; p < 128; ++p)
        {
            if (!used[p])
            {
                return p;
            }
        }
        return -1;
    }

    void InitBankAddDialogDefaults()
    {
        int bank = 2;
        bool percussion = false;
        if (m_instrument_filter_index >= 0 &&
            m_instrument_filter_index < static_cast<int>(m_instrument_filters.size()))
        {
            const InstrumentFilterOption &opt =
                m_instrument_filters[static_cast<size_t>(m_instrument_filter_index)];
            if (opt.bank >= 0)
            {
                bank = opt.bank;
            }
            if (opt.category == 1)
            {
                percussion = true;
            }
            else if (opt.category == 0)
            {
                percussion = false;
            }
        }
        m_bank_add_bank = bank;
        m_bank_add_percussion = percussion;
        m_bank_add_root_key = 60;
        if (m_selected_empty_slot && m_selected_program >= 0 && m_selected_program < 128)
        {
            m_bank_add_bank = m_selected_bank;
            m_bank_add_program = m_selected_program;
            if (m_instrument_filter_index >= 0 &&
                m_instrument_filter_index < static_cast<int>(m_instrument_filters.size()) &&
                m_instrument_filters[static_cast<size_t>(m_instrument_filter_index)].category >= 0)
            {
                m_bank_add_percussion =
                    (m_instrument_filters[static_cast<size_t>(m_instrument_filter_index)].category ==
                     1);
            }
            return;
        }
        const int free_prog = FirstFreeProgramForGroup(bank, percussion);
        m_bank_add_program = free_prog >= 0 ? free_prog : 0;
    }

    void OpenAddInstrumentDialog()
    {
        if (!m_bank_token)
        {
            SetStatus("No bank loaded — load a bank or open a session first");
            return;
        }
        m_bank_add_mode = BankAddMode::Instrument;
        m_bank_add_snd_id = kNoSndResourceId;
        m_bank_add_alias_source_index = -1;
        for (size_t i = 0; i < m_instruments.size(); ++i)
        {
            if (!m_instruments[i].is_alias && !m_instruments[i].from_song_document)
            {
                m_bank_add_alias_source_index = static_cast<int>(i);
                break;
            }
        }
        InitBankAddDialogDefaults();
        std::snprintf(m_bank_add_name, sizeof(m_bank_add_name), "Empty Instrument");
        m_bank_add_open = true;
    }

    void OpenAddSampleFileDialog()
    {
        if (!m_bank_token)
        {
            SetStatus("No bank loaded — load a bank or open a session first");
            return;
        }
        m_pending_dialog_action = DialogAction::AddSample;
        static const SDL_DialogFileFilter filters[] = {
            {"Audio files (*.wav;*.aif;*.aiff;*.mp3;*.ogg;*.flac;*.opus)",
             "wav;aif;aiff;mp3;ogg;flac;opus"},
            {"WAV (*.wav)", "wav"},
            {"AIFF (*.aif;*.aiff)", "aif;aiff"},
            {"All files (*.*)", "*"},
        };
        SDL_ShowOpenFileDialog(OnFileDialogResult,
                               this,
                               m_main_window,
                               filters,
                               static_cast<int>(SDL_arraysize(filters)),
                               nullptr,
                               false);
    }

    bool AllocateUniqueSndId(XFILE bank_file,
                             XLongResourceID *out_id,
                             const std::set<XLongResourceID> *also_reserved = nullptr)
    {
        if (!bank_file || !out_id)
        {
            return false;
        }
        *out_id = 0;

        /* Collect IDs used by ANY sample container type. ESND-only unique IDs can
         * collide with existing SND/CSND (e.g. Steel Drum) and look like a reuse.
         * SND ID 0 is valid and must be treated as occupied when present. */
        static const XResourceType kTypes[] = {ID_ESND, ID_SND, ID_CSND, 0};
        std::set<XLongResourceID> used;
        XLongResourceID max_id = -1;
        for (int t = 0; kTypes[t] != 0; ++t)
        {
            const int32_t count = XCountFileResourcesOfType(bank_file, kTypes[t]);
            for (int32_t i = 0; i < count; ++i)
            {
                XLongResourceID rid = 0;
                int32_t size = 0;
                char name[256];
                name[0] = 0;
                XPTR data = XGetIndexedFileResource(bank_file, kTypes[t], &rid, i, name, &size);
                if (data)
                {
                    XDisposePtr(data);
                }
                if (rid >= 0 && rid <= 0x7FFF)
                {
                    used.insert(rid);
                    if (rid > max_id)
                    {
                        max_id = rid;
                    }
                }
            }
        }
        if (also_reserved)
        {
            for (XLongResourceID rid : *also_reserved)
            {
                if (rid >= 0 && rid <= 0x7FFF)
                {
                    used.insert(rid);
                    if (rid > max_id)
                    {
                        max_id = rid;
                    }
                }
            }
        }

        /* Prefer max+1 when it stays in the classic 16-bit INST/SND range. */
        if (max_id >= 0 && max_id < 0x7FFF && used.count(max_id + 1) == 0)
        {
            *out_id = max_id + 1;
            return true;
        }

        /* Otherwise scan for the first free ID (0 is valid but prefer 1+ for new). */
        for (XLongResourceID candidate = 1; candidate <= 0x7FFF; ++candidate)
        {
            if (used.count(candidate) == 0)
            {
                *out_id = candidate;
                return true;
            }
        }
        if (used.count(0) == 0)
        {
            *out_id = 0;
            return true;
        }
        return false;
    }

    bool AddOrphanSampleFromFile(const char *file_path)
    {
        if (!m_bank_token || !file_path || !file_path[0])
        {
            return false;
        }
        if (!EnsureActiveBankWritable())
        {
            return false;
        }

        const AudioFileType audio_type = AudioFileTypeFromPath(file_path);
        if (audio_type == FILE_INVALID_TYPE)
        {
            SetStatus("Unsupported audio type (use WAV/AIFF/MP3/Ogg/FLAC/Opus)");
            return false;
        }

        XFILENAME xf_in;
        XConvertPathToXFILENAME(const_cast<char *>(file_path), &xf_in);
        OPErr op_err = NO_ERR;
        GM_Waveform *waveform = GM_ReadFileIntoMemory(&xf_in, audio_type, TRUE, &op_err);
        if (!waveform || op_err != NO_ERR)
        {
            if (waveform)
            {
                GM_FreeWaveform(waveform);
            }
            SetStatus("Failed to read audio file");
            return false;
        }

        XPTR snd_obj = nullptr;
        const OPErr create_err =
            XCreateSoundObjectFromData(&snd_obj, waveform, C_NONE, CS_DEFAULT, nullptr, nullptr);
        const unsigned char base_key = waveform->baseMidiPitch ? waveform->baseMidiPitch : 60;
        GM_FreeWaveform(waveform);
        if (create_err != NO_ERR || !snd_obj)
        {
            if (snd_obj)
            {
                XDisposePtr(snd_obj);
            }
            SetStatus("Failed to build sample resource");
            return false;
        }

        XFILE bank_file = reinterpret_cast<XFILE>(m_bank_token);
        XLongResourceID snd_id = 0;
        if (!AllocateUniqueSndId(bank_file, &snd_id))
        {
            XDisposePtr(snd_obj);
            SetStatus("Failed to allocate sample ID");
            return false;
        }

        std::string leaf = FileNameFromPath(file_path);
        const size_t dot = leaf.find_last_of('.');
        if (dot != std::string::npos)
        {
            leaf = leaf.substr(0, dot);
        }
        if (leaf.empty())
        {
            leaf = "Sample";
        }

        char pascal_name[256];
        MakeResourcePascalName(leaf.c_str(), pascal_name);
        const int32_t snd_size = XGetPtrSize(snd_obj);
        /* ESND containers must be encrypted; plaintext here makes decode/paste fail. */
        XEncryptData(snd_obj, static_cast<uint32_t>(snd_size));
        if (XAddFileResource(bank_file,
                             ID_ESND,
                             snd_id,
                             pascal_name,
                             snd_obj,
                             snd_size) != 0)
        {
            XDisposePtr(snd_obj);
            SetStatus("Failed to add sample to bank");
            return false;
        }
        XDisposePtr(snd_obj);

        m_session_custom_snd_ids.insert(static_cast<uint32_t>(snd_id));
        RefreshListsFromBank();
        m_sample_show_filter = SampleShowFilter::Custom;
        for (size_t i = 0; i < m_samples.size(); ++i)
        {
            if (m_samples[i].unassigned && m_samples[i].snd_resource_id == static_cast<uint32_t>(snd_id))
            {
                m_selected_sample = static_cast<int>(i);
                break;
            }
        }

        char status[256];
        std::snprintf(status,
                      sizeof(status),
                      "Added unassigned sample \"%s\" (SND %u, root %u)",
                      leaf.c_str(),
                      static_cast<unsigned>(snd_id),
                      static_cast<unsigned>(base_key));
        SetStatus(status);
        (void)base_key;
        return true;
    }

    void DeleteSampleAt(int sample_list_index)
    {
        if (!m_bank_token || sample_list_index < 0 ||
            sample_list_index >= static_cast<int>(m_samples.size()))
        {
            return;
        }
        const SampleRow sample = m_samples[static_cast<size_t>(sample_list_index)];
        if (sample.source != SampleSource::Bank)
        {
            SetStatus("Song-document samples: remove via the instrument / export path");
            return;
        }
        if (!EnsureActiveBankWritable())
        {
            return;
        }

        if (sample.unassigned)
        {
            XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
            const XLongResourceID sid = static_cast<XLongResourceID>(sample.snd_resource_id);
            static const XResourceType kTypes[] = {ID_ESND, ID_CSND, ID_SND, 0};
            bool deleted = false;
            for (int t = 0; kTypes[t] != 0; ++t)
            {
                if (XExistsFileResource(bank, kTypes[t], sid) != FALSE)
                {
                    if (XDeleteFileResource(bank, kTypes[t], sid, FALSE) != FALSE)
                    {
                        deleted = true;
                        break;
                    }
                }
            }
            if (!deleted)
            {
                SetStatus("Failed to delete unassigned sample resource");
                return;
            }
            (void)XCleanResourceFile(bank);
            m_session_custom_snd_ids.erase(sample.snd_resource_id);
            m_session_pcm_cache.erase(sample.snd_resource_id);
            m_app_compression_cache.erase(sample.snd_resource_id);
            RefreshListsFromBank();
            m_selected_sample = -1;
            char status[128];
            std::snprintf(status, sizeof(status), "Deleted unassigned sample SND %u",
                          sample.snd_resource_id);
            SetStatus(status);
            return;
        }

        const BAEResult r =
            BAERmfEditorBank_DeleteInstrumentSample(m_bank_token,
                                                    sample.instrument_index,
                                                    sample.split_index,
                                                    true);
        if (r != BAE_NO_ERROR)
        {
            SetStatus(std::string("Delete sample failed: ") + FormatBAEError(r));
            return;
        }
        RefreshListsFromBank();
        m_selected_sample = -1;
        char status[160];
        std::snprintf(status,
                      sizeof(status),
                      "Deleted sample SND %u from B%dP%03d",
                      sample.snd_resource_id,
                      sample.bank,
                      sample.program);
        SetStatus(status);
    }

    void BeginAddSampleFromPath(const std::string &path)
    {
        (void)AddOrphanSampleFromFile(path.c_str());
    }

    bool CreateEmptyBankInstrument(uint32_t inst_id, int root_key, const char *display_name)
    {
        if (!EnsureActiveBankWritable())
        {
            return false;
        }
        XFILE bank_file = reinterpret_cast<XFILE>(m_bank_token);
        if (XExistsFileResource(bank_file, ID_INST, static_cast<XLongResourceID>(inst_id)) != FALSE)
        {
            SetStatus("Instrument slot already occupied");
            return false;
        }

        InstrumentResource instrument;
        std::memset(&instrument, 0, sizeof(instrument));
        XPutShort(&instrument.sndResourceID, 0);
        XPutShort(&instrument.midiRootKey, static_cast<uint16_t>(root_key));
        instrument.panPlacement = 0;
        instrument.flags1 = ZBF_useSampleRate;
        instrument.flags2 = ZBF_useSoundModifierAsRootKey;
        XPutShort(&instrument.miscParameter1, static_cast<uint16_t>(root_key));
        XPutShort(&instrument.miscParameter2, 100);
        XPutShort(&instrument.keySplitCount, 0);
        XPutShort(&instrument.tremoloCount, 0);
        XPutShort(&instrument.tremoloEnd, 0x8000);

        char pascal_name[256];
        MakeResourcePascalName(display_name && display_name[0] ? display_name : "Empty Instrument",
                               pascal_name);
        if (XAddFileResource(bank_file,
                             ID_INST,
                             static_cast<XLongResourceID>(inst_id),
                             pascal_name,
                             &instrument,
                             static_cast<int32_t>(sizeof(instrument))) != 0)
        {
            SetStatus("Failed to add instrument to bank");
            return false;
        }
        return true;
    }

    bool CreateInstrumentUsingSample(uint32_t inst_id,
                                     int root_key,
                                     const char *display_name,
                                     uint32_t snd_id)
    {
        if (!EnsureActiveBankWritable())
        {
            return false;
        }
        if (snd_id == 0xFFFFu || snd_id > 0xFFFF)
        {
            SetStatus("Invalid sample ID");
            return false;
        }
        XFILE bank_file = reinterpret_cast<XFILE>(m_bank_token);
        if (XExistsFileResource(bank_file, ID_INST, static_cast<XLongResourceID>(inst_id)) != FALSE)
        {
            SetStatus("Instrument slot already occupied");
            return false;
        }

        InstrumentResource instrument;
        std::memset(&instrument, 0, sizeof(instrument));
        XPutShort(&instrument.sndResourceID, static_cast<uint16_t>(snd_id));
        XPutShort(&instrument.midiRootKey, static_cast<uint16_t>(root_key));
        instrument.panPlacement = 0;
        instrument.flags1 = ZBF_useSampleRate;
        instrument.flags2 = ZBF_useSoundModifierAsRootKey;
        XPutShort(&instrument.miscParameter1, static_cast<uint16_t>(root_key));
        XPutShort(&instrument.miscParameter2, 100);
        XPutShort(&instrument.keySplitCount, 0);
        XPutShort(&instrument.tremoloCount, 0);
        XPutShort(&instrument.tremoloEnd, 0x8000);

        char pascal_name[256];
        MakeResourcePascalName(display_name && display_name[0] ? display_name : "Instrument",
                               pascal_name);
        if (XAddFileResource(bank_file,
                             ID_INST,
                             static_cast<XLongResourceID>(inst_id),
                             pascal_name,
                             &instrument,
                             static_cast<int32_t>(sizeof(instrument))) != 0)
        {
            SetStatus("Failed to add instrument to bank");
            return false;
        }
        return true;
    }

    void SelectInstrumentByInstId(uint32_t inst_id)
    {
        for (size_t i = 0; i < m_instruments.size(); ++i)
        {
            if (m_instruments[i].inst_id == inst_id)
            {
                m_selected_instrument = static_cast<int>(i);
                m_selected_bank = m_instruments[i].bank;
                m_selected_program = m_instruments[i].program;
                return;
            }
        }
    }

    void EnsureInstrumentFilterShows(int bank, bool percussion)
    {
        const int category = percussion ? 1 : 0;
        for (size_t i = 0; i < m_instrument_filters.size(); ++i)
        {
            if (m_instrument_filters[i].bank == bank &&
                m_instrument_filters[i].category == category)
            {
                m_instrument_filter_index = static_cast<int>(i);
                return;
            }
        }
        m_instrument_filter_index = 0;
    }

    /* BE2: single sample → make instrument with no dialog (auto slot). */
    bool MakeInstrumentUsingSelectedSampleAuto()
    {
        if (m_selected_sample < 0 || m_selected_sample >= static_cast<int>(m_samples.size()))
        {
            SetStatus("No sample selected");
            return false;
        }
        const SampleRow &sample = m_samples[static_cast<size_t>(m_selected_sample)];
        if (!sample.unassigned || sample.snd_resource_id == 0xFFFFu)
        {
            SetStatus("Make Instrument Using requires an unassigned sample");
            return false;
        }

        InitBankAddDialogDefaults();
        const int free_prog = FirstFreeProgramForGroup(m_bank_add_bank, m_bank_add_percussion);
        if (free_prog < 0)
        {
            /* Group full — open dialog so user can pick another bank. */
            m_bank_add_mode = BankAddMode::MakeInstrument;
            m_bank_add_snd_id = sample.snd_resource_id;
            m_bank_add_root_key = sample.root_key ? sample.root_key : 60;
            std::snprintf(m_bank_add_name, sizeof(m_bank_add_name), "%s", sample.name.c_str());
            m_bank_add_open = true;
            SetStatus("No free program in this bank group — pick a slot");
            return false;
        }

        m_bank_add_program = free_prog;
        const uint32_t inst_id =
            BankInstIdFromSlot(m_bank_add_bank, m_bank_add_percussion, m_bank_add_program);
        std::string name = sample.name;
        /* Strip " [SND:N]" suffix from list display names if present. */
        const size_t bracket = name.rfind(" [SND:");
        if (bracket != std::string::npos)
        {
            name = name.substr(0, bracket);
        }
        if (name.empty())
        {
            name = "Instrument";
        }
        const int root = sample.root_key ? sample.root_key : 60;
        const uint32_t snd_id = sample.snd_resource_id;
        if (!CreateInstrumentUsingSample(inst_id, root, name.c_str(), snd_id))
        {
            return false;
        }

        m_session_custom_snd_ids.insert(snd_id);
        m_session_custom_inst_ids.insert(inst_id);
        RefreshListsFromBank();
        EnsureInstrumentFilterShows(m_bank_add_bank, m_bank_add_percussion);
        SelectInstrumentByInstId(inst_id);
        m_sample_show_filter = SampleShowFilter::Custom;
        for (size_t i = 0; i < m_samples.size(); ++i)
        {
            if (m_samples[i].snd_resource_id == snd_id && !m_samples[i].unassigned)
            {
                m_selected_sample = static_cast<int>(i);
                break;
            }
        }

        char status[256];
        std::snprintf(status,
                      sizeof(status),
                      "Made instrument \"%s\" at B%d%s P%03d (SND %u)",
                      name.c_str(),
                      m_bank_add_bank,
                      m_bank_add_percussion ? " Perc" : "",
                      m_bank_add_program,
                      snd_id);
        SetStatus(status);
        return true;
    }

    void OpenMakeInstrumentUsingDialog()
    {
        if (m_selected_sample < 0 || m_selected_sample >= static_cast<int>(m_samples.size()))
        {
            SetStatus("No sample selected");
            return;
        }
        const SampleRow &sample = m_samples[static_cast<size_t>(m_selected_sample)];
        if (!sample.unassigned || sample.snd_resource_id == 0xFFFFu)
        {
            SetStatus("Make Instrument Using requires an unassigned sample");
            return;
        }
        m_bank_add_mode = BankAddMode::MakeInstrument;
        m_bank_add_snd_id = sample.snd_resource_id;
        InitBankAddDialogDefaults();
        m_bank_add_root_key = sample.root_key ? sample.root_key : 60;
        std::string name = sample.name;
        const size_t bracket = name.rfind(" [SND:");
        if (bracket != std::string::npos)
        {
            name = name.substr(0, bracket);
        }
        std::snprintf(m_bank_add_name, sizeof(m_bank_add_name), "%s",
                      name.empty() ? "Instrument" : name.c_str());
        m_bank_add_open = true;
    }

    bool CommitBankAddDialog()
    {
        if (!m_bank_token)
        {
            SetStatus("No bank loaded");
            return false;
        }
        m_bank_add_bank = std::clamp(m_bank_add_bank, 0, 127);
        m_bank_add_program = std::clamp(m_bank_add_program, 0, 127);
        m_bank_add_root_key = std::clamp(m_bank_add_root_key, 0, 127);

        bool used[128];
        CollectUsedProgramsForGroup(m_bank_add_bank, m_bank_add_percussion, used);
        if (used[m_bank_add_program])
        {
            SetStatus("Program already in use in this bank group");
            return false;
        }

        const uint32_t inst_id =
            BankInstIdFromSlot(m_bank_add_bank, m_bank_add_percussion, m_bank_add_program);
        const char *name = m_bank_add_name[0] ? m_bank_add_name : "Untitled";

        bool ok = false;
        if (m_bank_add_mode == BankAddMode::Alias)
        {
            if (m_bank_add_alias_source_index < 0 ||
                m_bank_add_alias_source_index >= static_cast<int>(m_instruments.size()))
            {
                SetStatus("Pick a source instrument for the alias");
                return false;
            }
            if (!EnsureActiveBankWritable())
            {
                return false;
            }
            const InstrumentRow &src =
                m_instruments[static_cast<size_t>(m_bank_add_alias_source_index)];
            const BAEResult r =
                BAERmfEditorBank_AliasInstrument(m_bank_token, src.instrument_index, inst_id);
            ok = (r == BAE_NO_ERROR);
            if (!ok)
            {
                SetStatus(std::string("Create alias failed: ") + FormatBAEError(r));
                return false;
            }
            MarkBank01TouchedIfNeeded(m_bank_add_bank);
        }
        else if (m_bank_add_mode == BankAddMode::MakeInstrument)
        {
            ok = CreateInstrumentUsingSample(inst_id,
                                             m_bank_add_root_key,
                                             name,
                                             m_bank_add_snd_id);
            if (ok && m_bank_add_snd_id != kNoSndResourceId)
            {
                m_session_custom_snd_ids.insert(m_bank_add_snd_id);
            }
        }
        else
        {
            ok = CreateEmptyBankInstrument(inst_id, m_bank_add_root_key, name);
        }
        if (!ok)
        {
            return false;
        }
        m_session_custom_inst_ids.insert(inst_id);

        RefreshListsFromBank();
        EnsureInstrumentFilterShows(m_bank_add_bank, m_bank_add_percussion);
        SelectInstrumentByInstId(inst_id);
        if (m_bank_add_mode == BankAddMode::MakeInstrument &&
            m_bank_add_snd_id != kNoSndResourceId)
        {
            m_sample_show_filter = SampleShowFilter::Custom;
            for (size_t i = 0; i < m_samples.size(); ++i)
            {
                if (m_samples[i].snd_resource_id == m_bank_add_snd_id && !m_samples[i].unassigned)
                {
                    m_selected_sample = static_cast<int>(i);
                    break;
                }
            }
        }

        char status[256];
        const char *verb = "Added instrument";
        if (m_bank_add_mode == BankAddMode::MakeInstrument)
        {
            verb = "Made instrument";
        }
        else if (m_bank_add_mode == BankAddMode::Alias)
        {
            verb = "Created alias";
        }
        std::snprintf(status,
                      sizeof(status),
                      "%s B%d%s P%03d (instID %u)",
                      verb,
                      m_bank_add_bank,
                      m_bank_add_percussion ? " Perc" : "",
                      m_bank_add_program,
                      inst_id);
        SetStatus(status);
        return true;
    }

    void DrawBankAddDialog()
    {
        if (m_bank_add_open)
        {
            ImGui::OpenPopup("Add Bank Entry");
        }
        if (!ImGui::BeginPopupModal("Add Bank Entry",
                                    &m_bank_add_open,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            return;
        }

        if (m_bank_add_mode != BankAddMode::MakeInstrument)
        {
            if (ImGui::RadioButton("Empty Instrument", m_bank_add_mode == BankAddMode::Instrument))
            {
                m_bank_add_mode = BankAddMode::Instrument;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Alias Instrument", m_bank_add_mode == BankAddMode::Alias))
            {
                m_bank_add_mode = BankAddMode::Alias;
            }
        }
        else
        {
            ImGui::TextUnformatted("Make Instrument Using Sample");
            ImGui::TextDisabled("SND %u", m_bank_add_snd_id);
        }
        ImGui::Separator();

        if (m_bank_add_mode == BankAddMode::Alias)
        {
            const char *src_preview = "(none)";
            char src_buf[256];
            if (m_bank_add_alias_source_index >= 0 &&
                m_bank_add_alias_source_index < static_cast<int>(m_instruments.size()))
            {
                const InstrumentRow &src =
                    m_instruments[static_cast<size_t>(m_bank_add_alias_source_index)];
                std::snprintf(src_buf,
                              sizeof(src_buf),
                              "B%dP%03d  %s",
                              src.bank,
                              src.program,
                              src.name.c_str());
                src_preview = src_buf;
            }
            ImGui::SetNextItemWidth(360.0f);
            if (ImGui::BeginCombo("Source instrument", src_preview))
            {
                for (size_t i = 0; i < m_instruments.size(); ++i)
                {
                    const InstrumentRow &src = m_instruments[i];
                    if (src.is_alias || src.from_song_document)
                    {
                        continue;
                    }
                    char row[256];
                    std::snprintf(row,
                                  sizeof(row),
                                  "B%dP%03d  %s##alias_src_%zu",
                                  src.bank,
                                  src.program,
                                  src.name.c_str(),
                                  i);
                    const bool selected = (m_bank_add_alias_source_index == static_cast<int>(i));
                    if (ImGui::Selectable(row, selected))
                    {
                        m_bank_add_alias_source_index = static_cast<int>(i);
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Destination is a free program slot that will alias the source.");
        }

        const int prev_bank = m_bank_add_bank;
        const bool prev_perc = m_bank_add_percussion;

        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Bank", &m_bank_add_bank);
        m_bank_add_bank = std::clamp(m_bank_add_bank, 0, 127);
        ImGui::SameLine();
        ImGui::Checkbox("Percussion", &m_bank_add_percussion);

        if (m_bank_add_bank != prev_bank || m_bank_add_percussion != prev_perc)
        {
            const int free_prog = FirstFreeProgramForGroup(m_bank_add_bank, m_bank_add_percussion);
            if (free_prog >= 0)
            {
                m_bank_add_program = free_prog;
            }
        }

        bool used[128];
        CollectUsedProgramsForGroup(m_bank_add_bank, m_bank_add_percussion, used);

        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Program", &m_bank_add_program);
        m_bank_add_program = std::clamp(m_bank_add_program, 0, 127);
        if (used[m_bank_add_program])
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "in use");
        }
        else
        {
            ImGui::SameLine();
            ImGui::TextDisabled("instID %u",
                               BankInstIdFromSlot(m_bank_add_bank,
                                                  m_bank_add_percussion,
                                                  m_bank_add_program));
        }

        if (m_bank_add_mode != BankAddMode::Alias)
        {
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputInt("Root Key", &m_bank_add_root_key);
            m_bank_add_root_key = std::clamp(m_bank_add_root_key, 0, 127);

            ImGui::SetNextItemWidth(280.0f);
            ImGui::InputText("Name", m_bank_add_name, sizeof(m_bank_add_name));
        }

        ImGui::Separator();
        const bool can_create =
            !used[m_bank_add_program] && m_bank_token != 0 &&
            (m_bank_add_mode != BankAddMode::MakeInstrument ||
             m_bank_add_snd_id != kNoSndResourceId) &&
            (m_bank_add_mode != BankAddMode::Alias || m_bank_add_alias_source_index >= 0);
        if (!can_create)
        {
            ImGui::BeginDisabled();
        }
        const char *ok_label = "Add Instrument";
        if (m_bank_add_mode == BankAddMode::MakeInstrument)
        {
            ok_label = "Make Instrument";
        }
        else if (m_bank_add_mode == BankAddMode::Alias)
        {
            ok_label = "Create Alias";
        }
        if (ImGui::Button(ok_label, ImVec2(140.0f, 0.0f)))
        {
            if (CommitBankAddDialog())
            {
                m_bank_add_open = false;
                ImGui::CloseCurrentPopup();
            }
        }
        if (!can_create)
        {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f)))
        {
            m_bank_add_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
