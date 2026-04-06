/*****************************************************************************/
/*
**  XOpusFiles.c
**
**  Integration of libopus/libopusfile for Ogg Opus audio file support in NeoBAE
**
**  This file provides decoding support for Ogg Opus audio files and
**  encoding support for Ogg Opus export/sample workflows.
*/
/*****************************************************************************/

#include "X_API.h"
#include "X_Formats.h"
#include "GenSnd.h"
#include "GenPriv.h"

#include <stdlib.h>

#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE
#include <opus/opus.h>
#include <ogg/ogg.h>
#endif

#if USE_OPUS_DECODER == TRUE
#include <opusfile.h>
#endif

#if USE_OPUS_DECODER == TRUE

// Structure to hold Opus decoder state
typedef struct {
    OggOpusFile *of;
    bool is_open;
} XOpusDecoder;


// Callback functions for libopusfile to read from XFILE
static int opus_read_func(void *stream, unsigned char *ptr, int nbytes)
{
    XFILE file = (XFILE)stream;
    int32_t err;

    if (nbytes == 0) return 0;

    /* XFileRead returns 0 on success, -1 on failure */
    err = XFileRead(file, ptr, nbytes);
    if (err == 0) {
        return nbytes; /* Success - return number of bytes read */
    } else {
        return 0; /* EOF or error */
    }
}

static int opus_seek_func(void *stream, opus_int64 offset, int whence)
{
    XFILE file = (XFILE)stream;
    int32_t new_pos;

    switch (whence) {
        case SEEK_SET:
            new_pos = (int32_t)offset;
            break;
        case SEEK_CUR:
            new_pos = XFileGetPosition(file) + (int32_t)offset;
            break;
        case SEEK_END:
            new_pos = XFileGetLength(file) + (int32_t)offset;
            break;
        default:
            return -1;
    }

    if (XFileSetPosition(file, new_pos) == 0) {
        return 0;
    } else {
        return -1;
    }
}

static opus_int64 opus_tell_func(void *stream)
{
    XFILE file = (XFILE)stream;
    return (opus_int64)XFileGetPosition(file);
}

static OpusFileCallbacks opus_callbacks = {
    opus_read_func,
    opus_seek_func,
    opus_tell_func,
    NULL  // close_func - we handle closing ourselves
};


// Check if file is an Ogg Opus file
bool XIsOpusFile(XFILE file)
{
    OggOpusFile *of;
    int error;
    long pos;

    if (file == NULL) return FALSE;

    // Save current position
    pos = XFileGetPosition(file);

    // Try to open as Opus file
    of = op_open_callbacks(file, &opus_callbacks, NULL, 0, &error);

    // Restore position
    XFileSetPosition(file, pos);

    if (of != NULL) {
        op_free(of);
        return TRUE;
    }

    return FALSE;
}

// Open Opus file for decoding
void* XOpenOpusFile(XFILE file)
{
    XOpusDecoder *decoder;
    int error;

    if (file == NULL) return NULL;

    decoder = (XOpusDecoder *)XNewPtr(sizeof(XOpusDecoder));
    if (decoder == NULL) return NULL;

    decoder->of = op_open_callbacks(file, &opus_callbacks, NULL, 0, &error);
    if (decoder->of == NULL) {
        XDisposePtr(decoder);
        return NULL;
    }

    decoder->is_open = TRUE;
    return decoder;
}

// Get Opus file information
OPErr XGetOpusFileInfo(void *decoder_handle, uint32_t *samples, uint32_t *sample_rate,
                      uint32_t *channels, uint32_t *bit_depth)
{
    XOpusDecoder *decoder = (XOpusDecoder *)decoder_handle;
    const OpusHead *head;

    if (decoder == NULL || decoder->of == NULL || !decoder->is_open) {
        return PARAM_ERR;
    }

    head = op_head(decoder->of, -1);
    if (head == NULL) {
        return BAD_FILE;
    }

    *channels = head->channel_count;
    *sample_rate = 48000;  // Opus always decodes to 48kHz
    *samples = (uint32_t)op_pcm_total(decoder->of, -1);
    *bit_depth = 16;  // Opus decodes to 16-bit PCM

    return NO_ERR;
}

// Decode Opus file data
long XDecodeOpusFile(void *decoder_handle, void *buffer, long buffer_size)
{
    XOpusDecoder *decoder = (XOpusDecoder *)decoder_handle;
    const OpusHead *head;
    int channels;
    int samples_read;
    long bytes_read;

    if (decoder == NULL || decoder->of == NULL || !decoder->is_open) {
        return 0;
    }

    head = op_head(decoder->of, -1);
    channels = (head && head->channel_count >= 1) ? head->channel_count : 1;

    // op_read returns number of samples read per channel
    // Convert to total bytes using current channel count.
    samples_read = op_read(decoder->of, (opus_int16 *)buffer, buffer_size / 2, NULL);

    if (samples_read < 0) {
        return 0;  // Error
    }

    bytes_read = (long)samples_read * (long)channels * 2;  // 16-bit samples, all channels
    return bytes_read;
}

// Close Opus file
void XCloseOpusFile(void *decoder_handle)
{
    XOpusDecoder *decoder = (XOpusDecoder *)decoder_handle;

    if (decoder != NULL) {
        if (decoder->of != NULL && decoder->is_open) {
            op_free(decoder->of);
            decoder->is_open = FALSE;
        }
        XDisposePtr(decoder);
    }
}

#endif // USE_OPUS_DECODER

#if USE_OPUS_ENCODER == TRUE

typedef struct {
    unsigned char *data;
    uint32_t size;
    uint32_t capacity;
} XOpusMemBuf;

typedef struct {
    OpusEncoder *encoder;
    ogg_stream_state os;
    bool stream_initialized;

    uint32_t input_sample_rate;
    uint32_t channels;
    uint32_t bitrate;
    uint32_t preskip;

    double resample_step;
    double resample_pos;

    int16_t *input_fifo;
    uint32_t input_fifo_frames;
    uint32_t input_fifo_capacity;

    int16_t *frame_pcm;
    uint32_t frame_fill;

    unsigned char *packet_buf;
    uint32_t packet_buf_size;

    ogg_int64_t granule_pos;
    ogg_int64_t packet_no;
    bool headers_queued;
    bool prepended_preskip_silence; /* TRUE if preskip silence was prepended before real audio */
} XOpusEncoder;

static int PV_OpusMemBufAppend(XOpusMemBuf *buf, const unsigned char *bytes, uint32_t len)
{
    uint32_t newSize;
    uint32_t newCap;
    unsigned char *grown;

    if (!buf || !bytes || len == 0) return 0;

    newSize = buf->size + len;
    if (newSize > buf->capacity)
    {
        newCap = buf->capacity ? (buf->capacity * 2) : 65536;
        while (newCap < newSize)
        {
            newCap *= 2;
        }

        grown = (unsigned char *)XNewPtr((int32_t)newCap);
        if (!grown) return -1;

        if (buf->data && buf->size)
        {
            XBlockMove(buf->data, grown, (int32_t)buf->size);
            XDisposePtr((XPTR)buf->data);
        }
        buf->data = grown;
        buf->capacity = newCap;
    }

    XBlockMove((void *)bytes, buf->data + buf->size, (int32_t)len);
    buf->size = newSize;
    return 0;
}

static int PV_EnsureInputFifoCapacity(XOpusEncoder *enc, uint32_t neededFrames)
{
    uint32_t needed;
    uint32_t newCap;
    int16_t *grown;

    needed = enc->input_fifo_frames + neededFrames;
    if (needed <= enc->input_fifo_capacity) return 0;

    newCap = enc->input_fifo_capacity ? enc->input_fifo_capacity : 4096;
    while (newCap < needed)
    {
        newCap *= 2;
    }

    grown = (int16_t *)XNewPtr((int32_t)(newCap * enc->channels * sizeof(int16_t)));
    if (!grown) return -1;

    if (enc->input_fifo && enc->input_fifo_frames)
    {
        XBlockMove(enc->input_fifo,
                   grown,
                   (int32_t)(enc->input_fifo_frames * enc->channels * sizeof(int16_t)));
        XDisposePtr((XPTR)enc->input_fifo);
    }

    enc->input_fifo = grown;
    enc->input_fifo_capacity = newCap;
    return 0;
}

static long PV_WriteOggPages(XOpusEncoder *enc, XFILE output_file, XOpusMemBuf *mem, bool flushAll)
{
    ogg_page og;
    long total = 0;

    while (1)
    {
        int result = flushAll ? ogg_stream_flush(&enc->os, &og)
                              : ogg_stream_pageout(&enc->os, &og);
        if (result == 0) break;

        if (output_file)
        {
            if (XFileWrite(output_file, (XPTRC)og.header, og.header_len) == -1) return -1;
            if (XFileWrite(output_file, (XPTRC)og.body, og.body_len) == -1) return -1;
            total += (long)(og.header_len + og.body_len);
        }
        else if (mem)
        {
            if (PV_OpusMemBufAppend(mem, (const unsigned char *)og.header, (uint32_t)og.header_len) != 0) return -1;
            if (PV_OpusMemBufAppend(mem, (const unsigned char *)og.body, (uint32_t)og.body_len) != 0) return -1;
            total += (long)(og.header_len + og.body_len);
        }
        else
        {
            return -1;
        }
    }

    return total;
}

static int PV_QueueOpusHeaders(XOpusEncoder *enc)
{
    unsigned char head[19];
    const char vendor[] = "NeoBAE";
    unsigned char tags[8 + 4 + sizeof(vendor) - 1 + 4];
    ogg_packet op;

    /* OpusHead */
    XSetMemory(head, sizeof(head), 0);
    XBlockMove((void *)"OpusHead", head, 8);
    head[8] = 1;
    head[9] = (unsigned char)enc->channels;
    head[10] = (unsigned char)(enc->preskip & 0xFFU);
    head[11] = (unsigned char)((enc->preskip >> 8) & 0xFFU);
    head[12] = (unsigned char)(enc->input_sample_rate & 0xFF);
    head[13] = (unsigned char)((enc->input_sample_rate >> 8) & 0xFF);
    head[14] = (unsigned char)((enc->input_sample_rate >> 16) & 0xFF);
    head[15] = (unsigned char)((enc->input_sample_rate >> 24) & 0xFF);
    head[16] = 0;
    head[17] = 0;
    head[18] = 0;

    XSetMemory(&op, sizeof(op), 0);
    op.packet = head;
    op.bytes = (long)sizeof(head);
    op.b_o_s = 1;
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = enc->packet_no++;
    if (ogg_stream_packetin(&enc->os, &op) != 0) return -1;

    /* OpusTags */
    XSetMemory(tags, sizeof(tags), 0);
    XBlockMove((void *)"OpusTags", tags, 8);
    tags[8] = (unsigned char)((sizeof(vendor) - 1) & 0xFF);
    tags[9] = (unsigned char)(((sizeof(vendor) - 1) >> 8) & 0xFF);
    tags[10] = (unsigned char)(((sizeof(vendor) - 1) >> 16) & 0xFF);
    tags[11] = (unsigned char)(((sizeof(vendor) - 1) >> 24) & 0xFF);
    XBlockMove((void *)vendor, tags + 12, (int32_t)(sizeof(vendor) - 1));
    tags[12 + (sizeof(vendor) - 1)] = 0;
    tags[13 + (sizeof(vendor) - 1)] = 0;
    tags[14 + (sizeof(vendor) - 1)] = 0;
    tags[15 + (sizeof(vendor) - 1)] = 0;

    XSetMemory(&op, sizeof(op), 0);
    op.packet = tags;
    op.bytes = (long)sizeof(tags);
    op.b_o_s = 0;
    op.e_o_s = 0;
    op.granulepos = 0;
    op.packetno = enc->packet_no++;
    if (ogg_stream_packetin(&enc->os, &op) != 0) return -1;

    enc->headers_queued = TRUE;
    return 0;
}

static int PV_AppendInputPcm(XOpusEncoder *enc, const int16_t *pcm, uint32_t frames)
{
    if (frames == 0) return 0;
    if (PV_EnsureInputFifoCapacity(enc, frames) != 0) return -1;

    XBlockMove((void *)pcm,
               enc->input_fifo + (enc->input_fifo_frames * enc->channels),
               (int32_t)(frames * enc->channels * sizeof(int16_t)));
    enc->input_fifo_frames += frames;
    return 0;
}

static uint32_t PV_GenerateResampledFrames(XOpusEncoder *enc, int16_t *dst, uint32_t wanted)
{
    uint32_t produced;

    produced = 0;

    if (enc->input_sample_rate == 48000)
    {
        uint32_t take = enc->input_fifo_frames;
        if (take > wanted) take = wanted;

        if (take)
        {
            XBlockMove(enc->input_fifo,
                       dst,
                       (int32_t)(take * enc->channels * sizeof(int16_t)));

            enc->input_fifo_frames -= take;
            if (enc->input_fifo_frames)
            {
                XBlockMove(enc->input_fifo + (take * enc->channels),
                           enc->input_fifo,
                           (int32_t)(enc->input_fifo_frames * enc->channels * sizeof(int16_t)));
            }
        }
        return take;
    }

    while (produced < wanted)
    {
        uint32_t i0 = (uint32_t)enc->resample_pos;
        double frac;
        uint32_t ch;

        if (i0 + 1 >= enc->input_fifo_frames)
        {
            break;
        }

        frac = enc->resample_pos - (double)i0;
        for (ch = 0; ch < enc->channels; ++ch)
        {
            int16_t s0 = enc->input_fifo[(i0 * enc->channels) + ch];
            int16_t s1 = enc->input_fifo[((i0 + 1) * enc->channels) + ch];
            double v = (double)s0 + (((double)s1 - (double)s0) * frac);
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            dst[(produced * enc->channels) + ch] = (int16_t)v;
        }

        enc->resample_pos += enc->resample_step;
        produced++;
    }

    {
        uint32_t consume = (uint32_t)enc->resample_pos;
        if (consume > 0)
        {
            if (consume > enc->input_fifo_frames)
            {
                consume = enc->input_fifo_frames;
            }
            enc->input_fifo_frames -= consume;
            if (enc->input_fifo_frames)
            {
                XBlockMove(enc->input_fifo + (consume * enc->channels),
                           enc->input_fifo,
                           (int32_t)(enc->input_fifo_frames * enc->channels * sizeof(int16_t)));
            }
            enc->resample_pos -= (double)consume;
        }
    }

    return produced;
}

static long PV_PushEncodedPacket(XOpusEncoder *enc, int packetBytes, XFILE output_file, XOpusMemBuf *mem, bool endOfStream)
{
    ogg_packet op;

    XSetMemory(&op, sizeof(op), 0);
    op.packet = enc->packet_buf;
    op.bytes = packetBytes;
    op.b_o_s = 0;
    op.e_o_s = endOfStream ? 1 : 0;
    /* Ogg Opus granule position tracks decoded sample count at 48 kHz.
     * For EOS packets:
     *   - If preskip silence was prepended before the real audio, the
     *     granule position already includes those preskip frames, so
     *     op_pcm_total() = granule_pos - preskip = (preskip + N) - preskip = N.
     *     Do NOT add preskip again.
     *   - Otherwise (legacy no-padding path), add preskip to the granule
     *     so that op_pcm_total() = (N + preskip) - preskip = N. */
    op.granulepos = endOfStream
                        ? (enc->prepended_preskip_silence
                               ? enc->granule_pos
                               : (enc->granule_pos + (ogg_int64_t)enc->preskip))
                        : enc->granule_pos;
    op.packetno = enc->packet_no++;

    if (ogg_stream_packetin(&enc->os, &op) != 0)
    {
        return -1;
    }

    return PV_WriteOggPages(enc, output_file, mem, endOfStream);
}

static long PV_EncodeQueuedFrames(XOpusEncoder *enc, XFILE output_file, XOpusMemBuf *mem)
{
    long totalWritten;

    totalWritten = 0;

    while (1)
    {
        uint32_t needed = 960 - enc->frame_fill;
        uint32_t produced;
        int packetBytes;
        long wrote;

        produced = PV_GenerateResampledFrames(enc,
                                              enc->frame_pcm + (enc->frame_fill * enc->channels),
                                              needed);
        enc->frame_fill += produced;

        if (enc->frame_fill < 960)
        {
            break;
        }

        packetBytes = opus_encode(enc->encoder,
                                  (const opus_int16 *)enc->frame_pcm,
                                  960,
                                  enc->packet_buf,
                                  (opus_int32)enc->packet_buf_size);
        if (packetBytes < 0)
        {
            return -1;
        }

        enc->granule_pos += 960;
        wrote = PV_PushEncodedPacket(enc, packetBytes, output_file, mem, FALSE);
        if (wrote < 0)
        {
            return -1;
        }
        totalWritten += wrote;
        enc->frame_fill = 0;
    }

    return totalWritten;
}

static long PV_FlushEncoderInternal(XOpusEncoder *enc, XFILE output_file, XOpusMemBuf *mem)
{
    long totalWritten;
    int packetBytes;
    long wrote;

    totalWritten = 0;

    wrote = PV_EncodeQueuedFrames(enc, output_file, mem);
    if (wrote < 0) return -1;
    totalWritten += wrote;

    /* Flush any remaining resampled/interpolated frames by padding the final packet. */
    if (enc->frame_fill > 0)
    {
        uint32_t remainingFrames = 960 - enc->frame_fill;

        if (remainingFrames > 0)
        {
            XSetMemory(enc->frame_pcm + (enc->frame_fill * enc->channels),
                       (int32_t)(remainingFrames * enc->channels * sizeof(int16_t)),
                       0);
        }

        packetBytes = opus_encode(enc->encoder,
                                  (const opus_int16 *)enc->frame_pcm,
                                  960,
                                  enc->packet_buf,
                                  (opus_int32)enc->packet_buf_size);
        if (packetBytes < 0)
        {
            return -1;
        }

        enc->granule_pos += enc->frame_fill;
        enc->frame_fill = 0;

        wrote = PV_PushEncodedPacket(enc, packetBytes, output_file, mem, TRUE);
        if (wrote < 0)
        {
            return -1;
        }
        totalWritten += wrote;
    }
    else
    {
        wrote = PV_WriteOggPages(enc, output_file, mem, TRUE);
        if (wrote < 0)
        {
            return -1;
        }
        totalWritten += wrote;
    }

    return totalWritten;
}

void* XInitOpusEncoder(uint32_t sample_rate, uint32_t channels, uint32_t bitrate, uint32_t mode)
{
    XOpusEncoder *enc;
    int err;
    int application;
    opus_int32 lookahead;

    if (channels < 1 || channels > 2)
    {
        return NULL;
    }
    if (sample_rate == 0)
    {
        sample_rate = 48000;
    }
    if (bitrate < 6000)
    {
        bitrate = 6000;
    }
    if (bitrate > 510000)
    {
        bitrate = 510000;
    }

    enc = (XOpusEncoder *)XNewPtr(sizeof(XOpusEncoder));
    if (!enc) return NULL;
    XSetMemory(enc, sizeof(XOpusEncoder), 0);

    enc->input_sample_rate = sample_rate;
    enc->channels = channels;
    enc->bitrate = bitrate;
    enc->resample_step = (double)sample_rate / 48000.0;
    enc->resample_pos = 0.0;

    enc->frame_pcm = (int16_t *)XNewPtr((int32_t)(960 * channels * sizeof(int16_t)));
    if (!enc->frame_pcm)
    {
        XDisposePtr((XPTR)enc);
        return NULL;
    }

    enc->packet_buf_size = 4000;
    enc->packet_buf = (unsigned char *)XNewPtr((int32_t)enc->packet_buf_size);
    if (!enc->packet_buf)
    {
        XDisposePtr((XPTR)enc->frame_pcm);
        XDisposePtr((XPTR)enc);
        return NULL;
    }

    application = OPUS_APPLICATION_AUDIO;
    switch (mode)
    {
        case 2:
            application = OPUS_APPLICATION_VOIP;
            break;
        case 1:
        case 0:
        default:
            application = OPUS_APPLICATION_AUDIO;
            break;
    }

    enc->encoder = opus_encoder_create(48000, (int)channels, application, &err);
    if (!enc->encoder || err != OPUS_OK)
    {
        if (enc->encoder)
        {
            opus_encoder_destroy(enc->encoder);
        }
        XDisposePtr((XPTR)enc->packet_buf);
        XDisposePtr((XPTR)enc->frame_pcm);
        XDisposePtr((XPTR)enc);
        return NULL;
    }

    opus_encoder_ctl(enc->encoder, OPUS_SET_BITRATE((opus_int32)bitrate));
    opus_encoder_ctl(enc->encoder, OPUS_SET_VBR(1));
    if (mode == 1)
    {
        opus_encoder_ctl(enc->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    }
    else if (mode == 2)
    {
        opus_encoder_ctl(enc->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }

    lookahead = 0;
    if (opus_encoder_ctl(enc->encoder, OPUS_GET_LOOKAHEAD(&lookahead)) == OPUS_OK && lookahead > 0)
    {
        enc->preskip = (uint32_t)lookahead;
    }
    else
    {
        enc->preskip = 0;
    }
    enc->granule_pos = 0;

    if (ogg_stream_init(&enc->os, rand()) != 0)
    {
        opus_encoder_destroy(enc->encoder);
        XDisposePtr((XPTR)enc->packet_buf);
        XDisposePtr((XPTR)enc->frame_pcm);
        XDisposePtr((XPTR)enc);
        return NULL;
    }
    enc->stream_initialized = TRUE;

    if (PV_QueueOpusHeaders(enc) != 0)
    {
        ogg_stream_clear(&enc->os);
        opus_encoder_destroy(enc->encoder);
        XDisposePtr((XPTR)enc->packet_buf);
        XDisposePtr((XPTR)enc->frame_pcm);
        XDisposePtr((XPTR)enc);
        return NULL;
    }

    return enc;
}

long XWriteOpusHeader(void *encoder_handle, XFILE output_file)
{
    XOpusEncoder *enc = (XOpusEncoder *)encoder_handle;

    if (!enc || !enc->stream_initialized || !output_file)
    {
        return -1;
    }

    return PV_WriteOggPages(enc, output_file, NULL, TRUE);
}

long XEncodeOpusData(void *encoder_handle, const int16_t *pcm_interleaved, long frames, XFILE output_file)
{
    XOpusEncoder *enc = (XOpusEncoder *)encoder_handle;

    if (!enc || !enc->stream_initialized || !pcm_interleaved || frames < 0 || !output_file)
    {
        return -1;
    }

    if (PV_AppendInputPcm(enc, pcm_interleaved, (uint32_t)frames) != 0)
    {
        return -1;
    }

    return PV_EncodeQueuedFrames(enc, output_file, NULL);
}

long XFlushOpusEncoder(void *encoder_handle, XFILE output_file)
{
    XOpusEncoder *enc = (XOpusEncoder *)encoder_handle;

    if (!enc || !enc->stream_initialized || !output_file)
    {
        return -1;
    }

    return PV_FlushEncoderInternal(enc, output_file, NULL);
}

void XCloseOpusEncoder(void *encoder_handle)
{
    XOpusEncoder *enc = (XOpusEncoder *)encoder_handle;

    if (!enc) return;

    if (enc->stream_initialized)
    {
        ogg_stream_clear(&enc->os);
        enc->stream_initialized = FALSE;
    }

    if (enc->encoder)
    {
        opus_encoder_destroy(enc->encoder);
        enc->encoder = NULL;
    }

    if (enc->packet_buf)
    {
        XDisposePtr((XPTR)enc->packet_buf);
        enc->packet_buf = NULL;
    }
    if (enc->frame_pcm)
    {
        XDisposePtr((XPTR)enc->frame_pcm);
        enc->frame_pcm = NULL;
    }
    if (enc->input_fifo)
    {
        XDisposePtr((XPTR)enc->input_fifo);
        enc->input_fifo = NULL;
    }

    XDisposePtr((XPTR)enc);
}

OPErr XEncodeOpusToMemory(GM_Waveform const *src, uint32_t bitrate, uint32_t mode,
                          XPTR *outData, uint32_t *outSize)
{
    XOpusEncoder *enc;
    XOpusMemBuf buf;
    uint32_t frames;
    uint32_t channels;
    uint32_t encodeChannels;
    uint32_t f;
    uint32_t chunkFrames;
    int16_t *chunkPcm;
    long wrote;
    bool collapseDualMono;

    if (!src || !src->theWaveform || !outData || !outSize)
    {
        return PARAM_ERR;
    }
    if (src->compressionType != (uint32_t)C_NONE)
    {
        return PARAM_ERR;
    }
    if (src->channels < 1 || src->channels > 2)
    {
        return PARAM_ERR;
    }

    *outData = NULL;
    *outSize = 0;
    XSetMemory(&buf, sizeof(buf), 0);

    channels = src->channels;
    collapseDualMono = FALSE;
    encodeChannels = channels;

    /*
     * Preserve mono quality when upstream hands us dual-mono stereo PCM.
     * If every L/R frame pair matches exactly, encode a single channel.
     */
    if (channels == 2 && src->waveFrames > 0)
    {
        uint32_t frame;
        bool isDualMono;

        isDualMono = TRUE;
        if (src->bitSize == 16)
        {
            int16_t const *pcm16;
            pcm16 = (int16_t const *)src->theWaveform;
            for (frame = 0; frame < src->waveFrames; ++frame)
            {
                if (pcm16[frame * 2] != pcm16[frame * 2 + 1])
                {
                    isDualMono = FALSE;
                    break;
                }
            }
        }
        else if (src->bitSize == 8)
        {
            unsigned char const *pcm8;
            pcm8 = (unsigned char const *)src->theWaveform;
            for (frame = 0; frame < src->waveFrames; ++frame)
            {
                if (pcm8[frame * 2] != pcm8[frame * 2 + 1])
                {
                    isDualMono = FALSE;
                    break;
                }
            }
        }
        else
        {
            isDualMono = FALSE;
        }

        if (isDualMono)
        {
            collapseDualMono = TRUE;
            encodeChannels = 1;
        }
    }

    enc = (XOpusEncoder *)XInitOpusEncoder((uint32_t)(src->sampledRate >> 16),
                                           encodeChannels,
                                           bitrate,
                                           mode);
    if (!enc)
    {
        return MEMORY_ERR;
    }

    wrote = PV_WriteOggPages(enc, NULL, &buf, TRUE);
    if (wrote < 0)
    {
        XCloseOpusEncoder(enc);
        if (buf.data) XDisposePtr((XPTR)buf.data);
        return BAD_FILE;
    }

    frames = src->waveFrames;
    chunkFrames = 4096;
    chunkPcm = (int16_t *)XNewPtr((int32_t)(chunkFrames * encodeChannels * sizeof(int16_t)));
    if (!chunkPcm)
    {
        XCloseOpusEncoder(enc);
        if (buf.data) XDisposePtr((XPTR)buf.data);
        return MEMORY_ERR;
    }

    /* No preskip priming — let the encoder's internal state handle
     * convergence naturally.  The EOS granule path without prepended
     * silence adds preskip to the final granule position so that
     * op_pcm_total() = (N + preskip) - preskip = N. */

    f = 0;
    while (f < frames)
    {
        uint32_t count = frames - f;
        if (count > chunkFrames) count = chunkFrames;

        if (src->bitSize == 16)
        {
            if (collapseDualMono)
            {
                uint32_t i;
                int16_t const *src16;

                src16 = ((int16_t const *)src->theWaveform) + (f * channels);
                for (i = 0; i < count; ++i)
                {
                    chunkPcm[i] = src16[i * 2];
                }
            }
            else
            {
                XBlockMove(((int16_t const *)src->theWaveform) + (f * channels),
                           chunkPcm,
                           (int32_t)(count * channels * sizeof(int16_t)));
            }
        }
        else if (src->bitSize == 8)
        {
            uint32_t i;
            unsigned char const *pcm8 = (unsigned char const *)src->theWaveform;
            if (collapseDualMono)
            {
                for (i = 0; i < count; ++i)
                {
                    chunkPcm[i] = (int16_t)(((int)pcm8[(f * channels) + (i * 2)] - 128) << 8);
                }
            }
            else
            {
                for (i = 0; i < count * channels; ++i)
                {
                    chunkPcm[i] = (int16_t)(((int)pcm8[(f * channels) + i] - 128) << 8);
                }
            }
        }
        else
        {
            XDisposePtr((XPTR)chunkPcm);
            XCloseOpusEncoder(enc);
            if (buf.data) XDisposePtr((XPTR)buf.data);
            return PARAM_ERR;
        }

        if (PV_AppendInputPcm(enc, chunkPcm, count) != 0)
        {
            XDisposePtr((XPTR)chunkPcm);
            XCloseOpusEncoder(enc);
            if (buf.data) XDisposePtr((XPTR)buf.data);
            return MEMORY_ERR;
        }

        wrote = PV_EncodeQueuedFrames(enc, NULL, &buf);
        if (wrote < 0)
        {
            XDisposePtr((XPTR)chunkPcm);
            XCloseOpusEncoder(enc);
            if (buf.data) XDisposePtr((XPTR)buf.data);
            return BAD_FILE;
        }

        f += count;
    }

    XDisposePtr((XPTR)chunkPcm);

    wrote = PV_FlushEncoderInternal(enc, NULL, &buf);
    XCloseOpusEncoder(enc);
    if (wrote < 0)
    {
        if (buf.data) XDisposePtr((XPTR)buf.data);
        return BAD_FILE;
    }

    if (!buf.data || buf.size == 0)
    {
        if (buf.data) XDisposePtr((XPTR)buf.data);
        return BAD_FILE;
    }

    *outData = (XPTR)buf.data;
    *outSize = buf.size;
    return NO_ERR;
}

#endif // USE_OPUS_ENCODER
