# Beatnik Editor 2 Session `.bsn` format

Evidence from fixtures in [`reference/bsn/`](../reference/bsn/). Inspect with:

```bash
python3 tools/bsn_session_inspect.py reference/bsn/*.bsn
```

## Naming collision

| File | Role |
|------|------|
| **BE2 Session `.bsn`** | IREZ resource document: **optional bank body** + session songs + optional PCM masters + editor prefs |
| **NeoBAE / HSB bank `.bsn`** | IREZ bank + trailing `BEPF` only (no `BePf` Session Prefs) |
| **`.nbs`** | Old NeoBAE nbstudio LZMA TLV session (not BE2) |
| **`.zsn`** | NeoBAE Session with ZREZ map (ZMF/ZSB tripwires); same BePf/session layout |

Detector for BE2 editor documents: presence of resource type **`BePf`** named **`Session Prefs`**.

Plain converted banks (e.g. `patches111.bsn`) have `BEPF` but **not** `BePf`.

## Container

Same on-disk family as HSB/RMF banks:

| Offset | Field | Notes |
|--------|-------|--------|
| 0 | `'IREZ'` | Big-endian resource map |
| 4 | `uint32` version | Observed `1` |
| 8 | `uint32` resource count | |
| 12… | Resources | `nextOff`, type FOURCC, id, Pascal name, `uint32` bodyLen, body |

Trailing / editor resources typically include `BEPF`, `BePf`, `DATe`. Soft-deleted prior versions appear as **`TRSH`** (see `XFILETRASH_ID` in NeoBAE `X_API`).

## What lives where

| Content | In Session `.bsn`? | Notes |
|---------|--------------------|--------|
| Bank body (`INST`, `esnd`/`snd `, groovoid `SONG`+`emid`, …) | **Optional** | `empty.bsn` ≈ 473 KB full bank; `blank.bsn` ≈ 392 B has only stub `BANK`/`VERS` + prefs |
| User songs (`Midi` SMF + `SONG` metadata) | **Yes** (when present) | |
| Custom instruments / samples in bank | **When bank embedded** | e.g. `INST` 512, `snd ` id 0 MPEG |
| Uncompressed PCM edit masters | **Optional** | Resource type **`CaSd`** (classic Mac `snd` format 1 PCM) |
| Compressed *preview* / “compression cache” | **No** | App cache outside the file (Clear compression cache) |

NeoBAE Save Session embeds the **currently loaded bank** (whatever is in `m_bank_token`), not a forced full GM dump.

Fixture size deltas vs `empty.bsn`:

| File | Δ size | Cause |
|------|--------|--------|
| `1song.bsn` | +~10 KB | `Midi` + `SONG` |
| `…_nocompresscache_nouncompressed.bsn` | +~22 KB | song + MPEG `snd ` + INST (no `CaSd`) |
| `1song_1compressedsample.bsn` | +~140 KB | above + **`CaSd` ~118 KB** PCM |

## Session metadata

### `BePf` — Session Prefs

- Name: `Session Prefs`
- Body: 34 bytes (window / UI state; exact field map TBD)
- Present on Session saves and some editor bank exports
- NeoBAE still writes a stub for BE2 compatibility; real nbeditor layout is in `nBeT`

### `nBeT` — NeoBAE Editor layout (NeoBAE-only)

- Type: `nBeT` (`FOUR_CHAR('n','B','e','T')`)
- Name: `NeoBAE Editor`, id `1`
- On-disk body: plain layout blob, **or** `XCompressPtr` / `X_LZMA_RAW` frame (type byte `0xFE` + 3-byte uncompressed length + LZMA). Open accepts both; Save uses LZMA when it shrinks the payload.
- Plain layout blob (version 3 — current write):
  - `uint32` magic `'nBeT'` (big-endian), `uint32` version `3`
  - Main SDL window: `int32 x, y, w, h` + `uint32` maximized flag
  - `uint32` open_flags (bit0 IE, bit1 Sample Editor, bit2 Song Info, bit3 Song Settings)
  - IE context: `inst_id`, `instrument_index`, `bank`, `program`, `from_song`
  - SE context: `sample_row`, `is_song_sample`
  - Player mix: `volume_percent`, `tempo_percent`, `reverb_type`
  - `uint32` length + UTF-8 ImGui ini (`SaveIniSettingsToMemory`) for dock + floating window geometry
- Version 1/2 (read still supported): older headers without mix / with fewer fields
- ImGui ini alone cannot reopen gated dialogs; open_flags drives IE/SE/Song Info restore
- Written/read by nbeditor Save/Open Session; BE2 ignores unknown resource types
- Edit → **Reset Layout** restores default Player/Session/Status dock and closes floating editors
- Distinct from app prefs (`~/.config/neobae/nbeditor_prefs`):
  - `save_session_layout=1` (default) — write `nBeT` on Save Session
  - `ignore_session_layout=0` (default) — when `1`, Open Session skips applying `nBeT`

### `BEPF`

- Small trailer resource (often body `example`); also used by NeoBAE `tools/hsb_bsn.py` bank toggle

### `DATe` — datestamp list

Packed records of 12 bytes:

```
FOURCC type | uint32 id | uint32 timestamp
```

Terminated by zeros. Tracks create/modify times for `Midi`, `SONG`, `snd `, `CaSd`, `INST`, `BePf`, etc.

### `TRSH`

Marked-deleted resources (old `BEPF` / `BePf` / `DATe` bodies) left in the file until compacted.

## Songs

Built-in groovoids: `SONG` (high random-ish IDs) → **`emid`** objects (classic banks).

**User Session songs:** `SONG` → **`Midi`** objects containing raw Standard MIDI (`MThd…`), and stamped in **`DATe`** (`Midi` + `SONG` entries).

Some sessions (e.g. `zpatches.bsn`) store bank groovoids as decrypted `SONG`→`Midi` as well. Those keep the original high `SONG` IDs and are **not** DATe-stamped; only the editor-added custom songs appear in `DATe`. NeoBAE uses that DATe filter so Custom Songs vs Groovoids still split correctly when both use `Midi`.

`SONG` body is `SongResource_RMF` ([`X_Formats.h`](../neobae/src/BAE_Source/Common/X_Formats.h) / [`BAE_Resource_SONG.txt`](BAE_Resource_SONG.txt)):

- `uint16` object id → `Midi` / `emid` resource id  
- `songType == 1` (RMF structured)  
- trailing typed blocks, e.g. `TITL` + C string  

Examples:

| SONG id | Name | Midi id |
|---------|------|---------|
| 0 | Cool Chick | 73 |
| 1 | Brook | 74 |

## Samples / PCM cache

- **Committed compressed sample:** bank `snd ` / `csnd` / `esnd` (fixtures use type-3 `snd ` with MPEG subtype for “test (MPEG 128k)”).
- **Uncompressed master in session:** **`CaSd`**, named like the sample (`test`), body is classic **`snd` format 1** with ExtendedHeader (`0xFF`) and PCM payload (not type-3 MPEG).
- Removing uncompressed originals drops `CaSd`; bank `snd ` remains.
- Compressed preview cache is **not** a session resource.

## Open strategy (NeoBAE)

1. Detect `BePf` / `Session Prefs` (editor session document; flat even in ZREZ).
2. Load whole file with existing IREZ/ZREZ bank loader (`BAEMixer_AddBankFromFile`).
3. Enumerate user songs via **XFILE APIs** (`XCountFileResourcesOfType` / `XGetFileResource`), not a raw flat resource walk — ZREZ sessions pack `SONG`→`ZSNG` and `Midi`→`ZBNK`. Keep DATe filter: stamped `SONG`→`Midi` when any Midi/SONG stamps exist; otherwise all `SONG`→`Midi`.
4. Enumerate groovoids the same way: `SONG`→`emid`, plus unstamped `SONG`→`Midi` when the DATe filter is active (`XExistsFileResource` expands ZBNK for Midi).
5. Load active song via `BAERmfEditorDocument_LoadFromMemory(midiBytes, …, BAE_MIDI_TYPE)`.
6. Optional later: hydrate SE PCM masters from `CaSd` by matching names/ids to bank samples.

## Write / Phase 3 gate (2026-08)

| Goal | Assessment |
|------|------------|
| **Open** BE2 Session `.bsn` | **Go** — shipped prototype: nbeditor File→Open on `.bsn` with `BePf`/`Session Prefs`; bank via `BAEMixer_AddBankFromFile`; user songs via `SONG`→`Midi` → `BAERmfEditorDocument_LoadFromMemory` |
| **Write** BE2-openable Session `.bsn` | **Go for Phase 3 classic path** — same IREZ writer stack as HSB/BSN banks; must emit full bank + `Midi`/`SONG` + optional `CaSd` + `BePf`/`DATe`/`BEPF`. Verify in BE2 before claiming compatibility. |
| **Compressed preview cache in file** | **No** — app cache only (`Clear compression cache`) |
| **Z / modern codecs in session** | **`.zsn`** (ZREZ); not BE2-openable |

Phase 3 Save Session matrix (unchanged intent):

- Classic-only content → write Session **`.bsn`** (IREZ layout above)
- Needs ZMF/ZSB → **`.zsn`**
- Do **not** rename `.nbs` to `.bsn`

Unknowns for write (non-blocking for starting Phase 3): exact `BePf` field map; whether BE2 tolerates omitting `TRSH`; `CaSd` id pairing to bank `snd `; SE hydration of `CaSd` into the in-memory PCM master (open prototype loads bank sample + song only).

Fallback if BE2 rejects our writes: keep open path; Save Session as `.zsn` + document gap.

## Fixtures

| Path | Purpose |
|------|---------|
| `empty.bsn` | Full bank + prefs, no user songs |
| `blank.bsn` | Minimal editor shell (almost empty) |
| `1song.bsn` | One user MIDI song |
| `1song_1compressedsample.bsn` | Song + MPEG sample + `CaSd` PCM |
| `…_nocompresscache_nouncompressed.bsn` | Song + MPEG sample, no `CaSd` |
| `…_multisong.bsn` | Two user songs, no `CaSd` |
| `Idefix Bank.bsn` / `Underground Export Bank.bsn` | Editor-exported banks (also have `BePf`) |
