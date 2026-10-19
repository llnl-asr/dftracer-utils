// Harness: peak memory, spilled bytes and wall time of every LazyFrame op that
// can spill, at memory budgets of 4 MiB, 64 MiB, 1 GiB and 4 GiB.
//
// The input is generated per morsel from a fixed seed by a streaming Source,
// so nothing but the op's own state is resident. The output is drained morsel
// by morsel and dropped. The budget is set with LazyFrame::memory_budget(),
// which is what the lazy cursors read; DFTRACER_UTILS_MEM_BUDGET sets the
// process-wide memory manager and is left alone.
//
// Data size is max(2 x budget, 64 MiB), capped at 512 MiB. The main frame has
// the columns
//   k Int64   random in [0, 2^40)            sort key, top-k
//   g Int64   random in [0, rows / 4)        group_by, unique, head_by, join
//   p Int64   random in [0, 8)               asof and interval partition,
//                                            window partition, pivot on
//   t Int64   10 x row, ascending            asof, interval, window order
//   v Float64 random in [0, 1000)
//   s String  13 bytes, random               sort_by
// Cases:
//   sort_by        sort_by("s")
//   group_by       group_by("g", sum v, count v)
//   unique         unique({"g"})
//   is_duplicated  select({"g"}).is_duplicated()
//   join inner     inner join on g with a 4096-row table: the build fits
//   join grace     inner join on g with a frame as large as the main one whose
//                  g is skewed (a quarter of rows on 16 hot keys)
//   asof           asof on t by p, backward, with rows / 2 rows
//   interval       interval of t in [lo, lo + 1600] by p, with rows / 16 ranges
//                  spaced 160 apart, so a point falls in about 10 ranges
//   topk           topk("k", 1000)
//   head_by        head_by({"g"}, 2)
//   window         lag(v) over (partition by p order by t)
//   pivot          pivot(index g, on p, values v, sum)
//
// Each case runs in its own process. Peak is the child's ru_maxrss minus its
// resident set just before the op starts. Spilled bytes are the bytes the
// process wrote during the op (macOS: proc_pid_rusage ri_logical_writes;
// Linux: /proc/self/io wchar), because spill files are unlinked at creation
// and no op keeps a counter.
//
//   spill_harness            all cases, a table per budget
//   spill_harness --smoke    in this process, about 1 MiB of data, budget
//                            4 MiB, every case once with a row check
//   spill_harness --case NAME BUDGET_BYTES
//                            one case in this process; prints RESULT

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/runtime.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#endif

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace df = dftracer::utils::dataframe;
namespace coro = dftracer::utils::coro;

namespace {

using Clock = std::chrono::steady_clock;
using df::DataFrame;
using df::LazyFrame;
using df::Series;

constexpr std::uint64_t MIB = std::uint64_t{1} << 20;
constexpr std::uint64_t GIB = std::uint64_t{1} << 30;
constexpr std::uint64_t BUDGETS[] = {4 * MIB, 64 * MIB, 1 * GIB, 4 * GIB};
constexpr std::uint64_t SMOKE_BUDGET = 4 * MIB;
constexpr std::uint64_t SMOKE_DATA = 1 * MIB;
constexpr std::uint64_t MIN_DATA = 64 * MIB;
constexpr std::uint64_t MAX_DATA = 512 * MIB;
constexpr std::uint64_t SPILLING_BUDGET_MAX = 64 * MIB;
constexpr int SPILLING_RUNS = 3;
constexpr std::int64_t MORSEL_ROWS = 65536;
constexpr std::uint64_t SEED = 42;
constexpr std::int64_t HOT_KEYS = 16;
constexpr std::int64_t DIM_ROWS = 4096;
constexpr std::int64_t PARTITIONS = 8;
constexpr std::int64_t TIME_STEP = 10;
constexpr std::int64_t RANGE_SPACING = 16 * TIME_STEP;
constexpr std::int64_t RANGE_LENGTH = 10 * RANGE_SPACING;
constexpr std::int64_t TOPK = 1000;
constexpr std::int64_t HEAD_BY_N = 2;
// Bytes per main-frame row: four Int64, one Float64, a 13-byte string and its
// 8-byte offset.
constexpr std::uint64_t ROW_BYTES = 5 * 8 + 13 + 8;

void check(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

// splitmix64 finalizer, Sebastiano Vigna, public domain
// (https://prng.di.unimi.it/splitmix64.c).
std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

enum class Gen : std::uint8_t { Random, Skewed, Time, Float, Text };

struct Column {
    std::string name;
    Gen gen;
    std::int64_t mod = 0;
    std::int64_t step = 0;
    std::int64_t offset = 0;
};

struct Shape {
    std::vector<Column> columns;
    std::int64_t rows = 0;
    std::uint64_t seed = 0;
};

df::DataType type_of(Gen gen) {
    const std::int64_t i = 0;
    const double f = 0;
    switch (gen) {
        case Gen::Random:
        case Gen::Skewed:
        case Gen::Time:
            return Series::flat_i64(&i, 1).data_type();
        case Gen::Float:
            return Series::flat_f64(&f, 1).data_type();
        case Gen::Text:
            return Series::strings(std::vector<std::string>{""}).data_type();
    }
    throw std::logic_error("unknown generator");
}

Series generate(const Column& c, std::size_t salt, std::uint64_t seed,
                std::int64_t from, std::int64_t n) {
    auto hash = [&](std::int64_t row) {
        return mix(seed ^ (salt * 0x100000001b3ULL) ^
                   static_cast<std::uint64_t>(row));
    };
    auto count = static_cast<std::size_t>(n);
    if (c.gen == Gen::Float) {
        std::vector<double> out(count);
        for (std::size_t j = 0; j < count; ++j)
            out[j] = static_cast<double>(
                         hash(from + static_cast<std::int64_t>(j)) >> 11) *
                     0x1.0p-53 * 1000.0;
        return Series::flat_f64(out.data(), n);
    }
    if (c.gen == Gen::Text) {
        std::vector<std::string> out(count);
        char buffer[16];
        for (std::size_t j = 0; j < count; ++j) {
            std::snprintf(buffer, sizeof(buffer), "s%012" PRIu64,
                          static_cast<std::uint64_t>(
                              hash(from + static_cast<std::int64_t>(j)) %
                              1000000000000ULL));
            out[j] = buffer;
        }
        return Series::strings(out);
    }
    std::vector<std::int64_t> out(count);
    for (std::size_t j = 0; j < count; ++j) {
        const std::int64_t row = from + static_cast<std::int64_t>(j);
        const std::uint64_t h = hash(row);
        const auto mod =
            static_cast<std::uint64_t>(std::max<std::int64_t>(c.mod, 1));
        switch (c.gen) {
            case Gen::Random:
                out[j] = static_cast<std::int64_t>(h % mod);
                break;
            case Gen::Skewed:
                out[j] = static_cast<std::int64_t>(
                    (h >> 62) == 0 ? h % static_cast<std::uint64_t>(HOT_KEYS)
                                   : h % mod);
                break;
            case Gen::Time:
                out[j] = row * c.step + c.offset;
                break;
            case Gen::Float:
            case Gen::Text:
                break;
        }
    }
    return Series::flat_i64(out.data(), n);
}

// Generates each morsel from (seed, column, row), so the data never lives in
// memory as a whole and any morsel size gives the same rows.
class GenSource : public df::Source {
   public:
    explicit GenSource(Shape shape)
        : shape_(std::make_shared<const Shape>(std::move(shape))) {}

    df::Schema schema() const override {
        df::Schema out;
        for (const Column& c : shape_->columns)
            out.fields.push_back(df::Field{c.name, type_of(c.gen), true});
        return out;
    }

    df::ScanResult scan(const df::ScanRequest& req) const override {
        std::vector<std::size_t> picked;
        if (req.projection.empty()) {
            for (std::size_t c = 0; c < shape_->columns.size(); ++c)
                picked.push_back(c);
        } else {
            for (const std::string& name : req.projection) {
                auto it = std::find_if(
                    shape_->columns.begin(), shape_->columns.end(),
                    [&](const Column& c) { return c.name == name; });
                check(it != shape_->columns.end(), "projection: no column");
                picked.push_back(
                    static_cast<std::size_t>(it - shape_->columns.begin()));
            }
        }
        df::ScanResult r;
        r.cursor = std::make_unique<Rows>(shape_, std::move(picked));
        r.filters.assign(req.filters.size(), df::Pushed::No);
        return r;
    }

   private:
    class Rows : public df::Cursor {
       public:
        Rows(std::shared_ptr<const Shape> shape,
             std::vector<std::size_t> picked)
            : shape_(std::move(shape)), picked_(std::move(picked)) {}

        coro::CoroTask<std::optional<df::Morsel>> next(
            std::int64_t max_rows) override {
            if (pos_ >= shape_->rows) co_return std::nullopt;
            const std::int64_t want = max_rows > 0 ? max_rows : MORSEL_ROWS;
            const std::int64_t n = std::min(want, shape_->rows - pos_);
            df::Morsel m;
            m.rows = n;
            for (std::size_t c : picked_)
                m.columns.push_back(
                    generate(shape_->columns[c], c, shape_->seed, pos_, n));
            pos_ += n;
            co_return m;
        }

       private:
        std::shared_ptr<const Shape> shape_;
        std::vector<std::size_t> picked_;
        std::int64_t pos_ = 0;
    };

    std::shared_ptr<const Shape> shape_;
};

std::uint64_t data_bytes(std::uint64_t budget) {
    return std::min(std::max(2 * budget, MIN_DATA), MAX_DATA);
}

struct Inputs {
    std::int64_t rows;
    std::uint64_t budget;

    LazyFrame scan(Shape shape) const {
        return LazyFrame::scan(std::make_shared<GenSource>(std::move(shape)))
            .memory_budget(budget);
    }

    LazyFrame main() const {
        return scan({{{"k", Gen::Random, std::int64_t{1} << 40},
                      {"g", Gen::Random, groups()},
                      {"p", Gen::Random, PARTITIONS},
                      {"t", Gen::Time, 0, TIME_STEP},
                      {"v", Gen::Float},
                      {"s", Gen::Text}},
                     rows,
                     SEED});
    }

    std::int64_t groups() const { return std::max<std::int64_t>(rows / 4, 1); }
};

LazyFrame window_lag(const LazyFrame& plan) {
    const char* part[] = {"p"};
    const char* order[] = {"t"};
    std::vector<dftu_window_spec> specs(1);
    specs[0] = dftu_window_spec{};
    specs[0].func = DFTU_WINDOW_LAG;
    specs[0].value = "v";
    specs[0].out = "lag_v";
    specs[0].param.offset = 1;
    df::OpArgs args;
    args.strlist(1, part, 1).strlist(2, order, 1).winlist(3, specs);
    std::vector<std::string> names = plan.schema();
    names.emplace_back("lag_v");
    return plan.frame_op("dftu.frame.window", args, {}, std::move(names));
}

struct Case {
    std::string name;
    std::function<LazyFrame(const Inputs&)> plan;
    std::function<bool(std::int64_t out_rows, const Inputs&)> rows_ok;
};

bool any_rows(std::int64_t out, const Inputs&) { return out > 0; }
bool all_rows(std::int64_t out, const Inputs& in) { return out == in.rows; }
bool at_most_groups(std::int64_t out, const Inputs& in) {
    return out > 0 && out <= in.groups();
}

std::vector<Case> make_cases() {
    std::vector<df::GroupAgg> aggs{{df::Agg::Sum, "v", "sum_v"},
                                   {df::Agg::Count, "v", "count_v"}};
    return {
        {"sort_by", [](const Inputs& in) { return in.main().sort_by("s"); },
         all_rows},
        {"group_by",
         [aggs](const Inputs& in) { return in.main().group_by("g", aggs); },
         at_most_groups},
        {"unique", [](const Inputs& in) { return in.main().unique({"g"}); },
         at_most_groups},
        {"is_duplicated",
         [](const Inputs& in) {
             return in.main().select({"g"}).is_duplicated();
         },
         all_rows},
        {"join inner",
         [](const Inputs& in) {
             LazyFrame dim =
                 in.scan({{{"g", Gen::Time, 0, 1}, {"w", Gen::Float}},
                          DIM_ROWS,
                          SEED + 1});
             return in.main().join(dim, std::vector<std::string>{"g"});
         },
         any_rows},
        {"join grace",
         [](const Inputs& in) {
             LazyFrame big =
                 in.scan({{{"k", Gen::Random, std::int64_t{1} << 40},
                           {"g", Gen::Skewed, in.groups()},
                           {"p", Gen::Random, PARTITIONS},
                           {"t", Gen::Time, 0, TIME_STEP},
                           {"v", Gen::Float},
                           {"s", Gen::Text}},
                          in.rows,
                          SEED + 2});
             return in.main().join(big, std::vector<std::string>{"g"});
         },
         any_rows},
        {"asof",
         [](const Inputs& in) {
             LazyFrame right = in.scan({{{"t", Gen::Time, 0, 2 * TIME_STEP, 5},
                                         {"p", Gen::Random, PARTITIONS},
                                         {"w", Gen::Float}},
                                        std::max<std::int64_t>(in.rows / 2, 1),
                                        SEED + 3});
             return in.main().asof(right, "t", {"p"},
                                   df::AsofDirection::Backward);
         },
         all_rows},
        {"interval",
         [](const Inputs& in) {
             LazyFrame ranges =
                 in.scan({{{"lo", Gen::Time, 0, RANGE_SPACING},
                           {"hi", Gen::Time, 0, RANGE_SPACING, RANGE_LENGTH},
                           {"p", Gen::Random, PARTITIONS},
                           {"w", Gen::Float}},
                          std::max<std::int64_t>(in.rows / 16, 1),
                          SEED + 4});
             return in.main().interval(ranges, "t", "lo", "hi", {"p"});
         },
         any_rows},
        {"topk", [](const Inputs& in) { return in.main().topk("k", TOPK); },
         [](std::int64_t out, const Inputs& in) {
             return out == std::min(TOPK, in.rows);
         }},
        {"head_by",
         [](const Inputs& in) { return in.main().head_by({"g"}, HEAD_BY_N); },
         [](std::int64_t out, const Inputs& in) {
             return out > 0 && out <= HEAD_BY_N * in.groups();
         }},
        {"window", [](const Inputs& in) { return window_lag(in.main()); },
         all_rows},
        {"pivot",
         [](const Inputs& in) { return in.main().pivot("g", "p", "v", "sum"); },
         at_most_groups},
    };
}

coro::CoroTask<std::int64_t> drain(LazyFrame plan) {
    auto gen = plan.stream(MORSEL_ROWS);
    std::int64_t rows = 0;
    while (auto part = co_await gen.next()) rows += part->num_rows();
    co_return rows;
}

std::uint64_t current_rss() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return info.resident_size;
#else
    unsigned long long size = 0;
    unsigned long long resident = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    const int got = std::fscanf(f, "%llu %llu", &size, &resident);
    std::fclose(f);
    if (got != 2) return 0;
    return resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

std::uint64_t peak_rss() {
    struct rusage usage{};
    ::getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

std::uint64_t bytes_written() {
#if defined(__APPLE__)
    rusage_info_v4 info{};
    if (proc_pid_rusage(::getpid(), RUSAGE_INFO_V4,
                        reinterpret_cast<rusage_info_t*>(&info)) != 0)
        return 0;
    return info.ri_logical_writes;
#else
    FILE* f = std::fopen("/proc/self/io", "r");
    if (!f) return 0;
    char key[64];
    unsigned long long value = 0;
    std::uint64_t out = 0;
    while (std::fscanf(f, "%63s %llu", key, &value) == 2) {
        if (std::string_view(key) == "wchar:") {
            out = value;
            break;
        }
    }
    std::fclose(f);
    return out;
#endif
}

const char* written_source() {
#if defined(__APPLE__)
    return "proc_pid_rusage ri_logical_writes";
#else
    return "/proc/self/io wchar";
#endif
}

const Case* find_case(const std::vector<Case>& cases, std::string_view name) {
    for (const Case& c : cases)
        if (c.name == name) return &c;
    return nullptr;
}

std::int64_t rows_for(std::uint64_t bytes) {
    return static_cast<std::int64_t>(bytes / ROW_BYTES);
}

int run_one(std::string_view name, std::uint64_t budget) {
    std::vector<Case> cases = make_cases();
    const Case* c = find_case(cases, name);
    if (!c) {
        std::fprintf(stderr, "unknown case: %.*s\n",
                     static_cast<int>(name.size()), name.data());
        return 1;
    }
    auto& runtime = dftracer::utils::default_runtime();
    Inputs in{rows_for(data_bytes(budget)), budget};
    try {
        LazyFrame plan = c->plan(in);
        const std::uint64_t base = current_rss();
        const std::uint64_t written = bytes_written();
        auto start = Clock::now();
        const std::int64_t out = runtime.submit(drain(std::move(plan))).get();
        auto end = Clock::now();
        const std::uint64_t spilled = bytes_written() - written;
        const std::uint64_t peak = peak_rss();
        check(c->rows_ok(out, in), "unexpected output row count");
        std::printf(
            "RESULT rows=%" PRId64 " out=%" PRId64 " base=%" PRIu64
            " peak=%" PRIu64 " spilled=%" PRIu64 " ns=%.0f headroom=%zu\n",
            in.rows, out, base, peak, spilled,
            std::chrono::duration<double, std::nano>(end - start).count(),
            dftracer::utils::memory::Manager::get().spill_headroom());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: %s\n", c->name.c_str(), e.what());
        return 1;
    }
    return 0;
}

struct ChildResult {
    std::uint64_t base = 0;
    std::uint64_t peak = 0;
    std::uint64_t spilled = 0;
    double ns = 0;
};

std::optional<ChildResult> run_child(const std::string& binary,
                                     const std::string& name,
                                     std::uint64_t budget) {
    int fds[2];
    if (::pipe(fds) != 0) return std::nullopt;
    const std::string budget_arg = std::to_string(budget);
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        ::execl(binary.c_str(), binary.c_str(), "--case", name.c_str(),
                budget_arg.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);
    std::string out;
    char buffer[256];
    ssize_t n;
    while ((n = ::read(fds[0], buffer, sizeof(buffer))) > 0)
        out.append(buffer, static_cast<std::size_t>(n));
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ChildResult r;
    unsigned long long base = 0;
    unsigned long long peak = 0;
    unsigned long long spilled = 0;
    std::size_t at = out.find("RESULT ");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        at == std::string::npos ||
        std::sscanf(out.c_str() + at,
                    "RESULT rows=%*s out=%*s base=%llu peak=%llu "
                    "spilled=%llu ns=%lf",
                    &base, &peak, &spilled, &r.ns) != 4) {
        return std::nullopt;
    }
    r.base = base;
    r.peak = peak;
    r.spilled = spilled;
    return r;
}

template <class T>
T median(std::vector<T> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double mib(std::uint64_t bytes) {
    return static_cast<double>(bytes) / static_cast<double>(MIB);
}

std::string budget_label(std::uint64_t budget) {
    return budget >= GIB ? std::to_string(budget / GIB) + " GiB"
                         : std::to_string(budget / MIB) + " MiB";
}

int drive(const std::string& self) {
    std::vector<Case> cases = make_cases();
    const std::size_t headroom =
        dftracer::utils::memory::Manager::get().spill_headroom();
    std::printf(
        "spill_harness hw=%u budget=LazyFrame::memory_budget "
        "spilled=%s spill_headroom=%.1f MiB\n",
        std::thread::hardware_concurrency(), written_source(), mib(headroom));
    std::printf(
        "\n| op | budget | data MiB | runs | peak above baseline MiB | "
        "spilled MiB | wall ms | over budget | over budget + headroom |\n"
        "|---|---|---|---|---|---|---|---|---|\n");
    std::fflush(stdout);
    int failures = 0;
    for (std::uint64_t budget : BUDGETS) {
        const int runs = budget <= SPILLING_BUDGET_MAX ? SPILLING_RUNS : 1;
        for (const Case& c : cases) {
            std::vector<std::uint64_t> above;
            std::vector<std::uint64_t> spilled;
            std::vector<double> ns;
            for (int run = 0; run < runs; ++run) {
                auto r = run_child(self, c.name, budget);
                if (!r) break;
                above.push_back(r->peak > r->base ? r->peak - r->base : 0);
                spilled.push_back(r->spilled);
                ns.push_back(r->ns);
            }
            if (above.size() != static_cast<std::size_t>(runs)) {
                ++failures;
                std::printf("| %s | %s | %.0f | %d | FAILED | | | | |\n",
                            c.name.c_str(), budget_label(budget).c_str(),
                            mib(data_bytes(budget)), runs);
                std::fflush(stdout);
                continue;
            }
            const std::uint64_t peak = median(above);
            std::printf(
                "| %s | %s | %.0f | %d | %.1f | %.1f | %.1f | %s | %s |\n",
                c.name.c_str(), budget_label(budget).c_str(),
                mib(data_bytes(budget)), runs, mib(peak), mib(median(spilled)),
                median(ns) / 1e6, peak > budget ? "yes" : "no",
                peak > budget + headroom ? "yes" : "no");
            std::fflush(stdout);
        }
    }
    return failures == 0 ? 0 : 1;
}

int smoke() {
    auto& runtime = dftracer::utils::default_runtime();
    Inputs in{rows_for(SMOKE_DATA), SMOKE_BUDGET};
    int failures = 0;
    for (const Case& c : make_cases()) {
        bool ok = false;
        std::int64_t out = -1;
        try {
            out = runtime.submit(drain(c.plan(in))).get();
            ok = c.rows_ok(out, in);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s: %s\n", c.name.c_str(), e.what());
        }
        std::printf("smoke %-14s rows=%" PRId64 " out=%" PRId64 " %s\n",
                    c.name.c_str(), in.rows, out, ok ? "ok" : "FAILED");
        if (!ok) ++failures;
    }
    return failures == 0 ? 0 : 1;
}

int usage() {
    std::fprintf(stderr,
                 "usage: spill_harness [--smoke | --case NAME BUDGET_BYTES]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string_view first = argc > 1 ? argv[1] : "";
    if (argc == 1) return drive(argv[0]);
    if (first == "--smoke" && argc == 2) return smoke();
    if (first == "--case" && argc == 4) {
        char* end = nullptr;
        const unsigned long long budget = std::strtoull(argv[3], &end, 10);
        if (end == argv[3] || *end != '\0' || budget == 0) return usage();
        return run_one(argv[2], budget);
    }
    return usage();
}
