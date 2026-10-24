#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <testing_utilities.h>

#include <fstream>
#include <string>

namespace gzc = dftracer::utils::utilities::fileio::compress;
using dftracer::utils::ScopedFd;

namespace {

// Multi-line, mildly compressible NDJSON-ish payload.
std::string make_lines(std::size_t n) {
    std::string s;
    s.reserve(n * 48);
    for (std::size_t i = 0; i < n; ++i) {
        s += "{\"id\":" + std::to_string(i) +
             ",\"name\":\"read\",\"cat\":\"POSIX\",\"dur\":42}\n";
    }
    return s;
}

std::string write_single_member_gz(const std::string& path,
                                   const std::string& payload) {
    gzc::GzipMemberCompressor comp;
    auto member = comp.compress_member(payload.data(), payload.size());
    REQUIRE(member.has_value());
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(member->data()),
              static_cast<std::streamsize>(member->size()));
    ofs.close();
    return path;
}

// Decode every member; returns (concatenated bytes, per-member last byte).
std::pair<std::string, std::vector<char>> decode_members(
    const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    ScopedFd sfd(fd);
    struct stat st;
    REQUIRE(::fstat(sfd.get(), &st) == 0);
    const auto size = static_cast<std::uint64_t>(st.st_size);

    std::string out;
    std::vector<char> last_bytes;
    [&]() -> dftracer::utils::coro::CoroTask<void> {
        auto gen = gzc::decode_gzip_members(sfd.get(), size);
        while (auto chunk = co_await gen.next()) {
            last_bytes.push_back(chunk->empty() ? '\0' : chunk->back());
            out.append(chunk->data(), chunk->size());
        }
    }()
                 .get();
    return {out, last_bytes};
}

std::pair<std::string, int> decode_all(const std::string& path) {
    auto [bytes, lasts] = decode_members(path);
    return {bytes, static_cast<int>(lasts.size())};
}

bool needs_rechunk(const std::string& path, std::size_t cap) {
    int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    ScopedFd sfd(fd);
    struct stat st;
    REQUIRE(::fstat(sfd.get(), &st) == 0);
    return gzc::gzip_first_member(sfd.get(),
                                  static_cast<std::uint64_t>(st.st_size), cap)
        .get()
        .exceeds_cap;
}

}  // namespace

TEST_SUITE("gzip_rechunker") {
    TEST_CASE("needs_rechunk flags a member over the cap only") {
        auto dir = dftu_utils_test::make_unique_test_path("rechunk_probe");
        fs::create_directories(dir);
        const std::string gz = (dir / "one.pfw.gz").string();
        const auto payload = make_lines(2000);  // ~90 KB uncompressed
        write_single_member_gz(gz, payload);

        CHECK(needs_rechunk(gz, 1024));         // member > 1 KB -> yes
        CHECK_FALSE(needs_rechunk(gz, 10 * 1024 * 1024));  // under cap -> no

        fs::remove_all(dir);
    }

    TEST_CASE("first_member samples past a tiny first member") {
        auto dir = dftu_utils_test::make_unique_test_path("rechunk_sample");
        fs::create_directories(dir);
        const std::string gz = (dir / "wrapped.pfw.gz").string();
        const auto payload = make_lines(5000);
        {
            gzc::GzipMemberCompressor comp;
            auto head = comp.compress_member("[\n", 2);
            auto body = comp.compress_member(payload.data(), payload.size());
            REQUIRE(head.has_value());
            REQUIRE(body.has_value());
            std::ofstream ofs(gz, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(head->data()),
                      static_cast<std::streamsize>(head->size()));
            ofs.write(reinterpret_cast<const char*>(body->data()),
                      static_cast<std::streamsize>(body->size()));
        }

        int fd = ::open(gz.c_str(), O_RDONLY);
        REQUIRE(fd >= 0);
        ScopedFd sfd(fd);
        struct stat st;
        REQUIRE(::fstat(sfd.get(), &st) == 0);
        const auto size = static_cast<std::uint64_t>(st.st_size);
        constexpr std::size_t CAP = 10 * 1024 * 1024;

        // Without a sample only the "[" member is read.
        auto first = gzc::gzip_first_member(sfd.get(), size, CAP).get();
        CHECK(first.uncompressed == 2);
        CHECK_FALSE(first.exceeds_cap);

        // With one, the next member counts too, and the cap still applies to
        // the first member only.
        auto sampled =
            gzc::gzip_first_member(sfd.get(), size, 16, 1 << 20).get();
        CHECK(sampled.uncompressed == 2 + payload.size());
        CHECK(sampled.compressed == size);
        CHECK_FALSE(sampled.exceeds_cap);

        fs::remove_all(dir);
    }

    TEST_CASE("rechunk preserves bytes and produces multiple members") {
        auto dir = dftu_utils_test::make_unique_test_path("rechunk_split");
        fs::create_directories(dir);
        const std::string in = (dir / "big.pfw.gz").string();
        const std::string out = (dir / "out.pfw.gz").string();
        const auto payload = make_lines(5000);
        write_single_member_gz(in, payload);

        // One member in, small member size out.
        CHECK(decode_all(in).second == 1);
        gzc::gzip_rechunk_to_members(in, out, /*member_size=*/8 * 1024,
                                     /*level=*/6)
            .get();

        auto [content, last_bytes] = decode_members(out);
        CHECK(content == payload);     // exact byte preservation
        CHECK(last_bytes.size() > 1);  // now multi-member
        // Every member but the last ends at a line end.
        for (std::size_t i = 0; i + 1 < last_bytes.size(); ++i) {
            CHECK(last_bytes[i] == '\n');
        }

        fs::remove_all(dir);
    }

    TEST_CASE("rechunk handles a payload with no trailing newline") {
        auto dir = dftu_utils_test::make_unique_test_path("rechunk_nonl");
        fs::create_directories(dir);
        const std::string in = (dir / "in.pfw.gz").string();
        const std::string out = (dir / "out.pfw.gz").string();
        std::string payload = make_lines(1000);
        payload += "{\"id\":99999,\"trailing\":true}";  // no final '\n'
        write_single_member_gz(in, payload);

        gzc::gzip_rechunk_to_members(in, out, 4 * 1024, 6).get();
        CHECK(decode_all(out).first == payload);

        fs::remove_all(dir);
    }
}
