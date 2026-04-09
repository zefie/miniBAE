/*
 * XMPEG_lame_encoder.cpp
 * Adapter to use libmp3lame as a replacement for the old Helix/HMP3 encoder.
 * Implements the legacy MPG_Encode* API used by the project.
 * Built when USE_LAME_ENCODER and USE_MPEG_ENCODER are defined.
 */

#if defined(USE_LAME_ENCODER) && (USE_MPEG_ENCODER!=0)

#include "XMPEG_BAE_API.h" // for prototypes / types
#include "X_API.h"
#include "X_Formats.h"
#include "X_Assert.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <new>

#if defined(__has_include)
#if __has_include(<lame/lame.h>)
#include <lame/lame.h>
#else
#include <lame.h>
#endif
#else
#include <lame.h>
#endif

#define MAX_BITSTREAM_SIZE 8192
#define LAME_DECODER_PRIMING_SAMPLES 529U

typedef bool (*MPEGFillBufferInternalFn)(void *buffer, void *userRef);

struct LAMEEncoderStream {
    lame_t gf;
    uint32_t sampleRate;
    uint32_t sourceSampleRate;
    uint32_t channels;
    uint32_t encodeRateKbpsTotal; /* total kbps to pass to LAME */
    int16_t *pcmBuffer;
    uint32_t pcmFramesPerCall;
    MPEGFillBufferInternalFn refill;
    void *refillUser;
    /* 2x size: last frame may produce both encode + flush output */
    unsigned char bitstream[MAX_BITSTREAM_SIZE * 2];
    uint32_t bitstreamBytes;
    bool lastFrame;
    /* leftover frames when slice doesn't align to MP3 frame size */
    int16_t *leftoverBuf;
    uint32_t leftoverFrames;
    /* position tracking for no-refill (in-memory) mode */
    uint32_t framesConsumed;
};

// Pick the nearest supported TOTAL kbps value LAME understands (common MPEG1/LAME CBR table)
static int pick_nearest_total_kbps(int targetTotal)
{
    const int table[] = {32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    const int count = sizeof(table)/sizeof(table[0]);
    int best = table[0];
    int bestDiff = abs(targetTotal - best);
    for(int i=1;i<count;i++){
        int d = abs(targetTotal - table[i]);
        if(d < bestDiff){ bestDiff = d; best = table[i]; }
    }
    return best;
}

extern "C" void * MPG_EncodeNewStream(uint32_t encodeRate /* bits/sec total */, uint32_t sampleRate, uint32_t channels, XPTR pSampleData16Bits, uint32_t frames){
    if(channels == 0 || channels > 2) return NULL;
    LAMEEncoderStream *s = new (std::nothrow) LAMEEncoderStream();
    if(!s){ BAE_PRINTF("audio: MPG_EncodeNewStream allocation failed\n"); return NULL; }
    s->sampleRate = sampleRate; s->sourceSampleRate = sampleRate; s->channels = channels;
    s->pcmBuffer = (int16_t*)pSampleData16Bits; s->pcmFramesPerCall = frames;
    s->refill = NULL; s->refillUser = NULL; s->bitstreamBytes = 0; s->lastFrame = FALSE; s->leftoverBuf = NULL; s->leftoverFrames = 0; s->framesConsumed = 0;

    /* Accept both legacy caller styles:
     * - bits/sec total (e.g. 128000) from mixer output path
     * - kbps enum-like values (e.g. 128) from XMPEGFilesSun/XGetMPEGEncodeRate
     */
    uint32_t providedTotalBits = encodeRate;
    if(providedTotalBits > 0 && providedTotalBits <= 512) {
        providedTotalBits *= 1000;
    }
    if(providedTotalBits < 8000) providedTotalBits = 8000; /* clamp sensible lower bound */
    /* convert to nearest kbps (round) */
    uint32_t totalKbps = (providedTotalBits + 500) / 1000;
    if(totalKbps < 8) {
        totalKbps = 8;
    }
    if(totalKbps > 320) {
        totalKbps = 320;
    }
    s->encodeRateKbpsTotal = totalKbps;

    /* Create and configure LAME global flags */
    lame_t gf = lame_init();
    if(!gf){ BAE_PRINTF("audio: MPG_EncodeNewStream lame_init() returned NULL\n"); delete s; return NULL; }
    lame_set_in_samplerate(gf, s->sampleRate);
    /* Keep encoded stream sample rate aligned with caller intent.
     * Without this, LAME may auto-downsample low-bitrate encodes (e.g. 48k -> 22.05k),
     * which later conflicts with RMF SND sample-rate metadata and causes pitch drift. */
    lame_set_out_samplerate(gf, s->sampleRate);
    lame_set_num_channels(gf, (int)s->channels);
    /* LAME expects overall kbps; use the provided total kbps and snap to nearest supported total kbps */
    int total_kbps = (int)s->encodeRateKbpsTotal;
    total_kbps = pick_nearest_total_kbps(total_kbps);
    lame_set_brate(gf, total_kbps);
    /* Disable VBR by default to match simpler Helix behavior unless caller expects VBR. */
    lame_set_VBR(gf, vbr_off);
    /* Default quality / fast mode */
    lame_set_quality(gf, 5);

    /* Disable the Xing/Info VBR header frame — it decodes as silence and
     * shifts all audio forward by one extra MP3 frame (~1152 samples). */
    lame_set_bWriteVbrTag(gf, 0);
    /* Keep output stream payload-only for embedded sample blobs. */
    lame_set_write_id3tag_automatic(gf, 0);

    if(lame_init_params(gf) < 0){ BAE_PRINTF("audio: MPG_EncodeNewStream lame_init_params() failed\n"); lame_close(gf); delete s; return NULL; }
    s->gf = gf;

    /* Allocate leftover buffer if frames per call may leave remainder; MP3 frame is 1152 samples */
    if(s->pcmFramesPerCall % 1152){
        uint32_t maxLeft = s->pcmFramesPerCall;
        s->leftoverBuf = (int16_t*)XNewPtr(maxLeft * s->channels * sizeof(int16_t));
        if(!s->leftoverBuf){ BAE_PRINTF("audio: MPG_EncodeNewStream leftoverBuf allocation failed (frames=%u channels=%u)\n", (unsigned)maxLeft, (unsigned)s->channels); lame_close(gf); delete s; return NULL; }
    }

    BAE_PRINTF("audio: MPG_EncodeNewStream using LAME in_sr=%d out_sr=%d ch=%d totalKbps=%d\n",
               (int)s->sampleRate,
               (int)lame_get_out_samplerate(gf),
               (int)s->channels,
               total_kbps);
    return s;
}

extern "C" void MPG_EncodeSetRefillCallback(void *stream, MPEGFillBufferFn cb, void *userRef){
    LAMEEncoderStream *s = (LAMEEncoderStream*)stream; if(!s) return; s->refill = (MPEGFillBufferInternalFn)cb; s->refillUser = userRef; }

extern "C" uint32_t MPG_EncodeMaxFrames(void *stream){
    LAMEEncoderStream *s = (LAMEEncoderStream*)stream;
    if (!s || s->pcmFramesPerCall == 0) return 16;  /* safe minimum */
    /* Number of LAME encode calls = ceil(pcmFrames / 1152) + 2 for flush */
    return (s->pcmFramesPerCall + 1151) / 1152 + 2;
}
extern "C" uint32_t MPG_EncodeMaxFrameSize(void *stream){ return MAX_BITSTREAM_SIZE; }

/* Return the number of leading PCM samples a decoder should skip.
 * For LAME this is the encoder delay plus the documented decoder priming
 * latency (528 + 1 samples) needed to remove the remaining startup gap. */
extern "C" uint32_t MPG_EncodeGetDelay(void *stream){
    LAMEEncoderStream *s = (LAMEEncoderStream*)stream;
    if(!s || !s->gf) return 576U + LAME_DECODER_PRIMING_SAMPLES;
    int d = lame_get_encoder_delay(s->gf);
    return (d > 0) ? ((uint32_t)d + LAME_DECODER_PRIMING_SAMPLES)
                   : (576U + LAME_DECODER_PRIMING_SAMPLES);
}

/* Process: call refill, assemble PCM frames into a buffer sized for lame_encode_buffer_interleaved,
   invoke LAME and return produced bytes. */
extern "C" int MPG_EncodeProcess(void *stream, XPTR *pReturnedBuffer, uint32_t *pReturnedSize, bool *pLastFrame){
    if(pLastFrame) *pLastFrame = FALSE;
    if(!stream){ if(pReturnedBuffer) *pReturnedBuffer=NULL; if(pReturnedSize) *pReturnedSize=0; return 0; }
    LAMEEncoderStream *s = (LAMEEncoderStream*)stream;
    /* Already signaled done on a previous call */
    if(s->lastFrame){ if(pReturnedBuffer) *pReturnedBuffer=NULL; if(pReturnedSize) *pReturnedSize=0; if(pLastFrame) *pLastFrame=TRUE; return 0; }

    const uint32_t targetFrames = 1152U;
    int16_t *workBuf = (int16_t*)XNewPtr((uint32_t)(targetFrames * s->channels * sizeof(int16_t)));
    if(!workBuf){ if(pReturnedBuffer) *pReturnedBuffer=NULL; if(pReturnedSize) *pReturnedSize=0; return 0; }
    uint32_t filled = 0;

    /* consume leftovers first */
    if(s->leftoverFrames){
        uint32_t use = s->leftoverFrames; if(use > targetFrames) use = targetFrames;
        XBlockMove((XPTR)s->leftoverBuf, (XPTR)workBuf, use * s->channels * 2);
        filled += use;
        if(use < s->leftoverFrames){
            uint32_t rem = s->leftoverFrames - use;
            XBlockMove((XPTR)(s->leftoverBuf + use * s->channels), (XPTR)s->leftoverBuf, rem * s->channels * 2);
            s->leftoverFrames = rem;
        } else {
            s->leftoverFrames = 0;
        }
    }

    if(s->refill == NULL){
        /* In-memory mode: advance through pcmBuffer using framesConsumed */
        uint32_t remaining = (s->framesConsumed < s->pcmFramesPerCall)
                             ? (s->pcmFramesPerCall - s->framesConsumed) : 0;
        uint32_t need = targetFrames - filled;
        uint32_t toRead = (remaining < need) ? remaining : need;
        if(toRead > 0){
            XBlockMove((XPTR)(s->pcmBuffer + s->framesConsumed * s->channels),
                       (XPTR)(workBuf + filled * s->channels),
                       toRead * s->channels * sizeof(int16_t));
            filled += toRead;
            s->framesConsumed += toRead;
        }
        if(s->framesConsumed >= s->pcmFramesPerCall){
            s->lastFrame = TRUE;
        }
    } else {
        /* Streaming/callback mode: call refill to get new PCM data each time */
        while(filled < targetFrames && !s->lastFrame){
            bool ok = s->refill(s->pcmBuffer, s->refillUser);
            if(!ok){ s->lastFrame = TRUE; break; }
            uint32_t sliceFrames = s->pcmFramesPerCall;
            uint32_t need = targetFrames - filled;
            if(sliceFrames <= need){
                XBlockMove((XPTR)s->pcmBuffer, (XPTR)(workBuf + filled * s->channels), sliceFrames * s->channels * 2);
                filled += sliceFrames;
            } else {
                XBlockMove((XPTR)s->pcmBuffer, (XPTR)(workBuf + filled * s->channels), need * s->channels * 2);
                filled += need;
                if(s->leftoverBuf){
                    uint32_t rem = sliceFrames - need;
                    XBlockMove((XPTR)(s->pcmBuffer + need * s->channels), (XPTR)s->leftoverBuf, rem * s->channels * 2);
                    s->leftoverFrames = rem;
                }
            }
        }
    }

    /* zero-pad short final frame to complete the MP3 frame */
    if(filled < targetFrames){
        uint32_t pad = targetFrames - filled;
        XSetMemory((unsigned char*)(workBuf + filled * s->channels), pad * s->channels * 2, 0);
    }

    /* Encode this chunk; bitstream is 2x MAX_BITSTREAM_SIZE so encode+flush fit */
    uint32_t encBytes = 0;
    if(filled > 0){
        int ret;
        if(s->channels == 2){
            ret = lame_encode_buffer_interleaved(s->gf, workBuf, (int)targetFrames,
                                                 s->bitstream, MAX_BITSTREAM_SIZE);
        } else {
            ret = lame_encode_buffer(s->gf, workBuf, NULL, (int)targetFrames,
                                     s->bitstream, MAX_BITSTREAM_SIZE);
        }
        encBytes = (ret > 0) ? (uint32_t)ret : 0;
        if(ret < 0){
            /* LAME error; mark as done */
            BAE_PRINTF("audio: MPG_EncodeProcess lame encode error %d\n", ret);
            s->lastFrame = TRUE;
        }
    }

    if(s->lastFrame){
        /* Flush LAME's internal delay buffers; append after any encode bytes */
        int flushRet = lame_encode_flush(s->gf, s->bitstream + encBytes,
                                          MAX_BITSTREAM_SIZE);
        if(flushRet > 0) encBytes += (uint32_t)flushRet;
        BAE_PRINTF("audio: MPG_EncodeProcess last frame: encBytes=%u total=%u framesConsumed=%u\n",
                   (unsigned)(encBytes - (flushRet > 0 ? (uint32_t)flushRet : 0)),
                   (unsigned)encBytes, (unsigned)s->framesConsumed);
    }

    s->bitstreamBytes = encBytes;
    if(pReturnedBuffer) *pReturnedBuffer = (encBytes ? (XPTR)s->bitstream : NULL);
    if(pReturnedSize)   *pReturnedSize   = encBytes;
    if(pLastFrame)      *pLastFrame       = s->lastFrame;

    XDisposePtr((XPTR)workBuf);
    return (int)s->framesConsumed;
}

extern "C" void MPG_EncodeFreeStream(void *stream){
    LAMEEncoderStream *s = (LAMEEncoderStream*)stream; if(!s) return;
    if(s->leftoverBuf) XDisposePtr((XPTR)s->leftoverBuf);
    if(s->gf) lame_close(s->gf);
    delete s;
}

#if USE_MPEG_DECODER == 0
/* ------------------------------------------------------------------ */
/*  Lightweight MPEG frame-header parser for encoder-only builds.      */
/*  Extracts sample rate, channels, bitrate and frame geometry from    */
/*  the first valid frame header so the SND metadata is correct.       */
/* ------------------------------------------------------------------ */

/* MPEG sample-rate tables indexed by [version][sr_index].
 * version: 0 = MPEG-2.5, 1 = reserved, 2 = MPEG-2, 3 = MPEG-1 */
static const uint32_t sMpegSampleRates[4][3] = {
    { 11025, 12000,  8000 },  /* MPEG-2.5 */
    {     0,     0,     0 },  /* reserved  */
    { 22050, 24000, 16000 },  /* MPEG-2   */
    { 44100, 48000, 32000 },  /* MPEG-1   */
};

/* MPEG bitrate tables indexed by [version_group][layer_index][br_index].
 * version_group: 0 = MPEG-1, 1 = MPEG-2/2.5
 * layer_index:   0 = Layer I, 1 = Layer II, 2 = Layer III */
static const uint16_t sMpegBitrates[2][3][15] = {
    { /* MPEG-1 */
        { 0,32,64,96,128,160,192,224,256,288,320,352,384,416,448 },  /* Layer I   */
        { 0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384 },  /* Layer II  */
        { 0,32,40,48, 56, 64, 80, 96,112,128,160,192,224,256,320 },  /* Layer III */
    },
    { /* MPEG-2 / MPEG-2.5 */
        { 0,32,48,56,64,80,96,112,128,144,160,176,192,224,256 },     /* Layer I   */
        { 0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160 },    /* Layer II  */
        { 0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160 },    /* Layer III */
    },
};

/* Samples per MPEG frame indexed by [version_group][layer_index]. */
static const uint32_t sMpegSamplesPerFrame[2][3] = {
    { 384, 1152, 1152 },  /* MPEG-1 */
    { 384, 1152,  576 },  /* MPEG-2 / MPEG-2.5 */
};

/* Find and parse the first valid MPEG sync-word in the buffer.
 * Returns TRUE on success, filling the out-parameters. */
static bool PV_ParseFirstMpegFrame(const uint8_t *buf, uint32_t bufSize,
                                     uint32_t *outSampleRate, uint32_t *outChannels,
                                     uint32_t *outBitrateKbps, uint32_t *outSamplesPerFrame,
                                     uint32_t *outFrameBytes)
{
    uint32_t i;
    for (i = 0; i + 3 < bufSize; ++i)
    {
        uint32_t hdr, ver, layer, brIdx, srIdx, pad, mode;
        uint32_t vg; /* version group: 0 = MPEG-1, 1 = MPEG-2/2.5 */
        uint32_t li; /* layer index:   0 = I, 1 = II, 2 = III */
        uint32_t sr, br, spf, frameBytes;

        if (buf[i] != 0xFF || (buf[i+1] & 0xE0) != 0xE0)
            continue;

        hdr   = ((uint32_t)buf[i] << 24) | ((uint32_t)buf[i+1] << 16) |
                ((uint32_t)buf[i+2] << 8) | (uint32_t)buf[i+3];
        ver   = (hdr >> 19) & 0x03;
        layer = (hdr >> 17) & 0x03;
        brIdx = (hdr >> 12) & 0x0F;
        srIdx = (hdr >> 10) & 0x03;
        pad   = (hdr >>  9) & 0x01;
        mode  = (hdr >>  6) & 0x03;

        if (ver == 1 || layer == 0 || brIdx == 0 || brIdx == 15 || srIdx == 3)
            continue; /* reserved / free-format / bad index */

        li = 3 - layer;  /* layer field: 3=I, 2=II, 1=III → index 0,1,2 */
        vg = (ver == 3) ? 0 : 1;

        sr = sMpegSampleRates[ver][srIdx];
        br = (uint32_t)sMpegBitrates[vg][li][brIdx];
        spf = sMpegSamplesPerFrame[vg][li];

        if (sr == 0 || br == 0)
            continue;

        if (li == 0) /* Layer I */
            frameBytes = (12 * br * 1000 / sr + pad) * 4;
        else
            frameBytes = 144 * br * 1000 / sr + pad;

        *outSampleRate = sr;
        *outChannels = (mode == 3) ? 1 : 2; /* mode 3 = mono */
        *outBitrateKbps = br;
        *outSamplesPerFrame = spf;
        *outFrameBytes = frameBytes;
        return TRUE;
    }
    return FALSE;
}

// Provide stub implementations of decoder functions needed by encoder code
// when decoder is disabled. These are used to get metadata from the encoded
// stream so that the SND header is written with correct values.
extern "C" XMPEGDecodedData * XOpenMPEGStreamFromMemory(XPTR pBlock, uint32_t blockSize, OPErr *pErr) {
    XMPEGDecodedData *stream = (XMPEGDecodedData*)XNewPtr(sizeof(XMPEGDecodedData));
    if (!stream) {
        if (pErr) *pErr = MEMORY_ERR;
        return NULL;
    }

    uint32_t sr = 44100, ch = 2, br = 128, spf = 1152, frameBytes = 417;

    if (pBlock && blockSize >= 4)
    {
        PV_ParseFirstMpegFrame((const uint8_t *)pBlock, blockSize < 2048 ? blockSize : 2048,
                               &sr, &ch, &br, &spf, &frameBytes);
    }

    stream->sampleRate = UNSIGNED_LONG_TO_XFIXED(sr);
    stream->bitSize = 16;
    stream->channels = (uint8_t)ch;
    stream->bitrate = br * 1000;
    stream->frameBufferSize = spf * ch * 2; /* decoded PCM bytes per MPEG frame */
    stream->maxFrameBuffers = (frameBytes > 0) ? (blockSize / frameBytes) : 1;
    stream->lengthInSamples = stream->maxFrameBuffers * spf;
    stream->lengthInBytes = stream->lengthInSamples * ch * 2;
    stream->stream = NULL;

    if (pErr) *pErr = NO_ERR;
    return stream;
}

extern "C" OPErr XCloseMPEGStream(XMPEGDecodedData *stream) {
    if (stream) {
        XDisposePtr((XPTR)stream);
    }
    return NO_ERR;
}

// Stub implementation of XFillMPEGStreamBuffer for encoder-only builds
extern "C" OPErr XFillMPEGStreamBuffer(XMPEGDecodedData *stream, void *pcmAudioBuffer, bool *pDone) {
    // Simple stub - this shouldn't be called in normal encoder-only operation
    if (pDone) *pDone = TRUE;
    return PARAM_ERR; // Signal that this is not a real decoder
}

// Additional encoder helper functions needed by XMPEGFilesSun.c
extern "C" XMPEGEncodeRate XGetMPEGEncodeRate(SndCompressionType type) {
    // Map compression types to bitrates
    switch (type) {
        case C_MPEG_32: return (XMPEGEncodeRate)32;
        case C_MPEG_40: return (XMPEGEncodeRate)40;
        case C_MPEG_48: return (XMPEGEncodeRate)48;
        case C_MPEG_56: return (XMPEGEncodeRate)56;
        case C_MPEG_64: return (XMPEGEncodeRate)64;
        case C_MPEG_80: return (XMPEGEncodeRate)80;
        case C_MPEG_96: return (XMPEGEncodeRate)96;
        case C_MPEG_112: return (XMPEGEncodeRate)112;
        case C_MPEG_128: return (XMPEGEncodeRate)128;
        case C_MPEG_160: return (XMPEGEncodeRate)160;
        case C_MPEG_192: return (XMPEGEncodeRate)192;
        case C_MPEG_224: return (XMPEGEncodeRate)224;
        case C_MPEG_256: return (XMPEGEncodeRate)256;
        case C_MPEG_320: return (XMPEGEncodeRate)320;
        default: return (XMPEGEncodeRate)128; // Default to 128 kbps
    }
}

extern "C" SndCompressionType XGetMPEGCompressionType(XMPEGEncodeRate rate) {
    // Map bitrates back to compression types
    switch ((int)rate) {
        case 32: return C_MPEG_32;
        case 40: return C_MPEG_40;
        case 48: return C_MPEG_48;
        case 56: return C_MPEG_56;
        case 64: return C_MPEG_64;
        case 80: return C_MPEG_80;
        case 96: return C_MPEG_96;
        case 112: return C_MPEG_112;
        case 128: return C_MPEG_128;
        case 160: return C_MPEG_160;
        case 192: return C_MPEG_192;
        case 224: return C_MPEG_224;
        case 256: return C_MPEG_256;
        case 320: return C_MPEG_320;
        default: return C_MPEG_128; // Default to 128 kbps
    }
}

extern "C" XFIXED XGetClosestMPEGSampleRate(XFIXED sourceRate, SndCompressionSubType subType) {
    // For simplicity, return common sample rates
    (void)subType; // Unused
    unsigned long rate = XFIXED_TO_UNSIGNED_LONG(sourceRate);
    
    // Common MPEG sample rates
    if (rate <= 8000) return UNSIGNED_LONG_TO_XFIXED(8000);
    if (rate <= 11025) return UNSIGNED_LONG_TO_XFIXED(11025);
    if (rate <= 12000) return UNSIGNED_LONG_TO_XFIXED(12000);
    if (rate <= 16000) return UNSIGNED_LONG_TO_XFIXED(16000);
    if (rate <= 22050) return UNSIGNED_LONG_TO_XFIXED(22050);
    if (rate <= 24000) return UNSIGNED_LONG_TO_XFIXED(24000);
    if (rate <= 32000) return UNSIGNED_LONG_TO_XFIXED(32000);
    if (rate <= 44100) return UNSIGNED_LONG_TO_XFIXED(44100);
    return UNSIGNED_LONG_TO_XFIXED(48000); // Default to 48kHz for higher rates
}

extern "C" void XGetClosestMPEGSampleRateAndEncodeRate(XFIXED inSampleRate, 
                                                      XMPEGEncodeRate inEncodeRate,
                                                      XFIXED *outSampleRate,
                                                      XMPEGEncodeRate *outEncodeRate,
                                                      SndCompressionSubType subType) {
    if (outSampleRate) {
        *outSampleRate = XGetClosestMPEGSampleRate(inSampleRate, subType);
    }
    if (outEncodeRate) {
        *outEncodeRate = inEncodeRate; // Just pass through the encode rate
    }
}
#endif // USE_MPEG_DECODER == 0

#endif /* USE_LAME_ENCODER && USE_MPEG_ENCODER */
