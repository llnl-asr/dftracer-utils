#include <dftracer/utils/binaries/common_cli.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/str_format.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_MPI
#include <dftracer/utils/core/distributed/mpi_transport.h>
#include <mpi.h>
#endif

using namespace dftracer::utils;
namespace genesis = dftracer::utils::trace::genesis;
namespace fileio = dftracer::utils::utilities::fileio;

// Part `index` of `count` of the run groups.
struct Shard {
    std::size_t index = 0;
    std::size_t count = 1;
};

class GenDistArgParse : public cli::ArgParse {
   public:
    std::vector<std::string> roots;
    std::string output;
    std::size_t member_size = 0;
    std::string shard_text;
    Shard shard;
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
        parser()
            .add_argument("--shard")
            .help(
                "Process only part i of N of the run groups, as i/N with 0 <= "
                "i < N; the parts split the trace bytes evenly. Under MPI "
                "with more than one rank, each rank takes its part by default")
            .default_value(std::string());
    }

    void post_parse() override {
        roots = parser().get<std::vector<std::string>>("roots");
        output = parser().get<std::string>("--output");
        member_size = cli::get_bytes_arg(parser(), "--member-size");
        shard_text = parser().get<std::string>("--shard");
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
        if (!shard_text.empty() && !parse_shard()) {
            DFTRACER_UTILS_LOG_ERROR("--shard must be i/N with 0 <= i < N: %s",
                                     shard_text.c_str());
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

   private:
    bool parse_shard() {
        const std::string_view t(shard_text);
        const auto slash = t.find('/');
        if (slash == std::string_view::npos) return false;
        auto number = [](std::string_view v, std::size_t& out) {
            const auto r = std::from_chars(v.data(), v.data() + v.size(), out);
            return r.ec == std::errc() && r.ptr == v.data() + v.size() &&
                   !v.empty();
        };
        Shard sh;
        if (!number(t.substr(0, slash), sh.index) ||
            !number(t.substr(slash + 1), sh.count) || sh.count == 0 ||
            sh.index >= sh.count)
            return false;
        shard = sh;
        return true;
    }
};

namespace {

// The groups of `shard`: largest first, each group goes to the part with the
// fewest trace bytes so far (the lowest part on a tie), so the parts are even
// and every process that sees the same groups picks the same ones.
std::vector<genesis::RunGroup> take_shard(std::vector<genesis::RunGroup> groups,
                                          Shard shard) {
    std::vector<std::size_t> order(groups.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(
        order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return groups[a].compressed_bytes > groups[b].compressed_bytes;
        });
    std::vector<std::uint64_t> load(shard.count, 0);
    std::vector<genesis::RunGroup> mine;
    for (const std::size_t i : order) {
        const auto part = static_cast<std::size_t>(
            std::min_element(load.begin(), load.end()) - load.begin());
        load[part] += groups[i].compressed_bytes;
        if (part == shard.index) mine.push_back(std::move(groups[i]));
    }
    return mine;
}

std::string shard_tag(Shard shard) {
    if (shard.count <= 1) return "[genesis]";
    return str_cat("[genesis ", shard.index, "/", shard.count, "]");
}

// What one process wrote, for the summary line and the MPI join.
struct Report {
    std::size_t runs = 0;
    std::size_t skips = 0;
};

coro::CoroTask<void> group_feeder(coro::ChannelProducer<std::size_t> producer,
                                  const std::vector<std::size_t>* order) {
    auto guard = producer.guard();
    for (const std::size_t i : *order)
        if (!co_await producer.send(i)) break;
}

// Peak resident memory of this process so far, in bytes.
std::uint64_t peak_rss_bytes() {
    struct rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
#endif
}

std::string clock_text(double seconds) {
    const auto t = static_cast<long long>(seconds);
    char buf[32];
    if (t >= 3600)
        std::snprintf(buf, sizeof buf, "%lldh%02lldm%02llds", t / 3600,
                      t / 60 % 60, t % 60);
    else
        std::snprintf(buf, sizeof buf, "%lldm%02llds", t / 60, t % 60);
    return buf;
}

// `done` and `total` bytes in the unit of `total`, as in "68.3/68.8 GB".
std::string bytes_of(double done, double total) {
    static const char* const UNITS[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int i = 0;
    while (total >= 1024.0 && i < 5) {
        done /= 1024.0;
        total /= 1024.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f/%.1f %s", done, total, UNITS[i]);
    return buf;
}

// `done` and `total` counts in the unit of `total`, as in "1.9/5.1 G".
std::string count_of(double done, double total) {
    static const char* const UNITS[] = {"", " K", " M", " G", " T"};
    int i = 0;
    while (total >= 1000.0 && i < 4) {
        done /= 1000.0;
        total /= 1000.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, i ? "%.1f/%.1f%s" : "%.0f/%.0f%s", done,
                  total, UNITS[i]);
    return buf;
}

std::string rate_of(double per_second) {
    static const char* const UNITS[] = {"", " K", " M", " G", " T"};
    int i = 0;
    while (per_second >= 1000.0 && i < 4) {
        per_second /= 1000.0;
        ++i;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f%s/s", per_second, UNITS[i]);
    return buf;
}

using Phase = genesis::GroupProgress::Phase;
constexpr std::size_t PHASES = 5;

const char* phase_name(Phase p) {
    switch (p) {
        case Phase::WAIT:
            return "wait";
        case Phase::READ:
            return "read";
        case Phase::SORT:
            return "sort";
        case Phase::WALK:
            return "walk";
        case Phase::WRITE:
            return "write";
    }
    return "?";
}

// The group one worker processes, and the counters process_group updates.
struct Slot {
    std::atomic<const genesis::RunGroup*> group{nullptr};
    std::atomic<std::int64_t> start_ms{0};
    genesis::GroupProgress live;
};

// Prints the progress of the run to stderr. Whatever updates a count (a
// reader, a sort or walk task, a worker) prints when PROGRESS_INTERVAL has
// passed since the last line, so a run shows how far it is without a thread
// of its own. At most every GROUP_INTERVAL the line is followed by one line
// per group that has run for SLOW_GROUP or more. Rates and the ETA come from
// the change since the previous line.
class Progress {
   public:
    static constexpr std::chrono::seconds PROGRESS_INTERVAL{10};
    static constexpr std::chrono::seconds GROUP_INTERVAL{60};
    static constexpr std::chrono::seconds SLOW_GROUP{60};

    Progress(std::string tag, std::size_t groups, std::uint64_t bytes,
             std::size_t workers, std::uint64_t budget)
        : tag_(std::move(tag)),
          total_groups_(groups),
          total_bytes_(bytes),
          budget_(budget),
          workers_(workers),
          slots_(std::make_unique<Slot[]>(workers)) {
        for (std::size_t w = 0; w < workers_; ++w)
            slots_[w].live.on_update = [this] { maybe_print(false); };
    }

    const std::string& tag() const { return tag_; }

    // The counters for worker `w`, which starts `group`.
    genesis::GroupProgress* begin(std::size_t w, const genesis::RunGroup* g) {
        Slot& s = slots_[w];
        s.live.reset();
        s.start_ms.store(elapsed_ms());
        s.group.store(g);
        return &s.live;
    }

    void retry(std::size_t w) { slots_[w].live.reset(); }

    void writing(std::size_t w) { slots_[w].live.enter(Phase::WRITE); }

    void end(std::size_t w, std::size_t runs) {
        Slot& s = slots_[w];
        const genesis::RunGroup* g = s.group.exchange(nullptr);
        if (g) done_bytes_.fetch_add(g->compressed_bytes);
        done_calls_.fetch_add(s.live.calls.load());
        runs_.fetch_add(runs);
        maybe_print(groups_.fetch_add(1) + 1 == total_groups_);
    }

   private:
    void maybe_print(bool last_group) {
        const auto now = elapsed_ms();
        auto last = last_ms_.load();
        if (!last_group &&
            now - last < std::chrono::milliseconds(PROGRESS_INTERVAL).count())
            return;
        if (!last_ms_.compare_exchange_strong(last, now)) return;
        std::lock_guard<std::mutex> lock(print_mu_);
        print(now, last_group);
    }

    std::int64_t elapsed_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start_)
            .count();
    }

    std::string prefix(std::int64_t now_ms) const {
        return str_cat(tag_.substr(0, tag_.size() - 1), " ",
                       clock_text(static_cast<double>(now_ms) / 1000.0), "]");
    }

    void print(std::int64_t now_ms, bool last_group) {
        double read = static_cast<double>(done_bytes_.load());
        double calls = static_cast<double>(done_calls_.load());
        double sorted = calls;
        double walked = calls;
        std::size_t in_phase[PHASES] = {};
        for (std::size_t w = 0; w < workers_; ++w) {
            const Slot& s = slots_[w];
            const genesis::RunGroup* g = s.group.load();
            if (!g) continue;
            const Phase ph = s.live.phase.load(std::memory_order_relaxed);
            ++in_phase[static_cast<std::size_t>(ph)];
            if (ph == Phase::READ)
                read += static_cast<double>(
                    std::min(s.live.read_bytes.load(), g->compressed_bytes));
            else if (ph != Phase::WAIT)
                read += static_cast<double>(g->compressed_bytes);
            calls += static_cast<double>(s.live.calls.load());
            sorted += static_cast<double>(s.live.sorted.load());
            walked += static_cast<double>(s.live.walked.load());
        }
        const double dt = static_cast<double>(now_ms - prev_ms_) / 1000.0;
        const double read_rate = dt > 0 ? (read - prev_read_) / dt : 0;
        const double sort_rate = dt > 0 ? (sorted - prev_sorted_) / dt : 0;
        const double walk_rate = dt > 0 ? (walked - prev_walked_) / dt : 0;
        const double call_rate = sort_rate + walk_rate;
        prev_ms_ = now_ms;
        prev_read_ = read;
        prev_sorted_ = sorted;
        prev_walked_ = walked;

        const double read_left = static_cast<double>(total_bytes_) - read;
        const double calls_left = (calls - sorted) + (calls - walked);
        // Reading uses the rate since the start: groups that wait for memory
        // read nothing for a while, which a recent rate would take as a stall.
        const double secs = static_cast<double>(now_ms) / 1000.0;
        const double read_avg = secs > 0 ? read / secs : 0;
        std::string eta = "?";
        if ((read_left <= 0 || read_avg > 0) &&
            (calls_left <= 0 || call_rate > 0))
            eta = clock_text((read_left > 0 ? read_left / read_avg : 0) +
                             (calls_left > 0 ? calls_left / call_rate : 0));

        const std::string pre = prefix(now_ms);
        std::fprintf(
            stderr,
            "%s groups %zu/%zu | phase wait %zu, read %zu, sort %zu, walk %zu, "
            "write %zu "
            "| read %s %s | sort %s calls %s | walk %s calls %s | mem %s peak "
            "| "
            "ETA %s\n",
            pre.c_str(), groups_.load(), total_groups_, in_phase[0],
            in_phase[1], in_phase[2], in_phase[3], in_phase[4],
            bytes_of(read, static_cast<double>(total_bytes_)).c_str(),
            human_bytes(std::max(0.0, read_rate), "/s").c_str(),
            count_of(sorted, calls).c_str(),
            rate_of(std::max(0.0, sort_rate)).c_str(),
            count_of(walked, calls).c_str(),
            rate_of(std::max(0.0, walk_rate)).c_str(),
            bytes_of(static_cast<double>(peak_rss_bytes()),
                     static_cast<double>(budget_))
                .c_str(),
            eta.c_str());
        if (last_group || now_ms - last_groups_ms_ <
                              std::chrono::milliseconds(GROUP_INTERVAL).count())
            return;
        last_groups_ms_ = now_ms;
        const std::int64_t now_ns =
            std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::size_t w = 0; w < workers_; ++w) {
            const Slot& s = slots_[w];
            const genesis::RunGroup* g = s.group.load();
            if (!g) continue;
            const std::int64_t ran = now_ms - s.start_ms.load();
            if (ran < std::chrono::milliseconds(SLOW_GROUP).count()) continue;
            const Phase ph = s.live.phase.load(std::memory_order_relaxed);
            const double c = static_cast<double>(s.live.calls.load());
            std::string count;
            if (ph == Phase::READ)
                count = bytes_of(static_cast<double>(s.live.read_bytes.load()),
                                 static_cast<double>(g->compressed_bytes));
            else if (ph == Phase::SORT)
                count = count_of(static_cast<double>(s.live.sorted.load()), c) +
                        " calls";
            else if (ph == Phase::WALK)
                count = count_of(static_cast<double>(s.live.walked.load()), c) +
                        " calls";
            const double in_ph =
                static_cast<double>(now_ns - s.live.phase_since_ns.load()) /
                1e9;
            std::fprintf(stderr, "%s   %s  %s %s  %s in %s, %s in group\n",
                         pre.c_str(), g->rel_dir.c_str(), phase_name(ph),
                         count.c_str(), clock_text(in_ph).c_str(),
                         phase_name(ph),
                         clock_text(static_cast<double>(ran) / 1000.0).c_str());
        }
    }

    const std::string tag_;
    const std::chrono::steady_clock::time_point start_ =
        std::chrono::steady_clock::now();
    const std::size_t total_groups_;
    const std::uint64_t total_bytes_;
    const std::uint64_t budget_;
    const std::size_t workers_;
    std::unique_ptr<Slot[]> slots_;
    std::atomic<std::size_t> groups_{0};
    std::atomic<std::uint64_t> done_bytes_{0};
    std::atomic<std::uint64_t> done_calls_{0};
    std::atomic<std::size_t> runs_{0};
    std::atomic<std::int64_t> last_ms_{0};
    std::mutex print_mu_;
    std::int64_t prev_ms_ = 0;
    double prev_read_ = 0;
    double prev_sorted_ = 0;
    double prev_walked_ = 0;
    std::int64_t last_groups_ms_ = 0;
};

struct WorkerTotals {
    std::size_t runs = 0;
    std::vector<genesis::Skip> skips;
    bool io_error = false;
};

constexpr std::size_t SLICE_BYTES = 1024 * 1024;
constexpr std::size_t MIN_MEMBER_BYTES = 1024 * 1024;

// The groups' part of the budget and how one group's share of it is chosen.
// A group starts with its part of the budget by trace bytes, at least the even
// split `base`, so a large group reads many files at once. Its counter
// statistics and path records grow with call paths times counters, not with
// bytes, so a group over its share runs again with four times as much, up to
// the whole budget.
struct Shares {
    std::uint64_t budget;
    std::uint64_t base;
    std::uint64_t input_bytes;

    std::uint64_t first(const genesis::RunGroup& g) const {
        if (input_bytes == 0) return base;
        const double part = static_cast<double>(budget) *
                            (static_cast<double>(g.compressed_bytes) /
                             static_cast<double>(input_bytes));
        if (part >= static_cast<double>(budget)) return budget;
        return std::max(base, static_cast<std::uint64_t>(part));
    }

    std::uint64_t next(std::uint64_t share) const {
        return share > budget / 4 ? budget : share * 4;
    }
};

// Processes groups as they come and writes each run through this worker's own
// producer, so no run waits for another and the members are compressed and
// written in parallel, in the order they finish. A run is made and written
// SLICE_BYTES of lines at a time, which the producer gathers into members, and
// its group holds its share of `memory` until its runs are written.
coro::CoroTask<void> group_worker(
    CoroScope& ctx, coro::ChannelConsumer<std::size_t> in,
    fileio::GzipLineWriter* writer, fileio::GzipLineWriter* head_writer,
    const std::vector<genesis::RunGroup>* groups, coro::CoroSemaphore* memory,
    Shares shares, WorkerTotals* totals, Progress* progress, std::size_t w) {
    auto prod = writer->producer();
    auto head_prod = head_writer->producer();
    if (!prod || !head_prod) {
        totals->io_error = true;
        co_return;
    }
    std::string slice;
    std::string head;
    // Free memory of the budget lent to the group being processed, returned
    // with its share; only a group that cannot borrow runs again.
    std::atomic<std::uint64_t> borrowed{0};
    const genesis::BorrowFn borrow =
        [memory, &borrowed](std::uint64_t n) -> std::uint64_t {
        if (!memory->try_acquire(n)) return 0;
        borrowed.fetch_add(n);
        return n;
    };
    while (auto i = co_await in.receive()) {
        const genesis::RunGroup& group = (*groups)[*i];
        std::uint64_t share = shares.first(group);
        genesis::GroupProgress* live = progress->begin(w, &group);
        co_await memory->acquire(share);
        auto result =
            co_await genesis::process_group(ctx, group, share, live, &borrow);
        while (result.over_budget && share < shares.budget) {
            result = {};
            memory->release(share + borrowed.exchange(0));
            const std::uint64_t larger = shares.next(share);
            std::fprintf(stderr, "%s %s needs more than %s, retrying with %s\n",
                         progress->tag().c_str(), group.dir.c_str(),
                         human_bytes(static_cast<double>(share)).c_str(),
                         human_bytes(static_cast<double>(larger)).c_str());
            share = larger;
            progress->retry(w);
            co_await memory->acquire(share);
            result = co_await genesis::process_group(ctx, group, share, live,
                                                     &borrow);
        }
        progress->writing(w);
        for (auto& run : result.runs) {
            while (!totals->io_error && run.next(head, slice, SLICE_BYTES)) {
                // Named, not awaited in the condition (the GCC 12 trap in the
                // coroutine caveats).
                if (!slice.empty()) {
                    const auto ok = co_await prod->append(slice);
                    if (!ok) totals->io_error = true;
                }
                if (!head.empty()) {
                    const auto ok = co_await head_prod->append(head);
                    if (!ok) totals->io_error = true;
                }
                slice.clear();
                head.clear();
            }
            ++totals->runs;
        }
        const std::size_t runs = result.runs.size();
        result.runs.clear();
        memory->release(share + borrowed.exchange(0));
        totals->skips.insert(totals->skips.end(),
                             std::make_move_iterator(result.skips.begin()),
                             std::make_move_iterator(result.skips.end()));
        progress->end(w, runs);
    }
    if (!totals->io_error) {
        const auto ok = co_await prod->flush();
        if (!ok) totals->io_error = true;
    }
    if (!totals->io_error) {
        const auto ok = co_await head_prod->flush();
        if (!ok) totals->io_error = true;
    }
}

constexpr std::size_t COPY_BYTES = 8 * 1024 * 1024;

// Copies all of `from` into `to` at `offset`.
bool copy_into(const std::string& from, int to, std::uint64_t offset) {
    const int in = ::open(from.c_str(), O_RDONLY);
    if (in < 0) return false;
    auto buf = std::make_unique_for_overwrite<char[]>(COPY_BYTES);
    bool ok = true;
    for (;;) {
        const ssize_t n = ::read(in, buf.get(), COPY_BYTES);
        if (n < 0) {
            ok = false;
            break;
        }
        if (n == 0) break;
        for (ssize_t done = 0; done < n;) {
            const ssize_t w = ::pwrite(to, buf.get() + done,
                                       static_cast<std::size_t>(n - done),
                                       static_cast<off_t>(offset));
            if (w <= 0) {
                ok = false;
                break;
            }
            done += w;
            offset += static_cast<std::uint64_t>(w);
        }
        if (!ok) break;
    }
    ::close(in);
    return ok;
}

// Writes this process's groups to `output`. Skips go to stderr; the summary
// line is the caller's, after an MPI join.
coro::CoroTask<int> run_gen_dist(CoroScope& ctx, const GenDistArgParse* cli,
                                 Shard shard, std::string output,
                                 Report* report) {
    genesis::Discovery discovery = co_await genesis::discover(ctx, cli->roots);
    if (shard.count > 1) {
        discovery.groups = take_shard(std::move(discovery.groups), shard);
        // Every part sees the same discovery, so only the first reports it.
        if (shard.index != 0) discovery.skips.clear();
    }
    // An eighth of the budget for the writers' buffers and in-flight members,
    // the rest for the groups being read. The run and func records, about a
    // hundredth of the bytes, get a quarter of the writers' part.
    const std::uint64_t total = resolve_spill_budget(cli->memory.memory_budget);
    const std::uint64_t head_budget = total / 32;
    const std::uint64_t writer_budget = total / 8 - head_budget;
    const std::uint64_t budget = total - writer_budget - head_budget;
    const std::size_t threads = concurrent_spill_ways(
        budget, std::min<std::size_t>(
                    std::max<std::size_t>(1, cli->pipeline.executor_threads),
                    std::max<std::size_t>(1, discovery.groups.size())));
    const std::uint64_t base = share_spill_budget(budget, threads);
    // Every worker's producer fills a member from the half of the writer
    // budget kept for buffers being filled, and holds up to a member and a
    // line. When those do not all fit at once, every producer can wait for
    // another, so a member takes at most half of a worker's part of that half.
    auto member_size_of = [&](std::uint64_t writer_bytes) {
        return static_cast<std::size_t>(std::min<std::uint64_t>(
            cli->member_size,
            std::max<std::uint64_t>(MIN_MEMBER_BYTES,
                                    writer_bytes / 4 / threads)));
    };
    const std::size_t member_size = member_size_of(writer_budget);
    auto open_writer = [&](const std::string& path, std::uint64_t bytes)
        -> coro::CoroTask<std::optional<fileio::GzipLineWriter>> {
        fileio::GzipWriterOptions opts;
        opts.member_size = member_size_of(bytes);
        opts.workers = std::max<std::size_t>(1, cli->pipeline.executor_threads);
        opts.memory_budget = bytes;
        opts.ordered = false;
        auto opened =
            co_await fileio::GzipLineWriter::open(path, std::move(opts));
        if (!opened) {
            DFTRACER_UTILS_LOG_ERROR("Cannot write %s: %s", path.c_str(),
                                     opened.error().message.c_str());
            co_return std::nullopt;
        }
        co_return std::move(*opened);
    };
    // The run and func records go to their own members after the counters,
    // so a query on them reads a few chunks, not every chunk of every run.
    const std::string head_path = output + ".head";
    auto head_opened = co_await open_writer(head_path, head_budget);
    if (!head_opened) co_return 1;
    auto body_opened = co_await open_writer(output, writer_budget);
    if (!body_opened) co_return 1;
    fileio::GzipLineWriter writer = std::move(*body_opened);
    fileio::GzipLineWriter head_writer = std::move(*head_opened);

    std::uint64_t input_bytes = 0;
    for (const auto& g : discovery.groups) input_bytes += g.compressed_bytes;
    const Shares shares{budget, base, input_bytes};
    const std::string tag = shard_tag(shard);
    std::fprintf(stderr,
                 "%s %zu run groups, %s of traces, up to %zu at a time, "
                 "%s to %s per group, members of %s\n",
                 tag.c_str(), discovery.groups.size(),
                 human_bytes(static_cast<double>(input_bytes)).c_str(), threads,
                 human_bytes(static_cast<double>(base)).c_str(),
                 human_bytes(static_cast<double>(budget)).c_str(),
                 human_bytes(static_cast<double>(member_size)).c_str());
    Progress progress(tag, discovery.groups.size(), input_bytes, threads,
                      total);

    // Largest groups first, so the longest ones do not start last.
    std::vector<std::size_t> order(discovery.groups.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) {
                         return discovery.groups[a].compressed_bytes >
                                discovery.groups[b].compressed_bytes;
                     });
    coro::CoroSemaphore memory(budget);

    std::vector<WorkerTotals> totals(threads);
    auto indices = coro::make_channel<std::size_t>(threads * 2);
    const auto* groups = &discovery.groups;
    const auto* order_ptr = &order;
    auto* writer_ptr = &writer;
    auto* head_writer_ptr = &head_writer;
    auto* totals_ptr = totals.data();
    auto* progress_ptr = &progress;
    auto* memory_ptr = &memory;
    co_await ctx.scope([&indices, groups, order_ptr, memory_ptr, shares,
                        writer_ptr, head_writer_ptr, totals_ptr, threads,
                        progress_ptr](
                           CoroScope& scope) -> coro::CoroTask<void> {
        scope.spawn([ch = indices->producer(), order_ptr](CoroScope&) mutable {
            return group_feeder(std::move(ch), order_ptr);
        });
        for (std::size_t w = 0; w < threads; ++w) {
            scope.spawn([in = indices->consumer(), writer_ptr, head_writer_ptr,
                         groups, memory_ptr, shares, t = totals_ptr + w,
                         progress_ptr, w](CoroScope& s) {
                return group_worker(s, in, writer_ptr, head_writer_ptr, groups,
                                    memory_ptr, shares, t, progress_ptr, w);
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
    auto head_closed = co_await head_writer.close();
    if (closed && !head_closed) closed = std::move(head_closed);
    bool appended = false;
    if (!io_error && closed) {
        std::error_code ec;
        const std::uint64_t end = fs::file_size(output, ec);
        const int fd = ec ? -1 : ::open(output.c_str(), O_WRONLY);
        appended = fd >= 0 && copy_into(head_path, fd, end);
        if (fd >= 0 && ::close(fd) != 0) appended = false;
    }
    {
        std::error_code ec;
        fs::remove(head_path, ec);
    }
    if (io_error || !closed || !appended) {
        DFTRACER_UTILS_LOG_ERROR("Cannot write %s%s%s", output.c_str(),
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
    report->runs = runs;
    report->skips = skips.size();
    co_return 0;
}

int run_local(GenDistArgParse& cli, Shard shard, const std::string& output,
              Report* report) {
    return cli::run_single_task("DFTracer Genesis Gen Dist", cli.pipeline,
                                [&cli, shard, output, report](CoroScope& ctx) {
                                    return run_gen_dist(ctx, &cli, shard,
                                                        output, report);
                                });
}

void print_summary(const Report& r, const std::string& output) {
    std::printf("Wrote %zu run(s) to %s, skipped %zu\n", r.runs, output.c_str(),
                r.skips);
}

#ifdef DFTRACER_UTILS_ENABLE_MPI

bool all_ranks(bool ok) {
    int mine = ok ? 1 : 0;
    int all = 0;
    MPI_Allreduce(&mine, &all, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD);
    return all == 1;
}

// Joins every rank's part into `output` in rank order: gzip members
// concatenate into one valid file. Rank 0 sizes the file, then every rank
// copies its part to its offset at the same time.
int join_parts(const std::string& output, const std::string& part, int rank,
               int size, bool ok, const Report& report) {
    std::error_code ec;
    const std::uint64_t mine = ok ? fs::file_size(part, ec) : 0;
    if (!all_ranks(ok && !ec)) {
        fs::remove(part, ec);
        return 1;
    }
    std::vector<std::uint64_t> sizes(static_cast<std::size_t>(size));
    MPI_Allgather(&mine, 1, MPI_UINT64_T, sizes.data(), 1, MPI_UINT64_T,
                  MPI_COMM_WORLD);
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    for (int r = 0; r < size; ++r) {
        if (r < rank) offset += sizes[static_cast<std::size_t>(r)];
        total += sizes[static_cast<std::size_t>(r)];
    }
    bool made = true;
    if (rank == 0) {
        const int fd =
            ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        made = fd >= 0 && ::ftruncate(fd, static_cast<off_t>(total)) == 0;
        if (fd >= 0) ::close(fd);
    }
    bool copied = all_ranks(made);
    if (copied) {
        const int fd = ::open(output.c_str(), O_WRONLY);
        copied = fd >= 0 && copy_into(part, fd, offset);
        if (fd >= 0 && ::close(fd) != 0) copied = false;
    }
    fs::remove(part, ec);
    copied = all_ranks(copied);
    unsigned long long counts[2] = {report.runs, report.skips};
    unsigned long long sums[2] = {0, 0};
    MPI_Reduce(counts, sums, 2, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0,
               MPI_COMM_WORLD);
    if (rank != 0) return copied ? 0 : 1;
    if (!copied) {
        DFTRACER_UTILS_LOG_ERROR("Cannot write %s", output.c_str());
        fs::remove(output, ec);
        return 1;
    }
    print_summary(
        {static_cast<std::size_t>(sums[0]), static_cast<std::size_t>(sums[1])},
        output);
    return 0;
}

// Ranks on one node share its memory, so an automatic budget is split
// between them.
std::size_t node_ranks() {
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm local = MPI_COMM_NULL;
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank,
                        MPI_INFO_NULL, &local);
    int n = 1;
    MPI_Comm_size(local, &n);
    MPI_Comm_free(&local);
    return static_cast<std::size_t>(std::max(1, n));
}

int run_mpi(GenDistArgParse& cli) {
    int rank = 0;
    int size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (size == 1) {
        Report report;
        const int rc = run_local(cli, cli.shard, cli.output, &report);
        if (rc == 0) print_summary(report, cli.output);
        return rc;
    }
    if (!cli.shard_text.empty()) {
        if (rank == 0)
            DFTRACER_UTILS_LOG_ERROR(
                "--shard cannot be used with more than one MPI rank: the "
                "ranks take their parts themselves");
        return 1;
    }
    if (cli.memory.memory_budget == 0)
        cli.memory.memory_budget = std::max<std::uint64_t>(
            MIN_MEMORY_BUDGET_BYTES, resolve_spill_budget(0) / node_ranks());
    const Shard shard{static_cast<std::size_t>(rank),
                      static_cast<std::size_t>(size)};
    const std::string part = str_cat(cli.output, ".", rank, ".part");
    Report report;
    const int rc = run_local(cli, shard, part, &report);
    return join_parts(cli.output, part, rank, size, rc == 0, report);
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef DFTRACER_UTILS_ENABLE_MPI
    MPI_Init(&argc, &argv);
    distributed::set_local_thread_budget();
#endif
    const int rc = cli::cli_main<GenDistArgParse>(
        argc, argv, "dftracer_genesis_gen_dist",
        "Build per-call-path duration and counter distributions from genesis "
        "sweep traces into one combined .pfw.gz trace",
        [](GenDistArgParse& cli) {
#ifdef DFTRACER_UTILS_ENABLE_MPI
            return run_mpi(cli);
#else
            Report report;
            const int code = run_local(cli, cli.shard, cli.output, &report);
            if (code == 0) print_summary(report, cli.output);
            return code;
#endif
        });
#ifdef DFTRACER_UTILS_ENABLE_MPI
    MPI_Finalize();
#endif
    return rc;
}
