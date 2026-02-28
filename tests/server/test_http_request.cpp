#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/server/http_request.h>
#include <doctest/doctest.h>

#include <string>

using namespace dftracer::utils::server;

// ============================================================================
// Parsing (delegates to http_parser internally)
// ============================================================================

TEST_CASE("HttpRequest - parse valid GET") {
    std::string raw =
        "GET /api/v1/files HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Accept: application/json\r\n"
        "\r\n";

    HttpRequest req;
    int consumed = req.parse(raw.data(), raw.size());

    REQUIRE(consumed > 0);
    CHECK(req.method == "GET");
    CHECK(req.path == "/api/v1/files");
    CHECK(req.minor_version == 1);
    REQUIRE(req.headers.size() == 2);
}

TEST_CASE("HttpRequest - parse incomplete returns -2") {
    std::string raw = "GET /path HTTP/1.1\r\n";

    HttpRequest req;
    int result = req.parse(raw.data(), raw.size());
    CHECK(result == -2);
}

TEST_CASE("HttpRequest - parse malformed returns -1") {
    std::string raw =
        "INVALID\r\n"
        "\r\n";

    HttpRequest req;
    int result = req.parse(raw.data(), raw.size());
    CHECK(result == -1);
}

// ============================================================================
// Header lookup (case-insensitive)
// ============================================================================

TEST_CASE("HttpRequest - header lookup case-insensitive") {
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Content-Type: text/html\r\n"
        "X-Custom-Header: custom-value\r\n"
        "\r\n";

    HttpRequest req;
    int consumed = req.parse(raw.data(), raw.size());
    REQUIRE(consumed > 0);

    SUBCASE("exact case match") {
        CHECK(req.header("Host") == "example.com");
        CHECK(req.header("Content-Type") == "text/html");
    }

    SUBCASE("different case") {
        CHECK(req.header("host") == "example.com");
        CHECK(req.header("HOST") == "example.com");
        CHECK(req.header("content-type") == "text/html");
    }

    SUBCASE("missing header returns empty") {
        CHECK(req.header("Missing").empty());
    }
}

// ============================================================================
// has_header (case-insensitive name + value)
// ============================================================================

TEST_CASE("HttpRequest - has_header") {
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    HttpRequest req;
    int consumed = req.parse(raw.data(), raw.size());
    REQUIRE(consumed > 0);

    SUBCASE("exact match") {
        CHECK(req.has_header("Connection", "keep-alive"));
    }

    SUBCASE("case-insensitive name") {
        CHECK(req.has_header("connection", "keep-alive"));
    }

    SUBCASE("case-insensitive value") {
        CHECK(req.has_header("Connection", "Keep-Alive"));
    }

    SUBCASE("wrong value") {
        CHECK_FALSE(req.has_header("Connection", "close"));
    }

    SUBCASE("missing header") {
        CHECK_FALSE(req.has_header("Accept", "text/html"));
    }
}
