# Fuzzing Amarian

Harnesses live one per parser. Build them with the `fuzz` preset, which is Clang +
libFuzzer + ASan + UBSan:

```bash
cmake --preset fuzz && cmake --build build/fuzz
```

## Running

`scripts/fuzz_session.sh <seconds> [target ...]` is the whole recipe as one
command, and is what the numbers below came from. It bakes in the two host
specifics established by measurement rather than guessed: `-print_funcs=0`, and
corpora and logs under the Windows temp directory rather than `/tmp`, because the
WSL VM stops when idle and takes `/tmp` with it — that has already silently
discarded one ten-minute run's output.

`scripts/fuzz_report.sh [target ...]` re-derives the summary from a log that has
already been written, which is what to reach for when a backgrounded session's
stdout went nowhere.

By hand:

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

Corpora live under the Windows temp directory for the reason above; a corpus is
disposable either way, since coverage is re-derived in minutes.

## Running a harness without libFuzzer

Every target also builds as `standalone_<target>`, linking the same harness
against a plain `main` ([standalone_main.cpp](standalone_main.cpp)) instead of
libFuzzer's:

```bash
./build/fuzz/fuzz/standalone_fuzz_serialize crash-3f2a1c
./build/fuzz/fuzz/standalone_fuzz_serialize -n 5000 corpus/*
```

Two things this is for. Replaying a saved input under gdb, valgrind or a profiler,
none of which cooperate with libFuzzer's signal handlers and its own `main`. And
attributing memory behaviour: libFuzzer's resident set includes its corpus, its
feature table and its mutator, so a question about the harness has to be asked
without it. With `-n`, RSS is sampled from `/proc/self/statm` ten times across the
run, so a leak reads as a rising line and a bounded cause as a flat one.

## Current harnesses

| target | input | kind |
| --- | --- | --- |
| `fuzz_serialize` | arbitrary bytes as a program of read operations plus the buffer they read from | attacker-facing, and the widest surface in the project: every byte a node acts on arrives through `Reader` |
| `fuzz_hex` | arbitrary bytes as a hex string | attacker-facing (RPC, config, test vectors) |
| `fuzz_args` | arbitrary bytes as NUL-separated argv | operator-facing (CLI) |

All three check correctness properties, not just memory safety — see the file
header comments for the exact list. Properties use `FUZZ_CHECK`, never `assert`:
the preset builds RelWithDebInfo, so `assert` is compiled out and would check
nothing.

`fuzz_serialize` parses each input twice and requires identical results, because a
decoder that is non-deterministic across two runs of the same bytes is a chain
split rather than a bug.

## Last session

2026-09-05, Clang 21.1.8, ASan+UBSan with integer findings fatal, 12 × 2.5 GHz
x86-64:

| target | executions | rate | coverage | corpus | findings |
| --- | --- | --- | --- | --- | --- |
| `fuzz_serialize` | 21 121 422 in 601 s | 35 143/s | cov 106 / ft 525 | 180 units | none |
| `fuzz_hex` | 6 572 590 in 45 s | 146 057/s | saturated | 30 units | none |
| `fuzz_args` | 1 112 793 in 45 s | 24 728/s | — | 105 units | none |

Zero crashes, leaks, timeouts, OOMs and UBSan reports. That zero means something
it did not mean a day earlier: until 2026-09-05 a UBSan *integer* finding printed
a diagnostic and let the run exit zero, because `-fsanitize=integer` is not part
of the `undefined` group and `-fno-sanitize-recover=undefined` therefore never
covered it. One had already fired. See decision 34 in
[../docs/DECISIONS.md](../docs/DECISIONS.md).

What the zero still does not mean: `fuzz_hex` reached coverage saturation, which
says further time on it is wasted, not that it is correct. Three small parsers over
one session is evidence of the absence of *shallow* bugs and nothing more.

### The 878 MB that was not a leak

`fuzz_serialize`'s resident set climbed from 32 MB at `INITED` to 878 MB at exit,
with LeakSanitizer reporting nothing. In a parser an attacker feeds, that needed an
answer. Measured with the standalone driver on the saved 178-unit corpus, 4000
replays each:

| ASan configuration | RSS |
| --- | --- |
| default | 946 MB, flat from ~4000 replays onward |
| `quarantine_size_mb=1:thread_local_quarantine_size_kb=64` | 18.5 MB |
| `allocator_release_to_os_interval_ms=0` | no effect |

It is ASan's quarantine — freed chunks deliberately withheld from reuse, each
carrying its allocation and free stack traces, which is what makes a use-after-free
detectable at all. A quarantined chunk has not been returned to the allocator, so
the release interval cannot touch it, which is why that arm changes nothing. The
plateau is the proof it is bounded; a leak does not plateau. The code's real steady
state is 18.5 MB across 712 000 harness invocations.

Nothing is changed as a result. The quarantine is worth its memory, and 946 MB sits
under libFuzzer's 2048 MB default `-rss_limit_mb` with room to spare. It is written
down so the next person to see 878 MB does not spend an evening on it.
`scripts/rss_probe.sh` reproduces the table.

## Triage

A reproducible crash gets minimised (`-minimize_crash=1`) and then promoted into
`tests/vectors/` as an explicit regression case with a comment naming the bug.
Crash inputs do not stay in the corpus directory, where they would be silently
dropped the next time someone cleans `build/`.

### Slow units

libFuzzer writes `slow-unit-<sha1>` into its working directory — the repo root,
when a harness is run by hand — for any input whose single execution exceeds
`-report_slow_units` seconds, default 10. A genuinely slow input in an
attacker-facing parser is the resource-exhaustion class in
[../docs/THREAT_MODEL.md](../docs/THREAT_MODEL.md) and gets promoted like a crash.

But the timer covers the whole callback, so a stall the harness had nothing to do
with lands on whichever input was unlucky. `scripts/time_slow_units.sh` decides
which it is: replay the unit a few thousand times under the standalone driver, with
no fuzzer in the measurement, and divide. Enough replays to amortise process
startup — at `-n 20`, ASan init and loading the binary off the drvfs mount dominate
and every unit reads as ~27 ms.

Three had accumulated in the repo root by 2026-09-05. At `-n 2000`:

| unit | bytes | per call |
| --- | --- | --- |
| `758c527241…` | 2624 | 952 µs |
| `2cbc477cac…` | 827 | 558 µs |
| `9b3e0216fc…` | 752 | 323 µs |

Four orders of magnitude under the 10 s that caused them to be written, and the
recorded 600 s session's log has no `Slowest unit` line at all — these postdate it.
Deleted rather than promoted. What that does not establish is which run wrote them
or what stalled; the attribution to load rather than to the parser rests on the
replay times alone.

The artifact prefixes are now in `.gitignore`, which is about not committing
fuzzer droppings by accident, not about skipping this step.
