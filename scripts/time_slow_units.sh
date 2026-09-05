#!/usr/bin/env bash
# Decide whether a libFuzzer slow-unit-* artifact is a real finding.
#
# libFuzzer writes slow-unit-<sha1> into its working directory for any input whose
# single execution exceeds -report_slow_units seconds (default 10) and prints
# "Slowest unit: N s". In a parser an attacker feeds, a genuinely slow input is the
# resource-exhaustion class in THREAT_MODEL.md and belongs in tests/vectors/ as a
# regression case. But the timer covers the whole callback, so a stall the harness
# had nothing to do with — a loaded box, a symboliser, an allocator arm running in
# another process — lands on whichever input was unlucky.
#
# So replay each unit a few thousand times under the standalone driver, which has
# no fuzzer, no mutator and no feature table, and divide. Enough replays that
# process startup is amortised: at n=20, ASan init and loading a 2 MB binary off
# the drvfs mount dominate and every unit reads as ~27 ms.
#
# Result on this host, 2026-09-05, n=2000, the three units left in the repo root:
#
#     758c527241…  2624 bytes   952 us/call
#     2cbc477cac…   827 bytes   558 us/call
#     9b3e0216fc…   752 bytes   323 us/call
#
# Four orders of magnitude below the 10 s that caused them to be written, so
# nothing here is slow. Corroborating: the recorded 600 s session's log has no
# "Slowest unit" line at all, and these files postdate it. They were deleted rather
# than promoted, and the artifact prefixes are now in .gitignore.
#
# What this does not establish is which run wrote them, or what stalled. Attributed
# to load rather than to the parser on the strength of the replay times alone.
#
# Usage: time_slow_units.sh [replays] [file ...]
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

REPLAYS="${1:-2000}"
shift 2>/dev/null || true
BIN=build/fuzz/fuzz/standalone_fuzz_serialize

if [ ! -x "$BIN" ]; then
  echo "MISSING $BIN — cmake --build build/fuzz --target standalone_fuzz_serialize"
  exit 1
fi

shopt -s nullglob
units=("$@")
if [ "${#units[@]}" -eq 0 ]; then
  units=(slow-unit-*)
fi
if [ "${#units[@]}" -eq 0 ]; then
  echo "no slow-unit-* files in the repo root — nothing to triage"
  exit 0
fi

printf '%-42s %-7s %-9s %-9s %s\n' unit bytes replays wall_s per_call
for unit in "${units[@]}"; do
  start=$(date +%s.%N)
  ASAN_OPTIONS=detect_leaks=0 "$BIN" -n "$REPLAYS" "$unit" >/dev/null 2>&1
  status=$?
  end=$(date +%s.%N)
  printf '%-42s %-7s %-9s %-9s %s%s\n' \
      "$(basename "$unit" | sed 's/^slow-unit-//')" "$(stat -c %s "$unit")" \
      "$REPLAYS" \
      "$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", e-s}')" \
      "$(awk -v s="$start" -v e="$end" -v n="$REPLAYS" \
           'BEGIN{printf "%.1f us", (e-s)/n*1e6}')" \
      "$([ "$status" -eq 0 ] || echo "  EXIT=$status")"
done

echo "--- microseconds per call means the 10 s timer caught a stall, not the parser."
echo "--- milliseconds per call is a real finding: minimise it and promote it to tests/vectors/."
