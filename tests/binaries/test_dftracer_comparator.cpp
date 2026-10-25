#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <simdjson.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

std::string find_gen_binary() {
    return dftu_utils_test::find_binary_by_name(
        "DFTRACER_UTILS_GEN_FAKE_TRACE_PATH", "dftracer_gen_fake_trace");
}

std::string create_pfw_gz(dftu_utils_test::TestEnvironment& env,
                          int /* num_events */, int id) {
    auto gen = find_gen_binary();
    if (gen.empty()) return "";

    std::string out_dir = env.get_dir() + "/gen_" + std::to_string(id);
    // Generate a tiny trace: 1 rank, 1 epoch, 2 steps
    std::string cmd = gen + " -o '" + out_dir +
                      "' -p 1 -e 1 -s 2 --num-train-files 1"
                      " --num-val-files 1 --seed 42 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) return "";

    std::string src = out_dir + "/rank_0.pfw.gz";
    std::string dst =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    if (!fs::exists(src)) return "";
    fs::rename(src, dst);
    return dst;
}

std::string find_comparator_binary() {
    return dftu_utils_test::find_binary_by_name(
        "DFTRACER_UTILS_COMPARATOR_PATH", "dftracer_comparator");
}

int run_comparator(const std::string& binary,
                   const std::vector<std::string>& args,
                   const std::string& stdout_file = "") {
    std::vector<const char*> argv;
    argv.push_back(binary.c_str());
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!stdout_file.empty()) {
            int fd =
                ::open(stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                ::dup2(fd, STDOUT_FILENO);
                ::close(fd);
            }
        }
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Peak resident set of the child, from wait4. Returns 0 on a failed run.
std::uint64_t run_comparator_peak_rss(const std::string& binary,
                                      const std::vector<std::string>& args,
                                      const std::string& stdout_file) {
    std::vector<const char*> argv;
    argv.push_back(binary.c_str());
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        int fd =
            ::open(stdout_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::close(fd);
        }
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    int status = 0;
    struct rusage ru{};
    ::wait4(pid, &status, 0, &ru);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
#endif
}

// `files` small traces, one pid per file, so the number of (pid, tid, file,
// window) groups grows with the file count the way a real run's does.
void write_scale_set(const std::string& dir, int files, bool variant) {
    fs::create_directories(dir);
    const char* names[] = {"read", "write", "open"};
    for (int f = 0; f < files; ++f) {
        std::string text = "[\n";
        const std::uint64_t pid = 1000 + static_cast<std::uint64_t>(f);
        for (int i = 0; i < 60; ++i) {
            const std::uint64_t ts =
                1000000000ULL + static_cast<std::uint64_t>(i) * 190000ULL;
            const std::uint64_t dur = 100 +
                                      static_cast<std::uint64_t>(i % 7) * 40 +
                                      (variant ? 30 : 0);
            text +=
                R"({"id":)" + std::to_string(i) + R"(,"ph":"X","pid":)" +
                std::to_string(pid) + R"(,"tid":)" +
                std::to_string(pid * 10 + static_cast<std::uint64_t>(i % 2)) +
                R"(,"name":")" + names[i % 3] + R"(","cat":"POSIX","ts":)" +
                std::to_string(ts) + R"(,"dur":)" + std::to_string(dur) +
                R"(,"args":{"fhash":"f)" + std::to_string(i % 4) +
                R"(","size":)" + std::to_string(4096 * (1 + i % 5)) +
                R"(,"level":)" + std::to_string(i % 11) + R"(,"epoch":)" +
                std::to_string(i % 3) + R"(,"step":)" + std::to_string(i) +
                R"(,"batch":)" + std::to_string(i % 17) + R"(,"rank":)" +
                std::to_string(f) + R"(,"hops":)" + std::to_string(i % 5) +
                "}}\n";
        }
        text += "]\n";
        char name[32];
        std::snprintf(name, sizeof(name), "/trace_%04d.pfw.gz", f);
        REQUIRE(!dftu_utils_test::write_gz_trace(dir + name, text).empty());
    }
}

constexpr std::uint64_t MB = 1024 * 1024;
constexpr std::uint64_t WARM_BUDGET_BYTES = 160 * MB;
constexpr std::uint64_t WARM_GROWTH_BYTES = 64 * MB;
constexpr std::uint64_t COLD_BUDGET_BYTES = 768 * MB;

bool under_instrumentation() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

// A build whose instrumentation adds a fixed memory cost of its own, which
// the budgets measured on a release build do not hold.
bool fixed_cost_build() {
#if defined(DFTRACER_UTILS_COVERAGE_MODE) || \
    defined(DFTRACER_UTILS_VALGRIND_MODE)
    return true;
#else
    return false;
#endif
}

struct ScaleRss {
    /// The run that builds the indexes and then compares.
    std::uint64_t cold = 0;
    /// The run on the indexes the first run left: the comparison alone.
    std::uint64_t warm = 0;
};

// Peak RSS in bytes of comparing two `files`-file sets.
ScaleRss scale_run_rss(const std::string& binary, const std::string& work_dir,
                       int files) {
    const std::string base = work_dir + "/base_" + std::to_string(files);
    const std::string var = work_dir + "/var_" + std::to_string(files);
    write_scale_set(base, files, false);
    write_scale_set(var, files, true);
    const std::vector<std::string> args{"--baseline", base,   "--variant", var,
                                        "--format",   "json", "--no-color"};
    ScaleRss rss;
    rss.cold = run_comparator_peak_rss(
        binary, args, work_dir + "/cold_" + std::to_string(files) + ".json");
    rss.warm = run_comparator_peak_rss(
        binary, args, work_dir + "/warm_" + std::to_string(files) + ".json");
    REQUIRE(rss.cold > 0);
    REQUIRE(rss.warm > 0);
    return rss;
}

// One trace with a known number of events per kind: 6 POSIX reads, 4 POSIX
// writes, 3 STDIO fwrites, 2 COMPUTE steps and 5 dataloader fetches whose
// level arg is 1 to 5. 20 events in all.
std::string write_filter_trace(const std::string& dir) {
    fs::create_directories(dir);
    struct Kind {
        const char* cat;
        const char* name;
        int count;
        bool level;
    };
    const Kind kinds[] = {{"POSIX", "read", 6, false},
                          {"POSIX", "write", 4, false},
                          {"STDIO", "fwrite", 3, false},
                          {"COMPUTE", "step", 2, false},
                          {"dataloader", "fetch", 5, true}};
    std::string text = "[\n";
    int id = 0;
    for (const Kind& k : kinds) {
        for (int i = 0; i < k.count; ++i, ++id) {
            text += R"({"id":)" + std::to_string(id) +
                    R"(,"ph":"X","pid":7,"tid":8,"name":")" + k.name +
                    R"(","cat":")" + k.cat + R"(","ts":)" +
                    std::to_string(1000000000ULL + id * 1000ULL) +
                    R"(,"dur":100,"args":{"fhash":"f1")" +
                    (k.level ? R"(,"level":)" + std::to_string(i + 1)
                             : std::string()) +
                    "}}\n";
        }
    }
    text += "]\n";
    const std::string path = dir + "/filters.pfw.gz";
    REQUIRE(!dftu_utils_test::write_gz_trace(path, text).empty());
    return path;
}

// The events the root node counted for `query`.
std::uint64_t counted_events(const std::string& binary,
                             const std::string& trace,
                             const std::string& out_dir,
                             const std::string& query) {
    const std::string out = out_dir + "/count.json";
    REQUIRE(run_comparator(binary,
                           {"--baseline", trace, "--variant", trace, "--format",
                            "json", "--duql", query},
                           out) == 0);
    const std::string content = read_file(out);
    simdjson::dom::parser parser;
    auto doc = parser.parse(content);
    REQUIRE(!doc.error());
    for (auto m : doc["nodes"].at(0)["summary"]["metrics"]) {
        std::string_view name;
        if (m["name"].get(name) == simdjson::SUCCESS && name == "count") {
            double v = 0;
            REQUIRE(m["baseline"].get(v) == simdjson::SUCCESS);
            return static_cast<std::uint64_t>(v);
        }
    }
    return UINT64_MAX;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerComparator") {
    TEST_CASE("binary exists") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_comparator binary not found, skipping. "
                "Set DFTRACER_UTILS_COMPARATOR_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("help flag") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }
        int rc = run_comparator(binary, {"--help"});
        CHECK(rc == 0);
    }

    TEST_CASE("missing arguments") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }
        int rc = run_comparator(binary, {});
        CHECK(rc != 0);
    }

    TEST_CASE("basic comparison - same file") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/cmp_output.txt";
        int rc = run_comparator(binary,
                                {"--baseline", f, "--variant", f, "--no-color",
                                 "--duql", R"(cat == "POSIX")"},
                                output);
        CHECK(rc == 0);

        auto content = read_file(output);
        REQUIRE(!content.empty());
        CHECK(contains(content, "SUMMARY"));
        CHECK(contains(content, "count"));
        CHECK(contains(content, "Comparison:"));
        CHECK(contains(content, "+0.0%"));
    }

    TEST_CASE("basic comparison - two different files") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto baseline = create_pfw_gz(env, 100, 0);
        auto variant = create_pfw_gz(env, 200, 1);
        REQUIRE(!baseline.empty());
        REQUIRE(!variant.empty());

        std::string output = env.get_dir() + "/cmp_diff.txt";
        int rc = run_comparator(binary,
                                {"--baseline", baseline, "--variant", variant,
                                 "--no-color", "--duql", R"(cat == "POSIX")"},
                                output);
        CHECK(rc == 0);

        auto content = read_file(output);
        REQUIRE(!content.empty());
        CHECK(contains(content, "SUMMARY"));
        CHECK(contains(content, "count"));
        CHECK(contains(content, "Comparison:"));
    }

    TEST_CASE("directory comparison") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string base_dir = env.get_dir() + "/baseline";
        std::string var_dir = env.get_dir() + "/variant";
        fs::create_directories(base_dir);
        fs::create_directories(var_dir);

        {
            dftu_utils_test::TestEnvironment base_env(100);
            auto f = base_env.create_dft_test_gzip_file(50);
            REQUIRE(!f.empty());
            fs::rename(f, base_dir + "/trace_0.pfw.gz");
        }
        {
            dftu_utils_test::TestEnvironment var_env(100);
            auto f = var_env.create_dft_test_gzip_file(80);
            REQUIRE(!f.empty());
            fs::rename(f, var_dir + "/trace_0.pfw.gz");
        }

        std::string output = env.get_dir() + "/cmp_dir.txt";
        int rc = run_comparator(binary,
                                {"--baseline", base_dir, "--variant", var_dir,
                                 "--no-color", "--duql", R"(cat == "POSIX")"},
                                output);
        CHECK(rc == 0);

        auto content = read_file(output);
        REQUIRE(!content.empty());
        CHECK(contains(content, "SUMMARY"));
        CHECK(contains(content, "Comparison:"));
    }

    TEST_CASE("json output - valid structure") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/cmp_json.txt";
        int rc = run_comparator(binary,
                                {"--baseline", f, "--variant", f, "--format",
                                 "json", "--duql", R"(cat == "POSIX")"},
                                output);
        CHECK(rc == 0);

        auto content = read_file(output);
        REQUIRE(!content.empty());

        // Parse JSON
        simdjson::dom::parser parser;
        auto result = parser.parse(content);
        REQUIRE(!result.error());
        auto root = result.value_unsafe();
        REQUIRE(root.is_object());

        // Top-level fields
        CHECK(root["baseline"].is_string());
        CHECK(root["variant"].is_string());
        CHECK(root["baseline_meta"].is_object());
        CHECK(root["variant_meta"].is_object());
        CHECK(root["execution_time_ms"].is_number());

        // Nodes array
        auto nodes = root["nodes"];
        REQUIRE(!nodes.error());
        REQUIRE(nodes.is_array());
        auto nodes_arr = nodes.get_array().value_unsafe();
        REQUIRE(nodes_arr.size() > 0);

        // First node structure
        auto node0 = nodes_arr.at(0);
        REQUIRE(node0.is_object());
        CHECK(node0["name"].is_string());
        CHECK(node0["duql"].is_string());

        // Summary
        auto summary = node0["summary"];
        REQUIRE(!summary.error());
        REQUIRE(summary.is_object());
        auto sum_metrics = summary["metrics"];
        REQUIRE(!sum_metrics.error());
        REQUIRE(sum_metrics.is_array());
        auto sum_metrics_arr = sum_metrics.get_array().value_unsafe();
        REQUIRE(sum_metrics_arr.size() > 0);

        // First metric structure
        auto metric0 = sum_metrics_arr.at(0);
        REQUIRE(metric0.is_object());
        CHECK(metric0["name"].is_string());
        CHECK(metric0["baseline"].is_number());
        CHECK(metric0["variant"].is_number());
        CHECK(metric0["delta"].is_number());
        CHECK(metric0["pct_change"].is_number());
        CHECK(metric0["cohens_d"].is_number());
        CHECK(metric0["significance"].is_string());
        CHECK(metric0["is_regression"].is_bool());

        // Groups array exists
        CHECK(node0["groups"].is_array());

        // Children array exists
        CHECK(node0["children"].is_array());

        // Metadata objects
        auto base_meta = root["baseline_meta"];
        REQUIRE(!base_meta.error());
        REQUIRE(base_meta.is_object());
        CHECK(base_meta["files"].is_number());
        CHECK(base_meta["processes"].is_number());
        CHECK(base_meta["threads"].is_number());
        CHECK(base_meta["total_bytes"].is_number());
        CHECK(base_meta["total_io_time_us"].is_number());
        CHECK(base_meta["makespan_us"].is_number());

        auto var_meta = root["variant_meta"];
        REQUIRE(!var_meta.error());
        REQUIRE(var_meta.is_object());
        CHECK(var_meta["files"].is_number());
    }

    TEST_CASE("json output - same file deltas are zero") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/cmp_json_zero.txt";
        int rc = run_comparator(binary,
                                {"--baseline", f, "--variant", f, "--format",
                                 "json", "--duql", R"(cat == "POSIX")"},
                                output);
        CHECK(rc == 0);

        auto content = read_file(output);
        REQUIRE(!content.empty());

        simdjson::dom::parser parser;
        auto result = parser.parse(content);
        REQUIRE(!result.error());
        auto root = result.value_unsafe();
        auto nodes_arr = root["nodes"].get_array().value_unsafe();
        auto node0 = nodes_arr.at(0);
        auto metrics_arr =
            node0["summary"]["metrics"].get_array().value_unsafe();

        // All deltas should be ~0 when comparing same file
        for (auto m : metrics_arr) {
            double baseline = m["baseline"].get_double().value();
            double variant = m["variant"].get_double().value();
            CHECK(baseline == doctest::Approx(variant).epsilon(0.01));
        }
    }

    TEST_CASE("custom time interval") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/cmp_interval.txt";
        int rc = run_comparator(binary,
                                {"--baseline", f, "--variant", f, "--no-color",
                                 "-t", "1000", "--duql", R"(cat == "POSIX")"},
                                output);
        CHECK(rc == 0);
    }

    TEST_CASE("nonexistent baseline fails") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = run_comparator(binary, {"--baseline", "/nonexistent/path",
                                         "--variant", f, "--no-color"});
        CHECK(rc != 0);
    }

    TEST_CASE("peak memory does not grow with the file count") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }
        dftu_utils_test::ScopedTestDir dir("comparator_scale");
        const std::string work = dir.path().string();

        const ScaleRss small = scale_run_rss(binary, work, 100);
        const ScaleRss large = scale_run_rss(binary, work, 400);
        MESSAGE("peak rss, 100 files: cold "
                << small.cold << " warm " << small.warm << "; 400 files: cold "
                << large.cold << " warm " << large.warm);
        if (under_instrumentation()) return;

        // Measured on Corona (Linux, release): warm 58 MB at 100 files and
        // 92 MB at 400; the comparator before the index queries took 78 MB
        // and 220 MB. The budget and the margin sit between the two.
        CHECK(large.warm < small.warm + WARM_GROWTH_BYTES);
        if (fixed_cost_build()) return;
        CHECK(large.warm < WARM_BUDGET_BYTES);
        // The cold run also builds the indexes, whose per-key aggregation
        // tier holds one entry per (pid, tid, file, window, arg) until the
        // build ends; measured 180 MB and 540 MB.
        CHECK(large.cold < COLD_BUDGET_BYTES);
    }

    TEST_CASE("node queries count the events a hand count says") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }
        dftu_utils_test::ScopedTestDir dir("comparator_filters");
        const std::string work = dir.path().string();
        const std::string trace = write_filter_trace(work + "/data");

        struct Case {
            const char* what;
            const char* query;
            std::uint64_t events;
        };
        const Case cases[] = {
            {"all events", "", 20},
            {"category equality, either case", R"(cat == "posix")", 10},
            {"category equality, other case", R"(cat == "Compute")", 2},
            {"category in a list", R"(cat in ["PoSiX", "stdio"])", 13},
            {"name in a list", R"(name in ["read", "step"])", 8},
            {"like", R"(name like "%rit%")", 7},
            {"!=", R"(cat != "POSIX")", 10},
            {"not", R"(not (name == "read"))", 14},
            {"arg", "level > 2", 3},
            {"or across fields", R"(cat == "STDIO" or name == "read")", 9},
            {"pattern on cat", R"(cat ~ "^pos")", 10},
            {"set and expression", R"(cat == "POSIX" and name != "read")", 4}};
        for (const Case& c : cases) {
            SUBCASE(c.what) {
                CHECK(counted_events(binary, trace, work, c.query) == c.events);
            }
        }
    }
}
