#!/usr/bin/env bash
# Refresh every preset's test discovery, then run the nine-preset matrix.
#
# `cmake --build` re-runs the configure step by itself when CMakeLists.txt is newer than the
# Ninja cache, but nothing invalidates the `*_tests.cmake` files that DISCOVERY_MODE PRE_TEST
# writes to cache each target's discovered test list. A newly added test target, or a changed
# LABELS property, is invisible until those are gone.
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

for preset in dev debug clang-dev asan tsan; do
  cmake --preset "$preset" >/dev/null 2>&1
  find "build/$preset/tests" -name '*_tests.cmake' -delete 2>/dev/null
done

exec bash scripts/preset_matrix_check.sh
