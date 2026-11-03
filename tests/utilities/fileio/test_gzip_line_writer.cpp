// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
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
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::utilities::fileio;
using dftu_utils_test::run_coro;

namespace {

dstd::String gunzip_all(const dstd::String& path) {
    gzFile g = gzopen(path.c_str(), "rb");
    REQUIRE(g != nullptr);
    dstd::String out;
    char b[65536];
    int n;
    while ((n = gzread(g, b, sizeof(b))) > 0) out.append(b, n);
    gzclose(g);
    return out;
}

dstd::String line(std::size_t i, std::size_t pad = 0) {
    return "{\"id\":" + dstd::to_string(i) + ",\"p\":\"" +
           dstd::String(pad, 'x') + "\"}\n";
}

struct Written {
    dstd::Vector<MemberInfo> members;
    WriteSummary summary;
    dstd::Result<void> err = {};
};

Written write_all(const dstd::String& path, GzipWriterOptions o,
                  const dstd::Vector<dstd::String>& pieces,
                  bool cut_after_each = false) {
    Written w;
    std::mutex mu;
    o.on_member = [&](const MemberInfo& m) { w.members.push_back(m); };
    run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(path, std::move(o));
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

dstd::Vector<index::gzip::GzipMemberRecord> index_of(const dstd::String& path) {
    const dstd::String path_std(path);
    dstd::Optional<index::gzip::GzipBuildArtifacts> arts;
    run_coro([&](coro::Scope& scope) -> coro::CoroTask<void> {
        index::gzip::CheckpointIndexer::VisitorList none;
        arts = co_await index::gzip::build_gzip_index_artifacts(
            path_std, 32u << 20, none, &scope);
    });
    REQUIRE(arts.has_value());
    return dstd::Vector<index::gzip::GzipMemberRecord>(arts->members.begin(),
                                                       arts->members.end());
}

void expect_equal_to_index(
    const dstd::Vector<MemberInfo>& got,
    const dstd::Vector<index::gzip::GzipMemberRecord>& want,
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
    dstd::Vector<dstd::String> pieces;
    dstd::String all;
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
    auto w = write_all(dstd::String(path.str()), std::move(o), pieces);
    REQUIRE(!w.err.has_value() == false);
    CHECK(gunzip_all(dstd::String(path.str())) == all);
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
    expect_equal_to_index(w.members, index_of(dstd::String(path.str())), 0);
    (void)dstd::fs::remove(path);

    // cut() ends the current member at once; a long line cut off is alone.
    GzipWriterOptions c;
    c.member_size = MS;
    c.workers = 2;
    auto w2 = write_all(dstd::String(path.str()), std::move(c),
                        {line(1), line(2), line(3, 5 * MS), line(4), line(5)},
                        /*cut_after_each=*/true);
    REQUIRE(w2.members.size() == 5);
    CHECK(w2.members[2].lines == 1);
    CHECK(w2.members[2].uc_size > 5 * MS);
    CHECK(w2.members[0].lines == 1);
    expect_equal_to_index(w2.members, index_of(dstd::String(path.str())), 0);
    (void)dstd::fs::remove(path);
}

TEST_CASE("GzipLineWriter - order, bounded in flight, fold once") {
    auto path = dftu_utils_test::make_unique_test_path("glw_order.pfw.gz");
    constexpr std::size_t WORKERS = 8;
    dstd::Vector<dstd::String> pieces;
    dstd::String all;
    // Uneven members: sizes cycle so compression times differ.
    for (std::size_t i = 0; i < 400; ++i) {
        pieces.push_back(line(i, (i % 13) * 700));
        all += pieces.back();
    }
    std::mutex mu;
    dstd::MultiSet<std::uint64_t> folded;
    std::atomic<int> running{0}, max_running{0};
    GzipWriterOptions o;
    o.member_size = 4000;
    o.workers = WORKERS;
    o.fold = [&](const MemberRef& ref, dstd::StringView pt) {
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
    auto w = write_all(dstd::String(path.str()), std::move(o), pieces);
    CHECK(gunzip_all(dstd::String(path.str())) == all);
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
    expect_equal_to_index(w.members, index_of(dstd::String(path.str())), 0);
    (void)dstd::fs::remove(path);

    GzipWriterOptions one;
    one.member_size = 4000;
    one.workers = 1;
    auto w1 = write_all(dstd::String(path.str()), std::move(one), pieces);
    CHECK(w1.summary.peak_in_flight <= 3);
    CHECK(gunzip_all(dstd::String(path.str())) == all);
    (void)dstd::fs::remove(path);
}

TEST_CASE("GzipLineWriter - part files") {
    auto dir = dftu_utils_test::make_unique_test_path("glw_parts");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String base((dir / "t.pfw.gz").str());
    dstd::Vector<dstd::String> pieces;
    dstd::String all;
    for (std::size_t i = 0; i < 200; ++i) {
        pieces.push_back(line(i, 100));
        all += pieces.back();
    }
    GzipWriterOptions o;
    o.member_size = 2000;
    o.part_size = 5000;
    o.workers = 4;
    const std::size_t part_size = o.part_size;
    auto w = write_all(dstd::String(base), std::move(o), pieces);
    REQUIRE(w.summary.parts > 3);
    dstd::String joined;
    std::size_t files = 0;
    for (std::size_t p = 0; p < w.summary.parts; ++p) {
        auto pp = gzip_part_path(dstd::String(base), static_cast<int>(p), true);
        REQUIRE(dstd::fs::exists(pp).value());
        ++files;
        joined += gunzip_all(pp);
        dstd::Vector<MemberInfo> mine;
        for (auto& m : w.members)
            if (m.part == p) mine.push_back(m);
        REQUIRE(!mine.empty());
        expect_equal_to_index(mine, index_of(pp), mine.front().index);
        std::uint64_t uc = 0;
        for (auto& m : mine) uc += m.uc_size;
        if (p + 1 < w.summary.parts) CHECK(uc >= part_size);
    }
    CHECK(joined == all);
    std::size_t on_disk = 0;
    for (auto& e : dftu_utils_test::list_dir(dir)) {
        (void)e;
        ++on_disk;
    }
    CHECK(on_disk == files);
    (void)dstd::fs::remove_all(dir);
}

TEST_CASE("GzipLineWriter - compress false") {
    auto path = dftu_utils_test::make_unique_test_path("glw_plain.pfw");
    dstd::Vector<dstd::String> pieces;
    dstd::String all;
    for (std::size_t i = 0; i < 100; ++i) {
        pieces.push_back(line(i, 50));
        all += pieces.back();
    }
    GzipWriterOptions o;
    o.member_size = 1500;
    o.compress = false;
    o.workers = 3;
    auto w = write_all(dstd::String(path.str()), std::move(o), pieces);
    dstd::String got =
        dstd::fs::read_file(dstd::PathView(path)).value_or(dstd::String());
    CHECK(got == all);
    REQUIRE(w.members.size() > 3);
    for (auto& m : w.members) {
        CHECK(m.c_offset == m.uc_offset);
        CHECK(m.c_size == m.uc_size);
    }
    (void)dstd::fs::remove(path);
}

TEST_CASE("GzipLineWriter - errors") {
    SUBCASE("append without a trailing newline is refused") {
        auto path = dftu_utils_test::make_unique_test_path("glw_nonl.pfw.gz");
        GzipWriterOptions o;
        o.member_size = 100;
        Written w =
            write_all(dstd::String(path.str()), std::move(o), {"{\"a\":1}"});
        CHECK(!w.err.has_value());
        CHECK(w.err.error().is_code(common::ErrorCode::InvalidArgument));
        (void)dstd::fs::remove(path);
    }
    SUBCASE("a worker failure removes the output") {
        auto path = dftu_utils_test::make_unique_test_path("glw_fail.pfw.gz");
        GzipWriterOptions o;
        o.member_size = 200;
        o.workers = 2;
        o.fold = [](const MemberRef& ref, dstd::StringView) {
            const std::uint64_t idx = ref.index;
            if (idx == 3) throw std::runtime_error("boom");
        };
        bool saw_error = false;
        run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            auto wr = co_await GzipLineWriter::open(dstd::String(path.str()),
                                                    std::move(o));
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
        CHECK(!dstd::fs::exists(path).value());
    }
    SUBCASE("an unwritable directory fails open") {
        if (geteuid() == 0) return;
        auto dir = dftu_utils_test::make_unique_test_path("glw_ro");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        chmod(dir.c_str(), 0555);
        const dstd::String path((dir / "t.pfw.gz").str());
        bool failed = false;
        run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
            // Named: GCC 12 destroys a temporary built inside co_await twice.
            GzipWriterOptions defaults;
            auto wr = co_await GzipLineWriter::open(dstd::String(path),
                                                    std::move(defaults));
            failed = !wr.has_value();
        });
        chmod(dir.c_str(), 0755);
        CHECK(failed);
        CHECK(!dstd::fs::exists(path).value());
        (void)dstd::fs::remove_all(dir);
    }
}

TEST_CASE("GzipLineWriterBlocking - round trip") {
    auto path = dftu_utils_test::make_unique_test_path("glw_block.pfw.gz");
    GzipWriterOptions o;
    o.member_size = 500;
    o.workers = 2;
    dstd::Vector<MemberInfo> members;
    o.on_member = [&](const MemberInfo& m) { members.push_back(m); };
    auto w =
        GzipLineWriterBlocking::open(dstd::String(path.str()), std::move(o));
    REQUIRE(w.has_value());
    dstd::String all;
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
    CHECK(gunzip_all(dstd::String(path.str())) == all);
    expect_equal_to_index(members, index_of(dstd::String(path.str())), 0);
    (void)dstd::fs::remove(path);
}

TEST_CASE("GzipLineWriter - destroying without close aborts") {
    auto dir = dftu_utils_test::make_unique_test_path("glw_abort");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    const dstd::String base((dir / "t.pfw.gz").str());
    GzipWriterOptions o;
    o.member_size = 500;
    o.part_size = 2000;
    o.workers = 3;
    run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        {
            auto wr =
                co_await GzipLineWriter::open(dstd::String(base), std::move(o));
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
    for (auto& e : dftu_utils_test::list_dir(dir)) {
        (void)e;
        ++left;
    }
    CHECK(left == 0);
    (void)dstd::fs::remove_all(dir);
}

TEST_CASE("GzipLineWriter - a moved writer closes once") {
    auto path = dftu_utils_test::make_unique_test_path("glw_move.pfw.gz");
    dstd::String all;
    run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        GzipWriterOptions o;
        o.member_size = 300;
        auto wr = co_await GzipLineWriter::open(dstd::String(path.str()),
                                                std::move(o));
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
    CHECK(gunzip_all(dstd::String(path.str())) == all);
    (void)dstd::fs::remove(path);
}

namespace {

dstd::String inflate_member(const dstd::String& file, std::uint64_t off,
                            std::uint64_t len) {
    z_stream z{};
    REQUIRE(inflateInit2(&z, 31) == Z_OK);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(file.data())) + off;
    z.avail_in = static_cast<uInt>(len);
    dstd::String out;
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

dstd::String slurp(const dstd::String& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    dstd::String out;
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
    dstd::String all;
    run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(dstd::String(path.str()),
                                                std::move(o));
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
    CHECK(gunzip_all(dstd::String(path.str())) == all + "tail-without-newline");
    (void)dstd::fs::remove(path);

    // Only a tail: it forms its own member.
    auto only = dftu_utils_test::make_unique_test_path("glw_tail2.pfw.gz");
    GzipWriterOptions o2;
    o2.member_size = 1000;
    o2.workers = 2;
    auto w =
        GzipLineWriterBlocking::open(dstd::String(only.str()), std::move(o2));
    REQUIRE(w.has_value());
    auto s = w->close("abc");
    REQUIRE(s.has_value());
    CHECK(s->members == 1);
    CHECK(gunzip_all(dstd::String(only.str())) == "abc");
    (void)dstd::fs::remove(only);
}

TEST_CASE("GzipLineWriter - named parts, header, footer, on_part") {
    auto dir = dftu_utils_test::make_unique_test_path("glw_named");
    REQUIRE(dstd::fs::create_directories(dir).has_value());
    dstd::Vector<dstd::String> pieces;
    for (std::size_t i = 0; i < 100; ++i) pieces.push_back(line(i, 100));

    struct Part {
        std::size_t part;
        dstd::String path;
        std::uint64_t uc, lines;
    };
    dstd::Vector<Part> parts;
    GzipWriterOptions o;
    o.member_size = 1500;
    o.part_size = 4000;
    o.workers = 3;
    o.part_header = "[\n";
    o.part_footer = "\n]\n";
    o.part_name = [&](std::size_t i) {
        return dstd::String(
            (dir / ("chunk" + dstd::to_string(i) + ".pfw.gz")).str());
    };
    o.on_part = [&](std::size_t p, dstd::StringView path, std::uint64_t uc,
                    std::uint64_t lines) {
        CHECK(dstd::fs::exists(path).value());
        parts.push_back({p, dstd::String(path), uc, lines});
    };
    auto w = write_all(dstd::String((dir / "ignored.pfw.gz").str()),
                       std::move(o), pieces);
    REQUIRE(w.summary.parts > 2);
    REQUIRE(parts.size() == w.summary.parts);
    CHECK(!dstd::fs::exists(dir / "ignored.pfw.gz").value());

    std::uint64_t total_lines = 0;
    for (std::size_t p = 0; p < parts.size(); ++p) {
        CHECK(parts[p].part == p);
        CHECK(parts[p].path ==
              (dir / ("chunk" + dstd::to_string(p) + ".pfw.gz")).str());
        const dstd::String file = slurp(parts[p].path);
        const dstd::String text = gunzip_all(parts[p].path);
        REQUIRE(text.size() == parts[p].uc + 5);
        CHECK(text.rfind("[\n", 0) == 0);
        CHECK(text.substr(text.size() - 3) == "\n]\n");
        total_lines += parts[p].lines;

        // The index sees the header and footer as members of their own;
        // data members sit between them and their offsets point at the data.
        auto idx = index_of(parts[p].path);
        dstd::Vector<MemberInfo> mine;
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
    const dstd::String one((dir / "one.pfw").str());
    GzipWriterOptions o1;
    o1.part_header = "H\n";
    o1.part_footer = "F\n";
    o1.compress = false;
    auto w1 = write_all(dstd::String(one), std::move(o1), {"a\n", "b\n"});
    CHECK(slurp(dstd::String(one)) == "H\na\nb\nF\n");
    CHECK(w1.summary.lines == 2);
    (void)dstd::fs::remove_all(dir);
}

TEST_CASE("GzipLineWriter - fold sees each member's decoded bytes once") {
    auto path = dftu_utils_test::make_unique_test_path("glw_plain.pfw.gz");
    dstd::Vector<MemberInfo> members;
    std::mutex mu;
    dstd::Map<std::uint64_t, dstd::String> folded;
    int dup = 0;
    GzipWriterOptions o;
    o.member_size = 700;
    o.workers = 4;
    o.on_member = [&](const MemberInfo& m) { members.push_back(m); };
    o.fold = [&](const MemberRef& ref, dstd::StringView pt) {
        const std::uint64_t idx = ref.index;
        std::lock_guard<std::mutex> lk(mu);
        if (!folded.emplace(idx, dstd::String(pt)).second) ++dup;
    };
    dstd::Vector<dstd::String> pieces;
    for (std::size_t i = 0; i < 200; ++i) pieces.push_back(line(i, 40));
    write_all(dstd::String(path.str()), std::move(o), pieces);
    (void)members;
    CHECK(dup == 0);
    REQUIRE(folded.size() > 5);
    const dstd::String file = slurp(dstd::String(path.str()));
    auto idx = index_of(dstd::String(path.str()));
    REQUIRE(idx.size() == folded.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        CHECK(inflate_member(file, idx[i].c_offset, idx[i].c_size) ==
              folded[i]);
    (void)dstd::fs::remove(path);
}

TEST_CASE(
    "GzipLineWriter - fold names the part and index each member lands in") {
    auto path = dftu_utils_test::make_unique_test_path("glw_ref.pfw.gz");
    std::mutex mu;
    dstd::Map<std::uint64_t, MemberRef> refs;
    GzipWriterOptions o;
    o.member_size = 700;
    o.part_size = 3000;
    o.workers = 4;
    o.fold = [&](const MemberRef& ref, dstd::StringView) {
        std::lock_guard<std::mutex> lk(mu);
        refs.emplace(ref.index, ref);
    };
    dstd::Vector<dstd::String> pieces;
    for (std::size_t i = 0; i < 200; ++i) pieces.push_back(line(i, 40));
    auto w = write_all(dstd::String(path.str()), std::move(o), pieces);
    REQUIRE(w.summary.parts > 2);
    REQUIRE(refs.size() == w.members.size());
    dstd::Map<std::size_t, std::uint64_t> in_part;
    for (const auto& m : w.members) {
        const auto& r = refs.at(m.index);
        CHECK(r.part == m.part);
        CHECK(r.part_index == in_part[m.part]++);
    }
    for (std::size_t i = 0; i < w.summary.parts; ++i)
        dstd::fs::remove(
            gzip_part_path(dstd::String(path.str()), static_cast<int>(i), true))
            .has_value();
}

TEST_CASE("GzipLineWriter - append_with equals append") {
    auto a = dftu_utils_test::make_unique_test_path("glw_aw_a.pfw.gz");
    auto b = dftu_utils_test::make_unique_test_path("glw_aw_b.pfw.gz");
    GzipWriterOptions o;
    o.member_size = 500;
    o.workers = 2;
    dstd::Vector<dstd::String> pieces;
    for (std::size_t i = 0; i < 150; ++i) pieces.push_back(line(i, i % 50));
    GzipWriterOptions o2;
    o2.member_size = 500;
    o2.workers = 2;
    write_all(dstd::String(a.str()), std::move(o), pieces);
    run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        auto wr =
            co_await GzipLineWriter::open(dstd::String(b.str()), std::move(o2));
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
    CHECK(slurp(dstd::String(a.str())) == slurp(dstd::String(b.str())));
    (void)dstd::fs::remove(a);
    (void)dstd::fs::remove(b);
}

TEST_CASE("GzipLineWriter - unordered mode") {
    auto path = dftu_utils_test::make_unique_test_path("glw_unord.pfw.gz");
    constexpr std::size_t PRODUCERS = 4, PER = 300;
    GzipWriterOptions bad;
    bad.ordered = false;
    bad.on_member = [](const MemberInfo&) {};
    run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        auto r = co_await GzipLineWriter::open(dstd::String(path.str()),
                                               std::move(bad));
        CHECK(!r.has_value());
        GzipWriterOptions b2;
        b2.ordered = false;
        b2.part_size = 10;
        CHECK(!(co_await GzipLineWriter::open(dstd::String(path.str()),
                                              std::move(b2)))
                   .has_value());
    });

    GzipWriterOptions o;
    o.ordered = false;
    o.member_size = 1000;
    o.workers = 4;
    o.part_header = "[\n";
    o.part_footer = "]\n";
    dstd::MultiSet<dstd::String> expect;
    for (std::size_t p = 0; p < PRODUCERS; ++p)
        for (std::size_t i = 0; i < PER; ++i)
            expect.insert(line(p * 1000 + i, i % 30));
    WriteSummary sum;
    run_coro([&](coro::Scope& scope) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(dstd::String(path.str()),
                                                std::move(o));
        REQUIRE(wr.has_value());
        GzipLineWriter& w = *wr;
        coro::CoroSemaphore done(PRODUCERS);
        co_await done.acquire(PRODUCERS);
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            scope.spawn([&, p]() -> coro::CoroTask<void> {
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
    const dstd::String text = gunzip_all(dstd::String(path.str()));
    REQUIRE(text.size() > 4);
    CHECK(text.substr(0, 2) == "[\n");
    CHECK(text.substr(text.size() - 2) == "]\n");
    dstd::MultiSet<dstd::String> got;
    std::size_t pos = 2;
    while (pos < text.size() - 2) {
        auto nl = text.find('\n', pos);
        got.insert(text.substr(pos, nl + 1 - pos));
        pos = nl + 1;
    }
    CHECK(got == expect);
    CHECK(sum.lines == expect.size());
    for (auto& m : index_of(dstd::String(path.str()))) {
        const dstd::String file = slurp(dstd::String(path.str()));
        auto pt = inflate_member(file, m.c_offset, m.c_size);
        CHECK(pt.back() == '\n');
    }
    (void)dstd::fs::remove(path);
}

TEST_CASE("GzipLineWriter - producers fill their own members") {
    auto path = dftu_utils_test::make_unique_test_path("glw_prod.pfw.gz");
    constexpr std::size_t PRODUCERS = 4, PER = 300, MEMBER = 1000;
    GzipWriterOptions o;
    o.ordered = false;
    o.member_size = MEMBER;
    o.workers = 4;
    std::mutex mu;
    dstd::Vector<dstd::Pair<char, std::size_t>> members;
    o.fold = [&](const MemberRef&, dstd::StringView pt) {
        std::lock_guard<std::mutex> lk(mu);
        members.emplace_back(pt[0], pt.size());
    };
    auto mk = [](std::size_t p, std::size_t i) {
        return dstd::String(1, static_cast<char>('A' + p)) + line(i, i % 30);
    };
    dstd::MultiSet<dstd::String> expect;
    for (std::size_t p = 0; p < PRODUCERS; ++p)
        for (std::size_t i = 0; i < PER; ++i) expect.insert(mk(p, i));
    run_coro([&](coro::Scope& scope) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(dstd::String(path.str()),
                                                std::move(o));
        REQUIRE(wr.has_value());
        GzipLineWriter& w = *wr;
        dstd::Vector<GzipLineWriter::Producer> prods;
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            auto pr = w.producer();
            REQUIRE(pr.has_value());
            prods.push_back(std::move(*pr));
        }
        coro::CoroSemaphore done(PRODUCERS);
        co_await done.acquire(PRODUCERS);
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            scope.spawn([&, p]() -> coro::CoroTask<void> {
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
    const dstd::String text = gunzip_all(dstd::String(path.str()));
    dstd::MultiSet<dstd::String> got;
    for (std::size_t pos = 0; pos < text.size();) {
        auto nl = text.find('\n', pos);
        got.insert(text.substr(pos, nl + 1 - pos));
        pos = nl + 1;
    }
    CHECK(got == expect);
    dstd::Map<char, std::size_t> small;
    for (auto& [tag, size] : members)
        if (size < MEMBER) ++small[tag];
    for (auto& [tag, n] : small) CHECK(n <= 1);
    const dstd::String file = slurp(dstd::String(path.str()));
    for (auto& m : index_of(dstd::String(path.str())))
        CHECK(inflate_member(file, m.c_offset, m.c_size).back() == '\n');
    (void)dstd::fs::remove(path);
}

TEST_CASE("GzipLineWriter - producer refused when ordered, close needs flush") {
    auto path = dftu_utils_test::make_unique_test_path("glw_prod2.pfw.gz");
    run_coro([&](coro::Scope&) -> coro::CoroTask<void> {
        GzipWriterOptions o;
        auto wr = co_await GzipLineWriter::open(dstd::String(path.str()),
                                                std::move(o));
        REQUIRE(wr.has_value());
        CHECK(!wr->producer().has_value());
        REQUIRE(co_await wr->close());
        GzipWriterOptions unordered;
        unordered.ordered = false;
        auto w2 = co_await GzipLineWriter::open(dstd::String(path.str()),
                                                std::move(unordered));
        REQUIRE(w2.has_value());
        auto pr = w2->producer();
        REQUIRE(pr.has_value());
        REQUIRE(co_await pr->append(line(1)));
        CHECK(!(co_await w2->close()).has_value());
    });
    CHECK(!dstd::fs::exists(path).value());
}

TEST_CASE("GzipLineWriter - a drop without close on an executor joins") {
    auto path = dftu_utils_test::make_unique_test_path("glw_dropped.pfw.gz");
    GzipWriterOptions o;
    o.member_size = 500;
    o.workers = 2;
    std::atomic<bool> on_executor{false};
    Runtime rt(4);
    rt.submit(coro::with_scope([&](coro::Scope&) -> coro::CoroTask<void> {
                  {
                      auto wr = co_await GzipLineWriter::open(
                          dstd::String(path.str()), std::move(o));
                      REQUIRE(wr.has_value());
                      for (std::size_t i = 0; i < 100; ++i)
                          REQUIRE(co_await wr->append(line(i, 40)));
                      on_executor = coro::Executor::current() != nullptr;
                  }
                  co_return;
              }),
              "glw_dropped")
        .wait();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (dstd::fs::exists(path).value() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(dstd::fs::exists(path).value());
    CHECK(on_executor.load());
    rt.shutdown();
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
    const std::size_t memory_budget = o.memory_budget;
    WriteSummary sum;
    run_coro([&](coro::Scope& scope) -> coro::CoroTask<void> {
        auto wr = co_await GzipLineWriter::open(dstd::String(path.str()),
                                                std::move(o));
        REQUIRE(wr.has_value());
        GzipLineWriter& w = *wr;
        dstd::Vector<GzipLineWriter::Producer> prods;
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            auto pr = w.producer();
            REQUIRE(pr.has_value());
            prods.push_back(std::move(*pr));
        }
        coro::CoroSemaphore done(PRODUCERS);
        co_await done.acquire(PRODUCERS);
        for (std::size_t p = 0; p < PRODUCERS; ++p) {
            scope.spawn([&, p]() -> coro::CoroTask<void> {
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
    CHECK(sum.peak_bytes <= memory_budget);
    CHECK(sum.peak_in_flight <= 4);
    CHECK(sum.lines == expect_lines);
    const dstd::String text = gunzip_all(dstd::String(path.str()));
    CHECK(static_cast<std::uint64_t>(
              std::count(text.begin(), text.end(), '\n')) == expect_lines);
    (void)dstd::fs::remove(path);
}
