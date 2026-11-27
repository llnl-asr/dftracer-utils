// SPDX-License-Identifier: MIT
// Gathers keep a string column's layout: a view stays a view over the same
// blobs, a dictionary stays a dictionary over the same child, and the values
// and nulls equal the flat path.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dataframe/selection_over.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/column_read.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>
#include <dftracer/utils/dataframe/kernels/dictionary.h>
#include <dftracer/utils/dataframe/kernels/field_stat.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/dstd/algorithm.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>
#include <span_list.h>
#include <task_result.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace dftracer::utils;

using namespace dftracer::utils::dataframe;

namespace {

using Row = dstd::Optional<dstd::String>;
using Rows = dstd::Vector<Row>;
using Idx = dstd::Vector<std::int64_t>;

const Rows POOL = {Row("read"),
                   Row("write"),
                   Row("a name of exactly 16"),
                   Row(),
                   Row(""),
                   Row("0123456789abcdef"),
                   Row("a considerably longer name than the others")};

Rows pool_rows(std::size_t n) {
    Rows r;
    for (std::size_t i = 0; i < n; ++i) r.push_back(POOL[(i * 3 + 1) % 7]);
    return r;
}

Series flat_of(const Rows& rows) {
    dstd::Vector<dstd::StringView> v;
    dstd::Vector<std::uint8_t> valid((rows.size() + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        v.emplace_back(rows[i] ? dstd::StringView(*rows[i])
                               : dstd::StringView());
        if (rows[i])
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            any_null = true;
    }
    return Series::strings(v, any_null ? valid.data() : nullptr);
}

Series view_of(const Rows& rows) {
    Series flat = flat_of(rows);
    ViewBuilder b;
    b.append_column(*flat.handle());
    return b.finish(TypeId::String, false);
}

// A DICTIONARY column of `type` over `values`, with an Arrow bitmap of the
// rows' own validity (null = no own nulls).
Series dict_over(TypeId type, dstd::Span<const std::int32_t> codes,
                 Series&& values, const std::uint8_t* validity) {
    Series out =
        Series::flat(TypeId::Int32, codes.data(),
                     static_cast<std::int64_t>(codes.size()), validity);
    dftu_series* h = out.handle();
    h->type = type;
    h->params = TypeParams{};
    h->encoding = Encoding::Dictionary;
    h->set_child(adopt_series(values.release()));
    return out;
}

Rows rows_of(const Series& s) {
    Rows r;
    for (std::int64_t i = 0; i < s.length(); ++i)
        r.push_back(s.is_null(i) ? Row() : Row(dstd::String(s.string_at(i))));
    return r;
}

Rows pick(const Rows& rows, const Idx& idx) {
    Rows r;
    for (std::int64_t k : idx)
        r.push_back(k < 0 ? Row() : rows[static_cast<std::size_t>(k)]);
    return r;
}

Series mask_of(dstd::Span<const std::uint8_t> v) {
    dstd::Vector<std::uint8_t> bits((v.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return Series::flat(TypeId::Bool, bits.data(),
                        static_cast<std::int64_t>(v.size()));
}

enum class Kind { Flat, View, DictFlat, DictView, SelFlat, SelView, SelDict };

struct Case {
    const char* name;
    Kind kind;
    Series col;
    Rows rows;
    const void* shared;
};

const void* blobs_of(const Series& s) { return s.handle()->blobs.get(); }
const void* child_data_of(const Series& s) {
    return s.handle()->child()->data.data();
}

dstd::Vector<Case> cases() {
    const Rows base = pool_rows(120);
    const Idx sel = {5, 5, -1, 17, 0, 119, 64, 3, 3, 99, 40, 41, 42, 7};
    dstd::Vector<Case> out;
    out.push_back({"flat", Kind::Flat, flat_of(base), base, nullptr});
    Series v = view_of(base);
    out.push_back({"view", Kind::View, view_of(base), base, nullptr});
    out.back().shared = blobs_of(out.back().col);
    Series d = flat_of(base).dictionary_encode();
    out.push_back(
        {"dict over flat", Kind::DictFlat, std::move(d), base, nullptr});
    out.back().shared = child_data_of(out.back().col);
    Series dv = flat_of(base).dictionary_encode();
    {
        Series values{dftu_series_share(dv.handle()->child().get())};
        const_cast<dftu_series*>(dv.handle())
            ->set_child(adopt_series(view_of(rows_of(values)).release()));
    }
    out.push_back(
        {"dict over view", Kind::DictView, std::move(dv), base, nullptr});
    out.back().shared = child_data_of(out.back().col);
    out.push_back({"selection over flat", Kind::SelFlat,
                   selection_over(flat_of(base), sel), pick(base, sel),
                   nullptr});
    out.push_back({"selection over view", Kind::SelView, selection_over(v, sel),
                   pick(base, sel), nullptr});
    out.back().shared = blobs_of(v);
    Series sd = flat_of(base).dictionary_encode();
    out.push_back({"selection over dict", Kind::SelDict,
                   selection_over(sd, sel), pick(base, sel), nullptr});
    out.back().shared = child_data_of(sd);
    return out;
}

void check_layout(const Case& c, const Series& got) {
    switch (c.kind) {
        case Kind::View:
        case Kind::SelView:
            CHECK(got.encoding() == Encoding::View);
            CHECK(blobs_of(got) == c.shared);
            break;
        case Kind::DictFlat:
        case Kind::DictView:
        case Kind::SelDict:
            CHECK(got.encoding() == Encoding::Dictionary);
            CHECK(child_data_of(got) == c.shared);
            break;
        case Kind::SelFlat:
            CHECK(got.encoding() == Encoding::Flat);
            break;
        case Kind::Flat:
            CHECK(got.encoding() == Encoding::Flat);
            break;
    }
}

Idx gather_idx() { return {7, 7, -1, 0, 13, 11, -1, 6, 2, 2, 10}; }

dstd::String truth_of(const Series& m) {
    dstd::String out;
    for (std::int64_t i = 0; i < m.length(); ++i) {
        if (m.is_null(i))
            out += 'N';
        else
            out +=
                ((m.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) ? 'T' : 'F';
    }
    return out;
}

dstd::String ints_of(const Series& m) {
    dstd::String out;
    for (std::int64_t i = 0; i < m.length(); ++i)
        out += dstd::fmt<"{},">(
            (m.is_null(i) ? dstd::String("N")
                          : dstd::to_string(m.data<std::int64_t>()[i])));
    return out;
}

using StrFn = dftu_series* (*)(const dftu_series*, const char*, std::int32_t);

struct Pred {
    const char* name;
    StrFn fn;
    const char* arg;
};

const Pred PREDS[] = {
    {"str_eq", dftu_series_str_eq, "read"},
    {"str_eq empty", dftu_series_str_eq, ""},
    {"contains", dftu_series_str_contains, "name"},
    {"starts_with", dftu_series_str_starts_with, "a "},
    {"ends_with", dftu_series_str_ends_with, "e"},
    {"matches", dftu_series_str_matches, "[a-z]+ ?.*"},
    {"search", dftu_series_str_search, "^re|f$"},
    {"like", dftu_series_str_like, "%name%"},
    {"like literal", dftu_series_str_like, "a %"},
};

const Pred INT_PREDS[] = {
    {"find", dftu_series_str_find, "name"},
    {"count", dftu_series_str_count, "e"},
};

const dftu_cmp_op CMP_OPS[] = {DFTU_CMP_EQ, DFTU_CMP_NE, DFTU_CMP_LT,
                               DFTU_CMP_LE, DFTU_CMP_GT, DFTU_CMP_GE};

Series run(StrFn fn, const Series& col, dstd::CStringView arg) {
    return Series{
        fn(col.handle(), arg.c_str(), static_cast<std::int32_t>(arg.size()))};
}

Series cmp_str(const Series& col, dftu_cmp_op op, dstd::StringView lit) {
    dftu_scalar rhs{};
    rhs.kind = DFTU_SCALAR_TAG_STR;
    rhs.len = static_cast<std::uint32_t>(lit.size());
    rhs.value.s = lit.data();
    return Series{dftu_series_compare(col.handle(), op, rhs)};
}

dstd::String in_truth(const Series& col, dstd::StringView query) {
    DataFrame f;
    f.names = dstd::make_shared_strings({"name"});
    f.columns.push_back(col.share());
    auto q = dftracer::utils::duql::Query::from_string(query);
    REQUIRE(q.has_value());
    return truth_of(dftu_utils_test::ok(
        evaluate_mask(duql::detail::QueryAccess::root(*q), f)));
}

}  // namespace

TEST_SUITE("kernels keep layouts") {
    TEST_CASE("filter of a 3-value dictionary keeps the dictionary") {
        dstd::Vector<dstd::String> cats;
        for (int i = 0; i < 1000; ++i)
            cats.push_back(i % 3 == 0 ? "read" : i % 3 == 1 ? "write" : "open");
        dstd::Vector<std::int64_t> ids;
        dstd::Vector<std::uint8_t> keep;
        for (int i = 0; i < 1000; ++i) {
            ids.push_back(i);
            keep.push_back(i % 10 == 3);
        }
        Series flat = Series::strings(cats);
        DataFrame df;
        df.names = dstd::make_shared_strings({"cat", "id"});
        df.columns.push_back(flat.dictionary_encode());
        df.columns.push_back(Series::flat_i64(ids.data(), 1000));
        DataFrame flat_df;
        flat_df.names = dstd::make_shared_strings({"cat", "id"});
        flat_df.columns.push_back(Series::strings(cats));
        flat_df.columns.push_back(Series::flat_i64(ids.data(), 1000));

        Series mask = mask_of(keep);
        DataFrame got = df.filter(mask);
        DataFrame want = flat_df.filter(mask);
        REQUIRE(got.num_rows() == 100);
        const Series& cat = got.columns[0];
        CHECK(cat.encoding() == Encoding::Dictionary);
        CHECK(child_data_of(cat) == child_data_of(df.columns[0]));
        CHECK(rows_of(cat) == rows_of(want.columns[0]));
    }

    TEST_CASE("Series take, head, tail and filter keep layout, equal flat") {
        for (const Case& c : cases()) {
            CAPTURE(c.name);
            const Idx idx = gather_idx();
            Series t = c.col.take(idx);
            check_layout(c, t);
            CHECK(rows_of(t) == pick(c.rows, idx));

            const auto n = static_cast<std::int64_t>(c.rows.size());
            Idx h, tl;
            for (std::int64_t i = 0; i < 9; ++i) {
                h.push_back(i);
                tl.push_back(n - 9 + i);
            }
            Series hs = c.col.head(9);
            check_layout(c, hs);
            CHECK(rows_of(hs) == pick(c.rows, h));
            Series ts = c.col.tail(9);
            check_layout(c, ts);
            CHECK(rows_of(ts) == pick(c.rows, tl));

            dstd::Vector<std::uint8_t> keep;
            Idx kept;
            for (std::int64_t i = 0; i < n; ++i) {
                keep.push_back(i % 4 == 1);
                if (i % 4 == 1) kept.push_back(i);
            }
            Series f = c.col.filter(mask_of(keep));
            CHECK(rows_of(f) == pick(c.rows, kept));
            if (c.kind == Kind::DictFlat || c.kind == Kind::DictView)
                check_layout(c, f);
        }
    }

    TEST_CASE("take32 and take_all keep layout") {
        for (const Case& c : cases()) {
            CAPTURE(c.name);
            const Idx idx = gather_idx();
            dstd::Vector<std::int32_t> idx32(idx.begin(), idx.end());
            Series t = take32(c.col, idx32);
            check_layout(c, t);
            CHECK(rows_of(t) == pick(c.rows, idx));

            dstd::Vector<Series> cols;
            cols.push_back(c.col.share());
            cols.push_back(c.col.share());
            auto out = take_all(cols, idx.data(),
                                static_cast<std::int64_t>(idx.size()));
            REQUIRE(out.size() == 2);
            for (const Series& s : out) {
                check_layout(c, s);
                CHECK(rows_of(s) == pick(c.rows, idx));
            }
        }
    }

    TEST_CASE("validity-only ops and drop_nulls equal the flat path") {
        for (const Case& c : cases()) {
            CAPTURE(c.name);
            Series flat = flat_of(c.rows);
            const auto n = static_cast<std::int64_t>(c.rows.size());
            std::int64_t valid = 0;
            for (const Row& r : c.rows) valid += r.has_value();
            CHECK(c.col.count() == valid);
            Series nm = c.col.null_mask();
            Series vm = c.col.valid_mask();
            for (std::int64_t i = 0; i < n; ++i) {
                const unsigned nb =
                    (nm.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1u;
                const unsigned vb =
                    (vm.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1u;
                CHECK(nb + vb == 1u);
                CHECK((vb == 1u) ==
                      c.rows[static_cast<std::size_t>(i)].has_value());
            }
            Series dn = c.col.drop_nulls();
            Rows want;
            for (const Row& r : c.rows)
                if (r) want.push_back(r);
            CHECK(rows_of(dn) == want);
            CHECK(rows_of(dn) == rows_of(flat.drop_nulls()));
            if (c.kind == Kind::View || c.kind == Kind::DictFlat ||
                c.kind == Kind::DictView)
                check_layout(c, dn);
        }
    }

    TEST_CASE("DataFrame gathers, sort and join payload keep layout") {
        const Rows base = pool_rows(120);
        dstd::Vector<std::int64_t> id(120), key(120);
        for (int i = 0; i < 120; ++i) {
            id[static_cast<std::size_t>(i)] = (i * 37) % 120;
            key[static_cast<std::size_t>(i)] = i % 7;
        }
        auto frame = [&](Series col) {
            DataFrame df;
            df.names = dstd::make_shared_strings({"s", "id", "key"});
            df.columns.push_back(std::move(col));
            df.columns.push_back(Series::flat_i64(id.data(), 120));
            df.columns.push_back(Series::flat_i64(key.data(), 120));
            return df;
        };
        dstd::Vector<std::uint8_t> keep;
        for (int i = 0; i < 120; ++i) keep.push_back(i % 5 == 0);
        for (const Case& c : cases()) {
            if (c.kind == Kind::Flat || c.kind == Kind::SelFlat) continue;
            if (c.col.length() != 120) continue;
            CAPTURE(c.name);
            DataFrame df = frame(c.col.share());
            DataFrame flat = frame(flat_of(c.rows));
            auto same = [&](const DataFrame& got, const DataFrame& want) {
                REQUIRE(got.num_rows() == want.num_rows());
                CHECK(rows_of(got.columns[0]) == rows_of(want.columns[0]));
                check_layout(c, got.columns[0]);
            };
            same(df.filter(mask_of(keep)), flat.filter(mask_of(keep)));
            same(df.head(10), flat.head(10));
            same(df.tail(10), flat.tail(10));
            same(df.slice(20, 30), flat.slice(20, 30));
            same(
                df.take(dftu_utils_test::span_of<std::int64_t>({3, -1, 3, 80})),
                flat.take(
                    dftu_utils_test::span_of<std::int64_t>({3, -1, 3, 80})));
            same(dftu_utils_test::ok(df.sort_by("id")),
                 dftu_utils_test::ok(flat.sort_by("id")));
            same(dftu_utils_test::ok(df.sort_by_multi(
                     dftu_utils_test::span_of<dstd::String>({"key", "id"}),
                     true)),
                 dftu_utils_test::ok(flat.sort_by_multi(
                     dftu_utils_test::span_of<dstd::String>({"key", "id"}),
                     true)));

            DataFrame right;
            right.names = dstd::make_shared_strings({"key", "w"});
            dstd::Vector<std::int64_t> rk = {1, 2, 2, 9}, rw = {10, 20, 21, 90};
            right.columns.push_back(Series::flat_i64(rk.data(), 4));
            right.columns.push_back(Series::flat_i64(rw.data(), 4));
            for (JoinHow how : {JoinHow::Inner, JoinHow::Left}) {
                DataFrame j = dftu_utils_test::ok(df.join(
                    right, dftu_utils_test::span_of<dstd::String>({"key"}),
                    how));
                DataFrame jf = dftu_utils_test::ok(flat.join(
                    right, dftu_utils_test::span_of<dstd::String>({"key"}),
                    how));
                REQUIRE(j.num_rows() == jf.num_rows());
                CHECK(rows_of(j.columns[0]) == rows_of(jf.columns[0]));
                check_layout(c, j.columns[0]);
            }
        }
    }

    TEST_CASE("a dictionary entry that is null makes its rows null") {
        const dstd::Vector<dstd::StringView> vals{"a", "", "c"};
        const std::uint8_t child_valid = 0b101;
        const dstd::Vector<std::int32_t> codes{0, 1, 0, 2};
        Series d = dictionary_from_codes(
            codes,
            Series::strings(dstd::Span<const dstd::StringView>(vals),
                            &child_valid),
            nullptr);
        REQUIRE(d.encoding() == Encoding::Dictionary);
        CHECK_FALSE(d.is_null(0));
        CHECK(d.is_null(1));
        CHECK(d.string_at(3) == "c");

        const Series valid{dftu_series_valid_mask(d.handle())};
        const Series nulls{dftu_series_null_mask(d.handle())};
        const std::uint8_t vb = valid.data<std::uint8_t>()[0];
        const std::uint8_t nb = nulls.data<std::uint8_t>()[0];
        CHECK((vb & 0x0F) == 0b1101);
        CHECK((nb & 0x0F) == 0b0010);
        CHECK(dftu_series_count(d.handle()) == 3);
        const Series kept{dftu_series_drop_nulls(d.handle())};
        CHECK(kept.length() == 3);
    }

    TEST_CASE("string predicates and lengths equal the flat path per layout") {
        for (const Case& c : cases()) {
            CAPTURE(c.name);
            const Series flat = flat_of(c.rows);
            for (const Pred& p : PREDS) {
                CAPTURE(p.name);
                const Series got = run(p.fn, c.col, p.arg);
                REQUIRE(got.handle() != nullptr);
                CHECK(truth_of(got) == truth_of(run(p.fn, flat, p.arg)));
            }
            for (const Pred& p : INT_PREDS) {
                CAPTURE(p.name);
                const Series got = run(p.fn, c.col, p.arg);
                REQUIRE(got.handle() != nullptr);
                CHECK(ints_of(got) == ints_of(run(p.fn, flat, p.arg)));
            }
            CHECK(ints_of(Series{dftu_series_str_len_bytes(c.col.handle())}) ==
                  ints_of(Series{dftu_series_str_len_bytes(flat.handle())}));
            CHECK(ints_of(Series{dftu_series_str_len_chars(c.col.handle())}) ==
                  ints_of(Series{dftu_series_str_len_chars(flat.handle())}));
        }
    }

    TEST_CASE("starts_with over a 3-entry dictionary equals the flat mask") {
        dstd::Vector<dstd::String> cats;
        for (int i = 0; i < 1000; ++i)
            cats.push_back(i % 3 == 0 ? "POSIX" : i % 3 == 1 ? "STDIO" : "PO");
        dstd::Vector<dstd::StringView> views(cats.begin(), cats.end());
        const Series flat = Series::strings(views);
        const Series dict = flat.dictionary_encode();
        REQUIRE(dict.encoding() == Encoding::Dictionary);
        const Series got = run(dftu_series_str_starts_with, dict, "PO");
        CHECK(got.encoding() == Encoding::Flat);
        CHECK(truth_of(got) ==
              truth_of(run(dftu_series_str_starts_with, flat, "PO")));
    }

    TEST_CASE("a null dictionary entry gives null predicate rows") {
        const dstd::Vector<dstd::StringView> vals{"apple", "", "pear"};
        const std::uint8_t child_valid = 0b101;
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 0, 1};
        const Series d = dictionary_from_codes(
            codes,
            Series::strings(dstd::Span<const dstd::StringView>(vals),
                            &child_valid),
            nullptr);
        CHECK(truth_of(run(dftu_series_str_starts_with, d, "a")) == "TNFTN");
        CHECK(ints_of(run(dftu_series_str_find, d, "p")) == "1,N,0,1,N,");
        CHECK(truth_of(cmp_str(d, DFTU_CMP_LT, "b")) == "TNFTN");
    }

    TEST_CASE("ordered scalar compares go by byte order") {
        const Rows rows = {Row("apple"), Row("zoo"), Row("m"), Row()};
        const Series flat = flat_of(rows);
        CHECK(truth_of(cmp_str(flat, DFTU_CMP_LT, "m")) == "TFFN");
        CHECK(truth_of(cmp_str(flat, DFTU_CMP_LE, "m")) == "TFTN");
        CHECK(truth_of(cmp_str(flat, DFTU_CMP_GT, "m")) == "FTFN");
        CHECK(truth_of(cmp_str(flat, DFTU_CMP_GE, "m")) == "FTTN");
        CHECK(truth_of(cmp_str(flat, DFTU_CMP_EQ, "m")) == "FFTN");
        CHECK(truth_of(cmp_str(flat, DFTU_CMP_NE, "m")) == "TTFN");
        const Rows prefix = {Row("ab"), Row("abc"), Row("b"), Row("")};
        CHECK(truth_of(cmp_str(flat_of(prefix), DFTU_CMP_LT, "abc")) == "TFFT");
        CHECK(truth_of(cmp_str(flat_of(prefix), DFTU_CMP_GT, "ab")) == "FTTF");
    }

    TEST_CASE("scalar compares equal the flat path per layout") {
        for (const Case& c : cases()) {
            CAPTURE(c.name);
            const Series flat = flat_of(c.rows);
            for (const char* lit : {"read", "", "a name of exactly 16", "x"})
                for (dftu_cmp_op op : CMP_OPS) {
                    const Series got = cmp_str(c.col, op, lit);
                    REQUIRE(got.handle() != nullptr);
                    CHECK(truth_of(got) == truth_of(cmp_str(flat, op, lit)));
                }
        }
    }

    TEST_CASE("in over every layout equals the flat path") {
        dstd::Vector<dstd::String> lits;
        for (int i = 0; i < 18; ++i) lits.push_back(dstd::fmt<"lit{}">(i));
        lits.push_back("read");
        lits.push_back("a name of exactly 16");
        for (const std::size_t n :
             {std::size_t{1}, std::size_t{2}, lits.size()}) {
            dstd::String list = dstd::fmt<"\"{}\"">(lits[lits.size() - 1]);
            if (n == 1) list = "\"read\"";
            if (n == 2) list = "\"read\", \"write\"";
            if (n == lits.size()) {
                list.clear();
                for (const dstd::String& l : lits)
                    list +=
                        (list.empty() ? "" : ", ") + (dstd::fmt<"\"{}\"">(l));
            }
            for (const char* head : {"name in [", "name not in ["}) {
                const dstd::String q = dstd::fmt<"{}{}]">(head, list);
                CAPTURE(q);
                for (const Case& c : cases()) {
                    CAPTURE(c.name);
                    CHECK(in_truth(c.col, q) == in_truth(flat_of(c.rows), q));
                }
            }
        }
        const Series f = flat_of(pool_rows(7));
        CHECK(in_truth(f, "name in [\"read\", \"write\"]").size() == 7);
        CHECK(in_truth(flat_of({Row("read"), Row("x"), Row()}),
                       "name in [\"read\", \"write\"]") == "TFN");
        CHECK(in_truth(flat_of({Row("read"), Row("x"), Row()}),
                       "name < \"s\"") == "TFN");
    }
}

namespace {

constexpr std::int64_t SCALE_ROWS = 1'100'000;
constexpr std::int64_t SCALE_GROUPS = 5000;

struct Backend {
    Backend() { install_runtime_parallel_backend(); }
} backend;

struct Frame {
    dstd::Vector<Row> keys;
    dstd::Vector<std::int64_t> a, b, c;
};

Frame scale_frame(bool long_keys) {
    Frame f;
    for (std::int64_t i = 0; i < SCALE_ROWS; ++i) {
        const std::int64_t g = (i * 7919) % SCALE_GROUPS;
        if (g % 97 == 0)
            f.keys.emplace_back();
        else
            f.keys.emplace_back(dstd::fmt<"{}{}">(
                (long_keys ? "a group key longer than sixteen bytes " : "k"),
                g));
        f.a.push_back(i % 1000);
        f.b.push_back((i * 31) % 977);
        f.c.push_back(i % 13);
    }
    return f;
}

dstd::Vector<dstd::String> int_cells(const DataFrame& df) {
    dstd::Vector<dstd::String> out;
    for (const Series& col : df.columns)
        for (std::int64_t i = 0; i < col.length(); ++i) {
            if (col.is_null(i))
                out.push_back("<null>");
            else if (col.type() == TypeId::String)
                out.emplace_back(col.string_at(i));
            else if (col.type() == TypeId::Int64)
                out.push_back(dstd::to_string(col.data<std::int64_t>()[i]));
        }
    return out;
}

dstd::Vector<AggSpec> scale_specs() {
    auto s = [](AggOp op, std::int32_t col, dstd::StringView out) {
        AggSpec sp;
        sp.op = op;
        sp.value_col = col;
        sp.out = out;
        return sp;
    };
    return {s(AggOp::Count, -1, "n"),  s(AggOp::Sum, 0, "sum_a"),
            s(AggOp::Min, 1, "min_b"), s(AggOp::Max, 2, "max_c"),
            s(AggOp::Var, 0, "var_a"), s(AggOp::Var, 1, "var_b"),
            s(AggOp::Var, 2, "var_c")};
}

void scale_case(bool long_keys) {
    const Frame f = scale_frame(long_keys);
    const Series flat = flat_of(f.keys);
    const Series a = Series::flat_i64(f.a.data(), SCALE_ROWS);
    const Series b = Series::flat_i64(f.b.data(), SCALE_ROWS);
    const Series c = Series::flat_i64(f.c.data(), SCALE_ROWS);
    const dstd::Vector<const Series*> values{&a, &b, &c};
    const auto names = dstd::Vector<dstd::String>{"k"};
    const DataFrame want = dftu_utils_test::ok(group_agg(
        dftu_utils_test::span_of<const dftracer::utils::dataframe::Series*>(
            {&flat}),
        values, scale_specs(), names));
    REQUIRE(want.num_rows() > 2048);
    ViewBuilder vb;
    vb.append_column(*flat.handle());
    const Series view = vb.finish(TypeId::String, false);
    const Series dict = flat.dictionary_encode();
    REQUIRE(view.encoding() == Encoding::View);
    REQUIRE(dict.encoding() == Encoding::Dictionary);
    for (const Series* key : {&view, &dict}) {
        const DataFrame got = dftu_utils_test::ok(group_agg(
            dftu_utils_test::span_of<const dftracer::utils::dataframe::Series*>(
                {key}),
            values, scale_specs(), names));
        CHECK(int_cells(got) == int_cells(want));
    }
}

}  // namespace

TEST_SUITE("kernels keep layouts: group-by drivers and size checks") {
    TEST_CASE("many groups on a short dictionary or view key equal flat") {
        scale_case(false);
    }

    TEST_CASE("many groups on a long dictionary or view key equal flat") {
        scale_case(true);
    }

    TEST_CASE("string bytes past INT32_MAX never fit int32 offsets") {
        constexpr std::uint64_t MAX = std::numeric_limits<std::int32_t>::max();
        CHECK(fits_int32_offsets(0));
        CHECK(fits_int32_offsets(MAX));
        CHECK_FALSE(fits_int32_offsets(MAX + 1));
        Series::check_string_bytes(MAX);
    }

    TEST_CASE("offsets from lengths refuse a total past the offset width") {
        constexpr std::int32_t MAX = std::numeric_limits<std::int32_t>::max();
        dstd::Vector<std::int32_t> fits = {0, MAX / 2, MAX / 2, 1};
        REQUIRE(offsets_from_lengths(fits.data(), 3));
        CHECK(fits[3] == MAX);
        CHECK(fits[1] == MAX / 2);
        dstd::Vector<std::int32_t> over = {0, MAX / 2, MAX / 2, 2};
        CHECK_FALSE(offsets_from_lengths(over.data(), 3));
        dstd::Vector<std::int64_t> wide = {0, MAX, MAX, MAX};
        REQUIRE(offsets_from_lengths(wide.data(), 3));
        CHECK(wide[3] == std::int64_t{MAX} * 3);
    }
    TEST_CASE("sorts by a string key of any layout equal the general sort") {
        constexpr std::size_t N = 100000;
        constexpr std::size_t DISTINCT = 37;
        auto text = [](std::size_t j) {
            dstd::String t = dstd::fmt<"name_{}">((j * 7919) % 1000);
            if (j % 5 == 0) t += " with a tail longer than twelve bytes";
            return t;
        };
        dstd::Vector<dstd::String> child;
        for (std::size_t j = 0; j < DISTINCT; ++j) child.push_back(text(j));
        child.push_back(child[3]);
        child.push_back(child[11]);
        child.push_back("");
        const std::size_t null_entry = child.size();
        child.push_back("never read");
        dstd::Vector<std::uint8_t> child_valid((child.size() + 7) / 8, 0xFF);
        child_valid[null_entry >> 3] &=
            static_cast<std::uint8_t>(~(1u << (null_entry & 7)));
        dstd::Vector<dstd::StringView> views(child.begin(), child.end());

        dstd::Vector<std::int32_t> codes(N);
        dstd::Vector<std::uint8_t> row_valid((N + 7) / 8, 0xFF);
        dstd::Vector<std::int64_t> ids(N), other(N);
        Rows rows(N);
        for (std::size_t i = 0; i < N; ++i) {
            codes[i] = static_cast<std::int32_t>((i * 2654435761u >> 7) %
                                                 child.size());
            if (i % 13 == 0)
                row_valid[i >> 3] &=
                    static_cast<std::uint8_t>(~(1u << (i & 7)));
            ids[i] = static_cast<std::int64_t>(i);
            other[i] = static_cast<std::int64_t>((i * 31) % 5);
            const bool null_row =
                i % 13 == 0 || static_cast<std::size_t>(codes[i]) == null_entry;
            rows[i] = null_row ? Row()
                               : Row(child[static_cast<std::size_t>(codes[i])]);
        }
        Series dict = dictionary_from_codes(
            codes,
            Series::strings(dstd::Span<const dstd::StringView>(views),
                            child_valid.data()),
            row_valid.data());
        REQUIRE(dict.encoding() == Encoding::Dictionary);
        REQUIRE(rows_of(dict) == rows);

        Idx sel(N);
        for (std::size_t i = 0; i < N; ++i)
            sel[i] =
                i % 11 == 0 ? -1 : static_cast<std::int64_t>((i * 48271) % N);
        struct Key {
            const char* name;
            Series col;
            Rows rows;
        };
        dstd::Vector<Key> keys;
        keys.push_back({"dictionary", dict.share(), rows});
        keys.push_back({"flat", flat_of(rows), rows});
        keys.push_back({"view", view_of(rows), rows});
        Rows unique(N);
        for (std::size_t i = 0; i < N; ++i)
            unique[i] = i % 13 == 0   ? Row()
                        : i % 50 == 0 ? Row("duplicated")
                                      : Row(dstd::fmt<"u{}">((i * 7919) % N));
        keys.push_back({"view mostly unique", view_of(unique), unique});
        keys.push_back({"selection over flat",
                        selection_over(flat_of(rows), sel), pick(rows, sel)});
        keys.push_back({"selection over dictionary", selection_over(dict, sel),
                        pick(rows, sel)});

        auto expected = [&](const Rows& r, bool desc, bool with_other) {
            Idx idx(N);
            for (std::size_t i = 0; i < N; ++i)
                idx[i] = static_cast<std::int64_t>(i);
            dstd::stable_sort(
                idx.begin(), idx.end(), [&](std::int64_t a, std::int64_t b) {
                    const Row& x = r[static_cast<std::size_t>(a)];
                    const Row& y = r[static_cast<std::size_t>(b)];
                    if (!x != !y) return static_cast<bool>(x);
                    if (x && *x != *y) return desc ? *y < *x : *x < *y;
                    if (!with_other) return false;
                    const std::int64_t oa = other[static_cast<std::size_t>(a)];
                    const std::int64_t ob = other[static_cast<std::size_t>(b)];
                    return desc ? ob < oa : oa < ob;
                });
            return idx;
        };
        auto ids_of = [](const Series& s) {
            Idx r(static_cast<std::size_t>(s.length()));
            for (std::int64_t i = 0; i < s.length(); ++i)
                r[static_cast<std::size_t>(i)] = s.data<std::int64_t>()[i];
            return r;
        };
        for (const Key& k : keys) {
            CAPTURE(k.name);
            DataFrame df;
            df.names = dstd::make_shared_strings({"s", "id", "o"});
            df.columns.push_back(k.col.share());
            df.columns.push_back(Series::flat_i64(ids.data(), N));
            df.columns.push_back(Series::flat_i64(other.data(), N));
            for (bool desc : {false, true}) {
                CAPTURE(desc);
                const Idx one = expected(k.rows, desc, false);
                CHECK(ids_of(dftu_utils_test::ok(df.sort_by("s", desc))
                                 .columns[1]) == one);
                CHECK(ids_of(
                          dftu_utils_test::ok(
                              df.sort_by_multi(
                                  dftu_utils_test::span_of<dstd::String>({"s"}),
                                  desc))
                              .columns[1]) == one);
                CHECK(ids_of(k.col.argsort(desc)) == one);
                CHECK(ids_of(dftu_utils_test::ok(
                                 df.sort_by_multi(
                                     dftu_utils_test::span_of<dstd::String>(
                                         {"s", "o"}),
                                     desc))
                                 .columns[1]) == expected(k.rows, desc, true));
            }
        }
    }

    TEST_CASE("materialize and argsort read nested selection and dictionary") {
        const dstd::Vector<dstd::StringView> vals{"a", "b", "c"};
        const dstd::Vector<std::int32_t> codes{0, 1, 0, 2};
        const Series dict = dictionary_from_codes(
            codes, Series::strings(dstd::Span<const dstd::StringView>(vals)),
            nullptr);

        auto* sel = new dftu_series();
        sel->type = TypeId::String;
        sel->encoding = Encoding::Selection;
        sel->length = 3;
        sel->data = new_buffer(3 * sizeof(std::int64_t));
        const std::int64_t idx[3] = {3, 0, -1};
        std::memcpy(writable(sel->data), idx, sizeof idx);
        sel->set_child(adopt_series(dftu_series_share(dict.handle())));
        const Series over_dict{sel};

        const Series flat = over_dict.materialize();
        REQUIRE(flat.valid());
        CHECK(flat.string_at(0) == "c");
        CHECK(flat.string_at(1) == "a");
        CHECK(flat.is_null(2));
        CHECK(over_dict.argsort().valid());

        auto* outer = new dftu_series();
        outer->type = TypeId::String;
        outer->encoding = Encoding::Dictionary;
        outer->length = 2;
        outer->data = new_buffer(2 * sizeof(std::int32_t));
        const std::int32_t outer_codes[2] = {1, 0};
        std::memcpy(writable(outer->data), outer_codes, sizeof outer_codes);
        outer->set_child(adopt_series(dftu_series_share(over_dict.handle())));
        const Series dict_over_sel{outer};
        const Series flat2 = dict_over_sel.materialize();
        REQUIRE(flat2.valid());
        CHECK(flat2.string_at(0) == "a");
        CHECK(flat2.string_at(1) == "c");
    }
}

namespace {

using UnaryFn = dftu_series* (*)(const dftu_series*);

struct ElementwiseOp {
    const char* name;
    UnaryFn fn;
};

Series dictionary_over(dstd::Span<const std::int32_t> codes, Series values,
                       const std::uint8_t* validity) {
    Series out =
        Series::flat(TypeId::Int32, codes.data(),
                     static_cast<std::int64_t>(codes.size()), validity);
    dftu_series* h = out.handle();
    adopt_type_from(*h, *values.handle());
    h->encoding = Encoding::Dictionary;
    h->set_child(adopt_series(values.release()));
    return out;
}

dstd::Vector<std::uint8_t> valid_except(std::size_t n,
                                        dstd::Span<const std::size_t> nulls) {
    dstd::Vector<std::uint8_t> bits((n + 7) / 8, 0xFF);
    for (std::size_t i : nulls)
        bits[i >> 3] &= static_cast<std::uint8_t>(~(1u << (i & 7)));
    return bits;
}

dstd::String column_text(const Series& s) {
    const Series flat =
        s.encoding() == Encoding::Flat ? s.share() : s.materialize();
    REQUIRE(flat.valid());
    dstd::String out;
    for (std::int64_t i = 0; i < s.length(); ++i) {
        if (s.is_null(i)) {
            out += "N,";
            continue;
        }
        switch (flat.type()) {
            case TypeId::String:
            case TypeId::LargeString:
            case TypeId::Binary:
            case TypeId::LargeBinary:
                out.append(flat.string_at(i));
                break;
            case TypeId::Bool:
                out += ((flat.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1)
                           ? 'T'
                           : 'F';
                break;
            case TypeId::Int64:
            case TypeId::Timestamp:
            case TypeId::Duration:
                out += dstd::fmt<"{}">(flat.data<std::int64_t>()[i]);
                break;
            case TypeId::Decimal128:
                out +=
                    dstd::fmt<"{}:{}">(flat.data<std::uint64_t>()[2 * i],
                                       flat.data<std::uint64_t>()[2 * i + 1]);
                break;
            case TypeId::Uint64:
                out += dstd::fmt<"{}">(flat.data<std::uint64_t>()[i]);
                break;
            case TypeId::Float64: {
                std::uint64_t bits = 0;
                std::memcpy(&bits, flat.data<double>() + i, sizeof bits);
                out += dstd::fmt<"{}">(bits);
                break;
            }
            default:
                FAIL("a result type the test does not read");
        }
        out += ',';
    }
    return out;
}

void check_elementwise(const ElementwiseOp& op, const Series& dict,
                       bool bool_result = false) {
    CAPTURE(op.name);
    const Series flat_in = dict.materialize();
    REQUIRE(flat_in.valid());
    const Series want{op.fn(flat_in.handle())};
    const Series got{op.fn(dict.handle())};
    REQUIRE(want.valid());
    REQUIRE(got.valid());
    CHECK(got.type() == want.type());
    CHECK(static_cast<int>(got.handle()->time_unit()) ==
          static_cast<int>(want.handle()->time_unit()));
    CHECK(got.handle()->timezone() == want.handle()->timezone());
    CHECK(got.handle()->decimal_precision() ==
          want.handle()->decimal_precision());
    CHECK(got.handle()->decimal_scale() == want.handle()->decimal_scale());
    CHECK(got.handle()->fixed_size() == want.handle()->fixed_size());
    CHECK(got.handle()->json() == want.handle()->json());
    CHECK(got.length() == dict.length());
    if (bool_result) {
        CHECK(got.encoding() == Encoding::Flat);
    } else {
        CHECK(got.encoding() == Encoding::Dictionary);
        CHECK(got.handle()->data.data() == dict.handle()->data.data());
    }
    CHECK(column_text(got) == column_text(want));
    CHECK(got.null_count() == want.null_count());
}

dftu_series* lower_fn(const dftu_series* v) {
    return dftu_series_to_lowercase(v);
}
dftu_series* upper_fn(const dftu_series* v) {
    return dftu_series_to_uppercase(v);
}
dftu_series* replace_fn(const dftu_series* v) {
    return dftu_series_str_replace(v, "a", 1, "b", 1);
}
dftu_series* replace_all_fn(const dftu_series* v) {
    return dftu_series_str_replace_all(v, "e", 1, "33", 2);
}
dftu_series* extract_fn(const dftu_series* v) {
    return dftu_series_str_extract(v, "[a-z]+", 6, 0);
}
dftu_series* regex_replace_fn(const dftu_series* v) {
    return dftu_series_str_regex_replace(v, "[0-9]", 5, "#", 1);
}
dftu_series* slice_fn(const dftu_series* v) {
    return dftu_series_str_slice(v, 1, 3);
}
dftu_series* pad_start_fn(const dftu_series* v) {
    return dftu_series_str_pad_start(v, 8, '*');
}
dftu_series* pad_end_fn(const dftu_series* v) {
    return dftu_series_str_pad_end(v, 8, '*');
}
dftu_series* center_fn(const dftu_series* v) {
    return dftu_series_str_center(v, 9, '*');
}
dftu_series* zfill_fn(const dftu_series* v) {
    return dftu_series_str_zfill(v, 6);
}
dftu_series* case_fn(const dftu_series* v) {
    return dftu_series_str_case(v, DFTU_STR_TITLE_CASE);
}
dftu_series* remove_prefix_fn(const dftu_series* v) {
    return dftu_series_str_remove_prefix(v, "a-", 2);
}
dftu_series* remove_suffix_fn(const dftu_series* v) {
    return dftu_series_str_remove_suffix(v, "2", 1);
}
dftu_series* repeat_fn(const dftu_series* v) {
    return dftu_series_str_repeat(v, 2);
}
dftu_series* normalize_fn(const dftu_series* v) {
    return dftu_series_str_normalize(v, DFTU_NORM_NFD);
}
dftu_series* translate_fn(const dftu_series* v) {
    static const Series from =
        Series::strings(dstd::Span<const dstd::StringView>(
            dstd::Vector<dstd::StringView>{"a", "e"}));
    static const Series to = Series::strings(dstd::Span<const dstd::StringView>(
        dstd::Vector<dstd::StringView>{"4", "3"}));
    return dftu_series_str_translate(v, from.handle(), to.handle());
}
dftu_series* wrap_fn(const dftu_series* v) {
    return dftu_series_str_wrap(v, 4, 0);
}

const ElementwiseOp STRING_OPS[] = {
    {"to_lowercase", lower_fn},
    {"to_uppercase", upper_fn},
    {"strip", dftu_series_str_strip},
    {"lstrip", dftu_series_str_lstrip},
    {"rstrip", dftu_series_str_rstrip},
    {"replace", replace_fn},
    {"replace_all", replace_all_fn},
    {"str_extract", extract_fn},
    {"str_regex_replace", regex_replace_fn},
    {"str_slice", slice_fn},
    {"pad_start", pad_start_fn},
    {"pad_end", pad_end_fn},
    {"center", center_fn},
    {"zfill", zfill_fn},
    {"case", case_fn},
    {"remove_prefix", remove_prefix_fn},
    {"remove_suffix", remove_suffix_fn},
    {"repeat", repeat_fn},
    {"normalize", normalize_fn},
    {"translate", translate_fn},
    {"wrap", wrap_fn},
    {"hex64_parse", dftu_series_hex64_parse},
    {"fnv1a", dftu_series_fnv1a},
};

dftu_series* clip_fn(const dftu_series* v) {
    return dftu_series_clip(v, f64(-1.0), f64(3.0));
}
dftu_series* add_scalar_fn(const dftu_series* v) {
    return dftu_series_add_scalar(v, i64(5));
}
dftu_series* sub_scalar_fn(const dftu_series* v) {
    return dftu_series_sub_scalar(v, f64(0.5));
}
dftu_series* mul_scalar_fn(const dftu_series* v) {
    return dftu_series_mul_scalar(v, i64(3));
}
dftu_series* div_scalar_fn(const dftu_series* v) {
    return dftu_series_div_scalar(v, i64(4));
}
dftu_series* floordiv_scalar_fn(const dftu_series* v) {
    return dftu_series_floordiv_scalar(v, i64(3), 0);
}
dftu_series* mod_scalar_fn(const dftu_series* v) {
    return dftu_series_mod_scalar(v, i64(3), 0);
}
dftu_series* pow_scalar_fn(const dftu_series* v) {
    return dftu_series_pow_scalar(v, i64(2), 0);
}
dftu_series* cast_f64_fn(const dftu_series* v) {
    return dftu_series_cast(v, DFTU_TYPE_FLOAT64);
}
dftu_series* cast_bool_fn(const dftu_series* v) {
    return dftu_series_cast(v, DFTU_TYPE_BOOL);
}
dftu_series* cast_string_fn(const dftu_series* v) {
    return dftu_series_cast(v, DFTU_TYPE_STRING);
}

const ElementwiseOp NUMERIC_OPS[] = {
    {"abs", dftu_series_abs},
    {"clip", clip_fn},
    {"round", dftu_series_round},
    {"ceil", dftu_series_ceil},
    {"floor", dftu_series_floor},
    {"trunc", dftu_series_trunc},
    {"sign", dftu_series_sign},
    {"negate", dftu_series_negate},
    {"sqrt", dftu_series_sqrt},
    {"exp", dftu_series_exp},
    {"log", dftu_series_log},
    {"log1p", dftu_series_log1p},
    {"expm1", dftu_series_expm1},
    {"sin", dftu_series_sin},
    {"cos", dftu_series_cos},
    {"tan", dftu_series_tan},
    {"cot", dftu_series_cot},
    {"arcsin", dftu_series_arcsin},
    {"arccos", dftu_series_arccos},
    {"arctan", dftu_series_arctan},
    {"sinh", dftu_series_sinh},
    {"cosh", dftu_series_cosh},
    {"tanh", dftu_series_tanh},
    {"arcsinh", dftu_series_arcsinh},
    {"arccosh", dftu_series_arccosh},
    {"arctanh", dftu_series_arctanh},
    {"cbrt", dftu_series_cbrt},
    {"log10", dftu_series_log10},
    {"erf", dftu_series_erf},
    {"erfc", dftu_series_erfc},
    {"add_scalar", add_scalar_fn},
    {"sub_scalar", sub_scalar_fn},
    {"mul_scalar", mul_scalar_fn},
    {"div_scalar", div_scalar_fn},
    {"floordiv_scalar", floordiv_scalar_fn},
    {"mod_scalar", mod_scalar_fn},
    {"pow_scalar", pow_scalar_fn},
    {"cast to Float64", cast_f64_fn},
    {"cast to String", cast_string_fn},
};

const ElementwiseOp MASK_OPS[] = {
    {"is_nan", dftu_series_is_nan},
    {"is_finite", dftu_series_is_finite},
    {"is_infinite", dftu_series_is_infinite},
};

dstd::Vector<Series> string_dictionaries() {
    const Rows rows = {Row(" Read "),  Row("WRITE"),  Row("a-12"),
                       Row(""),        Row("  x"),    Row("Zz9"),
                       Row("ff"),      Row(),         Row("10"),
                       Row("xyz abc"), Row("a-12"),   Row("ff"),
                       Row("WRITE"),   Row(" Read "), Row("caf\xc3\xa9")};
    dstd::Vector<Series> out;
    Series encoded = flat_of(rows).dictionary_encode();
    REQUIRE(encoded.encoding() == Encoding::Dictionary);
    REQUIRE(encoded.handle()->null_count == 1);
    out.push_back(std::move(encoded));

    const dstd::Vector<dstd::StringView> vals{"AaEe", " a ", "", "10", "ff"};
    const std::uint8_t entry_valid = 0b10111;
    const dstd::Vector<std::int32_t> codes{0, 1, 2, 3, 4, 1, 0, 3,
                                           2, 4, 1, 3, 0, 4, 2};
    const std::size_t own_nulls[] = {5, 12};
    const auto own = valid_except(codes.size(), own_nulls);
    out.push_back(dictionary_over(
        codes,
        Series::strings(dstd::Span<const dstd::StringView>(vals), &entry_valid),
        own.data()));
    REQUIRE(out.back().handle()->child()->null_count == 1);
    REQUIRE(out.back().handle()->null_count == 2);
    return out;
}

}  // namespace

TEST_SUITE("dictionary fast path for elementwise ops") {
    TEST_CASE(
        "string ops over a dictionary equal the flat path and keep codes") {
        for (const Series& dict : string_dictionaries())
            for (const ElementwiseOp& op : STRING_OPS)
                check_elementwise(op, dict);
    }

    TEST_CASE("numeric ops over a Float64 dictionary equal the flat path") {
        const dstd::Vector<double> vals{
            -1.5, 2.25, std::nan(""),
            0.0,  9.0,  std::numeric_limits<double>::infinity()};
        const std::uint8_t entry_valid = 0b110111;
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 3, 4, 5, 1, 0,
                                               3, 4, 2, 5, 1, 0, 5};
        const std::size_t own_nulls[] = {6, 10};
        const auto own = valid_except(codes.size(), own_nulls);
        const Series dict = dictionary_over(
            codes, Series::flat(TypeId::Float64, vals.data(), 6, &entry_valid),
            own.data());
        REQUIRE(dict.is_null(3));
        REQUIRE(dict.is_null(6));
        for (const ElementwiseOp& op : NUMERIC_OPS) check_elementwise(op, dict);
        for (const ElementwiseOp& op : MASK_OPS)
            check_elementwise(op, dict, true);
        check_elementwise({"cast to Bool", cast_bool_fn}, dict, true);
    }

    TEST_CASE("numeric ops over an Int64 dictionary equal the flat path") {
        const dstd::Vector<std::int64_t> vals{-3, 0, 7, 100, 5};
        const std::uint8_t entry_valid = 0b10111;
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 3, 4, 2, 1, 0, 3, 4};
        const std::size_t own_nulls[] = {4};
        const auto own = valid_except(codes.size(), own_nulls);
        const Series dict = dictionary_over(
            codes, Series::flat_i64(vals.data(), 5, &entry_valid), own.data());
        for (const ElementwiseOp& op : NUMERIC_OPS) check_elementwise(op, dict);
        for (const ElementwiseOp& op : MASK_OPS)
            check_elementwise(op, dict, true);
    }

    TEST_CASE("a result child with nulls is seen by every null reader") {
        const dstd::Vector<dstd::StringView> vals{"a1", "b", "a2"};
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 1, 0};
        const Series dict = dictionary_over(
            codes, Series::strings(dstd::Span<const dstd::StringView>(vals)),
            nullptr);
        const Series got{dftu_series_str_extract(dict.handle(), "[0-9]", 5, 0)};
        REQUIRE(got.encoding() == Encoding::Dictionary);
        CHECK(got.handle()->null_count == 0);
        CHECK(got.handle()->rowwise_nulls());
        CHECK(got.null_count() == 2);
        CHECK(got.is_null(1));
        CHECK_FALSE(got.is_null(2));
        CHECK(dftu_series_count(got.handle()) == 3);
        CHECK(truth_of(Series{dftu_series_null_mask(got.handle())}) == "FTFTF");
        CHECK(Series{dftu_series_drop_nulls(got.handle())}.length() == 3);
    }

    TEST_CASE("a cast failure makes the dictionary entry null") {
        const dstd::Vector<dstd::StringView> vals{"12", "x", "7"};
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 1, 0, 2};
        const Series dict = dictionary_over(
            codes, Series::strings(dstd::Span<const dstd::StringView>(vals)),
            nullptr);
        const Series got{dftu_series_cast(dict.handle(), DFTU_TYPE_INT64)};
        REQUIRE(got.encoding() == Encoding::Dictionary);
        REQUIRE(got.type() == TypeId::Int64);
        CHECK(got.handle()->null_count == 0);
        CHECK(got.null_count() == 2);
        CHECK(column_text(got) == "12,N,7,N,12,7,");
    }

    TEST_CASE("a Bool result expands through the codes with both null kinds") {
        const dstd::Vector<double> vals{1.0, std::nan(""), 3.0};
        const std::uint8_t entry_valid = 0b101;
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 0, 2, 1};
        const std::size_t own_nulls[] = {3};
        const auto own = valid_except(codes.size(), own_nulls);
        const Series dict = dictionary_over(
            codes, Series::flat(TypeId::Float64, vals.data(), 3, &entry_valid),
            own.data());
        const Series got{dftu_series_is_nan(dict.handle())};
        REQUIRE(got.encoding() == Encoding::Flat);
        CHECK(got.null_count() == 3);
        CHECK(truth_of(got) == "FNFNFN");
    }

    TEST_CASE("dictionary_encode of a dictionary re-encodes and dedupes") {
        const dstd::Vector<dstd::StringView> vals{"a", "A", "b"};
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 1, 0};
        const Series dict = dictionary_over(
            codes, Series::strings(dstd::Span<const dstd::StringView>(vals)),
            nullptr);
        const Series lowered{dftu_series_to_lowercase(dict.handle())};
        REQUIRE(lowered.encoding() == Encoding::Dictionary);
        REQUIRE(lowered.handle()->child()->length == 3);
        const Series again = lowered.dictionary_encode();
        REQUIRE(again.valid());
        CHECK(again.encoding() == Encoding::Dictionary);
        CHECK(again.handle()->child()->length == 2);
        CHECK(again.handle()->child()->encoding == Encoding::Flat);
        CHECK(rows_of(again) == rows_of(lowered));
    }
}

namespace {

dftu_series* floordiv_rev_fn(const dftu_series* v) {
    return dftu_series_floordiv_scalar(v, i64(7), 1);
}
dftu_series* mod_rev_fn(const dftu_series* v) {
    return dftu_series_mod_scalar(v, i64(7), 1);
}
dftu_series* pow_rev_fn(const dftu_series* v) {
    return dftu_series_pow_scalar(v, i64(2), 1);
}
dftu_series* floordiv_zero_fn(const dftu_series* v) {
    return dftu_series_floordiv_scalar(v, i64(0), 0);
}
dftu_series* mod_zero_fn(const dftu_series* v) {
    return dftu_series_mod_scalar(v, i64(0), 0);
}
dftu_series* pow_negative_fn(const dftu_series* v) {
    return dftu_series_pow_scalar(v, i64(-2), 0);
}

const ElementwiseOp DIVISION_OPS[] = {
    {"floordiv_scalar reverse", floordiv_rev_fn},
    {"mod_scalar reverse", mod_rev_fn},
    {"pow_scalar reverse", pow_rev_fn},
    {"floordiv_scalar by zero", floordiv_zero_fn},
    {"mod_scalar by zero", mod_zero_fn},
    {"pow_scalar negative exponent", pow_negative_fn},
};

void check_same_values(const ElementwiseOp& op, const Series& dict) {
    CAPTURE(op.name);
    const Series flat_in = dict.materialize();
    REQUIRE(flat_in.valid());
    const Series want{op.fn(flat_in.handle())};
    const Series got{op.fn(dict.handle())};
    REQUIRE(want.valid());
    REQUIRE(got.valid());
    CHECK(got.type() == want.type());
    CHECK(column_text(got) == column_text(want));
    CHECK(got.null_count() == want.null_count());
}

Series int_dictionary() {
    const dstd::Vector<std::int64_t> vals{-3, 0, 7, 2, 100};
    const std::uint8_t entry_valid = 0b10111;
    const dstd::Vector<std::int32_t> codes{0, 1, 2, 3, 4, 2, 1, 0, 3, 4, 1, 1};
    const std::size_t own_nulls[] = {5};
    const auto own = valid_except(codes.size(), own_nulls);
    return dictionary_over(
        codes, Series::flat_i64(vals.data(), 5, &entry_valid), own.data());
}

Series text_key_dictionary(std::size_t rows, std::size_t plain_entries) {
    dstd::Vector<dstd::String> names;
    for (std::size_t k = 0; k < plain_entries; ++k)
        names.push_back(dstd::fmt<"a{}">(k));
    names.push_back("");
    names.push_back("gone");
    dstd::Vector<dstd::StringView> vals(names.begin(), names.end());
    const std::size_t entries = vals.size();
    dstd::Vector<std::uint8_t> entry_valid((entries + 7) / 8, 0xFF);
    const std::size_t gone = entries - 1;
    entry_valid[gone >> 3] &= static_cast<std::uint8_t>(~(1u << (gone & 7)));
    dstd::Vector<std::int32_t> codes;
    dstd::Vector<std::size_t> own_nulls;
    for (std::size_t i = 0; i < rows; ++i) {
        codes.push_back(static_cast<std::int32_t>((i * 7) % entries));
        if (i % 1009 == 5) own_nulls.push_back(i);
    }
    const auto own = valid_except(rows, own_nulls);
    return dictionary_over(
        codes,
        Series::strings(dstd::Span<const dstd::StringView>(vals),
                        entry_valid.data()),
        own.data());
}

dstd::Vector<dstd::String> grouped(const Series& key) {
    dstd::Vector<std::int64_t> ids;
    for (std::int64_t i = 0; i < key.length(); ++i) ids.push_back(i % 17);
    DataFrame df;
    df.names = dstd::make_shared_strings({"k", "v"});
    df.columns.push_back(key.share());
    df.columns.push_back(Series::flat_i64(ids.data(), key.length()));
    const GroupAgg aggs[] = {{Agg::Count, "", "cnt", 0.0},
                             {Agg::Sum, "v", "sum", 0.0}};
    DataFrame out = dftu_utils_test::ok(df.group_by("k", aggs));
    const Series counts = out.column("cnt");
    const Series sums = out.column("sum");
    const Series keys = out.column("k");
    dstd::Vector<dstd::String> rows;
    for (std::int64_t i = 0; i < out.num_rows(); ++i) {
        const Series one_count = counts.slice(i, 1);
        const Series one_sum = sums.slice(i, 1);
        dstd::String row = keys.is_null(i) ? dstd::String("N")
                                           : dstd::String(keys.string_at(i));
        row += '|';
        row += column_text(one_count);
        row += column_text(one_sum);
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

}  // namespace

TEST_SUITE("dictionary nulls, ranges and layouts") {
    TEST_CASE(
        "group_by on a dictionary key reads a null entry as the null key") {
        const Series dict = text_key_dictionary(300000, 3000);
        REQUIRE(dict.handle()->child()->null_count == 1);
        const Series flat = dict.materialize();
        const auto want = grouped(flat);
        const auto got = grouped(dict);
        REQUIRE(want.size() == got.size());
        CHECK(got == want);
        bool null_group = false, empty_group = false;
        for (const dstd::String& row : got) {
            null_group |= row.rfind("N|", 0) == 0;
            empty_group |= row.rfind("|", 0) == 0;
        }
        CHECK(null_group);
        CHECK(empty_group);
    }

    TEST_CASE("group_by on the result of an op that adds child nulls") {
        const Series dict = text_key_dictionary(300000, 3000);
        const Series extracted{
            dftu_series_str_extract(dict.handle(), "a([0-9]*)", 9, 1)};
        REQUIRE(extracted.encoding() == Encoding::Dictionary);
        const Series flat = extracted.materialize();
        CHECK(grouped(extracted) == grouped(flat));
    }

    TEST_CASE("an out-of-range code under an own null row is not read") {
        const dstd::Vector<dstd::StringView> vals{"Ab", "CD"};
        const dstd::Vector<std::int32_t> codes{0, -1, 99999, 1, 0};
        const std::size_t own_nulls[] = {1, 2};
        const auto own = valid_except(codes.size(), own_nulls);
        const Series strings = dictionary_over(
            codes, Series::strings(dstd::Span<const dstd::StringView>(vals)),
            own.data());
        for (const ElementwiseOp& op : STRING_OPS)
            if (dstd::StringView(op.name) != "hex64_parse")
                check_elementwise(op, strings);

        const dstd::Vector<double> nums{1.0, std::nan("")};
        const Series floats = dictionary_over(
            codes, Series::flat(TypeId::Float64, nums.data(), 2), own.data());
        for (const ElementwiseOp& op : MASK_OPS)
            check_elementwise(op, floats, true);
        check_elementwise({"abs", dftu_series_abs}, floats);
    }

    TEST_CASE("an out-of-range code on a valid row is refused") {
        const dstd::Vector<dstd::StringView> vals{"Ab", "CD"};
        const dstd::Vector<double> nums{1.0, std::nan("")};
        for (std::int32_t bad : {-1, 2, 99999}) {
            CAPTURE(bad);
            const dstd::Vector<std::int32_t> codes{0, bad, 1};
            const Series strings = dictionary_over(
                codes,
                Series::strings(dstd::Span<const dstd::StringView>(vals)),
                nullptr);
            const Series floats = dictionary_over(
                codes, Series::flat(TypeId::Float64, nums.data(), 2), nullptr);
            CHECK(dftu_series_to_lowercase(strings.handle()) == nullptr);
            CHECK(dftu_series_abs(floats.handle()) == nullptr);
            CHECK(dftu_series_is_nan(floats.handle()) == nullptr);
        }
    }

    TEST_CASE("scalar division and power forms equal the flat path") {
        const Series dict = int_dictionary();
        for (const ElementwiseOp& op : DIVISION_OPS)
            check_elementwise(op, dict);
    }

    TEST_CASE("a view child or a nested dictionary child takes the flat path") {
        const Rows rows = pool_rows(30);
        const Series encoded = flat_of(rows).dictionary_encode();
        const Series values{dftu_series_share(encoded.handle()->child().get())};

        Series over_view{dftu_series_share(encoded.handle())};
        over_view.handle()->set_child(
            adopt_series(view_of(rows_of(values)).release()));
        REQUIRE(over_view.handle()->child()->encoding == Encoding::View);

        Series inner = flat_of(rows_of(values)).dictionary_encode();
        const Series nested =
            dictionary_over(dstd::Span<const std::int32_t>(
                                reinterpret_cast<const std::int32_t*>(
                                    encoded.handle()->data.data()),
                                static_cast<std::size_t>(encoded.length())),
                            std::move(inner), nullptr);
        REQUIRE(nested.handle()->child()->encoding == Encoding::Dictionary);

        const Series* const layouts[] = {&over_view, &nested};
        for (const Series* col : layouts)
            for (const ElementwiseOp& op : STRING_OPS) {
                if (dstd::StringView(op.name) == "hex64_parse") continue;
                check_same_values(op, *col);
            }
    }

    TEST_CASE("a dictionary over a child that is a dictionary with nulls") {
        const dstd::Vector<dstd::StringView> vals{"x", "y"};
        const std::uint8_t entry_valid = 0b01;
        const dstd::Vector<std::int32_t> inner_codes{0, 1, 0};
        const dstd::Vector<std::int32_t> outer_codes{2, 1, 0, 1, 2};
        const Series inner = dictionary_over(
            inner_codes,
            Series::strings(dstd::Span<const dstd::StringView>(vals),
                            &entry_valid),
            nullptr);
        const Series outer =
            dictionary_over(outer_codes, inner.share(), nullptr);
        CHECK(outer.handle()->null_count == 0);
        CHECK(outer.handle()->rowwise_nulls());
        CHECK(outer.null_count() == 2);
        CHECK(outer.is_null(1));
        CHECK_FALSE(outer.is_null(0));
        CHECK(dftu_series_count(outer.handle()) == 3);
        CHECK(column_text(outer) == "x,N,x,N,x,");

        const Idx sel = {0, -1, 1};
        const Series flat = flat_of({Row("p"), Row("q")});
        const Series selected = selection_over(flat, sel);
        const dstd::Vector<std::int32_t> selection_codes{0, 1, 2, 1};
        const Series over_selection =
            dictionary_over(selection_codes, selected.share(), nullptr);
        CHECK(over_selection.null_count() == 2);
        CHECK(over_selection.is_null(1));
        CHECK_FALSE(over_selection.is_null(2));
        CHECK(over_selection.is_null(3));
    }

    TEST_CASE("Large and Binary dictionaries keep the flat path's type") {
        const dstd::Vector<dstd::StringView> vals{"Ab", " c ", "ff"};
        const std::uint8_t entry_valid = 0b101;
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 1, 0, 2, 1};
        const std::size_t own_nulls[] = {3};
        const auto own = valid_except(codes.size(), own_nulls);
        Series wide = Series::strings(dstd::Span<const dstd::StringView>(vals),
                                      &entry_valid)
                          .cast(TypeId::LargeString);
        REQUIRE(wide.type() == TypeId::LargeString);
        const Series large =
            dictionary_over(codes, std::move(wide), own.data());
        for (const ElementwiseOp& op : STRING_OPS) {
            if (dstd::StringView(op.name) == "hex64_parse") continue;
            check_elementwise(op, large);
        }

        const std::int32_t offsets[] = {0, 2, 3, 5};
        const char bytes[] = {'a', 'b', '\xff', 'c', 'd'};
        const std::uint8_t binary_valid = 0b111;
        Series bin{dftu_series_new_string(DFTU_TYPE_BINARY, offsets, bytes, 3,
                                          &binary_valid)};
        REQUIRE(bin.valid());
        const Series binary =
            dictionary_over(codes, std::move(bin), own.data());
        check_elementwise({"cast to String", cast_string_fn}, binary);
        const Series text{dftu_series_cast(binary.handle(), DFTU_TYPE_STRING)};
        REQUIRE(text.valid());
        CHECK(text.null_count() == 3);
        CHECK(text.handle()->null_count == 1);
    }

    TEST_CASE("Timestamp, Duration and Decimal128 keep type and parameters") {
        const std::int64_t ticks[] = {10, 20, 30, 40};
        const std::uint8_t entry_valid = 0b0111;
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 3, 1, 0};
        const std::size_t own_nulls[] = {4};
        const auto own = valid_except(codes.size(), own_nulls);

        auto checked_ops = [&](const Series& dict) {
            int checked = 0;
            const Series flat_in = dict.materialize();
            REQUIRE(flat_in.valid());
            for (const ElementwiseOp& op : NUMERIC_OPS) {
                const Series want{op.fn(flat_in.handle())};
                if (!want.valid()) continue;
                check_elementwise(op, dict);
                ++checked;
            }
            return checked;
        };

        Series ts = Series::flat_i64(ticks, 4, &entry_valid);
        ts.handle()->type = TypeId::Timestamp;
        ts.handle()->set_time_unit(TimeUnit::Milli);
        ts.handle()->set_timezone("Europe/Paris");
        CHECK(checked_ops(dictionary_over(codes, std::move(ts), own.data())) >
              0);

        Series dur = Series::flat_i64(ticks, 4, &entry_valid);
        dur.handle()->type = TypeId::Duration;
        dur.handle()->set_time_unit(TimeUnit::Micro);
        CHECK(checked_ops(dictionary_over(codes, std::move(dur), own.data())) >
              0);

        const std::uint8_t dec[64] = {
            7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        Series d = Series::flat(TypeId::Decimal128, dec, 4, &entry_valid);
        d.handle()->set_decimal(20, 3);
        CHECK(checked_ops(dictionary_over(codes, std::move(d), own.data())) >
              0);
    }

    TEST_CASE("empty and zero-entry dictionaries expand a Bool result") {
        const dstd::Vector<double> nums{1.0, std::nan("")};
        const dstd::Vector<std::int32_t> none;
        const Series empty = dictionary_over(
            none, Series::flat(TypeId::Float64, nums.data(), 2), nullptr);
        for (const ElementwiseOp& op : MASK_OPS) {
            const Series got{op.fn(empty.handle())};
            REQUIRE(got.valid());
            CHECK(got.length() == 0);
            CHECK(got.encoding() == Encoding::Flat);
        }
        const Series wrapped{dftu_series_abs(empty.handle())};
        REQUIRE(wrapped.valid());
        CHECK(wrapped.length() == 0);

        const dstd::Vector<std::int32_t> garbage{0, 5, -4};
        const std::size_t all[] = {0, 1, 2};
        const auto own = valid_except(3, all);
        const Series no_entries = dictionary_over(
            garbage, Series::flat(TypeId::Float64, nullptr, 0), own.data());
        for (const ElementwiseOp& op : MASK_OPS) {
            const Series got{op.fn(no_entries.handle())};
            REQUIRE(got.valid());
            CHECK(got.encoding() == Encoding::Flat);
            CHECK(got.null_count() == 3);
        }
        const Series kept{dftu_series_abs(no_entries.handle())};
        REQUIRE(kept.valid());
        CHECK(kept.null_count() == 3);
    }

    TEST_CASE("a JSON-marked dictionary cast to String equals the flat path") {
        const dstd::Vector<dstd::StringView> vals{"1", "[2]"};
        const dstd::Vector<std::int32_t> codes{0, 1, 0};
        const Series plain = dictionary_over(
            codes, Series::strings(dstd::Span<const dstd::StringView>(vals)),
            nullptr);
        const Series marked{dftu_series_mark_json(plain.handle())};
        REQUIRE(marked.valid());
        REQUIRE(marked.handle()->json());
        check_elementwise({"cast to String", cast_string_fn}, marked);
    }
}

namespace {

using Cell = dstd::Optional<double>;
using Cells = dstd::Vector<Cell>;

struct Layout {
    dstd::StringView name;
    Series col;
    Cells cells;
};

// A FLAT Float64 or Int64 column. `under` is stored under every null row, so
// a kernel that reads such a value gives a different result.
Series numbers(TypeId type, dstd::Span<const Cell> cells, double under = -777) {
    const auto n = static_cast<std::int64_t>(cells.size());
    dstd::Vector<double> d;
    dstd::Vector<std::int64_t> l;
    dstd::Vector<std::uint8_t> valid((cells.size() + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const double v = cells[i] ? *cells[i] : under;
        d.push_back(v);
        // A NaN has no int64 value, so only an Int64 column converts.
        l.push_back(type == TypeId::Int64 ? static_cast<std::int64_t>(v) : 0);
        if (cells[i])
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            any_null = true;
    }
    const std::uint8_t* bits = any_null ? valid.data() : nullptr;
    return type == TypeId::Int64 ? Series::flat_i64(l.data(), n, bits)
                                 : Series::flat_f64(d.data(), n, bits);
}

Cells number_cells(TypeId type, std::int64_t n, bool nulls) {
    Cells out;
    for (std::int64_t i = 0; i < n; ++i) {
        if (nulls && i % 11 == 5) {
            out.emplace_back();
            continue;
        }
        // Squares, so the values are skewed and no moment is near zero.
        const std::int64_t k = (i * 37) % 23;
        double v = static_cast<double>(k * k - 40);
        if (type == TypeId::Float64) v += 0.25 * static_cast<double>(i % 3);
        out.emplace_back(v);
    }
    return out;
}

Cells slice_of(dstd::Span<const Cell> c, std::size_t from, std::size_t to) {
    return Cells(c.begin() + static_cast<std::ptrdiff_t>(from),
                 c.begin() + static_cast<std::ptrdiff_t>(to));
}

Cells pick_cells(dstd::Span<const Cell> c, dstd::Span<const std::int64_t> idx) {
    Cells out;
    for (std::int64_t k : idx)
        out.push_back(k < 0 ? Cell() : c[static_cast<std::size_t>(k)]);
    return out;
}

Series chunks_of(dstd::Vector<Series> parts) {
    dstd::Vector<dstd::SharedPtr<dftu_series>> chunks;
    for (Series& p : parts) chunks.push_back(adopt_series(p.release()));
    return Series{make_chunked(std::move(chunks))};
}

// Selection indices over `base_rows` rows, out of order, with repeats and,
// when `fill`, outer-fill rows.
Idx shuffled(std::int64_t n, std::int64_t base_rows, bool fill) {
    Idx out;
    for (std::int64_t i = 0; i < n; ++i)
        out.push_back(fill && i % 13 == 4 ? -1 : (i * 7 + 3) % base_rows);
    return out;
}

// A numeric DICTIONARY of `n` rows over a child of eight entries, the third
// one null, with some null codes of its own. `over_selection` makes the null
// entry a SELECTION's -1 below the dictionary.
Layout dictionary_layout(dstd::StringView name, TypeId type, std::int64_t n,
                         bool over_selection) {
    const Cells entries = {Cell(4), Cell(-3), Cell(),  Cell(90),
                           Cell(0), Cell(4),  Cell(7), Cell(-6)};
    Series child = numbers(type, entries, 999);
    if (over_selection) {
        const Idx through = {0, 1, -1, 3, 4, 5, 6, 7};
        child = selection_over(numbers(type, entries, 999), through);
    }
    dstd::Vector<std::int32_t> codes;
    dstd::Vector<std::uint8_t> valid(static_cast<std::size_t>((n + 7) / 8), 0);
    Cells cells;
    for (std::int64_t i = 0; i < n; ++i) {
        const auto code = static_cast<std::int32_t>((i * 5 + 1) % 8);
        codes.push_back(code);
        if (i % 9 == 2) {
            cells.emplace_back();
            continue;
        }
        valid[static_cast<std::size_t>(i >> 3)] |=
            static_cast<std::uint8_t>(1u << (i & 7));
        cells.push_back(entries[static_cast<std::size_t>(code)]);
    }
    Series d = dictionary_from_codes(codes, std::move(child), valid.data());
    d.handle()->type = type;
    return {name, std::move(d), std::move(cells)};
}

// No op builds a CONSTANT column, so make one by hand: the one row of `one`
// repeated, with every seventh row null when `nulls`.
Series constant_of(const Series& one, std::int64_t n, bool nulls) {
    auto* c = new dftu_series(*one.handle());
    c->encoding = Encoding::Constant;
    c->length = n;
    c->validity = {};
    c->null_count = 0;
    if (nulls) {
        c->validity = new_buffer(static_cast<std::size_t>((n + 7) / 8));
        std::memset(writable(c->validity), 0, c->validity.size());
        for (std::int64_t i = 0; i < n; ++i) {
            if (i % 7 == 0)
                ++c->null_count;
            else
                writable(c->validity)[i >> 3] |=
                    static_cast<std::uint8_t>(1u << (i & 7));
        }
    }
    return Series{c};
}

Layout constant_layout(dstd::StringView name, TypeId type, std::int64_t n,
                       double value, bool nulls) {
    Cells cells;
    for (std::int64_t i = 0; i < n; ++i)
        cells.push_back(nulls && i % 7 == 0 ? Cell() : Cell(value));
    return {name, constant_of(numbers(type, Cells{Cell(value)}), n, nulls),
            std::move(cells)};
}

dstd::Vector<Layout> number_layouts(TypeId type, std::int64_t n) {
    const auto un = static_cast<std::size_t>(n);
    const Cells dense = number_cells(type, n, false);
    const Cells base = number_cells(type, n, true);
    dstd::Vector<Layout> out;
    {
        dstd::Vector<Series> parts;
        parts.push_back(numbers(type, slice_of(dense, 0, un / 3)));
        parts.push_back(numbers(type, slice_of(dense, un / 3, un / 3 + 1)));
        parts.push_back(numbers(type, slice_of(dense, un / 3 + 1, un)));
        out.push_back({"chunked dense", chunks_of(std::move(parts)), dense});
    }
    {
        dstd::Vector<Series> parts;
        Cells cells = slice_of(dense, 0, un / 4);
        parts.push_back(numbers(type, cells));
        const Cells holes = slice_of(base, un / 4, un / 2);
        parts.push_back(numbers(type, holes));
        cells.insert(cells.end(), holes.begin(), holes.end());
        const Idx sel = shuffled(n / 4, n, true);
        parts.push_back(selection_over(numbers(type, base), sel));
        const Cells picked = pick_cells(base, sel);
        cells.insert(cells.end(), picked.begin(), picked.end());
        Layout dict = dictionary_layout("", type, n / 4, false);
        parts.push_back(std::move(dict.col));
        cells.insert(cells.end(), dict.cells.begin(), dict.cells.end());
        out.push_back(
            {"chunked", chunks_of(std::move(parts)), std::move(cells)});
    }
    out.push_back(dictionary_layout("dictionary", type, n, false));
    out.push_back(
        dictionary_layout("dictionary over selection", type, n, true));
    {
        dstd::Vector<std::uint8_t> keep;
        Cells cells;
        for (std::size_t i = 0; i < un; ++i) {
            keep.push_back(i % 5 != 3);
            if (keep.back()) cells.push_back(base[i]);
        }
        Series filtered = numbers(type, base).filter(mask_of(keep));
        REQUIRE(filtered.encoding() == Encoding::Selection);
        out.push_back({"filtered", std::move(filtered), std::move(cells)});
    }
    {
        const Idx sel = shuffled(n, n, true);
        out.push_back({"selection with fill rows",
                       selection_over(numbers(type, base), sel),
                       pick_cells(base, sel)});
        const Idx dense_sel = shuffled(n, n, false);
        out.push_back({"selection dense",
                       selection_over(numbers(type, dense), dense_sel),
                       pick_cells(dense, dense_sel)});
    }
    {
        const Layout dict = dictionary_layout("", type, n, false);
        const Idx sel = shuffled(n, n, true);
        out.push_back({"selection over dictionary",
                       selection_over(dict.col, sel),
                       pick_cells(dict.cells, sel)});
    }
    out.push_back(constant_layout("constant", type, n, 6, false));
    out.push_back(constant_layout("constant with nulls", type, n, -4, true));
    {
        const Layout c = constant_layout("", type, n, 5, true);
        const Idx sel = shuffled(n, n, true);
        out.push_back({"selection over constant", selection_over(c.col, sel),
                       pick_cells(c.cells, sel)});
    }
    {
        dstd::Vector<Series> parts;
        Cells cells = slice_of(dense, 0, un / 3);
        parts.push_back(numbers(type, cells));
        Layout c = constant_layout("", type, n / 3, 9, true);
        parts.push_back(std::move(c.col));
        cells.insert(cells.end(), c.cells.begin(), c.cells.end());
        const Cells tail = slice_of(base, un / 2, un);
        parts.push_back(numbers(type, tail));
        cells.insert(cells.end(), tail.begin(), tail.end());
        out.push_back({"chunked with constant", chunks_of(std::move(parts)),
                       std::move(cells)});
    }
    return out;
}

bool same_value(double a, double b) {
    return a == b || (std::isnan(a) && std::isnan(b));
}

// A moment of `rows` values against another reduction of them or its known
// value: within a relative 1e-12. A long sum is only rows * epsilon close to
// the exact moment (the FLAT pass is 6e-12 off at 600000 rows), so a long
// column takes that bound.
bool close_to(double got, double want, std::int64_t rows) {
    const double bound =
        std::max(1e-12, static_cast<double>(rows) *
                            std::numeric_limits<double>::epsilon());
    return same_value(got, want) ||
           (std::isfinite(want) &&
            std::abs(got - want) <= bound * std::abs(want));
}

// Two FLAT numeric or Bool results are equal row by row, nulls included.
void check_same(const Series& got, const Series& want) {
    REQUIRE(want.handle() != nullptr);
    REQUIRE(got.handle() != nullptr);
    REQUIRE(got.type() == want.type());
    REQUIRE(got.length() == want.length());
    std::int64_t first_bad = -1;
    for (std::int64_t i = 0; first_bad < 0 && i < got.length(); ++i) {
        if (got.is_null(i) != want.is_null(i))
            first_bad = i;
        else if (!got.is_null(i) &&
                 !same_value(read_f64(got, i), read_f64(want, i)))
            first_bad = i;
    }
    CHECK(first_bad == -1);
}

// The rows of a numeric or Bool column, N for a null, each with a comma.
dstd::String text_of(const Series& s) {
    REQUIRE(s.handle() != nullptr);
    dstd::String out;
    for (std::int64_t i = 0; i < s.length(); ++i)
        out += s.is_null(i) ? dstd::String("N,")
                            : dstd::fmt<"{},">(read_f64(s, i));
    return out;
}

// `cells` as a FLAT column and below each layer that holds the same rows.
dstd::Vector<Layout> known_layouts(TypeId type, dstd::Span<const Cell> cells) {
    const std::size_t half = cells.size() / 2;
    const Cells rows(cells.begin(), cells.end());
    dstd::Vector<Layout> out;
    out.push_back({"flat", numbers(type, cells), rows});
    dstd::Vector<Series> parts;
    parts.push_back(numbers(type, slice_of(cells, 0, half)));
    parts.push_back(numbers(type, slice_of(cells, half, cells.size())));
    out.push_back({"chunked", chunks_of(std::move(parts)), rows});
    Idx same;
    dstd::Vector<std::int32_t> codes;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        same.push_back(static_cast<std::int64_t>(i));
        codes.push_back(static_cast<std::int32_t>(i));
    }
    out.push_back(
        {"selection", selection_over(numbers(type, cells), same), rows});
    Series d = dictionary_from_codes(codes, numbers(type, cells), nullptr);
    d.handle()->type = type;
    out.push_back({"dictionary", std::move(d), rows});
    return out;
}

struct SeriesOp {
    dstd::StringView name;
    Series (*run)(const Series&);
};

const SeriesOp KEY_OPS[] = {
    {"is_unique", [](const Series& s) { return s.is_unique(); }},
    {"is_duplicated", [](const Series& s) { return s.is_duplicated(); }},
    {"duplicated first", [](const Series& s) { return s.duplicated(0); }},
    {"duplicated last", [](const Series& s) { return s.duplicated(1); }},
    {"duplicated all", [](const Series& s) { return s.duplicated(2); }},
    {"rank average",
     [](const Series& s) { return s.rank(RankMethod::Average); }},
    {"rank min descending",
     [](const Series& s) { return s.rank(RankMethod::Min, true); }},
    {"rank dense pct",
     [](const Series& s) { return s.rank(RankMethod::Dense, false, true); }},
    {"rank ordinal",
     [](const Series& s) { return s.rank(RankMethod::Ordinal); }},
};

const SeriesOp NUMBER_OPS[] = {
    {"rolling sum",
     [](const Series& s) { return s.rolling(RollingOp::Sum, 4); }},
    {"rolling mean",
     [](const Series& s) { return s.rolling(RollingOp::Mean, 4); }},
    {"rolling min",
     [](const Series& s) { return s.rolling(RollingOp::Min, 4); }},
    {"rolling max",
     [](const Series& s) { return s.rolling(RollingOp::Max, 4); }},
    {"rolling var", [](const Series& s) { return s.rolling_var(4); }},
    {"rolling std", [](const Series& s) { return s.rolling_std(4); }},
    {"rolling median", [](const Series& s) { return s.rolling_median(4); }},
    {"rolling quantile",
     [](const Series& s) { return s.rolling_quantile(4, 0.3); }},
    {"ewm mean", [](const Series& s) { return s.ewm_mean(0.3); }},
    {"ewm std", [](const Series& s) { return s.ewm_std(0.3); }},
    {"qcut", [](const Series& s) { return s.qcut(4); }},
    {"interpolate", [](const Series& s) { return s.interpolate(); }},
};

struct ScalarOp {
    dstd::StringView name;
    double (*run)(const Series&);
};

const ScalarOp MOMENT_OPS[] = {
    {"variance", [](const Series& s) { return s.variance(true); }},
    {"variance population", [](const Series& s) { return s.variance(false); }},
    {"stddev", [](const Series& s) { return s.stddev(true); }},
    {"skewness", [](const Series& s) { return s.skewness(); }},
    {"kurtosis", [](const Series& s) { return s.kurtosis(); }},
    {"skewness unbiased",
     [](const Series& s) { return s.skewness_unbiased(); }},
    {"kurtosis unbiased",
     [](const Series& s) { return s.kurtosis_unbiased(); }},
};

// These sort the same values, so a view gives the exact FLAT result.
const ScalarOp ORDER_OPS[] = {
    {"quantile", [](const Series& s) { return s.quantile(0.3); }},
    {"median", [](const Series& s) { return s.median(); }},
};

void check_scalars(const Series& got_of, const Series& want_of) {
    for (const ScalarOp& op : MOMENT_OPS) {
        CAPTURE(op.name);
        const double got = op.run(got_of);
        const double want = op.run(want_of);
        CAPTURE(got);
        CAPTURE(want);
        CHECK(close_to(got, want, got_of.length()));
    }
    for (const ScalarOp& op : ORDER_OPS) {
        CAPTURE(op.name);
        const double got = op.run(got_of);
        const double want = op.run(want_of);
        CAPTURE(got);
        CAPTURE(want);
        CHECK(same_value(got, want));
    }
}

void check_number_layouts(TypeId type, std::int64_t n) {
    const Cells breaks_cells = {Cell(-15), Cell(24), Cell(81)};
    const Series breaks = numbers(type, breaks_cells);
    dstd::Vector<Series> layered_breaks;
    {
        dstd::Vector<Series> parts;
        parts.push_back(numbers(type, slice_of(breaks_cells, 0, 1)));
        parts.push_back(numbers(type, slice_of(breaks_cells, 1, 3)));
        layered_breaks.push_back(chunks_of(std::move(parts)));
        const Cells spread = {Cell(81), Cell(-15), Cell(24)};
        layered_breaks.push_back(
            selection_over(numbers(type, spread), Idx{1, 2, 0}));
        const dstd::Vector<std::int32_t> codes{2, 0, 1};
        Series d = dictionary_from_codes(codes, numbers(type, spread), nullptr);
        d.handle()->type = type;
        layered_breaks.push_back(std::move(d));
    }
    const Series sorted = numbers(type, Cells{Cell(-40), Cell(-4), Cell(9),
                                              Cell(41), Cell(60), Cell(104)});

    for (const Layout& c : number_layouts(type, n)) {
        CAPTURE(c.name);
        REQUIRE(c.col.length() == static_cast<std::int64_t>(c.cells.size()));
        const Series flat = numbers(type, c.cells, 0);
        for (const SeriesOp& op : KEY_OPS) {
            CAPTURE(op.name);
            check_same(op.run(c.col), op.run(flat));
        }
        for (const SeriesOp& op : NUMBER_OPS) {
            CAPTURE(op.name);
            check_same(op.run(c.col), op.run(flat));
        }
        check_scalars(c.col, flat);
        for (const std::int32_t flags : {0, DFTU_CUT_RIGHT | DFTU_CUT_INNER}) {
            CAPTURE(flags);
            check_same(c.col.cut(breaks, flags), flat.cut(breaks, flags));
            for (const Series& b : layered_breaks)
                check_same(flat.cut(b, flags), flat.cut(breaks, flags));
        }
        for (const bool right : {false, true}) {
            CAPTURE(right);
            check_same(c.col.search_sorted(sorted, right),
                       flat.search_sorted(sorted, right));
            check_same(sorted.search_sorted(c.col, right),
                       sorted.search_sorted(flat, right));
        }
        check_same(c.col.is_in(sorted), flat.is_in(sorted));
        check_same(flat.is_in(c.col), flat.is_in(flat));
        for (const Series& b : layered_breaks)
            check_same(flat.is_in(b), flat.is_in(breaks));
        // An integer column against float values and the reverse.
        const TypeId other =
            type == TypeId::Int64 ? TypeId::Float64 : TypeId::Int64;
        const Series others = numbers(other, breaks_cells);
        check_same(c.col.is_in(others), flat.is_in(others));
    }
}

Series string_chunks(dstd::Span<const Row> rows) {
    const std::size_t third = rows.size() / 3;
    auto part = [&](std::size_t from, std::size_t to) {
        return Rows(rows.begin() + static_cast<std::ptrdiff_t>(from),
                    rows.begin() + static_cast<std::ptrdiff_t>(to));
    };
    dstd::Vector<Series> parts;
    parts.push_back(flat_of(part(0, third)));
    parts.push_back(view_of(part(third, 2 * third)));
    parts.push_back(flat_of(part(2 * third, rows.size())).dictionary_encode());
    return chunks_of(std::move(parts));
}

using Wide = dstd::Vector<dstd::Optional<std::int64_t>>;

// A FLAT Decimal128 column of whole numbers.
Series decimals(dstd::Span<const dstd::Optional<std::int64_t>> values) {
    auto* c = new dftu_series();
    c->type = TypeId::Decimal128;
    c->set_decimal(38, 0);
    c->length = static_cast<std::int64_t>(values.size());
    c->data = new_buffer(values.size() * 16);
    c->validity = new_buffer((values.size() + 7) / 8);
    std::memset(writable(c->validity), 0, c->validity.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::int64_t lo = values[i] ? *values[i] : -777;
        const std::int64_t hi = lo < 0 ? -1 : 0;
        std::memcpy(writable(c->data) + i * 16, &lo, 8);
        std::memcpy(writable(c->data) + i * 16 + 8, &hi, 8);
        if (values[i])
            writable(c->validity)[i >> 3] |=
                static_cast<std::uint8_t>(1u << (i & 7));
        else
            ++c->null_count;
    }
    return Series{c};
}

}  // namespace

TEST_SUITE("kernels read every layout") {
    TEST_CASE("the cell readers give each layout's rows and nulls") {
        for (const TypeId type : {TypeId::Float64, TypeId::Int64}) {
            for (const Layout& c : number_layouts(type, 130)) {
                CAPTURE(c.name);
                const dftu_series* h = c.col.handle();
                std::int64_t nulls = 0;
                std::int64_t first_bad = -1;
                for (std::size_t k = 0; k < c.cells.size(); ++k) {
                    const auto i = static_cast<std::int64_t>(k);
                    const Cell& want = c.cells[k];
                    nulls += !want;
                    bool good = cell_is_null(h, i) == !want;
                    if (good && want) {
                        const auto whole = static_cast<std::int64_t>(*want);
                        good = type == TypeId::Float64
                                   ? cell_f64(h, i) == *want
                                   : cell_i64(h, i) == whole &&
                                         cell_u64(h, i) ==
                                             static_cast<std::uint64_t>(whole);
                    }
                    if (!good && first_bad < 0) first_bad = i;
                }
                CHECK(first_bad == -1);
                CHECK(cell_null_count(*h) == nulls);
            }
        }
    }

    TEST_CASE("Float64 kernels equal the flat path on every layout") {
        check_number_layouts(TypeId::Float64, 130);
    }
    TEST_CASE("Int64 kernels equal the flat path on every layout") {
        check_number_layouts(TypeId::Int64, 130);
    }
    TEST_CASE("long columns take the parallel paths to the same result") {
        check_number_layouts(TypeId::Float64, 40000);
    }

    TEST_CASE("moments and quantiles of a very long view equal the flat path") {
        constexpr std::int64_t N = 600'000;
        for (const TypeId type : {TypeId::Float64, TypeId::Int64}) {
            for (const Layout& c : number_layouts(type, N)) {
                CAPTURE(c.name);
                const Series flat = numbers(type, c.cells, 0);
                check_scalars(c.col, flat);
            }
        }
    }

    TEST_CASE("an all-null dictionary has no spread") {
        const Cells entries = {Cell()};
        const dstd::Vector<std::int32_t> codes{0, 0, 0};
        Series d = dictionary_from_codes(
            codes, numbers(TypeId::Float64, entries), nullptr);
        d.handle()->type = TypeId::Float64;
        CHECK(std::isnan(d.variance(true)));
        CHECK(std::isnan(d.variance(false)));
        CHECK(std::isnan(d.stddev(true)));
        CHECK(std::isnan(d.quantile(0.5)));
        const Series ranked = d.rank(RankMethod::Average);
        REQUIRE(ranked.length() == 3);
        CHECK(ranked.null_count() == 3);
    }

    TEST_CASE("a constant NaN ranks as missing") {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const Layout c = constant_layout("nan", TypeId::Float64, 9, nan, false);
        const Series flat = numbers(TypeId::Float64, c.cells);
        for (const SeriesOp& op : KEY_OPS) {
            CAPTURE(op.name);
            check_same(op.run(c.col), op.run(flat));
        }
    }

    TEST_CASE("string keys equal the flat path on every layout") {
        dstd::Vector<Case> all = cases();
        const Rows base = pool_rows(120);
        all.push_back(
            {"chunked", Kind::Flat, string_chunks(base), base, nullptr});
        const Rows same(120, Row("read"));
        all.push_back({"constant", Kind::Flat,
                       constant_of(flat_of({Row("read")}), 120, false), same,
                       nullptr});
        Rows holes;
        for (std::size_t i = 0; i < 120; ++i)
            holes.push_back(i % 7 == 0 ? Row() : Row("read"));
        all.push_back({"constant with nulls", Kind::Flat,
                       constant_of(flat_of({Row("read")}), 120, true), holes,
                       nullptr});
        const Idx through = shuffled(120, 120, true);
        all.push_back(
            {"selection over constant", Kind::Flat,
             selection_over(constant_of(flat_of({Row("read")}), 120, true),
                            through),
             pick(holes, through), nullptr});
        dstd::Vector<Series> mixed;
        mixed.push_back(flat_of({Row("write"), Row()}));
        mixed.push_back(constant_of(flat_of({Row("read")}), 118, false));
        Rows mixed_rows(120, Row("read"));
        mixed_rows[0] = Row("write");
        mixed_rows[1] = Row();
        all.push_back({"chunked with constant", Kind::Flat,
                       chunks_of(std::move(mixed)), std::move(mixed_rows),
                       nullptr});
        const Series needles = flat_of({Row("read"), Row(""), Row("nope")});
        for (const Case& c : all) {
            CAPTURE(dstd::StringView(c.name));
            const Series flat = flat_of(c.rows);
            for (const SeriesOp& op : KEY_OPS) {
                CAPTURE(op.name);
                check_same(op.run(c.col), op.run(flat));
            }
            check_same(c.col.is_in(needles), flat.is_in(needles));
            check_same(needles.is_in(c.col), needles.is_in(flat));
        }
    }

    TEST_CASE("decimal keys equal the flat path on every layout") {
        Wide base;
        for (std::int64_t i = 0; i < 60; ++i)
            base.push_back(i % 8 == 3
                               ? dstd::Optional<std::int64_t>()
                               : dstd::Optional<std::int64_t>((i * 7) % 9 - 4));
        const Idx sel = shuffled(60, 60, true);
        Wide picked;
        for (std::int64_t k : sel)
            picked.push_back(k < 0 ? dstd::Optional<std::int64_t>()
                                   : base[static_cast<std::size_t>(k)]);
        dstd::Vector<Series> parts;
        parts.push_back(decimals(Wide(base.begin(), base.begin() + 20)));
        parts.push_back(decimals(Wide(base.begin() + 20, base.end())));
        struct Pair {
            dstd::StringView name;
            Series col;
            Series flat;
        };
        dstd::Vector<Pair> pairs;
        pairs.push_back({"selection", selection_over(decimals(base), sel),
                         decimals(picked)});
        pairs.push_back(
            {"chunked", chunks_of(std::move(parts)), decimals(base)});
        pairs.push_back({"constant", constant_of(decimals(Wide{5}), 12, false),
                         decimals(Wide(12, 5))});
        const Series needles = decimals(Wide{-4, 0, 5});
        for (const Pair& c : pairs) {
            CAPTURE(c.name);
            for (const SeriesOp& op : KEY_OPS) {
                CAPTURE(op.name);
                check_same(op.run(c.col), op.run(c.flat));
            }
            check_same(c.col.is_in(needles), c.flat.is_in(needles));
            check_same(needles.is_in(c.col), needles.is_in(c.flat));
            CHECK(close_to(c.col.variance(true), c.flat.variance(true),
                           c.col.length()));
            check_same(c.col.interpolate(), c.flat.interpolate());
        }
    }

    TEST_CASE("known moments, ranks, masks and bins of every layout") {
        // [1, 2, 3, 4, 10]: mean 4, central sums 50, 180 and 1394.
        const Cells five = {Cell(1), Cell(2), Cell(3), Cell(4), Cell(10)};
        const double skew = 36.0 / std::sqrt(1000.0);
        const Cells keys = {Cell(3), Cell(1), Cell(), Cell(3), Cell(2)};
        for (const TypeId type : {TypeId::Float64, TypeId::Int64}) {
            for (const Layout& c : known_layouts(type, five)) {
                CAPTURE(c.name);
                CHECK(close_to(c.col.variance(true), 12.5, 5));
                CHECK(close_to(c.col.variance(false), 10.0, 5));
                CHECK(close_to(c.col.stddev(true), std::sqrt(12.5), 5));
                CHECK(close_to(c.col.skewness(), skew, 5));
                CHECK(close_to(c.col.skewness_unbiased(),
                               skew * std::sqrt(20.0) / 3.0, 5));
                CHECK(close_to(c.col.kurtosis(), -0.212, 5));
                CHECK(close_to(c.col.kurtosis_unbiased(), 3.152, 5));
                CHECK(close_to(c.col.quantile(0.3), 2.2, 5));
                CHECK(c.col.median() == 3.0);
            }
            const Series edges = numbers(type, Cells{Cell(2), Cell(3)});
            const Series sorted =
                numbers(type, Cells{Cell(1), Cell(3), Cell(5)});
            for (const Layout& c : known_layouts(type, keys)) {
                CAPTURE(c.name);
                CHECK(text_of(c.col.rank(RankMethod::Average)) ==
                      "3.5,1,N,3.5,2,");
                CHECK(text_of(c.col.rank(RankMethod::Min)) == "3,1,N,3,2,");
                CHECK(text_of(c.col.rank(RankMethod::Dense)) == "3,1,N,3,2,");
                CHECK(text_of(c.col.duplicated(0)) == "0,0,0,1,0,");
                CHECK(text_of(c.col.duplicated(1)) == "1,0,0,0,0,");
                CHECK(text_of(c.col.is_unique()) == "0,1,1,0,1,");
                CHECK(text_of(c.col.is_in(edges)) == "1,0,0,1,1,");
                CHECK(text_of(c.col.cut(edges)) == "2,0,N,2,1,");
                CHECK(text_of(c.col.cut(edges, DFTU_CUT_RIGHT)) ==
                      "1,0,N,1,0,");
                CHECK(text_of(sorted.search_sorted(c.col)) == "1,0,3,1,1,");
                CHECK(text_of(sorted.search_sorted(c.col, true)) ==
                      "2,1,3,2,1,");
            }
        }
    }

    TEST_CASE("moments of far apart chunks and views stay finite") {
        // `low` rows at 0 and `high` rows at d, with p = high / (low + high):
        // variance d^2 p (1 - p), skewness (1 - 2p) / sqrt(p (1 - p)), excess
        // kurtosis (1 - 6 p (1 - p)) / (p (1 - p)).
        auto check = [](std::int64_t low, std::int64_t high, double d,
                        bool third, bool fourth) {
            CAPTURE(d);
            CAPTURE(low);
            const auto at = static_cast<std::size_t>(low);
            Cells cells(at, Cell(0));
            cells.insert(cells.end(), static_cast<std::size_t>(high), Cell(d));
            Idx idx(at, 0);
            idx.insert(idx.end(), static_cast<std::size_t>(high), 1);
            dstd::Vector<Series> parts;
            parts.push_back(numbers(TypeId::Float64, slice_of(cells, 0, at)));
            parts.push_back(
                numbers(TypeId::Float64, slice_of(cells, at, cells.size())));
            dstd::Vector<Layout> all;
            all.push_back({"flat", numbers(TypeId::Float64, cells), Cells()});
            all.push_back({"chunked", chunks_of(std::move(parts)), Cells()});
            all.push_back(
                {"selection",
                 selection_over(
                     numbers(TypeId::Float64, Cells{Cell(0), Cell(d)}), idx),
                 Cells()});
            const double n = static_cast<double>(low + high);
            const double p = static_cast<double>(high) / n;
            const double pq = p * (1.0 - p);
            const double var = d * pq * d;
            for (const Layout& c : all) {
                CAPTURE(c.name);
                CHECK(close_to(c.col.variance(false), var, low + high));
                CHECK(close_to(c.col.variance(true), var * (n / (n - 1.0)),
                               low + high));
                CHECK(
                    close_to(c.col.stddev(false), std::sqrt(var), low + high));
                if (third)
                    CHECK(close_to(c.col.skewness(),
                                   (1.0 - 2.0 * p) / std::sqrt(pq),
                                   low + high));
                if (fourth)
                    CHECK(close_to(c.col.kurtosis(), (1.0 - 6.0 * pq) / pq,
                                   low + high));
            }
        };
        check(2, 2, 8e153, false, false);
        check(1, 1, 1.5e154, false, false);
        check(2048, 2048, 1e152, false, false);
        check(2048, 2048, 1e75, false, true);
        check(3072, 1024, 1e100, true, false);
        check(3072, 1024, 1e70, true, true);
        check(300'000, 300'000, 1e150, false, false);
        check(450'000, 150'000, 1e70, true, true);
    }

    TEST_CASE("chunks and views keep the wraps of an exact integer sum") {
        constexpr std::int64_t TOP = std::numeric_limits<std::int64_t>::max();
        constexpr std::int64_t LOW = std::numeric_limits<std::int64_t>::min();
        using Whole = dstd::Vector<dstd::Optional<std::int64_t>>;
        const auto flat_of_whole =
            [](dstd::Span<const dstd::Optional<std::int64_t>> v) {
                dstd::Vector<std::int64_t> data;
                dstd::Vector<std::uint8_t> valid((v.size() + 7) / 8, 0);
                bool any_null = false;
                for (std::size_t i = 0; i < v.size(); ++i) {
                    data.push_back(v[i] ? *v[i] : -777);
                    if (v[i])
                        valid[i >> 3] |=
                            static_cast<std::uint8_t>(1u << (i & 7));
                    else
                        any_null = true;
                }
                return Series::flat_i64(data.data(),
                                        static_cast<std::int64_t>(v.size()),
                                        any_null ? valid.data() : nullptr);
            };
        dstd::Vector<Whole> sets;
        sets.push_back({TOP, TOP, 0, dstd::Optional<std::int64_t>()});
        sets.push_back({LOW, LOW, 0, dstd::Optional<std::int64_t>()});
        sets.push_back({TOP, TOP, TOP});
        sets.push_back({LOW, LOW, LOW, LOW, LOW});
        sets.push_back({TOP, 1, LOW, -1, TOP, TOP, 7});
        for (const std::int64_t rows : {5'000, 600'000}) {
            Whole mixed;
            for (std::int64_t i = 0; i < rows; ++i) {
                const std::int64_t near = i % 3 == 0 ? LOW + i : TOP - i;
                mixed.push_back(i % 5 == 4 ? -i : near);
            }
            sets.push_back(std::move(mixed));
        }
        for (const Whole& values : sets) {
            CAPTURE(values.size());
            FieldStat want;
            for (const auto& v : values)
                if (v) want.add(*v);
            const auto half = static_cast<std::ptrdiff_t>(values.size() / 2);
            dstd::Vector<Series> parts;
            parts.push_back(
                flat_of_whole(Whole(values.begin(), values.begin() + half)));
            parts.push_back(
                flat_of_whole(Whole(values.begin() + half, values.end())));
            Idx idx;
            for (std::size_t i = 0; i < values.size(); ++i)
                idx.push_back(static_cast<std::int64_t>(i));
            dstd::Vector<Layout> all;
            all.push_back({"flat", flat_of_whole(values), Cells()});
            all.push_back({"chunked", chunks_of(std::move(parts)), Cells()});
            all.push_back({"selection",
                           selection_over(flat_of_whole(values), idx),
                           Cells()});
            for (const Layout& c : all) {
                CAPTURE(c.name);
                const FieldStat got = field_stat_reduce(c.col);
                CHECK(got.n == want.n);
                CHECK(got.domain == want.domain);
                CHECK(got.esum == want.esum);
                CHECK(got.ecarry == want.ecarry);
                CHECK(got.emin == want.emin);
                CHECK(got.emax == want.emax);
                CHECK(got.mean() == want.mean());
            }
        }
    }

    TEST_CASE("cut refuses a null break in every layout") {
        for (const TypeId type : {TypeId::Float64, TypeId::Int64}) {
            const Cells holed = {Cell(-5), Cell(), Cell(4)};
            dstd::Vector<Layout> breaks;
            breaks.push_back({"flat", numbers(type, holed, -2), Cells()});
            breaks.push_back(
                {"selection",
                 selection_over(numbers(type, holed, -2), Idx{0, 1, 2}),
                 Cells()});
            breaks.push_back(
                {"fill row",
                 selection_over(numbers(type, Cells{Cell(-5), Cell(4)}),
                                Idx{0, -1, 1}),
                 Cells()});
            dstd::Vector<Series> parts;
            parts.push_back(numbers(type, slice_of(holed, 0, 1)));
            parts.push_back(numbers(type, slice_of(holed, 1, 3), -2));
            breaks.push_back({"chunked", chunks_of(std::move(parts)), Cells()});
            const dstd::Vector<std::int32_t> codes{0, 1, 2};
            Series d =
                dictionary_from_codes(codes, numbers(type, holed, -2), nullptr);
            d.handle()->type = type;
            breaks.push_back({"dictionary", std::move(d), Cells()});
            Layout fixed = constant_layout("", type, 3, 4, true);
            breaks.push_back({"constant", std::move(fixed.col), Cells()});
            const Series x = numbers(type, Cells{Cell(-1)});
            const Series viewed =
                selection_over(numbers(type, Cells{Cell(-1)}), Idx{0});
            for (const Layout& b : breaks) {
                CAPTURE(b.name);
                for (const std::int32_t flags :
                     {0, DFTU_CUT_RIGHT | DFTU_CUT_INNER}) {
                    CHECK_FALSE(x.cut(b.col, flags).valid());
                    CHECK_FALSE(viewed.cut(b.col, flags).valid());
                }
            }
            CHECK(x.cut(numbers(type, Cells{Cell(-5), Cell(4)})).valid());
        }
    }

    TEST_CASE("a constant below another layer reads as its flat column") {
        const Idx rows = {0, 1, 2, -1};
        const Layout three = constant_layout("", TypeId::Int64, 3, 3, false);
        const Series number = selection_over(three.col, rows);
        CHECK(text_of(number.rank(RankMethod::Average)) == "2,2,2,N,");
        CHECK(text_of(number.duplicated(0)) == "0,1,1,0,");

        const Series text =
            selection_over(constant_of(flat_of({Row("x")}), 3, false), rows);
        CHECK(text_of(text.is_in(flat_of({Row("x")}))) == "1,1,1,0,");
        CHECK(text_of(text.rank(RankMethod::Average)) == "2,2,2,N,");
        CHECK(text_of(text.duplicated(0)) == "0,1,1,0,");
        CHECK(text_of(text.is_unique()) == "0,0,0,1,");
    }

    TEST_CASE("a constant at any depth materializes and gathers") {
        const Rows same = {Row("read"), Row("read"), Row("read")};
        const Idx picked = {2, -1, 0};
        const Rows want_picked = {Row("read"), Row(), Row("read")};
        const Series text = constant_of(flat_of({Row("read")}), 3, false);
        const Series flat = text.materialize();
        REQUIRE(flat.valid());
        CHECK(flat.encoding() == Encoding::Flat);
        CHECK(rows_of(flat) == same);
        const Series taken = text.take(picked);
        REQUIRE(taken.valid());
        CHECK(rows_of(taken) == want_picked);

        const Series over = selection_over(text, picked).materialize();
        REQUIRE(over.valid());
        CHECK(rows_of(over) == want_picked);

        const dstd::Vector<std::int32_t> codes{1, 0, 2};
        const Series dict = dictionary_from_codes(
            codes, constant_of(flat_of({Row("read")}), 3, false), nullptr);
        const Series from_dict = dict.materialize();
        REQUIRE(from_dict.valid());
        CHECK(rows_of(from_dict) == same);

        dstd::Vector<Series> parts;
        parts.push_back(flat_of({Row("write")}));
        parts.push_back(constant_of(flat_of({Row("read")}), 2, false));
        const Series chunked = chunks_of(std::move(parts));
        const Series joined = chunked.materialize();
        REQUIRE(joined.valid());
        CHECK(rows_of(joined) == Rows{Row("write"), Row("read"), Row("read")});
        const Series reordered = chunked.take(Idx{2, 0});
        REQUIRE(reordered.valid());
        CHECK(rows_of(reordered) == Rows{Row("read"), Row("write")});

        for (const TypeId type : {TypeId::Float64, TypeId::Int64}) {
            for (const std::int64_t n : {std::int64_t{1}, std::int64_t{20}}) {
                const Layout c = constant_layout("", type, n, 6, n > 1);
                const Series got = c.col.materialize();
                REQUIRE(got.valid());
                CHECK(got.encoding() == Encoding::Flat);
                check_same(got, numbers(type, c.cells));
                const Idx two = {n - 1, -1};
                const Series some = c.col.take(two);
                REQUIRE(some.valid());
                check_same(some, numbers(type, pick_cells(c.cells, two)));
            }
        }
        const std::uint8_t yes = 1;
        const Series flags =
            constant_of(Series::flat(TypeId::Bool, &yes, 1), 12, true);
        const Series bools = flags.materialize();
        REQUIRE(bools.valid());
        CHECK(text_of(bools) == "N,1,1,1,1,1,1,N,1,1,1,1,");
    }

    TEST_CASE(
        "a range over chunks reduces as the same range of a flat column") {
        constexpr std::int64_t N = 600'000;
        for (const Layout& c : number_layouts(TypeId::Int64, N)) {
            if (c.col.encoding() != Encoding::Chunked) continue;
            CAPTURE(c.name);
            const Series flat = numbers(TypeId::Int64, c.cells, 0);
            const std::int64_t len = c.col.length();
            const dstd::Pair<std::int64_t, std::int64_t> ranges[] = {
                {0, len},
                {len / 4 - 3, len / 4 + 3},
                {len / 3, len / 3 + 1},
                {5, len - 5},
                {7, 7}};
            for (const auto& [from, to] : ranges) {
                CAPTURE(from);
                CAPTURE(to);
                const FieldStat got = field_stat_reduce(c.col, from, to);
                const FieldStat want = field_stat_reduce(flat, from, to);
                CHECK(got.n == want.n);
                CHECK(got.domain == want.domain);
                CHECK(got.esum == want.esum);
                CHECK(got.emin == want.emin);
                CHECK(got.emax == want.emax);
                CHECK(close_to(got.variance(true), want.variance(true),
                               to - from));
                CHECK(close_to(got.kurtosis(), want.kurtosis(), to - from));
            }
        }
    }
}

namespace {

// A reduced scalar as text: its kind and its value, so two reductions of the
// same rows compare exactly.
dstd::String scalar_text(const dftu_scalar& s) {
    switch (s.kind) {
        case DFTU_SCALAR_TAG_NULL:
            return "null";
        case DFTU_SCALAR_TAG_I64:
            return dstd::fmt<"i{}">(s.value.i);
        case DFTU_SCALAR_TAG_U64:
            return dstd::fmt<"u{}">(s.value.u);
        case DFTU_SCALAR_TAG_F64:
            return dstd::fmt<"f{}">(s.value.d);
        case DFTU_SCALAR_TAG_STR:
            return dstd::String(dstd::StringView(s.value.s, s.len));
        default:
            return "error";
    }
}

// Every scalar reduction of `got` equals the one of `want`, its FLAT column.
void check_reductions(const Series& got, const Series& want) {
    for (const dftu_reduce_op op :
         {DFTU_REDUCE_SUM, DFTU_REDUCE_MIN, DFTU_REDUCE_MAX}) {
        CAPTURE(static_cast<int>(op));
        const dstd::String a =
            scalar_text(dftu_series_reduce(got.handle(), op));
        const dstd::String b =
            scalar_text(dftu_series_reduce(want.handle(), op));
        CHECK(a == b);
    }
    const dstd::String got_mode = scalar_text(dftu_series_mode(got.handle()));
    CHECK(got_mode == scalar_text(dftu_series_mode(want.handle())));
    CHECK(dftu_series_arg_min(got.handle()) ==
          dftu_series_arg_min(want.handle()));
    CHECK(dftu_series_arg_max(got.handle()) ==
          dftu_series_arg_max(want.handle()));
    CHECK(dftu_series_count(got.handle()) == dftu_series_count(want.handle()));
    CHECK(dftu_series_nunique(got.handle()) ==
          dftu_series_nunique(want.handle()));
}

// A FLAT Bool column of true, false and null rows.
Series bool_flat(dstd::Span<const dstd::Optional<bool>> rows) {
    dstd::Vector<std::uint8_t> bits((rows.size() + 7) / 8, 0);
    dstd::Vector<std::uint8_t> valid((rows.size() + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i]) {
            any_null = true;
            continue;
        }
        valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        if (*rows[i]) bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    return Series::flat(TypeId::Bool, bits.data(),
                        static_cast<std::int64_t>(rows.size()),
                        any_null ? valid.data() : nullptr);
}

// A Bool column of `rows` under every layer.
dstd::Vector<Layout> bool_layouts(dstd::Span<const dstd::Optional<bool>> rows) {
    const std::size_t n = rows.size();
    const Series flat = bool_flat(rows);
    dstd::Vector<Layout> out;
    out.push_back({"flat", flat.share(), Cells()});
    const std::size_t half = n / 2;
    dstd::Vector<Series> parts;
    parts.push_back(bool_flat(rows.subspan(0, half)));
    parts.push_back(bool_flat(rows.subspan(half)));
    out.push_back({"chunked", chunks_of(std::move(parts)), Cells()});
    Idx same;
    for (std::size_t i = 0; i < n; ++i)
        same.push_back(static_cast<std::int64_t>(i));
    out.push_back({"selection", selection_over(flat, same), Cells()});
    // A dictionary whose third entry is null, over codes that use it and over
    // one own null row.
    dstd::Vector<std::int32_t> codes;
    for (std::size_t i = 0; i < n; ++i)
        codes.push_back(!rows[i] ? 2 : (*rows[i] ? 1 : 0));
    dstd::Vector<std::uint8_t> own((n + 7) / 8, 0xFF);
    if (n > 4) own[3 >> 3] &= static_cast<std::uint8_t>(~(1u << (3 & 7)));
    const dstd::Optional<bool> entries[] = {dstd::Optional<bool>(false),
                                            dstd::Optional<bool>(true),
                                            dstd::Optional<bool>()};
    const Series fixed{
        dict_over(TypeId::Bool, codes,
                  bool_flat(dstd::Span<const dstd::Optional<bool>>(entries, 3)),
                  own.data())
            .release()};
    out.push_back({"dictionary", fixed.share(), Cells()});
    // A CONSTANT column reproduces only a uniform run of present rows, with
    // the nulls on its own bitmap.
    dstd::Optional<bool> uniform;
    bool is_uniform = true;
    for (std::size_t i = 0; i < n; ++i) {
        if (!rows[i]) continue;
        if (!uniform)
            uniform = rows[i];
        else if (*uniform != *rows[i])
            is_uniform = false;
    }
    if (!is_uniform) return out;
    dstd::Vector<std::uint8_t> const_bits((n + 7) / 8, 0);
    dstd::Vector<std::uint8_t> const_valid((n + 7) / 8, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (!rows[i]) continue;
        const_valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        if (*rows[i])
            const_bits[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    }
    auto* constant = new dftu_series(
        *bool_flat(dstd::Span<const dstd::Optional<bool>>(&rows[0], 1))
             .handle());
    constant->encoding = Encoding::Constant;
    constant->length = static_cast<std::int64_t>(n);
    constant->data = new_buffer(1);
    *reinterpret_cast<std::uint8_t*>(writable(constant->data)) =
        uniform && *uniform ? 1 : 0;
    constant->validity = new_buffer(const_valid.size());
    std::memcpy(writable(constant->validity), const_valid.data(),
                const_valid.size());
    constant->null_count = 0;
    for (std::size_t i = 0; i < n; ++i) constant->null_count += !rows[i];
    const Series constant_series{constant};
    out.push_back({"constant", constant_series.share(), Cells()});
    out.push_back({"selection over constant",
                   selection_over(constant_series, same), Cells()});
    return out;
}

}  // namespace

TEST_SUITE("kernels read every layout in place") {
    TEST_CASE("reductions read every layout") {
        for (const TypeId type : {TypeId::Float64, TypeId::Int64}) {
            for (const Layout& c : number_layouts(type, 130)) {
                CAPTURE(c.name);
                CAPTURE(static_cast<int>(type));
                check_reductions(c.col, numbers(type, c.cells, 0));
            }
        }
    }

    TEST_CASE("product and bitwise reductions read every layout") {
        const Cells small = {Cell(1), Cell(2), Cell(3), Cell(1), Cell(2),
                             Cell(3), Cell(1), Cell(2), Cell(3), Cell(1),
                             Cell(),  Cell(2), Cell(3), Cell(1), Cell(2)};
        dstd::Vector<Layout> all = known_layouts(TypeId::Int64, small);
        const Layout fixed = constant_layout("", TypeId::Int64, 15, 2, true);
        const Series want = numbers(TypeId::Int64, small, 0);
        const Series fixed_want = numbers(TypeId::Int64, fixed.cells, 0);
        for (std::size_t k = 0; k < all.size() + 1; ++k) {
            const bool is_fixed = k == all.size();
            CAPTURE(k);
            const Series& col = is_fixed ? fixed.col : all[k].col;
            const Series& flat = is_fixed ? fixed_want : want;
            CHECK(scalar_text(dftu_series_product(col.handle())) ==
                  scalar_text(dftu_series_product(flat.handle())));
            for (const std::int32_t op : {0, 1, 2}) {
                CAPTURE(op);
                const dftu_series* h = col.handle();
                const dftu_series* f = flat.handle();
                const dftu_scalar got = op == 0   ? dftu_series_bitwise_and(h)
                                        : op == 1 ? dftu_series_bitwise_or(h)
                                                  : dftu_series_bitwise_xor(h);
                const dftu_scalar expect = op == 0 ? dftu_series_bitwise_and(f)
                                           : op == 1
                                               ? dftu_series_bitwise_or(f)
                                               : dftu_series_bitwise_xor(f);
                CHECK(scalar_text(got) == scalar_text(expect));
            }
        }
    }

    TEST_CASE("boolean reductions read every layout") {
        dstd::Vector<dstd::Optional<bool>> rows;
        for (std::size_t i = 0; i < 40; ++i) {
            if (i % 9 == 4)
                rows.emplace_back();
            else
                rows.push_back(dstd::Optional<bool>(i % 3 != 0));
        }
        const Series want = bool_flat(rows);
        for (const Layout& c : bool_layouts(rows)) {
            CAPTURE(c.name);
            CHECK(dftu_series_all(c.col.handle()) ==
                  dftu_series_all(want.handle()));
            CHECK(dftu_series_any(c.col.handle()) ==
                  dftu_series_any(want.handle()));
            CHECK(scalar_text(dftu_series_bitwise_or(c.col.handle())) ==
                  scalar_text(dftu_series_bitwise_or(want.handle())));
            CHECK(dftu_series_nunique(c.col.handle()) ==
                  dftu_series_nunique(want.handle()));
        }
    }

    TEST_CASE("forward and backward fills read every layout") {
        for (const TypeId type : {TypeId::Float64, TypeId::Int64}) {
            for (const Layout& c : number_layouts(type, 130)) {
                CAPTURE(c.name);
                const Series flat = numbers(type, c.cells, 0);
                CHECK(column_text(c.col.ffill()) == column_text(flat.ffill()));
                CHECK(column_text(c.col.bfill()) == column_text(flat.bfill()));
            }
        }
        const Rows rows = pool_rows(60);
        dstd::Vector<Case> all = cases();
        all.push_back(
            {"chunked", Kind::Flat, string_chunks(rows), rows, nullptr});
        const Rows same(60, Row("read"));
        all.push_back({"constant", Kind::Flat,
                       constant_of(flat_of({Row("read")}), 60, false), same,
                       nullptr});
        for (const Case& c : all) {
            CAPTURE(dstd::StringView(c.name));
            const Series flat = flat_of(c.rows);
            CHECK(column_text(c.col.ffill()) == column_text(flat.ffill()));
            CHECK(column_text(c.col.bfill()) == column_text(flat.bfill()));
        }
    }

    TEST_CASE("a filter mask in any layout keeps the flat rows") {
        const std::int64_t n = 40;
        dstd::Vector<std::int64_t> ids;
        for (std::int64_t i = 0; i < n; ++i) ids.push_back(i);
        DataFrame df;
        df.names = dstd::make_shared_strings({"a"});
        df.columns.push_back(Series::flat_i64(ids.data(), n));
        dstd::Vector<dstd::Optional<bool>> rows;
        dstd::Vector<std::uint8_t> plain(static_cast<std::size_t>(n), 0);
        for (std::int64_t i = 0; i < n; ++i) {
            const bool chosen = i % 5 != 2;
            rows.push_back(i % 7 == 3 ? dstd::Optional<bool>()
                                      : dstd::Optional<bool>(chosen));
            plain[static_cast<std::size_t>(i)] = chosen && i % 7 != 3 ? 1 : 0;
        }
        // A null mask row selects nothing, so 27 of the 40 rows pass.
        const DataFrame reference = df.filter(mask_of(plain));
        REQUIRE(reference.num_rows() == 27);
        const dstd::String want_rows = dstd::fmt<"{}">(reference.num_rows());
        for (const Layout& c : bool_layouts(rows)) {
            CAPTURE(c.name);
            // The rule is the one flat_mask_to_indices applies to the
            // materialized mask, whatever layer the mask is stored in.
            const Series materialized = c.col.materialize();
            const DataFrame want = df.filter(materialized);
            const DataFrame got = df.filter(c.col);
            CHECK(dstd::fmt<"{}">(want.num_rows()) == want_rows);
            CHECK(dstd::fmt<"{}">(got.num_rows()) == want_rows);
            const Series lazy_mask = c.col.share();
            LazyFrame lz = df.lazy().filter_mask(lazy_mask.share());
            DataFrame out = dftu_utils_test::ok(
                dftracer::utils::default_runtime().submit(lz.collect(2)).get());
            for (Series& col : out.columns) col = col.materialize();
            CHECK(dstd::fmt<"{}">(out.num_rows()) == want_rows);
            const std::int64_t* a = got.column("a").data<std::int64_t>();
            const std::int64_t* b = out.column("a").data<std::int64_t>();
            const std::int64_t* w = want.column("a").data<std::int64_t>();
            bool same_rows = got.num_rows() == out.num_rows() &&
                             out.num_rows() == want.num_rows();
            for (std::int64_t i = 0; same_rows && i < got.num_rows(); ++i)
                same_rows = a[i] == b[i] && a[i] == w[i];
            CHECK(same_rows);
        }
        // Wide enough for the parallel ranges, with an own bitmap, a null
        // DICTIONARY entry and a SELECTION -1 row.
        const std::int64_t wide = 40000;
        dstd::Vector<dstd::Optional<bool>> wide_rows;
        dstd::Vector<std::uint8_t> wide_plain(static_cast<std::size_t>(wide),
                                              0);
        dstd::Vector<std::uint8_t> holed(static_cast<std::size_t>(wide), 0);
        dstd::Vector<std::int64_t> wide_ids;
        for (std::int64_t i = 0; i < wide; ++i) {
            const bool on = i % 3 != 0;
            const bool null = i % 11 == 0;
            wide_rows.push_back(null ? dstd::Optional<bool>()
                                     : dstd::Optional<bool>(on));
            wide_plain[static_cast<std::size_t>(i)] = on && !null ? 1 : 0;
            holed[static_cast<std::size_t>(i)] =
                on && !null && i % 13 != 0 ? 1 : 0;
            wide_ids.push_back(i);
        }
        DataFrame big;
        big.names = dstd::make_shared_strings({"a"});
        big.columns.push_back(Series::flat_i64(wide_ids.data(), wide));
        const DataFrame flat_want = big.filter(mask_of(wide_plain));
        Idx holes;
        for (std::int64_t i = 0; i < wide; ++i)
            holes.push_back(i % 13 == 0 ? -1 : i);
        dstd::Vector<Layout> wide_layouts = bool_layouts(wide_rows);
        wide_layouts.push_back({"selection with -1",
                                selection_over(mask_of(wide_plain), holes),
                                Cells()});
        for (const Layout& c : wide_layouts) {
            CAPTURE(c.name);
            const DataFrame want = big.filter(c.col.materialize());
            const DataFrame got = big.filter(c.col);
            CHECK(got.num_rows() == want.num_rows());
            const std::int64_t* got_ids = got.column("a").data<std::int64_t>();
            const std::int64_t* want_ids =
                want.column("a").data<std::int64_t>();
            bool same_rows = got.num_rows() == want.num_rows();
            std::int64_t first = -1;
            for (std::int64_t i = 0; same_rows && i < got.num_rows(); ++i)
                if (got_ids[i] != want_ids[i] && first < 0) first = i;
            CAPTURE(got.num_rows());
            CAPTURE(want.num_rows());
            CAPTURE(first);
            CAPTURE(flat_want.num_rows());
            CHECK(same_rows);
        }
    }

    TEST_CASE("string ops read the nulls of every layer") {
        const Rows base = pool_rows(90);
        dstd::Vector<Case> all = cases();
        all.push_back(
            {"chunked", Kind::Flat, string_chunks(base), base, nullptr});
        const Rows holes = pool_rows(90);
        Rows holed = holes;
        for (std::size_t i = 0; i < holed.size(); i += 5) holed[i] = Row();
        const Series null_const = constant_of(flat_of({Row("read")}), 90, true);
        Rows null_const_rows;
        for (std::int64_t i = 0; i < 90; ++i)
            null_const_rows.push_back(null_const.is_null(i) ? Row()
                                                            : Row("read"));
        all.push_back({"constant with nulls", Kind::Flat, null_const.share(),
                       null_const_rows, nullptr});
        const Idx through = shuffled(90, 90, true);
        all.push_back({"selection over constant", Kind::Flat,
                       selection_over(null_const, through),
                       pick(null_const_rows, through), nullptr});
        for (const Case& c : all) {
            CAPTURE(dstd::StringView(c.name));
            const Series flat = flat_of(c.rows);
            CHECK(rows_of(c.col.to_lowercase()) ==
                  rows_of(flat.to_lowercase()));
            CHECK(text_of(c.col.str_len_bytes()) ==
                  text_of(flat.str_len_bytes()));
            CHECK(text_of(c.col.str_contains("a")) ==
                  text_of(flat.str_contains("a")));
            CHECK(text_of(c.col.str_eq("read")) ==
                  text_of(flat.str_eq("read")));
            CHECK(rows_of(c.col.materialize()) == c.rows);
        }
    }

    TEST_CASE("a constant string column is a value, not a null") {
        const Series read = flat_of({Row("read")});
        const Series constant = constant_of(read, 3, false);
        const Series flat = flat_of(Rows(3, Row("read")));
        CHECK(text_of(constant.str_eq("read")) == "1,1,1,");
        CHECK(text_of(constant.str_eq("read")) == text_of(flat.str_eq("read")));
        CHECK(text_of(constant.str_contains("ea")) ==
              text_of(flat.str_contains("ea")));
        CHECK(rows_of(constant.to_lowercase()) == rows_of(flat.to_lowercase()));
        CHECK(text_of(constant.str_len_bytes()) ==
              text_of(flat.str_len_bytes()));
        CHECK(text_of(constant.is_in(flat_of({Row("read")}))) ==
              text_of(flat.is_in(flat_of({Row("read")}))));
        CHECK(constant.nunique() == 1);
        CHECK(constant.min().has_value());
        CHECK(scalar_text(
                  dftu_series_reduce(constant.handle(), DFTU_REDUCE_MIN)) ==
              scalar_text(dftu_series_reduce(flat.handle(), DFTU_REDUCE_MIN)));
        CHECK(scalar_text(
                  dftu_series_reduce(constant.handle(), DFTU_REDUCE_MAX)) ==
              scalar_text(dftu_series_reduce(flat.handle(), DFTU_REDUCE_MAX)));
        const dstd::Vector<std::int64_t> ids{2, 0, 1};
        DataFrame df;
        df.names = dstd::make_shared_strings({"k", "v"});
        df.columns.push_back(constant.share());
        df.columns.push_back(Series::flat_i64(ids.data(), 3));
        const GroupAgg aggs[] = {{Agg::Count, "", "cnt", 0.0},
                                 {Agg::Sum, "v", "sum", 0.0}};
        const DataFrame got = dftu_utils_test::ok(df.group_by("k", aggs));
        REQUIRE(got.num_rows() == 1);
        CHECK_FALSE(got.column("k").is_null(0));
        CHECK(dstd::String(got.column("k").string_at(0)) == "read");
        CHECK(got.column("cnt").data<std::int64_t>()[0] == 3);
        CHECK(got.column("sum").data<std::int64_t>()[0] == 3);
    }

    TEST_CASE("a null dictionary entry is a null row, not a value") {
        for (const TypeId type : {TypeId::Float64, TypeId::Int64}) {
            for (const Layout& c : number_layouts(type, 130)) {
                CAPTURE(c.name);
                const Series flat = numbers(type, c.cells, 0);
                CHECK(c.col.nunique() == flat.nunique());
                CHECK(c.col.count() == flat.count());
                const Series got = c.col.is_unique().materialize();
                const Series want = flat.is_unique().materialize();
                REQUIRE(got.length() == want.length());
                const auto* got_bits = static_cast<const std::uint8_t*>(
                    dftu_series_data(got.handle()));
                const auto* want_bits = static_cast<const std::uint8_t*>(
                    dftu_series_data(want.handle()));
                for (std::int64_t i = 0; i < want.length(); ++i) {
                    CAPTURE(i);
                    CHECK(got.is_null(i) == want.is_null(i));
                    if (!want.is_null(i))
                        CHECK(((got_bits[i >> 3] >> (i & 7)) & 1) ==
                              ((want_bits[i >> 3] >> (i & 7)) & 1));
                }
            }
        }
        const Series dict = dictionary_layout("", TypeId::Int64, 40, false).col;
        std::int64_t nulls = 0;
        for (std::int64_t i = 0; i < dict.length(); ++i)
            nulls += dict.is_null(i);
        CHECK(nulls > 0);
        CHECK(dict.nunique() == dict.materialize().nunique());
        CHECK(dftu_series_null_count(dict.handle()) == nulls);
    }

    TEST_CASE("a selection row with no base row is null under a dictionary") {
        const Cells entries = {Cell(4), Cell(-3), Cell()};
        const Idx through = {0, 1, -1, 0};
        const Series base =
            selection_over(numbers(TypeId::Int64, entries), through);
        const dstd::Vector<std::int32_t> codes{0, 1, 2, 3};
        const Series inner =
            dict_over(TypeId::Int64, codes, base.share(), nullptr);
        const dstd::Vector<std::int32_t> outer_codes{0, 1, 2, 3};
        const Series outer =
            dict_over(TypeId::Int64, outer_codes, inner.share(), nullptr);
        for (const Series* c : {&inner, &outer}) {
            CHECK_FALSE(c->is_null(0));
            CHECK_FALSE(c->is_null(1));
            CHECK(c->is_null(2));
            CHECK_FALSE(c->is_null(3));
            CHECK(c->null_count() == 1);
            CHECK(c->count() == 3);
        }
        CHECK(scalar_text(dftu_series_reduce(outer.handle(),
                                             DFTU_REDUCE_MIN)) == "i-3");
        CHECK(scalar_text(
                  dftu_series_reduce(outer.handle(), DFTU_REDUCE_MAX)) == "i4");
        CHECK(outer.arg_max() == 0);
        const Series flat = outer.materialize();
        CHECK(column_text(outer) == "4,-3,N,4,");
        CHECK(column_text(flat) == column_text(outer));
        CHECK(text_of(flat) == "4,-3,N,4,");
    }
}

TEST_CASE("a float sum of a selection matches the materialized bits") {
    constexpr std::int64_t N = 100000 + 37;
    dstd::Vector<double> values(static_cast<std::size_t>(N));
    for (std::int64_t i = 0; i < N; ++i)
        values[static_cast<std::size_t>(i)] =
            (i % 2 == 0 ? 1.0 : -1.0) *
            (1.0 + 1e-12 * static_cast<double>((i * 31) % 101));
    const Series flat = Series::flat_f64(values.data(), N);

    dstd::Vector<std::int64_t> idx(static_cast<std::size_t>(N));
    for (std::int64_t i = 0; i < N; ++i)
        idx[static_cast<std::size_t>(i)] = (i % 11 == 5) ? -1 : (i * 7919) % N;
    const Series selected = selection_over(flat, idx);

    dstd::Vector<std::int32_t> codes(static_cast<std::size_t>(N));
    for (std::int64_t i = 0; i < N; ++i)
        codes[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(i % 97);
    const Series dictionary =
        dict_over(TypeId::Float64, codes, flat.share(), nullptr);

    struct Input {
        const char* name;
        const Series& col;
    };
    const Input inputs[] = {{"selection", selected},
                            {"dictionary", dictionary}};
    for (const Input& in : inputs) {
        const dftu_scalar got =
            dftu_series_reduce(in.col.handle(), DFTU_REDUCE_SUM);
        const Series copy = in.col.materialize();
        const dftu_scalar want =
            dftu_series_reduce(copy.handle(), DFTU_REDUCE_SUM);
        CHECK(got.kind == want.kind);
        CHECK(std::memcmp(&got.value.d, &want.value.d, sizeof(double)) == 0);
    }
}

namespace {

bool same_scalar(const dftu_scalar& a, const dftu_scalar& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case DFTU_SCALAR_TAG_F64:
            return std::memcmp(&a.value.d, &b.value.d, sizeof(double)) == 0;
        case DFTU_SCALAR_TAG_STR:
            return dstd::StringView(a.value.s, a.len) ==
                   dstd::StringView(b.value.s, b.len);
        case DFTU_SCALAR_TAG_NULL:
            return true;
        case DFTU_SCALAR_TAG_ERR:
            return dstd::StringView(a.value.err->message) ==
                   dstd::StringView(b.value.err->message);
        default:
            return a.value.i == b.value.i;
    }
}

Series two_chunks(Series a, Series b) {
    dstd::Vector<Series> parts;
    parts.push_back(std::move(a));
    parts.push_back(std::move(b));
    return chunks_of(std::move(parts));
}

void check_reduce_matches(const Series& col, dftu_reduce_op op) {
    const Series copy = col.materialize();
    const dftu_scalar got = dftu_series_reduce(col.handle(), op);
    const dftu_scalar want = dftu_series_reduce(copy.handle(), op);
    CHECK(same_scalar(got, want));
}

}  // namespace

TEST_CASE("a chunked mask selects every row under parallel ranges") {
    // Chunks of 97 rows share bitmap bytes at their edges; two threads take
    // alternate ranges of the mask scan.
    constexpr std::int64_t CHUNK_ROWS = 97;
    constexpr std::int64_t CHUNKS = 200;
    dstd::Vector<Series> parts;
    dstd::Vector<std::int64_t> expected;
    for (std::int64_t c = 0; c < CHUNKS; ++c) {
        dstd::Vector<std::uint8_t> bits(
            static_cast<std::size_t>((CHUNK_ROWS + 7) / 8), 0);
        for (std::int64_t r = 0; r < CHUNK_ROWS; ++r) {
            const std::int64_t row = c * CHUNK_ROWS + r;
            if ((row * 2654435761u) % 5 == 0) continue;
            bits[static_cast<std::size_t>(r >> 3)] |=
                static_cast<std::uint8_t>(1u << (r & 7));
            expected.push_back(row);
        }
        parts.push_back(Series{dftu_series_new_flat(DFTU_TYPE_BOOL, bits.data(),
                                                    CHUNK_ROWS, nullptr)});
    }
    const Series mask = chunks_of(std::move(parts));
    set_parallel_backend(
        [](void*, std::int64_t n, std::int64_t grain,
           void (*body)(void*, std::int64_t, std::int64_t), void* ctx) {
            dstd::Thread odd([&] {
                for (std::int64_t b = grain; b < n; b += 2 * grain)
                    body(ctx, b, std::min(n, b + grain));
            });
            for (std::int64_t b = 0; b < n; b += 2 * grain)
                body(ctx, b, std::min(n, b + grain));
            odd.join();
        },
        nullptr);
    int wrong = 0;
    for (int i = 0; i < 2000; ++i) {
        const Series idx = mask_index_column(mask);
        const auto* got =
            static_cast<const std::int64_t*>(dftu_series_data(idx.handle()));
        bool same = idx.length() == static_cast<std::int64_t>(expected.size());
        for (std::int64_t j = 0; same && j < idx.length(); ++j)
            same = got[j] == expected[static_cast<std::size_t>(j)];
        wrong += same ? 0 : 1;
    }
    set_parallel_backend(nullptr, nullptr);
    CHECK(wrong == 0);
}

TEST_CASE("a group sum over a selection wraps as the flat sum does") {
    constexpr auto MAXIMUM = std::numeric_limits<std::int64_t>::max();
    constexpr std::size_t N = 8194;
    Idx values(N, 0), keys(N, 0), idx(N, 0);
    values[0] = -MAXIMUM;
    values[8192] = MAXIMUM;
    values[8193] = 1;
    for (std::size_t i = 0; i < N; ++i) idx[i] = static_cast<std::int64_t>(i);
    const Series flat =
        Series::flat_i64(values.data(), static_cast<std::int64_t>(N));
    const Series key =
        Series::flat_i64(keys.data(), static_cast<std::int64_t>(N));
    const Series selected = selection_over(flat, idx);
    set_parallel_backend(
        [](void*, std::int64_t n, std::int64_t grain,
           void (*body)(void*, std::int64_t, std::int64_t), void* ctx) {
            for (std::int64_t b = 0; b < n; b += grain)
                body(ctx, b, std::min(n, b + grain));
        },
        nullptr);
    auto sum_of = [&](const Series& col) {
        dftu_series* out_keys = nullptr;
        dftu_series* out_values[1]{};
        dftu_series_group_by(key.handle(), col.handle(), DFTU_REDUCE_SUM,
                             &out_keys, out_values, 1);
        std::int64_t sum = 0;
        std::memcpy(&sum, dftu_series_data(out_values[0]), sizeof(sum));
        dftu_series_free(out_keys);
        dftu_series_free(out_values[0]);
        return sum;
    };
    const std::int64_t want = sum_of(selected.materialize());
    const std::int64_t got = sum_of(selected);
    set_parallel_backend(nullptr, nullptr);
    CHECK(want == 1);
    CHECK(got == want);
}

TEST_CASE("a chunked float reduce keeps the order of the flat kernel") {
    const double a[] = {1e16};
    const double b[] = {-1e16, 1};
    const Series sum_col =
        two_chunks(Series::flat_f64(a, 1), Series::flat_f64(b, 2));
    check_reduce_matches(sum_col, DFTU_REDUCE_SUM);
    const double c[] = {10};
    const double d[] = {std::numeric_limits<double>::quiet_NaN(), 1};
    const Series nan_col =
        two_chunks(Series::flat_f64(c, 1), Series::flat_f64(d, 2));
    check_reduce_matches(nan_col, DFTU_REDUCE_MIN);
    check_reduce_matches(nan_col, DFTU_REDUCE_MAX);
    check_reduce_matches(nan_col, DFTU_REDUCE_SUM);
    const dftu_scalar prod = dftu_series_product(nan_col.handle());
    const Series copy = nan_col.materialize();
    CHECK(same_scalar(prod, dftu_series_product(copy.handle())));
}

TEST_CASE("a chunk with no valid row does not enter a chunked reduce") {
    const std::int64_t x[] = {99};
    const std::int64_t y[] = {5, -7};
    const std::uint8_t invalid[] = {0};
    const Series col =
        two_chunks(Series::flat_i64(x, 1, invalid), Series::flat_i64(y, 2));
    for (dftu_reduce_op op :
         {DFTU_REDUCE_MIN, DFTU_REDUCE_MAX, DFTU_REDUCE_SUM}) {
        check_reduce_matches(col, op);
    }
    const Series copy = col.materialize();
    CHECK(dftu_series_arg_min(col.handle()) ==
          dftu_series_arg_min(copy.handle()));
    CHECK(dftu_series_arg_max(col.handle()) ==
          dftu_series_arg_max(copy.handle()));
    CHECK(same_scalar(dftu_series_product(col.handle()),
                      dftu_series_product(copy.handle())));
    CHECK(same_scalar(dftu_series_bitwise_and(col.handle()),
                      dftu_series_bitwise_and(copy.handle())));
    CHECK(same_scalar(dftu_series_bitwise_or(col.handle()),
                      dftu_series_bitwise_or(copy.handle())));
    CHECK(same_scalar(dftu_series_bitwise_xor(col.handle()),
                      dftu_series_bitwise_xor(copy.handle())));

    const std::uint8_t bits[] = {0, 1};
    const std::uint8_t none[] = {0};
    const Series flags = two_chunks(
        Series{dftu_series_new_flat(DFTU_TYPE_BOOL, bits, 1, none)},
        Series{dftu_series_new_flat(DFTU_TYPE_BOOL, bits + 1, 1, nullptr)});
    const Series flags_copy = flags.materialize();
    CHECK(dftu_series_all(flags.handle()) ==
          dftu_series_all(flags_copy.handle()));
    CHECK(dftu_series_any(flags.handle()) ==
          dftu_series_any(flags_copy.handle()));
    for (dftu_reduce_op op :
         {DFTU_REDUCE_MIN, DFTU_REDUCE_MAX, DFTU_REDUCE_SUM})
        check_reduce_matches(flags, op);

    const std::uint8_t all_bad[] = {0};
    const Series empty_valid = two_chunks(Series::flat_i64(x, 1, all_bad),
                                          Series::flat_i64(x, 1, all_bad));
    for (dftu_reduce_op op :
         {DFTU_REDUCE_MIN, DFTU_REDUCE_MAX, DFTU_REDUCE_SUM})
        check_reduce_matches(empty_valid, op);
}

TEST_CASE("a chunked string min and max compare the bytes") {
    // Many chunks in a scrambled order: an address comparison cannot match
    // the byte order for both extremes by luck.
    const char* letters = "pcmgxbkdwahtnesz";
    const std::int32_t offsets[] = {0, 1};
    dstd::Vector<Series> parts;
    for (const char* c = letters; *c != '\0'; ++c)
        parts.push_back(Series{
            dftu_series_new_string(DFTU_TYPE_STRING, offsets, c, 1, nullptr)});
    const Series words = chunks_of(std::move(parts));
    check_reduce_matches(words, DFTU_REDUCE_MIN);
    check_reduce_matches(words, DFTU_REDUCE_MAX);
    const dftu_scalar lo = dftu_series_reduce(words.handle(), DFTU_REDUCE_MIN);
    const dftu_scalar hi = dftu_series_reduce(words.handle(), DFTU_REDUCE_MAX);
    CHECK(dstd::StringView(lo.value.s, lo.len) == "a");
    CHECK(dstd::StringView(hi.value.s, hi.len) == "z");
}

TEST_CASE("an empty string under a selection is a value, not a null key") {
    const std::int32_t offsets[] = {0, 0, 0};
    const Series base{dftu_series_new_string_borrowed(
        DFTU_TYPE_STRING, offsets, nullptr, 0, 2, nullptr, nullptr, nullptr)};
    const Idx idx = {1, 0, 1};
    const Series keys = selection_over(base, idx);
    const Series flat = keys.materialize();
    const std::int64_t vals[] = {1, 2, 3};
    const Series values = Series::flat_i64(vals, 3);
    int nulls[2] = {0, 0};
    std::int32_t outputs[2] = {0, 0};
    int at = 0;
    for (const Series* k : {&keys, &flat}) {
        dftu_series* out_keys = nullptr;
        dftu_series* out_vals[1]{};
        outputs[at] =
            dftu_series_group_by(k->handle(), values.handle(),
                                 DFTU_REDUCE_COUNT, &out_keys, out_vals, 1);
        for (std::int64_t i = 0; i < dftu_series_length(out_keys); ++i)
            nulls[at] += dftu_series_is_null(out_keys, i) ? 1 : 0;
        CHECK(dftu_series_length(out_keys) == 1);
        dftu_series_free(out_keys);
        dftu_series_free(out_vals[0]);
        ++at;
    }
    CHECK(nulls[0] == nulls[1]);
    CHECK(nulls[0] == 0);
    CHECK(outputs[0] == outputs[1]);
}

namespace {

// Runs `on_view` and keeps its error text before `on_copy` runs, since both
// errors share one thread-local slot.
template <class F, class G>
bool same_result(F on_view, G on_copy) {
    const dftu_scalar x = on_view();
    const dstd::String x_err = x.kind == DFTU_SCALAR_TAG_ERR
                                   ? dstd::String(x.value.err->message)
                                   : dstd::String();
    const dftu_scalar y = on_copy();
    if (x.kind == DFTU_SCALAR_TAG_ERR && y.kind == DFTU_SCALAR_TAG_ERR)
        return dstd::StringView(x_err) ==
               dstd::StringView(y.value.err->message);
    return same_scalar(x, y);
}

void check_every_reduce_matches(const Series& col) {
    const Series copy = col.materialize();
    const dftu_series* a = col.handle();
    const dftu_series* b = copy.handle();
    for (dftu_reduce_op op :
         {DFTU_REDUCE_SUM, DFTU_REDUCE_MIN, DFTU_REDUCE_MAX}) {
        CAPTURE(static_cast<int>(op));
        CHECK(same_result([&] { return dftu_series_reduce(a, op); },
                          [&] { return dftu_series_reduce(b, op); }));
    }
    CHECK(same_result([&] { return dftu_series_product(a); },
                      [&] { return dftu_series_product(b); }));
    CHECK(same_result([&] { return dftu_series_bitwise_and(a); },
                      [&] { return dftu_series_bitwise_and(b); }));
    CHECK(same_result([&] { return dftu_series_bitwise_or(a); },
                      [&] { return dftu_series_bitwise_or(b); }));
    CHECK(same_result([&] { return dftu_series_bitwise_xor(a); },
                      [&] { return dftu_series_bitwise_xor(b); }));
    CHECK(dftu_series_arg_min(a) == dftu_series_arg_min(b));
    CHECK(dftu_series_arg_max(a) == dftu_series_arg_max(b));
    CHECK(same_result([&] { return dftu_series_mode(a); },
                      [&] { return dftu_series_mode(b); }));
    CHECK(dftu_series_all(a) == dftu_series_all(b));
    CHECK(dftu_series_any(a) == dftu_series_any(b));
}

Series constant_over(const Series& base, std::int64_t n,
                     bool all_null = false) {
    auto* c = new dftu_series(*base.handle());
    c->encoding = Encoding::Constant;
    c->length = n;
    if (all_null) {
        c->validity = new_buffer(static_cast<std::size_t>((n + 7) / 8));
        std::memset(writable(c->validity), 0,
                    static_cast<std::size_t>((n + 7) / 8));
        c->null_count = n;
    }
    return Series{c};
}

}  // namespace

TEST_CASE("an empty or all-null layered column reduces to the flat identity") {
    const std::int64_t signed_values[] = {10, 20, 30};
    const std::uint64_t unsigned_values[] = {10, 20, 30};
    const Idx none;
    const Idx missing = {-1, -1};
    const std::int32_t codes[] = {0, 1, 0};
    const std::uint8_t no_row_valid[] = {0};
    for (const TypeId type : {TypeId::Int64, TypeId::Uint64}) {
        CAPTURE(static_cast<int>(type));
        const Series base =
            type == TypeId::Int64
                ? Series::flat_i64(signed_values, 3)
                : Series::flat(TypeId::Uint64, unsigned_values, 3);
        check_every_reduce_matches(selection_over(base, none));
        check_every_reduce_matches(selection_over(base, missing));
        const Series values =
            type == TypeId::Int64
                ? Series::flat_i64(signed_values, 3)
                : Series::flat(TypeId::Uint64, unsigned_values, 3);
        check_every_reduce_matches(
            dict_over(type, dstd::Span<const std::int32_t>(codes, 0),
                      Series(values.share()), nullptr));
        check_every_reduce_matches(
            dict_over(type, codes, Series(values.share()), no_row_valid));
        check_every_reduce_matches(constant_over(base, 0));
        check_every_reduce_matches(constant_over(base, 3, true));
        dstd::Vector<Series> empties;
        empties.push_back(selection_over(base, none));
        empties.push_back(selection_over(base, missing));
        check_every_reduce_matches(chunks_of(std::move(empties)));
    }
    const Series kept = chunks_of([&] {
        dstd::Vector<Series> parts;
        parts.push_back(selection_over(
            Series::flat(TypeId::Uint64, unsigned_values, 3), none));
        return parts;
    }());
    CHECK(dftu_series_reduce(kept.handle(), DFTU_REDUCE_SUM).kind ==
          DFTU_SCALAR_TAG_U64);
}

TEST_CASE(
    "a chunked integer product wraps across blocks as the flat one does") {
    constexpr std::size_t N = 3 * 8192;
    Idx values(N, 1), idx(N, 0);
    values[0] = 3037000500;
    values[8192] = 3037000500;
    for (std::size_t i = 0; i < N; ++i) idx[i] = static_cast<std::int64_t>(i);
    const Series flat =
        Series::flat_i64(values.data(), static_cast<std::int64_t>(N));
    const Series selected = selection_over(flat, idx);
    CHECK(same_scalar(dftu_series_product(selected.handle()),
                      dftu_series_product(flat.handle())));
}

TEST_CASE("a bool group key reads and writes bits in every layout") {
    const std::uint8_t bits[1] = {0b1010};
    const Series flat{dftu_series_new_flat(DFTU_TYPE_BOOL, bits, 4, nullptr)};
    const Series selected = selection_over(flat, Idx{0, 1, 2, 3});
    const std::int64_t vals[] = {1, 2, 3, 4};
    const Series values = Series::flat_i64(vals, 4);
    for (const Series* key : {&flat, &selected}) {
        dftu_series* out_keys = nullptr;
        dftu_series* out_vals[1]{};
        dftu_series_group_by(key->handle(), values.handle(), DFTU_REDUCE_SUM,
                             &out_keys, out_vals, 1);
        REQUIRE(dftu_series_length(out_keys) == 2);
        const auto* packed =
            static_cast<const std::uint8_t*>(dftu_series_data(out_keys));
        const auto* sums =
            static_cast<const std::int64_t*>(dftu_series_data(out_vals[0]));
        CHECK((packed[0] & 3) == 2);
        CHECK(sums[0] == 4);
        CHECK(sums[1] == 6);
        dftu_series_free(out_keys);
        dftu_series_free(out_vals[0]);
    }
}

TEST_CASE("a constant string key reads a row whose first row is null") {
    const dstd::StringView one[] = {"read"};
    const Series base = Series::strings(one);
    const Series constant = constant_over(base, 3);
    auto* with_null = new dftu_series(*constant.handle());
    with_null->validity = new_buffer(1);
    *writable(with_null->validity) = 0b110;
    with_null->null_count = 1;
    const Series keys{with_null};
    const Series copy = keys.materialize();
    const std::int64_t vals[] = {10, 20, 30};
    const Series values = Series::flat_i64(vals, 3);
    Rows rows[2];
    std::int64_t sums[2][3] = {};
    int at = 0;
    for (const Series* key : {&keys, &copy}) {
        dftu_series* out_keys = nullptr;
        dftu_series* out_vals[1]{};
        dftu_series_group_by(key->handle(), values.handle(), DFTU_REDUCE_SUM,
                             &out_keys, out_vals, 1);
        const Series got{out_keys};
        REQUIRE(got.length() == 2);
        rows[at] = rows_of(got);
        std::memcpy(sums[at], dftu_series_data(out_vals[0]),
                    2 * sizeof(std::int64_t));
        dftu_series_free(out_vals[0]);
        ++at;
    }
    CHECK(rows[0] == rows[1]);
    CHECK(sums[0][0] == sums[1][0]);
    CHECK(sums[0][1] == sums[1][1]);
}

TEST_CASE(
    "a mask gather clears the valid bit of a selection row with no base") {
    const std::uint8_t bits[2] = {0xff, 0xff};
    const Series flat{dftu_series_new_flat(DFTU_TYPE_BOOL, bits, 16, nullptr)};
    // Twelve rows: -1 rows inside the eight-row loop and in the tail.
    const Idx idx = {0, -1, 2, -1, 4, 5, -1, 7, 8, -1, 10, 11};
    const Series selected = selection_over(flat, idx);
    const MaskBits mask(*selected.handle());
    std::uint8_t out_bits[2] = {0xff, 0xff};
    std::uint8_t out_valid[2] = {0, 0};
    mask.gather(0, 12, out_bits, out_valid);
    for (std::size_t i = 0; i < idx.size(); ++i) {
        CAPTURE(i);
        const bool want = idx[i] >= 0;
        CHECK(((out_valid[i >> 3] >> (i & 7)) & 1) == want);
        CHECK(((out_bits[i >> 3] >> (i & 7)) & 1) == want);
    }
}

TEST_CASE("a constant string column reads through dftu_series_string_at") {
    const dstd::StringView one[] = {"read"};
    const Series constant = constant_over(Series::strings(one), 3);
    for (std::int64_t i = 0; i < 3; ++i) {
        std::int64_t len = 0;
        const char* s = dftu_series_string_at(constant.handle(), i, &len);
        REQUIRE(s != nullptr);
        CHECK(dstd::StringView(s, static_cast<std::size_t>(len)) == "read");
    }
    CHECK(constant.string_at(1) == "read");
}

TEST_CASE("a chunked column reduces chunk by chunk as its copy does") {
    constexpr std::size_t PER_CHUNK[] = {5, 9, 1, 12};
    for (const TypeId type :
         {TypeId::Int64, TypeId::Int32, TypeId::Uint64, TypeId::Uint8}) {
        CAPTURE(static_cast<int>(type));
        for (int salt = 0; salt < 4; ++salt) {
            dstd::Vector<Series> parts;
            std::uint64_t x = static_cast<std::uint64_t>(salt) + 3;
            for (std::size_t len : PER_CHUNK) {
                dstd::Vector<std::uint64_t> raw(len);
                dstd::Vector<std::uint8_t> valid((len + 7) / 8, 0xff);
                for (std::size_t i = 0; i < len; ++i) {
                    x = x * 6364136223846793005ull + 1442695040888963407ull;
                    raw[i] =
                        (x >> 33) % 5 +
                        (type == TypeId::Uint64 ? 0x8000000000000000ull : 0);
                    if (salt >= 2 && (x >> 20) % 3 == 0)
                        valid[i >> 3] = static_cast<std::uint8_t>(
                            valid[i >> 3] & ~(1u << (i & 7)));
                }
                if (salt == 3 && len == 1) valid[0] = 0;
                dstd::Vector<std::int64_t> wide(len);
                for (std::size_t i = 0; i < len; ++i)
                    wide[i] = static_cast<std::int64_t>(raw[i]);
                Series chunk =
                    type == TypeId::Int64
                        ? Series::flat_i64(wide.data(),
                                           static_cast<std::int64_t>(len),
                                           valid.data())
                        : Series::flat(type, nullptr, 0);
                if (type != TypeId::Int64) {
                    switch (type) {
                        case TypeId::Int32: {
                            dstd::Vector<std::int32_t> v(len);
                            for (std::size_t i = 0; i < len; ++i)
                                v[i] = static_cast<std::int32_t>(raw[i]) - 2;
                            chunk = Series::flat(type, v.data(),
                                                 static_cast<std::int64_t>(len),
                                                 valid.data());
                            break;
                        }
                        case TypeId::Uint8: {
                            dstd::Vector<std::uint8_t> v(len);
                            for (std::size_t i = 0; i < len; ++i)
                                v[i] = static_cast<std::uint8_t>(raw[i] + 250);
                            chunk = Series::flat(type, v.data(),
                                                 static_cast<std::int64_t>(len),
                                                 valid.data());
                            break;
                        }
                        default:
                            chunk = Series::flat(type, raw.data(),
                                                 static_cast<std::int64_t>(len),
                                                 valid.data());
                    }
                }
                parts.push_back(std::move(chunk));
            }
            check_every_reduce_matches(chunks_of(std::move(parts)));
        }
    }
    const std::uint8_t a[] = {0b101};
    const std::uint8_t b[] = {0b011};
    const std::uint8_t only_first[] = {0b1};
    const Series flags = two_chunks(
        Series{dftu_series_new_flat(DFTU_TYPE_BOOL, a, 3, nullptr)},
        Series{dftu_series_new_flat(DFTU_TYPE_BOOL, b, 3, only_first)});
    check_every_reduce_matches(flags);
}

TEST_CASE("a chunked quantile of signed zeros has the sign of the copy's") {
    std::uint64_t state = 7;
    int differ = 0;
    for (int round = 0; round < 400; ++round) {
        const std::size_t n = 20 + static_cast<std::size_t>(round % 40);
        dstd::Vector<double> d(n);
        for (double& v : d) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const auto k = (state >> 33) % 4;
            v = k == 0 ? -1.0 : k == 1 ? -0.0 : k == 2 ? 0.0 : 1.0;
        }
        const Series flat =
            Series::flat_f64(d.data(), static_cast<std::int64_t>(n));
        const std::size_t cut[] = {0, n / 3, 2 * n / 3, n};
        dstd::Vector<Series> parts;
        for (int k = 0; k < 3; ++k)
            parts.push_back(Series::flat_f64(
                d.data() + cut[k],
                static_cast<std::int64_t>(cut[k + 1] - cut[k])));
        const Series chunked = chunks_of(std::move(parts));
        const double a = dftu_series_quantile_interp(chunked.handle(), 0.5,
                                                     DFTU_INTERP_LOWER);
        const double b =
            dftu_series_quantile_interp(flat.handle(), 0.5, DFTU_INTERP_LOWER);
        differ += std::memcmp(&a, &b, sizeof(double)) != 0;
    }
    CHECK(differ == 0);
}

TEST_CASE(
    "every layout of a temporal or float column reduces as its copy does") {
    const std::int64_t ticks[] = {5, -3, 5, 9, -3, 0};
    const double reals[] = {1.5, -2.5, 1.5, 9.0, -2.5, 0.0};
    dstd::Vector<Series> bases;
    for (const TypeId type : {TypeId::Timestamp, TypeId::Date64,
                              TypeId::Duration, TypeId::Time64}) {
        Series s = Series::flat_i64(ticks, 6);
        s.handle()->type = type;
        bases.push_back(std::move(s));
    }
    {
        const std::int32_t days[] = {5, -3, 5, 9, -3, 0};
        Series s = Series::flat(TypeId::Int32, days, 6);
        s.handle()->type = TypeId::Date32;
        bases.push_back(std::move(s));
    }
    bases.push_back(Series::flat_f64(reals, 6));
    for (const Series& base : bases) {
        CAPTURE(static_cast<int>(base.type()));
        check_every_reduce_matches(selection_over(base, Idx{3, 1, 1, 0, 5, 2}));
        check_every_reduce_matches(
            selection_over(base, Idx{3, -1, 1, 0, -1, 2}));
        check_every_reduce_matches(two_chunks(base.share(), base.share()));
        check_every_reduce_matches(constant_over(base.slice(0, 1), 4));
        const std::int32_t codes[] = {2, 0, 1, 0, 2};
        check_every_reduce_matches(
            dictionary_over(codes, base.slice(0, 3), nullptr));
    }
}

TEST_CASE("a flat integer product wraps without signed overflow") {
    constexpr std::int64_t BIG = 3037000500;
    for (std::size_t n = 2; n <= 9; ++n) {
        for (std::size_t first = 0; first < n; ++first) {
            for (std::size_t second = first + 1; second < n; ++second) {
                Idx v(n, 1);
                v[first] = BIG;
                v[second] = BIG;
                const Series col =
                    Series::flat_i64(v.data(), static_cast<std::int64_t>(n));
                const auto want =
                    static_cast<std::int64_t>(static_cast<std::uint64_t>(BIG) *
                                              static_cast<std::uint64_t>(BIG));
                const dftu_scalar got = dftu_series_product(col.handle());
                CHECK(got.kind == DFTU_SCALAR_TAG_I64);
                CHECK(got.value.i == want);
            }
        }
    }
}

TEST_CASE("a bool group key stays a bool column in the aggregated frame") {
    const std::uint8_t bits[1] = {0b1001};
    const std::uint8_t valid[1] = {0b1011};
    const std::int64_t vals[] = {1, 2, 3, 4};
    for (const std::uint8_t* validity :
         {static_cast<const std::uint8_t*>(nullptr), valid}) {
        DataFrame df;
        df.names = dstd::make_shared_strings({"k", "v"});
        df.columns.push_back(
            Series{dftu_series_new_flat(DFTU_TYPE_BOOL, bits, 4, validity)});
        df.columns.push_back(Series::flat_i64(vals, 4));
        const GroupAgg aggs[] = {{Agg::Sum, "v", "sum", 0.0}};
        const DataFrame got = dftu_utils_test::ok(df.group_by("k", aggs));
        const Series& key = got.column("k");
        CHECK(key.type() == TypeId::Bool);
        const auto* packed =
            static_cast<const std::uint8_t*>(dftu_series_data(key.handle()));
        CHECK((packed[0] & 1) == 1);
        CHECK(((packed[0] >> 1) & 1) == 0);
    }
}

TEST_CASE("a quantile of negative zeros is 0.0 in every layout") {
    const double zeros[] = {-0.0, -0.0};
    const Series flat = Series::flat_f64(zeros, 2);
    const Series chunked =
        two_chunks(Series::flat_f64(zeros, 1), Series::flat_f64(zeros + 1, 1));
    const Series selected = selection_over(flat, Idx{1, 0});
    const std::int64_t ids[] = {1, 1};
    for (const Series* values : {&flat, &chunked, &selected}) {
        DataFrame df;
        df.names = dstd::make_shared_strings({"k", "v"});
        df.columns.push_back(Series::flat_i64(ids, 2));
        df.columns.push_back(values->share());
        for (const Interpolation interp :
             {Interpolation::Nearest, Interpolation::Midpoint,
              Interpolation::Lower, Interpolation::Higher}) {
            const GroupAgg aggs[] = {
                {Agg::Quantile, "v", "q", 0.5, {}, {}, {}, interp}};
            const DataFrame got = dftu_utils_test::ok(df.group_by("k", aggs));
            const double q = got.column("q").data<double>()[0];
            CHECK(q == 0.0);
            CHECK_FALSE(std::signbit(q));
        }
        const Series rolled{dftu_series_rolling_median(values->handle(), 2)};
        const double last = rolled.data<double>()[1];
        CHECK(last == 0.0);
        CHECK_FALSE(std::signbit(last));
        const Series rolled_q{
            dftu_series_rolling_quantile(values->handle(), 2, 0.5)};
        CHECK_FALSE(std::signbit(rolled_q.data<double>()[1]));
        const double m = dftu_series_quantile_interp(values->handle(), 0.5,
                                                     DFTU_INTERP_NEAREST);
        CHECK_FALSE(std::signbit(m));
    }
}
