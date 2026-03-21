#!/bin/zsh
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/esp-idf/export.sh"
export ESP_MATTER_PATH="$SCRIPT_DIR/esp-matter"
source "$ESP_MATTER_PATH/export.sh"
idf.py -D DEVICE=tanmatsu build
