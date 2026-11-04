// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_STORE_EXTENT_CODEC_H
#define DFTRACER_UTILS_STORE_EXTENT_CODEC_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/store.h>

#include <cstddef>
#include <cstdint>
#include <utility>

struct ZSTD_CDict_s;
struct ZSTD_DDict_s;

namespace dftracer::utils::store::engine {

/// A stored extent starts with this header: u32 page count, u32 raw length, u32
/// crc of the rest of the header and the page table, u32 dictionary length, u64
/// dictionary offset, u32 dictionary crc, four zero bytes. Then the page table,
/// the pages and zero padding. Segment offsets count from the raw bytes.
inline constexpr std::size_t EXTENT_HEADER_BYTES = 32;

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

struct DictRef {
    std::uint64_t offset = 0;
    std::uint32_t length = 0;
    std::uint32_t crc = 0;

    bool empty() const { return length == 0; }
};

inline constexpr std::size_t PAGE_DICT_BYTES = 112 * 1024;

/// A zstd dictionary for the pages of one namespace in one data file, ready to
/// compress or to decompress with. Safe to share between threads.
class PageDict {
   public:
    /// A dictionary to compress with, whose `bytes` lie at `ref` in the file.
    /// A Parse error when zstd rejects them.
    static dstd::Expected<dstd::SharedPtr<const PageDict>, dstd::Error>
    for_encode(dstd::ByteView bytes, const DictRef& ref);
    /// A dictionary to decompress with, from the `bytes` read at `ref`. A Parse
    /// error when their length or crc disagrees with `ref` or zstd rejects
    /// them.
    static dstd::Expected<dstd::SharedPtr<const PageDict>, dstd::Error>
    for_decode(dstd::ByteView bytes, const DictRef& ref);

    struct Key {
        explicit Key() = default;
    };

    PageDict(Key, const DictRef& ref, ZSTD_CDict_s* cdict, ZSTD_DDict_s* ddict)
        : ref_(ref), cdict_(cdict), ddict_(ddict) {}
    PageDict(const PageDict&) = delete;
    PageDict& operator=(const PageDict&) = delete;
    ~PageDict();

    const DictRef& ref() const { return ref_; }
    const ZSTD_CDict_s* cdict() const { return cdict_; }
    const ZSTD_DDict_s* ddict() const { return ddict_; }

   private:
    DictRef ref_;
    ZSTD_CDict_s* cdict_ = nullptr;
    ZSTD_DDict_s* ddict_ = nullptr;
};

/// The bytes of a dictionary of at most PAGE_DICT_BYTES trained on `samples`,
/// the raw pages laid end to end with `sizes` their lengths. A Parse error
/// when zstd cannot train one from them.
dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> train_page_dict(
    dstd::ByteView samples, dstd::Span<const std::size_t> sizes);

struct PageEntry {
    std::uint32_t raw_offset = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t stored_offset = 0;
    std::uint32_t stored_size = 0;
    std::uint32_t crc = 0;
    PageCodec codec = PageCodec::None;
};

struct SegmentSpan {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    /// The span takes pages of its own, which no other span shares: the head
    /// of a segment over several pages, so a point read decodes the head
    /// without the granules after it.
    bool alone = false;
};

/// The ascending raw offsets the pages of an extent holding `segments` start
/// at, over its first `raw_size` bytes, the first of them 0. A segment of at
/// most PAGE_BYTES never spans two pages, every page of a larger segment
/// starts inside it, and a span marked alone starts a page and ends one.
dstd::Vector<std::uint32_t> page_starts(dstd::Span<const SegmentSpan> segments,
                                        std::uint32_t raw_size);

/// The stored form of `raw` cut into pages at `starts`, padded with zeros to a
/// multiple of `align`. A Zstd page is compressed (with `dict` if given) when
/// that saves an eighth, else raw. InvalidArgument for empty or oversized
/// `raw`, a zero `align`, or starts that do not cut `raw` into pages of 1 to
/// PAGE_BYTES.
dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_extent(
    dstd::ByteView raw, dstd::Span<const std::uint32_t> starts,
    std::size_t align, PageCodec codec = PageCodec::Zstd,
    const PageDict* dict = nullptr);

/// The zstd level of the metadata a data file writes once, at close.
inline constexpr int META_ZSTD_LEVEL = 3;
/// The zstd level of a record body, written on every commit.
inline constexpr int RECORD_ZSTD_LEVEL = 1;
/// The zstd level of a big value, written outside the extents.
inline constexpr int BIG_ZSTD_LEVEL = 1;

/// The stored form of one metadata body (a block, a record body, a summary
/// entry): a PageCodec byte, then for PageCodec::Zstd the raw length as a u32
/// and one zstd frame, else the raw bytes. Compressed when that saves at least
/// an eighth, at zstd `level`.
dstd::Vector<std::uint8_t> encode_meta(dstd::ByteView raw, int level);

/// The PageCodec::Zstd stored form of `raw` in `out`, as encode_meta writes
/// it, when that saves at least an eighth. False, with `out` left empty, when
/// it does not, so the caller stores the PageCodec::None form itself.
bool compress_meta(dstd::ByteView raw, int level,
                   dstd::Vector<std::uint8_t>& out);

/// The raw body of a stored form encode_meta wrote. A Parse error for an
/// unknown codec, a raw length past EXTENT_MAX_RAW_BYTES or a frame that does
/// not decode to that length.
dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> decode_meta(
    dstd::ByteView stored);

/// The pages of one stored extent, read in place over bytes that outlive it,
/// or its page table alone, whose pages are read one range at a time.
class ExtentPages {
   public:
    /// The bytes of the header and the page table of the extent whose stored
    /// bytes start with `header`, or 0 when `header` is shorter than
    /// EXTENT_HEADER_BYTES or names no page.
    static std::size_t table_size(dstd::ByteView header);

    /// Opens the extent over `stored`, checking the header and every page table
    /// entry. A Parse error when `stored` cannot hold what the header claims,
    /// the pages leave gaps or leave `stored`, or a reserved byte is set.
    static dstd::Expected<ExtentPages, dstd::Error> open(dstd::ByteView stored);
    /// Opens the page table alone. `head` holds the header and the whole table,
    /// and every page must lie inside `stored_size`. The same Parse errors as
    /// open, and Parse when the header crc fails. decode() fails on the result,
    /// so read pages with decode_page.
    static dstd::Expected<ExtentPages, dstd::Error> open_table(
        dstd::ByteView head, std::uint64_t stored_size);
    /// The raw bytes of `page` from its `stored` bytes into `out` (room for its
    /// raw length), checked against the page crc. `dict` is the dictionary the
    /// extent names, needed by a ZstdDict page. A Parse error when the length,
    /// crc, codec or decoded length disagrees, or a ZstdDict page has no
    /// matching `dict`.
    static dstd::Expected<void, dstd::Error> decode_page(
        const PageEntry& page, dstd::ByteView stored, std::uint8_t* out,
        const PageDict* dict = nullptr);

    std::size_t size() const { return entries_.size(); }
    std::uint32_t raw_size() const { return raw_size_; }
    const DictRef& dict() const { return dict_; }

    const PageEntry& entry(std::size_t i) const { return entries_[i]; }

    /// The first page and the one past the last whose raw range meets
    /// [raw_offset, raw_offset + length). Both equal for an empty range.
    dstd::Pair<std::size_t, std::size_t> covering(std::uint32_t raw_offset,
                                                  std::uint32_t length) const;

    /// The raw bytes of page `i` in `out`, checked against the page crc, with
    /// `dict` as decode_page takes it. A Parse error when the crc, the codec or
    /// the decoded length disagrees.
    dstd::Expected<void, dstd::Error> decode(
        std::size_t i, dstd::Vector<std::uint8_t>& out,
        const PageDict* dict = nullptr) const;

   private:
    static dstd::Expected<ExtentPages, dstd::Error> parse(
        dstd::ByteView head, std::uint64_t stored_size);

    ExtentPages(dstd::ByteView stored, dstd::Vector<PageEntry> entries,
                std::uint32_t raw_size, const DictRef& dict)
        : stored_(stored),
          entries_(std::move(entries)),
          dict_(dict),
          raw_size_(raw_size) {}

    dstd::ByteView stored_;
    dstd::Vector<PageEntry> entries_;
    DictRef dict_;
    std::uint32_t raw_size_ = 0;
};

/// The raw bytes of a stored extent. A Parse error when its header, its page
/// table or any page does not decode. `stored` is reused as the result. `dict`
/// is the dictionary the extent names, if any.
dstd::Expected<void, dstd::Error> decode_extent(
    dstd::Vector<std::uint8_t>& stored, const PageDict* dict = nullptr);

}  // namespace dftracer::utils::store::engine

#endif  // DFTRACER_UTILS_STORE_EXTENT_CODEC_H
