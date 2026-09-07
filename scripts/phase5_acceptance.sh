#!/usr/bin/env bash
# Phase 5 acceptance: create wallet, mine coins, send over P2P, receive on another
# node, verify balance, and confirm backup/restore produces identical first address.

set -eu

ROOT="/mnt/e/project Amarian"
NODE="$ROOT/build/dev/src/amariand"
CLI="$ROOT/build/dev/src/amarian-cli"
WALLET_BIN="$ROOT/build/dev/src/amarian-wallet"
WORK="/tmp/amarian_phase5"
A="$WORK/node_a"
B="$WORK/node_b"
WALLET_A="$WORK/wallet_a.dat"
WALLET_B="$WORK/wallet_b.dat"
WALLET_C="$WORK/wallet_c.dat"
RPC_A=12540
P2P_A=12541
RPC_B=12542
P2P_B=12543

rm -rf "$WORK"
mkdir -p "$WORK"

cleanup() {
    [ -n "${A_PID:-}" ] && kill "$A_PID" 2>/dev/null || true
    [ -n "${B_PID:-}" ] && kill "$B_PID" 2>/dev/null || true
    wait "${A_PID:-}" 2>/dev/null || true
    wait "${B_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT

tip_height() {
    sed -n 's/.*"blocks": \([0-9][0-9]*\),.*/\1/p' "$1" | head -1
}

# --- Step 1: Create wallets ---------------------------------------------------
printf '=== Step 1: create wallets ===\n'
"$WALLET_BIN" --chain regtest --wallet "$WALLET_A" create > "$WORK/create_a.txt" 2>&1
"$WALLET_BIN" --chain regtest --wallet "$WALLET_B" create > "$WORK/create_b.txt" 2>&1

# --- Step 2: Get receive address on Node A wallet ----------------------------
printf '=== Step 2: generate receive address for Node A ===\n'
ADDR_A=$("$WALLET_BIN" --chain regtest --wallet "$WALLET_A" getnewaddress 2>&1 | tr -d '[:space:]')
printf 'Node A address: %s\n' "$ADDR_A"
[ -n "$ADDR_A" ] || { printf 'ERROR: empty address from Node A wallet\n'; exit 1; }

# --- Step 3: Mine 25 blocks to Node A address --------------------------------
# Coinbase maturity is 20 blocks on regtest; block 1 reward is spendable at h 21+.
printf '=== Step 3: mine 25 blocks to Node A address ===\n'
"$NODE" --chain regtest --datadir "$A" --generate 25 --payout "$ADDR_A" \
    > "$WORK/mine_a.log" 2>&1 || exit 1

# --- Step 4: Start Node A as RPC + P2P server --------------------------------
printf '=== Step 4: start Node A (RPC+P2P+wallet) ===\n'
timeout 30s "$NODE" --chain regtest --datadir "$A" \
    --rpc --rpcport "$RPC_A" \
    --p2p-port "$P2P_A" \
    --payout "$ADDR_A" \
    --wallet "$WALLET_A" \
    --log-level info --log-deterministic > "$WORK/a.log" 2>&1 &
A_PID=$!
sleep 2

# --- Step 5: Get receive address for Node B ----------------------------------
printf '=== Step 5: generate receive address for Node B ===\n'
ADDR_B=$("$WALLET_BIN" --chain regtest --wallet "$WALLET_B" getnewaddress 2>&1 | tr -d '[:space:]')
printf 'Node B address: %s\n' "$ADDR_B"
[ -n "$ADDR_B" ] || { printf 'ERROR: empty address from Node B wallet\n'; exit 1; }

# --- Step 6: Start Node B connected to Node A --------------------------------
printf '=== Step 6: start Node B connected to Node A ===\n'
timeout 25s "$NODE" --chain regtest --datadir "$B" \
    --rpc --rpcport "$RPC_B" \
    --p2p-port "$P2P_B" \
    --wallet "$WALLET_B" \
    --connect "127.0.0.1:$P2P_A" \
    --log-level info --log-deterministic > "$WORK/b.log" 2>&1 &
B_PID=$!

# --- Step 7: Wait for Node B to sync to height 25 ----------------------------
printf '=== Step 7: waiting for Node B to sync to height 25 ===\n'
for _ in $(seq 1 15); do
    "$CLI" --chain regtest --datadir "$B" --rpcport "$RPC_B" getblockchaininfo \
        > "$WORK/b_info.json" 2>/dev/null || true
    H=$(tip_height "$WORK/b_info.json")
    [ "$H" = "25" ] && break
    sleep 1
done
B_HEIGHT=$(tip_height "$WORK/b_info.json")
printf 'Node B synced to height: %s\n' "$B_HEIGHT"
[ "$B_HEIGHT" = "25" ] || \
    { printf 'ERROR: Node B failed to sync to height 25 (got %s)\n' "$B_HEIGHT"; exit 1; }

# --- Step 8: Send 50 AMR from Node A to Node B -------------------------------
printf '=== Step 8: send 50 AMR from Node A wallet to Node B address ===\n'
"$CLI" --chain regtest --datadir "$A" --rpcport "$RPC_A" sendtoaddress "$ADDR_B" 50 \
    > "$WORK/send.json" 2>&1
cat "$WORK/send.json"

# --- Step 9: Mine 1 confirmation block on Node A -----------------------------
printf '=== Step 9: mine 1 confirmation block on Node A ===\n'
"$CLI" --chain regtest --datadir "$A" --rpcport "$RPC_A" generate 1 \
    > "$WORK/confirm.json" 2>&1

# --- Step 10: Wait for Node B to sync block 26 -------------------------------
printf '=== Step 10: waiting for Node B to sync block 26 ===\n'
for _ in $(seq 1 15); do
    "$CLI" --chain regtest --datadir "$B" --rpcport "$RPC_B" getblockchaininfo \
        > "$WORK/b_info2.json" 2>/dev/null || true
    H=$(tip_height "$WORK/b_info2.json")
    [ "$H" = "26" ] && break
    sleep 1
done
B_HEIGHT2=$(tip_height "$WORK/b_info2.json")
printf 'Node B height after transfer: %s\n' "$B_HEIGHT2"
[ "$B_HEIGHT2" = "26" ] || \
    { printf 'ERROR: Node B failed to sync block 26 (got %s)\n' "$B_HEIGHT2"; exit 1; }

# --- Step 11: Verify Node B received funds -----------------------------------
printf '=== Step 11: verify Node B received funds ===\n'
"$CLI" --chain regtest --datadir "$B" --rpcport "$RPC_B" getbalance \
    > "$WORK/b_balance.json" 2>&1
cat "$WORK/b_balance.json"
"$CLI" --chain regtest --datadir "$B" --rpcport "$RPC_B" listtransactions \
    > "$WORK/b_txlist.json" 2>&1
TX_COUNT=$(grep -c '"txid"' "$WORK/b_txlist.json" || true)
printf 'Node B transaction count: %s\n' "$TX_COUNT"
[ "$TX_COUNT" -ge 1 ] || \
    { printf 'ERROR: Node B listtransactions shows no transactions\n'; exit 1; }

# --- Step 12: Backup / restore verification ----------------------------------
printf '=== Step 12: backup Node A wallet ===\n'
"$WALLET_BIN" --chain regtest --wallet "$WALLET_A" backup \
    > "$WORK/backup_a.txt" 2>&1
cat "$WORK/backup_a.txt"
MNEMONIC=$(awk 'BEGIN { capture=0 } /^Mnemonic:/ { capture=1; next } /^Metadata:/ { capture=0 } capture { printf "%s%s", sep, $0; sep=" " }' "$WORK/backup_a.txt")
[ -n "$MNEMONIC" ] || { printf 'ERROR: no mnemonic in backup output\n'; exit 1; }

printf '=== Step 13: restore into wallet C and verify first address ===\n'
"$WALLET_BIN" --chain regtest --wallet "$WALLET_C" restore "$MNEMONIC" \
    > "$WORK/restore_c.txt" 2>&1
cat "$WORK/restore_c.txt"

ADDR_A2=$("$WALLET_BIN" --chain regtest --wallet "$WALLET_A" listaddresses 2>&1 | head -1 | tr -d '[:space:]')
ADDR_C2=$("$WALLET_BIN" --chain regtest --wallet "$WALLET_C" listaddresses 2>&1 | head -1 | tr -d '[:space:]')
printf 'Node A first address: %s\n' "$ADDR_A2"
printf 'Node C first address: %s\n' "$ADDR_C2"
[ "$ADDR_A2" = "$ADDR_C2" ] || \
    { printf 'ERROR: restored wallet produces different first address\n'; exit 1; }

printf '\nPHASE 5 ACCEPTANCE: pass\n'
