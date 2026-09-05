# Decisions

A log of decisions that were made rather than defaulted into, with the evidence that
produced each one and the condition that would reverse it. The last line of each entry
is the important one: a decision with no stated reversal condition is a decision
nobody can revisit.

Status values: **settled** — changing it changes the project; **intent** — decided but
not yet exercised by code; **implemented** — the code exists and is tested, and the
entry says where; **provisional** — deliberately open, with the deciding phase named.

Anything measured was measured on 12 × 2.5 GHz x86-64, WSL2 on Windows 11, on
2026-09-04 unless stated. Numbers from a laptop are labelled as such wherever they
appear.

## Environment and build

### 1. C++23 for the core node

**Settled.**

The requirement was the strongest practical systems language, with Rust as the fallback
and TypeScript only if native development were impossible. C++23 was not chosen for
familiarity; it was chosen after confirming, by running things rather than by reading
version strings, that the whole toolchain was actually present and working: GCC 15.2.0
and Clang 21.1.8, CMake 4.2.3 with Ninja, libsecp256k1 0.7.0 with a real BIP-340
signature round trip, OpenSSL 3.5.5 with a real ML-DSA-44 keygen whose output size was
checked against FIPS 204, RocksDB, Asio, GoogleTest, Google Benchmark, AFL++, libFuzzer,
four sanitizers, gdb and valgrind.

The deciding factor over Rust was not language quality. It was that libsecp256k1 and
OpenSSL's post-quantum implementations are C libraries, the mature consensus
implementations to compare against are C++, and every binding layer is a place a
correctness bug can hide in the one part of the system where a bug is a chain split.

**Reversed if:** a specific, demonstrated memory-safety failure in consensus code that
the sanitizer matrix did not catch. Not by preference.

### 2. Build inside WSL2, source on the Windows drive

**Settled.**

The source of truth is `E:\project Amarian`, visible to WSL as
`/mnt/e/project Amarian`, and builds happen in-tree at `build/<preset>`. The obvious
concern is drvfs filesystem overhead, so it was measured rather than assumed: compiling
on ext4 against the Windows drive differed by **under 1%**. Keeping the source where the
editor and git identity already live is worth more than that.

**Reversed if:** the penalty grows materially once there are hundreds of translation
units. Worth re-measuring at Phase 4, not before.

### 3. The layering rule is enforced by the linker

**Settled.**

`util ← crypto ← primitives ← consensus ← utxo ← chain`, with `consensus` never linking
storage, networking, wallet or RPC. Each layer is a separate CMake target linking only
its permitted dependencies, so a forbidden `#include` fails to link.

A reviewer can miss an include; a link error cannot be missed. The failure this
prevents is concrete: a consensus layer that can reach RocksDB can have a validation
outcome that depends on cache state, and one that can reach the network can have an
outcome that depends on which peer answered first. Both are chain splits waiting for
the right timing.

**Reversed if:** never. This is the load-bearing structural decision of the project.

### 4. Errors are values, not exceptions

**Settled.**

`Result<T>` is `std::expected<T, Error>`, with `Error` carrying a stable
machine-readable context string plus a human-readable detail — `args.invalid_integer`
with `--port=abc`. `AMARIAN_TRY` makes propagation explicit at every call site.

A validation failure is the *expected* outcome of processing untrusted input, not an
exceptional one, and an exception makes the propagation path invisible in exactly the
code that most needs it to be visible. The stable context strings have a second purpose
that matters more than tidiness: when two nodes disagree about a block, comparing why
each rejected it is the fastest route to the cause, and that only works if the reason is
a stable identifier rather than prose someone will reword.

**Reversed if:** never for validation. Exceptions remain acceptable for genuinely
exceptional conditions such as allocation failure.

### 5. Checked arithmetic keeps its API despite a measured 21× GCC penalty

**Settled**, and the most interesting measurement of Phase 0.

`CheckedAdd`/`Sub`/`Mul` return `std::optional`, so an overflowed result cannot be read
without acknowledging it. Benchmarked in the shipping configuration, nanoseconds per
operation:

| | GCC | Clang |
|---|---|---|
| Raw addition | 0.20 | 0.14 |
| Raw, guarded by a manual range check | 0.45 | 0.40 |
| `__builtin_add_overflow` directly | 0.41 | 0.36 |
| `CheckedAdd` returning `std::optional` | **8.67** | 0.44 |
| `CheckedMul` returning `std::optional` | 0.44 | 0.69 |

A **21× penalty under GCC and none under Clang**, for the same source. Diagnosed to the
instruction level: GCC fails to scalarise the `optional` return in the addition case
specifically, materialising it to the stack in the hot loop, while Clang does. Not an
inherent cost of the abstraction — a single missed optimisation in one compiler for one
of the three operations.

Deliberately not acted on. 8.67 ns is meaningless against a 300-second block interval
and 136 µs signature verification, and the alternatives — dropping `optional`, or
introducing an unchecked fast path — trade a correctness property for a performance
number nobody needs. The full diagnosis lives in the header comment of
[bench/util_overflow_bench.cpp](../bench/util_overflow_bench.cpp).

**Reversed if:** profiling in [Phase 12](ROADMAP.md#phase-12--performance-and-decentralisation)
shows this in a real hot path. The constraint that governs that phase is that consensus
and security are never weakened for performance, so the answer would be to fix the
codegen or narrow the call site, not to remove the check.

### 6. `-fwrapv` is on, in Release too

**Settled.**

Signed overflow becomes defined wraparound instead of undefined behaviour. This is not
about making overflow acceptable — every attacker-influenced arithmetic operation is
checked. It is about what happens when a check is *missed*: under undefined behaviour,
the optimiser is entitled to delete the very check that would have caught it, because it
may assume overflow cannot occur. `-fwrapv` removes that entitlement.

The cost is some loss of loop optimisation. For a workload dominated by hashing and
signature verification, that is not measurable.

**Reversed if:** never.

### 7. Hardening stays on in Release

**Settled.**

`-fstack-protector-strong`, `-fstack-clash-protection`, `-fcf-protection=full`,
`-fno-strict-aliasing`, `-fno-delete-null-pointer-checks`, `_FORTIFY_SOURCE=3`,
`_GLIBCXX_ASSERTIONS`, and `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack`. All of it in
the configuration that ships, suppressed only when a sanitizer is active because the two
conflict.

Hardening that is disabled in Release is hardening that protects development and not
users. The flags are in a single interface target every other target links, so nothing
can be built with a weaker configuration by accident, and `amariand --build-info` prints
what was actually used so the claim is checkable rather than asserted.

**Reversed if:** never, and a missing flag that is already visible in `--build-info` is
explicitly [out of scope](../SECURITY.md#deliberately-out-of-scope) as a security report.

### 8. `-Werror`, with the conversion warnings that matter

**Settled.**

`-Wconversion -Wsign-conversion -Wold-style-cast -Wswitch-enum -Wuseless-cast
-Warith-conversion -Wpedantic -Wmissing-declarations -Wredundant-decls`, plus Clang's
`-Wunreachable-code-aggressive -Wshadow-all -Wthread-safety -Wloop-analysis
-Wrange-loop-analysis`.

`-Wconversion` and `-Wsign-conversion` are the load-bearing pair. A silent narrowing in
a fee calculation or an implicit sign change in an amount comparison is exactly the class
of bug that produces a monetary error, and with `-Werror` it becomes a build failure
instead of a code review someone has to get right.

The cost is real and was paid: `-Wunreachable-code-aggressive` correctly rejected a
runtime branch on a compile-time-constant sanitizer string, fixed by using
`if constexpr`. That is the warning doing its job.

**Reversed if:** never. Individual warnings may be suppressed at a specific site with a
comment explaining why; the set is not narrowed.

### 9. No C++20 modules

**Settled**, and it changed the build.

`set(CMAKE_CXX_SCAN_FOR_MODULES OFF)`. Left at its default, CMake adds a dependency-scan
step per source file and compiles everything with GCC's experimental `-fmodules-ts`.
Neither buys anything for a header-based project, and consensus code should not be built
with an experimental front-end mode.

Verified rather than assumed after the change: `-fmodules-ts` appears 0 times in
`compile_commands.json` for both the dev and release presets, and `.ddi` rules appear 0
times in the generated `build.ninja`.

**Reversed if:** the project adopts modules deliberately, which would be a separate
decision with its own migration.

### 10. `.clang-format` applied to the whole tree, and gated

**Settled.**

The config had been written with care — 100-column limit, no argument bin-packing,
`SeparateDefinitionBlocks: Always`, custom include-priority categories placing
`<amarian/...>` first — and had never been run against the code. 20 of 23 C++ files
violated it, 267 lines of diff.

Fixed in one mechanical commit kept separate from any behavioural change, then verified:
all nine presets build clean, 47/47 tests pass on five presets, and both fuzz harnesses
still run — worth checking specifically because the `FUZZ_CHECK` macro was among the
reformatted files. `clang-format-21 --dry-run --Werror $(git ls-files "*.cpp" "*.hpp")`
now exits clean and is usable as a CI gate.

**Reversed if:** never. A checked-in format config that the code does not satisfy is
worse than no config, because it makes every diff ambiguous about intent.

### 11. Fuzzing runs with `-print_funcs=0`

**Settled**, and it is a three-orders-of-magnitude finding.

libFuzzer's default `NEW_FUNC` symbolisation shells out to `llvm-symbolizer` each time
coverage reaches a new function. Measured on `fuzz_hex`:

| Configuration | Executions per second |
|---|---|
| Default, symbolisation on | 222 |
| `-print_funcs=0` | **160 000** |

The process spends the difference *blocked*, not busy — 90 seconds of wall clock against
0.1 seconds of CPU. Without the flag a fuzzing campaign looks like it is running and is
in fact almost entirely waiting on a subprocess.

**Reversed if:** never for campaigns. Symbolisation is useful when triaging a specific
crash, which is a different activity.

### 12. Nine presets across two compilers

**Settled.**

`dev`, `debug`, `clang-dev`, `asan`, `tsan`, `fuzz`, `bench`, `bench-clang`, `release`.
Two compilers is not redundancy: the 21× checked-arithmetic disagreement in decision 5
was only visible because both were measured, and a codegen difference between compilers
in a consensus path would be a chain split. `bench-clang` exists specifically so that
benchmark numbers are never a single compiler's opinion.

Current state, from actual runs: all nine configure and build clean; `dev`, `debug`,
`clang-dev`, `asan` and `tsan` pass 47/47 tests each.

**Reversed if:** never. Presets get added, not removed.

## Cryptography

### 13. OpenSSL's default provider for post-quantum signatures

**Settled.**

OpenSSL 3.5.5 ships ML-DSA-44/65/87 (FIPS 204) and SLH-DSA-SHA2-128s (FIPS 205) in the
**default provider**, verified by running them. So `liboqs` and `oqsprovider` are not
dependencies of this project.

That matters more than convenience. The post-quantum code path goes through the same
library, the same release process and the same audit surface as the rest of the
cryptography, rather than a separately maintained plug-in with its own lifecycle.

**Reversed if:** OpenSSL's implementations prove deficient, or a scheme is needed that
they do not provide. The scheme registry exists so that this is an interface swap.

### 14. Two post-quantum families, not one

**Settled.**

ML-DSA is lattice-based; SLH-DSA is hash-based. If lattice assumptions turn out weaker
than believed, a hash-based scheme is still standing, and vice versa. Holding both is the
entire point of the agility machinery — having the mechanism and only one scheme to put in
it would be theatre.

**Reversed if:** never, though which specific parameter sets are registered will change.

### 15. Post-quantum signatures go in the witness section; SLH-DSA is cold storage only

**Settled**, on measurement.

Both conclusions were produced by the numbers rather than imported. Verification is
**not** the constraint: ML-DSA-44 verifies in 136 µs against Ed25519's 129 µs on the same
machine, so a full block of single-input ML-DSA-44 transactions costs 0.064 s of
verification CPU against a 300-second interval. **Size is** the constraint: authorisation
data goes from 96 bytes to 3 732 per input, **38.9×**, which is a 6.7× reduction in
transactions per block.

Hence the witness placement — the 4:1 weight discount is what keeps a 3.7 KB
authorisation from consuming 15 KB of weight — and hence SLH-DSA's restriction: 424 ms
per signature means a ten-input transaction takes over four seconds and a fifty-input
sweep takes twenty. Fine for a vault, unacceptable for a hot wallet.

Consensus commits to every witness byte in the signature hash regardless; the discount is
block-space accounting, not a gap in what is signed.

**Reversed if:** a scheme appears with materially better size characteristics, which is
what the registry is for.

### 16. Hybrid classical + post-quantum authorisation

**Provisional — [Phase 7](ROADMAP.md#phase-7--hybrid-ownership) decides.**

Requiring both signatures costs little in size — 3 828 bytes per input against 3 732 for
ML-DSA-44 alone — and hedges against either scheme breaking. Against that: two key types
per wallet, two things every backup must preserve, and **two** ways for a spend to become
permanently impossible instead of one. A hybrid scheme is strictly *less available* than
either component, and for long-term self-custody, availability failures lose real coins
today while cryptographic breaks are hypothetical.

Not adopted on the grounds that it sounds stronger. "Deferred, for these reasons" is an
acceptable outcome, and the agility machinery is what makes deferring different from
foreclosing.

### 17. No hand-rolled primitives, ever

**Settled.**

Hashing, classical signatures and post-quantum signatures all come from libsecp256k1 and
OpenSSL. Amarian's job is to call them correctly, which is why the two kinds of security
report — "Amarian misuses a primitive" and "the primitive is broken" — are separated in
[SECURITY.md](../SECURITY.md#cryptography).

The consequence is a load-bearing assumption that cannot be engineered away, only reduced
by keeping the interface narrow enough to swap implementations. That is stated as
assumption 3 in [PQ_CRYPTO.md](PQ_CRYPTO.md#stated-assumptions) rather than hidden.

**Reversed if:** never.

## Monetary policy

### 18. 8 399 999.993 217 AMR, 12.5% per era, no tail emission

**Settled.** Changing any of it changes the monetary system, which is the one thing this
project exists to keep fixed.

The parameters were not picked and then summed; the sum was picked and the parameters
chosen to produce it. A geometric schedule retaining 7/8 per era has the closed form
`initial × era_blocks × 8`, so 10 AMR over 105 000 blocks is exactly **8 400 000 AMR** —
round by construction, with no special cases anywhere in the schedule. Integer truncation
at era boundaries brings the realised cap 0.006 783 AMR below that, and consensus enforces
the realised figure, 83 999 999 932 170 000 facets. Truncation can only reduce the reward,
so the closed form is a strict upper bound no execution can exceed.

12.5% annually rather than 50% every four years, because a halving drops the security
budget by half between two consecutive blocks and a new chain has neither the hashrate nor
the derivatives market to absorb that. Worth noting that it is not a slower schedule
overall: 12.50% annualised against Bitcoin's 15.91%, and Amarian is slightly *less*
front-loaded at every comparable year.

**Reversed if:** nothing short of a decision that the project has a different purpose.
The known risk — that a halving has fifteen years of adversarial history and this
schedule has none — is recorded in [../DEVELOPMENT_STATUS.md](../DEVELOPMENT_STATUS.md).

### 19. Base unit "facet", ten decimal places

**Settled**, and consensus-visible through the amount encoding.

Ten places puts the smallest unit at 10⁻¹⁰ AMR, so a facet stays worth less than a cent
even if one AMR reached a hundred thousand dollars. Bitcoin's eight decimals were
reasonable in 2009 and have been visibly tight at times; two more cost nothing at 57 bits
and close the question permanently.

### 20. Amounts are signed `int64_t`, not unsigned

**Settled**, and deliberately counter-intuitive.

Unsigned makes a negative amount unrepresentable, which sounds safer until a subtraction
underflows and yields an enormous positive number rather than a detectably wrong one.
Signed, plus `-fwrapv`, plus a `[0, MAX_MONEY]` range check applied at deserialisation
rather than at point of use, means a bad amount is representable and therefore
*detectable*, and the check that catches it cannot be optimised away.

The cap occupies 57 bits, leaving 109.8× headroom below `INT64_MAX` — which matters
because block validation sums many outputs and fee-rate arithmetic multiplies an amount
by a size. A cap that fitted only just inside the type would make every intermediate an
overflow risk, and the overflow of a monetary sum is an inflation bug.

### 21. Ticker

**Provisional.** `AMR` is the working label. Checked on 2026-09-04: unclaimed among the
coins CoinGecko indexes, and **in active use on the NYSE by Alpha Metallurgical
Resources.** That is a real conflict and the reason this is not settled.

It is deferrable because the ticker is a display-layer chain parameter — it appears in no
serialised structure, no hash preimage and no network message, so changing it is a
parameter edit and a rebuild, not a fork. An address prefix is the opposite, which is why
[that one is not being deferred](WALLET.md#addresses). A final decision needs a check
against exchange and data-provider registries rather than one aggregator, and it has to
happen before any testnet that people outside the project use.

## Protocol

Most of the following is design intent that Phase 1 implements. Three entries have
now landed in code and say so: the transaction structure of #22 and #23, and the
Merkle construction of #25. Full specification in
[AMARIAN_PROTOCOL.md](AMARIAN_PROTOCOL.md).

### 22. No script virtual machine

**Structure implemented, Phase 1; evaluator not yet.** `SpendCondition` and `Lock` exist
in `primitives/` with their canonical encodings and 36 tests; the code that evaluates a
condition against a witness is part of the consensus work still to land.

A lock commits to `{condition_version, threshold, keys[]}` rather than to a program. A
threshold over a list of scheme-tagged keys covers single-key, `m`-of-`n`, and
classical-plus-post-quantum hybrid authorisation with one evaluator small enough to audit
completely. A script VM covers more, at the cost of an evaluation surface where the
interesting consensus bugs historically live.

**Reversed if:** a requirement genuinely needs more expressiveness — in which case it
arrives as a new `condition_version` with its own review, rather than having been
available and unreviewed from the first block.

### 23. Every public key carries an explicit scheme identifier

**Implemented, Phase 1.** `PublicKey` and `Signature` are both `{scheme: u16, bytes}` on
the wire, and the codec preserves an unknown scheme rather than rejecting it, so a node
relays what it cannot verify. The rule that **scheme 0 is never valid** is a consensus
check rather than a parse check — `primitives` deliberately decides nothing about validity
— and it is enforced in `CheckSpendCondition` and `CheckWitness`, alongside the rule that
a key or signature under a scheme the registry *knows* must be exactly that scheme's
length. A scheme the registry does not know has no length to check and is left alone.

A key is `{scheme: u16, bytes}`, never a bare byte string whose meaning is inferred from
its length. Length inference is how a codebase ends up unable to add a scheme whose key
size collides with an existing one, and how a verifier ends up guessing. Scheme 0 is
reserved and never valid, so an all-zero field is not a valid key.

### 24. A 92-byte header, with height, a 64-bit timestamp and a 64-bit nonce

**Intent, Phase 1.** Three departures from Bitcoin's 80-byte header, each for a reason:

**Height in the header** makes duplicate coinbase transactions impossible by
construction, closing the whole BIP-30/BIP-34 class in the format rather than with a rule
bolted on afterwards. It also lets a node compute a block's scheduled reward from the
header alone. Cost: redundant with chain position, so it must be checked equal to
`prev.height + 1`.

**A 64-bit signed timestamp** because a 32-bit unsigned one overflows in 2106, and
shipping a field that stops working is indefensible for a chain whose issuance runs to
year 178.

**A 64-bit nonce** because a 32-bit nonce is exhausted in milliseconds, forcing miners to
roll the extranonce and rebuild the Merkle root to keep searching. The extranonce remains
for pool work-splitting, not as a necessity.

### 25. Merkle tree: promotion, distinct tags, and a leaf-count commitment

**Implemented and tested, Phase 1.** [src/primitives/merkle.cpp](../src/primitives/merkle.cpp),
covered by `primitives_merkle_test.cpp` at both levels the CVE occurs at: three leaves
against the four-leaf list that repeats the third, and six leaves against the eight-leaf
list whose final pair repeats the preceding one — the second of which passes on an
implementation that promotes leaves but still duplicates inner nodes.

Bitcoin duplicates the last node of an odd level, which is CVE-2012-2459: a block of `2n`
transactions whose second half repeats the first produces the same root as the
`n`-transaction block, letting an attacker make a node mark a valid block permanently
invalid. Three independent fixes are applied — odd nodes promoted unchanged rather than
duplicated, leaves and branches hashed under different tags, and the leaf count committed
in the root. Any one would close it; all three are present because the cost is nil and
the failure mode is remote, permanent invalidation of a valid block.

### 26. SHA-256d for proof of work

**Intent, Phase 1.**

The most-analysed proof-of-work function in existence, and the roadmap's constraint is
that novel cryptography needs a compelling research reason. Grover's algorithm gives a
quadratic speedup on hash inversion, which for mining is a hashrate change the difficulty
adjustment absorbs exactly as it absorbs new silicon — which is why the post-quantum work
in this project is about signatures and not about the proof of work.

### 27. ASERT for difficulty

**Intent, [Phase 3](ROADMAP.md#phase-3--mining-and-difficulty)**, with parameters from
simulation rather than copied.

A window-boundary retarget is what creates the timewarp vector, because the rule only
inspects timestamps at the window edges. ASERT retargets every block against an absolute
schedule, so there is no boundary to exploit and no accumulated drift, and its exponential
response degrades gracefully under the failure mode a young chain actually faces —
hashrate arriving and leaving faster than a linear controller can track.

Difficulty is the most fragile part of a new chain, so this gets simulation evidence in
Phase 3 rather than an appeal to what other chains do.

### 28. Sighash commits to everything, with no flag byte

**Intent, Phase 1.**

Every signature commits to `chain_id`, the input index, the spent amount, the spend
condition, and hashes of all outpoints, outputs and sequences. Per-transaction hashes are
computed once and reused across inputs — not as an optimisation but as the fix for a
specific denial-of-service class: a preimage recomputed per input makes validation cost
grow with the square of transaction size, which Bitcoin shipped and could not remove.

No `SIGHASH` flags in generation 1. They enable real use cases and a long history of
subtle failures; if wanted, they arrive as a `condition_version` with their own analysis.

### 29. Genesis pays zero

**Intent, Phase 1.**

Not a token amount, not a burn address — a reward of zero, enforced as a consensus rule. A
spendable genesis reward is a premine, and the difference between "earned by work under
published rules" and "allocated by a founder" is the entire distributional claim this
project makes. Zero means there is nothing to argue about.

### 30. `MAX_BLOCK_WEIGHT` = 2 000 000

**Provisional**, and flagged as such because it is the one consensus constant that
depends on measurement not yet taken — initial-block-download time, storage growth, and
the bandwidth a node needs are [Phase 12](ROADMAP.md#phase-12--performance-and-decentralisation)
numbers. Adopting it now because 2 000 000 is a familiar figure would be exactly the
reasoning this project is meant to avoid, so it carries the label until it is earned.

### 31. Non-canonical encodings are rejected, not normalised

**Implemented, Phase 1**, in [src/util/serialize.cpp](../src/util/serialize.cpp).

A compact-size integer that could have been written in fewer bytes is a parse failure. The
tempting alternative — read it, normalise it, carry on — is what gives one transaction two
valid encodings and therefore two ids. That is not a theoretical concern: it is transaction
malleability, and the version of it that reaches consensus is a chain split, because two
nodes that disagree about whether `0xfd 0x01 0x00` is a valid encoding of 1 disagree about
whether a block is valid.

The same principle is why `Reader::Finish()` requires that every byte has been consumed.
Trailing bytes after a well-formed structure are not debris to ignore; they are a second
encoding of the same object, so they are an error.

**Reversed if:** never, for consensus-facing data. A non-consensus format that needs
lenient parsing gets its own reader rather than a relaxation of this one.

### 32. Counts are bounded against remaining bytes before anything is allocated

**Implemented, Phase 1**, in [src/util/serialize.cpp](../src/util/serialize.cpp).

`Reader::ReadCompactSize` takes a per-element minimum encoded size and refuses a count that
could not possibly be satisfied by the bytes still in the buffer, before the caller reserves
anything. A count field is attacker-controlled; a 40-byte message claiming four billion
inputs must cost 40 bytes of work, not four billion allocations. Checking after the
allocation, or trusting a `MAX_*` constant alone, both leave a remote memory-exhaustion
vector open — `MAX_*` bounds what is valid, not what an attacker can make a node try.

Two supporting properties are part of the same decision: `Reader` failure is **sticky**, so
a partial parse can never be mistaken for a successful short one, and a failed read writes
nothing to its output parameter, so no caller can act on a half-populated object.

### 33. The txid/wtxid split sits before the witness count

**Implemented, Phase 1**, in [src/primitives/transaction.cpp](../src/primitives/transaction.cpp).

`txid` covers version, inputs, outputs and locktime. `wtxid` covers all of that plus the
witness section, and the boundary is drawn *before* the compact-size count that introduces
the witnesses — not after it.

One field either side and the txid would depend on how many witnesses a transaction
carries, which is precisely the malleability the split exists to remove: a relay node could
alter the count, and the transaction's identity would change while its authorised effect
did not. Putting the count on the witness side of the line makes the txid a function of the
transaction's committed content and nothing else.

**Reversed if:** never without a new transaction version, since the split point is
consensus-visible in every id the chain has ever recorded.

### 34. UBSan integer findings are fatal, and scoped away from the standard library

**Implemented, 2026-09-05**, in [cmake/AmarianSanitizers.cmake](../cmake/AmarianSanitizers.cmake)
and [cmake/sanitizer-ignorelist.txt](../cmake/sanitizer-ignorelist.txt).

The first fuzzing session printed a UBSan report and exited zero. `-fsanitize=integer` is
not a member of the `undefined` group, so `-fno-sanitize-recover=undefined` never covered
it: the process printed its diagnostic and carried on, and the fuzzer reported success. A
check that can fire without failing the run is decorative, and a fuzzing campaign built on
one is worse than none, because it produces evidence of absence that is not evidence of
anything.

The report itself was a `char`→`unsigned char` conversion inside libstdc++'s `<charconv>`,
reached from a correct `std::from_chars` call in `util/args`. That conversion is
well-defined; `implicit-integer-sign-change` flags it as suspicious, which is useful on our
own code and noise on a header we do not control. So the fix has two halves: make the
findings fatal, and scope the `implicit-*` family away from `include/c++/*` only — leaving
every other check live everywhere, and leaving `implicit-*` live on all Amarian code.

Both halves are demonstrated rather than asserted, by
[scripts/ubsan_charconv_probe.sh](../scripts/ubsan_charconv_probe.sh), which reproduces the
finding, shows it is recoverable, sweeps the ignorelist syntaxes, and confirms the check
still fires on one of our own translation units afterwards. That last step matters: the
first ignorelist written for this was a **silent no-op**, because a section header is a glob
over sanitizer names and the comma form `[a,b]` matches none of them. Nothing in the build
output says so.

The flag is Clang-only because GCC 15.2 rejects `-fsanitize-ignorelist` outright, which is
consistent — `-fsanitize=integer` is a Clang check, and both sanitizer presets are Clang.

**Reversed if:** a suppression is ever needed for Amarian's own code. At that point the
suppression is the wrong tool and the code should change.

### 35. Signature verification answers with five values, not a boolean

**Implemented, 2026-09-05**, in [include/amarian/crypto/signature.hpp](../include/amarian/crypto/signature.hpp).

`crypto::Verify` returns `Valid`, `Invalid`, `Malformed`, `UnknownScheme` or `Reserved`. A
boolean would collapse the two answers a validator must treat oppositely: "this signature
is a forgery" is a rejection, and "I do not implement this scheme" is the soft-fork path
where consensus accepts something it has not checked. Fold them into `false` and a node
that lacks a scheme rejects valid spends and forks itself off the chain; fold them into
`true` and a forgery passes. They are not the same fact and the type says so.

`Malformed` is separate from `Invalid` for a narrower reason: a key that is the right length
for its scheme but that the implementation cannot parse is a different event from a
signature that simply did not verify, and the two get different treatment in the threshold
walk — see decision 38.

`Reserved` exists so identifier 0 has a name at every layer. An all-zero field is a
plausible accident and a plausible attack, and it is never a valid scheme.

**Consequence, and the reason for the startup gate in `amariand`:** because
`UnknownScheme` is accepting, a node whose OpenSSL cannot supply a *registered* scheme
would report it as unknown and accept every spend under it while believing it was
verifying signatures. That is the worst available failure mode, so the node refuses to
start rather than warning. The check is `crypto::FirstUnavailableScheme()`, probed against
the real backend at startup, not a compile-time assumption.

**Reversed if:** never for the `Valid`/`Invalid`/`UnknownScheme` distinction, which is
load-bearing for soft-fork safety. `Malformed` could be merged into `Invalid` if decision
38 were reversed.

### 36. Spend authorisation takes the spent outputs as a span, not a UTXO handle

**Implemented, 2026-09-05**, in [src/consensus/validation.cpp](../src/consensus/validation.cpp).
**Amended by decision 40**, which changed the element type from `TxOutput` to `Coin` when
coinbase maturity needed a per-coin creation height. The reasoning below is unchanged and is
why the amendment was a one-line type change rather than a redesign.

`CheckSpendAuthorisation(tx, std::span<const TxOutput> spent_outputs, params)` and
`TransactionFee(tx, spent_outputs)` are handed the outputs being spent. They do not look
them up, and they do not know what a database is.

*Finding* those outputs is a lookup; deciding whether they may be spent is arithmetic and
cryptography. Splitting the two along that line has three consequences worth the slightly
awkward parameter. The expensive half becomes a pure function of values, so it is testable
exhaustively without a database — the 16 tests covering it use real ML-DSA-44 signatures
and no storage at all. It can later run on several threads without a lock, because there
is no shared state to lock. And it let the rules land *before* the UTXO set instead of
after it, which is why signature verification is enforced today rather than blocked behind
a component that had not been written.

`TxOutput` is already `{int64_t amount; Lock lock;}` — literally the output being spent —
so no new type was invented to carry it. The count is checked against the input count
rather than assumed, because a caller that got it wrong would otherwise read past the end
of the span, and a bound that memory safety depends on is not one to take on trust.

**Reversed if:** a rule appears that genuinely needs the whole set rather than the outputs
one transaction names. Coinbase maturity is not such a rule — it needs a per-coin height,
which belongs on the `Coin` the UTXO set stores.

### 37. Thresholds are satisfied by ordered forward match

**Implemented, 2026-09-05**, in `SatisfiesThreshold` in
[src/consensus/validation.cpp](../src/consensus/validation.cpp).

A witness's signatures are matched against its condition's keys with **one key index that
only ever advances**. Signature *i* is tried against the keys left over after signature
*i−1* stopped; a key that answers a signature is consumed, and so is a key whose scheme
does not match. The alternative considered was trying every signature against every key.

Two properties follow, and both are the reason for choosing it.

**Cost.** At most `len(keys)` verifications happen for an input no matter how many
signatures are offered, and `len(keys)` is capped at 16 by the condition the coin's own
commitment named. Try-every-pair is quadratic in a count the *spender* chooses, which is a
denial-of-service vector bought at linear weight — the shape of bug that has cost other
chains emergency releases.

**No malleability.** Exactly one ordering of a given signature set verifies: the ascending
one, matching the condition's own ascending key order. Any permutation of a valid witness
is an invalid witness, so a relay node cannot reorder signatures to produce a second wtxid
for one transaction. Try-every-pair would accept every permutation, and each would be a
distinct encoding of the same authorised effect.

The two combine into the property that actually matters for a threshold: because each key
is consumed at most once, a 2-of-2 cannot be satisfied by offering one key's signature
twice. Without a monotonic index that would be a 1-of-2 wearing a 2-of-2's address.

Bitcoin's `CHECKMULTISIG` uses the same forward walk, for the same reasons, and has held
for fifteen years.

**Reversed if:** a future `condition_version` defines aggregation, where one signature
answers several keys and the walk does not apply. That is a new version with its own
review, not a change to this one.

### 38. An unparseable key is skipped, not fatal

**Implemented, 2026-09-05**, in `SatisfiesThreshold` in
[src/consensus/validation.cpp](../src/consensus/validation.cpp).

A key that is the correct length for its scheme but that the implementation cannot parse —
not a valid curve point, a rejected encoding — is treated exactly like a key whose
signature failed to verify: the walk moves to the next key. It is not a reason to reject
the transaction.

The reasoning is specific to how Amarian locks work. The lock is a *commitment* to a
`SpendCondition`, and the spender reveals it, so the key list was fixed when the coin was
created and cannot be changed by whoever spends it. If one unparseable key were fatal, an
*n*-key condition would be permanently unspendable because of a single key that could
never have verified anything anyway — the owner's other *n*−1 keys, and their coins, gone.
Skipping leaves the coin spendable by the keys that work, which is what its owner asked
for.

There is no denial-of-service cost, because decision 37 already bounds the walk at
`len(keys)` parse attempts. And there is no path by which this weakens authorisation: a
skipped key cannot satisfy anything, so a threshold still needs `threshold` keys that
genuinely verify.

**Consequence:** the `TxPublicKeyMalformed` error was removed from `ValidationError`
rather than left in place. An enumerator naming a rule the code cannot produce is a lie
about what consensus enforces, and the enumeration is meant to be readable as the complete
list of reasons a node rejects something.

Bitcoin behaves the same way for the same reason.

**Reversed if:** a scheme is registered whose parse failures are cheap to distinguish from
verification failures *and* whose keys are validated when the lock is created rather than
when it is spent. Neither is true of any registered scheme, since a commitment hides the
key until it is revealed.

## State

### 39. A coin is a primitive with a canonical encoding, not a UTXO-layer detail

**Implemented, 2026-09-05**, in [include/amarian/primitives/coin.hpp](../include/amarian/primitives/coin.hpp).

`Coin` is `{TxOutput output; uint32_t height; bool is_coinbase;}` and lives in
`amarian::primitives`, one layer *below* the UTXO set that stores it.

The alternative — declaring it inside `amarian::utxo`, where it is used — was rejected
because a coin has an encoding, and an encoding is a consensus artefact. Two things that do
not exist yet will need to agree on it byte for byte: a UTXO set hash, so a node can state
in one 32-byte value which set it holds, and any signed or shared snapshot that lets a new
node start from a committed set rather than from height zero. Both are consensus-visible,
and neither may depend on how a particular node happens to store its database. Putting the
type where the codec lives keeps the byte layout reviewable next to every other encoding
instead of buried in a storage layer.

`height` and `is_coinbase` are on the coin rather than looked up per spend because they are
properties of the coin's *creation*, and after a reorganisation the block that created it
may no longer be reachable. A rule that needed to consult that block would be a rule that
cannot be evaluated at the moment it matters.

**Consequence:** coinbase maturity is decidable from the coin alone, which is what made
decision 40 possible.

**Reversed if:** nothing plausible. The type is 3 fields wide and its encoding is fixed by
the same codec as everything else.

### 40. Spend authorisation takes a span of coins, amending decision 36

**Implemented, 2026-09-05**, in [src/consensus/validation.cpp](../src/consensus/validation.cpp).

`CheckSpendAuthorisation` and `TransactionFee` now take `std::span<const Coin>` where
decision 36 gave them `std::span<const TxOutput>`. The substance of 36 is unchanged: they
are handed values, they perform no lookup, and they still do not know what a database is.

The change was forced by coinbase maturity, which needs each spent coin's creation height —
information a `TxOutput` does not carry and cannot be given without inventing a parallel
array. Building a `std::vector<TxOutput>` per transaction to preserve the old signature was
considered and rejected: it would have copied every output of every input of every
transaction during a sync to avoid touching a parameter type, and a copy that exists only to
keep a signature is the kind of workaround that turns into architecture.

`CheckTransactionInputs(tx, spent, spend_height, params)` is the new entry point and orders
its rules cheapest first: reject a coinbase outright, check the span length against the
input count, check maturity in integers, compute the fee in integers — which is where a
transaction that tries to mint is caught — and verify cryptography last. Ordering is not
cosmetic here. Every rule above the signature check is a rule an attacker cannot make a node
pay for, and signature verification is by far the most expensive thing a node does.

**Consequence:** the layer boundary held. Consensus gained a per-coin height without gaining
a dependency on storage; `amarian::consensus` still links nothing but `primitives`.

**Reversed if:** a rule appears that needs the whole set rather than the coins one
transaction names. Duplicate-outpoint creation is such a rule, and it is deliberately in the
UTXO layer (decision 46) rather than pushed into this signature.

### 41. A cache holds only its change set — no read-through, no unnecessary tombstone

**Implemented, 2026-09-05**, in [src/utxo/coins.cpp](../src/utxo/coins.cpp).

`CoinsCache` stores an entry only for an outpoint whose truth *differs* from its base view.
A read for an untouched outpoint goes to the base every time, and is not remembered.

That costs a lookup a read-through cache would have saved. It buys a property worth more
than the lookup: "what is in the map" and "what must be written" are the same question. No
dirty flags, no `mutable` members, no distinction between a cached clean entry and a pending
change — so a flush writes precisely what changed, and a cache that is abandoned is provably
free of consequences. This is the property a reorganisation has to be able to trust, and the
one Bitcoin Core's equivalent spends real complexity to maintain alongside its caching.

The same reasoning produces the tombstone rule. Spending a coin the base holds *must* leave
a `std::nullopt` entry, or a later read would fall through and find the coin again. Spending
a coin the base never had — one this layer created, or any coin at all when the base is the
empty view — erases the entry outright instead. Without that distinction a root cache would
accumulate one tombstone per coin ever spent, and the node's in-memory set would grow with
the chain's entire history rather than with its unspent output count.

**Consequence:** `ChangeCount()` is a meaningful number, and the tests assert on it. Several
of them would pass under a leaky implementation if they only checked coin presence.

**Reversed if:** profiling on a real chain shows base lookups dominating. The fix would be a
separate read-through layer *below* the change set, not a dirty flag inside it.

### 42. A coins cache cannot be copied, and layering one is a named operation

**Implemented, 2026-09-05**, in [include/amarian/utxo/coins.hpp](../include/amarian/utxo/coins.hpp).

`CoinsCache` has a private constructor, deleted copy and move operations, and one way to
build one: `CoinsCache::Over(base)`. This is recorded because it was not a stylistic
preference — it is a bug that was written, shipped into the first test run, and diagnosed.

The natural spelling was `explicit CoinsCache(const CoinsView& base)`, used as
`CoinsCache batch(coins)`. Because `CoinsCache` *derives* from `CoinsView`, and because a
cache over a cache is the normal case here — a block's batch over the node's set — that
expression does not call the constructor it appears to call. An implicitly-declared
`CoinsCache(const CoinsCache&)` is an exact match; the reference constructor requires a
derived-to-base conversion; overload resolution takes the exact match. The result is a
*duplicate of the parent's change set sharing the parent's base*, not a layer above it.

Both spellings compile, both look right, and the second one silently destroys atomicity. The
duplicate's `MarkSpent` consults the grandparent view, so tombstones the batch needed were
erased instead of created; and flushing it wrote the parent's own entries back over the
parent while losing the batch's. Four tests failed, and the failures were traced by hand
against that prediction before anything was changed — every observed count matched. The same
defect was live in `ConnectBlock` and `DisconnectBlock`, which is to say a rejected block
could have left the node's real UTXO set edited.

Deleting the copy operations makes the mistake unwritable rather than merely documented, and
the named factory makes the intent explicit at every one of the ~25 construction sites.
`Over` returns by value with both copy and move deleted, which C++17's guaranteed copy
elision permits: the prvalue initialises the destination directly.

**Consequence:** the general rule this is a case of — a type that both derives from an
interface and takes that interface as a constructor parameter has an ambiguity a reader
cannot see — applies to any future view layer. Storage-backed views should follow the same
shape.

**Reversed if:** never. There is no case in this system where duplicating a change set apart
from its base is a meaningful operation.

### 43. Connecting and disconnecting a block are atomic, by nesting rather than by rollback

**Implemented, 2026-09-05**, in [src/utxo/connect.cpp](../src/utxo/connect.cpp).

Both operations build their own `CoinsCache` over the caller's set, make every change in it,
and flush only after the last rule has passed. A block rejected half way through therefore
leaves the caller's set exactly as it was — not repaired, but never edited.

The alternative is an undo log applied on failure, which is the same idea with a failure mode:
the rollback path is code that runs only when something has already gone wrong, so it is the
code least likely to be correct and least likely to be exercised. Nesting has no failure path
at all. Not flushing is not an action.

The cost is one hash map per block. Measured against the signature verification the same block
requires — ML-DSA-44 verification per input, thousands of inputs — it is not a number worth
optimising, and it removes the entire class of bug where a rejected block has already changed
the state.

This is also what makes intra-block dependencies work rather than merely tolerable. Each
transaction's outputs enter the batch before the next transaction is examined, so a
transaction may spend an output created earlier in the same block and may not spend one
created later. **Within a block, transactions must be topologically ordered** — and that is
not a separate check anywhere in the code. It is what applying the block in order *means*, and
a block that violates it is rejected with `TxInputMissingOrSpent`, because at the moment the
spend is examined the coin genuinely does not exist. Removing each coin as it is found is also
what makes a double spend *within* a block impossible here as well as in `CheckBlock`: the
second attempt finds nothing.

**Consequence:** `ConnectBlock` deliberately does not re-run `CheckBlock` or the header
checks. They are documented preconditions, because re-running them would re-serialise and
re-hash every transaction of every block during a sync to re-derive an answer the caller
already holds. The cheap bounds that *memory safety* depends on — a non-empty transaction
list, a coinbase in position zero, no second coinbase — are re-checked anyway, which is the
same line decision 36 draws.

**Reversed if:** a block ever needs to be applied to a set too large to layer over. It does
not: the layer holds one block's changes, not the set.

### 44. An undo record exists, and disconnecting compares every coin in full

**Implemented, 2026-09-05**, in [include/amarian/utxo/connect.hpp](../include/amarian/utxo/connect.hpp).

Connecting a block is not invertible from the block alone. The outputs it created are in it,
so removing them again needs nothing else — but the coins it *spent* are not: the block names
their outpoints, not their contents. Restoring them requires the amounts, locks, creation
heights and coinbase flags that were removed. `BlockUndo` records exactly that, written while
the block is connected because that is the only moment the data is in hand.

`BlockUndo` therefore has an encoding, for one reason: a reorganisation may begin after a
restart, so the undo data for every block on the active chain has to survive one. Its decoder
is bounded the same way every other decoder in the project is — the count checked against
`min_element_bytes` of remaining input *and* against the chain's transaction and input limits
before anything is reserved. It holds one entry per **non-coinbase** transaction rather than a
padded entry per transaction, because an always-empty slot is a thing to be checked instead of
a thing that cannot happen.

Disconnecting checks **full equality** on every coin it removes, not merely presence. This is
the decision with real cost — a comparison per output of every block being disconnected — and
it is worth it because "revert restores exactly what was removed" is the property a
reorganisation stakes everything on. An approximate revert is a silent chain split: two nodes
that both believe they are on the same chain, holding different money, with no rule having
rejected anything. The alternative to comparing is trusting a database, and a corrupted
database is precisely the condition this check exists to catch. A test pins it down by
disconnecting a block whose header height was altered: the txid is unchanged so the outpoint
is found, and presence alone would accept it.

Transactions are walked in reverse block order, and within each one the outputs it created are
removed before the inputs it spent are restored. That is not a stylistic mirror of connecting:
a transaction may spend an output created earlier in the same block, so undoing forwards would
try to remove a coin the later transaction had not yet given back.

`DisconnectBlock` takes no `ChainParams`, because undoing a block enforces no rule that could
differ between networks. It restores what was recorded and checks only that what it restores
is what was recorded.

**Consequence:** undo data is per-block state a node must persist alongside blocks. Storage
sizing has to account for it.

**Reversed if:** nothing. The alternative to an undo record is re-deriving spent coins by
replaying the chain, which is the operation an undo record exists to avoid.

### 45. A provably unspendable output is never stored

**Implemented, 2026-09-05**, in `IsStored` in [src/utxo/connect.cpp](../src/utxo/connect.cpp).

An output whose lock is `version == 0` — `LOCK_VERSION_UNSPENDABLE`, the reserved-and-invalid
value the extension-point policy sets aside at every version field — is not added to the UTXO
set. The coins are still destroyed, which is the point: this decides only whether every node
has to remember for ever that they were.

A lock nothing can ever open holds coins that are gone. Keeping the entry would mean carrying
it in every node's set, every snapshot and every set hash in perpetuity, for an outpoint no
transaction can ever name. Bitcoin reached the same conclusion about `OP_RETURN`, and the
reasoning transfers exactly.

The part worth recording is *how* it is implemented. Connect and disconnect must agree on this
predicate exactly — an output one skips and the other does not is either a coin that cannot be
removed or a coin restored from nothing. They agree because it is a single function that both
call, which is the only way to make "the same outputs were skipped" a fact rather than a hope.
Two copies of the same condition, however carefully written, are two things that can drift.

**Consequence:** unknown lock versions are *not* covered by this and are stored normally. Only
the one reserved value is provably dead, and treating an unrecognised version as unspendable
would break the soft-fork extension point it exists to protect.

**Reversed if:** a use appears for retrievable proof that specific coins were destroyed. It
would belong in an index outside the UTXO set, not in the set.

### 46. No duplicate-transaction rule to activate, but the invariant is enforced anyway

**Implemented, 2026-09-05**, in `CoinsCache::AddCoin` and `ConnectBlock`.

Creating an outpoint that is already unspent is rejected with `TxCreatesExistingOutpoint`.
Amarian needs no equivalent of Bitcoin's BIP-30 rule, and no activation height for one, because
the situation is unreachable by construction: a coinbase input's `sequence` must equal the
block height, so no two coinbase transactions can share a txid, and a duplicate non-coinbase
txid would have had to re-spend outpoints the first one already consumed — which the set
refuses independently. Bitcoin's problem was that its coinbases had no such distinguisher and
two pairs of identical ones actually reached the chain.

The check is enforced regardless, and that is the decision. An invariant guaranteed by
construction is guaranteed by an *argument*, and the argument depends on facts that a future
change could quietly alter — a new coinbase format, a transaction version that computes its id
differently. The cost of being wrong is a live coin silently overwritten: money that vanishes
with no rule having rejected anything, and no log line to find it by. The cost of checking is a
hash-map lookup the code performs anyway to add the coin.

**Consequence:** a coin may still be recreated at an outpoint it previously occupied, provided
that outpoint is unspent at the moment of creation. The rule is about live coins, and a test
pins that distinction down.

**Reversed if:** never. This is the check whose absence has cost other chains money.

### 47. A failed disconnect is not a rejected block, and the type system says so

**Implemented, 2026-09-05**, in [include/amarian/utxo/connect.hpp](../include/amarian/utxo/connect.hpp).

`DisconnectBlock` returns `std::expected<void, DisconnectError>` — a separate enumeration from
`consensus::ValidationError`, with four values of its own. Sharing one error type would have
been less code and is the obvious thing to do, since both are "why an operation on a block
failed".

They are not the same kind of failure, and the distinction is the whole point. A block that
fails to *connect* is invalid: it was proposed, it broke a rule, it is rejected, and the node
carries on and is right to. A block that fails to *disconnect* was already validated and
already applied — so a failure means the undo record and the UTXO set no longer describe each
other. That is a corrupted database, or a caller disconnecting a block that is not the tip.
There is no valid continuation from it, and continuing anyway means operating on a set that is
no longer the chain's state.

Sharing one enumeration would invite exactly one wrong reaction: a caller with a generic
`if (!result) { reject_block(); continue; }` treating corrupted state as a peer's bad block,
and carrying on with a set that has quietly diverged. Two types make that unwritable, and the
compiler enforces it at every call site. `-Wswitch-enum` means neither `Describe` can silently
grow a hole when an enumerator is added.

**Consequence:** the node layer above must handle the two paths differently by construction —
a rejected block is a peer's problem, a failed disconnect is the operator's. Both enumerations
have a `Describe` returning a line a node operator can act on.

**Reversed if:** never. Two failure modes demanding opposite reactions is the case type
distinctions exist for.







## Chain

### 48. Accumulated work is a measured quantity with its own type

**Implemented, 2026-09-05**, in [include/amarian/consensus/work.hpp](../include/amarian/consensus/work.hpp).

`Work` is a 256-bit amount supporting exactly three operations: construction from a target,
addition, and comparison. It cannot be multiplied, cannot be converted to a floating-point
number, and has no conversion to or from `Hash256` or `Target` even though all three are
32-byte values. A `uint64_t` of "difficulty", or a shared 256-bit integer type, would have been
less code.

Three different 256-bit quantities appear within a few lines of each other in any
proof-of-work implementation: the digest a miner found, the threshold it had to fall under, and
the cost of finding it. They are not interchangeable — the first two are compared, the third is
summed, and the second and third are inverses of one another. Confusing them is the classic way
an implementation ends up disagreeing with every other node while still appearing to work,
because a chain will still be selected; it will just be the wrong one. Separate types with no
conversions between them make each of those confusions a compile error rather than a consensus
split.

Excluding multiplication and floating point is the same argument applied to the operations. A
difficulty expressed as a `double` is the standard way to display work to a human, and it is
also a value two nodes can compute differently. Nothing in consensus may see one, so the type
cannot produce one.

**Consequence:** displaying difficulty as a human-readable number is a job for the RPC layer,
which may convert `ToHex` output however it likes. Consensus never does.

**Reversed if:** never. This is a type distinction that costs nothing and forecloses a class of
bug that is nearly invisible in testing.

### 49. Work is stored most significant byte first, so comparison is correct by construction

**Implemented, 2026-09-05**, in `Work`'s defaulted `==` and `<=>`.

The 32 bytes are held big-endian, index 0 most significant, and both comparison operators are
`= default`. Four little-endian `uint64_t` limbs would be the faster representation and the more
usual one for arithmetic.

Lexicographic order over big-endian bytes *is* numeric order, so a defaulted `<=>` over the
array is the correct numeric comparison with no hand-written code to get wrong. Over
least-significant-first limbs the same defaulted operator compares the *low* limb first and is
silently, catastrophically wrong: it would order chains by the bottom 64 bits of their work.
That comparison is the single most consequential operation in the node — it decides which chain
is real — and this representation makes writing it impossible to get wrong rather than merely
easy to get right. `Hash256` uses the identical technique for the identical reason.

Arithmetic pays for it: addition and division work a byte at a time instead of a limb at a
time. Both are performed once per header, against a `SHA-256` that costs far more.

**Consequence:** `Work`'s byte order is the opposite of `Hash256`'s internal order, which is one
more reason no conversion between them exists.

**Reversed if:** profiling ever shows the byte-at-a-time division mattering. The fix would be
limbs plus a hand-written comparison and a test that a chain differing only in its high limb
compares correctly — not a defaulted operator.

### 50. Work addition saturates rather than wrapping

**Implemented, 2026-09-05**, in `Work::operator+=`.

Overflow fills all 32 bytes rather than wrapping to zero. Neither behaviour is reachable: the
total work of every chain that will ever exist is bounded by the number of hash attempts the
physical universe permits, which is not close to 2^256.

The choice is made on the asymmetry of the two failure modes if the unreachable is somehow
reached. A wrap makes an enormous chain compare as a tiny one, so a node would abandon the real
chain for a trivial one — the worst chain-selection bug there is. Saturation can at worst make
two physically impossible chains compare equal, which the first-seen tie-break then resolves
into a deterministic answer. Given a choice between an unreachable path that is catastrophic and
an unreachable path that is merely arbitrary, the code takes the arbitrary one.

**Consequence:** `Work` has no overflow signal. A caller cannot distinguish saturation from a
genuine maximum, and does not need to.

**Reversed if:** never.

### 51. The work of a target is computed without a 512-bit intermediate

**Implemented, 2026-09-05**, in `Work::OfTarget`.

Work is `floor(2^256 / (t + 1))`, and 2^256 does not fit in 256 bits. Rather than widen every
intermediate to 512 bits, the implementation uses

    floor(2^256 / d) == floor((2^256 - 1 - t) / d) + 1     where d = t + 1

The numerator on the right is the bitwise complement of `t`, which fits exactly. The identity
holds because `(2^256 - 1 - t) + d == 2^256` exactly, so the true quotient is one more than the
truncated one, and the `+ 1` cannot itself carry out of the top: `t >= 1` implies `d >= 2`, so
the quotient is at most 2^255 - 1.

The division is restoring binary long division with the divisor aligned under the dividend's
highest set bit. That algorithm was chosen over quotient estimation for a property that can be
stated in one sentence and checked by eye: every shifted divisor has at most as many bits as the
dividend, so no intermediate in the loop can exceed 256 bits, so no intermediate can overflow.
Estimation is faster and its overflow argument is a paragraph.

**Consequence:** a zero target returns zero work rather than dividing by one. No digest is at
most zero, so a zero target describes an impossible block, not an infinitely valuable one, and
crediting a chain for it would be the wrong answer in the one direction that matters.
`CompactToTarget` rejects the encoding long before this is reached; this is the second answer to
the same question, and the two agree.

**Reversed if:** a wider integer type enters the codebase for another reason and the identity
stops paying for itself.

### 52. Equal work goes to the header seen first, not to the lower hash

**Implemented, 2026-09-05**, in `IsBetterTip` and `BlockIndexEntry::sequence`.

When two branches have identical accumulated work, the tip stays with whichever header this node
learned of first, recorded as a monotonic sequence number assigned in `AddHeader`. A
deterministic tie-break — lowest hash, say — was considered and rejected.

The deterministic rule looks strictly better: every node would agree on the winner instantly,
instead of disagreeing until the next block resolves it. It is worse, because it makes block
withholding free. A miner who finds a block whose hash happens to be low can sit on it, watch a
rival win the height, and publish later to displace them at no cost — the withheld block wins the
tie-break whenever it arrives. That turns a one-confirmation payment into something an attacker
can reverse for free, which is a far worse property than a few seconds of honest disagreement.
First-seen makes withholding lose: publish late and you lose the tie you would have won. The
disagreement it permits is resolved by the next block either way.

Recording it as a sequence number rather than a wall-clock time is what makes it testable. The
index becomes a pure function of the order headers were offered to it, with no clock in the
selection path, so a reorganisation test is reproducible and a node's tip does not depend on how
its clock is set.

**Consequence:** two honest nodes can briefly disagree about the tip at equal work. This is
correct and is what Bitcoin does. Sequence numbers are node-local and never leave the node; they
are not consensus data.

**Reversed if:** never. This is the tie-break that makes withholding a losing strategy.

### 53. A header's rejection carries either a rule verdict or an index reason, never a flat code

**Implemented, 2026-09-05**, in `HeaderError`.

`AddHeader` fails with `std::variant<consensus::ValidationError, IndexError>` rather than one
enumeration covering both. `IndexError` has two values: the predecessor is not known, and the
predecessor was rejected.

The two halves demand opposite reactions from the caller. A `ValidationError` means the header
can never be valid on this network, so whoever sent it is broken or hostile and should be scored
accordingly. An `IndexError::UnknownPredecessor` means this node is not yet in a position to
judge — headers arrive out of order constantly, and the sender did nothing wrong. Flattened into
one enumeration, the natural implementation of peer scoring is a threshold on "how many headers
did this peer send that I refused", and that implementation bans peers for being early. Keeping
them in separate types means the wrong version does not compile.

**Consequence:** a caller must destructure the variant. `Describe` is overloaded on both halves
and on the variant itself, so logging needs no `std::visit` at the call site.

**Reversed if:** never.

### 54. A rejected header is not remembered

**Implemented, 2026-09-05**, in `AddHeader`.

A header that fails any check is not stored, so offering it again re-runs every check. Caching
the rejection would be cheap and would stop a peer from making the node re-validate the same
garbage repeatedly.

It would also be a consensus bug, not merely an optimisation with a downside.
`HeaderTimestampTooFarAhead` is a verdict that expires: a header more than
`MAX_FUTURE_BLOCK_SECONDS` ahead of this node's clock is invalid now and valid in an hour. A node
that recorded that rejection would permanently refuse a block the rest of the network accepted,
and would then refuse every descendant of it — a permanent fork caused by nothing but its own
cache. Since one rule's verdict expires, the safe design is to remember no verdict at all.

**Consequence:** the cheapest check runs first. `CheckBlockHeader` — one hash and one comparison,
no lookup, nothing allocated — gates everything else, so a flood of fabricated headers costs a
hash each and never reaches the index.

**Reversed if:** a rejection cache is genuinely needed for denial-of-service resistance. It would
have to be keyed on verdicts that provably cannot expire, and the timestamp rules would have to
be excluded by construction rather than by a comment.

### 55. How far a block was validated and whether it is ruled out are two separate fields

**Implemented, 2026-09-05**, in `BlockValidity` and `BlockFailure`.

`BlockValidity` is a three-rung ladder — `Header`, `Body`, `Full` — declared in ascending order so
that "never lower a recorded level" is a single `>` comparison. `BlockFailure` is a separate
field with `None`, `Itself` and `Ancestor`. A single enumeration with a `Failed` value would have
been smaller.

Failure is not a rung on the validity ladder; it is the absence of the whole ladder, and it
behaves differently. Validity is a fact about one block and is not inherited — a valid parent says
nothing about its child. Failure *is* inherited, downwards and permanently: no descendant of a
rejected block can ever be connected, however sound its own header. One field holding both would
have to answer two questions with one value, and the natural mistakes follow immediately: a
`Failed` enumerator that a monotonic "never go backwards" rule would refuse to record, or a
recorded failure erased by a later validity report.

`BlockFailure::Ancestor` is worth its own value rather than reusing `Itself`. A header that failed
nothing itself is not evidence against whoever sent it, and a node that scored peers on inherited
failures would punish them for relaying a chain that was fine when they saw it.

**Consequence:** genesis is entered as `BlockValidity::Full` rather than validated up the ladder.
It is a chain parameter checked against its recorded hash by `consensus::CheckGenesis` before the
node starts, and its single output is unspendable and therefore never stored — so applying it to
an unspent output set is a no-op, and there is no weaker state for it to occupy.

**Reversed if:** never.

### 56. Failure is propagated to descendants when it is recorded, not when it is read

**Implemented, 2026-09-05**, in `BlockIndex::RecordFailure`.

Marking a block failed walks its whole subtree immediately, setting `BlockFailure::Ancestor` on
every descendant. The alternative — deciding eligibility on demand by walking up the parent
chain — needs no walk at rejection time.

It moves the cost to the wrong place. `IsBetterTip` runs on every candidate tip, and if
eligibility required walking an ancestry then the single most frequent comparison in the node
would cost the depth of the chain instead of reading one field. Rejections are rare; tip
comparisons are not. Marking eagerly also makes the invariant checkable by inspection: no
eligible entry has an ineligible ancestor, as a property of stored state rather than of a
traversal nobody can see.

`best_header_` follows the same shape. Adding an entry can only raise the best tip, never lower
it, so an add is one comparison; only a recorded failure can remove the incumbent, and only then
is a rescan needed.

**Consequence:** `RecordFailure` allocates — it holds a frontier vector — so it is not `noexcept`,
and returns how many entries it newly marked so a caller can log the size of what it just ruled
out.

**Reversed if:** subtrees of rejected blocks grow large enough that the eager walk is itself a
denial-of-service vector. Since a header must carry valid proof of work to be indexed at all,
building a large subtree costs real hashing.

### 57. Index entries are allocated individually and never move

**Implemented, 2026-09-05**, in `BlockIndex`'s `unordered_map<Hash256, unique_ptr<BlockIndexEntry>>`.

Entries are held behind `unique_ptr` rather than by value in the map, and every accessor returns
`const`. Holding them by value would work today: `std::unordered_map` is node-based, so element
addresses are already stable across rehashing.

They would be stable by accident. The whole structure rests on `parent` pointers taken once and
dereferenced for the lifetime of the index — by `Ancestor`, by `MedianTimePastAt`, by
`PlanChainSwitch` — and a `parent` that could dangle is a bug in the code that decides which
chain is real. Making stability a property of the owning pointer rather than of the container's
implementation means a future change of container cannot silently break it. The indirection also
survives moving the `BlockIndex` itself, which by-value storage would not.

The `const` boundary is the other half. Nothing outside the class can obtain a mutable entry:
`RecordValidity` and `RecordFailure` are members that look the entry up again by hash and mutate
the stored copy. So the invariants in the class comment — every non-genesis entry has an indexed
parent, `height == parent->height + 1`, `total_work == parent->total_work + work(bits)`, unique
increasing `sequence`, no eligible entry with an ineligible ancestor — hold by construction rather
than by convention.

**Consequence:** one allocation per header. At 300-second blocks that is roughly 105,000 entries
per era, each around 150 bytes; the index of a decade-old chain is a few megabytes.

**Reversed if:** header count ever makes per-entry allocation the bottleneck. The fix is an arena
that hands out stable addresses, not by-value storage.

### 58. Difficulty is constant today, and that is a complete rule rather than a stub

**Implemented, 2026-09-05**, in `NextTargetBits`.

A child inherits its predecessor's `target_bits`, clamped so it is never easier than the
network's floor. Amarian has no retargeting algorithm yet — choosing one is Phase 3, and the
project's own rules forbid picking a consensus constant without the analysis to justify it.

What matters is that this is a *rule* and not a placeholder. The expected target is computed by
the node from the chain and the header's claim is checked against it by
`ContextualCheckBlockHeader`, which means a miner does not get to choose the difficulty they mined
at — the property that makes proof of work mean anything. A stub would have accepted whatever the
header claimed and left that property absent until Phase 3. Constant difficulty is also exactly
what regtest will always do, since its blocks cost a couple of hash attempts by design, so this
is the final answer for one network already. Replacing the body of the function later changes no
caller.

The clamp compares `Work::OfCompactTarget` values rather than the compact encodings themselves.
"Less work than the floor demands" and "easier than the floor" are the same statement, and
comparing work avoids having to reason about the ordering of a mantissa-and-exponent
representation. It is unreachable through the index, since `CheckBlockHeader` rejects such a
header before it is stored — but this same function is what a miner will be handed, and a miner
asking for a target must never receive an invalid one.

**Consequence:** every block on a chain has the target its genesis block had, until Phase 3.
Mainnet and testnet are not launchable before then, which is already the plan.

**Reversed if:** Phase 3, by design. The shape of the boundary is what this decision fixes, not
the algorithm.



### 59. Activation refuses to shrink the chain, and the refusal is a proof

**Implemented, 2026-09-05**, in `ChainState::ActivateBestChain`.

Selection ranks branches by the work their *headers* claim. A header is 92 bytes; a body is up
to a megabyte. So the naive activation — plan a switch to the best header, revert to the fork
point, apply whatever bodies have arrived — hands any peer a way to shorten this node's chain
for the price of an announcement it never follows up on. The node would abandon a valid tip,
connect nothing, and sit at the fork point.

The fix is not a timeout or a heuristic. Before a switch starts, the target is truncated to the
highest block on the new branch whose *whole path* from the fork point is present in the store,
and the switch is abandoned unless that truncated target still wins under `IsBetterTip`. Because
every block contributes at least one unit of work, an ancestor's total work is strictly less than
its descendant's, so a truncated target that is an ancestor of the current tip can never beat it.
The tip therefore cannot regress — not "usually", not "unless an attacker is patient".

Considered and rejected: reverting optimistically and re-applying the old branch if the new one
stalled. It is more code, it touches the unspent output set twice for no gain, and it leaves a
window in which the node's tip is a chain nobody chose.

**Consequence:** during a sync the tip lags the best known header, and `reached_best_header` in
the activation summary is false. That is the honest report of a node that knows about a chain it
has not received.

**Reversed if:** never on these grounds. The bound is arithmetic, not empirical.

### 60. Atomicity is per block, not per reorganisation

**Implemented, 2026-09-05**, in `ChainState`, resting on `ConnectBlock` and `DisconnectBlock`.

Each block is applied or reversed in one commit, so every state the node passes through during a
reorganisation is the unspent output set for *some* valid chain. An interruption leaves a shorter
chain, never an inconsistent one, and the next activation continues from wherever it stopped.

The alternative — one staging layer spanning the whole switch — buys atomicity over a span where
a partial result is already correct, and pays for it with memory proportional to the number of
blocks in the switch. An initial sync is a switch of hundreds of thousands of blocks; it would
hold the entire change set before committing anything.

**Consequence:** a crash mid-reorganisation can leave the node on a *worse* chain than the one it
started from, which the next activation repairs by walking back up. This is preferable to a set
that describes no chain at all, which no amount of later work can repair.

**Reversed if:** a measured need for the whole switch to be observable atomically appears — an
RPC that must never see an intermediate tip, say. That is an interface concern and would be
answered with a lock rather than a staging buffer.

### 61. Genesis is seeded onto the active chain, never connected

**Implemented, 2026-09-05**, in `ChainState`'s constructor.

Genesis's single output carries `LOCK_VERSION_UNSPENDABLE`, and `ConnectBlock` stores no
unspendable output — the `IsStored` predicate it shares with `DisconnectBlock` drops them. So the
unspent output set before genesis and the set after it are byte-for-byte the same set. "Genesis is
applied" and "genesis is not applied" describe identical state, which means there is nothing for a
first activation to get right or wrong about the difference.

Seeding it rather than connecting it also removes a requirement that would otherwise be real: the
node would need genesis's *body* in its block store before it could make any progress at all.
Genesis is a chain parameter, reconstructible from `BuildGenesisBlock`, not a block that arrived
from anywhere, and making progress conditional on having stored it would invent a startup failure
mode for no gain.

**Consequence:** `ActiveChain` is never empty and `ChainState::Tip()` never returns null, which
`DisconnectTip` relies on — a plan naming genesis for reversal is reported as
`ChainOutOfStep` rather than attempted.

**Reversed if:** genesis ever pays a spendable output. It will not; an unspendable genesis
coinbase is itself a deliberate decision recorded earlier.

### 62. Block and undo storage is an interface, and the memory implementation is not a stub

**Implemented, 2026-09-05**, in `chain::BlockStore` and `chain::MemoryBlockStore`.

The code that decides which chain is real must not link a database — the same layering rule that
keeps consensus free of storage, enforced the same way, by target dependencies. So bodies and undo
records live behind five virtual functions, and RocksDB will be another implementation of them
rather than a change to anything above.

`HaveBlock` is separate from `GetBlock` for the reason `CoinsView::HaveCoin` is separate from
`GetCoin`: deciding how far up a branch this node could get asks only about presence, and the
availability walk in decision 59 runs on every activation. A backend answers presence without
deserialising a megabyte.

`GetBlock` returns by value. For any backend that is not a memory map the read *is* a
deserialisation into a fresh object, and returning a pointer would put a lifetime question into the
interface that only one implementation could answer.

The in-memory implementation is complete, not provisional: it implements every function with the
real semantics and is what regtest and the unit tests run against. The one thing it does not do is
survive a process exit, which is exactly and only what the RocksDB implementation adds.

**Consequence:** until persistence lands, a node loses its chain on restart. That is a missing
component, not a defect in this one.

**Reversed if:** never — but the interface will gain a batch or a transaction handle when RocksDB
arrives, if writing a body and its undo record separately turns out to be a durability gap.

### 63. Accepting a block and activating a chain are separate operations

**Implemented, 2026-09-05**, as `ChainState::AcceptBlock` and `ChainState::ActivateBestChain`.

Their failures are different kinds of fact, and merging them would force a caller to disentangle
them. A block can be entirely valid and still not worth switching to. A switch can fail for
reasons that say nothing about the block that triggered it — a lost undo record, a set that
disagrees with its own history. One function returning one error type would have to flatten both
into a vocabulary where a peer's misbehaviour and this machine's disk trouble look alike, and peer
scoring in Phase 4 depends on telling them apart.

The split is also what lets a synchronising node accept a run of blocks and activate once, instead
of reorganising on every message.

`AcceptBlock` runs `CheckBlock` before storing, so nothing structurally invalid is ever written,
and records the result on the entry's validity ladder so activation does not repeat it. It is
idempotent: a block arriving from two peers at once is checked once, because the hash names the
bytes and a body already stored under that hash is that body.

**Consequence:** a caller that accepts without activating has a node whose tip is behind what it
knows. That is a legitimate state and the activation summary reports it.

**Reversed if:** never on these grounds.

### 64. Block assembly is a separate library from consensus

**Implemented, 2026-09-05**, as `amarian::mining` in `src/mining/block_assembler.cpp`, linked by
nothing except `amariand`.

Every layer below it answers *is this block allowed*. This is the only one that answers *what
block should exist next*, and the two questions must be answered by different code. An assembler
sharing a translation unit with the validator would let a mistake become invisible: the block is
built on an assumption, the assumption is then "checked" by the same expression that produced it,
and it passes. Keeping them apart is what makes the assembler's output something the rules judge
rather than something they assume.

The dependency runs one way only. Mining links `chain`, because the height, the previous hash and
the target all come from the tip. Nothing links mining in return, so no rule can consult a
builder.

**Consequence:** two computations of the next target exist in the process — the assembler's and
`ContextualCheckBlockHeader`'s — and they must agree. That is the point, not a cost: they agree
because both call `chain::NextTargetBits`, and if that ever stops being true the assembler finds
out at build time instead of the network finding out later.

**Reversed if:** never on these grounds. The separation is cheaper the larger the assembler gets.

### 65. The assembler self-checks with exactly the proof-of-work-independent half of validation

**Implemented, 2026-09-05**, as the `ContextualCheckBlockHeader` and `CheckCoinbaseAmount` calls
at the end of `BuildBlockTemplate`.

A template is judged before a single hash is spent on it, because a block this node's own rules
refuse is a bug to report rather than work to pay for. But it cannot be judged by `CheckBlock`:
that includes `HeaderInsufficientWork`, which an unsolved template fails by definition. Checking
the wrong thing there would have meant either disabling the check or accepting a failure that
means nothing, and both teach a reader to ignore it.

So the self-check is the two calls that are proof-of-work-independent by construction — the
height, the linkage, the expected target, both timestamp bounds, and the supply. Every field they
judge was computed a few lines above from the same functions they use, which makes them
tautologies for an honest miner and refusals for a dishonest one. A disagreement is always a bug
in the assembler, and `TemplateError::HeaderRefused` and `RewardRefused` say so in as many words.

**Consequence:** the solved block is still checked in full. It goes through `AcceptBlock`, which
runs `CheckBlock` including the work check, exactly as a block from a stranger does.

**Reversed if:** never. A cheaper self-check would be no self-check.

### 66. A miner chooses the timestamp, the payout and the extranonce, and nothing else

**Implemented, 2026-09-05**, as `BuildBlockTemplate`'s parameter list.

The height, the previous hash, the target and the reward are computed from the tip. They are not
parameters, and a caller has no way to express a preference about them. That is what makes the
rules that check them tautologies rather than tests: `ContextualCheckBlockHeader` recomputes the
target from the same `chain::NextTargetBits` the assembler called, so the only compact target that
can appear in a block this node accepts is the one the assembler produces.

The three that are chosen are chosen because consensus genuinely permits a range. The timestamp is
`max(now, median-time-past + 1)`, saturating, and never lowered: a tip whose timestamps run ahead
of this machine is a chain this node has already accepted, so its next block must still be
mineable, while a clock far *ahead* of the network is a fault to fix on this machine rather than
one to paper over in the assembler. The payout is free because who receives the reward is not a
consensus question. The extranonce — `coinbase_data` — is free because it is arbitrary bytes by
definition.

**Consequence:** there is no `getblocktemplate`-shaped interface where a caller supplies fields. If
Phase 3's mining RPC needs one, the fields a caller may set are exactly these three, and the RPC
will have to say no to the rest.

**Reversed if:** never for the four computed fields. That would be reintroducing the class of bug
the split in decision 64 exists to prevent.

### 67. The nonce search is a budget the caller controls, not a loop that runs to completion

**Implemented, 2026-09-05**, as `SolveHeader(header, target, attempts)`.

An unbounded search cannot be interrupted, and a node has things it must be able to do while
mining: answer a shutdown, notice that a peer already published this height, roll the extranonce.
None of them is reachable from inside a loop that does not return.

`attempts` is a budget rather than a range because of what is left behind on failure: the *first
untried* nonce, not the last tried one. n calls of one attempt therefore search exactly what one
call of n attempts searches, so a caller can resume a search across as many calls as it likes
without re-hashing anything it has already rejected, and without tracking where it was.

Exhausting the 64-bit range returns false rather than wrapping. Wrapping would re-search nonces
already known to fail, forever, and a caller could not tell that from a search still making
progress.

**Consequence:** `--generate` needs a default budget, and picking one is a judgement rather than a
rule. `DEFAULT_GENERATE_ATTEMPTS` is 2^26: instant on regtest, and a difficulty a CPU cannot meet
is reported in under a minute instead of spun on forever.

**Reversed if:** a mining RPC needs a long-poll, which is a different shape again — but it needs
this one underneath it.

### 68. An unspendable payout lock is refused rather than mined to

**Implemented, 2026-09-05**, as the `IsUnspendable()` check in `amariand`'s `ParsePayout`.

Lock version 0 is a valid output and a legitimate thing to build — genesis pays to one, and the
provable burn it gives is why the version exists (decision 11). But a *reward* sent there can never
be moved by anyone, and the way an operator arrives at one is almost always a flag copied wrong or
a default left in place, not an intention to destroy issuance.

This is a node refusing to help make an irreversible mistake, not a consensus rule. Nothing about
the block would be invalid; consensus has no opinion on who is paid. The refusal lives in the
daemon's option parsing, which is the layer that can still ask the question.

**Consequence:** burning a reward on purpose requires a lock version other than 0 whose program
cannot be satisfied, which is improbability rather than proof. That asymmetry is deliberate: the
easy path is the recoverable one.

**Reversed if:** never. The cost of the check is one branch at startup.

### 69. Blocks move between nodes as a framed flat file until there is a network

**Implemented, 2026-09-05**, as `amariand --export-blocks` and `--import-blocks`.

Phase 1's acceptance criterion is that two nodes independently validate the same chain, and Phase 4
is where a node learns to talk to a peer. Something has to carry blocks between them in the
meantime, and a file is the honest choice precisely because it is the *weaker* transport: it says
nothing about who produced it, carries no work of its own, and offers no authority whatsoever. An
importer that reaches the exporter's tip does so only because its own consensus code agrees with
every block in the file. A network that authenticated its peers would prove less.

The framing is `magic ‖ uint32 length ‖ block`, which is the shape Bitcoin's bootstrap files use,
for its two reasons: the magic makes a file from the wrong network fail on its first record instead
of deep inside a block, and the length lets a reader skip a record without parsing it.

Genesis is not exported. It is a chain parameter every node rebuilds from `BuildGenesisBlock`, so a
file has nothing to teach an importer about it, and shipping it would invite the idea that block 0
is something a peer supplies.

A wrong-network file stops the import at the first record rather than being attempted. The
encodings are identical across networks, so a foreign file decodes as perfectly well-formed blocks
and every one of them would be refused for a reason — an unknown predecessor — that says nothing
about what actually went wrong.

**Consequence:** a rejected block does not stop an import, so the operator is shown every problem
in a file rather than the first, but it does make the run exit non-zero. A file this node partly
refuses is not a file it agrees with.

**Reversed if:** never removed — bootstrap files remain useful after P2P exists — but it stops
being the *only* transport in Phase 4.

### 70. The daemon does the work it was asked for and exits, rather than sitting in an event loop

**Implemented, 2026-09-05**, as `Run` in `src/node/amariand.cpp`, which has no loop in it.

There is nothing yet to service. The network layer is Phase 4; there are no peers to answer, no
mempool to accept into, no RPC socket to read. A process that sat in a loop anyway would be idling
while claiming to be a node, and the claim is the problem — it would make "the node is running"
mean nothing, and it would be the sort of scaffolding that survives long past the point where it
should have been replaced by the real thing.

So a run opens the chainstate, brings the active chain up to the best block it holds, does whatever
was asked — import, mine, export — makes the database durable, and stops. That is a complete thing
to be, and it is precisely what makes the two-node acceptance test possible today: one process
mines and exports, another imports and judges, and the two tips are compared after both have
exited.

**Consequence:** mining is `--generate n` and not a background thread. A miner that wants to keep
going runs the command again, which is fine at regtest difficulty and is not a mining strategy for
anything else. Phase 3's mining RPC is where continuous mining belongs, because it needs a
long-poll and an extranonce protocol rather than a function call.

**Reversed if:** Phase 4, when there is a socket to select on.

### 71. Every clock read in the node happens in one file

**Implemented, 2026-09-05**, as `UnixSeconds()` in `src/node/amariand.cpp`, the only clock call
outside a test in the project.

Consensus takes `now` as a parameter. So does `chain::HeaderContextFor`, so does
`storage::LoadChain`, so does `BuildBlockTemplate`, so does `ChainState::AcceptBlock`. None of them
can read a clock, which means a node's verdict on a block is reproducible from a transcript: given
the same bytes and the same `now`, it decides the same thing, in a test, on another machine, a year
later.

The alternative — a rule that consults the system clock where it needs it — makes two nodes'
disagreement about a block untraceable, because the input that caused it was never written down.

**Consequence:** `now` is threaded through several signatures that would otherwise not need a
parameter, and `LoadChain` has to be told a clock in order to replay headers it has already
accepted (decision 55). That is the price and it is worth paying.

**Reversed if:** never.

### 72. One database with five column families, and one write batch per block

**Implemented, 2026-09-05**, as `storage::ChainDb` in
[src/storage/chain_db.cpp](../src/storage/chain_db.cpp): one RocksDB instance, with the column
families `coins`, `blocks`, `undo`, `index` and `meta`. RocksDB's mandatory default family is
opened because it must be and is left empty, so no record's home is implicit.

The alternative was one database per kind of record, which is tidier to describe and unusable
for the thing that actually matters. Applying a block changes five things at once: the coins it
spends and creates, its body, its undo record, its index entry, and the tip. Those five are one
fact. Split across five databases there is no way to make them one write, so a crash between the
second and the third leaves a node whose tip names a block whose coins were never applied — and
that state is not merely wrong, it is undetectable from inside, because every individual record
is well-formed.

One instance makes the batch possible, and the batch is written per block rather than per
reorganisation for the reason in decision 60: every intermediate state is then the state of some
valid chain, so an interrupted node is behind rather than corrupt.

**Consequence:** the five families share a WAL and a flush schedule, so a large reorganisation is
a sequence of batches rather than one, and the cost of the durability guarantee is paid per block.
Measured cost is not yet interesting at regtest volumes and is a Phase 12 question.

**Reversed if:** measurement shows the per-block batch dominating block application at real
volumes. The fix would be batching a whole activation with an intermediate marker record, not
splitting the database.

### 73. The data directory carries its network's identity, and a mismatch is refused

**Implemented, 2026-09-05**, as `CheckNetwork` in
[src/storage/chain_db.cpp](../src/storage/chain_db.cpp): the `chain_id` stamp in the `meta`
family, written on a database that has none, compared on one that has, and reported as
`DbError::WrongNetwork` when it disagrees.

Two networks' chains are both valid and completely unrelated. Opening a mainnet directory as
regtest would offer real coins a chain whose blocks cost nothing to produce; opening a regtest
directory as mainnet would announce a tip nobody else can reach. Neither is recoverable by
reconciliation, because the two histories share no ancestor — so the only correct response is to
refuse, and to refuse before anything is written.

This is the second of two independent guards. The first is that `--datadir` defaults to
`$HOME/.amarian/<network>`, one directory per network, so the mistake is hard to make by
accident. The stamp is what catches it when the path was given explicitly, which is the case
where the operator was most confident and most likely to be wrong.

`CheckSchema` is the same mechanism for a different question — the layout version of the stored
encodings, refused rather than upgraded in place, because reading one layout's bytes under
another layout's rules is how a node quietly disagrees with itself about its own history. It
reports `CorruptRecord` rather than a distinct error, which is honest for now: there is exactly
one layout, so an unrecognised stamp is not a version this build could migrate from, it is a
database this build cannot read.

**Consequence:** a directory cannot be repurposed between networks by deleting a file. That is
the intended cost.

**Reversed if:** never for the network stamp. The schema stamp gains a real migration path, and
an error to go with it, the first time a stored encoding changes.

### 74. A storage fault is latched, and a run that latched one fails

**Implemented, 2026-09-05**, as `ChainDb::HasFault()`, surfaced to the layer below as
`ChainSink::HasFault` and read there by `ChainState::StorageFaulted()`, and checked at the end of
`Run` in [src/node/amariand.cpp](../src/node/amariand.cpp).

A read that fails is not an absent value. `GetCoin` returning "no coin" because RocksDB could not
read the block containing it, and `GetCoin` returning "no coin" because the coin was spent, are
the same answer to the caller and opposite facts about the chain. Consensus cannot tell them
apart and should not have to: the interface it was written against is a lookup, and adding an
error channel to it would put I/O handling in the layer that must not have any.

So the failure is recorded where it happens and consulted where it can be acted on. It travels
as a question the sink interface can answer, so `chain` can ask whether its own state is
trustworthy without naming a database. A run that latched a fault fails even though every
individual step of it returned, because the tip it would otherwise print may name a chain whose
coins or bodies are not all present — and a node that prints a tip it cannot substantiate is
worse than one that admits it stopped.

**Consequence:** the exit status is the honest one, but the process may have already reported a
tip in its log before the fault was surfaced. The log line is the node's belief at the time; the
exit status is the verdict on the run. Anything reading a tip from a node's output must check the
status too.

**Reversed if:** the `CoinsView` interface gains an error channel — which would mean threading
`std::expected` through every rule that looks up a coin, and is a much larger change than this
one.

### 75. Restoring a chain re-judges every stored header

**Implemented, 2026-09-05**, as `storage::LoadChain`, which rebuilds the `BlockIndex` by
offering each stored header to the same acceptance path a header off the network takes.

A database file is untrusted input (see [THREAT_MODEL.md](THREAT_MODEL.md)). It may have been
corrupted by a power failure or a failing disk, edited by someone with access to the machine, or
written by an earlier version of this software with a bug. Rebuilding the index by trusting what
is on disk would make every rule the node enforces conditional on nobody having touched a file
— which is to say, not enforced.

The cost is real: startup is linear in the height of the chain, and re-judging a header means
recomputing its work and re-checking its linkage, timestamps and target. That is the price of
the property that a tampered database is caught at startup rather than propagated.

**Consequence:** `LoadChain` needs a clock (decision 71), because the header rules include a
future-time bound. It is given `now` rather than reading one.

**Reversed if:** startup time on a long chain becomes a real problem. The escape is a checkpoint
of validated work, signed by nothing and trusted only as far as the operator's own disk — which
weakens exactly the property above, and so needs measurement to justify.

### 76. The serialised formats are frozen now that the acceptance criterion has passed

**Decided, 2026-09-05.** [AMARIAN_PROTOCOL.md](AMARIAN_PROTOCOL.md) said the encodings were
provisional until two nodes independently validated the same chain. They now do, so they are not
provisional any more.

The rule was written that way because a format is only worth freezing once something depends on
it, and until there were two nodes nothing did. What changed is not confidence in the encodings
but their status: from here, a change to a field's width or order is a hard fork, and is treated
as one rather than as an edit — even though no public chain runs yet, so today's practical cost
of breaking the freeze is regenerating the genesis blocks and discarding stored chains rather
than splitting a live network.

The point of holding the line while that cost is still low is that the layers being built next —
the mempool, the wire protocol, the wallet — all encode assumptions about these bytes. A format
that drifts under them produces bugs that look like protocol bugs and are not.

**Consequence:** anything the formats got wrong is now paid for with a versioned extension rather
than an edit. The two extension points that exist — a version field on every lock, with the
reserved-and-invalid value the extension-point policy sets aside at each one (decision 45), and
an explicit scheme identifier on every public key (decision 23) — were put there for exactly this
reason. `MAX_BLOCK_WEIGHT` is explicitly marked provisional in the protocol document because it
is a parameter and not a format.

**Reversed if:** a defect is found in an encoding that cannot be worked around by a versioned
extension. That is a hard fork, and it would be recorded here as one.

## Still open

| Question | Decided in |
|---|---|
| Hybrid classical + post-quantum authorisation | Phase 7 |
| Ticker, against registries rather than one aggregator | Before an external testnet |
| Address human-readable prefix, network magic, P2P ports | Phase 5 / Phase 10 |
| `MAX_BLOCK_WEIGHT`, on measured decentralisation cost | Phase 12 |
| Soft-fork activation and deprecation mechanics | Phase 8 |
| P2P transport encryption, mandatory-or-absent if adopted | Phase 4 |
| Reproducible builds | Phase 11 |
| Mainnet launch criteria — deliberately unwritten | After Phases 9–12 |





