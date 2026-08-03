/*
 * Mixer-free song duration (+ RMF/ZMF metadata) probe for the foobar2000
 * libneobae build. Only compiled when BAE_PLATFORM=foobar2000.
 */
#pragma once

#include "NeoBAE.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BAE_RMF_META_FIELD_MAX 512

/* Beatnik RMF/ZMF song-info text fields (MacRoman→WinANSI via probe). */
typedef struct BAE_RmfSongMetadata
{
	char title[BAE_RMF_META_FIELD_MAX];
	char performed[BAE_RMF_META_FIELD_MAX];
	char composer[BAE_RMF_META_FIELD_MAX];
	char copyright[BAE_RMF_META_FIELD_MAX];
	char publisher[BAE_RMF_META_FIELD_MAX];
	char use_license[BAE_RMF_META_FIELD_MAX];
	char licensed_url[BAE_RMF_META_FIELD_MAX];
	char license_term[BAE_RMF_META_FIELD_MAX];
	char expiration[BAE_RMF_META_FIELD_MAX];
	char composer_notes[BAE_RMF_META_FIELD_MAX];
	char index_number[BAE_RMF_META_FIELD_MAX];
	char genre[BAE_RMF_META_FIELD_MAX];
	char sub_genre[BAE_RMF_META_FIELD_MAX];
	char tempo[BAE_RMF_META_FIELD_MAX];
	char original_source[BAE_RMF_META_FIELD_MAX];
	int present; /* non-zero when container was RMF/ZMF and fields were filled */
} BAE_RmfSongMetadata;

// Resolve song duration without opening a BAEMixer / MusicGlobals session.
// Supports MIDI, RMI, MThc, RMF/ZMF, XMF/MXMF, and ringtone formats that
// convert to SMF. Returns BAE_NO_ERROR with *outLengthMicros > 0 on success.
//
// If outMetadata is non-NULL and the file is RMF/ZMF, song-info text is copied
// out while the SongResource is already open (no second parse). For non-RMF
// types, outMetadata->present is set to 0.
BAEResult BAE_ProbeSongLengthFromMemory(void const *data,
                                        uint32_t dataSize,
                                        uint32_t *outLengthMicros,
                                        BAEFileType *outFileType,
                                        BAE_RmfSongMetadata *outMetadata);

#ifdef __cplusplus
}
#endif
