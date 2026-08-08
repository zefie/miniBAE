# nbeditor hyphenation data

TeX/LibreOffice hyphenation dictionaries used by NeoBAE Editor’s karaoke
**Apply text → Syllables** mode (when built with `USE_LIB_HYPHEN=1`).

## Layout

| File | Purpose |
|------|---------|
| `hyph_en_US.dic` | English (US) patterns (Windows suite bundle) |
| `hyph_ru_RU.dic` | Russian patterns (Windows suite bundle) |
| `hyph_catalog.json` | Download catalog for additional languages |

At runtime, dictionaries are discovered in this order (same basename → earlier wins):

1. `<NeoBAE config>/hyphen/` (downloads; writable)
2. `$exeDir/nbeditor/` (Windows suite next to `nbeditor.exe`)
3. `/usr/share/neobae/nbeditor/` (Linux packaged catalog)
4. `/usr/share/hyphen/`
5. `/usr/local/share/hyphen/`
6. `/usr/share/myspell/dicts/`

On Linux the editor binary is named `nbeditor`, so a sibling `nbeditor/`
data folder is not used. Install distro packages such as `hyphen-en-us` and
`hyphen-ru`, or use **Get more languages…** to fetch into the config directory.

Sources: [LibreOffice/dictionaries](https://github.com/LibreOffice/dictionaries),
engine: [hunspell/hyphen](https://github.com/hunspell/hyphen).
