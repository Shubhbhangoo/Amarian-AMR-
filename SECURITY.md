# Security Policy

## Status: pre-release, no mainnet, no funds at risk

Amarian is in Phase 0 of 13. There is no chain, no network, no wallet and no
coin. Nothing here is holding anyone's money, and it is not yet possible to lose
funds by running this software — because it is not yet possible to hold any.

That matters for expectations: this document describes how reports will be
handled, not a battle-tested process with a track record. There is no bug
bounty, and no promise of a payout.

## Reporting a vulnerability

Report privately. Open a GitHub **security advisory** on the repository
(Security → Advisories → Report a vulnerability), which keeps the report
non-public until a fix exists.

Do not open a public issue for anything that could let someone create coins out
of nothing, spend coins they do not own, split the network onto two chains, or
crash a node remotely.

A useful report contains:

- the commit hash (`amariand --version` prints it, and marks a build from a
  modified working tree as such),
- the preset or compiler flags, from `amariand --build-info`,
- what you expected and what happened,
- a reproducer — for a parser bug the raw input bytes are worth more than a
  description of them, and a libFuzzer artefact is ideal.

You will get an acknowledgement. Until this project has a real user base, that
is the only commitment worth making; a fixed disclosure-window SLA from a
single-maintainer pre-release project would be a number invented to look
professional.

## Severity, as this project ranks it

Consensus comes first, because a consensus bug is the only class that can
destroy the property the project exists to provide.

| Class | Examples |
|---|---|
| **Critical** | Inflation past the supply cap; spending without authorisation; two honest nodes permanently disagreeing on the best chain; a consensus rule that depends on wall-clock time, uninitialised memory, iteration order, locale, or anything else not committed to by the block data |
| **High** | Remote crash or unbounded memory growth from peer-supplied data; a chain split reachable by a peer; loss of wallet keys or of a backup's ability to recover them |
| **Medium** | Denial of service requiring significant resources; mempool or relay policy abuse that does not affect consensus; privacy leaks in the wallet or P2P layer |
| **Low** | Crashes reachable only by the node's own operator through the CLI or config; incorrect RPC output that does not affect validation |

A consensus divergence is Critical even when it needs an unlikely trigger, and
even when both outcomes look "reasonable". Determinism is the property; the
specific rule matters less than every node agreeing on it.

## Cryptography

Amarian does not implement cryptographic primitives. Hashing, classical
signatures and post-quantum signatures come from OpenSSL and libsecp256k1, and
the project's job is to call them correctly.

So there are two distinct kinds of report, and both are welcome:

- **Amarian misuses a primitive** — nonce handling, a malleable signature
  encoding accepted where it should be rejected, a hash not domain-separated
  from another use of the same hash, a sighash that fails to commit to something
  it must commit to. These are ours.
- **A primitive itself is broken** — report upstream first. Amarian's response
  is to swap the implementation, which is exactly why the interfaces are narrow.

Claims about post-quantum security hold only under the assumptions written down
in [docs/PQ_CRYPTO.md](docs/PQ_CRYPTO.md). A report that a stated assumption is
false is a security report, and a valuable one.

## Deliberately out of scope

- Missing hardening flags that are already enabled and visible in
  `amariand --build-info`. Check first.
- "Proof of work is wasteful", "the supply cap is the wrong number", and other
  disagreements with design decisions. Those belong in an issue or a discussion;
  see [docs/DECISIONS.md](docs/DECISIONS.md) for the reasoning already recorded.
- Anything requiring the reporter to already control the operator's machine,
  data directory or private keys.
- Theoretical 51% attacks. Proof of work assumes an honest hashrate majority;
  that is a stated assumption in [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md),
  not a bug. A way to reorganise the chain *without* a hashrate majority is very
  much a bug.

## What this project promises about itself

Nothing is claimed until it is demonstrated. Specifically: Amarian does not
currently claim to be decentralised, does not claim to be post-quantum secure,
and does not quote a performance number that was not produced by a real run on
stated hardware. Where a measurement appears in this repository, the command and
the machine are recorded next to it.

If you find a claim in this repository that is not backed by evidence in this
repository, that is a defect worth reporting too.
