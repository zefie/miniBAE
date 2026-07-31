# Native DLS / MobileBAE Plus compatibility analysis

Reference: `reference/mBAE_plus14.dll` and Hex-Rays output
`reference/mBAE_plus14.dll.c`.

Implementation reviewed: `neobae/src/BAE_Source/Common/GenDLS_MobileBAE.c`.

## Executive summary

The current native DLS engine is not yet an accurate MobileBAE Plus renderer.
It mixes three different targets in one implementation:

1. behavior recovered from MobileBAE Plus / RetroDLS;
2. standards-oriented DLS2 and SP-MIDI behavior; and
3. NeoBAE integration heuristics added to make particular XMF/HSB files play.

The largest audible correctness bug is that native DLS creates only one voice
for the first matching region. MobileBAE Plus iterates every region and starts a
voice for every matching region. This breaks layered instruments outright.

Several other high-confidence differences are listed below. Line numbers refer
to the current source at the time of analysis.

## High-confidence defects

### 1. Only the first matching region is rendered (critical)

Native code:

- `DLS_Synth_FindRegion()` returns immediately on the first match
  (`GenDLS_MobileBAE.c:2196`).
- `GM_DLS_ProcessNoteOn()` obtains that single region and initializes one voice
  (`GenDLS_MobileBAE.c:2907`).

MobileBAE Plus:

- `sub_11F7110` loops from region index zero through the instrument region
  count (`reference/mBAE_plus14.dll.c`, around lines 82280-82345).
- For every region whose key and velocity ranges match, it allocates and starts
  a separate voice.

Impact: layered DLS instruments lose all but one layer. This changes timbre,
level, stereo image, envelopes, filters, and polyphony.

Required fix: replace the single-region lookup in note-on with iteration over
all matching regions. Exclusive-class handling and voice allocation must run
per matching region, as in `sub_11F7110`.

### 2. Polyphony is hard-coded to 256 and bypasses mixer/song voice limits

Native code stores `DLS_Voice voices[256]` and scans all 256 voices. It does not
use the configured mixer `MaxNotes`, song voice limit, or active voice budget.

MobileBAE Plus tracks configured voice limits. `sub_11F7110` checks a per-song
limit before voice creation, and `sub_11F6BD0` / `sub_11F67C0` participate in
allocation and stealing. The DLL also maintains global and per-channel active
voice counts.

Impact: DLS songs can exceed requested polyphony, consume much more CPU than
other synth paths, and steal voices differently from MobileBAE.

Required fix: size or cap the active DLS pool from mixer/song voice settings and
implement the DLL's allocation order against that cap.

### 3. Program/bank lookup contains non-MobileBAE guessing

`DLS_Bank_FindMidiInstrument()` and `DLS_Synth_FindInstrument()` contain broad
fallbacks not supported by the DLL behavior:

- parity conversion between HSB and raw selectors;
- probing a parity-swapped sibling bank;
- scanning an entire overlay for any matching program in the same family;
- percussion fallback to the first program found;
- main/overlay retry sequences that can select an unintended instrument.

MobileBAE Plus stores a concrete 14-bit bank selector in channel state. In
`sub_11F7520`, controller 256 selects either explicit 120/121 mode or the
channel-default family (121 melodic, 120 on channel 10). Note-on then resolves
the selected program/bank object; it does not scan arbitrary programs.

Impact: missing patches silently play the wrong instrument, especially in XMF
overlays and percussion banks. This also obscures parser/selector bugs.

Required fix: separate exact MobileBAE selector behavior from an optional
NeoBAE compatibility fallback. Quirks mode should use exact lookup and fail
silently when the selected instrument is absent.

### 4. Region articulation incorrectly replaces instrument articulation

Native parsing marks a region with `lart`/`lar2` as owning its articulation and
does not combine it with instrument articulation. A region without articulation
receives a deep copy of the instrument articulation (`GenDLS_MobileBAE.c`,
around 1340).

MobileBAE's object flow merges articulation connections into the effective
instrument/region connection set and then adds missing defaults
(`sub_11F4670`, `sub_11F4720`, and their callers around lines 79100-79500).
DLS semantics also require region-level articulation to override matching
connections while retaining non-overridden instrument connections—not replace
the entire instrument set.

Impact: any instrument with both instrument-level and region-level articulation
loses modulation routes and/or direct generator values.

Required fix: build each region's effective connection list by copying
instrument connections, then replacing connections with the same
source/control/destination from the region list, then add absent defaults.

### 5. Controller reset behavior is wrong

Native CC121 calls `dls_channel_reset_controllers()` for every value and also
clears the selected instrument/program state (`GenDLS_MobileBAE.c:3043-3048`).

In MobileBAE Plus `sub_11F7520`, controller 121 is acted upon only when the
value is 127, and it resets channel controller state without performing a
program change or deleting the selected instrument.

Impact: ordinary CC121 events can leave the channel with no bound program until
another lazy selection occurs, and behavior differs for values other than 127.

Required fix: in quirks mode, gate the operation on value 127 and preserve the
selected program/instrument.

### 6. Omni/mono/poly mode controllers are collapsed into All Notes Off

Native code treats CC123 and all CC124-127 alike by releasing notes.

MobileBAE Plus `sub_11F7520` distinguishes them:

- 124/125 perform all-notes-off only when the channel is not percussion;
- 126 performs all-notes-off and enters mono mode;
- 127 conditionally performs all-notes-off and returns to poly mode;
- percussion channels bypass parts of this behavior.

Impact: channel mode state and percussion behavior are incorrect.

Required fix: represent the channel mode bits and implement the four distinct
paths.

### 7. Pitch bend and controller indexing are not safely masked

`GM_DLS_ProcessPitchBend()` indexes `channels[channel]` directly, as do parts of
note-on/controller processing, while other handlers use `channel & 0x0F`.

Impact: malformed or expanded channel values can access outside the 16-channel
array. Even when callers currently pass 0-15, this inconsistency is unsafe.

Required fix: normalize the channel once at every public event entry point.

### 8. Runtime filter resonance modulation is discarded

`dls_filter_update()` explicitly ignores `runtimeResonanceDelta`, and
destination `0x501` is also ignored in `dls_apply_runtime_connection()`.

MobileBAE accepts filter resonance as a valid destination (`sub_11F44C0`) and
stores direct resonance in `sub_11F4720`. Runtime routes to this destination
must affect filter coefficients.

Impact: DLS2 articulations modulating resonance have no effect.

Required fix: calculate effective resonance alongside cutoff and recompute
coefficients when either changes.

### 9. Renderer output gain is an arbitrary post-hoc constant

The final mix uses mutable function-static `DLS_GAIN_FACTOR = 5`, then applies
`(32 * dry) >> 5` (unity) and `(20 * wet) >> 5`. This is not derived from the
MobileBAE mixer, NeoBAE `mixLevel`, song volume, master volume, or configured
voice normalization.

MobileBAE Plus computes channel/song gain before voice creation and applies
configured mixer normalization. For example, `sub_11F7110` caches song/channel
gain values, while the voice renderer consumes those values.

Impact: DLS loudness changes independently of mixer configuration and does not
balance consistently with RMF/SF2. Wet/dry ratios are also invented.

Required fix: feed native DLS through the same BAE scale-back, song-volume, and
master-volume policy as other MIDI voices. Keep MobileBAE's per-voice fixed
point gain behavior before that shared mixer scaling.

### 10. Quirks mode is global, mutable, and bank-incoherent

`g_use_mobilebae_quirks` is process-global. Mode refresh temporarily mutates it,
forces overlays to quirks mode, kills all voices, and clears channel selections.
Voice stealing determines mode by OR'ing the modes of both loaded banks, while
other operations choose mode from the selected voice's bank.

Impact: two mixers cannot safely use different modes; changing a GUI option can
invalidate unrelated playback; loading one quirks overlay changes allocation
policy for voices from the standards bank.

Required fix: store mode on `DLS_Synth`/mixer and resolve it per selected bank
or voice. Do not use a process-global during parsing or articulation rebuild.

## Additional correctness concerns

### 11. Note-off releases every same-key voice, not one note-on instance

Native note-off clears `keyHeld` on all active voices with the channel/key.
This is appropriate for all layers belonging to one note-on, but once layered
regions are implemented it also needs a note-instance identifier so overlapping
repeated note-ons can be paired according to MobileBAE's voice-list behavior.

### 12. Sustain is sampled only at the 10 ms control tick

The current voice stores `sustainSnapshot` during rendering rather than handling
pedal transitions as channel events. Release can therefore be delayed by up to
one control period, and a quick pedal transition between ticks can be lost.

### 13. Missing bounds/overflow validation in parser allocation arithmetic

Several checks use 32-bit expressions such as `8 + count * 12`,
`20 + cSampleLoops * 16`, and `recordOffset + count * 8`. Crafted values can
overflow before comparison. Declared region/instrument counts are also used for
allocation without practical caps.

Required fix: use subtraction-based bounds checks or checked `size_t`
multiplication before every allocation/chunk traversal.

### 14. Parse errors from nested chunks are commonly ignored

Calls such as `DLS_Parse_ArticulationChunk`, `DLS_Parse_Region`, and
`DLS_Parse_Instrument` are often made without propagating their `OPErr` result.
An allocation failure or malformed nested chunk can produce a partially loaded
bank reported as successful.

### 15. Standards mode is not actually DLS2 compliant

The non-quirks default velocity-to-gain route intentionally retains the
MobileBAE -48 dB scale rather than DLS2's -96 dB scale. The comment says this is
to keep drums audible. Thus the setting currently named non-quirks/SP-MIDI is a
hybrid compatibility mode, not a standards mode.

## Recommended implementation order

1. Render all matching regions and add a note-instance ID.
2. Implement exact MobileBAE bank/program selection without heuristic scanning.
3. Correct articulation inheritance/override merging.
4. Connect voice allocation to configured mixer/song polyphony.
5. Correct CC121 and CC124-127 semantics and event-time sustain handling.
6. Make quirks mode mixer-local and bank/voice-local.
7. Replace arbitrary final gain with shared BAE mixer normalization.
8. Implement resonance modulation and then audit the remaining connection
   transforms against `sub_11F44C0` and the `sub_11E0xxx` voice routines.
9. Harden parser arithmetic and error propagation.

## Useful decompiled anchors

- `sub_11F44C0`: validates supported source/control/destination combinations.
- `sub_11F4670`: adds the nine default DLS connections.
- `sub_11F4720`: consumes direct-source connections into articulation fields.
- `sub_11F5600`, `sub_11F5A10`, `sub_11F67C0`: voice selection/allocation.
- `sub_11F5680`: channel/song reset defaults.
- `sub_11F5D00`: channel gain curve.
- `sub_11F5D50`: pitch bend normalization.
- `sub_11F5D80`: coarse tuning normalization.
- `sub_11F7110`: note-on, all matching regions, exclusive classes, allocation.
- `sub_11F7520`: channel-mode and bank-controller behavior.
