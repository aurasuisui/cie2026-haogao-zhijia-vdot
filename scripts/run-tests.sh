#!/usr/bin/env bash
# Run the six vdot.vv functional tests (AM apps) on the emulator.
# Usage: bash scripts/run-tests.sh <path-to-xs-env> [--diff <nemu-so>]
set -euo pipefail

XS_ENV="${1:-$HOME/xs-env}"; shift || true
EMU="$XS_ENV/XiangShan/build/emu"
AM_HOME="${AM_HOME:-$XS_ENV/nexus-am}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

DIFF_ARGS=(--no-diff)
if [ "${1:-}" = "--diff" ]; then
  DIFF_ARGS=(--diff "${2:-$XS_ENV/NEMU/build/riscv64-nemu-interpreter-so}")
  echo "difftest against ${DIFF_ARGS[1]}"
fi

for app in cie-vdot cie-vdot-ill cie-vdot-n1-pos cie-vdot-n2-neg cie-vdot-n3-vl cie-vdot-n4-mask cie-vdot-n5-vstart cie-vdot-n6-illegal; do
  src="$AM_HOME/apps/$app"
  if [ ! -d "$src" ]; then
    echo "skip $app (not found in $AM_HOME/apps; copy from $ROOT/sw/$app)"
    continue
  fi
  echo "== $app =="
  make -C "$src" ARCH=riscv64-xs >/dev/null
  bin="$src/build/$app-riscv64-xs.bin"
  # cie-vdot-ill traps illegal instructions on purpose; its -C can stay small
  "$EMU" -i "$bin" "${DIFF_ARGS[@]}" -C 1500000 2>&1 | grep -aE 'checks|GOOD TRAP|Mismatch|mcause|PASS|FAIL' || true
done
