# Building Atari800 on Modern macOS Systems

This document provides instructions for building the Atari800 emulator with NetSIO support on modern macOS systems, including Apple Silicon (ARM) Macs.

## Prerequisites

You'll need the following tools and libraries:

1. Xcode Command Line Tools
   ```bash
   xcode-select --install
   ```

2. Homebrew package manager
   ```bash
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

3. Required libraries
   ```bash
   # Install SDL 1.2 compatibility layer (required)
   brew install sdl12-compat
   
   # Optional: Install additional libraries for enhanced functionality
   brew install sdl2_net libpng
   ```

## Build Instructions

1. Clone the repository (if you haven't already)
   ```bash
   git clone https://github.com/your-repo/atari800-moz.git
   cd atari800-moz
   ```

2. Configure the build with Homebrew paths
   ```bash
   # Set environment variables to find Homebrew's headers and libraries
   export CPPFLAGS="-I/opt/homebrew/include"
   export LDFLAGS="-L/opt/homebrew/lib"
   
   # Configure with SDL support
   ./configure --with-sdl-prefix=/opt/homebrew
   ```

3. Build the emulator
   ```bash
   make clean
   make
   ```

4. The executable will be created in the `src` directory as `atari800`

## Running with NetSIO Support

To use the NetSIO protocol with FujiNet:

1. Start the FujiNet service first
   ```bash
   cd /path/to/fujinet-firmware
   ./run-fujinet
   ```

2. In another terminal, run Atari800 with NetSIO enabled
   ```bash
   cd /path/to/atari800-moz/src
   ./atari800 -ntsc -xl -netsio
   ```

## Troubleshooting

### Common Issues

1. **SDL headers not found**
   
   If you see errors like `'SDL/SDL.h' file not found`, ensure SDL12-compat is installed and the CPPFLAGS environment variable is set correctly.

2. **Linker errors**
   
   If you encounter linker errors, make sure LDFLAGS is set correctly to point to Homebrew's library path.

3. **Runtime errors**
   
   If the emulator crashes at startup with library-related errors, you may need to set:
   ```bash
   export DYLD_LIBRARY_PATH=/opt/homebrew/lib
   ```

### Debugging

For more verbose build output:
```bash
make V=1
```

For debugging the NetSIO protocol:
```bash
./atari800 -ntsc -xl -netsio -verbose
```

## Notes for Apple Silicon (ARM) Macs

The build process is the same on Apple Silicon Macs, but note that Homebrew installs to `/opt/homebrew` instead of `/usr/local` on these systems. The instructions above assume an Apple Silicon Mac.

If you're on an Intel Mac, adjust the paths to `/usr/local` instead.
