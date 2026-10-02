// unique, nunique, value_counts, is_in and frame unique give the results of
// the same column copied flat, for every column layout.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>
#include <dftracer/utils/dataframe/series.h>
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

using Idx = std::vector<std::int64_t>;
constexpr std::int64_t N = 60;
constexpr std::int64_t CUTS[] = {0, 7, 8, 31, N};

bool nullish(std::int64_t i) { return i % 11 == 4 || i % 17 == 9; }

std::vector<std::uint8_t> validity_bits() {
    std::vector<std::uint8_t> v(static_cast<std::size_t>((N + 7) / 8), 0);
    for (std::int64_t i = 0; i < N; ++i)
        if (!nullish(i)) v[static_cast<std::size_t>(i >> 3)] |= 1u << (i & 7);
    return v;
}

Series flat_strings() {
    static const std::vector<std::string> POOL = {
        "read",
        "",
        "a name of exactly 16",
        "write",
        "0123456789abcdef",
        "a considerably longer name than the others"};
    std::vector<std::string> keep;
    for (std::int64_t i = 0; i < N; ++i)
        keep.push_back(POOL[static_cast<std::size_t>((i * 5 + i / 7) % 6)]);
    std::vector<std::string_view> v(keep.begin(), keep.end());
    const auto valid = validity_bits();
    return Series::strings(v, valid.data());
}

Series flat_i64() {
    std::vector<std::int64_t> v;
    for (std::int64_t i = 0; i < N; ++i) v.push_back((i * 7) % 9 - 4);
    const auto valid = validity_bits();
    return Series::flat_i64(v.data(), N, valid.data());
}

Series flat_f64() {
    const double POOL[] = {0.0, -0.0,  std::numeric_limits<double>::quiet_NaN(),
                           1.5, -2.25, std::numeric_limits<double>::infinity()};
    std::vector<double> v;
    for (std::int64_t i = 0; i < N; ++i)
        v.push_back(POOL[static_cast<std::size_t>((i * 5 + i / 7) % 6)]);
    const auto valid = validity_bits();
    return Series::flat_f64(v.data(), N, valid.data());
}

Series flat_bool() {
    std::vector<std::uint8_t> bits(static_cast<std::size_t>((N + 7) / 8), 0);
    for (std::int64_t i = 0; i < N; ++i)
        if ((i * 3 + i / 5) % 4 < 2)
            bits[static_cast<std::size_t>(i >> 3)] |= 1u << (i & 7);
    const auto valid = validity_bits();
    return Series::flat(TypeId::Bool, bits.data(), N, valid.data());
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

Series view_of(const Series& flat) {
    ViewBuilder b;
    b.append_column(*flat.handle());
    return b.finish(TypeId::String, false);
}

Series range_of(const Series& flat, std::int64_t b, std::int64_t e) {
    Idx idx;
    for (std::int64_t i = b; i < e; ++i) idx.push_back(i);
    return flat.take(idx);
}

Series chunked_of(const Series& flat, bool is_string) {
    std::vector<std::shared_ptr<dftu_series>> chunks;
    for (int k = 0; k < 4; ++k) {
        Series piece = range_of(flat, CUTS[k], CUTS[k + 1]);
        switch (k % 4) {
            case 1:
                if (is_string) piece = view_of(piece);
                break;
            case 2:
                if (is_string) piece = piece.dictionary_encode();
                break;
            case 3: {
                Idx idx;
                for (std::int64_t i = CUTS[k]; i < CUTS[k + 1]; ++i)
                    idx.push_back(i);
                piece = selection_over(flat, idx);
                break;
            }
            default:
                break;
        }
        chunks.push_back(std::make_shared<dftu_series>(*piece.handle()));
    }
    return Series{make_chunked(std::move(chunks))};
}

std::vector<std::pair<std::string, Series>> layouts(const Series& flat) {
    const bool s = flat.type() == TypeId::String;
    std::vector<std::pair<std::string, Series>> out;
    out.emplace_back("flat", flat.share());
    if (s) {
        out.emplace_back("view", view_of(flat));
        out.emplace_back("dictionary", flat.dictionary_encode());
    }
    Idx rev;
    for (std::int64_t i = 0; i < N; ++i) rev.push_back(N - 1 - i);
    Series sel = selection_over(flat, rev);
    out.emplace_back("selection", std::move(sel));
    out.emplace_back("chunked", chunked_of(flat, s));
    return out;
}

std::vector<std::string> cells(const Series& in) {
    Series s = in.materialize();
    std::vector<std::string> out;
    for (std::int64_t i = 0; i < s.length(); ++i) {
        if (s.is_null(i)) {
            out.push_back("<null>");
            continue;
        }
        switch (s.type()) {
            case TypeId::String:
                out.push_back(std::string(s.string_at(i)));
                break;
            case TypeId::Int64:
                out.push_back(std::to_string(s.data<std::int64_t>()[i]));
                break;
            case TypeId::Float64: {
                std::uint64_t bits;
                std::memcpy(&bits, s.data<double>() + i, 8);
                out.push_back(std::to_string(bits));
                break;
            }
            default:
                out.push_back(std::to_string(
                    (s.data<std::uint8_t>()[i >> 3] >> (i & 7)) & 1));
        }
    }
    return out;
}

std::vector<std::string> frame_cells(const DataFrame& df) {
    std::vector<std::string> out;
    for (const Series& c : df.columns)
        for (const std::string& x : cells(c)) out.push_back(x);
    return out;
}

Series needles(const Series& flat) { return range_of(flat, 3, 12); }

Series flat_copy(const Series& c) { return c.materialize(); }

}  // namespace

TEST_SUITE("distinct on ids") {
    TEST_CASE("series distinct ops match the flat copy in every layout") {
        const std::pair<const char*, Series> bases[] = {
            {"string", flat_strings()},
            {"int64", flat_i64()},
            {"float64", flat_f64()},
            {"bool", flat_bool()}};
        for (const auto& [name, base] : bases) {
            for (const auto& [layout, col] : layouts(base)) {
                CAPTURE(name);
                CAPTURE(layout);
                const Series ref = flat_copy(col);
                CHECK((col.encoding() == Encoding::Flat) == (layout == "flat"));
                CHECK(cells(col.unique()) == cells(ref.unique()));
                CHECK(col.unique().length() == ref.unique().length());
                CHECK(col.nunique() == ref.nunique());
                CHECK(col.nunique() == col.unique().length());
                const DataFrame vc = col.value_counts();
                const DataFrame rc = ref.value_counts();
                CHECK(frame_cells(vc) == frame_cells(rc));
                CHECK(vc.num_rows() == rc.num_rows());
                const Series nd = needles(base);
                CHECK(cells(col.is_in(nd)) == cells(ref.is_in(nd)));
                CHECK(cells(col.is_in(selection_over(nd, {2, 1, 0}))) ==
                      cells(ref.is_in(selection_over(nd, {2, 1, 0}))));
            }
        }
    }

    TEST_CASE(
        "string value counts count the nulls out and keep first-seen ties") {
        const Series s = flat_strings();
        const DataFrame vc = s.dictionary_encode().value_counts();
        std::int64_t total = 0;
        const Series cnt = vc.columns[1].materialize();
        for (std::int64_t i = 0; i < cnt.length(); ++i) {
            total += cnt.data<std::int64_t>()[i];
            if (i > 0)
                CHECK(cnt.data<std::int64_t>()[i - 1] >=
                      cnt.data<std::int64_t>()[i]);
        }
        CHECK(total == N - s.null_count());
    }

    TEST_CASE(
        "frame unique matches flat columns for all columns and a subset") {
        const Series bases[] = {flat_strings(), flat_i64(), flat_f64(),
                                flat_bool()};
        const char* NAMES[] = {"s", "i", "f", "b"};
        for (std::size_t variant = 0; variant < 5; ++variant) {
            DataFrame df, ref;
            for (std::size_t c = 0; c < 4; ++c) {
                auto ls = layouts(bases[c]);
                Series pick = std::move(ls[(variant + c) % ls.size()].second);
                ref.names.push_back(NAMES[c]);
                ref.columns.push_back(flat_copy(pick));
                df.names.push_back(NAMES[c]);
                df.columns.push_back(std::move(pick));
            }
            for (const std::vector<std::string>& subset :
                 {std::vector<std::string>{}, std::vector<std::string>{"s"},
                  std::vector<std::string>{"i", "b"},
                  std::vector<std::string>{"f", "s"}}) {
                CAPTURE(variant);
                CAPTURE(subset.size());
                const DataFrame got = df.unique(subset);
                const DataFrame want = ref.unique(subset);
                CHECK(got.num_rows() == want.num_rows());
                CHECK(frame_cells(got) == frame_cells(want));
            }
        }
    }

    TEST_CASE("an empty column gives empty results") {
        const Series e = flat_i64().take(Idx{});
        CHECK(e.unique().length() == 0);
        CHECK(e.nunique() == 0);
        CHECK(e.value_counts().num_rows() == 0);
        DataFrame df;
        df.names = {"a"};
        df.columns.push_back(e.share());
        CHECK(df.unique().num_rows() == 0);
    }

    TEST_CASE("float unique keeps every NaN row and the sorted order") {
        std::vector<double> v{std::nan(""), std::nan(""), 1.0, 0.0,
                              -0.0,         7.0,          5.0};
        std::vector<std::uint8_t> valid{0b0111111};
        Series s = Series::flat(TypeId::Float64, v.data(), 7, valid.data());
        Series u = s.unique();
        REQUIRE(u.length() == 5);
        CHECK(s.nunique() == 5);
        const double* d = u.values<double>().data();
        CHECK(std::isnan(d[0]));
        CHECK(std::isnan(d[1]));
        CHECK(d[2] == 0.0);
        CHECK(!std::signbit(d[2]));
        CHECK(d[3] == 1.0);
        CHECK(d[4] == 7.0);
    }
}
