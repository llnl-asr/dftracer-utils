// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/domains/server/http_connection.h>
#include <dftracer/utils/domains/server/http_request.h>
#include <dftracer/utils/domains/server/http_response.h>
#include <dftracer/utils/domains/server/router.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <doctest/doctest.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <testing_runtime.h>
#include <unistd.h>

using namespace dftracer::utils;
using namespace dftracer::utils::domains::server;

namespace {

std::size_t count_of(dstd::StringView text, dstd::StringView needle) {
    std::size_t n = 0;
    for (std::size_t pos = text.find(needle); pos != dstd::StringView::npos;
         pos = text.find(needle, pos + needle.size())) {
        ++n;
    }
    return n;
}

// Sends the pipelined bytes at once, runs the connection to its end and gives
// everything the server wrote back.
dstd::String serve(dstd::StringView pipelined) {
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    // Both ends hold the whole input, so the write below never blocks.
    const int buffer_size = 1 << 20;
    ::setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &buffer_size,
                 sizeof(buffer_size));
    ::setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &buffer_size,
                 sizeof(buffer_size));

    Router router;
    router.get("/a",
               [](const HttpRequest&,
                  const QueryParams&) -> coro::CoroTask<HttpResponse> {
                   co_return HttpResponse::ok("a");
               });

    REQUIRE(::write(fds[1], pipelined.data(), pipelined.size()) ==
            static_cast<ssize_t>(pipelined.size()));
    ::shutdown(fds[1], SHUT_WR);

    struct sockaddr_in addr{};
    dftu_utils_test::run_here(handle_connection(fds[0], addr, router));
    ::close(fds[0]);

    dstd::String out;
    char chunk[4096];
    ssize_t n;
    while ((n = ::read(fds[1], chunk, sizeof(chunk))) > 0) {
        out.append(chunk, static_cast<std::size_t>(n));
    }
    ::close(fds[1]);
    return out;
}

// The receive buffer holds 8 KiB, so the filler keeps the first read short of
// the whole input: the server reads the rest only if it keeps the connection.
dstd::String pipelined_with_filler(dstd::StringView first) {
    dstd::String all(first);
    all += "GET /a?padding=0123456789 HTTP/1.1\r\nHost: h\r\n\r\n";
    all += "GET /a HTTP/1.1\r\nX-Pad: ";
    all.append(9000, 'x');
    all += "\r\n\r\n";
    return all;
}

}  // namespace

TEST_CASE("Connection: close ends the connection after the first response") {
    auto out = serve(pipelined_with_filler(
        "GET /a HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n"));
    CHECK(count_of(out, "HTTP/1.1 200") == 1);
}

TEST_CASE("pipelined keep-alive requests are served") {
    auto out =
        serve(pipelined_with_filler("GET /a HTTP/1.1\r\nHost: h\r\n\r\n"));
    CHECK(count_of(out, "HTTP/1.1 200") == 2);
}

TEST_CASE("two keep-alive requests in one write are both answered") {
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    Router router;
    router.get("/a",
               [](const HttpRequest&,
                  const QueryParams&) -> coro::CoroTask<HttpResponse> {
                   co_return HttpResponse::ok("a");
               });

    // The client keeps its write side open until it has both responses, so a
    // server that waits for more input before it parses the second request
    // leaves the client to time out.
    struct timeval timeout{};
    timeout.tv_sec = 3;
    REQUIRE(::setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout)) == 0);

    dstd::String out;
    dstd::Thread client([&] {
        const dstd::StringView both =
            "GET /a HTTP/1.1\r\nHost: h\r\n\r\n"
            "GET /a HTTP/1.1\r\nHost: h\r\n\r\n";
        if (::write(fds[1], both.data(), both.size()) !=
            static_cast<ssize_t>(both.size()))
            return;
        char chunk[4096];
        while (count_of(out, "HTTP/1.1 200") < 2) {
            const ssize_t n = ::read(fds[1], chunk, sizeof(chunk));
            if (n <= 0) break;
            out.append(chunk, static_cast<std::size_t>(n));
        }
        ::shutdown(fds[1], SHUT_WR);
    });

    struct sockaddr_in addr{};
    dftu_utils_test::run_here(handle_connection(fds[0], addr, router));
    client.join();
    ::close(fds[0]);
    ::close(fds[1]);
    CHECK(count_of(out, "HTTP/1.1 200") == 2);
}
