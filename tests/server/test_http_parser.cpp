#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/server/http_parser.h>
#include <doctest/doctest.h>

#include <cstring>
#include <string>

using namespace dftracer::utils::server::parser;

// ============================================================================
// Valid GET requests
// ============================================================================

TEST_CASE("HttpParser - simple GET request") {
    std::string raw =
        "GET /api/v1/files HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    CHECK(consumed == static_cast<int>(raw.size()));
    CHECK(req.method == "GET");
    CHECK(req.path == "/api/v1/files");
    CHECK(req.minor_version == 1);
    REQUIRE(req.headers.size() == 1);
    CHECK(req.headers[0].name == "Host");
    CHECK(req.headers[0].value == "localhost");
}

TEST_CASE("HttpParser - GET with query string") {
    std::string raw =
        "GET /api/v1/events?limit=100&file=test.pfw.gz HTTP/1.1\r\n"
        "Host: localhost:8080\r\n"
        "Accept: application/json\r\n"
        "\r\n";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    CHECK(req.method == "GET");
    CHECK(req.path == "/api/v1/events?limit=100&file=test.pfw.gz");
    CHECK(req.minor_version == 1);
    REQUIRE(req.headers.size() == 2);
    CHECK(req.headers[0].name == "Host");
    CHECK(req.headers[0].value == "localhost:8080");
    CHECK(req.headers[1].name == "Accept");
    CHECK(req.headers[1].value == "application/json");
}

TEST_CASE("HttpParser - multiple headers") {
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "User-Agent: test/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    CHECK(req.method == "GET");
    CHECK(req.path == "/");
    REQUIRE(req.headers.size() == 4);
    CHECK(req.headers[0].name == "Host");
    CHECK(req.headers[1].name == "User-Agent");
    CHECK(req.headers[2].name == "Accept");
    CHECK(req.headers[3].name == "Connection");
    CHECK(req.headers[3].value == "keep-alive");
}

TEST_CASE("HttpParser - HTTP/1.0") {
    std::string raw =
        "GET /index.html HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "\r\n";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    CHECK(req.method == "GET");
    CHECK(req.path == "/index.html");
    CHECK(req.minor_version == 0);
}

TEST_CASE("HttpParser - no headers") {
    std::string raw =
        "GET /path HTTP/1.1\r\n"
        "\r\n";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    CHECK(req.method == "GET");
    CHECK(req.path == "/path");
    CHECK(req.headers.empty());
}

// ============================================================================
// POST requests
// ============================================================================

TEST_CASE("HttpParser - POST request with body indication") {
    std::string raw =
        "POST /api/v1/data HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello, World!";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    // Parser only parses headers, not body.
    // consumed should point to end of headers.
    REQUIRE(consumed > 0);
    CHECK(req.method == "POST");
    CHECK(req.path == "/api/v1/data");
    REQUIRE(req.headers.size() == 3);
    CHECK(req.headers[1].name == "Content-Length");
    CHECK(req.headers[1].value == "13");
}

// ============================================================================
// Bare LF line endings (tolerated)
// ============================================================================

TEST_CASE("HttpParser - bare LF line endings") {
    std::string raw =
        "GET /path HTTP/1.1\n"
        "Host: localhost\n"
        "\n";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    CHECK(req.method == "GET");
    CHECK(req.path == "/path");
    REQUIRE(req.headers.size() == 1);
    CHECK(req.headers[0].name == "Host");
}

// ============================================================================
// Header value trimming
// ============================================================================

TEST_CASE("HttpParser - header value with leading/trailing whitespace") {
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host:   localhost   \r\n"
        "\r\n";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    REQUIRE(req.headers.size() == 1);
    // Leading OWS after colon is consumed by parser, trailing
    // whitespace trimmed.
    CHECK(req.headers[0].value == "localhost");
}

// ============================================================================
// Incomplete requests (returns -2)
// ============================================================================

TEST_CASE("HttpParser - incomplete request (no terminating CRLF)") {
    std::string raw =
        "GET /api/v1/files HTTP/1.1\r\n"
        "Host: localhost\r\n";

    ParsedRequest req;
    int result = parse_request(raw.data(), raw.size(), req);
    CHECK(result == -2);
}

TEST_CASE("HttpParser - empty buffer") {
    ParsedRequest req;
    int result = parse_request("", 0, req);
    CHECK(result == -2);
}

TEST_CASE("HttpParser - partial request line") {
    std::string raw = "GET /path";

    ParsedRequest req;
    int result = parse_request(raw.data(), raw.size(), req);
    CHECK(result == -2);
}

// ============================================================================
// Malformed requests (returns -1)
// ============================================================================

TEST_CASE("HttpParser - missing HTTP version") {
    std::string raw =
        "GET /path\r\n"
        "\r\n";

    ParsedRequest req;
    int result = parse_request(raw.data(), raw.size(), req);
    CHECK(result == -1);
}

TEST_CASE("HttpParser - bad HTTP version string") {
    std::string raw =
        "GET /path HTPP/1.1\r\n"
        "\r\n";

    ParsedRequest req;
    int result = parse_request(raw.data(), raw.size(), req);
    CHECK(result == -1);
}

TEST_CASE("HttpParser - control character in path") {
    std::string raw =
        "GET /path\x01more HTTP/1.1\r\n"
        "\r\n";

    ParsedRequest req;
    int result = parse_request(raw.data(), raw.size(), req);
    CHECK(result == -1);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("HttpParser - leading CRLF before request") {
    // Some clients send a leading CRLF after POST body.
    // Parser should tolerate it.
    std::string raw =
        "\r\n"
        "GET /api HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    CHECK(req.method == "GET");
    CHECK(req.path == "/api");
}

TEST_CASE("HttpParser - path with encoded characters") {
    std::string raw =
        "GET /api/v1/files/info?file=%2Ftmp%2Ftest.pfw.gz HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    CHECK(req.method == "GET");
    // Parser does NOT decode percent-encoding; it preserves raw path.
    CHECK(req.path == "/api/v1/files/info?file=%2Ftmp%2Ftest.pfw.gz");
}

TEST_CASE("HttpParser - extra data after headers is not consumed") {
    std::string raw =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n"
        "extra data here";

    ParsedRequest req;
    int consumed = parse_request(raw.data(), raw.size(), req);

    REQUIRE(consumed > 0);
    // consumed should NOT include the extra data
    CHECK(consumed < static_cast<int>(raw.size()));
    CHECK(req.method == "GET");
}
