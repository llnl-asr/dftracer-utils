#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/index_visitor.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <testing_runtime.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::index::gzip::GzipBuildArtifacts;
using dftracer::utils::index::gzip::GzipMemberRecord;
using dftu_utils_test::run_coro;
namespace gz = dftracer::utils::utilities::fileio::compress;

namespace {

constexpr std::uint64_t CKPT = 1u << 20;

std::string hex_lines(std::size_t bytes, std::uint32_t seed) {
    std::mt19937 rng(seed);
    static const char HEX[] = "0123456789abcdef";
    std::string s;
    while (s.size() < bytes) {
        s += "{\"id\":\"";
        for (int i = 0; i < 64; ++i) s += HEX[rng() & 15];
        s += "\"}\n";
    }
    return s;
}

std::vector<std::uint8_t> gzip(const std::string& text) {
    gz::GzipMemberCompressor c;
    std::vector<std::uint8_t> out(c.bound(text.size()));
    auto n = c.compress(text.data(), text.size(), out.data(), out.size());
    REQUIRE(n.has_value());
    out.resize(*n);
    return out;
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
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
    std::string bytes;
    std::vector<std::size_t> checkpoints;
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
    void finalize(index::store::IndexDatabaseWriterContext&, int) override {}
};

GzipBuildArtifacts build(const std::string& path, Recorder* rec) {
    std::optional<GzipBuildArtifacts> arts;
    run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
        index::gzip::CheckpointIndexer::VisitorList visitors;
        if (rec) visitors.emplace_back(*rec);
        arts = co_await index::gzip::build_gzip_index_artifacts(
            path, CKPT, visitors, &scope);
    });
    REQUIRE(arts.has_value());
    return *arts;
}

void check_same(const GzipBuildArtifacts& a, const GzipBuildArtifacts& b) {
    CHECK(a.total_lines == b.total_lines);
    CHECK(a.total_uc_size == b.total_uc_size);
    CHECK(a.truncated == b.truncated);
    CHECK(a.restart_windows == b.restart_windows);
    REQUIRE(a.members.size() == b.members.size());
    for (std::size_t i = 0; i < a.members.size(); ++i) {
        CAPTURE(i);
        const GzipMemberRecord& x = a.members[i];
        const GzipMemberRecord& y = b.members[i];
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

void check_file(const std::vector<std::uint8_t>& comp,
                const std::string& expected) {
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
        std::string text;
        std::vector<std::uint8_t> comp;
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

    TEST_CASE("a cut file gives the same table") {
        std::string text;
        std::vector<std::uint8_t> comp;
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
