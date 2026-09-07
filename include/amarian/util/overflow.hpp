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
#include <type_traits>
#include <utility>

namespace amarian {

template<std::integral T>
[[nodiscard]] constexpr std::optional<T> CheckedAdd(T a, T b) noexcept {
    T out{};
#if defined(_MSC_VER) && !defined(__clang__)
    if constexpr (std::is_signed_v<T>) {
        if ((b > 0 && a > std::numeric_limits<T>::max() - b) ||
            (b < 0 && a < std::numeric_limits<T>::min() - b)) {
            return std::nullopt;
        }
    } else if (a > std::numeric_limits<T>::max() - b) {
        return std::nullopt;
    }
    out = static_cast<T>(a + b);
    return out;
#else
    if (__builtin_add_overflow(a, b, &out)) {
        return std::nullopt;
    }
    return out;
#endif
}

template<std::integral T>
[[nodiscard]] constexpr std::optional<T> CheckedSub(T a, T b) noexcept {
    T out{};
#if defined(_MSC_VER) && !defined(__clang__)
    if constexpr (std::is_signed_v<T>) {
        if ((b > 0 && a < std::numeric_limits<T>::min() + b) ||
            (b < 0 && a > std::numeric_limits<T>::max() + b)) {
            return std::nullopt;
        }
    } else if (a < b) {
        return std::nullopt;
    }
    out = static_cast<T>(a - b);
    return out;
#else
    if (__builtin_sub_overflow(a, b, &out)) {
        return std::nullopt;
    }
    return out;
#endif
}

template<std::integral T>
[[nodiscard]] constexpr std::optional<T> CheckedMul(T a, T b) noexcept {
    T out{};
#if defined(_MSC_VER) && !defined(__clang__)
    if constexpr (std::is_signed_v<T>) {
        if (a != 0 && b != 0) {
            constexpr T min = std::numeric_limits<T>::min();
            constexpr T max = std::numeric_limits<T>::max();
            if ((a == -1 && b == min) || (b == -1 && a == min)) return std::nullopt;
            if (a > 0) {
                if (b > 0 ? a > max / b : b < min / a) return std::nullopt;
            } else if (b > 0 ? a < min / b : a < max / b) {
                return std::nullopt;
            }
        }
    } else if (a != 0 && b > std::numeric_limits<T>::max() / a) {
        return std::nullopt;
    }
    out = static_cast<T>(a * b);
    return out;
#else
    if (__builtin_mul_overflow(a, b, &out)) {
        return std::nullopt;
    }
    return out;
#endif
}

/// Adds into an accumulator, returning false (and leaving `acc` untouched) on overflow.
template<std::integral T>
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
template<std::integral To, std::integral From>
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
