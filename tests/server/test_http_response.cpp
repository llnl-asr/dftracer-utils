#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/server/http_response.h>
#include <doctest/doctest.h>

#include <string>

using namespace dftracer::utils::server;

// ============================================================================
// Factory methods
// ============================================================================

TEST_CASE("HttpResponse - ok() empty body") {
    auto resp = HttpResponse::ok();

    CHECK(resp.status_code == 200);
    CHECK(resp.status_text == "OK");
    CHECK(resp.body.empty());
    CHECK(resp.headers.empty());
}

TEST_CASE("HttpResponse - ok() with JSON body") {
    auto resp = HttpResponse::ok("{\"count\":42}", "application/json");

    CHECK(resp.status_code == 200);
    CHECK(resp.status_text == "OK");
    CHECK(resp.body == "{\"count\":42}");
    REQUIRE(resp.headers.size() == 1);
    CHECK(resp.headers[0].first == "Content-Type");
    CHECK(resp.headers[0].second == "application/json");
}

TEST_CASE("HttpResponse - ok() with custom content type") {
    auto resp = HttpResponse::ok("hello", "text/plain");

    CHECK(resp.status_code == 200);
    REQUIRE(resp.headers.size() == 1);
    CHECK(resp.headers[0].second == "text/plain");
}

TEST_CASE("HttpResponse - not_found()") {
    auto resp = HttpResponse::not_found();

    CHECK(resp.status_code == 404);
    CHECK(resp.status_text == "Not Found");
    CHECK(resp.body == "Not Found");
    REQUIRE(resp.headers.size() == 1);
    CHECK(resp.headers[0].first == "Content-Type");
    CHECK(resp.headers[0].second == "text/plain");
}

TEST_CASE("HttpResponse - bad_request()") {
    auto resp = HttpResponse::bad_request("missing 'file' parameter");

    CHECK(resp.status_code == 400);
    CHECK(resp.status_text == "Bad Request");
    CHECK(resp.body == "missing 'file' parameter");
    REQUIRE(resp.headers.size() == 1);
    CHECK(resp.headers[0].second == "text/plain");
}

TEST_CASE("HttpResponse - internal_error()") {
    auto resp = HttpResponse::internal_error("something went wrong");

    CHECK(resp.status_code == 500);
    CHECK(resp.status_text == "Internal Server Error");
    CHECK(resp.body == "something went wrong");
    REQUIRE(resp.headers.size() == 1);
    CHECK(resp.headers[0].second == "text/plain");
}

// ============================================================================
// Serialization
// ============================================================================

TEST_CASE("HttpResponse - serialize_headers adds Content-Length") {
    auto resp = HttpResponse::ok("{}", "application/json");
    auto headers = resp.serialize_headers();

    CHECK(headers.find("HTTP/1.1 200 OK\r\n") != std::string::npos);
    CHECK(headers.find("Content-Type: application/json\r\n") !=
          std::string::npos);
    CHECK(headers.find("Content-Length: 2\r\n") != std::string::npos);
    // Ends with blank line
    CHECK(headers.size() >= 4);
    CHECK(headers.substr(headers.size() - 2) == "\r\n");
}

TEST_CASE(
    "HttpResponse - serialize_headers omits Content-Length for empty "
    "body") {
    auto resp = HttpResponse::ok();
    auto headers = resp.serialize_headers();

    CHECK(headers.find("Content-Length") == std::string::npos);
}

TEST_CASE(
    "HttpResponse - serialize_headers does not duplicate Content-Length") {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers = {{"Content-Length", "5"}};
    resp.body = "hello";

    auto headers = resp.serialize_headers();

    // Should only have one Content-Length header
    auto first = headers.find("Content-Length");
    auto second = headers.find("Content-Length", first + 1);
    CHECK(first != std::string::npos);
    CHECK(second == std::string::npos);
}

TEST_CASE("HttpResponse - full serialize") {
    auto resp = HttpResponse::ok("test body", "text/plain");
    auto full = resp.serialize();

    CHECK(full.find("HTTP/1.1 200 OK\r\n") == 0);
    CHECK(full.find("Content-Type: text/plain\r\n") != std::string::npos);
    CHECK(full.find("Content-Length: 9\r\n") != std::string::npos);
    // Body comes after \r\n\r\n
    auto body_start = full.find("\r\n\r\n");
    REQUIRE(body_start != std::string::npos);
    CHECK(full.substr(body_start + 4) == "test body");
}

TEST_CASE("HttpResponse - serialize 404") {
    auto resp = HttpResponse::not_found();
    auto full = resp.serialize();

    CHECK(full.find("HTTP/1.1 404 Not Found\r\n") == 0);
    auto body_start = full.find("\r\n\r\n");
    REQUIRE(body_start != std::string::npos);
    CHECK(full.substr(body_start + 4) == "Not Found");
}

// ============================================================================
// Custom response construction
// ============================================================================

TEST_CASE("HttpResponse - custom headers") {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = "OK";
    resp.headers = {
        {"Content-Type", "application/x-ndjson"},
        {"Transfer-Encoding", "chunked"},
        {"X-Custom", "value"},
    };
    resp.body = "";

    auto headers = resp.serialize_headers();
    CHECK(headers.find("Content-Type: application/x-ndjson\r\n") !=
          std::string::npos);
    CHECK(headers.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
    CHECK(headers.find("X-Custom: value\r\n") != std::string::npos);
    // No Content-Length since body is empty
    CHECK(headers.find("Content-Length") == std::string::npos);
}
