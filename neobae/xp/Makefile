TEST_BIN_PREFIX 	:= "wine "
BITS := 32
BAE_BUILT_IN_PATCHES	:= 1
WINDOWS		:= 0
XPHACK := 1
BAE_API := WinOS

ifeq ($(USE_SDL2),1)
    BAE_API		:= SDL2
endif

include inc/Makefile.common
TARGET_BIN		:= $(TARGET_NAME).exe

CROSS 		:= i686-w64-mingw32
ARCH		:= -static
CC			:= $(CROSS)-gcc
CXX			:= $(CROSS)-g++

# Use C++ linker if we have any C++ sources
ifneq ($(filter %.cpp,$(SRC) $(SRC_BIN)),)
LD			:= $(CXX)
else
LD			:= $(CC)
endif
AR      	:= $(CROSS)-gcc-ar
STRIP      	:= $(CROSS)-strip
DLLTOOL		:= $(CROSS)-dlltool
WINDRES		:= $(CROSS)-windres

INC_PATH += -I/mingw32-xp/local/include -I/mingw32-xp/include
BAE_LD_FLAGS += -L/mingw32-xp/local/lib -L/mingw32-xp/lib


OPTI            := -O2 -fPIC
ifeq ($(LDEBUG),1)
    OPTI := -g -O0 -ggdb3 -fPIC
endif
CFLAGS  	:= $(ARCH) $(OPTI) $(INC_PATH)

ifneq ($(BAE_FLAGS),)
	CFLAGS	+= $(BAE_FLAGS)
endif

include inc/Makefile.versioning
CFLAGS		+= -D_VERSION=\""$(VERSION)"\"
CFLAGS += -I/mingw32-xp/i686-w64-mingw32/include -I/mingw32-xp/local/include -I/mingw32-xp/opt/include -I/mingw32-xp/include
CXXFLAGS 	:= $(CFLAGS)
LDFLAGS		:= $(ARCH) $(OPTI)
LDFLAGS += -L/mingw32-xp/i686-w64-mingw32/lib -L/mingw32-xp/local/lib -L/mingw32-xp/opt/lib -L/mingw32-xp/lib


ifneq ($(BAE_LD_FLAGS),)
	LDFLAGS += $(BAE_LD_FLAGS)
endif

ifneq ($(LDEBUG),1)
	LDFLAGS += -s
endif

LIBS   = \
    -ldxguid \
    -ldsound \
    -lwinmm \
    -lole32 \
	-lfluidsynth-3.dll \
	-lpsapi

ifeq ($(USE_SDL2),1)
    # Override LIBS completely for SDL build to enforce correct ordering
    LIBS += \
        -lSDL2.dll 
endif

ifneq ($(BAE_LD_FLAGS),)
    LD_FLAGS += $(BAE_LD_FLAGS)
endif

ifneq ($(BAE_LIBS),)
    LIBS += $(BAE_LIBS)
endif

# Use C++ linker if we have any C++ sources
ifneq ($(filter %.cpp,$(SRC) $(SRC_BIN)),)
    LD := $(CXX)
    LIBS += -lstdc++ -lsupc++
endif

CXXFLAGS 	:= $(CFLAGS)

all: $(TARGET_LIB).a ${TARGET_LIB}.dll $(TARGET_BIN)

$(TARGET_LIB).a: $(OBJ_BIN)
	@mkdir -p $(TARGET_OUT)
	$(AR) rcs $(TARGET_OUT)$(TARGET_LIB).a $(OBJ_BIN)

$(TARGET_LIB).dll: ${OBJ_BIN}
	@mkdir -p $(TARGET_OUT)
	@$(DLLTOOL) --export-all --export-all-symbols -e $(OBJ_DIR)exports.o -z $(OBJ_DIR)$(TARGET_LIB).def2 $(OBJ_BIN)
	@echo "EXPORTS" > $(OBJ_DIR)$(TARGET_LIB).def
	@cat $(OBJ_DIR)$(TARGET_LIB).def2 | grep "BAE" >> $(OBJ_DIR)$(TARGET_LIB).def
	@rm -f $(OBJ_DIR)$(TARGET_LIB).def2 $(OBJ_DIR)exports.o
	@$(DLLTOOL) -e $(OBJ_DIR)exports.o -d $(OBJ_DIR)$(TARGET_LIB).def -l $(TARGET_OUT)$(TARGET_LIB).lib $(OBJ_BIN)

$(TARGET_BIN): ${OBJ_BIN}
	@mkdir -p $(TARGET_OUT)Banks
	$(SHELL) src/banks/copy.sh $(TARGET_OUT)Banks
	${LD} -o $(TARGET_OUT)${TARGET_BIN} ${LDFLAGS} ${OBJ_BIN} ${LIBS}
	@cp /mingw32-xp/bin/libgcc_s_dw2-1.dll $(TARGET_OUT) 2>/dev/null || true;
	@cp /mingw32-xp/bin/libwinpthread-1.dll $(TARGET_OUT) 2>/dev/null || true;
	@cp /mingw32-xp/bin/libstdc++-6.dll $(TARGET_OUT) 2>/dev/null || true;
	@cp /mingw32-xp/bin/libgomp-1.dll $(TARGET_OUT) 2>/dev/null || true;
	@cp /mingw32-xp/bin/libreadline8.dll $(TARGET_OUT) 2>/dev/null || true;
	@cp /mingw32-xp/bin/libssp-0.dll $(TARGET_OUT) 2>/dev/null || true;
	
	@if [ "$(USE_FLUIDSYNTH)" = "1" ]; then \
		cp /mingw32-xp/local/bin/libfluidsynth-3.dll $(TARGET_OUT) 2>/dev/null || true; \
		cp /mingw32-xp/local/bin/libsndfile.dll $(TARGET_OUT) 2>/dev/null || true; \
		cp /mingw32-xp/local/bin/libmp3lame-0.dll $(TARGET_OUT) 2>/dev/null || true; \
		cp /mingw32-xp/local/bin/libbz2.dll $(TARGET_OUT) 2>/dev/null || true; \
		cp /mingw32-xp/local/bin/libmpg123-0.dll $(TARGET_OUT) 2>/dev/null || true; \
		cp /mingw32-xp/local/bin/libharfbuzz-0.dll $(TARGET_OUT) 2>/dev/null || true; \
		cp /mingw32-xp/local/bin/libpng16.dll $(TARGET_OUT) 2>/dev/null || true; \
		cp /mingw32-xp/local/bin/zlib1.dll $(TARGET_OUT) 2>/dev/null || true; \
	fi
	
	@if [ "$(OGG_SUPPORT)" = "1" ]; then \
		cp /mingw32-xp/local/bin/libogg.dll $(TARGET_OUT)/ogg.dll 2>/dev/null || true; \
		\
		if [ "$(FLAC_DEC)" = "1" ] || [ "$(FLAC_ENC)" = "1" ]; then \
			cp /mingw32-xp/local/bin/libFLAC.dll $(TARGET_OUT) 2>/dev/null || true; \
		fi; \
		\
		if [ "$(VORBIS_ENC)" = "1" ] || [ "$(VORBIS_DEC)" = "1" ]; then \
			cp /mingw32-xp/local/bin/libvorbis.dll $(TARGET_OUT) 2>/dev/null || true; \
			cp /mingw32-xp/local/bin/libvorbisfile.dll $(TARGET_OUT) 2>/dev/null || true; \
		fi; \
		\
		if [ "$(VORBIS_ENC)" = "1" ]; then \
			cp /mingw32-xp/local/bin/libvorbisenc.dll $(TARGET_OUT) 2>/dev/null || true; \
		fi; \
		\
		if [ "$(OPUS_DEC)" = "1" ]; then \
			cp /mingw32-xp/local/bin/libopus.dll $(TARGET_OUT) 2>/dev/null || true; \
			cp /mingw32-xp/local/bin/libopusfile.dll $(TARGET_OUT) 2>/dev/null || true; \
		fi; \
	fi
	
	@if [ "$(BAE_API)" = "SDL2" ]; then \
		cp /mingw32-xp/local/bin/SDL2.dll $(TARGET_OUT)/libSDL2.dll 2>/dev/null || true; \
	fi

# Rules for compiling source files to object files
$(OBJ_DIR)%.o : %.cpp
	@echo @compile $<
	@mkdir -p $(OBJ_DIR)
	$(SILENT)${CXX} -c ${CXXFLAGS} $< -o $@

$(OBJ_DIR)%.o : %.c
	@echo @compile $<
	@mkdir -p $(OBJ_DIR)
	$(SILENT)${CC} -c ${CFLAGS} $< -o $@

clean:
	@rm -rf $(TARGET_OUT)
	@rm -rf $(OBJ_DIR)
	@rm -rf $(BUILD_DIR)
	@rm -rf $(TEST_OUT_DIR)
	@echo Cleaned!

# Generate rules for all unique source files (same approach as top-level Makefile)
ALL_SOURCES := $(sort $(SRC) $(SRC_BIN))
$(foreach src,$(ALL_SOURCES),$(eval $(call make_obj_rule,$(src))))

include inc/Makefile.tests
