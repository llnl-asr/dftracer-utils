#define DFTRACER_UTILS_MEMORY_PROBE_DEFINE_NEW
#include <memory_probe.h>

#include <cstdio>

namespace {

constexpr std::uint64_t MIB = 1024ULL * 1024ULL;
constexpr std::size_t BUFFER_BYTES = 64 * MIB;
constexpr std::uint64_t MIN_MOVE = 60 * MIB;
constexpr std::size_t PAGE = 4096;

int failures = 0;

void check_moved(const char* name, std::uint64_t before, std::uint64_t after) {
    const bool ok = after >= before && after - before >= MIN_MOVE;
    std::printf("%-12s before %8.1f MiB after %8.1f MiB  %s\n", name,
                static_cast<double>(before) / MIB,
                static_cast<double>(after) / MIB, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

}  // namespace

int main() {
    namespace mp = dftu::memory_probe;

    const mp::Reading before = mp::read();
    const std::int64_t live_before = mp::live_bytes();
    const std::uint64_t bucket_before =
        mp::histogram_count(mp::histogram_bucket(BUFFER_BYTES));

    char* buffer = new char[BUFFER_BYTES];
    volatile char* touch = buffer;
    for (std::size_t i = 0; i < BUFFER_BYTES; i += PAGE) {
        touch[i] = static_cast<char>(i / PAGE | 1);
    }
    const mp::Reading during = mp::read();
    const std::int64_t live_during = mp::live_bytes();
    const std::uint64_t bucket_during =
        mp::histogram_count(mp::histogram_bucket(BUFFER_BYTES));

    check_moved("current_rss", before.current_rss, during.current_rss);
    check_moved("peak_rss", before.current_rss, during.peak_rss);
    check_moved("anon", before.anon, during.anon);
    if (before.cgroup_present && during.cgroup_present) {
        check_moved("cgroup_anon", before.cgroup_anon, during.cgroup_anon);
    } else {
        std::printf(
            "cgroup_anon  absent (no cgroup v2 memory.stat); skipped\n");
    }

    const bool live_up =
        live_during - live_before >= static_cast<std::int64_t>(BUFFER_BYTES);
    std::printf("live_bytes   before %lld during %lld  %s\n",
                static_cast<long long>(live_before),
                static_cast<long long>(live_during), live_up ? "ok" : "FAIL");
    if (!live_up) ++failures;

    if (bucket_during != bucket_before + 1) {
        std::printf("histogram    bucket %zu did not count the buffer  FAIL\n",
                    mp::histogram_bucket(BUFFER_BYTES));
        ++failures;
    }

    delete[] buffer;
    const std::int64_t live_after = mp::live_bytes();
    const bool back = live_after == live_before;
    std::printf("live_bytes   after free %lld  %s\n",
                static_cast<long long>(live_after), back ? "ok" : "FAIL");
    if (!back) ++failures;

    return failures == 0 ? 0 : 1;
}
