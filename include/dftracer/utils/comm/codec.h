// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_CODEC_H
#define DFTRACER_UTILS_COMM_CODEC_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <concepts>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace dftracer::utils::comm {

/**
 * @brief How a value of @p T that is not trivially copyable crosses ranks, for
 * the `_object` forms of @ref dftracer::utils::comm::Group.
 *
 * `serialized_size(value)` is the exact number of bytes `serialize` writes.
 * `serialize(value, out)` fills all of @p out, which the caller sized to
 * `serialized_size`, and never allocates or grows it. It gives the number of
 * bytes it wrote, which must be `serialized_size`: another number is an error
 * of the call. It gives an error when @p out does not fit the value.
 * `deserialize(bytes)` reads one value from exactly the bytes one `serialize`
 * wrote, into an owned @p T, and gives an error for bytes that are not such a
 * value: they come from another rank and are never trusted. It must be
 * deterministic: the same bytes decode the same way, or fail the same way, on
 * every process, since the rank that serialized a value reads the bytes back
 * once to know that the others can.
 */
template <class C, class T>
concept Codec =
    requires(const C& codec, const T& value, dstd::Span<std::byte> out,
             dstd::Span<const std::byte> bytes) {
        { codec.serialized_size(value) } -> std::same_as<std::size_t>;
        {
            codec.serialize(value, out)
        } -> std::same_as<dstd::Result<std::size_t>>;
        { codec.deserialize(bytes) } -> std::same_as<dstd::Result<T>>;
    };

/// The codec of a @ref dstd::String, its bytes, with no length head, since the
/// block of a rank already has one.
struct StringCodec {
    std::size_t serialized_size(dstd::StringView value) const noexcept;
    dstd::Result<std::size_t> serialize(dstd::StringView value,
                                        dstd::Span<std::byte> out) const;
    dstd::Result<dstd::String> deserialize(
        dstd::Span<const std::byte> bytes) const;
};

/// The codec of a @ref dstd::Vector of trivially copyable @p T: the raw values
/// back to back. The ranks must agree on the layout of @p T.
template <class T>
    requires std::is_trivially_copyable_v<T>
struct VectorCodec {
    std::size_t serialized_size(dstd::Span<const T> value) const noexcept {
        return value.size() * sizeof(T);
    }

    dstd::Result<std::size_t> serialize(dstd::Span<const T> value,
                                        dstd::Span<std::byte> out) const {
        const std::size_t bytes = serialized_size(value);
        if (out.size() != bytes) {
            return dstd::Unexpected(
                dstd::Error::from_format<"{} bytes do not hold {} bytes">(
                    dstd::Condition::InvalidArgument, out.size(), bytes));
        }
        if (bytes != 0) std::memcpy(out.data(), value.data(), bytes);
        return bytes;
    }

    dstd::Result<dstd::Vector<T>> deserialize(
        dstd::Span<const std::byte> bytes) const {
        if (bytes.size() % sizeof(T) != 0) {
            return dstd::Unexpected(
                dstd::Error::from_format<
                    "{} bytes are not a whole number of {}-byte "
                    "values">(dstd::Condition::InvalidArgument, bytes.size(),
                              sizeof(T)));
        }
        dstd::Vector<T> out(bytes.size() / sizeof(T));
        if (!bytes.empty()) std::memcpy(out.data(), bytes.data(), bytes.size());
        return out;
    }
};

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_CODEC_H
