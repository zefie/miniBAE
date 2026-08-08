/* Virtual MIDI keyboard — 61-key (C2–C7), channel-linked bank/prog + note light-up
 * (methods of Zefidi2App). Mirrors classic zefidi keyboard behaviour. */

    int KeyboardChannel() const
    {
        return std::clamp(m_keyboard_channel, 0, 15);
    }

    void SyncKeyboardBankProgramFromEngine()
    {
        const uint32_t now = SDL_GetTicks();
        if (m_keyboard_prog_dirty && now < m_keyboard_prog_hold_until_ms)
        {
            return;
        }
        m_keyboard_prog_dirty = false;
        BAESong song = ActiveNoteSong();
        if (!song)
        {
            return;
        }
        unsigned char program = 0;
        unsigned char bank = 0;
        if (BAESong_GetProgramBank(song, static_cast<unsigned char>(KeyboardChannel()), &program,
                                   &bank, TRUE) == BAE_NO_ERROR)
        {
            m_keyboard_program = static_cast<int>(program);
            m_keyboard_bank = static_cast<int>(bank);
        }
    }

    void ApplyKeyboardBankProgram()
    {
        BAESong song = ActiveNoteSong();
        if (!song)
        {
            return;
        }
        const unsigned char ch = static_cast<unsigned char>(KeyboardChannel());
        (void)BAESong_ProgramBankChange(song, ch,
                                        static_cast<unsigned char>(std::clamp(m_keyboard_program, 0, 127)),
                                        static_cast<unsigned char>(std::clamp(m_keyboard_bank, 0, 127)),
                                        0);
        (void)BAESong_LoadInstrument(song, static_cast<unsigned char>(std::clamp(m_keyboard_program, 0, 127)));
        m_keyboard_prog_dirty = true;
        m_keyboard_prog_hold_until_ms = SDL_GetTicks() + 400;
    }

    void SelectKeyboardChannel(int ch)
    {
        ch = std::clamp(ch, 0, 15);
        if (ch == m_keyboard_channel)
        {
            return;
        }
        /* Release any locally held notes on the old channel. */
        BAESong song = ActiveNoteSong();
        if (song)
        {
            for (int n = 0; n < 128; ++n)
            {
                if (m_keyboard_note_on[static_cast<size_t>(n)])
                {
                    (void)BAESong_NoteOff(song, static_cast<unsigned char>(m_keyboard_channel),
                                          static_cast<unsigned char>(n), 0, 0);
                    m_keyboard_note_on[static_cast<size_t>(n)] = false;
                }
            }
        }
        m_keyboard_channel = ch;
        m_keyboard_midi_lit.fill(false);
        SyncKeyboardBankProgramFromEngine();
    }

    void CollectKeyboardLitNotes(bool out_lit[128])
    {
        std::memset(out_lit, 0, 128);
        const int ch = KeyboardChannel();
        for (int n = 0; n < 128; ++n)
        {
            if (m_keyboard_note_on[static_cast<size_t>(n)] || m_keyboard_midi_lit[static_cast<size_t>(n)])
            {
                out_lit[n] = true;
            }
        }
        if (m_exporting)
        {
            return;
        }
        BAESong song = ActiveNoteSong();
        if (!song)
        {
            return;
        }
        /* Light keys from engine activity on the selected channel (playback + live). */
        unsigned char engine[128] = {};
        if (BAESong_GetActiveNotes(song, static_cast<unsigned char>(ch), engine) == BAE_NO_ERROR)
        {
            for (int n = 0; n < 128; ++n)
            {
                if (engine[n])
                {
                    out_lit[n] = true;
                }
            }
        }
    }

    void DrawKeyboardSection()
    {
        if (!m_show_keyboard || m_is_audio_file)
        {
            return;
        }
        if (!ImGui::CollapsingHeader("Keyboard", ImGuiTreeNodeFlags_DefaultOpen))
        {
            return;
        }

        SyncKeyboardBankProgramFromEngine();

        int ch_ui = KeyboardChannel() + 1;
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::SliderInt("Ch", &ch_ui, 1, 16))
        {
            SelectKeyboardChannel(ch_ui - 1);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::SliderInt("Bank", &m_keyboard_bank, 0, 127))
        {
            ApplyKeyboardBankProgram();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        if (ImGui::SliderInt("Prog", &m_keyboard_program, 0, 127))
        {
            ApplyKeyboardBankProgram();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        ImGui::SliderInt("Oct", &m_keyboard_octave, 0, 8);
        ImGui::SameLine();
        if (ImGui::SmallButton("Panic"))
        {
            BAESong song = ActiveNoteSong();
            if (song)
            {
                for (int n = 0; n < 128; ++n)
                {
                    if (m_keyboard_note_on[static_cast<size_t>(n)] ||
                        m_keyboard_midi_lit[static_cast<size_t>(n)])
                    {
                        (void)BAESong_NoteOff(song, static_cast<unsigned char>(KeyboardChannel()),
                                              static_cast<unsigned char>(n), 0, 0);
                    }
                    m_keyboard_note_on[static_cast<size_t>(n)] = false;
                    m_keyboard_midi_lit[static_cast<size_t>(n)] = false;
                }
            }
        }

        bool lit[128];
        CollectKeyboardLitNotes(lit);

        /* 61-key span C2 (36) .. C7 (96), like classic zefidi. */
        constexpr int kFirstNote = 36;
        constexpr int kLastNote = 96;
        auto is_white = [](int note) {
            const int m = note % 12;
            return m == 0 || m == 2 || m == 4 || m == 5 || m == 7 || m == 9 || m == 11;
        };

        int white_count = 0;
        for (int n = kFirstNote; n <= kLastNote; ++n)
        {
            if (is_white(n))
            {
                ++white_count;
            }
        }
        if (white_count < 1)
        {
            white_count = 1;
        }

        const float avail_w = ImGui::GetContentRegionAvail().x;
        const float white_h = 64.0f;
        const float black_h = 40.0f;
        const float white_w = std::max(10.0f, avail_w / static_cast<float>(white_count));
        const float black_w = white_w * 0.62f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImGui::Dummy(ImVec2(white_w * static_cast<float>(white_count), white_h));

        const ImU32 accent = Zefidi2Theme::ToU32(Zefidi2Theme::Accent());
        const ImU32 local_hi = IM_COL32(255, 210, 90, 255);

        auto note_button = [&](int note, ImVec2 min, ImVec2 max, bool black) {
            if (note < kFirstNote || note > kLastNote || !dl)
            {
                return;
            }
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(note + (black ? 1000 : 0));
            char id_buf[32];
            std::snprintf(id_buf, sizeof(id_buf), "##vkbd_%d", note);
            ImGui::InvisibleButton(id_buf, ImVec2(max.x - min.x, max.y - min.y));
            const bool held = ImGui::IsItemActive();
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            bool &on = m_keyboard_note_on[static_cast<size_t>(note)];
            BAESong song = ActiveNoteSong();
            if (held && !on && song)
            {
                (void)BAESong_NoteOn(song, static_cast<unsigned char>(KeyboardChannel()),
                                     static_cast<unsigned char>(note), 100, 0);
                on = true;
            }
            else if (!held && on && song)
            {
                (void)BAESong_NoteOff(song, static_cast<unsigned char>(KeyboardChannel()),
                                      static_cast<unsigned char>(note), 0, 0);
                on = false;
            }

            ImU32 col;
            if (on)
            {
                col = local_hi;
            }
            else if (lit[note])
            {
                col = accent;
            }
            else if (hovered)
            {
                col = black ? IM_COL32(60, 70, 72, 255) : IM_COL32(230, 235, 238, 255);
            }
            else
            {
                col = black ? IM_COL32(20, 24, 26, 255) : IM_COL32(210, 216, 220, 255);
            }
            dl->AddRectFilled(min, max, col, black ? 1.0f : 2.0f);
            dl->AddRect(min, max, IM_COL32(40, 50, 52, 255), black ? 1.0f : 2.0f);
            if (!black && (note % 12) == 0)
            {
                char nb[8];
                std::snprintf(nb, sizeof(nb), "C%d", (note / 12) - 1);
                dl->AddText(ImVec2(min.x + 2.0f, max.y - 14.0f), IM_COL32(40, 40, 44, 200), nb);
            }
        };

        /* White keys */
        int w_index = 0;
        for (int n = kFirstNote; n <= kLastNote; ++n)
        {
            if (!is_white(n))
            {
                continue;
            }
            const float x0 = origin.x + white_w * static_cast<float>(w_index);
            const float x1 = origin.x + white_w * static_cast<float>(w_index + 1);
            note_button(n, ImVec2(x0, origin.y), ImVec2(x1 - 1.0f, origin.y + white_h), false);
            ++w_index;
        }
        /* Black keys — position relative to preceding white key index. */
        w_index = 0;
        for (int n = kFirstNote; n <= kLastNote; ++n)
        {
            if (is_white(n))
            {
                ++w_index;
                continue;
            }
            /* Center black key over the boundary between white w_index-1 and w_index. */
            const float boundary = origin.x + white_w * static_cast<float>(w_index);
            const float x0 = boundary - black_w * 0.5f;
            note_button(n, ImVec2(x0, origin.y), ImVec2(x0 + black_w, origin.y + black_h), true);
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + white_h + 4.0f));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        ImGui::TextDisabled("Ch %d  ·  lights follow selected channel (playback + MIDI in)",
                            KeyboardChannel() + 1);
    }

    void HandlePcKeyboardEvent(const SDL_Event &event)
    {
        if (!m_show_keyboard || m_is_audio_file || ImGui::GetIO().WantTextInput)
        {
            return;
        }
        /* Covers one octave + C above; transposed by Oct slider. */
        auto map_key = [](SDL_Keycode k) -> int {
            switch (k)
            {
            case SDLK_A:
                return 0;
            case SDLK_W:
                return 1;
            case SDLK_S:
                return 2;
            case SDLK_E:
                return 3;
            case SDLK_D:
                return 4;
            case SDLK_F:
                return 5;
            case SDLK_T:
                return 6;
            case SDLK_G:
                return 7;
            case SDLK_Y:
                return 8;
            case SDLK_H:
                return 9;
            case SDLK_U:
                return 10;
            case SDLK_J:
                return 11;
            case SDLK_K:
                return 12;
            default:
                return -1;
            }
        };
        const int off = map_key(event.key.key);
        if (off < 0)
        {
            return;
        }
        const int note = std::clamp(m_keyboard_octave * 12 + off, 0, 127);
        BAESong song = ActiveNoteSong();
        if (!song)
        {
            return;
        }
        bool &on = m_keyboard_note_on[static_cast<size_t>(note)];
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !on)
        {
            (void)BAESong_NoteOn(song, static_cast<unsigned char>(KeyboardChannel()),
                                 static_cast<unsigned char>(note), 100, 0);
            on = true;
        }
        else if (event.type == SDL_EVENT_KEY_UP && on)
        {
            (void)BAESong_NoteOff(song, static_cast<unsigned char>(KeyboardChannel()),
                                  static_cast<unsigned char>(note), 0, 0);
            on = false;
        }
    }
