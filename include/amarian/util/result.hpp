#pragma once

/// \file
/// Generic error plumbing for non-consensus code (config, RPC, storage, hex, CLI).
///
/// Consensus rule failures deliberately do NOT use this type: they use
/// `amarian::consensus::ValidationError`, which is allocation-free and carries a
/// compile-checked reason enum so tests can assert on the exact rule that fired.

#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace amarian {

/// A human-facing error. `context` is a short stable identifier; `detail` is optional
/// free text. Only used off the block/transaction validation hot path.
class Error {
public:
    explicit Error(std::string_view context) : context_(context) {}

    Error(std::string_view context, std::string detail)
        : context_(context), detail_(std::move(detail)) {}

    [[nodiscard]] std::string_view Context() const noexcept { return context_; }

    [[nodiscard]] const std::string& Detail() const noexcept { return detail_; }

    [[nodiscard]] std::string Message() const {
        if (detail_.empty()) {
            return std::string(context_);
        }
        return std::string(context_) + ": " + detail_;
    }

private:
    std::string_view context_;
    std::string detail_;
};

template<typename T>
using Result = std::expected<T, Error>;

using Status = std::expected<void, Error>;

[[nodiscard]] inline std::unexpected<Error> Fail(std::string_view context) {
    return std::unexpected(Error(context));
}

[[nodiscard]] inline std::unexpected<Error> Fail(std::string_view context, std::string detail) {
    return std::unexpected(Error(context, std::move(detail)));
}

[[nodiscard]] inline Status Ok() {
    return Status{};
}

/// Propagates a failure from `expr` out of the enclosing function.
/// Usage: `AMARIAN_TRY(DoThing());` for Status, `auto v = AMARIAN_TRY_VALUE(GetThing());`
#define AMARIAN_TRY(expr)                                                   \
    do {                                                                    \
        auto amarian_try_result_ = (expr);                                  \
        if (!amarian_try_result_.has_value()) {                             \
            return std::unexpected(std::move(amarian_try_result_).error()); \
        }                                                                   \
    } while (false)

}  // namespace amarian
