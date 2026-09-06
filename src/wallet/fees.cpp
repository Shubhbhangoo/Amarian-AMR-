/// \file
/// Fee estimation from observed inclusion in recent blocks.

#include <amarian/wallet/fees.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace amarian::wallet {

void FeeEstimator::RecordBlock(uint32_t height, const std::vector<int64_t>& tx_feerates) {
    if (tx_feerates.empty()) return;

    FeeSample sample;
    sample.height = height;
    sample.tx_count = tx_feerates.size();

    // Sort to compute percentiles.
    std::vector<int64_t> sorted = tx_feerates;
    std::sort(sorted.begin(), sorted.end());

    // Median.
    sample.median_feerate = sorted[sorted.size() / 2];

    // 10th percentile.
    const size_t p10_idx = sorted.size() / 10;
    sample.p10_feerate = sorted[std::min(p10_idx, sorted.size() - 1)];

    samples_.push_back(sample);

    // Trim the window.
    if (samples_.size() > WINDOW_SIZE) {
        samples_.erase(samples_.begin());
    }
}

FeeEstimate FeeEstimator::Estimate() const {
    FeeEstimate estimate;

    if (samples_.empty()) {
        estimate.economy_feerate = DEFAULT_FEERATE;
        estimate.normal_feerate = DEFAULT_FEERATE * 2;
        estimate.priority_feerate = DEFAULT_FEERATE * 5;
        return estimate;
    }

    // Compute mean of p10 for economy, mean of median for normal, mean of
    // the top decile for priority, over the most recent samples.
    const size_t recent = std::min(samples_.size(), size_t{10});

    int64_t sum_p10 = 0;
    int64_t sum_median = 0;
    int64_t sum_p90 = 0;

    for (size_t i = samples_.size() - recent; i < samples_.size(); ++i) {
        sum_p10 += samples_[i].p10_feerate;
        sum_median += samples_[i].median_feerate;
        // Estimate p90 as 2x median (simplified).
        sum_p90 += samples_[i].median_feerate * 2;
    }

    const int64_t count = static_cast<int64_t>(recent);
    estimate.economy_feerate = sum_p10 / count;
    estimate.normal_feerate = sum_median / count;
    estimate.priority_feerate = sum_p90 / count;

    // Ensure minimums.
    estimate.economy_feerate = std::max(estimate.economy_feerate, MinFeerate());
    estimate.normal_feerate = std::max(estimate.normal_feerate, MinFeerate());
    estimate.priority_feerate = std::max(estimate.priority_feerate, MinFeerate());

    // Block estimate: economy ~6 blocks, normal ~3, priority ~1.
    estimate.economy_blocks = 6;
    estimate.normal_blocks = 3;
    estimate.priority_blocks = 1;

    return estimate;
}

}  // namespace amarian::wallet