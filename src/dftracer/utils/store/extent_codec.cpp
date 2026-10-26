#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/store/crc.h>
#include <dftracer/utils/store/extent_codec.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace dftracer::utils::store {

namespace {

/// Level 1: extents are written on the appending thread, so speed comes first.
constexpr int PAGE_ZSTD_LEVEL = 1;

dstd::Unexpected<dstd::Error> parse_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

struct CompressContext {
    ZSTD_CCtx* ctx = ZSTD_createCCtx();
    dstd::Vector<std::uint8_t> scratch;
    ~CompressContext() { ZSTD_freeCCtx(ctx); }
};

struct DecompressContext {
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    ~DecompressContext() { ZSTD_freeDCtx(ctx); }
};

std::size_t align_up(std::size_t value, std::size_t align) {
    return (value + align - 1) / align * align;
}

const std::uint8_t* bytes_of(ByteView view) {
    return reinterpret_cast<const std::uint8_t*>(view.data());
}

bool cuts_pages(dstd::Span<const std::uint32_t> starts,
                std::size_t raw_length) {
    if (starts.empty() || starts.front() != 0) return false;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const std::size_t begin = starts[i];
        const std::size_t end =
            i + 1 < starts.size() ? starts[i + 1] : raw_length;
        if (end <= begin || end - begin > PAGE_BYTES) return false;
    }
    return true;
}

}  // namespace

dstd::Vector<std::uint32_t> page_starts(dstd::Span<const SegmentSpan> segments,
                                        std::uint32_t raw_length) {
    dstd::Vector<std::uint32_t> starts;
    starts.push_back(0);
    std::uint32_t page = 0;
    for (const SegmentSpan& segment : segments) {
        if (segment.length == 0) continue;
        const std::uint32_t end = segment.offset + segment.length;
        if (end - page <= PAGE_BYTES) continue;
        if (segment.offset > page) {
            page = segment.offset;
            starts.push_back(page);
        }
        while (end - page > PAGE_BYTES) {
            page += static_cast<std::uint32_t>(PAGE_BYTES);
            starts.push_back(page);
        }
    }
    while (raw_length > page && raw_length - page > PAGE_BYTES) {
        page += static_cast<std::uint32_t>(PAGE_BYTES);
        starts.push_back(page);
    }
    return starts;
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_extent(
    ByteView raw, dstd::Span<const std::uint32_t> starts, std::size_t align,
    PageCodec codec_wanted) {
    const std::size_t raw_length = raw.size();
    if (raw_length == 0 || raw_length > EXTENT_MAX_RAW_BYTES || align == 0 ||
        (codec_wanted != PageCodec::None && codec_wanted != PageCodec::Zstd) ||
        !cuts_pages(starts, raw_length)) {
        return invalid_error();
    }
    const std::uint8_t* data = bytes_of(raw);
    const std::size_t count = starts.size();
    const std::size_t table =
        EXTENT_HEADER_BYTES + count * EXTENT_TABLE_ENTRY_BYTES;
    dstd::Vector<std::uint8_t> out(table + raw_length, std::uint8_t{0});
    thread_local CompressContext context;
    std::size_t stored = table;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t begin = starts[i];
        const std::uint32_t end = i + 1 < count
                                      ? starts[i + 1]
                                      : static_cast<std::uint32_t>(raw_length);
        const std::uint32_t length = end - begin;
        PageCodec codec = PageCodec::None;
        std::uint32_t page_bytes = length;
        if (codec_wanted == PageCodec::Zstd && context.ctx != nullptr) {
            const std::size_t bound = ZSTD_compressBound(length);
            if (context.scratch.size() < bound) context.scratch.resize(bound);
            const std::size_t compressed = ZSTD_compressCCtx(
                context.ctx, context.scratch.data(), context.scratch.size(),
                data + begin, length, PAGE_ZSTD_LEVEL);
            if (ZSTD_isError(compressed) == 0 &&
                compressed <= length - length / 8) {
                codec = PageCodec::Zstd;
                page_bytes = static_cast<std::uint32_t>(compressed);
                std::memcpy(out.data() + stored, context.scratch.data(),
                            compressed);
            }
        }
        if (codec == PageCodec::None) {
            std::memcpy(out.data() + stored, data + begin, length);
        }
        std::uint8_t* entry =
            out.data() + EXTENT_HEADER_BYTES + i * EXTENT_TABLE_ENTRY_BYTES;
        write_u32_le(entry, begin);
        write_u32_le(entry + 4, length);
        write_u32_le(entry + 8, static_cast<std::uint32_t>(stored));
        write_u32_le(entry + 12, page_bytes);
        write_u32_le(entry + 16, crc32(0, out.data() + stored, page_bytes));
        entry[20] = static_cast<std::uint8_t>(codec);
        stored += page_bytes;
    }
    out.resize(stored);
    out.resize(align_up(out.size(), align));
    write_u32_le(out.data(), static_cast<std::uint32_t>(count));
    write_u32_le(out.data() + 4, static_cast<std::uint32_t>(raw_length));
    return out;
}

dstd::Expected<ExtentPages, dstd::Error> ExtentPages::open(ByteView stored) {
    if (stored.size() < EXTENT_HEADER_BYTES) return parse_error();
    const std::uint8_t* header = bytes_of(stored);
    const std::uint32_t count = read_u32_le(header);
    const std::uint32_t raw_length = read_u32_le(header + 4);
    if (read_u32_le(header + 8) != 0 || read_u32_le(header + 12) != 0 ||
        count == 0 || raw_length == 0 || raw_length > EXTENT_MAX_RAW_BYTES) {
        return parse_error();
    }
    if (count >
        (stored.size() - EXTENT_HEADER_BYTES) / EXTENT_TABLE_ENTRY_BYTES) {
        return parse_error();
    }
    const std::size_t table_end =
        EXTENT_HEADER_BYTES + count * EXTENT_TABLE_ENTRY_BYTES;
    dstd::Vector<PageEntry> entries;
    entries.reserve(count);
    std::uint64_t next_raw = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* entry =
            header + EXTENT_HEADER_BYTES + i * EXTENT_TABLE_ENTRY_BYTES;
        PageEntry page;
        page.raw_offset = read_u32_le(entry);
        page.raw_length = read_u32_le(entry + 4);
        page.stored_offset = read_u32_le(entry + 8);
        page.stored_length = read_u32_le(entry + 12);
        page.crc = read_u32_le(entry + 16);
        page.codec = static_cast<PageCodec>(entry[20]);
        if (entry[21] != 0 || entry[22] != 0 || entry[23] != 0 ||
            (page.codec != PageCodec::None && page.codec != PageCodec::Zstd)) {
            return parse_error();
        }
        if (page.raw_offset != next_raw || page.raw_length == 0 ||
            page.raw_length > PAGE_BYTES ||
            static_cast<std::uint64_t>(page.raw_offset) + page.raw_length >
                raw_length) {
            return parse_error();
        }
        if (page.stored_offset < table_end ||
            page.stored_offset > stored.size() ||
            page.stored_length > stored.size() - page.stored_offset) {
            return parse_error();
        }
        next_raw += page.raw_length;
        entries.push_back(page);
    }
    if (next_raw != raw_length) return parse_error();
    return ExtentPages(stored, std::move(entries), raw_length);
}

std::pair<std::size_t, std::size_t> ExtentPages::covering(
    std::uint32_t raw_offset, std::uint32_t length) const {
    const std::uint64_t begin = raw_offset;
    const std::uint64_t end = begin + length;
    const auto first = static_cast<std::size_t>(
        std::partition_point(entries_.begin(), entries_.end(),
                             [begin](const PageEntry& page) {
                                 return static_cast<std::uint64_t>(
                                            page.raw_offset) +
                                            page.raw_length <=
                                        begin;
                             }) -
        entries_.begin());
    if (length == 0) return {first, first};
    const auto last = static_cast<std::size_t>(
        std::partition_point(
            entries_.begin(), entries_.end(),
            [end](const PageEntry& page) { return page.raw_offset < end; }) -
        entries_.begin());
    return {first, last};
}

dstd::Expected<void, dstd::Error> ExtentPages::decode(
    std::size_t i, dstd::Vector<std::uint8_t>& out) const {
    if (i >= entries_.size()) return parse_error();
    const PageEntry& page = entries_[i];
    const std::uint8_t* src = bytes_of(stored_) + page.stored_offset;
    if (crc32(0, src, page.stored_length) != page.crc) return parse_error();
    out.resize(page.raw_length);
    switch (page.codec) {
        case PageCodec::None: {
            if (page.stored_length != page.raw_length) return parse_error();
            std::memcpy(out.data(), src, page.raw_length);
            return {};
        }
        case PageCodec::Zstd: {
            thread_local DecompressContext context;
            if (context.ctx == nullptr) return parse_error();
            const std::size_t got = ZSTD_decompressDCtx(
                context.ctx, out.data(), out.size(), src, page.stored_length);
            if (ZSTD_isError(got) != 0 || got != page.raw_length) {
                return parse_error();
            }
            return {};
        }
    }
    return parse_error();
}

dstd::Expected<void, dstd::Error> decode_extent(
    dstd::Vector<std::uint8_t>& stored) {
    auto pages = ExtentPages::open(ByteView(stored.data(), stored.size()));
    if (!pages) return parse_error();
    dstd::Vector<std::uint8_t> raw(pages->raw_length(), std::uint8_t{0});
    dstd::Vector<std::uint8_t> page;
    for (std::size_t i = 0; i < pages->size(); ++i) {
        auto decoded = pages->decode(i, page);
        if (!decoded) return decoded;
        std::memcpy(raw.data() + pages->entry(i).raw_offset, page.data(),
                    page.size());
    }
    stored = std::move(raw);
    return {};
}

}  // namespace dftracer::utils::store
