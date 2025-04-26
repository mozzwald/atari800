# Building Atari800 Emulator on macOS

This guide explains how to build the Atari800 emulator on macOS, including support for the NetSIO/FujiNet functionality.

## Prerequisites

Before starting, you need to have the following tools and libraries installed:

1. **Xcode Command Line Tools**
   ```bash
   xcode-select --install
   ```

2. **Homebrew** (package manager for macOS)
   ```bash
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

3. **Required Libraries**
   ```bash
   brew install sdl autoconf automake
   ```

## Building the Emulator

### Step 1: Clone the Repository

```bash
git clone https://github.com/atari800/atari800.git
cd atari800
```

Or if you're building the NetSIO/FujiNet branch:

```bash
git clone https://github.com/atari800/atari800.git
cd atari800
git checkout netsio
```

### Step 2: Generate Build System

```bash
./autogen.sh
```

### Step 3: Configure

For a basic build with SDL:

```bash
./configure --with-video=sdl --with-sound=sdl
```

For a build with OpenGL support (recommended for better performance):

```bash
./configure --with-video=sdl --with-sound=sdl --enable-opengl-by-default
```

See `./configure --help` for more options.

### Step 4: Fix SDL Header Path (Required for NetSIO/FujiNet Branch)

If you're building the NetSIO/FujiNet branch, you need to create a symbolic link to help the compiler find the SDL headers:

```bash
mkdir -p src/SDL
ln -s /opt/homebrew/include/SDL/SDL.h src/SDL/
```

### Step 5: Build

```bash
make
```

### Step 6: Install (Optional)

```bash
sudo make install
```

This will install the emulator system-wide. If you prefer not to install it system-wide, you can simply run it from the build directory.

## Running the Emulator

After building, you can run the emulator directly from the build directory:

```bash
./src/atari800
```

Or, if you installed it system-wide:

```bash
atari800
```

## Common Issues and Solutions

### Missing SDL Headers

If you encounter an error like:

```
fatal error: 'SDL/SDL.h' file not found
```

Create a symbolic link as described in Step 4.

### Non-void Functions with Missing Return Values

If you're working with the NetSIO/FujiNet branch and encounter errors like:

```
error: non-void function 'Command_Frame' should return a value
```

You need to fix the return values in the affected functions in `src/sio.c`:

1. Locate the `return;` statements in the `Command_Frame` function
2. Replace them with `return 'E';` to properly return an error value
3. In the `WriteSectorBack` function, replace `return 0;` with `return 'E';` for the error condition

### Compiler Warnings

Some warnings about missing newlines at the end of header files (like `fujinet.h` and `netsio.h`) may appear during compilation. These don't affect functionality but can be fixed for cleaner code.

## Using NetSIO/FujiNet

The NetSIO/FujiNet functionality allows the Atari800 emulator to communicate with a FujiNet device over the network, providing virtual peripherals.

### Configuration

1. By default, the emulator listens for NetSIO traffic on UDP port 9997.
2. The FujiNet device should be configured to communicate with the IP address of the machine running Atari800.

### Command-Line Options

When running the emulator with FujiNet support, you may need to specify additional command-line options to configure the NetSIO functionality. Check the emulator's help (`--help`) for available options.

## Troubleshooting

### SDL Sound Issues

If you experience sound problems, try different sound drivers or buffer sizes:

```bash
./src/atari800 -sound-fragsize 512 -sound
```

### Performance Issues

If the emulator runs slowly, try enabling the OpenGL renderer (if available):

```bash
./src/atari800 -video-opengl
```

Or, when building, configure with OpenGL support:

```bash
./configure --with-video=sdl --with-sound=sdl --enable-opengl-by-default
```

### Network Connectivity

If the NetSIO/FujiNet functionality isn't working:

1. Check your firewall settings to ensure UDP port 9997 is open
2. Verify the network connectivity between the emulator and the FujiNet device
3. Ensure the FujiNet device is properly configured to communicate with the emulator
