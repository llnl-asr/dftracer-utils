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
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
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

void rewrite_header_crc(std::vector<std::uint8_t>& bytes) {
    REQUIRE(bytes.size() >= engine::FILE_HEADER_BYTES);
    dftracer::utils::write_u32_le(bytes.data() + HEADER_CRC_AT, 0);
    const std::uint32_t crc =
        dftracer::utils::utilities::fileio::compress::gzip_crc32(
            0, bytes.data(), engine::FILE_HEADER_BYTES);
    dftracer::utils::write_u32_le(bytes.data() + HEADER_CRC_AT, crc);
}

/// Copies `source` into `target` under `name`, with the header writer id and
/// the header crc rewritten so the copy verifies.
std::string forge_copy(const std::string& source,
                       const dftu_utils_test::ScopedTestDir& target,
                       const std::string& name, std::uint64_t writer_random) {
    std::vector<std::uint8_t> bytes = read_file(source);
    REQUIRE(bytes.size() > engine::FILE_HEADER_BYTES);
    dftracer::utils::write_u64_le(bytes.data() + HEADER_WRITER_RANDOM_AT,
                                  writer_random);
    rewrite_header_crc(bytes);
    const std::string path = target.file(name);
    write_file(path, bytes);
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
    const dstd::Vector<engine::ScopeGeneration> all = snapshot.generations();
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
/// so two opens under different runtimes compare with one string.
struct OpenDump {
    std::string text;
    std::size_t files = 0;
    std::size_t pages = 0;
    std::uint64_t read_calls = 0;
};

OpenDump dump_open(const dftu_utils_test::ScopedTestDir& root,
                   const NamespaceSpec& spec,
                   const std::vector<ScopeKey>& scopes) {
    auto snapshot = Snapshot::open(root.str());
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->skipped().empty());
    OpenDump dump;
    dump.files = snapshot->open_stats().files;
    dump.pages = snapshot->open_stats().pages;
    dump.read_calls = snapshot->open_stats().read_calls;
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
    return dump;
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
        for (const engine::IndexedPageRef& page : reader->index().page_refs) {
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

        // The big value run is a block of its commit: when it does not
        // verify, the whole commit reads as bad, so neither key is returned.
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
        REQUIRE(reader->index().page_refs.size() == 1);
        const engine::PageRefItem& run = reader->index().page_refs[0].ref;
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
        REQUIRE(reader->index().page_refs.size() == 2);
        engine::PageRef broken_page{};
        broken_page.offset = reader->index().page_refs[0].ref.offset;
        broken_page.length = reader->index().page_refs[0].ref.length;
        broken_page.crc = reader->index().page_refs[0].ref.crc;

        std::vector<std::uint8_t> bytes = read_file(to_std(writer->path()));
        REQUIRE(broken_page.offset + broken_page.length <= bytes.size());
        bytes[static_cast<std::size_t>(broken_page.offset) + 4U] ^= 0xFFU;
        const std::string copy_path = copy_dir.file("w-bad.dfs");
        write_file(copy_path, bytes);

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

        std::vector<std::uint8_t> bytes = read_file(to_std(writer->path()));
        dftracer::utils::write_u32_le(bytes.data() + 8, 1);
        rewrite_header_crc(bytes);
        write_file(dir.file("w-version-one.dfs"), bytes);

        auto snapshot = Snapshot::open(dir.str());
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->skipped().size() == 1);
        CHECK(to_std(snapshot->skipped()[0].name) == "w-version-one.dfs");
        CHECK(snapshot->skipped()[0].error.is(Condition::Unsupported));
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
        write_file(corrupt.file("w-bad.dfs"), bytes);

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
            REQUIRE(reader->index().page_refs.size() == std::size(lengths));
            for (const engine::IndexedPageRef& page :
                 reader->index().page_refs) {
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
        REQUIRE(reader->index().page_refs.size() == 1);
        REQUIRE(reader->index().page_refs[0].ref.sections.size() == 1);
        CHECK(reader->index().page_refs[0].ref.sections[0].entry_count ==
              BIG_COUNT + 1);
        CHECK(reader->index().page_refs[0].ref.length == PAGE);

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
        // A file written without a group commit per batch holds adjacent page
        // refs, one run per batch, so the read calls of the whole root stay
        // well under its page count.
        CHECK(one.pages >= WRITERS * BATCHES);
        CHECK(one.pages == eight.pages);
        CHECK(eight.read_calls == one.read_calls);
        CHECK(eight.read_calls < eight.pages);
        CHECK(eight.read_calls * 4 <= eight.pages);
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
        REQUIRE(reader->index().page_refs.size() == 3);
        // One group commit per scope, so the middle reference is one commit's
        // pages, read in one preadv and crc-checked as one block.
        const engine::PageRefItem middle = reader->index().page_refs[1].ref;
        CHECK(middle.length > SMALL_PAGE);
        CHECK(middle.length % SMALL_PAGE == 0);

        std::vector<std::uint8_t> bytes = read_file(path);
        REQUIRE(middle.offset + middle.length <= bytes.size());
        // The last byte of the reference is padding no section covers, so only
        // the block crc sees it.
        bytes[static_cast<std::size_t>(middle.offset) + middle.length - 1U] ^=
            0xFFU;
        write_file(copy_dir.file("w-corrupt.dfs"), bytes);

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
            CHECK(snapshot->open_stats().read_calls == 3);
        };
        check(1);
        check(8);
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
        REQUIRE(reader->index().page_refs.size() == 3);
        const engine::PageRefItem middle = reader->index().page_refs[1].ref;

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
            CHECK(snapshot->open_stats().read_calls == 1);
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
}
