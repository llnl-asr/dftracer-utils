// Benchmark: streaming gzip inflate with zlib's own CRC32 versus raw inflate
// plus libdeflate's CRC32 (GzipStreamInflater). Both verify every member CRC.
//
// Not a unit test: built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON, or by
// hand against the utilities library, and run manually.
//
//   gzip_crc_bench [uncompressed_MB] [rounds]

#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace gzc = dftracer::utils::utilities::fileio::compress;

namespace {

constexpr std::size_t OUT_CHUNK = 1u << 20;

std::string hex_lines(std::size_t bytes) {
    std::mt19937 rng(11);
    static const char HEX[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes + 128);
    while (s.size() < bytes) {
        s += "{\"id\":\"";
        for (int i = 0; i < 64; ++i) s += HEX[rng() & 15];
        s += "\",\"name\":\"read\",\"dur\":42}\n";
    }
    return s;
}

template <typename Inflate>
double run(const std::vector<std::uint8_t>& gz, std::size_t expect,
           Inflate&& inflate_fn) {
    std::vector<unsigned char> out(OUT_CHUNK);
    const auto t0 = std::chrono::steady_clock::now();
    std::size_t total = 0;
    inflate_fn(gz, out, total);
    const auto t1 = std::chrono::steady_clock::now();
    if (total != expect) {
        std::fprintf(stderr, "decoded %zu bytes, expected %zu\n", total,
                     expect);
        std::exit(1);
    }
    return std::chrono::duration<double>(t1 - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t mb = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 64;
    const int rounds = argc > 2 ? std::atoi(argv[2]) : 5;
    const std::string text = hex_lines(mb << 20);

    gzc::GzipMemberCompressor c(6);
    std::vector<std::uint8_t> gz;
    if (!c.compress_member_into(gz, text.data(), text.size())) return 1;

    auto zlib_gzip = [](const std::vector<std::uint8_t>& in,
                        std::vector<unsigned char>& out, std::size_t& total) {
        z_stream zs{};
        inflateInit2(&zs, 31);
        zs.next_in = const_cast<Bytef*>(in.data());
        zs.avail_in = static_cast<uInt>(in.size());
        int rc;
        do {
            zs.next_out = out.data();
            zs.avail_out = static_cast<uInt>(out.size());
            rc = inflate(&zs, Z_NO_FLUSH);
            total += out.size() - zs.avail_out;
        } while (rc == Z_OK);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END) std::exit(2);
    };
    auto libdeflate_crc = [](const std::vector<std::uint8_t>& in,
                             std::vector<unsigned char>& out,
                             std::size_t& total) {
        gzc::GzipStreamInflater inf;
        inf.init(31);
        z_stream& zs = inf.z();
        zs.next_in = const_cast<Bytef*>(in.data());
        zs.avail_in = static_cast<uInt>(in.size());
        int rc;
        do {
            zs.next_out = out.data();
            zs.avail_out = static_cast<uInt>(out.size());
            rc = inf.inflate(Z_NO_FLUSH);
            total += out.size() - zs.avail_out;
        } while (rc == Z_OK);
        if (rc != Z_STREAM_END) std::exit(2);
    };

    double best_zlib = 1e9;
    double best_new = 1e9;
    for (int r = 0; r < rounds; ++r) {
        best_zlib = std::min(best_zlib, run(gz, text.size(), zlib_gzip));
        best_new = std::min(best_new, run(gz, text.size(), libdeflate_crc));
    }
    const double mbs = static_cast<double>(text.size()) / (1 << 20);
    std::printf("input %.0f MB uncompressed, %.1f MB compressed, best of %d\n",
                mbs, static_cast<double>(gz.size()) / (1 << 20), rounds);
    std::printf("zlib gzip (zlib crc32):          %7.1f MB/s  %.3f s\n",
                mbs / best_zlib, best_zlib);
    std::printf("raw inflate + libdeflate crc32:  %7.1f MB/s  %.3f s\n",
                mbs / best_new, best_new);

    double crc_zlib = 1e9;
    double crc_new = 1e9;
    std::uint32_t sink = 0;
    for (int r = 0; r < rounds; ++r) {
        auto t0 = std::chrono::steady_clock::now();
        sink += static_cast<std::uint32_t>(crc32_z(
            0, reinterpret_cast<const Bytef*>(text.data()), text.size()));
        auto t1 = std::chrono::steady_clock::now();
        sink += gzc::gzip_crc32(0, text.data(), text.size());
        auto t2 = std::chrono::steady_clock::now();
        crc_zlib =
            std::min(crc_zlib, std::chrono::duration<double>(t1 - t0).count());
        crc_new =
            std::min(crc_new, std::chrono::duration<double>(t2 - t1).count());
    }
    std::printf("crc32 only, zlib crc32_z:        %7.1f MB/s\n",
                mbs / crc_zlib);
    std::printf("crc32 only, libdeflate:          %7.1f MB/s (sink %u)\n",
                mbs / crc_new, sink);
    return 0;
}
