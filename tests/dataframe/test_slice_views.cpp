// A column or frame slice is a view with the values of a gather: same values,
// nulls, null count, type and layout, sharing the value storage and valid after
// the base is released.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

constexpr std::int64_t N = 40;

using Row = std::optional<std::string>;
using Rows = std::vector<Row>;
using Idx = std::vector<std::int64_t>;

const char* const POOL[] = {"read",
                            "write",
                            "a name of exactly 16",
                            "",
                            "0123456789abcdef",
                            "a considerably longer name than the others",
                            "x"};

bool null_at(std::int64_t i, bool nulls) { return nulls && i % 5 == 2; }

Rows rows_for(bool nulls) {
    Rows r;
    for (std::int64_t i = 0; i < N; ++i)
        r.push_back(null_at(i, nulls) ? Row() : Row(POOL[(i * 3 + 1) % 7]));
    return r;
}

std::vector<std::uint8_t> bitmap(bool nulls) {
    std::vector<std::uint8_t> v((N + 7) / 8, 0);
    for (std::int64_t i = 0; i < N; ++i)
        if (!null_at(i, nulls))
            v[static_cast<std::size_t>(i >> 3)] |=
                static_cast<std::uint8_t>(1u << (i & 7));
    return v;
}

Series strings_of(const Rows& rows, TypeId type) {
    std::vector<std::int32_t> off{0};
    std::string data;
    for (const Row& r : rows) {
        if (r) data += *r;
        off.push_back(static_cast<std::int32_t>(data.size()));
    }
    std::vector<std::uint8_t> valid((rows.size() + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i])
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            any_null = true;
    }
    return Series{dftu_series_new_string(static_cast<dftu_dtype>(type),
                                         off.data(), data.data(),
                                         static_cast<std::int64_t>(rows.size()),
                                         any_null ? valid.data() : nullptr)};
}

Series large_of(const Rows& rows) {
    Series s = strings_of(rows, TypeId::String);
    auto* o = new dftu_series(*s.handle());
    o->type = TypeId::LargeString;
    o->offsets = Buffer::allocate(static_cast<std::size_t>(o->length + 1) *
                                  sizeof(std::int64_t));
    const std::int32_t* src = s.offsets();
    auto* dst = reinterpret_cast<std::int64_t*>(o->offsets->data());
    for (std::int64_t i = 0; i <= o->length; ++i) dst[i] = src[i];
    return Series{o};
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

Series int64_of(bool nulls) {
    std::vector<std::int64_t> v;
    for (std::int64_t i = 0; i < N; ++i) v.push_back(i * 37 - 100);
    auto valid = bitmap(nulls);
    return Series::flat_i64(v.data(), N, nulls ? valid.data() : nullptr);
}

Series float64_of(bool nulls) {
    std::vector<double> v;
    for (std::int64_t i = 0; i < N; ++i)
        v.push_back(static_cast<double>(i) * 1.5);
    auto valid = bitmap(nulls);
    return Series::flat_f64(v.data(), N, nulls ? valid.data() : nullptr);
}

Series bool_of(bool nulls) {
    std::vector<std::uint8_t> bits((N + 7) / 8, 0);
    for (std::int64_t i = 0; i < N; ++i)
        if (i % 3 != 1) bits[static_cast<std::size_t>(i >> 3)] |= 1u << (i & 7);
    auto valid = bitmap(nulls);
    return Series::flat(TypeId::Bool, bits.data(), N,
                        nulls ? valid.data() : nullptr);
}

Series view_of(const Rows& rows) {
    Series flat = strings_of(rows, TypeId::String);
    ViewBuilder b;
    b.append_column(*flat.handle());
    return b.finish(TypeId::String, false);
}

Idx scatter() {
    Idx idx;
    for (std::int64_t i = 0; i < N; ++i) idx.push_back((i * 7 + 3) % N);
    return idx;
}

struct Case {
    std::string name;
    Series col;
};

std::vector<Case> cases() {
    std::vector<Case> out;
    for (bool nulls : {false, true}) {
        const std::string suffix = nulls ? " nulls" : "";
        const Rows rows = rows_for(nulls);
        out.push_back({"int64" + suffix, int64_of(nulls)});
        out.push_back({"float64" + suffix, float64_of(nulls)});
        out.push_back({"bool" + suffix, bool_of(nulls)});
        out.push_back({"string" + suffix, strings_of(rows, TypeId::String)});
        out.push_back({"large string" + suffix, large_of(rows)});
        out.push_back({"binary" + suffix, strings_of(rows, TypeId::Binary)});
        out.push_back(
            {"json" + suffix, strings_of(rows, TypeId::String).as_json()});
        out.push_back({"view" + suffix, view_of(rows)});
        out.push_back({"dictionary" + suffix,
                       strings_of(rows, TypeId::String).dictionary_encode()});
        out.push_back(
            {"selection string" + suffix,
             selection_over(strings_of(rows, TypeId::String), scatter())});
        out.push_back({"selection int64" + suffix,
                       selection_over(int64_of(nulls), scatter())});
    }
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

std::vector<std::string> reprs(const Series& in) {
    Series flat = in.is_flat() ? in.share() : in.materialize();
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < flat.length(); ++i)
        out.push_back(repr(flat, i));
    return out;
}

Idx range(std::int64_t offset, std::int64_t len) {
    Idx idx;
    for (std::int64_t i = 0; i < len; ++i) idx.push_back(offset + i);
    return idx;
}

void check_equal_to_gather(const Series& base, const Series& got,
                           std::int64_t offset, std::int64_t len) {
    REQUIRE(got.valid());
    CHECK(got.length() == len);
    Series want = base.take(range(offset, len));
    CHECK(got.type() == want.type());
    CHECK(dftu_series_is_json(got.handle()) ==
          dftu_series_is_json(want.handle()));
    CHECK(got.null_count() == want.null_count());
    CHECK(got.encoding() == base.encoding());
    CHECK(reprs(got) == reprs(want));
}

std::int64_t length_for(std::int64_t offset, int pick) {
    const std::int64_t lens[] = {0, 1, 7, 8, 9, N - offset};
    return lens[pick];
}

}  // namespace

TEST_SUITE("slice views") {
    TEST_CASE("a slice equals a gather of the same rows for every layout") {
        for (const Case& c : cases()) {
            CAPTURE(c.name);
            for (std::int64_t offset = 0; offset <= 17; ++offset)
                for (int pick = 0; pick < 6; ++pick) {
                    const std::int64_t len = length_for(offset, pick);
                    CAPTURE(offset);
                    CAPTURE(len);
                    Series got{dftu_series_slice(c.col.handle(), offset, len)};
                    check_equal_to_gather(c.col, got, offset, len);
                }
        }
    }

    TEST_CASE("a slice of a slice reads the same as one slice") {
        for (const Case& c : cases()) {
            CAPTURE(c.name);
            Series outer{dftu_series_slice(c.col.handle(), 5, 25)};
            Series inner{dftu_series_slice(outer.handle(), 3, 11)};
            check_equal_to_gather(c.col, inner, 8, 11);
        }
    }

    TEST_CASE("a slice reads after its base is freed") {
        for (const Case& c : cases()) {
            CAPTURE(c.name);
            auto base = std::make_unique<Series>(c.col.share());
            std::vector<std::string> want = reprs(c.col.take(range(9, 17)));
            Series got{dftu_series_slice(base->handle(), 9, 17)};
            base.reset();
            std::vector<std::string> before = reprs(got);
            CHECK(before == want);
        }
        std::vector<Case> all = cases();
        std::vector<std::vector<std::string>> want;
        std::vector<Series> slices;
        for (Case& c : all) {
            want.push_back(reprs(c.col.take(range(9, 17))));
            slices.emplace_back(dftu_series_slice(c.col.handle(), 9, 17));
        }
        all.clear();
        for (std::size_t i = 0; i < slices.size(); ++i)
            CHECK(reprs(slices[i]) == want[i]);
    }

    TEST_CASE("a string slice points into the base bytes") {
        for (TypeId type : {TypeId::String, TypeId::Binary}) {
            Series base = strings_of(rows_for(true), type);
            Series got{dftu_series_slice(base.handle(), 3, 7)};
            const char* lo = base.data<char>();
            const char* hi = lo + base.offsets()[N];
            const char* p = got.data<char>();
            CHECK(p >= lo);
            CHECK(p <= hi);
            CHECK(p == lo + base.offsets()[3]);
        }
    }

    TEST_CASE("view, dictionary and selection slices share their storage") {
        const Rows rows = rows_for(true);
        Series view = view_of(rows);
        Series v{dftu_series_slice(view.handle(), 4, 12)};
        CHECK(v.handle()->blobs.get() == view.handle()->blobs.get());

        Series dict = strings_of(rows, TypeId::String).dictionary_encode();
        Series d{dftu_series_slice(dict.handle(), 4, 12)};
        CHECK(d.handle()->child().get() == dict.handle()->child().get());

        Series sel =
            selection_over(strings_of(rows, TypeId::String), scatter());
        Series s{dftu_series_slice(sel.handle(), 4, 12)};
        CHECK(s.handle()->child().get() == sel.handle()->child().get());
    }

    TEST_CASE("a bool slice sums like the gathered column") {
        for (bool nulls : {false, true}) {
            Series base = bool_of(nulls);
            for (std::int64_t offset = 0; offset <= 17; ++offset)
                for (int pick = 0; pick < 6; ++pick) {
                    const std::int64_t len = length_for(offset, pick);
                    CAPTURE(offset);
                    CAPTURE(len);
                    Series got{dftu_series_slice(base.handle(), offset, len)};
                    Series want = base.take(range(offset, len));
                    CHECK(got.sum().i64() == want.sum().i64());
                }
        }
    }

    TEST_CASE("list and struct columns are refused") {
        std::vector<std::int64_t> v{1, 2, 3, 4};
        Series list = Series::list({0, 1, 3, 4}, Series::flat_i64(v.data(), 4));
        CHECK(dftu_series_slice(list.handle(), 0, 2) == nullptr);

        std::vector<Series> fields;
        fields.push_back(Series::flat_i64(v.data(), 4));
        Series st = Series::structs({"a"}, std::move(fields));
        CHECK(dftu_series_slice(st.handle(), 0, 2) == nullptr);
        CHECK(dftu_series_slice(nullptr, 0, 2) == nullptr);
    }

    TEST_CASE("frame slice, head and tail equal take for every layout") {
        DataFrame df;
        for (const Case& c : cases()) {
            df.names.push_back(c.name);
            df.columns.push_back(c.col.share());
        }
        std::vector<std::int32_t> off{0};
        std::vector<std::int64_t> vals;
        for (std::int64_t i = 0; i < N; ++i) {
            for (std::int64_t k = 0; k < i % 4; ++k) vals.push_back(i * 10 + k);
            off.push_back(static_cast<std::int32_t>(vals.size()));
        }
        df.names.push_back("list");
        df.columns.push_back(Series::list(
            off, Series::flat_i64(vals.data(),
                                  static_cast<std::int64_t>(vals.size()))));

        auto same = [&](const DataFrame& got, std::int64_t offset,
                        std::int64_t len) {
            DataFrame want = df.take(range(offset, len));
            REQUIRE(got.num_columns() == want.num_columns());
            CHECK(got.names == want.names);
            CHECK(got.num_rows() == len);
            for (std::size_t c = 0; c < got.num_columns(); ++c) {
                CAPTURE(got.names[c]);
                if (got.columns[c].type() == TypeId::List) {
                    CHECK(got.columns[c].length() == want.columns[c].length());
                    const std::int32_t* go = got.columns[c].offsets();
                    const std::int32_t* wo = want.columns[c].offsets();
                    for (std::int64_t i = 0; i <= len; ++i)
                        CHECK(go[i] - go[0] == wo[i] - wo[0]);
                    Series gv = got.columns[c].child(0);
                    Series wv = want.columns[c].child(0);
                    for (std::int64_t i = 0; i < go[len] - go[0]; ++i)
                        CHECK(gv.data<std::int64_t>()[go[0] + i] ==
                              wv.data<std::int64_t>()[wo[0] + i]);
                } else {
                    CHECK(reprs(got.columns[c]) == reprs(want.columns[c]));
                    CHECK(got.columns[c].null_count() ==
                          want.columns[c].null_count());
                }
            }
        };
        same(df.slice(6, 19), 6, 19);
        same(df.head(9), 0, 9);
        same(df.tail(11), N - 11, 11);
        same(df.slice(5, 0), 5, 0);
    }

    TEST_CASE("lazy filter and with_column over string layouts match eager") {
        const Rows rows = rows_for(true);
        DataFrame df;
        df.names = {"i", "flat", "view", "dict"};
        df.columns.push_back(int64_of(false));
        df.columns.push_back(strings_of(rows, TypeId::String));
        df.columns.push_back(view_of(rows));
        df.columns.push_back(
            strings_of(rows, TypeId::String).dictionary_encode());

        DataFrame lazy = dftracer::utils::default_runtime()
                             .submit(df.lazy()
                                         .filter(col(0) > std::int64_t{10})
                                         .with_column("j", col(0) + col(0))
                                         .collect(7))
                             .get();

        std::vector<const Series*> in;
        for (const Series& c : df.columns) in.push_back(&c);
        DataFrame kept = df.filter(eval(col(0) > std::int64_t{10}, in));
        std::vector<const Series*> kin;
        for (const Series& c : kept.columns) kin.push_back(&c);
        DataFrame want = kept.with_column("j", eval(col(0) + col(0), kin));

        REQUIRE(lazy.num_columns() == want.num_columns());
        CHECK(lazy.names == want.names);
        for (std::size_t c = 0; c < lazy.num_columns(); ++c) {
            CAPTURE(lazy.names[c]);
            CHECK(reprs(lazy.columns[c]) == reprs(want.columns[c]));
        }
    }

    TEST_CASE("eval of a string expression past one chunk equals the kernel") {
        constexpr std::int64_t M = 70001;
        Rows rows;
        for (std::int64_t i = 0; i < M; ++i)
            rows.push_back(i % 11 == 4 ? Row() : Row(POOL[(i * 5) % 7]));
        for (int layout = 0; layout < 3; ++layout) {
            Series s = strings_of(rows, TypeId::String);
            if (layout == 1) s = view_of(rows);
            if (layout == 2) s = s.dictionary_encode();
            std::vector<const Series*> in{&s};
            Series got = eval(
                expr_str_pred(StrPredOp::Contains, expr_col(0), "name"), in);
            Series want = s.str_contains("name");
            REQUIRE(got.length() == want.length());
            CHECK(got.null_count() == want.null_count());
            for (std::int64_t i = 0; i < M; ++i) {
                REQUIRE(got.is_null(i) == want.is_null(i));
                if (!got.is_null(i)) REQUIRE(repr(got, i) == repr(want, i));
            }
        }
    }
}
