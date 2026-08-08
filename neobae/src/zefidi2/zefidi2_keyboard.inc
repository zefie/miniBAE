/* Compact virtual MIDI keyboard (methods of Zefidi2App). */

    void DrawKeyboardSection()
    {
        if (!m_show_keyboard)
        {
            return;
        }
        if (!ImGui::CollapsingHeader("Keyboard"))
        {
            return;
        }

        ImGui::SetNextItemWidth(70.0f);
        ImGui::SliderInt("Oct", &m_keyboard_octave, 0, 8);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::SliderInt("Ch", &m_keyboard_channel, 1, 16);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::SliderInt("Prog", &m_keyboard_program, 0, 127);
        ImGui::SameLine();
        if (ImGui::SmallButton("Panic"))
        {
            BAESong song = ActiveNoteSong();
            if (song)
            {
                for (int n = 0; n < 128; ++n)
                {
                    if (m_keyboard_note_on[static_cast<size_t>(n)])
                    {
                        (void)BAESong_NoteOff(song,
                                              static_cast<unsigned char>(m_keyboard_channel - 1),
                                              static_cast<unsigned char>(n), 0, 0);
                        m_keyboard_note_on[static_cast<size_t>(n)] = false;
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply Prog"))
        {
            BAESong song = ActiveNoteSong();
            if (song)
            {
                (void)BAESong_ProgramBankChange(song,
                                                static_cast<unsigned char>(m_keyboard_channel - 1),
                                                static_cast<unsigned char>(m_keyboard_program),
                                                0, 0);
            }
        }

        /* One octave white/black keys starting at C of selected octave. */
        const int base = std::clamp(m_keyboard_octave, 0, 8) * 12;
        static const int kWhiteOffsets[] = {0, 2, 4, 5, 7, 9, 11};
        static const int kBlackOffsets[] = {1, 3, -1, 6, 8, 10}; /* -1 = gap */
        const float white_w = 22.0f;
        const float white_h = 56.0f;
        const float black_w = 14.0f;
        const float black_h = 34.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        auto note_button = [&](int note, ImVec2 min, ImVec2 max, bool black) {
            if (note < 0 || note > 127)
            {
                return;
            }
            const ImGuiID id = ImGui::GetID(static_cast<int>(1000 + note));
            ImGui::SetCursorScreenPos(min);
            ImGui::InvisibleButton("##k", ImVec2(max.x - min.x, max.y - min.y));
            const bool held = ImGui::IsItemActive();
            const bool hovered = ImGui::IsItemHovered();
            bool &on = m_keyboard_note_on[static_cast<size_t>(note)];
            BAESong song = ActiveNoteSong();
            if (held && !on && song)
            {
                (void)BAESong_NoteOn(song, static_cast<unsigned char>(m_keyboard_channel - 1),
                                     static_cast<unsigned char>(note), 100, 0);
                on = true;
            }
            else if (!held && on && song)
            {
                (void)BAESong_NoteOff(song, static_cast<unsigned char>(m_keyboard_channel - 1),
                                      static_cast<unsigned char>(note), 0, 0);
                on = false;
            }
            ImU32 col;
            if (on)
            {
                col = IM_COL32(40, 200, 180, 255);
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
            (void)id;
        };

        for (int i = 0; i < 7; ++i)
        {
            const int note = base + kWhiteOffsets[i];
            const ImVec2 min(origin.x + white_w * static_cast<float>(i), origin.y);
            const ImVec2 max(min.x + white_w - 1.0f, min.y + white_h);
            note_button(note, min, max, false);
        }
        for (int i = 0; i < 6; ++i)
        {
            if (kBlackOffsets[i] < 0)
            {
                continue;
            }
            const int note = base + kBlackOffsets[i];
            const float x = origin.x + white_w * static_cast<float>(i) + white_w - black_w * 0.5f;
            note_button(note, ImVec2(x, origin.y), ImVec2(x + black_w, origin.y + black_h), true);
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + white_h + 4.0f));
        ImGui::Dummy(ImVec2(white_w * 7.0f, 1.0f));
    }

    void HandlePcKeyboardEvent(const SDL_Event &event)
    {
        if (!m_show_keyboard || ImGui::GetIO().WantTextInput)
        {
            return;
        }
        /* Simple QWERTY row → notes. */
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
            (void)BAESong_NoteOn(song, static_cast<unsigned char>(m_keyboard_channel - 1),
                                 static_cast<unsigned char>(note), 100, 0);
            on = true;
        }
        else if (event.type == SDL_EVENT_KEY_UP && on)
        {
            (void)BAESong_NoteOff(song, static_cast<unsigned char>(m_keyboard_channel - 1),
                                  static_cast<unsigned char>(note), 0, 0);
            on = false;
        }
    }
