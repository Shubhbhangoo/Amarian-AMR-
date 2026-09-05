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





