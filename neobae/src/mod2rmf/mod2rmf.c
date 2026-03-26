/****************************************************************************
 *
 * mod2rmf.c
 *
 * Tracker module -> RMF/ZMF converter (via libxmp).
 * Supports all formats handled by libxmp (MOD, S3M, XM, IT, etc.).
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>

#include <xmp.h>

#include <NeoBAE.h>
#include <X_Formats.h>

#include "mod2rmf_encoder.h"
#include "mod2rmf_resampler.h"

#define MOD2RMF_MAX_CHANNELS 64
#define MOD2RMF_MAX_MIDI_CHANNELS BAE_MAX_MIDI_CHANNELS
#define MOD2RMF_MAX_SAMPLES 256
#define MOD2RMF_ROW_TICKS 120
#define MOD2RMF_SAMPLE_RATE 8287
/* BAERmfEditor stores bank as 14-bit (MSB: bits 7-13, LSB: bits 0-6).
 * Embedded RMF bank 2 must be encoded as MSB=2, LSB=0. */
#define MOD2RMF_EMBEDDED_BANK ((uint16_t)(2u << 7))
/* Instrument flag bits from X_Formats.h; duplicated here so mod2rmf stays
 * self-contained and does not depend on internal engine headers. */
#define MOD2RMF_ZBF_USE_SAMPLE_RATE 0x08
#define MOD2RMF_ZBF_USE_SMOD_AS_ROOTKEY 0x08
#define MOD2RMF_ZBF_ENABLE_INTERPOLATE 0x80
#define MOD2RMF_ZBF_ADVANCED_INTERPOLATION 0x80
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
#define MOD2RMF_FX_MULTI_RETRIG         0x1B
#define MOD2RMF_FX_TONEPORTA            0x03
#define MOD2RMF_FX_TONE_VSLIDE          0x05
#define MOD2RMF_MAX_ADSR_STAGES         8 /* matches BAE_EDITOR_MAX_ADSR_STAGES */

typedef struct {
    int32_t level;  /* 0..VOLUME_RANGE */
    int32_t timeUs; /* microseconds */
    int32_t flags;  /* ADSR_LINEAR_RAMP_LONG, ADSR_SUSTAIN_LONG, etc. */
} ModAdsrStage;

typedef struct {
    XBOOL valid;
    char name[23];
    uint32_t frameCount;
    uint32_t loopStart;
    uint32_t loopEnd;
    unsigned char rootKey;
    unsigned char defaultVolume; /* 0..64 from MOD sample header */
    int8_t  finetune;           /* unused; libxmp handles finetune via pitchbend */
    uint8_t loopType;           /* MOD2RMF_LOOP_FORWARD/BIDIR/REVERSE */
    XBOOL   hasEnvelope;        /* instrument has an amplitude envelope */
    uint32_t adsrStageCount;
    ModAdsrStage adsrStages[MOD2RMF_MAX_ADSR_STAGES];
    int16_t defaultPan;          /* sub-instrument pan: 0..255, 128=center, -1=unset */
    int8_t *pcm8;
} ModRawSample;

typedef struct {
    uint32_t sourceSlot;
    unsigned char program;
    unsigned char rootKey;
    uint32_t sampleOffsetBytes;
    XBOOL offsetVariant;
    XBOOL hasSampleRateOverride;
    uint32_t sampleRateOverrideHz;
    XBOOL hasVolumeAdsr;
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

static XBOOL mod2rmf_sample_requires_processing(const ModPlayable *playable,
                                                const Mod2RmfResamplerSettings *settings,
                                                uint32_t moduleBaseRateHz)
{
    uint32_t srcRate;

    if (!playable || !settings)
    {
        return FALSE;
    }

    if (settings->amigaFilter != MOD2RMF_AMIGA_FILTER_NONE)
    {
        return TRUE;
    }

    if (settings->targetRate == 0)
    {
        return FALSE;
    }

    srcRate = moduleBaseRateHz;
    if (playable->hasSampleRateOverride && playable->sampleRateOverrideHz > 0u)
    {
        srcRate = playable->sampleRateOverrideHz;
    }

    return settings->targetRate != srcRate;
}

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
    XBOOL loopEnabled;
    uint32_t loopStartTick;  /* MIDI tick where playback should loop back to */
    uint32_t loopEndTick;    /* MIDI tick where the loop point is (end of song data) */
} ModSongModel;

typedef struct {
    XBOOL active;
    uint64_t startTickFP;
    unsigned char note;
    unsigned char velocity;
    unsigned char program;
    int bendOffsetCents;
} ActiveNote;

typedef struct {
    uint8_t retrigInterval;     /* retrigger every N frames (0 = inactive) */
    uint8_t noteDelayFrames;    /* delay note-on by N frames (0 = no delay) */
    XBOOL   hasDelayedNote;     /* a note is pending for this row */
    unsigned char delayedEvNote;/* the note value to trigger after delay */
    int     delayedSid;         /* sample ID for the delayed note */
    uint8_t delayedVolume;      /* volume at time of row start */
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
    XBOOL used;
} ChannelProfile;

/* Mapping from tracker channels (0..63) to MIDI channels (0..15). */
typedef struct {
    uint8_t trackerToMidi[MOD2RMF_MAX_CHANNELS];
    XBOOL midiChannelUsed[MOD2RMF_MAX_MIDI_CHANNELS];
} ChannelMap;

typedef struct {
    void *sourceData;
    size_t sourceSize;
    ModRawSample *rawSamples;
    uint32_t rawSampleCount;
    uint32_t moduleBaseRateHz;
    XBOOL isMod;

    BAERmfEditorDocument *document;
    uint16_t *channelToTrackIndex;
    ChannelMap channelMap;
    Mod2RmfResamplerSettings resamplerSettings;
    XBOOL forceOriginalSamples;
    uint8_t stereoSeparation;  /* 0=mono (center), 75=default, 100=hard L/R */
} Mod2RmfConverter;

static XBOOL libxmp_is_mod_family(const char *type)
{
    if (!type || !type[0])
    {
        return FALSE;
    }

    if (strstr(type, "MOD") ||
        strstr(type, "ProTracker") ||
        strstr(type, "NoiseTracker") ||
        strstr(type, "Startrekker") ||
        strstr(type, "Fast Tracker"))
    {
        return TRUE;
    }

    return FALSE;
}

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s [options] <source module> <dest.rmf|dest.zmf>\n"
            "\n"
            "Options:\n"
            "  --codec N|NAME        Set sample compression (number or name, default: 0/pcm)\n"
            "  --bitrate N           Set bitrate in kbps for lossy codecs\n"
            "  --original            Preserve original 8-bit sample data (forces PCM, ignores --codec/--bitrate)\n"
            "  --codecs              List available codecs and bitrates\n"
            "  --amiga-filter NAME   Amiga hardware LPF sim: none|a500|a1200 (default: none)\n"
            "  --resample-rate HZ    Upsample/downsample samples to HZ (0=native, default: 0)\n"
            "  --resample-filter N   Interpolation: nearest|linear|cubic|sinc (default: sinc)\n"
            "  --filters             List available filter/resample options\n"
            "  --stereo-separation N Stereo width 0-100%% (0=mono, 75=default, 100=hard L/R)\n"
            "  --spread              [Experimental] Spread instruments across MIDI channels\n"
            "  --tempomap            Reserved for future tempo-map handling\n"
            "  --help, -h            Show this help\n",
            program_name);
}

static int file_exists(const char *path)
{
    FILE *f;
    f = fopen(path, "rb");
    if (!f)
    {
        return 0;
    }
    fclose(f);
    return 1;
}


static void song_model_init(ModSongModel *song)
{
    if (song)
    {
        memset(song, 0, sizeof(*song));
        song->pitchBendRangeSemitones = MOD2RMF_PITCH_BEND_RANGE_ST;
    }
}

static void song_model_dispose(ModSongModel *song)
{
    if (!song)
    {
        return;
    }
    free(song->playables);
    song->playables = NULL;
    free(song->notes);
    song->notes = NULL;
    free(song->ccEvents);
    song->ccEvents = NULL;
    free(song->pitchBendEvents);
    song->pitchBendEvents = NULL;
    free(song->tempoChanges);
    song->tempoChanges = NULL;
    song->playableCount = 0;
    song->noteCount = 0;
    song->noteCapacity = 0;
    song->ccCount = 0;
    song->ccCapacity = 0;
    song->pitchBendCount = 0;
    song->pitchBendCapacity = 0;
    song->tempoChangeCount = 0;
    song->tempoChangeCapacity = 0;
}

static int is_ascii_space(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v');
}

static void trim_copy_ascii(char *dst, size_t dstSize, const char *src)
{
    size_t start;
    size_t end;
    size_t i;
    size_t out;

    if (!dst || dstSize == 0)
    {
        return;
    }
    dst[0] = '\0';
    if (!src)
    {
        return;
    }

    start = 0;
    while (src[start] && is_ascii_space(src[start]))
    {
        start++;
    }

    end = start;
    while (src[end])
    {
        end++;
    }
    while (end > start && is_ascii_space(src[end - 1]))
    {
        end--;
    }

    out = 0;
    for (i = start; i < end && out + 1 < dstSize; ++i)
    {
        unsigned char ch;
        ch = (unsigned char)src[i];
        if (ch < 32u)
        {
            continue;
        }
        dst[out++] = (char)ch;
    }
    dst[out] = '\0';
}

static void append_linef(char *dst, size_t dstSize, const char *text)
{
    size_t len;

    if (!dst || !text || !text[0] || dstSize == 0)
    {
        return;
    }

    len = strlen(dst);
    if (len >= dstSize - 1)
    {
        return;
    }

    (void)snprintf(dst + len,
                   dstSize - len,
                   "%s\n",
                   text);
}

static const char *path_basename_ptr(const char *path)
{
    const char *p;
    const char *last;

    if (!path)
    {
        return "";
    }
    last = path;
    for (p = path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            last = p + 1;
        }
    }
    return last;
}

static int song_model_append_note(ModSongModel *song,
                                  uint16_t sourceChannel,
                                  uint32_t startTick,
                                  uint32_t durationTicks,
                                  unsigned char note,
                                  unsigned char velocity,
                                  unsigned char program)
{
    ModNoteEvent *newNotes;
    uint32_t newCapacity;

    if (!song)
    {
        return 0;
    }
    if (song->noteCount >= song->noteCapacity)
    {
        newCapacity = (song->noteCapacity == 0) ? 1024U : (song->noteCapacity * 2U);
        newNotes = (ModNoteEvent *)realloc(song->notes, newCapacity * sizeof(ModNoteEvent));
        if (!newNotes)
        {
            return 0;
        }
        song->notes = newNotes;
        song->noteCapacity = newCapacity;
    }

    song->notes[song->noteCount].sourceChannel = sourceChannel;
    song->notes[song->noteCount].startTick = startTick;
    song->notes[song->noteCount].durationTicks = durationTicks;
    song->notes[song->noteCount].note = note;
    song->notes[song->noteCount].velocity = velocity;
    song->notes[song->noteCount].program = program;
    song->noteCount++;
    return 1;
}

static int song_model_append_cc_event(ModSongModel *song,
                                      uint16_t sourceChannel,
                                      uint32_t tick,
                                      unsigned char cc,
                                      unsigned char value,
                                      unsigned char program)
{
    ModCCEvent *newEvents;
    uint32_t newCapacity;

    if (!song)
    {
        return 0;
    }

    if (song->ccCount > 0)
    {
        ModCCEvent *last;
        last = &song->ccEvents[song->ccCount - 1];
        if (last->sourceChannel == sourceChannel && last->cc == cc && last->value == value)
        {
            return 1;
        }
    }

    if (song->ccCount >= song->ccCapacity)
    {
        newCapacity = (song->ccCapacity == 0) ? 1024U : (song->ccCapacity * 2U);
        newEvents = (ModCCEvent *)realloc(song->ccEvents, newCapacity * sizeof(ModCCEvent));
        if (!newEvents)
        {
            return 0;
        }
        song->ccEvents = newEvents;
        song->ccCapacity = newCapacity;
    }

    song->ccEvents[song->ccCount].sourceChannel = sourceChannel;
    song->ccEvents[song->ccCount].tick = tick;
    song->ccEvents[song->ccCount].cc = cc;
    song->ccEvents[song->ccCount].value = value;
    song->ccEvents[song->ccCount].program = program;
    song->ccCount++;
    return 1;
}

static int song_model_append_pitch_bend(ModSongModel *song,
                                        uint16_t sourceChannel,
                                        uint32_t tick,
                                        uint16_t value,
                                        unsigned char program)
{
    ModPitchBendEvent *newEvents;
    uint32_t newCapacity;

    if (!song)
    {
        return 0;
    }

    if (song->pitchBendCount >= song->pitchBendCapacity)
    {
        newCapacity = (song->pitchBendCapacity == 0) ? 1024U : (song->pitchBendCapacity * 2U);
        newEvents = (ModPitchBendEvent *)realloc(song->pitchBendEvents, newCapacity * sizeof(ModPitchBendEvent));
        if (!newEvents)
        {
            return 0;
        }
        song->pitchBendEvents = newEvents;
        song->pitchBendCapacity = newCapacity;
    }

    song->pitchBendEvents[song->pitchBendCount].sourceChannel = sourceChannel;
    song->pitchBendEvents[song->pitchBendCount].tick = tick;
    song->pitchBendEvents[song->pitchBendCount].value = value;
    song->pitchBendEvents[song->pitchBendCount].program = program;
    song->pitchBendCount++;
    return 1;
}

static int song_model_append_tempo_change(ModSongModel *song,
                                          uint32_t tick,
                                          uint32_t bpm)
{
    ModTempoChange *newChanges;
    uint32_t newCapacity;

    if (!song)
    {
        return 0;
    }

    if (song->tempoChangeCount >= song->tempoChangeCapacity)
    {
        newCapacity = (song->tempoChangeCapacity == 0) ? 64U : (song->tempoChangeCapacity * 2U);
        newChanges = (ModTempoChange *)realloc(song->tempoChanges, newCapacity * sizeof(ModTempoChange));
        if (!newChanges)
        {
            return 0;
        }
        song->tempoChanges = newChanges;
        song->tempoChangeCapacity = newCapacity;
    }

    song->tempoChanges[song->tempoChangeCount].tick = tick;
    song->tempoChanges[song->tempoChangeCount].bpm = bpm;
    song->tempoChangeCount++;
    return 1;
}

static uint32_t fp_ticks_to_int(uint64_t fp)
{
    return (uint32_t)((fp + 0x8000u) >> 16);
}

static int flush_active_note(ModSongModel *song,
                             uint16_t sourceChannel,
                             ActiveNote *note,
                             uint64_t endTickFP)
{
    uint32_t startTick;
    uint32_t endTick;
    uint32_t duration;

    if (!note || !note->active)
    {
        return 1;
    }

    startTick = fp_ticks_to_int(note->startTickFP);
    endTick = fp_ticks_to_int(endTickFP);
    duration = (endTick > startTick) ? (endTick - startTick) : 1u;

    if (!song_model_append_note(song,
                                sourceChannel,
                                startTick,
                                duration,
                                note->note,
                                note->velocity,
                                note->program))
    {
        return 0;
    }

    note->active = FALSE;
    return 1;
}

static Mod2RmfConverter *converter_create(void)
{
    Mod2RmfConverter *conv;
    conv = (Mod2RmfConverter *)malloc(sizeof(Mod2RmfConverter));
    if (conv)
    {
        memset(conv, 0, sizeof(*conv));
        conv->moduleBaseRateHz = MOD2RMF_SAMPLE_RATE;
        conv->isMod = FALSE;
    }
    return conv;
}

static void converter_delete(Mod2RmfConverter *conv)
{
    uint32_t i;
    if (!conv)
    {
        return;
    }

    if (conv->document)
    {
        BAERmfEditorDocument_Delete(conv->document);
    }
    free(conv->sourceData);
    free(conv->channelToTrackIndex);

    if (conv->rawSamples)
    {
        for (i = 0; i < conv->rawSampleCount; ++i)
        {
            free(conv->rawSamples[i].pcm8);
        }
        free(conv->rawSamples);
    }

    free(conv);
}


static unsigned char mod_vol_to_midi(unsigned char vol64)
{
    double linear;
    int midi;
    /* MOD volume 0..64 is linear amplitude.
     * MIDI volume (CC 7) is usually squared by the engine.
     * So we need to apply a square root curve to the MOD volume. */
    if (vol64 >= 64u) return 127u;
    if (vol64 == 0u) return 0u;
    
    linear = (double)vol64 / 64.0;
    midi = (int)(sqrt(linear) * 127.0 + 0.5);
    if (midi > 127) midi = 127;
    if (midi < 0) midi = 0;
    return (unsigned char)midi;
}

static unsigned char note_velocity_from_volume(unsigned char vol64)
{
    (void)vol64;
    return 127u;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Apply stereo separation to a raw libxmp panning value (0..255).
 * For MOD-family formats, uses the Amiga LRRL hard-panning pattern
 * scaled by the separation percentage.  For other formats, scales the
 * native panning distance from center by the separation percentage.
 * Returns an adjusted 0..255 panning value. */
static uint8_t apply_stereo_separation(uint8_t rawPan, uint32_t ch,
                                       XBOOL isMod, uint8_t stereoSep)
{
    if (stereoSep == 0)
    {
        return 128u; /* mono: center */
    }

    if (isMod)
    {
        /* Amiga L-R-R-L hard-panning pattern */
        static const int amigaSide[4] = { -1, 1, 1, -1 };
        int offset = (128 * (int)stereoSep) / 100;
        int pan = 128 + amigaSide[ch % 4u] * offset;
        return (uint8_t)clamp_int(pan, 0, 255);
    }

    /* Non-MOD: scale native panning around center */
    {
        int centered = (int)rawPan - 128;
        int scaled = (centered * (int)stereoSep) / 100;
        return (uint8_t)clamp_int(128 + scaled, 0, 255);
    }
}

static uint16_t libxmp_pitchbend_to_midi(int32_t xmpPitchbend,
                                         uint16_t bendRangeSemitones)
{
    double semitoneDelta;
    int32_t bend;

    if (bendRangeSemitones == 0)
    {
        bendRangeSemitones = MOD2RMF_PITCH_BEND_RANGE_ST;
    }

    /* libxmp pitchbend units are in cents (100 = 1 semitone). */
    semitoneDelta = (double)xmpPitchbend / 100.0;
    bend = (int32_t)(MOD2RMF_PITCH_BEND_CENTER +
                     (semitoneDelta / (double)bendRangeSemitones) *
                     (double)MOD2RMF_PITCH_BEND_CENTER);
    if (bend < 0) bend = 0;
    if (bend > 0x3FFF) bend = 0x3FFF;
    return (uint16_t)bend;
}

/* Map a libxmp instrument's amplitude envelope directly to BAE ADSR stages.
 * Each pair of consecutive envelope points becomes one LINEAR_RAMP stage,
 * preserving the multi-segment shape.  BAE supports up to 8 stages; with
 * sustain + terminate that leaves room for up to 6 envelope segments (7 points).
 *
 * Envelope x-coordinates are in ticks (one per tracker frame), converted
 * to microseconds via (2500/bpm)*1000.  y-coordinates are 0..64, scaled to
 * 0..VOLUME_RANGE for BAE. */
static void extract_envelope_adsr(const struct xmp_instrument *inst,
                                  uint32_t bpm,
                                  ModRawSample *raw)
{
    const struct xmp_envelope *aei;
    int sustainIdx;
    int npt;
    int idx;
    double usPerTick; /* microseconds per envelope tick */
    uint32_t stage;
    /* Max segments before sustain+terminate = 8-2 = 6, needing up to 7 points */
    int maxSegments;

    if (!inst || !raw)
    {
        return;
    }

    aei = &inst->aei;
    if (!(aei->flg & XMP_ENVELOPE_ON) || aei->npt < 2)
    {
        return;
    }

    npt = aei->npt;
    if (npt > XMP_MAX_ENV_POINTS)
    {
        npt = XMP_MAX_ENV_POINTS;
    }

    raw->hasEnvelope = TRUE;
    usPerTick = 2500000.0 / (double)(bpm > 0 ? bpm : 125);

    /* Find sustain point (or last point if no sustain flag). */
    sustainIdx = (aei->flg & XMP_ENVELOPE_SUS) ? aei->sus : npt - 1;
    if (sustainIdx < 0)
    {
        sustainIdx = 0;
    }
    if (sustainIdx >= npt)
    {
        sustainIdx = npt - 1;
    }

    #ifdef _DEBUG
    {
        int dbgIdx;
        fprintf(stderr, "[mod2rmf]  ADSR extract: npt=%d flg=0x%02x sus=%d rls=%d usPerTick=%.1f\n",
                npt, aei->flg, aei->sus, inst->rls, usPerTick);
        for (dbgIdx = 0; dbgIdx < npt; ++dbgIdx)
        {
            fprintf(stderr, "    pt[%d]: X=%d Y=%d%s\n",
                    dbgIdx, aei->data[dbgIdx * 2], aei->data[dbgIdx * 2 + 1],
                    (dbgIdx == sustainIdx) ? " <sustain>" : "");
        }
    }
    #endif

    /* Map each envelope segment (pair of consecutive points) up to and including
     * the sustain point as a LINEAR_RAMP stage.  Reserve 2 stages for
     * sustain-hold and terminate. */
    maxSegments = MOD2RMF_MAX_ADSR_STAGES - 2;
    stage = 0;

    /* If the first envelope point is not at Y=0 and X=0, insert an initial
     * ramp to the first point's level (the envelope starts at pt[0] level). */
    for (idx = 0; idx <= sustainIdx && (int)stage < maxSegments; ++idx)
    {
        uint32_t ptX = (uint32_t)aei->data[idx * 2];
        uint32_t ptY = (uint32_t)aei->data[idx * 2 + 1];
        uint32_t prevX = (idx > 0) ? (uint32_t)aei->data[(idx - 1) * 2] : 0;
        uint32_t deltaX = ptX - prevX;

        raw->adsrStages[stage].level = (int32_t)(ptY * VOLUME_RANGE / 64u);
        raw->adsrStages[stage].timeUs = (int32_t)(deltaX * usPerTick + 0.5);
        raw->adsrStages[stage].flags = ADSR_LINEAR_RAMP_LONG;
        stage++;
    }

    /* Sustain-hold stage at the sustain point's level */
    {
        uint32_t susY = (uint32_t)aei->data[sustainIdx * 2 + 1];
        raw->adsrStages[stage].level = (int32_t)(susY * VOLUME_RANGE / 64u);
        raw->adsrStages[stage].timeUs = 0;
        raw->adsrStages[stage].flags = ADSR_SUSTAIN_LONG;
        stage++;
    }

    /* Terminate/release stage */
    {
        uint32_t susY = (uint32_t)aei->data[sustainIdx * 2 + 1];
        uint32_t releaseUs = 50000; /* 50ms default */

        if (inst->rls > 0)
        {
            /* rls is fadeout per tick (XM: 0..65535).  Time to fade from
             * sustain amplitude to silence = susY / rls * 1024 ticks. */
            double releaseTicks = (susY > 0)
                                    ? ((double)susY * 1024.0 / (double)inst->rls)
                                    : 1.0;
            releaseUs = (uint32_t)(releaseTicks * usPerTick + 0.5);
        }
        else if (sustainIdx + 1 < npt)
        {
            uint32_t lastX = (uint32_t)aei->data[(npt - 1) * 2];
            uint32_t susX = (uint32_t)aei->data[sustainIdx * 2];
            releaseUs = (lastX > susX)
                          ? (uint32_t)((lastX - susX) * usPerTick + 0.5)
                          : 50000;
        }

        if (releaseUs > 10000000u) /* 10 second cap */
        {
            releaseUs = 10000000u;
        }

        raw->adsrStages[stage].level = 0;
        raw->adsrStages[stage].timeUs = (int32_t)releaseUs;
        raw->adsrStages[stage].flags = ADSR_TERMINATE_LONG;
        stage++;
    }

    raw->adsrStageCount = stage;

    #ifdef _DEBUG
    {
        uint32_t s;
        fprintf(stderr, "[mod2rmf]  ADSR result: %u stages\n", stage);
        for (s = 0; s < stage; ++s)
        {
            fprintf(stderr, "    stage[%u]: level=%d time=%dus flags=0x%08x\n",
                    s, raw->adsrStages[s].level, raw->adsrStages[s].timeUs,
                    raw->adsrStages[s].flags);
        }
    }
    #endif
}

/* Emulate a bidirectional (ping-pong) sample loop by appending a reversed
 * copy of the loop body (minus the two endpoints) after loopEnd.  The
 * resulting sample uses a standard forward loop that is (2*loopLen - 2)
 * frames long, producing the same audible oscillation as a true bidi loop
 * without a click at the turnaround point. */
static void emulate_bidi_loop(ModRawSample *raw)
{
    uint32_t loopLen;
    uint32_t reversedLen;
    uint32_t newFrameCount;
    int8_t  *newPcm;
    uint32_t dst;
    uint32_t src;

    if (!raw || !raw->pcm8 || raw->loopEnd <= raw->loopStart)
    {
        return;
    }

    loopLen = raw->loopEnd - raw->loopStart;
    if (loopLen < 3)
    {
        /* Bidi with fewer than 3 frames is identical to forward loop. */
        raw->loopType = MOD2RMF_LOOP_FORWARD;
        return;
    }

    reversedLen = loopLen - 2; /* skip both endpoints to avoid doubling */
    newFrameCount = raw->loopEnd + reversedLen;

    newPcm = (int8_t *)malloc(newFrameCount);
    if (!newPcm)
    {
        return; /* leave sample unchanged on OOM */
    }

    /* Copy everything up to loopEnd. */
    memcpy(newPcm, raw->pcm8, raw->loopEnd);

    /* Append reversed loop body, skipping the endpoint samples. */
    dst = raw->loopEnd;
    for (src = raw->loopEnd - 2; src > raw->loopStart; --src)
    {
        newPcm[dst++] = raw->pcm8[src];
    }

    free(raw->pcm8);
    raw->pcm8 = newPcm;
    raw->frameCount = newFrameCount;
    raw->loopEnd = raw->loopStart + loopLen + reversedLen; /* = loopStart + 2*loopLen - 2 */
    raw->loopType = MOD2RMF_LOOP_FORWARD;
}

/* Emulate a reverse (backwards-only) sample loop by reversing the samples
 * in the loop region in-place.  After this, a standard forward loop will
 * play the region in the originally backward direction. */
static void emulate_reverse_loop(ModRawSample *raw)
{
    uint32_t lo;
    uint32_t hi;
    int8_t   tmp;

    if (!raw || !raw->pcm8 || raw->loopEnd <= raw->loopStart)
    {
        return;
    }

    lo = raw->loopStart;
    hi = raw->loopEnd - 1;
    while (lo < hi)
    {
        tmp = raw->pcm8[lo];
        raw->pcm8[lo] = raw->pcm8[hi];
        raw->pcm8[hi] = tmp;
        lo++;
        hi--;
    }

    raw->loopType = MOD2RMF_LOOP_FORWARD;
}

/* Parse a row's primary and secondary effect columns for retrigger and
 * note-delay commands.  Sets *outRetrigInterval and *outDelayFrames
 * (either may remain unchanged if the effect is not present). */
static void parse_row_effects(const struct xmp_event *ev,
                              uint8_t *outRetrigInterval,
                              uint8_t *outDelayFrames)
{
    int col;

    if (!ev || !outRetrigInterval || !outDelayFrames)
    {
        return;
    }

    /* Check both primary (fxt/fxp) and secondary (f2t/f2p) columns. */
    for (col = 0; col < 2; ++col)
    {
        uint8_t fxType = (col == 0) ? ev->fxt : ev->f2t;
        uint8_t fxParam = (col == 0) ? ev->fxp : ev->f2p;

        if (fxType == MOD2RMF_FX_EXTENDED)
        {
            uint8_t subCmd = (fxParam >> 4) & 0x0F;
            uint8_t subVal = fxParam & 0x0F;

            if (subCmd == MOD2RMF_EX_RETRIG && subVal > 0)
            {
                *outRetrigInterval = subVal;
            }
            else if (subCmd == MOD2RMF_EX_DELAY && subVal > 0)
            {
                *outDelayFrames = subVal;
            }
        }
        else if (fxType == MOD2RMF_FX_MULTI_RETRIG)
        {
            uint8_t interval = fxParam & 0x0F;
            if (interval > 0)
            {
                *outRetrigInterval = interval;
            }
        }
    }
}

/* Return TRUE if tone portamento (effect 3xx or 5xx) is active in either
 * effect column of this row.  When tone portamento is active the note in
 * the pattern is the slide TARGET; the sample must NOT be retriggered. */
static XBOOL row_has_tone_portamento(const struct xmp_event *ev)
{
    if (!ev)
    {
        return FALSE;
    }
    if (ev->fxt == MOD2RMF_FX_TONEPORTA || ev->fxt == MOD2RMF_FX_TONE_VSLIDE)
    {
        return TRUE;
    }
    if (ev->f2t == MOD2RMF_FX_TONEPORTA || ev->f2t == MOD2RMF_FX_TONE_VSLIDE)
    {
        return TRUE;
    }
    return FALSE;
}

static int build_song_model(Mod2RmfConverter *conv, ModSongModel *song)
{
    xmp_context ctx;
    struct xmp_module_info mi;
    struct xmp_module *mod;
    struct xmp_frame_info fi;
    ActiveNote activeNotes[MOD2RMF_MAX_CHANNELS];
    ChannelEffectState chEffects[MOD2RMF_MAX_CHANNELS];
    uint8_t chLastVol[MOD2RMF_MAX_CHANNELS];
    uint8_t chLastPan[MOD2RMF_MAX_CHANNELS];
    uint16_t chLastBend[MOD2RMF_MAX_CHANNELS];
    int16_t sampleTranspose[MOD2RMF_MAX_SAMPLES];  /* sub-instrument xpo per sample */
    XBOOL sampleHasEnvelope[MOD2RMF_MAX_SAMPLES];   /* instrument has amplitude envelope */
    uint64_t positionTickFP[XMP_MAX_MOD_LENGTH]; /* MIDI tick at start of each order position */
    int positionSeen[XMP_MAX_MOD_LENGTH];        /* whether we've recorded the tick for this pos */
    int lastPos;                                 /* last seen order position */
    int *sampleToProgram;
    uint64_t currentTickFP;
    uint64_t tickPerFrameFP;
    uint32_t frameGuard;
    uint16_t lastBpm;
    int playerStarted;
    uint32_t i;

    if (!conv || !song || !conv->sourceData || conv->sourceSize == 0)
    {
        return 0;
    }

    ctx = xmp_create_context();
    if (!ctx)
    {
        return 0;
    }
    if (xmp_load_module_from_memory(ctx, conv->sourceData, (long)conv->sourceSize) != 0)
    {
        xmp_free_context(ctx);
        return 0;
    }

    memset(&mi, 0, sizeof(mi));
    xmp_get_module_info(ctx, &mi);
    mod = mi.mod;
    if (!mod)
    {
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return 0;
    }

    trim_copy_ascii(song->moduleName, sizeof(song->moduleName), mod->name);
    if (mi.comment)
    {
        trim_copy_ascii(song->composerNotes, sizeof(song->composerNotes), mi.comment);
    }

     conv->isMod = libxmp_is_mod_family(mod->type);
     conv->moduleBaseRateHz = conv->isMod ? MOD2RMF_SAMPLE_RATE : 8363u;

    song->channelCount = (mod->chn > MOD2RMF_MAX_CHANNELS) ? MOD2RMF_MAX_CHANNELS : (uint32_t)mod->chn;
    lastBpm = (mod->bpm > 0) ? (uint16_t)mod->bpm : 125u;
    song->bpm = lastBpm;
    song->pitchBendRangeSemitones = MOD2RMF_PITCH_BEND_RANGE_ST;
    (void)song_model_append_tempo_change(song, 0, lastBpm);

    free(conv->rawSamples);
    conv->rawSamples = NULL;
    conv->rawSampleCount = (mod->smp > 0) ? (uint32_t)mod->smp : 0u;
    if (conv->rawSampleCount > 0)
    {
        conv->rawSamples = (ModRawSample *)calloc(conv->rawSampleCount, sizeof(ModRawSample));
        if (!conv->rawSamples)
        {
            xmp_release_module(ctx);
            xmp_free_context(ctx);
            return 0;
        }
    }

    /* Capture per-sample instrument transpose (sub->xpo) so we can
     * reconstruct the transposed note for MIDI output.  libxmp's public
     * ci->note (= xc->key) is the raw pattern key WITHOUT transpose,
     * while ci->pitchbend is relative to the TRANSPOSED note. */
    memset(sampleTranspose, 0, sizeof(sampleTranspose));
    memset(sampleHasEnvelope, 0, sizeof(sampleHasEnvelope));
    if (mod->ins > 0 && mod->xxi)
    {
        for (i = 0; i < (uint32_t)mod->ins; ++i)
        {
            const struct xmp_instrument *inst;
            int sub;

            inst = &mod->xxi[i];
            if (inst->nsm <= 0 || !inst->sub)
            {
                continue;
            }

            for (sub = 0; sub < inst->nsm; ++sub)
            {
                int sid;
                sid = inst->sub[sub].sid;
                if (sid < 0 || sid >= (int)conv->rawSampleCount || sid >= MOD2RMF_MAX_SAMPLES)
                {
                    continue;
                }
                if (sampleTranspose[sid] == 0)
                {
                    sampleTranspose[sid] = (int16_t)inst->sub[sub].xpo;
                }
            }
        }
    }

    for (i = 0; i < conv->rawSampleCount; ++i)
    {
        const struct xmp_sample *s;
        ModRawSample *raw;
        int len;

        s = &mod->xxs[i];
        raw = &conv->rawSamples[i];
        memset(raw, 0, sizeof(*raw));
        trim_copy_ascii(raw->name, sizeof(raw->name), s->name);
        raw->rootKey = 72;
        raw->defaultVolume = 64;
        raw->defaultPan = -1; /* unset; will be filled from sub-instrument */
        raw->finetune = 0;

        len = s->len;
        if (len <= 0 || !s->data || (s->flg & XMP_SAMPLE_SYNTH))
        {
            continue;
        }

        if (s->flg & XMP_SAMPLE_16BIT)
        {
            const int16_t *src16;
            uint32_t outFrames;
            uint32_t f;

            outFrames = (uint32_t)len;
            raw->pcm8 = (int8_t *)malloc(outFrames);
            if (!raw->pcm8)
            {
                xmp_release_module(ctx);
                xmp_free_context(ctx);
                return 0;
            }
            src16 = (const int16_t *)s->data;
            for (f = 0; f < outFrames; ++f)
            {
                int v;
                v = (int)src16[f] >> 8;
                v += 128;
                raw->pcm8[f] = (int8_t)clamp_int(v, 0, 255);
            }
            raw->frameCount = outFrames;
        }
        else
        {
            const uint8_t *src8;
            uint32_t outFrames;
            uint32_t f;

            outFrames = (uint32_t)len;
            raw->pcm8 = (int8_t *)malloc(outFrames);
            if (!raw->pcm8)
            {
                xmp_release_module(ctx);
                xmp_free_context(ctx);
                return 0;
            }
            src8 = (const uint8_t *)s->data;
            for (f = 0; f < outFrames; ++f)
            {
                raw->pcm8[f] = (int8_t)(src8[f] ^ 0x80u);
            }
            raw->frameCount = outFrames;
        }

        raw->loopStart = 0;
        raw->loopEnd = 0;

        /* Only set loop points if libxmp reports the sample as looping.
         * Many tracker formats store lps=0 lpe=2 as a "no loop" sentinel;
         * without the flag check those would produce a tiny invalid loop. */
        if (s->flg & XMP_SAMPLE_LOOP)
        {
            raw->loopStart = (s->lps > 0) ? (uint32_t)s->lps : 0u;
            raw->loopEnd = (s->lpe > s->lps) ? (uint32_t)s->lpe : 0u;
            if (raw->loopStart > raw->frameCount) raw->loopStart = 0;
            if (raw->loopEnd > raw->frameCount) raw->loopEnd = raw->frameCount;
            /* Discard degenerate loops (< 3 frames) */
            if (raw->loopEnd - raw->loopStart < 3)
            {
                raw->loopStart = 0;
                raw->loopEnd = 0;
            }

            /* Detect loop type from libxmp flags (only meaningful with a valid loop). */
            if (raw->loopEnd > raw->loopStart)
            {
                if (s->flg & XMP_SAMPLE_LOOP_BIDIR)
                {
                    raw->loopType = MOD2RMF_LOOP_BIDIR;
                }
                else if (s->flg & XMP_SAMPLE_LOOP_REVERSE)
                {
                    raw->loopType = MOD2RMF_LOOP_REVERSE;
                }
            }
        }

        raw->valid = TRUE;

        /* Emulate non-forward loop types by transforming the PCM data
         * so the BAE engine's forward-only loop player can reproduce
         * the correct audible result. */
        if (raw->loopType == MOD2RMF_LOOP_BIDIR)
        {
            emulate_bidi_loop(raw);
        }
        else if (raw->loopType == MOD2RMF_LOOP_REVERSE)
        {
            emulate_reverse_loop(raw);
        }
    }

    /* Extract amplitude envelope ADSR for each sample from the first
     * instrument that references it (same "first wins" policy as
     * sampleTranspose).  This runs AFTER the sample extraction loop
     * above so the memset() on each raw sample doesn't clobber the
     * envelope data. */
    if (mod->ins > 0 && mod->xxi)
    {
        for (i = 0; i < (uint32_t)mod->ins; ++i)
        {
            struct xmp_instrument *inst;
            int sub;

            inst = &mod->xxi[i];
            if (inst->nsm <= 0 || !inst->sub)
            {
                continue;
            }

            for (sub = 0; sub < inst->nsm; ++sub)
            {
                int sid;
                sid = inst->sub[sub].sid;
                if (sid < 0 || sid >= (int)conv->rawSampleCount || sid >= MOD2RMF_MAX_SAMPLES)
                {
                    continue;
                }
                if (!sampleHasEnvelope[sid] &&
                    (inst->aei.flg & XMP_ENVELOPE_ON) && inst->aei.npt >= 2)
                {
                    extract_envelope_adsr(inst, (uint32_t)(mod->bpm > 0 ? mod->bpm : 125),
                                          &conv->rawSamples[sid]);
                    sampleHasEnvelope[sid] = conv->rawSamples[sid].hasEnvelope;
                }
                /* Capture default pan from sub-instrument (first assignment wins) */
                if (conv->rawSamples[sid].defaultPan < 0)
                {
                    conv->rawSamples[sid].defaultPan = (int16_t)inst->sub[sub].pan;
                }
            }

            /* Now that we have captured the ADSR parameters, disable the
             * amplitude envelope so libxmp no longer folds it into
             * ci->volume.  This lets us emit CC7 from the raw channel
             * volume (including volume slides) while BAE handles the
             * envelope shape through its own ADSR engine. */
            if (inst->aei.flg & XMP_ENVELOPE_ON)
            {
                inst->aei.flg &= ~XMP_ENVELOPE_ON;
            }
        }
    }

    sampleToProgram = NULL;
    if (conv->rawSampleCount > 0)
    {
        sampleToProgram = (int *)malloc(conv->rawSampleCount * sizeof(int));
        if (!sampleToProgram)
        {
            xmp_release_module(ctx);
            xmp_free_context(ctx);
            return 0;
        }
        for (i = 0; i < conv->rawSampleCount; ++i)
        {
            sampleToProgram[i] = -1;
        }
    }

    memset(activeNotes, 0, sizeof(activeNotes));
    memset(chEffects, 0, sizeof(chEffects));
    for (i = 0; i < MOD2RMF_MAX_CHANNELS; ++i)
    {
        chLastVol[i] = 0xFF;
        chLastPan[i] = 0xFF;
        chLastBend[i] = MOD2RMF_PITCH_BEND_CENTER;
    }

    currentTickFP = 0;
    /* MOD2RMF_ROW_TICKS (120) is defined as MIDI ticks per row at speed 6.
     * Dividing by 6 gives 20 MIDI ticks per tracker frame (tick).
     * Speed changes are handled naturally: fewer frames per row at lower
     * speed means fewer MIDI ticks per row = rows play faster.  The /6
     * constant must NOT be replaced with the current speed. */
    tickPerFrameFP = ((uint64_t)MOD2RMF_ROW_TICKS << 16) / 6u;
    frameGuard = 0;
    playerStarted = 0;
    memset(positionTickFP, 0, sizeof(positionTickFP));
    memset(positionSeen, 0, sizeof(positionSeen));
    lastPos = -1;

    if (xmp_start_player(ctx, 44100, 0) != 0)
    {
        free(sampleToProgram);
        xmp_release_module(ctx);
        xmp_free_context(ctx);
        return 0;
    }
    playerStarted = 1;

    while (frameGuard < 4000000u)
    {
        uint32_t ch;
        int playRc;
        uint32_t tick;

        playRc = xmp_play_frame(ctx);
        if (playRc != 0)
        {
            break;
        }

        xmp_get_frame_info(ctx, &fi);
        tick = fp_ticks_to_int(currentTickFP);

        /* Check for loop BEFORE processing this frame's events.
         * When loop_count > 0 libxmp has already jumped back, so this
         * frame is the first frame of the repeated section.  We must
         * not emit its events a second time (they were already recorded
         * during the first playthrough), and the loop end tick is the
         * current tick (the first tick that would repeat). */
        if (fi.loop_count > 0)
        {
            song->loopEnabled = TRUE;
            /* fi.pos is the position libxmp jumped back to (Bxx target).
             * Use the recorded tick for that position as the loop start. */
            if (fi.pos >= 0 && fi.pos < XMP_MAX_MOD_LENGTH && positionSeen[fi.pos])
            {
                song->loopStartTick = fp_ticks_to_int(positionTickFP[fi.pos]);
            }
            else
            {
                song->loopStartTick = 0;
            }
            song->loopEndTick = tick;
            break;
        }

        /* Track the MIDI tick at the start of each order position so we
         * can map Bxx loop targets back to MIDI ticks. */
        if (fi.pos >= 0 && fi.pos < XMP_MAX_MOD_LENGTH && fi.pos != lastPos)
        {
            lastPos = fi.pos;
            if (!positionSeen[fi.pos])
            {
                positionSeen[fi.pos] = 1;
                positionTickFP[fi.pos] = currentTickFP;
            }
        }

        if (fi.bpm > 0 && (uint16_t)fi.bpm != lastBpm)
        {
            lastBpm = (uint16_t)fi.bpm;
            song->bpm = lastBpm;
            (void)song_model_append_tempo_change(song, tick, lastBpm);
        }

        for (ch = 0; ch < song->channelCount; ++ch)
        {
            const struct xmp_channel_info *ci;
            uint8_t evNote;
            uint8_t sampleNum;
            int sid;
            int program;

            ci = &fi.channel_info[ch];
            evNote = ci->event.note;
            sampleNum = ci->sample;
            sid = (int)sampleNum; /* 0-based index into mod->xxs[] */

            /* Volume, panning, and pitch bend update on every frame
             * so that slides/vibrato/tremolo are captured.
             *
             * For enveloped instruments the BAE ADSR handles the volume
             * shape, so we only emit CC7 at row boundaries (frame 0)
             * where ci->volume reflects the channel volume before the
             * envelope has modulated it.  This avoids a flood of CC7
             * events that just approximate the envelope curve.
             *
             * Snapshot the CC event count so we can fix program tags
             * below if a note-on changes the active program this frame.
             * CC7/CC10 emitted here use the *previous* note's program,
             * but the volume/pan actually belongs to the incoming note. */
            {
                uint32_t preNoteCCIdx = song->ccCount;
                uint32_t preNoteBendIdx = song->pitchBendCount;
                uint8_t  preNoteProg  = activeNotes[ch].program;

                {
                    uint8_t adjPan;
                    adjPan = apply_stereo_separation((uint8_t)ci->pan, ch,
                                                     conv->isMod, conv->stereoSeparation);
                    if (adjPan != chLastPan[ch])
                    {
                        chLastPan[ch] = adjPan;
                        (void)song_model_append_cc_event(song, (uint16_t)ch, tick, 10,
                                                         (unsigned char)(adjPan >> 1),
                                                         activeNotes[ch].program);
                    }
                }
                if (ci->volume != chLastVol[ch])
                {
                    uint8_t vol64;
                    vol64 = (uint8_t)clamp_int((int)ci->volume, 0, 64);
                    chLastVol[ch] = vol64;
                    (void)song_model_append_cc_event(song, (uint16_t)ch, tick, 7,
                                                    mod_vol_to_midi(vol64),
                                                    activeNotes[ch].program);
                }

                if (activeNotes[ch].active)
                {
                    uint16_t bend;
                    bend = libxmp_pitchbend_to_midi(ci->pitchbend + activeNotes[ch].bendOffsetCents,
                                                    song->pitchBendRangeSemitones);
                    if (bend != chLastBend[ch])
                    {
                        chLastBend[ch] = bend;
                        (void)song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                        activeNotes[ch].program);
                    }
                }

                /* ---- Row boundary (frame 0): parse effects, handle note events ---- */
                if (fi.frame == 0)
                {
                    uint8_t retrigInterval = 0;
                    uint8_t noteDelayFrames = 0;

                    /* Reset per-row effect state. */
                    memset(&chEffects[ch], 0, sizeof(chEffects[ch]));

                    /* Parse retrigger and note-delay effects from both effect columns. */
                    parse_row_effects(&ci->event, &retrigInterval, &noteDelayFrames);
                    chEffects[ch].retrigInterval = retrigInterval;
                    chEffects[ch].noteDelayFrames = noteDelayFrames;

                    /* Key-off events are always processed immediately, even with delay. */
                    if (evNote == XMP_KEY_OFF || evNote == XMP_KEY_CUT || evNote == XMP_KEY_FADE)
                    {
                        if (!flush_active_note(song, (uint16_t)ch, &activeNotes[ch], currentTickFP))
                        {
                            if (playerStarted) xmp_end_player(ctx);
                            free(sampleToProgram);
                            xmp_release_module(ctx);
                            xmp_free_context(ctx);
                            return 0;
                        }
                    }
                    else if (evNote > 0 && evNote <= 120 && sid >= 0 && sid < (int)conv->rawSampleCount)
                    {
                        if (conv->rawSamples[sid].valid)
                        {
                            if (noteDelayFrames > 0)
                            {
                                /* Note delay: defer the note-on to a later frame. */
                                chEffects[ch].hasDelayedNote = TRUE;
                                chEffects[ch].delayedEvNote = evNote;
                                chEffects[ch].delayedSid = sid;
                                chEffects[ch].delayedVolume = (uint8_t)clamp_int((int)ci->volume, 0, 64);
                            }
                            else
                            {
                                /* Normal note-on (or retrigger tick 0). */
                                int midiNote;
                                int noteXpo;

                                if (sampleToProgram[sid] < 0)
                                {
                                    if (sid >= 128)
                                    {
                                        if (playerStarted) xmp_end_player(ctx);
                                        free(sampleToProgram);
                                        xmp_release_module(ctx);
                                        xmp_free_context(ctx);
                                        return 0;
                                    }
                                    sampleToProgram[sid] = sid;
                                }

                                program = sampleToProgram[sid];

                                if (!flush_active_note(song, (uint16_t)ch, &activeNotes[ch], currentTickFP))
                                {
                                    if (playerStarted) xmp_end_player(ctx);
                                    free(sampleToProgram);
                                    xmp_release_module(ctx);
                                    xmp_free_context(ctx);
                                    return 0;
                                }

                                noteXpo = (sid >= 0 && sid < MOD2RMF_MAX_SAMPLES)
                                            ? (int)sampleTranspose[sid] : 0;
                                midiNote = (int)ci->note + 12 + noteXpo;

                                activeNotes[ch].active = TRUE;
                                activeNotes[ch].startTickFP = currentTickFP;
                                activeNotes[ch].note = (unsigned char)clamp_int(midiNote, 0, 127);
                                activeNotes[ch].velocity = note_velocity_from_volume((uint8_t)clamp_int((int)ci->volume, 0, 64));
                                activeNotes[ch].program = (uint8_t)program;
                                activeNotes[ch].bendOffsetCents = (midiNote - (int)activeNotes[ch].note) * 100;
                                chLastBend[ch] = 0xFFFFu;
                                {
                                    uint16_t bend;
                                    bend = libxmp_pitchbend_to_midi(ci->pitchbend + activeNotes[ch].bendOffsetCents,
                                                                    song->pitchBendRangeSemitones);
                                    chLastBend[ch] = bend;
                                    (void)song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                                        activeNotes[ch].program);
                                }
                            }
                        }
                    }
                }

                /* ---- Non-zero frames: handle note delay trigger and retrigger ---- */
                if (fi.frame > 0)
                {
                    /* Note delay: trigger the deferred note-on at the correct frame. */
                    if (chEffects[ch].hasDelayedNote &&
                        (uint8_t)fi.frame == chEffects[ch].noteDelayFrames)
                    {
                        int delaySid = chEffects[ch].delayedSid;
                        if (delaySid >= 0 && delaySid < (int)conv->rawSampleCount &&
                            conv->rawSamples[delaySid].valid)
                        {
                            int midiNote;
                            int noteXpo;

                            if (sampleToProgram[delaySid] < 0)
                            {
                                if (delaySid >= 128)
                                {
                                    if (playerStarted) xmp_end_player(ctx);
                                    free(sampleToProgram);
                                    xmp_release_module(ctx);
                                    xmp_free_context(ctx);
                                    return 0;
                                }
                                sampleToProgram[delaySid] = delaySid;
                            }

                            program = sampleToProgram[delaySid];

                            if (!flush_active_note(song, (uint16_t)ch, &activeNotes[ch], currentTickFP))
                            {
                                if (playerStarted) xmp_end_player(ctx);
                                free(sampleToProgram);
                                xmp_release_module(ctx);
                                xmp_free_context(ctx);
                                return 0;
                            }

                            noteXpo = (delaySid >= 0 && delaySid < MOD2RMF_MAX_SAMPLES)
                                        ? (int)sampleTranspose[delaySid] : 0;
                            midiNote = (int)ci->note + 12 + noteXpo;

                            activeNotes[ch].active = TRUE;
                            activeNotes[ch].startTickFP = currentTickFP;
                            activeNotes[ch].note = (unsigned char)clamp_int(midiNote, 0, 127);
                            activeNotes[ch].velocity = note_velocity_from_volume((uint8_t)clamp_int((int)ci->volume, 0, 64));
                            activeNotes[ch].program = (uint8_t)program;
                            activeNotes[ch].bendOffsetCents = (midiNote - (int)activeNotes[ch].note) * 100;
                            chLastBend[ch] = 0xFFFFu;
                            {
                                uint16_t bend;
                                bend = libxmp_pitchbend_to_midi(ci->pitchbend + activeNotes[ch].bendOffsetCents,
                                                                song->pitchBendRangeSemitones);
                                chLastBend[ch] = bend;
                                (void)song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                                    activeNotes[ch].program);
                            }
                        }
                        chEffects[ch].hasDelayedNote = FALSE;
                    }

                    /* Retrigger: fire a new note-on at each interval boundary. */
                    if (chEffects[ch].retrigInterval > 0 &&
                        ((uint8_t)fi.frame % chEffects[ch].retrigInterval) == 0 &&
                        activeNotes[ch].active)
                    {
                        if (!flush_active_note(song, (uint16_t)ch, &activeNotes[ch], currentTickFP))
                        {
                            if (playerStarted) xmp_end_player(ctx);
                            free(sampleToProgram);
                            xmp_release_module(ctx);
                            xmp_free_context(ctx);
                            return 0;
                        }

                        /* Re-trigger the same note with current volume from libxmp
                        * (which already applies Rxy volume table changes). */
                        activeNotes[ch].active = TRUE;
                        activeNotes[ch].startTickFP = currentTickFP;
                        /* note and program stay the same from the row's note-on */
                        activeNotes[ch].velocity = note_velocity_from_volume((uint8_t)clamp_int((int)ci->volume, 0, 64));
                        chLastBend[ch] = 0xFFFFu;
                        {
                            uint16_t bend;
                            bend = libxmp_pitchbend_to_midi(ci->pitchbend + activeNotes[ch].bendOffsetCents,
                                                            song->pitchBendRangeSemitones);
                            chLastBend[ch] = bend;
                            (void)song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend,
                                                                activeNotes[ch].program);
                        }
                    }
                }

                /* If the note-on changed the active program, patch the CC
                 * and pitch bend events we emitted earlier this frame so
                 * they carry the correct program tag for spread-mode
                 * routing. */
                if (activeNotes[ch].program != preNoteProg)
                {
                    uint32_t j;
                    for (j = preNoteCCIdx; j < song->ccCount; ++j)
                    {
                        song->ccEvents[j].program = activeNotes[ch].program;
                    }
                    for (j = preNoteBendIdx; j < song->pitchBendCount; ++j)
                    {
                        song->pitchBendEvents[j].program = activeNotes[ch].program;
                    }
                }
            } /* end preNoteCCIdx scope */
        }

        currentTickFP += tickPerFrameFP;
        frameGuard++;
    }

    if (playerStarted)
    {
        xmp_end_player(ctx);
    }

    for (i = 0; i < song->channelCount; ++i)
    {
        if (!flush_active_note(song, (uint16_t)i, &activeNotes[i], currentTickFP))
        {
            free(sampleToProgram);
            xmp_release_module(ctx);
            xmp_free_context(ctx);
            return 0;
        }
    }

    /* playableCount must cover the highest assigned program number + 1.
     * Since programs now equal sample indices (which may have gaps), find
     * the maximum assigned program. */
    {
        uint32_t maxProg = 0;
        XBOOL anyAssigned = FALSE;
        for (i = 0; i < conv->rawSampleCount; ++i) {
            if (sampleToProgram[i] >= 0) {
                if ((uint32_t)sampleToProgram[i] >= maxProg) {
                    maxProg = (uint32_t)sampleToProgram[i] + 1;
                }
                anyAssigned = TRUE;
            }
        }
        song->playableCount = anyAssigned ? maxProg : 0;
    }
    if (song->playableCount > 0)
    {
        song->playables = (ModPlayable *)calloc(song->playableCount, sizeof(ModPlayable));
        if (!song->playables)
        {
            free(sampleToProgram);
            xmp_release_module(ctx);
            xmp_free_context(ctx);
            return 0;
        }
    }
    for (i = 0; i < conv->rawSampleCount; ++i)
    {
        ModPlayable *p;
        int program;
        char tmp[64];

        program = sampleToProgram ? sampleToProgram[i] : -1;
        if (program < 0 || (uint32_t)program >= song->playableCount)
        {
            continue;
        }
        p = &song->playables[program];
        p->sourceSlot = i;
        p->program = (unsigned char)program;
        p->rootKey = 72;
        p->hasSampleRateOverride = TRUE;
        p->sampleRateOverrideHz = conv->moduleBaseRateHz;
        
        p->sampleOffsetBytes = 0;
        p->offsetVariant = FALSE;
        p->rawSample = &conv->rawSamples[i];

        /* If the instrument has an amplitude envelope, map it to a BAE
         * ADSR so the engine produces smooth volume shaping instead of
         * relying on per-frame CC7 events.  The per-frame CC7 tracking
         * is correspondingly suppressed for enveloped channels. */
        if (conv->rawSamples[i].hasEnvelope && conv->rawSamples[i].adsrStageCount > 0)
        {
            p->hasVolumeAdsr = TRUE;
            p->adsrStageCount = conv->rawSamples[i].adsrStageCount;
            memcpy(p->adsrStages, conv->rawSamples[i].adsrStages,
                   conv->rawSamples[i].adsrStageCount * sizeof(ModAdsrStage));
        }

        /* Map sub-instrument default pan to BAE panPlacement */
        if (conv->rawSamples[i].defaultPan >= 0)
        {
            p->panPlacement = (int8_t)((int)conv->rawSamples[i].defaultPan - 128);
        }

        if (conv->rawSamples[i].name[0])
        {
            snprintf(p->displayName, sizeof(p->displayName), "%s", conv->rawSamples[i].name);
        }
        else
        {
            snprintf(tmp, sizeof(tmp), "Sample %u", (unsigned)(i + 1u));
            trim_copy_ascii(p->displayName, sizeof(p->displayName), tmp);
        }
    }

    free(sampleToProgram);
    xmp_release_module(ctx);
    xmp_free_context(ctx);
    return 1;
}

static int load_source_data(Mod2RmfConverter *conv, const char *sourcePath)
{
    FILE *file;
    size_t fileSize;
    size_t bytesRead;

    if (!conv || !sourcePath)
    {
        return 0;
    }

    file = fopen(sourcePath, "rb");
    if (!file)
    {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return 0;
    }
    {
        long rawSize;
        rawSize = ftell(file);
        if (rawSize < 0)
        {
            fclose(file);
            return 0;
        }
        fileSize = (size_t)rawSize;
    }
    if (fseek(file, 0, SEEK_SET) != 0 || fileSize == 0)
    {
        fclose(file);
        return 0;
    }

    conv->sourceData = malloc(fileSize);
    if (!conv->sourceData)
    {
        fclose(file);
        return 0;
    }
    conv->sourceSize = fileSize;

    bytesRead = fread(conv->sourceData, 1, fileSize, file);
    fclose(file);
    return bytesRead == fileSize;
}

static int setup_document(Mod2RmfConverter *conv,
                          const ModSongModel *song,
                          const char *sourcePath)
{
    BAEResult result;
    BAERmfEditorTrackSetup setup;
    char conductorName[] = "Conductor";

    if (!conv || !song)
    {
        return 0;
    }

    conv->document = BAERmfEditorDocument_New();
    if (!conv->document)
    {
        return 0;
    }

    BAERmfEditorDocument_SetTempoBPM(conv->document, song->bpm);
    BAERmfEditorDocument_AddTempoEvent(conv->document, 0, 60000000UL / song->bpm);
    BAERmfEditorDocument_SetTicksPerQuarter(conv->document, 480);
    if (song->moduleName[0])
    {
        BAERmfEditorDocument_SetInfo(conv->document, TITLE_INFO, song->moduleName);
        BAERmfEditorDocument_SetInfo(conv->document, COPYRIGHT_INFO, "Converted by mod2rmf");
    }
    if (song->composerNotes[0])
    {
        BAERmfEditorDocument_SetInfo(conv->document, COMPOSER_NOTES_INFO, song->composerNotes);
    }
    if (sourcePath && sourcePath[0])
    {
        BAERmfEditorDocument_SetInfo(conv->document,
                                     ORIGINAL_SOURCE_INFO,
                                     path_basename_ptr(sourcePath));
    }

    memset(&setup, 0, sizeof(setup));
    setup.channel = 0;
    setup.bank = 0;
    setup.program = 0;
    setup.name = conductorName;

    result = BAERmfEditorDocument_AddTrack(conv->document, &setup, NULL);
    return result == BAE_NO_ERROR;
}

static int setup_samples(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;
    uint32_t baseAssetBySourceSlot[MOD2RMF_MAX_SAMPLES];

    if (!conv || !song)
    {
        return 0;
    }

    for (i = 0; i < MOD2RMF_MAX_SAMPLES; ++i)
    {
        baseAssetBySourceSlot[i] = 0xFFFFFFFFu;
    }

    for (i = 0; i < song->playableCount; ++i)
    {
        BAERmfEditorSampleSetup setup;
        BAESampleInfo sampleInfo;
        BAEResult result;
        uint32_t sampleIndex;
        uint32_t chosenLoopStart;
        uint32_t chosenLoopEnd;
        uint32_t sourceSlot;
        XBOOL usedSharedAsset;
        const ModPlayable *playable;
        ModRawSample *raw;

        playable = &song->playables[i];
        raw = playable->rawSample;
        sourceSlot = playable->sourceSlot;
        usedSharedAsset = FALSE;

        memset(&setup, 0, sizeof(setup));
        setup.program = playable->program;
        
        /* BAE limits downward pitch shifts to -24 semitones from rootKey.
         * To allow tracker modules to play extremely low notes (e.g. laugh samples),
         * we virtually shift the rootKey down by 2 octaves (24 semitones) and 
         * correspondingly divide the sample rate by 4 prior to saving. */
        setup.rootKey = (playable->rootKey >= 24) ? (playable->rootKey - 24) : 0;
        
        setup.lowKey = 0;
        setup.highKey = 127;
        setup.displayName = (char *)playable->displayName;

        sampleIndex = 0;
        chosenLoopStart = 0;
        chosenLoopEnd = 0;

        result = BAERmfEditorDocument_AddEmptySample(conv->document,
                                                     &setup,
                                                     &sampleIndex,
                                                     &sampleInfo);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "[mod2rmf] Warning: failed to add sample for program %u (%d)\n", (unsigned)setup.program, (int)result);
            continue;
        }

        if (raw && raw->valid && raw->pcm8 && raw->frameCount > 0)
        {
            uint32_t pcmFrames;
            uint32_t loopStart;
            uint32_t loopEnd;

            pcmFrames = raw->frameCount;
            loopStart = 0;
            loopEnd = 0;

            if (raw->loopEnd > raw->loopStart)
            {
                loopStart = raw->loopStart;
                loopEnd = raw->loopEnd;
            }

            if (playable->offsetVariant && sourceSlot < MOD2RMF_MAX_SAMPLES &&
                baseAssetBySourceSlot[sourceSlot] != 0xFFFFFFFFu)
            {
                result = BAERmfEditorDocument_SetSampleAssetForSample(conv->document,
                                                                       sampleIndex,
                                                                       baseAssetBySourceSlot[sourceSlot]);
                if (result == BAE_NO_ERROR)
                {
                    usedSharedAsset = TRUE;
                }
                else
                {
                    fprintf(stderr, "[mod2rmf] Warning: failed to share sample asset for program %u (%d); falling back to PCM copy\n",
                            (unsigned)setup.program,
                            (int)result);
                }
            }

            if (!usedSharedAsset)
            {
                XBOOL needsProcessing;

                chosenLoopStart = loopStart;
                chosenLoopEnd = loopEnd;
                needsProcessing = conv->forceOriginalSamples ? FALSE : TRUE;

                 if (!needsProcessing)
                 {
                     BAE_UNSIGNED_FIXED sampledRate;
                     double baseRate = playable->hasSampleRateOverride ? 
                                         (double)playable->sampleRateOverrideHz : 
                                         (double)conv->moduleBaseRateHz;
                     /* finetune is intentionally NOT applied to sample rate;
                      * libxmp folds it into ci->pitchbend (which we emit as
                      * MIDI pitch bend events) so applying it here would
                      * double the offset. */
                     double finetuneRatio = pow(2.0, (double)raw->finetune / 96.0);
                     /* Divide the physical baseRate by 4 to complement the -24 rootKey shift */
                     sampledRate = (BAE_UNSIGNED_FIXED)((double)(baseRate / 4.0) * finetuneRatio * 65536.0 + 0.5);
                     if (sampledRate < 65536u)
                     {
                         sampledRate = 65536u;
                     }

                     result = BAERmfEditorDocument_ReplaceSampleFromPCM(conv->document,
                                                                        sampleIndex,
                                                                        raw->pcm8,
                                                                        pcmFrames,
                                                                        8,
                                                                        1,
                                                                        sampledRate,
                                                                        loopStart,
                                                                        loopEnd,
                                                                        &sampleInfo);
                     if (result != BAE_NO_ERROR)
                     {
                         fprintf(stderr, "[mod2rmf] Warning: failed to inject raw 8-bit PCM for program %u (%d)\n",
                                 (unsigned)setup.program,
                                 (int)result);
                     }
                 }
                else
                {
                    int16_t *pcm16;
                    uint32_t outFrames;
                    uint32_t outRate;
                    uint32_t srcRateForProcessing;
                    uint32_t scaledLoopStart;
                    uint32_t scaledLoopEnd;
                    BAE_UNSIGNED_FIXED sampledRate;

                    /* Apply Amiga hardware filter and/or resampling.  The
                     * returned int16 buffer is already 16-bit so the engine can
                     * use the advancedInterpolation (PV_LoopWrapSample16) path. */
                     srcRateForProcessing = playable->hasSampleRateOverride && playable->sampleRateOverrideHz > 0u ?
                                             playable->sampleRateOverrideHz : conv->moduleBaseRateHz;

                    pcm16 = mod2rmf_process_sample(raw->pcm8, pcmFrames,
                                                   srcRateForProcessing,
                                                   &conv->resamplerSettings,
                                                   &outFrames, &outRate);
                    if (!pcm16)
                    {
                        fprintf(stderr,
                                "[mod2rmf] Warning: failed to process sample for program %u\n",
                                (unsigned)setup.program);
                        continue;
                    }

                    if (playable->hasSampleRateOverride)
                    {
                        sampledRate = (BAE_UNSIGNED_FIXED)((outRate / 4u) << 16);
                    }
                    else
                    {
                        /* finetune is intentionally NOT applied here;
                         * libxmp folds it into ci->pitchbend. */
                        double finetuneRatio = pow(2.0, (double)raw->finetune / 96.0);
                        sampledRate = (BAE_UNSIGNED_FIXED)((double)(outRate / 4.0) * finetuneRatio * 65536.0 + 0.5);
                        if (sampledRate < 65536u)
                        {
                            sampledRate = 65536u;
                        }
                    }

                    /* Scale loop end-points to the output frame domain so that
                     * ReplaceSampleFromPCM receives indices within the buffer.
                     * The subsequent remapping block recomputes them from
                     * sampleInfo.waveFrames, which handles any codec re-framing. */
                    scaledLoopStart = loopStart;
                    scaledLoopEnd   = loopEnd;
                    if (outFrames != pcmFrames && pcmFrames > 0)
                    {
                        scaledLoopStart = (uint32_t)(((uint64_t)loopStart * outFrames
                                                      + pcmFrames / 2u) / pcmFrames);
                        scaledLoopEnd   = (uint32_t)(((uint64_t)loopEnd   * outFrames
                                                      + pcmFrames / 2u) / pcmFrames);
                    }

                    result = BAERmfEditorDocument_ReplaceSampleFromPCM(conv->document,
                                                                       sampleIndex,
                                                                       pcm16,
                                                                       outFrames,
                                                                       16,
                                                                       1,
                                                                       sampledRate,
                                                                       scaledLoopStart,
                                                                       scaledLoopEnd,
                                                                       &sampleInfo);
                    free(pcm16);
                    if (result != BAE_NO_ERROR)
                    {
                        fprintf(stderr,
                                "[mod2rmf] Warning: failed to inject processed PCM for program %u (%d)\n",
                                (unsigned)setup.program,
                                (int)result);
                    }
                }

                if (result == BAE_NO_ERROR && !playable->offsetVariant && sourceSlot < MOD2RMF_MAX_SAMPLES)
                {
                    uint32_t assetID;

                    if (BAERmfEditorDocument_GetSampleAssetIDForSample(conv->document,
                                                                       sampleIndex,
                                                                       &assetID) == BAE_NO_ERROR)
                    {
                        baseAssetBySourceSlot[sourceSlot] = assetID;
                    }
                }
            }
            else
            {
                chosenLoopStart = loopStart;
                chosenLoopEnd = loopEnd;
            }
        }

        if (raw && raw->valid && chosenLoopEnd > chosenLoopStart)
        {
            BAERmfEditorSampleInfo editorSampleInfo;
            BAEResult infoResult;
            uint32_t dstFrames;
            uint32_t srcFrames;
            uint32_t mappedStart;
            uint32_t mappedEnd;
            uint32_t srcLoopStart;
            uint32_t srcLoopEnd;

            dstFrames = sampleInfo.waveFrames;
            srcFrames = raw->frameCount;
            srcLoopStart = chosenLoopStart;
            srcLoopEnd = chosenLoopEnd;
            mappedStart = srcLoopStart;
            mappedEnd = srcLoopEnd;

            if (srcFrames > 0 && dstFrames > 0 && srcFrames != dstFrames)
            {
                mappedStart = (uint32_t)(((uint64_t)srcLoopStart * (uint64_t)dstFrames + (uint64_t)(srcFrames / 2u)) / (uint64_t)srcFrames);
                mappedEnd = (uint32_t)(((uint64_t)srcLoopEnd * (uint64_t)dstFrames + (uint64_t)(srcFrames / 2u)) / (uint64_t)srcFrames);
            }
            if (mappedStart > dstFrames)
            {
                mappedStart = dstFrames;
            }
            if (mappedEnd > dstFrames)
            {
                mappedEnd = dstFrames;
            }
            if (mappedEnd <= mappedStart)
            {
                mappedStart = 0;
                mappedEnd = 0;
            }

            infoResult = BAERmfEditorDocument_GetSampleInfo(conv->document, sampleIndex, &editorSampleInfo);
            if (infoResult == BAE_NO_ERROR)
            {
                editorSampleInfo.sampleInfo.startLoop = mappedStart;
                editorSampleInfo.sampleInfo.endLoop = mappedEnd;
                infoResult = BAERmfEditorDocument_SetSampleInfo(conv->document, sampleIndex, &editorSampleInfo);
            }

            if (infoResult != BAE_NO_ERROR)
            {
                fprintf(stderr,
                        "[mod2rmf] Warning: failed to apply loop points for program %u (%d)\n",
                        (unsigned)setup.program,
                        (int)infoResult);
            }
        }
    }

    return 1;
}

/* --- Event sorting helpers for channel spreading ------------------------ */

/* Sort pitch bend events by tick. At the same tick, center-value resets
 * (MOD2RMF_PITCH_BEND_CENTER) sort first so that a new note's initial bend
 * at the same tick overrides the previous note's center reset. */
static int compare_pitch_bend_by_tick(const void *a, const void *b)
{
    const ModPitchBendEvent *ea = (const ModPitchBendEvent *)a;
    const ModPitchBendEvent *eb = (const ModPitchBendEvent *)b;

    if (ea->tick != eb->tick)
    {
        return (ea->tick < eb->tick) ? -1 : 1;
    }
    /* Same tick: center resets first */
    {
        int aCenter = (ea->value == MOD2RMF_PITCH_BEND_CENTER) ? 0 : 1;
        int bCenter = (eb->value == MOD2RMF_PITCH_BEND_CENTER) ? 0 : 1;
        return aCenter - bCenter;
    }
}

/* Sort CC events by tick. */
static int compare_cc_by_tick(const void *a, const void *b)
{
    const ModCCEvent *ea = (const ModCCEvent *)a;
    const ModCCEvent *eb = (const ModCCEvent *)b;

    if (ea->tick != eb->tick)
    {
        return (ea->tick < eb->tick) ? -1 : 1;
    }
    return 0;
}

/* --- Loop CC state reset ------------------------------------------------ */

/* When a song loops via meta markers, the engine kills active notes but does
 * NOT reset channel state (CC7, CC10, pitch bend, etc.).  If a channel's last
 * CC7 value at the end of the song differs from what was in effect at the loop
 * start, the channel will have the wrong volume until its first CC7 event
 * replays.
 *
 * This function finds the effective CC7 value at loopStartTick (the last CC7
 * at or before that tick, or 127 if none) and the last CC7 at or before
 * loopEndTick.  If they differ, it inserts a CC7 at loopStartTick with the
 * correct first-playthrough value so that the engine's channel state is
 * restored on loop-back.  A re-sort of CCs is performed afterwards to keep
 * chronological order. */

static int ensure_loop_cc_resets(ModSongModel *song)
{
    uint32_t ch, i;
    XBOOL needsSort = FALSE;

    if (!song || !song->loopEnabled)
    {
        return 1;
    }

    for (ch = 0; ch < song->channelCount; ++ch)
    {
        XBOOL hasNotes = FALSE;
        uint8_t channelProgram = 0xFF;
        /* Track the effective CC7 at loopStartTick and loopEndTick.
         * Engine default is 127 (MAX_NOTE_VOLUME). */
        uint8_t cc7AtLoopStart = 127; /* engine default */
        uint8_t cc7AtLoopEnd = 127;   /* engine default */
        XBOOL hasCC7 = FALSE;
        XBOOL hasCC7AtLoopStart = FALSE;

        /* Check if this channel has notes */
        for (i = 0; i < song->noteCount; ++i)
        {
            if (song->notes[i].sourceChannel == ch)
            {
                hasNotes = TRUE;
                if (channelProgram == 0xFF)
                {
                    channelProgram = song->notes[i].program;
                }
                break;
            }
        }
        if (!hasNotes) continue;

        /* Scan CC7 events (sorted by tick) to find:
         * 1. Effective CC7 at loopStartTick (last CC7 at or before that tick)
         * 2. Effective CC7 at loopEndTick (last CC7 at or before that tick)
         * 3. Whether an explicit CC7 exists exactly at loopStartTick */
        for (i = 0; i < song->ccCount; ++i)
        {
            if (song->ccEvents[i].sourceChannel == ch && song->ccEvents[i].cc == 7)
            {
                hasCC7 = TRUE;
                if (song->ccEvents[i].tick <= song->loopStartTick)
                {
                    cc7AtLoopStart = song->ccEvents[i].value;
                    if (song->ccEvents[i].tick == song->loopStartTick)
                    {
                        hasCC7AtLoopStart = TRUE;
                    }
                }
                if (song->ccEvents[i].tick <= song->loopEndTick)
                {
                    cc7AtLoopEnd = song->ccEvents[i].value;
                }
            }
        }

        /* Only insert a reset if:
         * 1. The channel has CC7 events (otherwise engine default applies)
         * 2. No explicit CC7 already exists at loopStartTick
         * 3. The CC7 at loopEnd differs from CC7 at loopStart — meaning the
         *    engine will have the wrong volume when it loops back */
        if (hasCC7 && !hasCC7AtLoopStart && cc7AtLoopEnd != cc7AtLoopStart)
        {
            if (!song_model_append_cc_event(song, (uint16_t)ch,
                                            song->loopStartTick, 7,
                                            cc7AtLoopStart,
                                            channelProgram))
            {
                return 0;
            }
            needsSort = TRUE;
            #ifdef _DEBUG
            fprintf(stderr, "[mod2rmf] Loop CC7 reset: ch %u -> CC7=%u @ tick %u (end was %u)\n",
                    ch, cc7AtLoopStart, (unsigned)song->loopStartTick, cc7AtLoopEnd);
            #endif
        }
    }

    if (needsSort && song->ccCount > 1)
    {
        qsort(song->ccEvents, song->ccCount,
              sizeof(ModCCEvent), compare_cc_by_tick);
    }

    return 1;
}

/* Same idea as ensure_loop_cc_resets, but for pitch bend.  The engine keeps
 * channel pitch-bend state across loop-back, so if the last bend before
 * loopEnd differs from what was active at loopStart, notes will play at the
 * wrong pitch on subsequent loops (and it compounds each iteration). */

static int ensure_loop_pitch_bend_resets(ModSongModel *song)
{
    uint32_t ch, i;
    XBOOL needsSort = FALSE;

    if (!song || !song->loopEnabled)
    {
        return 1;
    }

    for (ch = 0; ch < song->channelCount; ++ch)
    {
        XBOOL hasNotes = FALSE;
        uint8_t channelProgram = 0xFF;
        /* Engine default pitch bend is center */
        uint16_t bendAtLoopStart = MOD2RMF_PITCH_BEND_CENTER;
        uint16_t bendAtLoopEnd = MOD2RMF_PITCH_BEND_CENTER;
        XBOOL hasBend = FALSE;
        XBOOL hasBendAtLoopStart = FALSE;

        /* Check if this channel has notes */
        for (i = 0; i < song->noteCount; ++i)
        {
            if (song->notes[i].sourceChannel == ch)
            {
                hasNotes = TRUE;
                if (channelProgram == 0xFF)
                {
                    channelProgram = song->notes[i].program;
                }
                break;
            }
        }
        if (!hasNotes) continue;

        /* Scan pitch bend events to find effective values at loop boundaries */
        for (i = 0; i < song->pitchBendCount; ++i)
        {
            if (song->pitchBendEvents[i].sourceChannel == ch)
            {
                hasBend = TRUE;
                if (song->pitchBendEvents[i].tick <= song->loopStartTick)
                {
                    bendAtLoopStart = song->pitchBendEvents[i].value;
                    if (song->pitchBendEvents[i].tick == song->loopStartTick)
                    {
                        hasBendAtLoopStart = TRUE;
                    }
                }
                if (song->pitchBendEvents[i].tick <= song->loopEndTick)
                {
                    bendAtLoopEnd = song->pitchBendEvents[i].value;
                }
            }
        }

        /* Insert a reset if bends exist, no explicit bend at loop start,
         * and the end-of-song bend differs from the loop-start bend */
        if (hasBend && !hasBendAtLoopStart && bendAtLoopEnd != bendAtLoopStart)
        {
            if (!song_model_append_pitch_bend(song, (uint16_t)ch,
                                              song->loopStartTick,
                                              bendAtLoopStart,
                                              channelProgram))
            {
                return 0;
            }
            needsSort = TRUE;
            #ifdef _DEBUG
            fprintf(stderr, "[mod2rmf] Loop pitch bend reset: ch %u -> bend=%u @ tick %u (end was %u)\n",
                    ch, bendAtLoopStart, (unsigned)song->loopStartTick, bendAtLoopEnd);
            #endif
        }
    }

    if (needsSort && song->pitchBendCount > 1)
    {
        qsort(song->pitchBendEvents, song->pitchBendCount,
              sizeof(ModPitchBendEvent), compare_pitch_bend_by_tick);
    }

    return 1;
}

/* --- Channel spreading by program --------------------------------------- */

/* Spread tracker channels so each unique program (instrument/sample) gets its
 * own virtual channel.  The BAE engine is fully polyphonic (note-on over
 * note-on is supported), so there is no need to split the same program across
 * multiple channels even if it plays on several tracker channels at once.
 *
 * After spreading:
 *  - note->sourceChannel is rewritten to the program's virtual channel
 *  - CC and pitch-bend events are routed via their program tag
 *  - song->channelCount is updated to reflect the new virtual channel count
 *
 * If unique programs > 16, programs are packed into 16 channels using an
 * overlap-minimizing algorithm. */

static int spread_channels_by_program(ModSongModel *song, XBOOL isMod,
                                      uint8_t stereoSep)
{
    uint8_t virtualChanMap[MOD2RMF_MAX_CHANNELS][128];
    uint8_t originalChMapped[MOD2RMF_MAX_CHANNELS];
    uint8_t nextVirtualChan;
    uint32_t i;
    uint32_t preBendCount;

    (void)isMod;
    (void)stereoSep;

    if (!song || song->noteCount == 0)
    {
        return 1;
    }

    memset(virtualChanMap, 0xFF, sizeof(virtualChanMap));
    memset(originalChMapped, 0, sizeof(originalChMapped));
    
    nextVirtualChan = (uint8_t)song->channelCount;

    /* Phase 1: Assign a virtual channel to each (tracker channel, program) tuple */
    for (i = 0; i < song->noteCount; ++i)
    {
        uint8_t ch = song->notes[i].sourceChannel;
        uint8_t prog = song->notes[i].program;

        if (ch >= MOD2RMF_MAX_CHANNELS) continue;

        if (virtualChanMap[ch][prog] == 0xFF)
        {
            if (originalChMapped[ch] == 0)
            {
                /* First program seen on this tracker channel keeps the original channel ID */
                virtualChanMap[ch][prog] = ch;
                originalChMapped[ch] = 1;
            }
            else if (nextVirtualChan < MOD2RMF_MAX_CHANNELS)
            {
                /* Subsequent programs on this channel get a new virtual channel */
                virtualChanMap[ch][prog] = nextVirtualChan++;
            }
            else
            {
                /* Fallback if we run out of virtual channels */
                virtualChanMap[ch][prog] = ch;
            }
        }
        
        song->notes[i].sourceChannel = virtualChanMap[ch][prog];
    }
    
    /* Phase 2: Route CC events */
    for (i = 0; i < song->ccCount; ++i)
    {
        uint8_t ch = song->ccEvents[i].sourceChannel;
        uint8_t prog = song->ccEvents[i].program;
        if (ch < MOD2RMF_MAX_CHANNELS)
        {
            if (virtualChanMap[ch][prog] == 0xFF)
            {
                if (originalChMapped[ch] == 0) { virtualChanMap[ch][prog] = ch; originalChMapped[ch] = 1; }
                else if (nextVirtualChan < MOD2RMF_MAX_CHANNELS) { virtualChanMap[ch][prog] = nextVirtualChan++; }
                else { virtualChanMap[ch][prog] = ch; }
            }
            song->ccEvents[i].sourceChannel = virtualChanMap[ch][prog];
        }
    }

    /* Phase 3: Route pitch bend events */
    preBendCount = song->pitchBendCount;
    for (i = 0; i < preBendCount; ++i)
    {
        uint8_t ch = song->pitchBendEvents[i].sourceChannel;
        uint8_t prog = song->pitchBendEvents[i].program;
        if (ch < MOD2RMF_MAX_CHANNELS)
        {
            if (virtualChanMap[ch][prog] == 0xFF)
            {
                if (originalChMapped[ch] == 0) { virtualChanMap[ch][prog] = ch; originalChMapped[ch] = 1; }
                else if (nextVirtualChan < MOD2RMF_MAX_CHANNELS) { virtualChanMap[ch][prog] = nextVirtualChan++; }
                else { virtualChanMap[ch][prog] = ch; }
            }
            song->pitchBendEvents[i].sourceChannel = virtualChanMap[ch][prog];
        }
    }

    song->channelCount = nextVirtualChan;

    /* Phase 4: Sort events by tick so the write-phase dedup processes them chronologically. */
    if (song->pitchBendCount > 1)
    {
        qsort(song->pitchBendEvents, song->pitchBendCount,
              sizeof(ModPitchBendEvent), compare_pitch_bend_by_tick);
    }
    if (song->ccCount > 1)
    {
        qsort(song->ccEvents, song->ccCount,
              sizeof(ModCCEvent), compare_cc_by_tick);
    }

    return 1;
}

/* --- Channel analysis and mapping --------------------------------------- */

static void channel_profile_cleanup(ChannelProfile *profiles, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i)
    {
        free(profiles[i].activeRanges);
        profiles[i].activeRanges = NULL;
        profiles[i].rangeCount = 0;
        profiles[i].rangeCapacity = 0;
    }
}

static int channel_profile_add_range(ChannelProfile *p, uint32_t start, uint32_t end)
{
    if (p->rangeCount >= p->rangeCapacity)
    {
        uint32_t newCap = (p->rangeCapacity == 0) ? 64 : p->rangeCapacity * 2;
        TickRange *tmp = (TickRange *)realloc(p->activeRanges, newCap * sizeof(TickRange));
        if (!tmp) return 0;
        p->activeRanges = tmp;
        p->rangeCapacity = newCap;
    }
    p->activeRanges[p->rangeCount].startTick = start;
    p->activeRanges[p->rangeCount].endTick = end;
    p->rangeCount++;
    return 1;
}

static void channel_profile_add_program(ChannelProfile *p, uint8_t program)
{
    uint32_t i;
    for (i = 0; i < p->programCount; ++i)
    {
        if (p->programs[i] == program) return;
    }
    if (p->programCount < CHANNEL_PROFILE_MAX_PROGRAMS)
    {
        p->programs[p->programCount++] = program;
    }
}

static void analyze_channel_usage(const ModSongModel *song,
                                  ChannelProfile profiles[],
                                  uint32_t maxChannels)
{
    uint32_t i;

    memset(profiles, 0, maxChannels * sizeof(ChannelProfile));

    /* Scan notes to build active ranges and program sets */
    for (i = 0; i < song->noteCount; ++i)
    {
        const ModNoteEvent *n = &song->notes[i];
        uint16_t ch = n->sourceChannel;
        if (ch >= maxChannels) continue;

        profiles[ch].used = TRUE;
        profiles[ch].noteCount++;
        channel_profile_add_program(&profiles[ch], n->program);
        channel_profile_add_range(&profiles[ch], n->startTick,
                                  n->startTick + n->durationTicks);
    }
}

/* Check whether any active range in profile 'a' overlaps with any in 'b'. */
static XBOOL ranges_overlap(const ChannelProfile *a, const ChannelProfile *b)
{
    uint32_t i, j;
    for (i = 0; i < a->rangeCount; ++i)
    {
        for (j = 0; j < b->rangeCount; ++j)
        {
            if (a->activeRanges[i].startTick < b->activeRanges[j].endTick &&
                b->activeRanges[j].startTick < a->activeRanges[i].endTick)
            {
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* Count how many ticks of overlap exist between two channel profiles. */
static uint32_t overlap_ticks(const ChannelProfile *a, const ChannelProfile *b)
{
    uint32_t total = 0;
    uint32_t i, j;
    for (i = 0; i < a->rangeCount; ++i)
    {
        for (j = 0; j < b->rangeCount; ++j)
        {
            uint32_t lo = (a->activeRanges[i].startTick > b->activeRanges[j].startTick)
                          ? a->activeRanges[i].startTick : b->activeRanges[j].startTick;
            uint32_t hi = (a->activeRanges[i].endTick < b->activeRanges[j].endTick)
                          ? a->activeRanges[i].endTick : b->activeRanges[j].endTick;
            if (lo < hi) total += (hi - lo);
        }
    }
    return total;
}

/* Build aggregate profile for a MIDI channel (union of all tracker channels
 * already assigned to it). Only considers active ranges for overlap testing. */
static void build_midi_channel_aggregate(const ChannelProfile trackerProfiles[],
                                         const uint8_t trackerToMidi[],
                                         uint32_t trackerCount,
                                         uint8_t midiCh,
                                         ChannelProfile *agg)
{
    uint32_t i, j;
    memset(agg, 0, sizeof(*agg));
    for (i = 0; i < trackerCount; ++i)
    {
        if (trackerToMidi[i] != midiCh) continue;
        if (!trackerProfiles[i].used) continue;

        for (j = 0; j < trackerProfiles[i].rangeCount; ++j)
        {
            channel_profile_add_range(agg, trackerProfiles[i].activeRanges[j].startTick,
                                           trackerProfiles[i].activeRanges[j].endTick);
        }
        agg->noteCount += trackerProfiles[i].noteCount;
        agg->used = TRUE;
    }
}

static void compute_channel_map(const ChannelProfile profiles[],
                                uint32_t trackerCount,
                                ChannelMap *map)
{
    uint32_t i;
    uint8_t midiCh;

    memset(map, 0, sizeof(*map));
    /* Initialize all mappings to 0xFF (unmapped) */
    memset(map->trackerToMidi, 0xFF, sizeof(map->trackerToMidi));

    /* Pass 1: Direct assignment for first min(trackerCount, 16) channels */
    for (i = 0; i < trackerCount && i < MOD2RMF_MAX_MIDI_CHANNELS; ++i)
    {
        map->trackerToMidi[i] = (uint8_t)i;
        if (profiles[i].used)
        {
            map->midiChannelUsed[i] = TRUE;
        }
    }

    /* Pass 2: Overflow assignment for channels 16+ */
    for (i = MOD2RMF_MAX_MIDI_CHANNELS; i < trackerCount; ++i)
    {
        uint8_t bestMidi = 0;
        uint32_t bestScore = UINT32_MAX; /* lower = better */
        XBOOL foundEmpty = FALSE;

        if (!profiles[i].used)
        {
            /* Unused tracker channel — map to ch 0 as placeholder */
            map->trackerToMidi[i] = 0;
            continue;
        }

        for (midiCh = 0; midiCh < MOD2RMF_MAX_MIDI_CHANNELS; ++midiCh)
        {
            ChannelProfile agg;
            uint32_t ovlap;

            memset(&agg, 0, sizeof(agg));

            if (!map->midiChannelUsed[midiCh])
            {
                /* Empty MIDI channel — best possible choice */
                bestMidi = midiCh;
                foundEmpty = TRUE;
                break;
            }

            /* Build aggregate profile for this MIDI channel */
            build_midi_channel_aggregate(profiles, map->trackerToMidi,
                                         trackerCount, midiCh, &agg);

            /* Check overlap between overflow channel and aggregate */
            ovlap = overlap_ticks(&profiles[i], &agg);
            free(agg.activeRanges);

            if (ovlap < bestScore)
            {
                bestScore = ovlap;
                bestMidi = midiCh;
                if (ovlap == 0) break; /* no overlap = no conflict */
            }
        }

        map->trackerToMidi[i] = bestMidi;
        map->midiChannelUsed[bestMidi] = TRUE;

        if (!foundEmpty && bestScore > 0)
        {
            #ifdef _DEBUG
            fprintf(stderr, "[mod2rmf] Channel map: tracker ch %u -> MIDI ch %u (overlap %u ticks)\n",
                    i, bestMidi, bestScore);
            #endif
        }
    }
}

static int setup_tracks(Mod2RmfConverter *conv, const ModSongModel *song, const ChannelMap *chMap)
{
    uint32_t i;
    uint32_t channelsToAdd;
    XBOOL midiChInitialized[MOD2RMF_MAX_MIDI_CHANNELS];

    if (!conv || !song || !chMap)
    {
        return 0;
    }

    channelsToAdd = song->channelCount;
    conv->channelToTrackIndex = (uint16_t *)malloc(song->channelCount * sizeof(uint16_t));
    if (!conv->channelToTrackIndex)
    {
        return 0;
    }
    memset(conv->channelToTrackIndex, 0xFF, song->channelCount * sizeof(uint16_t));
    memset(midiChInitialized, 0, sizeof(midiChInitialized));

    for (i = 0; i < channelsToAdd; ++i)
    {
        BAERmfEditorTrackSetup setup;
        BAEResult result;
        uint16_t trackIndex;
        char trackName[64];

        memset(&setup, 0, sizeof(setup));
        setup.channel = chMap->trackerToMidi[i];
        setup.bank = MOD2RMF_EMBEDDED_BANK;
        setup.program = 0;
        snprintf(trackName, sizeof(trackName), "Ch %u", i + 1);
        setup.name = trackName;

        result = BAERmfEditorDocument_AddTrack(conv->document, &setup, &trackIndex);
        if (result != BAE_NO_ERROR)
        {
            fprintf(stderr, "[mod2rmf] Warning: failed to add track for channel %u (%d)\n", i, (int)result);
            continue;
        }

        conv->channelToTrackIndex[i] = trackIndex;
        BAERmfEditorDocument_SetTrackDefaultInstrument(conv->document,
                                   trackIndex,
                                   MOD2RMF_EMBEDDED_BANK,
                                   0);

        /* Per-MIDI-channel initialization (pitch bend range, ch10 melodic mode)
         * — only emit once per MIDI channel even if multiple tracks share it. */
        if (!midiChInitialized[setup.channel])
        {
            midiChInitialized[setup.channel] = TRUE;

            /* Set pitch bend range to ±N semitones via RPN */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 101, 0, 0); /* RPN MSB */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 100, 0, 0); /* RPN LSB */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,   6, 0,
                                                 song->pitchBendRangeSemitones);         /* Data Entry */
            BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,  38, 0, 0); /* Data LSB */

            /* MIDI channel 10 (zero-based 9) defaults to percussion in many synths.
             * NRPN 5,0 with data 3 switches it to melodic playback. */
            if (setup.channel == 9u)
            {
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 99, 0, 5); /* NRPN MSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex, 98, 0, 0); /* NRPN LSB */
                BAERmfEditorDocument_AddTrackCCEvent(conv->document, trackIndex,  6, 0, 3); /* Data Entry MSB */
            }
        }
    }

    return 1;
}

static XBOOL is_zmf_path(const char *path)
{
    const char *ext;

    if (!path)
    {
        return FALSE;
    }

    ext = strrchr(path, '.');
    return (ext && (!strcmp(ext, ".zmf") || !strcmp(ext, ".ZMF"))) ? TRUE : FALSE;
}

static int setup_instrument_ext(Mod2RmfConverter *conv, const ModSongModel *song, XBOOL useZmfContainer)
{
    uint32_t i;

    if (!conv || !conv->document || !song)
    {
        return 0;
    }

    for (i = 0; i < song->playableCount; ++i)
    {
        BAERmfEditorInstrumentExtInfo extInfo;
        BAEResult result;
        uint32_t instID;
        const ModPlayable *playable;

        playable = &song->playables[i];
        instID = 512u + (uint32_t)playable->program;

        memset(&extInfo, 0, sizeof(extInfo));
        result = BAERmfEditorDocument_GetInstrumentExtInfo(conv->document, instID, &extInfo);
        if (result != BAE_NO_ERROR)
        {
            /* Continue with safe defaults if the get call fails. */
            memset(&extInfo, 0, sizeof(extInfo));
            extInfo.instID = instID;
            extInfo.flags1 = MOD2RMF_ZBF_USE_SAMPLE_RATE;
            extInfo.flags2 = 0;
            extInfo.midiRootKey = 60;
            extInfo.miscParameter2 = 100;
            /* 2-stage ADSR: sustain at max, then immediate release to 0.
             * This activates the "new style" ADSR path in the engine,
             * which frees voices immediately on note-off instead of
             * lingering for 8 render cycles (the "old style" NoteDecay
             * path).  Without this, CC#7 changes during the 8-cycle
             * decay bleed into the releasing voice. */
            extInfo.volumeADSR.stageCount = 2;
            extInfo.volumeADSR.stages[0].level = VOLUME_RANGE;
            extInfo.volumeADSR.stages[0].time = 0;
            extInfo.volumeADSR.stages[0].flags = ADSR_SUSTAIN_LONG;
            extInfo.volumeADSR.stages[1].level = 0;
            extInfo.volumeADSR.stages[1].time = 0;
            extInfo.volumeADSR.stages[1].flags = ADSR_TERMINATE_LONG;
        }

        extInfo.instID = instID;
        extInfo.displayName = playable->displayName;
        /* Always force root key to 60 (C-4).  GetInstrumentExtInfo may
         * return a different default depending on the sample rate the
         * engine inferred during ReplaceSampleFromPCM; that mismatch
         * causes some S3M samples to play at the wrong pitch. */
        extInfo.midiRootKey = 60;

        /* Apply per-instrument pan placement from tracker sub-instrument */
        extInfo.panPlacement = playable->panPlacement;

        if (playable->hasVolumeAdsr && playable->adsrStageCount > 0)
        {
            /* Multi-stage envelope: each stage was pre-built by extract_envelope_adsr
             * with level, timeUs, and flags already in BAE format. */
            uint32_t s;
            extInfo.volumeADSR.stageCount = playable->adsrStageCount;
            for (s = 0; s < playable->adsrStageCount && s < BAE_EDITOR_MAX_ADSR_STAGES; ++s)
            {
                extInfo.volumeADSR.stages[s].level = playable->adsrStages[s].level;
                extInfo.volumeADSR.stages[s].time = playable->adsrStages[s].timeUs;
                extInfo.volumeADSR.stages[s].flags = playable->adsrStages[s].flags;
            }
        }
        else
        {
            /* MOD fallback: sustain at max then immediate release. */
            extInfo.volumeADSR.stageCount = 2;
            extInfo.volumeADSR.stages[0].level = VOLUME_RANGE;
            extInfo.volumeADSR.stages[0].time = 0;
            extInfo.volumeADSR.stages[0].flags = ADSR_SUSTAIN_LONG;
            extInfo.volumeADSR.stages[1].level = 0;
            extInfo.volumeADSR.stages[1].time = 0;
            extInfo.volumeADSR.stages[1].flags = ADSR_TERMINATE_LONG;
        }

        extInfo.flags1 |= MOD2RMF_ZBF_USE_SAMPLE_RATE;
        /* RMF uses normal interpolation; advanced interpolation is ZMF-only
         * and only valid for the processed 16-bit sample path. */
        if (useZmfContainer && !conv->forceOriginalSamples)
        {
            extInfo.flags1 &= (unsigned char)~MOD2RMF_ZBF_ENABLE_INTERPOLATE;
            extInfo.flags2 |= MOD2RMF_ZBF_ADVANCED_INTERPOLATION;
        }
        else
        {
            extInfo.flags1 |= MOD2RMF_ZBF_ENABLE_INTERPOLATE;
            extInfo.flags2 &= (unsigned char)~MOD2RMF_ZBF_ADVANCED_INTERPOLATION;
        }
        extInfo.flags1 &= (unsigned char)~(MOD2RMF_ZBF_DISABLE_SND_LOOPING | MOD2RMF_ZBF_SAMPLE_AND_HOLD);
        /* Match what the instrument editor produces on OK for plain samples:
         * do not force miscParameter1 as root key. */
        extInfo.flags2 &= (unsigned char)~MOD2RMF_ZBF_USE_SMOD_AS_ROOTKEY;

        if (useZmfContainer && playable->offsetVariant)
        {
            uint32_t offsetFrames;
            offsetFrames = playable->sampleOffsetBytes;
            extInfo.flags2 |= MOD2RMF_ZBF_ENABLE_SAMPLE_OFFSET_START;
            extInfo.miscParameter1 = (int16_t)((offsetFrames >> 16) & 0xFFFFu);
            extInfo.miscParameter2 = (int16_t)(offsetFrames & 0xFFFFu);
        }
        else
        {
            extInfo.flags2 &= (unsigned char)~MOD2RMF_ZBF_ENABLE_SAMPLE_OFFSET_START;
            extInfo.miscParameter1 = 0;
            if (extInfo.miscParameter2 == 0)
            {
                extInfo.miscParameter2 = 100;
            }
        }

        /* MOD files don't use mod-wheel vibrato; suppress the engine's
         * automatic pitch LFO injection. */
        extInfo.hasDefaultMod = TRUE;

        (void)BAERmfEditorDocument_SetInstrumentExtInfo(conv->document, instID, &extInfo);
    }

    return 1;
}

static int add_programmed_note(Mod2RmfConverter *conv,
                               uint16_t trackIndex,
                               uint32_t startTick,
                               uint32_t durationTicks,
                               unsigned char note,
                               unsigned char velocity,
                               unsigned char program)
{
    BAEResult result;
    uint32_t noteCount;
    BAERmfEditorNoteInfo info;

    if (!conv || trackIndex == (uint16_t)0xFFFF)
    {
        return 0;
    }

    result = BAERmfEditorDocument_AddNote(conv->document,
                                          trackIndex,
                                          startTick,
                                          durationTicks,
                                          note,
                                          velocity);
    if (result != BAE_NO_ERROR)
    {
        return 0;
    }

    noteCount = 0;
    if (BAERmfEditorDocument_GetNoteCount(conv->document, trackIndex, &noteCount) != BAE_NO_ERROR || noteCount == 0)
    {
        return 0;
    }
    if (BAERmfEditorDocument_GetNoteInfo(conv->document, trackIndex, noteCount - 1, &info) != BAE_NO_ERROR)
    {
        return 0;
    }

    info.bank = MOD2RMF_EMBEDDED_BANK;
    info.program = program;
    return BAERmfEditorDocument_SetNoteInfo(conv->document, trackIndex, noteCount - 1, &info) == BAE_NO_ERROR;
}

static int write_song_notes(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;

    if (!conv || !song)
    {
        return 0;
    }

    for (i = 0; i < song->noteCount; ++i)
    {
        const ModNoteEvent *note;
        uint16_t trackIndex;

        note = &song->notes[i];
        trackIndex = conv->channelToTrackIndex[note->sourceChannel];
        if (trackIndex == (uint16_t)0xFFFF)
        {
            continue;
        }

        (void)add_programmed_note(conv,
                                  trackIndex,
                                  note->startTick,
                                  note->durationTicks,
                                  note->note,
                                  note->velocity,
                                  note->program);
    }

    return 1;
}

static int write_song_cc_events(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;
    /* For deduplication: track the primary RMF track per MIDI channel,
     * and the last emitted value per (midiCh, cc#) to avoid redundant events. */
    uint16_t midiChPrimaryTrack[MOD2RMF_MAX_MIDI_CHANNELS];
    /* Last emitted CC values: [midiCh][cc] — 0xFFFF = not yet emitted */
    uint16_t lastCC[MOD2RMF_MAX_MIDI_CHANNELS][128];

    if (!conv || !song)
    {
        return 0;
    }

    XBOOL ccDedupReset = FALSE;
    memset(lastCC, 0xFF, sizeof(lastCC));

    /* Find primary (first) RMF track for each MIDI channel */
    memset(midiChPrimaryTrack, 0xFF, sizeof(midiChPrimaryTrack));
    for (i = 0; i < song->channelCount; ++i)
    {
        uint8_t midiCh = conv->channelMap.trackerToMidi[i];
        if (midiCh < MOD2RMF_MAX_MIDI_CHANNELS &&
            midiChPrimaryTrack[midiCh] == (uint16_t)0xFFFF &&
            conv->channelToTrackIndex[i] != (uint16_t)0xFFFF)
        {
            midiChPrimaryTrack[midiCh] = conv->channelToTrackIndex[i];
        }
    }

    for (i = 0; i < song->ccCount; ++i)
    {
        const ModCCEvent *ev;
        uint8_t midiCh;
        uint16_t trackIndex;

        ev = &song->ccEvents[i];
        if (ev->sourceChannel >= song->channelCount) continue;

        /* When we reach the loop start tick, reset dedup so every CC
         * value is re-emitted.  The engine's channel state at loop-back
         * comes from the end of the song, which may differ from what
         * was active at the loop start on the first playthrough. */
        if (song->loopEnabled && !ccDedupReset &&
            ev->tick >= song->loopStartTick)
        {
            memset(lastCC, 0xFF, sizeof(lastCC));
            ccDedupReset = TRUE;
        }

        midiCh = conv->channelMap.trackerToMidi[ev->sourceChannel];
        if (midiCh >= MOD2RMF_MAX_MIDI_CHANNELS) continue;

        /* Route through the primary track for this MIDI channel */
        trackIndex = midiChPrimaryTrack[midiCh];
        if (trackIndex == (uint16_t)0xFFFF) continue;

        /* Skip if this CC value was already emitted for this MIDI channel */
        if (lastCC[midiCh][ev->cc] == (uint16_t)ev->value) continue;
        lastCC[midiCh][ev->cc] = (uint16_t)ev->value;

        (void)BAERmfEditorDocument_AddTrackCCEvent(conv->document,
                                                   trackIndex,
                                                   ev->cc,
                                                   ev->tick,
                                                   ev->value);
    }

    return 1;
}

static int write_song_pitch_bend_events(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;
    /* Deduplication: primary track and last value per MIDI channel */
    uint16_t midiChPrimaryTrack[MOD2RMF_MAX_MIDI_CHANNELS];
    uint16_t lastBend[MOD2RMF_MAX_MIDI_CHANNELS];

    if (!conv || !song)
    {
        return 0;
    }

    /* 0xFFFF = not yet emitted */
    XBOOL bendDedupReset = FALSE;
    memset(lastBend, 0xFF, sizeof(lastBend));

    /* Find primary (first) RMF track for each MIDI channel */
    memset(midiChPrimaryTrack, 0xFF, sizeof(midiChPrimaryTrack));
    for (i = 0; i < song->channelCount; ++i)
    {
        uint8_t midiCh = conv->channelMap.trackerToMidi[i];
        if (midiCh < MOD2RMF_MAX_MIDI_CHANNELS &&
            midiChPrimaryTrack[midiCh] == (uint16_t)0xFFFF &&
            conv->channelToTrackIndex[i] != (uint16_t)0xFFFF)
        {
            midiChPrimaryTrack[midiCh] = conv->channelToTrackIndex[i];
        }
    }

    for (i = 0; i < song->pitchBendCount; ++i)
    {
        const ModPitchBendEvent *ev;
        uint8_t midiCh;
        uint16_t trackIndex;
        uint32_t j;
        XBOOL superseded = FALSE;

        ev = &song->pitchBendEvents[i];
        if (ev->sourceChannel >= song->channelCount) continue;

        /* Reset dedup at loop start so bend state is re-established */
        if (song->loopEnabled && !bendDedupReset &&
            ev->tick >= song->loopStartTick)
        {
            memset(lastBend, 0xFF, sizeof(lastBend));
            bendDedupReset = TRUE;
        }

        midiCh = conv->channelMap.trackerToMidi[ev->sourceChannel];
        if (midiCh >= MOD2RMF_MAX_MIDI_CHANNELS) continue;

        /* Route through the primary track for this MIDI channel */
        trackIndex = midiChPrimaryTrack[midiCh];
        if (trackIndex == (uint16_t)0xFFFF) continue;

        /* Look ahead to see if there's another bend for this MIDI channel at the same tick.
         * If there is, we skip this one and let the later one take effect. */
        for (j = i + 1; j < song->pitchBendCount; ++j)
        {
            const ModPitchBendEvent *nextEv = &song->pitchBendEvents[j];
            if (nextEv->tick > ev->tick) break; /* Events are strictly sorted by tick */
            if (nextEv->tick == ev->tick)
            {
                if (nextEv->sourceChannel < song->channelCount &&
                    conv->channelMap.trackerToMidi[nextEv->sourceChannel] == midiCh)
                {
                    superseded = TRUE;
                    break;
                }
            }
        }
        
        if (superseded) {
            continue;
        }

        /* Skip if bend value hasn't changed for this MIDI channel */
        if (lastBend[midiCh] == ev->value) {
            continue;
        }
        lastBend[midiCh] = ev->value;

        (void)BAERmfEditorDocument_AddTrackPitchBendEvent(conv->document,
                                                           trackIndex,
                                                           ev->tick,
                                                           ev->value);
    }

    return 1;
}

static int write_song_tempo_events(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;

    if (!conv || !song)
    {
        return 0;
    }

    for (i = 0; i < song->tempoChangeCount; ++i)
    {
        const ModTempoChange *tc;
        uint32_t microsecondsPerBeat;

        tc = &song->tempoChanges[i];
        if (tc->bpm == 0)
        {
            continue;
        }
        microsecondsPerBeat = 60000000UL / tc->bpm;
        BAERmfEditorDocument_AddTempoEvent(conv->document, tc->tick, microsecondsPerBeat);
    }

    return 1;
}

static int save_document(Mod2RmfConverter *conv, const char *destPath)
{
    FILE *outFile;
    unsigned char *rmfData;
    uint32_t rmfSize;
    BAEResult result;
    const char *ext;
    XBOOL useZmfContainer;
    XBOOL requiresZmf;

    if (!conv || !conv->document || !destPath)
    {
        return 0;
    }

    ext = strrchr(destPath, '.');
    useZmfContainer = (ext && (!strcmp(ext, ".zmf") || !strcmp(ext, ".ZMF"))) ? TRUE : FALSE;
    requiresZmf = BAERmfEditorDocument_RequiresZmf(conv->document);

    if (requiresZmf && !useZmfContainer)
    {
        fprintf(stderr,
                "[mod2rmf] Error: document requires ZMF format due to RMF-incompatible sample data \n"
                "[mod2rmf] (use of a modern codec, or likely a loop shorter than %u frames). \n"
                "[mod2rmf] Please use a .zmf output extension.\n",
                (unsigned)MIN_LOOP_SIZE_RMF);
        return 0;
    }

    rmfData = NULL;
    rmfSize = 0;
    result = BAERmfEditorDocument_SaveAsRmfToMemory(conv->document,
                                                    useZmfContainer,
                                                    &rmfData,
                                                    &rmfSize);
    if (result != BAE_NO_ERROR)
    {
        fprintf(stderr, "[mod2rmf] Error: save failed (%d): %s\n", (int)result, destPath);
        return 0;
    }

    outFile = fopen(destPath, "wb");
    if (!outFile)
    {
        XDisposePtr((XPTR)rmfData);
        return 0;
    }
    if (fwrite(rmfData, 1, rmfSize, outFile) != rmfSize)
    {
        fclose(outFile);
        XDisposePtr((XPTR)rmfData);
        return 0;
    }
    fclose(outFile);
    XDisposePtr((XPTR)rmfData);

    return 1;
}

int main(int argc, char *argv[])
{
    const char *sourcePath;
    const char *destPath;
    int argi;
    int tempoMap;
    XBOOL useZmfContainer;
    Mod2RmfConverter *conv;
    ModSongModel song;
    BAEResult setupResult;
    Mod2RmfEncoderSettings encSettings;
    Mod2RmfResamplerSettings resamplerSettings;
    BAERmfEditorCompressionType compressionType;
    XBOOL forceOriginalSamples;
    XBOOL codecArgSeen;
    XBOOL bitrateArgSeen;
    XBOOL spreadChannels;
    uint8_t stereoSeparation;

    sourcePath = NULL;
    destPath = NULL;
    tempoMap = 0;
    mod2rmf_encoder_defaults(&encSettings);
    mod2rmf_resampler_defaults(&resamplerSettings);
    forceOriginalSamples = FALSE;
    codecArgSeen = FALSE;
    bitrateArgSeen = FALSE;
    spreadChannels = FALSE;
    stereoSeparation = 75;
    song_model_init(&song);

    if (argc < 3)
    {
        print_usage(argv[0]);
        return 1;
    }

    for (argi = 1; argi < argc; ++argi)
    {
        const char *arg;
        arg = argv[argi];

        if (!strcmp(arg, "--tempomap"))
        {
            tempoMap = 1;
            continue;
        }
        if (!strcmp(arg, "--spread"))
        {
            spreadChannels = TRUE;
            continue;
        }
        if (!strcmp(arg, "--original"))
        {
            forceOriginalSamples = TRUE;
            continue;
        }
        if (!strcmp(arg, "--codecs"))
        {
            mod2rmf_encoder_print_codecs();
            return 0;
        }
        if (!strcmp(arg, "--filters"))
        {
            mod2rmf_resampler_print_options();
            return 0;
        }
        if (!strcmp(arg, "--amiga-filter"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --amiga-filter requires an argument\n");
                return 1;
            }
            ++argi;
            if (mod2rmf_resampler_parse_amiga(argv[argi], &resamplerSettings.amigaFilter) != 0)
            {
                fprintf(stderr, "Error: unknown amiga filter '%s' (use --filters to list)\n", argv[argi]);
                return 1;
            }
            continue;
        }
        if (!strcmp(arg, "--resample-rate"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --resample-rate requires an argument\n");
                return 1;
            }
            ++argi;
            {
                long hz = strtol(argv[argi], NULL, 10);
                if (hz < 1000 || hz > 384000)
                {
                    fprintf(stderr, "Error: invalid resample rate '%s'\n", argv[argi]);
                    return 1;
                }
                resamplerSettings.targetRate = (uint32_t)hz;
            }
            continue;
        }
        if (!strcmp(arg, "--resample-filter"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --resample-filter requires an argument\n");
                return 1;
            }
            ++argi;
            if (mod2rmf_resampler_parse_filter(argv[argi], &resamplerSettings.resampleFilter) != 0)
            {
                fprintf(stderr, "Error: unknown resample filter '%s' (use --filters to list)\n", argv[argi]);
                return 1;
            }
            continue;
        }
        if (!strcmp(arg, "--stereo-separation"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --stereo-separation requires an argument (0-100)\n");
                return 1;
            }
            ++argi;
            {
                long val = strtol(argv[argi], NULL, 10);
                if (val < 0 || val > 100)
                {
                    fprintf(stderr, "Error: --stereo-separation must be 0-100 (got '%s')\n", argv[argi]);
                    return 1;
                }
                stereoSeparation = (uint8_t)val;
            }
            continue;
        }
        if (!strcmp(arg, "--codec"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --codec requires an argument\n");
                return 1;
            }
            ++argi;
            if (mod2rmf_encoder_parse_codec(argv[argi], &encSettings.codec) != 0)
            {
                fprintf(stderr, "Error: unknown codec '%s' (use --codecs to list)\n", argv[argi]);
                return 1;
            }
            codecArgSeen = TRUE;
            continue;
        }
        if (!strcmp(arg, "--bitrate"))
        {
            if (argi + 1 >= argc)
            {
                fprintf(stderr, "Error: --bitrate requires an argument\n");
                return 1;
            }
            ++argi;
            if (mod2rmf_encoder_parse_bitrate(argv[argi], &encSettings.bitrateKbps) != 0)
            {
                fprintf(stderr, "Error: invalid bitrate '%s'\n", argv[argi]);
                return 1;
            }
            bitrateArgSeen = TRUE;
            continue;
        }
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h"))
        {
            print_usage(argv[0]);
            return 0;
        }
        if (!sourcePath)
        {
            sourcePath = arg;
            continue;
        }
        if (!destPath)
        {
            destPath = arg;
            continue;
        }
    }

    if (forceOriginalSamples)
    {
        if (codecArgSeen || bitrateArgSeen)
        {
            fprintf(stderr,
                    "Note: --original ignores --codec/--bitrate and forces PCM sample storage.\n");
        }
        encSettings.codec = MOD2RMF_CODEC_PCM;
        encSettings.bitrateKbps = 0;
        resamplerSettings.amigaFilter = MOD2RMF_AMIGA_FILTER_NONE;
        resamplerSettings.targetRate = 0;
    }
    /*
    if (resamplerSettings.targetRate == 11025u)
    {
        // 11025 Hz has shown pitch drift through some codec/decode paths.
        // Snap to 12000 Hz, which aligns with codec-native rate families
        // (notably Opus) and avoids the observed detune.
        fprintf(stderr,
                "Note: --resample-rate 11025 may detune after compression; using 12000 instead.\n");
        resamplerSettings.targetRate = 12000u;
    }
    */

    compressionType = mod2rmf_encoder_resolve(&encSettings);

    if (!sourcePath || !destPath || !file_exists(sourcePath))
    {
        fprintf(stderr, "Error: invalid source or destination path\n");
        return 1;
    }

    /* Vorbis, FLAC, Opus and Opus-RT require the ZMF container. */
    if (mod2rmf_encoder_requires_zmf(encSettings.codec) && !is_zmf_path(destPath))
    {
        fprintf(stderr,
                "Error: %s codec requires ZMF format. "
                "Please use a .zmf output extension.\n",
                mod2rmf_encoder_label(compressionType));
        return 1;
    }

    setupResult = BAE_Setup();
    if (setupResult != BAE_NO_ERROR)
    {
        fprintf(stderr, "Error: BAE_Setup failed (%d)\n", (int)setupResult);
        return 1;
    }

    conv = converter_create();
    if (!conv)
    {
        BAE_Cleanup();
        return 1;
    }

    (void)tempoMap; /* Reserved for future tempo-map handling. */
    useZmfContainer = is_zmf_path(destPath);    
    conv->resamplerSettings = resamplerSettings;
    conv->forceOriginalSamples = forceOriginalSamples;
    conv->stereoSeparation = stereoSeparation;

    if (!load_source_data(conv, sourcePath))
    {
        fprintf(stderr, "Error: failed to read source file\n");
        song_model_dispose(&song);
        converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    {
        struct xmp_test_info testInfo;
        memset(&testInfo, 0, sizeof(testInfo));
        if (xmp_test_module_from_memory(conv->sourceData, (long)conv->sourceSize, &testInfo) != 0)
        {
            fprintf(stderr, "Error: unsupported or invalid tracker module\n");
            song_model_dispose(&song);
            converter_delete(conv);
            BAE_Cleanup();
            return 1;
        }
        fprintf(stderr, "Module detected by libxmp: %s (%s)\n",
                testInfo.name[0] ? testInfo.name : "(untitled)",
                testInfo.type[0] ? testInfo.type : "unknown");
    }

    if (!build_song_model(conv, &song))
    {
        fprintf(stderr, "Error: failed to build song model\n");
        song_model_dispose(&song);
        converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    /* Spread tracker channels by program so each instrument gets its own
     * MIDI channel where possible. Must run before setup_document because
     * it may increase song.channelCount. */
    if (spreadChannels)
    {
        if (!spread_channels_by_program(&song, conv->isMod, conv->stereoSeparation))
        {
            fprintf(stderr, "Error: channel spreading failed\n");
            song_model_dispose(&song);
            converter_delete(conv);
            BAE_Cleanup();
            return 1;
        }
    }

    /* Ensure channels have correct CC state at loop start so the engine's
     * meta-marker loop-back (which doesn't reset CC state) works properly. */
    if (!ensure_loop_cc_resets(&song))
    {
        fprintf(stderr, "Error: loop CC reset failed\n");
        song_model_dispose(&song);
        converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    /* Same for pitch bend — engine keeps bend state across loop-back. */
    if (!ensure_loop_pitch_bend_resets(&song))
    {
        fprintf(stderr, "Error: loop pitch bend reset failed\n");
        song_model_dispose(&song);
        converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

#ifdef _DEBUG
    /* Diagnostic dump: per-virtual-channel event summary (spread mode only) */
    if (spreadChannels)
    {
        uint32_t vch;
        uint32_t maxCh = song.channelCount;
        fprintf(stderr, "\n=== POST-SPREAD EVENT SUMMARY (loopStart=%u loopEnd=%u) ===\n",
                (unsigned)song.loopStartTick, (unsigned)song.loopEndTick);
        for (vch = 0; vch < maxCh; ++vch)
        {
            uint32_t ei;
            uint32_t firstNoteTick = UINT32_MAX, lastNoteTick = 0;
            uint32_t firstNoteCount = 0;
            uint32_t firstCC7Tick = UINT32_MAX, lastCC7Tick = 0;
            uint8_t firstCC7Val = 0, lastCC7Val = 0;
            XBOOL hasCC7 = FALSE;
            uint32_t firstBendTick = UINT32_MAX, lastBendTick = 0;
            uint16_t firstBendVal = 0, lastBendVal = 0;
            XBOOL hasBend = FALSE;

            /* Scan notes */
            for (ei = 0; ei < song.noteCount; ++ei) {
                if (song.notes[ei].sourceChannel == vch) {
                    firstNoteCount++;
                    if (song.notes[ei].startTick < firstNoteTick) firstNoteTick = song.notes[ei].startTick;
                    if (song.notes[ei].startTick > lastNoteTick) lastNoteTick = song.notes[ei].startTick;
                }
            }
            /* Scan CC7 */
            for (ei = 0; ei < song.ccCount; ++ei) {
                if (song.ccEvents[ei].sourceChannel == vch && song.ccEvents[ei].cc == 7) {
                    if (!hasCC7 || song.ccEvents[ei].tick < firstCC7Tick) {
                        firstCC7Tick = song.ccEvents[ei].tick;
                        firstCC7Val = song.ccEvents[ei].value;
                    }
                    if (!hasCC7 || song.ccEvents[ei].tick > lastCC7Tick) {
                        lastCC7Tick = song.ccEvents[ei].tick;
                        lastCC7Val = song.ccEvents[ei].value;
                    }
                    hasCC7 = TRUE;
                }
            }
            /* Scan pitch bend */
            for (ei = 0; ei < song.pitchBendCount; ++ei) {
                if (song.pitchBendEvents[ei].sourceChannel == vch) {
                    if (!hasBend || song.pitchBendEvents[ei].tick < firstBendTick) {
                        firstBendTick = song.pitchBendEvents[ei].tick;
                        firstBendVal = song.pitchBendEvents[ei].value;
                    }
                    if (!hasBend || song.pitchBendEvents[ei].tick > lastBendTick) {
                        lastBendTick = song.pitchBendEvents[ei].tick;
                        lastBendVal = song.pitchBendEvents[ei].value;
                    }
                    hasBend = TRUE;
                }
            }
            if (firstNoteCount == 0) continue;
            fprintf(stderr, "  vCh %u: notes=%u first@%u last@%u", vch, firstNoteCount, firstNoteTick, lastNoteTick);
            if (hasCC7) fprintf(stderr, "  CC7: first=%u@%u last=%u@%u", firstCC7Val, firstCC7Tick, lastCC7Val, lastCC7Tick);
            else fprintf(stderr, "  CC7: NONE");
            if (hasBend) fprintf(stderr, "  Bend: first=0x%04X@%u last=0x%04X@%u", firstBendVal, firstBendTick, lastBendVal, lastBendTick);
            else fprintf(stderr, "  Bend: NONE");
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "=== END EVENT SUMMARY ===\n\n");
    }
#endif

    if (!setup_document(conv, &song, sourcePath))
    {
        fprintf(stderr, "Error: document setup failed\n");
        song_model_dispose(&song);
        converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    /* Set per-song engine config flags. For ZMF the sample-offset-start feature
     * must be announced so the engine activates it on playback. */
    if (useZmfContainer)
    {
        int32_t engineFlags;
        engineFlags = 0;
        BAERmfEditorDocument_GetEngineConfig(conv->document, &engineFlags);
        engineFlags |= SONG_CONFIG_HAS_SAMPLE_OFFSET_START | SONG_CONFIG_SAMPLE_OFFSET_START_ON;
        BAERmfEditorDocument_SetEngineConfig(conv->document, engineFlags);
    }

    /* Emit MIDI loop markers if the song has an infinite loop */
    if (song.loopEnabled)
    {
        BAERmfEditorDocument_SetMidiLoopMarkers(conv->document,
                                                TRUE,
                                                song.loopStartTick,
                                                song.loopEndTick,
                                                -1); /* -1 = loop forever */
        fprintf(stderr, "Loop detected: start=%u end=%u ticks\n",
                (unsigned)song.loopStartTick, (unsigned)song.loopEndTick);
    }

    if (!setup_samples(conv, &song))
    {
        fprintf(stderr, "Error: sample setup failed\n");
        song_model_dispose(&song);
        converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    /* Analyze channel usage and compute tracker→MIDI channel mapping */
    {
        ChannelProfile profiles[MOD2RMF_MAX_CHANNELS];

        analyze_channel_usage(&song, profiles, song.channelCount);
        compute_channel_map(profiles, song.channelCount, &conv->channelMap);

        #ifdef _DEBUG
        {
            uint32_t ci;
            for (ci = 0; ci < song.channelCount; ++ci)
            {
                if (profiles[ci].used)
                {
                    fprintf(stderr, "[mod2rmf] Channel map: tracker ch %u -> MIDI ch %u (%u notes, %u ranges)\n",
                            ci, conv->channelMap.trackerToMidi[ci], profiles[ci].noteCount, profiles[ci].rangeCount);
                }
            }
        }
        #endif

        channel_profile_cleanup(profiles, song.channelCount);
    }

    if (!setup_tracks(conv, &song, &conv->channelMap) ||
        !setup_instrument_ext(conv, &song, useZmfContainer) ||
        !write_song_cc_events(conv, &song) ||
        !write_song_pitch_bend_events(conv, &song) ||
        !write_song_notes(conv, &song) ||
        !write_song_tempo_events(conv, &song) ||
        !mod2rmf_encoder_apply(conv->document, &encSettings, compressionType) ||
        !save_document(conv, destPath))
    {
        fprintf(stderr, "Error: conversion failed\n");
        song_model_dispose(&song);
        converter_delete(conv);
        BAE_Cleanup();
        return 1;
    }

    fprintf(stdout, "Conversion complete: %s -> %s\n", sourcePath, destPath);

    song_model_dispose(&song);
    converter_delete(conv);
    BAE_Cleanup();
    return 0;
}

