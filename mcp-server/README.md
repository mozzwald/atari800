# Atari 800 MCP Server

MCP server for controlling the Atari 800 emulator with AI interface.

## Installation

1. Build the emulator with AI interface enabled (see main README)

2. Add to your Claude Code MCP settings (`~/.claude/settings.json` or project `.claude/settings.json`):

```json
{
  "mcpServers": {
    "atari800": {
      "command": "node",
      "args": ["/path/to/atari800-ai/mcp-server/index.js"]
    }
  }
}
```

The server automatically finds the emulator binary relative to its location (`../src/atari800`).

To use a different emulator path, set the `ATARI800_PATH` environment variable:

```json
{
  "mcpServers": {
    "atari800": {
      "command": "node",
      "args": ["/path/to/mcp-server/index.js"],
      "env": {
        "ATARI800_PATH": "/custom/path/to/atari800"
      }
    }
  }
}
```

## Available Tools

- `atari_build` - Build the game using make
- `atari_run` - Start emulator with a program
- `atari_stop` - Stop emulator
- `atari_status` - Check if emulator is running
- `atari_run_frames` - Run for N frames
- `atari_screen` - Get ASCII screen (40x24)
- `atari_screenshot` - Save PNG screenshot
- `atari_joystick` - Control joystick (direction + fire)
- `atari_send_key` - Press keyboard key
- `atari_peek` - Read memory
- `atari_poke` - Write memory
- `atari_cpu` - Get CPU registers
- `atari_antic` - Get ANTIC (display) state
- `atari_gtia` - Get GTIA (graphics) state
- `atari_pokey` - Get POKEY (sound/keyboard) state
- `atari_pia` - Get PIA (I/O) state
- `atari_breakpoint` - Set/clear breakpoint
- `atari_step` - Single-step CPU
- `atari_save_state` - Save emulator state
- `atari_load_state` - Load emulator state

## Example Usage

1. Start emulator: `atari_run` with game_path="/path/to/game.xex"
2. Run some frames: `atari_run_frames` with frames=60
3. See screen: `atari_screen`
4. Press fire: `atari_joystick` with direction="center", fire=true
5. Take screenshot: `atari_screenshot`
