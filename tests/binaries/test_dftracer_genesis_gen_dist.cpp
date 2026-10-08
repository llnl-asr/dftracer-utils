#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/view.h>
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

std::string call(int pid, const std::string& name, int ts, int dur,
                 const std::string& cat = "CPP_APP") {
    std::ostringstream o;
    o << R"({"name":")" << name << R"(","cat":")" << cat << R"(","pid":)" << pid
      << R"(,"tid":)" << pid << R"(,"ts":)" << ts << R"(,"dur":)" << dur
      << R"(,"ph":1,"args":{"hhash":"h1"}})"
      << "\n";
    return o.str();
}

// One complete matrix-layout run under `root` with a single set.
fs::path write_run(const fs::path& root, const std::string& input) {
    const fs::path dir = root / "app" / "sys" / input / "nodes_1" / "ppn_1";
    fs::create_directories(dir / "compacted");
    std::ofstream(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":")" << input
        << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
    std::string t = "[\n" + call(1, "start", 0, 0, "dftracer") +
                    call(1, "end", 100000, 0, "dftracer");
    for (int i = 0; i < 500; ++i) t += call(1, "work", i * 100, 50);
    dftu_utils_test::write_gz_trace(
        (dir / "compacted" / "set1.pfw.gz").string(), t);
    return dir / "compacted" / "set1.pfw.gz";
}

// A run with `paths` distinct call names, so records outnumber its run record.
void write_wide_run(const fs::path& root, const std::string& input, int paths) {
    const fs::path dir = root / "app" / "sys" / input / "nodes_1" / "ppn_1";
    fs::create_directories(dir / "compacted");
    std::ofstream(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":")" << input
        << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
    std::string t = "[\n" + call(1, "start", 0, 0, "dftracer") +
                    call(1, "end", 1000000, 0, "dftracer");
    for (int i = 0; i < 400; ++i)
        t += call(1, "f" + std::to_string(i % paths), i * 1000, 500);
    dftu_utils_test::write_gz_trace(
        (dir / "compacted" / "set1.pfw.gz").string(), t);
}

std::vector<std::string> gz_lines(const std::string& path) {
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
    return out;
}

std::vector<std::size_t> member_sizes(const std::string& path) {
    const std::string gz = [&] {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();
    std::vector<std::size_t> sizes;
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(gz.data()));
    zs.avail_in = static_cast<uInt>(gz.size());
    std::vector<unsigned char> out(1 << 16);
    while (zs.avail_in > 0) {
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        const int rc = inflate(&zs, Z_NO_FLUSH);
        REQUIRE((rc == Z_OK || rc == Z_STREAM_END));
        if (rc == Z_STREAM_END) {
            sizes.push_back(zs.total_out);
            REQUIRE(inflateReset(&zs) == Z_OK);
        }
    }
    inflateEnd(&zs);
    return sizes;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

int run(const std::vector<std::string>& args, std::string* out = nullptr) {
    int code = -1;
    const std::string text =
        dftu_utils_test::run_process_capture(binary(), args, true, &code);
    if (out) *out = text;
    return code;
}

}  // namespace

TEST_CASE("a truncated run is skipped and the others are written") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_trunc");
    write_run(d.path() / "root", "good");
    const fs::path bad = write_run(d.path() / "root", "bad");
    fs::resize_file(bad, fs::file_size(bad) / 2);
    std::string text;
    CHECK(run({(d.path() / "root").string(), "-o", d.file("out.pfw.gz")},
              &text) == 1);
    CHECK(text.find("Wrote 1 run(s)") != std::string::npos);
    CHECK(text.find(bad.string()) != std::string::npos);
    const auto lines = gz_lines(d.file("out.pfw.gz"));
    REQUIRE(!lines.empty());
    CHECK(lines[0].find(R"("unique_input":"good")") != std::string::npos);
}

TEST_CASE("the output path must end with .pfw.gz") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_ext");
    write_run(d.path(), "a");
    CHECK(run({d.str(), "-o", d.file("out.jsonl")}) != 0);
    CHECK_FALSE(fs::exists(d.file("out.jsonl")));
}

TEST_CASE(
    "output holds the same lines every time and concatenates like one run") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_det");
    write_run(d.path() / "a", "x");
    write_run(d.path() / "b", "y");
    const std::string a = (d.path() / "a").string();
    const std::string b = (d.path() / "b").string();
    CHECK(run({a, b, "-o", d.file("ab1.pfw.gz")}) == 0);
    CHECK(run({a, b, "-o", d.file("ab2.pfw.gz")}) == 0);
    // Workers write their runs as they finish, so only the set of lines is
    // fixed, not their order in the file.
    auto first = gz_lines(d.file("ab1.pfw.gz"));
    auto second = gz_lines(d.file("ab2.pfw.gz"));
    std::sort(first.begin(), first.end());
    std::sort(second.begin(), second.end());
    CHECK(first == second);

    CHECK(run({a, "-o", d.file("a.pfw.gz")}) == 0);
    CHECK(run({b, "-o", d.file("b.pfw.gz")}) == 0);
    std::ofstream(d.file("cat.pfw.gz"), std::ios::binary)
        << slurp(d.file("a.pfw.gz")) << slurp(d.file("b.pfw.gz"));
    auto joined = gz_lines(d.file("cat.pfw.gz"));
    auto together = gz_lines(d.file("ab1.pfw.gz"));
    std::sort(joined.begin(), joined.end());
    std::sort(together.begin(), together.end());
    CHECK(joined == together);
}

TEST_CASE("members follow the member size, not the run") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_member");
    write_wide_run(d.path() / "root", "w", 300);
    const std::string root = (d.path() / "root").string();
    constexpr std::size_t MEMBER = 2048;
    CHECK(run({root, "-o", d.file("small.pfw.gz"), "--member-size",
               std::to_string(MEMBER)}) == 0);
    CHECK(run({root, "-o", d.file("big.pfw.gz")}) == 0);

    auto small = gz_lines(d.file("small.pfw.gz"));
    auto big = gz_lines(d.file("big.pfw.gz"));
    std::sort(small.begin(), small.end());
    std::sort(big.begin(), big.end());
    CHECK(small == big);
    std::size_t longest = 0;
    for (const auto& l : small) longest = std::max(longest, l.size() + 1);
    const auto sizes = member_sizes(d.file("small.pfw.gz"));
    CHECK(sizes.size() > 1);
    for (std::size_t n : sizes) CHECK(n <= MEMBER + longest);
    CHECK(member_sizes(d.file("big.pfw.gz")).size() == 1);
}

TEST_CASE("records carry the run id and resolve run keys through the index") {
    namespace ix = dftracer::utils::index;
    using dftracer::utils::trace::views::View;
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_run_dict");
    write_wide_run(d.path() / "root", "x", 20);
    write_wide_run(d.path() / "root", "y", 20);
    const std::string out = d.file("out.pfw.gz");
    REQUIRE(run({(d.path() / "root").string(), "-o", out}) == 0);

    std::size_t records = 0;
    for (const auto& line : gz_lines(out)) {
        CHECK(line.find(R"("ph":)") == std::string::npos);
        CHECK(line.find(R"("args":)") == std::string::npos);
        if (line.rfind(R"({"gtype":"run",)", 0) == 0) continue;
        ++records;
        CHECK(line.find(R"("run":")") != std::string::npos);
        CHECK(line.find(R"("unique_input")") == std::string::npos);
        CHECK(line.find(R"("papi_set")") == std::string::npos);
    }
    REQUIRE(records > 0);

    ix::IndexerOptions o;
    o.index_dir = d.str();
    auto indexer = ix::Indexer::open({out}, o);
    indexer.build();
    REQUIRE(indexer.files().size() == 1);
    CHECK(indexer.files()[0].schema == "genesis");

    const std::string index_path = d.file(".dftindex");
    auto rows = [&](const std::string& q) {
        return View::from_file(out, index_path)
            .duql(q)
            .collect()
            .get()
            .num_rows();
    };
    const std::size_t x = rows(R"(run -> runs.unique_input == "x")");
    const std::size_t y = rows(R"(run -> runs.unique_input == "y")");
    CHECK(x > 0);
    CHECK(x + y == records);
    CHECK(rows("run -> runs.nodes == 1") == records);
    CHECK(rows(R"(run -> runs.papi_set == "set1")") == records);

    const auto df = View::from_file(out, index_path)
                        .duql(
                            "derive u = run -> runs.unique_input | select "
                            "path, u")
                        .collect()
                        .get();
    const auto& col = df.columns[df.column_index("u")];
    REQUIRE(col.length() > 0);
    for (std::int64_t i = 0; i < col.length(); ++i) {
        const std::string v(col.string_at(i));
        CHECK((v == "x" || v == "y"));
    }

    const auto runs = View::from_file(out, index_path)
                          .duql("from runs | where true")
                          .collect()
                          .get();
    CHECK(runs.num_rows() == 2);
    for (const char* name :
         {"run", "app", "system", "unique_input", "nodes", "ppn", "papi_set",
          "method", "sketch_accuracy", "leaf"}) {
        CAPTURE(name);
        CHECK(runs.column_index(name) >= 0);
    }

    CHECK(rows(R"(gtype == "run")") == 0);
    const auto run_records = View::from_file(out, index_path)
                                 .duql(R"(from all | where gtype == "run")")
                                 .collect()
                                 .get();
    CHECK(run_records.num_rows() == 2);
    CHECK(run_records.column_index("summary.ok") >= 0);
}

TEST_CASE("a dftracer trace is still detected as dftracer") {
    namespace ix = dftracer::utils::index;
    ScopedTestDir d("gen_dist_detect_dft");
    const std::string trace = d.file("t.pfw.gz");
    dftu_utils_test::write_gz_trace(
        trace, R"({"id":1,"name":"read","cat":"POSIX","pid":1,"tid":1,)"
               R"("ts":1,"dur":5,"ph":"X"})"
               "\n");
    ix::IndexerOptions o;
    o.index_dir = d.str();
    auto indexer = ix::Indexer::open({trace}, o);
    indexer.build();
    REQUIRE(indexer.files().size() == 1);
    CHECK(indexer.files()[0].schema == "dftracer");
}
