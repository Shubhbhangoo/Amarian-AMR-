#!/usr/bin/env bash
# Re-verify the sanitizer presets after making -fsanitize=integer findings fatal.
#
# Two questions: does the existing suite still pass now that a previously
# recoverable integer finding aborts, and is the libstdc++ <charconv> report gone
# without having disabled the check for our own code?
set -uo pipefail
cd "/mnt/e/project Amarian" || exit 1

for p in asan fuzz; do
  echo "### configure+build $p"
  cmake --preset "$p" >/tmp/rv_cfg_$p.log 2>&1 || { echo "  CONFIGURE FAILED"; tail -20 /tmp/rv_cfg_$p.log; exit 1; }
  grep -m1 "Sanitizers enabled" /tmp/rv_cfg_$p.log | sed 's/^/  /'
  cmake --build "build/$p" >/tmp/rv_bld_$p.log 2>&1 || { echo "  BUILD FAILED"; grep -E "error:" /tmp/rv_bld_$p.log | head -20; exit 1; }
  echo "  build ok (compiler warnings: $(grep -cE " warning:" /tmp/rv_bld_$p.log))"
done

# Confirm the flags actually reached the compile line.
echo "### flags on one asan object"
grep -o -- "-fno-sanitize-recover=[a-z]*" build/asan/compile_commands.json | sort -u | sed 's/^/  /'
grep -o -- "-fsanitize-ignorelist=[^ \"]*" build/asan/compile_commands.json | sort -u | head -1 | sed 's/^/  /'

echo "### asan test suite"
ctest --preset asan >/tmp/rv_tst_asan.log 2>&1
rc=$?
grep -E "tests passed|tests failed" /tmp/rv_tst_asan.log | tail -1 | sed 's/^/  /'
if [ "$rc" -ne 0 ]; then
  echo "  TESTS FAILED"
  grep -E "\(Failed\)|runtime error|SUMMARY:" /tmp/rv_tst_asan.log | head -20
fi
echo "  sanitizer reports in test output: $(grep -c "runtime error" /tmp/rv_tst_asan.log)"

echo "### fuzz targets, integer findings now fatal"
for t in fuzz_args fuzz_hex fuzz_serialize; do
  mkdir -p /tmp/amarian_corpus/$t
  ./build/fuzz/fuzz/$t /tmp/amarian_corpus/$t -max_total_time=45 -print_funcs=0 \
      >/tmp/rv_fuzz_$t.log 2>&1
  frc=$?
  runs=$(grep -oE "^Done [0-9]+ runs" /tmp/rv_fuzz_$t.log | grep -oE "[0-9]+")
  echo "  $t: exit=$frc runs=${runs:-0} reports=$(grep -c "runtime error" /tmp/rv_fuzz_$t.log) crashes=$(grep -c "ERROR: libFuzzer" /tmp/rv_fuzz_$t.log)"
  if [ "$frc" -ne 0 ]; then
    grep -E "runtime error|ERROR|SUMMARY" /tmp/rv_fuzz_$t.log | head -5 | sed 's/^/      /'
  fi
done
