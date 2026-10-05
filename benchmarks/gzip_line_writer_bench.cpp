// Benchmark: NDJSON lines through GzipLineWriterBlocking (member size, level,
// compress=false) against a replica of the dftracer_pgzip pipeline (fixed
// 4 MiB raw chunks, N GzipMemberCompressor workers, ordered writer). The
// pgzip loop is replicated with threads here because its coroutines are
// private to the binary. Not a unit test: built only with
// DFTRACER_UTILS_BUILD_BENCHMARKS=ON.
//
//   gzip_line_writer_bench <input.pfw> [workdir]

#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fileio = dftracer::utils::utilities::fileio;
namespace compress = dftracer::utils::utilities::fileio::compress;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr std::size_t MIB = 1024 * 1024;
constexpr int ROUNDS = 3;

std::vector<std::string_view> split_appends(const std::string& data) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos < data.size()) {
        std::size_t end = std::min(pos + MIB, data.size());
        if (end < data.size()) {
            auto nl = data.rfind('\n', end - 1);
            end = (nl == std::string::npos || nl < pos)
                      ? data.find('\n', end) + 1
                      : nl + 1;
        }
        out.emplace_back(data.data() + pos, end - pos);
        pos = end;
    }
    return out;
}

std::uint64_t run_writer(const std::vector<std::string_view>& appends,
                         const std::string& path, std::size_t member, int level,
                         bool comp) {
    fileio::GzipWriterOptions o;
    o.member_size = member;
    o.level = level;
    o.compress = comp;
    auto w = fileio::GzipLineWriterBlocking::open(path, std::move(o));
    if (!w) {
        std::fprintf(stderr, "open failed\n");
        std::exit(1);
    }
    for (auto a : appends)
        if (!w->append(a)) {
            std::fprintf(stderr, "append failed\n");
            std::exit(1);
        }
    auto s = w->close();
    if (!s) {
        std::fprintf(stderr, "close failed\n");
        std::exit(1);
    }
    return s->c_bytes;
}

std::uint64_t run_pgzip(const std::string& data, const std::string& path) {
    constexpr std::size_t CHUNK = 4 * MIB;
    const std::size_t workers =
        std::max(1u, std::thread::hardware_concurrency());
    const std::size_t nchunks = (data.size() + CHUNK - 1) / CHUNK;
    std::mutex mu;
    std::condition_variable cv;
    std::map<std::size_t, std::string> done;
    std::size_t next_claim = 0, next_write = 0;
    std::uint64_t out_bytes = 0;

    auto worker = [&] {
        compress::GzipMemberCompressor c(6);
        std::vector<std::uint8_t> scratch;
        for (;;) {
            std::size_t i;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait(lk, [&] {
                    return next_claim >= nchunks ||
                           next_claim < next_write + workers * 2;
                });
                if (next_claim >= nchunks) return;
                i = next_claim++;
            }
            std::size_t off = i * CHUNK;
            std::size_t len = std::min(CHUNK, data.size() - off);
            std::string chunk(data.data() + off, len);
            c.compress_member_into(scratch, chunk.data(), chunk.size());
            std::string res(reinterpret_cast<const char*>(scratch.data()),
                            scratch.size());
            {
                std::lock_guard<std::mutex> lk(mu);
                done.emplace(i, std::move(res));
            }
            cv.notify_all();
        }
    };
    std::vector<std::thread> pool;
    for (std::size_t t = 0; t < workers; ++t) pool.emplace_back(worker);

    std::ofstream ofs(path, std::ios::binary);
    while (next_write < nchunks) {
        std::string blob;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [&] { return done.count(next_write) != 0; });
            auto it = done.find(next_write);
            blob = std::move(it->second);
            done.erase(it);
            ++next_write;
        }
        cv.notify_all();
        ofs.write(blob.data(), static_cast<std::streamsize>(blob.size()));
        out_bytes += blob.size();
    }
    ofs.close();
    for (auto& t : pool) t.join();
    return out_bytes;
}

double memcpy_loop(const std::vector<std::string_view>& appends,
                   std::size_t member) {
    std::vector<char> buf(member + 2 * MIB);
    auto t0 = Clock::now();
    std::size_t fill = 0;
    for (auto a : appends) {
        std::memcpy(buf.data() + fill, a.data(), a.size());
        fill += a.size();
        if (fill >= member) fill = 0;
    }
    volatile char sink = buf[0];
    (void)sink;
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

double run_case(const char* name, std::size_t in_bytes, const fs::path& dir,
                const std::function<std::uint64_t(const std::string&)>& fn) {
    std::vector<double> ms;
    std::uint64_t out = 0;
    const std::string path = (dir / "out.pfw.gz").string();
    for (int r = 0; r < ROUNDS; ++r) {
        auto t0 = Clock::now();
        out = fn(path);
        ms.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - t0)
                .count());
        fs::remove(path);
    }
    std::sort(ms.begin(), ms.end());
    double med = ms[ms.size() / 2];
    std::printf("%-28s %9.1f ms %9.1f MB/s %9.1f out_MB\n", name, med,
                static_cast<double>(in_bytes) / 1e6 / (med / 1e3),
                static_cast<double>(out) / 1e6);
    std::fflush(stdout);
    return med;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <input.pfw> [workdir]\n", argv[0]);
        return 2;
    }
    std::string data;
    {
        std::ifstream ifs(argv[1], std::ios::binary | std::ios::ate);
        if (!ifs) {
            std::fprintf(stderr, "cannot open %s\n", argv[1]);
            return 2;
        }
        data.resize(static_cast<std::size_t>(ifs.tellg()));
        ifs.seekg(0);
        ifs.read(data.data(), static_cast<std::streamsize>(data.size()));
    }
    if (data.empty() || data.back() != '\n') data.push_back('\n');
    fs::path base = argc > 2 ? fs::path(argv[2]) : fs::temp_directory_path();
    fs::path dir = base / "gzip_line_writer_bench";
    fs::create_directories(dir);
    auto appends = split_appends(data);

    std::printf("input %.1f MB, %zu appends\n", data.size() / 1e6,
                appends.size());
    std::printf("%-28s %12s %14s %13s\n", "case", "ms", "MB/s", "out_MB");

    auto writer_case = [&](const char* n, std::size_t m, int lvl, bool comp) {
        return run_case(n, data.size(), dir, [&](const std::string& p) {
            return run_writer(appends, p, m, lvl, comp);
        });
    };
    writer_case("writer 4MiB L1", 4 * MIB, 1, true);
    writer_case("writer 4MiB L6", 4 * MIB, 6, true);
    writer_case("writer 32MiB L1", 32 * MIB, 1, true);
    writer_case("writer 32MiB L6", 32 * MIB, 6, true);
    writer_case("writer 4MiB compress=false", 4 * MIB, 6, false);
    run_case("pgzip loop 4MiB L6", data.size(), dir,
             [&](const std::string& p) { return run_pgzip(data, p); });

    std::vector<double> cp;
    for (int r = 0; r < ROUNDS; ++r)
        cp.push_back(memcpy_loop(appends, 4 * MIB));
    std::sort(cp.begin(), cp.end());
    std::printf("%-28s %9.1f ms %9.1f MB/s (append copy into a 4MiB member)\n",
                "memcpy only", cp[1],
                static_cast<double>(data.size()) / 1e6 / (cp[1] / 1e3));
    fs::remove_all(dir);
    return 0;
}
