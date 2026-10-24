// The index fold walks each record's top-level object once and each args
// object once, and the stored index does not change. The cases count walks,
// not time.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/parse_walks.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/parse_inflated.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <rocksdb/iterator.h>
#include <sys/stat.h>
#include <testing_utilities.h>

#include <cstdint>
#include <ctime>
#include <iterator>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace store = dftracer::utils::index::store;
namespace walks = dftracer::utils::trace::internal::parse_walks;

namespace {

constexpr int RECORDS = 4000;
constexpr int SHAPES = 8;
constexpr std::time_t FIXED_MTIME = 1700000000;

// Schema detection samples the head of the file, so the records that do not
// look like dftracer events come after it.
constexpr int HEAD_RECORDS = 1000;

int shape_of(int i) { return i < HEAD_RECORDS ? i % 3 : i % SHAPES; }

// Shapes 0, 2, 3, 5 and 6 carry an args object, which may be empty. Shape 7
// has no phase: it is walked once and then dropped.
bool has_args_object(int shape) {
    return shape == 0 || shape == 2 || shape == 3 || shape == 5 || shape == 6;
}

std::string write_trace(const std::string& dir) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < RECORDS; ++i) {
        const int pid = 1 + i % 4;
        const int ts = 1000 + i * 10;
        out << R"({"id":)" << i;
        switch (shape_of(i)) {
            case 0:
                out << R"(,"name":"read","cat":"POSIX","pid":)" << pid
                    << R"(,"tid":1,"ph":"X","ts":)" << ts
                    << R"(,"dur":3,"args":{"fname":"/data/f)" << i % 37
                    << R"(","size":)" << i % 4096
                    << R"(,"fhash":"f00d","hhash":"a1b2c3d4e5f60718","level":)"
                    << i % 3 << "}}\n";
                break;
            case 1:
                out << R"(,"name":"compute","cat":"APP","pid":)" << pid
                    << R"(,"tid":2,"ph":"X","ts":)" << ts << R"(,"dur":7})"
                    << "\n";
                break;
            case 2:
                out << R"(,"name":"write","cat":"POSIX","pid":)" << pid
                    << R"(,"tid":1,"ph":"X","ts":)" << ts
                    << R"(,"dur":5,"args":{"io":{"off":)" << i
                    << R"(,"len":512},"hosts":["n1","n2"],"note":null,)"
                    << R"("ratio":0.5}})"
                    << "\n";
                break;
            case 3:
                out << R"(,"name":"open","cat":"POSIX","pid":)" << pid
                    << R"(,"tid":1,"ph":"X","ts":)" << ts
                    << R"(,"dur":1,"args":{}})"
                    << "\n";
                break;
            case 4:
                out << R"(,"name":"close","cat":"POSIX","pid":)" << pid
                    << R"(,"tid":1,"ph":"X","ts":)" << ts
                    << R"(,"dur":1,"args":7})"
                    << "\n";
                break;
            case 5:
                out << R"(,"name":"stat","cat":"POSIX","pid":)" << pid
                    << R"(,"tid":3,"ph":"X","ts":)" << ts
                    << R"(,"dur":2,"extra":"tag)" << i % 5
                    << R"(","meta":{"k":[1,2],"z":null},"args":{"fname":"/d/s)"
                    << i % 11 << R"(","ret":-)" << i << "}}\n";
                break;
            case 6:
                out << R"(,"name":"seek","cat":"STDIO","pid":"7","tid":1,)"
                    << R"("ph":"X","ts":)" << ts
                    << R"(,"dur":2.5,"args":{"neg":-3,"big":)"
                    << "18446744073709551615"
                    << R"(,"flag":true,"off":false,"ratio":1.25,)"
                    << R"("tag":"x"}})"
                    << "\n";
                break;
            default:
                out << R"(,"name":"nophase","cat":"POSIX","pid":)" << pid
                    << R"(,"ts":)" << ts << "}\n";
        }
    }
    out << "]\n";
    const std::string path =
        dftu_utils_test::write_gz_trace(dir + "/trace.pfw.gz", out.str());
    const timespec fixed[2] = {{FIXED_MTIME, 0}, {FIXED_MTIME, 0}};
    REQUIRE(::utimensat(AT_FDCWD, path.c_str(), fixed, 0) == 0);
    return path;
}

build::IndexBuildBatchResult build_one(const std::string& file,
                                       const std::string& index_dir) {
    Runtime rt(4);
    build::IndexBuildBatchResult result;
    auto task = run_coro_scope(
        rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
            auto config = std::make_shared<build::IndexBuildBatchConfig>();
            config->file_paths = {file};
            config->index_dir = index_dir;
            config->parallelism = 4;
            result = co_await build::BatchBuilder::process(&scope,
                                                           std::move(config));
        });
    rt.submit(std::move(task), "parse-walks").wait();
    rt.shutdown();
    REQUIRE(result.indexed == 1);
    return result;
}

// The digest of each column family of the index write_trace builds, recorded
// from the build before the single-walk parse. A family that holds no rows
// digests to the FNV offset.
struct FamilyDigest {
    std::string_view family;
    std::uint64_t digest;
};
constexpr FamilyDigest RECORDED[] = {
    {"default", 8611890762692540955ULL},
    {"members", 12797188868763278476ULL},
    {"granule", 309768426608778519ULL},
    {"blob", 8561683722829537242ULL},
    {"postings", 10242643526126183152ULL},
    {"system_metrics", 1469598103934665603ULL},
    {"rollup", 1469598103934665603ULL},
};

// FNV-1a of every key and value of one column family. A row that names the
// test directory (the trace's path, whose length varies) is left out.
std::uint64_t family_digest(store::RocksDatabase& db, std::string_view cf,
                            const std::string& dir) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&h](std::string_view bytes) {
        for (unsigned char c : bytes) h = (h ^ c) * 1099511628211ULL;
        h = (h ^ 0xFF) * 1099511628211ULL;
    };
    auto it = db.new_iterator(cf);
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        const std::string_view key = it->key().ToStringView();
        const std::string_view value = it->value().ToStringView();
        if (key.find(dir) != std::string_view::npos ||
            value.find(dir) != std::string_view::npos)
            continue;
        mix(key);
        mix(value);
    }
    REQUIRE(it->status().ok());
    return h;
}

namespace views = dftracer::utils::trace::views::detail;
using dftracer::utils::dstd::StringIntern;

// A FoldEvent with every interned id resolved to its string, so two events
// from different interns compare.
std::string describe(const views::FoldEvent& ev, const StringIntern& in) {
    const auto text = [&](std::uint32_t id) {
        return id == StringIntern::NO_ID ? std::string("<none>")
                                         : std::string(in.resolve(id));
    };
    std::ostringstream o;
    o << "cat=" << text(ev.cat_id) << " name=" << text(ev.name_id)
      << " fhash=" << text(ev.fhash_id) << " hhash=" << text(ev.hhash_id)
      << " pid=" << ev.pid << " tid=" << ev.tid << " ts=" << ev.ts
      << " dur=" << ev.dur << " phase=" << static_cast<int>(ev.phase)
      << " has_dur=" << ev.has_dur << " by_path=" << ev.by_path
      << " unconverted=" << ev.unconverted << "\n";
    const auto list = [&](const char* label, const auto& pairs) {
        o << label << ":";
        for (const auto& [k, v] : pairs) {
            o << " " << text(k) << "=";
            std::visit(
                [&](const auto& x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, std::uint32_t>)
                        o << "s:" << text(x);
                    else
                        o << typeid(T).name() << ":" << x;
                },
                v);
        }
        o << "\n";
    };
    list("args", ev.args);
    list("top", ev.top_fields);
    o << "specials:";
    for (const auto& sp : ev.specials)
        o << " " << text(sp.key) << "/" << static_cast<int>(sp.kind) << "/"
          << sp.top;
    o << "\nleaves:";
    for (const auto& [id, tag] : ev.schema_leaves)
        o << " " << text(id) << "#" << static_cast<int>(tag);
    return o.str();
}

}  // namespace

TEST_SUITE("IndexParseWalks") {
    TEST_CASE("each record and each args object is walked once") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string file = write_trace(env.get_dir());
        std::uint64_t with_args = 0;
        for (int i = 0; i < RECORDS; ++i)
            if (has_args_object(shape_of(i))) ++with_args;

        walks::g_record_walks = 0;
        walks::g_args_walks = 0;
        walks::g_count = true;
        build_one(file, env.get_dir() + "/idx");
        walks::g_count = false;

        MESSAGE("record walks " << walks::g_record_walks.load() << " for "
                                << RECORDS << " records, args walks "
                                << walks::g_args_walks.load() << " for "
                                << with_args << " args objects");
        CHECK(walks::g_record_walks.load() == std::uint64_t{RECORDS});
        CHECK(walks::g_args_walks.load() == with_args);
    }

    TEST_CASE("the index of a fixed trace is unchanged") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string file = write_trace(env.get_dir());
        build_one(file, env.get_dir() + "/idx");
        const std::string path =
            trace::internal::determine_index_path(file, env.get_dir() + "/idx");
        store::RocksDatabase db(path, store::RocksDatabase::OpenMode::ReadOnly);
        REQUIRE(db.is_open());
        std::size_t checked = 0;
        for (const auto cf : store::RocksDatabase::default_column_families()) {
            const std::uint64_t digest = family_digest(db, cf, env.get_dir());
            MESSAGE("family " << std::string(cf) << " " << digest);
            for (const auto& want : RECORDED)
                if (want.family == cf) {
                    CHECK(digest == want.digest);
                    ++checked;
                }
        }
        CHECK(checked == std::size(RECORDED));
    }

    TEST_CASE("a malformed record drops only itself") {
        constexpr int GOOD = 3000;
        const std::string bad[] = {
            R"({"id":1,"name":"cut","cat":"c","pid":1,"tid":1,"ph":"X","ts":5,"args":{"a":)",
            R"({"id":2,"name":"quote","cat":"c","pid":1,"tid":1,"ph":"X","ts":6,"args":{"s":"a"b"}})",
            "not json at all",
            "[1,2,3]",
            "{\"id\":3,\"name\":\"ctl\tchar\",\"cat\":\"c\",\"pid\":1,"
            "\"tid\":1,\"ph\":\"X\",\"ts\":7}",
        };
        std::ostringstream out;
        out << "[\n";
        for (int i = 0; i < GOOD; ++i) {
            out << R"({"id":)" << i
                << R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X",)"
                << R"("ts":)" << 1000 + i << R"(,"dur":2,"args":{"size":)" << i
                << R"(,"huge":123456789012345678901234567890,"neg":-5}})"
                << "\n";
            if (i % 600 == 0)
                for (const auto& line : bad) out << line << "\n";
        }
        out << "]\n";
        dftu_utils_test::TestEnvironment env(10);
        const std::string file = dftu_utils_test::write_gz_trace(
            env.get_dir() + "/bad.pfw.gz", out.str());
        const auto result = build_one(file, env.get_dir() + "/idx");
        REQUIRE(result.results.size() == 1);
        CHECK(result.results[0].events_processed == std::size_t{GOOD});
    }

    TEST_CASE("a record longer than a parse batch is indexed") {
        constexpr int LINES = 40;
        constexpr std::size_t LONG_BYTES = 3 * trace::PARSE_BATCH_BYTES;
        std::ostringstream out;
        out << "[\n";
        for (int i = 0; i < LINES; ++i) {
            out << R"({"id":)" << i
                << R"(,"name":"read","cat":"POSIX","pid":1,"tid":1,"ph":"X",)"
                << R"("ts":)" << 1000 + i << R"(,"dur":2,"args":{"size":)" << i;
            if (i == 20)
                out << R"(,"note":")" << std::string(LONG_BYTES, 'x') << '"';
            out << "}}\n";
        }
        out << "]\n";
        dftu_utils_test::TestEnvironment env(10);
        const std::string file = dftu_utils_test::write_gz_trace(
            env.get_dir() + "/long.pfw.gz", out.str());
        walks::g_record_walks = 0;
        walks::g_count = true;
        const auto result = build_one(file, env.get_dir() + "/idx");
        walks::g_count = false;
        REQUIRE(result.results.size() == 1);
        CHECK(result.results[0].events_processed == std::size_t{LINES});
        CHECK(walks::g_record_walks.load() == std::uint64_t{LINES});
    }

    TEST_CASE("the one-walk decode equals the old three-walk decode") {
        const std::string records[] = {
            R"({"ph":"X","name":"n","cat":"c","pid":1,"tid":2,"ts":3,"dur":4,"args":{"a":1,"b":"x","fhash":"f","hhash":"h"}})",
            R"({"ph":"X","name":"n","args":{"a":1},"cat":"c"})",
            R"({"args":{"a":1},"ph":"X","name":"n","cat":"c","pid":5})",
            R"({"ph":"X","args":{"a":1},"args":{"b":2}})",
            R"({"ph":"X","args":{"a":1},"args":5})",
            R"({"ph":"X","args":5,"args":{"a":1}})",
            R"({"ph":"X","args":{}})",
            R"({"ph":"X","args":{},"x":1})",
            R"({"ph":"X","args":7,"x":[1,{"y":null}]})",
            R"({"ph":"X","args":[1,2,{"z":true}]})",
            R"({"ph":"X","args":"s","name":"n"})",
            R"({"ph":"X","args":{"io":{"off":1,"len":[1,2,{"k":"v"}]},"e":{},"f":[],"n":null,"t":true,"d":0.5,"neg":-3}})",
            R"({"ph":"X","name":"n","name":"m","ts":1,"ts":2,"pid":1,"pid":-1})",
            R"({"name":"n","cat":"c","ts":1,"args":{"a":1}})",
            R"({"ph":1,"ts":-5,"pid":2.5,"tid":"7","dur":1.5})",
            R"({"ph":"X","ts":18446744073709551615,"pid":-1,"dur":9223372036854775808})",
            R"({"ph":"X","cat":5,"name":false,"type":3,"id":9})",
            R"({"ph":"X","args":{"big":18446744073709551615,"huge":123456789012345678901234567890,"i":-9223372036854775808}})",
            R"({"ph":"X","args":{"fhash":5,"hhash":{"a":1}}})",
            R"({"ph":"X","args":{"fhash":"f","k":"v"},"fhash":"top","": 1})",
            R"({"ph":"X","":{"a":1},"a":{"":2}})",
            R"({"ph":null,"name":"n"})",
            R"([1,2])",
        };
        std::size_t compared = 0;
        for (const auto& text : records)
            for (const bool needs_args : {false, true})
                for (const bool capture : {false, true}) {
                    simdjson::dom::parser parser;
                    parser.number_as_string(true);
                    simdjson::dom::element root;
                    const std::string padded =
                        text + std::string(simdjson::SIMDJSON_PADDING, ' ');
                    if (parser.parse(padded.data(), text.size(), false)
                            .get(root))
                        continue;
                    const std::string label =
                        text + " needs_args=" + std::to_string(needs_args) +
                        " capture=" + std::to_string(capture);

                    StringIntern old_intern;
                    views::DecodeHints old_hints;
                    views::FoldEvent old_ev;
                    trace::DFTracerEvent probe;
                    simdjson::dom::element args;
                    bool has_args = false;
                    const bool kept = trace::DFTracerEvent::parse_scalars(
                        root, probe, args, has_args);
                    views::extract_fold_event_into(old_ev, root, old_intern,
                                                   needs_args, nullptr, capture,
                                                   &old_hints);

                    StringIntern new_intern;
                    views::DecodeHints new_hints;
                    views::FoldEventBuffer batch;
                    const bool appended = views::append_dftracer_record(
                        batch, root, new_intern, needs_args, capture, new_hints,
                        nullptr);
                    INFO(label);
                    REQUIRE(appended == kept);
                    if (!kept) {
                        CHECK(batch.empty());
                        continue;
                    }
                    REQUIRE(batch.size() == 1);
                    CHECK(describe(batch.back(), new_intern) ==
                          describe(old_ev, old_intern));
                    ++compared;
                }
        CHECK(compared > 60);
    }
}
