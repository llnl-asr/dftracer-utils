#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/mask.h>
#include <dftracer/utils/dataframe/plan.h>
#include <dftracer/utils/duql/abi.h>
#include <dftracer/utils/duql/errc.h>
#include <dftracer/utils/duql/query.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using dftracer::utils::Condition;
using dftracer::utils::DFTUtilsException;
using dftracer::utils::dataframe::DataFrame;
using dftracer::utils::dataframe::Series;
namespace duql = dftracer::utils::duql;
namespace dataframe = dftracer::utils::dataframe;

namespace {
int bit_at(const Series& mask, int i) {
    const std::uint8_t* bits = mask.data<std::uint8_t>();
    return (bits[i >> 3] >> (i & 7)) & 1;
}

// 'T', 'F' or 'N' (null) per row; a null row must store data 0.
std::string truth(const Series& m) {
    std::string out;
    for (int i = 0; i < m.length(); ++i) {
        if (m.is_null(i)) {
            CHECK(bit_at(m, i) == 0);
            out += 'N';
        } else {
            out += bit_at(m, i) ? 'T' : 'F';
        }
    }
    return out;
}

Series mask_of(const char* q, const DataFrame& b) {
    auto parsed = duql::Query::from_string(q);
    REQUIRE(parsed.has_value());
    return dataframe::evaluate_mask(parsed->root(), b);
}

std::int64_t kept_rows(const char* q, const DataFrame& b) {
    auto parsed = duql::Query::from_string(q);
    REQUIRE(parsed.has_value());
    dataframe::QueryPlan plan;
    plan.where = &parsed->root();
    return dataframe::execute(plan, b).num_rows();
}

DataFrame nullable_x() {
    std::vector<std::int64_t> x{1, 0, 3, 2};
    std::uint8_t valid = 0b1101;
    DataFrame b;
    b.names = {"x", "cat"};
    b.columns.push_back(Series::flat_i64(x.data(), 4, &valid));
    b.columns.push_back(Series::strings({"io", "io", "cpu", "net"}));
    return b;
}
}  // namespace

TEST_SUITE("query vec_mask") {
    TEST_CASE("numeric + string predicate lowers to a SIMD mask") {
        std::vector<std::int64_t> dur{50, 150, 200, 80, 300};
        DataFrame b;
        b.names = {"dur", "cat"};
        b.columns.push_back(Series::flat_i64(dur.data(), 5));
        b.columns.push_back(Series::strings({"io", "cpu", "io", "io", "net"}));

        auto q = duql::Query::from_string("dur > 100 and cat == \"io\"");
        REQUIRE(q.has_value());
        Series mask = dataframe::evaluate_mask(q->root(), b);
        REQUIRE(mask.type() == dftracer::utils::dataframe::TypeId::Bool);
        REQUIRE(mask.length() == 5);
        // dur>100: {1,2,4}; cat==io: {0,2,3}; AND -> only row 2.
        CHECK(bit_at(mask, 0) == 0);
        CHECK(bit_at(mask, 1) == 0);
        CHECK(bit_at(mask, 2) == 1);
        CHECK(bit_at(mask, 3) == 0);
        CHECK(bit_at(mask, 4) == 0);
    }

    TEST_CASE("in-list lowers to OR-of-equals") {
        std::vector<std::int64_t> x{1, 2, 3, 4};
        DataFrame b;
        b.names = {"x"};
        b.columns.push_back(Series::flat_i64(x.data(), 4));

        auto q = duql::Query::from_string("x in [2, 4]");
        REQUIRE(q.has_value());
        Series mask = dataframe::evaluate_mask(q->root(), b);
        CHECK(bit_at(mask, 0) == 0);
        CHECK(bit_at(mask, 1) == 1);
        CHECK(bit_at(mask, 2) == 0);
        CHECK(bit_at(mask, 3) == 1);
    }

    TEST_CASE("an expression leaf is evaluated row by row") {
        const DataFrame b = nullable_x();
        Series mask = mask_of("x * 2 > 3", b);
        REQUIRE(mask.type() == dftracer::utils::dataframe::TypeId::Bool);
        CHECK(bit_at(mask, 0) == 0);
        CHECK(bit_at(mask, 2) == 1);
        CHECK(bit_at(mask, 3) == 1);
        CHECK(mask.is_null(1));
        CHECK(kept_rows("x * 2 > 3", b) == 2);
        CHECK(kept_rows("x is null", b) == 1);
        CHECK(kept_rows(R"(lower(cat) == "io" and x // 2 == 0)", b) == 1);
        CHECK(kept_rows("nope is missing", b) == 4);
    }

    TEST_CASE("unsupported predicate throws DuqlErrc::Unsupported") {
        std::vector<std::int64_t> v{1};
        DataFrame b;
        b.names = {"name"};
        b.columns.push_back(Series::flat_i64(v.data(), 1));

        CHECK(truth(mask_of("name ~ \"^p\"", b)) == "N");
        auto q = duql::Query::from_string("any(name) == \"p\"");
        REQUIRE(q.has_value());
        bool threw = false;
        try {
            dataframe::evaluate_mask(q->root(), b);
        } catch (const DFTUtilsException& e) {
            threw = true;
            CHECK(e.condition() == Condition::Unsupported);
            CHECK(e.domain() == duql::ERROR_DOMAIN.id);
        }
        CHECK(threw);
    }

    TEST_CASE("missing column is UNKNOWN on every row") {
        DataFrame b = nullable_x();
        CHECK(truth(mask_of("dur > 5", b)) == "NNNN");
        CHECK(truth(mask_of("not dur > 5", b)) == "NNNN");
        CHECK(truth(mask_of("dur > 5 or x == 1", b)) == "TNNN");
        CHECK(truth(mask_of("dur > 5 and x == 2", b)) == "FNFN");
        CHECK(kept_rows("dur > 5", b) == 0);
        CHECK(kept_rows("not dur > 5", b) == 0);
    }

    TEST_CASE("null cell is UNKNOWN and not keeps it UNKNOWN") {
        DataFrame b = nullable_x();
        CHECK(truth(mask_of("x == 1", b)) == "TNFF");
        CHECK(truth(mask_of("not x == 1", b)) == "FNTT");
        CHECK(truth(mask_of("x != 1", b)) == "FNTT");
        CHECK(kept_rows("not x == 1", b) == 2);
        CHECK(kept_rows("x != 1 or cat == \"io\"", b) == 4);
    }

    TEST_CASE("type mismatch is UNKNOWN") {
        DataFrame b = nullable_x();
        CHECK(truth(mask_of("x == \"1\"", b)) == "NNNN");
        CHECK(truth(mask_of("x == true", b)) == "NNNN");
        CHECK(truth(mask_of("cat == 1", b)) == "NNNN");
        CHECK(kept_rows("not cat == 1", b) == 0);
    }

    TEST_CASE("in skips incomparable elements; not in negates") {
        DataFrame b = nullable_x();
        CHECK(truth(mask_of("x in [1, \"a\", 3]", b)) == "TNTF");
        CHECK(truth(mask_of("x not in [1, \"a\", 3]", b)) == "FNFT");
        CHECK(truth(mask_of("x in [\"a\", \"b\"]", b)) == "NNNN");
        CHECK(truth(mask_of("cat in [\"io\", 2]", b)) == "TTFF");
    }

    TEST_CASE("Kleene and/or/not truth table") {
        using dataframe::TypeId;
        // a = TTT FFF NNN, b = TFN TFN TFN; null slots carry data 1.
        std::vector<std::uint8_t> ad{0b11000111, 0b1};
        std::vector<std::uint8_t> av{0b00111111, 0b0};
        std::vector<std::uint8_t> bd{0b01101101, 0b1};
        std::vector<std::uint8_t> bv{0b11011011, 0b0};
        Series a = Series::flat(TypeId::Bool, ad.data(), 9, av.data());
        Series c = Series::flat(TypeId::Bool, bd.data(), 9, bv.data());
        CHECK(truth(a & c) == "TFNFFFNFN");
        CHECK(truth(a | c) == "TTTTFNTNN");
        CHECK(truth(~a) == "FFFTTTNNN");
        CHECK((a & c).null_count() == 3);
    }

    TEST_CASE("query plan: where + select + order by + limit") {
        std::vector<std::int64_t> dur{50, 150, 200, 80, 300};
        DataFrame b;
        b.names = {"dur", "cat"};
        b.columns.push_back(Series::flat_i64(dur.data(), 5));
        b.columns.push_back(Series::strings({"io", "cpu", "io", "io", "net"}));

        auto q = duql::Query::from_string("dur > 60");
        REQUIRE(q.has_value());
        dataframe::QueryPlan plan;
        plan.where = &q->root();
        plan.select = {"dur", "cat"};
        plan.order_by = "dur";
        plan.descending = true;
        plan.limit = 2;
        DataFrame out = dataframe::execute(plan, b);
        // dur>60: {150,200,80,300}; sort desc -> 300,200,150,80; limit 2.
        REQUIRE(out.num_rows() == 2);
        REQUIRE(out.names == std::vector<std::string>{"dur", "cat"});
        CHECK(out.columns[0].data<std::int64_t>()[0] == 300);
        CHECK(out.columns[0].data<std::int64_t>()[1] == 200);
        CHECK(out.columns[1].string_at(0) == "net");
    }

    TEST_CASE("query plan: where + group by/agg + order by + limit") {
        std::vector<std::int64_t> dur{10, 5, 20, 15, 30};
        DataFrame b;
        b.names = {"cat", "dur"};
        b.columns.push_back(Series::strings({"io", "cpu", "io", "io", "net"}));
        b.columns.push_back(Series::flat_i64(dur.data(), 5));

        dataframe::QueryPlan plan;
        plan.group_by = "cat";
        plan.aggs = {{dataframe::Agg::Count, "", "count"},
                     {dataframe::Agg::Sum, "dur", "sum_dur"}};
        plan.order_by = "count";
        plan.descending = true;
        DataFrame out = dataframe::execute(plan, b);
        // groups io(3),cpu(1),net(1); order by count desc -> io first.
        REQUIRE(out.num_rows() == 3);
        REQUIRE(out.names ==
                std::vector<std::string>{"cat", "count", "sum_dur"});
        CHECK(out.columns[0].string_at(0) == "io");
        CHECK(out.columns[1].data<std::int64_t>()[0] == 3);
        CHECK(out.columns[2].data<std::int64_t>()[0] == 45);  // io dur 10+20+15
    }

    TEST_CASE("C ABI: parse / to_string / mask round-trip") {
        dftu_duql* q = dftu_duql_parse("dur >= 100 and cat == \"io\"");
        REQUIRE(q != nullptr);

        char* s = dftu_duql_to_string(q);
        REQUIRE(s != nullptr);
        CHECK(std::strstr(s, "dur") != nullptr);
        CHECK(std::strstr(s, "io") != nullptr);
        dftu_duql_string_free(s);

        std::vector<std::int64_t> dur{50, 150, 200};
        Series cdur = Series::flat_i64(dur.data(), 3);
        Series ccat = Series::strings({"io", "io", "net"});
        const dftu_series* cols[2] = {cdur.handle(), ccat.handle()};
        const char* names[2] = {"dur", "cat"};
        dftu_series* m = dftu_dataframe_mask(q, cols, names, 2);
        REQUIRE(m != nullptr);
        Series mask{m};
        // dur>=100: {1,2}; cat==io: {0,1}; AND -> row 1.
        CHECK(bit_at(mask, 0) == 0);
        CHECK(bit_at(mask, 1) == 1);
        CHECK(bit_at(mask, 2) == 0);

        // Unsupported predicate -> NULL (caller falls back to pushdown).
        dftu_duql* rq = dftu_duql_parse("any(cat) == \"io\"");
        REQUIRE(rq != nullptr);
        CHECK(dftu_dataframe_mask(rq, cols, names, 2) == nullptr);
        dftu_duql_free(rq);
        dftu_duql_free(q);
    }
}
