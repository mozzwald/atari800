#!/bin/bash
# Build atari800 with AI interface

set -e

cd /tmp/atari800_src

# Run autoconf if needed
if [ ! -f configure ]; then
    echo "Running autoreconf..."
    autoreconf -i
fi

# Configure with SDL 1.2 (NOT SDL2 - SDL2 has broken keyboard trigger!)
echo "Configuring with SDL 1.2..."
./configure \
    --with-video=sdl \
    --with-sound=sdl \
    SDL_CONFIG=/opt/homebrew/bin/sdl-config \
    CFLAGS="-O2 -g"

# Add ai_interface.o to the build
# Patch the Makefile after configure generates it
if [ -f src/Makefile ]; then
    # Add ai_interface.c to sources
    if ! grep -q "ai_interface" src/Makefile; then
        sed -i.bak 's/SRCS = /SRCS = ai_interface.c /' src/Makefile
        echo "Added ai_interface.c to Makefile"
    fi
fi

# Build
echo "Building..."
make -j4

echo ""
echo "Build complete!"
echo "Binary: src/atari800"
echo ""
echo "Usage:"
echo "  ./src/atari800 -ai -xl -run your_program.xex"
echo ""
echo "Then connect to socket: /tmp/atari800_ai.sock"
echo "Send JSON commands with length prefix (e.g., '14\\n{\"cmd\":\"ping\"}')"
