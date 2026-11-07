// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H
#define DFTRACER_UTILS_TESTS_TESTING_UTILITIES_H

#ifdef __cplusplus
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <type_traits>

extern "C" {
#endif

// `n`, or under Valgrind `n / d` but at least `floor`.
#ifdef __cplusplus
#ifdef DFTRACER_UTILS_VALGRIND_MODE
#define DFTRACER_UTILS_VALGRIND_SCALE_MIN(n, d, floor)             \
    ((size_t)(n) / (size_t)(d) < (size_t)(floor) ? (size_t)(floor) \
                                                 : (size_t)(n) / (size_t)(d))
#else
#define DFTRACER_UTILS_VALGRIND_SCALE_MIN(n, d, floor) ((size_t)(n))
#endif
#else
#define DFTRACER_UTILS_VALGRIND_SCALE_MIN(n, d, floor)  \
    (getenv("DFTRACER_UTILS_VALGRIND")                  \
         ? ((size_t)(n) / (size_t)(d) < (size_t)(floor) \
                ? (size_t)(floor)                       \
                : (size_t)(n) / (size_t)(d))            \
         : (size_t)(n))
#endif
#define DFTRACER_UTILS_VALGRIND_SCALE(n, d) \
    DFTRACER_UTILS_VALGRIND_SCALE_MIN(n, d, 10)

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
typedef enum { TestFormatGzip = 0 } test_format_t;

/**
 * Create a test gzip file and return the path
 * Returns allocated string - caller must free
 */
char* test_environment_create_test_gzip_file(test_environment_handle_t env);

/**
 * Create a dftracer trace (.gz) of `event_count` events and return the path
 * Returns allocated string - caller must free
 */
char* test_environment_create_dft_gzip_file(test_environment_handle_t env,
                                            int event_count);

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

enum class Format { Gzip = 0 };

inline std::size_t valgrind_scale(std::size_t n,
                                  [[maybe_unused]] std::size_t divisor = 10) {
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    return std::max(std::size_t(10), n / divisor);
#else
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

/// Removes `path` when this process exits normally, so a test that never
/// cleans up leaves nothing in the temp directory. A forked child that calls
/// exit() leaves its parent's paths alone.
void remember_test_path(const dftracer::utils::dstd::Path& path);

inline dftracer::utils::dstd::Path make_unique_test_path(
    const dftracer::utils::dstd::String& name) {
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
    const auto tid =
        dftracer::utils::dstd::Hash<dftracer::utils::dstd::Thread::Id>{}(
            dftracer::utils::dstd::Thread::current_id());
    const auto unique_id = dftracer::utils::dstd::to_string(pid) + "_" +
                           dftracer::utils::dstd::to_string(now) + "_" +
                           dftracer::utils::dstd::to_string(tid) + "_" +
                           dftracer::utils::dstd::to_string(
                               counter.fetch_add(1, std::memory_order_relaxed));
    dftracer::utils::dstd::Path path =
        dftracer::utils::dstd::fs::temp_directory() / (name + "_" + unique_id);
    remember_test_path(path);
    return path;
}

/// Write `content` as a gzip trace at `path` and return that path. Traces
/// must be gzip, so fixtures that used to be written as plain .pfw go
/// through this.
dftracer::utils::dstd::String write_gz_trace(
    const dftracer::utils::dstd::String& path,
    const dftracer::utils::dstd::String& content);

/// Like write_gz_trace, but in stored (uncompressed) deflate blocks written
/// here, so the file has the same bytes whichever zlib the process loads. For
/// tests that compare an index with recorded digests.
dftracer::utils::dstd::String write_stored_gz_trace(
    const dftracer::utils::dstd::String& path,
    const dftracer::utils::dstd::String& content);

bool compress_file_to_gzip(const dftracer::utils::dstd::String& input_file,
                           const dftracer::utils::dstd::String& output_file);

/// Like compress_file_to_gzip but emits a multi-member gzip: the input is
/// split into `member_bytes`-sized uncompressed chunks, each written as an
/// independent gzip stream and concatenated. The reader seeks random access
/// by member, so multi-member output keeps a read from re-inflating the whole
/// file to reach a mid-file offset.
bool compress_file_to_gzip_multimember(
    const dftracer::utils::dstd::String& input_file,
    const dftracer::utils::dstd::String& output_file, std::size_t member_bytes);

/// Build the index for a single gzip trace via the batch pipeline, blocking.
/// Returns true if indexed or already up to date. Empty `index_dir` writes
/// next to the trace; a 0 checkpoint size means the default.
bool build_index(const dftracer::utils::dstd::String& gz,
                 const dftracer::utils::dstd::String& index_dir = "",
                 std::size_t checkpoint_size = 0);

class TestEnvironment {
   public:
    TestEnvironment() : TestEnvironment(100, Format::Gzip) {}
    TestEnvironment(std::size_t lines) : TestEnvironment(lines, Format::Gzip) {}
    TestEnvironment(std::size_t lines, Format format);
    TestEnvironment(const TestEnvironment&) = delete;
    TestEnvironment& operator=(const TestEnvironment&) = delete;
    ~TestEnvironment();

    const dftracer::utils::dstd::String& dir() const;
    bool is_valid() const;
    dftracer::utils::dstd::String
    create_test_file();  // Format-aware file creation
    dftracer::utils::dstd::String create_test_gzip_file();
    dftracer::utils::dstd::String get_index_path(
        const dftracer::utils::dstd::String& gz_file);
    Format format() const { return format_; }

    // DFTracer-specific test file creation
    dftracer::utils::dstd::String create_dft_test_file(int event_count = 100);
    dftracer::utils::dstd::String create_dft_test_gzip_file(
        int event_count = 100);

    // Multi-run trace: `run_count` app spans (one pid each), separated by
    // `gap_us` idle time. Returns the .pfw.gz path.
    dftracer::utils::dstd::String create_dft_multirun_gzip_file(
        int run_count, std::uint64_t run_us = 10000,
        std::uint64_t gap_us = 5000);

   private:
    std::size_t line_count_;
    dftracer::utils::dstd::String test_dir_;
    Format format_;

    dftracer::utils::dstd::String create_test_gzip_file_impl();
};

// -- Running a built binary from a test --

// Run `binary` with `args` (no shell). Returns the exit code, or -1 on failure.
// Everything between fork and exec must be async-signal-safe, which is why the
// argv is built before the fork. A test process that links a threaded runtime
// cannot use this at all: use posix_spawn there.
inline int run_process(
    const dftracer::utils::dstd::String& binary,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& args) {
    // A fixed array, so the forked child (which exits with _exit and never
    // unwinds) holds no heap block of its own for a leak checker to count.
    constexpr std::size_t MAX_ARGS = 64;
    if (args.size() + 2 > MAX_ARGS) return -1;
    const char* argv[MAX_ARGS];
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
inline dftracer::utils::dstd::String find_binary_by_name(
    const char* env_name, const dftracer::utils::dstd::String& name) {
    const char* env_path = std::getenv(env_name);
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;
    for (const dftracer::utils::dstd::String prefix :
         {"./", "../", "../../", "../bin/", "../../bin/"}) {
        const dftracer::utils::dstd::String path = prefix + name;
        if (::access(path.c_str(), X_OK) == 0) return path;
    }
    return "";
}

/// Point LD_LIBRARY_PATH at the build tree's lib dirs so a binary exec'd from
/// a test finds the shared libraries it was linked against. `binary` is the
/// path the build root is derived from.
void set_test_library_path(const dftracer::utils::dstd::String& binary);

/// Run `binary` with `args`, capturing what it writes to stdout (and to stderr
/// too when `capture_stderr`). Returns the captured text; writes the exit code
/// to `exit_code` when it is not NULL, or -1 if the process did not exit
/// normally. The output is read to EOF before the wait, so a child that writes
/// more than a pipe buffer does not deadlock.
dftracer::utils::dstd::String run_process_capture(
    const dftracer::utils::dstd::String& binary,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>& args,
    bool capture_stderr = false, int* exit_code = nullptr);

/// The first non-empty line of a gzip file, with any trailing CR/LF removed,
/// or "" if the file cannot be read.
dftracer::utils::dstd::String gz_first_line(
    const dftracer::utils::dstd::String& gz_path);

/// Prints the error and aborts when a setup call failed. For code that runs
/// outside a doctest assertion, such as a constructor of a shared fixture.
template <class T>
inline void require_ok(const dftracer::utils::dstd::Expected<
                           T, dftracer::utils::dstd::Error>& result,
                       const char* what) {
    if (result.has_value()) return;
    const auto& error = result.error();
    std::fprintf(stderr, "%s failed: %.*s %.*s\n", what,
                 static_cast<int>(error.name().size()), error.name().data(),
                 static_cast<int>(error.message().size()),
                 error.message().data());
    std::abort();
}

/// Appends `value` to `out` the way operator<< of a std::ostream prints it.
template <class T>
void append_text(dftracer::utils::dstd::String& out, const T& value) {
    if constexpr (std::is_same_v<T, char>) {
        out.push_back(value);
    } else if constexpr (std::is_same_v<T, bool>) {
        out.push_back(value ? '1' : '0');
    } else if constexpr (std::is_integral_v<T>) {
        out += dftracer::utils::dstd::to_string(value);
    } else if constexpr (std::is_floating_point_v<T>) {
        char text[32];
        const int n =
            std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
        out.append(text, static_cast<std::size_t>(n));
    } else {
        out.append(dftracer::utils::dstd::StringView(value));
    }
}

/// Stands in for std::ostringstream. What is streamed in is kept in str().
class TextOut {
   public:
    template <class T>
    TextOut& operator<<(const T& value) {
        append_text(buffer_, value);
        return *this;
    }

    const dftracer::utils::dstd::String& str() const { return buffer_; }

   private:
    dftracer::utils::dstd::String buffer_;
};

/// Stands in for std::ofstream. The file is created and emptied on
/// construction, what is streamed in is kept in memory, and close() or the
/// destructor writes it with dstd::fs::write_file.
class FileOut {
   public:
    explicit FileOut(dftracer::utils::dstd::PathView path)
        : path_(path.str()),
          open_(dftracer::utils::dstd::fs::write_file(path, "").has_value()) {}
    FileOut(const FileOut&) = delete;
    FileOut& operator=(const FileOut&) = delete;
    ~FileOut() { close(); }

    template <class T>
    FileOut& operator<<(const T& value) {
        append_text(buffer_, value);
        return *this;
    }

    FileOut& write(const char* data, std::size_t size) {
        buffer_.append(data, size);
        return *this;
    }

    bool is_open() const { return open_; }
    bool good() const { return open_; }
    explicit operator bool() const { return open_; }

    /// Writes the buffer to the file. False when the file was not opened or
    /// the write failed.
    bool close() {
        if (!open_) return false;
        open_ = false;
        return dftracer::utils::dstd::fs::write_file(path_, buffer_)
            .has_value();
    }

   private:
    dftracer::utils::dstd::String path_;
    dftracer::utils::dstd::String buffer_;
    bool open_ = false;
};

/// The number at the start of `text` after any white space, as std::stol and
/// its siblings read it. Ends the test binary when there is none.
template <class T>
T to_number(dftracer::utils::dstd::StringView text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    if (begin < text.size() && text[begin] == '+') ++begin;
    std::size_t end = begin;
    if (end < text.size() && text[end] == '-') ++end;
    while (end < text.size()) {
        const char c = text[end];
        const bool digit = c >= '0' && c <= '9';
        const bool real = std::is_floating_point_v<T> &&
                          (c == '.' || c == 'e' || c == 'E' ||
                           ((c == '-' || c == '+') &&
                            (text[end - 1] == 'e' || text[end - 1] == 'E')));
        if (!digit && !real) break;
        ++end;
    }
    auto value =
        dftracer::utils::dstd::from_chars<T>(text.substr(begin, end - begin));
    if (!value.has_value()) {
        std::fprintf(stderr, "not a number: %.*s\n",
                     static_cast<int>(text.size()), text.data());
        std::abort();
    }
    return *value;
}

/// Contents of the file at `path`, empty when it cannot be read.
inline dftracer::utils::dstd::String read_text(
    dftracer::utils::dstd::PathView path) {
    auto text = dftracer::utils::dstd::fs::read_file(path);
    return text.has_value() ? std::move(*text)
                            : dftracer::utils::dstd::String();
}

/// Stands in for a std::istringstream read with std::getline. A final newline
/// does not start another line.
class LineStream {
   public:
    explicit LineStream(dftracer::utils::dstd::StringView text) : text_(text) {}

    bool next(dftracer::utils::dstd::String& line) {
        if (pos_ >= text_.size()) return false;
        std::size_t end = text_.find('\n', pos_);
        if (end == dftracer::utils::dstd::String::npos) end = text_.size();
        line.assign(text_, pos_, end - pos_);
        pos_ = end + 1;
        return true;
    }

   private:
    dftracer::utils::dstd::String text_;
    std::size_t pos_ = 0;
};

/// A scratch directory that exists for the lifetime of the object. The path is
/// unique per process (make_unique_test_path), and the destructor removes the
/// tree, so a test case that returns early or throws still cleans up.
class ScopedTestDir {
   public:
    explicit ScopedTestDir(const dftracer::utils::dstd::String& tag)
        : path_(make_unique_test_path(tag)) {
        dftu_utils_test::require_ok(
            dftracer::utils::dstd::fs::create_directories(path_.view()),
            "create_directories");
    }
    ScopedTestDir(const ScopedTestDir&) = delete;
    ScopedTestDir& operator=(const ScopedTestDir&) = delete;
    ~ScopedTestDir() {
        (void)dftracer::utils::dstd::fs::remove_all(path_.view()).has_value();
    }

    const dftracer::utils::dstd::Path& path() const { return path_; }
    dftracer::utils::dstd::String str() const {
        return dftracer::utils::dstd::String(path_.str());
    }
    /// `name` resolved inside the directory.
    dftracer::utils::dstd::String file(
        dftracer::utils::dstd::StringView name) const {
        return dftracer::utils::dstd::String((path_ / name).str());
    }

    /// Write `content` at `relative_path`, creating any parent directories.
    void create_file(
        const dftracer::utils::dstd::String& relative_path,
        const dftracer::utils::dstd::String& content = "test") const {
        const dftracer::utils::dstd::Path p = path_ / relative_path;
        dftu_utils_test::require_ok(
            dftracer::utils::dstd::fs::create_directories(p.view().parent()),
            "create_directories");
        FileOut ofs(p.view());
        ofs << content;
    }

    void create_dir(const dftracer::utils::dstd::String& relative_path) const {
        dftu_utils_test::require_ok(
            dftracer::utils::dstd::fs::create_directories(
                (path_ / relative_path).view()),
            "create_directories");
    }

   private:
    dftracer::utils::dstd::Path path_;
};

/// One entry of a listing, with the kind and size of the entry itself.
struct ListedEntry {
    dftracer::utils::dstd::Path path;
    dftracer::utils::dstd::fs::FileKind kind =
        dftracer::utils::dstd::fs::FileKind::Unknown;
    std::uint64_t size = 0;
};

/// The direct children of `dir` in directory order, each with its own stat.
dftracer::utils::dstd::Vector<ListedEntry> list_dir(
    dftracer::utils::dstd::PathView dir);
/// Every descendant of `dir` below it, in walk order, skipping symlinks.
dftracer::utils::dstd::Vector<ListedEntry> walk_dir(
    dftracer::utils::dstd::PathView dir);

#ifdef DFTRACER_UTILS_MPI_ENABLED
// -- Shared helpers for the MPI binary integration tests --

// Locate an MPI launcher: $MPIEXEC_EXECUTABLE if set, else mpiexec/mpirun on
// PATH. Returns "" if none is found.
inline dftracer::utils::dstd::String find_mpi_launcher() {
    const char* env_path = std::getenv("MPIEXEC_EXECUTABLE");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) return env_path;
    for (const auto& name : {"mpiexec", "mpirun"}) {
        dftracer::utils::dstd::String cmd =
            dftracer::utils::dstd::String("command -v ") + name +
            " 2>/dev/null";
        FILE* p = ::popen(cmd.c_str(), "r");
        if (!p) continue;
        char buf[4096];
        dftracer::utils::dstd::String out;
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
inline int run_mpi(
    const dftracer::utils::dstd::String& launcher, int np,
    const dftracer::utils::dstd::String& binary,
    const dftracer::utils::dstd::Vector<dftracer::utils::dstd::String>&
        binary_args) {
    ::setenv("OMPI_ALLOW_RUN_AS_ROOT", "1", 1);
    ::setenv("OMPI_ALLOW_RUN_AS_ROOT_CONFIRM", "1", 1);
    dftracer::utils::dstd::Vector<dftracer::utils::dstd::String> args = {
        "-n", dftracer::utils::dstd::to_string(np), binary};
    for (const auto& a : binary_args) args.push_back(a);
    return run_process(launcher, args);
}

// Shared setup for the *_mpi binary integration tests: locates the serial and
// MPI binaries plus an MPI launcher, and records a skip reason if any is
// missing. Tests derive a small Env from this with their specific names.
struct MpiTestEnv {
    dftracer::utils::dstd::String serial_bin;
    dftracer::utils::dstd::String mpi_bin;
    dftracer::utils::dstd::String launcher;
    bool ready = false;
    dftracer::utils::dstd::String skip_reason;

    MpiTestEnv(const dftracer::utils::dstd::String& serial_name,
               const char* serial_env,
               const dftracer::utils::dstd::String& mpi_name,
               const char* mpi_env) {
        serial_bin = find_binary_by_name(serial_env, serial_name);
        mpi_bin = find_binary_by_name(mpi_env, mpi_name);
        launcher = find_mpi_launcher();
        if (serial_bin.empty()) {
            skip_reason = serial_name + " binary not found";
            return;
        }
        if (mpi_bin.empty()) {
            skip_reason = mpi_name + " binary not found (set " +
                          dftracer::utils::dstd::String(mpi_env) +
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
