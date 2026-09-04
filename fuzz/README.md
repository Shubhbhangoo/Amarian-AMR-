# Fuzzing Amarian

Harnesses live one per parser. Build them with the `fuzz` preset, which is Clang +
libFuzzer + ASan + UBSan:

```bash
cmake --preset fuzz && cmake --build build/fuzz
```

## Running

```bash
mkdir -p build/fuzz/corpus/hex
./build/fuzz/fuzz/fuzz_hex build/fuzz/corpus/hex -max_total_time=120 -max_len=4096 -print_funcs=0
```

`-print_funcs=0` is not optional on this setup. libFuzzer symbolises every newly
covered function to print a `NEW_FUNC` line, and `llvm-symbolizer` takes tens of
seconds to parse the DWARF of an ASan+UBSan build living on the `/mnt/e` drvfs
mount. Measured on 2026-09-04, same binary and seed budget:

| flags | throughput |
| --- | --- |
| default (`NEW_FUNC` symbolisation on) | 222 exec/s |
| `-print_funcs=0` | 160 000 exec/s |

Three orders of magnitude, and the process is blocked rather than busy the whole
time — 90 s of wall clock against 0.1 s of CPU. Crash reports are symbolised by
ASan's own path and are unaffected, so nothing diagnostic is lost; a crash pays
the symboliser cost once, which is fine.

Corpora live under `build/fuzz/corpus/` because `build/` is gitignored. They are
disposable: coverage is re-derived in minutes.

## Current harnesses

| target | input | kind |
| --- | --- | --- |
| `fuzz_hex` | arbitrary bytes as a hex string | attacker-facing (RPC, config, test vectors) |
| `fuzz_args` | arbitrary bytes as NUL-separated argv | operator-facing (CLI) |

Both check correctness properties, not just memory safety — see the file header
comments for the exact list. Properties use `FUZZ_CHECK`, never `assert`: the
preset builds RelWithDebInfo, so `assert` is compiled out and would check nothing.

## Last session

2026-09-04, Clang 21.1.8, ASan+UBSan, 12 × 2.5 GHz x86-64:

| target | executions | rate | corpus | crashes |
| --- | --- | --- | --- | --- |
| `fuzz_hex` | 19 384 470 | 160 202/s | 30 units, cov 40 / ft 74 (saturated) | none |
| `fuzz_args` | 1 502 126 | 12 414/s | 105 units | none |

`fuzz_args` runs slower because the harness parses each input twice to check
determinism, and the parser allocates a `std::string` per option.

## Triage

A reproducible crash gets minimised (`-minimize_crash=1`) and then promoted into
`tests/vectors/` as an explicit regression case with a comment naming the bug.
Crash inputs do not stay in the corpus directory, where they would be silently
dropped the next time someone cleans `build/`.
