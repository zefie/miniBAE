cmake_minimum_required(VERSION 3.10)
project(NeoBAE LANGUAGES C CXX)

if(POLICY CMP0003)
  cmake_policy(SET CMP0003 NEW)
endif()

# Version string generation equivalent to neobae/inc/Makefile.versioning.
set(BAE_VERSION "")
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE BAE_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  execute_process(
    COMMAND ${GIT_EXECUTABLE} status --porcelain
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE BAE_GIT_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-list --abbrev-commit --tags --max-count=1
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE BAE_GIT_TAG_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  set(BAE_GIT_TAG "")
  if(NOT BAE_GIT_TAG_COMMIT STREQUAL "")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} describe --abbrev=0 --tags ${BAE_GIT_TAG_COMMIT}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE BAE_GIT_TAG
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
  endif()

  execute_process(
    COMMAND ${GIT_EXECUTABLE} log -1 --format=%cd --date=format:%Y%m%d
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE BAE_GIT_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()

if(BAE_GIT_COMMIT STREQUAL "")
  if(BAE_GIT_DATE STREQUAL "")
    string(TIMESTAMP BAE_GIT_DATE "%Y%m%d")
  endif()
  set(BAE_VERSION "${BAE_GIT_DATE}")
else()
  if(NOT BAE_GIT_TAG STREQUAL "")
    if(BAE_GIT_COMMIT STREQUAL BAE_GIT_TAG_COMMIT)
      string(REGEX REPLACE "^v" "" BAE_VERSION "${BAE_GIT_TAG}")
    else()
      set(BAE_VERSION "git-${BAE_GIT_COMMIT}")
    endif()
  else()
    set(BAE_VERSION "git-${BAE_GIT_COMMIT}")
  endif()
  if(NOT BAE_GIT_STATUS STREQUAL "")
    set(BAE_VERSION "${BAE_VERSION}-dirty")
  endif()
endif()

add_compile_definitions(_VERSION="${BAE_VERSION}")
message(STATUS "NeoBAE build version: ${BAE_VERSION}")

option(BAE_DISABLE_RMF_EDITOR "Disable BAERmfEditor (Creation API) support" OFF)
option(BAE_DISABLE_SF2_CONVERTER "Disable SF2 converter support" OFF)
option(BAE_DISABLE_ZMF "Disable ZMF support" OFF)
option(BAE_DISABLE_LZMA "Disable LZMA compression support" OFF)
option(BAE_DISABLE_MP3_ENCODER "Disable MP3 encoder support" OFF)
option(BAE_DISABLE_MP3_DECODER "Disable MP3 decoder support" OFF)
option(BAE_DISABLE_FLAC_ENCODER "Disable FLAC encoder support" OFF)
option(BAE_DISABLE_FLAC_DECODER "Disable FLAC decoder support" OFF)
option(BAE_DISABLE_VORBIS_ENCODER "Disable Vorbis encoder support" OFF)
option(BAE_DISABLE_VORBIS_DECODER "Disable Vorbis decoder support" OFF)
option(BAE_DISABLE_OPUS_ENCODER "Disable Opus encoder support" OFF)
option(BAE_DISABLE_OPUS_DECODER "Disable Opus decoder support" OFF)
option(BAE_DISABLE_KARAOKE "Disable karaoke support" OFF)
option(BAE_DISABLE_BAESCRIPT "Disable BAEScript support" OFF)
option(BAE_DISABLE_PLAYLIST "Disable playlist support" OFF)
option(BAE_DISABLE_SF2_SUPPORT "Disable SF2 support" OFF)
option(BAE_DISABLE_FLUIDSYNTH "Disable FluidSynth integration" OFF)
option(BAE_DISABLE_XMF_SUPPORT "Disable XMF support" OFF)
option(BAE_DISABLE_NOKIA_PATCH "Disable Nokia patch handling" OFF)
option(BAE_DISABLE_BEATNIK_SF2_NRPN "Disable Beatnik SF2 NRPN behavior" OFF)
option(BAE_DISABLE_BUILTIN_PATCHES_WITH_SF2 "Disable loading built-in patches for SF2" OFF)
option(BAE_DISABLE_FIX_SPAN_DC "Disable span DC fix" OFF)
option(BAE_DISABLE_CLASSIC_CHORUS "Disable classic chorus path" OFF)
option(BAE_DISABLE_MTHC_SUPPORT "Disable MTHC support" OFF)
option(BAE_DISABLE_ADP_SUPPORT "Disable ADP support" OFF)
option(BAE_DISABLE_CREATION_API "Disable Creation API support (alias for RMF editor APIs)" OFF)
option(BAE_DISABLE_EMBED_PATCHES "Disable embedded built-in patches" OFF)
option(BAE_DISABLE_EMBED_FONT "Disable embedded GUI font" OFF)
option(BAE_DISABLE_RETRO_RINGTONE_SUPPORT "Disable retro mobile ringtone format support (IMY/EMY/RNG/RTX/RTTTL)" OFF)
option(BAE_DISABLE_QOA_SUPPORT "Disable QOA codec support" OFF)
option(BUILD_PLAYBAE "Build playbae when a non-Dummy/non-Ansi backend is selected" ON)
option(BUILD_ZEFIDI "Build zefidi when SDL2/SDL3 backend and matching SDL_ttf are available" ON)
option(BUILD_CLITOOLS "Build CLI tools (songtool, rmf2mid, mid2rmf, mid2rmi, sf2-hsb, mod2rmf, instdump, bankrecomp)" ON)

# Internal compatibility booleans: keep existing logic in terms of ENABLE vars.
set(BAE_ENABLE_RMF_EDITOR ON)
set(BAE_ENABLE_SF2_CONVERTER ON)
set(BAE_ENABLE_ZMF ON)
set(BAE_ENABLE_LZMA ON)
set(BAE_ENABLE_MP3_ENCODER ON)
set(BAE_ENABLE_MP3_DECODER ON)
set(BAE_ENABLE_FLAC_ENCODER ON)
set(BAE_ENABLE_FLAC_DECODER ON)
set(BAE_ENABLE_VORBIS_ENCODER ON)
set(BAE_ENABLE_VORBIS_DECODER ON)
set(BAE_ENABLE_OPUS_ENCODER ON)
set(BAE_ENABLE_OPUS_DECODER ON)
set(BAE_ENABLE_KARAOKE ON)
set(BAE_ENABLE_BAESCRIPT ON)
set(BAE_ENABLE_PLAYLIST ON)
set(BAE_ENABLE_SF2_SUPPORT ON)
set(BAE_ENABLE_FLUIDSYNTH ON)
set(BAE_ENABLE_XMF_SUPPORT ON)
set(BAE_ENABLE_NOKIA_PATCH ON)
set(BAE_ENABLE_BEATNIK_SF2_NRPN ON)
set(BAE_ENABLE_BUILTIN_PATCHES_WITH_SF2 ON)
set(BAE_ENABLE_FIX_SPAN_DC ON)
set(BAE_ENABLE_CLASSIC_CHORUS ON)
set(BAE_ENABLE_MTHC_SUPPORT ON)
set(BAE_ENABLE_ADP_SUPPORT ON)
set(BAE_ENABLE_EMBED_PATCHES ON)
set(BAE_ENABLE_EMBED_FONT ON)
set(BAE_ENABLE_RETRO_RINGTONE_SUPPORT ON)
set(BAE_ENABLE_QOA_SUPPORT ON)

if(BAE_DISABLE_RMF_EDITOR)
  set(BAE_ENABLE_RMF_EDITOR OFF)
  message(STATUS "Disabling RMF/ZMF Creation API Support")
else()
  message(STATUS "Enabling RMF/ZMF Creation API Support")
endif()
if(BAE_DISABLE_SF2_CONVERTER)
  set(BAE_ENABLE_SF2_CONVERTER OFF)
  message(STATUS "Disabling SF2 Converter Support")
else()
  message(STATUS "Enabling SF2 Converter Support")
endif()
if(BAE_DISABLE_ZMF)
  set(BAE_ENABLE_ZMF OFF)
  message(STATUS "Disabling ZMF Support")
else()
  message(STATUS "Enabling ZMF Support")    
endif()
if(BAE_DISABLE_LZMA)
  set(BAE_ENABLE_LZMA OFF)
  message(STATUS "Disabling LZMA Support")
else()
  message(STATUS "Enabling LZMA Support")
endif()
if(BAE_DISABLE_MP3_ENCODER)
  set(BAE_ENABLE_MP3_ENCODER OFF)
  message(STATUS "Disabling MP3 encoder support")
endif()
if(BAE_DISABLE_MP3_DECODER)
  set(BAE_ENABLE_MP3_DECODER OFF)
  message(STATUS "Disabling MP3 decoder support")
endif()
if(BAE_DISABLE_FLAC_ENCODER)
  set(BAE_ENABLE_FLAC_ENCODER OFF)
  message(STATUS "Disabling FLAC encoder support")
endif()
if(BAE_DISABLE_FLAC_DECODER)
  set(BAE_ENABLE_FLAC_DECODER OFF)
  message(STATUS "Disabling FLAC decoder support")
endif()
if(BAE_DISABLE_VORBIS_ENCODER)
  set(BAE_ENABLE_VORBIS_ENCODER OFF)
  message(STATUS "Disabling Vorbis encoder support")
endif()
if(BAE_DISABLE_VORBIS_DECODER)
  set(BAE_ENABLE_VORBIS_DECODER OFF)
  message(STATUS "Disabling Vorbis decoder support")
endif()
if(BAE_DISABLE_OPUS_ENCODER)
  set(BAE_ENABLE_OPUS_ENCODER OFF)
  message(STATUS "Disabling Opus encoder support")
endif()
if(BAE_DISABLE_OPUS_DECODER)
  set(BAE_ENABLE_OPUS_DECODER OFF)
  message(STATUS "Disabling Opus decoder support")
endif()
if(BAE_DISABLE_RETRO_RINGTONE_SUPPORT)
  set(BAE_ENABLE_RETRO_RINGTONE_SUPPORT OFF)
  message(STATUS "Disabling retro mobile ringtone format support (IMY/EMY/RNG/RTX/RTTTL)")
else()
  message(STATUS "Enabling retro mobile ringtone format support (IMY/EMY/RNG/RTX/RTTTL)")
endif()
if (BAE_DISABLE_QOA_SUPPORT)
  set(BAE_ENABLE_QOA_SUPPORT OFF)
  message(STATUS "Disabling QOA codec support")
else()
  message(STATUS "Enabling QOA codec support")
endif()
if(BAE_DISABLE_KARAOKE)
  set(BAE_ENABLE_KARAOKE OFF)
endif()
if(BAE_DISABLE_BAESCRIPT)
  set(BAE_ENABLE_BAESCRIPT OFF)
endif()
if(BAE_DISABLE_PLAYLIST)
  set(BAE_ENABLE_PLAYLIST OFF)
endif()
if(BAE_DISABLE_SF2_SUPPORT)
  set(BAE_ENABLE_SF2_SUPPORT OFF)
endif()
if(BAE_DISABLE_FLUIDSYNTH)
  set(BAE_ENABLE_FLUIDSYNTH OFF)
endif()
if(BAE_DISABLE_XMF_SUPPORT)
  set(BAE_ENABLE_XMF_SUPPORT OFF)
endif()
if(BAE_DISABLE_NOKIA_PATCH)
  set(BAE_ENABLE_NOKIA_PATCH OFF)
endif()
if(BAE_DISABLE_BEATNIK_SF2_NRPN)
  set(BAE_ENABLE_BEATNIK_SF2_NRPN OFF)
endif()
if(BAE_DISABLE_BUILTIN_PATCHES_WITH_SF2)
  set(BAE_ENABLE_BUILTIN_PATCHES_WITH_SF2 OFF)
endif()
if(BAE_DISABLE_FIX_SPAN_DC)
  set(BAE_ENABLE_FIX_SPAN_DC OFF)
endif()
if(BAE_DISABLE_CLASSIC_CHORUS)
  set(BAE_ENABLE_CLASSIC_CHORUS OFF)
endif()
if(BAE_DISABLE_MTHC_SUPPORT)
  set(BAE_ENABLE_MTHC_SUPPORT OFF)
endif()
if(BAE_DISABLE_ADP_SUPPORT)
  set(BAE_ENABLE_ADP_SUPPORT OFF)
endif()
if(BAE_DISABLE_CREATION_API)
  set(BAE_ENABLE_RMF_EDITOR OFF)
endif()
if(BAE_DISABLE_EMBED_PATCHES)
  set(BAE_ENABLE_EMBED_PATCHES OFF)
endif()
if(BAE_DISABLE_EMBED_FONT)
  set(BAE_ENABLE_EMBED_FONT OFF)
endif()

# FluidSynth probe and gating for SF2/XMF behavior from Makefile.common.
find_path(BAE_FLUIDSYNTH_INCLUDE_DIR fluidsynth.h PATH_SUFFIXES fluidsynth)
find_library(BAE_FLUIDSYNTH_LIBRARY NAMES fluidsynth)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_FLUIDSYNTH QUIET fluidsynth)
  if(NOT BAE_FLUIDSYNTH_INCLUDE_DIR AND PC_FLUIDSYNTH_FOUND)
    set(BAE_FLUIDSYNTH_INCLUDE_DIR ${PC_FLUIDSYNTH_INCLUDE_DIRS})
  endif()
  if(NOT BAE_FLUIDSYNTH_LIBRARY AND PC_FLUIDSYNTH_FOUND)
    find_library(BAE_FLUIDSYNTH_LIBRARY NAMES fluidsynth HINTS ${PC_FLUIDSYNTH_LIBRARY_DIRS})
  endif()
endif()
set(BAE_FLUIDSYNTH_AVAILABLE OFF)
if(BAE_FLUIDSYNTH_INCLUDE_DIR AND BAE_FLUIDSYNTH_LIBRARY)
  set(BAE_FLUIDSYNTH_AVAILABLE ON)
endif()

if(BAE_ENABLE_FLUIDSYNTH AND NOT BAE_FLUIDSYNTH_AVAILABLE)
  set(BAE_ENABLE_FLUIDSYNTH OFF)
  message(STATUS "Disabling FluidSynth support: library/headers not found")
endif()
if(BAE_ENABLE_SF2_SUPPORT AND NOT BAE_ENABLE_FLUIDSYNTH)
  set(BAE_ENABLE_SF2_SUPPORT OFF)
  message(STATUS "Disabling SF2 support because FluidSynth is unavailable/disabled")
endif()
if(BAE_ENABLE_XMF_SUPPORT AND NOT BAE_ENABLE_FLUIDSYNTH)
  set(BAE_ENABLE_XMF_SUPPORT OFF)
  message(STATUS "Disabling XMF support because FluidSynth is unavailable/disabled")
endif()

# External codec dependency probes (system libs).
set(BAE_LAME_WRAPPER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/BAE_MPEG_Source_II/XMPEG_lame_encoder.cpp")
set(BAE_LAME_WRAPPER_AVAILABLE OFF)
if(EXISTS "${BAE_LAME_WRAPPER_SOURCE}")
  set(BAE_LAME_WRAPPER_AVAILABLE ON)
endif()

find_path(BAE_LAME_INCLUDE_DIR lame/lame.h)
find_library(BAE_LAME_LIBRARY NAMES mp3lame lame)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LAME QUIET libmp3lame)
  if(NOT BAE_LAME_INCLUDE_DIR AND PC_LAME_FOUND)
    set(BAE_LAME_INCLUDE_DIR ${PC_LAME_INCLUDE_DIRS})
  endif()
  if(NOT BAE_LAME_LIBRARY AND PC_LAME_FOUND)
    find_library(BAE_LAME_LIBRARY NAMES mp3lame lame HINTS ${PC_LAME_LIBRARY_DIRS})
  endif()
endif()

set(BAE_MP3_ENCODER_AVAILABLE OFF)
if(BAE_LAME_WRAPPER_AVAILABLE AND BAE_LAME_INCLUDE_DIR AND BAE_LAME_LIBRARY)
  set(BAE_MP3_ENCODER_AVAILABLE ON)
endif()

find_path(BAE_FLAC_INCLUDE_DIR FLAC/all.h)
find_library(BAE_FLAC_LIBRARY NAMES FLAC)
set(BAE_FLAC_AVAILABLE OFF)
if(BAE_FLAC_INCLUDE_DIR AND BAE_FLAC_LIBRARY)
  set(BAE_FLAC_AVAILABLE ON)
endif()

find_path(BAE_VORBIS_INCLUDE_DIR vorbis/codec.h)
find_library(BAE_VORBIS_LIBRARY NAMES vorbis)
find_library(BAE_VORBISFILE_LIBRARY NAMES vorbisfile)
find_library(BAE_VORBISENC_LIBRARY NAMES vorbisenc)
set(BAE_VORBIS_DECODER_AVAILABLE OFF)
set(BAE_VORBIS_ENCODER_AVAILABLE OFF)
if(BAE_VORBIS_INCLUDE_DIR AND BAE_VORBIS_LIBRARY AND BAE_VORBISFILE_LIBRARY)
  set(BAE_VORBIS_DECODER_AVAILABLE ON)
  if(BAE_VORBISENC_LIBRARY)
    set(BAE_VORBIS_ENCODER_AVAILABLE ON)
  endif()
endif()

find_path(BAE_OPUS_INCLUDE_DIR opus/opus.h)
find_library(BAE_OPUS_LIBRARY NAMES opus)
find_path(BAE_OPUSFILE_INCLUDE_DIR NAMES opusfile.h opus/opusfile.h)
find_library(BAE_OPUSFILE_LIBRARY NAMES opusfile)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_OPUS QUIET opus)
  pkg_check_modules(PC_OPUSFILE QUIET opusfile)
  pkg_check_modules(PC_OGG QUIET ogg)

  if(NOT BAE_OPUS_INCLUDE_DIR AND PC_OPUS_FOUND)
    set(BAE_OPUS_INCLUDE_DIR ${PC_OPUS_INCLUDE_DIRS})
  endif()
  if(NOT BAE_OPUS_LIBRARY AND PC_OPUS_FOUND)
    find_library(BAE_OPUS_LIBRARY NAMES opus HINTS ${PC_OPUS_LIBRARY_DIRS})
  endif()

  if(NOT BAE_OPUSFILE_INCLUDE_DIR AND PC_OPUSFILE_FOUND)
    set(BAE_OPUSFILE_INCLUDE_DIR ${PC_OPUSFILE_INCLUDE_DIRS})
  endif()
  if(NOT BAE_OPUSFILE_LIBRARY AND PC_OPUSFILE_FOUND)
    find_library(BAE_OPUSFILE_LIBRARY NAMES opusfile HINTS ${PC_OPUSFILE_LIBRARY_DIRS})
  endif()

  if(NOT BAE_OGG_INCLUDE_DIR AND PC_OGG_FOUND)
    set(BAE_OGG_INCLUDE_DIR ${PC_OGG_INCLUDE_DIRS})
  endif()
  if(NOT BAE_OGG_LIBRARY AND PC_OGG_FOUND)
    find_library(BAE_OGG_LIBRARY NAMES ogg HINTS ${PC_OGG_LIBRARY_DIRS})
  endif()
endif()
set(BAE_OPUS_ENCODER_AVAILABLE OFF)
set(BAE_OPUS_DECODER_AVAILABLE OFF)
if(BAE_OPUS_INCLUDE_DIR AND BAE_OPUS_LIBRARY)
  set(BAE_OPUS_ENCODER_AVAILABLE ON)
endif()
if(BAE_OPUS_INCLUDE_DIR AND BAE_OPUSFILE_INCLUDE_DIR AND BAE_OPUS_LIBRARY AND BAE_OPUSFILE_LIBRARY)
  set(BAE_OPUS_DECODER_AVAILABLE ON)
endif()

find_path(BAE_OGG_INCLUDE_DIR ogg/ogg.h)
find_library(BAE_OGG_LIBRARY NAMES ogg)
set(BAE_OGG_AVAILABLE OFF)
if(BAE_OGG_INCLUDE_DIR AND BAE_OGG_LIBRARY)
  set(BAE_OGG_AVAILABLE ON)
endif()

# Auto-disable codec toggles when required external dependencies are missing.
if(BAE_ENABLE_FLAC_DECODER AND NOT BAE_FLAC_AVAILABLE)
  set(BAE_ENABLE_FLAC_DECODER OFF)
  message(STATUS "Disabling FLAC decoder: external libFLAC not found")
else()
  if(BAE_ENABLE_FLAC_DECODER)
    message(STATUS "Enabling FLAC decoder: external libFLAC found")
  endif()
endif()
if(BAE_ENABLE_MP3_ENCODER AND NOT BAE_MP3_ENCODER_AVAILABLE)
  set(BAE_ENABLE_MP3_ENCODER OFF)
  message(STATUS "Disabling MP3 encoder: LAME wrapper source or external libmp3lame not found")
else()
  if(BAE_ENABLE_MP3_ENCODER)
    message(STATUS "Enabling MP3 encoder: LAME wrapper source and external libmp3lame found")
  endif()
endif()
if(BAE_ENABLE_FLAC_ENCODER AND NOT BAE_FLAC_AVAILABLE)
  set(BAE_ENABLE_FLAC_ENCODER OFF)
  message(STATUS "Disabling FLAC encoder: external libFLAC not found")
else()
  if(BAE_ENABLE_FLAC_ENCODER)
    message(STATUS "Enabling FLAC encoder: external libFLAC found")
  endif()
endif()

if(BAE_ENABLE_VORBIS_DECODER AND (NOT BAE_VORBIS_DECODER_AVAILABLE OR NOT BAE_OGG_AVAILABLE))
  set(BAE_ENABLE_VORBIS_DECODER OFF)
  message(STATUS "Disabling Vorbis decoder: external libvorbis/libvorbisfile/libogg not found")
else()
  if(BAE_ENABLE_VORBIS_DECODER)
    message(STATUS "Enabling Vorbis decoder: external libvorbis/libvorbisfile/libogg found")
  endif()
endif()
if(BAE_ENABLE_VORBIS_ENCODER AND (NOT BAE_VORBIS_ENCODER_AVAILABLE OR NOT BAE_OGG_AVAILABLE))
  set(BAE_ENABLE_VORBIS_ENCODER OFF)
  message(STATUS "Disabling Vorbis encoder: external libvorbisenc/libvorbis/libvorbisfile/libogg not found")
else()
  if(BAE_ENABLE_VORBIS_ENCODER)
    message(STATUS "Enabling Vorbis encoder: external libvorbisenc/libvorbis/libvorbisfile/libogg found")
  endif()
endif()

if(BAE_ENABLE_OPUS_DECODER AND (NOT BAE_OPUS_DECODER_AVAILABLE OR NOT BAE_OGG_AVAILABLE))
  set(BAE_ENABLE_OPUS_DECODER OFF)
  message(STATUS "Disabling Opus decoder: external libopus/libopusfile/libogg not found")
else()
  if(BAE_ENABLE_OPUS_DECODER)
    message(STATUS "Enabling Opus decoder: external libopus/libopusfile/libogg found")
  endif()
endif()
if(BAE_ENABLE_OPUS_ENCODER AND (NOT BAE_OPUS_ENCODER_AVAILABLE OR NOT BAE_OGG_AVAILABLE))
  set(BAE_ENABLE_OPUS_ENCODER OFF)
  message(STATUS "Disabling Opus encoder: external libopus/libogg not found")
else()
  if(BAE_ENABLE_OPUS_ENCODER)
    message(STATUS "Enabling Opus encoder: external libopus/libogg found")
  endif()
endif()

if(NOT DEFINED BAE_PLATFORM OR BAE_PLATFORM STREQUAL "")
  set(_BAE_PLATFORM_DEFAULT "Dummy")

  find_path(_BAE_DEFAULT_SDL3_INCLUDE_DIR SDL3/SDL.h)
  find_library(_BAE_DEFAULT_SDL3_LIBRARY NAMES SDL3)
  if(_BAE_DEFAULT_SDL3_INCLUDE_DIR AND _BAE_DEFAULT_SDL3_LIBRARY)
    set(_BAE_PLATFORM_DEFAULT "SDL3")
    message(STATUS "Defaulting to SDL3 platform backend (found SDL3 headers and library).")
  else()
    find_path(_BAE_DEFAULT_SDL2_INCLUDE_DIR SDL2/SDL.h)
    find_library(_BAE_DEFAULT_SDL2_LIBRARY NAMES SDL2)
    if(_BAE_DEFAULT_SDL2_INCLUDE_DIR AND _BAE_DEFAULT_SDL2_LIBRARY)
      set(_BAE_PLATFORM_DEFAULT "SDL2")
      message(STATUS "Defaulting to SDL2 platform backend (found SDL2 headers and library).")
    endif()
  endif()

  set(BAE_PLATFORM "${_BAE_PLATFORM_DEFAULT}" CACHE STRING "Backend platform (Dummy, SDL2, SDL3, Raylib, WinOS, Ansi, WASM, Android, IOS, MacOSX, foobar2000)")
else()
  set(BAE_PLATFORM "${BAE_PLATFORM}" CACHE STRING "Backend platform (Dummy, SDL2, SDL3, Raylib, WinOS, Ansi, WASM, Android, IOS, MacOSX, foobar2000)")
endif()
message(STATUS "Selected BAE_PLATFORM='${BAE_PLATFORM}'")
set_property(CACHE BAE_PLATFORM PROPERTY STRINGS Dummy SDL2 SDL3 Raylib WinOS Ansi WASM Android IOS MacOSX foobar2000)

set(BAE_PLATFORM_SOURCE "")
set(BAE_X_PLATFORM_DEFINE "")
if(BAE_PLATFORM STREQUAL "Dummy")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_Dummy.c)
  set(BAE_X_PLATFORM_DEFINE X_DUMMY)
elseif(BAE_PLATFORM STREQUAL "SDL2")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_SDL2.c)
  set(BAE_X_PLATFORM_DEFINE X_SDL2)
elseif(BAE_PLATFORM STREQUAL "SDL3")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_SDL3.c)
  set(BAE_X_PLATFORM_DEFINE X_SDL3)
elseif(BAE_PLATFORM STREQUAL "Raylib")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_Raylib.c)
  set(BAE_X_PLATFORM_DEFINE X_RAYLIB)
elseif(BAE_PLATFORM STREQUAL "WinOS")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_WinOS.c)
  set(BAE_X_PLATFORM_DEFINE X_WIN95)
elseif(BAE_PLATFORM STREQUAL "Ansi")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_Ansi.c)
  set(BAE_X_PLATFORM_DEFINE X_ANSI)
elseif(BAE_PLATFORM STREQUAL "WASM")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_WASM.c)
  set(BAE_X_PLATFORM_DEFINE X_WASM)
elseif(BAE_PLATFORM STREQUAL "Android")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_Android.c)
  set(BAE_X_PLATFORM_DEFINE X_ANDROID)
elseif(BAE_PLATFORM STREQUAL "IOS")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_IOS.c)
  set(BAE_X_PLATFORM_DEFINE X_IOS)
elseif(BAE_PLATFORM STREQUAL "MacOSX")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_MacOSX.c)
  set(BAE_X_PLATFORM_DEFINE X_MACINTOSH)
elseif(BAE_PLATFORM STREQUAL "foobar2000")
  set(BAE_PLATFORM_SOURCE neobae/src/BAE_Source/Platform/BAE_API_foobar2000.c)
  set(BAE_X_PLATFORM_DEFINE X_FOOBAR2000_PLUGIN)
else()
  message(FATAL_ERROR "Unsupported BAE_PLATFORM='${BAE_PLATFORM}'.")
endif()

set(BAE_SDL_BACKEND_AVAILABLE OFF)
if(BAE_PLATFORM STREQUAL "SDL2")
  find_path(BAE_SDL2_INCLUDE_DIR SDL2/SDL.h)
  find_library(BAE_SDL2_LIBRARY NAMES SDL2)
  if(BAE_SDL2_INCLUDE_DIR AND BAE_SDL2_LIBRARY)
    set(BAE_SDL_BACKEND_AVAILABLE ON)
  else()
    message(STATUS "BAE_PLATFORM=SDL2 selected, but SDL2 headers/library were not found.")
  endif()
elseif(BAE_PLATFORM STREQUAL "SDL3")
  find_path(BAE_SDL3_INCLUDE_DIR SDL3/SDL.h)
  find_library(BAE_SDL3_LIBRARY NAMES SDL3)
  if(BAE_SDL3_INCLUDE_DIR AND BAE_SDL3_LIBRARY)
    set(BAE_SDL_BACKEND_AVAILABLE ON)
  else()
    message(STATUS "BAE_PLATFORM=SDL3 selected, but SDL3 headers/library were not found.")
  endif()
endif()

set(BAE_SDL_TTF_AVAILABLE OFF)
if(BUILD_ZEFIDI)
   message(STATUS "Checking for SDL_ttf support for zefidi...")
  if(BAE_PLATFORM STREQUAL "SDL2")
    find_path(BAE_SDL2_TTF_INCLUDE_DIR SDL2/SDL_ttf.h)
    find_library(BAE_SDL2_TTF_LIBRARY NAMES SDL2_ttf)
    if(BAE_SDL2_TTF_INCLUDE_DIR AND BAE_SDL2_TTF_LIBRARY)
      set(BAE_SDL_TTF_AVAILABLE ON)
      message(STATUS "SDL_ttf support found for SDL2 backend.")
    else()
      message(STATUS "BAE_PLATFORM=SDL2 selected, but SDL2_ttf headers/library were not found.")
    endif()
  elseif(BAE_PLATFORM STREQUAL "SDL3")
    find_path(BAE_SDL3_TTF_INCLUDE_DIR SDL3_ttf/SDL_ttf.h)
    find_library(BAE_SDL3_TTF_LIBRARY NAMES SDL3_ttf)
    if(BAE_SDL3_TTF_INCLUDE_DIR AND BAE_SDL3_TTF_LIBRARY)
      set(BAE_SDL_TTF_AVAILABLE ON)
      message(STATUS "SDL_ttf support found for SDL3 backend.")
    else()
      message(STATUS "BAE_PLATFORM=SDL3 selected, but SDL3_ttf headers/library were not found.")
    endif()
  endif()
endif()

set(BAE_ZEFIDI_AVAILABLE OFF)
if(BAE_SDL_BACKEND_AVAILABLE AND BAE_SDL_TTF_AVAILABLE)
  set(BAE_ZEFIDI_AVAILABLE ON)
endif()

set(BAE_PLAYBAE_PLATFORM_AVAILABLE ON)
if(BAE_PLATFORM STREQUAL "Dummy" OR BAE_PLATFORM STREQUAL "Ansi")
  set(BAE_PLAYBAE_PLATFORM_AVAILABLE OFF)
endif()

set(NEOBAE_SOURCES
  ${BAE_PLATFORM_SOURCE}
  neobae/src/BAE_Source/Common/DriverTools.c
  neobae/src/BAE_Source/Common/GenAudioStreams.c
  neobae/src/BAE_Source/Common/GenCache.c
  neobae/src/BAE_Source/Common/GenChorus.c
  neobae/src/BAE_Source/Common/GenFiltersReverbU3232.c
  neobae/src/BAE_Source/Common/GenInterp2ReverbU3232.c
  neobae/src/BAE_Source/Common/GenOutput.c
  neobae/src/BAE_Source/Common/GenPatch.c
  neobae/src/BAE_Source/Common/GenReverb.c
  neobae/src/BAE_Source/Common/GenReverbNew.c
  neobae/src/BAE_Source/Common/GenReverbNeo.c
  neobae/src/BAE_Source/Common/GenRingtone.c
  neobae/src/BAE_Source/Common/GenRMI.c
  neobae/src/BAE_Source/Common/GenSample.c
  neobae/src/BAE_Source/Common/GenSeq.c
  neobae/src/BAE_Source/Common/GenSeqTools.c
  neobae/src/BAE_Source/Common/GenSetup.c
  neobae/src/BAE_Source/Common/GenSong.c
  neobae/src/BAE_Source/Common/GenSoundFiles.c
  neobae/src/BAE_Source/Common/GenSynth.c
  neobae/src/BAE_Source/Common/GenSynthFiltersSimple.c
  neobae/src/BAE_Source/Common/GenSynthFiltersU3232.c
  neobae/src/BAE_Source/Common/GenSynthInterp2Simple.c
  neobae/src/BAE_Source/Common/GenSynthInterp2U3232.c
  neobae/src/BAE_Source/Common/NeoBAE.c
  neobae/src/BAE_Source/Common/NewNewLZSS.c
  neobae/src/BAE_Source/Common/SampleTools.c
  neobae/src/BAE_Source/Common/X_API.c
  neobae/src/BAE_Source/Common/X_Decompress.c
  neobae/src/BAE_Source/Common/X_IMA.c
  neobae/src/BAE_Source/Common/g711.c
  neobae/src/BAE_Source/Common/g721.c
  neobae/src/BAE_Source/Common/g723_24.c
  neobae/src/BAE_Source/Common/g723_40.c
  neobae/src/BAE_Source/Common/g72x.c
  neobae/src/BAE_Source/Common/sha1mini.c
  neobae/src/BAE_Source/Common/XFileTypes.c
)

if(BAE_ENABLE_LZMA)
  list(APPEND NEOBAE_SOURCES neobae/src/BAE_Source/Common/X_LZMA.c)
endif()

if(BAE_ENABLE_RMF_EDITOR)
  list(APPEND NEOBAE_SOURCES neobae/src/BAE_Source/Common/BAERmfEditor.c)
endif()

if(BAE_ENABLE_SF2_CONVERTER)
  list(APPEND NEOBAE_SOURCES
    neobae/src/sf2-hsb/hsb_writer.c
    neobae/src/sf2-hsb/sf2_hsb_converter.c
    neobae/src/sf2-hsb/sf2_parser.c
  )
endif()

if(BAE_ENABLE_SF2_SUPPORT AND BAE_ENABLE_FLUIDSYNTH)
  list(APPEND NEOBAE_SOURCES neobae/src/BAE_Source/Common/GenSF2_FluidSynth.c)
endif()

if(BAE_ENABLE_XMF_SUPPORT)
  list(APPEND NEOBAE_SOURCES neobae/src/BAE_Source/Common/GenXMF.c)
endif()

if(BAE_ENABLE_MTHC_SUPPORT)
  list(APPEND NEOBAE_SOURCES neobae/src/mthc/mthc_decomp.c)
endif()

if(BAE_ENABLE_ADP_SUPPORT)
  list(APPEND NEOBAE_SOURCES
    neobae/src/adp2wav/adp2wav_decode.c
    neobae/src/thirdparty/libg722/g722_decode.c
  )
endif()

if(BAE_ENABLE_RETRO_RINGTONE_SUPPORT)
  list(APPEND NEOBAE_SOURCES
    neobae/src/BAE_Source/Common/GenRingtone.c
  )
endif()

if(BAE_ENABLE_QOA_SUPPORT)
  list(APPEND NEOBAE_SOURCES
    neobae/src/BAE_Source/Common/XQOAFiles.c
  )
endif()

if(BAE_ENABLE_BAESCRIPT)
  list(APPEND NEOBAE_SOURCES
    neobae/src/script/baescript.c
    neobae/src/script/baescript_lexer.c
    neobae/src/script/baescript_parser.c
    neobae/src/script/baescript_vm.c
  )
endif()

if(BAE_ENABLE_MP3_DECODER)
  list(APPEND NEOBAE_SOURCES neobae/src/BAE_MPEG_Source_II/XMPEG_minimp3_wrapper.c)
endif()

if(BAE_ENABLE_MP3_ENCODER OR BAE_ENABLE_MP3_DECODER)
  list(APPEND NEOBAE_SOURCES neobae/src/BAE_MPEG_Source_II/XMPEGFilesSun.c)
endif()

if(BAE_ENABLE_MP3_ENCODER)
  list(APPEND NEOBAE_SOURCES neobae/src/BAE_MPEG_Source_II/XMPEG_lame_encoder.cpp)
endif()

if(BAE_ENABLE_VORBIS_ENCODER OR BAE_ENABLE_VORBIS_DECODER)
  list(APPEND NEOBAE_SOURCES neobae/src/BAE_Source/Common/XVorbisFiles.c)
endif()

if(BAE_ENABLE_OPUS_ENCODER OR BAE_ENABLE_OPUS_DECODER)
  list(APPEND NEOBAE_SOURCES neobae/src/BAE_Source/Common/XOpusFiles.c)
endif()

add_library(neobae STATIC ${NEOBAE_SOURCES})

target_include_directories(neobae
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/BAE_Source/Common
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/BAE_Source/Platform
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/config
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/qoa
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/mod2rmf
)

if(BAE_ENABLE_MP3_ENCODER OR BAE_ENABLE_MP3_DECODER)
  target_include_directories(neobae PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/BAE_MPEG_Source_II)
endif()

if(BAE_ENABLE_MP3_ENCODER AND BAE_LAME_INCLUDE_DIR)
  target_include_directories(neobae PUBLIC ${BAE_LAME_INCLUDE_DIR})
endif()

if(BAE_ENABLE_MP3_DECODER)
  target_include_directories(neobae PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/minimp3)
endif()

if(BAE_ENABLE_FLAC_ENCODER OR BAE_ENABLE_FLAC_DECODER)
  target_include_directories(neobae PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/flac/include
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/flac/src/libFLAC/include
  )
  if(BAE_FLAC_INCLUDE_DIR)
    target_include_directories(neobae PUBLIC ${BAE_FLAC_INCLUDE_DIR})
  endif()
endif()

if(BAE_ENABLE_VORBIS_ENCODER OR BAE_ENABLE_VORBIS_DECODER)
  target_include_directories(neobae PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/libvorbis/include
    ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/libvorbis/lib
  )
  if(BAE_VORBIS_INCLUDE_DIR)
    target_include_directories(neobae PUBLIC ${BAE_VORBIS_INCLUDE_DIR})
  endif()
endif()

if(BAE_ENABLE_OPUS_ENCODER OR BAE_ENABLE_OPUS_DECODER)
  target_include_directories(neobae PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/opus/include)
  if(BAE_OPUS_INCLUDE_DIR)
    target_include_directories(neobae PUBLIC ${BAE_OPUS_INCLUDE_DIR})
  endif()
endif()

if(BAE_ENABLE_OPUS_DECODER)
  target_include_directories(neobae PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/opusfile/include)
  if(BAE_OPUSFILE_INCLUDE_DIR)
    target_include_directories(neobae PUBLIC ${BAE_OPUSFILE_INCLUDE_DIR})
  endif()
endif()

if(BAE_ENABLE_FLAC_ENCODER OR BAE_ENABLE_FLAC_DECODER OR BAE_ENABLE_VORBIS_ENCODER OR BAE_ENABLE_VORBIS_DECODER OR BAE_ENABLE_OPUS_ENCODER OR BAE_ENABLE_OPUS_DECODER)
  target_include_directories(neobae PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/thirdparty/libogg/include)
  if(BAE_OGG_INCLUDE_DIR)
    target_include_directories(neobae PUBLIC ${BAE_OGG_INCLUDE_DIR})
  endif()
endif()

if(BAE_ENABLE_SF2_CONVERTER)
  target_include_directories(neobae PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/sf2-hsb)
endif()

if(BAE_ENABLE_BAESCRIPT)
  target_include_directories(neobae PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/script)
endif()

if(BAE_ENABLE_SF2_SUPPORT AND BAE_ENABLE_FLUIDSYNTH AND BAE_FLUIDSYNTH_INCLUDE_DIR)
  target_include_directories(neobae PUBLIC ${BAE_FLUIDSYNTH_INCLUDE_DIR})
endif()

if(BAE_PLATFORM STREQUAL "SDL2" AND BAE_SDL_BACKEND_AVAILABLE)
  target_include_directories(neobae PUBLIC ${BAE_SDL2_INCLUDE_DIR})
  target_link_libraries(neobae PUBLIC ${BAE_SDL2_LIBRARY})
endif()

if(BAE_PLATFORM STREQUAL "SDL3" AND BAE_SDL_BACKEND_AVAILABLE)
  target_include_directories(neobae PUBLIC ${BAE_SDL3_INCLUDE_DIR})
  target_link_libraries(neobae PUBLIC ${BAE_SDL3_LIBRARY})
endif()

target_compile_definitions(neobae
  PUBLIC
    X_PLATFORM=${BAE_X_PLATFORM_DEFINE}
    USE_ZMF_SUPPORT=$<IF:$<BOOL:${BAE_ENABLE_ZMF}>,1,0>
    USE_ZMF_FORMAT=$<IF:$<BOOL:${BAE_ENABLE_ZMF}>,1,0>
    USE_LZMA_COMPRESSION=$<IF:$<BOOL:${BAE_ENABLE_LZMA}>,1,0>
    USE_MPEG_ENCODER=$<IF:$<BOOL:${BAE_ENABLE_MP3_ENCODER}>,1,0>
    USE_MPEG_DECODER=$<IF:$<BOOL:${BAE_ENABLE_MP3_DECODER}>,1,0>
    USE_FLAC_ENCODER=$<IF:$<BOOL:${BAE_ENABLE_FLAC_ENCODER}>,1,0>
    USE_FLAC_DECODER=$<IF:$<BOOL:${BAE_ENABLE_FLAC_DECODER}>,1,0>
    USE_VORBIS_ENCODER=$<IF:$<BOOL:${BAE_ENABLE_VORBIS_ENCODER}>,1,0>
    USE_VORBIS_DECODER=$<IF:$<BOOL:${BAE_ENABLE_VORBIS_DECODER}>,1,0>
    USE_OPUS_ENCODER=$<IF:$<BOOL:${BAE_ENABLE_OPUS_ENCODER}>,1,0>
    USE_OPUS_DECODER=$<IF:$<BOOL:${BAE_ENABLE_OPUS_DECODER}>,1,0>
    USE_MINIMP3_WRAPPER=$<IF:$<BOOL:${BAE_ENABLE_MP3_DECODER}>,1,0>
    USE_LAME_ENCODER=$<IF:$<BOOL:${BAE_ENABLE_MP3_ENCODER}>,1,0>
    USE_OGG_FORMAT=$<IF:$<BOOL:$<OR:$<BOOL:${BAE_ENABLE_FLAC_ENCODER}>,$<BOOL:${BAE_ENABLE_FLAC_DECODER}>,$<BOOL:${BAE_ENABLE_VORBIS_ENCODER}>,$<BOOL:${BAE_ENABLE_VORBIS_DECODER}>,$<BOOL:${BAE_ENABLE_OPUS_ENCODER}>,$<BOOL:${BAE_ENABLE_OPUS_DECODER}>>>,1,0>
    SUPPORT_OGG_FORMAT=$<IF:$<BOOL:$<OR:$<BOOL:${BAE_ENABLE_FLAC_ENCODER}>,$<BOOL:${BAE_ENABLE_FLAC_DECODER}>,$<BOOL:${BAE_ENABLE_VORBIS_ENCODER}>,$<BOOL:${BAE_ENABLE_VORBIS_DECODER}>,$<BOOL:${BAE_ENABLE_OPUS_ENCODER}>,$<BOOL:${BAE_ENABLE_OPUS_DECODER}>>>,1,0>
    USE_SF2_SUPPORT=$<IF:$<BOOL:${BAE_ENABLE_SF2_SUPPORT}>,1,0>
    _USING_FLUIDSYNTH=$<IF:$<BOOL:${BAE_ENABLE_FLUIDSYNTH}>,1,0>
    USE_XMF_SUPPORT=$<IF:$<BOOL:${BAE_ENABLE_XMF_SUPPORT}>,1,0>
    DISABLE_NOKIA_PATCH=$<IF:$<BOOL:$<NOT:$<BOOL:${BAE_ENABLE_NOKIA_PATCH}>>>,1,0>
    DISABLE_BEATNIK_SF2_NRPN=$<IF:$<BOOL:$<NOT:$<BOOL:${BAE_ENABLE_BEATNIK_SF2_NRPN}>>>,1,0>
    _LOAD_BUILTIN_PATCHES_FOR_SF2=$<IF:$<BOOL:${BAE_ENABLE_BUILTIN_PATCHES_WITH_SF2}>,1,0>
    BAE_FIX_SPAN_DC=$<IF:$<BOOL:${BAE_ENABLE_FIX_SPAN_DC}>,1,0>
    BAE_CLASSIC_CHORUS=$<IF:$<BOOL:${BAE_ENABLE_CLASSIC_CHORUS}>,1,0>
    USE_MTHC_SUPPORT=$<IF:$<BOOL:${BAE_ENABLE_MTHC_SUPPORT}>,1,0>
    USE_ADP_SUPPORT=$<IF:$<BOOL:${BAE_ENABLE_ADP_SUPPORT}>,1,0>
    SUPPORT_KARAOKE=$<IF:$<BOOL:${BAE_ENABLE_KARAOKE}>,1,0>
    SUPPORT_BAESCRIPT=$<IF:$<BOOL:${BAE_ENABLE_BAESCRIPT}>,1,0>
    SUPPORT_PLAYLIST=$<IF:$<BOOL:${BAE_ENABLE_PLAYLIST}>,1,0>
)

target_compile_options(neobae PRIVATE -w)
target_compile_features(neobae PUBLIC c_std_99)

find_package(Threads REQUIRED)
target_link_libraries(neobae PUBLIC Threads::Threads)

if(UNIX AND NOT APPLE)
  target_link_libraries(neobae PUBLIC m)
  if(CMAKE_DL_LIBS)
    target_link_libraries(neobae PUBLIC ${CMAKE_DL_LIBS})
  endif()
endif()

if(BAE_ENABLE_MP3_ENCODER)
  target_link_libraries(neobae PUBLIC ${BAE_LAME_LIBRARY})
endif()

if(BAE_ENABLE_SF2_SUPPORT AND BAE_ENABLE_FLUIDSYNTH)
  target_link_libraries(neobae PUBLIC ${BAE_FLUIDSYNTH_LIBRARY})
endif()

if(BAE_ENABLE_XMF_SUPPORT)
  target_link_libraries(neobae PUBLIC z)
endif()

if(BAE_ENABLE_FLAC_ENCODER OR BAE_ENABLE_FLAC_DECODER)
  target_link_libraries(neobae PUBLIC ${BAE_FLAC_LIBRARY})
endif()

if(BAE_ENABLE_VORBIS_ENCODER OR BAE_ENABLE_VORBIS_DECODER)
  target_link_libraries(neobae PUBLIC ${BAE_VORBIS_LIBRARY} ${BAE_VORBISFILE_LIBRARY})
  if(BAE_ENABLE_VORBIS_ENCODER)
    target_link_libraries(neobae PUBLIC ${BAE_VORBISENC_LIBRARY})
  endif()
endif()

if(BAE_ENABLE_OPUS_ENCODER OR BAE_ENABLE_OPUS_DECODER)
  target_link_libraries(neobae PUBLIC ${BAE_OPUS_LIBRARY})
  if(BAE_ENABLE_OPUS_DECODER)
    target_link_libraries(neobae PUBLIC ${BAE_OPUSFILE_LIBRARY})
  endif()
endif()

if(BAE_ENABLE_FLAC_ENCODER OR BAE_ENABLE_FLAC_DECODER OR BAE_ENABLE_VORBIS_ENCODER OR BAE_ENABLE_VORBIS_DECODER OR BAE_ENABLE_OPUS_ENCODER OR BAE_ENABLE_OPUS_DECODER)
  target_link_libraries(neobae PUBLIC ${BAE_OGG_LIBRARY})
endif()

if(BAE_ENABLE_LZMA)
  find_library(LIBLZMA_LIBRARY NAMES liblzma lzma)
  find_path(LIBLZMA_INCLUDE_DIR lzma.h)
  if(LIBLZMA_LIBRARY)
    if(LIBLZMA_INCLUDE_DIR)
      target_include_directories(neobae PRIVATE ${LIBLZMA_INCLUDE_DIR})
    endif()
    if(WIN32)
      target_compile_definitions(neobae PRIVATE LZMA_API_STATIC)
    endif()
    target_link_libraries(neobae PUBLIC ${LIBLZMA_LIBRARY})
  endif()
endif()

if(BUILD_PLAYBAE)
  if(BAE_PLAYBAE_PLATFORM_AVAILABLE)
    add_executable(playbae neobae/src/playbae/playbae.c)
    target_link_libraries(playbae PRIVATE neobae)
  else()
    message(STATUS "Skipping BUILD_PLAYBAE because BAE_PLATFORM is Dummy or Ansi.")
  endif()
endif()

if(BUILD_ZEFIDI)
  if(BAE_ZEFIDI_AVAILABLE)
    set(ZEFIDI_SOURCES
      neobae/src/gui/gui_theme.c
      neobae/src/gui/gui_widgets.c
      neobae/src/gui/gui_text.c
      neobae/src/gui/gui_midi_vkbd.c
      neobae/src/gui/gui_export.c
      neobae/src/gui/gui_dialogs.c
      neobae/src/gui/gui_bae.c
      neobae/src/gui/gui_settings.c
      neobae/src/gui/gui_panels.c
      neobae/src/gui/gui_main.c
    )
    if(BAE_ENABLE_KARAOKE)
      list(APPEND ZEFIDI_SOURCES neobae/src/gui/gui_karaoke.c)
    endif()
    if(BAE_ENABLE_PLAYLIST)
      list(APPEND ZEFIDI_SOURCES neobae/src/gui/gui_playlist.c)
    endif()
    if(BAE_ENABLE_BAESCRIPT)
      list(APPEND ZEFIDI_SOURCES neobae/src/gui/gui_script_editor.c)
    endif()
    add_executable(zefidi ${ZEFIDI_SOURCES})
    target_compile_definitions(zefidi PRIVATE _ZEFI_GUI=1)
    target_link_libraries(zefidi PRIVATE neobae)
    if(BAE_PLATFORM STREQUAL "SDL2")
      target_include_directories(zefidi PRIVATE ${BAE_SDL2_TTF_INCLUDE_DIR})
      target_link_libraries(zefidi PRIVATE ${BAE_SDL2_TTF_LIBRARY})
    elseif(BAE_PLATFORM STREQUAL "SDL3")
      target_include_directories(zefidi PRIVATE ${BAE_SDL3_TTF_INCLUDE_DIR})
      target_link_libraries(zefidi PRIVATE ${BAE_SDL3_TTF_LIBRARY})
    endif()
  else()
    message(STATUS "Skipping BUILD_ZEFIDI because matching SDL and SDL_ttf were not found for selected BAE_PLATFORM.")
  endif()
endif()

if(BUILD_CLITOOLS)
  add_executable(songtool 
    neobae/src/songtool/songtool.c
    neobae/src/mod2rmf/mod2rmf_encoder.c
  )
  target_link_libraries(songtool PRIVATE neobae)

  add_executable(rmf2mid neobae/src/rmf2mid/rmf2mid.c)
  target_link_libraries(rmf2mid PRIVATE neobae)

  add_executable(mid2rmf neobae/src/mid2rmf/mid2rmf.c)
  target_link_libraries(mid2rmf PRIVATE neobae)

  add_executable(mid2rmi neobae/src/mid2rmi/mid2rmi.c)
  target_link_libraries(mid2rmi PRIVATE neobae)

  add_executable(instdump neobae/src/rmfinfo/instdump.c)
  target_link_libraries(instdump PRIVATE neobae)

  add_executable(bankrecomp neobae/src/bankrecomp/bankrecomp.c)
  target_link_libraries(bankrecomp PRIVATE neobae)

  add_executable(sf2_hsb
    neobae/src/sf2-hsb/sf2_hsb_main.c
    neobae/src/sf2-hsb/sf2_hsb_converter.c
    neobae/src/sf2-hsb/sf2_parser.c
    neobae/src/sf2-hsb/hsb_writer.c
  )
  target_include_directories(sf2_hsb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/sf2-hsb)
  target_link_libraries(sf2_hsb PRIVATE neobae)
  set_target_properties(sf2_hsb PROPERTIES OUTPUT_NAME sf2-hsb)

  find_path(LIBXMP_INCLUDE_DIR xmp.h)
  find_library(LIBXMP_LIBRARY NAMES xmp)
  if(LIBXMP_INCLUDE_DIR AND LIBXMP_LIBRARY)
    add_executable(mod2rmf
      neobae/src/mod2rmf/mod2rmf_main.c
      neobae/src/mod2rmf/mod2rmf_common.c
      neobae/src/mod2rmf/mod2rmf_encoder.c
      neobae/src/mod2rmf/mod2rmf_resampler.c
      neobae/src/mod2rmf/mod2rmf_rmfcreat.c
      neobae/src/mod2rmf/mod2rmf_song.c
    )
    target_include_directories(mod2rmf PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/neobae/src/mod2rmf
      ${LIBXMP_INCLUDE_DIR}
    )
    target_link_libraries(mod2rmf PRIVATE neobae ${LIBXMP_LIBRARY})
  else()
    message(STATUS "Skipping mod2rmf (libxmp not found).")
  endif()
endif()
