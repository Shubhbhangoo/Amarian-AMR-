#!/usr/bin/env bash
# Count the tests in each test binary, from the binaries rather than by estimate.
# Usage: count_tests.sh [preset]
#
# Globs every tier under tests/, not just unit/, so the TOTAL is comparable with what ctest
# reports. When they disagree, a target exists that some preset has not discovered — which is
# what the trailing ctest line is for.
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1
PRESET="${1:-dev}"

total=0
for bin in build/"$PRESET"/tests/*/test_*; do
  # -x alone also matches the gtest_discover_tests helper .cmake files, which are
  # world-executable on drvfs. Require an ELF file instead.
  [ -f "$bin" ] || continue
  case "$bin" in *.cmake) continue ;; esac
  file -b "$bin" | grep -q ELF || continue
  # A test case line is indented; a suite line ends with a dot.
  n=$("$bin" --gtest_list_tests 2>/dev/null | grep -cE "^  [A-Za-z]")
  printf '  %-20s %3d\n' "$(basename "$bin")" "$n"
  total=$((total + n))
done
printf '  %-20s %3d\n' TOTAL "$total"

echo "--- ctest sees:"
ctest --test-dir build/"$PRESET" -N 2>/dev/null | tail -1 | sed 's/^/  /'
for label in unit consensus; do
  n=$(ctest --test-dir build/"$PRESET" -N -L "$label" 2>/dev/null | tail -1 | grep -oE '[0-9]+$')
  printf '  label %-12s %s\n' "$label" "${n:-0}"
done

