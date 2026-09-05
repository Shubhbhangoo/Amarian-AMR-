#!/usr/bin/env bash
# Count the tests in each unit-test binary, from the binaries rather than by
# estimate. Usage: count_tests.sh [preset]
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1
PRESET="${1:-dev}"

total=0
for bin in build/"$PRESET"/tests/unit/test_*; do
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
