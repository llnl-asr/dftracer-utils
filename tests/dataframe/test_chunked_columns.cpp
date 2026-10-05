// A CHUNKED column reads, slices, joins and counts bytes as the one-buffer
// column of the same rows does, and a streamed collect returns one.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>
#include <dftracer/utils/dataframe/kernels/field_stat.h>
#include <dftracer/utils/dataframe/kernels/sort.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

dftu_scalar scalar_str(std::string_view v) { return str(v); }

constexpr std::int64_t N = 40;
const std::int64_t CUTS[] = {0, 13, 14, 29, N};

using Row = std::optional<std::string>;
using Rows = std::vector<Row>;

const char* const POOL[] = {"read", "write", "a name of exactly 16", "",
                            "a considerably longer name than the others"};

bool null_at(std::int64_t i, bool nulls) { return nulls && i % 5 == 2; }

Rows rows_for(std::int64_t from, std::int64_t to, bool nulls) {
    Rows r;
    for (std::int64_t i = from; i < to; ++i)
        r.push_back(null_at(i, nulls) ? Row() : Row(POOL[(i * 3 + 1) % 5]));
    return r;
}

Series strings_of(const Rows& rows) {
    std::vector<std::int32_t> off{0};
    std::string data;
    std::vector<std::uint8_t> valid((rows.size() + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i]) {
            data += *rows[i];
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        } else {
            any_null = true;
        }
        off.push_back(static_cast<std::int32_t>(data.size()));
    }
    return Series{dftu_series_new_string(
        static_cast<dftu_dtype>(TypeId::String), off.data(), data.data(),
        static_cast<std::int64_t>(rows.size()),
        any_null ? valid.data() : nullptr)};
}

Series view_of(const Rows& rows) {
    Series flat = strings_of(rows);
    ViewBuilder b;
    b.append_column(*flat.handle());
    return b.finish(TypeId::String, false);
}

enum class Kind {
    Int64,
    Float64,
    Bool,
    StringFlat,
    StringView,
    StringDict,
    StringMixed
};

// Rows [from, to) of the column `kind`; a mixed string column takes its layout
// from `piece`.
Series piece_of(Kind kind, std::int64_t from, std::int64_t to, bool nulls,
                int piece) {
    const std::int64_t n = to - from;
    std::vector<std::uint8_t> valid((n + 7) / 8, 0);
    for (std::int64_t i = 0; i < n; ++i)
        if (!null_at(from + i, nulls))
            valid[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    const std::uint8_t* v = nulls ? valid.data() : nullptr;
    switch (kind) {
        case Kind::Int64: {
            std::vector<std::int64_t> d;
            for (std::int64_t i = from; i < to; ++i) d.push_back(i * 37 - 100);
            return Series::flat_i64(d.data(), n, v);
        }
        case Kind::Float64: {
            std::vector<double> d;
            for (std::int64_t i = from; i < to; ++i)
                d.push_back(static_cast<double>(i) * 1.5);
            return Series::flat_f64(d.data(), n, v);
        }
        case Kind::Bool: {
            std::vector<std::uint8_t> bits((n + 7) / 8, 0);
            for (std::int64_t i = 0; i < n; ++i)
                if ((from + i) % 3 != 1)
                    bits[static_cast<std::size_t>(i >> 3)] |= 1u << (i & 7);
            return Series::flat(TypeId::Bool, bits.data(), n, v);
        }
        default:
            break;
    }
    const Rows rows = rows_for(from, to, nulls);
    if (kind == Kind::StringView ||
        (kind == Kind::StringMixed && piece % 3 == 1))
        return view_of(rows);
    if (kind == Kind::StringDict ||
        (kind == Kind::StringMixed && piece % 3 == 2))
        return strings_of(rows).dictionary_encode();
    return strings_of(rows);
}

struct Pair {
    Series chunked;
    Series joined;
};

Pair build(Kind kind, bool nulls) {
    std::vector<std::shared_ptr<dftu_series>> chunks;
    for (int k = 0; k < 4; ++k) {
        Series p = piece_of(kind, CUTS[k], CUTS[k + 1], nulls, k);
        chunks.push_back(std::make_shared<dftu_series>(*p.handle()));
    }
    Pair out{Series{make_chunked(std::move(chunks))},
             piece_of(kind, 0, N, nulls, 0)};
    return out;
}

std::string repr(const Series& in, std::int64_t i) {
    if (in.is_null(i)) return "<null>";
    switch (in.type()) {
        case TypeId::Int64:
            return std::to_string(in.data<std::int64_t>()[i]);
        case TypeId::Float64:
            return std::to_string(in.data<double>()[i]);
        case TypeId::Bool:
            return ((in.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1) ? "T"
                                                                      : "F";
        default:
            return std::string(in.string_at(i));
    }
}

std::vector<std::string> flat_reprs(const Series& in) {
    Series flat = in.is_flat() ? in.share() : in.materialize();
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < flat.length(); ++i)
        out.push_back(repr(flat, i));
    return out;
}

const Kind KINDS[] = {Kind::Int64,      Kind::Float64,    Kind::Bool,
                      Kind::StringFlat, Kind::StringView, Kind::StringDict,
                      Kind::StringMixed};

DataFrame run(dftracer::utils::coro::CoroTask<DataFrame> t) {
    return dftracer::utils::default_runtime().submit(std::move(t)).get();
}

class PartsCursor : public Cursor {
   public:
    explicit PartsCursor(std::shared_ptr<const std::vector<DataFrame>> parts)
        : parts_(std::move(parts)) {}

    dftracer::utils::coro::CoroTask<std::optional<Morsel>> next(
        std::int64_t) override {
        if (next_ >= parts_->size()) co_return std::nullopt;
        const DataFrame& df = (*parts_)[next_++];
        Morsel m;
        m.rows = df.num_rows();
        auto& dyn = m.dyn_state();
        dyn.intern = intern_;
        for (std::size_t c = 0; c < df.columns.size(); ++c) {
            dyn.name_ids.push_back(intern_->get_or_insert(df.names[c]));
            m.columns.push_back(df.columns[c].share());
        }
        co_return m;
    }

   private:
    std::shared_ptr<const std::vector<DataFrame>> parts_;
    std::shared_ptr<dftracer::utils::StringIntern> intern_ =
        std::make_shared<dftracer::utils::StringIntern>();
    std::size_t next_ = 0;
};

class PartsSource : public Source {
   public:
    explicit PartsSource(std::vector<DataFrame> parts)
        : parts_(std::make_shared<const std::vector<DataFrame>>(
              std::move(parts))) {}

    Schema schema() const override { return Schema{}; }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        r.cursor = std::make_unique<PartsCursor>(parts_);
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

   private:
    std::shared_ptr<const std::vector<DataFrame>> parts_;
};

}  // namespace

TEST_SUITE("chunked columns") {
    TEST_CASE(
        "per-row readers, null count and materialize match the joined column") {
        for (Kind kind : KINDS)
            for (bool nulls : {false, true}) {
                CAPTURE(static_cast<int>(kind));
                CAPTURE(nulls);
                Pair p = build(kind, nulls);
                REQUIRE(p.chunked.handle()->is_chunked());
                CHECK(p.chunked.length() == N);
                CHECK(p.chunked.null_count() == p.joined.null_count());
                for (std::int64_t i = 0; i < N; ++i) {
                    REQUIRE(p.chunked.is_null(i) == p.joined.is_null(i));
                    if (kind >= Kind::StringFlat && !p.joined.is_null(i))
                        CHECK(p.chunked.string_at(i) == p.joined.string_at(i));
                }
                Series m = p.chunked.materialize();
                CHECK(m.is_flat());
                CHECK(m.null_count() == p.joined.null_count());
                CHECK(flat_reprs(m) == flat_reprs(p.joined));
            }
    }

    TEST_CASE("every slice range reads the rows of the joined column") {
        for (Kind kind : KINDS)
            for (bool nulls : {false, true}) {
                CAPTURE(static_cast<int>(kind));
                CAPTURE(nulls);
                Pair p = build(kind, nulls);
                const std::vector<std::string> all = flat_reprs(p.joined);
                for (std::int64_t off = 0; off <= N; ++off)
                    for (std::int64_t len = 0; off + len <= N; ++len) {
                        Series s{
                            dftu_series_slice(p.chunked.handle(), off, len)};
                        REQUIRE(s.valid());
                        REQUIRE(s.length() == len);
                        const std::vector<std::string> want(
                            all.begin() + off, all.begin() + off + len);
                        REQUIRE(flat_reprs(s) == want);
                    }
            }
    }

    TEST_CASE("buffer_bytes counts a shared buffer once") {
        Series base = view_of(rows_for(0, N, false));
        std::vector<std::shared_ptr<dftu_series>> chunks;
        for (int k = 0; k < 4; ++k)
            chunks.emplace_back(dftu_series_slice(base.handle(), CUTS[k],
                                                  CUTS[k + 1] - CUTS[k]));
        Series chunked{make_chunked(std::move(chunks))};
        const std::int64_t blobs = dftu_series_buffer_bytes(base.handle()) -
                                   static_cast<std::int64_t>(N) * 16;
        constexpr std::int64_t STARTS = 5 * sizeof(std::int64_t);
        CHECK(dftu_series_buffer_bytes(chunked.handle()) ==
              static_cast<std::int64_t>(N) * 16 + blobs + STARTS);
    }

    TEST_CASE("a chunked column of flat chunks counts every chunk") {
        Pair p = build(Kind::Int64, false);
        CHECK(dftu_series_buffer_bytes(p.chunked.handle()) >=
              dftu_series_buffer_bytes(p.joined.handle()));
    }

    TEST_CASE(
        "a streamed collect returns chunked columns equal to the joined "
        "frame") {
        DataFrame df;
        for (Kind kind :
             {Kind::Int64, Kind::Float64, Kind::Bool, Kind::StringFlat}) {
            df.names.push_back("c" + std::to_string(static_cast<int>(kind)));
            df.columns.push_back(piece_of(kind, 0, N, true, 0));
        }
        DataFrame got = run(df.lazy().collect(6));
        REQUIRE(got.names == df.names);
        for (std::size_t c = 0; c < df.columns.size(); ++c) {
            CAPTURE(df.names[c]);
            CHECK(got.columns[c].encoding() == Encoding::Chunked);
            CHECK(got.columns[c].null_count() == df.columns[c].null_count());
            CHECK(flat_reprs(got.columns[c]) == flat_reprs(df.columns[c]));
        }
    }

    TEST_CASE("a collect of parts whose column types differ") {
        DataFrame a, b, c;
        a.names = b.names = c.names = {"v", "s"};
        a.columns.push_back(piece_of(Kind::Int64, 0, 10, false, 0));
        b.columns.push_back(piece_of(Kind::Float64, 0, 10, false, 0));
        c.columns.push_back(piece_of(Kind::Int64, 10, 20, true, 0));
        a.columns.push_back(piece_of(Kind::StringFlat, 0, 10, true, 0));
        b.columns.push_back(piece_of(Kind::StringView, 10, 20, false, 0));
        c.columns.push_back(piece_of(Kind::StringDict, 20, 30, true, 0));
        const DataFrame eager = concat({&a, &b, &c}, ConcatHow::Diagonal);
        std::vector<DataFrame> parts;
        parts.push_back(std::move(a));
        parts.push_back(std::move(b));
        parts.push_back(std::move(c));
        auto source = std::make_shared<PartsSource>(std::move(parts));
        const DataFrame got = run(LazyFrame::scan(source).collect());
        REQUIRE(got.names == eager.names);
        for (std::size_t k = 0; k < eager.columns.size(); ++k) {
            CAPTURE(eager.names[k]);
            CHECK(got.columns[k].type() == eager.columns[k].type());
            CHECK(got.columns[k].null_count() == eager.columns[k].null_count());
            CHECK(flat_reprs(got.columns[k]) == flat_reprs(eager.columns[k]));
        }
    }

    TEST_CASE("kernels given a chunked column match the joined column") {
        for (Kind kind : KINDS)
            for (bool nulls : {false, true}) {
                CAPTURE(static_cast<int>(kind));
                CAPTURE(nulls);
                Pair p = build(kind, nulls);
                const Series& c = p.chunked;
                const Series& j = p.joined;
                const bool text = kind >= Kind::StringFlat;
                CHECK(flat_reprs(c.null_mask()) == flat_reprs(j.null_mask()));
                CHECK(flat_reprs(c.valid_mask()) == flat_reprs(j.valid_mask()));

                if (text) {
                    CHECK(flat_reprs(c.str_eq("read")) ==
                          flat_reprs(j.str_eq("read")));
                    CHECK(flat_reprs(c.str_len_bytes()) ==
                          flat_reprs(j.str_len_bytes()));
                    ViewBuilder b;
                    b.append_column(*c.handle());
                    CHECK(flat_reprs(b.finish(TypeId::String, false)) ==
                          flat_reprs(j));
                }

                if (kind != Kind::StringMixed) {
                    CHECK(flat_reprs(argsort(c, false)) ==
                          flat_reprs(argsort(j, false)));
                    CHECK(flat_reprs(argsort(c, true)) ==
                          flat_reprs(argsort(j, true)));
                    CHECK(flat_reprs(topk_indices(c, 5, true)) ==
                          flat_reprs(topk_indices(j, 5, true)));
                }

                for (std::int64_t a = 0; a < N; ++a) {
                    if (c.is_null(a)) {
                        std::string key_c, key_j;
                        append_cell(key_c, c, a);
                        append_cell(key_j, j, a);
                        CHECK(key_c == key_j);
                        continue;
                    }
                    std::string key_c, key_j;
                    append_cell(key_c, c, a);
                    append_cell(key_j, j, a);
                    CHECK(key_c == key_j);
                    CHECK(cell_to_string(c, a) == cell_to_string(j, a));
                    if (kind == Kind::Int64)
                        CHECK(read_i64(c, a) == read_i64(j, a));
                    if (kind == Kind::Float64)
                        CHECK(read_f64(c, a) == read_f64(j, a));
                    if (text) CHECK(read_bytes(c, a) == read_bytes(j, a));
                    if (kind == Kind::StringMixed) continue;
                    const Series joined = join_chunks(c);
                    for (std::int64_t b = 0; b < N; ++b)
                        if (!c.is_null(b))
                            REQUIRE(compare_rows(joined, a, b) ==
                                    compare_rows(j, a, b));
                }

                if (kind == Kind::Int64 || kind == Kind::Float64) {
                    const FieldStat fc = field_stat_reduce(c);
                    const FieldStat fj = field_stat_reduce(j);
                    CHECK(fc.n == fj.n);
                    CHECK(fc.sum == fj.sum);
                    CHECK(fc.min == fj.min);
                    CHECK(fc.max == fj.max);

                    std::vector<std::int64_t> g;
                    for (std::int64_t i = 0; i < N; ++i) g.push_back(i % 3);
                    const Series keys = Series::flat_i64(g.data(), N);
                    auto grouped = [&](const Series& values) {
                        dftu_series* k = nullptr;
                        dftu_series* vals[5] = {};
                        const std::int32_t n = dftu_series_group_by(
                            keys.handle(), values.handle(),
                            DFTU_REDUCE_SUM | DFTU_REDUCE_MIN | DFTU_REDUCE_MAX,
                            &k, vals, 5);
                        std::vector<std::string> out;
                        Series ks{k};
                        if (k)
                            for (std::int64_t i = 0; i < ks.length(); ++i)
                                out.push_back(repr(ks, i));
                        for (std::int32_t v = 0; v < n; ++v) {
                            Series vs{vals[v]};
                            for (std::int64_t i = 0; i < vs.length(); ++i)
                                out.push_back(repr(vs, i));
                        }
                        out.push_back(std::to_string(n));
                        return out;
                    };
                    const auto want = grouped(j);
                    CHECK(want.size() > 1);
                    CHECK(grouped(c) == want);
                }

                if (kind == Kind::Bool) {
                    const Series a = piece_of(Kind::Int64, 0, N, false, 0);
                    const Series b = piece_of(Kind::Float64, 0, N, false, 0);
                    CHECK(flat_reprs(a.filter(c)) == flat_reprs(a.filter(j)));
                    const Series b64 = b.cast(TypeId::Int64);
                    CHECK(flat_reprs(a.where(c, b64)) ==
                          flat_reprs(a.where(j, b64)));
                }
            }
    }

    TEST_CASE("take gathers per chunk and matches the joined column") {
        const std::vector<std::vector<std::int64_t>> index_sets = {
            {},
            {0, 5, 12},
            {0, 1, 5, 13, 14, 20, 29, 35, 39},
            {3, 3, 13, 13, 14, 14, 39, 39},
            {-1, 0, 13, -1, 14, 29, -1},
            {-1, -1, 20, 30},
            {39, 0, 20, 13, -1, 14},
            {35, -1, 2},
            {-1, -1},
            {39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26,
             25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12,
             11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1,  0}};
        for (Kind kind : KINDS)
            for (bool nulls : {false, true}) {
                Pair p = build(kind, nulls);
                for (const auto& idx : index_sets) {
                    CAPTURE(static_cast<int>(kind));
                    CAPTURE(nulls);
                    CAPTURE(idx);
                    const Series got = p.chunked.take(idx);
                    const Series want = p.joined.take(idx);
                    REQUIRE(got.valid());
                    CHECK(got.length() == want.length());
                    CHECK(got.null_count() == want.null_count());
                    CHECK(flat_reprs(got) == flat_reprs(want));
                }
                const Series spanning =
                    p.chunked.take({0, 1, 5, 13, 14, 20, 29, 35, 39});
                CHECK(spanning.handle()->is_chunked());
                const Series one_chunk = p.chunked.take({0, 5, 12});
                CHECK_FALSE(one_chunk.handle()->is_chunked());
            }
    }

    TEST_CASE("row-wise kernels on a chunked column match the joined column") {
        const Series values =
            strings_of({Row("read"), Row("a name of exactly 16")});
        std::vector<std::int64_t> ints{-100, -63, 1000};
        const Series int_values = Series::flat_i64(ints.data(), 3);
        for (Kind kind : KINDS)
            for (bool nulls : {false, true}) {
                CAPTURE(static_cast<int>(kind));
                CAPTURE(nulls);
                Pair p = build(kind, nulls);
                const Series& c = p.chunked;
                const Series& j = p.joined;
                auto same = [&](const Series& a, const Series& b) {
                    REQUIRE(a.valid());
                    CHECK(a.null_count() == b.null_count());
                    CHECK(flat_reprs(a) == flat_reprs(b));
                };
                if (kind >= Kind::StringFlat) {
                    same(c.str_eq("read"), j.str_eq("read"));
                    same(c.str_contains("a"), j.str_contains("a"));
                    same(c.str_starts_with("wr"), j.str_starts_with("wr"));
                    same(c.str_ends_with("e"), j.str_ends_with("e"));
                    same(c.str_matches("w.*"), j.str_matches("w.*"));
                    same(c.str_search("name"), j.str_search("name"));
                    same(c.str_like("re%"), j.str_like("re%"));
                    same(c.str_len_bytes(), j.str_len_bytes());
                    same(c.str_len_chars(), j.str_len_chars());
                    same(c.str_find("a"), j.str_find("a"));
                    same(c.is_in(values), j.is_in(values));
                    for (dftu_cmp_op op : {DFTU_CMP_LT, DFTU_CMP_NE})
                        same(Series{dftu_series_compare(c.handle(), op,
                                                        scalar_str("read"))},
                             Series{dftu_series_compare(j.handle(), op,
                                                        scalar_str("read"))});
                } else if (kind == Kind::Int64) {
                    same(c.gt(std::int64_t{300}), j.gt(std::int64_t{300}));
                    same(c.eq(std::int64_t{-63}), j.eq(std::int64_t{-63}));
                    same(c.is_in(int_values), j.is_in(int_values));
                } else if (kind == Kind::Float64) {
                    same(c.le(30.0), j.le(30.0));
                }
                same(c.null_mask(), j.null_mask());
                same(c.valid_mask(), j.valid_mask());
            }
    }

    TEST_CASE("a chunked filter mask selects the rows of the joined mask") {
        for (bool nulls : {false, true}) {
            Pair strings = build(Kind::StringMixed, nulls);
            Pair ints = build(Kind::Int64, nulls);
            const Series chunked_mask = strings.chunked.str_starts_with("re");
            const Series joined_mask = strings.joined.str_starts_with("re");
            CHECK(flat_reprs(ints.chunked.filter(chunked_mask)) ==
                  flat_reprs(ints.joined.filter(joined_mask)));
            CHECK(flat_reprs(strings.chunked.filter(chunked_mask)) ==
                  flat_reprs(strings.joined.filter(joined_mask)));
        }
    }

    TEST_CASE("group-by and sort of chunked frames match the joined frames") {
        for (Kind kind : KINDS)
            for (bool nulls : {false, true}) {
                CAPTURE(static_cast<int>(kind));
                CAPTURE(nulls);
                Pair key = build(kind, nulls);
                Pair ints = build(Kind::Int64, nulls);
                Pair reals = build(Kind::Float64, nulls);
                Pair text = build(Kind::StringMixed, nulls);
                DataFrame chunked, joined;
                chunked.names = joined.names = {"key", "i", "f", "s"};
                for (Pair* col : {&key, &ints, &reals, &text}) {
                    chunked.columns.push_back(col->chunked.share());
                    joined.columns.push_back(col->joined.share());
                }
                auto same = [&](const DataFrame& a, const DataFrame& b) {
                    REQUIRE(a.names == b.names);
                    for (std::size_t k = 0; k < a.columns.size(); ++k) {
                        CAPTURE(a.names[k]);
                        CHECK(a.columns[k].null_count() ==
                              b.columns[k].null_count());
                        CHECK(flat_reprs(a.columns[k]) ==
                              flat_reprs(b.columns[k]));
                    }
                };

                same(chunked.sort_by("key"), joined.sort_by("key"));
                same(chunked.sort_by("key", true), joined.sort_by("key", true));
                same(chunked.sort_by("s"), joined.sort_by("s"));
                same(chunked.sort_by("f", true), joined.sort_by("f", true));
                same(chunked.sort_by_multi({"key", "i"}, {false, true}),
                     joined.sort_by_multi({"key", "i"}, {false, true}));

                if (kind == Kind::Bool) continue;
                std::vector<GroupAgg> aggs;
                for (Agg op :
                     {Agg::Sum, Agg::Min, Agg::Max, Agg::Count, Agg::CountValid,
                      Agg::Mean, Agg::Var, Agg::Std, Agg::First, Agg::Last})
                    for (const char* col : {"i", "f"}) {
                        GroupAgg a;
                        a.op = op;
                        a.column = col;
                        a.out = std::string(col) + "_" + to_string(op);
                        aggs.push_back(std::move(a));
                    }
                const DataFrame want = joined.group_by("key", aggs);
                REQUIRE(want.num_rows() > 0);
                same(chunked.group_by("key", aggs), want);
                same(chunked.group_by(std::vector<std::string>{"key", "s"},
                                      aggs),
                     joined.group_by(std::vector<std::string>{"key", "s"},
                                     aggs));
            }
    }
}
