// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/gzip_reader.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;

namespace {

constexpr std::uint64_t CKPT = 1u << 20;

dstd::String hex_lines(std::size_t bytes, std::uint32_t seed) {
    std::mt19937 rng(seed);
    static const char HEX[] = "0123456789abcdef";
    dstd::String s;
    s.reserve(bytes + 128);
    while (s.size() < bytes) {
        s += "{\"id\":\"";
        for (int i = 0; i < 64; ++i) s += HEX[rng() & 15];
        s += "\"}\n";
    }
    return s;
}

dstd::Vector<std::uint8_t> gzip(const dstd::String& text) {
    fileio::compress::GzipMemberCompressor c;
    dstd::Vector<std::uint8_t> out(c.bound(text.size()));
    auto n = c.compress(text.data(), text.size(), out.data(), out.size());
    REQUIRE(n.has_value());
    out.resize(*n);
    return out;
}

struct Fixture {
    dstd::String dir;
    dstd::String gz_path;
    dstd::String index_path;
    dstd::SharedPtr<index::gzip::GzipIndexer> indexer;

    explicit Fixture(const dstd::Vector<std::uint8_t>& bytes) {
        char tmpl[] = "/tmp/dftu_restart_XXXXXX";
        REQUIRE(::mkdtemp(tmpl) != nullptr);
        dir = tmpl;
        gz_path = dir + "/t.gz";
        index_path = dir + "/idx";
        std::FILE* f = std::fopen(gz_path.c_str(), "wb");
        REQUIRE(f != nullptr);
        REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
        std::fclose(f);
        indexer = dstd::make_shared<index::gzip::GzipIndexer>(
            dstd::String(dstd::StringView(gz_path)),
            dstd::String(dstd::StringView(index_path)), CKPT, true);
        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            co_await indexer->build_async();
        });
    }
    ~Fixture() { (void)dstd::fs::remove_all(dir); }

    dstd::String read_from(std::size_t start) {
        reader::internal::GzipReader reader(indexer);
        auto stream = reader.stream(
            reader::internal::StreamConfig()
                .stream_type(reader::internal::StreamType::Bytes)
                .range_type(reader::internal::RangeType::ByteRange)
                .from(start)
                .to(reader.max_bytes()));
        dstd::String out;
        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            dstd::Vector<char> buf(1u << 20);
            while (true) {
                const auto n =
                    co_await stream->read_async(buf.data(), buf.size());
                if (n == 0) break;
                out.append(buf.data(), n);
            }
        });
        return out;
    }

    dstd::String read_lines(std::size_t first, std::size_t last) {
        reader::internal::GzipReader reader(indexer);
        dstd::String out;
        dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            out = co_await reader.read_lines_async(first, last);
        });
        return out;
    }
};

void check_pieces_cover(
    const dstd::Vector<index::gzip::GzipMemberRecord>& pieces,
    std::size_t total) {
    std::uint64_t uc = 0;
    for (const auto& p : pieces) {
        CHECK(p.uc_offset == uc);
        uc += p.uc_size;
    }
    CHECK(uc == total);
}

void check_reads_from_every_piece(Fixture& fx, const dstd::String& text) {
    for (const auto& p : fx.indexer->members()) {
        CAPTURE(p.member_idx);
        CHECK(fx.read_from(p.uc_offset) == text.substr(p.uc_offset));
    }
}

}  // namespace

TEST_SUITE("gzip_restart_points") {
    TEST_CASE("a one-member file splits into a head and restart pieces") {
        const auto text = hex_lines(12u << 20, 1);
        Fixture fx(gzip(text));
        const auto pieces = fx.indexer->members();
        REQUIRE(pieces.size() >= 6);
        CHECK(pieces[0].kind == index::gzip::GzipRecordKind::Head);
        CHECK(pieces[0].c_offset == 0);
        for (std::size_t i = 1; i < pieces.size(); ++i) {
            CHECK(pieces[i].kind == index::gzip::GzipRecordKind::Restart);
            CHECK(pieces[i].bits <= 7);
            CHECK(pieces[i].c_offset ==
                  pieces[i - 1].c_offset + pieces[i - 1].c_size);
            CHECK(fx.indexer->restart_window(pieces[i].member_idx).size() ==
                  32768);
        }
        check_pieces_cover(pieces, text.size());
        check_reads_from_every_piece(fx, text);
    }

    TEST_CASE("a read from a piece runs into the members after it") {
        const auto text = hex_lines(12u << 20, 2);
        const auto tail = hex_lines(1000, 3) + hex_lines(2000, 4);
        auto comp = gzip(text);
        for (const auto& t : {hex_lines(1000, 3), hex_lines(2000, 4)}) {
            const auto m = gzip(t);
            comp.insert(comp.end(), m.begin(), m.end());
        }
        Fixture fx(comp);
        const auto pieces = fx.indexer->members();
        CHECK(pieces[0].kind == index::gzip::GzipRecordKind::Head);
        CHECK(pieces[pieces.size() - 1].kind ==
              index::gzip::GzipRecordKind::Member);
        check_pieces_cover(pieces, text.size() + tail.size());
        check_reads_from_every_piece(fx, text + tail);
    }

    TEST_CASE("small members give one member record each") {
        dstd::Vector<std::uint8_t> comp;
        dstd::Vector<std::size_t> sizes;
        dstd::String all;
        for (std::uint32_t i = 0; i < 5; ++i) {
            const auto t = hex_lines(i % 2 ? 400u << 10 : 3000, 10 + i);
            const auto m = gzip(t);
            comp.insert(comp.end(), m.begin(), m.end());
            sizes.push_back(t.size());
            all += t;
        }
        Fixture fx(comp);
        const auto members = fx.indexer->members();
        REQUIRE(members.size() == sizes.size());
        for (std::size_t i = 0; i < members.size(); ++i) {
            CHECK(members[i].kind == index::gzip::GzipRecordKind::Member);
            CHECK(members[i].bits == 0);
            CHECK(members[i].uc_size == sizes[i]);
        }
        check_reads_from_every_piece(fx, all);
    }

    TEST_CASE("a cut one-member file keeps its complete lines") {
        const auto text = hex_lines(12u << 20, 5);
        auto comp = gzip(text);
        comp.resize(comp.size() * 7 / 10);
        Fixture fx(comp);
        const auto pieces = fx.indexer->members();
        REQUIRE(pieces.size() >= 3);
        const std::size_t total = fx.indexer->max_bytes();
        REQUIRE(total > text.size() / 2);
        CHECK(text[total - 1] == '\n');
        check_pieces_cover(pieces, total);
        check_reads_from_every_piece(fx, text.substr(0, total));
    }

    TEST_CASE("a cut inside a line longer than a piece keeps whole lines") {
        dstd::String long_line(3u << 20, 'a');
        std::mt19937 rng(9);
        for (auto& c : long_line) c = static_cast<char>('a' + rng() % 26);
        const auto head = hex_lines(4u << 20, 8);
        const auto text = head + long_line + "\n" + hex_lines(1u << 20, 10);
        auto comp = gzip(text);
        // Cut inside the long line, past at least one restart point in it.
        comp.resize(comp.size() * (head.size() + long_line.size() * 3 / 4) /
                    text.size());
        Fixture fx(comp);
        const auto pieces = fx.indexer->members();
        const std::size_t total = fx.indexer->max_bytes();
        CHECK(total == head.size());
        check_pieces_cover(pieces, total);
        check_reads_from_every_piece(fx, head);
    }

    TEST_CASE("a line range from the split file equals the lines") {
        const auto text = hex_lines(12u << 20, 6);
        Fixture fx(gzip(text));
        dstd::Vector<std::size_t> starts;
        std::size_t pos = 0;
        starts.push_back(0);
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n') starts.push_back(i + 1);
        }
        const std::size_t lines = starts.size() - 1;
        for (const std::size_t first :
             {std::size_t{1}, lines / 2, lines - 10}) {
            const std::size_t last = first + 9;
            CAPTURE(first);
            CHECK(fx.read_lines(first, last) ==
                  text.substr(starts[first - 1],
                              starts[last] - starts[first - 1]));
        }
        (void)pos;
    }
}
