# Phase 11: release and operator workflow

## Build and install

The release build uses the CMake `release` preset. It produces the node, RPC
client, wallet CLI, genesis checker, and retarget simulator:

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix "$PWD/install"
```

The installed binaries are under `bin/`, with project documentation under
`share/doc/amarian/`.

Packages can be generated from the same build tree:

```sh
cmake --build build/release --target package
```

The current CPack configuration produces a tarball and a Debian package. The
package intentionally does not bundle OpenSSL, RocksDB, Asio, or libsecp256k1;
operators must install the supported system dependencies documented by the
build environment.

For the Windows CUDA testnet bundle, use the repository packaging script from
PowerShell. It installs all Windows executables, copies the CUDA/OpenSSL/zlib
and MSVC runtime dependencies, records `--build-info`, writes SHA-256 checksums,
and creates a ZIP archive:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_windows_testnet.ps1 -Clean
```

The result is `dist/amarian-testnet-windows-x64.zip`. This is a testnet
distribution only; the miner refuses mainnet and regtest.

## Running a node and wallet

Create a wallet and receive address:

```sh
amarian-wallet --chain regtest --wallet ./wallet.dat create
amarian-wallet --chain regtest --wallet ./wallet.dat getnewaddress
```

Run the node with RPC and the read-only explorer listener:

```sh
amariand --chain regtest --datadir ./node \
  --rpc --rpcport 12521 \
  --explorer-port 12522 \
  --wallet ./wallet.dat
```

RPC is authenticated by the per-run `.cookie` file. The explorer listener is
read-only, requires no cookie, and binds only to `127.0.0.1`.

Explorer endpoints include:

```text
GET /explorer/
GET /explorer/status
GET /explorer/tip
GET /explorer/block-at/<height>
GET /explorer/block/<hash>
GET /explorer/mempool
```

## Release reproducibility

Release builds should use a pinned compiler, dependency versions, locale, and
build environment. The binary reports compiler, build type, hardening,
OpenSSL, libsecp256k1, and Git identity through `--build-info`. A dirty tree is
reported explicitly. Release verification should build twice from the same
source and compare normalized artifacts, while treating archive timestamps and
package metadata as controlled inputs rather than silently ignoring differences.

## Operational requirements

- Back up the wallet mnemonic and encrypted wallet file separately.
- Keep node data directories and wallet files on different backup schedules.
- Never expose the RPC port outside loopback without an authenticated proxy.
- The explorer API is read-only but should still be protected by the deployment
  web server if it is made reachable beyond the local machine.
- Before upgrades, stop the node cleanly and retain a copy of the prior binary,
  configuration, wallet, and chainstate.
- Verify `amarian-genesis --check` and `amariand --build-info` as part of release
  validation.

The repeatable end-user acceptance probe is
`scripts/phase11_acceptance.sh`. It verifies installed artifacts, wallet create,
address derivation, mining, RPC balance, explorer HTTP, backup, and restore.
