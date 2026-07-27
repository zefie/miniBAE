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

/*****************************************************************************/
/*
**  XVorbisFiles.c
**
**  Integration of libvorbis for Ogg Vorbis audio file support in NeoBAE
**
**  This file provides encoding and decoding support for Ogg Vorbis audio files
**  using the reference libvorbis implementation.
*/
/*****************************************************************************/

#include "X_API.h"
#include "X_Formats.h"
#include "GenSnd.h"
#include "GenPriv.h"
#include <limits.h>

#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE

#include <vorbis/codec.h>
#include <vorbis/vorbisfile.h>

#if USE_VORBIS_ENCODER == TRUE
#include <vorbis/vorbisenc.h>
#endif

// Structure to hold Vorbis decoder state
typedef struct {
    OggVorbis_File vf;
    vorbis_info *vi;
    int32_t current_section;
    bool is_open;
} XVorbisDecoder;

// Structure to hold Vorbis encoder state
#if USE_VORBIS_ENCODER == TRUE
typedef struct {
    ogg_stream_state os;
    ogg_page og;
    ogg_packet op;
    vorbis_info vi;
    vorbis_comment vc;
    vorbis_dsp_state vd;
    vorbis_block vb;
    bool is_initialized;
} XVorbisEncoder;
#endif

#if USE_VORBIS_DECODER == TRUE
// Callback functions for libvorbisfile to read from XFILE
static size_t vorbis_read_func(void *ptr, size_t size, size_t nmemb, void *datasource)
{
    XFILE file = (XFILE)datasource;
    size_t bytes_to_read = size * nmemb;
    int32_t err;
    if (bytes_to_read == 0) return 0;

    /* XFileRead returns 0 on success, -1 on failure. It will fail if
       asked to read more bytes than remain in the file, so cap the
       requested size to the remaining bytes to allow partial/EOF reads. */
    {
        int32_t file_len = XFileGetLength(file);
        int32_t file_pos = XFileGetPosition(file);
        if (file_len >= 0 && file_pos >= 0) {
            int32_t remaining = file_len - file_pos;
            if (remaining <= 0) return 0; /* EOF */
            if ((uint64_t)bytes_to_read > (uint64_t)remaining) {
                bytes_to_read = (size_t)remaining;
            }
        }
    }

    err = XFileRead(file, ptr, (int32_t)bytes_to_read);

    if (err != 0) {
        /* Error reading */
        return 0;
    }

    /* Return number of 'items' read as fread would (bytes / size). */
    return bytes_to_read / size;
}

static int vorbis_seek_func(void *datasource, ogg_int64_t offset, int whence)
{
    XFILE file = (XFILE)datasource;
    
    // Handle different seek modes
    switch (whence) {
        case 0: // SEEK_SET - absolute position from start
            {
                int result = XFileSetPosition(file, (long)offset) == NO_ERR ? 0 : -1;
                return result;
            }
        case 1: // SEEK_CUR - relative to current position  
            {
                int result = XFileSetPositionRelative(file, (long)offset) == NO_ERR ? 0 : -1;
                return result;
            }
        case 2: // SEEK_END - relative to end
            {
                int32_t file_length = XFileGetLength(file);
                if (file_length >= 0) {
                    int32_t new_pos = file_length + (int32_t)offset;
                    int result = XFileSetPosition(file, new_pos) == NO_ERR ? 0 : -1;
                    return result;
                } else {
                    return -1;
                }
            }
        default:
            return -1;
    }
}

static int vorbis_close_func(void *datasource)
{
    // We don't close the file here, let the caller handle it
    return 0;
}

static long vorbis_tell_func(void *datasource)
{
    XFILE file = (XFILE)datasource;
    return XFileGetPosition(file);
}

static ov_callbacks vorbis_callbacks = {
    vorbis_read_func,
    vorbis_seek_func,
    vorbis_close_func,
    vorbis_tell_func
};
#endif


#if USE_VORBIS_DECODER == TRUE

// Check if file is an Ogg Vorbis file
bool XIsVorbisFile(XFILE file)
{
    OggVorbis_File vf;
    int result;
    long pos;
    
    if (file == NULL) return FALSE;
    
    // Save current position
    pos = XFileGetPosition(file);
    
    // Try to open as Vorbis file
    result = ov_test_callbacks(file, &vf, NULL, 0, vorbis_callbacks);
    
    // Restore position
    XFileSetPosition(file, pos);
    
    if (result == 0) {
        ov_clear(&vf);
        return TRUE;
    }
    
    return FALSE;
}

// Open Vorbis file for decoding
void* XOpenVorbisFile(XFILE file)
{
    XVorbisDecoder *decoder;
    int result;
    
    if (file == NULL) return NULL;
    
    decoder = (XVorbisDecoder*)XNewPtr(sizeof(XVorbisDecoder));
    if (decoder == NULL) return NULL;
    
    decoder->is_open = FALSE;
    decoder->current_section = 0;
    
    result = ov_open_callbacks(file, &decoder->vf, NULL, 0, vorbis_callbacks);
    if (result != 0) {
        XDisposePtr(decoder);
        return NULL;
    }
    
    decoder->vi = ov_info(&decoder->vf, -1);
    if (decoder->vi == NULL) {
        ov_clear(&decoder->vf);
        XDisposePtr(decoder);
        return NULL;
    }
    
    decoder->is_open = TRUE;
    return decoder;
}

// Get Vorbis file information
OPErr XGetVorbisFileInfo(void *decoder_handle, uint32_t *samples, uint32_t *sample_rate, 
                        uint32_t *channels, uint32_t *bit_depth)
{
    XVorbisDecoder *decoder = (XVorbisDecoder*)decoder_handle;
    
    if (decoder == NULL || !decoder->is_open || decoder->vi == NULL) {
        return PARAM_ERR;
    }
    
    if (samples) *samples = (uint32_t)ov_pcm_total(&decoder->vf, -1);
    if (sample_rate) *sample_rate = decoder->vi->rate;
    if (channels) *channels = decoder->vi->channels;
    if (bit_depth) *bit_depth = 16; // Vorbis outputs 16-bit PCM
    
    return NO_ERR;
}

// Decode Vorbis data to PCM
long XDecodeVorbisFile(void *decoder_handle, void *buffer, long buffer_size)
{
    XVorbisDecoder *decoder = (XVorbisDecoder*)decoder_handle;
    long bytes_read = 0;
    long total_read = 0;
    int bitstream;
    
    if (decoder == NULL || !decoder->is_open || buffer == NULL) {
        return -1;
    }
    
    while (total_read < buffer_size) {
        bytes_read = ov_read(&decoder->vf, 
                           (char*)buffer + total_read, 
                           buffer_size - total_read,
                           0, // little endian
                           2, // 16-bit samples
                           1, // signed
                           &bitstream);
        
        if (bytes_read <= 0) {
            break; // EOF or error
        }
        
        total_read += bytes_read;
    }
    
    return total_read;
}

// Close Vorbis decoder
void XCloseVorbisFile(void *decoder_handle)
{
    XVorbisDecoder *decoder = (XVorbisDecoder*)decoder_handle;
    
    if (decoder != NULL) {
        if (decoder->is_open) {
            ov_clear(&decoder->vf);
        }
        XDisposePtr(decoder);
    }
}

#endif // USE_VORBIS_DECODER

#if USE_VORBIS_ENCODER == TRUE

// Initialize Vorbis encoder
void* XInitVorbisEncoder(uint32_t sample_rate, uint32_t channels, float quality)
{
    XVorbisEncoder *encoder;
    int result;
    
    encoder = (XVorbisEncoder*)XNewPtr(sizeof(XVorbisEncoder));
    if (encoder == NULL) return NULL;
    
    encoder->is_initialized = FALSE;
    
    // Initialize vorbis info
    vorbis_info_init(&encoder->vi);
    
    // Set encoding parameters (VBR mode with quality setting)
    result = vorbis_encode_init_vbr(&encoder->vi, channels, sample_rate, quality);
    if (result != 0) {
        vorbis_info_clear(&encoder->vi);
        XDisposePtr(encoder);
        return NULL;
    }
    
    // Initialize comment
    vorbis_comment_init(&encoder->vc);
    vorbis_comment_add_tag(&encoder->vc, "ENCODER", "NeoBAE");
    
    // Initialize analysis state and auxiliary encoding storage
    vorbis_analysis_init(&encoder->vd, &encoder->vi);
    vorbis_block_init(&encoder->vd, &encoder->vb);
    
    // Initialize stream
    ogg_stream_init(&encoder->os, rand());
    
    encoder->is_initialized = TRUE;
    return encoder;
}

// Write Vorbis header to output
long XWriteVorbisHeader(void *encoder_handle, XFILE output_file)
{
    XVorbisEncoder *encoder = (XVorbisEncoder*)encoder_handle;
    ogg_packet header, header_comm, header_code;
    long bytes_written = 0;
    
    if (encoder == NULL || !encoder->is_initialized || output_file == NULL) {
        return -1;
    }
    
    // Build headers
    vorbis_analysis_headerout(&encoder->vd, &encoder->vc, 
                             &header, &header_comm, &header_code);
    
    // Stream headers
    ogg_stream_packetin(&encoder->os, &header);
    ogg_stream_packetin(&encoder->os, &header_comm);  
    ogg_stream_packetin(&encoder->os, &header_code);
    
    // Write header pages
    while (ogg_stream_flush(&encoder->os, &encoder->og) != 0) {
        bytes_written += XFileWrite(output_file, encoder->og.header, encoder->og.header_len);
        bytes_written += XFileWrite(output_file, encoder->og.body, encoder->og.body_len);
    }
    
    return bytes_written;
}

// Encode PCM data to Vorbis
long XEncodeVorbisData(void *encoder_handle, float **pcm_data, long samples, XFILE output_file)
{
    XVorbisEncoder *encoder = (XVorbisEncoder*)encoder_handle;
    float **buffer;
    long bytes_written = 0;
    int eos = 0;
    
    if (encoder == NULL || !encoder->is_initialized) {
        return -1;
    }
    
    // Get analysis buffer
    buffer = vorbis_analysis_buffer(&encoder->vd, samples);
    
    // Copy PCM data to analysis buffer
    if (pcm_data != NULL && samples > 0) {
        int channels = encoder->vi.channels;
        for (int ch = 0; ch < channels; ch++) {
            for (long i = 0; i < samples; i++) {
                buffer[ch][i] = pcm_data[ch][i];
            }
        }
        
        vorbis_analysis_wrote(&encoder->vd, samples);
    } else {
        // Signal end of stream
        vorbis_analysis_wrote(&encoder->vd, 0);
        eos = 1;
    }
    
    // Process blocks
    while (vorbis_analysis_blockout(&encoder->vd, &encoder->vb) == 1) {
        vorbis_analysis(&encoder->vb, NULL);
        vorbis_bitrate_addblock(&encoder->vb);
        
        while (vorbis_bitrate_flushpacket(&encoder->vd, &encoder->op)) {
            ogg_stream_packetin(&encoder->os, &encoder->op);
            
            while (!eos) {
                int result = ogg_stream_pageout(&encoder->os, &encoder->og);
                if (result == 0) break;
                
                if (output_file) {
                    bytes_written += XFileWrite(output_file, encoder->og.header, encoder->og.header_len);
                    bytes_written += XFileWrite(output_file, encoder->og.body, encoder->og.body_len);
                }
                
                if (ogg_page_eos(&encoder->og)) eos = 1;
            }
        }
    }
    
    return bytes_written;
}

// Close Vorbis encoder
void XCloseVorbisEncoder(void *encoder_handle)
{
    XVorbisEncoder *encoder = (XVorbisEncoder*)encoder_handle;

    if (encoder != NULL) {
        if (encoder->is_initialized) {
            ogg_stream_clear(&encoder->os);
            vorbis_block_clear(&encoder->vb);
            vorbis_dsp_clear(&encoder->vd);
            vorbis_comment_clear(&encoder->vc);
            vorbis_info_clear(&encoder->vi);
        }
        XDisposePtr(encoder);
    }
}

/* Growing byte buffer used by XEncodeVorbisToMemory */
typedef struct {
    unsigned char *data;
    uint32_t       size;
    uint32_t       capacity;
} VorbisMembuf;

static int PV_VorbisMembufAppend(VorbisMembuf *buf, const unsigned char *bytes, long len)
{
    uint32_t addLen;
    uint32_t newSize;
    uint32_t newCap;

    if (len <= 0) return 0;
    if (!buf || !bytes) return -1;
    if ((unsigned long)len > (unsigned long)UINT32_MAX) return -1;

    addLen = (uint32_t)len;
    if (addLen > (uint32_t)INT32_MAX) return -1;
    if (buf->size > (UINT32_MAX - addLen)) return -1;

    newSize = buf->size + addLen;
    if (newSize > buf->capacity) {
        newCap = buf->capacity ? buf->capacity * 2 : 65536;
        unsigned char *grown;
        while (newCap < newSize) {
            if (newCap > (UINT32_MAX / 2)) {
                newCap = UINT32_MAX;
                break;
            }
            newCap *= 2;
        }
        if (newCap < newSize || newCap > (uint32_t)INT32_MAX) return -1;
        grown = (unsigned char *)XNewPtr((int32_t)newCap);
        if (!grown) return -1;
        if (buf->data && buf->size) XBlockMove(buf->data, grown, (int32_t)buf->size);
        if (buf->data) XDisposePtr((XPTR)buf->data);
        buf->data = grown;
        buf->capacity = newCap;
    }
    XBlockMove((void *)bytes, buf->data + buf->size, (int32_t)addLen);
    buf->size = newSize;
    return 0;
}

static void PV_FlushOggPages(XVorbisEncoder *enc, VorbisMembuf *buf, int eos)
{
    ogg_page og;
    while (1) {
        int result = eos ? ogg_stream_flush(&enc->os, &og)
                         : ogg_stream_pageout(&enc->os, &og);
        if (result == 0) break;
        PV_VorbisMembufAppend(buf, (const unsigned char *)og.header, og.header_len);
        PV_VorbisMembufAppend(buf, (const unsigned char *)og.body,   og.body_len);
    }
}

/* Encode a PCM GM_Waveform to Ogg Vorbis in memory.
 * src->compressionType must be C_NONE (raw PCM, 8- or 16-bit).
 * On success allocates *outData and sets *outSize; caller must XDisposePtr it. */
OPErr XEncodeVorbisToMemory(GM_Waveform const *src, float quality,
                            XPTR *outData, uint32_t *outSize)
{
    XVorbisEncoder *enc;
    VorbisMembuf    buf;
    ogg_packet      header, header_comm, header_code;
    uint32_t        ch, frames, f;
    float         **pcmBuf;
    int16_t        *src16;
    unsigned char  *src8;
    uint32_t        chunkFrames;

    if (!src || !src->theWaveform || !outData || !outSize)
        return PARAM_ERR;
    if (src->compressionType != (uint32_t)C_NONE)
        return PARAM_ERR;
    if (src->channels < 1 || src->channels > 2)
        return PARAM_ERR;

    *outData = NULL;
    *outSize = 0;
    XSetMemory(&buf, sizeof(buf), 0);

    enc = (XVorbisEncoder *)XInitVorbisEncoder(
            (uint32_t)(src->sampledRate >> 16),
            (uint32_t)src->channels,
            quality);
    if (!enc) return MEMORY_ERR;

    /* Write headers */
    vorbis_analysis_headerout(&enc->vd, &enc->vc, &header, &header_comm, &header_code);
    ogg_stream_packetin(&enc->os, &header);
    ogg_stream_packetin(&enc->os, &header_comm);
    ogg_stream_packetin(&enc->os, &header_code);
    PV_FlushOggPages(enc, &buf, 1 /* flush to force header pages */);

    frames = src->waveFrames;
    ch     = src->channels;
    src16  = (int16_t *)src->theWaveform;
    src8   = (unsigned char *)src->theWaveform;

    /* Feed PCM in 4096-frame chunks */
    chunkFrames = 4096;
    f = 0;
    while (f < frames) {
        uint32_t count = frames - f;
        if (count > chunkFrames) count = chunkFrames;
        pcmBuf = vorbis_analysis_buffer(&enc->vd, (int)count);
        if (src->bitSize == 16) {
            uint32_t i, c;
            for (c = 0; c < ch; c++)
                for (i = 0; i < count; i++)
                    pcmBuf[c][i] = (float)src16[(f + i) * ch + c] / 32768.0f;
        } else {
            uint32_t i, c;
            for (c = 0; c < ch; c++)
                for (i = 0; i < count; i++)
                    pcmBuf[c][i] = ((float)(src8[(f + i) * ch + c]) - 128.0f) / 128.0f;
        }
        vorbis_analysis_wrote(&enc->vd, (int)count);
        /* Drain blocks */
        while (vorbis_analysis_blockout(&enc->vd, &enc->vb) == 1) {
            vorbis_analysis(&enc->vb, NULL);
            vorbis_bitrate_addblock(&enc->vb);
            while (vorbis_bitrate_flushpacket(&enc->vd, &enc->op)) {
                ogg_stream_packetin(&enc->os, &enc->op);
                PV_FlushOggPages(enc, &buf, 0);
            }
        }
        f += count;
    }

    /* Signal end of stream */
    vorbis_analysis_wrote(&enc->vd, 0);
    while (vorbis_analysis_blockout(&enc->vd, &enc->vb) == 1) {
        vorbis_analysis(&enc->vb, NULL);
        vorbis_bitrate_addblock(&enc->vb);
        while (vorbis_bitrate_flushpacket(&enc->vd, &enc->op)) {
            ogg_stream_packetin(&enc->os, &enc->op);
            PV_FlushOggPages(enc, &buf, 0);
        }
    }
    /* Flush remaining pages */
    PV_FlushOggPages(enc, &buf, 1);

    XCloseVorbisEncoder(enc);

    if (!buf.data || buf.size == 0) {
        if (buf.data) XDisposePtr((XPTR)buf.data);
        return BAD_FILE;
    }
    *outData = (XPTR)buf.data;
    *outSize = buf.size;
    return NO_ERR;
}

#endif // USE_VORBIS_ENCODER

#endif // USE_VORBIS_DECODER || USE_VORBIS_ENCODER
