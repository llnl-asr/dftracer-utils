#include <dftracer/utils/core/common/config.h>
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

#include <cstdint>
#include <cstdio>
#include <string>

#include "common_cli.h"

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
    }

    void post_parse() override {
        index_dir = parser().get<std::string>("--index-dir");
        bind_addr = parser().get<std::string>("--bind");
        auth_token = parser().get<std::string>("--token");
        port = parser().get<uint16_t>("--port");
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

    auto pipeline_config =
        cli::build_pipeline_config("DFTracer Server", cli->pipeline);
    pipeline_config.with_io_backend(io::IoBackendType::THREADPOOL)
        .with_io_batch_size(1)
        .with_watchdog(false)
        .with_global_timeout(std::chrono::seconds(0))
        .with_task_timeout(std::chrono::seconds(0));

    Pipeline pipeline(pipeline_config);

    TraceIndex trace_index(dir, index_dir, executor_threads);
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
