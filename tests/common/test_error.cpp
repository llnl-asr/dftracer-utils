// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/abi.h>
#include <dftracer/utils/common/abi_error.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/memory_config.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/errc.h>
#include <doctest/doctest.h>

#include <exception>
#include <stdexcept>

using namespace dftracer::utils;

TEST_CASE("an ErrorCode error keeps its code and domain") {
    const dstd::Error e(common::ErrorCode::Io, "cannot open trace");
    const dstd::Error reader(common::ErrorCode::Reader, "short read");
    CHECK(e.condition() == dstd::Condition::Io);
    CHECK(e.domain().id == dstd::CORE_ERROR_DOMAIN.id);
    CHECK(e.domain().name == "dftracer.utils.core");
    CHECK(dstd::String(e.c_str()) == "cannot open trace");
    CHECK(e.is_code(common::ErrorCode::Io));
    CHECK(common::errorcode_of(e) == common::ErrorCode::Io);
    CHECK(reader.condition() == dstd::Condition::Internal);
    CHECK(common::errorcode_of(reader) == common::ErrorCode::Reader);
}

TEST_CASE("a Condition error maps to an ErrorCode by its condition") {
    const dstd::Error cancelled{dstd::Condition::Cancelled, "stop"};
    CHECK(common::errorcode_of(cancelled) == common::ErrorCode::Unknown);
    CHECK(cancelled.condition() == dstd::Condition::Cancelled);

    const dstd::Error io{dstd::Condition::Io, "read"};
    CHECK(common::errorcode_of(io) == common::ErrorCode::Io);
    CHECK(dstd::String(io.c_str()) == "read");
}

TEST_CASE("a duql error keeps its domain and maps by its condition") {
    const dstd::Error err{duql::DuqlErrc::Unsupported, "no columnar form"};
    CHECK(err.domain().name == "dftracer.utils.duql");
    CHECK(common::errorcode_of(err) == common::errorcode_of(err.condition()));
}

TEST_CASE("make_error carries the code and the message") {
    const dstd::Result<int> r =
        common::make_error(common::ErrorCode::Parse, "bad line");
    REQUIRE(!r);
    CHECK(r.error().is_code(common::ErrorCode::Parse));
    CHECK(r.error().message() == "bad line");
    CHECK(dstd::String(common::error_code_name(common::ErrorCode::Parse)) ==
          "PARSE");
}

TEST_CASE("error_of_exception reads a std exception as an Unknown error") {
    const dstd::Error err = common::error_of_exception(
        std::make_exception_ptr(std::runtime_error("disk gone")));
    CHECK(err.condition() == dstd::Condition::Unknown);
    CHECK(dstd::String(err.c_str()) == "disk gone");
    CHECK(common::errorcode_of(err) == common::ErrorCode::Unknown);
}

TEST_CASE("error_of_exception gives a fixed text for a non-std exception") {
    const dstd::Error err =
        common::error_of_exception(std::make_exception_ptr(7));
    CHECK(err.condition() == dstd::Condition::Unknown);
    CHECK(dstd::String(err.c_str()) == "unknown error");
}

TEST_CASE("a refused reservation reaches the C ABI as DFTU_E_NO_MEMORY") {
    for (const memory::Status status :
         {memory::Status::OverBudget, memory::Status::OverCap,
          memory::Status::Pending}) {
        const dstd::Error error(common::error_code_of(status),
                                "reservation refused");
        CHECK(error.condition() == dstd::Condition::OutOfMemory);
        CHECK(common::to_status(error.condition()) == DFTU_E_NO_MEMORY);

        dftu_error out = DFTU_ERROR_INIT;
        CHECK(common::fill_error(&out, error) == DFTU_E_NO_MEMORY);
        CHECK(out.status == DFTU_E_NO_MEMORY);
        REQUIRE(out.message != nullptr);
        CHECK(dstd::String(out.message) == "reservation refused");
        dftu_error_release(&out);
    }
    CHECK(common::to_condition(DFTU_E_NO_MEMORY) ==
          dstd::Condition::OutOfMemory);
}
