#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using dftu_utils_test::ScopedTestDir;

namespace {

std::string binary() {
    static const std::string path = dftu_utils_test::find_binary_by_name(
        "DFTRACER_GENESIS_GEN_DIST_PATH", "dftracer_genesis_gen_dist");
    return path;
}

std::string call(const std::string& name, int ts, int dur,
                 const std::string& cat = "CPP_APP") {
    std::ostringstream o;
    o << R"({"name":")" << name << R"(","cat":")" << cat
      << R"(","pid":1,"tid":1,"ts":)" << ts << R"(,"dur":)" << dur
      << R"(,"ph":1,"args":{"hhash":"h1"}})"
      << "\n";
    return o.str();
}

void write_run(const fs::path& root, const std::string& input, int calls) {
    const fs::path dir = root / "app" / "sys" / input / "nodes_1" / "ppn_1";
    fs::create_directories(dir / "compacted");
    std::ofstream(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":")" << input
        << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
    std::string t = "[\n" + call("start", 0, 0, "dftracer") +
                    call("end", calls * 100 + 100, 0, "dftracer");
    for (int i = 0; i < calls; ++i)
        t += call("f" + std::to_string(i % 7), i * 100, 50);
    dftu_utils_test::write_gz_trace(
        (dir / "compacted" / "set1.pfw.gz").string(), t);
}

std::vector<std::string> sorted_gz_lines(const std::string& path) {
    std::vector<std::string> out;
    gzFile f = gzopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::string text;
    char buf[65536];
    int n = 0;
    while ((n = gzread(f, buf, sizeof(buf))) > 0) text.append(buf, n);
    gzclose(f);
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l))
        if (!l.empty()) out.push_back(l);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST_CASE("ranks split the run groups and join one output") {
    const std::string launcher = dftu_utils_test::find_mpi_launcher();
    if (binary().empty() || launcher.empty()) {
        MESSAGE("skipped: no dftracer_genesis_gen_dist or MPI launcher");
        return;
    }
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_mpi");
    const fs::path root = d.path() / "root";
    for (int i = 0; i < 6; ++i)
        write_run(root, "in" + std::to_string(i), 200 * (i + 1));

    REQUIRE(dftu_utils_test::run_mpi(
                launcher, 1, binary(),
                {root.string(), "-o", d.file("one.pfw.gz")}) == 0);
    REQUIRE(dftu_utils_test::run_mpi(
                launcher, 3, binary(),
                {root.string(), "-o", d.file("three.pfw.gz")}) == 0);
    const auto one = sorted_gz_lines(d.file("one.pfw.gz"));
    REQUIRE_FALSE(one.empty());
    CHECK(sorted_gz_lines(d.file("three.pfw.gz")) == one);
    for (int r = 0; r < 3; ++r)
        CHECK_FALSE(
            fs::exists(d.file("three.pfw.gz." + std::to_string(r) + ".part")));
}

TEST_CASE("--shard is refused with more than one rank") {
    const std::string launcher = dftu_utils_test::find_mpi_launcher();
    if (binary().empty() || launcher.empty()) {
        MESSAGE("skipped: no dftracer_genesis_gen_dist or MPI launcher");
        return;
    }
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_mpi_shard");
    write_run(d.path() / "root", "in", 100);
    CHECK(dftu_utils_test::run_mpi(launcher, 2, binary(),
                                   {(d.path() / "root").string(), "-o",
                                    d.file("o.pfw.gz"), "--shard", "0/2"}) !=
          0);
}
