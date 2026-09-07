# Economics

Every number in this document was computed with exact integer arithmetic, not
estimated. The schedule is a pure function of block height; nothing about it
depends on a clock, a vote, or a key anyone holds.

**Nothing here is a claim about value.** A supply schedule determines how many
units exist, and that is all it determines. It says nothing about what a unit is
worth, and this project makes no prediction about that.

## Parameters

| Parameter | Value |
|---|---|
| Base unit | facet |
| Facets per AMR | 10 000 000 000 (10<sup>10</sup>, 10 decimal places) |
| Target block interval | 300 s (5 minutes) |
| Era length | 105 000 blocks (364.58 days) |
| Initial block reward | 10 AMR = 100 000 000 000 facets |
| Per-era reward decay | `reward = reward * 7 / 8`, integer division |
| **Maximum supply** | **83 999 999 932 170 000 facets = 8 399 999.993 217 0000 AMR** |
| Eras with a nonzero reward | 179 (era 0 through era 178) |
| Height at which issuance ends | 18 795 000 |
| Time to final issuance | ≈ 178.7 years |
| Coinbase maturity | 200 blocks (≈ 16.7 hours) |

## Where the cap comes from

The parameters were not picked and then summed; the sum was picked and the
parameters chosen to produce it. A geometric schedule with a per-era retention of
7/8 has the closed form

```
total = initial_reward × era_blocks × 1/(1 − 7/8) = initial_reward × era_blocks × 8
```

With 10 AMR and 105 000 blocks that is exactly **8 400 000 AMR**. So the cap is a
round number by construction rather than by coincidence, and the schedule that
produces it has no special cases.

The realised cap is 0.006 783 AMR *below* 8 400 000, because integer division
truncates the reward at every era boundary and the shortfalls accumulate. This is
the right direction for the error to go: truncation can only ever reduce the
reward, so the closed form is a strict upper bound that no execution of the
schedule can exceed. Consensus enforces the exact realised value —
83 999 999 932 170 000 facets — not the idealised one.

## Why 8.4 million and not some other number

In absolute terms the cap is arbitrary, and any honest treatment has to say so.
A fixed supply of 8.4 million divisible units and a fixed supply of 84 million
units one tenth the size are the same monetary system with different labels. What
is *not* arbitrary is the set of constraints the number has to satisfy:

**It must fit in a 64-bit signed integer with room to spare.** The cap in facets
occupies 57 bits, leaving a factor of 109.8 of headroom below `INT64_MAX`. That
margin is not decoration: intermediate sums during block validation add many
outputs together, and a fee-rate multiplication scales an amount by a size. A cap
that fit only just inside the type would make every such intermediate a potential
overflow, and the overflow of a monetary sum is an inflation bug.

**It must be divisible enough to stay usable across large changes in purchasing
power.** Ten decimal places means the smallest unit is 10<sup>-10</sup> AMR. If a
unit ever became worth a hundred thousand of today's dollars, a facet would still
be worth a hundredth of a cent, so ordinary small payments remain expressible in
whole base units. Bitcoin's eight decimals were a reasonable choice in 2009 and
have been visibly tight at times; two more decimal places cost nothing at 57 bits
and remove the question permanently.

**It must be smaller than Bitcoin's unit count, given the positioning.** Digital
diamond rather than digital gold implies fewer, more finely divisible units. 8.4
million against 21 million is that, and it lands on a round closed form.

**It must not require a special case anywhere.** The reward is a pure function of
height: divide by the era length, apply the retention 7/8 that many times, done.
There is no maximum-shift guard of the kind Bitcoin needs to stop its right-shift
becoming undefined behaviour after 64 halvings, because a repeated `* 7 / 8`
reaches zero on its own and stays there.

## Why 12.5% per era instead of a halving

A halving is a 50% cut to mining revenue delivered in a single block. The chain's
security budget drops by half between two consecutive blocks, and whether hashrate
leaves gradually or abruptly is left to how miners happen to be capitalised at
that moment. Bitcoin has absorbed this repeatedly, but Bitcoin has a hashrate
measured in exahashes and a deep derivatives market to smooth the transition. A
new chain has neither.

Amarian instead cuts 12.5% at each era boundary, roughly annually. The gradient is
gentler, and — this is the part worth noting — it is not actually a slower
schedule overall:

| | per step | annualised | 4-year retention |
|---|---|---|---|
| Bitcoin | 50% every ~4 years | 15.91% | 0.5000 |
| Amarian | 12.5% every ~1 year | 12.50% | 0.5862 |

So Amarian's issuance decays slightly *more slowly* per year than Bitcoin's, in
steps one quarter the size and four times as often. The cost of this choice is
honest and recorded in [../DEVELOPMENT_STATUS.md](../DEVELOPMENT_STATUS.md) as a
known risk: a halving schedule has fifteen years of adversarial history behind it
and this one has none.

## The schedule

First twelve eras, then the end of the tail. Rewards are exact; cumulative
percentages are of the realised cap.

| Era | Height range | Reward (AMR) | Era emission (AMR) | Cumulative |
|---|---|---|---|---|
| 0 | 0 – 104 999 | 10.000 000 0000 | 1 050 000.0000 | 12.500% |
| 1 | 105 000 – 209 999 | 8.750 000 0000 | 918 750.0000 | 23.438% |
| 2 | 210 000 – 314 999 | 7.656 250 0000 | 803 906.2500 | 33.008% |
| 3 | 315 000 – 419 999 | 6.699 218 7500 | 703 417.9688 | 41.382% |
| 4 | 420 000 – 524 999 | 5.861 816 4062 | 615 490.7227 | 48.709% |
| 5 | 525 000 – 629 999 | 5.129 089 3554 | 538 554.3823 | 55.120% |
| 6 | 630 000 – 734 999 | 4.487 953 1859 | 471 235.0845 | 60.730% |
| 7 | 735 000 – 839 999 | 3.926 959 0376 | 412 330.6989 | 65.639% |
| 8 | 840 000 – 944 999 | 3.436 089 1579 | 360 789.3616 | 69.934% |
| 9 | 945 000 – 1 049 999 | 3.006 578 0131 | 315 690.6914 | 73.692% |
| 10 | 1 050 000 – 1 154 999 | 2.630 755 7614 | 276 229.3549 | 76.981% |
| 11 | 1 155 000 – 1 259 999 | 2.301 911 2912 | 241 700.6856 | 79.858% |
| … | | | | |
| 178 | 18 690 000 – 18 794 999 | 0.000 000 0001 | 0.0000 | 100.000% |
| 179 | 18 795 000 – | 0 | 0 | — |

Era 178 pays a single facet per block. Era 179 pays nothing, and every era after
it pays nothing. **There is no tail emission.** After height 18 795 000 a
coinbase transaction may claim transaction fees and nothing else.

### Milestones

| Fraction of cap emitted | By end of era | Height | Year |
|---|---|---|---|
| 50% | 5 | 630 000 | 6.0 |
| 75% | 10 | 1 155 000 | 11.0 |
| 90% | 17 | 1 890 000 | 18.0 |
| 95% | 22 | 2 415 000 | 23.0 |
| 99% | 34 | 3 675 000 | 34.9 |

### Issuance rate

Annual issuance as a fraction of the coins already in existence — the figure
usually and misleadingly called inflation:

| End of year | Circulating (AMR) | Next year's issuance | Rate |
|---|---|---|---|
| 1 | 1 050 000.00 | 918 750.00 | 87.50% |
| 2 | 1 968 750.00 | 803 906.25 | 40.83% |
| 3 | 2 772 656.25 | 703 417.97 | 25.37% |
| 5 | 4 091 564.94 | 538 554.38 | 13.16% |
| 8 | 5 513 685.11 | 360 789.36 | 6.54% |
| 12 | 6 708 095.20 | 211 488.10 | 3.15% |
| 20 | 7 818 646.43 | 72 669.20 | 0.93% |
| 30 | 8 247 060.07 | 19 117.49 | 0.23% |
| 50 | 8 389 415.22 | 1 323.10 | 0.02% |

Annual issuance falls below 1% of supply in year 20 and below 0.1% in year 37.

### Compared with Bitcoin's distribution curve

Amarian is slightly *less* front-loaded than Bitcoin at every comparable point,
despite the smoother steps:

| Year | Amarian | Bitcoin |
|---|---|---|
| 4 | 41.4% | 50.0% |
| 10 | 73.8% | 81.3% |
| 20 | 93.1% | 96.9% |
| 50 | 99.88% | 99.98% |

Both schedules put most of the supply into circulation early. That is a real
property of any decaying-reward chain and worth stating plainly rather than
presenting a distribution curve as if it were flat: whoever mines in the first few
years receives a large share of everything that will ever exist. The difference
between that and a premine is that the coins must be *earned* by expending work,
by anyone who chooses to, under rules published before the first block — not
allocated by a founder.

## The supply cap in consensus

The cap is not a number a node checks against a running total it keeps. Two
independent mechanisms enforce it, and neither requires trusting the other:

1. **Per-block:** a block's coinbase output may not exceed the scheduled reward
   for its height plus the fees actually paid by the transactions in that block.
   The scheduled reward is recomputed from the height by every node.
2. **Per-transaction:** the sum of a transaction's outputs may not exceed the sum
   of its inputs. Every amount that goes into these sums is bounded to
   `[0, MAX_MONEY]`, and every addition is checked for overflow.

Together these make the total supply an emergent consequence of rules that are
each locally checkable, which is the property that lets a node verify issuance
without replaying the entire chain's arithmetic in one accumulator.

Phase 2 exists to attack this. The tests that must fail to validate include: a
coinbase one facet over the schedule; a coinbase claiming fees that were not paid;
outputs exceeding inputs; amounts engineered so a sum wraps; a negative amount; a
duplicate coinbase; and a coinbase spent before maturity. Until those tests exist
and pass, the cap is a design and not a demonstrated property.

## Fees after issuance ends

Once the reward reaches zero the coinbase can claim only fees, so the security
budget becomes whatever users pay for block space. This is the same open question
Bitcoin has, on a similar timescale, and Amarian does not claim to have solved it.

What can be said now is that the transition is gradual. Issuance is below 1% of
supply within twenty years and below 0.1% within thirty-seven, so fee revenue has
to become the dominant share of miner income within a few decades, not suddenly at
year 178. If it turns out that fees cannot sustain the security budget, that will
be visible long before the last facet is mined.

There is no tail emission, and adding one later would break the cap. The cap is
the property; a chain that mints new coins to pay for its own security has a
different property.

## Unit naming and ticker

**The base unit is the facet.** One AMR is 10 000 000 000 facets. The name follows
the positioning: a diamond's facets are what make it usable, and they are what
divide the whole into parts without diminishing it.

All consensus arithmetic is in facets. Nothing in the consensus layer knows the
name "AMR", and no monetary value is ever a floating-point number anywhere in the
codebase — the conversion to a decimal string with ten places is a display
operation performed at the RPC and CLI boundary, on integers.

**The ticker is not finalised.** `AMR` is the working label, and here is exactly
what was checked, on 2026-09-04:

- Unclaimed among the coins CoinGecko indexes.
- **In active use on the NYSE by Alpha Metallurgical Resources.** That is a real
  conflict, and it is the reason this is provisional rather than settled.

The conflict is with an equity ticker rather than another cryptocurrency, so the
practical consequences are search-result collisions and ambiguity in
financial-data contexts rather than exchange listing conflicts. That may be
acceptable; it may not. What makes the question deferrable rather than urgent is a
deliberate design property:

**The ticker is a display-layer chain parameter and is not consensus-critical.**
It appears in no serialised structure, no hash preimage, and no network message.
Changing it requires editing a parameter table and rebuilding, not a fork. An
address prefix, by contrast, *is* baked into address encoding and is much more
expensive to change — so that one is not being deferred.

A final decision needs a check against exchange and data-provider registries
rather than one aggregator, and it should happen before there is a testnet anyone
outside the project uses. Recorded as an open item in
[DECISIONS.md](DECISIONS.md).

## Parameters that are not final

To be explicit about what is settled and what is not:

| Parameter | State |
|---|---|
| Maximum supply, era length, decay, initial reward | **Settled.** Changing any of them changes the monetary system, which is the one thing this project exists to keep fixed. |
| Base unit name and decimal places | **Settled.** Decimal places are consensus-visible through the amount encoding. |
| Coinbase maturity (200 blocks) | Intent. Confirmed in Phase 2 against reorganisation depth. |
| Block interval (300 s) | Settled as a design input to the schedule above; a change would change the era duration and the emission timeline. |
| Ticker | **Provisional.** See above. |
| Minimum relay fee, dust threshold | Not yet designed. Policy, not consensus. |




