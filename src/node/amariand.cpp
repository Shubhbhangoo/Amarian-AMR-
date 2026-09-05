/// \file
/// The Amarian node daemon.
///
/// What this binary does: parses options, selects a network, checks that every consensus
/// signature scheme is usable in this build and that this build's genesis is the selected
/// network's, opens the chainstate, restores the chain a previous run left behind, brings the
/// active chain up to the best block it holds, and — on request — mines blocks onto it or
/// moves blocks in and out of a flat file. Then it makes the database durable and exits.
///
/// What it does not do: talk to peers. There is no event loop here because there is nothing
/// yet to service — the network layer is Phase 4, and until it exists a process sitting in a
/// loop would only be idling while claiming to be a node. So the daemon does the work it was
/// asked for and stops, which is a complete thing to be and is what makes two independent
/// nodes checkable against each other today: one mines and exports, the other imports and
/// judges every block by its own rules, and the two tips are compared.
///
/// Every clock read in this file happens here, at the top, and is passed down as a value.
/// Nothing in consensus, the chain layer or the assembler calls a clock, which is what makes
/// a node's verdict on a block reproducible from a transcript rather than dependent on when
/// it was asked.

#include <amarian/chain/block_index.hpp>
#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/genesis.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/crypto/signature.hpp>
#include <amarian/mempool.hpp>
#include <amarian/mining/block_assembler.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/lock.hpp>
#include <amarian/storage/chain_db.hpp>
#include <amarian/util/args.hpp>
#include <amarian/util/hex.hpp>
#include <amarian/util/logging.hpp>
#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/coins.hpp>
#include <amarian/version.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace amarian {
namespace {

constexpr int EXIT_USAGE = 2;

/// Network magic, then the block's length: eight bytes ahead of every exported block.
///
/// The same framing Bitcoin's bootstrap files use, for the same two reasons. The magic makes
/// a file from the wrong network fail on its first record rather than deep inside a block,
/// and the length lets a reader skip a record without parsing it.
constexpr size_t BLOCK_RECORD_FRAMING_SIZE = sizeof(NetworkMagic) + sizeof(uint32_t);

/// How many nonces `--generate` searches per block before giving up.
///
/// Enough that regtest, whose target is met by almost any hash, is instant; small enough that
/// a difficulty a CPU cannot meet is reported in under a minute instead of spun on forever.
/// Raise it with `--generate-attempts`; a miner that needs more than this needs Phase 3's
/// mining RPC rather than a bigger number.
constexpr uint64_t DEFAULT_GENERATE_ATTEMPTS = uint64_t{1} << 26;

void Print(const std::string& text) {
    (void)std::fwrite(text.data(), 1, text.size(), stdout);
}

/// The node's wall clock, in seconds since the Unix epoch.
[[nodiscard]] int64_t UnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void RegisterOptions(ArgsParser& parser) {
    parser.Add({.name = "help",
                .kind = ArgKind::Flag,
                .help = "Show this help and exit.",
                .short_name = 'h'});
    parser.Add({.name = "version",
                .kind = ArgKind::Flag,
                .help = "Print the version and exit.",
                .short_name = 'v'});
    parser.Add({.name = "build-info",
                .kind = ArgKind::Flag,
                .help = "Print compiler, hardening and crypto library details, then exit."});
    parser.Add({.name = "chain",
                .kind = ArgKind::String,
                .value_hint = "<network>",
                .help = "One of mainnet, testnet, regtest. Default: mainnet."});
    parser.Add({.name = "datadir",
                .kind = ArgKind::String,
                .value_hint = "<dir>",
                .help = "Where the chain is kept. Default: $HOME/.amarian/<network>."});
    parser.Add({.name = "generate",
                .kind = ArgKind::Integer,
                .value_hint = "<n>",
                .help = "Mine n blocks onto the tip. Requires --payout."});
    parser.Add({.name = "generate-attempts",
                .kind = ArgKind::Integer,
                .value_hint = "<n>",
                .help = "Nonces to search per block before giving up. Default: 67108864."});
    parser.Add({.name = "payout",
                .kind = ArgKind::String,
                .value_hint = "<hex>",
                .help = "Hex-encoded lock the mined reward pays to."});
    parser.Add({.name = "export-blocks",
                .kind = ArgKind::String,
                .value_hint = "<file>",
                .help = "Write every block on the active chain to this file."});
    parser.Add({.name = "import-blocks",
                .kind = ArgKind::String,
                .value_hint = "<file>",
                .help = "Validate and accept every block in this file."});
    parser.Add({.name = "log-level",
                .kind = ArgKind::String,
                .value_hint = "<level>",
                .help = "One of error, warn, info, debug, trace. Default: info."});
    parser.Add({.name = "log-categories",
                .kind = ArgKind::String,
                .value_hint = "<list>",
                .help = "Comma-separated categories, 'all', or 'none'. Default: general."});
    parser.Add({.name = "log-file",
                .kind = ArgKind::String,
                .value_hint = "<path>",
                .help = "Also append log output to this file."});
    parser.Add({.name = "log-deterministic",
                .kind = ArgKind::Flag,
                .help = "Omit timestamps so logs from separate nodes can be diffed."});
}

/// Applies logging options. Returns false after reporting a usage error.
bool ConfigureLogging(const ArgsParser& parser) {
    if (parser.Has("log-level")) {
        const std::string name = parser.GetString("log-level");
        log::Level level{};
        if (!log::ParseLevel(name, &level)) {
            std::fprintf(
                stderr,
                "amariand: unknown --log-level '%s' (expected error, warn, info, debug or trace)\n",
                name.c_str());
            return false;
        }
        log::SetLevel(level);
    }

    if (parser.Has("log-categories")) {
        const std::string csv = parser.GetString("log-categories");
        std::string unknown;
        const uint32_t mask = log::ParseCategories(csv, &unknown);
        if (!unknown.empty()) {
            std::fprintf(stderr,
                         "amariand: unknown log category '%s' (known: %s, all, none)\n",
                         unknown.c_str(),
                         log::CategoryNames().c_str());
            return false;
        }
        log::DisableCategories(log::ToBits(log::Category::All));
        log::EnableCategories(mask);
    }

    if (parser.Has("log-deterministic")) {
        log::SetDeterministicOutput(parser.GetBool("log-deterministic"));
    }

    if (parser.Has("log-file")) {
        const std::string path = parser.GetString("log-file");
        if (!log::AddFileSink(path)) {
            std::fprintf(stderr, "amariand: cannot open log file '%s'\n", path.c_str());
            return false;
        }
    }
    return true;
}

/// Resolves --chain. Returns nullopt after reporting a usage error.
///
/// An unrecognised name is an error and never a default: silently falling back to the
/// network that holds real value is the wrong way to be wrong.
std::optional<Network> SelectNetwork(const ArgsParser& parser) {
    if (!parser.Has("chain")) {
        return Network::Mainnet;
    }
    const std::string name = parser.GetString("chain");
    const std::optional<Network> network = NetworkFromName(name);
    if (!network.has_value()) {
        std::fprintf(stderr,
                     "amariand: unknown --chain '%s' (expected mainnet, testnet or regtest)\n",
                     name.c_str());
        return std::nullopt;
    }
    return network;
}

/// Verifies that every signature scheme consensus knows about is actually usable in this
/// build, and reports the list.
///
/// A refusal to start rather than a warning. The scheme table is consensus: an output
/// locked to ML-DSA-44 is spendable only if this binary can verify ML-DSA-44, and a node
/// whose OpenSSL cannot provide it would not reject those spends — it would report them as
/// a scheme it does not know and, by the soft-fork rule, accept them unchecked. That is
/// precisely the failure that must never happen silently: a validator that believes it is
/// verifying signatures while verifying nothing.
bool CheckSignatureBackends() {
    if (const uint16_t missing = crypto::FirstUnavailableScheme();
        missing != crypto::SCHEME_RESERVED) {
        const crypto::SchemeSpec* spec = crypto::FindScheme(missing);
        AMARIAN_ERROR(log::Category::General,
                      "signature scheme {} ({}) is not available from this build's {}: refusing to "
                      "start, because a node that cannot verify a consensus scheme would accept "
                      "spends under it without checking them",
                      missing,
                      spec != nullptr ? spec->name : "unknown",
                      spec != nullptr ? spec->backend : "backend");
        return false;
    }

    for (const crypto::SchemeSpec& scheme : crypto::KnownSchemes()) {
        AMARIAN_INFO(log::Category::General,
                     "signature scheme {} {} ({}, {} byte key, {} byte signature) via {}",
                     scheme.id,
                     scheme.name,
                     scheme.scheme_class == crypto::SchemeClass::PostQuantum ? "post-quantum"
                                                                            : "classical",
                     scheme.public_key_bytes,
                     scheme.signature_bytes,
                     scheme.backend);
    }
    return true;
}

/// Verifies that this build's genesis parameters are the selected network's, and
/// reports the identity a node operator needs to confirm they are on the right chain.
///
/// A node whose block 0 differs from the network's shares no history with it at all,
/// so this is a refusal to start rather than a warning.
bool ReportChainIdentity(const ChainParams& params) {
    if (const GenesisFault fault = CheckGenesis(params); fault != GenesisFault::None) {
        AMARIAN_ERROR(log::Category::General,
                      "genesis check failed for {}: {}",
                      params.name,
                      Describe(fault));
        return false;
    }

    AMARIAN_INFO(
        log::Category::General, "network {} (chain_id {})", params.name, params.chain_id.ToHex());
    AMARIAN_INFO(log::Category::General,
                 "magic {:02x}{:02x}{:02x}{:02x}, p2p port {}, rpc port {}",
                 params.magic[0],
                 params.magic[1],
                 params.magic[2],
                 params.magic[3],
                 params.default_p2p_port,
                 params.default_rpc_port);
    AMARIAN_INFO(log::Category::General, "genesis {}", params.genesis_hash.ToHex());
    return true;
}

/// Where the chain lives: `--datadir` when given, else `$HOME/.amarian/<network>`.
///
/// One directory per network rather than one for all three. `ChainDb::Open` also refuses a
/// directory stamped with another network's id, so this is the first of two independent
/// guards against the same accident — a mainnet chainstate one mistyped flag away from being
/// overwritten with regtest blocks that cost nothing to produce.
std::optional<std::string> ResolveDataDir(const ArgsParser& parser, const ChainParams& params) {
    if (parser.Has("datadir")) {
        return parser.GetString("datadir");
    }
    const char* const home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        std::fprintf(stderr, "amariand: HOME is not set, so --datadir must be given\n");
        return std::nullopt;
    }
    return std::string(home) + "/.amarian/" + std::string(params.name);
}

/// Decodes `--payout`: the lock a mined reward pays to, in its canonical encoding.
///
/// A lock and not an address, and not derived from a key here. Turning a key or a recovery
/// phrase into a lock is the wallet's job, and a mining path that knew how to do it would be
/// a second implementation of the wallet's most important function — one that no wallet test
/// would ever cover. Who receives a reward is not a consensus question, so the node takes the
/// answer as bytes and commits to them unexamined beyond their being decodable.
std::optional<Lock> ParsePayout(const std::string& hex, const ChainParams& params) {
    const std::optional<ByteVec> bytes = FromHex(hex);
    if (!bytes.has_value()) {
        std::fprintf(stderr, "amariand: --payout is not hex\n");
        return std::nullopt;
    }
    Reader reader(*bytes);
    Lock lock;
    if (!Lock::Deserialize(reader, lock, params.block_limits.tx.max_lock_program_size) ||
        !reader.Finish()) {
        std::fprintf(stderr, "amariand: --payout is not a lock this network can decode\n");
        return std::nullopt;
    }
    if (lock.IsUnspendable()) {
        // Refused rather than mined to. An unspendable lock is a valid output and a legitimate
        // thing to build — genesis pays to one — but a reward sent there can never be moved by
        // anyone, and silently burning issuance because a flag was copied wrong is not a
        // mistake a node should help make.
        std::fprintf(stderr,
                     "amariand: --payout is an unspendable lock, so the reward could never be "
                     "spent by anyone\n");
        return std::nullopt;
    }
    return lock;
}

/// Keeps the mempool in step with the active chain.
///
/// The pool holds transactions that are unconfirmed *as of the tip*, and the block assembler
/// takes what the pool hands it without judging it again — deliberately, because a second
/// validation there would be a second implementation of the rules. So every transaction in
/// the pool must be one the next block could legally contain, and that is a property the
/// chain moving underneath it can destroy. This class is what stops it destroying it
/// quietly.
///
/// Connections are handled as they happen and reorganisations once at the end, which is not
/// an inconsistency but the difference in what the two cost. `RemoveForBlock` is proportional
/// to the block and exactly right: it drops what the block confirmed and what the block
/// conflicts with. The reorganisation sweep re-judges every entry in the pool, signatures
/// included, so running it per reversed block would do the same expensive work once per block
/// of a reorganisation to reach the answer the last pass would have reached anyway.
class PoolKeeper final : public chain::TipObserver {
public:
    PoolKeeper(const chain::ChainState& state, const ChainParams& params)
        : state_(&state), params_(&params) {}

    /// The pool itself. Owned here rather than beside this class so that the pool and the
    /// thing responsible for its upkeep cannot be wired to two different objects — which is
    /// the same reason `ChainState` owns the correspondence between the index and the coins
    /// set instead of leaving a caller to hold both.
    [[nodiscard]] mempool::Mempool& Pool() noexcept { return pool_; }
    [[nodiscard]] const mempool::Mempool& Pool() const noexcept { return pool_; }

    void BlockConnected(const Block& block, const chain::BlockIndexEntry& entry) override {
        const size_t dropped = pool_.RemoveForBlock(block);
        if (dropped != 0) {
            AMARIAN_DEBUG(log::Category::General,
                          "mempool: the block at height {} removed {} entry(ies)",
                          entry.height,
                          dropped);
        }
    }

    void BlockDisconnected(const Block& unused_block,
                           const chain::BlockIndexEntry& entry) override {
        static_cast<void>(unused_block);
        // Recorded rather than acted on. The sweep needs the coins set and the tip as they
        // will be when activation has finished, and mid-walk they are neither — the chain may
        // still reverse further blocks or apply a whole branch before it settles.
        reorganised_ = true;
        AMARIAN_DEBUG(log::Category::General, "mempool: height {} was reversed", entry.height);
    }

    /// Re-judges the pool against the chain as it now stands, if anything was reversed.
    ///
    /// Called after `ActivateBestChain` returns and never from a notification, which is what
    /// makes reading the state here allowed: `TipObserver` forbids an observer from touching
    /// the chain during a callback because activation is mid-walk, and this is not one.
    void Settle() {
        if (!reorganised_) {
            return;
        }
        reorganised_ = false;
        // The height a pool transaction would now confirm at, which is what maturity and
        // locktime are judged against. Saturating for the same reason the assembler's is: a
        // tip at the top of the range has no valid child, and that is for the rules to say.
        const uint32_t tip_height = state_->Tip().height;
        const uint32_t spend_height =
            tip_height < std::numeric_limits<uint32_t>::max() ? tip_height + 1 : tip_height;
        const size_t dropped = pool_.RemoveForReorg(state_->Coins(), spend_height, *params_);
        if (dropped != 0) {
            // Worth a warning rather than a debug line. These are transactions this node had
            // accepted and would have mined, and the reason they are gone is that the chain
            // moved under them — which the sender has no way to see and will want to know.
            AMARIAN_WARN(log::Category::General,
                         "mempool: dropped {} entry(ies) that the reorganisation invalidated",
                         dropped);
        }
    }

private:
    const chain::ChainState* state_;
    const ChainParams* params_;
    mempool::Mempool pool_;
    bool reorganised_ = false;
};

/// Brings the active chain up to the best block this node holds, reporting what moved.
///
/// Called at startup as well as after every block, because an interrupted run can leave
/// bodies stored above the committed tip: they were accepted, and the process stopped before
/// they were applied. Resuming means applying them, and it is the same call either way.
bool Advance(chain::ChainState& state, PoolKeeper& keeper) {
    const std::expected<chain::ActivationSummary, chain::ActivationFailure> activated =
        state.ActivateBestChain();
    if (!activated.has_value()) {
        AMARIAN_ERROR(log::Category::General,
                      "activation stopped at {}: {}",
                      activated.error().block.ToHex(),
                      chain::Describe(activated.error().error));
        return false;
    }

    // Before anything is reported, so that a log line about a reorganisation and the pool it
    // invalidated appear in the order the two things happened.
    keeper.Settle();

    // Not a failure. Refusing an invalid block and carrying on to the next-best branch is
    // activation working, and the operator is told which blocks those were because a node that
    // rejected a block silently would give nobody anything to investigate.
    for (const chain::RejectedBlock& refused : activated->rejected) {
        AMARIAN_WARN(log::Category::General,
                     "rejected height {} {}: {}",
                     refused.height,
                     refused.hash.ToHex(),
                     consensus::Describe(refused.error));
    }
    if (activated->connected != 0 || activated->disconnected != 0) {
        AMARIAN_INFO(log::Category::General,
                     "applied {} block(s), reversed {}",
                     activated->connected,
                     activated->disconnected);
    }
    return true;
}

/// Reports the tip an operator has to be able to compare against another node's.
void ReportTip(const chain::ChainState& state) {
    const chain::BlockIndexEntry& tip = state.Tip();
    AMARIAN_INFO(log::Category::General,
                 "tip height {} {} (work {})",
                 tip.height,
                 tip.hash.ToHex(),
                 tip.total_work.ToHex());
}

/// Mines `count` blocks onto the tip, one at a time, each committed before the next begins.
///
/// Not a competitive miner and not pretending to be: one thread, one nonce range, and a
/// budget, so that a difficulty a CPU cannot meet is reported rather than spun on. It exists
/// because Phase 1 needs blocks to exist at all; the `getblocktemplate`-shaped RPC a real
/// miner talks to, with long-polling and extranonce handling, is Phase 3.
///
/// Every block goes through `AcceptBlock` — the same entry point a block from a stranger uses.
/// The assembler and the validator are separate bodies of code on purpose, so a template this
/// node refuses is a bug worth stopping on and not a formality to skip.
bool GenerateBlocks(chain::ChainState& state,
                    PoolKeeper& keeper,
                    const Lock& payout,
                    int64_t count,
                    uint64_t attempts,
                    const ChainParams& params) {
    for (int64_t made = 0; made < count; ++made) {
        const int64_t now = UnixSeconds();
        const std::expected<mining::BlockTemplate, mining::TemplateError> assembled =
            mining::BuildBlockTemplate(
                state.Tip(), payout, now, ByteVec{}, params, &keeper.Pool());
        if (!assembled.has_value()) {
            AMARIAN_ERROR(log::Category::General,
                          "cannot build a block on {}: {}",
                          state.Tip().hash.ToHex(),
                          mining::Describe(assembled.error()));
            return false;
        }

        Block block = assembled->block;
        if (!mining::SolveHeader(block.header, assembled->target, attempts)) {
            AMARIAN_ERROR(log::Category::General,
                          "no nonce in {} attempts solved height {} at target {:08x}: CPU mining "
                          "is not viable at this difficulty",
                          attempts,
                          block.header.height,
                          block.header.target_bits);
            return false;
        }

        const std::expected<const chain::BlockIndexEntry*, chain::HeaderError> accepted =
            state.AcceptBlock(block, now);
        if (!accepted.has_value()) {
            AMARIAN_ERROR(log::Category::General,
                          "this node refused a block it built itself at height {}: {}",
                          block.header.height,
                          chain::Describe(accepted.error()));
            return false;
        }
        if (!Advance(state, keeper)) {
            return false;
        }

        // The transaction count excludes the coinbase, so it reads as "what this block carried
        // for other people" — which is the number that says whether the mempool is reaching the
        // assembler at all, and is zero for every block until one does.
        AMARIAN_INFO(log::Category::General,
                     "mined height {} {} (nonce {}, {} tx, {} facets to a version {} lock, {} of "
                     "it fees)",
                     block.header.height,
                     block.header.Hash().ToHex(),
                     block.header.nonce,
                     block.transactions.size() - 1,
                     assembled->reward,
                     payout.version,
                     assembled->fees);
    }
    return true;
}

[[nodiscard]] bool WriteAll(std::FILE* file, ByteSpan bytes) {
    return bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
}

/// Writes every block on the active chain to `path`, lowest height first.
///
/// This exists because two independent nodes have to be able to check each other before there
/// is a network to do it over, and it is not a substitute for one: a file says nothing about
/// who produced it, carries no work of its own, and every block in it is judged by the
/// importing node's own rules. It is a transport, and a slow one.
///
/// Genesis is skipped. It is a chain parameter that every node rebuilds from
/// `BuildGenesisBlock`, so a file has nothing to teach an importer about it, and shipping it
/// would invite the idea that block 0 is something a peer supplies.
bool ExportBlocks(const chain::ChainState& state,
                  const storage::ChainDb& db,
                  const std::string& path,
                  const ChainParams& params) {
    std::FILE* const out = std::fopen(path.c_str(), "wb");
    if (out == nullptr) {
        AMARIAN_ERROR(log::Category::General, "cannot open '{}' for writing", path);
        return false;
    }

    bool ok = true;
    size_t exported = 0;
    const chain::ActiveChain& chain = state.Chain();
    for (uint32_t height = 1; ok; ++height) {
        const chain::BlockIndexEntry* const entry = chain.AtHeight(height);
        if (entry == nullptr) {
            break;
        }
        const std::optional<Block> block = db.GetBlock(entry->hash);
        if (!block.has_value()) {
            // Every block on the active chain was connected, and connecting one requires its
            // body. A hole here is this node's own storage contradicting its own chain.
            AMARIAN_ERROR(log::Category::General,
                          "no stored body for height {} {}",
                          height,
                          entry->hash.ToHex());
            ok = false;
            break;
        }

        Writer body(params.max_block_weight / 4);
        block->Serialize(body);
        Writer record(BLOCK_RECORD_FRAMING_SIZE + body.Size());
        record.WriteBytes(ByteSpan(params.magic));
        record.WriteU32(static_cast<uint32_t>(body.Size()));
        record.WriteBytes(body.Bytes());
        ok = WriteAll(out, record.Bytes());
        if (!ok) {
            AMARIAN_ERROR(log::Category::General, "write to '{}' failed", path);
            break;
        }
        ++exported;
    }

    // Closed even on the failure path, and its result checked: a stream closed with buffered
    // bytes still in it reports the loss here or nowhere.
    if (std::fclose(out) != 0) {
        AMARIAN_ERROR(log::Category::General, "closing '{}' failed", path);
        ok = false;
    }
    if (ok) {
        AMARIAN_INFO(log::Category::General, "exported {} block(s) to '{}'", exported, path);
    }
    return ok;
}

/// Offers every block in `path` to the chain, then activates once.
///
/// Each block is decoded under this network's limits, then handed to `AcceptBlock`, which
/// applies every rule that needs no chain and stores the body. Activation happens once at the
/// end rather than per block, because applying a run of blocks to the coins set is the same
/// work whether it is asked for once or a thousand times, and asking once means a single walk
/// up the branch.
///
/// A rejected block does not stop the import — the operator is shown every problem in the file
/// rather than the first — but it does make the run fail. A file that this node partly refuses
/// is not a file it agrees with, and reporting success would be reporting a chain that is not
/// the one that was offered.
bool ImportBlocks(chain::ChainState& state,
                  PoolKeeper& keeper,
                  const std::string& path,
                  const ChainParams& params) {
    std::FILE* const in = std::fopen(path.c_str(), "rb");
    if (in == nullptr) {
        AMARIAN_ERROR(log::Category::General, "cannot open '{}' for reading", path);
        return false;
    }

    bool ok = true;
    size_t offered = 0;
    size_t refused = 0;
    for (;;) {
        std::array<uint8_t, BLOCK_RECORD_FRAMING_SIZE> framing{};
        const size_t read = std::fread(framing.data(), 1, framing.size(), in);
        if (read == 0 && std::feof(in) != 0) {
            break;
        }
        if (read != framing.size()) {
            AMARIAN_ERROR(log::Category::General, "'{}' ends inside a record header", path);
            ok = false;
            break;
        }

        Reader framing_reader(framing);
        NetworkMagic magic{};
        uint32_t length = 0;
        if (!framing_reader.ReadBytes(magic) || !framing_reader.ReadU32(length) ||
            !framing_reader.Finish()) {
            AMARIAN_ERROR(log::Category::General, "'{}' has an unreadable record header", path);
            ok = false;
            break;
        }
        if (magic != params.magic) {
            // Stopped at the first record rather than attempted. A file from another network
            // decodes as blocks — the encodings are identical — and every one of them would be
            // refused for a reason that says nothing about what actually went wrong.
            AMARIAN_ERROR(log::Category::General,
                          "'{}' is not a {} block file: magic {}",
                          path,
                          params.name,
                          ToHex(ByteSpan(magic)));
            ok = false;
            break;
        }
        if (length == 0 || length > params.max_block_weight) {
            AMARIAN_ERROR(log::Category::General,
                          "'{}' claims a {} byte block, which this network cannot hold",
                          path,
                          length);
            ok = false;
            break;
        }

        ByteVec encoded(length);
        if (std::fread(encoded.data(), 1, encoded.size(), in) != encoded.size()) {
            AMARIAN_ERROR(log::Category::General, "'{}' ends inside a block", path);
            ok = false;
            break;
        }
        Reader block_reader(encoded);
        Block block;
        if (!Block::Deserialize(block_reader, block, params.block_limits) ||
            !block_reader.Finish()) {
            AMARIAN_ERROR(log::Category::General,
                          "'{}' holds {} bytes that are not a block",
                          path,
                          length);
            ok = false;
            break;
        }

        ++offered;
        const std::expected<const chain::BlockIndexEntry*, chain::HeaderError> accepted =
            state.AcceptBlock(block, UnixSeconds());
        if (!accepted.has_value()) {
            AMARIAN_ERROR(log::Category::General,
                          "refused height {} {}: {}",
                          block.header.height,
                          block.Hash().ToHex(),
                          chain::Describe(accepted.error()));
            ++refused;
        }
    }
    (void)std::fclose(in);

    if (ok) {
        AMARIAN_INFO(log::Category::General,
                     "read {} block(s) from '{}', refused {}",
                     offered,
                     path,
                     refused);
    } else if (offered != 0) {
        // Worded as a stop rather than a read, because the count is how far the file got before
        // it broke and not how much of it there was. Reported at all because the blocks ahead of
        // the fault were still offered and may still have been accepted, so a failed import is
        // not the same thing as an import that did nothing.
        AMARIAN_WARN(log::Category::General,
                     "stopped after {} block(s) of '{}', refused {}",
                     offered,
                     path,
                     refused);
    }

    // Applied once, after the whole file, rather than per block: a run of blocks is one walk up
    // the branch either way. Skipped when the file yielded nothing at all, so that a file this
    // node refused outright does not also log a chain movement that never happened.
    if (offered != 0 && !Advance(state, keeper)) {
        return false;
    }
    return ok && refused == 0;
}

int Run(int argc, char* argv[]) {
    ArgsParser parser("amariand", "[options]");
    RegisterOptions(parser);

    if (const auto parsed = parser.Parse(argc, argv); !parsed.has_value()) {
        std::fprintf(stderr, "amariand: %s\n", parsed.error().Message().c_str());
        std::fprintf(stderr, "Try 'amariand --help'.\n");
        return EXIT_USAGE;
    }

    if (parser.Has("help")) {
        Print("Amarian node daemon " + VersionString() + "\n\n" + parser.HelpText());
        return EXIT_SUCCESS;
    }
    if (parser.Has("version")) {
        Print(VersionStringLong() + "\n");
        return EXIT_SUCCESS;
    }
    if (parser.Has("build-info")) {
        Print("Amarian build information\n" + BuildInfoString());
        return EXIT_SUCCESS;
    }

    if (!parser.Positional().empty()) {
        std::fprintf(
            stderr, "amariand: unexpected argument '%s'\n", parser.Positional().front().c_str());
        return EXIT_USAGE;
    }

    if (!ConfigureLogging(parser)) {
        return EXIT_USAGE;
    }

    const std::optional<Network> network = SelectNetwork(parser);
    if (!network.has_value()) {
        return EXIT_USAGE;
    }

    AMARIAN_INFO(log::Category::General, "Amarian {} starting", VersionStringLong());
    AMARIAN_INFO(log::Category::General, "user agent {}", UserAgent());

    const ChainParams& params = ParamsFor(*network);
    if (!CheckSignatureBackends()) {
        return EXIT_FAILURE;
    }
    if (!ReportChainIdentity(params)) {
        return EXIT_FAILURE;
    }

    // Options that need each other, resolved before any state is opened. A run that created a
    // data directory and then refused the flags meant to use it would leave the operator with a
    // directory they did not ask for.
    std::optional<Lock> payout;
    if (parser.Has("payout")) {
        payout = ParsePayout(parser.GetString("payout"), params);
        if (!payout.has_value()) {
            return EXIT_USAGE;
        }
    }
    const int64_t generate = parser.GetInt("generate", 0);
    if (generate < 0) {
        std::fprintf(stderr, "amariand: --generate cannot be negative\n");
        return EXIT_USAGE;
    }
    if (generate > 0 && !payout.has_value()) {
        std::fprintf(stderr, "amariand: --generate needs --payout to say who is paid\n");
        return EXIT_USAGE;
    }
    const int64_t asked_attempts = parser.GetInt("generate-attempts", 0);
    if (asked_attempts < 0) {
        std::fprintf(stderr, "amariand: --generate-attempts cannot be negative\n");
        return EXIT_USAGE;
    }
    const uint64_t attempts =
        asked_attempts > 0 ? static_cast<uint64_t>(asked_attempts) : DEFAULT_GENERATE_ATTEMPTS;

    const std::optional<std::string> datadir = ResolveDataDir(parser, params);
    if (!datadir.has_value()) {
        return EXIT_USAGE;
    }
    // RocksDB creates the database's own directory but not the path leading to it, and the
    // default data directory has one.
    std::error_code unusable;
    std::filesystem::create_directories(*datadir, unusable);
    if (unusable) {
        AMARIAN_ERROR(
            log::Category::General, "cannot create '{}': {}", *datadir, unusable.message());
        return EXIT_FAILURE;
    }

    const std::expected<std::unique_ptr<storage::ChainDb>, storage::DbError> db =
        storage::ChainDb::Open(*datadir, params);
    if (!db.has_value()) {
        AMARIAN_ERROR(log::Category::General,
                      "cannot open the chainstate at '{}': {}",
                      *datadir,
                      storage::Describe(db.error()));
        return EXIT_FAILURE;
    }
    AMARIAN_INFO(log::Category::General, "chainstate at '{}'", *datadir);

    // The header tree and the tip the stored coins belong to, every header re-judged by the
    // rules it passed when it arrived. `LoadChain` is where a tampered database is caught, and
    // it is caught before anything here weighs a single branch.
    std::expected<storage::LoadedChain, storage::DbError> loaded =
        storage::LoadChain(**db, UnixSeconds(), params);
    if (!loaded.has_value()) {
        AMARIAN_ERROR(log::Category::General,
                      "cannot restore the chain from '{}': {}",
                      *datadir,
                      storage::Describe(loaded.error()));
        return EXIT_FAILURE;
    }
    AMARIAN_INFO(log::Category::General, "restored {} stored header(s)", loaded->restored);

    utxo::CoinsCache coins = utxo::CoinsCache::Over(**db);
    chain::ChainState state(loaded->index, coins, **db, params);
    state.PersistTo(**db);

    // The pool is empty at startup and stays that way until something offers a transaction, so
    // it is registered before the catch-up activation below purely so there is no window in
    // which the chain can move without the pool hearing about it. A pool that missed a
    // connection would keep offering a transaction that block already confirmed, and the
    // assembler does not re-check what the pool hands it.
    //
    // Nothing persists it. A mempool is unconfirmed by definition — every entry in it is
    // either mined, replaced, or invalidated eventually, and none of it is state this node
    // owes anyone across a restart. Reloading a pool from disk would mean re-judging every
    // entry against a chain that may have moved a long way, which is the sweep, at startup,
    // for transactions nobody asked this node to keep.
    PoolKeeper keeper(state, params);
    state.ObserveWith(keeper);

    state.ResumeAt(*loaded->tip);
    ReportTip(state);

    // Anything accepted but never applied before the last run stopped is applied now. Without
    // this a resumed node would sit below a tip it already holds every block for.
    if (!Advance(state, keeper)) {
        return EXIT_FAILURE;
    }

    const bool acted = parser.Has("import-blocks") || generate > 0 || parser.Has("export-blocks");
    if (parser.Has("import-blocks") &&
        !ImportBlocks(state, keeper, parser.GetString("import-blocks"), params)) {
        return EXIT_FAILURE;
    }
    if (generate > 0 && !GenerateBlocks(state, keeper, *payout, generate, attempts, params)) {
        return EXIT_FAILURE;
    }
    if (parser.Has("export-blocks") &&
        !ExportBlocks(state, **db, parser.GetString("export-blocks"), params)) {
        return EXIT_FAILURE;
    }
    if (acted) {
        ReportTip(state);
    }

    // Durability against the machine dying rather than the process dying, which every commit
    // already has. Paid for once, here, rather than once per block.
    if (!(*db)->Sync()) {
        AMARIAN_ERROR(log::Category::General, "flushing the chainstate to disk failed");
        return EXIT_FAILURE;
    }
    if ((*db)->HasFault()) {
        // A latched fault means a read was answered wrongly or a write was reported as done
        // when it was not. The tip above may name a chain whose coins or bodies are not all
        // there, so the run is failed even though every step of it returned.
        AMARIAN_ERROR(log::Category::General,
                      "the chainstate reported a fault during this run: check the disk before "
                      "trusting this node's tip");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

}  // namespace
}  // namespace amarian

int main(int argc, char* argv[]) {
    return amarian::Run(argc, argv);
}
