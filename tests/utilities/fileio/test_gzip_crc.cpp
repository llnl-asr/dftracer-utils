// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_rechunker.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_stream_inflater.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/reader/internal/inflater.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <task_result.h>
#include <testing_runtime.h>
#include <unistd.h>
#include <zlib.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <random>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities;

namespace {
// Streams opened in a mode where zlib-ng verifies the checksum, counted while
// a path under test decodes. The Runtime start-up and the test's own
// reference decode are left out.
std::atomic<std::uint64_t> PATH_CHECKSUM_OPENS{0};

struct CrcWindow {
    std::uint64_t start = fileio::compress::zlib_checksum_streams_opened();
    ~CrcWindow() {
        PATH_CHECKSUM_OPENS +=
            fileio::compress::zlib_checksum_streams_opened() - start;
    }
};
}  // namespace

namespace {

constexpr std::uint64_t CKPT = 1u << 20;

// Set by Sample to the decoded size of the whole file.
std::size_t FULL_TEXT_SIZE = 0;
constexpr std::size_t CRC_BYTE_FROM_END = 8;

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

dstd::Vector<std::uint8_t> gzip_member(const dstd::String& text) {
    fileio::compress::GzipMemberCompressor c;
    dstd::Vector<std::uint8_t> out;
    REQUIRE(c.compress_member_into(out, text.data(), text.size()));
    return out;
}

struct Sample {
    dstd::String text;
    dstd::Vector<std::uint8_t> bytes;
    std::size_t first_member_size = 0;
    std::size_t first_text_size = 0;

    Sample() {
        const dstd::String big = hex_lines(6u << 20, 7);
        const auto first = gzip_member(big);
        first_member_size = first.size();
        first_text_size = big.size();
        text = big;
        bytes = first;
        for (std::uint32_t i = 0; i < 3; ++i) {
            const dstd::String small = hex_lines(40000, 100 + i);
            const auto m = gzip_member(small);
            bytes.insert(bytes.end(), m.begin(), m.end());
            text += small;
        }
        FULL_TEXT_SIZE = text.size();
    }

    dstd::Vector<std::uint8_t> bad_trailer_crc() const {
        auto b = bytes;
        b[first_member_size - CRC_BYTE_FROM_END] ^= 0x55;
        return b;
    }
    dstd::Vector<std::uint8_t> bad_data_byte() const {
        auto b = bytes;
        b[first_member_size / 2] ^= 0x55;
        return b;
    }
};

struct TempFile {
    dstd::String dir;
    dstd::String path;

    explicit TempFile(const dstd::Vector<std::uint8_t>& bytes) {
        char tmpl[] = "/tmp/dftu_gzcrc_XXXXXX";
        REQUIRE(::mkdtemp(tmpl) != nullptr);
        dir = tmpl;
        path = dstd::fmt<"{}/t.gz">(dir);
        std::FILE* f = std::fopen(path.c_str(), "wb");
        REQUIRE(f != nullptr);
        REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
        std::fclose(f);
    }
    ~TempFile() { dstd::fs::remove_all(dir).discard(); }
};

// Whole-file reference decode with the system zlib.
dstd::String zlib_decode_all(const dstd::String& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    dstd::Vector<unsigned char> in;
    unsigned char buf[1 << 16];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        in.insert(in.end(), buf, buf + n);
    std::fclose(f);
    z_stream zs{};
    REQUIRE(inflateInit2(&zs, 31) == Z_OK);
    zs.next_in = in.data();
    zs.avail_in = static_cast<uInt>(in.size());
    dstd::String out;
    dstd::Vector<unsigned char> chunk(1 << 20);
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
using PathResult = dstd::Optional<dstd::String>;

PathResult read_with_inflater(const dstd::String& path,
                              index::gzip::GzipRecordKind kind) {
    auto fd_file = dstd::File::open(path, {});
    REQUIRE(fd_file);
    const int fd = fd_file->native_handle();
    dstd::String out;
    bool ok = false;
    dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        reader::internal::ReaderInflater inflater;
        CrcWindow window;
        off_t offset = 0;
        index::gzip::GzipMemberRecord rec;
        rec.kind = kind;
        ok = co_await inflater.seek_to_member(fd, offset, rec, dstd::String());
        dstd::Vector<unsigned char> buf(1u << 20);
        while (ok) {
            std::size_t got = 0;
            ok =
                co_await inflater.read(fd, offset, buf.data(), buf.size(), got);
            if (!ok || got == 0) break;
            out.append(reinterpret_cast<char*>(buf.data()), got);
        }
    });
    if (!ok) return dstd::nullopt;
    return out;
}

PathResult reader_head_stream(const dstd::String& path) {
    return read_with_inflater(path, index::gzip::GzipRecordKind::Head);
}

PathResult reader_whole_member(const dstd::String& path) {
    return read_with_inflater(path, index::gzip::GzipRecordKind::Member);
}

PathResult indexer_stream(const dstd::String& path) {
    dstd::Optional<index::gzip::GzipBuildArtifacts> art;
    dftu_utils_test::run_coro([&](coro::Scope& scope) -> coro::CoroTask<void> {
        CrcWindow window;
        auto built = co_await index::gzip::build_gzip_index_artifacts(
            dstd::String(path), CKPT,
            index::gzip::CheckpointIndexer::VisitorList{}, &scope);
        if (built) art = std::move(*built);
    });
    // A corrupt member ends the build at that member, short of the full text.
    if (!art || art->total_uc_size != FULL_TEXT_SIZE) return dstd::nullopt;
    return dstd::String(art->total_uc_size, 'x');
}

PathResult truncated_decoder(const dstd::String& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    dstd::Vector<std::uint8_t> in;
    std::uint8_t buf[1 << 16];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        in.insert(in.end(), buf, buf + n);
    std::fclose(f);
    dstd::Vector<std::uint8_t> out;
    dstd::Optional<std::size_t> keep;
    {
        CrcWindow window;
        keep = fileio::compress::decode_truncated_member(in.data(), in.size(),
                                                         out);
    }
    if (!keep) return dstd::nullopt;
    return dstd::String(out.begin(), out.begin() + *keep);
}

PathResult rechunker(const dstd::String& path) {
    const dstd::String out_path = dstd::fmt<"{}.rechunked.gz">(path);
    bool ok = true;
    dftu_utils_test::run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        CrcWindow window;
        auto rechunked = co_await fileio::compress::gzip_rechunk_to_members(
            path, out_path, 1u << 20, 1);
        ok = rechunked.has_value();
    });
    if (!ok) return dstd::nullopt;
    auto decoded = zlib_decode_all(out_path);
    return decoded;
}

struct PathCase {
    const char* name;
    PathResult (*run)(const dstd::String&);
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

bool same(const PathCase& p, const PathResult& got, const dstd::String& text) {
    if (!got) return false;
    if (p.size_only) return got->size() == text.size();
    return *got == text;
}

}  // namespace

// Schema detection reads the head of a gzip file with a zlib-ng stream that
// verifies the checksum, so it shows the counter moves.
TEST_CASE("the control: a stream that verifies the gzip checksum is counted") {
    Sample s;
    TempFile f(s.bytes);
    const auto before = fileio::compress::zlib_checksum_streams_opened();
    dftu_utils_test::ok_ref(index::detect_file_schema(dstd::String(f.path)));
    CHECK(fileio::compress::zlib_checksum_streams_opened() > before);
}

TEST_CASE(
    "good multi-member input decodes identically without a zlib checksum") {
    Sample s;
    for (const auto& p : PATHS) {
        CAPTURE(dstd::String(p.name));
        dstd::String expect = s.text;
        dstd::Vector<std::uint8_t> bytes = s.bytes;
        if (!p.whole_file) {
            bytes.resize(s.first_member_size);
            expect = s.text.substr(0, s.first_text_size);
        }
        TempFile file(bytes);
        PATH_CHECKSUM_OPENS = 0;
        const auto got = p.run(file.path);
        CHECK(PATH_CHECKSUM_OPENS.load() == 0);
        CHECK(same(p, got, expect));
    }
}

TEST_CASE("a flipped trailer crc byte is rejected on every path") {
    Sample s;
    for (const auto& p : PATHS) {
        CAPTURE(dstd::String(p.name));
        auto bytes = s.bad_trailer_crc();
        if (!p.whole_file) bytes.resize(s.first_member_size);
        TempFile file(bytes);
        CHECK_FALSE(p.run(file.path).has_value());
    }
}

TEST_CASE("a flipped data byte is rejected on every path") {
    Sample s;
    for (const auto& p : PATHS) {
        CAPTURE(dstd::String(p.name));
        auto bytes = s.bad_data_byte();
        if (!p.whole_file) bytes.resize(s.first_member_size);
        TempFile file(bytes);
        CHECK_FALSE(p.run(file.path).has_value());
    }
}

TEST_CASE("a gzip header with FHCRC and optional fields is accepted") {
    const dstd::String text = hex_lines(200000, 3);
    fileio::compress::GzipMemberCompressor c;
    dstd::Vector<std::uint8_t> plain;
    REQUIRE(c.compress_member_into(plain, text.data(), text.size()));
    constexpr std::size_t PLAIN_HEADER = 10;
    dstd::Vector<std::uint8_t> header = {0x1f, 0x8b, 8, 2 | 4 | 8 | 16, 0, 0, 0,
                                         0,    0,    3};
    const std::uint8_t extra[] = {3, 0, 'a', 'b', 'c'};
    header.insert(header.end(), extra, extra + sizeof(extra));
    for (const char* s : {"name", "note"}) {
        for (const char* p = s; *p; ++p) header.push_back(*p);
        header.push_back(0);
    }
    const std::uint32_t hcrc =
        fileio::compress::gzip_crc32(0, header.data(), header.size());
    header.push_back(static_cast<std::uint8_t>(hcrc & 0xff));
    header.push_back(static_cast<std::uint8_t>((hcrc >> 8) & 0xff));
    dstd::Vector<std::uint8_t> member = header;
    member.insert(member.end(), plain.begin() + PLAIN_HEADER, plain.end());

    auto decode = [&](const dstd::Vector<std::uint8_t>& in) {
        fileio::compress::GzipStreamInflater inf;
        REQUIRE(inf.init(31));
        inf.set_input(in.data(), in.size());
        dstd::String out(text.size() + 16, '\0');
        inf.set_output(reinterpret_cast<unsigned char*>(out.data()),
                       out.size());
        const auto rc = inf.inflate();
        out.resize(out.size() - inf.avail_out());
        return dstd::Pair<decltype(rc), dstd::String>{rc, out};
    };
    const auto good = decode(member);
    CHECK(good.first == fileio::compress::InflateStatus::StreamEnd);
    CHECK(good.second == text);

    auto bad = member;
    bad[header.size() - 1] ^= 1;
    CHECK(decode(bad).first == fileio::compress::InflateStatus::DataError);
}

TEST_CASE("compression state is charged to the calling pool") {
    memory::PoolHandle pool =
        memory::detail::Manager::get().root().child("codecs");
    REQUIRE(pool);
    {
        memory::detail::Scope scope{*pool};
        fileio::compress::GzipStreamInflater inf;
        REQUIRE(inf.init(31));
        CHECK(pool->live() > 0);
    }
    CHECK(pool->live() == 0);
    {
        memory::detail::Scope scope{*pool};
        fileio::compress::GzipMemberDecompressor decompressor;
        CHECK(pool->live() > 0);
    }
    CHECK(pool->live() == 0);
}
