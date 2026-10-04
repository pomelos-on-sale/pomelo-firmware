#!/usr/bin/env bash
# Build, flash and monitor. Loads the toolchains first if this shell lacks them.
set -e
cd "$(dirname "$0")"

# `idf.py` for ESP-IDF, `cargo` for the Rust half (ninja builds it with `cargo +esp`).
# Testing the commands rather than `$IDF_PATH` matters: VS Code's ESP-IDF extension
# exports that variable without putting the tools on PATH.
command -v idf.py >/dev/null || . "${IDF_PATH:-$HOME/.espressif/v6.1/esp-idf}/export.sh"
command -v cargo >/dev/null || . "$HOME/.cargo/env"

idf.py -p "${1:-/dev/ttyACM0}" flash monitor
