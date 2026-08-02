/*
 * Mixer-free song duration probe for the foobar2000 libneobae build.
 * Only compiled when BAE_PLATFORM=foobar2000.
 */
#pragma once

#include "NeoBAE.h"

#ifdef __cplusplus
extern "C" {
#endif

// Resolve song duration without opening a BAEMixer / MusicGlobals session.
// Supports MIDI, RMI, MThc, RMF/ZMF, XMF/MXMF, and ringtone formats that
// convert to SMF. Returns BAE_NO_ERROR with *outLengthMicros > 0 on success.
BAEResult BAE_ProbeSongLengthFromMemory(void const *data,
                                        uint32_t dataSize,
                                        uint32_t *outLengthMicros,
                                        BAEFileType *outFileType);

#ifdef __cplusplus
}
#endif
