// SPDX-License-Identifier: MIT
// Registered UDAFs in the aggregation engine: group_by and LazyFrame::group_by
// over several batches, merged partial states, the thread rules of a UDAF
// without DFTU_AGG_PARALLEL_SAFE, the C ABI group-by, the errors a UDAF
// reports, and a spill of non-POD states that must equal the in-memory result.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/agg_spill.h>
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/udaf_registry.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dlfcn.h>
#include <doctest/doctest.h>

#include <cstdint>

using namespace dftracer::utils;
using dataframe::Agg;
using dataframe::AggOp;
using dataframe::AggSpec;
using dataframe::DataFrame;
using dataframe::GroupAgg;
using dataframe::Series;

#ifndef UDAF_PLUGIN_PATH
#error "UDAF_PLUGIN_PATH must be defined by CMake"
#endif

namespace {

using Ints = dstd::Vector<std::int64_t>;
using ByKey = dstd::Map<std::int64_t, double>;

bool has(dstd::StringView text, dstd::StringView word) {
    return text.find(word) != dstd::StringView::npos;
}

dstd::Optional<plugins::host::Plugins> load(const char* path) {
    auto built = plugins::host::Plugins::builder().add(path).build();
    INFO((built.has_value() ? dstd::String{}
                            : dstd::String(built.error().message())));
    REQUIRE(built.has_value());
    return std::move(*built);
}

void* plugin_symbol(const char* symbol) {
    void* h = dlopen(UDAF_PLUGIN_PATH, RTLD_NOW | RTLD_NOLOAD);
    REQUIRE(h != nullptr);
    void* fn = dlsym(h, symbol);
    dlclose(h);
    REQUIRE(fn != nullptr);
    return fn;
}

std::int64_t counter(const char* symbol) {
    return reinterpret_cast<std::int64_t (*)()>(plugin_symbol(symbol))();
}

void reset_active() {
    reinterpret_cast<void (*)()>(plugin_symbol("udaf_plugin_reset_active"))();
}

// Row i is null when `null_every` divides i + 1; zero means no nulls.
Series ints(const Ints& values, std::int64_t null_every = 0) {
    dstd::Vector<std::uint8_t> valid((values.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < values.size(); ++i)
        if (null_every == 0 ||
            (static_cast<std::int64_t>(i) + 1) % null_every != 0)
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return Series::flat_i64(values.data(),
                            static_cast<std::int64_t>(values.size()),
                            null_every == 0 ? nullptr : valid.data());
}

std::int64_t key_of(std::int64_t row, std::int64_t keys) {
    return (row * 7 + row / 3) % keys;
}

// Key k = key_of(i), value v = i % 13, every fifth value null.
DataFrame make_frame(std::int64_t rows, std::int64_t keys) {
    Ints k(static_cast<std::size_t>(rows));
    Ints v(static_cast<std::size_t>(rows));
    for (std::int64_t i = 0; i < rows; ++i) {
        k[static_cast<std::size_t>(i)] = key_of(i, keys);
        v[static_cast<std::size_t>(i)] = i % 13;
    }
    DataFrame df;
    df.names.push_back(dstd::SharedString{"k"});
    df.names.push_back(dstd::SharedString{"v"});
    df.columns.push_back(ints(k));
    df.columns.push_back(ints(v, 5));
    return df;
}

GroupAgg builtin(Agg op, const char* column, const char* out) {
    GroupAgg a;
    a.op = op;
    a.column = column;
    a.out = out;
    return a;
}

GroupAgg udaf_agg(const char* fn, const char* column, const char* out) {
    GroupAgg a;
    a.op = Agg::Udaf;
    a.fn = fn;
    a.column = column;
    a.out = out;
    return a;
}

double number(const Series& s, std::int64_t i) {
    switch (s.type()) {
        case dataframe::TypeId::Int64:
            return static_cast<double>(s.data<std::int64_t>()[i]);
        case dataframe::TypeId::Float64:
            return s.data<double>()[i];
        default:
            FAIL("a column that is not Int64 or Float64");
            return 0;
    }
}

// The values of column `name` by the Int64 key `k`, whatever the row order.
ByKey by_key(const DataFrame& df, const char* name) {
    const Series key = df.column("k").materialize();
    const Series col = df.column(name).materialize();
    REQUIRE(key.valid());
    REQUIRE(col.valid());
    ByKey out;
    for (std::int64_t i = 0; i < key.length(); ++i) {
        const std::int64_t k = key.data<std::int64_t>()[i];
        CHECK(out.find(k) == out.end());
        out[k] = number(col, i);
    }
    return out;
}

void same(const ByKey& a, const ByKey& b) {
    REQUIRE(a.size() == b.size());
    for (const auto& [k, v] : a) {
        const auto it = b.find(k);
        REQUIRE(it != b.end());
        CHECK(v == it->second);
    }
}

// What the UDAFs must give, computed by hand from make_frame's rule.
void expected(std::int64_t rows, std::int64_t keys, ByKey& sums,
              ByKey& distinct) {
    dstd::Map<std::int64_t, dstd::Set<std::int64_t>> seen;
    for (std::int64_t i = 0; i < rows; ++i) {
        const std::int64_t k = key_of(i, keys);
        sums[k] += 0;
        seen[k];
        if ((i + 1) % 5 == 0) continue;
        sums[k] += static_cast<double>(i % 13);
        seen[k].insert(i % 13);
    }
    for (const auto& [k, set] : seen)
        distinct[k] = static_cast<double>(set.size());
}

// The keys in the order the rows first show them.
Ints first_seen(std::int64_t rows, std::int64_t keys) {
    Ints order;
    dstd::Set<std::int64_t> seen;
    for (std::int64_t i = 0; i < rows; ++i)
        if (seen.insert(key_of(i, keys)).second)
            order.push_back(key_of(i, keys));
    return order;
}

DataFrame collect_lazy(const DataFrame& df, const dstd::Vector<GroupAgg>& aggs,
                       std::int64_t batch) {
    auto out = default_runtime()
                   .submit(df.lazy().group_by("k", aggs).collect(batch))
                   .get();
    INFO((out.has_value() ? dstd::String{}
                          : dstd::String(out.error().message())));
    REQUIRE(out.has_value());
    return std::move(*out);
}

// A parallel backend for the life of a test, serial after it.
struct RuntimeBackend {
    RuntimeBackend() { dataframe::install_runtime_parallel_backend(); }
    ~RuntimeBackend() { dataframe::set_parallel_backend(nullptr, nullptr); }
};

}  // namespace

TEST_CASE("group_by runs a UDAF like the built-in aggregate it mirrors") {
    auto set = load(UDAF_PLUGIN_PATH);
    const DataFrame df = make_frame(900, 7);
    ByKey want_sum, want_distinct;
    expected(900, 7, want_sum, want_distinct);

    const dstd::Vector<GroupAgg> aggs{builtin(Agg::Sum, "v", "sum"),
                                      udaf_agg("usum", "v", "usum"),
                                      builtin(Agg::Distinct, "v", "distinct"),
                                      udaf_agg("udistinct", "v", "udistinct")};
    auto out = df.group_by("k", aggs);
    REQUIRE(out.has_value());
    CHECK(out->num_rows() == 7);
    CHECK(out->column("usum").type() == dataframe::TypeId::Int64);
    same(by_key(*out, "usum"), want_sum);
    same(by_key(*out, "sum"), want_sum);
    same(by_key(*out, "udistinct"), want_distinct);
    same(by_key(*out, "distinct"), want_distinct);
    // The groups keep the order the frame first shows them.
    const Ints order = first_seen(900, 7);
    REQUIRE(static_cast<std::int64_t>(order.size()) == out->num_rows());
    for (std::int64_t i = 0; i < out->num_rows(); ++i)
        CHECK(out->column("k").data<std::int64_t>()[i] ==
              order[static_cast<std::size_t>(i)]);
}

TEST_CASE("a UDAF gives the same result over many batches of a lazy plan") {
    auto set = load(UDAF_PLUGIN_PATH);
    RuntimeBackend backend;
    const DataFrame df = make_frame(1500, 11);
    ByKey want_sum, want_distinct;
    expected(1500, 11, want_sum, want_distinct);

    for (const std::int64_t batch : {7, 64, 1500}) {
        INFO("batch " << batch);
        // usum and udistinct merge partial states, userial feeds one state.
        const DataFrame out = collect_lazy(
            df,
            {udaf_agg("usum", "v", "usum"), udaf_agg("userial", "v", "userial"),
             udaf_agg("udistinct", "v", "udistinct")},
            batch);
        same(by_key(out, "usum"), want_sum);
        same(by_key(out, "userial"), want_sum);
        same(by_key(out, "udistinct"), want_distinct);
    }
}

TEST_CASE("two states fed apart and merged equal one state fed everything") {
    auto set = load(UDAF_PLUGIN_PATH);
    const DataFrame df = make_frame(600, 9);
    const Series k = df.column("k");
    const Series v = df.column("v");
    const dstd::Vector<AggSpec> specs = [] {
        dstd::Vector<AggSpec> s(2);
        s[0].op = AggOp::Udaf;
        s[0].fn = "usum";
        s[0].value_col = 0;
        s[0].out = "usum";
        s[1].op = AggOp::Udaf;
        s[1].fn = "udistinct";
        s[1].value_col = 0;
        s[1].out = "udistinct";
        return s;
    }();
    auto fed = [&](std::int64_t begin, std::int64_t end) {
        auto st = dataframe::agg_new(specs);
        const dstd::Vector<const Series*> values{&v};
        REQUIRE(
            dataframe::agg_accumulate(*st, k, values, begin, end).has_value());
        return st;
    };
    auto whole = fed(0, 600);
    auto head = fed(0, 250);
    auto tail = fed(250, 600);
    dataframe::agg_merge(*head, *tail);
    REQUIRE(dataframe::agg_check(*head).has_value());
    CHECK(dataframe::agg_group_count(*head) == 9);
    const DataFrame merged = dataframe::agg_finalize(*head, "k");
    const DataFrame single = dataframe::agg_finalize(*whole, "k");
    same(by_key(merged, "usum"), by_key(single, "usum"));
    same(by_key(merged, "udistinct"), by_key(single, "udistinct"));
    CHECK(by_key(merged, "usum").size() == 9);

    // One state fed twice, a batch at a time, is the same again.
    auto twice = dataframe::agg_new(specs);
    const Series v_head = v.slice(0, 250);
    const Series k_head = k.slice(0, 250);
    const Series v_tail = v.slice(250, 350);
    const Series k_tail = k.slice(250, 350);
    REQUIRE(dataframe::agg_accumulate(*twice, k_head, {&v_head}).has_value());
    REQUIRE(dataframe::agg_accumulate(*twice, k_tail, {&v_tail}).has_value());
    same(by_key(dataframe::agg_finalize(*twice, "k"), "usum"),
         by_key(single, "usum"));
}

TEST_CASE("a serialized state brings its UDAF states back") {
    auto set = load(UDAF_PLUGIN_PATH);
    const DataFrame df = make_frame(400, 5);
    dstd::Vector<AggSpec> specs(2);
    specs[0].op = AggOp::Udaf;
    specs[0].fn = "udistinct";
    specs[0].value_col = 0;
    specs[0].out = "udistinct";
    specs[1].op = AggOp::Udaf;
    specs[1].fn = "usum";
    specs[1].value_col = 0;
    specs[1].out = "usum";
    auto st = dataframe::agg_new(specs);
    const Series v = df.column("v");
    REQUIRE(dataframe::agg_accumulate(*st, df.column("k"), {&v}).has_value());
    for (const bool compact : {false, true}) {
        const dstd::String blob = dataframe::agg_serialize(*st, compact);
        REQUIRE(dataframe::agg_check(*st).has_value());
        auto back = dataframe::agg_deserialize(blob, compact);
        REQUIRE(back.has_value());
        const DataFrame again = dataframe::agg_finalize(**back, "k");
        const DataFrame first = dataframe::agg_finalize(*st, "k");
        same(by_key(again, "udistinct"), by_key(first, "udistinct"));
        same(by_key(again, "usum"), by_key(first, "usum"));
        ByKey want_sum, want_distinct;
        expected(400, 5, want_sum, want_distinct);
        same(by_key(again, "usum"), want_sum);
        same(by_key(again, "udistinct"), want_distinct);
    }
}

TEST_CASE("a UDAF without the parallel flag is never entered by two threads") {
    auto set = load(UDAF_PLUGIN_PATH);
    RuntimeBackend backend;
    // Above 16 grains, with a string key of thousands of groups: the shape the
    // partitioned and by-key-hash drivers split over threads.
    constexpr std::int64_t ROWS = 16 * (std::int64_t{1} << 16) + 4096;
    constexpr std::int64_t KEYS = 3000;
    dstd::Vector<dstd::String> keys(static_cast<std::size_t>(ROWS));
    Ints v(static_cast<std::size_t>(ROWS));
    for (std::int64_t i = 0; i < ROWS; ++i) {
        keys[static_cast<std::size_t>(i)] =
            dstd::fmt<"key{}">((i * 2654435761LL) % KEYS);
        v[static_cast<std::size_t>(i)] = i % 101;
    }
    DataFrame df;
    df.names.push_back(dstd::SharedString{"k"});
    df.names.push_back(dstd::SharedString{"v"});
    df.columns.push_back(Series::strings(keys));
    df.columns.push_back(ints(v));

    auto sums = [](const DataFrame& out, const char* name) {
        dstd::Map<dstd::String, std::int64_t> m;
        const Series key = out.column("k");
        const Series col = out.column(name);
        for (std::int64_t i = 0; i < key.length(); ++i)
            m[dstd::String(key.string_at(i))] = col.data<std::int64_t>()[i];
        return m;
    };

    const std::int64_t binds_before = counter("udaf_plugin_binds");
    auto safe = df.group_by(
        "k", {builtin(Agg::Sum, "v", "sum"), udaf_agg("usum", "v", "usum")});
    REQUIRE(safe.has_value());
    // The parallel-safe UDAF got a state set for each partition.
    CHECK(counter("udaf_plugin_binds") - binds_before > 2);
    CHECK(sums(*safe, "usum") == sums(*safe, "sum"));

    reset_active();
    auto serial = df.group_by("k", {builtin(Agg::Sum, "v", "sum"),
                                    udaf_agg("userial", "v", "userial")});
    REQUIRE(serial.has_value());
    CHECK(counter("udaf_plugin_max_active") == 1);
    CHECK(sums(*serial, "userial") == sums(*serial, "sum"));
    CHECK(sums(*serial, "userial") == sums(*safe, "usum"));

    // The lazy plan folds batches on several threads for a safe UDAF and one
    // after another for the other.
    reset_active();
    const DataFrame lazy = [&] {
        auto out =
            default_runtime()
                .submit(
                    df.lazy()
                        .group_by("k", {udaf_agg("userial", "v", "userial")})
                        .collect(1 << 14))
                .get();
        REQUIRE(out.has_value());
        return std::move(*out);
    }();
    CHECK(counter("udaf_plugin_max_active") == 1);
    CHECK(sums(lazy, "userial") == sums(*safe, "usum"));
}

TEST_CASE("a spilled lazy group-by never enters a serial UDAF on two threads") {
    auto set = load(UDAF_PLUGIN_PATH);
    RuntimeBackend backend;
    constexpr std::int64_t ROWS = 6000;
    constexpr std::int64_t KEYS = 1500;
    const DataFrame df = make_frame(ROWS, KEYS);
    ByKey want_sum, want_distinct;
    expected(ROWS, KEYS, want_sum, want_distinct);

    const std::int64_t serializes_before =
        counter("udaf_plugin_serial_serializes");
    reset_active();
    auto out = default_runtime()
                   .submit(df.lazy()
                               .memory_budget(2000)
                               .group_by("k", {udaf_agg("userial", "v", "us")})
                               .collect(16))
                   .get();
    INFO((out.has_value() ? dstd::String{}
                          : dstd::String(out.error().message())));
    REQUIRE(out.has_value());
    // The states went to part files and came back.
    CHECK(counter("udaf_plugin_serial_serializes") - serializes_before >= KEYS);
    CHECK(counter("udaf_plugin_max_active") == 1);
    CHECK(out->num_rows() == KEYS);
    same(by_key(*out, "us"), want_sum);
}

TEST_CASE("group_by_dynamic runs a UDAF over its windows, eager and lazy") {
    auto set = load(UDAF_PLUGIN_PATH);
    RuntimeBackend backend;
    constexpr std::int64_t ROWS = 300;
    Ints t(ROWS), v(ROWS);
    for (std::int64_t i = 0; i < ROWS; ++i) {
        t[static_cast<std::size_t>(i)] = i;
        v[static_cast<std::size_t>(i)] = i % 13;
    }
    DataFrame df;
    df.names.push_back(dstd::SharedString{"t"});
    df.names.push_back(dstd::SharedString{"v"});
    df.columns.push_back(ints(t));
    df.columns.push_back(ints(v));
    const dstd::Vector<GroupAgg> aggs{builtin(Agg::Sum, "v", "sum"),
                                      udaf_agg("usum", "v", "usum"),
                                      udaf_agg("userial", "v", "userial")};

    reset_active();
    auto eager = df.group_by_dynamic("t", 5, 12, aggs);
    REQUIRE(eager.has_value());
    auto lazy =
        default_runtime()
            .submit(df.lazy().group_by_dynamic("t", 5, 12, aggs).collect(7))
            .get();
    REQUIRE(lazy.has_value());
    CHECK(counter("udaf_plugin_max_active") == 1);
    for (const DataFrame* out : {&*eager, &*lazy}) {
        REQUIRE(out->num_rows() > 40);
        const Series sum = out->column("sum").materialize();
        for (const char* name : {"usum", "userial"}) {
            const Series col = out->column(name).materialize();
            for (std::int64_t i = 0; i < out->num_rows(); ++i)
                CHECK(col.data<std::int64_t>()[i] ==
                      sum.data<std::int64_t>()[i]);
        }
    }
    CHECK(eager->num_rows() == lazy->num_rows());

    // A UDAF that fails is an error from both, not a column of zeros.
    const dstd::Vector<GroupAgg> failing{udaf_agg("ufail", "v", "x")};
    auto eager_fail = df.group_by_dynamic("t", 5, 12, failing);
    REQUIRE_FALSE(eager_fail.has_value());
    CHECK(has(eager_fail.error().message(), "refused on purpose"));
    auto lazy_fail =
        default_runtime()
            .submit(df.lazy().group_by_dynamic("t", 5, 12, failing).collect(7))
            .get();
    REQUIRE_FALSE(lazy_fail.has_value());
    CHECK(has(lazy_fail.error().message(), "refused on purpose"));
}

TEST_CASE("a UDAF name or call that does not fit is an error") {
    auto set = load(UDAF_PLUGIN_PATH);
    const DataFrame df = make_frame(40, 3);

    auto unknown = df.group_by("k", {udaf_agg("nope", "v", "x")});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(has(unknown.error().message(), "nope"));
    CHECK(has(unknown.error().message(), "udaf"));

    auto unnamed = df.group_by("k", {udaf_agg("", "v", "x")});
    REQUIRE_FALSE(unnamed.has_value());
    CHECK(has(unnamed.error().message(), "no function name"));

    auto missing = df.group_by("k", {udaf_agg("usum", "absent", "x")});
    REQUIRE_FALSE(missing.has_value());
    CHECK(has(missing.error().message(), "absent"));

    GroupAgg two = udaf_agg("usum", "v", "x");
    two.args.push_back("v");
    auto arity = df.group_by("k", {two});
    REQUIRE_FALSE(arity.has_value());
    CHECK(has(arity.error().message(), "usum"));
    CHECK(has(arity.error().message(), "arguments"));

    DataFrame strings;
    strings.names.push_back(dstd::SharedString{"k"});
    strings.names.push_back(dstd::SharedString{"v"});
    strings.columns.push_back(ints({1, 2}));
    strings.columns.push_back(
        Series::strings(dstd::Vector<dstd::String>{"a", "b"}));
    auto refused = strings.group_by("k", {udaf_agg("usum", "v", "x")});
    REQUIRE_FALSE(refused.has_value());
    CHECK(has(refused.error().message(), "usum"));

    // An unknown name reaches a lazy plan's collect too.
    auto lazy = default_runtime()
                    .submit(df.lazy()
                                .group_by("k", {udaf_agg("nope", "v", "x")})
                                .collect(8))
                    .get();
    REQUIRE_FALSE(lazy.has_value());
    CHECK(has(lazy.error().message(), "nope"));
}

TEST_CASE("a batch whose argument type changed is refused") {
    auto set = load(UDAF_PLUGIN_PATH);
    dstd::Vector<AggSpec> specs(1);
    specs[0].op = AggOp::Udaf;
    specs[0].fn = "usum";
    specs[0].value_col = 0;
    specs[0].out = "usum";
    auto st = dataframe::agg_new(specs);
    const Series k = ints({1, 2, 1});
    const Series as_int = ints({1, 2, 3});
    const double floats[] = {1.5, 2.5, 3.5};
    const Series as_float = Series::flat_f64(floats, 3);
    REQUIRE(dataframe::agg_accumulate(*st, k, {&as_int}).has_value());
    auto changed = dataframe::agg_accumulate(*st, k, {&as_float});
    REQUIRE_FALSE(changed.has_value());
    CHECK(has(changed.error().message(), "usum"));
    CHECK(has(changed.error().message(), "changed type"));
    CHECK(has(changed.error().message(), "int64"));
    CHECK(has(changed.error().message(), "float64"));
}

TEST_CASE("a failing UDAF callback reaches the caller with its message") {
    auto set = load(UDAF_PLUGIN_PATH);
    const DataFrame df = make_frame(40, 3);

    auto update = df.group_by("k", {udaf_agg("ufail", "v", "x")});
    REQUIRE_FALSE(update.has_value());
    CHECK(has(update.error().message(), "ufail"));
    CHECK(has(update.error().message(), "refused on purpose"));

    auto finalize = df.group_by("k", {udaf_agg("ubadrows", "v", "x")});
    REQUIRE_FALSE(finalize.has_value());
    CHECK(has(finalize.error().message(), "ubadrows"));
    CHECK(has(finalize.error().message(), "rows"));

    // A merge cannot return its error, so the state holds it.
    dstd::Vector<AggSpec> specs(1);
    specs[0].op = AggOp::Udaf;
    specs[0].fn = "ufailcombine";
    specs[0].value_col = 0;
    specs[0].out = "x";
    const Series k = df.column("k");
    const Series v = df.column("v");
    auto a = dataframe::agg_new(specs);
    auto b = dataframe::agg_new(specs);
    REQUIRE(dataframe::agg_accumulate(*a, k, {&v}, 0, 20).has_value());
    REQUIRE(dataframe::agg_accumulate(*b, k, {&v}, 20, 40).has_value());
    REQUIRE(dataframe::agg_check(*a).has_value());
    dataframe::agg_merge(*a, *b);
    auto merged = dataframe::agg_check(*a);
    REQUIRE_FALSE(merged.has_value());
    CHECK(has(merged.error().message(), "ufailcombine"));
    CHECK(has(merged.error().message(), "refused merge"));
}

TEST_CASE("the C ABI group-by resolves an unknown op name to a UDAF") {
    auto set = load(UDAF_PLUGIN_PATH);
    Ints k{1, 2, 1, 2, 1};
    Ints v{10, 20, 30, 40, 50};
    dftu_series* cols[] = {ints(k).release(), ints(v).release()};
    const char* names[] = {"k", "v"};
    dftu_dataframe* df = dftu_dataframe_new(names, cols, 2);
    REQUIRE(df != nullptr);
    const char* keys[] = {"k"};

    const dftu_group_agg aggs[] = {{"sum", "v", "sum"}, {"usum", "v", "usum"}};
    dftu_dataframe* out = dftu_dataframe_group_by(df, keys, 1, aggs, 2);
    REQUIRE(out != nullptr);
    CHECK(dftu_dataframe_num_rows(out) == 2);
    dftu_series* usum = dftu_dataframe_column(out, "usum");
    dftu_series* sum = dftu_dataframe_column(out, "sum");
    REQUIRE(usum != nullptr);
    REQUIRE(sum != nullptr);
    const auto* got = static_cast<const std::int64_t*>(dftu_series_data(usum));
    const auto* want = static_cast<const std::int64_t*>(dftu_series_data(sum));
    CHECK(got[0] == 90);
    CHECK(got[1] == 60);
    CHECK(got[0] == want[0]);
    CHECK(got[1] == want[1]);
    dftu_series_free(usum);
    dftu_series_free(sum);
    dftu_dataframe_free(out);

    const dftu_group_agg unknown[] = {{"nope", "v", "x"}};
    CHECK(dftu_dataframe_group_by(df, keys, 1, unknown, 1) == nullptr);
    const dftu_group_agg bare[] = {{"udaf", "v", "x"}};
    CHECK(dftu_dataframe_group_by(df, keys, 1, bare, 1) == nullptr);
    dftu_dataframe_free(df);
}

// A non-POD UDAF spilled and merged back, the proof the spill path serializes
// and combines the states: a run per batch under a one byte budget.
namespace {

void spill_equals_memory(const char* fn, const char* what,
                         bool counts_binds = true) {
    constexpr std::int64_t ROWS = 6000;
    constexpr std::int64_t KEYS = 1500;
    constexpr std::int64_t BATCH = 500;
    const DataFrame df = make_frame(ROWS, KEYS);
    const Series key = df.column("k");
    const Series val = df.column("v");

    dstd::Vector<AggSpec> specs(1);
    specs[0].op = AggOp::Udaf;
    specs[0].fn = fn;
    specs[0].value_col = 0;
    specs[0].out = what;

    dataframe::AggSpiller spiller(1);
    auto state = dataframe::agg_new(specs);
    const std::int64_t binds_before =
        counts_binds ? counter("udaf_plugin_binds") : 0;
    for (std::int64_t at = 0; at < ROWS; at += BATCH) {
        const Series k = key.slice(at, BATCH);
        const Series v = val.slice(at, BATCH);
        REQUIRE(dataframe::agg_accumulate(*state, k, {&v}).has_value());
        auto spilled = spiller.maybe_spill(state);
        REQUIRE(spilled.has_value());
        CHECK(*spilled);
    }
    CHECK(spiller.runs() >= 12);
    // Keys seen in more than one run are merged back from their states.
    auto drained = spiller.drain(*state, dstd::Vector<dstd::String>{"k"});
    INFO((drained.has_value() ? dstd::String{}
                              : dstd::String(drained.error().message())));
    REQUIRE(drained.has_value());
    // Reading a run back binds once for the run, not once per group in it.
    if (counts_binds) {
        const std::int64_t binds = counter("udaf_plugin_binds") - binds_before;
        INFO("binds " << binds);
        CHECK(binds < KEYS / 10);
    }

    auto memory = df.group_by("k", {udaf_agg(fn, "v", what)});
    REQUIRE(memory.has_value());
    CHECK(drained->num_rows() == KEYS);
    same(by_key(*drained, what), by_key(*memory, what));
}

}  // namespace

TEST_CASE("a spilled non-POD UDAF equals the in-memory result") {
    auto set = load(UDAF_PLUGIN_PATH);
    CHECK_FALSE(dataframe::find_udaf("udistinct")->is_pod());
    spill_equals_memory("udistinct", "distinct");
    spill_equals_memory("usum", "sum");
}

#ifdef STATS_UDAFS_PLUGIN_PATH
TEST_CASE("a spilled p50 equals the in-memory median") {
    auto set = load(STATS_UDAFS_PLUGIN_PATH);
    CHECK_FALSE(dataframe::find_udaf("p50")->is_pod());
    spill_equals_memory("p50", "median", false);
}
#endif

TEST_CASE("group_by runs a UDAF of two arguments and one of none") {
    auto set = load(UDAF_PLUGIN_PATH);
    DataFrame df = make_frame(900, 7);
    Ints w(900);
    for (std::int64_t i = 0; i < 900; ++i)
        w[static_cast<std::size_t>(i)] = i % 5 + 1;
    df.names.push_back(dstd::SharedString{"w"});
    df.columns.push_back(ints(w, 7));

    GroupAgg weighted = udaf_agg("uwsum", "v", "uwsum");
    weighted.args = {"w"};
    GroupAgg swapped = udaf_agg("uwsum", "w", "swapped");
    swapped.args = {"v"};
    GroupAgg count = udaf_agg("ucount", "", "ucount");
    const dstd::Vector<GroupAgg> aggs{weighted, swapped, count,
                                      builtin(Agg::Count, "k", "count")};
    ByKey want, want_rows;
    for (std::int64_t i = 0; i < 900; ++i) {
        const std::int64_t k = key_of(i, 7);
        want_rows[k] += 1;
        want[k] += 0;
        if ((i + 1) % 5 != 0 && (i + 1) % 7 != 0)
            want[k] += static_cast<double>((i % 13) * (i % 5 + 1));
    }
    for (const std::int64_t batch : {0, 64}) {
        INFO("batch " << batch);
        auto out = batch == 0
                       ? df.group_by("k", aggs)
                       : dstd::Result<DataFrame>(collect_lazy(df, aggs, batch));
        REQUIRE(out.has_value());
        same(by_key(*out, "uwsum"), want);
        same(by_key(*out, "swapped"), want);
        same(by_key(*out, "ucount"), want_rows);
    }
}
