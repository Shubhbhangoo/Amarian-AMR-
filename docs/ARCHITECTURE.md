# Architecture

## The one rule

```
util  <-  crypto  <-  primitives  <-  consensus  <-  utxo  <-  chain  <-  storage
                                         ^                       ^
                                         |                       |
                                 mempool / mining  ---------------+
                                         ^
                           net / wallet / rpc / node / cli
```

Arrows point from a layer to what it is allowed to depend on. **Consensus never
links storage, networking, wallet or RPC.** That absent edge is the most
important line in this document.

Note where the database sits: `storage` is *above* everything it serves. It implements
interfaces declared by the layers below it — `utxo::CoinsView`, `chain::BlockStore`,
`chain::ChainSink` — rather than being reached down to. So a bug in RocksDB's option handling
cannot reach the code that decides whether a block is valid, because that code cannot see
RocksDB at all: the dependency points the other way.

It is not a style preference. Every dependency consensus acquires is code that
can change what a node considers valid, and therefore code that has to be audited
to the same standard. A consensus layer that can reach RocksDB can have a
validation outcome that depends on a cache state; one that can reach the network
can have an outcome that depends on which peer answered first. Both are chain
splits waiting for the right timing.

The rule is enforced by the build, not by review: each layer is a separate CMake
target that links only its permitted dependencies. Adding a forbidden `#include`
fails to link. That check runs on every build, which is the only kind of check
that keeps working after everyone stops paying attention.

## What exists today

The close of Phase 1. Nine libraries and two executables, each a CMake target linking only
its permitted dependencies:

| Target | Contents | Links |
|---|---|---|
| `amarian::util` | bytes and hashes, hex codec, canonical serialisation, checked arithmetic, logging, CLI option parsing | `amarian_settings` only |
| `amarian::crypto` | SHA-256, double SHA-256, tagged hashing, the signature scheme registry and verification | `util`; OpenSSL and libsecp256k1 privately |
| `amarian::primitives` | amounts, outpoints, locks, spend conditions, witnesses, transactions, blocks, the Merkle tree, the signature hash | `crypto` |
| `amarian::consensus` | chain parameters, issuance, the compact target codec, genesis, accumulated work, and the validation rules | `primitives` — and deliberately nothing else |
| `amarian::utxo` | the coins cache and view/sink interfaces, `ConnectBlock`, `DisconnectBlock`, undo records | `consensus` |
| `amarian::chain` | the header tree, work per branch, the best-tip rule, the switch plan, and activation | `utxo` |
| `amarian::mining` | block assembly from a tip and the bounded nonce search | `chain` |
| `amarian::storage` | coins, bodies, undo records, the header tree and the tip in RocksDB | `chain`; RocksDB privately |
| `amarian::version` | build identity, including the OpenSSL actually loaded at run time | OpenSSL, privately |
| `amariand` | the node executable | `storage`, `mining`, `consensus`, `util`, `version` |
| `amarian-genesis` | prints, checks and regenerates the genesis parameters | `consensus` |

Two things about that table are load-bearing. `amarian::consensus` links `primitives` and
nothing else, which is the absent edge above expressed as a build rule. And every third-party
library is linked `PRIVATE`, so a target that links `amarian::crypto` gets Amarian's interface
rather than OpenSSL's headers — a caller that could reach `EVP` directly would be a second
place cryptography happens.

`amarian_settings` is an interface target carrying the include path, the C++23
requirement, the warning set, the hardening flags and the sanitizer flags.
Everything links it, so nothing can be built with a weaker configuration than the
rest of the project by accident.

`amarian::version` links OpenSSL and is deliberately its own target. Folding it
into `util` would give every consumer of `ByteVec` a dependency on OpenSSL in
order to print a version banner, and would put a crypto library on the link line
of the layer that consensus depends on most directly.

Not built yet: `mempool`, `net`, `wallet` and `rpc`. Their responsibilities are below.

## What each layer is responsible for

**`util`** — no domain knowledge. Bytes, hashes as opaque values, hex, checked
integer arithmetic, logging, option parsing, error types. Nothing in `util` knows
what a transaction is, and that is what makes it safe for every other layer to
depend on.

**`crypto`** — the only layer that calls libsecp256k1 or OpenSSL. Hashing,
tagged hashing, signature verification, key handling, the CSPRNG. Exposes a
scheme registry keyed by an algorithm identifier, so callers ask "verify this
signature under scheme 1" rather than naming an algorithm. That indirection is
what makes [cryptographic agility](PQ_CRYPTO.md) an ordinary feature instead of a
rewrite, and it is why no other layer is allowed to `#include <openssl/...>`.

**`primitives`** — the data. Amounts, outpoints, spend conditions, transactions,
blocks, headers, Merkle trees, and the canonical serialisation of each. Pure data
plus its encoding, with no policy: a `Transaction` can be malformed, and saying
whether it is acceptable is somebody else's job.

**`consensus`** — the rules. Chain parameters, the issuance schedule, the compact
target encoding, proof-of-work checking, accumulated work, the signature
hash, and transaction and block validation. This is the layer where a bug is a
chain split, so it is kept deliberately small, has no I/O, no clock, no
randomness, and no allocation it does not need. Every function here is a pure
function of data the chain commits to — including the two rules that are contextual,
`ContextualCheckBlockHeader` and `CheckCoinbaseAmount`, which take their context as an explicit
value. Difficulty retargeting will be a pure function on the same pattern; gathering the
ancestor headers it reads is `chain`'s job, not this layer's, which is why `NextTargetBits`
lives there.

**`utxo`** — the unspent output set and its transitions. Applying and reverting a
block, and the interface a validator uses to ask what an outpoint refers to.
Separated from `consensus` because the *rules* about a transition and the
*storage* of the set have very different testing needs.

**`chain`** — the block index, chain selection, and activation: the tree of known headers, the
accumulated work on each branch, the plan for moving from one tip to another, and the walk that
carries it out over the unspent output set. It touches no disk. Block bodies and undo records
reach it through the `BlockStore` interface it declares, which is what lets the code deciding
which chain is real be tested without a database.

**`storage`** — the state that survives a restart, and the only layer permitted to link a
database. Coins, block bodies, undo records, the header tree and the tip, in one RocksDB
instance with a column family each, written one atomic batch per block. It implements the
interfaces the layers below declare; nothing below it names RocksDB.

**`mempool`, `mining`** — policy, not consensus. Relay rules, fee estimation,
block template construction. The distinction is load-bearing: a policy rule that
is stricter than consensus is fine and can differ between nodes, while a policy
rule mistaken for a consensus rule is a split. `mining` is also the only layer that answers
*what block should exist next* rather than *is this block allowed*, and it is a separate target
from `consensus` so that an assembler cannot make a block pass by sharing a mistake with the
validator — it self-checks against the rules instead of assuming them.

**`net`** — the wire protocol and peer management, via Asio. Where untrusted
bytes arrive.

**`wallet`** — keys, addresses, coin selection, signing, backup. Above the node
and outside consensus: a wallet bug can lose its owner's coins, but must not be
able to change what the network accepts.

**`rpc`** — JSON-RPC. nlohmann/json is used here and nowhere else; JSON never
touches consensus serialisation, because a consensus encoding must be canonical
and a JSON library's output is not.

**`node`, `cli`** — assembly and the executables. The only layers allowed to know
about all the others.

## Conventions that hold everywhere

**Errors are values.** `Result<T>` is `std::expected<T, Error>`, and `Error`
carries a stable machine-readable context string plus a human detail — for
instance `args.invalid_integer` with `--port=abc`. Exceptions are not used for
control flow in validation: a validation failure is an expected outcome of
processing untrusted input, not an exceptional one, and `AMARIAN_TRY` makes the
propagation explicit at every call site rather than invisible.

The stable context strings matter for a reason beyond tidiness. When two nodes
disagree, comparing why each one rejected a block is the fastest route to the
cause, and that comparison only works if the reason is a stable identifier rather
than prose someone will reword.

**Arithmetic on anything an attacker influences is checked.** `CheckedAdd`,
`CheckedSub`, `CheckedMul` return `std::optional`, so an overflowed result cannot
be read without acknowledging it. `-fwrapv` is set so that any signed overflow
which does slip past review has defined behaviour instead of licensing the
optimiser to delete the check that would have caught it. The measured cost of
this, and the decision to keep it anyway, is in
[bench/util_overflow_bench.cpp](../bench/util_overflow_bench.cpp).

**Two byte orders, never mixed silently.** `Hash256` stores bytes in internal
order and has separate accessors for internal and display order. Block hashes are
conventionally displayed reversed; a codebase that conflates the two produces a
node that agrees with the network internally and disagrees with every explorer, or
worse, the other way round. The distinction is in the type's API so it cannot be
lost in a helper function.

**One canonical encoding.** A consensus serialisation must have exactly one
byte representation per value, or two nodes can hash the same transaction to two
different ids. This is why JSON is confined to RPC.

## Testing structure

| Directory | Label | Runs |
|---|---|---|
| `tests/unit/` | `unit` | Fast, no I/O. Every commit. |
| `tests/consensus/` | `consensus` | Rule-by-rule, including vectors that must be rejected. Gates a release on its own via `ctest -L consensus`. |
| `tests/integration/` | `integration` | Multiple nodes, real sockets, real disk. Empty today: the one multi-node check that exists, the Phase 1 acceptance criterion, is a shell script driving two real `amariand` processes — [scripts/phase1_acceptance.sh](../scripts/phase1_acceptance.sh) — run deliberately rather than on every build, because it costs two RocksDB directories and a mining run. It moves here when there are sockets to test and the setup is worth a fixture. |
| `fuzz/` | — | One libFuzzer harness per parser; see [fuzz/README.md](../fuzz/README.md). |
| `bench/` | — | Built in the shipping configuration, so numbers describe what ships. |

Consensus tests are labelled separately because "did I break a consensus rule" is
a different question from "did I break the build", and it should be answerable in
one command without waiting for anything slow.

Fuzz harnesses assert correctness properties, not only memory safety. A parser can
be perfectly memory-safe and still accept a transaction it should reject, and only
the second kind of bug can split a chain.


