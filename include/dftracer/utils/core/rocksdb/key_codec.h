#ifndef DFTRACER_UTILS_CORE_ROCKSDB_KEY_CODEC_H
#define DFTRACER_UTILS_CORE_ROCKSDB_KEY_CODEC_H

#include <cstdint>
#include <string>
#include <string_view>

namespace dftracer::utils::rocksdb {

class KeyCodec {
   public:
    static std::string encode_be32(std::uint32_t value);
    static std::string encode_be64(std::uint64_t value);

    static std::uint32_t decode_be32(std::string_view bytes);
    static std::uint64_t decode_be64(std::string_view bytes);

    static void append_be32(std::string& out, std::uint32_t value);
    static void append_be64(std::string& out, std::uint64_t value);
};

class KeyBuilder {
   public:
    KeyBuilder& append_tag(std::string_view tag);
    KeyBuilder& append_separator();
    KeyBuilder& append_string(std::string_view value);
    KeyBuilder& append_be32(std::uint32_t value);
    KeyBuilder& append_be64(std::uint64_t value);

    std::string build() const;
    void clear();

   private:
    std::string key_;
};

}  // namespace dftracer::utils::rocksdb

#endif  // DFTRACER_UTILS_CORE_ROCKSDB_KEY_CODEC_H
