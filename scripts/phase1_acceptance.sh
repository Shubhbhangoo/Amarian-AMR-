#!/usr/bin/env bash
# Phase 1 acceptance: two independent nodes end on the same chain, one of them having mined
# it and the other having judged every block by its own rules.
#
# The spec's acceptance criterion for Phase 1 is "two local nodes independently validate the
# same chain". There is no network layer yet — that is Phase 4 — so the blocks travel through
# a flat file, which is the weaker transport and therefore the honest one to test with: a file
# says nothing about who produced it and carries no authority of its own. Node B is handed
# bytes and nothing else, and reaches node A's tip only because its own consensus code agrees.
#
# Also checked, because a node that cannot refuse is not validating: a regtest block file
# offered to a testnet node, and a regtest data directory opened as testnet.
#
# Data directories live in the WSL filesystem rather than under /mnt, because RocksDB's file
# locking is being tested here only incidentally and drvfs is not the thing under test. Logs
# are copied to the Windows temp path at the end so they outlive the VM.

set -u

ROOT="/mnt/e/project Amarian"
NODE="$ROOT/build/dev/src/amariand"
WORK="/tmp/amarian_phase1"
KEEP="/mnt/c/Users/Shubhkarman/AppData/Local/Temp/amarian_phase1"

A="$WORK/node_a"
B="$WORK/node_b"
WRONG="$WORK/node_testnet"
FILE="$WORK/blocks.dat"

# A version-1 commitment lock over a 32-byte program: `01`, CompactSize `20`, then the bytes.
# Not derived from a key, and it does not need to be — nothing in Phase 1 spends a coinbase,
# and a payout the node cannot spend is still a payout consensus must account for.
PAYOUT="0120$(printf '11%.0s' $(seq 32))"

rm -rf "$WORK"
mkdir -p "$WORK"

fail=0
step() { printf '\n=== %s ===\n' "$1"; }
check() {
    if [ "$1" = "0" ]; then
        printf 'ok    %s\n' "$2"
    else
        printf 'FAIL  %s\n' "$2"
        fail=1
    fi
}

# The last "tip height N HASH (work W)" a run logged, as "N HASH".
tip_of() { sed -n 's/.*tip height \([0-9][0-9]*\) \([0-9a-f][0-9a-f]*\) .*/\1 \2/p' "$1" | tail -1; }

step "node A mines five blocks and exports them"
"$NODE" --chain regtest --datadir "$A" --log-level info \
    --generate 5 --payout "$PAYOUT" --export-blocks "$FILE" >"$WORK/a_mine.log" 2>&1
check $? "node A run"
cat "$WORK/a_mine.log"
TIP_A="$(tip_of "$WORK/a_mine.log")"

step "node B imports the file, having never seen node A"
"$NODE" --chain regtest --datadir "$B" --log-level info \
    --import-blocks "$FILE" >"$WORK/b_import.log" 2>&1
check $? "node B run"
cat "$WORK/b_import.log"
TIP_B="$(tip_of "$WORK/b_import.log")"

step "the two tips"
printf 'A: %s\nB: %s\n' "$TIP_A" "$TIP_B"
[ -n "$TIP_A" ] && [ "$TIP_A" = "$TIP_B" ]
check $? "two independent nodes on the same tip"
[ "${TIP_A%% *}" = "5" ]
check $? "that tip is height 5"

step "both nodes restart and still hold the chain"
"$NODE" --chain regtest --datadir "$A" --log-level info >"$WORK/a_restart.log" 2>&1
check $? "node A restart"
"$NODE" --chain regtest --datadir "$B" --log-level info >"$WORK/b_restart.log" 2>&1
check $? "node B restart"
[ "$(tip_of "$WORK/a_restart.log")" = "$TIP_A" ]
check $? "node A's tip survived the restart"
[ "$(tip_of "$WORK/b_restart.log")" = "$TIP_A" ]
check $? "node B's tip survived the restart"
cat "$WORK/a_restart.log"

step "a testnet node refuses a regtest block file"
"$NODE" --chain testnet --datadir "$WRONG" --log-level info \
    --import-blocks "$FILE" >"$WORK/wrong_file.log" 2>&1
rc=$?
[ "$rc" -ne 0 ]
check $? "the run failed"
grep -q "is not a testnet block file" "$WORK/wrong_file.log"
check $? "and failed on the magic, not on a block rule"
cat "$WORK/wrong_file.log"

step "a testnet node refuses a regtest data directory"
"$NODE" --chain testnet --datadir "$A" --log-level info >"$WORK/wrong_dir.log" 2>&1
rc=$?
[ "$rc" -ne 0 ]
check $? "the run failed"
cat "$WORK/wrong_dir.log"

mkdir -p "$KEEP"
cp "$WORK"/*.log "$KEEP/" 2>/dev/null

printf '\n'
if [ "$fail" = "0" ]; then
    printf 'PHASE 1 ACCEPTANCE: pass\n'
else
    printf 'PHASE 1 ACCEPTANCE: fail\n'
fi
exit "$fail"
