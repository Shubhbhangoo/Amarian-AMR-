#!/usr/bin/env python3
"""Phase 3 patch 1: add ASERT half-life params, register consensus/asert.cpp,
and switch chain::NextTargetBits to the ASERT rule."""
import io
import sys

ROOT = "/mnt/e/project Amarian"


def read(path):
    with io.open(path, encoding="utf-8", newline="") as fh:
        return fh.read()


def write(path, text):
    with io.open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)


def replace_once(path, old, new, what):
    text = read(path)
    if old not in text:
        sys.exit(f"PATCH FAILED ({what}): anchor not found in {path}")
    if text.count(old) > 1:
        sys.exit(f"PATCH FAILED ({what}): anchor not unique in {path}")
    write(path, text.replace(old, new, 1))
    print(f"patched {path}: {what}")


# ---- 1. params.hpp: ASERT half-life constant definitions --------------------
params = read(f"{ROOT}/include/amarian/consensus/params.hpp")

anchor_consts = "inline constexpr uint32_t REGTEST_POW_LIMIT_BITS = 0x207FFFFFU;\n"
add_consts = anchor_consts + """
// --- ASERT half-lives --------------------------------------------------------
//
// The half-life of the difficulty retarget, in seconds: the cumulative schedule
// deviation that doubles difficulty. Mainnet's two days is the value the ASERT
// literature and the fielded Bitcoin Cash algorithm converged on; at Amarian's
// 300-second target that is 576 blocks. Testnet's one hour (12 blocks) keeps the
// test network responsive when its hashrate swings, the same choice Bitcoin Cash
// made for its test networks. Regtest never retargets (its difficulty is trivial
// by design), so its half-life is unused and exists only so the field is never
// zero. Chosen with simulation evidence, Phase 3; see DECISIONS.
inline constexpr int64_t MAINNET_ASERT_HALF_LIFE_SECONDS = 172'800;
inline constexpr int64_t TESTNET_ASERT_HALF_LIFE_SECONDS = 3'600;
inline constexpr int64_t REGTEST_ASERT_HALF_LIFE_SECONDS = 3'600;
"""
if "MAINNET_ASERT_HALF_LIFE_SECONDS" not in params:
    if params.count(anchor_consts) != 1:
        sys.exit("params.hpp const anchor not unique")
    params = params.replace(anchor_consts, add_consts, 1)
    print("patched params.hpp: half-life constants")
else:
    print("params.hpp: half-life constants already present")

member_anchor = "    int64_t target_block_seconds;\n    uint32_t coinbase_maturity;\n"
member_add = "    int64_t target_block_seconds;\n\n    /// Seconds per half-life of the ASERT difficulty retarget (see above).\n    /// Part of consensus: every node must compute the same target from the same\n    /// schedule deviation, so this is a per-network constant and never an option.\n    int64_t asert_half_life_seconds;\n    uint32_t coinbase_maturity;\n"
if "int64_t asert_half_life_seconds;" not in params:
    if params.count(member_anchor) != 1:
        sys.exit("params.hpp member anchor not unique")
    params = params.replace(member_anchor, member_add, 1)
    print("patched params.hpp: member field")
else:
    print("params.hpp: member field already present")

init_anchor = ".target_block_seconds = TARGET_BLOCK_SECONDS,\n"
if params.count(init_anchor) != 3:
    sys.exit("params.hpp init anchor count != 3")
params = params.replace(
    init_anchor,
    init_anchor + "    .asert_half_life_seconds = MAINNET_ASERT_HALF_LIFE_SECONDS,\n", 1)
params = params.replace(
    init_anchor,
    init_anchor + "    .asert_half_life_seconds = TESTNET_ASERT_HALF_LIFE_SECONDS,\n", 1)
params = params.replace(
    init_anchor,
    init_anchor + "    .asert_half_life_seconds = REGTEST_ASERT_HALF_LIFE_SECONDS,\n", 1)
print("patched params.hpp: three initializer lists")

assert_anchor = "static_assert(!MAINNET_PARAMS.trivial_difficulty);\n"
assert_add = assert_anchor + "static_assert(MAINNET_PARAMS.asert_half_life_seconds > 0);\nstatic_assert(TESTNET_PARAMS.asert_half_life_seconds > 0);\nstatic_assert(REGTEST_PARAMS.asert_half_life_seconds > 0);\n"
if params.count(assert_anchor) == 1:
    params = params.replace(assert_anchor, assert_add, 1)
    print("patched params.hpp: static asserts")
write(f"{ROOT}/include/amarian/consensus/params.hpp", params)

# ---- 2. src/CMakeLists.txt: register consensus/asert.cpp ---------------------
cmake_anchor = "    consensus/target.cpp\n    consensus/validation.cpp\n"
cmake_add = "    consensus/asert.cpp\n    consensus/target.cpp\n    consensus/validation.cpp\n"
replace_once(f"{ROOT}/src/CMakeLists.txt", cmake_anchor, cmake_add,
             "register consensus/asert.cpp")

# ---- 3. chain/block_index.cpp: include + NextTargetBits body -----------------
bi = read(f"{ROOT}/src/chain/block_index.cpp")

inc_anchor = "#include <amarian/consensus/genesis.hpp>\n"
if bi.count(inc_anchor) != 1:
    sys.exit("block_index.cpp include anchor not unique")
if "#include <amarian/consensus/asert.hpp>" not in bi:
    bi = bi.replace(inc_anchor, inc_anchor + "#include <amarian/consensus/asert.hpp>\n", 1)
    print("patched block_index.cpp: include asert.hpp")

start = bi.index("uint32_t NextTargetBits(const BlockIndexEntry& parent, const ChainParams& params) noexcept {")
end = bi.index("\nconsensus::HeaderContext")
new_fn = """uint32_t NextTargetBits(const BlockIndexEntry& parent, const ChainParams& params) noexcept {
    // Regtest never retargets: its floor is the largest target the encoding admits,
    // a block costs a couple of hash attempts by design, and the point of the network
    // is deterministic on-demand generation. This branch is the explicit statement of
    // what `trivial_difficulty` means, and it is what the old inherit-and-clamp rule
    // did for every network before retargeting existed.
    if (params.trivial_difficulty) {
        return params.pow_limit_bits;
    }

    // ASERT, anchored at genesis and evaluated at the tip: the expected target is the
    // genesis target scaled by 2^(schedule deviation / half-life), where the schedule
    // deviation is how far the tip's header time is from an on-schedule chain. A pure
    // consensus function of facts the index holds; the rule itself clamps to the
    // network floor, so a miner can never be handed, nor a header judged against, a
    // target easier than the network permits.
    return consensus::AsertNextBits(params, parent.height, parent.header.timestamp);
}
"""
bi = bi[:start] + new_fn + bi[end:]
write(f"{ROOT}/src/chain/block_index.cpp", bi)
print("patched block_index.cpp: NextTargetBits body")

# ---- 4. chain/block_index.hpp: doc comment on NextTargetBits -----------------
bh = read(f"{ROOT}/include/amarian/chain/block_index.hpp")
cstart = bh.index("/// The `target_bits` a child of `parent` must carry.")
cend = bh.index("[[nodiscard]] uint32_t NextTargetBits(const BlockIndexEntry& parent,")
new_comment = """/// The `target_bits` a child of `parent` must carry.
///
/// **ASERT**, anchored at genesis and evaluated at the tip. The expected target is
/// the genesis target scaled by `2^(schedule deviation / half-life)`, where the
/// schedule deviation is how far the tip's header time is from where an on-schedule
/// chain would be; `consensus::AsertNextBits` is the pure integer rule and this
/// function supplies it the facts the index holds (the tip's height and header
/// time) and the network's constants. Regtest never retargets: its
/// `trivial_difficulty` branch answers with the network floor, as it always has.
///
/// What matters is the *shape*: the expected target is computed by the node from
/// the chain, and the header's claim is checked against it by
/// `ContextualCheckBlockHeader`. A miner does not get to choose the difficulty they
/// mined at.
"""
bh = bh[:cstart] + new_comment + bh[cend:]
write(f"{ROOT}/include/amarian/chain/block_index.hpp", bh)
print("patched block_index.hpp: NextTargetBits doc")
print("ALL PATCHES OK")
