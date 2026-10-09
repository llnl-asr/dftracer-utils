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
#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/op.h>
#include <doctest/doctest.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// (pseudo-random), s (a short string, or one of about `pad` more bytes, unique
// per row), g (64 distinct values).
class GenCursor : public Cursor {
   public:
    GenCursor(std::int64_t rows, std::int64_t keys, std::uint64_t seed,
              std::size_t pad)
        : rows_(rows), keys_(keys), seed_(seed), pad_(pad) {}

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
            s[i] = pad_ ? std::string(pad_, 'x') + std::to_string(h)
                        : "s" + std::to_string(h % 977);
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
    std::size_t pad_;
    std::int64_t at_ = 0;
};

class GenSource : public Source {
   public:
    GenSource(std::int64_t rows, std::int64_t keys, std::uint64_t seed,
              std::size_t pad)
        : rows_(rows), keys_(keys), seed_(seed), pad_(pad) {}

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
        r.cursor = std::make_unique<GenCursor>(rows_, keys_, seed_, pad_);
        r.filters.assign(req.filters.size(), Pushed::No);
        return r;
    }

   private:
    std::int64_t rows_;
    std::int64_t keys_;
    std::uint64_t seed_;
    std::size_t pad_;
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

LazyFrame gen(std::int64_t rows, std::int64_t keys, std::uint64_t seed = 1,
              std::size_t pad = 0) {
    return LazyFrame::scan(std::make_shared<GenSource>(rows, keys, seed, pad));
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
        // k rows far past the budget: cut from the spilling sort.
        {"topk_wide", [](const LazyFrame& b) { return b.topk("v", ROWS / 2); }},
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
        // unique_by is unique(subset) in the lazy ABI, the same plan op.
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
        // Dyn columns over nearly unique keys: a spilled part merges alone.
        {"group_by_dyn",
         [](const LazyFrame& b) {
             return b.rename_columns({"t", "v"}, {"arg.t", "arg.v"})
                 .group_by(std::vector<std::string>{"k"},
                           {GroupAgg{Agg::Count, "", "n"}},
                           {AggDynSpec{AggOp::Sum, 0.0, "sum_"}}, "arg.");
         }},
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
        // Strings of about 200 bytes, unique per row: 50 MB of keys.
        {"sort_by_long_strings",
         [](const LazyFrame&) {
             return gen(ROWS / 4, ROWS, 1, 200).sort_by("s");
         }},
        // One key on both sides: the matches of a single key outgrow the
        // budget, so the side that is built cannot be split by key.
        {"join_skewed",
         [](const LazyFrame&) {
             return gen(4, 1, 5).join(gen(ROWS / 2, 1, 2), {"k"}, {"k"},
                                      JoinHow::Inner);
         }},
        // One key on both sides of a lookup, whose right rows all agree: the
        // right side is read in blocks and only its first row is kept.
        {"join_lookup_skewed",
         [](const LazyFrame&) {
             return gen(4, 1, 5).join(
                 gen(ROWS, 1, 2)
                     .select({"k"})
                     .with_column("c", lit(std::int64_t{7})),
                 {"k"}, {"k"}, JoinHow::Lookup);
         }},
        // Sixteen dyn names over nearly unique keys.
        {"group_by_dyn_wide",
         [](const LazyFrame& b) {
             LazyFrame w = b.select({"k", "v"});
             for (int i = 0; i < 16; ++i)
                 w = w.with_column("arg.c" + std::to_string(i),
                                   col(1) + lit(std::int64_t{i}));
             return w.group_by(std::vector<std::string>{"k"},
                               {GroupAgg{Agg::Count, "", "n"}},
                               {AggDynSpec{AggOp::Sum, 0.0, "sum_"}}, "arg.");
         }},
        // A registry column op that needs the whole column: its input past a
        // quarter of the budget is mapped from a spill file.
        {"column_op_whole",
         [](const LazyFrame& b) {
             dftu_scalar zero{};
             zero.kind = DFTU_SCALAR_TAG_I64;
             OpArgs args;
             args.str(1, "v")
                 .str(2, "dftu.series.cumsum")
                 .str(3, "")
                 .scalar(4, zero)
                 .scalar(5, zero)
                 .str(6, "");
             const LazyFrame in = b.select({"k", "t", "v", "g"});
             return in.frame_op("dftu.frame.column_op", args, {}, in.schema());
         }},
        // A positional column op: each morsel carries the last rows of the
        // one before it.
        {"column_op_shift",
         [](const LazyFrame& b) {
             dftu_scalar three{};
             three.kind = DFTU_SCALAR_TAG_I64;
             three.value.i = 3;
             dftu_scalar zero{};
             zero.kind = DFTU_SCALAR_TAG_I64;
             OpArgs args;
             args.str(1, "v")
                 .str(2, "dftu.series.shift")
                 .str(3, "")
                 .scalar(4, three)
                 .scalar(5, zero)
                 .str(6, "");
             const LazyFrame in = b.select({"k", "t", "v", "g"});
             return in.frame_op("dftu.frame.column_op", args, {}, in.schema());
         }},
        // The frame op with one direction per key runs as the spilling sort.
        {"sort_by_multi_per_col",
         [](const LazyFrame& b) {
             const char* by[] = {"k", "v"};
             const std::int32_t descending[] = {1, 0};
             OpArgs args;
             args.strlist(1, by, 2).i32list(2, descending);
             return b.frame_op("dftu.frame.sort_by_multi_per_col", args);
         }},
        // One partition far past the budget, with the functions that used to
        // hold a whole partition: variance, a frame from the partition start,
        // first and last value.
        {"window_whole",
         [](const LazyFrame& b) {
             dftu_window_spec var{};
             var.func = DFTU_WINDOW_FRAME_VAR;
             var.value = "v";
             var.out = "var";
             var.param.frame = {0, 2, 1, DFTU_WINDOW_FRAME_ROWS, 0.5, nullptr};
             dftu_window_spec run{};
             run.func = DFTU_WINDOW_FRAME_SUM;
             run.value = "v";
             run.out = "run";
             run.param.frame = {0,   DFTU_WINDOW_UNBOUNDED,
                                1,   DFTU_WINDOW_FRAME_ROWS,
                                0.5, nullptr};
             dftu_window_spec first{};
             first.func = DFTU_WINDOW_FIRST_VALUE;
             first.value = "s";
             first.out = "first";
             dftu_window_spec last{};
             last.func = DFTU_WINDOW_LAST_VALUE;
             last.value = "v";
             last.out = "last";
             const std::vector<dftu_window_spec> specs{var, run, first, last};
             const char* order[] = {"t"};
             OpArgs args;
             args.strlist(1, nullptr, 0).strlist(2, order, 1).winlist(3, specs);
             std::vector<std::string> names = b.schema();
             for (const dftu_window_spec& w : specs) names.emplace_back(w.out);
             return b.frame_op("dftu.frame.window", args, {}, names);
         }},
        // One partition far past the budget, with the frames that read to its
        // end or over all of it, and a text minimum from its start.
        {"window_end",
         [](const LazyFrame& b) {
             const auto frame = [](dftu_window_func func, const char* value,
                                   const char* out, std::int64_t pre,
                                   std::int64_t fol) {
                 dftu_window_spec w{};
                 w.func = func;
                 w.value = value;
                 w.out = out;
                 w.param.frame = {0,   pre,    fol, DFTU_WINDOW_FRAME_ROWS,
                                  0.5, nullptr};
                 return w;
             };
             const std::int64_t ALL = DFTU_WINDOW_UNBOUNDED;
             const std::vector<dftu_window_spec> specs{
                 frame(DFTU_WINDOW_FRAME_VAR, "v", "var", ALL, ALL),
                 frame(DFTU_WINDOW_FRAME_SUM, "v", "rest", 3, ALL),
                 frame(DFTU_WINDOW_FRAME_COUNT, "v", "left", 0, ALL),
                 frame(DFTU_WINDOW_FRAME_MIN, "s", "lo", ALL, 0)};
             const char* order[] = {"t"};
             OpArgs args;
             args.strlist(1, nullptr, 0).strlist(2, order, 1).winlist(3, specs);
             std::vector<std::string> names = b.schema();
             for (const dftu_window_spec& w : specs) names.emplace_back(w.out);
             return b.frame_op("dftu.frame.window", args, {}, names);
         }},
        // One partition far past the budget, with the frames that read to its
        // end and need the rows ahead: a min and a max (text too) from each
        // row on, a variance, a range frame whose float mean follows the
        // kernel's steps, and a text maximum that reads rows ahead.
        {"window_suffix",
         [](const LazyFrame& b) {
             const auto frame = [](dftu_window_func func, const char* value,
                                   const char* out, std::int64_t pre,
                                   std::int64_t fol, bool range = false) {
                 dftu_window_spec w{};
                 w.func = func;
                 w.value = value;
                 w.out = out;
                 w.param.frame = {
                     0,
                     pre,
                     fol,
                     range ? DFTU_WINDOW_FRAME_RANGE : DFTU_WINDOW_FRAME_ROWS,
                     0.5,
                     nullptr};
                 return w;
             };
             const std::int64_t ALL = DFTU_WINDOW_UNBOUNDED;
             const std::vector<dftu_window_spec> specs{
                 frame(DFTU_WINDOW_FRAME_MIN, "v", "lo", 3, ALL),
                 frame(DFTU_WINDOW_FRAME_MAX, "s", "hi", 2, ALL),
                 frame(DFTU_WINDOW_FRAME_VAR, "v", "var", 1, ALL),
                 frame(DFTU_WINDOW_FRAME_COUNT, "v", "near", 50, ALL, true),
                 frame(DFTU_WINDOW_FRAME_MEAN, "v", "avg", 10, ALL, true),
                 frame(DFTU_WINDOW_FRAME_MAX, "s", "ahead", ALL, 2)};
             const char* order[] = {"t"};
             OpArgs args;
             args.strlist(1, nullptr, 0).strlist(2, order, 1).winlist(3, specs);
             std::vector<std::string> names = b.schema();
             for (const dftu_window_spec& w : specs) names.emplace_back(w.out);
             return b.frame_op("dftu.frame.window", args, {}, names);
         }},
        {"compare_agg",
         [](const LazyFrame& b) {
             const std::vector<GroupAgg> aggs{GroupAgg{Agg::Sum, "v", "v"}};
             const std::vector<std::string> by{"k"};
             return b.group_by(by, aggs).compare_agg(
                 gen(ROWS, ROWS, 2).group_by(by, aggs), 1);
         }},
        // 125000 right rows over 64 groups against 1M left rows: both sides
        // are sorted and merged, and the output is one row per left row.
        {"asof",
         [](const LazyFrame& b) {
             return b.asof(gen(ROWS / 4, ROWS, 2), "t", {"g"},
                           AsofDirection::Nearest);
         }},
        // Each right range holds 41 points of t, and the groups cut the
        // matches to about one per left row.
        {"interval",
         [](const LazyFrame& b) {
             const LazyFrame ranges =
                 gen(ROWS / 4, ROWS, 2)
                     .with_column("hi", col(1) + lit(std::int64_t{40}));
             return b.interval(ranges, "t", "t", "hi", {"g"}, true);
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
        // 100 left rows meet 100000 right rows: a right side over the budget
        // and ten million output rows.
        {"cross_join",
         [](const LazyFrame& b) {
             return b.head(100).join(gen(ROWS / 10, ROWS, 3), {}, {},
                                     JoinHow::Cross);
         }},
        {"group_by_dynamic",
         [](const LazyFrame& b) {
             return b.group_by_dynamic("t", 1000, 1000,
                                       {GroupAgg{Agg::Sum, "v", "v_sum"}});
         }},
        // Sliding windows of two rows every two rows: half a million windows.
        {"group_by_dynamic_many",
         [](const LazyFrame& b) {
             return b.group_by_dynamic("t", 2, 4,
                                       {GroupAgg{Agg::Sum, "v", "v_sum"},
                                        GroupAgg{Agg::Count, "", "n"}});
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
        {"topk_wide", {Kind::SPILLS, "k past the budget"}},
        {"join", {Kind::SPILLS, ""}},
        {"cross_join", {Kind::SPILLS, "the right side is spooled"}},
        // Spill triggers exist, but memory stays far above the budget.
        {"sort_by", {Kind::SPILLS, ""}},
        {"sort_by_multi", {Kind::SPILLS, ""}},
        {"unique", {Kind::SPILLS, ""}},
        {"drop_duplicates", {Kind::SPILLS, ""}},
        {"unique_by", {Kind::SPILLS, ""}},
        {"is_duplicated", {Kind::SPILLS, ""}},
        {"is_unique", {Kind::SPILLS, ""}},
        {"group_by", {Kind::SPILLS, ""}},
        {"group_by_dyn", {Kind::SPILLS, ""}},
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
        {"group_by_dynamic", {Kind::STREAMING, "the open windows"}},
        {"group_by_dynamic_many", {Kind::STREAMING, "the open windows"}},
        {"sort_by_long_strings", {Kind::SPILLS, ""}},
        {"join_skewed", {Kind::SPILLS, ""}},
        {"join_lookup_skewed", {Kind::SPILLS, ""}},
        {"group_by_dyn_wide", {Kind::SPILLS, ""}},
        {"compare_agg", {Kind::SPILLS, "two group-bys, a join and a sort", 2}},
        {"group_transform", {Kind::SPILLS, "two sorts and a window chunk", 2}},
        {"window_whole", {Kind::SPILLS, "a sort, a spool and a window chunk"}},
        {"window_end", {Kind::SPILLS, "a sort, two spool passes and a chunk"}},
        {"window_suffix",
         {Kind::SPILLS, "a sort, a spool read both ways and a chunk"}},
        {"asof", {Kind::SPILLS, "two sorts and a merge window"}},
        {"interval", {Kind::SPILLS, "two sorts and the open ranges"}},
        // cumsum over an integer column runs one morsel at a time with the
        // running total carried over; no input or result is held whole.
        {"column_op_whole", {Kind::STREAMING, ""}},
        {"column_op_shift", {Kind::STREAMING, ""}},
        {"sort_by_multi_per_col", {Kind::SPILLS, "the spilling sort"}},
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

// A child that takes longer than this is killed and fails the test.
constexpr std::chrono::seconds CHILD_TIMEOUT{300};

// Peak RSS under a sanitizer or Valgrind is the tool's shadow memory, not the
// engine's, so the bounds mean nothing there.
bool under_instrumentation() {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || \
    __has_feature(memory_sanitizer)
    return true;
#endif
#endif
    const char* preload = std::getenv("LD_PRELOAD");
    return preload && std::strstr(preload, "vgpreload") != nullptr;
}

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
    bool timed_out = false;
    for (;;) {
        const auto left =
            CHILD_TIMEOUT - (std::chrono::steady_clock::now() - started);
        if (left <= std::chrono::steady_clock::duration::zero()) {
            timed_out = true;
            break;
        }
        struct pollfd pfd{fds[0], POLLIN, 0};
        const int ready = ::poll(
            &pfd, 1,
            static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(left)
                    .count()));
        if (ready == 0) {
            timed_out = true;
            break;
        }
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        const ssize_t k = ::read(fds[0], buf, sizeof buf);
        if (k <= 0) break;
        r.out.append(buf, static_cast<std::size_t>(k));
    }
    if (timed_out) ::kill(pid, SIGKILL);
    ::close(fds[0]);
    int status = 0;
    struct rusage ru{};
    ::wait4(pid, &status, 0, &ru);
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              started)
                    .count();
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (timed_out) r.status = -2;
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
    // A recipe that runs a registry op another way, to measure that way too.
    const std::map<std::string, std::string> variants = {
        {"cross_join", "join"},
        {"topk_wide", "topk"},
        {"group_by_dyn", "group_by"},
        {"column_op_whole", "with_column"},
        {"column_op_shift", "with_column"},
        {"sort_by_multi_per_col", "sort_by_multi"},
        {"window_whole", "sort_by"},
        {"window_end", "sort_by"},
        {"window_suffix", "sort_by"},
        {"group_by_dynamic_many", "group_by_dynamic"},
        {"sort_by_long_strings", "sort_by"},
        {"join_skewed", "join"},
        {"join_lookup_skewed", "join"},
        {"group_by_dyn_wide", "group_by"}};
    for (const auto& [name, e] : expectations()) {
        const auto v = variants.find(name);
        const bool known = ops.count(name) == 1 ||
                           (v != variants.end() && ops.count(v->second) == 1);
        CHECK_MESSAGE(known, "classified op is not in the registry: ", name);
    }
    for (const auto& [name, r] : recipes())
        CHECK_MESSAGE(expectations().count(name) == 1,
                      "recipe without an expectation: ", name);
}

TEST_CASE("each op stays within its bound at a small budget" *
          doctest::skip(under_instrumentation())) {
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
        REQUIRE_MESSAGE(r.status != -2, name, " ran past ",
                        CHILD_TIMEOUT.count(), " s");
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
#if defined(DFTRACER_UTILS_COVERAGE_BUILD)
        // The coverage build is Debug and counts every branch, so its peaks
        // run above a bound set for an optimized build; the child must still
        // finish.
        (void)extra;
#else
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
#endif
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
