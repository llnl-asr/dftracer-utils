#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/tasks/coro_scope.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/view.h>
#include <dftracer/utils/utilities/fileio/compress/gzip_member_reader.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/utilities/reader/internal/gzip_reader.h>
#include <dftracer/utils/utilities/reader/internal/inflater.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>
#include <dftracer/utils/utilities/reader/internal/stream_config.h>
#include <doctest/doctest.h>
#include <fcntl.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using dftracer::utils::index::gzip::GzipIndexer;
using dftracer::utils::index::gzip::GzipMemberRecord;
using dftracer::utils::index::gzip::GzipRecordKind;
using dftracer::utils::utilities::reader::internal::GzipReader;
using dftracer::utils::utilities::reader::internal::MemberDecodeCache;
using dftracer::utils::utilities::reader::internal::ReaderInflater;
using dftu_utils_test::run_coro;
namespace gz = dftracer::utils::utilities::fileio::compress;
namespace views = dftracer::utils::trace::views;

namespace {

constexpr std::uint64_t CKPT = 1u << 20;
constexpr std::size_t SMALL_BYTES = std::size_t{8} << 20;
constexpr std::size_t BIG_BYTES = SMALL_BYTES * 8;
constexpr std::size_t PREFIX_LINES = 1000;
constexpr std::size_t MEMBER_BYTES = std::size_t{256} << 10;
// Memory a read of any member size may hold: a member above the bound is
// streamed, and the bound plus its input is what a member below it costs.
constexpr std::size_t BOUND_BYTES = std::size_t{4} << 20;
constexpr std::size_t BUDGET_BYTES = std::size_t{24} << 20;
constexpr std::size_t DEFAULT_BOUND_BUDGET_BYTES = std::size_t{72} << 20;

struct Trace {
    std::string text;
    std::size_t lines = 0;
    std::size_t reads = 0;
};

// Event lines of about 140 bytes: `bytes` of them after PREFIX_LINES more, so a
// file can hold a small member before the big one.
Trace make_trace(std::size_t bytes, std::uint32_t seed) {
    static const char HEX[] = "0123456789abcdef";
    std::mt19937 rng(seed);
    Trace t;
    t.text.reserve(bytes + 4096);
    while (t.text.size() < bytes) {
        const std::size_t i = t.lines++;
        const bool read = i % 3 != 0;
        t.reads += read;
        std::string fh;
        for (int k = 0; k < 32; ++k) fh += HEX[rng() & 15];
        t.text += std::string("{\"id\":") + std::to_string(i) + ",\"name\":\"" +
                  (read ? "read" : "write") +
                  "\",\"cat\":\"POSIX\",\"pid\":" + std::to_string(1 + i % 4) +
                  ",\"tid\":" + std::to_string(1 + i % 8) +
                  ",\"ts\":" + std::to_string(1000 + i) +
                  ",\"dur\":" + std::to_string(1 + i % 17) +
                  ",\"ph\":\"X\",\"args\":{\"fh\":\"" + fh + "\"}}\n";
    }
    return t;
}

// Lines of 1 to 6 letters, so a restart piece starts after a newline about
// one time in four.
Trace short_lines(std::size_t bytes, std::uint32_t seed) {
    std::mt19937 rng(seed);
    Trace t;
    t.text.reserve(bytes + 16);
    while (t.text.size() < bytes) {
        ++t.lines;
        for (auto n = static_cast<std::uint32_t>(1 + rng() % 6); n > 0; --n)
            t.text += static_cast<char>('a' + rng() % 26);
        t.text += '\n';
    }
    return t;
}

std::vector<std::uint8_t> member(const std::string& text) {
    gz::GzipMemberCompressor c;
    std::vector<std::uint8_t> out(c.bound(text.size()));
    auto n = c.compress(text.data(), text.size(), out.data(), out.size());
    REQUIRE(n.has_value());
    out.resize(*n);
    return out;
}

void append(std::vector<std::uint8_t>& to, const std::string& text) {
    const auto m = member(text);
    to.insert(to.end(), m.begin(), m.end());
}

void write_file(const std::string& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

// The first PREFIX_LINES lines in one member, the rest in one member.
std::vector<std::uint8_t> one_big_member(const Trace& t) {
    std::size_t at = 0;
    for (std::size_t i = 0; i < PREFIX_LINES; ++i)
        at = t.text.find('\n', at) + 1;
    std::vector<std::uint8_t> bytes;
    append(bytes, t.text.substr(0, at));
    append(bytes, t.text.substr(at));
    return bytes;
}

// The same lines in members of about MEMBER_BYTES.
std::vector<std::uint8_t> many_members(const Trace& t) {
    std::vector<std::uint8_t> bytes;
    std::size_t from = 0;
    while (from < t.text.size()) {
        std::size_t to = std::min(t.text.size(), from + MEMBER_BYTES);
        if (to < t.text.size()) to = t.text.find('\n', to) + 1;
        append(bytes, t.text.substr(from, to - from));
        from = to;
    }
    return bytes;
}

// Largest live byte count above the start while `body` runs.
std::size_t peak_above_start(const std::function<void()>& body) {
    std::atomic<bool> done{false};
    std::atomic<std::size_t> peak{0};
    const std::size_t base = memory::Manager::get().stats().live;
    std::thread sampler([&] {
        while (!done.load(std::memory_order_relaxed)) {
            const auto live = memory::Manager::get().stats().live;
            auto seen = peak.load(std::memory_order_relaxed);
            while (live > seen && !peak.compare_exchange_weak(
                                      seen, live, std::memory_order_relaxed)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    body();
    done.store(true);
    sampler.join();
    const auto top = peak.load();
    return top > base ? top - base : 0;
}

struct Files {
    std::string dir;
    std::string big;   // a small member, then one big member
    std::string many;  // the same lines in small members
    Trace trace;
    explicit Files(std::size_t bytes, std::uint32_t seed, const char* tag) {
        dir = std::string("/tmp/dftu_bigmember_") + tag + "_XXXXXX";
        REQUIRE(::mkdtemp(dir.data()) != nullptr);
        trace = make_trace(bytes, seed);
        big = dir + "/big.pfw.gz";
        many = dir + "/many.pfw.gz";
        write_file(big, one_big_member(trace));
        write_file(many, many_members(trace));
    }
    ~Files() { std::filesystem::remove_all(dir); }
};

// Inflates the file with a bound of BOUND_BYTES and counts the buffers that do
// not equal `expected` there.
std::size_t inflate_mismatches(
    const std::string& path, const std::string& expected,
    utilities::reader::internal::MemberDecodeCache* cache = nullptr,
    const dstd::Vector<std::uint64_t>* heads = nullptr) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    std::size_t at = 0, bad = 0;
    bool ok = false;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        ReaderInflater inf;
        inf.set_member_bound(BOUND_BYTES);
        if (cache != nullptr) inf.set_cache(cache, 7);
        if (heads != nullptr) inf.set_index_hint(*heads);
        off_t offset = 0;
        if (!co_await inf.initialize(fd, offset)) co_return;
        std::vector<unsigned char> buf(1 << 20);
        while (true) {
            std::size_t n = 0;
            if (!co_await inf.read(fd, offset, buf.data(), buf.size(), n))
                co_return;
            if (n == 0) break;
            if (at + n > expected.size() ||
                std::memcmp(buf.data(), expected.data() + at, n) != 0)
                ++bad;
            at += n;
        }
        ok = true;
    });
    ::close(fd);
    REQUIRE(ok);
    return bad + (at == expected.size() ? 0 : 1);
}

std::size_t members_lines(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    REQUIRE(fd >= 0);
    const auto size = static_cast<std::uint64_t>(::lseek(fd, 0, SEEK_END));
    std::size_t lines = 0;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        auto gen = gz::decode_gzip_members(fd, size, BOUND_BYTES);
        while (auto part = co_await gen.next())
            lines += static_cast<std::size_t>(
                std::count(part->begin(), part->end(), '\n'));
    });
    ::close(fd);
    return lines;
}

// A GzipIndexer over `path` and the reads the reader serves from it.
struct Indexed {
    std::shared_ptr<GzipIndexer> indexer;
    Indexed(const std::string& path, const std::string& index_path) {
        indexer = std::make_shared<GzipIndexer>(path, index_path, CKPT, true);
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            co_await indexer->build_async();
        });
    }
    std::string read_lines(std::size_t first, std::size_t last) {
        GzipReader reader(indexer);
        std::string out;
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            out = co_await reader.read_lines_async(first, last);
        });
        return out;
    }
};

// Bytes the process has read, from /proc/self/io; 0 where there is none.
std::uint64_t bytes_read_by_process() {
    std::ifstream io("/proc/self/io");
    std::string key;
    std::uint64_t value = 0;
    while (io >> key >> value)
        if (key == "rchar:") return value;
    return 0;
}

std::size_t newlines_before(const std::string& text, std::size_t at);

// What the line-aligned byte stream returns for [from, end of file).
std::string read_line_bytes(const std::shared_ptr<GzipIndexer>& indexer,
                            std::size_t from) {
    GzipReader reader(indexer);
    auto stream = reader.stream(
        utilities::reader::internal::StreamConfig()
            .stream_type(utilities::reader::internal::StreamType::LineBytes)
            .range_type(utilities::reader::internal::RangeType::ByteRange)
            .from(from)
            .to(reader.get_max_bytes()));
    std::string out;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        std::vector<char> buf(1u << 20);
        while (true) {
            const auto n = co_await stream->read_async(buf.data(), buf.size());
            if (n == 0) break;
            out.append(buf.data(), n);
        }
    });
    return out;
}

std::string lines_of(const std::string& text, std::size_t first,
                     std::size_t last) {
    std::size_t at = 0;
    for (std::size_t i = 1; i < first; ++i) at = text.find('\n', at) + 1;
    std::size_t end = at;
    for (std::size_t i = first; i <= last; ++i) end = text.find('\n', end) + 1;
    return text.substr(at, end - at);
}

std::size_t newlines_before(const std::string& text, std::size_t at) {
    return static_cast<std::size_t>(
        std::count(text.begin(), text.begin() + static_cast<long>(at), '\n'));
}

views::ViewFile view_file(const std::string& gz_path, const std::string& dir) {
    REQUIRE(dftu_utils_test::build_index(gz_path, dir, CKPT));
    views::ViewFile f;
    f.file_path = gz_path;
    f.index_path = trace::internal::determine_index_path(gz_path, dir);
    f.checkpoint_size = CKPT;
    return f;
}

std::size_t count_reads(const views::ViewFile& f) {
    auto df = views::View::from_files({f})
                  .duql(R"(name == "read" | agg { n = count() })")
                  .lazy()
                  .collect()
                  .get();
    REQUIRE(df.num_rows() == 1);
    return static_cast<std::size_t>(df.columns[0].data<std::uint64_t>()[0]);
}

}  // namespace

TEST_SUITE("BigMemberReads") {
    TEST_CASE(
        "a member after a small one inflates with the memory of a small") {
        Files small(SMALL_BYTES, 1, "a");
        Files big(BIG_BYTES, 1, "b");
        std::size_t bad_small = 1, bad_big = 1;
        const auto p_small = peak_above_start([&] {
            bad_small = inflate_mismatches(small.big, small.trace.text);
        });
        const auto p_big = peak_above_start(
            [&] { bad_big = inflate_mismatches(big.big, big.trace.text); });
        CHECK(bad_small == 0);
        CHECK(bad_big == 0);
        MESSAGE("inflater peak: " << p_small << " small, " << p_big << " big");
        CHECK(p_big <= BUDGET_BYTES);
    }

    TEST_CASE("the member line reader holds a bounded part of a big member") {
        Files small(SMALL_BYTES, 2, "c");
        Files big(BIG_BYTES, 2, "d");
        std::size_t lines_small = 0, lines_big = 0;
        const auto p_small =
            peak_above_start([&] { lines_small = members_lines(small.big); });
        const auto p_big =
            peak_above_start([&] { lines_big = members_lines(big.big); });
        CHECK(lines_small == small.trace.lines);
        CHECK(lines_big == big.trace.lines);
        MESSAGE("member reader peak: " << p_small << " small, " << p_big
                                       << " big");
        CHECK(p_big <= BUDGET_BYTES);
    }

    TEST_CASE("the line reader serves lines from every piece of a big member") {
        Files fx(BIG_BYTES, 3, "e");
        Indexed big(fx.big, fx.dir + "/idx_big");
        Indexed many(fx.many, fx.dir + "/idx_many");
        const auto pieces = big.indexer->get_members();
        REQUIRE(pieces.size() > 16);
        std::size_t restarts = 0;
        for (const auto& p : pieces)
            restarts += p.kind == GzipRecordKind::Restart;
        CHECK(restarts > 16);
        CHECK(big.indexer->get_num_lines() == many.indexer->get_num_lines());

        const std::size_t total = fx.trace.lines;
        std::mt19937 rng(11);
        std::size_t compared = 0;
        const auto before = peak_above_start([&] {
            // Starts inside every kind of piece, found by line number.
            for (const auto& p : pieces) {
                const std::size_t first = std::min<std::size_t>(
                    static_cast<std::size_t>(p.first_line_num) + 3, total - 4);
                const std::size_t last = first + 3;
                CAPTURE(p.member_idx);
                CHECK(big.read_lines(first, last) ==
                      lines_of(fx.trace.text, first, last));
                CHECK(many.read_lines(first, last) ==
                      lines_of(fx.trace.text, first, last));
                ++compared;
            }
            for (int i = 0; i < 20; ++i) {
                const std::size_t first = 1 + rng() % (total - 8);
                CHECK(big.read_lines(first, first + 7) ==
                      lines_of(fx.trace.text, first, first + 7));
            }
        });
        CHECK(compared == pieces.size());
        MESSAGE("line reader peak: " << before);
        CHECK(before <= BUDGET_BYTES + BOUND_BYTES * 4);
    }

    TEST_CASE(
        "the lines the reader reports for a piece match the stored counts") {
        // A piece starts where the inflater stopped at a block end, so it
        // starts mid-line almost always. Search seeds for a file with a piece
        // that starts right after a newline too.
        bool after_newline = false, mid_line = false;
        for (std::uint32_t seed = 1; seed <= 80 && !(after_newline && mid_line);
             ++seed) {
            const Trace t = short_lines(std::size_t{6} << 20, 100 + seed);
            const std::string dir =
                "/tmp/dftu_bigmember_n_" + std::to_string(seed);
            std::filesystem::create_directories(dir);
            std::vector<std::uint8_t> bytes;
            append(bytes, t.text);
            write_file(dir + "/t.gz", bytes);
            Indexed ix(dir + "/t.gz", dir + "/idx");
            const auto pieces = ix.indexer->get_members();
            for (const auto& p : pieces) {
                if (p.kind != GzipRecordKind::Restart) continue;
                const std::size_t at = static_cast<std::size_t>(p.uc_offset);
                const bool newline_before = t.text[at - 1] == '\n';
                // The folds count a line in the piece where it ends.
                CHECK(p.first_line_num == newlines_before(t.text, at) + 1);
                // The first line the piece owns whole.
                const std::size_t first = static_cast<std::size_t>(
                    p.first_line_num + (newline_before ? 0 : 1));
                if (first + 2 > t.lines) continue;
                CAPTURE(seed);
                CAPTURE(p.member_idx);
                CHECK(ix.read_lines(first, first + 2) ==
                      lines_of(t.text, first, first + 2));
                (newline_before ? after_newline : mid_line) = true;
            }
            std::filesystem::remove_all(dir);
        }
        CHECK(mid_line);
        CHECK(after_newline);
    }

    TEST_CASE("the unindexed line reader streams a big member") {
        Files small(SMALL_BYTES, 4, "f");
        Files big(BIG_BYTES, 4, "g");
        auto count = [](const std::string& path) {
            std::size_t n = 0;
            run_coro([&](CoroScope&) -> coro::CoroTask<void> {
                auto lines =
                    utilities::fileio::lines::sources::async_streaming_gz_lines(
                        path);
                while (auto line = co_await lines.next()) ++n;
            });
            return n;
        };
        std::size_t n_small = 0, n_big = 0;
        const auto p_small =
            peak_above_start([&] { n_small = count(small.big); });
        const auto p_big = peak_above_start([&] { n_big = count(big.big); });
        CHECK(n_small == small.trace.lines);
        CHECK(n_big == big.trace.lines);
        MESSAGE("line generator peak: " << p_small << " small, " << p_big
                                        << " big");
        CHECK(p_big <= DEFAULT_BOUND_BUDGET_BYTES);
    }

    TEST_CASE("the view counts a big member like the same lines in members") {
        Files fx(BIG_BYTES, 5, "h");
        const auto big = view_file(fx.big, fx.dir + "/vidx_big");
        const auto many = view_file(fx.many, fx.dir + "/vidx_many");
        std::size_t from_big = 0;
        const auto peak =
            peak_above_start([&] { from_big = count_reads(big); });
        const auto from_many = count_reads(many);
        CHECK(from_big == fx.trace.reads);
        CHECK(from_many == fx.trace.reads);
        MESSAGE("view peak: " << peak);
        CHECK(peak <= DEFAULT_BOUND_BUDGET_BYTES * 2);
    }

    TEST_CASE(
        "cached members before a big one leave the stream at its offset") {
        // Small members, a big one, small ones again: the cache serves the
        // small members and advances the reader without reading their input,
        // so the stream of the big member must start from the member's offset.
        const std::size_t small_bytes = std::size_t{300} << 10;
        std::string dir = "/tmp/dftu_bigmember_cache_XXXXXX";
        REQUIRE(::mkdtemp(dir.data()) != nullptr);
        std::string text;
        std::vector<std::uint8_t> bytes;
        std::uint64_t big_offset = 0;
        auto add = [&](const Trace& t) {
            text += t.text;
            append(bytes, t.text);
        };
        for (std::uint32_t i = 0; i < 6; ++i)
            add(make_trace(small_bytes, 20 + i));
        big_offset = bytes.size();
        add(make_trace(std::size_t{24} << 20, 30));
        for (std::uint32_t i = 0; i < 4; ++i)
            add(make_trace(small_bytes, 40 + i));
        const std::string path = dir + "/mixed.pfw.gz";
        write_file(path, bytes);

        dstd::Vector<std::uint64_t> heads{big_offset};
        for (const bool hinted : {false, true}) {
            MemberDecodeCache cache(std::size_t{64} << 20);
            CAPTURE(hinted);
            CHECK(inflate_mismatches(path, text, &cache,
                                     hinted ? &heads : nullptr) == 0);
            // The same walk again is served by the cache for what it kept.
            CHECK(inflate_mismatches(path, text, &cache,
                                     hinted ? &heads : nullptr) == 0);
        }
        std::filesystem::remove_all(dir);
    }

    TEST_CASE(
        "a byte range inside the first partial line of a piece is aligned") {
        bool mid = false, after_newline = false;
        for (std::uint32_t seed = 1; seed <= 80 && !(mid && after_newline);
             ++seed) {
            const Trace t = short_lines(std::size_t{6} << 20, 300 + seed);
            const std::string dir =
                "/tmp/dftu_bigmember_b_" + std::to_string(seed);
            std::filesystem::create_directories(dir);
            std::vector<std::uint8_t> bytes;
            append(bytes, t.text);
            write_file(dir + "/t.gz", bytes);
            Indexed ix(dir + "/t.gz", dir + "/idx");
            for (const auto& p : ix.indexer->get_members()) {
                if (p.kind != GzipRecordKind::Restart) continue;
                const std::size_t at = static_cast<std::size_t>(p.uc_offset);
                const std::size_t nl = t.text.find('\n', at);
                if (nl == std::string::npos || nl < at + 2) continue;
                CAPTURE(seed);
                CAPTURE(p.member_idx);
                const bool newline_before = t.text[at - 1] == '\n';
                // The target is inside the first line the piece holds; that
                // line is ours when it starts at the piece, else the next one.
                const std::size_t expect_from = newline_before ? at : nl + 1;
                CHECK(read_line_bytes(ix.indexer, at + 1) ==
                      t.text.substr(expect_from));
                (newline_before ? after_newline : mid) = true;
            }
            std::filesystem::remove_all(dir);
        }
        CHECK(mid);
        CHECK(after_newline);
    }

    TEST_CASE(
        "a line range reads the piece it starts in, not the whole member") {
        Files fx(BIG_BYTES, 6, "i");
        Indexed big(fx.big, fx.dir + "/idx_big");
        if (bytes_read_by_process() == 0) {
            MESSAGE("no /proc/self/io; the bytes read are not counted");
            return;
        }
        const auto pieces = big.indexer->get_members();
        REQUIRE(pieces.size() > 16);
        const auto& last = pieces.back();
        REQUIRE(last.kind == GzipRecordKind::Restart);
        const std::size_t first =
            static_cast<std::size_t>(last.first_line_num) + 3;
        REQUIRE(first + 3 < fx.trace.lines);
        const std::uint64_t before = bytes_read_by_process();
        const std::string got = big.read_lines(first, first + 2);
        const std::uint64_t used = bytes_read_by_process() - before;
        CHECK(got == lines_of(fx.trace.text, first, first + 2));
        const std::uint64_t file_bytes = std::filesystem::file_size(fx.big);
        MESSAGE("bytes read: " << used << " of a " << file_bytes << " file");
        // From the head the reader would read the whole member's input.
        CHECK(used < file_bytes / 8);
    }

    TEST_CASE("a last piece without a trailing newline reads to the end") {
        Trace t = short_lines(std::size_t{6} << 20, 77);
        t.text.pop_back();
        --t.lines;  // the last line has no newline, and the index counts none
        const std::string dir = "/tmp/dftu_bigmember_nonl";
        std::filesystem::create_directories(dir);
        std::vector<std::uint8_t> bytes;
        append(bytes, t.text);
        write_file(dir + "/t.gz", bytes);
        Indexed ix(dir + "/t.gz", dir + "/idx");
        const auto pieces = ix.indexer->get_members();
        REQUIRE(pieces.size() >= 3);
        const auto& last = pieces.back();
        CHECK(last.kind == GzipRecordKind::Restart);
        CHECK(ix.indexer->get_max_bytes() == t.text.size());
        CHECK(ix.indexer->get_num_lines() ==
              newlines_before(t.text, t.text.size()));
        const std::size_t at = static_cast<std::size_t>(last.uc_offset);
        const bool newline_before = t.text[at - 1] == '\n';
        const std::size_t from =
            newline_before ? at : t.text.find('\n', at) + 1;
        // The line-aligned stream returns whole lines only: the last line,
        // which has no newline, is not among them.
        const std::size_t last_newline = t.text.rfind('\n');
        CHECK(read_line_bytes(ix.indexer, at) ==
              t.text.substr(from, last_newline + 1 - from));
        const std::size_t numbered = ix.indexer->get_num_lines();
        CHECK(ix.read_lines(numbered - 2, numbered) ==
              lines_of(t.text, numbered - 2, numbered));
        std::filesystem::remove_all(dir);
    }
}
