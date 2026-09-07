#pragma once

/// \file
/// Fee estimation from observed inclusion.
///
/// Estimates the fee rate needed for a given confirmation target based on a
/// rolling window of recent blocks. Presented to the user as three choices:
/// economical, normal, and priority - with estimated confirmation time.

#include <amarian/primitives/transaction.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace amarian::wallet {

/// One block's worth of fee-rate observation.
struct FeeSample {
    /// Block height this sample came from.
    uint32_t height;
    /// The median fee rate of transactions that confirmed in this block.
    int64_t median_feerate;
    /// The 10th-percentile fee rate (cheapest that got in).
    int64_t p10_feerate;
    /// Number of transactions in this block.
    size_t tx_count;
};

/// Fee estimates for three confirmation targets.
struct FeeEstimate {
    /// Fee rate for economy (slow but cheap), in facets/weight.
    int64_t economy_feerate = 0;
    /// Fee rate for normal (medium trade-off).
    int64_t normal_feerate = 0;
    /// Fee rate for priority (fast confirmation).
    int64_t priority_feerate = 0;
    /// Estimated number of blocks for economy.
    uint32_t economy_blocks = 0;
    /// Estimated number of blocks for normal.
    uint32_t normal_blocks = 0;
    /// Estimated number of blocks for priority.
    uint32_t priority_blocks = 0;
};

/// Fee estimator over a rolling window of recent blocks.
class FeeEstimator {
public:
    /// Size of the rolling window (in blocks).
    static constexpr size_t WINDOW_SIZE = 100;

    FeeEstimator() = default;

    /// Records one block's fee-rate statistics.
    void RecordBlock(uint32_t height, const std::vector<int64_t>& tx_feerates);

    /// Estimates the current fee rates.
    [[nodiscard]] FeeEstimate Estimate() const;

    /// Whether enough data has been seen to produce a meaningful estimate.
    [[nodiscard]] bool HasEnoughData() const noexcept { return samples_.size() >= 3; }

    /// Minimum relay feerate, as a fallback when data is insufficient.
    [[nodiscard]] int64_t MinFeerate() const noexcept { return 1; }

private:
    std::vector<FeeSample> samples_;
};

/// The default fee rate to use when the estimator has no data yet.
inline constexpr int64_t DEFAULT_FEERATE = 10;  // 10 facets/weight

}  // namespace amarian::wallet
