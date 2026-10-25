#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/index/store/db_manager.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <doctest/doctest.h>
#include <sys/resource.h>
#include <testing_utilities.h>
#include <trace/bootstrap_test_common.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::views;
namespace dataframe = dftracer::utils::dataframe;
namespace store = dftracer::utils::index::store;
using bootstrap_test::count_by_name;
using bootstrap_test::counts;
using bootstrap_test::view_files_of;

namespace {

constexpr int FILES = 96;
constexpr std::size_t WORKERS = 24;
#ifdef DFTRACER_UTILS_VALGRIND_MODE
// A Valgrind build caps every executor at 2 threads (thread_pool_executor.cpp).
constexpr std::size_t RUNNING_WORKERS = 2;
#else
constexpr std::size_t RUNNING_WORKERS = WORKERS;
#endif
constexpr int EVENTS = 1000;
constexpr std::size_t MAX_PREFIXES = 32;

std::string write_trace(const std::string& dir, int n) {
    std::ostringstream out;
    out << "[\n";
    for (int i = 0; i < EVENTS; ++i) {
        out << R"({"id":)" << i << R"(,"name":"op)" << (i + n) % 7
            << R"(","cat":"POSIX","pid":1,"tid":1,"ph":"X","ts":)"
            << 1000 + i * 10 << R"(,"dur":3,"args":{}})" << "\n";
    }
    out << "]\n";
    char name[32];
    std::snprintf(name, sizeof(name), "/t%04d.pfw.gz", n);
    return dftu_utils_test::write_gz_trace(dir + name, out.str());
}

std::size_t count_ssts(const std::string& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec))
        if (it->path().extension() == ".sst") ++n;
    return n;
}

std::size_t open_descriptors() {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it("/dev/fd", ec), end; !ec && it != end;
         it.increment(ec))
        ++n;
    return n;
}

std::string index_root_of(const std::string& file,
                          const std::string& index_dir) {
    return dftracer::utils::trace::internal::determine_index_path(file,
                                                                  index_dir);
}

class LineSink : public ExportSink {
   public:
    void write(std::string_view data) override { text.append(data); }
    std::string text;

    // A first touch dumps the raw file, array brackets included.
    std::vector<std::string> lines() const {
        std::vector<std::string> out;
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);)
            if (!line.empty() && line.front() == '{') out.push_back(line);
        std::sort(out.begin(), out.end());
        return out;
    }
};

std::vector<std::string> export_lines(const std::vector<std::string>& files,
                                      const std::string& index_dir) {
    const auto view = View::from_files(view_files_of(files, index_dir));
    LineSink sink;
    default_runtime().run_blocking("bootstrap-export",
                                   [&](CoroScope&) -> coro::CoroTask<void> {
                                       co_await view.sink_json(sink);
                                   });
    return sink.lines();
}

}  // namespace

TEST_SUITE("BootstrapParallelism") {
    TEST_CASE("a cold bootstrap keeps a file in flight per worker") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i));
        const auto index_dir = env.get_dir() + "/idx";

        set_default_runtime(std::make_shared<Runtime>(WORKERS));
        REQUIRE(default_runtime().threads() == WORKERS);

        trace::views::detail::bootstrap_counters().reset();
        const auto opens_before = store::IndexDatabase::read_write_opens();
        const auto cold = count_by_name(files, index_dir);
        const auto opens =
            store::IndexDatabase::read_write_opens() - opens_before;

        CHECK(trace::views::detail::bootstrap_counters().max_in_flight.load() ==
              RUNNING_WORKERS);
        CHECK(opens == 1);

        const auto warm = count_by_name(files, index_dir);
        CHECK(counts(cold) == counts(warm));
        std::int64_t total = 0;
        for (const auto& [name, n] : counts(cold)) total += n;
        CHECK(total == FILES * EVENTS);

        set_default_runtime(nullptr);
    }

    TEST_CASE("an export bootstrap commits its sink at the sink limit") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i));
        set_default_runtime(std::make_shared<Runtime>(WORKERS));

        trace::views::detail::bootstrap_counters().reset();
        const auto lines = export_lines(files, env.get_dir() + "/idx");
        CHECK(lines.size() == static_cast<std::size_t>(FILES) * EVENTS);
        const auto commits =
            trace::views::detail::bootstrap_counters().sink_commits.load();
        CHECK(commits >= 1);
        CHECK(commits < FILES / 8);

        set_default_runtime(nullptr);
    }

    TEST_CASE("the SST count does not grow with the files") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < 256; ++i)
            files.push_back(write_trace(env.get_dir(), i));
        const std::vector<std::string> few(files.begin(), files.begin() + 64);

        set_default_runtime(std::make_shared<Runtime>(WORKERS));
        const auto small_dir = env.get_dir() + "/idx64";
        const auto large_dir = env.get_dir() + "/idx256";
        count_by_name(few, small_dir);
        count_by_name(files, large_dir);
        const auto small_ssts = count_ssts(small_dir);
        const auto large_ssts = count_ssts(large_dir);
        MESSAGE("SSTs: 64 files " << small_ssts << ", 256 files "
                                  << large_ssts);

        // A worker leaves one SST per key prefix, whatever its number of files.
        CHECK(small_ssts > 0);
        CHECK(small_ssts <= WORKERS * MAX_PREFIXES);
        CHECK(large_ssts <= WORKERS * MAX_PREFIXES);
        CHECK(large_ssts <= small_ssts);
        CHECK_FALSE(memory::Manager::get().leaked());
        set_default_runtime(nullptr);
    }

    TEST_CASE("a read-only open of more SSTs than the descriptor limit works") {
#ifdef DFTRACER_UTILS_VALGRIND_MODE
        // 2 workers write too few SSTs to pass the lowered descriptor limit.
        MESSAGE("not run in a Valgrind build");
        return;
#endif
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i));
        const auto index_dir = env.get_dir() + "/idx";

        set_default_runtime(std::make_shared<Runtime>(WORKERS));
        count_by_name(files, index_dir);
        const auto root = index_root_of(files.front(), index_dir);
        store::RocksDBManager::instance().reset(root);

        struct rlimit before{};
        REQUIRE(::getrlimit(RLIMIT_NOFILE, &before) == 0);
        constexpr std::size_t HEADROOM = 64;
        const std::size_t ssts = count_ssts(root);
        struct rlimit lowered = before;
        lowered.rlim_cur = open_descriptors() + HEADROOM;
        REQUIRE(ssts > lowered.rlim_cur);
        REQUIRE(::setrlimit(RLIMIT_NOFILE, &lowered) == 0);
        bool opened = false;
        try {
            store::IndexDatabase db(root, store::IndexOpenMode::ReadOnly);
            opened = true;
        } catch (const std::exception& e) {
            MESSAGE("open failed: " << e.what());
        }
        REQUIRE(::setrlimit(RLIMIT_NOFILE, &before) == 0);
        CHECK(opened);
        set_default_runtime(nullptr);
    }

    TEST_CASE(
        "an export bootstrap opens the store once and equals a warm one") {
        dftu_utils_test::TestEnvironment env(10);
        std::vector<std::string> files;
        for (int i = 0; i < FILES; ++i)
            files.push_back(write_trace(env.get_dir(), i));
        const auto index_dir = env.get_dir() + "/idx";

        set_default_runtime(std::make_shared<Runtime>(WORKERS));
        const auto opens_before = store::IndexDatabase::read_write_opens();
        const auto cold = export_lines(files, index_dir);
        const auto opens =
            store::IndexDatabase::read_write_opens() - opens_before;
        CHECK(opens == 1);
        CHECK(cold.size() == static_cast<std::size_t>(FILES) * EVENTS);

        const auto warm = export_lines(files, index_dir);
        CHECK(cold == warm);
        CHECK(counts(count_by_name(files, index_dir)).size() == 7);
        CHECK_FALSE(memory::Manager::get().leaked());
        set_default_runtime(nullptr);
    }
}
