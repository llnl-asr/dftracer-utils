#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/duql/errc.h>
#include <doctest/doctest.h>

#include <string>

using namespace dftracer::utils;

TEST_CASE("an ErrorCode error keeps its code through the exception") {
    const common::DFTUtilsException e(common::ErrorCode::Io,
                                      "cannot open trace");
    const common::DFTUtilsException reader(common::ErrorCode::Reader,
                                           "short read");
    CHECK(e.condition() == dstd::Condition::Io);
    CHECK(e.domain() == dstd::CORE_ERROR_DOMAIN.id);
    CHECK(e.domain_name() == "dftracer.utils.core");
    CHECK(std::string(e.what()) == "cannot open trace");
    CHECK(e.error().is_code(common::ErrorCode::Io));
    CHECK(common::errorcode_of(e.error()) == common::ErrorCode::Io);
    CHECK(reader.condition() == dstd::Condition::Internal);
    CHECK(common::errorcode_of(reader.error()) == common::ErrorCode::Reader);
}

TEST_CASE("a Condition error maps to an ErrorCode by its condition") {
    const common::DFTUtilsException cancelled(
        dstd::Error{dstd::Condition::Cancelled, "stop"});
    CHECK(common::errorcode_of(cancelled.error()) ==
          common::ErrorCode::Unknown);
    CHECK(cancelled.condition() == dstd::Condition::Cancelled);

    const common::DFTUtilsException io(
        dstd::Error{dstd::Condition::Io, "read"});
    CHECK(common::errorcode_of(io.error()) == common::ErrorCode::Io);
    CHECK(std::string(io.what()) == "read");
}

TEST_CASE("a duql error keeps its domain and maps by its condition") {
    const dstd::Error err{duql::DuqlErrc::Unsupported, "no columnar form"};
    CHECK(err.domain().name == "dftracer.utils.duql");
    const common::DFTUtilsException e(err);
    CHECK(e.domain() == err.domain().id);
    CHECK(common::errorcode_of(e.error()) ==
          common::errorcode_of(err.condition()));
}

TEST_CASE("make_error carries the code and the message") {
    const common::Result<int> r =
        common::make_error(common::ErrorCode::Parse, "bad line");
    REQUIRE(!r);
    CHECK(r.error().is_code(common::ErrorCode::Parse));
    CHECK(r.error().message() == "bad line");
    CHECK(std::string(common::error_code_name(common::ErrorCode::Parse)) ==
          "PARSE");
}
