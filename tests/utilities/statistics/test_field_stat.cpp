#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/field_stat.h>
#include <doctest/doctest.h>

#include <bit>
#include <cstdint>

using dftracer::utils::dataframe::FieldStat;
using dftracer::utils::dataframe::FieldStatDomain;

TEST_SUITE("FieldStat") {
    TEST_CASE("int64 sum/min/max stay exact past 2^53") {
        // Epoch-ns timestamps exceed 2^53, so a double sum/min/max would round.
        const std::int64_t base = 1700000000000000001LL;  // > 2^53
        FieldStat fs;
        fs.add(base);
        fs.add(base + 4);
        fs.add(base + 2);
        CHECK(fs.domain == FieldStatDomain::I64);
        CHECK(fs.n == 3);
        CHECK(fs.emin == base);
        CHECK(fs.emax == base + 4);
        CHECK(fs.esum == 3 * base + 6);
        // The double sum would have lost the low bits.
        CHECK(static_cast<std::int64_t>(fs.sum) != fs.esum);
    }

    TEST_CASE("uint64 domain via bit-cast storage") {
        const std::uint64_t big = 18000000000000000000ULL;  // > int64 max
        FieldStat fs;
        fs.add(big);
        fs.add(big + 10);
        CHECK(fs.domain == FieldStatDomain::U64);
        CHECK(std::bit_cast<std::uint64_t>(fs.emin) == big);
        CHECK(std::bit_cast<std::uint64_t>(fs.emax) == big + 10);
        CHECK(std::bit_cast<std::uint64_t>(fs.esum) == big + big + 10);
    }

    TEST_CASE("a float value demotes the field to F64") {
        FieldStat fs;
        fs.add(std::int64_t{5});
        CHECK(fs.domain == FieldStatDomain::I64);
        fs.add(2.5);  // a real value: field is no longer integral
        CHECK(fs.domain == FieldStatDomain::F64);
        CHECK(fs.sum == doctest::Approx(7.5));
        // Mixed signedness also demotes.
        FieldStat mixed;
        mixed.add(std::int64_t{1});
        mixed.add(std::uint64_t{2});
        CHECK(mixed.domain == FieldStatDomain::F64);
    }

    TEST_CASE("merge preserves the exact integer domain") {
        const std::int64_t base = 1700000000000000001LL;
        FieldStat a, b;
        a.add(base);
        a.add(base + 6);
        b.add(base + 2);
        b.add(base + 4);
        a.merge(b);
        CHECK(a.domain == FieldStatDomain::I64);
        CHECK(a.n == 4);
        CHECK(a.emin == base);
        CHECK(a.emax == base + 6);
        CHECK(a.esum == 4 * base + 12);
    }

    TEST_CASE("merge of different domains demotes to F64") {
        FieldStat a, b;
        a.add(std::int64_t{10});
        b.add(2.5);  // F64
        a.merge(b);
        CHECK(a.domain == FieldStatDomain::F64);
        CHECK(a.n == 2);
        CHECK(a.sum == doctest::Approx(12.5));
    }

    TEST_CASE("merge into an empty stat adopts the other's domain") {
        FieldStat a, b;
        b.add(std::int64_t{7});
        a.merge(b);
        CHECK(a.domain == FieldStatDomain::I64);
        CHECK(a.esum == 7);
    }
}
