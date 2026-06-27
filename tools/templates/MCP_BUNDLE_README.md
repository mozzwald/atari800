# Atari800 MCP Runtime Bundle

This bundle contains a built Atari800 binary plus the MCP server files needed to run the AI interface without the Atari800 source tree.

## Requirements

- Linux x86_64 host matching the bundle platform
- Node.js 18 or newer
- `Xvfb` for headless display mode
- FujiNet-PC for FujiNet/NetSIO workflows

## Start

```sh
./start-mcp.sh
```

For MCP client configuration:

```json
{
  "mcpServers": {
    "atari800": {
      "command": "/path/to/bundle/start-mcp.sh"
    }
  }
}
```

For Codex `config.toml`:

```toml
[mcp_servers.atari800]
command = "/path/to/bundle/start-mcp.sh"
args = []
cwd = "/path/to/bundle"
```

Use `start-mcp.sh` for bundled installs. It sets `ATARI800_PATH` to the bundled `bin/atari800`. Direct `node mcp-server/index.js` startup is supported as a fallback, but the launcher is the intended entry point.

## Display Modes

Use `display_mode=headless` for automated runs. Use `display_mode=visible` when you want to watch the emulator. Visible mode uses inherited `DISPLAY`/`WAYLAND_DISPLAY` first, then attempts to discover a same-user Linux X11 or Wayland desktop session. Run `atari_preflight` to inspect detected display candidates. If needed, pass `display`, `wayland_display`, `xauthority`, or `xdg_runtime_dir` explicitly.

## Skill

The bundle includes `skills/atari800-mcp`, a Codex/Claude Code compatible skill for Atari 8-bit app/game development and validation through this MCP server. Copy or symlink that directory into your client skill directory if you want the agent to automatically use the Atari800 MCP testing workflow.

## FujiNet

Use a real FujiNet-PC archive or unpacked directory with `fujinet_set_local_path`. FujiNet/NetSIO tests should be run against the real sidecar, not mocks.

## Contents

- `bin/atari800`: emulator binary
- `mcp-server/`: MCP server JavaScript package with production `node_modules`
- `start-mcp.sh`: launcher that points the MCP server at the bundled emulator
- `skills/atari800-mcp`: Codex/Claude Code compatible Atari app/game testing skill
- `README.AI.md`, `AGENT_CONTRACT.md`, `mcp-server/README.md`: protocol and workflow docs
- `manifest.json`: generated file list, sizes, and bundle metadata
