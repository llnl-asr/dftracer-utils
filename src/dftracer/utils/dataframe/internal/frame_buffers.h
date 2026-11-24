// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_FRAME_BUFFERS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_FRAME_BUFFERS_H

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/buffers.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::dataframe {

/// A frame as a small header and the column buffers it names, for a transport
/// that moves buffers without copying them, such as pickle protocol 5.
struct FrameBuffers {
    /// Names, types, row count, row labels and, for each column, which of
    /// `buffers` hold its validity, values, offsets and children.
    dstd::String header;
    /// Each view holds its block, so the bytes live as long as the view.
    dstd::Vector<BufferView> buffers;
};

/// `f` as a header and its buffers. A flat column hands over its own
/// buffers. A column of another encoding is made flat first, and string or
/// list offsets that do not start at 0 are rebased into a new buffer: those
/// are the only copies. Machine byte order. An InvalidArgument error for a
/// list child that cannot be gathered.
dstd::Result<FrameBuffers> frame_to_buffers(const DataFrame& f);

/// One received buffer and the owner that keeps its bytes alive.
struct ReceivedBuffer {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    /// True when no one can change the bytes while the owner lives.
    bool read_only = false;
    dstd::SharedPtr<const void> owner;
};

/// The frame that @ref frame_to_buffers wrote, reading each buffer in place: a
/// column keeps the owners of its own buffers. A buffer that is not aligned
/// for its values is copied, and so are the offsets and validity bits of a
/// buffer that is not read-only, since a later change to those could move a
/// read outside the data. nullopt for another format or version, a header
/// that does not decode, a time unit or decimal size out of range, a null
/// count that the validity bits do not give, a buffer of the wrong size or
/// offsets that leave their data. Never throws.
dstd::Optional<DataFrame> frame_from_buffers(
    dstd::StringView header, dstd::Span<const ReceivedBuffer> buffers) noexcept;

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_FRAME_BUFFERS_H
