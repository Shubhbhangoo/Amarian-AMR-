#include <amarian/storage/chain_db.hpp>

#include <amarian/chain/block_index.hpp>
#include <amarian/chain/chain_state.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/validation.hpp>
#include <amarian/primitives/block.hpp>
#include <amarian/primitives/coin.hpp>
#include <amarian/primitives/outpoint.hpp>
#include <amarian/util/serialize.hpp>
#include <amarian/util/types.hpp>
#include <amarian/utxo/coins.hpp>
#include <amarian/utxo/connect.hpp>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amarian::storage {
namespace {

/// The layout version. Bumped when a stored encoding changes in a way this build could
/// misread; a database stamped with anything else is refused rather than guessed at.
constexpr uint32_t SCHEMA_VERSION = 1;

constexpr std::string_view CF_COINS = "coins";
constexpr std::string_view CF_BLOCKS = "blocks";
constexpr std::string_view CF_UNDO = "undo";
constexpr std::string_view CF_INDEX = "index";
constexpr std::string_view CF_META = "meta";

constexpr std::string_view KEY_SCHEMA = "schema";
constexpr std::string_view KEY_CHAIN_ID = "chain_id";
constexpr std::string_view KEY_TIP = "tip";

/// Which entry of `Impl::families_` each column family is. The order the descriptors are
/// built in, and the order RocksDB returns handles in.
enum class Family : size_t {
    Default = 0,
    Coins = 1,
    Blocks = 2,
    Undo = 3,
    Index = 4,
    Meta = 5,
};

constexpr size_t FAMILY_COUNT = 6;

[[nodiscard]] rocksdb::Slice AsSlice(std::string_view text) noexcept {
    return rocksdb::Slice(text.data(), text.size());
}

[[nodiscard]] rocksdb::Slice AsSlice(ByteSpan bytes) noexcept {
    // RocksDB's Slice is a (const char*, size_t) pair over borrowed memory. The cast is a
    // reinterpretation of byte storage, which is exactly what a slice of bytes is.
    return rocksdb::Slice(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] rocksdb::Slice AsSlice(const ByteVec& bytes) noexcept {
    return AsSlice(ByteSpan(bytes.data(), bytes.size()));
}

/// A borrowed view of a RocksDB value, for feeding to a `Reader`.
[[nodiscard]] ByteSpan Borrow(const std::string& value) noexcept {
    return ByteSpan(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

[[nodiscard]] ByteSpan Borrow(const rocksdb::Slice& value) noexcept {
    return ByteSpan(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

/// Serialises one value into an owned buffer.
///
/// `reserve_hint` is the record's exact size where that is a constant, and left at zero for
/// the variable-length records — a block, an undo record — where guessing it would be a
/// second, worse implementation of "how long is this". A `Writer` grows geometrically, so the
/// cost of not guessing is a few reallocations on a path that is about to do disk I/O.
template<typename T>
[[nodiscard]] ByteVec Encode(const T& value, size_t reserve_hint = 0) {
    Writer writer(reserve_hint);
    value.Serialize(writer);
    return writer.Take();
}

}  // namespace

std::string_view Describe(DbError error) noexcept {
    switch (error) {
        case DbError::CannotOpen:
            return "the chain database could not be opened";
        case DbError::ReadFailed:
            return "a read from the chain database failed";
        case DbError::WriteFailed:
            return "a write to the chain database was refused";
        case DbError::CorruptRecord:
            return "a stored record did not decode or no longer passes its own rules";
        case DbError::WrongNetwork:
            return "the data directory holds another network's chain";
        case DbError::TipNotIndexed:
            return "the recorded tip is not among the recorded headers";
    }
    // Unreachable: the switch is total, and -Wswitch-enum makes a new enumerator a build
    // failure here rather than a silent fall-through.
    return "unknown database error";
}

// --- The handle -------------------------------------------------------------

/// Everything that would otherwise put RocksDB's headers in the public interface.
struct ChainDb::Impl {
    std::unique_ptr<rocksdb::DB> db;
    std::vector<rocksdb::ColumnFamilyHandle*> families;

    /// The network whose rules bound every decode this database performs. A stored block or
    /// coin is decoded against the same limits a relayed one is, because a record that
    /// exceeds them is a corrupt record whichever direction it arrived from — and decoding
    /// without a bound is how a damaged length prefix becomes an allocation.
    ///
    /// Borrowed. `ParamsFor` returns a reference to a constant with static storage duration,
    /// which is what every caller passes.
    const ChainParams* params = nullptr;

    /// Set by any refused read, any refused write, and any stored record that did not decode.
    ///
    /// Latching rather than counting. One fault is enough to make every answer this database
    /// gives suspect — a read that failed has already been reported as "no such record" — and
    /// clearing it would be claiming the loss had been repaired rather than merely noticed.
    ///
    /// Mutated from `const` member functions of `ChainDb`. That is not a `mutable` hack: a
    /// `const std::unique_ptr<Impl>` yields a non-const `Impl*`, because constness does not
    /// propagate through a smart pointer. The read path is logically const and physically not,
    /// which is exactly the shape a latch needs.
    bool faulted = false;

    void Fault() noexcept { faulted = true; }

    [[nodiscard]] rocksdb::ColumnFamilyHandle* Of(Family family) const noexcept {
        return families[static_cast<size_t>(family)];
    }

    /// Reads one key. Absent returns `false` with `failed` untouched; a real failure returns
    /// `false`, sets `failed`, and latches.
    ///
    /// `failed` is an out-parameter as well as a latch because the two have different
    /// audiences: `Open` needs to tell a missing stamp from an unreadable one in order to
    /// choose between writing it and refusing, while everything after `Open` only needs to
    /// know that this database can no longer be believed.
    [[nodiscard]] bool Read(Family family, const rocksdb::Slice& key, std::string& value,
                            bool& failed) {
        const rocksdb::Status status = db->Get(rocksdb::ReadOptions(), Of(family), key, &value);
        if (status.ok()) {
            return true;
        }
        if (!status.IsNotFound()) {
            failed = true;
            Fault();
        }
        return false;
    }

    [[nodiscard]] bool Write(Family family, const rocksdb::Slice& key,
                             const rocksdb::Slice& value) {
        const rocksdb::Status status =
            db->Put(rocksdb::WriteOptions(), Of(family), key, value);
        if (!status.ok()) {
            Fault();
        }
        return status.ok();
    }

    /// Whether a key is present, without copying its value into a fresh allocation.
    ///
    /// A `PinnableSlice` borrows RocksDB's own buffer where it can, so this costs the lookup
    /// but not a copy of the record — which matters because the largest record here is a
    /// block body. The lookup itself is unavoidable: locating a key in an LSM tree means
    /// reading the data block that holds it, so presence cannot be answered more cheaply than
    /// a read without a second structure to hold it, and nothing asks often enough yet to
    /// justify one.
    ///
    /// `KeyMayExist` is deliberately not that shortcut. It may answer true for an absent key,
    /// and a false positive here would report a block body this node does not have — which
    /// `ChainState` would treat as a reachable block and then fail to read, stopping
    /// activation on a database that is in fact healthy.
    [[nodiscard]] bool Exists(Family family, const rocksdb::Slice& key) {
        rocksdb::PinnableSlice pinned;
        const rocksdb::Status status = db->Get(rocksdb::ReadOptions(), Of(family), key, &pinned);
        if (status.ok()) {
            return true;
        }
        if (!status.IsNotFound()) {
            Fault();
        }
        return false;
    }

    /// Reads the layout stamp, writing it on a database that has none.
    ///
    /// A member rather than a free helper because it is an operation on this database's `meta`
    /// family, and because `Impl` is private: a free function could not name it.
    [[nodiscard]] std::optional<DbError> CheckSchema() {
        bool failed = false;
        std::string stored;
        if (Read(Family::Meta, AsSlice(KEY_SCHEMA), stored, failed)) {
            Reader reader(Borrow(stored));
            uint32_t version = 0;
            if (!reader.ReadU32(version) || !reader.Finish()) {
                return DbError::CorruptRecord;
            }
            // A stamp this build does not recognise is refused rather than upgraded in place.
            // There is nothing to migrate yet, and reading one layout's bytes under another
            // layout's rules is how a node quietly disagrees with itself about its own history.
            if (version != SCHEMA_VERSION) {
                return DbError::CorruptRecord;
            }
            return std::nullopt;
        }
        if (failed) {
            return DbError::ReadFailed;
        }

        Writer writer(sizeof(uint32_t));
        writer.WriteU32(SCHEMA_VERSION);
        if (!Write(Family::Meta, AsSlice(KEY_SCHEMA), AsSlice(writer.Bytes()))) {
            return DbError::WriteFailed;
        }
        return std::nullopt;
    }

    /// Reads the network stamp, writing it on a database that has none.
    [[nodiscard]] std::optional<DbError> CheckNetwork(const ChainParams& expected) {
        bool failed = false;
        std::string stored;
        if (Read(Family::Meta, AsSlice(KEY_CHAIN_ID), stored, failed)) {
            Reader reader(Borrow(stored));
            Hash256 recorded;
            if (!reader.ReadHash256(recorded) || !reader.Finish()) {
                return DbError::CorruptRecord;
            }
            if (recorded != expected.chain_id) {
                return DbError::WrongNetwork;
            }
            return std::nullopt;
        }
        if (failed) {
            return DbError::ReadFailed;
        }

        Writer writer(Hash256::SIZE);
        writer.WriteHash256(expected.chain_id);
        if (!Write(Family::Meta, AsSlice(KEY_CHAIN_ID), AsSlice(writer.Bytes()))) {
            return DbError::WriteFailed;
        }
        return std::nullopt;
    }

    ~Impl() {
        // Handles must go before the database they belong to, and the database must be closed
        // explicitly so that a failure to flush is observable rather than swallowed by a
        // destructor. `db` is released last, after every handle it owns is gone.
        for (rocksdb::ColumnFamilyHandle* handle : families) {
            if (handle != nullptr && db != nullptr) {
                (void)db->DestroyColumnFamilyHandle(handle);
            }
        }
        families.clear();
        if (db != nullptr) {
            (void)db->Close();
        }
    }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
};

ChainDb::ChainDb(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ChainDb::~ChainDb() = default;

std::expected<std::unique_ptr<ChainDb>, DbError> ChainDb::Open(const std::string& directory,
                                                              const ChainParams& params) {
    rocksdb::DBOptions options;
    options.create_if_missing = true;
    options.create_missing_column_families = true;

    // Report a damaged file rather than opening past it. Every other layer treats this
    // node's own storage as authoritative, so storage that has quietly lost records is the
    // one thing that must not be presented as if it had not.
    options.paranoid_checks = true;

    const rocksdb::ColumnFamilyOptions family_options;
    std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
    descriptors.reserve(FAMILY_COUNT);
    descriptors.emplace_back(rocksdb::kDefaultColumnFamilyName, family_options);
    for (const std::string_view name : {CF_COINS, CF_BLOCKS, CF_UNDO, CF_INDEX, CF_META}) {
        descriptors.emplace_back(std::string(name), family_options);
    }

    std::unique_ptr<Impl> impl = std::make_unique<Impl>();
    rocksdb::DB* raw = nullptr;
    const rocksdb::Status opened =
        rocksdb::DB::Open(options, directory, descriptors, &impl->families, &raw);
    if (!opened.ok() || raw == nullptr || impl->families.size() != FAMILY_COUNT) {
        return std::unexpected(DbError::CannotOpen);
    }
    impl->db.reset(raw);
    impl->params = &params;

    // Identity before anything else. A directory that exists at all is one whose network and
    // layout this build has agreed with, so no later read has to wonder.
    if (const std::optional<DbError> fault = impl->CheckSchema(); fault.has_value()) {
        return std::unexpected(*fault);
    }
    if (const std::optional<DbError> fault = impl->CheckNetwork(params); fault.has_value()) {
        return std::unexpected(*fault);
    }
    return std::unique_ptr<ChainDb>(new ChainDb(std::move(impl)));
}

// --- utxo::CoinsView --------------------------------------------------------

std::optional<Coin> ChainDb::GetCoin(const OutPoint& outpoint) const {
    const ByteVec key = Encode(outpoint, OutPoint::SERIALIZED_SIZE);
    bool failed = false;
    std::string value;
    if (!impl_->Read(Family::Coins, AsSlice(key), value, failed)) {
        return std::nullopt;
    }

    Reader reader(Borrow(value));
    Coin coin;
    if (!Coin::Deserialize(reader, coin, impl_->params->block_limits.tx.max_lock_program_size) ||
        !reader.Finish()) {
        // A coin this node wrote and cannot read back is its own state contradicting itself.
        // The latch is what stops that from reaching a validator as "not unspent", which would
        // rule out a block for spending a coin that is in fact there.
        impl_->Fault();
        return std::nullopt;
    }
    return coin;
}

bool ChainDb::HaveCoin(const OutPoint& outpoint) const {
    const ByteVec key = Encode(outpoint, OutPoint::SERIALIZED_SIZE);
    return impl_->Exists(Family::Coins, AsSlice(key));
}

// --- chain::BlockStore ------------------------------------------------------

bool ChainDb::HaveBlock(const Hash256& hash) const {
    return impl_->Exists(Family::Blocks, AsSlice(hash.Span()));
}

std::optional<Block> ChainDb::GetBlock(const Hash256& hash) const {
    bool failed = false;
    std::string value;
    if (!impl_->Read(Family::Blocks, AsSlice(hash.Span()), value, failed)) {
        return std::nullopt;
    }

    Reader reader(Borrow(value));
    Block block;
    if (!Block::Deserialize(reader, block, impl_->params->block_limits) || !reader.Finish()) {
        impl_->Fault();
        return std::nullopt;
    }

    // The key is the header's hash, so this checks that the record came back from the key it
    // was filed under. Ninety-two bytes of hashing, not a megabyte: the body is committed to
    // by the header's Merkle root, which `CheckBlock` verifies. Without it a body stored under
    // the wrong key would be connected in place of the block the index actually weighed.
    if (block.Hash() != hash) {
        impl_->Fault();
        return std::nullopt;
    }
    return block;
}

std::optional<utxo::BlockUndo> ChainDb::GetUndo(const Hash256& hash) const {
    bool failed = false;
    std::string value;
    if (!impl_->Read(Family::Undo, AsSlice(hash.Span()), value, failed)) {
        return std::nullopt;
    }

    Reader reader(Borrow(value));
    utxo::BlockUndo undo;
    if (!utxo::BlockUndo::Deserialize(reader, undo, *impl_->params) || !reader.Finish()) {
        impl_->Fault();
        return std::nullopt;
    }
    // Not checked against the block here: an undo record is not content-addressed, and the
    // check that matters is the one `utxo::DisconnectBlock` already makes — every coin it
    // restores must equal what the block says it created. A record that does not describe the
    // block is caught there, with the error that says so.
    return undo;
}

void ChainDb::PutBlock(const Hash256& hash, const Block& block) {
    // Written before the tip that will reference it, and that ordering is safe because a body
    // is content-addressed: a body stored under a hash the chain never reaches is an
    // unreferenced blob, whereas a tip whose body is missing is a contradiction.
    const ByteVec encoded = Encode(block);
    (void)impl_->Write(Family::Blocks, AsSlice(hash.Span()), AsSlice(encoded));
}

void ChainDb::PutUndo(const Hash256& hash, const utxo::BlockUndo& undo) {
    // Same ordering, and safe for a narrower reason: an undo record keyed by block hash is
    // read only when that block is disconnected, and nothing disconnects a block the stored
    // tip says was never connected. A refused write is latched, and `CommitTip` refuses to
    // advance the tip past it.
    const ByteVec encoded = Encode(undo);
    (void)impl_->Write(Family::Undo, AsSlice(hash.Span()), AsSlice(encoded));
}

// --- chain::ChainSink -------------------------------------------------------

bool ChainDb::CommitHeader(const chain::BlockIndexEntry& entry) {
    // Only what cannot be recomputed. Height is in the header, the parent link is its
    // `prev_block`, and total work is the sum over an ancestry the index rebuilds anyway —
    // storing those would be storing a second copy of a derivation, and a second copy is
    // something that can disagree. Arrival order and the two recorded judgements are the
    // facts that exist nowhere else.
    Writer writer(StoredHeader::SERIALIZED_SIZE);
    entry.header.Serialize(writer);
    writer.WriteU64(entry.sequence);
    writer.WriteU8(static_cast<uint8_t>(entry.validity));
    writer.WriteU8(static_cast<uint8_t>(entry.failure));
    const ByteVec encoded = writer.Take();
    return impl_->Write(Family::Index, AsSlice(entry.hash.Span()), AsSlice(encoded));
}

namespace {

/// A `utxo::CoinsSink` that writes into a RocksDB batch rather than into another cache.
///
/// This is the adaptor that makes the atomicity story work. `CoinsCache::Flush` emits a
/// sequence of "the truth about this outpoint is now X"; a batch accumulates them without
/// applying any, so the whole change set becomes one write that either lands or does not.
class BatchSink final : public utxo::CoinsSink {
public:
    BatchSink(rocksdb::WriteBatch& batch, rocksdb::ColumnFamilyHandle* coins) noexcept
        : batch_(&batch), coins_(coins) {}

    void Write(const OutPoint& outpoint, const std::optional<Coin>& coin) override {
        const ByteVec key = Encode(outpoint, OutPoint::SERIALIZED_SIZE);
        if (!coin.has_value()) {
            // A spend is a deletion. The set holds unspent outputs and nothing else, so a
            // coin that is gone is a key that is gone — there is no tombstone to keep, and
            // keeping one would make the stored set grow with the chain's history rather than
            // with its unspent output count.
            Record(batch_->Delete(coins_, AsSlice(key)));
            return;
        }
        const ByteVec encoded = Encode(*coin, Coin::MIN_SERIALIZED_SIZE);
        Record(batch_->Put(coins_, AsSlice(key), AsSlice(encoded)));
    }

    /// Whether any entry was refused. Latched, because `Write` implements an interface with
    /// nowhere to report failure and a batch missing one entry is not a smaller batch — it is
    /// a different chain's coins set.
    [[nodiscard]] bool Failed() const noexcept { return failed_; }

private:
    void Record(const rocksdb::Status& status) noexcept { failed_ = failed_ || !status.ok(); }

    rocksdb::WriteBatch* batch_;
    rocksdb::ColumnFamilyHandle* coins_;
    bool failed_ = false;
};

}  // namespace

bool ChainDb::CommitTip(utxo::CoinsCache& changes, const Hash256& tip) {
    // Refused on top of a loss rather than attempted. A block body or undo record whose write
    // was silently dropped leaves a database that will fail its next reorganisation with
    // `UndoRecordUnavailable`, and committing a tip is precisely the act that would make that
    // state the one a restart resumes from. Stopping now costs a restart; not stopping costs
    // the ability to reorganise, discovered at the worst possible moment.
    if (impl_->faulted) {
        return false;
    }

    rocksdb::WriteBatch batch;
    BatchSink sink(batch, impl_->Of(Family::Coins));
    changes.Flush(sink);
    if (sink.Failed()) {
        impl_->Fault();
        return false;
    }

    // The tip pointer goes into the same batch as the coins it describes. This is the entire
    // reason the five families share one database: RocksDB applies a batch spanning them whole
    // or not at all, so there is no instant at which the stored coins set belongs to one chain
    // and the stored tip names another. Two databases could not promise that, and the promise
    // is what `ChainState::ResumeAt` is built on.
    if (const rocksdb::Status staged =
            batch.Put(impl_->Of(Family::Meta), AsSlice(KEY_TIP), AsSlice(tip.Span()));
        !staged.ok()) {
        impl_->Fault();
        return false;
    }

    if (const rocksdb::Status written = impl_->db->Write(rocksdb::WriteOptions(), &batch);
        !written.ok()) {
        impl_->Fault();
        return false;
    }
    return true;
}

bool ChainDb::HasFault() const noexcept {
    return impl_->faulted;
}

// --- startup ----------------------------------------------------------------

std::expected<std::vector<StoredHeader>, DbError> ChainDb::LoadHeaders() const {
    const std::unique_ptr<rocksdb::Iterator> records(
        impl_->db->NewIterator(rocksdb::ReadOptions(), impl_->Of(Family::Index)));
    if (records == nullptr) {
        impl_->Fault();
        return std::unexpected(DbError::ReadFailed);
    }

    std::vector<StoredHeader> headers;
    for (records->SeekToFirst(); records->Valid(); records->Next()) {
        Reader reader(Borrow(records->value()));
        StoredHeader stored{};
        uint8_t validity = 0;
        uint8_t failure = 0;
        if (!BlockHeader::Deserialize(reader, stored.header) ||
            !reader.ReadU64(stored.sequence) || !reader.ReadU8(validity) ||
            !reader.ReadU8(failure) || !reader.Finish()) {
            impl_->Fault();
            return std::unexpected(DbError::CorruptRecord);
        }

        // Range-checked against the top of each enumeration rather than cast blindly. A byte
        // outside the range names no enumerator, and `IsEligible()` on such a value would be
        // asking a question about a state the type does not have.
        if (validity > static_cast<uint8_t>(chain::BlockValidity::Full) ||
            failure > static_cast<uint8_t>(chain::BlockFailure::Ancestor)) {
            impl_->Fault();
            return std::unexpected(DbError::CorruptRecord);
        }
        stored.validity = static_cast<chain::BlockValidity>(validity);
        stored.failure = static_cast<chain::BlockFailure>(failure);
        headers.push_back(stored);
    }

    // An iterator reports I/O failure by going invalid, which is the same thing it does at the
    // end of the family. Without this the two are indistinguishable, and a truncated read
    // would look like a shorter header tree.
    if (!records->status().ok()) {
        impl_->Fault();
        return std::unexpected(DbError::ReadFailed);
    }

    // RocksDB iterates in key order, which is hash order — arbitrary. Sorting restores the
    // arrival order that is half of the chain selection rule and a topological order of the
    // tree besides, so the replay below never meets a child before its parent.
    std::sort(headers.begin(), headers.end(),
              [](const StoredHeader& left, const StoredHeader& right) noexcept {
                  return left.sequence < right.sequence;
              });
    return headers;
}

std::expected<std::optional<Hash256>, DbError> ChainDb::LoadTip() const {
    bool failed = false;
    std::string value;
    if (!impl_->Read(Family::Meta, AsSlice(KEY_TIP), value, failed)) {
        if (failed) {
            return std::unexpected(DbError::ReadFailed);
        }
        // No record at all is a database that has never committed a tip, which is a fresh one.
        // Not an error, and not the same thing as a record that would not decode.
        return std::optional<Hash256>{};
    }

    Reader reader(Borrow(value));
    Hash256 tip;
    if (!reader.ReadHash256(tip) || !reader.Finish()) {
        impl_->Fault();
        return std::unexpected(DbError::CorruptRecord);
    }
    return std::optional<Hash256>{tip};
}

bool ChainDb::Sync() {
    // Every column family, then the write-ahead log. Flushing a family turns its memtable into
    // an SST file the operating system holds; syncing the log covers anything written since the
    // last flush. Both are needed, because a commit is durable through either route and this
    // call is about leaving nothing that depends on the machine staying up.
    rocksdb::FlushOptions options;
    options.wait = true;

    bool flushed = true;
    for (const Family family :
         {Family::Coins, Family::Blocks, Family::Undo, Family::Index, Family::Meta}) {
        if (!impl_->db->Flush(options, impl_->Of(family)).ok()) {
            flushed = false;
        }
    }
    if (!impl_->db->SyncWAL().ok()) {
        flushed = false;
    }
    if (!flushed) {
        impl_->Fault();
    }
    return flushed;
}

std::expected<LoadedChain, DbError>
LoadChain(const ChainDb& db, int64_t now, const ChainParams& params) {
    const std::expected<std::vector<StoredHeader>, DbError> stored = db.LoadHeaders();
    if (!stored.has_value()) {
        return std::unexpected(stored.error());
    }
    const std::expected<std::optional<Hash256>, DbError> committed = db.LoadTip();
    if (!committed.has_value()) {
        return std::unexpected(committed.error());
    }

    // The replay clock. Every header rule but one is a fact about the header, and a fact does
    // not change while a node is switched off; the exception compares the timestamp against the
    // clock, so a machine whose time was corrected backwards would refuse headers it accepted
    // an hour earlier — up to and including its own committed tip, whose coins are already on
    // disk and cannot be un-applied. Advancing the replay clock to the highest stored timestamp
    // satisfies that one ceiling for every stored header, and leaves every other rule to judge
    // them exactly as it did when they arrived.
    int64_t clock = now;
    for (const StoredHeader& record : *stored) {
        clock = std::max(clock, record.header.timestamp);
    }
    // `ContextualCheckBlockHeader` adds `MAX_FUTURE_BLOCK_SECONDS` to this and rejects
    // everything if that addition would overflow, so the clock is held below where it would.
    clock = std::min(clock, std::numeric_limits<int64_t>::max() - MAX_FUTURE_BLOCK_SECONDS);

    LoadedChain loaded{
        .index = chain::BlockIndex::ForNetwork(params),
        .tip = nullptr,
        .restored = 0,
    };

    // Headers first, in arrival order, each through the same `AddHeader` a relayed header goes
    // through. A tampered header does not become valid by having been written to this node's
    // own database, and this is where that is established.
    for (const StoredHeader& record : *stored) {
        // Genesis is a chain parameter, already seeded, and not a header that arrived from
        // anywhere. Compared by value rather than by height, so that a stored record claiming
        // height zero while being some other block is left to `AddHeader` to refuse.
        if (record.header == loaded.index.Genesis().header) {
            continue;
        }
        const std::expected<const chain::BlockIndexEntry*, chain::HeaderError> added =
            loaded.index.AddHeader(record.header, clock, params);
        if (!added.has_value()) {
            return std::unexpected(DbError::CorruptRecord);
        }
        loaded.index.RecordValidity(**added, record.validity);
        ++loaded.restored;
    }

    // Failures second, once every header is in place. Only a block ruled out by its *own* rules
    // is replayed: an `Ancestor` failure is derived, and the index derives it again here by
    // propagation, exactly as it did the first time. Replaying a derived failure as if it were
    // the block's own would blame a header for something that was never wrong with it, and
    // doing either pass in one loop with the one above would make `AddHeader` refuse the
    // descendants outright — losing headers this node genuinely knows.
    for (const StoredHeader& record : *stored) {
        if (record.failure != chain::BlockFailure::Itself) {
            continue;
        }
        const chain::BlockIndexEntry* const ruled_out = loaded.index.Find(record.header.Hash());
        if (ruled_out == nullptr) {
            return std::unexpected(DbError::CorruptRecord);
        }
        (void)loaded.index.RecordFailure(*ruled_out);
    }

    if (!committed->has_value()) {
        // A database that has never committed a tip. Its coins set is empty, which is the set as
        // of genesis, so genesis is where this node resumes — the same state a fresh
        // `ChainState` starts in.
        loaded.tip = &loaded.index.Genesis();
        return loaded;
    }

    const chain::BlockIndexEntry* const resumed = loaded.index.Find(**committed);
    if (resumed == nullptr) {
        return std::unexpected(DbError::TipNotIndexed);
    }
    if (!resumed->IsEligible()) {
        // The coins set on disk is the set as of a block this node now rules out, so it
        // describes a chain nothing may be built on. Reported rather than resumed from: there is
        // no correct continuation from a state whose starting point is invalid.
        return std::unexpected(DbError::CorruptRecord);
    }
    loaded.tip = resumed;
    return loaded;
}

}  // namespace amarian::storage


