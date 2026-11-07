// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>

using namespace dftracer::utils;

namespace {

constexpr std::uint64_t CKPT = 1u << 20;

dstd::String hex_lines(std::size_t bytes, std::uint32_t seed) {
    std::mt19937 rng(seed);
    static const char HEX[] = "0123456789abcdef";
    dstd::String s;
    while (s.size() < bytes) {
        s += "{\"id\":\"";
        for (int i = 0; i < 64; ++i) s += HEX[rng() & 15];
        s += "\"}\n";
    }
    return s;
}

dstd::Vector<std::uint8_t> gzip(const dstd::String& text) {
    utilities::fileio::compress::GzipMemberCompressor c;
    dstd::Vector<std::uint8_t> out(c.bound(text.size()));
    auto n = c.compress(text.data(), text.size(), out.data(), out.size());
    REQUIRE(n.has_value());
    out.resize(*n);
    return out;
}

struct TempFile {
    dstd::String path;
    explicit TempFile(const dstd::Vector<std::uint8_t>& bytes) {
        char tmpl[] = "/tmp/dftu_index_pass_XXXXXX";
        const int fd = ::mkstemp(tmpl);
        REQUIRE(fd >= 0);
        path = tmpl;
        REQUIRE(::write(fd, bytes.data(), bytes.size()) ==
                static_cast<ssize_t>(bytes.size()));
        ::close(fd);
    }
    ~TempFile() { std::remove(path.c_str()); }
};

class Recorder : public index::build::IndexVisitor {
   public:
    dstd::String bytes;
    dstd::Vector<std::size_t> checkpoints;
    void begin(std::size_t) override {}
    coro::CoroTask<void> on_checkpoint(std::size_t idx) override {
        checkpoints.push_back(idx);
        co_return;
    }
    coro::CoroTask<void> on_chunk(const char* data, std::size_t len,
                                  std::size_t) override {
        bytes.append(data, len);
        co_return;
    }
};

index::gzip::GzipBuildArtifacts build(const dstd::String& path, Recorder* rec) {
    dstd::Optional<index::gzip::GzipBuildArtifacts> arts;
    dftu_utils_test::run_coro([&](coro::Scope& scope) -> coro::CoroTask<void> {
        index::gzip::CheckpointIndexer::VisitorList visitors;
        if (rec) visitors.emplace_back(*rec);
        arts = co_await index::gzip::build_gzip_index_artifacts(
            path, CKPT, visitors, &scope);
    });
    REQUIRE(arts.has_value());
    return *arts;
}

void check_same(const index::gzip::GzipBuildArtifacts& a,
                const index::gzip::GzipBuildArtifacts& b) {
    CHECK(a.total_lines == b.total_lines);
    CHECK(a.total_uc_size == b.total_uc_size);
    CHECK(a.truncated == b.truncated);
    CHECK(a.restart_windows == b.restart_windows);
    REQUIRE(a.members.size() == b.members.size());
    for (std::size_t i = 0; i < a.members.size(); ++i) {
        CAPTURE(i);
        const index::gzip::GzipMemberRecord& x = a.members[i];
        const index::gzip::GzipMemberRecord& y = b.members[i];
        CHECK(x.c_offset == y.c_offset);
        CHECK(x.c_size == y.c_size);
        CHECK(x.uc_offset == y.uc_offset);
        CHECK(x.uc_size == y.uc_size);
        CHECK(x.first_line_num == y.first_line_num);
        CHECK(x.last_line_num == y.last_line_num);
        CHECK(x.kind == y.kind);
        CHECK(x.bits == y.bits);
    }
}

void check_file(const dstd::Vector<std::uint8_t>& comp,
                const dstd::String& expected) {
    TempFile f(comp);
    const auto bare = build(f.path, nullptr);
    Recorder rec;
    const auto seen = build(f.path, &rec);
    check_same(bare, seen);
    CHECK(rec.bytes == expected);
    CHECK(rec.checkpoints.size() == seen.members.size());
}

}  // namespace

TEST_SUITE("index pass without visitors") {
    TEST_CASE("many members give the table a visitor sees") {
        dstd::String text;
        dstd::Vector<std::uint8_t> comp;
        for (std::uint32_t i = 0; i < 40; ++i) {
            const auto part = hex_lines(200u << 10, i);
            const auto m = gzip(part);
            comp.insert(comp.end(), m.begin(), m.end());
            text += part;
        }
        check_file(comp, text);
    }

    TEST_CASE("one large member gives the same pieces") {
        const auto text = hex_lines(6u << 20, 99);
        check_file(gzip(text), text);
    }

    TEST_CASE("zero bytes between members are skipped") {
        dstd::String text;
        dstd::Vector<std::uint8_t> comp;
        for (std::uint32_t i = 0; i < 40; ++i) {
            const auto part = hex_lines(200u << 10, 300 + i);
            const auto m = gzip(part);
            comp.insert(comp.end(), m.begin(), m.end());
            text += part;
            // An older padded striped writer left up to 24 zero bytes
            // before a stripe boundary.
            if (i % 7 == 3) comp.insert(comp.end(), 11 + i % 13, 0);
        }
        check_file(comp, text);
        TempFile f(comp);
        const auto arts = build(f.path, nullptr);
        REQUIRE(arts.members.size() == 40);
        for (std::size_t i = 0; i + 1 < arts.members.size(); ++i) {
            CAPTURE(i);
            const auto& m = arts.members[i];
            CHECK(m.c_offset + m.c_size == arts.members[i + 1].c_offset);
            CHECK(comp[arts.members[i + 1].c_offset] == 0x1F);
        }
    }

    TEST_CASE("a header inside a stored member does not split it") {
        // A stored (level 0) member copies its text verbatim, so a gzip
        // header in the text is a member candidate that starts a range.
        static const char FAKE[] = "\x10\0\0\0\x1f\x8b\x08\0\0\0\0\0\0\x03";
        const dstd::String head = hex_lines(CKPT + (100u << 10), 1);
        const dstd::String fake =
            dstd::String("x") + dstd::String(FAKE, sizeof(FAKE) - 1) + "\n";
        const dstd::String tail = hex_lines(100u << 10, 2);
        dstd::String text = hex_lines(CKPT + (200u << 10), 0);
        dstd::Vector<std::uint8_t> comp = gzip(text);
        utilities::fileio::compress::GzipMemberCompressor stored(0);
        const dstd::String mid = head + fake + tail;
        dstd::Vector<std::uint8_t> m(stored.bound(mid.size()));
        auto n = stored.compress(mid.data(), mid.size(), m.data(), m.size());
        REQUIRE(n.has_value());
        m.resize(*n);
        comp.insert(comp.end(), m.begin(), m.end());
        text += mid;
        for (std::uint32_t i = 0; i < 2; ++i) {
            const auto part = hex_lines(300u << 10, 10 + i);
            const auto g = gzip(part);
            comp.insert(comp.end(), g.begin(), g.end());
            text += part;
        }
        check_file(comp, text);
        const auto* header = reinterpret_cast<const std::uint8_t*>(FAKE) + 4;
        const std::uint64_t fake_at = static_cast<std::uint64_t>(
            std::search(comp.begin() + 1, comp.end(), header,
                        header + sizeof(FAKE) - 5) -
            comp.begin());
        REQUIRE(fake_at < comp.size());
        TempFile f(comp);
        for (const auto& rec : build(f.path, nullptr).members)
            CHECK(rec.c_offset != fake_at);
    }

    TEST_CASE("a visitor that throws ends the pass with its error") {
        class Thrower : public Recorder {
           public:
            coro::CoroTask<void> on_chunk(const char*, std::size_t,
                                          std::size_t) override {
                if (++chunks == 5) throw std::runtime_error("bad chunk");
                co_return;
            }
            int chunks = 0;
        };
        dstd::Vector<std::uint8_t> comp;
        for (std::uint32_t i = 0; i < 60; ++i) {
            const auto m = gzip(hex_lines(200u << 10, 500 + i));
            comp.insert(comp.end(), m.begin(), m.end());
        }
        TempFile f(comp);
        Thrower thrower;
        bool threw = false;
        dftu_utils_test::run_coro(
            [&](coro::Scope& scope) -> coro::CoroTask<void> {
                index::gzip::CheckpointIndexer::VisitorList visitors;
                visitors.emplace_back(thrower);
                try {
                    co_await index::gzip::build_gzip_index_artifacts(
                        f.path, CKPT, visitors, &scope);
                } catch (const std::runtime_error& e) {
                    threw = dstd::String(e.what()) == "bad chunk";
                }
            });
        CHECK(threw);
    }

    TEST_CASE("a cut file gives the same table") {
        dstd::String text;
        dstd::Vector<std::uint8_t> comp;
        for (std::uint32_t i = 0; i < 8; ++i) {
            const auto part = hex_lines(300u << 10, 200 + i);
            const auto m = gzip(part);
            comp.insert(comp.end(), m.begin(), m.end());
            text += part;
        }
        comp.resize(comp.size() - 1000);
        TempFile f(comp);
        const auto bare = build(f.path, nullptr);
        Recorder rec;
        const auto seen = build(f.path, &rec);
        check_same(bare, seen);
        CHECK(bare.truncated);
        CHECK(rec.bytes == text.substr(0, rec.bytes.size()));
    }
}
