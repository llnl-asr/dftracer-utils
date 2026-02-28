#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/pipeline/pipeline_config.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/server/http_connection.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/signal_handler.h>
#include <dftracer/utils/server/tcp_listener.h>
#include <dftracer/utils/server/trace_api.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>

#include <argparse/argparse.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::server;

/// RAII guard that removes a directory tree on destruction.
/// Covers normal return, signal-induced pipeline drain, and exceptions.
struct TempDirGuard {
    std::string path;
    explicit TempDirGuard(std::string p) : path(std::move(p)) {}
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    ~TempDirGuard() {
        if (path.empty()) return;
        std::error_code ec;
        fs::remove_all(path, ec);
        if (!ec) {
            std::fprintf(stderr, "Cleaned up temp index directory\n");
        }
    }
};

static coro::CoroTask<int> run_server(argparse::ArgumentParser& program) {
    std::string bind_addr = program.get<std::string>("--bind");
    uint16_t port = program.get<uint16_t>("--port");
    std::string directory = program.get<std::string>("--directory");
    std::string index_dir = program.get<std::string>("--index-dir");
    std::size_t executor_threads =
        program.get<std::size_t>("--executor-threads");

    // When no explicit index dir is given, use a temp directory so
    // on-the-fly .idx/.bidx files don't pollute the data directory.
    std::unique_ptr<TempDirGuard> temp_guard;
    if (index_dir.empty()) {
        auto tmp = fs::temp_directory_path() / "dftracer-server-index";
        fs::create_directories(tmp);
        index_dir = tmp.string();
        temp_guard = std::make_unique<TempDirGuard>(index_dir);
        std::fprintf(stderr, "Using temp index directory: %s\n",
                     index_dir.c_str());
    }

    auto pipeline_config =
        PipelineConfig()
            .with_name("DFTracer Server")
            .with_compute_threads(executor_threads)
            .with_watchdog(false)  // Server is long-lived; no watchdog
            .with_global_timeout(std::chrono::seconds(0))  // Run forever
            .with_task_timeout(std::chrono::seconds(0))  // No per-task timeout
            .with_io_backend(
                io::IoBackendType::THREADPOOL)  // Thread pool IO for server
            .with_io_batch_size(1);

    Pipeline pipeline(pipeline_config);

    // Build trace index (scan directory, load bloom indexes)
    TraceIndex trace_index(directory, index_dir);
    co_await trace_index.initialize();

    // Set up router
    Router router;
    register_trace_api(router, trace_index);
    register_viz_api(router, trace_index);

    // Start TCP listener
    TcpListener listener(bind_addr, port);
    if (!listener.start()) {
        DFTRACER_UTILS_LOG_ERROR("Failed to bind to %s:%u", bind_addr.c_str(),
                                 port);
        co_return 1;
    }

    std::fprintf(stderr, "DFTracer server listening on %s:%u\n",
                 bind_addr.c_str(), port);
    std::fprintf(stderr, "Serving %zu trace files from %s\n",
                 trace_index.file_count(), directory.c_str());

    // Register listen fd so signal handler can unblock accept().
    g_listen_fd.store(listener.fd(), std::memory_order_release);

    auto server_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            auto* router_ptr = &router;
            co_await listener.accept_loop(
                ctx,
                [router_ptr](int client_fd,
                             struct sockaddr_in addr) -> coro::CoroTask<void> {
                    co_await handle_connection(client_fd, addr, *router_ptr);
                });
        },
        "Server");

    pipeline.set_source(server_task);
    pipeline.set_destination(server_task);

    // Run until SIGINT/SIGTERM.
    // Signal handler sets g_shutdown_requested, which causes accept_loop
    // to break, CoroScope drains in-flight handlers, and
    // pipeline.execute() returns.
    pipeline.execute();

    std::fprintf(stderr, "Server shut down gracefully\n");

    // temp_guard destructor handles cleanup.
    co_return 0;
}

int main(int argc, char** argv) {
    DFTRACER_UTILS_LOGGER_INIT();

    argparse::ArgumentParser program("dftracer_server",
                                     DFTRACER_UTILS_PACKAGE_VERSION);
    program.add_description(
        "Serve DFTracer trace data over HTTP. Query, filter, and stream "
        "trace events via REST API.");

    program.add_argument("-b", "--bind")
        .help("Bind address")
        .default_value<std::string>("0.0.0.0");

    program.add_argument("-p", "--port")
        .help("Listen port")
        .scan<'d', uint16_t>()
        .default_value(static_cast<uint16_t>(8080));

    program.add_argument("-d", "--directory")
        .help("Directory containing trace files")
        .required();

    program.add_argument("--index-dir")
        .help("Directory for bloom/checkpoint index files")
        .default_value<std::string>("");

    program.add_argument("--executor-threads")
        .help("Number of worker threads")
        .scan<'d', std::size_t>()
        .default_value(
            static_cast<std::size_t>(std::thread::hardware_concurrency()));

    install_signal_handlers();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        DFTRACER_UTILS_LOG_ERROR("Error: %s", err.what());
        std::cerr << program;
        return 1;
    }

    return run_server(program).get();
}
