#ifndef GEN_DLS_MOBILEBAE_H
#define GEN_DLS_MOBILEBAE_H

#include "X_API.h"
#include "GenSnd.h"

#ifdef __cplusplus
extern "C" {
#endif


void GM_SetMixerDLSMode(bool isDLS);
bool GM_GetMixerDLSMode();

// Forward declarations
typedef struct DLS_Bank DLS_Bank;
typedef struct DLS_Instrument DLS_Instrument;
typedef struct DLS_Region DLS_Region;
typedef struct DLS_Wave DLS_Wave;
typedef struct DLS_Articulation DLS_Articulation;
typedef struct DLS_Connection DLS_Connection;
typedef struct DLS_SampleInfo DLS_SampleInfo;

// Synth specific forward declarations
typedef struct DLS_Synth DLS_Synth;
typedef struct DLS_Voice DLS_Voice;
typedef struct DLS_ChannelState DLS_ChannelState;
typedef struct DLS_Envelope DLS_Envelope;
typedef struct DLS_Lfo DLS_Lfo;
typedef struct DLS_PlusFilter DLS_PlusFilter;
typedef struct DLS_ProgramAlias DLS_ProgramAlias;

// DLS Constants
#define DLS_FILTER_DISABLED_CUTOFF 65470464
#define DLS_LOOP_NONE 7777
#define DLS_LOOP_FORWARD 7778

// DLS Structures
struct DLS_Connection {
    uint16_t source;
    uint16_t control;
    uint16_t destination;
    uint16_t transform;
    int32_t scale;
};

struct DLS_Articulation {
    int32_t lfoFrequency;
    int32_t lfoStartDelay;
    int32_t vibratoFrequency;
    int32_t vibratoStartDelay;
    int32_t eg1Delay;
    int32_t eg1Attack;
    int32_t eg1Hold;
    int32_t eg1Decay;
    int32_t eg1Sustain;
    int32_t eg1Release;
    int32_t eg2Delay;
    int32_t eg2Attack;
    int32_t eg2Hold;
    int32_t eg2Decay;
    int32_t eg2Sustain;
    int32_t eg2Release;
    int32_t eg1Shutdown;
    int32_t eg2Shutdown;
    int32_t pitch;
    int32_t pan;
    int32_t chorus;
    int32_t reverb;
    int32_t filterCutoff;
    int32_t filterResonance;
    
    uint16_t connectionCount;
    DLS_Connection* runtimeConnections;
};

struct DLS_SampleInfo {
    bool present;
    int32_t loopMode;
    int32_t unityNote;
    int32_t fineTuneCents;
    int32_t attenuation;
    int32_t loopStart;
    int32_t loopEndInclusive;
    bool loopUntilRelease;
};

struct DLS_Wave {
    int32_t index;
    int32_t formatTag;
    int32_t channels;
    int32_t sampleRate;
    int32_t bitsPerSample;
    int32_t blockAlign;
    int32_t avgBytesPerSec;
    int32_t factFrames;
    uint16_t wmaExtraLen;
    uint8_t wmaExtra[8];
    uint32_t frames;
    int16_t* pcm;
    DLS_SampleInfo sample;
};

struct DLS_Region {
    bool level2;
    int32_t keyLow;
    int32_t keyHigh;
    int32_t velocityLow;
    int32_t velocityHigh;
    int32_t options;
    int32_t keyGroup;
    int32_t channel;
    uint16_t waveLinkOptions; /* wlnk.fusOptions; bit0 = PHASE_MASTER */
    int32_t phaseGroup;       /* wlnk.usPhaseGroup; 0 = unlocked */
    int32_t tableIndex;
    int32_t index;
    
    DLS_Articulation articulation;
    bool ownsArticulation;
    DLS_SampleInfo sample;
};

struct DLS_Instrument {
    DLS_Bank* parentBank;
    int32_t rawBank;
    int32_t rawInstrument;
    int32_t bankMsb;
    int32_t bankLsb;
    int32_t program;
    bool drum;
    bool rawMode;
    
    DLS_Articulation articulation;
    
    uint32_t regionCount;
    DLS_Region* regions;
};

struct DLS_ProgramAlias {
    uint32_t fromSelector;
    uint32_t toSelector;
};

struct DLS_Bank {
    bool isDLSM;
    /* True if the file contains Level-2 chunks (art2/lar2/rgn2). Used under
     * -dlscompat to pick DLS2 vs DLS1 implied default connections. */
    bool isDLS2;
    bool forceQuirks;
    bool selectorRawModeActive;
    bool selectorImplicitModeSeen;
    /* microQ/QSound: ptbl extension encodes 'eggs' (bit-reversed); art1
     * usDestination/lScale and wlnk.ulTableIndex are stored bit-reversed and
     * must be decoded (wlnk → direct ptbl cue index). */
    bool eggsArticulators;
    /* MobileBAE banks ship a proprietary 'pgal' (program/percussion alias) chunk. */
    bool hasPgal;
    /* True for pgal banks, or bankinfo.h BANKINFO_FLAG_MOBILEBAE matches.
     * When set, forceQuirks is also set so quirks stay on under -dlscompat. */
    bool isMobileBAE;
    uint32_t declaredInstrumentCount;
    uint32_t articulationChunkCount;
    uint32_t articulationConnectionCount;
    
    uint32_t instrumentCount;
    DLS_Instrument* instruments;

    uint32_t programAliasCount;
    DLS_ProgramAlias* programAliases;
    
    /* Percussion key remap: PGAL banks (full table), or eggs/microQ gap-fill
     * for missing drum keys 27..87 only (present keys stay identity). */
    int32_t percussionKeyAliases[128];
    bool hasPercussionKeyAliases;
    
    uint32_t waveCount;
    DLS_Wave* waves;
};

// Parser Public API
OPErr GM_LoadDLSBankFromMemory(void* pMemory, uint32_t memorySize, DLS_Bank** ppBank);
OPErr GM_LoadDLSFromMemory(struct GM_Mixer* pMixer, const void* pMemory, uint32_t memorySize);
OPErr GM_LoadDLSAsXMFOverlayFromMemory(struct GM_Mixer* pMixer, const void* pMemory, uint32_t memorySize);
void GM_UnloadXMFDLSOverlay(struct GM_Mixer* pMixer);
void GM_UnloadDLSBank(DLS_Bank* pBank);

/* Median effective sample loudness (linear) for a loaded DLS bank, including
 * sample/region attenuation. 0 if unavailable. */
float GM_DLS_MeasureBankLoudness(DLS_Bank* bank);

/* Effective sample loudness for one song note (all matching layered regions). */
float GM_DLS_EstimateNoteLoudness(struct GM_Song* pSong, int16_t channel,
                                  int16_t note, int16_t velocity);

// Synth Public API
OPErr GM_InitDLSSynth(DLS_Synth** ppSynth, int32_t sampleRate);
void GM_FinisDLSSynth(DLS_Synth* pSynth);
bool GM_IsDLSSong(GM_Song* pSong);
/* TRUE when the song should run the DLS render/note-off path: full DLS songs,
   or hybrid RMF/GM songs that have bound at least one CHANNEL_TYPE_DLS. */
bool GM_DLS_SongNeedsRender(GM_Song* pSong);
bool GM_DLS_HasXmfEmbeddedBank(struct GM_Mixer* pMixer);
/* True if main or XMF-overlay DLS bank is a microQ "eggs" (scrambled) bank. */
bool GM_DLS_HasEggsBank(struct GM_Mixer* pMixer);
/* True if main or XMF-overlay DLS bank is treated as MobileBAE
 * (has 'pgal', or matched bankinfo.h BANKINFO_FLAG_MOBILEBAE). */
bool GM_DLS_HasMobileBAEBank(struct GM_Mixer* pMixer);
/* 2 if primary active DLS bank is Level 2, 1 if Level 1, 0 if none. */
int GM_DLS_GetBankLevel(struct GM_Mixer* pMixer);
/* Temporarily force quirks bake for the next DLS bank load (nesting-safe). */
void GM_DLS_BeginForcedQuirksLoad(void);
void GM_DLS_EndForcedQuirksLoad(void);
/* Mark banks[0] as MobileBAE (quirks + badge) after a bankinfo-flagged load. */
void GM_DLS_MarkMainBankMobileBAE(struct GM_Mixer* pMixer);
bool GM_DLS_XmfOverlayHasBankProgram(struct GM_Mixer* pMixer, int32_t bankMsb, int32_t bankLsb, int32_t program);
uint16_t GM_DLS_GetActiveVoiceCount(struct GM_Mixer* pMixer);
GM_Instrument* GM_DLS_CreateInstrumentStub(int32_t instrument);
void GM_DLS_ResetForSong(GM_Song* pSong);

// Synth internal structures
#define DLS_EG1_FULL 0xFFFF0000LL
#define DLS_EG2_FULL 0xFFFF

struct DLS_Envelope {
    int32_t delayMicros;
    int32_t attackMicros;
    int32_t holdMicros;
    int32_t decayMicros;
    int32_t attackTicks;
    int32_t decayTicks;
    int32_t releaseMicros;
    int32_t activeReleaseMicros;
    int32_t shutdownMicros;
    int32_t activeShutdownMicros;
    int32_t sustain;
    bool eg1;
    int64_t eg1Sustain;
    int32_t decayMultiplier;
    int32_t activeReleaseMultiplier;
    int32_t stage;
    int32_t tickIndex;
    int32_t shutdownStart;
    int32_t current;
    int64_t eg1Current;
    bool finished;
    bool forceQuirks;
};

struct DLS_Lfo {
    int32_t startDelay;
    int32_t period;
    int32_t phase;
    int32_t output;
    bool active;
};

struct DLS_PlusFilter {
    int32_t lowThreshold;
    int32_t midThreshold;
    int32_t highThreshold;
    int32_t baseCutoff;
    int32_t baseResonance;
    int32_t resonance;
    int32_t effectiveCutoff;
    int32_t c0;
    int32_t c1;
    int32_t c2;
    int32_t h1Left;
    int32_t h2Left;
    int32_t h1Right;
    int32_t h2Right;
};

struct DLS_ChannelState {
    int32_t channel;
    int32_t program;
    int32_t bankMsb;
    int32_t bankLsb;
    DLS_Instrument* selectedInstrument;
    int32_t selectedBankSelector;
    bool programSelected;
    int32_t pitchBend;
    int32_t modulation;
    int32_t modulationLsb;
    int32_t foot;
    int32_t footLsb;
    int32_t volume;
    int32_t volumeLsb;
    int32_t pan;
    int32_t panLsb;
    int32_t expression;
    int32_t expressionLsb;
    int32_t reverb;
    int32_t chorus;
    int32_t rpnValues[5];
    int32_t rpnMsb;
    int32_t rpnLsb;
    int32_t nrpnMsb;
    int32_t nrpnLsb;
    int32_t selectorMode;
    int32_t channelPressure;
    int32_t keyPressure[128];
    bool sustain;
    bool monoMode; /* CC126 mono / CC127 poly */
    
    /* Portamento/Glide state */
    int32_t portamentoTime; /* CC5 value, 0-127 (0=fastest) */
    bool portamentoEnabled; /* CC65 value */
    int32_t lastNote; /* Previous note for glide target */
};

#define DLS_MAX_VOICE_POOL 256

struct DLS_Voice {
    bool active;
    bool keyHeld;
    bool sustainSnapshot;
    int32_t channel;
    int32_t key;
    int32_t velocity;
    int32_t regionIndex;
    int32_t keyGroup;
    int64_t noteInstanceId; /* groups layered region voices from one note-on */
    int32_t phaseGroup;
    uint16_t waveLinkOptions;
    int32_t phaseLockMaster; /* voice pool index, or -1 if master/unlocked */
    DLS_Wave* wave;
    DLS_Articulation* articulation;
    DLS_ChannelState* channelState;
    DLS_Bank* parentBank;

    uint16_t connectionCount;
    DLS_Connection* runtimeConnections;

    int32_t baseGainQ16;
    int32_t basePanOffset;
    int32_t baseReverbSend;
    int32_t baseChorusSend;
    int64_t baseIncrement;

    int64_t loopStart;
    int64_t loopEnd;
    bool looping;
    bool loopUntilRelease;

    int32_t controlBlockFrames;

    DLS_PlusFilter filter;
    bool filterEnabled;
    DLS_Envelope envelope;
    DLS_Envelope eg2Envelope;
    DLS_Lfo vibratoLfo;
    DLS_Lfo modulationLfo;

    int64_t startSerial;
    int64_t currentIncrement;
    int32_t controlFramesUntilTick;
    
    /* Portamento/Glide state */
    bool portamentoActive;
    int64_t portamentoStartPitch; /* Q16.16 cents */
    int64_t portamentoTargetPitch; /* Q16.16 cents */
    int32_t portamentoFramesRemaining;
    int64_t portamentoTotalFrames;

    int32_t targetLeftGain;
    int32_t targetRightGain;
    int32_t targetReverbSend;
    int32_t targetChorusSend;

    int32_t rampStartLeftGain;
    int32_t rampStartRightGain;
    int32_t rampStartReverbSend;
    int32_t rampStartChorusSend;
    int32_t rampFrame;

    int32_t rampSegmentFrame;
    int32_t rampSegmentFrames;
    int32_t rampSegmentStartLeftGain;
    int32_t rampSegmentStartRightGain;
    int32_t rampSegmentStartReverbSend;
    int32_t rampSegmentStartChorusSend;
    bool rampInitialized; /* first-control-tick gain/send initialization */

    int32_t leftGain;
    int32_t rightGain;
    int32_t reverbSend;
    int32_t chorusSend;

    int32_t lastLeftSample;
    int32_t lastRightSample;
    bool lastFiltered;

    int64_t position;
};

struct DLS_Synth {
    DLS_Bank* banks[2]; /* 0 = main bank, 1 = embedded XMF bank */
    int32_t sampleRate;
    int32_t maxVoices; /* active voice cap from mixer/song settings */
    bool useQuirks;    /* mixer-local MobileBAE quirks mode */
    /* SP-MIDI MIP (mBAE_plus14 byte_1239164 / sub_11F5990): index 0 = highest
     * priority (steal last). Default [9,0,1,...,8,10,...,15]. */
    uint8_t channelPriority[16];
    uint8_t mipChannel[16];
    uint8_t mipLevel[16];
    uint8_t mipPairCount;
    bool mipActive;
    uint16_t mipChannelMask; /* bit N set => channel N allowed; default 0xFFFF */
    DLS_Voice voices[DLS_MAX_VOICE_POOL];
    DLS_ChannelState channels[16];
    int64_t nextVoiceSerial;
    int64_t nextNoteInstance;
    int32_t limiterGainQ16; /* stereo-linked DLS bus limiter state */
};

// Callbacks for Sequencer
void GM_DLS_ProcessNoteOn(GM_Song* pSong, uint16_t channel, uint16_t note, uint16_t velocity);
void GM_DLS_ProcessNoteOff(GM_Song* pSong, uint16_t channel, uint16_t note, uint16_t velocity);
void GM_DLS_ProcessProgramChange(GM_Song* pSong, uint16_t channel, uint16_t program);
void GM_DLS_ProcessPitchBend(GM_Song* pSong, uint16_t channel, uint16_t value);
void GM_DLS_ProcessKeyPressure(GM_Song* pSong, uint16_t channel, uint16_t key, uint16_t value);
void GM_DLS_ProcessChannelPressure(GM_Song* pSong, uint16_t channel, uint16_t value);
void GM_DLS_ProcessController(GM_Song* pSong, uint16_t channel, uint16_t controller, uint16_t value);
void GM_DLS_ProcessSysEx(GM_Song* pSong, const unsigned char* message, int32_t length);
void GM_DLS_RenderAudioSlice(GM_Song* pSong, int32_t* pBuffer, int32_t* pReverbBuffer, int32_t* pChorusBuffer, uint32_t frames);
void GM_DLS_AllNotesOff(GM_Song* pSong, int16_t channel, bool immediate);
bool GM_DLS_HasProgram(GM_Song* pSong, uint16_t channel, uint16_t program);
void GM_DLS_SetMobileBAEQuirks(bool useQuirks);
bool GM_DLS_GetMobileBAEQuirks();
#ifdef __cplusplus
}
#endif

#endif // GEN_DLS_MOBILEBAE_H
