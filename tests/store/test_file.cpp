// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/store/crc.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/extent_codec.h>
#include <dftracer/utils/store/fold.h>
#include <dftracer/utils/store/scope_directory.h>
#include <dftracer/utils/store/segment.h>
#include <dftracer/utils/store/varint.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <random>
#include <utility>

using namespace dftracer::utils;

namespace dftracer::utils::store::engine {

struct DataFileWriterTestHook {
    static void close_fd(DataFileWriter& writer) {
        if (writer.fd_ >= 0) ::close(writer.fd_);
        writer.fd_ = -1;
    }
};

}  // namespace dftracer::utils::store::engine

namespace {

namespace store = dftracer::utils::store;
namespace memory = dftracer::utils::memory;

using dstd::ByteView;
using store::ScopeKind;
using store::SourceStat;
using store::engine::CopyItem;
using store::engine::CopyState;
using store::engine::DataFileReader;
using store::engine::DataFileWriter;
using store::engine::DropItem;
using store::engine::EraseItem;
using store::engine::ExtentRef;
using store::engine::ExtentRefItem;
using store::engine::ExtentSegment;
using store::engine::FileIndex;
using store::engine::IndexedExtentRef;
using store::engine::NamespaceItem;
using store::engine::ScopeItem;
using store::engine::SegmentBuilder;

constexpr std::size_t EXTENT_SIZE = 4096;

ByteView view_of(const dstd::String& bytes) {
    return {bytes.data(), bytes.size()};
}

ByteView view_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

dstd::Vector<std::uint8_t> bytes_of(ByteView bytes) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return dstd::Vector<std::uint8_t>(begin, begin + bytes.size());
}

dstd::Vector<std::uint8_t> bytes_of_string(const dstd::String& value) {
    return bytes_of(view_of(value));
}

dstd::Vector<std::uint8_t> std_bytes(const dstd::Vector<std::uint8_t>& bytes) {
    return dstd::Vector<std::uint8_t>(bytes.begin(), bytes.end());
}

bool same_bytes(ByteView a, ByteView b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

std::uint32_t read_u32_le_at(const dstd::Vector<std::uint8_t>& bytes,
                             std::size_t offset) {
    const auto* data = bytes.data();
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
}

std::uint64_t read_u64_le_at(const dstd::Vector<std::uint8_t>& bytes,
                             std::size_t offset) {
    return static_cast<std::uint64_t>(read_u32_le_at(bytes, offset)) |
           (static_cast<std::uint64_t>(read_u32_le_at(bytes, offset + 4U))
            << 32U);
}

void write_u32_le_at(dstd::Vector<std::uint8_t>& bytes, std::size_t offset,
                     std::uint32_t value) {
    dstd::write_u32_le(bytes.data() + offset, value);
}

void write_u64_le_at(dstd::Vector<std::uint8_t>& bytes, std::size_t offset,
                     std::uint64_t value) {
    dstd::write_u64_le(bytes.data() + offset, value);
}

std::size_t header_tail_field_offset(const dstd::Vector<std::uint8_t>& file) {
    const std::size_t host_length =
        std::size_t{file[44]} | (std::size_t{file[45]} << 8U);
    const std::size_t label_length =
        std::size_t{file[46]} | (std::size_t{file[47]} << 8U);
    return 48U + host_length + label_length;
}

std::uint64_t header_tail_offset(const dstd::Vector<std::uint8_t>& file) {
    return read_u64_le_at(file, header_tail_field_offset(file));
}

void rewrite_header_crc(dstd::Vector<std::uint8_t>& header) {
    REQUIRE(header.size() >= store::engine::FILE_HEADER_BYTES);
    write_u32_le_at(header, 12, 0);
    const std::uint32_t crc = store::engine::crc32(
        0, header.data(), store::engine::FILE_HEADER_BYTES);
    write_u32_le_at(header, 12, crc);
}

std::size_t record_crc_offset(const dstd::Vector<std::uint8_t>& file,
                              std::uint64_t record_offset) {
    const std::uint32_t body_length =
        read_u32_le_at(file, static_cast<std::size_t>(record_offset) + 12U);
    return static_cast<std::size_t>(record_offset) + 40U + body_length;
}

void rewrite_record_crc(dstd::Vector<std::uint8_t>& file,
                        std::uint64_t record_offset) {
    const std::size_t crc_at = record_crc_offset(file, record_offset);
    const std::size_t record_at = static_cast<std::size_t>(record_offset);
    const std::uint32_t crc =
        store::engine::crc32(0, file.data() + record_at, crc_at - record_at);
    write_u32_le_at(file, crc_at, crc);
}

// 40-byte record header, the body codec byte, then the one-byte varint item
// tag and length. A test that edits the payload needs a body stored raw.
std::size_t first_item_payload_offset(
    const dstd::Vector<std::uint8_t>& record) {
    REQUIRE(record.size() > 40U);
    REQUIRE(record[40] == 0U);
    return 43U;
}

dstd::Vector<std::uint8_t> read_file(dstd::PathView path) {
    const dstd::Path owned(path.str());
    const dstd::String text = dftu_utils_test::read_text(owned);
    return dstd::Vector<std::uint8_t>(text.begin(), text.end());
}

void write_file(dstd::PathView path, const dstd::Vector<std::uint8_t>& bytes) {
    const dstd::Path owned(path.str());
    dftu_utils_test::FileOut output(owned);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void corrupt_header(dstd::Vector<std::uint8_t>& bytes) {
    REQUIRE(bytes.size() > 16);
    bytes[1] ^= 1;
}

struct TempDir {
    dstd::Path path;

    TempDir() {
        const auto now =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = dstd::fs::temp_directory() /
               ("dftu_store_file_" + dstd::to_string(::getpid()) + "_" +
                dstd::to_string(now));
        REQUIRE(dstd::fs::create_directories(path).has_value());
    }

    ~TempDir() { (void)dstd::fs::remove_all(path); }

    dstd::String file(const dstd::String& name) const {
        return dstd::String((path / name).str());
    }
};

struct SegmentData {
    dstd::Vector<std::uint8_t> bytes;
    std::uint64_t entry_count = 0;
    dstd::Vector<std::uint8_t> first_key;
    dstd::Vector<std::uint8_t> last_key;
};

SegmentData make_segment(std::uint32_t seed, std::size_t entry_count) {
    SegmentBuilder builder;
    for (std::size_t i = 0; i < entry_count; ++i) {
        const dstd::String key =
            "key-" + dstd::to_string(seed) + "-" + dstd::to_string(i);
        const dstd::String value(
            64, static_cast<char>('a' + static_cast<int>((seed + i) % 26U)));
        builder.add(view_of(key), view_of(value));
    }
    auto sealed = builder.seal(nullptr, nullptr, nullptr);
    if (!sealed) return {};
    return {std::move(*sealed), builder.entry_count(),
            bytes_of(builder.first_key()), bytes_of(builder.last_key())};
}

NamespaceItem make_namespace(std::uint32_t local_id) {
    NamespaceItem item;
    item.local_id = local_id;
    item.name = dstd::String("test.ns." + dstd::to_string(local_id));
    item.format_version = 7;
    item.fold_kind = 0;
    item.codec = 0;
    item.durable = local_id % 2;
    return item;
}

ScopeItem make_scope(std::uint32_t local_id) {
    ScopeItem item;
    item.local_id = local_id;
    item.kind = local_id % 2 == 0 ? ScopeKind::File : ScopeKind::Files;
    item.paths.push_back(
        dstd::String("/traces/run" + dstd::to_string(local_id) + ".pfw"));
    return item;
}

SourceStat make_source(std::uint32_t local_id, std::uint32_t namespace_id) {
    SourceStat source;
    source.mtime = 1000 + local_id;
    source.size = 2000 + local_id;
    source.hash = 3000 + namespace_id;
    return source;
}

CopyItem make_copy(std::uint32_t local_id, std::uint32_t namespace_id,
                   std::uint32_t scope_id) {
    CopyItem item;
    item.local_id = local_id;
    item.namespace_id = namespace_id;
    item.scope_id = scope_id;
    item.params_hash = 4000 + local_id;
    item.generation = local_id;
    item.state = local_id % 2 == 0 ? CopyState::Ready : CopyState::Failed;
    item.sources.push_back(make_source(local_id, namespace_id));
    return item;
}

DropItem make_drop(std::uint32_t namespace_id, std::uint32_t scope_id,
                   std::uint32_t generation) {
    DropItem item;
    item.namespace_id = namespace_id;
    item.scope_id = scope_id;
    item.generation = generation;
    if (generation % 2 != 0) {
        item.begin_key =
            bytes_of_string("begin-" + dstd::to_string(generation));
        item.end_key = bytes_of_string("end-" + dstd::to_string(generation));
    }
    return item;
}

ExtentRefItem make_extent_ref(const ExtentRef& extent,
                              const SegmentData& segment,
                              std::uint32_t copy_id) {
    ExtentRefItem item;
    item.offset = extent.offset;
    item.length = extent.length;
    item.crc = extent.crc;
    item.sequence = copy_id;
    ExtentSegment extent_segment;
    extent_segment.copy_id = copy_id;
    extent_segment.offset_in_extent = 0;
    extent_segment.length = static_cast<std::uint32_t>(segment.bytes.size());
    extent_segment.entry_count = segment.entry_count;
    extent_segment.first_key = segment.first_key;
    extent_segment.last_key = segment.last_key;
    item.segments.push_back(std::move(extent_segment));
    return item;
}

SegmentData make_wide_segment(std::uint32_t seed, std::size_t key_bytes) {
    SegmentBuilder builder;
    dstd::String key = dstd::to_string(seed);
    key.resize(key_bytes, 'k');
    const dstd::String value = "v" + dstd::to_string(seed);
    builder.add(view_of(key), view_of(value));
    auto sealed = builder.seal(nullptr, nullptr, nullptr);
    if (!sealed) return {};
    return {std::move(*sealed), builder.entry_count(),
            bytes_of(builder.first_key()), bytes_of(builder.last_key())};
}

struct SegmentBlockEntry {
    std::uint32_t extent_index = 0;
    std::uint64_t sequence = 0;
    std::uint64_t extent_offset = 0;
    std::uint32_t extent_length = 0;
    std::uint32_t extent_crc = 0;
    std::uint32_t offset_in_extent = 0;
    std::uint32_t length = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t ordinal = 0;
    dstd::String first_key;
    dstd::String last_key;
};

struct SegmentBlockView {
    store::engine::BlockHandle handle;
    std::size_t raw_bytes = 0;
    dstd::Vector<SegmentBlockEntry> entries;
};

struct ClosedRun {
    std::uint32_t copy_id = 0;
    std::uint64_t sequence = 0;
    store::engine::BlockHandle handle;
};

dstd::Vector<ClosedRun> closed_runs(const DataFileReader& reader) {
    dstd::Vector<ClosedRun> runs;
    const FileIndex& index = reader.index();
    REQUIRE(index.tail.has_value());
    REQUIRE(index.tail->directory_levels == 1);
    auto top = store::engine::SlotTable::open(view_of(index.tail->top_table));
    REQUIRE(top.has_value());
    for (std::size_t i = 0; i < top->size(); ++i) {
        store::engine::BlockHandle handle;
        std::size_t at = 0;
        REQUIRE(store::engine::read_block_handle(top->payload(i), at, handle));
        auto bytes = reader.read_block(handle, nullptr);
        REQUIRE(bytes.has_value());
        auto directory = store::engine::ScopeDirectory::open(view_of(*bytes));
        REQUIRE(directory.has_value());
        for (std::size_t entry = 0; entry < directory->size(); ++entry) {
            auto decoded = directory->entry(entry);
            REQUIRE(decoded.has_value());
            for (const store::engine::DirectoryCopy& copy : decoded->copies) {
                for (const store::engine::DirectoryCommit& commit :
                     copy.commits) {
                    runs.push_back(
                        {copy.local_id, commit.sequence, commit.segments});
                }
            }
        }
    }
    return runs;
}

SegmentBlockView read_segment_block(const DataFileReader& reader,
                                    store::engine::BlockHandle handle) {
    SegmentBlockView block;
    block.handle = handle;
    auto bytes = reader.read_block(handle, nullptr);
    REQUIRE(bytes.has_value());
    const dstd::Vector<std::uint8_t> raw = std_bytes(*bytes);
    block.raw_bytes = raw.size();
    const std::uint32_t count = read_u32_le_at(raw, 0);
    std::size_t at = 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        REQUIRE(at + 52 <= raw.size());
        SegmentBlockEntry entry;
        entry.extent_index = read_u32_le_at(raw, at);
        entry.sequence = read_u64_le_at(raw, at + 4U);
        entry.extent_offset = read_u64_le_at(raw, at + 12U);
        entry.extent_length = read_u32_le_at(raw, at + 20U);
        entry.extent_crc = read_u32_le_at(raw, at + 24U);
        entry.offset_in_extent = read_u32_le_at(raw, at + 28U);
        entry.length = read_u32_le_at(raw, at + 32U);
        entry.entry_count = read_u64_le_at(raw, at + 36U);
        entry.ordinal = read_u64_le_at(raw, at + 44U);
        at += 52;
        for (dstd::String* key : {&entry.first_key, &entry.last_key}) {
            ByteView body = view_of(raw);
            auto length = store::engine::decode_varint(body, at);
            REQUIRE(length.has_value());
            REQUIRE(at + *length <= raw.size());
            key->assign(reinterpret_cast<const char*>(raw.data() + at),
                        static_cast<std::size_t>(*length));
            at += static_cast<std::size_t>(*length);
        }
        block.entries.push_back(std::move(entry));
    }
    CHECK(at == raw.size());
    return block;
}

void check_namespace(const NamespaceItem& actual,
                     const NamespaceItem& expected) {
    CHECK(actual.local_id == expected.local_id);
    CHECK(dstd::String(actual.name) == dstd::String(expected.name));
    CHECK(actual.format_version == expected.format_version);
    CHECK(actual.fold_kind == expected.fold_kind);
    CHECK(actual.codec == expected.codec);
    CHECK(actual.durable == expected.durable);
}

void check_scope(const ScopeItem& actual, const ScopeItem& expected) {
    CHECK(actual.local_id == expected.local_id);
    CHECK(actual.kind == expected.kind);
    REQUIRE(actual.paths.size() == expected.paths.size());
    for (std::size_t i = 0; i < actual.paths.size(); ++i) {
        CHECK(dstd::String(actual.paths[i]) == dstd::String(expected.paths[i]));
    }
}

void check_copy(const CopyItem& actual, const CopyItem& expected) {
    CHECK(actual.local_id == expected.local_id);
    CHECK(actual.namespace_id == expected.namespace_id);
    CHECK(actual.scope_id == expected.scope_id);
    CHECK(actual.params_hash == expected.params_hash);
    CHECK(actual.generation == expected.generation);
    CHECK(actual.state == expected.state);
    REQUIRE(actual.sources.size() == expected.sources.size());
    for (std::size_t i = 0; i < actual.sources.size(); ++i) {
        CHECK(actual.sources[i].mtime == expected.sources[i].mtime);
        CHECK(actual.sources[i].size == expected.sources[i].size);
        CHECK(actual.sources[i].hash == expected.sources[i].hash);
    }
}

void check_drop(const DropItem& actual, const DropItem& expected) {
    CHECK(actual.namespace_id == expected.namespace_id);
    CHECK(actual.scope_id == expected.scope_id);
    CHECK(actual.generation == expected.generation);
    CHECK(same_bytes(view_of(actual.begin_key), view_of(expected.begin_key)));
    CHECK(same_bytes(view_of(actual.end_key), view_of(expected.end_key)));
}

void check_erase(const EraseItem& actual, const EraseItem& expected) {
    CHECK(actual.copy_id == expected.copy_id);
    CHECK(actual.ordinal == expected.ordinal);
    CHECK(same_bytes(view_of(actual.begin_key), view_of(expected.begin_key)));
    CHECK(same_bytes(view_of(actual.end_key), view_of(expected.end_key)));
}

void check_extent_ref(const IndexedExtentRef& actual,
                      const IndexedExtentRef& expected) {
    CHECK(actual.sequence == expected.sequence);
    CHECK(actual.ref.offset == expected.ref.offset);
    CHECK(actual.ref.length == expected.ref.length);
    CHECK(actual.ref.crc == expected.ref.crc);
    REQUIRE(actual.ref.segments.size() == expected.ref.segments.size());
    for (std::size_t i = 0; i < actual.ref.segments.size(); ++i) {
        CHECK(actual.ref.segments[i].copy_id ==
              expected.ref.segments[i].copy_id);
        CHECK(actual.ref.segments[i].offset_in_extent ==
              expected.ref.segments[i].offset_in_extent);
        CHECK(actual.ref.segments[i].length == expected.ref.segments[i].length);
        CHECK(actual.ref.segments[i].entry_count ==
              expected.ref.segments[i].entry_count);
        CHECK(same_bytes(view_of(actual.ref.segments[i].first_key),
                         view_of(expected.ref.segments[i].first_key)));
        CHECK(same_bytes(view_of(actual.ref.segments[i].last_key),
                         view_of(expected.ref.segments[i].last_key)));
    }
}

void check_index_equal(const FileIndex& actual, const FileIndex& expected) {
    REQUIRE(actual.namespaces.size() == expected.namespaces.size());
    REQUIRE(actual.scopes.size() == expected.scopes.size());
    REQUIRE(actual.copies.size() == expected.copies.size());
    REQUIRE(actual.drops.size() == expected.drops.size());
    REQUIRE(actual.erases.size() == expected.erases.size());
    REQUIRE(actual.extent_refs.size() == expected.extent_refs.size());
    CHECK(actual.last_sequence == expected.last_sequence);
    for (std::size_t i = 0; i < actual.namespaces.size(); ++i) {
        check_namespace(actual.namespaces[i], expected.namespaces[i]);
    }
    for (std::size_t i = 0; i < actual.scopes.size(); ++i) {
        check_scope(actual.scopes[i], expected.scopes[i]);
    }
    for (std::size_t i = 0; i < actual.copies.size(); ++i) {
        check_copy(actual.copies[i], expected.copies[i]);
    }
    for (std::size_t i = 0; i < actual.drops.size(); ++i) {
        check_drop(actual.drops[i], expected.drops[i]);
    }
    for (std::size_t i = 0; i < actual.erases.size(); ++i) {
        check_erase(actual.erases[i], expected.erases[i]);
    }
    for (std::size_t i = 0; i < actual.extent_refs.size(); ++i) {
        check_extent_ref(actual.extent_refs[i], expected.extent_refs[i]);
    }
}

void check_index_equal(const DataFileReader& reader,
                       const FileIndex& expected) {
    FileIndex actual = reader.index();
    auto refs = reader.all_extent_refs();
    REQUIRE(refs.has_value());
    actual.extent_refs = std::move(*refs);
    check_index_equal(actual, expected);
}

FileIndex prefix_state(const FileIndex& full, std::size_t commit_count) {
    FileIndex prefix;
    prefix.last_sequence = static_cast<std::uint64_t>(commit_count);
    for (std::size_t i = 0; i < commit_count; ++i) {
        prefix.namespaces.push_back(full.namespaces[i]);
        prefix.scopes.push_back(full.scopes[i]);
        prefix.copies.push_back(full.copies[i]);
        prefix.drops.push_back(full.drops[i]);
        prefix.extent_refs.push_back(full.extent_refs[i]);
    }
    return prefix;
}

FileIndex state_after_sequence(const FileIndex& full, std::uint64_t sequence) {
    FileIndex state;
    state.last_sequence = sequence;
    const std::size_t count = sequence < full.namespaces.size()
                                  ? static_cast<std::size_t>(sequence)
                                  : full.namespaces.size();
    for (std::size_t i = 0; i < count; ++i) {
        state.namespaces.push_back(full.namespaces[i]);
        state.scopes.push_back(full.scopes[i]);
        state.copies.push_back(full.copies[i]);
        state.drops.push_back(full.drops[i]);
        state.extent_refs.push_back(full.extent_refs[i]);
    }
    return state;
}

struct RecordMeta {
    std::uint64_t sequence = 0;
    std::uint64_t offset = 0;
    std::size_t crc_end = 0;
    bool terminal = false;
};

std::uint64_t commit_record_offset(std::uint32_t commit_id);

dstd::Vector<RecordMeta> scan_record_metas(
    const dstd::Vector<std::uint8_t>& file) {
    dstd::Vector<RecordMeta> records;
    for (std::size_t offset = store::engine::FILE_HEADER_BYTES;
         offset + 40U <= file.size();
         offset += store::engine::FILE_HEADER_BYTES) {
        if (std::memcmp(file.data() + offset, "DFTUREC0", 8) != 0) continue;
        const std::uint32_t kind = read_u32_le_at(file, offset + 8U);
        if (kind < 1 || kind > 3) continue;
        const std::uint32_t body_length = read_u32_le_at(file, offset + 12U);
        const std::uint64_t sequence = read_u64_le_at(file, offset + 16U);
        const std::uint64_t self_offset = read_u64_le_at(file, offset + 32U);
        const std::size_t crc_end = offset + 40U + body_length;
        if (sequence == 0 || self_offset != offset ||
            crc_end + 4U > file.size()) {
            continue;
        }
        const std::uint32_t stored_crc = read_u32_le_at(file, crc_end);
        const std::uint32_t crc =
            store::engine::crc32(0, file.data() + offset, crc_end - offset);
        if (stored_crc != crc) continue;
        records.push_back({sequence, offset, crc_end, kind >= 2});
    }
    std::sort(records.begin(), records.end(),
              [](const RecordMeta& a, const RecordMeta& b) {
                  return a.sequence < b.sequence;
              });
    return records;
}

bool chain_is_valid(const dstd::Vector<RecordMeta>& records,
                    std::uint64_t start_sequence,
                    std::uint64_t flipped_sequence) {
    std::uint64_t sequence = start_sequence;
    while (true) {
        const RecordMeta* record = nullptr;
        for (const RecordMeta& candidate : records) {
            if (candidate.sequence == sequence) {
                record = &candidate;
                break;
            }
        }
        if (record == nullptr || record->sequence == flipped_sequence) {
            return false;
        }
        if (record->terminal || record->sequence == 1) return true;
        sequence = record->sequence - 1;
    }
}

std::uint64_t newest_visible_sequence(const dstd::Vector<RecordMeta>& records,
                                      std::uint64_t flipped_sequence) {
    for (std::uint64_t sequence = records.back().sequence; sequence >= 1;
         --sequence) {
        if (chain_is_valid(records, sequence, flipped_sequence)) {
            return sequence;
        }
    }
    return 0;
}

dstd::Vector<std::size_t> record_flip_offsets(const RecordMeta& record) {
    const std::size_t begin = static_cast<std::size_t>(record.offset);
    const std::size_t end = record.crc_end + 4U;
    dstd::Vector<std::size_t> offsets;
    for (std::size_t i = 0; i < 8; ++i) {
        offsets.push_back(begin + ((end - begin) * i) / 8U);
    }
    offsets.push_back(end - 1U);
    return offsets;
}

void add_five_commit(DataFileWriter& writer, FileIndex& expected,
                     std::uint32_t commit_id,
                     dstd::Vector<ExtentRef>* extents = nullptr) {
    const NamespaceItem ns = make_namespace(commit_id);
    const ScopeItem scope = make_scope(commit_id);
    const CopyItem copy = make_copy(commit_id, commit_id, commit_id);
    const DropItem drop = make_drop(commit_id, commit_id, commit_id);
    const SegmentData segment = make_segment(commit_id, 4);
    REQUIRE_FALSE(segment.bytes.empty());

    auto extent = writer.append_extent(view_of(segment.bytes));
    REQUIRE(extent.has_value());
    const ExtentRefItem extent_ref =
        make_extent_ref(*extent, segment, commit_id);

    writer.add(ns);
    writer.add(scope);
    writer.add(copy);
    writer.add(drop);
    writer.add(extent_ref);

    expected.namespaces.push_back(ns);
    expected.scopes.push_back(scope);
    expected.copies.push_back(copy);
    expected.drops.push_back(drop);
    expected.extent_refs.push_back({commit_id, extent_ref});
    if (extents != nullptr) extents->push_back(*extent);

    REQUIRE(writer.commit().has_value());
}

std::uint64_t commit_record_offset(std::uint32_t commit_id) {
    return 2U * static_cast<std::uint64_t>(commit_id) * EXTENT_SIZE;
}

}  // namespace

TEST_SUITE("index store file") {
    TEST_CASE("five commits round-trip through a footer") {
        TempDir temp;
        const dstd::String path = temp.file("footer.dfs");
        const dstd::String host = "host.example";
        const dstd::String label = "rank-0";

        auto created = DataFileWriter::create(path, EXTENT_SIZE, host, label);
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.extent_size = EXTENT_SIZE;
        expected.host = dstd::String(host);
        expected.label = dstd::String(label);
        dstd::Vector<ExtentRef> extents;
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id, &extents);
        }
        expected.last_sequence = 6;
        REQUIRE(writer.close().has_value());

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        CHECK(opened->records_read() == 1);
        const FileIndex& actual = opened->index();
        CHECK(actual.extent_size == EXTENT_SIZE);
        CHECK(dstd::String(actual.host) == host);
        CHECK(dstd::String(actual.label) == label);
        CHECK(actual.pid == static_cast<std::uint32_t>(::getpid()));
        CHECK(actual.writer_random != 0);
        CHECK(actual.creation_time != 0);
        check_index_equal(*opened, expected);

        for (std::size_t i = 0; i < extents.size(); ++i) {
            auto read = opened->read_extent(extents[i]);
            REQUIRE(read.has_value());
            REQUIRE(store::engine::decode_extent(*read).has_value());
            CHECK(same_bytes(
                view_of(*read),
                view_of(
                    make_segment(static_cast<std::uint32_t>(i) + 1, 4).bytes)));
        }
    }

    TEST_CASE("five commits recover from the tail without a footer") {
        TempDir temp;
        const dstd::String path = temp.file("tail.dfs");
        const dstd::String host = "host.example";
        const dstd::String label = "rank-0";

        auto created = DataFileWriter::create(path, EXTENT_SIZE, host, label);
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.extent_size = EXTENT_SIZE;
        expected.host = dstd::String(host);
        expected.label = dstd::String(label);
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 5;

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        CHECK(opened->records_read() == 5);
        check_index_equal(*opened, expected);
    }

    TEST_CASE("200 commits recover through the newest checkpoint") {
        TempDir temp;
        const dstd::String path = temp.file("checkpoints.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        dstd::Vector<NamespaceItem> expected_namespaces;
        dstd::Vector<ExtentRef> expected_extents;
        for (std::uint32_t commit_id = 1; commit_id <= 200; ++commit_id) {
            const NamespaceItem ns = make_namespace(commit_id);
            const dstd::String extent_bytes =
                "extent-" + dstd::to_string(commit_id);
            auto extent = writer.append_extent(view_of(extent_bytes));
            REQUIRE(extent.has_value());
            ExtentRefItem ref;
            ref.offset = extent->offset;
            ref.length = extent->length;
            ref.crc = extent->crc;
            ref.sequence = commit_id;
            writer.add(ns);
            writer.add(ref);
            expected_namespaces.push_back(ns);
            expected_extents.push_back(*extent);
            REQUIRE(writer.commit().has_value());
        }

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        CHECK(opened->records_read() == 9);
        CHECK(opened->index().last_sequence == 200);
        REQUIRE(opened->index().namespaces.size() ==
                expected_namespaces.size());
        auto extent_refs = opened->all_extent_refs();
        REQUIRE(extent_refs.has_value());
        REQUIRE(extent_refs->size() == expected_extents.size());
        for (std::size_t i = 0; i < expected_namespaces.size(); ++i) {
            check_namespace(opened->index().namespaces[i],
                            expected_namespaces[i]);
        }
        CHECK(opened->index().commit_refs.size() == 191);
        for (std::size_t i = 0; i < expected_extents.size(); ++i) {
            const auto& found = std::find_if(
                extent_refs->begin(), extent_refs->end(),
                [i](const auto& ref) { return ref.sequence == i + 1; });
            REQUIRE(found != extent_refs->end());
            CHECK(found->ref.offset == expected_extents[i].offset);
        }
    }

    TEST_CASE(
        "truncated files open as exactly the commit prefix before the cut") {
        TempDir temp;
        const dstd::String path = temp.file("crash.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.extent_size = EXTENT_SIZE;
        expected.host = dstd::String("host");
        expected.label = dstd::String("label");
        dstd::Vector<ExtentRef> expected_extents;
        for (std::uint32_t commit_id = 1; commit_id <= 20; ++commit_id) {
            add_five_commit(writer, expected, commit_id, &expected_extents);
        }
        expected.last_sequence = 20;

        const dstd::Vector<std::uint8_t> original = read_file(path);
        REQUIRE_FALSE(original.empty());

        dstd::Vector<std::size_t> lengths;
        for (std::size_t length = 0; length <= original.size();
             length += EXTENT_SIZE) {
            lengths.push_back(length);
        }
        std::mt19937_64 rng(1234567);
        std::uniform_int_distribution<std::size_t> dist(0, original.size() - 1);
        for (int i = 0; i < 200; ++i) {
            lengths.push_back(dist(rng));
        }

        for (const std::size_t length : lengths) {
            const dstd::String copy_path = temp.file("crash_copy.dfs");
            dstd::Vector<std::uint8_t> truncated(
                original.begin(),
                original.begin() + static_cast<std::ptrdiff_t>(length));
            write_file(copy_path, truncated);

            std::size_t commits_before = 0;
            for (std::uint32_t commit_id = 1; commit_id <= 20; ++commit_id) {
                const std::uint64_t extent_end =
                    commit_record_offset(commit_id) - EXTENT_SIZE +
                    expected_extents[commit_id - 1].length;
                const std::uint64_t record_end =
                    commit_record_offset(commit_id) + EXTENT_SIZE;
                if (extent_end <= length && record_end <= length) {
                    commits_before = commit_id;
                } else {
                    break;
                }
            }

            auto opened = DataFileReader::open(copy_path);
            if (commits_before == 0) {
                REQUIRE_FALSE(opened.has_value());
                CHECK((opened.error().is(dstd::Condition::Parse) ||
                       opened.error().is(dstd::Condition::Io)));
                continue;
            }

            REQUIRE(opened.has_value());
            check_index_equal(*opened, prefix_state(expected, commits_before));
        }
    }

    TEST_CASE("a missing middle record recovers to the exact earlier commit") {
        TempDir temp;
        const dstd::String path = temp.file("missing_middle.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        for (std::uint32_t commit_id = 1; commit_id <= 6; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 6;

        const dstd::Vector<std::uint8_t> original = read_file(path);
        const std::uint64_t record_offset = commit_record_offset(3);
        REQUIRE(record_offset + EXTENT_SIZE <= original.size());

        for (const bool bad_header : {false, true}) {
            dstd::Vector<std::uint8_t> corrupted = original;
            std::fill(
                corrupted.begin() + static_cast<std::ptrdiff_t>(record_offset),
                corrupted.begin() +
                    static_cast<std::ptrdiff_t>(record_offset + EXTENT_SIZE),
                0);
            if (bad_header) corrupt_header(corrupted);

            const dstd::String copy_path = temp.file("missing_middle_copy.dfs");
            write_file(copy_path, corrupted);

            auto opened = DataFileReader::open(copy_path);
            REQUIRE(opened.has_value());
            check_index_equal(*opened, prefix_state(expected, 2));
        }
    }

    TEST_CASE("a record copied inside an extent is not applied") {
        TempDir temp;
        const dstd::String path = temp.file("copied_record.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 5;

        dstd::Vector<std::uint8_t> corrupted = read_file(path);
        const std::uint64_t record_offset = commit_record_offset(1);
        REQUIRE(record_offset + EXTENT_SIZE <= corrupted.size());
        const std::size_t append_offset = corrupted.size();
        corrupted.insert(
            corrupted.end(),
            corrupted.begin() + static_cast<std::ptrdiff_t>(record_offset),
            corrupted.begin() +
                static_cast<std::ptrdiff_t>(record_offset + EXTENT_SIZE));
        REQUIRE(corrupted.size() == append_offset + EXTENT_SIZE);
        corrupt_header(corrupted);

        const dstd::String copy_path = temp.file("copied_record_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        check_index_equal(*opened, expected);
    }

    TEST_CASE("an empty file opens as an empty index with and without close") {
        TempDir temp;
        const dstd::String no_close_path = temp.file("empty_no_close.dfs");
        auto created =
            DataFileWriter::create(no_close_path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());

        auto opened_no_close = DataFileReader::open(no_close_path);
        REQUIRE(opened_no_close.has_value());
        CHECK(opened_no_close->index().extent_size == EXTENT_SIZE);
        CHECK(opened_no_close->index().last_sequence == 0);
        CHECK(opened_no_close->index().namespaces.size() == 0);
        CHECK(opened_no_close->index().scopes.size() == 0);
        CHECK(opened_no_close->index().copies.size() == 0);
        CHECK(opened_no_close->index().drops.size() == 0);
        CHECK(opened_no_close->index().extent_refs.size() == 0);

        const dstd::String close_path = temp.file("empty_close.dfs");
        auto close_created =
            DataFileWriter::create(close_path, EXTENT_SIZE, "host", "label");
        REQUIRE(close_created.has_value());
        REQUIRE(close_created->close().has_value());

        auto opened_close = DataFileReader::open(close_path);
        REQUIRE(opened_close.has_value());
        CHECK(opened_close->index().extent_size == EXTENT_SIZE);
        CHECK(opened_close->index().last_sequence == 1);
        CHECK(opened_close->index().namespaces.size() == 0);
        CHECK(opened_close->index().scopes.size() == 0);
        CHECK(opened_close->index().copies.size() == 0);
        CHECK(opened_close->index().drops.size() == 0);
        CHECK(opened_close->index().extent_refs.size() == 0);
    }

    TEST_CASE("a header with version 1 opens as unsupported") {
        TempDir temp;
        const dstd::String path = temp.file("version.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        REQUIRE(created->close().has_value());

        dstd::Vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(bytes.size() >= 12);
        bytes[8] = 1;
        bytes[9] = 0;
        bytes[10] = 0;
        bytes[11] = 0;
        write_file(path, bytes);

        auto opened = DataFileReader::open(path);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error().is(dstd::Condition::Unsupported));
    }

    TEST_CASE(
        "a writer latches its first write error and writes no duplicates") {
        TempDir temp;
        const dstd::String path = temp.file("latched.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        writer.add(make_namespace(1));
        store::engine::DataFileWriterTestHook::close_fd(writer);

        auto first = writer.commit();
        REQUIRE_FALSE(first.has_value());
        CHECK(first.error().is(dstd::Condition::Io));

        writer.add(make_namespace(2));
        auto second = writer.commit();
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().is(dstd::Condition::Io));

        auto append = writer.append_extent(view_of(dstd::String("never")));
        REQUIRE_FALSE(append.has_value());
        CHECK(append.error().is(dstd::Condition::Io));

        auto closed = writer.close();
        REQUIRE_FALSE(closed.has_value());
        CHECK(closed.error().is(dstd::Condition::Io));

        const dstd::Vector<std::uint8_t> bytes = read_file(path);
        CHECK(bytes.size() == store::engine::FILE_HEADER_BYTES);
    }

    TEST_CASE("a bad header with a checkpoint still scans the full state") {
        TempDir temp;
        const dstd::String path = temp.file("checkpoint_bad_header.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        for (std::uint32_t commit_id = 1; commit_id <= 70; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 70;

        dstd::Vector<std::uint8_t> corrupted = read_file(path);
        corrupt_header(corrupted);
        const dstd::String copy_path =
            temp.file("checkpoint_bad_header_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        check_index_equal(*opened, expected);
    }

    TEST_CASE("garbage and truncated headers open with an error") {
        TempDir temp;

        const dstd::String garbage_path = temp.file("garbage.dfs");
        dstd::Vector<std::uint8_t> garbage(store::engine::FILE_HEADER_BYTES * 2,
                                           'g');
        write_file(garbage_path, garbage);
        auto opened_garbage = DataFileReader::open(garbage_path);
        REQUIRE_FALSE(opened_garbage.has_value());
        CHECK((opened_garbage.error().is(dstd::Condition::Parse) ||
               opened_garbage.error().is(dstd::Condition::Io)));

        const dstd::String valid_path = temp.file("valid.dfs");
        auto created =
            DataFileWriter::create(valid_path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        REQUIRE(created->commit().has_value());
        const dstd::Vector<std::uint8_t> valid = read_file(valid_path);
        REQUIRE(valid.size() > 100);

        const dstd::String short_path = temp.file("short_header.dfs");
        write_file(short_path, dstd::Vector<std::uint8_t>(valid.begin(),
                                                          valid.begin() + 100));
        auto opened_short = DataFileReader::open(short_path);
        REQUIRE_FALSE(opened_short.has_value());
        CHECK((opened_short.error().is(dstd::Condition::Parse) ||
               opened_short.error().is(dstd::Condition::Io)));
    }

    TEST_CASE("a zeroed extent passes open and fails the extent crc") {
        TempDir temp;
        const dstd::String path = temp.file("zero_extent.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        const dstd::String extent_bytes(64, 'p');
        auto extent = writer.append_extent(view_of(extent_bytes));
        REQUIRE(extent.has_value());
        ExtentRefItem ref;
        ref.offset = extent->offset;
        ref.length = extent->length;
        ref.crc = extent->crc;
        ref.sequence = 1;
        writer.add(make_namespace(1));
        writer.add(ref);
        REQUIRE(writer.commit().has_value());

        dstd::Vector<std::uint8_t> corrupted = read_file(path);
        REQUIRE(corrupted.size() >= extent->offset + extent->length);
        std::fill(
            corrupted.begin() + static_cast<std::ptrdiff_t>(extent->offset),
            corrupted.begin() +
                static_cast<std::ptrdiff_t>(extent->offset + extent->length),
            0);

        const dstd::String copy_path = temp.file("zero_extent_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        REQUIRE(opened->index().extent_refs.size() == 1);
        auto read =
            opened->read_extent({opened->index().extent_refs[0].ref.offset,
                                 opened->index().extent_refs[0].ref.length,
                                 opened->index().extent_refs[0].ref.crc});
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().is(dstd::Condition::Parse));
    }

    TEST_CASE("a corrupt header falls back to the boundary scan") {
        TempDir temp;
        const dstd::String path = temp.file("bad_header.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            const NamespaceItem ns = make_namespace(commit_id);
            const dstd::String extent_bytes =
                "extent-" + dstd::to_string(commit_id);
            auto extent = writer.append_extent(view_of(extent_bytes));
            REQUIRE(extent.has_value());
            ExtentRefItem ref;
            ref.offset = extent->offset;
            ref.length = extent->length;
            ref.crc = extent->crc;
            ref.sequence = commit_id;
            writer.add(ns);
            writer.add(ref);
            REQUIRE(writer.commit().has_value());
        }

        dstd::Vector<std::uint8_t> corrupted = read_file(path);
        corrupt_header(corrupted);

        const dstd::String copy_path = temp.file("bad_header_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        CHECK(opened->index().last_sequence == 5);
        CHECK(opened->index().namespaces.size() == 5);
        CHECK(opened->index().extent_refs.size() == 5);
        for (std::size_t i = 0; i < 5; ++i) {
            CHECK(opened->index().extent_refs[i].sequence == i + 1);
        }
    }

    TEST_CASE("a corrupt newest commit uses the previous commit") {
        TempDir temp;
        const dstd::String path = temp.file("bad_tail.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            const NamespaceItem ns = make_namespace(commit_id);
            const dstd::String extent_bytes =
                "extent-" + dstd::to_string(commit_id);
            auto extent = writer.append_extent(view_of(extent_bytes));
            REQUIRE(extent.has_value());
            ExtentRefItem ref;
            ref.offset = extent->offset;
            ref.length = extent->length;
            ref.crc = extent->crc;
            ref.sequence = commit_id;
            writer.add(ns);
            writer.add(ref);
            REQUIRE(writer.commit().has_value());
        }

        dstd::Vector<std::uint8_t> corrupted = read_file(path);
        const std::uint64_t tail = header_tail_offset(corrupted);
        REQUIRE(tail >= store::engine::FILE_HEADER_BYTES);
        REQUIRE(tail + 40 <= corrupted.size());
        corrupted[tail + 20] ^= 1;

        const dstd::String copy_path = temp.file("bad_tail_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        CHECK(opened->index().last_sequence == 4);
        CHECK(opened->index().namespaces.size() == 4);
        CHECK(opened->index().extent_refs.size() == 4);
    }

    TEST_CASE("extents without a commit open as an empty index") {
        TempDir temp;
        const dstd::String path = temp.file("extents_no_commit.dfs");

        {
            auto created =
                DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
            REQUIRE(created.has_value());
            auto& writer = *created;
            REQUIRE(writer.append_extent(view_of(dstd::String(64, 'a')))
                        .has_value());
            REQUIRE(writer.append_extent(view_of(dstd::String(64, 'b')))
                        .has_value());
        }

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        CHECK(opened->index().extent_size == EXTENT_SIZE);
        CHECK(opened->index().last_sequence == 0);
        CHECK(opened->index().namespaces.size() == 0);
        CHECK(opened->index().scopes.size() == 0);
        CHECK(opened->index().copies.size() == 0);
        CHECK(opened->index().drops.size() == 0);
        CHECK(opened->index().extent_refs.size() == 0);
    }

    TEST_CASE("record item counts are bounded by the body size") {
        const std::uint64_t record_offset = store::engine::FILE_HEADER_BYTES;
        const auto decode_with_tracked_peak =
            [](const dstd::Vector<std::uint8_t>& record) {
                memory::PoolHandle pool =
                    memory::Manager::get().general().child(
                        "record_count_bound", memory::PoolKind::Scratch);
                REQUIRE(pool);
                const memory::Config saved = memory::config();
                memory::Config capped = saved;
                const std::size_t live = memory::Manager::get().stats().live;
                capped.hard_cap = live + (1U << 20U);
                if (capped.hard_cap < live) capped.hard_cap = 0;
                memory::configure(capped);
                bool over_cap = false;
                {
                    memory::Scope scope(*pool);
                    try {
                        auto decoded =
                            store::engine::decode_record(view_of(record));
                        CHECK_FALSE(decoded.has_value());
                        if (!decoded.has_value()) {
                            CHECK(decoded.error().is(dstd::Condition::Parse));
                        }
                    } catch (const std::exception&) {
                        over_cap = true;
                    }
                }
                memory::configure(saved);
                if (over_cap) {
                    FAIL("decode_record allocated past its bounded count");
                }
                CHECK(pool->peak() < (1U << 20U));
            };

        {
            ScopeItem scope;
            scope.local_id = 1;
            scope.kind = ScopeKind::Root;
            auto encoded =
                store::engine::encode_record(store::engine::RecordKind::Commit,
                                             1, 0, record_offset, {scope});
            REQUIRE(encoded.has_value());
            dstd::Vector<std::uint8_t> record = std_bytes(*encoded);
            write_u32_le_at(record, first_item_payload_offset(record) + 5U,
                            0xFFFFFFF0U);
            rewrite_record_crc(record, 0);
            decode_with_tracked_peak(bytes_of(view_of(record)));
        }

        {
            ExtentRefItem ref;
            ref.offset = record_offset;
            ref.length = 16;
            ref.crc = 0;
            ref.sequence = 1;
            auto encoded = store::engine::encode_record(
                store::engine::RecordKind::Commit, 1, 0, record_offset, {ref});
            REQUIRE(encoded.has_value());
            dstd::Vector<std::uint8_t> record = std_bytes(*encoded);
            write_u32_le_at(record, first_item_payload_offset(record) + 24U,
                            0xFFFFFFF0U);
            rewrite_record_crc(record, 0);
            decode_with_tracked_peak(bytes_of(view_of(record)));
        }
    }

    TEST_CASE("an erase record round-trips its range") {
        const std::uint64_t record_offset = store::engine::FILE_HEADER_BYTES;
        const auto encode = [&](const EraseItem& erase) {
            auto encoded =
                store::engine::encode_record(store::engine::RecordKind::Commit,
                                             1, 0, record_offset, {erase});
            REQUIRE(encoded.has_value());
            return std_bytes(*encoded);
        };
        const auto decode = [](const dstd::Vector<std::uint8_t>& record) {
            return store::engine::decode_record(
                ByteView{record.data(), record.size()});
        };

        EraseItem item;
        item.copy_id = 3;
        item.ordinal = 9;
        item.begin_key = bytes_of_string("key-b");
        item.end_key = bytes_of_string("key-d");

        {
            auto decoded = decode(encode(item));
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->items.size() == 1);
            const auto* found = std::get_if<EraseItem>(&decoded->items[0]);
            REQUIRE(found != nullptr);
            check_erase(*found, item);
        }

        {
            EraseItem open_end = item;
            open_end.end_key.clear();
            auto decoded = decode(encode(open_end));
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->items.size() == 1);
            const auto* found = std::get_if<EraseItem>(&decoded->items[0]);
            REQUIRE(found != nullptr);
            check_erase(*found, open_end);
            CHECK(found->end_key.empty());
        }

        // The begin length sits at payload offset 12 and does not fit, and the
        // record is the reader's only view of the range.
        {
            dstd::Vector<std::uint8_t> record = encode(item);
            record[first_item_payload_offset(record) + 12U] = 0x7FU;
            rewrite_record_crc(record, 0);
            auto decoded = decode(record);
            CHECK_FALSE(decoded.has_value());
            if (!decoded.has_value()) {
                CHECK(decoded.error().is(dstd::Condition::Parse));
            }
        }

        {
            dstd::Vector<std::uint8_t> record = encode(item);
            write_u32_le_at(record, first_item_payload_offset(record), 0U);
            rewrite_record_crc(record, 0);
            auto decoded = decode(record);
            CHECK_FALSE(decoded.has_value());
            if (!decoded.has_value()) {
                CHECK(decoded.error().is(dstd::Condition::Parse));
            }
        }

        {
            dstd::Vector<std::uint8_t> record = encode(item);
            write_u64_le_at(record, first_item_payload_offset(record) + 4U, 0U);
            rewrite_record_crc(record, 0);
            auto decoded = decode(record);
            CHECK_FALSE(decoded.has_value());
            if (!decoded.has_value()) {
                CHECK(decoded.error().is(dstd::Condition::Parse));
            }
        }
    }

    TEST_CASE("a copy record round-trips its sources") {
        const std::uint64_t record_offset = store::engine::FILE_HEADER_BYTES;
        const auto encode = [&](const CopyItem& copy) {
            auto encoded = store::engine::encode_record(
                store::engine::RecordKind::Commit, 1, 0, record_offset, {copy});
            REQUIRE(encoded.has_value());
            return std_bytes(*encoded);
        };
        const auto decode = [](const dstd::Vector<std::uint8_t>& record) {
            auto decoded = store::engine::decode_record(
                ByteView{record.data(), record.size()});
            return decoded;
        };

        CopyItem item;
        item.local_id = 3;
        item.namespace_id = 2;
        item.scope_id = 7;
        item.params_hash = 0x0102030405060708ULL;
        item.generation = 5;
        item.state = CopyState::Failed;
        // Values with few repeated bytes keep the body raw, so the cases below
        // can edit it in place.
        item.sources.push_back(SourceStat{0x8A3F19C2E4B75D61ULL,
                                          0x1D7E5B93C04A26F8ULL,
                                          0xE6912C4FB8037DA5ULL});
        item.sources.push_back(SourceStat{0x5C08D3A7F1E96B24ULL,
                                          0xB47A21E5096FC3D8ULL,
                                          0x37F6E80B5D2A9C41ULL});

        {
            auto decoded = decode(encode(item));
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->items.size() == 1);
            const auto* found = std::get_if<CopyItem>(&decoded->items[0]);
            REQUIRE(found != nullptr);
            check_copy(*found, item);
        }

        {
            CopyItem bare = item;
            bare.sources.clear();
            auto decoded = decode(encode(bare));
            REQUIRE(decoded.has_value());
            REQUIRE(decoded->items.size() == 1);
            const auto* found = std::get_if<CopyItem>(&decoded->items[0]);
            REQUIRE(found != nullptr);
            check_copy(*found, bare);
            CHECK(found->sources.empty());
        }

        // The source count is at payload offset 29: u32 id, namespace and
        // scope, two u64 and the state byte.
        {
            dstd::Vector<std::uint8_t> record = encode(item);
            write_u32_le_at(record, first_item_payload_offset(record) + 29U,
                            3U);
            rewrite_record_crc(record, 0);
            auto decoded = decode(record);
            CHECK_FALSE(decoded.has_value());
            if (!decoded.has_value()) {
                CHECK(decoded.error().is(dstd::Condition::Parse));
            }
        }

        {
            dstd::Vector<std::uint8_t> record = encode(item);
            write_u32_le_at(record, first_item_payload_offset(record) + 29U,
                            0xFFFFFFF0U);
            rewrite_record_crc(record, 0);
            auto decoded = decode(record);
            CHECK_FALSE(decoded.has_value());
            if (!decoded.has_value()) {
                CHECK(decoded.error().is(dstd::Condition::Parse));
            }
        }
    }

    TEST_CASE("a newest record with a forward prev offset falls back cleanly") {
        TempDir temp;
        const dstd::String path = temp.file("forward_prev.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.extent_size = EXTENT_SIZE;
        expected.host = dstd::String("host");
        expected.label = dstd::String("label");
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 5;

        dstd::Vector<std::uint8_t> corrupted = read_file(path);
        const std::uint64_t tail_offset = commit_record_offset(5);
        const std::uint64_t copied_offset =
            static_cast<std::uint64_t>(corrupted.size());
        const std::uint64_t source_offset = commit_record_offset(4);
        REQUIRE(copied_offset % store::engine::FILE_HEADER_BYTES == 0);
        REQUIRE(source_offset + EXTENT_SIZE <= corrupted.size());
        corrupted.insert(
            corrupted.end(),
            corrupted.begin() + static_cast<std::ptrdiff_t>(source_offset),
            corrupted.begin() +
                static_cast<std::ptrdiff_t>(source_offset + EXTENT_SIZE));
        REQUIRE(corrupted.size() == copied_offset + EXTENT_SIZE);

        write_u64_le_at(corrupted,
                        static_cast<std::size_t>(copied_offset) + 32U,
                        copied_offset);
        rewrite_record_crc(corrupted, copied_offset);

        write_u64_le_at(corrupted, static_cast<std::size_t>(tail_offset) + 24U,
                        copied_offset);
        rewrite_record_crc(corrupted, tail_offset);
        const std::size_t tail_at = header_tail_field_offset(corrupted);
        write_u32_le_at(corrupted, tail_at + 12U,
                        read_u32_le_at(corrupted, record_crc_offset(
                                                      corrupted, tail_offset)));
        rewrite_header_crc(corrupted);

        const dstd::String copy_path = temp.file("forward_prev_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        check_index_equal(*opened, prefix_state(expected, 4));
    }

    TEST_CASE("record bit flips recover to the newest intact chain") {
        TempDir temp;
        const dstd::String path = temp.file("bit_flips.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.extent_size = EXTENT_SIZE;
        expected.host = dstd::String("host");
        expected.label = dstd::String("label");
        for (std::uint32_t commit_id = 1; commit_id <= 70; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 70;

        const dstd::Vector<std::uint8_t> original = read_file(path);
        const dstd::Vector<RecordMeta> records = scan_record_metas(original);
        REQUIRE(records.size() == 70);

        for (const RecordMeta& record : records) {
            if (record.sequence < 64) continue;
            for (const std::size_t flip_at : record_flip_offsets(record)) {
                REQUIRE(flip_at < original.size());
                dstd::Vector<std::uint8_t> corrupted = original;
                corrupted[flip_at] ^= 1;
                const dstd::String copy_path = temp.file("flip_record.dfs");
                write_file(copy_path, corrupted);

                auto opened = DataFileReader::open(copy_path);
                REQUIRE(opened.has_value());
                const std::uint64_t visible_sequence =
                    newest_visible_sequence(records, record.sequence);
                REQUIRE(visible_sequence != 0);
                check_index_equal(
                    *opened, state_after_sequence(expected, visible_sequence));
            }
        }

        for (const IndexedExtentRef& extent_ref : expected.extent_refs) {
            REQUIRE(extent_ref.ref.length != 0);
            const std::size_t begin =
                static_cast<std::size_t>(extent_ref.ref.offset);
            const std::size_t middle = begin + extent_ref.ref.length / 2U;
            const std::size_t end = begin + extent_ref.ref.length - 1U;
            for (const std::size_t flip_at : {begin, middle, end}) {
                REQUIRE(flip_at < original.size());
                dstd::Vector<std::uint8_t> corrupted = original;
                corrupted[flip_at] ^= 1;
                const dstd::String copy_path = temp.file("flip_extent.dfs");
                write_file(copy_path, corrupted);

                auto opened = DataFileReader::open(copy_path);
                REQUIRE(opened.has_value());
                check_index_equal(*opened, state_after_sequence(expected, 70));
            }
        }
    }

    TEST_CASE("torn headers still open the newest commit") {
        TempDir temp;
        const dstd::String path = temp.file("torn_header.dfs");

        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.extent_size = EXTENT_SIZE;
        expected.host = dstd::String("host");
        expected.label = dstd::String("label");
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 5;

        const dstd::Vector<std::uint8_t> current = read_file(path);
        REQUIRE(current.size() >= store::engine::FILE_HEADER_BYTES);
        const std::size_t tail_at = header_tail_field_offset(current);
        const std::uint64_t stale_tail = commit_record_offset(3);
        const std::size_t stale_crc_at = record_crc_offset(current, stale_tail);
        REQUIRE(stale_crc_at + 4 <= current.size());

        dstd::Vector<std::uint8_t> stale_header = current;
        write_u64_le_at(stale_header, tail_at, stale_tail);
        write_u32_le_at(stale_header, tail_at + 8U, 1);
        write_u32_le_at(stale_header, tail_at + 12U,
                        read_u32_le_at(current, stale_crc_at));
        rewrite_header_crc(stale_header);

        const dstd::String stale_path = temp.file("torn_header_stale.dfs");
        write_file(stale_path, stale_header);
        auto opened_stale = DataFileReader::open(stale_path);
        REQUIRE(opened_stale.has_value());
        check_index_equal(*opened_stale, prefix_state(expected, 3));

        dstd::Vector<std::uint8_t> torn = stale_header;
        write_u64_le_at(torn, tail_at, read_u64_le_at(current, tail_at));
        write_u32_le_at(torn, tail_at + 8U,
                        read_u32_le_at(current, tail_at + 8U));
        write_u32_le_at(torn, tail_at + 12U,
                        read_u32_le_at(current, tail_at + 12U));
        const dstd::String torn_path = temp.file("torn_header_new_tail.dfs");
        write_file(torn_path, torn);
        auto opened_torn = DataFileReader::open(torn_path);
        REQUIRE(opened_torn.has_value());
        check_index_equal(*opened_torn, expected);
    }

    TEST_CASE("data file names keep only safe characters") {
        const auto name = store::engine::data_file_name(
            "my host.example_", 42, 0x0123456789ABCDEFULL, "a/b:c+d");
        CHECK(dstd::String(name) ==
              "w-myhost.example_-42-0123456789abcdef-abcd.dfs");
    }

    TEST_CASE("a scope directory finds every scope in scope order") {
        dstd::Vector<NamespaceItem> namespaces(2);
        namespaces[0].local_id = 1;
        namespaces[0].name = dstd::String("b.space");
        namespaces[0].fold_kind = 1;
        namespaces[1].local_id = 2;
        namespaces[1].name = dstd::String("a.space");
        namespaces[1].format_version = 3;

        const dstd::Vector<dstd::Vector<dstd::String>> scope_paths = {
            {"/t/b.pfw"}, {"/t/a.pfw"}, {"/t/a.pfw", "/t/c.pfw"}, {"/t/a"}};
        dstd::Vector<ScopeItem> scopes;
        for (std::size_t i = 0; i < scope_paths.size(); ++i) {
            ScopeItem scope;
            scope.local_id = static_cast<std::uint32_t>(10 + i);
            scope.kind =
                scope_paths[i].size() > 1 ? ScopeKind::Files : ScopeKind::File;
            for (const dstd::String& path : scope_paths[i]) {
                scope.paths.push_back(dstd::String(path));
            }
            scopes.push_back(std::move(scope));
        }
        dstd::Vector<CopyItem> copies;
        for (std::uint32_t i = 0; i < 4; ++i) {
            copies.push_back(
                {100 + i, 1, 10 + i, 7, i + 1, CopyState::Ready, {}});
        }
        copies.push_back({200, 2, 10, 0, 5, CopyState::Failed, {}});
        copies.push_back({201, 2, 99, 0, 6, CopyState::Ready, {}});
        dstd::Vector<DropItem> drops;
        drops.push_back({1, 10, 9, {}, {}});

        const dstd::String low = "k0";
        const dstd::String mid = "k5";
        const dstd::String high = "k9";
        dstd::Vector<EraseItem> erases;
        EraseItem erase;
        erase.copy_id = 100;
        erase.ordinal = 3;
        erase.begin_key = bytes_of_string(mid);
        erase.end_key = bytes_of_string(high);
        erases.push_back(std::move(erase));
        erases.push_back({999, 4, bytes_of_string(low), bytes_of_string(mid)});
        const dstd::Vector<store::engine::DirectoryCommit> commits = {
            {4, 100, view_of(mid), view_of(high), {}},
            {4, 100, view_of(low), view_of(mid), {}},
            {2, 100, view_of(low), view_of(low), {}},
            {3, 200, view_of(mid), view_of(mid), {}}};
        auto built = store::engine::build_scope_directory(
            namespaces, scopes, copies, drops, erases,
            dstd::Span<const store::engine::DirectoryCommit>(commits.data(),
                                                             commits.size()));
        REQUIRE(built.has_value());
        auto directory = store::engine::ScopeDirectory::open(view_of(*built));
        REQUIRE(directory.has_value());
        REQUIRE(directory->size() == 5);

        const dstd::Vector<dstd::Pair<dstd::String, dstd::Vector<dstd::String>>>
            order = {{"a.space", {"/t/b.pfw"}},
                     {"b.space", {"/t/a"}},
                     {"b.space", {"/t/a.pfw"}},
                     {"b.space", {"/t/b.pfw"}},
                     {"b.space", {"/t/a.pfw", "/t/c.pfw"}}};
        for (std::size_t i = 0; i < order.size(); ++i) {
            auto key = directory->key(i);
            REQUIRE(key.has_value());
            CHECK(dstd::String(key->name) == order[i].first);
            REQUIRE(key->paths.size() == order[i].second.size());
            for (std::size_t p = 0; p < key->paths.size(); ++p) {
                CHECK(dstd::String(key->paths[p]) == order[i].second[p]);
            }
            dstd::Vector<dstd::String> paths;
            for (const dstd::String& path : order[i].second) {
                paths.push_back(dstd::String(path));
            }
            CHECK(directory->find(key->name, key->format_version, key->kind,
                                  paths) == i);
        }
        CHECK(directory->namespace_range("a.space", 3) ==
              dstd::Pair<std::size_t, std::size_t>{0, 1});
        CHECK(directory->namespace_range("b.space", 0) ==
              dstd::Pair<std::size_t, std::size_t>{1, 5});
        CHECK(directory->namespace_range("a.space", 0).first ==
              directory->namespace_range("a.space", 0).second);
        dstd::Vector<dstd::String> absent;
        absent.push_back(dstd::String("/t/z.pfw"));
        CHECK(directory->find("b.space", 0, ScopeKind::File, absent) ==
              directory->size());

        auto entry = directory->entry(3);
        REQUIRE(entry.has_value());
        CHECK(entry->highest_generation == 9);
        REQUIRE(entry->copies.size() == 1);
        const store::engine::DirectoryCopy& copy = entry->copies[0];
        CHECK(copy.local_id == 100);
        CHECK(copy.generation == 1);
        CHECK(copy.params_hash == 7);
        CHECK(copy.fold_kind == 1);
        REQUIRE(copy.commits.size() == 2);
        CHECK(copy.commits[0].sequence == 2);
        CHECK(copy.commits[1].sequence == 4);
        CHECK(same_bytes(copy.commits[1].first_key, view_of(low)));
        CHECK(same_bytes(copy.commits[1].last_key, view_of(high)));
        REQUIRE(entry->drops.size() == 1);
        CHECK(entry->drops[0].generation == 9);
        REQUIRE(copy.erases.size() == 1);
        CHECK(copy.erases[0].ordinal == 3);
        CHECK(same_bytes(copy.erases[0].begin, view_of(mid)));
        CHECK(same_bytes(copy.erases[0].end, view_of(high)));

        auto other = directory->entry(0);
        REQUIRE(other.has_value());
        REQUIRE(other->copies.size() == 1);
        CHECK(other->copies[0].state == CopyState::Failed);
        CHECK(other->copies[0].commits.size() == 1);
    }

    TEST_CASE("a scope directory keeps the sources of each copy") {
        dstd::Vector<NamespaceItem> namespaces(1);
        namespaces[0] = make_namespace(1);
        dstd::Vector<ScopeItem> scopes(1);
        scopes[0].local_id = 10;
        scopes[0].kind = ScopeKind::Files;
        scopes[0].paths.push_back(dstd::String("/t/a.pfw"));
        scopes[0].paths.push_back(dstd::String("/t/b.pfw"));

        dstd::Vector<CopyItem> copies;
        copies.push_back({100, 1, 10, 5, 1, CopyState::Ready, {}});
        copies.back().sources.push_back(SourceStat{11, 12, 13});
        copies.back().sources.push_back(SourceStat{21, 22, 23});
        copies.push_back({101, 1, 10, 6, 2, CopyState::Failed, {}});

        auto built = store::engine::build_scope_directory(namespaces, scopes,
                                                          copies, {}, {}, {});
        REQUIRE(built.has_value());
        auto directory = store::engine::ScopeDirectory::open(view_of(*built));
        REQUIRE(directory.has_value());
        auto entry = directory->entry(0);
        REQUIRE(entry.has_value());
        REQUIRE(entry->copies.size() == 2);
        CHECK(entry->highest_generation == 2);
        REQUIRE(entry->copies[0].sources.size() == 2);
        CHECK(entry->copies[0].sources[0].mtime == 11);
        CHECK(entry->copies[0].sources[0].size == 12);
        CHECK(entry->copies[0].sources[0].hash == 13);
        CHECK(entry->copies[0].sources[1].mtime == 21);
        CHECK(entry->copies[0].sources[1].size == 22);
        CHECK(entry->copies[0].sources[1].hash == 23);
        CHECK(entry->copies[0].params_hash == 5);
        CHECK(entry->copies[1].sources.empty());
        CHECK(entry->copies[1].state == CopyState::Failed);

        dstd::Vector<CopyItem> wrong;
        wrong.push_back({102, 1, 10, 7, 3, CopyState::Ready, {}});
        wrong.back().sources.push_back(SourceStat{1, 2, 3});
        auto rejected = store::engine::build_scope_directory(namespaces, scopes,
                                                             wrong, {}, {}, {});
        REQUIRE_FALSE(rejected.has_value());
        if (!rejected.has_value()) {
            CHECK(rejected.error().is(dstd::Condition::InvalidArgument));
        }
    }

    TEST_CASE("a scope directory whose slot leaves its bytes does not open") {
        dstd::Vector<NamespaceItem> namespaces(1);
        namespaces[0].local_id = 1;
        namespaces[0].name = dstd::String("ns");
        dstd::Vector<ScopeItem> scopes(1);
        scopes[0].local_id = 2;
        dstd::Vector<CopyItem> copies;
        copies.push_back({3, 1, 2, 0, 1, CopyState::Ready, {}});
        auto built = store::engine::build_scope_directory(namespaces, scopes,
                                                          copies, {}, {}, {});
        REQUIRE(built.has_value());
        CHECK(store::engine::ScopeDirectory::open(view_of(*built)).has_value());
        dstd::Vector<std::uint8_t> broken = *built;
        dstd::write_u32_le(broken.data() + 4 + 12, 1U << 20);
        CHECK_FALSE(
            store::engine::ScopeDirectory::open(view_of(broken)).has_value());
        CHECK(store::engine::ScopeDirectory::open(ByteView{}).has_value());
    }

    TEST_CASE("segment runs pack into blocks of one copy") {
        TempDir temp;
        const dstd::String path = temp.file("segment_blocks.dfs");
        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host.example", "rank-0");
        REQUIRE(created.has_value());
        auto& writer = *created;

        const NamespaceItem ns = make_namespace(1);
        const ScopeItem scope = make_scope(1);
        const std::uint32_t small_copy = 10;
        const std::uint32_t other_copy = 12;
        writer.add(ns);
        writer.add(scope);
        writer.add(make_copy(small_copy, 1, 1));
        writer.add(make_copy(other_copy, 1, 1));

        const SegmentData first = make_segment(1, 4);
        REQUIRE_FALSE(first.bytes.empty());
        auto first_extent = writer.append_extent(view_of(first.bytes));
        REQUIRE(first_extent.has_value());
        ExtentRefItem first_item =
            make_extent_ref(*first_extent, first, small_copy);
        first_item.sequence = 1;
        writer.add(std::move(first_item));
        REQUIRE(writer.commit().has_value());

        const SegmentData second = make_segment(2, 4);
        REQUIRE_FALSE(second.bytes.empty());
        auto second_extent = writer.append_extent(view_of(second.bytes));
        REQUIRE(second_extent.has_value());
        ExtentRefItem second_item =
            make_extent_ref(*second_extent, second, small_copy);
        second_item.sequence = 2;
        writer.add(std::move(second_item));
        REQUIRE(writer.commit().has_value());

        constexpr std::size_t WIDE_SEGMENTS = 640;
        constexpr std::size_t WIDE_KEY_BYTES = 200;
        dstd::Vector<SegmentData> wide;
        dstd::Vector<ExtentRef> wide_extents;
        wide.reserve(WIDE_SEGMENTS);
        wide_extents.reserve(WIDE_SEGMENTS);
        for (std::size_t i = 0; i < WIDE_SEGMENTS; ++i) {
            wide.push_back(make_wide_segment(static_cast<std::uint32_t>(i),
                                             WIDE_KEY_BYTES));
            REQUIRE_FALSE(wide.back().bytes.empty());
            auto extent = writer.append_extent(view_of(wide.back().bytes));
            REQUIRE(extent.has_value());
            wide_extents.push_back(*extent);
        }
        const std::uint64_t wide_sequence = 3;
        for (std::size_t i = 0; i < WIDE_SEGMENTS; ++i) {
            ExtentRefItem item =
                make_extent_ref(wide_extents[i], wide[i], small_copy);
            item.sequence = wide_sequence;
            writer.add(std::move(item));
        }
        REQUIRE(writer.commit().has_value());

        const SegmentData last = make_segment(3, 4);
        REQUIRE_FALSE(last.bytes.empty());
        auto last_extent = writer.append_extent(view_of(last.bytes));
        REQUIRE(last_extent.has_value());
        ExtentRefItem last_item =
            make_extent_ref(*last_extent, last, other_copy);
        last_item.sequence = 4;
        writer.add(std::move(last_item));
        REQUIRE(writer.commit().has_value());

        REQUIRE(writer.close().has_value());

        auto reader = DataFileReader::open(path);
        REQUIRE(reader.has_value());
        const dstd::Vector<ClosedRun> runs = closed_runs(*reader);

        const std::size_t wide_entry_bytes = 52 + 2 * (2 + WIDE_KEY_BYTES);
        REQUIRE(WIDE_SEGMENTS * wide_entry_bytes >
                store::engine::SEGMENT_BLOCK_BYTES);

        const auto handle_of = [&runs](std::uint32_t copy_id,
                                       std::uint64_t sequence) {
            for (const ClosedRun& run : runs) {
                if (run.copy_id == copy_id && run.sequence == sequence) {
                    return run.handle;
                }
            }
            FAIL("no run of copy " << copy_id << " of commit " << sequence);
            return store::engine::BlockHandle{};
        };
        const store::engine::BlockHandle small_first = handle_of(small_copy, 1);
        const store::engine::BlockHandle small_second =
            handle_of(small_copy, 2);
        const store::engine::BlockHandle wide_handle =
            handle_of(small_copy, wide_sequence);
        const store::engine::BlockHandle other_handle =
            handle_of(other_copy, 4);
        CHECK(small_first.offset == small_second.offset);
        CHECK(small_first.offset != wide_handle.offset);
        CHECK(other_handle.offset != small_first.offset);
        CHECK(other_handle.offset != wide_handle.offset);

        const SegmentBlockView small_block =
            read_segment_block(*reader, small_first);
        REQUIRE(small_block.entries.size() == 2);
        CHECK(small_block.entries[0].sequence == 1);
        CHECK(small_block.entries[1].sequence == 2);
        CHECK(small_block.entries[0].extent_offset == first_extent->offset);
        CHECK(small_block.entries[0].extent_length == first_extent->length);
        CHECK(small_block.entries[0].extent_crc == first_extent->crc);
        CHECK(small_block.entries[0].offset_in_extent == 0);
        CHECK(small_block.entries[0].length == first.bytes.size());
        CHECK(small_block.entries[0].entry_count == first.entry_count);
        CHECK(
            small_block.entries[0].first_key ==
            dstd::String(reinterpret_cast<const char*>(first.first_key.data()),
                         first.first_key.size()));

        const SegmentBlockView wide_block =
            read_segment_block(*reader, wide_handle);
        REQUIRE(wide_block.entries.size() == WIDE_SEGMENTS);
        CHECK(wide_block.raw_bytes > store::engine::SEGMENT_BLOCK_BYTES);
        for (std::size_t i = 0; i < wide_block.entries.size(); ++i) {
            CHECK(wide_block.entries[i].sequence == wide_sequence);
            CHECK(wide_block.entries[i].extent_offset ==
                  wide_extents[i].offset);
            CHECK(wide_block.entries[i].length == wide[i].bytes.size());
        }

        const SegmentBlockView other_block =
            read_segment_block(*reader, other_handle);
        REQUIRE(other_block.entries.size() == 1);
        CHECK(other_block.entries[0].sequence == 4);
        CHECK(other_block.entries[0].extent_offset == last_extent->offset);
        CHECK(other_block.handle.length < store::engine::SEGMENT_BLOCK_BYTES);
    }

    TEST_CASE("a space report accounts for every byte of a closed file") {
        TempDir temp;
        const dstd::String path = temp.file("space.dfs");
        constexpr std::size_t SPACE_EXTENT_SIZE = std::size_t{1} << 20;
        auto created = DataFileWriter::create(path, SPACE_EXTENT_SIZE,
                                              "host.example", "rank-0");
        REQUIRE(created.has_value());
        auto& writer = *created;

        NamespaceItem fold_namespace = make_namespace(3);
        fold_namespace.fold_kind =
            static_cast<std::uint8_t>(store::FoldKind::SumI64);
        writer.add(make_namespace(1));
        writer.add(make_namespace(2));
        writer.add(fold_namespace);
        for (std::uint32_t id = 1; id <= 3; ++id) writer.add(make_scope(id));
        for (std::uint32_t id = 1; id <= 3; ++id) {
            writer.add(make_copy(id, id, id));
        }
        writer.add(make_copy(4, 1, 1));

        const auto segment_entry =
            [](std::uint32_t copy_id, std::uint32_t offset,
               const SegmentData& segment, std::uint64_t ordinal) {
                ExtentSegment entry;
                entry.copy_id = copy_id;
                entry.offset_in_extent = offset;
                entry.length = static_cast<std::uint32_t>(segment.bytes.size());
                entry.entry_count = segment.entry_count;
                entry.ordinal = ordinal;
                entry.first_key = segment.first_key;
                entry.last_key = segment.last_key;
                return entry;
            };
        const auto extent_item = [](const ExtentRef& extent,
                                    std::uint64_t sequence) {
            ExtentRefItem item;
            item.offset = extent.offset;
            item.length = extent.length;
            item.crc = extent.crc;
            item.sequence = sequence;
            return item;
        };

        constexpr std::uint64_t COMMITS = 6;
        dstd::Vector<ExtentRef> written;
        std::uint64_t raw_written = 0;
        for (std::uint64_t sequence = 1; sequence <= COMMITS; ++sequence) {
            const SegmentData left =
                make_segment(static_cast<std::uint32_t>(sequence), 4);
            const SegmentData right =
                make_segment(static_cast<std::uint32_t>(sequence) + 100, 3);
            const SegmentData folded =
                make_segment(static_cast<std::uint32_t>(sequence) + 200, 2);
            REQUIRE_FALSE(left.bytes.empty());
            REQUIRE_FALSE(right.bytes.empty());
            REQUIRE_FALSE(folded.bytes.empty());

            dstd::Vector<std::uint8_t> packed = left.bytes;
            while (packed.size() % store::engine::SEGMENT_ALIGN != 0) {
                packed.push_back(0);
            }
            const std::uint32_t right_at =
                static_cast<std::uint32_t>(packed.size());
            packed.insert(packed.end(), right.bytes.begin(), right.bytes.end());
            auto packed_extent = writer.append_extent(view_of(packed));
            REQUIRE(packed_extent.has_value());
            auto folded_extent = writer.append_extent(view_of(folded.bytes));
            REQUIRE(folded_extent.has_value());
            raw_written += packed.size() + folded.bytes.size();
            written.push_back(*packed_extent);
            written.push_back(*folded_extent);

            ExtentRefItem packed_item = extent_item(*packed_extent, sequence);
            packed_item.segments.push_back(segment_entry(1, 0, left, sequence));
            packed_item.segments.push_back(
                segment_entry(2, right_at, right, sequence));
            writer.add(std::move(packed_item));

            ExtentRefItem folded_item = extent_item(*folded_extent, sequence);
            folded_item.segments.push_back(
                segment_entry(3, 0, folded, sequence));
            writer.add(std::move(folded_item));
            REQUIRE(writer.commit().has_value());
        }

        SegmentBuilder large;
        const dstd::String large_key = "large-key";
        const dstd::String large_value(300U * 1024U, 'x');
        large.add(view_of(large_key), view_of(large_value));
        auto large_bytes = large.seal(nullptr, nullptr, nullptr);
        REQUIRE(large_bytes.has_value());
        SegmentData large_segment;
        large_segment.bytes = std::move(*large_bytes);
        large_segment.entry_count = large.entry_count();
        large_segment.first_key = bytes_of(large.first_key());
        large_segment.last_key = bytes_of(large.last_key());
        auto large_extent = writer.append_extent(view_of(large_segment.bytes));
        REQUIRE(large_extent.has_value());
        raw_written += large_segment.bytes.size();
        written.push_back(*large_extent);
        ExtentRefItem large_item = extent_item(*large_extent, COMMITS + 1);
        large_item.segments.push_back(
            segment_entry(4, 0, large_segment, COMMITS + 1));
        writer.add(std::move(large_item));
        REQUIRE(writer.commit().has_value());
        REQUIRE(writer.close().has_value());

        constexpr std::uint64_t EXPECTED_COMMITS = COMMITS + 1;
        constexpr std::uint64_t EXPECTED_SEGMENT_ENTRIES = 3 * COMMITS + 1;

        auto report = store::engine::space_report(path);
        REQUIRE(report.has_value());
        const store::engine::SpaceReport& space = *report;
        CHECK(space.file_bytes == dstd::fs::file_size(path).value());
        CHECK(space.header_bytes == store::engine::FILE_HEADER_BYTES);
        CHECK(space.header_bytes + space.extent_bytes + space.record_bytes +
                  space.gap_bytes + space.segment_block_bytes +
                  space.items_block_bytes + space.directory_leaf_bytes +
                  space.directory_separator_bytes + space.footer_bytes +
                  space.dict_bytes ==
              space.file_bytes);

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        const FileIndex& index = opened->index();
        REQUIRE(index.tail.has_value());
        REQUIRE(written.size() == index.tail->extent_count);

        std::uint64_t extents = 0;
        std::uint64_t extent_bytes = 0;
        std::uint64_t table_bytes = 0;
        std::uint64_t pages = 0;
        std::uint64_t page_bytes = 0;
        std::uint64_t page_raw_bytes = 0;
        std::uint64_t compressed = 0;
        for (const ExtentRef& ref : written) {
            auto stored = opened->read_extent(ref);
            REQUIRE(stored.has_value());
            auto view = store::engine::ExtentPages::open(view_of(*stored));
            REQUIRE(view.has_value());
            ++extents;
            extent_bytes += ref.length;
            table_bytes +=
                store::engine::ExtentPages::table_length(view_of(*stored)) -
                store::engine::EXTENT_HEADER_BYTES;
            pages += view->size();
            page_raw_bytes += view->raw_length();
            for (std::size_t page = 0; page < view->size(); ++page) {
                page_bytes += view->entry(page).stored_length;
                if (view->entry(page).codec == store::PageCodec::Zstd) {
                    ++compressed;
                }
            }
        }
        CHECK(space.extents == extents);
        CHECK(space.extent_bytes == extent_bytes);
        CHECK(space.extent_page_table_bytes == table_bytes);
        CHECK(space.pages == pages);
        CHECK(space.page_bytes == page_bytes);
        CHECK(space.page_raw_bytes == page_raw_bytes);
        CHECK(space.page_raw_bytes == raw_written);
        CHECK(space.pages_compressed == compressed);
        CHECK(space.pages_compressed > 0);
        CHECK(space.extent_bytes % store::engine::EXTENT_ALIGN_BYTES == 0);
        CHECK(space.extent_bytes ==
              store::engine::EXTENT_HEADER_BYTES * space.extents +
                  space.extent_page_table_bytes + space.page_bytes +
                  space.extent_padding_bytes);

        const dstd::Vector<ClosedRun> runs = closed_runs(*opened);
        CHECK(space.segment_entries == EXPECTED_SEGMENT_ENTRIES);
        CHECK(space.segment_entries == runs.size());
        CHECK(space.segment_key_bytes > 0);
        CHECK(space.segment_block_bytes > 0);
        CHECK(space.items_block_bytes == index.tail->items.length);
        CHECK(space.filter_bytes == index.tail->scope_filter.size());
        CHECK(space.directory_entries == 3);
        CHECK(space.directory_leaf_bytes > 0);
        CHECK(space.directory_separator_bytes == 0);
        CHECK(space.directory_key_bytes > 0);
        CHECK(space.directory_commit_key_bytes > 0);
        CHECK(space.record_bytes ==
              EXPECTED_COMMITS * store::engine::FILE_HEADER_BYTES);
        CHECK(space.commits == EXPECTED_COMMITS);
        CHECK(space.copies == 4);
        CHECK(space.copies == index.copies.size());
        CHECK(space.footer_bytes > store::engine::FILE_TRAILER_BYTES);

        const std::uint64_t blocks =
            space.segment_block_bytes + space.items_block_bytes +
            space.directory_leaf_bytes + space.directory_separator_bytes;
        CHECK(space.gap_bytes == (store::engine::EXTENT_ALIGN_BYTES -
                                  blocks % store::engine::EXTENT_ALIGN_BYTES) %
                                     store::engine::EXTENT_ALIGN_BYTES);
    }

    TEST_CASE("a space report reads every leaf of a directory") {
        TempDir temp;
        const dstd::String path = temp.file("space_leaves.dfs");
        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        constexpr std::uint32_t ENTRIES = 300;
        static_assert(ENTRIES > store::engine::DIRECTORY_BLOCK_ENTRIES);
        for (std::uint32_t id = 1; id <= ENTRIES; ++id) {
            writer.add(make_namespace(id));
            writer.add(make_scope(id));
            writer.add(make_copy(id, id, id));
        }
        REQUIRE(writer.commit().has_value());
        REQUIRE(writer.close().has_value());

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        auto top = store::engine::SlotTable::open(
            view_of(opened->index().tail->top_table));
        REQUIRE(top.has_value());
        CHECK(top->size() == 2);

        auto report = store::engine::space_report(path);
        REQUIRE(report.has_value());
        const store::engine::SpaceReport& space = *report;
        CHECK(space.file_bytes == dstd::fs::file_size(path).value());
        CHECK(space.extents == 0);
        CHECK(space.extent_bytes == 0);
        CHECK(space.pages == 0);
        CHECK(space.segment_entries == 0);
        CHECK(space.directory_entries == ENTRIES);
        CHECK(space.directory_leaf_bytes > 0);
        CHECK(space.directory_separator_bytes == 0);
        CHECK(space.directory_key_bytes > 0);
        CHECK(space.commits == 1);
        CHECK(space.copies == ENTRIES);
        CHECK(space.header_bytes + space.extent_bytes + space.record_bytes +
                  space.gap_bytes + space.segment_block_bytes +
                  space.items_block_bytes + space.directory_leaf_bytes +
                  space.directory_separator_bytes + space.footer_bytes +
                  space.dict_bytes ==
              space.file_bytes);
    }

    TEST_CASE("a space report refuses a file that is not closed") {
        TempDir temp;
        const dstd::String path = temp.file("space_open.dfs");
        auto created =
            DataFileWriter::create(path, EXTENT_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;
        const SegmentData segment = make_segment(1, 2);
        REQUIRE_FALSE(segment.bytes.empty());
        auto extent = writer.append_extent(view_of(segment.bytes));
        REQUIRE(extent.has_value());
        writer.add(make_namespace(1));
        writer.add(make_scope(1));
        writer.add(make_copy(1, 1, 1));
        writer.add(make_extent_ref(*extent, segment, 1));
        REQUIRE(writer.commit().has_value());

        auto report = store::engine::space_report(path);
        REQUIRE_FALSE(report.has_value());
        CHECK(report.error().is(dstd::Condition::Parse));
    }
}
