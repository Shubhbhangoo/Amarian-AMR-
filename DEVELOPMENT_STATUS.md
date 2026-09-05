# Development Status

Living record of where the project actually is. Updated as work lands, not as
work is planned. Anything not listed as done is not done.

**Last updated:** 2026-09-05
**Current phase:** Phase 1 — Minimal blockchain
**Phase 0 status:** complete
**Phase 1 status:** in progress. Canonical encoding, consensus hashing, the
transaction and block primitives, the chain parameters, the compact target codec, the
issuance schedule, genesis for all three networks, and the context-free validation
rules have landed. The scheme registry, the signature hash, the contextual
transaction rules, the UTXO set, chain selection, chainstate persistence and the
two-node acceptance test remain.

---

## Phase 0 acceptance criteria

| Criterion | State | Evidence |
|---|---|---|
| Clean build | met | Nine presets configure and build with no warnings and `-Werror` on; see the matrix under Test results |
| Clean test command | met | `ctest --preset dev` — 47/47 when Phase 0 closed, 160/160 now, and the same count on `debug`, `clang-dev`, `asan`, `tsan` |
| Basic executable | met | `amariand --version`, `--build-info`, `--help` |
| Basic project documentation | met | `README.md`, `SECURITY.md`, nine documents in `docs/`, `fuzz/README.md`, this file |

## Phase 1 acceptance criteria

Acceptance is one criterion and it is not met yet: **two local nodes independently
validate the same chain.** Progress towards it, by component:

| Component | State |
|---|---|
| Canonical serialisation codec (`util/serialize`) | landed — fuzz harness and benchmark; part of the 72 `test_util` tests |
| Consensus hashing (`crypto/hash`) | landed — 4 tests against published vectors, independently recomputed |
| Transaction primitives and Merkle root (`primitives/`) | landed — part of the 51 `test_primitives` tests |
| Block header and block | landed — 108-byte header, weight from the encodings, height in the header |
| Chain parameters, networks, compact target codec, PoW check | landed — part of the 28 `test_consensus` tests |
| Issuance schedule as a consensus rule | landed — `MAX_MONEY` is a `static_assert` computed from the schedule |
| Genesis block, reward zero enforced as a rule | landed — three networks mined, recomputed and checked at node startup |
| `consensus::ValidationError` — allocation-free, enum-reasoned | landed — 38 named rules, total `switch`, no `default` |
| Context-free transaction, header and block validation | landed — **no tests yet**, which is the next thing to fix |
| Scheme registry and BIP-340 Schnorr verification | not started |
| Signature hash | not started |
| Contextual transaction rules (outpoint exists, maturity, commitment, sums, signatures) | not started |
| UTXO set with apply and revert | not started |
| Block index and chain selection | not started |
| RocksDB chainstate persistence | not started |
| Node wiring and the two-node integration test | not started |

Difficulty *retargeting* is deliberately not on this list: Phase 1 uses a constant
target, and the retarget algorithm arrives in Phase 3 where it can be evaluated
against simulated hashrate rather than asserted.

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
- Sanitizer presets with ASan/TSan mutual exclusion,
  `-fno-sanitize-recover=undefined` and `-fno-sanitize-recover=integer`. The
  second was missing until 2026-09-05: `-fsanitize=integer` is not part of the
  `undefined` group, so its findings were recoverable and a fuzz run that hit one
  still exited zero. One did. The report was a defined-but-flagged conversion
  inside libstdc++'s `<charconv>`, not an Amarian defect, so the fix was to make
  such findings fatal and scope only the `implicit-*` checks away from standard
  library headers via [cmake/sanitizer-ignorelist.txt](cmake/sanitizer-ignorelist.txt).
  `scripts/ubsan_charconv_probe.sh` establishes that the check still fires on our
  own translation units afterwards.
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
- `serialize`: the canonical codec, and the security perimeter — every byte a node
  acts on arrives through `Reader`. Non-minimal compact sizes are **rejected, not
  normalised**, because accepting two encodings of one value gives one transaction
  two ids. Counts are bounded against the bytes actually remaining before anything
  is allocated, via a per-element minimum size. Failure is sticky, so a partial
  parse cannot be mistaken for a short one. `Finish()` requires exact consumption,
  so trailing bytes are a parse failure rather than debris. Failed reads write
  nothing to their output.

**`crypto` layer**

- `hash`: `Sha256`, `DoubleSha256`, and `TaggedHash` — the BIP-340 construction
  `SHA256(SHA256(tag) ‖ SHA256(tag) ‖ x)`. No primitive is implemented here;
  OpenSSL does the hashing and this layer exists so that nothing else in the tree
  sees an OpenSSL type. OpenSSL is linked `PRIVATE` to `amarian_crypto`, so that
  boundary is a link error rather than a review comment.

**`primitives` layer**

- `OutPoint`, `PublicKey`, `Signature`, `SpendCondition`, `Lock`, `Witness`,
  `TxInput`, `TxOutput`, `Transaction`, and the Merkle root. Structure and encoding
  only: no rule decides validity here, so a threshold of zero decodes successfully
  and is rejected later as a rule violation with its own reason. Conflating the two
  would make a malformed message indistinguishable from an invalid one.
- `txid` covers version, inputs, outputs and locktime; `wtxid` covers everything.
  The split is *before* the witness count, so the count belongs to the witness
  section — one field either side and the txid would depend on how many witnesses a
  transaction carries, which is the malleability the split removes.
- Output amounts are range-checked into `[0, MAX_MONEY]` at deserialisation, so no
  code path in the node can hold a `TxOutput` with a negative or supply-violating
  amount.
- The Merkle tree carries three independent defences against CVE-2012-2459:
  separate leaf and branch tags, odd nodes promoted unchanged rather than
  duplicated, and the leaf count committed under a third tag. All three would have
  to fail to reintroduce the vulnerability.
- `BlockHeader` is fixed at 108 bytes and carries its own height. `Block::Weight`
  and `Transaction::Weight` are `base × 4 + witness`, measured by serialising rather
  than by a second size calculation that could disagree.
- The coinbase is a tagged union whose tag is the input itself. `IsCoinbase()` is
  decidable from the inputs, which precede the witness section on the wire, so a
  decoder always knows whether a witness list or the coinbase's single arbitrary byte
  string follows — without looking at the enclosing block.

**`consensus` layer**

- Links `primitives` and nothing else. No storage, no networking, no wallet, no RPC:
  the absence of those edges is enforced by CMake, not by convention, and it is what
  keeps the surface that decides which chain is real small enough to audit.
- `BlockReward` starts at 10 AMR and multiplies by 7/8 every 105 000 blocks, paying a
  nonzero reward in eras 0–178 and nothing after height 18 795 000. `MAX_MONEY =
  83 999 999 932 170 000` facets is a `static_assert` that sums the schedule at
  compile time, so the constant and the schedule cannot drift apart.
- Compact target codec, canonical by round-tripping through `TargetToCompact`. The
  block hash is compared as a big-endian integer in *display* order — the digest's
  last byte is the most significant — which is pinned in code and in the protocol
  document because getting it wrong is a consensus split.
- `ChainParams` is a value passed to every rule, not an ambient current network. Each
  network's chain id is bound into its genesis block's unspendable lock program, so
  the three genesis hashes differ by construction and a `static_assert` checks it.
- Genesis is mined, recorded, and recomputed at node startup: `amariand --chain`
  refuses to run if this build's block 0 is not the selected network's, because a node
  whose block 0 differs shares no history with the network at all.
- `ValidationError` names 38 distinct rules with no shared "invalid" value, so
  rejection allocates nothing on an attacker-driven path. `Describe` turns one into
  text at the edges only, over a total `switch` with no `default`.
- The context-free rules — `CheckSpendCondition`, `CheckWitness`, `CheckTransaction`,
  `CheckBlockHeader`, `ContextualCheckBlockHeader`, `CheckBlock` and
  `CheckCoinbaseAmount` — are pure functions of their explicit inputs, ordered
  cheapest-first because the order decides how much work an attacker can make a node
  do before rejection. **They have no tests yet.** A block that passes `CheckBlock` is
  not yet valid: the contextual transaction rules need the UTXO set and the scheme
  registry, and the function is named for the half it does.

**Node**

- `amariand` with `--version`, `--build-info`, `--help` and logging options.
  Exits non-zero with an explicit message instead of pretending to run a node.
- Build identity split between configure time (version, commit, compiler,
  dependency versions) and run time (the OpenSSL actually loaded).
- `PROTOCOL_VERSION` separate from the release version, pinned by
  `static_assert`.

**Fuzzing**

- `fuzz_hex` (attacker-facing: hex arrives from RPC arguments and config files),
  `fuzz_args` (operator-facing: the CLI parser) and `fuzz_serialize` (attacker-facing
  and the widest surface in the project: every byte a node acts on passes through
  `Reader`). All assert correctness properties, not only memory safety — round-tripping,
  length agreement, determinism across two independent parses, no value stored that
  did not appear in argv, no schema mutation during parsing, cursor monotonicity and
  exact width, sticky total failure, no writes on a failed read, and accepted compact
  sizes re-encoding to themselves.
- `fuzz_serialize` parses each input twice and requires identical results. A decoder
  that is non-deterministic across two runs of the same bytes is a chain split.
- Properties use a `FUZZ_CHECK` macro rather than `assert`, because the `fuzz`
  preset is `RelWithDebInfo` and `assert` would compile out to nothing.
- Recipe and the throughput finding that makes it usable: [fuzz/README.md](fuzz/README.md).
  `scripts/fuzz_session.sh` wraps the recipe so a session is one command.

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
- [docs/DECISIONS.md](docs/DECISIONS.md) records 34 decisions with the evidence
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

Tests for the context-free validation rules. They are the only component in the
checklist above that landed without any, and everything after them — the UTXO set,
chain selection, the node — is built on top of them, so a rule that is wrong now is a
rule that is wrong under a growing pile of code. Focused rather than exhaustive: one
case per rule family, plus the ordering properties that the rules' security actually
depends on.

Then the `crypto` scheme registry and BIP-340 Schnorr verification through
libsecp256k1, followed by the signature hash. The registry is what makes
`PublicKey.scheme` mean something — a key length, a signature length, and a
verification function — and it is the seam the post-quantum work in Phase 6 arrives
through, so it is worth getting right before anything depends on it.

Acceptance criterion for the phase as a whole: **two local nodes independently
validate the same chain.** The component checklist above is the honest measure of
distance from it.

## Next task

The contextual transaction rules and the UTXO set with apply and revert, then the
block index and chain selection, then RocksDB chainstate persistence, then node wiring
and the two-node integration test.

After that, Phase 2 — hard-cap monetary system, with the acceptance criterion that
invalid inflation attempts are rejected. The supply schedule is already designed and
analysed in [docs/ECONOMICS.md](docs/ECONOMICS.md), and `CheckCoinbaseAmount` already
enforces the cap given a fee total; Phase 2 is where the fee total comes from the UTXO
set rather than from a parameter, and where the whole thing is adversarially tested.

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
| Design documented ahead of implementation | `NETWORK.md` and `WALLET.md` still specify structures no code implements, so they can silently drift from what gets built; both are labelled design intent at the top. `AMARIAN_PROTOCOL.md` is no longer in that position — the encodings, the target codec, the issuance schedule, genesis and the validation order it specifies are implemented, and its "gap between this document and the code" section names what is not. Its validation-order section and `src/consensus/validation.cpp` are meant to be read together; where they disagree, one of them is a bug, and writing the code already found one such disagreement in the document's favour of the wrong order. |

## Test results

Recorded from actual runs, 2026-09-05, 12 × 2.5 GHz x86-64.

Full preset matrix, re-run after the consensus layer landed:

| preset | compiler | configuration | result |
|---|---|---|---|
| `dev` | GCC 15.2.0 | RelWithDebInfo | 160/160 passed |
| `debug` | GCC 15.2.0 | `-O0 -g` | 160/160 passed |
| `clang-dev` | Clang 21.1.8 | RelWithDebInfo | 160/160 passed |
| `asan` | Clang 21.1.8 | ASan + UBSan, integer findings fatal | 160/160 passed |
| `tsan` | Clang 21.1.8 | TSan | 160/160 passed |
| `release` | GCC 15.2.0 | Release | builds |
| `bench` / `bench-clang` | GCC / Clang | Release + hardening | build |
| `fuzz` | Clang 21.1.8 | libFuzzer + ASan/UBSan | all six executables build |

Zero compiler warnings on every one of the nine, with `-Werror` on.

The 160 are 72 `test_util`, 51 `test_primitives`, 28 `test_consensus`, 5
`test_version` and 4 `test_crypto`, counted with `--gtest_list_tests` rather than
estimated. None of them covers `consensus/validation.cpp`, which is the gap named
under Current task.

Genesis, verified end to end rather than asserted: `amarian-genesis --check` reports
`check ok` for mainnet, testnet and regtest — each block 257 bytes, weight 812 — and
`amariand --chain <network>` recomputes block 0 from the recorded fields at startup
and reports the matching chain id, magic and genesis hash for each.

Fuzzing, 2026-09-05, ASan+UBSan with integer findings fatal:

| target | executions | rate | coverage | findings |
|---|---|---|---|---|
| `fuzz_serialize` | 21 121 422 in 601 s | 35 143/s | cov 106 / ft 525, 180 units | none |
| `fuzz_hex` | 6 572 590 in 45 s | 146 057/s | saturated, 30 units | none |
| `fuzz_args` | 1 112 793 in 45 s | 24 728/s | 105 units | none |

No crashes, leaks, timeouts, OOMs or UBSan reports. That zero means something it did
not mean a day earlier, when a UBSan *integer* finding could print and still let the
run exit zero — see decision 34. What it still does not mean: `fuzz_hex` has reached
coverage saturation, which says further time on it is wasted, not that it is correct.
Three small parsers over one session is evidence of the absence of *shallow* bugs and
nothing more.

`fuzz_serialize`'s resident set climbing to 878 MB with LeakSanitizer silent was
chased down rather than shrugged at: it is ASan's quarantine, bounded, and the
harness's real steady state is 18.5 MB across 712 000 invocations. Table and method
in [fuzz/README.md](fuzz/README.md).

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

Recorded with dates and reasoning in [docs/DECISIONS.md](docs/DECISIONS.md), now 34
entries. Three are summarised here because they changed the build or the fuzzing
evidence:

**UBSan integer findings were recoverable, so they were decorative.** The first fuzzing
session printed `charconv:531:66: runtime error: implicit conversion from type 'char' of
value -83 … changed the value to 173` and exited zero. `-fsanitize=integer` is not part
of the `undefined` group, so `-fno-sanitize-recover=undefined` never covered it. The
report itself was not an Amarian defect — a well-defined `char`→`unsigned char`
conversion inside libstdc++, reached from a correct `std::from_chars` call — but a check
that fires without failing the run makes a clean fuzz result meaningless. Fixed by
`-fno-sanitize-recover=integer` plus a Clang-only ignorelist scoping only the
`implicit-*` family away from `include/c++/*`. `scripts/ubsan_charconv_probe.sh` proves
the check still fires on our own translation units afterwards — necessary, because the
first ignorelist written was a silent no-op and nothing in the build output said so. Full
entry: decision 34.

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
