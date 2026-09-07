/// ile
/// The explorer HTTP listener. Routing itself remains in http_api.cpp.

#include <amarian/explorer/server.hpp>

#include <amarian/util/logging.hpp>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/socket_base.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace amarian::explorer {
namespace {

constexpr size_t MAX_REQUEST = 4096;
constexpr std::chrono::seconds REQUEST_TIMEOUT{10};

std::string Response(int status, std::string_view reason, std::string_view body,
                     std::string_view type = "application/json") {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) +
                      "\r\nContent-Type: " + std::string(type) +
                      "\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    out += body;
    return out;
}

std::string ErrorResponse(int status, std::string_view reason, std::string_view detail) {
    const nlohmann::json body = {{"error", status}, {"detail", std::string(detail)}};
    return Response(status, reason, body.dump());
}

std::string_view Header(std::string_view request, std::string_view name) {
    size_t line_start = request.find("\r\n");
    if (line_start == std::string_view::npos) return {};
    line_start += 2;
    while (line_start < request.size()) {
        const size_t line_end = request.find("\r\n", line_start);
        if (line_end == std::string_view::npos || line_end == line_start) break;
        const std::string_view line = request.substr(line_start, line_end - line_start);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos && line.substr(0, colon) == name) {
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.remove_prefix(1);
            }
            return value;
        }
        line_start = line_end + 2;
    }
    return {};
}

bool LoopbackHost(std::string_view host, uint16_t expected_port) {
    const size_t colon = host.find(':');
    const std::string_view name = colon == std::string_view::npos ? host : host.substr(0, colon);
    if (name != "127.0.0.1" && name != "localhost") return false;
    if (colon == std::string_view::npos) return true;
    const std::string_view port = host.substr(colon + 1);
    if (port.empty() || !std::all_of(port.begin(), port.end(),
                                     [](char c) { return c >= '0' && c <= '9'; })) {
        return false;
    }
    uint32_t value = 0;
    for (const char c : port) {
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (value > (65535U - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    return value == expected_port;
}

class Connection final : public std::enable_shared_from_this<Connection> {
public:
    Connection(asio::ip::tcp::socket socket, chain::ChainState& state,
               const chain::BlockIndex& index, mempool::Mempool* pool, uint16_t port)
        : socket_(std::move(socket)), state_(state), index_(index), pool_(pool),
          port_(port), timer_(socket_.get_executor()) {}

    void Start() {
        timer_.expires_after(REQUEST_TIMEOUT);
        timer_.async_wait([self = shared_from_this()](const std::error_code& ec) {
            if (!ec) self->Close();
        });
        Read();
    }

private:
    void Read() {
        socket_.async_read_some(asio::buffer(buffer_), [self = shared_from_this()](
                                    const std::error_code& ec, size_t count) {
            if (ec) return;
            self->request_.append(self->buffer_.data(), count);
            if (self->request_.size() > MAX_REQUEST) {
                self->Write(ErrorResponse(413, "Payload Too Large", "request too large"));
                return;
            }
            if (self->request_.find("\r\n\r\n") == std::string::npos) {
                self->Read();
                return;
            }
            self->Answer();
        });
    }

    void Answer() {
        const size_t line_end = request_.find("\r\n");
        if (line_end == std::string::npos) {
            Write(ErrorResponse(400, "Bad Request", "malformed request"));
            return;
        }
        const std::string_view line(request_.data(), line_end);
        const size_t first = line.find(' ');
        const size_t second = first == std::string_view::npos ? std::string_view::npos
                                                                : line.find(' ', first + 1);
        if (first == std::string_view::npos || second == std::string_view::npos ||
            line.substr(0, first) != "GET" || line.substr(second + 1) != "HTTP/1.1") {
            Write(ErrorResponse(405, "Method Not Allowed", "GET HTTP/1.1 required"));
            return;
        }
        const std::string_view host = Header(request_, "Host");
        if (!LoopbackHost(host, port_)) {
            Write(ErrorResponse(403, "Forbidden", "loopback Host required"));
            return;
        }
        const std::string_view target = line.substr(first + 1, second - first - 1);
        const size_t query = target.find('?');
        std::string_view path = target.substr(0, query);
        if (path == "/explorer") {
            path = "/";
        } else if (path.starts_with("/explorer/")) {
            path.remove_prefix(std::string_view("/explorer").size());
        } else {
            Write(ErrorResponse(404, "Not Found", "unknown explorer path"));
            return;
        }
        const ExplorerReply reply = Route(state_, index_, pool_, path);
        const std::string reason = reply.http_status == 200 ? "OK"
                                   : reply.http_status == 400 ? "Bad Request"
                                   : reply.http_status == 404 ? "Not Found"
                                                               : "Error";
        Write(Response(reply.http_status, reason, reply.body, reply.content_type));
    }

    void Write(std::string response) {
        auto body = std::make_shared<std::string>(std::move(response));
        asio::async_write(socket_, asio::buffer(*body),
                          [self = shared_from_this(), body](const std::error_code&, size_t) {
                              self->Close();
                          });
    }

    void Close() {
        std::error_code ignored;
        timer_.cancel();
        socket_.close(ignored);
    }

    asio::ip::tcp::socket socket_;
    chain::ChainState& state_;
    const chain::BlockIndex& index_;
    mempool::Mempool* pool_;
    uint16_t port_;
    asio::steady_timer timer_;
    std::array<char, 1024> buffer_{};
    std::string request_;
};

}  // namespace

struct Server::Impl {
    asio::io_context io{1};
    asio::ip::tcp::acceptor acceptor{io};
    chain::ChainState* state = nullptr;
    const chain::BlockIndex* index = nullptr;
    mempool::Mempool* pool = nullptr;
    uint16_t port = 0;

    void Accept() {
        acceptor.async_accept([this](const std::error_code& ec, asio::ip::tcp::socket socket) {
            if (!ec) {
                std::make_shared<Connection>(std::move(socket), *state, *index, pool, port)->Start();
            }
            if (acceptor.is_open()) Accept();
        });
    }
};

Server::Server(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;
Server::~Server() = default;

std::expected<Server, std::string> Server::Bind(chain::ChainState& state,
                                                const chain::BlockIndex& index,
                                                mempool::Mempool* pool,
                                                const ServerConfig& config) {
    auto impl = std::make_unique<Impl>();
    impl->state = &state;
    impl->index = &index;
    impl->pool = pool;
    std::error_code ec;
    impl->acceptor.open(asio::ip::tcp::v4(), ec);
    if (ec) return std::unexpected("cannot open explorer socket: " + ec.message());
    impl->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    impl->acceptor.bind({asio::ip::address_v4::loopback(), config.port}, ec);
    if (ec) return std::unexpected("cannot bind explorer socket: " + ec.message());
    impl->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) return std::unexpected("cannot listen for explorer requests: " + ec.message());
    impl->port = impl->acceptor.local_endpoint(ec).port();
    if (ec) return std::unexpected("cannot read explorer port: " + ec.message());
    return Server(std::move(impl));
}

uint16_t Server::Port() const noexcept { return impl_ == nullptr ? 0 : impl_->port; }

void Server::Serve() {
    impl_->Accept();
    impl_->io.run();
}

void Server::Stop() noexcept {
    if (impl_ != nullptr) impl_->io.stop();
}

}  // namespace amarian::explorer
