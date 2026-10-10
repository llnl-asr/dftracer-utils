#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::utilities::fileio::parallel;

namespace {

constexpr std::size_t STRIPE = 1 * 1024 * 1024;  // 1 MB stripe for the test

std::string tmp_path(const char* name) {
    return dftu_utils_test::make_unique_test_path(name).string();
}

// Compress `data` into a standalone gzip member with zlib.
std::vector<std::uint8_t> gzip_member(const std::string& data) {
    uLongf bound = compressBound(data.size()) + 64;
    std::vector<std::uint8_t> out(bound);
    z_stream s{};
    REQUIRE(deflateInit2(&s, Z_BEST_SPEED, Z_DEFLATED, 15 | 16, 8,
                         Z_DEFAULT_STRATEGY) == Z_OK);
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    s.avail_in = static_cast<uInt>(data.size());
    s.next_out = out.data();
    s.avail_out = static_cast<uInt>(out.size());
    REQUIRE(deflate(&s, Z_FINISH) == Z_STREAM_END);
    out.resize(s.total_out);
    deflateEnd(&s);
    return out;
}

ByteView as_bv(const std::vector<std::uint8_t>& v) {
    return ByteView(reinterpret_cast<const std::byte*>(v.data()), v.size());
}

// Decompress the whole file via gzread (spans concatenated members natively).
std::string gunzip_all(const std::string& path) {
    gzFile g = gzopen(path.c_str(), "rb");
    REQUIRE(g != nullptr);
    std::string out;
    char buf[4096];
    int n;
    while ((n = gzread(g, buf, sizeof(buf))) > 0) {
        out.append(buf, n);
    }
    gzclose(g);
    return out;
}

}  // namespace

TEST_CASE("PaddedStripedWriter - gzip -t passes and payload round-trips") {
    std::string path = tmp_path("test_padded_basic.json.gz");
    std::remove(path.c_str());

    std::string hdr_payload = "HDR\n";
    std::string w0_payload = R"({"ev":0})"
                             "\n";
    std::string w1_payload = R"({"ev":1})"
                             "\n";
    std::string w2_payload = R"({"ev":2})"
                             "\n";
    std::string ftr_payload = "]\n";

    auto hdr_mem = gzip_member(hdr_payload);
    auto w0_mem = gzip_member(w0_payload);
    auto w1_mem = gzip_member(w1_payload);
    auto w2_mem = gzip_member(w2_payload);
    auto ftr_mem = gzip_member(ftr_payload);

    Runtime runtime(2);
    int result = -1;
    runtime
        .scope("padded_basic",
               [&](CoroScope& s) -> CoroTask<void> {
                   auto w = make_padded_striped_writer(STRIPE);
                   if (co_await w->open(path, 3, true, &s) != 0) {
                       result = 1;
                       co_return;
                   }
                   if (co_await w->write_header(as_bv(hdr_mem)) != 0) {
                       result = 2;
                       co_return;
                   }
                   if (co_await w->write_chunk(0, as_bv(w0_mem)) != 0) {
                       result = 3;
                       co_return;
                   }
                   if (co_await w->write_chunk(1, as_bv(w1_mem)) != 0) {
                       result = 4;
                       co_return;
                   }
                   if (co_await w->write_chunk(2, as_bv(w2_mem)) != 0) {
                       result = 5;
                       co_return;
                   }
                   if (co_await w->write_footer(as_bv(ftr_mem)) != 0) {
                       result = 6;
                       co_return;
                   }
                   if (co_await w->close() != 0) {
                       result = 7;
                       co_return;
                   }
                   result = 0;
               })
        .get();
    CHECK(result == 0);

    // With coalescing, the three tiny worker members pack into a single
    // stripe. File = [header stripe][one coalesced stripe][footer bytes].
    CHECK(fs::file_size(path) == 2 * STRIPE + ftr_mem.size());

    auto decompressed = gunzip_all(path);
    // Decompressed content = hdr + w0 + w1 + w2 + footer, with worker ordering
    // determined by the atomic stripe allocation (sequential here since we
    // serialized calls).
    CHECK(decompressed ==
          hdr_payload + w0_payload + w1_payload + w2_payload + ftr_payload);

    fs::remove(path);
}

// GzipLineWriter passes its own scope to open(), runs its compress jobs in it
// and joins that scope before it calls close(). A packer started in that scope
// could only end in close(), so the join never returned.
TEST_CASE("PaddedStripedWriter - the caller's scope joins before close") {
    std::string path = tmp_path("test_padded_join.json.gz");
    std::remove(path.c_str());
    constexpr std::size_t WORKERS = 8;
    std::vector<std::string> payloads;
    std::vector<std::vector<std::uint8_t>> members;
    for (std::size_t i = 0; i < WORKERS; ++i) {
        payloads.push_back(R"({"w":)" + std::to_string(i) + "}\n");
        members.push_back(gzip_member(payloads.back()));
    }
    Runtime runtime(4);
    int result = -1;
    runtime
        .scope(
            "padded_join",
            [&](CoroScope&) -> CoroTask<void> {
                auto w = make_padded_striped_writer(STRIPE);
                CoroScope jobs;
                if (co_await w->open(path, WORKERS, true, &jobs) != 0) {
                    result = 1;
                    co_return;
                }
                std::atomic<int> failures{0};
                for (std::size_t i = 0; i < WORKERS; ++i)
                    jobs.spawn([&, i](CoroScope&) -> CoroTask<void> {
                        if (co_await w->write_chunk(i, as_bv(members[i])) != 0)
                            failures.fetch_add(1);
                    });
                co_await jobs.join();
                if (failures.load() != 0) {
                    result = 2;
                    co_return;
                }
                result = co_await w->close() == 0 ? 0 : 3;
            })
        .get();
    REQUIRE(result == 0);
    std::string all = gunzip_all(path);
    for (const auto& p : payloads) CHECK(all.find(p) != std::string::npos);
    CHECK(all.size() == [&] {
        std::size_t n = 0;
        for (const auto& p : payloads) n += p.size();
        return n;
    }());
    fs::remove(path);
}

TEST_CASE("PaddedStripedWriter - oversize chunk is rejected") {
    std::string path = tmp_path("test_padded_oversize.gz");
    std::remove(path.c_str());

    // Create a "payload" larger than one stripe (minus overhead).
    std::vector<std::uint8_t> huge(STRIPE, 0xAA);

    Runtime runtime(2);
    int result = -1;
    runtime
        .scope("padded_oversize",
               [&](CoroScope& s) -> CoroTask<void> {
                   auto w = make_padded_striped_writer(STRIPE);
                   if (co_await w->open(path, 1, true, &s) != 0) {
                       result = 1;
                       co_return;
                   }
                   auto rc = co_await w->write_chunk(0, as_bv(huge));
                   co_await w->close();
                   result = rc == 0 ? 99 : 0;  // expect failure
               })
        .get();
    CHECK(result == 0);
    std::remove(path.c_str());
}

TEST_CASE(
    "PaddedStripedWriter - file-level gzip integrity (decompress twice)") {
    std::string path = tmp_path("test_padded_integrity.json.gz");
    std::remove(path.c_str());

    // Bigger payload so the test meaningfully exercises padding members.
    std::string big;
    big.reserve(200 * 1024);
    for (int i = 0; i < 2000; ++i) {
        char line[128];
        std::snprintf(line, sizeof(line),
                      R"({"id":%d,"name":"evt_%d"} )"
                      "\n",
                      i, i);
        big += line;
    }
    auto mem = gzip_member(big);
    REQUIRE(mem.size() + 25 < STRIPE);  // fits in one stripe with padding

    Runtime runtime(2);
    int result = -1;
    runtime
        .scope("padded_integrity",
               [&](CoroScope& s) -> CoroTask<void> {
                   auto w = make_padded_striped_writer(STRIPE);
                   if (co_await w->open(path, 1, true, &s) != 0) {
                       result = 1;
                       co_return;
                   }
                   // Two chunks of the same payload; packer coalesces both
                   // into a single stripe since they fit together.
                   if (co_await w->write_chunk(0, as_bv(mem)) != 0) {
                       result = 2;
                       co_return;
                   }
                   if (co_await w->write_chunk(0, as_bv(mem)) != 0) {
                       result = 3;
                       co_return;
                   }
                   if (co_await w->close() != 0) {
                       result = 4;
                       co_return;
                   }
                   result = 0;
               })
        .get();
    CHECK(result == 0);

    // Two payloads totaling 2*mem.size() + 25 (pad overhead). Whether they
    // fit in a single stripe or spill into two depends on mem size relative
    // to STRIPE; we assert only that the file is a multiple of STRIPE.
    CHECK(fs::file_size(path) % STRIPE == 0);
    auto round = gunzip_all(path);
    CHECK(round == big + big);

    fs::remove(path);
}

namespace {

// A gzip member of exactly `total` bytes: `head`, then `fill` up to the size
// that stored deflate blocks of 65535 bytes leave, and an FEXTRA field of a
// few bytes for the rest. The decoded bytes go to `payload`.
std::vector<std::uint8_t> exact_member(const std::string& head, char fill,
                                       std::size_t total,
                                       std::string& payload) {
    std::size_t blocks = (total - 20 + 65539) / 65540;
    std::size_t size = total - 20 - 5 * blocks;
    while ((size + 65534) / 65535 > blocks) {
        ++blocks;
        size = total - 20 - 5 * blocks;
    }
    REQUIRE(size > head.size());
    payload = head + std::string(size - head.size() - 1, fill) + "\n";
    const std::size_t xlen = total - (20 + payload.size() + 5 * blocks);
    REQUIRE(xlen <= 65535);
    std::vector<std::uint8_t> m = {0x1f, 0x8b, 8, 0x04, 0, 0, 0, 0, 0, 0xff};
    m.push_back(static_cast<std::uint8_t>(xlen & 0xff));
    m.push_back(static_cast<std::uint8_t>(xlen >> 8));
    m.insert(m.end(), xlen, 0);
    for (std::size_t b = 0, at = 0; b < blocks; ++b) {
        const std::size_t len =
            std::min<std::size_t>(65535, payload.size() - at);
        m.push_back(b + 1 == blocks ? 1 : 0);
        m.push_back(static_cast<std::uint8_t>(len & 0xff));
        m.push_back(static_cast<std::uint8_t>(len >> 8));
        m.push_back(static_cast<std::uint8_t>(~len & 0xff));
        m.push_back(static_cast<std::uint8_t>((~len >> 8) & 0xff));
        m.insert(m.end(), payload.begin() + static_cast<std::ptrdiff_t>(at),
                 payload.begin() + static_cast<std::ptrdiff_t>(at + len));
        at += len;
    }
    const uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(payload.data()),
                            static_cast<uInt>(payload.size()));
    for (int i = 0; i < 4; ++i)
        m.push_back(static_cast<std::uint8_t>((crc >> (8 * i)) & 0xff));
    for (int i = 0; i < 4; ++i)
        m.push_back(
            static_cast<std::uint8_t>((payload.size() >> (8 * i)) & 0xff));
    REQUIRE(m.size() == total);
    return m;
}

}  // namespace

TEST_CASE("PaddedStripedWriter - every stripe ends in whole gzip members") {
    // Each chunk leaves 65560 + k bytes of its stripe, one full-size padding
    // member plus k: a k below the 25-byte minimum of a member must not stay
    // zero, or gzip stops reading there.
    constexpr std::size_t PAD_MAX = 25 + 65535;
    std::vector<std::string> payloads(31);
    std::vector<std::vector<std::uint8_t>> members;
    for (std::size_t k = 0; k < payloads.size(); ++k)
        members.push_back(exact_member("{\"k\":" + std::to_string(k) + "}\n",
                                       static_cast<char>('a' + k % 26),
                                       STRIPE - PAD_MAX - k, payloads[k]));
    std::string path = tmp_path("test_padded_whole_members.json.gz");
    std::remove(path.c_str());
    Runtime runtime(2);
    int result = -1;
    runtime
        .scope("padded_whole_members",
               [&](CoroScope& s) -> CoroTask<void> {
                   auto w = make_padded_striped_writer(STRIPE);
                   if (co_await w->open(path, 1, true, &s) != 0) {
                       result = 1;
                       co_return;
                   }
                   for (const auto& m : members) {
                       if (co_await w->write_chunk(0, as_bv(m)) != 0) {
                           result = 2;
                           co_return;
                       }
                   }
                   result = co_await w->close() == 0 ? 0 : 3;
               })
        .get();
    REQUIRE(result == 0);
    CHECK(fs::file_size(path) == (members.size() + 1) * STRIPE);
    std::string want;
    for (const auto& p : payloads) want += p;
    CHECK(gunzip_all(path) == want);
    fs::remove(path);
}
