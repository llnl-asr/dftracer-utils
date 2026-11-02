#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/either.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>

#include <utility>

using namespace dftracer::utils;

TEST_CASE("Either holds the side it was made with") {
    auto l = dstd::Either<int, dstd::String>::left(7);
    CHECK(l.is_left());
    CHECK_FALSE(l.is_right());
    CHECK(l.left() == 7);

    auto r = dstd::Either<int, dstd::String>::right(dstd::String("x"));
    CHECK(r.is_right());
    CHECK(r.right() == "x");
}

TEST_CASE("Either keeps the side when both sides have one type") {
    auto l = dstd::Either<int, int>::left(1);
    auto r = dstd::Either<int, int>::right(2);
    CHECK(l.is_left());
    CHECK(r.is_right());
    CHECK(l.left() == 1);
    CHECK(r.right() == 2);
}

TEST_CASE("Either fold calls the function of the held side") {
    auto l = dstd::Either<int, double>::left(3);
    auto r = dstd::Either<int, double>::right(1.5);
    const auto fold_value = [](auto&& e) {
        return e.fold([](int v) { return v * 10.0; },
                      [](double v) { return v; });
    };
    CHECK(fold_value(l) == doctest::Approx(30.0));
    CHECK(fold_value(r) == doctest::Approx(1.5));
}

TEST_CASE("Either moves a move-only value out") {
    auto e = dstd::Either<dstd::UniquePtr<int>, int>::left(
        dstd::make_unique<int>(5));
    dstd::UniquePtr<int> taken = std::move(e).left();
    REQUIRE(taken != nullptr);
    CHECK(*taken == 5);
}
