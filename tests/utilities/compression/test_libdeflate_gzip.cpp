#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <latch>
#include <string>
#include <thread>
#include <vector>

using dftracer::utils::utilities::fileio::compress::GzipMemberCompressor;
using dftracer::utils::utilities::fileio::compress::GzipMemberDecompressor;

namespace {

std::vector<std::uint8_t> make_payload(std::size_t n, unsigned seed) {
    // Deterministic, compressible-ish bytes (no Math.random equivalent needed).
    std::vector<std::uint8_t> v(n);
    std::uint32_t x = seed * 2654435761u + 1u;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = static_cast<std::uint8_t>((x >> 24) % 64);  // small alphabet
    }
    return v;
}

}  // namespace

TEST_SUITE("libdeflate_gzip") {
    // First in the file: libdeflate's lazy kernel selection must be resolved
    // before threads first decode at once (a data race under TSan otherwise).
    TEST_CASE("concurrent first use decodes on every thread") {
        // gzip.compress(b"dftracer " * 64, mtime=0)
        static const std::uint8_t MEMBER[] = {
            0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
            0xff, 0x4b, 0x49, 0x2b, 0x29, 0x4a, 0x4c, 0x4e, 0x2d,
            0x52, 0x48, 0x19, 0x65, 0x8c, 0x32, 0x48, 0x67, 0x00,
            0x00, 0x13, 0xc0, 0xd8, 0x38, 0x40, 0x02, 0x00, 0x00};
        std::string want;
        for (int i = 0; i < 64; ++i) want += "dftracer ";

        constexpr int THREADS = 8;
        std::latch start(THREADS);
        std::vector<int> ok(THREADS, 0);
        std::vector<std::thread> threads;
        for (int t = 0; t < THREADS; ++t)
            threads.emplace_back([&, t] {
                start.arrive_and_wait();
                GzipMemberDecompressor dec;
                auto out =
                    dec.decompress_member(MEMBER, sizeof(MEMBER), want.size());
                ok[static_cast<std::size_t>(t)] =
                    out && std::string(out->begin(), out->end()) == want;
            });
        for (auto& th : threads) th.join();
        for (int v : ok) CHECK(v == 1);
    }

    TEST_CASE("round-trips a single member") {
        GzipMemberCompressor comp;
        GzipMemberDecompressor dec;
        REQUIRE(comp.valid());
        REQUIRE(dec.valid());

        const auto payload = make_payload(4096, 7);
        auto member = comp.compress_member(payload.data(), payload.size());
        REQUIRE(member.has_value());
        CHECK(member->size() > 0);
        CHECK(member->size() < payload.size());  // small alphabet compresses

        auto out = dec.decompress_member(member->data(), member->size(),
                                         payload.size());
        REQUIRE(out.has_value());
        CHECK(*out == payload);
    }

    TEST_CASE("decodes concatenated members independently via in_bytes") {
        GzipMemberCompressor comp;
        GzipMemberDecompressor dec;

        const auto a = make_payload(2000, 1);
        const auto b = make_payload(3000, 2);
        auto ma = comp.compress_member(a.data(), a.size());
        auto mb = comp.compress_member(b.data(), b.size());
        REQUIRE(ma.has_value());
        REQUIRE(mb.has_value());

        // Concatenate the two gzip members into one buffer (multi-member gzip).
        std::vector<std::uint8_t> file;
        file.insert(file.end(), ma->begin(), ma->end());
        file.insert(file.end(), mb->begin(), mb->end());

        // Member 1: decode from offset 0; in_bytes tells us where member 2
        // starts.
        std::vector<std::uint8_t> out_a(a.size());
        auto r1 = dec.decompress(file.data(), file.size(), out_a.data(),
                                 out_a.size());
        REQUIRE(r1.has_value());
        CHECK(r1->out_bytes == a.size());
        CHECK(r1->in_bytes == ma->size());
        out_a.resize(r1->out_bytes);
        CHECK(out_a == a);

        // Member 2: decode starting exactly after member 1.
        const std::uint8_t* m2 = file.data() + r1->in_bytes;
        const std::size_t m2_len = file.size() - r1->in_bytes;
        std::vector<std::uint8_t> out_b(b.size());
        auto r2 = dec.decompress(m2, m2_len, out_b.data(), out_b.size());
        REQUIRE(r2.has_value());
        CHECK(r2->out_bytes == b.size());
        CHECK(r2->in_bytes == mb->size());
        out_b.resize(r2->out_bytes);
        CHECK(out_b == b);
    }

    TEST_CASE("round-trips an empty payload") {
        GzipMemberCompressor comp;
        GzipMemberDecompressor dec;
        auto member = comp.compress_member(nullptr, 0);
        REQUIRE(member.has_value());
        auto out = dec.decompress_member(member->data(), member->size(), 0);
        REQUIRE(out.has_value());
        CHECK(out->empty());
    }

    TEST_CASE("fails cleanly when the output buffer is too small") {
        GzipMemberCompressor comp;
        GzipMemberDecompressor dec;
        const auto payload = make_payload(1024, 9);
        auto member = comp.compress_member(payload.data(), payload.size());
        REQUIRE(member.has_value());

        std::vector<std::uint8_t> too_small(payload.size() / 2);
        auto out = dec.decompress(member->data(), member->size(),
                                  too_small.data(), too_small.size());
        CHECK_FALSE(out.has_value());
    }

    TEST_CASE("rejects non-gzip input") {
        GzipMemberDecompressor dec;
        const std::vector<std::uint8_t> junk(64, 0xAB);
        std::vector<std::uint8_t> out(256);
        auto r =
            dec.decompress(junk.data(), junk.size(), out.data(), out.size());
        CHECK_FALSE(r.has_value());
    }
}
