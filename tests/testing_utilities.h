#ifndef DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H
#define DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H

#ifdef __cplusplus
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#endif

#ifdef __cplusplus
#ifdef DFTRACER_UTILS_VALGRIND_MODE
#define DFTRACER_UTILS_VALGRIND_SCALE(n, d) \
    ((size_t)(n) / (size_t)(d) < 10 ? (size_t)10 : (size_t)(n) / (size_t)(d))
#else
#define DFTRACER_UTILS_VALGRIND_SCALE(n, d) ((size_t)(n))
#endif
#else
#define DFTRACER_UTILS_VALGRIND_SCALE(n, d)                             \
    (getenv("DFTRACER_UTILS_VALGRIND")                                  \
         ? ((size_t)(n) / (size_t)(d) < 10 ? (size_t)10                 \
                                           : (size_t)(n) / (size_t)(d)) \
         : (size_t)(n))
#endif

// C API for testing utilities
typedef struct test_environment* test_environment_handle_t;

/**
 * Create a test environment with default number of lines (100)
 */
test_environment_handle_t test_environment_create(void);

/**
 * Create a test environment with specified number of lines
 */
test_environment_handle_t test_environment_create_with_lines(size_t lines);

/**
 * Destroy a test environment and clean up resources
 */
void test_environment_destroy(test_environment_handle_t env);

/**
 * Check if test environment is valid
 */
int test_environment_is_valid(test_environment_handle_t env);

/**
 * Get the test directory path
 * Returns a pointer to internal string - do not free
 */
const char* test_environment_get_dir(test_environment_handle_t env);

/**
 * Archive formats supported by the test environment
 */
typedef enum { TEST_FORMAT_GZIP = 0 } test_format_t;

/**
 * Create a test gzip file and return the path
 * Returns allocated string - caller must free
 */
char* test_environment_create_test_gzip_file(test_environment_handle_t env);

/**
 * Create a test file with specified format and return the path
 * Returns allocated string - caller must free
 */
char* test_environment_create_test_file_with_format(
    test_environment_handle_t env, test_format_t format);

/**
 * Get the `.dftindex` path for a given gzip file
 * Returns allocated string - caller must free
 */
char* test_environment_get_index_path(test_environment_handle_t env,
                                      const char* gz_file);

/**
 * Compress a file to gzip format
 * Returns 1 on success, 0 on failure
 */
int compress_file_to_gzip_c(const char* input_file, const char* output_file);

size_t mb_to_b(double mb);

/**
 * Generate a unique path for temporary tests.
 * Returns allocated string - caller must free
 */
char* test_make_unique_test_path(const char* name);

#ifdef __cplusplus
}

namespace dftu_utils_test {

enum class Format { GZIP = 0 };

inline std::size_t valgrind_scale(std::size_t n, std::size_t divisor = 10) {
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    return std::max(std::size_t(10), n / divisor);
#else
    (void)divisor;
    return n;
#endif
}

inline int valgrind_threads(int n) {
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    return std::min(n, 2);
#else
    return n;
#endif
}

inline fs::path make_unique_test_path(const std::string& name) {
    static std::atomic<unsigned long long> counter{0};
    // The pid is what keeps concurrently running test processes apart. None
    // of the other components do: steady_clock reads the same on processes
    // started in the same tick, the main thread's id hashes identically in
    // every process, and the counter restarts at 0. Two processes agreeing on
    // a path means one TestEnvironment destructor removes the other's fixture
    // mid-test, which surfaces as a file that was just written having
    // vanished.
    const auto pid = static_cast<unsigned long long>(::getpid());
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto unique_id =
        std::to_string(pid) + "_" + std::to_string(now) + "_" +
        std::to_string(tid) + "_" +
        std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return fs::temp_directory_path() / (name + "_" + unique_id);
}

/// Write `content` as a gzip trace at `path` and return that path. Traces
/// must be gzip, so fixtures that used to be written as plain .pfw go
/// through this.
std::string write_gz_trace(const std::string& path, const std::string& content);

bool compress_file_to_gzip(const std::string& input_file,
                           const std::string& output_file);

/// Like compress_file_to_gzip but emits a multi-member gzip: the input is
/// split into `member_bytes`-sized uncompressed chunks, each written as an
/// independent gzip stream and concatenated. The reader seeks random access
/// by member, so multi-member output keeps a read from re-inflating the whole
/// file to reach a mid-file offset.
bool compress_file_to_gzip_multimember(const std::string& input_file,
                                       const std::string& output_file,
                                       std::size_t member_bytes);

/// Build the index for a single gzip trace via the batch pipeline, blocking.
/// Returns true if indexed or already up to date. Empty `index_dir` writes
/// next to the trace; a 0 argument means "default" for the sizing knobs.
bool build_index(const std::string& gz, const std::string& index_dir = "",
                 std::size_t sub_chunk_events = 0,
                 std::size_t checkpoint_size = 0);

class TestEnvironment {
   public:
    TestEnvironment() : TestEnvironment(100, Format::GZIP) {}
    TestEnvironment(std::size_t lines) : TestEnvironment(lines, Format::GZIP) {}
    TestEnvironment(std::size_t lines, Format format);
    TestEnvironment(const TestEnvironment&) = delete;
    TestEnvironment& operator=(const TestEnvironment&) = delete;
    ~TestEnvironment();

    const std::string& get_dir() const;
    bool is_valid() const;
    std::string create_test_file();  // Format-aware file creation
    std::string create_test_gzip_file();
    std::string get_index_path(const std::string& gz_file);
    Format get_format() const { return format_; }

    // DFTracer-specific test file creation
    std::string create_dft_test_file(int num_events = 100);
    std::string create_dft_test_gzip_file(int num_events = 100);

    // Multi-run trace: `num_runs` app spans (one pid each), separated by
    // `gap_us` idle time. Returns the .pfw.gz path.
    std::string create_dft_multirun_gzip_file(int num_runs,
                                              std::uint64_t run_us = 10000,
                                              std::uint64_t gap_us = 5000);

   private:
    std::size_t num_lines;
    std::string test_dir;
    Format format_;

    std::string create_test_gzip_file_impl();
};

// -- Running a built binary from a test --

// Run `binary` with `args` (no shell). Returns the exit code, or -1 on failure.
// Everything between fork and exec must be async-signal-safe, which is why the
// argv is built before the fork. A test process that links a threaded runtime
// cannot use this at all: use posix_spawn there.
inline int run_process(const std::string& binary,
                       const std::vector<std::string>& args) {
    // A fixed array, so the forked child (which exits with _exit and never
    // unwinds) holds no heap block of its own for a leak checker to count.
    constexpr std::size_t kMaxArgs = 64;
    if (args.size() + 2 > kMaxArgs) return -1;
    const char* argv[kMaxArgs];
    std::size_t n = 0;
    argv[n++] = binary.c_str();
    for (const auto& a : args) argv[n++] = a.c_str();
    argv[n] = nullptr;
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::execv(binary.c_str(), const_cast<char* const*>(argv));
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Locate a built binary by name: prefer $env_name (set by CMake), else search
// the common build-output locations relative to the test's working directory.
inline std::string find_binary_by_name(const char* env_name,
                                       const std::string& name) {
    const char* env_path = std::getenv(env_name);
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;
    for (const std::string prefix :
         {"./", "../", "../../", "../bin/", "../../bin/"}) {
        const std::string path = prefix + name;
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

/// Point LD_LIBRARY_PATH at the build tree's lib dirs so a binary exec'd from
/// a test finds the shared libraries it was linked against. `binary` is the
/// path the build root is derived from.
void set_test_library_path(const std::string& binary);

/// Run `binary` with `args`, capturing what it writes to stdout (and to stderr
/// too when `capture_stderr`). Returns the captured text; writes the exit code
/// to `exit_code` when it is not NULL, or -1 if the process did not exit
/// normally. The output is read to EOF before the wait, so a child that writes
/// more than a pipe buffer does not deadlock.
std::string run_process_capture(const std::string& binary,
                                const std::vector<std::string>& args,
                                bool capture_stderr = false,
                                int* exit_code = nullptr);

/// The first non-empty line of a gzip file, with any trailing CR/LF removed,
/// or "" if the file cannot be read.
std::string gz_first_line(const std::string& gz_path);

/// A scratch directory that exists for the lifetime of the object. The path is
/// unique per process (make_unique_test_path), and the destructor removes the
/// tree, so a test case that returns early or throws still cleans up.
class ScopedTestDir {
   public:
    explicit ScopedTestDir(const std::string& tag) {
        path_ = make_unique_test_path(tag);
        fs::create_directories(path_);
    }
    ScopedTestDir(const ScopedTestDir&) = delete;
    ScopedTestDir& operator=(const ScopedTestDir&) = delete;
    ~ScopedTestDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }
    std::string str() const { return path_.string(); }
    /// `name` resolved inside the directory.
    std::string file(const std::string& name) const {
        return (path_ / name).string();
    }

    /// Write `content` at `relative_path`, creating any parent directories.
    void create_file(const std::string& relative_path,
                     const std::string& content = "test") const {
        const fs::path p = path_ / relative_path;
        fs::create_directories(p.parent_path());
        std::ofstream ofs(p);
        ofs << content;
    }

    void create_dir(const std::string& relative_path) const {
        fs::create_directories(path_ / relative_path);
    }

   private:
    fs::path path_;
};

#ifdef DFTRACER_UTILS_MPI_ENABLED
// -- Shared helpers for the MPI binary integration tests --

// Locate an MPI launcher: $MPIEXEC_EXECUTABLE if set, else mpiexec/mpirun on
// PATH. Returns "" if none is found.
inline std::string find_mpi_launcher() {
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

// Launch `binary` under `launcher` with `np` ranks. Returns the exit code.
// OpenMPI refuses to run as root (common in CI containers); permit it via env
// vars rather than --allow-run-as-root, which MPICH does not recognize. MPICH
// ignores unknown env vars, so this is portable across implementations.
inline int run_mpi(const std::string& launcher, int np,
                   const std::string& binary,
                   const std::vector<std::string>& binary_args) {
    ::setenv("OMPI_ALLOW_RUN_AS_ROOT", "1", 1);
    ::setenv("OMPI_ALLOW_RUN_AS_ROOT_CONFIRM", "1", 1);
    std::vector<std::string> args = {"-n", std::to_string(np), binary};
    for (const auto& a : binary_args) args.push_back(a);
    return run_process(launcher, args);
}

// Shared setup for the *_mpi binary integration tests: locates the serial and
// MPI binaries plus an MPI launcher, and records a skip reason if any is
// missing. Tests derive a small Env from this with their specific names.
struct MpiTestEnv {
    std::string serial_bin;
    std::string mpi_bin;
    std::string launcher;
    bool ready = false;
    std::string skip_reason;

    MpiTestEnv(const std::string& serial_name, const char* serial_env,
               const std::string& mpi_name, const char* mpi_env) {
        serial_bin = find_binary_by_name(serial_env, serial_name);
        mpi_bin = find_binary_by_name(mpi_env, mpi_name);
        launcher = find_mpi_launcher();
        if (serial_bin.empty()) {
            skip_reason = serial_name + " binary not found";
            return;
        }
        if (mpi_bin.empty()) {
            skip_reason = mpi_name + " binary not found (set " +
                          std::string(mpi_env) +
                          " or build with DFTRACER_UTILS_ENABLE_MPI=ON)";
            return;
        }
        if (launcher.empty()) {
            skip_reason = "no mpiexec/mpirun on PATH";
            return;
        }
        ready = true;
    }
};
#endif  // DFTRACER_UTILS_MPI_ENABLED
}  // namespace dftu_utils_test
#endif

#endif  // DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H
