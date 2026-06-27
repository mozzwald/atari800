---
name: atari800-mcp
description: Atari 8-bit app/game development and validation workflow using the Atari800 MCP server. Use when Codex or Claude Code needs to build, boot, drive, inspect, debug, or test software for Atari 400/800/XL/XE systems, including ATR disk images, executable loads, BASIC/assembly programs, gameplay/input loops, screen validation, disk I/O, and optional FujiNet/NetSIO network-device behavior.
---

# Atari800 MCP

Use the configured `atari800` MCP server as the emulator test harness for Atari 8-bit software. The MCP server owns emulator startup, runtime directories, sockets, artifacts, native disk mounts, FujiNet-PC sidecars, managed FujiNet config, NetSIO ports, and cleanup.

## Purpose

Use this skill to develop and validate Atari 8-bit apps and games. Do not treat it as an MCP server development guide unless the user explicitly asks to change the MCP server itself.

The expected loop is:

1. Build or locate the Atari target artifact.
2. Start or boot it through MCP-managed tools.
3. Drive input deterministically.
4. Wait for bounded observable conditions with MCP automation.
5. Inspect screen, artifacts, debug output, memory, CPU, disk, FujiNet logs, or NetSIO trace as needed.
6. Report pass/fail with concrete emulator evidence.

## Core Rules

- Prefer MCP tools over unmanaged emulator or FujiNet process launches.
- Use managed runtime/artifact directories and safe disk workflows.
- Use `run_until`-style bounded waits instead of open-ended sleeps.
- Use screen/input helpers first; use debugger and memory tools when behavior needs deeper diagnosis.
- For FujiNet workflows, let MCP select/fetch/start/stop FujiNet-PC and configure managed `fnconfig.ini`; do not manually run FujiNet-PC unless the user explicitly asks.
- Stop managed sessions cleanly after testing.

## References

Read `references/workflow.md` for normal Atari app/game development and test workflows.

Read `references/fujinet-testing.md` when the target app uses FujiNet, NetSIO, SIO network devices, remote hosts, network streams, or FujiNet-mounted disks.
