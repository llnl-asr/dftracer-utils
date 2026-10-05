#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "test_view_common.h"

using namespace dftracer::utils::trace::views::detail;
using dftracer::utils::index::gzip::GzipRecordKind;
namespace gz = dftracer::utils::utilities::fileio::compress;
namespace st = dftracer::utils::index::store;
namespace scan = dftracer::utils::trace::views::detail::scan;

namespace {

constexpr std::size_t CKPT = 1u << 20;
constexpr int EVENTS = 80000;
constexpr std::size_t MEMBER_BYTES = 256u << 10;

struct Trace {
    std::vector<std::string> lines;
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
        std::string fh;
        for (int k = 0; k < 32; ++k) fh += HEX[rng() & 15];
        t.lines.push_back(
            std::string("{\"id\":") + std::to_string(i) + ",\"name\":\"" +
            (read ? "read" : "write") +
            "\",\"cat\":\"POSIX\",\"pid\":" + std::to_string(1 + i % 4) +
            ",\"tid\":" + std::to_string(1 + i % 8) + ",\"ts\":" +
            std::to_string(1000 + i) + ",\"dur\":" + std::to_string(dur) +
            ",\"ph\":\"X\",\"args\":{\"fhash\":\"" + fh +
            "\",\"size\":" + std::to_string(i) + "}}\n");
        t.reads += read;
        t.dur_sum += dur;
    }
    return t;
}

void append_member(std::vector<std::uint8_t>& out, const std::string& text) {
    gz::GzipMemberCompressor c;
    std::vector<std::uint8_t> buf(c.bound(text.size()));
    auto n = c.compress(text.data(), text.size(), buf.data(), buf.size());
    REQUIRE(n.has_value());
    out.insert(out.end(), buf.begin(), buf.begin() + *n);
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& b) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
}

struct Indexed {
    std::string gz;
    std::string idx;
    ViewFile file() const { return ViewFile{gz, idx, 0, 0, CKPT}; }
};

Indexed make_indexed(TestEnvironment& env, const Trace& t,
                     const std::string& name, bool one_member) {
    std::vector<std::uint8_t> bytes;
    if (one_member) {
        std::string all;
        for (const auto& l : t.lines) all += l;
        append_member(bytes, all);
    } else {
        std::string cur;
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
    r.gz = env.get_dir() + "/" + name + ".pfw.gz";
    write_file(r.gz, bytes);
    const std::string dir = env.get_dir() + "/" + name + ".dftindex";
    REQUIRE(dftu_utils_test::build_index(r.gz, dir, CKPT));
    r.idx = determine_index_path(r.gz, dir);
    return r;
}

std::vector<dftracer::utils::index::gzip::GzipMemberRecord> members(
    const Indexed& x) {
    st::IndexDatabase db(x.idx, st::IndexOpenMode::ReadOnly);
    const int id = db.get_file_info_id(st::internal::get_logical_path(x.gz));
    REQUIRE(id >= 0);
    return db.query_gzip_members(id);
}

std::vector<std::int64_t> sorted_col(
    const dftracer::utils::dataframe::DataFrame& f, const char* name) {
    std::vector<std::int64_t> v;
    for (std::int64_t r = 0; r < f.num_rows(); ++r)
        v.push_back(static_cast<std::int64_t>(bnum(f, r, name)));
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

TEST_SUITE("RestartScan") {
    TEST_CASE("a one-member trace scans like the same lines in many members") {
        TestEnvironment env(10);
        const Trace t = make_trace();
        const Indexed one = make_indexed(env, t, "one", true);
        const Indexed many = make_indexed(env, t, "many", false);

        const auto pieces = members(one);
        REQUIRE(pieces.size() > 3);
        CHECK(pieces[0].kind == GzipRecordKind::HEAD);
        for (std::size_t i = 1; i < pieces.size(); ++i)
            CHECK(pieces[i].kind == GzipRecordKind::RESTART);
        const auto multi = members(many);
        REQUIRE(multi.size() > 3);
        for (const auto& m : multi) CHECK(m.kind == GzipRecordKind::MEMBER);

        const auto a = View::from_files({one.file()}).collect().get();
        const auto b = View::from_files({many.file()}).collect().get();
        REQUIRE(a.num_rows() == EVENTS);
        REQUIRE(b.num_rows() == EVENTS);
        CHECK(sorted_col(a, "ts") == sorted_col(b, "ts"));
        CHECK(sorted_col(a, "dur") == sorted_col(b, "dur"));
        std::int64_t sum = 0;
        for (auto d : sorted_col(a, "dur")) sum += d;
        CHECK(sum == t.dur_sum);
    }

    TEST_CASE("a filtered count over pieces matches the generated lines") {
        TestEnvironment env(10);
        const Trace t = make_trace();
        const Indexed one = make_indexed(env, t, "one", true);
        const Indexed many = make_indexed(env, t, "many", false);
        for (const auto& x : {one, many}) {
            auto f = View::from_files({x.file()})
                         .duql(R"(name == "read" | agg { n = count() })")
                         .lazy()
                         .collect();
            const auto df = run(std::move(f));
            REQUIRE(df.num_rows() == 1);
            CHECK(static_cast<std::int64_t>(bnum(df, 0, "n")) == t.reads);
        }
    }

    TEST_CASE("the plan has one scan unit per piece") {
        TestEnvironment env(10);
        const Trace t = make_trace();
        const Indexed one = make_indexed(env, t, "one", true);
        const auto pieces = members(one);
        REQUIRE(pieces.size() > 3);

        ScanPlan plan = scan::from_files({one.file()});
        ViewDefinition vdef = make_vdef(*plan, /*for_aggregation=*/false);
        std::uint64_t skipped = 0;
        const auto units = run(gather_units(*plan, vdef, skipped));
        CHECK(units.size() == pieces.size());
        CHECK(skipped == 0);
    }
}
