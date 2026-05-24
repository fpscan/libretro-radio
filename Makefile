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
CFLAGS += -O3 -Wall $(fpic) -I../../libretro-common/include -I../../deps/dr -D__LIBRETRO__

# Compile with fast math for visualizer if possible
CFLAGS += -ffast-math

OBJS := libretro-radio.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(SHARED) $(LIBS)

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
