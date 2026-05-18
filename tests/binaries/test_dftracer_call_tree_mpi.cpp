#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string find_binary(const char* env_name,
                        const std::vector<std::string>& candidates) {
    const char* env_path = std::getenv(env_name);
    if (env_path && ::access(env_path, X_OK) == 0) return env_path;
    for (const auto& p : candidates) {
        if (::access(p.c_str(), X_OK) == 0) return p;
    }
    return "";
}

std::string find_serial_binary() {
    return find_binary("DFTRACER_CALL_TREE_PATH",
                       {"./dftracer_call_tree", "../dftracer_call_tree",
                        "../../dftracer_call_tree", "../bin/dftracer_call_tree",
                        "../../bin/dftracer_call_tree"});
}

std::string find_mpi_binary() {
    return find_binary(
        "DFTRACER_CALL_TREE_MPI_PATH",
        {"./dftracer_call_tree_mpi", "../dftracer_call_tree_mpi",
         "../../dftracer_call_tree_mpi", "../bin/dftracer_call_tree_mpi",
         "../../bin/dftracer_call_tree_mpi"});
}

std::string find_launcher() {
    const char* env_path = std::getenv("MPIEXEC_EXECUTABLE");
    if (env_path && ::access(env_path, X_OK) == 0) return env_path;
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
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int run_mpi(const std::string& launcher, int np, const std::string& binary,
            const std::vector<std::string>& binary_args) {
    std::vector<std::string> args = {"--allow-run-as-root", "-n",
                                     std::to_string(np), binary};
    for (const auto& a : binary_args) args.push_back(a);
    return run_process(launcher, args);
}

std::string create_pfw_gz(dft_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";
    std::string path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, path);
    return path;
}

bool copy_file(const std::string& src, const std::string& dst) {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in.is_open() || !out.is_open()) return false;
    out << in.rdbuf();
    return out.good();
}

// Strip the "id":<N>, prefix from a Chrome Tracing event line. Event id
// differs between serial (sequential) and MPI (rank-base + slice stride)
// runs even when the underlying events are identical, so we compare the
// remaining fields. Non-event lines (header brackets) pass through.
std::string strip_event_id(const std::string& line) {
    static const std::string prefix = "{\"id\":";
    if (line.compare(0, prefix.size(), prefix) != 0) return line;
    std::size_t comma = line.find(',', prefix.size());
    if (comma == std::string::npos) return line;
    return std::string("{") + line.substr(comma + 1);
}

std::vector<std::string> read_event_lines_sorted(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream f(path);
    if (!f.is_open()) return lines;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line == "[" || line == "]") continue;
        // Drop trailing comma so equivalent events match.
        if (!line.empty() && line.back() == ',') line.pop_back();
        // Metadata events ({"name":"M",...}) embed wall-clock timestamp and
        // change between independent process invocations. Skip them; we
        // only compare actual tree events.
        static const std::string meta_prefix = "{\"name\":\"M\"";
        if (line.compare(0, meta_prefix.size(), meta_prefix) == 0) continue;
        lines.push_back(strip_event_id(line));
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

struct Env {
    std::string serial_bin, mpi_bin, launcher;
    bool ready = false;
    std::string skip_reason;

    Env() {
        serial_bin = find_serial_binary();
        mpi_bin = find_mpi_binary();
        launcher = find_launcher();
        if (serial_bin.empty()) {
            skip_reason = "dftracer_call_tree binary not found";
            return;
        }
        if (mpi_bin.empty()) {
            skip_reason = "dftracer_call_tree_mpi binary not found";
            return;
        }
        if (launcher.empty()) {
            skip_reason = "no mpiexec/mpirun on PATH";
            return;
        }
        ready = true;
    }
};

std::pair<std::vector<std::string>, std::vector<std::string>> run_and_compare(
    const Env& e, int mpi_ranks, int num_events, int num_files) {
    dft_utils_test::TestEnvironment src(100);
    if (!src.is_valid()) return {};
    std::vector<std::string> srcs;
    for (int i = 0; i < num_files; ++i) {
        auto p = create_pfw_gz(src, num_events, i);
        if (p.empty()) return {};
        srcs.push_back(p);
    }

    // Identical input dirs for serial and MPI runs.
    std::string ser_in = src.get_dir() + "/_ser_in";
    std::string mpi_in = src.get_dir() + "/_mpi_in";
    fs::create_directories(ser_in);
    fs::create_directories(mpi_in);
    for (const auto& f : srcs) {
        auto name = fs::path(f).filename().string();
        if (!copy_file(f, ser_in + "/" + name)) return {};
        if (!copy_file(f, mpi_in + "/" + name)) return {};
    }

    std::string ser_out = src.get_dir() + "/ser.pfw";
    int rs = run_process(e.serial_bin, {ser_in, "-o", ser_out});
    if (rs != 0) return {};

    std::string mpi_out = src.get_dir() + "/mpi.pfw";
    std::string mpi_stg = src.get_dir() + "/mpi_stg";
    int rm = run_mpi(e.launcher, mpi_ranks, e.mpi_bin,
                     {mpi_in, "-o", mpi_out, "--staging-dir", mpi_stg});
    if (rm != 0) return {};

    return {read_event_lines_sorted(ser_out), read_event_lines_sorted(mpi_out)};
}

void check_parity(const std::vector<std::string>& ser,
                  const std::vector<std::string>& mpi) {
    const bool equal = ser == mpi;
    if (!equal) {
        MESSAGE("serial events=" << ser.size() << " mpi events=" << mpi.size());
        std::size_t shown = 0;
        for (std::size_t i = 0;
             i < std::min(ser.size(), mpi.size()) && shown < 3; ++i) {
            if (ser[i] != mpi[i]) {
                MESSAGE("first diff at " << i);
                MESSAGE("  ser: " << ser[i]);
                MESSAGE("  mpi: " << mpi[i]);
                ++shown;
            }
        }
    }
    CHECK(equal);
}

}  // namespace

TEST_SUITE("DFTracerCallTreeMpi") {
    TEST_CASE("binary exists") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        CHECK(!e.mpi_bin.empty());
        CHECK(!e.launcher.empty());
    }

    TEST_CASE("basic MPI run (n=1)") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        dft_utils_test::TestEnvironment env(100);
        REQUIRE(env.is_valid());
        REQUIRE(!create_pfw_gz(env, 100, 0).empty());
        std::string out = env.get_dir() + "/mpi.pfw";
        std::string stg = env.get_dir() + "/stg";
        int rc = run_mpi(e.launcher, 1, e.mpi_bin,
                         {env.get_dir(), "-o", out, "--staging-dir", stg});
        CHECK(rc == 0);
        CHECK(fs::exists(out));
    }

    TEST_CASE("serial parity (n=1)") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        auto [s, m] = run_and_compare(e, 1, 200, 1);
        REQUIRE(!s.empty());
        REQUIRE(!m.empty());
        check_parity(s, m);
    }

    TEST_CASE("serial parity (n=2 multi-file)") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        auto [s, m] = run_and_compare(e, 2, 200, 4);
        REQUIRE(!s.empty());
        REQUIRE(!m.empty());
        check_parity(s, m);
    }

    TEST_CASE("serial parity (n=4 multi-file)") {
        Env e;
        if (!e.ready) {
            MESSAGE("skipping: " << e.skip_reason);
            return;
        }
        auto [s, m] = run_and_compare(e, 4, 300, 8);
        REQUIRE(!s.empty());
        REQUIRE(!m.empty());
        check_parity(s, m);
    }
}
