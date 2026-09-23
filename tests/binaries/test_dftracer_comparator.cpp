#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <simdjson.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

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
    return dftu_utils_test::find_binary_by_name("DFTRACER_GEN_FAKE_TRACE_PATH",
                                                "dftracer_gen_fake_trace");
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
    return dftu_utils_test::find_binary_by_name("DFTRACER_COMPARATOR_PATH",
                                                "dftracer_comparator");
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
                "Set DFTRACER_COMPARATOR_PATH env to specify location.");
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
                                 "--query", R"(cat == "POSIX")"},
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
                                 "--no-color", "--query", R"(cat == "POSIX")"},
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
                                 "--no-color", "--query", R"(cat == "POSIX")"},
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
                                 "json", "--query", R"(cat == "POSIX")"},
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
        CHECK(node0["query"].is_string());

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
                                 "json", "--query", R"(cat == "POSIX")"},
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
                                 "-t", "1000", "--query", R"(cat == "POSIX")"},
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
}
