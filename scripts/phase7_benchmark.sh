#!/usr/bin/env bash
# Phase 7 evidence: compare classical, post-quantum, and hybrid ownership.

set -eu

ROOT="/mnt/e/project Amarian"
BENCH="$ROOT/build/bench/bench/bench_wallet"

if [ ! -x "$BENCH" ]; then
    printf 'Phase 7 benchmark is not built. Run: cmake --preset bench && cmake --build --preset bench --target bench_wallet\n' >&2
    exit 1
fi

exec "$BENCH" \
    --benchmark_min_time=1s \
    --benchmark_repetitions=3 \
    --benchmark_report_aggregates_only=true
