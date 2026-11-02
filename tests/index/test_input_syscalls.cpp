#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// Every CLI tool stats each input at most once per run and opens it at most
// once per pass over its bytes (2 on a cold run, 1 for dftracer_index and on a
// warm run). A test-only library injected into the tool counts the calls.

namespace {

#ifdef __APPLE__
constexpr const char* PRELOAD_VAR = "DYLD_INSERT_LIBRARIES";
#else
constexpr const char* PRELOAD_VAR = "LD_PRELOAD";
#endif

constexpr std::size_t MAX_STATS_PER_INPUT = 1;
constexpr std::size_t MAX_STATS_PER_ROOT = 2;
// The schemas folder lookup of an index root is a few per run, never one per
// input file, since one lookup can block for minutes on a parallel file system.
constexpr std::size_t MAX_SCHEMA_LOOKUPS = 8;

struct Counts {
    std::size_t stat = 0;
    std::size_t open = 0;
};

std::string make_trace(int pid, int events) {
    std::ostringstream os;
    os << "[\n";
    for (int i = 0; i < events; ++i) {
        os << R"({"id":)" << i << R"(,"name":"read","cat":"POSIX","pid":)"
           << pid << R"(,"tid":)" << pid << R"(,"ts":)" << (1000 + i * 10)
           << R"(,"dur":5,"ph":"X","args":{"hhash":"abc","ret":1024}})"
           << "\n";
    }
    return os.str();
}

struct Tool {
    std::string name;
    // in, idx and out are the input, index and output directories of the run.
    std::vector<std::string> (*args)(const std::string& in,
                                     const std::string& idx,
                                     const std::string& out);
};

std::vector<std::string> index_args(const std::string& in,
                                    const std::string& idx,
                                    const std::string&) {
    return {"--executor-threads", "2", "-d", in, "--index-dir", idx};
}

std::vector<std::string> view_args(const std::string& in,
                                   const std::string& idx, const std::string&) {
    return {"--executor-threads", "2", "-d",         in,
            "--index-dir",        idx, "--group-by", "name"};
}

std::vector<std::string> split_args(const std::string& in, const std::string&,
                                    const std::string& out) {
    return {"--executor-threads",
            "2",
            "-d",
            in,
            "-o",
            out,
            "-n",
            "bench",
            "-s",
            "64MB"};
}

std::vector<std::string> validate_args(const std::string& in,
                                       const std::string&, const std::string&) {
    return {"--executor-threads", "2", "-d", in};
}

std::vector<std::string> comparator_args(const std::string& in,
                                         const std::string& idx,
                                         const std::string&) {
    return {"--executor-threads",
            "2",
            "--baseline",
            in,
            "--variant",
            in + "_variant",
            "--baseline-index-dir",
            idx + "/baseline",
            "--variant-index-dir",
            idx + "/variant"};
}

std::vector<std::string> replay_args(const std::string& in, const std::string&,
                                     const std::string&) {
    return {"--executor-threads", "2",          "--dry-run",
            "--dftracer-mode",    "--no-sleep", in};
}

std::string canonical_or_empty(const std::string& path) {
    std::error_code ec;
    auto p = fs::weakly_canonical(path, ec);
    return ec ? std::string() : p.string();
}

struct RunCounts {
    bool log_found = false;
    std::vector<Counts> per_input;
    Counts root;
    std::size_t schema_lookups = 0;
    int exit_code = -1;
    std::string output;
};

RunCounts run_once(const std::string& binary,
                   const std::vector<std::string>& args,
                   const std::string& log_path,
                   const std::vector<std::string>& roots,
                   const std::vector<std::string>& inputs) {
    RunCounts rc;
    rc.per_input.resize(inputs.size());
    std::error_code ec;
    fs::remove(log_path, ec);

    dftu_utils_test::set_test_library_path(binary);
    ::setenv(PRELOAD_VAR, DFTRACER_UTILS_TEST_INTERPOSE_PATH, 1);
    ::setenv("DFTRACER_UTILS_TEST_SYSCALL_LOG", log_path.c_str(), 1);
    rc.output =
        dftu_utils_test::run_process_capture(binary, args, true, &rc.exit_code);
    ::unsetenv(PRELOAD_VAR);
    ::unsetenv("DFTRACER_UTILS_TEST_SYSCALL_LOG");

    std::ifstream in(log_path);
    if (!in) return rc;
    rc.log_found = true;

    std::vector<std::string> canon_inputs;
    for (const auto& i : inputs) canon_inputs.push_back(canonical_or_empty(i));
    std::vector<std::string> canon_roots;
    for (const auto& r : roots) canon_roots.push_back(canonical_or_empty(r));

    std::map<std::string, std::string> canon_cache;
    std::string line;
    while (std::getline(in, line)) {
        auto t1 = line.find('\t');
        auto t2 = line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        const std::string cls = line.substr(0, t1);
        const std::size_t n = std::stoul(line.substr(t1 + 1, t2 - t1 - 1));
        const std::string path = line.substr(t2 + 1);
        if (path.empty() || path[0] != '/') continue;
        if (cls == "stat" && fs::path(path).filename() == "schemas")
            rc.schema_lookups += n;
        auto it = canon_cache.find(path);
        if (it == canon_cache.end())
            it = canon_cache.emplace(path, canonical_or_empty(path)).first;
        const std::string& c = it->second;
        Counts* hit = nullptr;
        if (std::find(canon_roots.begin(), canon_roots.end(), c) !=
            canon_roots.end()) {
            hit = &rc.root;
        } else {
            for (std::size_t k = 0; k < inputs.size(); ++k)
                if (c == canon_inputs[k]) hit = &rc.per_input[k];
        }
        if (hit == nullptr) continue;
        (cls == "stat" ? hit->stat : hit->open) += n;
    }
    return rc;
}

void check_run(const std::string& label, const std::string& tool,
               const RunCounts& rc, const std::vector<std::string>& inputs,
               std::size_t max_open, std::size_t roots) {
    INFO(tool << " " << label << " exit code " << rc.exit_code);
    if (!rc.log_found) MESSAGE(tool << " output: " << rc.output);
    REQUIRE_MESSAGE(rc.log_found,
                    tool << " " << label << ": no syscall log (exit code "
                         << rc.exit_code
                         << "); the interposer did not load or the tool "
                            "died before exit");
    std::size_t total = rc.root.stat + rc.root.open;
    for (const auto& c : rc.per_input) total += c.stat + c.open;
    CHECK_MESSAGE(total > 0, tool << " " << label
                                  << ": zero calls counted; the interposer "
                                     "did not load");
    for (std::size_t k = 0; k < inputs.size(); ++k) {
        MESSAGE(tool << " " << label << " " << inputs[k]
                     << " stat=" << rc.per_input[k].stat
                     << " open=" << rc.per_input[k].open);
        CHECK_MESSAGE(rc.per_input[k].stat <= MAX_STATS_PER_INPUT,
                      tool << " " << label << " stat count of " << inputs[k]
                           << " is " << rc.per_input[k].stat);
        CHECK_MESSAGE(rc.per_input[k].open <= max_open,
                      tool << " " << label << " open count of " << inputs[k]
                           << " is " << rc.per_input[k].open);
    }
    MESSAGE(tool << " " << label << " schemas lookups=" << rc.schema_lookups);
    CHECK_MESSAGE(rc.schema_lookups <= MAX_SCHEMA_LOOKUPS,
                  tool << " " << label << " looked for the schemas folder "
                       << rc.schema_lookups << " times");
    MESSAGE(tool << " " << label << " input directory stat=" << rc.root.stat);
    CHECK_MESSAGE(rc.root.stat <= MAX_STATS_PER_ROOT * roots,
                  tool << " " << label << " input directory stat count is "
                       << rc.root.stat);
}

void run_tool(const Tool& tool) {
    const std::string binary = std::string(DFTRACER_UTILS_TEST_BIN_DIR) + "/" +
                               "dftracer_" + tool.name;
    REQUIRE_MESSAGE(::access(binary.c_str(), X_OK) == 0,
                    binary << " is not built");

    dftu_utils_test::ScopedTestDir dir("input_syscalls_" + tool.name);
    const std::string in = dir.file("in");
    // The comparator reads one directory per side. Each side gets its own copy
    // of the files, so every input file is read for one side only.
    std::vector<std::string> roots{in};
    if (tool.name == "comparator") roots.push_back(in + "_variant");
    std::vector<std::string> inputs;
    for (const auto& root : roots) {
        fs::create_directories(root + "/sub");
        for (int i = 0; i < 4; ++i)
            inputs.push_back(root + "/trace_" + std::to_string(i) + ".pfw.gz");
        inputs.push_back(root + "/sub/trace_4.pfw.gz");
    }
    for (std::size_t i = 0; i < inputs.size(); ++i)
        REQUIRE(!dftu_utils_test::write_gz_trace(
                     inputs[i], make_trace(static_cast<int>(i % 5) + 1, 300))
                     .empty());

    const std::string idx = dir.file("idx");
    const std::string out = dir.file("out");
    fs::create_directories(idx);
    fs::create_directories(out);
    const auto args = tool.args(in, idx, out);

    const auto cold =
        run_once(binary, args, dir.file("cold.log"), roots, inputs);
    check_run("cold", tool.name, cold, inputs, tool.name == "index" ? 1 : 2,
              roots.size());
    const auto warm =
        run_once(binary, args, dir.file("warm.log"), roots, inputs);
    check_run("warm", tool.name, warm, inputs, 1, roots.size());
}

}  // namespace

TEST_CASE("a cold index build looks for the schemas folder once per root") {
    const std::string binary =
        std::string(DFTRACER_UTILS_TEST_BIN_DIR) + "/dftracer_index";
    REQUIRE_MESSAGE(::access(binary.c_str(), X_OK) == 0,
                    binary << " is not built");
    dftu_utils_test::ScopedTestDir dir("input_syscalls_schemas");
    const std::string in = dir.file("in");
    const std::string idx = dir.file("idx");
    fs::create_directories(in);
    fs::create_directories(idx);
    constexpr int FILES = 24;
    std::vector<std::string> inputs;
    for (int i = 0; i < FILES; ++i) {
        inputs.push_back(in + "/trace_" + std::to_string(i) + ".pfw.gz");
        REQUIRE(!dftu_utils_test::write_gz_trace(inputs.back(),
                                                 make_trace(i + 1, 50))
                     .empty());
    }
    const auto run = run_once(binary, index_args(in, idx, {}),
                              dir.file("schemas.log"), {in}, inputs);
    REQUIRE(run.log_found);
    REQUIRE(run.exit_code == 0);
    MESSAGE("schemas lookups=" << run.schema_lookups);
    CHECK(run.schema_lookups <= 2);
}

TEST_CASE("dftracer_index") { run_tool({"index", index_args}); }
TEST_CASE("dftracer_event_count") { run_tool({"event_count", index_args}); }
TEST_CASE("dftracer_info") { run_tool({"info", index_args}); }
TEST_CASE("dftracer_stats") { run_tool({"stats", index_args}); }
TEST_CASE("dftracer_view") { run_tool({"view", view_args}); }
TEST_CASE("dftracer_split") { run_tool({"split", split_args}); }
TEST_CASE("dftracer_validate") { run_tool({"validate", validate_args}); }
TEST_CASE("dftracer_comparator") { run_tool({"comparator", comparator_args}); }
TEST_CASE("dftracer_replay") { run_tool({"replay", replay_args}); }
