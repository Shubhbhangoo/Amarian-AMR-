# Network protocol

## Status: design intent, Phase 4

**The message framing, handshake, peer manager and sync state machine are implemented. Full P2P sync (two nodes discovering each other and converging) is not yet complete.** 
socket, and no message. This document exists so that the design decisions are made
before Phase 1 fixes the block and transaction formats the wire protocol has to
carry — writing the network layer first, against a format that then changes, is
precisely the failure mode the [phase ordering](ROADMAP.md) exists to prevent.

Phase 4's acceptance criterion is that **two or more independent nodes discover each
other, synchronise, and converge on the same best chain, including after a deliberate
fork.** Two nodes that were never in disagreement have not demonstrated that they can
resolve one.

## The one thing the network layer must not be able to do

**Nothing a peer sends can change what a node considers valid.** The `net` layer
delivers bytes to a validator and manages who it talks to; it makes no validation
decision, and it never links `consensus` in the other direction. Enforced by the
build, not by review — see [ARCHITECTURE.md](ARCHITECTURE.md).

Three specific consequences, each of which is a rule rather than a preference:

- **Every claim in a message is a claim, never a fact.** A peer's advertised height,
  the difficulty in a header, the fee a transaction says it pays: all recomputed
  locally. The one thing a peer can be trusted about is bytes it has actually
  provided, and only after those bytes have been validated.
- **No consensus outcome may depend on message ordering, arrival timing, or which
  peer answered.** If it does, two honest nodes with different peers reach different
  conclusions, which is a chain split with an unusual trigger.
- **Peer agreement counts for nothing.** Identities are free. Every security decision
  is on accumulated work.

## Transport and framing

TCP, via standalone Asio (not Boost.Asio — the dependency is smaller and the API is
the same). One connection per peer, length-prefixed messages, no long-lived
per-message state.

Message header, **14 bytes**:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | Network identifier. Distinct per network, so a mainnet node and a testnet node cannot complete a handshake |
| 4 | 2 | `command` | Numeric message id |
| 6 | 4 | `length` | Payload bytes, checked against the maximum before any allocation |
| 10 | 4 | `checksum` | First four bytes of `SHA256d(payload)` |

**A numeric command id rather than a 12-byte ASCII string.** Bitcoin's padded command
field raises a canonicality question — whether `block` padded with nulls is
distinguishable from a differently padded encoding of the same word — and costs ten
bytes on every message. A `u16` has one encoding per command, and an unknown id is
ignored rather than parsed, which is what allows new message types to be added without
a protocol break.

The checksum is for corruption, not for authentication. A four-byte checksum stops
nothing an attacker intends; TCP's own checksum is weak enough that silent corruption
on a long-running connection is a real observed phenomenon, and a corrupted block that
fails validation looks exactly like a malicious peer unless something distinguishes
them.

`length` is bounded by a maximum message size — provisionally 4 MB, which must exceed
`MAX_BLOCK_WEIGHT` with room for framing — and is checked **before allocation**, never
after. A length prefix is an attacker-chosen number, and treating it as a size to
reserve is the oldest memory-exhaustion bug there is.

## Handshake

```
-> hello      { protocol_version, services, timestamp, chain_id,
                best_height, nonce, user_agent }
<- hello      { ... }
-> hello_ack  { }
<- hello_ack  { }
```

Nothing else is processed until both `hello_ack` messages have crossed. A peer that
sends anything before completing the handshake is disconnected, which removes a class
of state-machine confusion before it can exist.

| Field | Purpose | Trusted? |
|---|---|---|
| `protocol_version` | Feature availability | Yes, as a lower bound only — a node claiming a version must speak it |
| `services` | Whether the peer serves historical blocks, relays transactions | As a hint. A peer that fails to serve what it advertised is disconnected |
| `chain_id` | Second-line network separation, after `magic` | Yes — a mismatch is an immediate disconnect |
| `best_height` | Sync progress display, and whether to request headers | **No.** Never used in a validation decision |
| `nonce` | Self-connection detection: a node seeing its own nonce disconnects | Yes, for that purpose only |
| `user_agent` | Diagnostics | No. Bounded in length, never parsed for behaviour |

**Version negotiation has no security-relevant dimension.** There is a minimum
supported version, below which the connection is refused; there is no negotiable
authentication, no negotiable encryption, and no negotiable validation strictness.
Anything an attacker can negotiate downward is something an attacker will negotiate
downward.

## Synchronisation

**Headers first, and work before storage.**

1. `getheaders { locator, stop_hash }` → `headers { header[] }`, capped per message.
2. Each header is validated in sequence: it connects to a known header, its
   `target_bits` matches the node's own recomputation, its hash meets that target,
   and its timestamp satisfies the rules. A header that fails any of these ends the
   batch and scores the peer.
3. **Only once a header chain demonstrates more accumulated work than the current tip**
   are its blocks requested. This is the defence against a cheap-header flood: a
   header is 92 bytes, an attacker can produce millions, and a node that stores them
   before checking work has been made to spend disk on nothing.
4. Blocks are requested from multiple peers in parallel, in a moving window, with a
   per-request stall timeout. A peer that fails to deliver a block it was asked for is
   disconnected and the block re-requested elsewhere — otherwise one unresponsive peer
   can halt a sync indefinitely.

Reorganisation is a chain-selection outcome, not a network operation: the node
requests whatever chain has more work, validates it, and switches if it is valid.
There is no message that asks a peer to reorganise, and no peer can cause one except
by providing more valid work.

## Relay

| Message | Purpose |
|---|---|
| `inv` | Announce transaction or block ids |
| `getdata` | Request announced items |
| `tx`, `block` | The items themselves |
| `notfound` | An item is no longer available |
| `getheaders`, `headers` | Header synchronisation |
| `getaddr`, `addr` | Peer discovery |
| `ping`, `pong` | Liveness, with a timeout that disconnects |
| `reject` | **Deliberately absent.** See below |

**There is no `reject` message.** Bitcoin removed its equivalent for good reasons: it
is unreliable enough to be useless for diagnostics, it leaks the node's policy to
anyone probing, and a wallet that trusts it can be lied to by any peer. A node that
wants to know why its transaction was not accepted asks its own node over RPC, where
the answer is authoritative.

Transactions are announced with a randomised per-peer delay rather than immediately.
This weakens first-broadcast origin inference. It is a genuine improvement and
explicitly **not** an anonymity claim — a global observer still correlates.

Blocks are announced immediately, because propagation latency is what determines how
much work is wasted on stale chains, and that is a decentralisation property. Compact
block relay is a known and deliberate omission from Phase 4: it is a bandwidth
optimisation whose complexity is only justified once there is a real network to
measure, and [Phase 12](ROADMAP.md#phase-12--performance-and-decentralisation) is
where measurement decides it.

## Peer discovery and address management

Discovery, in order of preference: peers persisted from previous runs, then DNS seeds,
then a small hard-coded seed list as a last resort, then addresses gossiped by peers.
Manual peers configured by the operator override everything and are never evicted.

The address database is bucketed by **network group** — roughly the /16 of an IPv4
address, the /32 of an IPv6 one — rather than by individual address. This is the entire
defence against address poisoning: an attacker with a thousand addresses in one hosting
provider's range occupies one bucket, not a thousand entries, so filling a victim's
table requires genuine network diversity rather than merely renting IPs.

Two tables, following the design Bitcoin arrived at after being attacked:

| Table | Contents | Why separate |
|---|---|---|
| *tried* | Addresses this node has itself connected to successfully | Cannot be filled by gossip. An attacker cannot put an address here without the node having actually reached it |
| *new* | Addresses learned from peers, never yet connected to | Attacker-influenceable by construction, which is why it is bounded, bucketed, and randomly evicted |

Outbound connections are selected with a **diversity requirement across network
groups**, and never solely from peer-supplied addresses. This is the eclipse defence:
occupying all of a victim's connections requires controlling addresses in many
distinct groups, not many addresses. **Anchor connections** — a small number of
outbound peers persisted across restarts — close the remaining window, which is that a
restart is otherwise a fresh opportunity to be surrounded.

Inbound and outbound slots are counted separately, and inbound connections can never
displace outbound ones. A node that lets inbound peers crowd out its own chosen peers
has handed its view of the network to whoever connected first.

## Denial of service

Every limit below exists because the cost to the attacker and the cost to the victim
are asymmetric without it.

| Limit | Applies to |
|---|---|
| Maximum message size, checked before allocation | Every message |
| Maximum items per `inv`, `getdata`, `headers`, `addr` | Announcements and batches |
| Per-peer inflight request accounting | Block and transaction requests |
| Per-peer bandwidth and message-rate limits | All traffic |
| Bounded caches with random eviction | Seen-inventory, address tables, orphan pool |
| Stall timeout | Block and header requests |
| Ping timeout | Liveness |
| Inbound slot cap with eviction preferring useful peers | Connection exhaustion |

**Ban scoring rather than immediate banning.** A peer accumulates a score, and
disconnects at a threshold; only unambiguous protocol violations — an invalid header,
a block that fails proof of work, a malformed message — score heavily. A peer being
slow, or on a slightly older version, or relaying a transaction this node's policy
rejects, is not misbehaving. Aggressive banning is itself an attack surface: an
attacker who can induce honest nodes to ban each other partitions the network for free.

Bans are per-address and time-limited, never permanent, because addresses are reused
and a permanent ban list eventually bans honest operators.

## Privacy: what is and is not claimed

**Claimed:** randomised relay delays make first-broadcast origin inference harder than
immediate flooding; the address gossip design does not leak which peers a node is
connected to on request.

**Not claimed:** anonymity of any kind. A node's IP address is visible to every peer
it connects to. A global observer of network traffic can correlate transaction
broadcasts with origins. Transaction graph analysis is unaffected by anything in this
layer — it is a property of a public ledger.

Amarian is not an anonymity network and will not describe itself as one. Operators who
need network-level privacy should run over Tor, which is an operational choice this
project should support rather than a property it should claim.

## Open questions

Recorded as open rather than answered, because answering them now would be guessing.

**Transport encryption.** Blockchain data is public, so encryption is not
confidentiality — but an opportunistic authenticated transport of the kind BIP-324
specifies does two useful things: it stops a network observer trivially identifying
protocol traffic for filtering, and it stops a man-in-the-middle tampering with
messages between honest peers. Whether that is worth the complexity in generation 1 is
undecided. If it is adopted it must be **mandatory or absent, never negotiable**,
because a downgradeable transport gives an attacker exactly the choice it removes.

**DNS seeds.** Necessary for bootstrapping and a centralisation point. Multiple
independently operated seeds reduce it; nothing eliminates it, because a node's first
connection has to come from somewhere. Honest framing rather than a solved problem.

**Compact block relay and transaction reconciliation.** Both are bandwidth
optimisations that matter at scale and neither is justified before there is a network
to measure. Phase 12.

**IPv6 and onion address support.** Address encoding needs to accommodate them from the
start even if the connection logic arrives later, because retrofitting an address
format is a protocol break.

## Not done yet

No socket, no message, no peer manager, no address database. The `net` layer does not
exist as a CMake target. Asio is resolved as a build dependency and nothing includes
it.

The fuzzing consequence is worth stating in advance: Phase 4 grows the fuzzing surface
from the two `util` parsers that exist today ([fuzz/README.md](../fuzz/README.md)) to
the whole message layer, because every message is a parser and every parser is a place
an attacker chooses the bytes.


