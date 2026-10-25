#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/runtime/coro/coro.h>
#include <dftracer/utils/runtime/runtime.h>
#include <doctest/doctest.h>

#include <memory>
#include <stdexcept>

using namespace dftracer::utils;

namespace {

// The thrown object owns `token`, so the token expires exactly when the last
// copy of the exception is destroyed.
struct TokenError : std::runtime_error {
    std::shared_ptr<int> token;
    explicit TokenError(std::shared_ptr<int> t)
        : std::runtime_error("task failed"), token(std::move(t)) {}
};

coro::CoroTask<int> fail_int(std::shared_ptr<int> token) {
    throw TokenError(std::move(token));
    co_return 0;
}

coro::CoroTask<void> fail_void(std::shared_ptr<int> token) {
    throw TokenError(std::move(token));
    co_return;
}

}  // namespace

TEST_CASE("a failed typed task frees its exception once the caller drops it") {
    Runtime rt;
    std::weak_ptr<int> watch;
    {
        auto token = std::make_shared<int>(1);
        watch = token;
        auto handle = rt.submit(fail_int(std::move(token)));
        CHECK_THROWS_AS(handle.get(), TokenError);
    }
    rt.shutdown();
    CHECK(watch.expired());
}

TEST_CASE("a failed untyped task frees its exception at shutdown") {
    Runtime rt;
    std::weak_ptr<int> watch;
    {
        auto token = std::make_shared<int>(1);
        watch = token;
        auto handle = rt.submit(fail_void(std::move(token)));
        CHECK_THROWS_AS(handle.get(), TokenError);
    }
    rt.shutdown();
    CHECK(watch.expired());
}
