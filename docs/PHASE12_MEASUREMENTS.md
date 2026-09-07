# Phase 12 measurements

Phase 12 is evidence, not an optimisation target. The project measures the
cost of validating and storing a chain on ordinary hardware before changing a
consensus limit or weakening a check. The measurement harness is
[`scripts/phase12_measure.sh`](../scripts/phase12_measure.sh).

It creates a controlled regtest chain, exports the serialized blocks, and
imports those exact bytes into a fresh node. The import is the initial-block-
download proxy: it exercises block decoding, contextual validation, chain
activation, and durable storage. The report includes elapsed time, CPU time,
peak resident memory, serialized bytes, chainstate bytes, and the resulting
import throughput.

Run it from WSL at the repository root:

```bash
scripts/phase12_measure.sh 100
```

The script prints a Markdown report. Numbers are machine-specific and must be
copied here only with the date, build, compiler, and hardware preserved.

## Controlled regtest run

Run on 2026-09-06 UTC under WSL2 on the local x86-64 host, using the hardened
`build/dev` executable and 100 regtest blocks. This is the current rerun from
the completed Phase 12 harness:

| operation | wall seconds | user seconds | system seconds | max RSS KiB |
|---|---:|---:|---:|---:|
| generate | 0.09 | 0.02 | 0.04 | 21,544 |
| export | 0.06 | 0.01 | 0.02 | 21,404 |
| import | 0.06 | 0.02 | 0.03 | 21,576 |

The exported block file was 19,400 bytes and the fresh destination chainstate
was 174,335 bytes. The serialized-file/import-time proxy throughput was 0.308
MiB/s. This is a small-regtest baseline, not a mainnet performance claim.

The earlier run was 0.370 MiB/s; the difference is ordinary host/storage
variance at this small workload, not a consensus or protocol change.

## Compiler comparison

The hardened Release benchmark presets were also built on the same WSL2 host
with GCC 15.2 and Clang 21.1.8. The Google Benchmark support library in this
environment reports itself as a DEBUG build, so these numbers are useful for
comparison and regression detection but are not release-grade absolute claims.

| benchmark | GCC 15.2 | Clang 21.1.8 |
|---|---:|---:|
| `BM_ToHex/4096` | 6.295 us | 4.949 us |
| `BM_FromHex/4096` | 16.090 us | 30.080 us |
| `BM_SumChecked/20000` | 228.512 us | 10.510 us |
| `BM_HeaderRoundTrip` | 10.4 ns | 76.4 ns |
| `BM_ByteStringRoundTrip/2420` | 201 ns | 209 ns |

The large checked-arithmetic and header-round-trip differences are exactly why
the project keeps both compiler presets: a single compiler's timing is not a
portable fact about the implementation. No optimisation was made from these
numbers alone.

The comparison build also found and fixed portability defects that GCC did not
diagnose under the same `-Werror` policy: non-UTF-8 source comments, an
unused wallet sizing constant, an unused sync parameter member, and an unused
peer nonce member. These were source-quality fixes, not consensus changes.

## P2P serialization baseline

`bench_net` now measures the protocol layer without pretending that socket
transport is already operational. The same WSL2 host and benchmark-library
caveat apply.

| benchmark | GCC 15.2 | Clang 21.1.8 |
|---|---:|---:|
| `SerialiseMessage/4096` | 3.477 us (1.097 GiB/s) | 3.395 us (1.124 GiB/s) |
| `FrameMessage/4096` | 3.377 us (1.130 GiB/s) | 3.581 us (1.065 GiB/s) |
| `SerialiseMessage/1048576` | 750.368 us (1.301 GiB/s) | 774.023 us (1.262 GiB/s) |
| `FrameMessage/1048576` | 784.586 us (1.245 GiB/s) | 793.291 us (1.231 GiB/s) |
| `HelloPayloadRoundTrip` | 106 ns (9.40M/s) | 164 ns (6.09M/s) |

These are CPU-only protocol costs. They are not peer bandwidth, propagation
latency, or a claim that a node can sustain those rates over a real connection.

## Full live measurement run

The full harness is [`scripts/phase12_full_measure.sh`](../scripts/phase12_full_measure.sh).
It measures cold import/storage at three chain sizes, then creates a real
two-node P2P sync and a three-node mesh. Transport counters are counted at the
socket write/read boundary, including protocol framing, and are flushed by the
daemon on clean shutdown.

Run on 2026-09-07 UTC under WSL2 on the local x86-64 host, using the hardened
`build/dev` executable:

| blocks | serialized bytes | chainstate bytes | generate wall s | generate RSS KiB | import wall s |
|---:|---:|---:|---:|---:|---:|
| 100 | 19,400 | 175,271 | 0.09 | 21,472 | 0.05 |
| 500 | 97,000 | 316,668 | 0.07 | 21,680 | 0.09 |
| 1,000 | 194,000 | 494,580 | 0.10 | 22,136 | 0.11 |

The live two-node run downloaded and validated 25 blocks in 2,093 ms and both
nodes ended at the same tip. The three-node mesh converged at height 25 in
1,154 ms. The measured socket traffic for that run was:

| node | bytes sent | bytes received |
|---|---:|---:|
| A | 33,457 | 5,750 |
| B | 41,983 | 39,399 |
| C | 5,942 | 36,233 |

These are local-regtest measurements, not mainnet capacity claims. They include
handshake, headers, inventory, requests, blocks, and keepalive traffic, so the
sync elapsed time and byte totals should be reported together rather than
converted into a misleading single bandwidth figure.

## Phase 12 conclusion

Phase 12 is complete as a reproducible baseline. The project now has measured
validation/storage cost, cold import as an IBD baseline, actual live P2P sync
traffic, resource usage, and multi-node convergence. No consensus or security
check was weakened for performance.

The remaining work is Phase 13 mainnet readiness: external review, long-running
testnet evidence, upgrade and recovery procedures, signed releases, monitoring,
and final consensus/network freeze.
