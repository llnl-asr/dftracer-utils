#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/section.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::index::store::engine {

struct DataFileWriterTestHook {
    static void close_fd(DataFileWriter& writer) {
        if (writer.fd_ >= 0) ::close(writer.fd_);
        writer.fd_ = -1;
    }
};

}  // namespace dftracer::utils::index::store::engine

namespace {

namespace dstd = dftracer::utils::dstd;
namespace engine = dftracer::utils::index::store::engine;
namespace memory = dftracer::utils::memory;
namespace compress = dftracer::utils::utilities::fileio::compress;

using dftracer::utils::ByteView;
using engine::CopyItem;
using engine::CopyState;
using engine::DataFileReader;
using engine::DataFileWriter;
using engine::DropItem;
using engine::FileIndex;
using engine::IndexedPageRef;
using engine::NamespaceItem;
using engine::PageRef;
using engine::PageRefItem;
using engine::PageSection;
using engine::ScopeItem;
using engine::ScopeKind;
using engine::ScopePath;
using engine::SectionBuilder;

constexpr std::size_t PAGE_SIZE = 4096;

ByteView view_of(const std::string& bytes) {
    return {bytes.data(), bytes.size()};
}

ByteView view_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

ByteView view_of(const std::vector<std::uint8_t>& bytes) {
    return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

std::string to_std(const dstd::String& value) {
    return std::string(value.data(), value.size());
}

dstd::String dstring(const std::string& value) {
    return dstd::String(value.data(), value.size());
}

dstd::Vector<std::uint8_t> bytes_of(ByteView bytes) {
    const auto* begin = reinterpret_cast<const std::uint8_t*>(bytes.data());
    return dstd::Vector<std::uint8_t>(begin, begin + bytes.size());
}

dstd::Vector<std::uint8_t> bytes_of_string(const std::string& value) {
    return bytes_of(view_of(value));
}

std::vector<std::uint8_t> std_bytes(const dstd::Vector<std::uint8_t>& bytes) {
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

bool same_bytes(ByteView a, ByteView b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

std::uint32_t read_u32_le_at(const std::vector<std::uint8_t>& bytes,
                             std::size_t offset) {
    const auto* data = bytes.data();
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(data[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(data[offset + 3U]) << 24U);
}

std::uint64_t read_u64_le_at(const std::vector<std::uint8_t>& bytes,
                             std::size_t offset) {
    return static_cast<std::uint64_t>(read_u32_le_at(bytes, offset)) |
           (static_cast<std::uint64_t>(read_u32_le_at(bytes, offset + 4U))
            << 32U);
}

void write_u32_le_at(std::vector<std::uint8_t>& bytes, std::size_t offset,
                     std::uint32_t value) {
    dftracer::utils::write_u32_le(bytes.data() + offset, value);
}

void write_u64_le_at(std::vector<std::uint8_t>& bytes, std::size_t offset,
                     std::uint64_t value) {
    dftracer::utils::write_u64_le(bytes.data() + offset, value);
}

std::size_t header_tail_field_offset(const std::vector<std::uint8_t>& file) {
    const std::uint16_t host_length =
        static_cast<std::uint16_t>(file[44]) |
        (static_cast<std::uint16_t>(file[45]) << 8U);
    const std::uint16_t label_length =
        static_cast<std::uint16_t>(file[46]) |
        (static_cast<std::uint16_t>(file[47]) << 8U);
    return 48U + host_length + label_length;
}

std::uint64_t header_tail_offset(const std::vector<std::uint8_t>& file) {
    return read_u64_le_at(file, header_tail_field_offset(file));
}

void rewrite_header_crc(std::vector<std::uint8_t>& header) {
    REQUIRE(header.size() >= engine::FILE_HEADER_BYTES);
    write_u32_le_at(header, 12, 0);
    const std::uint32_t crc =
        compress::gzip_crc32(0, header.data(), engine::FILE_HEADER_BYTES);
    write_u32_le_at(header, 12, crc);
}

std::size_t record_crc_offset(const std::vector<std::uint8_t>& file,
                              std::uint64_t record_offset) {
    const std::uint32_t body_length =
        read_u32_le_at(file, static_cast<std::size_t>(record_offset) + 12U);
    return static_cast<std::size_t>(record_offset) + 40U + body_length;
}

void rewrite_record_crc(std::vector<std::uint8_t>& file,
                        std::uint64_t record_offset) {
    const std::size_t crc_at = record_crc_offset(file, record_offset);
    const std::size_t record_at = static_cast<std::size_t>(record_offset);
    const std::uint32_t crc =
        compress::gzip_crc32(0, file.data() + record_at, crc_at - record_at);
    write_u32_le_at(file, crc_at, crc);
}

// 40-byte record header plus one-byte varint item tag and length.
std::size_t first_item_payload_offset() { return 42U; }

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

void corrupt_header(std::vector<std::uint8_t>& bytes) {
    REQUIRE(bytes.size() > 16);
    bytes[1] ^= 1;
}

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        const auto now =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("dftu_store_file_" + std::to_string(::getpid()) + "_" +
                std::to_string(now));
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::string file(const std::string& name) const {
        return (path / name).string();
    }
};

struct SectionData {
    dstd::Vector<std::uint8_t> bytes;
    std::uint64_t entry_count = 0;
    dstd::Vector<std::uint8_t> first_key;
    dstd::Vector<std::uint8_t> last_key;
};

SectionData make_section(std::uint32_t seed, std::size_t entry_count) {
    SectionBuilder builder;
    for (std::size_t i = 0; i < entry_count; ++i) {
        const std::string key =
            "key-" + std::to_string(seed) + "-" + std::to_string(i);
        const std::string value(
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
    item.name = dstring("test.ns." + std::to_string(local_id));
    item.format_version = 7;
    item.fold_kind = 0;
    item.codec = 0;
    item.durable = local_id % 2;
    return item;
}

ScopeItem make_scope(std::uint32_t local_id, std::uint32_t namespace_id) {
    ScopeItem item;
    item.local_id = local_id;
    item.kind = local_id % 2 == 0 ? ScopeKind::File : ScopeKind::Files;
    ScopePath path;
    path.logical_path =
        dstring("/traces/run" + std::to_string(local_id) + ".pfw");
    path.mtime = 1000 + local_id;
    path.size = 2000 + local_id;
    path.hash = 3000 + namespace_id;
    item.paths.push_back(std::move(path));
    return item;
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
    return item;
}

DropItem make_drop(std::uint32_t namespace_id, std::uint32_t scope_id,
                   std::uint32_t generation) {
    DropItem item;
    item.namespace_id = namespace_id;
    item.scope_id = scope_id;
    item.generation = generation;
    if (generation % 2 != 0) {
        item.begin_key = bytes_of_string("begin-" + std::to_string(generation));
        item.end_key = bytes_of_string("end-" + std::to_string(generation));
    }
    return item;
}

PageRefItem make_page_ref(const PageRef& page, const SectionData& section,
                          std::uint32_t copy_id) {
    PageRefItem item;
    item.offset = page.offset;
    item.length = page.length;
    item.crc = page.crc;
    item.sequence = copy_id;
    PageSection page_section;
    page_section.copy_id = copy_id;
    page_section.offset_in_page = 0;
    page_section.length = static_cast<std::uint32_t>(section.bytes.size());
    page_section.entry_count = section.entry_count;
    page_section.first_key = section.first_key;
    page_section.last_key = section.last_key;
    item.sections.push_back(std::move(page_section));
    return item;
}

void check_namespace(const NamespaceItem& actual,
                     const NamespaceItem& expected) {
    CHECK(actual.local_id == expected.local_id);
    CHECK(to_std(actual.name) == to_std(expected.name));
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
        CHECK(to_std(actual.paths[i].logical_path) ==
              to_std(expected.paths[i].logical_path));
        CHECK(actual.paths[i].mtime == expected.paths[i].mtime);
        CHECK(actual.paths[i].size == expected.paths[i].size);
        CHECK(actual.paths[i].hash == expected.paths[i].hash);
    }
}

void check_copy(const CopyItem& actual, const CopyItem& expected) {
    CHECK(actual.local_id == expected.local_id);
    CHECK(actual.namespace_id == expected.namespace_id);
    CHECK(actual.scope_id == expected.scope_id);
    CHECK(actual.params_hash == expected.params_hash);
    CHECK(actual.generation == expected.generation);
    CHECK(actual.state == expected.state);
}

void check_drop(const DropItem& actual, const DropItem& expected) {
    CHECK(actual.namespace_id == expected.namespace_id);
    CHECK(actual.scope_id == expected.scope_id);
    CHECK(actual.generation == expected.generation);
    CHECK(same_bytes(view_of(actual.begin_key), view_of(expected.begin_key)));
    CHECK(same_bytes(view_of(actual.end_key), view_of(expected.end_key)));
}

void check_page_ref(const IndexedPageRef& actual,
                    const IndexedPageRef& expected) {
    CHECK(actual.sequence == expected.sequence);
    CHECK(actual.ref.offset == expected.ref.offset);
    CHECK(actual.ref.length == expected.ref.length);
    CHECK(actual.ref.crc == expected.ref.crc);
    REQUIRE(actual.ref.sections.size() == expected.ref.sections.size());
    for (std::size_t i = 0; i < actual.ref.sections.size(); ++i) {
        CHECK(actual.ref.sections[i].copy_id ==
              expected.ref.sections[i].copy_id);
        CHECK(actual.ref.sections[i].offset_in_page ==
              expected.ref.sections[i].offset_in_page);
        CHECK(actual.ref.sections[i].length == expected.ref.sections[i].length);
        CHECK(actual.ref.sections[i].entry_count ==
              expected.ref.sections[i].entry_count);
        CHECK(same_bytes(view_of(actual.ref.sections[i].first_key),
                         view_of(expected.ref.sections[i].first_key)));
        CHECK(same_bytes(view_of(actual.ref.sections[i].last_key),
                         view_of(expected.ref.sections[i].last_key)));
    }
}

void check_index_equal(const FileIndex& actual, const FileIndex& expected) {
    REQUIRE(actual.namespaces.size() == expected.namespaces.size());
    REQUIRE(actual.scopes.size() == expected.scopes.size());
    REQUIRE(actual.copies.size() == expected.copies.size());
    REQUIRE(actual.drops.size() == expected.drops.size());
    REQUIRE(actual.page_refs.size() == expected.page_refs.size());
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
    for (std::size_t i = 0; i < actual.page_refs.size(); ++i) {
        check_page_ref(actual.page_refs[i], expected.page_refs[i]);
    }
}

FileIndex prefix_state(const FileIndex& full, std::size_t commit_count) {
    FileIndex prefix;
    prefix.last_sequence = static_cast<std::uint64_t>(commit_count);
    for (std::size_t i = 0; i < commit_count; ++i) {
        prefix.namespaces.push_back(full.namespaces[i]);
        prefix.scopes.push_back(full.scopes[i]);
        prefix.copies.push_back(full.copies[i]);
        prefix.drops.push_back(full.drops[i]);
        prefix.page_refs.push_back(full.page_refs[i]);
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
        state.page_refs.push_back(full.page_refs[i]);
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

std::vector<RecordMeta> scan_record_metas(
    const std::vector<std::uint8_t>& file) {
    std::vector<RecordMeta> records;
    for (std::size_t offset = engine::FILE_HEADER_BYTES;
         offset + 40U <= file.size(); offset += engine::FILE_HEADER_BYTES) {
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
            compress::gzip_crc32(0, file.data() + offset, crc_end - offset);
        if (stored_crc != crc) continue;
        records.push_back({sequence, offset, crc_end, kind >= 2});
    }
    std::sort(records.begin(), records.end(),
              [](const RecordMeta& a, const RecordMeta& b) {
                  return a.sequence < b.sequence;
              });
    return records;
}

bool chain_is_valid(const std::vector<RecordMeta>& records,
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

std::uint64_t newest_visible_sequence(const std::vector<RecordMeta>& records,
                                      std::uint64_t flipped_sequence) {
    for (std::uint64_t sequence = records.back().sequence; sequence >= 1;
         --sequence) {
        if (chain_is_valid(records, sequence, flipped_sequence)) {
            return sequence;
        }
    }
    return 0;
}

std::vector<std::size_t> record_flip_offsets(const RecordMeta& record) {
    const std::size_t begin = static_cast<std::size_t>(record.offset);
    const std::size_t end = record.crc_end + 4U;
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < 8; ++i) {
        offsets.push_back(begin + ((end - begin) * i) / 8U);
    }
    offsets.push_back(end - 1U);
    return offsets;
}

void add_five_commit(DataFileWriter& writer, FileIndex& expected,
                     std::uint32_t commit_id,
                     dstd::Vector<PageRef>* pages = nullptr) {
    const NamespaceItem ns = make_namespace(commit_id);
    const ScopeItem scope = make_scope(commit_id, commit_id);
    const CopyItem copy = make_copy(commit_id, commit_id, commit_id);
    const DropItem drop = make_drop(commit_id, commit_id, commit_id);
    const SectionData section = make_section(commit_id, 4);
    REQUIRE_FALSE(section.bytes.empty());

    auto page = writer.append_page(view_of(section.bytes));
    REQUIRE(page.has_value());
    const PageRefItem page_ref = make_page_ref(*page, section, commit_id);

    writer.add(ns);
    writer.add(scope);
    writer.add(copy);
    writer.add(drop);
    writer.add(page_ref);

    expected.namespaces.push_back(ns);
    expected.scopes.push_back(scope);
    expected.copies.push_back(copy);
    expected.drops.push_back(drop);
    expected.page_refs.push_back({commit_id, page_ref});
    if (pages != nullptr) pages->push_back(*page);

    REQUIRE(writer.commit().has_value());
}

std::uint64_t commit_record_offset(std::uint32_t commit_id) {
    return 2U * static_cast<std::uint64_t>(commit_id) * PAGE_SIZE;
}

}  // namespace

TEST_SUITE("index store file") {
    TEST_CASE("five commits round-trip through a footer") {
        TempDir temp;
        const std::string path = temp.file("footer.dfs");
        const std::string host = "host.example";
        const std::string label = "rank-0";

        auto created = DataFileWriter::create(path, PAGE_SIZE, host, label);
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.page_size = PAGE_SIZE;
        expected.host = dstring(host);
        expected.label = dstring(label);
        dstd::Vector<PageRef> pages;
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id, &pages);
        }
        expected.last_sequence = 6;
        REQUIRE(writer.close().has_value());

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        CHECK(opened->records_read() == 1);
        const FileIndex& actual = opened->index();
        CHECK(actual.page_size == PAGE_SIZE);
        CHECK(to_std(actual.host) == host);
        CHECK(to_std(actual.label) == label);
        CHECK(actual.pid == static_cast<std::uint32_t>(::getpid()));
        CHECK(actual.writer_random != 0);
        CHECK(actual.creation_time != 0);
        check_index_equal(actual, expected);

        for (std::size_t i = 0; i < pages.size(); ++i) {
            auto read = opened->read_page(pages[i]);
            REQUIRE(read.has_value());
            CHECK(same_bytes(
                view_of(*read),
                view_of(
                    make_section(static_cast<std::uint32_t>(i) + 1, 4).bytes)));
        }
    }

    TEST_CASE("five commits recover from the tail without a footer") {
        TempDir temp;
        const std::string path = temp.file("tail.dfs");
        const std::string host = "host.example";
        const std::string label = "rank-0";

        auto created = DataFileWriter::create(path, PAGE_SIZE, host, label);
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.page_size = PAGE_SIZE;
        expected.host = dstring(host);
        expected.label = dstring(label);
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 5;

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        CHECK(opened->records_read() == 5);
        check_index_equal(opened->index(), expected);
    }

    TEST_CASE("200 commits recover through the newest checkpoint") {
        TempDir temp;
        const std::string path = temp.file("checkpoints.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        dstd::Vector<NamespaceItem> expected_namespaces;
        dstd::Vector<PageRef> expected_pages;
        for (std::uint32_t commit_id = 1; commit_id <= 200; ++commit_id) {
            const NamespaceItem ns = make_namespace(commit_id);
            const std::string page_bytes = "page-" + std::to_string(commit_id);
            auto page = writer.append_page(view_of(page_bytes));
            REQUIRE(page.has_value());
            PageRefItem ref;
            ref.offset = page->offset;
            ref.length = page->length;
            ref.crc = page->crc;
            ref.sequence = commit_id;
            writer.add(ns);
            writer.add(ref);
            expected_namespaces.push_back(ns);
            expected_pages.push_back(*page);
            REQUIRE(writer.commit().has_value());
        }

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        CHECK(opened->records_read() == 9);
        CHECK(opened->index().last_sequence == 200);
        REQUIRE(opened->index().namespaces.size() ==
                expected_namespaces.size());
        REQUIRE(opened->index().page_refs.size() == expected_pages.size());
        for (std::size_t i = 0; i < expected_namespaces.size(); ++i) {
            check_namespace(opened->index().namespaces[i],
                            expected_namespaces[i]);
        }
        for (std::size_t i = 0; i < expected_pages.size(); ++i) {
            CHECK(opened->index().page_refs[i].sequence == i + 1);
            CHECK(opened->index().page_refs[i].ref.offset ==
                  expected_pages[i].offset);
        }
    }

    TEST_CASE(
        "truncated files open as exactly the commit prefix before the cut") {
        TempDir temp;
        const std::string path = temp.file("crash.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.page_size = PAGE_SIZE;
        expected.host = dstring("host");
        expected.label = dstring("label");
        dstd::Vector<PageRef> expected_pages;
        for (std::uint32_t commit_id = 1; commit_id <= 20; ++commit_id) {
            add_five_commit(writer, expected, commit_id, &expected_pages);
        }
        expected.last_sequence = 20;

        const std::vector<std::uint8_t> original = read_file(path);
        REQUIRE_FALSE(original.empty());

        std::vector<std::size_t> lengths;
        for (std::size_t length = 0; length <= original.size();
             length += PAGE_SIZE) {
            lengths.push_back(length);
        }
        std::mt19937_64 rng(1234567);
        std::uniform_int_distribution<std::size_t> dist(0, original.size() - 1);
        for (int i = 0; i < 200; ++i) {
            lengths.push_back(dist(rng));
        }

        for (const std::size_t length : lengths) {
            const std::string copy_path = temp.file("crash_copy.dfs");
            std::vector<std::uint8_t> truncated(
                original.begin(),
                original.begin() + static_cast<std::ptrdiff_t>(length));
            write_file(copy_path, truncated);

            std::size_t commits_before = 0;
            for (std::uint32_t commit_id = 1; commit_id <= 20; ++commit_id) {
                const std::uint64_t page_end =
                    commit_record_offset(commit_id) - PAGE_SIZE +
                    expected_pages[commit_id - 1].length;
                const std::uint64_t record_end =
                    commit_record_offset(commit_id) + PAGE_SIZE;
                if (page_end <= length && record_end <= length) {
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
            check_index_equal(opened->index(),
                              prefix_state(expected, commits_before));
        }
    }

    TEST_CASE("a missing middle record recovers to the exact earlier commit") {
        TempDir temp;
        const std::string path = temp.file("missing_middle.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        for (std::uint32_t commit_id = 1; commit_id <= 6; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 6;

        const std::vector<std::uint8_t> original = read_file(path);
        const std::uint64_t record_offset = commit_record_offset(3);
        REQUIRE(record_offset + PAGE_SIZE <= original.size());

        for (const bool bad_header : {false, true}) {
            std::vector<std::uint8_t> corrupted = original;
            std::fill(
                corrupted.begin() + static_cast<std::ptrdiff_t>(record_offset),
                corrupted.begin() +
                    static_cast<std::ptrdiff_t>(record_offset + PAGE_SIZE),
                0);
            if (bad_header) corrupt_header(corrupted);

            const std::string copy_path = temp.file("missing_middle_copy.dfs");
            write_file(copy_path, corrupted);

            auto opened = DataFileReader::open(copy_path);
            REQUIRE(opened.has_value());
            check_index_equal(opened->index(), prefix_state(expected, 2));
        }
    }

    TEST_CASE("a record copied inside a page is not applied") {
        TempDir temp;
        const std::string path = temp.file("copied_record.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 5;

        std::vector<std::uint8_t> corrupted = read_file(path);
        const std::uint64_t record_offset = commit_record_offset(1);
        REQUIRE(record_offset + PAGE_SIZE <= corrupted.size());
        const std::size_t append_offset = corrupted.size();
        corrupted.insert(
            corrupted.end(),
            corrupted.begin() + static_cast<std::ptrdiff_t>(record_offset),
            corrupted.begin() +
                static_cast<std::ptrdiff_t>(record_offset + PAGE_SIZE));
        REQUIRE(corrupted.size() == append_offset + PAGE_SIZE);
        corrupt_header(corrupted);

        const std::string copy_path = temp.file("copied_record_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        check_index_equal(opened->index(), expected);
    }

    TEST_CASE("an empty file opens as an empty index with and without close") {
        TempDir temp;
        const std::string no_close_path = temp.file("empty_no_close.dfs");
        auto created =
            DataFileWriter::create(no_close_path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());

        auto opened_no_close = DataFileReader::open(no_close_path);
        REQUIRE(opened_no_close.has_value());
        CHECK(opened_no_close->index().page_size == PAGE_SIZE);
        CHECK(opened_no_close->index().last_sequence == 0);
        CHECK(opened_no_close->index().namespaces.size() == 0);
        CHECK(opened_no_close->index().scopes.size() == 0);
        CHECK(opened_no_close->index().copies.size() == 0);
        CHECK(opened_no_close->index().drops.size() == 0);
        CHECK(opened_no_close->index().page_refs.size() == 0);

        const std::string close_path = temp.file("empty_close.dfs");
        auto close_created =
            DataFileWriter::create(close_path, PAGE_SIZE, "host", "label");
        REQUIRE(close_created.has_value());
        REQUIRE(close_created->close().has_value());

        auto opened_close = DataFileReader::open(close_path);
        REQUIRE(opened_close.has_value());
        CHECK(opened_close->index().page_size == PAGE_SIZE);
        CHECK(opened_close->index().last_sequence == 1);
        CHECK(opened_close->index().namespaces.size() == 0);
        CHECK(opened_close->index().scopes.size() == 0);
        CHECK(opened_close->index().copies.size() == 0);
        CHECK(opened_close->index().drops.size() == 0);
        CHECK(opened_close->index().page_refs.size() == 0);
    }

    TEST_CASE("a header with version 1 opens as unsupported") {
        TempDir temp;
        const std::string path = temp.file("version.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        REQUIRE(created->close().has_value());

        std::vector<std::uint8_t> bytes = read_file(path);
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
        const std::string path = temp.file("latched.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        writer.add(make_namespace(1));
        engine::DataFileWriterTestHook::close_fd(writer);

        auto first = writer.commit();
        REQUIRE_FALSE(first.has_value());
        CHECK(first.error().is(dstd::Condition::Io));

        writer.add(make_namespace(2));
        auto second = writer.commit();
        REQUIRE_FALSE(second.has_value());
        CHECK(second.error().is(dstd::Condition::Io));

        auto append = writer.append_page(view_of(std::string("never")));
        REQUIRE_FALSE(append.has_value());
        CHECK(append.error().is(dstd::Condition::Io));

        auto closed = writer.close();
        REQUIRE_FALSE(closed.has_value());
        CHECK(closed.error().is(dstd::Condition::Io));

        const std::vector<std::uint8_t> bytes = read_file(path);
        CHECK(bytes.size() == engine::FILE_HEADER_BYTES);
    }

    TEST_CASE("a bad header with a checkpoint still scans the full state") {
        TempDir temp;
        const std::string path = temp.file("checkpoint_bad_header.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        for (std::uint32_t commit_id = 1; commit_id <= 70; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 70;

        std::vector<std::uint8_t> corrupted = read_file(path);
        corrupt_header(corrupted);
        const std::string copy_path =
            temp.file("checkpoint_bad_header_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        check_index_equal(opened->index(), expected);
    }

    TEST_CASE("garbage and truncated headers open with an error") {
        TempDir temp;

        const std::string garbage_path = temp.file("garbage.dfs");
        std::vector<std::uint8_t> garbage(engine::FILE_HEADER_BYTES * 2, 'g');
        write_file(garbage_path, garbage);
        auto opened_garbage = DataFileReader::open(garbage_path);
        REQUIRE_FALSE(opened_garbage.has_value());
        CHECK((opened_garbage.error().is(dstd::Condition::Parse) ||
               opened_garbage.error().is(dstd::Condition::Io)));

        const std::string valid_path = temp.file("valid.dfs");
        auto created =
            DataFileWriter::create(valid_path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        REQUIRE(created->commit().has_value());
        const std::vector<std::uint8_t> valid = read_file(valid_path);
        REQUIRE(valid.size() > 100);

        const std::string short_path = temp.file("short_header.dfs");
        write_file(short_path, std::vector<std::uint8_t>(valid.begin(),
                                                         valid.begin() + 100));
        auto opened_short = DataFileReader::open(short_path);
        REQUIRE_FALSE(opened_short.has_value());
        CHECK((opened_short.error().is(dstd::Condition::Parse) ||
               opened_short.error().is(dstd::Condition::Io)));
    }

    TEST_CASE("a zeroed page passes open and fails the page crc") {
        TempDir temp;
        const std::string path = temp.file("zero_page.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        const std::string page_bytes(64, 'p');
        auto page = writer.append_page(view_of(page_bytes));
        REQUIRE(page.has_value());
        PageRefItem ref;
        ref.offset = page->offset;
        ref.length = page->length;
        ref.crc = page->crc;
        ref.sequence = 1;
        writer.add(make_namespace(1));
        writer.add(ref);
        REQUIRE(writer.commit().has_value());

        std::vector<std::uint8_t> corrupted = read_file(path);
        REQUIRE(corrupted.size() >= page->offset + page->length);
        std::fill(corrupted.begin() + static_cast<std::ptrdiff_t>(page->offset),
                  corrupted.begin() +
                      static_cast<std::ptrdiff_t>(page->offset + page->length),
                  0);

        const std::string copy_path = temp.file("zero_page_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        REQUIRE(opened->index().page_refs.size() == 1);
        auto read = opened->read_page({opened->index().page_refs[0].ref.offset,
                                       opened->index().page_refs[0].ref.length,
                                       opened->index().page_refs[0].ref.crc});
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error().is(dstd::Condition::Parse));
    }

    TEST_CASE("a corrupt header falls back to the boundary scan") {
        TempDir temp;
        const std::string path = temp.file("bad_header.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            const NamespaceItem ns = make_namespace(commit_id);
            const std::string page_bytes = "page-" + std::to_string(commit_id);
            auto page = writer.append_page(view_of(page_bytes));
            REQUIRE(page.has_value());
            PageRefItem ref;
            ref.offset = page->offset;
            ref.length = page->length;
            ref.crc = page->crc;
            ref.sequence = commit_id;
            writer.add(ns);
            writer.add(ref);
            REQUIRE(writer.commit().has_value());
        }

        std::vector<std::uint8_t> corrupted = read_file(path);
        corrupt_header(corrupted);

        const std::string copy_path = temp.file("bad_header_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        CHECK(opened->index().last_sequence == 5);
        CHECK(opened->index().namespaces.size() == 5);
        CHECK(opened->index().page_refs.size() == 5);
        for (std::size_t i = 0; i < 5; ++i) {
            CHECK(opened->index().page_refs[i].sequence == i + 1);
        }
    }

    TEST_CASE("a corrupt newest commit uses the previous commit") {
        TempDir temp;
        const std::string path = temp.file("bad_tail.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            const NamespaceItem ns = make_namespace(commit_id);
            const std::string page_bytes = "page-" + std::to_string(commit_id);
            auto page = writer.append_page(view_of(page_bytes));
            REQUIRE(page.has_value());
            PageRefItem ref;
            ref.offset = page->offset;
            ref.length = page->length;
            ref.crc = page->crc;
            ref.sequence = commit_id;
            writer.add(ns);
            writer.add(ref);
            REQUIRE(writer.commit().has_value());
        }

        std::vector<std::uint8_t> corrupted = read_file(path);
        const std::uint64_t tail = header_tail_offset(corrupted);
        REQUIRE(tail >= engine::FILE_HEADER_BYTES);
        REQUIRE(tail + 40 <= corrupted.size());
        corrupted[tail + 20] ^= 1;

        const std::string copy_path = temp.file("bad_tail_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        CHECK(opened->index().last_sequence == 4);
        CHECK(opened->index().namespaces.size() == 4);
        CHECK(opened->index().page_refs.size() == 4);
    }

    TEST_CASE("pages without a commit open as an empty index") {
        TempDir temp;
        const std::string path = temp.file("pages_no_commit.dfs");

        {
            auto created =
                DataFileWriter::create(path, PAGE_SIZE, "host", "label");
            REQUIRE(created.has_value());
            auto& writer = *created;
            REQUIRE(
                writer.append_page(view_of(std::string(64, 'a'))).has_value());
            REQUIRE(
                writer.append_page(view_of(std::string(64, 'b'))).has_value());
        }

        auto opened = DataFileReader::open(path);
        REQUIRE(opened.has_value());
        CHECK(opened->index().page_size == PAGE_SIZE);
        CHECK(opened->index().last_sequence == 0);
        CHECK(opened->index().namespaces.size() == 0);
        CHECK(opened->index().scopes.size() == 0);
        CHECK(opened->index().copies.size() == 0);
        CHECK(opened->index().drops.size() == 0);
        CHECK(opened->index().page_refs.size() == 0);
    }

    TEST_CASE("record item counts are bounded by the body size") {
        const std::uint64_t record_offset = engine::FILE_HEADER_BYTES;
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
                        auto decoded = engine::decode_record(view_of(record));
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
            auto encoded = engine::encode_record(engine::RecordKind::Commit, 1,
                                                 0, record_offset, {scope});
            REQUIRE(encoded.has_value());
            std::vector<std::uint8_t> record = std_bytes(*encoded);
            write_u32_le_at(record, first_item_payload_offset() + 5U,
                            0xFFFFFFF0U);
            rewrite_record_crc(record, 0);
            decode_with_tracked_peak(bytes_of(view_of(record)));
        }

        {
            PageRefItem ref;
            ref.offset = record_offset;
            ref.length = 16;
            ref.crc = 0;
            ref.sequence = 1;
            auto encoded = engine::encode_record(engine::RecordKind::Commit, 1,
                                                 0, record_offset, {ref});
            REQUIRE(encoded.has_value());
            std::vector<std::uint8_t> record = std_bytes(*encoded);
            write_u32_le_at(record, first_item_payload_offset() + 24U,
                            0xFFFFFFF0U);
            rewrite_record_crc(record, 0);
            decode_with_tracked_peak(bytes_of(view_of(record)));
        }
    }

    TEST_CASE("a newest record with a forward prev offset falls back cleanly") {
        TempDir temp;
        const std::string path = temp.file("forward_prev.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.page_size = PAGE_SIZE;
        expected.host = dstring("host");
        expected.label = dstring("label");
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 5;

        std::vector<std::uint8_t> corrupted = read_file(path);
        const std::uint64_t tail_offset = commit_record_offset(5);
        const std::uint64_t copied_offset =
            static_cast<std::uint64_t>(corrupted.size());
        const std::uint64_t source_offset = commit_record_offset(4);
        REQUIRE(copied_offset % engine::FILE_HEADER_BYTES == 0);
        REQUIRE(source_offset + PAGE_SIZE <= corrupted.size());
        corrupted.insert(
            corrupted.end(),
            corrupted.begin() + static_cast<std::ptrdiff_t>(source_offset),
            corrupted.begin() +
                static_cast<std::ptrdiff_t>(source_offset + PAGE_SIZE));
        REQUIRE(corrupted.size() == copied_offset + PAGE_SIZE);

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

        const std::string copy_path = temp.file("forward_prev_copy.dfs");
        write_file(copy_path, corrupted);

        auto opened = DataFileReader::open(copy_path);
        REQUIRE(opened.has_value());
        check_index_equal(opened->index(), prefix_state(expected, 4));
    }

    TEST_CASE("record bit flips recover to the newest intact chain") {
        TempDir temp;
        const std::string path = temp.file("bit_flips.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.page_size = PAGE_SIZE;
        expected.host = dstring("host");
        expected.label = dstring("label");
        for (std::uint32_t commit_id = 1; commit_id <= 70; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 70;

        const std::vector<std::uint8_t> original = read_file(path);
        const std::vector<RecordMeta> records = scan_record_metas(original);
        REQUIRE(records.size() == 70);

        for (const RecordMeta& record : records) {
            if (record.sequence < 64) continue;
            for (const std::size_t flip_at : record_flip_offsets(record)) {
                REQUIRE(flip_at < original.size());
                std::vector<std::uint8_t> corrupted = original;
                corrupted[flip_at] ^= 1;
                const std::string copy_path = temp.file("flip_record.dfs");
                write_file(copy_path, corrupted);

                auto opened = DataFileReader::open(copy_path);
                REQUIRE(opened.has_value());
                const std::uint64_t visible_sequence =
                    newest_visible_sequence(records, record.sequence);
                REQUIRE(visible_sequence != 0);
                check_index_equal(
                    opened->index(),
                    state_after_sequence(expected, visible_sequence));
            }
        }

        for (const IndexedPageRef& page_ref : expected.page_refs) {
            REQUIRE(page_ref.ref.length != 0);
            const std::size_t begin =
                static_cast<std::size_t>(page_ref.ref.offset);
            const std::size_t middle = begin + page_ref.ref.length / 2U;
            const std::size_t end = begin + page_ref.ref.length - 1U;
            for (const std::size_t flip_at : {begin, middle, end}) {
                REQUIRE(flip_at < original.size());
                std::vector<std::uint8_t> corrupted = original;
                corrupted[flip_at] ^= 1;
                const std::string copy_path = temp.file("flip_page.dfs");
                write_file(copy_path, corrupted);

                auto opened = DataFileReader::open(copy_path);
                REQUIRE(opened.has_value());
                check_index_equal(opened->index(),
                                  state_after_sequence(expected, 70));
            }
        }
    }

    TEST_CASE("torn headers still open the newest commit") {
        TempDir temp;
        const std::string path = temp.file("torn_header.dfs");

        auto created = DataFileWriter::create(path, PAGE_SIZE, "host", "label");
        REQUIRE(created.has_value());
        auto& writer = *created;

        FileIndex expected;
        expected.page_size = PAGE_SIZE;
        expected.host = dstring("host");
        expected.label = dstring("label");
        for (std::uint32_t commit_id = 1; commit_id <= 5; ++commit_id) {
            add_five_commit(writer, expected, commit_id);
        }
        expected.last_sequence = 5;

        const std::vector<std::uint8_t> current = read_file(path);
        REQUIRE(current.size() >= engine::FILE_HEADER_BYTES);
        const std::size_t tail_at = header_tail_field_offset(current);
        const std::uint64_t stale_tail = commit_record_offset(3);
        const std::size_t stale_crc_at = record_crc_offset(current, stale_tail);
        REQUIRE(stale_crc_at + 4 <= current.size());

        std::vector<std::uint8_t> stale_header = current;
        write_u64_le_at(stale_header, tail_at, stale_tail);
        write_u32_le_at(stale_header, tail_at + 8U, 1);
        write_u32_le_at(stale_header, tail_at + 12U,
                        read_u32_le_at(current, stale_crc_at));
        rewrite_header_crc(stale_header);

        const std::string stale_path = temp.file("torn_header_stale.dfs");
        write_file(stale_path, stale_header);
        auto opened_stale = DataFileReader::open(stale_path);
        REQUIRE(opened_stale.has_value());
        check_index_equal(opened_stale->index(), prefix_state(expected, 3));

        std::vector<std::uint8_t> torn = stale_header;
        write_u64_le_at(torn, tail_at, read_u64_le_at(current, tail_at));
        write_u32_le_at(torn, tail_at + 8U,
                        read_u32_le_at(current, tail_at + 8U));
        write_u32_le_at(torn, tail_at + 12U,
                        read_u32_le_at(current, tail_at + 12U));
        const std::string torn_path = temp.file("torn_header_new_tail.dfs");
        write_file(torn_path, torn);
        auto opened_torn = DataFileReader::open(torn_path);
        REQUIRE(opened_torn.has_value());
        check_index_equal(opened_torn->index(), expected);
    }

    TEST_CASE("data file names keep only safe characters") {
        const auto name = engine::data_file_name(
            "my host.example_", 42, 0x0123456789ABCDEFULL, "a/b:c+d");
        CHECK(to_std(name) == "w-myhost.example_-42-0123456789abcdef-abcd.dfs");
    }
}
