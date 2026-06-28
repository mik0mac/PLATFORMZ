#!/usr/bin/env bash
# Rebuild every PLATFORMZ binary from scratch: desktop, web (wasm), and server.
# Fails loudly (set -e) so a broken build never leaves a stale artifact behind.
set -euo pipefail
cd "$(dirname "$0")"

# Homebrew emscripten needs a >=3.10 python; Xcode's 3.9 fails its assert.
export EMSDK_PYTHON="/opt/homebrew/opt/python@3.14/bin/python3.14"

echo "==> Desktop (platformz)"
make clean        # removes only the binary, keeps the slow IXWebSocket lib
make

echo "==> Web (web/platformz.html + .wasm)"
make clean-web
make web RAYLIB_WEB_DIR="${RAYLIB_WEB_DIR:-$HOME/raylib}"

echo "==> Server (server/gameserver)"
make -C server clean   # server Makefile ignores game-header deps, so force it
make -C server

echo "==> Done: platformz, web/platformz.html, server/gameserver"
