#!/usr/bin/env bash
# clang-format compliance for every C++ source in the tree.
#
# This used to carry a hand-written list of the Phase 1 files, which meant a new
# source was unformatted-by-default until someone remembered to add it —
# fuzz/standalone_main.cpp was written and very nearly committed that way. The list
# is now derived from the tree, so the failure mode is gone.
#
# Search is scoped to the directories that hold our code. build/ is excluded
# because it contains generated sources and vendored dependency copies, and
# reformatting those is neither our business nor a signal.
#
# Usage: check_format_phase1.sh [--fix]
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

CF="$(command -v clang-format-21 || command -v clang-format)"
if [ -z "$CF" ]; then
  echo "MISSING clang-format"
  exit 1
fi
echo "clang-format: $CF"
"$CF" --version

mapfile -t FILES < <(find include src tests fuzz bench apps \
    -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' \) \
    -print 2>/dev/null | sort)

echo "checking ${#FILES[@]} files"

if [ "${1:-}" = "--fix" ]; then
  "$CF" -i "${FILES[@]}"
  echo "reformatted in place"
  exit 0
fi

echo "--- files needing reformat ---"
rc=0
for f in "${FILES[@]}"; do
  if ! "$CF" --dry-run --Werror "$f" >/dev/null 2>&1; then
    echo "NEEDS-FORMAT: $f"
    rc=1
  fi
done
[ "$rc" -eq 0 ] && echo "(none)"
echo "--- done ---"
exit "$rc"
