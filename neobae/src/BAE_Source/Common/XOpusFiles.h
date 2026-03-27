/*****************************************************************************/
/*
**  XOpusFiles.h
**
**  Header for Ogg Opus audio file support in miniBAE
*/
/*****************************************************************************/

#ifndef __X_OPUS_FILES__
#define __X_OPUS_FILES__

#include "X_API.h"

#if USE_OPUS_DECODER == TRUE || USE_OPUS_ENCODER == TRUE

#ifdef __cplusplus
extern "C" {
#endif

#if USE_OPUS_DECODER == TRUE

// Opus decoder functions
bool XIsOpusFile(XFILE file);
void* XOpenOpusFile(XFILE file);
OPErr XGetOpusFileInfo(void *decoder_handle, uint32_t *samples, uint32_t *sample_rate, 
                      uint32_t *channels, uint32_t *bit_depth);
long XDecodeOpusFile(void *decoder_handle, void *buffer, long buffer_size);
void XCloseOpusFile(void *decoder_handle);

#endif // USE_OPUS_DECODER

#if USE_OPUS_ENCODER == TRUE

// Opus encoder functions
void* XInitOpusEncoder(uint32_t sample_rate, uint32_t channels, uint32_t bitrate, uint32_t mode);
long XWriteOpusHeader(void *encoder_handle, XFILE output_file);
long XEncodeOpusData(void *encoder_handle, const int16_t *pcm_interleaved, long frames, XFILE output_file);
long XFlushOpusEncoder(void *encoder_handle, XFILE output_file);
void XCloseOpusEncoder(void *encoder_handle);

#endif // USE_OPUS_ENCODER

#ifdef __cplusplus
}
#endif

#endif // USE_OPUS_DECODER || USE_OPUS_ENCODER

#endif // __X_OPUS_FILES__