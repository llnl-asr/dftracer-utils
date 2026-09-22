// Dynamic filter pushdown (docs/plans/plugin_runtime.md section 12, gap 10):
// once a scan is already open, nothing could previously narrow what remains.
// dataframe::Cursor::narrow() is the advisory hook; views::detail::DynamicPrune
// is the state ViewSource's streaming cursor writes into so a running fuse()
// scan can skip units it has not claimed yet without the caller re-planning.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/lazyframe.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/query/builder.h>
#include <dftracer/utils/query/query.h>
#include <dftracer/utils/trace/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_source.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "test_view_common.h"

using namespace dftracer::utils::trace::views::detail;
using dftracer::utils::StringIntern;
namespace indexing = dftracer::utils::trace::indexing;
namespace q = dftracer::utils::query;

namespace {

// Collects the interned `name` of every event this fold sees, plus how many
// units it saw sealed (fully scanned end to end), so a test can compare row
// content and scan work between a narrowed and unnarrowed run.
struct NameCollectorFold : Fold {
    std::vector<std::uint32_t> name_ids;
    std::uint64_t sealed_units = 0;

    bool accepts(const ScanShape&) const override { return true; }
    std::unique_ptr<Fold> slice() const override {
        return std::make_unique<NameCollectorFold>();
    }
    void step(const FoldBatch& b) override {
        for (const auto& e : b.events) name_ids.push_back(e.name_id);
    }
    void seal_unit(const ScanUnit&) override { ++sealed_units; }
    void drop_unit(const ScanUnit&) override {}
    void merge(Fold& other) override {
        auto& o = static_cast<NameCollectorFold&>(other);
        name_ids.insert(name_ids.end(), o.name_ids.begin(), o.name_ids.end());
        sealed_units += o.sealed_units;
    }
    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }
};

std::vector<std::string> matching_names(const NameCollectorFold& f,
                                        StringIntern& intern,
                                        std::string_view want) {
    std::vector<std::string> out;
    for (std::uint32_t id : f.name_ids) {
        std::string n(intern.resolve(id));
        if (n == want) out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Three named groups (pid 1/2/3, name alpha/beta/gamma in lockstep), each far
// bigger than the checkpoint size the test builds the index with, so only the
// (few) checkpoints straddling a group boundary can ever mix values - every
// other checkpoint is single-valued and a bloom/dictionary prune on it is
// exact. `pid` (numeric) is what narrow()'s Expr predicate targets - the
// existing col-vs-scalar translator (translate_leaf) only handles numeric
// comparisons, so a string-equality predicate is not representable via
// dataframe::Expr today; `name` stays only for the fold-level test, which
// builds its query::Query directly and so is not subject to that limit.
// `per_name` events for each of three names, one pid per name. The tests that
// narrow a scan already in flight pass a larger count: the prune can only skip
// what the producer has not claimed yet, and the producer runs ahead by a
// roughly fixed number of rows, so at 40 it pruned 8 rows of 158 and a fast
// machine lost the race outright. At 200 it prunes about half.
std::string create_grouped_trace(TestEnvironment& env, int per_name = 40) {
    std::string pfw = env.get_dir() + "/grouped.pfw";
    std::ofstream ofs(pfw);
    std::uint64_t ts = 1000;
    int pid = 1;
    for (const char* name : {"alpha", "beta", "gamma"}) {
        for (int i = 0; i < per_name; ++i) {
            ofs << R"({"ph":"X","name":")" << name
                << R"(","cat":"POSIX","pid":)" << pid << R"(,"tid":1,"ts":)"
                << ts << R"(,"dur":)" << (10 + i) << "}\n";
            ts += 100;
        }
        ++pid;
    }
    ofs.close();
    std::string gz = pfw + ".gz";
    dftu_utils_test::compress_file_to_gzip_multimember(pfw, gz, 200);
    fs::remove(pfw);
    return gz;
}

// Wraps a Source so a test can count the rows its cursor actually produced
// and see whether a narrow() reached it; everything else passes through.
struct CountingCursor : dataframe::Cursor {
    std::unique_ptr<dataframe::Cursor> inner;
    std::size_t* rows;
    std::size_t* narrows;
    CountingCursor(std::unique_ptr<dataframe::Cursor> in, std::size_t* r,
                   std::size_t* n)
        : inner(std::move(in)), rows(r), narrows(n) {}
    coro::CoroTask<std::optional<dataframe::Morsel>> next(
        std::int64_t max_rows) override {
        auto m = co_await inner->next(max_rows);
        if (m) *rows += static_cast<std::size_t>(m->rows);
        co_return m;
    }
    coro::CoroTask<bool> narrow(const dataframe::Expr& predicate) override {
        ++*narrows;
        co_return co_await inner->narrow(predicate);
    }
};

struct CountingSource : dataframe::Source {
    std::shared_ptr<const dataframe::Source> inner;
    mutable std::size_t rows = 0;
    mutable std::size_t narrows = 0;
    explicit CountingSource(std::shared_ptr<const dataframe::Source> in)
        : inner(std::move(in)) {}
    dataframe::Schema schema() const override { return inner->schema(); }
    dataframe::ScanResult scan(
        const dataframe::ScanRequest& req) const override {
        dataframe::ScanResult r = inner->scan(req);
        r.cursor = std::make_unique<CountingCursor>(std::move(r.cursor), &rows,
                                                    &narrows);
        return r;
    }
};

}  // namespace

TEST_SUITE("Dynamic filter pushdown") {
    TEST_CASE(
        "a narrow() prune reads strictly fewer chunks and the same rows") {
        TestEnvironment env(200);
        std::string gz = create_grouped_trace(env);
        std::string idx = env.get_dir() + "/grouped.dftindex";
        REQUIRE(dftu_utils_test::build_index(gz, idx, /*sub_chunk_events=*/0,
                                             /*checkpoint_size=*/256));

        View v = View::from_file(gz, idx).metadata(false);
        const ViewPlan& plan = v.plan();
        ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);

        std::uint64_t static_skipped = 0;
        auto units = run(gather_units(plan, vdef, static_skipped));
        // The small checkpoint size must actually have produced more than one
        // chunk, or this test would not exercise anything.
        REQUIRE(units.size() > 1);

        StringIntern intern_a;
        NameCollectorFold fold_a;
        std::array<Fold*, 1> folds_a{&fold_a};
        ExportStats baseline = run(fuse(plan, vdef, folds_a, intern_a));
        CHECK(baseline.chunks_scanned == units.size());

        // Build the same narrowing ViewSource::scan()/Cursor::narrow() would:
        // a bloom/dictionary prune of every remaining checkpoint against
        // name == "gamma", turned into per-file exclusions.
        q::Expr e = q::field_cmp("name", q::CompareOp::EQ,
                                 q::LiteralNode{std::string("gamma")});
        auto built = std::move(e).build();
        REQUIRE(built.has_value());
        q::Query narrow_query = std::move(built.value());

        DynamicPrune dyn_prune;
        for (const ViewFile& f : plan.files) {
            indexing::ChunkPrunerInput pin{f.index_path, f.file_path,
                                           narrow_query, plan.bloom_cache};
            indexing::ChunkPrunerUtility pruner;
            auto out = run(pruner(pin));
            REQUIRE(out.success);
            std::set<std::uint64_t> keep(out.candidate_checkpoints.begin(),
                                         out.candidate_checkpoints.end());
            std::vector<std::uint64_t> excluded;
            for (std::uint64_t c = 0; c < out.total_checkpoints; ++c)
                if (!keep.count(c)) excluded.push_back(c);
            // The prune must find at least one chunk it can rule out, or the
            // "strictly fewer chunks" assertion below would be vacuous.
            REQUIRE_FALSE(excluded.empty());
            dyn_prune.exclude_checkpoints(f.file_path, excluded);
        }

        StringIntern intern_b;
        NameCollectorFold fold_b;
        std::array<Fold*, 1> folds_b{&fold_b};
        ExportStats narrowed =
            run(fuse(plan, vdef, folds_b, intern_b, nullptr, 0, &dyn_prune));

        // The hook was accepted AND actually used, not just called: fewer
        // units were sealed (real work skipped), and the stats say so.
        CHECK(dyn_prune.skipped() > 0);
        CHECK(narrowed.chunks_skipped > baseline.chunks_skipped);
        CHECK(fold_b.sealed_units < fold_a.sealed_units);

        // Soundness: every "gamma" row the unnarrowed scan saw is still there
        // in the narrowed one - the prune only ever drops chunks that cannot
        // contain a match, never a chunk that does. (The exact count is not
        // asserted: tight checkpointing can replay a boundary chunk's events
        // in more than one unit, which is a property of the index's chunk
        // layout, not of narrowing - the two scans still must agree.)
        auto gamma_a = matching_names(fold_a, intern_a, "gamma");
        auto gamma_b = matching_names(fold_b, intern_b, "gamma");
        CHECK(gamma_a.size() >= 40);
        CHECK(gamma_a == gamma_b);
    }

    TEST_CASE(
        "ViewSource's streaming cursor narrow() drives DynamicPrune and "
        "keeps results correct") {
        TestEnvironment env(200);
        std::string gz = create_grouped_trace(env, /*per_name=*/200);
        std::string idx = env.get_dir() + "/grouped2.dftindex";
        REQUIRE(dftu_utils_test::build_index(gz, idx, /*sub_chunk_events=*/0,
                                             /*checkpoint_size=*/256));

        View v = View::from_file(gz, idx).metadata(false);
        ViewSource src(v);
        // A pushed projection fixes every morsel's columns to exactly this
        // list, in this order (see ScanRequest::projection); the unselected
        // path instead lets a morsel's columns vary batch to batch, which
        // would make "column 0 is pid" an assumption this test cannot make.
        // A tight memory_budget throttles the scan to about one checkpoint
        // in flight at a time (CoroSemaphore gates each unit's byte range
        // before it is claimed), so "narrow partway" below is deterministic
        // instead of racing a scan that a handful of worker threads could
        // otherwise finish before the test ever calls narrow().
        dataframe::ScanRequest req;
        req.projection = {"pid"};
        req.memory_budget = 300;
        dataframe::ScanResult res = src.scan(req);
        REQUIRE(res.cursor);

        // Pull the first morsel before narrowing, exactly like a plugin that
        // learns a bound partway through a scan already in flight.
        auto first = run(res.cursor->next(8));
        REQUIRE(first.has_value());

        // A translatable, index-pushable predicate over the cursor's own
        // schema (pid is column 0, the sole projected column). Numeric,
        // unlike name/cat: translate_leaf (the col-vs-scalar translator both
        // scan() and narrow() share) only understands a numeric scalar today.
        dataframe::Expr pred =
            dataframe::expr_cmp(dataframe::CmpOp::Eq, dataframe::col(0),
                                dataframe::to_scalar(std::int64_t{3}));
        bool did_narrow = run(res.cursor->narrow(pred));
        CHECK(did_narrow);

        // narrow() prunes whole CHUNKS, not rows - it never filters a row
        // itself (that stays the engine's job), so a surviving chunk can still
        // carry a non-pid-3 event. The one thing it must never do is drop a
        // pid-3 event: count every pid across the first morsel plus the rest
        // of the drain.
        auto count_pid3 = [](const dataframe::Morsel& m, std::size_t* total,
                             std::size_t* matched) {
            const dataframe::Series& pid_col = m.columns[0];
            const std::int64_t* pids = pid_col.data<std::int64_t>();
            for (std::int64_t i = 0; i < m.rows; ++i) {
                ++*total;
                if (pids[i] == 3) ++*matched;
            }
        };
        std::size_t total_rows = 0, pid3_rows = 0;
        count_pid3(*first, &total_rows, &pid3_rows);
        for (;;) {
            auto m = run(res.cursor->next(64));
            if (!m.has_value()) break;
            count_pid3(*m, &total_rows, &pid3_rows);
        }

        CHECK(pid3_rows >= 200);

        // The prune must have actually skipped some of the pure non-pid-3
        // chunks, or this only shows narrow() was accepted, not used. Compare
        // against a fresh, unnarrowed scan of the same file/config rather
        // than the nominal 120 events: tight checkpointing can replay a
        // boundary chunk in more than one unit, inflating even a full scan
        // past the nominal count.
        dataframe::ScanResult unnarrowed = src.scan(req);
        REQUIRE(unnarrowed.cursor);
        std::size_t full_total = 0, full_pid3 = 0;
        for (;;) {
            auto m = run(unnarrowed.cursor->next(64));
            if (!m.has_value()) break;
            count_pid3(*m, &full_total, &full_pid3);
        }
        CHECK(total_rows < full_total);
    }

    TEST_CASE(
        "a source that ignores narrow() entirely still returns correct "
        "results") {
        using dataframe::DataFrame;
        using dataframe::InMemorySource;
        using dataframe::LazyFrame;
        using dataframe::Series;

        std::array<std::int64_t, 5> xs{1, 2, 3, 4, 5};
        DataFrame df;
        df.names = {"x"};
        df.columns.push_back(Series::flat_i64(xs.data(), xs.size()));

        auto source = std::make_shared<InMemorySource>(std::move(df));
        dataframe::ScanRequest req;
        dataframe::ScanResult res = source->scan(req);
        REQUIRE(res.cursor);

        // The base Cursor::narrow() a source does not override: it must be
        // honest that it did nothing.
        bool did = run(res.cursor->narrow(dataframe::col(0) > std::int64_t{2}));
        CHECK_FALSE(did);

        // Correctness never depended on the source acting on it: the engine
        // still applies the filter itself over every row the cursor yields.
        DataFrame filtered =
            run(LazyFrame::scan(source)
                    .filter(dataframe::col(0) > std::int64_t{2})
                    .collect());
        REQUIRE(filtered.num_rows() == 3);
    }

    TEST_CASE(
        "a lazy join over a trace scan narrows the scan to its build keys") {
        using dataframe::DataFrame;
        using dataframe::JoinHow;
        using dataframe::LazyFrame;
        using dataframe::Series;

        TestEnvironment env(200);
        std::string gz = create_grouped_trace(env, /*per_name=*/200);
        std::string idx = env.get_dir() + "/grouped3.dftindex";
        REQUIRE(dftu_utils_test::build_index(gz, idx, /*sub_chunk_events=*/0,
                                             /*checkpoint_size=*/256));
        View v = View::from_file(gz, idx).metadata(false);

        // The build side: one key, pid 3, the last of the three groups, in
        // the scan's own key type.
        std::array<std::uint64_t, 1> key{3};
        DataFrame right;
        right.names = {"pid"};
        right.columns.push_back(
            Series::flat(dataframe::TypeId::Uint64, key.data(), 1));

        // The same tight budget as the cursor-level test above, so the
        // producer cannot run the whole scan before the join's first pull
        // sends the narrowing.
        auto joined_src =
            std::make_shared<CountingSource>(std::make_shared<ViewSource>(v));
        DataFrame out = run(LazyFrame::scan(joined_src)
                                .select({"pid"})
                                .join(right.lazy(), {"pid"}, JoinHow::Inner)
                                .memory_budget(300)
                                .collect());
        CHECK(joined_src->narrows == 1);
        REQUIRE(out.num_rows() >= 200);
        const std::uint64_t* pids = out.columns[0].data<std::uint64_t>();
        for (std::int64_t i = 0; i < out.num_rows(); ++i) CHECK(pids[i] == 3);

        // Against a plain scan: the join read strictly fewer rows from the
        // source, and every pid-3 row the full scan has is in the join.
        auto full_src =
            std::make_shared<CountingSource>(std::make_shared<ViewSource>(v));
        DataFrame full = run(LazyFrame::scan(full_src)
                                 .select({"pid"})
                                 .memory_budget(300)
                                 .collect());
        CHECK(full_src->narrows == 0);
        std::int64_t full_pid3 = 0;
        const std::uint64_t* all = full.columns[0].data<std::uint64_t>();
        for (std::int64_t i = 0; i < full.num_rows(); ++i)
            if (all[i] == 3) ++full_pid3;
        CHECK(out.num_rows() == full_pid3);
        CHECK(joined_src->rows < full_src->rows);
    }
}
