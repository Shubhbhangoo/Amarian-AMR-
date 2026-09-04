# Amarian protocol

## Status: design intent, not implementation

**Nothing in this document is implemented.** It is the design that Phase 1 will
build, written down first so that the decisions are visible and arguable before
there is code defending them. Every structure here is subject to change until the
Phase 1 acceptance criterion passes — two local nodes independently validating the
same chain — after which the serialised formats are frozen, because a format change
after that point is a hard fork.

Where a value is genuinely undecided it says so. Where a value is decided, the
reason is given, because a specification that lists field widths without saying why
is a specification nobody can review.

The layering these structures live in is [ARCHITECTURE.md](ARCHITECTURE.md); the
monetary rules are [ECONOMICS.md](ECONOMICS.md); the attack classes these rules
exist to stop are [THREAT_MODEL.md](THREAT_MODEL.md).

## Conventions

**Canonical encoding.** Every consensus structure has exactly one valid byte
representation. Two encodings of one value means two ids for one transaction, which
means two nodes can disagree about whether they have seen it. Decoders therefore
reject non-canonical input rather than normalising it — normalising is how a
malleable encoding becomes invisible.

**Integers are fixed-width and little-endian.** No host-order serialisation
anywhere. Widths are explicit in every structure below.

**Lengths are compact-encoded and canonically checked.** A variable-length integer
uses the smallest encoding that fits its value; a longer encoding of the same value
is invalid input, not an alternative spelling. Every length is checked against the
bytes actually remaining before anything is allocated.

**No optional fields, no defaults, no reserved-but-ignored space.** A field that is
ignored today is a field an attacker can vary while the transaction id stays the
same, unless it is committed to. Everything present is committed to.

**No floating point, anywhere.** Not in consensus, not in policy, not in fee
estimation. Integer base units only.

## Hashing

Two distinct uses, deliberately kept apart.

| Use | Construction | Why |
|---|---|---|
| Proof of work, block id, transaction id | `SHA256(SHA256(x))` | Double SHA-256 for length-extension resistance and for the same reason Bitcoin uses it: it is the most heavily attacked hash construction in existence and it has held |
| Every internal commitment — Merkle nodes, spend conditions, signature hashes | `SHA256(SHA256(tag) ‖ SHA256(tag) ‖ x)`, the BIP-340 tagged-hash construction | Domain separation. A hash computed for one purpose must never be a valid hash for another, or a structure from one context can be replayed into another |

Tags are ASCII strings naming their purpose, fixed in the consensus parameters:
`Amarian/MerkleLeaf`, `Amarian/MerkleBranch`, `Amarian/SpendCondition`,
`Amarian/SigHash`. The tag is part of the preimage, so a value hashed under one tag
cannot collide with the same value hashed under another except by breaking SHA-256.

**Hashes have two byte orders and the type keeps them apart.** `Hash256` stores
internal order — the order the hash function produces and the order that is
serialised and hashed. Display order is the reverse, by the convention every block
explorer follows. Accessors are separate and named, so a helper function cannot lose
the distinction. A codebase that conflates these produces a node that agrees with
the network and disagrees with every explorer, or, worse, the reverse.

## Amounts

| Property | Value |
|---|---|
| Type | `int64_t`, signed |
| Unit | facet |
| Valid range | `[0, MAX_MONEY]` |
| `MAX_MONEY` | 83 999 999 932 170 000 facets |
| Bits occupied | 57, leaving 109.8× headroom below `INT64_MAX` |

Signed rather than unsigned, which is a deliberate and slightly unusual choice. An
unsigned amount makes a negative value impossible to represent, which sounds safer
until a subtraction underflows and produces an enormous positive number instead of a
detectably wrong one. Signed plus `-fwrapv` plus a range check on every value read
from the wire means a bad amount is *representable and therefore detectable*, and
the check that catches it cannot be optimised away.

Every amount is validated against `[0, MAX_MONEY]` **at deserialisation**, not at
point of use. Validating at point of use means every use site is a potential missed
check; validating on parse means a value that exists in memory has already been
checked. Sums use `CheckedAdd`, which returns `std::optional`, so an overflowed
total cannot be read without acknowledging that it overflowed.

The headroom matters for a specific reason: block validation adds many outputs
together, and a fee-rate calculation multiplies an amount by a size. Both are
intermediates that can exceed any single amount. A cap that fitted only just inside
the type would make every such intermediate an overflow risk, and the overflow of a
monetary sum is an inflation bug.

## Public keys and spend conditions

This is the part of the design that exists specifically to make
[cryptographic agility](PQ_CRYPTO.md) ordinary rather than a rewrite.

```
PublicKey {
    scheme : u16          // explicit scheme identifier
    bytes  : var          // length determined by the scheme, not by a prefix
}
```

**The scheme is explicit and never inferred from length.** Length inference is how a
codebase ends up unable to add a scheme whose key size collides with an existing
one, and it is how a verifier ends up guessing. The scheme identifier resolves
through a registry in the `crypto` layer to a key length, a signature length, and a
verification function; a scheme the node does not know is not a parse error, it is a
key it cannot verify, which is a different and important distinction.

Provisional registry — identifiers are **not final** until Phase 1 fixes them:

| Id | Scheme | Key (bytes) | Signature (bytes) | Status |
|---|---|---|---|---|
| 0 | reserved, never valid | — | — | so that an all-zero field is not a valid scheme |
| 1 | BIP-340 Schnorr over secp256k1, x-only | 32 | 64 | Phase 1 |
| 2 | ML-DSA-44 (FIPS 204) | 1 312 | 2 420 | Phase 6 |
| 3 | SLH-DSA-SHA2-128s (FIPS 205) | 32 | 7 856 | Phase 6, cold storage |

Sizes are the measured raw values from [PQ_CRYPTO.md](PQ_CRYPTO.md#measured-sizes),
not estimates. ML-DSA-65 and ML-DSA-87 are deliberately absent: adding a scheme is
designed to be cheap, so there is no reason to activate three lattice parameter sets
before anyone has asked for one.

```
SpendCondition {
    condition_version : u8
    threshold         : u8           // signatures required
    keys              : PublicKey[]  // 1..=16, distinct
}
```

**There is no script virtual machine, and that is a decision rather than an
omission.** A threshold over a list of scheme-tagged keys expresses single-key
ownership, `m`-of-`n` multisignature, and classical-plus-post-quantum hybrid
authorisation, with one evaluator small enough to audit line by line. A script VM
expresses more — and its evaluation surface is where the interesting consensus bugs
historically live. If a future requirement genuinely needs more expressiveness, a new
`condition_version` is the mechanism, and it arrives with its own review rather than
having been available and unreviewed from the first block.

Constraints, all checked: `threshold` in `1..=len(keys)`; `keys` non-empty, at most
16, sorted, and distinct. Sorted and distinct together make the encoding canonical
and make a threshold impossible to satisfy by counting one key twice.

## Locks

```
Lock {
    version : u8
    program : var    // for version 1: a 32-byte commitment
}
```

For `version = 1`, `program` is
`TaggedHash("Amarian/SpendCondition", serialise(SpendCondition))`.

**A lock commits to a hash of the spend condition and never to the keys
themselves.** Two consequences, and both are load-bearing:

1. An unspent output reveals nothing an attacker can run Shor's algorithm on. The
   key appears on chain only when the coins move.
2. A 1 312-byte ML-DSA key costs exactly as much in the UTXO set as a 32-byte
   Schnorr key — 32 bytes. The entire size cost of post-quantum authorisation lands
   on the spend, where the weight discount applies, rather than on the state every
   node holds forever.

**An unknown lock version is valid-and-spendable for consensus, and non-standard for
relay.** This pairing is what makes a new scheme deployable by soft fork: an old node
accepts a block containing a lock version it does not understand, so it stays on the
same chain, while relay policy prevents anyone from actually creating such an output
before activation. Getting only half of this — rejecting unknown versions — makes
every future upgrade a hard fork.

## Transactions

```
TxInput {
    outpoint : Outpoint            // { txid: hash256, index: u32 }
    sequence : u32                 // relative-locktime semantics, Phase 1 reserved
}

TxOutput {
    amount : i64                   // facets, in [0, MAX_MONEY]
    lock   : Lock
}

Witness {
    condition  : SpendCondition    // the preimage of the lock's commitment
    signatures : Signature[]       // exactly `threshold` of them, key-ordered
}

Transaction {
    version   : u32
    inputs    : TxInput[]          // >= 1
    outputs   : TxOutput[]         // >= 1
    locktime  : u32
    witnesses : Witness[]          // one per input, same order
}
```

Two identifiers, and the split matters:

| Id | Covers | Used for |
|---|---|---|
| `txid` | version, inputs, outputs, locktime — **not** witnesses | Outpoint references. Stable regardless of how the spend was authorised |
| `wtxid` | the entire transaction including witnesses | The Merkle tree, and relay deduplication |

`txid` excluding the witness is what makes the authorisation data segregable and
therefore weight-discountable. It also means a transaction's identity does not change
if the same spend is authorised by a different valid signature set — which removes
the class of malleability where a third party can alter a txid in flight. The Merkle
root commits to `wtxid`, so consensus still commits to every witness byte; the
discount is block-space accounting, not a gap in what is signed.

Coinbase transactions are the one structural exception: exactly one per block, at
index 0, with a single input whose outpoint is all-zero with index `0xFFFFFFFF`, and
whose witness is replaced by an arbitrary byte string carrying the extranonce.

## Signature hash

What a signature commits to, and this list is the whole security of spend
authorisation:

| Committed | Prevents |
|---|---|
| `chain_id` — a distinct constant per network | Replaying a mainnet transaction on testnet or the reverse |
| Transaction version and locktime | Altering either after signing |
| The input index being signed | Reusing input 0's signature for input 1 |
| A hash of all outpoints | Adding or removing an input |
| A hash of all output amounts and locks | Redirecting the coins |
| The spent output's amount | Signing a spend of 1 facet and having it apply to 1 000 |
| A hash of the input's own `SpendCondition` | Satisfying a different condition than the one committed to |
| A hash of all sequence values | Altering relative locktimes |

Everything is hashed under the `Amarian/SigHash` tag.

**The per-transaction hashes are computed once and reused across inputs.** This is
not an optimisation, it is the fix for a specific historical denial-of-service class:
a sighash whose preimage includes the whole transaction, recomputed per input, makes
validation cost grow with the square of transaction size. Bitcoin shipped that and
could not remove it. Committing to precomputed midstates makes each input's preimage
a fixed size, so total validation cost is linear.

There is deliberately **no sighash flag byte** in the Phase 1 design. Every signature
commits to everything. Flags such as "sign only this input" or "sign none of the
outputs" enable real use cases and also enable a long history of subtle failures; if
they are wanted, they arrive as a `condition_version` with their own analysis.

## Blocks

The header is fixed at **92 bytes**:

| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 4 | `version` | u32 |
| 4 | 4 | `height` | u32 |
| 8 | 32 | `prev_block` | hash256 |
| 40 | 32 | `merkle_root` | hash256 |
| 72 | 8 | `timestamp` | i64, seconds since the Unix epoch |
| 80 | 4 | `target_bits` | u32, compact target |
| 84 | 8 | `nonce` | u64 |

Three departures from Bitcoin's 80-byte header, each for a stated reason:

**`height` is in the header.** It is redundant with the block's position in the
chain, and it must be checked equal to `prev.height + 1`. What it buys is that
duplicate coinbase transactions become impossible by construction: two coinbases at
different heights cannot serialise identically, so the entire BIP-30/BIP-34 class of
duplicate-txid problems is closed by the format rather than by a rule bolted on
afterwards. It also lets a node compute the block's scheduled reward from the header
alone.

**`timestamp` is 64-bit and signed.** A 32-bit unsigned timestamp overflows in 2106.
For a chain whose issuance schedule runs to year 178 and whose stated purpose is
multi-decade ownership, shipping a field that stops working is indefensible. Signed
because `time_t` is, and mixing signedness at an interface boundary is how a
conversion warning becomes a bug.

**`nonce` is 64-bit.** A 32-bit nonce is exhausted in milliseconds by modern
hardware, which forces miners to roll the extranonce in the coinbase and rebuild the
Merkle root to continue searching. 64 bits removes that entirely. The extranonce
remains for pool work-splitting, not as a necessity.

`version` is for signalling and future soft-fork activation. Phase 1 sets it to 1 and
requires it; what the bits mean is Phase 8's decision, and inventing a meaning now
would be inventing a mechanism before its requirements are known.

## Merkle tree

Leaves are `wtxid`s in block order. The construction differs from Bitcoin's in three
ways, all to close one specific vulnerability:

1. Leaves are hashed under `Amarian/MerkleLeaf`; internal nodes under
   `Amarian/MerkleBranch`. A leaf hash can therefore never be mistaken for a branch
   hash.
2. **An odd node is promoted unchanged, never duplicated.** Bitcoin duplicates the
   last node of an odd level, which is CVE-2012-2459: a block containing `2n`
   transactions where the second half repeats the first half produces the same Merkle
   root as the `n`-transaction block, so a node can be tricked into marking a valid
   block permanently invalid. Promotion makes the roots differ, because `H(C, C)` is
   not `C`.
3. The leaf count is committed in the root computation, so the tree's shape is
   unambiguous and a one-leaf tree's root cannot be confused with a bare leaf hash.

Any one of the three would close the CVE. All three are present because the cost is
nil and the failure mode is a permanent, remotely triggerable invalidation of a valid
block.

## Proof of work

**SHA-256d**, evaluated over the 92-byte serialised header. Chosen rather than
invented: it is the most-analysed proof-of-work function in existence, and
[the roadmap's constraint](ROADMAP.md) is that novel cryptography needs a compelling
research reason, which "we would like our own ASICs" is not.

Grover's algorithm gives a quadratic speedup on hash inversion, which for mining is a
hashrate change rather than a break — the difficulty adjustment absorbs it exactly as
it absorbs new silicon. This is why the post-quantum work in this project is about
signatures and not about the proof of work.

**Compact target encoding**, the same 32-bit mantissa-and-exponent form Bitcoin uses,
with two rules Bitcoin learned the hard way and this design adopts from the start: a
negative-sign bit is invalid, and a target must be encoded in its canonical form.
Without the canonicality rule the same difficulty has several encodings, each hashing
to a different header, which is malleability in the one field that determines whether
work is valid.

A block is valid work when its hash, read as a 256-bit big-endian integer, is at most
the target decoded from `target_bits`, and `target_bits` equals what the node computes
from the header chain. **The target is never taken from the block** — that field is a
claim, and the node's own recomputation is the fact.

## Difficulty

**ASERT is the intent**, with parameters decided by simulation in
[Phase 3](ROADMAP.md#phase-3--mining-and-difficulty) rather than copied.

The reasoning: a window-boundary retarget of the kind Bitcoin uses is what creates the
timewarp vector, because the rule only inspects timestamps at the edges of the window.
ASERT retargets on every block against an absolute schedule, so there is no boundary
to exploit and no accumulated drift. An exponential response also degrades gracefully
under the failure mode a new chain actually faces — hashrate arriving and leaving
faster than a linear controller can track.

Timestamps are constrained rather than trusted: strictly greater than the median of
the preceding eleven blocks, and not more than a bounded interval ahead of the node's
own view of the time. The second rule is the only place a node's clock enters
validation at all, it is bounded, and it can only delay acceptance of a block rather
than change whether that block is ultimately valid — which is what keeps it from being
a source of permanent divergence.

## Weight and block limits

```
weight = base_size × 4 + witness_size
```

| Constant | Value | Status |
|---|---|---|
| `MAX_BLOCK_WEIGHT` | 2 000 000 | provisional |
| Target block interval | 300 s | settled — the issuance schedule depends on it |
| Coinbase maturity | 200 blocks | intent, confirmed in Phase 2 against reorganisation depth |

The 4:1 discount is what makes post-quantum authorisation affordable: a 3 732-byte
ML-DSA-44 input contributes 3 732 to weight rather than 14 928. The measured
consequence is 467 single-input ML-DSA-44 transactions per block against 3 110 for
Schnorr — a 6.7× reduction, which is the real and honest price, paid in bandwidth and
archival storage rather than in CPU. The full derivation and its assumptions are in
[PQ_CRYPTO.md](PQ_CRYPTO.md#what-the-numbers-imply).

`MAX_BLOCK_WEIGHT` is provisional because it is the one consensus constant that
genuinely depends on measurement not yet taken: initial-block-download time, storage
growth, and the bandwidth a node needs are [Phase 12](ROADMAP.md) numbers. Choosing it
now on the basis that 2 000 000 is a familiar figure would be exactly the reasoning
this project is supposed to avoid.

## Validation order

Order is part of the specification, not an implementation detail: it decides how much
work an attacker can make a node do before their input is rejected. Cheap and
context-free checks come first, expensive and contextual ones last.

**Transaction, in isolation:** structurally decodable and canonically encoded → at
least one input and one output → no duplicate outpoints within the transaction →
every amount in `[0, MAX_MONEY]` → the sum of outputs does not overflow → weight
within limits. None of this needs the UTXO set.

**Transaction, in context:** every input's outpoint exists and is unspent → coinbase
maturity satisfied → the revealed `SpendCondition` hashes to the lock's commitment →
sum of inputs ≥ sum of outputs → *then* signature verification. Signature
verification is last because it is the only step whose cost an attacker can raise
substantially, and by that point everything cheap has already had a chance to reject.

**Block:** header decodes → `height == prev.height + 1` → `target_bits` matches the
node's own recomputation → the hash meets the target → timestamp rules → weight within
limits → exactly one coinbase, at index 0 → Merkle root matches the recomputed root →
every transaction valid → no outpoint spent twice within the block → coinbase output ≤
scheduled reward for `height` + fees actually paid by the block's transactions.

The last check is the supply cap, and it is worth restating that it is enforced from
`height` by every node independently, with no running total to trust and nothing an
operator can configure.

## Networks

Three, with parameters chosen so that a node on one cannot accidentally speak to,
or replay a transaction from, another.

| Parameter | Mainnet | Testnet | Regtest |
|---|---|---|---|
| `chain_id` (in every sighash) | distinct | distinct | distinct |
| Network magic | distinct | distinct | distinct |
| Default P2P port | not chosen | not chosen | not chosen |
| Address prefix | not chosen | not chosen | not chosen |
| Genesis block | distinct | distinct | distinct |
| Difficulty rules | ASERT | ASERT, with a minimum-difficulty allowance | trivial, blocks on demand |
| Issuance schedule | as [ECONOMICS.md](ECONOMICS.md) | same shape, shorter eras | same shape, very short eras |

The unchosen values are unchosen deliberately. A port number and an address prefix
need a check against what other projects already use, and picking them now to fill a
table is how collisions happen. **The address prefix is explicitly not being
deferred past Phase 5**, because unlike the ticker it is baked into address encoding
and changing it later invalidates every address anyone has written down. That
asymmetry — ticker cheap to change, prefix expensive — is why one is deferred and the
other is not; see [ECONOMICS.md](ECONOMICS.md#unit-naming-and-ticker).

`chain_id` inside the sighash is the strong form of replay protection. Network magic
stops nodes connecting; `chain_id` stops a signature itself from being valid
elsewhere, which is the protection that still holds when someone deliberately
bridges two networks.

## Genesis

The genesis block is a chain parameter, not a computed value: hard-coded, with its
hash checked at startup so a node built with a corrupted parameter table fails
immediately rather than forking silently.

**Its coinbase output is unspendable and its reward is zero.** Not a token amount, not
an amount sent to a burn address — zero, enforced as a consensus rule. A genesis
reward that anyone can spend is a premine, and the difference between "earned by work
under published rules" and "allocated by a founder" is the entire distributional claim
this project makes. Making it zero rather than merely unspendable means there is
nothing to argue about.

The coinbase's arbitrary bytes will carry a timestamped reference to a public event
that could not have been known in advance, in the way Bitcoin's did, as evidence that
the chain was not mined before its stated start. The specific text is chosen when
genesis is generated, and no earlier — choosing it now and generating genesis later
would defeat the purpose.

## What is not specified yet

| Area | State |
|---|---|
| P2P wire format, message set, handshake | [NETWORK.md](NETWORK.md), Phase 4 |
| Address encoding | [WALLET.md](WALLET.md), Phase 5 |
| Relay policy, minimum fee, dust threshold | Policy, Phase 4 |
| Soft-fork activation mechanism | Phase 8, and deliberately not invented early |
| `sequence` semantics | Reserved in Phase 1, defined when there is a use for it |
| Scheme identifiers 4 and above | Added when a scheme is needed, which is the point of the registry |
| Hybrid classical + post-quantum conditions | Genuinely undecided; [Phase 7](ROADMAP.md#phase-7--hybrid-ownership) |
| `MAX_BLOCK_WEIGHT`, network ports, address prefix | Provisional or unchosen, as marked above |

## The gap between this document and the code

`src/` currently contains the `util` layer, a version banner, and the node
executable's argument handling. There is no serialisation codec, no primitive, no
consensus rule, no UTXO set, and no cryptography beyond linking OpenSSL to print
which version is loaded.

So this document is a specification to build against, and the only claim it makes is
that the decisions in it were made for the stated reasons. When Phase 1 lands, every
structure here either matches the code or this document is wrong — and the code is
what the network runs.





