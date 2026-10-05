#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
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

    explicit GenDistArgParse(argparse::ArgumentParser& p) : ArgParse(p) {
        schema(pipeline);
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

struct GroupMsg {
    std::size_t index = 0;
    std::string lines;
    std::size_t runs = 0;
    std::vector<genesis::Skip> skips;
};

struct BudgetPermit {
    coro::CoroSemaphore& sem;
    std::uint64_t bytes;
    BudgetPermit(coro::CoroSemaphore& s, std::uint64_t n) : sem(s), bytes(n) {}
    ~BudgetPermit() { sem.release(bytes); }
    BudgetPermit(const BudgetPermit&) = delete;
    BudgetPermit& operator=(const BudgetPermit&) = delete;
};

coro::CoroTask<void> group_feeder(coro::ChannelProducer<std::size_t> producer,
                                  std::size_t n) {
    auto guard = producer.guard();
    for (std::size_t i = 0; i < n; ++i)
        if (!co_await producer.send(i)) break;
}

coro::CoroTask<void> group_worker(CoroScope& ctx,
                                  coro::ChannelConsumer<std::size_t> in,
                                  coro::ChannelProducer<GroupMsg> out,
                                  const std::vector<genesis::RunGroup>* groups,
                                  coro::CoroSemaphore* budget) {
    auto guard = out.guard();
    while (auto i = co_await in.receive()) {
        const genesis::RunGroup& g = (*groups)[*i];
        const std::uint64_t reserve = estimate_per_file_bytes(
            {static_cast<std::size_t>(g.compressed_bytes)});
        co_await budget->acquire(reserve);
        GroupMsg msg;
        msg.index = *i;
        {
            BudgetPermit permit{*budget, reserve};
            auto result = co_await genesis::process_group(ctx, g);
            for (const auto& run : result.runs) {
                msg.lines += run.lines;
                ++msg.runs;
            }
            msg.skips.insert(msg.skips.end(),
                             std::make_move_iterator(result.skips.begin()),
                             std::make_move_iterator(result.skips.end()));
        }
        if (!co_await out.send(std::move(msg))) break;
    }
}

struct WriteTotals {
    std::size_t runs = 0;
    std::vector<genesis::Skip> skips;
    bool io_error = false;
};

// Appends group lines to one writer in discovery order as they arrive.
coro::CoroTask<void> ordered_writer(coro::ChannelConsumer<GroupMsg> in,
                                    const std::string* path,
                                    std::size_t member_size,
                                    WriteTotals* totals) {
    fileio::GzipWriterOptions opts;
    opts.member_size = member_size;
    auto opened = co_await fileio::GzipLineWriter::open(*path, std::move(opts));
    if (!opened) totals->io_error = true;
    std::size_t next_expected = 0;
    std::map<std::size_t, GroupMsg> pending;
    auto write = [&](GroupMsg& m) -> coro::CoroTask<void> {
        if (!totals->io_error && !m.lines.empty() &&
            !(co_await opened->append(m.lines)))
            totals->io_error = true;
        totals->runs += m.runs;
        totals->skips.insert(totals->skips.end(), m.skips.begin(),
                             m.skips.end());
        ++next_expected;
    };
    while (auto msg = co_await in.receive()) {
        if (msg->index != next_expected) {
            pending.emplace(msg->index, std::move(*msg));
            continue;
        }
        co_await write(*msg);
        for (auto it = pending.find(next_expected); it != pending.end();
             it = pending.find(next_expected)) {
            co_await write(it->second);
            pending.erase(it);
        }
    }
    if (!totals->io_error && !(co_await opened->close()))
        totals->io_error = true;
}

coro::CoroTask<int> run_gen_dist(CoroScope& ctx, const GenDistArgParse* cli) {
    genesis::Discovery discovery = co_await genesis::discover(ctx, cli->roots);
    const std::size_t threads =
        std::max<std::size_t>(1, cli->pipeline.executor_threads);
    coro::CoroSemaphore budget(compute_memory_budget());
    WriteTotals totals;
    totals.skips = std::move(discovery.skips);

    auto indices = coro::make_channel<std::size_t>(threads * 2);
    auto results = coro::make_channel<GroupMsg>(threads * 2);
    const auto* groups = &discovery.groups;
    const auto* output = &cli->output;
    const std::size_t member_size = cli->member_size;
    auto* budget_ptr = &budget;
    auto* totals_ptr = &totals;
    co_await ctx.scope([&indices, &results, groups, output, budget_ptr,
                        totals_ptr, member_size,
                        threads](CoroScope& scope) -> coro::CoroTask<void> {
        scope.spawn(
            [ch = indices->producer(), n = groups->size()](CoroScope&) mutable {
                return group_feeder(std::move(ch), n);
            });
        for (std::size_t w = 0; w < threads; ++w) {
            scope.spawn([in = indices->consumer(), out = results->producer(),
                         groups, budget_ptr](CoroScope& s) mutable {
                return group_worker(s, in, std::move(out), groups, budget_ptr);
            });
        }
        scope.spawn([ch = results->consumer(), output, member_size,
                     totals_ptr](CoroScope&) {
            return ordered_writer(ch, output, member_size, totals_ptr);
        });
        co_return;
    });

    if (totals.io_error) {
        DFTRACER_UTILS_LOG_ERROR("Cannot write %s", cli->output.c_str());
        co_return 1;
    }
    for (const auto& s : totals.skips)
        std::fprintf(stderr, "skipped: %s [%s] %s\n", s.dir.c_str(),
                     s.file.c_str(), s.reason.c_str());
    std::printf("Wrote %zu run(s) to %s, skipped %zu\n", totals.runs,
                cli->output.c_str(), totals.skips.size());
    co_return totals.skips.empty() ? 0 : 1;
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
