#!/usr/bin/env bash
# Apply the vdot.vv patches to a XiangShan / YunSuan checkout.
# Usage: bash scripts/apply-patches.sh <path-to-xs-env>
#   e.g. bash scripts/apply-patches.sh ~/xs-env
set -euo pipefail

XS_ENV="${1:-$HOME/xs-env}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XS="$XS_ENV/XiangShan"
YS="$XS_ENV/XiangShan/yunsuan"

[ -d "$XS/.git" ] || { echo "not a XiangShan checkout: $XS"; exit 1; }
[ -d "$YS/.git" ] || { echo "not a yunsuan checkout: $YS"; exit 1; }

echo "== XiangShan =="
git -C "$XS" apply --stat "$ROOT/rtl/xiangshan-vdot.patch"
git -C "$XS" apply        "$ROOT/rtl/xiangshan-vdot.patch"
echo "   applied (Vdot32.scala, VIAluFix.scala, VecDecoder.scala)"

echo "== YunSuan =="
git -C "$YS" apply --stat "$ROOT/rtl/yunsuan-vdot.patch"
git -C "$YS" apply        "$ROOT/rtl/yunsuan-vdot.patch"
echo "   applied (VialuOpcode.scala, package.scala)"

echo
echo "Done. Next: cd \$XS_ENV/XiangShan && make emu -j\$(nproc)   # ~30 min on 16 cores"
