// Expressions read flat, view, dictionary and chunked inputs in place and give
// the values of the same inputs joined into flat buffers.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

using Row = std::optional<std::string>;
using Rows = std::vector<Row>;
using Bounds = std::vector<std::int64_t>;

const char* const POOL[] = {"read", "write", "a name of exactly 16", "",
                            "a considerably longer name than the others"};

bool null_at(std::int64_t i) { return i % 7 == 3; }

std::vector<std::uint8_t> valid_bits(std::int64_t n) {
    std::vector<std::uint8_t> v(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t i = 0; i < n; ++i)
        if (!null_at(i))
            v[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    return v;
}

Series ints(std::int64_t n, bool nulls) {
    std::vector<std::int64_t> d;
    for (std::int64_t i = 0; i < n; ++i) d.push_back(i % 1000 - 300);
    auto v = valid_bits(n);
    return Series::flat_i64(d.data(), n, nulls ? v.data() : nullptr);
}

Series floats(std::int64_t n, bool nulls) {
    std::vector<double> d;
    for (std::int64_t i = 0; i < n; ++i)
        d.push_back(static_cast<double>(i % 97) * 0.5);
    auto v = valid_bits(n);
    return Series::flat_f64(d.data(), n, nulls ? v.data() : nullptr);
}

Series bools(std::int64_t n) {
    std::vector<std::uint8_t> bits(static_cast<std::size_t>((n + 7) / 8), 0);
    for (std::int64_t i = 0; i < n; ++i)
        if (i % 3 != 1) bits[static_cast<std::size_t>(i >> 3)] |= 1u << (i & 7);
    auto v = valid_bits(n);
    return Series::flat(TypeId::Bool, bits.data(), n, v.data());
}

Series strings(std::int64_t n, bool nulls) {
    std::vector<std::int32_t> off{0};
    std::string data;
    for (std::int64_t i = 0; i < n; ++i) {
        if (!(nulls && null_at(i))) data += POOL[(i * 3 + 1) % 5];
        off.push_back(static_cast<std::int32_t>(data.size()));
    }
    auto v = valid_bits(n);
    return Series{dftu_series_new_string(
        static_cast<dftu_dtype>(TypeId::String), off.data(), data.data(), n,
        nulls ? v.data() : nullptr)};
}

Series as_view(const Series& flat) {
    ViewBuilder b;
    b.append_column(*flat.handle());
    return b.finish(TypeId::String, false);
}

Series chunk(const Series& flat, const Bounds& bounds) {
    std::vector<std::shared_ptr<dftu_series>> parts;
    for (std::size_t k = 0; k + 1 < bounds.size(); ++k) {
        Series p{dftu_series_slice(flat.handle(), bounds[k],
                                   bounds[k + 1] - bounds[k])};
        parts.push_back(std::make_shared<dftu_series>(*p.handle()));
    }
    return Series{make_chunked(std::move(parts))};
}

std::string repr(const Series& s, std::int64_t i) {
    if (s.is_null(i)) return "<null>";
    switch (s.type()) {
        case TypeId::Int64:
            return std::to_string(s.data<std::int64_t>()[i]);
        case TypeId::Float64:
            return std::to_string(s.data<double>()[i]);
        case TypeId::Bool:
            return ((s.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) ? "T"
                                                                     : "F";
        default:
            return std::string(s.string_at(i));
    }
}

std::vector<std::string> reprs(const Series& in) {
    Series flat = in.is_flat() ? in.share() : in.materialize();
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < flat.length(); ++i)
        out.push_back(repr(flat, i));
    return out;
}

void same(const Series& got, const Series& want) {
    REQUIRE(got.length() == want.length());
    CHECK(got.type() == want.type());
    CHECK(got.null_count() == want.null_count());
    CHECK(reprs(got) == reprs(want));
}

Series joined(const Series& s) { return s.materialize(); }

std::vector<const Series*> ptrs(const std::vector<Series>& v) {
    std::vector<const Series*> p;
    for (const Series& s : v) p.push_back(&s);
    return p;
}

std::vector<Expr> roots() {
    return {col(0) + col(1), col(0) > std::int64_t{100},
            expr_str_pred(StrPredOp::Contains, col(2), "name"),
            expr_str_len(col(2), true)};
}

void compare(const std::vector<Series>& in, const std::vector<Series>& flat,
             bool expect_chunked) {
    const std::vector<Expr> rs = roots();
    const std::vector<Series> got = eval_many(rs, ptrs(in));
    const std::vector<Series> want = eval_many(rs, ptrs(flat));
    REQUIRE(got.size() == want.size());
    for (std::size_t k = 0; k < got.size(); ++k) {
        CAPTURE(k);
        same(got[k], want[k]);
        if (expect_chunked) CHECK(got[k].encoding() == Encoding::Chunked);
    }
}

dftracer::utils::dataframe::DataFrame run(
    dftracer::utils::coro::CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

}  // namespace

TEST_SUITE("eval inputs in place") {
    TEST_CASE("flat, view and dictionary inputs with nulls") {
        constexpr std::int64_t M = 1000;
        for (bool nulls : {false, true})
            for (int layout = 0; layout < 3; ++layout) {
                CAPTURE(nulls);
                CAPTURE(layout);
                Series s = strings(M, nulls);
                Series laid = layout == 1   ? as_view(s)
                              : layout == 2 ? s.dictionary_encode()
                                            : s.share();
                std::vector<Series> in;
                in.push_back(ints(M, nulls));
                in.push_back(floats(M, nulls));
                in.push_back(std::move(laid));
                std::vector<Series> flat;
                flat.push_back(ints(M, nulls));
                flat.push_back(floats(M, nulls));
                flat.push_back(std::move(s));
                compare(in, flat, false);
            }
    }

    TEST_CASE("chunked inputs with equal bounds give a chunked result") {
        constexpr std::int64_t M = 1000;
        const Bounds b{0, 130, 131, 700, M};
        std::vector<Series> in, flat;
        in.push_back(chunk(ints(M, true), b));
        in.push_back(chunk(floats(M, true), b));
        in.push_back(chunk(strings(M, true), b));
        for (const Series& c : in) flat.push_back(joined(c));
        compare(in, flat, true);
        const std::vector<Series> outs = eval_many(roots(), ptrs(in));
        const Series& out = outs[0];
        REQUIRE(out.handle()->is_chunked());
        CHECK(out.handle()->nested.size() == b.size() - 1);
        for (std::size_t k = 0; k + 1 < b.size(); ++k)
            CHECK(out.handle()->nested[k].series->length == b[k + 1] - b[k]);
    }

    TEST_CASE("chunked view and dictionary strings with equal bounds") {
        constexpr std::int64_t M = 500;
        const Bounds b{0, 200, M};
        for (int layout = 1; layout < 3; ++layout) {
            Series s = strings(M, true);
            Series laid = layout == 1 ? as_view(s) : s.dictionary_encode();
            std::vector<Series> in, flat;
            in.push_back(chunk(ints(M, true), b));
            in.push_back(chunk(floats(M, true), b));
            in.push_back(chunk(laid, b));
            for (const Series& c : in) flat.push_back(joined(c));
            compare(in, flat, true);
        }
    }

    TEST_CASE("chunked inputs with different bounds equal the joined ones") {
        constexpr std::int64_t M = 150000;
        std::vector<Series> in, flat;
        in.push_back(chunk(ints(M, true), {0, 70000, M}));
        in.push_back(chunk(floats(M, true), {0, 50000, 100000, M}));
        in.push_back(chunk(strings(M, true), {0, 65536, 65537, M}));
        for (const Series& c : in) flat.push_back(joined(c));
        compare(in, flat, false);
    }

    TEST_CASE("a chunked input mixed with a flat one") {
        constexpr std::int64_t M = 150000;
        std::vector<Series> in, flat;
        in.push_back(chunk(ints(M, true), {0, 70000, M}));
        in.push_back(floats(M, true));
        in.push_back(strings(M, true));
        for (const Series& c : in) flat.push_back(joined(c));
        compare(in, flat, false);
    }

    TEST_CASE("a gather of no rows from a chunked column is empty and typed") {
        constexpr std::int64_t M = 40;
        const Bounds b{0, 13, M};
        Series s = strings(M, true);
        const Series cols[] = {ints(M, true), floats(M, true),
                               bools(M),      s.share(),
                               as_view(s),    s.dictionary_encode()};
        for (const Series& c : cols) {
            Series ch = chunk(c, b);
            REQUIRE(ch.handle()->is_chunked());
            Series got = ch.take(std::vector<std::int64_t>{});
            CHECK(got.length() == 0);
            CHECK(got.type() == c.type());
        }
    }

    TEST_CASE("lazy filter and with_column over a collected chunked frame") {
        constexpr std::int64_t M = 200;
        DataFrame df;
        df.names = {"a", "b", "s"};
        df.columns.push_back(ints(M, true));
        df.columns.push_back(floats(M, true));
        df.columns.push_back(strings(M, true));
        DataFrame ch = run(df.lazy().collect(30));
        REQUIRE(ch.columns[0].encoding() == Encoding::Chunked);

        const Expr preds[] = {col(0) > std::int64_t{100},
                              col(0) > std::int64_t{100000}};
        for (const Expr& p : preds) {
            DataFrame got = run(ch.lazy().filter(p).collect());
            const std::vector<const Series*> in = {
                &df.columns[0], &df.columns[1], &df.columns[2]};
            DataFrame want = df.filter(eval(p, in));
            REQUIRE(got.names == want.names);
            for (std::size_t c = 0; c < want.columns.size(); ++c) {
                CAPTURE(c);
                same(got.columns[c], want.columns[c]);
            }
        }

        DataFrame got =
            run(ch.lazy().with_column("c", col(0) + col(1)).collect());
        const std::vector<const Series*> in = {&df.columns[0], &df.columns[1]};
        DataFrame want = df.with_column("c", eval(col(0) + col(1), in));
        REQUIRE(got.names == want.names);
        for (std::size_t c = 0; c < want.columns.size(); ++c) {
            CAPTURE(c);
            same(got.columns[c], want.columns[c]);
        }
    }
}
