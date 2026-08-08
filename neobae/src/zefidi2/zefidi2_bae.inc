/* Playback / bank lifecycle (methods of Zefidi2App). */

    static BAERate RateFromHz(int hz)
    {
        switch (hz)
        {
        case 11025:
            return BAE_RATE_11K;
        case 22050:
            return BAE_RATE_22K;
        case 48000:
            return BAE_RATE_48K;
        case 44100:
        default:
            return BAE_RATE_44K;
        }
    }

    static std::string PathBasename(const std::string &path)
    {
        if (path.empty())
        {
            return {};
        }
        const size_t slash = path.find_last_of("/\\");
        return (slash == std::string::npos) ? path : path.substr(slash + 1);
    }

    static bool EndsWithIgnoreCase(const std::string &value, const char *suffix)
    {
        if (!suffix || !suffix[0])
        {
            return false;
        }
        const size_t n = std::strlen(suffix);
        if (value.size() < n)
        {
            return false;
        }
        for (size_t i = 0; i < n; ++i)
        {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(value[value.size() - n + i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
            if (a != b)
            {
                return false;
            }
        }
        return true;
    }

    static bool IsBankPath(const std::string &path)
    {
        return EndsWithIgnoreCase(path, ".hsb") || EndsWithIgnoreCase(path, ".zsb") ||
               EndsWithIgnoreCase(path, ".sf2") || EndsWithIgnoreCase(path, ".sf3") ||
               EndsWithIgnoreCase(path, ".sfo") || EndsWithIgnoreCase(path, ".dls");
    }

    static bool IsAudioFileType(BAEFileType t)
    {
        switch (t)
        {
        case BAE_WAVE_TYPE:
        case BAE_AIFF_TYPE:
        case BAE_AU_TYPE:
        case BAE_MPEG_TYPE:
#if USE_FLAC_DECODER == TRUE || USE_FLAC_ENCODER == TRUE
        case BAE_FLAC_TYPE:
#endif
#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE
        case BAE_VORBIS_TYPE:
#endif
#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
        case BAE_OPUS_TYPE:
#endif
#if USE_ADP_SUPPORT == TRUE
        case BAE_ADP_TYPE:
#endif
#if USE_ADX_SUPPORT == TRUE
        case BAE_ADX_TYPE:
#endif
#if USE_QOA_SUPPORT == TRUE
        case BAE_QOA_TYPE:
#endif
#if USE_WMA_SUPPORT == TRUE
        case BAE_WMA_TYPE:
#endif
            return true;
        default:
            return false;
        }
    }

    /* Prefer RMF/ZMF TITLE_INFO (same source as RMF Info dialog). BAESong_GetTitle
     * often synthesizes "Untitled <id>" for containers that only store meta in INFO. */
    std::string ResolveSongDisplayTitle(const std::string &path, bool is_rmf)
    {
        if (is_rmf && !path.empty())
        {
            char buf[512] = {};
            if (BAEUtil_GetRmfSongInfoFromFile((BAEPathName)path.c_str(), 0, TITLE_INFO, buf,
                                               sizeof(buf)) == BAE_NO_ERROR &&
                buf[0])
            {
                return std::string(buf);
            }
        }
        if (m_song)
        {
            char title[256] = {};
            if (BAESong_GetTitle(m_song, title, static_cast<int>(sizeof(title))) == BAE_NO_ERROR &&
                title[0])
            {
                /* Skip engine placeholder "Untitled N" / "Unknown N". */
                const bool placeholder =
                    (std::strncmp(title, "Untitled", 8) == 0) ||
                    (std::strncmp(title, "Unknown", 7) == 0);
                if (!placeholder)
                {
                    return std::string(title);
                }
            }
        }
        return PathBasename(path);
    }

    void SetStatus(const std::string &msg)
    {
        m_status = msg;
    }

    const char *GetStatus() const
    {
        return m_status.c_str();
    }

    void UnloadMedia()
    {
#if SUPPORT_KARAOKE == TRUE
        ClearKaraokeCallbacks();
        KaraokeReset();
#endif
        if (m_song)
        {
            BAESong_Stop(m_song, FALSE);
            BAESong_Delete(m_song);
            m_song = nullptr;
        }
        if (m_sound)
        {
            BAESound_Stop(m_sound, FALSE);
            BAESound_Delete(m_sound);
            m_sound = nullptr;
        }
        m_song_loaded = false;
        m_is_audio_file = false;
        m_is_rmf_file = false;
        m_song_started = false;
        m_playing = false;
        m_loaded_media_path.clear();
        m_song_title.clear();
    }

    void EnsureLiveSong()
    {
        if (!m_mixer || m_live_song)
        {
            return;
        }
        m_live_song = BAESong_New(m_mixer);
        if (m_live_song)
        {
            (void)BAESong_Preroll(m_live_song);
        }
    }

    void DeleteLiveSong()
    {
        if (m_live_song)
        {
            BAESong_Stop(m_live_song, FALSE);
            BAESong_Delete(m_live_song);
            m_live_song = nullptr;
        }
    }

    BAESong ActiveNoteSong()
    {
        if (m_song && !m_is_audio_file)
        {
            return m_song;
        }
        EnsureLiveSong();
        return m_live_song;
    }

    bool CreateMixer()
    {
        if (m_mixer)
        {
            return true;
        }
        m_mixer = BAEMixer_New();
        if (!m_mixer)
        {
            SetStatus("BAEMixer_New failed");
            return false;
        }
        BAEAudioModifiers mods = BAE_USE_16;
        if (m_stereo)
        {
            mods |= BAE_USE_STEREO;
        }
        const int16_t voices = static_cast<int16_t>(std::clamp(m_midi_voices, 8, 128));
        const BAEResult open_result =
            BAEMixer_Open(m_mixer, RateFromHz(m_sample_rate_hz), BAE_LINEAR_INTERPOLATION, mods,
                          voices, 16, 32, TRUE);
        if (open_result != BAE_NO_ERROR)
        {
            BAEMixer_Delete(m_mixer);
            m_mixer = nullptr;
            SetStatus("BAEMixer_Open failed");
            return false;
        }
        ApplyVolume();
        ApplyReverb();
        ApplyEq();
        EnsureLiveSong();
        return true;
    }

    void DestroyMixer()
    {
        UnloadMedia();
        DeleteLiveSong();
        if (m_mixer)
        {
            BAEMixer_Close(m_mixer);
            BAEMixer_Delete(m_mixer);
            m_mixer = nullptr;
        }
    }

    bool LoadBuiltinBank()
    {
        if (!m_mixer)
        {
            return false;
        }
#if _BUILT_IN_PATCHES == TRUE
        BAEBankLoadInfo info{};
        if (BAEMixer_LoadBankBuiltinOnly(m_mixer, &info) != BAE_NO_ERROR)
        {
            SetStatus("Builtin bank load failed");
            return false;
        }
        m_loaded_bank_path = "__builtin__";
        m_loaded_bank_display = "Builtin";
        SetStatus("Loaded builtin bank");
        return true;
#else
        BAEBankToken token = 0;
        if (BAEMixer_LoadBuiltinBank(m_mixer, &token) != BAE_NO_ERROR)
        {
            SetStatus("Builtin bank unavailable");
            return false;
        }
        m_bank_token = token;
        m_loaded_bank_path = "__builtin__";
        m_loaded_bank_display = "Builtin";
        SetStatus("Loaded builtin bank");
        return true;
#endif
    }

    bool LoadBankFromPath(const char *path)
    {
        if (!m_mixer || !path || !path[0])
        {
            return false;
        }
        if (std::strcmp(path, "__builtin__") == 0)
        {
            return LoadBuiltinBank();
        }
        BAEBankLoadInfo info{};
        const BAEResult r = BAEMixer_LoadBankFromPath(m_mixer, (BAEPathName)path, &info);
        if (r != BAE_NO_ERROR)
        {
            SetStatus(std::string("Bank load failed: ") + PathBasename(path));
            return false;
        }
        m_loaded_bank_path = path;
        m_loaded_bank_display = PathBasename(path);
        SetStatus(std::string("Bank: ") + m_loaded_bank_display);
        SaveSettings();
        return true;
    }

    bool TryLoadDefaultBank()
    {
        if (!m_mixer)
        {
            return false;
        }
        if (!m_settings_last_bank.empty() && LoadBankFromPath(m_settings_last_bank.c_str()))
        {
            return true;
        }
        /* Probe next to executable / cwd. */
        static const char *kCandidates[] = {
            "patches.hsb", "patches.zsb", "patches.sf2", "patches.dls",
            "Patches.hsb", "Patches.zsb",
        };
        for (const char *name : kCandidates)
        {
            FILE *fp = std::fopen(name, "rb");
            if (fp)
            {
                std::fclose(fp);
                if (LoadBankFromPath(name))
                {
                    return true;
                }
            }
        }
        return LoadBuiltinBank();
    }

    void ApplyVolume()
    {
        if (!m_mixer)
        {
            return;
        }
        int pct = std::clamp(m_volume_percent, 0, 150);
        const double normalized = static_cast<double>(pct) / 100.0;
        const double engine_gain = (normalized * normalized) * 1.0;
        if (m_is_audio_file && m_sound)
        {
            double sound_gain = engine_gain * 3.0 * (1.0 + normalized) * 0.25;
            if (sound_gain < 0.0)
            {
                sound_gain = 0.0;
            }
            (void)BAESound_SetVolume(m_sound, FLOAT_TO_UNSIGNED_FIXED(sound_gain));
        }
        else
        {
            (void)BAEMixer_SetGlobalVolume(m_mixer, FLOAT_TO_UNSIGNED_FIXED(engine_gain));
        }
        (void)BAEMixer_SetOutputGain(m_mixer, pct > 100 ? 100 : pct);
    }

    void ApplyTempo()
    {
        if (!m_song || m_is_audio_file)
        {
            return;
        }
        const int pct = std::clamp(m_tempo_percent, 25, 200);
        (void)BAESong_SetMasterTempo(m_song, FLOAT_TO_UNSIGNED_FIXED(pct / 100.0));
    }

    void ApplyTranspose()
    {
        if (!m_song || m_is_audio_file)
        {
            return;
        }
        (void)BAESong_SetTranspose(m_song, static_cast<int32_t>(std::clamp(m_transpose, -24, 24)));
    }

    void ApplyLoop()
    {
        if (m_is_audio_file && m_sound)
        {
            (void)BAESound_SetLoopCount(m_sound, m_loop_enabled ? 0xFFFFFFFFu : 0u);
        }
        else if (m_song)
        {
            (void)BAESong_SetLoops(m_song, m_loop_enabled ? 32767 : 0);
        }
    }

    void ApplyReverb()
    {
        if (!m_mixer)
        {
            return;
        }
        int t = std::clamp(m_reverb_type, static_cast<int>(BAE_REVERB_TYPE_1),
                           static_cast<int>(BAE_REVERB_TYPE_19));
        (void)BAEMixer_SetDefaultReverb(m_mixer, static_cast<BAEReverbType>(t));
    }

    void ApplyVelocityCurve()
    {
        if (!m_song || m_is_audio_file)
        {
            return;
        }
        (void)BAESong_SetVelocityCurve(m_song, std::clamp(m_velocity_curve, 0, 5));
    }

    void ApplyChannelMutes()
    {
        if (!m_song || m_is_audio_file)
        {
            return;
        }
        for (int ch = 0; ch < 16; ++ch)
        {
            const bool muted = ChannelEffectivelyMuted(ch);
            if (muted)
            {
                (void)BAESong_MuteChannel(m_song, static_cast<uint16_t>(ch));
            }
            else
            {
                (void)BAESong_UnmuteChannel(m_song, static_cast<uint16_t>(ch));
            }
        }
    }

    bool ChannelEffectivelyMuted(int ch) const
    {
        if (ch < 0 || ch >= 16)
        {
            return true;
        }
        if (m_channel_solo >= 0)
        {
            return m_channel_solo != ch;
        }
        return m_channel_muted[static_cast<size_t>(ch)];
    }

    void ApplyEq()
    {
        if (!m_mixer)
        {
            return;
        }
        (void)BAEMixer_SetEQEnabled(m_mixer, m_eq_enabled ? TRUE : FALSE);
        for (uint32_t i = 0; i < 5; ++i)
        {
            (void)BAEMixer_SetEQGain(m_mixer, i, m_eq_gains[i]);
        }
    }

    void ApplyCustomReverbParams()
    {
        SetNeoCustomReverbCombCount(std::clamp(m_custom_comb_count, 1, 8));
        for (int i = 0; i < m_custom_comb_count && i < 8; ++i)
        {
            SetNeoCustomReverbCombDelay(i, m_custom_comb_delay[i]);
            SetNeoCustomReverbCombFeedback(i, m_custom_comb_feedback[i]);
            SetNeoCustomReverbCombGain(i, m_custom_comb_gain[i]);
        }
        SetNeoCustomReverbLowpass(m_custom_lowpass);
    }

    void ApplyAllMixSettings()
    {
        ApplyVolume();
        ApplyTempo();
        ApplyTranspose();
        ApplyLoop();
        ApplyReverb();
        ApplyVelocityCurve();
        ApplyChannelMutes();
        ApplyEq();
        if (m_reverb_type == static_cast<int>(BAE_REVERB_TYPE_19))
        {
            ApplyCustomReverbParams();
        }
    }

    bool LoadMediaFromPath(const char *path)
    {
        if (!m_mixer || !path || !path[0])
        {
            return false;
        }
        if (IsBankPath(path))
        {
            return LoadBankFromPath(path);
        }

        UnloadMedia();
        const BAEFileType ftype = X_DetermineFileType(path);

        if (IsAudioFileType(ftype))
        {
            m_sound = BAESound_New(m_mixer);
            if (!m_sound)
            {
                SetStatus("BAESound_New failed");
                return false;
            }
            if (BAESound_LoadFileSample(m_sound, (BAEPathName)path, ftype) != BAE_NO_ERROR)
            {
                BAESound_Delete(m_sound);
                m_sound = nullptr;
                SetStatus(std::string("Audio load failed: ") + PathBasename(path));
                return false;
            }
            m_is_audio_file = true;
            m_song_loaded = true;
            m_loaded_media_path = path;
            m_song_title = PathBasename(path);
            ApplyAllMixSettings();
            SetStatus(std::string("Loaded: ") + m_song_title);
            return true;
        }

        m_song = BAESong_New(m_mixer);
        if (!m_song)
        {
            SetStatus("BAESong_New failed");
            return false;
        }

        BAEResult lr = BAE_GENERAL_ERR;
        bool is_rmf = false;
        if (ftype == BAE_RMF || EndsWithIgnoreCase(path, ".rmf") || EndsWithIgnoreCase(path, ".zmf"))
        {
            lr = BAESong_LoadRmfFromFile(m_song, (BAEPathName)path, 0, TRUE);
            is_rmf = (lr == BAE_NO_ERROR);
        }
#if USE_XMF_SUPPORT == TRUE
        else if (ftype == BAE_XMF || EndsWithIgnoreCase(path, ".xmf") || EndsWithIgnoreCase(path, ".mxmf"))
        {
            lr = BAESong_LoadXmfFromFile(m_song, (BAEPathName)path, TRUE);
        }
#endif
#if USE_RMI_SUPPORT == TRUE
        else if (ftype == BAE_RMI || EndsWithIgnoreCase(path, ".rmi"))
        {
            lr = BAESong_LoadRmiFromFile(m_song, (BAEPathName)path, TRUE, TRUE);
            if (lr != BAE_NO_ERROR)
            {
                lr = BAESong_LoadRmiFromFile(m_song, (BAEPathName)path, TRUE, FALSE);
            }
        }
#endif
        else
        {
            lr = BAESong_LoadMidiFromFile(m_song, (BAEPathName)path, TRUE);
        }

        if (lr != BAE_NO_ERROR)
        {
            /* Fallback: try MIDI then RMF. */
            lr = BAESong_LoadMidiFromFile(m_song, (BAEPathName)path, TRUE);
            if (lr != BAE_NO_ERROR)
            {
                lr = BAESong_LoadRmfFromFile(m_song, (BAEPathName)path, 0, TRUE);
                is_rmf = (lr == BAE_NO_ERROR);
            }
        }

        if (lr != BAE_NO_ERROR)
        {
            BAESong_Delete(m_song);
            m_song = nullptr;
            SetStatus(std::string("Load failed: ") + PathBasename(path));
            return false;
        }

        m_is_rmf_file = is_rmf;
        m_song_loaded = true;
        m_loaded_media_path = path;
        m_song_title = ResolveSongDisplayTitle(path, is_rmf);
        (void)BAESong_Preroll(m_song);
#if SUPPORT_KARAOKE == TRUE
        SetupKaraokeCallbacks();
#endif
#if SUPPORT_BAESCRIPT == TRUE
        BindScriptToSong();
#endif
        ApplyAllMixSettings();
        SetStatus(std::string("Loaded: ") + m_song_title);
        return true;
    }

    bool PlayPause()
    {
        if (!m_song_loaded)
        {
            return false;
        }
        if (m_is_audio_file && m_sound)
        {
            if (!m_playing)
            {
                ApplyLoop();
                ApplyVolume();
                if (BAESound_Start(m_sound, 0, FLOAT_TO_UNSIGNED_FIXED(1.0), 0) != BAE_NO_ERROR)
                {
                    SetStatus("Sound start failed");
                    return false;
                }
                m_playing = true;
                m_song_started = true;
            }
            else
            {
                (void)BAESound_Stop(m_sound, FALSE);
                m_playing = false;
            }
            return true;
        }
        if (!m_song)
        {
            return false;
        }
        BAE_BOOL paused = FALSE;
        (void)BAESong_IsPaused(m_song, &paused);
        BAE_BOOL done = FALSE;
        (void)BAESong_IsDone(m_song, &done);

        if (!m_song_started || done)
        {
            ApplyLoop();
            ApplyVelocityCurve();
            (void)BAESong_SetMicrosecondPosition(m_song, 0);
            (void)BAESong_Preroll(m_song);
            if (BAESong_Start(m_song, 0) != BAE_NO_ERROR)
            {
                SetStatus("Song start failed");
                return false;
            }
            m_song_started = true;
            m_playing = true;
        }
        else if (paused || !m_playing)
        {
            (void)BAESong_Resume(m_song);
            m_playing = true;
        }
        else
        {
            (void)BAESong_Pause(m_song);
            m_playing = false;
        }
        return true;
    }

    void Stop()
    {
        if (m_is_audio_file && m_sound)
        {
            (void)BAESound_Stop(m_sound, FALSE);
            (void)BAESound_SetSamplePlaybackPosition(m_sound, 0);
        }
        else if (m_song)
        {
            (void)BAESong_Stop(m_song, FALSE);
            (void)BAESong_SetMicrosecondPosition(m_song, 0);
            (void)BAESong_Preroll(m_song);
        }
        m_playing = false;
        m_song_started = false;
    }

    uint32_t GetPosMs() const
    {
        if (m_is_audio_file && m_sound)
        {
            uint32_t frames = 0;
            BAESampleInfo info{};
            if (BAESound_GetInfo(m_sound, &info) == BAE_NO_ERROR && info.sampledRate > 0)
            {
                (void)BAESound_GetSamplePlaybackPosition(m_sound, &frames);
                const double hz = UNSIGNED_FIXED_TO_LONG_ROUNDED(info.sampledRate);
                if (hz > 0.0)
                {
                    return static_cast<uint32_t>((frames * 1000.0) / hz);
                }
            }
            return 0;
        }
        if (m_song)
        {
            uint32_t us = 0;
            (void)BAESong_GetMicrosecondPosition(m_song, &us);
            return us / 1000u;
        }
        return 0;
    }

    uint32_t GetLenMs() const
    {
        if (m_is_audio_file && m_sound)
        {
            BAESampleInfo info{};
            if (BAESound_GetInfo(m_sound, &info) == BAE_NO_ERROR && info.sampledRate > 0)
            {
                const double hz = UNSIGNED_FIXED_TO_LONG_ROUNDED(info.sampledRate);
                if (hz > 0.0 && info.waveFrames > 0)
                {
                    return static_cast<uint32_t>((info.waveFrames * 1000.0) / hz);
                }
            }
            return 0;
        }
        if (m_song)
        {
            uint32_t us = 0;
            (void)BAESong_GetMicrosecondLength(m_song, &us);
            return us / 1000u;
        }
        return 0;
    }

    void SeekMs(uint32_t ms)
    {
        if (m_is_audio_file && m_sound)
        {
            BAESampleInfo info{};
            if (BAESound_GetInfo(m_sound, &info) == BAE_NO_ERROR && info.sampledRate > 0)
            {
                const double hz = UNSIGNED_FIXED_TO_LONG_ROUNDED(info.sampledRate);
                const uint32_t frames = static_cast<uint32_t>((ms / 1000.0) * hz);
                (void)BAESound_SetSamplePlaybackPosition(m_sound, frames);
            }
            return;
        }
        if (m_song)
        {
            (void)BAESong_SetMicrosecondPosition(m_song, ms * 1000u);
        }
    }

    bool IsSongFinished() const
    {
        if (!m_song_loaded || !m_playing)
        {
            return false;
        }
        if (m_is_audio_file && m_sound)
        {
            BAE_BOOL done = FALSE;
            (void)BAESound_IsDone(m_sound, &done);
            return done == TRUE;
        }
        if (m_song)
        {
            BAE_BOOL done = FALSE;
            (void)BAESong_IsDone(m_song, &done);
            return done == TRUE;
        }
        return false;
    }

    void PollPlaybackFinished()
    {
        if (!IsSongFinished())
        {
            return;
        }
        m_playing = false;
#if SUPPORT_PLAYLIST == TRUE
        if (m_playlist_enabled && !m_loop_enabled)
        {
            const int next = PlaylistNextForEndOfSong();
            if (next >= 0)
            {
                PlaylistPlayIndex(next, true);
                return;
            }
        }
#endif
        if (m_loop_enabled && m_song_loaded)
        {
            SeekMs(0);
            (void)PlayPause();
        }
    }

    void UpdateChannelVuMeters()
    {
        const uint32_t now = SDL_GetTicks();
        float dt_ms = 16.0f;
        if (m_channel_vu_last_tick_ms != 0 && now >= m_channel_vu_last_tick_ms)
        {
            dt_ms = static_cast<float>(now - m_channel_vu_last_tick_ms);
            dt_ms = std::clamp(dt_ms, 1.0f, 100.0f);
        }
        m_channel_vu_last_tick_ms = now;

        if (!m_mixer)
        {
            m_master_vu_l = 0.0f;
            m_master_vu_r = 0.0f;
            return;
        }

        /* GM_GetRealtimeChannelLevels reads the *current* mixer — bind ours. */
        (void)BAEMixer_MakeCurrent(m_mixer);

        float ch_l[16] = {};
        float ch_r[16] = {};
        if (!m_is_audio_file)
        {
            GM_GetRealtimeChannelLevels(ch_l, ch_r);
        }

        auto ema_alpha = [](float dt, float tau) -> float {
            if (tau <= 1.0f)
            {
                return 1.0f;
            }
            return std::clamp(1.0f - std::exp(-dt / tau), 0.0f, 1.0f);
        };

        for (int ch = 0; ch < 16; ++ch)
        {
            float &vu = m_channel_vu[static_cast<size_t>(ch)];
            if (ChannelEffectivelyMuted(ch))
            {
                vu = 0.0f;
                continue;
            }
            float lvl = std::clamp((ch_l[ch] + ch_r[ch]) * 0.5f, 0.0f, 1.0f);
            const float tau = (lvl > vu) ? 30.0f : 140.0f;
            const float alpha = ema_alpha(dt_ms, tau);
            vu = vu * (1.0f - alpha) + lvl * alpha;
            if (vu < 0.005f && lvl <= 0.0f)
            {
                vu = 0.0f;
            }
        }

        /* Scope + stereo peak from PCM (nbeditor-style). Full slice buffers. */
        static int16_t left[16384] = {};
        static int16_t right[16384] = {};
        int16_t frame = 0;
        if (BAEMixer_GetAudioSampleFrame(m_mixer, left, right, &frame) != BAE_NO_ERROR)
        {
            return;
        }
        const int max_idx = static_cast<int>(sizeof(left) / sizeof(left[0])) - 1;
        int idx = 0;
        if (frame > 0 && frame <= max_idx)
        {
            idx = static_cast<int>(frame) / 2;
        }
        idx = std::clamp(idx, 0, max_idx);

        const float l = static_cast<float>(left[idx]) / 32768.0f;
        const float r = static_cast<float>(right[idx]) / 32768.0f;
        m_master_vu_l = std::clamp(std::fabs(l), 0.0f, 1.0f);
        m_master_vu_r = std::clamp(std::fabs(r), 0.0f, 1.0f);

        float env_accum = 0.0f;
        constexpr int kEnvWindow = 48;
        for (int i = 0; i < kEnvWindow; ++i)
        {
            const int sidx = std::clamp(idx - i, 0, max_idx);
            const float sl = static_cast<float>(left[sidx]) / 32768.0f;
            const float sr = static_cast<float>(right[sidx]) / 32768.0f;
            env_accum += std::fabs((sl + sr) * 0.5f);
        }
        const float env = env_accum / static_cast<float>(kEnvWindow);
        const float dynamic_gain = std::clamp(4.4f - (env * 8.0f), 1.8f, 4.4f);
        const float mono = std::clamp(((l + r) * 0.5f) * dynamic_gain, -1.0f, 1.0f);

        m_scope_write_index = (m_scope_write_index + 1) % static_cast<int>(m_scope_history.size());
        m_scope_history[static_cast<size_t>(m_scope_write_index)] = mono;
    }
