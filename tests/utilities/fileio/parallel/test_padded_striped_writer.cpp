// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/stop_token.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/time.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <task_result.h>
#include <testing_utilities.h>
#include <zlib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::coro;
using namespace dftracer::utils::fileio::parallel;

namespace {

constexpr std::size_t STRIPE = 1 * 1024 * 1024;  // 1 MB stripe for the test

dstd::String tmp_path(const char* name) {
    return dstd::String(dftu_utils_test::make_unique_test_path(name).str());
}

// Compress `data` into a standalone gzip member with zlib.
dstd::Vector<std::uint8_t> gzip_member(const dstd::String& data) {
    uLongf bound = compressBound(data.size()) + 64;
    dstd::Vector<std::uint8_t> out(bound);
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

dstd::ByteView as_bv(const dstd::Vector<std::uint8_t>& v) {
    return dstd::ByteView(reinterpret_cast<const std::byte*>(v.data()),
                          v.size());
}

// Decompress the whole file via gzread (spans concatenated members natively).
dstd::String gunzip_all(const dstd::String& path) {
    gzFile g = gzopen(path.c_str(), "rb");
    REQUIRE(g != nullptr);
    dstd::String out;
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
    dstd::String path = tmp_path("test_padded_basic.json.gz");
    std::remove(path.c_str());

    dstd::String hdr_payload = "HDR\n";
    dstd::String w0_payload = R"({"ev":0})"
                              "\n";
    dstd::String w1_payload = R"({"ev":1})"
                              "\n";
    dstd::String w2_payload = R"({"ev":2})"
                              "\n";
    dstd::String ftr_payload = "]\n";

    auto hdr_mem = gzip_member(hdr_payload);
    auto w0_mem = gzip_member(w0_payload);
    auto w1_mem = gzip_member(w1_payload);
    auto w2_mem = gzip_member(w2_payload);
    auto ftr_mem = gzip_member(ftr_payload);

    Runtime runtime(2);
    int result = -1;
    dftu_utils_test::ok(
        runtime
            .submit(coro::with_scope([&](coro::Scope& s) -> CoroTask<void> {
                        auto w = make_padded_striped_writer(STRIPE);
                        if (co_await w->open(dstd::String(path), 3, true, &s) !=
                            0) {
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
                    }),
                    "padded_basic")
            .get());
    CHECK(result == 0);

    // With coalescing, the three tiny worker members pack into a single
    // stripe. File = [header stripe][one coalesced stripe][footer bytes].
    CHECK(dstd::fs::file_size(path).value() == 2 * STRIPE + ftr_mem.size());

    auto decompressed = gunzip_all(path);
    // Decompressed content = hdr + w0 + w1 + w2 + footer, with worker ordering
    // determined by the atomic stripe allocation (sequential here since we
    // serialized calls).
    CHECK(decompressed ==
          hdr_payload + w0_payload + w1_payload + w2_payload + ftr_payload);

    dstd::fs::remove(path).discard();
}

// GzipLineWriter passes its own scope to open(), runs its compress jobs in it
// and joins that scope before it calls close(). A packer started in that scope
// could only end in close(), so the join never returned.
TEST_CASE("PaddedStripedWriter - the caller's scope joins before close") {
    dstd::String path = tmp_path("test_padded_join.json.gz");
    std::remove(path.c_str());
    constexpr std::size_t WORKERS = 8;
    dstd::Vector<dstd::String> payloads;
    dstd::Vector<dstd::Vector<std::uint8_t>> members;
    for (std::size_t i = 0; i < WORKERS; ++i) {
        payloads.push_back(dstd::fmt<"{\"w\":{}}\n">(i));
        members.push_back(gzip_member(payloads.back()));
    }
    Runtime runtime(4);
    int result = -1;
    dftu_utils_test::ok(
        runtime
            .submit(coro::with_scope([&](coro::Scope&) -> CoroTask<void> {
                        auto w = make_padded_striped_writer(STRIPE);
                        auto jobs = dstd::make_unique<coro::Scope>();
                        if (co_await w->open(dstd::String(path), WORKERS, true,
                                             jobs.get()) != 0) {
                            result = 1;
                            co_return;
                        }
                        std::atomic<int> failures{0};
                        for (std::size_t i = 0; i < WORKERS; ++i)
                            jobs->spawn([&, i]() -> CoroTask<void> {
                                if (co_await w->write_chunk(
                                        i, as_bv(members[i])) != 0)
                                    failures.fetch_add(1);
                            });
                        co_await jobs->join();
                        if (failures.load() != 0) {
                            result = 2;
                            co_return;
                        }
                        result = co_await w->close() == 0 ? 0 : 3;
                    }),
                    "padded_join")
            .get());
    REQUIRE(result == 0);
    dstd::String all = gunzip_all(path);
    for (const auto& p : payloads) CHECK(all.find(p) != dstd::String::npos);
    CHECK(all.size() == [&] {
        std::size_t n = 0;
        for (const auto& p : payloads) n += p.size();
        return n;
    }());
    dstd::fs::remove(path).discard();
}

TEST_CASE(
    "PaddedStripedWriter - a member larger than a stripe takes whole stripes") {
    dstd::String path = tmp_path("test_padded_oversize.gz");
    std::remove(path.c_str());

    // Text that does not compress, so its member spans more than two stripes,
    // as a member copied whole from an input file can.
    dstd::String big_payload;
    std::uint64_t x = 88172645463325252ULL;
    while (big_payload.size() < 2 * STRIPE + STRIPE / 2) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        big_payload.push_back(static_cast<char>(' ' + x % 94));
    }
    const dstd::String small0 = R"({"ev":0})"
                                "\n";
    const dstd::String small1 = R"({"ev":1})"
                                "\n";
    const dstd::String ftr_payload = "]\n";
    const auto big = gzip_member(big_payload);
    REQUIRE(big.size() > 2 * STRIPE);
    const auto m0 = gzip_member(small0);
    const auto m1 = gzip_member(small1);
    const auto ftr = gzip_member(ftr_payload);

    Runtime runtime(2);
    int result = -1;
    dftu_utils_test::ok(
        runtime
            .submit(coro::with_scope([&](coro::Scope& s) -> CoroTask<void> {
                        auto w = make_padded_striped_writer(STRIPE);
                        if (co_await w->open(dstd::String(path), 1, true, &s) !=
                            0) {
                            result = 1;
                            co_return;
                        }
                        if (co_await w->write_chunk(0, as_bv(m0)) != 0) {
                            result = 2;
                            co_return;
                        }
                        if (co_await w->write_chunk(0, as_bv(big)) != 0) {
                            result = 3;
                            co_return;
                        }
                        if (co_await w->write_chunk(0, as_bv(m1)) != 0) {
                            result = 4;
                            co_return;
                        }
                        if (co_await w->write_footer(as_bv(ftr)) != 0) {
                            result = 5;
                            co_return;
                        }
                        result = co_await w->close() == 0 ? 0 : 6;
                    }),
                    "padded_oversize")
            .get());
    REQUIRE(result == 0);

    // [header][m0][big: three stripes][m1][footer].
    CHECK(dstd::fs::file_size(path).value() == 6 * STRIPE + ftr.size());
    CHECK(gunzip_all(path) == small0 + big_payload + small1 + ftr_payload);
    std::remove(path.c_str());
}

TEST_CASE(
    "PaddedStripedWriter - file-level gzip integrity (decompress twice)") {
    dstd::String path = tmp_path("test_padded_integrity.json.gz");
    std::remove(path.c_str());

    // Bigger payload so the test meaningfully exercises padding members.
    dstd::String big;
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
    dftu_utils_test::ok(
        runtime
            .submit(coro::with_scope([&](coro::Scope& s) -> CoroTask<void> {
                        auto w = make_padded_striped_writer(STRIPE);
                        if (co_await w->open(dstd::String(path), 1, true, &s) !=
                            0) {
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
                    }),
                    "padded_integrity")
            .get());
    CHECK(result == 0);

    // Two payloads totaling 2*mem.size() + 25 (pad overhead). Whether they
    // fit in a single stripe or spill into two depends on mem size relative
    // to STRIPE; we assert only that the file is a multiple of STRIPE.
    CHECK(dstd::fs::file_size(path).value() % STRIPE == 0);
    auto round = gunzip_all(path);
    CHECK(round == big + big);

    dstd::fs::remove(path).discard();
}

namespace {

// A gzip member of exactly `total` bytes: `head`, then `fill` up to the size
// that stored deflate blocks of 65535 bytes leave, and an FEXTRA field of a
// few bytes for the rest. The decoded bytes go to `payload`.
dstd::Vector<std::uint8_t> exact_member(const dstd::String& head, char fill,
                                        std::size_t total,
                                        dstd::String& payload) {
    std::size_t blocks = (total - 20 + 65539) / 65540;
    std::size_t size = total - 20 - 5 * blocks;
    while ((size + 65534) / 65535 > blocks) {
        ++blocks;
        size = total - 20 - 5 * blocks;
    }
    REQUIRE(size > head.size());
    payload =
        dstd::fmt<"{}{}\n">(head, dstd::String(size - head.size() - 1, fill));
    const std::size_t xlen = total - (20 + payload.size() + 5 * blocks);
    REQUIRE(xlen <= 65535);
    dstd::Vector<std::uint8_t> m = {0x1f, 0x8b, 8, 0x04, 0, 0, 0, 0, 0, 0xff};
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

TEST_CASE("PaddedStripedWriter - a drop without close on an executor joins") {
    dstd::String path = tmp_path("test_padded_dropped.json.gz");
    std::remove(path.c_str());
    const dstd::String payload = "{\"dropped\":1}\n";
    const auto member = gzip_member(payload);
    std::atomic<bool> on_executor{false};
    int result = -1;
    Runtime runtime(4);
    dftu_utils_test::ok(
        runtime
            .submit(coro::with_scope([&](coro::Scope& s) -> CoroTask<void> {
                        auto w = make_padded_striped_writer(STRIPE);
                        if (co_await w->open(dstd::String(path), 2, true, &s) !=
                            0) {
                            result = 1;
                            co_return;
                        }
                        result = co_await w->write_chunk(0, as_bv(member)) == 0
                                     ? 0
                                     : 2;
                        on_executor = coro::Executor::current() != nullptr;
                        w.reset();
                    }),
                    "padded_dropped")
            .get());
    REQUIRE(result == 0);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (gunzip_all(path).find(payload) == dstd::String::npos &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(gunzip_all(path).find(payload) != dstd::String::npos);
    CHECK(on_executor.load());
    runtime.shutdown();
    dstd::fs::remove(path).discard();
}

TEST_CASE("PaddedStripedWriter - every stripe ends in whole gzip members") {
    // Each chunk leaves 65560 + k bytes of its stripe, one full-size padding
    // member plus k: a k below the 25-byte minimum of a member must not stay
    // zero, or gzip stops reading there.
    constexpr std::size_t PAD_MAX = 25 + 65535;
    dstd::Vector<dstd::String> payloads(31);
    dstd::Vector<dstd::Vector<std::uint8_t>> members;
    for (std::size_t k = 0; k < payloads.size(); ++k)
        members.push_back(exact_member(dstd::fmt<"{\"k\":{}}\n">(k),
                                       static_cast<char>('a' + k % 26),
                                       STRIPE - PAD_MAX - k, payloads[k]));
    dstd::String path = tmp_path("test_padded_whole_members.json.gz");
    std::remove(path.c_str());
    Runtime runtime(2);
    int result = -1;
    dftu_utils_test::ok(
        runtime
            .submit(coro::with_scope([&](coro::Scope& s) -> CoroTask<void> {
                        auto w = make_padded_striped_writer(STRIPE);
                        if (co_await w->open(dstd::String(path), 1, true, &s) !=
                            0) {
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
                    }),
                    "padded_whole_members")
            .get());
    REQUIRE(result == 0);
    CHECK(dstd::fs::file_size(path).value() == (members.size() + 1) * STRIPE);
    dstd::String want;
    for (const auto& p : payloads) want += p;
    CHECK(gunzip_all(path) == want);
    dstd::fs::remove(path).discard();
}

TEST_CASE(
    "PaddedStripedWriter - a task stopped after a write drops its writer "
    "cleanly and the chunk lands") {
    dstd::String path = tmp_path("test_padded_stopped.json.gz");
    std::remove(path.c_str());
    const dstd::String payload = "{\"stopped\":1}\n";
    const auto member = gzip_member(payload);
    bool stopped = false;
    Runtime runtime(4);
    coro::StopSource source;
    dftu_utils_test::ok(runtime.block_on([&]() -> CoroTask<void> {
        coro::Scope scope;
        auto writing = [&]() -> CoroTask<void> {
            auto w = make_padded_striped_writer(STRIPE);
            auto jobs = dstd::make_unique<coro::Scope>();
            REQUIRE(co_await w->open(dstd::String(path), 2, true, jobs.get()) ==
                    0);
            REQUIRE(co_await w->write_chunk(0, as_bv(member)) == 0);
            co_await coro::sleep(std::chrono::seconds(30));
            co_await jobs->join();
        };
        auto waiting = scope.spawn_future(
            coro::with_stop_token(writing(), source.token()));
        co_await coro::sleep(std::chrono::milliseconds(100));
        source.request_stop();
        auto settled = co_await coro::settle_in_place(waiting);
        stopped = settled.stopped;
        CHECK(!settled.exception);
        co_await scope.join();
    }()));
    CHECK(stopped);
    // The dropped writer drains its packer in the background and the chunk
    // reaches the file.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (gunzip_all(path).find(payload) == dstd::String::npos &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(gunzip_all(path).find(payload) != dstd::String::npos);
    runtime.shutdown();
    dstd::fs::remove(path).discard();
}
