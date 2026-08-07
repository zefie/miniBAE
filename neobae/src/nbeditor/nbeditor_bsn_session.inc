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

static void NbetPutBE32(std::vector<unsigned char> &out, uint32_t v)
{
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>(v & 0xff));
}

static uint32_t NbetReadBE32(const unsigned char *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static int32_t NbetReadBE32Signed(const unsigned char *p)
{
    return static_cast<int32_t>(NbetReadBE32(p));
}

static bool NbetBodyLooksPlain(const unsigned char *data, size_t size)
{
    return data && size >= 8 &&
           NbetReadBE32(data) == static_cast<uint32_t>(FOUR_CHAR('n', 'B', 'e', 'T'));
}

/* Decode on-disk nBeT resource: plain legacy blob, or XCompressPtr LZMA frame. */
static bool DecodeNbetResourceBody(const unsigned char *raw,
                                   size_t raw_size,
                                   std::vector<unsigned char> &out_plain)
{
    out_plain.clear();
    if (!raw || raw_size < 4)
    {
        return false;
    }
    if (NbetBodyLooksPlain(raw, raw_size))
    {
        out_plain.assign(raw, raw + raw_size);
        return true;
    }
#if defined(USE_LZMA_COMPRESSION) && (USE_LZMA_COMPRESSION == TRUE)
    XPTR decoded = XDecompressPtr(const_cast<unsigned char *>(raw),
                                  static_cast<uint32_t>(raw_size),
                                  TRUE);
    if (!decoded)
    {
        return false;
    }
    const int32_t dsize = XGetPtrSize(decoded);
    const unsigned char *p = static_cast<const unsigned char *>(decoded);
    bool ok = false;
    if (dsize > 0 && NbetBodyLooksPlain(p, static_cast<size_t>(dsize)))
    {
        out_plain.assign(p, p + static_cast<size_t>(dsize));
        ok = true;
    }
    XDisposePtr(decoded);
    return ok;
#else
    return false;
#endif
}

/* LZMA-wrap the plain nBeT blob (XCompressPtr / X_LZMA_RAW) when it shrinks.
 * Open still accepts legacy plaintext bodies. */
static std::vector<unsigned char> EncodeNbetResourceBody(const std::vector<unsigned char> &plain)
{
#if USE_LZMA_COMPRESSION == TRUE
#if USE_CREATION_API != FALSE
    if (!plain.empty())
    {
        XPTR compressed = nullptr;
        const int32_t csize =
            XCompressPtr(&compressed,
                         const_cast<unsigned char *>(plain.data()),
                         static_cast<uint32_t>(plain.size()),
                         X_LZMA_RAW,
                         nullptr,
                         nullptr);
        if (compressed && csize > 0 && static_cast<size_t>(csize) < plain.size())
        {
            std::vector<unsigned char> out(static_cast<unsigned char *>(compressed),
                                           static_cast<unsigned char *>(compressed) +
                                               static_cast<size_t>(csize));
            XDisposePtr(compressed);
            return out;
        }
        if (compressed)
        {
            XDisposePtr(compressed);
        }
    }
#endif
#endif
    return plain;
}

/* True if the window title-bar strip intersects any display usable area. */
static bool NbetWindowTitleBarOnScreen(int x, int y, int w, int h)
{
    if (w < 1 || h < 1)
    {
        return false;
    }
    const SDL_Rect title{x, y, w, (h < 32) ? h : 32};
    int count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&count);
    if (!displays || count <= 0)
    {
        SDL_free(displays);
        return true;
    }
    bool ok = false;
    for (int i = 0; i < count; ++i)
    {
        SDL_Rect bounds{};
        if (!SDL_GetDisplayUsableBounds(displays[i], &bounds))
        {
            continue;
        }
        if (SDL_HasRectIntersection(&title, &bounds))
        {
            ok = true;
            break;
        }
    }
    SDL_free(displays);
    return ok;
}

/* Keep current position; only relocate if the window is off-screen.
 * Prefer saved coords when those land on a display; otherwise clamp to primary. */
static void NbetEnsureMainWindowOnScreen(SDL_Window *window, int saved_x, int saved_y, int w, int h)
{
    if (!window || w < 1 || h < 1)
    {
        return;
    }
    int x = 0;
    int y = 0;
    SDL_GetWindowPosition(window, &x, &y);
    if (NbetWindowTitleBarOnScreen(x, y, w, h))
    {
        return;
    }
    if (NbetWindowTitleBarOnScreen(saved_x, saved_y, w, h))
    {
        SDL_SetWindowPosition(window, saved_x, saved_y);
        return;
    }
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    SDL_Rect bounds{};
    if (primary && SDL_GetDisplayUsableBounds(primary, &bounds))
    {
        const int nx = bounds.x + std::max(0, (bounds.w - w) / 2);
        const int ny = bounds.y + std::max(0, (bounds.h - h) / 2);
        SDL_SetWindowPosition(window, nx, ny);
    }
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
        /* outReason==NULL → RequiresZsb stops on first hit (large banks). */
        uint32_t bank_reason = 0;
        if (BAERmfEditorBank_RequiresZsb(m_bank_token,
                                         out_reason ? &bank_reason : nullptr) != FALSE)
        {
            reason |= (out_reason ? bank_reason : 1u);
        }
    }
    if (m_document)
    {
        uint32_t doc_reason = 0;
        if (BAERmfEditorDocument_RequiresZmf(m_document, &doc_reason) != FALSE)
        {
            /* Container-only bit does not force .zsn by itself. */
            doc_reason &= ~(uint32_t)BAEZMF_ALREADY_ZMF;
            reason |= doc_reason;
        }
    }
    if (out_reason)
    {
        *out_reason = reason;
    }
    return reason != 0;
}

bool BankRequiresZsb(uint32_t *out_reason) const
{
    uint32_t reason = 0;
    if (m_bank_token)
    {
        /* outReason==NULL → RequiresZsb stops on first hit (large banks). */
        if (BAERmfEditorBank_RequiresZsb(m_bank_token,
                                         out_reason ? &reason : nullptr) != FALSE)
        {
            if (!out_reason)
            {
                return true;
            }
        }
    }
    if (out_reason)
    {
        *out_reason = reason;
    }
    return reason != 0;
}

static std::string ReplacePathExtension(const std::string &path, const char *new_ext_with_dot)
{
    if (!new_ext_with_dot || !new_ext_with_dot[0])
    {
        return path;
    }
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
    {
        return path.substr(0, dot) + new_ext_with_dot;
    }
    return path + new_ext_with_dot;
}

/* Force .zsn when session content needs NeoBAE container features.
 * Pass want_zsn_override >= 0 to skip a second RequiresZsb scan. */
std::string EnforceSessionSavePath(const std::string &path,
                                   bool *out_remapped = nullptr,
                                   int want_zsn_override = -1) const
{
    if (out_remapped)
    {
        *out_remapped = false;
    }
    if (path.empty())
    {
        return path;
    }
    const bool want_zsn =
        (want_zsn_override >= 0) ? (want_zsn_override != 0) : SessionRequiresZsn(nullptr);
    if (!want_zsn)
    {
        return path;
    }
    if (EndsWith(path, ".zsn"))
    {
        return path;
    }
    if (out_remapped)
    {
        *out_remapped = true;
    }
    if (EndsWith(path, ".bsn"))
    {
        return ReplacePathExtension(path, ".zsn");
    }
    return ReplacePathExtension(path, ".zsn");
}

/* Force .zsb when bank content needs NeoBAE container features. */
std::string EnforceBankSavePath(const std::string &path, bool *out_remapped = nullptr) const
{
    if (out_remapped)
    {
        *out_remapped = false;
    }
    if (path.empty())
    {
        return path;
    }
    const bool want_zsb = BankRequiresZsb(nullptr);
    if (!want_zsb)
    {
        return path;
    }
    if (EndsWith(path, ".zsb"))
    {
        return path;
    }
    if (out_remapped)
    {
        *out_remapped = true;
    }
    return ReplacePathExtension(path, ".zsb");
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

    const bool is_rmf_like = EndsWith(path, ".rmf") || EndsWith(path, ".zmf");
    if (is_rmf_like)
    {
        /* Promote embeds into the current bank; do not wipe the session. */
        (void)PromoteEmbeddedResourcesFromRmfPath(path.c_str());
    }

    /* Force append: do not update the previous selection slot. */
    m_session_song_index = -1;
    UpdateSongRowFromDocument(path, source_type);
    if (!SyncSessionSongsFromDocument())
    {
        SetStatus("Song imported but failed to add to Session song list");
        return false;
    }
    if (is_rmf_like && m_session_song_index >= 0 &&
        m_session_song_index < static_cast<int>(m_session_songs.size()))
    {
        std::ifstream in(path, std::ios::binary);
        if (in)
        {
            m_session_songs[static_cast<size_t>(m_session_song_index)].rmf_bytes.assign(
                std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    const bool play_ok = ReloadPlaybackFromDocument(true);
    ApplyPlayerMixSettings();
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
        EnsureBankAuditionSong();
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

    static bool MidiReadVlq(const unsigned char *midi,
                            size_t end,
                            size_t *pos,
                            uint32_t *out_value)
    {
        if (!midi || !pos || !out_value || *pos >= end)
        {
            return false;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (*pos >= end)
            {
                return false;
            }
            const unsigned char b = midi[(*pos)++];
            value = (value << 7) | static_cast<uint32_t>(b & 0x7Fu);
            if ((b & 0x80u) == 0)
            {
                *out_value = value;
                return true;
            }
        }
        return false;
    }

    /* Beatnik patch bank = CC0 MSB; instID = bank*256 + program. */
    void CollectInstIdsReferencedByMidi(const unsigned char *midi,
                                        size_t midi_len,
                                        std::set<uint32_t> &out_inst) const
    {
        if (!midi || midi_len < 14 ||
            std::memcmp(midi, "MThd", 4) != 0)
        {
            return;
        }
        const uint32_t hdr_len = (static_cast<uint32_t>(midi[4]) << 24) |
                                 (static_cast<uint32_t>(midi[5]) << 16) |
                                 (static_cast<uint32_t>(midi[6]) << 8) |
                                 static_cast<uint32_t>(midi[7]);
        if (hdr_len < 6 || 8u + hdr_len > midi_len)
        {
            return;
        }
        const uint16_t n_tracks = static_cast<uint16_t>((midi[10] << 8) | midi[11]);
        size_t pos = 8u + hdr_len;
        uint8_t bank_msb[16] = {};
        uint8_t program[16] = {};

        for (uint16_t t = 0; t < n_tracks; ++t)
        {
            if (pos + 8 > midi_len || std::memcmp(midi + pos, "MTrk", 4) != 0)
            {
                return;
            }
            const uint32_t trk_len = (static_cast<uint32_t>(midi[pos + 4]) << 24) |
                                     (static_cast<uint32_t>(midi[pos + 5]) << 16) |
                                     (static_cast<uint32_t>(midi[pos + 6]) << 8) |
                                     static_cast<uint32_t>(midi[pos + 7]);
            pos += 8;
            if (pos + trk_len > midi_len)
            {
                return;
            }
            const size_t end = pos + trk_len;
            uint8_t running = 0;
            while (pos < end)
            {
                uint32_t delta = 0;
                if (!MidiReadVlq(midi, end, &pos, &delta))
                {
                    return;
                }
                if (pos >= end)
                {
                    return;
                }
                uint8_t status = midi[pos];
                if (status & 0x80u)
                {
                    ++pos;
                    running = status;
                }
                else
                {
                    if (!running)
                    {
                        return;
                    }
                    status = running;
                }
                if (status == 0xFFu)
                {
                    if (pos + 1 > end)
                    {
                        return;
                    }
                    ++pos; /* meta type */
                    uint32_t mlen = 0;
                    if (!MidiReadVlq(midi, end, &pos, &mlen) || pos + mlen > end)
                    {
                        return;
                    }
                    pos += mlen;
                    continue;
                }
                if (status == 0xF0u || status == 0xF7u)
                {
                    uint32_t slen = 0;
                    if (!MidiReadVlq(midi, end, &pos, &slen) || pos + slen > end)
                    {
                        return;
                    }
                    pos += slen;
                    continue;
                }
                const uint8_t type = static_cast<uint8_t>(status & 0xF0u);
                const uint8_t ch = static_cast<uint8_t>(status & 0x0Fu);
                uint8_t p1 = 0;
                uint8_t p2 = 0;
                if (type == 0xC0u || type == 0xD0u)
                {
                    if (pos + 1 > end)
                    {
                        return;
                    }
                    p1 = midi[pos++];
                }
                else
                {
                    if (pos + 2 > end)
                    {
                        return;
                    }
                    p1 = midi[pos++];
                    p2 = midi[pos++];
                }
                if (type == 0xB0u)
                {
                    if (p1 == 0)
                    {
                        bank_msb[ch] = p2;
                    }
                }
                else if (type == 0xC0u)
                {
                    program[ch] = static_cast<uint8_t>(p1 & 0x7Fu);
                    out_inst.insert(static_cast<uint32_t>(bank_msb[ch]) * 256u +
                                    static_cast<uint32_t>(program[ch]));
                }
                else if (type == 0x90u && (p2 & 0x7Fu) != 0)
                {
                    out_inst.insert(static_cast<uint32_t>(bank_msb[ch]) * 256u +
                                    static_cast<uint32_t>(program[ch]));
                }
            }
        }
    }

    void CollectSongEmbedInstAndSnd(const SessionSongEntry &entry,
                                    std::set<uint32_t> &out_inst,
                                    std::set<uint32_t> &out_snd) const
    {
        if (entry.rmf_bytes.empty())
        {
            return;
        }
        XFILE donor = XFileOpenResourceFromMemory(
            (XPTR)const_cast<unsigned char *>(entry.rmf_bytes.data()),
            static_cast<uint32_t>(entry.rmf_bytes.size()),
            TRUE);
        if (!donor)
        {
            return;
        }
        std::vector<XShortResourceID> inst_ids;
        std::vector<XShortResourceID> snd_ids;
        CollectAllInstAndSndIdsFromFile(donor, inst_ids, snd_ids);
        XFileClose(donor);
        for (XShortResourceID id : inst_ids)
        {
            out_inst.insert(static_cast<uint32_t>(static_cast<uint16_t>(id)));
        }
        for (XShortResourceID id : snd_ids)
        {
            out_snd.insert(static_cast<uint32_t>(static_cast<uint16_t>(id)));
        }
    }

    void CollectCustomResourcesUsedBySong(const SessionSongEntry &entry,
                                          std::set<uint32_t> &out_inst,
                                          std::set<uint32_t> &out_snd) const
    {
        std::set<uint32_t> embed_inst;
        std::set<uint32_t> embed_snd;
        CollectSongEmbedInstAndSnd(entry, embed_inst, embed_snd);

        std::set<uint32_t> midi_inst;
        if (!entry.midi_bytes.empty())
        {
            CollectInstIdsReferencedByMidi(entry.midi_bytes.data(),
                                           entry.midi_bytes.size(),
                                           midi_inst);
        }
        else if (!entry.rmf_bytes.empty())
        {
            XFILE donor = XFileOpenResourceFromMemory(
                (XPTR)const_cast<unsigned char *>(entry.rmf_bytes.data()),
                static_cast<uint32_t>(entry.rmf_bytes.size()),
                TRUE);
            if (donor)
            {
                static const XResourceType kMidiTypes[] = {ID_MIDI, ID_MIDI_OLD, 0};
                for (int t = 0; kMidiTypes[t] != 0; ++t)
                {
                    const int32_t count = XCountFileResourcesOfType(donor, kMidiTypes[t]);
                    for (int32_t i = 0; i < count; ++i)
                    {
                        XLongResourceID rid = 0;
                        int32_t size = 0;
                        XPTR data = XGetIndexedFileResource(donor,
                                                            kMidiTypes[t],
                                                            &rid,
                                                            i,
                                                            nullptr,
                                                            &size);
                        if (data && size > 0)
                        {
                            CollectInstIdsReferencedByMidi(
                                static_cast<const unsigned char *>(data),
                                static_cast<size_t>(size),
                                midi_inst);
                        }
                        if (data)
                        {
                            XDisposePtr(data);
                        }
                    }
                }
                XFileClose(donor);
            }
        }

        for (uint32_t id : embed_inst)
        {
            if (m_session_custom_inst_ids.count(id) != 0)
            {
                out_inst.insert(id);
            }
        }
        for (uint32_t id : midi_inst)
        {
            if (m_session_custom_inst_ids.count(id) != 0)
            {
                out_inst.insert(id);
            }
        }
        for (uint32_t id : embed_snd)
        {
            if (m_session_custom_snd_ids.count(id) != 0)
            {
                out_snd.insert(id);
            }
        }
    }

    void CollectSndIdsReferencedByInstrument(uint32_t inst_id, std::set<uint32_t> &out_snd) const
    {
        if (!m_bank_token)
        {
            return;
        }
        uint32_t resolved = 0;
        uint32_t index = 0;
        if (BAERmfEditorBank_ResolveInstID(m_bank_token, inst_id, &resolved, &index) !=
            BAE_NO_ERROR)
        {
            return;
        }
        uint32_t split_count = 0;
        if (BAERmfEditorBank_GetInstrumentSampleCount(m_bank_token, index, &split_count) !=
            BAE_NO_ERROR)
        {
            return;
        }
        for (uint32_t s = 0; s < split_count; ++s)
        {
            BAERmfEditorBankSampleInfo info;
            std::memset(&info, 0, sizeof(info));
            if (BAERmfEditorBank_GetInstrumentSampleInfo(m_bank_token, index, s, &info) ==
                BAE_NO_ERROR)
            {
                out_snd.insert(static_cast<uint32_t>(info.sndResourceID));
            }
        }
    }

    void ComputeUnusedCustomResourcesForSongDelete(const std::vector<int> &delete_indices,
                                                   std::set<uint32_t> &out_unused_inst,
                                                   std::set<uint32_t> &out_unused_snd) const
    {
        out_unused_inst.clear();
        out_unused_snd.clear();
        std::set<int> deleting(delete_indices.begin(), delete_indices.end());

        std::set<uint32_t> used_by_deleted_inst;
        std::set<uint32_t> used_by_deleted_snd;
        std::set<uint32_t> used_by_kept_inst;
        std::set<uint32_t> used_by_kept_snd;

        for (int i = 0; i < static_cast<int>(m_session_songs.size()); ++i)
        {
            const SessionSongEntry &entry = m_session_songs[static_cast<size_t>(i)];
            if (deleting.count(i) != 0)
            {
                CollectCustomResourcesUsedBySong(entry, used_by_deleted_inst, used_by_deleted_snd);
            }
            else
            {
                CollectCustomResourcesUsedBySong(entry, used_by_kept_inst, used_by_kept_snd);
            }
        }

        for (uint32_t id : used_by_deleted_inst)
        {
            if (used_by_kept_inst.count(id) == 0)
            {
                out_unused_inst.insert(id);
            }
        }
        for (uint32_t id : used_by_deleted_snd)
        {
            if (used_by_kept_snd.count(id) == 0)
            {
                out_unused_snd.insert(id);
            }
        }

        /* SNDs exclusive to instruments we're about to trash. */
        for (uint32_t inst_id : out_unused_inst)
        {
            std::set<uint32_t> from_inst;
            CollectSndIdsReferencedByInstrument(inst_id, from_inst);
            for (uint32_t snd_id : from_inst)
            {
                if (used_by_kept_snd.count(snd_id) != 0)
                {
                    continue;
                }
                /* Still used by a kept custom instrument? */
                bool kept_inst_uses = false;
                for (uint32_t keep_inst : used_by_kept_inst)
                {
                    std::set<uint32_t> keep_snd;
                    CollectSndIdsReferencedByInstrument(keep_inst, keep_snd);
                    if (keep_snd.count(snd_id) != 0)
                    {
                        kept_inst_uses = true;
                        break;
                    }
                }
                if (!kept_inst_uses)
                {
                    out_unused_snd.insert(snd_id);
                }
            }
        }
    }

    unsigned TrashCustomInstrumentsAndSamples(const std::set<uint32_t> &inst_ids,
                                              const std::set<uint32_t> &snd_ids)
    {
        if (!m_bank_token || (inst_ids.empty() && snd_ids.empty()))
        {
            return 0;
        }
        if (!EnsureActiveBankWritable())
        {
            return 0;
        }
        XFILE bank = reinterpret_cast<XFILE>(m_bank_token);
        std::set<uint32_t> snd_from_inst;
        for (uint32_t inst_id : inst_ids)
        {
            CollectSndIdsReferencedByInstrument(inst_id, snd_from_inst);
        }

        unsigned trashed = 0;
        for (uint32_t inst_id : inst_ids)
        {
            if (XTrashFileResource(bank, ID_INST, static_cast<XLongResourceID>(inst_id)) != FALSE)
            {
                m_session_custom_inst_ids.erase(inst_id);
                ++trashed;
            }
        }

        std::set<uint32_t> bank01_snd;
        std::set<uint32_t> bank2_snd;
        CollectBankReferencedSndIds(bank01_snd, bank2_snd);
        std::set<uint32_t> still_ref = bank01_snd;
        still_ref.insert(bank2_snd.begin(), bank2_snd.end());

        std::set<uint32_t> candidates = snd_ids;
        candidates.insert(snd_from_inst.begin(), snd_from_inst.end());
        static const XResourceType kSndTypes[] = {ID_ESND, ID_CSND, ID_SND, 0};
        for (uint32_t snd_id : candidates)
        {
            if (still_ref.count(snd_id) != 0)
            {
                continue;
            }
            const XLongResourceID sid = static_cast<XLongResourceID>(snd_id);
            bool ok = false;
            for (int t = 0; kSndTypes[t] != 0; ++t)
            {
                if (XExistsFileResource(bank, kSndTypes[t], sid) != FALSE)
                {
                    if (XTrashFileResource(bank, kSndTypes[t], sid) != FALSE)
                    {
                        ok = true;
                    }
                    break;
                }
            }
            if (ok)
            {
                m_session_custom_snd_ids.erase(snd_id);
                m_session_pcm_cache.erase(snd_id);
                m_app_compression_cache.erase(snd_id);
                ++trashed;
            }
        }
        return trashed;
    }

    void RequestDeleteSessionSongs(const std::vector<int> &indices)
    {
        m_pending_delete_song_indices.clear();
        for (int idx : indices)
        {
            if (idx >= 0 && idx < static_cast<int>(m_session_songs.size()))
            {
                m_pending_delete_song_indices.push_back(idx);
            }
        }
        if (m_pending_delete_song_indices.empty())
        {
            return;
        }
        std::sort(m_pending_delete_song_indices.begin(), m_pending_delete_song_indices.end());
        m_pending_delete_song_indices.erase(
            std::unique(m_pending_delete_song_indices.begin(), m_pending_delete_song_indices.end()),
            m_pending_delete_song_indices.end());

        std::set<uint32_t> unused_inst;
        std::set<uint32_t> unused_snd;
        ComputeUnusedCustomResourcesForSongDelete(m_pending_delete_song_indices,
                                                  unused_inst,
                                                  unused_snd);
        m_pending_delete_song_unused_inst = unused_inst.size();
        m_pending_delete_song_unused_snd = unused_snd.size();
        m_pending_delete_song_trash_unused =
            (m_pending_delete_song_unused_inst + m_pending_delete_song_unused_snd) > 0;
        m_confirm_delete_song_open = true;
    }

    void ConfirmDeletePendingSessionSongs()
    {
        if (m_pending_delete_song_indices.empty())
        {
            m_confirm_delete_song_open = false;
            return;
        }

        std::set<uint32_t> unused_inst;
        std::set<uint32_t> unused_snd;
        if (m_pending_delete_song_trash_unused)
        {
            ComputeUnusedCustomResourcesForSongDelete(m_pending_delete_song_indices,
                                                      unused_inst,
                                                      unused_snd);
        }

        std::vector<int> sorted = m_pending_delete_song_indices;
        std::sort(sorted.begin(), sorted.end(), std::greater<int>());
        const size_t song_count = sorted.size();
        for (int idx : sorted)
        {
            RemoveSessionSongAt(idx);
        }
        m_multi_songs.clear();

        unsigned trashed = 0;
        if (m_pending_delete_song_trash_unused && (!unused_inst.empty() || !unused_snd.empty()))
        {
            trashed = TrashCustomInstrumentsAndSamples(unused_inst, unused_snd);
            RefreshListsFromBank();
        }

        m_pending_delete_song_indices.clear();
        m_confirm_delete_song_open = false;

        char status[256];
        if (trashed > 0)
        {
            std::snprintf(status,
                          sizeof(status),
                          "Deleted %zu song%s; moved %u unused custom resource%s to Trash",
                          song_count,
                          song_count == 1 ? "" : "s",
                          trashed,
                          trashed == 1 ? "" : "s");
        }
        else
        {
            std::snprintf(status,
                          sizeof(status),
                          "Deleted %zu song%s",
                          song_count,
                          song_count == 1 ? "" : "s");
        }
        SetStatus(status);
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
    if (!m_bank_token || snd_id == kNoSndResourceId)
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
        if (!data)
        {
            continue;
        }
        XDisposePtr(data);
        (void)HydrateSessionPcmEntryFromCasd(static_cast<uint32_t>(rid));
    }
    m_session_casd_count = static_cast<int>(m_session_pcm_cache.size());
}

bool HasUncompressedMasterForSnd(uint32_t snd_id) const
{
    if (snd_id == kNoSndResourceId)
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
    m_midi_selected_track = -1;
    m_midi_selected_notes.clear();
    m_midi_primary_note = -1;
    m_midi_undo.clear();
    m_midi_redo.clear();
    m_midi_undo_pending_valid = false;
    m_midi_note_cache_dirty = true;
    m_midi_cached_notes.clear();
    m_midi_track_muted.clear();
    m_midi_track_solo.clear();
    m_midi_scroll_tick = 0.0f;

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
    m_velocity_curve_save_to_song = false;
    SyncPlayerMixNrpnsFromDocument();
    ApplyPlayerMixSettings();
    RefreshSongOverridesFromDocument();
    RefreshSongSamplesFromDocument();
    (void)BAERmfEditorDocument_SetInfo(m_document, TITLE_INFO, entry.name.c_str());
    return ReloadPlaybackFromDocument(true);
}

/* nBeT open-editor flags (version 2+). ImGui ini alone cannot reopen gated dialogs. */
static constexpr uint32_t kNbetOpenIE = 1u << 0;
static constexpr uint32_t kNbetOpenSE = 1u << 1;
static constexpr uint32_t kNbetOpenSongInfo = 1u << 2;
static constexpr uint32_t kNbetOpenSongSettings = 1u << 3;
static constexpr uint32_t kNbetOpenMidiEditor = 1u << 4;

void ApplyPlayerMixSettings()
{
    m_volume_percent = std::clamp(m_volume_percent, 0, 150);
    m_song_settings_tempo_percent = std::clamp(m_song_settings_tempo_percent, 25, 200);
    m_transpose_semitones = std::clamp(m_transpose_semitones, -24, 24);
    m_velocity_curve = std::clamp(m_velocity_curve, 0, 5);
    if (m_mixer)
    {
        BAEMixer_SetOutputGain(m_mixer, m_volume_percent);
        BAEMixer_SetDefaultReverb(m_mixer, static_cast<BAEReverbType>(m_reverb_type));
    }
    if (m_song)
    {
        const float ratio =
            static_cast<float>(std::max(1, m_song_settings_tempo_percent)) / 100.0f;
        (void)BAESong_SetMasterTempo(m_song, FLOAT_TO_UNSIGNED_FIXED(ratio));
        (void)BAESong_SetTranspose(m_song, m_transpose_semitones);
    }
    ApplyVelocityCurveToSongs();
}

/* Drop song/editors/bank-dependent UI before replacing the session bank token. */
void TeardownForSessionReload()
{
    if (m_mouse_active_note >= 0)
    {
        NoteOff(m_mouse_active_note);
        m_key_mouse_held[static_cast<size_t>(m_mouse_active_note)] = false;
        m_mouse_active_note = -1;
    }
    if (m_ie_note_song)
    {
        BAESong_KillActiveNotes(m_ie_note_song);
        m_ie_note_song = nullptr;
    }
    m_instrument_editor_open = false;
    m_ie_dirty = false;
    m_ie_undo.clear();
    m_ie_redo.clear();
#if NBEDITOR_MVP
    StopSampleEditorPreview();
    m_sample_editor_dirty = false;
    m_sample_editor_loop_dirty = false;
    m_sample_editor_compression_open = false;
    m_sample_editor_choose_note_open = false;
    m_sample_editor_open = false;
    ClearSampleEditorSndSnapshot();
#endif
    m_song_info_open = false;
    m_song_settings_open = false;
    m_midi_editor_open = false;
    MeClearMidiEditorState();
    m_export_rmf_open = false;
    m_export_sample_rmf_open = false;
    m_export_bank_open = false;
    m_export_audio_open = false;

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
    m_loaded_song_path.clear();
    m_loaded_doc_path.clear();
    m_loaded_session_path.clear();
    m_song_info_title[0] = 0;
    m_song_row = SongRow{};
    m_document_dirty = false;
    m_song_started = false;
    ClearSongOverrides();
    m_session_songs.clear();
    m_session_song_index = -1;
    m_session_pcm_cache.clear();
    m_app_compression_cache.clear();
}

std::vector<unsigned char> CaptureNbetLayoutBlob(bool include_layout = true)
{
    std::vector<unsigned char> blob;
    int x = 100;
    int y = 100;
    int w = 1280;
    int h = 820;
    uint32_t maximized = 0;
    uint32_t open_flags = 0;
    uint32_t ini_len = 0;
    const char *ini = nullptr;

    if (include_layout)
    {
        if (m_main_window)
        {
            SDL_GetWindowPosition(m_main_window, &x, &y);
            SDL_GetWindowSize(m_main_window, &w, &h);
            const SDL_WindowFlags flags = SDL_GetWindowFlags(m_main_window);
            if (flags & SDL_WINDOW_MAXIMIZED)
            {
                maximized = 1;
            }
        }
        /* Force a fresh dump of window + docking handlers into the ini blob. */
        ImGui::MarkIniSettingsDirty();
        size_t ini_size = 0;
        ini = ImGui::SaveIniSettingsToMemory(&ini_size);
        ini_len = (ini && ini_size > 0) ? static_cast<uint32_t>(ini_size) : 0;

        if (m_instrument_editor_open)
        {
            open_flags |= kNbetOpenIE;
        }
#if NBEDITOR_MVP
        if (m_sample_editor_open)
        {
            open_flags |= kNbetOpenSE;
        }
#endif
        if (m_song_info_open)
        {
            open_flags |= kNbetOpenSongInfo;
        }
        if (m_song_settings_open)
        {
            open_flags |= kNbetOpenSongSettings;
        }
        if (m_midi_editor_open)
        {
            open_flags |= kNbetOpenMidiEditor;
        }
    }

    /* Version 3: v2 + player mix (volume / tempo / reverb) before ImGui ini.
     * When include_layout is false (ZSN marker / prefs off), still store mix. */
    blob.reserve(76 + ini_len);
    NbetPutBE32(blob, FOUR_CHAR('n', 'B', 'e', 'T'));
    NbetPutBE32(blob, 3u);
    NbetPutBE32(blob, static_cast<uint32_t>(x));
    NbetPutBE32(blob, static_cast<uint32_t>(y));
    NbetPutBE32(blob, static_cast<uint32_t>(w));
    NbetPutBE32(blob, static_cast<uint32_t>(h));
    NbetPutBE32(blob, maximized);
    NbetPutBE32(blob, open_flags);
    if (include_layout)
    {
        NbetPutBE32(blob, m_ie_inst_id);
        NbetPutBE32(blob, m_ie_instrument_index);
        NbetPutBE32(blob, static_cast<uint32_t>(m_ie_bank));
        NbetPutBE32(blob, static_cast<uint32_t>(m_ie_program));
        NbetPutBE32(blob, m_ie_from_song ? 1u : 0u);
#if NBEDITOR_MVP
        NbetPutBE32(blob, static_cast<uint32_t>(m_sample_editor_sample_row));
        NbetPutBE32(blob, m_sample_editor_is_song_sample ? 1u : 0u);
#else
        NbetPutBE32(blob, static_cast<uint32_t>(-1));
        NbetPutBE32(blob, 0u);
#endif
    }
    else
    {
        NbetPutBE32(blob, 0u);
        NbetPutBE32(blob, 0u);
        NbetPutBE32(blob, 0u);
        NbetPutBE32(blob, 0u);
        NbetPutBE32(blob, 0u);
        NbetPutBE32(blob, static_cast<uint32_t>(-1));
        NbetPutBE32(blob, 0u);
    }
    NbetPutBE32(blob, static_cast<uint32_t>(m_volume_percent));
    NbetPutBE32(blob, static_cast<uint32_t>(m_song_settings_tempo_percent));
    NbetPutBE32(blob, static_cast<uint32_t>(m_reverb_type));
    NbetPutBE32(blob, ini_len);
    if (ini_len > 0 && ini)
    {
        blob.insert(blob.end(),
                    reinterpret_cast<const unsigned char *>(ini),
                    reinterpret_cast<const unsigned char *>(ini) + ini_len);
    }
    return blob;
}

void RestoreNbetEditorWindows()
{
    if (m_nbet_restore_open_flags == 0)
    {
        return;
    }
    const uint32_t flags = m_nbet_restore_open_flags;
    m_nbet_restore_open_flags = 0;

    if (flags & kNbetOpenIE)
    {
        int match = -1;
        for (int i = 0; i < static_cast<int>(m_instruments.size()); ++i)
        {
            const InstrumentRow &row = m_instruments[static_cast<size_t>(i)];
            if (m_nbet_restore_ie_inst_id != 0 && row.inst_id == m_nbet_restore_ie_inst_id)
            {
                match = i;
                break;
            }
            if (row.instrument_index == m_nbet_restore_ie_instrument_index &&
                row.bank == m_nbet_restore_ie_bank &&
                row.program == m_nbet_restore_ie_program)
            {
                match = i;
                break;
            }
        }
        if (match >= 0)
        {
            m_selected_instrument = match;
            m_selected_empty_slot = false;
            OpenInstrumentEditorForSelection();
        }
    }

#if NBEDITOR_MVP
    if (flags & kNbetOpenSE)
    {
        if (m_nbet_restore_se_sample_row >= 0 &&
            m_nbet_restore_se_sample_row < static_cast<int>(m_samples.size()))
        {
            m_selected_sample = m_nbet_restore_se_sample_row;
            OpenSampleEditorForSelection();
        }
    }
#endif

    if ((flags & kNbetOpenSongInfo) && m_document)
    {
        OpenSongInfoDialog();
    }
    if (flags & kNbetOpenSongSettings)
    {
        OpenSongSettingsDialog();
    }
    if ((flags & kNbetOpenMidiEditor) && m_document)
    {
        OpenMidiEditor();
    }
}

void ApplyNbetLayoutBlob(const unsigned char *data, size_t size)
{
    if (!data || size < 32)
    {
        return;
    }
    const uint32_t magic = NbetReadBE32(data + 0);
    const uint32_t version = NbetReadBE32(data + 4);
    if (magic != static_cast<uint32_t>(FOUR_CHAR('n', 'B', 'e', 'T')) ||
        (version != 1 && version != 2 && version != 3))
    {
        return;
    }
    const int32_t x = NbetReadBE32Signed(data + 8);
    const int32_t y = NbetReadBE32Signed(data + 12);
    const int32_t w = NbetReadBE32Signed(data + 16);
    const int32_t h = NbetReadBE32Signed(data + 20);
    const uint32_t maximized = NbetReadBE32(data + 24);

    size_t ini_off = 32;
    uint32_t ini_len = 0;
    m_nbet_restore_open_flags = 0;
    if (version == 1)
    {
        ini_len = NbetReadBE32(data + 28);
        ini_off = 32;
    }
    else if (version == 2)
    {
        /* v2 fixed header is 64 bytes before ini payload. */
        if (size < 64)
        {
            return;
        }
        m_nbet_restore_open_flags = NbetReadBE32(data + 28);
        m_nbet_restore_ie_inst_id = NbetReadBE32(data + 32);
        m_nbet_restore_ie_instrument_index = NbetReadBE32(data + 36);
        m_nbet_restore_ie_bank = NbetReadBE32Signed(data + 40);
        m_nbet_restore_ie_program = NbetReadBE32Signed(data + 44);
        m_nbet_restore_ie_from_song = (NbetReadBE32(data + 48) != 0);
        m_nbet_restore_se_sample_row = NbetReadBE32Signed(data + 52);
        m_nbet_restore_se_is_song = (NbetReadBE32(data + 56) != 0);
        ini_len = NbetReadBE32(data + 60);
        ini_off = 64;
    }
    else
    {
        /* v3: v2 fields + volume/tempo/reverb (76 bytes before ini). */
        if (size < 76)
        {
            return;
        }
        m_nbet_restore_open_flags = NbetReadBE32(data + 28);
        m_nbet_restore_ie_inst_id = NbetReadBE32(data + 32);
        m_nbet_restore_ie_instrument_index = NbetReadBE32(data + 36);
        m_nbet_restore_ie_bank = NbetReadBE32Signed(data + 40);
        m_nbet_restore_ie_program = NbetReadBE32Signed(data + 44);
        m_nbet_restore_ie_from_song = (NbetReadBE32(data + 48) != 0);
        m_nbet_restore_se_sample_row = NbetReadBE32Signed(data + 52);
        m_nbet_restore_se_is_song = (NbetReadBE32(data + 56) != 0);
        m_volume_percent = static_cast<int>(NbetReadBE32(data + 60));
        m_song_settings_tempo_percent = static_cast<int>(NbetReadBE32(data + 64));
        m_reverb_type = static_cast<int>(NbetReadBE32(data + 68));
        ini_len = NbetReadBE32(data + 72);
        ini_off = 76;
        ApplyPlayerMixSettings();
    }
    if (ini_off + ini_len > size)
    {
        return;
    }
    /* Player mix (v3) already applied; layout/editors are optional. */
    if (m_pref_ignore_session_layout)
    {
        m_nbet_restore_open_flags = 0;
        return;
    }
    if (m_main_window && w > 100 && h > 80)
    {
        /* Restore size (and maximize), but leave the window where it is unless
         * the current placement is off-screen (e.g. removed monitor). */
        SDL_SetWindowSize(m_main_window, w, h);
        if (maximized)
        {
            SDL_MaximizeWindow(m_main_window);
        }
        else
        {
            SDL_RestoreWindow(m_main_window);
            NbetEnsureMainWindowOnScreen(m_main_window, x, y, w, h);
        }
    }
    m_pending_imgui_ini.clear();
    if (ini_len > 0)
    {
        m_pending_imgui_ini.assign(reinterpret_cast<const char *>(data + ini_off),
                                   static_cast<size_t>(ini_len));
    }
    m_pending_apply_nbet = true;
    /* Wait for SDL resize + one frame with DockSpace/windows present.
     * LoadIniSettingsFromMemory must run outside NewFrame (see PumpPendingNbetLayout). */
    m_nbet_apply_delay_frames = 2;
}

void ResetSessionLayoutToDefault()
{
    m_pending_apply_nbet = false;
    m_nbet_apply_delay_frames = 0;
    m_pending_imgui_ini.clear();
    m_nbet_restore_open_flags = 0;

    m_instrument_editor_open = false;
#if NBEDITOR_MVP
    m_sample_editor_open = false;
    m_sample_editor_compression_open = false;
    m_sample_editor_choose_note_open = false;
#endif
    m_song_info_open = false;
    m_song_settings_open = false;
    m_midi_editor_open = false;
    MeClearMidiEditorState();
    m_export_rmf_open = false;
    m_export_sample_rmf_open = false;
    m_export_bank_open = false;
    m_export_audio_open = false;
    m_prefs_open = false;
    m_mod_import_prefs_open = false;
    m_session_info_open = false;
    m_resource_usage_open = false;
    m_bank_add_open = false;
    m_inst_dest_open = false;

    m_force_reset_dock_layout = true;
    m_dock_layout_initialized = false;
    SetStatus("Layout reset to default");
}

bool LoadSessionBsnFromPath(const char *path)
{
    if (!path || !path[0])
    {
        return false;
    }

    /* Copy path first — callers may pass m_loaded_session_path.c_str(). */
    const std::string path_owned(path);
    path = path_owned.c_str();

    std::vector<SessionSongEntry> songs;
    int casd_count = 0;
    if (!ProbeBe2SessionBsn(path, &songs, &casd_count))
    {
        return false;
    }

    /* Must release song/editors before UnloadBanks replaces the session token. */
    TeardownForSessionReload();

    /* Thin sessions (blank.bsn) may have no playable bank body.
     * Full replace: session file must become the active bank token. */
    const bool bank_ok = LoadBankFromPath(path, false);
    if (!bank_ok)
    {
        LoadBuiltinBankAsActive();
    }
    /* Teardown deleted the audition song; recreate for IE / instrument keys. */
    EnsureBankAuditionSong();

    /* Re-collect from the open bank token so ZREZ ZSNG/ZBNK songs are visible.
     * Probe may already have filled songs via a temporary XFILE; this keeps
     * load consistent with the active bank handle. */
    if (bank_ok && m_bank_token)
    {
        CollectSessionUserSongsFromBank(reinterpret_cast<XFILE>(m_bank_token),
                                        songs,
                                        casd_count);
    }

    /* NeoBAE nBeT: player mix always restored; window/ini/editors respect prefs. */
    {
        std::vector<unsigned char> bytes;
        if (ReadEntireFileBytes(path, bytes))
        {
            std::vector<BsnIrezResource> resources;
            if (ParseIrezResources(bytes.data(), bytes.size(), resources))
            {
                for (const BsnIrezResource &r : resources)
                {
                    if (BsnResourceTypeEquals(r.type, 'n', 'B', 'e', 'T') && r.id == 1 &&
                        !r.body.empty())
                    {
                        std::vector<unsigned char> plain;
                        if (DecodeNbetResourceBody(r.body.data(), r.body.size(), plain))
                        {
                            ApplyNbetLayoutBlob(plain.data(), plain.size());
                        }
                        break;
                    }
                }
            }
        }
    }

    m_session_songs = std::move(songs);
    m_session_casd_count = casd_count;
    m_loaded_session_path = path_owned;
    m_session_song_index = -1;
    RememberRecentSession(path);
    HydrateSessionPcmCacheFromBankCasd();

    char status[256];
    const char *kind = EndsWith(path_owned, ".zsn") ? "Session .zsn" : "Session .bsn";
    if (m_session_songs.empty())
    {
        ApplyPlayerMixSettings();
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
    ApplyPlayerMixSettings();
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

    /* Force paints — throttle would skip these right after BeginBusyProgress. */
    m_busy_last_pump_ms = 0;
    UpdateBusyProgress(0.02f, "Preparing session…");
    RememberSessionPcmFromSampleEditor();
    (void)SyncSessionSongsFromDocument();
    /* Module/RMF song embeds live in rmf_bytes until promoted. Session files
     * only store bank + Midi/SONG — promote so customs survive save→reload. */
    m_busy_last_pump_ms = 0;
    UpdateBusyProgress(0.05f, "Promoting song instruments into bank…");
    (void)PromoteAllSessionSongEmbedsIntoBank();

    m_busy_last_pump_ms = 0;
    UpdateBusyProgress(0.07f, "Checking session format…");
    /* Boolean probe only — full reason scan is reserved for remap / status text. */
    uint32_t z_reason = 0;
    const bool requires_zsn = SessionRequiresZsn(nullptr);
    bool remapped = false;
    const std::string enforced_path =
        EnforceSessionSavePath(path, &remapped, requires_zsn ? 1 : 0);
    path = enforced_path.c_str();

    /* Container follows extension: .zsn → ZREZ even when content wouldn't force it.
     * Save As to .bsn can still write IREZ when features allow. */
    const bool want_zsn = requires_zsn || EndsWith(enforced_path, ".zsn");
    const int32_t resource_map_id = want_zsn ? XFILERESOURCE_ZMF_ID : XFILERESOURCE_ID;
    if (remapped)
    {
        /* Full reason scan only when remapping (busy UI already visible). */
        (void)SessionRequiresZsn(&z_reason);
        char reason_buf[256] = {0};
        BAEZMFReasonCodeToString(z_reason, reason_buf, sizeof(reason_buf));
        SetStatus(std::string("NeoBAE features require .zsn (") + reason_buf + "); remapping save path");
    }

    XFILE out_file = nullptr;
    /* ---- bank body (optional): seed from a one-shot image clone ---- */
    if (m_bank_token)
    {
        UpdateBusyProgress(0.08f, "Cloning bank image…");
        unsigned char *bank_image = nullptr;
        uint32_t bank_image_size = 0;
        /* Same-format is a raw image memcpy (keeps ZINS packed). Format
         * override (IREZ↔ZREZ) walks resources once. */
        const BAEResult snap_r =
            BAERmfEditorBank_SaveToMemoryEx(m_bank_token,
                                            resource_map_id,
                                            &bank_image,
                                            &bank_image_size);
        if (snap_r != BAE_NO_ERROR || !bank_image || bank_image_size < 12)
        {
            if (bank_image)
            {
                XDisposePtr(bank_image);
            }
            SetStatus(std::string("Session save failed: clone bank: ") + FormatBAEError(snap_r));
            return false;
        }
        out_file = XFileOpenWritableResourceFromMemory(bank_image, bank_image_size);
        XDisposePtr(bank_image);
        if (!out_file)
        {
            SetStatus("Session save failed: open bank clone");
            return false;
        }

        UpdateBusyProgress(0.35f, "Stripping session songs from bank clone…");
        std::set<uint32_t> session_song_ids;
        std::set<uint32_t> session_midi_ids;
        for (const SessionSongEntry &entry : m_session_songs)
        {
            session_song_ids.insert(entry.song_resource_id);
            session_midi_ids.insert(entry.midi_resource_id);
        }
        /* One-shot raw omit — never loop XPurgeFileResource (each purge ran
         * Clean/PackInst and dropped flat Bank 2+/custom INST from the clone). */
        {
            std::vector<XResourceType> omit_types;
            std::vector<XLongResourceID> omit_ids;
            auto omit_one = [&](XResourceType t, XLongResourceID id) {
                omit_types.push_back(t);
                omit_ids.push_back(id);
            };

            const int32_t song_count = XCountFileResourcesOfType(out_file, ID_SONG);
            for (int32_t i = 0; i < song_count; ++i)
            {
                XLongResourceID song_id = 0;
                int32_t song_size = 0;
                char name_buf[256];
                name_buf[0] = 0;
                XPTR song_data =
                    XGetIndexedFileResource(out_file, ID_SONG, &song_id, i, name_buf, &song_size);
                if (!song_data)
                {
                    continue;
                }
                bool strip = false;
                if (BsnSongObjectIsMidi(out_file,
                                        static_cast<const unsigned char *>(song_data),
                                        song_size))
                {
                    if (session_song_ids.count(static_cast<uint32_t>(song_id)) != 0)
                    {
                        strip = true;
                    }
                    else
                    {
                        const uint16_t obj_id =
                            BsnReadBE16(static_cast<const unsigned char *>(song_data));
                        if (session_midi_ids.count(obj_id) != 0)
                        {
                            strip = true;
                        }
                    }
                }
                XDisposePtr(song_data);
                if (strip)
                {
                    omit_one(ID_SONG, song_id);
                }
            }
            for (uint32_t midi_id : session_midi_ids)
            {
                if (midi_id != 0)
                {
                    omit_one(ID_MIDI, static_cast<XLongResourceID>(midi_id));
                }
            }
            omit_one(FOUR_CHAR('n', 'B', 'e', 'T'), 1);
            omit_one(FOUR_CHAR('B', 'e', 'P', 'f'), 1);
            omit_one(FOUR_CHAR('B', 'E', 'P', 'F'), 1);
            omit_one(FOUR_CHAR('D', 'A', 'T', 'e'), 1);
            {
                const int32_t casd_count =
                    XCountFileResourcesOfType(out_file, FOUR_CHAR('C', 'a', 'S', 'd'));
                for (int32_t i = 0; i < casd_count; ++i)
                {
                    XLongResourceID casd_id = 0;
                    int32_t sz = 0;
                    char nm[256];
                    nm[0] = 0;
                    XPTR d = XGetIndexedFileResource(
                        out_file, FOUR_CHAR('C', 'a', 'S', 'd'), &casd_id, i, nm, &sz);
                    if (d)
                    {
                        XDisposePtr(d);
                    }
                    omit_one(FOUR_CHAR('C', 'a', 'S', 'd'), casd_id);
                }
            }
            if (!omit_types.empty() &&
                XOmitFileResources(out_file,
                                   omit_types.data(),
                                   omit_ids.data(),
                                   static_cast<int32_t>(omit_types.size())) == FALSE)
            {
                XFileClose(out_file);
                SetStatus("Session save failed: strip session resources");
                return false;
            }
        }
    }
    else
    {
        out_file = XFileOpenVirtualResource(resource_map_id);
        if (!out_file)
        {
            SetStatus("Session save failed: virtual resource");
            return false;
        }
        /* Thin shell like blank.bsn */
        static const unsigned char k_zero6[6] = {0, 0, 0, 0, 0, 0};
        char empty_pascal[256];
        empty_pascal[0] = 0;
        (void)XAddFileResource(out_file, ID_VERS, 0, empty_pascal, (void *)k_zero6, 6);
        (void)XAddFileResource(out_file, ID_BANK, 0, empty_pascal, (void *)k_zero6, 6);
    }

    /* ---- user songs (Midi + SONG) ---- */
    UpdateBusyProgress(0.55f, "Writing session songs…");
    struct DateStamp
    {
        uint32_t type;
        uint32_t id;
    };
    std::vector<DateStamp> date_stamps;
    const uint32_t now_ts = static_cast<uint32_t>(std::time(nullptr));

    for (size_t i = 0; i < m_session_songs.size(); ++i)
    {
        if (!m_session_songs.empty())
        {
            UpdateBusyProgress(0.55f + 0.12f * (static_cast<float>(i) /
                                                static_cast<float>(m_session_songs.size())),
                               "Writing session songs…");
        }
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
    UpdateBusyProgress(0.70f, "Writing PCM masters…");
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

    /* ---- nBeT / BePf / BEPF / DATe ---- */
    UpdateBusyProgress(0.80f, "Writing session metadata…");
    {
        /* .zsn always writes nBeT (session marker + player mix). Layout/dock/ini
         * follow the save-layout pref. Classic .bsn only writes nBeT when that
         * pref is on; BePf marks the BE2 session instead. */
        if (want_zsn || m_pref_save_session_layout)
        {
            std::vector<unsigned char> nbet =
                CaptureNbetLayoutBlob(m_pref_save_session_layout);
            if (!nbet.empty())
            {
                std::vector<unsigned char> stored = EncodeNbetResourceBody(nbet);
                char nbet_pascal[256];
                BsnMakePascalName("NeoBAE Editor", nbet_pascal);
                (void)XAddFileResource(out_file,
                                       FOUR_CHAR('n', 'B', 'e', 'T'),
                                       1,
                                       nbet_pascal,
                                       stored.data(),
                                       static_cast<int32_t>(stored.size()));
                date_stamps.push_back({FOUR_CHAR('n', 'B', 'e', 'T'), 1});
            }
        }

        /* BEPF / BePf are Beatnik Editor session/bank trailers. Classic .bsn
         * keeps them for BE2; .zsn is NeoBAE-only (detect via nBeT). */
        if (!want_zsn)
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
        }

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

    UpdateBusyProgress(0.88f, want_zsn ? "Packing ZREZ (LZMA)…" : "Packing resource map…");
    /* Never PackInst here: flat Bank 2+/custom INST must survive. PackSong/Bank
     * only; skip ZSHD too (payload-refs without headers lose samples). */
    if (XCleanResourceFileOptions(out_file, FALSE, FALSE) == FALSE)
    {
        /* Still try to write the in-memory image. */
    }

    /* Fail closed if promoted custom INST did not make it into the clone. */
    if (m_bank_token && !m_session_custom_inst_ids.empty())
    {
        std::vector<uint32_t> missing;
        for (uint32_t inst_id : m_session_custom_inst_ids)
        {
            int32_t sz = 0;
            char name[256];
            name[0] = 0;
            XPTR data = XGetFileResource(out_file,
                                         ID_INST,
                                         static_cast<XLongResourceID>(inst_id),
                                         name,
                                         &sz);
            if (!data)
            {
                missing.push_back(inst_id);
            }
            else
            {
                XDisposePtr(data);
            }
        }
        if (!missing.empty())
        {
            XFileClose(out_file);
            char buf[160];
            std::snprintf(buf,
                          sizeof(buf),
                          "Session save failed: %zu custom instrument(s) missing after pack (e.g. id %u)",
                          missing.size(),
                          missing.front());
            SetStatus(buf);
            return false;
        }
    }

    UpdateBusyProgress(0.94f, "Writing session file…");
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
    UpdateBusyProgress(1.0f, "Done");

    m_loaded_session_path = path;
    m_document_dirty = false;
    m_session_casd_count = static_cast<int>(m_session_pcm_cache.size());
    RememberRecentSession(path);

    char status[320];
    if (want_zsn)
    {
        if (z_reason != 0)
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
                          "Session saved (.zsn): %zu song(s), %d CaSd",
                          m_session_songs.size(),
                          m_session_casd_count);
        }
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
