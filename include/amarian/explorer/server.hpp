#pragma once

/// ile
/// Loopback HTTP listener for the read-only explorer API.

#include <amarian/chain/chain_state.hpp>
#include <amarian/explorer/http_api.hpp>
#include <amarian/mempool.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace amarian::explorer {

struct ServerConfig {
    uint16_t port = 0;
};

/// A loopback-only, unauthenticated GET listener for explorer data.
class Server {
public:
    [[nodiscard]] static std::expected<Server, std::string>
    Bind(chain::ChainState& state, const chain::BlockIndex& index, mempool::Mempool* pool,
         const ServerConfig& config);

    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server();

    [[nodiscard]] uint16_t Port() const noexcept;
    void Serve();
    void Stop() noexcept;

private:
    struct Impl;
    explicit Server(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace amarian::explorer
