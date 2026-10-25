#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/memory/manager.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>
#include <utility>

using namespace dftracer::utils;

namespace {

enum class TestError : std::int32_t {
    NotFound = 42,
    Parse = 7,
};

constexpr dstd::ErrorDomain TEST_ERROR_DOMAIN = dstd::make_error_domain("test");

constexpr dstd::ErrorCodeInfo TEST_ERROR_INFOS[] = {
    {&TEST_ERROR_DOMAIN, static_cast<std::int32_t>(TestError::NotFound),
     dstd::Condition::NotFound, "NOT_FOUND"},
    {&TEST_ERROR_DOMAIN, static_cast<std::int32_t>(TestError::Parse),
     dstd::Condition::Parse, "PARSE"},
};

constexpr const dstd::ErrorCodeInfo& error_info(TestError code) noexcept {
    switch (code) {
        case TestError::NotFound:
            return TEST_ERROR_INFOS[0];
        case TestError::Parse:
            return TEST_ERROR_INFOS[1];
    }
    return TEST_ERROR_INFOS[0];
}

memory::Pool& root() { return memory::Manager::get().root(); }

}  // namespace

static_assert(sizeof(dstd::Error) == 16);
static_assert(dstd::ErrorEnum<TestError>);
static_assert(dstd::ErrorEnum<dstd::Condition>);

TEST_CASE("Error carries domain code condition and name") {
    const dstd::Error err{TestError::NotFound};
    CHECK(err.code() == 42);
    CHECK(err.condition() == dstd::Condition::NotFound);
    CHECK(err.is(dstd::Condition::NotFound));
    CHECK(!err.is(dstd::Condition::Parse));
    CHECK(err.is_code(TestError::NotFound));
    CHECK(!err.is_code(TestError::Parse));
    CHECK(!err.is_code(dstd::Condition::NotFound));
    CHECK(err.in_domain(TEST_ERROR_DOMAIN));
    CHECK(!err.in_domain(dstd::CORE_ERROR_DOMAIN));
    CHECK(err.domain().name == "test");
    CHECK(err.domain().id == TEST_ERROR_DOMAIN.id);
    CHECK(err.name() == "NOT_FOUND");
    CHECK(err.message().empty());
}

TEST_CASE("a message is the only allocation and copies share it") {
    memory::PoolHandle pool = root().child("error_message");
    REQUIRE(pool);
    memory::Scope scope{*pool};
    const std::size_t base = root().live();
    {
        const dstd::Error plain{TestError::Parse};
        CHECK(root().live() == base);
        dstd::Error with_message{TestError::NotFound, "missing thing"};
        CHECK(root().live() > base);
        const void* message = with_message.message().data();
        dstd::Error copy = with_message;
        CHECK(copy.message().data() == message);
        CHECK(copy.message() == "missing thing");
        dstd::Error moved = std::move(with_message);
        CHECK(with_message.message().empty());
        CHECK(moved.message().data() == message);
        CHECK(moved.message() == "missing thing");
        moved = dstd::Error{TestError::Parse};
        CHECK(copy.message().data() == message);
    }
    CHECK(root().live() == base);
}

TEST_CASE("copy assignment drops the old message and shares the new one") {
    memory::PoolHandle pool = root().child("error_assign");
    REQUIRE(pool);
    memory::Scope scope{*pool};
    const std::size_t base = root().live();
    {
        dstd::Error target{TestError::Parse, "old text"};
        const dstd::Error source{TestError::NotFound, "new text"};
        target = source;
        CHECK(target.message() == "new text");
        CHECK(target.message().data() == source.message().data());
        CHECK(target.code() == 42);
        const dstd::Error& same = target;
        target = same;
        CHECK(target.message() == "new text");
    }
    CHECK(root().live() == base);
}

TEST_CASE("Error formats like the legacy error") {
    const dstd::Error err{TestError::NotFound, "missing thing"};
    CHECK(err.format() == "test#42 NOT_FOUND: missing thing");

    const dstd::Error core{dstd::Condition::Timeout, "late"};
    CHECK(core.format() == "dftracer.utils.core#7 TIMEOUT: late");
}

TEST_CASE("Condition is a core error enum") {
    const dstd::Error err{dstd::Condition::Timeout};
    CHECK(err.code() == static_cast<std::int32_t>(dstd::Condition::Timeout));
    CHECK(err.domain().id == dstd::CORE_ERROR_DOMAIN.id);
    CHECK(err.domain().name == "dftracer.utils.core");
    CHECK(err.name() == "TIMEOUT");
    CHECK(err.condition() == dstd::Condition::Timeout);
    CHECK(err.is(dstd::Condition::Timeout));
}

TEST_CASE("Expected holds an Error") {
    dstd::Expected<int, dstd::Error> result = dstd::Unexpected<dstd::Error>(
        dstd::Error{dstd::Condition::Unsupported});
    REQUIRE(!result.has_value());
    CHECK(result.error().is(dstd::Condition::Unsupported));
    CHECK(result.error().message().empty());
}
