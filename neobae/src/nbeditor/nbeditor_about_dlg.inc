/* About NeoBAE Editor — included inside NbEditorApp. */

    void OpenAboutDialog()
    {
        m_about_open = true;
    }

    void AboutDrawCreditLine(const char *text)
    {
        if (!text)
        {
            return;
        }
        if (text[0] == '\0')
        {
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            return;
        }
        if (std::strncmp(text, "http", 4) == 0)
        {
            ImGui::Indent(8.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, NeoBaeTheme::LinkText());
            if (ImGui::Selectable(text, false))
            {
                (void)SDL_OpenURL(text);
            }
            ImGui::PopStyleColor();
            ImGui::Unindent(8.0f);
            return;
        }
        ImGui::Indent(8.0f);
        ImGui::TextUnformatted(text);
        ImGui::Unindent(8.0f);
    }

    void DrawAboutDialog()
    {
        if (!m_about_open)
        {
            return;
        }
        ImGui::OpenPopup("About NeoBAE Editor");
        ImGui::SetNextWindowSize(ImVec2(560.0f, 520.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::BeginPopupModal("About NeoBAE Editor", &m_about_open, 0))
        {
            return;
        }

#ifndef _VERSION
#define _VERSION "unknown"
#endif
        char *bae_version = const_cast<char *>(BAE_GetVersion());
        char *compile_info = const_cast<char *>(BAE_GetCompileInfo());
        const char *features = BAE_GetFeatureString();
        const char *cpu = BAE_GetCurrentCPUArchitecture();

        ImGui::Text("NeoBAE Editor version: %s", _VERSION);
        if (cpu && cpu[0])
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", cpu);
        }
        ImGui::TextWrapped("libNeoBAE Version: %s",
                           (bae_version && bae_version[0]) ? bae_version : "(unknown)");
        ImGui::TextWrapped("Build info: %s",
                           (compile_info && compile_info[0]) ? compile_info : "(unknown)");
        ImGui::TextWrapped("Feature string: %s",
                           (features && features[0]) ? features : "(none)");

        ImGui::Separator();
        ImGui::TextUnformatted("(C) 2021-2026 Zefie Networks");
        AboutDrawCreditLine("https://www.soundmusicsys.com/");
        AboutDrawCreditLine("https://github.com/zefie/NeoBAE/");

        ImGui::Separator();
        ImGui::TextUnformatted("This software makes use of the following software:");
        ImGui::BeginChild("##AboutLicenses", ImVec2(0.0f, -36.0f), ImGuiChildFlags_Borders);

        AboutDrawCreditLine("");
        AboutDrawCreditLine("NeoBAE");
        AboutDrawCreditLine("Copyright (c) 2026 Zefie Networks");
        AboutDrawCreditLine("Based on miniBAE, Copyright (c) 2009 Beatnik, Inc.");
        AboutDrawCreditLine("Original miniBAE source code available at:");
        AboutDrawCreditLine("https://github.com/heyigor/miniBAE/");

        AboutDrawCreditLine("");
        AboutDrawCreditLine("SDL3");
        AboutDrawCreditLine("Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>");
        AboutDrawCreditLine("https://www.libsdl.org/");

        AboutDrawCreditLine("");
        AboutDrawCreditLine("Dear ImGui");
        AboutDrawCreditLine("Copyright (c) 2014-2026 Omar Cornut");
        AboutDrawCreditLine("https://github.com/ocornut/imgui");

        AboutDrawCreditLine("");
        AboutDrawCreditLine("Liberation Sans fonts");
        AboutDrawCreditLine("Copyright (c) 2012 Red Hat, Inc.");
        AboutDrawCreditLine("https://github.com/liberationfonts/liberation-fonts");

        AboutDrawCreditLine("");
        AboutDrawCreditLine("libxmp-lite");
        AboutDrawCreditLine("Copyright (C) 1996-2025 Claudio Matsuoka and Hipolito Carraro Jr");
        AboutDrawCreditLine("https://github.com/libxmp/libxmp");

#if USE_MPEG_DECODER == TRUE
        AboutDrawCreditLine("");
        AboutDrawCreditLine("minimp3");
        AboutDrawCreditLine("Licensed under the CC0");
        AboutDrawCreditLine("http://creativecommons.org/publicdomain/zero/1.0/");
#endif
#if USE_MPEG_ENCODER == TRUE
        AboutDrawCreditLine("");
        AboutDrawCreditLine("libmp3lame");
        AboutDrawCreditLine("https://lame.sourceforge.io/");
#endif
#if USE_XMF_SUPPORT == TRUE
        AboutDrawCreditLine("");
        AboutDrawCreditLine("zlib");
        AboutDrawCreditLine("Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler");
        AboutDrawCreditLine("http://www.zlib.net/");
#endif
#if defined(SUPPORT_MIDI_HW) && (SUPPORT_MIDI_HW == TRUE)
        AboutDrawCreditLine("");
        AboutDrawCreditLine("RtMidi: realtime MIDI i/o C++ classes");
        AboutDrawCreditLine("Copyright (c) 2003-2023 Gary P. Scavone");
        AboutDrawCreditLine("https://github.com/thestk/rtmidi");
#endif
#if defined(SUPPORT_OGG_FORMAT) && (SUPPORT_OGG_FORMAT == TRUE)
        AboutDrawCreditLine("");
        AboutDrawCreditLine("libogg");
        AboutDrawCreditLine("Copyright (c) 2002, Xiph.org Foundation");
        AboutDrawCreditLine("https://www.xiph.org/ogg/");
#endif
#if (USE_VORBIS_DECODER == TRUE) || (USE_VORBIS_ENCODER == TRUE)
        AboutDrawCreditLine("");
        AboutDrawCreditLine("libvorbis");
        AboutDrawCreditLine("Copyright (c) 2002-2020 Xiph.org Foundation");
        AboutDrawCreditLine("https://www.xiph.org/vorbis/");
#endif
#if (USE_OPUS_DECODER == TRUE) || (USE_OPUS_ENCODER == TRUE)
        AboutDrawCreditLine("");
        AboutDrawCreditLine("libopus");
        AboutDrawCreditLine("Copyright (c) 2007-2026 Xiph.org Foundation");
        AboutDrawCreditLine("https://www.xiph.org/opus/");
#endif
#if (USE_FLAC_DECODER == TRUE) || (USE_FLAC_ENCODER == TRUE)
        AboutDrawCreditLine("");
        AboutDrawCreditLine("libFLAC");
        AboutDrawCreditLine("Copyright (C) 2000-2009 Josh Coalson, (C) 2011-2025 Xiph.Org Foundation");
        AboutDrawCreditLine("https://www.xiph.org/flac/");
#endif
#if defined(_USING_FLUIDLITE) && (_USING_FLUIDLITE == TRUE)
        AboutDrawCreditLine("");
        AboutDrawCreditLine("FluidLite (modified for NeoBAE)");
        AboutDrawCreditLine("Copyright (C) 2016 Robin Lobel & FluidSynth Team");
#endif
#if defined(USE_QOA_SUPPORT) && (USE_QOA_SUPPORT == TRUE)
        AboutDrawCreditLine("");
        AboutDrawCreditLine("Quite OK Audio Codec (QOA)");
        AboutDrawCreditLine("Copyright (C) 2023 Dominic Szablewski");
        AboutDrawCreditLine("https://github.com/phoboslab/qoa");
#endif
#if defined(USE_ADP_SUPPORT) && (USE_ADP_SUPPORT == TRUE)
        AboutDrawCreditLine("");
        AboutDrawCreditLine("libg722");
        AboutDrawCreditLine("Copyright (c) 2014-2025 Sippy Software, Inc.");
        AboutDrawCreditLine("http://www.sippysoft.com");
#endif
#if defined(USE_LZMA_COMPRESSION) && (USE_LZMA_COMPRESSION == TRUE)
        AboutDrawCreditLine("");
        AboutDrawCreditLine("LZMA SDK");
        AboutDrawCreditLine("Copyright (C) 1999-2026 Igor Pavlov");
        AboutDrawCreditLine("https://www.7-zip.org/sdk.html");
#endif

        ImGui::EndChild();

        if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
        {
            m_about_open = false;
            ImGui::CloseCurrentPopup();
        }

        if (bae_version && bae_version[0])
        {
            free(bae_version);
        }
        if (compile_info && compile_info[0])
        {
            free(compile_info);
        }

        ImGui::EndPopup();
    }
