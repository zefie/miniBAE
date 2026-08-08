/* Startup tips — included inside NbEditorApp.
 *
 * To add a tip: append one entry to kEditorTips[] below (title + body).
 * Startup shows tips in array order via persisted tips_next. Prefer appending
 * new tips rather than reordering (tips_seen indices stay stable).
 */

    struct EditorTip
    {
        const char *title;
        const char *body;
    };

    /* ---- Tip table (append new tips here) -------------------------------- */
    static constexpr EditorTip kEditorTips[] = {
        {
            "Welcome to NeoBAE Editor",
            "NeoBAE Editor builds Beatnik-style banks (HSB/ZSB) and songs (RMF/ZMF) around a "
            "Session: Songs, Instruments, and Samples live together. Save with "
            "File → Save Session so you can reopen the whole project later.",
        },
        {
            "Backwards compatible",
            "NeoBAE Editor is backwards compatible with Beatnik Editor 2. "
            "You can open your old Beatnik Editor 2 sessions into NeoBAE "
            "Editor. If your project does not use ZMF features, you can also "
            "save your session as a .bsn file, which can then be opened in Beatnik "
            "Editor 2.",
        },
        {
            "Sessions vs banks vs songs",
            "A Session (.bsn / .zsn) is your working project. The bank holds "
            "instruments and samples; songs reference them by program. Export "
            "RMF/ZMF when you want a distributable song+bank package — keep "
            "saving the Session while you edit.",
        },
        {
            "Open / Import almost anything",
            "File → Open / Import… accepts sessions, RMF/ZMF, MIDI, WAV/AIFF, "
            "and tracker modules (MOD/S3M/XM/IT/…). Drag-and-drop onto the "
            "window works the same way. Modules convert through mod2rmf using "
            "your Module Import Settings.",
        },
        {
            "Built-in bank for playback",
            "File → Load Built-in Bank loads the standard Beatnik GM-like set "
            "into Bank 0/1 so General MIDI songs play immediately. Custom "
            "instruments usually go in Bank 2+ so they stay separate from the "
            "built-in set.",
        },
        {
            "Bank 2 is your sandbox",
            "User/custom instruments typically live in Bank 2. Bank 0/1 are "
            "for the built-in (or imported GM) set. Unload Bank removes Bank "
            "0/1 instruments while leaving Bank 2+ alone — handy when cleaning "
            "up after testing.",
        },
        {
            "Instrument Editor (IE)",
            "Double-click an instrument in the Session list (or use the "
            "context menu) to open the Instrument Editor. There you wire "
            "samples, key splits, ADSR, root key, and flags like Use Sample "
            "Rate / interpolate.",
        },
        {
            "Sample Editor",
            "Open a sample from the Samples tab to trim, loop, gain, and "
            "compress. Loop points matter for pads and sustains — a bad loop "
            "clicks even if the instrument ADSR looks fine.",
        },
        {
            "ADSR envelopes",
            "Amplitude envelopes live on the instrument (ADSR unit). Tracker "
            "imports can pull multi-stage envelopes when Module Import → "
            "extended ADSR is enabled (ZMF). Sustain/LAST stage flags control "
            "how notes hold and release in the engine.",
        },
        {
            "MIDI Editor",
            "Song → MIDI Editor… opens a piano-roll style editor for the "
            "current song. Use it to fix programs, volumes (CC7), timing, and "
            "loops after a module or MIDI import.",
        },
        {
            "Karaoke lyrics",
            "Song → Lyrics… (or MIDI Editor → Lyrics…) paste verse lines, "
            "enable Tap-to-sync, and stamp words with Space while the song "
            "plays. Then drag chips on the Lyrics lane or Quantize to the "
            "snap grid. Lyrics save as MIDI meta 0x05.",
        },
        {
            "Export RMF vs ZMF",
            "Classic RMF has tighter limits; ZMF unlocks NeoBAE extensions "
            "(deeper pitch, longer ADSR, etc.). The export dialog will suggest "
            "ZMF when your content needs it. Embed only the instruments the "
            "song uses to keep files small.",
        },
        {
            "Export as Audio",
            "File → Export as Audio… (or Song →) renders the current song "
            "through the engine to WAV (and other formats when built-in). "
            "Useful for proofing mixes without a separate player.",
        },
        {
            "Trash is soft-delete",
            "Edit → Trash… holds soft-deleted samples/instruments. Restore "
            "mistakes from there, or Empty Trash when you are sure. Older "
            "sessions may still carry legacy TRSH blocks — the Trash window "
            "can recover those too.",
        },
        {
            "Multiple Instances",
            "NeoBAE Editor can run multiple instances, allowing you to edit "
            "multiple sessions at once. You can even copy / paste resources "
            "between sessions."
        },
        {
            "Copy / paste across sessions",
            "Cut/Copy/Paste works on the Songs, Instruments, and Samples "
            "tabs (Ctrl+X/C/V). Instrument clips can include samples — watch "
            "for the prompt so you do not accidentally leave orphans behind.",
        },
        {
            "Module import settings",
            "Edit → Preferences → Module Import Settings… controls tracker "
            "conversion: extended pitch/ADSR, resample filter/rate, Amiga LPF "
            "simulation, and stereo separation. Changes apply to the next "
            "module you open.",
        },
        {
            "Session layout",
            "Dock the Player, Session, and Status panels to taste. Preferences "
            "can save that layout into the Session. Edit → Reset Layout "
            "restores the default arrangement if things get messy.",
        },
        {
            "Song Info & metadata",
            "Song → Song Info… sets title, composer, copyright, and notes "
            "embedded on export. Session Info covers the project file itself. "
            "Fill these in before shipping an RMF/ZMF.",
        },
        {
            "Resource usage",
            "Right-click a song → Get Resource Usage… to see which instruments "
            "and samples it references. Great for trimming unused bank data "
            "before Export Bank or Export RMF.",
        },
#if SUPPORT_MIDI_HW == TRUE       
        {
            "Hardware MIDI",
            "NeoBAE Editor features MIDI hardware support, Edit → Preferences → "
            "General… can enable a MIDI input device for live notes while "
            "editing instruments.",
        },
#endif
        {
            "MIDI Editor",
            "The MIDI Editor allows real-time editing of the selected song. "
            "You can use CTRL+Mouse Wheel to zoom in and out of the timeline. "
            "The MIDI Editor allows you to change notes, as well as add/remove: "
            "Notes, Tracks, (N)RPN, Automation, Loop Point, Lyrics, and more.",
        },
        {
            "Status bar & long jobs",
            "The bottom status line reports what the editor is doing. Long "
            "imports, saves, and exports show progress — wait for them to "
            "finish before quitting so you do not interrupt a write. If the "
            "program appears to be frozen or locked up, please be patient. "
            "The operation will finish even if the program appears to be frozen.",
        },
        {
            "Reopen these tips anytime",
            "Help → Tips… brings this dialog back. Turn startup tips off with "
            "“Don't show this again”, or re-enable them under Edit → "
            "Preferences → General… (“Show tips at startup”).",
        },
    };

    static constexpr int kEditorTipCount =
        static_cast<int>(sizeof(kEditorTips) / sizeof(kEditorTips[0]));

    static bool TipsIndexValid(int index)
    {
        return index >= 0 && index < kEditorTipCount;
    }

    bool TipsIsSeen(int index) const
    {
        if (!TipsIndexValid(index))
        {
            return false;
        }
        if (index >= static_cast<int>(m_tips_seen.size()))
        {
            return false;
        }
        return m_tips_seen[static_cast<size_t>(index)] != 0;
    }

    void TipsMarkSeen(int index)
    {
        if (!TipsIndexValid(index))
        {
            return;
        }
        if (static_cast<int>(m_tips_seen.size()) < kEditorTipCount)
        {
            m_tips_seen.resize(static_cast<size_t>(kEditorTipCount), 0);
        }
        m_tips_seen[static_cast<size_t>(index)] = 1;
    }

    void TipsClearSeen()
    {
        m_tips_seen.assign(static_cast<size_t>(kEditorTipCount), 0);
    }

    int TipsCountSeen() const
    {
        int n = 0;
        const int lim = std::min(kEditorTipCount, static_cast<int>(m_tips_seen.size()));
        for (int i = 0; i < lim; ++i)
        {
            if (m_tips_seen[static_cast<size_t>(i)])
            {
                ++n;
            }
        }
        return n;
    }

    /* Startup sequence uses persisted m_tips_next (0..count-1), then advances. */
    int TipsPickStartupIndex()
    {
        if (kEditorTipCount <= 0)
        {
            return 0;
        }
        if (m_tips_next < 0)
        {
            m_tips_next = 0;
        }
        return m_tips_next % kEditorTipCount;
    }

    void TipsAdvanceStartupIndex()
    {
        if (kEditorTipCount <= 0)
        {
            return;
        }
        m_tips_next = (TipsPickStartupIndex() + 1) % kEditorTipCount;
    }

    void TipsSetCurrent(int index, bool mark_seen_and_save)
    {
        if (!TipsIndexValid(index))
        {
            index = 0;
        }
        m_tips_index = index;
        if (mark_seen_and_save)
        {
            TipsMarkSeen(index);
            SaveEditorPrefsToDisk();
        }
    }

    void OpenTipsDialog(bool from_startup)
    {
        m_tips_dont_show_again = !m_pref_show_tips_at_startup;
        if (from_startup)
        {
            const int index = TipsPickStartupIndex();
            TipsMarkSeen(index);
            TipsAdvanceStartupIndex();
            m_tips_index = index;
            SaveEditorPrefsToDisk();
        }
        else
        {
            /* Help → Tips: open on the tip most recently shown, mark it seen. */
            if (!TipsIndexValid(m_tips_index))
            {
                m_tips_index = TipsPickStartupIndex();
            }
            TipsSetCurrent(m_tips_index, true);
        }
        m_tips_open = true;
    }

    void ScheduleStartupTipsIfEnabled()
    {
        if (!m_pref_show_tips_at_startup || kEditorTipCount <= 0)
        {
            m_tips_startup_pending = false;
            return;
        }
        m_tips_startup_pending = true;
        m_tips_startup_delay_frames = 3;
    }

    void PumpStartupTips()
    {
        if (!m_tips_startup_pending)
        {
            return;
        }
        if (m_tips_startup_delay_frames > 0)
        {
            --m_tips_startup_delay_frames;
            return;
        }
        m_tips_startup_pending = false;
        if (m_pref_show_tips_at_startup)
        {
            OpenTipsDialog(true);
        }
    }

    static constexpr const char *kTipsPopupId = "Tip##nbeditor_startup_tips";

    void TipsApplyDontShowPref()
    {
        if (m_tips_dont_show_again)
        {
            m_pref_show_tips_at_startup = false;
        }
        SaveEditorPrefsToDisk();
    }

    /* Clear our flag; DrawTipsDialog drains any leftover ImGui modal so it
     * cannot leave a dimmer that blocks Quit / the menu bar. */
    void DismissTipsDialog()
    {
        m_tips_startup_pending = false;
        m_tips_open = false;
    }

    void CloseTipsDialog()
    {
        TipsApplyDontShowPref();
        m_tips_open = false;
        ImGui::CloseCurrentPopup();
    }

    void DrawTipsDialog()
    {
        PumpStartupTips();

        if (kEditorTipCount <= 0)
        {
            DismissTipsDialog();
            return;
        }

        if (m_tips_open)
        {
            ImGui::OpenPopup(kTipsPopupId);
        }

        /* Must still BeginPopupModal when ImGui has the popup open even if our
         * flag was cleared — otherwise a stuck modal dimmer blocks the UI. */
        if (!m_tips_open && !ImGui::IsPopupOpen(kTipsPopupId))
        {
            return;
        }

        if (!TipsIndexValid(m_tips_index))
        {
            m_tips_index = 0;
        }

        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        if (viewport)
        {
            ImGui::SetNextWindowPos(viewport->GetCenter(),
                                    ImGuiCond_Appearing,
                                    ImVec2(0.5f, 0.5f));
        }
        ImGui::SetNextWindowSize(ImVec2(520.0f, 340.0f), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(kTipsPopupId, &m_tips_open, ImGuiWindowFlags_AlwaysAutoResize))
        {
            /* Closed via title-bar X / Escape — Begin cleared m_tips_open. */
            if (!m_tips_open)
            {
                TipsApplyDontShowPref();
            }
            return;
        }

        /* Flag cleared externally (e.g. quit) while ImGui still had us open. */
        if (!m_tips_open)
        {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        const EditorTip &tip = kEditorTips[m_tips_index];

        ImGui::Text("Tip %d of %d", m_tips_index + 1, kEditorTipCount);
        ImGui::SameLine();
        ImGui::TextDisabled("(%d seen)", TipsCountSeen());

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        if (ImGui::BeginCombo("##tips_selector", tip.title ? tip.title : "(untitled)"))
        {
            for (int i = 0; i < kEditorTipCount; ++i)
            {
                const bool selected = (i == m_tips_index);
                const char *label = kEditorTips[i].title ? kEditorTips[i].title : "(untitled)";
                char row[160];
                std::snprintf(row, sizeof(row), "%d. %s", i + 1, label);
                if (ImGui::Selectable(row, selected))
                {
                    TipsSetCurrent(i, true);
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();
        ImGui::TextUnformatted(tip.title ? tip.title : "");
        ImGui::Spacing();
        ImGui::BeginChild("##tips_body", ImVec2(480.0f, 160.0f), ImGuiChildFlags_Borders);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(tip.body ? tip.body : "");
        ImGui::PopTextWrapPos();
        ImGui::EndChild();

        if (ImGui::ArrowButton("##tips_prev", ImGuiDir_Left))
        {
            const int next = (m_tips_index + kEditorTipCount - 1) % kEditorTipCount;
            TipsSetCurrent(next, true);
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##tips_next", ImGuiDir_Right))
        {
            const int next = (m_tips_index + 1) % kEditorTipCount;
            TipsSetCurrent(next, true);
        }

        ImGui::Checkbox("Don't show this again", &m_tips_dont_show_again);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip(
                "Disables tips on startup.\n"
                "Re-enable under Edit → Preferences → General…");
        }

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120.0f, 0.0f)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            CloseTipsDialog();
        }

        ImGui::EndPopup();
    }
