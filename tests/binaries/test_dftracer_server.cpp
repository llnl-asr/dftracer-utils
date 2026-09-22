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
#include <zlib.h>

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
std::string create_pfw_gz(dftu_utils_test::TestEnvironment& env, int num_events,
                          int id) {
    auto trace_gz = env.create_dft_test_gzip_file(num_events);
    if (trace_gz.empty()) return "";

    std::string pfw_path =
        env.get_dir() + "/trace_" + std::to_string(id) + ".pfw.gz";
    fs::rename(trace_gz, pfw_path);
    return pfw_path;
}

/// Write a gzipped .pfw trace declaring time_metric=NS into `dir`. Events span
/// ts 5000100000..5020000000 ns (=> 5000100..5020000 us) with durations
/// 50000..250000 ns (=> 50..250 us), so scaling is observable in the response.
std::string create_ns_pfw_gz(const std::string& dir) {
    std::string path = dir + "/ns_trace.pfw.gz";
    gzFile f = gzopen(path.c_str(), "wb");
    if (f == nullptr) return "";
    gzputs(f, "[\n");
    gzputs(
        f,
        "{\"id\":0,\"name\":\"CM\",\"cat\":\"dft\",\"pid\":0,\"tid\":0,"
        "\"ph\":\"M\",\"args\":{\"name\":\"time_metric\",\"value\":\"NS\"}}\n");
    for (int i = 1; i <= 200; ++i) {
        unsigned long long ts =
            5000000000ULL + static_cast<unsigned long long>(i) * 100000ULL;
        unsigned long long dur =
            50000ULL * static_cast<unsigned long long>(1 + (i % 5));
        char buf[256];
        std::snprintf(
            buf, sizeof(buf),
            "{\"id\":%d,\"name\":\"read\",\"cat\":\"posix\",\"pid\":1,"
            "\"tid\":%d,\"ts\":%llu,\"dur\":%llu,\"ph\":\"X\"}\n",
            i, 1000 + i % 3, ts, dur);
        gzputs(f, buf);
    }
    gzclose(f);
    return path;
}

/// Find the dftracer_server binary. Checks DFTRACER_SERVER_PATH env first,
/// then common build paths relative to the test binary.
std::string find_server_binary() {
    return dftu_utils_test::find_binary_by_name("DFTRACER_SERVER_PATH",
                                                "dftracer_server");
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
/// Send `request` to the local server and return the raw response, or an
/// empty string on failure.
///
/// The timeout is generous because the whole suite runs in parallel and
/// several of these endpoints do real work; it bounds a hang, it is not a
/// latency assertion. Failures say which way they failed, since an empty
/// response otherwise cannot distinguish a server that never came up from
/// one that was too slow.
std::string http_request(int port, const std::string& request,
                         int recv_timeout_s = 60) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        MESSAGE("http_request: socket() failed");
        return "";
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0) {
        ::close(sock);
        MESSAGE("http_request: connect to port " << port << " failed");
        return "";
    }

    ssize_t sent = ::send(sock, request.data(), request.size(), 0);
    if (sent < 0) {
        ::close(sock);
        MESSAGE("http_request: send failed");
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
    if (response.empty()) {
        MESSAGE("http_request: no response within " << recv_timeout_s << "s");
    }
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
                                  "GET /api/files HTTP/1.1\r\n"
                                  "Host: localhost\r\n"
                                  "Connection: close\r\n"
                                  "\r\n",
                                  1);
        if (!probe.empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

/// Ask the OS for a free loopback port, one per call.
///
/// Deriving it from the pid collides across concurrently running test
/// processes - pids of processes started together are adjacent, and the
/// cases here used pick_port()+1..+4 - so a request could be answered by
/// another test's server, which serves a different trace directory.
int pick_port() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return 0;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    int port = 0;
    if (::bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) ==
        0) {
        socklen_t len = sizeof(addr);
        if (::getsockname(sock, reinterpret_cast<struct sockaddr*>(&addr),
                          &len) == 0) {
            port = ntohs(addr.sin_port);
        }
    }
    ::close(sock);
    return port;
}

/// RAII server process manager.
struct ServerProcess {
    pid_t pid = -1;
    int port = 0;

    ~ServerProcess() { stop(); }

    // pick_port() reserves an ephemeral port then closes it, so under parallel
    // load another process can claim it in the long gap before the child (which
    // indexes first) binds. Retry on a fresh port rather than fail; `port` is
    // updated to the port the server actually came up on.
    bool start(const std::string& binary, const std::string& data_dir,
               int& port_io) {
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (attempt > 0) port_io = pick_port();
            if (start_once(binary, data_dir, port_io)) return true;
            stop();
        }
        return false;
    }

    bool start_once(const std::string& binary, const std::string& data_dir,
                    int p) {
        port = p;
        // Build before fork: the child may only call async-signal-safe
        // functions until execl, and std::to_string allocates.
        auto port_str = std::to_string(port);
        pid = ::fork();
        if (pid < 0) return false;

        if (pid == 0) {
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

    dftu_utils_test::TestEnvironment env(100);
    REQUIRE(env.is_valid());

    // Create test files with .pfw.gz extension
    auto file1 = create_pfw_gz(env, 50, 1);
    REQUIRE(!file1.empty());

    int port = pick_port();
    ServerProcess server;
    REQUIRE(server.start(binary, env.get_dir(), port));

    // -- GET /api/files returns 200 with JSON object --
    {
        auto resp = http_request(port,
                                 "GET /api/files HTTP/1.1\r\n"
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

    // -- GET /api/files/info returns file metadata --
    {
        // First get the file list to find a valid path
        auto list_resp = http_request(port,
                                      "GET /api/files HTTP/1.1\r\n"
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
        auto resp = http_request(port, "GET /api/files/info?file=" + file_path +
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

    // -- GET /api/files/info returns 400 without file param --
    {
        auto resp = http_request(port,
                                 "GET /api/files/info HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 400);
    }

    // -- GET /api/viz/events returns viz data --
    {
        auto resp =
            http_request(port,
                         "GET /api/viz/events?begin=0&end=999999999&summary=1"
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

    // -- GET /api/viz/events with lanes param (JSON array) --
    {
        auto resp = http_request(
            port,
            "GET /api/viz/events?begin=0&end=999999999&summary=1"
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

    // -- GET /api/viz/events with filters param (JSON array) --
    {
        // filters=[{"field":"pid","op":"=","value":1}]
        auto resp = http_request(
            port,
            "GET /api/viz/events?begin=0&end=999999999&summary=1"
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

    // -- GET /api/viz/events with duration filter --
    {
        // filters=[{"field":"dur","op":">=","value":0}]
        auto resp = http_request(
            port,
            "GET /api/viz/events?begin=0&end=999999999&summary=1"
            "&filters=%5B%7B%22field%22%3A%22dur%22%2C%22op%22%3A%22%3E%3D"
            "%22%2C%22value%22%3A0%7D%5D"
            " HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
    }

    // -- GET /api/viz/events returns 400 without required params --
    {
        auto resp = http_request(port,
                                 "GET /api/viz/events HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");

        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 400);
    }

    // -- GET /api/info returns global time bounds --
    {
        auto resp = http_request(port,
                                 "GET /api/info HTTP/1.1\r\n"
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

    // -- GET /api/viz/breaks reports idle gaps + multi-run detection --
    {
        auto resp = http_request(port,
                                 "GET /api/viz/breaks HTTP/1.1\r\n"
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

    // -- GET /api/viz/events returns normalized ts by default --
    {
        auto resp =
            http_request(port,
                         "GET /api/viz/events?begin=0&end=999999999&summary=1"
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

    // -- GET /api/viz/events?ts_normalize=0 returns raw timestamps --
    {
        auto resp =
            http_request(port,
                         "GET /api/viz/events?begin=0&end=999999999&summary=1"
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

    // -- GET /api/viz/density returns aggregated density blocks --
    {
        auto resp = http_request(
            port,
            "GET /api/viz/density?begin=0&end=999999999&summary=2"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.front() == '{');
        CHECK(body.find("\"density\"") != std::string::npos);
    }

    // -- GET /api/viz/counters returns bandwidth/IOPS buckets --
    {
        auto resp = http_request(
            port,
            "GET /api/viz/counters?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"buckets\"") != std::string::npos);
    }

    // -- /viz/counters live path (a cat filter makes it summary-ineligible, so
    // the request folds counters over the View scan) --
    {
        auto resp = http_request(
            port,
            "GET /api/viz/counters?begin=0&end=999999999&cat=POSIX"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.find("\"read_bytes\"") != std::string::npos);
        CHECK(body.find("\"write_bytes\"") != std::string::npos);
        CHECK(body.find("\"ops\"") != std::string::npos);
    }

    // -- GET /api/viz/stats returns per-name aggregation --
    {
        auto resp = http_request(
            port,
            "GET /api/viz/stats?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"names\"") != std::string::npos);
    }

    // -- GET /api/viz/proctree returns the inferred process tree --
    {
        auto resp =
            http_request(port,
                         "GET /api/viz/proctree HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"nodes\"") != std::string::npos);
    }

    // -- GET /api/viz/calltree returns a merged flamegraph tree --
    {
        auto resp = http_request(
            port,
            "GET /api/viz/calltree?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"children\"") != std::string::npos);
    }

    // -- GET /api/viz/histogram returns a duration distribution --
    {
        auto resp = http_request(
            port,
            "GET /api/viz/histogram?begin=0&end=999999999&summary=1"
            " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        CHECK(extract_body(resp).find("\"buckets\"") != std::string::npos);
    }

    // -- GET /api/viz/layers returns name->category + file counts --
    {
        auto resp =
            http_request(port,
                         "GET /api/viz/layers HTTP/1.1\r\n"
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
                         "GET /api/viz/layers HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n");
        REQUIRE(!resp.empty());
        CHECK(resp.find("Access-Control-Allow-Origin: *") != std::string::npos);
    }

    // -- CORS preflight is answered (browsers omit Authorization from it) --
    {
        auto resp = http_request(port,
                                 "OPTIONS /api/info HTTP/1.1\r\n"
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
                         "GET /api/viz/density?begin=0&end="
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

    dftu_utils_test::TestEnvironment env(100);
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
    // A generous exit budget: the graceful teardown (close listen fd, drain the
    // async runtime + RocksDB) can be CPU-starved when the full suite runs many
    // process-spawning tests unbounded-parallel, so 15s occasionally flaked.
    for (int i = 0; i < 600; ++i) {
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

    dftu_utils_test::TestEnvironment env(1);
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
                             "GET /api/viz/breaks HTTP/1.1\r\n"
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

    dftu_utils_test::TestEnvironment env(1);
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
                         "GET /api/viz/breaks HTTP/1.1\r\n"
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
        int port = pick_port();
        ServerProcess server;
        REQUIRE(server.start(binary, env.get_dir(), port));
        REQUIRE(wait_for_http(port));
        auto body = extract_body(
            http_request(port,
                         "GET /api/viz/breaks HTTP/1.1\r\n"
                         "Host: localhost\r\nConnection: close\r\n\r\n"));
        CHECK(body.find("\"begin\":1000000,\"end\":1010000") !=
              std::string::npos);
        CHECK(body.find("\"begin\":2010000,\"end\":2020000") !=
              std::string::npos);
    }
}

// /viz/density folds real ph="C" counter events into a counter_series section
// alongside the ph="X" density blocks - both from one scan. group_by forces the
// live path (the summary path carries no counters) without filtering ph="C".
TEST_CASE("DFTracer Server - density surfaces ph=C counters as blocks") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE("dftracer_server binary not found, skipping.");
        return;
    }
    if (!can_bind_local_tcp_socket()) {
        MESSAGE("local TCP bind is unavailable in this environment, skipping.");
        return;
    }

    dftu_utils_test::TestEnvironment env(1);
    REQUIRE(env.is_valid());
    std::string path = env.get_dir() + "/ctr.pfw.gz";
    gzFile f = gzopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    gzputs(f, "[\n");
    for (int i = 0; i < 10; ++i) {
        std::string ev =
            "{\"name\":\"read\",\"cat\":\"POSIX\",\"pid\":1,\"tid\":1,\"ts\":" +
            std::to_string(1000 + i * 1000) +
            ",\"dur\":10,\"ph\":\"X\",\"args\":{}}\n";
        gzputs(f, ev.c_str());
    }
    // Counter events: cpu utilization with two numeric args -> two series.
    for (int i = 0; i < 5; ++i) {
        std::string ev =
            "{\"name\":\"cpu\",\"cat\":\"sys\",\"pid\":0,\"tid\":0,\"ts\":" +
            std::to_string(1000 + i * 2000) +
            ",\"ph\":\"C\",\"args\":{"
            "\"user_pct\":" +
            std::to_string(40 + i) + ",\"idle_pct\":" + std::to_string(60 - i) +
            "}}\n";
        gzputs(f, ev.c_str());
    }
    gzputs(f, "]\n");
    gzclose(f);

    int port = pick_port();
    ServerProcess server;
    REQUIRE(server.start(binary, env.get_dir(), port));
    REQUIRE(wait_for_http(port));

    auto resp = http_request(
        port,
        "GET /api/viz/density?begin=0&end=100000&summary=1&group_by=name"
        " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    REQUIRE(!resp.empty());
    CHECK(extract_status_code(resp) == 200);
    auto body = extract_body(resp);
    // ph="C" counters are aggregated into ordinary density blocks: one series
    // per (name.arg), flagged counter with the mean reading as value.
    CHECK(body.find("\"cpu.user_pct\"") != std::string::npos);
    CHECK(body.find("\"cpu.idle_pct\"") != std::string::npos);
    CHECK(body.find("\"counter\":true") != std::string::npos);
    CHECK(body.find("\"value\":") != std::string::npos);
}

// The viz summary is cached to disk so a restart skips the full rescan, and the
// cache must invalidate when the source changes rather than serve stale data.
TEST_CASE("DFTracer Server - viz summary cache persists and invalidates") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE("dftracer_server binary not found, skipping.");
        return;
    }
    if (!can_bind_local_tcp_socket()) {
        MESSAGE("local TCP bind is unavailable in this environment, skipping.");
        return;
    }

    dftu_utils_test::TestEnvironment env(1);
    REQUIRE(env.is_valid());
    std::string pfw = env.get_dir() + "/mr.pfw.gz";
    std::string cache = env.get_dir() + "/.dftviz_summary";
    const std::string breaks_req =
        "GET /api/viz/breaks HTTP/1.1\r\n"
        "Host: localhost\r\nConnection: close\r\n\r\n";

    auto v1 = env.create_dft_multirun_gzip_file(2, 1000000, 10000);
    REQUIRE(!v1.empty());
    fs::rename(v1, pfw);

    // First run builds the summary (a /viz request) and writes the cache.
    {
        int port = pick_port();
        ServerProcess server;
        REQUIRE(server.start(binary, env.get_dir(), port));
        REQUIRE(wait_for_http(port));
        auto body = extract_body(http_request(port, breaks_req));
        CHECK(body.find("\"begin\":1000000,\"end\":1010000") !=
              std::string::npos);
    }
    CHECK(fs::exists(cache));

    // Second run reuses the cache; the result is unchanged.
    {
        int port = pick_port();
        ServerProcess server;
        REQUIRE(server.start(binary, env.get_dir(), port));
        REQUIRE(wait_for_http(port));
        auto body = extract_body(http_request(port, breaks_req));
        CHECK(body.find("\"begin\":1000000,\"end\":1010000") !=
              std::string::npos);
        CHECK(body.find("\"begin\":2010000") == std::string::npos);
    }

    // A changed source invalidates the cache: the new run's gap must appear.
    auto v2 = env.create_dft_multirun_gzip_file(3, 1000000, 10000);
    REQUIRE(!v2.empty());
    fs::remove(pfw);
    fs::rename(v2, pfw);
    {
        int port = pick_port();
        ServerProcess server;
        REQUIRE(server.start(binary, env.get_dir(), port));
        REQUIRE(wait_for_http(port));
        auto body = extract_body(http_request(port, breaks_req));
        CHECK(body.find("\"begin\":2010000,\"end\":2020000") !=
              std::string::npos);
    }
}

// The calltree endpoint builds its tree with streaming per-file workers merged
// into one tree; two files (with overlapping pids) exercise the worker fan-out,
// the merge, and the grouped P-node dedup. Grouped and flat trees must report
// the same totals.
TEST_CASE("DFTracer Server - calltree endpoint") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE("dftracer_server binary not found, skipping.");
        return;
    }
    if (!can_bind_local_tcp_socket()) {
        MESSAGE("local TCP bind is unavailable in this environment, skipping.");
        return;
    }

    dftu_utils_test::TestEnvironment env(1);
    REQUIRE(env.is_valid());
    REQUIRE(!create_pfw_gz(env, 40, 1).empty());
    REQUIRE(!create_pfw_gz(env, 30, 2).empty());

    int port = pick_port();
    ServerProcess server;
    REQUIRE(server.start(binary, env.get_dir(), port));
    REQUIRE(wait_for_http(port));

    // Root fields come right after `"tree":{` in serialization order.
    auto root_field = [](const std::string& body, const char* key) -> double {
        auto tpos = body.find("\"tree\":{");
        REQUIRE(tpos != std::string::npos);
        auto kpos = body.find("\"" + std::string(key) + "\":", tpos);
        REQUIRE(kpos != std::string::npos);
        return std::strtod(body.c_str() + kpos + std::strlen(key) + 3, nullptr);
    };

    auto flat = http_request(port,
                             "GET /api/viz/calltree?begin=0&end=999999999"
                             " HTTP/1.1\r\n"
                             "Host: localhost\r\n"
                             "X-Request-Id: e2e-calltree\r\n"
                             "Connection: close\r\n"
                             "\r\n");
    REQUIRE(!flat.empty());
    CHECK(extract_status_code(flat) == 200);
    auto fbody = extract_body(flat);
    CHECK(fbody.find("\"truncated\":false") != std::string::npos);
    CHECK(fbody.find("\"name\":\"all\"") != std::string::npos);
    double f_total = root_field(fbody, "total");
    double f_count = root_field(fbody, "count");
    CHECK(f_total > 0);
    CHECK(f_count > 0);

    // group=<field> roots the tree by an arbitrary key over the raw events
    // (not just pid); the synthetic node is named by that value. Grouping only
    // reparents subtrees, so the root total and frame count are unchanged.
    auto grouped =
        http_request(port,
                     "GET /api/viz/calltree?begin=0&end=999999999&group=pid"
                     " HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Connection: close\r\n"
                     "\r\n");
    REQUIRE(!grouped.empty());
    CHECK(extract_status_code(grouped) == 200);
    auto gbody = extract_body(grouped);
    // A process node named by its pid value (the generator uses pid 1000+i).
    CHECK(gbody.find("\"name\":\"100") != std::string::npos);
    CHECK(root_field(gbody, "total") == doctest::Approx(f_total));
    CHECK(root_field(gbody, "count") == doctest::Approx(f_count));

    // group by a non-pid field (cat) also roots the tree - proving the general
    // key, not a hardcoded by-process path.
    auto by_cat =
        http_request(port,
                     "GET /api/viz/calltree?begin=0&end=999999999&group=cat"
                     " HTTP/1.1\r\n"
                     "Host: localhost\r\n"
                     "Connection: close\r\n"
                     "\r\n");
    REQUIRE(!by_cat.empty());
    CHECK(extract_status_code(by_cat) == 200);
    auto cbody = extract_body(by_cat);
    CHECK(root_field(cbody, "total") == doctest::Approx(f_total));

    // -- POST /api/cancel: unknown id is a no-op, route is wired --
    auto cancel = http_request(port,
                               "POST /api/cancel?id=nope HTTP/1.1\r\n"
                               "Host: localhost\r\n"
                               "Connection: close\r\n"
                               "\r\n");
    REQUIRE(!cancel.empty());
    CHECK(extract_status_code(cancel) == 200);
    CHECK(extract_body(cancel).find("\"cancelled\":false") !=
          std::string::npos);

    // -- GET /api/viz/columns lists groupable columns (incl. args keys) --
    {
        auto resp = http_request(port,
                                 "GET /api/viz/columns HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.find("\"columns\"") != std::string::npos);
        CHECK(body.find("\"ready\":true") != std::string::npos);
        // The test events carry cat/name and an args.ret key.
        CHECK(body.find("\"cat\"") != std::string::npos);
        CHECK(body.find("\"ret\"") != std::string::npos);
        // Lane-level / bookkeeping fields are not offered as columns.
        CHECK(body.find("\"pid\"") == std::string::npos);
        // The response also carries a per-column type map (View::schema()).
        CHECK(body.find("\"types\"") != std::string::npos);
    }

    // -- GET /api/viz/density with group_by --
    {
        // summary=64 forces sub-pixel folding so density blocks exist.
        auto resp =
            http_request(port,
                         "GET /api/viz/density?begin=0&end=999999999&summary=64"
                         "&group_by=cat HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: close\r\n"
                         "\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.find("\"density\"") != std::string::npos);
        // Every test event has a cat, so grouped blocks echo it.
        CHECK(body.find("\"group\":\"") != std::string::npos);
    }
    {
        // A column no event has: still 200, blocks just carry no group (the
        // client renders them under "(none)").
        auto resp =
            http_request(port,
                         "GET /api/viz/density?begin=0&end=999999999&summary=64"
                         "&group_by=no_such_column HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: close\r\n"
                         "\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.find("\"density\"") != std::string::npos);
        CHECK(body.find("\"group\":\"") == std::string::npos);
    }
    {
        // Malformed column names are rejected.
        auto resp =
            http_request(port,
                         "GET /api/viz/density?begin=0&end=999999999&summary=64"
                         "&group_by=bad%20name! HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: close\r\n"
                         "\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 400);
    }
}

// A trace declaring time_metric=NS must be presented in microseconds by the
// viz/info APIs (native ns values divided by 1000), not the raw ns magnitudes.
TEST_CASE("DFTracer Server - CM time_metric scales viz to microseconds") {
    auto binary = find_server_binary();
    if (binary.empty()) {
        MESSAGE("dftracer_server binary not found, skipping.");
        return;
    }
    if (!can_bind_local_tcp_socket()) {
        MESSAGE("local TCP bind is unavailable in this environment, skipping.");
        return;
    }

    dftu_utils_test::TestEnvironment env(1);
    REQUIRE(env.is_valid());
    auto ns_file = create_ns_pfw_gz(env.get_dir());
    REQUIRE(!ns_file.empty());

    int port = pick_port();
    ServerProcess server;
    REQUIRE(server.start(binary, env.get_dir(), port));

    // /info bounds are microseconds (5000100..5020000), not native ns.
    {
        auto resp = http_request(port,
                                 "GET /api/info HTTP/1.1\r\n"
                                 "Host: localhost\r\n"
                                 "Connection: close\r\n"
                                 "\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.find("\"min_timestamp_us\":5000100") != std::string::npos);
        // Max is max(ts+dur): event 199 ends at 5020150000 ns => 5020150 us.
        CHECK(body.find("\"max_timestamp_us\":5020150") != std::string::npos);
        // The unscaled native magnitude must not leak through.
        CHECK(body.find("5000100000") == std::string::npos);
    }

    // /viz/events durations are microseconds (50..250), not native ns.
    {
        auto resp = http_request(
            port,
            "GET /api/viz/events?begin=0&end=25000&summary=1 HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.find("\"dur\":250,") != std::string::npos);
        CHECK(body.find("\"dur\":250000") == std::string::npos);
        CHECK(body.find("\"global_min_timestamp_us\":5000100") !=
              std::string::npos);
    }

    // /viz/stats aggregates durations in microseconds.
    {
        auto resp =
            http_request(port,
                         "GET /api/viz/stats?begin=0&end=25000 HTTP/1.1\r\n"
                         "Host: localhost\r\n"
                         "Connection: close\r\n"
                         "\r\n");
        REQUIRE(!resp.empty());
        CHECK(extract_status_code(resp) == 200);
        auto body = extract_body(resp);
        CHECK(body.find("\"max\":250") != std::string::npos);
        CHECK(body.find("\"max\":250000") == std::string::npos);
    }
}
