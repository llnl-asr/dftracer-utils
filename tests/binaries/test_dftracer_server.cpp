#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <arpa/inet.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <doctest/doctest.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <testing_utilities.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ============================================================================
// Helpers
// ============================================================================

namespace {

int valgrind_timeout_scale() {
#ifdef DFTRACER_UTILS_VALGRIND_MODE
    return 20;
#else
    return 1;
#endif
}

/// Create a DFTracer gzip file with .pfw.gz extension
/// (the server scans for .pfw and .pfw.gz patterns).
std::string create_pfw_gz(dft_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

/// Find the dftracer_server binary. Checks DFTRACER_SERVER_PATH env first,
/// then common build paths relative to the test binary.
std::string find_server_binary() {
    const char* env_path = std::getenv("DFTRACER_SERVER_PATH");
    if (env_path != nullptr && ::access(env_path, X_OK) == 0) {
        return env_path;
    }

    std::vector<std::string> candidates = {
        "./dftracer_server",         "../dftracer_server",
        "../../dftracer_server",     "../bin/dftracer_server",
        "../../bin/dftracer_server",
    };

    for (const auto& path : candidates) {
        if (::access(path.c_str(), X_OK) == 0) {
            return path;
        }
    }

    return "";
}

/// Check if a TCP port is accepting connections.
bool port_is_listening(int port, int timeout_ms = 100) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    struct timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int result = ::connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                           sizeof(addr));
    ::close(sock);
    return result == 0;
}

bool can_bind_local_tcp_socket() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    int opt = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    const int rc =
        ::bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    ::close(sock);
    return rc == 0;
}

/// Wait until port is listening or timeout expires.
bool wait_for_port(int port, int timeout_s = 10) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_s * valgrind_timeout_scale());
    while (std::chrono::steady_clock::now() < deadline) {
        if (port_is_listening(port)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

/// Send a raw HTTP request and receive the response.
std::string http_request(int port, const std::string& request,
                         int recv_timeout_s = 15) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        ::close(sock);
        return "";
    }

    ssize_t sent = ::send(sock, request.data(), request.size(), 0);
    if (sent < 0) {
        ::close(sock);
        return "";
    }

    struct timeval tv{};
    tv.tv_sec = recv_timeout_s * valgrind_timeout_scale();
    tv.tv_usec = 0;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, static_cast<std::size_t>(n));

        // Check if we have a complete response (headers + full body)
        auto hdr_end = response.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            auto cl_pos = response.find("Content-Length: ");
            if (cl_pos != std::string::npos) {
                auto cl_end = response.find("\r\n", cl_pos);
                auto cl_str =
                    response.substr(cl_pos + 16, cl_end - cl_pos - 16);
                auto content_length =
                    static_cast<std::size_t>(std::atoi(cl_str.c_str()));
                auto body_start = hdr_end + 4;
                if (response.size() >= body_start + content_length) break;
            }
        }
    }

    ::close(sock);
    return response;
}

/// Extract HTTP status code from response.
int extract_status_code(const std::string& response) {
    auto space = response.find(' ');
    if (space == std::string::npos) return -1;
    return std::atoi(response.c_str() + space + 1);
}

/// Extract body from HTTP response.
std::string extract_body(const std::string& response) {
    auto pos = response.find("\r\n\r\n");
    if (pos == std::string::npos) return "";
    std::string raw = response.substr(pos + 4);

    // Decode chunked transfer encoding if present
    if (response.find("Transfer-Encoding: chunked") != std::string::npos) {
        std::string decoded;
        std::size_t i = 0;
        while (i < raw.size()) {
            auto crlf = raw.find("\r\n", i);
            if (crlf == std::string::npos) break;
            std::size_t chunk_size = 0;
            try {
                chunk_size = std::stoul(raw.substr(i, crlf - i), nullptr, 16);
            } catch (...) {
                break;
            }
            if (chunk_size == 0) break;
            i = crlf + 2;
            if (i + chunk_size > raw.size()) break;
            decoded.append(raw, i, chunk_size);
            i += chunk_size + 2;  // skip data + \r\n
        }
        return decoded;
    }

    return raw;
}

/// Wait until the HTTP handler responds, not just the TCP port.
bool wait_for_http(int port, int timeout_s = 30) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_s * valgrind_timeout_scale());
    while (std::chrono::steady_clock::now() < deadline) {
        auto probe = http_request(port,
                                  "GET /api/v1/files HTTP/1.1\r\n"
                                  "Host: localhost\r\n"
                                  "Connection: close\r\n"
                                  "\r\n",
                                  1);
        if (!probe.empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

/// Pick a random port in the ephemeral range.
int pick_port() { return 10000 + (::getpid() % 50000); }

/// RAII server process manager.
struct ServerProcess {
    pid_t pid = -1;
    int port = 0;

    ~ServerProcess() { stop(); }

    bool start(const std::string& binary, const std::string& data_dir, int p) {
        port = p;
        pid = ::fork();
        if (pid < 0) return false;

        if (pid == 0) {
            auto port_str = std::to_string(port);
            ::execl(binary.c_str(), binary.c_str(), "-d", data_dir.c_str(),
                    "-p", port_str.c_str(), "--bind", "127.0.0.1",
                    "--executor-threads", "2", nullptr);
            ::_exit(127);
        }

        // The server indexes its trace directory before binding, which has
        // taken ~17s on a loaded arm64 runner.
        return wait_for_port(port, 60) && wait_for_http(port, 30);
    }

    void stop() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            for (int i = 0; i < 50; ++i) {
                if (::waitpid(pid, &status, WNOHANG) > 0) {
                    pid = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }
};

}  // namespace

// ============================================================================
// Integration tests
// ============================================================================

TEST_CASE("DFTracer Server - binary exists") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE(
            "dftracer_server binary not found, skipping integration "
            "tests. Set DFTRACER_SERVER_PATH env to specify location.");
        return;
    }
    CHECK(!binary.empty());
}

// All endpoint checks run against ONE server process (no SUBCASEs).
// Doctest SUBCASEs re-enter the TEST_CASE body once per SUBCASE,
// which would fork+start+stop the server 8 times -- too slow for CI.
TEST_CASE("DFTracer Server - start and respond to endpoints") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE("dftracer_server binary not found, skipping.");
        return;
    }
    if (!can_bind_local_tcp_socket()) {
        MESSAGE("local TCP bind is unavailable in this environment, skipping.");
        return;
    }

    dft_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    // Create test files with .pfw.gz extension
    auto file1 = create_pfw_gz(env, 50, 1);
    REQUIRE(!file1.empty());

    int port = pick_port();
    ServerProcess server;
    REQUIRE(server.start(binary, env.get_dir(), port));

    // -- GET /api/v1/files returns 200 with JSON object --
    {
        auto resp = http_request(port,
                                 "GET /api/v1/files HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(!body.empty());
        CHECK(body.front() == '{');
        CHECK(body.find("\"files\"") != std::string::npos);
        CHECK(body.find("\"count\"") != std::string::npos);
    }

    // -- GET /api/v1/files/info returns file metadata --
    {
        // First get the file list to find a valid path
        auto list_resp = http_request(port,
                                      "GET /api/v1/files HTTP/1.1\r\n"
                                      "Host: localhost\r\n"
                                      "Connection: close\r\n"
                                      "\r\n");
        REQUIRE(!list_resp.empty());
        REQUIRE(extract_status_code(list_resp) == 200);

        // Extract a file path from the response
        auto list_body = extract_body(list_resp);
        auto path_pos = list_body.find("\"path\":\"");
        REQUIRE(path_pos != std::string::npos);
        auto path_start = path_pos + 8;  // skip '"path":"'
        auto path_end = list_body.find('"', path_start);
        REQUIRE(path_end != std::string::npos);
        auto file_path = list_body.substr(path_start, path_end - path_start);

        // Now query file info
        auto resp =
            http_request(port, "GET /api/v1/files/info?file=" + file_path +
                                   " HTTP/1.1\r\n"
                                   "Host: localhost\r\n"
                                   "Connection: close\r\n"
                                   "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(!body.empty());
        CHECK(body.front() == '{');
        CHECK(body.find("\"path\"") != std::string::npos);
        CHECK(body.find("\"has_bloom_data\"") != std::string::npos);
    }

    // -- GET /api/v1/files/info returns 400 without file param --
    {
        auto resp = http_request(port,
                                 "GET /api/v1/files/info HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 400);
    }

    // -- GET /api/v1/events returns 200 with NDJSON --
    {
        auto resp = http_request(port,
                                 "GET /api/v1/events?limit=10 HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(!body.empty());
        CHECK(body.front() == '{');
    }

    // -- GET /api/v1/events/stream returns NDJSON --
    {
        auto resp = http_request(port,
                                 "GET /api/v1/events/stream HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(resp.find("application/x-ndjson") != std::string::npos);
    }

    // -- GET /api/v1/stats returns 200 --
    {
        auto resp = http_request(port,
                                 "GET /api/v1/stats HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
    }

    // -- GET /api/v1/viz/events returns viz data --
    {
        auto resp = http_request(
            port,
            "GET /api/v1/viz/events?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(!body.empty());
        CHECK(body.front() == '{');
        CHECK(body.find("\"events\"") != std::string::npos);
        CHECK(body.find("\"metadata\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/events with lanes param (JSON array) --
    {
        auto resp = http_request(
            port,
            "GET /api/v1/viz/events?begin=0&end=999999999&summary=1"
            "&lanes=%5B%7B%22field%22%3A%22pid%22%2C%22value%22%3A%221%22%7D%5D"
            " HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(!body.empty());
        CHECK(body.front() == '{');
        CHECK(body.find("\"events\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/events with filters param (JSON array) --
    {
        // filters=[{"field":"pid","op":"=","value":1}]
        auto resp = http_request(
            port,
            "GET /api/v1/viz/events?begin=0&end=999999999&summary=1"
            "&filters=%5B%7B%22field%22%3A%22pid%22%2C%22op%22%3A%22%3D"
            "%22%2C%22value%22%3A1%7D%5D"
            " HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(!body.empty());
        CHECK(body.front() == '{');
        CHECK(body.find("\"events\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/events with duration filter --
    {
        // filters=[{"field":"dur","op":">=","value":0}]
        auto resp = http_request(
            port,
            "GET /api/v1/viz/events?begin=0&end=999999999&summary=1"
            "&filters=%5B%7B%22field%22%3A%22dur%22%2C%22op%22%3A%22%3E%3D"
            "%22%2C%22value%22%3A0%7D%5D"
            " HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
    }

    // -- GET /api/v1/viz/events returns 400 without required params --
    {
        auto resp = http_request(port,
                                 "GET /api/v1/viz/events HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 400);
    }

    // -- GET /api/v1/info returns global time bounds --
    {
        auto resp = http_request(port,
                                 "GET /api/v1/info HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(!body.empty());
        CHECK(body.front() == '{');
        CHECK(body.find("\"file_count\"") != std::string::npos);
        CHECK(body.find("\"files\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/breaks reports idle gaps + multi-run detection --
    {
        auto resp = http_request(port,
                                 "GET /api/v1/viz/breaks HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(body.front() == '{');
        CHECK(body.find("\"gaps\"") != std::string::npos);
        CHECK(body.find("\"multi_run\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/events returns normalized ts by default --
    {
        auto resp = http_request(
            port,
            "GET /api/v1/viz/events?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(!body.empty());
        // Default: ts_normalized should be true (when time bounds exist)
        CHECK(body.find("\"ts_normalized\"") != std::string::npos);
        CHECK(body.find("\"global_min_timestamp_us\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/events?ts_normalize=0 returns raw timestamps --
    {
        auto resp = http_request(
            port,
            "GET /api/v1/viz/events?begin=0&end=999999999&summary=1"
            "&ts_normalize=0"
            " HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);

        auto body = extract_body(resp);
        CHECK(!body.empty());
        // ts_normalize=0: should report ts_normalized:false
        CHECK(body.find("\"ts_normalized\":false") != std::string::npos);
        CHECK(body.find("\"global_min_timestamp_us\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/density returns aggregated density blocks --
    {
        auto resp = http_request(
            port,
            "GET /api/v1/viz/density?begin=0&end=999999999&summary=2"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.front() == '{');
        CHECK(body.find("\"density\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/counters returns bandwidth/IOPS buckets --
    {
        auto resp = http_request(
            port,
            "GET /api/v1/viz/counters?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"buckets\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/stats returns per-name aggregation --
    {
        auto resp = http_request(
            port,
            "GET /api/v1/viz/stats?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"names\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/proctree returns the inferred process tree --
    {
        auto resp =
            http_request(port,
                         "GET /api/v1/viz/proctree HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"nodes\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/calltree returns a merged flamegraph tree --
    {
        auto resp = http_request(
            port,
            "GET /api/v1/viz/calltree?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"children\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/histogram returns a duration distribution --
    {
        auto resp = http_request(
            port,
            "GET /api/v1/viz/histogram?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"buckets\"") != std::string::npos);
    }

    // -- GET /api/v1/viz/layers returns name->category + file counts --
    {
        auto resp =
            http_request(port,
                         "GET /api/v1/viz/layers HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.find("\"layers\"") != std::string::npos);
        CHECK(body.find("\"total_files\"") != std::string::npos);
        CHECK(body.find("\"io_files\"") != std::string::npos);
    }

    // -- GET / serves the embedded viewer page --
    {
        auto resp =
            http_request(port,
                         "GET / HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(resp.find("text/html") != std::string::npos);
    }

    // -- responses carry an open CORS header (for the webview client) --
    {
        auto resp =
            http_request(port,
                         "GET /api/v1/viz/layers HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(resp.find("Access-Control-Allow-Origin: *") != std::string::npos);
    }

    // -- CORS preflight is answered (browsers omit Authorization from it) --
    {
        auto resp = http_request(port,
                                 "OPTIONS /api/v1/info HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Origin: vscode-webview://x\r\n"
                                 "Access-Control-Request-Method: GET\r\n"
                                 "Connection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 204);
        CHECK(resp.find("Access-Control-Allow-Methods") != std::string::npos);
        CHECK(resp.find("Access-Control-Allow-Headers") != std::string::npos);
    }

    // -- /viz/density honors `limit` (it used to overshoot by whole batches) --
    {
        auto resp =
            http_request(port,
                         "GET /api/v1/viz/density?begin=0&end="
                         "999999999&summary=1&width=8192&limit=3 HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.find("\"limit\":3") != std::string::npos);
        // Only events carry "ph"; density blocks do not. This fixture's events
        // all fold, so the bound holds trivially here - it guards traces whose
        // events outlive the fold cutoff.
        std::size_t events = 0;
        for (std::size_t p = body.find("\"ph\":"); p != std::string::npos;
             p = body.find("\"ph\":", p + 1))
            ++events;
        CHECK(events <= 3);
    }

    // -- GET unknown path returns 404 --
    {
        auto resp = http_request(port,
                                 "GET /nonexistent HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 404);
    }
}

TEST_CASE("DFTracer Server - graceful shutdown via SIGTERM") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE("dftracer_server binary not found, skipping.");
        return;
    }
    if (!can_bind_local_tcp_socket()) {
        MESSAGE("local TCP bind is unavailable in this environment, skipping.");
        return;
    }

    dft_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    auto file = create_pfw_gz(env, 50, 1);
    REQUIRE(!file.empty());

    int port = pick_port();
    ServerProcess server;
    REQUIRE(server.start(binary, env.get_dir(), port));

    CHECK(port_is_listening(port));

    ::kill(server.pid, SIGTERM);

    int status = 0;
    bool exited = false;
    for (int i = 0; i < 150; ++i) {
        if (::waitpid(server.pid, &status, WNOHANG) > 0) {
            exited = true;
            server.pid = -1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    CHECK(exited);
    if (exited) {
        CHECK((WIFEXITED(status) || WIFSIGNALED(status)));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK_FALSE(port_is_listening(port));
}

// A chunk carries two iovec entries per event, so past ~511 events a single
// writev exceeds IOV_MAX and used to fail, yielding 200 with an empty body.
TEST_CASE("DFTracer Server - streams chunks larger than IOV_MAX") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE("dftracer_server binary not found, skipping.");
        return;
    }
    if (!can_bind_local_tcp_socket()) {
        MESSAGE("local TCP bind is unavailable in this environment, skipping.");
        return;
    }

    constexpr int NUM_EVENTS = 2000;
    dft_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());
    auto file = create_pfw_gz(env, NUM_EVENTS, 1);
    REQUIRE(!file.empty());

    int port = pick_port();
    ServerProcess server;
    REQUIRE(server.start(binary, env.get_dir(), port));
    REQUIRE(wait_for_http(port));

    auto resp = http_request(port,
                             "GET /api/v1/events/stream HTTP/1.1\r\n"
                             "Host: localhost\r\nConnection: close\r\n\r\n");
    REQUIRE(!resp.empty());
    CHECK(extract_status_code(resp) == 200);

    auto body = extract_body(resp);
    CHECK(!body.empty());
    std::size_t lines = 0;
    for (char c : body)
        if (c == '\n') ++lines;
    CHECK(lines >= static_cast<std::size_t>(NUM_EVENTS));
}

// Inter-run gaps must break at exact app-span boundaries even when far smaller
// than the old 2%-of-span threshold that used to drop them.
TEST_CASE("DFTracer Server - multi-run break detection") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE("dftracer_server binary not found, skipping.");
        return;
    }
    if (!can_bind_local_tcp_socket()) {
        MESSAGE("local TCP bind is unavailable in this environment, skipping.");
        return;
    }

    dft_utils_test::TestEnvironment env(1);
    REQUIRE(env.is_valid());
    // 3 runs of 1s, 10ms idle between: gap is ~0.33% of the span (under old
    // 2%).
    auto raw = env.create_dft_multirun_gzip_file(3, 1000000, 10000);
    REQUIRE(!raw.empty());
    std::string pfw = env.get_dir() + "/multirun.pfw.gz";
    fs::rename(raw, pfw);

    int port = pick_port();
    ServerProcess server;
    REQUIRE(server.start(binary, env.get_dir(), port));
    REQUIRE(wait_for_http(port));

    auto resp = http_request(port,
                             "GET /api/v1/viz/breaks HTTP/1.1\r\n"
                             "Host: localhost\r\nConnection: close\r\n\r\n");
    REQUIRE(!resp.empty());
    CHECK(extract_status_code(resp) == 200);

    auto body = extract_body(resp);
    CHECK(body.find("\"multi_run\":true") != std::string::npos);
    // Two gaps, normalized to the global min timestamp.
    CHECK(body.find("\"begin\":1000000,\"end\":1010000") != std::string::npos);
    CHECK(body.find("\"begin\":2010000,\"end\":2020000") != std::string::npos);
}

// A changed source under an existing index must be re-indexed on startup, not
// served stale. The extra run (extra gap) after the change proves the rebuild.
TEST_CASE("DFTracer Server - rebuilds stale index on changed source") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE("dftracer_server binary not found, skipping.");
        return;
    }
    if (!can_bind_local_tcp_socket()) {
        MESSAGE("local TCP bind is unavailable in this environment, skipping.");
        return;
    }

    dft_utils_test::TestEnvironment env(1);
    REQUIRE(env.is_valid());
    std::string pfw = env.get_dir() + "/mr.pfw.gz";

    auto v1 = env.create_dft_multirun_gzip_file(2, 1000000, 10000);
    REQUIRE(!v1.empty());
    fs::rename(v1, pfw);
    {
        int port = pick_port();
        ServerProcess server;
        REQUIRE(server.start(binary, env.get_dir(), port));
        REQUIRE(wait_for_http(port));
        auto body = extract_body(
            http_request(port,
                         "GET /api/v1/viz/breaks HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n"));
        CHECK(body.find("\"begin\":1000000,\"end\":1010000") !=
              std::string::npos);
        CHECK(body.find("\"begin\":2010000") == std::string::npos);
    }

    // Third run added; the on-disk index is now stale.
    auto v2 = env.create_dft_multirun_gzip_file(3, 1000000, 10000);
    REQUIRE(!v2.empty());
    fs::remove(pfw);
    fs::rename(v2, pfw);
    {
        int port = pick_port() + 1;
        ServerProcess server;
        REQUIRE(server.start(binary, env.get_dir(), port));
        REQUIRE(wait_for_http(port));
        auto body = extract_body(
            http_request(port,
                         "GET /api/v1/viz/breaks HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n"));
        CHECK(body.find("\"begin\":1000000,\"end\":1010000") !=
              std::string::npos);
        CHECK(body.find("\"begin\":2010000,\"end\":2020000") !=
              std::string::npos);
    }
}
