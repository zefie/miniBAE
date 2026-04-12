# neobae/mingw-static/Makefile
#
# Standalone CLI build for Windows (MinGW cross-compilation).
# Codec libraries are linked as external pre-built static libraries rather
# than compiled from the embedded source trees, except where noted below.
#
# Usage (from neobae/ directory):
#   make -f mingw-static/Makefile [BITS=64|32] [DEBUG=1] [NOAUTO=1] ...
#
# Required pre-built static libraries are searched under
# /opt/llvm-mingw/$(TARGET)/lib by default.
# Override per-library *_DIR variables on the command line if needed:
#
#   libFLAC.a          FLAC_DIR
#   libogg.a           OGG_DIR
#   libvorbis.a        VORBIS_DIR
#   libvorbisenc.a     VORBIS_DIR
#   libvorbisfile.a    VORBIS_DIR
#   libopus.a          OPUS_DIR
#   libopusfile.a      OPUSFILE_DIR
#   libmp3lame.a       LAME_DIR

# ============================================================
# Feature flags  (all on by default; NOAUTO=1 to suppress)
# ============================================================
BAE_BUILT_IN_PATCHES := 1
BAE_API              := WinOS

WINDOWS              := 1

ifneq ($(NOAUTO),1)
ENABLE_MIDI_HW           := 0
ZMF_SUPPORT              := 1
KARAOKE                  := 0
BAESCRIPT                := 1
PLAYLIST                 := 1
SF2_SUPPORT              := 1
USE_FLUIDSYNTH           := 1
EMBED_PATCHES            := 1
EMBED_FONT               := 1
XMF_SUPPORT              := 1
RMI_SUPPORT              := 1
RETRO_RINGTONE_SUPPORT   := 1
DISABLE_NOKIA            := 0
DISABLE_BEATNIK_SF2_NRPN := 0
USE_BUILTIN_PATCHES_WITH_SF2 := 1
FIX_SPAN_DC              := 1
CLASSIC_CHORUS           := 1
LZMA_SUPPORT             := 1
MTHC_SUPPORT             := 1
ADP_SUPPORT              := 1
QOA_SUPPORT              := 1
MP3_DEC                  := 1
MP3_ENC                  := 1
FLAC_DEC                 := 1
FLAC_ENC                 := 1
VORBIS_DEC               := 1
VORBIS_ENC               := 1
OPUS_DEC                 := 1
OPUS_ENC                 := 1
CREATION_NO_CODECS       := 0
CREATION_API             := 0
endif

# ============================================================
# Arch / cross-compiler
# ============================================================

ifeq ($(BITS),32)
TARGET ?= i686-w64-mingw32
else
TARGET ?= x86_64-w64-mingw32
endif

ARCH    ?= -static
CC      ?= $(TARGET)-gcc
CXX     ?= $(TARGET)-g++
AR      := $(TARGET)-ar
STRIP   := $(TARGET)-strip
DLLTOOL := $(TARGET)-dlltool
WINDRES := $(TARGET)-windres

# ============================================================
# External dependency base directory
# ============================================================
# Default external static library prefix for codec dependencies.
LLVM_MINGW_PREFIX ?= $(TOOLCHAIN_PREFIX)/$(TARGET)

# FluidSynth (DLL import lib)
FLUIDSYNTH_DIR   ?= $(LLVM_MINGW_PREFIX)

# Static codec libraries
FLAC_DIR         ?= $(LLVM_MINGW_PREFIX)
OGG_DIR          ?= $(LLVM_MINGW_PREFIX)
VORBIS_DIR       ?= $(LLVM_MINGW_PREFIX)
OPUS_DIR         ?= $(LLVM_MINGW_PREFIX)
OPUSFILE_DIR     ?= $(LLVM_MINGW_PREFIX)
LAME_DIR         ?= $(LLVM_MINGW_PREFIX)

include inc/Makefile.versioning

# ============================================================
# Directory layout
# ============================================================
BUILD_DIR    := build/
TEST_OUT_DIR := tests/
TARGET_OUT   := bin/
OBJ_DIR      := $(BUILD_DIR)obj/
BAE_FLAGS    :=
BAE_LD_FLAGS :=
BAE_LIBS     :=
.DEFAULT_GOAL := all

# ============================================================
# Target names
# ============================================================
TARGET_LIB  := libNeoBAE
TARGET_BIN  := playbae
TARGET_NAME := $(TARGET_BIN)
TARGET_BIN_FULL := $(TARGET_BIN).exe
TEST_BIN    := $(TARGET_OUT)$(TARGET_BIN)

SOURCE_DIR  := $(CURDIR)

# ============================================================
# Embedded asset paths
# ============================================================
EMBED_PATCH_FILE ?= src/banks/patches111/patches111.hsb

# ============================================================
# Verbose flag
# ============================================================
ifneq ($(filter 1,$(VERBOSE) $(V)),)
SILENT :=
else
SILENT := @
endif

# ============================================================
# Feature cascade logic
# ============================================================

# SF2 support requires FluidSynth
ifeq ($(SF2_SUPPORT),1)
USE_FLUIDSYNTH := 1
endif

ifneq ($(USE_FLUIDSYNTH),1)
XMF_SUPPORT := 0
endif

# ZMF support requires LZMA and quality-fix flags
ifeq ($(ZMF_SUPPORT),1)
LZMA_SUPPORT   := 1
FIX_SPAN_DC    := 1
CLASSIC_CHORUS := 1
endif

# CREATION_API also implies ZMF
ifeq ($(CREATION_API),1)
ifneq ($(CREATION_NO_CODECS),1)
OGG_SUPPORT  := 1
MP3_DEC      := 1
MP3_ENC      := 1
FLAC_DEC     := 1
FLAC_ENC     := 1
VORBIS_DEC   := 1
VORBIS_ENC   := 1
OPUS_DEC     := 1
OPUS_ENC     := 1
QOA_SUPPORT  := 1
endif
ZMF_SUPPORT := 1
endif

# Force OGG when any codec that requires it is enabled
ifneq ($(OGG_SUPPORT),1)
ifneq ($(filter 1,$(VORBIS_ENC) $(VORBIS_DEC) $(FLAC_DEC) $(FLAC_ENC) $(OPUS_DEC) $(OPUS_ENC)),)
OGG_SUPPORT := 1
endif
endif

# ============================================================
# Source files
# ============================================================

# Platform API
SRC := src/BAE_Source/Platform/BAE_API_WinOS.c \
	src/BAE_Source/Platform/BAE_API_WinOS_Capture.c \
	src/BAE_Source/Platform/BAE_API_WinOS_Thread.c

# Core libNeoBAE
SRC += \
	src/BAE_Source/Common/DriverTools.c \
	src/BAE_Source/Common/GenAudioStreams.c \
	src/BAE_Source/Common/GenCache.c \
	src/BAE_Source/Common/GenChorus.c \
	src/BAE_Source/Common/GenFiltersReverbU3232.c \
	src/BAE_Source/Common/GenInterp2ReverbU3232.c \
	src/BAE_Source/Common/GenOutput.c \
	src/BAE_Source/Common/GenPatch.c \
	src/BAE_Source/Common/GenReverb.c \
	src/BAE_Source/Common/GenReverbNew.c \
	src/BAE_Source/Common/GenReverbNeo.c \
	src/BAE_Source/Common/GenSample.c \
	src/BAE_Source/Common/GenSeq.c \
	src/BAE_Source/Common/GenSeqTools.c \
	src/BAE_Source/Common/GenSetup.c \
	src/BAE_Source/Common/GenSong.c \
	src/BAE_Source/Common/GenSoundFiles.c \
	src/BAE_Source/Common/GenSynth.c \
	src/BAE_Source/Common/GenSynthFiltersSimple.c \
	src/BAE_Source/Common/GenSynthFiltersU3232.c \
	src/BAE_Source/Common/GenSynthInterp2Simple.c \
	src/BAE_Source/Common/GenSynthInterp2U3232.c \
	src/BAE_Source/Common/NeoBAE.c \
	src/BAE_Source/Common/NewNewLZSS.c \
	src/BAE_Source/Common/SampleTools.c \
	src/BAE_Source/Common/X_API.c \
	src/BAE_Source/Common/X_Decompress.c \
	src/BAE_Source/Common/X_IMA.c \
	src/BAE_Source/Common/g711.c \
	src/BAE_Source/Common/g721.c \
	src/BAE_Source/Common/g723_24.c \
	src/BAE_Source/Common/g723_40.c \
	src/BAE_Source/Common/g72x.c \
	src/BAE_Source/Common/sha1mini.c \
	src/BAE_Source/Common/XFileTypes.c \
	src/playbae/playbae.c

ifeq ($(RETRO_RINGTONE_SUPPORT),1)
SRC += src/BAE_Source/Common/GenRingtone.c
endif

ifeq ($(RMI_SUPPORT),1)
SRC += src/BAE_Source/Common/GenRMI.c
endif

ifeq ($(XMF_SUPPORT),1)
SRC += src/BAE_Source/Common/GenXMF.c
endif

ifeq ($(SF2_SUPPORT),1)
ifeq ($(USE_FLUIDSYNTH),1)
SRC += src/BAE_Source/Common/GenSF2_FluidSynth.c
endif
endif

# MP3: minimp3 (header-only decoder) wrapper and shared MPEG file I/O
ifeq ($(MP3_DEC),1)
SRC += src/BAE_MPEG_Source_II/XMPEG_minimp3_wrapper.c
endif

ifneq ($(filter 1,$(MP3_ENC) $(MP3_DEC)),)
SRC += src/BAE_MPEG_Source_II/XMPEGFilesSun.c
endif

# MP3 encoder: project wrapper — links against external libmp3lame.a
ifeq ($(MP3_ENC),1)
SRC += src/BAE_MPEG_Source_II/XMPEG_lame_encoder.cpp
endif

# Vorbis project wrapper — links against external libvorbis*.a
ifneq ($(filter 1,$(VORBIS_DEC) $(VORBIS_ENC)),)
SRC += src/BAE_Source/Common/XVorbisFiles.c
endif

# Opus project wrapper — links against external libopus.a / libopusfile.a
ifneq ($(filter 1,$(OPUS_DEC) $(OPUS_ENC)),)
SRC += src/BAE_Source/Common/XOpusFiles.c
endif

# QOA (header-only library; include path set below)
ifeq ($(QOA_SUPPORT),1)
SRC += src/BAE_Source/Common/XQOAFiles.c
endif

# LZMA: wrapper + full in-tree 7-Zip LZMA SDK sources
ifeq ($(LZMA_SUPPORT),1)
SRC += src/BAE_Source/Common/X_LZMA.c
SRC += src/thirdparty/lzma-26.00/C/Alloc.c
SRC += src/thirdparty/lzma-26.00/C/7zAlloc.c
SRC += src/thirdparty/lzma-26.00/C/7zCrc.c
SRC += src/thirdparty/lzma-26.00/C/7zCrcOpt.c
SRC += src/thirdparty/lzma-26.00/C/7zStream.c
SRC += src/thirdparty/lzma-26.00/C/XzCrc64.c
SRC += src/thirdparty/lzma-26.00/C/XzCrc64Opt.c
SRC += src/thirdparty/lzma-26.00/C/Sha256.c
SRC += src/thirdparty/lzma-26.00/C/Sha256Opt.c
SRC += src/thirdparty/lzma-26.00/C/CpuArch.c
SRC += src/thirdparty/lzma-26.00/C/Bra.c
SRC += src/thirdparty/lzma-26.00/C/Bra86.c
SRC += src/thirdparty/lzma-26.00/C/BraIA64.c
SRC += src/thirdparty/lzma-26.00/C/Delta.c
SRC += src/thirdparty/lzma-26.00/C/LzmaDec.c
SRC += src/thirdparty/lzma-26.00/C/Lzma2Dec.c
SRC += src/thirdparty/lzma-26.00/C/LzFind.c
SRC += src/thirdparty/lzma-26.00/C/LzmaEnc.c
SRC += src/thirdparty/lzma-26.00/C/Lzma2Enc.c
SRC += src/thirdparty/lzma-26.00/C/Xz.c
SRC += src/thirdparty/lzma-26.00/C/XzDec.c
endif

# MTHC decompressor
ifeq ($(MTHC_SUPPORT),1)
SRC += src/mthc/mthc_decomp.c
endif

# G.711 ADPCM / G.722 — libg722 is a single file with no standard MinGW package;
# kept as an embedded compiled source here.
ifeq ($(ADP_SUPPORT),1)
SRC += src/adp2wav/adp2wav_decode.c \
       src/thirdparty/libg722/g722_decode.c
endif

# Creation API (RMF editor/authoring)
ifeq ($(CREATION_API),1)
SRC += src/BAE_Source/Common/BAERmfEditor.c
endif

# BAEScript
ifeq ($(BAESCRIPT),1)
SRC += src/script/baescript.c \
       src/script/baescript_lexer.c \
       src/script/baescript_parser.c \
       src/script/baescript_vm.c
endif

# Debug helpers
ifeq ($(DEBUG),1)
SRC += src/BAE_Source/Common/X_DebugCallback.c
endif

# ============================================================
# BAE compile flags  (assembled before CFLAGS is defined)
# ============================================================
ifeq ($(DISABLE_NOKIA),1)
BAE_FLAGS += -DDISABLE_NOKIA_PATCH=1
endif

ifeq ($(DISABLE_BEATNIK_SF2_NRPN),1)
BAE_FLAGS += -DDISABLE_BEATNIK_SF2_NRPN=1
endif

ifeq ($(FIX_SPAN_DC),1)
BAE_FLAGS += -DBAE_FIX_SPAN_DC=1
endif

ifeq ($(CLASSIC_CHORUS),1)
BAE_FLAGS += -DBAE_CLASSIC_CHORUS=1
endif

ifeq ($(ZMF_SUPPORT),1)
BAE_FLAGS += -DUSE_ZMF_SUPPORT=1
endif

ifeq ($(RMI_SUPPORT),1)
BAE_FLAGS += -DUSE_RMI_SUPPORT=1
endif

ifeq ($(RETRO_RINGTONE_SUPPORT),1)
BAE_FLAGS += -DUSE_RETRO_RINGTONE_SUPPORT=1
endif

ifeq ($(SF2_SUPPORT),1)
BAE_FLAGS += -DUSE_SF2_SUPPORT=1
ifeq ($(USE_FLUIDSYNTH),1)
BAE_FLAGS += -D_USING_FLUIDSYNTH=1
endif
endif

ifeq ($(XMF_SUPPORT),1)
BAE_FLAGS += -DUSE_XMF_SUPPORT=1
endif

ifeq ($(OGG_SUPPORT),1)
BAE_FLAGS += -DUSE_OGG_FORMAT=1 -DSUPPORT_OGG_FORMAT=1
endif

ifeq ($(MP3_DEC),1)
BAE_FLAGS += -DUSE_MINIMP3_WRAPPER=1 -DUSE_MPEG_DECODER=1
endif

ifeq ($(MP3_ENC),1)
BAE_FLAGS += -DUSE_MPEG_ENCODER=1 -DUSE_LAME_ENCODER=1
endif

ifneq ($(filter 1,$(FLAC_DEC) $(FLAC_ENC)),)
# FLAC__NO_DLL suppresses Windows DLL-import decoration in the public FLAC headers
BAE_FLAGS += -DFLAC__NO_DLL
ifeq ($(OGG_SUPPORT),1)
BAE_FLAGS += -DFLAC__HAS_OGG=1
else
BAE_FLAGS += -DFLAC__HAS_OGG=0
endif
endif

ifeq ($(FLAC_DEC),1)
BAE_FLAGS += -DUSE_FLAC_DECODER=1
endif

ifeq ($(FLAC_ENC),1)
BAE_FLAGS += -DUSE_FLAC_ENCODER=1
endif

ifeq ($(VORBIS_DEC),1)
BAE_FLAGS += -DUSE_VORBIS_DECODER=1
endif

ifeq ($(VORBIS_ENC),1)
BAE_FLAGS += -DUSE_VORBIS_ENCODER=1
endif

ifeq ($(OPUS_DEC),1)
BAE_FLAGS += -DUSE_OPUS_DECODER=1
endif

ifeq ($(OPUS_ENC),1)
BAE_FLAGS += -DUSE_OPUS_ENCODER=1
endif

ifeq ($(LZMA_SUPPORT),1)
# Z7_ST: single-threaded LZMA SDK build
BAE_FLAGS += -DUSE_LZMA_COMPRESSION=1 -DZ7_ST
endif

ifeq ($(MTHC_SUPPORT),1)
BAE_FLAGS += -DUSE_MTHC_SUPPORT=1
endif

ifeq ($(ADP_SUPPORT),1)
BAE_FLAGS += -DUSE_ADP_SUPPORT=1
endif

ifeq ($(QOA_SUPPORT),1)
BAE_FLAGS += -DUSE_QOA_SUPPORT=1
endif

ifeq ($(KARAOKE),1)
BAE_FLAGS += -DSUPPORT_KARAOKE=1
endif

ifeq ($(BAESCRIPT),1)
BAE_FLAGS += -DSUPPORT_BAESCRIPT=1
endif

ifeq ($(PLAYLIST),1)
BAE_FLAGS += -DSUPPORT_PLAYLIST=1
endif

ifeq ($(LOGFILE),1)
BAE_FLAGS += -DOUTPUT_TO_LOGFILE=1
endif

# Platform / GUI defines (always on for this makefile)
BAE_FLAGS += -DX_PLATFORM=X_WIN95

ifeq ($(DEBUG),1)
BAE_FLAGS += -D_DEBUG=1 -Wall -Wno-unused-function
endif

# Embedded patches header generation
ifeq ($(EMBED_PATCHES),1)
ifdef EMBED_PATCH_FILE
BAE_FLAGS += -D_BUILT_IN_PATCHES=1 -I$(OBJ_DIR)
EMBED_PATCH_HEADER := $(OBJ_DIR)BAEPatches.h
$(EMBED_PATCH_HEADER): $(EMBED_PATCH_FILE) ../scripts/create_embedded_patches_h.py
	@echo @embed $<
	@mkdir -p $(dir $(EMBED_PATCH_HEADER))
	@python3 ../scripts/create_embedded_patches_h.py $(EMBED_PATCH_FILE) $(EMBED_PATCH_HEADER)
ifeq ($(USE_BUILTIN_PATCHES_WITH_SF2),1)
BAE_FLAGS += -D_LOAD_BUILTIN_PATCHES_FOR_SF2=1
endif
endif
endif

# ============================================================
# Include paths  (assembled after BAE_FLAGS, before CFLAGS)
# ============================================================
INC_PATH := -Isrc/BAE_Source/Common
INC_PATH += -Isrc/BAE_Source/Platform
INC_PATH += -Isrc/thirdparty/config

ifneq ($(filter 1,$(MP3_ENC) $(MP3_DEC)),)
INC_PATH += -Isrc/BAE_MPEG_Source_II
endif

ifeq ($(MP3_DEC),1)
# minimp3 is a header-only library served from the in-tree copy
INC_PATH += -Isrc/thirdparty/minimp3
endif

ifeq ($(MP3_ENC),1)
INC_PATH += -I$(LAME_DIR)/include
endif

ifneq ($(ENABLE_MIDI_HW),)
INC_PATH += -Isrc/thirdparty/rtmidi
endif

ifneq ($(filter 1,$(FLAC_DEC) $(FLAC_ENC)),)
INC_PATH += -I$(FLAC_DIR)/include
endif

ifeq ($(OGG_SUPPORT),1)
INC_PATH += -I$(OGG_DIR)/include
endif

ifneq ($(filter 1,$(VORBIS_DEC) $(VORBIS_ENC)),)
INC_PATH += -I$(VORBIS_DIR)/include
endif

ifneq ($(filter 1,$(OPUS_DEC) $(OPUS_ENC)),)
INC_PATH += -I$(OPUS_DIR)/include
ifeq ($(OPUS_DEC),1)
INC_PATH += -I$(OPUSFILE_DIR)/include
endif
endif

ifeq ($(LZMA_SUPPORT),1)
INC_PATH += -Isrc/thirdparty/lzma-26.00/C
endif

ifeq ($(QOA_SUPPORT),1)
# QOA is a header-only library served from the in-tree copy
INC_PATH += -Isrc/thirdparty/qoa
endif

ifeq ($(BAESCRIPT),1)
INC_PATH += -Isrc/script
endif

ifeq ($(USE_FLUIDSYNTH),1)
INC_PATH += -I$(FLUIDSYNTH_DIR)/include
endif

# ============================================================
# Object file list
# ============================================================
OBJ_BIN := $(addprefix $(OBJ_DIR),$(addsuffix .o,$(notdir $(basename $(SRC)))))

# ============================================================
# Compiler / linker setup  (CFLAGS expands fully-assembled BAE_FLAGS)
# ============================================================
ifneq ($(filter %.cpp,$(SRC)),)
LD := $(CXX)
else
LD := $(CC)
endif

OPTI := -O2 -fPIC
ifeq ($(LDEBUG),1)
OPTI := -g -O0 -ggdb3 -fPIC
endif

CFLAGS   := $(ARCH) $(OPTI) $(INC_PATH) -D_THREAD_SAFE -Wno-unused-value -DFLAC__NO_DLL
CFLAGS   += $(BAE_FLAGS) -Wno-pragma-pack
CFLAGS   += -D_VERSION=\"$(VERSION)\"

ifneq ($(TARGET),)
    CFLAGS += -I/opt/lvmm-mingw/$(TARGET)/include -I/opt/lvmm-mingw/generic-w64-mingw32/include -I/opt/llvm-mingw/generic-w64-mingw32/include/opus
endif

CXXFLAGS := $(CFLAGS)

LDFLAGS  := $(ARCH) $(OPTI)
ifneq ($(LDEBUG),1)
LDFLAGS += -s
endif

# FluidSynth (DLL import lib)
ifeq ($(USE_FLUIDSYNTH),1)
LDFLAGS += -L$(FLUIDSYNTH_DIR)/lib
endif

# Per-codec static lib search paths
ifneq ($(filter 1,$(FLAC_DEC) $(FLAC_ENC)),)
LDFLAGS += -L$(FLAC_DIR)/lib
endif

ifeq ($(OGG_SUPPORT),1)
LDFLAGS += -L$(OGG_DIR)/lib
endif

ifneq ($(filter 1,$(VORBIS_DEC) $(VORBIS_ENC)),)
LDFLAGS += -L$(VORBIS_DIR)/lib
endif

ifneq ($(filter 1,$(OPUS_DEC) $(OPUS_ENC)),)
LDFLAGS += -L$(OPUS_DIR)/lib
ifeq ($(OPUS_DEC),1)
LDFLAGS += -L$(OPUSFILE_DIR)/lib
endif
endif

ifeq ($(MP3_ENC),1)
LDFLAGS += -L$(LAME_DIR)/lib
endif

OMP_LIB_DIR ?= $(TOOLCHAIN_PREFIX)/$(TARGET)/lib
LDFLAGS += -L$(OMP_LIB_DIR)

# ---- Library list ----
# DLL import libs (must come before static codec group)
FLAC_LINK_LIB    ?= -lFLAC
SNDFILE_LINK_LIB ?= -lsndfile
MPG123_LINK_LIB  ?= -lmpg123
OMP_LINK_LIB     ?= -l:libomp.dll.a

LIBS := \
	-lmingw32

ifeq ($(USE_FLUIDSYNTH),1)
LIBS += $(FLUIDSYNTH_DIR)/lib/libfluidsynth-3.a
endif

# Static codec group (--start-group / --end-group resolves cross-lib dependencies)
STATIC_CODEC_LIBS :=

ifneq ($(filter 1,$(FLAC_DEC) $(FLAC_ENC)),)
STATIC_CODEC_LIBS += $(FLAC_LINK_LIB)
endif

ifeq ($(OGG_SUPPORT),1)
STATIC_CODEC_LIBS += -logg
endif

ifneq ($(filter 1,$(VORBIS_DEC) $(VORBIS_ENC)),)
ifeq ($(VORBIS_ENC),1)
STATIC_CODEC_LIBS += -lvorbisenc
endif
STATIC_CODEC_LIBS += -lvorbisfile -lvorbis
endif

ifneq ($(filter 1,$(OPUS_DEC) $(OPUS_ENC)),)
ifeq ($(OPUS_DEC),1)
STATIC_CODEC_LIBS += -lopusfile
endif
STATIC_CODEC_LIBS += -lopus
endif

ifeq ($(MP3_ENC),1)
STATIC_CODEC_LIBS += -lmp3lame
endif

ifeq ($(XMF_SUPPORT),1)
STATIC_CODEC_LIBS += -lzs
endif

ifneq ($(STATIC_CODEC_LIBS),)
LIBS += -Wl,--start-group $(STATIC_CODEC_LIBS) -Wl,--end-group
endif

# Windows system libraries and C++ runtime
LIBS += \
	-luuid \
	-lstdc++ \
	-lshlwapi \
	-lwinmm \
	-ldsound \
	-ldxguid \
	-lole32


# Static deps
LIBS += \
	$(OMP_LINK_LIB) \
    -lwinpthread \
	$(SNDFILE_LINK_LIB) \
	$(FLAC_LINK_LIB) \
	$(MPG123_LINK_LIB)


# ============================================================
# Embedded asset order-only prerequisites
# ============================================================
ifdef EMBED_PATCH_HEADER
$(OBJ_DIR)NeoBAE.o: $(EMBED_PATCH_HEADER)
endif

# ============================================================
# Build targets
# ============================================================
all: $(TARGET_BIN)

$(TARGET_BIN): ${OBJ_BIN}
	@mkdir -p $(TARGET_OUT)Banks
	$(SHELL) src/banks/copy.sh $(TARGET_OUT)Banks
	@cp -f $(TOOLCHAIN_PREFIX)/$(TARGET)/bin/libomp.dll $(TARGET_OUT)/libomp.dll 2>/dev/null || true
	${LD} -o $(TARGET_OUT)${TARGET_BIN} ${LDFLAGS} ${OBJ_BIN} ${LIBS}

# ============================================================
# Per-file compilation rules  (flattened object names under OBJ_DIR)
# ============================================================
define make_obj_rule
$(OBJ_DIR)$(notdir $(basename $(1))).o: $(1)
	@echo @compile $$<
	@mkdir -p $(OBJ_DIR)
	$(SILENT)$(if $(filter %.cpp,$(1)),$(CXX) -c $(CXXFLAGS),$(CC) -c $(CFLAGS)) $$< -o $$@
endef

ALL_SOURCES := $(sort $(SRC))
$(foreach src,$(ALL_SOURCES),$(eval $(call make_obj_rule,$(src))))

# ============================================================
# clean
# ============================================================
clean:
	@rm -rf $(TARGET_OUT)
	@rm -rf $(OBJ_DIR)
	@rm -rf $(BUILD_DIR)
	@rm -rf $(TEST_OUT_DIR)
	@echo Cleaned!
