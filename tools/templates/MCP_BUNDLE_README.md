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

## FujiNet

Use a real FujiNet-PC archive or unpacked directory with `fujinet_set_local_path`. FujiNet/NetSIO tests should be run against the real sidecar, not mocks.

## Contents

- `bin/atari800`: emulator binary
- `mcp-server/`: MCP server JavaScript package
- `start-mcp.sh`: launcher that points the MCP server at the bundled emulator
- `README.AI.md`, `AGENT_CONTRACT.md`, `mcp-server/README.md`: protocol and workflow docs
- `manifest.json`: generated file list, sizes, and bundle metadata
