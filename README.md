# Atari800-AI: Atari 800 Emulator with AI Interface

A fork of the [atari800](https://github.com/atari800/atari800) emulator with a built-in AI/automation interface via Unix socket.

## What's New

This fork adds a **real-time AI/MCP interface** that allows external programs and agents to:
- Control the emulator programmatically (joystick, keyboard, console keys)
- Read screen output (ASCII text or raw data)
- Inspect hardware registers (CPU, ANTIC, GTIA, POKEY, PIA)
- Read/write memory
- Save/load emulator state
- Run frame-by-frame for precise control
- Manage headless sessions through `mcp-server/index.js`
- Boot native disk images and FujiNet-PC/NetSIO workflows through managed MCP tools

Useful for automated testing, debugging tools, and programmatic emulator control.

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

Recommended AI/debug configure flags for manual builds:

```bash
./configure --with-video=sdl --with-sound=sdl \
    --enable-ai-interface --enable-netsio \
    --enable-monitorbreak --enable-monitorbreakpoints \
    --enable-monitortrace --enable-monitorprofile
```

`--enable-netsio` is required for the supported FujiNet-PC workflows. `build_ai.sh` uses SDL 1.2 and enables the AI interface plus NetSIO for the MCP-targeted build.

## Running with AI Interface

```bash
./src/atari800 -ai -xl -run /path/to/game.xex
```

The `-ai` flag enables the AI interface, which creates a Unix socket at `/tmp/atari800_ai.sock`.

For agent workflows, prefer the MCP server:

```bash
cd mcp-server
npm install
npm start
```

The MCP server starts per-session Atari800 processes, uses headless Xvfb by default on Linux, owns runtime/artifact directories, manages native disk image copies, and integrates with real FujiNet-PC sidecars for NetSIO tests. See `README.AI.md`, `AGENT_CONTRACT.md`, and `mcp-server/README.md`.

Frame streaming is only active when both are true:
- Build was configured with `--enable-ai-interface`
- Emulator is launched with `-ai`

## Socket Protocol

The AI interface uses a simple length-prefixed JSON protocol:

```
Client -> Server: <json_length>\n<json_command>
Server -> Client: <json_length>\n<json_response>
```

## Video Frame Streaming Interface

In addition to JSON control on `/tmp/atari800_ai.sock`, the emulator exposes two
binary frame sockets when AI is enabled:

- Push stream: `/tmp/atari800-fb-push.sock`
- Pull stream: `/tmp/atari800-fb-pull.sock`

Frames are streamed as final rendered pixels in **RGB565**.

### Frame Record (`A8FB`)

Each frame is:
- 36-byte header (little-endian)
- RGB565 payload (`stride * height` bytes)

Header layout:
- `magic[4]` = `"A8FB"`
- `version u16` = `1`
- `flags u16` (`bit0=rgb565`, `bit1=timestamp`)
- `width u16`
- `height u16`
- `stride u32`
- `frame_no u32`
- `payload_len u32`
- `timestamp_us u64`
- `crc32 u32` (CRC of payload)

### Pull Request (`A8RQ`)

Pull socket requests are fixed 16 bytes (little-endian):
- `magic[4]` = `"A8RQ"`
- `version u16` = `1`
- `command u16`
- `arg0 u32`
- `arg1 u32` (reserved)

Commands:
- `1` = `GET_LATEST`
- `2` = `RUN_FRAMES_AND_GET` (`arg0` = frames to run)

Responses:
- Frame: `A8FB` + payload
- Error: `A8ER`

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
| `hello` / `capabilities` | - | Return protocol version, build flags, limits, sockets, commands, and command classes |
| `load` | `path` | Load an executable-style program through `BINLOAD_Loader()` |
| `run` | `frames` | Run emulator for N frames (1 frame = 1/60 sec) |
| `frame_step` | `frames` | Run N frame-loop ticks, then pause |
| `step` | `instructions` | Deprecated compatibility alias for frame-loop stepping; not CPU instruction stepping |
| `pause` | - | Pause emulation |
| `reset` | - | Reset the Atari |

### Input Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `joystick` | `port`, `direction`, `fire` | Set joystick state |
| `key` | `code`, `shift` | Press an AKEY code |
| `key_release` | - | Release all AI key state |
| `paddle` | `port`, `value` | Set paddle position (0-255) |
| `consol` | `start`, `select`, `option` | Set console keys; active-low behavior needs runtime validation |

**Joystick Directions:** `center`, `up`, `down`, `left`, `right`, `ul`, `ur`, `ll`, `lr`

### Screen Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `screenshot` | `path` | Save screenshot as PNG |
| `screen_ascii` | - | Get 40x24 ASCII representation |
| `screen_raw` | - | Get rendered framebuffer bytes as base64, not Atari screen RAM |

### Video Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `video.enable_push` | - | Enable push streaming socket |
| `video.disable_push` | - | Disable push streaming socket |
| `video.enable_pull` | - | Enable pull request socket |
| `video.disable_pull` | - | Disable pull request socket |
| `video.push.set_fps_cap` | `value` | Set push max FPS (`0` = uncapped) |
| `video.push.set_frameskip` | `n` | Send 1 frame every `n` emulator frames |
| `video.push.enable_change_triggered` | `enabled` | Only push when frame CRC changes |
| `video.status` | - | Return socket paths and current video stream status |

### Memory Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `peek` | `addr`, `len` | Read memory bytes; `len` is capped at 256 |
| `poke` | `addr`, `data` | Write byte array directly to `MEMORY_mem` |
| `dump` | `start`, `end`, `path` | Dump memory to file |

### CPU/Chip State Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `cpu` | - | Get CPU registers (A, X, Y, PC, SP, flags) |
| `cpu_set` | `pc`, `a`, `x`, `y`, `sp` | Set selected CPU registers |
| `antic` | - | Get ANTIC chip state |
| `gtia` | - | Get GTIA chip state (colors, triggers, PMG) |
| `pokey` | - | Get POKEY chip state (audio, keyboard) |
| `pia` | - | Get PIA chip state (ports, interrupts) |

### Disk Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `disk.insert` | `drive`, `path`, `read_only` | Mount a native disk image in D1-D8 |
| `disk.eject` | `drive` | Dismount a native disk drive |
| `disk.status` | `drive` | Report SIO drive state and mounted image metadata |

For MCP users, prefer `atari_disk_insert`; it copies source images into a managed session workspace and mounts the copy read-only by default.

### Packaging

Create a standalone MCP runtime bundle outside the normal Atari800 Makefile/autotools flow:

```bash
python3 tools/package_mcp_bundle.py
```

The output under `dist/` contains `bin/atari800`, `mcp-server/` with production `node_modules`, `start-mcp.sh`, `skills/atari800-mcp`, documentation, and `manifest.json`, with a platform-named `.tar.gz`. Configure MCP clients to run the bundle `start-mcp.sh`; it sets `ATARI800_PATH` to the bundled emulator.

### State Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `save_state` | `path` | Save emulator state |
| `load_state` | `path` | Load emulator state |

### Debug Commands

| Command | Parameters | Description |
|---------|------------|-------------|
| `debug_enable` | `addr` | Enable debug capture address, default `$D7FF` |
| `debug_read` | - | Read data from debug port |
| `debugger.status` | - | Report debugger capabilities, stop state, and CPU state |
| `debugger.show_state` | - | Report debugger stop state and CPU state |
| `debugger.history` | - | Return bounded monitor-formatted recent instruction history |
| `debugger.jumps` | - | Return bounded recent JMP/JSR history |
| `debugger.stack` | `count` | Return stack bytes above SP |
| `debugger.disassemble` | `addr`, `count` | Return monitor-formatted disassembly |
| `debugger.disassemble_loop` | `addr` | Return loop disassembly when detectable |
| `debugger.dlist` | `addr`, `count` | Return bounded ANTIC display list entries |
| `debugger.search_memory` | `start`, `end`, `pattern` | Search memory for a byte pattern |
| `debugger.search_string` | `start`, `end`, `text` | Search memory for an ATASCII string |
| `debugger.search_screencode_string` | `start`, `end`, `text` | Search memory for an ANTIC screen-code string |
| `debugger.labels` | `limit` | List monitor labels when `MONITOR_HINTS` is available |
| `debugger.step_instruction` | `instructions` | True CPU instruction stepping when `MONITOR_BREAK` is available |
| `debugger.continue` | - | Continue emulation from a debugger stop |
| `breakpoint.pc` | `addr`, `enabled` | Enable or disable simple AI-owned break-on-PC |
| `breakpoint.brk` | `enabled` | Enable or disable simple break-on-BRK |
| `breakpoint.status` | - | Report AI-owned breakpoint state |
| `breakpoint.clear` | `type` | Clear simple and/or rich breakpoints |
| `breakpoint.list` | - | List rich monitor breakpoint table when `MONITOR_BREAKPOINTS` is available |
| `breakpoint.add` | `condition` or structured fields | Add one rich monitor breakpoint table entry |
| `breakpoint.delete` | `slot` | Delete a rich monitor breakpoint slot |
| `breakpoint.enable` | `slot` | Enable a rich monitor breakpoint slot |
| `breakpoint.disable` | `slot` | Disable a rich monitor breakpoint slot |

Rich breakpoint table commands return `CAPABILITY_UNAVAILABLE` when `MONITOR_BREAKPOINTS` is not compiled in. Unsafe interactive monitor commands are not exposed through the AI socket.

### Video Usage Examples

Get streaming status through AI JSON socket:

```python
resp = send_command({'cmd': 'video.status'})
print(resp)
```

Pull one frame (`GET_LATEST`) directly from pull socket:

```python
import socket, struct

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/tmp/atari800-fb-pull.sock')
s.sendall(b'A8RQ' + struct.pack('<HHII', 1, 1, 0, 0))  # version=1, command=GET_LATEST
header = s.recv(36)
magic = header[:4]
ver, flags, w, h, stride, frame_no, payload_len = struct.unpack('<HHHHIII', header[4:24])
payload = b''
while len(payload) < payload_len:
    payload += s.recv(payload_len - len(payload))
s.close()
print(magic, w, h, frame_no, len(payload))
```

Subscribe to push stream and read one frame:

```python
import socket, struct

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/tmp/atari800-fb-push.sock')
header = s.recv(36)
ver, flags, w, h, stride, frame_no, payload_len = struct.unpack('<HHHHIII', header[4:24])
payload = b''
while len(payload) < payload_len:
    payload += s.recv(payload_len - len(payload))
s.close()
print(w, h, frame_no, len(payload))
```

## Files Modified

From the original atari800:

- **`src/ai_interface.c`** - NEW: AI socket server + frame streaming implementation
- **`src/ai_interface.h`** - NEW: API header with documentation
- **`src/atari.c`** - Modified: Added `AI_Initialise()`, `AI_Frame()`, `AI_ApplyInput()` hooks
- **`src/sdl/video_sw.c`** - Modified: Added final-frame tap for RGB565 frame streaming
- **`src/memory.c`** - Modified: Debug port hook at $D7xx range
- **`configure.ac`** - Modified: Added `--enable-ai-interface` option
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
- **AI Interface additions:** Benj Edwards using Claude Code

## See Also

- [Original atari800 documentation](DOC/)
- [Atari 800 technical reference](https://www.atariarchives.org/mapping/)
