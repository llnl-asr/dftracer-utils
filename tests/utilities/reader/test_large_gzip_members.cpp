#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/inflater.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <testing_runtime.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::utilities::reader::internal::ReaderInflater;
using dftu_utils_test::run_coro;
namespace gz = dftracer::utils::utilities::fileio::compress;

namespace {

// Random hex compresses about 2:1, so the member spans many 1 MiB reads.
std::string hex_lines(std::size_t bytes, std::uint32_t seed) {
    std::mt19937 rng(seed);
    static const char HEX[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes + 128);
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
        char tmpl[] = "/tmp/dftu_inflater_XXXXXX";
        const int fd = ::mkstemp(tmpl);
        REQUIRE(fd >= 0);
        path = tmpl;
        REQUIRE(::write(fd, bytes.data(), bytes.size()) ==
                static_cast<ssize_t>(bytes.size()));
        ::close(fd);
    }
    ~TempFile() { std::remove(path.c_str()); }
};

std::string read_all(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::string out;
    bool ok = false;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        ReaderInflater inf;
        off_t offset = 0;
        if (!co_await inf.initialize(fd, offset)) co_return;
        std::vector<unsigned char> buf(1 << 20);
        while (true) {
            std::size_t n = 0;
            if (!co_await inf.read(fd, offset, buf.data(), buf.size(), n))
                co_return;
            if (n == 0) break;
            out.append(reinterpret_cast<const char*>(buf.data()), n);
        }
        ok = true;
    });
    ::close(fd);
    REQUIRE(ok);
    return out;
}

std::string read_members(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    const auto size = static_cast<std::uint64_t>(::lseek(fd, 0, SEEK_END));
    std::string out;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        auto gen = gz::decode_gzip_members(fd, size);
        while (auto part = co_await gen.next()) out.append(*part);
    });
    ::close(fd);
    return out;
}

}  // namespace

TEST_SUITE("ReaderInflater") {
    TEST_CASE("one member larger than many reads decodes whole") {
        const auto text = hex_lines(12u << 20, 1);
        const auto comp = gzip(text);
        REQUIRE(comp.size() > (4u << 20));
        TempFile f(comp);
        CHECK(read_all(f.path) == text);
    }

    TEST_CASE("members of different sizes decode in order") {
        const auto a = hex_lines(6u << 20, 2);
        const auto b = hex_lines(1000, 3);
        const auto c = hex_lines(3u << 20, 4);
        std::vector<std::uint8_t> comp;
        for (const auto* t : {&a, &b, &c}) {
            const auto m = gzip(*t);
            comp.insert(comp.end(), m.begin(), m.end());
        }
        TempFile f(comp);
        CHECK(read_all(f.path) == a + b + c);
    }

    TEST_CASE("a large cut member keeps the complete lines") {
        const auto text = hex_lines(12u << 20, 5);
        auto comp = gzip(text);
        comp.resize(comp.size() * 7 / 10);
        TempFile f(comp);
        const auto got = read_all(f.path);
        REQUIRE(!got.empty());
        CHECK(got.back() == '\n');
        CHECK(got.size() > text.size() / 2);
        CHECK(got == text.substr(0, got.size()));
    }

    TEST_CASE("the member line reader decodes one large member") {
        const auto text = hex_lines(12u << 20, 6);
        TempFile f(gzip(text));
        CHECK(read_members(f.path) == text);
    }

    TEST_CASE("the index build counts one large member") {
        const auto text = hex_lines(12u << 20, 7);
        TempFile f(gzip(text));
        std::optional<index::gzip::GzipBuildArtifacts> arts;
        run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
            index::gzip::CheckpointIndexer::VisitorList none;
            arts = co_await index::gzip::build_gzip_index_artifacts(
                f.path, 32u << 20, none, &scope);
        });
        REQUIRE(arts.has_value());
        CHECK(arts->total_uc_size == text.size());
        CHECK(arts->total_lines == static_cast<std::uint64_t>(std::count(
                                       text.begin(), text.end(), '\n')));
        CHECK(arts->members.size() == 1);
    }
}
