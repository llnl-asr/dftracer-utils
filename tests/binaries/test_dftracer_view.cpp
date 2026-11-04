// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/binaries/json_cell_printer.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/int128.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <refused_lines.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dataframe/builders_arrow_types.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

// ============================================================================
// Helpers
// ============================================================================

using namespace dftracer::utils;
namespace {

dstd::String create_pfw_gz(dftu_utils_test::TestEnvironment& env,
                           int num_events, int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    dstd::String pfw_path =
        env.get_dir() + "/trace_" + dstd::to_string(id) + ".pfw.gz";
    REQUIRE(dstd::fs::rename(trace_gz, pfw_path).has_value());
    return pfw_path;
}

// Rechunk a single-member fixture into a multi-member trace whose members are
// about `member_bytes` uncompressed, for exercising ingest member handling.
dstd::String make_multimember_pfw_gz(dftu_utils_test::TestEnvironment& env,
                                     int num_events, std::size_t member_bytes,
                                     int id) {
    dstd::String single = create_pfw_gz(env, num_events, id);
    if (single.empty()) return "";
    dstd::String mm = env.get_dir() + "/mm_" + dstd::to_string(id) + ".pfw.gz";
    utilities::fileio::compress::gzip_rechunk_to_members(
        dstd::String(single), dstd::String(mm), member_bytes, 6)
        .get();
    (void)dstd::fs::remove(single);
    return mm;
}

dstd::String find_view_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_UTILS_VIEW_PATH",
                                                "dftracer_view");
}

int run_view(const dstd::String& binary,
             const dstd::Vector<dstd::String>& args) {
    dstd::Vector<const char*> argv;
    argv.push_back(binary.c_str());
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        MESSAGE("Binary killed by signal ", sig);
        return -(sig);
    }
    return -1;
}

// Run `binary`, capturing its stdout into `out`. The capture file is opened in
// the parent; the child only calls async-signal-safe dup2/close/execv.
int run_view_capture(const dstd::String& binary,
                     const dstd::Vector<dstd::String>& args,
                     const dstd::String& capture_path, dstd::String& out) {
    dstd::Vector<const char*> argv;
    argv.push_back(binary.c_str());
    for (const auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    int fd = ::open(capture_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fd);
        return -1;
    }
    if (pid == 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::close(fd);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    ::close(fd);
    int status = 0;
    ::waitpid(pid, &status, 0);
    out = dftu_utils_test::read_text(capture_path);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

std::size_t count_lines(const dstd::String& s) {
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n'));
}

// ----------------------------------------------------------------------------
// append_cell_json builders: cover every dataframe::TypeId directly, since
// no trace field the CLI's own query/aggregation paths produce carries the
// narrow-int, Float16, temporal, decimal, or Large*/FixedSizeBinary types.
// ----------------------------------------------------------------------------

dataframe::Series make_bool_series(const dstd::Vector<bool>& bits) {
    dstd::Vector<std::uint8_t> packed((bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) packed[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return dataframe::Series::flat(dataframe::TypeId::Bool, packed.data(),
                                   static_cast<std::int64_t>(bits.size()));
}

// A String/Binary FLAT column; dftu_series_new_string supports both dtypes.
dataframe::Series make_bytes_series(dataframe::TypeId type,
                                    const dstd::Vector<dstd::String>& values) {
    dstd::Vector<std::int32_t> offsets(values.size() + 1, 0);
    dstd::String data;
    for (std::size_t i = 0; i < values.size(); ++i) {
        data += values[i];
        offsets[i + 1] = static_cast<std::int32_t>(data.size());
    }
    return dataframe::Series{dftu_series_new_string(
        static_cast<dftu_dtype>(type), offsets.data(), data.data(),
        static_cast<std::int64_t>(values.size()), nullptr)};
}

dataframe::Series make_float16_series(const dstd::Vector<float>& values) {
    dstd::Vector<std::uint16_t> halves(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        halves[i] = dataframe::float_to_half(values[i]);
    return dataframe::Series::flat(dataframe::TypeId::Float16, halves.data(),
                                   static_cast<std::int64_t>(values.size()));
}

// Decimal128: exact little-endian two's-complement payload, decimal_scale 0
// (the only scale a FLAT-constructed decimal column carries; nonzero scale
// only reaches the engine via Arrow import, unreachable from this CLI).
dataframe::Series make_decimal128_series(
    const dstd::Vector<dstd::int128_t>& values) {
    dstd::Vector<std::uint8_t> bytes(values.size() * 16);
    for (std::size_t i = 0; i < values.size(); ++i)
        std::memcpy(bytes.data() + i * 16, &values[i], 16);
    return dataframe::Series::flat(dataframe::TypeId::Decimal128, bytes.data(),
                                   static_cast<std::int64_t>(values.size()));
}

// Decimal256: `values` sign-extended from 128 to 256 bits.
dataframe::Series make_decimal256_series(
    const dstd::Vector<dstd::int128_t>& values) {
    dstd::Vector<std::uint8_t> bytes(values.size() * 32, 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::memcpy(bytes.data() + i * 32, &values[i], 16);
        if (values[i] < 0) std::memset(bytes.data() + i * 32 + 16, 0xFF, 16);
    }
    return dataframe::Series::flat(dataframe::TypeId::Decimal256, bytes.data(),
                                   static_cast<std::int64_t>(values.size()));
}

dataframe::Series make_unknown_series(std::int64_t n) {
    auto* raw = new dftu_series();
    raw->type = dataframe::TypeId::Unknown;
    raw->length = n;
    return dataframe::Series(raw);
}

dstd::String cell(const dataframe::Series& c, std::int64_t i) {
    dstd::String s;
    dftracer::utils::binaries::append_cell_json(s, c, i);
    return dstd::String(s);
}

// Create a trace in its own subdir and build its index, with the aggregation
// tier every build makes, over it; returns the shard's .dftindex path.
dstd::String build_aggregated_shard(dftu_utils_test::TestEnvironment& env,
                                    const dstd::String& tag, int num_events,
                                    const dstd::String& index_dir = "") {
    dstd::String dir = env.get_dir() + "/" + tag;
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    dstd::String src = env.create_dft_test_gzip_file(num_events);
    dstd::String gz = dir + "/t.pfw.gz";
    REQUIRE(dstd::fs::rename(src, gz).has_value());

    dftracer::utils::index::IndexerOptions options;
    options.index_dir = index_dir;
    dftracer::utils::index::Indexer::open({dstd::String(dstd::StringView(gz))},
                                          options)
        .build();
    if (index_dir.empty())
        return dstd::String(trace::internal::determine_index_path(gz, ""));
    return dstd::String(trace::internal::determine_index_path("x", index_dir));
}

// Split into non-empty lines and sort, so two aggregations can be compared
// regardless of row emission order.
dstd::Vector<dstd::String> sorted_lines(const dstd::String& s) {
    dstd::Vector<dstd::String> lines;
    dstd::String cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty()) lines.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(cur);
    std::sort(lines.begin(), lines.end());
    return lines;
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerView") {
    TEST_CASE("binary exists") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE(
                "dftracer_view binary not found, skipping. "
                "Set DFTRACER_UTILS_VIEW_PATH env to specify location.");
            return;
        }
        CHECK(!binary.empty());
    }

    TEST_CASE("binary runs (--help)") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        int rc = run_view(binary, {"--help"});
        MESSAGE("--help returned: ", rc);
        CHECK(rc == 0);
    }

    TEST_CASE("stream all events") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = run_view(binary, {"-q", R"(cat == "POSIX")", env.get_dir()});
        CHECK(rc == 0);
    }

    TEST_CASE("duql filter") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = run_view(binary, {"-q", R"(cat == "POSIX")", env.get_dir()});
        CHECK(rc == 0);
    }

    TEST_CASE("percentile and shape aggregations") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = run_view(
            binary, {"--group-by", "cat", "--agg",
                     "p99:dur,pct:dur:0.95,skew:dur,kurt:dur", env.get_dir()});
        CHECK(rc == 0);

        // pct without a quantile and pNN without a field are rejected.
        CHECK(run_view(binary, {"--group-by", "cat", "--agg", "pct:dur",
                                env.get_dir()}) != 0);
        CHECK(run_view(binary, {"--group-by", "cat", "--agg", "p99",
                                env.get_dir()}) != 0);
    }

    TEST_CASE("output to file") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        // File output is always a compressed (.gz) trace.
        dstd::String output = env.get_dir() + "/view_output.pfw";
        int rc = run_view(binary, {"-q", R"(cat == "POSIX")", "--no-index",
                                   env.get_dir(), "-o", output});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(output + ".gz").value());
        CHECK(dstd::fs::file_size(output + ".gz").value() > 0);
    }

    TEST_CASE("fused index round-trips (--merge --verify)") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        // Inputs in their own dir so the .pfw.gz output is not rescanned as
        // input on a subsequent run.
        dstd::String in = env.get_dir() + "/in";
        REQUIRE(dstd::fs::create_directories(in).has_value());
        auto trace_gz = env.create_dft_test_gzip_file(50);
        REQUIRE(!trace_gz.empty());
        REQUIRE(dstd::fs::rename(trace_gz, in + "/trace_0.pfw.gz").has_value());

        // Default write path builds the index during the write (fused).
        // --verify re-scans the written+indexed trace via that index and checks
        // the event count round-trips, so rc==0 proves the fused member index
        // is correct.
        dstd::String output = env.get_dir() + "/merged.pfw";
        int rc = run_view(binary, {in, "--merge", "--verify", "-o", output});
        CHECK(rc == 0);
        REQUIRE(dstd::fs::exists(output + ".gz").value());
        // The index was built by the write, not a separate pass: its .dftindex
        // directory must exist next to the output.
        CHECK(dstd::fs::exists(env.get_dir() + "/.dftindex").value());
    }

    TEST_CASE("duql with name filter") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = run_view(
            binary, {"-q", R"(cat == "POSIX" and name in ["pread", "pwrite"])",
                     env.get_dir()});
        CHECK(rc == 0);
    }

    // Fixture ts = 1000000000 + i*100000 for i=1..N, so a window that ends at
    // 1000200000 keeps only i=1,2. --time-range must filter per-event, not just
    // prune whole chunks.
    TEST_CASE("time-range filters events exactly") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50, 0).empty());

        dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::String full, narrow;
        CHECK(run_view_capture(binary, {"-q", "ts >= 0", env.get_dir()}, cap,
                               full) == 0);
        CHECK(run_view_capture(binary,
                               {"--time-range", "0,1000200000", env.get_dir()},
                               cap, narrow) == 0);
        // [0, ts of i=2) keeps only i=1: the window excludes its end.
        CHECK(count_lines(narrow) == 1);
        CHECK(count_lines(full) > count_lines(narrow));
    }

    TEST_CASE("agg-numeric-args aggregates numeric args") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50, 0).empty());

        dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::String plain, numeric;
        CHECK(run_view_capture(
                  binary,
                  {"--group-by", "cat", "--agg", "count", env.get_dir()}, cap,
                  plain) == 0);
        CHECK(run_view_capture(
                  binary,
                  {"--group-by", "cat", "--agg-numeric-args", env.get_dir()},
                  cap, numeric) == 0);
        CHECK(!numeric.empty());
        // The fixture carries a numeric arg, so --agg-numeric-args adds a value
        // column that a bare count does not.
        CHECK(numeric.size() > plain.size());
    }

    // Pointing -d at a directory whose index root holds shards.json
    // autodetects the set and answers the aggregate over the immutable shards,
    // matching a direct query over the same traces.
    TEST_CASE("shard-set query matches a direct query") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found; skipping");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        const dstd::String set = env.get_dir() + "/set";
        const dstd::String root =
            set + "/" + dstd::String(dftracer::utils::index::INDEX_ROOT_NAME);
        dstd::String sa = build_aggregated_shard(
            env, "sa", 50,
            dstd::String(dftracer::utils::index::shard_root(root, 0)));
        dstd::String sb = build_aggregated_shard(
            env, "sb", 40,
            dstd::String(dftracer::utils::index::shard_root(root, 1)));
        dftracer::utils::trace::views::write_shard_set(
            dstd::String(root), {dstd::String(sa), dstd::String(sb)});

        // A flat directory with the same two traces for the baseline query.
        dstd::String both = env.get_dir() + "/both";
        REQUIRE(dstd::fs::create_directories(both).has_value());
        REQUIRE(dstd::fs::copy_file(env.get_dir() + "/sa/t.pfw.gz",
                                    both + "/a.pfw.gz",
                                    dstd::fs::CopyMode::FailIfExists)
                    .has_value());
        REQUIRE(dstd::fs::copy_file(env.get_dir() + "/sb/t.pfw.gz",
                                    both + "/b.pfw.gz",
                                    dstd::fs::CopyMode::FailIfExists)
                    .has_value());

        dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::String shard_out, direct_out;
        CHECK(run_view_capture(binary,
                               {"--group-by", "cat", "--agg", "count", set},
                               cap, shard_out) == 0);
        CHECK(run_view_capture(binary,
                               {"--group-by", "cat", "--agg", "count", both},
                               cap, direct_out) == 0);

        CHECK(!shard_out.empty());
        CHECK(sorted_lines(shard_out) == sorted_lines(direct_out));

        // The projection and row window apply to the merged table too.
        dstd::String shaped;
        CHECK(run_view_capture(binary,
                               {"--group-by", "cat", "--agg", "count,sum:dur",
                                "--select", "cat,count", "--limit", "1", set},
                               cap, shaped) == 0);
        CHECK(sorted_lines(shaped).size() == 1);
        CHECK(shaped.find("\"count\"") != dstd::String::npos);
        CHECK(shaped.find("sum_dur") == dstd::String::npos);
    }

    // --select projects raw (non-aggregate) events to the chosen fields,
    // SQL-style, flattening args - even on a fresh trace (lazy indexing).
    TEST_CASE("select projects raw event fields") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found; skipping");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        dstd::String dir = env.get_dir() + "/sel";
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        dstd::String pfw = dir + "/t.pfw";
        {
            dftu_utils_test::FileOut out(pfw);
            out << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"args":{"fhash":"abc","bytes":4096}})"
                << "\n";
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        (void)dstd::fs::remove(pfw);

        dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::String out;
        CHECK(run_view_capture(binary,
                               {gz, "-q", "name == \"read\"", "--select",
                                "name,fhash,bytes"},
                               cap, out) == 0);
        // Projected to exactly the selected fields (name top-level; fhash/bytes
        // lifted out of args); the unselected cat/pid/ts/dur are gone.
        CHECK(out.find("\"name\":\"read\"") != dstd::String::npos);
        CHECK(out.find("\"fhash\":\"abc\"") != dstd::String::npos);
        CHECK(out.find("\"bytes\":4096") != dstd::String::npos);
        CHECK(out.find("\"cat\"") == dstd::String::npos);
        CHECK(out.find("\"dur\"") == dstd::String::npos);
    }

    TEST_CASE("select projects the lines the DOM parser refuses") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found; skipping");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        dstd::String dir = env.get_dir() + "/sel_refused";
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        dstd::String pfw = dir + "/t.pfw";
        {
            dftu_utils_test::FileOut out(pfw);
            for (int k = 0; k < dftu_utils_test::REFUSED_KINDS; ++k)
                out << dftu_utils_test::refused_line(
                           k, R"("ph":"X","name":"read","cat":"POSIX","pid":1,)"
                              R"("tid":1,"ts":1000,"dur":10)")
                    << "\n";
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        (void)dstd::fs::remove(pfw);

        dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::String out;
        CHECK(run_view_capture(
                  binary,
                  {gz, "-q", "name == \"read\"", "--select", "name,ret"}, cap,
                  out) == 0);
        std::size_t projected = 0;
        for (auto at = out.find("{\"name\":\"read\",\"ret\":4096}");
             at != dstd::String::npos;
             at = out.find("{\"name\":\"read\",\"ret\":4096}", at + 1))
            ++projected;
        CHECK(projected == dftu_utils_test::REFUSED_KINDS);
        CHECK(out.find("\"cat\"") == dstd::String::npos);
    }

    TEST_CASE("select keeps a value nested 100000 deep as its raw text") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found; skipping");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        dstd::String dir = env.get_dir() + "/sel_deep";
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        dstd::String pfw = dir + "/t.pfw";
        {
            dftu_utils_test::FileOut out(pfw);
            dstd::String line =
                R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,)"
                R"("ts":1000,"dur":10,"args":{"ret":4096,"deep":)";
            line.append(100000, '[');
            line.append(100000, ']');
            out << line << "}}\n";
        }
        dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        (void)dstd::fs::remove(pfw);

        dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::String out;
        CHECK(run_view_capture(
                  binary,
                  {gz, "-q", "name == \"read\"", "--select", "name,ret,deep"},
                  cap, out) == 0);
        CHECK(out.find("\"name\":\"read\"") != dstd::String::npos);
        CHECK(out.find("\"ret\":4096") != dstd::String::npos);
        dstd::String text;
        text.append(100000 - 127, '[');
        text.append(100000 - 127, ']');
        CHECK(out.find(text) != dstd::String::npos);
    }

    TEST_CASE("--query runs a pipeline with --param and --explain") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        const dstd::String gz = create_pfw_gz(env, 50, 0);
        REQUIRE(!gz.empty());
        const dstd::String cap = env.get_dir() + "/cap.txt";
        const dstd::String q = "sort -ts | take $n | select ts";

        dstd::String out;
        CHECK(run_view_capture(binary, {gz, "-q", q, "--param", "n=3"}, cap,
                               out) == 0);
        dstd::Vector<long long> ts;
        for (std::size_t at = out.find("\"ts\":"); at != dstd::String::npos;
             at = out.find("\"ts\":", at + 1))
            ts.push_back(
                dftu_utils_test::to_number<long long>(out.substr(at + 5)));
        REQUIRE(ts.size() == 3);
        CHECK(ts[0] >= ts[1]);
        CHECK(ts[1] >= ts[2]);

        dstd::String plan;
        CHECK(run_view_capture(binary,
                               {gz, "-q", q, "--param", "n=3", "--explain"},
                               cap, plan) == 0);
        CHECK(plan ==
              "scan select: ts\nscan order: file, then line\nsort_by_multi "
              "ts\nhead 3\nselect ts\n");

        CHECK(run_view(binary, {gz, "-q", q}) != 0);
        CHECK(run_view(binary, {gz, "-q", q, "--param", "n"}) != 0);
    }

    TEST_CASE("--query runs a semi-join, an arrow and a lookup") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        const dstd::String gz = create_pfw_gz(env, 50, 0);
        REQUIRE(!gz.empty());
        const dstd::String cap = env.get_dir() + "/cap.txt";
        auto rows = [&](const dstd::String& q) {
            dstd::String out;
            CHECK(run_view_capture(binary, {gz, "-q", q}, cap, out) == 0);
            std::size_t n = 0;
            for (std::size_t at = out.find("\"cat\":");
                 at != dstd::String::npos; at = out.find("\"cat\":", at + 1))
                ++n;
            return n;
        };
        const std::size_t posix = rows(R"(where cat == "POSIX" | select cat)");
        CHECK(posix == 38);
        CHECK(
            rows(
                R"(where cat in (from data | where cat == "POSIX" | select cat) | select cat)") ==
            posix);
        const dstd::String let =
            R"(let one = where cat == "POSIX" | take 1 | select cat, k = 1; )";
        CHECK(rows(let + "where cat -> one.k == 1 | select cat") == posix);
        CHECK(rows(let + "lookup one on cat | where k == 1 | select cat") ==
              posix);
        dstd::String plan;
        CHECK(run_view_capture(
                  binary,
                  {gz, "-q", let + "where cat -> one.k == 1", "--explain"}, cap,
                  plan) == 0);
        CHECK(plan.find("side one: from data") != dstd::String::npos);
    }

    TEST_CASE("--query writes the events an arrow filter keeps") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        const dstd::String pfw = env.get_dir() + "/files.pfw";
        {
            dftu_utils_test::FileOut o(pfw);
            o << R"({"ph":"M","name":"FH","pid":1,"tid":1,"args":{"name":"/scratch/a","value":"f1"}})"
              << "\n"
              << R"({"ph":"M","name":"FH","pid":1,"tid":1,"args":{"name":"/data/b","value":"f2"}})"
              << "\n";
            for (int i = 0; i < 4; ++i)
                o << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":)"
                  << 10 * (i + 1) << R"(,"dur":1,"args":{"fhash":"f)"
                  << 1 + i % 2 << "\"}}\n";
        }
        const dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        const dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::String out;
        CHECK(run_view_capture(
                  binary,
                  {gz, "-q", R"(fhash -> files.path like "/scratch/%")"}, cap,
                  out) == 0);
        std::size_t events = 0;
        for (std::size_t at = out.find(R"("ph":"X")"); at != dstd::String::npos;
             at = out.find(R"("ph":"X")", at + 1))
            ++events;
        CHECK(events == 2);
        CHECK(out.find(R"("fhash":"f2")") == dstd::String::npos);
        CHECK(out.find(R"("ph":"M")") == dstd::String::npos);
    }

    TEST_CASE("--query prints the table of a group with --param") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        const dstd::String gz = create_pfw_gz(env, 50, 0);
        REQUIRE(!gz.empty());
        const dstd::String cap = env.get_dir() + "/cap.txt";
        const dstd::String q = "where dur >= $min | group cat { n = count() }";

        dstd::String out;
        CHECK(run_view_capture(binary, {gz, "-q", q, "--param", "min=0"}, cap,
                               out) == 0);
        long long total = 0;
        std::size_t groups = 0;
        for (std::size_t at = out.find("\"n\":"); at != dstd::String::npos;
             at = out.find("\"n\":", at + 1), ++groups)
            total += dftu_utils_test::to_number<long long>(out.substr(at + 4));
        CHECK(groups >= 1);
        CHECK(total == 50);
        CHECK(out.find("\"cat\":") != dstd::String::npos);

        dstd::String plan;
        CHECK(run_view_capture(binary,
                               {gz, "-q", q, "--param", "min=0", "--explain"},
                               cap, plan) == 0);
        CHECK(plan.find("group (trace plan): keys cat; aggs n = count()") !=
              dstd::String::npos);
    }

    TEST_CASE("--query runs window and expand") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        const dstd::String pfw = env.get_dir() + "/hosts.pfw";
        {
            dftu_utils_test::FileOut o(pfw);
            for (int i = 0; i < 3; ++i)
                o << R"({"ph":"X","name":"op","cat":"io","pid":1,"tid":1,)"
                  << R"("ts":)" << 10 * (i + 1) << R"(,"dur":1,"args":)"
                  << (i == 1 ? R"({"hosts":[]})" : R"({"hosts":["a","b"]})")
                  << "}\n";
        }
        const dstd::String gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        const dstd::String cap = env.get_dir() + "/cap.txt";
        const dstd::String q =
            "window cat sort ts { g = ts - lag(ts) } | expand hosts"
            " | select ts, g, hosts";
        dstd::String out;
        CHECK(run_view_capture(binary, {gz, "-q", q}, cap, out) == 0);
        CHECK(out.find(R"({"ts":10,"g":null,"hosts":"a"})") !=
              dstd::String::npos);
        CHECK(out.find(R"({"ts":30,"g":10,"hosts":"b"})") !=
              dstd::String::npos);
        CHECK(out.find(R"("ts":20)") == dstd::String::npos);

        dstd::String plan;
        CHECK(run_view_capture(binary, {gz, "-q", q, "--explain"}, cap, plan) ==
              0);
        CHECK(plan.find("window keys cat; sort ts; g = ts - lag(ts)\n") !=
              dstd::String::npos);
        CHECK(plan.find("expand hosts as hosts\n") != dstd::String::npos);
    }

    // A 1-byte budget forces a spill; the result must match the in-memory run.
    TEST_CASE("memory-budget spill matches --no-spill") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50, 0).empty());

        dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::String spilled, in_mem;
        dstd::Vector<dstd::String> agg = {"--group-by", "name", "--agg",
                                          "count,sum:dur"};
        dstd::Vector<dstd::String> a1 = agg;
        a1.insert(a1.end(), {"--memory-budget", "1", env.get_dir()});
        dstd::Vector<dstd::String> a2 = agg;
        a2.insert(a2.end(), {"--no-spill", env.get_dir()});
        CHECK(run_view_capture(binary, a1, cap, spilled) == 0);
        CHECK(run_view_capture(binary, a2, cap, in_mem) == 0);
        CHECK(!spilled.empty());
        CHECK(sorted_lines(spilled) == sorted_lines(in_mem));
    }

    TEST_CASE("time-scale runs") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50, 0).empty());

        CHECK(run_view(binary, {"--group-by", "cat", "--agg", "sum:dur",
                                "--time-scale", "1000", env.get_dir()}) == 0);
    }

    TEST_CASE("select projects out unselected columns") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50, 0).empty());

        dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::String full, projected;
        dstd::Vector<dstd::String> agg = {"--group-by", "cat", "--agg",
                                          "count,sum:dur"};
        dstd::Vector<dstd::String> a1 = agg;
        a1.insert(a1.end(), {env.get_dir()});
        dstd::Vector<dstd::String> a2 = agg;
        a2.insert(a2.end(), {"--select", "cat,count", env.get_dir()});
        CHECK(run_view_capture(binary, a1, cap, full) == 0);
        CHECK(run_view_capture(binary, a2, cap, projected) == 0);
        CHECK(full.find("sum_dur") != dstd::String::npos);
        CHECK(projected.find("sum_dur") == dstd::String::npos);  // dropped
        CHECK(projected.find("count") != dstd::String::npos);    // kept
    }

    // --materialize persists a rollup; the identical query must then return the
    // same rows (served from the rollup instead of a scan).
    TEST_CASE("materialize persists a reusable rollup") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50, 0).empty());

        dstd::String cap = env.get_dir() + "/cap.txt";
        dstd::Vector<dstd::String> q = {"--group-by", "cat", "--agg",
                                        "count,sum:dur", env.get_dir()};
        dstd::String baseline, requery;
        CHECK(run_view_capture(binary, q, cap, baseline) == 0);

        dstd::Vector<dstd::String> mat = {"--group-by",    "cat",
                                          "--agg",         "count,sum:dur",
                                          "--materialize", env.get_dir()};
        CHECK(run_view(binary, mat) == 0);

        CHECK(run_view_capture(binary, q, cap, requery) == 0);
        CHECK(!baseline.empty());
        CHECK(sorted_lines(baseline) == sorted_lines(requery));
    }

    // The build made the aggregation tier too, so the typed families exist;
    // the --collect-typed terminal must still run cleanly.
    TEST_CASE("collect-typed runs") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50, 0).empty());

        CHECK(run_view(binary, {"--group-by", "cat", "--agg", "count",
                                "--collect-typed", env.get_dir()}) == 0);
    }

    // --materialize is meaningless without an aggregation and must be rejected.
    TEST_CASE("materialize without aggregation is rejected") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 50, 0).empty());

        CHECK(run_view(binary, {"-q", R"(cat == "POSIX")", "--materialize",
                                env.get_dir()}) != 0);
    }

    // A single-member input larger than the checkpoint size is read through
    // the restart points of its index: nothing is rewritten and the original
    // file is never touched. A tiny --checkpoint-size makes the small fixture
    // a big member.
    TEST_CASE("single-member input over the checkpoint size is read in place") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        dstd::String orig = create_pfw_gz(env, 50, 0);
        REQUIRE(!orig.empty());
        const auto orig_size = dstd::fs::file_size(orig).value();

        int rc = run_view(binary, {"--group-by", "cat", "--agg", "count",
                                   "--checkpoint-size", "2048", env.get_dir()});
        CHECK(rc == 0);
        CHECK(dstd::fs::file_size(orig).value() == orig_size);
        CHECK_FALSE(dstd::fs::exists(env.get_dir() + "/split").value());
        CHECK_FALSE(
            dstd::fs::exists(env.get_dir() + "/.dftindex.split").value());
    }

    // A multi-member input whose members already fit under the checkpoint size
    // is left alone (no split copy).
    TEST_CASE("multi-member input under the cap is read in place") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!make_multimember_pfw_gz(env, 200, 4096, 0).empty());

        int rc =
            run_view(binary, {"--group-by", "cat", "--agg", "count",
                              "--checkpoint-size", "1048576", env.get_dir()});
        CHECK(rc == 0);
        CHECK_FALSE(dstd::fs::exists(env.get_dir() + "/split").value());
        CHECK_FALSE(
            dstd::fs::exists(env.get_dir() + "/.dftindex.split").value());
    }
}

// ============================================================================
// append_cell_json: one explicit case per dataframe::TypeId
// ============================================================================

TEST_SUITE("DFTracerView JSON cell printer") {
    TEST_CASE("bit-packed Bool reads the right bit at every index") {
        // 9 rows so row 8 crosses into the packed array's second byte; a
        // per-row 8-byte reinterpret (the old default: case) would run off
        // the 2-byte packed buffer entirely.
        auto c = make_bool_series(
            {true, false, true, true, false, false, true, false, true});
        CHECK(cell(c, 0) == "true");
        CHECK(cell(c, 1) == "false");
        CHECK(cell(c, 7) == "false");
        CHECK(cell(c, 8) == "true");
    }

    TEST_CASE("narrow signed ints read at their own width") {
        auto i8 = dataframe::Series::flat(
            dataframe::TypeId::Int8,
            dstd::Vector<std::int8_t>{-128, 127, 5}.data(), 3);
        CHECK(cell(i8, 0) == "-128");
        CHECK(cell(i8, 1) == "127");
        CHECK(cell(i8, 2) == "5");

        auto i16 = dataframe::Series::flat(
            dataframe::TypeId::Int16,
            dstd::Vector<std::int16_t>{-32768, 32767, 100}.data(), 3);
        CHECK(cell(i16, 0) == "-32768");
        CHECK(cell(i16, 1) == "32767");

        auto i32 = dataframe::Series::flat(
            dataframe::TypeId::Int32,
            dstd::Vector<std::int32_t>{-2147483647 - 1, 2147483647, 42}.data(),
            3);
        CHECK(cell(i32, 0) == "-2147483648");
        CHECK(cell(i32, 1) == "2147483647");
    }

    TEST_CASE("narrow unsigned ints read at their own width") {
        auto u8 = dataframe::Series::flat(
            dataframe::TypeId::Uint8,
            dstd::Vector<std::uint8_t>{0, 255, 10}.data(), 3);
        CHECK(cell(u8, 0) == "0");
        CHECK(cell(u8, 1) == "255");

        auto u16 = dataframe::Series::flat(
            dataframe::TypeId::Uint16,
            dstd::Vector<std::uint16_t>{0, 65535, 10}.data(), 3);
        CHECK(cell(u16, 1) == "65535");

        auto u32 = dataframe::Series::flat(
            dataframe::TypeId::Uint32,
            dstd::Vector<std::uint32_t>{0, 4294967295u, 10}.data(), 3);
        CHECK(cell(u32, 1) == "4294967295");

        auto u64 = dataframe::Series::flat(
            dataframe::TypeId::Uint64,
            dstd::Vector<std::uint64_t>{0, 18446744073709551615ull, 10}.data(),
            3);
        CHECK(cell(u64, 1) == "18446744073709551615");
    }

    TEST_CASE("Float32 reads 4 bytes per row, not 8") {
        auto f32 = dataframe::Series::flat(
            dataframe::TypeId::Float32,
            dstd::Vector<float>{1.5f, -2.25f, 3.0f}.data(), 3);
        CHECK(cell(f32, 0) == "1.500000");
        CHECK(cell(f32, 1) == "-2.250000");
        // Integral value prints bare, matching the pre-existing convention.
        CHECK(cell(f32, 2) == "3");
    }

    TEST_CASE("Float64 reads correctly") {
        auto f64 = dataframe::Series::flat(
            dataframe::TypeId::Float64,
            dstd::Vector<double>{1.5, -2.25, 3.0}.data(), 3);
        CHECK(cell(f64, 0) == "1.500000");
        CHECK(cell(f64, 2) == "3");
    }

    TEST_CASE("Float16 promotes to float32 before printing") {
        auto f16 = make_float16_series({1.5f, -2.0f});
        CHECK(cell(f16, 0) == "1.500000");
        CHECK(cell(f16, 1) == "-2");
    }

    TEST_CASE("String escapes quotes, backslashes, and control chars") {
        auto s = make_bytes_series(dataframe::TypeId::String,
                                   {dstd::String("a\"b\\c") + '\x01' + "d"});
        CHECK(cell(s, 0) == "\"a\\\"b\\\\c\\u0001d\"");
    }

    TEST_CASE("Binary is escaped the same way String is") {
        auto b = make_bytes_series(dataframe::TypeId::Binary, {"a\tb"});
        CHECK(cell(b, 0) == "\"a\\tb\"");
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW
    TEST_CASE("FixedSizeBinary reads its declared width") {
        auto fsb = dataframe::test_types::make_fixed_size_binary({"ab\"c"}, 4);
        CHECK(cell(fsb, 0) == "\"ab\\\"c\"");
    }

    TEST_CASE("LargeString/LargeBinary read via the 64-bit offsets buffer") {
        auto ls = dataframe::test_types::make_large_utf8({"hello\\world"});
        CHECK(cell(ls, 0) == "\"hello\\\\world\"");
        auto lb = dataframe::test_types::make_large_binary({"a\nb"});
        CHECK(cell(lb, 0) == "\"a\\nb\"");
    }
#endif

    TEST_CASE("temporal types emit the exact physical integer") {
        auto date32 = dataframe::Series::flat(
            dataframe::TypeId::Date32,
            dstd::Vector<std::int32_t>{19700, -5}.data(), 2);
        CHECK(cell(date32, 0) == "19700");
        CHECK(cell(date32, 1) == "-5");

        auto date64 = dataframe::Series::flat(
            dataframe::TypeId::Date64,
            dstd::Vector<std::int64_t>{1700000000000LL}.data(), 1);
        CHECK(cell(date64, 0) == "1700000000000");

        auto time32 =
            dataframe::Series::flat(dataframe::TypeId::Time32,
                                    dstd::Vector<std::int32_t>{3600}.data(), 1);
        CHECK(cell(time32, 0) == "3600");

        auto time64 = dataframe::Series::flat(
            dataframe::TypeId::Time64,
            dstd::Vector<std::int64_t>{123456789LL}.data(), 1);
        CHECK(cell(time64, 0) == "123456789");

        auto ts = dataframe::Series::flat(
            dataframe::TypeId::Timestamp,
            dstd::Vector<std::int64_t>{1700000000000000LL}.data(), 1);
        CHECK(cell(ts, 0) == "1700000000000000");

        auto dur = dataframe::Series::flat(
            dataframe::TypeId::Duration,
            dstd::Vector<std::int64_t>{-42LL}.data(), 1);
        CHECK(cell(dur, 0) == "-42");
    }

    TEST_CASE("Decimal128/256 render the exact value, not a lossy double") {
        // INT64_MAX * 1000 + 807: 22 digits, well past a double's ~17
        // significant digits, so a lossy path would visibly round this.
        const dstd::int128_t big =
            static_cast<dstd::int128_t>(9223372036854775807LL) * 1000 + 807;
        // Rendered as a quoted JSON string: a JSON number cannot hold a
        // precision-38 decimal exactly once a double consumer parses it.
        auto d128 = make_decimal128_series({big, -big, 0});
        CHECK(cell(d128, 0) == "\"9223372036854775807807\"");
        CHECK(cell(d128, 1) == "\"-9223372036854775807807\"");
        CHECK(cell(d128, 2) == "\"0\"");

        auto d256 = make_decimal256_series({big, -big});
        CHECK(cell(d256, 0) == "\"9223372036854775807807\"");
        CHECK(cell(d256, 1) == "\"-9223372036854775807807\"");
    }

    TEST_CASE("nested and Unknown types render null, not garbage") {
        auto unk = make_unknown_series(1);
        CHECK(cell(unk, 0) == "null");
    }

    TEST_CASE("null cells, lists and structs render as JSON") {
        auto nulls = dataframe::Series::nulls(dataframe::TypeId::Int64, 2);
        CHECK(cell(nulls, 1) == "null");
        auto text = dataframe::Series::nulls(dataframe::TypeId::String, 1);
        CHECK(cell(text, 0) == "null");

        dstd::Vector<dataframe::Series> fields;
        fields.push_back(dataframe::Series::flat_i64(
            dstd::Vector<std::int64_t>{1, 2, 3}.data(), 3));
        fields.push_back(dataframe::Series::strings({"a", "b", "c"}));
        auto rec = dataframe::Series::structs({"n", "s"}, std::move(fields));
        auto list = dataframe::Series::list({0, 2, 2, 3}, std::move(rec));
        CHECK(cell(list, 0) == R"([{"n":1,"s":"a"},{"n":2,"s":"b"}])");
        CHECK(cell(list, 1) == "[]");
        CHECK(cell(list, 2) == R"([{"n":3,"s":"c"}])");
    }

    TEST_CASE("cli_emittable leaves out the types it cannot render") {
        CHECK(
            dftracer::utils::binaries::cli_emittable(dataframe::TypeId::List));
        CHECK(dftracer::utils::binaries::cli_emittable(
            dataframe::TypeId::Struct));
        CHECK(dftracer::utils::binaries::cli_emittable(
            dataframe::TypeId::LargeList));
        CHECK_FALSE(dftracer::utils::binaries::cli_emittable(
            dataframe::TypeId::FixedSizeList));
        CHECK_FALSE(
            dftracer::utils::binaries::cli_emittable(dataframe::TypeId::Map));
        CHECK(
            dftracer::utils::binaries::cli_emittable(dataframe::TypeId::Int64));
        CHECK(
            dftracer::utils::binaries::cli_emittable(dataframe::TypeId::Bool));
        CHECK(dftracer::utils::binaries::cli_emittable(
            dataframe::TypeId::Decimal128));
    }
}
