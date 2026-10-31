#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/filesystem.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/trace/internal/index_root.h>
#include <doctest/doctest.h>
#include <testing_utilities.h>

#include <string>
#include <system_error>

using namespace dftracer::utils;

TEST_SUITE("IndexRootStat") {
    TEST_CASE("a root found to exist is kept until it is forgotten") {
        const fs::path dir =
            dftu_utils_test::make_unique_test_path("test_index_root_stat");
        const std::string root = (dir / ".dftindex").string();
        fs::create_directories(dir);

        InputStatScope scope;
        CHECK_FALSE(input_exists(root));
        fs::create_directories(root);
        CHECK(input_exists(root));

        fs::remove_all(root);
        forget_input(root);
        CHECK_FALSE(input_exists(root));

        fs::create_directories(root);
        CHECK(input_exists(root));
        fs::remove_all(root);
        CHECK(input_exists(root));

        fs::remove_all(dir);
    }

    TEST_CASE("forget_input without a scope does nothing") {
        forget_input("/no/such/path/for/index_root_stat");
        CHECK_FALSE(input_exists("/no/such/path/for/index_root_stat"));
    }

    TEST_CASE("two spellings of one root share one IndexRoot") {
        const fs::path dir =
            dftu_utils_test::make_unique_test_path("test_index_root_spell");
        const fs::path other =
            dftu_utils_test::make_unique_test_path("test_index_root_other");
        fs::create_directories(dir);
        fs::create_directories(other);

        const auto a =
            trace::internal::index_root((dir / ".dftindex").string());
        const auto b =
            trace::internal::index_root((dir / "." / ".dftindex").string());
        CHECK(a == b);
        CHECK(a == trace::internal::index_root((dir / ".dftindex").string()));
        CHECK(a != trace::internal::index_root((other / ".dftindex").string()));

        const fs::path link =
            dir.parent_path() / (dir.filename().string() + "_ln");
        std::error_code ec;
        fs::create_directory_symlink(dir, link, ec);
        if (!ec) {
            CHECK(a ==
                  trace::internal::index_root((link / ".dftindex").string()));
            fs::remove(link);
        }

        fs::remove_all(dir);
        fs::remove_all(other);
    }
}
