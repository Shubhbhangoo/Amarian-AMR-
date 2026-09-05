#pragma once

/// \file
/// Per-network consensus parameters.
///
/// A `ChainParams` is the complete set of constants that decide what a node
/// considers valid: the identity of the chain, the bounds a block must respect, the
/// issuance schedule, and the two values that keep networks apart. It is a value
/// passed to validation explicitly, not a global. There is no ambient "current
/// network" that a code path could read and be wrong about.
///
/// Everything here is a consensus rule. Relay policy, fee floors, connection
/// counts, and cache sizes are node configuration and live elsewhere — the
/// distinction matters because a node may change the second set freely and can
/// never change the first without leaving the network.
///
/// ## Where these numbers come from
///
/// The identity values are derived, not invented: `chain_id` is a BIP-340 style
/// tagged hash of the network's name, and the wire magic is its first four bytes
/// with the top bit of the first byte set. Both are written out as literals so that
/// consensus reads a constant rather than calling into the hash layer during
/// startup, and `ChainIdMatchesItsDerivation` in the consensus tests recomputes
/// them so the literals cannot drift from the rule that produced them.
///
/// The resource bounds are derived from `MAX_BLOCK_WEIGHT` and the smallest legal
/// encoding of each item, so a limit cannot be larger than what could physically
/// appear in a block. Hard-coding them separately would let the two disagree.

#include <amarian/consensus/issuance.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/util/types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace amarian {

/// The three networks. This numbering is never serialised: identity on the wire is
/// the magic, and identity inside a signature is `chain_id`.
enum class Network : uint8_t { Mainnet, Testnet, Regtest };

/// The four bytes that prefix every P2P message.
using NetworkMagic = std::array<uint8_t, 4>;

// --- Values every network shares --------------------------------------------

/// Maximum block weight, where `weight = base_size * 4 + witness_size`.
///
/// Provisional, and marked so in AMARIAN_PROTOCOL.md: it depends on
/// initial-block-download, storage-growth, and bandwidth measurements that are
/// Phase 12 work. It is the same on every network because a test network that
/// admits blocks mainnet would reject cannot test mainnet.
inline constexpr size_t MAX_BLOCK_WEIGHT = 2'000'000;

/// Target seconds between blocks.
///
/// Settled. The issuance schedule's eras are counted in blocks, so this constant is
/// what converts the schedule into a rate: changing it changes the monetary policy
/// even though it is not a monetary parameter.
inline constexpr int64_t TARGET_BLOCK_SECONDS = 300;

/// Blocks a coinbase output must age before it may be spent.
inline constexpr uint32_t COINBASE_MATURITY = 200;

/// How far ahead of a node's own clock a block's timestamp may be.
///
/// One hour, which is twelve block intervals — the same ratio Bitcoin's two-hour
/// allowance has to its ten-minute target. The ratio is what matters rather than the
/// wall-clock figure: the allowance is how many blocks' worth of timestamp a miner can
/// push into the future, and therefore how much room there is to influence a
/// difficulty calculation. It is still far above any clock skew a node with a
/// functioning NTP client will see, and a node that rejects a block for this reason
/// must reconsider it later rather than remember it as invalid, since the same block
/// becomes acceptable as the clock advances.
inline constexpr int64_t MAX_FUTURE_BLOCK_SECONDS = 3'600;

/// Blocks in the median-time-past window.
///
/// A block's timestamp must be strictly greater than the median of this many
/// predecessors, which is what stops a miner from stalling or rewinding the chain's
/// notion of time. Odd, so the median is an element rather than an average of two, and
/// eleven for the same reason Bitcoin uses eleven: long enough that a single miner
/// cannot move the median, short enough that the window is cheap to keep.
inline constexpr size_t MEDIAN_TIME_SPAN = 11;

/// Keys in one spend condition: `1..=16`, per AMARIAN_PROTOCOL.md.
inline constexpr size_t MAX_SPEND_CONDITION_KEYS = 16;

/// Bytes in a lock program. Version 1 is a 32-byte commitment; the bound is larger
/// so that a future `lock_version` with a wider commitment does not need a change
/// to a decoding limit, which is the kind of change that splits a network.
inline constexpr size_t MAX_LOCK_PROGRAM_BYTES = 128;

/// Bytes in one public key. The largest key any currently intended scheme uses is
/// ML-DSA-87's at 2 592 bytes; this leaves room above it. Exact per-scheme sizes are
/// the crypto layer's rule — this is only the bound that stops a declared length
/// from being allocated.
inline constexpr size_t MAX_PUBLIC_KEY_BYTES = 4'096;

/// Bytes in one signature. SLH-DSA-SHA2-128s is the largest at 7 856 bytes, which is
/// what sets this bound; ML-DSA-87 is 4 627.
inline constexpr size_t MAX_SIGNATURE_BYTES = 8'192;

/// Bytes of arbitrary data in a coinbase.
///
/// This is space every node stores forever and nobody can verify the meaning of, so
/// it is bounded tightly: enough for an extranonce, a miner's identifying tag, and
/// genesis's timestamped reference, and not enough to be a data-storage layer. 100 is
/// Bitcoin's limit for the same field and has held up in practice.
inline constexpr size_t MAX_COINBASE_DATA_BYTES = 100;

/// How many items of `min_base_bytes` each could fit in one block at most.
///
/// Every count limit is expressed this way rather than as a chosen number, so that
/// a limit is provably reachable-but-not-exceedable given the weight cap. If
/// `MAX_BLOCK_WEIGHT` changes, these follow it.
[[nodiscard]] constexpr size_t WeightBoundedCount(size_t min_base_bytes) noexcept {
    return MAX_BLOCK_WEIGHT / (WITNESS_SCALE_FACTOR * min_base_bytes);
}

/// The decoding bounds consensus uses for any transaction in a block.
inline constexpr TxLimits CONSENSUS_TX_LIMITS{
    .max_inputs = WeightBoundedCount(TxInput::SERIALIZED_SIZE),
    .max_outputs = WeightBoundedCount(TxOutput::MIN_SERIALIZED_SIZE),
    // Consensus requires one witness per input; the decode bound is the same
    // because a witness cannot be cheaper than an input to carry.
    .max_witnesses = WeightBoundedCount(TxInput::SERIALIZED_SIZE),
    .max_lock_program_size = MAX_LOCK_PROGRAM_BYTES,
    .max_keys = MAX_SPEND_CONDITION_KEYS,
    .max_key_size = MAX_PUBLIC_KEY_BYTES,
    // At most one signature per key, since `threshold <= keys`.
    .max_signatures = MAX_SPEND_CONDITION_KEYS,
    .max_signature_size = MAX_SIGNATURE_BYTES,
    .max_coinbase_data_size = MAX_COINBASE_DATA_BYTES,
};

/// The decoding bounds consensus uses for a block.
inline constexpr BlockLimits CONSENSUS_BLOCK_LIMITS{
    .max_transactions = WeightBoundedCount(Transaction::MIN_SERIALIZED_SIZE),
    .tx = CONSENSUS_TX_LIMITS,
};

// --- The parameter set ------------------------------------------------------

/// Everything that distinguishes one Amarian network from another.
struct ChainParams {
    Network network;

    /// Lowercase, stable, and used in configuration, logs, and data directory
    /// names. It is also the message the `chain_id` is derived from, so it is part
    /// of consensus by way of that derivation and cannot be renamed casually.
    std::string_view name;

    /// Domain separator mixed into every signature hash.
    ///
    /// This is the strong form of replay protection: network magic only stops nodes
    /// from connecting, while a distinct `chain_id` makes a signature produced for
    /// one network arithmetically invalid on another, even if a transaction is
    /// carried across by hand.
    Hash256 chain_id;

    /// Prefix on every P2P message. Distinct in its first byte across networks, and
    /// with that byte's top bit set, so a mis-dialled peer or a line of text
    /// arriving on the port fails at byte zero rather than being partly parsed.
    NetworkMagic magic;

    uint16_t default_p2p_port;
    uint16_t default_rpc_port;

    /// Easiest permitted target, compact-encoded: no block may claim a target
    /// weaker than this, and retargeting is clamped to it.
    uint32_t pow_limit_bits;

    /// Genesis `target_bits`. Separate from `pow_limit_bits` because a network may
    /// want to start harder than its floor.
    uint32_t genesis_bits;

    /// Genesis's header `timestamp` and `nonce`, and the hash the whole block
    /// therefore has. The hash is stored rather than derived so that startup can
    /// check the parameter table against a value a reader can compare with a
    /// published one — a table that computed its own expected answer would agree
    /// with itself no matter what it contained.
    int64_t genesis_timestamp;
    uint64_t genesis_nonce;
    Hash256 genesis_hash;

    int64_t target_block_seconds;

    /// Seconds per half-life of the ASERT difficulty retarget (see above).
    /// Part of consensus: every node must compute the same target from the same
    /// schedule deviation, so this is a per-network constant and never an option.
    int64_t asert_half_life_seconds;
    uint32_t coinbase_maturity;
    size_t max_block_weight;
    BlockLimits block_limits;
    IssuanceParams issuance;

    /// Whether a block may claim the minimum difficulty after a long gap. True on
    /// testnet only: it keeps a test network from stalling when its hashrate
    /// vanishes, and it is exactly the property mainnet must not have.
    bool allow_min_difficulty_blocks;

    /// Whether blocks are produced on demand rather than by sustained work. Regtest
    /// only: its target floor is high enough that a block costs a couple of hash
    /// attempts, which is also why regtest must never be reachable from a public
    /// network.
    bool trivial_difficulty;
};

// --- Chain identity ---------------------------------------------------------
//
// chain_id = TaggedHash("Amarian/chain-id", name), i.e.
//   SHA256(SHA256(tag) || SHA256(tag) || name)
// written out as a literal so that consensus reads a constant. The consensus test
// `Params.ChainIdIsTheTaggedHashOfTheName` recomputes each of these through the
// crypto layer, so a typo here fails a test rather than silently creating a network
// whose signatures nobody else can verify.

inline constexpr Hash256 MAINNET_CHAIN_ID{std::array<uint8_t, Hash256::SIZE>{
    0x4C, 0x6C, 0x27, 0xEC, 0x0A, 0xA1, 0xB5, 0x9E, 0x12, 0x5E, 0x4B,
    0x9D, 0xAE, 0x51, 0x55, 0x99, 0x30, 0xEF, 0xDE, 0xC0, 0x1A, 0xD1,
    0xA6, 0xE0, 0x35, 0x0F, 0x66, 0x76, 0x9D, 0x6A, 0x8B, 0xB9}};

inline constexpr Hash256 TESTNET_CHAIN_ID{std::array<uint8_t, Hash256::SIZE>{
    0xE2, 0x9A, 0x85, 0xD4, 0x36, 0x91, 0x6C, 0xF8, 0x6E, 0x92, 0x07,
    0x4F, 0x76, 0xF3, 0x0B, 0x4A, 0x7D, 0x7F, 0xAA, 0x5B, 0x07, 0x4C,
    0xBA, 0xDE, 0xB5, 0x9D, 0xA8, 0x28, 0xA5, 0xF4, 0x67, 0xFE}};

inline constexpr Hash256 REGTEST_CHAIN_ID{std::array<uint8_t, Hash256::SIZE>{
    0x75, 0xB9, 0xD4, 0xC0, 0x43, 0xF1, 0xDD, 0x98, 0x8B, 0x02, 0x03,
    0x5A, 0x28, 0xC6, 0xA2, 0x26, 0x74, 0x1C, 0x9F, 0x91, 0xEF, 0xC3,
    0x5B, 0xD3, 0xCD, 0xE7, 0x65, 0x53, 0xE0, 0x1F, 0xA5, 0xCC}};

/// The wire magic for a chain id: its first four bytes, with the top bit of the
/// first byte set.
///
/// Setting that bit costs nothing and buys two things: the magic can never be
/// printable ASCII, so an HTTP request or a stray line of text on the P2P port is
/// rejected at the first byte; and it is not a bare prefix of a published hash,
/// so the magic cannot be mistaken for a truncated chain id.
[[nodiscard]] constexpr NetworkMagic MagicFromChainId(const Hash256& chain_id) noexcept {
    const std::array<uint8_t, Hash256::SIZE>& bytes = chain_id.Array();
    return NetworkMagic{static_cast<uint8_t>(bytes[0] | 0x80U), bytes[1], bytes[2], bytes[3]};
}

// --- Default ports ----------------------------------------------------------
//
// 12500 is the base because 12.5% is Amarian's per-era reward reduction — the one
// number this monetary policy actually owns. The band 12500-12521 was checked
// against the IANA service-name and port-number registry and against the de-facto
// ports of the major chains (scripts/check_port_registry.sh); it is unassigned in
// both. It is also below 32768, which keeps it out of Linux's default ephemeral
// range, where a default listening port can lose a bind race against an outbound
// connection's source port.
//
// Ten apart per network so that RPC sits beside its P2P port and there is room to
// add a port to a network later without renumbering another one.

inline constexpr uint16_t MAINNET_P2P_PORT = 12500;
inline constexpr uint16_t MAINNET_RPC_PORT = 12501;
inline constexpr uint16_t TESTNET_P2P_PORT = 12510;
inline constexpr uint16_t TESTNET_RPC_PORT = 12511;
inline constexpr uint16_t REGTEST_P2P_PORT = 12520;
inline constexpr uint16_t REGTEST_RPC_PORT = 12521;

// --- Proof-of-work floors ---------------------------------------------------

/// Mainnet and testnet floor: about 2^32 expected hashes per block.
///
/// The same value Bitcoin uses, kept deliberately so that difficulty intuition and
/// existing tooling transfer, and because it is a launch difficulty a single CPU can
/// meet in minutes rather than one that either stalls the chain or invites a trivial
/// early reorganisation.
inline constexpr uint32_t POW_LIMIT_BITS = 0x1D00FFFFU;

/// Regtest floor: the largest target the compact encoding admits, roughly 2^255. A
/// little over half of all hashes are work at this target, so a block is one or two
/// attempts away and tests do not spend their time mining.
inline constexpr uint32_t REGTEST_POW_LIMIT_BITS = 0x207FFFFFU;

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

// --- Genesis ----------------------------------------------------------------
//
// Genesis is a chain parameter, not a computed value: its hash is checked at startup
// so that a node built with a corrupted parameter table stops instead of forking
// silently. The block itself is rebuilt from these fields by BuildGenesisBlock in
// consensus/genesis.hpp, so there is one description of it and not two.
//
// The three networks were generated together at GENESIS_TIMESTAMP, and each one's
// coinbase carries the same public reference — dated, published, and impossible to
// have known in advance. What makes the three blocks distinct is the coinbase's
// unspendable output, whose lock program is the network's own chain id: a genesis
// block from one network is therefore not the genesis block of another, and the
// difference is inside the Merkle root that the header commits to.

/// 2026-09-05T09:00:00Z, the moment the three genesis blocks were generated.
inline constexpr int64_t GENESIS_TIMESTAMP = 1'788'598'800;

// The hashes below are in internal byte order, which is the reverse of the order they
// are displayed in. Each was produced by `amarian-genesis --mine`, which searches
// upward from nonce 0 and reports the smallest nonce that satisfies the target, so the
// answer does not depend on how many cores the search ran on and any reviewer can
// reproduce it. `CheckGenesis` recomputes the block from these fields at startup and
// refuses to run if it disagrees, and the four genesis tests do the same in the build.

/// Mainnet: `000000001872d649f36e25be94d2f562f06bc243f582af561f1ffe1ab376662f`.
inline constexpr Hash256 MAINNET_GENESIS_HASH{std::array<uint8_t, Hash256::SIZE>{
    0x2F, 0x66, 0x76, 0xB3, 0x1A, 0xFE, 0x1F, 0x1F, 0x56, 0xAF, 0x82, 0xF5, 0x43, 0xC2, 0x6B, 0xF0,
    0x62, 0xF5, 0xD2, 0x94, 0xBE, 0x25, 0x6E, 0xF3, 0x49, 0xD6, 0x72, 0x18, 0x00, 0x00, 0x00, 0x00}};
inline constexpr uint64_t MAINNET_GENESIS_NONCE = 570'575'708;

/// Testnet: `0000000037c5c1182536c905bb2e23931e3393e476f806405045c2d5332876c0`.
inline constexpr Hash256 TESTNET_GENESIS_HASH{std::array<uint8_t, Hash256::SIZE>{
    0xC0, 0x76, 0x28, 0x33, 0xD5, 0xC2, 0x45, 0x50, 0x40, 0x06, 0xF8, 0x76, 0xE4, 0x93, 0x33, 0x1E,
    0x93, 0x23, 0x2E, 0xBB, 0x05, 0xC9, 0x36, 0x25, 0x18, 0xC1, 0xC5, 0x37, 0x00, 0x00, 0x00, 0x00}};
inline constexpr uint64_t TESTNET_GENESIS_NONCE = 2'119'016'593;

/// Regtest: `02248d2fa761c196fd9a63f7a44c1efbca3acf2e882c3f1c707aba6237bda967`. Two
/// leading zero *bits* rather than bytes, because regtest's floor is the largest target
/// the compact encoding admits — the point is that a block costs a couple of attempts.
inline constexpr Hash256 REGTEST_GENESIS_HASH{std::array<uint8_t, Hash256::SIZE>{
    0x67, 0xA9, 0xBD, 0x37, 0x62, 0xBA, 0x7A, 0x70, 0x1C, 0x3F, 0x2C, 0x88, 0x2E, 0xCF, 0x3A, 0xCA,
    0xFB, 0x1E, 0x4C, 0xA4, 0xF7, 0x63, 0x9A, 0xFD, 0x96, 0xC1, 0x61, 0xA7, 0x2F, 0x8D, 0x24, 0x02}};
inline constexpr uint64_t REGTEST_GENESIS_NONCE = 3;

// Three distinct genesis blocks, which is the property the chain-id-as-lock-program
// device exists to produce. Checked here so that a copy-paste of one network's hash
// into another's slot is a build failure rather than two networks sharing a first
// block.
static_assert(MAINNET_GENESIS_HASH != TESTNET_GENESIS_HASH);
static_assert(MAINNET_GENESIS_HASH != REGTEST_GENESIS_HASH);
static_assert(TESTNET_GENESIS_HASH != REGTEST_GENESIS_HASH);
static_assert(MAINNET_GENESIS_NONCE != TESTNET_GENESIS_NONCE);

// --- The three networks -----------------------------------------------------

inline constexpr ChainParams MAINNET_PARAMS{
    .network = Network::Mainnet,
    .name = "mainnet",
    .chain_id = MAINNET_CHAIN_ID,
    .magic = MagicFromChainId(MAINNET_CHAIN_ID),
    .default_p2p_port = MAINNET_P2P_PORT,
    .default_rpc_port = MAINNET_RPC_PORT,
    .pow_limit_bits = POW_LIMIT_BITS,
    .genesis_bits = POW_LIMIT_BITS,
    .genesis_timestamp = GENESIS_TIMESTAMP,
    .genesis_nonce = MAINNET_GENESIS_NONCE,
    .genesis_hash = MAINNET_GENESIS_HASH,
    .target_block_seconds = TARGET_BLOCK_SECONDS,
    .asert_half_life_seconds = MAINNET_ASERT_HALF_LIFE_SECONDS,
    .coinbase_maturity = COINBASE_MATURITY,
    .max_block_weight = MAX_BLOCK_WEIGHT,
    .block_limits = CONSENSUS_BLOCK_LIMITS,
    .issuance = MAINNET_ISSUANCE,
    .allow_min_difficulty_blocks = false,
    .trivial_difficulty = false,
};

/// Testnet: mainnet's rules, with eras 100x shorter so the whole issuance curve —
/// including its end — is reachable in a test, and with the minimum-difficulty
/// allowance that keeps the chain moving when test hashrate disappears.
inline constexpr ChainParams TESTNET_PARAMS{
    .network = Network::Testnet,
    .name = "testnet",
    .chain_id = TESTNET_CHAIN_ID,
    .magic = MagicFromChainId(TESTNET_CHAIN_ID),
    .default_p2p_port = TESTNET_P2P_PORT,
    .default_rpc_port = TESTNET_RPC_PORT,
    .pow_limit_bits = POW_LIMIT_BITS,
    .genesis_bits = POW_LIMIT_BITS,
    .genesis_timestamp = GENESIS_TIMESTAMP,
    .genesis_nonce = TESTNET_GENESIS_NONCE,
    .genesis_hash = TESTNET_GENESIS_HASH,
    .target_block_seconds = TARGET_BLOCK_SECONDS,
    .asert_half_life_seconds = TESTNET_ASERT_HALF_LIFE_SECONDS,
    .coinbase_maturity = COINBASE_MATURITY,
    .max_block_weight = MAX_BLOCK_WEIGHT,
    .block_limits = CONSENSUS_BLOCK_LIMITS,
    .issuance = {.era_blocks = MAINNET_ISSUANCE.era_blocks / 100,
                 .initial_reward = MAINNET_ISSUANCE.initial_reward},
    .allow_min_difficulty_blocks = true,
    .trivial_difficulty = false,
};

/// Regtest: a private network for deterministic tests. Eras are 1 000x shorter than
/// mainnet's, so the end of issuance is 18 795 blocks away and the supply cap is
/// something a test can actually reach and then assert about. Coinbase maturity is
/// kept short for the same reason.
inline constexpr ChainParams REGTEST_PARAMS{
    .network = Network::Regtest,
    .name = "regtest",
    .chain_id = REGTEST_CHAIN_ID,
    .magic = MagicFromChainId(REGTEST_CHAIN_ID),
    .default_p2p_port = REGTEST_P2P_PORT,
    .default_rpc_port = REGTEST_RPC_PORT,
    .pow_limit_bits = REGTEST_POW_LIMIT_BITS,
    .genesis_bits = REGTEST_POW_LIMIT_BITS,
    .genesis_timestamp = GENESIS_TIMESTAMP,
    .genesis_nonce = REGTEST_GENESIS_NONCE,
    .genesis_hash = REGTEST_GENESIS_HASH,
    .target_block_seconds = TARGET_BLOCK_SECONDS,
    .asert_half_life_seconds = REGTEST_ASERT_HALF_LIFE_SECONDS,
    .coinbase_maturity = 20,
    .max_block_weight = MAX_BLOCK_WEIGHT,
    .block_limits = CONSENSUS_BLOCK_LIMITS,
    .issuance = {.era_blocks = MAINNET_ISSUANCE.era_blocks / 1'000,
                 .initial_reward = MAINNET_ISSUANCE.initial_reward},
    .allow_min_difficulty_blocks = true,
    .trivial_difficulty = true,
};

/// The parameters for a network. Total over the enumeration, so a new network cannot
/// be added without deciding what its parameters are.
[[nodiscard]] constexpr const ChainParams& ParamsFor(Network network) noexcept {
    switch (network) {
        case Network::Testnet:
            return TESTNET_PARAMS;
        case Network::Regtest:
            return REGTEST_PARAMS;
        case Network::Mainnet:
            break;
    }
    return MAINNET_PARAMS;
}

/// Parses the name a user or configuration file writes. Nothing is defaulted: an
/// unrecognised name is an error, never mainnet, because silently defaulting to the
/// network that holds real value is the wrong way to be wrong.
[[nodiscard]] constexpr std::optional<Network> NetworkFromName(std::string_view name) noexcept {
    if (name == MAINNET_PARAMS.name) {
        return Network::Mainnet;
    }
    if (name == TESTNET_PARAMS.name) {
        return Network::Testnet;
    }
    if (name == REGTEST_PARAMS.name) {
        return Network::Regtest;
    }
    return std::nullopt;
}

// --- Separation, verified at compile time -----------------------------------
//
// The point of these parameters is that a node on one network cannot accidentally
// speak to, or accept a signature from, another. A copy-paste that broke that would
// be a network split, so it is checked here rather than hoped for.

static_assert(MAINNET_CHAIN_ID != TESTNET_CHAIN_ID);
static_assert(MAINNET_CHAIN_ID != REGTEST_CHAIN_ID);
static_assert(TESTNET_CHAIN_ID != REGTEST_CHAIN_ID);

static_assert(MAINNET_PARAMS.magic != TESTNET_PARAMS.magic);
static_assert(MAINNET_PARAMS.magic != REGTEST_PARAMS.magic);
static_assert(TESTNET_PARAMS.magic != REGTEST_PARAMS.magic);

// Distinct in the *first* byte, so a mis-dialled connection is rejected on the first
// byte read rather than after three of four have matched.
static_assert(MAINNET_PARAMS.magic[0] != TESTNET_PARAMS.magic[0]);
static_assert(MAINNET_PARAMS.magic[0] != REGTEST_PARAMS.magic[0]);
static_assert(TESTNET_PARAMS.magic[0] != REGTEST_PARAMS.magic[0]);

// Never printable ASCII, so a text protocol cannot open a session by accident.
static_assert((MAINNET_PARAMS.magic[0] & 0x80U) != 0);
static_assert((TESTNET_PARAMS.magic[0] & 0x80U) != 0);
static_assert((REGTEST_PARAMS.magic[0] & 0x80U) != 0);

// Not Bitcoin's, Litecoin's, or their test networks' magic, so a peer running one of
// those is rejected rather than half-understood.
static_assert(MAINNET_PARAMS.magic != NetworkMagic{0xF9, 0xBE, 0xB4, 0xD9});
static_assert(MAINNET_PARAMS.magic != NetworkMagic{0x0B, 0x11, 0x09, 0x07});
static_assert(MAINNET_PARAMS.magic != NetworkMagic{0xFB, 0xC0, 0xB6, 0xDB});

// Six distinct ports, all outside Linux's default ephemeral range.
static_assert(MAINNET_P2P_PORT != MAINNET_RPC_PORT);
static_assert(TESTNET_P2P_PORT != MAINNET_P2P_PORT && TESTNET_P2P_PORT != MAINNET_RPC_PORT);
static_assert(REGTEST_P2P_PORT != TESTNET_P2P_PORT && REGTEST_RPC_PORT != TESTNET_RPC_PORT);
static_assert(REGTEST_RPC_PORT < 32'768, "a default listening port must not be ephemeral");

// Every network runs the same issuance shape, so the test networks exercise the real
// schedule rather than a simplified one, and every one of them has an end.
static_assert(EraCountWithReward(TESTNET_PARAMS.issuance) ==
              EraCountWithReward(MAINNET_PARAMS.issuance));
static_assert(EraCountWithReward(REGTEST_PARAMS.issuance) ==
              EraCountWithReward(MAINNET_PARAMS.issuance));
static_assert(TotalIssuance(TESTNET_PARAMS.issuance) < MAX_MONEY);
static_assert(TotalIssuance(REGTEST_PARAMS.issuance) < TotalIssuance(TESTNET_PARAMS.issuance));
static_assert(BlockReward(static_cast<uint32_t>(IssuanceEndHeight(REGTEST_PARAMS.issuance)),
                          REGTEST_PARAMS.issuance) == 0);

// Only mainnet is a real network: neither relaxation may reach it.
static_assert(!MAINNET_PARAMS.allow_min_difficulty_blocks);
static_assert(!MAINNET_PARAMS.trivial_difficulty);
static_assert(MAINNET_PARAMS.asert_half_life_seconds > 0);
static_assert(TESTNET_PARAMS.asert_half_life_seconds > 0);
static_assert(REGTEST_PARAMS.asert_half_life_seconds > 0);
static_assert(MAINNET_PARAMS.pow_limit_bits != REGTEST_PARAMS.pow_limit_bits);

// The derived bounds must be reachable in a block but not exceed what one can hold.
static_assert(CONSENSUS_BLOCK_LIMITS.max_transactions * Transaction::MIN_SERIALIZED_SIZE *
                  WITNESS_SCALE_FACTOR <=
              MAX_BLOCK_WEIGHT);
static_assert(CONSENSUS_TX_LIMITS.max_inputs * TxInput::SERIALIZED_SIZE * WITNESS_SCALE_FACTOR <=
              MAX_BLOCK_WEIGHT);
static_assert(CONSENSUS_TX_LIMITS.max_keys == MAX_SPEND_CONDITION_KEYS);
static_assert(CONSENSUS_TX_LIMITS.max_signatures <= CONSENSUS_TX_LIMITS.max_keys,
              "threshold <= keys, so no witness needs more signatures than keys");

// The name is the chain id's preimage, so it is consensus-relevant and must parse
// back to the network it names.
static_assert(NetworkFromName("mainnet") == Network::Mainnet);
static_assert(NetworkFromName("testnet") == Network::Testnet);
static_assert(NetworkFromName("regtest") == Network::Regtest);
static_assert(!NetworkFromName("main").has_value());
static_assert(!NetworkFromName("").has_value());
static_assert(ParamsFor(Network::Mainnet).name == "mainnet");
static_assert(ParamsFor(Network::Testnet).name == "testnet");
static_assert(ParamsFor(Network::Regtest).name == "regtest");

}  // namespace amarian
