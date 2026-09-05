#!/usr/bin/env bash
# Mutation test for the three CVE-2012-2459 defences in the Merkle tree.
#
# A passing test on correct code proves nothing about whether the test would
# catch the bug. Phase 9's acceptance criterion is that every threat-model row
# has a test that fails when the defence is removed, so establish that here for
# the defence that already exists rather than asserting it in a document.
#
# Each arm removes exactly one defence, rebuilds, and records which Merkle tests
# fail. The source file is restored from a copy outside the repository — not with
# git — because git checkout has already cost this project unstaged work once.
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

SRC=src/primitives/merkle.cpp
SAVE=/mnt/c/Users/Shubhkarman/AppData/Local/Temp/amarian_mutate
mkdir -p "$SAVE"
cp "$SRC" "$SAVE/merkle.cpp.orig" || exit 1
trap 'cp "$SAVE/merkle.cpp.orig" "$SRC"; cmake --build build/dev --target test_primitives >/dev/null 2>&1' EXIT

arm() {
  local name="$1"; shift
  cp "$SAVE/merkle.cpp.orig" "$SRC"
  "$@" || { echo "  $name: PATCH FAILED"; return; }
  if ! cmake --build build/dev --target test_primitives >"$SAVE/$name.build" 2>&1; then
    echo "  $name: BUILD FAILED (see $SAVE/$name.build)"
    return
  fi
  local out="$SAVE/$name.test"
  ./build/dev/tests/unit/test_primitives --gtest_filter='Merkle.*' >"$out" 2>&1
  local rc=$?
  local failed
  failed=$(grep -E "^\[  FAILED  \] Merkle\." "$out" | sed 's/^\[  FAILED  \] //;s/ .*//' | sort -u | tr '\n' ' ')
  printf '  %-26s exit=%-3s caught_by: %s\n' "$name" "$rc" "${failed:-NOTHING — DEFENCE UNTESTED}"
}

# Defence 1: odd nodes are promoted unchanged. Removing it means duplicating the
# last node, which is Bitcoin's rule and the bug itself.
mutate_duplicate() {
  python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = "            next.push_back(level.back());"
new = "            next.push_back(MerkleBranch(level.back(), level.back()));"
assert s.count(old) == 1, f"promotion line not found exactly once in {p}"
open(p, 'w', encoding='utf-8', newline='\n').write(s.replace(old, new))
PY
}

# Defence 2: leaves and branches are hashed under different tags. Removing it
# lets an inner node be presented as a leaf.
mutate_same_tag() {
  python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = 'constexpr std::string_view MERKLE_BRANCH_TAG = "Amarian/MerkleBranch";'
new = 'constexpr std::string_view MERKLE_BRANCH_TAG = "Amarian/MerkleLeaf";'
assert s.count(old) == 1, f"branch tag not found exactly once in {p}"
open(p, 'w', encoding='utf-8', newline='\n').write(s.replace(old, new))
PY
}

# Defence 3: the leaf count is committed in the root. Removing it means the root
# is the top node hashed under the root tag with no count.
mutate_no_count() {
  python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8').read()
old = """    Writer root_preimage(CompactSizeLen(wtxids.size()) + Hash256::SIZE);
    root_preimage.WriteCompactSize(wtxids.size());
    root_preimage.WriteHash256(level.front());"""
new = """    Writer root_preimage(Hash256::SIZE);
    root_preimage.WriteHash256(level.front());"""
assert s.count(old) == 1, f"root preimage block not found exactly once in {p}"
open(p, 'w', encoding='utf-8', newline='\n').write(s.replace(old, new))
PY
}

echo "baseline (unmutated):"
cp "$SAVE/merkle.cpp.orig" "$SRC"
cmake --build build/dev --target test_primitives >/dev/null 2>&1
./build/dev/tests/unit/test_primitives --gtest_filter='Merkle.*' 2>&1 | tail -1 | sed 's/^/  /'

echo "mutants:"
arm duplicate-odd-node   mutate_duplicate
arm branch-tag-eq-leaf   mutate_same_tag
arm drop-count-commit    mutate_no_count
echo "(source restored from $SAVE/merkle.cpp.orig on exit)"
