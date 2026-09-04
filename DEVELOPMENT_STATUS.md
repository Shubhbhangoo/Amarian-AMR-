# Development Status

Living record of where the project actually is. Updated as work lands, not as
work is planned. Anything not listed as done is not done.

**Last updated:** 2026-09-04
**Current phase:** Phase 0 — Environment and architecture
**Phase 0 status:** complete

---

## Phase 0 acceptance criteria

| Criterion | State | Evidence |
|---|---|---|
| Clean build | met | Nine presets configure and build with no warnings and `-Werror` on; see the matrix under Test results |
| Clean test command | met | `ctest --preset dev` — 47/47 passing, and the same 47 on `debug`, `clang-dev`, `asan`, `tsan` |
| Basic executable | met | `amariand --version`, `--build-info`, `--help` |
| Basic project documentation | met | `README.md`, `SECURITY.md`, nine documents in `docs/`, `fuzz/README.md`, this file |

## Completed

**Environment (verified by running it, not by reading documentation)**

- Build host: WSL2 Ubuntu 26.04 on Windows 11, x86-64.
- GCC 15.2.0 (primary) and Clang 21.1.8 (secondary). C++23 confirmed via
  `__cplusplus == 202302`.
- CMake 4.2.3, Ninja 1.13.2, ccache, mold.
- libsecp256k1 0.7.0 — BIP-340 Schnorr, x-only public keys, ECDSA, MuSig2.
  Verified with a real signature round trip (32-byte x-only key, 64-byte
  signature, `verify=true`).
- OpenSSL 3.5.5 — SHA-256, CSPRNG, and ML-DSA-44/65/87 (FIPS 204) plus
  SLH-DSA-SHA2-128s (FIPS 205) in the **default provider**. Verified with
  `openssl list -key-managers` and a real ML-DSA-44 keygen, which produced a
  1312-byte public key as FIPS 204 specifies. `liboqs`/`oqsprovider` is
  therefore not required.
- RocksDB 9.11.2, standalone Asio 1.30.2, nlohmann/json 3.11.3, GoogleTest
  1.17.0, Google Benchmark, AFL++ 4.33c, clang libFuzzer, ASan/UBSan/TSan/MSan,
  gdb 17.1, valgrind 3.26, clang-tidy-21, clang-format-21.
- Source of truth is on the Windows filesystem, built in-tree. The drvfs
  penalty was measured rather than assumed: 16.480 s (ext4) vs 16.623 s
  (`/mnt/e`) for three identical compilations — under 1%, so no shadow build
  tree is needed.

**Build system**

- C++23, extensions off, `RelWithDebInfo` default, `compile_commands.json`
  exported.
- `-Werror` with conversion, sign-conversion, old-style-cast, switch-enum,
  useless-cast and arith-conversion warnings enabled.
- Hardening: stack protector, `_FORTIFY_SOURCE=3`, `_GLIBCXX_ASSERTIONS`,
  stack-clash protection, CET, full RELRO, non-executable stack.
- Sanitizer presets with ASan/TSan mutual exclusion and
  `-fno-sanitize-recover=undefined`.
- Presets: `dev`, `debug`, `clang-dev`, `asan`, `tsan`, `fuzz`, `bench`,
  `bench-clang`, `release`. `bench-clang` exists because the compilers were
  measured to disagree by 21× on one benchmark (see Architectural decisions), so
  a single-compiler benchmark number is not a fact about the code.
- C++20 module scanning explicitly off. CMake turns it on by default at
  `CMAKE_CXX_STANDARD 23`, which added a per-source scan step and compiled
  everything with GCC's experimental `-fmodules-ts`. Amarian uses headers, and
  consensus code should not be built through an experimental front-end mode.
  Build steps for `dev` dropped 24 → 12.
- Test tiers labelled `unit`, `consensus`, `integration` so `ctest -L consensus`
  can gate a release on its own.

**`util` layer**

- `types`: `ByteVec`/`ByteSpan`, `Hash256` with internal and display byte order
  kept deliberately distinct, `ConstantTimeEqual`.
- `hex`: strict codec — rejects odd length, non-hex characters, whitespace and
  `0x` prefixes.
- `overflow`: `CheckedAdd`/`Sub`/`Mul`, `TryAccumulate`, `TryNarrow`, with the
  properties consensus depends on pinned by `static_assert`.
- `logging`: level and category filtering behind one atomic load; errors and
  warnings bypass category filters; deterministic mode for diffing two nodes'
  logs.
- `args`: schema-driven CLI parsing. Unknown, duplicate, malformed and
  missing-value options are hard errors.
- `result`: `std::expected`-based `Result<T>`/`Status`.

**Node**

- `amariand` with `--version`, `--build-info`, `--help` and logging options.
  Exits non-zero with an explicit message instead of pretending to run a node.
- Build identity split between configure time (version, commit, compiler,
  dependency versions) and run time (the OpenSSL actually loaded).
- `PROTOCOL_VERSION` separate from the release version, pinned by
  `static_assert`.

**Fuzzing**

- `fuzz_hex` (attacker-facing: hex arrives from RPC arguments and config files)
  and `fuzz_args` (operator-facing: the CLI parser). Both assert correctness
  properties, not only memory safety — round-tripping, length agreement,
  determinism across two independent parses, no value stored that did not appear
  in argv, and no schema mutation during parsing.
- Properties use a `FUZZ_CHECK` macro rather than `assert`, because the `fuzz`
  preset is `RelWithDebInfo` and `assert` would compile out to nothing.
- Recipe and the throughput finding that makes it usable: [fuzz/README.md](fuzz/README.md).

**Benchmarks**

- `bench_util` covering the hex codec and checked arithmetic, built in the
  shipping configuration — Release *with* hardening — so the numbers describe
  what ships rather than a faster hypothetical build.
- Checked arithmetic is measured against three baselines, not one, because a
  naive raw-vs-checked ratio conflates the overflow check with the fact that a
  plain reduction auto-vectorises. Details in
  [bench/util_overflow_bench.cpp](bench/util_overflow_bench.cpp).

**Documentation**

- Nine documents in `docs/`, plus `README.md`, `SECURITY.md` and this file. Every
  one states what is implemented and what is design intent, because a document
  that reads as a specification of working software when the software does not
  exist is the most expensive kind of wrong.
- [docs/DECISIONS.md](docs/DECISIONS.md) records 30 decisions with the evidence
  behind each and the condition that would reverse it.
- [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) enumerates attack classes with a
  per-class *Tested* column, currently "no" in six of nine rows. That column is
  the input to Phase 9, whose criterion is that every row has a test that fails
  when the defence is removed.
- [docs/AMARIAN_PROTOCOL.md](docs/AMARIAN_PROTOCOL.md),
  [docs/NETWORK.md](docs/NETWORK.md) and [docs/WALLET.md](docs/WALLET.md) are
  design intent for Phases 1, 4 and 5 and are labelled as such at the top.

**Post-quantum measurement campaign**

- Key and signature sizes measured with `openssl genpkey`/`pkeyutl`, matching
  FIPS 204 and FIPS 205 exactly; per-operation costs measured with
  `openssl speed -seconds 2`. All figures and their caveats in
  [docs/PQ_CRYPTO.md](docs/PQ_CRYPTO.md).
- The finding that shaped the design: ML-DSA-44 verifies in 136 µs against
  Ed25519's 129 µs on the same machine, so verification CPU is 0.064 s for a full
  block against a 300 s interval — not a constraint. Authorisation data is 38.9×
  larger, giving 6.7× fewer transactions per block. **Size is the constraint, not
  CPU**, which is the opposite of the usual assumption.

## Current task

Phase 1 — minimal blockchain. Serialisation codec, primitives, consensus rules,
UTXO set, chainstate persistence, genesis block.

Acceptance criterion: **two local nodes independently validate the same chain.**

## Next task

Phase 2 — hard-cap monetary system, with the acceptance criterion that invalid
inflation attempts are rejected. The supply schedule is already designed and
analysed in [docs/ECONOMICS.md](docs/ECONOMICS.md); Phase 2 is where it becomes
consensus-enforced and adversarially tested.

## Blockers

None.

## Known risks

| Risk | Assessment |
|---|---|
| Post-quantum signature sizes inflate transactions and the UTXO set | **Measured, not estimated.** ML-DSA-44 costs 3732 B of authorisation data per input against Schnorr's 96 B — 38.9× — which is 467 transactions per block instead of 3110. The UTXO-set half of this risk is closed by design: locks commit to a 32-byte hash, so a 1312-byte key costs the same as a 32-byte one in the set. The block-space half is real and is the honest price. Full numbers in [docs/PQ_CRYPTO.md](docs/PQ_CRYPTO.md). |
| Cryptographic agility increases the consensus surface | Mitigated by one generic spend-condition primitive with versioned algorithm identifiers rather than per-scheme special cases. Reviewed again in Phase 8. |
| Hybrid classical+PQ authorisation may not be worth its cost in generation 1 | Deliberately undecided. Phase 7 decides on benchmark evidence, not assumption. |
| Novel monetary curve (12.5% per-era decay) is less battle-tested than halving | Rationale and analysis in `docs/ECONOMICS.md`. The mechanism is strictly simpler than Bitcoin's in one respect: reward is a pure function of height with no bit-shift edge cases. |
| Single-implementation consensus risk | Inherent to a new chain. Mitigated by keeping the consensus surface small, deterministic and covered by explicit vector tests, so a second implementation is feasible later. |
| Difficulty algorithm choice | ASERT is the current intent, chosen over Bitcoin's 2016-block retarget for resistance to hashrate oscillation on a small chain. Not yet implemented; revisit with simulation evidence in Phase 3. |
| Design documented ahead of implementation | `AMARIAN_PROTOCOL.md`, `NETWORK.md` and `WALLET.md` specify structures no code implements yet, so they can silently drift from what gets built. Mitigated by labelling every one as design intent at the top, and by the rule that each phase ends with its documentation updated to match what was actually built. When Phase 1 lands, either the structures match or the document is wrong — and the code is what the network runs. |

## Test results

Recorded from actual runs, 2026-09-04, 12 × 2.5 GHz x86-64.

Full preset matrix, re-run after the module-scanning change:

| preset | compiler | configuration | result |
|---|---|---|---|
| `dev` | GCC 15.2.0 | RelWithDebInfo | 47/47 passed |
| `debug` | GCC 15.2.0 | `-O0 -g` | 47/47 passed |
| `clang-dev` | Clang 21.1.8 | RelWithDebInfo | 47/47 passed |
| `asan` | Clang 21.1.8 | ASan + UBSan | 47/47 passed |
| `tsan` | Clang 21.1.8 | TSan | 47/47 passed |
| `release` | GCC 15.2.0 | Release | builds; `--version` runs |
| `fuzz` | Clang 21.1.8 | libFuzzer + ASan/UBSan | both targets build and run |
| `bench` / `bench-clang` | GCC / Clang | Release + hardening | build and run |

The 47 are 42 `test_util` and 5 `test_version`, counted with
`--gtest_list_tests` rather than estimated.

Fuzzing, 120 s per target:

| target | executions | rate | corpus | findings |
|---|---|---|---|---|
| `fuzz_hex` | 19 384 470 | 160 202/s | 30 units, cov 40 / ft 74 (saturated) | none |
| `fuzz_args` | 1 502 126 | 12 414/s | 105 units | none |

No crashes, leaks, timeouts or OOMs. This is evidence of absence of *shallow*
bugs in two small parsers over one session, and nothing more; `fuzz_hex` reaching
coverage saturation means further time on it is wasted, not that it is proven
correct.

Benchmarks (`--benchmark_min_time=0.2s`, Release + hardening):

| benchmark | GCC 15.2 | Clang 21.1 |
|---|---|---|
| `ToHex`, 1 MiB | 582 MiB/s | — |
| `ToHex`, 4 KiB | — | 922 MiB/s |
| `FromHex`, 1 MiB | 75.3 MiB/s | — |
| `Hash256::ToHex` | 81.7 ns | 54.8 ns |
| `Hash256FromHex` | 114 ns | — |
| `TryAccumulate`, per add | 8.67 ns | 0.44 ns |

The last row is a real 21× compiler disagreement, diagnosed rather than averaged
away — see Architectural decisions. Consensus-code benchmarks arrive with the
consensus code.

## Architectural decisions

Recorded with dates and reasoning in [docs/DECISIONS.md](docs/DECISIONS.md). Two
landed this session and are summarised here because they changed the build:

**Checked arithmetic stays as it is, despite a 21× GCC penalty.** `TryAccumulate`
costs 8.67 ns per addition under GCC 15.2 and 0.44 ns under Clang 21.1, for
identical source. The overflow check itself is free on both — `__builtin_add_overflow`
matches an unchecked scalar loop to within noise. It is also not `std::optional`
as such: `CheckedMul`, which also returns `std::optional`, costs 0.44 ns under
GCC. The penalty comes from carrying the *unwrapped* value into the next
iteration; the assembly shows GCC spilling the optional to the stack and
reloading it through an SSE register every iteration, putting store-to-load
forwarding on the loop-carried dependency, where Clang emits `addq` + `jo`.
Toggling `_GLIBCXX_ASSERTIONS`, `_FORTIFY_SOURCE`, `-fno-strict-aliasing` and
`-fwrapv` individually changes nothing, so it is codegen and not a hardening
cost. Kept because 20 000 outputs at 8.67 ns is ~0.2 ms against a 300 s block
interval, post-quantum verification will dominate this by orders of magnitude,
and `std::optional` makes it impossible to read a result that overflowed — which
in consensus code is worth far more than 0.2 ms per block.

**Fuzzing needs `-print_funcs=0` on this host.** Throughput is 222 exec/s with
libFuzzer's `NEW_FUNC` symbolisation on and 160 000 exec/s with it off, because
`llvm-symbolizer` parses the DWARF of an ASan+UBSan build across the drvfs mount
once per newly covered function. Crash traces use ASan's own symbolisation path
and are unaffected. Recorded in [fuzz/README.md](fuzz/README.md) so it is not
rediscovered by watching a fuzzer appear to hang.
