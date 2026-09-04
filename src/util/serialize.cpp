#include <amarian/util/overflow.hpp>
#include <amarian/util/serialize.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace amarian {

void Writer::WriteCompactSize(uint64_t value) {
    if (value < 0xFDU) {
        WriteU8(static_cast<uint8_t>(value));
    } else if (value <= 0xFFFFU) {
        WriteU8(0xFDU);
        WriteU16(static_cast<uint16_t>(value));
    } else if (value <= 0xFFFFFFFFU) {
        WriteU8(0xFEU);
        WriteU32(static_cast<uint32_t>(value));
    } else {
        WriteU8(0xFFU);
        WriteU64(value);
    }
}

void Writer::WriteByteString(ByteSpan bytes) {
    WriteCompactSize(bytes.size());
    WriteBytes(bytes);
}

bool Reader::ReadCompactSize(uint64_t& out, size_t min_element_bytes) noexcept {
    // Zero would make every count plausible, which is the opposite of the point.
    // A caller passing zero has a bug, and in consensus code a bug should stop the
    // parse rather than widen a bound.
    if (min_element_bytes == 0) {
        ok_ = false;
        return false;
    }

    uint8_t tag = 0;
    if (!ReadU8(tag)) {
        return false;
    }

    uint64_t value = 0;
    if (tag < 0xFDU) {
        value = tag;
    } else if (tag == 0xFDU) {
        uint16_t wide = 0;
        if (!ReadU16(wide)) {
            return false;
        }
        // Would have fitted in the one-byte form.
        if (wide < 0xFDU) {
            ok_ = false;
            return false;
        }
        value = wide;
    } else if (tag == 0xFEU) {
        uint32_t wide = 0;
        if (!ReadU32(wide)) {
            return false;
        }
        if (wide <= 0xFFFFU) {
            ok_ = false;
            return false;
        }
        value = wide;
    } else {
        uint64_t wide = 0;
        if (!ReadU64(wide)) {
            return false;
        }
        if (wide <= 0xFFFFFFFFU) {
            ok_ = false;
            return false;
        }
        value = wide;
    }

    // The allocation bound. Remaining() is measured after the length prefix, so
    // this asks the only question worth asking: could the bytes that are actually
    // here hold that many elements? std::cmp_greater rather than a cast, because a
    // cast between size_t and uint64_t is either useless or lossy depending on the
    // platform and neither is something to write by hand.
    if (std::cmp_greater(value, Remaining() / min_element_bytes)) {
        ok_ = false;
        return false;
    }

    out = value;
    return true;
}

bool Reader::ReadByteString(ByteVec& out, size_t max_len) {
    uint64_t len = 0;
    if (!ReadCompactSize(len, 1)) {
        return false;
    }
    if (std::cmp_greater(len, max_len)) {
        ok_ = false;
        return false;
    }

    // ReadCompactSize already bounded len by the bytes remaining, so this narrowing
    // cannot fail on any platform where those bytes exist. Checked anyway: the
    // alternative is a static_cast whose safety depends on a bound two functions
    // away, and that is exactly the kind of reasoning that stops being true later.
    const auto count = TryNarrow<size_t>(len);
    if (!count.has_value()) {
        ok_ = false;
        return false;
    }

    const uint8_t* src = Consume(*count);
    if (src == nullptr) {
        return false;
    }
    out.assign(src, src + *count);
    return true;
}

}  // namespace amarian
