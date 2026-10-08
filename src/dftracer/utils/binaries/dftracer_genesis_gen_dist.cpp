#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace dftracer::utils;
namespace genesis = dftracer::utils::trace::genesis;
namespace fileio = dftracer::utils::utilities::fileio;

class GenDistArgParse : public cli::ArgParse {
   public:
    std::vector<std::string> roots;
    std::string output;
    std::size_t member_size = 0;
    cli::PipelineArgs pipeline;
    cli::MemoryArgs memory;

    explicit GenDistArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(pipeline);
        schema(memory);
    }

   protected:
    void register_args() override {
        parser()
            .add_argument("roots")
            .help("Genesis trace roots to walk for nodes_<N>/ppn_<M> runs")
            .nargs(argparse::nargs_pattern::at_least_one);
        parser()
            .add_argument("-o", "--output")
            .help("Output .pfw.gz file")
            .required();
        parser()
            .add_argument("--member-size")
            .help(
                "Gzip member size (default: the index checkpoint size). "
                "Accepts units, e.g. 512KB, 32MB")
            .default_value(
                std::to_string(constants::indexer::DEFAULT_CHECKPOINT_SIZE));
    }

    void post_parse() override {
        roots = parser().get<std::vector<std::string>>("roots");
        output = parser().get<std::string>("--output");
        member_size = cli::get_bytes_arg(parser(), "--member-size");
    }

    bool validate() override {
        if (!output.ends_with(".pfw.gz")) {
            DFTRACER_UTILS_LOG_ERROR("--output must end with .pfw.gz: %s",
                                     output.c_str());
            return false;
        }
        if (memory.memory_budget != 0 &&
            memory.memory_budget < MIN_MEMORY_BUDGET_BYTES) {
            DFTRACER_UTILS_LOG_ERROR(
                "--memory-budget must be at least %zu bytes (64MB)",
                static_cast<std::size_t>(MIN_MEMORY_BUDGET_BYTES));
            return false;
        }
        for (const auto& r : roots) {
            if (!fs::is_directory(r)) {
                DFTRACER_UTILS_LOG_ERROR("Not a directory: %s", r.c_str());
                return false;
            }
        }
        return true;
    }
};

namespace {

coro::CoroTask<void> group_feeder(coro::ChannelProducer<std::size_t> producer,
                                  std::size_t n) {
    auto guard = producer.guard();
    for (std::size_t i = 0; i < n; ++i)
        if (!co_await producer.send(i)) break;
}

struct WorkerTotals {
    std::size_t runs = 0;
    std::vector<genesis::Skip> skips;
    bool io_error = false;
};

// Appends `lines` in slices of about `slice` bytes cut at a line, so one large
// run reserves one member's worth of the writer budget at a time.
coro::CoroTask<bool> append_sliced(fileio::GzipLineWriter::Producer& prod,
                                   std::string_view lines, std::size_t slice) {
    while (!lines.empty()) {
        std::size_t n = std::min(lines.size(), slice);
        const std::size_t nl = lines.find('\n', n - 1);
        n = nl == std::string_view::npos ? lines.size() : nl + 1;
        if (!co_await prod.append(lines.substr(0, n))) co_return false;
        lines.remove_prefix(n);
    }
    co_return true;
}

// Processes groups as they come and writes each run through this worker's own
// producer, so no run waits for another and the members are compressed and
// written in parallel, in the order they finish.
coro::CoroTask<void> group_worker(CoroScope& ctx,
                                  coro::ChannelConsumer<std::size_t> in,
                                  fileio::GzipLineWriter* writer,
                                  const std::vector<genesis::RunGroup>* groups,
                                  std::uint64_t memory_share,
                                  std::size_t member_size,
                                  WorkerTotals* totals) {
    auto prod = writer->producer();
    if (!prod) {
        totals->io_error = true;
        co_return;
    }
    while (auto i = co_await in.receive()) {
        auto result =
            co_await genesis::process_group(ctx, (*groups)[*i], memory_share);
        for (auto& run : result.runs) {
            if (!totals->io_error &&
                !co_await append_sliced(*prod, run.lines, member_size))
                totals->io_error = true;
            std::string().swap(run.lines);
            ++totals->runs;
        }
        totals->skips.insert(totals->skips.end(),
                             std::make_move_iterator(result.skips.begin()),
                             std::make_move_iterator(result.skips.end()));
    }
    if (!totals->io_error && !co_await prod->flush()) totals->io_error = true;
}

coro::CoroTask<int> run_gen_dist(CoroScope& ctx, const GenDistArgParse* cli) {
    genesis::Discovery discovery = co_await genesis::discover(ctx, cli->roots);
    // An eighth of the budget for the writer's buffers and in-flight members,
    // the rest for the groups being read.
    const std::uint64_t total = resolve_spill_budget(cli->memory.memory_budget);
    const std::uint64_t writer_budget = total / 8;
    const std::uint64_t budget = total - writer_budget;
    const std::size_t threads = concurrent_spill_ways(
        budget, std::min<std::size_t>(
                    std::max<std::size_t>(1, cli->pipeline.executor_threads),
                    std::max<std::size_t>(1, discovery.groups.size())));
    const std::uint64_t share = share_spill_budget(budget, threads);

    fileio::GzipWriterOptions opts;
    opts.member_size = cli->member_size;
    opts.workers = std::max<std::size_t>(1, cli->pipeline.executor_threads);
    opts.memory_budget = writer_budget;
    opts.ordered = false;
    auto opened =
        co_await fileio::GzipLineWriter::open(cli->output, std::move(opts));
    if (!opened) {
        DFTRACER_UTILS_LOG_ERROR("Cannot write %s: %s", cli->output.c_str(),
                                 opened.error().message.c_str());
        co_return 1;
    }
    fileio::GzipLineWriter writer = std::move(*opened);

    std::vector<WorkerTotals> totals(threads);
    auto indices = coro::make_channel<std::size_t>(threads * 2);
    const auto* groups = &discovery.groups;
    const std::size_t member_size = cli->member_size;
    auto* writer_ptr = &writer;
    auto* totals_ptr = totals.data();
    co_await ctx.scope([&indices, groups, share, writer_ptr, totals_ptr,
                        member_size,
                        threads](CoroScope& scope) -> coro::CoroTask<void> {
        scope.spawn(
            [ch = indices->producer(), n = groups->size()](CoroScope&) mutable {
                return group_feeder(std::move(ch), n);
            });
        for (std::size_t w = 0; w < threads; ++w) {
            scope.spawn([in = indices->consumer(), writer_ptr, groups, share,
                         member_size, t = totals_ptr + w](CoroScope& s) {
                return group_worker(s, in, writer_ptr, groups, share,
                                    member_size, t);
            });
        }
        co_return;
    });

    bool io_error = false;
    std::size_t runs = 0;
    std::vector<genesis::Skip> skips = std::move(discovery.skips);
    for (auto& t : totals) {
        io_error = io_error || t.io_error;
        runs += t.runs;
        skips.insert(skips.end(), std::make_move_iterator(t.skips.begin()),
                     std::make_move_iterator(t.skips.end()));
    }
    auto closed = co_await writer.close();
    if (io_error || !closed) {
        DFTRACER_UTILS_LOG_ERROR("Cannot write %s%s%s", cli->output.c_str(),
                                 closed ? "" : ": ",
                                 closed ? "" : closed.error().message.c_str());
        co_return 1;
    }
    std::sort(skips.begin(), skips.end(), [](const auto& a, const auto& b) {
        return std::tie(a.dir, a.file, a.reason) <
               std::tie(b.dir, b.file, b.reason);
    });
    for (const auto& s : skips)
        std::fprintf(stderr, "skipped: %s [%s] %s\n", s.dir.c_str(),
                     s.file.c_str(), s.reason.c_str());
    std::printf("Wrote %zu run(s) to %s, skipped %zu\n", runs,
                cli->output.c_str(), skips.size());
    co_return skips.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    return cli::cli_main<GenDistArgParse>(
        argc, argv, "dftracer_genesis_gen_dist",
        "Build per-call-path duration and counter distributions from genesis "
        "sweep traces into one combined .pfw.gz trace",
        [](GenDistArgParse& cli) {
            return cli::run_single_task(
                "DFTracer Genesis Gen Dist", cli.pipeline,
                [&cli](CoroScope& ctx) { return run_gen_dist(ctx, &cli); });
        });
}
