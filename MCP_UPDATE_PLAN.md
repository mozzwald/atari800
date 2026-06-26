# MCP / AI Interface Update Plan for Atari800

Status: reorganized implementation plan, updated for the cleaned `ai-mcp` branch based on `origin/master` with only AI/MCP interface changes retained.

This plan focuses on making the Atari800 AI interface reliable for AI agents, especially for testing and writing FujiNet-based Atari games and apps. It intentionally separates foundational protocol/session work from debugger, FujiNet, NetSIO, and higher-level automation features so Codex can implement and test the upgrade incrementally.

## Source Snapshot Findings

The cleaned `ai-mcp` branch already has a working but early AI interface. These findings should be treated as the current baseline before any implementation work begins.

### Current C AI socket commands

Discovered from `src/ai_interface.c` command dispatch:

`ping`, `load`, `run`, `step`, `pause`, `reset`, `key`, `key_release`, `joystick`, `paddle`, `consol`, `screenshot`, `screen_ascii`, `screen_raw`, `video.enable_push`, `video.disable_push`, `video.enable_pull`, `video.disable_pull`, `video.push.set_fps_cap`, `video.push.set_frameskip`, `video.push.enable_change_triggered`, `video.status`, `peek`, `poke`, `dump`, `cpu`, `cpu_set`, `antic`, `gtia`, `pokey`, `pia`, `debug_enable`, `debug_read`, `save_state`, `load_state`

Important mismatches and behaviors observed in the current source:

- `src/ai_interface.h` documents `breakpoint`, `disk_insert`, `disk_eject`, and `disk_status`, but `src/ai_interface.c` does not currently dispatch those commands.
- `step` is documented as CPU-instruction stepping, but the current implementation decrements `ai_steps_to_run` from `AI_Frame()`, which is called from the frame loop in `src/atari.c`; therefore current `step` behaves like frame stepping, not true CPU instruction stepping.
- `load` calls `BINLOAD_Loader(path)`, so it is executable-style loading, not general ATR disk insertion.
- `debug_enable` uses the JSON field `addr`; current README text says `port`.
- `poke` expects `data: [byte, ...]`; current README text says `value`.
- `dump` expects `start`, `end`, and `path`; current README text says `addr`, `len`, and `path`.
- `peek` is capped at 256 bytes in C.
- `screen_raw` returns the rendered screen buffer base64 encoded, not raw Atari screen RAM.
- The default AI command socket is `/tmp/atari800_ai.sock`.
- Video push/pull sockets are currently fixed global paths from the C constants, so concurrent MCP-managed sessions are not safe yet.
- Screenshot defaults to `/tmp/atari800_ai_<time>.png` when no path is supplied.
- `poke` writes directly to `MEMORY_mem[(UWORD)addr++]`, bypassing memory attribute checks. That may be useful for debugging but should be clearly documented as unsafe/direct memory mutation.

### Current MCP tools

Discovered from `mcp-server/index.js`:

`atari_start`, `atari_stop`, `atari_run`, `atari_screen`, `atari_joystick`, `atari_key`, `atari_consol`, `atari_peek`, `atari_poke`, `atari_cpu`, `atari_gtia`, `atari_pokey`, `atari_antic`, `atari_pia`, `atari_reset`, `atari_save_state`, `atari_load_state`

Important MCP baseline observations:

- `mcp-server/README.md` lists tools that do not match the actual `index.js` tool names. Examples: README refers to `atari_build`, `atari_status`, `atari_run_frames`, `atari_screenshot`, `atari_send_key`, `atari_breakpoint`, and `atari_step`; current `index.js` exposes a smaller and differently named set.
- `atari_start` currently requires `program`, spawns `src/atari800 -ai -xl -run <program>`, removes the fixed global socket before start, and uses the fixed `/tmp/atari800_ai.sock` path.
- `atari_stop` currently kills the tracked process if known, but also calls `pkill -9 atari800`, which can kill unrelated emulator instances.
- MCP currently sends JSON over a length-prefixed Unix socket and parses length-prefixed JSON replies, but command/response handling is ad hoc and not yet a versioned contract.
- MCP output often formats text only. For automation, most tools should also preserve structured JSON response data where useful.

### Current build/configuration facts

- `build_ai.sh` currently configures SDL 1.2, not SDL2, with `--enable-ai-interface` and `--enable-netsio` because FujiNet/NetSIO support is a required MCP feature.
- The build script comments say SDL2 has broken keyboard trigger behavior for this branch.
- `configure.ac` exposes `--enable-ai-interface`, default OFF.
- `configure.ac` exposes `--enable-netsio`, default ON when supported by the target build. The MCP-targeted build should pass it explicitly so missing NetSIO support fails visibly.
- `configure.ac` has monitor options: `--enable-monitorbreak` default ON, `--enable-monitorbreakpoints` default OFF, `--enable-monitorprofile` default OFF, and `--enable-monitortrace` default OFF.
- `src/atari.c` supports `-netsio [port]`, defaulting to UDP port `9997`. Enabling NetSIO disables patched SIO/H/P/R handlers and waits briefly for a FujiNet connection before continuing startup.

### Current NetSIO facts

- `src/netsio.h` defines `NETSIO_PROCEED_OFF/ON` as `0x30/0x31` and `NETSIO_INTERRUPT_OFF/ON` as `0x40/0x41`.
- `src/netsio.c` applies Proceed to PIA CA1 and Interrupt to PIA CB1; active-low ON means asserted/low.
- The uploaded `netsio-protocol.md` top table agrees with the source constants, but later Proceed/Interrupt sections appear swapped. Treat the source as authoritative unless tests prove otherwise.
- Current NetSIO tracks useful state internally: `netsio_enabled`, `fujinet_known`, sync number, sync wait state, `netsio_next_write_size`, FujiNet address, and NetStream gate state.
- Current NetStream activity is gated by FujiNet enable acknowledgement for command frame `$70/$F0`, motor state, and compatible POKEY serial configuration.

## Goals

- Make documented AI commands match implemented behavior.
- Establish a versioned, schema-validated, deterministic AI socket protocol.
- Add a session model so MCP can safely manage one or more emulator/FujiNet/Xvfb runtimes without global socket collisions or destructive process cleanup.
- Expose the useful existing C socket API through MCP.
- Treat NetSIO as a required MCP build/runtime feature for FujiNet workflows.
- Add FujiNet-PC and NetSIO observability tools that help agents diagnose app behavior.
- Add safe disk/mount workflows for both native Atari800 disk images and FujiNet-PC `fnconfig.ini` based mounts.
- Add debugger features in stages, starting with safe state/step/continue and only later rich monitor breakpoints.
- Add `run_until` and screen/input automation primitives for repeatable agent test loops.
- Add an AI-targeted README/contract that teaches agents how to use the interface correctly.
- Keep the interface deterministic enough for repeatable automated tests.

## Non-Goals

- Do not replace Atari800's interactive human monitor UI.
- Do not expose the monitor `!command` shell escape through MCP.
- Do not expose host shell execution through MCP.
- Do not make FujiNet CONFIG UI automation the primary workflow. Agents should configure FujiNet-PC through managed files and deterministic resets.
- Do not kill unrelated Atari800, Xvfb, FujiNet-PC, VNC, or xpra processes by default.
- Do not guarantee text extraction for arbitrary custom graphics modes.
- Do not support every FujiNet-PC platform in v1.
- Do not silently fall back from headless to visible mode.
- Do not allow arbitrary host paths by default without a clear path policy.
- Do not expose assembler, RAM/ROM/hardware memory attribute mutation, or other high-risk monitor mutations in the default safe toolset.

## Implementation Strategy

Work from the bottom up:

1. Define the protocol and safety contract.
2. Define sessions and managed runtime directories.
3. Normalize existing command/documentation parity.
4. Expose existing stable capabilities through MCP.
5. Add headless startup and process lifecycle safety.
6. Add basic debugger controls.
7. Add FujiNet-PC sidecar and deterministic disk boot workflows.
8. Add NetSIO/SIO observability.
9. Add high-level agent automation tools.
10. Add optional/richer debugging and packaging.

Every phase should include tests and documentation updates. The document should remain the live source of truth: update checkboxes as tasks are completed.

---

# Phase 0: Establish Agent Contract and Source Inventory

Purpose: create a reliable map of what exists before changing behavior.

Progress checklist:

- [ ] Create an authoritative AI command inventory from `src/ai_interface.c`.
- [ ] Create an authoritative MCP tool inventory from `mcp-server/index.js`.
- [ ] Compare C, Python, MCP, `README.md`, `src/ai_interface.h`, and `mcp-server/README.md`.
- [ ] Mark each command/tool as implemented, exposed by Python, exposed by MCP, documented, mismatched, or missing.
- [ ] Create a minimal `README.AI.md` or `AGENT_CONTRACT.md` describing current behavior and planned contract.
- [ ] Add a generated or scripted inventory check so docs can be kept in sync.

## 0.1 Inventory Table Requirements

For every AI command, record:

- command name
- input schema
- output schema
- implemented in C socket API
- exposed by Python client
- exposed by MCP
- documented in main README
- documented in MCP README
- safe/mutating/unsafe classification
- current limitations
- planned replacement or rename, if any

## 0.2 Known Initial Mismatches to Fix or Document

Document these immediately so Codex does not build on false assumptions:

- `step` currently runs frame-loop ticks, not CPU instructions.
- `breakpoint` is documented but missing from C dispatch.
- `disk_insert`, `disk_eject`, and `disk_status` are documented but missing from C dispatch.
- `debug_enable` docs should use `addr`, not `port`.
- `poke` docs should use `data: [byte, ...]`, not `value`.
- `dump` docs should use `start`, `end`, and `path`, not `addr`/`len`.
- `screen_raw` should be described as rendered framebuffer data, not Atari screen memory.
- `load` should be described as `BINLOAD_Loader()` executable-style loading.

## 0.3 Minimal Agent Contract

Before broad implementation, create a short agent-facing document containing:

- how to start a managed emulator session
- how to check capabilities
- safe path policy
- command timeout behavior
- structured error shape
- session status shape
- how to run frames and read screen/debug output
- distinction between native Atari800 disk mounts and FujiNet-PC mounts
- warning that CONFIG UI automation is not the intended FujiNet workflow

---

# Phase 1: Protocol Hardening and Capability Discovery

Purpose: establish the stable AI socket protocol before adding many tools.

Progress checklist:

- [x] Add protocol version and capability discovery command.
- [x] Ensure the MCP-targeted build path explicitly enables and reports NetSIO as a required capability.
- [x] Add structured response/error helpers in C.
- [x] Add JSON string escaping for all returned strings and paths.
- [x] Add schema validation for command inputs.
- [x] Add bounded command size and bounded response size behavior.
- [x] Add consistent timeout behavior for commands that wait for frames or screen changes. Breakpoint, sync, and lifecycle waits may define timeout shape here but should be runtime-tested in later phases.
- [x] Add safe path handling helpers.
- [x] Add command metadata classifications: read-only, mutating, unsafe.
- [x] Add build-safe protocol helper tests for invalid JSON, unknown command, missing field, bad type, oversized request, and unsafe path without starting the emulator.
- [x] Defer live AI socket, lifecycle, process, display, FujiNet, and NetSIO integration tests to later phases after the session model exists.

## 1.1 Add `hello` / `capabilities`

Add a C socket command:

```json
{"cmd":"hello"}
```

Suggested response fields:

```json
{
  "status": "ok",
  "protocol_version": 1,
  "emulator": "atari800",
  "ai_interface": true,
  "build": {
    "ai_interface": true,
    "netsio": true,
    "monitor_break": true,
    "monitor_breakpoints": false,
    "monitor_trace": false,
    "monitor_profile": false
  },
  "limits": {
    "max_command_bytes": 65535,
    "max_response_bytes": 1048576,
    "peek_max_len": 256
  },
  "commands": ["ping", "hello", "capabilities", "run", "peek"],
  "sockets": {
    "command": "/tmp/atari800_ai.sock",
    "video_push": "/tmp/atari800-fb-push.sock",
    "video_pull": "/tmp/atari800-fb-pull.sock"
  }
}
```

Implementation notes:

- Compile-time capability flags should be based on `#ifdef AI_INTERFACE`, `#ifdef NETSIO`, `#ifdef MONITOR_BREAK`, `#ifdef MONITOR_BREAKPOINTS`, `#ifdef MONITOR_TRACE`, and `#ifdef MONITOR_PROFILE`.
- Runtime capability fields should report active AI socket path, actual video socket paths, and NetSIO compiled/enabled/runtime status. For the MCP-targeted build, missing NetSIO should be reported as a build/preflight failure rather than treated as an optional capability.
- This should be the first command MCP uses after connecting.

## 1.2 Structured Error Shape

All errors should use a consistent shape:

```json
{
  "status": "error",
  "code": "BAD_ARGUMENT",
  "message": "addr must be 0..65535",
  "details": {"field":"addr","value":70000}
}
```

Recommended error codes:

- `BAD_JSON`
- `UNKNOWN_COMMAND`
- `BAD_ARGUMENT`
- `MISSING_FIELD`
- `UNSUPPORTED`
- `CAPABILITY_UNAVAILABLE`
- `TIMEOUT`
- `NOT_RUNNING`
- `NOT_PAUSED`
- `BUSY`
- `PATH_DENIED`
- `IO_ERROR`
- `EMULATOR_EXITED`
- `INTERNAL_ERROR`

## 1.3 Safe Path Policy

Do not leave path policy implicit. Define managed directories:

- runtime directory: sockets, pid files, temporary logs
- artifact directory: screenshots, dumps, trace exports, generated state files
- disk workspace: copied ATR/XFD images for write-enabled tests
- FujiNet sidecar directory: managed FujiNet-PC config and SD tree

Rules:

- By default, write operations may only target the managed artifact/workspace directories.
- Read operations may read user-provided program/disk paths if explicitly passed by the caller.
- `dump`, `screenshot`, `save_state`, and generated config writes should reject paths outside the allowed artifact/workspace unless `unsafe_paths=true` is explicitly enabled by MCP server configuration.
- Normalize and resolve paths before checking them.
- Never unlink sockets, files, or directories that are not recorded as MCP-owned for the session.

## 1.4 JSON Escaping and Response Safety

Current code uses `snprintf()` into JSON strings with paths and error messages. Replace this pattern with helper functions that escape JSON strings correctly.

Acceptance criteria:

- paths containing quotes, backslashes, spaces, or Unicode do not break JSON
- failed load/screenshot/save paths return valid JSON
- all C socket responses parse with a standard JSON parser

---

# Phase 2: Session Model, Runtime Directories, and Process Lifecycle

Purpose: make MCP-managed emulator runs deterministic and non-destructive.

Progress checklist:

- [ ] Add MCP session object.
- [ ] Add managed runtime directory creation.
- [ ] Derive per-session AI command socket path.
- [ ] Derive per-session video push/pull socket paths.
- [ ] Add C CLI options for custom video push/pull socket paths, or another safe mechanism for per-session paths.
- [ ] Add `atari_status` with session state and effective launch details.
- [ ] Replace global fixed socket cleanup with MCP-owned cleanup only.
- [ ] Remove default `pkill -9 atari800` behavior.
- [ ] Add bounded stdout/stderr capture for Atari800 process.
- [ ] Add graceful stop, then optional force stop.
- [ ] Add stale MCP-owned runtime cleanup.

## 2.1 Session Object

MCP should maintain a session object like:

```json
{
  "session_id": "a8-20260626-abcdef",
  "state": "running",
  "runtime_dir": "/tmp/atari800-mcp/a8-...",
  "artifact_dir": "/tmp/atari800-mcp/a8-.../artifacts",
  "emulator": {
    "pid": 12345,
    "argv": ["/path/src/atari800", "-ai", "-ai-socket", "..."],
    "socket": "/tmp/atari800-mcp/a8-.../ai.sock",
    "started_at": "...",
    "exit_code": null
  },
  "xvfb": null,
  "fujinet": null,
  "owned_paths": []
}
```

Recommended states:

- `not_started`
- `starting`
- `running`
- `paused`
- `breakpoint_stopped`
- `exited`
- `crashed`
- `cleanup_failed`

## 2.2 Per-Session Socket Paths

Current C supports `-ai-socket`, but video push/pull paths are fixed constants. Add one of these:

Preferred:

- `-ai-video-push-socket <path>`
- `-ai-video-pull-socket <path>`

Alternative:

- derive video socket paths from `AI_socket_path`, e.g. `<ai_socket>.video_push` and `<ai_socket>.video_pull`

Acceptance criteria:

- two MCP sessions can run without socket collisions
- cleanup only removes sockets recorded in `owned_paths`
- `video.status` reports effective paths

## 2.3 Process Stop Policy

Replace the destructive `pkill -9 atari800` default.

Suggested `atari_stop` input:

```json
{
  "force": false,
  "cleanup_runtime_dir": true
}
```

Behavior:

1. If emulator process is tracked and alive, request graceful termination.
2. Wait a bounded time.
3. If still alive and `force=true`, hard kill tracked pid only.
4. Stop MCP-owned Xvfb and FujiNet-PC sidecars.
5. Remove only MCP-owned sockets/files.
6. Never kill unrelated emulator processes by default.

---

# Phase 3: Fix Documentation / Implementation Mismatches

Purpose: make the current interface honest before adding higher-level tools.

Progress checklist:

- [ ] Correct `README.md` AI command table.
- [ ] Correct `src/ai_interface.h` command docs.
- [ ] Correct `mcp-server/README.md` to match actual MCP tools or add missing tools first.
- [ ] Fix or rename current `step` behavior.
- [ ] Decide whether to implement or remove documented missing disk and breakpoint commands.
- [ ] Add tests that compare documented command inventory to C dispatch and MCP tool list.

## 3.1 Correct Current C API Documentation

Required corrections:

- `load`: executable-style load through `BINLOAD_Loader()`, not disk insertion.
- `step`: current behavior is frame-loop stepping; do not claim CPU instruction stepping until implemented.
- `debug_enable`: field is `addr`.
- `poke`: field is `data: [byte, ...]`.
- `dump`: fields are `start`, `end`, `path`.
- `screen_raw`: rendered framebuffer bytes, not Atari screen RAM.
- `key`: field is `code`, not `keycode`.
- `key_release`: no keycode argument; it releases all AI key state.
- `consol`: current C default behavior uses booleans in an inverted/active-low way. Test and document exact semantics before changing.
- `peek`: maximum length is 256 bytes.

## 3.2 Missing C Commands

Currently documented but not dispatched:

- `breakpoint`
- `disk_insert`
- `disk_eject`
- `disk_status`

Decision:

- Implement native disk commands because they are directly useful for app tests.
- Do not implement rich `breakpoint` in this phase. Replace with explicit debugger phase commands later.
- If a temporary simple PC breakpoint command is implemented, name it clearly, e.g. `breakpoint.pc`, not generic `breakpoint`.

## 3.3 Native Disk Command Semantics

Add native Atari800 disk image tools separately from FujiNet-PC mount tools.

Suggested C commands:

- `disk.insert`
- `disk.eject`
- `disk.status`

Suggested MCP tools:

- `atari_disk_insert`
- `atari_disk_eject`
- `atari_disk_status`

Inputs/outputs should include:

- drive number, D1-D8
- image path
- read-only/read-write mode
- image type detection if available
- mounted path
- write/dirty status if available
- eject flush behavior
- error if attempting to mutate a managed read-only image

Document clearly that these are Atari800 native disk mounts, not FujiNet-PC `fnconfig.ini` mounts.

---

# Phase 4: MCP Tool Coverage for Existing Safe C API

Purpose: expose the stable existing socket commands through MCP before implementing new emulator internals.

Progress checklist:

- [ ] Add `atari_status`.
- [ ] Add `atari_pause`.
- [ ] Add `atari_load`.
- [ ] Add `atari_screenshot`.
- [ ] Add `atari_screen_raw`.
- [ ] Add `atari_paddle`.
- [ ] Add `atari_key_release`.
- [ ] Add `atari_cpu_set` with safety notes.
- [ ] Add `atari_debug_enable` and `atari_debug_read`.
- [ ] Add video push/pull MCP tools.
- [ ] Return structured JSON and a human-readable summary where useful.
- [ ] Sync `mcp-server/README.md` with actual tool list.

## 4.1 Existing C Commands to Expose

Add MCP wrappers for:

- `ping` → used internally for health checks
- `load` → `atari_load`
- `run` → `atari_run`
- `pause` → `atari_pause`
- `reset` → `atari_reset`
- `key` → `atari_key`
- `key_release` → `atari_key_release`
- `joystick` → `atari_joystick`
- `paddle` → `atari_paddle`
- `consol` → `atari_consol`
- `screenshot` → `atari_screenshot`
- `screen_ascii` → `atari_screen`
- `screen_raw` → `atari_screen_raw`
- `peek` → `atari_peek`
- `poke` → `atari_poke`
- `dump` → `atari_dump_memory`
- `cpu` → `atari_cpu`
- `cpu_set` → `atari_cpu_set`
- `antic` → `atari_antic`
- `gtia` → `atari_gtia`
- `pokey` → `atari_pokey`
- `pia` → `atari_pia`
- `debug_enable` → `atari_debug_enable`
- `debug_read` → `atari_debug_read`
- `save_state` → `atari_save_state`
- `load_state` → `atari_load_state`
- `video.status` → `atari_video_status`
- `video.enable_push` → `atari_video_enable_push`
- `video.disable_push` → `atari_video_disable_push`
- `video.enable_pull` → `atari_video_enable_pull`
- `video.disable_pull` → `atari_video_disable_pull`
- `video.push.set_fps_cap` → `atari_video_set_fps_cap`
- `video.push.set_frameskip` → `atari_video_set_frameskip`
- `video.push.enable_change_triggered` → `atari_video_set_change_triggered`

## 4.2 MCP Naming Convention

Use `atari_` prefix consistently for Atari800/emulator tools.

Use `fujinet_` prefix for FujiNet-PC sidecar tools.

Use `netsio_` prefix only for NetSIO bus/protocol tools, unless everything is grouped under `atari_netsio_*` for discoverability.

Avoid duplicate tool names that mean different things:

- `atari_run` should mean run frames if emulator is already started.
- `atari_start` should start emulator.
- Do not call start-with-program `atari_run`.

## 4.3 Structured Tool Results

MCP tools should not throw away JSON fields. Return text summaries, but preserve JSON details either as formatted JSON text or as a clearly structured block.

Example for `atari_cpu`:

```text
CPU State:
PC=$1234 A=$00 X=$00 Y=$00 SP=$FF Flags=N0 V0 B0 D0 I1 Z0 C0

Raw:
{...}
```

---

# Phase 5: Improve `atari_start`, Preflight, and Headless Display Handling

Purpose: make remote SSH/tmux/CI usage reliable while keeping visible mode available.

Progress checklist:

- [ ] Add `atari_preflight`.
- [ ] Add expanded `atari_start` arguments.
- [ ] Implement `display_mode`: `auto`, `headless`, `visible`.
- [ ] Launch Xvfb directly for headless Linux/X11-compatible builds.
- [ ] Track Xvfb process and cleanup ownership.
- [ ] Add structured startup errors.
- [ ] Add stdout/stderr ring buffer and log tools for emulator startup failures.
- [ ] Return effective argv/display/audio/socket settings in `atari_status`.

## 5.1 `atari_preflight`

Report:

- emulator path exists and executable
- emulator version/help output if available
- whether `--enable-ai-interface` appears available or pingable after start
- whether `Xvfb` executable exists
- whether native display appears available: `DISPLAY`, `WAYLAND_DISPLAY`, macOS desktop session
- detected OS/platform/architecture
- detected SDL expectations if possible
- whether FujiNet-PC is configured or selected
- available artifact/runtime directories
- missing dependency hints

Important display caveat:

- Xvfb only helps if the Atari800 build can use an X11-compatible SDL backend. If this build uses SDL 1.2 on Linux, Xvfb is appropriate. If a macOS build uses native Cocoa/Quartz instead of X11, Xvfb may not help. Preflight should report the limitation instead of assuming Xvfb solves all platforms.
- Headless macOS is optional future work, not a v1 requirement. Initial macOS support should prioritize visible native-window runs and clear preflight errors when headless/Xvfb cannot work with the selected build.

## 5.2 Expanded `atari_start` Arguments

Suggested arguments:

- `program`
- `machine`: `atari`, `xl`, `xe`, `5200`
- `ram`
- `basic`: boolean
- `netsio`: boolean
- `netsio_port`
- `ai_socket`
- `debug_port`
- `turbo`: boolean
- `sound`: boolean, default `false` for headless/Xvfb and default `true` for visible
- `display_mode`: `auto`, `headless`, `visible`
- `xvfb_display`: optional display number, default auto-selected
- `xvfb_screen`: optional screen geometry, default `1024x768x24`
- `args`: extra emulator CLI arguments, advanced/debug only
- `disks`: array of native ATR/XFD paths to mount at startup
- `artifact_dir`: optional managed artifact output directory

Suggested behavior:

- Default emulator args: `-ai -xl`.
- Pass `-ai-socket <path>` using per-session socket.
- Add `-netsio <port>` only when `netsio=true`.
- Use `-run <program>` only for executable-style programs.
- Pass disk images as normal Atari800 positional disk arguments when appropriate.
- Add `-nosound` for headless by default unless `sound=true`.
- Prefer `-no-video-accel` for Xvfb/headless launches if the current build supports it and testing confirms it helps.
- Log and report final argv.

## 5.3 `args` Escape Hatch Policy

`args` is useful but dangerous. Rules:

- Treat `args` as advanced/debug.
- Log final argv.
- MCP-managed socket, display, audio, and NetSIO flags should override conflicting `args` unless `unsafe_override=true` is explicitly enabled.
- Consider a denylist for flags that conflict with managed mode: `-ai-socket`, video socket flags, `-netsio`, display flags, save paths, or path-changing options.
- Return the resolved argv in `atari_status`.

## 5.4 Display Launch Policy

- `auto`: behave like `headless` unless caller explicitly requests `visible`.
- `headless`: start MCP-owned Xvfb and launch Atari800 with `DISPLAY=:N`.
- `visible`: use native display/session; do not wrap in Xvfb.
- If headless requested and Xvfb unavailable, return `CAPABILITY_UNAVAILABLE` with package hints.
- If visible requested but no compatible display is available, return `MISSING_DISPLAY` and suggest `display_mode=headless`.
- Launch `Xvfb` directly instead of depending on `xvfb-run`.
- Auto-select a free display number and wait for the X socket before launching Atari800.
- Stop only MCP-owned Xvfb process.
- Expose display mode, display number, Xvfb pid, and video/audio flags in `atari_status`.

## 5.5 Host Dependency Hints

- macOS/Homebrew: SDL as required by the project; Xvfb may require `xorg-server`, but only helps X11-compatible builds.
- Debian/Ubuntu: SDL runtime/dev packages as needed and `xvfb`.
- Fedora/RHEL: SDL packages and `xorg-x11-server-Xvfb`.
- Arch: SDL packages and `xorg-server-xvfb`.

Dependency detection should check for actual executables/libraries first and only use distro-specific names in hints.

---

# Phase 6: Basic Debugger Integration

Purpose: add reliable AI debugger control without entering the interactive monitor.

Progress checklist:

- [ ] Add debugger capability reporting.
- [ ] Add `debugger.show_state` / `atari_show_state`.
- [ ] Identify the exact monitor/CPU execution path to reuse for true instruction stepping before implementing it.
- [ ] Add true CPU instruction stepping.
- [ ] Add continue/pause semantics.
- [ ] Add simple break-on-PC and break-on-BRK.
- [ ] Ensure AI breakpoint hits pause emulation and return JSON, not interactive monitor.
- [ ] Add tests proving breakpoint hits do not enter `MONITOR_Run()` in AI mode.

## 6.1 Capabilities

Add `debugger.status` C command and `atari_debugger_status` MCP tool.

Report compile-time flags:

- `MONITOR_BREAK`
- `MONITOR_BREAKPOINTS`
- `MONITOR_ASSEMBLER`
- `MONITOR_HINTS`
- `MONITOR_TRACE`
- `MONITOR_PROFILE`

Also report runtime state:

- paused/running
- stopped reason
- last breakpoint id/slot
- PC
- whether history is available

## 6.2 True Instruction Stepping

First perform a feasibility pass: identify whether Atari800's existing monitor step/over/return implementation can be reused directly, whether it requires a small refactor, or whether a new CPU execution hook is needed. Document the chosen call path before changing stepping semantics.

Current `step` is frame-based. Replace or supplement it with explicit names:

- `run_frames` / `atari_run` for frames
- `debugger.step_instruction` / `atari_step_instruction` for one or more CPU instructions
- optional compatibility: keep `step` but document as deprecated or make it call true instruction stepping only after implemented

Do not rely on the command letter names from the human monitor until verified from source. The AI API should expose semantic names and call lower-level monitor/CPU functions internally.

## 6.3 Avoid Interactive Monitor on AI Breaks

Current monitor break path in CPU can lead to `Atari800_Exit(TRUE)` and `MONITOR_Run()`. For AI mode:

- breakpoint hit should set AI paused/stopped state
- capture stop reason
- capture CPU state
- capture PC and next instruction/disassembly if available
- capture matching breakpoint id/slot
- return JSON to waiting MCP command or make it visible in `atari_status`
- leave human monitor path unchanged when AI mode is not active

Suggested stop reason values:

- `manual_pause`
- `frame_limit`
- `instruction_limit`
- `breakpoint_pc`
- `breakpoint_brk`
- `breakpoint_condition`
- `program_exit`
- `emulator_exit`
- `timeout`

## 6.4 Simple Breakpoint Commands

Suggested C commands:

- `breakpoint.pc`
- `breakpoint.brk`
- `breakpoint.status`
- `breakpoint.clear`

Suggested MCP tools:

- `atari_break_on_pc`
- `atari_break_on_brk`
- `atari_breakpoint_status`
- `atari_breakpoint_clear`

Keep this phase limited. Rich conditional breakpoints come later.

---

# Phase 7: Rich Debugger / Monitor-Aligned Tools

Purpose: expose monitor-aligned read-only debugger views and optional rich breakpoint table support.

Progress checklist:

- [ ] Add read-only debugger views.
- [ ] Add label support.
- [ ] Add rich breakpoint table support when `MONITOR_BREAKPOINTS` is compiled in.
- [ ] Add trace/profile/coverage tools only behind capability checks.
- [ ] Keep unsafe monitor commands out of default MCP toolset.

## 7.1 Read-Only Debugger Views

Suggested C commands:

- `debugger.history`
- `debugger.jumps`
- `debugger.stack`
- `debugger.disassemble`
- `debugger.disassemble_loop`
- `debugger.dlist`
- `debugger.search_memory`
- `debugger.search_string`
- `debugger.search_screencode_string`
- `debugger.labels`

Suggested MCP tools:

- `atari_history`
- `atari_jumps`
- `atari_stack`
- `atari_disassemble`
- `atari_disassemble_loop`
- `atari_display_list`
- `atari_memory_search`
- `atari_string_search`
- `atari_screencode_string_search`
- `atari_labels`

## 7.2 Rich Breakpoints

Only expose these when `MONITOR_BREAKPOINTS` is compiled in:

- `breakpoint.list`
- `breakpoint.add`
- `breakpoint.delete`
- `breakpoint.enable`
- `breakpoint.disable`
- `breakpoint.clear`

Use monitor table fields internally:

- `enabled`
- `condition`
- `value`
- `m_addr`

Accept both:

1. structured JSON form
2. monitor-compatible condition string

Return both `id` and `slot` if deletion compacts or reuses slots. If ids are exactly monitor table slots, document that slots may be reused after delete.

Supported rich condition types should mirror monitor/source support:

- `PC`
- `A`
- `X`
- `Y`
- `S`
- `READ`
- `WRITE`
- `ACCESS`
- `MEM:addr`
- flag set/clear checks
- explicit `OR`; adjacent conditions are AND-connected

If `MONITOR_BREAKPOINTS` is not compiled in, return `CAPABILITY_UNAVAILABLE` for rich conditions and keep only simple PC/BRK behavior.

## 7.3 Optional Trace/Profile/Coverage

Suggested MCP tools:

- `atari_trace_start`
- `atari_trace_stop`
- `atari_trace_read`
- `atari_profile`
- `atari_coverage`

Rules:

- Return capability errors unless built with required flags.
- Keep trace reads bounded.
- Include dropped record counts.
- Avoid unbounded memory growth.

## 7.4 Unsafe Monitor Commands Excluded by Default

Do not expose by default:

- shell escape `!command`
- assembler `A`
- memory attribute mutation `RAM`, `ROM`, `HARDWARE`
- arbitrary monitor file read/write commands

If later exposed, require explicit `unsafe_debug=true` MCP server configuration.

---

# Phase 8: FujiNet-PC Sidecar Management

Purpose: make FujiNet-PC deterministic and inspectable under MCP control.

Progress checklist:

- [ ] Add FujiNet-PC version selection and optional nightly fetch.
- [ ] Add pinned version support.
- [ ] Add offline local FujiNet-PC path support.
- [ ] Start FujiNet-PC as an MCP-owned sidecar.
- [ ] Allocate non-default UDP ports.
- [ ] Pass selected port to Atari800 `-netsio <port>`.
- [ ] Capture FujiNet-PC stdout/stderr in a bounded ring buffer.
- [ ] Add `fujinet_status`, `fujinet_logs`, and debug read/clear/status tools.
- [ ] Stop only MCP-owned FujiNet-PC process.

## 8.1 Version Selection / Fetching

Tools:

- `fujinet_fetch_latest`
- `fujinet_list_versions`
- `fujinet_select_version`
- `fujinet_set_local_path`

Rules:

- Do not make “latest nightly” the only path.
- Allow exact tag/version pinning.
- Allow an already-unpacked local FujiNet-PC path.
- Record selected version/tag/hash in session status.
- Cache downloads under a managed cache such as `.cache/fujinet-pc/`.
- Never overwrite user-managed FujiNet-PC installs.
- If GitHub asset names do not match expected patterns, fail with a list of available Atari assets instead of guessing.

Known asset patterns from prior observation:

- `fujinet-pc-ATARI_*_macos-14-arm64.tar.gz`
- `fujinet-pc-ATARI_*_macos-15-arm64.tar.gz`
- `fujinet-pc-ATARI_*_macos-15-x64.tar.gz`
- `fujinet-pc-ATARI_*_ubuntu-22.04-amd64.tar.gz`
- `fujinet-pc-ATARI_*_ubuntu-24.04-amd64.tar.gz`

## 8.2 Sidecar Process

Tools:

- `fujinet_start`
- `fujinet_stop`
- `fujinet_status`
- `fujinet_logs`
- `fujinet_debug_read`
- `fujinet_debug_clear`
- `fujinet_debug_status`

Port allocation:

- choose a free UDP port starting in a project-specific range such as `19997-20097`
- avoid default `9997` unless explicitly requested
- pass same port to FujiNet-PC and Atari800 `-netsio <port>`
- report selected port in both FujiNet and Atari status

`fujinet_status` should report:

- selected version/tag
- executable path
- working directory
- process id
- UDP port
- config path
- SD/data path
- whether Atari800 NetSIO has connected
- last stdout/stderr sequence
- exit code if exited

## 8.3 FujiNet-PC Debug Capture

Start FujiNet-PC with stdout/stderr piped into MCP.

Store output in a bounded ring buffer with:

- sequence number
- timestamp
- stream: stdout/stderr
- raw line text

`fujinet_debug_read` inputs:

- `since_seq`
- `limit`
- `contains`
- `regex`
- `stream`: `stdout`, `stderr`, or `both`

Output:

```json
{
  "lines": [{"seq":1,"timestamp_us":123,"stream":"stdout","text":"..."}],
  "next_seq": 2,
  "dropped": 0
}
```

Include debug excerpts in failed boot/remount/run_until responses.

---

# Phase 9: Managed FujiNet `fnconfig.ini` and Mount/Boot Workflow

Purpose: boot FujiNet-based programs deterministically without driving CONFIG UI.

Progress checklist:

- [ ] Generate managed FujiNet-PC working directory.
- [ ] Parse and write `fnconfig.ini` as structured config.
- [ ] Use atomic writes and backups.
- [ ] Add managed SD/data path handling.
- [ ] Implement mount-and-boot workflow using deterministic config and cold reset.
- [ ] Add remount and cold reset semantics.
- [ ] Document FujiNet workflows for agents in `README.AI.md`.

## 9.1 Config Ownership Rules

Rules:

- Prefer writing only MCP-managed `fnconfig.ini` in a sidecar working directory.
- Never overwrite unmanaged FujiNet-PC config unless explicitly requested.
- Write temp file then atomic rename.
- Keep timestamped backups of prior managed config.
- Report exactly which config paths were written.
- Normalize paths copied into SD/data directory.
- Record config path and SD/data path in `fujinet_status`.

## 9.2 Mount Tools

Suggested tools:

- `fujinet_config_get`
- `fujinet_config_set`
- `fujinet_mount_disk`
- `fujinet_unmount_disk`
- `fujinet_mount_status`
- `fujinet_boot`
- `fujinet_remount`

Inputs should distinguish:

- source image path
- slot/drive number
- read-only vs write-enabled
- copy-to-workspace mode
- whether to preserve modified output image
- boot mode

Default should be safe:

- copy disk images into managed workspace
- mount copies read-only unless write-enabled explicitly requested
- never mutate source disk images by default

## 9.3 Boot Workflow

Suggested deterministic flow:

1. Create or reuse session runtime directory.
2. Select FujiNet-PC version/path.
3. Create managed FujiNet working directory and config.
4. Copy desired disk images into managed SD/data workspace.
5. Set `boot_mode=1` or equivalent deterministic mount/boot config.
6. Start FujiNet-PC on allocated UDP port.
7. Start Atari800 with `-netsio <same_port> -ai -xl`.
8. Wait for NetSIO connection and FujiNet readiness.
9. Cold reset Atari/FujiNet pair.
10. Use `run_until` plus screen/debug/SIO trace to confirm boot.

---

# Phase 10: NetSIO / SIO Observability

Purpose: let agents diagnose SIO, FujiNet, and NETStream behavior without reading raw emulator logs.

Progress checklist:

- [ ] Add NetSIO status C command.
- [ ] Add NetSIO trace ring buffer.
- [ ] Add SIO command frame tracing.
- [ ] Add sync/ACK/NAK observability.
- [ ] Add credit/queue/backpressure visibility.
- [ ] Add NETStream status fields.
- [ ] Verify Proceed/Interrupt constants and PIA CA1/CB1 mapping with source and tests.

## 10.1 Important Source-Based Mapping

Use current source constants unless tests prove otherwise:

- Proceed OFF/ON: `0x30/0x31`
- Interrupt OFF/ON: `0x40/0x41`
- Proceed maps to PIA CA1
- Interrupt maps to PIA CB1
- NetSIO active-low ON means asserted/low

Add a test or code comment because the uploaded protocol doc has a later section where Proceed/Interrupt IDs appear swapped.

## 10.2 `netsio.status`

Suggested C command:

```json
{"cmd":"netsio.status"}
```

Suggested MCP tool:

- `atari_netsio_status`

Report baseline fields available from the current NetSIO source where practical:

- `compiled`: whether `NETSIO` compiled
- `initialized`
- `enabled`
- `fujinet_known`
- UDP port
- peer address/port if known
- sync number
- sync wait active
- next write size
- proceed CA1 state if externally readable or newly instrumented
- interrupt CB1 state if externally readable or newly instrumented
- NetStream gate fields already tracked in `src/netsio.c`

Optional instrumentation to add during this phase where useful:

- last sync response number
- last ACK/NAK byte
- sync timeout count
- last received datagram timestamp
- last sent datagram timestamp
- counters by message type
- current baud if observable from existing Atari800/POKEY state
- queue, credit, or buffering metrics if available from the current NetSIO implementation

## 10.3 NetSIO Trace Ring

Suggested C commands:

- `netsio.trace.status`
- `netsio.trace.read`
- `netsio.trace.clear`
- `netsio.trace.enable`
- `netsio.trace.disable`

Trace entries:

```json
{
  "seq": 123,
  "timestamp_us": 456,
  "direction": "atari_to_fujinet",
  "type": "COMMAND_OFF_SYNC",
  "id": 24,
  "len": 2,
  "data_hex": "18 07",
  "decoded": {"sync":7}
}
```

Keep buffer bounded and report dropped count.

## 10.4 SIO Command Frame Decoding

Add optional higher-level decoded events:

- device id
- command byte
- aux1/aux2
- checksum
- ACK/NAK/completion bytes
- sector number where applicable
- Fuji device `$70` commands where known
- command duration and sync wait duration

This will help diagnose FujiNet boot failures and high-speed SIO timing issues.

## 10.5 NETStream Status

Add NETStream status fields based on current `src/netsio.c` and the NETStream handler API:

- stream active
- pending enable
- Fuji enable accepted
- motor asserted
- POKEY config compatible
- current baud if observable from existing Atari800/POKEY state
- requested flags if observable from command frame
- final flags if reported by handler/test app
- transport UDP/TCP
- REGISTER enabled
- UDP sequencing enabled
- requested nominal baud
- final AUDF3/AUDF4
- detected NTSC/PAL
- RX bytes available
- sticky status if the handler exposes it to test app/debug port

If the emulator cannot know handler-level fields directly, document which fields come from emulator NetSIO state and which must be reported by the Atari-side test app through debug output.

---

# Phase 11: `run_until` and Agent Automation Primitives

Purpose: give agents a deterministic way to wait for expected app behavior.

Progress checklist:

- [ ] Add MCP-level `atari_run_until`.
- [ ] Add frame, memory, CPU, screen text, debug output, FujiNet log, and NetSIO trace predicates.
- [ ] Add hard wall-clock and frame limits.
- [ ] Add timeout diagnostics.
- [ ] Add optional screenshot/debug/trace tails on success or failure.

## 11.1 `atari_run_until`

Suggested input:

```json
{
  "predicates": [
    {"type":"screen_contains","text":"READY"},
    {"type":"memory_equals","addr":1536,"data":[1,2,3]},
    {"type":"debug_contains","text":"BOOT OK"}
  ],
  "mode": "any",
  "max_frames": 600,
  "max_ms_wallclock": 10000,
  "poll_interval_frames": 5,
  "stable_for_frames": 2,
  "on_timeout": "pause",
  "include_screenshot": true,
  "include_debug_tail": 100,
  "include_netsio_trace_tail": 100,
  "include_fujinet_log_tail": 100
}
```

Supported predicates:

- `frames_elapsed`
- `screen_contains`
- `screen_not_contains`
- `memory_equals`
- `memory_changed`
- `pc_equals`
- `pc_in_range`
- `debug_contains`
- `fujinet_log_contains`
- `netsio_event`
- `breakpoint_hit`
- `emulator_exited`

Rules:

- Always require `max_frames` or `max_ms_wallclock`; preferably both.
- On timeout, include diagnostics.
- Poll by running small frame batches instead of busy waiting.
- Do not hang indefinitely waiting on screen text.

## 11.2 Diagnostic Response

On failure, include:

- reason
- elapsed frames
- elapsed wall-clock ms
- last CPU state
- last screen text if available
- screenshot path if requested
- debug output tail
- FujiNet log tail
- NetSIO trace tail
- session status

---

# Phase 12: Screen and Input Improvements

Purpose: make screen reading and input generation more reliable for AI-driven tests.

Progress checklist:

- [ ] Rename or clarify `screen_raw` as framebuffer raw/base64.
- [ ] Add display-list-aware `screen_text` v1.
- [ ] Add confidence and unsupported-mode reporting.
- [ ] Add key down/up/typed-string helpers.
- [ ] Add input state readback.
- [ ] Add console key press-duration helper.

## 12.1 Screen Text v1

Start intentionally limited.

Suggested C command:

- `screen.text`

Suggested MCP tool:

- `atari_screen_text`

v1 behavior:

- Support OS text modes and simple ANTIC text modes only.
- Return raw screen codes and best-effort ATASCII/ASCII conversion.
- Include detected mode, screen memory address, charset address, width, height, and confidence.
- If custom charset or graphics mode is detected, return `unsupported_reason` instead of pretending confidence is high.

Suggested response:

```json
{
  "status":"ok",
  "supported":true,
  "confidence":0.93,
  "mode":"antic_2_text_40",
  "width":40,
  "height":24,
  "screen_addr":1536,
  "charset_addr":57344,
  "custom_charset":false,
  "lines":["READY"]
}
```

## 12.2 Input Helpers

Add tools for common agent input loops:

- `atari_key_down`
- `atari_key_up`
- `atari_type_text`
- `atari_press_key`
- `atari_press_console`
- `atari_input_status`

Clarify active-low behavior for console keys and joystick trigger state.

---

# Phase 13: Native Disk and Artifact Workflows

Purpose: make program/disk/image testing safe and repeatable.

Progress checklist:

- [ ] Add native disk insert/eject/status C commands.
- [ ] Add MCP wrappers.
- [ ] Add managed disk workspace.
- [ ] Add read-only by default behavior for user-supplied images.
- [ ] Add dirty/writeback policy.
- [ ] Add artifact listing helper.

## 13.1 Artifact Tools

Suggested MCP tools:

- `atari_artifact_list`
- `atari_artifact_read_text`
- `atari_artifact_info`
- `atari_artifact_delete`

Artifacts include:

- screenshots
- memory dumps
- save states
- trace exports
- generated configs
- copied disk images
- logs

## 13.2 Disk Write Policy

Default:

- source disk image is copied into session workspace
- mounted copy is read-only unless write-enabled requested
- original source is never changed

Write-enabled mode:

- require `write_enabled=true`
- report output disk path
- do not copy modifications back to source automatically
- optional explicit export/copyback tool later

---

# Phase 14: FujiNet/NetSIO Test Apps and Examples

Purpose: provide reproducible tests for the new MCP and NetSIO tools.

Progress checklist:

- [ ] Add a basic XEX/COM smoke test program.
- [ ] Add a debug-port test program.
- [ ] Add a native disk boot smoke test.
- [ ] Add FujiNet boot smoke test.
- [ ] Add NETStream speed/pacing test app integration.
- [ ] Add automated MCP tests that run these programs.

Recommended test programs:

1. `hello_debug.xex`: writes known bytes to debug port and displays simple text.
2. `screen_text.xex`: shows stable OS text for screen extraction.
3. `joystick_test.xex`: writes joystick/trigger state to memory/debug port.
4. `disk_boot.atr`: boots and writes READY marker to screen/debug port.
5. `fujinet_boot.atr`: boots through FujiNet-PC managed mount.
6. `netstream_speed.xex`: reports NETStream init flags, final AUDF3/AUDF4, bytes sent/received, error counts, and throughput.

For NETStream, use the existing handler API facts:

- jump table starts at `BASEADDR`, currently `$2800`
- `NS_InitNetstream` configures FujiNet and POKEY
- `NS_BeginStream` asserts motor and enables serial IRQs
- `NS_SendByte` / `NS_RecvByte` are main transfer calls
- `NS_GetStatus`, `NS_GetVideoStd`, `NS_GetFinalFlags`, `NS_GetFinalAUDF3`, and `NS_GetFinalAUDF4` are useful status probes

---

# Phase 15: Automated Test Matrix

Purpose: prevent regressions and give Codex a concrete finish line.

Progress checklist:

- [ ] Add unit tests for JS MCP helper functions.
- [ ] Add C-level or integration tests for AI JSON responses where practical.
- [ ] Add end-to-end MCP tests for headless startup, run, screen, memory, and cleanup.
- [ ] Add tests for no destructive process handling.
- [ ] Add FujiNet-PC sidecar tests where available.
- [ ] Add NetSIO trace/status tests.
- [ ] Add debugger tests.

## 15.1 Minimum Test Matrix

Required tests:

- command inventory: C dispatch vs documented command list
- MCP tool list matches MCP README
- invalid JSON returns structured error
- unknown command returns structured error
- unsafe output path rejected
- escaped path string returns valid JSON
- `atari_start` creates per-session runtime dir and socket
- `atari_stop` does not kill unrelated Atari800 process
- headless Xvfb start, screenshot, stop, cleanup
- visible mode fails clearly without display
- `atari_run` runs a bounded number of frames
- `atari_screen` returns screen data
- `atari_peek` length cap works
- `atari_poke` writes expected bytes in RAM
- `atari_debug_enable`/`atari_debug_read` works with test app
- `run_until` success predicate works
- `run_until` timeout returns diagnostics
- basic true instruction step changes PC or state predictably
- breakpoint hit pauses without entering interactive monitor
- FujiNet sidecar starts on non-default port
- managed `fnconfig.ini` is generated in isolated directory
- NetSIO status reports port/connection state
- NetSIO trace ring caps output and reports dropped records

---

# Phase 16: Documentation and Packaging

Purpose: make the interface usable by Codex, Claude Code, and humans.

Progress checklist:

- [ ] Update main README.
- [ ] Update MCP README.
- [ ] Add `README.AI.md` / `AGENT_CONTRACT.md`.
- [ ] Add examples for agent workflows.
- [ ] Document build flags for AI/debug builds.
- [ ] Document supported host environments.
- [ ] Add separate manual bundle packaging script outside Atari800 Makefile/autotools flow.
- [ ] Add `start-mcp.sh` and bundle README templates.
- [ ] Package MCP server with clear setup instructions.
- [ ] Generate bundle manifest and platform-specific tarball names.

## 16.1 AI Build Recommendations

Recommended basic AI/debug build:

```sh
./configure   --with-video=sdl   --with-sound=sdl   --enable-ai-interface   --enable-monitorbreak   --enable-monitorbreakpoints   --enable-monitortrace   --enable-monitorprofile
```

Notes:

- Current `build_ai.sh` uses SDL 1.2 because this branch comments that SDL2 has broken keyboard trigger behavior. It enables both the AI interface and NetSIO for the MCP-targeted build.
- `--enable-netsio` is required for the MCP-targeted build because FujiNet/NetSIO is a major supported workflow.
- Full rich breakpoint support requires `--enable-monitorbreakpoints`.
- Trace/profile tools require their corresponding monitor flags.
- If trace/profile are enabled, reads must be bounded.
- Monitor flags may affect performance; document AI/debug build vs fast runtime build.

## 16.2 README.AI Contents

Include:

- session lifecycle
- start/preflight examples
- frame run loop
- screen reading limitations
- input examples
- native disk vs FujiNet disk workflows
- FujiNet sidecar boot example
- NetSIO trace/status examples
- debugger step/breakpoint examples
- `run_until` examples
- artifact directory rules
- path safety rules
- troubleshooting section for SSH/tmux/Xvfb

## 16.3 Manual Runtime Bundle Packaging

Provide an optional tarball packaging path for users who want to download and run the MCP server without installing Atari800 build tools.

Important constraint:

- Keep bundle creation outside Atari800's normal `Makefile.am` / autotools flow.
- Do not modify the default Atari800 Makefile for packaging.
- Use separate scripts so this branch stays easier to rebase against upstream Atari800.

Suggested files:

- `tools/package_mcp_bundle.sh`
- `tools/package_mcp_bundle.py` if Python is more practical for manifest generation
- `tools/templates/start-mcp.sh`
- `tools/templates/README.bundle.md`

Packaging inputs:

- A previously built `src/atari800` binary with AI interface enabled.
- `mcp-server/` JavaScript files.
- Production MCP server dependencies, installed separately from the Atari800 build.
- `README.AI.md`, MCP README, and license files.
- Optional helper scripts for FujiNet fetch/config workflows.

Suggested bundle layout:

- `bin/atari800`
- `bin/start-mcp.sh`
- `mcp-server/index.js`
- `mcp-server/package.json`
- `mcp-server/node_modules/`
- `share/README.AI.md`
- `share/README.bundle.md`
- `runtime/artifacts/`
- `runtime/logs/`
- `runtime/sockets/`

Manual start behavior:

- User unpacks the tarball and runs `bin/start-mcp.sh`.
- The script resolves paths relative to the bundle root.
- The script sets environment variables such as `ATARI800_PATH`, runtime directory, artifact directory, and socket directory.
- The script runs MCP over stdio by default so clients can launch it directly from MCP config.
- The script performs preflight checks and reports missing runtime dependencies, especially `Xvfb`, SDL runtime libraries, Node.js if not bundled, and FujiNet-PC if a FujiNet workflow is requested.

Bundling policy:

- Start with manual-start tarballs only; service installation can be considered later.
- Do not bundle FujiNet-PC initially. Use the planned FujiNet fetch tooling so the MCP can get the correct nightly for the host.
- Do not bundle Xvfb. Detect it and show OS-specific install hints.
- Decide explicitly whether Node.js is bundled. If Node.js is not bundled, document the required Node version and fail preflight clearly when missing.
- Produce platform-specific tarballs named like `atari800-mcp-<version>-<os>-<arch>.tar.gz`.
- Include a machine-readable bundle manifest with build date, git revision, Atari800 version/configure flags, MCP version, target OS/arch, and included dependency versions.

---

# Phase 17: Final Hardening and Security Review

Purpose: review attack surface and accidental destructive behavior before treating MCP as reliable.

Progress checklist:

- [ ] Review all file reads/writes.
- [ ] Review all process spawning.
- [ ] Review path normalization.
- [ ] Review regex filters for ReDoS or unbounded work.
- [ ] Review log ring buffer bounds.
- [ ] Review trace/profile memory bounds.
- [ ] Review all tools for explicit unsafe modes.
- [ ] Confirm no shell escape path is exposed.
- [ ] Confirm `pkill` or global process cleanup is not used by default.

## 17.1 Safety Checklist

- All spawned processes are tracked.
- All cleanup is scoped to session-owned resources.
- All output files go to managed directories unless explicitly allowed.
- All long-running operations have wall-clock timeout.
- All ring buffers have caps and dropped counts.
- All errors are structured.
- All capabilities are discoverable.
- Required compile-time features such as AI interface and NetSIO fail clearly when unavailable; optional debugger/trace/profile features report unavailable capabilities clearly.

---

# Suggested Implementation Order for Codex

1. Add inventory/check script and minimal `README.AI.md` current-state contract.
2. Add `hello`/`capabilities`, structured errors, JSON escaping helpers, and input validation.
3. Add session/runtime directory model in MCP and per-session AI socket usage.
4. Add per-session video socket path support.
5. Remove destructive `pkill` behavior and add tracked-process cleanup.
6. Fix current README/header/MCP README mismatches.
7. Expose existing safe C commands through MCP.
8. Add `atari_preflight` and robust headless `atari_start` with Xvfb.
9. Add basic debugger: status, show state, true instruction step, continue, simple PC/BRK break.
10. Add native disk insert/eject/status.
11. Add FujiNet-PC sidecar version selection, pinned/local path support, port allocation, logs.
12. Add managed `fnconfig.ini` and FujiNet mount/boot workflow.
13. Add NetSIO status and trace ring.
14. Add `run_until` with hard timeouts and diagnostics.
15. Add screen text v1 and improved input helpers.
16. Add rich monitor breakpoint/read-only debugger views.
17. Add trace/profile/coverage behind capability checks.
18. Add separate manual bundle packaging script, bundle README, manifest, and final docs.

---

# Acceptance Criteria Summary

The upgraded MCP/AI interface is acceptable when:

- Docs match actual source behavior.
- MCP tools match MCP README.
- `hello`/capabilities lets an agent discover available features.
- All errors are structured and valid JSON.
- Multiple managed sessions cannot collide on fixed sockets.
- `atari_stop` does not kill unrelated processes.
- Headless mode works over SSH/tmux with Xvfb on compatible Linux/X11 builds.
- macOS visible mode is supported when a native desktop session is available; macOS headless support is optional future work unless the selected build is proven X11-compatible.
- Visible mode fails clearly when no display exists.
- Existing C API commands are exposed through MCP.
- `step_instruction` is true CPU stepping, while frame running remains separate.
- Breakpoint hits in AI mode pause and report JSON instead of entering the interactive monitor.
- FujiNet-PC can be started as an MCP-owned sidecar on a non-default port.
- FujiNet boot workflows use managed `fnconfig.ini`, not CONFIG UI automation.
- NetSIO status and trace expose sync/ACK/credit/line state enough to diagnose boot and NETStream failures.
- `run_until` can drive common test loops without hanging indefinitely.
- A separate packaging script can produce manual-start runtime tarballs without modifying Atari800's default Makefile/autotools flow.
- Tests cover startup, cleanup, command parity, protocol errors, debugger basics, FujiNet sidecar, and NetSIO tracing.
