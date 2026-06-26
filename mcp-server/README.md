# Atari 800 MCP Server

MCP server for controlling an MCP-owned Atari800 emulator session through the AI socket.

## Installation

Build the emulator with the AI interface enabled:

```sh
./build_ai.sh
```

Add the server to MCP client settings:

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

The server uses `../src/atari800` relative to `mcp-server/index.js` by default. Override it with `ATARI800_PATH`.

## Runtime Model

`atari_start` creates one managed session under `/tmp/atari800-mcp` by default. The session has its own runtime directory, artifact directory, command socket, video push socket, and video pull socket.

`display_mode` can be `auto`, `headless`, or `visible`. `auto` currently behaves like `headless`; on Linux this launches an MCP-owned `Xvfb` process directly and cleans it up with the session. Use `atari_preflight` to check whether Xvfb and a native display are available.

`atari_stop` stops only the tracked MCP-owned emulator process. It does not use global `pkill` cleanup.

## Available Tools

| Tool | Description |
|------|-------------|
| `atari_preflight` | Report emulator, display, Xvfb, runtime, and host dependency status. |
| `atari_start` | Start an MCP-owned emulator session. |
| `atari_stop` | Stop the tracked session; accepts `force` and `cleanup_runtime_dir`. |
| `atari_status` | Report session state, launch details, sockets, and bounded logs. |
| `atari_logs` | Read bounded emulator/Xvfb startup logs. |
| `atari_load` | Load an executable-style program through the Atari800 BIN loader. |
| `atari_run` | Run for N frame-loop frames. |
| `atari_frame_step` | Run N frame-loop ticks, then pause. |
| `atari_pause` | Pause emulation. |
| `atari_screen` | Get the approximate 40x24 ASCII screen. |
| `atari_screen_raw` | Get rendered framebuffer bytes as base64 data. |
| `atari_screenshot` | Save a screenshot to the managed artifact directory or an explicit safe path. |
| `atari_joystick` | Set joystick direction/fire/port. |
| `atari_key` | Press a supported key name through AKEY mapping. |
| `atari_key_release` | Release all AI key state. |
| `atari_paddle` | Set paddle position. |
| `atari_consol` | Set console key booleans. |
| `atari_peek` | Read memory bytes. |
| `atari_poke` | Write memory bytes directly. |
| `atari_dump_memory` | Dump memory to an artifact-safe path. |
| `atari_cpu` | Get CPU state. |
| `atari_cpu_set` | Unsafe: set selected CPU registers. |
| `atari_antic` | Get ANTIC state. |
| `atari_gtia` | Get GTIA state. |
| `atari_pokey` | Get POKEY state. |
| `atari_pia` | Get PIA state. |
| `atari_reset` | Cold reset the Atari. |
| `atari_debug_enable` | Enable debug output capture at an address. |
| `atari_debug_read` | Read and clear debug output. |
| `atari_debugger_status` | Report debugger capabilities and stop state. |
| `atari_show_state` | Report CPU and debugger stop state. |
| `atari_history` | Show recent executed instruction history. |
| `atari_jumps` | Show recent JMP/JSR history. |
| `atari_stack` | Show stack bytes above SP. |
| `atari_disassemble` | Disassemble memory using monitor formatting. |
| `atari_disassemble_loop` | Disassemble a detected loop around an address. |
| `atari_display_list` | Show ANTIC display list entries. |
| `atari_memory_search` | Search memory for a byte pattern. |
| `atari_string_search` | Search memory for an ATASCII string. |
| `atari_screencode_string_search` | Search memory for an ANTIC screen-code string. |
| `atari_labels` | List monitor labels when available. |
| `atari_step_instruction` | True CPU instruction stepping when available. |
| `atari_debugger_continue` | Continue emulation from debugger stop. |
| `atari_break_on_pc` | Enable or disable simple break-on-PC. |
| `atari_break_on_brk` | Enable or disable break-on-BRK. |
| `atari_breakpoint_status` | Report simple AI breakpoint state. |
| `atari_breakpoint_list` | List rich monitor breakpoint table entries when available. |
| `atari_breakpoint_add` | Add a rich monitor breakpoint entry when available. |
| `atari_breakpoint_delete` | Delete a rich monitor breakpoint slot when available. |
| `atari_breakpoint_enable` | Enable a rich monitor breakpoint slot when available. |
| `atari_breakpoint_disable` | Disable a rich monitor breakpoint slot when available. |
| `atari_breakpoint_clear` | Clear AI breakpoints. |
| `atari_save_state` | Save emulator state to an artifact-safe path. |
| `atari_load_state` | Load emulator state from a caller-provided path. |
| `atari_video_status` | Get video socket and stream state. |
| `atari_video_enable_push` | Enable video push streaming. |
| `atari_video_disable_push` | Disable video push streaming. |
| `atari_video_enable_pull` | Enable video pull requests. |
| `atari_video_disable_pull` | Disable video pull requests. |
| `atari_video_set_fps_cap` | Set push stream max FPS. |
| `atari_video_set_frameskip` | Set push stream frame skip. |
| `atari_video_set_change_triggered` | Push only frames whose CRC changes. |

## Example Workflow

1. Start emulator: `atari_start` with optional `program`.
2. Check session and capabilities: `atari_status`.
3. Run frames: `atari_run` with `frames`.
4. Inspect output: `atari_screen`, `atari_cpu`, `atari_peek`.
5. Stop session: `atari_stop`.

MCP wrappers are intentionally narrower than the C socket API. See `AGENT_CONTRACT.md` for the full C command inventory and known gaps.
