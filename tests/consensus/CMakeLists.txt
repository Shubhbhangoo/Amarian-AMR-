# Phase 2's deliverable: the attempts to break the supply cap, each of which must fail.
#
# Links amarian::utxo rather than amarian::consensus because an inflation attempt is only
# meaningful against a set of coins — the rule that rejects a coinbase claiming fees needs
# the fees, and those come from what the block actually spent.
amarian_add_test(test_supply consensus
    supply_test.cpp
)
target_link_libraries(test_supply PRIVATE amarian::utxo)
