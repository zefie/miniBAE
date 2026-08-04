/* Beatnik Editor 2 Session .bsn/.zsn open + save — included inside NbEditorApp.
 * Parsers live in anonymous namespace (nbeditor_main.cpp). See docs/be2_session_bsn.md.
 */

static void BsnMakePascalName(const char *c_name, char out_pascal[256])
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

static bool BsnSongObjectIsMidi(XFILE bank_file, const unsigned char *song_body, int32_t song_size)
{
    if (!song_body || song_size < 8)
    {
        return false;
    }
    const uint16_t obj_id = BsnReadBE16(song_body);
    return XExistsFileResource(bank_file, ID_MIDI, static_cast<XLongResourceID>(obj_id)) != FALSE;
}

bool SessionRequiresZsn(uint32_t *out_reason) const
{
    uint32_t reason = 0;
    if (m_bank_token)
    {
        uint32_t bank_reason = 0;
        if (BAERmfEditorBank_RequiresZsb(m_bank_token, &bank_reason) != FALSE)
        {
            reason |= bank_reason;
        }
    }
    if (m_document)
    {
        uint32_t doc_reason = 0;
        if (BAERmfEditorDocument_RequiresZmf(m_document, &doc_reason) != FALSE)
        {
            reason |= doc_reason;
        }
    }
    if (out_reason)
    {
        *out_reason = reason;
    }
    return reason != 0;
}

bool SyncSessionSongsFromDocument()
{
    if (!m_document)
    {
        return !m_session_songs.empty();
    }

    unsigned char *midi_data = nullptr;
    uint32_t midi_size = 0;
    const BAEResult midi_result =
        BAERmfEditorDocument_SaveAsMidiToMemory(m_document, &midi_data, &midi_size);
    if (midi_result != BAE_NO_ERROR || !midi_data || midi_size == 0)
    {
        if (midi_data)
        {
            XDisposePtr(midi_data);
        }
        return !m_session_songs.empty();
    }

    std::vector<unsigned char> rmf_bytes;
    {
        unsigned char *rmf_data = nullptr;
        uint32_t rmf_size = 0;
        uint32_t zmf_reason = 0;
        const bool use_zmf = BAERmfEditorDocument_RequiresZmf(m_document, &zmf_reason) != FALSE;
        const BAEResult rmf_result =
            BAERmfEditorDocument_SaveAsRmfToMemory(m_document, use_zmf, &rmf_data, &rmf_size);
        if (rmf_result == BAE_NO_ERROR && rmf_data && rmf_size > 0)
        {
            rmf_bytes.assign(rmf_data, rmf_data + rmf_size);
        }
        if (rmf_data)
        {
            XDisposePtr(rmf_data);
        }
    }

    std::string name = m_song_row.name;
    const char *title = BAERmfEditorDocument_GetInfo(m_document, TITLE_INFO);
    if (title && title[0])
    {
        name = title;
    }
    if (name.empty())
    {
        name = "Untitled Song";
    }

    if (m_session_song_index >= 0 &&
        m_session_song_index < static_cast<int>(m_session_songs.size()))
    {
        SessionSongEntry &entry = m_session_songs[static_cast<size_t>(m_session_song_index)];
        entry.name = name;
        if (!m_song_row.source_type.empty())
        {
            entry.source_type = m_song_row.source_type;
        }
        entry.midi_bytes.assign(midi_data, midi_data + midi_size);
        entry.rmf_bytes = std::move(rmf_bytes);
    }
    else
    {
        /* No selection (or empty list): append a new song slot — never overwrite [0]. */
        SessionSongEntry entry;
        entry.song_resource_id = 0;
        entry.midi_resource_id = 0;
        entry.name = name;
        entry.source_type = m_song_row.source_type.empty() ? "Song" : m_song_row.source_type;
        entry.midi_bytes.assign(midi_data, midi_data + midi_size);
        entry.rmf_bytes = std::move(rmf_bytes);
        m_session_songs.push_back(std::move(entry));
        m_session_song_index = static_cast<int>(m_session_songs.size()) - 1;
    }

    XDisposePtr(midi_data);
    return true;
}

/* File→Open RMF/ZMF: new session around the song; promote embeds into bank as custom. */
bool OpenRmfAsSession(const std::string &path, const char *source_type)
{
    if (m_song)
    {
        BAESong_Stop(m_song, FALSE);
        BAESong_Delete(m_song);
        m_song = nullptr;
    }
    if (m_document)
    {
        BAERmfEditorDocument_Delete(m_document);
        m_document = nullptr;
    }
    ClearSessionSongState(false);
    ClearBank2AndCustomSamples();
    m_loaded_song_path.clear();
    m_loaded_doc_path.clear();
    m_song_row = SongRow{};
    m_document_dirty = false;
    m_song_started = false;
    ClearSongOverrides();
    m_instrument_editor_open = false;

    if (!m_bank_token)
    {
        (void)LoadBuiltinBankAsActive();
    }

    if (!LoadDocumentFromPath(path.c_str()))
    {
        return false;
    }

    /* Promote embeds from the on-disk RMF once. Do NOT re-promote from a
     * re-serialized SaveAsRmf blob: that path can turn embeds into bank aliases
     * after the first promote, then delete the just-copied bank SNDs. */
    (void)PromoteEmbeddedResourcesFromRmfPath(path.c_str());
    UpdateSongRowFromDocument(path, source_type);
    m_session_song_index = -1;

    /* Keep original file bytes for later ActivateSessionSongAt promotes. */
    std::vector<unsigned char> original_rmf;
    {
        std::ifstream in(path, std::ios::binary);
        if (in)
        {
            original_rmf.assign(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
        }
    }

    if (!SyncSessionSongsFromDocument())
    {
        SetStatus("RMF opened but failed to add Session song entry");
        return false;
    }
    if (!original_rmf.empty() && m_session_song_index >= 0 &&
        m_session_song_index < static_cast<int>(m_session_songs.size()))
    {
        m_session_songs[static_cast<size_t>(m_session_song_index)].rmf_bytes =
            std::move(original_rmf);
    }

    const bool play_ok = ReloadPlaybackFromDocument(true);
    char status[256];
    std::snprintf(status,
                  sizeof(status),
                  "Opened %s as Session (%zu custom INST)%s",
                  source_type && source_type[0] ? source_type : "RMF",
                  m_session_custom_inst_ids.size(),
                  play_ok ? "" : " (playback failed)");
    SetStatus(status);
    return true;
}

bool CommitActiveSessionSong()
{
    if (!m_document)
    {
        return !m_session_songs.empty();
    }
    return SyncSessionSongsFromDocument();
}

bool ImportSongDocumentAsNewSessionEntry(const std::string &path, const char *source_type)
{
    /* Preserve the previously active song before replacing m_document. */
    (void)CommitActiveSessionSong();

    if (!LoadDocumentFromPath(path.c_str()))
    {
        return false;
    }

    /* Force append: do not update the previous selection slot. */
    m_session_song_index = -1;
    UpdateSongRowFromDocument(path, source_type);
    if (!SyncSessionSongsFromDocument())
    {
        SetStatus("Song imported but failed to add to Session song list");
        return false;
    }
    const bool play_ok = ReloadPlaybackFromDocument(true);
    char status[256];
    std::snprintf(status,
                  sizeof(status),
                  "Added Session song %d/%zu%s",
                  m_session_song_index + 1,
                  m_session_songs.size(),
                  play_ok ? "" : " (playback failed)");
    SetStatus(status);
    return true;
}

bool RemoveSessionSongAt(int index)
{
    if (index < 0 || index >= static_cast<int>(m_session_songs.size()))
    {
        return false;
    }

    const bool removing_active = (index == m_session_song_index);
    if (!removing_active && m_document &&
        m_session_song_index >= 0 &&
        m_session_song_index < static_cast<int>(m_session_songs.size()))
    {
        (void)CommitActiveSessionSong();
    }

    m_session_songs.erase(m_session_songs.begin() + index);

    if (m_session_songs.empty())
    {
        StopSessionSong();
        if (m_song)
        {
            BAESong_Stop(m_song, FALSE);
            BAESong_Delete(m_song);
            m_song = nullptr;
        }
        if (m_preview_song)
        {
            BAESong_Stop(m_preview_song, FALSE);
            BAESong_Delete(m_preview_song);
            m_preview_song = nullptr;
        }
        if (m_document)
        {
            BAERmfEditorDocument_Delete(m_document);
            m_document = nullptr;
        }
        m_session_song_index = -1;
        m_song_row = SongRow{};
        m_loaded_song_path.clear();
        m_loaded_doc_path.clear();
        m_document_dirty = false;
        m_song_started = false;
        ClearSongOverrides();
        RefreshSongSamplesFromDocument();
        SetStatus("Removed Session song (none left)");
        return true;
    }

    if (removing_active)
    {
        const int next = std::min(index, static_cast<int>(m_session_songs.size()) - 1);
        m_session_song_index = -1; /* avoid syncing deleted slot */
        if (!ActivateSessionSongAt(next))
        {
            SetStatus("Removed Session song (failed to activate next)");
            return false;
        }
        SetStatus("Removed Session song");
        return true;
    }

    if (m_session_song_index > index)
    {
        --m_session_song_index;
    }
    SetStatus("Removed Session song");
    return true;
}

void RememberSessionPcmFromSampleEditor()
{
#if NBEDITOR_MVP
    if (m_sample_editor_cached_pcm.empty() || m_sample_editor_cached_frame_count == 0)
    {
        return;
    }
    SessionPcmCacheEntry entry;
    entry.name = m_sample_editor_sample_name.empty() ? "sample" : m_sample_editor_sample_name;
    /* Strip codec suffix like " (MPEG 128k)" for CaSd naming closer to BE2. */
    const size_t paren = entry.name.find(" (");
    if (paren != std::string::npos)
    {
        entry.name = entry.name.substr(0, paren);
    }
    entry.pcm = m_sample_editor_cached_pcm;
    entry.frame_count = m_sample_editor_cached_frame_count;
    entry.bit_size = m_sample_editor_cached_bit_size ? m_sample_editor_cached_bit_size : 16;
    entry.channels = m_sample_editor_cached_channels ? m_sample_editor_cached_channels : 1;
    entry.sample_rate = m_sample_editor_cached_sample_rate
                            ? m_sample_editor_cached_sample_rate
                            : UNSIGNED_LONG_TO_XFIXED(m_sample_editor_sample_rate_hz
                                                          ? m_sample_editor_sample_rate_hz
                                                          : 44100u);
    entry.loop_start = m_sample_editor_loop_start;
    entry.loop_end = m_sample_editor_loop_end;
    entry.base_key = m_sample_editor_root_key;
    entry.snd_resource_id = m_sample_editor_snd_resource_id;
    const uint32_t key = entry.snd_resource_id ? entry.snd_resource_id
                                               : static_cast<uint32_t>(m_session_pcm_cache.size() + 1);
    m_session_pcm_cache[key] = std::move(entry);
#endif
}

void ClearAppCompressionCache()
{
    PerformClearCompressionCache();
}

static XResourceType SessionCasdType()
{
    return FOUR_CHAR('C', 'a', 'S', 'd');
}

bool HydrateSessionPcmEntryFromCasd(uint32_t snd_id)
{
    if (!m_bank_token || snd_id == 0)
    {
        return false;
    }
    if (m_session_pcm_cache.count(snd_id) != 0 && !m_session_pcm_cache[snd_id].pcm.empty())
    {
        return true;
    }
    XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
    int32_t size = 0;
    char name[256];
    name[0] = 0;
    XPTR casd = XGetFileResource(bank,
                                 SessionCasdType(),
                                 static_cast<XLongResourceID>(snd_id),
                                 name,
                                 &size);
    if (!casd || size <= 0)
    {
        if (casd)
        {
            XDisposePtr(casd);
        }
        return false;
    }

    SampleDataInfo sdi;
    std::memset(&sdi, 0, sizeof(sdi));
    if (XGetSampleInfoFromSnd(casd, &sdi) != 0 || sdi.frames == 0)
    {
        XDisposePtr(casd);
        return false;
    }
    XPTR pcm_ptr = XGetSamplePtrFromSnd(casd, &sdi);
    if (!pcm_ptr)
    {
        XDisposePtr(casd);
        return false;
    }
    const uint32_t bytes_per =
        static_cast<uint32_t>((sdi.bitSize ? sdi.bitSize : 16) / 8) *
        static_cast<uint32_t>(sdi.channels ? sdi.channels : 1);
    const size_t pcm_bytes = static_cast<size_t>(sdi.frames) * bytes_per;
    if (pcm_bytes == 0 || pcm_bytes > static_cast<size_t>(size) + 65536u)
    {
        XDisposePtr(casd);
        return false;
    }

    SessionPcmCacheEntry entry;
    entry.snd_resource_id = snd_id;
    entry.frame_count = sdi.frames;
    entry.bit_size = static_cast<uint16_t>(sdi.bitSize ? sdi.bitSize : 16);
    entry.channels = static_cast<uint16_t>(sdi.channels ? sdi.channels : 1);
    entry.sample_rate = sdi.rate ? sdi.rate : UNSIGNED_LONG_TO_XFIXED(44100);
    entry.loop_start = sdi.loopStart;
    entry.loop_end = sdi.loopEnd;
    entry.base_key = sdi.baseKey ? sdi.baseKey : 60;
    {
        const unsigned char plen = static_cast<unsigned char>(name[0]);
        if (plen > 0 && plen < 255)
        {
            entry.name.assign(name + 1, name + 1 + plen);
        }
        else
        {
            entry.name = "sample";
        }
    }
    entry.pcm.assign(static_cast<const unsigned char *>(pcm_ptr),
                     static_cast<const unsigned char *>(pcm_ptr) + pcm_bytes);
    m_session_pcm_cache[snd_id] = std::move(entry);
    XDisposePtr(casd);
    return true;
}

void HydrateSessionPcmCacheFromBankCasd()
{
    if (!m_bank_token)
    {
        return;
    }
    XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
    const int32_t count = XCountFileResourcesOfType(bank, SessionCasdType());
    for (int32_t i = 0; i < count; ++i)
    {
        XLongResourceID rid = 0;
        int32_t size = 0;
        char name[256];
        name[0] = 0;
        XPTR data = XGetIndexedFileResource(bank, SessionCasdType(), &rid, i, name, &size);
        if (data)
        {
            XDisposePtr(data);
        }
        if (rid > 0)
        {
            (void)HydrateSessionPcmEntryFromCasd(static_cast<uint32_t>(rid));
        }
    }
    m_session_casd_count = static_cast<int>(m_session_pcm_cache.size());
}

bool HasUncompressedMasterForSnd(uint32_t snd_id) const
{
    if (snd_id == 0)
    {
        return false;
    }
    auto it = m_session_pcm_cache.find(snd_id);
    if (it != m_session_pcm_cache.end() && !it->second.pcm.empty())
    {
        return true;
    }
    if (!m_bank_token)
    {
        return false;
    }
    return XExistsFileResource(reinterpret_cast<XFILE>(m_bank_token),
                               SessionCasdType(),
                               static_cast<XLongResourceID>(snd_id)) != FALSE;
}

bool ActivateSessionSongAt(int index)
{
    if (index < 0 || index >= static_cast<int>(m_session_songs.size()))
    {
        return false;
    }
    if (index == m_session_song_index && m_document != nullptr)
    {
        return true;
    }

    /* Persist edits on the song we're leaving so multi-song sessions keep all data. */
    if (m_document && m_session_song_index >= 0 &&
        m_session_song_index < static_cast<int>(m_session_songs.size()) &&
        m_session_song_index != index)
    {
        (void)CommitActiveSessionSong();
    }

    const SessionSongEntry &entry = m_session_songs[static_cast<size_t>(index)];
    if (entry.rmf_bytes.empty() && entry.midi_bytes.empty())
    {
        return false;
    }

    BAERmfEditorDocument *new_doc = nullptr;
    if (!entry.rmf_bytes.empty())
    {
        new_doc = BAERmfEditorDocument_LoadFromMemory(
            entry.rmf_bytes.data(),
            static_cast<uint32_t>(entry.rmf_bytes.size()),
            BAE_INVALID_TYPE);
    }
    if (!new_doc && !entry.midi_bytes.empty())
    {
        new_doc = BAERmfEditorDocument_LoadFromMemory(
            entry.midi_bytes.data(),
            static_cast<uint32_t>(entry.midi_bytes.size()),
            BAE_MIDI_TYPE);
    }
    if (!new_doc)
    {
        SetStatus("Session song load failed");
        return false;
    }
    if (m_document)
    {
        BAERmfEditorDocument_Delete(m_document);
    }
    m_document = new_doc;
    m_session_song_index = index;
    m_document_dirty = false;

    /* Promote after m_document is swapped so bank list naming can read
     * document display names for empty INST/SND Pascal names. */
    if (!entry.rmf_bytes.empty())
    {
        (void)PromoteEmbeddedResourcesFromRmfMemory(
            entry.rmf_bytes.data(),
            static_cast<uint32_t>(entry.rmf_bytes.size()));
    }

    m_song_row.name = entry.name;
    if (!m_loaded_session_path.empty())
    {
        m_song_row.path = m_loaded_session_path;
        m_song_row.source_type =
            entry.source_type.empty()
                ? (EndsWith(m_loaded_session_path, ".zsn") ? "ZSN" : "BSN")
                : entry.source_type;
        m_loaded_song_path = m_loaded_session_path;
        m_loaded_doc_path = m_loaded_session_path;
    }
    else
    {
        m_song_row.path.clear();
        m_song_row.source_type = entry.source_type.empty() ? "Session" : entry.source_type;
        m_loaded_song_path.clear();
        m_loaded_doc_path.clear();
    }
    RefreshSongOverridesFromDocument();
    RefreshSongSamplesFromDocument();
    (void)BAERmfEditorDocument_SetInfo(m_document, TITLE_INFO, entry.name.c_str());
    return ReloadPlaybackFromDocument(true);
}

bool LoadSessionBsnFromPath(const char *path)
{
    if (!path || !path[0])
    {
        return false;
    }

    std::vector<SessionSongEntry> songs;
    int casd_count = 0;
    if (!ProbeBe2SessionBsn(path, &songs, &casd_count))
    {
        return false;
    }

    /* Thin sessions (blank.bsn) may have no playable bank body.
     * Full replace: session file must become the active bank token. */
    const bool bank_ok = LoadBankFromPath(path, false);
    if (!bank_ok)
    {
        LoadBuiltinBankAsActive();
    }

    m_session_songs = std::move(songs);
    m_session_casd_count = casd_count;
    m_loaded_session_path = path;
    m_session_song_index = -1;
    m_session_pcm_cache.clear();
    m_app_compression_cache.clear();
    RememberRecentSession(path);
    HydrateSessionPcmCacheFromBankCasd();

    if (m_document)
    {
        BAERmfEditorDocument_Delete(m_document);
        m_document = nullptr;
    }
    m_song_row = SongRow{};
    m_document_dirty = false;

    char status[256];
    const char *kind = EndsWith(path, ".zsn") ? "Session .zsn" : "Session .bsn";
    if (m_session_songs.empty())
    {
        std::snprintf(status,
                      sizeof(status),
                      "Opened %s (%s, %d CaSd)",
                      kind,
                      bank_ok ? "bank" : "thin/no bank",
                      m_session_casd_count);
        SetStatus(status);
        return true;
    }

    const bool ok = ActivateSessionSongAt(0);
    std::snprintf(status,
                  sizeof(status),
                  "Opened %s: %zu song(s), %d CaSd%s",
                  kind,
                  m_session_songs.size(),
                  m_session_casd_count,
                  ok ? "" : " (playback failed)");
    SetStatus(status);
    return true;
}

bool SaveSessionToPath(const char *path)
{
    if (!path || !path[0])
    {
        return false;
    }

    RememberSessionPcmFromSampleEditor();
    (void)SyncSessionSongsFromDocument();

    uint32_t z_reason = 0;
    const bool want_zsn = SessionRequiresZsn(&z_reason);
    const int32_t resource_map_id = want_zsn ? XFILERESOURCE_ZMF_ID : XFILERESOURCE_ID;

    XFILE out_file = XFileOpenVirtualResource(resource_map_id);
    if (!out_file)
    {
        SetStatus("Session save failed: virtual resource");
        return false;
    }

    /* ---- bank body (optional) ---- */
    if (m_bank_token)
    {
        XFILE bank_file = reinterpret_cast<XFILE>(m_bank_token);
        static const XResourceType bank_resource_types[] = {
            ID_INST,
            ID_SND,
            ID_CSND,
            ID_ESND,
            ID_ALIAS,
            ID_BANK,
            ID_SONG,
            ID_MIDI_OLD,
            ID_CMID,
            ID_EMID,
            ID_ECMI,
            ID_RMF,
            ID_TEXT,
            ID_VERS,
            0,
        };

        for (int type_idx = 0; bank_resource_types[type_idx] != 0; ++type_idx)
        {
            const XResourceType res_type = bank_resource_types[type_idx];
            const int32_t res_count = XCountFileResourcesOfType(bank_file, res_type);
            for (int32_t res_index = 0; res_index < res_count; ++res_index)
            {
                XLongResourceID res_id = 0;
                int32_t res_size = 0;
                char res_name[256];
                res_name[0] = 0;
                XPTR res_data = XGetIndexedFileResource(bank_file,
                                                       res_type,
                                                       &res_id,
                                                       res_index,
                                                       res_name,
                                                       &res_size);
                if (!res_data)
                {
                    continue;
                }
                if (res_type == ID_SONG &&
                    BsnSongObjectIsMidi(bank_file,
                                        static_cast<const unsigned char *>(res_data),
                                        res_size))
                {
                    XDisposePtr(res_data);
                    continue; /* user songs rewritten below */
                }
                if (XAddFileResource(out_file, res_type, res_id, res_name, res_data, res_size) != 0)
                {
                    XDisposePtr(res_data);
                    XFileClose(out_file);
                    SetStatus("Session save failed: copy bank resource");
                    return false;
                }
                XDisposePtr(res_data);
            }
        }
    }
    else
    {
        /* Thin shell like blank.bsn */
        static const unsigned char k_zero6[6] = {0, 0, 0, 0, 0, 0};
        char empty_pascal[256];
        empty_pascal[0] = 0;
        (void)XAddFileResource(out_file, ID_VERS, 0, empty_pascal, (void *)k_zero6, 6);
        (void)XAddFileResource(out_file, ID_BANK, 0, empty_pascal, (void *)k_zero6, 6);
    }

    /* ---- user songs (Midi + SONG) ---- */
    struct DateStamp
    {
        uint32_t type;
        uint32_t id;
    };
    std::vector<DateStamp> date_stamps;
    const uint32_t now_ts = static_cast<uint32_t>(std::time(nullptr));

    for (size_t i = 0; i < m_session_songs.size(); ++i)
    {
        SessionSongEntry &entry = m_session_songs[i];
        if (entry.midi_bytes.empty())
        {
            continue;
        }
        if (entry.midi_resource_id == 0)
        {
            entry.midi_resource_id = static_cast<uint32_t>(73 + i);
        }
        /* SONG id 0 is valid for the first user song (matches BE2 fixtures). */
        if (i > 0 && entry.song_resource_id == 0)
        {
            entry.song_resource_id = static_cast<uint32_t>(i);
        }

        char midi_pascal[256];
        BsnMakePascalName(entry.name.c_str(), midi_pascal);
        if (XAddFileResource(out_file,
                             ID_MIDI,
                             static_cast<XLongResourceID>(entry.midi_resource_id),
                             midi_pascal,
                             entry.midi_bytes.data(),
                             static_cast<int32_t>(entry.midi_bytes.size())) != 0)
        {
            XFileClose(out_file);
            SetStatus("Session save failed: add Midi");
            return false;
        }
        date_stamps.push_back({static_cast<uint32_t>(ID_MIDI), entry.midi_resource_id});

        SongResource_Info *song_info = XNewSongResourceInfo();
        if (!song_info)
        {
            XFileClose(out_file);
            SetStatus("Session save failed: song info");
            return false;
        }
        XClearSongResourceInfo(song_info);
        song_info->songType = SONG_TYPE_RMF;
        song_info->objectResourceID = static_cast<XShortResourceID>(entry.midi_resource_id);
        song_info->maxMidiNotes = 56;
        song_info->maxEffects = 4;
        song_info->mixLevel = 8;
        song_info->songVolume = 127;
        song_info->reverbType = 1;
        {
            const size_t tlen = entry.name.size();
            char *title_copy = static_cast<char *>(XNewPtr(static_cast<int32_t>(tlen + 1)));
            if (title_copy)
            {
                std::memcpy(title_copy, entry.name.c_str(), tlen);
                title_copy[tlen] = 0;
                song_info->title = title_copy;
            }
        }
        SongResource *song_res = XNewSongFromSongResourceInfo(song_info);
        XDisposeSongResourceInfo(song_info);
        if (!song_res)
        {
            XFileClose(out_file);
            SetStatus("Session save failed: build SONG");
            return false;
        }
        char song_pascal[256];
        BsnMakePascalName(entry.name.c_str(), song_pascal);
        const int32_t song_size = XGetPtrSize(song_res);
        if (XAddFileResource(out_file,
                             ID_SONG,
                             static_cast<XLongResourceID>(entry.song_resource_id),
                             song_pascal,
                             song_res,
                             song_size) != 0)
        {
            XDisposeSongPtr(song_res);
            XFileClose(out_file);
            SetStatus("Session save failed: add SONG");
            return false;
        }
        XDisposeSongPtr(song_res);
        date_stamps.push_back({static_cast<uint32_t>(ID_SONG), entry.song_resource_id});
    }

    /* ---- CaSd PCM masters ---- */
#if USE_CREATION_API == TRUE
    for (const auto &kv : m_session_pcm_cache)
    {
        const SessionPcmCacheEntry &pcm = kv.second;
        if (pcm.pcm.empty() || pcm.frame_count == 0)
        {
            continue;
        }
        GM_Waveform wave;
        XSetMemory(&wave, sizeof(wave), 0);
        wave.theWaveform = const_cast<void *>(static_cast<const void *>(pcm.pcm.data()));
        wave.waveFrames = pcm.frame_count;
        wave.bitSize = static_cast<unsigned char>(pcm.bit_size ? pcm.bit_size : 16);
        wave.channels = static_cast<unsigned char>(pcm.channels ? pcm.channels : 1);
        wave.waveSize = static_cast<uint32_t>(pcm.pcm.size());
        wave.sampledRate = pcm.sample_rate ? pcm.sample_rate
                                           : UNSIGNED_LONG_TO_XFIXED(44100);
        wave.startLoop = pcm.loop_start;
        wave.endLoop = pcm.loop_end;
        wave.baseMidiPitch = static_cast<unsigned char>(pcm.base_key ? pcm.base_key : 60);
        wave.compressionType = C_NONE;

        XPTR snd_obj = nullptr;
        if (XCreateSoundObjectFromData(&snd_obj, &wave, C_NONE, CS_DEFAULT, nullptr, nullptr) != NO_ERR ||
            !snd_obj)
        {
            continue;
        }
        char casd_pascal[256];
        BsnMakePascalName(pcm.name.c_str(), casd_pascal);
        const uint32_t casd_id = pcm.snd_resource_id ? pcm.snd_resource_id : kv.first;
        const int32_t snd_size = XGetPtrSize(snd_obj);
        if (XAddFileResource(out_file,
                             FOUR_CHAR('C', 'a', 'S', 'd'),
                             static_cast<XLongResourceID>(casd_id),
                             casd_pascal,
                             snd_obj,
                             snd_size) == 0)
        {
            date_stamps.push_back({FOUR_CHAR('C', 'a', 'S', 'd'), casd_id});
        }
        XDisposePtr(snd_obj);
    }
#endif

    /* ---- BePf / BEPF / DATe ---- */
    {
        static const unsigned char k_bepf_body[] = {'e', 'x', 'a', 'm', 'p', 'l', 'e'};
        char empty_pascal[256];
        empty_pascal[0] = 0;
        (void)XAddFileResource(out_file,
                               FOUR_CHAR('B', 'E', 'P', 'F'),
                               1,
                               empty_pascal,
                               (void *)k_bepf_body,
                               static_cast<int32_t>(sizeof(k_bepf_body)));
        date_stamps.push_back({FOUR_CHAR('B', 'E', 'P', 'F'), 1});

        /* 34-byte Session Prefs stub (layout TBD; matches fixture size). */
        unsigned char bepf_prefs[34];
        std::memset(bepf_prefs, 0, sizeof(bepf_prefs));
        bepf_prefs[0] = 0x00;
        bepf_prefs[1] = 0x02;
        char bepf_pascal[256];
        BsnMakePascalName("Session Prefs", bepf_pascal);
        (void)XAddFileResource(out_file,
                               FOUR_CHAR('B', 'e', 'P', 'f'),
                               1,
                               bepf_pascal,
                               bepf_prefs,
                               static_cast<int32_t>(sizeof(bepf_prefs)));
        date_stamps.push_back({FOUR_CHAR('B', 'e', 'P', 'f'), 1});

        std::vector<unsigned char> date_body;
        date_body.reserve((date_stamps.size() + 1) * 12);
        auto put_be32 = [&date_body](uint32_t v) {
            date_body.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
            date_body.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
            date_body.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
            date_body.push_back(static_cast<unsigned char>(v & 0xff));
        };
        for (const DateStamp &ds : date_stamps)
        {
            put_be32(ds.type);
            put_be32(ds.id);
            put_be32(now_ts);
        }
        put_be32(0);
        put_be32(0);
        put_be32(0);
        char date_pascal[256];
        BsnMakePascalName("datestamp list", date_pascal);
        (void)XAddFileResource(out_file,
                               FOUR_CHAR('D', 'A', 'T', 'e'),
                               1,
                               date_pascal,
                               date_body.data(),
                               static_cast<int32_t>(date_body.size()));
    }

    if (XCleanResourceFile(out_file) == FALSE)
    {
        XFileClose(out_file);
        SetStatus("Session save failed: clean");
        return false;
    }

    XPTR packed = nullptr;
    int32_t packed_size = 0;
    if (XFileGetMemoryFileAsData(out_file, &packed, &packed_size) != 0 || !packed || packed_size <= 0)
    {
        XFileClose(out_file);
        if (packed)
        {
            XDisposePtr(packed);
        }
        SetStatus("Session save failed: extract");
        return false;
    }
    XFileClose(out_file);

    XFILENAME xf_out;
    XConvertPathToXFILENAME(const_cast<char *>(path), &xf_out);
    XFILE disk = XFileOpenForWrite(&xf_out, TRUE);
    if (!disk)
    {
        XDisposePtr(packed);
        SetStatus("Session save failed: open for write");
        return false;
    }
    if (XFileSetLength(disk, 0) != 0 ||
        XFileSetPosition(disk, 0L) != 0 ||
        XFileWrite(disk, packed, packed_size) != 0)
    {
        XFileClose(disk);
        XDisposePtr(packed);
        SetStatus("Session save failed: write");
        return false;
    }
    XFileClose(disk);
    XDisposePtr(packed);

    m_loaded_session_path = path;
    m_document_dirty = false;
    m_session_casd_count = static_cast<int>(m_session_pcm_cache.size());
    RememberRecentSession(path);

    char status[320];
    if (want_zsn)
    {
        std::snprintf(status,
                      sizeof(status),
                      "Session saved (.zsn, Z reason=0x%x): %zu song(s), %d CaSd",
                      z_reason,
                      m_session_songs.size(),
                      m_session_casd_count);
    }
    else
    {
        std::snprintf(status,
                      sizeof(status),
                      "Session saved (.bsn): %zu song(s), %d CaSd",
                      m_session_songs.size(),
                      m_session_casd_count);
    }
    SetStatus(status);
    return true;
}
