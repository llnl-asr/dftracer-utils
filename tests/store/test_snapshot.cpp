// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/store/crc.h>
#include <dftracer/utils/store/data_file.h>
#include <dftracer/utils/store/extent_codec.h>
#include <dftracer/utils/store/fold.h>
#include <dftracer/utils/store/open_extent.h>
#include <dftracer/utils/store/scope_directory.h>
#include <dftracer/utils/store/segment.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/writer.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <mmix_lcg.h>
#include <sys/stat.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <thread>
#include <utility>

using namespace dftracer::utils;

namespace {

using namespace dftracer::utils::store;

// Sets the dictionary sample size and training wait for one test case.
class DictLimits {
   public:
    DictLimits(std::size_t sample_bytes, std::uint64_t wait_bytes)
        : sample_(engine::g_dict_sample_bytes.exchange(sample_bytes)),
          wait_(engine::g_dict_wait_bytes.exchange(wait_bytes)) {}
    ~DictLimits() {
        engine::g_dict_sample_bytes.store(sample_);
        engine::g_dict_wait_bytes.store(wait_);
    }
    DictLimits(const DictLimits&) = delete;
    DictLimits& operator=(const DictLimits&) = delete;

   private:
    std::size_t sample_;
    std::uint64_t wait_;
};

constexpr std::uint32_t SMALL_EXTENT = 4096;
constexpr std::size_t HEADER_WRITER_RANDOM_AT = 32;
constexpr std::size_t HEADER_CRC_AT = 12;
constexpr std::size_t KEY_COUNT = 1000;
constexpr int RUNTIME_THREADS = 8;

dstd::ByteView view_of(const dstd::String& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::ByteView view_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::String to_std(dstd::ByteView bytes) {
    return dstd::String(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size());
}

dstd::String key_of(int index) {
    const dstd::String digits = dstd::to_string(index);
    return "key-" + dstd::String(4 - digits.size(), '0') + digits;
}

dstd::String value_of(int index) { return "value-" + dstd::to_string(index); }

dstd::String i64_bytes(std::int64_t value) {
    dstd::String bytes(8, '\0');
    dstd::write_u64_le(reinterpret_cast<std::uint8_t*>(bytes.data()),
                       static_cast<std::uint64_t>(value));
    return bytes;
}

dstd::String f64_bytes(double value) {
    dstd::String bytes(8, '\0');
    dstd::write_f64_le(reinterpret_cast<std::uint8_t*>(bytes.data()), value);
    return bytes;
}

std::int64_t read_i64(dstd::ByteView bytes) {
    return std::bit_cast<std::int64_t>(
        dstd::read_u64_le(reinterpret_cast<const std::uint8_t*>(bytes.data())));
}

double read_f64(dstd::ByteView bytes) {
    return dstd::read_f64_le(
        reinterpret_cast<const std::uint8_t*>(bytes.data()));
}

NamespaceSpec put_spec(const dstd::String& name, std::uint32_t version = 0) {
    NamespaceSpec spec;
    spec.name = dstd::String(name);
    spec.format_version = version;
    spec.fold = FoldKind::None;
    return spec;
}

NamespaceSpec fold_spec(const dstd::String& name, FoldKind kind) {
    NamespaceSpec spec;
    spec.name = dstd::String(name);
    spec.format_version = 0;
    spec.fold = kind;
    return spec;
}

ScopeKey file_scope(const dstd::String& path) {
    return ScopeKey::file(dstd::String(path));
}

engine::WriterOptions small_extents() {
    engine::WriterOptions options;
    options.extent_size = SMALL_EXTENT;
    options.host = dstd::String("host");
    options.label = dstd::String("label");
    return options;
}

Runtime& store_runtime() {
    static Runtime runtime(RUNTIME_THREADS);
    return runtime;
}

dstd::UniquePtr<engine::Writer> make_writer(
    const dftu_utils_test::ScopedTestDir& dir,
    const engine::WriterOptions& options = small_extents()) {
    auto created =
        engine::Writer::create(dir.str(), *store_runtime().executor(), options);
    REQUIRE(created.has_value());
    return std::move(*created);
}

dstd::Expected<void, dstd::Error> flush_writer(engine::Writer& writer) {
    return store_runtime().submit(writer.flush()).get();
}

dstd::Expected<void, dstd::Error> close_writer(engine::Writer& writer) {
    return store_runtime().submit(writer.close()).get();
}

dstd::Vector<dstd::Pair<dstd::String, dstd::String>> collect(
    store::engine::ScanCursor& cursor) {
    dstd::Vector<dstd::Pair<dstd::String, dstd::String>> rows;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        rows.emplace_back(to_std(cursor.key()), to_std(*value));
        cursor.next();
    }
    return rows;
}

struct ScopedRow {
    dstd::String scope;
    dstd::String key;
    dstd::String value;
};

dstd::Vector<ScopedRow> collect_scoped(store::engine::ScanCursor& cursor) {
    dstd::Vector<ScopedRow> rows;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        REQUIRE_FALSE(cursor.scope().paths.empty());
        rows.push_back({dstd::String(cursor.scope().paths[0]),
                        to_std(cursor.key()), to_std(*value)});
        cursor.next();
    }
    return rows;
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

dstd::Vector<store::engine::IndexedExtentRef> extent_refs_of(
    const engine::DataFileReader& reader) {
    auto refs = reader.all_extent_refs();
    REQUIRE(refs.has_value());
    return dstd::Vector<store::engine::IndexedExtentRef>(refs->begin(),
                                                         refs->end());
}

/// The bytes of a data file without its footer and trailer, as a crash before
/// the close leaves them: the reader recovers the commit chain by a scan, and a
/// commit that was not durable counts as not synced.
dstd::Vector<std::uint8_t> without_footer(dstd::Vector<std::uint8_t> bytes) {
    REQUIRE(bytes.size() >= store::engine::FILE_TRAILER_BYTES);
    const std::uint64_t footer = dstd::read_u64_le(
        bytes.data() + bytes.size() - store::engine::FILE_TRAILER_BYTES + 8U);
    REQUIRE(footer >= store::engine::FILE_HEADER_BYTES);
    REQUIRE(footer < bytes.size());
    bytes.resize(static_cast<std::size_t>(footer));
    return bytes;
}

bool is_data_file(const dstd::Path& path) {
    return path.view().extension() == ".dfs";
}

dstd::String only_data_file(const dstd::String& dir) {
    dstd::Vector<dstd::String> found;
    for (const auto& entry : dftu_utils_test::list_dir(dir)) {
        if (is_data_file(entry.path))
            found.push_back(dstd::String(entry.path.str()));
    }
    REQUIRE(found.size() == 1);
    return found.front();
}

void drop_summary(const dstd::String& dir) {
    (void)dstd::fs::remove(dstd::Path(dir) /
                           dstd::String(store::engine::SUMMARY_FILE_NAME));
}

dstd::Vector<std::uint8_t> with_tail_version(dstd::Vector<std::uint8_t> bytes,
                                             std::uint32_t version) {
    REQUIRE(bytes.size() >= store::engine::FILE_TRAILER_BYTES);
    const std::uint8_t* trailer =
        bytes.data() + bytes.size() - store::engine::FILE_TRAILER_BYTES;
    const std::uint64_t at = dstd::read_u64_le(trailer + 8U);
    const std::uint32_t length = dstd::read_u32_le(trailer + 16U);
    REQUIRE(at + length <= bytes.size());
    auto footer = store::engine::decode_record(dstd::ByteView{
        reinterpret_cast<const std::byte*>(bytes.data() + at), length});
    REQUIRE(footer.has_value());
    bool found = false;
    for (store::engine::RecordItem& item : footer->items) {
        if (auto* tail = std::get_if<store::engine::FileTailPtr>(&item)) {
            (*tail)->format_version = version;
            found = true;
        }
    }
    REQUIRE(found);
    auto encoded = store::engine::encode_record(footer->kind, footer->sequence,
                                                footer->prev_offset,
                                                footer->offset, footer->items);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() <= length);
    std::copy(encoded->begin(), encoded->end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(at));
    return bytes;
}

void rewrite_header_crc(dstd::Vector<std::uint8_t>& bytes) {
    REQUIRE(bytes.size() >= store::engine::FILE_HEADER_BYTES);
    dstd::write_u32_le(bytes.data() + HEADER_CRC_AT, 0);
    const std::uint32_t crc =
        store::engine::crc32(0, bytes.data(), store::engine::FILE_HEADER_BYTES);
    dstd::write_u32_le(bytes.data() + HEADER_CRC_AT, crc);
}

/// Copies `source` into `target` under `name`, with the header writer id and
/// the header crc rewritten so the copy verifies, and without its footer, so
/// the reader takes the writer id from the header.
dstd::String forge_copy(const dstd::String& source,
                        const dftu_utils_test::ScopedTestDir& target,
                        const dstd::String& name, std::uint64_t writer_random) {
    dstd::Vector<std::uint8_t> bytes = read_file(source);
    REQUIRE(bytes.size() > store::engine::FILE_HEADER_BYTES);
    dstd::write_u64_le(bytes.data() + HEADER_WRITER_RANDOM_AT, writer_random);
    rewrite_header_crc(bytes);
    const dstd::String path = target.file(name);
    write_file(path, without_footer(bytes));
    return path;
}

std::uint64_t header_writer_random(const dstd::String& path) {
    auto reader = engine::DataFileReader::open(path);
    REQUIRE(reader.has_value());
    return reader->index().writer_random;
}

dstd::Vector<std::uint64_t> copy_generations(const dstd::String& path) {
    auto reader = engine::DataFileReader::open(path);
    REQUIRE(reader.has_value());
    dstd::Vector<std::uint64_t> out;
    for (const store::engine::CopyItem& copy : reader->index().copies) {
        out.push_back(copy.generation);
    }
    std::sort(out.begin(), out.end());
    return out;
}

dstd::Vector<std::uint64_t> drop_generations(const dstd::String& path) {
    auto reader = engine::DataFileReader::open(path);
    REQUIRE(reader.has_value());
    dstd::Vector<std::uint64_t> out;
    for (const store::engine::DropItem& drop : reader->index().drops) {
        out.push_back(drop.generation);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uint64_t only_copy_generation(const dstd::String& path) {
    auto reader = engine::DataFileReader::open(path);
    REQUIRE(reader.has_value());
    REQUIRE(reader->index().copies.size() == 1);
    return reader->index().copies[0].generation;
}

void check_generation(const engine::Snapshot& snapshot,
                      const dstd::String& namespace_name, const ScopeKey& scope,
                      std::uint64_t expected) {
    auto listed = snapshot.generations();
    REQUIRE(listed.has_value());
    const dstd::Vector<store::engine::ScopeGeneration>& all = *listed;
    REQUIRE(all.size() == 1);
    CHECK(dstd::String(all[0].namespace_name) == namespace_name);
    CHECK(all[0].format_version == 0);
    CHECK(all[0].scope.kind == scope.kind);
    REQUIRE(all[0].scope.paths.size() == 1);
    CHECK(dstd::String(all[0].scope.paths[0]) == dstd::String(scope.paths[0]));
    CHECK(all[0].generation == expected);
}

dstd::Vector<std::uint8_t> bytes_vector(const dstd::String& bytes) {
    return dstd::Vector<std::uint8_t>(bytes.begin(), bytes.end());
}

dstd::String concat_value(int index) {
    const dstd::String digits = dstd::to_string(index);
    return "concat-" + dstd::String(10 - digits.size(), '0') + digits;
}

class ScopedDefaultRuntime {
   public:
    explicit ScopedDefaultRuntime(std::size_t threads)
        : previous_(dftracer::utils::default_runtime().threads()) {
        dftracer::utils::configure_default_runtime(
            {.threads = threads, .eager = true});
    }

    ~ScopedDefaultRuntime() {
        dftracer::utils::configure_default_runtime({.threads = previous_});
    }

    ScopedDefaultRuntime(const ScopedDefaultRuntime&) = delete;
    ScopedDefaultRuntime& operator=(const ScopedDefaultRuntime&) = delete;

   private:
    std::size_t previous_;
};

/// What one open of a root read, and a text of every scope state and row, so
/// two opens under different runtimes compare with one string.
struct OpenDump {
    dstd::String text;
    std::size_t files = 0;
    std::size_t extents = 0;
    std::uint64_t read_calls = 0;
    std::uint64_t commits_verified = 0;
    std::uint64_t extents_read = 0;
    std::size_t commits = 0;
    std::uint64_t indexes_read = 0;
    std::uint64_t metadata_blocks_read = 0;
    std::uint64_t page_reads = 0;
    std::uint64_t windows_warmed = 0;
};

OpenDump dump_open(const dftu_utils_test::ScopedTestDir& root,
                   const NamespaceSpec& spec,
                   const dstd::Vector<ScopeKey>& scopes) {
    auto snapshot = engine::Snapshot::open(root.str());
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->skipped().empty());
    OpenDump dump;
    dump.files = snapshot->open_stats().files;
    CHECK(snapshot->read_stats().read_calls == 0);
    for (const ScopeKey& scope : scopes) {
        dump.text +=
            "state " + dstd::String(scope.paths[0]) + " " +
            dstd::to_string(static_cast<int>(snapshot->state(spec, scope))) +
            "\n";
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        for (const ScopedRow& row : collect_scoped(cursor)) {
            dump.text += row.key + "=" + row.value + "\n";
        }
    }
    dump.read_calls = snapshot->read_stats().read_calls;
    dump.commits_verified = snapshot->read_stats().commits_verified;
    dump.extents_read = snapshot->read_stats().extents_read;
    dump.indexes_read = snapshot->read_stats().indexes_read;
    dump.metadata_blocks_read = snapshot->read_stats().metadata_blocks_read;
    dump.page_reads = snapshot->read_stats().page_reads;
    dump.windows_warmed = snapshot->read_stats().windows_warmed;
    auto report = snapshot->check();
    REQUIRE(report.has_value());
    dump.commits = report->commits;
    dump.extents = report->blocks;
    return dump;
}

int descriptor_of(const dstd::String& path) {
    struct stat file_stat{};
    if (::stat(path.c_str(), &file_stat) != 0) return -1;
    for (int fd = 0; fd < 4096; ++fd) {
        struct stat fd_stat{};
        if (::fstat(fd, &fd_stat) == 0 && fd_stat.st_dev == file_stat.st_dev &&
            fd_stat.st_ino == file_stat.st_ino) {
            return fd;
        }
    }
    return -1;
}

void write_scopes_file(const dftu_utils_test::ScopedTestDir& dir,
                       const NamespaceSpec& spec,
                       const dstd::Vector<ScopeKey>& scopes, int keys,
                       bool flushed) {
    auto writer = make_writer(dir);
    auto id = writer->add_namespace(spec);
    REQUIRE(id.has_value());
    for (const ScopeKey& scope : scopes) {
        Batch entries = writer->batch(*id, scope, 0);
        for (int k = 0; k < keys; ++k) {
            const dstd::String key = "b0-" + key_of(k);
            const dstd::String value = value_of(k);
            entries.put(view_of(key), view_of(value));
        }
        REQUIRE(writer->commit(std::move(entries)).has_value());
        if (flushed) REQUIRE(flush_writer(*writer).has_value());
    }
    REQUIRE(close_writer(*writer).has_value());
}

void write_scoped_file(const dftu_utils_test::ScopedTestDir& dir,
                       const NamespaceSpec& spec, const ScopeKey& scope,
                       int batches, int keys, bool flushed) {
    auto writer = make_writer(dir);
    auto id = writer->add_namespace(spec);
    REQUIRE(id.has_value());
    const dstd::String prefix = dstd::String(scope.paths[0]);
    for (int batch = 0; batch < batches; ++batch) {
        Batch entries = writer->batch(*id, scope, 0);
        for (int k = 0; k < keys; ++k) {
            const dstd::String key =
                "b" + dstd::to_string(batch) + "-" + key_of(k);
            const dstd::String value = value_of(k);
            entries.put(view_of(key), view_of(value));
        }
        REQUIRE(writer->commit(std::move(entries)).has_value());
        if (flushed) REQUIRE(flush_writer(*writer).has_value());
    }
    REQUIRE(close_writer(*writer).has_value());
}

dstd::Vector<dstd::Map<dstd::String, dstd::String>> write_many_commits(
    const dftu_utils_test::ScopedTestDir& dir, const NamespaceSpec& spec,
    const dstd::Vector<ScopeKey>& scopes, int commits) {
    constexpr int PER_COMMIT = 8;
    constexpr int KEY_SPAN = 32;
    dstd::Vector<dstd::Map<dstd::String, dstd::String>> model(scopes.size());
    auto writer = make_writer(dir);
    auto id = writer->add_namespace(spec);
    REQUIRE(id.has_value());
    for (int commit = 0; commit < commits; ++commit) {
        const std::size_t which =
            static_cast<std::size_t>(commit) % scopes.size();
        Batch entries = writer->batch(*id, scopes[which], 0);
        const int base = (commit / static_cast<int>(scopes.size())) * 3 %
                         (KEY_SPAN - PER_COMMIT);
        const dstd::String tag = "c" + dstd::to_string(commit) + "-";
        for (int k = 0; k < PER_COMMIT; ++k) {
            const dstd::String key = key_of(base + k);
            const dstd::String value = tag + dstd::to_string(base + k);
            entries.put(view_of(key), view_of(value));
            model[which][key] = value;
        }
        REQUIRE(writer->commit(std::move(entries)).has_value());
    }
    REQUIRE(close_writer(*writer).has_value());
    return model;
}

struct ClosedSegments {
    dstd::Vector<store::engine::BlockHandle> blocks;
    dstd::Vector<std::uint64_t> runs;
};

ClosedSegments closed_segments(const engine::DataFileReader& reader) {
    ClosedSegments found;
    const store::engine::FileIndex& index = reader.index();
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
                    found.runs.push_back(commit.sequence);
                    const auto known = std::find_if(
                        found.blocks.begin(), found.blocks.end(),
                        [&commit](const store::engine::BlockHandle& block) {
                            return block.offset == commit.segments.offset;
                        });
                    if (known == found.blocks.end()) {
                        found.blocks.push_back(commit.segments);
                    }
                }
            }
        }
    }
    return found;
}

}  // namespace

TEST_SUITE("index store snapshot") {
    TEST_CASE("scope keys sort by kind and then by paths") {
        const dstd::String a = "/traces/a.pfw";
        const dstd::String b = "/traces/b.pfw";
        const dstd::Vector<dstd::StringView> given = {
            dstd::StringView(b), dstd::StringView(a), dstd::StringView(b)};
        const ScopeKey files = ScopeKey::files(given);
        CHECK(files.kind == store::ScopeKind::Files);
        REQUIRE(files.paths.size() == 2);
        CHECK(dstd::String(files.paths[0]) == a);
        CHECK(dstd::String(files.paths[1]) == b);

        const ScopeKey root = ScopeKey::root();
        const ScopeKey file = file_scope(a);
        CHECK(root.kind == store::ScopeKind::Root);
        CHECK(root.paths.empty());
        CHECK(root < file);
        CHECK(file < files);
        CHECK(file_scope(a) < file_scope(b));
        CHECK(files == ScopeKey::files(given));
        CHECK_FALSE(files == file);
    }

    TEST_CASE("put entries round trip through get and both scans") {
        dftu_utils_test::ScopedTestDir dir("snapshot_round_trip");
        const ScopeKey scope = file_scope("/traces/a.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        for (int part = 0; part < 2; ++part) {
            Batch batch = writer->batch(*id, scope, 0);
            for (int i = part * 500; i < (part + 1) * 500; ++i) {
                const dstd::String key = key_of(i);
                const dstd::String value = value_of(i);
                batch.put(view_of(key), view_of(value));
            }
            REQUIRE(writer->commit(std::move(batch)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->skipped().empty());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);

        for (int i = 0; i < static_cast<int>(KEY_COUNT); ++i) {
            const dstd::String key = key_of(i);
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == value_of(i));
        }

        const dstd::String missing = "key-9999";
        auto absent = snapshot->get(spec, scope, view_of(missing));
        REQUIRE(absent.has_value());
        CHECK_FALSE(absent->has_value());

        const dstd::String prefix = "key-01";
        auto prefix_cursor = snapshot->scan(spec, scope, view_of(prefix));
        const auto prefix_rows = collect(prefix_cursor);
        REQUIRE(prefix_rows.size() == 100);
        for (int i = 0; i < 100; ++i) {
            CHECK(prefix_rows[static_cast<std::size_t>(i)].first ==
                  key_of(100 + i));
            CHECK(prefix_rows[static_cast<std::size_t>(i)].second ==
                  value_of(100 + i));
        }

        const dstd::String begin = key_of(100);
        const dstd::String end = key_of(200);
        auto range_cursor =
            snapshot->scan(spec, scope, view_of(begin), view_of(end));
        const auto range_rows = collect(range_cursor);
        REQUIRE(range_rows.size() == 100);
        for (int i = 0; i < 100; ++i) {
            CHECK(range_rows[static_cast<std::size_t>(i)].first ==
                  key_of(100 + i));
            CHECK(range_rows[static_cast<std::size_t>(i)].second ==
                  value_of(100 + i));
        }

        auto all_cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto all_rows = collect(all_cursor);
        REQUIRE(all_rows.size() == KEY_COUNT);
        for (std::size_t i = 0; i < all_rows.size(); ++i) {
            CHECK(all_rows[i].first == key_of(static_cast<int>(i)));
            CHECK(all_rows[i].second == value_of(static_cast<int>(i)));
        }
    }

    TEST_CASE(
        "a namespace trains two page dictionaries and reads through them") {
        dftu_utils_test::ScopedTestDir dir("snapshot_page_dict");
        const ScopeKey scope = file_scope("/traces/dict.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.dict");
        // The first dictionary trains on the first 1 MiB, the second on every
        // eighth page of the 8 MiB after it, and a seal waits for a training
        // after 2 MiB more. 20000 values of 1 KiB hold both; 24000 leave room.
        const DictLimits limits(std::size_t{1} << 20, std::uint64_t{2} << 20);
        constexpr int KEYS = 24000;
        constexpr int PER_BATCH = 200;
        constexpr int PER_BIG_BATCH = 2000;
        const auto dict_key = [](int index) {
            const dstd::String digits = dstd::to_string(index);
            return "key-" + dstd::String(6 - digits.size(), '0') + digits;
        };
        const auto value = [](int index) {
            dstd::String out;
            std::uint64_t state =
                static_cast<std::uint64_t>(index) * 2654435761U;
            while (out.size() < 1024) {
                state = mmix_lcg_step(state);
                out += "{\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":" +
                       dstd::to_string(state >> 52U) +
                       ",\"file\":\"/p/lustre/run/" +
                       dstd::to_string((state >> 20U) % 97U) +
                       "/data.h5\",\"size\":" +
                       dstd::to_string((state >> 8U) % 65536U) + "}\n";
            }
            return out;
        };

        engine::WriterOptions options;
        options.host = dstd::String("host");
        options.label = dstd::String("label");
        auto writer = make_writer(dir, options);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        int batches = 0;
        for (int first = 0; first < KEYS;) {
            const int count = std::min(
                KEYS - first, ++batches % 10 == 0 ? PER_BIG_BATCH : PER_BATCH);
            Batch batch = writer->batch(*id, scope, 0);
            for (int i = first; i < first + count; ++i) {
                const dstd::String key = dict_key(i);
                const dstd::String bytes = value(i);
                batch.put(view_of(key), view_of(bytes));
            }
            auto committed = writer->commit(std::move(batch));
            REQUIRE_MESSAGE(
                committed.has_value(),
                (committed ? dstd::String()
                           : dstd::String(committed.error().message())));
            first += count;
        }
        REQUIRE(flush_writer(*writer).has_value());

        {
            auto open = engine::Snapshot::open(dir.str());
            REQUIRE(open.has_value());
            for (int i = 0; i < KEYS; i += 997) {
                const dstd::String key = dict_key(i);
                auto found = open->get(spec, scope, view_of(key));
                REQUIRE(found.has_value());
                REQUIRE(found->has_value());
                CHECK(to_std((*found)->bytes()) == value(i));
            }
        }
        REQUIRE(close_writer(*writer).has_value());

        const dstd::String path = only_data_file(dir.str());
        auto report = store::engine::space_report(path);
        REQUIRE(report.has_value());
        CHECK(report->dicts == 2);
        CHECK(report->pages_dict > report->pages / 2);
        CHECK(report->dict_bytes > 0);
        CHECK(report->header_bytes + report->extent_bytes +
                  report->record_bytes + report->gap_bytes +
                  report->segment_block_bytes + report->items_block_bytes +
                  report->directory_leaf_bytes +
                  report->directory_separator_bytes + report->footer_bytes +
                  report->dict_bytes ==
              report->file_bytes);

        {
            auto snapshot = engine::Snapshot::open(dir.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->skipped().empty());
            auto cursor =
                snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
            const auto rows = collect(cursor);
            REQUIRE(rows.size() == static_cast<std::size_t>(KEYS));
            bool all_equal = true;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].first != dict_key(static_cast<int>(i)) ||
                    rows[i].second != value(static_cast<int>(i))) {
                    all_equal = false;
                }
            }
            CHECK(all_equal);
        }

        // Damaged dictionaries fail the reads of the pages that need them.
        // Every dictionary is damaged, so the check does not depend on which
        // one a page's extent took.
        auto opened = engine::DataFileReader::open(path);
        REQUIRE(opened.has_value());
        dstd::Vector<std::uint8_t> bytes = read_file(path);
        dstd::Set<std::uint64_t> dict_offsets;
        for (const store::engine::IndexedExtentRef& indexed :
             extent_refs_of(*opened)) {
            if (indexed.ref.segments.empty()) continue;
            auto pages = store::engine::ExtentPages::open_table(
                dstd::ByteView{reinterpret_cast<const std::byte*>(
                                   bytes.data() + indexed.ref.offset),
                               indexed.ref.length},
                indexed.ref.length);
            REQUIRE(pages.has_value());
            if (!pages->dict().empty())
                dict_offsets.insert(pages->dict().offset);
        }
        REQUIRE(dict_offsets.size() == 2);
        for (const std::uint64_t offset : dict_offsets) {
            bytes[static_cast<std::size_t>(offset) + 100U] ^= 0xFFU;
        }
        write_file(path, bytes);
        auto damaged = engine::Snapshot::open(dir.str());
        REQUIRE(damaged.has_value());
        std::size_t failed = 0;
        std::size_t read = 0;
        for (int i = 0; i < KEYS; i += 499) {
            const dstd::String key = dict_key(i);
            auto found = damaged->get(spec, scope, view_of(key));
            ++read;
            if (!found.has_value()) {
                ++failed;
            } else {
                REQUIRE(found->has_value());
                CHECK(to_std((*found)->bytes()) == value(i));
            }
        }
        CHECK(failed > read / 4);
    }

    TEST_CASE("a building copy stays hidden until it is published") {
        dftu_utils_test::ScopedTestDir dir("snapshot_building_copy");
        const ScopeKey scope = file_scope("/traces/building.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.building");
        const auto get =
            [&](const engine::Snapshot& snapshot,
                const dstd::String& key) -> dstd::Optional<dstd::String> {
            auto found = snapshot.get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            if (!found->has_value()) return dstd::nullopt;
            return to_std((*found)->bytes());
        };

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        const dstd::Vector<SourceStat> old_sources{SourceStat{1, 2, 3}};
        Batch ready = writer->batch(*id, scope, 5, &old_sources);
        ready.put(view_of(dstd::String("k1")), view_of(dstd::String("old1")));
        ready.put(view_of(dstd::String("k2")), view_of(dstd::String("old2")));
        REQUIRE(writer->commit(std::move(ready)).has_value());
        REQUIRE(flush_writer(*writer).has_value());

        Batch first = writer->building_batch(*id, scope);
        first.put(view_of(dstd::String("k1")), view_of(dstd::String("new1")));
        first.put(view_of(dstd::String("k3")), view_of(dstd::String("new3")));
        REQUIRE(writer->commit(std::move(first)).has_value());
        REQUIRE(flush_writer(*writer).has_value());
        {
            auto snapshot = engine::Snapshot::open(dir.str());
            REQUIRE(snapshot.has_value());
            CHECK(get(*snapshot, "k1") == dstd::Optional<dstd::String>("old1"));
            CHECK(get(*snapshot, "k2") == dstd::Optional<dstd::String>("old2"));
            CHECK_FALSE(get(*snapshot, "k3").has_value());
            CHECK(snapshot->copy_info(spec, scope).params_hash == 5);
        }

        Batch plain = writer->batch(*id, scope, 5, &old_sources);
        plain.put(view_of(dstd::String("k9")), view_of(dstd::String("x")));
        CHECK_FALSE(writer->commit(std::move(plain)).has_value());

        const dstd::String begin = "k3";
        const dstd::String end = dstd::String("k3") + '\0';
        REQUIRE(writer->erase(*id, scope, view_of(begin), view_of(end))
                    .has_value());
        Batch second = writer->building_batch(*id, scope);
        second.put(view_of(dstd::String("k4")), view_of(dstd::String("new4")));
        REQUIRE(writer->commit(std::move(second)).has_value());
        REQUIRE(flush_writer(*writer).has_value());

        const dstd::Vector<SourceStat> new_sources{SourceStat{4, 5, 6}};
        const engine::CopyChange target{*id, &scope, 9, &new_sources};
        REQUIRE(writer->apply({&target, 1}).has_value());
        REQUIRE(flush_writer(*writer).has_value());
        const auto check_published = [&](const engine::Snapshot& snapshot) {
            CHECK(get(snapshot, "k1") == dstd::Optional<dstd::String>("new1"));
            CHECK_FALSE(get(snapshot, "k2").has_value());
            CHECK_FALSE(get(snapshot, "k3").has_value());
            CHECK(get(snapshot, "k4") == dstd::Optional<dstd::String>("new4"));
            const CopyInfo info = snapshot.copy_info(spec, scope);
            CHECK(info.state == ScopeState::Ready);
            CHECK(info.params_hash == 9);
            REQUIRE(info.sources.size() == 1);
            CHECK(info.sources[0].mtime == 4);
        };
        {
            auto snapshot = engine::Snapshot::open(dir.str());
            REQUIRE(snapshot.has_value());
            check_published(*snapshot);
        }
        CHECK_FALSE(writer->apply({&target, 1}).has_value());

        REQUIRE(close_writer(*writer).has_value());
        auto closed = engine::Snapshot::open(dir.str());
        REQUIRE(closed.has_value());
        check_published(*closed);
    }

    TEST_CASE("a building copy never published stays hidden after close") {
        dftu_utils_test::ScopedTestDir dir("snapshot_building_unpublished");
        const ScopeKey scope = file_scope("/traces/unpublished.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.building");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch ready = writer->batch(*id, scope, 5);
        ready.put(view_of(dstd::String("k1")), view_of(dstd::String("old1")));
        REQUIRE(writer->commit(std::move(ready)).has_value());
        Batch building = writer->building_batch(*id, scope);
        building.put(view_of(dstd::String("k1")),
                     view_of(dstd::String("new1")));
        REQUIRE(writer->commit(std::move(building)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto found = snapshot->get(spec, scope, view_of(dstd::String("k1")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == "old1");
        CHECK(snapshot->copy_info(spec, scope).params_hash == 5);
    }

    TEST_CASE("the last put wins inside a batch and a later batch wins") {
        dftu_utils_test::ScopedTestDir dir("snapshot_last_wins");
        const ScopeKey scope = file_scope("/traces/b.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());

        Batch first = writer->batch(*id, scope, 0);
        first.put(view_of(dstd::String("key-b")), view_of(dstd::String("b1")));
        first.put(view_of(dstd::String("key-a")), view_of(dstd::String("a1")));
        first.put(view_of(dstd::String("key-b")), view_of(dstd::String("b2")));
        REQUIRE(writer->commit(std::move(first)).has_value());

        Batch second = writer->batch(*id, scope, 0);
        second.put(view_of(dstd::String("key-b")), view_of(dstd::String("b3")));
        second.put(view_of(dstd::String("key-c")), view_of(dstd::String("c1")));
        REQUIRE(writer->commit(std::move(second)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());

        const dstd::Vector<dstd::Pair<dstd::String, dstd::String>> expected = {
            {"key-a", "a1"}, {"key-b", "b3"}, {"key-c", "c1"}};
        for (const auto& row : expected) {
            auto found = snapshot->get(spec, scope, view_of(row.first));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == row.second);
        }

        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(rows[i].first == expected[i].first);
            CHECK(rows[i].second == expected[i].second);
        }
    }

    TEST_CASE("a batch that commits later wins over one recorded later") {
        dftu_utils_test::ScopedTestDir dir("snapshot_batch_ordinal");
        const ScopeKey scope = file_scope("/traces/o.pfw");
        const NamespaceSpec put = put_spec("dftu.test.put");
        const NamespaceSpec concat =
            fold_spec("dftu.test.concat", FoldKind::Concat);
        const dstd::String key = "same-key";
        // Longer than the capacity of a small extent, so its segment goes to a
        // run of its own and its write completes at once, while the earlier
        // batch stays in an extent that is sealed only at close.
        const dstd::String longer(8000, 'n');

        auto writer = make_writer(dir);
        auto put_id = writer->add_namespace(put);
        REQUIRE(put_id.has_value());
        auto concat_id = writer->add_namespace(concat);
        REQUIRE(concat_id.has_value());

        const auto put_value = [&](const dstd::String& value) {
            Batch batch = writer->batch(*put_id, scope, 0);
            batch.put(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(batch)).has_value());
        };
        const auto fold_value = [&](const dstd::String& value) {
            Batch batch = writer->batch(*concat_id, scope, 0);
            batch.fold(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(batch)).has_value());
        };
        put_value("old");
        fold_value("a");
        put_value(longer);
        fold_value(longer);
        REQUIRE(close_writer(*writer).has_value());

        // The batch that came first is written last, so neither the records nor
        // the extents of the file are in the order of the batches.
        auto reader =
            engine::DataFileReader::open(dstd::String(writer->path()));
        REQUIRE(reader.has_value());
        const dstd::Vector<store::engine::IndexedExtentRef> refs =
            extent_refs_of(*reader);
        const auto ref_of = [&refs](std::uint64_t ordinal) {
            for (const store::engine::IndexedExtentRef& ref : refs) {
                if (!ref.ref.segments.empty() &&
                    ref.ref.segments.front().ordinal == ordinal) {
                    return ref;
                }
            }
            FAIL("no extent of the batch with ordinal " << ordinal);
            return store::engine::IndexedExtentRef{};
        };
        const dstd::Pair<std::uint64_t, std::uint64_t> ordinals[] = {{1, 3},
                                                                     {2, 4}};
        for (const dstd::Pair<std::uint64_t, std::uint64_t>& pair : ordinals) {
            const store::engine::IndexedExtentRef older = ref_of(pair.first);
            const store::engine::IndexedExtentRef newer = ref_of(pair.second);
            REQUIRE(older.sequence >= newer.sequence);
            REQUIRE(older.ref.offset > newer.ref.offset);
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        const auto newest = [&](const NamespaceSpec& spec) {
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            return to_std((*found)->bytes());
        };
        CHECK(newest(put) == longer);
        CHECK(newest(concat) == "a" + longer);

        const auto scanned = [&](const NamespaceSpec& spec) {
            auto cursor =
                snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
            const auto rows = collect(cursor);
            REQUIRE(rows.size() == 1);
            CHECK(rows[0].first == key);
            return rows[0].second;
        };
        CHECK(scanned(put) == longer);
        CHECK(scanned(concat) == "a" + longer);
    }

    TEST_CASE("every fold kind folds across a batch and across copies") {
        dftu_utils_test::ScopedTestDir dir("snapshot_folds");
        const ScopeKey scope = file_scope("/traces/f.pfw");

        struct FoldCase {
            FoldKind kind;
            dstd::String name;
            dstd::Vector<dstd::String> first;
            dstd::Vector<dstd::String> second;
            dstd::String expected;
        };
        const dstd::Vector<FoldCase> cases = {
            {FoldKind::SumI64,
             "sum-i64",
             {i64_bytes(5), i64_bytes(7)},
             {i64_bytes(10)},
             i64_bytes(22)},
            {FoldKind::SumF64,
             "sum-f64",
             {f64_bytes(1.5), f64_bytes(2.25)},
             {f64_bytes(0.25)},
             f64_bytes(4.0)},
            {FoldKind::MinI64,
             "min-i64",
             {i64_bytes(5), i64_bytes(3)},
             {i64_bytes(4)},
             i64_bytes(3)},
            {FoldKind::MaxI64,
             "max-i64",
             {i64_bytes(5), i64_bytes(9)},
             {i64_bytes(1)},
             i64_bytes(9)},
            {FoldKind::MinF64,
             "min-f64",
             {f64_bytes(2.5), f64_bytes(1.5)},
             {f64_bytes(0.5)},
             f64_bytes(0.5)},
            {FoldKind::MaxF64,
             "max-f64",
             {f64_bytes(2.5), f64_bytes(7.5)},
             {f64_bytes(1.0)},
             f64_bytes(7.5)},
            {FoldKind::Count,
             "count",
             {"ignored", "also-ignored"},
             {"four"},
             i64_bytes(3)},
            {FoldKind::Last, "last", {"abc", "def"}, {"ghi"}, "ghi"},
            {FoldKind::Concat, "concat", {"a", "bc"}, {"d"}, "abcd"},
        };

        auto writer = make_writer(dir);
        for (const FoldCase& fold_case : cases) {
            const NamespaceSpec spec =
                fold_spec("dftu.test." + fold_case.name, fold_case.kind);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            const dstd::String key = "folded";

            Batch first = writer->batch(*id, scope, 0);
            for (const dstd::String& value : fold_case.first) {
                first.fold(view_of(key), view_of(value));
            }
            REQUIRE(writer->commit(std::move(first)).has_value());

            Batch second = writer->batch(*id, scope, 0);
            for (const dstd::String& value : fold_case.second) {
                second.fold(view_of(key), view_of(value));
            }
            REQUIRE(writer->commit(std::move(second)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        for (const FoldCase& fold_case : cases) {
            const NamespaceSpec spec =
                fold_spec("dftu.test." + fold_case.name, fold_case.kind);
            auto found =
                snapshot->get(spec, scope, view_of(dstd::String("folded")));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == fold_case.expected);
        }

        auto sum =
            snapshot->get(fold_spec("dftu.test.sum-i64", FoldKind::SumI64),
                          scope, view_of(dstd::String("folded")));
        REQUIRE(sum.has_value());
        REQUIRE(sum->has_value());
        CHECK(read_i64((*sum)->bytes()) == 22);
        auto mean =
            snapshot->get(fold_spec("dftu.test.sum-f64", FoldKind::SumF64),
                          scope, view_of(dstd::String("folded")));
        REQUIRE(mean.has_value());
        REQUIRE(mean->has_value());
        CHECK(read_f64((*mean)->bytes()) ==
              doctest::Approx(4.0).epsilon(1e-12));
    }

    TEST_CASE("a numeric fold rejects a value that is not eight bytes") {
        dftu_utils_test::ScopedTestDir dir("snapshot_fold_reject");
        const ScopeKey scope = file_scope("/traces/bad.pfw");
        const NamespaceSpec spec = fold_spec("dftu.test.sum", FoldKind::SumI64);

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());

        Batch batch = writer->batch(*id, scope, 0);
        batch.fold(view_of(dstd::String("good")), view_of(i64_bytes(1)));
        batch.fold(view_of(dstd::String("short")),
                   view_of(dstd::String("abcd")));
        auto rejected = writer->commit(std::move(batch));
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().is(dstd::Condition::InvalidArgument));

        auto put_id = writer->add_namespace(put_spec("dftu.test.put"));
        REQUIRE(put_id.has_value());
        Batch wrong_fold = writer->batch(*put_id, scope, 0);
        wrong_fold.fold(view_of(dstd::String("key")), view_of(i64_bytes(1)));
        auto folded = writer->commit(std::move(wrong_fold));
        REQUIRE_FALSE(folded.has_value());
        CHECK(folded.error().is(dstd::Condition::InvalidArgument));
        Batch wrong_put = writer->batch(*id, scope, 0);
        wrong_put.put(view_of(dstd::String("key")), view_of(i64_bytes(1)));
        auto put = writer->commit(std::move(wrong_put));
        REQUIRE_FALSE(put.has_value());
        CHECK(put.error().is(dstd::Condition::InvalidArgument));
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Absent);
        auto good = snapshot->get(spec, scope, view_of(dstd::String("good")));
        REQUIRE(good.has_value());
        CHECK_FALSE(good->has_value());
        auto short_value =
            snapshot->get(spec, scope, view_of(dstd::String("short")));
        REQUIRE(short_value.has_value());
        CHECK_FALSE(short_value->has_value());
    }

    TEST_CASE("a big value that does not compress round trips") {
        dftu_utils_test::ScopedTestDir dir("snapshot_big_raw");
        const ScopeKey scope = file_scope("/traces/big_raw.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        dstd::String big(2 * 1024 * 1024, '\0');
        std::uint64_t state = 0x9E3779B97F4A7C15ULL;
        for (char& c : big) {
            state ^= state << 13U;
            state ^= state >> 7U;
            state ^= state << 17U;
            c = static_cast<char>(state & 0xFFU);
        }

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        batch.put(view_of(dstd::String("big")), view_of(big));
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        CHECK(read_file(dstd::String(writer->path())).size() > big.size());
        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto found = snapshot->get(spec, scope, view_of(dstd::String("big")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == big);
    }

    TEST_CASE("a big value round trips and its crc is checked on read") {
        dftu_utils_test::ScopedTestDir dir("snapshot_big");
        dftu_utils_test::ScopedTestDir corrupt_dir("snapshot_big_corrupt");
        const ScopeKey scope = file_scope("/traces/big.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        dstd::String big(3 * 1024 * 1024, 'x');
        for (std::size_t i = 0; i < big.size(); i += 4096) {
            big[i] = static_cast<char>('a' + static_cast<int>((i / 4096) % 26));
        }

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        batch.put(view_of(dstd::String("big")), view_of(big));
        batch.put(view_of(dstd::String("small")),
                  view_of(dstd::String("small")));
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto found = snapshot->get(spec, scope, view_of(dstd::String("big")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == big);

        auto reader =
            engine::DataFileReader::open(dstd::String(writer->path()));
        REQUIRE(reader.has_value());
        store::engine::BigRef ref{};
        bool found_ref = false;
        for (const store::engine::IndexedExtentRef& extent :
             extent_refs_of(*reader)) {
            if (extent.ref.segments.empty()) continue;
            const store::engine::ExtentRef extent_ref{
                extent.ref.offset, extent.ref.length, extent.ref.crc};
            auto bytes = reader->read_extent(extent_ref);
            REQUIRE(bytes.has_value());
            REQUIRE(store::engine::decode_extent(*bytes).has_value());
            for (const store::engine::ExtentSegment& segment :
                 extent.ref.segments) {
                auto view = engine::SegmentView::open(view_of(*bytes).subspan(
                    segment.offset_in_extent, segment.length));
                REQUIRE(view.has_value());
                auto cursor = view->lower_bound(dstd::ByteView{});
                while (!cursor.at_end()) {
                    const auto entry = cursor.entry();
                    if (to_std(entry.key()) == "big") {
                        REQUIRE(entry.is_big());
                        ref = entry.big_ref();
                        found_ref = true;
                    }
                    cursor.next();
                }
            }
        }
        REQUIRE(found_ref);
        CHECK(ref.length < big.size() / 8);

        dstd::Vector<std::uint8_t> bytes =
            read_file(dstd::String(writer->path()));
        REQUIRE(ref.offset + ref.length <= bytes.size());
        bytes[static_cast<std::size_t>(ref.offset + ref.length / 2)] ^= 0xFFU;
        const dstd::String corrupt_path = corrupt_dir.file("w-corrupt.dfs");
        write_file(corrupt_path, bytes);

        {
            auto corrupt = engine::Snapshot::open(corrupt_dir.str());
            REQUIRE(corrupt.has_value());
            CHECK(corrupt->state(spec, scope) == ScopeState::Ready);
            auto bad = corrupt->get(spec, scope, view_of(dstd::String("big")));
            REQUIRE_FALSE(bad.has_value());
            CHECK(bad.error().is(dstd::Condition::Parse));
            auto other =
                corrupt->get(spec, scope, view_of(dstd::String("small")));
            REQUIRE(other.has_value());
            CHECK(other->has_value());
            auto report = corrupt->check();
            REQUIRE(report.has_value());
            CHECK(report->blocks == 2);
            CHECK(report->segments == 1);
            CHECK(report->bad_segments.empty());
            REQUIRE(report->bad_commits.size() == 1);
            CHECK(report->bad_commits[0].sequence == 1);
        }
        {
            auto sound = engine::Snapshot::open(dir.str());
            REQUIRE(sound.has_value());
            auto report = sound->check();
            REQUIRE(report.has_value());
            CHECK(report->blocks == 2);
            CHECK(report->bad_commits.empty());
        }

        // Without the footer the commit is not synced, and the big value run
        // is a block of it: when it does not verify, the whole commit reads as
        // bad, so neither key is returned.
        write_file(corrupt_path, without_footer(bytes));
        auto corrupt = engine::Snapshot::open(corrupt_dir.str());
        REQUIRE(corrupt.has_value());
        CHECK(corrupt->state(spec, scope) == ScopeState::Bad);
        auto bad = corrupt->get(spec, scope, view_of(dstd::String("big")));
        REQUIRE(bad.has_value());
        CHECK_FALSE(bad->has_value());
        auto other = corrupt->get(spec, scope, view_of(dstd::String("small")));
        REQUIRE(other.has_value());
        CHECK_FALSE(other->has_value());
    }

    TEST_CASE(
        "a batch larger than an extent is one segment in a run of its own") {
        dftu_utils_test::ScopedTestDir dir("snapshot_split");
        const ScopeKey scope = file_scope("/traces/many.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        constexpr int COUNT = 5000;

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        for (int i = 0; i < COUNT; ++i) {
            const dstd::String key = key_of(i);
            const dstd::String value = value_of(i);
            batch.put(view_of(key), view_of(value));
        }
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto reader =
            engine::DataFileReader::open(dstd::String(writer->path()));
        REQUIRE(reader.has_value());
        REQUIRE(extent_refs_of(*reader).size() == 1);
        const store::engine::ExtentRefItem run = extent_refs_of(*reader)[0].ref;
        REQUIRE(run.segments.size() == 1);
        CHECK(run.segments[0].entry_count == static_cast<std::uint64_t>(COUNT));
        CHECK(run.length > SMALL_EXTENT);
        CHECK(run.length % store::engine::EXTENT_ALIGN_BYTES == 0);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == COUNT);
        for (int i = 0; i < COUNT; ++i) {
            CHECK(rows[static_cast<std::size_t>(i)].first == key_of(i));
            CHECK(rows[static_cast<std::size_t>(i)].second == value_of(i));
        }
        for (int i = 0; i < COUNT; i += 137) {
            auto found = snapshot->get(spec, scope, view_of(key_of(i)));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == value_of(i));
        }
    }

    TEST_CASE("the newest generation wins, then the writer id, then the name") {
        dftu_utils_test::ScopedTestDir dir("snapshot_generation");
        dftu_utils_test::ScopedTestDir left("snapshot_generation_left");
        dftu_utils_test::ScopedTestDir right("snapshot_generation_right");
        dftu_utils_test::ScopedTestDir by_writer("snapshot_generation_writer");
        dftu_utils_test::ScopedTestDir by_name("snapshot_generation_name");
        const ScopeKey scope = file_scope("/traces/g.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::String key = "shared";

        {
            auto first = make_writer(dir);
            auto id = first->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = first->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(dstd::String("from-first")));
            REQUIRE(first->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*first).has_value());
        }

        {
            auto second = make_writer(dir);
            auto id = second->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = second->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(dstd::String("from-second")));
            REQUIRE(second->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*second).has_value());
        }

        auto newest = engine::Snapshot::open(dir.str());
        REQUIRE(newest.has_value());
        auto newest_value = newest->get(spec, scope, view_of(key));
        REQUIRE(newest_value.has_value());
        REQUIRE(newest_value->has_value());
        CHECK(to_std((*newest_value)->bytes()) == "from-second");

        // A writer in an empty directory starts its scope at generation 1, so
        // the two files below hold the same generation and tie.
        dstd::String left_path;
        {
            auto writer = make_writer(left);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(dstd::String("from-left")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            left_path = dstd::String(writer->path());
        }
        dstd::String right_path;
        {
            auto writer = make_writer(right);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(dstd::String("from-right")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            right_path = dstd::String(writer->path());
        }
        CHECK(only_copy_generation(left_path) == 1);
        CHECK(only_copy_generation(right_path) == 1);

        const dstd::String tied_low =
            forge_copy(left_path, by_writer, "w-z.dfs", 100);
        const dstd::String tied_high =
            forge_copy(right_path, by_writer, "w-a.dfs", 200);
        CHECK(header_writer_random(tied_low) == 100);
        CHECK(header_writer_random(tied_high) == 200);
        CHECK(only_copy_generation(tied_low) == 1);
        CHECK(only_copy_generation(tied_high) == 1);
        auto tie = engine::Snapshot::open(by_writer.str());
        REQUIRE(tie.has_value());
        auto tie_value = tie->get(spec, scope, view_of(key));
        REQUIRE(tie_value.has_value());
        REQUIRE(tie_value->has_value());
        CHECK(to_std((*tie_value)->bytes()) == "from-right");

        const dstd::String named_low =
            forge_copy(right_path, by_name, "w-a.dfs", 300);
        const dstd::String named_high =
            forge_copy(left_path, by_name, "w-b.dfs", 300);
        CHECK(header_writer_random(named_low) == 300);
        CHECK(header_writer_random(named_high) == 300);
        CHECK(only_copy_generation(named_low) == 1);
        CHECK(only_copy_generation(named_high) == 1);
        auto name_tie = engine::Snapshot::open(by_name.str());
        REQUIRE(name_tie.has_value());
        auto name_value = name_tie->get(spec, scope, view_of(key));
        REQUIRE(name_value.has_value());
        REQUIRE(name_value->has_value());
        CHECK(to_std((*name_value)->bytes()) == "from-left");
    }

    TEST_CASE("a whole drop hides the scope and a later copy is visible") {
        dftu_utils_test::ScopedTestDir dir("snapshot_drop_whole");
        const ScopeKey scope = file_scope("/traces/d.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(dstd::String("key-a")),
                      view_of(dstd::String("a")));
            batch.put(view_of(dstd::String("key-b")),
                      view_of(dstd::String("b")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            REQUIRE(writer->drop(*id, scope).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        {
            auto dropped = engine::Snapshot::open(dir.str());
            REQUIRE(dropped.has_value());
            CHECK(dropped->state(spec, scope) == ScopeState::Absent);
            auto found =
                dropped->get(spec, scope, view_of(dstd::String("key-a")));
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
            auto cursor =
                dropped->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
            CHECK(cursor.at_end());
        }

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(dstd::String("key-z")),
                      view_of(dstd::String("z")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto rewritten = engine::Snapshot::open(dir.str());
        REQUIRE(rewritten.has_value());
        CHECK(rewritten->state(spec, scope) == ScopeState::Ready);
        auto old_key =
            rewritten->get(spec, scope, view_of(dstd::String("key-a")));
        REQUIRE(old_key.has_value());
        CHECK_FALSE(old_key->has_value());
        auto cursor =
            rewritten->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].first == "key-z");
        CHECK(rows[0].second == "z");
    }

    TEST_CASE("a range drop hides exactly the keys it names") {
        dftu_utils_test::ScopedTestDir dir("snapshot_drop_range");
        const ScopeKey scope = file_scope("/traces/r.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            for (const dstd::String key :
                 {"key-a", "key-b", "key-c", "key-d"}) {
                batch.put(view_of(key), view_of("v-" + key));
            }
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            const dstd::String begin = "key-b";
            const dstd::String end = "key-d";
            REQUIRE(writer->drop(*id, scope, view_of(begin), view_of(end))
                        .has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        for (const dstd::String key : {"key-b", "key-c"}) {
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
        }
        for (const dstd::String key : {"key-a", "key-d"}) {
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == "v-" + key);
        }
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].first == "key-a");
        CHECK(rows[0].second == "v-key-a");
        CHECK(rows[1].first == "key-d");
        CHECK(rows[1].second == "v-key-d");
    }

    TEST_CASE("an erase hides the earlier keys of a copy in its range") {
        dftu_utils_test::ScopedTestDir dir("snapshot_erase_range");
        const ScopeKey scope = file_scope("/traces/e.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        dstd::Vector<SourceStat> sources;
        sources.push_back(SourceStat{11, 22, 33});

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch first = writer->batch(*id, scope, 41, &sources);
        for (const dstd::String key : {"key-a", "key-b", "key-c", "key-d"}) {
            first.put(view_of(key), view_of("v1-" + key));
        }
        REQUIRE(writer->commit(std::move(first)).has_value());
        const dstd::String begin = "key-b";
        const dstd::String end = "key-d";
        REQUIRE(writer->erase(*id, scope, view_of(begin), view_of(end))
                    .has_value());
        Batch second = writer->batch(*id, scope, 41, &sources);
        second.put(view_of(dstd::String("key-c")),
                   view_of(dstd::String("v2-key-c")));
        REQUIRE(writer->commit(std::move(second)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        const auto check_rows = [&](engine::Snapshot& snapshot) {
            const CopyInfo info = snapshot.copy_info(spec, scope);
            CHECK(info.state == ScopeState::Ready);
            CHECK(info.generation == 1);
            CHECK(info.params_hash == 41);
            REQUIRE(info.sources.size() == 1);
            CHECK(info.sources[0].mtime == 11);

            auto hidden =
                snapshot.get(spec, scope, view_of(dstd::String("key-b")));
            REQUIRE(hidden.has_value());
            CHECK_FALSE(hidden->has_value());
            auto shown =
                snapshot.get(spec, scope, view_of(dstd::String("key-c")));
            REQUIRE(shown.has_value());
            REQUIRE(shown->has_value());
            CHECK(to_std((*shown)->bytes()) == "v2-key-c");
            for (const dstd::String key : {"key-a", "key-d"}) {
                auto found = snapshot.get(spec, scope, view_of(key));
                REQUIRE(found.has_value());
                REQUIRE(found->has_value());
                CHECK(to_std((*found)->bytes()) == "v1-" + key);
            }

            auto whole =
                snapshot.scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
            const auto rows = collect(whole);
            REQUIRE(rows.size() == 3);
            CHECK(rows[0] == dstd::make_pair(dstd::String("key-a"),
                                             dstd::String("v1-key-a")));
            CHECK(rows[1] == dstd::make_pair(dstd::String("key-c"),
                                             dstd::String("v2-key-c")));
            CHECK(rows[2] == dstd::make_pair(dstd::String("key-d"),
                                             dstd::String("v1-key-d")));

            auto prefix =
                snapshot.scan(spec, scope, view_of(dstd::String("key-b")));
            CHECK(collect(prefix).empty());

            auto range = snapshot.scan(spec, scope, view_of(begin),
                                       view_of(dstd::String("key-cc")));
            const auto in_range = collect(range);
            REQUIRE(in_range.size() == 1);
            CHECK(in_range[0].first == "key-c");
            CHECK(in_range[0].second == "v2-key-c");

            auto all =
                snapshot.scan_all(spec, dstd::ByteView{}, dstd::ByteView{});
            const auto all_rows = collect(all);
            REQUIRE(all_rows.size() == 3);
            CHECK(all_rows[1].second == "v2-key-c");
        };

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        check_rows(*snapshot);

        dftu_utils_test::ScopedTestDir chain_dir("snapshot_erase_range_chain");
        write_file(chain_dir.file("data.dfs"),
                   without_footer(read_file(only_data_file(dir.str()))));
        auto chain = engine::Snapshot::open(chain_dir.str());
        REQUIRE(chain.has_value());
        check_rows(*chain);
    }

    TEST_CASE("an erase with an empty end hides to the last key") {
        dftu_utils_test::ScopedTestDir dir("snapshot_erase_open_end");
        const ScopeKey scope = file_scope("/traces/o.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        for (const dstd::String key : {"key-a", "key-b", "key-c"}) {
            batch.put(view_of(key), view_of(key));
        }
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(writer
                    ->erase(*id, scope, view_of(dstd::String("key-b")),
                            dstd::ByteView{})
                    .has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].first == "key-a");
        for (const dstd::String key : {"key-b", "key-c"}) {
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
        }
    }

    TEST_CASE("an erase hides a version a fold namespace would fold") {
        dftu_utils_test::ScopedTestDir dir("snapshot_erase_fold");
        const ScopeKey scope = file_scope("/traces/f.pfw");
        const NamespaceSpec spec =
            fold_spec("dftu.test.concat", FoldKind::Concat);

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        const auto fold = [&](const dstd::String& key,
                              const dstd::String& value) {
            Batch batch = writer->batch(*id, scope, 0);
            batch.fold(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(batch)).has_value());
        };
        fold("key-x", "x1");
        fold("key-z", "z1");
        REQUIRE(writer
                    ->erase(*id, scope, view_of(dstd::String("key-x")),
                            view_of(dstd::String("key-y")))
                    .has_value());
        fold("key-x", "x2");
        fold("key-z", "z2");
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto folded =
            snapshot->get(spec, scope, view_of(dstd::String("key-x")));
        REQUIRE(folded.has_value());
        REQUIRE(folded->has_value());
        CHECK(to_std((*folded)->bytes()) == "x2");
        auto both = snapshot->get(spec, scope, view_of(dstd::String("key-z")));
        REQUIRE(both.has_value());
        REQUIRE(both->has_value());
        CHECK(to_std((*both)->bytes()) == "z1z2");

        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].first == "key-x");
        CHECK(rows[0].second == "x2");
        CHECK(rows[1].first == "key-z");
        CHECK(rows[1].second == "z1z2");
    }

    TEST_CASE("an erase with no current copy of the scope does nothing") {
        dftu_utils_test::ScopedTestDir dir("snapshot_erase_absent");
        const ScopeKey scope = file_scope("/traces/n.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(dstd::String("key-a")),
                      view_of(dstd::String("a")));
            batch.put(view_of(dstd::String("key-b")),
                      view_of(dstd::String("b")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            REQUIRE(writer
                        ->erase(*id, scope, view_of(dstd::String("key-a")),
                                view_of(dstd::String("key-b")))
                        .has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        for (const dstd::String key : {"key-a", "key-b"}) {
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == key.substr(key.size() - 1));
        }
        const CopyInfo info = snapshot->copy_info(spec, scope);
        CHECK(info.state == ScopeState::Ready);
        CHECK(info.generation == 1);
    }

    TEST_CASE("an erase is visible only once its commit record is") {
        dftu_utils_test::ScopedTestDir dir("snapshot_erase_crash");
        const ScopeKey scope = file_scope("/traces/cr.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        for (const dstd::String key : {"key-a", "key-b", "key-c"}) {
            batch.put(view_of(key), view_of(key));
        }
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(flush_writer(*writer).has_value());
        // A crash here: the batch is recorded and the erase is not.
        const dstd::Vector<std::uint8_t> before =
            read_file(dstd::String(writer->path()));
        REQUIRE(writer
                    ->erase(*id, scope, view_of(dstd::String("key-b")),
                            view_of(dstd::String("key-c")))
                    .has_value());
        REQUIRE(close_writer(*writer).has_value());

        dftu_utils_test::ScopedTestDir cut("snapshot_erase_crash_cut");
        write_file(cut.file("data.dfs"), before);
        auto recovered = engine::Snapshot::open(cut.str());
        REQUIRE(recovered.has_value());
        auto recovered_cursor =
            recovered->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto recovered_rows = collect(recovered_cursor);
        REQUIRE(recovered_rows.size() == 3);
        auto visible =
            recovered->get(spec, scope, view_of(dstd::String("key-b")));
        REQUIRE(visible.has_value());
        REQUIRE(visible->has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].first == "key-a");
        CHECK(rows[1].first == "key-c");
        auto hidden =
            snapshot->get(spec, scope, view_of(dstd::String("key-b")));
        REQUIRE(hidden.has_value());
        CHECK_FALSE(hidden->has_value());
    }

    TEST_CASE("a failed copy reads as Failed and returns nothing") {
        dftu_utils_test::ScopedTestDir dir("snapshot_failed");
        const ScopeKey scope = file_scope("/traces/x.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        REQUIRE(writer->fail(*id, scope, 17).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Failed);
        auto found = snapshot->get(spec, scope, view_of(dstd::String("key-a")));
        REQUIRE(found.has_value());
        CHECK_FALSE(found->has_value());
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        CHECK(cursor.at_end());
    }

    TEST_CASE("a copy keeps the sources of its build") {
        dftu_utils_test::ScopedTestDir dir("snapshot_copy_sources");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey one = file_scope("/traces/a.pfw");
        const dstd::Vector<dstd::StringView> given = {
            dstd::StringView("/traces/b.pfw"),
            dstd::StringView("/traces/a.pfw")};
        const ScopeKey two = ScopeKey::files(given);
        REQUIRE(two.paths.size() == 2);
        CHECK(dstd::String(two.paths[0]) == "/traces/a.pfw");
        CHECK(dstd::String(two.paths[1]) == "/traces/b.pfw");

        dstd::Vector<SourceStat> one_sources;
        one_sources.push_back(SourceStat{100, 200, 300});
        dstd::Vector<SourceStat> two_sources;
        two_sources.push_back(SourceStat{400, 500, 600});
        two_sources.push_back(SourceStat{700, 800, 900});

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch first = writer->batch(*id, one, 41, &one_sources);
            first.put(view_of(dstd::String("key-a")),
                      view_of(dstd::String("a")));
            REQUIRE(writer->commit(std::move(first)).has_value());
            Batch second = writer->batch(*id, two, 42, &two_sources);
            second.put(view_of(dstd::String("key-b")),
                       view_of(dstd::String("b")));
            REQUIRE(writer->commit(std::move(second)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        const auto check_one = [](const CopyInfo& info) {
            CHECK(info.state == ScopeState::Ready);
            CHECK(info.params_hash == 41);
            CHECK(info.generation == 1);
            REQUIRE(info.sources.size() == 1);
            CHECK(info.sources[0].mtime == 100);
            CHECK(info.sources[0].size == 200);
            CHECK(info.sources[0].hash == 300);
        };
        const auto check_two = [](const CopyInfo& info) {
            CHECK(info.state == ScopeState::Ready);
            CHECK(info.params_hash == 42);
            CHECK(info.generation == 1);
            REQUIRE(info.sources.size() == 2);
            CHECK(info.sources[0].mtime == 400);
            CHECK(info.sources[0].size == 500);
            CHECK(info.sources[0].hash == 600);
            CHECK(info.sources[1].mtime == 700);
            CHECK(info.sources[1].size == 800);
            CHECK(info.sources[1].hash == 900);
        };

        {
            auto snapshot = engine::Snapshot::open(dir.str());
            REQUIRE(snapshot.has_value());
            check_one(snapshot->copy_info(spec, one));
            check_two(snapshot->copy_info(spec, two));

            const CopyInfo absent =
                snapshot->copy_info(spec, file_scope("/t/z"));
            CHECK(absent.state == ScopeState::Absent);
            CHECK(absent.params_hash == 0);
            CHECK(absent.generation == 0);
            CHECK(absent.sources.empty());
        }

        drop_summary(dir.str());
        const dstd::String path = only_data_file(dir.str());
        write_file(path, without_footer(read_file(path)));
        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        check_one(snapshot->copy_info(spec, one));
        check_two(snapshot->copy_info(spec, two));
    }

    TEST_CASE("a rebuild with new sources replaces the copy") {
        dftu_utils_test::ScopedTestDir dir("snapshot_copy_sources_rebuilt");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey scope = file_scope("/traces/a.pfw");

        dstd::Vector<SourceStat> old_sources;
        old_sources.push_back(SourceStat{1, 2, 3});
        dstd::Vector<SourceStat> new_sources;
        new_sources.push_back(SourceStat{7, 8, 9});

        const auto write = [&](const dstd::Vector<SourceStat>& sources) {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 5, &sources);
            batch.put(view_of(dstd::String("key-a")),
                      view_of(dstd::String("a")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        };
        write(old_sources);
        write(new_sources);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        const CopyInfo info = snapshot->copy_info(spec, scope);
        CHECK(info.state == ScopeState::Ready);
        CHECK(info.generation == 2);
        CHECK(info.params_hash == 5);
        REQUIRE(info.sources.size() == 1);
        CHECK(info.sources[0].mtime == 7);
        CHECK(info.sources[0].size == 8);
        CHECK(info.sources[0].hash == 9);
    }

    TEST_CASE(
        "scopes lists every scope with a current copy in ScopeKey order") {
        dftu_utils_test::ScopedTestDir dir("snapshot_scopes");
        const NamespaceSpec spec = put_spec("dftu.test.scopes");
        const NamespaceSpec other = put_spec("dftu.test.scopes.other");
        const ScopeKey root = ScopeKey::root();
        const ScopeKey a = file_scope("/traces/a.pfw");
        const ScopeKey b = file_scope("/traces/b.pfw");
        const ScopeKey dropped = file_scope("/traces/c.pfw");
        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            auto other_id = writer->add_namespace(other);
            REQUIRE(id.has_value());
            REQUIRE(other_id.has_value());
            const auto put =
                [&](store::engine::NamespaceId at, const ScopeKey& scope,
                    std::uint64_t params_hash, const dstd::String& value) {
                    Batch batch = writer->batch(at, scope, params_hash);
                    batch.put(view_of(dstd::String("key")), view_of(value));
                    REQUIRE(writer->commit(std::move(batch)).has_value());
                };
            put(*id, root, 10, "root");
            put(*id, b, 12, "b");
            put(*id, a, 11, "a");
            put(*id, dropped, 13, "c");
            put(*other_id, a, 20, "other-a");
            REQUIRE(writer->drop(*id, dropped).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());

        auto listed = snapshot->scopes(spec);
        REQUIRE(listed.has_value());
        REQUIRE(listed->size() == 3);
        CHECK((*listed)[0].scope == root);
        CHECK((*listed)[0].copy.params_hash == 10);
        CHECK((*listed)[1].scope == a);
        CHECK((*listed)[1].copy.params_hash == 11);
        CHECK((*listed)[1].copy.state == ScopeState::Ready);
        CHECK((*listed)[2].scope == b);
        CHECK((*listed)[2].copy.params_hash == 12);
        for (const store::ScopeEntry& entry : *listed) {
            CHECK(entry.scope != dropped);
        }

        auto theirs = snapshot->scopes(other);
        REQUIRE(theirs.has_value());
        REQUIRE(theirs->size() == 1);
        CHECK((*theirs)[0].scope == a);
        CHECK((*theirs)[0].copy.params_hash == 20);

        auto unknown = snapshot->scopes(put_spec("dftu.test.absent"));
        REQUIRE(unknown.has_value());
        CHECK(unknown->empty());
    }

    TEST_CASE("namespaces lists every namespace its files declare") {
        dftu_utils_test::ScopedTestDir dir("snapshot_namespaces");
        const NamespaceSpec older = put_spec("dftu.test.namespaces", 1);
        const NamespaceSpec raised = put_spec("dftu.test.namespaces", 3);
        const NamespaceSpec other = put_spec("dftu.test.namespaces.other", 2);
        const ScopeKey a = file_scope("/traces/a.pfw");
        const ScopeKey b = file_scope("/traces/b.pfw");
        {
            auto writer = make_writer(dir);
            auto first = writer->add_namespace(older);
            auto second = writer->add_namespace(other);
            REQUIRE(first.has_value());
            REQUIRE(second.has_value());
            for (const auto& [at, scope] :
                 {dstd::make_pair(*first, a), dstd::make_pair(*second, a)}) {
                Batch batch = writer->batch(at, scope, 0);
                batch.put(view_of(dstd::String("key")),
                          view_of(dstd::String("v")));
                REQUIRE(writer->commit(std::move(batch)).has_value());
            }
            REQUIRE(close_writer(*writer).has_value());
        }
        {
            auto writer = make_writer(dir);
            auto only = writer->add_namespace(raised);
            REQUIRE(only.has_value());
            Batch batch = writer->batch(*only, b, 0);
            batch.put(view_of(dstd::String("key")), view_of(dstd::String("v")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto listed = snapshot->namespaces();
        REQUIRE(listed.has_value());
        REQUIRE(listed->size() == 3);
        CHECK(dstd::String((*listed)[0].name) == "dftu.test.namespaces");
        CHECK((*listed)[0].format_version == 1);
        CHECK(dstd::String((*listed)[1].name) == "dftu.test.namespaces");
        CHECK((*listed)[1].format_version == 3);
        CHECK(dstd::String((*listed)[2].name) == "dftu.test.namespaces.other");
        CHECK((*listed)[2].format_version == 2);
    }

    TEST_CASE("a batch whose sources differ from its copy fails") {
        dftu_utils_test::ScopedTestDir dir("snapshot_copy_sources_differ");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey scope = file_scope("/traces/a.pfw");

        dstd::Vector<SourceStat> kept_sources;
        kept_sources.push_back(SourceStat{1, 2, 3});
        dstd::Vector<SourceStat> other_sources;
        other_sources.push_back(SourceStat{4, 5, 6});

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch first = writer->batch(*id, scope, 9, &kept_sources);
        first.put(view_of(dstd::String("key-a")), view_of(dstd::String("a")));
        REQUIRE(writer->commit(std::move(first)).has_value());

        Batch sources_changed = writer->batch(*id, scope, 9, &other_sources);
        sources_changed.put(view_of(dstd::String("key-b")),
                            view_of(dstd::String("b")));
        auto rejected = writer->commit(std::move(sources_changed));
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().is(dstd::Condition::InvalidArgument));

        Batch hash_changed = writer->batch(*id, scope, 10, &kept_sources);
        hash_changed.put(view_of(dstd::String("key-c")),
                         view_of(dstd::String("c")));
        auto rejected_hash = writer->commit(std::move(hash_changed));
        REQUIRE_FALSE(rejected_hash.has_value());
        CHECK(rejected_hash.error().is(dstd::Condition::InvalidArgument));

        Batch again = writer->batch(*id, scope, 9, &kept_sources);
        again.put(view_of(dstd::String("key-d")), view_of(dstd::String("d")));
        REQUIRE(writer->commit(std::move(again)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        const CopyInfo info = snapshot->copy_info(spec, scope);
        CHECK(info.state == ScopeState::Ready);
        CHECK(info.generation == 1);
        CHECK(info.params_hash == 9);
        REQUIRE(info.sources.size() == 1);
        CHECK(info.sources[0].mtime == 1);
        CHECK(info.sources[0].size == 2);
        CHECK(info.sources[0].hash == 3);
        auto found = snapshot->get(spec, scope, view_of(dstd::String("key-d")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == "d");
        auto missing =
            snapshot->get(spec, scope, view_of(dstd::String("key-b")));
        REQUIRE(missing.has_value());
        CHECK_FALSE(missing->has_value());
    }

    TEST_CASE("a failed copy reads as Failed through copy_info") {
        dftu_utils_test::ScopedTestDir dir("snapshot_copy_failed_info");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey scope = file_scope("/traces/x.pfw");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        REQUIRE(writer->fail(*id, scope, 17).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        const CopyInfo info = snapshot->copy_info(spec, scope);
        CHECK(info.state == ScopeState::Failed);
        CHECK(info.generation == 1);
        CHECK(info.params_hash == 17);
        CHECK(info.sources.empty());
    }

    TEST_CASE("a corrupt extent makes its copy Bad and leaves other scopes") {
        dftu_utils_test::ScopedTestDir dir("snapshot_bad_extent");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_bad_extent_copy");
        const ScopeKey broken = file_scope("/traces/broken.pfw");
        const ScopeKey sound = file_scope("/traces/sound.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        for (int batch = 1; batch <= 3; ++batch) {
            Batch entries = writer->batch(*id, broken, 0);
            const dstd::String key = "broken-" + dstd::to_string(batch);
            const dstd::String value = "broken-value-" + dstd::to_string(batch);
            entries.put(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        // One extent mixes the segments of every scope of a namespace, so the
        // broken scope is sealed into an extent of its own here, and the sound
        // scope writes into the next one.
        REQUIRE(flush_writer(*writer).has_value());
        for (int batch = 1; batch <= 2; ++batch) {
            Batch entries = writer->batch(*id, sound, 0);
            const dstd::String key = "sound-" + dstd::to_string(batch);
            const dstd::String value = "sound-value-" + dstd::to_string(batch);
            entries.put(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());

        auto reader =
            engine::DataFileReader::open(dstd::String(writer->path()));
        REQUIRE(reader.has_value());
        REQUIRE(extent_refs_of(*reader).size() == 2);
        store::engine::ExtentRef broken_extent{};
        broken_extent.offset = extent_refs_of(*reader)[0].ref.offset;
        broken_extent.length = extent_refs_of(*reader)[0].ref.length;
        broken_extent.crc = extent_refs_of(*reader)[0].ref.crc;

        dstd::Vector<std::uint8_t> bytes =
            read_file(dstd::String(writer->path()));
        REQUIRE(broken_extent.offset + broken_extent.length <= bytes.size());
        bytes[static_cast<std::size_t>(broken_extent.offset) + 4U] ^= 0xFFU;
        const dstd::String copy_path = copy_dir.file("w-bad.dfs");
        write_file(copy_path, without_footer(bytes));

        auto snapshot = engine::Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, broken) == ScopeState::Bad);
        auto hidden =
            snapshot->get(spec, broken, view_of(dstd::String("broken-1")));
        REQUIRE(hidden.has_value());
        CHECK_FALSE(hidden->has_value());
        auto broken_cursor =
            snapshot->scan(spec, broken, dstd::ByteView{}, dstd::ByteView{});
        CHECK(broken_cursor.at_end());

        CHECK(snapshot->state(spec, sound) == ScopeState::Ready);
        auto visible =
            snapshot->get(spec, sound, view_of(dstd::String("sound-2")));
        REQUIRE(visible.has_value());
        REQUIRE(visible->has_value());
        CHECK(to_std((*visible)->bytes()) == "sound-value-2");
        auto sound_cursor =
            snapshot->scan(spec, sound, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(sound_cursor);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].first == "sound-1");
        CHECK(rows[1].first == "sound-2");
    }

    TEST_CASE("a copy taken before close shows exactly the commits it holds") {
        dftu_utils_test::ScopedTestDir dir("snapshot_crash");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_crash_copy");
        const ScopeKey scope = file_scope("/traces/c.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        for (int batch = 1; batch <= 5; ++batch) {
            Batch entries = writer->batch(*id, scope, 0);
            const dstd::String key = "commit-" + dstd::to_string(batch);
            const dstd::String value = "value-" + dstd::to_string(batch);
            entries.put(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(entries)).has_value());
            if (batch == 3) {
                // A group commit is what makes a commit visible, so the copy
                // holds the three commits flushed here and neither of the two
                // after.
                REQUIRE(flush_writer(*writer).has_value());
                write_file(copy_dir.file("w-crash.dfs"),
                           read_file(dstd::String(writer->path())));
            }
        }

        auto snapshot = engine::Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        for (int batch = 1; batch <= 3; ++batch) {
            const dstd::String key = "commit-" + dstd::to_string(batch);
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) ==
                  "value-" + dstd::to_string(batch));
        }
        for (int batch = 4; batch <= 5; ++batch) {
            const dstd::String key = "commit-" + dstd::to_string(batch);
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
        }
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        CHECK(collect(cursor).size() == 3);
        REQUIRE(close_writer(*writer).has_value());
    }

    TEST_CASE("a file shorter than its header holds nothing and is no skip") {
        dftu_utils_test::ScopedTestDir dir("snapshot_newborn");
        const ScopeKey scope = file_scope("/traces/n.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        batch.put(view_of(dstd::String("key-a")), view_of(dstd::String("a")));
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        write_file(dir.file("w-newborn-empty.dfs"), {});
        write_file(dir.file("w-newborn-part.dfs"),
                   dstd::Vector<std::uint8_t>(100, 0));

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->skipped().empty());
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        CHECK(collect(cursor).size() == 1);
    }

    TEST_CASE("a file of another format version is skipped and listed") {
        dftu_utils_test::ScopedTestDir dir("snapshot_unsupported");
        const ScopeKey scope = file_scope("/traces/u.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        batch.put(view_of(dstd::String("key-a")), view_of(dstd::String("a")));
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        dstd::Vector<std::uint8_t> bytes =
            read_file(dstd::String(writer->path()));
        dstd::Vector<std::uint8_t> header_two = bytes;
        dstd::write_u32_le(header_two.data() + 8, 2);
        rewrite_header_crc(header_two);
        write_file(dir.file("w-header-two.dfs"), without_footer(header_two));
        write_file(dir.file("w-tail-two.dfs"), with_tail_version(bytes, 2));

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->skipped().size() == 2);
        CHECK(dstd::String(snapshot->skipped()[0].name) == "w-header-two.dfs");
        CHECK(snapshot->skipped()[0].error.is(dstd::Condition::Unsupported));
        CHECK(dstd::String(snapshot->skipped()[1].name) == "w-tail-two.dfs");
        CHECK(snapshot->skipped()[1].error.is(dstd::Condition::Unsupported));
        auto found = snapshot->get(spec, scope, view_of(dstd::String("key-a")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == "a");
    }

    TEST_CASE("scans follow the given scope order and fold across scopes") {
        dftu_utils_test::ScopedTestDir dir("snapshot_scopes");
        const ScopeKey first = file_scope("/traces/a.pfw");
        const ScopeKey second = file_scope("/traces/b.pfw");
        const NamespaceSpec put = put_spec("dftu.test.put");
        const NamespaceSpec sum = fold_spec("dftu.test.sum", FoldKind::SumI64);

        auto writer = make_writer(dir);
        auto put_id = writer->add_namespace(put);
        REQUIRE(put_id.has_value());
        auto sum_id = writer->add_namespace(sum);
        REQUIRE(sum_id.has_value());

        Batch first_batch = writer->batch(*put_id, first, 0);
        first_batch.put(view_of(dstd::String("a-key")),
                        view_of(dstd::String("a")));
        REQUIRE(writer->commit(std::move(first_batch)).has_value());
        Batch second_batch = writer->batch(*put_id, second, 0);
        second_batch.put(view_of(dstd::String("b-key")),
                         view_of(dstd::String("b")));
        REQUIRE(writer->commit(std::move(second_batch)).has_value());

        Batch first_sum = writer->batch(*sum_id, first, 0);
        first_sum.fold(view_of(dstd::String("total")), view_of(i64_bytes(1)));
        REQUIRE(writer->commit(std::move(first_sum)).has_value());
        Batch second_sum = writer->batch(*sum_id, second, 0);
        second_sum.fold(view_of(dstd::String("total")), view_of(i64_bytes(3)));
        REQUIRE(writer->commit(std::move(second_sum)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());

        {
            dstd::Vector<ScopeKey> order;
            order.push_back(second);
            order.push_back(first);
            auto cursor =
                snapshot->scan(put, order, dstd::ByteView{}, dstd::ByteView{});
            const auto rows = collect_scoped(cursor);
            REQUIRE(rows.size() == 2);
            CHECK(rows[0].scope == "/traces/b.pfw");
            CHECK(rows[0].key == "b-key");
            CHECK(rows[0].value == "b");
            CHECK(rows[1].scope == "/traces/a.pfw");
            CHECK(rows[1].key == "a-key");
            CHECK(rows[1].value == "a");
        }

        {
            auto cursor =
                snapshot->scan_all(put, dstd::ByteView{}, dstd::ByteView{});
            const auto rows = collect_scoped(cursor);
            REQUIRE(rows.size() == 2);
            CHECK(rows[0].scope == "/traces/a.pfw");
            CHECK(rows[0].key == "a-key");
            CHECK(rows[1].scope == "/traces/b.pfw");
            CHECK(rows[1].key == "b-key");
        }

        {
            auto cursor =
                snapshot->scan_all(sum, dstd::ByteView{}, dstd::ByteView{});
            const auto rows = collect_scoped(cursor);
            REQUIRE(rows.size() == 1);
            CHECK(rows[0].key == "total");
            CHECK(read_i64(view_of(rows[0].value)) == 4);
        }

        {
            auto cursor =
                snapshot->scan(sum, first, dstd::ByteView{}, dstd::ByteView{});
            const auto rows = collect(cursor);
            REQUIRE(rows.size() == 1);
            CHECK(read_i64(view_of(rows[0].second)) == 1);
        }
    }

    TEST_CASE("a namespace of another format version is invisible") {
        dftu_utils_test::ScopedTestDir dir("snapshot_version");
        const ScopeKey scope = file_scope("/traces/v.pfw");
        const NamespaceSpec writer_spec = put_spec("dftu.test.versioned", 1);
        const NamespaceSpec other_spec = put_spec("dftu.test.versioned", 2);

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(writer_spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        batch.put(view_of(dstd::String("key-a")), view_of(dstd::String("a")));
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(writer_spec, scope) == ScopeState::Ready);
        CHECK(snapshot->state(other_spec, scope) == ScopeState::Absent);
        auto hidden =
            snapshot->get(other_spec, scope, view_of(dstd::String("key-a")));
        REQUIRE(hidden.has_value());
        CHECK_FALSE(hidden->has_value());
        auto cursor = snapshot->scan(other_spec, scope, dstd::ByteView{},
                                     dstd::ByteView{});
        CHECK(cursor.at_end());
        auto visible =
            snapshot->get(writer_spec, scope, view_of(dstd::String("key-a")));
        REQUIRE(visible.has_value());
        REQUIRE(visible->has_value());
        CHECK(to_std((*visible)->bytes()) == "a");
    }

    TEST_CASE("generations count copies, drops and failures") {
        dftu_utils_test::ScopedTestDir replaced("snapshot_generations_drop");
        dftu_utils_test::ScopedTestDir failed("snapshot_generations_fail");
        dftu_utils_test::ScopedTestDir next("snapshot_generations_next");
        const ScopeKey scope = file_scope("/traces/order.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        dstd::String replaced_path;
        {
            auto writer = make_writer(replaced);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch first = writer->batch(*id, scope, 0);
            first.put(view_of(dstd::String("key-one")),
                      view_of(dstd::String("one")));
            REQUIRE(writer->commit(std::move(first)).has_value());
            REQUIRE(writer->drop(*id, scope).has_value());
            Batch second = writer->batch(*id, scope, 0);
            second.put(view_of(dstd::String("key-two")),
                       view_of(dstd::String("two")));
            REQUIRE(writer->commit(std::move(second)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            replaced_path = dstd::String(writer->path());
        }
        CHECK((copy_generations(replaced_path) ==
               dstd::Vector<std::uint64_t>{1, 3}));
        CHECK((drop_generations(replaced_path) ==
               dstd::Vector<std::uint64_t>{2}));
        {
            auto snapshot = engine::Snapshot::open(replaced.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
            auto hidden =
                snapshot->get(spec, scope, view_of(dstd::String("key-one")));
            REQUIRE(hidden.has_value());
            CHECK_FALSE(hidden->has_value());
            auto visible =
                snapshot->get(spec, scope, view_of(dstd::String("key-two")));
            REQUIRE(visible.has_value());
            REQUIRE(visible->has_value());
            CHECK(to_std((*visible)->bytes()) == "two");
            auto cursor =
                snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
            const auto rows = collect(cursor);
            REQUIRE(rows.size() == 1);
            CHECK(rows[0].first == "key-two");
            CHECK(rows[0].second == "two");
            check_generation(*snapshot, "dftu.test.put", scope, 3);
        }

        dstd::String failed_path;
        {
            auto writer = make_writer(failed);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            REQUIRE(writer->fail(*id, scope, 7).has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(dstd::String("key")), view_of(dstd::String("v")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            failed_path = dstd::String(writer->path());
        }
        CHECK((copy_generations(failed_path) ==
               dstd::Vector<std::uint64_t>{1, 2}));
        CHECK(drop_generations(failed_path).empty());
        {
            auto snapshot = engine::Snapshot::open(failed.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
            auto found =
                snapshot->get(spec, scope, view_of(dstd::String("key")));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == "v");
            check_generation(*snapshot, "dftu.test.put", scope, 2);
        }

        {
            auto writer = make_writer(next);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch first = writer->batch(*id, scope, 0);
            first.put(view_of(dstd::String("key-one")),
                      view_of(dstd::String("one")));
            REQUIRE(writer->commit(std::move(first)).has_value());
            REQUIRE(writer->drop(*id, scope).has_value());
            Batch second = writer->batch(*id, scope, 0);
            second.put(view_of(dstd::String("key-two")),
                       view_of(dstd::String("two")));
            REQUIRE(writer->commit(std::move(second)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }
        dstd::String next_path;
        {
            auto writer = make_writer(next);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(dstd::String("key-three")),
                      view_of(dstd::String("three")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            next_path = dstd::String(writer->path());
        }
        CHECK((copy_generations(next_path) == dstd::Vector<std::uint64_t>{4}));
        {
            auto snapshot = engine::Snapshot::open(next.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
            auto found =
                snapshot->get(spec, scope, view_of(dstd::String("key-three")));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == "three");
            auto gone =
                snapshot->get(spec, scope, view_of(dstd::String("key-two")));
            REQUIRE(gone.has_value());
            CHECK_FALSE(gone->has_value());
            check_generation(*snapshot, "dftu.test.put", scope, 4);
        }
    }

    TEST_CASE("scans fold in commit order and in scope order") {
        dftu_utils_test::ScopedTestDir dir("snapshot_fold_order");
        const ScopeKey first = file_scope("/traces/a.pfw");
        const ScopeKey second = file_scope("/traces/b.pfw");
        const NamespaceSpec last = fold_spec("dftu.test.last", FoldKind::Last);
        const NamespaceSpec concat =
            fold_spec("dftu.test.concat", FoldKind::Concat);
        const dstd::String key = "same-key";

        auto writer = make_writer(dir);
        auto last_id = writer->add_namespace(last);
        REQUIRE(last_id.has_value());
        auto concat_id = writer->add_namespace(concat);
        REQUIRE(concat_id.has_value());

        const auto fold_last = [&](const ScopeKey& scope,
                                   const dstd::String& value) {
            Batch batch = writer->batch(*last_id, scope, 0);
            batch.fold(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(batch)).has_value());
        };
        const auto fold_concat = [&](const ScopeKey& scope,
                                     const dstd::String& value) {
            Batch batch = writer->batch(*concat_id, scope, 0);
            batch.fold(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(batch)).has_value());
        };
        fold_last(first, "a1");
        fold_concat(first, "a1");
        fold_last(first, "a2");
        fold_concat(first, "a2");
        fold_last(second, "b1");
        fold_concat(second, "b1");
        fold_last(second, "b2");
        fold_concat(second, "b2");
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        const auto expect_one = [&key](store::engine::ScanCursor& cursor,
                                       const dstd::String& expected) {
            const auto rows = collect(cursor);
            REQUIRE(rows.size() == 1);
            CHECK(rows[0].first == key);
            CHECK(rows[0].second == expected);
        };

        {
            auto cursor =
                snapshot->scan(last, first, dstd::ByteView{}, dstd::ByteView{});
            expect_one(cursor, "a2");
        }
        {
            auto cursor = snapshot->scan(concat, first, dstd::ByteView{},
                                         dstd::ByteView{});
            expect_one(cursor, "a1a2");
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(first);
            order.push_back(second);
            auto cursor =
                snapshot->scan(last, order, dstd::ByteView{}, dstd::ByteView{});
            expect_one(cursor, "b2");
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(first);
            order.push_back(second);
            auto cursor = snapshot->scan(concat, order, dstd::ByteView{},
                                         dstd::ByteView{});
            expect_one(cursor, "a1a2b1b2");
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(second);
            order.push_back(first);
            auto cursor =
                snapshot->scan(last, order, dstd::ByteView{}, dstd::ByteView{});
            expect_one(cursor, "a2");
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(second);
            order.push_back(first);
            auto cursor = snapshot->scan(concat, order, dstd::ByteView{},
                                         dstd::ByteView{});
            expect_one(cursor, "b1b2a1a2");
        }
        {
            auto cursor =
                snapshot->scan_all(last, dstd::ByteView{}, dstd::ByteView{});
            expect_one(cursor, "b2");
        }
        {
            auto cursor =
                snapshot->scan_all(concat, dstd::ByteView{}, dstd::ByteView{});
            expect_one(cursor, "a1a2b1b2");
        }
    }

    TEST_CASE("a whole drop at the generation of a copy does not hide it") {
        dftu_utils_test::ScopedTestDir dropped("snapshot_equal_drop_dropped");
        dftu_utils_test::ScopedTestDir copied("snapshot_equal_drop_copied");
        dftu_utils_test::ScopedTestDir merged("snapshot_equal_drop_merged");
        const ScopeKey scope = file_scope("/traces/e.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::String key = "shared";

        dstd::String drop_path;
        {
            auto writer = make_writer(dropped);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(dstd::String("dropped")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(writer->drop(*id, scope).has_value());
            REQUIRE(close_writer(*writer).has_value());
            drop_path = dstd::String(writer->path());
        }
        CHECK((drop_generations(drop_path) == dstd::Vector<std::uint64_t>{2}));

        dstd::String first_path;
        {
            auto writer = make_writer(copied);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(dstd::String("old")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            first_path = dstd::String(writer->path());
        }
        dstd::String second_path;
        {
            auto writer = make_writer(copied);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(dstd::String("kept")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            second_path = dstd::String(writer->path());
        }
        CHECK(only_copy_generation(second_path) == 2);

        forge_copy(first_path, merged, "w-first.dfs", 20);
        forge_copy(second_path, merged, "w-second.dfs", 30);
        forge_copy(drop_path, merged, "w-drop.dfs", 10);

        auto snapshot = engine::Snapshot::open(merged.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        auto found = snapshot->get(spec, scope, view_of(key));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == "kept");
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].first == key);
        CHECK(rows[0].second == "kept");
        check_generation(*snapshot, "dftu.test.put", scope, 2);
    }

    TEST_CASE("a bad extent marks every copy of its commit") {
        dftu_utils_test::ScopedTestDir built("snapshot_bad_commit_built");
        dftu_utils_test::ScopedTestDir corrupt("snapshot_bad_commit_corrupt");
        const ScopeKey first = file_scope("/traces/a.pfw");
        const ScopeKey second = file_scope("/traces/b.pfw");
        const ScopeKey third = file_scope("/traces/c.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        const auto seal = [](const dstd::String& key,
                             const dstd::String& value) {
            store::engine::SegmentBuilder builder;
            builder.add(view_of(key), view_of(value));
            auto sealed = builder.seal(nullptr, nullptr, nullptr);
            REQUIRE(sealed.has_value());
            return std::move(*sealed);
        };
        const auto add_scope = [](store::engine::DataFileWriter& file,
                                  std::uint32_t id, const dstd::String& path) {
            store::engine::ScopeItem item;
            item.local_id = id;
            item.kind = store::ScopeKind::File;
            item.paths.push_back(dstd::String(path));
            file.add(std::move(item));
        };
        const auto add_copy = [](store::engine::DataFileWriter& file,
                                 std::uint32_t id, std::uint32_t scope_id) {
            store::engine::CopyItem item;
            item.local_id = id;
            item.namespace_id = 1;
            item.scope_id = scope_id;
            item.generation = 1;
            item.state = store::engine::CopyState::Ready;
            file.add(std::move(item));
        };
        const auto extent_segment =
            [](std::uint32_t copy_id, std::uint32_t offset,
               const dstd::String& key,
               const dstd::Vector<std::uint8_t>& bytes) {
                store::engine::ExtentSegment segment;
                segment.copy_id = copy_id;
                segment.offset_in_extent = offset;
                segment.length = static_cast<std::uint32_t>(bytes.size());
                segment.entry_count = 1;
                segment.first_key = bytes_vector(key);
                segment.last_key = bytes_vector(key);
                return segment;
            };
        const auto append_extent =
            [](store::engine::DataFileWriter& file, std::uint64_t sequence,
               const dstd::Vector<std::uint8_t>& extent,
               dstd::Vector<store::engine::ExtentSegment> segments) {
                auto reference = file.append_extent(view_of(extent));
                REQUIRE(reference.has_value());
                store::engine::ExtentRefItem item;
                item.offset = reference->offset;
                item.length = reference->length;
                item.crc = reference->crc;
                item.sequence = sequence;
                item.segments = std::move(segments);
                return item;
            };

        const dstd::String path = built.file("w-built.dfs");
        store::engine::ExtentRefItem shared{};
        {
            auto file = store::engine::DataFileWriter::create(
                path, SMALL_EXTENT, "host", "label");
            REQUIRE(file.has_value());

            store::engine::NamespaceItem namespaces;
            namespaces.local_id = 1;
            namespaces.name = dstd::String("dftu.test.put");
            file->add(std::move(namespaces));
            add_scope(*file, 1, "/traces/a.pfw");
            add_copy(*file, 1, 1);
            const auto segment_a1 = seal("a-1", "a-value-1");
            dstd::Vector<std::uint8_t> first_extent(segment_a1.begin(),
                                                    segment_a1.end());
            dstd::Vector<store::engine::ExtentSegment> first_segments;
            first_segments.push_back(extent_segment(1, 0, "a-1", segment_a1));
            file->add(append_extent(*file, 1, first_extent,
                                    std::move(first_segments)));
            REQUIRE(file->commit().has_value());

            add_scope(*file, 2, "/traces/b.pfw");
            add_copy(*file, 2, 2);
            const auto segment_a2 = seal("a-2", "a-value-2");
            const auto segment_b1 = seal("b-1", "b-value-1");
            dstd::Vector<std::uint8_t> shared_extent(segment_a2.begin(),
                                                     segment_a2.end());
            shared_extent.insert(shared_extent.end(), segment_b1.begin(),
                                 segment_b1.end());
            dstd::Vector<store::engine::ExtentSegment> shared_segments;
            shared_segments.push_back(extent_segment(1, 0, "a-2", segment_a2));
            shared_segments.push_back(
                extent_segment(2, static_cast<std::uint32_t>(segment_a2.size()),
                               "b-1", segment_b1));
            shared = append_extent(*file, 2, shared_extent,
                                   std::move(shared_segments));
            file->add(shared);
            REQUIRE(file->commit().has_value());

            add_scope(*file, 3, "/traces/c.pfw");
            add_copy(*file, 3, 3);
            const auto segment_c1 = seal("c-1", "c-value-1");
            dstd::Vector<std::uint8_t> third_extent(segment_c1.begin(),
                                                    segment_c1.end());
            dstd::Vector<store::engine::ExtentSegment> third_segments;
            third_segments.push_back(extent_segment(3, 0, "c-1", segment_c1));
            file->add(append_extent(*file, 3, third_extent,
                                    std::move(third_segments)));
            REQUIRE(file->commit().has_value());
            REQUIRE(file->close().has_value());
        }

        dstd::Vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(shared.offset + shared.length <= bytes.size());
        bytes[static_cast<std::size_t>(shared.offset) + 4U] ^= 0xFFU;
        write_file(corrupt.file("w-bad.dfs"), without_footer(bytes));

        auto snapshot = engine::Snapshot::open(corrupt.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, first) == ScopeState::Bad);
        CHECK(snapshot->state(spec, second) == ScopeState::Bad);
        CHECK(snapshot->state(spec, third) == ScopeState::Ready);
        for (const auto& hidden :
             {dstd::make_pair(first, dstd::String("a-1")),
              dstd::make_pair(first, dstd::String("a-2")),
              dstd::make_pair(second, dstd::String("b-1"))}) {
            auto found =
                snapshot->get(spec, hidden.first, view_of(hidden.second));
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(first);
            order.push_back(second);
            auto cursor =
                snapshot->scan(spec, order, dstd::ByteView{}, dstd::ByteView{});
            CHECK(cursor.at_end());
        }
        auto visible = snapshot->get(spec, third, view_of(dstd::String("c-1")));
        REQUIRE(visible.has_value());
        REQUIRE(visible->has_value());
        CHECK(to_std((*visible)->bytes()) == "c-value-1");
        auto cursor =
            snapshot->scan(spec, third, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].first == "c-1");
        CHECK(rows[0].second == "c-value-1");
    }

    TEST_CASE("an entry larger than an extent commits as a run of its own") {
        const std::size_t lengths[] = {3900, 3925, 3950, 3975,
                                       4000, 4025, 4050};
        const auto large_key = [&lengths](std::size_t index) {
            dstd::String key(lengths[index], 'k');
            key[key.size() - 1] = static_cast<char>('a' + index);
            return key;
        };
        const ScopeKey scope = file_scope("/traces/large.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        dftu_utils_test::ScopedTestDir dir("snapshot_extent_entry_large");
        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            for (std::size_t index = 0; index < std::size(lengths); ++index) {
                Batch batch = writer->batch(*id, scope, 0);
                batch.put(view_of(large_key(index)),
                          view_of(dstd::String("value")));
                REQUIRE(writer->commit(std::move(batch)).has_value());
            }
            REQUIRE(flush_writer(*writer).has_value());

            auto reader =
                engine::DataFileReader::open(dstd::String(writer->path()));
            REQUIRE(reader.has_value());
            REQUIRE(extent_refs_of(*reader).size() == std::size(lengths));
            for (const store::engine::IndexedExtentRef& extent :
                 extent_refs_of(*reader)) {
                REQUIRE(extent.ref.segments.size() == 1);
                CHECK(extent.ref.segments[0].length >=
                      SMALL_EXTENT - store::engine::EXTENT_HEADER_BYTES);
                CHECK(extent.ref.length % store::engine::EXTENT_ALIGN_BYTES ==
                      0);
            }
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        for (std::size_t index = 0; index < std::size(lengths); ++index) {
            auto found = snapshot->get(spec, scope, view_of(large_key(index)));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == "value");
        }
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == std::size(lengths));
        for (std::size_t index = 0; index < std::size(lengths); ++index) {
            CHECK(rows[index].first == large_key(index));
            CHECK(rows[index].second == "value");
        }
    }

    TEST_CASE("a concat batch is sized by its folded length") {
        dftu_utils_test::ScopedTestDir dir("snapshot_extent_concat");
        const ScopeKey scope = file_scope("/traces/concat.pfw");
        const NamespaceSpec spec =
            fold_spec("dftu.test.concat", FoldKind::Concat);
        constexpr std::size_t VALUE_COUNT = 200;
        const dstd::String keys[] = {"dup-a", "dup-b"};

        dstd::String expected;
        for (std::size_t index = 0; index < VALUE_COUNT; ++index) {
            expected += concat_value(static_cast<int>(index));
        }
        REQUIRE(expected.size() == VALUE_COUNT * 17);

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            for (const dstd::String& key : keys) {
                for (std::size_t index = 0; index < VALUE_COUNT; ++index) {
                    batch.fold(view_of(key),
                               view_of(concat_value(static_cast<int>(index))));
                }
            }
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        for (const dstd::String& key : keys) {
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == expected);
        }
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].first == "dup-a");
        CHECK(rows[0].second == expected);
        CHECK(rows[1].first == "dup-b");
        CHECK(rows[1].second == expected);
    }

    TEST_CASE("a batch that fills an extent exactly stays in one segment") {
        dftu_utils_test::ScopedTestDir dir("snapshot_extent_exact");
        const ScopeKey scope = file_scope("/traces/exact.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        // 64 entries of 512 bytes and one of 451 bytes per granule make the
        // segment 131072 bytes, i.e. exactly one extent of the size named here.
        constexpr std::uint32_t EXTENT = 128U * 1024U;
        constexpr std::size_t SMALL_VALUE = 446;
        constexpr std::size_t BIG_COUNT = 255;
        constexpr std::size_t BIG_VALUE = 508;

        engine::WriterOptions options;
        options.extent_size = EXTENT;
        options.host = dstd::String("host");
        options.label = dstd::String("label");
        auto writer = make_writer(dir, options);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        const dstd::String small_key("\0\x01", 2);
        batch.put(view_of(small_key), view_of(dstd::String(SMALL_VALUE, 's')));
        for (std::size_t index = 0; index < BIG_COUNT; ++index) {
            const dstd::String key(1, static_cast<char>(index + 1));
            batch.put(view_of(key), view_of(dstd::String(BIG_VALUE, 'v')));
        }
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto reader =
            engine::DataFileReader::open(dstd::String(writer->path()));
        REQUIRE(reader.has_value());
        REQUIRE(extent_refs_of(*reader).size() == 1);
        REQUIRE(extent_refs_of(*reader)[0].ref.segments.size() == 1);
        CHECK(extent_refs_of(*reader)[0].ref.segments[0].entry_count ==
              BIG_COUNT + 1);
        CHECK(extent_refs_of(*reader)[0].ref.segments[0].length == EXTENT);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == BIG_COUNT + 1);
        CHECK(rows[0].first == small_key);
        CHECK(rows[0].second == dstd::String(SMALL_VALUE, 's'));
        for (std::size_t index = 0; index < BIG_COUNT; ++index) {
            CHECK(rows[index + 1].first ==
                  dstd::String(1, static_cast<char>(index + 1)));
            CHECK(rows[index + 1].second == dstd::String(BIG_VALUE, 'v'));
        }
        auto found =
            snapshot->get(spec, scope, view_of(dstd::String(1, '\x05')));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == dstd::String(BIG_VALUE, 'v'));
    }

    TEST_CASE(
        "a root of four writers opens the same with one and eight threads") {
        dftu_utils_test::ScopedTestDir dir("snapshot_open_threads");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        constexpr int WRITERS = 4;
        constexpr int BATCHES = 8;
        constexpr int KEYS = 1000;
        dstd::Vector<ScopeKey> scopes;
        for (int w = 0; w < WRITERS; ++w) {
            scopes.push_back(
                file_scope("/traces/w" + dstd::to_string(w) + ".pfw"));
        }
        for (int w = 0; w < WRITERS; ++w) {
            write_scoped_file(dir, spec, scopes[static_cast<std::size_t>(w)],
                              BATCHES, KEYS, false);
        }

        const OpenDump one = [&] {
            ScopedDefaultRuntime runtime(1);
            return dump_open(dir, spec, scopes);
        }();
#if DFTRACER_UTILS_TEST_PROBES
        const std::uint64_t direct_before = engine::g_direct_reads.load();
#endif
        const OpenDump eight = [&] {
            ScopedDefaultRuntime runtime(8);
            return dump_open(dir, spec, scopes);
        }();
        // Direct reads skip the page cache, so they warm no window.
#if DFTRACER_UTILS_TEST_PROBES
        const bool direct = engine::g_direct_reads.load() > direct_before;
#else
        const bool direct = false;
#endif

        CHECK(one.text == eight.text);
        CHECK(one.files == WRITERS);
        CHECK(eight.files == WRITERS);
        CHECK(one.extents == eight.extents);
        for (int w = 0; w < WRITERS; ++w) {
            CHECK(one.text.find("state /traces/w" + dstd::to_string(w) +
                                ".pfw 1") != dstd::String::npos);
        }
        CHECK(one.text.find("b0-key-0000=value-0") != dstd::String::npos);
        CHECK(one.extents >= WRITERS * BATCHES);
        CHECK(one.extents == eight.extents);
        CHECK(eight.read_calls == one.read_calls);
        CHECK(eight.commits == one.commits);
        CHECK(eight.commits >= WRITERS);
        CHECK(eight.commits_verified == 0);
        CHECK(one.commits_verified == 0);
        CHECK(one.extents_read == one.extents);
        CHECK(eight.extents_read == eight.extents);
        CHECK(one.indexes_read == 0);
        CHECK(eight.indexes_read == 0);
        CHECK(eight.metadata_blocks_read == one.metadata_blocks_read);
        CHECK(eight.metadata_blocks_read >= 2U * WRITERS);
        CHECK(eight.windows_warmed == (direct ? 0U : WRITERS));
        CHECK(eight.read_calls == eight.extents_read +
                                      eight.metadata_blocks_read +
                                      eight.page_reads + eight.windows_warmed);
    }

    TEST_CASE("a corrupt extent in a middle run marks only its commit bad") {
        dftu_utils_test::ScopedTestDir dir("snapshot_open_corrupt");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_open_corrupt_copy");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::Vector<ScopeKey> scopes = {file_scope("/traces/a.pfw"),
                                               file_scope("/traces/b.pfw"),
                                               file_scope("/traces/c.pfw")};
        write_scopes_file(dir, spec, scopes, 1000, true);
        const dstd::String path = only_data_file(dir.str());

        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        REQUIRE(extent_refs_of(*reader).size() == 3);
        const store::engine::ExtentRefItem middle =
            extent_refs_of(*reader)[1].ref;
        std::uint64_t middle_raw = 0;
        for (const store::engine::ExtentSegment& segment : middle.segments) {
            middle_raw += segment.length;
        }
        CHECK(middle_raw > SMALL_EXTENT);
        CHECK(middle.length % store::engine::EXTENT_ALIGN_BYTES == 0);

        dstd::Vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(middle.offset + middle.length <= bytes.size());
        // The last byte of the reference is padding no segment covers, so only
        // the block crc sees it.
        bytes[static_cast<std::size_t>(middle.offset) + middle.length - 1U] ^=
            0xFFU;
        write_file(copy_dir.file("w-corrupt.dfs"), without_footer(bytes));

        const auto check = [&](std::size_t threads) {
            ScopedDefaultRuntime runtime(threads);
            auto snapshot = engine::Snapshot::open(copy_dir.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->skipped().empty());
            CHECK(snapshot->state(spec, scopes[1]) == ScopeState::Bad);
            CHECK(snapshot->state(spec, scopes[0]) == ScopeState::Ready);
            CHECK(snapshot->state(spec, scopes[2]) == ScopeState::Ready);
            auto hidden = snapshot->get(spec, scopes[1],
                                        view_of(dstd::String("b0-key-0000")));
            REQUIRE(hidden.has_value());
            CHECK_FALSE(hidden->has_value());
            auto visible = snapshot->get(spec, scopes[2],
                                         view_of(dstd::String("b0-key-0000")));
            REQUIRE(visible.has_value());
            REQUIRE(visible->has_value());
            CHECK(to_std((*visible)->bytes()) == "value-0");
            auto cursor = snapshot->scan(spec, scopes[0], dstd::ByteView{},
                                         dstd::ByteView{});
            CHECK(collect(cursor).size() == 1000);
            CHECK(snapshot->open_stats().files == 1);
            CHECK(snapshot->open_stats().extents == 3);
            CHECK(snapshot->read_stats().commits_verified == 3);
            CHECK(snapshot->read_stats().read_calls ==
                  3 + snapshot->read_stats().page_reads);
        };
        check(1);
        check(8);

        // A later snapshot of one store does not check a good commit again,
        // and still finds the bad one.
        const auto verified =
            dstd::make_shared<engine::detail::VerifiedCommits>();
        const auto read_all = [&](const engine::Snapshot& snapshot) {
            CHECK(snapshot.state(spec, scopes[1]) == ScopeState::Bad);
            for (const std::size_t at : {std::size_t{0}, std::size_t{2}}) {
                auto cursor = snapshot.scan(spec, scopes[at], dstd::ByteView{},
                                            dstd::ByteView{});
                CHECK(collect(cursor).size() == 1000);
            }
        };
        auto first = engine::Snapshot::open(copy_dir.str(), verified);
        REQUIRE(first.has_value());
        read_all(*first);
        CHECK(first->read_stats().commits_verified == 3);
        auto second = engine::Snapshot::open(copy_dir.str(), verified);
        REQUIRE(second.has_value());
        read_all(*second);
        CHECK(second->read_stats().commits_verified == 1);
    }

    TEST_CASE("a block that cannot be read fails its queries with the error") {
        dftu_utils_test::ScopedTestDir dir("snapshot_read_error");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::Vector<ScopeKey> scopes = {
            file_scope("/traces/a.pfw"), file_scope("/traces/b.pfw"),
            file_scope("/traces/c.pfw"), file_scope("/traces/d.pfw")};
        write_scopes_file(dir, spec, scopes, 1000, true);
        const dstd::String path = only_data_file(dir.str());
        write_file(path, without_footer(read_file(path)));
        drop_summary(dir.str());
        const dstd::String key_text = "b0-key-0000";
        const dstd::ByteView key = view_of(key_text);

        ScopedDefaultRuntime runtime(1);
        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());

        // A directory put in place of the snapshot's descriptor makes every
        // read of the file fail with EISDIR, an error that is not a bad crc.
        const int data_fd = descriptor_of(path);
        REQUIRE(data_fd >= 0);
        const int saved_fd = ::dup(data_fd);
        REQUIRE(saved_fd >= 0);
        const int dir_fd = ::open(dir.str().c_str(), O_RDONLY);
        REQUIRE(dir_fd >= 0);
        REQUIRE(::dup2(dir_fd, data_fd) == data_fd);

        auto failed = snapshot->get(spec, scopes[0], key);
        CHECK_FALSE(failed.has_value());
        auto cursor =
            snapshot->scan(spec, scopes[1], dstd::ByteView{}, dstd::ByteView{});
        CHECK(cursor.at_end());
        REQUIRE(cursor.error().has_value());
        CHECK(snapshot->state(spec, scopes[2]) == ScopeState::Bad);
        CHECK(snapshot->read_stats().commits_verified == 0);

        REQUIRE(::dup2(saved_fd, data_fd) == data_fd);
        ::close(saved_fd);
        ::close(dir_fd);
        auto still = snapshot->get(spec, scopes[0], key);
        CHECK_FALSE(still.has_value());
        auto fresh = snapshot->get(spec, scopes[3], key);
        REQUIRE(fresh.has_value());
        REQUIRE(fresh->has_value());
        CHECK(to_std((*fresh)->bytes()) == "value-0");
        CHECK(snapshot->read_stats().commits_verified == 1);
        CHECK_FALSE(snapshot->check().has_value());
    }

    TEST_CASE(
        "a bad extent of a closed file fails only the queries that read it") {
        dftu_utils_test::ScopedTestDir dir("snapshot_synced_bad_extent");
        dftu_utils_test::ScopedTestDir copy_dir(
            "snapshot_synced_bad_extent_copy");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey both = file_scope("/traces/both.pfw");
        const ScopeKey late = file_scope("/traces/late.pfw");
        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            const auto put = [&](const ScopeKey& scope,
                                 const dstd::String& tag) {
                Batch entries = writer->batch(*id, scope, 0);
                for (int k = 0; k < 100; ++k) {
                    const dstd::String key = tag + "-" + key_of(k);
                    const dstd::String value = value_of(k);
                    entries.put(view_of(key), view_of(value));
                }
                REQUIRE(writer->commit(std::move(entries)).has_value());
            };
            put(both, "b0");
            REQUIRE(flush_writer(*writer).has_value());
            put(both, "b1");
            put(late, "late");
            REQUIRE(flush_writer(*writer).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }
        const dstd::String path = only_data_file(dir.str());
        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        CHECK(reader->index().closed);
        const dstd::Vector<store::engine::IndexedExtentRef> refs =
            extent_refs_of(*reader);
        const store::engine::ExtentRefItem* late_extent = nullptr;
        for (const store::engine::IndexedExtentRef& ref : refs) {
            for (const store::engine::ExtentSegment& segment :
                 ref.ref.segments) {
                if (to_std(view_of(segment.first_key)).rfind("late-", 0) == 0) {
                    late_extent = &ref.ref;
                }
            }
        }
        REQUIRE(late_extent != nullptr);
        dstd::Vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(late_extent->offset + late_extent->length <= bytes.size());
        const auto late_at = static_cast<std::size_t>(late_extent->offset);
        auto late_pages = store::engine::ExtentPages::open(
            dstd::ByteView{bytes.data() + late_at, late_extent->length});
        REQUIRE(late_pages.has_value());
        bytes[late_at + late_pages->entry(0).stored_offset] ^= 0xFFU;
        const dstd::String copy_path = copy_dir.file("w-synced-bad.dfs");
        write_file(copy_path, bytes);

        ScopedDefaultRuntime runtime(1);
        auto snapshot = engine::Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, late) == ScopeState::Ready);
        CHECK(snapshot->read_stats().extents_read == 0);

        const dstd::String late_key = "late-" + key_of(0);
        auto failed = snapshot->get(spec, late, view_of(late_key));
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().is(dstd::Condition::Parse));
        auto late_cursor =
            snapshot->scan(spec, late, dstd::ByteView{}, dstd::ByteView{});
        CHECK(late_cursor.at_end());
        REQUIRE(late_cursor.error().has_value());
        CHECK(late_cursor.error()->is(dstd::Condition::Parse));

        const dstd::String both_key = "b1-" + key_of(0);
        auto found = snapshot->get(spec, both, view_of(both_key));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == value_of(0));
        CHECK(snapshot->read_stats().extents_read == 2);

        const int data_fd = descriptor_of(copy_path);
        REQUIRE(data_fd >= 0);
        const int saved_fd = ::dup(data_fd);
        REQUIRE(saved_fd >= 0);
        const int dir_fd = ::open(copy_dir.str().c_str(), O_RDONLY);
        REQUIRE(dir_fd >= 0);
        REQUIRE(::dup2(dir_fd, data_fd) == data_fd);
        const dstd::String first_key = "b0-" + key_of(0);
        auto unreadable = snapshot->get(spec, both, view_of(first_key));
        REQUIRE_FALSE(unreadable.has_value());
        CHECK(unreadable.error().is(dstd::Condition::Io));
        REQUIRE(::dup2(saved_fd, data_fd) == data_fd);
        ::close(saved_fd);
        ::close(dir_fd);
        auto still = snapshot->get(spec, both, view_of(first_key));
        REQUIRE_FALSE(still.has_value());
        CHECK(still.error().is(dstd::Condition::Io));
        auto again = snapshot->get(spec, both, view_of(both_key));
        REQUIRE(again.has_value());
        CHECK(again->has_value());
    }

    TEST_CASE(
        "a durable commit marks its extents synced and a close its file") {
        dftu_utils_test::ScopedTestDir dir("snapshot_synced_mark");
        dftu_utils_test::ScopedTestDir cut_dir("snapshot_synced_mark_cut");
        NamespaceSpec durable_spec = put_spec("dftu.test.durable");
        durable_spec.durable = true;
        const NamespaceSpec loose_spec = put_spec("dftu.test.loose");
        const ScopeKey scope = file_scope("/traces/mark.pfw");
        const dstd::String key_text = "b0-" + key_of(0);

        auto writer = make_writer(dir);
        auto durable_id = writer->add_namespace(durable_spec);
        auto loose_id = writer->add_namespace(loose_spec);
        REQUIRE(durable_id.has_value());
        REQUIRE(loose_id.has_value());
        const auto put = [&](store::engine::NamespaceId id) {
            Batch entries = writer->batch(id, scope, 0);
            for (int k = 0; k < 100; ++k) {
                const dstd::String key = "b0-" + key_of(k);
                const dstd::String value = value_of(k);
                entries.put(view_of(key), view_of(value));
            }
            REQUIRE(writer->commit(std::move(entries)).has_value());
            REQUIRE(flush_writer(*writer).has_value());
        };
        put(*durable_id);
        put(*loose_id);
        const dstd::String path = dstd::String(writer->path());
        {
            auto open_file = engine::DataFileReader::open(path);
            REQUIRE(open_file.has_value());
            CHECK_FALSE(open_file->index().closed);
            const auto& refs = extent_refs_of(*open_file);
            REQUIRE(refs.size() == 2);
            const auto& first =
                refs[0].sequence < refs[1].sequence ? refs[0] : refs[1];
            const auto& second =
                refs[0].sequence < refs[1].sequence ? refs[1] : refs[0];
            CHECK(first.ref.synced == 1);
            CHECK(second.ref.synced == 0);
        }
        REQUIRE(close_writer(*writer).has_value());
        auto closed_file = engine::DataFileReader::open(path);
        REQUIRE(closed_file.has_value());
        CHECK(closed_file->index().closed);

        write_file(cut_dir.file("w-mark.dfs"), without_footer(read_file(path)));
        ScopedDefaultRuntime runtime(1);
        auto snapshot = engine::Snapshot::open(cut_dir.str());
        REQUIRE(snapshot.has_value());
        auto durable_found =
            snapshot->get(durable_spec, scope, view_of(key_text));
        REQUIRE(durable_found.has_value());
        CHECK(durable_found->has_value());
        CHECK(snapshot->read_stats().commits_verified == 0);
        auto loose_found = snapshot->get(loose_spec, scope, view_of(key_text));
        REQUIRE(loose_found.has_value());
        CHECK(loose_found->has_value());
        CHECK(snapshot->read_stats().commits_verified == 1);
    }

    TEST_CASE(
        "a scan reads each scope's extents only when it reaches the scope") {
        dftu_utils_test::ScopedTestDir dir("snapshot_lazy_scan");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::Vector<ScopeKey> scopes = {file_scope("/traces/a.pfw"),
                                               file_scope("/traces/b.pfw"),
                                               file_scope("/traces/c.pfw")};
        write_scopes_file(dir, spec, scopes, 1000, true);
        ScopedDefaultRuntime runtime(1);
        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());

        // The begin bound is a temporary: the scan keeps its own copy, and
        // reads it each time a segment's extent loads.
        auto cursor = snapshot->scan(
            spec, dstd::Span<const ScopeKey>(scopes.data(), scopes.size()),
            view_of(dstd::String("b0-") + key_of(500)), dstd::ByteView{});
        REQUIRE_FALSE(cursor.at_end());
        CHECK(to_std(cursor.key()) == "b0-" + key_of(500));
        CHECK(snapshot->read_stats().extents_read == 1);
        std::size_t rows = 0;
        while (!cursor.at_end()) {
            ++rows;
            cursor.next();
        }
        CHECK_FALSE(cursor.error().has_value());
        CHECK(rows == 3U * 500U);
        CHECK(snapshot->read_stats().extents_read == 3);
    }

    TEST_CASE(
        "closed files read through their directories as their chains do") {
        dftu_utils_test::ScopedTestDir dir("snapshot_directory_same");
        dftu_utils_test::ScopedTestDir cut_dir("snapshot_directory_same_cut");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::Vector<ScopeKey> scopes = {
            file_scope("/traces/a.pfw"), file_scope("/traces/b.pfw"),
            file_scope("/traces/c.pfw"), file_scope("/traces/d.pfw")};
        const auto put = [&](engine::Writer& writer,
                             store::engine::NamespaceId id,
                             const ScopeKey& scope, const dstd::String& tag) {
            Batch entries = writer.batch(id, scope, 0);
            for (int k = 0; k < 50; ++k) {
                const dstd::String key = "key-" + key_of(k);
                const dstd::String value = tag + "-" + value_of(k);
                entries.put(view_of(key), view_of(value));
            }
            REQUIRE(writer.commit(std::move(entries)).has_value());
        };
        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            for (std::size_t s = 0; s < 3; ++s) {
                put(*writer, *id, scopes[s], "first");
                REQUIRE(flush_writer(*writer).has_value());
            }
            REQUIRE(close_writer(*writer).has_value());
        }
        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            put(*writer, *id, scopes[0], "second");
            REQUIRE(writer->drop(*id, scopes[1]).has_value());
            const dstd::String begin = "key-" + key_of(10);
            const dstd::String end = "key-" + key_of(20);
            REQUIRE(writer->drop(*id, scopes[2], view_of(begin), view_of(end))
                        .has_value());
            put(*writer, *id, scopes[3], "second");
            REQUIRE(close_writer(*writer).has_value());
        }
        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            for (int commit = 0; commit < 150; ++commit) {
                const ScopeKey scope = file_scope(
                    "/traces/m" + dstd::to_string(commit % 4) + ".pfw");
                Batch entries = writer->batch(*id, scope, 0);
                const dstd::String key = "many-" + key_of(commit);
                entries.put(view_of(key), view_of(value_of(commit)));
                if (commit == 77) {
                    entries.put(view_of(dstd::String("many-big")),
                                view_of(dstd::String(3 * SMALL_EXTENT, 'b')));
                }
                REQUIRE(writer->commit(std::move(entries)).has_value());
                REQUIRE(flush_writer(*writer).has_value());
            }
            REQUIRE(close_writer(*writer).has_value());
        }
        for (const auto& entry : dftu_utils_test::list_dir(dir.str())) {
            if (!is_data_file(entry.path)) continue;
            const dstd::String path(entry.path.str());
            auto light = engine::DataFileReader::open(
                path, store::engine::FooterItems::Directory);
            REQUIRE(light.has_value());
            CHECK(light->index().tail.has_value());
            CHECK(light->index().scopes.empty());
            CHECK(light->index().copies.empty());
            auto full = engine::DataFileReader::open(path);
            REQUIRE(full.has_value());
            CHECK_FALSE(full->index().scopes.empty());
            write_file(cut_dir.file(entry.path.view().filename()),
                       without_footer(read_file(path)));
        }

        const auto dump = [&](const dstd::String& root) {
            auto snapshot = engine::Snapshot::open(root);
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->skipped().empty());
            dstd::String text;
            auto generations = snapshot->generations();
            REQUIRE(generations.has_value());
            for (const store::engine::ScopeGeneration& generation :
                 *generations) {
                text += "generation " +
                        dstd::String(generation.namespace_name) + " " +
                        dstd::String(generation.scope.paths[0]) + " " +
                        dstd::to_string(generation.generation) + "\n";
            }
            for (const ScopeKey& scope : scopes) {
                text += "state " + dstd::String(scope.paths[0]) + " " +
                        dstd::to_string(
                            static_cast<int>(snapshot->state(spec, scope))) +
                        "\n";
            }
            auto cursor =
                snapshot->scan_all(spec, dstd::ByteView{}, dstd::ByteView{});
            for (const ScopedRow& row : collect_scoped(cursor)) {
                text += row.scope + " " + row.key + "=" + row.value + "\n";
            }
            return text;
        };
        const dstd::String closed = dump(dir.str());
        CHECK(closed == dump(cut_dir.str()));
        CHECK(closed.find("/traces/m1.pfw many-" + key_of(149)) !=
              dstd::String::npos);
        CHECK(closed.find("/traces/m1.pfw many-big=" +
                          dstd::String(3 * SMALL_EXTENT, 'b')) !=
              dstd::String::npos);
        const auto checked = [](const dstd::String& root) {
            auto snapshot = engine::Snapshot::open(root);
            REQUIRE(snapshot.has_value());
            auto report = snapshot->check();
            REQUIRE(report.has_value());
            CHECK(report->bad_commits.empty());
            CHECK(report->bad_segments.empty());
            return dstd::to_string(report->commits) + " " +
                   dstd::to_string(report->blocks) + " " +
                   dstd::to_string(report->segments);
        };
        CHECK(checked(dir.str()) == checked(cut_dir.str()));
        CHECK(closed.find("/traces/a.pfw key-" + key_of(0) + "=second-") !=
              dstd::String::npos);
        CHECK(closed.find("/traces/b.pfw key-") == dstd::String::npos);
        CHECK(closed.find("/traces/c.pfw key-" + key_of(15)) ==
              dstd::String::npos);
        CHECK(closed.find("/traces/c.pfw key-" + key_of(25)) !=
              dstd::String::npos);
        CHECK(closed.find("/traces/d.pfw key-" + key_of(0)) !=
              dstd::String::npos);
    }

    TEST_CASE("a bad commit decides over one that cannot be read") {
        dftu_utils_test::ScopedTestDir dir("snapshot_bad_over_failed");
        dftu_utils_test::ScopedTestDir copy_dir(
            "snapshot_bad_over_failed_copy");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey both = file_scope("/traces/both.pfw");
        const ScopeKey late = file_scope("/traces/late.pfw");
        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            const auto put = [&](const ScopeKey& scope,
                                 const dstd::String& tag) {
                Batch entries = writer->batch(*id, scope, 0);
                for (int k = 0; k < 100; ++k) {
                    const dstd::String key = tag + "-" + key_of(k);
                    const dstd::String value = value_of(k);
                    entries.put(view_of(key), view_of(value));
                }
                REQUIRE(writer->commit(std::move(entries)).has_value());
            };
            put(both, "b0");
            REQUIRE(flush_writer(*writer).has_value());
            put(both, "b1");
            put(late, "b0");
            REQUIRE(flush_writer(*writer).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }
        const dstd::String path = only_data_file(dir.str());
        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        const auto& refs = extent_refs_of(*reader);
        const auto newest = std::max_element(refs.begin(), refs.end(),
                                             [](const auto& a, const auto& b) {
                                                 return a.sequence < b.sequence;
                                             });
        const auto oldest = std::min_element(refs.begin(), refs.end(),
                                             [](const auto& a, const auto& b) {
                                                 return a.sequence < b.sequence;
                                             });
        REQUIRE(oldest->sequence < newest->sequence);
        const store::engine::ExtentRefItem newer = newest->ref;

        dstd::Vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(newer.offset + newer.length <= bytes.size());
        bytes[static_cast<std::size_t>(newer.offset) + newer.length - 1U] ^=
            0xFFU;
        const dstd::String copy_path = copy_dir.file("w-bad-over-failed.dfs");
        write_file(copy_path, without_footer(bytes));

        ScopedDefaultRuntime runtime(1);
        auto snapshot = engine::Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, late) == ScopeState::Bad);
        CHECK(snapshot->read_stats().commits_verified == 1);

        const int data_fd = descriptor_of(copy_path);
        REQUIRE(data_fd >= 0);
        const int saved_fd = ::dup(data_fd);
        REQUIRE(saved_fd >= 0);
        const int dir_fd = ::open(copy_dir.str().c_str(), O_RDONLY);
        REQUIRE(dir_fd >= 0);
        REQUIRE(::dup2(dir_fd, data_fd) == data_fd);

        const dstd::String key_text = "b0-key-0000";
        auto found = snapshot->get(spec, both, view_of(key_text));
        REQUIRE(found.has_value());
        CHECK_FALSE(found->has_value());
        auto cursor =
            snapshot->scan(spec, both, dstd::ByteView{}, dstd::ByteView{});
        CHECK(cursor.at_end());
        CHECK_FALSE(cursor.error().has_value());
        CHECK(snapshot->state(spec, both) == ScopeState::Bad);

        REQUIRE(::dup2(saved_fd, data_fd) == data_fd);
        ::close(saved_fd);
        ::close(dir_fd);
    }

    TEST_CASE("a cut past a run drops the extents it leaves behind") {
        dftu_utils_test::ScopedTestDir dir("snapshot_open_cut");
        dftu_utils_test::ScopedTestDir cut_dir("snapshot_open_cut_copy");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::Vector<ScopeKey> scopes = {file_scope("/traces/a.pfw"),
                                               file_scope("/traces/b.pfw"),
                                               file_scope("/traces/c.pfw")};
        write_scopes_file(dir, spec, scopes, 1000, true);
        const dstd::String path = only_data_file(dir.str());
        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        REQUIRE(extent_refs_of(*reader).size() == 3);
        const store::engine::ExtentRefItem middle =
            extent_refs_of(*reader)[1].ref;

        dstd::Vector<std::uint8_t> bytes = read_file(path);
        const std::size_t cut =
            static_cast<std::size_t>(middle.offset) + SMALL_EXTENT;
        REQUIRE(cut < bytes.size());
        write_file(cut_dir.file("w-cut.dfs"),
                   dstd::Vector<std::uint8_t>(
                       bytes.begin(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(cut)));

        const auto check = [&](std::size_t threads) {
            ScopedDefaultRuntime runtime(threads);
            auto snapshot = engine::Snapshot::open(cut_dir.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->skipped().empty());
            CHECK(snapshot->state(spec, scopes[0]) == ScopeState::Ready);
            CHECK(snapshot->state(spec, scopes[1]) == ScopeState::Absent);
            CHECK(snapshot->state(spec, scopes[2]) == ScopeState::Absent);
            auto found = snapshot->get(spec, scopes[0],
                                       view_of(dstd::String("b0-key-0999")));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == "value-999");
            CHECK(snapshot->open_stats().files == 1);
            CHECK(snapshot->open_stats().extents == 1);
            CHECK(snapshot->read_stats().commits_verified == 1);
            CHECK(snapshot->read_stats().read_calls ==
                  1 + snapshot->read_stats().page_reads);
        };
        check(1);
        check(8);
    }

    TEST_CASE("a cut that loses every record skips the whole file") {
        dftu_utils_test::ScopedTestDir dir("snapshot_open_cut_records");
        dftu_utils_test::ScopedTestDir cut_dir(
            "snapshot_open_cut_records_copy");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey scope = file_scope("/traces/a.pfw");
        write_scopes_file(cut_dir, spec, {scope}, 4000, false);
        const dstd::String copied = only_data_file(cut_dir.str());
        dstd::Vector<std::uint8_t> bytes = read_file(copied);
        auto copied_reader = engine::DataFileReader::open(copied);
        REQUIRE(copied_reader.has_value());
        REQUIRE(extent_refs_of(*copied_reader).size() == 1);
        const store::engine::ExtentRefItem run =
            extent_refs_of(*copied_reader)[0].ref;
        const std::uint64_t cut = run.offset + run.length / 2U;
        REQUIRE(cut < bytes.size());
        write_file(dir.file("w-cut.dfs"),
                   dstd::Vector<std::uint8_t>(
                       bytes.begin(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(cut)));
        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->skipped().size() == 1);
        CHECK(dstd::String(snapshot->skipped()[0].name) == "w-cut.dfs");
        CHECK(snapshot->open_stats().files == 0);
        CHECK(snapshot->open_stats().extents == 0);
        CHECK(snapshot->state(spec, scope) == ScopeState::Absent);
    }

    TEST_CASE("a closed file of many small extents opens through its end") {
        dftu_utils_test::ScopedTestDir dir("snapshot_small_extents_light");
        const ScopeKey scope = file_scope("/traces/small.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        engine::WriterOptions options = small_extents();
        options.extent_size = 1U << 20U;
        dstd::String path;
        {
            auto writer = make_writer(dir, options);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            for (int b = 0; b < 40; ++b) {
                Batch batch = writer->batch(*id, scope, 0);
                batch.put(view_of("k" + dstd::to_string(100 + b)),
                          view_of(dstd::String("v")));
                REQUIRE(writer->commit(std::move(batch)).has_value());
                REQUIRE(flush_writer(*writer).has_value());
            }
            path = dstd::String(writer->path());
            REQUIRE(close_writer(*writer).has_value());
        }
        auto light = engine::DataFileReader::open(
            path, store::engine::FooterItems::Directory);
        REQUIRE(light.has_value());
        REQUIRE(light->index().tail.has_value());
        REQUIRE(light->index().tail->extent_count >
                dstd::fs::file_size(path).value() / (1U << 20U) + 1);
        CHECK(light->records_read() == 1);
    }

    TEST_CASE("an open indexes closed files from the summary alone") {
        dftu_utils_test::ScopedTestDir dir("snapshot_summary_open");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        constexpr int FILES = 6;
        dstd::Vector<ScopeKey> scopes;
        for (int f = 0; f < FILES; ++f) {
            scopes.push_back(
                file_scope("/traces/s" + dstd::to_string(f) + ".pfw"));
            write_scoped_file(dir, spec, scopes.back(), 2, 40, true);
        }
        auto listed = store::engine::read_summary(dir.str());
        REQUIRE(listed.has_value());
        CHECK(listed->size() == FILES);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->open_stats().files == FILES);
        CHECK(snapshot->open_stats().files_summarized == FILES);
        CHECK(snapshot->read_stats().read_calls == 0);
        for (int f = 0; f < FILES; ++f) {
            const ScopeKey& scope = scopes[static_cast<std::size_t>(f)];
            auto found = snapshot->get(spec, scope, view_of("b1-" + key_of(9)));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == value_of(9));
            auto cursor =
                snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
            CHECK(collect(cursor).size() == 80);
        }
        auto report = snapshot->check();
        REQUIRE(report.has_value());
        CHECK(report->bad_commits.empty());
        CHECK(report->bad_segments.empty());
    }

    TEST_CASE("files the summary does not list open as before") {
        dftu_utils_test::ScopedTestDir dir("snapshot_summary_partial");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        dstd::Vector<ScopeKey> scopes;
        for (int f = 0; f < 4; ++f) {
            scopes.push_back(
                file_scope("/traces/p" + dstd::to_string(f) + ".pfw"));
            write_scoped_file(dir, spec, scopes.back(), 1, 30, true);
        }
        const dstd::String summary =
            dstd::String((dstd::Path(dir.str()) /
                          dstd::String(store::engine::SUMMARY_FILE_NAME))
                             .str());
        dstd::Vector<std::uint8_t> bytes = read_file(summary);
        auto listed = store::engine::read_summary(dir.str());
        REQUIRE(listed.has_value());
        REQUIRE(listed->size() == 4);

        const auto reads_all = [&](std::size_t summarized) {
            auto snapshot = engine::Snapshot::open(dir.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->open_stats().files == 4);
            CHECK(snapshot->open_stats().files_summarized == summarized);
            for (const ScopeKey& scope : scopes) {
                auto cursor = snapshot->scan(spec, scope, dstd::ByteView{},
                                             dstd::ByteView{});
                CHECK(collect(cursor).size() == 30);
            }
        };
        SUBCASE("a torn entry ends the summary") {
            const std::size_t first = 12 + dstd::read_u32_le(bytes.data() + 4);
            bytes[first + 20] ^= 0xFFU;
            write_file(summary, bytes);
            reads_all(1);
        }
        SUBCASE("a cut entry ends the summary") {
            bytes.resize(bytes.size() - 7);
            write_file(summary, bytes);
            reads_all(3);
        }
        SUBCASE("no summary") {
            drop_summary(dir.str());
            reads_all(0);
        }
    }

    TEST_CASE("a summary entry of a file that is gone is passed over") {
        dftu_utils_test::ScopedTestDir dir("snapshot_summary_gone");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey kept = file_scope("/traces/kept.pfw");
        const ScopeKey gone = file_scope("/traces/gone.pfw");
        write_scoped_file(dir, spec, kept, 1, 20, true);
        write_scoped_file(dir, spec, gone, 1, 20, true);
        auto listed = store::engine::read_summary(dir.str());
        REQUIRE(listed.has_value());
        REQUIRE(listed->size() == 2);
        (void)dstd::fs::remove(dstd::Path(dir.str()) /
                               dstd::String(listed->back().name.data(),
                                            listed->back().name.size()));

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->open_stats().files == 1);
        CHECK(snapshot->open_stats().files_summarized == 1);
        CHECK(snapshot->skipped().empty());
        auto cursor =
            snapshot->scan(spec, kept, dstd::ByteView{}, dstd::ByteView{});
        CHECK(collect(cursor).size() == 20);
        CHECK(snapshot->state(spec, gone) == ScopeState::Absent);
    }

    TEST_CASE("a file shorter than its summary entry fails its queries") {
        dftu_utils_test::ScopedTestDir dir("snapshot_summary_short");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey scope = file_scope("/traces/short.pfw");
        write_scoped_file(dir, spec, scope, 1, 20, true);
        const dstd::String path = only_data_file(dir.str());
        dstd::Vector<std::uint8_t> bytes = read_file(path);
        bytes.resize(store::engine::FILE_HEADER_BYTES);
        write_file(path, bytes);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->open_stats().files_summarized == 1);
        auto failed = snapshot->get(spec, scope, view_of("b0-" + key_of(3)));
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().is(dstd::Condition::Parse));
    }

    TEST_CASE("a summary entry holds when a crash loses the footer") {
        dftu_utils_test::ScopedTestDir dir("snapshot_summary_footer_lost");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey scope = file_scope("/traces/lost.pfw");
        write_scoped_file(dir, spec, scope, 3, 60, true);
        const dstd::String path = only_data_file(dir.str());
        write_file(path, without_footer(read_file(path)));

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->open_stats().files_summarized == 1);
        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        CHECK(collect(cursor).size() == 180);
        auto found = snapshot->get(spec, scope, view_of("b2-" + key_of(59)));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == value_of(59));
    }

    TEST_CASE("writers that close at once each add one whole summary entry") {
        dftu_utils_test::ScopedTestDir dir("snapshot_summary_threads");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        constexpr int WRITERS = 8;
        dstd::Vector<dstd::Thread> threads;
        for (int w = 0; w < WRITERS; ++w) {
            threads.emplace_back([&dir, &spec, w] {
                write_scoped_file(
                    dir, spec,
                    file_scope("/traces/t" + dstd::to_string(w) + ".pfw"), 2,
                    50, true);
            });
        }
        for (dstd::Thread& thread : threads) thread.join();
        auto listed = store::engine::read_summary(dir.str());
        REQUIRE(listed.has_value());
        CHECK(listed->size() == WRITERS);
        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->open_stats().files_summarized == WRITERS);
    }

    TEST_CASE("a scope lookup skips the files whose filter rules it out") {
        dftu_utils_test::ScopedTestDir dir("snapshot_scope_filter");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        constexpr int FILES = 12;
        dstd::Vector<ScopeKey> scopes;
        for (int f = 0; f < FILES; ++f) {
            scopes.push_back(
                file_scope("/traces/f" + dstd::to_string(f) + ".pfw"));
            write_scoped_file(dir, spec, scopes.back(), 1, 50, true);
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->open_stats().files == FILES);
        auto found =
            snapshot->get(spec, scopes[FILES / 2], view_of("b0-" + key_of(7)));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == value_of(7));
        CHECK(snapshot->read_stats().files_filtered == FILES - 1);
        CHECK(snapshot->read_stats().metadata_blocks_read == 2);

        const ScopeKey absent = file_scope("/traces/absent.pfw");
        auto missing = snapshot->get(spec, absent, view_of("b0-" + key_of(7)));
        REQUIRE(missing.has_value());
        CHECK_FALSE(missing->has_value());
        CHECK(snapshot->read_stats().files_filtered == 2 * FILES - 1);
        CHECK(snapshot->read_stats().metadata_blocks_read == 2);

        for (int f = 0; f < FILES; ++f) {
            auto cursor =
                snapshot->scan(spec, scopes[static_cast<std::size_t>(f)],
                               dstd::ByteView{}, dstd::ByteView{});
            CHECK(collect(cursor).size() == 50);
        }
    }

    TEST_CASE("the page cache size comes from its variable") {
        dftu_utils_test::ScopedTestDir dir("snapshot_page_cache_env");
        const ScopeKey scope = file_scope("/traces/a.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        write_scoped_file(dir, spec, scope, 3, 500, true);
        const char* name = dftracer::utils::common::constants::store::
            STORE_PAGE_CACHE_BYTES_ENV;

        ::setenv(name, "many", 1);
        auto refused = engine::Snapshot::open(dir.str());
        ::unsetenv(name);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().is(dstd::Condition::InvalidArgument));

        ::setenv(name, "0", 1);
        auto tiny = engine::Snapshot::open(dir.str());
        ::unsetenv(name);
        REQUIRE(tiny.has_value());
        for (int k = 0; k < 500; k += 37) {
            const dstd::String key = "b2-" + key_of(k);
            auto found = tiny->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == value_of(k));
        }
        auto cursor =
            tiny->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        CHECK(collect(cursor).size() == 1500);
    }

    TEST_CASE(
        "an open reads no block and a query reads only the extent it needs") {
        dftu_utils_test::ScopedTestDir dir("snapshot_lazy_open");
        const ScopeKey first = file_scope("/traces/a.pfw");
        const ScopeKey second = file_scope("/traces/b.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        write_scoped_file(dir, spec, first, 3, 500, true);
        write_scoped_file(dir, spec, second, 2, 500, true);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->open_stats().files == 2);
        const store::engine::ReadStats cold = snapshot->read_stats();
        CHECK(cold.commits_verified == 0);
        CHECK(cold.extents_read == 0);
        CHECK(cold.read_calls == 0);
        CHECK(cold.bytes_read == 0);

        auto found =
            snapshot->get(spec, second, view_of(dstd::String("b0-key-0000")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == value_of(0));
        const store::engine::ReadStats touched = snapshot->read_stats();
        CHECK(touched.commits_verified == 0);
        CHECK(touched.indexes_read == 0);
        CHECK(touched.files_filtered == 1);
        CHECK(touched.metadata_blocks_read == 2);
        CHECK(touched.extents_read == 1);
        CHECK(touched.page_reads == 1);
        CHECK(touched.read_calls == 4);
        CHECK(touched.bytes_read > 0);

        auto other =
            snapshot->get(spec, first, view_of(dstd::String("b0-key-0499")));
        REQUIRE(other.has_value());
        REQUIRE(other->has_value());
        CHECK(to_std((*other)->bytes()) == value_of(499));
        const store::engine::ReadStats both = snapshot->read_stats();
        CHECK(both.commits_verified == 0);
        CHECK(both.indexes_read == 0);
        CHECK(both.files_filtered == 2);
        CHECK(both.metadata_blocks_read == 4);
        CHECK(both.extents_read == 2);
        CHECK(both.page_reads == 2);
        CHECK(both.read_calls == 8);
        CHECK(snapshot->open_stats().commits == 0);
        CHECK(snapshot->open_stats().extents == 0);
    }

    TEST_CASE(
        "a corrupt commit stops only the scopes that hold a segment of it") {
        dftu_utils_test::ScopedTestDir dir("snapshot_lazy_bad");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_lazy_bad_copy");
        const ScopeKey broken = file_scope("/traces/broken.pfw");
        const ScopeKey shared = file_scope("/traces/shared.pfw");
        const ScopeKey sound = file_scope("/traces/sound.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        for (const ScopeKey& scope : {broken, shared}) {
            Batch entries = writer->batch(*id, scope, 0);
            const dstd::String key =
                dstd::String(scope.paths[0]) == "/traces/broken.pfw"
                    ? "broken-1"
                    : "shared-1";
            entries.put(view_of(key), view_of(dstd::String("value-1")));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        REQUIRE(flush_writer(*writer).has_value());
        {
            Batch entries = writer->batch(*id, sound, 0);
            entries.put(view_of(dstd::String("sound-1")),
                        view_of(dstd::String("sound-value-1")));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());

        auto reader =
            engine::DataFileReader::open(dstd::String(writer->path()));
        REQUIRE(reader.has_value());
        REQUIRE(extent_refs_of(*reader).size() == 2);
        const store::engine::ExtentRef dead{
            extent_refs_of(*reader)[0].ref.offset,
            extent_refs_of(*reader)[0].ref.length,
            extent_refs_of(*reader)[0].ref.crc};

        dstd::Vector<std::uint8_t> bytes =
            read_file(dstd::String(writer->path()));
        REQUIRE(dead.offset + dead.length <= bytes.size());
        bytes[static_cast<std::size_t>(dead.offset) + 4U] ^= 0xFFU;
        write_file(copy_dir.file("w-bad.dfs"), without_footer(bytes));

        auto snapshot = engine::Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, broken) == ScopeState::Bad);
        CHECK(snapshot->state(spec, shared) == ScopeState::Bad);
        CHECK(snapshot->read_stats().commits_verified == 1);

        auto visible =
            snapshot->get(spec, sound, view_of(dstd::String("sound-1")));
        REQUIRE(visible.has_value());
        REQUIRE(visible->has_value());
        CHECK(to_std((*visible)->bytes()) == "sound-value-1");
        const store::engine::ReadStats after = snapshot->read_stats();
        CHECK(after.commits_verified == 2);
        CHECK(after.extents_read == 2);

        for (const ScopeKey& scope : {broken, shared}) {
            auto hidden =
                snapshot->get(spec, scope, view_of(dstd::String("broken-1")));
            REQUIRE(hidden.has_value());
            CHECK_FALSE(hidden->has_value());
        }
        CHECK(snapshot->read_stats().commits_verified == 2);
    }

    TEST_CASE("a lost big value run reads Bad at its first touch") {
        dftu_utils_test::ScopedTestDir dir("snapshot_lazy_big");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_lazy_big_copy");
        const ScopeKey big_scope = file_scope("/traces/big.pfw");
        const ScopeKey sound_scope = file_scope("/traces/sound.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::String big(2 * 1024 * 1024, 'x');

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        {
            Batch entries = writer->batch(*id, big_scope, 0);
            entries.put(view_of(dstd::String("big")), view_of(big));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());
        write_scoped_file(copy_dir, spec, sound_scope, 1, 20, true);

        auto reader =
            engine::DataFileReader::open(dstd::String(writer->path()));
        REQUIRE(reader.has_value());
        bool found_run = false;
        store::engine::ExtentRef run{};
        for (const store::engine::IndexedExtentRef& extent :
             extent_refs_of(*reader)) {
            if (!extent.ref.segments.empty()) continue;
            REQUIRE_FALSE(found_run);
            run = {extent.ref.offset, extent.ref.length, extent.ref.crc};
            found_run = true;
        }
        REQUIRE(found_run);
        CHECK(run.length < big.size());

        dstd::Vector<std::uint8_t> bytes =
            read_file(dstd::String(writer->path()));
        REQUIRE(run.offset + run.length <= bytes.size());
        std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(run.offset),
                  bytes.begin() +
                      static_cast<std::ptrdiff_t>(run.offset + run.length),
                  std::uint8_t{0});
        write_file(copy_dir.file("w-big.dfs"), without_footer(bytes));

        auto snapshot = engine::Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->read_stats().extents_read == 0);
        CHECK(snapshot->state(spec, big_scope) == ScopeState::Bad);
        auto hidden =
            snapshot->get(spec, big_scope, view_of(dstd::String("big")));
        REQUIRE(hidden.has_value());
        CHECK_FALSE(hidden->has_value());

        CHECK(snapshot->state(spec, sound_scope) == ScopeState::Ready);
        auto visible = snapshot->get(spec, sound_scope,
                                     view_of(dstd::String("b0-key-0000")));
        REQUIRE(visible.has_value());
        REQUIRE(visible->has_value());
        CHECK(to_std((*visible)->bytes()) == value_of(0));
    }

    TEST_CASE("sixteen threads share the first touch of one cold snapshot") {
        dftu_utils_test::ScopedTestDir dir("snapshot_lazy_threads");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        constexpr int FILES = 4;
        constexpr int BATCHES = 3;
        constexpr int KEYS = 200;
        dstd::Vector<ScopeKey> scopes;
        for (int file = 0; file < FILES; ++file) {
            scopes.push_back(
                file_scope("/traces/t" + dstd::to_string(file) + ".pfw"));
        }
        for (const ScopeKey& scope : scopes) {
            write_scoped_file(dir, spec, scope, BATCHES, KEYS, true);
        }

        dftu_utils_test::ScopedTestDir cut_dir("snapshot_lazy_threads_cut");
        for (const auto& entry : dftu_utils_test::list_dir(dir.str())) {
            if (!is_data_file(entry.path)) continue;
            write_file(cut_dir.file(entry.path.view().filename()),
                       without_footer(read_file(entry.path)));
        }
        const auto touch_all = [&](const dstd::String& root, bool synced) {
            auto opened = engine::Snapshot::open(root);
            REQUIRE(opened.has_value());
            const engine::Snapshot& snapshot = *opened;
            CHECK(snapshot.open_stats().files == FILES);
            CHECK(snapshot.read_stats().extents_read == 0);

            std::atomic<int> failures{0};
            dstd::Vector<dstd::Thread> threads;
            for (int thread = 0; thread < 16; ++thread) {
                threads.emplace_back([&snapshot, &scopes, &spec, &failures,
                                      thread] {
                    const ScopeKey& scope =
                        scopes[static_cast<std::size_t>(thread) %
                               scopes.size()];
                    for (int batch = 0; batch < BATCHES; ++batch) {
                        for (int k = 0; k < KEYS; ++k) {
                            const dstd::String key =
                                "b" + dstd::to_string(batch) + "-" + key_of(k);
                            auto found =
                                snapshot.get(spec, scope, view_of(key));
                            if (!found.has_value() || !found->has_value() ||
                                to_std((*found)->bytes()) != value_of(k)) {
                                failures.fetch_add(1);
                            }
                        }
                    }
                    auto cursor = snapshot.scan(spec, scope, dstd::ByteView{},
                                                dstd::ByteView{});
                    std::size_t rows = 0;
                    while (!cursor.at_end()) {
                        auto value = cursor.value();
                        if (!value.has_value()) {
                            failures.fetch_add(1);
                        } else {
                            ++rows;
                        }
                        cursor.next();
                    }
                    if (rows != static_cast<std::size_t>(BATCHES * KEYS)) {
                        failures.fetch_add(1);
                    }
                });
            }
            for (dstd::Thread& thread : threads) thread.join();
            CHECK(failures.load() == 0);

            const store::engine::ReadStats reads = snapshot.read_stats();
            CHECK(reads.commits_verified ==
                  (synced ? 0U : static_cast<std::uint64_t>(FILES * BATCHES)));
            auto report = snapshot.check();
            REQUIRE(report.has_value());
            CHECK(reads.extents_read == report->blocks);
        };
        touch_all(dir.str(), true);
        touch_all(cut_dir.str(), false);
    }

    TEST_CASE("check reports every commit and segment that does not verify") {
        dftu_utils_test::ScopedTestDir built("snapshot_check_built");
        const ScopeKey scope = file_scope("/traces/check.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::String path = built.file("w-check.dfs");

        // A segment of two entries of equal size, with the two of them swapped:
        // every offset, length and count in it stays valid, its first key in
        // the index no longer matches its first entry, and its keys descend.
        // The extent crc is the one the writer computes over those bytes.
        store::engine::ExtentRefItem listed{};
        std::uint32_t offset_in_extent = 0;
        {
            store::engine::SegmentBuilder builder;
            builder.add(view_of(dstd::String("k1")),
                        view_of(dstd::String("v1")));
            builder.add(view_of(dstd::String("k2")),
                        view_of(dstd::String("v2")));
            auto one = builder.seal(nullptr, nullptr, nullptr);
            REQUIRE(one.has_value());
            dstd::Vector<std::uint8_t> segment = std::move(*one);

            constexpr std::size_t HEADER = 20;
            constexpr std::size_t GRANULE_INDEX = 8 + 1 + 2;
            constexpr std::size_t ENTRY = 6;
            REQUIRE(segment.size() >= HEADER + GRANULE_INDEX + 2 * ENTRY);
            std::rotate(segment.begin() + HEADER + GRANULE_INDEX,
                        segment.begin() + HEADER + GRANULE_INDEX + ENTRY,
                        segment.begin() + HEADER + GRANULE_INDEX + 2 * ENTRY);

            auto file = store::engine::DataFileWriter::create(
                path, SMALL_EXTENT, "host", "label");
            REQUIRE(file.has_value());
            store::engine::NamespaceItem namespaces;
            namespaces.local_id = 1;
            namespaces.name = dstd::String("dftu.test.put");
            namespaces.format_version = 0;
            file->add(std::move(namespaces));
            store::engine::ScopeItem scope_item;
            scope_item.local_id = 1;
            scope_item.kind = store::ScopeKind::File;
            scope_item.paths.push_back(dstd::String("/traces/check.pfw"));
            file->add(std::move(scope_item));
            store::engine::CopyItem copy;
            copy.local_id = 1;
            copy.namespace_id = 1;
            copy.scope_id = 1;
            copy.generation = 1;
            copy.state = store::engine::CopyState::Ready;
            file->add(std::move(copy));

            auto reference = file->append_extent(view_of(segment));
            REQUIRE(reference.has_value());
            listed.offset = reference->offset;
            listed.length = reference->length;
            listed.crc = reference->crc;
            listed.sequence = 1;
            store::engine::ExtentSegment extent_segment;
            extent_segment.copy_id = 1;
            extent_segment.offset_in_extent = 0;
            extent_segment.length = static_cast<std::uint32_t>(segment.size());
            extent_segment.entry_count = 2;
            extent_segment.first_key = bytes_vector("k1");
            extent_segment.last_key = bytes_vector("k2");
            listed.segments.push_back(std::move(extent_segment));
            file->add(listed);
            REQUIRE(file->commit().has_value());
            REQUIRE(file->close().has_value());
            offset_in_extent = 0;
        }

        auto snapshot = engine::Snapshot::open(built.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->read_stats().extents_read == 0);

        auto found = snapshot->get(spec, scope, view_of(dstd::String("k2")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == "v2");
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);

        const store::engine::ReadStats before = snapshot->read_stats();
        auto report = snapshot->check();
        REQUIRE(report.has_value());
        CHECK(report->files == 1);
        CHECK(report->commits == 1);
        CHECK(report->blocks == 1);
        CHECK(report->bad_commits.empty());
        CHECK(report->segments == 1);
        REQUIRE(report->bad_segments.size() == 1);
        CHECK(dstd::String(report->bad_segments[0].file) == "w-check.dfs");
        CHECK(report->bad_segments[0].extent_offset == listed.offset);
        CHECK(report->bad_segments[0].offset_in_extent == offset_in_extent);
        CHECK(snapshot->read_stats().extents_read == before.extents_read + 1);
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
    }

    TEST_CASE("check reads every block and reports the commit that fails") {
        dftu_utils_test::ScopedTestDir dir("snapshot_check_bad");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_check_bad_copy");
        const ScopeKey scope = file_scope("/traces/a.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        write_scoped_file(dir, spec, scope, 3, 20, true);

        const dstd::String path = only_data_file(dir.str());
        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        REQUIRE(extent_refs_of(*reader).size() == 3);
        const store::engine::ExtentRef dead{
            extent_refs_of(*reader)[1].ref.offset,
            extent_refs_of(*reader)[1].ref.length,
            extent_refs_of(*reader)[1].ref.crc};

        dstd::Vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(dead.offset + dead.length <= bytes.size());
        bytes[static_cast<std::size_t>(dead.offset) + 8U] ^= 0x01U;
        write_file(copy_dir.file("w-check-bad.dfs"), without_footer(bytes));

        auto snapshot = engine::Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->read_stats().extents_read == 0);

        auto report = snapshot->check();
        REQUIRE(report.has_value());
        CHECK(report->files == 1);
        CHECK(report->commits == 3);
        CHECK(report->blocks == 3);
        CHECK(report->segments == 2);
        CHECK(report->bad_segments.empty());
        REQUIRE(report->bad_commits.size() == 1);
        CHECK(dstd::String(report->bad_commits[0].file) == "w-check-bad.dfs");
        CHECK(report->bad_commits[0].sequence ==
              extent_refs_of(*reader)[1].sequence);
        CHECK(report->bytes_read > 0);
        CHECK(snapshot->state(spec, scope) == ScopeState::Bad);
    }

    TEST_CASE("a directory too large for the footer reads through its levels") {
        dftu_utils_test::ScopedTestDir dir("snapshot_directory_levels");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const dstd::String path = dir.file("w-levels.dfs");
        constexpr std::uint32_t SCOPES = 12000;
        const auto scope_path = [](std::uint32_t index) {
            dstd::String digits = dstd::to_string(index);
            return "/traces/" + dstd::String(1000, 'p') + "/s" +
                   dstd::String(7 - digits.size(), '0') + digits + ".pfw";
        };

        {
            auto file = store::engine::DataFileWriter::create(
                path, SMALL_EXTENT, "host", "label");
            REQUIRE(file.has_value());
            store::engine::NamespaceItem namespaces;
            namespaces.local_id = 1;
            namespaces.name = dstd::String("dftu.test.put");
            file->add(std::move(namespaces));
            for (std::uint32_t i = 0; i < SCOPES; ++i) {
                store::engine::ScopeItem scope_item;
                scope_item.local_id = i + 1;
                scope_item.kind = store::ScopeKind::File;
                scope_item.paths.push_back(dstd::String(scope_path(i)));
                file->add(std::move(scope_item));
                store::engine::CopyItem copy;
                copy.local_id = i + 1;
                copy.namespace_id = 1;
                copy.scope_id = i + 1;
                copy.generation = i + 1;
                copy.state = store::engine::CopyState::Ready;
                file->add(std::move(copy));
            }
            REQUIRE(file->commit().has_value());
            REQUIRE(file->close().has_value());
        }
        auto light = engine::DataFileReader::open(
            path, store::engine::FooterItems::Directory);
        REQUIRE(light.has_value());
        REQUIRE(light->index().tail.has_value());
        const std::uint32_t levels = light->index().tail->directory_levels;
        REQUIRE(levels >= 2);
        CHECK(light->index().tail->top_table.size() <=
              store::engine::DIRECTORY_TOP_BYTES);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->read_stats().read_calls == 0);
        CHECK(snapshot->state(spec, file_scope(scope_path(SCOPES / 2))) ==
              ScopeState::Ready);
        CHECK(snapshot->read_stats().metadata_blocks_read == levels);
        CHECK(snapshot->read_stats().extents_read == 0);
        for (std::uint32_t i = 0; i < SCOPES; i += 97) {
            CHECK(snapshot->state(spec, file_scope(scope_path(i))) ==
                  ScopeState::Ready);
        }
        CHECK(snapshot->state(spec, file_scope(scope_path(SCOPES - 1))) ==
              ScopeState::Ready);
        CHECK(snapshot->state(spec, file_scope("/a.pfw")) ==
              ScopeState::Absent);
        CHECK(snapshot->state(spec, file_scope(scope_path(0) + "x")) ==
              ScopeState::Absent);
        CHECK(snapshot->state(spec, file_scope("/zzz.pfw")) ==
              ScopeState::Absent);

        auto listed = snapshot->generations();
        REQUIRE(listed.has_value());
        const dstd::Vector<store::engine::ScopeGeneration>& all = *listed;
        REQUIRE(all.size() == SCOPES);
        CHECK(dstd::String(all.front().scope.paths[0]) == scope_path(0));
        CHECK(all.front().generation == 1);
        CHECK(dstd::String(all.back().scope.paths[0]) ==
              scope_path(SCOPES - 1));
        CHECK(all.back().generation == SCOPES);
    }

    TEST_CASE("a point read decodes only the pages of its segment") {
        const ScopeKey scopes[] = {file_scope("/traces/pages-a.pfw"),
                                   file_scope("/traces/pages-b.pfw"),
                                   file_scope("/traces/pages-c.pfw")};
        const NamespaceSpec spec = put_spec("dftu.test.pages");
        constexpr std::size_t KEYS_PER_SCOPE = 300;
        const dstd::String value(512, 'v');
        const auto key_at = [](std::size_t scope, std::size_t index) {
            return "key-" + dstd::to_string(scope) + "-" +
                   dstd::to_string(index);
        };

        dftu_utils_test::ScopedTestDir dir("snapshot_extent_pages");
        engine::WriterOptions options = small_extents();
        options.extent_size = 8U * 1024U * 1024U;
        dstd::String path;
        {
            auto writer = make_writer(dir, options);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            for (std::size_t scope = 0; scope < std::size(scopes); ++scope) {
                for (std::size_t index = 0; index < KEYS_PER_SCOPE; ++index) {
                    Batch batch = writer->batch(*id, scopes[scope], 0);
                    batch.put(view_of(key_at(scope, index)), view_of(value));
                    REQUIRE(writer->commit(std::move(batch)).has_value());
                }
            }
            REQUIRE(flush_writer(*writer).has_value());
            path = dstd::String(writer->path());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        const auto refs = extent_refs_of(*reader);
        REQUIRE(refs.size() == 1);
        CHECK(refs[0].ref.segments.size() == 3 * KEYS_PER_SCOPE);
        auto stored = reader->read_extent(
            {refs[0].ref.offset, refs[0].ref.length, refs[0].ref.crc});
        REQUIRE(stored.has_value());
        auto pages = store::engine::ExtentPages::open(view_of(*stored));
        REQUIRE(pages.has_value());
        REQUIRE(pages->size() >= 8);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto found = snapshot->get(spec, scopes[1], view_of(key_at(1, 7)));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == value);
        CHECK(snapshot->read_stats().extents_read == 1);
        CHECK(snapshot->read_stats().pages_decoded == 1);

        for (const ScopeKey& scope : scopes) {
            auto cursor =
                snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
            CHECK(collect(cursor).size() == KEYS_PER_SCOPE);
        }
        CHECK(snapshot->read_stats().extents_read == 1);
        CHECK(snapshot->read_stats().pages_decoded == pages->size());
    }

    TEST_CASE("a namespace codec decides how its pages are stored") {
        const ScopeKey scope = file_scope("/traces/codec.pfw");
        constexpr std::size_t KEY_COUNT = 200;
        const dstd::String value(512, 'v');
        const auto key_at = [](std::size_t index) {
            return "key-" + dstd::to_string(1000 + index);
        };
        const auto codecs_written = [&](store::PageCodec codec) {
            NamespaceSpec spec = put_spec("dftu.test.codec");
            spec.codec = codec;
            dftu_utils_test::ScopedTestDir dir("snapshot_namespace_codec");
            engine::WriterOptions options = small_extents();
            options.extent_size = 8U * 1024U * 1024U;
            dstd::String path;
            {
                auto writer = make_writer(dir, options);
                auto id = writer->add_namespace(spec);
                REQUIRE(id.has_value());
                for (std::size_t index = 0; index < KEY_COUNT; ++index) {
                    Batch batch = writer->batch(*id, scope, 0);
                    batch.put(view_of(key_at(index)), view_of(value));
                    REQUIRE(writer->commit(std::move(batch)).has_value());
                }
                REQUIRE(flush_writer(*writer).has_value());
                path = dstd::String(writer->path());
                REQUIRE(close_writer(*writer).has_value());
            }

            dstd::Vector<store::PageCodec> seen;
            auto reader = engine::DataFileReader::open(path);
            REQUIRE(reader.has_value());
            for (const auto& indexed : extent_refs_of(*reader)) {
                auto stored = reader->read_extent(
                    {indexed.ref.offset, indexed.ref.length, indexed.ref.crc});
                REQUIRE(stored.has_value());
                auto pages = store::engine::ExtentPages::open(view_of(*stored));
                REQUIRE(pages.has_value());
                for (std::size_t i = 0; i < pages->size(); ++i) {
                    seen.push_back(pages->entry(i).codec);
                }
            }

            auto snapshot = engine::Snapshot::open(dir.str());
            REQUIRE(snapshot.has_value());
            auto found = snapshot->get(put_spec("dftu.test.codec"), scope,
                                       view_of(key_at(KEY_COUNT / 2)));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == value);
            return seen;
        };

        const auto raw = codecs_written(store::PageCodec::None);
        REQUIRE_FALSE(raw.empty());
        for (const store::PageCodec codec : raw) {
            CHECK(codec == store::PageCodec::None);
        }
        const auto packed = codecs_written(store::PageCodec::Zstd);
        REQUIRE_FALSE(packed.empty());
        CHECK(std::count(packed.begin(), packed.end(), store::PageCodec::Zstd) >
              0);
    }

    TEST_CASE("a point read of a segment over pages decodes two of them") {
        const ScopeKey scope = file_scope("/traces/granules.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.granules");
        constexpr std::size_t KEY_COUNT = 700;
        const dstd::String value(400, 'v');
        const auto key_at = [](std::size_t index) {
            const dstd::String digits = dstd::to_string(index);
            return "key-" + dstd::String(6 - digits.size(), '0') + digits;
        };

        dftu_utils_test::ScopedTestDir dir("snapshot_segment_granules");
        engine::WriterOptions options = small_extents();
        options.extent_size = 8U * 1024U * 1024U;
        dstd::String path;
        {
            auto writer = make_writer(dir, options);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            for (std::size_t index = 0; index < KEY_COUNT; ++index) {
                batch.put(view_of(key_at(index)), view_of(value));
            }
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(flush_writer(*writer).has_value());
            path = dstd::String(writer->path());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        const auto refs = extent_refs_of(*reader);
        REQUIRE(refs.size() == 1);
        REQUIRE(refs[0].ref.segments.size() == 1);
        auto stored = reader->read_extent(
            {refs[0].ref.offset, refs[0].ref.length, refs[0].ref.crc});
        REQUIRE(stored.has_value());
        auto pages = store::engine::ExtentPages::open(view_of(*stored));
        REQUIRE(pages.has_value());
        REQUIRE(pages->size() > 4);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto found = snapshot->get(spec, scope, view_of(key_at(KEY_COUNT / 2)));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == value);
        CHECK(snapshot->read_stats().extents_read == 1);
        CHECK(snapshot->read_stats().pages_decoded == 2);

        const dstd::String absent = key_at(KEY_COUNT / 2) + "x";
        auto missing = snapshot->get(spec, scope, view_of(absent));
        REQUIRE(missing.has_value());
        CHECK_FALSE(missing->has_value());
        CHECK(snapshot->read_stats().pages_decoded <= 2);

        auto cursor =
            snapshot->scan(spec, scope, dstd::ByteView{}, dstd::ByteView{});
        const dstd::Vector<dstd::Pair<dstd::String, dstd::String>> rows =
            collect(cursor);
        REQUIRE(rows.size() == KEY_COUNT);
        for (std::size_t index = 0; index < rows.size(); ++index) {
            CHECK(rows[index].first == key_at(index));
            CHECK(rows[index].second == value);
        }
        CHECK(snapshot->read_stats().extents_read == 1);
        CHECK(snapshot->read_stats().pages_decoded == pages->size());
    }

    TEST_CASE(
        "a range scan of a segment over pages decodes only its granules") {
        const ScopeKey scope = file_scope("/traces/range_granules.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.range_granules");
        constexpr std::size_t KEY_COUNT = 700;
        const dstd::String value(400, 'v');
        const auto key_at = [](std::size_t index) {
            const dstd::String digits = dstd::to_string(index);
            return "key-" + dstd::String(6 - digits.size(), '0') + digits;
        };

        dftu_utils_test::ScopedTestDir dir("snapshot_range_granules");
        engine::WriterOptions options = small_extents();
        options.extent_size = 8U * 1024U * 1024U;
        dstd::String path;
        {
            auto writer = make_writer(dir, options);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            for (std::size_t index = 0; index < KEY_COUNT; ++index) {
                batch.put(view_of(key_at(index)), view_of(value));
            }
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(flush_writer(*writer).has_value());
            path = dstd::String(writer->path());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        const auto refs = extent_refs_of(*reader);
        REQUIRE(refs.size() == 1);
        REQUIRE(refs[0].ref.segments.size() == 1);
        auto stored = reader->read_extent(
            {refs[0].ref.offset, refs[0].ref.length, refs[0].ref.crc});
        REQUIRE(stored.has_value());
        auto pages = store::engine::ExtentPages::open(view_of(*stored));
        REQUIRE(pages.has_value());
        REQUIRE(pages->size() > 4);

        const auto scan_range = [&](std::size_t from, std::size_t to,
                                    bool bounded) {
            auto snapshot = engine::Snapshot::open(dir.str());
            REQUIRE(snapshot.has_value());
            const dstd::String begin = key_at(from);
            const dstd::String end = bounded ? key_at(to) : dstd::String();
            auto cursor =
                snapshot->scan(spec, scope, view_of(begin),
                               bounded ? view_of(end) : dstd::ByteView{});
            const dstd::Vector<dstd::Pair<dstd::String, dstd::String>> rows =
                collect(cursor);
            REQUIRE(rows.size() == to - from);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                CHECK(rows[i].first == key_at(from + i));
                CHECK(rows[i].second == value);
            }
            return snapshot->read_stats().pages_decoded;
        };

        // The header and index page, and the one or two granule pages.
        CHECK(scan_range(300, 320, true) <= 3);
        CHECK(scan_range(0, 5, true) <= 3);
        CHECK(scan_range(690, KEY_COUNT, false) <= 3);
        CHECK(scan_range(0, KEY_COUNT, false) == pages->size());
    }

    TEST_CASE(
        "a range scan merges later puts and an erase over a wide segment") {
        const ScopeKey scope = file_scope("/traces/range_merge.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.range_merge");
        constexpr std::size_t KEY_COUNT = 700;
        const auto key_at = [](std::size_t index) {
            const dstd::String digits = dstd::to_string(index);
            return "key-" + dstd::String(6 - digits.size(), '0') + digits;
        };
        const dstd::String old_value(400, 'a');
        const dstd::String new_value(400, 'b');

        dftu_utils_test::ScopedTestDir dir("snapshot_range_merge");
        engine::WriterOptions options = small_extents();
        options.extent_size = 8U * 1024U * 1024U;
        {
            auto writer = make_writer(dir, options);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch first = writer->batch(*id, scope, 0);
            for (std::size_t index = 0; index < KEY_COUNT; ++index) {
                first.put(view_of(key_at(index)), view_of(old_value));
            }
            REQUIRE(writer->commit(std::move(first)).has_value());
            const dstd::String erase_begin = key_at(310);
            const dstd::String erase_end = key_at(315);
            REQUIRE(writer
                        ->erase(*id, scope, view_of(erase_begin),
                                view_of(erase_end))
                        .has_value());
            Batch second = writer->batch(*id, scope, 0);
            for (std::size_t index = 300; index <= 340; index += 2) {
                second.put(view_of(key_at(index)), view_of(new_value));
            }
            REQUIRE(writer->commit(std::move(second)).has_value());
            REQUIRE(flush_writer(*writer).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        const dstd::String begin = key_at(300);
        const dstd::String end = key_at(330);
        auto cursor = snapshot->scan(spec, scope, view_of(begin), view_of(end));
        const dstd::Vector<dstd::Pair<dstd::String, dstd::String>> rows =
            collect(cursor);
        dstd::Vector<dstd::Pair<dstd::String, dstd::String>> expected;
        for (std::size_t index = 300; index < 330; ++index) {
            const bool put_again = index % 2 == 0;
            if (index >= 310 && index < 315 && !put_again) continue;
            expected.emplace_back(key_at(index),
                                  put_again ? new_value : old_value);
        }
        CHECK(rows == expected);
    }

    TEST_CASE(
        "a point read of a segment whose index spans pages skips its body") {
        const ScopeKey scope = file_scope("/traces/wide_head.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.wide_head");
        constexpr std::size_t KEY_COUNT = 400;
        // Long keys and two entries per granule make an index over one page.
        const dstd::String value(16 * 1024, 'v');
        const auto key_at = [](std::size_t index) {
            const dstd::String digits = dstd::to_string(index);
            return dstd::String(240, 'k') +
                   dstd::String(6 - digits.size(), '0') + digits;
        };

        dftu_utils_test::ScopedTestDir dir("snapshot_wide_head");
        engine::WriterOptions options = small_extents();
        options.extent_size = 16U * 1024U * 1024U;
        dstd::String path;
        {
            auto writer = make_writer(dir, options);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            for (std::size_t index = 0; index < KEY_COUNT; ++index) {
                batch.put(view_of(key_at(index)), view_of(value));
            }
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(flush_writer(*writer).has_value());
            path = dstd::String(writer->path());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        const auto refs = extent_refs_of(*reader);
        REQUIRE(refs.size() == 1);
        REQUIRE(refs[0].ref.segments.size() == 1);
        auto stored = reader->read_extent(
            {refs[0].ref.offset, refs[0].ref.length, refs[0].ref.crc});
        REQUIRE(stored.has_value());
        auto pages = store::engine::ExtentPages::open(view_of(*stored));
        REQUIRE(pages.has_value());
        REQUIRE(pages->size() > 100);
        REQUIRE(pages->entry(0).raw_size == store::engine::PAGE_BYTES);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto found = snapshot->get(spec, scope, view_of(key_at(KEY_COUNT / 2)));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == value);
        CHECK(snapshot->read_stats().pages_decoded <= 6);

        const dstd::String absent = key_at(KEY_COUNT / 2) + "x";
        auto missing = snapshot->get(spec, scope, view_of(absent));
        REQUIRE(missing.has_value());
        CHECK_FALSE(missing->has_value());
        CHECK(snapshot->read_stats().pages_decoded <= 8);
    }

    TEST_CASE("a point read of a granule over a page reads the whole segment") {
        const ScopeKey scope = file_scope("/traces/wide_granule.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.wide_granule");
        // Two pages of value, below BIG_VALUE_BYTES, so the entry stays in a
        // granule that no page holds alone.
        const dstd::String value(2 * store::engine::PAGE_BYTES, 'w');

        dftu_utils_test::ScopedTestDir dir("snapshot_wide_granule");
        engine::WriterOptions options = small_extents();
        options.extent_size = 8U * 1024U * 1024U;
        dstd::String path;
        {
            auto writer = make_writer(dir, options);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(dstd::String("a")),
                      view_of(dstd::String("small")));
            batch.put(view_of(dstd::String("m")), view_of(value));
            batch.put(view_of(dstd::String("z")),
                      view_of(dstd::String("small")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(flush_writer(*writer).has_value());
            path = dstd::String(writer->path());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        const auto refs = extent_refs_of(*reader);
        REQUIRE(refs.size() == 1);
        REQUIRE(refs[0].ref.segments.size() == 1);
        auto stored = reader->read_extent(
            {refs[0].ref.offset, refs[0].ref.length, refs[0].ref.crc});
        REQUIRE(stored.has_value());
        auto pages = store::engine::ExtentPages::open(view_of(*stored));
        REQUIRE(pages.has_value());
        REQUIRE(pages->size() > 2);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto head = snapshot->get(spec, scope, view_of(dstd::String("a")));
        REQUIRE(head.has_value());
        REQUIRE(head->has_value());
        CHECK(to_std((*head)->bytes()) == "small");
        CHECK(snapshot->read_stats().pages_decoded == 2);

        auto wide = snapshot->get(spec, scope, view_of(dstd::String("m")));
        REQUIRE(wide.has_value());
        REQUIRE(wide->has_value());
        CHECK(to_std((*wide)->bytes()) == value);
        CHECK(snapshot->read_stats().pages_decoded == pages->size());
    }

    TEST_CASE("a closed file of many commits reads few segment blocks") {
        dftu_utils_test::ScopedTestDir dir("snapshot_many_commits");
        const NamespaceSpec spec = put_spec("dftu.test.many-commits");
        constexpr int COMMITS = 300;
        constexpr std::size_t SCOPES = 3;
        dstd::Vector<ScopeKey> scopes;
        for (std::size_t s = 0; s < SCOPES; ++s) {
            scopes.push_back(
                file_scope("/traces/m" + dstd::to_string(s) + ".pfw"));
        }
        const dstd::Vector<dstd::Map<dstd::String, dstd::String>> model =
            write_many_commits(dir, spec, scopes, COMMITS);
        REQUIRE(model.size() == SCOPES);
        for (const dstd::Map<dstd::String, dstd::String>& scope_model : model) {
            REQUIRE_FALSE(scope_model.empty());
        }

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->skipped().empty());
        CHECK(snapshot->read_stats().metadata_blocks_read == 0);

        auto one = snapshot->get(spec, scopes[0], view_of(key_of(0)));
        REQUIRE(one.has_value());
        REQUIRE(one->has_value());
        CHECK(snapshot->read_stats().metadata_blocks_read == 2);

        for (std::size_t s = 0; s < SCOPES; ++s) {
            for (const auto& row : model[s]) {
                auto found = snapshot->get(spec, scopes[s], view_of(row.first));
                REQUIRE(found.has_value());
                REQUIRE(found->has_value());
                CHECK(to_std((*found)->bytes()) == row.second);
            }
            const dstd::Vector<dstd::Pair<dstd::String, dstd::String>> expected(
                model[s].begin(), model[s].end());
            auto cursor = snapshot->scan(spec, scopes[s], dstd::ByteView{},
                                         dstd::ByteView{});
            CHECK(collect(cursor) == expected);
        }
        CHECK(snapshot->read_stats().metadata_blocks_read == 1 + SCOPES);
        auto absent = snapshot->get(spec, scopes[0], view_of(key_of(1000)));
        REQUIRE(absent.has_value());
        CHECK_FALSE(absent->has_value());
    }

    TEST_CASE("a shared segment block is checked once and fails all at once") {
        dftu_utils_test::ScopedTestDir dir("snapshot_shared_block");
        dftu_utils_test::ScopedTestDir damaged_dir("snapshot_shared_block_bad");
        const NamespaceSpec spec = put_spec("dftu.test.many-commits");
        constexpr int COMMITS = 300;
        const dstd::Vector<ScopeKey> scopes = {file_scope("/traces/one.pfw")};
        write_many_commits(dir, spec, scopes, COMMITS);

        const dstd::String path = only_data_file(dir.str());
        auto reader = engine::DataFileReader::open(path);
        REQUIRE(reader.has_value());
        const ClosedSegments closed = closed_segments(*reader);
        REQUIRE(closed.blocks.size() == 1);
        CHECK(closed.blocks[0].length < store::engine::SEGMENT_BLOCK_BYTES);

        auto snapshot = engine::Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto report = snapshot->check();
        REQUIRE(report.has_value());
        CHECK(report->bad_commits.empty());
        CHECK(report->bad_segments.empty());
        CHECK(report->metadata_blocks == 2);
        CHECK(report->segments == static_cast<std::size_t>(COMMITS));

        // The last byte of the shared block breaks its crc: every commit that
        // points at it is bad, and no segment is walked.
        dstd::Vector<std::uint8_t> bytes = read_file(path);
        const std::size_t last =
            static_cast<std::size_t>(closed.blocks[0].offset) +
            closed.blocks[0].length - 1U;
        REQUIRE(last < bytes.size());
        bytes[last] ^= 0xFFU;
        write_file(damaged_dir.file("shared.dfs"), bytes);

        auto damaged = engine::Snapshot::open(damaged_dir.str());
        REQUIRE(damaged.has_value());
        auto broken = damaged->check();
        REQUIRE(broken.has_value());
        CHECK(broken->bad_segments.empty());
        CHECK(broken->segments == 0);
        CHECK(broken->bad_commits.size() == closed.runs.size());
        for (const store::engine::BadCommit& bad : broken->bad_commits) {
            CHECK(dstd::String(bad.file) == "shared.dfs");
        }
    }
}
