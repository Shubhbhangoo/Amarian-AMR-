#pragma once

/// \file
/// The socket half of the node's JSON-RPC interface: accept, read, answer, close.
///
/// This file deliberately contains no decisions. Every refusal is `FrameRequest`'s or
/// `Screen`'s, every answer is `AnswerJsonRpc`'s, and every byte written is
/// `RenderResponse`'s — all of them in `rpc/http.hpp`, all of them pure, all of them
/// testable without a port. If a question about what this interface allows can be answered
/// by reading this file, then something has been implemented in the wrong place.
///
/// ## One thread, one loop
///
/// There is exactly one `io_context` and exactly one thread running it, and the request
/// handlers reach straight into `ChainState` and the mempool without a lock. That is safe
/// only because of the "exactly one" — it is not a property of the handlers, and it is
/// written here because it is the kind of invariant a later `std::thread` would break
/// silently. Phase 4's network loop must share this `io_context` rather than start a second
/// one; when it does, the chain still has a single thread reaching into it.
///
/// A single thread is also why the per-connection deadline is not a nicety. A client that
/// connects and says nothing occupies a read that never completes, and one such client would
/// be enough to stop the node answering anyone — including the miner waiting for a template.
/// The timer is what makes an idle connection somebody else's problem.
///
/// ## Loopback
///
/// The acceptor binds `127.0.0.1` rather than binding everything and checking who connected.
/// A bind is enforcement by the kernel; a check is code that can be reordered, inverted, or
/// skipped on one path. It is also why `Screen`'s `Host` check exists in addition: binding
/// keeps remote *sockets* out, and the `Host` header is what keeps a remote *page* from
/// borrowing the operator's browser to reach a socket that is already local.
///
/// ## One request per connection
///
/// Read until a whole request has arrived, answer it, close. No keep-alive and no pipelining,
/// for the reason `http.hpp` gives: the state machines they need are where a transport's
/// exploits live, and a local caller asking for one template gains nothing from either.

#include <amarian/rpc/dispatch.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace amarian::rpc {

/// What the server needs before it binds.
struct ServerConfig {
    /// The loopback port to bind. Zero asks the operating system for a free one; `Port()`
    /// reports what it got, which is what makes a test able to run one of these without
    /// choosing a number that some other program on the machine might already hold.
    uint16_t port = 0;

    /// The credential a request must carry, as the full `Authorization` header value —
    /// `Cookie::Authorization()` produces it. Empty is refused by `Bind`: a server with no
    /// expected credential would authenticate nobody, and `ConstantTimeEquals` against an
    /// empty expectation is a comparison every request passes.
    std::string expected_authorization;

    /// How long one connection may take to deliver a whole request, measured from accept.
    ///
    /// Ten seconds is enormous for a local client sending at most eight mebibytes over
    /// loopback and small enough that a stalled connection is gone before anyone notices.
    std::chrono::seconds request_timeout{10};

    /// How many connections may be open at once. Beyond this, a connection is accepted and
    /// closed unread, which is deliberately not a refusal message: the point is to keep the
    /// acceptor running and the file descriptor count bounded, and a client that wanted an
    /// explanation should not have been the sixteenth simultaneous one.
    size_t max_connections = 16;
};

/// A bound, not-yet-serving JSON-RPC listener.
///
/// Asio appears nowhere in this header. That is the same boundary `storage::ChainDb` draws
/// around RocksDB and for the same reason: a target that links this one gets Amarian's
/// interface, not a networking library's headers, so there is no second place a socket can
/// be opened from.
class Server {
public:
    /// Binds a loopback socket, or explains why it could not.
    ///
    /// `node` must outlive the server, and its `state`, `pool` and `params` must all be set:
    /// this is a reference to the caller's node, not a copy of it, because the whole purpose
    /// of the interface is to act on the chain the daemon is running.
    ///
    /// Binding here rather than in `Serve()` is what lets a caller learn the port, and learn
    /// that the port was taken, before it commits to an event loop it cannot return from.
    [[nodiscard]] static std::expected<Server, std::string> Bind(Node& node,
                                                                 const ServerConfig& config);

    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    ~Server();

    /// The port actually bound, which is `config.port` unless that was zero.
    [[nodiscard]] uint16_t Port() const noexcept;

    /// Accepts and answers requests until `Stop()`, `SIGINT` or `SIGTERM`.
    ///
    /// A failure on one connection is logged and the connection is dropped; it is never
    /// fatal. A server that exits because one client sent nonsense is a server that any
    /// client can turn off, and this one can create blocks.
    void Serve();

    /// Asks `Serve()` to return. Safe to call from a signal handler or another thread, and
    /// safe to call on a server that is not running.
    void Stop() noexcept;

private:
    struct Impl;
    explicit Server(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace amarian::rpc
