#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/server/router.h>
#include <doctest/doctest.h>

#include <string>

using namespace dftracer::utils::server;

// ============================================================================
// Parsing
// ============================================================================

TEST_CASE("QueryParams - parse simple key=value pairs") {
    auto params = QueryParams::parse("limit=100&offset=50");

    CHECK(params.has("limit"));
    CHECK(params.has("offset"));
    CHECK(params.get("limit") == "100");
    CHECK(params.get("offset") == "50");
}

TEST_CASE("QueryParams - parse single parameter") {
    auto params = QueryParams::parse("file=test.pfw.gz");

    CHECK(params.has("file"));
    CHECK(params.get("file") == "test.pfw.gz");
}

TEST_CASE("QueryParams - parse empty query string") {
    auto params = QueryParams::parse("");

    CHECK_FALSE(params.has("anything"));
}

TEST_CASE("QueryParams - parse key without value") {
    auto params = QueryParams::parse("verbose");

    CHECK(params.has("verbose"));
    CHECK(params.get("verbose") == "");
}

TEST_CASE("QueryParams - parse multiple keys without values") {
    auto params = QueryParams::parse("a&b&c");

    CHECK(params.has("a"));
    CHECK(params.has("b"));
    CHECK(params.has("c"));
}

TEST_CASE("QueryParams - parse key with empty value") {
    auto params = QueryParams::parse("key=");

    CHECK(params.has("key"));
    CHECK(params.get("key") == "");
}

// ============================================================================
// URL decoding
// ============================================================================

TEST_CASE("QueryParams - percent-encoded values") {
    auto params = QueryParams::parse("file=%2Ftmp%2Ftest.pfw.gz");

    CHECK(params.get("file") == "/tmp/test.pfw.gz");
}

TEST_CASE("QueryParams - plus decoded as space") {
    auto params = QueryParams::parse("q=hello+world");

    CHECK(params.get("q") == "hello world");
}

TEST_CASE("QueryParams - percent-encoded key") {
    auto params = QueryParams::parse("my%20key=value");

    CHECK(params.has("my key"));
    CHECK(params.get("my key") == "value");
}

// ============================================================================
// Default values
// ============================================================================

TEST_CASE("QueryParams - get with default value") {
    auto params = QueryParams::parse("a=1");

    CHECK(params.get("a", "default") == "1");
    CHECK(params.get("missing", "default") == "default");
}

TEST_CASE("QueryParams - has returns false for missing key") {
    auto params = QueryParams::parse("a=1");

    CHECK_FALSE(params.has("b"));
}

// ============================================================================
// Typed getters
// ============================================================================

TEST_CASE("QueryParams - get_int") {
    auto params = QueryParams::parse("limit=42&offset=-5&bad=abc");

    CHECK(params.get_int("limit") == 42);
    CHECK(params.get_int("offset") == -5);
    CHECK(params.get_int("missing") == 0);
    CHECK(params.get_int("missing", 10) == 10);
}

TEST_CASE("QueryParams - get_int with non-numeric value") {
    auto params = QueryParams::parse("val=abc");

    // from_chars fails, returns default
    CHECK(params.get_int("val", 99) == 99);
}

TEST_CASE("QueryParams - get_double") {
    auto params = QueryParams::parse("begin=1.5&end=3.14");

    CHECK(params.get_double("begin") == doctest::Approx(1.5));
    CHECK(params.get_double("end") == doctest::Approx(3.14));
    CHECK(params.get_double("missing") == doctest::Approx(0.0));
    CHECK(params.get_double("missing", 2.5) == doctest::Approx(2.5));
}

TEST_CASE("QueryParams - get_double with non-numeric value") {
    auto params = QueryParams::parse("val=notanumber");

    CHECK(params.get_double("val", 7.7) == doctest::Approx(7.7));
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("QueryParams - multiple values for same key returns first") {
    auto params = QueryParams::parse("key=first&key=second");

    CHECK(params.get("key") == "first");
}

// ============================================================================
// Canonical key (result-cache key)
// ============================================================================

TEST_CASE("QueryParams - canonical_key is order-independent") {
    auto a = QueryParams::parse("begin=1&end=2&query=x");
    auto b = QueryParams::parse("query=x&end=2&begin=1");

    CHECK(a.canonical_key() == b.canonical_key());
}

TEST_CASE("QueryParams - canonical_key distinguishes different params") {
    auto a = QueryParams::parse("begin=1&end=2");
    auto b = QueryParams::parse("begin=1&end=3");

    CHECK(a.canonical_key() != b.canonical_key());
}

TEST_CASE("QueryParams - canonical_key distinguishes value vs no-value") {
    auto a = QueryParams::parse("flag=1");
    auto b = QueryParams::parse("flag");

    CHECK(a.canonical_key() != b.canonical_key());
}

TEST_CASE("QueryParams - canonical_key empty for empty query") {
    auto params = QueryParams::parse("");

    CHECK(params.canonical_key().empty());
}

TEST_CASE("QueryParams - complex real-world query string") {
    auto params = QueryParams::parse(
        "file=%2Ftmp%2Fdata.pfw.gz&limit=1000&begin=0.0"
        "&end=100.0&summary=true&category=POSIX");

    CHECK(params.get("file") == "/tmp/data.pfw.gz");
    CHECK(params.get_int("limit") == 1000);
    CHECK(params.get_double("begin") == doctest::Approx(0.0));
    CHECK(params.get_double("end") == doctest::Approx(100.0));
    CHECK(params.get("summary") == "true");
    CHECK(params.get("category") == "POSIX");
}
