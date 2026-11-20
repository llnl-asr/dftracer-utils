// SPDX-License-Identifier: MIT
// The native row index: every frame op has a rule, and each rule (keep,
// rebuild, drop) holds for a two-level index.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/frame_ops.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/internal/frame_index.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <span_list.h>
#include <task_result.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::dataframe;
using dftu_utils_test::ok;
using dftu_utils_test::span_of;

namespace {

Series i64s(dstd::Vector<std::int64_t> v) {
    return Series::flat_i64(v.data(), static_cast<std::int64_t>(v.size()));
}

// k1 = [1, 1, 2, 2, 3, 3], k2 = [10, 20, 10, 20, 10, 20], v = [6, 5, 4, 3, 2,
// 1]
DataFrame make_frame() {
    DataFrame df;
    df.names = dstd::make_shared_strings({"k1", "k2", "v"});
    df.columns.push_back(i64s({1, 1, 2, 2, 3, 3}));
    df.columns.push_back(i64s({10, 20, 10, 20, 10, 20}));
    df.columns.push_back(i64s({6, 5, 4, 3, 2, 1}));
    return df;
}

DataFrame indexed2() {
    const dstd::StringView names[] = {"k1", "k2"};
    return ok(make_frame().with_index(names));
}

dstd::Vector<dstd::String> index_of(const DataFrame& df) {
    dstd::Vector<dstd::String> out;
    for (const dstd::SharedString& n : df.index_names())
        out.emplace_back(dstd::StringView(n));
    return out;
}

bool index_is(const DataFrame& df, std::initializer_list<const char*> want) {
    const dstd::Vector<dstd::String> got = index_of(df);
    if (got.size() != want.size()) return false;
    std::size_t i = 0;
    for (const char* w : want)
        if (dstd::StringView(got[i++]) != dstd::StringView(w)) return false;
    return true;
}

DataFrame run(coro::CoroTask<dstd::Result<DataFrame>> t) {
    return ok(dftracer::utils::default_runtime().submit(std::move(t)).get());
}

// The lazy plan's result carries the index the eager op gives.
void same_index(const DataFrame& eager, const DataFrame& lazy) {
    CHECK(index_of(eager) == index_of(lazy));
    CHECK(eager.names.size() == lazy.names.size());
}

const dstd::String K1 = "k1";
const dstd::String K2 = "k2";

}  // namespace

TEST_CASE("every frame op has an index rule") {
    dstd::Vector<dstd::String> ops;
#define DFTU_FRAME_OP(name, fn, ret, o0, o1, o2, o3, o4, o5, o6) \
    ops.emplace_back(#name);
#include <dftracer/utils/dataframe/exported_frame_ops.def>
    REQUIRE(ops.size() > 40);
    for (const dstd::String& op : ops) {
        INFO("frame op " << op.c_str());
        CHECK(index_rule_of(op).has_value());
    }
    CHECK_FALSE(index_rule_of("not_an_op").has_value());
    CHECK(*index_rule_of("filter") == IndexRule::Keep);
    CHECK(*index_rule_of("group_by") == IndexRule::Rebuild);
    CHECK(*index_rule_of("describe") == IndexRule::Drop);
}

TEST_CASE("every index rule names a frame op") {
    dstd::Set<dstd::String> ops;
#define DFTU_FRAME_OP(name, fn, ret, o0, o1, o2, o3, o4, o5, o6) \
    ops.insert(#name);
#include <dftracer/utils/dataframe/exported_frame_ops.def>
    std::size_t rules = 0;
#define DFTU_FRAME_INDEX_RULE(name, rule) \
    {                                     \
        ++rules;                          \
        CHECK(ops.count(#name) == 1);     \
    }
#include <dftracer/utils/dataframe/frame_op_index_rules.def>
    CHECK(rules == ops.size());
}

TEST_CASE("with_index validates names and types") {
    DataFrame df = make_frame();
    CHECK(df.index_names().empty());
    const dstd::StringView good[] = {"k2", "k1"};
    DataFrame ix = ok(df.with_index(good));
    CHECK(index_is(ix, {"k2", "k1"}));
    CHECK(ix.num_columns() == 3);
    CHECK(index_is(ix.share(), {"k2", "k1"}));
    CHECK(ix.without_index().index_names().empty());
    CHECK(ok(ix.with_index({})).index_names().empty());

    const dstd::StringView missing[] = {"k1", "nope"};
    dftu_utils_test::expect_error(df.with_index(missing),
                                  common::ErrorCode::NotFound);

    const dstd::StringView dup[] = {"k1", "k1"};
    dftu_utils_test::expect_error(df.with_index(dup),
                                  common::ErrorCode::InvalidArgument);
}

TEST_CASE("keep: row ops carry a two-level index") {
    DataFrame ix = indexed2();
    const Series mask = ix.column("v").gt(2);
    CHECK(index_is(ix.filter(mask), {"k1", "k2"}));
    CHECK(index_is(ok(ix.sort_by("v")), {"k1", "k2"}));
    CHECK(index_is(ok(ix.sort_by_multi(span_of({K2, K1}))), {"k1", "k2"}));
    CHECK(index_is(ix.slice(1, 3), {"k1", "k2"}));
    CHECK(index_is(ix.head(2), {"k1", "k2"}));
    CHECK(index_is(ix.tail(2), {"k1", "k2"}));
    CHECK(index_is(ix.reverse(), {"k1", "k2"}));
    CHECK(index_is(ix.sample(3), {"k1", "k2"}));
    CHECK(index_is(ix.drop_nulls(), {"k1", "k2"}));
    CHECK(index_is(ix.fill_null(0), {"k1", "k2"}));
    const std::int64_t at[] = {5, 0, 2};
    DataFrame taken = ix.take(at);
    CHECK(index_is(taken, {"k1", "k2"}));
    CHECK(taken.num_rows() == 3);
    CHECK(index_is(ok(ix.unique()), {"k1", "k2"}));
    CHECK(index_is(ok(ix.drop_duplicates()), {"k1", "k2"}));
    CHECK(index_is(ok(ix.topk("v", 2)), {"k1", "k2"}));
    CHECK(index_is(ix.with_column("w", ix.column("v") + ix.column("v")),
                   {"k1", "k2"}));
}

TEST_CASE("keep: select drops the index names that are gone") {
    DataFrame ix = indexed2();
    CHECK(index_is(ok(ix.select(span_of({K1, K2, dstd::String("v")}))),
                   {"k1", "k2"}));
    CHECK(index_is(ok(ix.select(span_of({K2, dstd::String("v")}))), {"k2"}));
    CHECK(ok(ix.select(span_of({dstd::String("v")}))).index_names().empty());
}

TEST_CASE("an unindexed frame stays unindexed under every rule") {
    DataFrame df = make_frame();
    CHECK(df.head(2).index_names().empty());
    CHECK(ok(df.sort_by("v")).index_names().empty());
    CHECK(
        ok(df.group_by(span_of({K1}), span_of({GroupAgg{Agg::Sum, "v", "s"}})))
            .index_names()
            .empty());
}

TEST_CASE("rebuild: rename follows the index columns") {
    DataFrame ix = indexed2();
    DataFrame r = ok(ix.rename(
        span_of({dstd::String("a"), dstd::String("b"), dstd::String("c")})));
    CHECK(index_is(r, {"a", "b"}));
}

TEST_CASE("rebuild: group_by indexes by the keys") {
    DataFrame ix = indexed2();
    DataFrame g = ok(ix.group_by(span_of({K1, K2}),
                                 span_of({GroupAgg{Agg::Sum, "v", "s"}})));
    CHECK(index_is(g, {"k1", "k2"}));
    DataFrame g1 =
        ok(ix.group_by(span_of({K1}), span_of({GroupAgg{Agg::Sum, "v", "s"}})));
    CHECK(index_is(g1, {"k1"}));
    DataFrame all = ok(ix.group_by(dstd::Span<const dstd::String>{},
                                   span_of({GroupAgg{Agg::Sum, "v", "s"}})));
    CHECK(all.index_names().empty());
}

TEST_CASE("rebuild: join keeps the keys only when both sides index on them") {
    DataFrame left = indexed2();
    DataFrame right = indexed2();
    DataFrame j = ok(left.join(right, span_of({K1, K2})));
    CHECK(index_is(j, {"k1", "k2"}));

    DataFrame plain = make_frame();
    CHECK(ok(left.join(plain, span_of({K1, K2}))).index_names().empty());
    CHECK(ok(plain.join(right, span_of({K1, K2}))).index_names().empty());

    const dstd::StringView one[] = {"k1"};
    DataFrame r1 = ok(make_frame().with_index(one));
    CHECK(ok(left.join(r1, span_of({K1}))).index_names().empty());
    DataFrame l1 = ok(make_frame().with_index(one));
    CHECK(index_is(ok(l1.join(r1, span_of({K1}))), {"k1"}));
}

TEST_CASE("pivot indexes by its index column and unpivot drops the index") {
    const dstd::StringView one[] = {"k1"};
    DataFrame ix = ok(make_frame().with_index(one));
    DataFrame p = ok(ix.pivot("k1", "k2", "v", "sum"));
    CHECK(index_is(p, {"k1"}));
    DataFrame u = ok(ix.unpivot(span_of({K1}), span_of({K2})));
    CHECK(u.index_names().empty());
    DataFrame u2 =
        ok(indexed2().unpivot(span_of({K1}), span_of({dstd::String("v")})));
    CHECK(u2.index_names().empty());
}

TEST_CASE("rebuild: concat keeps a shared index and drops a mixed one") {
    DataFrame a = indexed2();
    DataFrame b = indexed2();
    const DataFrame* same[] = {&a, &b};
    CHECK(index_is(ok(concat(same)), {"k1", "k2"}));

    DataFrame plain = make_frame();
    const DataFrame* mixed[] = {&a, &plain};
    CHECK(ok(concat(mixed)).index_names().empty());

    const dstd::StringView one[] = {"k1"};
    DataFrame c = ok(make_frame().with_index(one));
    const DataFrame* other[] = {&a, &c};
    CHECK(ok(concat(other)).index_names().empty());
}

TEST_CASE("drop: summaries and positional columns have no index") {
    DataFrame ix = indexed2();
    CHECK(ix.describe().index_names().empty());
    CHECK(ix.null_count().index_names().empty());
    CHECK(ix.with_row_index("pos").index_names().empty());
    CHECK(ok(ix.reduce(Agg::Sum)).index_names().empty());
    CHECK(ok(ix.reduce(span_of({GroupAgg{Agg::Sum, "v", "s"}})))
              .index_names()
              .empty());
}

TEST_CASE("keep: window, asof and interval carry the left index") {
    DataFrame ix = indexed2();
    WindowColumn rank;
    rank.func = WindowFunc::RowNumber;
    rank.out = "rn";
    DataFrame w = ok(window(ix, span_of({K1}), span_of({K2}), span_of({rank})));
    CHECK(index_is(w, {"k1", "k2"}));
}

TEST_CASE("a lazy plan carries the index through the same rules") {
    DataFrame ix = indexed2();
    const dstd::String v = "v";

    SUBCASE("keep") {
        same_index(ix.filter(ix.column("v").gt(2)),
                   run(ix.lazy().filter(col(2) > std::int64_t{2}).collect()));
        same_index(ok(ix.sort_by("v")), run(ix.lazy().sort_by("v").collect()));
        same_index(ix.slice(1, 3), run(ix.lazy().slice(1, 3).collect()));
        same_index(ix.head(2), run(ix.lazy().head(2).collect()));
        same_index(ix.tail(2), run(ix.lazy().tail(2).collect()));
        same_index(ix.reverse(), run(ix.lazy().reverse().collect()));
        same_index(ok(ix.unique()), run(ix.lazy().unique().collect()));
        const std::int64_t at[] = {5, 0, 2};
        same_index(
            ix.take(at),
            run(ix.lazy().take(dstd::Vector<std::int64_t>{5, 0, 2}).collect()));
        CHECK(
            index_is(run(ix.lazy().filter(col(2) > std::int64_t{2}).collect()),
                     {"k1", "k2"}));
    }
    SUBCASE("select and drop remove the index names that are gone") {
        DataFrame sel =
            run(ix.lazy().select(dstd::Vector<dstd::String>{K2, v}).collect());
        CHECK(index_is(sel, {"k2"}));
        same_index(ok(ix.select(span_of({K2, v}))), sel);
        CHECK(index_is(
            run(ix.lazy().drop(dstd::Vector<dstd::String>{K1}).collect()),
            {"k2"}));
        CHECK(run(ix.lazy().select(dstd::Vector<dstd::String>{v}).collect())
                  .index_names()
                  .empty());
    }
    SUBCASE("rebuild") {
        const GroupAgg sum{Agg::Sum, "v", "s"};
        DataFrame g = run(ix.lazy()
                              .group_by(dstd::Vector<dstd::String>{K1, K2},
                                        dstd::Vector<GroupAgg>{sum})
                              .collect());
        same_index(ok(ix.group_by(span_of({K1, K2}), span_of({sum}))), g);
        CHECK(index_is(g, {"k1", "k2"}));

        DataFrame r = run(ix.lazy()
                              .rename(dstd::Vector<dstd::String>{"a", "b", "c"})
                              .collect());
        CHECK(index_is(r, {"a", "b"}));

        DataFrame j =
            run(ix.lazy()
                    .join(indexed2().lazy(), dstd::Vector<dstd::String>{K1, K2})
                    .collect());
        same_index(ok(ix.join(indexed2(), span_of({K1, K2}))), j);
        CHECK(index_is(j, {"k1", "k2"}));
        DataFrame jp = run(
            ix.lazy()
                .join(make_frame().lazy(), dstd::Vector<dstd::String>{K1, K2})
                .collect());
        CHECK(jp.index_names().empty());

        DataFrame c = run(ix.lazy().concat(indexed2().lazy()).collect());
        CHECK(index_is(c, {"k1", "k2"}));
        DataFrame cp = run(ix.lazy().concat(make_frame().lazy()).collect());
        CHECK(cp.index_names().empty());

        const dstd::StringView one[] = {"k1"};
        DataFrame ix1 = ok(make_frame().with_index(one));
        DataFrame u = run(ix1.lazy()
                              .unpivot(dstd::Vector<dstd::String>{K1},
                                       dstd::Vector<dstd::String>{K2})
                              .collect());
        same_index(ok(ix1.unpivot(span_of({K1}), span_of({K2}))), u);
        DataFrame p = run(ix1.lazy().pivot("k1", "k2", "v", "sum").collect());
        CHECK(index_is(p, {"k1"}));
    }
    SUBCASE("drop") {
        CHECK(run(ix.lazy().describe().collect()).index_names().empty());
        CHECK(run(ix.lazy().with_row_index("pos").collect())
                  .index_names()
                  .empty());
    }
    SUBCASE("an unindexed plan stays unindexed") {
        CHECK(run(make_frame().lazy().head(2).collect()).index_names().empty());
    }
}

TEST_CASE("the C ABI reads and sets the index") {
    dftu_dataframe* frame = dataframe_handle_wrap(make_frame());
    CHECK(dftu_frame_index_count(frame) == 0);
    CHECK(dftu_frame_index_count(nullptr) == 0);

    const char* names[] = {"k2", "k1"};
    const std::size_t lens[] = {2, 2};
    dftu_dataframe* indexed = nullptr;
    REQUIRE(dftu_frame_with_index(frame, names, lens, 2, &indexed) ==
            DFTU_FRAME_INDEX_OK);
    REQUIRE(indexed != nullptr);
    CHECK(dftu_frame_index_count(frame) == 0);
    CHECK(dftu_frame_index_count(indexed) == 2);
    const char* ptr = nullptr;
    std::size_t len = 0;
    CHECK(dftu_frame_index_name(indexed, 0, &ptr, &len) == 0);
    CHECK(dstd::StringView(ptr, len) == "k2");
    CHECK(dftu_frame_index_name(indexed, 1, &ptr, nullptr) == 0);
    CHECK(dstd::StringView(ptr) == "k1");
    ptr = nullptr;
    CHECK(dftu_frame_index_name(indexed, 2, &ptr, &len) == -1);
    CHECK(dftu_frame_index_name(indexed, -1, &ptr, &len) == -1);
    CHECK(dftu_frame_index_name(nullptr, 0, &ptr, &len) == -1);
    CHECK(dftu_frame_index_name(indexed, 0, nullptr, &len) == -1);
    CHECK(ptr == nullptr);

    // Not NUL-terminated: only `lens` bounds the name.
    const char* prefix[] = {"k1xyz"};
    const std::size_t two[] = {2};
    dftu_dataframe* one = nullptr;
    CHECK(dftu_frame_with_index(indexed, prefix, two, 1, &one) ==
          DFTU_FRAME_INDEX_OK);
    CHECK(dftu_frame_index_count(one) == 1);
    dftu_dataframe_free(one);

    dftu_dataframe* dropped = nullptr;
    CHECK(dftu_frame_with_index(indexed, nullptr, nullptr, 0, &dropped) ==
          DFTU_FRAME_INDEX_OK);
    CHECK(dftu_frame_index_count(dropped) == 0);
    dftu_dataframe_free(dropped);

    dftu_dataframe* out = frame;
    const char* missing[] = {"nope"};
    const std::size_t four[] = {4};
    CHECK(dftu_frame_with_index(frame, missing, four, 1, &out) ==
          DFTU_FRAME_INDEX_NOT_FOUND);
    CHECK(out == nullptr);
    const char* dup[] = {"k1", "k1"};
    out = frame;
    CHECK(dftu_frame_with_index(frame, dup, lens, 2, &out) ==
          DFTU_FRAME_INDEX_INVALID);
    CHECK(out == nullptr);
    CHECK(dftu_frame_with_index(nullptr, names, lens, 2, &out) ==
          DFTU_FRAME_INDEX_BAD_ARGUMENT);
    CHECK(dftu_frame_with_index(frame, names, lens, -1, &out) ==
          DFTU_FRAME_INDEX_BAD_ARGUMENT);
    CHECK(dftu_frame_with_index(frame, nullptr, lens, 2, &out) ==
          DFTU_FRAME_INDEX_BAD_ARGUMENT);
    CHECK(dftu_frame_with_index(frame, names, lens, 2, nullptr) ==
          DFTU_FRAME_INDEX_BAD_ARGUMENT);

    // The index crosses a C ABI frame op.
    dftu_dataframe* head = dftu_dataframe_head(indexed, 2);
    REQUIRE(head != nullptr);
    CHECK(dftu_frame_index_count(head) == 2);
    dftu_dataframe_free(head);
    dftu_dataframe_free(indexed);
    dftu_dataframe_free(frame);
}

TEST_CASE("a plan declares its index with with_index") {
    LazyFrame plain = make_frame().lazy();
    CHECK(plain.index_names().empty());
    LazyFrame ix = ok(plain.with_index(dstd::Vector<dstd::String>{K1, K2}));
    CHECK(ix.index_names() == dstd::Vector<dstd::String>{K1, K2});
    CHECK(index_is(run(ix.collect()), {"k1", "k2"}));

    // The declaration applies after the ops before it, and the rules after.
    LazyFrame by_k1 =
        ok(plain.head(4).with_index(dstd::Vector<dstd::String>{K1}));
    LazyFrame kept = by_k1.filter(col(2) > std::int64_t{2});
    CHECK(kept.index_names() == dstd::Vector<dstd::String>{K1});
    CHECK(index_is(run(kept.collect()), {"k1"}));
    CHECK(by_k1.select(dstd::Vector<dstd::String>{"v"}).index_names().empty());

    CHECK(
        ok(ix.with_index(dstd::Vector<dstd::String>{})).index_names().empty());
    CHECK(run(ok(ix.with_index(dstd::Vector<dstd::String>{})).collect())
              .index_names()
              .empty());

    dftu_utils_test::expect_error(
        plain.with_index(dstd::Vector<dstd::String>{"nope"}),
        common::ErrorCode::NotFound);
    dftu_utils_test::expect_error(
        plain.with_index(dstd::Vector<dstd::String>{K1, K1}),
        common::ErrorCode::InvalidArgument);
}

TEST_CASE("without_hidden_index_columns drops only __index_level_<digits>__") {
    CHECK(is_hidden_index_name("__index_level_0__"));
    CHECK(is_hidden_index_name("__index_level_12__"));
    CHECK_FALSE(is_hidden_index_name("__index_level_x"));
    CHECK_FALSE(is_hidden_index_name("__index_level___"));
    CHECK_FALSE(is_hidden_index_name("__index_level_1x__"));
    CHECK_FALSE(is_hidden_index_name("user"));

    DataFrame df;
    df.names = dstd::make_shared_strings(
        {"user", "__index_level_0__", "__index_level_x", "m"});
    df.columns.push_back(i64s({1, 2}));
    df.columns.push_back(i64s({3, 4}));
    df.columns.push_back(i64s({5, 6}));
    df.columns.push_back(i64s({7, 8}));
    const dstd::StringView index[] = {"__index_level_0__", "__index_level_x",
                                      "m"};
    const DataFrame indexed = ok(df.with_index(index));
    const DataFrame bare = without_hidden_index_columns(indexed);
    CHECK(bare.row_index == nullptr);
    REQUIRE(bare.names.size() == 3);
    CHECK(dstd::StringView(bare.names[0]) == "user");
    CHECK(dstd::StringView(bare.names[1]) == "__index_level_x");
    CHECK(dstd::StringView(bare.names[2]) == "m");
}
