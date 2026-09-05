#include <amarian/crypto/random.hpp>

#include <openssl/rand.h>

#include <algorithm>
#include <cstdint>
#include <span>

namespace amarian::crypto {

bool RandomBytes(std::span<uint8_t> out) noexcept {
    // `RAND_bytes` takes an `int` count, so a request larger than `INT_MAX` is filled in
    // pieces rather than truncated by a narrowing cast. No caller here asks for anything near
    // that, which is exactly why the cast would never be caught being wrong.
    constexpr size_t CHUNK = 1U << 20;
    size_t filled = 0;
    while (filled < out.size()) {
        const size_t take = std::min(CHUNK, out.size() - filled);
        // Returns 1 for success and 0 or -1 otherwise. It is OpenSSL 3's DRBG, seeded from
        // the platform source, and it is documented to fail rather than return low-quality
        // output — which is why the failure is passed on instead of retried.
        if (RAND_bytes(out.data() + filled, static_cast<int>(take)) != 1) {
            return false;
        }
        filled += take;
    }
    return true;
}

ByteVec RandomByteVec(size_t count) {
    ByteVec bytes(count, 0);
    if (!RandomBytes(bytes)) {
        return {};
    }
    return bytes;
}

}  // namespace amarian::crypto
