// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_STORE_INTERNAL_PAYLOAD_CODEC_H
#define DFTRACER_UTILS_INDEX_STORE_INTERNAL_PAYLOAD_CODEC_H

#include <dftracer/utils/dstd/big_endian.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/error.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace dftracer::utils::index::store::internal {

inline constexpr std::size_t DECODE_CONTEXT_BUF_SIZE = 256;
inline thread_local char t_decode_context[DECODE_CONTEXT_BUF_SIZE] = {};

struct DecodeContextGuard {
    template <typename... Args>
    explicit DecodeContextGuard(const char* fmt, Args... args) {
        std::memcpy(previous_, t_decode_context, DECODE_CONTEXT_BUF_SIZE);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        std::snprintf(t_decode_context, DECODE_CONTEXT_BUF_SIZE, fmt, args...);
#pragma GCC diagnostic pop
    }

    ~DecodeContextGuard() {
        std::memcpy(t_decode_context, previous_, DECODE_CONTEXT_BUF_SIZE);
    }

    DecodeContextGuard(const DecodeContextGuard&) = delete;
    DecodeContextGuard& operator=(const DecodeContextGuard&) = delete;

   private:
    char previous_[DECODE_CONTEXT_BUF_SIZE];
};

inline void append_u8(dstd::String& out, std::uint8_t value) {
    out.push_back(static_cast<char>(value));
}

inline void append_string(dstd::String& out, dstd::StringView value) {
    dstd::append_u32_be(out, static_cast<std::uint32_t>(value.size()));
    out.append(value.data(), value.size());
}

inline void append_blob(dstd::String& out,
                        dstd::Span<const unsigned char> blob) {
    dstd::append_u32_be(out, static_cast<std::uint32_t>(blob.size()));
    out.append(reinterpret_cast<const char*>(blob.data()), blob.size());
}

/// Reads big-endian fields from a payload. A read past the end sets failed()
/// and returns zeros and empty views from then on, so a decoder checks failed()
/// once after its last read.
class Cursor {
   public:
    explicit Cursor(dstd::StringView data) : data_(data) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }

    std::uint32_t u32() {
        return dstd::read_u32_be(
            reinterpret_cast<const std::uint8_t*>(take(4).data()));
    }

    std::uint64_t u64() {
        return dstd::read_u64_be(
            reinterpret_cast<const std::uint8_t*>(take(8).data()));
    }

    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

    double f64() { return dstd::bits_double(u64()); }

    dstd::StringView str_view() {
        auto len = static_cast<std::size_t>(u32());
        return take(len);
    }

    dstd::String str() {
        auto bytes = str_view();
        return dstd::String(bytes.data(), bytes.size());
    }

    dstd::Vector<unsigned char> blob() {
        auto len = static_cast<std::size_t>(u32());
        auto bytes = take(len);
        return dstd::Vector<unsigned char>(bytes.begin(), bytes.end());
    }

    // Length-prefixed blob as a non-owning view into the cursor's backing
    // buffer (no copy). Valid only while that buffer outlives the view; use
    // for transient decodes that consume the bytes immediately.
    dstd::StringView blob_view() {
        auto len = static_cast<std::size_t>(u32());
        return take(len);
    }

    std::size_t offset() const { return offset_; }
    bool eof() const { return offset_ >= data_.size(); }
    bool failed() const { return failed_; }

    /// The error of a failed cursor, naming the decode context.
    dstd::Error error() const {
        if (t_decode_context[0] != '\0')
            return indexer_error(
                IndexerErrorKind::DatabaseError,
                dstd::fmt<"Corrupt index payload [{}]">(t_decode_context),
                common::ErrorCode::Parse);
        return indexer_error(IndexerErrorKind::DatabaseError,
                             "Corrupt index payload", common::ErrorCode::Parse);
    }

   private:
    dstd::StringView take(std::size_t len) {
        static constexpr char ZEROS[8] = {};
        if (failed_ || offset_ + len > data_.size()) {
            failed_ = true;
            return dstd::StringView(ZEROS, len <= sizeof(ZEROS) ? len : 0);
        }
        auto chunk = data_.substr(offset_, len);
        offset_ += len;
        return chunk;
    }

    dstd::StringView data_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

}  // namespace dftracer::utils::index::store::internal

#endif  // DFTRACER_UTILS_INDEX_STORE_INTERNAL_PAYLOAD_CODEC_H
