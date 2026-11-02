#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/duql/errc.h>
#include <doctest/doctest.h>

#include <string>

using namespace dftracer::utils;

TEST_CASE("an ErrorCode error keeps its code through the exception") {
    const DFTUtilsException e(ErrorCode::Io, "cannot open trace");
    const DFTUtilsException reader(ErrorCode::Reader, "short read");
    CHECK(e.condition() == dstd::Condition::Io);
    CHECK(e.domain() == dstd::CORE_ERROR_DOMAIN.id);
    CHECK(e.domain_name() == "dftracer.utils.core");
    CHECK(std::string(e.what()) == "cannot open trace");
    CHECK(e.error().is_code(ErrorCode::Io));
    CHECK(errorcode_of(e.error()) == ErrorCode::Io);
    CHECK(reader.condition() == dstd::Condition::Internal);
    CHECK(errorcode_of(reader.error()) == ErrorCode::Reader);
}

TEST_CASE("a Condition error maps to an ErrorCode by its condition") {
    const DFTUtilsException cancelled(
        dstd::Error{dstd::Condition::Cancelled, "stop"});
    CHECK(errorcode_of(cancelled.error()) == ErrorCode::Unknown);
    CHECK(cancelled.condition() == dstd::Condition::Cancelled);

    const DFTUtilsException io(dstd::Error{dstd::Condition::Io, "read"});
    CHECK(errorcode_of(io.error()) == ErrorCode::Io);
    CHECK(std::string(io.what()) == "read");
}

TEST_CASE("a duql error keeps its domain and maps by its condition") {
    const dstd::Error err{duql::DuqlErrc::Unsupported, "no columnar form"};
    CHECK(err.domain().name == "dftracer.utils.duql");
    const DFTUtilsException e(err);
    CHECK(e.domain() == err.domain().id);
    CHECK(errorcode_of(e.error()) == errorcode_of(err.condition()));
}

TEST_CASE("make_error carries the code and the message") {
    const Result<int> r = make_error(ErrorCode::Parse, "bad line");
    REQUIRE(!r);
    CHECK(r.error().is_code(ErrorCode::Parse));
    CHECK(r.error().message() == "bad line");
    CHECK(std::string(error_code_name(ErrorCode::Parse)) == "PARSE");
}
