// Group-by over string keys held as DICTIONARY or VIEW columns folds in place
// and gives what the flat keys give, one morsel at a time.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/view_builder.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

using Row = std::optional<std::string>;
enum class Layout { Flat, Dictionary, View };

const std::vector<Row> POOL = {
    Row("read"),
    Row("write"),
    Row("a name of exactly 16"),
    Row("sixteen bytes xx!"),
    Row(""),
    Row(),
    Row("0123456789abcdef"),
    Row("a considerably longer name than the others")};

struct Morsel {
    std::vector<Row> keys;
    std::vector<std::int64_t> other;
    std::vector<std::int64_t> vals;
};

Morsel make_morsel(std::size_t n, std::size_t rot, std::size_t pool) {
    Morsel m;
    for (std::size_t i = 0; i < n; ++i) {
        m.keys.push_back(POOL[(i * 3 + rot) % pool]);
        m.other.push_back(static_cast<std::int64_t>(i % 2));
        m.vals.push_back(static_cast<std::int64_t>(i + rot));
    }
    return m;
}

Series flat_keys(const std::vector<Row>& keys) {
    std::vector<std::string_view> v;
    std::vector<std::uint8_t> valid((keys.size() + 7) / 8, 0);
    bool any_null = false;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        v.emplace_back(keys[i] ? std::string_view(*keys[i])
                               : std::string_view());
        if (keys[i])
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        else
            any_null = true;
    }
    return Series::strings(v, any_null ? valid.data() : nullptr);
}

Series keys_as(const std::vector<Row>& keys, Layout layout) {
    Series flat = flat_keys(keys);
    switch (layout) {
        case Layout::Flat:
            return flat;
        case Layout::Dictionary:
            return flat.dictionary_encode();
        case Layout::View: {
            ViewBuilder b;
            b.append_column(*flat.handle());
            return b.finish(TypeId::String, false);
        }
    }
    return flat;
}

using Cells = std::vector<std::vector<std::string>>;

Cells cells_of(const DataFrame& df) {
    Cells out;
    for (const Series& c : df.columns) {
        std::vector<std::string> col;
        for (std::int64_t i = 0; i < c.length(); ++i) {
            if (c.is_null(i))
                col.push_back("<null>");
            else if (c.type() == TypeId::String)
                col.push_back(std::string(c.string_at(i)));
            else
                col.push_back(std::to_string(c.data<std::int64_t>()[i]));
        }
        out.push_back(std::move(col));
    }
    return out;
}

Cells fold(const std::vector<Morsel>& morsels,
           const std::vector<Layout>& layouts, bool two_keys) {
    AggStatePtr st = agg_new(
        {AggSpec{AggOp::Count, -1, "n"}, AggSpec{AggOp::Sum, 0, "sum"}});
    for (std::size_t j = 0; j < morsels.size(); ++j) {
        const Morsel& m = morsels[j];
        const auto len = static_cast<std::int64_t>(m.keys.size());
        Series k = keys_as(m.keys, layouts[j]);
        Series o = Series::flat_i64(m.other.data(), len);
        Series v = Series::flat_i64(m.vals.data(), len);
        std::vector<const Series*> keys = {&k};
        if (two_keys) keys.push_back(&o);
        agg_accumulate(*st, keys, {&v}, 0, len);
    }
    DataFrame out = agg_finalize(*st, std::vector<std::string>{"k", "o"}.at(0));
    return cells_of(out);
}

std::vector<Morsel> morsels() {
    return {make_morsel(300, 0, 5), make_morsel(300, 2, 8),
            make_morsel(300, 1, 8), make_morsel(300, 5, 6)};
}

}  // namespace

TEST_SUITE("group by string encodings") {
    TEST_CASE("one layout for every morsel equals the flat result") {
        const auto ms = morsels();
        const Cells want =
            fold(ms, {Layout::Flat, Layout::Flat, Layout::Flat, Layout::Flat},
                 false);
        REQUIRE(!want.empty());
        for (Layout l : {Layout::Dictionary, Layout::View})
            CHECK(fold(ms, {l, l, l, l}, false) == want);
    }

    TEST_CASE("morsels in different layouts merge into the same groups") {
        const auto ms = morsels();
        const Cells want =
            fold(ms, {Layout::Flat, Layout::Flat, Layout::Flat, Layout::Flat},
                 false);
        const std::vector<std::vector<Layout>> mixes = {
            {Layout::Dictionary, Layout::Flat, Layout::View,
             Layout::Dictionary},
            {Layout::Flat, Layout::Dictionary, Layout::Flat, Layout::View},
            {Layout::View, Layout::View, Layout::Dictionary, Layout::Flat}};
        for (const auto& mix : mixes) CHECK(fold(ms, mix, false) == want);
    }

    TEST_CASE("a dictionary column with another key column") {
        const auto ms = morsels();
        const std::vector<Layout> flat(4, Layout::Flat);
        const Cells want = fold(ms, flat, true);
        CHECK(fold(ms, std::vector<Layout>(4, Layout::Dictionary), true) ==
              want);
        CHECK(fold(ms,
                   {Layout::Dictionary, Layout::Flat, Layout::View,
                    Layout::Dictionary},
                   true) == want);
    }
}
