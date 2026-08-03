/*
 * XWMAFiles.c - WMA audio decode support for NeoBAE
 * 
 * Implements XExpandWMA() following the same interface pattern as
 * XExpandQOA(), XExpandADX(), etc.
 *
 * Handles both raw WMA wave data (wFormatTag 0x0160/0x0161) and
 * the ASF container wrapper.
 */

#include "X_Formats.h"

#if USE_WMA_SUPPORT == TRUE

#include "GenSnd.h"
#include "libwma/wma_decoder.h"
#include <string.h>

/* ---- ASF container detection ---- */
/* ASF object GUID prefix: A1DCAB8C-47A9-CF11-8EE4-00C00C205365 for header objects */
#define ASF_HEADER_GUID 0x3026B275 /* first 4 bytes of the ASF header object GUID in LE: 75 B2 26 30 */

static int PV_IsASFData(const unsigned char *data, uint32_t size)
{
    if (size < 16) return 0;
    /* ASF starts with ASF_Header_Object GUID: 30 26 B2 75 8E 66 CF 11 A6 D9 00 AA 00 62 CE 6C */
    const unsigned char asf_guid[16] = {
        0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
        0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
    };
    return (memcmp(data, asf_guid, 16) == 0) ? 1 : 0;
}

static uint32_t PV_ReadLE(const uint8_t *data, uint32_t bytes)
{
    uint32_t value = 0;
    uint32_t i;

    for (i = 0; i < bytes; i++)
        value |= (uint32_t)data[i] << (i * 8);
    return value;
}

static uint32_t PV_ASFFieldWidth(uint32_t type)
{
    static const uint8_t widths[4] = { 0, 1, 2, 4 };
    return widths[type & 3];
}

/* Extract complete codec media objects from ASF packet payloads. */
static uint8_t *PV_ExtractASFWMABlocks(const uint8_t *data, uint32_t data_size,
                                       uint32_t packet_size, uint8_t stream_id,
                                       uint32_t block_align, uint32_t *out_size)
{
    uint8_t *output;
    uint32_t packet_offset = 0;
    uint32_t output_size = 0;

    if (!data || !out_size || packet_size < 16 || block_align == 0)
        return NULL;

    output = (uint8_t *)XNewPtr(data_size);
    if (!output)
        return NULL;

    while (packet_offset + packet_size <= data_size) {
        const uint8_t *packet = data + packet_offset;
        const uint8_t *cursor = packet;
        const uint8_t *packet_end = packet + packet_size;
        uint8_t ec_flags, length_flags, property_flags;
        uint32_t packet_length, padding_length;
        uint32_t payload_count = 1;
        uint32_t payload_length_type = 0;
        uint32_t i;

#define ASF_TAKE(count) do { if ((uint32_t)(packet_end - cursor) < (count)) goto bad_packet; } while (0)
#define ASF_READ_VAR(type, value) do { \
    uint32_t width__ = PV_ASFFieldWidth(type); \
    ASF_TAKE(width__); \
    value = width__ ? PV_ReadLE(cursor, width__) : 0; \
    cursor += width__; \
} while (0)

        ASF_TAKE(1);
        ec_flags = *cursor++;
        if (ec_flags & 0x80) {
            uint32_t ec_length = ec_flags & 0x0F;
            ASF_TAKE(ec_length);
            cursor += ec_length;
        }

        ASF_TAKE(2);
        length_flags = *cursor++;
        property_flags = *cursor++;

        ASF_READ_VAR((length_flags >> 5) & 3, packet_length);
        {
            uint32_t ignored;
            ASF_READ_VAR((length_flags >> 1) & 3, ignored); /* sequence / pad field; value unused */
            (void)ignored;
        }
        ASF_READ_VAR((length_flags >> 3) & 3, padding_length);

        if (packet_length == 0)
            packet_length = packet_size;
        if (packet_length > packet_size || padding_length > packet_length)
            goto bad_packet;
        packet_end = packet + packet_length - padding_length;

        ASF_TAKE(6); /* send time + duration */
        cursor += 6;

        if (length_flags & 1) {
            uint8_t multiple_flags;
            ASF_TAKE(1);
            multiple_flags = *cursor++;
            payload_count = multiple_flags & 0x3F;
            payload_length_type = multiple_flags >> 6;
        }

        for (i = 0; i < payload_count; i++) {
            uint32_t stream_value, object_number, object_offset;
            uint32_t replicated_length, payload_length;
            uint32_t object_size = 0;

            ASF_READ_VAR((property_flags >> 6) & 3, stream_value);
            ASF_READ_VAR((property_flags >> 4) & 3, object_number);
            ASF_READ_VAR((property_flags >> 2) & 3, object_offset);
            ASF_READ_VAR(property_flags & 3, replicated_length);
            (void)object_number;

            ASF_TAKE(replicated_length);
            if (replicated_length >= 4)
                object_size = PV_ReadLE(cursor, 4);
            cursor += replicated_length;

            if (length_flags & 1) {
                ASF_READ_VAR(payload_length_type, payload_length);
            } else {
                payload_length = (uint32_t)(packet_end - cursor);
            }

            ASF_TAKE(payload_length);
            if ((stream_value & 0x7F) == stream_id) {
                /* Compressed and fragmented ASF payloads need reassembly. */
                if (replicated_length == 1 || object_offset != 0 ||
                    (object_size && object_size != payload_length) ||
                    payload_length != block_align)
                    goto bad_packet;
                if (output_size + payload_length > data_size)
                    goto bad_packet;
                XBlockMove(cursor, output + output_size, payload_length);
                output_size += payload_length;
            }
            cursor += payload_length;
        }

        packet_offset += packet_size;
        continue;

bad_packet:
        XDisposePtr((XPTR)output);
        return NULL;
#undef ASF_READ_VAR
#undef ASF_TAKE
    }

    if (output_size == 0) {
        XDisposePtr((XPTR)output);
        return NULL;
    }

    *out_size = output_size;
    return output;
}

/* ---- WMA chunk helper: find codec info in a RIFF WAVE fmt chunk ---- */
static int PV_ParseWMAFormat(const uint8_t *fmt_data, uint32_t fmt_size,
                              WMAFormatInfo *info)
{
    if (fmt_size < 18) return 0; /* need at least wFormatTag + cbSize */

    info->codec_id = (uint16_t)(fmt_data[0] | (fmt_data[1] << 8));
    info->channels = (uint16_t)(fmt_data[2] | (fmt_data[3] << 8));
    info->rate     = (uint32_t)(fmt_data[4] | (fmt_data[5] << 8) |
                                 (fmt_data[6] << 16) | (fmt_data[7] << 24));
    info->bitrate  = (uint32_t)(fmt_data[8] | (fmt_data[9] << 8) |
                                 (fmt_data[10] << 16) | (fmt_data[11] << 24));
    info->blockalign = (uint16_t)(fmt_data[12] | (fmt_data[13] << 8));
    info->bitspersample = (uint16_t)(fmt_data[14] | (fmt_data[15] << 8));
    info->datalen  = (uint16_t)(fmt_data[16] | (fmt_data[17] << 8));

    if (info->codec_id != 0x0160 && info->codec_id != 0x0161) return 0;

    info->data = NULL;
    if (info->datalen > 0 && fmt_size >= 18U + info->datalen) {
        info->data = fmt_data + 18;
        if (info->datalen > 46) info->datalen = 46;
    }

    /* Compute bitrate if stored as 0 (some WMA encoders do this) */
    if (info->bitrate == 0 && info->blockalign > 0 && info->rate > 0) {
        info->bitrate = (uint32_t)((uint64_t)info->blockalign * (uint64_t)info->rate * 8 / 1024);
    }

    return 1;
}

/* ---- Decode WMA data from raw compressed bytes ---- */
OPErr XExpandWMA(GM_Waveform const* src, uint32_t startFrame, GM_Waveform* dst)
{
    WMADecodeContext *wma_ctx = NULL;
    WMAFormatInfo wma_info;
    const uint8_t *fmt_data = NULL;
    uint32_t fmt_size = 0;
    const uint8_t *wma_data = NULL;
    uint32_t wma_data_size = 0;
    uint8_t *owned_wma_data = NULL;
    const uint8_t *probe;
    uint32_t probe_size;

    if (!src || !dst || !src->theWaveform || src->waveSize == 0)
        return PARAM_ERR;

    probe = (const uint8_t *)src->theWaveform;
    probe_size = src->waveSize;

    XSetMemory(&wma_info, sizeof(wma_info), 0);
    XSetMemory(dst, sizeof(GM_Waveform), 0);

    /* Check if data is wrapped in an ASF container */
    if (PV_IsASFData(probe, probe_size)) {
        const unsigned char stream_props_guid[16] = {
            0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
            0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65
        };
        const unsigned char header_guid[16] = {
            0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
            0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
        };
        const unsigned char data_obj_guid[16] = {
            0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
            0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
        };

        uint32_t pos = 0;
        int found_wfx = 0;
        int found_data_obj = 0;
        uint32_t data_obj_offset = 0;
        uint32_t data_obj_size = 0;
        uint32_t packet_size = 0;
        uint8_t stream_id = 0;
        const unsigned char file_props_guid[16] = {
            0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
            0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65
        };

        while (pos + 24 <= probe_size) {
            uint64_t obj_size;
            obj_size = (uint64_t)probe[pos + 16] | ((uint64_t)probe[pos + 17] << 8) |
                       ((uint64_t)probe[pos + 18] << 16) | ((uint64_t)probe[pos + 19] << 24) |
                       ((uint64_t)probe[pos + 20] << 32) | ((uint64_t)probe[pos + 21] << 40) |
                       ((uint64_t)probe[pos + 22] << 48) | ((uint64_t)probe[pos + 23] << 56);

            if (obj_size < 24) break;
            if (pos + obj_size > probe_size) obj_size = probe_size - pos;

            /* Header Object contains sub-objects including Stream Properties */
            if (memcmp(probe + pos, header_guid, 16) == 0) {
                /* After GUID(16)+Size(8), Header has: NumObjects(4)+Reserved1(1)+Reserved2(1)=6 bytes, then data */
                uint32_t sub_pos = pos + 30;
                uint32_t sub_end = (uint32_t)(pos + obj_size);

                while (sub_pos + 24 <= sub_end) {
                    uint64_t sub_size;
                    sub_size = (uint64_t)probe[sub_pos + 16] | ((uint64_t)probe[sub_pos + 17] << 8) |
                               ((uint64_t)probe[sub_pos + 18] << 16) | ((uint64_t)probe[sub_pos + 19] << 24) |
                               ((uint64_t)probe[sub_pos + 20] << 32) | ((uint64_t)probe[sub_pos + 21] << 40) |
                               ((uint64_t)probe[sub_pos + 22] << 48) | ((uint64_t)probe[sub_pos + 23] << 56);

                    if (sub_size < 24) break;
                    if (sub_pos + sub_size > sub_end) sub_size = sub_end - sub_pos;

                    if (memcmp(probe + sub_pos, stream_props_guid, 16) == 0) {
                        /* Stream Properties: GUID(16)+Size(8) then:
                         * StreamType GUID(16) + ErrorCorrection Type GUID(16) + TimeOffset(8) +
                         * TypeSpecificDataLen(4) + ErrorCorrectionDataLen(4) + Flags(2) + Reserved(4)
                         * = 54 bytes of header. WAVEFORMATEX starts at pos+24+54 = pos+78 */
                        uint32_t wfx_off = sub_pos + 78;
                        if (wfx_off + 18 <= (uint32_t)(sub_pos + sub_size)) {
                            uint16_t cbSize = (uint16_t)(probe[wfx_off + 16] | (probe[wfx_off + 17] << 8));
                            uint32_t wfx_len = 18 + (uint32_t)cbSize;
                            if (wfx_off + wfx_len > (uint32_t)(sub_pos + sub_size))
                                wfx_len = (uint32_t)(sub_pos + sub_size) - wfx_off;
                            fmt_data = probe + wfx_off;
                            fmt_size = wfx_len;
                            stream_id = (uint8_t)(PV_ReadLE(probe + sub_pos + 72, 2) & 0x7F);
                            found_wfx = 1;
                        }
                    }
                    if (memcmp(probe + sub_pos, file_props_guid, 16) == 0 && sub_size >= 100) {
                        uint32_t min_packet = PV_ReadLE(probe + sub_pos + 92, 4);
                        uint32_t max_packet = PV_ReadLE(probe + sub_pos + 96, 4);
                        if (min_packet == max_packet)
                            packet_size = max_packet;
                    }
                    sub_pos += (uint32_t)sub_size;
                }
            }

            /* Data Object is top-level */
            if (memcmp(probe + pos, data_obj_guid, 16) == 0) {
                found_data_obj = 1;
                /* Data Object: GUID(16)+Size(8)+FileID(16)+TotalPackets(8)+Reserved(2)=50 */
                data_obj_offset = pos + 50;
                data_obj_size = (uint32_t)(obj_size - 50);
                if (data_obj_offset + data_obj_size > probe_size)
                    data_obj_size = probe_size - data_obj_offset;
            }

            pos += (uint32_t)obj_size;
        }

        if (!found_wfx || !found_data_obj) {
            return BAD_FILE_TYPE;
        }

        if (!PV_ParseWMAFormat(fmt_data, fmt_size, &wma_info) ||
            packet_size == 0 || stream_id == 0)
            return BAD_FILE_TYPE;

        owned_wma_data = PV_ExtractASFWMABlocks(probe + data_obj_offset,
                                                data_obj_size, packet_size,
                                                stream_id, wma_info.blockalign,
                                                &wma_data_size);
        if (!owned_wma_data)
            return BAD_FILE_TYPE;
        wma_data = owned_wma_data;
    } else {
        /* Raw WMA frames (block_align-sized). Prefer ASF/WAV containers that
         * carry a full WAVEFORMATEX; raw callers must fill GM_Waveform fields:
         *   channels, sampledRate, waveFrames=blockAlign, startLoop=avgBytesPerSec,
         *   endLoop=codec_id, and optional extradata prepended as:
         *   [uint16 cbSize][cbSize bytes][payload...] when numLoops==1. */
        wma_data = probe;
        wma_data_size = probe_size;

        wma_info.codec_id = (src->endLoop != 0) ? (uint16_t)src->endLoop : 0x0160;
        wma_info.channels = src->channels;
        wma_info.rate = (uint32_t)(src->sampledRate >> 16);
        wma_info.blockalign = (uint16_t)src->waveFrames;
        wma_info.bitspersample = 16;
        wma_info.bitrate = src->startLoop; /* nAvgBytesPerSec */
        wma_info.datalen = 0;
        wma_info.data = NULL;

        if (src->numLoops == 1 && wma_data_size >= 2) {
            uint16_t cb = (uint16_t)(wma_data[0] | (wma_data[1] << 8));
            if (cb > 0 && wma_data_size >= 2U + cb) {
                wma_info.datalen = cb > 46 ? 46 : cb;
                wma_info.data = wma_data + 2;
                wma_data += 2U + cb;
                wma_data_size -= 2U + cb;
            }
        }

        if (wma_info.bitrate == 0 && wma_info.blockalign > 0 && wma_info.rate > 0)
            wma_info.bitrate = (uint32_t)wma_info.blockalign * wma_info.rate / 1024;
    }

    /* If we have fmt_data from an ASF/WAV container, parse it */
    if (!PV_IsASFData(probe, probe_size) && fmt_data && fmt_size >= 18) {
        if (!PV_ParseWMAFormat(fmt_data, fmt_size, &wma_info)) {
            if (owned_wma_data) XDisposePtr((XPTR)owned_wma_data);
            return BAD_FILE_TYPE;
        }
    }

    /* Initialize WMA decoder (context is large; keep it on the heap). */
    wma_ctx = (WMADecodeContext *)XNewPtr(sizeof(WMADecodeContext));
    if (!wma_ctx) {
        if (owned_wma_data) XDisposePtr((XPTR)owned_wma_data);
        return MEMORY_ERR;
    }
    if (wma_decode_init(wma_ctx, &wma_info) < 0) {
        if (owned_wma_data) XDisposePtr((XPTR)owned_wma_data);
        XDisposePtr((XPTR)wma_ctx);
        return BAD_FILE_TYPE;
    }

    /*
     * Match DLS MSAUDIO1 priming: skip one IMDCT frame, not FFmpeg's two.
     * Two-frame skip drops the attack and shifts content so sample loops
     * (BAESound / full-file loop) no longer line up.
     */
    wma_ctx->skip_frames = 1;

    /* Estimate output buffer size */
    {
        int block_align = wma_ctx->block_align;
        uint32_t sample_rate = (uint32_t)wma_ctx->sample_rate;
        uint32_t nb_channels = (uint32_t)wma_ctx->nb_channels;
        uint32_t frame_len = (uint32_t)wma_ctx->frame_len;
        uint32_t num_superframes;
        /* Bit-reservoir packets can emit up to 15 frames (4-bit count). */
        int max_frames_per_sf = 16;
        float *frame_output;
        int frame_cap = max_frames_per_sf * (int)frame_len;

        if (block_align <= 0) block_align = 1;
        num_superframes = wma_data_size / (uint32_t)block_align + 2;
        if (num_superframes == 0) num_superframes = 1;

        uint32_t out_samples = num_superframes * frame_len;
        int16_t *pcm_out = (int16_t *)XNewPtr(out_samples * nb_channels * sizeof(int16_t));
        frame_output = (float *)XNewPtr((uint32_t)frame_cap * nb_channels * sizeof(float));
        if (!pcm_out || !frame_output) {
            if (pcm_out) XDisposePtr((XPTR)pcm_out);
            if (frame_output) XDisposePtr((XPTR)frame_output);
            if (owned_wma_data) XDisposePtr((XPTR)owned_wma_data);
            wma_decode_close(wma_ctx);
            XDisposePtr((XPTR)wma_ctx);
            return MEMORY_ERR;
        }

        dst->sampledRate = (XFIXED)((uint64_t)sample_rate << 16);
        dst->channels = (uint16_t)nb_channels;
        dst->bitSize = 16;
        dst->baseMidiPitch = src->baseMidiPitch ? src->baseMidiPitch : 60;

        {
            uint32_t sf_offset = 0, out_pos = 0;

            while (sf_offset < wma_data_size) {
                uint32_t sf_size = wma_data_size - sf_offset;
                int samples, i, ch;
                int skip = 0;

                if (sf_size > (uint32_t)block_align)
                    sf_size = (uint32_t)block_align;
                if (sf_size < 4) break;

                samples = wma_decode_superframe(wma_ctx, wma_data + sf_offset,
                                                (int)sf_size, frame_output,
                                                frame_cap);
                if (samples < 0) {
                    /* Resync: reservoir already cleared inside decoder. */
                    sf_offset += sf_size;
                    continue;
                }
                if (samples == 0) {
                    sf_offset += sf_size;
                    continue;
                }

                /* Drop decoder priming frames from the start of the stream. */
                while (skip < samples && wma_ctx->skip_frames > 0) {
                    skip += (int)frame_len;
                    wma_ctx->skip_frames--;
                }
                if (skip >= samples) {
                    sf_offset += sf_size;
                    continue;
                }

                {
                    int keep = samples - skip;
                    if (out_pos + (uint32_t)keep > out_samples) {
                        uint32_t ns = out_samples * 2 + (uint32_t)keep;
                        int16_t *np = (int16_t *)XNewPtr(ns * nb_channels * sizeof(int16_t));
                        if (!np) break;
                        XBlockMove(pcm_out, np, out_pos * nb_channels * sizeof(int16_t));
                        XDisposePtr((XPTR)pcm_out);
                        pcm_out = np;
                        out_samples = ns;
                    }

                    for (i = 0; i < keep; i++) {
                        for (ch = 0; ch < wma_ctx->nb_channels; ch++) {
                            float sample = frame_output[(skip + i) * nb_channels + ch] * 32767.0f;
                            if (sample > 32767.0f) sample = 32767.0f;
                            if (sample < -32768.0f) sample = -32768.0f;
                            pcm_out[(out_pos + (uint32_t)i) * nb_channels + ch] = (int16_t)sample;
                        }
                    }
                    out_pos += (uint32_t)keep;
                }
                sf_offset += sf_size;
            }

            /* Drain final overlap frame. */
            if (wma_ctx->skip_frames <= 0) {
                uint32_t i, ch;
                if (out_pos + frame_len > out_samples) {
                    uint32_t ns = out_pos + frame_len;
                    int16_t *np = (int16_t *)XNewPtr(ns * nb_channels * sizeof(int16_t));
                    if (np) {
                        XBlockMove(pcm_out, np, out_pos * nb_channels * sizeof(int16_t));
                        XDisposePtr((XPTR)pcm_out);
                        pcm_out = np;
                        out_samples = ns;
                    }
                }
                if (out_pos + frame_len <= out_samples) {
                    for (i = 0; i < frame_len; i++) {
                        for (ch = 0; ch < nb_channels; ch++) {
                            float sample = wma_ctx->frame_out[ch][i] * 32767.0f;
                            if (sample > 32767.0f) sample = 32767.0f;
                            if (sample < -32768.0f) sample = -32768.0f;
                            pcm_out[(out_pos + i) * nb_channels + ch] = (int16_t)sample;
                        }
                    }
                    out_pos += frame_len;
                }
            }

            /*
             * Drop the drained OLA flush frame from playable length. It is
             * near-silent and breaks seamless full-sample loops in BAESound.
             */
            if (out_pos > frame_len)
                out_pos -= frame_len;

            if (startFrame > 0 && startFrame < out_pos) {
                uint32_t keep = out_pos - startFrame;
                XBlockMove(pcm_out + startFrame * nb_channels,
                           pcm_out,
                           keep * nb_channels * sizeof(int16_t));
                out_pos = keep;
            } else if (startFrame >= out_pos) {
                out_pos = 0;
            }

            XDisposePtr((XPTR)frame_output);
            dst->theWaveform = (XPTR)pcm_out;
            dst->waveFrames = out_pos;
            dst->waveSize = out_pos * nb_channels * sizeof(int16_t);
            dst->compressionType = (uint32_t)C_NONE;
        }
    }

    wma_decode_close(wma_ctx);
    XDisposePtr((XPTR)wma_ctx);
    if (owned_wma_data) XDisposePtr((XPTR)owned_wma_data);

    return NO_ERR;
}

/* ---- PV_ReadIntoMemoryWMAFile: read a .wma/.asf file into a GM_Waveform ---- */
GM_Waveform *PV_ReadIntoMemoryWMAFile(XFILE file, bool decodeData,
                                       int32_t *pFormat, void **ppBlockPtr,
                                       uint32_t *pBlockSize, OPErr *pErr)
{
    GM_Waveform *wave;
    GM_Waveform src;
    XPTR wmaData;
    int32_t fileSize;
    OPErr err;

    if (!file || !pErr) {
        return NULL;
    }

    if (pFormat) *pFormat = X_NONE;
    if (ppBlockPtr) *ppBlockPtr = NULL;
    if (pBlockSize) *pBlockSize = 0;

    fileSize = XFileGetLength(file);
    if (fileSize <= 0) {
        *pErr = BAD_FILE;
        return NULL;
    }

    wmaData = XNewPtr(fileSize);
    if (!wmaData) {
        *pErr = MEMORY_ERR;
        return NULL;
    }

    XFileSetPosition(file, 0L);
    if (XFileRead(file, wmaData, fileSize) != 0) {
        XDisposePtr(wmaData);
        *pErr = BAD_FILE;
        return NULL;
    }

    wave = (GM_Waveform *)XNewPtr(sizeof(GM_Waveform));
    if (!wave) {
        XDisposePtr(wmaData);
        *pErr = MEMORY_ERR;
        return NULL;
    }

    XSetMemory(&src, sizeof(src), 0);
    src.theWaveform = (signed char *)wmaData;
    src.waveSize = (uint32_t)fileSize;
    src.baseMidiPitch = 60;
    src.compressionType = (uint32_t)C_WMA;

    err = XExpandWMA(&src, 0, wave);
    XDisposePtr(wmaData);
    if (err != NO_ERR) {
        XDisposePtr((XPTR)wave);
        *pErr = err;
        return NULL;
    }

    (void)decodeData;
    wave->currentFilePosition = 0;
    *pErr = NO_ERR;
    return wave;
}

#endif /* USE_WMA_SUPPORT */
