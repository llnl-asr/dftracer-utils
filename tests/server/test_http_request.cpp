// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/domains/server/http_request.h>
#include <dftracer/utils/dstd/string.h>
#include <doctest/doctest.h>

using namespace dftracer::utils;

using namespace dftracer::utils::domains::server;

// ============================================================================
// Parsing (delegates to http_parser internally)
// ============================================================================

TEST_CASE("HttpRequest - parse valid GET") {
    dstd::String raw =
        "GET /api/files HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Accept: application/json\r\n"
        "\r\n";

    HttpRequest req;
    int consumed = req.parse(raw.data(), raw.size());

    REQUIRE(consumed > 0);
    CHECK(req.method == "GET");
    CHECK(req.path == "/api/files");
    CHECK(req.minor_version == 1);
    REQUIRE(req.headers.size() == 2);
}

TEST_CASE("HttpRequest - parse incomplete returns -2") {
    dstd::String raw = "GET /path HTTP/1.1\r\n";

    HttpRequest req;
    int result = req.parse(raw.data(), raw.size());
    CHECK(result == -2);
}

TEST_CASE("HttpRequest - parse malformed returns -1") {
    dstd::String raw =
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
    dstd::String raw =
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
    dstd::String raw =
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
