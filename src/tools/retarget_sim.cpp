/// \file
/// A deterministic hashrate simulation for the difficulty-retargeting rule.
///
/// The Phase 3 acceptance criterion is that the choice of difficulty algorithm
/// (and of its parameters) rests on simulated hashrate rather than on the paper
/// that proposed it. This tool is that evidence: it links the *real* consensus
/// retarget — `chain::NextTargetBits`, the exact seam the assembler and the
/// validator use — and drives it with a synthetic chain whose per-block
/// inter-arrival times are exponential samples from the target each block faces.
///
/// Model
/// -----
/// A block at target `T` (decoded from the bits the rule returned) costs
/// `Work(T) = floor(2^256 / (T + 1))` expected hashes. With a hashrate that is
/// some multiple `m(t)` of the rate that would produce one block per
/// `target_block_seconds` at the network floor `T0`, the mean inter-arrival time
/// is
///
///     mean(t) = target_block_seconds * Work(T) / (Work(T0) * m(t))
///
/// The simulation samples an exponential inter-arrival from that mean, advances
/// the block time, and feeds the new tip's height and timestamp to the retarget
/// for the next block — exactly the information `NextTargetBits` uses on a real
/// node. Nothing is hashed; the difficulty, however, is the consensus function.
///
/// Determinism
/// -----------
/// Every random draw comes from a counter-mode SplitMix64 stream seeded by
/// `--seed`; two runs with the same seed and arguments print identical numbers.
/// Nothing reads a clock and nothing touches the network.
///
/// Output
/// ------
/// The run is divided into ten equal segments; each segment's mean interval is
/// printed, which shows a step change converging segment by segment and an
/// oscillation's shape over time. The tail is the last `--window` blocks (for
/// oscillation, pass a multiple of two periods so both phases are equally
/// represented), and its median, p99 and maximum gap are reported, along with
/// `static_mean` — the mean the same hashrate would have produced at a fixed
/// difficulty — and the ratio between the two, which is the retarget's own
/// contribution. Under a well-tuned rule the segment means all sit near the 300 s
/// target whenever the hashrate is near the baseline, and the ratio stays close to
/// 1 when it is not; a rule with
/// too short a half-life shows the oscillation's alternating gaps in the segment
/// means, and one with too long a half-life shows a long, slow recovery after a
/// step and long easy-block stretches under oscillation.
///
/// Scenarios
/// ---------
///   steady       constant hashrate; checks that the mean interval converges to
///                the target and difficulty stays put.
///   step         hashrate steps to `--mult` at the half-height of the run and
///                stays there; measures how the schedule recovers.
///   oscillation  hashrate alternates between `--hi` and `--lo` every `--period`
///                blocks — the switch-mining behaviour a small chain invites,
///                and the case the half-life choice exists to survive. The
///                defaults average to a hashrate of 1.0, but that does *not* mean
///                the mean interval should come out at the target: intervals go
///                as 1/hashrate, so the half of the run spent at `lo = 0.05`
///                dominates the average and inflates it by more than a factor of
///                ten before the rule does anything at all. `static_mean` in the
///                output is that inflation on its own, and the ratio printed
///                beside it is the only part attributable to the retarget.
///   pulse        hashrate is `--mult` for `--period` blocks a third of the way
///                in and 1.0 either side; the shock a rented burst of hashrate
///                is, and the case that shows how long the chain stays slow
///                after the burst leaves.

#include <amarian/chain/block_index.hpp>
#include <amarian/consensus/asert.hpp>
#include <amarian/consensus/params.hpp>
#include <amarian/consensus/target.hpp>
#include <amarian/consensus/work.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    int64_t half_life_seconds = 172800;
    int64_t blocks = 120000;
    int64_t window = 10000;
    uint64_t seed = 1;
    std::string scenario = "steady";
    double mult = 2.0;    // step: hashrate multiplier for the second half
    double hi = 1.95;     // oscillation: high phase hashrate
    double lo = 0.05;     // oscillation: low phase hashrate
    int64_t period = 100; // oscillation: blocks per half-cycle
};

constexpr int SEGMENTS = 10;

[[nodiscard]] uint64_t SplitMix64(uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/// A uniform draw from `[0, 1)`.
[[nodiscard]] double Uniform(uint64_t& state) noexcept {
    return static_cast<double>(SplitMix64(state) >> 11) * 0x1.0p-53;
}

/// An exponential inter-arrival with the given mean, inverse-CDF over the
/// deterministic stream.
[[nodiscard]] double Exponential(double mean, uint64_t& state) noexcept {
    return -std::log(1.0 - Uniform(state)) * mean;
}

/// The exact 256-bit amount of `work` as a `long double`. Simulation policy,
/// not consensus: the rule compares `Work` exactly, and only the statistical
/// model needs a real ratio.
[[nodiscard]] long double WorkAsFloat(const amarian::Work& work) noexcept {
    long double value = 0.0L;
    for (const uint8_t byte : work.BigEndian()) {
        value = value * 256.0L + static_cast<long double>(byte);
    }
    return value;
}

/// A minimal tip holding just the fields the retarget reads.
struct Tip {
    uint32_t height = 0;
    int64_t timestamp = 0;
};

[[nodiscard]] amarian::chain::BlockIndexEntry AsEntry(const Tip& tip) {
    amarian::chain::BlockIndexEntry entry;
    // `NextTargetBits` reads the *entry's* height field (the field the index
    // maintains), not the header's copy, so both must be set for the seam to see
    // the height the synthetic tip claims.
    entry.height = tip.height;
    entry.header.height = tip.height;
    entry.header.timestamp = tip.timestamp;
    return entry;
}

/// The hashrate multiplier the block about to be mined at `height` faces.
///
/// The scenario name is known-good by the time this runs: `ParseAndRun` refuses one it does
/// not recognise. That check belongs there rather than here because this is called once per
/// simulated block, and because a name this function did not recognise would have to either
/// pick a scenario anyway — silently running a different experiment than the one asked for —
/// or fail in the middle of a run that has already printed half its output.
[[nodiscard]] double HashrateAt(const Options& options, int64_t height) noexcept {
    if (options.scenario == "steady") {
        return 1.0;
    }
    if (options.scenario == "step") {
        return height < options.blocks / 2 ? 1.0 : options.mult;
    }
    if (options.scenario == "pulse") {
        const int64_t start = options.blocks / 3;
        return height >= start && height < start + options.period ? options.mult : 1.0;
    }
    // "oscillation": `options.period` blocks at `lo`, then `period` blocks at `hi`, repeating.
    const int64_t cycle = (height / options.period) % 2;
    return cycle == 0 ? options.lo : options.hi;
}

/// The scenarios `HashrateAt` implements. Checked before a run rather than during one.
[[nodiscard]] bool IsKnownScenario(const std::string& scenario) noexcept {
    return scenario == "steady" || scenario == "step" || scenario == "pulse" ||
           scenario == "oscillation";
}

struct Results {
    std::vector<double> segment_means;
    double p50 = 0.0;
    double p99 = 0.0;
    double max_gap = 0.0;
    double tail_mean = 0.0;
    /// The mean interval the same hashrate would have produced with difficulty pinned at
    /// the floor — the reference `tail_mean` has to be read against. See `Run`.
    double tail_static_mean = 0.0;
};

Results RunScenario(const Options& options) {
    amarian::ChainParams params = amarian::MAINNET_PARAMS;
    params.asert_half_life_seconds = options.half_life_seconds;

    const amarian::Work floor_work = amarian::Work::OfCompactTarget(params.pow_limit_bits);
    const long double floor_float = WorkAsFloat(floor_work);
    const double target_seconds = static_cast<double>(params.target_block_seconds);

    uint64_t rng = options.seed;
    Tip tip;
    tip.timestamp = params.genesis_timestamp;

    std::vector<double> gaps;
    gaps.reserve(static_cast<size_t>(options.blocks));
    // What each block's expected interval would have been with difficulty pinned at the
    // floor. Kept per block rather than as a running total because only the tail window is
    // reported, and for an oscillation the window's position within a cycle changes it.
    std::vector<double> static_means;
    static_means.reserve(static_cast<size_t>(options.blocks));

    for (int64_t height = 1; height <= options.blocks; ++height) {
        tip.height = static_cast<uint32_t>(height - 1);
        const uint32_t bits = amarian::chain::NextTargetBits(AsEntry(tip), params);
        const amarian::Work block_work = amarian::Work::OfCompactTarget(bits);
        const long double ratio = WorkAsFloat(block_work) / floor_float;
        const double hashrate = HashrateAt(options, height - 1);
        const double mean = target_seconds * static_cast<double>(ratio) / hashrate;
        const double gap = Exponential(mean, rng);
        tip.timestamp += static_cast<int64_t>(gap + 0.5);
        gaps.push_back(gap);
        static_means.push_back(target_seconds / hashrate);
    }

    Results results;
    const size_t segment_size = gaps.size() / static_cast<size_t>(SEGMENTS);
    for (int segment = 0; segment < SEGMENTS; ++segment) {
        double sum = 0.0;
        const size_t begin = static_cast<size_t>(segment) * segment_size;
        const size_t end = begin + segment_size;
        for (size_t i = begin; i < end; ++i) {
            sum += gaps[i];
        }
        results.segment_means.push_back(sum / static_cast<double>(segment_size));
    }

    const size_t tail_blocks =
        static_cast<size_t>(std::min<int64_t>(options.window, options.blocks));
    std::vector<double> tail(gaps.end() - static_cast<long>(tail_blocks), gaps.end());
    std::sort(tail.begin(), tail.end());

    double tail_sum = 0.0;
    for (const double gap : tail) {
        tail_sum += gap;
    }
    results.tail_mean = tail_sum / static_cast<double>(tail.size());

    double static_sum = 0.0;
    for (size_t i = static_means.size() - tail_blocks; i < static_means.size(); ++i) {
        static_sum += static_means[i];
    }
    results.tail_static_mean = static_sum / static_cast<double>(tail_blocks);

    results.p50 = tail[tail.size() / 2];
    results.p99 = tail[static_cast<size_t>(0.99 * static_cast<double>(tail.size() - 1))];
    results.max_gap = tail.back();
    return results;
}

int Run(const Options& options) {
    const Results results = RunScenario(options);

    std::cout << "scenario " << options.scenario << "  tau=" << options.half_life_seconds
              << "s  seed=" << options.seed << "\n  segment means (s):";
    std::cout << std::fixed << std::setprecision(0);
    for (const double mean : results.segment_means) {
        std::cout << ' ' << mean;
    }
    std::cout << std::fixed << std::setprecision(1) << "\n  tail_mean=" << results.tail_mean
              << "s  p50=" << results.p50 << "s  p99=" << results.p99
              << "s  max=" << results.max_gap << "s\n";

    // `static_mean` is what the same hashrate would have produced with difficulty pinned at
    // the floor, and it is the only honest reference for `tail_mean`: a scenario that spends
    // half its blocks at a fraction of the baseline hashrate inflates the *mean* interval
    // enormously all by itself, because intervals go as 1/hashrate, and none of that
    // inflation is the rule's doing. The ratio of the two is the retarget's own contribution,
    // and which direction is the good one depends on the scenario: where the hashrate has
    // risen above the baseline a ratio near that rise is the rule correctly taking the
    // difficulty up with it, and where the hashrate is at or below the baseline a ratio above
    // 1 is whiplash — the chain left slower than a fixed difficulty would have left it.
    std::cout << "  static_mean=" << results.tail_static_mean << "s  retarget_ratio="
              << std::setprecision(3)
              << (results.tail_static_mean > 0.0 ? results.tail_mean / results.tail_static_mean
                                                 : 0.0)
              << "\n";
    return 0;
}

int ParseAndRun(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto next_value = [&](const std::string& flag) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (argument == "--tau") {
            options.half_life_seconds = std::stoll(next_value("--tau"));
        } else if (argument == "--blocks") {
            options.blocks = std::stoll(next_value("--blocks"));
        } else if (argument == "--window") {
            options.window = std::stoll(next_value("--window"));
        } else if (argument == "--seed") {
            options.seed = std::stoull(next_value("--seed"));
        } else if (argument == "--scenario") {
            options.scenario = next_value("--scenario");
            if (!IsKnownScenario(options.scenario)) {
                // Refused rather than defaulted. This tool's output is the recorded evidence
                // for a consensus parameter, and a mistyped scenario that quietly ran a
                // different experiment would put numbers in the record that no reader could
                // tell apart from the ones they were meant to be.
                std::cerr << "unknown scenario '" << options.scenario
                          << "': expected steady, step, pulse or oscillation\n";
                return 2;
            }
        } else if (argument == "--mult") {
            options.mult = std::stod(next_value("--mult"));
        } else if (argument == "--hi") {
            options.hi = std::stod(next_value("--hi"));
        } else if (argument == "--lo") {
            options.lo = std::stod(next_value("--lo"));
        } else if (argument == "--period") {
            options.period = std::stoll(next_value("--period"));
        } else if (argument == "--help") {
            std::cout
                << "usage: amarian-retarget-sim [--tau s] [--scenario steady|step|pulse|oscillation]\n"
                   "       [--blocks N] [--window N] [--seed N] [--mult x] [--hi x] [--lo x]\n"
                   "       [--period blocks]\n";
            return 0;
        } else {
            std::cerr << "unknown argument " << argument << '\n';
            return 2;
        }
    }
    return Run(options);
}

}  // namespace

int main(int argc, char** argv) {
    return ParseAndRun(argc, argv);
}
