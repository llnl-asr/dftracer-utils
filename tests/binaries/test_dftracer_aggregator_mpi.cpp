#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

std::string create_pfw_gz(dft_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

// Prefer env-provided path (set by CMake), fall back to the common build
// output locations.
std::string find_binary(const char* env_name,
                        const std::vector<std::string>& candidates) {
    const char* env_path = std::getenv(env_name);
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;
    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

std::string find_serial_binary() {
    return find_binary("DFTRACER_AGGREGATOR_PATH",
                       {
                           "./dftracer_aggregator",
                           "../dftracer_aggregator",
                           "../../dftracer_aggregator",
                           "../bin/dftracer_aggregator",
                           "../../bin/dftracer_aggregator",
                       });
}

std::string find_mpi_binary() {
    return find_binary("DFTRACER_AGGREGATOR_MPI_PATH",
                       {
                           "./dftracer_aggregator_mpi",
                           "../dftracer_aggregator_mpi",
                           "../../dftracer_aggregator_mpi",
                           "../bin/dftracer_aggregator_mpi",
                           "../../bin/dftracer_aggregator_mpi",
                       });
}

// Locate an MPI launcher on $PATH. We prefer mpiexec to line up with the
// CMake `MPIEXEC_EXECUTABLE` default; fall back to mpirun.
std::string find_mpi_launcher() {
    const char* env_path = std::getenv("MPIEXEC_EXECUTABLE");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;
    for (const auto& name : {"mpiexec", "mpirun"}) {
        std::string cmd = std::string("command -v ") + name + " 2>/dev/null";
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) continue;
        char buf[4096];
        std::string out;
        while (std::fgets(buf, sizeof(buf), p)) out += buf;
        ::pclose(p);
        while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
            out.pop_back();
        if (!out.empty() && ::access(out.c_str(), X_OK) == 0) return out;
    }
    return "";
}

int run_process(const std::string& binary,
                const std::vector<std::string>& args) {
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<const char*> argv;
        argv.push_back(binary.c_str());
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

int run_mpi(const std::string& launcher, int np, const std::string& binary,
            const std::vector<std::string>& binary_args) {
    std::vector<std::string> args = {"--allow-run-as-root", "-n",
                                     std::to_string(np), binary};
    for (const auto& a : binary_args) args.push_back(a);
    return run_process(launcher, args);
}

// Read a gzip-compressed file fully into memory (as a string).
std::string read_gz_to_string(const std::string& path) {
    gzFile gz = gzopen(path.c_str(), "rb");
    if (!gz) return {};
    std::string out;
    char buf[1 << 16];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<std::size_t>(n));
    }
    gzclose(gz);
    return out;
}

// Sort lines in `content` and return the sorted blob. Used to make output
// comparison independent of row ordering, which isn't guaranteed across
// rank counts (parallel scan traverses shard ranges in different orders).
std::string sort_lines(const std::string& content) {
    std::vector<std::string> lines;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) lines.push_back(std::move(line));
    std::sort(lines.begin(), lines.end());
    std::string out;
    out.reserve(content.size());
    for (auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

// Helper: read output and return sorted lines. Handles three shapes:
//   1. `path` itself ends with ".gz"          -> decompress `path`
//   2. `path` does not end in ".gz" but a
//      sibling `path.gz` exists               -> decompress `path.gz`
//   3. otherwise                               -> read `path` as plain text
// The aggregator writes gzip when `-o foo.json.gz` is used but plain text
// when `-o foo.json` is used (without `--compress`). The MPI binary
// always gzips its final output.
std::string read_output_sorted(const std::string& path) {
    const bool ends_with_gz =
        path.size() >= 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
    if (ends_with_gz && fs::exists(path)) {
        return sort_lines(read_gz_to_string(path));
    }
    if (fs::exists(path + ".gz")) {
        return sort_lines(read_gz_to_string(path + ".gz"));
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return sort_lines(ss.str());
}

struct Env {
    std::string serial_bin;
    std::string mpi_bin;
    std::string launcher;
    bool ready = false;
    std::string skip_reason;

    Env() {
        serial_bin = find_serial_binary();
        mpi_bin = find_mpi_binary();
        launcher = find_mpi_launcher();
        if (serial_bin.empty()) {
            skip_reason = "dftracer_aggregator binary not found";
            return;
        }
        if (mpi_bin.empty()) {
            skip_reason =
                "dftracer_aggregator_mpi binary not found (set "
                "DFTRACER_AGGREGATOR_MPI_PATH or build with "
                "DFTRACER_UTILS_ENABLE_MPI=ON)";
            return;
        }
        if (launcher.empty()) {
            skip_reason = "no mpiexec/mpirun on PATH";
            return;
        }
        ready = true;
    }
};

// Byte-copy helper -- input files must be byte-identical between the
// serial and MPI runs or their outputs will diverge (TestEnvironment
// seeds randomness internally and does not promise cross-instance
// reproducibility).
bool copy_file(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in.is_open() || !out.is_open()) return false;
    out << in.rdbuf();
    return out.good();
}

// Compare two sorted blobs without blowing up doctest output: CHECK
// gets a bare bool so failure dumps just "false", and we print a
// compact summary (sizes + first-mismatch line) via MESSAGE.
void check_outputs_equal(const std::string& ser, const std::string& mpi) {
    const bool equal = ser == mpi;
    if (!equal) {
        std::size_t diff_pos = 0;
        const std::size_t n = std::min(ser.size(), mpi.size());
        while (diff_pos < n && ser[diff_pos] == mpi[diff_pos]) ++diff_pos;
        auto snippet = [&](const std::string& s) -> std::string {
            // Show up to 120 chars around the first differing byte.
            if (s.empty()) return "<empty>";
            std::size_t start = diff_pos > 60 ? diff_pos - 60 : 0;
            std::size_t len = std::min<std::size_t>(120, s.size() - start);
            return s.substr(start, len);
        };
        MESSAGE("serial bytes=" << ser.size() << " mpi bytes=" << mpi.size()
                                << " first_diff_offset=" << diff_pos);
        MESSAGE("serial near diff: " << snippet(ser));
        MESSAGE("mpi    near diff: " << snippet(mpi));
    }
    CHECK(equal);
}

// Drive one parity test: generate fixtures, clone into sibling dirs,
// run serial vs MPI, return both sorted outputs. Empty pair on setup
// failure.
std::pair<std::string, std::string> run_and_compare(
    const Env& e, int mpi_ranks, int num_events, int num_files,
    bool use_shared_staging = false) {
    dft_utils_test::TestEnvironment src_env(100);
    if (!src_env.is_valid()) return {};
    std::vector<std::string> src_files;
    for (int i = 0; i < num_files; ++i) {
        auto f = create_pfw_gz(src_env, num_events, i);
        if (f.empty()) return {};
        src_files.push_back(f);
    }

    std::string ser_in = src_env.get_dir() + "/_ser_in";
    std::string mpi_in = src_env.get_dir() + "/_mpi_in";
    fs::create_directories(ser_in);
    fs::create_directories(mpi_in);
    for (const auto& f : src_files) {
        std::string name = fs::path(f).filename().string();
        if (!copy_file(f, ser_in + "/" + name)) return {};
        if (!copy_file(f, mpi_in + "/" + name)) return {};
    }

    std::string ser_out = src_env.get_dir() + "/ser.json";
    std::string ser_idx = src_env.get_dir() + "/ser_idx";
    int rser = run_process(e.serial_bin, {"-d", ser_in, "--index-dir", ser_idx,
                                          "-o", ser_out, "--force"});
    if (rser != 0) return {};

    std::string mpi_out = src_env.get_dir() + "/mpi.json.gz";
    std::string mpi_idx = src_env.get_dir() + "/mpi_idx";
    std::string mpi_stg = src_env.get_dir() + "/mpi_stg";
    std::vector<std::string> mpi_args = {
        "-d",    mpi_in, "--index-dir", mpi_idx,  "--staging-dir",
        mpi_stg, "-o",   mpi_out,       "--force"};
    if (use_shared_staging) {
        // Force a distinct shared dir so Artifacts::move_to actually runs
        // (proves aggregation_sst / system_metrics_sst survive the move).
        mpi_args.push_back("--shared-staging");
        mpi_args.push_back(src_env.get_dir() + "/mpi_shared_stg");
    }
    int rmpi = run_mpi(e.launcher, mpi_ranks, e.mpi_bin, mpi_args);
    if (rmpi != 0) return {};

    return {read_output_sorted(ser_out), read_output_sorted(mpi_out)};
}

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_SUITE("DFTracerAggregatorMpi") {
    TEST_CASE("binary exists") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        CHECK(!e.mpi_bin.empty());
        CHECK(!e.launcher.empty());
    }

    TEST_CASE("basic aggregation (n=1)") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 100, 0).empty());

        std::string out = env.get_dir() + "/mpi_basic.json.gz";
        std::string idx = env.get_dir() + "/mpi_basic_idx";
        std::string stg = env.get_dir() + "/mpi_basic_stg";
        int rc = run_mpi(e.launcher, 1, e.mpi_bin,
                         {"-d", env.get_dir(), "--index-dir", idx,
                          "--staging-dir", stg, "-o", out, "--force"});
        CHECK(rc == 0);
        CHECK(fs::exists(out));
    }

    // Serial vs MPI (n=1): bit-for-bit identical. No cross-rank splitting
    // exercised; this is the canonical correctness guarantee for the
    // MPI binary's single-rank mode.
    TEST_CASE("serial parity (n=1)") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        auto [ser, mpi] = run_and_compare(e, /*mpi_ranks=*/1,
                                          /*num_events=*/200,
                                          /*num_files=*/1);
        REQUIRE(!ser.empty());
        REQUIRE(!mpi.empty());
        check_outputs_equal(ser, mpi);
    }

    // Serial vs MPI (n=4): bit-for-bit identical, including cross-rank
    // splitting of a multi-member .pfw.gz. The power-sum MetricStats
    // representation makes the merge order-independent.
    TEST_CASE("serial parity (n=4, cross-rank splitting)") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        auto [ser, mpi] = run_and_compare(e, /*mpi_ranks=*/4,
                                          /*num_events=*/5000,
                                          /*num_files=*/1);
        REQUIRE(!ser.empty());
        REQUIRE(!mpi.empty());
        check_outputs_equal(ser, mpi);
    }

    // Multi-file parity: events spread across several input files with
    // per-file LPT (no cross-rank splitting needed). Serial and MPI
    // should still produce byte-identical output.
    TEST_CASE("serial parity (n=2, multiple files)") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        auto [ser, mpi] = run_and_compare(e, /*mpi_ranks=*/2,
                                          /*num_events=*/500,
                                          /*num_files=*/3);
        REQUIRE(!ser.empty());
        REQUIRE(!mpi.empty());
        check_outputs_equal(ser, mpi);
    }

    // Shared-staging parity: forces the node-local -> shared-FS relocation
    // path via Artifacts::move_to. Regression guard for the bug where
    // aggregation_sst / system_metrics_sst were not listed in move_to and
    // silently dropped during the move, leaving only root_process records
    // in the final output.
    TEST_CASE("serial parity (n=4, shared-staging move path)") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        auto [ser, mpi] = run_and_compare(e, /*mpi_ranks=*/4,
                                          /*num_events=*/5000,
                                          /*num_files=*/1,
                                          /*use_shared_staging=*/true);
        REQUIRE(!ser.empty());
        REQUIRE(!mpi.empty());
        check_outputs_equal(ser, mpi);
    }
}
