// Every lazy op must stay within a small multiple of its memory budget over an
// input far larger than the budget, or be listed as known debt. The input is
// generated on demand (it is never resident), the result is streamed and
// dropped, and each op runs in a child process (this binary started again with
// --child) so its peak RSS is measured alone. A debt entry that now meets its
// bound fails the test, so the debt table only shrinks; an op of the registry
// that is in no table fails it too.
//
// DFTU_OP_PEAK_REPORT=1 prints one line per op (peak above the streaming
// baseline) instead of asserting.

#define DOCTEST_CONFIG_IMPLEMENT
#include <dftracer/utils/core/coro/async_generator.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <doctest/doctest.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using dftracer::utils::coro::CoroTask;
using namespace dftracer::utils::dataframe;

namespace {

// 1M rows against an 8 MiB budget keeps the input several times the budget and
// runs in under 10 s. At 4M rows and 16 MiB it took 30 s alone, and the macOS
// coverage job on three cores ran past its 300 s timeout.
std::int64_t rows_count() {
    const char* v = std::getenv("DFTU_OP_PEAK_ROWS");
    return v ? std::strtoll(v, nullptr, 10) : 1'000'000;
}
const std::int64_t ROWS = rows_count();
// DFTU_OP_PEAK_BUDGET_MB overrides the budget (child and parent both read it).
std::uint64_t budget_bytes() {
    const char* v = std::getenv("DFTU_OP_PEAK_BUDGET_MB");
    return (v ? std::strtoull(v, nullptr, 10) : 8ULL) << 20;
}
const std::uint64_t BUDGET = budget_bytes();
constexpr std::int64_t MORSEL_ROWS = 8192;
// An op may use this many budgets above the streaming baseline: its own state
// of one budget, a morsel or two in flight and the allocator's slack.
#if defined(__APPLE__)
constexpr std::uint64_t BOUND_BUDGETS = 5;  // macOS malloc keeps freed pages
#else
constexpr std::uint64_t BOUND_BUDGETS = 3;
#endif

std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

// Columns: k (a key with about `keys` distinct values), t (increasing), v
// (pseudo-random), s (a short string), g (64 distinct values).
class GenCursor : public Cursor {
   public:
    GenCursor(std::int64_t rows, std::int64_t keys, std::uint64_t seed)
        : rows_(rows), keys_(keys), seed_(seed) {}

    CoroTask<std::optional<Morsel>> next(std::int64_t) override {
        if (at_ >= rows_) co_return std::nullopt;
        const std::int64_t n = std::min(MORSEL_ROWS, rows_ - at_);
        std::vector<std::int64_t> k(n), t(n), v(n), g(n);
        std::vector<std::string> s(n);
        for (std::int64_t i = 0; i < n; ++i) {
            const std::uint64_t h =
                mix(seed_ + static_cast<std::uint64_t>(at_ + i));
            k[i] = static_cast<std::int64_t>(h %
                                             static_cast<std::uint64_t>(keys_));
            t[i] = at_ + i;
            v[i] = static_cast<std::int64_t>(h >> 20) % 1000003;
            s[i] = "s" + std::to_string(h % 977);
            g[i] = static_cast<std::int64_t>((h >> 40) % 64);
        }
        at_ += n;
        Morsel m;
        m.rows = n;
        m.columns.push_back(Series::flat_i64(k.data(), n));
        m.columns.push_back(Series::flat_i64(t.data(), n));
        m.columns.push_back(Series::flat_i64(v.data(), n));
        m.columns.push_back(Series::strings(s));
        m.columns.push_back(Series::flat_i64(g.data(), n));
        co_return m;
    }

   private:
    std::int64_t rows_;
    std::int64_t keys_;
    std::uint64_t seed_;
    std::int64_t at_ = 0;
};

class GenSource : public Source {
   public:
    GenSource(std::int64_t rows, std::int64_t keys, std::uint64_t seed)
        : rows_(rows), keys_(keys), seed_(seed) {}

    Schema schema() const override {
        const std::int64_t zero = 0;
        const DataType i = Series::flat_i64(&zero, 1).data_type();
        const std::vector<std::string> blank{""};
        const DataType s = Series::strings(blank).data_type();
        Schema out;
        out.fields = {Field{"k", i, true}, Field{"t", i, true},
                      Field{"v", i, true}, Field{"s", s, true},
                      Field{"g", i, true}};
        return out;
    }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        r.cursor = std::make_unique<GenCursor>(rows_, keys_, seed_);
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

   private:
    std::int64_t rows_;
    std::int64_t keys_;
    std::uint64_t seed_;
};

// Columns: pid (increasing) and tk, a list of two {value, count} structs per
// row.
Series make_tk(std::int64_t n, std::int64_t at) {
    std::vector<std::int32_t> offsets(static_cast<std::size_t>(n) + 1);
    std::vector<std::string> value(static_cast<std::size_t>(2 * n));
    std::vector<std::int64_t> count(static_cast<std::size_t>(2 * n));
    for (std::int64_t i = 0; i < n; ++i) {
        offsets[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(2 * i);
        for (std::int64_t j = 0; j < 2; ++j) {
            const auto e = static_cast<std::size_t>(2 * i + j);
            value[e] = "v" + std::to_string((at + i + j) % 977);
            count[e] = at + i + j;
        }
    }
    offsets[static_cast<std::size_t>(n)] = static_cast<std::int32_t>(2 * n);
    std::vector<Series> fields;
    fields.push_back(Series::strings(value));
    fields.push_back(Series::flat_i64(count.data(), 2 * n));
    return Series::list(offsets,
                        Series::structs({"value", "count"}, std::move(fields)));
}

class ListCursor : public Cursor {
   public:
    explicit ListCursor(std::int64_t rows) : rows_(rows) {}

    CoroTask<std::optional<Morsel>> next(std::int64_t) override {
        if (at_ >= rows_) co_return std::nullopt;
        const std::int64_t n = std::min(MORSEL_ROWS, rows_ - at_);
        std::vector<std::int64_t> pid(static_cast<std::size_t>(n));
        for (std::int64_t i = 0; i < n; ++i)
            pid[static_cast<std::size_t>(i)] = at_ + i;
        Morsel m;
        m.rows = n;
        m.columns.push_back(Series::flat_i64(pid.data(), n));
        m.columns.push_back(make_tk(n, at_));
        at_ += n;
        co_return m;
    }

   private:
    std::int64_t rows_;
    std::int64_t at_ = 0;
};

class ListSource : public Source {
   public:
    explicit ListSource(std::int64_t rows) : rows_(rows) {}

    Schema schema() const override {
        const std::int64_t zero = 0;
        Schema out;
        out.fields = {
            Field{"pid", Series::flat_i64(&zero, 1).data_type(), true},
            Field{"tk", make_tk(1, 0).data_type(), true}};
        return out;
    }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        r.cursor = std::make_unique<ListCursor>(rows_);
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

   private:
    std::int64_t rows_;
};

LazyFrame gen_list(std::int64_t rows) {
    return LazyFrame::scan(std::make_shared<ListSource>(rows));
}

LazyFrame gen(std::int64_t rows, std::int64_t keys, std::uint64_t seed = 1) {
    return LazyFrame::scan(std::make_shared<GenSource>(rows, keys, seed));
}

using Recipe = std::function<LazyFrame(const LazyFrame& base)>;

// Keys are nearly unique (many groups) so a group-by or a distinct cannot fit
// the budget; `few` has a handful of keys.
const std::map<std::string, Recipe>& recipes() {
    static const std::map<std::string, Recipe> r = {
        {"select", [](const LazyFrame& b) { return b.select({"k", "v"}); }},
        {"drop", [](const LazyFrame& b) { return b.drop({"s"}); }},
        {"rename",
         [](const LazyFrame& b) { return b.rename({"a", "b", "c", "d"}); }},
        {"head", [](const LazyFrame& b) { return b.head(ROWS / 2); }},
        {"slice", [](const LazyFrame& b) { return b.slice(10, ROWS / 2); }},
        {"tail", [](const LazyFrame& b) { return b.tail(1000); }},
        {"sample", [](const LazyFrame& b) { return b.sample(1000, 7); }},
        {"topk", [](const LazyFrame& b) { return b.topk("v", 1000); }},
        {"drop_nulls", [](const LazyFrame& b) { return b.drop_nulls(); }},
        {"with_row_index",
         [](const LazyFrame& b) { return b.with_row_index("i"); }},
        {"null_count", [](const LazyFrame& b) { return b.null_count(); }},
        {"describe",
         [](const LazyFrame& b) { return b.select({"k", "v"}).describe(); }},
        {"sort_by", [](const LazyFrame& b) { return b.sort_by("v"); }},
        {"sort_by_multi",
         [](const LazyFrame& b) { return b.sort_by_multi({"k", "v"}); }},
        {"reverse", [](const LazyFrame& b) { return b.reverse(); }},
        {"take", [](const LazyFrame& b) { return b.take({0, 5, 100}); }},
        {"unique", [](const LazyFrame& b) { return b.unique({"k", "v"}); }},
        {"drop_duplicates",
         [](const LazyFrame& b) { return b.drop_duplicates({"k", "v"}); }},
        {"unique_by", [](const LazyFrame& b) { return b.unique({"k", "v"}); }},
        {"is_duplicated",
         [](const LazyFrame& b) {
             return b.select({"k", "v"}).is_duplicated();
         }},
        {"is_unique",
         [](const LazyFrame& b) { return b.select({"k", "v"}).is_unique(); }},
        {"group_by",
         [](const LazyFrame& b) {
             return b.group_by(std::vector<std::string>{"k", "v"},
                               {GroupAgg{Agg::Sum, "t", "t_sum"}});
         }},
        {"reduce",
         [](const LazyFrame& b) { return b.select({"v"}).reduce(Agg::Sum); }},
        {"head_by", [](const LazyFrame& b) { return b.head_by({"k"}, 2); }},
        {"join",
         [](const LazyFrame& b) {
             return b.join(gen(ROWS / 4, ROWS, 2), {"k"}, {"k"}, JoinHow::Inner,
                           "_r");
         }},
        {"concat",
         [](const LazyFrame& b) { return b.concat(gen(ROWS / 4, ROWS, 2)); }},
        {"filter",
         [](const LazyFrame& b) { return b.filter(col(2) > std::int64_t{0}); }},
        {"with_column",
         [](const LazyFrame& b) {
             return b.with_column("c", col(1) + col(2));
         }},
        {"fill_null",
         [](const LazyFrame& b) { return b.fill_null(std::int64_t{0}); }},
        {"unpivot",
         [](const LazyFrame& b) {
             return b.select({"k", "t", "v"}).unpivot({"k"}, {"t", "v"});
         }},
        {"melt",
         [](const LazyFrame& b) {
             return b.select({"k", "t", "v"}).melt({"k"}, {"t", "v"});
         }},
        {"rename_columns",
         [](const LazyFrame& b) { return b.rename_columns({"k"}, {"key"}); }},
        {"pivot",
         [](const LazyFrame& b) { return b.pivot("k", "g", "v", "sum"); }},
        {"group_transform",
         [](const LazyFrame& b) { return b.group_by({"s"}).cumsum(); }},
        {"compare_agg",
         [](const LazyFrame& b) {
             const std::vector<GroupAgg> aggs{GroupAgg{Agg::Sum, "v", "v"}};
             const std::vector<std::string> by{"k"};
             return b.group_by(by, aggs).compare_agg(
                 gen(ROWS, ROWS, 2).group_by(by, aggs), 1);
         }},
        {"explode",
         [](const LazyFrame&) {
             return gen_list(ROWS / 4).memory_budget(BUDGET).explode("tk");
         }},
        {"unnest",
         [](const LazyFrame&) {
             return gen_list(ROWS / 4).memory_budget(BUDGET).unnest("tk");
         }},
        {"filter_mask",
         [](const LazyFrame& b) {
             std::vector<std::uint8_t> bits(
                 static_cast<std::size_t>((ROWS + 7) / 8), 0x55);
             return b.filter_mask(
                 Series::flat(TypeId::Bool, bits.data(), ROWS));
         }},
        {"to_dummies", [](const LazyFrame& b) { return b.to_dummies("s"); }},
        {"group_by_dynamic",
         [](const LazyFrame& b) {
             return b.group_by_dynamic("t", 1000, 1000,
                                       {GroupAgg{Agg::Sum, "v", "v_sum"}});
         }},
    };
    return r;
}

enum class Kind {
    STREAMING,     // holds about one morsel
    SPILLS,        // holds rows but writes them to disk past the budget
    DEBT,          // buffers more than its bound today; must shrink
    NOT_MEASURED,  // the harness has no recipe yet (reason in the table)
};

struct Expect {
    Kind kind;
    const char* note;
    // Operators of one plan that run at the same time each hold a share of the
    // budget, so a plan of several of them is allowed that many bounds.
    std::uint64_t stages = 1;
};

// Every op of exported_lazy_ops.def, in the table that says what it must do.
const std::map<std::string, Expect>& expectations() {
    static const std::map<std::string, Expect> e = {
        {"auto_spill", {Kind::NOT_MEASURED, "plan only"}},
        {"memory_budget", {Kind::NOT_MEASURED, "plan only"}},
        {"describe", {Kind::STREAMING, ""}},
        {"drop_nulls", {Kind::STREAMING, ""}},
        {"null_count", {Kind::STREAMING, ""}},
        {"with_row_index", {Kind::STREAMING, ""}},
        {"head", {Kind::STREAMING, ""}},
        {"slice", {Kind::STREAMING, ""}},
        {"select", {Kind::STREAMING, ""}},
        {"rename", {Kind::STREAMING, ""}},
        {"drop", {Kind::STREAMING, ""}},
        {"concat", {Kind::STREAMING, ""}},
        {"reduce", {Kind::STREAMING, "one aggregate state"}},
        {"tail", {Kind::STREAMING, "bounded by n"}},
        {"sample", {Kind::STREAMING, "bounded by n"}},
        {"topk", {Kind::STREAMING, "bounded by k"}},
        {"join", {Kind::SPILLS, ""}},
        // Spill triggers exist, but memory stays far above the budget.
        {"sort_by", {Kind::SPILLS, ""}},
        {"sort_by_multi", {Kind::SPILLS, ""}},
        {"unique", {Kind::SPILLS, ""}},
        {"drop_duplicates", {Kind::SPILLS, ""}},
        {"unique_by", {Kind::SPILLS, ""}},
        {"is_duplicated", {Kind::SPILLS, ""}},
        {"is_unique", {Kind::SPILLS, ""}},
        {"group_by", {Kind::SPILLS, ""}},
        {"reverse", {Kind::SPILLS, ""}},
        {"take", {Kind::STREAMING, "holds its result"}},
        {"head_by", {Kind::SPILLS, ""}},
        // Needs operands the harness does not build yet.
        {"explode", {Kind::STREAMING, ""}},
        {"unnest", {Kind::STREAMING, ""}},
        {"to_dummies", {Kind::STREAMING, "output is rows x distinct values"}},
        {"filter_mask", {Kind::STREAMING, ""}},
        {"fill_null", {Kind::STREAMING, ""}},
        {"filter", {Kind::STREAMING, ""}},
        {"with_column", {Kind::STREAMING, ""}},
        {"unpivot", {Kind::STREAMING, ""}},
        {"melt", {Kind::STREAMING, ""}},
        {"pivot", {Kind::SPILLS, ""}},
        {"group_by_dynamic", {Kind::STREAMING, "one window per group"}},
        {"compare_agg", {Kind::SPILLS, "two group-bys, a join and a sort", 2}},
        {"group_transform", {Kind::SPILLS, "two sorts and a window chunk", 2}},
        {"rename_columns", {Kind::STREAMING, ""}},
    };
    return e;
}

std::set<std::string> registry_ops() {
    std::set<std::string> names;
#define DFTU_LAZY_OP(name, fn, ret, o0, o1, o2, o3, o4, o5, o6) \
    names.insert(#name);
#include <dftracer/utils/dataframe/exported_lazy_ops.def>
    return names;
}

CoroTask<std::int64_t> drain_count(LazyFrame lf) {
    std::int64_t n = 0;
    auto g = lf.stream(MORSEL_ROWS);
    while (auto df = co_await g.next()) n += df->num_rows();
    co_return n;
}

int child_main(const std::string& op) {
    LazyFrame base = gen(ROWS, ROWS).memory_budget(BUDGET);
    LazyFrame plan = base;
    if (op != "input_only") {
        const auto it = recipes().find(op);
        if (it == recipes().end()) return 2;
        plan = it->second(base).memory_budget(BUDGET);
    }
    const std::int64_t n =
        dftracer::utils::default_runtime().submit(drain_count(plan)).get();
    std::printf("rows=%lld\n", static_cast<long long>(n));
    return 0;
}

std::string g_self;

struct ChildResult {
    int status = -1;
    std::uint64_t peak_bytes = 0;
    double seconds = 0;
    std::string out;
};

ChildResult run_child(const std::string& op) {
    const auto started = std::chrono::steady_clock::now();
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::dup2(fds[1], 1);
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl(g_self.c_str(), g_self.c_str(), "--child", op.c_str(),
                static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::close(fds[1]);
    ChildResult r;
    char buf[256];
    ssize_t k;
    while ((k = ::read(fds[0], buf, sizeof buf)) > 0) r.out.append(buf, k);
    ::close(fds[0]);
    int status = 0;
    struct rusage ru{};
    ::wait4(pid, &status, 0, &ru);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              started)
                    .count();
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#if defined(__APPLE__)
    r.peak_bytes = static_cast<std::uint64_t>(ru.ru_maxrss);
#else
    r.peak_bytes = static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;
#endif
    return r;
}

}  // namespace

TEST_CASE("every lazy op is in the expectation table") {
    const auto ops = registry_ops();
    for (const auto& name : ops)
        CHECK_MESSAGE(expectations().count(name) == 1,
                      "lazy op not classified in test_op_peak_memory: ", name);
    for (const auto& [name, e] : expectations())
        CHECK_MESSAGE(ops.count(name) == 1,
                      "classified op is not in the registry: ", name);
    for (const auto& [name, r] : recipes())
        CHECK_MESSAGE(expectations().count(name) == 1,
                      "recipe without an expectation: ", name);
}

TEST_CASE("each op stays within its bound at a small budget") {
    const bool report = std::getenv("DFTU_OP_PEAK_REPORT") != nullptr;
    const ChildResult base = run_child("input_only");
    REQUIRE(base.status == 0);
    const std::uint64_t bound = BOUND_BUDGETS * BUDGET;
    if (report)
        std::printf("baseline %llu MiB, budget %llu MiB, bound %llu MiB\n",
                    static_cast<unsigned long long>(base.peak_bytes >> 20),
                    static_cast<unsigned long long>(BUDGET >> 20),
                    static_cast<unsigned long long>(bound >> 20));
    for (const auto& [name, recipe] : recipes()) {
        const Expect& want = expectations().at(name);
        const ChildResult r = run_child(name);
        REQUIRE_MESSAGE(r.status == 0, name, " exited with ", r.status);
        const std::uint64_t extra =
            r.peak_bytes > base.peak_bytes ? r.peak_bytes - base.peak_bytes : 0;
        if (report) {
            std::printf("%-18s %6llu MiB %6.2f s  %s %s\n", name.c_str(),
                        static_cast<unsigned long long>(extra >> 20), r.seconds,
                        want.kind == Kind::DEBT        ? "DEBT"
                        : want.kind == Kind::SPILLS    ? "SPILLS"
                        : want.kind == Kind::STREAMING ? "STREAMING"
                                                       : "NOT_MEASURED",
                        want.note);
            continue;
        }
        if (want.kind == Kind::DEBT) {
            CHECK_MESSAGE(extra > bound, name,
                          " now meets its bound; remove it from the debt "
                          "table (",
                          extra >> 20, " MiB, bound ", bound >> 20, " MiB)");
        } else {
            CHECK_MESSAGE(
                extra <= bound * want.stages, name, " holds ", extra >> 20,
                " MiB above the streaming baseline; bound is ", bound >> 20,
                " MiB at a ", BUDGET >> 20, " MiB budget");
        }
    }
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--child")
        return child_main(argv[2]);
    char* resolved = ::realpath(argv[0], nullptr);
    g_self = resolved ? resolved : argv[0];
    std::free(resolved);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
