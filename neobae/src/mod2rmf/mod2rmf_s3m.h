#ifndef MOD2RMF_S3M_H
#define MOD2RMF_S3M_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOD2RMF_S3M_MAX_ORDERS   256
#define MOD2RMF_S3M_MAX_CHANNELS 32

typedef struct Mod2RmfS3mCell
{
    uint8_t channel;     /* 0-based channel (masked from packed byte) */
    uint8_t note;        /* 0=none, 255=note off, else hi=octave lo=semitone */
    uint8_t instrument;  /* 1-based, 0=none */
    uint8_t volume;      /* 255=none, else 0..64 */
    uint8_t command;     /* effect letter A..Z -> 1..26, 0=none */
    uint8_t info;        /* effect parameter */
} Mod2RmfS3mCell;

typedef struct Mod2RmfS3mPattern
{
    uint16_t rows;       /* always 64 in S3M */
    uint16_t channelCount;
    Mod2RmfS3mCell *cells; /* rows * channelCount */
} Mod2RmfS3mPattern;

typedef struct Mod2RmfS3mSample
{
    char name[29];
    uint8_t type;          /* 1=PCM sample, 0=empty, 2+=adlib (unsupported) */
    uint32_t length;       /* in frames */
    uint32_t loopStart;
    uint32_t loopEnd;
    uint8_t volume;        /* default volume 0..64 */
    uint8_t flags;         /* bit0=loop, bit2=16-bit, bit3=stereo */
    uint32_t c2spd;        /* C-4 playback rate (Hz) */
    uint8_t bits16;        /* derived: 1=16-bit, 0=8-bit */
    int8_t *pcm8;          /* unsigned-biased 8-bit PCM for engine */
} Mod2RmfS3mSample;

typedef struct Mod2RmfS3mModule
{
    char name[29];
    uint16_t orderCount;
    uint16_t instrumentCount;
    uint16_t patternCount;
    uint16_t flags;
    uint16_t trackerVersion;
    uint16_t fileFormatVersion;
    uint8_t  globalVolume;   /* 0..64 */
    uint8_t  initialSpeed;   /* default 6 */
    uint8_t  initialTempo;   /* default 125 */
    uint8_t  masterVolume;   /* bit7=stereo, bits0-6=volume */
    uint8_t  hasDefaultPanning; /* optional default panning table present */
    uint8_t  channelSettings[32]; /* per-channel panning/type */
    uint8_t  defaultPanning[32];  /* 0..255, 255=not set */
    uint8_t  orders[MOD2RMF_S3M_MAX_ORDERS];

    uint16_t channelCount;   /* derived: highest used channel + 1 */
    Mod2RmfS3mPattern *patterns;
    Mod2RmfS3mSample *samples;
} Mod2RmfS3mModule;

int mod2rmf_s3m_is_signature(const unsigned char *data, size_t size);

int mod2rmf_s3m_parse_module(const unsigned char *data,
                             size_t size,
                             Mod2RmfS3mModule *outModule,
                             char *errBuf,
                             size_t errBufSize);

void mod2rmf_s3m_free_module(Mod2RmfS3mModule *module);

#ifdef __cplusplus
}
#endif

#endif
