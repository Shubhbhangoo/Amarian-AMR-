#!/usr/bin/env python3
"""Independent recomputation of the Phase 1 golden vectors.

Written from docs/AMARIAN_PROTOCOL.md, not from the C++ source, so that agreement
is evidence rather than a tautology. If this disagrees with the unit tests, one of
the two is wrong and the specification decides which.
"""

import hashlib


def sha256(data: bytes) -> bytes:
    return hashlib.sha256(data).digest()


def tagged_hash(tag: str, message: bytes) -> bytes:
    """BIP-340 construction: SHA256(SHA256(tag) || SHA256(tag) || message)."""
    tag_hash = sha256(tag.encode("ascii"))
    return sha256(tag_hash + tag_hash + message)


def compact_size(value: int) -> bytes:
    """Shortest form only. Non-minimal encodings are not produced."""
    if value < 0xFD:
        return bytes([value])
    if value <= 0xFFFF:
        return b"\xfd" + value.to_bytes(2, "little")
    if value <= 0xFFFFFFFF:
        return b"\xfe" + value.to_bytes(4, "little")
    return b"\xff" + value.to_bytes(8, "little")


def merkle_root(wtxids: list[bytes]) -> bytes | None:
    if not wtxids:
        return None

    level = [tagged_hash("Amarian/MerkleLeaf", wtxid) for wtxid in wtxids]

    while len(level) > 1:
        nxt = []
        for i in range(0, len(level) - 1, 2):
            nxt.append(tagged_hash("Amarian/MerkleBranch", level[i] + level[i + 1]))
        if len(level) % 2 != 0:
            # Promoted unchanged, never duplicated (CVE-2012-2459).
            nxt.append(level[-1])
        level = nxt

    preimage = compact_size(len(wtxids)) + level[0]
    return tagged_hash("Amarian/MerkleRoot", preimage)


def test_ids(count: int) -> list[bytes]:
    """Matches TestIds() in tests/unit/primitives_merkle_test.cpp: id `i` is 32
    bytes each equal to `i`."""
    return [bytes([i]) * 32 for i in range(count)]


EXPECTED = {
    1: "12ffa1211615a6a0f7e887ef8a281c8a160da0bf88da67b73cc3304db0211848",
    2: "3bbf1c68180c0302be5e8d0a54f80bc487e9d6dfb999c8291068cadec45c929f",
    3: "8bf1e0bcb8ba9c658be180d5428f342c3840a68f53080629300e6f41edb4e4a1",
}


def main() -> int:
    failures = 0

    # Hash constructions. The four SHA-256 rows are the published NIST/Bitcoin
    # vectors; the tagged row is recomputed from the BIP-340 construction.
    print("--- hash constructions ---")
    hash_cases = [
        ("sha256('')", sha256(b"").hex(),
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        ("sha256('abc')", sha256(b"abc").hex(),
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        ("sha256d('')", sha256(sha256(b"")).hex(),
         "5df6e0e2761359d30a8275058e299fcc0381534545f55cf43e41983f5d4c9456"),
        ("sha256d('abc')", sha256(sha256(b"abc")).hex(),
         "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358"),
        ("tagged(MerkleLeaf, 00010203)",
         tagged_hash("Amarian/MerkleLeaf", bytes([0, 1, 2, 3])).hex(),
         "c5d359e1f85809f86d88386fb2b0ee3b2191b77986ca5f5531918b579a725fcd"),
    ]
    for label, got, expected in hash_cases:
        ok = got == expected
        failures += 0 if ok else 1
        print(f"  {label:32} {got}  {'OK' if ok else 'MISMATCH expected ' + expected}")

    print("--- merkle roots (internal byte order) ---")
    for count, expected in EXPECTED.items():
        root = merkle_root(test_ids(count))
        assert root is not None
        got = root.hex()
        ok = got == expected
        failures += 0 if ok else 1
        print(f"  n={count}  {got}  {'OK' if ok else 'MISMATCH expected ' + expected}")

    print("--- empty tree ---")
    empty_ok = merkle_root([]) is None
    failures += 0 if empty_ok else 1
    print(f"  none: {'OK' if empty_ok else 'MISMATCH'}")

    # The CVE-2012-2459 property, checked independently of the C++ test.
    print("--- odd-node promotion is not duplication ---")
    three = merkle_root(test_ids(3))
    four = merkle_root(test_ids(3) + [test_ids(3)[-1]])
    distinct = three != four
    failures += 0 if distinct else 1
    print(f"  root(3) != root(3 + dup last): {'OK' if distinct else 'MISMATCH'}")

    # Leaf-count commitment: a one-leaf root must not equal its bare leaf hash.
    print("--- leaf count is committed ---")
    bare_leaf = tagged_hash("Amarian/MerkleLeaf", test_ids(1)[0])
    one = merkle_root(test_ids(1))
    not_bare = one != bare_leaf
    failures += 0 if not_bare else 1
    print(f"  root(1) != leaf(1): {'OK' if not_bare else 'MISMATCH'}")

    print(f"--- {'ALL AGREE' if failures == 0 else str(failures) + ' MISMATCH(ES)'} ---")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
