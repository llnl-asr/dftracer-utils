// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_CODEC_H
#define DFTRACER_UTILS_COMM_CODEC_H

#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <concepts>
#include <cstddef>

namespace dftracer::utils::comm {

/**
 * @brief How a value of @p T that is not trivially copyable crosses ranks, for
 * the `_object` forms of @ref Group.
 *
 * `serialized_size(value)` is the exact number of bytes `serialize` writes.
 * `serialize(value, out)` fills all of @p out, which the caller sized to
 * `serialized_size`, and never allocates or grows it. It gives the number of
 * bytes it wrote, which must be `serialized_size`: another number is an error
 * of the call. It gives an error when @p out does not fit the value.
 * `deserialize(bytes)` reads one value from exactly the bytes one `serialize`
 * wrote, into an owned @p T, and gives an error for bytes that are not such a
 * value: they come from another rank and are never trusted.
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

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_CODEC_H
