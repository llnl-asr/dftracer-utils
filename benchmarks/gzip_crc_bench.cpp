// SPDX-License-Identifier: MIT
// Benchmark: streaming gzip inflate with zlib-ng's own CRC32 versus raw
// inflate plus libdeflate's CRC32 (GzipStreamInflater). Both verify every
// member CRC. The zlib-ng side is the copy the library builds with.
//
// Not a unit test: built only with DFTRACER_UTILS_BUILD_BENCHMARKS=ON, or by
// hand against the utilities library, and run manually.
//
//   gzip_crc_bench [uncompressed_MB] [rounds]

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <zlib-ng.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace dftracer::utils;

namespace {

constexpr std::size_t OUT_CHUNK = 1u << 20;

dftracer::utils::dstd::String hex_lines(std::size_t bytes) {
    std::mt19937 rng(11);
    static const char HEX[] = "0123456789abcdef";
    dftracer::utils::dstd::String s;
    s.reserve(bytes + 128);
    while (s.size() < bytes) {
        s += "{\"id\":\"";
        for (int i = 0; i < 64; ++i) s += HEX[rng() & 15];
        s += "\",\"name\":\"read\",\"dur\":42}\n";
    }
    return s;
}

template <typename Inflate>
double run(const dftracer::utils::dstd::Vector<std::uint8_t>& gz,
           std::size_t expect, Inflate&& inflate_fn) {
    dftracer::utils::dstd::Vector<unsigned char> out(OUT_CHUNK);
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
    const dftracer::utils::dstd::String text = hex_lines(mb << 20);

    utilities::fileio::compress::GzipMemberCompressor c(6);
    dftracer::utils::dstd::Vector<std::uint8_t> gz;
    if (!c.compress_member_into(gz, text.data(), text.size())) return 1;

    auto zlib_gzip = [](const dftracer::utils::dstd::Vector<std::uint8_t>& in,
                        dftracer::utils::dstd::Vector<unsigned char>& out,
                        std::size_t& total) {
        zng_stream zs{};
        if (zng_inflateInit2(&zs, 31) != Z_OK) std::exit(2);
        zs.next_in = in.data();
        zs.avail_in = static_cast<std::uint32_t>(in.size());
        int rc;
        do {
            zs.next_out = out.data();
            zs.avail_out = static_cast<std::uint32_t>(out.size());
            rc = zng_inflate(&zs, Z_NO_FLUSH);
            total += out.size() - zs.avail_out;
        } while (rc == Z_OK);
        zng_inflateEnd(&zs);
        if (rc != Z_STREAM_END) std::exit(2);
    };
    auto libdeflate_crc =
        [](const dftracer::utils::dstd::Vector<std::uint8_t>& in,
           dftracer::utils::dstd::Vector<unsigned char>& out,
           std::size_t& total) {
            utilities::fileio::compress::GzipStreamInflater inf;
            inf.init(31);
            inf.set_input(in.data(), in.size());
            utilities::fileio::compress::InflateStatus rc;
            do {
                inf.set_output(out.data(), out.size());
                rc = inf.inflate();
                total += out.size() - inf.avail_out();
            } while (rc == utilities::fileio::compress::InflateStatus::Ok);
            if (rc != utilities::fileio::compress::InflateStatus::StreamEnd)
                std::exit(2);
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
    std::printf("zlib-ng gzip (zlib-ng crc32):    %7.1f MB/s  %.3f s\n",
                mbs / best_zlib, best_zlib);
    std::printf("raw inflate + libdeflate crc32:  %7.1f MB/s  %.3f s\n",
                mbs / best_new, best_new);

    double crc_zlib = 1e9;
    double crc_new = 1e9;
    std::uint32_t sink = 0;
    for (int r = 0; r < rounds; ++r) {
        auto t0 = std::chrono::steady_clock::now();
        sink += static_cast<std::uint32_t>(
            zng_crc32_z(0, reinterpret_cast<const std::uint8_t*>(text.data()),
                        text.size()));
        auto t1 = std::chrono::steady_clock::now();
        sink += utilities::fileio::compress::gzip_crc32(0, text.data(),
                                                        text.size());
        auto t2 = std::chrono::steady_clock::now();
        crc_zlib =
            std::min(crc_zlib, std::chrono::duration<double>(t1 - t0).count());
        crc_new =
            std::min(crc_new, std::chrono::duration<double>(t2 - t1).count());
    }
    std::printf("crc32 only, zlib-ng crc32_z:     %7.1f MB/s\n",
                mbs / crc_zlib);
    std::printf("crc32 only, libdeflate:          %7.1f MB/s (sink %u)\n",
                mbs / crc_new, sink);
    return 0;
}
