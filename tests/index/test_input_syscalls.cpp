// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/trace/index/build/index_visitor.h>
#include <dftracer/utils/trace/index/gzip/gzip_indexer.h>
#include <dftracer/utils/trace/index/gzip/gzip_member_scanner.h>
#include <doctest/doctest.h>
#include <simdjson.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>

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
    std::size_t bytes = 0;
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
        if (cls == "bytes")
            hit->bytes += n;
        else
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

// An index build reads each gzip input once. The shapes below are multi-member
// gzip files that make the member-range planner of the indexer work for its
// bytes: members it can find at once, members it cannot find, zero gaps, a
// false header and a cut member.

namespace {

using Bytes = dstd::Vector<std::uint8_t>;

struct Text {
    dstd::String bytes;
    std::uint64_t lines = 0;
};

Text hex_lines(std::size_t size, std::uint32_t seed) {
    static const char HEX[] = "0123456789abcdef";
    std::mt19937 rng(seed);
    Text t;
    t.bytes.reserve(size + 160);
    while (t.bytes.size() < size) {
        t.bytes += common::str_cat(R"({"id":)", t.lines, R"(,"name":"read",)",
                                   R"("cat":"POSIX","args":{"fh":")");
        for (int k = 0; k < 32; ++k) t.bytes += HEX[rng() & 15];
        t.bytes += "\"}}\n";
        ++t.lines;
    }
    return t;
}

Bytes gzip_member(const dstd::String& text, int level = 6) {
    fileio::compress::GzipMemberCompressor c(level);
    Bytes out;
    REQUIRE(c.compress_member_into(out, text.data(), text.size()));
    return out;
}

void put(Bytes& to, const Bytes& m) { to.insert(to.end(), m.begin(), m.end()); }

void write_bytes(const dstd::String& path, const Bytes& bytes) {
    REQUIRE(dstd::fs::write_file(
                dstd::PathView(path),
                dstd::StringView(reinterpret_cast<const char*>(bytes.data()),
                                 bytes.size()))
                .has_value());
}

// What the builder knows about one member it wrote. `c_size` runs to the next
// member, so it takes in the zero bytes after this one.
struct Piece {
    std::uint64_t c_offset = 0;
    std::uint64_t c_size = 0;
    std::uint64_t uc_size = 0;
    std::uint64_t lines = 0;
};

struct Shape {
    dstd::String name;
    Bytes bytes;
    dstd::Vector<Piece> pieces;
    // Each member is one record. A member past the checkpoint size is cut into
    // several, and a cut member keeps only its whole lines.
    bool exact_pieces = true;
    // Lines of a member that cannot be read whole; a pass may recover some.
    std::uint64_t lost_lines = 0;
};

std::uint32_t g_seed = 1;

void add_member(Shape& s, std::size_t raw_bytes, std::size_t zeros = 0,
                int level = 6) {
    const Text t = hex_lines(raw_bytes, g_seed++);
    const Bytes m = gzip_member(t.bytes, level);
    s.pieces.push_back(
        {s.bytes.size(), m.size() + zeros, t.bytes.size(), t.lines});
    put(s.bytes, m);
    s.bytes.insert(s.bytes.end(), zeros, 0);
}

Shape many_members(std::size_t count, std::size_t raw_bytes) {
    Shape s{"many_members", {}, {}};
    for (std::size_t i = 0; i < count; ++i) add_member(s, raw_bytes);
    return s;
}

// Members that inflate past the checkpoint size, with small ones between.
Shape big_members(std::size_t big_bytes) {
    Shape s{"big_members", {}, {}, false};
    add_member(s, 40 << 10);
    for (std::size_t i = 0; i < 3; ++i) {
        add_member(s, big_bytes + i * (big_bytes / 4));
        add_member(s, 40 << 10);
    }
    return s;
}

Shape zero_gaps(std::size_t count, std::size_t raw_bytes) {
    Shape s{"zero_gaps", {}, {}};
    for (std::size_t i = 0; i < count; ++i) add_member(s, raw_bytes, i % 25);
    return s;
}

Shape single_member(std::size_t raw_bytes) {
    Shape s{"single_member", {}, {}, false};
    add_member(s, raw_bytes);
    return s;
}

Shape cut_member(std::size_t count, std::size_t raw_bytes, std::size_t big) {
    Shape s{"cut_member", {}, {}, false};
    for (std::size_t i = 0; i < count; ++i) add_member(s, raw_bytes);
    add_member(s, big);
    s.bytes.resize(s.bytes.size() - s.pieces.back().c_size * 2 / 5);
    return s;
}

// Members with `gap` bytes that are no gzip between member `after` and the
// next one.
Shape garbage_gap(std::size_t count, std::size_t raw_bytes, std::size_t after,
                  std::size_t gap) {
    Shape s{"garbage_gap", {}, {}, false};
    for (std::size_t i = 0; i < count; ++i) {
        add_member(s, raw_bytes);
        if (i == after) s.bytes.insert(s.bytes.end(), gap, 0xAB);
    }
    return s;
}

// Members with ten bytes flipped in the middle of member `bad`, which is lost;
// the lines of the others are the lines of the file.
Shape corrupt_member(std::size_t count, std::size_t raw_bytes,
                     std::size_t bad) {
    Shape s{"corrupt_member", {}, {}, false};
    for (std::size_t i = 0; i < count; ++i) add_member(s, raw_bytes);
    const Piece lost = s.pieces[bad];
    s.lost_lines = lost.lines;
    for (std::size_t k = 0; k < 10; ++k)
        s.bytes[lost.c_offset + lost.c_size / 2 + k] ^= 0xFF;
    s.pieces.erase(s.pieces.begin() + static_cast<std::ptrdiff_t>(bad));
    return s;
}

// Stored members, so every offset is known before the bytes are final, and one
// header pattern inside the payload of the member that spans the last multiple
// of `range` before the end, at its start. That header is not a member. The
// count is raised from `min_count` until the pattern is near the end, since a
// pass that takes it for a member reads from there to the end.
Shape false_header(std::size_t min_count, std::size_t range) {
    constexpr std::size_t RAW = 20000;
    constexpr std::size_t HEADER_AND_BLOCK = 10 + 5;
    static const unsigned char PATTERN[10] = {0x1F, 0x8B, 0x08, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x00, 0xFF};
    Shape s{"false_header", {}, {}};
    dstd::Vector<Text> texts;
    texts.push_back(hex_lines(RAW, g_seed++));
    const std::size_t member_size = gzip_member(texts[0].bytes, 0).size();
    std::size_t count = min_count;
    while ((count * member_size) % range < range / 32 ||
           (count * member_size) % range > range / 16)
        ++count;
    while (texts.size() < count) texts.push_back(hex_lines(RAW, g_seed++));
    const std::uint64_t total = std::uint64_t{count} * member_size;
    const std::uint64_t at = total / range * range + 64;
    const std::size_t holder = static_cast<std::size_t>(at / member_size);
    const std::size_t payload =
        static_cast<std::size_t>(at - holder * member_size - HEADER_AND_BLOCK);
    for (std::size_t i = 0; i < count; ++i) {
        Text& t = texts[i];
        if (i == holder) {
            REQUIRE(payload + sizeof PATTERN < t.bytes.size());
            std::memcpy(t.bytes.data() + payload, PATTERN, sizeof PATTERN);
            t.lines = static_cast<std::uint64_t>(
                std::count(t.bytes.begin(), t.bytes.end(), '\n'));
        }
        const Bytes m = gzip_member(t.bytes, 0);
        REQUIRE(m.size() == member_size);
        s.pieces.push_back({s.bytes.size(), m.size(), t.bytes.size(), t.lines});
        put(s.bytes, m);
    }
    return s;
}

}  // namespace

TEST_CASE("an index build reads each gzip input byte about once") {
    const dstd::String binary =
        dstd::String(DFTRACER_UTILS_TEST_BIN_DIR) + "/dftracer_index";
    REQUIRE_MESSAGE(::access(binary.c_str(), X_OK) == 0,
                    binary << " is not built");
    constexpr std::uint64_t CKPT = std::uint64_t{4} << 20;
    // The size of the ranges the indexer cuts, set to a small one so that the
    // files below have many.
    constexpr std::uint64_t RANGE = std::uint64_t{1} << 20;

    dstd::Vector<Shape> shapes;
    const auto add = [&](Shape s, const char* name) {
        s.name = name;
        shapes.push_back(std::move(s));
    };
    // The first is read once by the indexer whole, so what it costs beyond its
    // size is what the other layers read of any input (its head, its tail, its
    // magic), which the others are allowed on top of their own slack.
    add(single_member(std::size_t{4} << 20), "calibration");
    add(many_members(700, 24 << 10), "many");
    add(many_members(80, std::size_t{256} << 10), "mid");
    add(big_members(std::size_t{5} << 20), "big");
    add(zero_gaps(450, 24 << 10), "zero_gaps");
    add(false_header(360, RANGE), "false_hdr");
    add(cut_member(200, 24 << 10, std::size_t{5} << 20), "cut");
    add(single_member(std::size_t{6} << 20), "single");

    dftu_utils_test::ScopedTestDir dir("index_input_bytes");
    const dstd::String in = dir.file("in");
    const dstd::String idx = dir.file("idx");
    REQUIRE(dstd::fs::create_directories(in).has_value());
    REQUIRE(dstd::fs::create_directories(idx).has_value());
    dstd::Vector<dstd::String> inputs;
    for (const Shape& s : shapes) {
        inputs.push_back(in + "/" + s.name + ".pfw.gz");
        write_bytes(inputs.back(), s.bytes);
    }

    ::setenv("DFTRACER_UTILS_IO_BACKEND", "threadpool", 1);
    ::setenv("DFTRACER_UTILS_INDEX_RANGE_BYTES", dstd::to_string(RANGE).c_str(),
             1);
    const auto run = run_once(binary,
                              {"--executor-threads", "4", in, "--index-dir",
                               idx, "--checkpoint-size", dstd::to_string(CKPT)},
                              dir.file("bytes.log"), {in}, inputs);
    ::unsetenv("DFTRACER_UTILS_INDEX_RANGE_BYTES");
    ::unsetenv("DFTRACER_UTILS_IO_BACKEND");
    REQUIRE(run.log_found);
    REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    const std::size_t other = run.per_input[0].bytes - shapes[0].bytes.size();
    MESSAGE("other layers read " << other << " bytes of each input");
    CHECK(other < std::size_t{256} << 10);
    for (std::size_t k = 1; k < inputs.size(); ++k) {
        const std::size_t size = shapes[k].bytes.size();
        MESSAGE(shapes[k].name << ": read " << run.per_input[k].bytes
                               << " bytes of " << size);
        CHECK_MESSAGE(run.per_input[k].bytes > 0,
                      shapes[k].name << ": no bytes counted; the interposer "
                                        "did not see the reads");
        // 5% for the bytes a range passes over before its first header, and a
        // little for the false header that is read to the end of the file.
        CHECK_MESSAGE(
            run.per_input[k].bytes <= size + size / 20 + other + (4u << 10),
            shapes[k].name << ": read " << run.per_input[k].bytes
                           << " bytes of a " << size << " byte file");
    }
}

namespace {

struct Built {
    bool ok = false;
    trace::index::gzip::GzipBuildArtifacts artifacts;
    // What a visitor folded from the chunks it was given.
    std::uint64_t seen_bytes = 0;
    std::uint64_t seen_lines = 0;
    std::uint64_t seen_hash = 0;
    dstd::Vector<std::size_t> checkpoints;
};

class FoldVisitor : public trace::index::build::IndexVisitor {
   public:
    explicit FoldVisitor(Built& out) : out_(out) {}
    void begin(std::size_t) override {}
    coro::CoroTask<dstd::Result<void>> on_checkpoint(std::size_t idx) override {
        out_.checkpoints.push_back(idx);
        co_return dstd::Result<void>{};
    }
    coro::CoroTask<dstd::Result<void>> on_chunk(const char* data,
                                                std::size_t len,
                                                std::size_t) override {
        for (std::size_t i = 0; i < len; ++i) {
            out_.seen_hash =
                (out_.seen_hash ^ static_cast<unsigned char>(data[i])) *
                1099511628211ULL;
            out_.seen_lines += data[i] == '\n';
        }
        out_.seen_bytes += len;
        co_return dstd::Result<void>{};
    }

   private:
    Built& out_;
};

// `scanned` runs the pass on the member list that a header scan of the whole
// file gives, which is how a pass that does not cut ranges by bytes works.
Built build_artifacts(const dstd::String& path, std::uint64_t ckpt,
                      std::size_t ways, bool scanned) {
    Built b;
    b.seen_hash = 14695981039346656037ULL;
    dftu_utils_test::run_coro([&](coro::Scope& scope) -> coro::CoroTask<void> {
        FoldVisitor fold(b);
        trace::index::gzip::CheckpointIndexer::VisitorList visitors;
        visitors.emplace_back(fold);
        dstd::Vector<trace::index::gzip::GzipMember> candidates;
        trace::index::gzip::GzipMemberSlice slice;
        const trace::index::gzip::GzipMemberSlice* use = nullptr;
        if (scanned) {
            auto file = dstd::File::open(path, {});
            REQUIRE(file);
            const std::uint64_t size = dstd::fs::file_size(path).value();
            co_await trace::index::gzip::enumerate_gzip_member_candidates(
                file->native_handle(), size, candidates);
            slice.members = &candidates;
            slice.member_end = candidates.size();
            if (!candidates.empty()) use = &slice;
        }
        auto built = co_await trace::index::gzip::build_gzip_index_artifacts(
            path, ckpt, visitors, &scope, use, ways);
        if (built) {
            b.ok = true;
            b.artifacts = std::move(*built);
        }
    });
    return b;
}

void check_same(const Built& a, const Built& b, const char* what) {
    INFO(dstd::String(what));
    REQUIRE(a.ok == b.ok);
    if (!a.ok) return;
    const auto& x = a.artifacts;
    const auto& y = b.artifacts;
    CHECK(x.total_lines == y.total_lines);
    CHECK(x.total_uc_size == y.total_uc_size);
    CHECK(x.truncated == y.truncated);
    CHECK(x.recovered_bytes == y.recovered_bytes);
    CHECK(x.has_open_line == y.has_open_line);
    CHECK(a.seen_bytes == b.seen_bytes);
    CHECK(a.seen_lines == b.seen_lines);
    CHECK(a.seen_hash == b.seen_hash);
    CHECK(a.checkpoints == b.checkpoints);
    REQUIRE(x.members.size() == y.members.size());
    for (std::size_t i = 0; i < x.members.size(); ++i) {
        CAPTURE(i);
        const auto& p = x.members[i];
        const auto& q = y.members[i];
        CHECK(p.member_idx == q.member_idx);
        CHECK(p.c_offset == q.c_offset);
        CHECK(p.c_size == q.c_size);
        CHECK(p.uc_offset == q.uc_offset);
        CHECK(p.uc_size == q.uc_size);
        CHECK(p.first_line_num == q.first_line_num);
        CHECK(p.last_line_num == q.last_line_num);
        CHECK(p.kind == q.kind);
        CHECK(p.bits == q.bits);
        CHECK(p.opens_mid_line == q.opens_mid_line);
    }
    CHECK(x.restart_windows == y.restart_windows);
}

}  // namespace

TEST_CASE("member ranges cut by bytes index like a pass over scanned members") {
    constexpr std::uint64_t CKPT = std::uint64_t{512} << 10;
    constexpr std::uint64_t RANGE = std::uint64_t{64} << 10;
    ::setenv("DFTRACER_UTILS_INDEX_RANGE_BYTES", dstd::to_string(RANGE).c_str(),
             1);
    dstd::Vector<Shape> shapes;
    shapes.push_back(many_members(900, 3 << 10));
    shapes.push_back(many_members(300, 24 << 10));
    shapes.push_back(many_members(30, std::size_t{256} << 10));
    shapes.push_back(big_members(std::size_t{2} << 20));
    shapes.push_back(zero_gaps(300, 3 << 10));
    shapes.push_back(zero_gaps(100, 24 << 10));
    shapes.push_back(false_header(60, RANGE));
    shapes.push_back(cut_member(40, 24 << 10, std::size_t{2} << 20));
    shapes.push_back(single_member(std::size_t{3} << 20));
    shapes.push_back(single_member(10 << 10));

    dftu_utils_test::ScopedTestDir dir("index_member_ranges");
    std::size_t n = 0;
    for (const Shape& s : shapes) {
        const dstd::String path =
            dir.file(common::str_cat(n++, "_", s.name, ".pfw.gz"));
        write_bytes(path, s.bytes);
        INFO(path);
        const Built scanned = build_artifacts(path, CKPT, 4, true);
        const Built one = build_artifacts(path, CKPT, 1, false);
        const Built many = build_artifacts(path, CKPT, 4, false);
        REQUIRE(scanned.ok);
        check_same(scanned, one, "one worker");
        check_same(scanned, many, "four workers");

        const auto& arts = many.artifacts;
        if (s.exact_pieces) {
            REQUIRE(arts.members.size() == s.pieces.size());
            for (std::size_t i = 0; i < s.pieces.size(); ++i) {
                CAPTURE(i);
                CHECK(arts.members[i].c_offset == s.pieces[i].c_offset);
                CHECK(arts.members[i].c_size == s.pieces[i].c_size);
                CHECK(arts.members[i].uc_size == s.pieces[i].uc_size);
                CHECK(arts.members[i].last_line_num -
                          arts.members[i].first_line_num + 1 ==
                      s.pieces[i].lines);
            }
        }
        std::uint64_t uc = 0, lines = 0;
        for (const Piece& p : s.pieces) {
            uc += p.uc_size;
            lines += p.lines;
        }
        if (s.name != "cut_member") {
            CHECK(arts.total_uc_size == uc);
            CHECK(arts.total_lines == lines);
            CHECK(!arts.truncated);
            CHECK(many.seen_lines == lines);
        } else {
            CHECK(arts.truncated);
            CHECK(arts.recovered_bytes > 0);
        }
    }
    ::unsetenv("DFTRACER_UTILS_INDEX_RANGE_BYTES");
}

// A pass over scanned members stops at a member it cannot read, and the pass
// that cuts ranges by bytes must lose no more than that: it finds the next
// header and goes on, so only the unreadable member is lost.
TEST_CASE("unreadable bytes between or inside members lose only those bytes") {
    constexpr std::uint64_t RANGE = std::uint64_t{64} << 10;
    ::setenv("DFTRACER_UTILS_INDEX_RANGE_BYTES", dstd::to_string(RANGE).c_str(),
             1);
    dstd::Vector<Shape> shapes;
    shapes.push_back(garbage_gap(120, 120 << 10, 60, 100));
    shapes.push_back(garbage_gap(120, 120 << 10, 60, 300000));
    shapes.push_back(garbage_gap(40, std::size_t{600} << 10, 20, 100));
    shapes.push_back(corrupt_member(120, 120 << 10, 60));
    shapes.push_back(corrupt_member(120, 3 << 10, 60));
    shapes.push_back(corrupt_member(30, std::size_t{600} << 10, 10));

    dftu_utils_test::ScopedTestDir dir("index_member_loss");
    std::size_t n = 0;
    for (const Shape& s : shapes) {
        const dstd::String path =
            dir.file(common::str_cat(n++, "_", s.name, ".pfw.gz"));
        write_bytes(path, s.bytes);
        INFO(path);
        std::uint64_t lines = 0;
        for (const Piece& p : s.pieces) lines += p.lines;
        for (const std::uint64_t ckpt :
             {std::uint64_t{512} << 10, std::uint64_t{4} << 20}) {
            CAPTURE(ckpt);
            const Built one = build_artifacts(path, ckpt, 1, false);
            const Built many = build_artifacts(path, ckpt, 4, false);
            const Built scanned = build_artifacts(path, ckpt, 4, true);
            REQUIRE(one.ok);
            REQUIRE(many.ok);
            // A member cut by the damage may give up the lines before it.
            const std::uint64_t most = lines + s.lost_lines;
            CHECK(one.artifacts.total_lines >= lines);
            CHECK(one.artifacts.total_lines <= most);
            CHECK(many.artifacts.total_lines >= lines);
            CHECK(many.artifacts.total_lines <= most);
            CHECK(one.seen_lines == one.artifacts.total_lines);
            CHECK(many.seen_lines == many.artifacts.total_lines);
            check_same(one, many, "one and four workers");
            if (scanned.ok) {
                CHECK(many.artifacts.total_lines >=
                      scanned.artifacts.total_lines);
            }
        }
    }
    ::unsetenv("DFTRACER_UTILS_INDEX_RANGE_BYTES");
}

TEST_CASE("a file shorter than a gzip member indexes like the scanned pass") {
    dftu_utils_test::ScopedTestDir dir("index_member_ranges_tiny");
    constexpr std::uint64_t CKPT = std::uint64_t{1} << 20;
    // A gzip header and nothing else, and text.
    const unsigned char header[10] = {0x1F, 0x8B, 0x08, 0x00, 0x00,
                                      0x00, 0x00, 0x00, 0x00, 0x03};
    const dstd::String path = dir.file("header.pfw.gz");
    write_bytes(path, Bytes(header, header + sizeof header));
    check_same(build_artifacts(path, CKPT, 4, true),
               build_artifacts(path, CKPT, 4, false), "header only");
    const dstd::String text = dir.file("text.pfw.gz");
    write_bytes(text, Bytes{'a', ' ', 'b', '\n'});
    check_same(build_artifacts(text, CKPT, 4, true),
               build_artifacts(text, CKPT, 4, false), "text");
}

namespace {

dstd::String run_capture(const char* tool,
                         const dstd::Vector<dstd::String>& args) {
    const dstd::String binary =
        dstd::String(DFTRACER_UTILS_TEST_BIN_DIR) + "/" + tool;
    dftu_utils_test::set_test_library_path(binary);
    int code = -1;
    const dstd::String out =
        dftu_utils_test::run_process_capture(binary, args, false, &code);
    REQUIRE_MESSAGE(code == 0, tool << " exited with " << code);
    return out;
}

}  // namespace

// What a damaged member leaves in the index is what a read returns: every
// line read parses as JSON, the event count is the number of lines read but
// for a garbled line the index reads only in part, each event of the intact
// members comes back once, and the damaged member gives at most its text.
TEST_CASE("events read back after a damaged member match the index count") {
    constexpr std::size_t MEMBERS = 12;
    constexpr std::size_t BAD = 5;
    for (const std::size_t raw :
         {std::size_t{3} << 10, std::size_t{2200} << 10}) {
        const dstd::String ckpts[] = {dstd::String("1MB"),
                                      dstd::String("32MB")};
        for (const dstd::String& ckpt : ckpts) {
            Bytes file;
            dstd::Vector<Text> texts;
            for (std::size_t i = 0; i < MEMBERS; ++i) {
                texts.push_back(hex_lines(raw, g_seed++));
                Bytes m = gzip_member(texts.back().bytes);
                if (i == BAD) {
                    for (std::size_t k = 0; k < 8; ++k)
                        m[m.size() * 4 / 5 + k] ^= 0xA5;
                }
                put(file, m);
            }
            dftu_utils_test::ScopedTestDir dir("index_damaged_read");
            const dstd::String in = dir.file("in");
            REQUIRE(dstd::fs::create_directories(in).has_value());
            write_bytes(in + "/damaged.pfw.gz", file);
            const dstd::String idx = dir.file("idx");
            const dstd::Vector<dstd::String> common_args = {"--index-dir", idx,
                                                            in};
            dstd::Vector<dstd::String> index_run = {"--executor-threads", "2",
                                                    "--checkpoint-size",
                                                    dstd::String(ckpt)};
            for (const auto& a : common_args) index_run.push_back(a);
            run_capture("dftracer_index", index_run);
            const dstd::String counted = run_capture(
                "dftracer_view", {"--index-dir", idx, "--checkpoint-size",
                                  dstd::String(ckpt), "--agg", "count", in});
            const dstd::String viewed = run_capture(
                "dftracer_view", {"--index-dir", idx, "--checkpoint-size",
                                  dstd::String(ckpt), "-q", "id >= 0", in});
            const std::size_t digits = counted.find_first_of("0123456789");
            REQUIRE(digits != dstd::String::npos);
            const auto count =
                dstd::from_chars<std::size_t>(dstd::StringView(counted).substr(
                    digits,
                    counted.find_first_not_of("0123456789", digits) - digits));
            REQUIRE(count.has_value());

            // Events by their file handle, which is unique in the generated
            // text.
            dstd::Map<dstd::String, std::size_t> seen;
            std::size_t events = 0;
            std::size_t unparsed = 0;
            simdjson::dom::parser parser;
            for (std::size_t at = 0; at < viewed.size();) {
                std::size_t end = viewed.find('\n', at);
                if (end == dstd::String::npos) end = viewed.size();
                const dstd::String line = viewed.substr(at, end - at);
                at = end + 1;
                if (line.empty()) continue;
                ++events;
                const simdjson::padded_string padded(line.data(), line.size());
                simdjson::dom::element root;
                if (parser.parse(padded).get(root)) ++unparsed;
                const std::size_t fh = line.find("\"fh\":\"");
                if (fh != dstd::String::npos && line.size() >= fh + 38)
                    ++seen[line.substr(fh, 38)];
            }
            CHECK_MESSAGE(unparsed == 0,
                          unparsed << " printed lines are not JSON");
            std::size_t intact = 0;
            for (std::size_t i = 0; i < MEMBERS; ++i) {
                if (i == BAD) continue;
                for (std::size_t at = 0; at < texts[i].bytes.size();) {
                    const std::size_t end = texts[i].bytes.find('\n', at);
                    const dstd::String line =
                        texts[i].bytes.substr(at, end - at);
                    ++intact;
                    at = end + 1;
                    const auto it =
                        seen.find(line.substr(line.find("\"fh\":\""), 38));
                    const bool once = it != seen.end() && it->second == 1;
                    CHECK_MESSAGE(once, "an event of member "
                                            << i << " is not read once");
                }
            }
            INFO("raw " << raw << " ckpt " << dstd::String(ckpt));
            // The index reads the fields it indexes of a garbled line that
            // still parses lazily; a read prints only whole records, so it
            // can show fewer lines by a few of the garbled ones.
            CHECK(events <= *count);
            CHECK(*count - events <= texts[BAD].lines / 100 + 1);
            CHECK(events >= intact);
            CHECK(events <= intact + texts[BAD].lines);
        }
    }
}

namespace {

// A member of stored blocks of 65535 bytes, so the text is plain in the file,
// whose block `bad` has an invalid header: the text before that block is
// readable and the rest is not.
Bytes stored_member_with_bad_block(const dstd::String& text, std::size_t bad) {
    constexpr std::size_t BLOCK = 65535;
    Bytes m = {0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xFF};
    const std::size_t blocks = (text.size() + BLOCK - 1) / BLOCK;
    for (std::size_t k = 0; k < blocks; ++k) {
        const std::size_t len = std::min(BLOCK, text.size() - k * BLOCK);
        m.push_back(k == bad ? 0x07 : (k + 1 == blocks ? 1 : 0));
        m.push_back(static_cast<unsigned char>(len & 0xFF));
        m.push_back(static_cast<unsigned char>(len >> 8));
        m.push_back(static_cast<unsigned char>(~len & 0xFF));
        m.push_back(static_cast<unsigned char>((~len >> 8) & 0xFF));
        m.insert(m.end(), text.begin() + static_cast<std::ptrdiff_t>(k * BLOCK),
                 text.begin() + static_cast<std::ptrdiff_t>(k * BLOCK + len));
    }
    m.insert(m.end(), 8, 0);
    return m;
}

}  // namespace

// A member that breaks early keeps the lines before the break, however large
// the member is, and the index and a read agree on them. Every line a read
// prints parses as JSON.
TEST_CASE("the text before an early break in a member is read back") {
    constexpr std::size_t RAW = 800 << 10;
    const dstd::String ckpts[] = {dstd::String("128KB"), dstd::String("1MB")};
    for (const std::size_t bad :
         {std::size_t{3}, std::size_t{4}, std::size_t{11}}) {
        for (const dstd::String& ckpt : ckpts) {
            const Text before = hex_lines(40 << 10, g_seed++);
            const Text damaged = hex_lines(RAW, g_seed++);
            const Text after = hex_lines(40 << 10, g_seed++);
            Bytes file;
            put(file, gzip_member(before.bytes));
            put(file, stored_member_with_bad_block(damaged.bytes, bad));
            put(file, gzip_member(after.bytes));
            std::size_t kept = 0;
            for (std::size_t i = 0; i < bad * 65535; ++i)
                kept += damaged.bytes[i] == '\n';
            dftu_utils_test::ScopedTestDir dir("index_early_break");
            const dstd::String in = dir.file("in");
            REQUIRE(dstd::fs::create_directories(in).has_value());
            write_bytes(in + "/early.pfw.gz", file);
            const dstd::String idx = dir.file("idx");
            run_capture("dftracer_index",
                        {"--executor-threads", "2", "--checkpoint-size",
                         dstd::String(ckpt), "--index-dir", idx, in});
            const dstd::String counted = run_capture(
                "dftracer_view", {"--index-dir", idx, "--checkpoint-size",
                                  dstd::String(ckpt), "--agg", "count", in});
            const dstd::String viewed = run_capture(
                "dftracer_view", {"--index-dir", idx, "--checkpoint-size",
                                  dstd::String(ckpt), "-q", "id >= 0", in});
            const std::size_t digits = counted.find_first_of("0123456789");
            REQUIRE(digits != dstd::String::npos);
            const auto count =
                dstd::from_chars<std::size_t>(dstd::StringView(counted).substr(
                    digits,
                    counted.find_first_not_of("0123456789", digits) - digits));
            REQUIRE(count.has_value());
            std::size_t events = 0;
            std::size_t unparsed = 0;
            simdjson::dom::parser parser;
            for (std::size_t at = 0; at < viewed.size();) {
                std::size_t end = viewed.find('\n', at);
                if (end == dstd::String::npos) end = viewed.size();
                const dstd::String line = viewed.substr(at, end - at);
                at = end + 1;
                if (line.empty()) continue;
                ++events;
                const simdjson::padded_string padded(line.data(), line.size());
                simdjson::dom::element root;
                if (parser.parse(padded).get(root)) ++unparsed;
            }
            INFO("bad block " << bad << " ckpt " << dstd::String(ckpt));
            CHECK(unparsed == 0);
            CHECK(*count == before.lines + kept + after.lines);
            CHECK(events == *count);
        }
    }
}
