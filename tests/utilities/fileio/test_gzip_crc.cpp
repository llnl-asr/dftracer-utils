#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/inflater.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <testing_runtime.h>
#include <unistd.h>
#include <zlib.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::index::gzip::GzipMemberRecord;
using dftracer::utils::index::gzip::GzipRecordKind;
using dftracer::utils::utilities::reader::internal::ReaderInflater;
using dftu_utils_test::run_coro;
namespace gz = dftracer::utils::utilities::fileio::compress;

// Counts every call zlib makes to its own crc32. The bodies use libdeflate, so
// zlib's results stay correct and the process never reaches zlib's crc32_z.
namespace {
std::atomic<unsigned long> ZLIB_CRC_CALLS{0};
// Calls made while a path under test decodes. The Runtime start-up and the
// test's own reference decode are left out.
std::atomic<unsigned long> PATH_CRC_CALLS{0};

struct CrcWindow {
    unsigned long start = ZLIB_CRC_CALLS.load();
    ~CrcWindow() { PATH_CRC_CALLS += ZLIB_CRC_CALLS.load() - start; }
};
}  // namespace

extern "C" {
uLong crc32(uLong crc, const Bytef* buf, uInt len) {
    ZLIB_CRC_CALLS.fetch_add(1, std::memory_order_relaxed);
    if (buf == nullptr) return 0;
    return gz::gzip_crc32(static_cast<std::uint32_t>(crc), buf, len);
}
uLong crc32_z(uLong crc, const Bytef* buf, z_size_t len) {
    ZLIB_CRC_CALLS.fetch_add(1, std::memory_order_relaxed);
    if (buf == nullptr) return 0;
    return gz::gzip_crc32(static_cast<std::uint32_t>(crc), buf, len);
}
}

namespace {

constexpr std::uint64_t CKPT = 1u << 20;

// Set by Sample: the decoded size of the whole file.
std::size_t FULL_TEXT_SIZE = 0;
constexpr std::size_t CRC_BYTE_FROM_END = 8;

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

std::vector<std::uint8_t> gzip_member(const std::string& text) {
    gz::GzipMemberCompressor c;
    std::vector<std::uint8_t> out;
    REQUIRE(c.compress_member_into(out, text.data(), text.size()));
    return out;
}

struct Sample {
    std::string text;
    std::vector<std::uint8_t> bytes;
    std::size_t first_member_size = 0;
    std::size_t first_text_size = 0;

    Sample() {
        const std::string big = hex_lines(6u << 20, 7);
        const auto first = gzip_member(big);
        first_member_size = first.size();
        first_text_size = big.size();
        text = big;
        bytes = first;
        for (std::uint32_t i = 0; i < 3; ++i) {
            const std::string small = hex_lines(40000, 100 + i);
            const auto m = gzip_member(small);
            bytes.insert(bytes.end(), m.begin(), m.end());
            text += small;
        }
        FULL_TEXT_SIZE = text.size();
    }

    std::vector<std::uint8_t> bad_trailer_crc() const {
        auto b = bytes;
        b[first_member_size - CRC_BYTE_FROM_END] ^= 0x55;
        return b;
    }
    std::vector<std::uint8_t> bad_data_byte() const {
        auto b = bytes;
        b[first_member_size / 2] ^= 0x55;
        return b;
    }
};

struct TempFile {
    std::string dir;
    std::string path;

    explicit TempFile(const std::vector<std::uint8_t>& bytes) {
        char tmpl[] = "/tmp/dftu_gzcrc_XXXXXX";
        REQUIRE(::mkdtemp(tmpl) != nullptr);
        dir = tmpl;
        path = dir + "/t.gz";
        std::FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
        std::fclose(f);
    }
    ~TempFile() { std::filesystem::remove_all(dir); }
};

// Whole-file reference decode with zlib (counts crc calls; call it outside a
// measured region).
std::string zlib_decode_all(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::vector<unsigned char> in;
    unsigned char buf[1 << 16];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        in.insert(in.end(), buf, buf + n);
    std::fclose(f);
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, 31) == Z_OK);
    zs.next_in = in.data();
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out;
    std::vector<unsigned char> chunk(1 << 20);
    while (true) {
        zs.next_out = chunk.data();
        zs.avail_out = static_cast<uInt>(chunk.size());
        const int rc = inflate(&zs, Z_NO_FLUSH);
        out.append(reinterpret_cast<char*>(chunk.data()),
                   chunk.size() - zs.avail_out);
        if (rc == Z_STREAM_END) {
            if (zs.avail_in == 0) break;
            REQUIRE(inflateReset(&zs) == Z_OK);
        } else {
            REQUIRE(rc == Z_OK);
        }
    }
    inflateEnd(&zs);
    return out;
}

// Each path returns the decoded text, or nullopt when it rejected the input.
using PathResult = std::optional<std::string>;

PathResult read_with_inflater(const std::string& path, GzipRecordKind kind) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::string out;
    bool ok = false;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        ReaderInflater inflater;
        CrcWindow window;
        off_t offset = 0;
        GzipMemberRecord rec;
        rec.kind = kind;
        ok = co_await inflater.seek_to_member(fd, offset, rec, std::string());
        std::vector<unsigned char> buf(1u << 20);
        while (ok) {
            std::size_t got = 0;
            ok =
                co_await inflater.read(fd, offset, buf.data(), buf.size(), got);
            if (!ok || got == 0) break;
            out.append(reinterpret_cast<char*>(buf.data()), got);
        }
    });
    ::close(fd);
    if (!ok) return std::nullopt;
    return out;
}

PathResult reader_head_stream(const std::string& path) {
    return read_with_inflater(path, GzipRecordKind::Head);
}

PathResult reader_whole_member(const std::string& path) {
    return read_with_inflater(path, GzipRecordKind::Member);
}

PathResult indexer_stream(const std::string& path) {
    std::optional<index::gzip::GzipBuildArtifacts> art;
    run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
        CrcWindow window;
        art = co_await index::gzip::build_gzip_index_artifacts(
            path, CKPT, index::gzip::CheckpointIndexer::VisitorList{}, &scope);
    });
    // A corrupt member ends the build at that member, short of the full text.
    if (!art || art->total_uc_size != FULL_TEXT_SIZE) return std::nullopt;
    return std::string(art->total_uc_size, 'x');
}

PathResult truncated_decoder(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::vector<std::uint8_t> in;
    std::uint8_t buf[1 << 16];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        in.insert(in.end(), buf, buf + n);
    std::fclose(f);
    std::vector<std::uint8_t> out;
    std::optional<std::size_t> keep;
    {
        CrcWindow window;
        keep = gz::decode_truncated_member(in.data(), in.size(), out);
    }
    if (!keep) return std::nullopt;
    return std::string(out.begin(), out.begin() + *keep);
}

PathResult rechunker(const std::string& path) {
    const std::string out_path = path + ".rechunked.gz";
    bool ok = true;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        CrcWindow window;
        try {
            co_await gz::gzip_rechunk_to_members(path, out_path, 1u << 20, 1);
        } catch (...) {
            ok = false;
        }
    });
    if (!ok) return std::nullopt;
    auto decoded = zlib_decode_all(out_path);
    return decoded;
}

struct PathCase {
    const char* name;
    PathResult (*run)(const std::string&);
    // The index path reports only the decoded size, and the truncated decoder
    // takes one member.
    bool whole_file;
    bool size_only;
};

const PathCase PATHS[] = {
    {"reader head stream", reader_head_stream, true, false},
    {"reader whole member", reader_whole_member, true, false},
    {"index stream_member", indexer_stream, true, true},
    {"rechunker", rechunker, true, false},
    {"truncated-member decoder", truncated_decoder, false, false},
};

bool same(const PathCase& p, const PathResult& got, const std::string& text) {
    if (!got) return false;
    if (p.size_only) return got->size() == text.size();
    return *got == text;
}

}  // namespace

TEST_CASE("the control: zlib's own gzip inflate calls crc32") {
    Sample s;
    TempFile f(s.bytes);
    ZLIB_CRC_CALLS = 0;
    CHECK(zlib_decode_all(f.path) == s.text);
    CHECK(ZLIB_CRC_CALLS.load() > 0);
}

TEST_CASE("good multi-member input decodes identically without zlib crc32") {
    Sample s;
    for (const auto& p : PATHS) {
        CAPTURE(std::string(p.name));
        std::string expect = s.text;
        std::vector<std::uint8_t> bytes = s.bytes;
        if (!p.whole_file) {
            bytes.resize(s.first_member_size);
            expect = s.text.substr(0, s.first_text_size);
        }
        TempFile file(bytes);
        PATH_CRC_CALLS = 0;
        const auto got = p.run(file.path);
        CHECK(PATH_CRC_CALLS.load() == 0);
        CHECK(same(p, got, expect));
    }
}

TEST_CASE("a flipped trailer crc byte is rejected on every path") {
    Sample s;
    for (const auto& p : PATHS) {
        CAPTURE(std::string(p.name));
        auto bytes = s.bad_trailer_crc();
        if (!p.whole_file) bytes.resize(s.first_member_size);
        TempFile file(bytes);
        CHECK_FALSE(p.run(file.path).has_value());
    }
}

TEST_CASE("a flipped data byte is rejected on every path") {
    Sample s;
    for (const auto& p : PATHS) {
        CAPTURE(std::string(p.name));
        auto bytes = s.bad_data_byte();
        if (!p.whole_file) bytes.resize(s.first_member_size);
        TempFile file(bytes);
        CHECK_FALSE(p.run(file.path).has_value());
    }
}

TEST_CASE("a gzip header with FHCRC and optional fields is accepted") {
    const std::string text = hex_lines(200000, 3);
    gz::GzipMemberCompressor c;
    std::vector<std::uint8_t> plain;
    REQUIRE(c.compress_member_into(plain, text.data(), text.size()));
    constexpr std::size_t PLAIN_HEADER = 10;
    std::vector<std::uint8_t> header = {0x1f, 0x8b, 8, 2 | 4 | 8 | 16, 0, 0, 0,
                                        0,    0,    3};
    const std::uint8_t extra[] = {3, 0, 'a', 'b', 'c'};
    header.insert(header.end(), extra, extra + sizeof(extra));
    for (const char* s : {"name", "note"}) {
        for (const char* p = s; *p; ++p) header.push_back(*p);
        header.push_back(0);
    }
    const std::uint32_t hcrc = gz::gzip_crc32(0, header.data(), header.size());
    header.push_back(static_cast<std::uint8_t>(hcrc & 0xff));
    header.push_back(static_cast<std::uint8_t>((hcrc >> 8) & 0xff));
    std::vector<std::uint8_t> member = header;
    member.insert(member.end(), plain.begin() + PLAIN_HEADER, plain.end());

    auto decode = [&](const std::vector<std::uint8_t>& in) {
        gz::GzipStreamInflater inf;
        REQUIRE(inf.init(31));
        z_stream& zs = inf.z();
        zs.next_in = const_cast<Bytef*>(in.data());
        zs.avail_in = static_cast<uInt>(in.size());
        std::string out(text.size() + 16, '\0');
        zs.next_out = reinterpret_cast<Bytef*>(out.data());
        zs.avail_out = static_cast<uInt>(out.size());
        const int rc = inf.inflate(Z_NO_FLUSH);
        out.resize(out.size() - zs.avail_out);
        return std::make_pair(rc, out);
    };
    const auto good = decode(member);
    CHECK(good.first == Z_STREAM_END);
    CHECK(good.second == text);

    auto bad = member;
    bad[header.size() - 1] ^= 1;
    CHECK(decode(bad).first == Z_DATA_ERROR);
}
