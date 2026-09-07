/// \file
/// Peer connection management.

#include <amarian/net/peer_manager.hpp>

#include <amarian/net/message.hpp>
#include <amarian/net/protocol.hpp>
#include <amarian/util/logging.hpp>

#include <asio.hpp>
#include <asio/ts/buffer.hpp>
#include <asio/ts/internet.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <sstream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace amarian::net {

// --- One peer connection ---------------------------------------------------

/// Per-peer connection state.
class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
public:
    using MessageHandler = std::function<void(Command, ByteVec)>;
    using TrafficHandler = std::function<void(uint64_t, uint64_t)>;
    PeerConnection(asio::io_context& io, const ChainParams& params, uint64_t nonce,
                   const std::string& host, uint16_t port)
        : socket_(io), resolver_(io), params_(params), host_(host), port_(port),
          handshake_(params, nonce),
          resolution_timer_(io), connect_timer_(io), ping_timer_(io) {}

    PeerConnection(asio::ip::tcp::socket socket, const ChainParams& params, uint64_t nonce)
        : socket_(std::move(socket)), resolver_(socket_.get_executor()), params_(params),
          handshake_(params, nonce), inbound_(true), resolution_timer_(socket_.get_executor()),
          connect_timer_(socket_.get_executor()), ping_timer_(socket_.get_executor()) {}

    ~PeerConnection() { Close(); }

    void Start(std::function<void(PeerHello)> on_connected,
               std::function<void(std::string)> on_disconnected,
               MessageHandler on_message, TrafficHandler on_traffic);

    void StartInbound(std::function<void(PeerHello)> on_connected,
                      std::function<void(std::string)> on_disconnected,
                      MessageHandler on_message, TrafficHandler on_traffic);

    void Send(Command command, ByteVec payload);

    void Close();

    [[nodiscard]] bool IsConnected() const noexcept { return connected_; }
    [[nodiscard]] const PeerHello& Hello() const { return peer_hello_; }
    [[nodiscard]] std::string EndpointKey() const;

private:
    void DoResolve();
    void OnResolve(asio::error_code ec, asio::ip::tcp::resolver::results_type results);
    void DoConnect(asio::ip::tcp::resolver::results_type::iterator endpoint);
    void OnConnect(asio::error_code ecode, asio::ip::tcp::resolver::results_type::iterator);
    void SendHello(bool read_after_send = true);
    void ReadHeader();
    void OnHeader(asio::error_code hdr_err, size_t);
    void OnBody(asio::error_code body_err, size_t);
    void HandleMessage(Message msg);
    void WriteNext();
    void StartKeepalive();

    asio::ip::tcp::socket socket_;
    asio::ip::tcp::resolver resolver_;
    const ChainParams& params_;
    std::string host_;
    uint16_t port_;
    Handshake handshake_;

    std::function<void(PeerHello)> on_connected_;
    std::function<void(std::string)> on_disconnected_;
    MessageHandler on_message_;
    TrafficHandler on_traffic_;
    PeerHello peer_hello_;
    bool connected_ = false;
    bool inbound_ = false;

    // Read buffer
    std::array<uint8_t, HEADER_SIZE> header_buf_{};
    ByteVec body_buf_;

    asio::steady_timer resolution_timer_;
    asio::steady_timer connect_timer_;
    asio::steady_timer ping_timer_;
    std::deque<ByteVec> write_queue_;
    bool writing_ = false;
    bool ping_outstanding_ = false;
};

void PeerConnection::Start(std::function<void(PeerHello)> on_connected,
                           std::function<void(std::string)> on_disconnected,
                           MessageHandler on_message, TrafficHandler on_traffic) {
    on_connected_ = std::move(on_connected);
    on_disconnected_ = std::move(on_disconnected);
    on_message_ = std::move(on_message);
    on_traffic_ = std::move(on_traffic);
    DoResolve();
}

std::string PeerConnection::EndpointKey() const {
    if (!host_.empty()) return host_ + ":" + std::to_string(port_);
    asio::error_code ec;
    const auto endpoint = socket_.remote_endpoint(ec);
    if (ec) return {};
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

void PeerConnection::StartInbound(std::function<void(PeerHello)> on_connected,
                                  std::function<void(std::string)> on_disconnected,
                                  MessageHandler on_message, TrafficHandler on_traffic) {
    on_connected_ = std::move(on_connected);
    on_disconnected_ = std::move(on_disconnected);
    on_message_ = std::move(on_message);
    on_traffic_ = std::move(on_traffic);
    ReadHeader();
}

void PeerConnection::StartKeepalive() {
    ping_timer_.expires_after(std::chrono::seconds{30});
    auto self = shared_from_this();
    ping_timer_.async_wait([this, self](asio::error_code ec) {
        if (ec || !connected_) return;
        if (ping_outstanding_) {
            if (on_disconnected_) on_disconnected_("ping timeout");
            Close();
            return;
        }
        ping_outstanding_ = true;
        PingPayload ping;
        ping.nonce = 0xA6A6A6A6ULL;
        Writer writer;
        ping.Serialize(writer);
        Send(Command::Ping, writer.Take());
        ping_timer_.expires_after(std::chrono::seconds{10});
        ping_timer_.async_wait([this, self](asio::error_code timeout_ec) {
            if (timeout_ec || !connected_) return;
            if (ping_outstanding_) {
                if (on_disconnected_) on_disconnected_("ping timeout");
                Close();
            } else {
                StartKeepalive();
            }
        });
    });
}

void PeerConnection::Send(Command command, ByteVec payload) {
    write_queue_.push_back(SerialiseMessage(command, payload, params_.magic));
    if (writing_) return;
    writing_ = true;
    WriteNext();
}

void PeerConnection::WriteNext() {
    if (write_queue_.empty()) {
        writing_ = false;
        return;
    }
    const size_t message_bytes = write_queue_.front().size();
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(write_queue_.front()),
        [this, self, message_bytes](asio::error_code ec, size_t) {
            if (ec) {
                writing_ = false;
                if (on_disconnected_) on_disconnected_("send message failed: " + ec.message());
                return;
            }
            write_queue_.pop_front();
            if (on_traffic_) on_traffic_(message_bytes, 0);
            if (write_queue_.empty()) {
                writing_ = false;
                return;
            }
            WriteNext();
        });
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
    SendHello();
}

void PeerConnection::SendHello(bool read_after_send) {
    const HelloPayload hello = handshake_.BuildHello();
    Writer writer;
    hello.Serialize(writer);
    ByteVec payload = writer.Take();
    ByteVec message = SerialiseMessage(Command::Hello, payload, params_.magic);
    handshake_.OnSendHello();

    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(message),
        [this, self, read_after_send](asio::error_code ecode, size_t bytes) {
            if (ecode) {
                if (on_disconnected_) on_disconnected_("send hello failed: " + ecode.message());
                return;
            }
            if (on_traffic_) on_traffic_(bytes, 0);
            if (read_after_send) ReadHeader();
        });
}

void PeerConnection::ReadHeader() {
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(header_buf_),
        [this, self](asio::error_code ecode, size_t bytes) {
            OnHeader(ecode, bytes);
        });
}

void PeerConnection::OnHeader(asio::error_code hdr_err, size_t) {
    if (hdr_err) {
        if (on_disconnected_) on_disconnected_("read header failed: " + hdr_err.message());
        return;
    }

    if (on_traffic_) on_traffic_(0, HEADER_SIZE);
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

    if (on_traffic_) on_traffic_(0, body_buf_.size());

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
        if (inbound_) {
            // The inbound side must put its Hello before HelloAck on the wire. Sending the
            // two async writes independently lets the empty ack overtake the identity message,
            // which makes the outbound state machine reject an otherwise valid peer.
            Writer hello_writer;
            const HelloPayload own_hello = handshake_.BuildHello();
            own_hello.Serialize(hello_writer);
            const ByteVec hello_payload = hello_writer.Take();
            const ByteVec hello_message =
                SerialiseMessage(Command::Hello, hello_payload, params_.magic);
            ByteVec ordered;
            ordered.reserve(hello_message.size() + msg_out.size());
            ordered.insert(ordered.end(), hello_message.begin(), hello_message.end());
            ordered.insert(ordered.end(), msg_out.begin(), msg_out.end());
            msg_out = std::move(ordered);
        }
        auto self = shared_from_this();
        const size_t hello_response_bytes = msg_out.size();
        asio::async_write(socket_, asio::buffer(msg_out),
            [this, self, hello_response_bytes](asio::error_code ecode, size_t bytes) {
                if (ecode && on_disconnected_) {
                    on_disconnected_("send helloack failed: " + ecode.message());
                }
                if (!ecode && on_traffic_) on_traffic_(bytes == 0 ? hello_response_bytes : bytes, 0);
            });

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
        StartKeepalive();
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
        ping_outstanding_ = false;
        break;
    }
    if (msg.command != Command::Hello && msg.command != Command::HelloAck && on_message_) {
        on_message_(msg.command, std::move(msg.payload));
    }
}

// --- PeerManager -----------------------------------------------------------

struct PeerManager::Impl {
    asio::io_context io_context;
    const NetConfig config;
    std::vector<std::shared_ptr<PeerConnection>> connections;
    uint64_t local_nonce;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
    std::thread io_thread;
    bool started = false;
    std::unordered_set<Hash256> announced_blocks;
    std::unordered_set<Hash256> announced_transactions;
    std::unordered_map<std::string, int> misbehavior;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> bans;
    std::vector<NetworkAddress> addresses;
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};

    PeerConnection::TrafficHandler TrafficCallback() {
        return [this](uint64_t sent, uint64_t received) {
            bytes_sent.fetch_add(sent, std::memory_order_relaxed);
            bytes_received.fetch_add(received, std::memory_order_relaxed);
        };
    }

    static std::string AddressKey(const NetworkAddress& address) {
        std::ostringstream out;
        out << static_cast<unsigned>(address.addr_ver) << ':' << address.port << ':';
        out << std::hex;
        for (const auto byte : address.address) out << static_cast<unsigned>(byte);
        return out.str();
    }

    void AddAddress(const NetworkAddress& address) {
        if ((address.addr_ver != 4 && address.addr_ver != 6) || address.port == 0) return;
        const auto key = AddressKey(address);
        for (const auto& known : addresses) {
            if (AddressKey(known) == key) return;
        }
        if (addresses.size() >= 1'000) addresses.erase(addresses.begin());
        addresses.push_back(address);
    }

    void LoadAddresses() {
        if (config.address_book.empty()) return;
        std::ifstream file(config.address_book);
        unsigned version = 0;
        unsigned port = 0;
        std::string hex_address;
        while (file >> version >> port >> hex_address && hex_address.size() == 32) {
            NetworkAddress address;
            address.addr_ver = static_cast<uint8_t>(version);
            address.port = static_cast<uint16_t>(port);
            for (size_t i = 0; i < address.address.size(); ++i) {
                unsigned byte = 0;
                std::istringstream(hex_address.substr(i * 2, 2)) >> std::hex >> byte;
                address.address[i] = static_cast<uint8_t>(byte);
            }
            AddAddress(address);
        }
    }

    void SaveAddresses() const {
        if (config.address_book.empty()) return;
        std::ofstream file(config.address_book, std::ios::trunc);
        if (!file) return;
        for (const auto& address : addresses) {
            file << static_cast<unsigned>(address.addr_ver) << ' ' << address.port << ' ';
            file << std::hex;
            for (const auto byte : address.address) {
                file.width(2);
                file.fill('0');
                file << static_cast<unsigned>(byte);
            }
            file << std::dec << '\n';
        }
    }

    bool IsBanned(const std::string& endpoint) {
        const auto it = bans.find(endpoint);
        if (it == bans.end()) return false;
        if (std::chrono::steady_clock::now() >= it->second) {
            bans.erase(it);
            misbehavior.erase(endpoint);
            return false;
        }
        return true;
    }

    void Score(const std::shared_ptr<PeerConnection>& peer, int points,
               std::string_view reason) {
        const auto endpoint = peer->EndpointKey();
        if (endpoint.empty()) return;
        const int score = (misbehavior[endpoint] += points);
        AMARIAN_WARN(log::Category::Net, "p2p: peer {} misbehavior +{} ({} total): {}",
                     endpoint, points, score, reason);
        if (score >= config.ban_threshold) {
            bans[endpoint] = std::chrono::steady_clock::now() + config.ban_duration;
            peer->Close();
            AMARIAN_WARN(log::Category::Net, "p2p: temporarily banned {}", endpoint);
        }
    }

    void Send(const std::shared_ptr<PeerConnection>& peer, Command command,
              const ByteVec& payload) {
        peer->Send(command, payload);
    }

    void StartSync(const std::shared_ptr<PeerConnection>& peer) {
        GetHeadersPayload request;
        request.protocol_version = 1;
        request.locator.push_back(config.state->Tip().hash);
        if (request.locator.front() != config.state->Index().Genesis().hash) {
            request.locator.push_back(config.state->Index().Genesis().hash);
        }
        Writer writer;
        request.Serialize(writer);
        Send(peer, Command::GetHeaders, writer.Take());
    }

    void HandleMessage(const std::shared_ptr<PeerConnection>& peer, Command command,
                       ByteVec payload);

    void AnnounceBlock(const Hash256& hash) {
        if (!announced_blocks.insert(hash).second) return;
        GetDataPayload announcement;
        announcement.items.push_back(Inventory{InventoryType::Block, hash});
        Writer writer;
        announcement.Serialize(writer);
        const ByteVec payload = writer.Take();
        for (const auto& peer : connections) {
            if (peer->IsConnected()) peer->Send(Command::Inv, payload);
        }
    }

    void AnnounceTransaction(const Hash256& hash) {
        if (!announced_transactions.insert(hash).second) return;
        InvPayload announcement;
        announcement.items.push_back(Inventory{InventoryType::Transaction, hash});
        Writer writer;
        announcement.Serialize(writer);
        const ByteVec payload = writer.Take();
        for (const auto& peer : connections) {
            if (peer->IsConnected()) peer->Send(Command::Inv, payload);
        }
    }

    Impl(const NetConfig& cfg)
        : config(cfg) {
        std::random_device rd;
        local_nonce = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
        LoadAddresses();
    }

    ~Impl() { SaveAddresses(); }
};

void PeerManager::Impl::HandleMessage(const std::shared_ptr<PeerConnection>& peer,
                                      Command command, ByteVec payload) {
    AMARIAN_DEBUG(log::Category::Net, "p2p: received {} ({} bytes)", CommandName(command),
                  payload.size());
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    if (command == Command::GetHeaders) {
        Reader reader(payload);
        GetHeadersPayload request;
        if (!GetHeadersPayload::Deserialize(reader, request, 101) || !reader.Finish()) {
            peer->Close();
            return;
        }

        uint32_t start_height = 0;
        const auto& chain = config.state->Chain();
        for (const auto& locator : request.locator) {
            for (size_t height = chain.Length(); height > 0; --height) {
                const auto* entry = chain.AtHeight(static_cast<uint32_t>(height - 1));
                if (entry != nullptr && entry->hash == locator) {
                    start_height = static_cast<uint32_t>(height);
                    goto found_locator;
                }
            }
        }
    found_locator:
        HeadersPayload response;
        for (uint32_t height = start_height;
             height < chain.Length() && response.headers.size() < HeadersPayload::MAX_COUNT;
             ++height) {
            const auto* entry = chain.AtHeight(height);
            if (entry == nullptr) break;
            response.headers.push_back(entry->header);
            if (entry->hash == request.stop_hash) break;
        }
        Writer writer;
        response.Serialize(writer);
        Send(peer, Command::Headers, writer.Take());
        return;
    }

    if (command == Command::Headers) {
        Reader reader(payload);
        HeadersPayload headers;
        if (!HeadersPayload::Deserialize(reader, headers) || !reader.Finish()) {
            peer->Close();
            return;
        }
        GetDataPayload request;
        for (const auto& header : headers.headers) {
            const auto accepted = config.state->AcceptHeader(header, now);
            if (!accepted.has_value()) {
                peer->Close();
                return;
            }
            const auto* entry = *accepted;
            if (!config.state->Store().HaveBlock(entry->hash)) {
                request.items.push_back(Inventory{InventoryType::Block, entry->hash});
            }
        }
        if (!request.items.empty()) {
            Writer writer;
            request.Serialize(writer);
            Send(peer, Command::GetData, writer.Take());
        } else if (headers.headers.empty()) {
            AMARIAN_INFO(log::Category::Net, "p2p: synchronized at height {}",
                         config.state->Tip().height);
        }
        return;
    }

    if (command == Command::GetData) {
        Reader reader(payload);
        GetDataPayload request;
        if (!GetDataPayload::Deserialize(reader, request, 2'000) || !reader.Finish()) {
            peer->Close();
            return;
        }
        for (const auto& item : request.items) {
            if (item.type == InventoryType::Block) {
                const auto block = config.state->Store().GetBlock(item.hash);
                if (!block.has_value()) continue;
                BlockPayload response{.block = *block};
                Writer writer;
                response.Serialize(writer);
                Send(peer, Command::Block, writer.Take());
            } else if (item.type == InventoryType::Transaction && config.get_transaction) {
                const auto tx = config.get_transaction(item.hash);
                if (!tx.has_value()) continue;
                TxPayload response{.tx = *tx};
                Writer writer;
                response.Serialize(writer);
                Send(peer, Command::Tx, writer.Take());
            }
        }
        return;
    }

    if (command == Command::Inv) {
        Reader reader(payload);
        InvPayload inventory;
        if (!InvPayload::Deserialize(reader, inventory, 2'000) || !reader.Finish()) {
            peer->Close();
            return;
        }
        GetDataPayload request;
        for (const auto& item : inventory.items) {
            if ((item.type == InventoryType::Block &&
                 !config.state->Store().HaveBlock(item.hash)) ||
                (item.type == InventoryType::Transaction && config.get_transaction &&
                 !config.get_transaction(item.hash).has_value())) {
                request.items.push_back(item);
            }
        }
        if (!request.items.empty()) {
            Writer writer;
            request.Serialize(writer);
            Send(peer, Command::GetData, writer.Take());
        }
        return;
    }

    if (command == Command::Block) {
        Reader reader(payload);
        BlockPayload block;
        if (!BlockPayload::Deserialize(reader, block, config.params->block_limits) ||
            !reader.Finish()) {
            peer->Close();
            return;
        }
        const Hash256 hash = block.block.header.Hash();
        const bool already_have = config.state->Store().HaveBlock(hash);
        if (!config.state->AcceptBlock(block.block, now).has_value() ||
            !config.state->ActivateBestChain().has_value()) {
            peer->Close();
            return;
        }
        if (config.chain_changed) config.chain_changed();
        AMARIAN_INFO(log::Category::Net, "p2p: accepted block {} at height {}",
                     block.block.header.Hash().ToHex(), block.block.header.height);
        if (!already_have) AnnounceBlock(hash);
        StartSync(peer);
        return;
    }

    if (command == Command::Tx) {
        Reader reader(payload);
        TxPayload tx;
        if (!TxPayload::Deserialize(reader, tx, config.params->block_limits.tx) ||
            !reader.Finish()) {
            peer->Close();
            return;
        }
        if (config.accept_transaction && config.accept_transaction(tx.tx)) {
            AnnounceTransaction(tx.tx.Wtxid());
        }
        return;
    }

    if (command == Command::GetAddr) {
        Reader reader(payload);
        GetAddrPayload request;
        if (!GetAddrPayload::Deserialize(reader, request) || !reader.Finish()) {
            Score(peer, 20, "malformed getaddr");
            peer->Close();
            return;
        }
        AddrPayload response;
        response.addresses = addresses;
        Writer writer;
        response.Serialize(writer);
        Send(peer, Command::Addr, writer.Take());
        return;
    }

    if (command == Command::Addr) {
        Reader reader(payload);
        AddrPayload received;
        if (!AddrPayload::Deserialize(reader, received, 1'000) || !reader.Finish()) {
            Score(peer, 20, "malformed addr");
            peer->Close();
            return;
        }
        for (const auto& address : received.addresses) AddAddress(address);
        SaveAddresses();
        return;
    }

    if (command == Command::Ping) {
        Reader reader(payload);
        PingPayload ping;
        if (!PingPayload::Deserialize(reader, ping) || !reader.Finish()) {
            peer->Close();
            return;
        }
        Writer writer;
        ping.Serialize(writer);
        Send(peer, Command::Pong, writer.Take());
    } else if (command == Command::Pong) {
        Reader reader(payload);
        PingPayload pong;
        if (!PingPayload::Deserialize(reader, pong) || !reader.Finish()) {
            peer->Close();
        }
    }
}

PeerManager::PeerManager(const NetConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

PeerManager::~PeerManager() {
    Stop();
}

void PeerManager::Start() {
    if (impl_->started) return;
    impl_->started = true;
    auto& io = impl_->io_context;
    const auto& params = *impl_->config.params;

    if (impl_->config.p2p_port != 0) {
        impl_->acceptor = std::make_unique<asio::ip::tcp::acceptor>(io);
        asio::error_code listen_ec;
        const asio::ip::tcp::endpoint endpoint(asio::ip::address_v4::any(),
                                               impl_->config.p2p_port);
        impl_->acceptor->open(endpoint.protocol(), listen_ec);
        if (!listen_ec) {
            impl_->acceptor->set_option(asio::socket_base::reuse_address(true), listen_ec);
        }
        if (!listen_ec) impl_->acceptor->bind(endpoint, listen_ec);
        if (!listen_ec) {
            impl_->acceptor->listen(asio::socket_base::max_listen_connections, listen_ec);
        }
        if (listen_ec) {
            AMARIAN_ERROR(log::Category::General, "p2p: cannot listen on port {}: {}",
                          impl_->config.p2p_port, listen_ec.message());
            impl_->acceptor.reset();
        } else {
            AMARIAN_INFO(log::Category::General, "p2p: listening on 0.0.0.0:{}",
                         impl_->config.p2p_port);
            auto accept_next = std::make_shared<std::function<void()>>();
            *accept_next = [this, accept_next]() {
                if (!impl_->acceptor) return;
                impl_->acceptor->async_accept(
                    [this, accept_next](asio::error_code accept_ec,
                                        asio::ip::tcp::socket socket) {
                        if (!accept_ec) {
                            if (impl_->connections.size() >= impl_->config.max_inbound +
                                impl_->config.max_outbound) {
                                asio::error_code close_ec;
                                socket.close(close_ec);
                                if (impl_->acceptor) (*accept_next)();
                                return;
                            }
                            asio::error_code endpoint_ec;
                            const auto remote = socket.remote_endpoint(endpoint_ec);
                            if (!endpoint_ec && impl_->IsBanned(remote.address().to_string() + ":" +
                                                               std::to_string(remote.port()))) {
                                asio::error_code close_ec;
                                socket.close(close_ec);
                                if (impl_->acceptor) (*accept_next)();
                                return;
                            }
                            const auto& peer_params = *impl_->config.params;
                            auto conn = std::make_shared<PeerConnection>(
                                std::move(socket), peer_params, impl_->local_nonce);
                            std::weak_ptr<PeerConnection> weak_conn = conn;
                            conn->StartInbound(
                                [this, weak_conn](PeerHello hello) {
                                    AMARIAN_INFO(log::Category::Net,
                                                 "p2p: inbound handshake with {} at height {}",
                                                 hello.user_agent, hello.best_height);
                                    if (auto peer = weak_conn.lock()) {
                                        impl_->StartSync(peer);
                                        peer->Send(Command::GetAddr, {});
                                    }
                                },
                                [](std::string reason) {
                                    AMARIAN_DEBUG(log::Category::Net,
                                                  "p2p: inbound peer disconnected: {}", reason);
                                },
                                [this, weak_conn](Command command, ByteVec payload) {
                                    if (auto peer = weak_conn.lock()) {
                                        impl_->HandleMessage(peer, command, std::move(payload));
                                    }
                                },
                                impl_->TrafficCallback());
                            impl_->connections.push_back(std::move(conn));
                        }
                        if (impl_->acceptor) (*accept_next)();
                    });
            };
            (*accept_next)();
        }
    }

    size_t outbound_started = 0;
    for (const auto& peer : impl_->config.connect) {
        if (outbound_started >= impl_->config.max_outbound) break;
        ++outbound_started;
        auto colon = peer.find(':');
        std::string host;
        uint16_t port = params.default_p2p_port;
        if (colon != std::string::npos) {
            host = peer.substr(0, colon);
            port = static_cast<uint16_t>(std::stoul(peer.substr(colon + 1)));
        } else {
            host = peer;
        }

        AMARIAN_INFO(log::Category::Net, "p2p: connecting to {}:{}", host, port);

        auto conn = std::make_shared<PeerConnection>(io, params, impl_->local_nonce,
                                                      host, port);
        std::weak_ptr<PeerConnection> weak_conn = conn;
        conn->Start(
            [this, weak_conn](PeerHello hello) {
                AMARIAN_INFO(log::Category::Net,
                             "p2p: outbound handshake with {} at height {}",
                             hello.user_agent, hello.best_height);
                if (auto connected_peer = weak_conn.lock()) {
                    impl_->StartSync(connected_peer);
                    connected_peer->Send(Command::GetAddr, {});
                }
            },
            [this, &params, host, port](std::string reason) {
                AMARIAN_DEBUG(log::Category::Net, "p2p: outbound peer {}:{}: {}", host, port,
                              reason);
                auto reconnect_timer = std::make_shared<asio::steady_timer>(impl_->io_context);
                reconnect_timer->expires_after(impl_->config.reconnect_delay);
                reconnect_timer->async_wait(
                    [this, &params, host, port, reconnect_timer](asio::error_code ec) {
                        if (ec) return;
                        auto new_conn = std::make_shared<PeerConnection>(
                            impl_->io_context, params, impl_->local_nonce, host, port);
                        impl_->connections.push_back(new_conn);
                        std::weak_ptr<PeerConnection> weak_new = new_conn;
                        new_conn->Start(
                            [this, weak_new](PeerHello) {
                                if (auto connected_peer = weak_new.lock()) {
                                    impl_->StartSync(connected_peer);
                                }
                            },
                            [](std::string) {},
                            [this, weak_new](Command command, ByteVec payload) {
                                if (auto connected_peer = weak_new.lock()) {
                                    impl_->HandleMessage(connected_peer, command,
                                                         std::move(payload));
                                }
                            },
                            impl_->TrafficCallback());
                    });
            },
            [this, weak_conn](Command command, ByteVec payload) {
                if (auto connected_peer = weak_conn.lock()) {
                    impl_->HandleMessage(connected_peer, command, std::move(payload));
                }
            },
            impl_->TrafficCallback());
        impl_->connections.push_back(conn);
    }

    impl_->io_thread = std::thread([this] { impl_->io_context.run(); });
}

void PeerManager::Stop() {
    if (!impl_->started) return;
    impl_->started = false;
    if (impl_->acceptor) {
        asio::error_code ec;
        impl_->acceptor->close(ec);
        impl_->acceptor.reset();
    }
    for (auto& conn : impl_->connections) {
        conn->Close();
    }
    impl_->connections.clear();
    impl_->io_context.stop();
    if (impl_->io_thread.joinable()) impl_->io_thread.join();
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

TrafficStats PeerManager::Traffic() const noexcept {
    return TrafficStats{
        .bytes_sent = impl_->bytes_sent.load(std::memory_order_relaxed),
        .bytes_received = impl_->bytes_received.load(std::memory_order_relaxed),
    };
}

void PeerManager::AnnounceBlock(const Hash256& hash) {
    if (!impl_->started) return;
    impl_->AnnounceBlock(hash);
}

void PeerManager::AnnounceTransaction(const Hash256& hash) {
    if (!impl_->started) return;
    impl_->AnnounceTransaction(hash);
}

}  // namespace amarian::net
