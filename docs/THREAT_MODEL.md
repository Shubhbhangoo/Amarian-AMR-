# Threat model

## What this document is, and what it is not

This is a threat model written at Phase 0, which means it is a **specification of
what must be defended**, not a report of what has been defended. Every defence
below is labelled with the phase that implements it and whether a test exists. At
the time of writing, the honest answer to "is this defended?" is *no* for almost
everything here, because there was no consensus code yet.

That is the point of writing it now. An enumeration of attack classes produced
*after* the code exists is an enumeration of the attacks the code happens to
stop. This one is meant to be the input to
[Phase 9](ROADMAP.md#phase-9--security-engineering), whose acceptance criterion is
that every class listed here has a regression test that fails if the defence is
removed.

**Updated 2026-09-05**, part way through Phase 1: twenty-five rows now read **yes**.
The supply class is the one that changed most in this revision, and it changed because
the unspent output set exists. The two rows that could not previously be closed — the
coinbase fee bound and the phantom fee claim — are closed by `ConnectBlock`, which sums
each transaction's fee from the coins it actually spent and hands the total to
`CheckCoinbaseAmount`; coinbase maturity is enforced from the height stored on each coin;
and two rows were *added*, for spending a coin that does not exist and for spending one
coin twice, because until there was a set to consult neither attack had a defence to
describe. The spend-authorisation class was the previous revision's change, and it remains
the class where every row reads **yes** or **partial**.

Nothing in the proof-of-work or network classes reads **yes**. The supply class is now
complete apart from the `sequence == height` rule, which is implemented and exercised only
indirectly. The context-free validation rules
([src/consensus/validation.cpp](../src/consensus/validation.cpp)) are marked
**implemented, untested** where they defend a row and nothing exercises them; that is
a weaker claim than **yes** deliberately, because a rule nothing exercises is a rule
nobody has checked. A row marked **yes** means a named test exists and is in the
suite; it does not yet mean the stronger Phase 9 property that the test fails when
the defence is removed. Where that stronger property has been checked by mutation,
the row says so.

For how to report something found in the code, see
[../SECURITY.md](../SECURITY.md). This document is about the design.

## What is being protected

Ranked, because ranking is what makes trade-offs decidable when two properties
conflict.

| # | Property | What breaking it costs |
|---|---|---|
| 1 | **Supply integrity** — no more than 83 999 999 932 170 000 facets ever exist, on the schedule in [ECONOMICS.md](ECONOMICS.md) | The one thing the project exists to provide. Unrecoverable: a chain that has silently over-issued cannot un-issue. |
| 2 | **Spend authorisation** — coins move only with the authorisation their lock demands | Theft. Also unrecoverable for the victim. |
| 3 | **Determinism and convergence** — every honest node with the same data reaches the same conclusion, and honest nodes converge on one best chain | A permanent split is two incompatible histories. Both halves believe they are correct, which is worse than either being wrong. |
| 4 | **Node availability** — an honest operator can run a validating node on ordinary hardware and stay in sync | Decentralisation is the ability to verify for yourself. If that becomes expensive, verification centralises and properties 1–3 stop being checkable by the people relying on them. |
| 5 | **Privacy** | Real harm, but a privacy failure does not destroy the monetary system. Deliberately last, and deliberately not claimed as a feature. |

Ordering matters in practice. If a change would improve throughput at the cost of
determinism, it does not happen. If a policy rule improves privacy but makes a
consensus rule ambiguous, it does not happen.

## The adversary

Rather than one imagined attacker, a ladder of capabilities. Each rung is assumed
to be freely available to whoever is attacking.

| Capability | Assumed available | Notes |
|---|---|---|
| Send arbitrary bytes to any node's P2P port | Yes | Not a hypothetical: this is the normal operating condition. |
| Run many nodes and many identities | Yes | Identities are free. Sybil resistance comes from work, never from counting peers. |
| Choose transaction contents, sizes, structure, ordering, and timing | Yes | Including contents chosen specifically to make validation expensive. |
| Craft any byte string as a block, transaction, or message | Yes | Including structurally valid data with adversarial values, which is the harder case than garbage. |
| Delay, drop, reorder, or duplicate messages between others | Yes, partially | Assumed for a targeted victim; not assumed globally and permanently. |
| Substantial but minority hashrate | Yes | Up to just under 50%. Selfish mining and short reorganisations are in scope. |
| Majority hashrate | **Assumed not available** | See [accepted risks](#accepted-risks-and-explicit-non-goals). This is proof of work's foundational assumption, not a bug in this implementation. |
| Quantum computer able to run Shor's algorithm on secp256k1 | **Assumed not available yet** | The design goal is that the arrival date does not need to be predicted. See [PQ_CRYPTO.md](PQ_CRYPTO.md). |
| Read or write the operator's disk, memory, or keys | **Out of scope** | An attacker already inside the machine has won by definition. |

The rung that deserves emphasis is "structurally valid data with adversarial
values". Malformed garbage is caught by a parser and is the easy case; a
well-formed transaction whose amounts are chosen so that a sum wraps is caught only
by a rule that was written on purpose.

## Where untrusted data enters

Four boundaries, and it is worth being explicit that the *first* one is the one
people forget:

1. **The P2P socket.** Blocks, transactions, headers, addresses, and every protocol
   message. Phase 4.
2. **The RPC interface.** Semi-trusted — an operator's own request — but reachable
   from a browser via cross-origin requests if it is bound carelessly, so it is
   treated as untrusted input with an authentication layer in front. Phase 11.
3. **The command line, config file and data directory.** Trusted in the sense that
   the operator supplied them, which is why a crash here is Low severity rather
   than High, but still parsed defensively. This exists today, and
   [fuzz/args_fuzz.cpp](../fuzz/args_fuzz.cpp) fuzzes it.
4. **Anything read back from disk.** A database file is not a trusted input: it may
   have been corrupted by a power failure, a failing disk, or a previous version of
   the software with a bug. Phase 1 onward.

The rule that follows is that **deserialisation is the security perimeter**. Every
parser is a place where an attacker chooses the bytes, so each one gets a fuzz
harness rather than a review. Three exist today ([fuzz/README.md](../fuzz/README.md)),
the widest being `fuzz_serialize` on the canonical codec, through which every byte
a node acts on passes; the number grows with each phase that adds a parser.

## Attack classes

Each table row is intended to become a named test. The **Phase** column is where
the defence is implemented; **Tested** is the current honest state.

### 1. Supply integrity

| Attack | Mechanism | Defence | Phase | Tested |
|---|---|---|---|---|
| Coinbase over-claim | Coinbase output exceeds the scheduled reward | Reward recomputed from height by every node; coinbase ≤ reward + fees actually paid | 1, 2 | **yes** — `ConnectBlock.ACoinbaseMayClaimTheRewardPlusTheFeesTheBlockActuallyPaid`, which rejects a coinbase claiming one facet above reward + fees and accepts the exact figure, so the boundary is checked rather than assumed. Both halves are now real: the reward is derived from height alone, and the fee total is summed by `ConnectBlock` from the coins each transaction actually spent |
| Phantom fee claim | Coinbase claims fees no transaction paid | Fees derived from the block's own transactions, never from a field in the block | 2 | **yes** — the same test. Structurally, no block field carries a fee total for a node to trust; the only value `CheckCoinbaseAmount` accepts is the one `ConnectBlock` accumulated from `TransactionFee` over spent coins, and a block whose transactions pay no fees cannot claim any |
| Outputs exceed inputs | A transaction mints value directly | Per-transaction sum check, on integers | 2 | **yes** — `Validation.ATransactionMayNotPayOutMoreThanItSpends` and `.TheFeeIsWhatIsSpentMinusWhatIsPaid`. `TransactionFee` rejects any transaction paying out more than it spends; *finding* the outputs it spends is the UTXO set's job and is separate from this rule |
| Overflow to a small positive | Amounts chosen so an addition wraps | Every amount bounded to `[0, MAX_MONEY]` *before* summing, plus `CheckedAdd`; `-fwrapv` so a missed check is defined behaviour rather than an optimiser licence | 1, 2 | **yes** on both sides — `Validation.AmountsOutsideTheMoneyRangeAreRejectedOnBothSides` and `TransactionPrimitive.OutputAmountIsRangeCheckedAtTheWholeTransactionLevel`. Both running totals are re-checked against the money range at every step, so a sum that leaves it is rejected at the addition that took it out rather than after wrapping |
| Negative amount | A signed amount below zero | Amounts are validated on deserialisation, not at point of use | 1, 2 | **yes** — `TransactionPrimitive.OutputAmountIsRangeCheckedAtTheWholeTransactionLevel`. No code path in the node can hold a `TxOutput` outside `[0, MAX_MONEY]`, and `CheckTransaction` re-checks so that a transaction built in memory gets the same verdict as one parsed off the wire |
| Duplicate coinbase | Two coinbase transactions in one block | Exactly one, at index 0 | 1 | implemented, untested — `CheckBlock` rejects a block whose first transaction is not a coinbase and one that contains a second |
| Premature coinbase spend | Spending a reward before maturity | 200-block maturity checked against the spending block's height | 2 | **yes** — `ConnectBlock.ACoinbaseOutputCannotBeSpentUntilItHasMatured`, which rejects the spend one block early and accepts it at exactly `H + coinbase_maturity`. The creation height and coinbase flag travel on the stored `Coin`, so the rule needs neither the creating block nor a second lookup |
| Spend of a coin that does not exist | An input naming an outpoint that was never created, or was created on an abandoned branch | Every input resolved against the UTXO set before any arithmetic; absence and prior spend are indistinguishable and both fatal | 1 | **yes** — `ConnectBlock.AnInputThatIsNotInTheSetIsRejectedAndTheSetIsUntouched`, which also pins the atomicity half: the coinbase's outputs were already staged when the spend failed, and none of them reached the node's set |
| Double spend of one coin | The same outpoint consumed twice, in one block or across two | Within a block, the coin is removed as it is found, so the second attempt resolves nothing — there is no state in which it is still present; across blocks, the first spend left the set without it | 1 | **yes** — `ConnectBlock.ATransactionMaySpendAnOutputCreatedEarlierInTheSameBlockButNotLater` for the ordering half and the removal-as-found mechanism, on top of `CheckBlock`'s context-free duplicate-outpoint scan. The two defences are independent: one is a scan of the block, the other is the set refusing to answer twice |
| Duplicate transaction id | Re-mining an existing txid to overwrite or resurrect a UTXO (BIP-30 / BIP-34 class) | Height committed in the header and required to equal the coinbase input's `sequence`, which is inside the txid preimage, so identical coinbases across heights are impossible by construction — *and* creating an outpoint that is already unspent is rejected regardless | 1 | **yes** — `ConnectBlock.CreatingAnOutpointThatIsAlreadyUnspentIsRejected`, and `CoinsCache.AddingOverALiveCoinFailsAndChangesNothing` at the container level. Amarian needs no activated rule and no exception height for this; the invariant is enforced anyway, because the construction argument depends on facts a future change could alter and the cost of being wrong is a live coin silently overwritten. `CoinsCache.ACoinRecreatedAtAnOutpointItAlreadyOccupiedIsAllowedOnceSpent` pins that the rule is about *live* coins. The `sequence == height` rule itself remains implemented, untested in `CheckBlock` |
| Tail emission added later | A "temporary" subsidy to fund security | Not a code defence. The cap is enforced per-block from height, so adding one is a hard fork that every node must accept — the social defence is that it is impossible to do quietly | — | n/a |

The last row is not padding. Most supply failures in practice are not exploits;
they are decisions. The mechanism that stops it is that the schedule is a pure
function of height with no parameters an operator can set.

### 2. Spend authorisation

| Attack | Mechanism | Defence | Phase | Tested |
|---|---|---|---|---|
| Forged signature | A signature that verifies without the key | Not Amarian's to defend — libsecp256k1 and OpenSSL. Amarian's job is calling them correctly | 1, 6 | **yes** for the calling half — `CryptoSignature.SchnorrVerifiesBip340Vector` against published vectors, `.MlDsa44RoundTrips`, `.SlhDsaSha2128sRoundTrips`, `.SchnorrRejectsAMutatedSignature`, and `Validation.ARealSignatureAuthorisesASingleKeySpend` end to end through consensus with a real signature. The primitives themselves remain the libraries' claim, not Amarian's |
| Signature malleability | A second encoding of the same signature, giving a second valid txid | BIP-340 signatures are fixed 64 bytes with no encoding freedom; ML-DSA signatures are fixed-length; any variable-length encoding must be checked canonical on parse, not on use | 1, 6 | **yes** for fixed-length enforcement — `Validation.AKeyOrSignatureOfTheWrongLengthIsRejectedStructurally` and `CryptoSignature.WrongLengthsAreRejectedBeforeAnyImplementationIsCalled`: a signature under a scheme this build knows must be exactly that scheme's length, checked in the context-free pass. Witness *ordering* malleability is separately closed by the ordered forward match — `Validation.ATwoOfTwoNeedsBothSignaturesInKeyOrder`. No variable-length signature scheme is registered yet, so the canonical-parse half has nothing to check |
| Signature reuse across contexts | A signature valid for one transaction accepted for another | Sighash commits to inputs, outputs, the spent amount, the spend condition, and a network-specific `chain_id` | 1 | **yes** — `PrimitivesSigHash.CommitsToEveryOutpoint`, `.CommitsToEverySequence`, `.CommitsToEveryOutputAmountAndLock`, `.CommitsToTheInputAndOutputCounts`, `.CommitsToTheSpentAmount`, `.CommitsToTheRevealedCondition`, `.CommitsToTheTransactionVersionAndLocktime`, and `Validation.ASignatureOverADifferentSpentAmountDoesNotAuthorise` showing the rejection through consensus rather than only a differing hash |
| Cross-chain replay | A mainnet transaction replayed on testnet or vice versa | `chain_id` in the sighash preimage, distinct per network | 1 | **yes** — `PrimitivesSigHash.CommitsToTheChainId`, and `ConsensusParams` asserts the three networks' `chain_id` values differ |
| Cross-input replay | A signature for input 0 accepted for input 1 | The input index is in the preimage | 1 | **yes** — `PrimitivesSigHash.CommitsToTheInputIndex` |
| Wrong-key substitution | Spending with a key that is not the one committed to | Lock commits to a hash of the whole `SpendCondition`; the revealed condition is hashed and compared before any signature is checked | 1 | **yes** — `Validation.TheRevealedConditionMustBeTheOneTheOutputCommittedTo`, in which the attacker reveals a condition they control together with a signature that is perfectly valid for it, and `.ALockProgramOfTheWrongLengthMatchesNothing`. Also `PrimitivesSigHash.TheConditionCommitmentIsWhatAVersionOneLockHolds` |
| Threshold bypass | Satisfying a 2-of-3 with one signature counted twice | Each satisfied key counted at most once by the ordered forward match; duplicate keys in a condition rejected at construction | 1 | **yes** — `Validation.AThresholdIsNotMetByOfferingOneKeysSignatureTwice` for the counting half, `.ATwoOfTwoNeedsBothSignaturesInKeyOrder` for the ordering, and `.OneOfTwoIsSatisfiedByEitherKeyAlone` so the rule is not merely restrictive. The duplicate-key half remains implemented, untested: `CheckSpendCondition` requires the key list to be strictly ascending, which rejects duplicates and pins the ordering at the same time |
| Hash collision on a lock | Two spend conditions with the same commitment | 256-bit commitment; also why the commitment is domain-separated from every other hash use | 1 | no |
| Domain confusion | A hash from one context accepted in another | Tagged hashing throughout, with the tag part of the preimage | 1 | **yes** — `CryptoHash.TagsDomainSeparateTheSameMessage`, and the BIP-340 construction itself is checked against published vectors |
| Quantum key recovery from an exposed key | Shor's algorithm on a published public key | Locks commit to a hash, never a key; post-quantum schemes available from the first block. The mempool window is residual and acknowledged | 1, 6 | partial — both post-quantum schemes are registered and verifiable in consensus today, tested by `CryptoSignature.MlDsa44RoundTrips`, `.SlhDsaSha2128sRoundTrips` and `.ThePostQuantumSchemesRestOnUnrelatedAssumptions`; `amariand` refuses to start if its backend cannot supply one. What is missing is not verification but *use*: no wallet or address format produces such an output yet, which is Phase 6 |
| Unknown scheme accepted unchecked | Locking coins to a scheme identifier no node implements, so that every node treats the signature as satisfied | Deliberate and not a defect: an unknown scheme counts as satisfied, because a node cannot check a rule it does not contain, and rejecting would fork it off the chain the instant the scheme was deployed. What bounds it is that a scheme only becomes usable when a hashpower majority enforces it, and that identifier 0 is reserved and always rejected so an all-zero field is never a valid scheme | 1, 8 | **yes**, in the sense that the accepting behaviour is the specified behaviour and is pinned by `Validation.AnUnknownKeySchemeCountsAsSatisfied` and `.AnUnknownLockVersionStaysSpendableWithoutASignatureCheck`, and the reserved identifier by `CryptoSignature.ReservedSchemeIsNeverVerifiable`. The residual risk is a user locking coins to an identifier nothing will ever enforce — a wallet problem, addressed by Phase 8's activation mechanics, not a consensus one |

Three further rows belong to this class but are about a transaction's *identity*
rather than its authorisation, and they are the part of it Phase 1 has settled.
Identity matters here because a signature authorises a specific transaction: if the
same authorised effect can be presented under a second id, everything that
referenced the first id breaks.

| Attack | Mechanism | Defence | Phase | Tested |
|---|---|---|---|---|
| Encoding malleability | A second byte encoding of the same transaction, giving it a second id | Non-minimal compact sizes are **rejected, not normalised**; `Reader::Finish()` requires exact consumption, so trailing bytes are a parse error rather than debris | 1 | **yes** — `CompactSize.RejectsNonMinimalEncodings`, `Reader.TrailingBytesFailTheParse`, `TransactionPrimitive.TrailingBytesAreRejectedByFinishNotByDeserialize`, plus `fuzz_serialize` asserting that every accepted compact size re-encodes to itself |
| Witness malleability changing the txid | Altering the witness section in relay to change a transaction's id | The txid preimage ends *before* the compact-size count that introduces the witnesses, so neither witness data nor the count enters the txid | 1 | **yes** — `TransactionPrimitive.WitnessMalleabilityCannotChangeTheTxid` and `.TheWitnessCountBelongsToTheWitnessSection` |
| Merkle root collision (CVE-2012-2459) | A different transaction list producing the same Merkle root, letting an attacker make a node mark a valid block permanently invalid | Three independent defences: odd nodes promoted unchanged rather than duplicated, distinct leaf and branch tags, and the leaf count committed in the root | 1 | **yes** — `Merkle.OddLeafIsPromotedRatherThanDuplicated`, `.InnerLevelPromotionDoesNotCollide`, `.CountIsCommitted`, and `.DistinctListsHaveDistinctRoots` over all 3279 lists of length 1–7 on three ids. `scripts/mutate_merkle.sh` records what the property tests cannot do: because the defences are redundant, removing any one of them is caught only by the fixed vectors |

The malleability row is the one worth dwelling on. A signature that fails to verify
is a non-event; a *second valid encoding* of a signature that does verify changes
the transaction id, which breaks anything that referenced it. This is the class of
bug that is invisible in a "does it verify?" test and is exactly why
[Phase 6](ROADMAP.md#phase-6--post-quantum-integration)'s criterion has a rejection
half.

### 3. Determinism and convergence

This class has no exploit column, because the attack is usually not an attack. It
is a node behaving differently from another node for a reason nobody intended, and
an adversary who *notices* it first gets to choose which half of the network sees
which chain.

| Non-determinism source | Why it splits a chain | Defence |
|---|---|---|
| Wall-clock time in a validation rule | Two nodes validate at different instants | Consensus functions take data as arguments and read no clock. Timestamps are validated as *data*, against the header chain, never against local time except for a bounded far-future rule |
| Uninitialised memory | Same input, different result, per run | Sanitizer presets that fail rather than warn; MSan available |
| Hash-map or set iteration order | Ordering feeds a hash or a decision | Ordered containers in anything consensus-visible; a `#include <unordered_map>` in `consensus/` is a review stop |
| Floating point | Rounding differs by platform, compiler flags, and library version | No monetary or consensus value is ever floating point. Integer base units only |
| Locale | Case, collation and number formatting differ per environment | No locale-dependent function in a consensus path |
| Signed overflow | Undefined behaviour lets the optimiser delete the check | `-fwrapv`, plus checked arithmetic that returns `std::optional` so an overflowed value cannot be read unknowingly |
| Compiler or optimisation level | The same source produces different behaviour | Every rule is a pure function of committed data. Verified in practice by building the full matrix under GCC and Clang, Debug and Release, with sanitizers |
| Platform integer width or endianness | Serialisation differs by host | Fixed-width types and explicit little-endian encoding; two byte orders in `Hash256` separated in the type's API so they cannot be confused |
| Inexact state reversal | A node that reorganised arrives at a different UTXO set from one that synced the same chain directly — both believe they are on the same chain while holding different money, and neither has rejected anything | Undo data records the full contents of every coin a block spent, and disconnecting compares every coin it removes for **full equality** against what the block says it created rather than merely for presence. Pinned by `DisconnectBlock.RestoresTheSetExactlyAsItWas` and `.AStoredCoinIsComparedInFullRatherThanForPresenceAlone` |
| Partially applied block | A block rejected half way through leaves the set edited, so the node's state depends on *which rule* rejected it | Every change is staged in a layer over the node's set and committed only after the last rule passes. Not flushing is not an action, so there is no rollback path to get wrong. Pinned by `ConnectBlock.AnInputThatIsNotInTheSetIsRejectedAndTheSetIsUntouched` and the change-count assertions in `utxo_coins_test.cpp` |
| Storage or cache state | A validation outcome depends on what is cached | Structural: consensus does not link storage. Enforced by the build, not by review |
| Network state | An outcome depends on which peer answered first | Structural: consensus does not link networking |

The last two are the reason the layering rule in
[ARCHITECTURE.md](ARCHITECTURE.md) is enforced by the linker rather than by
convention. A reviewer can miss an `#include`; a link error cannot be missed.

### 4. Proof of work and chain selection

| Attack | Mechanism | Defence | Phase | Tested |
|---|---|---|---|---|
| Invalid work accepted | A header whose hash does not meet its target | Target recomputed from the header chain, never taken from the block | 1 | **yes** — `CheckBlockHeader` compares the hash against the header's own target in display order, and `ContextualCheckBlockHeader` refuses a header whose `target_bits` differ from the value computed from the chain. The index is what computes it: `HeaderContextFor` fills `expected_bits` from `NextTargetBits(parent)`, so the header's own field cannot buy a weaker target. `BlockIndex.AHeaderFailingItsOwnRulesIsRefusedWithTheRulesVerdict` takes a mined header, changes the nonce until the work is gone, and asserts it is refused and not stored; `NextTargetBits.IsInheritedAndNeverEasierThanTheFloor` covers the floor clamp |
| Compact target manipulation | A `target_bits` encoding with a negative or overflowing mantissa, or a non-canonical encoding of the same target | Bounds-checked decode plus a canonical-form check; a target above the network maximum rejected | 1 | partial — the codec is implemented and tested (`ConsensusTarget.*`, including the round-trip canonicality rule and the sign, overflow and zero-mantissa rejections). The floor is enforced in two places: `CheckBlockHeader` rejects a header claiming a target easier than `pow_limit_bits`, and `NextTargetBits` clamps the value the node computes for itself, covered by `NextTargetBits.IsInheritedAndNeverEasierThanTheFloor`. The `HeaderTargetBelowFloor` rejection itself has no direct test |
| Difficulty manipulation by timestamps | Backdated or forward-dated headers to make the next target easier | ASERT retargeting on every block rather than on a window boundary, removing the boundary that Bitcoin's timewarp exploits; timestamps bounded relative to median-time-past and to a far-future limit | 3 | no |
| Timewarp | Repeatedly rewinding timestamps across a retarget boundary to drive difficulty down | Per-block retarget plus a monotonicity constraint on median-time-past. The specific rule gets simulation evidence in Phase 3, not an appeal to precedent | 3 | no |
| Low-difficulty chain flood | Thousands of cheap headers to exhaust memory or CPU | Headers-first sync with work checked before storage; a header chain must demonstrate more work than the current tip before its blocks are requested | 4 | partial — the check order is implemented: `AddHeader` runs `CheckBlockHeader` first, before any map lookup and before anything is allocated, so a fabricated header costs one hash and is never stored. Bodies are not needed to index a header, so a node weighs every branch before requesting one. The per-peer accounting that turns this into a sync policy is Phase 4 |
| Selfish mining | Withholding blocks to waste honest work | Not preventable at minority hashrate; the defence is that it is unprofitable below a threshold, and that chain selection is on accumulated work rather than on block count or arrival time | 3 | partial — selection is on summed work, tested by `Work.TwoEasyBlocksCanOutweighOneHardBlock` and `BlockIndex.MoreWorkWinsHoweverLateItArrives`. Profitability thresholds are an economic question for Phase 3 |
| Free block withholding via a deterministic tie-break | With a lowest-hash tie-break, a miner sits on a low-hash block, lets a rival win the height, and publishes later to displace them at no cost — reversing a one-confirmation payment for free | Ties go to the header seen first, so publishing late loses a tie that publishing on time would have won. Recorded as the order headers were offered, not read from a clock | 1 | **yes** — `BlockIndex.EqualWorkKeepsTheBranchSeenFirst` asserts the first-seen branch keeps the tip at identical work, and that `IsBetterTip` is a strict order |
| Deep reorganisation | Rewriting confirmed history | Cost is the work in the rewritten span. Confirmation counts are a user-facing risk statement, not a consensus rule. Coinbase maturity of 200 blocks is set against this | 2, 3 | no |
| Reorganisation without hashrate | A bug allowing a lower-work chain to win | This is the actual bug to hunt: chain selection must be a total order on accumulated work with deterministic tie-breaking | 1 | partial — `IsBetterTip` is a strict total order by construction: work is compared as a big-endian byte array so the comparison is the defaulted one and cannot be written wrongly, and sequence numbers are unique so no two entries are each better than the other. `Work.*` pins the arithmetic, including agreement with the published difficulty-one chainwork constant; the chain tests cover more-work, equal-work and ruled-out branches. `ChainState` now performs the switch, and `DisconnectBlock` restores every coin a block spent from the undo record, so a branch that loses is fully undone rather than partially. What is not yet tested is a reorganisation driven by *two peers* rather than by a test harness, because there is no peer yet |
| Accumulated work overflowing | A chain whose summed work wraps past 2^256, making an enormous chain compare as a trivial one | Saturating addition rather than wrapping. Unreachable — the bound is the number of hashes physics permits — but the two failure modes are asymmetric, so the code takes the arbitrary one over the catastrophic one | 1 | **yes** — `Work.AdditionCarriesAcrossTheWholeWidth` and `Work.AdditionSaturatesRatherThanWrapping` |
| Two honest nodes computing different work for the same header | A rounded or floating-point difficulty, so nodes disagree about which of two nearly equal branches is heavier | Work is an integer floor division computed identically everywhere; the type admits no conversion to floating point and no multiplication, so consensus cannot see a rounded value | 1 | **yes** — `Work.TheDifficultyOneFloorAgreesWithTheKnownValue` checks the implementation against a constant derived outside this project, which is what makes the arithmetic more than self-consistent; the exact-power-of-two cases pin the division |
| A valid block permanently refused by a node's own cache | Caching the rejection of a header that was only too far in the future, so the node refuses it and every descendant forever once the network accepts it | Rejected headers are not remembered at all, so a later offer is re-judged | 1 | **yes** — `BlockIndex.AHeaderWithAnUnknownPredecessorIsRefusedAndNotRemembered` asserts a refused header is not stored and that the same header is accepted once its predecessor arrives |
| A ruled-out branch returning as a candidate | A rejected block's descendants still being weighed for the tip, or a later validity report erasing a recorded rejection | Failure is inherited to the whole subtree when it is recorded, and is a separate field from the validity ladder, which only ever rises | 1 | **yes** — `BlockIndex.MarkingABlockFailedRulesOutEverythingBelowIt`, `.AHeaderOnARejectedBranchIsRefusedWithoutBeingJudged`, `.RecordedValidityOnlyEverRises` |
| Tip regression bought with a header | Announcing a heavier branch and never sending its bodies, so the node reverts to the fork point, connects nothing, and ends up on a shorter chain than it started with — a reorganisation for the price of 92 bytes | Before a switch begins the target is truncated to the highest block whose whole path from the fork point is stored, and the switch is abandoned unless that truncated target still wins under `IsBetterTip`. Every block contributes at least one unit of work, so an ancestor's total work is strictly below its descendant's and a truncated target can never beat the tip it would replace | 3, 4 | partial — the guard is implemented in `ChainState::ActivateBestChain` and its argument is arithmetic rather than empirical, which is why it is stated as a bound in [AMARIAN_PROTOCOL.md](AMARIAN_PROTOCOL.md#carrying-the-plan-out) and decision 59. No test exercises a withholding peer, because nothing can withhold yet: the P2P layer is Phase 4, and that is where the adversarial test belongs |
| A corrupt coins set from an interrupted reorganisation | Killing a node part way through a deep switch so the unspent output set describes neither the old chain nor the new one | Application and reversal are atomic per *block*, not per switch, so every intermediate state is the set for some valid chain; an interrupted node is on a shorter chain and the next activation walks it back up | 1, 3 | partial — `ConnectBlock` and `DisconnectBlock` stage into an inner cache and flush only on success, covered by `ConnectBlock.*` and `DisconnectBlock.*` including the failure paths that must leave the set untouched. What is untested is an actual interruption, which needs the persistence layer to be meaningful — an in-memory set does not survive the process either way |
| Difficulty oscillation on a young chain | Hashrate arriving and leaving faster than the retarget adapts | ASERT's exponential response, parameterised with simulation evidence. Acknowledged as the most fragile part of a new chain | 3 | no |

The distinction in that table between "selfish mining" and "reorganisation without
hashrate" is the one that matters. The first is an economic problem inherent to
proof of work and is an accepted risk. The second is a software defect and is
Critical.

### 5. Resource exhaustion

Every row here is a case where the *cost to the attacker* and the *cost to the
victim* are asymmetric, which is the only thing that makes a denial of service
worth doing.

| Attack | Mechanism | Defence | Phase | Tested |
|---|---|---|---|---|
| Quadratic validation cost | A transaction whose signature hashing grows with the square of its size (Bitcoin's pre-SegWit sighash flaw) | Sighash midstate reuse so each input's preimage is linear; committed to in the design before the format is fixed rather than patched later | 1 | **yes** — `PrimitivesSigHash.ThePreimageIsAlwaysTheDocumentedFixedSize` asserts the preimage is 181 bytes regardless of the transaction's size, and `CheckSpendAuthorisation` calls `ComputeSigHashMidstates` once per transaction rather than once per input, so total hashing is linear in transaction size |
| Signature-count amplification | An input offering many more signatures than its condition has keys, so a naive matcher tries every pair | Ordered forward match: one monotonically advancing key index across the whole signature list, so at most `len(keys)` verifications happen per input no matter how many signatures are offered — and `len(keys)` is capped at 16 by the condition the coin's own commitment named | 1 | **yes** — the bound is structural in `SatisfiesThreshold`, and `Validation.AThresholdIsNotMetByOfferingOneKeysSignatureTwice` exercises the case where a signature is offered that no remaining key can answer. `CheckWitness` additionally requires exactly `threshold` signatures, so the list length is not free either |
| Expensive-to-validate block | A block filled with maximally costly inputs | Weight limit bounds size; measured per-block verification CPU for post-quantum schemes is 0.064–0.113 s against a 300 s interval, so the current parameters have four orders of magnitude of headroom | 1, 6 | partial — the weight limit is implemented, untested: `CheckBlock` measures the block's weight by serialising it and `CheckTransaction` bounds each transaction, so the cost of a block is bounded before any signature is verified. The measurement in the middle column is a real benchmark, recorded in [PQ_CRYPTO.md](PQ_CRYPTO.md) |
| Memory exhaustion on deserialisation | A length prefix declaring a gigabyte | Every length checked against the remaining buffer *before* allocation, never against a constant alone, using a per-element minimum encoded size so a count is bounded by the bytes that could possibly satisfy it | 1 | **yes** — `CompactSize.RejectsACountLargerThanTheBytesRemaining`, `.BoundScalesWithElementSize`, `.ZeroElementSizeIsRejectedRatherThanWideningTheBound`, `ByteString.RejectsLengthBeyondTheBuffer`, `TransactionPrimitive.CountsAboveTheLimitsAreRejected`, and 21 million `fuzz_serialize` executions with a peak RSS the run records |
| Deeply nested or recursive structure | Blowing the stack during parsing | Structures are flat by design; no recursive descent in consensus deserialisation | 1 | partial — true by construction and exercised by `fuzz_serialize`, but no test asserts the absence of recursion |
| Mempool flooding | Cheap transactions to fill memory | Minimum relay fee, mempool size cap with fee-based eviction, per-peer rate limits. Policy, not consensus | 4 | no |
| UTXO set bloat | Many tiny outputs to grow every node's state permanently | Dust threshold as relay policy. Not a consensus rule, because a consensus rule on output size cannot be relaxed later | 4 | no |
| Address or inventory flooding | Millions of announcements | Bounded caches with random eviction, and rate limits per peer | 4 | no |
| Connection exhaustion | Occupying every inbound slot | Inbound slot limits, eviction preferring peers that have provided useful data | 4 | no |
| Bandwidth amplification | Requesting the same large data repeatedly | Per-peer request accounting and ban scoring | 4 | no |

The quadratic-sighash row is deliberately first. It is the clearest historical
example of a denial-of-service vector baked into a transaction format, discovered
after the format could no longer be changed. Getting it right requires deciding it
in Phase 1, which is why the sighash design is fixed before the serialisation is.

### 6. Network layer

| Attack | Mechanism | Defence | Phase | Tested |
|---|---|---|---|---|
| Eclipse | Occupying all of a victim's connections to control its view | Diversity requirements on outbound peers across address groups, anchor connections persisted across restarts, and outbound selection never driven solely by peer-supplied addresses | 4 | no |
| Sybil | Many identities to appear as many independent peers | Identities are free and counting them is meaningless; every security decision is on work, never on peer agreement | 4 | no |
| Partition | Splitting the network into halves that cannot see each other | Cannot be prevented, only detected and survived. Convergence after a deliberate fork is an explicit acceptance criterion of Phase 4, not an assumption | 4 | no |
| Address poisoning | Filling a victim's address database with attacker-controlled entries | Bucketing by network group so one actor cannot dominate, and bounded tables with eviction | 4 | no |
| Transaction origin inference | Correlating first-broadcast to deanonymise | Randomised relay delays. Improves privacy; explicitly not a strong anonymity claim | 4 | no |
| Block withholding to a target | Delaying a victim's view of the tip | Multiple peers, headers-first sync, and a stall timeout that disconnects a peer failing to deliver | 4 | no |
| Protocol downgrade | Negotiating a weaker version or feature set | Minimum supported version enforced at handshake; no negotiable security feature | 4 | no |

### 7. Wallet

Wallet failures do not threaten the network, and the layering makes that structural
rather than aspirational: a wallet bug can lose its owner's coins and cannot change
what any node accepts.

| Attack | Mechanism | Defence | Phase | Tested |
|---|---|---|---|---|
| Weak key generation | Predictable randomness | OS CSPRNG via OpenSSL, never a user-supplied or time-seeded source | 5 | no |
| Nonce reuse in signing | Two signatures with the same nonce reveal the key | BIP-340's deterministic nonce derivation, from libsecp256k1's own implementation | 5 | no |
| Backup that does not restore | A file written but never verified | The Phase 5 criterion is tested by restoring from the backup, not by writing it | 5 | no |
| Address reuse | Publishing a key repeatedly, growing quantum and privacy exposure | The wallet makes reuse the awkward path rather than merely documenting against it | 5 | no |
| Change address confusion | Sending change somewhere unrecoverable | Change always to a key derivable from the same seed | 5 | no |
| Fee overpayment or stuck transaction | Bad estimation | Fee estimation with an explicit override, and a visible fee before confirmation | 5 | no |
| Amount or recipient display mismatch | The signed transaction differs from what was shown | The confirmation step renders the transaction that will be signed, from the same structure | 5, 11 | no |

### 8. Build and supply chain

| Attack | Mechanism | Defence | Phase | Tested |
|---|---|---|---|---|
| Compromised dependency | A malicious version of a library | Dependencies are few, mature, and system-packaged; versions are recorded in `--build-info` so a binary can be traced to what it was built against | 0 | partial |
| Undetected local modification | A build from a dirty tree passed off as a release | `amariand --version` prints the commit and marks a modified working tree as such | 0 | **yes** |
| Non-reproducible release | Two builds of the same commit differing | Not yet addressed. Reproducible builds are Phase 11 work and are listed as an open item, not claimed | 11 | no |
| Weakened build flags | Hardening silently disabled | Hardening is in a single interface target every other target links, and `--build-info` prints what was actually used | 0 | partial |

"Partial" means the mechanism exists and is visible in output, but no test asserts
it. That distinction is the reason the column exists.

## Cryptographic agility is itself an attack surface

A mechanism flexible enough to retire a broken signature scheme is flexible enough
to be abused, and this deserves its own section rather than a table row.

| Attack | Concern |
|---|---|
| Malicious activation | A scheme activated that nobody reviewed, or activated on a timeline too short to audit |
| Coerced migration | UTXOs on a retired scheme made unspendable, which is confiscation regardless of intent |
| Downgrade | An attacker causing a lock to be satisfiable by a weaker scheme than its owner chose |
| Activation ambiguity | Two nodes disagreeing about whether a scheme is active — a split, by definition |
| Emergency abuse | An emergency path, justified by a real break, used for something else |

The design constraints that follow: activation is a consensus rule derived from
block data, never an operator setting or a signed message from anyone; retirement
restricts *creating* new locks and never invalidates existing ones, so no UTXO
becomes unspendable by scheme retirement; and every scheme identifier is explicit,
so "which scheme is this" is never inferred. Phase 8 has to settle the process, and
that is the phase's actual content — the mechanism is the easy part.

## Accepted risks and explicit non-goals

Stating these plainly is more useful than a defence that does not exist.

**Majority hashrate.** An adversary with more than half the hashrate can reorganise
recent history, censor transactions, and double-spend. Proof of work does not defend
against this; it makes it expensive. **It cannot mint coins beyond the schedule, and
it cannot spend coins it has no key for** — those remain enforced by every node
regardless of hashrate. A young chain with little hashrate is genuinely more exposed
here than an established one, and no design choice in this project changes that.

**A compromised machine.** Malware, a hostile operator, physical access, or a
compromised OS. If the attacker is inside the machine, they have the keys.

**Coins locked to a scheme or lock version nothing enforces.** Consensus accepts an
unknown key scheme as satisfied and an unknown lock version as spendable without any
check, because a node cannot enforce a rule it does not contain and rejecting would
fork it off the chain the moment the rule was deployed. The consequence is that coins
sent to an identifier no deployed node implements are spendable by anyone. This is
not a consensus hole — it is what makes soft-fork extension possible at all, and the
protection is that upgraded nodes do check, so the identifier only becomes *usable*
once a hashpower majority enforces it. It does mean a wallet must never let a user
lock coins to an unactivated identifier, which is Phase 8's problem rather than
consensus's.

**Traffic analysis by a global observer.** Someone who sees all network traffic can
correlate broadcasts. Amarian is not an anonymity network and does not claim to be.

**Social engineering.** Users sending coins to the wrong recipient, or being talked
into revealing a seed phrase. Interface design can reduce this; nothing eliminates
it.

**Loss of a key.** Self-custody means an irrecoverable key is irrecoverable coins.
This is a property, not a bug, and it is the reason
[hybrid authorisation](PQ_CRYPTO.md#the-hybrid-question-is-open) is a genuine
trade-off rather than a free improvement: it doubles the number of ways to lose
access.

**Fee revenue after issuance ends.** Whether fees alone can fund the security
budget is unsolved, for Amarian and for every other proof-of-work chain. See
[ECONOMICS.md](ECONOMICS.md#fees-after-issuance-ends). Amarian's position is that
the transition is gradual enough to be observed decades before it matters, not that
it is solved.

**The 12.5%-per-era schedule has no adversarial history.** A halving has fifteen
years of it. This is a known risk, recorded in
[../DEVELOPMENT_STATUS.md](../DEVELOPMENT_STATUS.md).

## Stated assumptions

Every claim in this document holds only if these hold. A report that one of them is
false is a security report, and a valuable one.

1. **An honest majority of hashrate.** Foundational to proof of work, and the
   assumption most obviously outside the software's control.
2. **The cryptographic primitives are secure and correctly implemented.** SHA-256,
   BIP-340 Schnorr over secp256k1, ML-DSA, SLH-DSA — as implemented by
   libsecp256k1 and OpenSSL. Amarian implements none of them. The post-quantum
   assumptions are enumerated separately in
   [PQ_CRYPTO.md](PQ_CRYPTO.md#stated-assumptions).
3. **The network is not permanently partitioned.** Temporary partitions are
   expected and must be survived; a permanent one is two networks.
4. **Enough independent nodes exist to make verification meaningful.** This is
   currently **false** — there is no network, and no node in the tree has yet
   validated a block against a chain. The context-free rules that decide whether a
   block is *internally* well formed exist; what does not is a second node to
   disagree with. It is Phase 4 and Phase 10 work, and until it is true,
   "decentralised" is not claimed.
5. **The compiler and standard library are correct.** Mitigated rather than assumed:
   two compilers, four sanitizers, and a full build matrix, which is why a codegen
   disagreement between GCC and Clang was found in Phase 0 rather than later.
6. **The operator's machine is not compromised.** See above.

## Current coverage, stated honestly

| Class | Defence designed | Defence implemented | Regression test |
|---|---|---|---|
| Supply integrity | yes | the per-amount bound, the coinbase, height and output-sum rules, the per-transaction fee rule, and now the block-level rules — coinbase ≤ reward + fees actually paid, coinbase maturity, input existence and the duplicate-outpoint invariant | nine rows: the amount bound, both summing sides, outputs-exceed-inputs, the coinbase bound, phantom fees, maturity, missing inputs and double spends |
| Spend authorisation | yes | structure, identity, and verification — the lock commitment, the threshold walk and `crypto::Verify` | ten rows, on identity, the sighash commitments, the commitment check and the threshold |
| Determinism | yes, and partly structural | partly — layering, checked arithmetic, `-fwrapv`, sanitizers, canonical encoding, and atomic-plus-exact state transitions | encoding rows, plus the reversal and atomicity rows |
| Proof of work and chain selection | yes | the target codec and the per-header work check; no chain selection | one row on the Merkle construction, plus the target codec |
| Resource exhaustion | yes | the deserialisation rows, the weight limit, the linear sighash and the bounded threshold walk | four rows, one of them fuzzed |
| Network layer | outline only | no | no |
| Wallet | outline only | no | no |
| Build and supply chain | yes | mostly | one, informal |
| Cryptographic agility | yes | explicit scheme identifiers, a registry over two backends, and a startup gate on availability | the registry's self-consistency and both extension points' soft-fork behaviour |

Every class still has more "no" in it than "yes", which is what Phase 1 of 13 looks
like part way through. A document claiming otherwise would be the more serious
defect.

What exists today that belongs in this table at all: the layering contract enforced
by the linker; checked arithmetic with `-fwrapv` behind it; hardening flags that
stay on in Release; five test presets including two sanitizer builds, with UBSan
integer findings now fatal rather than recoverable; the canonical codec with its
reject-not-normalise rule, sticky failure and bounds-before-allocation; consensus
hashing checked against published vectors; the transaction primitives with the
txid/wtxid split and the Merkle construction; three fuzz harnesses, the widest of
which covers the deserialisation perimeter itself; the context-free validation
rules; the signature scheme registry over libsecp256k1 and OpenSSL with the startup
gate that refuses to run a build missing one; the fixed-size signature hash;
spend authorisation itself — the lock commitment, the ordered threshold walk against
real signatures, and the rule that a transaction cannot pay out more than it spends;
and the unspent output set, with atomic connect and disconnect, an undo record that is
compared for full equality on reversal, and the block-level supply bound that needed it.

What does not exist: any rule that decides *which* state to be in. A block can now be
applied to a set and taken back off again, and every rule that consults a coin is
enforced — input existence, maturity, the fee total, the duplicate-outpoint invariant.
What is missing is the chain: nothing yet decides which block to apply, so
`ConnectBlock` is handed a block and a height by a caller. No block index exists, no
accumulated-work comparison, no persistence — so a node cannot yet disagree with a peer
about a tip, because it does not have one. Everything in the network and wallet classes
is therefore still a plan, and the rows say so.

[Phase 9](ROADMAP.md#phase-9--security-engineering) is where every row above
acquires a test that fails when the defence is removed — a stronger property than
the **yes** entries currently claim, and one that
[scripts/mutate_merkle.sh](../scripts/mutate_merkle.sh) shows is not automatic:
redundant defences make single-mutation testing blind unless the tests are designed
against it. That is a Phase 9 problem worth knowing about now rather than
discovering then.






