#pragma once

/// \file
/// The genesis block of each network, and the check that a build has the right one.
///
/// Genesis is a chain parameter rather than a computed value, but it is not *stored*
/// as a byte blob either. It is rebuilt from `ChainParams` by `BuildGenesisBlock`,
/// and the parameters carry only the two fields a builder cannot derive — the
/// timestamp and the mined nonce — plus the hash that the finished block must have.
///
/// That split is the point. A hard-coded serialised block is a value nobody reviews:
/// it is 200 bytes of hex that either works or does not. A builder plus a recorded
/// hash means every field of genesis is written in readable code, and the recorded
/// hash is an independent statement about what that code must produce. `CheckGenesis`
/// compares the two at startup, so a corrupted parameter table, a changed encoding,
/// or an edited message stops the node instead of forking it onto a chain of one.
///
/// ## What genesis contains, and why
///
/// - **No reward.** Its coinbase output is zero facets, and the output's lock is
///   `LOCK_VERSION_UNSPENDABLE`, which no witness can ever satisfy. Together those
///   mean there is nothing to argue about: not a premine, not a burn address someone
///   might hold the key to, not a token amount. Zero, unspendable by rule.
/// - **The network's chain id, as the unspendable lock's program.** This is what makes
///   the three networks' genesis blocks different from each other rather than
///   accidentally identical, and it puts the difference inside the Merkle root that
///   the header commits to.
/// - **A dated public reference, in the coinbase's arbitrary bytes.** Evidence that
///   the chain was not mined before its stated start, in the way Bitcoin's was.
///
/// Genesis is also the one block whose `prev_block` is all-zero and whose height is
/// 0; both are checked here rather than assumed by later validation.

#include <amarian/consensus/params.hpp>
#include <amarian/primitives/block.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace amarian {

/// The public reference every network's genesis coinbase carries.
///
/// Reuters, Manila, Saturday 5 September 2026: the Philippine vice president posted
/// bail after a court ordered her arrest on three counts of grave threats. Reported
/// the same day by Reuters, AP, NPR, Rappler and GMA, so it is checkable against
/// several independent archives rather than one.
///
/// It is here for the reason Bitcoin's headline was: nobody could have written this
/// sentence before 5 September 2026, so no genesis block containing it existed before
/// then either. It is a claim about a lower bound on the chain's age and nothing else
/// — it says nothing about when mainnet opens to the public, which is a launch
/// procedure and is documented as one.
inline constexpr std::string_view GENESIS_MESSAGE =
    "Reuters 05/Sep/2026 Philippine VP Duterte posts bail after arrest order";

/// Builds a network's genesis block from its parameters. Deterministic: the same
/// `ChainParams` always produces byte-identical output.
[[nodiscard]] Block BuildGenesisBlock(const ChainParams& params);

/// Why a parameter table's genesis is unusable. Every case is a build or packaging
/// fault rather than anything a peer can cause, so each one must stop the node: a
/// node that disagrees with the network about block 0 agrees with it about nothing.
enum class GenesisFault : uint8_t {
    None,
    /// The rebuilt block does not hash to `params.genesis_hash`.
    HashMismatch,
    /// `genesis_bits` does not decode as a compact target.
    MalformedTarget,
    /// The recorded nonce does not satisfy the recorded target.
    InsufficientWork,
    /// The block does not have the shape genesis is defined to have.
    MalformedBlock,
};

/// Checks a network's genesis: that it rebuilds to the recorded hash, that the hash
/// is actually work against `genesis_bits`, and that the structural rules genesis is
/// defined by hold. Returns `GenesisFault::None` when every check passes.
[[nodiscard]] GenesisFault CheckGenesis(const ChainParams& params);

/// A short, stable description for a log line or a startup error.
[[nodiscard]] std::string_view Describe(GenesisFault fault) noexcept;

/// Searches for a nonce that makes `params`' genesis meet `genesis_bits`, starting at
/// `start_nonce` and trying at most `attempts` values. Returns the nonce, or nullopt
/// if the range ran out.
///
/// This is how the recorded nonces were produced, and it is kept so that a reader can
/// reproduce them instead of taking them on faith. It is a single-threaded search over
/// a caller-chosen range precisely so that a caller can split the range across
/// threads; it is not the mining loop a miner will use, which has different work to
/// do between attempts.
[[nodiscard]] std::optional<uint64_t>
FindGenesisNonce(const ChainParams& params, uint64_t start_nonce, uint64_t attempts);

}  // namespace amarian
