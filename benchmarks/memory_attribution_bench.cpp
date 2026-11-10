// SPDX-License-Identifier: MIT
// Benchmark: how much of a workload's memory the memory pools explain.
// Each workload runs in its own child process (fork) so the peak of one does
// not hide the next. A bench thread samples the memory probe and
// memory::detail::pool_stats() every millisecond; the parent prints per
// workload the peak and end RSS, the peak anon bytes, the peak `operator new`
// live bytes, the peak live and reserved bytes of the root pool and of the
// workload's own pools ("query" for a lazy plan, "genesis/..." for genesis,
// "fold buffers" for the index build), and unattributed = peak anon - peak root
// live. Memory that no pool holds (the C library) is unattributed.
//
//   memory_attribution_bench           (larger sizes)
//   memory_attribution_bench --smoke   (small sizes plus the checks, for ctest)
//
// The checks: a genesis pool showed live or reserved bytes during genesis,
// root live is at most anon + 16 MiB, and `operator new` live is at most
// anon + 16 MiB. The two live checks fail only under --smoke, since bytes that
// are allocated but never touched are live without being resident; a full run
// prints them as "over" and does not count them.

#define DFTRACER_UTILS_MEMORY_PROBE_DEFINE_NEW
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/thread.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/indexer.h>
#include <dftracer/utils/memory/detail/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/genesis/genesis.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <memory_probe.h>
#include <memory_workloads.h>
#include <simdjson.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

using namespace dftracer::utils;
using namespace dftu::memory_workloads;

namespace {

constexpr std::uint64_t MIB = 1024ULL * 1024ULL;
constexpr std::int64_t LIVE_OVER_ANON_SLACK =
    static_cast<std::int64_t>(16 * MIB);
constexpr std::chrono::milliseconds SAMPLE_INTERVAL{1};
constexpr std::uint64_t SPILL_BUDGET = 8 * MIB;

struct Sizes {
    std::int64_t frame_rows;
    std::int64_t trace_lines;
    std::int64_t genesis_calls;
};

constexpr Sizes SMOKE_SIZES{200'000, 2'000, 200'000};
constexpr Sizes FULL_SIZES{8'000'000, 4'000'000, 2'000'000};

enum class Workload : std::uint8_t {
    GroupBy,
    Sort,
    Join,
    IndexBuild,
    JsonRead,
    Genesis,
    SpillCycle,
};

// The pools a workload's own bytes sit in.
enum class PoolGroup : std::uint8_t { None, Query, Genesis, FoldBuffers };

struct WorkloadInfo {
    Workload id;
    const char* name;
    PoolGroup group;
};

constexpr WorkloadInfo WORKLOADS[] = {
    {Workload::GroupBy, "group_by", PoolGroup::Query},
    {Workload::Sort, "sort", PoolGroup::Query},
    {Workload::Join, "join", PoolGroup::Query},
    {Workload::IndexBuild, "index_build", PoolGroup::FoldBuffers},
    {Workload::JsonRead, "json_read", PoolGroup::None},
    {Workload::Genesis, "genesis", PoolGroup::Genesis},
    {Workload::SpillCycle, "spill_cycle", PoolGroup::Query},
};

const char* group_name(PoolGroup g) {
    switch (g) {
        case PoolGroup::None:
            return "-";
        case PoolGroup::Query:
            return "query";
        case PoolGroup::Genesis:
            return "genesis/";
        case PoolGroup::FoldBuffers:
            return "fold buffers";
    }
    return "-";
}

bool ends_with(dstd::StringView s, dstd::StringView tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

bool in_group(const memory::detail::PoolStats& p, PoolGroup g) {
    switch (g) {
        case PoolGroup::None:
            return false;
        case PoolGroup::Query:
            return ends_with(p.path, "/query");
        case PoolGroup::Genesis:
            return p.path.find("/genesis/") != dstd::String::npos;
        case PoolGroup::FoldBuffers:
            return ends_with(p.path, "/fold buffers");
    }
    return false;
}

struct Peaks {
    std::uint64_t peak_rss = 0;
    std::uint64_t end_rss = 0;
    std::uint64_t peak_anon = 0;
    std::int64_t peak_live = 0;
    std::uint64_t peak_root_live = 0;
    std::uint64_t peak_root_reserved = 0;
    std::uint64_t peak_group_live = 0;
    std::uint64_t peak_group_reserved = 0;
    std::uint64_t malloc_in_use = 0;
    std::uint64_t malloc_heap = 0;
    std::uint64_t end_anon = 0;
};

struct Paths {
    dstd::String trace;
    dstd::String genesis_root;
    dstd::String index_dir;
};

class Sampler {
   public:
    explicit Sampler(PoolGroup group)
        : group_(group), thread_([this] { loop(); }) {}
    ~Sampler() { stop(); }

    Peaks stop() {
        if (!stopped_) {
            stopped_ = true;
            run_.store(false);
            thread_.join();
            sample();
        }
        Peaks r = result_;
        const dftu::memory_probe::Reading end = dftu::memory_probe::read();
        r.peak_rss = end.peak_rss;
        r.end_rss = end.current_rss;
        r.end_anon = end.anon;
        return r;
    }

   private:
    void loop() {
        while (run_.load()) {
            sample();
            std::this_thread::sleep_for(SAMPLE_INTERVAL);
        }
    }

    void sample() {
        const dftu::memory_probe::Reading r = dftu::memory_probe::read();
        const std::int64_t live = dftu::memory_probe::live_bytes();
        const auto pools = memory::detail::pool_stats();
        std::uint64_t group_live = 0;
        std::uint64_t group_reserved = 0;
        for (const memory::detail::PoolStats& p : pools) {
            if (p.path == "root") {
                result_.peak_root_live =
                    std::max<std::uint64_t>(result_.peak_root_live, p.live);
                result_.peak_root_reserved = std::max<std::uint64_t>(
                    result_.peak_root_reserved, p.reserved);
            }
            // live and reserved cover a pool's subtree, so only the topmost
            // pool of the group counts.
            if (!in_group(p, group_) || parent_in_group(pools, p)) continue;
            group_live += p.live;
            group_reserved += p.reserved;
        }
        result_.peak_anon = std::max(result_.peak_anon, r.anon);
        result_.peak_live = std::max(result_.peak_live, live);
        result_.peak_group_live = std::max(result_.peak_group_live, group_live);
        result_.peak_group_reserved =
            std::max(result_.peak_group_reserved, group_reserved);
    }

    bool parent_in_group(const std::vector<memory::detail::PoolStats>& pools,
                         const memory::detail::PoolStats& p) const {
        std::uint16_t id = p.parent;
        for (std::size_t depth = 0; depth < pools.size(); ++depth) {
            const memory::detail::PoolStats* up = nullptr;
            for (const memory::detail::PoolStats& q : pools)
                if (q.id == id) up = &q;
            if (up == nullptr || up->path == "root") return false;
            if (in_group(*up, group_)) return true;
            id = up->parent;
        }
        return false;
    }

    PoolGroup group_;
    std::atomic<bool> run_{true};
    bool stopped_ = false;
    Peaks result_;
    dstd::Thread thread_;
};

void run_plan(const dataframe::LazyFrame& plan, std::uint64_t budget) {
    require_ok(
        default_runtime().submit(drain_count(plan.memory_budget(budget))).get(),
        "task");
}

template <class Fn>
void run_scoped(Fn fn) {
    require_ok(default_runtime().submit(coro::with_scope(std::move(fn))).wait(),
               "task");
}

void group_by(const Sizes& s) {
    run_plan(
        gen(s.frame_rows, s.frame_rows, 1)
            .group_by(dstd::Vector<dstd::String>{"k", "v"},
                      {dataframe::GroupAgg{dataframe::Agg::Sum, "t", "t_sum"}}),
        PLAN_BUDGET);
}

void sort(const Sizes& s) {
    run_plan(gen(s.frame_rows, s.frame_rows, 1).sort_by("v"), PLAN_BUDGET);
}

void join(const Sizes& s) {
    run_plan(gen(s.frame_rows, s.frame_rows, 1)
                 .join(gen(s.frame_rows / 4, s.frame_rows, 2), {"k"}, {"k"},
                       dataframe::JoinHow::Inner, "_r"),
             PLAN_BUDGET);
}

void spill_cycle(const Sizes& s) {
    run_plan(gen(s.frame_rows, s.frame_rows, 1).sort_by("v"), SPILL_BUDGET);
}

void index_build(const Paths& p) {
    require_ok(dstd::fs::create_directories(p.index_dir), "create_directories");
    index::IndexerOptions options;
    options.index_dir = p.index_dir;
    options.bloom->required = false;
    auto indexer = dftracer::utils::common::unwrap(index::Indexer::open(
        {dstd::String(p.trace.data(), p.trace.size())}, options));
    run_scoped([&](coro::Scope& scope) -> coro::CoroTask<void> {
        dftracer::utils::common::unwrap(co_await indexer.rebuild(scope));
    });
}

coro::CoroTask<void> read_lines(dstd::String file) {
    simdjson::dom::parser parser;
    auto gen =
        utilities::fileio::lines::sources::async_streaming_gz_lines(file);
    std::uint64_t parsed = 0;
    while (auto line = co_await gen.next()) {
        dstd::StringView l = line->content;
        simdjson::dom::object o;
        if (!l.empty() && l.front() == '{' &&
            parser.parse(l.data(), l.size()).get(o) == simdjson::SUCCESS)
            ++parsed;
    }
    if (parsed == 0) {
        std::fprintf(stderr, "json_read parsed no line\n");
        std::exit(1);
    }
}

void json_read(const Paths& p) {
    require_ok(default_runtime().submit(read_lines(p.trace)).get(), "task");
}

void genesis(const Paths& p) {
    run_scoped(
        [&](coro::Scope& ctx) { return genesis_run(ctx, p.genesis_root); });
}

dstd::String trace_text(std::int64_t lines) {
    static const char* const NAMES[] = {"read", "write", "open", "close"};
    dstd::String out;
    for (std::int64_t i = 0; i < lines; ++i) {
        out += R"({"ph":"X","name":")";
        out += NAMES[i % 4];
        out += R"(","cat":"POSIX","pid":)" + dstd::to_string(i % 4);
        out += R"(,"tid":1,"ts":)" + dstd::to_string(1000 + i * 10);
        out += R"(,"dur":)" + dstd::to_string(1 + i % 50);
        out +=
            R"(,"args":{"fname":"/data/f)" + dstd::to_string(i % 8) + "\"}}\n";
    }
    return out;
}

Paths prepare(const dstd::String& dir, const Sizes& s) {
    dstd::fs::remove_all(dir).discard();
    Paths p;
    p.trace = dir + "/trace/smoke.pfw.gz";
    write_gz(p.trace, trace_text(s.trace_lines));
    p.index_dir = dir + "/index";
    const dstd::String run = dir + "/genesis/app1/sys1/in1/nodes_1/ppn_1";
    require_ok(dstd::fs::create_directories(run), "create_directories");
    require_ok(
        dstd::fs::write_file(
            run + "/summary.json",
            common::str_cat(
                R"({"ok":true,"app":"app1","system":"sys1","input":"in1",)"
                R"("nodes":1,"ppn":)",
                RANKS, R"(,"sets":["set1"]})")),
        "write summary.json");
    write_gz(run + "/compacted/set1.pfw.gz", genesis_text(s.genesis_calls));
    p.genesis_root = dir + "/genesis";
    return p;
}

void run_workload(Workload w, const Sizes& s, const Paths& p) {
    switch (w) {
        case Workload::GroupBy:
            return group_by(s);
        case Workload::Sort:
            return sort(s);
        case Workload::Join:
            return join(s);
        case Workload::IndexBuild:
            return index_build(p);
        case Workload::JsonRead:
            return json_read(p);
        case Workload::Genesis:
            return genesis(p);
        case Workload::SpillCycle:
            return spill_cycle(s);
    }
}

#if defined(__GLIBC__)
void read_malloc(Peaks& r) {
#if __GLIBC_PREREQ(2, 33)
    const struct mallinfo2 info = mallinfo2();
#else
    const struct mallinfo info = mallinfo();
#endif
    r.malloc_in_use = static_cast<std::uint64_t>(info.uordblks) +
                      static_cast<std::uint64_t>(info.hblkhd);
    r.malloc_heap = static_cast<std::uint64_t>(info.arena) +
                    static_cast<std::uint64_t>(info.hblkhd);
}
#endif

int child_main(int fd, const WorkloadInfo& w, const Sizes& s, const Paths& p) {
    Peaks r;
    {
        Sampler sampler(w.group);
        run_workload(w.id, s, p);
        r = sampler.stop();
    }
#if defined(__GLIBC__)
    read_malloc(r);
#endif
    const ssize_t n = ::write(fd, &r, sizeof r);
    return n == static_cast<ssize_t>(sizeof r) ? 0 : 1;
}

bool run_child(const WorkloadInfo& w, const Sizes& s, const Paths& p,
               Peaks& out) {
    int fds[2];
    if (::pipe(fds) != 0) return false;
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::close(fds[0]);
        const int rc = child_main(fds[1], w, s, p);
        ::close(fds[1]);
        ::_exit(rc);
    }
    ::close(fds[1]);
    Peaks r;
    std::size_t got = 0;
    while (got < sizeof r) {
        const ssize_t n =
            ::read(fds[0], reinterpret_cast<char*>(&r) + got, sizeof r - got);
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    out = r;
    return got == sizeof r && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

double mib(std::uint64_t bytes) { return static_cast<double>(bytes) / MIB; }

}  // namespace

int main(int argc, char** argv) {
    const bool smoke = argc == 2 && dstd::StringView(argv[1]) == "--smoke";
    if (argc > 2 || (argc == 2 && !smoke)) {
        std::fprintf(stderr, "usage: memory_attribution_bench [--smoke]\n");
        return 2;
    }
    const Sizes sizes = smoke ? SMOKE_SIZES : FULL_SIZES;
    const dstd::String dir(
        (dstd::fs::temp_directory() / "memory_attribution_bench").str());
    const Paths paths = prepare(dir, sizes);

    int failures = 0;
    std::printf("%-12s %9s %9s %9s %9s %9s %9s %-13s %9s %9s %12s\n",
                "workload", "peak_rss", "end_rss", "peak_anon", "peak_new",
                "root_live", "root_res", "pools", "live", "reserved",
                "unattributed");
    for (const WorkloadInfo& info : WORKLOADS) {
#if !defined(__GLIBC__)
        if (info.id == Workload::SpillCycle) continue;
#endif
        Peaks r;
        if (!run_child(info, sizes, paths, r)) {
            std::printf("%-12s child failed  FAIL\n", info.name);
            ++failures;
            continue;
        }
        const std::int64_t unattributed =
            static_cast<std::int64_t>(r.peak_anon) -
            static_cast<std::int64_t>(r.peak_root_live);
        std::printf(
            "%-12s %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f %-13s %9.1f %9.1f "
            "%12.1f\n",
            info.name, mib(r.peak_rss), mib(r.end_rss), mib(r.peak_anon),
            static_cast<double>(r.peak_live) / MIB, mib(r.peak_root_live),
            mib(r.peak_root_reserved), group_name(info.group),
            mib(r.peak_group_live), mib(r.peak_group_reserved),
            static_cast<double>(unattributed) / MIB);
#if defined(__GLIBC__)
        if (info.id == Workload::SpillCycle)
            std::printf(
                "%-12s glibc after one spill cycle: mallinfo in use %.1f MiB, "
                "heap %.1f MiB, end RSS %.1f MiB, end anon %.1f MiB\n",
                info.name, mib(r.malloc_in_use), mib(r.malloc_heap),
                mib(r.end_rss), mib(r.end_anon));
#endif
        if (info.id == Workload::Genesis && r.peak_group_live == 0 &&
            r.peak_group_reserved == 0) {
            std::printf("%-12s no genesis pool held bytes  FAIL\n", info.name);
            ++failures;
        }
        if (static_cast<std::int64_t>(r.peak_root_live) >
            static_cast<std::int64_t>(r.peak_anon) + LIVE_OVER_ANON_SLACK) {
            std::printf(
                "%-12s root live %.1f MiB above anon %.1f MiB + 16  %s\n",
                info.name, mib(r.peak_root_live), mib(r.peak_anon),
                smoke ? "FAIL" : "over");
            if (smoke) ++failures;
        }
        if (r.peak_live >
            static_cast<std::int64_t>(r.peak_anon) + LIVE_OVER_ANON_SLACK) {
            std::printf("%-12s live %.1f MiB above anon %.1f MiB + 16  %s\n",
                        info.name, static_cast<double>(r.peak_live) / MIB,
                        mib(r.peak_anon), smoke ? "FAIL" : "over");
            if (smoke) ++failures;
        }
    }
    std::printf("sizes in MiB; unattributed = peak anon - peak root live\n");
    return failures == 0 ? 0 : 1;
}
