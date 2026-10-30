#!/usr/bin/env bash
# Reproduce the unified performance measurement (N=100, ser + thru).
# Usage: bash scripts/reproduce-perf.sh <path-to-xs-env>
set -euo pipefail

XS_ENV="${1:-$HOME/xs-env}"
AM_HOME="${AM_HOME:-$XS_ENV/nexus-am}"
EMU="$XS_ENV/XiangShan/build/emu"
LOG="${2:-$HOME/logs/perf-n100.log}"
mkdir -p "$(dirname "$LOG")"

echo "measuring (167k cycles, ~15 min at ~190 cycles/s), log: $LOG"
"$EMU" -i "$AM_HOME/apps/cie-vdot-perf/build/cie-vdot-perf-riscv64-xs.bin" --no-diff -C 400000 > "$LOG" 2>&1

echo "== result lines =="
grep -aE '^\[(B|BV)\]' "$LOG"
echo "== 换算 =="
awk '/^\[B\] N=100/ {
  nop=$3; sub("nop=","",nop);
  split($4,g,","); sub("gat=","",g[1]);
  split($5,n,","); sub("ngt=","",n[1]);
  printf "nop=%d gat_ser=%.2f gat_thru=%.2f ngt_ser=%.2f ngt_thru=%.2f\n", nop,(g[1]-nop)/100,(g[2]-nop)/400,(n[1]-nop)/100,(n[2]-nop)/400
}' "$LOG"
awk '/^\[BV\] N=100/ { split($3,v,","); sub("vdot=","",v[1]); printf "vdot_ser=%.2f vdot_thru=%.2f\n",(v[1]-457)/100,(v[2]-457)/400 }' "$LOG"
