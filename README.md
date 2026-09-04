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

Amarian is in **Phase 0 of 13** — environment, build system, and project
foundations. There is no chain yet.

What actually works today, verified by the test suite:

| Component | State |
|---|---|
| Hardened C++23 build (GCC 15 / Clang 21, CMake + Ninja) | working |
| Sanitizer, fuzzing and release presets | working |
| `util`: byte/hash types, strict hex codec, checked arithmetic, logging, CLI options | working |
| `amariand --version` / `--build-info` / `--help` | working |
| Consensus rules, blocks, transactions, UTXO set | **not started** (Phase 1) |
| Mining and difficulty adjustment | **not started** (Phase 3) |
| P2P networking | **not started** (Phase 4) |
| Wallet | **not started** (Phase 5) |
| Post-quantum signatures | **not started** (Phase 6) |

`amariand` currently exits non-zero and says so, rather than pretending to
start a node. See [DEVELOPMENT_STATUS.md](DEVELOPMENT_STATUS.md) for the
current task, the next task, and open risks.

Claims this project does **not** make yet, and will not make until there is
evidence: that it is decentralised, that it is post-quantum secure, or that any
performance number has been measured.

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
