// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <testing_utilities.h>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;

namespace {

// Multi-line, mildly compressible NDJSON-ish payload.
dstd::String make_lines(std::size_t n) {
    dstd::String s;
    s.reserve(n * 48);
    for (std::size_t i = 0; i < n; ++i) {
        s += "{\"id\":" + dstd::to_string(i) +
             ",\"name\":\"read\",\"cat\":\"POSIX\",\"dur\":42}\n";
    }
    return s;
}

dstd::String write_single_member_gz(const dstd::String& path,
                                    const dstd::String& payload) {
    fileio::compress::GzipMemberCompressor comp;
    auto member = comp.compress_member(payload.data(), payload.size());
    REQUIRE(member.has_value());
    dstd::String ofs;
    ofs.append(reinterpret_cast<const char*>(member->data()), member->size());
    dstd::fs::write_file(dstd::PathView(path), ofs).discard();
    return path;
}

// Decode every member; returns (concatenated bytes, per-member last byte).
dstd::Pair<dstd::String, dstd::Vector<char>> decode_members(
    const dstd::String& path) {
    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();
    struct stat st;
    REQUIRE(::fstat(fd, &st) == 0);
    const auto size = static_cast<std::uint64_t>(st.st_size);

    dstd::String out;
    dstd::Vector<char> last_bytes;
    [&]() -> dftracer::utils::coro::CoroTask<void> {
        auto gen = fileio::compress::decode_gzip_members(fd, size);
        while (auto chunk = co_await gen.next()) {
            last_bytes.push_back(chunk->empty() ? '\0' : chunk->back());
            out.append(chunk->data(), chunk->size());
        }
    }()
                 .get();
    return {out, last_bytes};
}

dstd::Pair<dstd::String, int> decode_all(const dstd::String& path) {
    auto [bytes, lasts] = decode_members(path);
    return {bytes, static_cast<int>(lasts.size())};
}

bool needs_rechunk(const dstd::String& path, std::size_t cap) {
    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();
    struct stat st;
    REQUIRE(::fstat(fd, &st) == 0);
    return fileio::compress::gzip_first_member(
               fd, static_cast<std::uint64_t>(st.st_size), cap)
        .get()
        .exceeds_cap;
}

}  // namespace

TEST_SUITE("gzip_rechunker") {
    TEST_CASE("needs_rechunk flags a member over the cap only") {
        auto dir = dftu_utils_test::make_unique_test_path("rechunk_probe");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String gz = dstd::String((dir / "one.pfw.gz").str());
        const auto payload = make_lines(2000);  // ~90 KB uncompressed
        write_single_member_gz(gz, payload);

        CHECK(needs_rechunk(gz, 1024));         // member > 1 KB -> yes
        CHECK_FALSE(needs_rechunk(gz, 10 * 1024 * 1024));  // under cap -> no

        dstd::fs::remove_all(dir).has_value();
    }

    TEST_CASE("first_member samples past a tiny first member") {
        auto dir = dftu_utils_test::make_unique_test_path("rechunk_sample");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String gz = dstd::String((dir / "wrapped.pfw.gz").str());
        const auto payload = make_lines(5000);
        {
            fileio::compress::GzipMemberCompressor comp;
            auto head = comp.compress_member("[\n", 2);
            auto body = comp.compress_member(payload.data(), payload.size());
            REQUIRE(head.has_value());
            REQUIRE(body.has_value());
            dstd::String ofs;
            ofs.append(reinterpret_cast<const char*>(head->data()),
                       head->size());
            ofs.append(reinterpret_cast<const char*>(body->data()),
                       body->size());
            dstd::fs::write_file(dstd::PathView(gz), ofs).discard();
        }

        auto fd_file = dstd::File::open(gz, {});
        REQUIRE(fd_file);
        const int fd = fd_file->native_handle();
        struct stat st;
        REQUIRE(::fstat(fd, &st) == 0);
        const auto size = static_cast<std::uint64_t>(st.st_size);
        constexpr std::size_t CAP = 10 * 1024 * 1024;

        // Without a sample only the "[" member is read.
        auto first = fileio::compress::gzip_first_member(fd, size, CAP).get();
        CHECK(first.uncompressed == 2);
        CHECK_FALSE(first.exceeds_cap);

        // With one, the next member counts too, and the cap still applies to
        // the first member only.
        auto sampled =
            fileio::compress::gzip_first_member(fd, size, 16, 1 << 20).get();
        CHECK(sampled.uncompressed == 2 + payload.size());
        CHECK(sampled.compressed == size);
        CHECK_FALSE(sampled.exceeds_cap);

        dstd::fs::remove_all(dir).discard();
    }

    TEST_CASE("rechunk preserves bytes and produces multiple members") {
        auto dir = dftu_utils_test::make_unique_test_path("rechunk_split");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String in = dstd::String((dir / "big.pfw.gz").str());
        const dstd::String out = dstd::String((dir / "out.pfw.gz").str());
        const auto payload = make_lines(5000);
        write_single_member_gz(in, payload);

        // One member in, small member size out.
        CHECK(decode_all(in).second == 1);
        fileio::compress::gzip_rechunk_to_members(in, out,
                                                  /*member_size=*/8 * 1024,
                                                  /*level=*/6)
            .get();

        auto [content, last_bytes] = decode_members(out);
        CHECK(content == payload);     // exact byte preservation
        CHECK(last_bytes.size() > 1);  // now multi-member
        // Every member but the last ends at a line end.
        for (std::size_t i = 0; i + 1 < last_bytes.size(); ++i) {
            CHECK(last_bytes[i] == '\n');
        }

        dstd::fs::remove_all(dir).discard();
    }

    TEST_CASE("rechunk handles a payload with no trailing newline") {
        auto dir = dftu_utils_test::make_unique_test_path("rechunk_nonl");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String in = dstd::String((dir / "in.pfw.gz").str());
        const dstd::String out = dstd::String((dir / "out.pfw.gz").str());
        dstd::String payload = make_lines(1000);
        payload += "{\"id\":99999,\"trailing\":true}";  // no final '\n'
        write_single_member_gz(in, payload);

        fileio::compress::gzip_rechunk_to_members(in, out, 4 * 1024, 6).get();
        CHECK(decode_all(out).first == payload);

        dstd::fs::remove_all(dir).discard();
    }
}
