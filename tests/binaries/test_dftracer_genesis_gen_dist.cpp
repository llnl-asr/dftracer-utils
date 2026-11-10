// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/trace/views/view.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

using namespace dftracer::utils;

namespace {

dstd::String binary() {
    static const dstd::String path = dftu_utils_test::find_binary_by_name(
        "DFTRACER_UTILS_GENESIS_GEN_DIST_PATH", "dftracer_genesis_gen_dist");
    return path;
}

dstd::String call(int pid, const dstd::String& name, int ts, int dur,
                  const dstd::String& cat = "CPP_APP") {
    dftu_utils_test::TextOut o;
    o << R"({"name":")" << name << R"(","cat":")" << cat << R"(","pid":)" << pid
      << R"(,"tid":)" << pid << R"(,"ts":)" << ts << R"(,"dur":)" << dur
      << R"(,"ph":1,"args":{"hhash":"h1"}})"
      << "\n";
    return o.str();
}

// One complete matrix-layout run under `root` with a single set.
dstd::Path write_run(const dstd::Path& root, const dstd::String& input) {
    const dstd::Path dir = root / "app" / "sys" / input / "nodes_1" / "ppn_1";
    REQUIRE(dstd::fs::create_directories(dir / "compacted").has_value());
    dftu_utils_test::FileOut(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":")" << input
        << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
    dstd::String t = "[\n" + call(1, "start", 0, 0, "dftracer") +
                     call(1, "end", 100000, 0, "dftracer");
    for (int i = 0; i < 500; ++i) t += call(1, "work", i * 100, 50);
    dftu_utils_test::write_gz_trace(
        dstd::String((dir / "compacted" / "set1.pfw.gz").str()), t);
    return dir / "compacted" / "set1.pfw.gz";
}

// A run with `paths` distinct call names, so records outnumber its run record.
void write_wide_run(const dstd::Path& root, const dstd::String& input,
                    int paths) {
    const dstd::Path dir = root / "app" / "sys" / input / "nodes_1" / "ppn_1";
    REQUIRE(dstd::fs::create_directories(dir / "compacted").has_value());
    dftu_utils_test::FileOut(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":")" << input
        << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
    dstd::String t = "[\n" + call(1, "start", 0, 0, "dftracer") +
                     call(1, "end", 1000000, 0, "dftracer");
    for (int i = 0; i < 400; ++i)
        t += call(1, "f" + dstd::to_string(i % paths), i * 1000, 500);
    dftu_utils_test::write_gz_trace(
        dstd::String((dir / "compacted" / "set1.pfw.gz").str()), t);
}

// A run of `depth` nested calls with long names: its path records hold about
// depth * depth * 13 bytes of path text, from a small trace file.
void write_deep_run(const dstd::Path& root, const dstd::String& input,
                    int depth) {
    const dstd::Path dir = root / "app" / "sys" / input / "nodes_1" / "ppn_1";
    REQUIRE(dstd::fs::create_directories(dir / "compacted").has_value());
    dftu_utils_test::FileOut(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":")" << input
        << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
    dstd::String t = "[\n" + call(1, "start", 0, 0, "dftracer") +
                     call(1, "end", 1000000, 0, "dftracer");
    for (int i = 0; i < depth; ++i) {
        dstd::String name = "deep_call_" + dstd::to_string(i);
        name.resize(24, '_');
        t += call(1, name, i, 2 * (depth - i));
    }
    dftu_utils_test::write_gz_trace(
        dstd::String((dir / "compacted" / "set1.pfw.gz").str()), t);
}

dstd::Vector<dstd::String> gz_lines(const dstd::String& path) {
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
    return out;
}

dstd::Vector<std::size_t> member_sizes(const dstd::String& path) {
    const dstd::String gz = dftu_utils_test::read_text(path);
    dstd::Vector<std::size_t> sizes;
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(gz.data()));
    zs.avail_in = static_cast<uInt>(gz.size());
    dstd::Vector<unsigned char> out(1 << 16);
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

dstd::String slurp(const dstd::String& path) {
    return dftu_utils_test::read_text(path);
}

int run(const dstd::Vector<dstd::String>& args, dstd::String* out = nullptr) {
    int code = -1;
    const dstd::String text =
        dftu_utils_test::run_process_capture(binary(), args, true, &code);
    if (out) *out = text;
    return code;
}

}  // namespace

TEST_CASE("a truncated run is skipped and the others are written") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    dftu_utils_test::ScopedTestDir d("gen_dist_trunc");
    write_run(d.path() / "root", "good");
    const dstd::Path bad = write_run(d.path() / "root", "bad");
    REQUIRE(dstd::fs::resize_file(bad, dstd::fs::file_size(bad).value() / 2)
                .has_value());
    dstd::String text;
    CHECK(run({d.file("root"), "-o", d.file("out.pfw.gz")}, &text) == 0);
    CHECK(text.find("Wrote 1 run(s)") != dstd::String::npos);
    CHECK(text.find(bad.str()) != dstd::String::npos);
    const auto lines = gz_lines(d.file("out.pfw.gz"));
    REQUIRE(!lines.empty());
    CHECK(lines[0].find(R"("unique_input":"good")") != dstd::String::npos);
}

TEST_CASE("the output path must end with .pfw.gz") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    dftu_utils_test::ScopedTestDir d("gen_dist_ext");
    write_run(d.path(), "a");
    CHECK(run({d.str(), "-o", d.file("out.jsonl")}) != 0);
    CHECK_FALSE(dstd::fs::exists(d.file("out.jsonl")).value());
}

TEST_CASE(
    "output holds the same lines every time and concatenates like one run") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    dftu_utils_test::ScopedTestDir d("gen_dist_det");
    write_run(d.path() / "a", "x");
    write_run(d.path() / "b", "y");
    const dstd::String a = dstd::String(d.file("a"));
    const dstd::String b = dstd::String(d.file("b"));
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
    dftu_utils_test::FileOut(d.file("cat.pfw.gz"))
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
    dftu_utils_test::ScopedTestDir d("gen_dist_member");
    write_wide_run(d.path() / "root", "w", 300);
    const dstd::String root = dstd::String(d.file("root"));
    constexpr std::size_t MEMBER = 2048;
    CHECK(run({root, "-o", d.file("small.pfw.gz"), "--member-size",
               dstd::to_string(MEMBER)}) == 0);
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
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    dftu_utils_test::ScopedTestDir d("gen_dist_run_dict");
    write_wide_run(d.path() / "root", "x", 20);
    write_wide_run(d.path() / "root", "y", 20);
    const dstd::String out = d.file("out.pfw.gz");
    REQUIRE(run({d.file("root"), "-o", out}) == 0);

    std::size_t records = 0;
    for (const auto& line : gz_lines(out)) {
        CHECK(line.find(R"("ph":)") == dstd::String::npos);
        CHECK(line.find(R"("args":)") == dstd::String::npos);
        if (line.rfind(R"({"gtype":"run",)", 0) == 0) continue;
        ++records;
        CHECK(line.find(R"("run":")") != dstd::String::npos);
        CHECK(line.find(R"("unique_input")") == dstd::String::npos);
        CHECK(line.find(R"("papi_set")") == dstd::String::npos);
    }
    REQUIRE(records > 0);

    index::IndexerOptions o;
    o.index_dir = d.str();
    auto indexer = dftu_utils_test::ok(
        index::Indexer::open({dstd::String(dstd::StringView(out))}, o));
    dftu_utils_test::ok(indexer.build());
    REQUIRE(dftu_utils_test::ok(indexer.files()).size() == 1);
    CHECK(dftu_utils_test::ok(indexer.files())[0].schema == "genesis");

    const dstd::String index_path = d.file(".dftindex");
    auto rows = [&](const dstd::String& q) {
        return dftu_utils_test::ok(
                   trace::views::View::from_file(dstd::String(out),
                                                 dstd::String(index_path))
                       .duql(q)
                       .collect()
                       .get())
            .num_rows();
    };
    const std::size_t x = rows(R"(run -> runs.unique_input == "x")");
    const std::size_t y = rows(R"(run -> runs.unique_input == "y")");
    CHECK(x > 0);
    CHECK(x + y == records);
    CHECK(rows("run -> runs.nodes == 1") == records);
    CHECK(rows(R"(run -> runs.papi_set == "set1")") == records);

    const auto df = dftu_utils_test::ok(
        trace::views::View::from_file(dstd::String(out),
                                      dstd::String(index_path))
            .duql("derive u = run -> runs.unique_input | select "
                  "path, u")
            .collect()
            .get());
    const auto& col = df.columns[df.column_index("u")];
    REQUIRE(col.length() > 0);
    for (std::int64_t i = 0; i < col.length(); ++i) {
        const dstd::String v(col.string_at(i));
        CHECK((v == "x" || v == "y"));
    }

    const auto runs =
        dftu_utils_test::ok(trace::views::View::from_file(
                                dstd::String(out), dstd::String(index_path))
                                .duql("from runs | where true")
                                .collect()
                                .get());
    CHECK(runs.num_rows() == 2);
    for (const char* name :
         {"run", "app", "system", "unique_input", "nodes", "ppn", "papi_set",
          "method", "sketch_accuracy", "leaf"}) {
        CAPTURE(name);
        CHECK(runs.column_index(name) >= 0);
    }

    CHECK(rows(R"(gtype == "run")") == 0);
    const auto run_records =
        dftu_utils_test::ok(trace::views::View::from_file(
                                dstd::String(out), dstd::String(index_path))
                                .duql(R"(from all | where gtype == "run")")
                                .collect()
                                .get());
    CHECK(run_records.num_rows() == 2);
    CHECK(run_records.column_index("summary.ok") >= 0);
}

TEST_CASE("a dftracer trace is still detected as dftracer") {
    dftu_utils_test::ScopedTestDir d("gen_dist_detect_dft");
    const dstd::String trace = d.file("t.pfw.gz");
    dftu_utils_test::write_gz_trace(
        trace, R"({"id":1,"name":"read","cat":"POSIX","pid":1,"tid":1,)"
               R"("ts":1,"dur":5,"ph":"X"})"
               "\n");
    index::IndexerOptions o;
    o.index_dir = d.str();
    auto indexer = dftu_utils_test::ok(
        index::Indexer::open({dstd::String(dstd::StringView(trace))}, o));
    dftu_utils_test::ok(indexer.build());
    REQUIRE(dftu_utils_test::ok(indexer.files()).size() == 1);
    CHECK(dftu_utils_test::ok(indexer.files())[0].schema == "dftracer");
}

TEST_CASE("a run over its share is still written") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    dftu_utils_test::ScopedTestDir d("gen_dist_retry");
    const dstd::Path root = d.path() / "root";
    for (int i = 0; i < 6; ++i) write_run(root, "small" + dstd::to_string(i));
    // About 25 MB of path text: over the state of a 70 MB share, so the run
    // borrows free memory of the 140 MB for groups of a 160 MB budget or runs
    // again with a larger share.
    write_deep_run(root, "deep", 1400);
    dstd::String text;
    CHECK(run({dstd::String(root.str()), "-o", d.file("out.pfw.gz"),
               "--memory-budget", "160MB", "--executor-threads", "2"},
              &text) == 0);
    CHECK(text.find("Wrote 7 run(s)") != dstd::String::npos);
    CHECK(text.find("skipped 0") != dstd::String::npos);
    std::size_t deep_paths = 0;
    for (const auto& l : gz_lines(d.file("out.pfw.gz")))
        if (l.find(R"("gtype":"func")") != dstd::String::npos &&
            l.find("deep_call_") != dstd::String::npos)
            ++deep_paths;
    CHECK(deep_paths == 1400);
}

TEST_CASE("the run and func records follow every counter record") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    dftu_utils_test::ScopedTestDir d("gen_dist_layout");
    for (const dstd::String input : {"a", "b", "c"}) {
        const dstd::Path dir =
            d.path() / "root" / "app" / "sys" / input / "nodes_1" / "ppn_1";
        dstd::fs::create_directories(dir / "compacted").has_value();
        dftu_utils_test::FileOut(dir / "summary.json")
            << R"({"ok":true,"app":"app","system":"sys","input":")" << input
            << R"(","nodes":1,"ppn":1,"sets":["set1"]})";
        dstd::String t = "[\n" + call(1, "start", 0, 0, "dftracer") +
                         call(1, "end", 100000, 0, "dftracer");
        for (int i = 0; i < 200; ++i) {
            dftu_utils_test::TextOut o;
            o << R"({"name":"PAPI","cat":"papi","pid":1,"tid":1,"ts":)"
              << i * 500
              << R"(,"ph":2,"args":{"hhash":"h1","multiplex":0,"PAPI_TOT_CYC":1,)"
                 R"("PAPI_TOT_CYC_delta":)"
              << 10 * i << "}}\n";
            t += o.str() + call(1, "f" + dstd::to_string(i % 40), i * 500, 200);
        }
        dftu_utils_test::write_gz_trace(
            dstd::String((dir / "compacted" / "set1.pfw.gz").str()), t);
    }
    const dstd::String out = d.file("out.pfw.gz");
    REQUIRE(run({d.file("root"), "-o", out, "--member-size", "4096"}) == 0);
    const auto lines = gz_lines(out);
    const auto first_head =
        std::find_if(lines.begin(), lines.end(), [](const dstd::String& l) {
            return l.find(R"("gtype":"counter")") == dstd::String::npos;
        });
    REQUIRE(first_head != lines.begin());
    REQUIRE(first_head != lines.end());
    std::size_t runs = 0;
    std::size_t funcs = 0;
    for (auto it = first_head; it != lines.end(); ++it) {
        CHECK(it->find(R"("gtype":"counter")") == dstd::String::npos);
        runs += it->rfind(R"({"gtype":"run",)", 0) == 0;
        funcs += it->rfind(R"({"gtype":"func",)", 0) == 0;
    }
    CHECK(runs == 3);
    CHECK(funcs == 3 * 40);
    CHECK(dstd::fs::exists(out + ".head").value() == false);
}

TEST_CASE("the shards of a sweep hold each run once") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    dftu_utils_test::ScopedTestDir d("gen_dist_shard");
    const dstd::Path root = d.path() / "root";
    for (int i = 0; i < 5; ++i) write_run(root, "in" + dstd::to_string(i));
    const dstd::Path off = root / "app" / "sys" / "off" / "nodes_1" / "ppn_1";
    dstd::fs::create_directories(off).has_value();
    dftu_utils_test::FileOut(off / "summary.json") << R"({"ok":false})";

    dstd::String full_text;
    REQUIRE(run({dstd::String(root.str()), "-o", d.file("full.pfw.gz")},
                &full_text) == 0);
    dstd::Vector<dstd::String> parts;
    dstd::String texts[2];
    for (int i = 0; i < 2; ++i) {
        const dstd::String out = d.file("s" + dstd::to_string(i) + ".pfw.gz");
        REQUIRE(run({dstd::String(root.str()), "-o", out, "--shard",
                     dstd::to_string(i) + "/2"},
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
    CHECK(full_text.find("neither sets nor runs key") != dstd::String::npos);
    CHECK(texts[0].find("neither sets nor runs key") != dstd::String::npos);
    CHECK(texts[1].find("neither sets nor runs key") == dstd::String::npos);
}

TEST_CASE("a shard outside i/N with 0 <= i < N is refused") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    dftu_utils_test::ScopedTestDir d("gen_dist_bad_shard");
    write_run(d.path() / "root", "in");
    for (const char* bad : {"2/2", "0/0", "1", "a/2", "1/2/3", "-1/2", "/2"}) {
        CAPTURE(bad);
        CHECK(run({d.file("root"), "-o", d.file("o.pfw.gz"), "--shard", bad}) !=
              0);
    }
}

TEST_CASE("the thread count does not change the output") {
    REQUIRE_FALSE(binary().empty());
    dftu_utils_test::set_test_library_path(binary());
    dftu_utils_test::ScopedTestDir d("gen_dist_threads");
    const dstd::Path dir =
        d.path() / "root" / "app" / "sys" / "in" / "nodes_1" / "ppn_4";
    dstd::fs::create_directories(dir / "compacted").has_value();
    dftu_utils_test::FileOut(dir / "summary.json")
        << R"({"ok":true,"app":"app","system":"sys","input":"in","nodes":1,)"
           R"("ppn":4,"sets":["set1"]})";
    dstd::String t = "[\n";
    for (int pid = 1; pid <= 4; ++pid) {
        t += call(pid, "start", 0, 0, "dftracer");
        t += call(pid, "end", 400000, 0, "dftracer");
        for (int i = 0; i < 2000; ++i)
            t += call(pid, "f" + dstd::to_string((i * pid) % 13), i * 100,
                      30 + (i * 7 + pid) % 60);
    }
    dftu_utils_test::write_gz_trace(
        dstd::String((dir / "compacted" / "set1.pfw.gz").str()), t);
    dstd::Vector<dstd::String> outs[2];
    const char* threads[2] = {"1", "4"};
    for (int k = 0; k < 2; ++k) {
        const dstd::String out =
            d.file(dstd::String("o") + threads[k] + ".pfw.gz");
        REQUIRE(run({d.file("root"), "-o", out, "--executor-threads",
                     threads[k]}) == 0);
        outs[k] = gz_lines(out);
        std::sort(outs[k].begin(), outs[k].end());
    }
    REQUIRE(outs[0].size() > 13);
    CHECK(outs[0] == outs[1]);
}
