#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/io/io_backend.h>
#include <dftracer/utils/core/pipeline/pipeline.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/tasks/task.h>
#include <dftracer/utils/server/http_connection.h>
#include <dftracer/utils/server/router.h>
#include <dftracer/utils/server/signal_handler.h>
#include <dftracer/utils/server/tcp_listener.h>
#include <dftracer/utils/server/trace_api.h>
#include <dftracer/utils/server/trace_index.h>
#include <dftracer/utils/server/viz_api.h>
#include <dftracer/utils/server/viz_ui.h>
#include <dftracer/utils/utilities/reader/internal/member_decode_cache.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::server;

class ServerArgParse : public cli::ArgParse {
   public:
    cli::DirectoryArgs directory{cli::DirMode::REQUIRED};
    cli::PipelineArgs pipeline;

    std::string index_dir;
    std::string bind_addr = "127.0.0.1";
    std::string auth_token;
    uint16_t port = 8080;
    std::size_t checkpoint_size = constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    std::size_t member_cache_size = 1024ull * 1024 * 1024;  // 1 GB
    std::uint64_t timeout_seconds = 0;                      // 0 = no timeout

    explicit ServerArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(directory, pipeline);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("--index-dir")
            .help(
                "Directory for root-local .dftindex stores (default: same as "
                "--directory)")
            .default_value<std::string>("");

        parser()
            .add_argument("-b", "--bind")
            .help("Bind address (use 0.0.0.0 to expose on all interfaces)")
            .default_value<std::string>("127.0.0.1");

        parser()
            .add_argument("-p", "--port")
            .help("Listen port")
            .scan<'d', uint16_t>()
            .default_value(static_cast<uint16_t>(8080));

        parser()
            .add_argument("--token")
            .help(
                "Optional access token; when set, every request must supply it "
                "via ?token= or an 'Authorization: Bearer <token>' header")
            .default_value<std::string>("");

        parser()
            .add_argument("--checkpoint-size")
            .help(
                "Decompression checkpoint interval for auto-indexing "
                "(default: " +
                std::to_string(constants::indexer::DEFAULT_CHECKPOINT_SIZE) +
                "). Accepts units, e.g. 512KB, 4MB. Smaller = finer zoom-in "
                "seeks, larger index")
            .default_value(
                std::to_string(constants::indexer::DEFAULT_CHECKPOINT_SIZE));

        parser()
            .add_argument("--member-cache-size")
            .help(
                "Decoded gzip members retained to share across concurrent "
                "queries (0 disables retention but still coalesces in-flight "
                "decodes). Accepts units, e.g. 512MB, 2GB; default: 1GB")
            .default_value(std::to_string(1024ull * 1024 * 1024));

        parser()
            .add_argument("--timeout")
            .help(
                "Auto-shutdown after this much uptime (0 disables; default). "
                "Accepts units, e.g. 30s, 10m, 1.5h. Useful to stop lingering "
                "processes when a client such as the VSCode extension closes "
                "without killing the server")
            .default_value(std::string("0"));
    }

    void post_parse() override {
        index_dir = parser().get<std::string>("--index-dir");
        bind_addr = parser().get<std::string>("--bind");
        auth_token = parser().get<std::string>("--token");
        port = parser().get<uint16_t>("--port");
        checkpoint_size = cli::get_bytes_arg(parser(), "--checkpoint-size");
        member_cache_size = cli::get_bytes_arg(parser(), "--member-cache-size");
        timeout_seconds = static_cast<std::uint64_t>(
            cli::get_seconds_arg(parser(), "--timeout"));
    }
};

static coro::CoroTask<int> run_server(const ServerArgParse* cli) {
    const auto& bind_addr = cli->bind_addr;
    auto port = cli->port;
    const auto& dir = cli->directory.value;
    auto index_dir = cli->index_dir;
    auto executor_threads = cli->pipeline.executor_threads;

    if (index_dir.empty()) {
        index_dir = dir;
        std::fprintf(stderr, "Using trace directory for indexes: %s\n",
                     index_dir.c_str());
    } else {
        fs::create_directories(index_dir);
    }

    // Share decoded gzip members across concurrent queries (single-flight +
    // LRU), so overlapping requests over the same member decode it once.
    utilities::reader::internal::configure_global_member_decode_cache(
        cli->member_cache_size);
    std::fprintf(stderr, "Member decode cache: %zu bytes\n",
                 cli->member_cache_size);

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer Server", cli->pipeline);
    // Steady-load, latency-sensitive service: keep the full pool warm rather
    // than paying the elastic grow ramp on every request.
    pipeline_config.with_io_backend(io::IoBackendType::THREADPOOL)
        .with_io_batch_size(1)
        .with_watchdog(false)
        .with_eager()
        .with_global_timeout(std::chrono::seconds(0))
        .with_task_timeout(std::chrono::seconds(0));

    Pipeline pipeline(pipeline_config);

    std::fprintf(stderr,
                 "Using %zu worker threads; auto-index checkpoint size %zu "
                 "bytes\n",
                 executor_threads, cli->checkpoint_size);

    TraceIndex trace_index(dir, index_dir, executor_threads,
                           cli->checkpoint_size);
    co_await trace_index.initialize();

    Router router;
    router.set_auth_token(cli->auth_token);
    register_trace_api(router, trace_index);
    register_viz_api(router, trace_index);
    register_viz_ui(router);

    TcpListener listener(bind_addr, port);
    if (!listener.start()) {
        DFTRACER_UTILS_LOG_ERROR("Failed to bind to %s:%u", bind_addr.c_str(),
                                 port);
        co_return 1;
    }

    std::fprintf(stderr, "DFTracer server listening on %s:%u\n",
                 bind_addr.c_str(), port);
    std::fprintf(stderr, "Trace viewer UI: http://%s:%u/\n", bind_addr.c_str(),
                 port);
    std::fprintf(stderr, "Serving %zu trace files from %s\n",
                 trace_index.file_count(), dir.c_str());

    g_listen_fd.store(listener.fd(), std::memory_order_release);

    // Raise SIGTERM at the deadline so shutdown runs the same graceful path as
    // a real signal, letting a launcher start the server without guaranteeing
    // it kills the process on exit.
    if (cli->timeout_seconds > 0) {
        auto timeout = std::chrono::seconds(cli->timeout_seconds);
        std::fprintf(stderr, "Auto-shutdown scheduled in %llu seconds\n",
                     static_cast<unsigned long long>(cli->timeout_seconds));
        std::thread([timeout]() {
            std::this_thread::sleep_for(timeout);
            if (!g_shutdown_requested.load(std::memory_order_acquire)) {
                std::fprintf(stderr, "Timeout reached; shutting down\n");
                std::raise(SIGTERM);
            }
        }).detach();
    }

    auto server_task = make_task(
        [&](CoroScope& ctx) -> coro::CoroTask<void> {
            auto* router_ptr = &router;
#ifndef DFTRACER_UTILS_VALGRIND_MODE
            // Eager prewarm is a full pass over every file (gzip decompress +
            // scan). Skip it under Valgrind - the summary still builds lazily
            // on the first viz request, so the instrumented server test does
            // not pay this cost up front and blow its time budget.
            auto* index_ptr = &trace_index;
            ctx.spawn([index_ptr](CoroScope&) -> coro::CoroTask<void> {
                co_await prewarm_viz_summary(*index_ptr);
            });
#endif
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

    pipeline.execute();

    std::fprintf(stderr, "Server shut down gracefully\n");

    co_return 0;
}

int main(int argc, char** argv) {
    return cli::cli_main<ServerArgParse>(
        argc, argv, "dftracer_server",
        "Serve DFTracer trace data over HTTP. Query, filter, and stream "
        "trace events via REST API.",
        [](ServerArgParse& cli) {
            install_signal_handlers();
            return run_server(&cli).get();
        });
}
