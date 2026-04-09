# Copyright (C) 2010 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# build miniBAE

# Collect Git metadata
ifeq ($(OS),Windows_NT)
  COMMIT      := $(shell git rev-parse --short HEAD 2>NUL)
  DIRTY       := $(shell powershell -Command "git status --porcelain | Select-Object -First 1")
  TAG_COMMIT  := $(shell git rev-list --abbrev-commit --tags --max-count=1 2>NUL)
  TAG         := $(shell git describe --abbrev=0 --tags $(TAG_COMMIT) 2>NUL)
  DATE        := $(shell git log -1 --format=%cd --date=format:"%Y%m%d" 2>NUL)
else
  COMMIT      := $(shell git rev-parse --short HEAD 2>/dev/null)
  DIRTY       := $(shell git status --porcelain 2>/dev/null | head -n 1)
  TAG_COMMIT  := $(shell git rev-list --abbrev-commit --tags --max-count=1 2>/dev/null)
  TAG         := $(shell git describe --abbrev=0 --tags $(TAG_COMMIT) 2>/dev/null || true)
  DATE        := $(shell git log -1 --format=%cd --date=format:"%Y%m%d" 2>/dev/null)
endif

# Compute VERSION
ifeq ($(COMMIT),)
  ifeq ($(DATE),)
    ifeq ($(OS),Windows_NT)
      DATE := $(shell powershell -Command "Get-Date -Format yyyyMMdd")
    else
      DATE := $(shell date +%Y%m%d)
    endif
  endif
  VERSION := $(DATE)
else
  ifneq ($(TAG),)
    ifeq ($(COMMIT),$(TAG_COMMIT))
      VERSION := $(TAG:v%=%)
    else
      VERSION := git-$(COMMIT)
    endif
  else
    VERSION := git-$(COMMIT)
  endif
  ifneq ($(DIRTY),)
    VERSION := $(VERSION)-dirty
  endif
endif

NDK_TOOLCHAIN_VERSION=clang

LOCAL_PATH := $(call my-dir)/../../BAE_Source
include $(CLEAR_VARS)

LOCAL_MODULE    := NeoBAE
LOCAL_SRC_FILES	:= \
			Common/DriverTools.c \
			Common/GenAudioStreams.c \
			Common/GenCache.c \
			Common/GenChorus.c \
			Common/GenFiltersReverbU3232.c \
			Common/GenInterp2ReverbU3232.c \
			Common/GenOutput.c \
			Common/GenPatch.c \
			Common/GenReverb.c \
			Common/GenReverbNew.c \
			Common/GenReverbNeo.c \
			Common/GenSample.c \
			Common/GenSeq.c \
			Common/GenSeqTools.c \
			Common/GenSetup.c \
			Common/GenSong.c \
			Common/GenSoundFiles.c \
			Common/GenSynth.c \
			Common/GenSynthFiltersSimple.c \
			Common/GenSynthFiltersU3232.c \
			Common/GenSynthInterp2Simple.c \
			Common/GenSynthInterp2U3232.c \
			Common/GenSF2_FluidSynth.c \
			Common/GenRMI.c \
      		Common/GenXMF.c \
      		Common/GenRingtone.c \
			Common/NeoBAE.c \
			Common/NewNewLZSS.c \
			Common/SampleTools.c \
			Common/X_API.c \
			Common/X_Decompress.c \
			Common/X_IMA.c \
			Common/X_LZMA.c \
			Common/g711.c \
			Common/g721.c \
			Common/g723_24.c \
			Common/g723_40.c \
			Common/g72x.c \
			Common/sha1mini.c \
			Common/XFileTypes.c \
      		Common/XVorbisFiles.c \
			Common/XOpusFiles.c \
			Common/XQOAFiles.c \
			../mthc/mthc_decomp.c \
			../adp2wav/adp2wav_decode.c \
			../thirdparty/libg722/g722_decode.c \
			../BAE_MPEG_Source_II/XMPEG_minimp3_wrapper.c \
			../BAE_MPEG_Source_II/XMPEGFilesSun.c \
			Platform/jni/com_zefie_NeoBAE_Mixer.c \
			Platform/jni/com_zefie_NeoBAE_SongExt.c \
			Platform/jni/com_zefie_NeoBAE_Sound.c \
			Platform/jni/com_zefie_NeoBAE_SQLiteHelper.c \
			Platform/BAE_API_Android.c \
			../thirdparty/libogg/src/bitwise.c \
			../thirdparty/libogg/src/framing.c \
			../thirdparty/libvorbis/lib/analysis.c \
      		../thirdparty/libvorbis/lib/bitrate.c \
      		../thirdparty/libvorbis/lib/block.c \
      		../thirdparty/libvorbis/lib/codebook.c \
      		../thirdparty/libvorbis/lib/envelope.c \
      		../thirdparty/libvorbis/lib/floor0.c \
      		../thirdparty/libvorbis/lib/floor1.c \
      		../thirdparty/libvorbis/lib/info.c \
      		../thirdparty/libvorbis/lib/lookup.c \
      		../thirdparty/libvorbis/lib/lsp.c \
      		../thirdparty/libvorbis/lib/mapping0.c \
      		../thirdparty/libvorbis/lib/mdct.c \
      		../thirdparty/libvorbis/lib/psy.c \
      		../thirdparty/libvorbis/lib/registry.c \
      		../thirdparty/libvorbis/lib/res0.c \
      		../thirdparty/libvorbis/lib/sharedbook.c \
      		../thirdparty/libvorbis/lib/smallft.c \
      		../thirdparty/libvorbis/lib/synthesis.c \
      		../thirdparty/libvorbis/lib/vorbisfile.c \
      		../thirdparty/libvorbis/lib/lpc.c \
      		../thirdparty/libvorbis/lib/window.c \
			../thirdparty/libvorbis/lib/vorbisenc.c \
      		../thirdparty/flac/src/libFLAC/stream_decoder.c \
			../thirdparty/flac/src/libFLAC/bitreader.c \
			../thirdparty/flac/src/libFLAC/bitmath.c \
			../thirdparty/flac/src/libFLAC/bitwriter.c \
			../thirdparty/flac/src/libFLAC/cpu.c \
			../thirdparty/flac/src/libFLAC/crc.c \
			../thirdparty/flac/src/libFLAC/fixed.c \
			../thirdparty/flac/src/libFLAC/format.c \
			../thirdparty/flac/src/libFLAC/lpc.c \
			../thirdparty/flac/src/libFLAC/md5.c \
			../thirdparty/flac/src/libFLAC/memory.c \
			../thirdparty/flac/src/libFLAC/metadata_iterators.c \
			../thirdparty/flac/src/libFLAC/metadata_object.c \
			../thirdparty/flac/src/libFLAC/stream_encoder_framing.c \
			../thirdparty/flac/src/libFLAC/window.c \
			../thirdparty/flac/src/libFLAC/ogg_decoder_aspect.c \
			../thirdparty/flac/src/libFLAC/ogg_helper.c \
			../thirdparty/flac/src/libFLAC/ogg_mapping.c \
      		../thirdparty/flac/src/libFLAC/stream_encoder.c \
			../thirdparty/flac/src/libFLAC/ogg_encoder_aspect.c \
			../thirdparty/lzma-26.00/C/Alloc.c \
			../thirdparty/lzma-26.00/C/7zAlloc.c \
			../thirdparty/lzma-26.00/C/7zStream.c \
			../thirdparty/lzma-26.00/C/7zCrc.c \
			../thirdparty/lzma-26.00/C/7zCrcOpt.c \
			../thirdparty/lzma-26.00/C/XzCrc64.c \
			../thirdparty/lzma-26.00/C/XzCrc64Opt.c \
			../thirdparty/lzma-26.00/C/Sha256.c \
			../thirdparty/lzma-26.00/C/Sha256Opt.c \
			../thirdparty/lzma-26.00/C/CpuArch.c \
			../thirdparty/lzma-26.00/C/Bra.c \
			../thirdparty/lzma-26.00/C/Bra86.c \
			../thirdparty/lzma-26.00/C/BraIA64.c \
			../thirdparty/lzma-26.00/C/Delta.c \
			../thirdparty/lzma-26.00/C/LzFind.c \
			../thirdparty/lzma-26.00/C/LzmaEnc.c \
			../thirdparty/lzma-26.00/C/LzmaDec.c \
			../thirdparty/lzma-26.00/C/Lzma2Enc.c \
			../thirdparty/lzma-26.00/C/Lzma2Dec.c \
			../thirdparty/lzma-26.00/C/Xz.c \
			../thirdparty/lzma-26.00/C/XzDec.c

# ndk-build doesn't reliably trigger custom prerequisite rules for generated
# headers, so generate at parse time via $(shell ...).
BAE_GEN := $(shell mkdir -p "$(GEN_DIR)" && python3 "$(PATCHES_SCRIPT)" "$(PATCHES_HSB)" "$(PATCHES_H)")


LOCAL_LDFLAGS += -Wl,-z,max-page-size=16384

LOCAL_C_INCLUDES	  := $(LOCAL_PATH)/Common
LOCAL_C_INCLUDES	  += $(LOCAL_PATH)/Platform
LOCAL_C_INCLUDES	  += $(LOCAL_PATH)/../BAE_MPEG_Source_II
LOCAL_C_INCLUDES	  += $(LOCAL_PATH)/../thirdparty/minimp3/
LOCAL_C_INCLUDES	  += $(LOCAL_PATH)/../NeoBAEDroid
LOCAL_C_INCLUDES	  += $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/fluidsynth/include
LOCAL_C_INCLUDES	  += $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/sqlite3/include
LOCAL_C_INCLUDES	  += $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/libopus/include
LOCAL_C_INCLUDES	  += $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/libopus/include/opus
LOCAL_C_INCLUDES	  += $(LOCAL_PATH)/../thirdparty/lzma-26.00/C
LOCAL_C_INCLUDES    += $(LOCAL_PATH)/../thirdparty/config
LOCAL_C_INCLUDES    += $(LOCAL_PATH)/../thirdparty/libogg/include
LOCAL_C_INCLUDES    += $(LOCAL_PATH)/../thirdparty/libvorbis/include
LOCAL_C_INCLUDES    += $(LOCAL_PATH)/../thirdparty/flac/include
LOCAL_C_INCLUDES    += $(LOCAL_PATH)/../thirdparty/flac/src/libFLAC/include
LOCAL_C_INCLUDES    += $(LOCAL_PATH)/../thirdparty/libvorbis/lib
LOCAL_C_INCLUDES    += $(LOCAL_PATH)/../thirdparty/libg722
LOCAL_C_INCLUDES    += $(LOCAL_PATH)/../thirdparty/qoa
LOCAL_C_INCLUDES	+= $(GEN_DIR)

LOCAL_CFLAGS := -std=c99 -O2 -D_VERSION=\"$(VERSION)\" \
	-DX_PLATFORM=X_ANDROID -D__ANDROID__=1 -D_BUILT_IN_PATCHES=1 \
	-DUSE_MINIMP3_WRAPPER=1 -DUSE_VORBIS_DECODER=1 -DUSE_FLAC_DECODER=1 \
	-DUSE_MPEG_DECODER=1 -DUSE_SF2_SUPPORT=1 -DUSE_OGG_FORMAT=1 \
	-DUSE_VORBIS_ENCODER=1 -DUSE_FLAC_ENCODER=1 -D_USING_FLUIDSYNTH=1 \
	-DUSE_XMF_SUPPORT=1 -DUSE_HIGHLEVEL_FILE_API=1 -DSUPPORT_KARAOKE=1 \
	-DUSE_OPUS_DECODER=1 -DUSE_LZMA_COMPRESSION=1 -DUSE_ZMF_FORMAT=1 \
	-DBAE_FIX_SPAN_DC=1 -DBAE_CLASSIC_CHORUS=1 -DFLAC__NO_DLL \
	-D_LOAD_BUILTIN_PATCHES_FOR_SF2=1 -DUSE_MTHC_SUPPORT=1 -DUSE_ADP_SUPPORT=1 \
	-DUSE_RETRO_RINGTONE_SUPPORT=1 -DUSE_QOA_SUPPORT=1 -DUSE_RMI_SUPPORT=1 \
	-DHAVE_CONFIG_H=1 -Wall -fsigned-char -DZ7_ST

ifeq ($(APP_OPTIM),debug)
    LOCAL_CFLAGS += -D_DEBUG=1
endif


# Only set ARM mode for 32-bit ARM builds; do not force for arm64
ifeq ($(TARGET_ARCH_ABI), armeabi-v7a)
LOCAL_ARM_MODE := arm
endif

# for native audio
LOCAL_LDLIBS    += -lOpenSLES
# for logging
LOCAL_LDLIBS    += -llog
# for native asset manager
LOCAL_LDLIBS    += -landroid
# for GenXMF zlib support
LOCAL_LDLIBS    += -lz


# Link against prebuilt FluidSynth
LOCAL_SHARED_LIBRARIES := fluidsynth sndfile ogg vorbis vorbisenc FLAC opus opusfile sqlite3

include $(BUILD_SHARED_LIBRARY)

# Import prebuilt FluidSynth .so
include $(CLEAR_VARS)
LOCAL_MODULE := fluidsynth
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/fluidsynth/lib/libfluidsynth.so
include $(PREBUILT_SHARED_LIBRARY)

# Import prebuilt libsndfile .so
include $(CLEAR_VARS)
LOCAL_MODULE := sndfile
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/libsndfile/lib/libsndfile.so
include $(PREBUILT_SHARED_LIBRARY)

# Import prebuilt libogg .so
include $(CLEAR_VARS)
LOCAL_MODULE := ogg
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/libogg/lib/libogg.so
include $(PREBUILT_SHARED_LIBRARY)

# Import prebuilt libvorbis .so
include $(CLEAR_VARS)
LOCAL_MODULE := vorbis
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/libvorbis/lib/libvorbis.so
include $(PREBUILT_SHARED_LIBRARY)

# Import prebuilt libvorbisenc .so
include $(CLEAR_VARS)
LOCAL_MODULE := vorbisenc
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/libvorbis/lib/libvorbisenc.so
include $(PREBUILT_SHARED_LIBRARY)

# Import prebuilt FLAC .so
include $(CLEAR_VARS)
LOCAL_MODULE := FLAC
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/libflac/lib/libFLAC.so
include $(PREBUILT_SHARED_LIBRARY)

# Import prebuilt opus .so
include $(CLEAR_VARS)
LOCAL_MODULE := sqlite3
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/sqlite3/lib/libsqlite3.so
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/sqlite3/include
include $(PREBUILT_SHARED_LIBRARY)

# Import prebuilt opus .so
include $(CLEAR_VARS)
LOCAL_MODULE := opus
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/libopus/lib/libopus.so
include $(PREBUILT_SHARED_LIBRARY)

# Import prebuilt opusfile .so
include $(CLEAR_VARS)
LOCAL_MODULE := opusfile
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/libopus/lib/libopusfile.so
include $(PREBUILT_SHARED_LIBRARY)

# Import prebuilt opusfile .so
include $(CLEAR_VARS)
LOCAL_MODULE := lzma
LOCAL_SRC_FILES := $(LOCAL_PATH)/../../../deps/android/jniLibs/$(TARGET_ARCH_ABI)/lzma/lib/liblzma.so
include $(PREBUILT_SHARED_LIBRARY)

