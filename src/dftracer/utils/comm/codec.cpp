// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/codec.h>

#include <cstring>

namespace dftracer::utils::comm {

std::size_t StringCodec::serialized_size(
    const dstd::String& value) const noexcept {
    return value.size();
}

dstd::Result<std::size_t> StringCodec::serialize(
    const dstd::String& value, dstd::Span<std::byte> out) const {
    if (out.size() != value.size()) {
        return dstd::Unexpected(
            dstd::Error::from_format<"{} bytes do not hold {} bytes">(
                dstd::Condition::InvalidArgument, out.size(), value.size()));
    }
    if (!value.empty()) std::memcpy(out.data(), value.data(), value.size());
    return value.size();
}

dstd::Result<dstd::String> StringCodec::deserialize(
    dstd::Span<const std::byte> bytes) const {
    dstd::String out;
    if (!bytes.empty())
        out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return out;
}

}  // namespace dftracer::utils::comm
