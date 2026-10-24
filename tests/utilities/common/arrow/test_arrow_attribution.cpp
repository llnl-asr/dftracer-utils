#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/common/arrow/array_view.h>
#include <dftracer/utils/utilities/common/arrow/arrow.h>
#include <doctest/doctest.h>
#include <nanoarrow/nanoarrow.h>

#include <optional>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::common::arrow;

namespace {

struct Reader {
    ArrowArrayView av;
    explicit Reader(ArrowExportResult& r) {
        REQUIRE(init_array_view(av, r.get_schema(), r.get_array()) ==
                NANOARROW_OK);
    }
    ~Reader() { ArrowArrayViewReset(&av); }
    std::int64_t rows() const { return av.length; }
    std::int64_t i64(int c, std::int64_t r) const {
        return ArrowArrayViewGetIntUnsafe(av.children[c], r);
    }
    double f64(int c, std::int64_t r) const {
        return ArrowArrayViewGetDoubleUnsafe(av.children[c], r);
    }
    std::string str(int c, std::int64_t r) const {
        ArrowStringView s = ArrowArrayViewGetStringUnsafe(av.children[c], r);
        return std::string(s.data, static_cast<std::size_t>(s.size_bytes));
    }
};

using OptB = std::optional<bool>;
using OptS = std::optional<std::string>;
using OptI = std::optional<std::int64_t>;

ArrowExportResult make_batch(const std::vector<OptB>& sel,
                             const std::vector<OptS>& file,
                             const std::vector<OptI>& rk) {
    RecordBatchBuilder b;
    b.declare_schema({{"sel", ColumnType::Bool},
                      {"file", ColumnType::String},
                      {"rank", ColumnType::Int64}});
    for (std::size_t i = 0; i < sel.size(); ++i) {
        if (sel[i])
            b.append_bool(0, *sel[i]);
        else
            b.append_null(0);
        if (file[i])
            b.append_string(1, *file[i]);
        else
            b.append_null(1);
        if (rk[i])
            b.append_int64(2, *rk[i]);
        else
            b.append_null(2);
        b.end_row();
    }
    return b.finish();
}

ArrowExportResult attr(ArrowExportResult& in, std::vector<std::uint32_t> dims,
                       std::uint32_t top_k) {
    return attribute(in.get_schema(), in.get_array(), 0, dims.data(),
                     static_cast<std::uint32_t>(dims.size()), top_k);
}

std::string name_of(ArrowExportResult& r, int c) {
    return r.get_schema()->children[c]->name;
}

}  // namespace

TEST_CASE("attribute - over/under-represented ranking") {
    // 10 selected, 10 baseline. hot: 9 sel/1 base (+0.8); cold: 1 sel/8 base
    // (-0.7); mid: 0 sel/1 base (-0.1).
    std::vector<OptB> sel;
    std::vector<OptS> file;
    std::vector<OptI> rk(20, OptI(0));
    for (int i = 0; i < 9; ++i) {
        sel.push_back(true);
        file.push_back(std::string("hot.dat"));
    }
    sel.push_back(true);
    file.push_back(std::string("cold.dat"));
    sel.push_back(false);
    file.push_back(std::string("hot.dat"));
    for (int i = 0; i < 8; ++i) {
        sel.push_back(false);
        file.push_back(std::string("cold.dat"));
    }
    sel.push_back(false);
    file.push_back(std::string("mid.dat"));

    auto in = make_batch(sel, file, rk);
    auto out = attr(in, {1}, 0);

    REQUIRE(out.num_columns() == 7);
    CHECK(name_of(out, 0) == "dimension");
    CHECK(name_of(out, 1) == "value");
    CHECK(name_of(out, 2) == "selected_count");
    CHECK(name_of(out, 3) == "baseline_count");
    CHECK(name_of(out, 4) == "selected_frac");
    CHECK(name_of(out, 5) == "baseline_frac");
    CHECK(name_of(out, 6) == "difference");

    Reader rd(out);
    REQUIRE(rd.rows() == 3);
    CHECK(rd.str(0, 0) == "file");
    CHECK(rd.str(1, 0) == "hot.dat");
    CHECK(rd.i64(2, 0) == 9);
    CHECK(rd.i64(3, 0) == 1);
    CHECK(rd.f64(4, 0) == doctest::Approx(0.9));
    CHECK(rd.f64(5, 0) == doctest::Approx(0.1));
    CHECK(rd.f64(6, 0) == doctest::Approx(0.8));
    CHECK(rd.str(1, 1) == "cold.dat");
    CHECK(rd.f64(6, 1) == doctest::Approx(-0.7));
    CHECK(rd.str(1, 2) == "mid.dat");
    CHECK(rd.f64(6, 2) == doctest::Approx(-0.1));
}

TEST_CASE("attribute - multi-dim string and int rendered to string") {
    std::vector<OptB> sel = {true, true, false, false};
    std::vector<OptS> file = {std::string("a"), std::string("a"),
                              std::string("b"), std::string("b")};
    std::vector<OptI> rk = {7, 7, 7, 9};

    auto in = make_batch(sel, file, rk);
    auto out = attr(in, {1, 2}, 0);
    Reader rd(out);
    REQUIRE(rd.rows() == 4);  // file:{a,b} + rank:{7,9}

    bool saw_rank7 = false;
    bool saw_int_rendered = false;
    for (std::int64_t r = 0; r < rd.rows(); ++r) {
        if (rd.str(0, r) == "rank" && rd.str(1, r) == "7") {
            saw_rank7 = true;
            saw_int_rendered = true;
            CHECK(rd.i64(2, r) == 2);
            CHECK(rd.i64(3, r) == 1);
            CHECK(rd.f64(6, r) == doctest::Approx(0.5));
        }
    }
    CHECK(saw_rank7);
    CHECK(saw_int_rendered);
}

TEST_CASE("attribute - top_k cap") {
    std::vector<OptB> sel = {true, true, false, false};
    std::vector<OptS> file = {std::string("a"), std::string("b"),
                              std::string("c"), std::string("d")};
    std::vector<OptI> rk(4, OptI(0));
    auto in = make_batch(sel, file, rk);
    auto full = attr(in, {1}, 0);
    {
        Reader rd(full);
        REQUIRE(rd.rows() == 4);
    }
    auto capped = attr(in, {1}, 2);
    Reader rd(capped);
    REQUIRE(rd.rows() == 2);
}

TEST_CASE("attribute - determinism across input order") {
    std::vector<OptB> s1 = {true, true, false, false};
    std::vector<OptS> f1 = {std::string("a"), std::string("b"),
                            std::string("a"), std::string("c")};
    std::vector<OptI> r1(4, OptI(0));
    std::vector<OptB> s2 = {false, true, false, true};
    std::vector<OptS> f2 = {std::string("c"), std::string("b"),
                            std::string("a"), std::string("a")};
    std::vector<OptI> r2(4, OptI(0));

    auto o1 = [&] {
        auto in = make_batch(s1, f1, r1);
        return attr(in, {1}, 0);
    }();
    auto o2 = [&] {
        auto in = make_batch(s2, f2, r2);
        return attr(in, {1}, 0);
    }();
    Reader a(o1), b(o2);
    REQUIRE(a.rows() == b.rows());
    for (std::int64_t i = 0; i < a.rows(); ++i) {
        CHECK(a.str(1, i) == b.str(1, i));
        CHECK(a.i64(2, i) == b.i64(2, i));
        CHECK(a.i64(3, i) == b.i64(3, i));
        CHECK(a.f64(6, i) == doctest::Approx(b.f64(6, i)));
    }
}

TEST_CASE("attribute - empty populations, no div-by-zero") {
    std::vector<OptI> rk(2, OptI(0));
    SUBCASE("all baseline (empty selected)") {
        std::vector<OptB> sel = {false, false};
        std::vector<OptS> file = {std::string("a"), std::string("b")};
        auto in = make_batch(sel, file, rk);
        auto out = attr(in, {1}, 0);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        for (std::int64_t i = 0; i < 2; ++i) {
            CHECK(rd.i64(2, i) == 0);
            CHECK(rd.f64(4, i) == doctest::Approx(0.0));
            CHECK(rd.f64(5, i) == doctest::Approx(0.5));
            CHECK(rd.f64(6, i) == doctest::Approx(-0.5));
        }
    }
    SUBCASE("all selected (empty baseline)") {
        std::vector<OptB> sel = {true, true};
        std::vector<OptS> file = {std::string("a"), std::string("b")};
        auto in = make_batch(sel, file, rk);
        auto out = attr(in, {1}, 0);
        Reader rd(out);
        REQUIRE(rd.rows() == 2);
        for (std::int64_t i = 0; i < 2; ++i) {
            CHECK(rd.i64(3, i) == 0);
            CHECK(rd.f64(4, i) == doctest::Approx(0.5));
            CHECK(rd.f64(5, i) == doctest::Approx(0.0));
            CHECK(rd.f64(6, i) == doctest::Approx(0.5));
        }
    }
}

TEST_CASE("attribute - null dim value and null select skipped") {
    // row2 has a null file (skipped in file counts); row3 has a null select
    // (excluded from both populations).
    std::vector<OptB> sel = {true, false, true, std::nullopt};
    std::vector<OptS> file = {std::string("a"), std::string("a"), std::nullopt,
                              std::string("a")};
    std::vector<OptI> rk(4, OptI(0));
    auto in = make_batch(sel, file, rk);
    auto out = attr(in, {1}, 0);
    Reader rd(out);
    REQUIRE(rd.rows() == 1);
    CHECK(rd.str(1, 0) == "a");
    CHECK(rd.i64(2, 0) == 1);
    CHECK(rd.i64(3, 0) == 1);
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW
