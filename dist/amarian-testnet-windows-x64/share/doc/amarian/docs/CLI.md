# Amarian CLI, Wallet and Explorer Quick Reference

## amariand — Node Daemon

`amariand [options]`

Starts an Amarian node. Without `--rpc`, it performs the requested actions
(import, generate, export) and exits.

| Option | Description |
|---|---|
| `--chain <network>` | mainnet, testnet, or regtest (default: mainnet) |
| `--datadir <dir>` | Where the chain is kept (default: `$HOME/.amarian/<network>`) |
| `--rpc` | Serve JSON-RPC on loopback until interrupted |
| `--rpcport <port>` | RPC port (default: network-specific) |
| `--wallet <path>` | Open or create a wallet at this path, implies `--rpc` |
| `--wallet-password <pw>` | Password for the wallet database |
| `--payout <hex>` | Hex-encoded lock the mined reward pays to |
| `--generate <n>` | Mine n blocks |
| `--generate-attempts <n>` | Nonces per block before giving up (default: 67108864) |
| `--import-blocks <file>` | Validate and accept blocks from a file |
| `--export-blocks <file>` | Write the active chain to a file |
| `--connect <host:port>` | Connect to a P2P peer (may be repeated) |
| `--log-level <level>` | error, warn, info, debug, trace (default: info) |
| `--log-file <path>` | Append log output to a file |
| `--version` / `--build-info` | Print version or build information |

## amarian-cli — RPC Client

`amarian-cli [options] <method> [arguments...]`

Calls any RPC method on a running `amariand`. Reads the credential from the
node's `.cookie` file, so it works with no configuration when run by the same
account.

| Option | Description |
|---|---|
| `--chain <network>` | Network to connect to |
| `--datadir <dir>` | Where the node's cookie lives |
| `--rpcport <port>` | Override the RPC port |
| `--cookie <file>` | Read credential from a specific file |
| `--timeout <sec>` | RPC timeout (default: 60) |
| `--raw` | Print the raw JSON response envelope |

### RPC Methods

| Method | Arguments | Description |
|---|---|---|
| `getblockchaininfo` | — | Chain state summary |
| `getmininginfo` | — | Mining and target info |
| `getblocktemplate` | `[payout, coinbase_data]` | Template for mining |
| `submitblock` | `block_hex` | Submit a mined block |
| `generate` | `[blocks, payout, attempts]` | CPU-mine blocks (regtest only) |
| `getblockhash` | `height` | Hash of block at height |
| `getblock` | `hash` | Block header info |
| `getmempoolinfo` | — | Mempool summary |
| `getrawmempool` | — | All mempool wtxids |
| `sendrawtransaction` | `tx_hex` | Offer tx to the mempool |
| `getbalance` | — | Wallet balance |
| `getnewaddress` | `[account]` | Generate a new receive address |
| `sendtoaddress` | `address, amount` | Send facets to an address |
| `listtransactions` | — | List wallet transactions |
| `gettransaction` | `txid` | Get transaction details |
| `gettxout` | `txid, vout` | Query UTXO set |
| `help` | — | List all methods |

### Argument Rules

- An argument that parses as JSON is sent as that JSON.
- An argument that does not parse as JSON is sent as a string.
- A hex string of 20+ digits is treated as a string (not a number).

**Byte order:** 32-byte hashes are in reversed display order (`Hash256::ToHex`).
All other hex (blocks, transactions, locks) is in wire order.

## amarian-wallet — Standalone Wallet CLI

`amarian-wallet [options] <command> [arguments...]`

Manages wallet databases. For online commands (balance, send), connects to a
running `amariand` via its RPC interface.

| Option | Description |
|---|---|
| `--wallet <path>` | Wallet database path (default: `<datadir>/wallet.dat`) |
| `--wallet-password <pw>` | Wallet password (default: empty) |
| `--chain <network>` | Network |
| `--rpcport <port>` | Node RPC port (for online commands) |
| `--timeout <sec>` | RPC timeout |

### Wallet Commands

| Command | Arguments | Description |
|---|---|---|
| `create` | — | Create a new wallet, prints mnemonic |
| `getnewaddress` | — | Generate a new receive address |
| `listaddresses` | — | List all derived addresses |
| `getbalance` | — | Show confirmed/pending/total balance |
| `send` | `<address> <amount>` | Send facets, broadcast via RPC |
| `listtransactions` | — | List stored transaction history |
| `backup` | — | Export mnemonic and metadata |
| `restore` | `<mnemonic>` | Restore wallet from mnemonic phrase |

### Examples

```bash
# Create a new wallet
amarian-wallet --chain regtest create

# Generate a new address
amarian-wallet --chain regtest getnewaddress

# Start a node with the wallet
amariand --chain regtest --rpc --wallet /tmp/amarian/regtest/wallet.dat

# Send coins
amarian-wallet --chain regtest send <address> 1000000000
```

## Explorer API

Available on the RPC port. No authentication required (loopback-only).

| Endpoint | Description |
|---|---|
| `GET /explorer/` | Service info and current tip |
| `GET /explorer/tip` | Active tip hash, height, work |
| `GET /explorer/status` | Node status summary |
| `GET /explorer/block/<hash>` | Block header by hash |
| `GET /explorer/block-at/<height>` | Block header at height |
| `GET /explorer/mempool` | Mempool transactions with fee rates |

All endpoints return JSON.