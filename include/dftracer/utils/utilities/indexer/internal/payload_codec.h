#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_PAYLOAD_CODEC_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_PAYLOAD_CODEC_H

#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <dftracer/utils/utilities/indexer/error.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal {

inline constexpr std::size_t DECODE_CONTEXT_BUF_SIZE = 256;
inline thread_local char g_decode_context[DECODE_CONTEXT_BUF_SIZE] = {};

struct DecodeContextGuard {
    template <typename... Args>
    explicit DecodeContextGuard(const char* fmt, Args... args) {
        std::memcpy(previous_, g_decode_context, DECODE_CONTEXT_BUF_SIZE);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        std::snprintf(g_decode_context, DECODE_CONTEXT_BUF_SIZE, fmt, args...);
#pragma GCC diagnostic pop
    }

    ~DecodeContextGuard() {
        std::memcpy(g_decode_context, previous_, DECODE_CONTEXT_BUF_SIZE);
    }

    DecodeContextGuard(const DecodeContextGuard&) = delete;
    DecodeContextGuard& operator=(const DecodeContextGuard&) = delete;

   private:
    char previous_[DECODE_CONTEXT_BUF_SIZE];
};

inline void append_u8(std::string& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

inline void append_u32(std::string& out, std::uint32_t value) {
    dftracer::utils::rocksdb::KeyCodec::append_be32(out, value);
}

inline void append_u64(std::string& out, std::uint64_t value) {
    dftracer::utils::rocksdb::KeyCodec::append_be64(out, value);
}

inline void append_i64(std::string& out, std::int64_t value) {
    dftracer::utils::rocksdb::KeyCodec::append_be64(
        out, static_cast<std::uint64_t>(value));
}

inline void append_double(std::string& out, double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t));
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u64(out, bits);
}

inline void append_string(std::string& out, std::string_view value) {
    append_u32(out, static_cast<std::uint32_t>(value.size()));
    out.append(value.data(), value.size());
}

inline void append_blob(std::string& out, std::span<const unsigned char> blob) {
    append_u32(out, static_cast<std::uint32_t>(blob.size()));
    out.append(reinterpret_cast<const char*>(blob.data()), blob.size());
}

class Cursor {
   public:
    explicit Cursor(std::string_view data) : data_(data) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }

    std::uint32_t u32() {
        return dftracer::utils::rocksdb::KeyCodec::decode_be32(take(4));
    }

    std::uint64_t u64() {
        return dftracer::utils::rocksdb::KeyCodec::decode_be64(take(8));
    }

    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

    double f64() {
        std::uint64_t bits = u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::string_view str_view() {
        auto len = static_cast<std::size_t>(u32());
        return take(len);
    }

    std::string str() {
        auto bytes = str_view();
        return std::string(bytes.data(), bytes.size());
    }

    std::vector<unsigned char> blob() {
        auto len = static_cast<std::size_t>(u32());
        auto bytes = take(len);
        return std::vector<unsigned char>(bytes.begin(), bytes.end());
    }

    // Length-prefixed blob as a non-owning view into the cursor's backing
    // buffer (no copy). Valid only while that buffer outlives the view; use
    // for transient decodes that consume the bytes immediately.
    std::string_view blob_view() {
        auto len = static_cast<std::size_t>(u32());
        return take(len);
    }

    std::size_t offset() const { return offset_; }
    bool eof() const { return offset_ >= data_.size(); }

   private:
    std::string_view take(std::size_t len) {
        if (offset_ + len > data_.size()) {
            char err[DECODE_CONTEXT_BUF_SIZE + 64];
            if (g_decode_context[0] != '\0') {
                std::snprintf(err, sizeof(err), "Corrupt RocksDB payload [%s]",
                              g_decode_context);
            } else {
                std::snprintf(err, sizeof(err), "Corrupt RocksDB payload");
            }
            throw IndexerError(IndexerError::Type::DATABASE_ERROR, err);
        }
        auto chunk = data_.substr(offset_, len);
        offset_ += len;
        return chunk;
    }

    std::string_view data_;
    std::size_t offset_ = 0;
};

}  // namespace dftracer::utils::utilities::indexer::internal

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_PAYLOAD_CODEC_H
