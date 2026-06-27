# Atari800 AI/MCP Agent Guide

This branch exposes Atari800 through the MCP server in `mcp-server/index.js`. Use the MCP tools instead of driving the SDL window or FujiNet CONFIG UI.

## Start Here

1. Run the MCP server with `node mcp-server/index.js`.
2. Call `atari_preflight` before starting a session.
3. Use headless mode unless the user explicitly requests a visible window.
4. Call `atari_stop` when finished; it stops only processes owned by the current MCP session.

The complete low-level AI command inventory and capability notes are in `AGENT_CONTRACT.md`. MCP tool summaries are in `mcp-server/README.md`.

## Build Flags

Recommended AI/debug build:

```sh
./configure --with-video=sdl --with-sound=sdl \
  --enable-ai-interface --enable-netsio \
  --enable-monitorbreak --enable-monitorbreakpoints \
  --enable-monitortrace --enable-monitorprofile
make -j4
```

`build_ai.sh` is the preferred local shortcut. NetSIO is required for FujiNet workflows. Monitor breakpoint, trace, and profile flags enable richer debugger tools but can carry runtime cost; use a separate fast runtime build if you do not need debugger instrumentation.

Supported full-test host: Linux, Node.js 18+, SDL 1.2 runtime libraries, Xvfb, and a real FujiNet-PC archive or unpacked directory.

## FujiNet Selection

Call `fujinet_list_versions` first. Select an existing archive or directory with `fujinet_set_local_path`, or use `fujinet_select_version` for a local pinned version. `fujinet_fetch_latest` is optional and requires network access.

The MCP copies or extracts FujiNet-PC into its managed runtime. It never modifies the selected archive or user installation.

## Preferred FujiNet Boot

The shortest deterministic workflow is:

1. `fujinet_set_local_path` with the FujiNet-PC archive or directory.
2. `fujinet_boot` with `source_path` set to the boot disk image.
3. Inspect `fujinet_mount_status`, `fujinet_status`, and `fujinet_debug_read`.
4. Use Atari tools such as `atari_run`, `atari_screen`, debugger tools, and screenshots.
5. Call `atari_stop`.

Equivalent MCP call sequence:

```text
fujinet_set_local_path(path=/home/mozzwald/fujinet-pc-ATARI)
fujinet_boot(source_path=/path/to/boot.atr, display_mode=headless)
atari_netsio_status()
atari_netsio_trace_enable()
atari_run_until(predicates=[{type:netsio_event,event:SIO_COMMAND_FRAME}], max_frames=300)
atari_stop(force=true)
```

`fujinet_boot` defaults to drive 1, a copied read-only working image, `boot_mode=1`, CONFIG disabled, headless Xvfb, and sound disabled. It starts missing managed processes, passes the allocated non-default NetSIO port to Atari800, reloads FujiNet configuration, cold-resets Atari, and waits for FujiNet to report NetSIO initialized.

## Mounting Disks

Use `fujinet_mount_disk` for drives 1 through 8.

Safe defaults:

- `read_only=true`
- `copy_to_workspace=true`
- source images are never modified
- images are copied under the managed FujiNet `SD/mcp-disks/driveN` directory
- `[MountN]` uses Host1, which is configured as the local SD host
- mounting sets `[General] boot_mode=1` and `configenabled=0`

Set `read_only=false` to test disk writes against a managed copy. Set `preserve_modified=true` to retain that copy when it is replaced, unmounted, or the session stops. Default preserved outputs are reported under the MCP persistent runtime `preserved/<session-id>` directory.

A direct source mount uses `copy_to_workspace=false`. A direct writable source mount also requires `allow_source_write=true` because it can modify the original image.

After changing mounts while processes are running, call `fujinet_remount`. It restarts only the MCP-owned FujiNet sidecar on the existing NetSIO port, waits for FujiNet to initialize NetSIO, then cold-resets Atari.

Use `fujinet_unmount_disk` to remove a drive. The response reports any preserved output path.

## Managed Configuration

Use:

- `fujinet_config_get` to read the full structured configuration, a section, or one key
- `fujinet_config_set` to update one section/key/value
- `fujinet_mount_status` to inspect mount sections and tracked working images

Writes apply only to the MCP-managed `fnconfig.ini`. Every update uses a same-directory temporary file, atomic rename, and timestamped backup. A write made while FujiNet-PC is running sets `pending_remount=true`; call `fujinet_remount` to load it.

FujiNet configuration fields follow the upstream [fnconfig.ini reference](https://github.com/FujiNetWIFI/fujinet-firmware/wiki/FujiNet-Configuration-File%3A-fnconfig.ini).

## Debugging FujiNet Apps

Use `fujinet_debug_read` with `since_seq`, `contains`, `regex`, and `stream` filters. Useful messages include:

- `### NetSIO initialized ###`
- disk selection and mount paths
- SIO command frames
- ACK/NAK and COMPLETE/ERROR responses
- network and app-key operations

Use `fujinet_debug_status` for counters and `fujinet_debug_clear` between test cases. FujiNet output is bounded; consume it incrementally during long runs.

Do not use the FujiNet CONFIG UI for automated tests. Configure mounts through the managed tools and verify behavior through screen, memory, debugger, and FujiNet debug output.

## Waiting for Behavior

Use `atari_run_until` for deterministic waits instead of open-ended frame loops. Always provide `max_frames` or `max_ms_wallclock`; providing both is preferred.

Useful predicates include:

- `screen_contains` / `screen_not_contains`
- `memory_equals` / `memory_changed`
- `pc_equals` / `pc_in_range`
- `debug_contains`
- `fujinet_log_contains`
- `netsio_event`
- `breakpoint_hit`
- `frames_elapsed`
- `emulator_exited`

Set `poll_interval_frames` to a small batch size such as 1-10 for boot and input-sensitive waits. On timeout, request diagnostics with `include_debug_tail`, `include_fujinet_log_tail`, `include_netsio_trace_tail`, and `include_screenshot` when those artifacts help explain the failure.

## Screen and Input

Use `atari_screen_text` when the program is in a simple ANTIC/OS text mode. It returns screen codes, best-effort text lines, display-list mode, screen and charset addresses, confidence, and `unsupported_reason` for custom charset or non-text display modes. Use `atari_framebuffer_raw` or `atari_screen_raw` for the rendered framebuffer base64 bytes.

For input, prefer bounded helpers: `atari_press_key`, `atari_type_text`, and `atari_press_console`. Use `atari_key_down`/`atari_key_up` only when a test needs a held key. `atari_consol` accepts natural pressed booleans; the low-level C command uses active-low console bits. `atari_input_status` reports the current key, console, joystick override, trigger override, and paddle state.

## Native Disks and Artifacts

Use `atari_disk_insert` for native Atari800 disk testing. The MCP copies the source image into the session `native-disks` workspace and mounts the copy read-only by default, so source disk images are not changed. Set `write_enabled=true` only when a test must write to the managed copy; the response reports the managed `output_path`, and no automatic copyback to the source occurs.

Use `atari_disk_status` to inspect native SIO drive state and `atari_disk_eject` to dismount a drive. Use `atari_artifact_list`, `atari_artifact_info`, `atari_artifact_read_text`, and `atari_artifact_delete` to inspect session artifacts, logs, and managed disk copies. Logs are listable but not deletable through the artifact delete tool.

## Debugger Examples

```text
atari_debugger_status()
atari_step_instruction(instructions=1)
atari_break_on_pc(addr=0x2000, enabled=true)
atari_run(frames=60)
atari_breakpoint_status()
```

Use `atari_run_until` for bounded waits:

```text
atari_run_until(
  predicates=[{type:debug_contains,text:READY}],
  max_frames=600,
  poll_interval_frames=5,
  include_debug_tail=4,
  include_screenshot=true
)
```

## Test Fixtures

Phase 14 fixtures are generated by `tests/fixtures/mcp_test_programs/generate.py`. They include XEX programs for debug-port, screen text, joystick, and NETStream probe workflows plus native/FujiNet boot ATRs. `tests/mcp_phase14_smoke.mjs` runs the generated XEX fixtures and native boot ATR by default; pass a FujiNet-PC path as the first argument or set `FUJINET_PATH` to also run the FujiNet boot ATR path.

Use `tests/mcp_phase15_matrix.mjs` for the broad MCP regression matrix. It requires a real FujiNet-PC path, defaulting to `/home/mozzwald/fujinet-pc-ATARI` or accepting `FUJINET_PATH` / the first CLI argument. It checks MCP README tool coverage, structured AI socket errors, output path policy, headless startup and cleanup, visible-mode failure without a display, run/screen/memory operations, debug-port fixtures, `run_until`, instruction stepping, breakpoint JSON stops, unrelated-process isolation, real FujiNet boot through `fujinet_boot`, managed `fnconfig.ini`, and real NetSIO status/trace traffic.

## Troubleshooting

- If headless start fails, install `Xvfb` and run `atari_preflight`.
- If running over SSH/tmux, prefer `display_mode=headless`; visible mode requires a real `DISPLAY` or native desktop.
- If FujiNet tests fail, first run `fujinet_set_local_path` with a real FujiNet-PC archive or unpacked directory, then check `fujinet_debug_read` and `atari_netsio_status`.
- If output paths are rejected, write under the session `artifact_dir` or use the MCP artifact tools instead of absolute `/tmp` paths.
- If long waits are flaky, replace open-ended loops with `atari_run_until` and bounded diagnostics.

## NetSIO Observability

Use these Atari-side tools when FujiNet boot, SIO timing, or NETStream behavior is unclear:

- `atari_netsio_status`
- `atari_netsio_trace_status`
- `atari_netsio_trace_enable`
- `atari_netsio_trace_read`
- `atari_netsio_trace_clear`
- `atari_netsio_trace_disable`

`atari_netsio_status` reports emulator-observed state: NetSIO compile/enable status, UDP port and peer, sync wait state, last sync/ACK/NAK data, timeout counters, queue and credit state, Proceed/Interrupt pin state, packet counters, and NETStream gates visible to the emulator.

Proceed and Interrupt use active-low NetSIO pin semantics. The source-verified IDs are Proceed OFF/ON `0x30/0x31`, Interrupt OFF/ON `0x40/0x41`; Proceed maps to PIA CA1 and Interrupt maps to PIA CB1.

`atari_netsio_trace_read` returns bounded decoded entries. Use `since_seq` for incremental reads and check `dropped`/`count` in trace status during long runs. Decoded entries include packet direction/type, SIO command frames, sync responses, speed changes, credit updates, send errors, and timestamps.

Some NETStream fields are handler-side only and are not visible to the emulator transport: requested/final flags, REGISTER enablement, UDP sequencing, detected video standard, and sticky app-level status. Those appear as `null` in `atari_netsio_status`; collect them from Atari-side test app telemetry or debug-port output when needed.
