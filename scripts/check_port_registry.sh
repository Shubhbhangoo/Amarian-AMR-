#!/usr/bin/env bash
# Find TCP port numbers that IANA has not assigned, so Amarian's default ports can
# be chosen against the registry rather than against intuition.
#
# docs/AMARIAN_PROTOCOL.md marks the default P2P port "not chosen" and says why:
# "A port number and an address prefix need a check against what other projects
# already use, and picking them now to fill a table is how collisions happen."
# This is that check.
#
# Output: for each candidate, whether the registry lists a TCP assignment; then
# the longest runs of consecutive unassigned TCP ports in a given window, so a
# contiguous block can be reserved for P2P and RPC across three networks.
set -uo pipefail

CACHE="/mnt/c/Users/Shubhkarman/AppData/Local/Temp/amarian-iana-ports.csv"
URL="https://www.iana.org/assignments/service-names-port-numbers/service-names-port-numbers.csv"

if [[ ! -s "$CACHE" ]]; then
    echo "fetching IANA registry..."
    curl -fsSL --retry 3 -o "$CACHE" "$URL" || { echo "FETCH FAILED"; exit 1; }
fi
echo "registry: $(wc -l <"$CACHE") rows, $(du -h "$CACHE" | cut -f1)"

python3 - "$CACHE" <<'PY'
import csv, sys

assigned = {}
with open(sys.argv[1], newline="", encoding="utf-8", errors="replace") as handle:
    for row in csv.DictReader(handle):
        if (row.get("Transport Protocol") or "").strip().lower() != "tcp":
            continue
        port_field = (row.get("Port Number") or "").strip()
        if not port_field:
            continue
        name = (row.get("Service Name") or "").strip()
        desc = (row.get("Description") or "").strip()
        # A row with neither a name nor a description is a reservation placeholder,
        # not a service; treat only named or described rows as taken.
        taken = bool(name) or bool(desc and desc.lower() != "unassigned")
        if not taken:
            continue
        try:
            if "-" in port_field:
                low, high = (int(part) for part in port_field.split("-", 1))
            else:
                low = high = int(port_field)
        except ValueError:
            continue
        for port in range(low, min(high, 65535) + 1):
            assigned.setdefault(port, name or desc)

print(f"tcp ports with an assignment: {len(assigned)}")

# De-facto blockchain ports the registry does not know about. Collisions here are
# just as real as registry collisions: two daemons on one host, or a firewall rule
# written for the wrong chain.
DE_FACTO = {
    8333: "Bitcoin mainnet p2p", 18333: "Bitcoin testnet p2p",
    38333: "Bitcoin signet p2p", 18444: "Bitcoin regtest p2p",
    8332: "Bitcoin mainnet rpc", 18332: "Bitcoin testnet rpc",
    9333: "Litecoin p2p", 19335: "Litecoin testnet p2p", 9332: "Litecoin rpc",
    30303: "Ethereum devp2p", 8545: "Ethereum json-rpc", 8546: "Ethereum ws-rpc",
    30333: "Substrate p2p", 9944: "Substrate ws-rpc", 9933: "Substrate rpc",
    18080: "Monero p2p", 18081: "Monero rpc", 28080: "Monero testnet p2p",
    8233: "Zcash p2p", 8232: "Zcash rpc", 18233: "Zcash testnet p2p",
    9999: "Dash p2p", 9998: "Dash rpc", 19999: "Dash testnet p2p",
    22556: "Dogecoin p2p", 22555: "Dogecoin rpc",
    26656: "Tendermint p2p", 26657: "Tendermint rpc",
    4001: "IPFS swarm", 5001: "IPFS api",
    8815: "Nano", 7075: "Nano p2p", 7076: "Nano rpc",
    9735: "Lightning p2p", 10009: "lnd grpc", 8080: "common http alt",
    3000: "common dev http", 5000: "common dev http", 1337: "common dev",
}

CANDIDATES = [8433, 8533, 8633, 9633, 11055, 13755, 14733, 21055, 23755,
              24733, 27055, 31055, 33055, 37055, 41055, 43755, 44733]
print("\ncandidate  status")
for port in CANDIDATES:
    marks = []
    if port in assigned:
        marks.append(f"IANA:{assigned[port]}")
    if port in DE_FACTO:
        marks.append(f"de-facto:{DE_FACTO[port]}")
    print(f"  {port:<7}  {'; '.join(marks) if marks else 'free'}")

# Mnemonic candidates, and the exact state of the band around 8400 — the ideal
# supply cap is 8 400 000 AMR, which would make 8400 the one memorable number
# this project actually owns.
print("\nband 8395-8425:")
for port in range(8395, 8426):
    state = assigned.get(port)
    print(f"  {port}  {state if state else ('de-facto:' + DE_FACTO[port] if port in DE_FACTO else 'free')}")

print("\nmnemonic candidates (need base..base+21 all free, and base+21 < 32768):")
for base in (8400, 10500, 11050, 11250, 12500, 13000, 14000, 15750, 16800,
             21000, 23000, 25000, 29000, 31000):
    blocked = [port for port in range(base, base + 22)
               if port in assigned or port in DE_FACTO]
    verdict = "ALL FREE" if not blocked else f"blocked at {blocked[:6]}"
    print(f"  {base}-{base + 21}: {verdict}")

def runs(low, high, want):
    """Runs of >= want consecutive ports free in both the registry and the
    de-facto list."""
    found, start = [], None
    for port in range(low, high + 1):
        free = port not in assigned and port not in DE_FACTO
        if free and start is None:
            start = port
        elif not free and start is not None:
            if port - start >= want:
                found.append((start, port - 1))
            start = None
    if start is not None and high + 1 - start >= want:
        found.append((start, high))
    return found

for low, high in ((8400, 9000), (11000, 12000), (27000, 28000), (43000, 44000)):
    blocks = runs(low, high, 8)
    print(f"\nfree runs of >=8 in {low}-{high}: {len(blocks)}")
    for start, end in blocks[:8]:
        print(f"  {start}-{end}  ({end - start + 1} ports)")
PY
