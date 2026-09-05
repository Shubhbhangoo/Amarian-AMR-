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
`Amarian/MerkleLeaf`, `Amarian/MerkleBranch`, `Amarian/MerkleRoot`,
`Amarian/SpendCondition`, `Amarian/SigHash`. The tag is part of the preimage, so a
value hashed under one tag cannot collide with the same value hashed under another
except by breaking SHA-256.

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
    bytes  : var          // compact-size length prefix, then the bytes
}
```

**The scheme is explicit and never inferred from length.** Length inference is how a
codebase ends up unable to add a scheme whose key size collides with an existing
one, and it is how a verifier ends up guessing. The scheme identifier resolves
through a registry in the `crypto` layer to a key length, a signature length, and a
verification function; a scheme the node does not know is not a parse error, it is a
key it cannot verify, which is a different and important distinction.

The direction of the inference is reversed, though, and that *is* a consensus rule: a key
whose scheme this node knows must be exactly the length that scheme's registry entry
gives, and so must a signature. Both are checked in the context-free pass, so a key that
could never have parsed is rejected before any cryptographic library is entered. A key
under an unknown scheme has no length this node can check against and is left alone — the
signature rule is additionally scoped to `condition_version` 1, since a later version is
free to define a different relationship between its signatures and the registry.

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
    key_count         : compact size
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
16, sorted, and distinct. Sorted and distinct together make the encoding canonical, and
the [ordered forward match](#validation-order) that consumes them makes a threshold
impossible to satisfy by offering one key's signature twice.

## Locks

```
Lock {
    version : u8
    program : var    // for version 1: a 32-byte commitment
}
```

**Version 0 is permanently unspendable** — a provable burn. There is no spend
condition to commit to, so no witness exists that consensus would accept, and
consensus rejects any input that names such an output. Genesis uses it, because
genesis's zero-value output has to be unspendable as a matter of rule rather than by
the improbability of someone holding a key. Reserving it costs nothing, since it is
fixed before any block exists, and it can never be relaxed: making version 0 spendable
would be a hard fork, which is precisely what "burned" has to mean. It also makes a
default-constructed lock lose coins rather than give them away.

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
every future upgrade a hard fork. Version 0 is the one carve-out, and in the safe
direction: it is not unknown, it is defined to be unsatisfiable.

## Transactions

```
TxInput {
    outpoint : Outpoint            // { txid: hash256, index: u32 }
    sequence : u32                 // relative-locktime semantics, Phase 1 reserved
}

TxOutput {
    amount : i64                   // facets; validated into [0, MAX_MONEY] at parse
    lock   : Lock
}

Signature {
    scheme : u16                   // explicit scheme identifier, same registry as keys
    bytes  : var                   // compact-size length prefix, then the bytes
}

Witness {
    condition        : SpendCondition   // the preimage of the lock's commitment
    signature_count  : compact size
    signatures       : Signature[]      // consensus requires count == threshold, key-ordered
}

Transaction {
    version        : u32
    input_count    : compact size
    inputs         : TxInput[]          // >= 1
    output_count   : compact size
    outputs        : TxOutput[]         // >= 1
    locktime       : u32

    // The witness section, in one of two forms, chosen by the inputs above.
    // Ordinary transaction:
    witness_count  : compact size
    witnesses      : Witness[]          // one per input, same order
    // Coinbase (single input, all-zero txid, index 0xFFFFFFFF):
    coinbase_data  : var                // compact-size length, then <= 100 bytes
}
```

Two identifiers, and the split matters:

| Id | Covers | Used for |
|---|---|---|
| `txid` | version, inputs, outputs, locktime — **not** the witness section, including its count | Outpoint references. Stable regardless of how the spend was authorised |
| `wtxid` | the entire transaction including witnesses | The Merkle tree, and relay deduplication |

`txid` excluding the witness is what makes the authorisation data segregable and
therefore weight-discountable. It also means a transaction's identity does not change
if the same spend is authorised by a different valid signature set — which removes
the class of malleability where a third party can alter a txid in flight. The Merkle
root commits to `wtxid`, so consensus still commits to every witness byte; the
discount is block-space accounting, not a gap in what is signed.

The witness section opens with its own compact-size count, so the txid preimage ends
where that count would begin. A `Signature` has the same self-describing shape as a
`PublicKey` — scheme identifier, then compact-size length, then bytes — so decoding
never needs the scheme registry and an unknown scheme is preserved for relay rather
than guessed at parse time. The signature list carries an explicit count rather than
deriving one from `threshold`, so a witness whose threshold is out of range still
decodes: it is rejected as a rule violation, not as an undecodable byte string.
Consensus then requires the count to equal `threshold` and the signatures to be
ordered like the keys they satisfy.

Coinbase transactions are the one structural exception: exactly one per block, at
index 0, with a single input whose outpoint is all-zero with index `0xFFFFFFFF`, and
whose witness section is replaced by an arbitrary byte string carrying the extranonce.
That byte string is capped at **100 bytes** — enough for an extranonce, a miner's tag,
and genesis's reference, and not enough to be a storage layer for data every node
keeps forever and nobody can verify.

Which of the two forms the witness section takes is decided by the input, which the
wire format places before it, so a decoder never needs the enclosing block to know
what it is reading. A non-coinbase transaction that forged the sentinel would decode
and would then be rejected for spending an outpoint that cannot exist: decoding says
what the bytes are, consensus says whether they are allowed.

**The coinbase input's `sequence` must equal the block height.** This is what actually
closes the duplicate-coinbase problem. Height lives in the header, which the coinbase
transaction does not contain, and the extranonce lives in the witness section, which
the txid preimage excludes — so two coinbases mined at different heights in the same
issuance era, with the same reward and the same output lock, would otherwise serialise
identically and share a txid. Binding the height into the one input field that is
already inside the txid preimage makes every coinbase txid distinct by construction,
with no extra bytes and without a rule that has to look outside the transaction.

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
chain, and it must be checked equal to `prev.height + 1`. What it buys is that a node
can compute the block's scheduled reward, and check the coinbase against it, from the
header alone — and that the height the coinbase input's `sequence` must match is
present in the block that carries it, so the rule closing the BIP-30/BIP-34
duplicate-txid class is checkable without walking the chain. Note that the header
field alone does not close that class: the coinbase txid preimage contains no header,
which is exactly why the rule lives in `sequence`.

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
3. The leaf count and the completed tree root are tagged under `Amarian/MerkleRoot`,
   so the tree's shape is unambiguous and a one-leaf tree's root cannot be confused
   with a bare leaf hash.

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

A block is valid work when its hash, read as a 256-bit big-endian integer **in
display order**, is at most the target decoded from `target_bits`, and `target_bits`
equals what the node computes from the header chain. **The target is never taken from
the block** — that field is a claim, and the node's own recomputation is the fact.

"In display order" is the whole of the byte-order rule and is stated explicitly
because getting it backwards yields a chain that mines, validates, and agrees with
nobody. Display order is the reversed digest order — the form `Hash256::ToHex` prints
and every explorer shows — so the digest's *last* byte is the most significant, and a
hash that satisfies a demanding target has leading zeros when printed. This is the
same convention Bitcoin uses, which is why existing difficulty intuition transfers.

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
context-free checks come first, expensive and contextual ones last. This section and
`src/consensus/validation.cpp` are meant to be read together; where they disagree, one
of them is a bug.

**Transaction, in isolation** (`CheckTransaction`): structurally decodable and
canonically encoded → at least one input and one output → counts within limits → the
coinbase/non-coinbase structural rules, including that no non-coinbase input names the
sentinel outpoint → every amount in `[0, MAX_MONEY]` and their sum likewise → no
duplicate outpoint within the transaction → each witness structurally satisfies its
revealed condition, which includes every key and signature being exactly the length its
scheme defines where the scheme is one this build knows → weight within limits. None of
this needs the UTXO set.

Amounts come before the duplicate-outpoint scan because they are arithmetic over values
already in hand, while the scan builds a hash set sized by the input count; weight is
last because it is the only one of these that serialises the transaction.

**Transaction, in context:** every input's outpoint exists and is unspent → coinbase
maturity satisfied → the revealed `SpendCondition` hashes to the lock's commitment →
sum of inputs ≥ sum of outputs → *then* signature verification. Signature
verification is last because it is the only step whose cost an attacker can raise
substantially, and by that point everything cheap has already had a chance to reject.

The last three of those are `CheckSpendAuthorisation` and `TransactionFee`, which take the
outputs being spent as an explicit list rather than a database handle: finding those
outputs is a lookup, and deciding whether they may be spent is arithmetic and
cryptography, so the expensive half stays a pure function of values.

**How a threshold is satisfied.** A witness's signatures are matched against its
condition's keys by **ordered forward match**: a single key index advances monotonically
across the whole signature list, so signature *i* is tried only against keys left over
after signature *i−1* stopped. Two consensus properties follow, and both are the reason
for specifying it rather than trying every pair:

- **Cost.** At most `keys.size()` verifications happen for an input no matter how many
  signatures are offered, so the work an input can demand is bounded by the key count its
  own commitment fixed — and a threshold condition is capped at 16 keys. Trying every pair
  would be quadratic in a number the spender chooses.
- **No malleability.** Exactly one ordering of a given signature set verifies: the
  ascending one. Any permutation of a valid witness is invalid, so a relayer cannot
  reorder signatures to produce a second `wtxid` for one transaction.

Each key therefore counts at most once, which is what makes a 2-of-2 need two distinct
keys rather than one signature offered twice.

A key that is the right length for its scheme but that the implementation cannot parse is
skipped, exactly like one whose signature failed — not treated as fatal. The keys were
fixed when the coin was created, and rejecting outright would make an *n*-key condition
unspendable because of one key that could never have verified anything.

At both extension points an unknown value stays spendable, and this is where that has
teeth: an **unknown lock version** is spent without any signature check, and a signature
under an **unknown key scheme** counts as satisfied. This is the only place in consensus
where something unverified is accepted, and it is deliberate — a node cannot check a rule
that does not exist in it, and rejecting would fork it off the chain the moment the rule
was deployed. Upgraded nodes do check; a hashpower majority enforcing a rule old nodes
cannot see is what a soft fork is.

**Block, in two stages.** The split is not cosmetic: a header can arrive unsolicited,
before the node has the block it claims to build on, so the part that needs no
predecessor has to be able to run alone.

`CheckBlockHeader` first, on the header alone: `target_bits` is a valid compact
encoding → it is no easier than the network's floor → the hash meets it. This is the
only check an attacker must pay more to pass than a node pays to run, so it gates
everything that follows and runs before the block index is touched at all.

`ContextualCheckBlockHeader`, once the predecessor is known: `prev_block` names it →
`height == prev.height + 1` → `target_bits` equals the value this node computes for
itself → timestamp strictly above the median of the last `MEDIAN_TIME_SPAN` blocks →
timestamp no more than `MAX_FUTURE_BLOCK_SECONDS` past this node's clock.

`CheckBlock` for the body: at least one transaction → transaction count within limits →
exactly one coinbase, at index 0 → the coinbase input's `sequence` equals the header's
`height` → weight within limits → Merkle root matches the recomputed root → every
transaction valid in isolation → no outpoint spent twice within the block.

The coinbase structural rules precede the weight check because they are a handful of
comparisons per transaction with nothing serialised, and weight precedes the Merkle root
because it is the bound on how much hashing the root can be made to cost.

Then, with the UTXO set: the contextual transaction rules above, and last
`CheckCoinbaseAmount` — the coinbase's outputs sum to no more than the scheduled reward
for `height` plus the fees the block's transactions actually paid.

The last check is the supply cap, and it is worth restating that it is enforced from
`height` by every node independently, with no running total to trust and nothing an
operator can configure.

## Networks

Three, with parameters chosen so that a node on one cannot accidentally speak to,
or replay a transaction from, another.

| Parameter | Mainnet | Testnet | Regtest |
|---|---|---|---|
| `chain_id` (in every sighash) | `4c6c27ec…9d6a8bb9` | `e29a85d4…a5f467fe` | `75b9d4c0…e01fa5cc` |
| Network magic | `CC 6C 27 EC` | `E2 9A 85 D4` | `F5 B9 D4 C0` |
| Default P2P port | 12500 | 12510 | 12520 |
| Default RPC port | 12501 | 12511 | 12521 |
| Address prefix | not chosen | not chosen | not chosen |
| Genesis block | distinct | distinct | distinct |
| Proof-of-work floor | `0x1d00ffff` | `0x1d00ffff` | `0x207fffff` |
| Difficulty rules | ASERT | ASERT, with a minimum-difficulty allowance | trivial, blocks on demand |
| Issuance schedule | as [ECONOMICS.md](ECONOMICS.md) | same shape, eras 100× shorter | same shape, eras 1 000× shorter |
| Coinbase maturity | 200 blocks | 200 blocks | 20 blocks |

`chain_id` is derived, not invented:

```
chain_id = TaggedHash("Amarian/chain-id", network_name)
network magic = chain_id[0..4], with the top bit of the first byte set
```

Deriving both from the network's name means there is one place a network's identity
comes from, and no second constant that could be edited independently. The values
above are compile-time constants in `include/amarian/consensus/params.hpp`, and a
test recomputes them through the hash layer so a mistyped byte fails a build rather
than producing a network whose signatures no other implementation can reproduce.

Setting the magic's top bit costs nothing and buys two things: the magic can never be
printable ASCII, so an HTTP request or a stray line of text arriving on the P2P port
is rejected at the first byte; and the magic is not a bare prefix of a published hash,
so it cannot be mistaken for a truncated `chain_id`. The three magics also differ in
their *first* byte, so a mis-dialled peer fails on the first byte read.

**The ports were checked, not picked.** 12500 is the base because 12.5% is the
per-era reward reduction — the one number this monetary policy owns. The band
12500–12521 is unassigned in the IANA service-name and port-number registry and is
clear of the de-facto ports of the major chains; `scripts/check_port_registry.sh`
is that check and can be re-run. It is also below 32768, which keeps it out of
Linux's default ephemeral range, where a default listening port can lose a bind race
against an outbound connection's source port.

**The address prefix remains unchosen, deliberately**, and is explicitly not being
deferred past Phase 5: unlike the ticker it is baked into address encoding, and
changing it later invalidates every address anyone has written down. That asymmetry —
ticker cheap to change, prefix expensive — is why one is deferred and the other is
not; see [ECONOMICS.md](ECONOMICS.md#unit-naming-and-ticker).

`chain_id` inside the sighash is the strong form of replay protection. Network magic
stops nodes connecting; `chain_id` stops a signature itself from being valid
elsewhere, which is the protection that still holds when someone deliberately
bridges two networks.

## Genesis

Genesis is a chain parameter, and it is stored as a builder plus a recorded hash
rather than as a hard-coded blob of bytes. A serialised block committed as hex is a
value nobody reviews — it either works or it does not, and no reader can tell what is
inside it. `BuildGenesisBlock` in `include/amarian/consensus/genesis.hpp` constructs it
from the parameter table, so every field is reviewable code; the hash is recorded
separately in that table, so it remains an independent statement about what the code
must produce. `CheckGenesis` compares the two at node startup and the node **refuses
to start** on any mismatch, because a node whose block 0 differs from the network's
does not fork from it — it shares no history with it at all.

**Its coinbase output is unspendable and its reward is zero.** Not a token amount, not
an amount sent to a burn address — zero, enforced as a consensus rule. A genesis
reward that anyone can spend is a premine, and the difference between "earned by work
under published rules" and "allocated by a founder" is the entire distributional claim
this project makes. Making it zero rather than merely unspendable means there is
nothing to argue about. The output's lock is version 0, which no witness can satisfy by
rule rather than by improbability.

**That unspendable lock's `program` is the network's own `chain_id`**, which is what
makes the three genesis blocks distinct. Mainnet and testnet share every numeric
parameter, so without it their genesis blocks would have differed only by an
independently mined nonce and could in principle have coincided. Putting the chain id
in the coinbase puts the difference inside the Merkle root that the header commits to,
so the three blocks are distinct by construction rather than by luck.

The coinbase's arbitrary bytes carry a dated public reference, in the way Bitcoin's
did:

```
Reuters 05/Sep/2026 Philippine VP Duterte posts bail after arrest order
```

Reuters, Manila, Saturday 5 September 2026: the Philippine vice president posted bail
after a court ordered her arrest on three counts of grave threats. The same event was
reported that day by AP, NPR, Rappler and GMA as well, so it is checkable against
several independent archives rather than one. Nobody could have written that sentence
before 5 September 2026, so no genesis block containing it existed before then either.
That is the entire claim: a lower bound on the chain's age. It says nothing about when
mainnet opens to the public, which is a launch procedure and is documented as one in
[ROADMAP.md](ROADMAP.md#phase-13--mainnet-readiness).

All three networks were generated at `GENESIS_TIMESTAMP = 1788598800`
(2026-09-05T09:00:00Z). The block is 257 bytes, weight 812.

| Network | Nonce | Genesis hash |
|---|---|---|
| Mainnet | `570575708` | `000000001872d649f36e25be94d2f562f06bc243f582af561f1ffe1ab376662f` |
| Testnet | `2119016593` | `0000000037c5c1182536c905bb2e23931e3393e476f806405045c2d5332876c0` |
| Regtest | `3` | `02248d2fa761c196fd9a63f7a44c1efbca3acf2e882c3f1c707aba6237bda967` |

Mainnet and testnet both mined at the `0x1d00ffff` floor, which is why both hashes open
with eight zero hex digits and why the nonces are in the billions — the expected search
is about 2^32 attempts. Regtest's floor is `0x207fffff`, so nonce 3 was enough and its
hash has only two leading zero *bits*; that is the point of a network where a block must
cost a couple of hash attempts rather than minutes.

The nonces are the recorded output of a search anyone can repeat: `amarian-genesis
--mine` finds the **smallest** satisfying nonce for each network, in rounds completed
across all threads before the next begins, so the answer does not depend on how many
cores ran it. `amarian-genesis` with no arguments prints each network's parameters and
its `CheckGenesis` verdict, which is how a reviewer reproduces a consensus constant
instead of trusting it.

## What is not specified yet

| Area | State |
|---|---|
| P2P wire format, message set, handshake | [NETWORK.md](NETWORK.md), Phase 4 |
| Address encoding | [WALLET.md](WALLET.md), Phase 5 |
| Relay policy, minimum fee, dust threshold | Policy, Phase 4 |
| Soft-fork activation mechanism | Phase 8, and deliberately not invented early |
| `sequence` semantics for non-coinbase inputs | Reserved in Phase 1; a coinbase's must equal the block height |
| Scheme identifiers 4 and above | Added when a scheme is needed, which is the point of the registry |
| Hybrid classical + post-quantum conditions | Genuinely undecided; [Phase 7](ROADMAP.md#phase-7--hybrid-ownership) |
| `MAX_BLOCK_WEIGHT` | Provisional, as marked above; it needs Phase 12 measurements |
| Address prefix | Unchosen, and being chosen in Phase 5 rather than deferred |

## The gap between this document and the code

Updated as the code lands, because a specification that silently outruns its
implementation is how "designed for" becomes indistinguishable from "does".

**Implemented and tested:** the canonical serialisation codec; the consensus hash
layer over OpenSSL (SHA-256, double SHA-256, and the BIP-340 style tagged hash); the
primitives — amounts, outpoints, locks, spend conditions, witnesses, transactions
with their `txid`/`wtxid` split and the coinbase's byte-string witness section, the
Merkle tree with its CVE-2012-2459 defences, and block headers and blocks with the
weight formula; the first consensus rules — the compact target codec with the
proof-of-work check, the issuance schedule with its supply cap verified at compile
time, and the per-network parameter sets; the three genesis blocks, with the
startup check that refuses to run a build whose block 0 is not the network's; and the
context-free validation rules — every rule in "Validation order" above that needs
neither the UTXO set nor a signature, as `CheckSpendCondition`, `CheckWitness`,
`CheckTransaction`, `CheckBlockHeader`, `CheckBlock`, and the two rules that are
contextual but expressible as pure functions of an explicit context,
`ContextualCheckBlockHeader` and `CheckCoinbaseAmount`; the signature hash, as
`ComputeSigHashMidstates`, `SignatureHash` and `SpendConditionCommitment`; the signature
scheme registry over libsecp256k1 and OpenSSL, as `crypto::Verify`, with the startup gate
that refuses to run a build whose OpenSSL cannot provide a scheme consensus knows; and
spend authorisation, as `CheckSpendAuthorisation` and `TransactionFee` — the lock
commitment, the ordered threshold walk against real signatures, and inputs covering
outputs.

**Specified here but not yet implemented:** the two contextual transaction rules that
need chain state — that an input's outpoint exists and is unspent, and coinbase maturity;
the UTXO set; the block index and chain selection; persistence; the P2P protocol; the
wallet; and difficulty retargeting, which is Phase 3 work and deliberately not attempted
early.

A block that passes `CheckBlock` is therefore **not yet valid**: it has not been checked
against the UTXO set, and `CheckBlock` does not call `CheckSpendAuthorisation`, because
the outputs being spent are not something a block carries. The function is named for the
half it actually does. Nothing in the code is allowed to imply otherwise until the rules
in the paragraph above exist.

Where this document and the code disagree, the code is what the network runs, and the
disagreement is a bug in one of them.

