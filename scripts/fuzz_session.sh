#!/usr/bin/env bash
# Run the fuzz targets for a fixed wall-clock budget and report one line each.
#
# Usage: fuzz_session.sh <seconds> [target ...]
#
# Two host-specific details are baked in, both established by measurement rather
# than guessed:
#
#   * -print_funcs=0. libFuzzer's default NEW_FUNC symbolisation costs roughly
#     three orders of magnitude on this host (222 exec/s against 160k on the hex
#     target). It prints nothing this project acts on.
#   * Logs and corpora live under the Windows temp directory, not /tmp. The WSL VM
#     stops when idle and takes /tmp with it, which has already silently discarded
#     one ten-minute run's output.
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

SECONDS_BUDGET="${1:?usage: fuzz_session.sh <seconds> [target ...]}"
shift
TARGETS=("$@")
[ "${#TARGETS[@]}" -eq 0 ] && TARGETS=(fuzz_serialize fuzz_hex fuzz_args)

STATE=/mnt/c/Users/Shubhkarman/AppData/Local/Temp/amarian_fuzz
mkdir -p "$STATE"

for t in "${TARGETS[@]}"; do
  bin="build/fuzz/fuzz/$t"
  if [ ! -x "$bin" ]; then
    echo "  $t: MISSING ($bin) — build the fuzz preset first"
    continue
  fi
  corpus="$STATE/corpus_$t"
  log="$STATE/$t.log"
  mkdir -p "$corpus"
  "$bin" "$corpus" -max_total_time="$SECONDS_BUDGET" -print_funcs=0 \
      -print_final_stats=1 >"$log" 2>&1
  rc=$?

  runs=$(sed -n 's/^stat::number_of_executed_units:[[:space:]]*\([0-9]*\)$/\1/p' "$log" | tail -1)
  rate=$(sed -n 's/^stat::average_exec_per_sec:[[:space:]]*\([0-9]*\)$/\1/p' "$log" | tail -1)
  cov=$(grep -oE "cov: [0-9]+" "$log" | tail -1 | tr -d 'cov: ')
  corp=$(grep -oE "corp: [0-9]+" "$log" | tail -1 | grep -oE "[0-9]+")
  reports=$(grep -c "runtime error" "$log")
  crashes=$(grep -c "ERROR: libFuzzer" "$log")
  leaks=$(grep -c "ERROR: LeakSanitizer" "$log")

  printf '  %-15s exit=%-3s runs=%-10s exec/s=%-7s cov=%-6s corpus=%-6s ubsan=%s crash=%s leak=%s\n' \
      "$t" "$rc" "${runs:-0}" "${rate:-0}" "${cov:-?}" "${corp:-?}" \
      "$reports" "$crashes" "$leaks"

  if [ "$rc" -ne 0 ] || [ "$reports" -ne 0 ] || [ "$crashes" -ne 0 ]; then
    echo "    --- findings ---"
    grep -E "runtime error|ERROR:|SUMMARY:|Test unit written" "$log" | head -10 | sed 's/^/    /'
  fi
done
