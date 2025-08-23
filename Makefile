# Git Video Codec - Makefile
# Builds encoder and player for storing/playing video frames as Git commits

CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -O3 -g
LDFLAGS = -lm -lpthread

# Metal player flags (high performance with aggressive optimizations)
LIBGIT2_PREFIX = $(shell brew --prefix libgit2 2>/dev/null || echo "/usr/local")
METAL_CFLAGS = -std=c99 -O3 -flto -ffast-math -funroll-loops -mtune=native -DMETAL_DISPLAY -I$(LIBGIT2_PREFIX)/include
METAL_LDFLAGS = -lm -lpthread -lz -L$(LIBGIT2_PREFIX)/lib -lgit2 -lcompression -framework Foundation -framework AppKit -framework Metal -framework MetalKit -framework CoreVideo -framework QuartzCore

# Platform-specific settings
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lX11 -lz
endif
ifeq ($(UNAME_S),Darwin)
    LDFLAGS += -framework Cocoa -framework OpenGL -lz -lcompression
    # Detect Apple Silicon for Metal optimization
    ARCH := $(shell uname -m)
    ifeq ($(ARCH),arm64)
        METAL_AVAILABLE = 1
    endif
endif
ifeq ($(OS),Windows_NT)
    LDFLAGS += -lgdi32 -luser32 -lz
    CFLAGS += -DWIN32
endif

# Source files
COMMON_SRCS = src/compression.c src/git_ops.c src/frame_format.c
ENCODER_LIB_SRCS = src/encoder_lib.c $(COMMON_SRCS)
ENCODER_SRCS = src/encoder.c $(ENCODER_LIB_SRCS)
PLAYER_SRCS = src/player.c src/display.m $(COMMON_SRCS)
METAL_PLAYER_SRCS = src/player_metal.c src/display_metal.m src/git_ops_libgit2.c src/compression.c src/frame_format.c
MP4_CONVERTER_SRCS = src/mp4_converter.c $(ENCODER_LIB_SRCS)

# Output binaries
ENCODER_BIN = git-vid-encode
PLAYER_BIN = git-vid-play
METAL_PLAYER_BIN = git-vid-play-metal
MP4_CONVERTER_BIN = git-vid-convert

# Default target
all: $(ENCODER_BIN) $(PLAYER_BIN) $(MP4_CONVERTER_BIN)

# Metal target (macOS only)
ifeq ($(METAL_AVAILABLE),1)
metal: $(ENCODER_BIN) $(METAL_PLAYER_BIN)
else
metal:
	@echo "Metal not available on this platform"
endif

# Create directories
src:
	mkdir -p src

# Encoder binary
$(ENCODER_BIN): $(ENCODER_SRCS) | src
	$(CC) $(CFLAGS) -o $@ $(ENCODER_SRCS) $(LDFLAGS)

# Player binary
$(PLAYER_BIN): $(PLAYER_SRCS) | src
	$(CC) $(CFLAGS) -o $@ $(PLAYER_SRCS) $(LDFLAGS)

# MP4 converter binary
$(MP4_CONVERTER_BIN): $(MP4_CONVERTER_SRCS) | src
	$(CC) $(CFLAGS) -o $@ $(MP4_CONVERTER_SRCS) $(LDFLAGS)

# High-performance Metal player binary (macOS only)
$(METAL_PLAYER_BIN): $(METAL_PLAYER_SRCS) | src
	$(CC) $(METAL_CFLAGS) -o $@ $(METAL_PLAYER_SRCS) $(METAL_LDFLAGS)

# Clean build artifacts
clean:
	rm -f $(ENCODER_BIN) $(PLAYER_BIN) $(METAL_PLAYER_BIN) $(MP4_CONVERTER_BIN)

# Install binaries
install: all
	cp $(ENCODER_BIN) $(PLAYER_BIN) $(MP4_CONVERTER_BIN) /usr/local/bin/

# Test with sample data
test: all
	./test.sh

# Static analysis
lint:
	cppcheck --enable=all src/

.PHONY: all clean install test lint