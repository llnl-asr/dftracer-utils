#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/filesystem.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <doctest/doctest.h>
#include <sys/stat.h>
#include <testing_utilities.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace store = dftracer::utils::index::store;
namespace scan = dftracer::utils::utilities::filesystem;
namespace views = dftracer::utils::trace::views;

namespace {

constexpr int SCAN_FILES = 200;
constexpr int NESTED_FILES = 10;
constexpr int INDEX_FILES = 64;
constexpr std::size_t WORKERS = 8;
constexpr int VIEW_FILES = 12;
constexpr std::size_t VIEW_WORKERS = 4;
constexpr std::size_t TINY_SINK = 1;
constexpr std::size_t HUGE_SINK = std::size_t{1} << 40;

void write_sized_file(const std::string& path, std::size_t bytes) {
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');
}

std::string read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string write_small_trace(const std::string& dir, int n) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < 40; ++i) {
        out << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":1,)"
            << R"("tid":1,"ph":"X","ts":)" << 1000 + i * 10
            << R"(,"dur":3,"args":{"fname":"/data/f)" << n << R"("}})"
            << "\n";
    }
    out << "]\n";
    char name[32];
    std::snprintf(name, sizeof(name), "/t%04d.pfw.gz", n);
    return dftu_utils_test::write_gz_trace(dir + name, out.str());
}

std::vector<scan::FileEntry> scan_dir(const std::string& dir) {
    std::vector<scan::FileEntry> entries;
    Runtime rt(4);
    auto task = run_coro_scope(
        rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
            scan::PatternDirectoryScannerUtility scanner;
            const scan::PatternDirectoryScannerUtilityInput input(
                dir, std::vector<std::string>{".pfw"}, true);
            entries = co_await scanner(scope, input);
        });
    rt.submit(std::move(task), "scan-dir").wait();
    rt.shutdown();
    return entries;
}

// The scan as the CLIs run it: from a coroutine that get() drives on the main
// thread, through scan_on_pool, which fans it out over the CLI's pool.
coro::CoroTask<std::vector<scan::FileEntry>> cli_scan_task(std::string dir) {
    auto entries = scan::scan_on_pool<scan::PatternDirectoryScannerUtility>(
        WORKERS, 2,
        scan::PatternDirectoryScannerUtilityInput(
            std::move(dir), std::vector<std::string>{".pfw"}, true, false));
    co_return entries;
}

std::vector<scan::FileEntry> cli_scan(const std::string& dir) {
    return cli_scan_task(dir).get();
}

std::vector<std::string> write_view_traces(const std::string& dir, int count) {
    std::vector<std::string> files;
    for (int i = 0; i < count; ++i) files.push_back(write_small_trace(dir, i));
    return files;
}

std::string write_corrupt_trace(const std::string& dir, int n) {
    char name[32];
    std::snprintf(name, sizeof(name), "/t%04d.pfw.gz", n);
    const auto path = dir + name;
    std::ofstream out(path, std::ios::binary);
    out << std::string(4096, 'x');
    return path;
}

std::string view_index_root(const std::string& file,
                            const std::string& index_dir) {
    return trace::internal::determine_index_path(file, index_dir);
}

// A cold first aggregate of `files`, which bootstraps their index.
void cold_count(const std::vector<std::string>& files,
                const std::string& index_dir) {
    std::vector<views::ViewFile> view_files;
    for (const auto& f : files) {
        views::ViewFile vf;
        vf.file_path = f;
        vf.index_path = view_index_root(f, index_dir);
        view_files.push_back(std::move(vf));
    }
    const auto view = views::View::from_files(std::move(view_files))
                          .group_by({views::GroupKey::name()})
                          .agg({{views::AggOp::Count, "", "n"}});
    default_runtime().run_blocking("serial-stages-view",
                                   [&](CoroScope&) -> coro::CoroTask<void> {
                                       (void)co_await view.collect();
                                   });
}

using Rows = std::vector<std::pair<std::string, std::string>>;

std::map<std::string, Rows> dump_index(const std::string& root) {
    store::RocksDBManager::instance().reset(root);
    store::RocksDatabase db(root, store::RocksDatabase::OpenMode::ReadOnly);
    REQUIRE(db.is_open());
    std::map<std::string, Rows> out;
    for (const auto cf : store::RocksDatabase::default_column_families()) {
        auto it = db.new_iterator(cf);
        auto& rows = out[std::string(cf)];
        for (it->SeekToFirst(); it->Valid(); it->Next())
            rows.emplace_back(it->key().ToString(), it->value().ToString());
        REQUIRE(it->status().ok());
    }
    return out;
}

struct SinkLimit {
    explicit SinkLimit(std::size_t bytes)
        : previous(build::g_max_sink_bytes.exchange(bytes)) {}
    ~SinkLimit() { build::g_max_sink_bytes.store(previous); }
    std::size_t previous;
};

// The traces of the view cases and one cold run of them with a sink that
// commits after every file, built once and shared by the cases that read it.
struct ViewRun {
    ViewRun() : env(10) {
        files = write_view_traces(env.get_dir(), VIEW_FILES);
        many_dir = env.get_dir() + "/many";
        set_default_runtime(std::make_shared<Runtime>(VIEW_WORKERS));
        auto& counters = views::detail::bootstrap_counters();
        counters.reset();
        {
            SinkLimit limit(TINY_SINK);
            cold_count(files, many_dir);
        }
        ingest_calls = counters.ingest_calls.load();
        during_parse = counters.ingests_during_parse.load();
        most_at_once = counters.max_ingests_in_flight.load();
        sink_commits = counters.sink_commits.load();
        set_default_runtime(nullptr);
    }

    dftu_utils_test::TestEnvironment env;
    std::vector<std::string> files;
    std::string many_dir;
    std::size_t ingest_calls = 0;
    std::size_t during_parse = 0;
    std::size_t most_at_once = 0;
    std::size_t sink_commits = 0;
};

ViewRun& view_run() {
    static ViewRun run;
    return run;
}

}  // namespace

TEST_SUITE("SerialStages") {
    TEST_CASE("a directory scan stats its files in parallel batches") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string dir = env.get_dir() + "/scan";
        fs::create_directories(dir + "/sub");
        for (int i = 0; i < SCAN_FILES; ++i)
            write_sized_file(dir + "/f" + std::to_string(i) + ".pfw",
                             static_cast<std::size_t>(i) * 7 + 1);
        for (int i = 0; i < NESTED_FILES; ++i)
            write_sized_file(dir + "/sub/n" + std::to_string(i) + ".pfw",
                             static_cast<std::size_t>(i) + 100);
        write_sized_file(dir + "/skip.txt", 5);

        std::vector<std::string> expected;
        for (const auto& e : fs::directory_iterator(dir))
            if (e.is_regular_file() && e.path().extension() == ".pfw")
                expected.push_back(e.path().string());
        for (const auto& e : fs::directory_iterator(dir + "/sub"))
            expected.push_back(e.path().string());

        const auto before = scan::scan_stat_batches().load();
        const auto entries = scan_dir(dir);
        const auto batches = scan::scan_stat_batches().load() - before;

        REQUIRE(entries.size() == expected.size());
        CHECK(entries.size() == SCAN_FILES + NESTED_FILES);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            CHECK(entries[i].path.string() == expected[i]);
            CHECK(entries[i].is_regular_file);
            CHECK(entries[i].size == fs::file_size(entries[i].path));
            CHECK(entries[i].mtime > 0);
        }
        CHECK(batches > 2);
    }

    TEST_CASE("a CLI scan stats its files on more than one thread") {
        dftu_utils_test::TestEnvironment env(10);
        const std::string dir = env.get_dir() + "/cli_scan";
        fs::create_directories(dir + "/a");
        fs::create_directories(dir + "/b");
        for (int i = 0; i < SCAN_FILES; ++i)
            write_sized_file(
                dir + (i % 2 ? "/a/f" : "/b/f") + std::to_string(i) + ".pfw",
                static_cast<std::size_t>(i) + 1);

        scan::scan_stat_threads().clear();
        // A CLI drives its run coroutine with get() on the main thread, with
        // no executor running there but the main run loop.
        const auto entries = cli_scan(dir);

        CHECK(entries.size() == SCAN_FILES);
        CHECK(scan::scan_stat_threads().count() > 1);
    }

    TEST_CASE("an ingest links its staged SSTs without a reopen and fsync") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < INDEX_FILES; ++i)
            files.push_back(write_small_trace(env.get_dir(), i));
        const auto index_dir = env.get_dir() + "/idx";
        const auto staging = env.get_dir() + "/staging";

        store::SstArtifactRegistry registry;
        build::IndexBuildBatchResult result;
        {
            Runtime rt(WORKERS);
            auto counter = std::make_shared<std::atomic<std::size_t>>(0);
            auto task = run_coro_scope(
                rt.executor(), [&](CoroScope& scope) -> coro::CoroTask<void> {
                    auto config =
                        std::make_shared<build::IndexBuildBatchConfig>();
                    config->file_paths = files;
                    config->index_dir = index_dir;
                    config->parallelism = WORKERS;
                    config->sink_factory = [&staging, counter] {
                        return std::make_unique<
                            store::IndexDatabaseSstWriterContext>(
                            staging,
                            "batch_" + std::to_string(counter->fetch_add(1)));
                    };
                    config->sink_commit = [&registry](store::IndexWrite& sink) {
                        registry.append(
                            static_cast<store::IndexDatabaseSstWriterContext&>(
                                sink)
                                .commit());
                    };
                    result = co_await build::BatchBuilder::process(
                        &scope, std::move(config));
                });
            rt.submit(std::move(task), "serial-stages-build").wait();
            rt.shutdown();
        }
        REQUIRE(result.indexed == INDEX_FILES);

        struct Staged {
            std::string path;
            ino_t inode;
            std::string bytes;
        };
        std::vector<Staged> staged;
        for (std::size_t f = 0; f < store::layout::FAMILY_COUNT; ++f) {
            for (const auto& path :
                 registry.files(static_cast<store::layout::Family>(f))) {
                struct stat st;
                REQUIRE(::stat(path.c_str(), &st) == 0);
                staged.push_back({path, st.st_ino, read_bytes(path)});
            }
        }
        REQUIRE(staged.size() > 1);

        const auto index_path =
            trace::internal::determine_index_path(files[0], index_dir);
        store::IndexDatabase db(index_path);
        db.init_schema();
        const auto skipped_before = store::ingest_reopens_skipped();
        db.bulk_ingest(registry, {});

        std::map<ino_t, std::string> in_db;
        for (const auto& e : fs::recursive_directory_iterator(index_path)) {
            if (e.path().extension() != ".sst") continue;
            struct stat st;
            REQUIRE(::stat(e.path().c_str(), &st) == 0);
            in_db[st.st_ino] = e.path().string();
        }
        for (const auto& s : staged) {
            CHECK_FALSE(fs::exists(s.path));
            const auto it = in_db.find(s.inode);
            REQUIRE(it != in_db.end());
            CHECK(read_bytes(it->second) == s.bytes);
        }
        CHECK(store::ingest_reopens_skipped() - skipped_before ==
              staged.size());
    }
    TEST_CASE("a cold view ingests its sinks while files still parse") {
        const auto& run = view_run();
        MESSAGE("ingests " << run.ingest_calls << ", during parse "
                           << run.during_parse << ", most at once "
                           << run.most_at_once << ", sink commits "
                           << run.sink_commits);
        CHECK(run.ingest_calls > 1);
        CHECK(run.during_parse > 0);
        CHECK(run.most_at_once > 1);
    }

    TEST_CASE("a bootstrap that fails midway marks only complete files") {
        const auto& run = view_run();
        auto files = run.files;
        files.push_back(write_corrupt_trace(run.env.get_dir(), VIEW_FILES));
        const auto index_dir = run.env.get_dir() + "/failed";
        set_default_runtime(std::make_shared<Runtime>(2));
        {
            SinkLimit limit(TINY_SINK);
            try {
                cold_count(files, index_dir);
            } catch (const std::exception&) {
            }
        }
        set_default_runtime(nullptr);

        const auto root = view_index_root(files.front(), index_dir);
        store::RocksDBManager::instance().reset(root);
        store::IndexDatabase db(root, store::IndexOpenMode::ReadOnly);
        std::size_t marked = 0;
        for (const auto& f : files) {
            const int fid = db.get_file_info_id(
                dftracer::utils::index::store::internal::get_logical_path(f));
            if (fid < 0) continue;
            ++marked;
            INFO("file " << f);
            CHECK(db.extension_current(fid, store::IndexExtension::Members));
            CHECK(db.get_num_lines(fid) > 0);
            CHECK_FALSE(db.query_gzip_members(fid).empty());
            CHECK(db.pruning_tier_current(fid));
            CHECK(db.extension_state(fid, store::IndexExtension::Profile)
                      .has_value());
        }
        MESSAGE("marked " << marked << " of " << files.size());
        REQUIRE(marked > 0);
        CHECK(marked < files.size());
        CHECK(db.get_file_info_id(
                  dftracer::utils::index::store::internal::get_logical_path(
                      files.back())) < 0);
    }

    TEST_CASE("many ingests store the index that one ingest stores") {
        const auto& run = view_run();
        const auto one_dir = run.env.get_dir() + "/one";
        set_default_runtime(std::make_shared<Runtime>(VIEW_WORKERS));
        auto& counters = views::detail::bootstrap_counters();
        {
            SinkLimit limit(HUGE_SINK);
            counters.reset();
            cold_count(run.files, one_dir);
        }
        const auto one_ingests = counters.ingest_calls.load();
        set_default_runtime(nullptr);
        REQUIRE(run.ingest_calls > 1);
        REQUIRE(one_ingests < run.ingest_calls);

        const auto many =
            dump_index(view_index_root(run.files.front(), run.many_dir));
        const auto one =
            dump_index(view_index_root(run.files.front(), one_dir));
        REQUIRE(many.size() == one.size());
        for (const auto& [name, rows] : many) {
            INFO("family " << name);
            const auto it = one.find(name);
            REQUIRE(it != one.end());
            REQUIRE(rows.size() == it->second.size());
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i] != it->second[i])
                    FAIL("row " << i << " differs in " << name);
            }
        }
    }
}
