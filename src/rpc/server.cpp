/// \file
/// The listener. See `rpc/server.hpp` for why there are no decisions in this file.

#include <amarian/rpc/server.hpp>

#include <amarian/rpc/http.hpp>
#include <amarian/util/logging.hpp>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/address_v6.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/v6_only.hpp>
#include <asio/signal_set.hpp>
#include <asio/socket_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace amarian::rpc {
namespace {

/// The node's wall clock, in seconds since the Unix epoch.
///
/// Read here and passed down as a value, which is the same rule the daemon follows and for the
/// same reason: nothing in consensus, the chain layer or the assembler calls a clock, so a
/// node's verdict on a block is reproducible from a transcript rather than dependent on when it
/// was asked. This is the only clock read on the serving path.
[[nodiscard]] int64_t UnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// Everything a connection needs from the server, with nothing socket-shaped in it.
///
/// A separate struct rather than a reference to the server because a connection has no business
/// reaching the acceptor, the signal set or the event loop: it reads bytes, hands them to the
/// pure functions in `http.hpp`, and writes what comes back.
struct ServerState {
    Node* node = nullptr;
    AuthPolicy policy;
    std::chrono::seconds timeout{10};
    size_t max_connections = 16;
    size_t open = 0;
};

/// One connection, from accept to close.
///
/// Kept alive by whatever is pending on it: every asynchronous operation captures a
/// `shared_ptr` to the connection, so it exists exactly as long as an operation refers to it
/// and is destroyed when none does. That is the standard Asio ownership idiom, and it is why
/// there is no lifetime bookkeeping in this class beyond the open-connection count.
class Connection final : public std::enable_shared_from_this<Connection> {
public:
    Connection(ServerState& state, asio::ip::tcp::socket socket)
        : state_(state), socket_(std::move(socket)), deadline_(socket_.get_executor()) {
        ++state_.open;
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;

    ~Connection() { --state_.open; }

    void Start() {
        // Armed before the first read, and measured from accept rather than from the last byte
        // received: a client that sends one byte every nine seconds would otherwise hold this
        // single-threaded server open indefinitely while never being idle enough to notice.
        deadline_.expires_after(state_.timeout);
        deadline_.async_wait([self = shared_from_this()](const std::error_code& ec) {
            if (ec == asio::error::operation_aborted) {
                return;
            }
            AMARIAN_DEBUG(log::Category::Rpc,
                          "rpc: dropping a connection that did not finish its request in {}s",
                          self->state_.timeout.count());
            self->Close();
        });
        Read();
    }

private:
    void Read() {
        socket_.async_read_some(
            asio::buffer(chunk_),
            [self = shared_from_this()](const std::error_code& ec, size_t bytes) {
                if (ec) {
                    // End of file included: a client that closed before finishing its request
                    // has asked nothing, and there is nothing to answer it with.
                    self->Close();
                    return;
                }
                self->buffer_.append(self->chunk_.data(), bytes);
                self->Advance();
            });
    }

    /// Asks the framer what has arrived so far, and acts on its answer.
    ///
    /// Called after every read rather than reading a fixed amount first, because `FrameRequest`
    /// is restartable: there is no parser state between reads, so a request split across an
    /// arbitrary number of packets is framed identically to one that arrived whole. The buffer
    /// is bounded by the framer too — a header section past `MAX_HEADER_BYTES` and a body past
    /// `MAX_REQUEST_BYTES` are both refusals, so this loop cannot be made to grow the buffer.
    void Advance() {
        const Framed framed = FrameRequest(buffer_);
        switch (framed.state) {
            case Framing::NeedMore:
                Read();
                return;
            case Framing::Malformed:
            case Framing::TooLarge:
                Answer(RenderRefusal(framed.status));
                return;
            case Framing::Complete:
                break;
        }

        if (const std::optional<HttpStatus> refusal = Screen(framed.request, state_.policy);
            refusal.has_value()) {
            // Logged as a status and nothing else. A refused request is the one most likely to
            // have been composed by something hostile, and a log file is read later by things
            // that trust it.
            AMARIAN_INFO(log::Category::Rpc, "rpc: refused a request with {} {}",
                         static_cast<int>(*refusal), ReasonPhrase(*refusal));
            Answer(RenderRefusal(*refusal));
            return;
        }

        const RpcAnswer answer = AnswerJsonRpc(*state_.node, framed.request.body, UnixSeconds());
        Answer(RenderResponse(answer.status, answer.body));
    }

    void Answer(std::string response) {
        // Held in a member for the duration of the write: `asio::buffer` does not copy, so the
        // bytes must outlive the call and a local would not.
        response_ = std::move(response);
        asio::async_write(socket_, asio::buffer(response_),
                          [self = shared_from_this()](const std::error_code&, size_t) {
                              // Written or not, this connection is finished: one request, one
                              // answer, close. A write that failed has no recovery worth
                              // attempting — the client is gone.
                              self->Close();
                          });
    }

    /// Closes the socket and disarms the deadline. Idempotent, and called on every path.
    ///
    /// Closing is also how a pending read is cancelled, which is what makes the deadline
    /// effective rather than advisory.
    void Close() {
        deadline_.cancel();
        std::error_code ignored;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    ServerState& state_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer deadline_;
    std::array<char, 8192> chunk_{};
    std::string buffer_;
    std::string response_;
};

/// Opens, binds and listens on one loopback endpoint, reporting the port it got.
[[nodiscard]] std::expected<uint16_t, std::string> BindLoopback(asio::ip::tcp::acceptor& acceptor,
                                                                const asio::ip::address& address,
                                                                uint16_t port) {
    const asio::ip::tcp::endpoint endpoint(address, port);
    const std::string where = endpoint.address().to_string() + ":" + std::to_string(port);
    std::error_code ec;
    acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        return std::unexpected("cannot open a socket for " + where + ": " + ec.message());
    }
    // Lets a restarted node rebind a port its predecessor left in TIME_WAIT. It does not let a
    // second node take a port from a running one: a listening socket still holds it, and the
    // bind below still fails with "address already in use".
    acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        return std::unexpected("cannot set reuse-address on " + where + ": " + ec.message());
    }
    if (endpoint.address().is_v6()) {
        // One address per socket. A v6 socket that also answered v4-mapped traffic would make
        // "which address did this arrive on" a question with two answers, and the IPv4 loopback
        // has its own acceptor already.
        acceptor.set_option(asio::ip::v6_only(true), ec);
        if (ec) {
            return std::unexpected("cannot restrict " + where + " to IPv6: " + ec.message());
        }
    }
    acceptor.bind(endpoint, ec);
    if (ec) {
        return std::unexpected("cannot bind " + where + ": " + ec.message());
    }
    acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        return std::unexpected("cannot listen on " + where + ": " + ec.message());
    }
    const asio::ip::tcp::endpoint bound = acceptor.local_endpoint(ec);
    if (ec) {
        return std::unexpected("cannot read back the port bound for " + where + ": " +
                               ec.message());
    }
    return bound.port();
}

}  // namespace

struct Server::Impl {
    asio::io_context io{1};
    asio::signal_set signals{io};
    std::vector<asio::ip::tcp::acceptor> acceptors;
    ServerState state;

    /// Accepts one connection and then itself again, forever.
    ///
    /// The chain is re-armed on every outcome including failure, because an accept can fail for
    /// reasons that have nothing to do with the next connection — a descriptor limit reached, a
    /// client that connected and vanished — and a listener that stopped at the first of those
    /// would be a node that any client could switch off.
    void Accept(asio::ip::tcp::acceptor& acceptor) {
        acceptor.async_accept(
            [this, &acceptor](const std::error_code& ec, asio::ip::tcp::socket socket) {
                if (ec == asio::error::operation_aborted) {
                    return;
                }
                if (ec) {
                    AMARIAN_WARN(log::Category::Rpc, "rpc: accept failed: {}", ec.message());
                } else if (state.open >= state.max_connections) {
                    // Accepted and dropped unread, which keeps the acceptor running and the
                    // descriptor count bounded. Refusing to accept instead would fill the
                    // kernel's backlog and make legitimate clients wait on a queue they cannot
                    // see the length of.
                    AMARIAN_WARN(log::Category::Rpc,
                                 "rpc: {} connections already open, dropping this one",
                                 state.open);
                    std::error_code ignored;
                    socket.close(ignored);
                } else {
                    std::make_shared<Connection>(state, std::move(socket))->Start();
                }
                Accept(acceptor);
            });
    }
};

Server::Server(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;
Server::~Server() = default;

std::expected<Server, std::string> Server::Bind(Node& node, const ServerConfig& config) {
    if (config.expected_authorization.empty()) {
        return std::unexpected(
            "refusing to serve RPC with no credential to check requests against");
    }
    if (node.state == nullptr || node.params == nullptr) {
        return std::unexpected("refusing to serve RPC on a node with no chain behind it");
    }
    if (config.request_timeout <= std::chrono::seconds::zero()) {
        return std::unexpected("an RPC request timeout must be positive");
    }
    if (config.max_connections == 0) {
        return std::unexpected("an RPC connection limit of zero would answer nobody");
    }

    auto impl = std::make_unique<Impl>();
    impl->state.node = &node;
    impl->state.policy.expected_authorization = config.expected_authorization;
    impl->state.timeout = config.request_timeout;
    impl->state.max_connections = config.max_connections;

    // Reserved so that the acceptors never move: the accept chain holds a reference to the one
    // it is running on, and a reallocation would leave it referring to a destroyed socket.
    impl->acceptors.reserve(2);

    // IPv4 first, and its port is then what IPv6 is asked for. With `config.port` zero the
    // kernel chooses, and the two acceptors must agree on the answer — one interface on two
    // ports would make `AuthPolicy::port` right for one of them and wrong for the other, and
    // the `Host` check would then refuse requests that arrived on the socket it did not know
    // about.
    std::string v4_failure;
    std::optional<uint16_t> bound;
    impl->acceptors.emplace_back(impl->io);
    if (const std::expected<uint16_t, std::string> got =
            BindLoopback(impl->acceptors.back(), asio::ip::address_v4::loopback(), config.port);
        got.has_value()) {
        bound = *got;
    } else {
        v4_failure = got.error();
        impl->acceptors.pop_back();
    }

    impl->acceptors.emplace_back(impl->io);
    if (const std::expected<uint16_t, std::string> got =
            BindLoopback(impl->acceptors.back(), asio::ip::address_v6::loopback(),
                         bound.value_or(config.port));
        got.has_value()) {
        // Only when IPv4 did not already answer: if both bound, they bound the same port.
        bound = bound.value_or(*got);
    } else {
        // Not fatal on its own. A machine without IPv6 is a machine a node still runs on, and
        // the note is at debug level because for most operators it is not news.
        AMARIAN_DEBUG(log::Category::Rpc, "rpc: no IPv6 loopback ({})", got.error());
        impl->acceptors.pop_back();
    }

    if (!bound.has_value()) {
        // Both failed, and the IPv4 message is the one to report: it is the address every
        // client reaches for, so its failure is the one an operator has to fix.
        return std::unexpected(v4_failure);
    }
    impl->state.policy.port = *bound;
    return Server(std::move(impl));
}

uint16_t Server::Port() const noexcept {
    return impl_ == nullptr ? 0 : impl_->state.policy.port;
}

void Server::Serve() {
    // The signal set is armed here rather than in `Bind` because this is the call that does not
    // return on its own: a process that bound a port and then went on to do something else has
    // no reason to have had its interrupt handling replaced. When Phase 4 adds a network loop
    // the two will share one `io_context`, and this moves up to whoever owns the process.
    std::error_code ec;
    impl_->signals.add(SIGINT, ec);
    if (!ec) {
        impl_->signals.add(SIGTERM, ec);
    }
    if (ec) {
        AMARIAN_WARN(log::Category::General, "rpc: cannot watch for an interrupt: {}",
                     ec.message());
    }
    impl_->signals.async_wait([this](const std::error_code& wait_ec, int signal) {
        if (wait_ec) {
            return;
        }
        AMARIAN_INFO(log::Category::General, "rpc: signal {}, stopping", signal);
        Stop();
    });

    // One line per acceptor, reporting the address each actually bound rather than the one it
    // was asked for: an operator whose IPv6 loopback is missing should be able to see that from
    // the log rather than infer it from a client failing to connect.
    //
    // `General` rather than `Rpc` for this and for every other line about the server's own
    // lifetime, because only `General` is on by default: an operator who asked for an interface
    // must be told which port it is on without also having to ask for a log category. `Rpc` is
    // for what individual requests do, which is chatter until something is being diagnosed.
    for (asio::ip::tcp::acceptor& acceptor : impl_->acceptors) {
        std::error_code local_ec;
        const asio::ip::tcp::endpoint local = acceptor.local_endpoint(local_ec);
        if (!local_ec) {
            AMARIAN_INFO(log::Category::General, "rpc: listening on {}:{}",
                         local.address().to_string(), local.port());
        }
        impl_->Accept(acceptor);
    }

    // Restarted rather than left to unwind. An exception escaping a handler is a bug, and the
    // handler it escaped from belongs to one client's connection — losing that connection is
    // the right cost, and taking the node's whole interface down with it would let one client
    // stop the miner from getting templates.
    for (;;) {
        try {
            impl_->io.run();
            return;
        } catch (const std::exception& failure) {
            AMARIAN_ERROR(log::Category::General, "rpc: a request handler threw: {}",
                          failure.what());
        }
    }
}

void Server::Stop() noexcept {
    if (impl_ != nullptr) {
        impl_->io.stop();
    }
}

}  // namespace amarian::rpc
