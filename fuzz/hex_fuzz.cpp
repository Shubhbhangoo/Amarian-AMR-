/// \file
/// Fuzz target for the hex decoder.
///
/// Hex strings arrive from RPC arguments, configuration files and test vectors.
/// The decoder must therefore survive arbitrary bytes: no crash, no
/// out-of-bounds read, and no acceptance of input it should have rejected.
///
/// Beyond memory safety, this harness asserts two correctness properties that a
/// unit test can only check on the cases someone thought of:
///
///   1. Anything the decoder accepts must re-encode to the same value it was
///      given, modulo case. If decode/encode is not a bijection on accepted
///      input, two nodes can read the same string as different bytes.
///   2. Every byte sequence must round-trip through encode then decode.

#include "fuzz_assert.hpp"

#include <amarian/util/hex.hpp>
#include <amarian/util/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);

    if (const auto decoded = amarian::FromHex(input)) {
        // Property 1: accepted input must re-encode to itself, lowercased.
        std::string expected(input);
        for (char& c : expected) {
            c = ToLowerAscii(c);
        }
        FUZZ_CHECK(amarian::ToHex(*decoded) == expected, "decode/encode is not a bijection");
        FUZZ_CHECK(decoded->size() * 2 == input.size(), "decoded length does not match input");

        // FromHexExact must agree with FromHex on the length it actually got,
        // and must reject every other length.
        FUZZ_CHECK(amarian::FromHexExact(input, decoded->size()).has_value(),
                   "FromHexExact rejected a length FromHex produced");
        FUZZ_CHECK(!amarian::FromHexExact(input, decoded->size() + 1).has_value(),
                   "FromHexExact accepted the wrong length");
    }

    // Property 2: every byte sequence round-trips.
    const amarian::ByteVec bytes(data, data + size);
    const auto round_tripped = amarian::FromHex(amarian::ToHex(bytes));
    FUZZ_CHECK(round_tripped.has_value(), "encoded bytes failed to decode");
    FUZZ_CHECK(*round_tripped == bytes, "round trip changed the bytes");

    // The Hash256 parsers are length-gated; feed them the same input so their
    // rejection paths are covered too.
    (void)amarian::Hash256FromHex(input);
    (void)amarian::Hash256FromHexInternal(input);

    return 0;
}
