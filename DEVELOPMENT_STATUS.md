# Development Status

Living record of where the project actually is. Updated as work lands, not as
work is planned. Anything not listed as done is not done.

**Last updated:** 2026-09-05
**Current phase:** Phase 1 — Minimal blockchain
**Phase 0 status:** complete
**Phase 1 status:** in progress. Canonical encoding, consensus hashing, the
transaction and block primitives, the chain parameters, the compact target codec, the
issuance schedule, genesis for all three networks, the context-free validation rules,
the signature scheme registry, the signature hash, spend authorisation, the two
contextual transaction rules that need chain state — that an input's outpoint exists and
is unspent, and coinbase maturity — and the unspent output set with atomic block
application and reversal have landed. Chain selection, chainstate persistence and the
two-node acceptance test remain.

---

## Phase 0 acceptance criteria

| Criterion | State | Evidence |
|---|---|---|
| Clean build | met | Nine presets configure and build with no warnings and `-Werror` on; see the matrix under Test results |
| Clean test command | met | `ctest --preset dev` — 47/47 when Phase 0 closed, 246/246 now, and the same count on `debug`, `clang-dev`, `asan`, `tsan` |
| Basic executable | met | `amariand --version`, `--build-info`, `--help` |
| Basic project documentation | met | `README.md`, `SECURITY.md`, nine documents in `docs/`, `fuzz/README.md`, this file |

## Phase 1 acceptance criteria

Acceptance is one criterion and it is not met yet: **two local nodes independently
validate the same chain.** Progress towards it, by component:

| Component | State |
|---|---|
| Canonical serialisation codec (`util/serialize`) | landed — fuzz harness and benchmark; part of the 72 `test_util` tests |
| Consensus hashing (`crypto/hash`) | landed — 4 tests against published vectors, independently recomputed |
| Transaction primitives and Merkle root (`primitives/`) | landed — part of the 66 `test_primitives` tests |
| Block header and block | landed — 92-byte header, weight from the encodings, height in the header |
| Chain parameters, networks, compact target codec, PoW check | landed — part of the 52 `test_consensus` tests |
| Issuance schedule as a consensus rule | landed — `MAX_MONEY` is a `static_assert` computed from the schedule |
| Genesis block, reward zero enforced as a rule | landed — three networks mined, recomputed and checked at node startup |
| `consensus::ValidationError` — allocation-free, enum-reasoned | landed — 52 named rules, total `switch`, no `default` |
| Context-free transaction, header and block validation | landed — tested |
| Scheme registry and BIP-340 Schnorr verification | landed — 15 `test_crypto` tests, including the BIP-340 vectors and both post-quantum schemes; `amariand` refuses to start if a registered scheme is unavailable |
| Signature hash | landed — fixed 181-byte preimage, midstates reused once per transaction, 15 tests |
| Spend authorisation (lock commitment, threshold walk, fee) | landed — `CheckSpendAuthorisation` and `TransactionFee` over `std::span<const Coin>`, 16 tests against real ML-DSA-44 signatures |
| Contextual transaction rules that need state (outpoint exists and unspent, maturity) | landed — `CheckTransactionInputs`, cheapest rule first and cryptography last |
| UTXO set with apply and revert | landed — `amarian::utxo`: the layered coins cache, `ConnectBlock`/`DisconnectBlock`, the undo record and its encoding; 19 `test_utxo` tests |
| Accumulated work as a comparable quantity | landed — `amarian::Work`, a 256-bit big-endian integer with saturating addition; 8 of the 52 `test_consensus` tests, including agreement with the published difficulty-one chainwork constant |
| Block index and chain selection | landed — `amarian::chain::BlockIndex`: the header tree, work per branch, the best-tip rule with a first-seen tie-break, inherited rejection, and `PlanChainSwitch`; 17 `test_chain` tests over real mined regtest headers |
| Applying a chain switch (`ActivateBestChain`) | landed — `amarian::chain::ChainState`: the active chain indexed by height, `AcceptBlock`, and an activation loop that reverses and applies blocks so the coins set follows the tip. Per-block atomicity, and a body-availability guard that makes a tip regression impossible rather than unlikely. Covered by the existing `test_chain` and `test_utxo` suites; a dedicated body-withholding test belongs with the P2P layer that can actually withhold |
| Block and undo storage behind an interface | landed — `amarian::chain::BlockStore` with a complete in-memory implementation, so `chain` still links no database |
| RocksDB chainstate persistence | not started — the next component: a `CoinsView`/`CoinsSink` and a `BlockStore` over storage |
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
- `signature`: the scheme registry. A `uint16_t` identifier resolves to a name, a key
  length, a signature length, a class (classical or post-quantum), a backend name and
  an availability probe. Three schemes are registered: BIP-340 Schnorr over
  libsecp256k1, ML-DSA-44 and SLH-DSA-SHA2-128s over OpenSSL's default provider. The
  table is checked dense, ascending and self-consistent by a test rather than by
  inspection.
- `crypto::Verify` returns a five-way `VerifyResult` — `Valid`, `Invalid`,
  `Malformed`, `UnknownScheme`, `Reserved` — and not a boolean, because "this is a
  forgery" and "I cannot check this" must lead a validator to opposite conclusions:
  the first is a rejection, the second is the soft-fork path. Lengths are checked
  against the registry before any implementation is entered, so a library never sees
  bytes that could not have parsed.
- **There is no signing interface, deliberately.** A node verifies; it never signs.
  The only exceptions are two test binaries that must produce a real signature to
  have anything to verify, and `tests/unit/CMakeLists.txt` records why for each.

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
- `BlockHeader` is fixed at 92 bytes and carries its own height. `Block::Weight`
  and `Transaction::Weight` are `base × 4 + witness`, measured by serialising rather
  than by a second size calculation that could disagree.
- The coinbase is a tagged union whose tag is the input itself. `IsCoinbase()` is
  decidable from the inputs, which precede the witness section on the wire, so a
  decoder always knows whether a witness list or the coinbase's single arbitrary byte
  string follows — without looking at the enclosing block.
- `sighash`: the message a signature commits to. The preimage is **181 bytes
  regardless of transaction size**, because the outpoint list, sequence list and
  output list each enter it as a single pre-computed midstate hash. That is the
  anti-quadratic property, and `SigHashMidstates` is a separate type so that the
  once-per-transaction cost cannot accidentally become once-per-input. The preimage
  commits to the chain id, the input index, the spent amount and the revealed
  condition, which is what makes a signature unreplayable across networks, inputs,
  amounts and conditions.

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
- `ValidationError` names 48 distinct rules with no shared "invalid" value, so
  rejection allocates nothing on an attacker-driven path. `Describe` turns one into
  text at the edges only, over a total `switch` with no `default`.
- The context-free rules — `CheckSpendCondition`, `CheckWitness`, `CheckTransaction`,
  `CheckBlockHeader`, `ContextualCheckBlockHeader`, `CheckBlock` and
  `CheckCoinbaseAmount` — are pure functions of their explicit inputs, ordered
  cheapest-first because the order decides how much work an attacker can make a node
  do before rejection.
- Spend authorisation — `CheckSpendAuthorisation` and `TransactionFee` — takes the
  coins being spent as a `std::span<const Coin>` rather than a database handle.
  Finding those coins is a lookup; deciding whether they may be spent is arithmetic
  and cryptography. Splitting them keeps the expensive half a pure function of
  values, which is what lets it be tested exhaustively without a database and, later,
  run on several threads without a lock. It is a span of `Coin` rather than `TxOutput`
  because maturity needs each coin's creation height, and after a reorganisation the
  block that created it may not be reachable.
- A threshold is satisfied by **ordered forward match**: one key index advances
  monotonically across the whole signature list. At most `keys.size()` verifications
  happen per input however many signatures are offered, and exactly one ordering of a
  given signature set verifies — so the cost is bounded by the condition the coin's
  own commitment named, and a permuted witness is invalid rather than a second
  encoding of a valid one.
- At both soft-fork extension points an unknown value stays spendable: an unknown lock
  version without a signature check, an unknown key scheme counted as satisfied. This
  is the only place consensus accepts something it has not checked, and it is why
  `amariand` refuses to start when a *registered* scheme is unavailable from its
  backend — otherwise a node with the wrong OpenSSL would report a known scheme as
  unknown and accept every spend under it while believing it was verifying.
- `CheckTransactionInputs` is the contextual entry point, and its rules run cheapest
  first: reject a coinbase, check the coin count against the input count, check maturity
  in integers, compute the fee in integers — which is where an attempt to mint is caught
  — and verify cryptography last. Every rule above the signature check is one an
  attacker cannot make a node pay for.
- A block that passes `CheckBlock` is still not valid on its own: `CheckBlock` is
  context-free and does not touch the UTXO set, because the coins being spent are not
  something a block carries. Validity is the header checks, then `CheckBlock`, then
  `ConnectBlock` against the set at the predecessor. Each function is named for the half
  it does.

**Unspent output set**

- `amarian::utxo` — three types with one job each. `CoinsView` is the read interface, so
  an empty set, an in-memory map and a future RocksDB column family are interchangeable
  and consensus never sees any of them. `CoinsSink` is the write interface: one call,
  "the truth about this outpoint is now *this*". `CoinsCache` is both, over a base view,
  and is the only mutable UTXO set in the system.
- **No read-through caching.** A cache holds only entries that *differ* from its base, so
  its map is exactly the change set — no dirty flags, no `mutable`, and a flush that
  writes precisely what changed. A read for an untouched outpoint costs a base lookup;
  what it buys is that "what is in the map" and "what must be written" are the same
  question, which is the property a reorganisation has to be able to trust.
- Spending a coin the base never had erases the entry rather than leaving a tombstone.
  Without that, a root cache would accumulate one tombstone per coin ever spent and the
  in-memory set would grow with the chain's whole history instead of its unspent output
  count.
- `ConnectBlock` and `DisconnectBlock` are **atomic by nesting**: each stages every change
  in its own cache over the caller's set and flushes only after the last rule passes. A
  block rejected half way through leaves the set exactly as it was. There is no rollback
  path, because not flushing is not an action — which removes the code least likely to be
  correct and least likely to be exercised.
- Applying transactions in order, removing each coin as it is found, is what makes
  intra-block dependencies and intra-block double spends fall out of the mechanism rather
  than needing separate rules. A transaction may spend an output created earlier in the
  same block and not one created later; a second spend of one outpoint finds nothing.
- `BlockUndo` records the full contents of every coin each non-coinbase transaction spent,
  because connecting is not invertible from the block alone — a block names spent
  outpoints, not their contents. It has an encoding, bounded the same way every other
  decoder is, because a reorganisation may begin after a restart.
- Disconnecting compares every coin it removes for **full equality** against what the
  block says it created, not merely for presence. An approximate reversal is a silent
  chain split: two nodes that both believe they are on the same chain, holding different
  money, with nothing having rejected anything.
- Provably unspendable outputs — lock version `0` — are never stored, via one `IsStored`
  predicate that connect and disconnect both call, so "the same outputs were skipped" is a
  fact rather than a hope. Unknown lock versions are stored normally.
- Creating an outpoint that is already unspent is rejected even though Amarian makes it
  unreachable by construction, because that construction is an *argument* that a future
  change could invalidate, and the cost of being wrong is a live coin silently
  overwritten.
- `DisconnectError` is a separate enumeration from `ValidationError`. A failed connect is
  an invalid block and the node carries on; a failed disconnect means the undo record and
  the set no longer describe each other, and there is no valid continuation from that. Two
  types make the wrong reaction unwritable.
- The bug worth recording: `CoinsCache` is non-copyable and built only through
  `CoinsCache::Over`, because `CoinsCache batch(coins)` silently resolved to the *copy*
  constructor — an exact match beats the derived-to-base conversion to `const CoinsView&`
  — producing a duplicate of the parent's change set sharing the parent's base instead of
  a layer above it. Four tests failed; the defect was live in `ConnectBlock`, meaning a
  rejected block could have left the real set edited. Deleting the copy operations makes
  the mistake unwritable rather than merely documented.

**Node**

- `amariand` with `--version`, `--build-info`, `--help` and logging options.
  Exits non-zero with an explicit message instead of pretending to run a node.
- Two startup checks, both refusals rather than warnings. Every registered signature
  scheme must be usable from this build's backend, because a node that cannot verify a
  consensus scheme would accept spends under it unchecked. And this build's block 0
  must be the selected network's, because a node whose genesis differs shares no
  history with the network at all.
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
- [docs/DECISIONS.md](docs/DECISIONS.md) records 63 decisions with the evidence
  behind each and the condition that would reverse it.
- [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) enumerates attack classes with a
  per-class *Tested* column. Thirty-eight individual rows now read **yes**; the
  spend-authorisation class went from "no verification" to fully implemented, and the
  supply class closed its last two open rows — the coinbase fee bound and the phantom
  fee claim — when the UTXO set landed, both on 2026-09-05. That column is the input to
  Phase 9, whose criterion is that every row has a test that fails when the defence is
  removed — strictly stronger than what a **yes** claims today.
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

The block index and chain selection. With the unspent output set in place, every rule
that consults a coin is enforced — but nothing yet decides *which* block to apply:
`ConnectBlock` is handed a block and a height by a caller, and a caller that chooses
wrongly is not something any rule below it can detect. The block index is what turns a
pile of validated blocks into a chain: the tree of known headers, accumulated work per
branch, a total order with deterministic tie-breaking, and the reorganisation walk that
disconnects back to a fork point and connects forward along the new branch.

That last part is the reason the UTXO layer was built with an exact, atomic reversal
rather than an approximate one. A reorganisation is where `DisconnectBlock` is called for
real, and where "restores exactly what was removed" stops being a test assertion and
becomes the thing standing between two nodes agreeing and two nodes silently disagreeing.

Chain selection is also the row [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) marks
"reorganisation without hashrate — this is the actual bug to hunt", and it is the only
Critical row in the proof-of-work class that is a software defect rather than an economic
property.

## Next task

RocksDB chainstate persistence — a `CoinsView` over storage, blocks and undo records on
disk — then node wiring and the two-node integration test that closes Phase 1. The view
interface was shaped for this: a storage-backed view is a new `CoinsView` subclass and
nothing above it changes.

After that, Phase 2 — hard-cap monetary system, with the acceptance criterion that
invalid inflation attempts are rejected. The supply schedule is designed and analysed in
[docs/ECONOMICS.md](docs/ECONOMICS.md), and the two halves that had to meet now have:
`ConnectBlock` sums each transaction's fee from the coins it actually spent and hands the
total to `CheckCoinbaseAmount`, so the cap is enforced per block from height with no
running total to trust. Phase 2 is where that is adversarially tested rather than merely
exercised.

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

Full preset matrix, re-run after the unspent output set landed:

| preset | compiler | configuration | result |
|---|---|---|---|
| `dev` | GCC 15.2.0 | RelWithDebInfo | 246/246 passed |
| `debug` | GCC 15.2.0 | `-O0 -g` | 246/246 passed |
| `clang-dev` | Clang 21.1.8 | RelWithDebInfo | 246/246 passed |
| `asan` | Clang 21.1.8 | ASan + UBSan, integer findings fatal | 246/246 passed |
| `tsan` | Clang 21.1.8 | TSan | 246/246 passed |
| `release` | GCC 15.2.0 | Release | builds |
| `bench` / `bench-clang` | GCC / Clang | Release + hardening | build |
| `fuzz` | Clang 21.1.8 | libFuzzer + ASan/UBSan | all six executables build |

Zero compiler warnings on every one of the nine, with `-Werror` on. Reproduced by
[scripts/preset_matrix_check.sh](scripts/preset_matrix_check.sh), which builds each
preset, counts `warning:` lines and reports the pass line, so the table above is a
transcript rather than a recollection.

The 246 are 72 `test_util`, 66 `test_primitives`, 52 `test_consensus`, 19 `test_utxo`,
17 `test_chain`, 15 `test_crypto` and 5 `test_version`, counted with
`--gtest_list_tests` by [scripts/count_tests.sh](scripts/count_tests.sh) rather than
estimated.

Genesis, verified end to end rather than asserted: `amarian-genesis --check` reports
`check ok` for mainnet, testnet and regtest — each block 257 bytes, weight 812 — and
`amariand --chain <network>` recomputes block 0 from the recorded fields at startup
and reports the matching chain id, magic and genesis hash for each.

Startup on regtest, verbatim, as the record of what both gates actually print:

```
[info] [general] signature scheme 1 schnorr-secp256k1 (classical, 32 byte key, 64 byte signature) via libsecp256k1
[info] [general] signature scheme 2 ml-dsa-44 (post-quantum, 1312 byte key, 2420 byte signature) via openssl
[info] [general] signature scheme 3 slh-dsa-sha2-128s (post-quantum, 32 byte key, 7856 byte signature) via openssl
[info] [general] network regtest (chain_id cca51fe05365e7cdd35bc3ef919f1c7426a2c6285a03028b98ddf143c0d4b975)
[info] [general] magic f5b9d4c0, p2p port 12520, rpc port 12521
[info] [general] genesis 02248d2fa761c196fd9a63f7a44c1efbca3acf2e882c3f1c707aba6237bda967
[error] [general] no chain backend in this build: block storage, validation and the network layer land in Phase 1.
```

The key and signature sizes in those lines are read from the registry and cross-checked
against the backend, so they are the sizes this build will actually accept — not the
sizes the documentation claims.

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

Recorded with dates and reasoning in [docs/DECISIONS.md](docs/DECISIONS.md), now 47
entries. Four are summarised here because they changed the build, the fuzzing
evidence, or a correctness guarantee that a test alone would not have caught:

**A coins cache silently resolved to its copy constructor.** `CoinsCache` derives from
`CoinsView` and originally took `explicit CoinsCache(const CoinsView& base)`. The
expression `CoinsCache batch(coins)` where `coins` is itself a `CoinsCache` therefore did
not call that constructor: the implicit copy constructor is an exact match and beats the
derived-to-base conversion. The result was a duplicate of the parent's change set sharing
the parent's base rather than a layer above it — so tombstones the batch needed were
erased instead of created, and flushing wrote the parent's own entries back over the
parent while losing the batch's. Four tests failed and were traced by hand against that
prediction before anything was changed. The defect was live in `ConnectBlock` and
`DisconnectBlock`, which is to say a rejected block could have left the node's real UTXO
set edited. Fixed by making the constructor private behind
`[[nodiscard]] static CoinsCache Over(const CoinsView&)` and deleting copy and move, so
the mistake cannot be written rather than merely being documented. Full entry: decision
42.

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
