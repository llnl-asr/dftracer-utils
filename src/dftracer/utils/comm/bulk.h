// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_COMM_BULK_H
#define DFTRACER_UTILS_COMM_BULK_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/optional.h>

#include <cstdint>
#include <type_traits>

namespace dftracer::utils::comm {

/// What a peer may do with exposed memory.
enum class Access : std::uint8_t {
    /// Peers pull from it; a push is refused.
    ReadOnly,
    /// Peers push into it; a pull is refused.
    WriteOnly,
    ReadWrite,
};

/// Where exposed memory lives. Only @ref Host works; the others are reserved
/// for device memory, and a call that names one fails with
/// @ref dstd::Condition::Unsupported.
enum class MemoryKind : std::uint8_t {
    Host,
    Cuda,
    Hip,
    Sycl,
};

/**
 * @brief The descriptor of memory a rank exposed, which a peer uses to pull
 * from it or push into it.
 *
 * A small value with no pointers: the owner sends it to a peer in a message,
 * and the peer passes it to @ref Group::pull or @ref Group::push of the group
 * it was made in. @ref size is 64 bits, so a buffer is not limited to 4 GiB.
 * The descriptor does not keep the memory alive: see @ref Group::expose.
 */
struct BufferRef {
    /// The group the owner exposed the memory in; a peer uses the ref with
    /// that group only.
    std::uint64_t group = 0;
    /// Names the exposure among those of its owner.
    std::uint64_t id = 0;
    /// The bytes exposed.
    std::uint64_t size = 0;
    /// The rank of the owner in @ref group.
    std::uint32_t owner = 0;
    Access access = Access::ReadWrite;
    MemoryKind kind = MemoryKind::Host;
    std::uint16_t reserved = 0;
};

static_assert(std::is_trivially_copyable_v<BufferRef>,
              "a ref travels between ranks as bytes");
static_assert(sizeof(BufferRef) == 32, "the wire ref has a fixed size");

/// The error of a call that names a memory kind other than host.
dstd::Error unsupported_kind_error(MemoryKind kind);

/// The error of a pull or push that @p ref refuses before any byte moves, or
/// none: a kind other than host, an access mode that does not allow the call
/// (@p writes tells which), or a range that does not fit. A pull's range is
/// from @p offset to the end of the exposure, which must fit in
/// @p buffer_size; a push's is @p buffer_size bytes from @p offset.
dstd::Optional<dstd::Error> check_transfer(const BufferRef& ref, bool writes,
                                           std::uint64_t offset,
                                           std::uint64_t buffer_size);

}  // namespace dftracer::utils::comm

#endif  // DFTRACER_UTILS_COMM_BULK_H
