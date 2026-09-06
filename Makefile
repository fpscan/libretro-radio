STATIC_LINKING := 0
AR             := ar

ifeq ($(platform),)
platform = unix
ifeq ($(shell uname -a),)
   platform = win
else ifneq ($(findstring MINGW,$(shell uname -a)),)
   platform = win
else ifneq ($(findstring Darwin,$(shell uname -a)),)
   platform = osx
else ifneq ($(findstring win,$(shell uname -a)),)
   platform = win
endif
endif

TARGET_NAME := radio

ARCH ?= $(shell uname -m)
ifneq ($(findstring 64,$(ARCH)),)
   FPM_DEFINE := -DFPM_64BIT
else ifneq ($(findstring arm,$(ARCH)),)
   FPM_DEFINE := -DFPM_ARM
else
   FPM_DEFINE := -DFPM_DEFAULT
endif

LIBMAD_DIR := deps/libretro-deps/libmad
LIBRETRO_COMMON_INC := deps/include

LIBMAD_SRCS := \
   $(LIBMAD_DIR)/bit.c \
   $(LIBMAD_DIR)/decoder.c \
   $(LIBMAD_DIR)/fixed.c \
   $(LIBMAD_DIR)/frame.c \
   $(LIBMAD_DIR)/huffman.c \
   $(LIBMAD_DIR)/layer12.c \
   $(LIBMAD_DIR)/layer3.c \
   $(LIBMAD_DIR)/stream.c \
   $(LIBMAD_DIR)/synth.c \
   $(LIBMAD_DIR)/timer.c

LIBMAD_OBJS := $(patsubst $(LIBMAD_DIR)/%.c,libmad_%.o,$(LIBMAD_SRCS))

ifeq ($(platform), unix)
   TARGET := $(TARGET_NAME)_libretro.so
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined
   LIBS += -lm -lpthread
else ifeq ($(platform), osx)
   TARGET := $(TARGET_NAME)_libretro.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   LIBS += -lm -lpthread
else
   TARGET := $(TARGET_NAME)_libretro.dll
   SHARED := -shared -static-libgcc -static-libstdc++
   LIBS += -lm -lws2_32 -lwinmm
endif

CC ?= gcc

LIBMAD_CFLAGS := -O3 $(fpic) -I$(LIBMAD_DIR) -I$(LIBRETRO_COMMON_INC) \
   $(FPM_DEFINE) \
   -DHAVE_CONFIG_H=0 \
   -Wno-unused-function \
   -Wno-sign-compare \
   -Wno-shift-negative-value

CFLAGS += -O3 -Wall $(fpic) -I. -I$(LIBRETRO_COMMON_INC) -I$(LIBMAD_DIR) -D__LIBRETRO__ $(FPM_DEFINE) -ffast-math

OBJS := libretro-radio.o $(LIBMAD_OBJS)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(SHARED) $(LIBS)

libretro-radio.o: libretro-radio.c
	$(CC) -c -o $@ $< $(CFLAGS)

libmad_%.o: $(LIBMAD_DIR)/%.c
	$(CC) -c -o $@ $< $(LIBMAD_CFLAGS)

clean:
	rm -f libretro-radio.o $(LIBMAD_OBJS) $(TARGET)

.PHONY: all clean
