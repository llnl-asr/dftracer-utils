#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dataframe/builders_arrow_types.h>
#include <dftracer/utils/binaries/json_cell_printer.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/float16.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/trace/aggregators/aggregator_utility.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/sharded_view.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

std::string create_pfw_gz(dftu_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

// Rechunk a single-member fixture into a multi-member trace whose members are
// about `member_bytes` uncompressed, for exercising ingest member handling.
std::string make_multimember_pfw_gz(dftu_utils_test::TestEnvironment& env,
                                    int num_events, std::size_t member_bytes,
                                    int id) {
    std::string single = create_pfw_gz(env, num_events, id);
    if (single.empty()) return "";
    std::string mm = env.get_dir() + "/mm_" + std::to_string(id) + ".pfw.gz";
    namespace gzc = dftracer::utils::utilities::fileio::compress;
    gzc::gzip_rechunk_to_members(single, mm, member_bytes, 6).get();
    fs::remove(single);
    return mm;
}

std::string find_view_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_VIEW_PATH",
                                                "dftracer_view");
}

int run_view(const std::string& binary, const std::vector<std::string>& args) {
    std::vector<const char*> argv;
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
int run_view_capture(const std::string& binary,
                     const std::vector<std::string>& args,
                     const std::string& capture_path, std::string& out) {
    std::vector<const char*> argv;
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
    std::ifstream ifs(capture_path);
    out.assign((std::istreambuf_iterator<char>(ifs)),
               std::istreambuf_iterator<char>());
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

std::size_t count_lines(const std::string& s) {
    return static_cast<std::size_t>(std::count(s.begin(), s.end(), '\n'));
}

// ----------------------------------------------------------------------------
// append_cell_json builders: cover every dataframe::TypeId directly, since
// no trace field the CLI's own query/aggregation paths produce carries the
// narrow-int, Float16, temporal, decimal, or Large*/FixedSizeBinary types.
// ----------------------------------------------------------------------------

namespace df = dftracer::utils::dataframe;
namespace cli = dftracer::utils::binaries;

__extension__ typedef unsigned __int128 uint128_t;
__extension__ typedef __int128 int128_t;

df::Series make_bool_series(const std::vector<bool>& bits) {
    std::vector<std::uint8_t> packed((bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i)
        if (bits[i]) packed[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return df::Series::flat(df::TypeId::Bool, packed.data(),
                            static_cast<std::int64_t>(bits.size()));
}

// A String/Binary FLAT column; dftu_series_new_string supports both dtypes.
df::Series make_bytes_series(df::TypeId type,
                             const std::vector<std::string>& values) {
    std::vector<std::int32_t> offsets(values.size() + 1, 0);
    std::string data;
    for (std::size_t i = 0; i < values.size(); ++i) {
        data += values[i];
        offsets[i + 1] = static_cast<std::int32_t>(data.size());
    }
    return df::Series{dftu_series_new_string(
        static_cast<dftu_dtype>(type), offsets.data(), data.data(),
        static_cast<std::int64_t>(values.size()), nullptr)};
}

df::Series make_float16_series(const std::vector<float>& values) {
    std::vector<std::uint16_t> halves(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        halves[i] = df::float_to_half(values[i]);
    return df::Series::flat(df::TypeId::Float16, halves.data(),
                            static_cast<std::int64_t>(values.size()));
}

// Decimal128: exact little-endian two's-complement payload, decimal_scale 0
// (the only scale a FLAT-constructed decimal column carries; nonzero scale
// only reaches the engine via Arrow import, unreachable from this CLI).
df::Series make_decimal128_series(const std::vector<int128_t>& values) {
    std::vector<std::uint8_t> bytes(values.size() * 16);
    for (std::size_t i = 0; i < values.size(); ++i)
        std::memcpy(bytes.data() + i * 16, &values[i], 16);
    return df::Series::flat(df::TypeId::Decimal128, bytes.data(),
                            static_cast<std::int64_t>(values.size()));
}

// Decimal256: `values` sign-extended from 128 to 256 bits.
df::Series make_decimal256_series(const std::vector<int128_t>& values) {
    std::vector<std::uint8_t> bytes(values.size() * 32, 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::memcpy(bytes.data() + i * 32, &values[i], 16);
        if (values[i] < 0) std::memset(bytes.data() + i * 32 + 16, 0xFF, 16);
    }
    return df::Series::flat(df::TypeId::Decimal256, bytes.data(),
                            static_cast<std::int64_t>(values.size()));
}

df::Series make_unknown_series(std::int64_t n) {
    auto* raw = new dftu_series();
    raw->type = df::TypeId::Unknown;
    raw->length = n;
    return df::Series(raw);
}

std::string cell(const df::Series& c, std::int64_t i) {
    std::string s;
    cli::append_cell_json(s, c, i);
    return s;
}

// Create a trace in its own subdir and build an aggregated index (tier) over it
// via the aggregator; returns the shard's .dftindex path.
std::string build_aggregated_shard(dftu_utils_test::TestEnvironment& env,
                                   const std::string& tag, int num_events) {
    namespace agg = dftracer::utils::trace::aggregators;
    namespace internal = dftracer::utils::trace::internal;
    std::string dir = env.get_dir() + "/" + tag;
    fs::create_directories(dir);
    std::string src = env.create_dft_test_gzip_file(num_events);
    std::string gz = dir + "/t.pfw.gz";
    fs::rename(src, gz);

    agg::AggregatorInput input;
    input.directory = dir;
    input.force_rebuild = true;
    dftracer::utils::Runtime rt(4);
    auto task = dftracer::utils::run_coro_scope(
        rt.executor(),
        [&](dftracer::utils::CoroScope& ctx)
            -> dftracer::utils::coro::CoroTask<void> {
            agg::AggregatorUtility u;
            auto gen = u(ctx, input);
            while (auto batch = co_await gen.next()) (void)batch;
            co_return;
        });
    rt.submit(std::move(task), "build-shard").wait();
    rt.shutdown();
    return internal::determine_index_path(gz, "");
}

// Split into non-empty lines and sort, so two aggregations can be compared
// regardless of row emission order.
std::vector<std::string> sorted_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::string cur;
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
                "Set DFTRACER_VIEW_PATH env to specify location.");
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

        int rc = run_view(binary, {"--query", R"(cat == "POSIX")", "--stream",
                                   "--no-metadata", "-d", env.get_dir()});
        CHECK(rc == 0);
    }

    TEST_CASE("query filter") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }

        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        auto f = create_pfw_gz(env, 50, 0);
        REQUIRE(!f.empty());

        int rc = run_view(binary, {"--query", R"(cat == "POSIX")", "--stream",
                                   "--no-metadata", "-d", env.get_dir()});
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

        int rc = run_view(binary, {"--group-by", "cat", "--agg",
                                   "p99:dur,pct:dur:0.95,skew:dur,kurt:dur",
                                   "-d", env.get_dir()});
        CHECK(rc == 0);

        // pct without a quantile and pNN without a field are rejected.
        CHECK(run_view(binary, {"--group-by", "cat", "--agg", "pct:dur", "-d",
                                env.get_dir()}) != 0);
        CHECK(run_view(binary, {"--group-by", "cat", "--agg", "p99", "-d",
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
        std::string output = env.get_dir() + "/view_output.pfw";
        int rc =
            run_view(binary, {"--query", R"(cat == "POSIX")", "--no-metadata",
                              "--no-index", "-d", env.get_dir(), "-o", output});
        CHECK(rc == 0);
        REQUIRE(fs::exists(output + ".gz"));
        CHECK(fs::file_size(output + ".gz") > 0);
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
        std::string in = env.get_dir() + "/in";
        fs::create_directories(in);
        auto trace_gz = env.create_dft_test_gzip_file(50);
        REQUIRE(!trace_gz.empty());
        fs::rename(trace_gz, in + "/trace_0.pfw.gz");

        // Default write path builds the index during the write (fused).
        // --verify re-scans the written+indexed trace via that index and checks
        // the event count round-trips, so rc==0 proves the fused member index
        // is correct.
        std::string output = env.get_dir() + "/merged.pfw";
        int rc = run_view(binary, {"-d", in, "--merge", "--verify",
                                   "--no-metadata", "-o", output});
        CHECK(rc == 0);
        REQUIRE(fs::exists(output + ".gz"));
        // The index was built by the write, not a separate pass: its .dftindex
        // directory must exist next to the output.
        CHECK(fs::exists(env.get_dir() + "/.dftindex"));
    }

    TEST_CASE("query with name filter") {
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
            binary,
            {"--query", R"(cat == "POSIX" and name in ["pread", "pwrite"])",
             "--stream", "-d", env.get_dir()});
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

        std::string cap = env.get_dir() + "/cap.txt";
        std::string full, narrow;
        CHECK(run_view_capture(binary,
                               {"--query", "ts >= 0", "--stream",
                                "--no-metadata", "-d", env.get_dir()},
                               cap, full) == 0);
        CHECK(run_view_capture(binary,
                               {"--stream", "--no-metadata", "--time-range",
                                "0,1000200000", "-d", env.get_dir()},
                               cap, narrow) == 0);
        // The window keeps only i=1,2; the unfiltered stream keeps them all.
        CHECK(count_lines(narrow) == 2);
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

        std::string cap = env.get_dir() + "/cap.txt";
        std::string plain, numeric;
        CHECK(run_view_capture(
                  binary,
                  {"--group-by", "cat", "--agg", "count", "-d", env.get_dir()},
                  cap, plain) == 0);
        CHECK(run_view_capture(binary,
                               {"--group-by", "cat", "--agg-numeric-args", "-d",
                                env.get_dir()},
                               cap, numeric) == 0);
        CHECK(!numeric.empty());
        // The fixture carries a numeric arg, so --agg-numeric-args adds a value
        // column that a bare count does not.
        CHECK(numeric.size() > plain.size());
    }

    // Pointing -d at a shard-set root (a dir with shards.json) autodetects it
    // and answers the aggregate over the immutable shards, matching a direct
    // query over the same traces.
    TEST_CASE("shard-set query matches a direct query") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found; skipping");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());

        std::string sa = build_aggregated_shard(env, "sa", 50);
        std::string sb = build_aggregated_shard(env, "sb", 40);
        std::string root = env.get_dir() + "/set";
        dftracer::utils::trace::views::write_shard_set(root, {sa, sb});

        // A flat directory with the same two traces for the baseline query.
        std::string both = env.get_dir() + "/both";
        fs::create_directories(both);
        fs::copy_file(env.get_dir() + "/sa/t.pfw.gz", both + "/a.pfw.gz");
        fs::copy_file(env.get_dir() + "/sb/t.pfw.gz", both + "/b.pfw.gz");

        std::string cap = env.get_dir() + "/cap.txt";
        std::string shard_out, direct_out;
        CHECK(run_view_capture(
                  binary, {"--group-by", "cat", "--agg", "count", "-d", root},
                  cap, shard_out) == 0);
        CHECK(run_view_capture(
                  binary, {"--group-by", "cat", "--agg", "count", "-d", both},
                  cap, direct_out) == 0);

        CHECK(!shard_out.empty());
        CHECK(sorted_lines(shard_out) == sorted_lines(direct_out));
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
        std::string dir = env.get_dir() + "/sel";
        fs::create_directories(dir);
        std::string pfw = dir + "/t.pfw";
        {
            std::ofstream out(pfw);
            out << R"({"ph":"X","name":"read","cat":"POSIX","pid":1,"tid":1,"ts":1000,"dur":10,"args":{"fhash":"abc","bytes":4096}})"
                << "\n";
        }
        std::string gz = pfw + ".gz";
        REQUIRE(dftu_utils_test::compress_file_to_gzip(pfw, gz));
        fs::remove(pfw);

        std::string cap = env.get_dir() + "/cap.txt";
        std::string out;
        CHECK(run_view_capture(binary,
                               {"--files", gz, "--query", "name == \"read\"",
                                "--select", "name,fhash,bytes"},
                               cap, out) == 0);
        // Projected to exactly the selected fields (name top-level; fhash/bytes
        // lifted out of args); the unselected cat/pid/ts/dur are gone.
        CHECK(out.find("\"name\":\"read\"") != std::string::npos);
        CHECK(out.find("\"fhash\":\"abc\"") != std::string::npos);
        CHECK(out.find("\"bytes\":4096") != std::string::npos);
        CHECK(out.find("\"cat\"") == std::string::npos);
        CHECK(out.find("\"dur\"") == std::string::npos);
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

        std::string cap = env.get_dir() + "/cap.txt";
        std::string spilled, in_mem;
        std::vector<std::string> agg = {"--group-by", "name", "--agg",
                                        "count,sum:dur"};
        std::vector<std::string> a1 = agg;
        a1.insert(a1.end(), {"--memory-budget", "1", "-d", env.get_dir()});
        std::vector<std::string> a2 = agg;
        a2.insert(a2.end(), {"--no-spill", "-d", env.get_dir()});
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
                                "--time-scale", "1000", "-d", env.get_dir()}) ==
              0);
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

        std::string cap = env.get_dir() + "/cap.txt";
        std::string full, projected;
        std::vector<std::string> agg = {"--group-by", "cat", "--agg",
                                        "count,sum:dur"};
        std::vector<std::string> a1 = agg;
        a1.insert(a1.end(), {"-d", env.get_dir()});
        std::vector<std::string> a2 = agg;
        a2.insert(a2.end(), {"--select", "cat,count", "-d", env.get_dir()});
        CHECK(run_view_capture(binary, a1, cap, full) == 0);
        CHECK(run_view_capture(binary, a2, cap, projected) == 0);
        CHECK(full.find("sum_dur") != std::string::npos);
        CHECK(projected.find("sum_dur") == std::string::npos);  // dropped
        CHECK(projected.find("count") != std::string::npos);    // kept
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

        std::string cap = env.get_dir() + "/cap.txt";
        std::vector<std::string> q = {"--group-by",    "cat", "--agg",
                                      "count,sum:dur", "-d",  env.get_dir()};
        std::string baseline, requery;
        CHECK(run_view_capture(binary, q, cap, baseline) == 0);

        std::vector<std::string> mat = {
            "--group-by",    "cat", "--agg",      "count,sum:dur",
            "--materialize", "-d",  env.get_dir()};
        CHECK(run_view(binary, mat) == 0);

        CHECK(run_view_capture(binary, q, cap, requery) == 0);
        CHECK(!baseline.empty());
        CHECK(sorted_lines(baseline) == sorted_lines(requery));
    }

    // No aggregation-index tier in this fixture, so the families are empty, but
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
                                "--collect-typed", "-d", env.get_dir()}) == 0);
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

        CHECK(run_view(binary, {"--query", R"(cat == "POSIX")", "--materialize",
                                "-d", env.get_dir()}) != 0);
    }

    // A single-member input is normalized to bounded multi-member gzip in a
    // sibling split/ dir; the original file is never touched. A tiny
    // --checkpoint-size forces the rechunk on the small fixture.
    TEST_CASE("single-member input auto-splits non-destructively") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        std::string orig = create_pfw_gz(env, 50, 0);
        REQUIRE(!orig.empty());
        const auto orig_size = fs::file_size(orig);

        int rc = run_view(
            binary, {"--group-by", "cat", "--agg", "count", "--checkpoint-size",
                     "2048", "-d", env.get_dir()});
        CHECK(rc == 0);

        const std::string split_copy =
            env.get_dir() + "/split/" + fs::path(orig).filename().string();
        CHECK(fs::exists(split_copy));            // multi-member copy created
        CHECK(fs::file_size(orig) == orig_size);  // original untouched
    }

    // A multi-member input whose members already fit under the checkpoint size
    // is left alone (no split/ dir).
    TEST_CASE("multi-member input under the cap is not re-split") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!make_multimember_pfw_gz(env, 200, 4096, 0).empty());

        int rc = run_view(
            binary, {"--group-by", "cat", "--agg", "count", "--checkpoint-size",
                     "1048576", "-d", env.get_dir()});
        CHECK(rc == 0);
        CHECK_FALSE(fs::exists(env.get_dir() + "/split"));
    }

    // A multi-member input whose members exceed the checkpoint size is
    // re-normalized to bounded members in split/.
    TEST_CASE("multi-member input over the cap is re-split") {
        auto binary = find_view_binary();
        if (binary.empty()) {
            MESSAGE("dftracer_view binary not found, skipping.");
            return;
        }
        dftu_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        std::string mm = make_multimember_pfw_gz(env, 200, 8192, 0);
        REQUIRE(!mm.empty());

        int rc = run_view(
            binary, {"--group-by", "cat", "--agg", "count", "--checkpoint-size",
                     "2048", "-d", env.get_dir()});
        CHECK(rc == 0);
        CHECK(fs::exists(env.get_dir() + "/split/" +
                         fs::path(mm).filename().string()));
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
        auto i8 = df::Series::flat(
            df::TypeId::Int8, std::vector<std::int8_t>{-128, 127, 5}.data(), 3);
        CHECK(cell(i8, 0) == "-128");
        CHECK(cell(i8, 1) == "127");
        CHECK(cell(i8, 2) == "5");

        auto i16 = df::Series::flat(
            df::TypeId::Int16,
            std::vector<std::int16_t>{-32768, 32767, 100}.data(), 3);
        CHECK(cell(i16, 0) == "-32768");
        CHECK(cell(i16, 1) == "32767");

        auto i32 = df::Series::flat(
            df::TypeId::Int32,
            std::vector<std::int32_t>{-2147483647 - 1, 2147483647, 42}.data(),
            3);
        CHECK(cell(i32, 0) == "-2147483648");
        CHECK(cell(i32, 1) == "2147483647");
    }

    TEST_CASE("narrow unsigned ints read at their own width") {
        auto u8 = df::Series::flat(
            df::TypeId::Uint8, std::vector<std::uint8_t>{0, 255, 10}.data(), 3);
        CHECK(cell(u8, 0) == "0");
        CHECK(cell(u8, 1) == "255");

        auto u16 = df::Series::flat(
            df::TypeId::Uint16, std::vector<std::uint16_t>{0, 65535, 10}.data(),
            3);
        CHECK(cell(u16, 1) == "65535");

        auto u32 = df::Series::flat(
            df::TypeId::Uint32,
            std::vector<std::uint32_t>{0, 4294967295u, 10}.data(), 3);
        CHECK(cell(u32, 1) == "4294967295");

        auto u64 = df::Series::flat(
            df::TypeId::Uint64,
            std::vector<std::uint64_t>{0, 18446744073709551615ull, 10}.data(),
            3);
        CHECK(cell(u64, 1) == "18446744073709551615");
    }

    TEST_CASE("Float32 reads 4 bytes per row, not 8") {
        auto f32 =
            df::Series::flat(df::TypeId::Float32,
                             std::vector<float>{1.5f, -2.25f, 3.0f}.data(), 3);
        CHECK(cell(f32, 0) == "1.500000");
        CHECK(cell(f32, 1) == "-2.250000");
        // Integral value prints bare, matching the pre-existing convention.
        CHECK(cell(f32, 2) == "3");
    }

    TEST_CASE("Float64 reads correctly") {
        auto f64 =
            df::Series::flat(df::TypeId::Float64,
                             std::vector<double>{1.5, -2.25, 3.0}.data(), 3);
        CHECK(cell(f64, 0) == "1.500000");
        CHECK(cell(f64, 2) == "3");
    }

    TEST_CASE("Float16 promotes to float32 before printing") {
        auto f16 = make_float16_series({1.5f, -2.0f});
        CHECK(cell(f16, 0) == "1.500000");
        CHECK(cell(f16, 1) == "-2");
    }

    TEST_CASE("String escapes quotes, backslashes, and control chars") {
        auto s = make_bytes_series(df::TypeId::String,
                                   {std::string("a\"b\\c") + '\x01' + "d"});
        CHECK(cell(s, 0) == "\"a\\\"b\\\\c\\u0001d\"");
    }

    TEST_CASE("Binary is escaped the same way String is") {
        auto b = make_bytes_series(df::TypeId::Binary, {"a\tb"});
        CHECK(cell(b, 0) == "\"a\\tb\"");
    }

    TEST_CASE("FixedSizeBinary reads its declared width") {
        auto fsb = df::test_types::make_fixed_size_binary({"ab\"c"}, 4);
        CHECK(cell(fsb, 0) == "\"ab\\\"c\"");
    }

    TEST_CASE("LargeString/LargeBinary read via the 64-bit offsets buffer") {
        auto ls = df::test_types::make_large_utf8({"hello\\world"});
        CHECK(cell(ls, 0) == "\"hello\\\\world\"");
        auto lb = df::test_types::make_large_binary({"a\nb"});
        CHECK(cell(lb, 0) == "\"a\\nb\"");
    }

    TEST_CASE("temporal types emit the exact physical integer") {
        auto date32 = df::Series::flat(
            df::TypeId::Date32, std::vector<std::int32_t>{19700, -5}.data(), 2);
        CHECK(cell(date32, 0) == "19700");
        CHECK(cell(date32, 1) == "-5");

        auto date64 = df::Series::flat(
            df::TypeId::Date64,
            std::vector<std::int64_t>{1700000000000LL}.data(), 1);
        CHECK(cell(date64, 0) == "1700000000000");

        auto time32 = df::Series::flat(
            df::TypeId::Time32, std::vector<std::int32_t>{3600}.data(), 1);
        CHECK(cell(time32, 0) == "3600");

        auto time64 =
            df::Series::flat(df::TypeId::Time64,
                             std::vector<std::int64_t>{123456789LL}.data(), 1);
        CHECK(cell(time64, 0) == "123456789");

        auto ts = df::Series::flat(
            df::TypeId::Timestamp,
            std::vector<std::int64_t>{1700000000000000LL}.data(), 1);
        CHECK(cell(ts, 0) == "1700000000000000");

        auto dur = df::Series::flat(df::TypeId::Duration,
                                    std::vector<std::int64_t>{-42LL}.data(), 1);
        CHECK(cell(dur, 0) == "-42");
    }

    TEST_CASE("Decimal128/256 render the exact value, not a lossy double") {
        // INT64_MAX * 1000 + 807: 22 digits, well past a double's ~17
        // significant digits, so a lossy path would visibly round this.
        const int128_t big =
            static_cast<int128_t>(9223372036854775807LL) * 1000 + 807;
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

    TEST_CASE("cli_emittable excludes every nested type") {
        CHECK_FALSE(cli::cli_emittable(df::TypeId::List));
        CHECK_FALSE(cli::cli_emittable(df::TypeId::Struct));
        CHECK_FALSE(cli::cli_emittable(df::TypeId::LargeList));
        CHECK_FALSE(cli::cli_emittable(df::TypeId::FixedSizeList));
        CHECK_FALSE(cli::cli_emittable(df::TypeId::Map));
        CHECK(cli::cli_emittable(df::TypeId::Int64));
        CHECK(cli::cli_emittable(df::TypeId::Bool));
        CHECK(cli::cli_emittable(df::TypeId::Decimal128));
    }
}
