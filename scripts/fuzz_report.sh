#!/usr/bin/env bash
# Summarise a completed fuzz_session.sh log without re-running the fuzzer.
#
# fuzz_session.sh prints its summary line to stdout, which is lost if the caller
# backgrounded it and the pipe went nowhere. The log itself survives, so recover
# the numbers from it instead of spending another wall-clock budget.
#
# Usage: fuzz_report.sh [target ...]
set -uo pipefail

STATE=/mnt/c/Users/Shubhkarman/AppData/Local/Temp/amarian_fuzz
TARGETS=("$@")
[ "${#TARGETS[@]}" -eq 0 ] && TARGETS=(fuzz_serialize fuzz_hex fuzz_args)

for t in "${TARGETS[@]}"; do
  log="$STATE/$t.log"
  if [ ! -f "$log" ]; then
    echo "  $t: no log at $log"
    continue
  fi
  runs=$(sed -n 's/^stat::number_of_executed_units:[[:space:]]*\([0-9]*\)$/\1/p' "$log" | tail -1)
  rate=$(sed -n 's/^stat::average_exec_per_sec:[[:space:]]*\([0-9]*\)$/\1/p' "$log" | tail -1)
  peak=$(sed -n 's/^stat::peak_rss_mb:[[:space:]]*\([0-9]*\)$/\1/p' "$log" | tail -1)
  # The last progress line carries the final coverage, feature and corpus figures.
  last=$(grep -E "^#[0-9]+" "$log" | tail -1)
  cov=$(printf '%s\n' "$last" | grep -oE "cov: [0-9]+" | grep -oE "[0-9]+")
  ft=$(printf '%s\n' "$last" | grep -oE "ft: [0-9]+" | grep -oE "[0-9]+")
  corp=$(printf '%s\n' "$last" | grep -oE "corp: [0-9]+" | grep -oE "[0-9]+")
  secs=$(grep -oE "Done [0-9]+ runs in [0-9]+ second" "$log" | grep -oE "in [0-9]+" | grep -oE "[0-9]+" | tail -1)
  units=$(ls -1 "$STATE/corpus_$t" 2>/dev/null | wc -l)
  reports=$(grep -c "runtime error" "$log")
  crashes=$(grep -c "ERROR: libFuzzer" "$log")
  leaks=$(grep -c "ERROR: LeakSanitizer" "$log")
  oom=$(grep -c "ERROR: out-of-memory" "$log")

  printf '  %-15s runs=%-10s secs=%-5s exec/s=%-7s cov=%-5s ft=%-5s corp=%-5s disk=%-5s rss=%-4s ubsan=%s crash=%s leak=%s oom=%s\n' \
      "$t" "${runs:-?}" "${secs:-?}" "${rate:-?}" "${cov:-?}" "${ft:-?}" \
      "${corp:-?}" "${units:-?}" "${peak:-?}" "$reports" "$crashes" "$leaks" "$oom"

  if [ "$reports" -ne 0 ] || [ "$crashes" -ne 0 ] || [ "$leaks" -ne 0 ] || [ "$oom" -ne 0 ]; then
    echo "    --- findings ---"
    grep -E "runtime error|ERROR:|SUMMARY:|Test unit written" "$log" | head -10 | sed 's/^/    /'
  fi
done
