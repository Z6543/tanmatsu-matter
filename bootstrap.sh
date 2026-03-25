#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "==> Installing ESP-IDF toolchain..."
"$SCRIPT_DIR/esp-idf/install.sh" all

echo "==> Installing esp-matter dependencies..."
"$SCRIPT_DIR/esp-matter/install.sh"

echo ""
echo "Bootstrap complete. To build, run:"
echo "  zsh build.sh"
