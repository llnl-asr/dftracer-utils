#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <yyjson.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

std::string create_pfw_gz(dft_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

std::string find_comparator_binary() {
    const char* env_path = std::getenv("DFTRACER_COMPARATOR_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;

    std::vector<std::string> candidates = {
        "./dftracer_comparator",         "../dftracer_comparator",
        "../../dftracer_comparator",     "../bin/dftracer_comparator",
        "../../bin/dftracer_comparator",
    };
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

int run_comparator(const std::string& binary,
                   const std::vector<std::string>& args,
                   const std::string& stdout_file = "") {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (!stdout_file.empty()) {
            FILE* f = std::fopen(stdout_file.c_str(), "w");
            if (f) {
                dup2(fileno(f), STDOUT_FILENO);
                std::fclose(f);
            }
        }
        std::vector<const char*> argv;
        argv.push_back(binary.c_str());
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
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

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 100, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/cmp_output.txt";
        int rc = run_comparator(binary,
                                {"--baseline", f, "--variant", f, "--no-color",
                                 "--query", R"(cat == "IO")"},
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

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto baseline = create_pfw_gz(env, 100, 0);
        auto variant = create_pfw_gz(env, 200, 1);
        REQUIRE(!baseline.empty());
        REQUIRE(!variant.empty());

        std::string output = env.get_dir() + "/cmp_diff.txt";
        int rc = run_comparator(binary,
                                {"--baseline", baseline, "--variant", variant,
                                 "--no-color", "--query", R"(cat == "IO")"},
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

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string base_dir = env.get_dir() + "/baseline";
        std::string var_dir = env.get_dir() + "/variant";
        fs::create_directories(base_dir);
        fs::create_directories(var_dir);

        {
            dft_utils_test::TestEnvironment base_env(100);
            auto f = base_env.create_dft_test_gzip_file(50);
            REQUIRE(!f.empty());
            fs::rename(f, base_dir + "/trace_0.pfw.gz");
        }
        {
            dft_utils_test::TestEnvironment var_env(100);
            auto f = var_env.create_dft_test_gzip_file(80);
            REQUIRE(!f.empty());
            fs::rename(f, var_dir + "/trace_0.pfw.gz");
        }

        std::string output = env.get_dir() + "/cmp_dir.txt";
        int rc = run_comparator(binary,
                                {"--baseline", base_dir, "--variant", var_dir,
                                 "--no-color", "--query", R"(cat == "IO")"},
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

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/cmp_json.txt";
        int rc = run_comparator(binary,
                                {"--baseline", f, "--variant", f, "--format",
                                 "json", "--query", R"(cat == "IO")"},
                                output);
        CHECK(rc == 0);

        auto content = read_file(output);
        REQUIRE(!content.empty());

        // Parse JSON
        yyjson_doc* doc = yyjson_read(content.c_str(), content.size(), 0);
        REQUIRE(doc != nullptr);
        yyjson_val* root = yyjson_doc_get_root(doc);
        REQUIRE(root != nullptr);
        REQUIRE(yyjson_is_obj(root));

        // Top-level fields
        CHECK(yyjson_is_str(yyjson_obj_get(root, "baseline")));
        CHECK(yyjson_is_str(yyjson_obj_get(root, "variant")));
        CHECK(yyjson_is_obj(yyjson_obj_get(root, "baseline_meta")));
        CHECK(yyjson_is_obj(yyjson_obj_get(root, "variant_meta")));
        CHECK(yyjson_is_num(yyjson_obj_get(root, "execution_time_ms")));

        // Nodes array
        yyjson_val* nodes = yyjson_obj_get(root, "nodes");
        REQUIRE(yyjson_is_arr(nodes));
        REQUIRE(yyjson_arr_size(nodes) > 0);

        // First node structure
        yyjson_val* node0 = yyjson_arr_get_first(nodes);
        REQUIRE(yyjson_is_obj(node0));
        CHECK(yyjson_is_str(yyjson_obj_get(node0, "name")));
        CHECK(yyjson_is_str(yyjson_obj_get(node0, "query")));

        // Summary
        yyjson_val* summary = yyjson_obj_get(node0, "summary");
        REQUIRE(yyjson_is_obj(summary));
        yyjson_val* sum_metrics = yyjson_obj_get(summary, "metrics");
        REQUIRE(yyjson_is_arr(sum_metrics));
        REQUIRE(yyjson_arr_size(sum_metrics) > 0);

        // First metric structure
        yyjson_val* metric0 = yyjson_arr_get_first(sum_metrics);
        REQUIRE(yyjson_is_obj(metric0));
        CHECK(yyjson_is_str(yyjson_obj_get(metric0, "name")));
        CHECK(yyjson_is_num(yyjson_obj_get(metric0, "baseline")));
        CHECK(yyjson_is_num(yyjson_obj_get(metric0, "variant")));
        CHECK(yyjson_is_num(yyjson_obj_get(metric0, "delta")));
        CHECK(yyjson_is_num(yyjson_obj_get(metric0, "pct_change")));
        CHECK(yyjson_is_num(yyjson_obj_get(metric0, "cohens_d")));
        CHECK(yyjson_is_str(yyjson_obj_get(metric0, "significance")));
        CHECK(yyjson_is_bool(yyjson_obj_get(metric0, "is_regression")));

        // Groups array exists
        yyjson_val* groups = yyjson_obj_get(node0, "groups");
        CHECK(yyjson_is_arr(groups));

        // Children array exists
        yyjson_val* children = yyjson_obj_get(node0, "children");
        CHECK(yyjson_is_arr(children));

        // Metadata objects
        yyjson_val* base_meta = yyjson_obj_get(root, "baseline_meta");
        REQUIRE(yyjson_is_obj(base_meta));
        CHECK(yyjson_is_int(yyjson_obj_get(base_meta, "files")));
        CHECK(yyjson_is_int(yyjson_obj_get(base_meta, "processes")));
        CHECK(yyjson_is_int(yyjson_obj_get(base_meta, "threads")));
        CHECK(yyjson_is_num(yyjson_obj_get(base_meta, "total_bytes")));
        CHECK(yyjson_is_num(yyjson_obj_get(base_meta, "total_io_time_us")));
        CHECK(yyjson_is_num(yyjson_obj_get(base_meta, "makespan_us")));

        yyjson_val* var_meta = yyjson_obj_get(root, "variant_meta");
        REQUIRE(yyjson_is_obj(var_meta));
        CHECK(yyjson_is_int(yyjson_obj_get(var_meta, "files")));

        yyjson_doc_free(doc);
    }

    TEST_CASE("json output - same file deltas are zero") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/cmp_json_zero.txt";
        int rc = run_comparator(binary,
                                {"--baseline", f, "--variant", f, "--format",
                                 "json", "--query", R"(cat == "IO")"},
                                output);
        CHECK(rc == 0);

        auto content = read_file(output);
        REQUIRE(!content.empty());

        yyjson_doc* doc = yyjson_read(content.c_str(), content.size(), 0);
        REQUIRE(doc != nullptr);
        yyjson_val* root = yyjson_doc_get_root(doc);
        yyjson_val* nodes = yyjson_obj_get(root, "nodes");
        yyjson_val* node0 = yyjson_arr_get_first(nodes);
        yyjson_val* summary = yyjson_obj_get(node0, "summary");
        yyjson_val* metrics = yyjson_obj_get(summary, "metrics");

        // All deltas should be ~0 when comparing same file
        std::size_t idx, max;
        yyjson_val* m;
        yyjson_arr_foreach(metrics, idx, max, m) {
            double baseline = yyjson_get_real(yyjson_obj_get(m, "baseline"));
            double variant = yyjson_get_real(yyjson_obj_get(m, "variant"));
            CHECK(baseline == doctest::Approx(variant).epsilon(0.01));
        }

        yyjson_doc_free(doc);
    }

    TEST_CASE("custom time interval") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        std::string output = env.get_dir() + "/cmp_interval.txt";
        int rc = run_comparator(binary,
                                {"--baseline", f, "--variant", f, "--no-color",
                                 "-t", "1000", "--query", R"(cat == "IO")"},
                                output);
        CHECK(rc == 0);
    }

    TEST_CASE("nonexistent baseline fails") {
        auto binary = find_comparator_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_comparator binary not found, skipping.");
            return;
        }

        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = run_comparator(binary, {"--baseline", "/nonexistent/path",
                                         "--variant", f, "--no-color"});
        CHECK(rc != 0);
    }
}
