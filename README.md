# Amarian

An independent proof-of-work blockchain built from scratch in C++23.

Bitcoin is digital gold. Amarian aims to be digital diamond: scarce, durable,
hard to manipulate, highly divisible, self-custodial, and designed from the
first commit for a world in which large quantum computers exist.

**This is not a promise of value or an investment.** It is an engineering
project. Nothing here is a security offering, a price prediction, or financial
advice.

---

## Status

**Phase 1 of 13 is complete.** A real Amarian node exists: it builds blocks, judges
them by a full consensus rule set, keeps them in RocksDB, follows the branch with the
most work, and survives a restart. Two independently launched nodes with separate data
directories reach the same tip when one mines a chain and hands the raw blocks to the
other — which is Phase 1's acceptance criterion, and it is checked by
[scripts/phase1_acceptance.sh](scripts/phase1_acceptance.sh) rather than asserted here.

What is still missing is the network. Blocks move between nodes through a flat file
today, not over a socket, and there is no mempool, no wallet and no RPC. Those are
Phases 3 to 5.

What actually works today, verified by 246 passing tests across five presets plus the
acceptance script:

| Component | State |
|---|---|
| Hardened C++23 build (GCC 15 / Clang 21, CMake + Ninja), nine presets | working |
| `util`: byte/hash types, strict hex codec, canonical serialisation codec, checked arithmetic, logging, CLI options | working |
| `crypto`: SHA-256, double SHA-256, tagged hashing; signature scheme registry; verification | working |
| `primitives`: amounts, outpoints, spend conditions, locks, witnesses, transactions, blocks, Merkle tree, signature hash | working |
| `consensus`: chain parameters for three networks, issuance schedule, genesis, compact target codec, context-free block and transaction rules, spend authorisation, the contextual input rules, accumulated work | working |
| `utxo`: the unspent output set, atomic block application and reversal, undo records | working |
| `chain`: the header tree, accumulated work per branch, the best-tip rule, the revert/apply plan between two tips, and the activation that carries it out | working |
| `storage`: coins, block bodies, undo records, the header tree and the tip in RocksDB, atomic across all five | working |
| `mining`: block assembly from the tip, self-checked against the node's own rules, and a bounded CPU nonce search | working |
| `amariand`: data directory, chainstate restore, `--generate`, `--import-blocks`, `--export-blocks`, chain identity and backend startup gates | working |
| Difficulty retargeting | **not started** (Phase 3) — the target is constant today, which is a complete rule and not a stub |
| Mempool, fee selection and mining RPC | **not started** (Phase 3) |
| P2P networking | **not started** (Phase 4) |
| Wallet | **not started** (Phase 5) |
| Post-quantum signatures | **verifiable in consensus**, not yet spendable — no wallet, address format or migration path (Phase 6) |

That last row is worth reading precisely. ML-DSA-44 and SLH-DSA-SHA2-128s
verify today through the same code path as BIP-340 Schnorr, and a transaction
authorised by a real ML-DSA-44 signature passes consensus in the test suite.
Nothing yet *creates* such an output, so the schemes are implemented but not
usable. See [docs/PQ_CRYPTO.md](docs/PQ_CRYPTO.md#what-is-built-and-what-is-not).

There is no mempool, so a mined block carries its coinbase and nothing else. The fee
term in the coinbase rule is present and is handed a real zero rather than an assumed
one, so adding a mempool changes an expression in the assembler and no consensus rule
anywhere. See [DEVELOPMENT_STATUS.md](DEVELOPMENT_STATUS.md) for the current task, the
next task, and open risks.

Claims this project does **not** make yet, and will not make until there is
evidence: that it is decentralised, that it is post-quantum secure, or that any
performance number outside [docs/PQ_CRYPTO.md](docs/PQ_CRYPTO.md) has been
measured.

## Design goals

- **Fixed supply.** A deterministic issuance schedule that is a pure function
  of block height, enforced by every node. No premine, no tail emission, no
  mint authority, no developer-controlled issuance.
- **Proof of work.** SHA-256d, Bitcoin-derived, with a difficulty algorithm
  chosen for stability rather than novelty.
- **UTXO model.** Small, deterministic, auditable validation rules.
- **Integer money only.** All monetary values are integer base units. No
  floating point touches a monetary quantity anywhere in the codebase.
- **Post-quantum readiness as a design constraint, not a bolt-on.** Spend
  conditions carry explicit algorithm identifiers from the first block, so
  adding or retiring a signature scheme is a protocol upgrade rather than a
  redesign. See [docs/PQ_CRYPTO.md](docs/PQ_CRYPTO.md).
- **No hand-rolled cryptography.** Primitives come from libsecp256k1 and
  OpenSSL, behind interfaces narrow enough to swap the implementation.
- **Usable by normal people, eventually.** The protocol comes first, but the
  architecture keeps wallet logic separate from the node so that a GUI or
  mobile wallet is an additional front-end rather than a rewrite.

## Building

Requires a C++23 compiler, CMake 3.28+, Ninja, libsecp256k1, OpenSSL 3.5+,
RocksDB, Asio and nlohmann-json. On Debian/Ubuntu:

```bash
sudo apt install build-essential cmake ninja-build pkg-config libsecp256k1-dev libssl-dev librocksdb-dev libasio-dev nlohmann-json3-dev libgtest-dev
```

Then:

```bash
cmake --preset dev && cmake --build build/dev && ctest --preset dev
```

Other presets: `debug`, `clang-dev`, `asan`, `tsan`, `fuzz`, `bench`,
`bench-clang`, `release`. The `asan` preset enables AddressSanitizer and
UndefinedBehavior Sanitizer with `-fno-sanitize-recover`, so a sanitizer finding
fails the run. Fuzzing has its own recipe and one non-obvious required flag —
see [fuzz/README.md](fuzz/README.md).

```bash
./build/dev/src/amariand --build-info
```

## Running a node

A run opens the chain, does what it was asked, makes the database durable and exits.
There is no event loop, because there is nothing yet to service — see decision 70.

Mine five regtest blocks onto a fresh chain, paying the reward to a lock:

```bash
./build/dev/src/amariand --chain regtest --datadir /tmp/amarian-a --generate 5 --payout 0120$(printf '11%.0s' $(seq 32))
```

Hand those blocks to a second, entirely separate node, which judges every one of them
by its own rules:

```bash
./build/dev/src/amariand --chain regtest --datadir /tmp/amarian-a --export-blocks /tmp/blocks.dat
```

```bash
./build/dev/src/amariand --chain regtest --datadir /tmp/amarian-b --import-blocks /tmp/blocks.dat
```

Both print the same `tip height 5 <hash>` line, and both still print it when run again
with no arguments. `--payout` is the hex encoding of a `Lock`: a version byte, a
compact-size length, then the program. The one above is a version-1 commitment lock
over 32 bytes, which is spendable in shape but not by anyone — there is no wallet yet,
so nothing can construct a lock whose coins it could later move.

`--datadir` defaults to `$HOME/.amarian/<network>`, one directory per network. Opening
one network's directory as another is refused rather than reconciled.

## Repository layout

```
include/amarian/   public headers, mirroring the library layering
src/               library sources and the amariand entry point
tests/unit/        fast unit tests            (ctest -L unit)
tests/consensus/   consensus rule tests       (ctest -L consensus)
tests/integration/ multi-node tests           (ctest -L integration)
fuzz/              libFuzzer harnesses, one per parser
bench/             Google Benchmark microbenchmarks
cmake/             warning, hardening, sanitizer and dependency modules
docs/              protocol, economics, threat model and design records
```

The library layering is a hard architectural rule, enforced by target
dependencies: `util → crypto → primitives → consensus → utxo → chain`, with
mempool, mining, net, wallet, rpc and node above. **Consensus code never links
storage, networking, wallet or RPC.** See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/ROADMAP.md](docs/ROADMAP.md) | The 13 phases and their acceptance criteria |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Layering rules and module responsibilities |
| [docs/AMARIAN_PROTOCOL.md](docs/AMARIAN_PROTOCOL.md) | Consensus rules, serialisation, hashing |
| [docs/ECONOMICS.md](docs/ECONOMICS.md) | Supply schedule and the analysis behind it |
| [docs/PQ_CRYPTO.md](docs/PQ_CRYPTO.md) | Post-quantum design, assumptions, and what is *not* claimed |
| [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md) | Adversaries, assumptions, attack classes |
| [docs/NETWORK.md](docs/NETWORK.md) | P2P protocol and network parameters |
| [docs/WALLET.md](docs/WALLET.md) | Key management, addresses, backup |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Dated record of architectural decisions and their evidence |
| [SECURITY.md](SECURITY.md) | How to report a vulnerability |

## Ticker

Not finalised. `AMR` is the working label. It is unclaimed among the coins
CoinGecko indexes as of 2026-09-04, but it is in active use on the NYSE by
Alpha Metallurgical Resources. The ticker is a display-layer chain parameter
and is not consensus-critical, so it can change without a fork. See
[docs/ECONOMICS.md](docs/ECONOMICS.md#unit-naming-and-ticker).

## Licence

MIT. See [LICENSE](LICENSE).
