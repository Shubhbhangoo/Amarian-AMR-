#pragma once

/// \file
/// The monetary bound shared by every value that carries facets.

#include <cstdint>

namespace amarian {

/// Largest valid amount in facets: 83 999 999 932 170 000, per ECONOMICS.md.
///
/// The value occupies 57 bits, leaving 109.8x headroom below `INT64_MAX` for the
/// intermediates of block validation (sums of many outputs, amount times size).
/// Amounts are signed `int64_t` and every value decoded from the wire is checked
/// into `[0, MAX_MONEY]` at deserialisation (DECISIONS #20), so a bad amount is
/// representable — and therefore detectable — rather than optimised away.
inline constexpr int64_t MAX_MONEY = 83'999'999'932'170'000LL;

/// Whether a value is a valid amount of money: in `[0, MAX_MONEY]`.
///
/// One definition, used by deserialisation, by the issuance schedule, and by every
/// validation rule that sums or compares amounts. A predicate written out by hand
/// at each site is a predicate that can differ at one of them.
[[nodiscard]] constexpr bool IsValidAmount(int64_t amount) noexcept {
    return amount >= 0 && amount <= MAX_MONEY;
}

}  // namespace amarian
