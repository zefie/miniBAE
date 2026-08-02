#pragma once

// Feature flags for compiling against NeoBAE headers when linking libneobae.dll.
// MUST match the flags used to build that DLL — BAEFileType enum values shift
// when codec/format toggles differ. Defaults below match a stock CMake build
// with BAE_PLATFORM=foobar2000 (no BAE_DISABLE_* overrides).

// Must match X_FOOBAR2000_PLUGIN in X_API.h (value 21).
#ifndef X_FOOBAR2000_PLUGIN
#define X_FOOBAR2000_PLUGIN 21
#endif
#ifndef X_PLATFORM
#define X_PLATFORM X_FOOBAR2000_PLUGIN
#endif

#ifndef USE_SF2_SUPPORT
#define USE_SF2_SUPPORT 1
#endif
#ifndef _USING_FLUIDLITE
#define _USING_FLUIDLITE 1
#endif
#ifndef FLUIDLITE_STATIC
#define FLUIDLITE_STATIC 1
#endif
#ifndef USE_NATIVE_DLS
#define USE_NATIVE_DLS 1
#endif
#ifndef USE_XMF_SUPPORT
#define USE_XMF_SUPPORT 1
#endif
#ifndef USE_RMI_SUPPORT
#define USE_RMI_SUPPORT 1
#endif
#ifndef USE_RETRO_RINGTONE_SUPPORT
#define USE_RETRO_RINGTONE_SUPPORT 1
#endif
#ifndef USE_ZMF_SUPPORT
#define USE_ZMF_SUPPORT 1
#endif
#ifndef _BUILT_IN_PATCHES
#define _BUILT_IN_PATCHES 1
#endif
#ifndef _LOAD_BUILTIN_PATCHES_FOR_SF2
#define _LOAD_BUILTIN_PATCHES_FOR_SF2 1
#endif
#ifndef _LOAD_BUILTIN_PATCHES_FOR_DLS
#define _LOAD_BUILTIN_PATCHES_FOR_DLS 1
#endif

// Codec toggles that affect BAEFileType layout — keep in sync with libneobae.
#ifndef USE_MPEG_DECODER
#define USE_MPEG_DECODER 1
#endif
#ifndef USE_MPEG_ENCODER
#define USE_MPEG_ENCODER 0
#endif
#ifndef USE_FLAC_DECODER
#define USE_FLAC_DECODER 1
#endif
#ifndef USE_FLAC_ENCODER
#define USE_FLAC_ENCODER 0
#endif
#ifndef USE_VORBIS_DECODER
#define USE_VORBIS_DECODER 1
#endif
#ifndef USE_VORBIS_ENCODER
#define USE_VORBIS_ENCODER 0
#endif
#ifndef USE_OPUS_DECODER
#define USE_OPUS_DECODER 1
#endif
#ifndef USE_OPUS_ENCODER
#define USE_OPUS_ENCODER 0
#endif
#ifndef USE_MTHC_SUPPORT
#define USE_MTHC_SUPPORT 1
#endif
#ifndef USE_ADP_SUPPORT
#define USE_ADP_SUPPORT 1
#endif
#ifndef USE_ADX_SUPPORT
#define USE_ADX_SUPPORT 1
#endif
#ifndef USE_QOA_SUPPORT
#define USE_QOA_SUPPORT 1
#endif
#ifndef USE_WMA_SUPPORT
#define USE_WMA_SUPPORT 1
#endif
#ifndef SF3_SUPPORT
#define SF3_SUPPORT 1
#endif

#ifndef SUPPORT_KARAOKE
#define SUPPORT_KARAOKE 0
#endif
#ifndef SUPPORT_BAESCRIPT
#define SUPPORT_BAESCRIPT 0
#endif
#ifndef SUPPORT_PLAYLIST
#define SUPPORT_PLAYLIST 0
#endif
#ifndef BAE_FIX_SPAN_DC
#define BAE_FIX_SPAN_DC 1
#endif
#ifndef BAE_CLASSIC_CHORUS
#define BAE_CLASSIC_CHORUS 1
#endif
#ifndef USE_J2ME_PATCH
#define USE_J2ME_PATCH 1
#endif
#ifndef BAE_ENABLE_ROLLED_MIDI_UNROLL
#define BAE_ENABLE_ROLLED_MIDI_UNROLL 1
#endif
