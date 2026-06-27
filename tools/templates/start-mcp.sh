#!/usr/bin/env sh
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export ATARI800_PATH="${ATARI800_PATH:-$HERE/bin/atari800}"
exec node "$HERE/mcp-server/index.js" "$@"
