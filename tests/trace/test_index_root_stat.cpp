// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/internal/index_root.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <system_error>

using namespace dftracer::utils;

TEST_SUITE("IndexRootStat") {
    TEST_CASE("a root found to exist is kept until it is forgotten") {
        const dstd::Path dir =
            dftu_utils_test::make_unique_test_path("test_index_root_stat");
        const dstd::String root = dstd::String((dir / ".dftindex").str());
        REQUIRE(dstd::fs::create_directories(dir).has_value());

        common::InputStatScope scope;
        CHECK_FALSE(common::input_exists(root));
        REQUIRE(dstd::fs::create_directories(root).has_value());
        CHECK(common::input_exists(root));

        (void)dstd::fs::remove_all(root);
        common::forget_input(root);
        CHECK_FALSE(common::input_exists(root));

        REQUIRE(dstd::fs::create_directories(root).has_value());
        CHECK(common::input_exists(root));
        (void)dstd::fs::remove_all(root);
        CHECK(common::input_exists(root));

        (void)dstd::fs::remove_all(dir);
    }

    TEST_CASE("forget_input without a scope does nothing") {
        common::forget_input("/no/such/path/for/index_root_stat");
        CHECK_FALSE(common::input_exists("/no/such/path/for/index_root_stat"));
    }

    TEST_CASE("two spellings of one root share one IndexRoot") {
        const dstd::Path dir =
            dftu_utils_test::make_unique_test_path("test_index_root_spell");
        const dstd::Path other =
            dftu_utils_test::make_unique_test_path("test_index_root_other");
        REQUIRE(dstd::fs::create_directories(dir).has_value());
        REQUIRE(dstd::fs::create_directories(other).has_value());

        const auto a = trace::internal::index_root(
            dstd::String(dstd::String((dir / ".dftindex").str())));
        const auto b = trace::internal::index_root(
            dstd::String(dstd::String((dir / "." / ".dftindex").str())));
        CHECK(a == b);
        CHECK(a == trace::internal::index_root(
                       dstd::String(dstd::String((dir / ".dftindex").str()))));
        CHECK(a != trace::internal::index_root(dstd::String(
                       dstd::String((other / ".dftindex").str()))));

        const dstd::Path link =
            dir.view().parent() / (dstd::String(dir.view().filename()) + "_ln");
        if (dstd::fs::create_symlink(dir, link).has_value()) {
            CHECK(a == trace::internal::index_root(dstd::String(
                           dstd::String((link / ".dftindex").str()))));
            (void)dstd::fs::remove(link);
        }

        (void)dstd::fs::remove_all(dir);
        (void)dstd::fs::remove_all(other);
    }
}
