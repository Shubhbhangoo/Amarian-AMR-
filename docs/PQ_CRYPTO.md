# Post-quantum cryptography

## What this project claims, and what it does not

**Not claimed:** that Amarian is quantum-proof, quantum-safe, or post-quantum
secure. No chain can honestly claim that today, and a project that does is either
misusing the words or has not thought about it.

**Claimed:** that Amarian is designed so that adding, activating and retiring a
signature scheme is a protocol upgrade rather than a redesign, and that the
post-quantum schemes it intends to use are standardised, implemented in a mature
library, and measured — not chosen from a paper.

Everything below holds only under the assumptions listed in
[Stated assumptions](#stated-assumptions). If one of those assumptions is false,
the conclusion that depends on it is false, and a report that one of them is false
is a security report.

## The threat, stated precisely

Two quantum algorithms matter, and they matter very differently.

**Shor's algorithm breaks elliptic-curve signatures outright.** Given a public
key, a sufficiently large quantum computer recovers the private key. This is not a
speedup that can be answered with larger parameters — 512-bit ECC would not help.
Every signature scheme Amarian would otherwise use for spending, including BIP-340
Schnorr over secp256k1, is in this category.

**Grover's algorithm weakens hash functions by roughly half their bits.** SHA-256
retains on the order of 128 bits of preimage resistance, which is fine. This is
why proof of work and the hash-based structures — the Merkle tree, block ids,
transaction ids — are not the urgent problem. Mining is a hash-inversion search,
and a quadratic speedup on a difficulty-adjusted search is a hashrate change, not
a break.

So the exposure is specifically **spend authorisation**, and specifically **public
keys that an adversary can see**.

### Why exposed public keys are the pivot

A public key that has never been published cannot be attacked by Shor's algorithm,
because there is nothing to run it on. This gives an ordering to the risk:

| Situation | Exposure |
|---|---|
| Coins locked to a *hash* of a public key, never spent | Key not on chain. Attacker must break the hash first, and Grover does not do that. |
| Coins spent once from an address, remainder left at the same address | **Key published.** Anything still there is attackable from the moment the spend confirms. |
| Coins locked directly to a public key | **Key published from the start.** |
| A spend in flight, sitting in the mempool | Key published, and the race is between the attacker and the next block. |

Three consequences follow, and they are design constraints rather than advice:

1. **Locks commit to a hash, never to a key.** The key appears on chain only when
   the coins move.
2. **Address reuse is a security problem, not a privacy preference.** The wallet
   must make reuse the awkward path, not merely discourage it in documentation.
3. **The mempool race is the residual risk that hashing does not fix.** Between
   broadcasting a spend and its confirmation, the key is public and the coins are
   not yet moved. Nothing hides that window; only a post-quantum signature removes
   the exposure.

Long-term cold storage is where this bites hardest. Coins locked to a hash today
are safe from Shor's algorithm indefinitely — until the owner tries to spend them,
at which point they must reveal a classical key. If large quantum computers exist
by then, the coins were never really safe; they were only safe while nobody
touched them. That is the argument for having a post-quantum scheme available from
the first block rather than adding one later, and it is the actual reason this
matters for a chain designed for multi-decade ownership.

## What is actually available

Verified by running it, on OpenSSL 3.5.5 (27 Jan 2026) as packaged for Ubuntu
26.04:

| Scheme | Standard | Availability |
|---|---|---|
| ML-DSA-44, ML-DSA-65, ML-DSA-87 | FIPS 204 | **Default provider.** Lattice-based (Dilithium). |
| SLH-DSA-SHA2-128s | FIPS 205 | **Default provider.** Hash-based (SPHINCS+). |

Both are in the *default* provider, so no third-party provider is needed —
`liboqs` and `oqsprovider` are not a dependency of this project. That matters more
than convenience: it means the post-quantum code path goes through the same
library, the same release process and the same audit surface as the rest of the
cryptography, instead of a separately maintained plug-in.

Having two families with different mathematical foundations is deliberate. If
lattice assumptions turn out to be weaker than believed, a hash-based scheme is
still standing, and vice versa. That is the whole point of the
[cryptographic agility](#design-consequences) machinery.

## Measured sizes

Exact, not estimated. Public and private key sizes are DER as OpenSSL emits them;
the raw values are the DER size minus a 22-byte `SubjectPublicKeyInfo` wrapper for
ML-DSA and 18 bytes for SLH-DSA, and they match FIPS 204 and FIPS 205 exactly.
Signatures are raw.

| Scheme | Public key (raw) | Public key (DER) | Private key (DER) | Signature |
|---|---|---|---|---|
| BIP-340 Schnorr, x-only | 32 | — | — | 64 |
| ML-DSA-44 | 1 312 | 1 334 | 2 626 | 2 420 |
| ML-DSA-65 | 1 952 | 1 974 | 4 098 | 3 309 |
| ML-DSA-87 | 2 592 | 2 614 | 4 962 | 4 627 |
| SLH-DSA-SHA2-128s | 32 | 50 | 84 | 7 856 |

SLH-DSA's 32-byte public key is worth noticing. Its signature is the largest of
the set, but its key is the same size as an x-only secp256k1 key, so it costs
nothing extra in a lock or in the UTXO set — the whole cost lands on the spend.

## Measured performance

`openssl speed -seconds 2`, 2026-09-04, 12 × 2.5 GHz x86-64, WSL2 on Windows 11.

| Scheme | Keygen | Sign | Verify | Verify/s |
|---|---|---|---|---|
| ML-DSA-44 | 132 µs | 610 µs | **136 µs** | 7 362 |
| ML-DSA-65 | 219 µs | 1 066 µs | 209 µs | 4 775 |
| ML-DSA-87 | 351 µs | 1 325 µs | 313 µs | 3 190 |
| SLH-DSA-SHA2-128s | 50 ms | **424 ms** | 476 µs | 2 101 |
| ECDSA P-256 (OpenSSL) | — | 32 µs | 92 µs | 10 877 |
| Ed25519 (OpenSSL) | — | 46 µs | 129 µs | 7 738 |

Caveats, because a benchmark without them is decoration. This is a laptop under
WSL2 with frequency scaling and no core isolation, so treat the numbers as
indicative to within tens of percent, not as a characterisation of the algorithms.
The classical rows are **OpenSSL's own** P-256 and Ed25519, included because they
were measured on the same machine in the same run. They are *not* libsecp256k1,
which is considerably faster at secp256k1 than OpenSSL is at P-256; a BIP-340
Schnorr number will be measured with a proper harness when the `crypto` layer
exists, and until then no ratio against Schnorr should be quoted from this table.

## What the numbers imply

The single most useful thing these measurements say is that **the received wisdom
is wrong about where the cost is.**

ML-DSA-44 verification takes 136 µs, against 129 µs for Ed25519 and 92 µs for
P-256 on the same machine. Verification — the operation every node performs for
every input of every transaction — is *within the same order of magnitude as
classical signatures*. It is not the bottleneck.

Put a number on it. Filling a block entirely with single-input transactions and
verifying every signature:

| Scheme | Inputs per block | Verification CPU per block |
|---|---|---|
| ML-DSA-44 | 467 | 0.064 s |
| ML-DSA-65 | 344 | 0.072 s |
| ML-DSA-87 | 257 | 0.080 s |
| SLH-DSA-SHA2-128s | 237 | 0.113 s |

Against a 300-second block interval, that is a fraction of a percent of the
available time. Signature verification CPU is simply not a constraint at these
parameters.

**Size is the constraint.** Authorisation data per input, signature plus public
key:

| Scheme | Signature + key | Relative to Schnorr |
|---|---|---|
| BIP-340 Schnorr | 96 B | 1.0× |
| ML-DSA-44 | 3 732 B | 38.9× |
| ML-DSA-65 | 5 261 B | 54.8× |
| ML-DSA-87 | 7 219 B | 75.2× |
| SLH-DSA-SHA2-128s | 7 888 B | 82.2× |

And in block capacity — an estimate, because the transaction format is not final;
the assumption is one input, two outputs, a 136-byte base section, authorisation
data in the witness, `weight = base × 4 + witness`, and a 2 000 000 weight cap:

| Scheme | Weight per tx | Transactions per block |
|---|---|---|
| BIP-340 Schnorr | 643 | 3 110 |
| ML-DSA-44 | 4 279 | 467 |
| ML-DSA-65 | 5 808 | 344 |
| ML-DSA-87 | 7 766 | 257 |
| SLH-DSA-SHA2-128s | 8 435 | 237 |

A 6.7× reduction in transactions per block for ML-DSA-44. That is the real price of
post-quantum authorisation, and it is paid in bandwidth, block space and archival
storage rather than in CPU.

Two design consequences follow directly, and both are decisions this evidence
makes rather than assumptions imported from elsewhere:

**Post-quantum signatures belong in the witness section.** The weight rule
discounts witness bytes 4:1 against base bytes, which is what keeps a 3.7 KB
authorisation from consuming 15 KB of weight. Consensus must commit to those bytes
in the signature hash regardless — the discount is about block space accounting,
not about what is signed.

**SLH-DSA is a cold-storage scheme, not a general one.** 424 ms to produce one
signature means a ten-input transaction takes over four seconds to sign, and a
wallet sweeping fifty inputs takes twenty. For a vault spent once a decade that is
irrelevant. For a hot wallet it is unacceptable. Offering it as *an* option rather
than *the* option is the right shape, and that is only possible because algorithm
identifiers are in the design from the first block.

## Design consequences

None of this is implemented yet — it is Phase 6 and Phase 8 work. What is decided
now, and shapes Phase 1, is the structure that makes it possible without a
redesign.

**Every public key carries an explicit scheme identifier.** A key is a
`{scheme: u16, bytes}` pair, not a bare byte string whose meaning is inferred from
its length. Length-based inference is how a codebase ends up unable to add a scheme
that happens to collide with an existing size, and it is how a verifier ends up
guessing.

**Spend conditions are generic and versioned.** A lock commits to a
`{condition_version, threshold, keys[]}` structure rather than to a script. There
is no script virtual machine, deliberately: a threshold over a list of
scheme-tagged keys covers single-key, multisignature, and classical-plus-post-
quantum hybrid authorisation with one evaluator small enough to audit completely.
A script VM would cover more, at the cost of an evaluation surface where the
interesting bugs live.

**Locks commit to a hash of the spend condition, never to the keys.** This is what
keeps unspent coins out of Shor's reach, and it is also what makes a 1 312-byte
ML-DSA key cost the same as a 32-byte Schnorr key in the UTXO set. The key is
revealed at spend time and never before.

**Unknown lock versions are anyone-can-spend for consensus and non-standard for
relay.** That combination is what allows a new scheme to be introduced by soft
fork: old nodes accept blocks containing the new lock without understanding it,
while relay policy stops anyone actually creating one before activation.

## The hybrid question is open

Requiring *both* a classical and a post-quantum signature to spend is not
automatically better, and this project will not adopt it on the grounds that it
sounds stronger.

The case for it: a spend remains authorised only if both schemes hold, so a break
in either one alone does not lose coins. Given that lattice assumptions are
younger than elliptic-curve ones, that is a real hedge.

The case against, with the numbers above attached: authorisation data becomes
3 828 B per input instead of 3 732 B — the marginal cost over ML-DSA-44 alone is
small — but every wallet must manage two key types, every backup must preserve
both, and there are now **two** ways for a spend to become permanently impossible
instead of one. A hybrid scheme is strictly *less* available than either component,
and for long-term self-custody, availability failures lose real coins today while
cryptographic breaks are hypothetical.

Phase 7 decides this, on evidence. "Deferred, for these reasons" is an acceptable
answer; the agility machinery exists precisely so that deferring is not the same as
foreclosing.

## Stated assumptions

The claims in this document hold if and only if:

1. **SHA-256 retains ~128-bit preimage resistance against a quantum adversary.**
   Grover's algorithm gives a quadratic speedup and no better attack exists. If
   this fails, hash-committed locks stop protecting unspent coins and proof of work
   needs revisiting.
2. **ML-DSA is secure at the levels FIPS 204 claims**, and SLH-DSA at the levels
   FIPS 205 claims, against both classical and quantum adversaries.
3. **OpenSSL's implementations are correct**, are not side-channel-vulnerable in
   ways that matter for a signing wallet, and continue to be maintained. Amarian
   implements no primitive itself, so this assumption is load-bearing and cannot be
   engineered away — only reduced by keeping the interface narrow enough to swap
   implementations.
4. **A quantum computer capable of running Shor's algorithm on secp256k1 does not
   exist yet.** Amarian makes no prediction about when one will. The design goal is
   that the answer does not have to be known in advance.
5. **Users can be migrated before the threat arrives.** Cryptographic agility is
   worthless if nobody moves their coins. This is a product and communication
   problem, not a cryptographic one, and it is the most likely of these assumptions
   to fail.
6. **Post-quantum readiness is about signatures, not confidentiality.** Amarian has
   no encrypted-payload feature, so there is no store-now-decrypt-later exposure of
   the kind that affects TLS traffic. If such a feature is ever added, this
   assumption stops holding and a key-encapsulation mechanism enters the design.

## What is built, and what is not

To be unambiguous about the gap between this document and the code: the `crypto` layer
exists, and so does everything the roadmap put in Phase 1. `crypto/signature.hpp`
defines the scheme registry — identifier, key length, signature length, class, backend
name, availability probe — with all three schemes in the table above registered:
BIP-340 Schnorr over libsecp256k1, and ML-DSA-44 and SLH-DSA-SHA2-128s over OpenSSL's
default provider. `crypto::Verify` dispatches through it and returns a four-way answer
(`Valid`, `Invalid`, `Malformed`, `UnknownScheme`, plus `Reserved` for identifier 0)
rather than a boolean, because "I cannot check this" and "this is a forgery" must not be
the same value to a validator. Consensus calls it from `CheckSpendAuthorisation`, over
the `Amarian/SigHash` message described in
[AMARIAN_PROTOCOL.md](AMARIAN_PROTOCOL.md#signature-hash), and it is exercised in tests
against real signatures rather than fixtures.

`amariand` refuses to start if any registered scheme is unavailable from this build's
backend. That is a startup gate rather than a warning for a specific reason: an
unavailable scheme would be reported as one the node does not know, and by the
soft-fork rule an unknown scheme counts as satisfied — so the node would accept every
spend under it without checking, while believing it was verifying signatures.

ML-DSA-44 and SLH-DSA-SHA2-128s are therefore *verifiable* today, which is not the same
as the Phase 6 goal: no wallet creates outputs under them, no migration path exists, and
no address format encodes them.

What remains: a decision on hybrid authorisation (Phase 7), activation and deprecation
mechanics (Phase 8), wallet and address support for the post-quantum schemes (Phase 6),
and a BIP-340 benchmark on the same machine so the classical baseline in this document
can be replaced with the scheme actually used.



