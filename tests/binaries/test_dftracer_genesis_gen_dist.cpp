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

// A run of `depth` nested calls with long names: its path records hold about
// depth * depth * 13 bytes of path text, from a small trace file.
void write_deep_run(const fs::path& root, const std::string& input, int depth) {
    const fs::path dir = root / "app" / "sys" / input / "nodes_1" / "ppn_1";
    fs::create_directories(dir / "compacted");
    std::ofstream(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":")" << input
        << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
    std::string t = "[\n" + call(1, "start", 0, 0, "dftracer") +
                    call(1, "end", 1000000, 0, "dftracer");
    for (int i = 0; i < depth; ++i) {
        std::string name = "deep_call_" + std::to_string(i);
        name.resize(24, '_');
        t += call(1, name, i, 2 * (depth - i));
    }
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
              &text) == 0);
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

TEST_CASE("a run over its share is still written") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_retry");
    const fs::path root = d.path() / "root";
    for (int i = 0; i < 6; ++i) write_run(root, "small" + std::to_string(i));
    // About 25 MB of path text: over the state of a 70 MB share, so the run
    // borrows free memory of the 140 MB for groups of a 160 MB budget or runs
    // again with a larger share.
    write_deep_run(root, "deep", 1400);
    std::string text;
    CHECK(run({root.string(), "-o", d.file("out.pfw.gz"), "--memory-budget",
               "160MB", "--executor-threads", "2"},
              &text) == 0);
    CHECK(text.find("Wrote 7 run(s)") != std::string::npos);
    CHECK(text.find("skipped 0") != std::string::npos);
    std::size_t deep_paths = 0;
    for (const auto& l : gz_lines(d.file("out.pfw.gz")))
        if (l.find(R"("gtype":"func")") != std::string::npos &&
            l.find("deep_call_") != std::string::npos)
            ++deep_paths;
    CHECK(deep_paths == 1400);
}

TEST_CASE("the run and func records follow every counter record") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_layout");
    for (const std::string input : {"a", "b", "c"}) {
        const fs::path dir =
            d.path() / "root" / "app" / "sys" / input / "nodes_1" / "ppn_1";
        fs::create_directories(dir / "compacted");
        std::ofstream(dir / "summary.json")
            << R"({"ok":true,"app":"app","system":"sys","input":")" << input
            << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
        std::string t = "[\n" + call(1, "start", 0, 0, "dftracer") +
                        call(1, "end", 100000, 0, "dftracer");
        for (int i = 0; i < 200; ++i) {
            std::ostringstream o;
            o << R"({"name":"PAPI","cat":"papi","pid":1,"tid":1,"ts":)"
              << i * 500
              << R"(,"ph":2,"args":{"hhash":"h1","multiplex":0,"PAPI_TOT_CYC":1,)"
                 R"("PAPI_TOT_CYC_delta":)"
              << 10 * i << "}}\n";
            t += o.str() + call(1, "f" + std::to_string(i % 40), i * 500, 200);
        }
        dftu_utils_test::write_gz_trace(
            (dir / "compacted" / "set1.pfw.gz").string(), t);
    }
    const std::string out = d.file("out.pfw.gz");
    REQUIRE(run({(d.path() / "root").string(), "-o", out, "--member-size",
                 "4096"}) == 0);
    const auto lines = gz_lines(out);
    const auto first_head =
        std::find_if(lines.begin(), lines.end(), [](const std::string& l) {
            return l.find(R"("gtype":"counter")") == std::string::npos;
        });
    REQUIRE(first_head != lines.begin());
    REQUIRE(first_head != lines.end());
    std::size_t runs = 0;
    std::size_t funcs = 0;
    for (auto it = first_head; it != lines.end(); ++it) {
        CHECK(it->find(R"("gtype":"counter")") == std::string::npos);
        runs += it->rfind(R"({"gtype":"run",)", 0) == 0;
        funcs += it->rfind(R"({"gtype":"func",)", 0) == 0;
    }
    CHECK(runs == 3);
    CHECK(funcs == 3 * 40);
    CHECK(fs::exists(out + ".head") == false);
}

TEST_CASE("the shards of a sweep hold each run once") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_shard");
    const fs::path root = d.path() / "root";
    for (int i = 0; i < 5; ++i) write_run(root, "in" + std::to_string(i));
    const fs::path off = root / "app" / "sys" / "off" / "nodes_1" / "ppn_1";
    fs::create_directories(off);
    std::ofstream(off / "summary.json") << R"({"ok":false})";

    std::string full_text;
    REQUIRE(run({root.string(), "-o", d.file("full.pfw.gz")}, &full_text) == 0);
    std::vector<std::string> parts;
    std::string texts[2];
    for (int i = 0; i < 2; ++i) {
        const std::string out = d.file("s" + std::to_string(i) + ".pfw.gz");
        REQUIRE(
            run({root.string(), "-o", out, "--shard", std::to_string(i) + "/2"},
                &texts[i]) == 0);
        const auto lines = gz_lines(out);
        CHECK_FALSE(lines.empty());
        parts.insert(parts.end(), lines.begin(), lines.end());
    }
    auto full = gz_lines(d.file("full.pfw.gz"));
    std::sort(full.begin(), full.end());
    std::sort(parts.begin(), parts.end());
    CHECK(parts == full);
    // Discovery skips are reported by the first shard only.
    CHECK(full_text.find("neither sets nor runs key") != std::string::npos);
    CHECK(texts[0].find("neither sets nor runs key") != std::string::npos);
    CHECK(texts[1].find("neither sets nor runs key") == std::string::npos);
}

TEST_CASE("a shard outside i/N with 0 <= i < N is refused") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_bad_shard");
    write_run(d.path() / "root", "in");
    for (const char* bad : {"2/2", "0/0", "1", "a/2", "1/2/3", "-1/2", "/2"}) {
        CAPTURE(bad);
        CHECK(run({(d.path() / "root").string(), "-o", d.file("o.pfw.gz"),
                   "--shard", bad}) != 0);
    }
}

TEST_CASE("the thread count does not change the output") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    ScopedTestDir d("gen_dist_threads");
    const fs::path dir =
        d.path() / "root" / "app" / "sys" / "in" / "nodes_1" / "ppn_4";
    fs::create_directories(dir / "compacted");
    std::ofstream(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":"in","nodes":1,)"
           R"("ppn":4,"sets":["set1"]})";
    std::string t = "[\n";
    for (int pid = 1; pid <= 4; ++pid) {
        t += call(pid, "start", 0, 0, "dftracer");
        t += call(pid, "end", 400000, 0, "dftracer");
        for (int i = 0; i < 2000; ++i)
            t += call(pid, "f" + std::to_string((i * pid) % 13), i * 100,
                      30 + (i * 7 + pid) % 60);
    }
    dftu_utils_test::write_gz_trace(
        (dir / "compacted" / "set1.pfw.gz").string(), t);
    std::vector<std::string> outs[2];
    const char* threads[2] = {"1", "4"};
    for (int k = 0; k < 2; ++k) {
        const std::string out =
            d.file(std::string("o") + threads[k] + ".pfw.gz");
        REQUIRE(run({(d.path() / "root").string(), "-o", out,
                     "--executor-threads", threads[k]}) == 0);
        outs[k] = gz_lines(out);
        std::sort(outs[k].begin(), outs[k].end());
    }
    REQUIRE(outs[0].size() > 13);
    CHECK(outs[0] == outs[1]);
}
