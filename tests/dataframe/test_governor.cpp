#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/internal/cell_ops.h>
#include <dftracer/utils/dataframe/internal/governor.h>
#include <dftracer/utils/dataframe/internal/spill.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <dftracer/utils/memory/heap.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dftracer::utils::dataframe;
using dftracer::utils::coro::CoroTask;
namespace memory = dftracer::utils::memory;
namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t MIB = std::uint64_t{1} << 20;
// No fraction of this budget is reached by the data below, so every spill in
// these tests comes from the governor.
constexpr std::uint64_t LAZY_BUDGET = std::uint64_t{1} << 30;

struct Budget {
    explicit Budget(std::size_t bytes) { set(bytes); }
    ~Budget() { memory::configure(memory::Config{}); }
    static void set(std::size_t bytes) {
        memory::Config c;
        c.budget = bytes;
        memory::configure(c);
    }
};

// Distinct keys: ten rows per key in a full frame. A group-by or a set of the
// keys needs state that grows with this.
constexpr std::int64_t KEYS = 6553;

std::int64_t key_of(std::int64_t i) { return (i * 7919) % KEYS; }

// `morsels` morsels of `rows` rows: k, a scrambled key, and v = the row
// number. `hook` runs before morsel i is made.
class Hooked : public Source {
   public:
    using Hook = std::function<void(std::int64_t)>;

    // An untyped source declares its columns with no type, as a plan whose
    // types are only known once it runs.
    Hooked(std::int64_t morsels, std::int64_t rows, Hook hook,
           bool typed = true)
        : morsels_(morsels),
          rows_(rows),
          hook_(std::move(hook)),
          typed_(typed) {}

    Schema schema() const override {
        const std::int64_t zero = 0;
        const DataType i =
            typed_ ? Series::flat_i64(&zero, 1).data_type() : DataType{};
        Schema out;
        out.fields = {Field{"k", i, true}, Field{"v", i, true},
                      Field{"p", i, true}};
        return out;
    }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        r.cursor = std::make_unique<Rows>(morsels_, rows_, hook_);
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

    DataFrame whole() const {
        DataFrame f;
        f.names = {"k", "v", "p"};
        std::vector<std::int64_t> k, v, p;
        for (std::int64_t i = 0; i < morsels_ * rows_; ++i) {
            k.push_back(key_of(i));
            v.push_back(i);
            p.push_back(i % 4);
        }
        const auto n = static_cast<std::int64_t>(k.size());
        f.columns.push_back(Series::flat_i64(k.data(), n));
        f.columns.push_back(Series::flat_i64(v.data(), n));
        f.columns.push_back(Series::flat_i64(p.data(), n));
        return f;
    }

   private:
    class Rows : public Cursor {
       public:
        Rows(std::int64_t morsels, std::int64_t rows, Hook hook)
            : morsels_(morsels), rows_(rows), hook_(std::move(hook)) {}
        CoroTask<std::optional<Morsel>> next(std::int64_t) override {
            if (at_ >= morsels_) co_return std::nullopt;
            if (hook_) hook_(at_);
            std::vector<std::int64_t> k(static_cast<std::size_t>(rows_)),
                v(static_cast<std::size_t>(rows_)),
                p(static_cast<std::size_t>(rows_));
            for (std::int64_t i = 0; i < rows_; ++i) {
                const std::int64_t row = at_ * rows_ + i;
                k[static_cast<std::size_t>(i)] = key_of(row);
                v[static_cast<std::size_t>(i)] = row;
                p[static_cast<std::size_t>(i)] = row % 4;
            }
            ++at_;
            Morsel m;
            m.rows = rows_;
            m.columns.push_back(Series::flat_i64(k.data(), rows_));
            m.columns.push_back(Series::flat_i64(v.data(), rows_));
            m.columns.push_back(Series::flat_i64(p.data(), rows_));
            co_return m;
        }

       private:
        std::int64_t morsels_, rows_;
        Hook hook_;
        std::int64_t at_ = 0;
    };

    std::int64_t morsels_, rows_;
    Hook hook_;
    bool typed_;
};

std::vector<std::int64_t> ints(const DataFrame& f, const std::string& name) {
    const auto it = std::find(f.names.begin(), f.names.end(), name);
    REQUIRE(it != f.names.end());
    const Series c =
        f.columns[static_cast<std::size_t>(it - f.names.begin())].materialize();
    const std::int64_t* p = c.data<std::int64_t>();
    return std::vector<std::int64_t>(p, p + c.length());
}

bool ends_with(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

// The open pools whose path ends in `tail`.
std::vector<memory::PoolStats> pools_named(std::string_view tail) {
    std::vector<memory::PoolStats> out;
    for (memory::PoolStats& p : memory::pool_stats())
        if (ends_with(p.path, tail)) out.push_back(std::move(p));
    return out;
}

std::size_t reserved_of(std::uint16_t id) {
    for (const memory::PoolStats& p : memory::pool_stats())
        if (p.id == id) return p.reserved;
    return 0;
}

// Reserves more than the budget leaves from a pool of its own, so the
// manager posts the shortfall to the holders. Returns the status.
memory::Status hog(std::size_t bytes) {
    memory::PoolHandle pool = memory::Manager::get().root().child("hog");
    REQUIRE(pool);
    return pool->reserve(bytes).status();
}

// Every row of `f` as one line of its cells, in order or sorted.
std::vector<std::string> rows_of(const DataFrame& f, bool sorted) {
    std::vector<std::string> out(static_cast<std::size_t>(f.num_rows()));
    for (const Series& c : f.columns)
        for (std::int64_t i = 0; i < f.num_rows(); ++i)
            out[static_cast<std::size_t>(i)] +=
                (c.is_null(i) ? std::string("null") : cell_to_string(c, i)) +
                "|";
    if (sorted) std::sort(out.begin(), out.end());
    return out;
}

constexpr std::int64_t MORSELS = 64;
constexpr std::int64_t ROWS = 1024;
static_assert(MORSELS * ROWS >= 10 * KEYS);
constexpr std::int64_t ASK_AT = 32;
// A global budget the state at ASK_AT is a fair share of, and that the whole
// frame stays under.
constexpr std::uint64_t BUDGET = 4 * MIB;

using Plan = std::function<LazyFrame(LazyFrame)>;

struct Asked {
    DataFrame plain;
    DataFrame asked;
    memory::Status status = memory::Status::Ok;
    std::size_t before = 0;
    std::size_t after = SIZE_MAX;
};

// Runs `plan` over a fresh source twice: once as is, and once with a request
// for half of what the budget holds posted at morsel ASK_AT. `before` and
// `after` are the reservation of the pool ending in `tail` then and its
// smallest one later.
Asked run_asked(const Plan& plan, std::string_view tail, bool typed = true) {
    Asked r;
    r.plain = plan(LazyFrame::scan(
                       std::make_shared<Hooked>(MORSELS, ROWS, nullptr, typed)))
                  .memory_budget(LAZY_BUDGET)
                  .collect()
                  .get();
    std::uint16_t id = 0;
    auto source = std::make_shared<Hooked>(
        MORSELS, ROWS,
        [&](std::int64_t i) {
            if (i == ASK_AT) {
                const std::vector<memory::PoolStats> pools = pools_named(tail);
                REQUIRE(pools.size() == 1);
                id = pools.front().id;
                r.before = pools.front().reserved;
                const std::size_t held =
                    memory::Manager::get().root().reserved();
                r.status = hog(memory::Manager::get().budget() - held / 2);
            }
            if (i > ASK_AT && id != 0)
                r.after = std::min(r.after, reserved_of(id));
        },
        typed);
    r.asked = plan(LazyFrame::scan(source))
                  .memory_budget(LAZY_BUDGET)
                  .collect()
                  .get();
    return r;
}

LazyFrame window_lag(const LazyFrame& plan) {
    const char* part[] = {"p"};
    const char* order[] = {"v"};
    std::vector<dftu_window_spec> specs(1);
    specs[0] = dftu_window_spec{};
    specs[0].func = DFTU_WINDOW_LAG;
    specs[0].value = "k";
    specs[0].out = "lag_k";
    specs[0].param.offset = 1;
    OpArgs args;
    args.strlist(1, part, 1).strlist(2, order, 1).winlist(3, specs);
    std::vector<std::string> names = plan.schema();
    names.emplace_back("lag_k");
    return plan.frame_op("dftu.frame.window", args, {}, std::move(names));
}

}  // namespace

TEST_CASE("a request posted to a sort makes it spill at the next morsel") {
    Budget budget(BUDGET);
    std::uint16_t sort_pool = 0;
    std::size_t before = 0;
    std::size_t after = SIZE_MAX;
    memory::Status asked = memory::Status::Ok;
    auto source = std::make_shared<Hooked>(MORSELS, ROWS, [&](std::int64_t i) {
        if (i == ASK_AT) {
            const std::vector<memory::PoolStats> sorts =
                pools_named("query/sort");
            REQUIRE(sorts.size() == 1);
            sort_pool = sorts.front().id;
            before = sorts.front().reserved;
            asked = hog(BUDGET - BUDGET / 16);
        }
        if (i > ASK_AT) after = std::min(after, reserved_of(sort_pool));
    });
    const DataFrame got = LazyFrame::scan(source)
                              .sort_by("k")
                              .memory_budget(LAZY_BUDGET)
                              .collect()
                              .get();
    CHECK(before > 0);
    CHECK(asked == memory::Status::Pending);
    CHECK(after == 0);
    const DataFrame want = source->whole().sort_by("k", false);
    CHECK(ints(got, "k") == ints(want, "k"));
    CHECK(ints(got, "v") == ints(want, "v"));
}

TEST_CASE("a request posted to a group by makes it spill at the next morsel") {
    Budget budget(BUDGET);
    std::uint16_t group_pool = 0;
    std::size_t before = 0;
    std::size_t after = SIZE_MAX;
    memory::Status asked = memory::Status::Ok;
    auto source = std::make_shared<Hooked>(MORSELS, ROWS, [&](std::int64_t i) {
        if (i == ASK_AT) {
            const std::vector<memory::PoolStats> groups =
                pools_named("query/group_by");
            REQUIRE(groups.size() == 1);
            group_pool = groups.front().id;
            before = groups.front().reserved;
            asked = hog(BUDGET - before / 2);
        }
        if (i > ASK_AT) after = std::min(after, reserved_of(group_pool));
    });
    const std::vector<GroupAgg> aggs = {GroupAgg{Agg::Sum, "v", "s"}};
    const DataFrame got = LazyFrame::scan(source)
                              .group_by("k", aggs)
                              .memory_budget(LAZY_BUDGET)
                              .collect()
                              .get()
                              .sort_by("k", false);
    CHECK(before > 0);
    CHECK(asked == memory::Status::Pending);
    CHECK(after == 0);
    const DataFrame want =
        source->whole().group_by("k", aggs).sort_by("k", false);
    CHECK(ints(got, "k") == ints(want, "k"));
    CHECK(ints(got, "s") == ints(want, "s"));
}

TEST_CASE(
    "a second query's reservation over the budget makes the first spill") {
    Budget budget(8 * BUDGET);
    constexpr std::int64_t SECOND_ROWS = MORSELS * ROWS;
    std::uint16_t first_pool = 0;
    std::size_t before = 0;
    std::size_t after = SIZE_MAX;
    DataFrame second_got;
    std::shared_ptr<Hooked> second_source;
    auto source = std::make_shared<Hooked>(MORSELS, ROWS, [&](std::int64_t i) {
        if (i == ASK_AT) {
            const std::vector<memory::PoolStats> sorts =
                pools_named("query/sort");
            REQUIRE(sorts.size() == 1);
            first_pool = sorts.front().id;
            before = sorts.front().reserved;
            // Room for less than a quarter of the second query's first
            // morsel: its own reservation cannot cover the shortfall, so
            // the rest is posted to the first query.
            Budget::set(memory::Manager::get().root().reserved() + BUDGET / 32);
            second_source = std::make_shared<Hooked>(1, SECOND_ROWS, nullptr);
            second_got = LazyFrame::scan(second_source)
                             .sort_by("k")
                             .memory_budget(LAZY_BUDGET)
                             .collect()
                             .get();
        }
        if (i > ASK_AT) after = std::min(after, reserved_of(first_pool));
    });
    const DataFrame got = LazyFrame::scan(source)
                              .sort_by("k")
                              .memory_budget(LAZY_BUDGET)
                              .collect()
                              .get();
    CHECK(before > 0);
    CHECK(after == 0);
    const DataFrame want = source->whole().sort_by("k", false);
    CHECK(ints(got, "k") == ints(want, "k"));
    CHECK(ints(got, "v") == ints(want, "v"));
    REQUIRE(second_source);
    const DataFrame second_want = second_source->whole().sort_by("k", false);
    CHECK(ints(second_got, "k") == ints(second_want, "k"));
    CHECK(ints(second_got, "v") == ints(second_want, "v"));
}

TEST_CASE("a run's pools close and its live bytes return to zero") {
    memory::Pool& root = memory::Manager::get().root();
    const std::size_t adopted_before = root.adopted();
    std::size_t run_live = 0;
    std::size_t scratch = 0;
    auto source = std::make_shared<Hooked>(16, 4096, [&](std::int64_t i) {
        if (i != 8) return;
        const std::vector<memory::PoolStats> runs = pools_named("root/query");
        REQUIRE(runs.size() == 1);
        run_live = runs.front().live;
        scratch = pools_named("root/query/sort").size();
    });
    {
        const DataFrame got = LazyFrame::scan(source)
                                  .sort_by("k")
                                  .memory_budget(LAZY_BUDGET)
                                  .collect()
                                  .get();
        CHECK(got.num_rows() == 16 * 4096);
        CHECK(pools_named("root/query").empty());
    }
    CHECK(run_live > 0);
    CHECK(scratch == 1);
    CHECK(root.adopted() == adopted_before);
    CHECK_FALSE(memory::Manager::get().leaked());
}

TEST_CASE(
    "a request posted to the other stateful cursors keeps their results") {
    Budget budget(BUDGET);

    SUBCASE("topk hands its best rows to a sort") {
        const Asked r = run_asked(
            [](LazyFrame lf) { return lf.topk("k", KEYS / 2); }, "query/topk");
        CHECK(r.before > 0);
        CHECK(r.status == memory::Status::Pending);
        CHECK(r.after == 0);
        CHECK(rows_of(r.asked, true) == rows_of(r.plain, true));
    }
    SUBCASE("is_duplicated") {
        const Asked r = run_asked(
            [](LazyFrame lf) { return lf.select({"k"}).is_duplicated(); },
            "query/is_duplicated");
        CHECK(r.before > 0);
        CHECK(r.status == memory::Status::Pending);
        CHECK(r.after == 0);
        CHECK(rows_of(r.asked, false) == rows_of(r.plain, false));
    }
    SUBCASE("pivot") {
        const Asked r = run_asked(
            [](LazyFrame lf) { return lf.pivot("k", "p", "v", "sum"); },
            "query/pivot");
        CHECK(r.before > 0);
        CHECK(r.status == memory::Status::Pending);
        CHECK(r.after == 0);
        CHECK(r.asked.names == r.plain.names);
        CHECK(rows_of(r.asked, false) == rows_of(r.plain, false));
    }
    SUBCASE("compare_agg") {
        const Asked r = run_asked(
            [](LazyFrame lf) {
                // The request lands on the left side's state, so a small
                // right side keeps the check and the run short.
                return lf.compare_agg(LazyFrame::scan(std::make_shared<Hooked>(
                                          4, ROWS, nullptr, false)),
                                      1);
            },
            "query/compare_agg", false);
        CHECK(r.before > 0);
        CHECK(r.status == memory::Status::Pending);
        CHECK(r.after == 0);
        CHECK(rows_of(r.asked, true) == rows_of(r.plain, true));
    }
    SUBCASE("window keeps its result; its chunk cannot leave memory") {
        const Asked r = run_asked(window_lag, "query/window");
        CHECK(r.status == memory::Status::Pending);
        CHECK(rows_of(r.asked, false) == rows_of(r.plain, false));
    }
}

TEST_CASE("a request posted to a join's build side turns it to grace") {
    Budget budget(BUDGET);
    DataFrame left;
    left.names = {"k", "w"};
    std::vector<std::int64_t> k, w;
    for (std::int64_t i = 0; i < KEYS / 5; ++i) {
        k.push_back(i * 5);
        w.push_back(i);
    }
    const auto n = static_cast<std::int64_t>(k.size());
    left.columns.push_back(Series::flat_i64(k.data(), n));
    left.columns.push_back(Series::flat_i64(w.data(), n));
    const Asked r = run_asked(
        [&](LazyFrame right) { return left.lazy().join(right, {"k"}); },
        "query/join");
    CHECK(r.before > 0);
    CHECK(r.status == memory::Status::Pending);
    CHECK(r.after == 0);
    CHECK(r.plain.num_rows() > 0);
    CHECK(rows_of(r.asked, true) == rows_of(r.plain, true));
}

TEST_CASE("unique and head_by keep their results when a grow is refused") {
    // The external path opens a spill directory of its own while it runs;
    // the in-memory path opens none.
    const fs::path dir = fs::temp_directory_path() /
                         ("dftu_governor_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    ::setenv("DFTRACER_UTILS_SPILL_DIR", dir.c_str(), 1);
    auto entries = [&] {
        std::size_t n = 0;
        for (const auto& e : fs::directory_iterator(dir)) {
            (void)e;
            ++n;
        }
        return n;
    };
    auto run = [&](const Plan& plan, std::size_t& seen) {
        auto source =
            std::make_shared<Hooked>(MORSELS, ROWS, [&](std::int64_t i) {
                if (i == MORSELS - 1) seen = entries();
            });
        return plan(LazyFrame::scan(source))
            .memory_budget(LAZY_BUDGET)
            .collect()
            .get();
    };
    for (const Plan& plan :
         {Plan([](LazyFrame lf) { return lf.unique({"k"}); }),
          Plan([](LazyFrame lf) { return lf.head_by({"k"}, 2); })}) {
        DataFrame plain;
        std::size_t plain_dirs = 0;
        {
            Budget roomy(8 * BUDGET);
            plain = run(plan, plain_dirs);
        }
        // Below the key set: its grow is refused.
        std::size_t tight_dirs = 0;
        Budget tight(BUDGET / 64);
        const DataFrame got = run(plan, tight_dirs);
        CHECK(plain.num_rows() > 0);
        CHECK(plain_dirs == 0);
        CHECK(tight_dirs > 0);
        CHECK(rows_of(got, false) == rows_of(plain, false));
    }
    ::unsetenv("DFTRACER_UTILS_SPILL_DIR");
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("a spool spilled on request replays every morsel in order") {
    spill::Spool spool(LAZY_BUDGET);
    std::uint64_t bytes = 0;
    auto add = [&](std::int64_t first) {
        std::vector<std::int64_t> v{first, first + 1};
        std::vector<Series> cols;
        cols.push_back(Series::flat_i64(v.data(), 2));
        bytes += spill::columns_bytes(cols);
        spool.add(std::move(cols), 2);
    };
    add(0);
    add(2);
    add(4);
    CHECK(spool.memory_bytes() == bytes);
    CHECK(spool.spill() == bytes);
    CHECK(spool.memory_bytes() == 0);
    CHECK(spool.spilled());
    add(6);
    add(8);
    CHECK(spool.spill() == 0);

    auto values = [](std::unique_ptr<Cursor> c) {
        std::vector<std::int64_t> out;
        for (;;) {
            auto m = c->next(16).get();
            if (!m) break;
            const std::int64_t* p = m->columns[0].data<std::int64_t>();
            out.insert(out.end(), p, p + m->rows);
        }
        return out;
    };
    CHECK(values(spool.reader()) ==
          std::vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
    CHECK(values(spool.reverse_reader()) ==
          std::vector<std::int64_t>{8, 9, 6, 7, 4, 5, 2, 3, 0, 1});
}

namespace {

struct Held {
    std::size_t live = 0;
    std::size_t reserved = 0;
};

// The largest live and reserved bytes of the pool ending in `tail` after the
// first output morsel, streaming the plan to its end.
CoroTask<Held> held_while_streaming(LazyFrame lf, std::string tail) {
    Held held;
    auto g = lf.stream(4096);
    bool first = true;
    while (auto df = co_await g.next()) {
        if (!first) continue;
        first = false;
        for (const memory::PoolStats& p : pools_named(tail)) {
            held.live = std::max(held.live, p.live);
            held.reserved = std::max(held.reserved, p.reserved);
        }
    }
    co_return held;
}

}  // namespace

TEST_CASE("unique and is_duplicated keep their tables in their own pool") {
    for (const auto& [plan, tail] :
         {std::pair<Plan, std::string>{
              [](LazyFrame lf) { return lf.unique({"k"}); }, "query/unique"},
          std::pair<Plan, std::string>{
              [](LazyFrame lf) { return lf.select({"k"}).is_duplicated(); },
              "query/is_duplicated"}}) {
        CAPTURE(tail);
        const LazyFrame lf = plan(LazyFrame::scan(std::make_shared<Hooked>(
                                      MORSELS, ROWS, nullptr)))
                                 .memory_budget(LAZY_BUDGET);
        CHECK(held_while_streaming(lf, tail).get().live > 0);
        CHECK(pools_named(tail).empty());
        CHECK_FALSE(memory::Manager::get().leaked());
    }
}

TEST_CASE("is_duplicated reserves its counts on its pool while it emits") {
    std::set<std::int64_t> keys;
    for (std::int64_t i = 0; i < MORSELS * ROWS; ++i) keys.insert(key_of(i));
    const LazyFrame lf =
        LazyFrame::scan(std::make_shared<Hooked>(MORSELS, ROWS, nullptr))
            .select({"k"})
            .is_duplicated()
            .memory_budget(LAZY_BUDGET);
    // One map entry is at least a key string and its count.
    const std::size_t entries =
        keys.size() * sizeof(std::pair<std::string, std::int64_t>);
    CHECK(held_while_streaming(lf, "query/is_duplicated").get().reserved >=
          entries);
    CHECK(pools_named("query/is_duplicated").empty());
    CHECK_FALSE(memory::Manager::get().leaked());
}

TEST_CASE("a request goes to state that can spill, not to state that cannot") {
    Budget budget(64 * MIB);
    memory::PoolHandle query = memory::Manager::get().root().child("query");
    REQUIRE(query);
    memory::Scope scope{*query};
    Governor keys("keys");
    Governor spool("spool");
    // The key set is larger than the shortfall below and cannot leave memory;
    // the spool can.
    CHECK(keys.check(16 * MIB, 0) == 0);
    CHECK(spool.check(4 * MIB) == 0);
    // A shortfall of 2 MiB, which the key set alone could have covered.
    const std::size_t held = memory::Manager::get().root().reserved();
    REQUIRE(held < memory::Manager::get().budget());
    CHECK(hog(memory::Manager::get().budget() - held + 2 * MIB) ==
          memory::Status::Pending);
    CHECK(spool.check(4 * MIB) > 0);
    CHECK(keys.check(16 * MIB, 0) == 0);
}
