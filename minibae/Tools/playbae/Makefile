TARGET = playbae.exe

vpath	%.c ../../BAE_Source/Common ../../BAE_Source/Platform ../../BAE_MPEG_Source_II

INC_PATH	:= -I../../BAE_Source/Common
INC_PATH	+= -I../../BAE_Source/Platform
INC_PATH	+= -I../../BAE_MPEG_Source_II

SRC		:=	BAE_API_WinOS.c \
			BAE_API_WinOS_Capture.c \
			BAE_API_WinOS_Thread.c \

SRC		+= \
			DriverTools.c \
			GenAudioStreams.c \
			GenCache.c \
			GenChorus.c \
			GenFiltersReverbU3232.c \
			GenInterp2ReverbU3232.c \
			GenOutput.c \
			GenPatch.c \
			GenReverb.c \
			GenReverbNew.c \
			GenSample.c \
			GenSeq.c \
			GenSeqTools.c \
			GenSetup.c \
			GenSong.c \
			GenSoundFiles.c \
			GenSynth.c \
			GenSynthFiltersSimple.c \
			GenSynthFiltersU3232.c \
			GenSynthInterp2Simple.c \
			GenSynthInterp2U3232.c \
			MiniBAE.c \
			NewNewLZSS.c \
			SampleTools.c \
			X_API.c \
			X_Decompress.c \
			X_IMA.c \
			g711.c \
			g721.c \
			g723_24.c \
			g723_40.c \
			g72x.c

			
#SRC		+= 	\
#		XMPEGFilesSun.c \
#		XMPEG_BAE_API.c \
#		mpa_c_copybytes.c \
#		mpa_conv.c \
#		mpa_dec.c \
#		mpa_dec_bits.c \
#		mpa_dec_crc.c \
#		mpa_dec_l1.c \
#		mpa_dec_l12.c \
#		mpa_dec_l2.c \
#		mpa_dec_l3.c \
#		mpa_dec_l3hf.c \
#		mpa_dec_l3u.c \
#		mpa_dec_poly.c \
#		mpa_dec_tab.c \
#		mpa_dec_vec.c \
#		mpa_version.c
			
SRC		+=	playbae.c

OBJ_DIR := obj/
OBJ := $(addprefix $(OBJ_DIR),$(addsuffix .o,$(basename ${SRC})))

#ARCH	:= -arch x86_64
ARCH	:= -m32 -static
CC		:= i686-w64-mingw32-gcc-win32
CXX		:= i686-w64-mingw32-g++-win32
LD		:= i686-w64-mingw32-g++-win32

CFLAGS  	:= $(ARCH) -g -Os $(INC_PATH) -D_THREAD_SAFE -Wno-unused-value
CFLAGS		+= -DX_PLATFORM=X_WIN95
#CFLAGS		+= -DUSE_MPEG_DECODER=1
CFLAGS		+= -D_DEBUG=1
#CFLAGS		+= -DREVERB_USED=REVERB_DISABLED
CXXFLAGS 	:= $(CFLAGS)
LDFLAGS		:= $(ARCH)


LIBS	= 	-L/opt/local/lib \
			-ldxguid \
			-ldsound \
			-lole32 \
			-lwinmm \
			-lkernel32 \
			-luser32 \
			-lgdi32 \
			-lwinspool \
			-lcomdlg32 \
			-ladvapi32 \
			-lshell32 \
			-loleaut32 \
			-luuid \
			-lodbc32 \
			-lodbccp32
#			-Wl,-framework,CoreFoundation \
#			-Wl,-framework,CoreAudio \
#			-Wl,-framework,AudioToolbox

all: $(TARGET)

$(TARGET): $(OBJ)
	${LD} -o $@ ${LDFLAGS} ${OBJ} ${LIBS}
		
# Rules for compiling source files to object files
$(OBJ_DIR)%.o : %.cpp
	@echo @compile $<
	@mkdir -p $(OBJ_DIR)
	@${CXX} -c ${CXXFLAGS} $< -o $@

$(OBJ_DIR)%.o : %.c
	@echo @compile $<
	@mkdir -p $(OBJ_DIR)
	@${CC} -c ${CFLAGS} $< -o $@

install: all
	cp $(TARGET) ../../bin/.

lib_install:

CONTENT := ../../Content
BANKS := ../../banks

# requires mp3 codec
#TEST_RMF    := LT_RMF_MPEG.rmf
#TEST_RMF	:= little_boss.rmf
#TEST_RMF	:= Ajarna.rmf
#TEST_RMF	:= health_and_safety.rmf

# standard
#TEST_RMF	:= renegado-full.rmf
#TEST_RMF	:= the-future-is.rmf
#TEST_RMF    := jetset-full.rmf
#TEST_RMF	:= "Hide 'n Seek.rmf"
#TEST_RMF	:= polyzoot-full.rmf
TEST_RMF	:= chop-squad-full.rmf

test: ${TARGET}
	./${TARGET} -p ../TestSuite/minibae-wtv.hsb -m ../TestSuite/world1.mid -mr 44100 -o ~/test.wav

test2: ${TARGET}
	./${TARGET} -p ../TestSuite/minibae-wtv.hsb -m ../TestSuite/karTV.mid -mr 44100 -o ~/test.wav

test3: ${TARGET}
	./${TARGET} -p ../TestSuite/minibae-wtv.hsb -m ../TestSuite/world1.mid -mr 11025 -o ~/test.wav

pack: ${TARGET}
	cat ./${TARGET} | gzip -9c > ~/playbae.exe.gz

#test1: $(TARGET)
#	./$(TARGET) -p $(TEST_BANK) -m $(CONTENT)/rmf/kartv.mid -mr 44100

#test2: $(TARGET)
#	./$(TARGET) -p $(TEST_BANK) -m $(CONTENT)/midi/synco.mid -mr 44100

#test3: $(TARGET)
#	./$(TARGET) -p $(TEST_BANK) -r $(CONTENT)/rmf/$(TEST_RMF) -mr 44100

#test4: $(TARGET)
#	./$(TARGET) -p $(TEST_BANK) -w $(CONTENT)/wav/sexy_coffee_pot.wav -mr 44100

#test5: $(TARGET)
#	./$(TARGET) -p $(TEST_BANK) -sw $(CONTENT)/wav/sexy_coffee_pot.wav -mr 44100

clean:
	@rm -f $(TARGET)
	@rm -rdf $(OBJ_DIR)
	@rm -rdf build
	@echo Cleaned!


