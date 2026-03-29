/* sf2_parser.h — Lightweight SoundFont 2 (SF2) RIFF parser.
 *
 * Parses the SF2 pdta and sdta LIST chunks into in-memory tables.
 * No external library dependencies; only standard C + math.
 */

#ifndef SF2_PARSER_H
#define SF2_PARSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Basic SF2 on-disk sub-structures ---------- */

/* SHDR record (46 bytes, little-endian) */
typedef struct {
    char     name[21];          /* null-padded, 20 chars + NUL */
    uint32_t start;             /* sample start in smpl bank (in samples) */
    uint32_t end;
    uint32_t loopStart;
    uint32_t loopEnd;
    uint32_t sampleRate;
    uint8_t  originalPitch;     /* MIDI note; 255 = no override */
    int8_t   pitchCorrection;   /* cents */
    uint16_t sampleLink;
    uint16_t sampleType;        /* 1=mono, 2=right, 4=left, 8=linked, 0x8xxx=ROM */
} SF2SampleHdr;

/* PHDR record (38 bytes, little-endian) */
typedef struct {
    char     name[21];
    uint16_t preset;            /* MIDI program 0–127 */
    uint16_t bank;              /* MIDI bank; 128 = percussion */
    uint16_t bagIndex;
    uint32_t library;           /* unused */
    uint32_t genre;             /* unused */
    uint32_t morphology;        /* unused */
} SF2PresetHdr;

/* INST record (22 bytes, little-endian) */
typedef struct {
    char     name[21];
    uint16_t bagIndex;
} SF2InstHdr;

/* Bag record (4 bytes, little-endian) */
typedef struct {
    uint16_t genIndex;
    uint16_t modIndex;
} SF2Bag;

/* Generator record (4 bytes, little-endian) */
typedef struct {
    uint16_t oper;
    uint16_t amount;            /* interpretation depends on oper */
} SF2Gen;

/* Known generator operation codes (subset used by the converter) */
#define SF2_GEN_START_ADDRS_OFFSET      0
#define SF2_GEN_END_ADDRS_OFFSET        1
#define SF2_GEN_STARTLOOP_ADDRS_OFFSET  2
#define SF2_GEN_ENDLOOP_ADDRS_OFFSET    3
#define SF2_GEN_START_ADDRS_COARSE      4
#define SF2_GEN_MOD_LFO_TO_PITCH        5
#define SF2_GEN_VIB_LFO_TO_PITCH        6
#define SF2_GEN_MOD_ENV_TO_PITCH        7
#define SF2_GEN_INITIAL_FILTER_FC       8
#define SF2_GEN_INITIAL_FILTER_Q        9
#define SF2_GEN_MOD_LFO_TO_FILTER_FC   10
#define SF2_GEN_MOD_ENV_TO_FILTER_FC   11
#define SF2_GEN_END_ADDRS_COARSE       12
#define SF2_GEN_MOD_LFO_TO_VOLUME      13
#define SF2_GEN_PAN                    17
#define SF2_GEN_CHORUS_EFFECTS_SEND    15
#define SF2_GEN_REVERB_EFFECTS_SEND    16
#define SF2_GEN_DELAY_MOD_LFO          21
#define SF2_GEN_FREQ_MOD_LFO           22
#define SF2_GEN_DELAY_VIB_LFO         23
#define SF2_GEN_FREQ_VIB_LFO           24
#define SF2_GEN_DELAY_MOD_ENV          25
#define SF2_GEN_ATTACK_MOD_ENV         26
#define SF2_GEN_HOLD_MOD_ENV           27
#define SF2_GEN_DECAY_MOD_ENV          28
#define SF2_GEN_SUSTAIN_MOD_ENV        29
#define SF2_GEN_RELEASE_MOD_ENV        30
#define SF2_GEN_DELAY_VOL_ENV          33
#define SF2_GEN_ATTACK_VOL_ENV         34
#define SF2_GEN_HOLD_VOL_ENV           35
#define SF2_GEN_DECAY_VOL_ENV          36
#define SF2_GEN_SUSTAIN_VOL_ENV        37
#define SF2_GEN_RELEASE_VOL_ENV        38
#define SF2_GEN_INSTRUMENT             41
#define SF2_GEN_KEY_RANGE              43
#define SF2_GEN_VEL_RANGE              44
#define SF2_GEN_STARTLOOP_ADDRS_COARSE 45
#define SF2_GEN_KEYNUM                 46
#define SF2_GEN_VELOCITY               47
#define SF2_GEN_INITIAL_ATTENUATION    48
#define SF2_GEN_ENDLOOP_ADDRS_COARSE   50
#define SF2_GEN_COARSE_TUNE            51
#define SF2_GEN_FINE_TUNE              52
#define SF2_GEN_SAMPLE_ID              53
#define SF2_GEN_SAMPLE_MODES           54
#define SF2_GEN_SCALE_TUNING           56
#define SF2_GEN_EXCLUSIVE_CLASS        57
#define SF2_GEN_OVERRIDING_ROOT_KEY    58

/* ---------- Resolved instrument zone ----------
 * One zone = one key-range split inside an SF2 instrument, which may have
 * been referenced by one or more preset bags.  All integer values carry
 * SF2 signed-16 semantics (timecents, centibels, cent-Hz, etc.).
 */
typedef struct {
    /* Articulation — key/vel range */
    int loKey;          /* 0–127; -1 = unspecified (global) */
    int hiKey;
    int loVel;
    int hiVel;

    /* Sample reference */
    int sampleIdx;      /* index into SF2Bank.samples; -1 = global zone */
    int overrideRootKey;/* -1 = use sample header */
    int coarseTune;     /* semitones */
    int fineTune;       /* cents */
    int sampleModes;    /* bit 0 = loop; bit 1 = loop+release */

    /* Volume envelope (timecents / centibels) */
    int volDelayTc;
    int volAttackTc;
    int volHoldTc;
    int volDecayTc;
    int volSustainCb;   /* centibels */
    int volReleaseTc;

    /* Sample offsets (in samples) */
    int startAddrsOffset;
    int startAddrsCoarse;
    int endAddrsOffset;
    int endAddrsCoarse;
    int startloopAddrsOffset;
    int startloopAddrsCoarse;
    int endloopAddrsOffset;
    int endloopAddrsCoarse;

    /* Modulation envelope */
    int modDelayTc;
    int modAttackTc;
    int modHoldTc;
    int modDecayTc;
    int modSustainPm;   /* per-mille, 0=full, 1000=silence */
    int modReleaseTc;
    int modEnvToPitchCents;
    int modEnvToFilterCents;

    /* Modulation LFO */
    int modLfoDelayTc;
    int modLfoFreqCh;   /* cent-Hz */
    int modLfoPitchCents;
    int modLfoVolCb;    /* centibels */
    int modLfoFilterCents;

    /* Vibrato LFO */
    int vibLfoDelayTc;
    int vibLfoFreqCh;
    int vibLfoPitchCents;

    /* Filter */
    int initialFilterFc;    /* cents */
    int initialFilterQ;     /* centibels */

    /* Volume / pan */
    int initialAttenuation; /* centibels */
    int pan;                /* -500 to +500 tenths-of-percent */
    int reverbEffectsSend;  /* permilles 0-1000 */
    int chorusEffectsSend;  /* permilles 0-1000 */

    /* Pitch scaling: cents per semitone key interval; default=100 (normal), 0=no pitch tracking */
    int scaleTuning;        /* cents per key, default 100 */
} SF2Zone;

/* ---------- Parsed bank ---------- */
typedef struct {
    /* Arrays parsed from pdta sub-chunks */
    SF2SampleHdr *samples;
    uint32_t      sampleCount;

    SF2PresetHdr *presets;
    uint32_t      presetCount;

    SF2InstHdr   *instruments;
    uint32_t      instCount;

    SF2Bag       *ibags;
    uint32_t      ibagCount;

    SF2Bag       *pbags;
    uint32_t      pbagCount;

    SF2Gen       *igens;
    uint32_t      igenCount;

    SF2Gen       *pgens;
    uint32_t      pgenCount;

    /* Raw 16-bit LE PCM from sdta/smpl.
     * This pointer points into fileData; do not free separately. */
    const uint8_t *smplData;
    uint32_t       smplSize;   /* bytes */

    /* Backing file buffer (owns all memory) */
    uint8_t *fileData;
    size_t   fileSize;
} SF2Bank;

/* ---------- Public API ---------- */

/* Load an SF2 file into *bank.  Returns 0 on success, -1 on error.
 * errorBuf receives a human-readable message on failure (may be NULL). */
int SF2Bank_Load(const char *path, SF2Bank *bank,
                 char *errorBuf, size_t errorBufSize);

/* Free all memory owned by *bank.  Safe to call on a zero-initialised struct. */
void SF2Bank_Free(SF2Bank *bank);

/* Resolve the instrument zones that a preset uses.
 * *outZones is allocated with malloc; caller must free().
 * Returns the number of zones written, or -1 on error. */
int SF2Bank_GetPresetZones(const SF2Bank *bank, uint32_t presetIdx,
                           SF2Zone **outZones, uint32_t *outCount);

#ifdef __cplusplus
}
#endif

#endif /* SF2_PARSER_H */
