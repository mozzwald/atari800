# Atari800-AI: Atari 800 Emulator with AI Interface

A fork of the [atari800](https://github.com/atari800/atari800) emulator with a built-in AI/automation interface via Unix socket.

## What's New

This fork adds a **real-time AI interface** that allows external programs to:
- Control the emulator programmatically (joystick, keyboard, console keys)
- Read screen output (ASCII text or raw data)
- Inspect hardware registers (CPU, ANTIC, GTIA, POKEY, PIA)
- Read/write memory, set breakpoints
- Save/load emulator state
- Run frame-by-frame for precise control

This enables **autonomous game testing**, **AI training**, **automated debugging**, and **tool-assisted development**.

## Building

**Important:** Must use SDL 1.2, not SDL 2 (SDL2 has broken keyboard-to-joystick trigger mapping).

```bash
# macOS with Homebrew
brew install sdl12-compat autoconf automake

# Configure with SDL 1.2
./configure --with-video=sdl --with-sound=sdl \
    SDL_CONFIG=/opt/homebrew/bin/sdl-config \
    CFLAGS="-O2 -g"

# Build
make -j4
```

Or use the included build script:
```bash
./build_ai.sh
```

## Running with AI Interface

```bash
./src/atari800 -ai -xl -run /path/to/game.xex
```

The `-ai` flag enables the AI interface, which creates a Unix socket at `/tmp/atari800_ai.sock`.

## Socket Protocol

The AI interface uses a simple length-prefixed JSON protocol:

```
Client -> Server: <json_length>\n<json_command>
Server -> Client: <json_length>\n<json_response>
```

### Example (Python)

```python
import socket
import json

def send_command(cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect('/tmp/atari800_ai.sock')
    data = json.dumps(cmd)
    s.send(f'{len(data)}\n{data}'.encode())

    # Read response
    resp = b''
    while b'\n' not in resp:
        resp += s.recv(1024)
    length = int(resp.split(b'\n')[0])
    body = resp.split(b'\n', 1)[1]
    while len(body) < length:
        body += s.recv(1024)
    s.close()
    return json.loads(body)

# Test connection
print(send_command({'cmd': 'ping'}))

# Set joystick
send_command({'cmd': 'joystick', 'port': 0, 'direction': 'up', 'fire': True})

# Run 60 frames (1 second)
send_command({'cmd': 'run', 'frames': 60})

# Get screen as ASCII
resp = send_command({'cmd': 'screen_ascii'})
for line in resp['data']:
    print(line)
```

## API Reference

### Control Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `ping` | - | Test connection, returns `{status: "ok"}` |
| `load` | `path` | Load a program file (.xex, .atr, etc.) |
| `run` | `frames` | Run emulator for N frames (1 frame = 1/60 sec) |
| `step` | - | Execute single CPU instruction |
| `pause` | - | Pause emulation |
| `reset` | - | Reset the Atari |

### Input Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `joystick` | `port`, `direction`, `fire` | Set joystick state |
| `key` | `keycode` | Press a key |
| `key_release` | `keycode` | Release a key |
| `paddle` | `port`, `value` | Set paddle position (0-227) |
| `consol` | `start`, `select`, `option` | Set console keys |

**Joystick Directions:** `center`, `up`, `down`, `left`, `right`, `ul`, `ur`, `ll`, `lr`

### Screen Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `screenshot` | `path` | Save screenshot as PNG |
| `screen_ascii` | - | Get 40x24 ASCII representation |
| `screen_raw` | - | Get raw screen memory |

### Memory Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `peek` | `addr`, `len` | Read memory bytes |
| `poke` | `addr`, `value` | Write memory byte |
| `dump` | `addr`, `len`, `path` | Dump memory to file |

### CPU/Chip State Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `cpu` | - | Get CPU registers (A, X, Y, PC, SP, flags) |
| `cpu_set` | `reg`, `value` | Set CPU register |
| `antic` | - | Get ANTIC chip state |
| `gtia` | - | Get GTIA chip state (colors, triggers, PMG) |
| `pokey` | - | Get POKEY chip state (audio, keyboard) |
| `pia` | - | Get PIA chip state (ports, interrupts) |
| `breakpoint` | `addr`, `enabled` | Set/clear breakpoint |

### Disk Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `disk_insert` | `drive`, `path` | Insert disk image |
| `disk_eject` | `drive` | Eject disk |
| `disk_status` | - | Get disk drive status |

### State Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `save_state` | `path` | Save emulator state |
| `load_state` | `path` | Load emulator state |

### Debug Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `debug_enable` | `port` | Enable debug port at $D7xx |
| `debug_read` | - | Read data from debug port |

## Files Modified

From the original atari800:

- **`src/ai_interface.c`** - NEW: AI socket server implementation (~720 lines)
- **`src/ai_interface.h`** - NEW: API header with documentation
- **`src/atari.c`** - Modified: Added `AI_Initialise()`, `AI_Frame()`, `AI_ApplyInput()` hooks
- **`src/memory.c`** - Modified: Debug port hook at $D7xx range
- **`configure.ac`** - Modified: Added `--enable-ai` option
- **`build_ai.sh`** - NEW: Build script with correct SDL 1.2 flags

## Technical Notes

### Why AI_ApplyInput() Exists

The Atari's `INPUT_Frame()` reads hardware state every frame and overwrites `GTIA_TRIG[]`. AI-set trigger values were being lost immediately.

**Solution:** `AI_ApplyInput()` is called AFTER `INPUT_Frame()` to re-apply AI overrides:

```c
// In atari.c Atari800_Frame():
INPUT_Frame();     // Reads hardware -> overwrites GTIA_TRIG
AI_ApplyInput();   // Re-applies AI overrides
GTIA_Frame();      // Game reads correct values
```

### Joystick Direction Encoding

| Direction | PIA Value | Binary |
|-----------|-----------|--------|
| center | 15 | 1111 |
| up | 14 | 1110 |
| down | 13 | 1101 |
| left | 11 | 1011 |
| right | 7 | 0111 |

### Trigger Values

- `1` = Not pressed (default)
- `0` = Pressed (active low)

## License

This fork maintains the same **GPLv2** license as the original atari800 emulator.

See [COPYING](COPYING) for the full license text.

## Credits

- **Original atari800 emulator:** https://github.com/atari800/atari800
- **AI Interface additions:** Benj Edwards with Claude (Anthropic)

## See Also

- [Original atari800 documentation](DOC/)
- [Atari 800 technical reference](https://www.atariarchives.org/mapping/)
