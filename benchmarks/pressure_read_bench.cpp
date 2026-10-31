// Benchmark: what one read of the OS memory pressure signals costs, and so how
// often the memory manager can afford to read them.
//
// macOS times task_info TASK_VM_INFO (phys_footprint). Linux times cgroup v2
// memory.stat (anon + shmem), memory.current and /proc/self/status (RssAnon),
// each with open, read, close every time and with one fd kept open and re-read
// by pread at offset 0. Without cgroup v2 the cgroup rows are skipped. Every
// platform also times memory_probe::read().
//
// The interval column is the smallest interval at which reads take under 0.1%
// of one core: ns per read / 0.001.
//
//   pressure_read_bench           (5 runs of 10000 reads per kind)
//   pressure_read_bench --smoke   (5 runs of 100 reads, checks each read is
//   nonzero)

#include <memory_probe.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#ifndef __APPLE__
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mp = dftu::memory_probe;

namespace {

constexpr std::size_t RUNS = 5;
constexpr std::size_t FULL_READS = 10000;
constexpr std::size_t SMOKE_READS = 100;
constexpr double CORE_SHARE = 0.001;

struct Row {
    std::string name;
    double ns_per_read;
};

volatile std::uint64_t sink = 0;

template <class Fn>
bool measure(const std::string& name, std::size_t reads, Fn read_once,
             std::vector<Row>& rows) {
    std::vector<double> runs;
    bool nonzero = true;
    for (std::size_t r = 0; r < RUNS; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < reads; ++i) {
            const std::uint64_t v = read_once();
            if (v == 0) nonzero = false;
            sink = sink + v;
        }
        const auto t1 = std::chrono::steady_clock::now();
        runs.push_back(
            std::chrono::duration<double, std::nano>(t1 - t0).count() /
            static_cast<double>(reads));
    }
    std::sort(runs.begin(), runs.end());
    rows.push_back({name, runs[runs.size() / 2]});
    if (!nonzero) std::printf("%s returned zero  FAIL\n", name.c_str());
    return nonzero;
}

#ifdef __APPLE__

std::uint64_t task_footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.phys_footprint;
}

bool run_platform(std::size_t reads, std::vector<Row>& rows) {
    return measure("task_info TASK_VM_INFO", reads, task_footprint, rows);
}

#else

constexpr std::size_t BUFFER_BYTES = 16384;
constexpr const char* CGROUP_ROOT = "/sys/fs/cgroup";

struct Buffer {
    char data[BUFFER_BYTES];
    std::size_t size = 0;
};

class KeptFd {
   public:
    explicit KeptFd(const std::string& path)
        : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {}
    ~KeptFd() {
        if (fd_ >= 0) ::close(fd_);
    }
    KeptFd(const KeptFd&) = delete;
    KeptFd& operator=(const KeptFd&) = delete;

    bool ok() const { return fd_ >= 0; }
    bool reread(Buffer& b) const {
        const ssize_t n = ::pread(fd_, b.data, BUFFER_BYTES - 1, 0);
        if (n <= 0) return false;
        b.size = static_cast<std::size_t>(n);
        b.data[b.size] = '\0';
        return true;
    }

   private:
    int fd_;
};

bool read_once_file(const std::string& path, Buffer& b) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = ::read(fd, b.data, BUFFER_BYTES - 1);
    ::close(fd);
    if (n <= 0) return false;
    b.size = static_cast<std::size_t>(n);
    b.data[b.size] = '\0';
    return true;
}

std::uint64_t parse_stat(const Buffer& b) {
    const std::string_view text(b.data, b.size);
    std::uint64_t anon = 0;
    std::uint64_t shmem = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view line = text.substr(pos, eol - pos);
        if (line.starts_with("anon "))
            anon = std::strtoull(line.data() + 5, nullptr, 10);
        else if (line.starts_with("shmem "))
            shmem = std::strtoull(line.data() + 6, nullptr, 10);
        pos = eol + 1;
    }
    return anon + shmem;
}

std::uint64_t parse_current(const Buffer& b) {
    return std::strtoull(b.data, nullptr, 10);
}

std::uint64_t parse_status(const Buffer& b) {
    const std::string_view text(b.data, b.size);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string line(text.substr(pos, eol - pos));
        if (const auto v = mp::detail::kib_field(line, "RssAnon:")) return v;
        pos = eol + 1;
    }
    return 0;
}

std::string cgroup_dir() {
    std::string path;
    Buffer b;
    if (!read_once_file("/proc/self/cgroup", b)) return path;
    const std::string_view text(b.data, b.size);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view line = text.substr(pos, eol - pos);
        if (line.starts_with("0::"))
            path = std::string(CGROUP_ROOT) + std::string(line.substr(3));
        pos = eol + 1;
    }
    return path;
}

template <class Parse>
bool measure_file(const std::string& label, const std::string& path,
                  std::size_t reads, Parse parse, std::vector<Row>& rows) {
    bool ok = true;
    Buffer b;
    ok &= measure(
        label + ", open/read/close", reads,
        [&] { return read_once_file(path, b) ? parse(b) : 0; }, rows);
    const KeptFd fd(path);
    if (!fd.ok()) {
        std::printf("%s: cannot open  FAIL\n", label.c_str());
        return false;
    }
    ok &= measure(
        label + ", pread kept fd", reads,
        [&] { return fd.reread(b) ? parse(b) : 0; }, rows);
    return ok;
}

bool run_platform(std::size_t reads, std::vector<Row>& rows) {
    bool ok = true;
    const std::string dir = cgroup_dir();
    if (dir.empty()) {
        std::printf("cgroup v2 is absent, timing /proc/self/status only\n");
    } else {
        ok &= measure(
            "probe cgroup anon+shmem (reads /proc/self/cgroup too)", reads,
            [] {
                std::uint64_t v = 0;
                return mp::detail::cgroup_anon_bytes(v) ? v : 0;
            },
            rows);
        ok &= measure_file("memory.stat anon+shmem", dir + "/memory.stat",
                           reads, parse_stat, rows);
        ok &= measure_file("memory.current", dir + "/memory.current", reads,
                           parse_current, rows);
    }
    ok &= measure_file("/proc/self/status RssAnon", "/proc/self/status", reads,
                       parse_status, rows);
    return ok;
}

#endif

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && std::string_view(argv[1]) == "--smoke";
    if (argc > 2 || (argc == 2 && !smoke)) {
        std::fprintf(stderr, "usage: pressure_read_bench [--smoke]\n");
        return 2;
    }
    const std::size_t reads = smoke ? SMOKE_READS : FULL_READS;
    std::vector<Row> rows;
    bool ok = run_platform(reads, rows);
    ok &= measure(
        "memory_probe::read()", reads, [] { return mp::read().anon; }, rows);

    std::printf("median of %zu runs of %zu reads\n", RUNS, reads);
    std::printf("%-62s %12s %22s\n", "read", "ns per read",
                "interval for 0.1% core");
    for (const Row& r : rows)
        std::printf("%-62s %12.0f %19.3f ms\n", r.name.c_str(), r.ns_per_read,
                    r.ns_per_read / CORE_SHARE / 1e6);
    return ok ? 0 : 1;
}
