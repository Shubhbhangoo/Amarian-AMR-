# Windows testnet mining

The Windows GPU miner is deliberately testnet-only. It refuses `mainnet` and `regtest`; it
gets a complete block template from a running node, searches the serialized header on CUDA, and
submits a solved block back over authenticated loopback RPC.

The release build places these files in `build/windows-gpu/src/`:

- `amariand.exe` — node daemon
- `amarian-cli.exe` — RPC command-line client
- `amarian-miner.exe` — CUDA testnet miner
- `amarian-gui.exe` — small testnet launcher panel
- `amarian-wallet-gui.exe` — standalone wallet interface

Start a testnet node from PowerShell:

```powershell
New-Item -ItemType Directory -Force "$env:LOCALAPPDATA\Amarian\testnet" | Out-Null
& .\build\windows-gpu\src\amariand.exe `
  --chain testnet `
  --datadir "$env:LOCALAPPDATA\Amarian\testnet" `
  --rpc --rpcport 12511 --explorer-port 12512 --p2p-port 12510
```

Create the wallet first, then start the node with that wallet:

```powershell
& .\build\windows-gpu\src\amarian-wallet.exe --chain testnet `
  --datadir "$env:LOCALAPPDATA\Amarian\testnet" create
& .\build\windows-gpu\src\amariand.exe `
  --chain testnet `
  --datadir "$env:LOCALAPPDATA\Amarian\testnet" `
  --wallet "$env:LOCALAPPDATA\Amarian\testnet\wallet.dat" `
  --rpc --rpcport 12511 --explorer-port 12512 --p2p-port 12510
```

In a second PowerShell window, start the wallet-backed miner:

```powershell
& .\build\windows-gpu\src\amarian-miner.exe `
  --chain testnet `
  --datadir "$env:LOCALAPPDATA\Amarian\testnet" `
  --rpcport 12511 `
  --wallet "$env:LOCALAPPDATA\Amarian\testnet\wallet.dat" `
  --blocks 1
```

The default testnet target is real proof-of-work, so a block is not expected immediately. Testnet
also has the documented minimum-difficulty escape hatch: if the candidate timestamp is more than
two target intervals after its parent, that block may use `pow_limit_bits`; mainnet never permits
this. During continuous fast mining, ASERT still adjusts normally. Use `--grid` and `--threads`
to tune CUDA occupancy. The miner obtains a fresh payout lock from the connected node wallet and
refuses to mine if the wallet file is missing or the node has no wallet loaded.

To use the panel, run `amarian-gui.exe` beside the other executables. It launches the same
testnet node, wallet, miner, and local explorer (`http://127.0.0.1:12512/explorer/`). Wallet
creation, receive-address generation, balance refresh, and sending are available as panel actions.

For a wallet-only window, run `amarian-wallet-gui.exe`. Start the node separately with the same
wallet file; the wallet window uses authenticated RPC for balance, receive, and send operations.
