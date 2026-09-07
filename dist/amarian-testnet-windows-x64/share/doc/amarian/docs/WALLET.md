# Wallet

## Status: complete — Phase 5

**Implemented.** Key derivation from a 256-bit master seed via HKDF-SHA256,
bech32m address encoding (BIP-350), wallet database with seed encryption at
rest, coin selection with dust-threshold enforcement, fee estimation from a
rolling window of recent blocks, transaction builder with Schnorr and ML-DSA-44
signing, BIP-39 backup/restore (full 2048-word English wordlist), and the
standalone `amarian-wallet` CLI. The end-to-end acceptance criterion — create an
address, acquire coins, send them, and receive them on a different node with the
backup verified by restoring it — is demonstrated by
[scripts/phase5_acceptance.sh](../scripts/phase5_acceptance.sh).

## What the wallet is not allowed to do

The wallet sits above the node in the [layering](ARCHITECTURE.md) and links
`consensus` only to read rules, never to make them. The consequence is deliberate and
worth stating as a design goal rather than an accident:

**A wallet bug must be able to lose its owner's coins. It must not be able to change
what the network accepts.** The first is unavoidable in self-custody. The second is a
structural property, enforced by the build.

So the wallet has no path to relax a validation rule, no path to construct a
transaction the node would accept but a peer would not, and no privileged position of
any kind. It is an ordinary client of the node's interface, which is also what makes a
third-party wallet possible later.

## The user-facing model

The design constraint from the outset: **a normal user must never encounter
cryptography.** Not the name of a signature scheme, not a key, not a hash, not the
word "post-quantum". Someone protecting savings for twenty years should get
post-quantum authorisation because it was the right default, not because they read
[PQ_CRYPTO.md](PQ_CRYPTO.md).

Six things, and nothing else at the top level:

| | Shows |
|---|---|
| **Balance** | One number, in AMR. Confirmed and pending distinguished, because "pending" is the honest state of a broadcast transaction |
| **Receive** | A fresh address, every time, with a QR code |
| **Send** | Recipient, amount, fee, confirm |
| **Transactions** | What happened, when, and to whom, in that order |
| **Backup** | A recovery phrase, and a way to check that it works |
| **Settings** | Fee preference, currency display, node connection |

A send is four fields and one confirmation. The confirmation screen renders the
transaction that will actually be signed, derived from the same structure the signer
receives — not a re-description assembled separately, which is how a display and a
signature come to disagree.

Amounts are displayed as decimal AMR with ten places, and every amount is an integer
number of facets underneath. The conversion happens at the interface boundary, on
integers, and no monetary value is ever a floating-point number anywhere in the
codebase.

## Where the cryptography hides: account types

Users choose a **purpose**, not an algorithm.

| Type | For | Scheme | Trade-off, in the user's terms |
|---|---|---|---|
| **Everyday** | Spending, receiving, day-to-day balances | BIP-340 Schnorr, migrating to ML-DSA-44 when it activates | Small, fast, cheap to send |
| **Vault** | Long-term savings, rarely touched | SLH-DSA-SHA2-128s | "Sending takes a few seconds and costs more. Built to still be safe in decades." |

That is the entire user-visible surface of scheme selection: two names, and a sentence
each. The words "lattice", "hash-based", "ML-DSA" and "SLH-DSA" appear in an advanced
view for people who want them, and nowhere else.

The mapping is evidence-driven rather than aesthetic. SLH-DSA takes **424 ms to
produce one signature** on measured hardware, so a fifty-input sweep takes twenty
seconds — irrelevant for a vault spent once a decade, unacceptable for a hot wallet.
Its public key is only 32 bytes, so it costs nothing extra to *hold*; the entire cost
lands on the spend. That is exactly the shape of a cold-storage scheme, and it is why
offering it as *an* option rather than *the* option was the right call. Numbers in
[PQ_CRYPTO.md](PQ_CRYPTO.md#measured-performance).

## Key derivation, and why BIP-32 does not work here

This is the part of the wallet design that a post-quantum chain cannot inherit, and it
is worth being precise about why.

**BIP-32 derives child keys by arithmetic on secp256k1 scalars.** `child = parent + H(...)`
is a group operation, and it is what gives Bitcoin its two most useful wallet
properties: a whole tree from one seed, and *public* derivation — an extended public key
that generates receive addresses without the private key ever being present.

Neither ML-DSA nor SLH-DSA has that structure. There is no scalar to add and no group
to add it in. A lattice key is generated from a seed by an algorithm, not derived from a
parent by arithmetic. Any attempt to force a BIP-32-shaped hierarchy onto them is
either inventing cryptography or pretending.

So derivation happens **one level lower, on seed bytes**:

```
master seed  (256 bits, from the recovery phrase)
     |
     +-- HKDF-SHA256, info = "Amarian/KeyDeriv/v1" || scheme || account || index
     |
     +-- per-key seed bytes, of exactly the length the scheme's keygen wants
     |
     +-- the scheme's own standard keygen  (FIPS 204 §5.1, FIPS 205, or a secp256k1 scalar)
```

This works for every scheme in the registry, present and future, because every
signature scheme has a "generate a key from this many random bytes" entry point.
FIPS 204 keygen takes a 32-byte seed; FIPS 205 takes three `n`-byte seeds; a secp256k1
key is 32 bytes reduced into range. The derivation is domain-separated by scheme, so the
same account and index under two schemes produce unrelated keys — otherwise a break in
one scheme would leak information about keys under another.

**What this costs, stated plainly: there is no watch-only extended public key.** Public
derivation is a property of BIP-32's algebra and it does not survive generalisation.
Deriving a receive address requires the seed. The mitigation is that a wallet can export
an explicit list of addresses for watch-only use, and can pre-generate a gap of them —
which covers the real use case (monitoring a balance from a machine that holds no keys)
without claiming a capability the mathematics does not provide.

This is a genuine regression against Bitcoin's wallet model, it is a direct consequence
of post-quantum schemes rather than a design preference, and no amount of engineering
removes it.

## Addresses

**bech32m** ([BIP-350](https://github.com/bitcoin/bips/blob/master/bip-0350.mediawiki)),
carrying a version byte and the 32-byte lock commitment.

```
<hrp>1<version><32-byte commitment><6-char checksum>
```

bech32m rather than bech32, for a specific reason: bech32's checksum has a known
weakness where inserting or deleting characters at the end of the data part can produce
another valid string. bech32m changes the checksum constant to remove it. Choosing the
version with the known flaw, when the fixed version exists and costs nothing, would be
indefensible.

What bech32m buys beyond that: a checksum that detects any four character errors and
locates single ones, case insensitivity, QR-friendly alphanumeric encoding, and no
visually confusable characters — `1`, `b`, `i` and `o` are excluded from the data
alphabet.

**The human-readable prefix is not chosen yet, and is being decided in Phase 5 rather
than deferred.** Unlike the [ticker](ECONOMICS.md#unit-naming-and-ticker), which is a
display-layer parameter changeable by editing a table, the prefix is baked into every
address anyone writes down. Changing it later invalidates all of them. It needs a check
against prefixes already in use before it is fixed, and then it is fixed permanently.

**An address encodes a commitment, not a key.** So an ML-DSA address and a Schnorr
address are the same length and indistinguishable, which is a privacy property that
falls out of the design rather than being added: an observer cannot tell which
signature scheme an unspent output uses until it is spent.

## Address reuse is treated as a defect

Every receive request produces a fresh address, and there is no interface path that
presents a previously used address as "your address". Not a warning, not a preference —
the reused address is simply not offered.

The justification is not privacy, or not only privacy. **A public key on chain is a
public key an attacker can run Shor's algorithm against.** Spending once from an
address publishes its key; anything left at that address is exposed from the moment the
spend confirms. Address reuse converts a hash-protected output into a key-protected one,
which is the difference between "safe until spent" and "safe until quantum computers
exist".

That makes it a security property, which is why it is enforced in the interface rather
than documented in a footnote. Guidance that users are expected to follow is guidance
that some users will not follow. Full reasoning in
[PQ_CRYPTO.md](PQ_CRYPTO.md#why-exposed-public-keys-are-the-pivot).

## Coin selection

Requirements, in priority order:

1. **Never overpay by more than necessary.** Excess above the target and fee becomes
   change, and change costs an output now and an input later.
2. **Prefer exact matches, to avoid creating change at all.** A branch-and-bound search
   for a subset summing within the fee tolerance of the target, falling back to a
   randomised knapsack when none exists.
3. **Never create an output below the dust threshold.** An output too small to be worth
   spending is a permanent cost to every node's UTXO set and a permanent loss to its
   owner.
4. **Do not leak more than necessary.** Avoid combining inputs from unrelated receives
   when an alternative exists, because common-input clustering is the single most
   effective chain analysis heuristic. This is a preference that yields to the first
   three, and saying so is more useful than implying the wallet defeats analysis.
5. **Be deterministic given the same inputs and the same randomness source.** Not for
   any user-facing reason — because a coin selector whose output cannot be reproduced
   cannot be tested, and an untestable component that handles money is unacceptable.

**Post-quantum sizes change the arithmetic here in a way worth noticing.** With
ML-DSA-44, each input carries 3 732 bytes of authorisation data against Schnorr's 96 —
**38.9×**. The marginal fee cost of one more input therefore dominates selection far
more than it does on Bitcoin, and a selector tuned for 96-byte inputs makes bad choices
at 3 732. Input count matters much more than input value. Consolidating dust while fees
are low goes from a nice habit to a materially valuable one.

## Fees

Fee estimation from observed inclusion, over a recent window of blocks, presented as
three choices — economical, normal, priority — with the estimated confirmation time
attached, plus an explicit override for anyone who wants one.

The fee is always visible before confirmation, in AMR and as a fee rate. A wallet that
hides the fee until after signing has removed the only decision the user was entitled
to make.

Two honest limits. Fee estimation on a young chain with few transactions is guesswork,
because there is no fee market to observe; the wallet should say "the network is not
busy" rather than manufacture a number that looks authoritative. And no estimate can
account for a fee spike after broadcast — which is why replacement, and a clear
"stuck transaction" state rather than silence, are part of Phase 5 and not a later
addition.

## Signing

**No cryptographic primitive is implemented here.** BIP-340 signing is libsecp256k1's;
ML-DSA and SLH-DSA signing is OpenSSL's. The wallet's job is to construct the correct
preimage, call the right function, and handle the key material carefully.

| Concern | Handling |
|---|---|
| Nonce generation | BIP-340's deterministic derivation, from libsecp256k1's own implementation. Never a nonce this project generates — nonce reuse in Schnorr or ECDSA reveals the private key outright, and it is the single most repeated catastrophic wallet bug in the field |
| Randomness | The OS CSPRNG through OpenSSL. Never time-seeded, never user-supplied |
| Key material in memory | Held for as short a time as possible, zeroed after use, never written to a log, never included in an error message or a crash report |
| Signing latency | An SLH-DSA vault spend takes 424 ms per signature. A multi-input spend must show progress rather than appear to hang, and must remain cancellable up to the point of broadcast |
| What is signed | The preimage described in [AMARIAN_PROTOCOL.md](AMARIAN_PROTOCOL.md#signature-hash), covering `chain_id`, the input index, the spent amount, and the spend condition |

## Backup and recovery

A recovery phrase — a standard mnemonic encoding of the 256-bit master seed — plus a
small metadata record.

The phrase alone is not sufficient, and pretending otherwise is how people lose coins.
Deriving keys requires knowing *which* schemes, accounts and indices were used, because
scheme is an input to the derivation. So the backup is:

| Part | Contents | Sensitivity |
|---|---|---|
| Recovery phrase | The 256-bit master seed | **Total.** Anyone holding it holds the coins |
| Metadata | Schemes in use, account list, highest index reached per account, labels, birth height | Private but not catastrophic. Losing it costs a scan, not the coins |

Recovery without the metadata is still possible — scan a bounded gap of indices across
every registered scheme — which is the property that makes the phrase a genuine
last-resort backup rather than half of one. The birth height is what keeps that scan
from being a full chain rescan.

**The backup is verified by restoring it.** Phase 5's criterion is a restore into a
fresh wallet that reproduces the same addresses and the same balance. A backup that has
never been restored is a file, not a backup, and the number of projects that have
discovered this in production is the reason it is an acceptance criterion rather than a
task.

At rest, key material is encrypted under a passphrase through a **memory-hard** key
derivation function — Argon2id being the intended choice, confirmed against what the
linked OpenSSL actually provides during Phase 5 rather than assumed here. Memory-hard
specifically: a fast KDF makes an offline attack against a stolen wallet file a
throughput problem, and GPUs are very good at throughput.

## Migration

When a post-quantum scheme activates, existing coins do not move themselves. Migration
is a spend from a Schnorr lock to an ML-DSA lock, and it is a user action.

What the wallet owes the user: a clear statement of which balances are on which scheme,
a one-action migration that batches sensibly rather than producing one transaction per
output, and honesty that migration costs a fee and takes block space.

What it must not do: move coins without authorisation, present migration as urgent when
[the assumptions](PQ_CRYPTO.md#stated-assumptions) say a quantum computer capable of
this does not yet exist, or imply that migrating makes anything "quantum proof".

**This is the assumption most likely to fail in the whole project.** Cryptographic
agility is worthless if nobody moves their coins, and that is a communication problem
rather than a cryptographic one. It is recorded as such in
[PQ_CRYPTO.md](PQ_CRYPTO.md#stated-assumptions), and the wallet is where it is either
solved or lost.

## Threats

Enumerated with defences in
[THREAT_MODEL.md](THREAT_MODEL.md#7-wallet). The short form: weak randomness, nonce
reuse, a backup that does not restore, address reuse, change sent somewhere
unrecoverable, and a confirmation screen that disagrees with what gets signed. None of
them threaten the network; all of them lose coins.

## Not done yet

No wallet target, no key handling, no address encoding, no coin selection, no signing.
`crypto` does not exist either, so there is nothing for a wallet to call.

Phase 5 is deliberately after [Phase 4](ROADMAP.md#phase-4--peer-to-peer-networking):
a wallet is only demonstrable against a working network, and "send" and "receive on
another node" is not a testable claim without two nodes that actually talk.



