# Roadmap

Fourteen phases, numbered 0 to 13. Each has one acceptance criterion that can be
checked by running something, not by reading a document. A phase is not done
because its code exists; it is done when its criterion passes.

The order is deliberate: nothing that depends on consensus rules is built before
the consensus rules, and nothing that claims a security property is built before
the property can be tested. Phases are not parallelised, because the failure mode
of parallelising them is a networking layer written against a block format that
then changes.

Current position and the day-to-day detail live in
[../DEVELOPMENT_STATUS.md](../DEVELOPMENT_STATUS.md). This file is the shape of
the whole project; that file is where it actually is.

## Status

| Phase | Subject | Acceptance criterion | State |
|---|---|---|---|
| 0 | Environment and architecture | Clean build, clean test command, a real executable, project documentation | **complete** |
| 1 | Minimal blockchain | Two local nodes independently validate the same chain | **complete** |
| 2 | Hard-cap monetary system | Invalid inflation attempts are rejected | not started — current phase |
| 3 | Mining and difficulty | A local miner produces valid blocks another node independently verifies | not started |
| 4 | Peer-to-peer networking | Two or more independent nodes discover each other, synchronise, and converge | not started |
| 5 | Wallet | Create an address, acquire coins, send, receive on another node | not started |
| 6 | Post-quantum integration | Valid post-quantum transactions work end to end; invalid ones are rejected | not started |
| 7 | Hybrid ownership | Decided on benchmark evidence, then implemented or explicitly deferred | not started |
| 8 | Cryptographic agility | A scheme can be added, activated and deprecated without a redesign | not started |
| 9 | Security engineering | Every consensus-critical attack class has explicit regression coverage | not started |
| 10 | Regtest and testnet | Independent network parameters; multi-node tooling brings up a network | not started |
| 11 | Real-world infrastructure | CLI node, CLI wallet, RPC, explorer API, docs, release packaging | not started |
| 12 | Performance and decentralisation | Measured, with the measurement method published | not started |
| 13 | Mainnet readiness | Deliberately unspecified — see the end of this file | not started |

## Phase 0 — Environment and architecture

**Criterion:** clean build, clean test command, a basic executable, basic project
documentation. **Met.**

The environment was inspected by running things rather than by reading version
strings: a real libsecp256k1 Schnorr signature round trip, a real ML-DSA-44
keygen checked against the size FIPS 204 specifies, and a measured comparison of
compiling on ext4 versus the Windows drive before deciding where to build.

Also landed here: the layering contract, `-Werror` with the integer-conversion
warnings that turn a silent narrowing in a fee calculation into a build failure,
hardening flags that stay on in Release, sanitizer presets that fail rather than
warn, the `util` layer, and fuzz and benchmark harnesses for it.

## Phase 1 — Minimal blockchain

**Criterion:** two local nodes independently validate the same chain.

"Independently" is the whole point. Two processes sharing a validation cache, or
one trusting the other's answer, does not demonstrate anything. Each node parses
the same bytes and reaches the same conclusion on its own.

Needs: canonical serialisation; block and transaction primitives; the Merkle
root; the compact target encoding; SHA-256d proof-of-work checking; a genesis
block; the UTXO set; and persistence, so that a node restarted from disk agrees
with one that has been running.

**Met on 2026-09-05**, and demonstrated rather than asserted:
[scripts/phase1_acceptance.sh](../scripts/phase1_acceptance.sh) mines five regtest blocks on
one node, hands the raw blocks to a second node with a separate data directory that has never
seen the first, and asserts that the second refuses none of them and reaches the identical tip
hash and identical accumulated work — then that both nodes report that tip again after a
restart, and that a testnet node refuses both the file and the data directory. The transport is
a flat file rather than a socket precisely because it carries no work and no authority, so the
agreement can only have come from the second node's own rules. Everything above went in: the
codec, the primitives, the validation rules, the unspent output set, the header tree with
accumulated work, activation, the RocksDB chainstate, and block assembly with a bounded nonce
search.

## Phase 2 — Hard-cap monetary system

**Criterion:** invalid inflation attempts are rejected.

The supply schedule is already designed and analysed in
[ECONOMICS.md](ECONOMICS.md). This phase is where it becomes consensus-enforced,
and where the attempts to break it are written as tests: a coinbase claiming one
facet more than the schedule allows; a coinbase claiming fees that do not exist;
a transaction whose outputs exceed its inputs; overflow arithmetic engineered so
that a sum wraps into a small positive number; a duplicate coinbase; and
spending a coinbase before it matures.

Each of those is a test that must fail to validate, not a paragraph asserting
that it would.

## Phase 3 — Mining and difficulty

**Criterion:** a local miner produces valid blocks that another node
independently verifies.

Needs: block templates, coinbase construction, nonce and extranonce handling,
difficulty retargeting, and enough mining RPC for a CPU miner to be driven from
outside the node. CPU mining is for development; it is not a claim about how the
network would be secured.

Difficulty is where a new chain with little hashrate is most fragile. A
retarget that reacts too slowly lets a hashrate spike mine a long stretch of
easy blocks and then leave; one that reacts too quickly oscillates. ASERT is the
current intent, and the choice gets simulation evidence in this phase rather
than an appeal to what other chains do.

## Phase 4 — Peer-to-peer networking

**Criterion:** two or more independent nodes discover each other, synchronise,
and converge on the same best chain.

Needs: the wire protocol, handshake and version negotiation, headers-first
synchronisation, block and transaction relay, peer management, ban scoring, and
the initial-block-download path. Convergence must be demonstrated after a
deliberate fork, not only from a cold start — two nodes that were never in
disagreement have not demonstrated that they can resolve one.

This is the phase where untrusted input starts arriving from the network, so it
is also where the fuzzing surface grows from two parsers to the whole message
layer.

## Phase 5 — Wallet

**Criterion:** create an address, acquire coins, send them, and receive them on
another node.

Needs: key generation from a seed, address encoding, UTXO selection, fee
estimation, transaction construction and signing, and a backup format that can
actually restore a wallet. The backup requirement is tested by restoring, not by
writing a file and assuming.

Wallet code lives above the node in the layering and never inside consensus. A
wallet bug should be able to lose the owner's coins; it should not be able to
change what the network considers valid.

## Phase 6 — Post-quantum integration

**Criterion:** valid post-quantum transactions work end to end; invalid ones are
rejected.

Measured signature sizes and per-operation costs are already recorded in
[PQ_CRYPTO.md](PQ_CRYPTO.md). What this phase adds is the consensus-level
consequence of those numbers: how a 2420-byte signature is accounted for in
block weight, what it does to the UTXO set, and what a node's bandwidth looks
like when most transactions carry one.

The rejection half of the criterion is the harder half. A signature that
verifies is easy to demonstrate; a malleable encoding accepted where it should
be rejected is the bug that matters.

## Phase 7 — Hybrid ownership

**Criterion:** the decision is made on evidence, and then either implemented or
explicitly deferred with the reasoning recorded.

Requiring both a classical and a post-quantum signature to spend is not
automatically better. It protects against one scheme being broken, at the cost of
larger transactions, more wallet state, and a second way for a spend to become
impossible. Whether that trade is worth making in generation 1 is genuinely
undecided; see [PQ_CRYPTO.md](PQ_CRYPTO.md) for what has to be measured first.

"Deferred, for these reasons" is an acceptable outcome. "We added it because it
sounds stronger" is not.

## Phase 8 — Cryptographic agility

**Criterion:** a signature scheme can be added, activated and deprecated without
redesigning anything.

The mechanism — versioned algorithm identifiers inside a generic spend condition
— is in the design from the first block precisely so that this phase is an
exercise of existing structure rather than a retrofit. What it must actually
settle is the process: activation rules, deprecation, migration windows, what
happens to UTXOs locked to a scheme being retired, and what an emergency
migration looks like if a scheme breaks suddenly.

It must also make a malicious migration hard. A mechanism flexible enough to
swap out a broken signature scheme is flexible enough to be abused, and that
tension is the real content of this phase.

## Phase 9 — Security engineering

**Criterion:** every consensus-critical attack class has explicit regression
coverage.

The attack classes are enumerated in [THREAT_MODEL.md](THREAT_MODEL.md), and this
phase is where each one acquires a test that fails if the defence is removed. A
test that passes both with and without the code it is supposed to be testing is
not coverage.

Fuzzing moves from the two `util` parsers to consensus deserialisation, script
and spend-condition evaluation, and the P2P message layer. Reproducible crashes
get minimised and promoted into `tests/vectors/` as named regressions rather than
left in a disposable corpus directory.

## Phase 10 — Regtest and testnet

**Criterion:** a multi-node network comes up from tooling, on parameters
independent of mainnet.

Independent parameters means genesis, network magic, port, address prefix and
difficulty rules all differ, so a testnet node and a mainnet node cannot
accidentally talk to each other or replay each other's transactions. Regtest
additionally needs instant block generation on demand, because that is what makes
consensus tests cheap enough to run on every commit.

## Phase 11 — Real-world infrastructure

**Criterion:** the CLI node, CLI wallet, RPC interface, explorer API,
documentation and release packaging all exist and work.

This is the phase that decides whether Amarian is a program someone can actually
run. It is deliberately after the consensus and security work, because packaging
a system whose rules are still moving produces releases that have to be recalled.

## Phase 12 — Performance and decentralisation

**Criterion:** both are measured, with the method published alongside the
numbers.

Profile first. The one benchmark finding so far — a 21× compiler disagreement on
checked arithmetic, diagnosed down to the instruction level and then deliberately
not acted on — is the model: measure, understand the cause, and only then decide
whether it is worth changing anything.

The rule that governs this phase: **consensus and security are never weakened for
performance.** If the only way to make something faster is to check less, it does
not get faster.

Decentralisation is measured, not asserted. Initial block download time, storage
growth, bandwidth, and the cost of running a full validating node on ordinary
hardware are the numbers that determine whether ordinary people can participate,
and they are the numbers that will be published.

## Phase 13 — Mainnet readiness

There is no acceptance criterion here yet, and inventing one now would be the
most dangerous thing in this document.

A launch checklist written at Phase 0 is a checklist written before knowing what
the risks are. What can be committed to now is the constraint: **the code
compiling is not a reason to launch.** A mainnet launch is irreversible in a way
nothing else in this roadmap is — a consensus bug found afterwards cannot be
fixed by editing a file, because by then other people's property depends on the
buggy behaviour.

What this phase will need, at minimum, is a testnet that has survived real
adversarial use, a second independent review of the consensus rules, and monetary
parameters that have not changed in a long time. The criterion gets written when
Phases 9 through 12 have produced the evidence to write it from.

## Rules that apply to every phase

- **Nothing is claimed until it is demonstrated.** No performance number that was
  not produced by a real run on stated hardware. No security property without the
  assumptions it rests on. No "decentralised" before independent nodes exist.
- **Consensus code stays small, deterministic, and heavily tested.** Every rule
  is a pure function of data committed to by the chain — never of wall-clock
  time, locale, uninitialised memory, hash-map iteration order, or floating point.
- **No monetary value is ever a floating-point number.** Integer base units only,
  with checked arithmetic on every attacker-influenced quantity.
- **No hand-rolled cryptographic primitives.** Ever. Mature implementations
  behind interfaces narrow enough to replace them.
- **Consensus never links storage, networking, wallet or RPC.** Enforced by the
  build, not by convention.
- Each phase ends with its tests, its adversarial cases, its documentation
  updated to match what was actually built, and a focused commit.





