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

/// Wait until port is listening or timeout expires.
bool wait_for_port(int port, int timeout_s = 10) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        if (port_is_listening(port)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

/// Send a raw HTTP request and receive the response.
std::string http_request(int port, const std::string& request,
                         int recv_timeout_s = 2) {
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
    tv.tv_sec = recv_timeout_s;
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
    return response.substr(pos + 4);
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

        return wait_for_port(port, 15);
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
        CHECK(body.find("\"has_bloom_index\"") != std::string::npos);
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

    // -- GET /api/v1/events returns 200 with JSON object --
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
        CHECK(body.find("\"events\"") != std::string::npos);
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
