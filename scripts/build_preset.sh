#!/usr/bin/env bash
# Configure, build and test one preset. Usage: build_preset.sh <preset>
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

PRESET="${1:?usage: build_preset.sh <preset>}"

echo "=== configure: $PRESET ==="
cmake --preset "$PRESET" >/tmp/amarian_cfg_"$PRESET".log 2>&1
cfg=$?
if [ "$cfg" -ne 0 ]; then
  echo "CONFIGURE FAILED ($cfg); tail:"
  tail -40 /tmp/amarian_cfg_"$PRESET".log
  exit "$cfg"
fi
echo "configure ok"

echo "=== build: $PRESET ==="
cmake --build "build/$PRESET" 2>&1 | tee /tmp/amarian_build_"$PRESET".log | \
  grep -E "error|Error|warning|FAILED" | head -60
build=${PIPESTATUS[0]}
if [ "$build" -ne 0 ]; then
  echo "BUILD FAILED ($build); tail:"
  tail -60 /tmp/amarian_build_"$PRESET".log
  exit "$build"
fi
echo "build ok"
# grep exits 1 when the count is zero, which is the good case; do not let that
# become the script's exit status.
echo "warnings: $(grep -cE 'warning' /tmp/amarian_build_"$PRESET".log || true)"
