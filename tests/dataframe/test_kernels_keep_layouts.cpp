// Gathers keep a string column's layout: a view stays a view over the same
// blobs, a dictionary stays a dictionary over the same child, and the values
// and nulls equal the flat path.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>
#include <dftracer/utils/dataframe/kernels/dictionary.h>
#include <dftracer/utils/dataframe/kernels/filter.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/dataframe/parallel.h>
#include <dftracer/utils/duql/query.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

using Row = std::optional<std::string>;
using Rows = std::vector<Row>;
using Idx = std::vector<std::int64_t>;

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
    std::vector<std::string_view> v;
    std::vector<std::uint8_t> valid((rows.size() + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        v.emplace_back(rows[i] ? std::string_view(*rows[i])
                               : std::string_view());
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

Series selection_over(const Series& base, const Idx& idx) {
    auto* o = new dftu_series();
    adopt_type_from(*o, *base.handle());
    o->encoding = Encoding::Selection;
    o->length = static_cast<std::int64_t>(idx.size());
    o->data = Buffer::allocate(idx.size() * sizeof(std::int64_t));
    std::memcpy(o->data->data(), idx.data(), idx.size() * sizeof(std::int64_t));
    o->set_child(std::make_shared<dftu_series>(*base.handle()));
    return Series{o};
}

Rows rows_of(const Series& s) {
    Rows r;
    for (std::int64_t i = 0; i < s.length(); ++i)
        r.push_back(s.is_null(i) ? Row() : Row(std::string(s.string_at(i))));
    return r;
}

Rows pick(const Rows& rows, const Idx& idx) {
    Rows r;
    for (std::int64_t k : idx)
        r.push_back(k < 0 ? Row() : rows[static_cast<std::size_t>(k)]);
    return r;
}

Series mask_of(const std::vector<bool>& v) {
    std::vector<std::uint8_t> bits((v.size() + 7) / 8, 0);
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
    return s.handle()->child()->data->data();
}

std::vector<Case> cases() {
    const Rows base = pool_rows(120);
    const Idx sel = {5, 5, -1, 17, 0, 119, 64, 3, 3, 99, 40, 41, 42, 7};
    std::vector<Case> out;
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
            ->set_child(std::shared_ptr<dftu_series>(
                view_of(rows_of(values)).release()));
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

std::string truth_of(const Series& m) {
    std::string out;
    for (std::int64_t i = 0; i < m.length(); ++i) {
        if (m.is_null(i))
            out += 'N';
        else
            out +=
                ((m.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) ? 'T' : 'F';
    }
    return out;
}

std::string ints_of(const Series& m) {
    std::string out;
    for (std::int64_t i = 0; i < m.length(); ++i)
        out += (m.is_null(i) ? std::string("N")
                             : std::to_string(m.data<std::int64_t>()[i])) +
               ",";
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

Series run(StrFn fn, const Series& col, const char* arg) {
    return Series{
        fn(col.handle(), arg, static_cast<std::int32_t>(std::strlen(arg)))};
}

Series cmp_str(const Series& col, dftu_cmp_op op, const std::string& lit) {
    dftu_scalar rhs{};
    rhs.kind = DFTU_SCALAR_TAG_STR;
    rhs.len = static_cast<std::uint32_t>(lit.size());
    rhs.value.s = lit.data();
    return Series{dftu_series_compare(col.handle(), op, rhs)};
}

std::string in_truth(const Series& col, const std::string& query) {
    DataFrame f;
    f.names = {"name"};
    f.columns.push_back(col.share());
    auto q = dftracer::utils::duql::Query::from_string(query);
    REQUIRE(q.has_value());
    return truth_of(evaluate_mask(q->root(), f));
}

}  // namespace

TEST_SUITE("kernels keep layouts") {
    TEST_CASE("filter of a 3-value dictionary keeps the dictionary") {
        std::vector<std::string> cats;
        for (int i = 0; i < 1000; ++i)
            cats.push_back(i % 3 == 0 ? "read" : i % 3 == 1 ? "write" : "open");
        std::vector<std::int64_t> ids;
        std::vector<bool> keep;
        for (int i = 0; i < 1000; ++i) {
            ids.push_back(i);
            keep.push_back(i % 10 == 3);
        }
        Series flat = Series::strings(cats);
        DataFrame df;
        df.names = {"cat", "id"};
        df.columns.push_back(flat.dictionary_encode());
        df.columns.push_back(Series::flat_i64(ids.data(), 1000));
        DataFrame flat_df;
        flat_df.names = {"cat", "id"};
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

            std::vector<bool> keep;
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
            std::vector<std::int32_t> idx32(idx.begin(), idx.end());
            Series t = take32(c.col, idx32);
            check_layout(c, t);
            CHECK(rows_of(t) == pick(c.rows, idx));

            std::vector<Series> cols;
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
        std::vector<std::int64_t> id(120), key(120);
        for (int i = 0; i < 120; ++i) {
            id[static_cast<std::size_t>(i)] = (i * 37) % 120;
            key[static_cast<std::size_t>(i)] = i % 7;
        }
        auto frame = [&](Series col) {
            DataFrame df;
            df.names = {"s", "id", "key"};
            df.columns.push_back(std::move(col));
            df.columns.push_back(Series::flat_i64(id.data(), 120));
            df.columns.push_back(Series::flat_i64(key.data(), 120));
            return df;
        };
        std::vector<bool> keep;
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
            same(df.take({3, -1, 3, 80}), flat.take({3, -1, 3, 80}));
            same(df.sort_by("id"), flat.sort_by("id"));
            same(df.sort_by_multi({"key", "id"}, true),
                 flat.sort_by_multi({"key", "id"}, true));

            DataFrame right;
            right.names = {"key", "w"};
            std::vector<std::int64_t> rk = {1, 2, 2, 9}, rw = {10, 20, 21, 90};
            right.columns.push_back(Series::flat_i64(rk.data(), 4));
            right.columns.push_back(Series::flat_i64(rw.data(), 4));
            for (JoinHow how : {JoinHow::Inner, JoinHow::Left}) {
                DataFrame j = df.join(right, {"key"}, how);
                DataFrame jf = flat.join(right, {"key"}, how);
                REQUIRE(j.num_rows() == jf.num_rows());
                CHECK(rows_of(j.columns[0]) == rows_of(jf.columns[0]));
                check_layout(c, j.columns[0]);
            }
        }
    }

    TEST_CASE("a dictionary entry that is null makes its rows null") {
        const std::vector<std::string_view> vals{"a", "", "c"};
        const std::uint8_t child_valid = 0b101;
        const std::vector<std::int32_t> codes{0, 1, 0, 2};
        Series d = dictionary_from_codes(
            codes,
            Series::strings(std::span<const std::string_view>(vals),
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
        std::vector<std::string> cats;
        for (int i = 0; i < 1000; ++i)
            cats.push_back(i % 3 == 0 ? "POSIX" : i % 3 == 1 ? "STDIO" : "PO");
        std::vector<std::string_view> views(cats.begin(), cats.end());
        const Series flat = Series::strings(views);
        const Series dict = flat.dictionary_encode();
        REQUIRE(dict.encoding() == Encoding::Dictionary);
        const Series got = run(dftu_series_str_starts_with, dict, "PO");
        CHECK(got.encoding() == Encoding::Flat);
        CHECK(truth_of(got) ==
              truth_of(run(dftu_series_str_starts_with, flat, "PO")));
    }

    TEST_CASE("a null dictionary entry gives null predicate rows") {
        const std::vector<std::string_view> vals{"apple", "", "pear"};
        const std::uint8_t child_valid = 0b101;
        const std::vector<std::int32_t> codes{0, 1, 2, 0, 1};
        const Series d = dictionary_from_codes(
            codes,
            Series::strings(std::span<const std::string_view>(vals),
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
        std::vector<std::string> lits;
        for (int i = 0; i < 18; ++i) lits.push_back("lit" + std::to_string(i));
        lits.push_back("read");
        lits.push_back("a name of exactly 16");
        for (const std::size_t n :
             {std::size_t{1}, std::size_t{2}, lits.size()}) {
            std::string list = "\"" + lits[lits.size() - 1] + "\"";
            if (n == 1) list = "\"read\"";
            if (n == 2) list = "\"read\", \"write\"";
            if (n == lits.size()) {
                list.clear();
                for (const std::string& l : lits)
                    list += (list.empty() ? "" : ", ") + ("\"" + l + "\"");
            }
            for (const char* head : {"name in [", "name not in ["}) {
                const std::string q = std::string(head) + list + "]";
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
    std::vector<Row> keys;
    std::vector<std::int64_t> a, b, c;
};

Frame scale_frame(bool long_keys) {
    Frame f;
    for (std::int64_t i = 0; i < SCALE_ROWS; ++i) {
        const std::int64_t g = (i * 7919) % SCALE_GROUPS;
        if (g % 97 == 0)
            f.keys.emplace_back();
        else
            f.keys.emplace_back(
                (long_keys ? "a group key longer than sixteen bytes " : "k") +
                std::to_string(g));
        f.a.push_back(i % 1000);
        f.b.push_back((i * 31) % 977);
        f.c.push_back(i % 13);
    }
    return f;
}

std::vector<std::string> int_cells(const DataFrame& df) {
    std::vector<std::string> out;
    for (const Series& col : df.columns)
        for (std::int64_t i = 0; i < col.length(); ++i) {
            if (col.is_null(i))
                out.push_back("<null>");
            else if (col.type() == TypeId::String)
                out.emplace_back(col.string_at(i));
            else if (col.type() == TypeId::Int64)
                out.push_back(std::to_string(col.data<std::int64_t>()[i]));
        }
    return out;
}

std::vector<AggSpec> scale_specs() {
    auto s = [](AggOp op, std::int32_t col, const char* out) {
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
    const std::vector<const Series*> values{&a, &b, &c};
    const auto names = std::vector<std::string>{"k"};
    const DataFrame want = group_agg({&flat}, values, scale_specs(), names);
    REQUIRE(want.num_rows() > 2048);
    ViewBuilder vb;
    vb.append_column(*flat.handle());
    const Series view = vb.finish(TypeId::String, false);
    const Series dict = flat.dictionary_encode();
    REQUIRE(view.encoding() == Encoding::View);
    REQUIRE(dict.encoding() == Encoding::Dictionary);
    for (const Series* key : {&view, &dict}) {
        const DataFrame got = group_agg({key}, values, scale_specs(), names);
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
        CHECK_NOTHROW(Series::check_string_bytes(MAX));
        CHECK_THROWS_AS(Series::check_string_bytes(MAX + 1), std::length_error);
    }

    TEST_CASE("offsets from lengths refuse a total past the offset width") {
        constexpr std::int32_t MAX = std::numeric_limits<std::int32_t>::max();
        std::vector<std::int32_t> fits = {0, MAX / 2, MAX / 2, 1};
        REQUIRE(offsets_from_lengths(fits.data(), 3));
        CHECK(fits[3] == MAX);
        CHECK(fits[1] == MAX / 2);
        std::vector<std::int32_t> over = {0, MAX / 2, MAX / 2, 2};
        CHECK_FALSE(offsets_from_lengths(over.data(), 3));
        std::vector<std::int64_t> wide = {0, MAX, MAX, MAX};
        REQUIRE(offsets_from_lengths(wide.data(), 3));
        CHECK(wide[3] == std::int64_t{MAX} * 3);
    }
    TEST_CASE("sorts by a string key of any layout equal the general sort") {
        constexpr std::size_t N = 100000;
        constexpr std::size_t DISTINCT = 37;
        auto text = [](std::size_t j) {
            std::string t = "name_" + std::to_string((j * 7919) % 1000);
            if (j % 5 == 0) t += " with a tail longer than twelve bytes";
            return t;
        };
        std::vector<std::string> child;
        for (std::size_t j = 0; j < DISTINCT; ++j) child.push_back(text(j));
        child.push_back(child[3]);
        child.push_back(child[11]);
        child.push_back("");
        const std::size_t null_entry = child.size();
        child.push_back("never read");
        std::vector<std::uint8_t> child_valid((child.size() + 7) / 8, 0xFF);
        child_valid[null_entry >> 3] &=
            static_cast<std::uint8_t>(~(1u << (null_entry & 7)));
        std::vector<std::string_view> views(child.begin(), child.end());

        std::vector<std::int32_t> codes(N);
        std::vector<std::uint8_t> row_valid((N + 7) / 8, 0xFF);
        std::vector<std::int64_t> ids(N), other(N);
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
            Series::strings(std::span<const std::string_view>(views),
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
        std::vector<Key> keys;
        keys.push_back({"dictionary", dict.share(), rows});
        keys.push_back({"flat", flat_of(rows), rows});
        keys.push_back({"view", view_of(rows), rows});
        Rows unique(N);
        for (std::size_t i = 0; i < N; ++i)
            unique[i] = i % 13 == 0 ? Row()
                        : i % 50 == 0
                            ? Row("duplicated")
                            : Row("u" + std::to_string((i * 7919) % N));
        keys.push_back({"view mostly unique", view_of(unique), unique});
        keys.push_back({"selection over flat",
                        selection_over(flat_of(rows), sel), pick(rows, sel)});
        keys.push_back({"selection over dictionary", selection_over(dict, sel),
                        pick(rows, sel)});

        auto expected = [&](const Rows& r, bool desc, bool with_other) {
            Idx idx(N);
            for (std::size_t i = 0; i < N; ++i)
                idx[i] = static_cast<std::int64_t>(i);
            std::stable_sort(
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
            df.names = {"s", "id", "o"};
            df.columns.push_back(k.col.share());
            df.columns.push_back(Series::flat_i64(ids.data(), N));
            df.columns.push_back(Series::flat_i64(other.data(), N));
            for (bool desc : {false, true}) {
                CAPTURE(desc);
                const Idx one = expected(k.rows, desc, false);
                CHECK(ids_of(df.sort_by("s", desc).columns[1]) == one);
                CHECK(ids_of(df.sort_by_multi({"s"}, desc).columns[1]) == one);
                CHECK(ids_of(k.col.argsort(desc)) == one);
                CHECK(ids_of(df.sort_by_multi({"s", "o"}, desc).columns[1]) ==
                      expected(k.rows, desc, true));
            }
        }
    }

    TEST_CASE("materialize and argsort read nested selection and dictionary") {
        const std::vector<std::string_view> vals{"a", "b", "c"};
        const std::vector<std::int32_t> codes{0, 1, 0, 2};
        const Series dict = dictionary_from_codes(
            codes, Series::strings(std::span<const std::string_view>(vals)),
            nullptr);

        auto* sel = new dftu_series();
        sel->type = TypeId::String;
        sel->encoding = Encoding::Selection;
        sel->length = 3;
        sel->data = Buffer::allocate(3 * sizeof(std::int64_t));
        const std::int64_t idx[3] = {3, 0, -1};
        std::memcpy(sel->data->data(), idx, sizeof idx);
        sel->set_child(
            std::shared_ptr<dftu_series>(dftu_series_share(dict.handle())));
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
        outer->data = Buffer::allocate(2 * sizeof(std::int32_t));
        const std::int32_t outer_codes[2] = {1, 0};
        std::memcpy(outer->data->data(), outer_codes, sizeof outer_codes);
        outer->set_child(std::shared_ptr<dftu_series>(
            dftu_series_share(over_dict.handle())));
        const Series dict_over_sel{outer};
        const Series flat2 = dict_over_sel.materialize();
        REQUIRE(flat2.valid());
        CHECK(flat2.string_at(0) == "a");
        CHECK(flat2.string_at(1) == "c");
    }
}
