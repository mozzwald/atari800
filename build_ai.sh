#!/bin/bash
# Build atari800 with AI interface

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
    --enable-ai-interface \
    --enable-netsio \
    SDL_CONFIG=/usr/bin/sdl-config \
    CFLAGS="-O2 -g"

# Build
echo "Building..."
make -j4

echo ""
echo "Build complete!"
echo "Binary: src/atari800"
echo ""
echo "Usage:"
echo "  ./src/atari800 -netsio -ai -xl -run your_program.xex"
echo ""
echo "Then connect to socket: /tmp/atari800_ai.sock"
echo "Send JSON commands with length prefix (e.g., '14\\n{\"cmd\":\"ping\"}')"
