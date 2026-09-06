/// \file
/// Reader/Writer serialisation implementations for P2P message payloads.

#include <amarian/net/protocol.hpp>

#include <amarian/primitives/block.hpp>
#include <amarian/primitives/transaction.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace amarian::net {

// --- Helpers ----------------------------------------------------------------

[[nodiscard]] static bool ReadHash256Vec(Reader& reader, std::vector<Hash256>& out,
                                          size_t max_items) {
    uint64_t count = 0;
    if (!reader.ReadCompactSize(count, Hash256::SIZE)) {
        return false;
    }
    if (count > max_items) {
        reader.Fail();
        return false;
    }
    out.resize(static_cast<size_t>(count));
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        if (!reader.ReadHash256(out[i])) {
            return false;
        }
    }
    return true;
}

static void WriteHash256Vec(Writer& writer, const std::vector<Hash256>& vec) {
    writer.WriteCompactSize(vec.size());
    for (const auto& h : vec) {
        writer.WriteHash256(h);
    }
}

void SerializeInventory(Writer& writer, const Inventory& inv) {
    writer.WriteU32(static_cast<uint32_t>(inv.type));
    writer.WriteHash256(inv.hash);
}

bool DeserializeInventory(Reader& reader, Inventory& out) {
    uint32_t raw_type = 0;
    if (!reader.ReadU32(raw_type)) {
        return false;
    }
    Inventory tmp;
    tmp.type = static_cast<InventoryType>(raw_type);
    if (!reader.ReadHash256(tmp.hash)) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

// --- HelloPayload ----------------------------------------------------------

void HelloPayload::Serialize(Writer& writer) const {
    writer.WriteU32(protocol_version);
    writer.WriteU64(services);
    writer.WriteI64(timestamp);
    writer.WriteHash256(chain_id);
    writer.WriteU32(best_height);
    writer.WriteU64(nonce);
    writer.WriteByteString(ByteSpan(
        reinterpret_cast<const uint8_t*>(user_agent.data()), user_agent.size()));
}

bool HelloPayload::Deserialize(Reader& reader, HelloPayload& out, size_t max_user_agent_len) {
    HelloPayload tmp;
    if (!reader.ReadU32(tmp.protocol_version) ||
        !reader.ReadU64(tmp.services) ||
        !reader.ReadI64(tmp.timestamp) ||
        !reader.ReadHash256(tmp.chain_id) ||
        !reader.ReadU32(tmp.best_height) ||
        !reader.ReadU64(tmp.nonce)) {
        return false;
    }
    ByteVec ua;
    if (!reader.ReadByteString(ua, max_user_agent_len)) {
        return false;
    }
    tmp.user_agent = std::string(ua.begin(), ua.end());
    out = std::move(tmp);
    return true;
}

// --- HelloAckPayload -------------------------------------------------------

void HelloAckPayload::Serialize(Writer&) const {
    // No fields — nothing to write.
}

bool HelloAckPayload::Deserialize(Reader& reader, HelloAckPayload&) {
    // Must be empty payload.
    (void)reader;
    return true;
}

// --- GetHeadersPayload -----------------------------------------------------

void GetHeadersPayload::Serialize(Writer& writer) const {
    writer.WriteU32(protocol_version);
    WriteHash256Vec(writer, locator);
    writer.WriteHash256(stop_hash);
}

bool GetHeadersPayload::Deserialize(Reader& reader, GetHeadersPayload& out, size_t max_items) {
    GetHeadersPayload tmp;
    if (!reader.ReadU32(tmp.protocol_version)) {
        return false;
    }
    if (!ReadHash256Vec(reader, tmp.locator, max_items)) {
        return false;
    }
    if (!reader.ReadHash256(tmp.stop_hash)) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

// --- HeadersPayload --------------------------------------------------------

void HeadersPayload::Serialize(Writer& writer) const {
    writer.WriteCompactSize(headers.size());
    for (const auto& h : headers) {
        h.Serialize(writer);
    }
}

bool HeadersPayload::Deserialize(Reader& reader, HeadersPayload& out) {
    HeadersPayload tmp;
    uint64_t count = 0;
    if (!reader.ReadCompactSize(count, BlockHeader::SERIALIZED_SIZE)) {
        return false;
    }
    if (count > MAX_COUNT) {
        reader.Fail();
        return false;
    }
    tmp.headers.resize(static_cast<size_t>(count));
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        if (!BlockHeader::Deserialize(reader, tmp.headers[i])) {
            return false;
        }
    }
    out = std::move(tmp);
    return true;
}

// --- GetDataPayload --------------------------------------------------------

void GetDataPayload::Serialize(Writer& writer) const {
    writer.WriteCompactSize(items.size());
    for (const auto& inv : items) {
        SerializeInventory(writer, inv);
    }
}

bool GetDataPayload::Deserialize(Reader& reader, GetDataPayload& out, size_t max_items) {
    GetDataPayload tmp;
    uint64_t count = 0;
    // Minimum serialised size per item: 4 (type u32) + 32 (hash)
    if (!reader.ReadCompactSize(count, 36)) {
        return false;
    }
    if (count > max_items) {
        reader.Fail();
        return false;
    }
    tmp.items.resize(static_cast<size_t>(count));
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        if (!DeserializeInventory(reader, tmp.items[i])) {
            return false;
        }
    }
    out = std::move(tmp);
    return true;
}

// --- TxPayload -------------------------------------------------------------

void TxPayload::Serialize(Writer& writer) const {
    tx.Serialize(writer);
}

bool TxPayload::Deserialize(Reader& reader, TxPayload& out, const TxLimits& limits) {
    TxPayload tmp;
    if (!Transaction::Deserialize(reader, tmp.tx, limits)) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

// --- BlockPayload ----------------------------------------------------------

void BlockPayload::Serialize(Writer& writer) const {
    block.Serialize(writer);
}

bool BlockPayload::Deserialize(Reader& reader, BlockPayload& out, const BlockLimits& limits) {
    BlockPayload tmp;
    if (!Block::Deserialize(reader, tmp.block, limits)) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

// --- NetworkAddress --------------------------------------------------------

void NetworkAddress::Serialize(Writer& writer) const {
    writer.WriteU8(addr_ver);
    writer.WriteBytes(ByteSpan(address.data(), address.size()));
    writer.WriteU16(port);
}

bool NetworkAddress::Deserialize(Reader& reader, NetworkAddress& out) {
    NetworkAddress tmp;
    if (!reader.ReadU8(tmp.addr_ver)) {
        return false;
    }
    if (!reader.ReadBytes(tmp.address)) {
        return false;
    }
    if (!reader.ReadU16(tmp.port)) {
        return false;
    }
    out = std::move(tmp);
    return true;
}

// --- AddrPayload -----------------------------------------------------------

void AddrPayload::Serialize(Writer& writer) const {
    writer.WriteCompactSize(addresses.size());
    for (const auto& addr : addresses) {
        addr.Serialize(writer);
    }
}

bool AddrPayload::Deserialize(Reader& reader, AddrPayload& out, size_t max_items) {
    AddrPayload tmp;
    uint64_t count = 0;
    // 1 (ver) + 16 (address bytes) + 2 (port) = 19 min per entry
    if (!reader.ReadCompactSize(count, 19)) {
        return false;
    }
    if (count > max_items) {
        reader.Fail();
        return false;
    }
    tmp.addresses.resize(static_cast<size_t>(count));
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
        if (!NetworkAddress::Deserialize(reader, tmp.addresses[i])) {
            return false;
        }
    }
    out = std::move(tmp);
    return true;
}

// --- PingPayload -----------------------------------------------------------

void PingPayload::Serialize(Writer& writer) const {
    writer.WriteU64(nonce);
}

bool PingPayload::Deserialize(Reader& reader, PingPayload& out) {
    return reader.ReadU64(out.nonce);
}

}  // namespace amarian::net