# XSoundHeader3 Reserved Field Usage

This document records the authoritative allocation of all bits/bytes in the
`XSoundHeader3` reserved fields (`reserved2` and `reserved3`).  Update this
file any time a new bit or byte is claimed.

---

## Field Locations in `XSoundHeader3` (`X_Formats.h`)

```c
typedef struct XSoundHeader3 {
    /* ... */
    unsigned char   reserved2[2];   /* 2 bytes — flag byte extension */
    unsigned char   reserved3[8];   /* 8 bytes — currently unused    */
    /* ... */
} XSoundHeader3;
```

---

## `reserved2[0]` — Codec/playback flag bits

| Bit mask | Constant name                      | Value  | Description |
|----------|------------------------------------|--------|-------------|
| `0x01`   | `XSOUND_OPUS_ROUNDTRIP_RESAMPLE`   | `0x01` | **Opus Round-Trip Resampling**: the PCM was encoded without prior resampling; the encoder was told the input rate was 48 000 Hz so it stored a sped-up bitstream. The `sampleRate` field in the header contains the **true source rate** (e.g. 22 050 Hz). On decode, the engine reads this rate and time-stretches the decoded 48 kHz PCM back to the original pitch. |
| `0x02`–`0x80` | *(free)* | — | Not yet allocated. |

### Who writes `XSOUND_OPUS_ROUNDTRIP_RESAMPLE`
* **Save path** — `BAE_EditorAPI.c` → `BAERmfEditorDocument_Save`:
  - When `sample->opusUseRoundTripResampling == TRUE`, the Opus encode loop
    spoofs `writeWaveform.sampledRate = 48000 << 16` before calling
    `XCreateSoundObjectFromData`, then calls `XSetSoundSampleRate` to restore
    the true source rate, and finally calls `XSetSoundOpusRoundTripFlag(snd,
    TRUE)` to write this bit.
* **Writer helper** — `SampleTools.c`: `XSetSoundOpusRoundTripFlag(XPTR, XBOOL)`

### Who reads `XSOUND_OPUS_ROUNDTRIP_RESAMPLE`
* **Engine decode path** — `SampleTools.c` → `XGetSamplePtrFromSnd`:
  - In the `XType3Header` / `C_OPUS` case, if the flag is set the stored
    `sampleRate` is saved before `XDecodeSampleData` overwrites it with
    48 000.  After decode the saved rate is restored in `info->rate` so the
    engine's mixer time-stretches correctly.
* **Editor load path** — `BAE_EditorAPI.c` → `PV_AddEmbeddedSampleVariant`:
  - After constructing the `BAERmfEditorSample`, calls
    `XGetSoundOpusRoundTripFlag(sndCopy)` and stores the result in
    `sample->opusUseRoundTripResampling` so round-trip mode is preserved
    through edit/resave cycles.
* **Reader helper** — `SampleTools.c`: `XGetSoundOpusRoundTripFlag(XPTR) → XBOOL`

---

## `reserved2[1]` — *(free)*

Not yet allocated.

---

## `reserved3[0..7]` — *(all free)*

Not yet allocated.

---

## How to allocate a new flag

1. Pick the lowest free bit in the appropriate byte.
2. Add a `#define XSOUND_YOUR_FLAG  0xXX` constant near the existing flags in
   `X_Formats.h` (after the block starting with `// XSoundHeader3 reserved2[0] flag bits`).
3. Add declarations for read/write helpers in `X_Formats.h` and implement them
   in `SampleTools.c` following the pattern of `XSetSoundOpusRoundTripFlag`.
4. Update this document.
