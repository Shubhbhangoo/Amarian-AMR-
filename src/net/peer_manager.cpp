/// \file
/// Peer connection management.

#include <amarian/net/peer_manager.hpp>

#include <amarian/net/message.hpp>
#include <amarian/net/protocol.hpp>

#include <asio.hpp>
#include <asio/ts/buffer.hpp>
#include <asio/ts/internet.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace amarian::net {

// --- One peer connection ---------------------------------------------------

/// Per-peer connection state.
class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
public:
    PeerConnection(asio::io_context& io, const ChainParams& params, uint64_t nonce,
                   const std::string& host, uint16_t port)
        : socket_(io), resolver_(io), params_(params), local_nonce_(nonce),
          host_(host), port_(port), handshake_(params, nonce),
          resolution_timer_(io), connect_timer_(io) {}

    ~PeerConnection() { Close(); }

    void Start(std::function<void(PeerHello)> on_connected,
               std::function<void(std::string)> on_disconnected);

    void Close();

    [[nodiscard]] bool IsConnected() const noexcept { return connected_; }
    [[nodiscard]] const PeerHello& Hello() const { return peer_hello_; }

private:
    void DoResolve();
    void OnResolve(asio::error_code ec, asio::ip::tcp::resolver::results_type results);
    void DoConnect(asio::ip::tcp::resolver::results_type::iterator endpoint);
    void OnConnect(asio::error_code ecode, asio::ip::tcp::resolver::results_type::iterator);
    void SendHello();
    void ReadHeader();
    void OnHeader(asio::error_code hdr_err, size_t);
    void OnBody(asio::error_code body_err, size_t);
    void HandleMessage(Message msg);

    asio::ip::tcp::socket socket_;
    asio::ip::tcp::resolver resolver_;
    const ChainParams& params_;
    uint64_t local_nonce_;
    std::string host_;
    uint16_t port_;
    Handshake handshake_;

    std::function<void(PeerHello)> on_connected_;
    std::function<void(std::string)> on_disconnected_;
    PeerHello peer_hello_;
    bool connected_ = false;

    // Read buffer
    std::array<uint8_t, HEADER_SIZE> header_buf_{};
    ByteVec body_buf_;

    asio::steady_timer resolution_timer_;
    asio::steady_timer connect_timer_;
};

void PeerConnection::Start(std::function<void(PeerHello)> on_connected,
                           std::function<void(std::string)> on_disconnected) {
    on_connected_ = std::move(on_connected);
    on_disconnected_ = std::move(on_disconnected);
    DoResolve();
}

void PeerConnection::Close() {
    asio::error_code ec;
    socket_.close(ec);
    connected_ = false;
}

void PeerConnection::DoResolve() {
    auto self = shared_from_this();
    resolver_.async_resolve(host_, std::to_string(port_),
        [this, self](asio::error_code ecode, asio::ip::tcp::resolver::results_type results) {
            OnResolve(ecode, results);
        });
}

void PeerConnection::OnResolve(asio::error_code ec,
                               asio::ip::tcp::resolver::results_type results) {
    if (ec || results.empty()) {
        if (on_disconnected_) on_disconnected_("resolve failed: " + ec.message());
        return;
    }
    DoConnect(results.begin());
}

void PeerConnection::DoConnect(asio::ip::tcp::resolver::results_type::iterator endpoint) {
    auto self = shared_from_this();
    socket_.async_connect(endpoint->endpoint(),
        [this, self](asio::error_code ecode) {
            OnConnect(ecode, asio::ip::tcp::resolver::results_type::iterator{});
        });
}

void PeerConnection::OnConnect(asio::error_code ecode,
                               asio::ip::tcp::resolver::results_type::iterator) {
    if (ecode) {
        if (on_disconnected_) on_disconnected_("connect failed: " + ecode.message());
        return;
    }
    connected_ = true;
    SendHello();
}

void PeerConnection::SendHello() {
    const HelloPayload hello = handshake_.BuildHello();
    Writer writer;
    hello.Serialize(writer);
    ByteVec payload = writer.Take();
    ByteVec message = SerialiseMessage(Command::Hello, payload, params_.magic);
    handshake_.OnSendHello();

    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(message),
        [this, self](asio::error_code ecode, size_t) {
            if (ecode) {
                if (on_disconnected_) on_disconnected_("send hello failed: " + ecode.message());
                return;
            }
            ReadHeader();
        });
}

void PeerConnection::ReadHeader() {
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(header_buf_),
        [this, self](asio::error_code ecode, size_t) {
            OnHeader(ecode, header_buf_.size());
        });
}

void PeerConnection::OnHeader(asio::error_code hdr_err, size_t) {
    if (hdr_err) {
        if (on_disconnected_) on_disconnected_("read header failed: " + hdr_err.message());
        return;
    }

    Reader reader(ByteSpan{header_buf_.data(), header_buf_.size()});
    std::array<uint8_t, 4> magic{};
    if (!reader.ReadBytes(magic) || magic != params_.magic) {
        if (on_disconnected_) on_disconnected_("bad magic");
        Close();
        return;
    }

    uint16_t cmd_raw = 0;
    uint32_t length = 0;
    uint32_t checksum = 0;
    reader.ReadU16(cmd_raw);
    reader.ReadU32(length);
    reader.ReadU32(checksum);

    if (length > MAX_MESSAGE_SIZE) {
        if (on_disconnected_) on_disconnected_("oversized message");
        Close();
        return;
    }

    body_buf_.resize(length);
    if (length == 0) {
        Message msg;
        msg.command = static_cast<Command>(cmd_raw);
        HandleMessage(std::move(msg));
        ReadHeader();
        return;
    }

    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(body_buf_),
        [this, self](asio::error_code body_err, size_t) {
            OnBody(body_err, body_buf_.size());
        });
}

void PeerConnection::OnBody(asio::error_code body_err, size_t) {
    if (body_err) {
        if (on_disconnected_) on_disconnected_("read body failed: " + body_err.message());
        return;
    }

    ByteVec full_message;
    full_message.reserve(HEADER_SIZE + body_buf_.size());
    full_message.insert(full_message.end(), header_buf_.begin(), header_buf_.end());
    full_message.insert(full_message.end(), body_buf_.begin(), body_buf_.end());

    Framed framed = FrameMessage(full_message, params_.magic);
    if (framed.result != FramingResult::Complete) {
        if (on_disconnected_) on_disconnected_("framing error: " + framed.failure);
        Close();
        return;
    }

    HandleMessage(std::move(framed.message));
    ReadHeader();
}

void PeerConnection::HandleMessage(Message msg) {
    switch (msg.command) {
    case Command::Hello: {
        Reader reader(msg.payload);
        HelloPayload hello;
        if (!HelloPayload::Deserialize(reader, hello, 256)) {
            if (on_disconnected_) on_disconnected_("malformed hello");
            Close();
            return;
        }
        auto ack = handshake_.ReceiveHello(hello);
        if (!ack.has_value()) {
            if (on_disconnected_) on_disconnected_(handshake_.FailureReason());
            Close();
            return;
        }
        ByteVec payload;
        Writer writer;
        ack->Serialize(writer);
        payload = writer.Take();
        ByteVec msg_out = SerialiseMessage(Command::HelloAck, payload, params_.magic);
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(msg_out),
            [this, self](asio::error_code ecode, size_t) {
                if (ecode && on_disconnected_) {
                    on_disconnected_("send helloack failed: " + ecode.message());
                }
            });

        if (handshake_.State() == HandshakeState::AwaitingSendHello) {
            SendHello();
        }
        break;
    }
    case Command::HelloAck: {
        if (!handshake_.OnReceiveHelloAck()) {
            if (on_disconnected_) on_disconnected_(handshake_.FailureReason());
            Close();
            return;
        }
        peer_hello_ = handshake_.Peer();
        connected_ = true;
        if (on_connected_) on_connected_(peer_hello_);
        break;
    }
    case Command::GetHeaders:
    case Command::Headers:
    case Command::Inv:
    case Command::GetData:
    case Command::Tx:
    case Command::Block:
    case Command::GetAddr:
    case Command::Addr:
    case Command::Ping:
    case Command::Pong:
        break;
    }
}

// --- PeerManager -----------------------------------------------------------

struct PeerManager::Impl {
    asio::io_context io_context;
    const NetConfig config;
    std::vector<std::shared_ptr<PeerConnection>> connections;
    uint64_t local_nonce;

    Impl(const NetConfig& cfg)
        : config(cfg) {
        std::random_device rd;
        local_nonce = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    }
};

PeerManager::PeerManager(const NetConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

PeerManager::~PeerManager() {
    Stop();
}

void PeerManager::Start() {
    auto& io = impl_->io_context;
    const auto& params = *impl_->config.params;

    for (const auto& peer : impl_->config.connect) {
        auto colon = peer.find(':');
        std::string host;
        uint16_t port = params.default_p2p_port;
        if (colon != std::string::npos) {
            host = peer.substr(0, colon);
            port = static_cast<uint16_t>(std::stoul(peer.substr(colon + 1)));
        } else {
            host = peer;
        }

        auto conn = std::make_shared<PeerConnection>(io, params, impl_->local_nonce,
                                                      host, port);
        conn->Start(
            [](PeerHello) {},
            [this, &params, host, port](std::string) {
                auto reconnect_timer = std::make_shared<asio::steady_timer>(impl_->io_context);
                reconnect_timer->expires_after(impl_->config.reconnect_delay);
                reconnect_timer->async_wait(
                    [this, &params, host, port, reconnect_timer](asio::error_code ec) {
                        if (ec) return;
                        auto new_conn = std::make_shared<PeerConnection>(
                            impl_->io_context, params, impl_->local_nonce, host, port);
                        impl_->connections.push_back(new_conn);
                        new_conn->Start(
                            [](PeerHello) {},
                            [](std::string) {});
                    });
            });
        impl_->connections.push_back(conn);
    }
}

void PeerManager::Stop() {
    for (auto& conn : impl_->connections) {
        conn->Close();
    }
    impl_->connections.clear();
    impl_->io_context.stop();
}

bool PeerManager::HasActivePeer() const noexcept {
    for (const auto& conn : impl_->connections) {
        if (conn->IsConnected()) return true;
    }
    return false;
}

size_t PeerManager::PeerCount() const noexcept {
    return impl_->connections.size();
}

uint32_t PeerManager::BestPeerHeight() const noexcept {
    uint32_t best = 0;
    for (const auto& conn : impl_->connections) {
        if (conn->IsConnected()) {
            best = std::max(best, conn->Hello().best_height);
        }
    }
    return best;
}

}  // namespace amarian::net