# Amarian

An independent proof-of-work blockchain built from scratch in C++23.

**Latest release: v2.8**

Bitcoin is digital gold. Amarian aims to be digital diamond: scarce, durable,
hard to manipulate, highly divisible, self-custodial, and designed from the
first commit for a world in which large quantum computers exist.

**This is not a promise of value or an investment.** It is an engineering
project. Nothing here is a security offering, a price prediction, or financial
advice.

---

## Status

**v2.8 is released.** Phases 0 through 12 are complete and Phase 13,
mainnet readiness, is in progress. The project has a validating node, CPU
mining, mempool and RPC workflows, wallet and backup/restore support,
post-quantum signature validation, live P2P synchronization, an explorer API,
release packaging, and reproducible performance measurements. Phase 7 hybrid
ownership is explicitly deferred for generation 1 based on benchmark evidence.

What works today, verified by the acceptance scripts and the full test suite:

| Component | State |
|---|---|
| Hardened C++23 build (GCC 15 / Clang 21, CMake + Ninja), nine presets | working |
| `util`: byte/hash types, strict hex codec, canonical serialisation codec, checked arithmetic, logging, CLI options | working |
| `crypto`: SHA-256, double SHA-256, tagged hashing; signature scheme registry; verification | working |
| `primitives`: amounts, outpoints, spend conditions, locks, witnesses, transactions, blocks, Merkle tree, signature hash | working |
| `consensus`: chain parameters for three networks, issuance schedule, genesis, compact target codec, context-free block and transaction rules, spend authorisation, contextual input rules, accumulated work | working |
| `utxo`: the unspent output set, atomic block application and reversal, undo records | working |
| `chain`: the header tree, accumulated work per branch, the best-tip rule, the revert/apply plan between two tips, and the activation that carries it out | working |
| `storage`: coins, block bodies, undo records, the header tree and the tip in RocksDB, atomic across all five | working |
| `mining`: block assembly from the tip, self-checked against the node's own rules, and a bounded CPU nonce search | working |
| `amariand`: data directory, chainstate restore, `--generate`, `--import-blocks`, `--export-blocks`, chain identity and backend startup gates | working |
| Difficulty retargeting, mempool, fee selection and mining RPC | working |
| P2P networking | working — inbound/outbound peers, headers-first sync, block relay and convergence |
| Wallet | **complete** (Phase 5) — key derivation, bech32m addresses, coin selection, fee estimation, transaction builder, BIP-39 backup/restore, and `amarian-wallet` CLI |
| Post-quantum signatures | working in consensus and wallet end-to-end tests |

Phase 12 results, including live socket byte counts, cold import/storage data,
and three-node convergence, are published in [docs/PHASE12_MEASUREMENTS.md](docs/PHASE12_MEASUREMENTS.md).
See [docs/ROADMAP.md](docs/ROADMAP.md) and [DEVELOPMENT_STATUS.md](DEVELOPMENT_STATUS.md)
for the Phase 13 launch-readiness work. This project makes no claim of production
security or monetary value.

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

A node can run one-shot commands or stay online with RPC and P2P listeners.

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

Both print the same `tip height 5 <hash>` line. For a long-running regtest node, add
`--rpc --rpcport 12521 --p2p-port 12520`; use `--connect 127.0.0.1:<peer-port>` for
an outbound peer. The wallet CLI and explorer usage are documented in
[docs/PHASE11_RELEASE.md](docs/PHASE11_RELEASE.md).

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
and is not consensus-critical, so it can change without a fork. See [docs/ECONOMICS.md](docs/ECONOMICS.md#unit-naming-and-ticker).

## Licence

MIT. See [LICENSE](LICENSE).
