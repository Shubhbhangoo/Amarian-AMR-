#!/usr/bin/env bash
# Phase 11 acceptance: installed tools, wallet workflow, RPC, and explorer HTTP.

set -eu

ROOT="/mnt/e/project Amarian"
NODE="$ROOT/build/dev/src/amariand"
CLI="$ROOT/build/dev/src/amarian-cli"
WALLET="$ROOT/build/dev/src/amarian-wallet"
WORK="/tmp/amarian_phase11_acceptance"
RPC=12720
EXPLORER=12721

rm -rf "$WORK"
mkdir -p "$WORK/install"

for binary in "$NODE" "$CLI" "$WALLET" "$ROOT/build/dev/src/amarian-genesis" "$ROOT/build/dev/src/amarian-retarget-sim"; do
    test -x "$binary"
done

cmake --install "$ROOT/build/dev" --prefix "$WORK/install" >/dev/null
test -x "$WORK/install/bin/amariand"
test -x "$WORK/install/bin/amarian-wallet"

"$WALLET" --chain regtest --wallet "$WORK/wallet.dat" create >"$WORK/create.txt"
ADDR=$("$WALLET" --chain regtest --wallet "$WORK/wallet.dat" getnewaddress)
test -n "$ADDR"

"$NODE" --chain regtest --datadir "$WORK/node" --generate 21 --payout "$ADDR" \
    >"$WORK/mine.txt" 2>&1

cleanup() {
    kill "${NODE_PID:-}" 2>/dev/null || true
    wait "${NODE_PID:-}" 2>/dev/null || true
}
trap cleanup EXIT

timeout 30s "$NODE" --chain regtest --datadir "$WORK/node" \
    --rpc --rpcport "$RPC" --explorer-port "$EXPLORER" \
    --wallet "$WORK/wallet.dat" --log-level error >"$WORK/node.log" 2>&1 &
NODE_PID=$!
sleep 2

curl --fail --silent "http://127.0.0.1:$EXPLORER/explorer/status" >"$WORK/explorer.json"
grep -q '"blocks":21' "$WORK/explorer.json"
curl --fail --silent "http://127.0.0.1:$EXPLORER/explorer/tip" >"$WORK/tip.json"
grep -q '"height":21' "$WORK/tip.json"

"$CLI" --chain regtest --datadir "$WORK/node" --rpcport "$RPC" getbalance \
    >"$WORK/balance.json"
grep -q '"confirmed"' "$WORK/balance.json"

"$WALLET" --chain regtest --wallet "$WORK/wallet.dat" backup >"$WORK/backup.txt"
MNEMONIC=$(awk 'BEGIN { capture=0 } /^Mnemonic:/ { capture=1; next } /^Metadata:/ { capture=0 } capture { printf "%s%s", sep, $0; sep=" " }' "$WORK/backup.txt")
test -n "$MNEMONIC"
"$WALLET" --chain regtest --wallet "$WORK/restored.dat" restore "$MNEMONIC" \
    >"$WORK/restore.txt"

printf 'PHASE 11 ACCEPTANCE: pass\n'
