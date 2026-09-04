#include <amarian/crypto/hash.hpp>

#include <openssl/sha.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace amarian {
namespace {

[[nodiscard]] Hash256 Digest(ByteSpan bytes) noexcept {
    std::array<uint8_t, Hash256::SIZE> digest{};
    // SHA256 is OpenSSL's vetted one-shot implementation. `digest` has exactly
    // SHA256_DIGEST_LENGTH bytes, pinned below, so OpenSSL cannot overrun it.
    static_assert(SHA256_DIGEST_LENGTH == Hash256::SIZE);
    // OpenSSL permits a zero-length message, but supply a real address in that
    // case rather than relying on its treatment of a null `data()` pointer.
    static constexpr uint8_t empty = 0;
    const uint8_t* data = bytes.empty() ? &empty : bytes.data();
    static_cast<void>(SHA256(data, bytes.size(), digest.data()));
    return Hash256(digest);
}

}  // namespace

Hash256 Sha256(ByteSpan bytes) noexcept {
    return Digest(bytes);
}

Hash256 DoubleSha256(ByteSpan bytes) noexcept {
    const Hash256 first = Sha256(bytes);
    return Sha256(first.Span());
}

Hash256 TaggedHash(std::string_view tag, ByteSpan message) {
    const auto* tag_data = reinterpret_cast<const uint8_t*>(tag.data());
    const ByteSpan tag_bytes(tag_data, tag.size());
    const Hash256 tag_hash = Sha256(tag_bytes);

    std::array<uint8_t, Hash256::SIZE * 2> prefix{};
    std::memcpy(prefix.data(), tag_hash.Data(), Hash256::SIZE);
    std::memcpy(prefix.data() + Hash256::SIZE, tag_hash.Data(), Hash256::SIZE);

    // SHA256() is one-shot, so compose its preimage explicitly. Tagged hashes
    // are fixed-size commitments in this protocol, so callers bound `message`
    // before reaching the crypto boundary.
    ByteVec preimage;
    preimage.reserve(prefix.size() + message.size());
    preimage.insert(preimage.end(), prefix.begin(), prefix.end());
    preimage.insert(preimage.end(), message.begin(), message.end());
    return Sha256(preimage);
}

}  // namespace amarian
