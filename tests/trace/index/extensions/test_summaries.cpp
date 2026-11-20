// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash/value_hash.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/index/extensions/column_feeder.h>
#include <dftracer/utils/trace/index/extensions/kinds/payloads.h>
#include <dftracer/utils/trace/index/extensions/summaries.h>
#include <dftracer/utils/trace/index/indexer.h>
#include <dftracer/utils/trace/index/plan/chunk_pruner.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/tables.h>
#include <doctest/doctest.h>
#include <index_test_helpers.h>
#include <task_result.h>
#include <testing_utilities.h>

#include <cmath>
#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::trace::index;
using namespace dftracer::utils::trace::index::extensions;
using dataframe::DataFrame;
using dataframe::Series;
using dataframe::TypeId;

namespace {

DataFrame frame_of(dstd::Vector<dstd::String> names,
                   dstd::Vector<Series> columns) {
    DataFrame f;
    for (const auto& n : names) f.names.emplace_back(dstd::StringView(n));
    f.columns = std::move(columns);
    return f;
}

dstd::String payload_of(SummaryBuilder& b, bool file = false) {
    ExtensionOut out{{}, UINT64_MAX, false};
    const bool stored = file ? b.finish_file(out) : b.finish_granule(out);
    REQUIRE(stored);
    return out.bytes;
}

ExtensionLeaf leaf(ExtensionOp op, const ExtensionValue& v) {
    ExtensionLeaf l;
    l.path = "p";
    l.op = op;
    l.value = v;
    return l;
}

ExtensionValue int_value(std::int64_t i) {
    ExtensionValue v;
    v.kind = ExtensionValue::Kind::Int64;
    v.i = i;
    return v;
}

ExtensionValue double_value(double f) {
    ExtensionValue v;
    v.kind = ExtensionValue::Kind::Double;
    v.f = f;
    return v;
}

bool may(const Summary& s, TypeId type, const ExtensionLeaf& l,
         dstd::StringView payload, bool file = false) {
    auto q = s.compile(l, type);
    REQUIRE(q.has_value());
    REQUIRE(*q);
    return file ? (*q)->file_may_match(payload) : (*q)->may_match(payload);
}

}  // namespace

TEST_SUITE("summaries") {
    TEST_CASE("a zonemap builder takes a column with nulls and NaNs") {
        const double values[] = {2.5, std::nan(""), 7.0, 0.5};
        const std::uint8_t valid[] = {0b00001101};  // row 1 is null
        const auto series = Series::flat(TypeId::Float64, values, 4, valid);
        ZonemapSummary summary(0);
        auto b = summary.builder(TypeId::Float64);
        REQUIRE(b);
        b->set_observed(4);
        REQUIRE(b->add(series));
        const auto zone = kinds::decode_zone(payload_of(*b));
        REQUIRE(zone.has_value());
        CHECK(zone->type() == BoundType::Double);
        CHECK(zone->min_double() == 0.5);
        CHECK(zone->max_double() == 7.0);
        CHECK(zone->null_count() == 1);
        CHECK(zone->nan_count() == 0);
        CHECK(zone->present == 3);
        CHECK(zone->observed == 4);
    }

    TEST_CASE("a column of NaNs is counted and has no bounds") {
        const double values[] = {std::nan(""), std::nan("")};
        const auto series = Series::flat(TypeId::Float64, values, 2);
        ZonemapSummary summary(0);
        auto b = summary.builder(TypeId::Float64);
        REQUIRE(b->add(series));
        const auto zone = kinds::decode_zone(payload_of(*b));
        REQUIRE(zone.has_value());
        CHECK(zone->type() == BoundType::None);
        CHECK(zone->nan_count() == 2);
        CHECK(may(summary, TypeId::Float64, leaf(ExtensionOp::Gt, int_value(1)),
                  payload_of(*b)));
    }

    TEST_CASE("a type no summary can build for records nothing") {
        ZonemapSummary z(0);
        BloomSummary b({}, 0);
        CountsSummary c(4096, 0);
        PostingsSummary p(0);
        for (const Summary* s : dstd::Vector<const Summary*>{&z, &b, &c, &p}) {
            CHECK(s->builder(TypeId::Binary) == nullptr);
            CHECK(s->builder(TypeId::List) == nullptr);
            CHECK(s->builder(TypeId::Int64) != nullptr);
        }
        // A column of a type with no builder stores nothing, while the others
        // still do.
        const std::uint8_t halves[] = {1, 2, 3, 4};
        const std::int64_t ints[] = {4, 5};
        ColumnFeeder feeder({dstd::make_shared<ZonemapSummary>(0)});
        dstd::Vector<Series> cols;
        cols.push_back(Series::flat(TypeId::Float16, halves, 2));
        cols.push_back(Series::flat_i64(ints, 2));
        feeder.feed(0, frame_of({"h", "i"}, std::move(cols)));
        const auto payloads = feeder.finish();
        REQUIRE(payloads.size() == 1);
        CHECK(payloads[0].path == "i");
    }

    TEST_CASE("each summary answers its leaves from a payload") {
        const std::int64_t ints[] = {10, 20, 30};
        const auto series = Series::flat_i64(ints, 3);

        ZonemapSummary zonemap(0);
        auto zb = zonemap.builder(TypeId::Int64);
        zb->set_observed(3);
        REQUIRE(zb->add(series));
        const auto zp = payload_of(*zb);
        CHECK(may(zonemap, TypeId::Int64, leaf(ExtensionOp::Gt, int_value(25)),
                  zp));
        CHECK_FALSE(may(zonemap, TypeId::Int64,
                        leaf(ExtensionOp::Gt, int_value(30)), zp));
        CHECK_FALSE(may(zonemap, TypeId::Int64,
                        leaf(ExtensionOp::Eq, double_value(5.5)), zp));
        CHECK(may(zonemap, TypeId::Int64,
                  leaf(ExtensionOp::Eq, double_value(20.0)), zp));

        BloomSummary bloom({}, 0);
        auto bb = bloom.builder(TypeId::Int64);
        REQUIRE(bb->add(series));
        const auto bp = payload_of(*bb);
        CHECK(may(bloom, TypeId::Int64, leaf(ExtensionOp::Eq, int_value(20)),
                  bp));
        // 20.0 is 20, whatever the type that built the filter.
        CHECK(may(bloom, TypeId::Int64,
                  leaf(ExtensionOp::Eq, double_value(20.0)), bp));
        CHECK_FALSE(may(bloom, TypeId::Int64,
                        leaf(ExtensionOp::Eq, int_value(21)), bp));
        CHECK_FALSE(may(bloom, TypeId::Int64,
                        leaf(ExtensionOp::Eq, double_value(20.5)), bp));
        CHECK(may(bloom, TypeId::Int64, leaf(ExtensionOp::Eq, int_value(20)),
                  payload_of(*bb, true), true));

        CountsSummary counts(4096, 0);
        auto cb = counts.builder(TypeId::Int64);
        cb->set_observed(3);
        REQUIRE(cb->add(series));
        const auto cp = payload_of(*cb);
        CHECK(may(counts, TypeId::Int64, leaf(ExtensionOp::Eq, int_value(30)),
                  cp));
        CHECK_FALSE(may(counts, TypeId::Int64,
                        leaf(ExtensionOp::Eq, int_value(31)), cp));

        PostingsSummary postings(0);
        auto pb = postings.builder(TypeId::Int64);
        REQUIRE(pb->add(series));
        const auto pp = payload_of(*pb);
        CHECK(may(postings, TypeId::Int64, leaf(ExtensionOp::Eq, int_value(10)),
                  pp));
        CHECK_FALSE(may(postings, TypeId::Int64,
                        leaf(ExtensionOp::Eq, int_value(11)), pp));
        CHECK(PostingsBuilder::decode(pp).size() == 3);
    }

    TEST_CASE("a number leaf probes the text of a string path") {
        const dstd::String values[] = {"7", "x"};
        const auto series = Series::strings(values);
        BloomSummary bloom({}, 0);
        auto bb = bloom.builder(TypeId::String);
        REQUIRE(bb->add(series));
        const auto bp = payload_of(*bb);
        CHECK(may(bloom, TypeId::String, leaf(ExtensionOp::Eq, int_value(7)),
                  bp));
        // A path known to hold numbers never equals a string.
        auto numeric =
            bloom.compile(leaf(ExtensionOp::Eq, int_value(7)), TypeId::Int64);
        REQUIRE(numeric.has_value());
        CHECK_FALSE((*numeric)->may_match(bp));
    }

    TEST_CASE("a uint64 above int64 is probed as itself and as its double") {
        const std::uint64_t big = (std::uint64_t{1} << 63) + 4096;
        ExtensionValue v;
        v.kind = ExtensionValue::Kind::UInt64;
        v.u = big;
        BloomSummary bloom({}, 0);
        for (int as_double = 0; as_double < 2; ++as_double) {
            auto bb = bloom.builder(TypeId::Float64);
            if (as_double == 0) {
                const std::uint64_t one[] = {big};
                bb->add_u64(one);
            } else {
                const double one[] = {static_cast<double>(big)};
                bb->add_f64(one);
            }
            CHECK(may(bloom, TypeId::Float64, leaf(ExtensionOp::Eq, v),
                      payload_of(*bb)));
        }
        auto other = bloom.builder(TypeId::Float64);
        const std::uint64_t unrelated[] = {7};
        other->add_u64(unrelated);
        CHECK_FALSE(may(bloom, TypeId::Float64, leaf(ExtensionOp::Eq, v),
                        payload_of(*other)));

        ZonemapSummary zonemap(0);
        auto zb = zonemap.builder(TypeId::Uint64);
        const std::uint64_t range[] = {big - 10, big + 10};
        zb->add_u64(range);
        zb->set_observed(2);
        CHECK(may(zonemap, TypeId::Uint64, leaf(ExtensionOp::Eq, v),
                  payload_of(*zb)));
        CHECK(may(zonemap, TypeId::Uint64, leaf(ExtensionOp::Gt, v),
                  payload_of(*zb)));
        ExtensionValue above;
        above.kind = ExtensionValue::Kind::UInt64;
        above.u = big + 100;
        CHECK_FALSE(may(zonemap, TypeId::Uint64, leaf(ExtensionOp::Gt, above),
                        payload_of(*zb)));
    }

    TEST_CASE("counts answer not-equal and not-in leaves") {
        const std::int64_t ints[] = {3, 3, 3};
        CountsSummary counts(4096, 0);
        auto cb = counts.builder(TypeId::Int64);
        cb->set_observed(3);
        REQUIRE(cb->add(Series::flat_i64(ints, 3)));
        const auto payload = payload_of(*cb);
        // Every value is 3, so no event is != 3.
        CHECK_FALSE(may(counts, TypeId::Int64,
                        leaf(ExtensionOp::Ne, int_value(3)), payload));
        CHECK(may(counts, TypeId::Int64, leaf(ExtensionOp::Ne, int_value(4)),
                  payload));
        ExtensionLeaf in_leaf;
        in_leaf.path = "p";
        in_leaf.op = ExtensionOp::NotIn;
        const ExtensionValue listed[] = {int_value(3), int_value(5)};
        in_leaf.items = listed;
        CHECK_FALSE(may(counts, TypeId::Int64, in_leaf, payload));
        const ExtensionValue other[] = {int_value(4)};
        in_leaf.items = other;
        CHECK(may(counts, TypeId::Int64, in_leaf, payload));
    }

    TEST_CASE("the version of a payload is checked") {
        ZonemapSummary zonemap(7);
        CHECK(zonemap.params_hash() == 7);
        CHECK(zonemap.version() >= 1);
        CHECK(zonemap.name() == "zonemap");
        CHECK(may(zonemap, TypeId::Int64, leaf(ExtensionOp::Gt, int_value(1)),
                  "garbage"));
    }
}

namespace {

// A trace whose events carry name, cat, pid and tid.
dstd::String write_trace(dftu_utils_test::TestEnvironment& env, int events) {
    const dstd::String pfw = env.dir() + "/feeders.pfw";
    {
        dftu_utils_test::FileOut out(pfw);
        const char* cats[] = {"POSIX", "STDIO", "MPI"};
        for (int i = 0; i < events; ++i)
            out << R"({"ph":1,"name":"op)" << (i * 7919) % 23 << R"(","cat":")"
                << cats[(i / 5) % 3] << R"(","pid":)" << 100 + (i / 400) % 4
                << R"(,"tid":)" << i % 6 << R"(,"ts":)" << 1000 + i * 10
                << R"(,"dur":5,"args":{}})"
                << "\n";
    }
    const dstd::String gz = pfw + ".gz";
    REQUIRE(
        dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 8 * 1024));
    dstd::fs::remove(pfw).discard();
    return gz;
}

}  // namespace

TEST_SUITE("feeders") {
    TEST_CASE("the column feeder gives the summaries the JSON feeder builds") {
        dftu_utils_test::TestEnvironment env(10);
        constexpr int EVENTS = 4000;
        const dstd::String gz = write_trace(env, EVENTS);
        auto ix = dftu_utils_test::ok(
            Indexer::open({dstd::String(dstd::StringView(gz))}));
        const auto built = dftu_utils_test::ok(ix.build());
        const auto snapshot = dftu_utils_test::index_snapshot(built.index_path);
        const auto file = dftu_utils_test::require_indexed_file(snapshot, gz);
        const auto spans =
            dftu_utils_test::index_take(trace::index::spans(snapshot, file));
        REQUIRE(spans.size() >= 2);

        const char* cats[] = {"POSIX", "STDIO", "MPI"};
        ColumnFeeder feeder({dstd::make_shared<ZonemapSummary>(0),
                             dstd::make_shared<BloomSummary>(BloomParams{}, 0),
                             dstd::make_shared<CountsSummary>(4096, 0)});
        for (std::uint64_t c = 0; c < spans.size(); ++c) {
            REQUIRE(spans[c].first_line_num >= 1);
            dstd::Vector<dstd::String> name, cat;
            dstd::Vector<std::uint64_t> pid, tid;
            for (auto line = spans[c].first_line_num;
                 line <= spans[c].last_line_num; ++line) {
                const auto i = static_cast<int>(line - 1);
                name.push_back("op" + dstd::to_string((i * 7919) % 23));
                cat.push_back(cats[(i / 5) % 3]);
                pid.push_back(static_cast<std::uint64_t>(100 + (i / 400) % 4));
                tid.push_back(static_cast<std::uint64_t>(i % 6));
            }
            dstd::Vector<Series> cols;
            cols.push_back(Series::strings(name));
            cols.push_back(Series::strings(cat));
            cols.push_back(Series::flat(TypeId::Uint64, pid.data(),
                                        static_cast<std::int64_t>(pid.size())));
            cols.push_back(Series::flat(TypeId::Uint64, tid.data(),
                                        static_cast<std::int64_t>(tid.size())));
            feeder.feed(
                c, frame_of({"name", "cat", "pid", "tid"}, std::move(cols)));
        }
        const auto payloads = feeder.finish();

        dstd::Map<dstd::String, dstd::String> fed;
        for (const auto& p : payloads)
            if (p.granule)
                fed[dstd::fmt<"{}|{}|{}">(static_cast<int>(p.extension), p.path,
                                          *p.granule)] = p.bytes;
        std::size_t compared = 0;
        for (auto ext : {trace::index::store::IndexExtension::Zonemap,
                         trace::index::store::IndexExtension::Bloom,
                         trace::index::store::IndexExtension::Counts})
            for (const char* path : {"name", "cat", "pid", "tid"}) {
                const auto stored = dftu_utils_test::index_take(
                    trace::index::granules(snapshot, file, ext, path));
                REQUIRE_FALSE(stored.empty());
                for (const auto& [granule, bytes] : stored) {
                    const auto key = dstd::fmt<"{}|{}|{}">(
                        static_cast<int>(ext), dstd::String(path), granule);
                    CAPTURE(key);
                    REQUIRE(fed.count(key));
                    CHECK(dstd::String(bytes.data(), bytes.size()) == fed[key]);
                    ++compared;
                }
            }
        CHECK(compared >= 3 * 4 * spans.size());
    }

    TEST_CASE("the column feeder writes an index the pruner reads") {
        auto dir = dftu_utils_test::make_unique_test_path("column_feeder");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        const dstd::String trace = "/fake/column.pfw.gz";
        const dstd::String path((dir / ".dftindex").str());
        ColumnFeeder feeder(
            {dstd::make_shared<ZonemapSummary>(0),
             dstd::make_shared<BloomSummary>(BloomParams{}, 0)});
        const std::int64_t low[] = {1, 5, 9};
        const std::int64_t high[] = {100, 500, 900};
        {
            auto idx = dftu_utils_test::open_test_index(path);
            auto w = dftu_utils_test::register_test_file(idx, trace, 1);
            for (std::uint64_t c = 0; c < 2; ++c) {
                trace::index::put_gzip_member(
                    w, {c, c * 100, 100, c * 200, 200, c * 3 + 1, c * 3 + 3});
                dstd::Vector<Series> cols;
                cols.push_back(Series::flat_i64(c == 0 ? low : high, 3));
                feeder.feed(c, frame_of({"n"}, std::move(cols)));
            }
            const auto payloads = feeder.finish();
            feeder.write(w, payloads);
            dftu_utils_test::mark_built(w);
            dftu_utils_test::commit_test_build(w);
        }
        auto q = duql::Query::from_string("args.n > 50");
        REQUIRE(q.has_value());
        const auto pruned = trace::index::plan::prune_file(
                                {path, trace, &*q, nullptr, dstd::nullopt})
                                .get();
        REQUIRE(pruned.has_value());
        CHECK(pruned->candidates == dstd::Vector<std::uint64_t>{1});
        auto eq = duql::Query::from_string("args.n == 500");
        REQUIRE(eq.has_value());
        const auto found = trace::index::plan::prune_file(
                               {path, trace, &*eq, nullptr, dstd::nullopt})
                               .get();
        REQUIRE(found.has_value());
        CHECK(found->candidates == dstd::Vector<std::uint64_t>{1});
    }
}
