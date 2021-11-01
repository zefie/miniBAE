BAE_BUILT_IN_PATCHES	:= 1
BAE_API			:= Ansi
DEBUG			:= 0
include inc/Makefile.common

ARCH		:= -m32 -static
CC		:= gcc
CXX		:= g++
LD		:= $(CC)
AR      	:= ar
STRIP		:= strip

OPTI            := -O2 -fPIC
CFLAGS  	:= $(ARCH) $(OPTI) -g $(INC_PATH) -D_THREAD_SAFE -Wno-unused-value

ifneq ($(BAE_FLAGS),)
	CFLAGS	+= $(BAE_FLAGS)
endif

include inc/Makefile.versioning
CFLAGS		+= -D_VERSION=\""$(VERSION)"\"

CXXFLAGS 	:= $(CFLAGS)
LDFLAGS		:= $(ARCH) $(OPTI) -s

LIBS	= 	-lpthread

all: $(TARGET_LIB).a ${TARGET_LIB}.so $(TARGET_BIN)

$(TARGET_LIB).a: $(OBJ)
	@mkdir -p $(TARGET_OUT)
	$(AR) rcs $(TARGET_OUT)$(TARGET_LIB).a $(OBJ)

$(TARGET_LIB).so: ${OBJ}
	@mkdir -p $(TARGET_OUT)
	${LD} -shared ${ARCH} ${OBJ} $(LIBS) -o $(TARGET_OUT)${TARGET_LIB}.so

$(TARGET_BIN): ${OBJ_BIN}
	@mkdir -p $(TARGET_OUT)
	${LD} -s -o $(TARGET_OUT)${TARGET_BIN} ${LDFLAGS} ${OBJ_BIN} ${LIBS}

pack: ${TARGET_BIN}
	cat $(TARGET_OUT)${TARGET_BIN} | gzip -9c > $(TARGET_OUT)playbae_linux32_static.gz

# Rules for compiling source files to object files
$(OBJ_DIR)%.o : %.cpp
	@echo @compile $<
	@mkdir -p $(OBJ_DIR)
	@${CXX} -c ${CXXFLAGS} $< -o $@

$(OBJ_DIR)%.o : %.c
	@echo @compile $<
	@mkdir -p $(OBJ_DIR)
	@${CC} -c ${CFLAGS} $< -o $@

clean:
	@rm -rdf $(TARGET_OUT)
	@rm -rdf $(OBJ_DIR)
	@rm -rdf $(BUILD_DIR)
	@rm -rdf $(TEST_OUT_DIR)
	@echo Cleaned!

include inc/Makefile.tests
