#!/usr/bin/env bash
# Full Phase 12 measurement run: storage/IBD proxy, live P2P traffic, and mesh convergence.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NODE="${AMARIAN_NODE:-$ROOT/build/dev/src/amariand}"
CLI="${AMARIAN_CLI:-$ROOT/build/dev/src/amarian-cli}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/amarian-phase12-full.XXXXXX")"
PAYOUT="0120$(printf '11%.0s' $(seq 1 32))"
trap 'kill ${PIDS[*]:-} 2>/dev/null || true; wait ${PIDS[*]:-} 2>/dev/null || true; rm -rf "$WORK"' EXIT
PIDS=()

if [[ ! -x "$NODE" || ! -x "$CLI" ]]; then
    echo "node or cli executable not found" >&2
    exit 2
fi

measure_chain() {
    local blocks="$1"
    local dir="$WORK/chain-$blocks"
    local file="$WORK/chain-$blocks.dat"
    /usr/bin/time -f '%e %U %S %M' -o "$WORK/chain-$blocks.generate" \
        "$NODE" --chain regtest --datadir "$dir/source" --generate "$blocks" \
        --payout "$PAYOUT" --log-level error
    # Export before import so the destination replays the exact source bytes.
    "$NODE" --chain regtest --datadir "$dir/source" --export-blocks "$file" \
        --log-level error >/dev/null
    /usr/bin/time -f '%e %U %S %M' -o "$WORK/chain-$blocks.import" \
        "$NODE" --chain regtest --datadir "$dir/dest" --import-blocks "$file" \
        --log-level error 2>/dev/null
    local bytes storage
    bytes="$(stat -c '%s' "$file")"
    storage="$(du -sb "$dir/dest" | awk '{print $1}')"
    printf '%s %s %s %s %s %s\n' "$blocks" "$bytes" "$storage" \
        "$(awk '{print $1}' "$WORK/chain-$blocks.generate")" \
        "$(awk '{print $4}' "$WORK/chain-$blocks.generate")" \
        "$(awk '{print $1}' "$WORK/chain-$blocks.import")"
}

start_node() {
    local name="$1" datadir="$2" rpc="$3" p2p="$4" connect="$5"
    local log="$WORK/$name.log"
    local args=(--chain regtest --datadir "$datadir" --rpc --rpcport "$rpc"
        --p2p-port "$p2p" --log-level info --log-deterministic)
    [[ -n "$connect" ]] && args+=(--connect "$connect")
    "$NODE" "${args[@]}" >"$log" 2>&1 &
    PIDS+=("$!")
}

height() {
    "$CLI" --chain regtest --datadir "$1" --rpcport "$2" getblockchaininfo 2>/dev/null \
        | sed -n 's/.*"blocks": \([0-9][0-9]*\),.*/\1/p' | head -1
}

hash() {
    "$CLI" --chain regtest --datadir "$1" --rpcport "$2" getblockchaininfo 2>/dev/null \
        | sed -n 's/.*"best_block_hash": "\([0-9a-f]*\)".*/\1/p' | head -1
}

traffic() {
    sed -n 's/.*p2p: traffic bytes sent \([0-9][0-9]*\) received \([0-9][0-9]*\).*/\1 \2/p' "$1" | tail -1
}

printf '# Amarian Phase 12 full measurement\n\n'
printf -- '- date (UTC): %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
printf -- '- host: %s\n' "$(uname -srvmo)"
printf -- '- node: %s\n\n' "$NODE"

printf '## Chain size and cold import\n\n'
printf '| blocks | serialized bytes | chainstate bytes | generate wall s | generate RSS KiB | import wall s |\n'
printf '|---:|---:|---:|---:|---:|---:|\n'
for count in 100 500 1000; do
    read -r blocks bytes storage gen_wall gen_rss import_wall <<<"$(measure_chain "$count")"
    printf '| %s | %s | %s | %s | %s | %s |\n' "$blocks" "$bytes" "$storage" \
        "$gen_wall" "$gen_rss" "$import_wall"
done

printf '\n## Live P2P cold sync\n\n'
MESH="$WORK/mesh"
mkdir -p "$MESH/a" "$MESH/b" "$MESH/c"
"$NODE" --chain regtest --datadir "$MESH/a" --generate 25 --payout "$PAYOUT" --log-level error
"$NODE" --chain regtest --datadir "$MESH/b" --generate 1 --payout "$PAYOUT" --log-level error
START_NS="$(date +%s%N)"
start_node a "$MESH/a" 12730 12731 ""
sleep 1
start_node b "$MESH/b" 12732 12733 127.0.0.1:12731
for _ in $(seq 1 30); do
    [[ "$(height "$MESH/b" 12732)" == 25 ]] && break
    sleep 1
done
END_NS="$(date +%s%N)"
if [[ "$(height "$MESH/b" 12732)" != 25 ]]; then
    echo "P2P sync did not reach height 25" >&2
    exit 1
fi
SYNC_MS="$(( (END_NS - START_NS) / 1000000 ))"
printf '| source height | destination height | sync ms | source hash = destination hash |\n'
printf '|---:|---:|---:|---|\n'
printf '| 25 | %s | %s | %s |\n' "$(height "$MESH/b" 12732)" "$SYNC_MS" \
    "$( [[ "$(hash "$MESH/a" 12730)" == "$(hash "$MESH/b" 12732)" ]] && echo yes || echo no )"

printf '\n## Three-node convergence\n\n'
"$NODE" --chain regtest --datadir "$MESH/c" --generate 2 --payout "$PAYOUT" --log-level error
start_node c "$MESH/c" 12734 12735 127.0.0.1:12733
MESH_START="$(date +%s%N)"
for _ in $(seq 1 30); do
    if [[ "$(height "$MESH/c" 12734)" == 25 && "$(hash "$MESH/b" 12732)" == "$(hash "$MESH/c" 12734)" ]]; then
        break
    fi
    sleep 1
done
MESH_END="$(date +%s%N)"
if [[ "$(height "$MESH/c" 12734)" != 25 || "$(hash "$MESH/b" 12732)" != "$(hash "$MESH/c" 12734)" ]]; then
    echo "three-node mesh did not converge" >&2
    exit 1
fi
printf '| nodes | final height | converged | convergence ms |\n|---:|---:|---|---:|\n'
printf '| 3 | 25 | yes | %s |\n' "$(( (MESH_END - MESH_START) / 1000000 ))"

# Stop cleanly so each daemon flushes its final traffic counter to its log.
for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null || true; done
for pid in "${PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
printf '\n### Transport counters\n\n'
printf '| node | bytes sent | bytes received |\n|---|---:|---:|\n'
for node in a b c; do
    read -r sent received <<<"$(traffic "$WORK/$node.log")"
    printf '| %s | %s | %s |\n' "$node" "$sent" "$received"
done
printf '\nPHASE 12 FULL MEASUREMENT: pass\n'
