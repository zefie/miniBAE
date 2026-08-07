# BAE_EditorAPI performance inventory

Scan of editor bank/document mutation paths in `BAE_EditorAPI*.c` and the
Clean/pack helpers they call in `X_API.c` / `X_API.h`.

**Scan date:** 2026-08-07 (updated for optional-LZMA / flat-ZREZ editor policy).

---

## Editor policy: Pack + LZMA is export-only

**Working set (editor, session, in-RAM audition):** flat `ZREZ` map is valid.
INST/SND/ALIAS/SONG/MIDI may live as normal flat resources. Existing packed
`ZINS`/`ZSNG`/`ZBNK`/`ZSHD` from ship banks stay opaque; edits use flat shadows
or omit-without-repack. **Never** PackInst/Song/Bank/ZSHD (LZMA) on interactive
paths.

**Ship export:** `XPackResourceFileForShip` / `XCleanResourceFile` /
`SaveAsRmfToMemory` (default) / `SaveToMemoryEx` with `packForShip=TRUE`.

| Helper | Meaning |
|--------|---------|
| `XFinalizeEditorResourceFile` | `XRebuildResourceFileCache` — cache only |
| `XPackResourceFileForShip` | Full Clean (Pack* + LZMA on ZREZ) |
| `SaveAsRmfToMemoryEx(..., packForShip)` | Preview/playback → `FALSE`; disk export → `TRUE` |
| `SaveToMemoryExFlags(..., packForShip)` | Session clone → `FALSE`; export → `TRUE` |

**CSND / codec vs Pack:** Sample editor **Apply** writes **PCM `SND`** into the
live bank. Chosen codec + CSND/ESND storage are **session export prefs**
(`SessionPcmCacheEntry.has_export_target`) applied in `ExportBankCloneToPath`
via `ApplySessionExportTargetsToBank` before ship write. Audition uses
`PreviewCompressPCM` (RAM only). PackInst/Song/Bank LZMA remains export-only.

**Uncompressed Z\* blocks (Phase 2 — gated off):** Flat ZREZ + opaque packed Z\*
from ship banks is enough after Purge/Unload emit flat INST and FinalizeEditor.
No plain-ZINS writer unless session size / omit cost regresses measurably.
See [`BE2_RESOURCE_MODEL.md`](BE2_RESOURCE_MODEL.md).

---

## Beatnik Editor 2 (light RE)

Binary: `/mnt/d/bin/BXPlayer/Editor/Beatnik Editor 2.exe` (2001).

Strings / docs show classic **IREZ + INST/SND**, `BAEFileResource`,
`CompressionDialog` / `CompressionManager` (per-sample), instrument UI —
**no ZINS/ZREZ/LZMA bank packing**. Import expands audio to uncompressed form;
sample compression is a deliberate dialog step. That matches NeoBAE’s flat
working-set policy: compression is an authoring/export choice, not something
done on every edit.

---

## Clean / pack matrix (`X_API`)

| API | PackInst | PackSong | PackBank | PackSndHeaders (ZSHD) | Notes |
|-----|:--------:|:--------:|:--------:|:---------------------:|-------|
| `XCleanResourceFile` / `XPackResourceFileForShip` | yes | yes | yes | yes | Ship export only |
| `XCleanResourceFileEx(f, packSndHeaders)` | yes | yes | yes | arg | Still PackSong+PackBank |
| `XCleanResourceFileOptions(f, packInst, packSndHeaders)` | arg | **yes** | **yes** | arg | Historic trap |
| `XCleanResourceFileOptionsEx(...)` | arg | arg | arg | arg | Full control |
| `XRebuildResourceFileCache` / `XFinalizeEditorResourceFile` | no | no | no | no | Editor / session / purge commit |

---

## Replace taxonomy (`BAE_EditorAPI` bank layer)

| Helper | What it does | Typical cost |
|--------|----------------|--------------|
| `XReplaceFileResource` | Flat trash+append; ZINS INST/ALIAS shadow | O(resource) |
| `PV_BankReplaceInstResourceInPlace` | XReplace only | Fast when writable |
| `PV_BankCommitResource` | XReplace → RebuildCache Replace | Clone/Alias/Grow/Delete |
| `PV_BankReplaceSndResourceInPlace` | Same-type XReplace; else MultipleSnd | Mitigated |
| `PV_BankReplaceResource` / `Ex` | Type-list copy + RebuildCache | No Pack LZMA |
| Purge / UnloadBank omit | Flat INST emit; FinalizeEditor commit | No ZINS re-LZMA |

---

## Keymap Vol all→192 (post flat XReplace)

**Cause:** `XReplaceFileResource` appends a flat INST shadow; `XGetIndexedFileResource`
enumerates flats first, so `m_ie_instrument_index` no longer points at the edited
INST. The table then reads another instrument’s splits (often `miscParameter2=192`).

**Fix:** `SyncInstrumentEditorBankIndex()` via `ResolveInstID` after keymap Vol /
zone / Grow / Delete / SetSndID (same pattern as Ext Apply).

---

## Mitigated interactive LZMA sites (this pass)

| Site | Fix |
|------|-----|
| `XPurgeFileResource` / trash rebuild commit | FinalizeEditor (was full Clean) |
| `XPurgeFileInstrumentAndSoundLists` | Flat INST/ALIAS emit; no ZINS `XCompressPtr` |
| nbeditor unload/merge / CaSd clear | `XFinalizeEditorResourceFile` |
| RMF preview / playback / undo / session song embed | `SaveAsRmfToMemoryEx(..., FALSE)` |
| Session IREZ→ZREZ clone | `SaveToMemoryExFlags(..., FALSE)` |
| Disk SaveAsRmf / bank Export / SaveToFile | Still pack (`TRUE` / default Ex) |

---

## Positive patterns

- Flat ZREZ working set; Pack only at export
- INST mutations via `XReplaceFileResource`
- `sampleRate == 0` INST-only keymap commits
- Volume gated on sample-offset / sound-modifier flags
- Opaque Z\* copy in MultipleSnd; ZINS extract from retained decode
- BE2 model: flat IREZ + optional sample compression dialog

---

## Module map (post-split)

| Module | Responsibility |
|--------|----------------|
| `BAE_EditorAPI_bank.c` | Bank mutate / pack / batch / save |
| `BAE_EditorAPI_document.c` | Document + `SaveAsRmfToMemoryEx` |
| `BAE_EditorAPI_rmf_io.c` | `PV_WriteRmfDocumentToResourceFile(..., packForShip)` |
| `X_API.c` | Clean / Finalize / PackForShip / Purge / ZINS cache |
