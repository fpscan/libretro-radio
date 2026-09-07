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

# libmad's fixed-point multiply. This guess is from the build machine, so every
# cross-compiled target below sets FPM_DEFINE for itself instead.
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

# macOS, native or cross from the other arch
else ifeq ($(platform), osx)
   TARGET := $(TARGET_NAME)_libretro.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   LIBS += -lm -lpthread
ifeq ($(CROSS_COMPILE),1)
   TARGET_RULE := -target $(LIBRETRO_APPLE_PLATFORM) -isysroot $(LIBRETRO_APPLE_ISYSROOT)
   CFLAGS += $(TARGET_RULE)
   LDFLAGS += $(TARGET_RULE)
   LIBMAD_EXTRA_CFLAGS += $(TARGET_RULE)
endif

# iOS
else ifeq ($(platform), ios-arm64)
   TARGET := $(TARGET_NAME)_libretro_ios.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   LIBS += -lm -lpthread
   FPM_DEFINE := -DFPM_64BIT
   # 12.0 and up gets an LC_BUILD_VERSION load command; below that the
   # linker emits LC_VERSION_MIN_IPHONEOS, which the buildbot's platform check
   # (vtool -show | grep IOS) does not recognise
   MINVERSION := -miphoneos-version-min=12.0
   IOSSDK := $(shell xcrun -sdk iphoneos -show-sdk-path)
   CC := cc -arch arm64 -isysroot $(IOSSDK)
   CFLAGS += $(MINVERSION)
   LDFLAGS += $(MINVERSION)
   LIBMAD_EXTRA_CFLAGS += $(MINVERSION)

# tvOS
else ifeq ($(platform), tvos-arm64)
   TARGET := $(TARGET_NAME)_libretro_tvos.dylib
   fpic := -fPIC
   SHARED := -dynamiclib
   LIBS += -lm -lpthread
   FPM_DEFINE := -DFPM_64BIT
   MINVERSION := -mappletvos-version-min=11.0
   IOSSDK := $(shell xcrun -sdk appletvos -show-sdk-path)
   CC := cc -arch arm64 -isysroot $(IOSSDK)
   CFLAGS += $(MINVERSION)
   LDFLAGS += $(MINVERSION)
   LIBMAD_EXTRA_CFLAGS += $(MINVERSION)

# Android. pthreads and the sockets are in libc here, so no -lpthread.
else ifneq (,$(findstring android,$(platform)))
   TARGET := $(TARGET_NAME)_libretro_android.so
   fpic := -fPIC
   SHARED := -shared -Wl,--version-script=link.T -Wl,--no-undefined
   LIBS += -lm
   ANDROID_API ?= 21
   CC_PREFIX := $(ANDROID_NDK_LLVM)/bin/
ifeq ($(platform), android-arm)
   CC := $(CC_PREFIX)armv7a-linux-androideabi$(ANDROID_API)-clang
   FPM_DEFINE := -DFPM_ARM
   # FPM_ARM's inline asm is ARM-only encodings; the NDK compiles thumb
   LIBMAD_EXTRA_CFLAGS += -marm
else ifeq ($(platform), android-arm64)
   CC := $(CC_PREFIX)aarch64-linux-android$(ANDROID_API)-clang
   FPM_DEFINE := -DFPM_64BIT
else ifeq ($(platform), android-x86)
   CC := $(CC_PREFIX)i686-linux-android$(ANDROID_API)-clang
   FPM_DEFINE := -DFPM_DEFAULT
else ifeq ($(platform), android-x86_64)
   CC := $(CC_PREFIX)x86_64-linux-android$(ANDROID_API)-clang
   FPM_DEFINE := -DFPM_64BIT
endif

# Windows, and anything unrecognised
else
   TARGET := $(TARGET_NAME)_libretro.dll
   SHARED := -shared -static-libgcc -static-libstdc++
   LIBS += -lm -lws2_32 -lwinmm
ifeq ($(platform), win32)
   FPM_DEFINE := -DFPM_DEFAULT
endif
endif

CC ?= gcc

LIBMAD_CFLAGS := -O3 $(fpic) -I$(LIBMAD_DIR) -I$(LIBRETRO_COMMON_INC) \
   $(FPM_DEFINE) \
   -DHAVE_CONFIG_H=0 \
   -Wno-unused-function \
   -Wno-sign-compare \
   -Wno-shift-negative-value \
   $(LIBMAD_EXTRA_CFLAGS)

CFLAGS += -O3 -Wall $(fpic) -I. -I$(LIBRETRO_COMMON_INC) -I$(LIBMAD_DIR) -D__LIBRETRO__ $(FPM_DEFINE) -ffast-math

OBJS := libretro-radio.o $(LIBMAD_OBJS)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS) $(SHARED) $(LIBS)

libretro-radio.o: libretro-radio.c
	$(CC) -c -o $@ $< $(CFLAGS)

libmad_%.o: $(LIBMAD_DIR)/%.c
	$(CC) -c -o $@ $< $(LIBMAD_CFLAGS)

clean:
	rm -f libretro-radio.o $(LIBMAD_OBJS) $(TARGET)

.PHONY: all clean
