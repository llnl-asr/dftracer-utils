#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_runtime.h>
#include <testing_utilities.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::genesis;
using dftu_utils_test::ScopedTestDir;

namespace {

std::string call(std::int64_t pid, std::int64_t tid, const std::string& name,
                 std::int64_t ts, std::int64_t dur,
                 const std::string& cat = "CPP_APP",
                 const std::string& extra = "") {
    std::ostringstream o;
    o << R"({"name":")" << name << R"(","cat":")" << cat << R"(","pid":)" << pid
      << R"(,"tid":)" << tid << R"(,"ts":)" << ts << R"(,"dur":)" << dur
      << R"(,"ph":1,"args":{"hhash":"h1")" << extra << "}}\n";
    return o.str();
}

std::string marker(std::int64_t pid, const std::string& name, std::int64_t ts) {
    return call(pid, pid, name, ts, 0, "dftracer");
}

std::string papi(std::int64_t pid, std::int64_t ts, const std::string& counter,
                 double delta) {
    std::ostringstream o;
    o << R"({"name":"PAPI","cat":"papi","pid":)" << pid << R"(,"tid":)" << pid
      << R"(,"ts":)" << ts << R"(,"ph":2,"args":{"hhash":"h1","multiplex":0,")"
      << counter << R"(":1,")" << counter << R"(_delta":)" << delta << "}}\n";
    return o.str();
}

std::string host(const std::string& cat, const std::string& name,
                 std::int64_t ts, const std::string& fields) {
    std::ostringstream o;
    o << R"({"name":")" << name << R"(","cat":")" << cat
      << R"(","pid":0,"tid":0,"ts":)" << ts
      << R"(,"ph":2,"args":{"hhash":"h1",)" << fields << "}}\n";
    return o.str();
}

std::string rank(std::int64_t pid, std::int64_t start, std::int64_t end) {
    return marker(pid, "start", start) + marker(pid, "end", end);
}

void write_gz(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    dftu_utils_test::write_gz_trace(p.string(), content);
}

void write_text(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << content;
}

fs::path matrix_run(const ScopedTestDir& d, std::int64_t ppn,
                    const std::string& trace,
                    const std::string& input = "in1") {
    const fs::path dir = d.path() / "app1" / "sys1" / input / "nodes_1" /
                         ("ppn_" + std::to_string(ppn));
    write_text(dir / "summary.json",
               R"({"ok":true,"app":"app1","system":"sys1","input":")" + input +
                   R"(","nodes":1,"ppn":)" + std::to_string(ppn) +
                   R"(,"sets":["set1"]})");
    write_gz(dir / "compacted" / "set1.pfw.gz", "[\n" + trace);
    return dir;
}

coro::CoroTask<void> process_into(CoroScope& ctx, RunGroup g,
                                  GroupResult* out) {
    *out = co_await process_group(ctx, std::move(g));
}

coro::CoroTask<void> discover_into(CoroScope& ctx, std::string root,
                                   Discovery* out) {
    *out = co_await discover(ctx, {std::move(root)});
}

GroupResult process(RunGroup g) {
    GroupResult out;
    dftu_utils_test::run_coro(
        [&](CoroScope& ctx) { return process_into(ctx, std::move(g), &out); });
    return out;
}

Discovery discover_dir(const ScopedTestDir& d) {
    Discovery out;
    dftu_utils_test::run_coro(
        [&](CoroScope& ctx) { return discover_into(ctx, d.str(), &out); });
    return out;
}

GroupResult process_only(const ScopedTestDir& d) {
    auto disc = discover_dir(d);
    REQUIRE(disc.skips.empty());
    REQUIRE(disc.groups.size() == 1);
    return process(disc.groups[0]);
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l))
        if (!l.empty()) out.push_back(l);
    return out;
}

// The record line whose args.path equals `path`, or "" when absent.
std::string record(const GroupResult& r, const std::string& path) {
    REQUIRE(r.runs.size() == 1);
    const std::string needle = R"("path":")" + path + R"(",)";
    for (const auto& l : lines_of(r.runs[0].lines))
        if (l.find(needle) != std::string::npos) return l;
    return "";
}

simdjson::dom::element at(simdjson::dom::parser& p, const std::string& line,
                          const std::string& pointer) {
    simdjson::dom::element e;
    REQUIRE(p.parse(line).at_pointer(pointer).get(e) == simdjson::SUCCESS);
    return e;
}

double num(const std::string& line, const std::string& pointer) {
    simdjson::dom::parser p;
    double d = 0;
    REQUIRE(at(p, line, pointer).get(d) == simdjson::SUCCESS);
    return d;
}

bool has(const std::string& line, const std::string& pointer) {
    simdjson::dom::parser p;
    return p.parse(line).at_pointer(pointer).error() == simdjson::SUCCESS;
}

bool skipped_with(const GroupResult& r, const std::string& text) {
    for (const auto& s : r.skips)
        if (s.reason.find(text) != std::string::npos ||
            s.file.find(text) != std::string::npos)
            return true;
    return false;
}

}  // namespace

TEST_CASE("run_id is FNV-1a 64 of the joined run keys") {
    RunKeys k{"laghos", "matrix", "p1_dim2_rs2_tf3.1_pa",
              1,        2,        "papi_set1_branch_cycle"};
    CHECK(run_id(k) == "c8a6831dbb2f883e");
}

TEST_CASE("discover reads the matrix layout, one group per set") {
    ScopedTestDir d("genesis_v1");
    const fs::path dir =
        d.path() / "laghos" / "matrix" / "p1" / "nodes_2" / "ppn_4";
    write_text(dir / "summary.json",
               R"({"ok":true,"app":"laghos","system":"matrix","input":"p1",)"
               R"("nodes":2,"ppn":4,"sets":["s1","s2"]})");
    write_gz(dir / "compacted" / "s1.pfw.gz", "[\n");
    write_gz(dir / "compacted" / "s2.pfw.gz", "[\n");
    auto disc = discover_dir(d);
    REQUIRE(disc.skips.empty());
    REQUIRE(disc.groups.size() == 2);
    const RunKeys& k = disc.groups[1].sets.at(0).keys;
    CHECK(k.app == "laghos");
    CHECK(k.system == "matrix");
    CHECK(k.unique_input == "p1");
    CHECK(k.nodes == 2);
    CHECK(k.ppn == 4);
    CHECK(k.papi_set == "s2");
    CHECK(disc.groups[1].files ==
          std::vector<std::string>{(dir / "compacted" / "s2.pfw.gz").string()});
}

TEST_CASE("discover reads the tioga v2 layout as one group of several sets") {
    ScopedTestDir d("genesis_v2");
    const fs::path dir =
        d.path() / "laghos" / "tioga" / "tioga" / "caseA" / "nodes_1" / "ppn_1";
    write_text(dir / "summary.json",
               R"({"application":"laghos","case":"caseA","nodes":1,"ppn":1,)"
               R"("runs":{"setB":{"papi_counters":["PAPI_B"]},)"
               R"("setA":{"papi_counters":["PAPI_A2","PAPI_A1"]}}})");
    write_gz(dir / "compacted" / "caseA_N1_ppn1-1_chunk0.pfw.gz", "[\n");
    write_gz(dir / "compacted" / "caseA_N1_ppn1-2_chunk0.pfw.gz", "[\n");
    auto disc = discover_dir(d);
    REQUIRE(disc.skips.empty());
    REQUIRE(disc.groups.size() == 1);
    const auto& g = disc.groups[0];
    CHECK(g.files.size() == 2);
    REQUIRE(g.sets.size() == 2);
    CHECK(g.sets[0].keys.papi_set == "setA");
    CHECK(g.sets[0].keys.app == "laghos");
    CHECK(g.sets[0].keys.system == "tioga");
    CHECK(g.sets[0].keys.unique_input == "caseA");
    CHECK(g.sets[0].papi_counters ==
          std::vector<std::string>{"PAPI_A1", "PAPI_A2"});
}

TEST_CASE("discover skips a tioga v2 directory with missing slices") {
    ScopedTestDir d("genesis_gap");
    const fs::path dir =
        d.path() / "laghos" / "tioga" / "tioga" / "c" / "nodes_1" / "ppn_1";
    write_text(dir / "summary.json",
               R"({"application":"laghos","case":"c","nodes":1,"ppn":1,)"
               R"("runs":{"s":{"papi_counters":["PAPI_A"]}}})");
    for (int i : {1, 3, 4, 7})
        write_gz(dir / "compacted" /
                     ("c_N1_ppn1-" + std::to_string(i) + "_chunk0.pfw.gz"),
                 "[\n");
    auto disc = discover_dir(d);
    CHECK(disc.groups.empty());
    REQUIRE(disc.skips.size() == 1);
    CHECK(disc.skips[0].reason == "missing slices 2,5-6");
}

TEST_CASE("discover reads the path-only layout as one sliced group") {
    ScopedTestDir d("genesis_path");
    const fs::path c = d.path() / "minife" / "tioga" / "tioga" / "input_50" /
                       "nodes_2" / "ppn_2" / "compacted";
    write_gz(c / "nx50_N2_ppn2-1_chunk0.pfw.gz", "[\n");
    write_gz(c / "nx50_N2_ppn2-2_chunk0.pfw.gz", "[\n");
    auto disc = discover_dir(d);
    REQUIRE(disc.skips.empty());
    REQUIRE(disc.groups.size() == 1);
    CHECK(disc.groups[0].sets_from_counters);
    CHECK(disc.groups[0].files.size() == 2);
    const RunKeys& k = disc.groups[0].sets.at(0).keys;
    CHECK(k.app == "minife");
    CHECK(k.system == "tioga");
    CHECK(k.unique_input == "input_50");
    CHECK(k.nodes == 2);
    CHECK(k.ppn == 2);
}

TEST_CASE("path-only sets come from each process's PAPI counters") {
    ScopedTestDir d("genesis_derive");
    const fs::path c = d.path() / "minife" / "tioga" / "tioga" / "in" /
                       "nodes_1" / "ppn_1" / "compacted";
    write_gz(c / "nx_N1_ppn1-1_chunk0.pfw.gz",
             rank(1, 0, 100) + papi(1, 0, "PAPI_B", 0) +
                 call(1, 1, "a", 0, 10) + rank(2, 200, 300) +
                 papi(2, 200, "PAPI_A", 0) + call(2, 2, "b", 200, 10));
    auto r = process_only(d);
    REQUIRE(r.skips.empty());
    REQUIRE(r.runs.size() == 2);
    CHECK(r.runs[0].lines.find(R"("papi_set":"PAPI_A")") != std::string::npos);
    CHECK(r.runs[0].lines.find(R"("path":"b")") != std::string::npos);
    CHECK(r.runs[1].lines.find(R"("papi_set":"PAPI_B")") != std::string::npos);
}

TEST_CASE("a matrix summary without app and system takes them from the path") {
    ScopedTestDir d("genesis_v1path");
    const fs::path dir =
        d.path() / "minife" / "matrix" / "input_9" / "nodes_1" / "ppn_1";
    write_text(dir / "summary.json",
               R"({"ok":true,"input":"9","nodes":1,"ppn":1,"sets":["s1"]})");
    write_gz(dir / "compacted" / "s1.pfw.gz", "[\n");
    auto disc = discover_dir(d);
    REQUIRE(disc.groups.size() == 1);
    const RunKeys& k = disc.groups[0].sets.at(0).keys;
    CHECK(k.app == "minife");
    CHECK(k.system == "matrix");
    CHECK(k.unique_input == "input_9");
}

TEST_CASE("discover reports directory-level problems as skips") {
    ScopedTestDir d("genesis_dirskip");
    const fs::path base = d.path() / "a" / "s";
    write_text(base / "i1" / "nodes_1" / "ppn_1" / "summary.json", "{not json");
    write_text(base / "i2" / "nodes_1" / "ppn_1" / "summary.json",
               R"({"ok":false,"sets":[]})");
    write_text(base / "i3" / "nodes_1" / "ppn_1" / "summary.json",
               R"({"ok":true,"app":"a","system":"s","input":"i3","nodes":1,)"
               R"("ppn":1,"sets":["missing"]})");
    auto disc = discover_dir(d);
    CHECK(disc.groups.empty());
    REQUIRE(disc.skips.size() == 3);
    CHECK(disc.skips[0].reason == "unparsable JSON");
    CHECK(disc.skips[1].reason == "\"ok\" is false");
    CHECK(disc.skips[2].reason == "set listed but file missing");
}

TEST_CASE("the same function under two parents gives two records") {
    ScopedTestDir d("genesis_paths");
    matrix_run(
        d, 2,
        rank(1, 0, 1000) + rank(2, 0, 1000) + call(1, 1, "main", 0, 1000) +
            call(1, 1, "SolveVelocity", 10, 190) +
            call(1, 1, "ForceMult", 20, 80) + call(1, 1, "cudaMemcpy", 30, 10) +
            call(1, 1, "SolveEnergy", 300, 100) +
            call(1, 1, "cudaMemcpy", 310, 10) + call(2, 2, "main", 0, 1000));
    auto r = process_only(d);
    REQUIRE(r.skips.empty());
    const auto a = record(r, "main;SolveVelocity;ForceMult;cudaMemcpy");
    const auto b = record(r, "main;SolveEnergy;cudaMemcpy");
    REQUIRE(!a.empty());
    REQUIRE(!b.empty());
    CHECK(num(a, "/args/depth") == 3);
    CHECK(a.find(R"("parent":"main;SolveVelocity;ForceMult")") !=
          std::string::npos);
    CHECK_FALSE(has(record(r, "main"), "/args/parent"));
    CHECK(num(record(r, "main"), "/args/count") == 2);
}

TEST_CASE("duration aggregates are exact across ranks") {
    ScopedTestDir d("genesis_exact");
    matrix_run(d, 2,
               rank(1, 0, 1000) + rank(2, 0, 1000) +
                   call(1, 1, "main", 0, 1000) + call(1, 1, "X", 10, 5) +
                   call(2, 2, "main", 0, 1000) + call(2, 2, "X", 10, 7) +
                   call(2, 2, "X", 100, 9));
    auto r = process_only(d);
    const auto x = record(r, "main;X");
    CHECK(num(x, "/args/count") == 3);
    CHECK(num(x, "/args/dur/min") == 5);
    CHECK(num(x, "/args/dur/max") == 9);
    CHECK(num(x, "/args/dur/sum") == 21);
    CHECK(num(x, "/args/dur/avg") == 7);
}

TEST_CASE("per-process counters are pro-rated by overlap") {
    ScopedTestDir d("genesis_prorate");
    matrix_run(d, 1,
               rank(1, 0, 1000) + papi(1, 0, "PAPI_TOT_CYC", 0) +
                   papi(1, 100, "PAPI_TOT_CYC", 1000) +
                   papi(1, 200, "PAPI_TOT_CYC", 2000) +
                   call(1, 1, "main", 0, 1000) + call(1, 1, "work", 50, 100));
    auto r = process_only(d);
    const auto w = record(r, "main;work");
    CHECK(num(w, "/args/counters/PAPI_TOT_CYC/sum") == doctest::Approx(1500));
    CHECK(num(record(r, "main"), "/args/counters/PAPI_TOT_CYC/sum") ==
          doctest::Approx(3000));
    simdjson::dom::parser p;
    std::string_view scope;
    REQUIRE(at(p, w, "/args/counters/PAPI_TOT_CYC/scope").get(scope) ==
            simdjson::SUCCESS);
    CHECK(scope == "pid");
}

TEST_CASE("host count-like counters are split across the ranks of a host") {
    ScopedTestDir d("genesis_net");
    std::string t =
        host("net", "net-  hsi0", 0, R"("bytes_sent":0)") +
        host("net", "net-  hsi0", 1000, R"("bytes_sent":100000000)");
    for (int pid = 1; pid <= 4; ++pid)
        t += rank(pid, 0, 1000) + call(pid, pid, "ForceMult", 0, 1000);
    matrix_run(d, 4, t);
    auto r = process_only(d);
    const auto f = record(r, "ForceMult");
    CHECK(num(f, "/args/counters/net-hsi0.bytes_sent/sum") ==
          doctest::Approx(100000000));
}

TEST_CASE("host levels are time-weighted and per-core cpu is dropped") {
    ScopedTestDir d("genesis_cpu");
    matrix_run(d, 1,
               rank(1, 0, 1000) + host("sys", "cpu", 0, R"("user_pct":99)") +
                   host("sys", "cpu", 30, R"("user_pct":10)") +
                   host("sys", "cpu", 70, R"("user_pct":40)") +
                   host("sys", "cpu-0", 30, R"("user_pct":5)") +
                   host("sys", "cpu-0", 70, R"("user_pct":5)") +
                   call(1, 1, "work", 0, 40));
    auto r = process_only(d);
    const auto w = record(r, "work");
    CHECK(num(w, "/args/counters/cpu.user_pct/min") == doctest::Approx(17.5));
    CHECK_FALSE(has(w, "/args/counters/cpu.user_pct/sum"));
    CHECK_FALSE(has(w, "/args/counters/cpu-0.user_pct"));
}

TEST_CASE("matrix GPU CSV values become per-GPU gauges") {
    ScopedTestDir d("genesis_csv");
    const fs::path dir = matrix_run(
        d, 1,
        R"({"name":"HH","cat":"dftracer","pid":1,"tid":1,"ph":4,"args":{"hhash":"h1","name":"node1","value":"h1"}})"
        "\n" +
            rank(1, 0, 1000) + call(1, 1, "work", 0, 200));
    write_text(
        dir / "raw" / "set1" / "dftracer_service" / "gpu_power_node1.csv",
        "0.000000000,node1,0, 50.00, 0, 0\n"
        "0.000100000,node1,0, 60.00, 10, 512\n"
        "0.000200000,node1,0, 80.00, 30, 1024\n");
    auto r = process_only(d);
    const auto w = record(r, "work");
    CHECK(num(w, "/args/counters/gpu.power.GPU_0/avg") == doctest::Approx(70));
    CHECK(num(w, "/args/counters/gpu.utilization.GPU_0/avg") ==
          doctest::Approx(20));
    CHECK(num(w, "/args/counters/gpu.memory_used.GPU_0/avg") ==
          doctest::Approx(768));
}

TEST_CASE("GPU events nest under the host thread named by args.tid") {
    ScopedTestDir d("genesis_gpu");
    matrix_run(
        d, 1,
        rank(1, 0, 1000) + call(1, 11, "operator()", 100, 200) +
            call(1, 99, "kern", 150, 10, "KERNEL_DISPATCH", R"(,"tid":11)"));
    auto r = process_only(d);
    CHECK_FALSE(record(r, "operator();kern").empty());
}

TEST_CASE("async GPU events attach under the host call at their start") {
    ScopedTestDir d("genesis_async");
    matrix_run(
        d, 1,
        rank(1, 0, 1000) + call(1, 11, "main", 0, 1000) +
            call(1, 99, "MemcpyDtoH", 100, 100, "CUDA_MEMCPY", R"(,"tid":11)") +
            call(1, 98, "StreamSynchronize", 150, 100, "CUDA_SYNC",
                 R"(,"tid":11)"));
    auto r = process_only(d);
    REQUIRE(r.skips.empty());
    CHECK(num(record(r, "main;MemcpyDtoH"), "/args/depth") == 1);
    CHECK(num(record(r, "main;StreamSynchronize"), "/args/depth") == 1);
}

TEST_CASE("a GPU tracer thread attaches to the rank's main thread") {
    ScopedTestDir d("genesis_cupti");
    matrix_run(d, 1,
               rank(1, 0, 1000) + call(1, 1, "main", 0, 1000) +
                   call(1, 7, "MemcpyDtoH", 100, 100, "CUDA_MEMCPY",
                        R"(,"correlation_id":5)") +
                   call(1, 7, "ContextSynchronize", 150, 100, "CUDA_SYNC",
                        R"(,"correlation_id":6)") +
                   call(1, 7, "OverheadUnknown", 120, 200, "CUDA_OVERHEAD"));
    auto r = process_only(d);
    REQUIRE(r.skips.empty());
    CHECK_FALSE(record(r, "main;MemcpyDtoH").empty());
    CHECK_FALSE(record(r, "main;ContextSynchronize").empty());
    CHECK_FALSE(record(r, "main;OverheadUnknown").empty());
}

TEST_CASE("events outside the process window are depth-0 duration records") {
    ScopedTestDir d("genesis_window");
    matrix_run(d, 1,
               rank(1, 1000, 2000) + papi(1, 1000, "PAPI_TOT_CYC", 0) +
                   papi(1, 2000, "PAPI_TOT_CYC", 5) +
                   call(1, 1, "main", 1000, 1000) +
                   call(1, 2, "cudaMalloc", 5, 3, "CUDA_RUNTIME_API"));
    auto r = process_only(d);
    const auto c = record(r, "cudaMalloc");
    REQUIRE(!c.empty());
    CHECK(num(c, "/args/depth") == 0);
    CHECK(c.find(R"("counters":{})") != std::string::npos);
}

TEST_CASE("a truncated trace file skips the run and names the file") {
    ScopedTestDir d("genesis_trunc");
    std::string t = rank(1, 0, 1000);
    for (int i = 0; i < 2000; ++i) t += call(1, 1, "w", i * 10, 5);
    const fs::path dir = matrix_run(d, 1, t);
    const fs::path f = dir / "compacted" / "set1.pfw.gz";
    fs::resize_file(f, fs::file_size(f) / 2);
    auto r = process_only(d);
    CHECK(r.runs.empty());
    CHECK(skipped_with(r, f.string()));
}

TEST_CASE("missing process markers or processes skip the run") {
    ScopedTestDir a("genesis_noend");
    matrix_run(a, 2,
               rank(1, 0, 1000) + marker(2, "start", 0) +
                   call(1, 1, "m", 0, 10) + call(2, 2, "m", 0, 10));
    CHECK(skipped_with(process_only(a), "lacks dftracer start or end"));

    ScopedTestDir b("genesis_noproc");
    matrix_run(b, 2, rank(1, 0, 1000) + call(1, 1, "m", 0, 10));
    CHECK(skipped_with(process_only(b), "expected 2 processes, found 1"));
}

TEST_CASE("a child that ends after its parent skips the run") {
    ScopedTestDir d("genesis_overlap");
    matrix_run(
        d, 1,
        rank(1, 0, 1000) + call(1, 1, "p", 0, 100) + call(1, 1, "c", 50, 100));
    CHECK(skipped_with(process_only(d), "ends after its parent"));
}

TEST_CASE("tioga v2 processes are assigned to sets by PAPI counters") {
    auto make = [](const ScopedTestDir& d, const std::string& extra) {
        const fs::path dir =
            d.path() / "l" / "tioga" / "tioga" / "c" / "nodes_1" / "ppn_1";
        write_text(dir / "summary.json",
                   R"({"application":"l","case":"c","nodes":1,"ppn":1,)"
                   R"("runs":{"sA":{"papi_counters":["PAPI_A"]},)"
                   R"("sB":{"papi_counters":["PAPI_B"]}}})");
        write_gz(dir / "compacted" / "c_N1_ppn1-1_chunk0.pfw.gz",
                 rank(1, 0, 100) + papi(1, 0, "PAPI_A", 0) +
                     call(1, 1, "a", 0, 10) + rank(2, 200, 300) +
                     papi(2, 200, "PAPI_B", 0) + call(2, 2, "b", 200, 10) +
                     extra);
    };
    ScopedTestDir ok("genesis_sets");
    make(ok, "");
    auto disc = discover_dir(ok);
    REQUIRE(disc.groups.size() == 1);
    auto r = process(disc.groups[0]);
    REQUIRE(r.skips.empty());
    REQUIRE(r.runs.size() == 2);
    CHECK(r.runs[0].lines.find(R"("papi_set":"sA")") != std::string::npos);
    CHECK(r.runs[0].lines.find(R"("path":"a")") != std::string::npos);
    CHECK(r.runs[1].lines.find(R"("path":"b")") != std::string::npos);

    ScopedTestDir bad("genesis_sets_bad");
    make(bad, rank(3, 0, 10) + papi(3, 0, "PAPI_C", 0));
    auto disc2 = discover_dir(bad);
    auto r2 = process(disc2.groups.at(0));
    CHECK(r2.runs.empty());
    CHECK(skipped_with(r2, "match no set"));
}

TEST_CASE("percentiles are within the sketch accuracy") {
    ScopedTestDir d("genesis_sketch");
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> dist(1, 10000);
    std::vector<int> durs;
    std::string t = rank(1, 0, 400000000);
    for (int i = 0; i < 10000; ++i) {
        durs.push_back(dist(rng));
        t += call(1, 1, "w", static_cast<std::int64_t>(i) * 20000, durs.back());
    }
    matrix_run(d, 1, t);
    auto r = process_only(d);
    std::sort(durs.begin(), durs.end());
    const double exact = durs[durs.size() / 2];
    CHECK(std::abs(num(record(r, "w"), "/args/dur/p50") - exact) / exact <=
          SKETCH_ACCURACY + 1e-9);
}

TEST_CASE("the RUN line carries keys, settings and the summary") {
    ScopedTestDir d("genesis_runline");
    matrix_run(d, 1, rank(1, 0, 10) + call(1, 1, "m", 0, 5));
    auto r = process_only(d);
    REQUIRE(r.runs.size() == 1);
    const auto first = lines_of(r.runs[0].lines).at(0);
    CHECK(num(first, "/ph") == 4);
    CHECK(num(first, "/args/sketch_accuracy") == doctest::Approx(0.01));
    CHECK(has(first, "/args/summary/sets"));
    CHECK(first.find(R"("run":")" +
                     run_id({"app1", "sys1", "in1", 1, 1, "set1"}) + "\"") !=
          std::string::npos);
}

TEST_CASE("processing the same group twice gives identical output") {
    ScopedTestDir d("genesis_det");
    matrix_run(d, 2,
               rank(1, 0, 1000) + rank(2, 0, 1000) +
                   papi(1, 0, "PAPI_TOT_CYC", 0) +
                   papi(1, 500, "PAPI_TOT_CYC", 7) +
                   host("io", "nvme0n1", 0, R"("bytes_read":0)") +
                   host("io", "nvme0n1", 500, R"("bytes_read":64)") +
                   call(1, 1, "main", 0, 900) + call(2, 2, "main", 0, 900) +
                   call(1, 1, "x", 10, 20));
    auto a = process_only(d);
    auto b = process_only(d);
    REQUIRE(a.runs.size() == 1);
    CHECK(a.runs[0].lines == b.runs[0].lines);
}

TEST_CASE("an unknown host counter category skips the run") {
    ScopedTestDir d("genesis_hostcat");
    matrix_run(d, 1,
               rank(1, 0, 1000) + host("fan", "fan0", 0, R"("rpm":1)") +
                   call(1, 1, "m", 0, 10));
    CHECK(skipped_with(process_only(d), "unknown host counter category fan"));
}

TEST_CASE("a trace file name that matches no layout is reported") {
    ScopedTestDir d("genesis_badname");
    const fs::path c = d.path() / "minife" / "tioga" / "tioga" / "input_50" /
                       "nodes_1" / "ppn_1" / "compacted";
    write_gz(c / "trace.pfw.gz", "[\n");
    auto disc = discover_dir(d);
    CHECK(disc.groups.empty());
    REQUIRE(disc.skips.size() == 1);
    CHECK(disc.skips[0].reason == "unrecognized trace file name");
}
