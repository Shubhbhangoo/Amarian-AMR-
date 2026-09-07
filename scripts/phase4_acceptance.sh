#!/usr/bin/env bash
# Phase 4 acceptance: a second node downloads and validates a mined chain over P2P.

set -u

ROOT="/mnt/e/project Amarian"
NODE="$ROOT/build/dev/src/amariand"
CLI="$ROOT/build/dev/src/amarian-cli"
WORK="/tmp/amarian_phase4"
A="$WORK/node_a"
B="$WORK/node_b"
PAYOUT="0120$(printf '11%.0s' $(seq 32))"
RPC_A=12580
P2P_A=12581
RPC_B=12582
P2P_B=12583

rm -rf "$WORK"
mkdir -p "$WORK"

cleanup() {
    [ -n "${A_PID:-}" ] && kill "$A_PID" 2>/dev/null || true
    [ -n "${B_PID:-}" ] && kill "$B_PID" 2>/dev/null || true
    wait "${A_PID:-}" 2>/dev/null || true
    wait "${B_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT

tip_hash() {
    sed -n 's/.*"best_block_hash": "\([0-9a-f]*\)".*/\1/p' "$1" | head -1
}
tip_height() {
    sed -n 's/.*"blocks": \([0-9][0-9]*\),.*/\1/p' "$1" | head -1
}

printf '=== node A mines three blocks ===\n'
"$NODE" --chain regtest --datadir "$A" --generate 3 --payout "$PAYOUT" \
    >"$WORK/mine.log" 2>&1 || exit 1

printf '=== node B creates a competing one-block fork before connecting ===\n'
"$NODE" --chain regtest --datadir "$B" --generate 1 --payout "$PAYOUT" \
    >"$WORK/fork.log" 2>&1 || exit 1

printf '=== start node A listener ===\n'
timeout 15s "$NODE" --chain regtest --datadir "$A" --rpc --rpcport "$RPC_A" \
    --p2p-port "$P2P_A" --log-level info --log-deterministic >"$WORK/a.log" 2>&1 &
A_PID=$!
sleep 1

printf '=== start node B and connect it to node A ===\n'
timeout 12s "$NODE" --chain regtest --datadir "$B" --rpc --rpcport "$RPC_B" \
    --p2p-port "$P2P_B" --connect "127.0.0.1:$P2P_A" --log-level info \
    --log-deterministic >"$WORK/b.log" 2>&1 &
B_PID=$!

for _ in $(seq 1 10); do
    "$CLI" --chain regtest --datadir "$B" --rpcport "$RPC_B" getblockchaininfo \
        >"$WORK/b_info.json" 2>/dev/null || true
    [ "$(tip_height "$WORK/b_info.json")" = "3" ] && break
    sleep 1
done

"$CLI" --chain regtest --datadir "$A" --rpcport "$RPC_A" getblockchaininfo \
    >"$WORK/a_info.json"
"$CLI" --chain regtest --datadir "$B" --rpcport "$RPC_B" getblockchaininfo \
    >"$WORK/b_info.json"

A_HASH="$(tip_hash "$WORK/a_info.json")"
B_HASH="$(tip_hash "$WORK/b_info.json")"
A_HEIGHT="$(tip_height "$WORK/a_info.json")"
B_HEIGHT="$(tip_height "$WORK/b_info.json")"

printf 'node A: height %s hash %s\n' "$A_HEIGHT" "$A_HASH"
printf 'node B: height %s hash %s\n' "$B_HEIGHT" "$B_HASH"

[ "$A_HEIGHT" = "3" ] || exit 1
[ "$B_HEIGHT" = "$A_HEIGHT" ] || exit 1
[ "$B_HASH" = "$A_HASH" ] || exit 1

printf 'PHASE 4 ACCEPTANCE: pass\n'
