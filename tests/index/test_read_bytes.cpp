#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/index/store/database.h>
#include <doctest/doctest.h>
#include <sys/resource.h>

#include <cstdint>

using dftracer::utils::index::store::RocksDatabase;

namespace {

std::int64_t minor_faults() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<std::int64_t>(usage.ru_minflt);
}

}  // namespace

TEST_SUITE("IndexReadBytes") {
    TEST_CASE("building store options does not build a block cache per call") {
        constexpr int CALLS = 2000;
        // Warm the process-wide objects first.
        (void)RocksDatabase::default_options();
        (void)RocksDatabase::default_column_family_options();
        (void)RocksDatabase::point_lookup_column_family_options();

        const auto faults = minor_faults();
        for (int i = 0; i < CALLS; ++i) {
            auto options = RocksDatabase::default_options();
            auto cf = RocksDatabase::default_column_family_options();
            auto point = RocksDatabase::point_lookup_column_family_options();
            CHECK(cf.table_factory != nullptr);
            CHECK(point.table_factory != nullptr);
            (void)options;
        }
        // A default-built table factory makes its own block cache, a fresh
        // mapping that faults in and is freed with it: one fault per call.
        const auto per_call =
            static_cast<double>(minor_faults() - faults) / (CALLS * 3);
        MESSAGE("minor faults per options call: " << per_call);
        CHECK(per_call < 0.1);
    }
}
