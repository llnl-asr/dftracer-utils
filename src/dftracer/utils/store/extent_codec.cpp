// The _advanced create functions take the tracked allocator.
#define ZSTD_STATIC_LINKING_ONLY
#include <dftracer/utils/dstd/bits.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/store/crc.h>
#include <dftracer/utils/store/extent_codec.h>
#include <zdict.h>
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace dftracer::utils::store::engine {

namespace {

/// Level 1: extents are written on the appending thread, so speed comes first.
constexpr int PAGE_ZSTD_LEVEL = 1;
constexpr std::size_t META_HEADER_BYTES = 5;

dstd::Unexpected<dstd::Error> parse_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

// Thread-local contexts and shared dictionaries outlive any query pool, so
// every block they take, including the workspace a context sizes on first use,
// is charged to the general pool.
ZSTD_customMem general_pool_mem() noexcept {
    return {[](void* pool, std::size_t size) -> void* {
                try {
                    return memory::allocate(size, alignof(std::max_align_t),
                                            *static_cast<memory::Pool*>(pool));
                } catch (...) {
                    return nullptr;
                }
            },
            [](void*, void* p) { memory::c_free(p); },
            &memory::Manager::get().general()};
}

struct CompressContext {
    ZSTD_CCtx* ctx = ZSTD_createCCtx_advanced(general_pool_mem());
    dstd::Vector<std::uint8_t> scratch;
    ~CompressContext() { ZSTD_freeCCtx(ctx); }
};

struct DecompressContext {
    ZSTD_DCtx* ctx = ZSTD_createDCtx_advanced(general_pool_mem());
    ~DecompressContext() { ZSTD_freeDCtx(ctx); }
};

const std::uint8_t* bytes_of(dstd::ByteView view) {
    return reinterpret_cast<const std::uint8_t*>(view.data());
}

std::uint32_t table_crc(const std::uint8_t* head, std::size_t count) {
    std::uint32_t crc = crc32(0, head, 8);
    crc = crc32(crc, head + 12, EXTENT_HEADER_BYTES - 12);
    return crc32(crc, head + EXTENT_HEADER_BYTES,
                 count * EXTENT_TABLE_ENTRY_BYTES);
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
        if (!segment.alone && end - page <= PAGE_BYTES) continue;
        if (segment.offset > page) {
            page = segment.offset;
            starts.push_back(page);
        }
        while (end - page > PAGE_BYTES) {
            page += static_cast<std::uint32_t>(PAGE_BYTES);
            starts.push_back(page);
        }
        if (segment.alone && end < raw_length) {
            page = end;
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
    dstd::ByteView raw, dstd::Span<const std::uint32_t> starts,
    std::size_t align, PageCodec codec_wanted, const PageDict* dict) {
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
            const std::size_t compressed =
                dict != nullptr
                    ? ZSTD_compress_usingCDict(
                          context.ctx, context.scratch.data(),
                          context.scratch.size(), data + begin, length,
                          dict->cdict())
                    : ZSTD_compressCCtx(context.ctx, context.scratch.data(),
                                        context.scratch.size(), data + begin,
                                        length, PAGE_ZSTD_LEVEL);
            if (ZSTD_isError(compressed) == 0 &&
                compressed <= length - length / 8) {
                codec = dict != nullptr ? PageCodec::ZstdDict : PageCodec::Zstd;
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
        dstd::write_u32_le(entry, begin);
        dstd::write_u32_le(entry + 4, length);
        dstd::write_u32_le(entry + 8, static_cast<std::uint32_t>(stored));
        dstd::write_u32_le(entry + 12, page_bytes);
        dstd::write_u32_le(entry + 16,
                           crc32(0, out.data() + stored, page_bytes));
        entry[20] = static_cast<std::uint8_t>(codec);
        stored += page_bytes;
    }
    out.resize(stored);
    out.resize(dstd::bits::align_up(out.size(), align));
    dstd::write_u32_le(out.data(), static_cast<std::uint32_t>(count));
    dstd::write_u32_le(out.data() + 4, static_cast<std::uint32_t>(raw_length));
    if (dict != nullptr && codec_wanted == PageCodec::Zstd) {
        dstd::write_u32_le(out.data() + 12, dict->ref().length);
        dstd::write_u64_le(out.data() + 16, dict->ref().offset);
        dstd::write_u32_le(out.data() + 24, dict->ref().crc);
    }
    dstd::write_u32_le(out.data() + 8, table_crc(out.data(), count));
    return out;
}

std::size_t ExtentPages::table_length(dstd::ByteView header) {
    if (header.size() < EXTENT_HEADER_BYTES) return 0;
    const std::uint32_t count = dstd::read_u32_le(bytes_of(header));
    if (count == 0) return 0;
    return EXTENT_HEADER_BYTES +
           static_cast<std::size_t>(count) * EXTENT_TABLE_ENTRY_BYTES;
}

dstd::Expected<ExtentPages, dstd::Error> ExtentPages::open(
    dstd::ByteView stored) {
    auto pages = parse(stored, stored.size());
    if (!pages) return pages;
    pages->stored_ = stored;
    return pages;
}

dstd::Expected<ExtentPages, dstd::Error> ExtentPages::open_table(
    dstd::ByteView head, std::uint64_t stored_length) {
    return parse(head, stored_length);
}

dstd::Expected<ExtentPages, dstd::Error> ExtentPages::parse(
    dstd::ByteView head, std::uint64_t stored_length) {
    if (head.size() < EXTENT_HEADER_BYTES || head.size() > stored_length) {
        return parse_error();
    }
    const std::uint8_t* header = bytes_of(head);
    const std::uint32_t count = dstd::read_u32_le(header);
    const std::uint32_t raw_length = dstd::read_u32_le(header + 4);
    DictRef dict;
    dict.length = dstd::read_u32_le(header + 12);
    dict.offset = dstd::read_u64_le(header + 16);
    dict.crc = dstd::read_u32_le(header + 24);
    if (dstd::read_u32_le(header + 28) != 0 || count == 0 || raw_length == 0 ||
        raw_length > EXTENT_MAX_RAW_BYTES || dict.length > PAGE_DICT_BYTES ||
        (dict.empty() && (dict.offset != 0 || dict.crc != 0))) {
        return parse_error();
    }
    if (count >
        (head.size() - EXTENT_HEADER_BYTES) / EXTENT_TABLE_ENTRY_BYTES) {
        return parse_error();
    }
    if (dstd::read_u32_le(header + 8) != table_crc(header, count)) {
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
        page.raw_offset = dstd::read_u32_le(entry);
        page.raw_length = dstd::read_u32_le(entry + 4);
        page.stored_offset = dstd::read_u32_le(entry + 8);
        page.stored_length = dstd::read_u32_le(entry + 12);
        page.crc = dstd::read_u32_le(entry + 16);
        page.codec = static_cast<PageCodec>(entry[20]);
        if (entry[21] != 0 || entry[22] != 0 || entry[23] != 0 ||
            (page.codec != PageCodec::None && page.codec != PageCodec::Zstd &&
             page.codec != PageCodec::ZstdDict) ||
            (page.codec == PageCodec::ZstdDict && dict.empty())) {
            return parse_error();
        }
        if (page.raw_offset != next_raw || page.raw_length == 0 ||
            page.raw_length > PAGE_BYTES ||
            static_cast<std::uint64_t>(page.raw_offset) + page.raw_length >
                raw_length) {
            return parse_error();
        }
        if (page.stored_offset < table_end ||
            page.stored_offset > stored_length ||
            page.stored_length > stored_length - page.stored_offset) {
            return parse_error();
        }
        next_raw += page.raw_length;
        entries.push_back(page);
    }
    if (next_raw != raw_length) return parse_error();
    return ExtentPages(dstd::ByteView{}, std::move(entries), raw_length, dict);
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
    std::size_t i, dstd::Vector<std::uint8_t>& out,
    const PageDict* dict) const {
    if (i >= entries_.size() || stored_.empty()) return parse_error();
    const PageEntry& page = entries_[i];
    out.resize(page.raw_length);
    return decode_page(
        page,
        dstd::ByteView{stored_.data() + page.stored_offset, page.stored_length},
        out.data(), dict);
}

dstd::Expected<void, dstd::Error> ExtentPages::decode_page(
    const PageEntry& page, dstd::ByteView stored, std::uint8_t* out,
    const PageDict* dict) {
    if (stored.size() != page.stored_length) return parse_error();
    const std::uint8_t* src = bytes_of(stored);
    if (crc32(0, src, page.stored_length) != page.crc) return parse_error();
    switch (page.codec) {
        case PageCodec::None: {
            if (page.stored_length != page.raw_length) return parse_error();
            std::memcpy(out, src, page.raw_length);
            return {};
        }
        case PageCodec::Zstd: {
            thread_local DecompressContext context;
            if (context.ctx == nullptr) return parse_error();
            const std::size_t got = ZSTD_decompressDCtx(
                context.ctx, out, page.raw_length, src, page.stored_length);
            if (ZSTD_isError(got) != 0 || got != page.raw_length) {
                return parse_error();
            }
            return {};
        }
        case PageCodec::ZstdDict: {
            thread_local DecompressContext context;
            if (context.ctx == nullptr || dict == nullptr) return parse_error();
            const std::size_t got = ZSTD_decompress_usingDDict(
                context.ctx, out, page.raw_length, src, page.stored_length,
                dict->ddict());
            if (ZSTD_isError(got) != 0 || got != page.raw_length) {
                return parse_error();
            }
            return {};
        }
    }
    return parse_error();
}

dstd::Expected<dstd::SharedPtr<const PageDict>, dstd::Error>
PageDict::for_encode(dstd::ByteView bytes, const DictRef& ref) {
    ZSTD_CDict* cdict = ZSTD_createCDict_advanced(
        bytes.data(), bytes.size(), ZSTD_dlm_byCopy, ZSTD_dct_auto,
        ZSTD_getCParams(PAGE_ZSTD_LEVEL, 0, bytes.size()), general_pool_mem());
    if (cdict == nullptr) return parse_error();
    return dstd::SharedPtr<const PageDict>(new PageDict(ref, cdict, nullptr));
}

dstd::Expected<dstd::SharedPtr<const PageDict>, dstd::Error>
PageDict::for_decode(dstd::ByteView bytes, const DictRef& ref) {
    if (ref.empty() || bytes.size() != ref.length ||
        crc32(0, bytes_of(bytes), bytes.size()) != ref.crc) {
        return parse_error();
    }
    ZSTD_DDict* ddict =
        ZSTD_createDDict_advanced(bytes.data(), bytes.size(), ZSTD_dlm_byCopy,
                                  ZSTD_dct_auto, general_pool_mem());
    if (ddict == nullptr) return parse_error();
    return dstd::SharedPtr<const PageDict>(new PageDict(ref, nullptr, ddict));
}

PageDict::~PageDict() {
    ZSTD_freeCDict(cdict_);
    ZSTD_freeDDict(ddict_);
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> train_page_dict(
    dstd::ByteView samples, dstd::Span<const std::size_t> sizes) {
    if (sizes.empty() || sizes.size() > std::numeric_limits<unsigned>::max()) {
        return parse_error();
    }
    dstd::Vector<std::uint8_t> dict(PAGE_DICT_BYTES);
    const std::size_t length = ZDICT_trainFromBuffer(
        dict.data(), dict.size(), samples.data(), sizes.data(),
        static_cast<unsigned>(sizes.size()));
    if (ZDICT_isError(length) != 0) return parse_error();
    dict.resize(length);
    return dict;
}

bool compress_meta(dstd::ByteView raw, int level,
                   dstd::Vector<std::uint8_t>& out) {
    const std::uint8_t* data = bytes_of(raw);
    const std::size_t length = raw.size();
    out.clear();
    thread_local CompressContext context;
    if (context.ctx == nullptr || length <= META_HEADER_BYTES ||
        length > EXTENT_MAX_RAW_BYTES) {
        return false;
    }
    out.resize(META_HEADER_BYTES + ZSTD_compressBound(length));
    const std::size_t compressed =
        ZSTD_compressCCtx(context.ctx, out.data() + META_HEADER_BYTES,
                          out.size() - META_HEADER_BYTES, data, length, level);
    if (ZSTD_isError(compressed) != 0 ||
        META_HEADER_BYTES + compressed > length - length / 8) {
        out.clear();
        return false;
    }
    out[0] = static_cast<std::uint8_t>(PageCodec::Zstd);
    dstd::write_u32_le(out.data() + 1, static_cast<std::uint32_t>(length));
    out.resize(META_HEADER_BYTES + compressed);
    return true;
}

dstd::Vector<std::uint8_t> encode_meta(dstd::ByteView raw, int level) {
    dstd::Vector<std::uint8_t> out;
    if (compress_meta(raw, level, out)) return out;
    const std::size_t length = raw.size();
    out.resize(1 + length);
    out[0] = static_cast<std::uint8_t>(PageCodec::None);
    if (length > 0) std::memcpy(out.data() + 1, bytes_of(raw), length);
    return out;
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> decode_meta(
    dstd::ByteView stored) {
    const std::uint8_t* src = bytes_of(stored);
    if (stored.empty()) return parse_error();
    switch (static_cast<PageCodec>(src[0])) {
        case PageCodec::None:
            return dstd::Vector<std::uint8_t>(src + 1, src + stored.size());
        case PageCodec::Zstd: {
            if (stored.size() < META_HEADER_BYTES) return parse_error();
            const std::uint32_t length = dstd::read_u32_le(src + 1);
            if (length > EXTENT_MAX_RAW_BYTES) return parse_error();
            thread_local DecompressContext context;
            if (context.ctx == nullptr) return parse_error();
            dstd::Vector<std::uint8_t> out(length);
            const std::size_t got = ZSTD_decompressDCtx(
                context.ctx, out.data(), length, src + META_HEADER_BYTES,
                stored.size() - META_HEADER_BYTES);
            if (ZSTD_isError(got) != 0 || got != length) return parse_error();
            return out;
        }
        case PageCodec::ZstdDict:
            return parse_error();
    }
    return parse_error();
}

dstd::Expected<void, dstd::Error> decode_extent(
    dstd::Vector<std::uint8_t>& stored, const PageDict* dict) {
    auto pages =
        ExtentPages::open(dstd::ByteView(stored.data(), stored.size()));
    if (!pages) return parse_error();
    if (dict != nullptr && (dict->ref().offset != pages->dict().offset ||
                            dict->ref().length != pages->dict().length ||
                            dict->ref().crc != pages->dict().crc)) {
        return parse_error();
    }
    dstd::Vector<std::uint8_t> raw(pages->raw_length(), std::uint8_t{0});
    dstd::Vector<std::uint8_t> page;
    for (std::size_t i = 0; i < pages->size(); ++i) {
        auto decoded = pages->decode(i, page, dict);
        if (!decoded) return decoded;
        std::memcpy(raw.data() + pages->entry(i).raw_offset, page.data(),
                    page.size());
    }
    stored = std::move(raw);
    return {};
}

}  // namespace dftracer::utils::store::engine
