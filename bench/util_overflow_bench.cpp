/// \file
/// Checked-arithmetic overhead.
///
/// Every attacker-influenced quantity — output amounts, fees, sizes, weights —
/// goes through amarian::Checked*, so the per-call cost is multiplied by the number
/// of outputs in a block. These benchmarks exist so that keeping the checks
/// unconditionally rests on a measured overhead rather than an assumption.
///
/// Four variants, because a naive raw-vs-checked ratio measures the wrong thing:
///
///   Raw        a plain reduction. Auto-vectorises, so it is a floor, not a
///              fair baseline for anything that has to inspect each result.
///   RawGuarded the same loop shape as the checked version — scalar, with a
///              never-taken early exit. This is the honest baseline.
///   Builtin    __builtin_add_overflow directly, no std::optional. Isolates the
///              cost of the overflow check itself.
///   Checked    TryAccumulate, i.e. what consensus code actually calls.
///
/// Measured 2026-09-04, 12 × 2.5 GHz x86-64, Release + hardening, 20 000 amounts,
/// nanoseconds per operation (`--benchmark_min_time=0.2s`):
///
///                     GCC 15.2   Clang 21.1
///   SumRaw               0.20       0.14
///   SumRawGuarded        0.45       0.40
///   SumBuiltin           0.41       0.36
///   SumChecked           8.67       0.44
///   MulChecked           0.44       0.69
///
/// Two things fall out. The overflow check itself is free: SumBuiltin matches
/// SumRawGuarded on both compilers. And GCC 15.2 pays a 21× penalty on
/// SumChecked that Clang does not — 8.67 ns against 0.41 ns for the same check.
///
/// It is not std::optional as such: MulChecked also returns std::optional and
/// costs 0.44 ns under GCC. The difference is that SumChecked carries the
/// unwrapped value into the next iteration. The assembly shows GCC spilling the
/// optional to the stack and reloading it through an SSE register every
/// iteration, which puts store-to-load forwarding on the loop-carried dependency;
/// Clang emits `addq` + `jo` and nothing else. Toggling _GLIBCXX_ASSERTIONS,
/// _FORTIFY_SOURCE, -fno-strict-aliasing and -fwrapv individually changes
/// nothing, so it is a code-generation difference and not a hardening cost.
///
/// The API is not changing on that basis. At 8.67 ns per addition, a block with
/// 20 000 outputs spends ~0.2 ms on amount arithmetic against a 300 s block
/// interval, and post-quantum signature verification will cost orders of magnitude
/// more per transaction. std::optional makes it impossible to read a result that
/// overflowed, which is worth far more in consensus code than 0.2 ms per block.
/// Recorded here so that if Phase 12 profiling ever does show amount arithmetic
/// near the top, the cause and the fix are already known and local.
///
/// The raw-addition baselines are well defined rather than undefined behaviour:
/// the hardening flags include -fwrapv, so signed overflow wraps. That is exactly
/// the silent-inflation behaviour the checked helpers exist to prevent.

#include <amarian/util/overflow.hpp>

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

/// Amounts small enough that the running total never overflows, so every variant
/// does the same amount of real work and none exits early.
std::vector<int64_t> MakeAmounts(size_t count) {
    std::vector<int64_t> amounts(count);
    uint64_t rng = 0xD1B54A32D192ED03ULL;
    for (int64_t& amount : amounts) {
        rng ^= rng << 13U;
        rng ^= rng >> 7U;
        rng ^= rng << 17U;
        amount = static_cast<int64_t>(rng % 1'000'000'000'000ULL);
    }
    return amounts;
}

void BM_SumRaw(benchmark::State& state) {
    const std::vector<int64_t> amounts = MakeAmounts(static_cast<size_t>(state.range(0)));
    for ([[maybe_unused]] auto iteration : state) {
        int64_t total = 0;
        for (const int64_t amount : amounts) {
            total += amount;
        }
        benchmark::DoNotOptimize(total);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SumRaw)->Arg(1000)->Arg(20000);

/// Scalar, with an exit condition that never fires. Same shape as the checked
/// loop, so the difference against it is the check and nothing else.
void BM_SumRawGuarded(benchmark::State& state) {
    const std::vector<int64_t> amounts = MakeAmounts(static_cast<size_t>(state.range(0)));
    for ([[maybe_unused]] auto iteration : state) {
        int64_t total = 0;
        bool ok = true;
        for (const int64_t amount : amounts) {
            total += amount;
            ok = total >= 0;
            if (!ok) {
                break;
            }
        }
        benchmark::DoNotOptimize(total);
        benchmark::DoNotOptimize(ok);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SumRawGuarded)->Arg(1000)->Arg(20000);

/// The compiler builtin with no wrapper, to separate the check from the optional.
void BM_SumBuiltin(benchmark::State& state) {
    const std::vector<int64_t> amounts = MakeAmounts(static_cast<size_t>(state.range(0)));
    for ([[maybe_unused]] auto iteration : state) {
        int64_t total = 0;
        bool ok = true;
        for (const int64_t amount : amounts) {
            if (__builtin_add_overflow(total, amount, &total)) {
                ok = false;
                break;
            }
        }
        benchmark::DoNotOptimize(total);
        benchmark::DoNotOptimize(ok);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SumBuiltin)->Arg(1000)->Arg(20000);

/// What consensus code calls.
void BM_SumChecked(benchmark::State& state) {
    const std::vector<int64_t> amounts = MakeAmounts(static_cast<size_t>(state.range(0)));
    for ([[maybe_unused]] auto iteration : state) {
        int64_t total = 0;
        bool ok = true;
        for (const int64_t amount : amounts) {
            ok = amarian::TryAccumulate(total, amount);
            if (!ok) {
                break;
            }
        }
        benchmark::DoNotOptimize(total);
        benchmark::DoNotOptimize(ok);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_SumChecked)->Arg(1000)->Arg(20000);

/// CheckedMul is the multiply path — fee rates, weight and subsidy arithmetic.
/// Values are bounded so the product never overflows.
void BM_MulChecked(benchmark::State& state) {
    const std::vector<int64_t> amounts = MakeAmounts(static_cast<size_t>(state.range(0)));
    for ([[maybe_unused]] auto iteration : state) {
        int64_t accepted = 0;
        for (const int64_t amount : amounts) {
            const std::optional<int64_t> scaled = amarian::CheckedMul<int64_t>(amount, 1000);
            accepted += scaled.has_value() ? 1 : 0;
        }
        benchmark::DoNotOptimize(accepted);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_MulChecked)->Arg(20000);

}  // namespace
