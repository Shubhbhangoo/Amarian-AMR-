#!/usr/bin/env bash
# Measure the Phase 12 decentralisation questions on a real local run.
#
# The harness deliberately uses regtest: it makes a controlled chain quickly,
# then replays the exact exported bytes into a fresh node.  It does not change
# consensus parameters and it never uses a prebuilt chain or estimated values.
#
# Usage: scripts/phase12_measure.sh [block-count]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NODE="${AMARIAN_NODE:-$ROOT/build/dev/src/amariand}"
BLOCKS="${1:-100}"

if [[ ! -x "$NODE" ]]; then
    echo "node executable not found: $NODE" >&2
    exit 2
fi
if ! [[ "$BLOCKS" =~ ^[1-9][0-9]*$ ]]; then
    echo "block-count must be a positive integer" >&2
    exit 2
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/amarian-phase12.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

SOURCE="$WORK/source"
DEST="$WORK/dest"
BLOCK_FILE="$WORK/blocks.dat"
PAYOUT="0120$(printf '11%.0s' $(seq 1 32))"

measure() {
    local output="$1"
    shift
    /usr/bin/time -f 'wall_seconds=%e\nuser_seconds=%U\nsystem_seconds=%S\nmax_rss_kb=%M' \
        -o "$output" "$@"
}

measure "$WORK/generate.time" "$NODE" --chain regtest --datadir "$SOURCE" \
    --generate "$BLOCKS" --payout "$PAYOUT" --log-level error
measure "$WORK/export.time" "$NODE" --chain regtest --datadir "$SOURCE" \
    --export-blocks "$BLOCK_FILE" --log-level error
measure "$WORK/import.time" "$NODE" --chain regtest --datadir "$DEST" \
    --import-blocks "$BLOCK_FILE" --log-level error

bytes="$(stat -c '%s' "$BLOCK_FILE")"
storage="$(du -sb "$DEST" | awk '{print $1}')"
import_wall="$(awk -F= '$1 == "wall_seconds" { print $2 }' "$WORK/import.time")"
if awk "BEGIN { exit !($import_wall > 0) }"; then
    throughput="$(awk -v b="$bytes" -v s="$import_wall" 'BEGIN { printf "%.3f", b / s / 1048576 }')"
else
    throughput="n/a"
fi

echo "# Amarian Phase 12 measurement"
echo
echo "- date (UTC): $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "- host: $(uname -srvmo)"
echo "- node: $NODE"
echo "- blocks: $BLOCKS"
echo "- chain: regtest"
echo "- exported block bytes: $bytes"
echo "- destination chainstate bytes: $storage"
echo "- import throughput: ${throughput} MiB/s (serialized block file / import wall time)"
echo
echo "## Timings"
echo
echo '| operation | wall seconds | user seconds | system seconds | max RSS KiB |'
echo '|---|---:|---:|---:|---:|'
for operation in generate export import; do
    awk -v op="$operation" '
        BEGIN { wall="n/a"; user="n/a"; sys="n/a"; rss="n/a" }
        {
            split($0, fields, "=")
            if (fields[1] == "wall_seconds") wall=fields[2]
            if (fields[1] == "user_seconds") user=fields[2]
            if (fields[1] == "system_seconds") sys=fields[2]
            if (fields[1] == "max_rss_kb") rss=fields[2]
        }
        END { printf "| %s | %s | %s | %s | %s |\n", op, wall, user, sys, rss }
    ' "$WORK/$operation.time"
done

echo
echo "The import is the initial-block-download proxy: it validates and stores"
echo "the exact bytes exported by the source node into an empty chainstate."
