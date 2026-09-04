#pragma once

/// \file
/// Checked integer arithmetic.
///
/// Every consensus quantity that can be attacker-influenced (amounts, sizes, weights,
/// counters) goes through these helpers. Silent wraparound in a supply or fee
/// calculation is an inflation bug, so overflow is always an explicit failure rather
/// than a defined-but-wrong value.

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace amarian {

template <std::integral T>
[[nodiscard]] constexpr std::optional<T> CheckedAdd(T a, T b) noexcept {
    T out{};
    if (__builtin_add_overflow(a, b, &out)) {
        return std::nullopt;
    }
    return out;
}

template <std::integral T>
[[nodiscard]] constexpr std::optional<T> CheckedSub(T a, T b) noexcept {
    T out{};
    if (__builtin_sub_overflow(a, b, &out)) {
        return std::nullopt;
    }
    return out;
}

template <std::integral T>
[[nodiscard]] constexpr std::optional<T> CheckedMul(T a, T b) noexcept {
    T out{};
    if (__builtin_mul_overflow(a, b, &out)) {
        return std::nullopt;
    }
    return out;
}

/// Adds into an accumulator, returning false (and leaving `acc` untouched) on overflow.
template <std::integral T>
[[nodiscard]] constexpr bool TryAccumulate(T& acc, T value) noexcept {
    const auto sum = CheckedAdd(acc, value);
    if (!sum.has_value()) {
        return false;
    }
    acc = *sum;
    return true;
}

/// Narrowing cast that reports whether the value survives the conversion.
///
/// Uses std::cmp_* rather than a wider intermediate type so it stays strictly
/// conforming (no __int128) and correct for every signed/unsigned combination.
template <std::integral To, std::integral From>
[[nodiscard]] constexpr std::optional<To> TryNarrow(From value) noexcept {
    if (std::cmp_less(value, std::numeric_limits<To>::min())) {
        return std::nullopt;
    }
    if (std::cmp_greater(value, std::numeric_limits<To>::max())) {
        return std::nullopt;
    }
    return static_cast<To>(value);
}

}  // namespace amarian
