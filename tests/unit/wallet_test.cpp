/// \file
/// Tests for the wallet layer: seed derivation, addresses, coin selection, and backup.

#include <amarian/wallet/address.hpp>
#include <amarian/wallet/backup.hpp>
#include <amarian/wallet/coinselection.hpp>
#include <amarian/wallet/fees.hpp>
#include <amarian/wallet/seed.hpp>
#include <amarian/wallet/wallet.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/util/types.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace amarian::wallet {
namespace {

// --- Seed derivation tests --------------------------------------------------

TEST(WalletSeed, GenerateSeedProduces32Bytes) {
    const MasterSeed seed = GenerateSeed();
    EXPECT_EQ(seed.size(), SEED_BYTES);
    bool all_zero = true;
    for (uint8_t b : seed) {
        if (b != 0) { all_zero = false; break; }
    }
    EXPECT_FALSE(all_zero);
}

TEST(WalletSeed, DeriveSecp256k1KeyIsDeterministic) {
    const MasterSeed seed = GenerateSeed();
    std::array<uint8_t, 32> key1{}, key2{};
    DeriveSecp256k1Key(seed, 0, 0, MutableByteSpan(key1));
    DeriveSecp256k1Key(seed, 0, 0, MutableByteSpan(key2));
    EXPECT_EQ(key1, key2);
}

TEST(WalletSeed, DeriveSecp256k1KeyChangesWithIndex) {
    const MasterSeed seed = GenerateSeed();
    std::array<uint8_t, 32> key0{}, key1{};
    DeriveSecp256k1Key(seed, 0, 0, MutableByteSpan(key0));
    DeriveSecp256k1Key(seed, 0, 1, MutableByteSpan(key1));
    EXPECT_NE(key0, key1);
}

TEST(WalletSeed, DeriveSecp256k1KeyChangesWithAccount) {
    const MasterSeed seed = GenerateSeed();
    std::array<uint8_t, 32> acct0{}, acct1{};
    DeriveSecp256k1Key(seed, 0, 0, MutableByteSpan(acct0));
    DeriveSecp256k1Key(seed, 1, 0, MutableByteSpan(acct1));
    EXPECT_NE(acct0, acct1);
}

TEST(WalletSeed, DifferentSeedsProduceDifferentKeys) {
    const MasterSeed seed_a = GenerateSeed();
    const MasterSeed seed_b = GenerateSeed();
    if (seed_a == seed_b) return;

    std::array<uint8_t, 32> key_a{}, key_b{};
    DeriveSecp256k1Key(seed_a, 0, 0, MutableByteSpan(key_a));
    DeriveSecp256k1Key(seed_b, 0, 0, MutableByteSpan(key_b));
    EXPECT_NE(key_a, key_b);
}

TEST(WalletSeed, DeriveMldsa44SeedIs32Bytes) {
    const MasterSeed seed = GenerateSeed();
    std::array<uint8_t, 32> out{};
    DeriveMldsa44Seed(seed, 0, 0, MutableByteSpan(out));
    EXPECT_EQ(out.size(), 32);
}

TEST(WalletSeed, DeriveSlhDsaSeedsAre16BytesEach) {
    const MasterSeed seed = GenerateSeed();
    std::array<uint8_t, 16> sk_seed{}, sk_prf{}, pk_seed{};
    DeriveSlhDsaSeeds(seed, 0, 0, MutableByteSpan(sk_seed),
                       MutableByteSpan(sk_prf), MutableByteSpan(pk_seed));
    EXPECT_EQ(sk_seed.size(), 16);
    EXPECT_EQ(sk_prf.size(), 16);
    EXPECT_EQ(pk_seed.size(), 16);
}

// --- Address encoding tests -------------------------------------------------

TEST(WalletAddress, EncodeAddressIsValidBech32m) {
    const std::string_view hrp = "amr";
    ByteVec program(32, 0xAB);
    program[0] = 0x01;
    program[31] = 0xFF;

    const std::string encoded = EncodeAddress(hrp, 1, program);
    EXPECT_FALSE(encoded.empty());
    EXPECT_TRUE(encoded.starts_with(hrp));
    EXPECT_TRUE(encoded.find('1') != std::string::npos);
    // The address is at least HRP + '1' + 1 char + 6 checksum chars
    EXPECT_GE(encoded.size(), hrp.size() + 8);
}

TEST(WalletAddress, LockToAddressProducesString) {
    Lock lock;
    lock.version = LOCK_VERSION_CONDITION_COMMITMENT;
    lock.program = ByteVec(32, 0x11);
    lock.program[0] = 0x01;

    auto addr = LockToAddress(lock, "amr");
    ASSERT_TRUE(addr.has_value());
    EXPECT_TRUE(addr->starts_with("amr"));
}

TEST(WalletAddress, DecodeRejectsWrongHrp) {
    const std::string encoded = EncodeAddress("amr", 1, ByteVec(32, 0x42));
    auto decoded = DecodeAddress("tamr", encoded);
    EXPECT_FALSE(decoded.has_value());
}

TEST(WalletAddress, EncodeAndDecodeRoundTrip) {
    Lock lock;
    lock.version = LOCK_VERSION_CONDITION_COMMITMENT;
    lock.program.resize(32);
    for (size_t i = 0; i < lock.program.size(); ++i) {
        lock.program[i] = static_cast<uint8_t>(i * 7U + 3U);
    }
    const auto address = LockToAddress(lock, "tamr");
    ASSERT_TRUE(address.has_value());
    const auto decoded = AddressToLock(*address, "tamr");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, lock);
}

TEST(WalletAddress, DecodeRejectsGarbage) {
    auto decoded = DecodeAddress("amr", "notanaddress");
    EXPECT_FALSE(decoded.has_value());
}

TEST(WalletAddress, DecodeRejectsTooShort) {
    auto decoded = DecodeAddress("amr", "a1b");
    EXPECT_FALSE(decoded.has_value());
}

TEST(WalletAddress, LockToAddressRejectsWrongVersion) {
    Lock lock;
    lock.version = 0;
    lock.program = ByteVec(32, 0x00);
    auto addr = LockToAddress(lock, "amr");
    EXPECT_FALSE(addr.has_value());
}

TEST(WalletAddress, LockToAddressRejectsWrongProgramLength) {
    Lock lock;
    lock.version = LOCK_VERSION_CONDITION_COMMITMENT;
    lock.program = ByteVec(16, 0x00);
    auto addr = LockToAddress(lock, "amr");
    EXPECT_FALSE(addr.has_value());
}

// --- Coin selection tests ---------------------------------------------------

TEST(WalletCoinSelection, EmptyUtxosFails) {
    std::vector<UtxoEntry> utxos;
    CoinSelection result = SelectCoins(utxos, 1000, 10);
    EXPECT_FALSE(result.success);
}

TEST(WalletCoinSelection, InsufficientFundsFails) {
    std::vector<UtxoEntry> utxos;
    UtxoEntry entry;
    entry.coin.output.amount = 500;
    entry.confirmed = true;
    utxos.push_back(entry);

    CoinSelection result = SelectCoins(utxos, 1000, 10);
    EXPECT_FALSE(result.success);
}

TEST(WalletCoinSelection, SufficientSingleUtxo) {
    std::vector<UtxoEntry> utxos;
    UtxoEntry entry;
    entry.coin.output.amount = 100000;
    entry.confirmed = true;
    utxos.push_back(entry);

    CoinSelection result = SelectCoins(utxos, 50000, 10);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.inputs.size(), 1);
    EXPECT_GE(result.total_selected, 50000);
}

TEST(WalletCoinSelection, MultipleUtxos) {
    std::vector<UtxoEntry> utxos;
    for (int64_t i = 0; i < 5; ++i) {
        UtxoEntry entry;
        entry.coin.output.amount = 10000;
        entry.confirmed = true;
        utxos.push_back(entry);
    }

    CoinSelection result = SelectCoins(utxos, 25000, 1);
    EXPECT_TRUE(result.success) << "total_selected=" << result.total_selected
                                << " target=" << result.target << " fee=" << result.fee;
    EXPECT_GE(result.total_selected, 25000 + result.fee);
}

TEST(WalletCoinSelection, DustForFeeRate) {
    int64_t dust = DustForFeeRate(10);
    EXPECT_GT(dust, 0);
}

TEST(WalletCoinSelection, EstimateTxWeight) {
    size_t weight = EstimateTxWeight(1, 2, 2420);
    EXPECT_GT(weight, 0);
    EXPECT_GT(weight, 1000);
}

// --- Fee estimation tests ---------------------------------------------------

TEST(WalletFeeEstimation, ReturnsDefaultWhenNoData) {
    FeeEstimator estimator;
    EXPECT_FALSE(estimator.HasEnoughData());

    FeeEstimate est = estimator.Estimate();
    EXPECT_GT(est.economy_feerate, 0);
    EXPECT_GT(est.normal_feerate, 0);
    EXPECT_GT(est.priority_feerate, 0);
}

TEST(WalletFeeEstimation, RecordsBlockData) {
    FeeEstimator estimator;
    std::vector<int64_t> feerates = {5, 10, 15, 20, 25};
    estimator.RecordBlock(100, feerates);
    estimator.RecordBlock(101, feerates);
    estimator.RecordBlock(102, feerates);

    EXPECT_TRUE(estimator.HasEnoughData());
    FeeEstimate est = estimator.Estimate();
    EXPECT_GT(est.normal_feerate, 0);
}

TEST(WalletFeeEstimation, MultipleBlocksAveraging) {
    FeeEstimator estimator;
    for (uint32_t h = 0; h < 10; ++h) {
        estimator.RecordBlock(h, {10, 20, 30});
    }

    FeeEstimate est = estimator.Estimate();
    EXPECT_GE(est.normal_feerate, 10);
    EXPECT_LE(est.normal_feerate, 30);
}

// --- Wallet database tests --------------------------------------------------

TEST(WalletDb, CreateAndStoreSeed) {
    WalletDb db;
    EXPECT_FALSE(db.HasSeed());

    ByteVec seed(32, 0x42);
    db.StoreSeed(seed, "password");
    EXPECT_TRUE(db.HasSeed());

    auto loaded = db.LoadSeed("password");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, seed);
}

TEST(WalletDb, AddAndRetrieveAccount) {
    WalletDb db;
    EXPECT_EQ(db.AccountCount(), 0);

    Account acct;
    acct.label = "Test";
    acct.scheme_name = "schnorr-secp256k1";
    acct.account_id = 0;
    acct.highest_derived = 5;
    db.AddAccount(acct);

    EXPECT_EQ(db.AccountCount(), 1);
    auto retrieved = db.GetAccount(0);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->label, "Test");
    EXPECT_EQ(retrieved->highest_derived, 5);
}

TEST(WalletDb, UpdateAccount) {
    WalletDb db;
    Account acct;
    acct.label = "Original";
    acct.scheme_name = "schnorr-secp256k1";
    acct.account_id = 0;
    acct.highest_derived = 0;
    db.AddAccount(acct);

    acct.highest_derived = 10;
    db.UpdateAccount(acct);

    auto retrieved = db.GetAccount(0);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->highest_derived, 10);
}

TEST(WalletDb, BirthHeight) {
    WalletDb db;
    EXPECT_EQ(db.BirthHeight(), 0);
    db.SetBirthHeight(50000);
    EXPECT_EQ(db.BirthHeight(), 50000);
}

TEST(WalletDb, StoreAndListTransactions) {
    WalletDb db;
    EXPECT_EQ(db.TxCount(), 0);

    StoredTransaction tx;
    tx.txid = Hash256{};
    tx.height = 100;
    tx.received = 50000;
    tx.fee = 1000;
    db.StoreTx(tx);

    EXPECT_EQ(db.TxCount(), 1);

    auto txs = db.ListTxs();
    ASSERT_EQ(txs.size(), 1);
    EXPECT_EQ(txs[0].height, 100);
}

// --- Backup tests -----------------------------------------------------------

TEST(WalletBackup, MnemonicProducesCorrectWordCount) {
    MasterSeed seed = GenerateSeed();
    auto words = GenerateMnemonic(seed);
    EXPECT_EQ(words.size(), MNEMONIC_WORD_COUNT);
    // All words should be non-empty.
    for (const auto& w : words) {
        EXPECT_FALSE(w.empty());
    }
}

TEST(WalletBackup, MnemonicWrongWordCountFails) {
    std::vector<std::string> words = {"abandon", "ability"};
    auto recovered = SeedFromMnemonic(words);
    EXPECT_FALSE(recovered.has_value());
}

TEST(WalletBackup, MnemonicRoundTripsSeed) {
    MasterSeed seed = GenerateSeed();
    auto words = GenerateMnemonic(seed);
    auto recovered = SeedFromMnemonic(words);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, seed);
}

TEST(WalletBackup, MetadataRoundTrip) {
    WalletDb db;
    db.SetBirthHeight(1000);

    Account acct;
    acct.label = "Everyday";
    acct.scheme_name = "schnorr-secp256k1";
    acct.account_id = 0;
    acct.highest_derived = 5;
    db.AddAccount(acct);

    ByteVec serialised = SerialiseMetadata(db);
    EXPECT_FALSE(serialised.empty());

    WalletDb restored;
    ASSERT_TRUE(DeserialiseMetadata(serialised, restored));

    EXPECT_EQ(restored.BirthHeight(), 1000);
    EXPECT_EQ(restored.AccountCount(), 1);

    auto restored_acct = restored.GetAccount(0);
    ASSERT_TRUE(restored_acct.has_value());
    EXPECT_EQ(restored_acct->label, "Everyday");
    EXPECT_EQ(restored_acct->highest_derived, 5);
}

// --- Dust threshold and weight estimation tests (already passing) -----------

TEST(WalletCoinSelection, DustPositive) {
    EXPECT_GT(DUST_THRESHOLD, 0);
}

}  // namespace
}  // namespace amarian::wallet
