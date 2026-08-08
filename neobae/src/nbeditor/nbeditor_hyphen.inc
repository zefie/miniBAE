/* Syllable hyphenation helpers for karaoke lyrics.
 * Included inside NbEditorApp when USE_LIB_HYPHEN == TRUE. */

#if USE_LIB_HYPHEN == TRUE

    struct LyHyphLang
    {
        std::string id;   /* e.g. en_US */
        std::string name; /* display */
        std::string path; /* .dic path */
    };

    struct LyHyphCatalogEntry
    {
        std::string id;
        std::string name;
        std::string file;
        std::string url;
    };

    enum LySplitMode : int
    {
        LySplit_Words = 0,
        LySplit_Syllables = 1,
    };

    static size_t LyUtf8CharLen(unsigned char c)
    {
        if (c < 0x80)
        {
            return 1;
        }
        if ((c & 0xE0) == 0xC0)
        {
            return 2;
        }
        if ((c & 0xF0) == 0xE0)
        {
            return 3;
        }
        if ((c & 0xF8) == 0xF0)
        {
            return 4;
        }
        return 1;
    }

    static bool LyWordHasCyrillic(const std::string &word)
    {
        for (size_t i = 0; i < word.size();)
        {
            const unsigned char c = static_cast<unsigned char>(word[i]);
            const size_t n = LyUtf8CharLen(c);
            if (n == 2 && i + 1 < word.size())
            {
                const unsigned char c1 = static_cast<unsigned char>(word[i + 1]);
                /* U+0400–U+04FF → D0 80–D3 BF roughly */
                if (c == 0xD0 || c == 0xD1 || (c == 0xD2 && c1 <= 0xBF) ||
                    (c == 0xD3 && c1 <= 0xBF))
                {
                    return true;
                }
            }
            i += n;
        }
        return false;
    }

    static std::string LyHyphConfigDir()
    {
        char root[1024];
        if (BAE_GetConfigDirectory(root, sizeof(root)) != 0)
        {
            return std::string();
        }
        return std::string(root) + "/hyphen";
    }

    static std::string LyHyphBundleDir()
    {
        char path[1100];
        if (BAE_GetRuntimeFilePath(path, sizeof(path), "nbeditor") != 0 || !path[0])
        {
            return std::string("nbeditor");
        }
        return std::string(path);
    }

    static void LyHyphEnsureDir(const std::string &dir)
    {
        if (dir.empty())
        {
            return;
        }
        (void)BAE_EnsureConfigDirectory();
#if defined(_WIN32)
        CreateDirectoryA(dir.c_str(), nullptr);
#else
        ::mkdir(dir.c_str(), 0755);
#endif
    }

    static bool LyHyphFileExists(const std::string &path)
    {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
        {
            return false;
        }
        std::fclose(f);
        return true;
    }

    static void LyHyphScanDir(const std::string &dir,
                              std::map<std::string, LyHyphLang> *by_id)
    {
        if (!by_id || dir.empty())
        {
            return;
        }
#if defined(_WIN32)
        WIN32_FIND_DATAA fd;
        const std::string pattern = dir + "/hyph_*.dic";
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            const char *name = fd.cFileName;
            if (std::strncmp(name, "hyph_", 5) != 0)
            {
                continue;
            }
            const char *dot = std::strrchr(name, '.');
            if (!dot || std::strcmp(dot, ".dic") != 0)
            {
                continue;
            }
            std::string id(name + 5, dot);
            if (id.empty() || by_id->count(id))
            {
                continue;
            }
            LyHyphLang lang;
            lang.id = id;
            lang.name = id;
            lang.path = dir + "/" + name;
            (*by_id)[id] = lang;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
#else
        DIR *d = opendir(dir.c_str());
        if (!d)
        {
            return;
        }
        while (dirent *ent = readdir(d))
        {
            const char *name = ent->d_name;
            if (std::strncmp(name, "hyph_", 5) != 0)
            {
                continue;
            }
            const char *dot = std::strrchr(name, '.');
            if (!dot || std::strcmp(dot, ".dic") != 0)
            {
                continue;
            }
            std::string id(name + 5, dot);
            if (id.empty() || by_id->count(id))
            {
                continue;
            }
            LyHyphLang lang;
            lang.id = id;
            lang.name = id;
            lang.path = dir + "/" + name;
            (*by_id)[id] = lang;
        }
        closedir(d);
#endif
    }

    void LyHyphRescanLanguages()
    {
        for (auto &kv : m_hyph_dicts)
        {
            if (kv.second)
            {
                hnj_hyphen_free(kv.second);
            }
        }
        m_hyph_dicts.clear();
        m_hyph_langs.clear();

        std::map<std::string, LyHyphLang> by_id;
        LyHyphScanDir(LyHyphConfigDir(), &by_id);
        LyHyphScanDir(LyHyphBundleDir(), &by_id);
#if !defined(_WIN32)
        LyHyphScanDir("/usr/share/neobae/nbeditor", &by_id);
        LyHyphScanDir("/usr/local/share/neobae/nbeditor", &by_id);
        LyHyphScanDir("/usr/share/hyphen", &by_id);
        LyHyphScanDir("/usr/local/share/hyphen", &by_id);
        LyHyphScanDir("/usr/share/myspell/dicts", &by_id);
#endif
        /* Friendlier names for known locales. */
        static const char *kNames[][2] = {
            {"en_US", "English (US)"},
            {"en_GB", "English (UK)"},
            {"ru_RU", "Russian"},
            {"de_DE", "German"},
            {"fr_FR", "French"},
            {"es_ES", "Spanish"},
            {nullptr, nullptr},
        };
        for (auto &kv : by_id)
        {
            for (int i = 0; kNames[i][0]; ++i)
            {
                if (kv.first == kNames[i][0])
                {
                    kv.second.name = kNames[i][1];
                    break;
                }
            }
            m_hyph_langs.push_back(kv.second);
        }
        std::sort(m_hyph_langs.begin(),
                  m_hyph_langs.end(),
                  [](const LyHyphLang &a, const LyHyphLang &b) {
                      return a.name < b.name;
                  });
        m_hyph_langs_scanned = true;
    }

    HyphenDict *LyHyphGetDict(const std::string &id)
    {
        if (id.empty())
        {
            return nullptr;
        }
        auto it = m_hyph_dicts.find(id);
        if (it != m_hyph_dicts.end())
        {
            return it->second;
        }
        if (!m_hyph_langs_scanned)
        {
            LyHyphRescanLanguages();
        }
        const char *path = nullptr;
        for (const LyHyphLang &lang : m_hyph_langs)
        {
            if (lang.id == id)
            {
                path = lang.path.c_str();
                break;
            }
        }
        if (!path)
        {
            m_hyph_dicts[id] = nullptr;
            return nullptr;
        }
        HyphenDict *dict = hnj_hyphen_load(path);
        m_hyph_dicts[id] = dict;
        return dict;
    }

    std::string LyHyphResolveLangForWord(const std::string &word) const
    {
        if (std::strcmp(m_lyrics_hyph_lang, "auto") != 0)
        {
            return std::string(m_lyrics_hyph_lang);
        }
        return LyWordHasCyrillic(word) ? std::string("ru_RU") : std::string("en_US");
    }

    /* Split a single word into syllable fragments (no leading space except
     * when lead_space is true on the first piece). */
    void LySyllabifyWord(const std::string &raw_word,
                         bool lead_space,
                         bool starts_new_line,
                         int line_index,
                         std::vector<LyFragment> *out)
    {
        if (!out || raw_word.empty())
        {
            return;
        }

        /* Separate leading whitespace from the core word. */
        size_t i0 = 0;
        while (i0 < raw_word.size() && (raw_word[i0] == ' ' || raw_word[i0] == '\t'))
        {
            ++i0;
        }
        std::string core = raw_word.substr(i0);
        if (core.empty())
        {
            return;
        }

        /* Honor typed ASCII '-' and UTF-8 soft hyphen (U+00AD = C2 AD) as hard breaks. */
        std::vector<std::string> hard_parts;
        {
            std::string cur;
            for (size_t i = 0; i < core.size();)
            {
                const unsigned char c = static_cast<unsigned char>(core[i]);
                if (c == '-')
                {
                    if (!cur.empty())
                    {
                        hard_parts.push_back(cur);
                        cur.clear();
                    }
                    ++i;
                    continue;
                }
                if (c == 0xC2 && i + 1 < core.size() &&
                    static_cast<unsigned char>(core[i + 1]) == 0xAD)
                {
                    if (!cur.empty())
                    {
                        hard_parts.push_back(cur);
                        cur.clear();
                    }
                    i += 2;
                    continue;
                }
                const size_t n = LyUtf8CharLen(c);
                cur.append(core, i, n);
                i += n;
            }
            if (!cur.empty())
            {
                hard_parts.push_back(cur);
            }
        }
        if (hard_parts.empty())
        {
            hard_parts.push_back(core);
        }

        bool first_piece = true;
        for (const std::string &part : hard_parts)
        {
            const std::string lang = LyHyphResolveLangForWord(part);
            HyphenDict *dict = LyHyphGetDict(lang);
            std::vector<std::string> syllables;
            if (!dict || part.size() < 2)
            {
                syllables.push_back(part);
            }
            else
            {
                std::vector<char> hyphens(part.size() + 5, 0);
                char **rep = nullptr;
                int *pos = nullptr;
                int *cut = nullptr;
                if (hnj_hyphen_hyphenate2(dict,
                                          part.c_str(),
                                          static_cast<int>(part.size()),
                                          hyphens.data(),
                                          nullptr,
                                          &rep,
                                          &pos,
                                          &cut) != 0)
                {
                    syllables.push_back(part);
                }
                else
                {
                    /* hyphens[] is indexed by character index; odd = break after that char.
                     * Walk UTF-8 chars. */
                    size_t byte = 0;
                    size_t char_idx = 0;
                    size_t seg_start = 0;
                    while (byte < part.size())
                    {
                        const size_t n = LyUtf8CharLen(static_cast<unsigned char>(part[byte]));
                        const size_t next = byte + n;
                        const bool is_break =
                            (char_idx < part.size() && (hyphens[char_idx] & 1) && next < part.size());
                        if (is_break)
                        {
                            syllables.push_back(part.substr(seg_start, next - seg_start));
                            seg_start = next;
                        }
                        byte = next;
                        ++char_idx;
                    }
                    if (seg_start < part.size())
                    {
                        syllables.push_back(part.substr(seg_start));
                    }
                    if (syllables.empty())
                    {
                        syllables.push_back(part);
                    }
                }
                if (rep)
                {
                    for (size_t k = 0; k < part.size(); ++k)
                    {
                        if (rep[k])
                        {
                            free(rep[k]);
                        }
                    }
                    free(rep);
                }
                free(pos);
                free(cut);
            }

            for (size_t s = 0; s < syllables.size(); ++s)
            {
                LyFragment frag;
                if (first_piece && s == 0 && lead_space)
                {
                    frag.text = std::string(" ") + syllables[s];
                }
                else
                {
                    frag.text = syllables[s];
                }
                frag.starts_new_line = first_piece && s == 0 && starts_new_line;
                frag.line_index = line_index;
                frag.synced = false;
                frag.tick = 0;
                out->push_back(std::move(frag));
                first_piece = false;
            }
        }
    }

    void LyLoadHyphCatalog()
    {
        if (m_hyph_catalog_loaded)
        {
            return;
        }
        m_hyph_catalog.clear();
        const std::string paths[] = {
            LyHyphBundleDir() + "/hyph_catalog.json",
#if !defined(_WIN32)
            std::string("/usr/share/neobae/nbeditor/hyph_catalog.json"),
            std::string("/usr/local/share/neobae/nbeditor/hyph_catalog.json"),
#endif
            std::string("data/nbeditor/hyph_catalog.json"),
            /* Dev/makefile: catalog next to sources when cwd is neobae/. */
            std::string("../data/nbeditor/hyph_catalog.json"),
        };
        std::string json;
        for (const std::string &p : paths)
        {
            std::ifstream in(p, std::ios::binary);
            if (!in)
            {
                continue;
            }
            json.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!json.empty())
            {
                break;
            }
        }
        if (json.empty())
        {
            m_hyph_catalog_loaded = true;
            return;
        }
        /* Minimal object scan for { "id","name","file","url" } groups. */
        size_t pos = 0;
        while (pos < json.size())
        {
            const size_t id_key = json.find("\"id\"", pos);
            if (id_key == std::string::npos)
            {
                break;
            }
            auto read_str = [&](const char *key, size_t from) -> std::string {
                size_t k = json.find(std::string("\"") + key + "\"", from);
                if (k == std::string::npos || k > from + 400)
                {
                    return std::string();
                }
                size_t colon = json.find(':', k);
                size_t q1 = json.find('"', colon + 1);
                if (q1 == std::string::npos)
                {
                    return std::string();
                }
                size_t q2 = q1 + 1;
                while (q2 < json.size() && json[q2] != '"')
                {
                    if (json[q2] == '\\' && q2 + 1 < json.size())
                    {
                        q2 += 2;
                    }
                    else
                    {
                        ++q2;
                    }
                }
                if (q2 >= json.size())
                {
                    return std::string();
                }
                return json.substr(q1 + 1, q2 - q1 - 1);
            };
            LyHyphCatalogEntry e;
            e.id = read_str("id", id_key);
            e.name = read_str("name", id_key);
            e.file = read_str("file", id_key);
            e.url = read_str("url", id_key);
            pos = id_key + 4;
            if (!e.id.empty() && !e.file.empty() && !e.url.empty())
            {
                if (e.name.empty())
                {
                    e.name = e.id;
                }
                m_hyph_catalog.push_back(std::move(e));
            }
        }
        m_hyph_catalog_loaded = true;
    }

    void LyStartHyphDownload(const LyHyphCatalogEntry &entry)
    {
        if (m_hyph_dl_busy.load())
        {
            return;
        }
        const std::string dir = LyHyphConfigDir();
        if (dir.empty())
        {
            std::lock_guard<std::mutex> lock(m_hyph_dl_mutex);
            m_hyph_dl_status = "Config directory unavailable";
            return;
        }
        LyHyphEnsureDir(dir);
        const std::string dest = dir + "/" + entry.file;
        const std::string url = entry.url;
        const std::string label = entry.name;
        m_hyph_dl_busy = true;
        {
            std::lock_guard<std::mutex> lock(m_hyph_dl_mutex);
            m_hyph_dl_status = "Downloading " + label + "…";
        }
        std::thread([this, dest, url, label]() {
            char err[256] = {0};
            const bool ok = NbHttpGetToFile(url.c_str(), dest.c_str(), err, sizeof(err), 2u * 1024u * 1024u);
            {
                std::lock_guard<std::mutex> lock(m_hyph_dl_mutex);
                if (ok)
                {
                    m_hyph_dl_status = "Installed " + label;
                }
                else
                {
                    m_hyph_dl_status = std::string("Failed: ") + (err[0] ? err : "unknown");
                }
            }
            m_hyph_dl_busy = false;
        }).detach();
    }

    void LyDrawHyphGetMorePopup()
    {
        if (!m_hyph_get_more_open)
        {
            return;
        }
        ImGui::OpenPopup("Get more hyphen languages");
        m_hyph_get_more_open = false;
        LyLoadHyphCatalog();
    }

    void LyDrawHyphGetMoreModal()
    {
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Get more hyphen languages",
                                   nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped(
                "Downloads LibreOffice hyphenation patterns into\n%s",
                LyHyphConfigDir().c_str());
            ImGui::Separator();
            if (m_hyph_catalog.empty())
            {
                ImGui::TextDisabled("No catalog found (hyph_catalog.json).");
            }
            else if (ImGui::BeginTable("##hyph_cat",
                                       3,
                                       ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_ScrollY,
                                       ImVec2(420.0f, 280.0f)))
            {
                ImGui::TableSetupColumn("Language");
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("##dl", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableHeadersRow();
                const std::string cfg = LyHyphConfigDir();
                for (const LyHyphCatalogEntry &e : m_hyph_catalog)
                {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(e.name.c_str());
                    ImGui::TableSetColumnIndex(1);
                    bool have = false;
                    for (const LyHyphLang &l : m_hyph_langs)
                    {
                        if (l.id == e.id)
                        {
                            have = true;
                            break;
                        }
                    }
                    if (!have)
                    {
                        have = LyHyphFileExists(cfg + "/" + e.file);
                    }
                    ImGui::TextUnformatted(have ? "Installed" : "—");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::BeginDisabled(m_hyph_dl_busy.load() || have);
                    char btn[64];
                    std::snprintf(btn, sizeof(btn), "Download##%s", e.id.c_str());
                    if (ImGui::Button(btn))
                    {
                        LyStartHyphDownload(e);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndTable();
            }
            {
                std::lock_guard<std::mutex> lock(m_hyph_dl_mutex);
                if (!m_hyph_dl_status.empty())
                {
                    ImGui::TextWrapped("%s", m_hyph_dl_status.c_str());
                }
            }
            if (!m_hyph_dl_busy.load() && ImGui::Button("Rescan"))
            {
                LyHyphRescanLanguages();
            }
            ImGui::SameLine();
            if (ImGui::Button("Close"))
            {
                if (!m_hyph_dl_busy.load())
                {
                    LyHyphRescanLanguages();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

#endif /* USE_LIB_HYPHEN == TRUE */
