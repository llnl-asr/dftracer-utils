#ifndef DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H
#define DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H

#ifdef __cplusplus
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef DFTRACER_UTILS_MPI_ENABLED
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#endif
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
typedef enum { TEST_FORMAT_GZIP = 0, TEST_FORMAT_TAR_GZIP = 1 } test_format_t;

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
 * Create a tar.gz archive with multiple files
 * Returns allocated string - caller must free
 */
char* test_environment_create_test_tar_gzip_file(test_environment_handle_t env);

/**
 * Create a tar archive and then compress it to gzip
 * Returns 1 on success, 0 on failure
 */
int create_tar_archive_and_compress(const char** file_paths, size_t num_files,
                                    const char* output_file);

/**
 * Extract a specific file from tar.gz archive
 * Returns 1 on success, 0 on failure
 */
int extract_file_from_tar_gz(const char* tar_gz_path, const char* file_path,
                             const char* output_path);

/**
 * Get list of files in tar archive
 * Returns allocated array of strings - caller must free each string and the
 * array
 */
char** get_tar_file_list(const char* tar_path, size_t* num_files);

/**
 * Free file list returned by get_tar_file_list
 */
void free_tar_file_list(char** file_list, size_t num_files);

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

namespace dft_utils_test {

enum class Format { GZIP = 0, TAR_GZIP = 1 };

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
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    const auto unique_id =
        std::to_string(now) + "_" + std::to_string(tid) + "_" +
        std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return fs::temp_directory_path() / (name + "_" + unique_id);
}

struct TarFileInfo {
    std::string filename;
    std::string content;
    std::size_t num_lines;
};

bool compress_file_to_gzip(const std::string& input_file,
                           const std::string& output_file);

bool create_tar_archive(const std::vector<TarFileInfo>& files,
                        const std::string& output_path);
bool create_tar_gz_archive(const std::vector<TarFileInfo>& files,
                           const std::string& output_path);
std::vector<std::string> list_tar_contents(const std::string& tar_path);
bool extract_from_tar_gz(const std::string& tar_gz_path,
                         const std::string& file_path,
                         const std::string& output_path);

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
    std::string create_test_tar_gzip_file();
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
    std::string create_test_tar_gzip_file_impl();
};

#ifdef DFTRACER_UTILS_MPI_ENABLED
// -- Shared helpers for the MPI binary integration tests --

// Run `binary` with `args` (no shell). Returns the exit code, or -1 on failure.
inline int run_process(const std::string& binary,
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
}  // namespace dft_utils_test
#endif

#endif  // DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H
