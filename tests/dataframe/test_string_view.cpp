// VIEW columns: 16-byte Arrow views over buffers the column keeps alive,
// their constructor, readers, kernels and the spill, native and IPC writers.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/column_data.h>
#include <dftracer/utils/dataframe/internal/frame_native.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/kernels/dictionary.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/series.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dftracer::utils::dataframe;

namespace {

using Row = std::optional<std::string>;
using Rows = std::vector<Row>;

constexpr std::int32_t INLINE_MAX = 12;

struct Owner {
    std::vector<std::uint8_t> views;
    std::vector<std::string> blobs;
    std::vector<std::uint8_t> validity;
    int* released = nullptr;
};

void on_release(void* ctx) {
    auto* o = static_cast<Owner*>(ctx);
    ++*o->released;
    delete o;
}

void put_view(std::uint8_t* v, const std::string& s, std::int32_t index,
              std::int32_t offset) {
    const auto size = static_cast<std::int32_t>(s.size());
    std::memset(v, 0, 16);
    std::memcpy(v, &size, 4);
    if (size <= INLINE_MAX) {
        std::memcpy(v + 4, s.data(), s.size());
    } else {
        std::memcpy(v + 4, s.data(), 4);
        std::memcpy(v + 8, &index, 4);
        std::memcpy(v + 12, &offset, 4);
    }
}

// Long values go to blob (i % 2), so two buffers are in use.
std::unique_ptr<Owner> build_owner(const Rows& rows, int* released) {
    auto o = std::make_unique<Owner>();
    o->released = released;
    o->blobs.assign(2, std::string());
    o->views.assign(rows.size() * 16, 0);
    bool any_null = false;
    o->validity.assign((rows.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i]) {
            any_null = true;
            continue;
        }
        o->validity[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        const std::string& s = *rows[i];
        std::int32_t offset = 0;
        const std::int32_t index = static_cast<std::int32_t>(i % 2);
        if (s.size() > static_cast<std::size_t>(INLINE_MAX)) {
            offset = static_cast<std::int32_t>(o->blobs[index].size());
            o->blobs[index] += s;
        }
        put_view(o->views.data() + i * 16, s, index, offset);
    }
    if (!any_null) o->validity.clear();
    return o;
}

dftu_series* make_raw(const Rows& rows, int* released,
                      TypeId type = TypeId::String) {
    auto o = build_owner(rows, released);
    const void* bufs[2] = {o->blobs[0].data(), o->blobs[1].data()};
    const std::int64_t sizes[2] = {
        static_cast<std::int64_t>(o->blobs[0].size()),
        static_cast<std::int64_t>(o->blobs[1].size())};
    Owner* raw = o.release();
    return dftu_series_new_string_view(
        static_cast<dftu_dtype>(type), raw->views.data(),
        static_cast<std::int64_t>(rows.size()),
        raw->validity.empty() ? nullptr : raw->validity.data(), bufs, sizes, 2,
        on_release, raw);
}

Series make_view(const Rows& rows, int* released) {
    Series s{make_raw(rows, released)};
    REQUIRE(s.valid());
    return s;
}

Series make_flat(const Rows& rows) {
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

Rows rows_of(const Series& s) {
    Rows out;
    for (std::int64_t i = 0; i < s.length(); ++i)
        out.push_back(s.is_null(i) ? Row() : Row(std::string(s.string_at(i))));
    return out;
}

std::vector<int> bits_of(const Series& mask) {
    const Series m = mask.materialize();
    std::vector<int> out;
    for (std::int64_t i = 0; i < m.length(); ++i) {
        if (m.is_null(i))
            out.push_back(2);
        else
            out.push_back((m.handle()->data->data()[i >> 3] >> (i & 7)) & 1);
    }
    return out;
}

const Rows SAMPLE = {Row("short"),
                     Row("a value longer than twelve"),
                     Row(),
                     Row("pear"),
                     Row("exactly12byt"),
                     Row("exactly13byte"),
                     Row("pear"),
                     Row(""),
                     Row("zz another long value here"),
                     Row("apple")};

}  // namespace

TEST_SUITE("string view") {
    TEST_CASE("inline and out-of-line values read back") {
        int released = 0;
        {
            Series s = make_view(SAMPLE, &released);
            CHECK(s.encoding() == Encoding::View);
            CHECK(s.type() == TypeId::String);
            CHECK(s.length() == 10);
            CHECK(s.null_count() == 1);
            CHECK(s.string_at(0) == "short");
            CHECK(s.string_at(1) == "a value longer than twelve");
            CHECK(s.string_at(4) == "exactly12byt");
            CHECK(s.string_at(5) == "exactly13byte");
            CHECK(s.string_at(7) == "");
            CHECK(s.string_at(8) == "zz another long value here");
            CHECK(s.is_null(2));
            CHECK(s.string_at(2).empty());
            CHECK(s.string_at(-1).empty());
            CHECK(s.string_at(10).empty());
            CHECK(rows_of(s) == SAMPLE);
        }
        CHECK(released == 1);
    }

    TEST_CASE("the release runs once, after the last column that uses it") {
        int released = 0;
        Series filtered;
        {
            Series s = make_view(SAMPLE, &released);
            filtered = s.filter(s.str_contains("long"));
            CHECK(filtered.encoding() == Encoding::View);
        }
        CHECK(released == 0);
        CHECK(rows_of(filtered) == Rows{Row("a value longer than twelve"),
                                        Row("zz another long value here")});
        filtered = Series();
        CHECK(released == 1);
    }

    TEST_CASE("the constructor rejects a bad column and releases once") {
        const std::string blob(40, 'q');
        std::uint8_t view[16];
        const void* bufs[1] = {blob.data()};
        const std::int64_t sizes[1] = {40};
        int released = 0;
        auto run = [&](dftu_dtype type, std::int64_t n, std::int32_t nb,
                       const std::int64_t* sz) {
            released = 0;
            dftu_series* s = dftu_series_new_string_view(
                type, view, n, nullptr, bufs, sz, nb,
                [](void* c) { ++*static_cast<int*>(c); }, &released);
            if (s != nullptr) dftu_series_free(s);
            return std::make_pair(s != nullptr, released);
        };
        const auto S = static_cast<dftu_dtype>(TypeId::String);

        put_view(view, blob.substr(0, 20), 0, 0);
        CHECK(run(S, 1, 1, sizes) == std::make_pair(true, 1));
        CHECK(run(static_cast<dftu_dtype>(TypeId::Int64), 1, 1, sizes) ==
              std::make_pair(false, 1));
        CHECK(run(S, -1, 1, sizes) == std::make_pair(false, 1));
        CHECK(run(S, 1, -1, sizes) == std::make_pair(false, 1));
        const std::int64_t negative[1] = {-1};
        CHECK(run(S, 1, 1, negative) == std::make_pair(false, 1));
        CHECK(run(S, 1, 0, sizes) == std::make_pair(false, 1));

        put_view(view, blob.substr(0, 20), 2, 0);
        CHECK(run(S, 1, 1, sizes) == std::make_pair(false, 1));
        put_view(view, blob.substr(0, 20), -1, 0);
        CHECK(run(S, 1, 1, sizes) == std::make_pair(false, 1));
        put_view(view, blob.substr(0, 20), 0, 21);
        CHECK(run(S, 1, 1, sizes) == std::make_pair(false, 1));
        put_view(view, blob.substr(0, 20), 0, -1);
        CHECK(run(S, 1, 1, sizes) == std::make_pair(false, 1));
        const std::int32_t negative_size = -5;
        put_view(view, blob.substr(0, 20), 0, 0);
        std::memcpy(view, &negative_size, 4);
        CHECK(run(S, 1, 1, sizes) == std::make_pair(false, 1));
    }

    TEST_CASE("a null row's view is not checked") {
        std::uint8_t view[16] = {};
        const std::int32_t size = 99;
        std::memcpy(view, &size, 4);
        const std::uint8_t validity[1] = {0};
        int released = 0;
        dftu_series* s = dftu_series_new_string_view(
            static_cast<dftu_dtype>(TypeId::String), view, 1, validity, nullptr,
            nullptr, 0, [](void* c) { ++*static_cast<int*>(c); }, &released);
        REQUIRE(s != nullptr);
        CHECK(dftu_series_is_null(s, 0) == 1);
        dftu_series_free(s);
        CHECK(released == 1);
    }

    TEST_CASE("a Binary view column reads and materializes as Binary") {
        int released = 0;
        Series s{make_raw(SAMPLE, &released, TypeId::Binary)};
        REQUIRE(s.valid());
        CHECK(s.type() == TypeId::Binary);
        const Series flat = s.materialize();
        CHECK(flat.type() == TypeId::Binary);
        CHECK(rows_of(flat) == SAMPLE);
    }

    TEST_CASE("materialize gives the flat column with the same rows") {
        int released = 0;
        const Series s = make_view(SAMPLE, &released);
        const Series flat = s.materialize();
        CHECK(flat.encoding() == Encoding::Flat);
        CHECK(flat.type() == TypeId::String);
        CHECK(flat.null_count() == 1);
        CHECK(rows_of(flat) == SAMPLE);
        const Series empty = make_view({}, &released).materialize();
        CHECK(empty.length() == 0);
        CHECK(empty.encoding() == Encoding::Flat);
    }

    TEST_CASE("buffer_bytes counts the views, validity and each blob once") {
        int released = 0;
        const Series s = make_view(SAMPLE, &released);
        std::int64_t expected = 10 * 16 + 2;
        for (const auto& b : *s.handle()->blobs)
            expected += static_cast<std::int64_t>(b->size());
        CHECK(dftu_series_buffer_bytes(s.handle()) == expected);
        const Series filtered = s.filter(s.str_contains("long"));
        CHECK(dftu_series_buffer_bytes(filtered.handle()) ==
              2 * 16 + 1 + (expected - 10 * 16 - 2));
    }

    TEST_CASE("a dictionary over a view column reads and materializes") {
        int released = 0;
        const Rows values = {Row("short"), Row("a value longer than twelve"),
                             Row("pear")};
        Series base = make_view(values, &released);
        const std::int32_t codes[5] = {1, 0, 2, 1, 1};
        auto* d = new dftu_series();
        d->type = TypeId::String;
        d->encoding = Encoding::Dictionary;
        d->length = 5;
        d->data = Buffer::allocate(sizeof(codes));
        std::memcpy(d->data->data(), codes, sizeof(codes));
        d->set_child(std::make_shared<dftu_series>(*base.handle()));
        Series dict{d};
        const Rows expected = {values[1], values[0], values[2], values[1],
                               values[1]};
        CHECK(rows_of(dict) == expected);
        CHECK(rows_of(dict.materialize()) == expected);
        CHECK(bits_of(dict.str_contains("long")) ==
              std::vector<int>{1, 0, 0, 1, 1});
        CHECK(bits_of(dict.str_eq("pear")) == std::vector<int>{0, 0, 1, 0, 0});
    }

    TEST_CASE("a selection over a view column reads and materializes") {
        int released = 0;
        Series base = make_view(SAMPLE, &released);
        const std::int64_t idx[4] = {8, -1, 3, 1};
        auto* s = new dftu_series();
        s->type = TypeId::String;
        s->encoding = Encoding::Selection;
        s->length = 4;
        s->data = Buffer::allocate(sizeof(idx));
        std::memcpy(s->data->data(), idx, sizeof(idx));
        s->set_child(std::make_shared<dftu_series>(*base.handle()));
        Series sel{s};
        const Rows expected = {SAMPLE[8], Row(), SAMPLE[3], SAMPLE[1]};
        CHECK(rows_of(sel) == expected);
        CHECK(rows_of(sel.materialize()) == expected);
        CHECK(sel.null_count() == 1);
    }
}

TEST_SUITE("string view kernels match the flat column") {
    struct Pair {
        int released = 0;
        Series view;
        Series flat;
        Pair() : view(make_view(SAMPLE, &released)), flat(make_flat(SAMPLE)) {}
    };

    TEST_CASE("compare, string predicates, filter and take") {
        Pair p;
        for (CmpOp op : {CmpOp::Eq, CmpOp::Ne})
            CHECK(bits_of(p.view.compare(op, str("pear"))) ==
                  bits_of(p.flat.compare(op, str("pear"))));
        const Series pivot = make_flat(Rows(10, Row("pear")));
        for (CmpOp op : {CmpOp::Lt, CmpOp::Ge}) {
            const auto code = static_cast<dftu_cmp_op>(op);
            const Series on_view{dftu_series_compare_series(
                p.view.handle(), pivot.handle(), code)};
            const Series on_flat{dftu_series_compare_series(
                p.flat.handle(), pivot.handle(), code)};
            REQUIRE(on_view.valid());
            REQUIRE(on_flat.valid());
            CHECK(bits_of(on_view) == bits_of(on_flat));
        }
        CHECK(bits_of(p.view.str_contains("long")) ==
              bits_of(p.flat.str_contains("long")));
        CHECK(bits_of(p.view.str_starts_with("exactly")) ==
              bits_of(p.flat.str_starts_with("exactly")));
        CHECK(bits_of(p.view.str_eq("exactly13byte")) ==
              bits_of(p.flat.str_eq("exactly13byte")));
        const Series mask = p.flat.str_contains("e");
        CHECK(rows_of(p.view.filter(mask)) == rows_of(p.flat.filter(mask)));
        const std::vector<std::int64_t> idx = {9, 1, 2, 2, 0, 8};
        CHECK(rows_of(p.view.take(idx)) == rows_of(p.flat.take(idx)));
        const std::vector<std::int64_t> with_null = {3, -1, 1};
        CHECK(rows_of(p.view.take(with_null)) ==
              rows_of(p.flat.take(with_null)));
    }

    TEST_CASE("sort, argsort and unique") {
        Pair p;
        CHECK(rows_of(p.view.sort()) == rows_of(p.flat.sort()));
        CHECK(rows_of(p.view.sort(true)) == rows_of(p.flat.sort(true)));
        std::vector<std::int64_t> order;
        const Series a = p.view.argsort();
        for (std::int64_t i = 0; i < a.length(); ++i)
            order.push_back(a.data<std::int64_t>()[i]);
        CHECK(rows_of(p.view.take(order)) == rows_of(p.flat.sort()));
        CHECK(rows_of(p.view.unique().sort()) ==
              rows_of(p.flat.unique().sort()));
    }

    TEST_CASE("group_by count and join on the string key") {
        Pair p;
        std::vector<std::int64_t> v(10);
        for (std::int64_t i = 0; i < 10; ++i)
            v[static_cast<std::size_t>(i)] = i;
        auto frame = [&](const Series& key) {
            DataFrame f;
            f.names = {"k", "v"};
            f.columns.push_back(key.share());
            f.columns.push_back(Series::flat_i64(v.data(), 10));
            return f;
        };
        auto counts = [&](const Series& key) {
            DataFrame g = frame(key).group_by(std::string("k"),
                                              {GroupAgg{Agg::Count, "v", "n"}});
            g = g.sort_by("k");
            std::vector<std::pair<Row, std::int64_t>> out;
            const Series k = g.column("k");
            const Series n = g.column("n").materialize();
            for (std::int64_t i = 0; i < g.num_rows(); ++i)
                out.emplace_back(rows_of(k)[static_cast<std::size_t>(i)],
                                 n.data<std::int64_t>()[i]);
            return out;
        };
        CHECK(counts(p.view) == counts(p.flat));

        DataFrame right;
        right.names = {"k", "w"};
        right.columns.push_back(
            Series::strings({"pear", "short", "zz another long value here"}));
        const std::int64_t w[3] = {10, 20, 30};
        right.columns.push_back(Series::flat_i64(w, 3));
        const DataFrame jv = frame(p.view).join(right, {"k"});
        const DataFrame jf = frame(p.flat).join(right, {"k"});
        REQUIRE(jv.num_rows() == jf.num_rows());
        CHECK(jv.num_rows() == 4);
        for (std::int64_t i = 0; i < jv.num_rows(); ++i) {
            CHECK(jv.column("v").materialize().data<std::int64_t>()[i] ==
                  jf.column("v").materialize().data<std::int64_t>()[i]);
            CHECK(jv.column("w").materialize().data<std::int64_t>()[i] ==
                  jf.column("w").materialize().data<std::int64_t>()[i]);
        }
        int released = 0;
        DataFrame left_flat = frame(p.flat);
        DataFrame right_view;
        right_view.names = {"k", "w"};
        right_view.columns.push_back(
            Series::strings({"pear", "short", "zz another long value here"}));
        right_view.columns.push_back(Series::flat_i64(w, 3));
        right_view.columns[0] = make_view(
            {Row("pear"), Row("short"), Row("zz another long value here")},
            &released);
        const DataFrame jw = left_flat.join(right_view, {"k"});
        CHECK(rows_of(jw.column("k")) == rows_of(jf.column("k")));
        CHECK(jw.num_rows() == 4);
    }

    TEST_CASE("concat of two view parts and of a view with a flat part") {
        Pair p;
        int released = 0;
        const Rows other = {Row("tail"), Row(), Row("a third long value here")};
        const Series other_view = make_view(other, &released);
        const Series other_flat = make_flat(other);
        Rows both = SAMPLE;
        both.insert(both.end(), other.begin(), other.end());

        CHECK(rows_of(concat_columns({&p.view, &other_view})) == both);
        CHECK(rows_of(concat_columns({&p.view, &other_flat})) == both);
        CHECK(rows_of(concat_columns({&p.flat, &other_view})) == both);
        CHECK(rows_of(concat_columns({&p.flat, &other_flat})) == both);
    }
}

TEST_SUITE("string view writers") {
    TEST_CASE("spill writes a view column as values") {
        int released = 0;
        const Series s = make_view(SAMPLE, &released);
        std::string bytes;
        spill::put_series(bytes, s);
        const auto* p = reinterpret_cast<const std::uint8_t*>(bytes.data());
        const Series back = spill::get_series(p, p + bytes.size());
        REQUIRE(back.valid());
        CHECK(back.encoding() == Encoding::Flat);
        CHECK(rows_of(back) == SAMPLE);
        std::vector<Series> cols;
        cols.push_back(s.share());
        CHECK(spill::columns_bytes(cols) > 10 * 16);
    }

    TEST_CASE("a native frame writes a view column as values") {
        int released = 0;
        DataFrame f;
        f.names = {"s"};
        f.columns.push_back(make_view(SAMPLE, &released));
        const auto back = frame_from_native(frame_to_native(f));
        REQUIRE(back);
        CHECK(back->columns[0].encoding() == Encoding::Flat);
        CHECK(back->columns[0].type() == TypeId::String);
        CHECK(rows_of(back->columns[0]) == SAMPLE);
    }

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    TEST_CASE("IPC writes a view column as values") {
        int released = 0;
        DataFrame f;
        f.names = {"s"};
        f.columns.push_back(make_view(SAMPLE, &released));
        std::vector<std::uint8_t> bytes;
        REQUIRE_NOTHROW(bytes = f.to_ipc());
        const std::string text(bytes.begin(), bytes.end());
        CHECK(text.find("a value longer than twelve") != std::string::npos);
        CHECK(text.find("zz another long value here") != std::string::npos);
    }
#endif
}

namespace {

Series make_large(const Rows& rows, bool binary = false) {
    auto* w = new dftu_series();
    w->type = binary ? TypeId::LargeBinary : TypeId::LargeString;
    w->encoding = Encoding::Flat;
    w->length = static_cast<std::int64_t>(rows.size());
    w->offsets = Buffer::allocate((rows.size() + 1) * sizeof(std::int64_t));
    std::string bytes;
    std::vector<std::uint8_t> valid((rows.size() + 7) / 8, 0);
    auto* off = reinterpret_cast<std::int64_t*>(w->offsets->data());
    off[0] = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i]) {
            bytes += *rows[i];
            valid[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
        } else {
            ++w->null_count;
        }
        off[i + 1] = static_cast<std::int64_t>(bytes.size());
    }
    w->data = Buffer::allocate(bytes.size());
    if (!bytes.empty())
        std::memcpy(w->data->data(), bytes.data(), bytes.size());
    if (w->null_count > 0) {
        w->validity = Buffer::allocate(valid.size());
        std::memcpy(w->validity->data(), valid.data(), valid.size());
    }
    return Series{w};
}

Series make_selection(const Series& base,
                      const std::vector<std::int64_t>& idx) {
    auto* s = new dftu_series();
    s->type = base.type();
    s->encoding = Encoding::Selection;
    s->length = static_cast<std::int64_t>(idx.size());
    s->data = Buffer::allocate(idx.size() * sizeof(std::int64_t));
    std::memcpy(s->data->data(), idx.data(), idx.size() * sizeof(std::int64_t));
    s->set_child(std::make_shared<dftu_series>(*base.handle()));
    return Series{s};
}

bool blobs_hold(const Series& s, const std::shared_ptr<Buffer>& b) {
    for (const auto& x : *s.handle()->blobs)
        if (x->data() == b->data()) return true;
    return false;
}

Rows join(std::initializer_list<Rows> parts) {
    Rows out;
    for (const Rows& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

}  // namespace

TEST_SUITE("string view concat") {
    TEST_CASE("flat, view and dictionary parts concatenate without copying") {
        int released = 0;
        const Rows flat_rows = {Row("a flat value over twelve"), Row(),
                                Row("f")};
        const Rows dict_rows = {Row("a dictionary value over twelve"), Row("d"),
                                Row("d")};
        Series f = make_flat(flat_rows);
        Series v = make_view(SAMPLE, &released);
        Series d = make_flat(dict_rows).dictionary_encode();
        REQUIRE(d.encoding() == Encoding::Dictionary);
        Series out = concat_columns({&f, &v, &d});
        CHECK(out.encoding() == Encoding::View);
        CHECK(out.type() == TypeId::String);
        CHECK(out.length() == 16);
        CHECK(out.null_count() == 2);
        CHECK(rows_of(out) == join({flat_rows, SAMPLE, dict_rows}));
        CHECK(blobs_hold(out, f.handle()->data));
        CHECK(blobs_hold(out, d.handle()->child()->data));
        for (const auto& b : *v.handle()->blobs) CHECK(blobs_hold(out, b));
        CHECK(out.handle()->blobs->size() == 4);
        v = Series();
        CHECK(released == 0);
        out = Series();
        CHECK(released == 1);
    }

    TEST_CASE("view parts remap buffer indexes and dedupe shared buffers") {
        int r1 = 0, r2 = 0;
        Series a = make_view(SAMPLE, &r1);
        Series b = make_view(SAMPLE, &r2);
        Series out = concat_columns({&a, &b, &a});
        CHECK(out.handle()->blobs->size() == 4);
        CHECK(rows_of(out) == join({SAMPLE, SAMPLE, SAMPLE}));
        CHECK(rows_of(out.materialize()) == join({SAMPLE, SAMPLE, SAMPLE}));
    }

    TEST_CASE("a selection part resolves its rows and nulls") {
        int released = 0;
        Series base = make_view(SAMPLE, &released);
        Series sel = make_selection(base, {8, -1, 3, 1});
        Series f = make_flat({Row("tail")});
        Series out = concat_columns({&sel, &f});
        CHECK(out.encoding() == Encoding::View);
        CHECK(rows_of(out) ==
              Rows{SAMPLE[8], Row(), SAMPLE[3], SAMPLE[1], Row("tail")});
        CHECK(out.null_count() == 1);
    }

    TEST_CASE("nulls of each layout survive at unaligned part seams") {
        int released = 0;
        const Rows a = {Row(), Row("x"), Row("long value number one")};
        const Rows b = {Row("y"), Row(), Row()};
        Series fa = make_flat(a);
        Series vb = make_view(b, &released);
        Series dc = make_flat({Row("p"), Row()}).dictionary_encode();
        Series out = concat_columns({&fa, &vb, &dc});
        CHECK(rows_of(out) == join({a, b, rows_of(dc)}));
        CHECK(out.null_count() == 4);
    }

    TEST_CASE("all dictionary parts with view children give one dictionary") {
        int released = 0;
        Series v1 =
            make_view({Row("alpha"), Row("a long distinct value"), Row("beta")},
                      &released);
        const std::int32_t c1[] = {0, 1, 2, 1};
        Series d1 = dictionary_from_codes(c1, Series(v1.share()), nullptr);
        Series d2 = make_flat({Row("beta"), Row("a long distinct value"),
                               Row("gamma"), Row("beta")})
                        .dictionary_encode();
        Series out = concat_columns({&d1, &d2});
        CHECK(out.encoding() == Encoding::Dictionary);
        CHECK(out.handle()->child()->encoding == Encoding::View);
        CHECK(out.handle()->child()->length == 4);
        CHECK(rows_of(out) == Rows{Row("alpha"), Row("a long distinct value"),
                                   Row("beta"), Row("a long distinct value"),
                                   Row("beta"), Row("a long distinct value"),
                                   Row("gamma"), Row("beta")});
    }

    TEST_CASE("dictionary parts with nulls keep them") {
        const std::int32_t codes[] = {0, 1, 0};
        const std::uint8_t valid = 0x05;
        Series a = dictionary_from_codes(
            codes, make_flat({Row("p"), Row("a long value past twelve")}),
            &valid);
        Series v = make_flat({Row("q")}).dictionary_encode();
        Series out = concat_columns({&a, &v});
        CHECK(out.encoding() == Encoding::Dictionary);
        CHECK(rows_of(out) == Rows{Row("p"), Row(), Row("p"), Row("q")});
    }

    TEST_CASE("JSON parts keep the flag") {
        int released = 0;
        Series a = make_flat({Row("\"a long json string value\""), Row("1")})
                       .as_json();
        Series b =
            make_view({Row("{\"k\":\"a long json object value\"}")}, &released)
                .as_json();
        Series out = concat_columns({&a, &b});
        CHECK(out.is_json());
        CHECK(out.encoding() == Encoding::View);
        CHECK(rows_of(out) ==
              Rows{Row("\"a long json string value\""), Row("1"),
                   Row("{\"k\":\"a long json object value\"}")});
    }

    TEST_CASE(
        "LargeString and LargeBinary parts become String and Binary views") {
        const Rows rows = {Row("wide value longer than twelve"), Row(),
                           Row("w")};
        Series w = make_large(rows);
        Series f = make_flat({Row("narrow")});
        Series out = concat_columns({&w, &f});
        CHECK(out.type() == TypeId::String);
        CHECK(out.encoding() == Encoding::View);
        CHECK(blobs_hold(out, w.handle()->data));
        CHECK(rows_of(out) == join({rows, Rows{Row("narrow")}}));

        Series wb = make_large(rows, true);
        Series bin = concat_columns({&wb, &wb});
        CHECK(bin.type() == TypeId::Binary);
        CHECK(bin.encoding() == Encoding::View);
        CHECK(bin.length() == 6);
        CHECK(bin.null_count() == 2);
        CHECK(std::string(bin.string_at(3)) == *rows[0]);
    }

    TEST_CASE("empty and single parts") {
        Series e = make_flat({});
        Series a = make_flat({Row("one"), Row("a value over twelve bytes")});
        Series out = concat_columns({&e, &a, &e});
        CHECK(out.encoding() == Encoding::View);
        CHECK(rows_of(out) == rows_of(a));
        Series none = concat_columns({&e, &e});
        CHECK(none.length() == 0);
        CHECK(none.null_count() == 0);
        Series one = concat_columns({&a});
        CHECK(rows_of(one) == rows_of(a));
    }

    TEST_CASE("a concat result filters and concatenates again") {
        Series a = make_flat({Row("keep this long value"), Row("drop")});
        Series b = make_flat({Row("keep that long value")});
        Series once = concat_columns({&a, &b});
        Series twice = concat_columns({&once, &a});
        CHECK(rows_of(twice) == Rows{Row("keep this long value"), Row("drop"),
                                     Row("keep that long value"),
                                     Row("keep this long value"), Row("drop")});
        Series kept = twice.filter(twice.str_contains("keep"));
        CHECK(kept.length() == 3);
    }
}
