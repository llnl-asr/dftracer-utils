#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/filesystem.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_sst_writer_context.h>
#include <dftracer/utils/index/store/layout.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/filesystem/directory_scanner_utility.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <doctest/doctest.h>
#include <sys/stat.h>
#include <testing_utilities.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
namespace build = dftracer::utils::index::build;
namespace store = dftracer::utils::index::store;
namespace scan = dftracer::utils::utilities::filesystem;

namespace {

constexpr int SCAN_FILES = 200;
constexpr int NESTED_FILES = 10;
constexpr int INDEX_FILES = 64;
constexpr std::size_t WORKERS = 8;

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
}
