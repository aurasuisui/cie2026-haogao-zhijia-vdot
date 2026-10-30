#!/usr/bin/env bash
# Build the XiangShan emulator with vdot.vv support.
# Usage: bash scripts/build-emu.sh <path-to-xs-env> [jobs]
set -euo pipefail

XS_ENV="${1:-$HOME/xs-env}"
JOBS="${2:-16}"

cd "$XS_ENV"
# shellcheck disable=SC1091
source ./env.sh
cd XiangShan

echo "building emu (-j$JOBS); mill+verilator take ~30 min, log to build/emu-vdot.log"
make emu -j"$JOBS" 2>&1 | tee build/emu-vdot.log
echo "done: $(readlink -f build/emu)"
