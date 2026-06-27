# Atari800 AI/MCP Agent Guide

This branch exposes Atari800 through the MCP server in `mcp-server/index.js`. Use the MCP tools instead of driving the SDL window or FujiNet CONFIG UI.

## Start Here

1. Run the MCP server with `node mcp-server/index.js`.
2. Call `atari_preflight` before starting a session.
3. Use headless mode unless the user explicitly requests a visible window.
4. Call `atari_stop` when finished; it stops only processes owned by the current MCP session.

The complete low-level AI command inventory and capability notes are in `AGENT_CONTRACT.md`. MCP tool summaries are in `mcp-server/README.md`.

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
