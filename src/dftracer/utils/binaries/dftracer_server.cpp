// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/common/config.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/runtime/io/io_backend.h>
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
#include <exception>
#include <string>
#include <thread>

using namespace dftracer::utils;
using namespace dftracer::utils::server;

class ServerArgParse : public binaries::ArgParse {
   public:
    binaries::InputArgs inputs{binaries::DirMode::Required,
                               "Folder of trace files to serve"};
    binaries::RuntimeArgs runtime_args;

    dstd::StringView index_dir;
    dstd::StringView bind_addr = "127.0.0.1";
    dstd::StringView auth_token;
    uint16_t port = 8080;
    std::size_t checkpoint_size =
        common::constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    std::size_t member_cache_size = 1024ull * 1024 * 1024;  // 1 GB
    std::uint64_t timeout_seconds = 0;                      // 0 = no timeout
    double timeout_raw = 0;

    explicit ServerArgParse(binaries::ArgParser& p) : ArgParse(p) {
        inputs.folder_only = true;
        schema(inputs, runtime_args);
    }

   protected:
    void register_args() override {
        parser().option(
            index_dir, {"--index-dir"},
            "Directory for root-local .dftindex stores (default: same as "
            "the input folder)");

        parser().option(
            bind_addr, {"-b", "--bind"},
            "Bind address (use 0.0.0.0 to expose on all interfaces)");

        parser().option(port, {"-p", "--port"}, "Listen port");

        parser().option(
            auth_token, {"--token"},
            "Optional access token; when set, every request must supply it "
            "via ?token= or an 'Authorization: Bearer <token>' header");

        parser().bytes(
            checkpoint_size, {"--checkpoint-size"},
            "Decompression checkpoint interval for auto-indexing "
            "(default: " +
                dstd::to_string(
                    common::constants::indexer::DEFAULT_CHECKPOINT_SIZE) +
                "). Accepts units, e.g. 512KB, 4MB. Smaller = finer zoom-in "
                "seeks, larger index",
            dstd::to_string(
                common::constants::indexer::DEFAULT_CHECKPOINT_SIZE));

        parser().bytes(
            member_cache_size, {"--member-cache-size"},
            "Decoded gzip members retained to share across concurrent "
            "queries (0 disables retention but still coalesces in-flight "
            "decodes). Accepts units, e.g. 512MB, 2GB; default: 1GB",
            dstd::to_string(1024ull * 1024 * 1024));

        parser().seconds(
            timeout_raw, {"--timeout"},
            "Auto-shutdown after this much uptime (0 disables; default). "
            "Accepts units, e.g. 30s, 10m, 1.5h. Useful to stop lingering "
            "processes when a client such as the VSCode extension closes "
            "without killing the server",
            "0");
    }

    dstd::Result<void> post_parse() override {
        timeout_seconds = static_cast<std::uint64_t>(timeout_raw);
        return {};
    }
};

static coro::CoroTask<int> run_server(const ServerArgParse* cli) {
    const dstd::String bind_addr(cli->bind_addr);
    auto port = cli->port;
    const dstd::String dir(cli->inputs.directory);
    dstd::String index_dir(cli->index_dir);
    auto executor_threads = cli->runtime_args.executor_threads;

    if (index_dir.empty()) {
        index_dir = dir;
        std::fprintf(stderr, "Using trace directory for indexes: %s\n",
                     index_dir.c_str());
    } else {
        auto created = dstd::fs::create_directories(dstd::PathView(index_dir));
        if (!created.has_value())
            co_return binaries::report_error(created.error());
    }

    // Share decoded gzip members across concurrent queries (single-flight +
    // LRU), so overlapping requests over the same member decode it once.
    utilities::reader::internal::configure_global_member_decode_cache(
        cli->member_cache_size);
    std::fprintf(stderr, "Member decode cache: %zu bytes\n",
                 cli->member_cache_size);

    // Steady-load, latency-sensitive service: keep the full pool warm rather
    // than paying the elastic grow ramp on every request.
    RuntimeSettings settings;
    cli->runtime_args.apply(settings);
    settings.io_backend = runtime::io::IoBackendType::Threadpool;
    settings.io_batch_threshold = 1;
    settings.eager = true;
    configure_default_runtime(settings);

    std::fprintf(stderr,
                 "Using %zu worker threads; auto-index checkpoint size %zu "
                 "bytes\n",
                 executor_threads, cli->checkpoint_size);

    TraceIndex trace_index(dir, index_dir, executor_threads,
                           cli->checkpoint_size);
    auto initialized = co_await trace_index.initialize();
    if (!initialized) {
        DFTRACER_UTILS_LOG_ERROR("Failed to index {}: {}", dir,
                                 initialized.error().message());
        co_return 1;
    }

    Router router;
    router.set_auth_token(dstd::String(cli->auth_token));
    register_trace_api(router, trace_index);
    register_viz_api(router, trace_index);
    register_viz_ui(router);

    TcpListener listener(bind_addr, port);
    if (!listener.start()) {
        DFTRACER_UTILS_LOG_ERROR("Failed to bind to {}:{}", bind_addr, port);
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
        dstd::Thread([timeout]() {
            std::this_thread::sleep_for(timeout);
            if (!g_shutdown_requested.load(std::memory_order_acquire)) {
                std::fprintf(stderr, "Timeout reached; shutting down\n");
                std::raise(SIGTERM);
            }
        }).detach();
    }

    auto ran = binaries::run_on_pool(
        "DFTracer Server", cli->runtime_args,
        [&](coro::Scope& ctx) -> coro::CoroTask<void> {
            auto* router_ptr = &router;
#ifndef DFTRACER_UTILS_VALGRIND_MODE
            // Eager prewarm is a full pass over every file (gzip
            // decompress + scan). Skip it under Valgrind - the summary
            // still builds lazily on the first viz request, so the
            // instrumented server test does not pay this cost up front
            // and blow its time budget.
            auto* index_ptr = &trace_index;
            // The summary also builds on the first viz request, so a
            // failed prewarm is logged and the server keeps running.
            ctx.spawn([index_ptr]() -> coro::CoroTask<void> {
                auto prewarmed = co_await prewarm_viz_summary(*index_ptr);
                if (!prewarmed)
                    DFTRACER_UTILS_LOG_ERROR("viz prewarm failed: {}",
                                             prewarmed.error().message());
            });
#endif
            auto on_connection =
                [router_ptr](int client_fd,
                             struct sockaddr_in addr) -> coro::CoroTask<void> {
                co_await handle_connection(client_fd, addr, *router_ptr);
            };
            co_await listener.accept_loop(ctx, on_connection);
        });
    if (!ran) co_return binaries::report_error(ran.error());

    std::fprintf(stderr, "Server shut down gracefully\n");

    co_return 0;
}

int main(int argc, char** argv) {
    return binaries::cli_main<ServerArgParse>(
        argc, argv, "dftracer_server",
        "Serve DFTracer trace data over HTTP. Query, filter, and stream "
        "trace events via REST API.",
        [](ServerArgParse& cli) {
            install_signal_handlers();
            return run_server(&cli).get();
        });
}
