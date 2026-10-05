#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/index/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <doctest/doctest.h>
#include <sys/stat.h>
#include <testing_runtime.h>
#include <testing_utilities.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::fileio;
using dftu_utils_test::run_coro;

namespace {

std::string gunzip_all(const std::string& path) {
    gzFile g = gzopen(path.c_str(), "rb");
    REQUIRE(g != nullptr);
    std::string out;
    char b[65536];
    int n;
    while ((n = gzread(g, b, sizeof(b))) > 0) out.append(b, n);
    gzclose(g);
    return out;
}

std::string line(std::size_t i, std::size_t pad = 0) {
    return "{\"id\":" + std::to_string(i) + ",\"p\":\"" +
           std::string(pad, 'x') + "\"}\n";
}

struct Written {
    std::vector<MemberInfo> members;
    WriteSummary summary;
    Result<void> err = {};
};

Written write_all(const std::string& path, GzipWriterOptions o,
                  const std::vector<std::string>& pieces,
                  bool cut_after_each = false) {
    Written w;
    std::mutex mu;
    o.on_member = [&](const MemberInfo& m) { w.members.push_back(m); };
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(path, o);
        REQUIRE(wr.has_value());
        for (auto& p : pieces) {
            auto r = co_await wr->append(p);
            if (!r) {
                w.err = r;
                co_return;
            }
            if (cut_after_each) REQUIRE(co_await wr->cut());
        }
        auto s = co_await wr->close();
        REQUIRE(s.has_value());
        w.summary = *s;
    });
    return w;
}

std::vector<index::gzip::GzipMemberRecord> index_of(const std::string& path) {
    std::optional<index::gzip::GzipBuildArtifacts> arts;
    run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
        index::gzip::CheckpointIndexer::VisitorList none;
        arts = co_await index::gzip::build_gzip_index_artifacts(path, 32u << 20,
                                                                none, &scope);
    });
    REQUIRE(arts.has_value());
    return arts->members;
}

void expect_equal_to_index(
    const std::vector<MemberInfo>& got,
    const std::vector<index::gzip::GzipMemberRecord>& want,
    std::uint64_t first_idx) {
    REQUIRE(got.size() == want.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i].index - first_idx == want[i].member_idx);
        CHECK(got[i].c_offset == want[i].c_offset);
        CHECK(got[i].c_size == want[i].c_size);
        CHECK(got[i].uc_offset == want[i].uc_offset);
        CHECK(got[i].uc_size == want[i].uc_size);
        CHECK(got[i].first_line == want[i].first_line_num);
        CHECK(got[i].first_line + got[i].lines - 1 == want[i].last_line_num);
    }
}

}  // namespace

TEST_CASE("GzipLineWriter - cut rule, long line, cut(), index equality") {
    auto path = dftu_utils_test::make_unique_test_path("glw_rule.pfw.gz");
    constexpr std::size_t MS = 1000;
    std::vector<std::string> pieces;
    std::string all;
    for (std::size_t i = 0; i < 300; ++i) {
        pieces.push_back(line(i, i % 7 * 20));
        all += pieces.back();
    }
    pieces.push_back(line(900, 5 * MS));  // longer than a member
    all += pieces.back();
    for (std::size_t i = 301; i < 330; ++i) {
        pieces.push_back(line(i, 10));
        all += pieces.back();
    }

    GzipWriterOptions o;
    o.member_size = MS;
    o.workers = 3;
    auto w = write_all(path.string(), o, pieces);
    REQUIRE(!w.err.has_value() == false);
    CHECK(gunzip_all(path.string()) == all);
    REQUIRE(w.members.size() > 10);
    CHECK(w.summary.members == w.members.size());
    CHECK(w.summary.uc_bytes == all.size());

    for (std::size_t i = 0; i < w.members.size(); ++i) {
        const auto& m = w.members[i];
        if (i + 1 < w.members.size()) {
            CHECK(all[m.uc_offset + m.uc_size - 1] == '\n');
            // No newline at or after the member size before the final one.
            for (std::size_t k = MS; k + 1 < m.uc_size; ++k)
                REQUIRE(all[m.uc_offset + k] != '\n');
            CHECK(m.uc_size > MS);
        }
    }
    bool long_alone = false;
    for (auto& m : w.members)
        if (m.uc_size > 5 * MS && m.lines == 1) long_alone = true;
    // The long line closes the member it joins; here it follows short lines.
    CHECK(std::any_of(w.members.begin(), w.members.end(),
                      [](const MemberInfo& m) { return m.uc_size > 5 * MS; }));
    (void)long_alone;
    expect_equal_to_index(w.members, index_of(path.string()), 0);
    fs::remove(path);

    // cut() ends the current member at once; a long line cut off is alone.
    GzipWriterOptions c;
    c.member_size = MS;
    c.workers = 2;
    auto w2 = write_all(path.string(), c,
                        {line(1), line(2), line(3, 5 * MS), line(4), line(5)},
                        /*cut_after_each=*/true);
    REQUIRE(w2.members.size() == 5);
    CHECK(w2.members[2].lines == 1);
    CHECK(w2.members[2].uc_size > 5 * MS);
    CHECK(w2.members[0].lines == 1);
    expect_equal_to_index(w2.members, index_of(path.string()), 0);
    fs::remove(path);
}

TEST_CASE("GzipLineWriter - order, bounded in flight, fold once") {
    auto path = dftu_utils_test::make_unique_test_path("glw_order.pfw.gz");
    constexpr std::size_t WORKERS = 8;
    std::vector<std::string> pieces;
    std::string all;
    // Uneven members: sizes cycle so compression times differ.
    for (std::size_t i = 0; i < 400; ++i) {
        pieces.push_back(line(i, (i % 13) * 700));
        all += pieces.back();
    }
    std::mutex mu;
    std::multiset<std::uint64_t> folded;
    std::atomic<int> running{0}, max_running{0};
    GzipWriterOptions o;
    o.member_size = 4000;
    o.workers = WORKERS;
    o.fold = [&](const MemberRef& ref, std::string_view pt) {
        const std::uint64_t idx = ref.index;
        int r = ++running;
        int m = max_running.load();
        while (r > m && !max_running.compare_exchange_weak(m, r)) {
        }
        std::this_thread::sleep_for(
            std::chrono::microseconds(200 + (idx * 7919) % 3000));
        {
            std::lock_guard<std::mutex> lk(mu);
            folded.insert(idx);
        }
        CHECK(!pt.empty());
        --running;
    };
    auto w = write_all(path.string(), o, pieces);
    CHECK(gunzip_all(path.string()) == all);
    REQUIRE(w.members.size() > 50);
    std::uint64_t off = 0;
    for (std::size_t i = 0; i < w.members.size(); ++i) {
        CHECK(w.members[i].index == i);
        CHECK(w.members[i].uc_offset == off);
        off += w.members[i].uc_size;
    }
    CHECK(folded.size() == w.members.size());
    for (std::size_t i = 0; i < w.members.size(); ++i)
        CHECK(folded.count(i) == 1);
    CHECK(w.summary.peak_in_flight <= WORKERS + 2);
    CHECK(w.summary.peak_in_flight >= 1);
    CHECK(max_running.load() <= static_cast<int>(WORKERS));
    expect_equal_to_index(w.members, index_of(path.string()), 0);
    fs::remove(path);

    GzipWriterOptions one;
    one.member_size = 4000;
    one.workers = 1;
    auto w1 = write_all(path.string(), one, pieces);
    CHECK(w1.summary.peak_in_flight <= 3);
    CHECK(gunzip_all(path.string()) == all);
    fs::remove(path);
}

TEST_CASE("GzipLineWriter - part files") {
    auto dir = dftu_utils_test::make_unique_test_path("glw_parts");
    fs::create_directories(dir);
    auto base = (dir / "t.pfw.gz").string();
    std::vector<std::string> pieces;
    std::string all;
    for (std::size_t i = 0; i < 200; ++i) {
        pieces.push_back(line(i, 100));
        all += pieces.back();
    }
    GzipWriterOptions o;
    o.member_size = 2000;
    o.part_size = 5000;
    o.workers = 4;
    auto w = write_all(base, o, pieces);
    REQUIRE(w.summary.parts > 3);
    std::string joined;
    std::size_t files = 0;
    for (std::size_t p = 0; p < w.summary.parts; ++p) {
        auto pp = gzip_part_path(base, static_cast<int>(p), true);
        REQUIRE(fs::exists(pp));
        ++files;
        joined += gunzip_all(pp);
        std::vector<MemberInfo> mine;
        for (auto& m : w.members)
            if (m.part == p) mine.push_back(m);
        REQUIRE(!mine.empty());
        expect_equal_to_index(mine, index_of(pp), mine.front().index);
        std::uint64_t uc = 0;
        for (auto& m : mine) uc += m.uc_size;
        if (p + 1 < w.summary.parts) CHECK(uc >= o.part_size);
    }
    CHECK(joined == all);
    std::size_t on_disk = 0;
    for (auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++on_disk;
    }
    CHECK(on_disk == files);
    fs::remove_all(dir);
}

TEST_CASE("GzipLineWriter - compress false") {
    auto path = dftu_utils_test::make_unique_test_path("glw_plain.pfw");
    std::vector<std::string> pieces;
    std::string all;
    for (std::size_t i = 0; i < 100; ++i) {
        pieces.push_back(line(i, 50));
        all += pieces.back();
    }
    GzipWriterOptions o;
    o.member_size = 1500;
    o.compress = false;
    o.workers = 3;
    auto w = write_all(path.string(), o, pieces);
    std::ifstream f(path, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    CHECK(got == all);
    REQUIRE(w.members.size() > 3);
    for (auto& m : w.members) {
        CHECK(m.c_offset == m.uc_offset);
        CHECK(m.c_size == m.uc_size);
    }
    fs::remove(path);
}

TEST_CASE("GzipLineWriter - errors") {
    SUBCASE("append without a trailing newline is refused") {
        auto path = dftu_utils_test::make_unique_test_path("glw_nonl.pfw.gz");
        GzipWriterOptions o;
        o.member_size = 100;
        Written w = write_all(path.string(), o, {"{\"a\":1}"});
        CHECK(!w.err.has_value());
        CHECK(w.err.error().code == ErrorCode::INVALID_ARGUMENT);
        fs::remove(path);
    }
    SUBCASE("a worker failure removes the output") {
        auto path = dftu_utils_test::make_unique_test_path("glw_fail.pfw.gz");
        GzipWriterOptions o;
        o.member_size = 200;
        o.workers = 2;
        o.fold = [](const MemberRef& ref, std::string_view) {
            const std::uint64_t idx = ref.index;
            if (idx == 3) throw std::runtime_error("boom");
        };
        bool saw_error = false;
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            auto wr = co_await GzipLineWriter::open(path.string(), o);
            REQUIRE(wr.has_value());
            for (std::size_t i = 0; i < 2000 && !saw_error; ++i) {
                auto r = co_await wr->append(line(i, 100));
                if (!r) saw_error = true;
            }
            if (!saw_error) {
                auto c = co_await wr->close();
                saw_error = !c.has_value();
            }
            auto again = co_await wr->append(line(1));
            CHECK(!again.has_value());
        });
        CHECK(saw_error);
        CHECK(!fs::exists(path));
    }
    SUBCASE("an unwritable directory fails open") {
        if (geteuid() == 0) return;
        auto dir = dftu_utils_test::make_unique_test_path("glw_ro");
        fs::create_directories(dir);
        chmod(dir.c_str(), 0555);
        auto path = (dir / "t.pfw.gz").string();
        bool failed = false;
        run_coro([&](CoroScope&) -> coro::CoroTask<void> {
            // Named: GCC 12 destroys a temporary built inside co_await twice.
            GzipWriterOptions defaults;
            auto wr = co_await GzipLineWriter::open(path, std::move(defaults));
            failed = !wr.has_value();
        });
        chmod(dir.c_str(), 0755);
        CHECK(failed);
        CHECK(!fs::exists(path));
        fs::remove_all(dir);
    }
}

TEST_CASE("GzipLineWriterBlocking - round trip") {
    auto path = dftu_utils_test::make_unique_test_path("glw_block.pfw.gz");
    GzipWriterOptions o;
    o.member_size = 500;
    o.workers = 2;
    std::vector<MemberInfo> members;
    o.on_member = [&](const MemberInfo& m) { members.push_back(m); };
    auto w = GzipLineWriterBlocking::open(path.string(), o);
    REQUIRE(w.has_value());
    std::string all;
    for (std::size_t i = 0; i < 100; ++i) {
        auto l = line(i, 30);
        all += l;
        REQUIRE(w->append(l));
    }
    CHECK(!w->append("no newline").has_value());
    REQUIRE(w->cut());
    auto s = w->close();
    REQUIRE(s.has_value());
    CHECK(s->members == members.size());
    CHECK(gunzip_all(path.string()) == all);
    expect_equal_to_index(members, index_of(path.string()), 0);
    fs::remove(path);
}

TEST_CASE("GzipLineWriter - destroying without close aborts") {
    auto dir = dftu_utils_test::make_unique_test_path("glw_abort");
    fs::create_directories(dir);
    auto base = (dir / "t.pfw.gz").string();
    GzipWriterOptions o;
    o.member_size = 500;
    o.part_size = 2000;
    o.workers = 3;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        {
            auto wr = co_await GzipLineWriter::open(base, o);
            REQUIRE(wr.has_value());
            for (std::size_t i = 0; i < 300; ++i)
                REQUIRE(co_await wr->append(line(i, 100)));
        }
        {
            GzipLineWriter empty;
            GzipLineWriter moved_from;
        }
        co_return;
    });
    std::size_t left = 0;
    for (auto& e : fs::directory_iterator(dir)) {
        (void)e;
        ++left;
    }
    CHECK(left == 0);
    fs::remove_all(dir);
}

TEST_CASE("GzipLineWriter - a moved writer closes once") {
    auto path = dftu_utils_test::make_unique_test_path("glw_move.pfw.gz");
    std::string all;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        GzipWriterOptions o;
        o.member_size = 300;
        auto wr = co_await GzipLineWriter::open(path.string(), o);
        REQUIRE(wr.has_value());
        GzipLineWriter a = std::move(*wr);
        GzipLineWriter b = std::move(a);
        for (std::size_t i = 0; i < 50; ++i) {
            all += line(i, 20);
            REQUIRE(co_await b.append(line(i, 20)));
        }
        auto s = co_await b.close();
        REQUIRE(s.has_value());
        co_return;
    });
    CHECK(gunzip_all(path.string()) == all);
    fs::remove(path);
}

namespace {

std::string inflate_member(const std::string& file, std::uint64_t off,
                           std::uint64_t len) {
    z_stream z{};
    REQUIRE(inflateInit2(&z, 31) == Z_OK);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(file.data())) + off;
    z.avail_in = static_cast<uInt>(len);
    std::string out;
    char b[65536];
    int rc;
    do {
        z.next_out = reinterpret_cast<Bytef*>(b);
        z.avail_out = sizeof(b);
        rc = inflate(&z, Z_NO_FLUSH);
        out.append(b, sizeof(b) - z.avail_out);
    } while (rc == Z_OK);
    inflateEnd(&z);
    REQUIRE(rc == Z_STREAM_END);
    return out;
}

std::string slurp(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    std::string out;
    char b[65536];
    std::size_t n;
    while ((n = std::fread(b, 1, sizeof(b), f)) > 0) out.append(b, n);
    std::fclose(f);
    return out;
}

}  // namespace

TEST_CASE("GzipLineWriter - close(tail) without a newline") {
    auto path = dftu_utils_test::make_unique_test_path("glw_tail.pfw.gz");
    GzipWriterOptions o;
    o.member_size = 1000;
    o.workers = 2;
    std::string all;
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(path.string(), o);
        REQUIRE(wr.has_value());
        for (std::size_t i = 0; i < 50; ++i) {
            all += line(i, 30);
            REQUIRE(co_await wr->append(line(i, 30)));
        }
        auto bad = co_await wr->append("no newline");
        CHECK(!bad.has_value());
        auto s = co_await wr->close("tail-without-newline");
        REQUIRE(s.has_value());
        CHECK(s->uc_bytes == all.size() + 20);
    });
    CHECK(gunzip_all(path.string()) == all + "tail-without-newline");
    fs::remove(path);

    // Only a tail: it forms its own member.
    auto only = dftu_utils_test::make_unique_test_path("glw_tail2.pfw.gz");
    auto w = GzipLineWriterBlocking::open(only.string(), o);
    REQUIRE(w.has_value());
    auto s = w->close("abc");
    REQUIRE(s.has_value());
    CHECK(s->members == 1);
    CHECK(gunzip_all(only.string()) == "abc");
    fs::remove(only);
}

TEST_CASE("GzipLineWriter - named parts, header, footer, on_part") {
    auto dir = dftu_utils_test::make_unique_test_path("glw_named");
    fs::create_directories(dir);
    std::vector<std::string> pieces;
    for (std::size_t i = 0; i < 100; ++i) pieces.push_back(line(i, 100));

    struct Part {
        std::size_t part;
        std::string path;
        std::uint64_t uc, lines;
    };
    std::vector<Part> parts;
    GzipWriterOptions o;
    o.member_size = 1500;
    o.part_size = 4000;
    o.workers = 3;
    o.part_header = "[\n";
    o.part_footer = "\n]\n";
    o.part_name = [&](std::size_t i) {
        return (dir / ("chunk" + std::to_string(i) + ".pfw.gz")).string();
    };
    o.on_part = [&](std::size_t p, const std::string& path, std::uint64_t uc,
                    std::uint64_t lines) {
        CHECK(fs::exists(path));
        parts.push_back({p, path, uc, lines});
    };
    auto w = write_all((dir / "ignored.pfw.gz").string(), o, pieces);
    REQUIRE(w.summary.parts > 2);
    REQUIRE(parts.size() == w.summary.parts);
    CHECK(!fs::exists(dir / "ignored.pfw.gz"));

    std::uint64_t total_lines = 0;
    for (std::size_t p = 0; p < parts.size(); ++p) {
        CHECK(parts[p].part == p);
        CHECK(parts[p].path ==
              (dir / ("chunk" + std::to_string(p) + ".pfw.gz")).string());
        const std::string file = slurp(parts[p].path);
        const std::string text = gunzip_all(parts[p].path);
        REQUIRE(text.size() == parts[p].uc + 5);
        CHECK(text.rfind("[\n", 0) == 0);
        CHECK(text.substr(text.size() - 3) == "\n]\n");
        total_lines += parts[p].lines;

        // The index sees the header and footer as members of their own;
        // data members sit between them and their offsets point at the data.
        auto idx = index_of(parts[p].path);
        std::vector<MemberInfo> mine;
        for (auto& m : w.members)
            if (m.part == p) mine.push_back(m);
        REQUIRE(idx.size() == mine.size() + 2);
        std::uint64_t uc = 0;
        for (std::size_t i = 0; i < mine.size(); ++i) {
            CHECK(mine[i].c_offset == idx[i + 1].c_offset);
            CHECK(mine[i].uc_offset == uc);
            CHECK(inflate_member(file, mine[i].c_offset, mine[i].c_size) ==
                  text.substr(2 + uc, mine[i].uc_size));
            uc += mine[i].uc_size;
        }
        CHECK(uc == parts[p].uc);
    }
    CHECK(total_lines == pieces.size());
    CHECK(w.summary.lines == pieces.size());

    // One part: header and footer still apply.
    auto one = (dir / "one.pfw").string();
    GzipWriterOptions o1;
    o1.part_header = "H\n";
    o1.part_footer = "F\n";
    o1.compress = false;
    auto w1 = write_all(one, o1, {"a\n", "b\n"});
    CHECK(slurp(one) == "H\na\nb\nF\n");
    CHECK(w1.summary.lines == 2);
    fs::remove_all(dir);
}

TEST_CASE("GzipLineWriter - fold sees each member's decoded bytes once") {
    auto path = dftu_utils_test::make_unique_test_path("glw_plain.pfw.gz");
    std::vector<MemberInfo> members;
    std::mutex mu;
    std::map<std::uint64_t, std::string> folded;
    int dup = 0;
    GzipWriterOptions o;
    o.member_size = 700;
    o.workers = 4;
    o.on_member = [&](const MemberInfo& m) { members.push_back(m); };
    o.fold = [&](const MemberRef& ref, std::string_view pt) {
        const std::uint64_t idx = ref.index;
        std::lock_guard<std::mutex> lk(mu);
        if (!folded.emplace(idx, std::string(pt)).second) ++dup;
    };
    std::vector<std::string> pieces;
    for (std::size_t i = 0; i < 200; ++i) pieces.push_back(line(i, 40));
    write_all(path.string(), o, pieces);
    (void)members;
    CHECK(dup == 0);
    REQUIRE(folded.size() > 5);
    const std::string file = slurp(path.string());
    auto idx = index_of(path.string());
    REQUIRE(idx.size() == folded.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        CHECK(inflate_member(file, idx[i].c_offset, idx[i].c_size) ==
              folded[i]);
    fs::remove(path);
}

TEST_CASE(
    "GzipLineWriter - fold names the part and index each member lands in") {
    auto path = dftu_utils_test::make_unique_test_path("glw_ref.pfw.gz");
    std::mutex mu;
    std::map<std::uint64_t, MemberRef> refs;
    GzipWriterOptions o;
    o.member_size = 700;
    o.part_size = 3000;
    o.workers = 4;
    o.fold = [&](const MemberRef& ref, std::string_view) {
        std::lock_guard<std::mutex> lk(mu);
        refs.emplace(ref.index, ref);
    };
    std::vector<std::string> pieces;
    for (std::size_t i = 0; i < 200; ++i) pieces.push_back(line(i, 40));
    auto w = write_all(path.string(), o, pieces);
    REQUIRE(w.summary.parts > 2);
    REQUIRE(refs.size() == w.members.size());
    std::map<std::size_t, std::uint64_t> in_part;
    for (const auto& m : w.members) {
        const auto& r = refs.at(m.index);
        CHECK(r.part == m.part);
        CHECK(r.part_index == in_part[m.part]++);
    }
    std::error_code ec;
    for (std::size_t i = 0; i < w.summary.parts; ++i)
        fs::remove(gzip_part_path(path.string(), static_cast<int>(i), true),
                   ec);
}

TEST_CASE("GzipLineWriter - append_with equals append") {
    auto a = dftu_utils_test::make_unique_test_path("glw_aw_a.pfw.gz");
    auto b = dftu_utils_test::make_unique_test_path("glw_aw_b.pfw.gz");
    GzipWriterOptions o;
    o.member_size = 500;
    o.workers = 2;
    std::vector<std::string> pieces;
    for (std::size_t i = 0; i < 150; ++i) pieces.push_back(line(i, i % 50));
    write_all(a.string(), o, pieces);
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(b.string(), o);
        REQUIRE(wr.has_value());
        for (auto& p : pieces) {
            const auto r = co_await wr->append_with(
                p.size() + 16, [&](char* dst, std::size_t cap) {
                    CHECK(cap >= p.size() + 16);
                    std::memcpy(dst, p.data(), p.size());
                    return p.size();
                });
            REQUIRE(r);
        }
        auto bad = co_await wr->append_with(8, [](char* dst, std::size_t) {
            dst[0] = 'x';
            return std::size_t{1};
        });
        CHECK(!bad.has_value());
        REQUIRE(co_await wr->close());
    });
    CHECK(slurp(a.string()) == slurp(b.string()));
    fs::remove(a);
    fs::remove(b);
}

TEST_CASE("GzipLineWriter - unordered mode") {
    auto path = dftu_utils_test::make_unique_test_path("glw_unord.pfw.gz");
    constexpr std::size_t PRODUCERS = 4, PER = 300;
    GzipWriterOptions bad;
    bad.ordered = false;
    bad.on_member = [](const MemberInfo&) {};
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        auto r = co_await GzipLineWriter::open(path.string(), bad);
        CHECK(!r.has_value());
        GzipWriterOptions b2;
        b2.ordered = false;
        b2.part_size = 10;
        CHECK(!(co_await GzipLineWriter::open(path.string(), b2)).has_value());
    });

    GzipWriterOptions o;
    o.ordered = false;
    o.member_size = 1000;
    o.workers = 4;
    o.part_header = "[\n";
    o.part_footer = "]\n";
    std::multiset<std::string> expect;
    for (std::size_t p = 0; p < PRODUCERS; ++p)
        for (std::size_t i = 0; i < PER; ++i)
            expect.insert(line(p * 1000 + i, i % 30));
    WriteSummary sum;
    run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(path.string(), o);
        REQUIRE(wr.has_value());
        GzipLineWriter& w = *wr;
        coro::CoroSemaphore done(PRODUCERS);
        co_await done.acquire(PRODUCERS);
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            scope.spawn([&, p](CoroScope&) -> coro::CoroTask<void> {
                for (std::size_t i = 0; i < PER; ++i) {
                    auto l = line(p * 1000 + i, i % 30);
                    if (p % 2 == 0) {
                        REQUIRE(co_await w.append(l));
                    } else {
                        REQUIRE(co_await w.append_with(
                            l.size(), [&](char* dst, std::size_t) {
                                std::memcpy(dst, l.data(), l.size());
                                return l.size();
                            }));
                    }
                }
                done.release(1);
            });
        }
        for (std::size_t p = 0; p < PRODUCERS; ++p) co_await done.acquire(1);
        auto s = co_await w.close();
        REQUIRE(s.has_value());
        sum = *s;
    });
    const std::string text = gunzip_all(path.string());
    REQUIRE(text.size() > 4);
    CHECK(text.substr(0, 2) == "[\n");
    CHECK(text.substr(text.size() - 2) == "]\n");
    std::multiset<std::string> got;
    std::size_t pos = 2;
    while (pos < text.size() - 2) {
        auto nl = text.find('\n', pos);
        got.insert(text.substr(pos, nl + 1 - pos));
        pos = nl + 1;
    }
    CHECK(got == expect);
    CHECK(sum.lines == expect.size());
    for (auto& m : index_of(path.string())) {
        const std::string file = slurp(path.string());
        auto pt = inflate_member(file, m.c_offset, m.c_size);
        CHECK(pt.back() == '\n');
    }
    fs::remove(path);
}

TEST_CASE("GzipLineWriter - producers fill their own members") {
    auto path = dftu_utils_test::make_unique_test_path("glw_prod.pfw.gz");
    constexpr std::size_t PRODUCERS = 4, PER = 300, MEMBER = 1000;
    GzipWriterOptions o;
    o.ordered = false;
    o.member_size = MEMBER;
    o.workers = 4;
    std::mutex mu;
    std::vector<std::pair<char, std::size_t>> members;
    o.fold = [&](const MemberRef&, std::string_view pt) {
        std::lock_guard<std::mutex> lk(mu);
        members.emplace_back(pt[0], pt.size());
    };
    auto mk = [](std::size_t p, std::size_t i) {
        return std::string(1, static_cast<char>('A' + p)) + line(i, i % 30);
    };
    std::multiset<std::string> expect;
    for (std::size_t p = 0; p < PRODUCERS; ++p)
        for (std::size_t i = 0; i < PER; ++i) expect.insert(mk(p, i));
    run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(path.string(), o);
        REQUIRE(wr.has_value());
        GzipLineWriter& w = *wr;
        std::vector<GzipLineWriter::Producer> prods;
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            auto pr = w.producer();
            REQUIRE(pr.has_value());
            prods.push_back(std::move(*pr));
        }
        coro::CoroSemaphore done(PRODUCERS);
        co_await done.acquire(PRODUCERS);
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            scope.spawn([&, p](CoroScope&) -> coro::CoroTask<void> {
                for (std::size_t i = 0; i < PER; ++i) {
                    auto l = mk(p, i);
                    if (p % 2 == 0) {
                        REQUIRE(co_await prods[p].append(l));
                    } else {
                        REQUIRE(co_await prods[p].append_with(
                            l.size(), [&](char* dst, std::size_t) {
                                std::memcpy(dst, l.data(), l.size());
                                return l.size();
                            }));
                    }
                }
                REQUIRE(co_await prods[p].flush());
                done.release(1);
            });
        }
        for (std::size_t p = 0; p < PRODUCERS; ++p) co_await done.acquire(1);
        REQUIRE(co_await w.close());
    });
    const std::string text = gunzip_all(path.string());
    std::multiset<std::string> got;
    for (std::size_t pos = 0; pos < text.size();) {
        auto nl = text.find('\n', pos);
        got.insert(text.substr(pos, nl + 1 - pos));
        pos = nl + 1;
    }
    CHECK(got == expect);
    std::map<char, std::size_t> small;
    for (auto& [tag, size] : members)
        if (size < MEMBER) ++small[tag];
    for (auto& [tag, n] : small) CHECK(n <= 1);
    const std::string file = slurp(path.string());
    for (auto& m : index_of(path.string()))
        CHECK(inflate_member(file, m.c_offset, m.c_size).back() == '\n');
    fs::remove(path);
}

TEST_CASE("GzipLineWriter - producer refused when ordered, close needs flush") {
    auto path = dftu_utils_test::make_unique_test_path("glw_prod2.pfw.gz");
    run_coro([&](CoroScope&) -> coro::CoroTask<void> {
        GzipWriterOptions o;
        auto wr = co_await GzipLineWriter::open(path.string(), o);
        REQUIRE(wr.has_value());
        CHECK(!wr->producer().has_value());
        REQUIRE(co_await wr->close());
        o.ordered = false;
        auto w2 = co_await GzipLineWriter::open(path.string(), o);
        REQUIRE(w2.has_value());
        auto pr = w2->producer();
        REQUIRE(pr.has_value());
        REQUIRE(co_await pr->append(line(1)));
        CHECK(!(co_await w2->close()).has_value());
    });
    CHECK(!fs::exists(path));
}

TEST_CASE("GzipLineWriter - producers stay within the memory budget") {
    auto path = dftu_utils_test::make_unique_test_path("glw_budget.pfw.gz");
    constexpr std::size_t PRODUCERS = 8, PER = 4000, MEMBER = 64 * 1024;
    GzipWriterOptions o;
    o.ordered = false;
    o.member_size = MEMBER;
    o.workers = 2;
    // Each compressed bound is about one member, so this holds about 8
    // members of both kinds in total.
    o.memory_budget = 4 * 2 * (MEMBER + MEMBER / 8);
    std::uint64_t expect_lines = PRODUCERS * PER;
    WriteSummary sum;
    run_coro([&](CoroScope& scope) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(path.string(), o);
        REQUIRE(wr.has_value());
        GzipLineWriter& w = *wr;
        std::vector<GzipLineWriter::Producer> prods;
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            auto pr = w.producer();
            REQUIRE(pr.has_value());
            prods.push_back(std::move(*pr));
        }
        coro::CoroSemaphore done(PRODUCERS);
        co_await done.acquire(PRODUCERS);
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            scope.spawn([&, p](CoroScope&) -> coro::CoroTask<void> {
                for (std::size_t i = 0; i < PER; ++i)
                    REQUIRE(co_await prods[p].append(line(p * PER + i, 40)));
                REQUIRE(co_await prods[p].flush());
                done.release(1);
            });
        }
        for (std::size_t p = 0; p < PRODUCERS; ++p) co_await done.acquire(1);
        auto s = co_await w.close();
        REQUIRE(s.has_value());
        sum = *s;
    });
    CHECK(sum.peak_bytes <= o.memory_budget);
    CHECK(sum.peak_in_flight <= 4);
    CHECK(sum.lines == expect_lines);
    const std::string text = gunzip_all(path.string());
    CHECK(static_cast<std::uint64_t>(
              std::count(text.begin(), text.end(), '\n')) == expect_lines);
    fs::remove(path);
}
