#!/usr/bin/env bash
# Explain fuzz_serialize's resident-set growth.
#
# The 600 s libFuzzer session climbed from 32 MB at INITED to 878 MB at exit —
# 27× — with zero LeakSanitizer findings. In a parser that an attacker feeds,
# unexplained memory growth is the resource-exhaustion class in THREAT_MODEL.md,
# so it needed an answer rather than a shrug. Three candidate explanations, and
# the arms below separate them:
#
#   1. A leak reachable from a live pointer. Invisible to LSan. RSS climbs
#      without bound, proportional to work done.
#   2. Freed pages the allocator has not returned to the OS. Bounded, and
#      sensitive to allocator_release_to_os_interval_ms.
#   3. ASan's quarantine: freed chunks deliberately withheld from reuse, each
#      with its allocation and free stack traces attached, so that a
#      use-after-free is still detectable. Bounded, insensitive to the release
#      interval — a quarantined chunk has not been returned to the allocator at
#      all — and sensitive only to quarantine_size_mb.
#
# Arms 1 and 2 use the standalone driver rather than libFuzzer, so no corpus,
# feature table or mutator is in the measurement: the only thing allocating is
# the harness. Arm 3 is the libFuzzer run for reference.
#
# Result on this host, 2026-09-05, 178-unit corpus, 4000 replays each (712 000
# harness invocations):
#
#     default quarantine                946 MB, flat after ~2000 replays
#     quarantine_size_mb=1              18.5 MB
#     release_to_os_interval_ms=0       no effect (see note above)
#
# So it is explanation 3, and the code's actual steady state is 18.5 MB. The
# plateau is the point: a leak does not plateau. Nothing is changed in the fuzz
# session as a result — the quarantine is what makes use-after-free detectable,
# and 946 MB sits comfortably under libFuzzer's 2048 MB default rss_limit — but
# the number is recorded so the next person to see 878 MB does not spend an
# evening on it.
#
# Usage: rss_probe.sh [replays]
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

REPLAYS="${1:-6000}"
CORPUS=/mnt/c/Users/Shubhkarman/AppData/Local/Temp/amarian_fuzz/corpus_fuzz_serialize
BIN=build/fuzz/fuzz/standalone_fuzz_serialize

if [ ! -x "$BIN" ]; then
  echo "MISSING $BIN — cmake --build build/fuzz --target standalone_fuzz_serialize"
  exit 1
fi
if [ ! -d "$CORPUS" ] || [ -z "$(ls -A "$CORPUS" 2>/dev/null)" ]; then
  echo "MISSING corpus at $CORPUS — run scripts/fuzz_session.sh first"
  exit 1
fi

units=$(ls -1 "$CORPUS" | wc -l)
echo "corpus units: $units   replays: $REPLAYS   invocations: $((units * REPLAYS))"

arm() {
  local label="$1" opts="$2" replays="$3"
  local out
  out=$(ASAN_OPTIONS="$opts" "$BIN" -n "$replays" "$CORPUS"/* 2>&1)
  local start end samples ninth
  start=$(printf '%s\n' "$out" | sed -n 's/.*rss_start=\([0-9]*\)KiB.*/\1/p')
  end=$(printf '%s\n' "$out" | sed -n 's/^rss_end=\([0-9]*\)KiB$/\1/p')
  samples=$(printf '%s\n' "$out" | sed -n 's/.*rss=\([0-9]*\)KiB$/\1/p')
  # A leak keeps climbing; a bounded cause plateaus. Compare the last two of the
  # driver's ten samples rather than the midpoint, so the verdict is right at a
  # replay count that only just reaches the plateau — the default quarantine
  # needs roughly 4000 replays of a 178-unit corpus to get there.
  ninth=$(printf '%s\n' "$samples" | tail -2 | head -1)
  printf '  %-34s n=%-6s start=%-8s penultimate=%-8s end=%-8s %s\n' \
      "$label" "$replays" "${start:-?}KiB" "${ninth:-?}KiB" "${end:-?}KiB" \
      "$(awk -v m="${ninth:-0}" -v e="${end:-0}" \
           'BEGIN{ if (m>0 && e <= m*1.02) print "plateau"; else print "STILL CLIMBING" }')"
}

arm "default quarantine"          "detect_leaks=1" "$REPLAYS"
arm "quarantine_size_mb=1"        "detect_leaks=1:quarantine_size_mb=1:thread_local_quarantine_size_kb=64" "$REPLAYS"
# A tenth of the replays: madvise on every free makes this arm roughly two orders
# of magnitude slower, and its only job is to show that the release interval does
# not change the trend, which is visible long before the plateau.
arm "release_to_os_interval_ms=0" "detect_leaks=1:allocator_release_to_os_interval_ms=0" \
    "$(( REPLAYS / 10 > 0 ? REPLAYS / 10 : 1 ))"

echo "--- a plateau under a small quarantine, and none without one, names the cause."
