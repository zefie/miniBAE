/****************************************************************************
 *
 * mod2rmf.c
 *
 * Native MOD/S3M -> RMF converter
 * Supports ProTracker/SoundTracker MOD and Scream Tracker 3 S3M formats.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>

#include <NeoBAE.h>
#include <X_Formats.h>

#include "mod2rmf_mod.h"
#include "mod2rmf_s3m.h"
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
#define MOD2RMF_PITCH_BEND_RANGE_ST     12
#define MOD2RMF_MOD_PERIOD_MIN          113
#define MOD2RMF_MOD_PERIOD_MAX          1712

typedef struct {
    XBOOL valid;
    char name[23];
    uint32_t frameCount;
    uint32_t loopStart;
    uint32_t loopEnd;
    unsigned char rootKey;
    unsigned char defaultVolume; /* 0..64 from MOD sample header */
    int8_t  finetune;           /* -8..+7 from MOD sample header */
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
                                                const Mod2RmfResamplerSettings *settings)
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

    srcRate = MOD2RMF_SAMPLE_RATE;
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
    uint16_t period;          /* current base period (modified by porta) */
    uint16_t targetPeriod;    /* tone porta target */
    uint16_t noteBasePeriod;  /* period at last note-on (for pitch bend ref) */
    unsigned char volume;     /* 0..64 */
    unsigned char panning;    /* 0..255 */
    unsigned char program;

    /* Effect memory (reused when param==0) */
    uint8_t portaUpSpeed;
    uint8_t portaDownSpeed;
    uint8_t tonePortaSpeed;
    uint8_t vibratoSpeed;
    uint8_t vibratoDepth;
    uint8_t tremoloSpeed;
    uint8_t tremoloDepth;
    uint8_t arpeggioParam;
    uint8_t sampleOffset;
    uint8_t retriggerParam;
    int8_t  finetune;
    uint8_t glissando;

    /* Oscillators */
    uint8_t vibratoPos;
    uint8_t vibratoWaveform;  /* 0..3, bit 2 = no retrig on note */
    uint8_t tremoloPos;
    uint8_t tremoloWaveform;

    /* Pattern loop (per-channel in PT) */
    uint32_t loopStartRow;
    uint8_t  loopCount;

    /* Dedup tracking */
    uint16_t lastPitchBend;
    unsigned char lastCCVolume;
    unsigned char lastCCPanning;
} ChannelState;

typedef struct {
    void *sourceData;
    size_t sourceSize;
    ModFormat format;
    ModHeader header;
    ModRawSample *rawSamples;
    unsigned char *slotToProgram;
    XBOOL enableZmfSampleOffset;

    BAERmfEditorDocument *document;
    uint16_t *channelToTrackIndex;
    Mod2RmfResamplerSettings resamplerSettings;
    XBOOL forceOriginalSamples;
    uint8_t stereoSeparation;  /* 0=mono (center), 75=default, 100=hard L/R */
} Mod2RmfConverter;

static void print_usage(const char *program_name)
{
    fprintf(stderr,
            "Usage: %s [options] <source.mod|source.s3m> <dest.rmf|dest.zmf>\n"
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

static uint16_t read_be16(const unsigned char *ptr)
{
    return (uint16_t)(((uint16_t)ptr[0] << 8) | (uint16_t)ptr[1]);
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

/* Standard ProTracker period table: C-1 (index 0) through B-5 (index 59).
 * MIDI note = 36 + index. */
static const uint16_t gPeriodTable[] = {
    1712,1616,1524,1440,1356,1280,1208,1140,1076,1016, 960, 907,
     856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
     428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
     214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113,
     107, 101,  95,  90,  85,  80,  75,  71,  67,  63,  60,  56
};
#define PERIOD_TABLE_SIZE (sizeof(gPeriodTable) / sizeof(gPeriodTable[0]))

/* Half-wave sine table (0..255) for vibrato/tremolo oscillators. */
static const uint8_t gSineTable[32] = {
      0,  24,  49,  74,  97, 120, 141, 161,
    180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197,
    180, 161, 141, 120,  97,  74,  49,  24
};

static int16_t get_waveform_value(uint8_t waveform, uint8_t pos)
{
    int16_t val;
    pos &= 63u;
    switch (waveform & 3u)
    {
        case 0: /* sine */
            val = (int16_t)gSineTable[pos & 31u];
            if (pos >= 32u) val = -val;
            break;
        case 1: /* ramp down */
            val = (int16_t)(255 - (int16_t)(pos & 63u) * 8);
            break;
        case 2: /* square */
            val = (pos < 32u) ? 255 : -255;
            break;
        default: /* random (approximate with sine) */
            val = (int16_t)gSineTable[pos & 31u];
            if (pos >= 32u) val = -val;
            break;
    }
    return val;
}

static uint16_t period_to_pitch_bend_range(uint16_t basePeriod,
                                           uint16_t currentPeriod,
                                           uint16_t rangeSemitones);

static uint16_t period_to_pitch_bend(uint16_t basePeriod, uint16_t currentPeriod)
{
    return period_to_pitch_bend_range(basePeriod, currentPeriod, MOD2RMF_PITCH_BEND_RANGE_ST);
}

static uint16_t period_to_pitch_bend_range(uint16_t basePeriod,
                                           uint16_t currentPeriod,
                                           uint16_t rangeSemitones)
{
    double cents;
    int32_t bend;

    if (basePeriod == 0 || currentPeriod == 0)
    {
        return MOD2RMF_PITCH_BEND_CENTER;
    }
    if (rangeSemitones == 0)
    {
        rangeSemitones = MOD2RMF_PITCH_BEND_RANGE_ST;
    }
    /* cents > 0 = pitch up (current period smaller = higher freq) */
    cents = 1200.0 * log2((double)basePeriod / (double)currentPeriod);
    bend = (int32_t)(MOD2RMF_PITCH_BEND_CENTER +
                     cents * (double)MOD2RMF_PITCH_BEND_CENTER /
                     ((double)rangeSemitones * 100.0));
    if (bend < 0) bend = 0;
    if (bend > 0x3FFF) bend = 0x3FFF;
    return (uint16_t)bend;
}

static void clamp_period(uint16_t *period)
{
    if (*period < MOD2RMF_MOD_PERIOD_MIN)
    {
        *period = MOD2RMF_MOD_PERIOD_MIN;
    }
    if (*period > MOD2RMF_MOD_PERIOD_MAX)
    {
        *period = MOD2RMF_MOD_PERIOD_MAX;
    }
}

static int period_to_midi(uint16_t period, unsigned char *outMidi)
{
    uint32_t i;
    uint32_t bestIndex;
    uint32_t bestDiff;

    if (!outMidi || period == 0)
    {
        return 0;
    }

    bestIndex = 0;
    bestDiff = 0xFFFFFFFFu;

    for (i = 0; i < (uint32_t)PERIOD_TABLE_SIZE; ++i)
    {
        uint32_t diff;
        diff = (gPeriodTable[i] > period)
             ? (uint32_t)(gPeriodTable[i] - period)
             : (uint32_t)(period - gPeriodTable[i]);
        if (diff < bestDiff)
        {
            bestDiff = diff;
            bestIndex = i;
        }
    }

    /* Map C-1 table start one octave higher for embedded sample root-key
     * defaults; sample rate tuning handles the remaining alignment. */
    {
        uint32_t midi = 36u + bestIndex;
        if (midi > 127u)
        {
            midi = 127u;
        }
        *outMidi = (unsigned char)midi;
    }
    return 1;
}

static int song_model_append_pitch_bend(ModSongModel *song,
                                        uint16_t sourceChannel,
                                        uint32_t tick,
                                        uint16_t value);

static uint16_t midi_note_to_mod_period(unsigned char midiNote)
{
    double semitones;
    double period;
    int32_t p;

    semitones = (double)((int)midiNote - 36);
    period = 1712.0 * pow(2.0, -semitones / 12.0);
    p = (int32_t)(period + 0.5);
    if (p < (int32_t)MOD2RMF_MOD_PERIOD_MIN)
    {
        p = MOD2RMF_MOD_PERIOD_MIN;
    }
    if (p > (int32_t)MOD2RMF_MOD_PERIOD_MAX)
    {
        p = MOD2RMF_MOD_PERIOD_MAX;
    }
    return (uint16_t)p;
}

static void append_pitch_bend_if_changed(ModSongModel *song,
                                         uint16_t sourceChannel,
                                         uint32_t tick,
                                         uint16_t noteBasePeriod,
                                         uint16_t effectivePeriod,
                                         uint16_t bendRangeSemitones,
                                         uint16_t *lastPitchBend)
{
    uint16_t bend;

    if (!song || !lastPitchBend)
    {
        return;
    }

    bend = period_to_pitch_bend_range(noteBasePeriod,
                                      effectivePeriod,
                                      bendRangeSemitones);
    if (bend == *lastPitchBend)
    {
        return;
    }
    *lastPitchBend = bend;
    (void)song_model_append_pitch_bend(song, sourceChannel, tick, bend);
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

static int materialize_playables(Mod2RmfConverter *conv, ModSongModel *song)
{
    const ModHeader *h;
    uint32_t used;
    uint32_t i;

    if (!conv || !song)
    {
        return 0;
    }

    h = &conv->header;
    used = 0;
    for (i = 0; i < h->sampleCount; ++i)
    {
        if (conv->slotToProgram[i] != 0xFF)
        {
            used++;
        }
    }

    song->playableCount = used;
    if (used)
    {
        song->playables = (ModPlayable *)calloc(used, sizeof(ModPlayable));
        if (!song->playables)
        {
            return 0;
        }
    }

    for (i = 0; i < h->sampleCount; ++i)
    {
        unsigned char program;
        ModPlayable *playable;
        if (conv->slotToProgram[i] == 0xFF)
        {
            continue;
        }
        program = conv->slotToProgram[i];
        playable = &song->playables[program];
        playable->sourceSlot = i;
        playable->program = program;
        playable->rootKey = 60;
        playable->sampleOffsetBytes = 0;
        playable->offsetVariant = FALSE;
        playable->rawSample = &conv->rawSamples[i];
        if (conv->rawSamples[i].name[0])
        {
            snprintf(playable->displayName, sizeof(playable->displayName), "%s", conv->rawSamples[i].name);
        }
        else
        {
            snprintf(playable->displayName, sizeof(playable->displayName), "Sample %u", i + 1);
        }
    }

    return 1;
}

static ModPlayable *find_playable_by_program(ModSongModel *song, unsigned char program)
{
    uint32_t i;

    if (!song)
    {
        return NULL;
    }
    for (i = 0; i < song->playableCount; ++i)
    {
        if (song->playables[i].program == program)
        {
            return &song->playables[i];
        }
    }
    return NULL;
}

static unsigned char ensure_sample_offset_variant_program(Mod2RmfConverter *conv,
                                                          ModSongModel *song,
                                                          unsigned char baseProgram,
                                                          uint32_t offsetBytes)
{
    ModPlayable *base;
    ModPlayable *newPlayables;
    ModPlayable *v;
    uint32_t i;
    uint32_t newIndex;

    if (!conv || !song || offsetBytes == 0)
    {
        return baseProgram;
    }

    base = find_playable_by_program(song, baseProgram);
    if (!base || !base->rawSample || !base->rawSample->valid || !base->rawSample->pcm8)
    {
        return baseProgram;
    }
    if (offsetBytes >= base->rawSample->frameCount)
    {
        return baseProgram;
    }

    for (i = 0; i < song->playableCount; ++i)
    {
        v = &song->playables[i];
        if (v->sourceSlot == base->sourceSlot &&
            v->offsetVariant &&
            v->sampleOffsetBytes == offsetBytes)
        {
            return v->program;
        }
    }

    if (song->playableCount >= 128u)
    {
        return baseProgram;
    }

    newPlayables = (ModPlayable *)realloc(song->playables,
                                          (song->playableCount + 1u) * sizeof(ModPlayable));
    if (!newPlayables)
    {
        return baseProgram;
    }
    song->playables = newPlayables;

    newIndex = song->playableCount;
    v = &song->playables[newIndex];
    memset(v, 0, sizeof(*v));
    v->sourceSlot = base->sourceSlot;
    v->program = (unsigned char)newIndex;
    v->rootKey = base->rootKey;
    v->sampleOffsetBytes = offsetBytes;
    v->offsetVariant = TRUE;
    v->rawSample = base->rawSample;
    snprintf(v->displayName,
             sizeof(v->displayName),
             "%s +ofs%u",
             base->displayName,
             (unsigned)offsetBytes);

    song->playableCount++;
    return v->program;
}

static uint64_t mod_row_ticks_fp(uint8_t speed, uint16_t bpm)
{
    (void)bpm;

    if (speed == 0)
    {
        speed = 6;
    }

    /* Tracker speed controls ticks-per-row. BPM is emitted separately as
     * tempo events, so applying BPM here would double-scale time. */
    return (((uint64_t)MOD2RMF_ROW_TICKS * (uint64_t)speed) << 16) /
           (uint64_t)6u;
}

/* Scale a period delta from S3M c2spd-dependent period space to our
 * raw Amiga period space.  S3M porta/vibrato speeds are designed for
 * periods proportional to 8363/c2spd; our periods assume c2spd=8363,
 * so deltas need to be multiplied by c2spd/8363. */
static int32_t s3m_scale_delta(int32_t delta, uint32_t c2spd)
{
    int64_t scaled;
    if (c2spd == 0u || c2spd == 8363u)
        return delta;
    scaled = (int64_t)delta * (int64_t)c2spd;
    if (scaled >= 0)
        return (int32_t)((scaled + 4181) / 8363);
    return (int32_t)((scaled - 4181) / 8363);
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
    free(conv->slotToProgram);
    free(conv->channelToTrackIndex);

    if (conv->rawSamples)
    {
        for (i = 0; i < conv->header.sampleCount; ++i)
        {
            free(conv->rawSamples[i].pcm8);
        }
        free(conv->rawSamples);
    }

    free(conv);
}

static int parse_mod_header(Mod2RmfConverter *conv)
{
    const unsigned char *data;
    size_t size;
    ModHeader *h;
    ModHeader candidate;
    uint32_t channels;

    if (!conv || !conv->sourceData)
    {
        return 0;
    }

    data = (const unsigned char *)conv->sourceData;
    size = conv->sourceSize;
    h = &conv->header;
    memset(h, 0, sizeof(*h));

    conv->format = mod2rmf_detect_mod_format(data, size);

    /* Prefer explicit 31-sample signatures; fallback to 15-sample layout when invalid. */
    channels = 0;
    if (size >= 1084)
    {
        channels = mod2rmf_detect_mod_channel_count(data + 1080);
    }
    if (channels >= 1 && channels <= 32 &&
        mod2rmf_try_parse_layout(&candidate,
                     data,
                     size,
                     31,
                     (unsigned char)channels,
                     950,
                     951,
                     952,
                     1084,
                     MOD2RMF_MAX_SAMPLES))
    {
        *h = candidate;
        conv->format = MOD_FORMAT_31;
        return 1;
    }

    if (size >= 600 && mod2rmf_try_parse_layout(&candidate,
                                                data,
                                                size,
                                                15,
                                                4,
                                                470,
                                                471,
                                                472,
                                                600,
                                                MOD2RMF_MAX_SAMPLES))
    {
        *h = candidate;
        conv->format = MOD_FORMAT_15;
        return 1;
    }

    conv->format = MOD_FORMAT_UNKNOWN;
    return 0;
}

static int extract_mod_samples(Mod2RmfConverter *conv)
{
    const unsigned char *data;
    size_t size;
    const ModHeader *h;
    uint32_t i;
    size_t sampleDataCursor;
    size_t sampleHeaderBase;

    if (!conv || !conv->sourceData)
    {
        return 0;
    }

    data = (const unsigned char *)conv->sourceData;
    size = conv->sourceSize;
    h = &conv->header;

    conv->rawSamples = (ModRawSample *)calloc(h->sampleCount, sizeof(ModRawSample));
    if (!conv->rawSamples)
    {
        return 0;
    }

    sampleDataCursor = h->sampleDataOffset;
    sampleHeaderBase = 20;

    for (i = 0; i < h->sampleCount; ++i)
    {
        const unsigned char *sh;
        uint32_t sampleBytes;
        uint32_t loopStart;
        uint32_t repeatLength;
        uint32_t f;
        ModRawSample *sample;

        sh = data + sampleHeaderBase + i * 30u;
        sample = &conv->rawSamples[i];

        memset(sample->name, 0, sizeof(sample->name));
        memcpy(sample->name, sh, 22);
        sample->name[22] = '\0';
        sample->rootKey = 60;
        {
            int8_t ft;
            ft = (int8_t)(sh[24] & 0x0Fu);
            if (ft > 7) ft -= 16;
            sample->finetune = ft;
        }
        sample->defaultVolume = (sh[25] > 64u) ? 64u : sh[25];

        sampleBytes = (uint32_t)read_be16(sh + 22) * 2u;
        loopStart = (uint32_t)read_be16(sh + 26) * 2u;
        repeatLength = (uint32_t)read_be16(sh + 28) * 2u;

        if (sampleBytes == 0)
        {
            continue;
        }
        if (sampleDataCursor + sampleBytes > size)
        {
            return 0;
        }

        sample->pcm8 = (int8_t *)malloc(sampleBytes);
        if (!sample->pcm8)
        {
            return 0;
        }

        for (f = 0; f < sampleBytes; ++f)
        {
            /* MOD stores 8-bit sample data as signed PCM; engine expects unsigned 8-bit PCM. */
            sample->pcm8[f] = (int8_t)(data[sampleDataCursor + f] ^ 0x80u);
        }

        sample->frameCount = sampleBytes;
        sample->loopStart = (loopStart < sampleBytes) ? loopStart : 0;
        sample->loopEnd = (repeatLength > 2 && loopStart < sampleBytes)
                        ? ((loopStart + repeatLength <= sampleBytes) ? (loopStart + repeatLength) : sampleBytes)
                        : 0;
        sample->valid = TRUE;

        sampleDataCursor += sampleBytes;
    }

    return 1;
}

static int build_slot_program_map(Mod2RmfConverter *conv)
{
    const ModHeader *h;
    const unsigned char *data;
    uint32_t nextProgram;
    uint32_t orderIndex;

    if (!conv || !conv->sourceData)
    {
        return 0;
    }

    h = &conv->header;
    data = (const unsigned char *)conv->sourceData;

    conv->slotToProgram = (unsigned char *)malloc(h->sampleCount);
    if (!conv->slotToProgram)
    {
        return 0;
    }
    memset(conv->slotToProgram, 0xFF, h->sampleCount);

    nextProgram = 0;
    for (orderIndex = 0; orderIndex < h->songLength; ++orderIndex)
    {
        uint32_t pattern;
        uint32_t row;

        pattern = h->orders[orderIndex];
        if (pattern >= h->patternCount)
        {
            continue;
        }
        for (row = 0; row < 64; ++row)
        {
            uint32_t channel;
            for (channel = 0; channel < h->channelCount; ++channel)
            {
                size_t cellOffset;
                const unsigned char *cell;
                uint8_t sampleNum;

                cellOffset = h->patternDataOffset +
                             (size_t)pattern * 64u * h->channelCount * 4u +
                             (size_t)row * h->channelCount * 4u +
                             (size_t)channel * 4u;
                cell = data + cellOffset;
                sampleNum = (uint8_t)((cell[0] & 0xF0u) | ((cell[2] & 0xF0u) >> 4));

                if (sampleNum == 0 || sampleNum > h->sampleCount)
                {
                    continue;
                }
                if (!conv->rawSamples[sampleNum - 1].valid ||
                    !conv->rawSamples[sampleNum - 1].pcm8 ||
                    conv->rawSamples[sampleNum - 1].frameCount == 0)
                {
                    continue;
                }
                if (conv->slotToProgram[sampleNum - 1] != 0xFF)
                {
                    continue;
                }
                if (nextProgram >= 128)
                {
                    fprintf(stderr, "Error: too many used sample slots for MIDI program mapping\n");
                    return 0;
                }
                conv->slotToProgram[sampleNum - 1] = (unsigned char)nextProgram;
                nextProgram++;
            }
        }
    }

    return 1;
}

static void init_channel_state(ChannelState *ch)
{
    memset(ch, 0, sizeof(*ch));
    ch->volume = 64;
    ch->panning = 128;
    ch->program = 0xFF;
    ch->lastPitchBend = MOD2RMF_PITCH_BEND_CENTER;
    ch->lastCCVolume = 0xFF;
    ch->lastCCPanning = 0xFF;
}

static unsigned char mod_vol_to_midi(unsigned char vol64)
{
    /* MOD volume 0..64 → MIDI 0..127, capped to prevent overflow */
    if (vol64 >= 64u) return 127u;
    return (unsigned char)(vol64 * 2u);
}

static unsigned char note_velocity_from_volume(unsigned char vol64)
{
    unsigned char v;
    v = mod_vol_to_midi(vol64);
    /* Keep note-on semantic even for near-silent starts. */
    return (v == 0u) ? 1u : v;
}

static BAE_UNSIGNED_FIXED mod_sample_rate_from_finetune_fixed(int8_t finetune)
{
    double ratio;
    double rate;

    /* ProTracker finetune step is 1/8 semitone. */
    ratio = pow(2.0, (double)finetune / 96.0);
    rate = (double)MOD2RMF_SAMPLE_RATE * ratio;
    if (rate < 1.0)
    {
        rate = 1.0;
    }
    return (BAE_UNSIGNED_FIXED)(rate * 65536.0 + 0.5);
}

static void emit_volume_cc(ModSongModel *song, uint16_t channel,
                           ChannelState *ch, uint32_t tick, unsigned char vol64)
{
    unsigned char midiVol;
    midiVol = mod_vol_to_midi(vol64);
    if (midiVol == ch->lastCCVolume)
    {
        return;
    }
    ch->lastCCVolume = midiVol;
    song_model_append_cc_event(song, channel, tick, 7, midiVol);
}

static void emit_panning_cc(ModSongModel *song, uint16_t channel,
                            ChannelState *ch, uint32_t tick, unsigned char pan255)
{
    unsigned char midiPan;
    midiPan = (unsigned char)(pan255 >> 1);
    if (midiPan == ch->lastCCPanning)
    {
        return;
    }
    ch->lastCCPanning = midiPan;
    song_model_append_cc_event(song, channel, tick, 10, midiPan);
}

static void emit_pitch_bend(ModSongModel *song, uint16_t channel,
                            ChannelState *ch, uint32_t tick)
{
    uint16_t bend;
    bend = period_to_pitch_bend(ch->noteBasePeriod, ch->period);
    if (bend == ch->lastPitchBend)
    {
        return;
    }
    ch->lastPitchBend = bend;
    song_model_append_pitch_bend(song, channel, tick, bend);
}

static void do_vibrato(ChannelState *ch)
{
    /* Vibrato modifies effective pitch but not the base period.
     * The caller should compute the effective period as:
     *   effectivePeriod = ch->period + vibratoDelta
     * We store the delta separately so it doesn't permanently alter ch->period. */
}

static int16_t get_vibrato_delta(const ChannelState *ch)
{
    int16_t waveVal;
    waveVal = get_waveform_value(ch->vibratoWaveform, ch->vibratoPos);
    return (int16_t)((waveVal * (int16_t)ch->vibratoDepth) / 128);
}

static int16_t get_tremolo_delta(const ChannelState *ch)
{
    int16_t waveVal;
    waveVal = get_waveform_value(ch->tremoloWaveform, ch->tremoloPos);
    return (int16_t)((waveVal * (int16_t)ch->tremoloDepth) / 64);
}

static void emit_pitch_with_vibrato(ModSongModel *song, uint16_t channel,
                                    ChannelState *ch, uint32_t tick)
{
    int32_t effectivePeriod;
    uint16_t savedPeriod;
    int16_t vibDelta;

    vibDelta = get_vibrato_delta(ch);
    effectivePeriod = (int32_t)ch->period + vibDelta;
    if (effectivePeriod < MOD2RMF_MOD_PERIOD_MIN)
    {
        effectivePeriod = MOD2RMF_MOD_PERIOD_MIN;
    }
    if (effectivePeriod > MOD2RMF_MOD_PERIOD_MAX)
    {
        effectivePeriod = MOD2RMF_MOD_PERIOD_MAX;
    }

    savedPeriod = ch->period;
    ch->period = (uint16_t)effectivePeriod;
    emit_pitch_bend(song, channel, ch, tick);
    ch->period = savedPeriod;
}

static void emit_volume_with_tremolo(ModSongModel *song, uint16_t channel,
                                     ChannelState *ch, uint32_t tick)
{
    int16_t effectiveVol;
    effectiveVol = (int16_t)ch->volume + get_tremolo_delta(ch);
    if (effectiveVol < 0) effectiveVol = 0;
    if (effectiveVol > 64) effectiveVol = 64;
    emit_volume_cc(song, channel, ch, tick, (unsigned char)effectiveVol);
}

static void do_volume_slide(ChannelState *ch, uint8_t param)
{
    uint8_t up, down;
    int16_t vol;

    up = (uint8_t)((param >> 4) & 0x0Fu);
    down = (uint8_t)(param & 0x0Fu);
    vol = (int16_t)ch->volume;

    if (up > 0)
    {
        vol += up;
    }
    else
    {
        vol -= down;
    }
    if (vol < 0) vol = 0;
    if (vol > 64) vol = 64;
    ch->volume = (unsigned char)vol;
}

static void do_porta_up(ChannelState *ch, uint8_t speed)
{
    int32_t p;
    p = (int32_t)ch->period - (int32_t)speed;
    if (p < MOD2RMF_MOD_PERIOD_MIN) p = MOD2RMF_MOD_PERIOD_MIN;
    ch->period = (uint16_t)p;
}

static void do_porta_down(ChannelState *ch, uint8_t speed)
{
    int32_t p;
    p = (int32_t)ch->period + (int32_t)speed;
    if (p > MOD2RMF_MOD_PERIOD_MAX) p = MOD2RMF_MOD_PERIOD_MAX;
    ch->period = (uint16_t)p;
}

static void do_tone_porta(ChannelState *ch)
{
    if (ch->period < ch->targetPeriod)
    {
        ch->period += ch->tonePortaSpeed;
        if (ch->period > ch->targetPeriod)
        {
            ch->period = ch->targetPeriod;
        }
    }
    else if (ch->period > ch->targetPeriod)
    {
        int32_t p;
        p = (int32_t)ch->period - (int32_t)ch->tonePortaSpeed;
        if (p < (int32_t)ch->targetPeriod)
        {
            p = (int32_t)ch->targetPeriod;
        }
        ch->period = (uint16_t)p;
    }
    clamp_period(&ch->period);
}

/* Re-anchor the MIDI note when tone portamento has slid the current period
 * to a different MIDI note than the one actively playing.  Without this,
 * sustained tone porta can exceed the ±12 semitone pitch-bend range and
 * the pitch saturates at the wrong value.
 *
 * Call this AFTER do_tone_porta() + emit_pitch_bend() on every tone-porta
 * tick (effects 0x03/0x05 for MOD, G/L for S3M).  Returns 0 on alloc fail. */
static int maybe_reanchor_tone_porta(ModSongModel *song,
                                     uint16_t channel,
                                     ChannelState *ch,
                                     ActiveNote *note,
                                     uint64_t tickPosFP,
                                     uint32_t midiTick)
{
    unsigned char newMidi;

    if (!note->active || ch->period == 0 || ch->noteBasePeriod == 0)
    {
        return 1;
    }

    if (!period_to_midi(ch->period, &newMidi))
    {
        return 1;
    }

    /* When the slide has reached its target, snap noteBasePeriod so the
     * pitch bend resolves to the exact target pitch (avoids ~50 cent drift
     * from the re-anchor boundary not aligning with the target period). */
    if (ch->period == ch->targetPeriod && ch->noteBasePeriod != ch->period)
    {
        if (newMidi == note->note)
        {
            /* Same MIDI note — just update the base and correct the bend */
            ch->noteBasePeriod = ch->period;
            ch->lastPitchBend = MOD2RMF_PITCH_BEND_CENTER;
            song_model_append_pitch_bend(song, channel, midiTick, MOD2RMF_PITCH_BEND_CENTER);
            return 1;
        }
    }

    /* If the MIDI note hasn't changed, no re-anchor needed */
    if (newMidi == note->note)
    {
        return 1;
    }

    /* Flush the old note, ending at the current tick (no gap).
     * The engine will cut the old voice when the new note-on arrives
     * on the same channel, so overlapping by 1 tick is fine. */
    if (!flush_active_note(song, channel, note, tickPosFP))
    {
        return 0;
    }

    /* Update base period and reset pitch bend */
    ch->noteBasePeriod = ch->period;
    ch->lastPitchBend = MOD2RMF_PITCH_BEND_CENTER;
    song_model_append_pitch_bend(song, channel, midiTick, MOD2RMF_PITCH_BEND_CENTER);

    /* Emit volume so the new note inherits the current level */
    emit_volume_cc(song, channel, ch, midiTick, ch->volume);

    /* Start new note at the correct MIDI pitch */
    note->active = TRUE;
    note->startTickFP = tickPosFP;
    note->note = newMidi;
    note->velocity = 127;
    note->program = ch->program;

    return 1;
}

static int build_song_model_native(Mod2RmfConverter *conv, ModSongModel *song)
{
    const ModHeader *h;
    const unsigned char *data;
    ActiveNote activeNotes[MOD2RMF_MAX_CHANNELS];
    ChannelState channels[MOD2RMF_MAX_CHANNELS];
    uint64_t currentTickFP;
    uint8_t speed;
    uint16_t bpm;
    uint32_t i;
    uint32_t orderPos;
    uint32_t rowPos;
    uint32_t rowsProcessed;
    uint32_t maxRows;

    if (!conv || !song || !conv->sourceData)
    {
        return 0;
    }

    h = &conv->header;
    data = (const unsigned char *)conv->sourceData;

    song_model_init(song);
    snprintf(song->moduleName, sizeof(song->moduleName), "%s",
             h->title[0] ? h->title : "Untitled MOD");
    song->channelCount = h->channelCount;
    song->bpm = 125;
    speed = 6;
    bpm = 125;

    for (i = 0; i < MOD2RMF_MAX_CHANNELS; ++i)
    {
        activeNotes[i].active = FALSE;
        activeNotes[i].startTickFP = 0;
        init_channel_state(&channels[i]);
    }

    /* Apply classic Amiga stereo separation (LRRL pattern).
     * stereoSeparation: 0=mono (all center), 75=default, 100=hard L/R.
     * Channels follow the Amiga hardware layout: L R R L repeating. */
    {
        static const int amigaSide[4] = { -1, 1, 1, -1 }; /* L R R L */
        uint32_t ch;
        int sep = (int)conv->stereoSeparation;
        for (ch = 0; ch < h->channelCount && ch < MOD2RMF_MAX_CHANNELS; ++ch)
        {
            /* sep=100: offset=127 → left=1, right=255 (hard pan)
             * sep=75:  offset=95  → left=33, right=223 (~73%)
             * sep=0:   offset=0   → all center (128) */
            int offset = (127 * sep) / 100;
            int pan = 128 + amigaSide[ch % 4u] * offset;
            if (pan < 0) pan = 0;
            if (pan > 255) pan = 255;
            channels[ch].panning = (unsigned char)pan;
        }
    }

    if (!materialize_playables(conv, song))
    {
        return 0;
    }

    /* Collect source-order sample names into metadata notes. */
    {
        uint32_t si;
        char clean[64];

        song->composerNotes[0] = '\0';
        for (si = 0; si < h->sampleCount; ++si)
        {
            trim_copy_ascii(clean, sizeof(clean), conv->rawSamples[si].name);
            if (!clean[0])
            {
                (void)snprintf(clean, sizeof(clean), "%s", "");
            }
            append_linef(song->composerNotes, sizeof(song->composerNotes), clean);
        }
    }

    /* Emit initial panning (CC#10) at tick 0 for all channels. */
    {
        uint32_t ch;
        for (ch = 0; ch < h->channelCount && ch < MOD2RMF_MAX_CHANNELS; ++ch)
        {
            emit_panning_cc(song, (uint16_t)ch, &channels[ch], 0, channels[ch].panning);
        }
    }

    currentTickFP = 0;
    orderPos = 0;
    rowPos = 0;
    rowsProcessed = 0;
    maxRows = (uint32_t)h->songLength * 64u * 4u;
    if (maxRows < 1024u)
    {
        maxRows = 1024u;
    }

    /* Order visit tracking for song-loop detection.
     * When a backward jump (Bxx) targets an already-visited order at row 0,
     * the song has looped and we stop. */
    {
        uint8_t orderVisited[128];
        uint64_t orderStartTickFP[128]; /* FP tick value at which each order began */
        memset(orderVisited, 0, sizeof(orderVisited));
        {
            uint32_t oi;
            for (oi = 0; oi < 128u; ++oi)
                orderStartTickFP[oi] = UINT64_MAX;
        }

    {
    XBOOL patternLoopBack = FALSE; /* TRUE when E6x just sent us back */

    while (orderPos < h->songLength && rowsProcessed < maxRows)
    {
        uint32_t pattern;
        uint32_t channel;
        uint64_t rowTickFP;
        uint64_t rowAdvanceFP;
        int jumpOrder;
        int breakRow;
        uint8_t rowSpeed;
        uint16_t rowBpm;
        uint8_t patternDelayRows;
        int patternLoopChannel;
        XBOOL patternLoopActive;

        /* Detect song loop: if we enter an order at row 0 that we've seen
         * before AND we got here via order transition (not E6x pattern loop),
         * the song has looped and we stop. */
        if (rowPos == 0 && !patternLoopBack)
        {
            if (orderVisited[orderPos])
            {
                /* Loop back to this order — record loop markers */
                if (orderStartTickFP[orderPos] != UINT64_MAX)
                {
                    song->loopEnabled = TRUE;
                    song->loopStartTick = fp_ticks_to_int(orderStartTickFP[orderPos]);
                    song->loopEndTick   = fp_ticks_to_int(currentTickFP);
                }
                break;
            }
            orderStartTickFP[orderPos] = currentTickFP;
            orderVisited[orderPos] = 1;
        }
        patternLoopBack = FALSE;

        /* Cell data cache per channel for the current row */
        uint8_t cellSample[MOD2RMF_MAX_CHANNELS];
        uint16_t cellPeriod[MOD2RMF_MAX_CHANNELS];
        uint8_t cellEffect[MOD2RMF_MAX_CHANNELS];
        uint8_t cellParam[MOD2RMF_MAX_CHANNELS];

        pattern = h->orders[orderPos];
        if (pattern >= h->patternCount)
        {
            orderPos++;
            rowPos = 0;
            continue;
        }

        /* ---- Parse all cells in this row ---- */
        for (channel = 0; channel < h->channelCount; ++channel)
        {
            size_t cellOffset;
            const unsigned char *cell;

            cellOffset = h->patternDataOffset +
                         (size_t)pattern * 64u * h->channelCount * 4u +
                         (size_t)rowPos * h->channelCount * 4u +
                         (size_t)channel * 4u;
            cell = data + cellOffset;

            cellSample[channel] = (uint8_t)((cell[0] & 0xF0u) | ((cell[2] & 0xF0u) >> 4));
            cellPeriod[channel] = (uint16_t)(((uint16_t)(cell[0] & 0x0Fu) << 8) | (uint16_t)cell[1]);
            cellEffect[channel] = (uint8_t)(cell[2] & 0x0Fu);
            cellParam[channel] = cell[3];
        }

        /* ---- First pass: structural effects ---- */
        jumpOrder = -1;
        breakRow = -1;
        rowSpeed = speed;
        rowBpm = bpm;
        patternDelayRows = 0;
        patternLoopChannel = -1;
        patternLoopActive = FALSE;

        for (channel = 0; channel < h->channelCount; ++channel)
        {
            uint8_t effect, param;
            effect = cellEffect[channel];
            param = cellParam[channel];

            if (effect == 0x0B)
            {
                jumpOrder = (int)param;
            }
            else if (effect == 0x0D)
            {
                int rb;
                rb = ((int)((param >> 4) & 0x0Fu) * 10) + (int)(param & 0x0Fu);
                if (rb < 0) rb = 0;
                if (rb > 63) rb = 63;
                breakRow = rb;
            }
            else if (effect == 0x0F)
            {
                if (param > 0 && param <= 31)
                {
                    rowSpeed = param;
                }
                else if (param >= 32)
                {
                    rowBpm = param;
                }
            }
            else if (effect == 0x0E)
            {
                uint8_t subEff;
                subEff = (uint8_t)((param >> 4) & 0x0Fu);
                if (subEff == 0x0E)
                {
                    uint8_t dr;
                    dr = (uint8_t)(param & 0x0Fu);
                    if (dr > patternDelayRows) patternDelayRows = dr;
                }
                else if (subEff == 0x06)
                {
                    /* Pattern loop */
                    uint8_t loopParam;
                    loopParam = (uint8_t)(param & 0x0Fu);
                    if (loopParam == 0)
                    {
                        channels[channel].loopStartRow = rowPos;
                    }
                    else
                    {
                        if (channels[channel].loopCount == 0)
                        {
                            channels[channel].loopCount = loopParam;
                            patternLoopChannel = (int)channel;
                            patternLoopActive = TRUE;
                        }
                        else
                        {
                            channels[channel].loopCount--;
                            if (channels[channel].loopCount > 0)
                            {
                                patternLoopChannel = (int)channel;
                                patternLoopActive = TRUE;
                            }
                        }
                    }
                }
            }
        }

        /* Apply tempo/speed changes */
        if (rowBpm != bpm)
        {
            bpm = rowBpm;
            song_model_append_tempo_change(song, fp_ticks_to_int(currentTickFP), bpm);
        }
        speed = rowSpeed;
        rowTickFP = mod_row_ticks_fp(speed, bpm);
        rowAdvanceFP = rowTickFP * (uint64_t)(1u + patternDelayRows);

        /* ---- Second pass: tick-by-tick simulation ---- */
        {
            uint32_t totalModTicks;
            uint32_t modTick;

            totalModTicks = (uint32_t)speed * (1u + (uint32_t)patternDelayRows);

            for (modTick = 0; modTick < totalModTicks; ++modTick)
            {
                uint64_t tickPosFP;
                uint32_t midiTick;

                tickPosFP = currentTickFP +
                            (rowAdvanceFP * (uint64_t)modTick) / (uint64_t)totalModTicks;
                midiTick = fp_ticks_to_int(tickPosFP);

                for (channel = 0; channel < h->channelCount; ++channel)
                {
                    uint8_t sampleNum, effect, param;
                    uint16_t period;
                    ChannelState *ch;
                    uint8_t subEff;

                    sampleNum = cellSample[channel];
                    period = cellPeriod[channel];
                    effect = cellEffect[channel];
                    param = cellParam[channel];
                    ch = &channels[channel];
                    subEff = (effect == 0x0E) ? (uint8_t)((param >> 4) & 0x0Fu) : 0;

                    /* ===================== TICK 0 ===================== */
                    if (modTick == 0)
                    {
                        /* --- Phase 1: Resolve volume state (no CC yet) ---
                         *
                         * ProTracker processes sample number, then 0x0C, before
                         * the note trigger.  We resolve ch->volume first but
                         * DEFER the CC#7 emission until after any active note
                         * is flushed, so a volume jump doesn't briefly reach
                         * the old (still-looping) voice.
                         */
                        XBOOL willTriggerNote = FALSE;

                        /* Sample number sets the program and resets the
                         * channel volume to the sample's default.
                         * ProTracker always resets volume when a sample
                         * number appears, even without a note (period). */
                        if (sampleNum > 0 && sampleNum <= h->sampleCount &&
                            conv->slotToProgram[sampleNum - 1] != 0xFF)
                        {
                            ch->program = conv->slotToProgram[sampleNum - 1];
                            ch->volume = conv->rawSamples[sampleNum - 1].defaultVolume;
                        }
                        else if (sampleNum > 0 && sampleNum <= h->sampleCount)
                        {
                            ch->program = 0xFF;
                            ch->volume = conv->rawSamples[sampleNum - 1].defaultVolume;
                        }

                        /* 0x0C: Set Volume — processed before note trigger
                         * (ProTracker: Cxx overrides sample default volume). */
                        if (effect == 0x0C)
                        {
                            ch->volume = (param > 64) ? 64 : (unsigned char)param;
                        }

                        /* Determine whether a note will trigger on this row
                         * so we can defer CC#7 until after the old note is flushed. */
                        if (period != 0 && ch->program != 0xFF &&
                            !(effect == 0x03 || effect == 0x05) &&
                            !(effect == 0x0E && subEff == 0x0D && (param & 0x0Fu) > 0))
                        {
                            willTriggerNote = TRUE;
                        }

                        /* Emit CC#7 now ONLY if no note trigger is coming.
                         * When a note triggers, CC#7 is emitted after the
                         * old note is flushed (see Phase 3 below). */
                        if (!willTriggerNote)
                        {
                            emit_volume_cc(song, (uint16_t)channel, ch, midiTick, ch->volume);
                        }

                        /* --- Phase 2: Update effect memory --- */
                        if (effect == 0x01 && param != 0) ch->portaUpSpeed = param;
                        if (effect == 0x02 && param != 0) ch->portaDownSpeed = param;
                        if (effect == 0x03 && param != 0) ch->tonePortaSpeed = param;
                        if (effect == 0x04)
                        {
                            if ((param >> 4) != 0) ch->vibratoSpeed = (uint8_t)(param >> 4);
                            if ((param & 0x0F) != 0) ch->vibratoDepth = (uint8_t)(param & 0x0F);
                        }
                        if (effect == 0x07)
                        {
                            if ((param >> 4) != 0) ch->tremoloSpeed = (uint8_t)(param >> 4);
                            if ((param & 0x0F) != 0) ch->tremoloDepth = (uint8_t)(param & 0x0F);
                        }
                        if (effect == 0x00 && param != 0) ch->arpeggioParam = param;
                        if (effect == 0x09 && param != 0) ch->sampleOffset = param;

                        /* --- Phase 3: Note trigger --- */

                        /* Note delay: skip note trigger on tick 0 if EDx with x>0 */
                        if (effect == 0x0E && subEff == 0x0D && (param & 0x0Fu) > 0)
                        {
                            /* Note will be triggered on the specified tick below */
                        }
                        else if (period != 0 && ch->program != 0xFF)
                        {
                            if (effect == 0x03 || effect == 0x05)
                            {
                                /* Tone portamento: set target, don't trigger new note */
                                ch->targetPeriod = period;
                            }
                            else
                            {
                                unsigned char midiNote;
                                uint64_t noteStartFP;
                                unsigned char noteProgram;

                                noteStartFP = tickPosFP;
                                noteProgram = ch->program;
                                if (conv->enableZmfSampleOffset && ch->sampleOffset > 0)
                                {
                                    noteProgram = ensure_sample_offset_variant_program(conv,
                                                                                       song,
                                                                                       ch->program,
                                                                                       (uint32_t)ch->sampleOffset * 256u);
                                }

                                if (!period_to_midi(period, &midiNote))
                                {
                                    goto next_channel_tick0;
                                }

                                /* Flush any active note, ending it 1 tick before
                                 * the new note so the CC#7 volume change at this
                                 * tick doesn't briefly reach the old (possibly
                                 * looping) voice.  1 MIDI tick is sub-ms. */
                                {
                                    uint64_t flushEndFP;
                                    flushEndFP = (noteStartFP >= 0x10000u)
                                               ? (noteStartFP - 0x10000u) : 0u;
                                    if (!flush_active_note(song, (uint16_t)channel,
                                                           &activeNotes[channel], flushEndFP))
                                    {
                                        return 0;
                                    }
                                }

                                /* Emit CC#7 with the resolved volume. */
                                emit_volume_cc(song, (uint16_t)channel, ch, midiTick, ch->volume);

                                ch->period = period;
                                ch->noteBasePeriod = period;
                                ch->targetPeriod = period;

                                /* Reset vibrato position if waveform < 4 (retrigger mode) */
                                if (ch->vibratoWaveform < 4) ch->vibratoPos = 0;
                                if (ch->tremoloWaveform < 4) ch->tremoloPos = 0;

                                /* Reset pitch bend to center for new note */
                                if (ch->lastPitchBend != MOD2RMF_PITCH_BEND_CENTER)
                                {
                                    ch->lastPitchBend = MOD2RMF_PITCH_BEND_CENTER;
                                    song_model_append_pitch_bend(song, (uint16_t)channel,
                                                                 midiTick, MOD2RMF_PITCH_BEND_CENTER);
                                }

                                activeNotes[channel].active = TRUE;
                                activeNotes[channel].startTickFP = noteStartFP;
                                activeNotes[channel].note = midiNote;
                                activeNotes[channel].velocity = 127;
                                activeNotes[channel].program = noteProgram;
                            }
                        }

next_channel_tick0:
                        /* ---- Tick-0-only effects (post-trigger) ---- */

                        /* 0x08: Set Panning */
                        if (effect == 0x08)
                        {
                            ch->panning = param;
                            emit_panning_cc(song, (uint16_t)channel, ch, midiTick, ch->panning);
                        }

                        /* E1x: Fine Portamento Up */
                        if (effect == 0x0E && subEff == 0x01)
                        {
                            do_porta_up(ch, (uint8_t)(param & 0x0Fu));
                            emit_pitch_bend(song, (uint16_t)channel, ch, midiTick);
                        }

                        /* E2x: Fine Portamento Down */
                        if (effect == 0x0E && subEff == 0x02)
                        {
                            do_porta_down(ch, (uint8_t)(param & 0x0Fu));
                            emit_pitch_bend(song, (uint16_t)channel, ch, midiTick);
                        }

                        /* E3x: Glissando Control */
                        if (effect == 0x0E && subEff == 0x03)
                        {
                            ch->glissando = (uint8_t)(param & 0x0Fu);
                        }

                        /* E4x: Set Vibrato Waveform */
                        if (effect == 0x0E && subEff == 0x04)
                        {
                            ch->vibratoWaveform = (uint8_t)(param & 0x07u);
                        }

                        /* E5x: Set Finetune */
                        if (effect == 0x0E && subEff == 0x05)
                        {
                            ch->finetune = (int8_t)(param & 0x0Fu);
                            if (ch->finetune > 7) ch->finetune -= 16;
                        }

                        /* E7x: Set Tremolo Waveform */
                        if (effect == 0x0E && subEff == 0x07)
                        {
                            ch->tremoloWaveform = (uint8_t)(param & 0x07u);
                        }

                        /* E8x: Set Panning (coarse, 0-F → 0-255) */
                        if (effect == 0x0E && subEff == 0x08)
                        {
                            ch->panning = (unsigned char)((param & 0x0Fu) * 17u);
                            emit_panning_cc(song, (uint16_t)channel, ch, midiTick, ch->panning);
                        }

                        /* EAx: Fine Volume Slide Up */
                        if (effect == 0x0E && subEff == 0x0A)
                        {
                            int16_t v;
                            v = (int16_t)ch->volume + (int16_t)(param & 0x0Fu);
                            if (v > 64) v = 64;
                            ch->volume = (unsigned char)v;
                            emit_volume_cc(song, (uint16_t)channel, ch, midiTick, ch->volume);
                        }

                        /* EBx: Fine Volume Slide Down */
                        if (effect == 0x0E && subEff == 0x0B)
                        {
                            int16_t v;
                            v = (int16_t)ch->volume - (int16_t)(param & 0x0Fu);
                            if (v < 0) v = 0;
                            ch->volume = (unsigned char)v;
                            emit_volume_cc(song, (uint16_t)channel, ch, midiTick, ch->volume);
                        }

                        /* E9x: Retrigger (tick 0 counts if param divides 0) */
                        if (effect == 0x0E && subEff == 0x09)
                        {
                            ch->retriggerParam = (uint8_t)(param & 0x0Fu);
                        }

                    } /* end tick == 0 */

                    /* ===================== TICK > 0 ===================== */
                    if (modTick > 0)
                    {
                        /* 0x01: Portamento Up */
                        if (effect == 0x01)
                        {
                            do_porta_up(ch, ch->portaUpSpeed);
                            emit_pitch_bend(song, (uint16_t)channel, ch, midiTick);
                        }

                        /* 0x02: Portamento Down */
                        if (effect == 0x02)
                        {
                            do_porta_down(ch, ch->portaDownSpeed);
                            emit_pitch_bend(song, (uint16_t)channel, ch, midiTick);
                        }

                        /* 0x03: Tone Portamento */
                        if (effect == 0x03)
                        {
                            do_tone_porta(ch);
                            if (ch->glissando)
                            {
                                /* Quantize to nearest semitone */
                                unsigned char dummy;
                                if (period_to_midi(ch->period, &dummy) && dummy >= 36u)
                                {
                                    ch->period = gPeriodTable[dummy - 36u];
                                }
                            }
                            emit_pitch_bend(song, (uint16_t)channel, ch, midiTick);
                            if (!maybe_reanchor_tone_porta(song, (uint16_t)channel, ch,
                                                           &activeNotes[channel],
                                                           tickPosFP, midiTick))
                            {
                                return 0;
                            }
                        }

                        /* 0x04: Vibrato */
                        if (effect == 0x04)
                        {
                            ch->vibratoPos += ch->vibratoSpeed;
                            emit_pitch_with_vibrato(song, (uint16_t)channel, ch, midiTick);
                        }

                        /* 0x05: Tone Porta + Volume Slide */
                        if (effect == 0x05)
                        {
                            do_tone_porta(ch);
                            if (param != 0) do_volume_slide(ch, param);
                            emit_pitch_bend(song, (uint16_t)channel, ch, midiTick);
                            if (param != 0) emit_volume_cc(song, (uint16_t)channel, ch, midiTick, ch->volume);
                            if (!maybe_reanchor_tone_porta(song, (uint16_t)channel, ch,
                                                           &activeNotes[channel],
                                                           tickPosFP, midiTick))
                            {
                                return 0;
                            }
                        }

                        /* 0x06: Vibrato + Volume Slide */
                        if (effect == 0x06)
                        {
                            ch->vibratoPos += ch->vibratoSpeed;
                            if (param != 0) do_volume_slide(ch, param);
                            emit_pitch_with_vibrato(song, (uint16_t)channel, ch, midiTick);
                            if (param != 0) emit_volume_cc(song, (uint16_t)channel, ch, midiTick, ch->volume);
                        }

                        /* 0x07: Tremolo */
                        if (effect == 0x07)
                        {
                            ch->tremoloPos += ch->tremoloSpeed;
                            emit_volume_with_tremolo(song, (uint16_t)channel, ch, midiTick);
                        }

                        /* 0x0A: Volume Slide (ProTracker: param 0 = no effect) */
                        if (effect == 0x0A && param != 0)
                        {
                            do_volume_slide(ch, param);
                            emit_volume_cc(song, (uint16_t)channel, ch, midiTick, ch->volume);
                        }
                    } /* end tick > 0 */

                    /* ===================== EVERY TICK ===================== */

                    /* 0x00: Arpeggio (cycles through base, +x, +y semitones).
                     * effect=0x00 with param=0x00 means "no effect", NOT arpeggio. */
                    if (effect == 0x00 && param != 0 &&
                        ch->noteBasePeriod > 0 && activeNotes[channel].active)
                    {
                        uint8_t arpX, arpY, arpNote;
                        uint16_t arpPeriod;
                        uint16_t savedPeriod;
                        uint32_t baseIndex;

                        arpX = (uint8_t)((ch->arpeggioParam >> 4) & 0x0Fu);
                        arpY = (uint8_t)(ch->arpeggioParam & 0x0Fu);

                        /* Find the base note index in the period table */
                        baseIndex = 0;
                        {
                            uint32_t bi;
                            uint32_t bestDiff = 0xFFFFFFFFu;
                            for (bi = 0; bi < (uint32_t)PERIOD_TABLE_SIZE; ++bi)
                            {
                                uint32_t d;
                                d = (gPeriodTable[bi] > ch->noteBasePeriod)
                                    ? (uint32_t)(gPeriodTable[bi] - ch->noteBasePeriod)
                                    : (uint32_t)(ch->noteBasePeriod - gPeriodTable[bi]);
                                if (d < bestDiff) { bestDiff = d; baseIndex = bi; }
                            }
                        }

                        switch (modTick % 3u)
                        {
                            case 0:
                                arpNote = 0;
                                break;
                            case 1:
                                arpNote = arpX;
                                break;
                            default:
                                arpNote = arpY;
                                break;
                        }

                        if (baseIndex + arpNote < (uint32_t)PERIOD_TABLE_SIZE)
                        {
                            arpPeriod = gPeriodTable[baseIndex + arpNote];
                        }
                        else
                        {
                            arpPeriod = gPeriodTable[PERIOD_TABLE_SIZE - 1];
                        }

                        savedPeriod = ch->period;
                        ch->period = arpPeriod;
                        emit_pitch_bend(song, (uint16_t)channel, ch, midiTick);
                        ch->period = savedPeriod;
                    }

                    /* ECx: Note Cut on tick x */
                    if (effect == 0x0E && subEff == 0x0C)
                    {
                        uint8_t cutTick;
                        cutTick = (uint8_t)(param & 0x0Fu);
                        if (modTick == cutTick && activeNotes[channel].active)
                        {
                            if (!flush_active_note(song, (uint16_t)channel,
                                                   &activeNotes[channel], tickPosFP))
                            {
                                return 0;
                            }
                            ch->volume = 0;
                            emit_volume_cc(song, (uint16_t)channel, ch, midiTick, 0);
                        }
                    }

                    /* EDx: Note Delay - trigger note on tick x */
                    if (effect == 0x0E && subEff == 0x0D)
                    {
                        uint8_t delayTick;
                        delayTick = (uint8_t)(param & 0x0Fu);
                        if (modTick == delayTick && period != 0 && ch->program != 0xFF)
                        {
                            unsigned char midiNote;
                            unsigned char noteProgram;
                            if (period_to_midi(period, &midiNote))
                            {
                                noteProgram = ch->program;
                                if (conv->enableZmfSampleOffset && ch->sampleOffset > 0)
                                {
                                    noteProgram = ensure_sample_offset_variant_program(conv,
                                                                                       song,
                                                                                       ch->program,
                                                                                       (uint32_t)ch->sampleOffset * 256u);
                                }

                                if (!flush_active_note(song, (uint16_t)channel,
                                                       &activeNotes[channel], tickPosFP))
                                {
                                    return 0;
                                }

                                ch->period = period;
                                ch->noteBasePeriod = period;
                                ch->targetPeriod = period;
                                if (ch->vibratoWaveform < 4) ch->vibratoPos = 0;
                                if (ch->tremoloWaveform < 4) ch->tremoloPos = 0;

                                if (ch->lastPitchBend != MOD2RMF_PITCH_BEND_CENTER)
                                {
                                    ch->lastPitchBend = MOD2RMF_PITCH_BEND_CENTER;
                                    song_model_append_pitch_bend(song, (uint16_t)channel,
                                                                 midiTick, MOD2RMF_PITCH_BEND_CENTER);
                                }

                                emit_volume_cc(song, (uint16_t)channel, ch, midiTick, ch->volume);

                                activeNotes[channel].active = TRUE;
                                activeNotes[channel].startTickFP = tickPosFP;
                                activeNotes[channel].note = midiNote;
                                activeNotes[channel].velocity = 127;
                                activeNotes[channel].program = noteProgram;
                            }
                        }
                    }

                    /* E9x: Retrigger note every x ticks */
                    if (effect == 0x0E && subEff == 0x09 && ch->retriggerParam > 0)
                    {
                        if (modTick > 0 && (modTick % ch->retriggerParam) == 0 &&
                            activeNotes[channel].active)
                        {
                            unsigned char retrigNote;
                            unsigned char retrigVel;

                            retrigNote = activeNotes[channel].note;
                            retrigVel = activeNotes[channel].velocity;

                            if (!flush_active_note(song, (uint16_t)channel,
                                                   &activeNotes[channel], tickPosFP))
                            {
                                return 0;
                            }

                            activeNotes[channel].active = TRUE;
                            activeNotes[channel].startTickFP = tickPosFP;
                            activeNotes[channel].note = retrigNote;
                            activeNotes[channel].velocity = retrigVel;
                            activeNotes[channel].program = ch->program;
                        }
                    }

                } /* end channel loop */
            } /* end modTick loop */
        }

        /* Advance the tick counter */
        currentTickFP += rowAdvanceFP;

        /* ---- Flow control ---- */
        if (patternLoopActive && patternLoopChannel >= 0)
        {
            rowPos = channels[patternLoopChannel].loopStartRow;
            patternLoopBack = TRUE;
        }
        else if (jumpOrder >= 0)
        {
            if (jumpOrder < (int)h->songLength)
            {
                orderPos = (uint32_t)jumpOrder;
                rowPos = (breakRow >= 0) ? (uint32_t)breakRow : 0u;
            }
            else
            {
                break;
            }
        }
        else if (breakRow >= 0)
        {
            orderPos++;
            rowPos = (uint32_t)breakRow;
        }
        else
        {
            rowPos++;
            if (rowPos >= 64)
            {
                orderPos++;
                rowPos = 0;
            }
        }

        rowsProcessed++;
    }

    /* Natural end: song played through all orders without a Bxx backward jump.
     * MOD files always loop — use restartPos (or order 0) as the loop start. */
    if (!song->loopEnabled)
    {
        unsigned char rp = h->restartPos;
        if (rp >= h->songLength)
        {
            rp = 0;
        }
        if (orderStartTickFP[rp] != UINT64_MAX)
        {
            song->loopEnabled = TRUE;
            song->loopStartTick = fp_ticks_to_int(orderStartTickFP[rp]);
            song->loopEndTick   = fp_ticks_to_int(currentTickFP);
        }
    }

    } /* end patternLoopBack scope */
    } /* end orderVisited scope */

    /* Flush any remaining active notes */
    for (i = 0; i < h->channelCount; ++i)
    {
        if (activeNotes[i].active)
        {
            if (!flush_active_note(song, (uint16_t)i, &activeNotes[i],
                                   currentTickFP + mod_row_ticks_fp(speed, bpm)))
            {
                return 0;
            }
        }
    }

    fprintf(stderr, "native: %u rows, %u notes, %u CC, %u PB, %u tempo, speed=%u bpm=%u ch=%u\n",
            rowsProcessed, song->noteCount, song->ccCount, song->pitchBendCount,
            song->tempoChangeCount, (unsigned)speed, (unsigned)bpm, (unsigned)h->channelCount);

    return 1;
}

static int build_song_model_s3m(Mod2RmfConverter *conv, ModSongModel *song)
{
    typedef struct
    {
        uint8_t inst;      /* 1-based S3M instrument */
        uint8_t program;   /* mapped playable program */
        uint8_t volume;    /* 0..64 */
        uint8_t pan;       /* 0..255 */
        uint8_t sampleOffsetCmd; /* Oxx memory, in 256-byte units */
        uint8_t retrigCmd;       /* Qxy memory */
        uint8_t volSlideCmd;     /* Dxy memory */
        uint8_t portaDownCmd;    /* Exx memory */
        uint8_t portaUpCmd;      /* Fxx memory */
        uint8_t tonePortaCmd;    /* Gxx memory */
        uint8_t vibratoSpeed;    /* Hx0/Kx0 memory */
        uint8_t vibratoDepth;    /* H0x/K0x memory */
        uint8_t vibratoPos;
        uint8_t fineVibratoSpeed; /* Ux0 memory */
        uint8_t fineVibratoDepth; /* U0x memory */
        uint8_t fineVibratoPos;
        uint8_t tremoloSpeed;    /* Rx0 memory */
        uint8_t tremoloDepth;    /* R0x memory */
        uint8_t tremoloPos;
        uint8_t tremorCmd;       /* Ixy memory */
        uint8_t vibratoWaveform; /* S3x: 0=sine 1=ramp 2=square 3=random */
        uint8_t tremoloWaveform; /* S4x: 0=sine 1=ramp 2=square 3=random */
        uint16_t period;
        uint16_t targetPeriod;
        uint16_t noteBasePeriod;
        uint16_t lastPitchBend;
        XBOOL active;
        uint64_t startTickFP;
        unsigned char activeNote;
        unsigned char activeVelocity;
        unsigned char activeProgram;
        /* SBx pattern loop state */
        uint32_t loopStartRow;
        uint8_t loopCount;
        uint32_t c2spd;        /* instrument C2Spd for pitch-delta scaling */
    } S3mChannelState;

    const unsigned char *srcData;
    size_t srcSize;
    Mod2RmfS3mModule s3m;
    char parseErr[256];
    uint32_t i;
    uint32_t k;
    uint32_t playableCap;
    uint32_t playableCount;
    S3mChannelState *channels;
    uint64_t orderStartTickFP[256];
    uint64_t currentTickFP;
    uint16_t speed;
    uint16_t bpm;
    uint32_t orderPos;
    uint32_t rowPos;
    uint32_t stepGuard;
    const uint32_t maxSteps = 400000u;

    if (!conv || !song || !conv->sourceData)
    {
        return 0;
    }

    srcData = (const unsigned char *)conv->sourceData;
    srcSize = conv->sourceSize;

    memset(&s3m, 0, sizeof(s3m));
    memset(parseErr, 0, sizeof(parseErr));
    if (!mod2rmf_s3m_parse_module(srcData, srcSize, &s3m, parseErr, sizeof(parseErr)))
    {
        fprintf(stderr, "Error: S3M parse failed: %s\n", parseErr[0] ? parseErr : "unknown");
        return 0;
    }

    snprintf(song->moduleName, sizeof(song->moduleName), "%s", s3m.name);
    song->channelCount = s3m.channelCount;
    song->bpm = (s3m.initialTempo > 0) ? s3m.initialTempo : 125u;
    if (song->bpm == 0)
    {
        song->bpm = 125u;
    }

    /* Collect source-order instrument/sample names into metadata notes. */
    {
        uint32_t si;
        char clean[64];

        song->composerNotes[0] = '\0';
        for (si = 0; si < s3m.instrumentCount; ++si)
        {
            trim_copy_ascii(clean, sizeof(clean), s3m.samples[si].name);
            if (!clean[0])
            {
                (void)snprintf(clean, sizeof(clean), "%s", "");
            }
            append_linef(song->composerNotes, sizeof(song->composerNotes), clean);
        }
    }

    /* --- Register S3M samples as playables --- */
    playableCap = MOD2RMF_MAX_SAMPLES;
    conv->rawSamples = (ModRawSample *)calloc(playableCap, sizeof(ModRawSample));
    song->playables = (ModPlayable *)calloc(playableCap, sizeof(ModPlayable));
    if (!conv->rawSamples || !song->playables)
    {
        mod2rmf_s3m_free_module(&s3m);
        free(conv->rawSamples);
        conv->rawSamples = NULL;
        free(song->playables);
        song->playables = NULL;
        return 0;
    }

    playableCount = 0;
    for (i = 0; i < s3m.instrumentCount && playableCount < playableCap; ++i)
    {
        Mod2RmfS3mSample *ss;
        ModRawSample *raw;
        ModPlayable *playable;

        ss = &s3m.samples[i];
        if (ss->type != 1 || !ss->pcm8 || ss->length == 0)
        {
            continue;
        }

        raw = &conv->rawSamples[playableCount];
        playable = &song->playables[playableCount];

        memset(raw, 0, sizeof(*raw));
        memset(playable, 0, sizeof(*playable));

        raw->valid = TRUE;
        snprintf(raw->name, sizeof(raw->name), "%s", ss->name);
        raw->frameCount = ss->length;
        raw->loopStart = ss->loopStart;
        raw->loopEnd = ss->loopEnd;
        raw->defaultVolume = ss->volume;
        raw->rootKey = 60;
        raw->finetune = 0;
        raw->pcm8 = (int8_t *)malloc(ss->length);
        if (!raw->pcm8)
        {
            for (k = 0; k < playableCount; ++k)
            {
                free(conv->rawSamples[k].pcm8);
                conv->rawSamples[k].pcm8 = NULL;
            }
            mod2rmf_s3m_free_module(&s3m);
            return 0;
        }
        memcpy(raw->pcm8, ss->pcm8, ss->length);

        playable->sourceSlot = playableCount;
        playable->program = (unsigned char)playableCount;
        playable->rootKey = raw->rootKey;
        playable->rawSample = raw;
        /* S3M C2Spd (C-4 playback rate) maps directly to sample rate. */
        playable->hasSampleRateOverride = TRUE;
        playable->sampleRateOverrideHz = (ss->c2spd > 0) ? ss->c2spd : 8363u;
        snprintf(playable->displayName,
                 sizeof(playable->displayName),
                 "%u %s",
                 (unsigned)(i + 1u),
                 ss->name[0] ? ss->name : "S3M Sample");

        playableCount++;
    }

    song->playableCount = playableCount;
    conv->header.sampleCount = (unsigned char)((playableCount > 255u) ? 255u : playableCount);

    /* Build instrument-to-program mapping (1-based inst → 0-based program).
     * S3M instruments are 1:1 with samples, but empty/OPL slots are skipped. */
    {
        uint8_t instToProgram[256];
        uint32_t progIdx;
        memset(instToProgram, 0xFF, sizeof(instToProgram));
        progIdx = 0;
        for (i = 0; i < s3m.instrumentCount; ++i)
        {
            if (s3m.samples[i].type == 1 && s3m.samples[i].pcm8 && s3m.samples[i].length > 0)
            {
                instToProgram[i] = (uint8_t)progIdx;
                progIdx++;
            }
        }

    /* --- Allocate channel state --- */
    channels = (S3mChannelState *)calloc(s3m.channelCount, sizeof(S3mChannelState));
    if (!channels)
    {
        mod2rmf_s3m_free_module(&s3m);
        return 0;
    }

    /* S3M default panning: channels with setting 0..7 are left, 8..15 are right.
     * Apply stereo separation scaling (0=mono, 75=default, 100=hard L/R). */
    for (i = 0; i < s3m.channelCount; ++i)
    {
        uint8_t cs = s3m.channelSettings[i];
        int sep = (int)conv->stereoSeparation;
        int rawPan;
        channels[i].inst = 0;
        channels[i].program = 0;
        channels[i].volume = s3m.globalVolume;
        channels[i].lastPitchBend = MOD2RMF_PITCH_BEND_CENTER;
        if (s3m.hasDefaultPanning && s3m.defaultPanning[i] != 255u)
        {
            rawPan = (int)s3m.defaultPanning[i];
        }
        else if (cs < 8)
            rawPan = 76;   /* slightly left */
        else if (cs < 16)
            rawPan = 179;  /* slightly right */
        else
            rawPan = 128;  /* center */

        /* Scale panning distance from center by separation %.
         * sep=100 maps original pan to full range (hard L/R for edge values).
         * sep=75 (default) preserves roughly the original S3M distances.
         * sep=0 collapses everything to center. */
        {
            int offset = rawPan - 128;
            offset = (offset * sep) / 75;
            rawPan = 128 + offset;
            if (rawPan < 0) rawPan = 0;
            if (rawPan > 255) rawPan = 255;
        }
        channels[i].pan = (uint8_t)rawPan;
        channels[i].c2spd = 8363u;
        (void)song_model_append_cc_event(song, (uint16_t)i, 0, 7, mod_vol_to_midi(channels[i].volume));
        (void)song_model_append_cc_event(song, (uint16_t)i, 0, 10, (unsigned char)(channels[i].pan >> 1));
    }

    {
    uint8_t orderVisited[256];
    XBOOL patternLoopBack = FALSE;
    memset(orderVisited, 0, sizeof(orderVisited));

    for (i = 0; i < 256u; ++i)
    {
        orderStartTickFP[i] = UINT64_MAX;
    }

    currentTickFP = 0;
    speed = s3m.initialSpeed ? s3m.initialSpeed : 6u;
    bpm = s3m.initialTempo ? s3m.initialTempo : 125u;
    orderPos = 0;
    rowPos = 0;
    stepGuard = 0;
    (void)song_model_append_tempo_change(song, 0, bpm);

    while (orderPos < s3m.orderCount && stepGuard < maxSteps)
    {
        uint8_t patNum;
        Mod2RmfS3mPattern *pat;
        uint64_t rowTicksFP;
        int jumpOrder;
        int breakRow;
        int patternLoopChannel;
        XBOOL patternLoopActive;
        uint8_t patternDelayRows;

        /* Skip marker (254) and end-of-song (255) orders */
        if (s3m.orders[orderPos] >= 254u)
        {
            if (s3m.orders[orderPos] == 255u)
                break;
            orderPos++;
            rowPos = 0;
            stepGuard++;
            continue;
        }

        /* Song-loop detection: if we enter an order at row 0 that we've
         * already visited (and we didn't get here via SBx pattern loop),
         * the song has looped — record loop markers and stop. */
        if (rowPos == 0 && !patternLoopBack)
        {
            if (orderVisited[orderPos])
            {
                if (orderStartTickFP[orderPos] != UINT64_MAX)
                {
                    song->loopEnabled = TRUE;
                    song->loopStartTick = fp_ticks_to_int(orderStartTickFP[orderPos]);
                    song->loopEndTick   = fp_ticks_to_int(currentTickFP);
                }
                break;
            }
            orderStartTickFP[orderPos] = currentTickFP;
            orderVisited[orderPos] = 1;
        }
        patternLoopBack = FALSE;

        patNum = s3m.orders[orderPos];
        if (patNum >= s3m.patternCount)
        {
            orderPos++;
            rowPos = 0;
            stepGuard++;
            continue;
        }

        pat = &s3m.patterns[patNum];
        if (rowPos >= pat->rows)
        {
            orderPos++;
            rowPos = 0;
            stepGuard++;
            continue;
        }

        jumpOrder = -1;
        breakRow = -1;
        rowTicksFP = mod_row_ticks_fp((uint8_t)((speed == 0u) ? 1u : speed), bpm);
        if (rowTicksFP == 0u)
        {
            rowTicksFP = ((uint64_t)MOD2RMF_ROW_TICKS << 16);
        }

        patternLoopChannel = -1;
        patternLoopActive = FALSE;
        patternDelayRows = 0;

        for (i = 0; i < s3m.channelCount; ++i)
        {
            Mod2RmfS3mCell *cell;
            S3mChannelState *ch;
            uint8_t note;
            uint8_t instrument;
            uint8_t volume;
            uint8_t command;
            uint8_t info;

            cell = &pat->cells[(uint32_t)rowPos * (uint32_t)s3m.channelCount + i];
            ch = &channels[i];
            note = cell->note;
            instrument = cell->instrument;
            volume = cell->volume;
            command = cell->command;
            info = cell->info;

            /* Instrument column: update program mapping.
             * Volume resets to instrument default only on note trigger
             * (not note-off 254, not empty 255, not tone porta G/L). */
            if (instrument >= 1u && instrument <= s3m.instrumentCount)
            {
                uint32_t instIdx = (uint32_t)(instrument - 1u);
                ch->inst = instrument;
                ch->c2spd = (s3m.samples[instIdx].c2spd > 0u) ? s3m.samples[instIdx].c2spd : 8363u;
                if (instToProgram[instIdx] != 0xFFu)
                {
                    ch->program = instToProgram[instIdx];
                }
                /* Reset volume to instrument default on note trigger.
                 * Only emit CC7 here if there is NO explicit volume column
                 * on this row — otherwise the volume column handler will
                 * emit it with the correct override value. */
                if (note != 255u && note != 254u && command != 7u && command != 12u)
                {
                    ch->volume = s3m.samples[instIdx].volume;
                    if (ch->volume > 64u) ch->volume = 64u;
                    if (volume == 255u)
                    {
                        (void)song_model_append_cc_event(song, (uint16_t)i,
                                                         fp_ticks_to_int(currentTickFP), 7,
                                                         mod_vol_to_midi(ch->volume));
                    }
                }
            }

            /* Volume column (255 = not present) */
            if (volume != 255u)
            {
                ch->volume = (volume > 64u) ? 64u : volume;
                (void)song_model_append_cc_event(song,
                                                 (uint16_t)i,
                                                 fp_ticks_to_int(currentTickFP),
                                                 7,
                                                 mod_vol_to_midi(ch->volume));
            }

            /* --- Effect processing ---
             * S3M effects use letters A-Z mapped to command values 1-26.
             *   A=1  Set speed
             *   B=2  Jump to order
             *   C=3  Break to row
             *   D=4  Volume slide
             *   E=5  Porta down (EFx=fine, EEx=extra-fine)
             *   F=6  Porta up (FFx=fine, FEx=extra-fine)
             *   G=7  Tone portamento (suppress note trigger)
             *   H=8  Vibrato
             *   I=9  Tremor
             *   J=10 Arpeggio
             *   K=11 Vibrato + volume slide
             *   L=12 Tone porta + volume slide (suppress note trigger)
             *   O=15 Sample offset
             *   Q=17 Retrigger + volume slide
             *   R=18 Tremolo
             *   S=19 Extended effects (S3x/S4x waveform, S8x pan,
             *        SBx loop, SCx cut, SDx delay, SEx pat delay)
             *   T=20 Set tempo / tempo slide (T0x down, T1x up)
             *   U=21 Fine vibrato
             *   V=22 Set global volume
             *   X=24 Set panning
             */

            /* O: Sample offset memory (256-byte units). */
            if (command == 15u && info > 0u)
            {
                ch->sampleOffsetCmd = info;
            }

            /* Q: Retrigger + volume slide memory. */
            if (command == 17u && info > 0u)
            {
                ch->retrigCmd = info;
            }

            /* E/F/G/H effect memory (reuse when xx=00). */
            if (command == 5u && info > 0u)
            {
                ch->portaDownCmd = info;
            }
            if (command == 6u && info > 0u)
            {
                ch->portaUpCmd = info;
            }
            if ((command == 7u || command == 12u) && info > 0u)
            {
                ch->tonePortaCmd = info;
            }
            if (command == 8u && info > 0u)
            {
                if ((info >> 4) != 0u)
                {
                    ch->vibratoSpeed = (uint8_t)(info >> 4);
                }
                if ((info & 0x0Fu) != 0u)
                {
                    ch->vibratoDepth = (uint8_t)(info & 0x0Fu);
                }
            }
            if ((command == 11u || command == 12u) && info > 0u)
            {
                ch->volSlideCmd = info;
            }
            if (command == 9u && info > 0u)
            {
                ch->tremorCmd = info;
            }
            if (command == 18u && info > 0u)
            {
                if ((info >> 4) != 0u)
                {
                    ch->tremoloSpeed = (uint8_t)(info >> 4);
                }
                if ((info & 0x0Fu) != 0u)
                {
                    ch->tremoloDepth = (uint8_t)(info & 0x0Fu);
                }
            }
            if (command == 21u && info > 0u)
            {
                if ((info >> 4) != 0u)
                {
                    ch->fineVibratoSpeed = (uint8_t)(info >> 4);
                }
                if ((info & 0x0Fu) != 0u)
                {
                    ch->fineVibratoDepth = (uint8_t)(info & 0x0Fu);
                }
            }

            /* A: Set speed */
            if (command == 1u && info > 0u)
            {
                speed = info;
                if (speed == 0u)
                {
                    speed = 1u;
                }
                rowTicksFP = mod_row_ticks_fp((uint8_t)speed, bpm);
                if (rowTicksFP == 0u)
                {
                    rowTicksFP = ((uint64_t)MOD2RMF_ROW_TICKS << 16);
                }
            }
            /* T: Set tempo / tempo slide.
             * Txx (xx >= 0x20): set tempo to xx BPM.
             * T0x: slide tempo down by x per tick (ticks 1..speed-1).
             * T1x: slide tempo up by x per tick (ticks 1..speed-1). */
            else if (command == 20u)
            {
                if (info >= 0x20u)
                {
                    bpm = info;
                }
                else
                {
                    /* Tempo slide: apply net change over (speed-1) ticks. */
                    uint8_t slideVal = (uint8_t)(info & 0x0Fu);
                    uint16_t ticks = (speed > 1u) ? (uint16_t)(speed - 1u) : 1u;
                    if ((info >> 4) == 0u)
                    {
                        /* T0x: slide down */
                        int32_t newBpm = (int32_t)bpm - (int32_t)slideVal * (int32_t)ticks;
                        bpm = (uint16_t)((newBpm < 32) ? 32 : newBpm);
                    }
                    else if ((info >> 4) == 1u)
                    {
                        /* T1x: slide up */
                        int32_t newBpm = (int32_t)bpm + (int32_t)slideVal * (int32_t)ticks;
                        bpm = (uint16_t)((newBpm > 255) ? 255 : newBpm);
                    }
                }
                rowTicksFP = mod_row_ticks_fp((uint8_t)((speed == 0u) ? 1u : speed), bpm);
                if (rowTicksFP == 0u)
                {
                    rowTicksFP = ((uint64_t)MOD2RMF_ROW_TICKS << 16);
                }
                (void)song_model_append_tempo_change(song, fp_ticks_to_int(currentTickFP), bpm);
            }
            /* B: Jump to order */
            else if (command == 2u)
            {
                jumpOrder = (int)info;
                if (breakRow < 0)
                    breakRow = 0;
            }
            /* C: Break to row */
            else if (command == 3u)
            {
                if (jumpOrder < 0)
                    jumpOrder = (int)(orderPos + 1u);
                breakRow = (int)(((info >> 4) * 10u) + (info & 0x0Fu));
            }
            /* D: Volume slide.
             * Dx0 = slide up x per tick (speed-1 ticks).
             * D0x = slide down x per tick (speed-1 ticks).
             * DxF = fine slide up x (once).
             * DFx = fine slide down x (once). */
            else if (command == 4u)
            {
                uint8_t slideParam;
                uint8_t up = (uint8_t)(info >> 4);
                uint8_t down = (uint8_t)(info & 0x0Fu);
                slideParam = (info != 0u) ? info : ch->volSlideCmd;
                if (info != 0u)
                {
                    ch->volSlideCmd = info;
                }
                up = (uint8_t)(slideParam >> 4);
                down = (uint8_t)(slideParam & 0x0Fu);
                if (down == 0x0Fu && up > 0u)
                {
                    /* Fine slide up (once) */
                    int32_t v = (int32_t)ch->volume + (int32_t)up;
                    ch->volume = (unsigned char)((v > 64) ? 64 : v);
                }
                else if (up == 0x0Fu && down > 0u)
                {
                    /* Fine slide down (once) */
                    int32_t v = (int32_t)ch->volume - (int32_t)down;
                    ch->volume = (unsigned char)((v < 0) ? 0 : v);
                }
                else if (up > 0u && down == 0u)
                {
                    /* Normal slide up: applied (speed-1) times */
                    uint16_t ticks = (speed > 1u) ? (uint16_t)(speed - 1u) : 1u;
                    int32_t v = (int32_t)ch->volume + (int32_t)up * (int32_t)ticks;
                    ch->volume = (unsigned char)((v > 64) ? 64 : v);
                }
                else if (down > 0u && up == 0u)
                {
                    /* Normal slide down: applied (speed-1) times */
                    uint16_t ticks = (speed > 1u) ? (uint16_t)(speed - 1u) : 1u;
                    int32_t v = (int32_t)ch->volume - (int32_t)down * (int32_t)ticks;
                    ch->volume = (unsigned char)((v < 0) ? 0 : v);
                }
                (void)song_model_append_cc_event(song,
                                                 (uint16_t)i,
                                                 fp_ticks_to_int(currentTickFP),
                                                 7,
                                                 mod_vol_to_midi(ch->volume));
            }
            /* X: Set panning (0x00=left, 0x40=center, 0x80=right, 0xA4=surround) */
            else if (command == 24u)
            {
                if (info <= 0x80u)
                {
                    /* Map S3M 0..128 to 0..255 */
                    ch->pan = (uint8_t)((uint32_t)info * 255u / 128u);
                }
                (void)song_model_append_cc_event(song,
                                                 (uint16_t)i,
                                                 fp_ticks_to_int(currentTickFP),
                                                 10,
                                                 (unsigned char)(ch->pan >> 1));
            }
            /* E: Fine/extra-fine porta down (tick 0 only).
             * EFx = fine porta down x units once.
             * EEx = extra-fine porta down x/4 units once.
             * Normal Exx (xx < 0xE0) handled in per-tick loop below. */
            else if (command == 5u && ch->active)
            {
                uint8_t param = (info != 0u) ? info : ch->portaDownCmd;
                if (param >= 0xF0u)
                {
                    int32_t p = (int32_t)ch->period + s3m_scale_delta((int32_t)(param & 0x0Fu), ch->c2spd);
                    if (p > MOD2RMF_MOD_PERIOD_MAX) p = MOD2RMF_MOD_PERIOD_MAX;
                    ch->period = (uint16_t)p;
                    append_pitch_bend_if_changed(song, (uint16_t)i,
                                                 fp_ticks_to_int(currentTickFP),
                                                 ch->noteBasePeriod, ch->period,
                                                 song->pitchBendRangeSemitones,
                                                 &ch->lastPitchBend);
                }
                else if (param >= 0xE0u)
                {
                    int32_t p = (int32_t)ch->period + s3m_scale_delta((int32_t)((param & 0x0Fu) >> 2), ch->c2spd);
                    if (p > MOD2RMF_MOD_PERIOD_MAX) p = MOD2RMF_MOD_PERIOD_MAX;
                    ch->period = (uint16_t)p;
                    append_pitch_bend_if_changed(song, (uint16_t)i,
                                                 fp_ticks_to_int(currentTickFP),
                                                 ch->noteBasePeriod, ch->period,
                                                 song->pitchBendRangeSemitones,
                                                 &ch->lastPitchBend);
                }
            }
            /* F: Fine/extra-fine porta up (tick 0 only).
             * FFx = fine porta up x units once.
             * FEx = extra-fine porta up x/4 units once.
             * Normal Fxx (xx < 0xE0) handled in per-tick loop below. */
            else if (command == 6u && ch->active)
            {
                uint8_t param = (info != 0u) ? info : ch->portaUpCmd;
                if (param >= 0xF0u)
                {
                    int32_t p = (int32_t)ch->period - s3m_scale_delta((int32_t)(param & 0x0Fu), ch->c2spd);
                    if (p < MOD2RMF_MOD_PERIOD_MIN) p = MOD2RMF_MOD_PERIOD_MIN;
                    ch->period = (uint16_t)p;
                    append_pitch_bend_if_changed(song, (uint16_t)i,
                                                 fp_ticks_to_int(currentTickFP),
                                                 ch->noteBasePeriod, ch->period,
                                                 song->pitchBendRangeSemitones,
                                                 &ch->lastPitchBend);
                }
                else if (param >= 0xE0u)
                {
                    int32_t p = (int32_t)ch->period - s3m_scale_delta((int32_t)((param & 0x0Fu) >> 2), ch->c2spd);
                    if (p < MOD2RMF_MOD_PERIOD_MIN) p = MOD2RMF_MOD_PERIOD_MIN;
                    ch->period = (uint16_t)p;
                    append_pitch_bend_if_changed(song, (uint16_t)i,
                                                 fp_ticks_to_int(currentTickFP),
                                                 ch->noteBasePeriod, ch->period,
                                                 song->pitchBendRangeSemitones,
                                                 &ch->lastPitchBend);
                }
            }
            /* V: Set global volume (0..64) — scale all channel volumes. */
            else if (command == 22u)
            {
                uint8_t gv = (info > 64u) ? 64u : info;
                uint32_t ci;
                s3m.globalVolume = gv;
                /* Re-emit CC7 for all active channels with updated volume. */
                for (ci = 0; ci < s3m.channelCount; ++ci)
                {
                    (void)song_model_append_cc_event(song, (uint16_t)ci,
                                                     fp_ticks_to_int(currentTickFP),
                                                     7,
                                                     mod_vol_to_midi(channels[ci].volume));
                }
            }
            /* S: Extended effects (S3M "S" = command 19) */
            else if (command == 19u)
            {
                uint8_t sub = (uint8_t)(info >> 4);
                uint8_t val = (uint8_t)(info & 0x0Fu);

                /* SBx: Pattern loop */
                if (sub == 0x0Bu)
                {
                    if (val == 0u)
                    {
                        ch->loopStartRow = rowPos;
                    }
                    else
                    {
                        if (ch->loopCount == 0u)
                        {
                            ch->loopCount = val;
                            patternLoopChannel = (int)i;
                            patternLoopActive = TRUE;
                        }
                        else
                        {
                            ch->loopCount--;
                            if (ch->loopCount > 0u)
                            {
                                patternLoopChannel = (int)i;
                                patternLoopActive = TRUE;
                            }
                        }
                    }
                }
                /* SCx: Note cut after x ticks */
                else if (sub == 0x0Cu)
                {
                    if (ch->active && speed > 0u && val < speed)
                    {
                        uint64_t cutTickFP;
                        uint32_t cutTick;
                        uint32_t startTick = fp_ticks_to_int(ch->startTickFP);
                        cutTickFP = currentTickFP +
                                    (rowTicksFP * (uint64_t)val) / (uint64_t)speed;
                        cutTick = fp_ticks_to_int(cutTickFP);
                        if (cutTick > startTick)
                        {
                            (void)song_model_append_note(song,
                                                         (uint16_t)i,
                                                         startTick,
                                                         cutTick - startTick,
                                                         ch->activeNote,
                                                         ch->activeVelocity,
                                                         ch->activeProgram);
                            ch->active = FALSE;
                        }
                    }
                }
                /* S3x: Set vibrato waveform (0=sine, 1=ramp, 2=square, 3=random). */
                else if (sub == 0x03u)
                {
                    ch->vibratoWaveform = (uint8_t)(val & 0x03u);
                }
                /* S4x: Set tremolo waveform (0=sine, 1=ramp, 2=square, 3=random). */
                else if (sub == 0x04u)
                {
                    ch->tremoloWaveform = (uint8_t)(val & 0x03u);
                }
                /* S8x: panning (0..15 => 0..255). */
                else if (sub == 0x08u)
                {
                    ch->pan = (uint8_t)(val * 17u);
                    (void)song_model_append_cc_event(song,
                                                     (uint16_t)i,
                                                     fp_ticks_to_int(currentTickFP),
                                                     10,
                                                     (unsigned char)(ch->pan >> 1));
                }
                /* SDx: Note delay x ticks — handled below in note trigger */
                /* SEx: Pattern delay — repeat this row for x additional rows. */
                else if (sub == 0x0Eu)
                {
                    if (val > patternDelayRows)
                        patternDelayRows = val;
                }
            }

            /* --- Note handling --- */
            /* S3M note byte: 254=note off, 255=no note, else hi=octave lo=semi */
            if (note == 254u)
            {
                /* Note off */
                if (ch->active)
                {
                    uint32_t dur;
                    dur = (currentTickFP > ch->startTickFP)
                        ? (fp_ticks_to_int(currentTickFP) - fp_ticks_to_int(ch->startTickFP))
                        : MOD2RMF_ROW_TICKS;
                    (void)song_model_append_note(song,
                                                 (uint16_t)i,
                                                 fp_ticks_to_int(ch->startTickFP),
                                                 dur,
                                                 ch->activeNote,
                                                 ch->activeVelocity,
                                                 ch->activeProgram);
                    ch->active = FALSE;
                }
            }
            else if (note != 255u)
            {
                /* Tone portamento (G=7) and tone porta + volslide (L=12)
                 * use the note as slide target, so they suppress retrigger
                 * only when a note is already active on this channel. */
                if ((command != 7u && command != 12u) || !ch->active)
                {
                    unsigned char midiNote;
                    unsigned char velocity;
                    uint8_t noteDelayTicks;
                    uint64_t noteStartTickFP;
                    uint32_t noteStartTick;
                    unsigned char noteProgram;
                    uint32_t offsetBytes;
                    uint8_t octave;
                    uint8_t semi;

                    octave = (uint8_t)(note >> 4);
                    semi = (uint8_t)(note & 0x0Fu);
                    if (semi > 11u)
                    {
                        continue;
                    }
                    /* S3M: C-4 (note 0x40) = MIDI 60.  base = 60 - 4*12 = 12. */
                    {
                        int mn = 12 + (int)octave * 12 + (int)semi;
                        if (mn > 127) mn = 127;
                        if (mn < 0) mn = 0;
                        midiNote = (unsigned char)mn;
                    }

                    noteDelayTicks = 0u;
                    if (command == 19u && (info >> 4) == 0x0Du)
                    {
                        noteDelayTicks = (uint8_t)(info & 0x0Fu);
                        if (speed > 0u && noteDelayTicks >= speed)
                        {
                            noteDelayTicks = (uint8_t)(speed - 1u);
                        }
                    }
                    if (speed > 0u)
                    {
                        noteStartTickFP = currentTickFP +
                                          (rowTicksFP * (uint64_t)noteDelayTicks) / (uint64_t)speed;
                    }
                    else
                    {
                        noteStartTickFP = currentTickFP;
                    }
                    noteStartTick = fp_ticks_to_int(noteStartTickFP);
                    /* Use fixed velocity for S3M; all volume control goes
                     * through CC7 so per-row volume column changes
                     * (fade-ins, envelopes) remain audible. */
                    velocity = 100u;
                    noteProgram = ch->program;

                    /* Oxx sample offset starts playback at xx*256 bytes.
                     * For S3M 16-bit samples we already decoded to 8-bit frames,
                     * so this is an approximation in decoded-byte space. */
                    offsetBytes = (uint32_t)ch->sampleOffsetCmd * 256u;
                    if (conv->enableZmfSampleOffset && offsetBytes > 0u)
                    {
                        noteProgram = ensure_sample_offset_variant_program(conv,
                                                                          song,
                                                                          noteProgram,
                                                                          offsetBytes);
                    }

                    /* Close previous note */
                    if (ch->active)
                    {
                        uint32_t dur;
                        dur = (noteStartTick > fp_ticks_to_int(ch->startTickFP))
                                ? (noteStartTick - fp_ticks_to_int(ch->startTickFP))
                                : MOD2RMF_ROW_TICKS;
                        (void)song_model_append_note(song,
                                                     (uint16_t)i,
                                                     fp_ticks_to_int(ch->startTickFP),
                                                     dur,
                                                     ch->activeNote,
                                                     ch->activeVelocity,
                                                     ch->activeProgram);
                    }

                    ch->active = TRUE;
                    ch->startTickFP = noteStartTickFP;
                    ch->activeNote = midiNote;
                    ch->activeVelocity = velocity;
                    ch->activeProgram = noteProgram;
                    ch->period = midi_note_to_mod_period(midiNote);
                    ch->targetPeriod = ch->period;
                    ch->noteBasePeriod = ch->period;
                    ch->vibratoPos = 0;
                    ch->fineVibratoPos = 0;
                    ch->tremoloPos = 0;

                    /* New note starts from neutral bend. */
                    if (ch->lastPitchBend != MOD2RMF_PITCH_BEND_CENTER)
                    {
                        ch->lastPitchBend = MOD2RMF_PITCH_BEND_CENTER;
                        (void)song_model_append_pitch_bend(song,
                                                           (uint16_t)i,
                                                           noteStartTick,
                                                           MOD2RMF_PITCH_BEND_CENTER);
                    }
                }
                else
                {
                    /* G/L with active voice: set target note for portamento. */
                    uint8_t octave;
                    uint8_t semi;
                    unsigned char midiNote;

                    octave = (uint8_t)(note >> 4);
                    semi = (uint8_t)(note & 0x0Fu);
                    if (semi <= 11u)
                    {
                        int mn;
                        mn = 12 + (int)octave * 12 + (int)semi;
                        if (mn > 127) mn = 127;
                        if (mn < 0) mn = 0;
                        midiNote = (unsigned char)mn;
                        ch->targetPeriod = midi_note_to_mod_period(midiNote);
                    }
                }
            }

            /* Pitch effects that operate across row ticks. */
            if (ch->active && speed > 1u &&
                (command == 5u || command == 6u || command == 7u ||
                  command == 8u || command == 11u || command == 12u ||
                  command == 21u))
            {
                uint8_t t;
                int32_t scaledSlide;
                int32_t scaledTone;

                scaledSlide = 0;
                scaledTone = 0;

                if (command == 5u)
                {
                    uint8_t sp = (info != 0u) ? info : ch->portaDownCmd;
                    /* Fine (xFy) and extra-fine (xEy) handled on tick 0 above. */
                    if (sp >= 0xE0u) sp = 0u;
                    scaledSlide = s3m_scale_delta((int32_t)sp, ch->c2spd);
                }
                else if (command == 6u)
                {
                    uint8_t sp = (info != 0u) ? info : ch->portaUpCmd;
                    if (sp >= 0xE0u) sp = 0u;
                    scaledSlide = s3m_scale_delta((int32_t)sp, ch->c2spd);
                }

                if (command == 7u || command == 12u)
                {
                    uint8_t ts = (info != 0u) ? info : ch->tonePortaCmd;
                    scaledTone = s3m_scale_delta((int32_t)ts, ch->c2spd);
                }

                for (t = 1u; t < speed; ++t)
                {
                    uint64_t tickFP;
                    uint32_t tick;

                    tickFP = currentTickFP +
                             (rowTicksFP * (uint64_t)t) / (uint64_t)speed;
                    tick = fp_ticks_to_int(tickFP);

                    if (command == 5u && scaledSlide > 0)
                    {
                        int32_t p;
                        p = (int32_t)ch->period + scaledSlide;
                        if (p > MOD2RMF_MOD_PERIOD_MAX) p = MOD2RMF_MOD_PERIOD_MAX;
                        ch->period = (uint16_t)p;
                        append_pitch_bend_if_changed(song,
                                                     (uint16_t)i,
                                                     tick,
                                                     ch->noteBasePeriod,
                                                     ch->period,
                                                     song->pitchBendRangeSemitones,
                                                     &ch->lastPitchBend);
                    }
                    else if (command == 6u && scaledSlide > 0)
                    {
                        int32_t p;
                        p = (int32_t)ch->period - scaledSlide;
                        if (p < MOD2RMF_MOD_PERIOD_MIN) p = MOD2RMF_MOD_PERIOD_MIN;
                        ch->period = (uint16_t)p;
                        append_pitch_bend_if_changed(song,
                                                     (uint16_t)i,
                                                     tick,
                                                     ch->noteBasePeriod,
                                                     ch->period,
                                                     song->pitchBendRangeSemitones,
                                                     &ch->lastPitchBend);
                    }
                    else if ((command == 7u || command == 12u) && scaledTone > 0)
                    {
                        if (ch->period < ch->targetPeriod)
                        {
                            int32_t p = (int32_t)ch->period + scaledTone;
                            ch->period = (uint16_t)((p > (int32_t)ch->targetPeriod) ? ch->targetPeriod : (uint16_t)p);
                        }
                        else if (ch->period > ch->targetPeriod)
                        {
                            int32_t p = (int32_t)ch->period - scaledTone;
                            ch->period = (uint16_t)((p < (int32_t)ch->targetPeriod) ? ch->targetPeriod : p);
                        }
                        clamp_period(&ch->period);
                        append_pitch_bend_if_changed(song,
                                                     (uint16_t)i,
                                                     tick,
                                                     ch->noteBasePeriod,
                                                     ch->period,
                                                     song->pitchBendRangeSemitones,
                                                     &ch->lastPitchBend);

                        /* Re-anchor / snap noteBasePeriod for tone porta. */
                        if (ch->active)
                        {
                            unsigned char newMidi;
                            if (period_to_midi(ch->period, &newMidi))
                            {
                                if (newMidi != ch->activeNote)
                                {
                                    /* MIDI note changed — flush old, start new */
                                    {
                                        uint32_t startT = fp_ticks_to_int(ch->startTickFP);
                                        uint32_t dur;
                                        dur = (tick >= startT) ? (tick - startT + 1u) : 1u;
                                        (void)song_model_append_note(song,
                                                                     (uint16_t)i,
                                                                     startT,
                                                                     dur,
                                                                     ch->activeNote,
                                                                     ch->activeVelocity,
                                                                     ch->activeProgram);
                                    }
                                    ch->noteBasePeriod = ch->period;
                                    ch->lastPitchBend = MOD2RMF_PITCH_BEND_CENTER;
                                    (void)song_model_append_pitch_bend(song,
                                                                       (uint16_t)i,
                                                                       tick,
                                                                       MOD2RMF_PITCH_BEND_CENTER);
                                    ch->startTickFP = tickFP;
                                    ch->activeNote = newMidi;
                                }
                                else if (ch->period == ch->targetPeriod &&
                                         ch->noteBasePeriod != ch->period)
                                {
                                    /* Same MIDI note but reached target — snap
                                     * noteBasePeriod to eliminate residual
                                     * pitch drift (~50 cents) from re-anchor
                                     * boundary not aligning with target. */
                                    ch->noteBasePeriod = ch->period;
                                    ch->lastPitchBend = MOD2RMF_PITCH_BEND_CENTER;
                                    (void)song_model_append_pitch_bend(song,
                                                                       (uint16_t)i,
                                                                       tick,
                                                                       MOD2RMF_PITCH_BEND_CENTER);
                                }
                            }
                        }
                    }

                    if (command == 8u || command == 11u || command == 21u)
                    {
                        int32_t effective;
                        int16_t wave;
                        int16_t delta;

                        if (command == 21u)
                        {
                            ch->fineVibratoPos = (uint8_t)(ch->fineVibratoPos + ch->fineVibratoSpeed);
                            wave = get_waveform_value(ch->vibratoWaveform, ch->fineVibratoPos);
                            delta = (int16_t)((wave * (int16_t)ch->fineVibratoDepth) / 256);
                        }
                        else
                        {
                            ch->vibratoPos = (uint8_t)(ch->vibratoPos + ch->vibratoSpeed);
                            wave = get_waveform_value(ch->vibratoWaveform, ch->vibratoPos);
                            delta = (int16_t)((wave * (int16_t)ch->vibratoDepth) / 128);
                        }
                        effective = (int32_t)ch->period + s3m_scale_delta((int32_t)delta, ch->c2spd);
                        if (effective < MOD2RMF_MOD_PERIOD_MIN)
                        {
                            effective = MOD2RMF_MOD_PERIOD_MIN;
                        }
                        if (effective > MOD2RMF_MOD_PERIOD_MAX)
                        {
                            effective = MOD2RMF_MOD_PERIOD_MAX;
                        }
                        append_pitch_bend_if_changed(song,
                                                     (uint16_t)i,
                                                     tick,
                                                     ch->noteBasePeriod,
                                                     (uint16_t)effective,
                                                     song->pitchBendRangeSemitones,
                                                     &ch->lastPitchBend);
                    }
                }

                /* K/L include a D-style volume slide component. */
                if (command == 11u || command == 12u)
                {
                    uint8_t slideInfo;
                    uint8_t up;
                    uint8_t down;

                    slideInfo = (info != 0u) ? info : ch->volSlideCmd;
                    up = (uint8_t)(slideInfo >> 4);
                    down = (uint8_t)(slideInfo & 0x0Fu);
                    if (up > 0u && down == 0u)
                    {
                        int32_t v;
                        v = (int32_t)ch->volume + (int32_t)up * (int32_t)(speed - 1u);
                        if (v > 64) v = 64;
                        ch->volume = (uint8_t)v;
                    }
                    else if (down > 0u && up == 0u)
                    {
                        int32_t v;
                        v = (int32_t)ch->volume - (int32_t)down * (int32_t)(speed - 1u);
                        if (v < 0) v = 0;
                        ch->volume = (uint8_t)v;
                    }
                    (void)song_model_append_cc_event(song,
                                                     (uint16_t)i,
                                                     fp_ticks_to_int(currentTickFP + rowTicksFP),
                                                     7,
                                                     mod_vol_to_midi(ch->volume));
                }
            }

            /* R: Tremolo (volume modulation, does not alter base channel volume). */
            if (ch->active && command == 18u && speed > 1u)
            {
                uint8_t t;
                for (t = 1u; t < speed; ++t)
                {
                    uint64_t tickFP;
                    uint32_t tick;
                    int16_t wave;
                    int16_t delta;
                    int32_t effectiveVol;

                    tickFP = currentTickFP +
                             (rowTicksFP * (uint64_t)t) / (uint64_t)speed;
                    tick = fp_ticks_to_int(tickFP);

                    ch->tremoloPos = (uint8_t)(ch->tremoloPos + ch->tremoloSpeed);
                    wave = get_waveform_value(ch->tremoloWaveform, ch->tremoloPos);
                    delta = (int16_t)((wave * (int16_t)ch->tremoloDepth) / 64);
                    effectiveVol = (int32_t)ch->volume + (int32_t)delta;
                    if (effectiveVol < 0) effectiveVol = 0;
                    if (effectiveVol > 64) effectiveVol = 64;

                    (void)song_model_append_cc_event(song,
                                                     (uint16_t)i,
                                                     tick,
                                                     7,
                                                     mod_vol_to_midi((unsigned char)effectiveVol));
                }
            }

            /* I: Tremor (periodic gate using current channel volume). */
            if (ch->active && command == 9u && speed > 1u)
            {
                uint8_t tremor;
                uint8_t onTicks;
                uint8_t offTicks;
                uint8_t t;
                XBOOL gateOn;

                tremor = (info != 0u) ? info : ch->tremorCmd;
                onTicks = (uint8_t)((tremor >> 4) & 0x0Fu);
                offTicks = (uint8_t)(tremor & 0x0Fu);
                onTicks = (uint8_t)(onTicks + 1u);
                offTicks = (uint8_t)(offTicks + 1u);
                gateOn = TRUE;

                for (t = 1u; t < speed; ++t)
                {
                    uint64_t tickFP;
                    uint32_t tick;
                    uint8_t pos;

                    tickFP = currentTickFP +
                             (rowTicksFP * (uint64_t)t) / (uint64_t)speed;
                    tick = fp_ticks_to_int(tickFP);
                    pos = (uint8_t)(t % (uint8_t)(onTicks + offTicks));
                    gateOn = (pos < onTicks) ? TRUE : FALSE;

                    (void)song_model_append_cc_event(song,
                                                     (uint16_t)i,
                                                     tick,
                                                     7,
                                                     gateOn ? mod_vol_to_midi(ch->volume) : 0u);
                }

                (void)song_model_append_cc_event(song,
                                                 (uint16_t)i,
                                                 fp_ticks_to_int(currentTickFP + rowTicksFP),
                                                 7,
                                                 mod_vol_to_midi(ch->volume));
            }

            /* Q: Retrigger + volume slide */
            if (ch->active && command == 17u)
            {
                uint8_t qparam;
                uint8_t retrigEvery;
                uint8_t volOp;

                qparam = (info != 0u) ? info : ch->retrigCmd;
                retrigEvery = (uint8_t)(qparam & 0x0Fu);
                volOp = (uint8_t)((qparam >> 4) & 0x0Fu);
                if (retrigEvery > 0u && speed > 0u)
                {
                    uint8_t t;
                    uint64_t startTickFP = ch->startTickFP;
                    for (t = retrigEvery; t < speed; t = (uint8_t)(t + retrigEvery))
                    {
                        uint64_t retrigTickFP;
                        uint32_t retrigTick;
                        uint32_t startTick;

                        retrigTickFP = currentTickFP +
                                       (rowTicksFP * (uint64_t)t) / (uint64_t)speed;
                        retrigTick = fp_ticks_to_int(retrigTickFP);
                        startTick = fp_ticks_to_int(startTickFP);
                        if (retrigTick <= startTick)
                            continue;
                        (void)song_model_append_note(song,
                                                     (uint16_t)i,
                                                     startTick,
                                                     retrigTick - startTick,
                                                     ch->activeNote,
                                                     ch->activeVelocity,
                                                     ch->activeProgram);

                        /* QxY retrigger volume op (x nibble). */
                        {
                            int32_t v;
                            v = (int32_t)ch->volume;
                            switch (volOp)
                            {
                                case 0x1: v -= 1; break;
                                case 0x2: v -= 2; break;
                                case 0x3: v -= 4; break;
                                case 0x4: v -= 8; break;
                                case 0x5: v -= 16; break;
                                case 0x6: v = (v * 2) / 3; break;
                                case 0x7: v = v / 2; break;
                                case 0x8: break;
                                case 0x9: v += 1; break;
                                case 0xA: v += 2; break;
                                case 0xB: v += 4; break;
                                case 0xC: v += 8; break;
                                case 0xD: v += 16; break;
                                case 0xE: v = (v * 3) / 2; break;
                                case 0xF: v = v * 2; break;
                                default: break;
                            }
                            if (v < 0) v = 0;
                            if (v > 64) v = 64;
                            ch->volume = (uint8_t)v;
                            (void)song_model_append_cc_event(song,
                                                             (uint16_t)i,
                                                             retrigTick,
                                                             7,
                                                             mod_vol_to_midi(ch->volume));
                        }

                        startTickFP = retrigTickFP;
                    }
                    ch->startTickFP = startTickFP;
                }
            }

            /* J: Arpeggio (Jxy) */
            if (ch->active && command == 10u && info != 0u)
            {
                uint8_t x = (uint8_t)(info >> 4);
                uint8_t y = (uint8_t)(info & 0x0Fu);
                uint8_t t;
                uint64_t rowEndTickFP = currentTickFP + rowTicksFP;
                uint32_t rowEndTick = fp_ticks_to_int(rowEndTickFP);
                uint64_t segStartFP = ch->startTickFP;
                unsigned char baseNote;

                if (segStartFP < currentTickFP)
                    segStartFP = currentTickFP;
                baseNote = ch->activeNote;

                for (t = 0; t < speed && fp_ticks_to_int(segStartFP) < rowEndTick; ++t)
                {
                    uint64_t tickStartFP;
                    uint64_t tickEndFP;
                    uint32_t tickStart;
                    uint32_t tickEnd;
                    unsigned char noteOut;

                    tickStartFP = currentTickFP +
                                  (rowTicksFP * (uint64_t)t) / (uint64_t)speed;
                    tickEndFP = currentTickFP +
                                (rowTicksFP * (uint64_t)(t + 1u)) / (uint64_t)speed;
                    if (tickEndFP > rowEndTickFP)
                        tickEndFP = rowEndTickFP;

                    tickStart = fp_ticks_to_int(tickStartFP);
                    tickEnd = fp_ticks_to_int(tickEndFP);
                    if (tickEnd > rowEndTick)
                        tickEnd = rowEndTick;
                    if (tickEnd <= fp_ticks_to_int(segStartFP))
                        continue;
                    if (tickStart < fp_ticks_to_int(segStartFP))
                        tickStart = fp_ticks_to_int(segStartFP);

                    noteOut = baseNote;
                    if ((t % 3u) == 1u)
                    {
                        int n = (int)baseNote + (int)x;
                        if (n > 127) n = 127;
                        noteOut = (unsigned char)n;
                    }
                    else if ((t % 3u) == 2u)
                    {
                        int n = (int)baseNote + (int)y;
                        if (n > 127) n = 127;
                        noteOut = (unsigned char)n;
                    }

                    if (tickEnd > tickStart)
                    {
                        (void)song_model_append_note(song,
                                                     (uint16_t)i,
                                                     tickStart,
                                                     tickEnd - tickStart,
                                                     noteOut,
                                                     ch->activeVelocity,
                                                     ch->activeProgram);
                    }
                    segStartFP = tickEndFP;
                }

                ch->activeNote = baseNote;
                ch->startTickFP = rowEndTickFP;
            }
        } /* end channel loop */

        /* Row duration (SEx pattern delay extends by extra rows) */
        currentTickFP += rowTicksFP * (uint64_t)(1u + patternDelayRows);

        /* Row advancement / flow control */
        if (patternLoopActive && patternLoopChannel >= 0)
        {
            rowPos = channels[patternLoopChannel].loopStartRow;
            patternLoopBack = TRUE;
        }
        else if (jumpOrder >= 0)
        {
            uint32_t targetOrder = (uint32_t)jumpOrder;
            uint32_t targetRow = (breakRow >= 0) ? (uint32_t)breakRow : 0u;
            if (targetOrder < s3m.orderCount &&
                !song->loopEnabled &&
                (targetOrder < orderPos || (targetOrder == orderPos && targetRow < rowPos)))
            {
                song->loopEnabled = TRUE;
                song->loopStartTick = (orderStartTickFP[targetOrder] != UINT64_MAX)
                                        ? fp_ticks_to_int(orderStartTickFP[targetOrder])
                                        : 0u;
                song->loopEndTick = fp_ticks_to_int(currentTickFP);
            }
            orderPos = targetOrder;
            rowPos = targetRow;
        }
        else
        {
            rowPos++;
            if (pat && rowPos >= pat->rows)
            {
                orderPos++;
                rowPos = 0;
            }
        }

        stepGuard++;
    }

    /* Flush any remaining active notes */
    for (i = 0; i < s3m.channelCount; ++i)
    {
        if (channels[i].active)
        {
            uint32_t dur;
            dur = (currentTickFP > channels[i].startTickFP)
                ? (fp_ticks_to_int(currentTickFP) - fp_ticks_to_int(channels[i].startTickFP))
                : MOD2RMF_ROW_TICKS;
            (void)song_model_append_note(song,
                                         (uint16_t)i,
                                         fp_ticks_to_int(channels[i].startTickFP),
                                         dur,
                                         channels[i].activeNote,
                                         channels[i].activeVelocity,
                                         channels[i].activeProgram);
            channels[i].active = FALSE;
        }
    }

    fprintf(stderr,
            "s3m: orders=%u ch=%u samples=%u notes=%u cc=%u tempo=%u bpm=%u speed=%u\n",
            (unsigned)s3m.orderCount,
            (unsigned)s3m.channelCount,
            (unsigned)song->playableCount,
            (unsigned)song->noteCount,
            (unsigned)song->ccCount,
            (unsigned)song->tempoChangeCount,
            (unsigned)bpm,
            (unsigned)speed);

    free(channels);
    } /* end orderVisited scope */
    } /* end instToProgram scope */
    mod2rmf_s3m_free_module(&s3m);
    return 1;
}

static int build_song_model(Mod2RmfConverter *conv, ModSongModel *song)
{
    if (conv && conv->sourceData &&
        mod2rmf_s3m_is_signature((const unsigned char *)conv->sourceData, conv->sourceSize))
    {
        return build_song_model_s3m(conv, song);
    }
    return build_song_model_native(conv, song);
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

static int write_wav_mono8(const char *path,
                           const int8_t *samples,
                            uint32_t frameCount,
                            uint32_t sampleRate)
{
    FILE *file;
    uint32_t dataSize;
    uint32_t riffSize;
    unsigned char header[44];
    uint8_t *wav8;
    uint32_t i;

    if (!path || !samples || frameCount == 0 || sampleRate == 0)
    {
        return 0;
    }

    dataSize = frameCount;
    riffSize = 36 + dataSize;
    memset(header, 0, sizeof(header));

    memcpy(header + 0, "RIFF", 4);
    header[4] = (unsigned char)(riffSize & 0xFF);
    header[5] = (unsigned char)((riffSize >> 8) & 0xFF);
    header[6] = (unsigned char)((riffSize >> 16) & 0xFF);
    header[7] = (unsigned char)((riffSize >> 24) & 0xFF);
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16;
    header[20] = 1;
    header[22] = 1;
    header[24] = (unsigned char)(sampleRate & 0xFF);
    header[25] = (unsigned char)((sampleRate >> 8) & 0xFF);
    header[26] = (unsigned char)((sampleRate >> 16) & 0xFF);
    header[27] = (unsigned char)((sampleRate >> 24) & 0xFF);
    header[28] = (unsigned char)(sampleRate & 0xFF);
    header[29] = (unsigned char)((sampleRate >> 8) & 0xFF);
    header[30] = (unsigned char)((sampleRate >> 16) & 0xFF);
    header[31] = (unsigned char)((sampleRate >> 24) & 0xFF);
    header[32] = 1;
    header[34] = 8;
    memcpy(header + 36, "data", 4);
    header[40] = (unsigned char)(dataSize & 0xFF);
    header[41] = (unsigned char)((dataSize >> 8) & 0xFF);
    header[42] = (unsigned char)((dataSize >> 16) & 0xFF);
    header[43] = (unsigned char)((dataSize >> 24) & 0xFF);

    file = fopen(path, "wb");
    if (!file)
    {
        return 0;
    }

    wav8 = (uint8_t *)malloc(frameCount);
    if (!wav8)
    {
        fclose(file);
        return 0;
    }
    for (i = 0; i < frameCount; ++i)
    {
        wav8[i] = (uint8_t)((int)samples[i] + 128);
    }

    if (fwrite(header, 1, sizeof(header), file) != sizeof(header) ||
        fwrite(wav8, 1, frameCount, file) != frameCount)
    {
        free(wav8);
        fclose(file);
        return 0;
    }
    free(wav8);
    return fclose(file) == 0;
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
            BAE_UNSIGNED_FIXED sampledRate;

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

                    if (playable->hasSampleRateOverride)
                    {
                        sampledRate = (BAE_UNSIGNED_FIXED)(playable->sampleRateOverrideHz << 16);
                    }
                    else
                    {
                        double finetuneRatio = pow(2.0, (double)raw->finetune / 96.0);

                        sampledRate = (BAE_UNSIGNED_FIXED)((double)MOD2RMF_SAMPLE_RATE * finetuneRatio * 65536.0 + 0.5);
                        if (sampledRate < 65536u)
                        {
                            sampledRate = 65536u;
                        }
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
                    srcRateForProcessing = MOD2RMF_SAMPLE_RATE;
                    if (playable->hasSampleRateOverride && playable->sampleRateOverrideHz > 0u)
                    {
                        srcRateForProcessing = playable->sampleRateOverrideHz;
                    }

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
                        /* MOD path: apply finetune pitch correction to effective
                         * output rate. ProTracker finetune step is 1/8 semitone. */
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
            extInfo.volumeADSR.stageCount = 4;
            extInfo.volumeADSR.stages[0].level = VOLUME_RANGE;
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
    XBOOL s3mDetected;
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
    conv->enableZmfSampleOffset = useZmfContainer;
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

    s3mDetected = mod2rmf_s3m_is_signature((const unsigned char *)conv->sourceData, conv->sourceSize) ? TRUE : FALSE;

    if (s3mDetected)
    {
        fprintf(stderr, "S3M detected\n");

        if (!build_song_model(conv, &song))
        {
            fprintf(stderr, "Error: failed to parse source with native S3M parser\n");
            song_model_dispose(&song);
            converter_delete(conv);
            BAE_Cleanup();
            return 1;
        }
    }
    else
    {
        if (!parse_mod_header(conv) ||
            !extract_mod_samples(conv) ||
            !build_slot_program_map(conv) ||
            !build_song_model(conv, &song))
        {
            fprintf(stderr, "Error: failed to parse source with native MOD parser\n");
            song_model_dispose(&song);
            converter_delete(conv);
            BAE_Cleanup();
            return 1;
        }
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
