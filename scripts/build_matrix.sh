#!/usr/bin/env bash
# Build and test every preset in the matrix, reporting one line per preset.
#
# Logs go under the Windows temp directory, not /tmp: the WSL VM stops when idle
# and takes /tmp with it, so a log written by a long backgrounded run is not
# reliably there afterwards. That has already silently discarded one run's output.
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

LOGS=/mnt/c/Users/Shubhkarman/AppData/Local/Temp/amarian_matrix
mkdir -p "$LOGS"

PRESETS=("$@")
[ "${#PRESETS[@]}" -eq 0 ] && PRESETS=(dev debug clang-dev asan tsan)

declare -a SUMMARY=()

for p in "${PRESETS[@]}"; do
  echo "############ $p ############"
  if ! cmake --preset "$p" >"$LOGS/cfg_$p.log" 2>&1; then
    echo "  configure FAILED"; tail -20 "$LOGS/cfg_$p.log"
    SUMMARY+=("$p: CONFIGURE FAILED"); continue
  fi
  if ! cmake --build "build/$p" >"$LOGS/bld_$p.log" 2>&1; then
    echo "  build FAILED"; grep -E "error:|Error" "$LOGS/bld_$p.log" | head -20
    SUMMARY+=("$p: BUILD FAILED"); continue
  fi
  warns=$(grep -cE " warning:" "$LOGS/bld_$p.log")
  # Not every preset defines tests — release and the bench presets build only.
  if ! grep -q '"'"$p"'"' CMakePresets.json || ! ctest --preset "$p" --show-only >/dev/null 2>&1; then
    echo "  built, no test preset"
    SUMMARY+=("$p: built, no test preset (compiler warnings: $warns)"); continue
  fi
  ctest --preset "$p" >"$LOGS/tst_$p.log" 2>&1
  rc=$?
  line=$(grep -E "tests passed|tests failed" "$LOGS/tst_$p.log" | tail -1)
  if [ "$rc" -ne 0 ]; then
    echo "  tests FAILED"
    grep -E "\(Failed\)|\(Subprocess aborted\)|error" "$LOGS/tst_$p.log" | head -20
    SUMMARY+=("$p: TESTS FAILED — $line (compiler warnings: $warns)")
  else
    echo "  $line"
    SUMMARY+=("$p: $line (compiler warnings: $warns)")
  fi
done

echo
echo "================ MATRIX SUMMARY ================"
for s in "${SUMMARY[@]}"; do echo "  $s"; done
echo "logs: $LOGS"
