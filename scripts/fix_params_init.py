#!/usr/bin/env python3
"""Fix the botched designated-initializer insertions in params.hpp."""
import io
import sys

path = "/mnt/e/project Amarian/include/amarian/consensus/params.hpp"
with io.open(path, encoding="utf-8", newline="") as fh:
    text = fh.read()

old_main = """    .target_block_seconds = TARGET_BLOCK_SECONDS,
    .asert_half_life_seconds = REGTEST_ASERT_HALF_LIFE_SECONDS,
    .asert_half_life_seconds = TESTNET_ASERT_HALF_LIFE_SECONDS,
    .asert_half_life_seconds = MAINNET_ASERT_HALF_LIFE_SECONDS,
"""
new_main = """    .target_block_seconds = TARGET_BLOCK_SECONDS,
    .asert_half_life_seconds = MAINNET_ASERT_HALF_LIFE_SECONDS,
"""
if text.count(old_main) != 1:
    sys.exit(f"mainnet block not found exactly once ({text.count(old_main)})")
text = text.replace(old_main, new_main, 1)

testnet_anchor = "    .target_block_seconds = TARGET_BLOCK_SECONDS,\n    .coinbase_maturity = COINBASE_MATURITY,\n"
testnet_new = "    .target_block_seconds = TARGET_BLOCK_SECONDS,\n    .asert_half_life_seconds = TESTNET_ASERT_HALF_LIFE_SECONDS,\n    .coinbase_maturity = COINBASE_MATURITY,\n"
if text.count(testnet_anchor) != 1:
    sys.exit(f"testnet anchor not found exactly once ({text.count(testnet_anchor)})")
text = text.replace(testnet_anchor, testnet_new, 1)

regtest_anchor = "    .target_block_seconds = TARGET_BLOCK_SECONDS,\n    .coinbase_maturity = 20,\n"
regtest_new = "    .target_block_seconds = TARGET_BLOCK_SECONDS,\n    .asert_half_life_seconds = REGTEST_ASERT_HALF_LIFE_SECONDS,\n    .coinbase_maturity = 20,\n"
if text.count(regtest_anchor) != 1:
    sys.exit(f"regtest anchor not found exactly once ({text.count(regtest_anchor)})")
text = text.replace(regtest_anchor, regtest_new, 1)

with io.open(path, "w", encoding="utf-8", newline="") as fh:
    fh.write(text)
print("params.hpp initializer lists fixed")
