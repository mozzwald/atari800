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
| `atari_preflight` | Report emulator, display, Xvfb, runtime, FujiNet selection, and host dependency status. |
| `fujinet_list_versions` | List local FujiNet-PC Atari archives and optionally remote GitHub assets. |
| `fujinet_set_local_path` | Select an unpacked FujiNet-PC directory or local `.tar.gz` archive. |
| `fujinet_select_version` | Select a pinned local FujiNet-PC archive by version/tag/name substring. |
| `fujinet_fetch_latest` | Download and select the latest matching FujiNet-PC Atari asset for this host. |
| `fujinet_start` | Start an MCP-owned FujiNet-PC sidecar on a non-default NetSIO UDP port. |
| `fujinet_stop` | Stop only the MCP-owned FujiNet-PC sidecar process. |
| `fujinet_status` | Report FujiNet-PC version, process, port, config, SD/data paths, and log counters. |
| `fujinet_logs` | Read bounded FujiNet-PC stdout/stderr logs. |
| `fujinet_debug_read` | Read bounded FujiNet-PC debug output with sequence/filter support. |
| `fujinet_debug_clear` | Clear the FujiNet-PC debug output buffer. |
| `fujinet_debug_status` | Report FujiNet-PC debug output buffer counters. |
| `fujinet_config_get` | Read the managed `fnconfig.ini` as structured data or a selected value. |
| `fujinet_config_set` | Atomically update a managed config value with a timestamped backup. |
| `fujinet_mount_disk` | Copy or explicitly link an image into a managed FujiNet drive slot. |
| `fujinet_unmount_disk` | Remove a drive mount and preserve its working image when requested. |
| `fujinet_mount_status` | Report configured slots, paths, modes, and pending remount state. |
| `fujinet_boot` | Mount optionally, start missing processes, reload FujiNet, cold reset, and wait for NetSIO. |
| `fujinet_remount` | Restart the owned sidecar, reload config, cold reset Atari, and wait for NetSIO. |
| `atari_start` | Start an MCP-owned emulator session. |
| `atari_stop` | Stop the tracked session; accepts `force` and `cleanup_runtime_dir`. |
| `atari_status` | Report session state, launch details, sockets, and bounded logs. |
| `atari_logs` | Read bounded emulator/Xvfb startup logs. |
| `atari_load` | Load an executable-style program through the Atari800 BIN loader. |
| `atari_disk_insert` | Copy a disk image into the session workspace and mount it read-only by default. |
| `atari_disk_eject` | Eject a native Atari disk drive. |
| `atari_disk_status` | Report native disk drive state and managed workspace copies. |
| `atari_artifact_list` | List session artifact, log, and native disk workspace files. |
| `atari_artifact_info` | Get metadata for one session artifact. |
| `atari_artifact_read_text` | Read a bounded UTF-8 text artifact. |
| `atari_artifact_delete` | Delete an artifact from a deletable session root. |
| `atari_run` | Run for N frame-loop frames. |
| `atari_run_until` | Run bounded frame batches until predicates match, with diagnostics on timeout. |
| `atari_frame_step` | Run N frame-loop ticks, then pause. |
| `atari_pause` | Pause emulation. |
| `atari_screen` | Get the approximate 40x24 ASCII screen. |
| `atari_screen_text` | Read simple ANTIC text memory with confidence and unsupported-mode reporting. |
| `atari_screen_raw` | Get rendered framebuffer bytes as base64 data. |
| `atari_framebuffer_raw` | Explicit rendered framebuffer base64 alias. |
| `atari_screenshot` | Save a screenshot to the managed artifact directory or an explicit safe path. |
| `atari_joystick` | Set joystick direction/fire/port. |
| `atari_key` | Press a supported key name through AKEY mapping. |
| `atari_key_down` | Hold a supported key name down. |
| `atari_key_up` | Release all AI key state. |
| `atari_press_key` | Press a supported key for a bounded number of frames, then release it. |
| `atari_type_text` | Type supported text one key press at a time. |
| `atari_key_release` | Release all AI key state. |
| `atari_paddle` | Set paddle position. |
| `atari_consol` | Set natural console pressed booleans; the C protocol underneath is active-low. |
| `atari_press_console` | Press one console key for a bounded number of frames, then release it. |
| `atari_input_status` | Read keyboard, console, joystick override, trigger, and paddle input state. |
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
| `atari_netsio_status` | Report emulator-side NetSIO, SIO, queue, sync, ACK/NAK, and NETStream status. |
| `atari_netsio_trace_status` | Report NetSIO trace ring enabled/count/drop state. |
| `atari_netsio_trace_read` | Read bounded decoded NetSIO/SIO trace entries. |
| `atari_netsio_trace_clear` | Clear the NetSIO trace ring. |
| `atari_netsio_trace_enable` | Enable NetSIO trace capture. |
| `atari_netsio_trace_disable` | Disable NetSIO trace capture. |

## Example Workflow

1. Start emulator: `atari_start` with optional `program`.
2. Check session and capabilities: `atari_status`.
3. Run frames: `atari_run` with `frames`.
4. Inspect output: `atari_screen`, `atari_cpu`, `atari_peek`.
5. Stop session: `atari_stop`.

## Native Disks and Artifacts

Use `atari_disk_insert` for native Atari800 disk images. The MCP copies the source image into the session `native-disks` workspace and mounts that copy read-only unless `write_enabled=true` is requested. Writable mounts report the managed `output_path`; source images are not modified automatically.

Use `atari_artifact_list`, `atari_artifact_info`, `atari_artifact_read_text`, and `atari_artifact_delete` for files under the session artifact root and managed native disk workspace. Log files are listable but not deletable through the artifact delete tool.

## FujiNet Workflow

Use `fujinet_list_versions` first. If a matching local archive exists, use `fujinet_select_version`; otherwise use `fujinet_set_local_path` for an unpacked directory or archive, or `fujinet_fetch_latest` to download the matching Atari asset.

`fujinet_start` extracts/copies FujiNet-PC into the managed runtime directory, writes the selected non-default NetSIO UDP port into `fnconfig.ini` under `[BOIP] port=`, and starts FujiNet-PC with stdout/stderr captured. When FujiNet-PC is running, `atari_start` automatically adds `-netsio <port>` unless `netsio` is explicitly set to `false`.

When supplied by the package, `run-fujinet` handles FujiNet-PC's exit-75 reboot requests while remaining under MCP lifecycle control.

Use `fujinet_debug_read` or `fujinet_logs` to inspect FujiNet-PC debug output while testing FujiNet-enabled apps. Use `fujinet_stop` to stop only the MCP-owned FujiNet-PC process; `atari_stop` stops the whole managed session.


For deterministic disk boot, use `fujinet_boot` with `source_path`. The default copies the image into the managed SD workspace, mounts it read-only on D1, disables CONFIG boot, starts both processes headlessly, reloads FujiNet configuration, and cold-resets Atari. Use `fujinet_remount` after config or mount changes made during a running session.

See `README.AI.md` for agent-oriented FujiNet safety, preservation, and debugging workflows.

## NetSIO Observability

Use `atari_netsio_status` after starting Atari800 with NetSIO enabled. It reports emulator-side state: compile/enable status, UDP port and peer, sync wait state, last ACK/NAK byte, timeout counters, queue capacity/availability, credit status, Proceed/Interrupt pin state, packet counters, and emulator-observable NETStream gates.

Use `atari_netsio_trace_enable`, `atari_netsio_trace_read`, and `atari_netsio_trace_clear` around FujiNet boot or NETStream workflows. Trace entries are bounded and include decoded SIO command frames, sync responses, speed changes, credit updates, send errors, timestamps, sequence numbers, and dropped-entry accounting.

Handler-only NETStream values such as requested/final flags, REGISTER enablement, UDP sequencing, and detected video standard are not directly observable from the emulator transport. Those fields are reported as `null` with a note; use Atari-side app/debug-port telemetry when those values are required.

## Run Until

Use `atari_run_until` instead of hand-written polling loops. It requires at least one of `max_frames` or `max_ms_wallclock`, runs small frame batches, and supports `any` or `all` predicate mode.

Supported predicates are `frames_elapsed`, `screen_contains`, `screen_not_contains`, `memory_equals`, `memory_changed`, `pc_equals`, `pc_in_range`, `debug_contains`, `fujinet_log_contains`, `netsio_event`, `breakpoint_hit`, and `emulator_exited`.

On timeout it can pause the emulator and include CPU/screen/session diagnostics plus optional screenshot, debug tail, FujiNet log tail, and NetSIO trace tail.

MCP wrappers are intentionally narrower than the C socket API. See `AGENT_CONTRACT.md` for the full C command inventory and known gaps.
