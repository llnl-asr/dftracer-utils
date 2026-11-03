// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

using namespace dftracer::utils;

using dftu_utils_test::ScopedTestDir;

namespace {

dstd::String binary() {
    static const dstd::String path = dftu_utils_test::find_binary_by_name(
        "DFTRACER_UTILS_GENESIS_GEN_DIST_PATH", "dftracer_genesis_gen_dist");
    return path;
}

dstd::String call(const dstd::String& name, int ts, int dur,
                  const dstd::String& cat = "CPP_APP") {
    dftu_utils_test::TextOut o;
    o << R"({"name":")" << name << R"(","cat":")" << cat
      << R"(","pid":1,"tid":1,"ts":)" << ts << R"(,"dur":)" << dur
      << R"(,"ph":1,"args":{"hhash":"h1"}})"
      << "\n";
    return o.str();
}

void write_run(const dstd::Path& root, const dstd::String& input, int calls) {
    const dstd::Path dir = root / "app" / "sys" / input / "nodes_1" / "ppn_1";
    REQUIRE(dstd::fs::create_directories(dir / "compacted").has_value());
    dftu_utils_test::FileOut(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":")" << input
        << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
    dstd::String t = "[\n" + call("start", 0, 0, "dftracer") +
                     call("end", calls * 100 + 100, 0, "dftracer");
    for (int i = 0; i < calls; ++i)
        t += call("f" + dstd::to_string(i % 7), i * 100, 50);
    dftu_utils_test::write_gz_trace(
        dstd::String((dir / "compacted" / "set1.pfw.gz").str()), t);
}

dstd::Vector<dstd::String> sorted_gz_lines(const dstd::String& path) {
    dstd::Vector<dstd::String> out;
    gzFile f = gzopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    dstd::String text;
    char buf[65536];
    int n = 0;
    while ((n = gzread(f, buf, sizeof(buf))) > 0) text.append(buf, n);
    gzclose(f);
    dftu_utils_test::LineStream in(text);
    dstd::String l;
    while (in.next(l))
        if (!l.empty()) out.push_back(l);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("ranks split the run groups and join one output") {
    const dstd::String launcher = dftu_utils_test::find_mpi_launcher();
    if (binary().empty() || launcher.empty()) {
        MESSAGE("skipped: no dftracer_genesis_gen_dist or MPI launcher");
        return;
    }
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_mpi");
    const dstd::Path root = d.path() / "root";
    for (int i = 0; i < 6; ++i)
        write_run(root, "in" + dstd::to_string(i), 200 * (i + 1));

    REQUIRE(dftu_utils_test::run_mpi(
                launcher, 1, binary(),
                {dstd::String(root.str()), "-o", d.file("one.pfw.gz")}) == 0);
    REQUIRE(dftu_utils_test::run_mpi(
                launcher, 3, binary(),
                {dstd::String(root.str()), "-o", d.file("three.pfw.gz")}) == 0);
    const auto one = sorted_gz_lines(d.file("one.pfw.gz"));
    REQUIRE_FALSE(one.empty());
    CHECK(sorted_gz_lines(d.file("three.pfw.gz")) == one);
    for (int r = 0; r < 3; ++r)
        CHECK_FALSE(dstd::fs::exists(
                        d.file("three.pfw.gz." + dstd::to_string(r) + ".part"))
                        .value());
}

TEST_CASE("--shard is refused with more than one rank") {
    const dstd::String launcher = dftu_utils_test::find_mpi_launcher();
    if (binary().empty() || launcher.empty()) {
        MESSAGE("skipped: no dftracer_genesis_gen_dist or MPI launcher");
        return;
    }
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_mpi_shard");
    write_run(d.path() / "root", "in", 100);
    CHECK(
        dftu_utils_test::run_mpi(launcher, 2, binary(),
                                 {dstd::String((d.path() / "root").str()), "-o",
                                  d.file("o.pfw.gz"), "--shard", "0/2"}) != 0);
}
