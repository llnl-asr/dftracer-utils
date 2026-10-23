#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/index/store/key_codec.h>

#include <stdexcept>

namespace dftracer::utils::index::store {

namespace {

template <typename T>
T decode_big_endian(std::string_view bytes) {
    if (bytes.size() != sizeof(T)) {
        throw DFTUtilsException(ErrorCode::Parse,
                                "KeyCodec: invalid big-endian integer width");
    }
    return read_be<T>(reinterpret_cast<const std::uint8_t*>(bytes.data()));
}

}  // namespace

std::string KeyCodec::encode_be32(std::uint32_t value) {
    std::string out;
    out.reserve(sizeof(value));
    append_be32(out, value);
    return out;
}

std::string KeyCodec::encode_be64(std::uint64_t value) {
    std::string out;
    out.reserve(sizeof(value));
    append_be64(out, value);
    return out;
}

std::uint32_t KeyCodec::decode_be32(std::string_view bytes) {
    return decode_big_endian<std::uint32_t>(bytes);
}

std::uint64_t KeyCodec::decode_be64(std::string_view bytes) {
    return decode_big_endian<std::uint64_t>(bytes);
}

void KeyCodec::append_be32(std::string& out, std::uint32_t value) {
    append_u32_be(out, value);
}

void KeyCodec::append_be64(std::string& out, std::uint64_t value) {
    append_u64_be(out, value);
}

KeyBuilder& KeyBuilder::append_tag(std::string_view tag) {
    key_.append(tag);
    return *this;
}

KeyBuilder& KeyBuilder::append_separator() {
    key_.push_back('\0');
    return *this;
}

KeyBuilder& KeyBuilder::append_string(std::string_view value) {
    key_.append(value);
    return *this;
}

KeyBuilder& KeyBuilder::append_be32(std::uint32_t value) {
    KeyCodec::append_be32(key_, value);
    return *this;
}

KeyBuilder& KeyBuilder::append_be64(std::uint64_t value) {
    KeyCodec::append_be64(key_, value);
    return *this;
}

std::string KeyBuilder::build() const { return key_; }

void KeyBuilder::clear() { key_.clear(); }

}  // namespace dftracer::utils::index::store
