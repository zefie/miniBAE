/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef MOD2RMF_COMMON_H
#define MOD2RMF_COMMON_H

#include <stdint.h>
#include <stdlib.h>
#include <NeoBAE.h>
#include <xmp.h>
#include "mod2rmf_resampler.h"

#define MOD2RMF_MAX_CHANNELS 64
#define MOD2RMF_MAX_MIDI_CHANNELS BAE_MAX_MIDI_CHANNELS
#define MOD2RMF_MAX_SAMPLES 256
#define MOD2RMF_ROW_TICKS 120
#define MOD2RMF_SAMPLE_RATE 8287
/* BAE stores sample rate as 16.16 in a 32-bit word — max integer Hz is 65535. */
#define MOD2RMF_MAX_FIXED_RATE_HZ 65535u
/* BAERmfEditor stores bank as 14-bit (MSB: bits 7-13, LSB: bits 0-6).
 * Embedded RMF bank 2 must be encoded as MSB=2, LSB=0. */
#define MOD2RMF_EMBEDDED_BANK ((uint16_t)(2u << 7))
/* Instrument flag bits from X_Formats.h; duplicated here so mod2rmf stays
 * self-contained and does not depend on internal engine headers. */
#define MOD2RMF_ZBF_USE_SAMPLE_RATE 0x08
#define MOD2RMF_ZBF_USE_SMOD_AS_ROOTKEY 0x08
#define MOD2RMF_ZBF_ENABLE_INTERPOLATE 0x80
#define MOD2RMF_ZBF_ADVANCED_INTERPOLATION 0x80
#define MOD2RMF_ZBF_PLAY_AT_SAMPLED_FREQ 0x40
#define MOD2RMF_ZBF_ENABLE_SAMPLE_OFFSET_START 0x20
#define MOD2RMF_ZBF_DISABLE_SND_LOOPING 0x20
#define MOD2RMF_ZBF_SAMPLE_AND_HOLD 0x04
#define MOD2RMF_PITCH_BEND_CENTER       0x2000
#define MOD2RMF_PITCH_BEND_RANGE_ST     24
#define MOD2RMF_MOD_PERIOD_MIN          113
#define MOD2RMF_MOD_PERIOD_MAX          1712
/* Loop type constants for ModRawSample.loopType */
#define MOD2RMF_LOOP_FORWARD            0
#define MOD2RMF_LOOP_BIDIR              1
#define MOD2RMF_LOOP_REVERSE            2
/* Effect type constants (libxmp internal numbering, same as raw MOD/XM) */
#define MOD2RMF_FX_EXTENDED             0x0E
#define MOD2RMF_EX_RETRIG               0x09
#define MOD2RMF_EX_DELAY                0x0D
#define MOD2RMF_FX_OFFSET               0x09 /* sample offset (9xx); not EX_RETRIG */
#define MOD2RMF_FX_HIOFFSET             0x8c /* IT high offset (SAx) */
#define MOD2RMF_FX_MULTI_RETRIG         0x1B
#define MOD2RMF_FX_TONEPORTA            0x03
#define MOD2RMF_FX_TONE_VSLIDE          0x05
#define MOD2RMF_FX_VIBRA_VSLIDE         0x06
#define MOD2RMF_FX_VOLSLIDE             0x0a
#define MOD2RMF_FX_TRK_VOL              0x80
#define MOD2RMF_FX_TRK_VSLIDE           0x81
#define MOD2RMF_FX_TRK_FVSLIDE          0x82
#define MOD2RMF_FX_VSLIDE_UP_2          0xc0 /* IT volume-column slide */
#define MOD2RMF_FX_VSLIDE_DN_2          0xc1
#define MOD2RMF_FX_F_VSLIDE_UP_2        0xc2
#define MOD2RMF_FX_F_VSLIDE_DN_2        0xc3
#define MOD2RMF_SAMPLE_OFFSET_MAX_FRAMES 0x1FFFFFu /* 21-bit NRPN 6,0 */
/* Array capacity matches ZMF/editor limit; RMF path caps at BAE_RMF_MAX_ADSR_STAGES. */
#define MOD2RMF_MAX_ADSR_STAGES         BAE_EDITOR_MAX_ADSR_STAGES

typedef struct {
    int32_t level;  /* 0..VOLUME_RANGE */
    int32_t timeUs; /* microseconds */
    int32_t flags;  /* ADSR_LINEAR_RAMP_LONG, ADSR_SUSTAIN_LONG, etc. */
} ModAdsrStage;

typedef struct {
    bool valid;
    char name[23];
    uint32_t frameCount;
    uint32_t loopStart;
    uint32_t loopEnd;
    unsigned char rootKey;
    unsigned char defaultVolume; /* 0..64 from MOD sample header */
    int8_t  finetune;           /* unused; libxmp handles finetune via pitchbend */
    uint8_t loopType;           /* MOD2RMF_LOOP_FORWARD/BIDIR/REVERSE */
    bool   hasEnvelope;        /* instrument has an amplitude envelope */
    uint32_t adsrStageCount;
    ModAdsrStage adsrStages[MOD2RMF_MAX_ADSR_STAGES];
    int16_t defaultPan;          /* sub-instrument pan: 0..255, 128=center, -1=unset */
    /* IT/S3M: true C5/C2 playback rate recovered from libxmp xpo/fin.
     * sampleRateHz is the stored rate (octave-folded to fit 16.16 FIXED).
     * rateXpo is stripped from MIDI notes (pattern-key domain).
     * rateRootAdjust shifts sample baseKey only for octave folding
     * (negative = lower root, compensates a halved stored rate).
     * rateFin is baked into sampleRateHz and subtracted from pitch bends. */
    bool hasRateMapping;
    uint32_t sampleRateHz;
    int16_t rateXpo;
    int16_t rateFin;             /* 0..127 libxmp finetune units (128 = 1 st) */
    int16_t rateRootAdjust;      /* semitones added to root (usually 0 or -12*n) */
    int8_t *pcm8;
    /* Optional native 16-bit PCM (IT/XM). Kept alongside pcm8 so we can
     * inject full resolution when no 8-bit-only processing is required.
     * Cleared if bidi/reverse loop baking mutates the buffer. */
    int16_t *pcm16;
} ModRawSample;

typedef struct {
    uint32_t sourceSlot;
    unsigned char program;
    unsigned char rootKey;
    uint32_t sampleOffsetBytes;
    bool offsetVariant;
    bool hasSampleRateOverride;
    uint32_t sampleRateOverrideHz;
    bool hasVolumeAdsr;
    uint32_t adsrStageCount;
    ModAdsrStage adsrStages[MOD2RMF_MAX_ADSR_STAGES];
    int8_t panPlacement;         /* BAE pan: -128..+127, 0=center */
    char displayName[256];
    ModRawSample *rawSample;
} ModPlayable;

typedef struct {
    uint16_t sourceChannel;
    uint32_t startTick;
    uint32_t durationTicks;
    unsigned char note;
    unsigned char velocity;
    unsigned char program;
    uint32_t sampleOffsetFrames; /* 0 = start of sample; ZMF NRPN 6,0 */
} ModNoteEvent;

typedef struct {
    uint16_t sourceChannel;
    uint32_t tick;
    unsigned char cc;
    unsigned char value;
    unsigned char program;  /* active program when this CC was emitted (0xFF = unknown) */
} ModCCEvent;

typedef struct {
    uint16_t sourceChannel;
    uint32_t tick;
    uint16_t value; /* 14-bit, center 0x2000 */
    unsigned char program;  /* active program when this bend was emitted (0xFF = unknown) */
} ModPitchBendEvent;

typedef struct {
    uint32_t tick;
    uint32_t bpm;
} ModTempoChange;

typedef struct {
    char moduleName[256];
    char composerNotes[8192];
    uint32_t channelCount;
    uint32_t bpm;
    uint16_t pitchBendRangeSemitones;
    uint32_t playableCount;
    ModPlayable *playables;
    uint32_t noteCount;
    ModNoteEvent *notes;
    uint32_t noteCapacity;
    uint32_t ccCount;
    ModCCEvent *ccEvents;
    uint32_t ccCapacity;
    uint32_t pitchBendCount;
    ModPitchBendEvent *pitchBendEvents;
    uint32_t pitchBendCapacity;
    uint32_t tempoChangeCount;
    ModTempoChange *tempoChanges;
    uint32_t tempoChangeCapacity;
    /* Song-level loop markers (filled by build_song_model_native) */
    bool loopEnabled;
    uint32_t loopStartTick;  /* MIDI tick where playback should loop back to */
    uint32_t loopEndTick;    /* MIDI tick where the loop point is (end of song data) */
    /* Per source channel: program to re-emit at loopStartTick (0xFF = none).
     * Filled by mod2rmf_ensure_loop_program_resets. */
    uint8_t loopProgramReset[MOD2RMF_MAX_CHANNELS];
} ModSongModel;

typedef struct {
    bool active;
    uint64_t startTickFP;
    unsigned char note;
    unsigned char velocity;
    unsigned char program;
    int bendOffsetCents;
    int rateFinCents; /* subtract: finetune already baked into sample rate */
    uint32_t sampleOffsetFrames; /* sticky for retriggers until next note-on */
} ActiveNote;

typedef struct {
    uint8_t retrigInterval;     /* retrigger every N frames (0 = inactive) */
    uint8_t noteDelayFrames;    /* delay note-on by N frames (0 = no delay) */
    bool   hasDelayedNote;     /* a note is pending for this row */
    unsigned char delayedEvNote;/* the note value to trigger after delay */
    int     delayedSid;         /* sample ID for the delayed note */
    uint8_t delayedVolume;      /* volume at time of row start */
    uint32_t delayedSampleOffsetFrames;
} ChannelEffectState;

/* --- Channel mapping structs -------------------------------------------- */

/* Per-tracker-channel usage profile, built by analyze_channel_usage(). */
#define CHANNEL_PROFILE_MAX_PROGRAMS 16  /* max tracked unique programs per ch */

typedef struct {
    uint32_t startTick;
    uint32_t endTick;
} TickRange;

typedef struct {
    /* Which programs (sample-mapped instruments) appear on this channel */
    uint8_t programs[CHANNEL_PROFILE_MAX_PROGRAMS];
    uint32_t programCount;
    /* Active note ranges (sorted by startTick) */
    TickRange *activeRanges;
    uint32_t rangeCount;
    uint32_t rangeCapacity;
    /* Total note count */
    uint32_t noteCount;
    /* Whether this channel has any events at all */
    bool used;
} ChannelProfile;

/* Mapping from tracker channels (0..63) to MIDI channels (0..15). */
typedef struct {
    uint8_t trackerToMidi[MOD2RMF_MAX_CHANNELS];
    bool midiChannelUsed[MOD2RMF_MAX_MIDI_CHANNELS];
} ChannelMap;


int mod2rmf_compare_pitch_bend_by_tick(const void *a, const void *b);
int mod2rmf_compare_cc_by_tick(const void *a, const void *b);
uint16_t mod2rmf_pitchbend_to_midi(int32_t xmpPitchbend,
                                         uint16_t bendRangeSemitones);
/* maxStages: stage budget (typically BAE_RMF_MAX_ADSR_STAGES or BAE_EDITOR_MAX_ADSR_STAGES).
 * Clamped to [2, MOD2RMF_MAX_ADSR_STAGES]. */
void mod2rmf_extract_envelope_adsr(const struct xmp_instrument *inst,
                                  uint32_t bpm,
                                  ModRawSample *raw,
                                  uint32_t maxStages);
void mod2rmf_emulate_bidi_loop(ModRawSample *raw);
void mod2rmf_emulate_reverse_loop(ModRawSample *raw);
void mod2rmf_parse_row_effects(const struct xmp_event *ev,
                              uint8_t *outRetrigInterval,
                              uint8_t *outDelayFrames);
/* Parse FX_OFFSET / FX_HIOFFSET. Updates per-channel lo/hi memory.
 * When FX_OFFSET is present, sets *outHasOffset and *outOffsetFrames
 * (libxmp units == frames for our mono pcm8 path), clamped to 21-bit. */
void mod2rmf_parse_sample_offset(const struct xmp_event *ev,
                                 uint8_t *ioOffsetMemory,
                                 uint8_t *ioHiOffsetMemory,
                                 bool *outHasOffset,
                                 uint32_t *outOffsetFrames);
bool mod2rmf_get_row_volume_command(const struct xmp_event *ev,
                                   uint8_t *outVol64);

bool mod2rmf_row_has_tone_portamento(const struct xmp_event *ev);
/* TRUE if this row has an intentional volume slide (IT Dxx / vol-col slide). */
bool mod2rmf_row_has_volume_slide(const struct xmp_event *ev);

bool mod2rmf_is_mod_family(const char *type);
bool mod2rmf_is_s3m_family(const char *type);
bool mod2rmf_format_uses_sample_c5_rate(bool isIt, const char *type);
uint32_t mod2rmf_c2spd_from_xpo_fin(int xpo, int fin);
int mod2rmf_rate_fin_to_cents(int fin);
/* Fold rate into 16.16-safe Hz; returns stored rate and root semitone adjust. */
uint32_t mod2rmf_fold_rate_for_fixed(uint32_t rateHz, int16_t *outRootAdjust);
int mod2rmf_is_ascii_space(char c);
void mod2rmf_trim_copy_ascii(char *dst, size_t dstSize, const char *src);
void mod2rmf_append_linef(char *dst, size_t dstSize, const char *text);
uint32_t mod2rmf_fp_ticks_to_int(uint64_t fp);
unsigned char mod2rmf_vol_to_midi(unsigned char vol64);
unsigned char mod2rmf_note_velocity_from_volume(unsigned char vol64);
int mod2rmf_clamp_int(int v, int lo, int hi);
void mod2rmf_compute_channel_map(const ChannelProfile profiles[],
                                uint32_t trackerCount,
                                ChannelMap *map,
                                bool avoidMidiChannel10);
bool mod2rmf_ranges_overlap(const ChannelProfile *a, const ChannelProfile *b);
uint32_t mod2rmf_overlap_ticks(const ChannelProfile *a, const ChannelProfile *b);
const char *mod2rmf_path_basename_ptr(const char *path);

bool mod2rmf_sample_requires_processing(const ModPlayable *playable,
                                                const Mod2RmfResamplerSettings *settings,
                                                uint32_t moduleBaseRateHz);

uint8_t mod2rmf_apply_stereo_separation(uint8_t rawPan, uint32_t ch,
                                    bool isMod, uint8_t stereoSep);                                                
#endif /* MOD2RMF_COMMON_H */