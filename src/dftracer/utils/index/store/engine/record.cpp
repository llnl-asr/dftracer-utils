#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/record.h>
#include <dftracer/utils/index/store/engine/varint.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>

namespace dftracer::utils::index::store::engine {

namespace {

namespace dstd = dftracer::utils::dstd;
namespace compress = dftracer::utils::utilities::fileio::compress;

constexpr std::string_view RECORD_MAGIC = "DFTUREC0";
constexpr std::size_t RECORD_HEADER_BYTES = 40;
constexpr std::size_t MIN_SCOPE_PATH_BYTES = 25;
constexpr std::size_t MIN_PAGE_SECTION_BYTES = 22;

dstd::Unexpected<dstd::Error> parse_error() {
    return dstd::Unexpected<dstd::Error>(dstd::Error(dstd::Condition::Parse));
}

dstd::Unexpected<dstd::Error> invalid_error() {
    return dstd::Unexpected<dstd::Error>(
        dstd::Error(dstd::Condition::InvalidArgument));
}

void append_magic(dstd::Vector<std::uint8_t>& out, std::string_view magic) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(magic.data());
    out.insert(out.end(), begin, begin + magic.size());
}

void append_byte_string(dstd::Vector<std::uint8_t>& out,
                        const dstd::String& value) {
    encode_varint(out, value.size());
    const auto* begin = reinterpret_cast<const std::uint8_t*>(value.data());
    out.insert(out.end(), begin, begin + value.size());
}

void append_byte_vector(dstd::Vector<std::uint8_t>& out,
                        const dstd::Vector<std::uint8_t>& value) {
    encode_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void append_item(dstd::Vector<std::uint8_t>& out, std::uint64_t tag,
                 const dstd::Vector<std::uint8_t>& payload) {
    encode_varint(out, tag);
    encode_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

bool read_u8(ByteView bytes, std::size_t& at, std::uint8_t& out) {
    if (bytes.size() - at < 1) return false;
    out = static_cast<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data())[at]);
    ++at;
    return true;
}

bool read_u32(ByteView bytes, std::size_t& at, std::uint32_t& out) {
    if (bytes.size() - at < 4) return false;
    out = read_u32_le(reinterpret_cast<const std::uint8_t*>(bytes.data()) + at);
    at += 4;
    return true;
}

bool read_u64(ByteView bytes, std::size_t& at, std::uint64_t& out) {
    if (bytes.size() - at < 8) return false;
    out = read_u64_le(reinterpret_cast<const std::uint8_t*>(bytes.data()) + at);
    at += 8;
    return true;
}

bool count_fits(std::size_t remaining, std::uint64_t count,
                std::size_t min_entry_bytes) {
    return count <= remaining / min_entry_bytes;
}

bool read_bytes(ByteView bytes, std::size_t& at,
                dstd::Vector<std::uint8_t>& out) {
    auto length = decode_varint(bytes, at);
    if (!length) return false;
    if (*length > bytes.size() - at) return false;
    const auto* begin =
        reinterpret_cast<const std::uint8_t*>(bytes.data()) + at;
    out.assign(begin, begin + *length);
    at += static_cast<std::size_t>(*length);
    return true;
}

bool read_string(ByteView bytes, std::size_t& at, dstd::String& out) {
    dstd::Vector<std::uint8_t> raw;
    if (!read_bytes(bytes, at, raw)) return false;
    out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    return true;
}

dstd::Expected<NamespaceItem, dstd::Error> decode_namespace(ByteView bytes) {
    NamespaceItem item;
    std::size_t at = 0;
    if (!read_u32(bytes, at, item.local_id) ||
        !read_string(bytes, at, item.name) ||
        !read_u32(bytes, at, item.format_version) ||
        !read_u8(bytes, at, item.fold_kind) ||
        !read_u8(bytes, at, item.codec) || !read_u8(bytes, at, item.durable) ||
        at != bytes.size()) {
        return parse_error();
    }
    return item;
}

dstd::Expected<ScopeItem, dstd::Error> decode_scope(ByteView bytes) {
    ScopeItem item;
    std::uint32_t path_count = 0;
    std::uint8_t kind = 0;
    std::size_t at = 0;
    if (!read_u32(bytes, at, item.local_id) || !read_u8(bytes, at, kind) ||
        !read_u32(bytes, at, path_count) || kind > 2) {
        return parse_error();
    }
    item.kind = static_cast<ScopeKind>(kind);
    if (!count_fits(bytes.size() - at, path_count, MIN_SCOPE_PATH_BYTES)) {
        return parse_error();
    }
    item.paths.reserve(path_count);
    for (std::uint32_t i = 0; i < path_count; ++i) {
        ScopePath path;
        if (!read_string(bytes, at, path.logical_path) ||
            !read_u64(bytes, at, path.mtime) ||
            !read_u64(bytes, at, path.size) ||
            !read_u64(bytes, at, path.hash)) {
            return parse_error();
        }
        item.paths.push_back(std::move(path));
    }
    if (at != bytes.size()) return parse_error();
    return item;
}

dstd::Expected<CopyItem, dstd::Error> decode_copy(ByteView bytes) {
    CopyItem item;
    std::uint8_t state = 0;
    std::size_t at = 0;
    if (!read_u32(bytes, at, item.local_id) ||
        !read_u32(bytes, at, item.namespace_id) ||
        !read_u32(bytes, at, item.scope_id) ||
        !read_u64(bytes, at, item.params_hash) ||
        !read_u64(bytes, at, item.generation) || !read_u8(bytes, at, state) ||
        state > 1 || at != bytes.size()) {
        return parse_error();
    }
    item.state = static_cast<CopyState>(state);
    return item;
}

dstd::Expected<DropItem, dstd::Error> decode_drop(ByteView bytes) {
    DropItem item;
    std::size_t at = 0;
    if (!read_u32(bytes, at, item.namespace_id) ||
        !read_u32(bytes, at, item.scope_id) ||
        !read_u64(bytes, at, item.generation) ||
        !read_bytes(bytes, at, item.begin_key) ||
        !read_bytes(bytes, at, item.end_key) || at != bytes.size()) {
        return parse_error();
    }
    return item;
}

dstd::Expected<PageRefItem, dstd::Error> decode_page_ref(ByteView bytes) {
    PageRefItem item;
    std::uint32_t section_count = 0;
    std::size_t at = 0;
    if (!read_u64(bytes, at, item.offset) ||
        !read_u32(bytes, at, item.length) || !read_u32(bytes, at, item.crc) ||
        !read_u64(bytes, at, item.sequence) ||
        !read_u32(bytes, at, section_count)) {
        return parse_error();
    }
    if (item.sequence == 0) return parse_error();
    if (!count_fits(bytes.size() - at, section_count, MIN_PAGE_SECTION_BYTES)) {
        return parse_error();
    }
    item.sections.reserve(section_count);
    for (std::uint32_t i = 0; i < section_count; ++i) {
        PageSection section;
        if (!read_u32(bytes, at, section.copy_id) ||
            !read_u32(bytes, at, section.offset_in_page) ||
            !read_u32(bytes, at, section.length) ||
            !read_u64(bytes, at, section.entry_count) ||
            !read_bytes(bytes, at, section.first_key) ||
            !read_bytes(bytes, at, section.last_key)) {
            return parse_error();
        }
        item.sections.push_back(std::move(section));
    }
    if (at != bytes.size()) return parse_error();
    return item;
}

void encode_namespace(dstd::Vector<std::uint8_t>& payload,
                      const NamespaceItem& item) {
    append_u32_le(payload, item.local_id);
    append_byte_string(payload, item.name);
    append_u32_le(payload, item.format_version);
    payload.push_back(item.fold_kind);
    payload.push_back(item.codec);
    payload.push_back(item.durable);
}

void encode_scope(dstd::Vector<std::uint8_t>& payload, const ScopeItem& item) {
    append_u32_le(payload, item.local_id);
    payload.push_back(static_cast<std::uint8_t>(item.kind));
    append_u32_le(payload, static_cast<std::uint32_t>(item.paths.size()));
    for (const ScopePath& path : item.paths) {
        append_byte_string(payload, path.logical_path);
        append_u64_le(payload, path.mtime);
        append_u64_le(payload, path.size);
        append_u64_le(payload, path.hash);
    }
}

void encode_copy(dstd::Vector<std::uint8_t>& payload, const CopyItem& item) {
    append_u32_le(payload, item.local_id);
    append_u32_le(payload, item.namespace_id);
    append_u32_le(payload, item.scope_id);
    append_u64_le(payload, item.params_hash);
    append_u64_le(payload, item.generation);
    payload.push_back(static_cast<std::uint8_t>(item.state));
}

void encode_drop(dstd::Vector<std::uint8_t>& payload, const DropItem& item) {
    append_u32_le(payload, item.namespace_id);
    append_u32_le(payload, item.scope_id);
    append_u64_le(payload, item.generation);
    append_byte_vector(payload, item.begin_key);
    append_byte_vector(payload, item.end_key);
}

void encode_page_ref(dstd::Vector<std::uint8_t>& payload,
                     const PageRefItem& item) {
    append_u64_le(payload, item.offset);
    append_u32_le(payload, item.length);
    append_u32_le(payload, item.crc);
    append_u64_le(payload, item.sequence);
    append_u32_le(payload, static_cast<std::uint32_t>(item.sections.size()));
    for (const PageSection& section : item.sections) {
        append_u32_le(payload, section.copy_id);
        append_u32_le(payload, section.offset_in_page);
        append_u32_le(payload, section.length);
        append_u64_le(payload, section.entry_count);
        append_byte_vector(payload, section.first_key);
        append_byte_vector(payload, section.last_key);
    }
}

}  // namespace

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_record(
    RecordKind kind, std::uint64_t sequence, std::uint64_t prev_offset,
    std::uint64_t offset, const RecordItems& items) {
    if (sequence == 0 || offset < RECORD_BYTES || offset % RECORD_BYTES != 0) {
        return invalid_error();
    }
    if (static_cast<std::uint32_t>(kind) < 1 ||
        static_cast<std::uint32_t>(kind) > 3) {
        return invalid_error();
    }

    dstd::Vector<std::uint8_t> body;
    for (const RecordItem& item : items) {
        dstd::Vector<std::uint8_t> payload;
        if (const auto* namespace_item = std::get_if<NamespaceItem>(&item)) {
            encode_namespace(payload, *namespace_item);
            append_item(body, 1, payload);
        } else if (const auto* scope_item = std::get_if<ScopeItem>(&item)) {
            encode_scope(payload, *scope_item);
            append_item(body, 2, payload);
        } else if (const auto* copy_item = std::get_if<CopyItem>(&item)) {
            encode_copy(payload, *copy_item);
            append_item(body, 3, payload);
        } else if (const auto* drop_item = std::get_if<DropItem>(&item)) {
            encode_drop(payload, *drop_item);
            append_item(body, 4, payload);
        } else if (const auto* page_ref_item =
                       std::get_if<PageRefItem>(&item)) {
            if (page_ref_item->sequence == 0) return invalid_error();
            encode_page_ref(payload, *page_ref_item);
            append_item(body, 5, payload);
        } else {
            return invalid_error();
        }
    }
    if (body.size() > std::numeric_limits<std::uint32_t>::max()) {
        return invalid_error();
    }

    dstd::Vector<std::uint8_t> out;
    out.reserve(RECORD_BYTES);
    append_magic(out, RECORD_MAGIC);
    append_u32_le(out, static_cast<std::uint32_t>(kind));
    append_u32_le(out, static_cast<std::uint32_t>(body.size()));
    append_u64_le(out, sequence);
    append_u64_le(out, prev_offset);
    append_u64_le(out, offset);
    out.insert(out.end(), body.begin(), body.end());
    const std::uint32_t crc = compress::gzip_crc32(0, out.data(), out.size());
    append_u32_le(out, crc);
    const std::size_t padded_size =
        (out.size() + RECORD_BYTES - 1) / RECORD_BYTES * RECORD_BYTES;
    out.resize(padded_size, 0);
    return out;
}

dstd::Expected<dstd::Vector<std::uint8_t>, dstd::Error> encode_record(
    RecordKind kind, std::uint64_t sequence, std::uint64_t prev_offset,
    std::uint64_t offset, std::initializer_list<RecordItem> items) {
    RecordItems owned;
    owned.reserve(items.size());
    for (const RecordItem& item : items) owned.push_back(item);
    return encode_record(kind, sequence, prev_offset, offset, owned);
}

dstd::Expected<Record, dstd::Error> decode_record(ByteView bytes) {
    if (bytes.size() < RECORD_HEADER_BYTES + 4) return parse_error();
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (std::memcmp(data, RECORD_MAGIC.data(), RECORD_MAGIC.size()) != 0) {
        return parse_error();
    }

    const std::uint32_t kind_raw = read_u32_le(data + 8);
    if (kind_raw < 1 || kind_raw > 3) return parse_error();
    const std::uint32_t body_length = read_u32_le(data + 12);
    const std::uint64_t sequence = read_u64_le(data + 16);
    if (sequence == 0) return parse_error();
    const std::uint64_t prev_offset = read_u64_le(data + 24);
    const std::uint64_t offset = read_u64_le(data + 32);

    const std::uint64_t body_end =
        static_cast<std::uint64_t>(RECORD_HEADER_BYTES) + body_length;
    if (body_end + 4 > bytes.size()) return parse_error();
    const std::uint32_t stored_crc = read_u32_le(data + body_end);
    const std::uint32_t crc =
        compress::gzip_crc32(0, data, static_cast<std::size_t>(body_end));
    if (stored_crc != crc) return parse_error();

    Record record;
    record.kind = static_cast<RecordKind>(kind_raw);
    record.sequence = sequence;
    record.prev_offset = prev_offset;
    record.offset = offset;
    record.crc = stored_crc;

    const ByteView body = bytes.subspan(RECORD_HEADER_BYTES, body_length);
    std::size_t at = 0;
    while (at < body.size()) {
        auto tag = decode_varint(body, at);
        if (!tag) return parse_error();
        auto item_length = decode_varint(body, at);
        if (!item_length || *item_length > body.size() - at)
            return parse_error();
        const ByteView item_bytes = body.subspan(at, *item_length);
        at += static_cast<std::size_t>(*item_length);

        switch (*tag) {
            case 1: {
                auto item = decode_namespace(item_bytes);
                if (!item) return parse_error();
                record.items.push_back(std::move(*item));
                break;
            }
            case 2: {
                auto item = decode_scope(item_bytes);
                if (!item) return parse_error();
                record.items.push_back(std::move(*item));
                break;
            }
            case 3: {
                auto item = decode_copy(item_bytes);
                if (!item) return parse_error();
                record.items.push_back(std::move(*item));
                break;
            }
            case 4: {
                auto item = decode_drop(item_bytes);
                if (!item) return parse_error();
                record.items.push_back(std::move(*item));
                break;
            }
            case 5: {
                auto item = decode_page_ref(item_bytes);
                if (!item) return parse_error();
                record.items.push_back(std::move(*item));
                break;
            }
            default:
                return parse_error();
        }
    }
    return record;
}

}  // namespace dftracer::utils::index::store::engine
