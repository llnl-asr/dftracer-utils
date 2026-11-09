// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

using namespace dftracer::utils;

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

dstd::String make_trace(int pid, int events) {
    dstd::String os;
    os += "[\n";
    for (int i = 0; i < events; ++i) {
        os += common::str_cat(
            R"({"id":)", i, R"(,"name":"read","cat":"POSIX","pid":)", pid,
            R"(,"tid":)", pid, R"(,"ts":)", (1000 + i * 10),
            R"(,"dur":5,"ph":"X","args":{"hhash":"abc","ret":1024}})", "\n");
    }
    return os;
}

struct Tool {
    dstd::String name;
    // in, idx and out are the input, index and output directories of the run.
    dstd::Vector<dstd::String> (*args)(const dstd::String& in,
                                       const dstd::String& idx,
                                       const dstd::String& out);
};

dstd::Vector<dstd::String> index_args(const dstd::String& in,
                                      const dstd::String& idx,
                                      const dstd::String&) {
    return {"--executor-threads", "2", in, "--index-dir", idx};
}

dstd::Vector<dstd::String> view_args(const dstd::String& in,
                                     const dstd::String& idx,
                                     const dstd::String&) {
    return {"--executor-threads", "2",   in, "--index-dir", idx,
            "--group-by",         "name"};
}

dstd::Vector<dstd::String> split_args(const dstd::String& in,
                                      const dstd::String&,
                                      const dstd::String& out) {
    return {"--executor-threads", "2",   in, "-o", out, "--app-name", "bench",
            "--chunk-size",       "64MB"};
}

dstd::Vector<dstd::String> validate_args(const dstd::String& in,
                                         const dstd::String&,
                                         const dstd::String&) {
    return {"--executor-threads", "2", in};
}

dstd::Vector<dstd::String> comparator_args(const dstd::String& in,
                                           const dstd::String& idx,
                                           const dstd::String&) {
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

dstd::Vector<dstd::String> replay_args(const dstd::String& in,
                                       const dstd::String&,
                                       const dstd::String&) {
    return {"--executor-threads", "2",          "--dry-run",
            "--dftracer-mode",    "--no-sleep", in};
}

dstd::String canonical_or_empty(const dstd::String& path) {
    auto p = dstd::fs::weakly_canonical(path);
    return p.has_value() ? dstd::String(p->str()) : dstd::String();
}

struct RunCounts {
    bool log_found = false;
    dstd::Vector<Counts> per_input;
    Counts root;
    std::size_t schema_lookups = 0;
    int exit_code = -1;
    dstd::String output;
};

RunCounts run_once(const dstd::String& binary,
                   const dstd::Vector<dstd::String>& args,
                   const dstd::String& log_path,
                   const dstd::Vector<dstd::String>& roots,
                   const dstd::Vector<dstd::String>& inputs) {
    RunCounts rc;
    rc.per_input.resize(inputs.size());
    dstd::fs::remove(log_path).discard();

    dftu_utils_test::set_test_library_path(binary);
    ::setenv(PRELOAD_VAR, DFTRACER_UTILS_TEST_INTERPOSE_PATH, 1);
    ::setenv("DFTRACER_UTILS_TEST_SYSCALL_LOG", log_path.c_str(), 1);
    rc.output =
        dftu_utils_test::run_process_capture(binary, args, true, &rc.exit_code);
    ::unsetenv(PRELOAD_VAR);
    ::unsetenv("DFTRACER_UTILS_TEST_SYSCALL_LOG");

    const auto log = dstd::fs::read_file(dstd::PathView(log_path));
    if (!log) return rc;
    rc.log_found = true;

    dstd::Vector<dstd::String> canon_inputs;
    for (const auto& i : inputs) canon_inputs.push_back(canonical_or_empty(i));
    dstd::Vector<dstd::String> canon_roots;
    for (const auto& r : roots) canon_roots.push_back(canonical_or_empty(r));

    dstd::Map<dstd::String, dstd::String> canon_cache;
    dstd::StringView rest = *log;
    while (!rest.empty()) {
        const std::size_t eol = rest.find('\n');
        const dstd::String line(rest.substr(0, eol));
        rest = eol == dstd::StringView::npos ? dstd::StringView{}
                                             : rest.substr(eol + 1);
        auto t1 = line.find('\t');
        auto t2 = line.find('\t', t1 + 1);
        if (t1 == dstd::String::npos || t2 == dstd::String::npos) continue;
        const dstd::String cls = line.substr(0, t1);
        const auto parsed =
            dstd::from_chars<std::size_t>(line.substr(t1 + 1, t2 - t1 - 1));
        REQUIRE(parsed.has_value());
        const std::size_t n = *parsed;
        const dstd::String path = line.substr(t2 + 1);
        if (path.empty() || path[0] != '/') continue;
        if (cls == "stat" && dstd::Path(path).view().filename() == "schemas")
            rc.schema_lookups += n;
        auto it = canon_cache.find(path);
        if (it == canon_cache.end())
            it = canon_cache.emplace(path, canonical_or_empty(path)).first;
        const dstd::String& c = it->second;
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

void check_run(const dstd::String& label, const dstd::String& tool,
               const RunCounts& rc, const dstd::Vector<dstd::String>& inputs,
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
    const dstd::String binary = dstd::String(DFTRACER_UTILS_TEST_BIN_DIR) +
                                "/" + "dftracer_" + tool.name;
    REQUIRE_MESSAGE(::access(binary.c_str(), X_OK) == 0,
                    binary << " is not built");

    dftu_utils_test::ScopedTestDir dir("input_syscalls_" + tool.name);
    const dstd::String in = dir.file("in");
    // The comparator reads one directory per side. Each side gets its own copy
    // of the files, so every input file is read for one side only.
    dstd::Vector<dstd::String> roots{in};
    if (tool.name == "comparator") roots.push_back(in + "_variant");
    dstd::Vector<dstd::String> inputs;
    for (const auto& root : roots) {
        REQUIRE(dstd::fs::create_directories(root + "/sub").has_value());
        for (int i = 0; i < 4; ++i)
            inputs.push_back(root + "/trace_" + dstd::to_string(i) + ".pfw.gz");
        inputs.push_back(root + "/sub/trace_4.pfw.gz");
    }
    for (std::size_t i = 0; i < inputs.size(); ++i)
        REQUIRE(!dftu_utils_test::write_gz_trace(
                     inputs[i], make_trace(static_cast<int>(i % 5) + 1, 300))
                     .empty());

    const dstd::String idx = dir.file("idx");
    const dstd::String out = dir.file("out");
    REQUIRE(dstd::fs::create_directories(idx).has_value());
    REQUIRE(dstd::fs::create_directories(out).has_value());
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
    const dstd::String binary =
        dstd::String(DFTRACER_UTILS_TEST_BIN_DIR) + "/dftracer_index";
    REQUIRE_MESSAGE(::access(binary.c_str(), X_OK) == 0,
                    binary << " is not built");
    dftu_utils_test::ScopedTestDir dir("input_syscalls_schemas");
    const dstd::String in = dir.file("in");
    const dstd::String idx = dir.file("idx");
    REQUIRE(dstd::fs::create_directories(in).has_value());
    REQUIRE(dstd::fs::create_directories(idx).has_value());
    constexpr int FILES = 24;
    dstd::Vector<dstd::String> inputs;
    for (int i = 0; i < FILES; ++i) {
        inputs.push_back(in + "/trace_" + dstd::to_string(i) + ".pfw.gz");
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
