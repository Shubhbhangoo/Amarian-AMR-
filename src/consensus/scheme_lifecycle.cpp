/// \file
/// Scheme lifecycle management implementation.

#include <amarian/consensus/scheme_lifecycle.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace amarian::consensus {

SchemeLifecycleRegistry::SchemeLifecycleRegistry() {
    for (const auto& spec : crypto::KnownSchemes()) {
        SchemeLifecycle lc;
        lc.scheme_id = spec.id;
        lc.activation_height = 0;
        lc.deprecation_height = 0xFFFFFFFFU;
        lc.retirement_height = 0xFFFFFFFFU;
        entries_.push_back(lc);
    }
}

SchemeState SchemeLifecycleRegistry::GetState(uint16_t scheme_id, uint32_t height) const noexcept {
    if (scheme_id == 0) return SchemeState::Retired;
    for (const auto& entry : entries_) {
        if (entry.scheme_id == scheme_id) {
            if (height < entry.activation_height) return SchemeState::Pending;
            if (height >= entry.retirement_height) return SchemeState::Retired;
            if (height >= entry.deprecation_height) return SchemeState::Deprecated;
            return SchemeState::Active;
        }
    }
    return SchemeState::Pending;
}

bool SchemeLifecycleRegistry::MayCreateOutput(uint16_t scheme_id, uint32_t height) const noexcept {
    const SchemeState state = GetState(scheme_id, height);
    return state == SchemeState::Active;
}

bool SchemeLifecycleRegistry::IsSpendable(uint16_t scheme_id, uint32_t height) const noexcept {
    (void)height;
    if (scheme_id == 0) return false;
    return true;
}

void SchemeLifecycleRegistry::Register(const SchemeLifecycle& lifecycle) {
    for (auto& entry : entries_) {
        if (entry.scheme_id == lifecycle.scheme_id) {
            entry = lifecycle;
            return;
        }
    }
    entries_.push_back(lifecycle);
}

std::optional<SchemeLifecycle> SchemeLifecycleRegistry::GetLifecycle(uint16_t scheme_id) const noexcept {
    if (scheme_id == 0) return std::nullopt;
    for (const auto& entry : entries_) {
        if (entry.scheme_id == scheme_id) return entry;
    }
    return std::nullopt;
}

}  // namespace amarian::consensus