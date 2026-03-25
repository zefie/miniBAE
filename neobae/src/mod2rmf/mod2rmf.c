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

#define MOD2RMF_MAX_CHANNELS 16
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
    uint32_t adsrAttackMs;
    uint32_t adsrPeakLevel;     /* 0..VOLUME_RANGE, attack target */
    uint32_t adsrDecayMs;
    uint32_t adsrSustainLevel;  /* 0..VOLUME_RANGE */
    uint32_t adsrReleaseMs;
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
    uint32_t adsrAttackMs;
    uint32_t adsrPeakLevel;    /* 0..VOLUME_RANGE, attack target */
    uint32_t adsrDecayMs;
    uint32_t adsrReleaseMs;
    uint32_t adsrSustainLevel; /* 0..VOLUME_RANGE */
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
} ModCCEvent;

typedef struct {
    uint16_t sourceChannel;
    uint32_t tick;
    uint16_t value; /* 14-bit, center 0x2000 */
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
} ActiveNote;

typedef struct {
    uint8_t retrigInterval;     /* retrigger every N frames (0 = inactive) */
    uint8_t noteDelayFrames;    /* delay note-on by N frames (0 = no delay) */
    XBOOL   hasDelayedNote;     /* a note is pending for this row */
    unsigned char delayedEvNote;/* the note value to trigger after delay */
    int     delayedSid;         /* sample ID for the delayed note */
    uint8_t delayedVolume;      /* volume at time of row start */
} ChannelEffectState;

typedef struct {
    void *sourceData;
    size_t sourceSize;
    ModRawSample *rawSamples;
    uint32_t rawSampleCount;
    uint32_t moduleBaseRateHz;
    XBOOL isMod;

    BAERmfEditorDocument *document;
    uint16_t *channelToTrackIndex;
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
                                      unsigned char value)
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
    song->ccCount++;
    return 1;
}

static int song_model_append_pitch_bend(ModSongModel *song,
                                        uint16_t sourceChannel,
                                        uint32_t tick,
                                        uint16_t value)
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
    /* Trackers don't have a separate "velocity" concept — volume is
     * continuous and already captured as CC7 every frame.  Using max
     * velocity avoids double-attenuation (CC7 × velocity) that would
     * make moderate-volume notes disproportionately quiet.  CC7 is
     * emitted at the same tick before the note-on, so the initial
     * loudness is correct. */
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

static uint16_t libxmp_pitchbend_to_midi(int16_t xmpPitchbend,
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

/* Extract a 4-stage ADSR approximation from a libxmp instrument's amplitude
 * envelope.  Writes the result directly into the raw sample's ADSR fields
 * and sets hasEnvelope = TRUE.
 *
 * Envelope x-coordinates are in ticks (one per tracker frame), converted
 * to milliseconds via 2500/bpm.  y-coordinates are 0..64, scaled to
 * 0..VOLUME_RANGE for BAE. */
static void extract_envelope_adsr(const struct xmp_instrument *inst,
                                  uint32_t bpm,
                                  ModRawSample *raw)
{
    const struct xmp_envelope *aei;
    int peakIdx;
    int sustainIdx;
    int npt;
    int idx;
    uint32_t peakX;
    uint32_t sustainX;
    uint32_t peakY;
    uint32_t sustainY;
    double msPerTick;

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
    msPerTick = 2500.0 / (double)(bpm > 0 ? bpm : 125);

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

    /* Find peak point (highest y between first point and sustain). */
    peakIdx = 0;
    peakY = 0;
    for (idx = 0; idx <= sustainIdx; ++idx)
    {
        uint32_t y = (uint32_t)aei->data[idx * 2 + 1];
        if (y >= peakY)
        {
            peakY = y;
            peakIdx = idx;
        }
    }

    peakX = (uint32_t)aei->data[peakIdx * 2];
    sustainX = (uint32_t)aei->data[sustainIdx * 2];
    sustainY = (uint32_t)aei->data[sustainIdx * 2 + 1];

    /* Attack: time from envelope start to peak. */
    raw->adsrAttackMs = (uint32_t)(peakX * msPerTick + 0.5);
    raw->adsrPeakLevel = (uint32_t)(peakY * VOLUME_RANGE / 64u);

    /* Decay: time from peak to sustain point. */
    raw->adsrDecayMs = (sustainX > peakX)
                         ? (uint32_t)((sustainX - peakX) * msPerTick + 0.5)
                         : 0;

    /* Sustain level. */
    raw->adsrSustainLevel = (uint32_t)(sustainY * VOLUME_RANGE / 64u);

    /* Release: derive from instrument fadeout (rls) or post-sustain points. */
    if (inst->rls > 0)
    {
        /* rls is fadeout per tick (XM: 0..65535).  Time to fade from
         * sustain amplitude to silence = sustain / rls * 65536 ticks
         * (each tick decreases a 16.16 counter by rls). */
        double releaseTicks = (sustainY > 0)
                                ? ((double)sustainY * 1024.0 / (double)inst->rls)
                                : 1.0;
        raw->adsrReleaseMs = (uint32_t)(releaseTicks * msPerTick + 0.5);
    }
    else if (sustainIdx + 1 < npt)
    {
        /* Use post-sustain envelope points as release duration. */
        uint32_t lastX = (uint32_t)aei->data[(npt - 1) * 2];
        raw->adsrReleaseMs = (lastX > sustainX)
                               ? (uint32_t)((lastX - sustainX) * msPerTick + 0.5)
                               : 50;
    }
    else
    {
        raw->adsrReleaseMs = 50; /* reasonable default */
    }

    /* Clamp release so it's not excessively long. */
    if (raw->adsrReleaseMs > 10000)
    {
        raw->adsrReleaseMs = 10000;
    }
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
    uint32_t nextProgram;
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

        raw->loopStart = (s->lps > 0) ? (uint32_t)s->lps : 0u;
        raw->loopEnd = (s->lpe > s->lps) ? (uint32_t)s->lpe : 0u;
        if (raw->loopStart > raw->frameCount) raw->loopStart = 0;
        if (raw->loopEnd > raw->frameCount) raw->loopEnd = raw->frameCount;

        /* Detect loop type from libxmp flags. */
        if (s->flg & XMP_SAMPLE_LOOP_BIDIR)
        {
            raw->loopType = MOD2RMF_LOOP_BIDIR;
        }
        else if (s->flg & XMP_SAMPLE_LOOP_REVERSE)
        {
            raw->loopType = MOD2RMF_LOOP_REVERSE;
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

    nextProgram = 0;
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
             * events that just approximate the envelope curve. */
            {
                uint8_t adjPan;
                adjPan = apply_stereo_separation((uint8_t)ci->pan, ch,
                                                 conv->isMod, conv->stereoSeparation);
                if (adjPan != chLastPan[ch])
                {
                    chLastPan[ch] = adjPan;
                    (void)song_model_append_cc_event(song, (uint16_t)ch, tick, 10,
                                                     (unsigned char)(adjPan >> 1));
                }
            }
            if (ci->volume != chLastVol[ch])
            {
                uint8_t vol64;
                vol64 = (uint8_t)clamp_int((int)ci->volume, 0, 64);
                chLastVol[ch] = vol64;
                (void)song_model_append_cc_event(song, (uint16_t)ch, tick, 7,
                                                 mod_vol_to_midi(vol64));
            }

            if (activeNotes[ch].active)
            {
                uint16_t bend;
                bend = libxmp_pitchbend_to_midi(ci->pitchbend,
                                                song->pitchBendRangeSemitones);
                if (bend != chLastBend[ch])
                {
                    chLastBend[ch] = bend;
                    (void)song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend);
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
                                if (nextProgram >= 128u)
                                {
                                    if (playerStarted) xmp_end_player(ctx);
                                    free(sampleToProgram);
                                    xmp_release_module(ctx);
                                    xmp_free_context(ctx);
                                    return 0;
                                }
                                sampleToProgram[sid] = (int)nextProgram;
                                nextProgram++;
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
                            chLastBend[ch] = 0xFFFFu;
                            {
                                uint16_t bend;
                                bend = libxmp_pitchbend_to_midi(ci->pitchbend,
                                                                song->pitchBendRangeSemitones);
                                chLastBend[ch] = bend;
                                (void)song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend);
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
                            if (nextProgram >= 128u)
                            {
                                if (playerStarted) xmp_end_player(ctx);
                                free(sampleToProgram);
                                xmp_release_module(ctx);
                                xmp_free_context(ctx);
                                return 0;
                            }
                            sampleToProgram[delaySid] = (int)nextProgram;
                            nextProgram++;
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
                        chLastBend[ch] = 0xFFFFu;
                        {
                            uint16_t bend;
                            bend = libxmp_pitchbend_to_midi(ci->pitchbend,
                                                            song->pitchBendRangeSemitones);
                            chLastBend[ch] = bend;
                            (void)song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend);
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
                        bend = libxmp_pitchbend_to_midi(ci->pitchbend,
                                                        song->pitchBendRangeSemitones);
                        chLastBend[ch] = bend;
                        (void)song_model_append_pitch_bend(song, (uint16_t)ch, tick, bend);
                    }
                }
            }
        }

        currentTickFP += tickPerFrameFP;
        frameGuard++;

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
            song->loopEndTick = fp_ticks_to_int(currentTickFP);
            break;
        }
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

    song->playableCount = nextProgram;
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
        if (conv->rawSamples[i].hasEnvelope)
        {
            p->hasVolumeAdsr = TRUE;
            p->adsrAttackMs = conv->rawSamples[i].adsrAttackMs;
            p->adsrPeakLevel = conv->rawSamples[i].adsrPeakLevel;
            p->adsrDecayMs = conv->rawSamples[i].adsrDecayMs;
            p->adsrSustainLevel = conv->rawSamples[i].adsrSustainLevel;
            p->adsrReleaseMs = conv->rawSamples[i].adsrReleaseMs;
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
        setup.rootKey = playable->rootKey;
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
            fprintf(stderr, "Warning: failed to add sample for program %u (%d)\n", (unsigned)setup.program, (int)result);
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
                    fprintf(stderr,
                            "Warning: failed to share sample asset for program %u (%d); falling back to PCM copy\n",
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
                     sampledRate = (BAE_UNSIGNED_FIXED)((double)baseRate * finetuneRatio * 65536.0 + 0.5);
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
                         fprintf(stderr,
                                 "Warning: failed to inject raw 8-bit PCM for program %u (%d)\n",
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
                                "Warning: failed to process sample for program %u\n",
                                (unsigned)setup.program);
                        continue;
                    }

                    if (playable->hasSampleRateOverride)
                    {
                        sampledRate = (BAE_UNSIGNED_FIXED)(outRate << 16);
                    }
                    else
                    {
                        /* finetune is intentionally NOT applied here;
                         * libxmp folds it into ci->pitchbend. */
                        double finetuneRatio = pow(2.0, (double)raw->finetune / 96.0);
                        sampledRate = (BAE_UNSIGNED_FIXED)((double)outRate * finetuneRatio * 65536.0 + 0.5);
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
                                "Warning: failed to inject processed PCM for program %u (%d)\n",
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
                        "Warning: failed to apply loop points for program %u (%d)\n",
                        (unsigned)setup.program,
                        (int)infoResult);
            }
        }
    }

    return 1;
}

static int setup_tracks(Mod2RmfConverter *conv, const ModSongModel *song)
{
    uint32_t i;
    uint32_t channelsToAdd;

    if (!conv || !song)
    {
        return 0;
    }

    channelsToAdd = (song->channelCount < MOD2RMF_MAX_CHANNELS) ? song->channelCount : MOD2RMF_MAX_CHANNELS;
    conv->channelToTrackIndex = (uint16_t *)malloc(song->channelCount * sizeof(uint16_t));
    if (!conv->channelToTrackIndex)
    {
        return 0;
    }
    memset(conv->channelToTrackIndex, 0xFF, song->channelCount * sizeof(uint16_t));

    for (i = 0; i < channelsToAdd; ++i)
    {
        BAERmfEditorTrackSetup setup;
        BAEResult result;
        uint16_t trackIndex;
        char trackName[64];

        memset(&setup, 0, sizeof(setup));
        /* Use all 16 MIDI channels (0..15), including channel 10 (index 9).
         * Channel 10 is switched to melodic mode via NRPN below. */
        {
            unsigned char midiCh = (unsigned char)i;
            // TODO: Smart distrubution of channels > 15 across MIDI channels, instead of just capping.
            // This is especially relevant for S3M which can have 32 channels
            if (midiCh > 15u) midiCh = 15u;
            setup.channel = midiCh;
        }
        setup.bank = MOD2RMF_EMBEDDED_BANK;
        setup.program = 0;
        snprintf(trackName, sizeof(trackName), "Ch %u", i + 1);
        setup.name = trackName;

        result = BAERmfEditorDocument_AddTrack(conv->document, &setup, &trackIndex);
        if (result != BAE_NO_ERROR)
        {
            continue;
        }

        conv->channelToTrackIndex[i] = trackIndex;
        BAERmfEditorDocument_SetTrackDefaultInstrument(conv->document,
                                   trackIndex,
                                   MOD2RMF_EMBEDDED_BANK,
                                   0);

        /* Set pitch bend range to ±12 semitones via RPN */
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

        if (playable->hasVolumeAdsr)
        {
            /* Envelope mapped to ADSR: attack -> decay -> sustain -> release. */
            int32_t peakLevel = (playable->adsrPeakLevel > 0)
                                  ? (int32_t)playable->adsrPeakLevel
                                  : VOLUME_RANGE;
            extInfo.volumeADSR.stageCount = 4;
            extInfo.volumeADSR.stages[0].level = peakLevel;
            extInfo.volumeADSR.stages[0].time = (int32_t)playable->adsrAttackMs;
            extInfo.volumeADSR.stages[0].flags = ADSR_LINEAR_RAMP_LONG;

            extInfo.volumeADSR.stages[1].level = (int32_t)playable->adsrSustainLevel;
            extInfo.volumeADSR.stages[1].time = (int32_t)playable->adsrDecayMs;
            extInfo.volumeADSR.stages[1].flags = ADSR_LINEAR_RAMP_LONG;

            extInfo.volumeADSR.stages[2].level = (int32_t)playable->adsrSustainLevel;
            extInfo.volumeADSR.stages[2].time = 0;
            extInfo.volumeADSR.stages[2].flags = ADSR_SUSTAIN_LONG;

            extInfo.volumeADSR.stages[3].level = 0;
            extInfo.volumeADSR.stages[3].time = (int32_t)playable->adsrReleaseMs;
            extInfo.volumeADSR.stages[3].flags = ADSR_TERMINATE_LONG;
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
        if (note->sourceChannel >= song->channelCount)
        {
            continue;
        }
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

    if (!conv || !song)
    {
        return 0;
    }

    for (i = 0; i < song->ccCount; ++i)
    {
        const ModCCEvent *ev;
        uint16_t trackIndex;

        ev = &song->ccEvents[i];
        if (ev->sourceChannel >= song->channelCount)
        {
            continue;
        }
        trackIndex = conv->channelToTrackIndex[ev->sourceChannel];
        if (trackIndex == (uint16_t)0xFFFF)
        {
            continue;
        }

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

    if (!conv || !song)
    {
        return 0;
    }

    for (i = 0; i < song->pitchBendCount; ++i)
    {
        const ModPitchBendEvent *ev;
        uint16_t trackIndex;

        ev = &song->pitchBendEvents[i];
        if (ev->sourceChannel >= song->channelCount)
        {
            continue;
        }
        trackIndex = conv->channelToTrackIndex[ev->sourceChannel];
        if (trackIndex == (uint16_t)0xFFFF)
        {
            continue;
        }

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
                "Error: document requires ZMF format due to RMF-incompatible sample data \n"
                "(modern codec, advanced interpolation, engine config, or loop shorter than %u frames). \n"
                "Please use a .zmf output extension.\n",
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
        fprintf(stderr, "Error: save failed (%d): %s\n", (int)result, destPath);
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
    uint8_t stereoSeparation;

    sourcePath = NULL;
    destPath = NULL;
    tempoMap = 0;
    mod2rmf_encoder_defaults(&encSettings);
    mod2rmf_resampler_defaults(&resamplerSettings);
    forceOriginalSamples = FALSE;
    codecArgSeen = FALSE;
    bitrateArgSeen = FALSE;
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

    if (!setup_samples(conv, &song) ||
        !setup_tracks(conv, &song) ||
        !setup_instrument_ext(conv, &song, useZmfContainer) ||
        !write_song_cc_events(conv, &song) ||
        !write_song_notes(conv, &song) ||
        !write_song_pitch_bend_events(conv, &song) ||
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

