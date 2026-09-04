#include <amarian/primitives/lock.hpp>

#include <utility>

namespace amarian {

void Lock::Serialize(Writer& writer) const {
    writer.WriteU8(version);
    writer.WriteByteString(program);
}

bool Lock::Deserialize(Reader& reader, Lock& out, size_t max_program_size) {
    Lock decoded;
    if (!reader.ReadU8(decoded.version) ||
        !reader.ReadByteString(decoded.program, max_program_size)) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

}  // namespace amarian
