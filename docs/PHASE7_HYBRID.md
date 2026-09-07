# Phase 7: hybrid ownership decision

## Criterion

Phase 7 asks whether generation 1 should require both a classical signature and
a post-quantum signature for one spend. The decision must be based on measured
transaction cost and verification cost, not on the appearance of stronger
security.

## Reproducible measurement

Run:

```sh
cmake --preset bench
cmake --build --preset bench --target bench_wallet
bash scripts/phase7_benchmark.sh
```

The benchmark constructs valid one-input transactions and verifies them through
the production `CheckSpendAuthorisation` path. It also reports transaction
weight and the number of such transactions that fit under the configured block
weight limit.

Environment for the recorded run: GCC benchmark preset, WSL2 on a 12-vCPU
2.496 GHz host, 2026-09-07. The benchmark dependency reported a debug build,
so the absolute timings are indicative; the comparison was made by the same
binary and workload.

| Ownership | Verification median | Transaction weight | Transactions per block |
|---|---:|---:|---:|
| Schnorr-only | 49.7 µs | 475 | 4,210 |
| ML-DSA-44-only | 140.7 µs | 4,115 | 486 |
| Schnorr + ML-DSA-44 hybrid | 211.6 µs | 4,217 | 474 |

The hybrid adds only 102 weight units over ML-DSA-44, but verification is about
50% slower than ML-DSA-44 alone and about four times slower than Schnorr-only.
It also requires every wallet backup, key-derivation path, signing path, and
recovery procedure to keep two independent key types usable forever.

## Decision: defer default hybrid ownership

Generation 1 will not make hybrid ownership the default wallet policy. The
availability cost is permanent and immediate, while the additional protection
is a hedge against a future break in either scheme. ML-DSA-44-only ownership
already provides a post-quantum option without requiring two independent keys
for every spend.

This is a deferral, not a consensus limitation. Spend conditions already support
threshold 2-of-2 ownership, the hybrid signing helper remains available, and
the cryptographic-agility lifecycle can introduce a hybrid wallet policy in a
future generation without changing the transaction or lock format. Any future
implementation must repeat this benchmark on release builds and document key
backup, migration, and recovery procedures before activation.

Phase 7 acceptance is therefore met by the benchmark artifact, the recorded
decision, and the existing valid/tampered hybrid consensus tests.
