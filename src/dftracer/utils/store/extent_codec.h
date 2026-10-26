#ifndef DFTRACER_UTILS_STORE_EXTENT_CODEC_H
#define DFTRACER_UTILS_STORE_EXTENT_CODEC_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::store {

/// A stored extent starts with this header: u32 page count, u32 raw length, and
/// eight zero bytes. The page table follows, then the stored pages, then zero
/// padding. Segment offsets count from the start of the raw bytes.
inline constexpr std::size_t EXTENT_HEADER_BYTES = 16;

/// One page table entry: u32 raw offset, u32 raw length, u32 stored offset from
/// the start of the extent, u32 stored length, u32 crc of the stored page
/// bytes, u8 codec and three zero bytes.
inline constexpr std::size_t EXTENT_TABLE_ENTRY_BYTES = 24;

/// The most raw bytes one page holds, and so the most a point read decodes to
/// reach the segment it wants.
inline constexpr std::size_t PAGE_BYTES = 32 * 1024;

/// The most raw bytes one stored extent may claim, so an extent that passes its
/// crc but was written wrong cannot ask for an unbounded buffer.
inline constexpr std::uint32_t EXTENT_MAX_RAW_BYTES = 1U << 30U;

enum class PageCodec : std::uint8_t { None = 0, Zstd = 1 };

/// One entry of the page table of a stored extent.
struct PageEntry {
    std::uint32_t raw_offset = 0;
    std::uint32_t raw_length = 0;
    std::uint32_t stored_offset = 0;
    std::uint32_t stored_length = 0;
    std::uint32_t crc = 0;
    PageCodec codec = PageCodec::None;
};

/// One segment's bytes inside an extent holding several.
struct SegmentSpan {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

/// The ascending raw offsets the pages of an extent holding `segments` start
/// at, over its first `raw_length` bytes, the first of them 0. A segment of at
/// most PAGE_BYTES never spans two pages, and every page of a larger segment
/// starts inside it.
dstd::Vector<std::uint32_t> page_starts(dstd::Span<const SegmentSpan> segments,
                                        std::uint32_t raw_length);

/// The stored form of `raw` cut into pages at `starts`: the header, the page
/// table, then each page compressed with zstd when that saves at least an
/// eighth of it and the page itself otherwise, padded with zeros to a multiple
/// of `align`. An InvalidArgument error for empty `raw`, `raw` past
/// EXTENT_MAX_RAW_BYTES, a zero `align`, or starts that do not cut `raw` into
/// pages of 1 to PAGE_BYTES bytes from offset 0.
dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_extent(
    ByteView raw, dstd::Span<const std::uint32_t> starts, std::size_t align);

/// The pages of one stored extent, read in place over bytes that outlive it.
class ExtentPages {
   public:
    /// Opens the extent over `stored`, checking the header and every entry of
    /// its page table, and freeing nothing. A Parse error when `stored` cannot
    /// hold what the header claims, when the pages do not cover [0, raw_length)
    /// without gaps, when an entry leaves `stored`, or when a reserved byte is
    /// set.
    static dstd::Expected<ExtentPages, dstd::Error> open(ByteView stored);

    std::size_t size() const { return entries_.size(); }
    std::uint32_t raw_length() const { return raw_length_; }

    /// The entry of page `i`, for `i` below size().
    const PageEntry& entry(std::size_t i) const { return entries_[i]; }

    /// The first page and the one past the last whose raw range meets
    /// [raw_offset, raw_offset + length); both equal for an empty range.
    std::pair<std::size_t, std::size_t> covering(std::uint32_t raw_offset,
                                                 std::uint32_t length) const;

    /// The raw bytes of page `i` in `out`, checked against the page crc. A
    /// Parse error when the crc, the codec or the decoded length disagrees.
    dstd::Expected<void, dstd::Error> decode(
        std::size_t i, dstd::Vector<std::uint8_t>& out) const;

   private:
    ExtentPages(ByteView stored, dstd::Vector<PageEntry> entries,
                std::uint32_t raw_length)
        : stored_(stored),
          entries_(std::move(entries)),
          raw_length_(raw_length) {}

    ByteView stored_;
    dstd::Vector<PageEntry> entries_;
    std::uint32_t raw_length_ = 0;
};

/// The raw bytes of a stored extent; a Parse error when its header, its page
/// table or any page does not decode. `stored` is reused as the result.
dstd::Expected<void, dstd::Error> decode_extent(
    dstd::Vector<std::uint8_t>& stored);

}  // namespace dftracer::utils::store

#endif  // DFTRACER_UTILS_STORE_EXTENT_CODEC_H
