# FujiNet And NetSIO App Testing

Use this reference for Atari software that depends on FujiNet, NetSIO, SIO network devices, mounted FujiNet disks, NETStream behavior, or network-visible side effects.

## Ownership Model

The MCP server manages FujiNet-PC for tests:

- version listing, selection, optional fetch, and local archive/path selection
- MCP-owned FujiNet-PC sidecar process
- non-default NetSIO UDP port allocation
- Atari800 startup with the matching `-netsio` port
- managed `fnconfig.ini`
- managed SD/data paths
- bounded FujiNet stdout/stderr log capture
- MCP-scoped stop/cleanup

Do not hardcode host-specific FujiNet paths in a generic test plan. If a local FujiNet-PC path is needed, use a path supplied by the user or select/fetch it through MCP tools. The final validation should exercise the real FujiNet-PC sidecar managed by MCP.

## Deterministic Boot Flow

For FujiNet-based disks/apps, prefer MCP's managed boot/mount workflow:

1. Select or fetch a FujiNet-PC build through MCP when needed.
2. Mount the app disk/image through FujiNet MCP tools.
3. Let MCP write managed FujiNet config and SD/data paths.
4. Start FujiNet-PC as an MCP sidecar.
5. Start Atari800 with the same allocated NetSIO port.
6. Wait for FujiNet/NetSIO readiness.
7. Cold reset or remount through MCP if required.
8. Use `run_until` predicates and diagnostics to confirm app behavior.

Avoid CONFIG UI automation as the primary path; managed config and deterministic resets are the intended workflow.

## Observability

Use both Atari-side and FujiNet-side evidence:

- Atari screen text or screenshots after boot/network actions
- debug output markers from the app when available
- `fujinet_status`, logs, and debug reads for sidecar state
- `fujinet_mount_status` and config tools for mounted images
- NetSIO status for port, connection, counters, sync/ACK/NAK, and handler-visible boundaries
- NetSIO trace reads for decoded SIO commands, Fuji device commands, and transport timing

For NETStream apps, expect some handler-level fields to be available only from the Atari-side test app or debug output. Use emulator-observed NetSIO fields for transport diagnosis.

## Disk Safety

- Default to copied/read-only managed mounts.
- Use write-enabled mode only when testing app writes.
- Preserve and report managed output paths; do not mutate the original source disk unless explicitly requested.
- Never overwrite unmanaged FujiNet config.

## Reporting

Report:

- FujiNet selection method: fetched, pinned version, or user-supplied local path
- mounted disk/image and boot mode
- allocated NetSIO port/status if relevant
- Atari-visible result
- FujiNet log or NetSIO trace evidence for failures
- whether the real MCP-managed FujiNet-PC sidecar was used
