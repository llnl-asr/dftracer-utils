#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/ops.h>
#include <dftracer/utils/server/http_connection.h>
#include <dftracer/utils/server/http_request.h>
#include <dftracer/utils/server/http_response.h>
#include <dftracer/utils/server/router.h>

#include <cstring>

#ifdef __linux__
#include <malloc.h>  // malloc_trim
#endif

namespace dftracer::utils::server {

coro::CoroTask<void> handle_connection(int client_fd,
                                       struct sockaddr_in /*addr*/,
                                       Router& router) {
    // 8 KiB receive buffer. For HTTP/1.1 GET requests this is plenty.
    // Requests larger than this are rejected as "too large".
    constexpr std::size_t BUF_SIZE = 8192;
    char buf[BUF_SIZE];
    std::size_t buf_used = 0;

    while (true) {
        // Read data from socket.
        ssize_t n = co_await io::recv(client_fd, buf + buf_used,
                                      BUF_SIZE - buf_used, 0);
        if (n <= 0) break;  // Connection closed or error
        buf_used += static_cast<std::size_t>(n);

        // Try to parse a complete request.
        HttpRequest req;
        int parsed = req.parse(buf, buf_used);
        if (parsed == -2) {
            // Incomplete — need more data.
            if (buf_used >= BUF_SIZE) {
                // Buffer full but still no complete request.
                auto resp = HttpResponse::bad_request("Request too large");
                auto out = resp.serialize();
                co_await io::send(client_fd, out.data(), out.size(), 0);
                break;
            }
            continue;
        }
        if (parsed < 0) {
            auto resp = HttpResponse::bad_request("Malformed HTTP request");
            auto out = resp.serialize();
            co_await io::send(client_fd, out.data(), out.size(), 0);
            break;
        }

        // Route and handle.
        HttpResponse resp;
        try {
            resp = co_await router.handle(req);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR("Handler exception: %s", e.what());
            resp = HttpResponse::internal_error(e.what());
        } catch (...) {
            DFTRACER_UTILS_LOG_ERROR("Handler threw unknown exception");
            resp = HttpResponse::internal_error("Internal server error");
        }
        auto out = resp.serialize();
        co_await io::send(client_fd, out.data(), out.size(), 0);

        // Consume parsed bytes; shift any remaining data.
        auto consumed = static_cast<std::size_t>(parsed);
        if (consumed < buf_used) {
            std::memmove(buf, buf + consumed, buf_used - consumed);
            buf_used -= consumed;
        } else {
            buf_used = 0;
        }

        // Check Connection: close or HTTP/1.0 (no keep-alive).
        if (req.minor_version == 0 || req.has_header("connection", "close")) {
            break;
        }
    }

    // Note: the caller (tcp_listener) is responsible for closing
    // client_fd via io::close() after this coroutine returns.

#ifdef __linux__
    // Return freed heap pages to the OS.  Each request may decompress
    // large gzip buffers and build sizeable JSON responses; without
    // this call glibc's ptmalloc2 keeps those arenas mapped, causing
    // RSS to grow monotonically.
    ::malloc_trim(0);
#endif
}

}  // namespace dftracer::utils::server