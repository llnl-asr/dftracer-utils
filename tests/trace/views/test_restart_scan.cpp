// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>
#include <trace/views/test_view_common.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <random>

using namespace dftracer::utils;

using namespace dftracer::utils::trace::internal;
using namespace dftracer::utils::trace::views;
using namespace dftu_utils_test;
using namespace test_view_common;

using namespace dftracer::utils::trace::views::detail;
using namespace dftracer::utils::trace;

namespace {

constexpr std::size_t CKPT = 128u << 10;
constexpr std::size_t MEMBER_BYTES = 32u << 10;
// A line is longer than 128 bytes, so the inflated trace spans more than 10
// checkpoints and 20 members.
constexpr int EVENTS = static_cast<int>(10 * CKPT / 128);

struct Trace {
    dstd::Vector<dstd::String> lines;
    std::int64_t reads = 0;
    std::int64_t dur_sum = 0;
};

Trace make_trace() {
    static const char HEX[] = "0123456789abcdef";
    std::mt19937 rng(7);
    Trace t;
    for (int i = 0; i < EVENTS; ++i) {
        const bool read = i % 3 != 0;
        const std::int64_t dur = 1 + i % 17;
        dstd::String fh;
        for (int k = 0; k < 32; ++k) fh += HEX[rng() & 15];
        t.lines.push_back(
            dstd::String("{\"id\":") + dstd::to_string(i) + ",\"name\":\"" +
            (read ? "read" : "write") +
            "\",\"cat\":\"POSIX\",\"pid\":" + dstd::to_string(1 + i % 4) +
            ",\"tid\":" + dstd::to_string(1 + i % 8) + ",\"ts\":" +
            dstd::to_string(1000 + i) + ",\"dur\":" + dstd::to_string(dur) +
            ",\"ph\":\"X\",\"args\":{\"fhash\":\"" + fh +
            "\",\"size\":" + dstd::to_string(i) + "}}\n");
        t.reads += read;
        t.dur_sum += dur;
    }
    return t;
}

void append_member(dstd::Vector<std::uint8_t>& out, const dstd::String& text) {
    utilities::fileio::compress::GzipMemberCompressor c;
    dstd::Vector<std::uint8_t> buf(c.bound(text.size()));
    auto n = c.compress(text.data(), text.size(), buf.data(), buf.size());
    REQUIRE(n.has_value());
    out.insert(out.end(), buf.begin(), buf.begin() + *n);
}

void write_file(const dstd::String& path, const dstd::Vector<std::uint8_t>& b) {
    dftu_utils_test::FileOut f(path);
    f.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
}

struct Indexed {
    dstd::String gz;
    dstd::String idx;
    ViewFile file() const {
        return ViewFile{dstd::String(gz), dstd::String(idx), 0, 0, CKPT};
    }
};

Indexed make_indexed(TestEnvironment& env, const Trace& t,
                     const dstd::String& name, bool one_member) {
    dstd::Vector<std::uint8_t> bytes;
    if (one_member) {
        dstd::String all;
        for (const auto& l : t.lines) all += l;
        append_member(bytes, all);
    } else {
        dstd::String cur;
        for (const auto& l : t.lines) {
            cur += l;
            if (cur.size() >= MEMBER_BYTES) {
                append_member(bytes, cur);
                cur.clear();
            }
        }
        if (!cur.empty()) append_member(bytes, cur);
    }
    Indexed r;
    r.gz = env.dir() + "/" + name + ".pfw.gz";
    write_file(r.gz, bytes);
    const dstd::String dir = env.dir() + "/" + name + ".dftindex";
    REQUIRE(dftu_utils_test::build_index(r.gz, dir, CKPT));
    r.idx = determine_index_path(r.gz, dir);
    return r;
}

dstd::Vector<dftracer::utils::index::gzip::GzipMemberRecord> members(
    const Indexed& x) {
    return dftu_utils_test::indexed_members(x.idx, x.gz);
}

dstd::Vector<std::int64_t> sorted_col(
    const dftracer::utils::dataframe::DataFrame& f, const char* name) {
    dstd::Vector<std::int64_t> v;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        v.push_back(static_cast<std::int64_t>(bnum(f, r, name)));
    std::sort(v.begin(), v.end());
    return v;
}

// Built once: every case reads the same two indexed copies.
struct Fixture {
    TestEnvironment env{10};
    Trace t = make_trace();
    Indexed one = make_indexed(env, t, "one", true);
    Indexed many = make_indexed(env, t, "many", false);
};

const Fixture& fixture() {
    static Fixture f;
    return f;
}

}  // namespace

TEST_SUITE("RestartScan") {
    TEST_CASE("a one-member trace scans like the same lines in many members") {
        const Fixture& fx = fixture();
        const auto pieces = members(fx.one);
        REQUIRE(pieces.size() > 3);
        CHECK(pieces[0].kind == index::gzip::GzipRecordKind::Head);
        for (std::size_t i = 1; i < pieces.size(); ++i)
            CHECK(pieces[i].kind == index::gzip::GzipRecordKind::Restart);
        const auto multi = members(fx.many);
        REQUIRE(multi.size() > 3);
        for (const auto& m : multi)
            CHECK(m.kind == index::gzip::GzipRecordKind::Member);

        const auto a = View::from_files({fx.one.file()}).collect().get();
        const auto b = View::from_files({fx.many.file()}).collect().get();
        REQUIRE(a.num_rows() == EVENTS);
        REQUIRE(b.num_rows() == EVENTS);
        CHECK(sorted_col(a, "ts") == sorted_col(b, "ts"));
        CHECK(sorted_col(a, "dur") == sorted_col(b, "dur"));
        std::int64_t sum = 0;
        for (auto d : sorted_col(a, "dur")) sum += d;
        CHECK(sum == fx.t.dur_sum);
    }

    TEST_CASE("a filtered count over pieces matches the generated lines") {
        const Fixture& fx = fixture();
        for (const Indexed* x : {&fx.one, &fx.many}) {
            auto f = View::from_files({x->file()})
                         .duql(R"(name == "read" | agg { n = count() })")
                         .lazy()
                         .collect();
            const auto df = run(std::move(f));
            REQUIRE(df.num_rows() == 1);
            CHECK(static_cast<std::int64_t>(bnum(df, 0, "n")) == fx.t.reads);
        }
    }

    TEST_CASE("the plan has one scan unit per piece") {
        const Fixture& fx = fixture();
        const auto pieces = members(fx.one);
        REQUIRE(pieces.size() > 3);

        ScanPlan plan = views::detail::scan::from_files({fx.one.file()});
        ViewDefinition vdef = make_vdef(*plan, /*for_aggregation=*/false);
        std::uint64_t skipped = 0;
        const auto units = run(gather_units(*plan, vdef, skipped));
        CHECK(units.size() == pieces.size());
        CHECK(skipped == 0);
    }
}
