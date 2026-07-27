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
**  XVorbisFiles.h
**
**  Header for Ogg Vorbis audio file support in miniBAE
*/
/*****************************************************************************/

#ifndef __X_VORBIS_FILES__
#define __X_VORBIS_FILES__

#include "X_API.h"

#if USE_VORBIS_DECODER == TRUE || USE_VORBIS_ENCODER == TRUE

#ifdef __cplusplus
extern "C" {
#endif

#if USE_VORBIS_DECODER == TRUE

// Vorbis decoder functions
bool XIsVorbisFile(XFILE file);
void* XOpenVorbisFile(XFILE file);
OPErr XGetVorbisFileInfo(void *decoder_handle, uint32_t *samples, uint32_t *sample_rate, 
                        uint32_t *channels, uint32_t *bit_depth);
long XDecodeVorbisFile(void *decoder_handle, void *buffer, long buffer_size);
void XCloseVorbisFile(void *decoder_handle);

#endif // USE_VORBIS_DECODER

#if USE_VORBIS_ENCODER == TRUE

// Vorbis encoder functions
void* XInitVorbisEncoder(uint32_t sample_rate, uint32_t channels, float quality);
long XWriteVorbisHeader(void *encoder_handle, XFILE output_file);
long XEncodeVorbisData(void *encoder_handle, float **pcm_data, long samples, XFILE output_file);
void XCloseVorbisEncoder(void *encoder_handle);

#endif // USE_VORBIS_ENCODER

#ifdef __cplusplus
}
#endif

#endif // USE_VORBIS_DECODER || USE_VORBIS_ENCODER

#endif // __X_VORBIS_FILES__
