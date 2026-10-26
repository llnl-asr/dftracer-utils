#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/index/store/engine/data_file.h>
#include <dftracer/utils/index/store/engine/fold.h>
#include <dftracer/utils/index/store/engine/section.h>
#include <dftracer/utils/index/store/engine/snapshot.h>
#include <dftracer/utils/index/store/engine/writer.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace dstd = dftracer::utils::dstd;
namespace engine = dftracer::utils::index::store::engine;

using dftracer::utils::ByteView;
using dftracer::utils::Runtime;
using dftracer::utils::dstd::Condition;
using engine::Batch;
using engine::DataFileReader;
using engine::FoldKind;
using engine::NamespaceSpec;
using engine::ScopeKey;
using engine::ScopeState;
using engine::SectionView;
using engine::Snapshot;
using engine::Writer;
using engine::WriterOptions;

constexpr std::uint32_t SMALL_PAGE = 4096;
constexpr std::size_t HEADER_WRITER_RANDOM_AT = 32;
constexpr std::size_t HEADER_CRC_AT = 12;
constexpr std::size_t KEY_COUNT = 1000;
constexpr int RUNTIME_THREADS = 8;

ByteView view_of(const std::string& bytes) {
    return {bytes.data(), bytes.size()};
}

ByteView view_of(const dstd::Vector<std::uint8_t>& bytes) {
    return {bytes.data(), bytes.size()};
}

dstd::String dstring(const std::string& value) {
    return dstd::String(value.data(), value.size());
}

std::string to_std(ByteView bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()),
                       bytes.size());
}

std::string to_std(const dstd::String& value) {
    return std::string(value.data(), value.size());
}

std::string key_of(int index) {
    const std::string digits = std::to_string(index);
    return "key-" + std::string(4 - digits.size(), '0') + digits;
}

std::string value_of(int index) { return "value-" + std::to_string(index); }

std::string i64_bytes(std::int64_t value) {
    std::string bytes(8, '\0');
    dftracer::utils::write_u64_le(reinterpret_cast<std::uint8_t*>(bytes.data()),
                                  static_cast<std::uint64_t>(value));
    return bytes;
}

std::string f64_bytes(double value) {
    std::string bytes(8, '\0');
    dftracer::utils::write_f64_le(reinterpret_cast<std::uint8_t*>(bytes.data()),
                                  value);
    return bytes;
}

std::int64_t read_i64(ByteView bytes) {
    return std::bit_cast<std::int64_t>(dftracer::utils::read_u64_le(
        reinterpret_cast<const std::uint8_t*>(bytes.data())));
}

double read_f64(ByteView bytes) {
    return dftracer::utils::read_f64_le(
        reinterpret_cast<const std::uint8_t*>(bytes.data()));
}

NamespaceSpec put_spec(const std::string& name, std::uint32_t version = 0) {
    NamespaceSpec spec;
    spec.name = dstring(name);
    spec.format_version = version;
    spec.fold = FoldKind::None;
    return spec;
}

NamespaceSpec fold_spec(const std::string& name, FoldKind kind) {
    NamespaceSpec spec;
    spec.name = dstring(name);
    spec.format_version = 0;
    spec.fold = kind;
    return spec;
}

ScopeKey file_scope(const std::string& path) {
    return ScopeKey::file(dstring(path));
}

WriterOptions small_pages() {
    WriterOptions options;
    options.page_size = SMALL_PAGE;
    options.host = dstring("host");
    options.label = dstring("label");
    return options;
}

/// The one executor of this suite: every writer coroutine runs on it, so a
/// test submits flush and close there and waits for the result.
Runtime& store_runtime() {
    static Runtime runtime(RUNTIME_THREADS);
    return runtime;
}

std::unique_ptr<Writer> make_writer(
    const dftu_utils_test::ScopedTestDir& dir,
    const WriterOptions& options = small_pages()) {
    auto created =
        Writer::create(dir.str(), *store_runtime().executor(), options);
    REQUIRE(created.has_value());
    return std::move(*created);
}

dstd::Expected<void, dstd::Error> flush_writer(Writer& writer) {
    return store_runtime().submit(writer.flush()).get();
}

dstd::Expected<void, dstd::Error> close_writer(Writer& writer) {
    return store_runtime().submit(writer.close()).get();
}

std::vector<std::pair<std::string, std::string>> collect(
    engine::ScanCursor& cursor) {
    std::vector<std::pair<std::string, std::string>> rows;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        rows.emplace_back(to_std(cursor.key()), to_std(*value));
        cursor.next();
    }
    return rows;
}

struct ScopedRow {
    std::string scope;
    std::string key;
    std::string value;
};

std::vector<ScopedRow> collect_scoped(engine::ScanCursor& cursor) {
    std::vector<ScopedRow> rows;
    while (!cursor.at_end()) {
        auto value = cursor.value();
        REQUIRE(value.has_value());
        REQUIRE_FALSE(cursor.scope().paths.empty());
        rows.push_back({to_std(cursor.scope().paths[0]), to_std(cursor.key()),
                        to_std(*value)});
        cursor.next();
    }
    return rows;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

void write_file(const std::string& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

/// Every page ref of a data file, with those a summary lists read from their
/// commit records.
std::vector<engine::IndexedPageRef> page_refs_of(const DataFileReader& reader) {
    auto refs = reader.all_page_refs();
    REQUIRE(refs.has_value());
    return std::vector<engine::IndexedPageRef>(refs->begin(), refs->end());
}

/// The bytes of a data file without its footer and trailer, as a crash before
/// the close leaves them: the reader recovers the commit chain by a scan, and a
/// commit that was not durable counts as not synced.
std::vector<std::uint8_t> without_footer(std::vector<std::uint8_t> bytes) {
    REQUIRE(bytes.size() >= engine::FILE_TRAILER_BYTES);
    const std::uint64_t footer = dftracer::utils::read_u64_le(
        bytes.data() + bytes.size() - engine::FILE_TRAILER_BYTES + 8U);
    REQUIRE(footer >= engine::FILE_HEADER_BYTES);
    REQUIRE(footer < bytes.size());
    bytes.resize(static_cast<std::size_t>(footer));
    return bytes;
}

/// The bytes of a closed data file with the format version of its footer
/// tail set to `version`, the footer record encoded again in place.
std::vector<std::uint8_t> with_tail_version(std::vector<std::uint8_t> bytes,
                                            std::uint32_t version) {
    REQUIRE(bytes.size() >= engine::FILE_TRAILER_BYTES);
    const std::uint8_t* trailer =
        bytes.data() + bytes.size() - engine::FILE_TRAILER_BYTES;
    const std::uint64_t at = dftracer::utils::read_u64_le(trailer + 8U);
    const std::uint32_t length = dftracer::utils::read_u32_le(trailer + 16U);
    REQUIRE(at + length <= bytes.size());
    auto footer = engine::decode_record(ByteView{
        reinterpret_cast<const std::byte*>(bytes.data() + at), length});
    REQUIRE(footer.has_value());
    bool found = false;
    for (engine::RecordItem& item : footer->items) {
        if (auto* tail = std::get_if<engine::FileTail>(&item)) {
            tail->format_version = version;
            found = true;
        }
    }
    REQUIRE(found);
    auto encoded = engine::encode_record(footer->kind, footer->sequence,
                                         footer->prev_offset, footer->offset,
                                         footer->items);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->size() <= length);
    std::copy(encoded->begin(), encoded->end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(at));
    return bytes;
}

void rewrite_header_crc(std::vector<std::uint8_t>& bytes) {
    REQUIRE(bytes.size() >= engine::FILE_HEADER_BYTES);
    dftracer::utils::write_u32_le(bytes.data() + HEADER_CRC_AT, 0);
    const std::uint32_t crc =
        dftracer::utils::utilities::fileio::compress::gzip_crc32(
            0, bytes.data(), engine::FILE_HEADER_BYTES);
    dftracer::utils::write_u32_le(bytes.data() + HEADER_CRC_AT, crc);
}

/// Copies `source` into `target` under `name`, with the header writer id and
/// the header crc rewritten so the copy verifies, and without its footer, so
/// the reader takes the writer id from the header.
std::string forge_copy(const std::string& source,
                       const dftu_utils_test::ScopedTestDir& target,
                       const std::string& name, std::uint64_t writer_random) {
    std::vector<std::uint8_t> bytes = read_file(source);
    REQUIRE(bytes.size() > engine::FILE_HEADER_BYTES);
    dftracer::utils::write_u64_le(bytes.data() + HEADER_WRITER_RANDOM_AT,
                                  writer_random);
    rewrite_header_crc(bytes);
    const std::string path = target.file(name);
    write_file(path, without_footer(bytes));
    return path;
}

std::uint64_t header_writer_random(const std::string& path) {
    auto reader = DataFileReader::open(path);
    REQUIRE(reader.has_value());
    return reader->index().writer_random;
}

std::vector<std::uint64_t> copy_generations(const std::string& path) {
    auto reader = DataFileReader::open(path);
    REQUIRE(reader.has_value());
    std::vector<std::uint64_t> out;
    for (const engine::CopyItem& copy : reader->index().copies) {
        out.push_back(copy.generation);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::uint64_t> drop_generations(const std::string& path) {
    auto reader = DataFileReader::open(path);
    REQUIRE(reader.has_value());
    std::vector<std::uint64_t> out;
    for (const engine::DropItem& drop : reader->index().drops) {
        out.push_back(drop.generation);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uint64_t only_copy_generation(const std::string& path) {
    auto reader = DataFileReader::open(path);
    REQUIRE(reader.has_value());
    REQUIRE(reader->index().copies.size() == 1);
    return reader->index().copies[0].generation;
}

void check_generation(const Snapshot& snapshot,
                      const std::string& namespace_name, const ScopeKey& scope,
                      std::uint64_t expected) {
    auto listed = snapshot.generations();
    REQUIRE(listed.has_value());
    const dstd::Vector<engine::ScopeGeneration>& all = *listed;
    REQUIRE(all.size() == 1);
    CHECK(to_std(all[0].namespace_name) == namespace_name);
    CHECK(all[0].format_version == 0);
    CHECK(all[0].scope.kind == scope.kind);
    REQUIRE(all[0].scope.paths.size() == 1);
    CHECK(to_std(all[0].scope.paths[0]) == to_std(scope.paths[0]));
    CHECK(all[0].generation == expected);
}

dstd::Vector<std::uint8_t> bytes_vector(const std::string& bytes) {
    return dstd::Vector<std::uint8_t>(bytes.begin(), bytes.end());
}

/// A 17-byte value whose bytes show its place in the fold order.
std::string concat_value(int index) {
    const std::string digits = std::to_string(index);
    return "concat-" + std::string(10 - digits.size(), '0') + digits;
}

/// Installs a runtime with `threads` threads as the process-wide default, so an
/// open under it reads its files in parallel, and puts the runtime it found
/// back at the end of the scope.
class ScopedDefaultRuntime {
   public:
    explicit ScopedDefaultRuntime(std::size_t threads)
        : previous_(dftracer::utils::peek_default_runtime()) {
        dftracer::utils::set_default_runtime(
            std::make_shared<Runtime>(threads));
    }

    ~ScopedDefaultRuntime() {
        dftracer::utils::set_default_runtime(std::move(previous_));
    }

    ScopedDefaultRuntime(const ScopedDefaultRuntime&) = delete;
    ScopedDefaultRuntime& operator=(const ScopedDefaultRuntime&) = delete;

   private:
    std::shared_ptr<Runtime> previous_;
};

/// What one open of a root read, and a text of every scope state and every row,
/// so two opens under different runtimes compare with one string. The read
/// counters are the ones the dump left behind: an open reads no block, and the
/// queries the dump makes read the commits they need.
struct OpenDump {
    std::string text;
    std::size_t files = 0;
    std::size_t pages = 0;
    std::uint64_t read_calls = 0;
    std::uint64_t commits_verified = 0;
    std::uint64_t blocks_read = 0;
    std::size_t commits = 0;
    std::uint64_t indexes_read = 0;
    std::uint64_t metadata_blocks_read = 0;
};

OpenDump dump_open(const dftu_utils_test::ScopedTestDir& root,
                   const NamespaceSpec& spec,
                   const std::vector<ScopeKey>& scopes) {
    auto snapshot = Snapshot::open(root.str());
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->skipped().empty());
    OpenDump dump;
    dump.files = snapshot->open_stats().files;
    // Open itself reads no block at all.
    CHECK(snapshot->read_stats().read_calls == 0);
    for (const ScopeKey& scope : scopes) {
        dump.text +=
            "state " + to_std(scope.paths[0]) + " " +
            std::to_string(static_cast<int>(snapshot->state(spec, scope))) +
            "\n";
        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        for (const ScopedRow& row : collect_scoped(cursor)) {
            dump.text += row.key + "=" + row.value + "\n";
        }
    }
    dump.read_calls = snapshot->read_stats().read_calls;
    dump.commits_verified = snapshot->read_stats().commits_verified;
    dump.blocks_read = snapshot->read_stats().blocks_read;
    dump.indexes_read = snapshot->read_stats().indexes_read;
    dump.metadata_blocks_read = snapshot->read_stats().metadata_blocks_read;
    auto report = snapshot->check();
    REQUIRE(report.has_value());
    dump.commits = report->commits;
    dump.pages = report->blocks;
    return dump;
}

/// The descriptor this process holds open on `path`, found by its inode, or -1.
int descriptor_of(const std::string& path) {
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

/// One data file of one writer of the open tests: one commit of `keys` entries
/// per scope, with a group commit after each one when `flushed`.
void write_scopes_file(const dftu_utils_test::ScopedTestDir& dir,
                       const NamespaceSpec& spec,
                       const std::vector<ScopeKey>& scopes, int keys,
                       bool flushed) {
    auto writer = make_writer(dir);
    auto id = writer->add_namespace(spec);
    REQUIRE(id.has_value());
    for (const ScopeKey& scope : scopes) {
        Batch entries = writer->batch(*id, scope, 0);
        for (int k = 0; k < keys; ++k) {
            const std::string key = "b0-" + key_of(k);
            const std::string value = value_of(k);
            entries.put(view_of(key), view_of(value));
        }
        REQUIRE(writer->commit(std::move(entries)).has_value());
        if (flushed) REQUIRE(flush_writer(*writer).has_value());
    }
    REQUIRE(close_writer(*writer).has_value());
}

/// One data file of one writer of the open tests: `batches` commits of `keys`
/// entries each, in one scope, with a group commit after each one when
/// `flushed`.
void write_scoped_file(const dftu_utils_test::ScopedTestDir& dir,
                       const NamespaceSpec& spec, const ScopeKey& scope,
                       int batches, int keys, bool flushed) {
    auto writer = make_writer(dir);
    auto id = writer->add_namespace(spec);
    REQUIRE(id.has_value());
    const std::string prefix = to_std(scope.paths[0]);
    for (int batch = 0; batch < batches; ++batch) {
        Batch entries = writer->batch(*id, scope, 0);
        for (int k = 0; k < keys; ++k) {
            const std::string key =
                "b" + std::to_string(batch) + "-" + key_of(k);
            const std::string value = value_of(k);
            entries.put(view_of(key), view_of(value));
        }
        REQUIRE(writer->commit(std::move(entries)).has_value());
        if (flushed) REQUIRE(flush_writer(*writer).has_value());
    }
    REQUIRE(close_writer(*writer).has_value());
}

}  // namespace

TEST_SUITE("index store snapshot") {
    TEST_CASE("scope keys sort by kind and then by paths") {
        const std::string a = "/traces/a.pfw";
        const std::string b = "/traces/b.pfw";
        const std::vector<dstd::StringView> given = {
            dstd::StringView(b), dstd::StringView(a), dstd::StringView(b)};
        const ScopeKey files = ScopeKey::files(given);
        CHECK(files.kind == engine::ScopeKind::Files);
        REQUIRE(files.paths.size() == 2);
        CHECK(to_std(files.paths[0]) == a);
        CHECK(to_std(files.paths[1]) == b);

        const ScopeKey root = ScopeKey::root();
        const ScopeKey file = file_scope(a);
        CHECK(root.kind == engine::ScopeKind::Root);
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
                const std::string key = key_of(i);
                const std::string value = value_of(i);
                batch.put(view_of(key), view_of(value));
            }
            REQUIRE(writer->commit(std::move(batch)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->skipped().empty());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);

        for (int i = 0; i < static_cast<int>(KEY_COUNT); ++i) {
            const std::string key = key_of(i);
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == value_of(i));
        }

        const std::string missing = "key-9999";
        auto absent = snapshot->get(spec, scope, view_of(missing));
        REQUIRE(absent.has_value());
        CHECK_FALSE(absent->has_value());

        const std::string prefix = "key-01";
        auto prefix_cursor = snapshot->scan(spec, scope, view_of(prefix));
        const auto prefix_rows = collect(prefix_cursor);
        REQUIRE(prefix_rows.size() == 100);
        for (int i = 0; i < 100; ++i) {
            CHECK(prefix_rows[static_cast<std::size_t>(i)].first ==
                  key_of(100 + i));
            CHECK(prefix_rows[static_cast<std::size_t>(i)].second ==
                  value_of(100 + i));
        }

        const std::string begin = key_of(100);
        const std::string end = key_of(200);
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

        auto all_cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        const auto all_rows = collect(all_cursor);
        REQUIRE(all_rows.size() == KEY_COUNT);
        for (std::size_t i = 0; i < all_rows.size(); ++i) {
            CHECK(all_rows[i].first == key_of(static_cast<int>(i)));
            CHECK(all_rows[i].second == value_of(static_cast<int>(i)));
        }
    }

    TEST_CASE("the last put wins inside a batch and a later batch wins") {
        dftu_utils_test::ScopedTestDir dir("snapshot_last_wins");
        const ScopeKey scope = file_scope("/traces/b.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());

        Batch first = writer->batch(*id, scope, 0);
        first.put(view_of(std::string("key-b")), view_of(std::string("b1")));
        first.put(view_of(std::string("key-a")), view_of(std::string("a1")));
        first.put(view_of(std::string("key-b")), view_of(std::string("b2")));
        REQUIRE(writer->commit(std::move(first)).has_value());

        Batch second = writer->batch(*id, scope, 0);
        second.put(view_of(std::string("key-b")), view_of(std::string("b3")));
        second.put(view_of(std::string("key-c")), view_of(std::string("c1")));
        REQUIRE(writer->commit(std::move(second)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());

        const std::vector<std::pair<std::string, std::string>> expected = {
            {"key-a", "a1"}, {"key-b", "b3"}, {"key-c", "c1"}};
        for (const auto& row : expected) {
            auto found = snapshot->get(spec, scope, view_of(row.first));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == row.second);
        }

        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(rows[i].first == expected[i].first);
            CHECK(rows[i].second == expected[i].second);
        }
    }

    TEST_CASE("every fold kind folds across a batch and across copies") {
        dftu_utils_test::ScopedTestDir dir("snapshot_folds");
        const ScopeKey scope = file_scope("/traces/f.pfw");

        struct FoldCase {
            FoldKind kind;
            std::string name;
            std::vector<std::string> first;
            std::vector<std::string> second;
            std::string expected;
        };
        const std::vector<FoldCase> cases = {
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
            const std::string key = "folded";

            Batch first = writer->batch(*id, scope, 0);
            for (const std::string& value : fold_case.first) {
                first.fold(view_of(key), view_of(value));
            }
            REQUIRE(writer->commit(std::move(first)).has_value());

            Batch second = writer->batch(*id, scope, 0);
            for (const std::string& value : fold_case.second) {
                second.fold(view_of(key), view_of(value));
            }
            REQUIRE(writer->commit(std::move(second)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        for (const FoldCase& fold_case : cases) {
            const NamespaceSpec spec =
                fold_spec("dftu.test." + fold_case.name, fold_case.kind);
            auto found =
                snapshot->get(spec, scope, view_of(std::string("folded")));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == fold_case.expected);
        }

        auto sum =
            snapshot->get(fold_spec("dftu.test.sum-i64", FoldKind::SumI64),
                          scope, view_of(std::string("folded")));
        REQUIRE(sum.has_value());
        REQUIRE(sum->has_value());
        CHECK(read_i64((*sum)->bytes()) == 22);
        auto mean =
            snapshot->get(fold_spec("dftu.test.sum-f64", FoldKind::SumF64),
                          scope, view_of(std::string("folded")));
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
        batch.fold(view_of(std::string("good")), view_of(i64_bytes(1)));
        batch.fold(view_of(std::string("short")), view_of(std::string("abcd")));
        auto rejected = writer->commit(std::move(batch));
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().is(Condition::InvalidArgument));

        auto put_id = writer->add_namespace(put_spec("dftu.test.put"));
        REQUIRE(put_id.has_value());
        Batch wrong_fold = writer->batch(*put_id, scope, 0);
        wrong_fold.fold(view_of(std::string("key")), view_of(i64_bytes(1)));
        auto folded = writer->commit(std::move(wrong_fold));
        REQUIRE_FALSE(folded.has_value());
        CHECK(folded.error().is(Condition::InvalidArgument));
        Batch wrong_put = writer->batch(*id, scope, 0);
        wrong_put.put(view_of(std::string("key")), view_of(i64_bytes(1)));
        auto put = writer->commit(std::move(wrong_put));
        REQUIRE_FALSE(put.has_value());
        CHECK(put.error().is(Condition::InvalidArgument));
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Absent);
        auto good = snapshot->get(spec, scope, view_of(std::string("good")));
        REQUIRE(good.has_value());
        CHECK_FALSE(good->has_value());
        auto short_value =
            snapshot->get(spec, scope, view_of(std::string("short")));
        REQUIRE(short_value.has_value());
        CHECK_FALSE(short_value->has_value());
    }

    TEST_CASE("a big value round trips and its crc is checked on read") {
        dftu_utils_test::ScopedTestDir dir("snapshot_big");
        dftu_utils_test::ScopedTestDir corrupt_dir("snapshot_big_corrupt");
        const ScopeKey scope = file_scope("/traces/big.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        std::string big(3 * 1024 * 1024, 'x');
        for (std::size_t i = 0; i < big.size(); i += 4096) {
            big[i] = static_cast<char>('a' + static_cast<int>((i / 4096) % 26));
        }

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        batch.put(view_of(std::string("big")), view_of(big));
        batch.put(view_of(std::string("small")), view_of(std::string("small")));
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto found = snapshot->get(spec, scope, view_of(std::string("big")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == big);

        auto reader = DataFileReader::open(to_std(writer->path()));
        REQUIRE(reader.has_value());
        engine::BigRef ref{};
        bool found_ref = false;
        for (const engine::IndexedPageRef& page : page_refs_of(*reader)) {
            const engine::PageRef page_ref{page.ref.offset, page.ref.length,
                                           page.ref.crc};
            auto bytes = reader->read_page(page_ref);
            REQUIRE(bytes.has_value());
            for (const engine::PageSection& section : page.ref.sections) {
                auto view = SectionView::open(view_of(*bytes).subspan(
                    section.offset_in_page, section.length));
                REQUIRE(view.has_value());
                auto cursor = view->lower_bound(ByteView{});
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
        CHECK(ref.length == big.size());

        std::vector<std::uint8_t> bytes = read_file(to_std(writer->path()));
        REQUIRE(ref.offset + ref.length <= bytes.size());
        bytes[static_cast<std::size_t>(ref.offset) + 4096U] ^= 0xFFU;
        const std::string corrupt_path = corrupt_dir.file("w-corrupt.dfs");
        write_file(corrupt_path, bytes);

        {
            // A closed file is synced: the bad run fails the read of its own
            // value, and the rest of the commit reads.
            auto corrupt = Snapshot::open(corrupt_dir.str());
            REQUIRE(corrupt.has_value());
            CHECK(corrupt->state(spec, scope) == ScopeState::Ready);
            auto bad = corrupt->get(spec, scope, view_of(std::string("big")));
            REQUIRE_FALSE(bad.has_value());
            CHECK(bad.error().is(dstd::Condition::Parse));
            auto other =
                corrupt->get(spec, scope, view_of(std::string("small")));
            REQUIRE(other.has_value());
            CHECK(other->has_value());
            // No directory or section block names the run, but check() reads
            // it through the entry that holds it.
            auto report = corrupt->check();
            REQUIRE(report.has_value());
            CHECK(report->blocks == 2);
            CHECK(report->sections == 1);
            CHECK(report->bad_sections.empty());
            REQUIRE(report->bad_commits.size() == 1);
            CHECK(report->bad_commits[0].sequence == 1);
        }
        {
            auto sound = Snapshot::open(dir.str());
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
        auto corrupt = Snapshot::open(corrupt_dir.str());
        REQUIRE(corrupt.has_value());
        CHECK(corrupt->state(spec, scope) == ScopeState::Bad);
        auto bad = corrupt->get(spec, scope, view_of(std::string("big")));
        REQUIRE(bad.has_value());
        CHECK_FALSE(bad->has_value());
        auto other = corrupt->get(spec, scope, view_of(std::string("small")));
        REQUIRE(other.has_value());
        CHECK_FALSE(other->has_value());
    }

    TEST_CASE("a batch larger than a page is one section in a run of its own") {
        dftu_utils_test::ScopedTestDir dir("snapshot_split");
        const ScopeKey scope = file_scope("/traces/many.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        constexpr int COUNT = 5000;

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        for (int i = 0; i < COUNT; ++i) {
            const std::string key = key_of(i);
            const std::string value = value_of(i);
            batch.put(view_of(key), view_of(value));
        }
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto reader = DataFileReader::open(to_std(writer->path()));
        REQUIRE(reader.has_value());
        // The batch is one section now instead of a run of page-sized ones:
        // it is longer than a page and is written as a run of its own.
        REQUIRE(page_refs_of(*reader).size() == 1);
        const engine::PageRefItem run = page_refs_of(*reader)[0].ref;
        REQUIRE(run.sections.size() == 1);
        CHECK(run.sections[0].entry_count == static_cast<std::uint64_t>(COUNT));
        CHECK(run.length > SMALL_PAGE);
        CHECK(run.length % SMALL_PAGE == 0);

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
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
        const std::string key = "shared";

        {
            auto first = make_writer(dir);
            auto id = first->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = first->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(std::string("from-first")));
            REQUIRE(first->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*first).has_value());
        }

        {
            auto second = make_writer(dir);
            auto id = second->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = second->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(std::string("from-second")));
            REQUIRE(second->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*second).has_value());
        }

        auto newest = Snapshot::open(dir.str());
        REQUIRE(newest.has_value());
        auto newest_value = newest->get(spec, scope, view_of(key));
        REQUIRE(newest_value.has_value());
        REQUIRE(newest_value->has_value());
        CHECK(to_std((*newest_value)->bytes()) == "from-second");

        // A writer in an empty directory starts its scope at generation 1, so
        // the two files below hold the same generation and tie.
        std::string left_path;
        {
            auto writer = make_writer(left);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(std::string("from-left")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            left_path = to_std(writer->path());
        }
        std::string right_path;
        {
            auto writer = make_writer(right);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(std::string("from-right")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            right_path = to_std(writer->path());
        }
        CHECK(only_copy_generation(left_path) == 1);
        CHECK(only_copy_generation(right_path) == 1);

        // The higher writer id wins, and here it holds the smaller name.
        const std::string tied_low =
            forge_copy(left_path, by_writer, "w-z.dfs", 100);
        const std::string tied_high =
            forge_copy(right_path, by_writer, "w-a.dfs", 200);
        CHECK(header_writer_random(tied_low) == 100);
        CHECK(header_writer_random(tied_high) == 200);
        CHECK(only_copy_generation(tied_low) == 1);
        CHECK(only_copy_generation(tied_high) == 1);
        auto tie = Snapshot::open(by_writer.str());
        REQUIRE(tie.has_value());
        auto tie_value = tie->get(spec, scope, view_of(key));
        REQUIRE(tie_value.has_value());
        REQUIRE(tie_value->has_value());
        CHECK(to_std((*tie_value)->bytes()) == "from-right");

        // Equal writer ids: the larger file name wins.
        const std::string named_low =
            forge_copy(right_path, by_name, "w-a.dfs", 300);
        const std::string named_high =
            forge_copy(left_path, by_name, "w-b.dfs", 300);
        CHECK(header_writer_random(named_low) == 300);
        CHECK(header_writer_random(named_high) == 300);
        CHECK(only_copy_generation(named_low) == 1);
        CHECK(only_copy_generation(named_high) == 1);
        auto name_tie = Snapshot::open(by_name.str());
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
            batch.put(view_of(std::string("key-a")), view_of(std::string("a")));
            batch.put(view_of(std::string("key-b")), view_of(std::string("b")));
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
            auto dropped = Snapshot::open(dir.str());
            REQUIRE(dropped.has_value());
            CHECK(dropped->state(spec, scope) == ScopeState::Absent);
            auto found =
                dropped->get(spec, scope, view_of(std::string("key-a")));
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
            auto cursor = dropped->scan(spec, scope, ByteView{}, ByteView{});
            CHECK(cursor.at_end());
        }

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(std::string("key-z")), view_of(std::string("z")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto rewritten = Snapshot::open(dir.str());
        REQUIRE(rewritten.has_value());
        CHECK(rewritten->state(spec, scope) == ScopeState::Ready);
        auto old_key =
            rewritten->get(spec, scope, view_of(std::string("key-a")));
        REQUIRE(old_key.has_value());
        CHECK_FALSE(old_key->has_value());
        auto cursor = rewritten->scan(spec, scope, ByteView{}, ByteView{});
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
            for (const std::string key : {"key-a", "key-b", "key-c", "key-d"}) {
                batch.put(view_of(key), view_of("v-" + key));
            }
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            const std::string begin = "key-b";
            const std::string end = "key-d";
            REQUIRE(writer->drop(*id, scope, view_of(begin), view_of(end))
                        .has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        for (const std::string key : {"key-b", "key-c"}) {
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
        }
        for (const std::string key : {"key-a", "key-d"}) {
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == "v-" + key);
        }
        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].first == "key-a");
        CHECK(rows[0].second == "v-key-a");
        CHECK(rows[1].first == "key-d");
        CHECK(rows[1].second == "v-key-d");
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

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Failed);
        auto found = snapshot->get(spec, scope, view_of(std::string("key-a")));
        REQUIRE(found.has_value());
        CHECK_FALSE(found->has_value());
        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        CHECK(cursor.at_end());
    }

    TEST_CASE("a corrupt page makes its copy Bad and leaves other scopes") {
        dftu_utils_test::ScopedTestDir dir("snapshot_bad_page");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_bad_page_copy");
        const ScopeKey broken = file_scope("/traces/broken.pfw");
        const ScopeKey sound = file_scope("/traces/sound.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        for (int batch = 1; batch <= 3; ++batch) {
            Batch entries = writer->batch(*id, broken, 0);
            const std::string key = "broken-" + std::to_string(batch);
            const std::string value = "broken-value-" + std::to_string(batch);
            entries.put(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        // One page mixes the sections of every scope of a namespace, so the
        // broken scope is sealed into a page of its own here, and the sound
        // scope writes into the next one.
        REQUIRE(flush_writer(*writer).has_value());
        for (int batch = 1; batch <= 2; ++batch) {
            Batch entries = writer->batch(*id, sound, 0);
            const std::string key = "sound-" + std::to_string(batch);
            const std::string value = "sound-value-" + std::to_string(batch);
            entries.put(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());

        auto reader = DataFileReader::open(to_std(writer->path()));
        REQUIRE(reader.has_value());
        REQUIRE(page_refs_of(*reader).size() == 2);
        engine::PageRef broken_page{};
        broken_page.offset = page_refs_of(*reader)[0].ref.offset;
        broken_page.length = page_refs_of(*reader)[0].ref.length;
        broken_page.crc = page_refs_of(*reader)[0].ref.crc;

        std::vector<std::uint8_t> bytes = read_file(to_std(writer->path()));
        REQUIRE(broken_page.offset + broken_page.length <= bytes.size());
        bytes[static_cast<std::size_t>(broken_page.offset) + 4U] ^= 0xFFU;
        const std::string copy_path = copy_dir.file("w-bad.dfs");
        write_file(copy_path, without_footer(bytes));

        auto snapshot = Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, broken) == ScopeState::Bad);
        auto hidden =
            snapshot->get(spec, broken, view_of(std::string("broken-1")));
        REQUIRE(hidden.has_value());
        CHECK_FALSE(hidden->has_value());
        auto broken_cursor =
            snapshot->scan(spec, broken, ByteView{}, ByteView{});
        CHECK(broken_cursor.at_end());

        CHECK(snapshot->state(spec, sound) == ScopeState::Ready);
        auto visible =
            snapshot->get(spec, sound, view_of(std::string("sound-2")));
        REQUIRE(visible.has_value());
        REQUIRE(visible->has_value());
        CHECK(to_std((*visible)->bytes()) == "sound-value-2");
        auto sound_cursor = snapshot->scan(spec, sound, ByteView{}, ByteView{});
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
            const std::string key = "commit-" + std::to_string(batch);
            const std::string value = "value-" + std::to_string(batch);
            entries.put(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(entries)).has_value());
            if (batch == 3) {
                // A group commit is what makes a commit visible, so the copy
                // holds the three commits flushed here and neither of the two
                // after.
                REQUIRE(flush_writer(*writer).has_value());
                write_file(copy_dir.file("w-crash.dfs"),
                           read_file(to_std(writer->path())));
            }
        }

        auto snapshot = Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        for (int batch = 1; batch <= 3; ++batch) {
            const std::string key = "commit-" + std::to_string(batch);
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) ==
                  "value-" + std::to_string(batch));
        }
        for (int batch = 4; batch <= 5; ++batch) {
            const std::string key = "commit-" + std::to_string(batch);
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
        }
        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        CHECK(collect(cursor).size() == 3);
        REQUIRE(close_writer(*writer).has_value());
    }

    TEST_CASE("a file of another format version is skipped and listed") {
        dftu_utils_test::ScopedTestDir dir("snapshot_unsupported");
        const ScopeKey scope = file_scope("/traces/u.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        batch.put(view_of(std::string("key-a")), view_of(std::string("a")));
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        // A file without a footer takes its version from the header, and a
        // closed file from the tail of its footer.
        std::vector<std::uint8_t> bytes = read_file(to_std(writer->path()));
        std::vector<std::uint8_t> header_one = bytes;
        dftracer::utils::write_u32_le(header_one.data() + 8, 1);
        rewrite_header_crc(header_one);
        write_file(dir.file("w-header-one.dfs"), without_footer(header_one));
        write_file(dir.file("w-tail-one.dfs"), with_tail_version(bytes, 1));

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->skipped().size() == 2);
        CHECK(to_std(snapshot->skipped()[0].name) == "w-header-one.dfs");
        CHECK(snapshot->skipped()[0].error.is(Condition::Unsupported));
        CHECK(to_std(snapshot->skipped()[1].name) == "w-tail-one.dfs");
        CHECK(snapshot->skipped()[1].error.is(Condition::Unsupported));
        auto found = snapshot->get(spec, scope, view_of(std::string("key-a")));
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
        first_batch.put(view_of(std::string("a-key")),
                        view_of(std::string("a")));
        REQUIRE(writer->commit(std::move(first_batch)).has_value());
        Batch second_batch = writer->batch(*put_id, second, 0);
        second_batch.put(view_of(std::string("b-key")),
                         view_of(std::string("b")));
        REQUIRE(writer->commit(std::move(second_batch)).has_value());

        Batch first_sum = writer->batch(*sum_id, first, 0);
        first_sum.fold(view_of(std::string("total")), view_of(i64_bytes(1)));
        REQUIRE(writer->commit(std::move(first_sum)).has_value());
        Batch second_sum = writer->batch(*sum_id, second, 0);
        second_sum.fold(view_of(std::string("total")), view_of(i64_bytes(3)));
        REQUIRE(writer->commit(std::move(second_sum)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());

        {
            dstd::Vector<ScopeKey> order;
            order.push_back(second);
            order.push_back(first);
            auto cursor = snapshot->scan(put, order, ByteView{}, ByteView{});
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
            auto cursor = snapshot->scan_all(put, ByteView{}, ByteView{});
            const auto rows = collect_scoped(cursor);
            REQUIRE(rows.size() == 2);
            CHECK(rows[0].scope == "/traces/a.pfw");
            CHECK(rows[0].key == "a-key");
            CHECK(rows[1].scope == "/traces/b.pfw");
            CHECK(rows[1].key == "b-key");
        }

        {
            auto cursor = snapshot->scan_all(sum, ByteView{}, ByteView{});
            const auto rows = collect_scoped(cursor);
            REQUIRE(rows.size() == 1);
            CHECK(rows[0].key == "total");
            CHECK(read_i64(view_of(rows[0].value)) == 4);
        }

        {
            auto cursor = snapshot->scan(sum, first, ByteView{}, ByteView{});
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
        batch.put(view_of(std::string("key-a")), view_of(std::string("a")));
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(writer_spec, scope) == ScopeState::Ready);
        CHECK(snapshot->state(other_spec, scope) == ScopeState::Absent);
        auto hidden =
            snapshot->get(other_spec, scope, view_of(std::string("key-a")));
        REQUIRE(hidden.has_value());
        CHECK_FALSE(hidden->has_value());
        auto cursor = snapshot->scan(other_spec, scope, ByteView{}, ByteView{});
        CHECK(cursor.at_end());
        auto visible =
            snapshot->get(writer_spec, scope, view_of(std::string("key-a")));
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

        std::string replaced_path;
        {
            auto writer = make_writer(replaced);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch first = writer->batch(*id, scope, 0);
            first.put(view_of(std::string("key-one")),
                      view_of(std::string("one")));
            REQUIRE(writer->commit(std::move(first)).has_value());
            REQUIRE(writer->drop(*id, scope).has_value());
            Batch second = writer->batch(*id, scope, 0);
            second.put(view_of(std::string("key-two")),
                       view_of(std::string("two")));
            REQUIRE(writer->commit(std::move(second)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            replaced_path = to_std(writer->path());
        }
        CHECK((copy_generations(replaced_path) ==
               std::vector<std::uint64_t>{1, 3}));
        CHECK(
            (drop_generations(replaced_path) == std::vector<std::uint64_t>{2}));
        {
            auto snapshot = Snapshot::open(replaced.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
            auto hidden =
                snapshot->get(spec, scope, view_of(std::string("key-one")));
            REQUIRE(hidden.has_value());
            CHECK_FALSE(hidden->has_value());
            auto visible =
                snapshot->get(spec, scope, view_of(std::string("key-two")));
            REQUIRE(visible.has_value());
            REQUIRE(visible->has_value());
            CHECK(to_std((*visible)->bytes()) == "two");
            auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
            const auto rows = collect(cursor);
            REQUIRE(rows.size() == 1);
            CHECK(rows[0].first == "key-two");
            CHECK(rows[0].second == "two");
            check_generation(*snapshot, "dftu.test.put", scope, 3);
        }

        std::string failed_path;
        {
            auto writer = make_writer(failed);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            REQUIRE(writer->fail(*id, scope, 7).has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(std::string("key")), view_of(std::string("v")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            failed_path = to_std(writer->path());
        }
        CHECK((copy_generations(failed_path) ==
               std::vector<std::uint64_t>{1, 2}));
        CHECK(drop_generations(failed_path).empty());
        {
            auto snapshot = Snapshot::open(failed.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
            auto found =
                snapshot->get(spec, scope, view_of(std::string("key")));
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
            first.put(view_of(std::string("key-one")),
                      view_of(std::string("one")));
            REQUIRE(writer->commit(std::move(first)).has_value());
            REQUIRE(writer->drop(*id, scope).has_value());
            Batch second = writer->batch(*id, scope, 0);
            second.put(view_of(std::string("key-two")),
                       view_of(std::string("two")));
            REQUIRE(writer->commit(std::move(second)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }
        std::string next_path;
        {
            auto writer = make_writer(next);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(std::string("key-three")),
                      view_of(std::string("three")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            next_path = to_std(writer->path());
        }
        CHECK((copy_generations(next_path) == std::vector<std::uint64_t>{4}));
        {
            auto snapshot = Snapshot::open(next.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
            auto found =
                snapshot->get(spec, scope, view_of(std::string("key-three")));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == "three");
            auto gone =
                snapshot->get(spec, scope, view_of(std::string("key-two")));
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
        const std::string key = "same-key";

        auto writer = make_writer(dir);
        auto last_id = writer->add_namespace(last);
        REQUIRE(last_id.has_value());
        auto concat_id = writer->add_namespace(concat);
        REQUIRE(concat_id.has_value());

        const auto fold_last = [&](const ScopeKey& scope,
                                   const std::string& value) {
            Batch batch = writer->batch(*last_id, scope, 0);
            batch.fold(view_of(key), view_of(value));
            REQUIRE(writer->commit(std::move(batch)).has_value());
        };
        const auto fold_concat = [&](const ScopeKey& scope,
                                     const std::string& value) {
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

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        const auto expect_one = [&key](engine::ScanCursor& cursor,
                                       const std::string& expected) {
            const auto rows = collect(cursor);
            REQUIRE(rows.size() == 1);
            CHECK(rows[0].first == key);
            CHECK(rows[0].second == expected);
        };

        {
            auto cursor = snapshot->scan(last, first, ByteView{}, ByteView{});
            expect_one(cursor, "a2");
        }
        {
            auto cursor = snapshot->scan(concat, first, ByteView{}, ByteView{});
            expect_one(cursor, "a1a2");
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(first);
            order.push_back(second);
            auto cursor = snapshot->scan(last, order, ByteView{}, ByteView{});
            expect_one(cursor, "b2");
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(first);
            order.push_back(second);
            auto cursor = snapshot->scan(concat, order, ByteView{}, ByteView{});
            expect_one(cursor, "a1a2b1b2");
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(second);
            order.push_back(first);
            auto cursor = snapshot->scan(last, order, ByteView{}, ByteView{});
            expect_one(cursor, "a2");
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(second);
            order.push_back(first);
            auto cursor = snapshot->scan(concat, order, ByteView{}, ByteView{});
            expect_one(cursor, "b1b2a1a2");
        }
        {
            auto cursor = snapshot->scan_all(last, ByteView{}, ByteView{});
            expect_one(cursor, "b2");
        }
        {
            auto cursor = snapshot->scan_all(concat, ByteView{}, ByteView{});
            expect_one(cursor, "a1a2b1b2");
        }
    }

    TEST_CASE("a whole drop at the generation of a copy does not hide it") {
        dftu_utils_test::ScopedTestDir dropped("snapshot_equal_drop_dropped");
        dftu_utils_test::ScopedTestDir copied("snapshot_equal_drop_copied");
        dftu_utils_test::ScopedTestDir merged("snapshot_equal_drop_merged");
        const ScopeKey scope = file_scope("/traces/e.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const std::string key = "shared";

        std::string drop_path;
        {
            auto writer = make_writer(dropped);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(std::string("dropped")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(writer->drop(*id, scope).has_value());
            REQUIRE(close_writer(*writer).has_value());
            drop_path = to_std(writer->path());
        }
        CHECK((drop_generations(drop_path) == std::vector<std::uint64_t>{2}));

        std::string first_path;
        {
            auto writer = make_writer(copied);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(std::string("old")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            first_path = to_std(writer->path());
        }
        std::string second_path;
        {
            auto writer = make_writer(copied);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            batch.put(view_of(key), view_of(std::string("kept")));
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
            second_path = to_std(writer->path());
        }
        CHECK(only_copy_generation(second_path) == 2);

        forge_copy(first_path, merged, "w-first.dfs", 20);
        forge_copy(second_path, merged, "w-second.dfs", 30);
        forge_copy(drop_path, merged, "w-drop.dfs", 10);

        auto snapshot = Snapshot::open(merged.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        auto found = snapshot->get(spec, scope, view_of(key));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == "kept");
        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].first == key);
        CHECK(rows[0].second == "kept");
        check_generation(*snapshot, "dftu.test.put", scope, 2);
    }

    TEST_CASE("a bad page marks every copy of its commit") {
        dftu_utils_test::ScopedTestDir built("snapshot_bad_commit_built");
        dftu_utils_test::ScopedTestDir corrupt("snapshot_bad_commit_corrupt");
        const ScopeKey first = file_scope("/traces/a.pfw");
        const ScopeKey second = file_scope("/traces/b.pfw");
        const ScopeKey third = file_scope("/traces/c.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        const auto seal = [](const std::string& key, const std::string& value) {
            engine::SectionBuilder builder;
            builder.add(view_of(key), view_of(value));
            auto sealed = builder.seal(nullptr, nullptr, nullptr);
            REQUIRE(sealed.has_value());
            return std::move(*sealed);
        };
        const auto add_scope = [](engine::DataFileWriter& file,
                                  std::uint32_t id, const std::string& path) {
            engine::ScopeItem item;
            item.local_id = id;
            item.kind = engine::ScopeKind::File;
            engine::ScopePath entry;
            entry.logical_path = dstring(path);
            item.paths.push_back(std::move(entry));
            file.add(std::move(item));
        };
        const auto add_copy = [](engine::DataFileWriter& file, std::uint32_t id,
                                 std::uint32_t scope_id) {
            engine::CopyItem item;
            item.local_id = id;
            item.namespace_id = 1;
            item.scope_id = scope_id;
            item.generation = 1;
            item.state = engine::CopyState::Ready;
            file.add(std::move(item));
        };
        const auto page_section = [](std::uint32_t copy_id,
                                     std::uint32_t offset,
                                     const std::string& key,
                                     const dstd::Vector<std::uint8_t>& bytes) {
            engine::PageSection section;
            section.copy_id = copy_id;
            section.offset_in_page = offset;
            section.length = static_cast<std::uint32_t>(bytes.size());
            section.entry_count = 1;
            section.first_key = bytes_vector(key);
            section.last_key = bytes_vector(key);
            return section;
        };
        const auto append_page =
            [](engine::DataFileWriter& file, std::uint64_t sequence,
               const dstd::Vector<std::uint8_t>& page,
               dstd::Vector<engine::PageSection> sections) {
                auto reference = file.append_page(view_of(page));
                REQUIRE(reference.has_value());
                engine::PageRefItem item;
                item.offset = reference->offset;
                item.length = reference->length;
                item.crc = reference->crc;
                item.sequence = sequence;
                item.sections = std::move(sections);
                return item;
            };

        const std::string path = built.file("w-built.dfs");
        engine::PageRefItem shared{};
        {
            auto file = engine::DataFileWriter::create(path, SMALL_PAGE, "host",
                                                       "label");
            REQUIRE(file.has_value());

            engine::NamespaceItem namespaces;
            namespaces.local_id = 1;
            namespaces.name = dstring("dftu.test.put");
            file->add(std::move(namespaces));
            add_scope(*file, 1, "/traces/a.pfw");
            add_copy(*file, 1, 1);
            const auto section_a1 = seal("a-1", "a-value-1");
            dstd::Vector<std::uint8_t> first_page(section_a1.begin(),
                                                  section_a1.end());
            dstd::Vector<engine::PageSection> first_sections;
            first_sections.push_back(page_section(1, 0, "a-1", section_a1));
            file->add(
                append_page(*file, 1, first_page, std::move(first_sections)));
            REQUIRE(file->commit().has_value());

            // One commit whose single page holds a section of copy 1 and a
            // section of copy 2.
            add_scope(*file, 2, "/traces/b.pfw");
            add_copy(*file, 2, 2);
            const auto section_a2 = seal("a-2", "a-value-2");
            const auto section_b1 = seal("b-1", "b-value-1");
            dstd::Vector<std::uint8_t> shared_page(section_a2.begin(),
                                                   section_a2.end());
            shared_page.insert(shared_page.end(), section_b1.begin(),
                               section_b1.end());
            dstd::Vector<engine::PageSection> shared_sections;
            shared_sections.push_back(page_section(1, 0, "a-2", section_a2));
            shared_sections.push_back(
                page_section(2, static_cast<std::uint32_t>(section_a2.size()),
                             "b-1", section_b1));
            shared =
                append_page(*file, 2, shared_page, std::move(shared_sections));
            file->add(shared);
            REQUIRE(file->commit().has_value());

            add_scope(*file, 3, "/traces/c.pfw");
            add_copy(*file, 3, 3);
            const auto section_c1 = seal("c-1", "c-value-1");
            dstd::Vector<std::uint8_t> third_page(section_c1.begin(),
                                                  section_c1.end());
            dstd::Vector<engine::PageSection> third_sections;
            third_sections.push_back(page_section(3, 0, "c-1", section_c1));
            file->add(
                append_page(*file, 3, third_page, std::move(third_sections)));
            REQUIRE(file->commit().has_value());
            REQUIRE(file->close().has_value());
        }

        std::vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(shared.offset + shared.length <= bytes.size());
        bytes[static_cast<std::size_t>(shared.offset) + 4U] ^= 0xFFU;
        write_file(corrupt.file("w-bad.dfs"), without_footer(bytes));

        auto snapshot = Snapshot::open(corrupt.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, first) == ScopeState::Bad);
        CHECK(snapshot->state(spec, second) == ScopeState::Bad);
        CHECK(snapshot->state(spec, third) == ScopeState::Ready);
        for (const auto& hidden : {std::pair{first, std::string("a-1")},
                                   std::pair{first, std::string("a-2")},
                                   std::pair{second, std::string("b-1")}}) {
            auto found =
                snapshot->get(spec, hidden.first, view_of(hidden.second));
            REQUIRE(found.has_value());
            CHECK_FALSE(found->has_value());
        }
        {
            dstd::Vector<ScopeKey> order;
            order.push_back(first);
            order.push_back(second);
            auto cursor = snapshot->scan(spec, order, ByteView{}, ByteView{});
            CHECK(cursor.at_end());
        }
        auto visible = snapshot->get(spec, third, view_of(std::string("c-1")));
        REQUIRE(visible.has_value());
        REQUIRE(visible->has_value());
        CHECK(to_std((*visible)->bytes()) == "c-value-1");
        auto cursor = snapshot->scan(spec, third, ByteView{}, ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].first == "c-1");
        CHECK(rows[0].second == "c-value-1");
    }

    TEST_CASE("an entry larger than a page commits as a run of its own") {
        const std::size_t lengths[] = {3900, 3925, 3950, 3975,
                                       4000, 4025, 4050};
        const auto large_key = [&lengths](std::size_t index) {
            std::string key(lengths[index], 'k');
            key[key.size() - 1] = static_cast<char>('a' + index);
            return key;
        };
        const ScopeKey scope = file_scope("/traces/large.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        dftu_utils_test::ScopedTestDir dir("snapshot_page_entry_large");
        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            for (std::size_t index = 0; index < std::size(lengths); ++index) {
                Batch batch = writer->batch(*id, scope, 0);
                batch.put(view_of(large_key(index)),
                          view_of(std::string("value")));
                REQUIRE(writer->commit(std::move(batch)).has_value());
            }
            REQUIRE(flush_writer(*writer).has_value());

            // One batch is one section, so a section of a page or more is a
            // run of its own: it is no longer a failed commit.
            auto reader = DataFileReader::open(to_std(writer->path()));
            REQUIRE(reader.has_value());
            REQUIRE(page_refs_of(*reader).size() == std::size(lengths));
            for (const engine::IndexedPageRef& page : page_refs_of(*reader)) {
                REQUIRE(page.ref.sections.size() == 1);
                CHECK(page.ref.length > SMALL_PAGE);
                CHECK(page.ref.length % SMALL_PAGE == 0);
            }
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        for (std::size_t index = 0; index < std::size(lengths); ++index) {
            auto found = snapshot->get(spec, scope, view_of(large_key(index)));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == "value");
        }
        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == std::size(lengths));
        for (std::size_t index = 0; index < std::size(lengths); ++index) {
            CHECK(rows[index].first == large_key(index));
            CHECK(rows[index].second == "value");
        }
    }

    TEST_CASE("a concat batch is sized by its folded length") {
        dftu_utils_test::ScopedTestDir dir("snapshot_page_concat");
        const ScopeKey scope = file_scope("/traces/concat.pfw");
        const NamespaceSpec spec =
            fold_spec("dftu.test.concat", FoldKind::Concat);
        constexpr std::size_t VALUE_COUNT = 200;
        const std::string keys[] = {"dup-a", "dup-b"};

        std::string expected;
        for (std::size_t index = 0; index < VALUE_COUNT; ++index) {
            expected += concat_value(static_cast<int>(index));
        }
        REQUIRE(expected.size() == VALUE_COUNT * 17);

        {
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            Batch batch = writer->batch(*id, scope, 0);
            for (const std::string& key : keys) {
                for (std::size_t index = 0; index < VALUE_COUNT; ++index) {
                    batch.fold(view_of(key),
                               view_of(concat_value(static_cast<int>(index))));
                }
            }
            REQUIRE(writer->commit(std::move(batch)).has_value());
            REQUIRE(close_writer(*writer).has_value());
        }

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
        for (const std::string& key : keys) {
            auto found = snapshot->get(spec, scope, view_of(key));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == expected);
        }
        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].first == "dup-a");
        CHECK(rows[0].second == expected);
        CHECK(rows[1].first == "dup-b");
        CHECK(rows[1].second == expected);
    }

    TEST_CASE("a batch that fills a page exactly stays in one section") {
        dftu_utils_test::ScopedTestDir dir("snapshot_page_exact");
        const ScopeKey scope = file_scope("/traces/exact.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        // 64 entries of 512 bytes and one of 451 bytes per sub-page make the
        // section 131072 bytes, i.e. exactly one page of the size named here.
        constexpr std::uint32_t PAGE = 128U * 1024U;
        constexpr std::size_t SMALL_VALUE = 446;
        constexpr std::size_t BIG_COUNT = 255;
        constexpr std::size_t BIG_VALUE = 508;

        WriterOptions options;
        options.page_size = PAGE;
        options.host = dstring("host");
        options.label = dstring("label");
        auto writer = make_writer(dir, options);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        Batch batch = writer->batch(*id, scope, 0);
        const std::string small_key("\0\x01", 2);
        batch.put(view_of(small_key), view_of(std::string(SMALL_VALUE, 's')));
        for (std::size_t index = 0; index < BIG_COUNT; ++index) {
            const std::string key(1, static_cast<char>(index + 1));
            batch.put(view_of(key), view_of(std::string(BIG_VALUE, 'v')));
        }
        REQUIRE(writer->commit(std::move(batch)).has_value());
        REQUIRE(close_writer(*writer).has_value());

        auto reader = DataFileReader::open(to_std(writer->path()));
        REQUIRE(reader.has_value());
        REQUIRE(page_refs_of(*reader).size() == 1);
        REQUIRE(page_refs_of(*reader)[0].ref.sections.size() == 1);
        CHECK(page_refs_of(*reader)[0].ref.sections[0].entry_count ==
              BIG_COUNT + 1);
        CHECK(page_refs_of(*reader)[0].ref.length == PAGE);

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        auto cursor = snapshot->scan(spec, scope, ByteView{}, ByteView{});
        const auto rows = collect(cursor);
        REQUIRE(rows.size() == BIG_COUNT + 1);
        CHECK(rows[0].first == small_key);
        CHECK(rows[0].second == std::string(SMALL_VALUE, 's'));
        for (std::size_t index = 0; index < BIG_COUNT; ++index) {
            CHECK(rows[index + 1].first ==
                  std::string(1, static_cast<char>(index + 1)));
            CHECK(rows[index + 1].second == std::string(BIG_VALUE, 'v'));
        }
        auto found =
            snapshot->get(spec, scope, view_of(std::string(1, '\x05')));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == std::string(BIG_VALUE, 'v'));
    }

    TEST_CASE(
        "a root of four writers opens the same with one and eight threads") {
        dftu_utils_test::ScopedTestDir dir("snapshot_open_threads");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        constexpr int WRITERS = 4;
        constexpr int BATCHES = 8;
        constexpr int KEYS = 1000;
        std::vector<ScopeKey> scopes;
        for (int w = 0; w < WRITERS; ++w) {
            scopes.push_back(
                file_scope("/traces/w" + std::to_string(w) + ".pfw"));
        }
        for (int w = 0; w < WRITERS; ++w) {
            write_scoped_file(dir, spec, scopes[static_cast<std::size_t>(w)],
                              BATCHES, KEYS, false);
        }

        const OpenDump one = [&] {
            ScopedDefaultRuntime runtime(1);
            return dump_open(dir, spec, scopes);
        }();
        const OpenDump eight = [&] {
            ScopedDefaultRuntime runtime(8);
            return dump_open(dir, spec, scopes);
        }();

        // Every namespace, scope state and row is the same either way.
        CHECK(one.text == eight.text);
        CHECK(one.files == WRITERS);
        CHECK(eight.files == WRITERS);
        CHECK(one.pages == eight.pages);
        for (int w = 0; w < WRITERS; ++w) {
            CHECK(one.text.find("state /traces/w" + std::to_string(w) +
                                ".pfw 1") != std::string::npos);
        }
        CHECK(one.text.find("b0-key-0000=value-0") != std::string::npos);
        CHECK(one.pages >= WRITERS * BATCHES);
        CHECK(one.pages == eight.pages);
        // A lazy open reads no block, and the dump reads every page once. The
        // files closed, so every commit is synced and none is checked whole.
        CHECK(eight.read_calls == one.read_calls);
        CHECK(eight.commits == one.commits);
        CHECK(eight.commits >= WRITERS);
        CHECK(eight.commits_verified == 0);
        CHECK(one.commits_verified == 0);
        CHECK(one.blocks_read == one.pages);
        CHECK(eight.blocks_read == eight.pages);
        // The files closed, so the dump reads the directory and section blocks
        // of each scope, one call each, and no commit record.
        CHECK(one.indexes_read == 0);
        CHECK(eight.indexes_read == 0);
        CHECK(eight.metadata_blocks_read == one.metadata_blocks_read);
        CHECK(eight.metadata_blocks_read >= 2U * WRITERS);
        CHECK(eight.read_calls ==
              eight.blocks_read + eight.metadata_blocks_read);
    }

    TEST_CASE("a corrupt page in a middle run marks only its commit bad") {
        dftu_utils_test::ScopedTestDir dir("snapshot_open_corrupt");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_open_corrupt_copy");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const std::vector<ScopeKey> scopes = {file_scope("/traces/a.pfw"),
                                              file_scope("/traces/b.pfw"),
                                              file_scope("/traces/c.pfw")};
        write_scopes_file(dir, spec, scopes, 1000, true);
        const std::string path =
            std::filesystem::directory_iterator(dir.str())->path().string();

        auto reader = DataFileReader::open(path);
        REQUIRE(reader.has_value());
        REQUIRE(page_refs_of(*reader).size() == 3);
        // One group commit per scope, so the middle reference is one commit's
        // pages, read in one preadv and crc-checked as one block.
        const engine::PageRefItem middle = page_refs_of(*reader)[1].ref;
        CHECK(middle.length > SMALL_PAGE);
        CHECK(middle.length % SMALL_PAGE == 0);

        std::vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(middle.offset + middle.length <= bytes.size());
        // The last byte of the reference is padding no section covers, so only
        // the block crc sees it.
        bytes[static_cast<std::size_t>(middle.offset) + middle.length - 1U] ^=
            0xFFU;
        write_file(copy_dir.file("w-corrupt.dfs"), without_footer(bytes));

        const auto check = [&](std::size_t threads) {
            ScopedDefaultRuntime runtime(threads);
            auto snapshot = Snapshot::open(copy_dir.str());
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->skipped().empty());
            CHECK(snapshot->state(spec, scopes[1]) == ScopeState::Bad);
            CHECK(snapshot->state(spec, scopes[0]) == ScopeState::Ready);
            CHECK(snapshot->state(spec, scopes[2]) == ScopeState::Ready);
            auto hidden = snapshot->get(spec, scopes[1],
                                        view_of(std::string("b0-key-0000")));
            REQUIRE(hidden.has_value());
            CHECK_FALSE(hidden->has_value());
            auto visible = snapshot->get(spec, scopes[2],
                                         view_of(std::string("b0-key-0000")));
            REQUIRE(visible.has_value());
            REQUIRE(visible->has_value());
            CHECK(to_std((*visible)->bytes()) == "value-0");
            auto cursor =
                snapshot->scan(spec, scopes[0], ByteView{}, ByteView{});
            CHECK(collect(cursor).size() == 1000);
            CHECK(snapshot->open_stats().files == 1);
            CHECK(snapshot->open_stats().pages == 3);
            // The three commits of the file, each read once at its first touch:
            // the queries of one dump touch each of them exactly once.
            CHECK(snapshot->read_stats().commits_verified == 3);
            CHECK(snapshot->read_stats().read_calls == 3);
        };
        check(1);
        check(8);
    }

    TEST_CASE("a block that cannot be read fails its queries with the error") {
        dftu_utils_test::ScopedTestDir dir("snapshot_read_error");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const std::vector<ScopeKey> scopes = {
            file_scope("/traces/a.pfw"), file_scope("/traces/b.pfw"),
            file_scope("/traces/c.pfw"), file_scope("/traces/d.pfw")};
        write_scopes_file(dir, spec, scopes, 1000, true);
        const std::string path =
            std::filesystem::directory_iterator(dir.str())->path().string();
        // Cut as a crash before the close leaves it, so each commit is checked
        // whole on its first touch.
        write_file(path, without_footer(read_file(path)));
        const std::string key_text = "b0-key-0000";
        const ByteView key = view_of(key_text);

        ScopedDefaultRuntime runtime(1);
        auto snapshot = Snapshot::open(dir.str());
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
        auto cursor = snapshot->scan(spec, scopes[1], ByteView{}, ByteView{});
        CHECK(cursor.at_end());
        REQUIRE(cursor.error().has_value());
        CHECK(snapshot->state(spec, scopes[2]) == ScopeState::Bad);
        CHECK(snapshot->read_stats().commits_verified == 0);

        // With the file readable again, a failed commit keeps its error and is
        // not read again, while a commit no query touched reads as usual.
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
        "a bad page of a closed file fails only the queries that read it") {
        dftu_utils_test::ScopedTestDir dir("snapshot_synced_bad_page");
        dftu_utils_test::ScopedTestDir copy_dir(
            "snapshot_synced_bad_page_copy");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey both = file_scope("/traces/both.pfw");
        const ScopeKey late = file_scope("/traces/late.pfw");
        {
            // The second commit holds two pages: one of `both` and one of
            // `late`.
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            const auto put = [&](const ScopeKey& scope,
                                 const std::string& tag) {
                Batch entries = writer->batch(*id, scope, 0);
                for (int k = 0; k < 100; ++k) {
                    const std::string key = tag + "-" + key_of(k);
                    const std::string value = value_of(k);
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
        const std::string path =
            std::filesystem::directory_iterator(dir.str())->path().string();
        auto reader = DataFileReader::open(path);
        REQUIRE(reader.has_value());
        CHECK(reader->index().closed);
        const std::vector<engine::IndexedPageRef> refs = page_refs_of(*reader);
        const engine::PageRefItem* late_page = nullptr;
        for (const engine::IndexedPageRef& ref : refs) {
            for (const engine::PageSection& section : ref.ref.sections) {
                if (to_std(view_of(section.first_key)).rfind("late-", 0) == 0) {
                    late_page = &ref.ref;
                }
            }
        }
        REQUIRE(late_page != nullptr);
        std::vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(late_page->offset + late_page->length <= bytes.size());
        bytes[static_cast<std::size_t>(late_page->offset) + late_page->length -
              1U] ^= 0xFFU;
        const std::string copy_path = copy_dir.file("w-synced-bad.dfs");
        write_file(copy_path, bytes);

        ScopedDefaultRuntime runtime(1);
        auto snapshot = Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        // The commit is synced, so nothing checks it whole: the state is the
        // one its records give.
        CHECK(snapshot->state(spec, late) == ScopeState::Ready);
        CHECK(snapshot->read_stats().blocks_read == 0);

        const std::string late_key = "late-" + key_of(0);
        auto failed = snapshot->get(spec, late, view_of(late_key));
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().is(dstd::Condition::Parse));
        auto late_cursor = snapshot->scan(spec, late, ByteView{}, ByteView{});
        CHECK(late_cursor.at_end());
        REQUIRE(late_cursor.error().has_value());
        CHECK(late_cursor.error()->is(dstd::Condition::Parse));

        // The other page of the same commit reads.
        const std::string both_key = "b1-" + key_of(0);
        auto found = snapshot->get(spec, both, view_of(both_key));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == value_of(0));
        CHECK(snapshot->read_stats().blocks_read == 2);

        // A page that cannot be read keeps its error after the file reads
        // again, while the pages already read stay readable.
        const int data_fd = descriptor_of(copy_path);
        REQUIRE(data_fd >= 0);
        const int saved_fd = ::dup(data_fd);
        REQUIRE(saved_fd >= 0);
        const int dir_fd = ::open(copy_dir.str().c_str(), O_RDONLY);
        REQUIRE(dir_fd >= 0);
        REQUIRE(::dup2(dir_fd, data_fd) == data_fd);
        const std::string first_key = "b0-" + key_of(0);
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

    TEST_CASE("a durable commit marks its pages synced and a close its file") {
        dftu_utils_test::ScopedTestDir dir("snapshot_synced_mark");
        dftu_utils_test::ScopedTestDir cut_dir("snapshot_synced_mark_cut");
        NamespaceSpec durable_spec = put_spec("dftu.test.durable");
        durable_spec.durable = true;
        const NamespaceSpec loose_spec = put_spec("dftu.test.loose");
        const ScopeKey scope = file_scope("/traces/mark.pfw");
        const std::string key_text = "b0-" + key_of(0);

        auto writer = make_writer(dir);
        auto durable_id = writer->add_namespace(durable_spec);
        auto loose_id = writer->add_namespace(loose_spec);
        REQUIRE(durable_id.has_value());
        REQUIRE(loose_id.has_value());
        const auto put = [&](engine::NamespaceId id) {
            Batch entries = writer->batch(id, scope, 0);
            for (int k = 0; k < 100; ++k) {
                const std::string key = "b0-" + key_of(k);
                const std::string value = value_of(k);
                entries.put(view_of(key), view_of(value));
            }
            REQUIRE(writer->commit(std::move(entries)).has_value());
            REQUIRE(flush_writer(*writer).has_value());
        };
        put(*durable_id);
        put(*loose_id);
        const std::string path = to_std(writer->path());
        {
            auto open_file = DataFileReader::open(path);
            REQUIRE(open_file.has_value());
            CHECK_FALSE(open_file->index().closed);
            const auto& refs = page_refs_of(*open_file);
            REQUIRE(refs.size() == 2);
            const auto& first =
                refs[0].sequence < refs[1].sequence ? refs[0] : refs[1];
            const auto& second =
                refs[0].sequence < refs[1].sequence ? refs[1] : refs[0];
            CHECK(first.ref.synced == 1);
            CHECK(second.ref.synced == 0);
        }
        REQUIRE(close_writer(*writer).has_value());
        auto closed_file = DataFileReader::open(path);
        REQUIRE(closed_file.has_value());
        CHECK(closed_file->index().closed);

        // Without the footer, only the durable commit is read page by page;
        // the other is checked whole on its first touch.
        write_file(cut_dir.file("w-mark.dfs"), without_footer(read_file(path)));
        ScopedDefaultRuntime runtime(1);
        auto snapshot = Snapshot::open(cut_dir.str());
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
        "a scan reads each scope's pages only when it reaches the scope") {
        dftu_utils_test::ScopedTestDir dir("snapshot_lazy_scan");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const std::vector<ScopeKey> scopes = {file_scope("/traces/a.pfw"),
                                              file_scope("/traces/b.pfw"),
                                              file_scope("/traces/c.pfw")};
        write_scopes_file(dir, spec, scopes, 1000, true);
        ScopedDefaultRuntime runtime(1);
        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());

        // The begin bound is a temporary: the scan keeps its own copy, and
        // reads it each time a section's page loads.
        auto cursor = snapshot->scan(
            spec, dstd::Span<const ScopeKey>(scopes.data(), scopes.size()),
            view_of(std::string("b0-") + key_of(500)), ByteView{});
        REQUIRE_FALSE(cursor.at_end());
        CHECK(to_std(cursor.key()) == "b0-" + key_of(500));
        // Only the first scope's page is read before the first row.
        CHECK(snapshot->read_stats().blocks_read == 1);
        std::size_t rows = 0;
        while (!cursor.at_end()) {
            ++rows;
            cursor.next();
        }
        CHECK_FALSE(cursor.error().has_value());
        CHECK(rows == 3U * 500U);
        CHECK(snapshot->read_stats().blocks_read == 3);
    }

    TEST_CASE(
        "closed files read through their directories as their chains do") {
        dftu_utils_test::ScopedTestDir dir("snapshot_directory_same");
        dftu_utils_test::ScopedTestDir cut_dir("snapshot_directory_same_cut");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const std::vector<ScopeKey> scopes = {
            file_scope("/traces/a.pfw"), file_scope("/traces/b.pfw"),
            file_scope("/traces/c.pfw"), file_scope("/traces/d.pfw")};
        const auto put = [&](Writer& writer, engine::NamespaceId id,
                             const ScopeKey& scope, const std::string& tag) {
            Batch entries = writer.batch(id, scope, 0);
            for (int k = 0; k < 50; ++k) {
                const std::string key = "key-" + key_of(k);
                const std::string value = tag + "-" + value_of(k);
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
            // A newer copy of a, a whole drop of b, a range drop of c and a
            // new scope d, in a second file.
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            put(*writer, *id, scopes[0], "second");
            REQUIRE(writer->drop(*id, scopes[1]).has_value());
            const std::string begin = "key-" + key_of(10);
            const std::string end = "key-" + key_of(20);
            REQUIRE(writer->drop(*id, scopes[2], view_of(begin), view_of(end))
                        .has_value());
            put(*writer, *id, scopes[3], "second");
            REQUIRE(close_writer(*writer).has_value());
        }
        {
            // More commits than a checkpoint interval, so the chain of the cut
            // file lists most commits by summary, and a big value, over scopes
            // of their own.
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            for (int commit = 0; commit < 150; ++commit) {
                const ScopeKey scope = file_scope(
                    "/traces/m" + std::to_string(commit % 4) + ".pfw");
                Batch entries = writer->batch(*id, scope, 0);
                const std::string key = "many-" + key_of(commit);
                entries.put(view_of(key), view_of(value_of(commit)));
                if (commit == 77) {
                    entries.put(view_of(std::string("many-big")),
                                view_of(std::string(3 * SMALL_PAGE, 'b')));
                }
                REQUIRE(writer->commit(std::move(entries)).has_value());
                REQUIRE(flush_writer(*writer).has_value());
            }
            REQUIRE(close_writer(*writer).has_value());
        }
        for (const auto& entry :
             std::filesystem::directory_iterator(dir.str())) {
            const std::string path = entry.path().string();
            // A light open keeps the directory and leaves the scope items of
            // the footer unread; a full open reads them.
            auto light =
                DataFileReader::open(path, engine::FooterItems::Directory);
            REQUIRE(light.has_value());
            CHECK(light->index().tail.has_value());
            CHECK(light->index().scopes.empty());
            CHECK(light->index().copies.empty());
            auto full = DataFileReader::open(path);
            REQUIRE(full.has_value());
            CHECK_FALSE(full->index().scopes.empty());
            write_file(cut_dir.file(entry.path().filename().string()),
                       without_footer(read_file(path)));
        }

        const auto dump = [&](const std::string& root) {
            auto snapshot = Snapshot::open(root);
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->skipped().empty());
            std::string text;
            auto generations = snapshot->generations();
            REQUIRE(generations.has_value());
            for (const engine::ScopeGeneration& generation : *generations) {
                text += "generation " + to_std(generation.namespace_name) +
                        " " + to_std(generation.scope.paths[0]) + " " +
                        std::to_string(generation.generation) + "\n";
            }
            for (const ScopeKey& scope : scopes) {
                text += "state " + to_std(scope.paths[0]) + " " +
                        std::to_string(
                            static_cast<int>(snapshot->state(spec, scope))) +
                        "\n";
            }
            auto cursor = snapshot->scan_all(spec, ByteView{}, ByteView{});
            for (const ScopedRow& row : collect_scoped(cursor)) {
                text += row.scope + " " + row.key + "=" + row.value + "\n";
            }
            return text;
        };
        const std::string closed = dump(dir.str());
        CHECK(closed == dump(cut_dir.str()));
        CHECK(closed.find("/traces/m1.pfw many-" + key_of(149)) !=
              std::string::npos);
        CHECK(closed.find("/traces/m1.pfw many-big=" +
                          std::string(3 * SMALL_PAGE, 'b')) !=
              std::string::npos);
        // check() sees the same commits, pages and sections either way.
        const auto checked = [](const std::string& root) {
            auto snapshot = Snapshot::open(root);
            REQUIRE(snapshot.has_value());
            auto report = snapshot->check();
            REQUIRE(report.has_value());
            CHECK(report->bad_commits.empty());
            CHECK(report->bad_sections.empty());
            return std::to_string(report->commits) + " " +
                   std::to_string(report->blocks) + " " +
                   std::to_string(report->sections);
        };
        CHECK(checked(dir.str()) == checked(cut_dir.str()));
        // The newer copy of a, no b, c without its dropped range, and d.
        CHECK(closed.find("/traces/a.pfw key-" + key_of(0) + "=second-") !=
              std::string::npos);
        CHECK(closed.find("/traces/b.pfw key-") == std::string::npos);
        CHECK(closed.find("/traces/c.pfw key-" + key_of(15)) ==
              std::string::npos);
        CHECK(closed.find("/traces/c.pfw key-" + key_of(25)) !=
              std::string::npos);
        CHECK(closed.find("/traces/d.pfw key-" + key_of(0)) !=
              std::string::npos);
    }

    TEST_CASE("a bad commit decides over one that cannot be read") {
        dftu_utils_test::ScopedTestDir dir("snapshot_bad_over_failed");
        dftu_utils_test::ScopedTestDir copy_dir(
            "snapshot_bad_over_failed_copy");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const ScopeKey both = file_scope("/traces/both.pfw");
        const ScopeKey late = file_scope("/traces/late.pfw");
        {
            // `both` has a section in each of two commits, and `late` one in
            // the second only.
            auto writer = make_writer(dir);
            auto id = writer->add_namespace(spec);
            REQUIRE(id.has_value());
            const auto put = [&](const ScopeKey& scope,
                                 const std::string& tag) {
                Batch entries = writer->batch(*id, scope, 0);
                for (int k = 0; k < 100; ++k) {
                    const std::string key = tag + "-" + key_of(k);
                    const std::string value = value_of(k);
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
        const std::string path =
            std::filesystem::directory_iterator(dir.str())->path().string();
        auto reader = DataFileReader::open(path);
        REQUIRE(reader.has_value());
        const auto& refs = page_refs_of(*reader);
        // Two commits, each with its own pages.
        const auto newest = std::max_element(refs.begin(), refs.end(),
                                             [](const auto& a, const auto& b) {
                                                 return a.sequence < b.sequence;
                                             });
        const auto oldest = std::min_element(refs.begin(), refs.end(),
                                             [](const auto& a, const auto& b) {
                                                 return a.sequence < b.sequence;
                                             });
        REQUIRE(oldest->sequence < newest->sequence);
        const engine::PageRefItem newer = newest->ref;

        // The last byte of the newer commit's page is padding no section
        // covers, so only its crc sees the change.
        std::vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(newer.offset + newer.length <= bytes.size());
        bytes[static_cast<std::size_t>(newer.offset) + newer.length - 1U] ^=
            0xFFU;
        const std::string copy_path = copy_dir.file("w-bad-over-failed.dfs");
        write_file(copy_path, without_footer(bytes));

        ScopedDefaultRuntime runtime(1);
        auto snapshot = Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        // The newer commit reads Bad through `late`, before any read fails.
        CHECK(snapshot->state(spec, late) == ScopeState::Bad);
        CHECK(snapshot->read_stats().commits_verified == 1);

        const int data_fd = descriptor_of(copy_path);
        REQUIRE(data_fd >= 0);
        const int saved_fd = ::dup(data_fd);
        REQUIRE(saved_fd >= 0);
        const int dir_fd = ::open(copy_dir.str().c_str(), O_RDONLY);
        REQUIRE(dir_fd >= 0);
        REQUIRE(::dup2(dir_fd, data_fd) == data_fd);

        // The older commit of `both` now fails to read, and its newer one is
        // Bad: the copy is unreadable whatever else failed, so the query finds
        // nothing and reports no error.
        const std::string key_text = "b0-key-0000";
        auto found = snapshot->get(spec, both, view_of(key_text));
        REQUIRE(found.has_value());
        CHECK_FALSE(found->has_value());
        auto cursor = snapshot->scan(spec, both, ByteView{}, ByteView{});
        CHECK(cursor.at_end());
        CHECK_FALSE(cursor.error().has_value());
        CHECK(snapshot->state(spec, both) == ScopeState::Bad);

        REQUIRE(::dup2(saved_fd, data_fd) == data_fd);
        ::close(saved_fd);
        ::close(dir_fd);
    }

    TEST_CASE("a cut past a run drops the pages it leaves behind") {
        dftu_utils_test::ScopedTestDir dir("snapshot_open_cut");
        dftu_utils_test::ScopedTestDir cut_dir("snapshot_open_cut_copy");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const std::vector<ScopeKey> scopes = {file_scope("/traces/a.pfw"),
                                              file_scope("/traces/b.pfw"),
                                              file_scope("/traces/c.pfw")};
        write_scopes_file(dir, spec, scopes, 1000, true);
        const std::string path =
            std::filesystem::directory_iterator(dir.str())->path().string();
        auto reader = DataFileReader::open(path);
        REQUIRE(reader.has_value());
        REQUIRE(page_refs_of(*reader).size() == 3);
        const engine::PageRefItem middle = page_refs_of(*reader)[1].ref;

        std::vector<std::uint8_t> bytes = read_file(path);
        const std::size_t cut =
            static_cast<std::size_t>(middle.offset) + SMALL_PAGE;
        REQUIRE(cut < bytes.size());
        write_file(cut_dir.file("w-cut.dfs"),
                   std::vector<std::uint8_t>(
                       bytes.begin(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(cut)));

        const auto check = [&](std::size_t threads) {
            ScopedDefaultRuntime runtime(threads);
            auto snapshot = Snapshot::open(cut_dir.str());
            REQUIRE(snapshot.has_value());
            // A page cut in two and the records after it are gone, so the
            // scopes they name are absent rather than bad, and the copy the
            // file still names is whole.
            CHECK(snapshot->skipped().empty());
            CHECK(snapshot->state(spec, scopes[0]) == ScopeState::Ready);
            CHECK(snapshot->state(spec, scopes[1]) == ScopeState::Absent);
            CHECK(snapshot->state(spec, scopes[2]) == ScopeState::Absent);
            auto found = snapshot->get(spec, scopes[0],
                                       view_of(std::string("b0-key-0999")));
            REQUIRE(found.has_value());
            REQUIRE(found->has_value());
            CHECK(to_std((*found)->bytes()) == "value-999");
            CHECK(snapshot->open_stats().files == 1);
            CHECK(snapshot->open_stats().pages == 1);
            // One commit, read once, by the state and the query that need it.
            CHECK(snapshot->read_stats().commits_verified == 1);
            CHECK(snapshot->read_stats().read_calls == 1);
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
        const std::string copied =
            std::filesystem::directory_iterator(cut_dir.str())->path().string();
        std::vector<std::uint8_t> bytes = read_file(copied);
        REQUIRE(bytes.size() > 2 * SMALL_PAGE);
        // Without a group commit per batch every record sits after the pages,
        // so a cut in the pages leaves no index to recover.
        write_file(dir.file("w-cut.dfs"),
                   std::vector<std::uint8_t>(
                       bytes.begin(), bytes.end() - static_cast<std::ptrdiff_t>(
                                                        bytes.size() / 3U)));
        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->skipped().size() == 1);
        CHECK(to_std(snapshot->skipped()[0].name) == "w-cut.dfs");
        CHECK(snapshot->open_stats().files == 0);
        CHECK(snapshot->open_stats().pages == 0);
        CHECK(snapshot->state(spec, scope) == ScopeState::Absent);
    }

    TEST_CASE(
        "an open reads no block and a query reads only the page it needs") {
        dftu_utils_test::ScopedTestDir dir("snapshot_lazy_open");
        const ScopeKey first = file_scope("/traces/a.pfw");
        const ScopeKey second = file_scope("/traces/b.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        write_scoped_file(dir, spec, first, 3, 500, true);
        write_scoped_file(dir, spec, second, 2, 500, true);

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        // Two files open, and not one block of them read.
        REQUIRE(snapshot->open_stats().files == 2);
        const engine::ReadStats cold = snapshot->read_stats();
        CHECK(cold.commits_verified == 0);
        CHECK(cold.blocks_read == 0);
        CHECK(cold.read_calls == 0);
        CHECK(cold.bytes_read == 0);

        auto found =
            snapshot->get(spec, second, view_of(std::string("b0-key-0000")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == value_of(0));
        // The files closed, so their commits are synced: the get reads the
        // directory block of each file to find the scope, the section block of
        // the one commit whose range holds the key, then the one page that
        // holds it.
        const engine::ReadStats touched = snapshot->read_stats();
        CHECK(touched.commits_verified == 0);
        CHECK(touched.indexes_read == 0);
        CHECK(touched.metadata_blocks_read == 3);
        CHECK(touched.blocks_read == 1);
        CHECK(touched.read_calls == 4);
        CHECK(touched.bytes_read > 0);

        auto other =
            snapshot->get(spec, first, view_of(std::string("b0-key-0499")));
        REQUIRE(other.has_value());
        REQUIRE(other->has_value());
        CHECK(to_std((*other)->bytes()) == value_of(499));
        const engine::ReadStats both = snapshot->read_stats();
        CHECK(both.commits_verified == 0);
        CHECK(both.indexes_read == 0);
        CHECK(both.metadata_blocks_read == 4);
        CHECK(both.blocks_read == 2);
        CHECK(both.read_calls == 6);
        // A closed file holds no commit object and lists no page ref.
        CHECK(snapshot->open_stats().commits == 0);
        CHECK(snapshot->open_stats().pages == 0);
    }

    TEST_CASE(
        "a corrupt commit stops only the scopes that hold a section of it") {
        dftu_utils_test::ScopedTestDir dir("snapshot_lazy_bad");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_lazy_bad_copy");
        const ScopeKey broken = file_scope("/traces/broken.pfw");
        const ScopeKey shared = file_scope("/traces/shared.pfw");
        const ScopeKey sound = file_scope("/traces/sound.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");

        // One page holds the section of `broken` and the section of `shared`,
        // and the commit that names it is the one the corrupt byte hits. The
        // next page holds the section of `sound` alone.
        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        for (const ScopeKey& scope : {broken, shared}) {
            Batch entries = writer->batch(*id, scope, 0);
            const std::string key =
                to_std(scope.paths[0]) == "/traces/broken.pfw" ? "broken-1"
                                                               : "shared-1";
            entries.put(view_of(key), view_of(std::string("value-1")));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        REQUIRE(flush_writer(*writer).has_value());
        {
            Batch entries = writer->batch(*id, sound, 0);
            entries.put(view_of(std::string("sound-1")),
                        view_of(std::string("sound-value-1")));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());

        auto reader = DataFileReader::open(to_std(writer->path()));
        REQUIRE(reader.has_value());
        REQUIRE(page_refs_of(*reader).size() == 2);
        const engine::PageRef dead{page_refs_of(*reader)[0].ref.offset,
                                   page_refs_of(*reader)[0].ref.length,
                                   page_refs_of(*reader)[0].ref.crc};

        std::vector<std::uint8_t> bytes = read_file(to_std(writer->path()));
        REQUIRE(dead.offset + dead.length <= bytes.size());
        bytes[static_cast<std::size_t>(dead.offset) + 4U] ^= 0xFFU;
        write_file(copy_dir.file("w-bad.dfs"), without_footer(bytes));

        auto snapshot = Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->state(spec, broken) == ScopeState::Bad);
        CHECK(snapshot->state(spec, shared) == ScopeState::Bad);
        CHECK(snapshot->read_stats().commits_verified == 1);

        // The good scope is read on its own, and the bad commit is not touched
        // by the query that does not need it.
        auto visible =
            snapshot->get(spec, sound, view_of(std::string("sound-1")));
        REQUIRE(visible.has_value());
        REQUIRE(visible->has_value());
        CHECK(to_std((*visible)->bytes()) == "sound-value-1");
        const engine::ReadStats after = snapshot->read_stats();
        CHECK(after.commits_verified == 2);
        CHECK(after.blocks_read == 2);

        for (const ScopeKey& scope : {broken, shared}) {
            auto hidden =
                snapshot->get(spec, scope, view_of(std::string("broken-1")));
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
        const std::string big(2 * 1024 * 1024, 'x');

        auto writer = make_writer(dir);
        auto id = writer->add_namespace(spec);
        REQUIRE(id.has_value());
        {
            Batch entries = writer->batch(*id, big_scope, 0);
            entries.put(view_of(std::string("big")), view_of(big));
            REQUIRE(writer->commit(std::move(entries)).has_value());
        }
        REQUIRE(close_writer(*writer).has_value());
        // The sound scope is a file of its own, so its commit is not the one
        // the big value run belongs to.
        write_scoped_file(copy_dir, spec, sound_scope, 1, 20, true);

        auto reader = DataFileReader::open(to_std(writer->path()));
        REQUIRE(reader.has_value());
        // The big value is a run of its own, the one page ref with no sections.
        bool found_run = false;
        engine::PageRef run{};
        for (const engine::IndexedPageRef& page : page_refs_of(*reader)) {
            if (!page.ref.sections.empty()) continue;
            REQUIRE_FALSE(found_run);
            run = {page.ref.offset, page.ref.length, page.ref.crc};
            found_run = true;
        }
        REQUIRE(found_run);
        CHECK(run.length >= big.size());

        std::vector<std::uint8_t> bytes = read_file(to_std(writer->path()));
        REQUIRE(run.offset + run.length <= bytes.size());
        std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(run.offset),
                  bytes.begin() +
                      static_cast<std::ptrdiff_t>(run.offset + run.length),
                  std::uint8_t{0});
        write_file(copy_dir.file("w-big.dfs"), without_footer(bytes));

        auto snapshot = Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        // Nothing of the run was read at open.
        CHECK(snapshot->read_stats().blocks_read == 0);
        CHECK(snapshot->state(spec, big_scope) == ScopeState::Bad);
        auto hidden =
            snapshot->get(spec, big_scope, view_of(std::string("big")));
        REQUIRE(hidden.has_value());
        CHECK_FALSE(hidden->has_value());

        // The other commit of the file is untouched by it.
        CHECK(snapshot->state(spec, sound_scope) == ScopeState::Ready);
        auto visible = snapshot->get(spec, sound_scope,
                                     view_of(std::string("b0-key-0000")));
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
        std::vector<ScopeKey> scopes;
        for (int file = 0; file < FILES; ++file) {
            scopes.push_back(
                file_scope("/traces/t" + std::to_string(file) + ".pfw"));
        }
        for (const ScopeKey& scope : scopes) {
            write_scoped_file(dir, spec, scope, BATCHES, KEYS, true);
        }

        // The same root as written, and again cut before every footer, as a
        // crash before the closes leaves it.
        dftu_utils_test::ScopedTestDir cut_dir("snapshot_lazy_threads_cut");
        for (const auto& entry :
             std::filesystem::directory_iterator(dir.str())) {
            write_file(cut_dir.file(entry.path().filename().string()),
                       without_footer(read_file(entry.path().string())));
        }
        const auto touch_all = [&](const std::string& root, bool synced) {
            auto opened = Snapshot::open(root);
            REQUIRE(opened.has_value());
            const Snapshot& snapshot = *opened;
            CHECK(snapshot.open_stats().files == FILES);
            CHECK(snapshot.read_stats().blocks_read == 0);

            std::atomic<int> failures{0};
            std::vector<std::thread> threads;
            for (int thread = 0; thread < 16; ++thread) {
                threads.emplace_back([&snapshot, &scopes, &spec, &failures,
                                      thread] {
                    const ScopeKey& scope =
                        scopes[static_cast<std::size_t>(thread) %
                               scopes.size()];
                    for (int batch = 0; batch < BATCHES; ++batch) {
                        for (int k = 0; k < KEYS; ++k) {
                            const std::string key =
                                "b" + std::to_string(batch) + "-" + key_of(k);
                            auto found =
                                snapshot.get(spec, scope, view_of(key));
                            if (!found.has_value() || !found->has_value() ||
                                to_std((*found)->bytes()) != value_of(k)) {
                                failures.fetch_add(1);
                            }
                        }
                    }
                    auto cursor =
                        snapshot.scan(spec, scope, ByteView{}, ByteView{});
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
            for (std::thread& thread : threads) thread.join();
            CHECK(failures.load() == 0);

            // Each page of a synced root, and each commit of a cut one, was
            // read exactly once, whatever the order the threads reached it in.
            const engine::ReadStats reads = snapshot.read_stats();
            CHECK(reads.commits_verified ==
                  (synced ? 0U : static_cast<std::uint64_t>(FILES * BATCHES)));
            auto report = snapshot.check();
            REQUIRE(report.has_value());
            CHECK(reads.blocks_read == report->blocks);
        };
        touch_all(dir.str(), true);
        touch_all(cut_dir.str(), false);
    }

    TEST_CASE("check reports every commit and section that does not verify") {
        dftu_utils_test::ScopedTestDir built("snapshot_check_built");
        const ScopeKey scope = file_scope("/traces/check.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const std::string path = built.file("w-check.dfs");

        // A section of two entries of equal size, with the two of them swapped:
        // every offset, length and count in it stays valid, its first key in
        // the index no longer matches its first entry, and its keys descend.
        // The page crc is the one the writer computes over those bytes.
        engine::PageRefItem listed{};
        std::uint32_t offset_in_page = 0;
        {
            engine::SectionBuilder builder;
            builder.add(view_of(std::string("k1")), view_of(std::string("v1")));
            builder.add(view_of(std::string("k2")), view_of(std::string("v2")));
            auto one = builder.seal(nullptr, nullptr, nullptr);
            REQUIRE(one.has_value());
            dstd::Vector<std::uint8_t> section = std::move(*one);

            constexpr std::size_t HEADER = 20;
            constexpr std::size_t SUBPAGE_INDEX = 8 + 1 + 2;
            constexpr std::size_t ENTRY = 6;
            REQUIRE(section.size() >= HEADER + SUBPAGE_INDEX + 2 * ENTRY);
            std::rotate(section.begin() + HEADER + SUBPAGE_INDEX,
                        section.begin() + HEADER + SUBPAGE_INDEX + ENTRY,
                        section.begin() + HEADER + SUBPAGE_INDEX + 2 * ENTRY);

            auto file = engine::DataFileWriter::create(path, SMALL_PAGE, "host",
                                                       "label");
            REQUIRE(file.has_value());
            engine::NamespaceItem namespaces;
            namespaces.local_id = 1;
            namespaces.name = dstring("dftu.test.put");
            namespaces.format_version = 0;
            file->add(std::move(namespaces));
            engine::ScopeItem scope_item;
            scope_item.local_id = 1;
            scope_item.kind = engine::ScopeKind::File;
            engine::ScopePath entry;
            entry.logical_path = dstring("/traces/check.pfw");
            scope_item.paths.push_back(std::move(entry));
            file->add(std::move(scope_item));
            engine::CopyItem copy;
            copy.local_id = 1;
            copy.namespace_id = 1;
            copy.scope_id = 1;
            copy.generation = 1;
            copy.state = engine::CopyState::Ready;
            file->add(std::move(copy));

            auto reference = file->append_page(view_of(section));
            REQUIRE(reference.has_value());
            listed.offset = reference->offset;
            listed.length = reference->length;
            listed.crc = reference->crc;
            listed.sequence = 1;
            engine::PageSection page_section;
            page_section.copy_id = 1;
            page_section.offset_in_page = 0;
            page_section.length = static_cast<std::uint32_t>(section.size());
            page_section.entry_count = 2;
            page_section.first_key = bytes_vector("k1");
            page_section.last_key = bytes_vector("k2");
            listed.sections.push_back(std::move(page_section));
            file->add(listed);
            REQUIRE(file->commit().has_value());
            REQUIRE(file->close().has_value());
            offset_in_page = 0;
        }

        auto snapshot = Snapshot::open(built.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->read_stats().blocks_read == 0);

        // The commit verifies: its page crc is right and its section index is
        // well formed. A lookup through the index does not reject it either.
        auto found = snapshot->get(spec, scope, view_of(std::string("k2")));
        REQUIRE(found.has_value());
        REQUIRE(found->has_value());
        CHECK(to_std((*found)->bytes()) == "v2");
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);

        const engine::ReadStats before = snapshot->read_stats();
        auto report = snapshot->check();
        REQUIRE(report.has_value());
        CHECK(report->files == 1);
        CHECK(report->commits == 1);
        CHECK(report->blocks == 1);
        CHECK(report->bad_commits.empty());
        CHECK(report->sections == 1);
        REQUIRE(report->bad_sections.size() == 1);
        CHECK(to_std(report->bad_sections[0].file) == "w-check.dfs");
        CHECK(report->bad_sections[0].page_offset == listed.offset);
        CHECK(report->bad_sections[0].offset_in_page == offset_in_page);
        // The page the get read is not read again.
        CHECK(snapshot->read_stats().blocks_read == before.blocks_read);
        // The walk does not change what the commit already read.
        CHECK(snapshot->state(spec, scope) == ScopeState::Ready);
    }

    TEST_CASE("check reads every block and reports the commit that fails") {
        dftu_utils_test::ScopedTestDir dir("snapshot_check_bad");
        dftu_utils_test::ScopedTestDir copy_dir("snapshot_check_bad_copy");
        const ScopeKey scope = file_scope("/traces/a.pfw");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        write_scoped_file(dir, spec, scope, 3, 20, true);

        const std::string path =
            std::filesystem::directory_iterator(dir.str())->path().string();
        auto reader = DataFileReader::open(path);
        REQUIRE(reader.has_value());
        REQUIRE(page_refs_of(*reader).size() == 3);
        // The middle page of the file is the one that does not verify.
        const engine::PageRef dead{page_refs_of(*reader)[1].ref.offset,
                                   page_refs_of(*reader)[1].ref.length,
                                   page_refs_of(*reader)[1].ref.crc};

        std::vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(dead.offset + dead.length <= bytes.size());
        bytes[static_cast<std::size_t>(dead.offset) + 8U] ^= 0x01U;
        write_file(copy_dir.file("w-check-bad.dfs"), without_footer(bytes));

        auto snapshot = Snapshot::open(copy_dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->read_stats().blocks_read == 0);

        auto report = snapshot->check();
        REQUIRE(report.has_value());
        CHECK(report->files == 1);
        CHECK(report->commits == 3);
        CHECK(report->blocks == 3);
        // The two page refs that do verify hold a walkable section each; the
        // one that does not is reported as the commit that fails, not as a
        // section, since its bytes are not there to walk.
        CHECK(report->sections == 2);
        CHECK(report->bad_sections.empty());
        REQUIRE(report->bad_commits.size() == 1);
        CHECK(to_std(report->bad_commits[0].file) == "w-check-bad.dfs");
        CHECK(report->bad_commits[0].sequence ==
              page_refs_of(*reader)[1].sequence);
        CHECK(report->bytes_read > 0);
        // The check leaves the state of every commit it read as it found it.
        CHECK(snapshot->state(spec, scope) == ScopeState::Bad);
    }

    TEST_CASE("a directory too large for the footer reads through its levels") {
        dftu_utils_test::ScopedTestDir dir("snapshot_directory_levels");
        const NamespaceSpec spec = put_spec("dftu.test.put");
        const std::string path = dir.file("w-levels.dfs");
        // Long paths make each separator of the top table long, so a few
        // thousand scopes fill more than one top table.
        constexpr std::uint32_t SCOPES = 12000;
        const auto scope_path = [](std::uint32_t index) {
            std::string digits = std::to_string(index);
            return "/traces/" + std::string(1000, 'p') + "/s" +
                   std::string(7 - digits.size(), '0') + digits + ".pfw";
        };

        // Copies without a commit: the directory lists every scope, and no
        // page is written.
        {
            auto file = engine::DataFileWriter::create(path, SMALL_PAGE, "host",
                                                       "label");
            REQUIRE(file.has_value());
            engine::NamespaceItem namespaces;
            namespaces.local_id = 1;
            namespaces.name = dstring("dftu.test.put");
            file->add(std::move(namespaces));
            for (std::uint32_t i = 0; i < SCOPES; ++i) {
                engine::ScopeItem scope_item;
                scope_item.local_id = i + 1;
                scope_item.kind = engine::ScopeKind::File;
                engine::ScopePath entry;
                entry.logical_path = dstring(scope_path(i));
                scope_item.paths.push_back(std::move(entry));
                file->add(std::move(scope_item));
                engine::CopyItem copy;
                copy.local_id = i + 1;
                copy.namespace_id = 1;
                copy.scope_id = i + 1;
                copy.generation = i + 1;
                copy.state = engine::CopyState::Ready;
                file->add(std::move(copy));
            }
            REQUIRE(file->commit().has_value());
            REQUIRE(file->close().has_value());
        }
        auto light = DataFileReader::open(path, engine::FooterItems::Directory);
        REQUIRE(light.has_value());
        REQUIRE(light->index().tail.has_value());
        const std::uint32_t levels = light->index().tail->directory_levels;
        REQUIRE(levels >= 2);
        CHECK(light->index().tail->top_table.size() <=
              engine::DIRECTORY_TOP_BYTES);

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->read_stats().read_calls == 0);
        // One scope reads one block per level, and nothing else.
        CHECK(snapshot->state(spec, file_scope(scope_path(SCOPES / 2))) ==
              ScopeState::Ready);
        CHECK(snapshot->read_stats().metadata_blocks_read == levels);
        CHECK(snapshot->read_stats().blocks_read == 0);
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
        const dstd::Vector<engine::ScopeGeneration>& all = *listed;
        REQUIRE(all.size() == SCOPES);
        CHECK(to_std(all.front().scope.paths[0]) == scope_path(0));
        CHECK(all.front().generation == 1);
        CHECK(to_std(all.back().scope.paths[0]) == scope_path(SCOPES - 1));
        CHECK(all.back().generation == SCOPES);
    }
}
