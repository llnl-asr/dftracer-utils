// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace dftracer::utils;

namespace {

// Test bookkeeping kept out of the memory budget, since it lives until after
// the memory exit check.
struct RememberedPaths {
    std::mutex mutex;
    std::vector<std::string> paths;
    pid_t owner = ::getpid();
};

RememberedPaths& remembered() {
    static RememberedPaths r;
    return r;
}

void remove_remembered_paths() {
    RememberedPaths& r = remembered();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (::getpid() != r.owner) return;
    for (const auto& p : r.paths)
        (void)dstd::fs::remove_all(dstd::PathView(p)).has_value();
    r.paths.clear();
}

}  // namespace

void dftu_utils_test::remember_test_path(const dstd::Path& path) {
    RememberedPaths& r = remembered();
    // Registered after `remembered()` is built, so it runs before that object
    // is destroyed.
    static const bool registered = [] {
        std::atexit(remove_remembered_paths);
        return true;
    }();
    (void)registered;
    std::lock_guard<std::mutex> lock(r.mutex);
    r.paths.emplace_back(path.str());
}

extern "C" {
size_t mb_to_b(double mb) { return static_cast<std::size_t>(mb * 1024 * 1024); }
}  // extern "C"

namespace dftu_utils_test {
bool build_index(const dstd::String& gz, const dstd::String& index_dir,
                 std::size_t checkpoint_size) {
    bool ok = false;
    Runtime rt(4);
    auto task = dftracer::utils::coro::with_scope(
        [&](dftracer::utils::coro::Scope&)
            -> dftracer::utils::coro::CoroTask<void> {
            auto config = dstd::make_shared<
                dftracer::utils::index::build::IndexBuildBatchConfig>();
            config->file_paths = {dstd::String(dstd::StringView(gz))};
            config->index_dir = index_dir;
            if (checkpoint_size > 0) config->checkpoint_size = checkpoint_size;
            auto r =
                co_await dftracer::utils::index::build::BatchBuilder::process(
                    std::move(config));
            ok = (r.indexed + r.skipped) >= 1 && r.failed == 0;
            co_return;
        });
    rt.submit(std::move(task), "test-build-index").wait();
    rt.shutdown();
    return ok;
}

dstd::String write_gz_trace(const dstd::String& path,
                            const dstd::String& content) {
    gzFile f = gzopen(path.c_str(), "wb");
    if (f == nullptr) return "";
    if (!content.empty()) {
        gzwrite(f, content.data(), static_cast<unsigned>(content.size()));
    }
    gzclose(f);
    return path;
}

dstd::String write_stored_gz_trace(const dstd::String& path,
                                   const dstd::String& content) {
    constexpr std::size_t STORED_MAX = 65535;
    dstd::String out = {'\x1f', '\x8b', '\x08', '\x00', '\x00',
                        '\x00', '\x00', '\x00', '\x00', '\xff'};
    const auto put16 = [&out](std::size_t v) {
        out += static_cast<char>(v & 0xFF);
        out += static_cast<char>((v >> 8) & 0xFF);
    };
    const auto put32 = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out += static_cast<char>((v >> (8 * i)) & 0xFF);
    };
    std::size_t at = 0;
    do {
        const std::size_t n = std::min(STORED_MAX, content.size() - at);
        const bool last = at + n == content.size();
        out += static_cast<char>(last ? 1 : 0);
        put16(n);
        put16(~n & 0xFFFF);
        out.append(content, at, n);
        at += n;
    } while (at < content.size());
    put32(dftracer::utils::utilities::fileio::compress::gzip_crc32(
        0, content.data(), content.size()));
    put32(static_cast<std::uint32_t>(content.size()));
    dftu_utils_test::FileOut file(path);
    if (!file.write(out.data(), static_cast<std::streamsize>(out.size())))
        return "";
    return path;
}

bool compress_file_to_gzip(const dstd::String& input_file,
                           const dstd::String& output_file) {
    auto input = dstd::fs::read_file(input_file);
    if (!input.has_value()) {
        return false;
    }

    gzFile gz_output = gzopen(output_file.c_str(), "wb");
    if (!gz_output) {
        return false;
    }

    const std::size_t buffer_size = 8192;
    for (std::size_t at = 0; at < input->size(); at += buffer_size) {
        const unsigned int bytes_read = static_cast<unsigned int>(
            std::min(buffer_size, input->size() - at));
        if (gzwrite(gz_output, input->data() + at, bytes_read) !=
            static_cast<int>(bytes_read)) {
            gzclose(gz_output);
            return false;
        }
    }

    gzclose(gz_output);
    return true;
}

bool compress_file_to_gzip_multimember(const dstd::String& input_file,
                                       const dstd::String& output_file,
                                       std::size_t member_bytes) {
    if (member_bytes == 0) member_bytes = 4 * 1024 * 1024;

    auto text = dstd::fs::read_file(input_file);
    if (!text.has_value()) return false;
    dftu_utils_test::LineStream input(*text);
    dftu_utils_test::FileOut output(output_file);
    if (!output.is_open()) return false;

    utilities::fileio::compress::GzipMemberCompressor comp;
    if (!comp.valid()) return false;

    // Accumulate whole lines so member boundaries land on newlines, matching
    // how the real writer emits line-aligned members.
    auto flush = [&](dstd::String& buf) -> bool {
        if (buf.empty()) return true;
        auto member = comp.compress_member(buf.data(), buf.size());
        if (!member.has_value()) return false;
        output.write(reinterpret_cast<const char*>(member->data()),
                     static_cast<std::streamsize>(member->size()));
        buf.clear();
        return true;
    };

    dstd::String buf;
    buf.reserve(member_bytes + 65536);
    dstd::String line;
    while (input.next(line)) {
        buf += line;
        buf += '\n';
        if (buf.size() >= member_bytes && !flush(buf)) return false;
    }
    return flush(buf) && output.close();
}

TestEnvironment::TestEnvironment(std::size_t lines, Format format)
    : num_lines_(lines), format_(format) {
    // @note: enable this for debugging
    // dftracer::utils::logger::init();
    const dstd::Path test_path = make_unique_test_path("dftracer_test");

    // Leave test_dir empty to indicate failure
    const bool made = dstd::fs::create_directories(test_path).has_value();
    const auto is_dir = dstd::fs::is_directory(test_path);
    if (made && is_dir.has_value() && *is_dir)
        test_dir_ = dstd::String(test_path.str());
}

TestEnvironment::~TestEnvironment() {
    if (!test_dir_.empty()) {
        (void)dstd::fs::remove_all(dstd::PathView(test_dir_)).has_value();
    }
}

const dstd::String& TestEnvironment::get_dir() const { return test_dir_; }
bool TestEnvironment::is_valid() const { return !test_dir_.empty(); }

dstd::String TestEnvironment::create_test_file() {
    switch (format_) {
        case Format::Gzip:
            return create_test_gzip_file_impl();
        default:
            return "";
    }
}

dstd::String TestEnvironment::create_test_gzip_file() {
    return create_test_gzip_file_impl();
}

dstd::String TestEnvironment::create_test_gzip_file_impl() {
    if (test_dir_.empty()) {
        return "";
    }

    // Create test file in the unique directory
    dstd::String gz_file = test_dir_ + "/test_data.gz";
    dstd::String txt_file = test_dir_ + "/test_data.txt";

    // Write test data to text file
    dftu_utils_test::FileOut f(txt_file);
    if (!f.is_open()) {
        return "";
    }

    for (std::size_t i = 1; i <= num_lines_; ++i) {
        f << "{\"id\": " << i << ", \"message\": \"Test message " << i
          << "\"}\n";
    }
    f.close();

    bool success = compress_file_to_gzip(txt_file, gz_file);

    (void)dstd::fs::remove(txt_file).has_value();

    if (success) {
        return gz_file;
    }

    return "";
}

dstd::String TestEnvironment::get_index_path(const dstd::String& gz_file) {
    return dstd::String(
        dftracer::utils::trace::internal::determine_index_path(gz_file, ""));
}

dstd::String TestEnvironment::create_dft_test_file(int num_events) {
    static std::size_t file_counter = 0;
    dstd::String file_path =
        test_dir_ + "/dftu_trace_" + dstd::to_string(file_counter++) + ".trace";

    dftu_utils_test::FileOut ofs(file_path);
    if (!ofs.is_open()) {
        return "";
    }

    const char* io_names[] = {"pread", "pwrite", "read", "write",
                              "fread", "fwrite", "open", "close"};
    const char* io_cats[] = {"POSIX", "POSIX", "POSIX", "POSIX",
                             "STDIO", "STDIO", "POSIX", "POSIX"};
    const int num_names = sizeof(io_names) / sizeof(io_names[0]);

    ofs << "[\n";
    for (int i = 1; i <= num_events; ++i) {
        uint64_t timestamp_us =
            1000000000ULL + static_cast<uint64_t>(i * 100000);
        int size = 1024 * i;
        const char* op_name = io_names[i % num_names];
        const char* op_cat = io_cats[i % num_names];

        ofs << R"({"id":)" << i << R"(,"pid":)" << (1000 + i) << R"(,"tid":)"
            << (2000 + i) << R"(,"name":")" << op_name << R"(")"
            << R"(,"cat":")" << op_cat << R"(")"
            << R"(,"ph":"X")"
            << R"(,"ts":)" << timestamp_us << R"(,"dur":)" << (100 + i * 10)
            << R"(,"args":{"ret":)" << size << R"(,"hhash":"abc123"})"
            << R"(})" << "\n";
    }
    ofs << "]\n";
    ofs.close();

    return file_path;
}

dstd::String TestEnvironment::create_dft_multirun_gzip_file(
    int num_runs, std::uint64_t run_us, std::uint64_t gap_us) {
    static std::size_t multirun_counter = 0;
    dstd::String plain_file = test_dir_ + "/dftu_multirun_" +
                              dstd::to_string(multirun_counter++) + ".trace";
    dftu_utils_test::FileOut ofs(plain_file);
    if (!ofs.is_open()) return "";

    const std::uint64_t base = 1000000000ULL;
    ofs << "[\n";
    for (int r = 0; r < num_runs; ++r) {
        const std::int64_t pid = 100 + r;
        const std::uint64_t start =
            base + static_cast<std::uint64_t>(r) * (run_us + gap_us);
        const std::uint64_t end = start + run_us;
        auto emit = [&](const char* name, const char* cat, std::uint64_t ts,
                        const dstd::String& extra) {
            ofs << R"({"id":1,"pid":)" << pid << R"(,"tid":)" << (pid * 10)
                << R"(,"name":")" << name << R"(","cat":")" << cat
                << R"(","ph":"X","ts":)" << ts << R"(,"dur":1,"args":{)"
                << extra << R"(}})" << "\n";
        };
        emit("start", "dftracer", start,
             R"("hhash":"abc123","exec_hash":"app","cmd_hash":"cmd","ppid":1)");
        for (int i = 0; i < 3; ++i)
            ofs << R"({"id":1,"pid":)" << pid << R"(,"tid":)" << (pid * 10)
                << R"(,"name":"read","cat":"POSIX","ph":"X","ts":)"
                << (start + static_cast<std::uint64_t>(i) * (run_us / 4))
                << R"(,"dur":100,"args":{"hhash":"abc123","ret":1024}})"
                << "\n";
        emit("end", "dftracer", end, R"("hhash":"abc123","num_events":3)");
    }
    ofs << "]\n";
    ofs.close();

    dstd::String gz_file = plain_file + ".gz";
    if (!compress_file_to_gzip(plain_file, gz_file)) {
        (void)dstd::fs::remove(plain_file).has_value();
        return "";
    }
    (void)dstd::fs::remove(plain_file).has_value();
    return gz_file;
}

dstd::String TestEnvironment::create_dft_test_gzip_file(int num_events) {
    // First create a plain DFTracer trace file
    dstd::String plain_file = create_dft_test_file(num_events);
    if (plain_file.empty()) {
        return "";
    }

    // Compress to gzip
    dstd::String gz_file = plain_file + ".gz";
    if (!compress_file_to_gzip(plain_file, gz_file)) {
        (void)dstd::fs::remove(plain_file).has_value();
        return "";
    }

    // Remove the plain file after compression
    (void)dstd::fs::remove(plain_file).has_value();

    return gz_file;
}

void set_test_library_path(const dstd::String& binary) {
    auto resolved = dstd::fs::absolute(dstd::PathView(binary));
    if (!resolved) {
        ::setenv("LD_LIBRARY_PATH", "", 1);
        return;
    }
    const dstd::PathView parent = resolved->view().parent();
    const dstd::String libs = dstd::String((parent.parent() / "lib").str());
    ::setenv("LD_LIBRARY_PATH", libs.c_str(), 1);
}

dstd::String run_process_capture(const dstd::String& binary,
                                 const dstd::Vector<dstd::String>& args,
                                 bool capture_stderr, int* exit_code) {
    if (exit_code) *exit_code = -1;
    int fds[2];
    if (::pipe(fds) != 0) return "";

    dstd::Vector<const char*> argv;
    argv.push_back(binary.c_str());
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return "";
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        if (capture_stderr) ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        ::execv(binary.c_str(), const_cast<char* const*>(argv.data()));
        ::_exit(127);
    }

    ::close(fds[1]);
    // Drain to EOF before waiting: a child writing more than one pipe buffer
    // blocks until it is read, so waiting first would deadlock.
    dstd::String out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(fds[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(fds[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (exit_code) *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return out;
}

dstd::String gz_first_line(const dstd::String& gz_path) {
    gzFile f = ::gzopen(gz_path.c_str(), "rb");
    if (!f) return "";
    dstd::String line;
    char buf[65536];
    while (::gzgets(f, buf, sizeof(buf)) != nullptr) {
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty()) break;
    }
    ::gzclose(f);
    return line;
}
}  // namespace dftu_utils_test

// C API implementations
extern "C" {

test_environment_handle_t test_environment_create(void) {
    return test_environment_create_with_lines(100);
}

test_environment_handle_t test_environment_create_with_lines(
    std::size_t lines) {
    try {
        auto* env = new dftu_utils_test::TestEnvironment(
            lines, dftu_utils_test::Format::Gzip);
        if (env->is_valid()) {
            return reinterpret_cast<test_environment_handle_t>(env);
        } else {
            delete env;
            return nullptr;
        }
    } catch (...) {
        return nullptr;
    }
}

void test_environment_destroy(test_environment_handle_t env) {
    if (env) {
        auto* cpp_env =
            reinterpret_cast<dftu_utils_test::TestEnvironment*>(env);
        delete cpp_env;
    }
}

int test_environment_is_valid(test_environment_handle_t env) {
    if (!env) return 0;
    auto* cpp_env = reinterpret_cast<dftu_utils_test::TestEnvironment*>(env);
    return cpp_env->is_valid() ? 1 : 0;
}

const char* test_environment_get_dir(test_environment_handle_t env) {
    if (!env) return nullptr;
    auto* cpp_env = reinterpret_cast<dftu_utils_test::TestEnvironment*>(env);
    return cpp_env->get_dir().c_str();
}

char* test_make_unique_test_path(const char* name) {
    if (!name) return nullptr;
    dstd::String path =
        dstd::String(dftu_utils_test::make_unique_test_path(name).str());
    char* result = static_cast<char*>(malloc(path.length() + 1));
    if (result) {
        std::memcpy(result, path.c_str(), path.length() + 1);
    }
    return result;
}

char* test_environment_create_dft_gzip_file(test_environment_handle_t env,
                                            int num_events) {
    if (!env) return nullptr;
    auto* cpp_env = reinterpret_cast<dftu_utils_test::TestEnvironment*>(env);
    dstd::String gz_file = cpp_env->create_dft_test_gzip_file(num_events);
    if (gz_file.empty()) return nullptr;
    char* result = static_cast<char*>(malloc(gz_file.length() + 1));
    if (result) strcpy(result, gz_file.c_str());
    return result;
}

char* test_environment_create_test_gzip_file(test_environment_handle_t env) {
    if (!env) return nullptr;
    auto* cpp_env = reinterpret_cast<dftu_utils_test::TestEnvironment*>(env);
    dstd::String gz_file = cpp_env->create_test_gzip_file();
    if (gz_file.empty()) {
        return nullptr;
    }
    char* result = static_cast<char*>(malloc(gz_file.length() + 1));
    if (result) {
        strcpy(result, gz_file.c_str());
    }
    return result;
}

char* test_environment_get_index_path(test_environment_handle_t env,
                                      const char* gz_file) {
    if (!env || !gz_file) return nullptr;
    auto* cpp_env = reinterpret_cast<dftu_utils_test::TestEnvironment*>(env);
    dstd::String index_path = cpp_env->get_index_path(gz_file);
    char* result = static_cast<char*>(malloc(index_path.length() + 1));
    if (result) {
        strcpy(result, index_path.c_str());
    }
    return result;
}

char* test_environment_create_test_file_with_format(
    test_environment_handle_t env, test_format_t) {
    if (!env) return nullptr;
    auto* cpp_env = reinterpret_cast<dftu_utils_test::TestEnvironment*>(env);

    dftu_utils_test::Format cpp_format = dftu_utils_test::Format::Gzip;

    try {
        dftu_utils_test::TestEnvironment temp_env(
            cpp_env->get_dir().empty() ? 100 : 100, cpp_format);
        dstd::String file_path;

        file_path = cpp_env->create_test_gzip_file();

        if (file_path.empty()) {
            return nullptr;
        }

        char* result = static_cast<char*>(malloc(file_path.length() + 1));
        if (result) {
            strcpy(result, file_path.c_str());
        }
        return result;
    } catch (...) {
        return nullptr;
    }
}

int compress_file_to_gzip_c(const char* input_file, const char* output_file) {
    if (!input_file || !output_file) return 0;
    try {
        return dftu_utils_test::compress_file_to_gzip(input_file, output_file)
                   ? 1
                   : 0;
    } catch (...) {
        return 0;
    }
}
}

namespace dftu_utils_test {

dstd::Vector<ListedEntry> list_dir(dstd::PathView dir) {
    dstd::Vector<ListedEntry> out;
    auto handle = dstd::fs::Dir::open(dir);
    if (!handle) return out;
    auto entries = handle->entries();
    if (!entries) return out;
    for (const auto& e : *entries) {
        ListedEntry item;
        item.path = dstd::Path(dir) / e.name;
        auto st = handle->stat(e.name);
        if (!st) continue;
        item.kind = st->kind;
        item.size = st->size;
        out.push_back(std::move(item));
    }
    return out;
}

dstd::Vector<ListedEntry> walk_dir(dstd::PathView dir) {
    dstd::Vector<ListedEntry> out;
    bool at_root = true;
    auto walked =
        dstd::fs::walk(dir, [&](dstd::PathView p, dstd::fs::FileKind kind) {
            if (at_root) {
                at_root = false;
                return dstd::fs::WalkAction::Continue;
            }
            ListedEntry item;
            item.path = dstd::Path(p);
            item.kind = kind;
            if (kind == dstd::fs::FileKind::File) {
                auto st = dstd::fs::stat(p);
                if (!st) return dstd::fs::WalkAction::Continue;
                item.size = st->size;
            }
            out.push_back(std::move(item));
            return dstd::fs::WalkAction::Continue;
        });
    if (!walked) out.clear();
    std::sort(out.begin(), out.end(),
              [](const ListedEntry& a, const ListedEntry& b) {
                  return a.path.str() < b.path.str();
              });
    return out;
}

}  // namespace dftu_utils_test
