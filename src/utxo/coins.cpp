#include <amarian/utxo/coins.hpp>

#include <utility>

namespace amarian::utxo {

std::optional<Coin> CoinsCache::GetCoin(const OutPoint& outpoint) const {
    if (const auto entry = entries_.find(outpoint); entry != entries_.end()) {
        return entry->second;
    }
    return base_->GetCoin(outpoint);
}

bool CoinsCache::HaveCoin(const OutPoint& outpoint) const {
    if (const auto entry = entries_.find(outpoint); entry != entries_.end()) {
        return entry->second.has_value();
    }
    return base_->HaveCoin(outpoint);
}

void CoinsCache::Write(const OutPoint& outpoint, const std::optional<Coin>& coin) {
    if (coin.has_value()) {
        entries_[outpoint] = coin;
    } else {
        MarkSpent(outpoint);
    }
}

void CoinsCache::MarkSpent(const OutPoint& outpoint) {
    // A tombstone is only needed to hide something. If the base does not have the coin,
    // erasing the entry says exactly as much as a tombstone would and costs nothing to
    // keep — which is what stops the root cache from growing a permanent record of every
    // coin the chain has ever spent.
    if (base_->HaveCoin(outpoint)) {
        entries_[outpoint] = std::nullopt;
    } else {
        entries_.erase(outpoint);
    }
}

bool CoinsCache::AddCoin(const OutPoint& outpoint, const Coin& coin) {
    if (HaveCoin(outpoint)) {
        return false;
    }
    entries_[outpoint] = coin;
    return true;
}

std::optional<Coin> CoinsCache::SpendCoin(const OutPoint& outpoint) {
    std::optional<Coin> coin = GetCoin(outpoint);
    if (coin.has_value()) {
        MarkSpent(outpoint);
    }
    return coin;
}

void CoinsCache::Flush(CoinsSink& sink) {
    for (const auto& [outpoint, coin] : entries_) {
        sink.Write(outpoint, coin);
    }
    entries_.clear();
}

}  // namespace amarian::utxo
