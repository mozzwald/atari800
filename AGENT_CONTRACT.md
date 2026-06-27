# Atari800 AI/MCP Agent Contract

This document is the Phase 0 source inventory and current agent-facing contract. It records what exists now, including mismatches, so later phases do not build on stale README claims.

## Current Startup Contract

Build the MCP-targeted emulator with:

```sh
./build_ai.sh
```

That script configures SDL 1.2 with `--enable-ai-interface` and `--enable-netsio`.

Current unmanaged emulator startup:

```sh
./src/atari800 -netsio -ai -xl -run /path/to/program.xex
```

The default command socket is `/tmp/atari800_ai.sock`. The default video sockets are `/tmp/atari800-fb-push.sock` and `/tmp/atari800-fb-pull.sock`. MCP-managed sessions use per-session socket paths via `-ai-socket`, `-ai-video-push-socket`, and `-ai-video-pull-socket`.

MCP `atari_start` supports `display_mode=auto|headless|visible`. `auto` behaves as headless unless visible mode is requested. Linux headless mode starts an MCP-owned `Xvfb` process, sets `DISPLAY`, defaults sound off, and adds `-no-video-accel`. Visible mode uses the caller's existing native display and fails clearly if no display is available.

## Capability Discovery

Agents should call:

```json
{"cmd":"hello"}
```

or:

```json
{"cmd":"capabilities"}
```

The response reports protocol version, build flags, response and command limits, socket paths, artifact path policy, command names, and command classes.

## Protocol Basics

The command socket uses length-prefixed JSON:

```text
<json_length>\n<json_command>
```

Responses use the same framing. Successful responses include `"status":"ok"`. Errors use:

```json
{"status":"error","code":"BAD_ARGUMENT","message":"...","details":{"field":"..."}}
```

Known error codes include `BAD_JSON`, `UNKNOWN_COMMAND`, `BAD_ARGUMENT`, `MISSING_FIELD`, `UNSUPPORTED`, `CAPABILITY_UNAVAILABLE`, `TIMEOUT`, `NOT_RUNNING`, `NOT_PAUSED`, `BUSY`, `PATH_DENIED`, `IO_ERROR`, `EMULATOR_EXITED`, and `INTERNAL_ERROR`.

## Timeout Behavior

`run`, `step`, and video pull frame waits use the AI command timeout. The CLI option is:

```sh
-ai-command-timeout-ms <milliseconds>
```

The default is 30000 ms. A value below zero is normalized to zero. Runtime testing of breakpoint, lifecycle, sync, FujiNet, and NetSIO waits is deferred to later phases.

## Path Policy

The default artifact directory is `/tmp/atari800_ai_artifacts`. It can be changed with:

```sh
-ai-artifact-dir <path>
```

Output paths for `screenshot`, `dump`, and `save_state` must be absolute, must not contain `..` path components, and must resolve under the artifact directory. `-ai-unsafe-paths` disables that protection and should not be used by MCP by default.

Read paths for `load` and `load_state` are caller-provided and are not yet managed. MCP now creates managed runtime and artifact directories for emulator sessions. Managed disk workspace and FujiNet sidecar directories are planned for later phases.

## Native Disk vs FujiNet Mounts

Native Atari800 disk commands are not implemented yet. FujiNet-PC sidecar management and deterministic `fnconfig.ini` mounts are also not implemented yet. Agents should not drive the FujiNet CONFIG UI as the primary workflow; managed FujiNet configuration is planned for later phases.

## AI Command Inventory

Legend: `Y` means present, `N` means absent, `bad` means documented but currently wrong or misleading. `Class` is the advertised command class.

<!-- BEGIN AI_COMMAND_INVENTORY -->
| Command | C | Python | MCP | README.md | ai_interface.h | MCP README | Class | Current notes |
|---------|---|--------|-----|-----------|----------------|------------|-------|---------------|
| `ping` | Y | N | internal | Y | Y | N | read-only | Health check. |
| `hello` | Y | N | N | N | Y | N | read-only | Protocol/capability discovery. |
| `capabilities` | Y | N | N | N | Y | N | read-only | Alias for `hello`. |
| `load` | Y | Y | Y | bad | Y | Y | mutating | Calls `BINLOAD_Loader(path)`, not disk insertion. |
| `run` | Y | Y | Y | Y | Y | Y | mutating | Runs frame-loop frames. |
| `frame_step` | Y | Y | Y | Y | Y | Y | mutating | Runs N frame-loop ticks, then pauses. |
| `step` | Y | Y | N | deprecated | deprecated | N | mutating | Deprecated compatibility alias for frame-loop stepping, not true CPU instruction stepping. |
| `pause` | Y | Y | Y | Y | Y | Y | mutating | Pauses emulation. |
| `reset` | Y | Y | Y | Y | Y | N | mutating | Cold reset. |
| `key` | Y | Y | Y | Y | Y | Y | mutating | C uses `code`; MCP accepts key names. |
| `key.down` | Y | N | Y | N | Y | Y | mutating | Alias for holding a key by AKEY code. |
| `key.up` | Y | N | Y | N | Y | Y | mutating | Alias for releasing all AI key state. |
| `key_release` | Y | Y | Y | Y | Y | Y | mutating | Releases all AI key state; no keycode argument. |
| `joystick` | Y | Y | Y | Y | Y | Y | mutating | Overrides stick and trigger state. |
| `paddle` | Y | Y | Y | Y | Y | Y | mutating | Value accepted as 0..255. |
| `consol` | Y | Y | Y | needs-test | Y | N | mutating | C booleans are active-low: false means pressed. MCP exposes natural pressed booleans. |
| `input.status` | Y | N | Y | N | Y | Y | read-only | Reports key, console active-low state, joystick overrides, trigger overrides, and paddles. |
| `screenshot` | Y | Y | Y | Y | Y | Y | mutating | Writes under artifact dir unless unsafe paths are enabled. |
| `screen_ascii` | Y | Y | Y | Y | Y | Y | read-only | Approximate 40x24 rendered screen text. |
| `screen.text` | Y | N | Y | N | Y | Y | read-only | Display-list-aware simple text memory read with confidence and unsupported reasons. |
| `screen_raw` | Y | Y | Y | bad | Y | Y | read-only | Base64 rendered framebuffer bytes, not Atari screen RAM. |
| `framebuffer.raw` | Y | N | Y | N | Y | Y | read-only | Explicit alias for rendered framebuffer base64 bytes. |
| `video.enable_push` | Y | N | Y | Y | N | Y | mutating | Enables push frame socket. |
| `video.disable_push` | Y | N | Y | Y | N | Y | mutating | Disables push frame socket. |
| `video.enable_pull` | Y | N | Y | Y | N | Y | mutating | Enables pull frame socket. |
| `video.disable_pull` | Y | N | Y | Y | N | Y | mutating | Disables pull frame socket. |
| `video.push.set_fps_cap` | Y | N | Y | Y | N | Y | mutating | Sets push FPS cap. |
| `video.push.set_frameskip` | Y | N | Y | Y | N | Y | mutating | Sends one frame every N frames. |
| `video.push.enable_change_triggered` | Y | N | Y | Y | N | Y | mutating | Pushes only changed frames when enabled. |
| `video.status` | Y | N | Y | Y | N | Y | read-only | Reports effective video socket paths and stream state. |
| `disk.insert` | Y | N | Y | N | Y | Y | mutating | Mounts a native disk image in D1..D8; MCP passes managed workspace copies. |
| `disk.eject` | Y | N | Y | N | Y | Y | mutating | Dismounts a native disk drive. |
| `disk.status` | Y | N | Y | N | Y | Y | read-only | Reports native SIO drive state, image path, sector size/count, and last disk op. |
| `peek` | Y | Y | Y | Y | Y | Y | read-only | `len` is capped at 256. |
| `poke` | Y | Y | Y | bad | Y | Y | unsafe | Uses `data:[byte,...]`; direct `MEMORY_mem` mutation. |
| `dump` | Y | Y | Y | bad | Y | Y | mutating | Uses `start`, `end`, and `path`; output path is protected. |
| `cpu` | Y | Y | Y | Y | Y | Y | read-only | CPU status. |
| `cpu_set` | Y | Y | Y | bad | Y | Y | unsafe | Sets selected CPU registers. |
| `antic` | Y | Y | Y | Y | Y | Y | read-only | ANTIC status. |
| `gtia` | Y | Y | Y | Y | Y | Y | read-only | GTIA status. |
| `pokey` | Y | Y | Y | Y | Y | Y | read-only | POKEY status. |
| `pia` | Y | Y | Y | Y | Y | Y | read-only | PIA status. |
| `debug_enable` | Y | Y | Y | Y | Y | Y | mutating | Uses `addr`, not `port`. |
| `debug_read` | Y | Y | Y | Y | Y | Y | read-only | Reads and clears debug output buffer. |
| `debugger.status` | Y | N | Y | N | Y | Y | read-only | Reports debugger capabilities and stop state. |
| `debugger.show_state` | Y | N | Y | N | Y | Y | read-only | Reports CPU and debugger stop state. |
| `debugger.history` | Y | N | Y | N | N | Y | read-only | Bounded monitor-formatted recent instruction history. |
| `debugger.jumps` | Y | N | Y | N | N | Y | read-only | Bounded recent JMP/JSR history. |
| `debugger.stack` | Y | N | Y | N | N | Y | read-only | Reads stack bytes above SP. |
| `debugger.disassemble` | Y | N | Y | N | N | Y | read-only | Monitor-formatted bounded disassembly. |
| `debugger.disassemble_loop` | Y | N | Y | N | N | Y | read-only | Monitor-aligned loop disassembly when detectable. |
| `debugger.dlist` | Y | N | Y | N | N | Y | read-only | Bounded ANTIC display list view. |
| `debugger.search_memory` | Y | N | Y | N | N | Y | read-only | Searches memory for byte pattern; result count capped. |
| `debugger.search_string` | Y | N | Y | N | N | Y | read-only | Searches memory for ATASCII string. |
| `debugger.search_screencode_string` | Y | N | Y | N | N | Y | read-only | Searches memory for ANTIC screen-code string. |
| `debugger.labels` | Y | N | Y | N | N | Y | read-only | Lists labels when `MONITOR_HINTS` is compiled; otherwise capability error. |
| `debugger.step_instruction` | Y | N | Y | N | Y | Y | mutating | True CPU instruction stepping when `MONITOR_BREAK` is compiled. |
| `debugger.continue` | Y | N | Y | N | Y | Y | mutating | Continues emulation from debugger stop. |
| `breakpoint.pc` | Y | N | Y | N | Y | Y | mutating | Simple AI-owned break-on-PC. |
| `breakpoint.brk` | Y | N | Y | N | Y | Y | mutating | Simple AI-owned break-on-BRK. |
| `breakpoint.status` | Y | N | Y | N | Y | Y | read-only | Reports AI-owned breakpoint state. |
| `breakpoint.clear` | Y | N | Y | N | Y | Y | mutating | Clears AI-owned breakpoints. |
| `breakpoint.list` | Y | N | Y | N | N | Y | read-only | Lists monitor breakpoint table when `MONITOR_BREAKPOINTS` is compiled. |
| `breakpoint.add` | Y | N | Y | N | N | Y | mutating | Adds one rich monitor breakpoint table condition when available. |
| `breakpoint.delete` | Y | N | Y | N | N | Y | mutating | Deletes a rich monitor breakpoint slot when available. |
| `breakpoint.enable` | Y | N | Y | N | N | Y | mutating | Enables a rich monitor breakpoint slot when available. |
| `breakpoint.disable` | Y | N | Y | N | N | Y | mutating | Disables a rich monitor breakpoint slot when available. |
| `save_state` | Y | Y | Y | Y | Y | Y | mutating | Output path is protected. |
| `load_state` | Y | Y | Y | Y | Y | Y | mutating | Reads caller-provided path. |
| `netsio.status` | Y | N | Y | N | N | Y | read-only | Emulator-side NetSIO/SIO/NETStream status; handler-only fields reported as `null`. |
| `netsio.trace.status` | Y | N | Y | N | N | Y | read-only | NetSIO trace ring enabled/count/drop status. |
| `netsio.trace.read` | Y | N | Y | N | N | Y | read-only | Bounded decoded NetSIO/SIO trace entries. |
| `netsio.trace.clear` | Y | N | Y | N | N | Y | mutating | Clears NetSIO trace ring. |
| `netsio.trace.enable` | Y | N | Y | N | N | Y | mutating | Enables NetSIO trace capture. |
| `netsio.trace.disable` | Y | N | Y | N | N | Y | mutating | Disables NetSIO trace capture. |
<!-- END AI_COMMAND_INVENTORY -->

Documented but not implemented in C: `breakpoint`, `disk_insert`, `disk_eject`, and `disk_status`.

## MCP Tool Inventory

<!-- BEGIN MCP_TOOL_INVENTORY -->
| Tool | C command used | Current notes |
|------|----------------|---------------|
| `atari_start` | process start, `hello` | Starts an MCP-owned session with managed runtime/artifact dirs and per-session sockets. |
| `atari_stop` | process stop | Stops tracked MCP-owned process only; no global process kill. |
| `atari_preflight` | host/emulator checks | Reports emulator, display, Xvfb, runtime, and dependency status. |
| `atari_status` | `hello`, process status | Reports active session, paths, process state, sockets, and bounded logs. |
| `atari_logs` | process log ring | Reads bounded emulator/Xvfb startup logs. |
| `atari_load` | `load` | Executable-style load through `BINLOAD_Loader(path)`. |
| `atari_disk_insert` | `disk.insert` | Copies source image into the session native disk workspace and mounts the copy read-only by default. |
| `atari_disk_eject` | `disk.eject` | Ejects a native Atari disk drive. |
| `atari_disk_status` | `disk.status` | Reports C SIO drive state and MCP managed native disk copies. |
| `atari_artifact_list` | filesystem | Lists session artifact, log, and native disk workspace files. |
| `atari_artifact_info` | filesystem | Reports metadata for one session artifact. |
| `atari_artifact_read_text` | filesystem | Reads a bounded UTF-8 text artifact. |
| `atari_artifact_delete` | filesystem | Deletes files from deletable session roots. |
| `atari_run` | `run` | Text response only. |
| `atari_run_until` | MCP loop over multiple C commands | Runs bounded frame batches until predicates match; returns diagnostics on timeout. |
| `atari_frame_step` | `frame_step` | Frame-loop stepping, not CPU instruction stepping. |
| `atari_pause` | `pause` | Pauses emulation. |
| `atari_screen` | `screen_ascii` | Text formatted 40x24 view. |
| `atari_screen_text` | `screen.text` | Simple ANTIC text memory with confidence and unsupported reasons. |
| `atari_screen_raw` | `screen_raw` | Rendered framebuffer base64 response. |
| `atari_framebuffer_raw` | `framebuffer.raw` | Explicit rendered framebuffer base64 alias. |
| `atari_screenshot` | `screenshot` | Optional path; C defaults into artifact dir. |
| `atari_joystick` | `joystick` | Direction/fire/port. |
| `atari_key` | `key` | Maps key names to AKEY codes. |
| `atari_key_down` | `key.down` | Holds a supported key down. |
| `atari_key_up` | `key.up` | Releases all AI key state. |
| `atari_press_key` | `key.down`, `run`, `key.up` | Presses one supported key for a bounded frame duration. |
| `atari_type_text` | `key.down`, `run`, `key.up` | Types supported text one key press at a time. |
| `atari_key_release` | `key_release` | Releases all AI key state. |
| `atari_paddle` | `paddle` | Sets paddle input value. |
| `atari_consol` | `consol` | Natural pressed booleans translated to C active-low console state. |
| `atari_press_console` | `consol`, `run`, `consol` | Presses one console key for a bounded frame duration. |
| `atari_input_status` | `input.status` | Reads current AI input overrides and active-low console state. |
| `atari_peek` | `peek` | Uses `address`/`length` wrapper fields. |
| `atari_poke` | `poke` | Uses `address`/`values` wrapper fields. |
| `atari_dump_memory` | `dump` | Dumps memory to artifact-safe path. |
| `atari_cpu` | `cpu` | Text formatted CPU state. |
| `atari_cpu_set` | `cpu_set` | Unsafe selected CPU register mutation. |
| `atari_gtia` | `gtia` | Text formatted GTIA state. |
| `atari_pokey` | `pokey` | Text formatted POKEY state. |
| `atari_antic` | `antic` | Text formatted ANTIC state. |
| `atari_pia` | `pia` | Text formatted PIA state. |
| `atari_reset` | `reset` | Cold reset. |
| `atari_debug_enable` | `debug_enable` | Enables debug capture at address. |
| `atari_debug_read` | `debug_read` | Reads and clears debug output. |
| `atari_debugger_status` | `debugger.status` | Reports debugger capabilities and stop state. |
| `atari_show_state` | `debugger.show_state` | Reports CPU and debugger stop state. |
| `atari_history` | `debugger.history` | Recent instruction history. |
| `atari_jumps` | `debugger.jumps` | Recent JMP/JSR history. |
| `atari_stack` | `debugger.stack` | Stack bytes above SP. |
| `atari_disassemble` | `debugger.disassemble` | Bounded monitor-formatted disassembly. |
| `atari_disassemble_loop` | `debugger.disassemble_loop` | Loop disassembly when detectable. |
| `atari_display_list` | `debugger.dlist` | Bounded ANTIC display list view. |
| `atari_memory_search` | `debugger.search_memory` | Byte-pattern memory search. |
| `atari_string_search` | `debugger.search_string` | ATASCII string memory search. |
| `atari_screencode_string_search` | `debugger.search_screencode_string` | ANTIC screen-code string search. |
| `atari_labels` | `debugger.labels` | Label list when available. |
| `atari_step_instruction` | `debugger.step_instruction` | True CPU instruction stepping when available. |
| `atari_debugger_continue` | `debugger.continue` | Continues emulation from debugger stop. |
| `atari_break_on_pc` | `breakpoint.pc` | Enables/disables simple break-on-PC. |
| `atari_break_on_brk` | `breakpoint.brk` | Enables/disables break-on-BRK. |
| `atari_breakpoint_status` | `breakpoint.status` | Reports AI-owned breakpoint state. |
| `atari_breakpoint_list` | `breakpoint.list` | Lists rich monitor breakpoint table entries when available. |
| `atari_breakpoint_add` | `breakpoint.add` | Adds a rich monitor breakpoint entry when available. |
| `atari_breakpoint_delete` | `breakpoint.delete` | Deletes a rich monitor breakpoint slot when available. |
| `atari_breakpoint_enable` | `breakpoint.enable` | Enables a rich monitor breakpoint slot when available. |
| `atari_breakpoint_disable` | `breakpoint.disable` | Disables a rich monitor breakpoint slot when available. |
| `atari_breakpoint_clear` | `breakpoint.clear` | Clears AI-owned breakpoints. |
| `atari_save_state` | `save_state` | Path must pass C output path policy. |
| `atari_load_state` | `load_state` | Reads caller-provided path. |
| `atari_video_status` | `video.status` | Video socket and stream state. |
| `atari_video_enable_push` | `video.enable_push` | Enables push streaming. |
| `atari_video_disable_push` | `video.disable_push` | Disables push streaming. |
| `atari_video_enable_pull` | `video.enable_pull` | Enables pull requests. |
| `atari_video_disable_pull` | `video.disable_pull` | Disables pull requests. |
| `atari_video_set_fps_cap` | `video.push.set_fps_cap` | Sets push FPS cap. |
| `atari_video_set_frameskip` | `video.push.set_frameskip` | Sets push frame skip. |
| `atari_video_set_change_triggered` | `video.push.enable_change_triggered` | Sets changed-frame-only push mode. |
| `atari_netsio_status` | `netsio.status` | Emulator-side NetSIO/SIO/NETStream status. |
| `atari_netsio_trace_status` | `netsio.trace.status` | NetSIO trace ring status. |
| `atari_netsio_trace_read` | `netsio.trace.read` | Bounded decoded NetSIO/SIO trace entries. |
| `atari_netsio_trace_clear` | `netsio.trace.clear` | Clears NetSIO trace ring. |
| `atari_netsio_trace_enable` | `netsio.trace.enable` | Enables NetSIO trace capture. |
| `atari_netsio_trace_disable` | `netsio.trace.disable` | Disables NetSIO trace capture. |
<!-- END MCP_TOOL_INVENTORY -->

`mcp-server/README.md` now matches the actual MCP tool list in `mcp-server/index.js`.

## Inventory Check

Run:

```sh
python3 tools/check_ai_inventory.py
```

The check compares this document's C command and MCP tool inventory tables against `src/ai_interface.c` and `mcp-server/index.js`.
