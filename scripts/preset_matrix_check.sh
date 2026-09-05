#!/usr/bin/env bash
# Build and test the five test presets, then build the four non-test presets, and
# report the real pass counts and warning counts. Usage: preset_matrix_check.sh
#
# Exists because a matrix result quoted in DEVELOPMENT_STATUS.md has to come from a
# run rather than from memory, and because an inline shell loop through the WSL
# wrapper gets mangled.
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

for preset in dev debug clang-dev asan tsan; do
  printf '=== %s\n' "$preset"
  warnings=$(cmake --build "build/$preset" 2>&1 | grep -c 'warning:' || true)
  printf '  warnings: %s\n' "$warnings"
  ctest --test-dir "build/$preset" --output-on-failure 2>&1 |
    grep -E '^[0-9]+% tests passed|tests failed out of' | sed 's/^/  /'
done

for preset in release bench bench-clang fuzz; do
  printf '=== %s (build only)\n' "$preset"
  warnings=$(cmake --build "build/$preset" 2>&1 | grep -c 'warning:' || true)
  if cmake --build "build/$preset" >/dev/null 2>&1; then
    printf '  built, warnings: %s\n' "$warnings"
  else
    printf '  BUILD FAILED\n'
  fi
done
