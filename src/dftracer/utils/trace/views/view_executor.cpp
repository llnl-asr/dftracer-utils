#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/trace/views/bloom_fold.h>
#include <dftracer/utils/trace/views/containment_fold.h>
#include <dftracer/utils/trace/views/coverage.h>
#include <dftracer/utils/trace/views/dict_fold.h>
#include <dftracer/utils/trace/views/engine_agg_fold.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/index_fold_driver.h>
#include <dftracer/utils/trace/views/mv_store.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/pipeline.h>
#include <dftracer/utils/trace/views/rollup_store.h>
#include <dftracer/utils/trace/views/typed_collect_fold.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_agg_tier.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_counter_format.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_resolver.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/common/serialization/binary_codec.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/index_database_writer_context.h>
#include <dftracer/utils/utilities/indexer/internal/gzip/gzip_indexer.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/indexer/internal/indexer.h>
#include <rocksdb/sst_file_writer.h>
#include <simdjson.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// The View terminals: each lowers a ViewPlan onto the module pieces and runs a
// pruned parallel scan. The scan Source lives in view_scan (gather_units +
// make_vdef); the group/agg fold in view_aggregate; ph="C" emission in
// view_counter_format; out-of-core spill in view_spill. A terminal is therefore
// "gather units -> fan out a scan -> feed each batch to its own body (write /
// fold / spill / write_chunk / branch-route)". The bodies differ on purpose;
// only the shared scan setup is factored out.
namespace dftracer::utils::trace::views::detail {

// Every file is a clean first touch: it has an index path that does not exist
// yet, so a bootstrap can build it (and there is nothing to read instead).
static bool all_files_fresh(const ViewPlan& plan) {
    for (const auto& f : plan.files)
        if (f.index_path.empty() || fs::exists(f.index_path)) return false;
    return true;
}

bool export_bootstrap_eligible(const ViewPlan& plan) {
    if (plan.query || plan.time_range || plan.offset || plan.limit ||
        plan.phase != Phase::Any || !plan.include_metadata ||
        !plan.select.empty() || plan.files.empty())
        return false;
    return all_files_fresh(plan);
}

// Project one NDJSON event to the selected bare-name fields (found at top level
// or nested in `args`), emitted flat as a JSON object - the raw-event form of
// project_columns. `parser` is caller-owned: a thread_local parser is unsafe
// here because the export emit runs on pooled coroutine threads.
static std::string project_event(simdjson::dom::parser& parser,
                                 std::string_view ev,
                                 const std::vector<std::string>& select) {
    simdjson::padded_string padded(ev);
    simdjson::dom::element doc;
    if (parser.parse(padded).get(doc) || !doc.is_object())
        return std::string(ev);
    simdjson::dom::object args;
    const bool has_args = !doc["args"].get(args);

    std::string out = "{";
    bool first = true;
    for (const auto& name : select) {
        simdjson::dom::element val;
        bool found = !doc[name].get(val);
        if (!found && has_args) found = !args[name].get(val);
        if (!found) continue;
        std::ostringstream os;
        os << val;
        if (!first) out += ',';
        first = false;
        out += '"';
        out += name;
        out += "\":";
        out += os.str();
    }
    out += '}';
    return out;
}

bool collect_bootstrap_eligible(const ViewPlan& plan) {
    if (plan.files.empty() || plan.time_range) return false;
    if (plan.query && !query_evaluable_by_fold(*plan.query)) return false;
    // A partial source (chunk-stats fast path) answers only from an existing
    // index, so it contributes nothing once all_files_fresh holds - no need to
    // exclude it here.
    return all_files_fresh(plan);
}

namespace {

// Write the index a bootstrap pass produced: members from the artifacts, bloom
// + hash from the folds. Caps claim MEMBERS/FILE_SUMMARY/INDEXING_COMPLETE
// here; BloomFold adds BLOOM.
void persist_bootstrap_index(
    const std::string& index_path, const std::string& file_path,
    const utilities::indexer::internal::gzip::GzipBuildArtifacts& arts,
    BloomFold& bloom, DictFold& dict) {
    namespace idx = utilities::indexer;
    idx::IndexDatabase db(index_path);
    auto logical = idx::internal::get_logical_path(file_path);
    const auto hash = idx::internal::calculate_file_hash(file_path);
    const auto mtime = static_cast<std::uint64_t>(
        idx::internal::get_file_modification_time(file_path));
    const auto bytes = idx::internal::file_size_bytes(file_path);
    auto w = db.begin_write();
    int fid = w->get_or_create_file_info(
        logical, hash,
        idx::IndexFileEntryCapability::INDEXING_COMPLETE |
            idx::IndexFileEntryCapability::MEMBERS |
            idx::IndexFileEntryCapability::FILE_SUMMARY,
        mtime, bytes);
    for (const auto& m : arts.members) w->insert_gzip_member(fid, m);
    w->insert_file_metadata(fid, arts.checkpoint_size, arts.total_lines,
                            arts.total_uc_size);
    bloom.write_to_sink(*w, fid);
    dict.write_to_sink(*w);
    w->add_file_capability(fid, idx::IndexFileEntryCapability::BLOOM);
    w->commit();
}

// An IndexVisitor that streams a member's plaintext lines straight to the
// export sink. Carries the partial last line across on_chunk calls so an event
// straddling an inflate boundary is written whole.
class SinkWriterVisitor : public utilities::indexer::IndexVisitor {
   public:
    explicit SinkWriterVisitor(ExportSink& sink) : sink_(&sink) {}
    void begin(std::size_t) override {}
    coro::CoroTask<void> on_checkpoint(std::size_t) override { co_return; }
    coro::CoroTask<void> on_chunk(const char* data, std::size_t len,
                                  std::size_t) override {
        buf_.append(data, len);
        const std::size_t last_nl = buf_.rfind('\n');
        if (last_nl != std::string::npos) {
            sink_->write(std::string_view(buf_.data(), last_nl + 1));
            buf_.erase(0, last_nl + 1);
        }
        co_return;
    }
    void finalize(utilities::indexer::IndexDatabaseWriterContext&,
                  int) override {}

   private:
    ExportSink* sink_;
    std::string buf_;
};

// First-touch full export: for an unindexed, unfiltered whole-trace dump, one
// raw-gzip pass per file streams every line to the sink AND builds the index.
// Returns false unless every file is a clean first touch and the export is a
// plain full dump (no filter, window, pagination, or phase/metadata selection),
// so anything else falls through to the streaming scanner path.
coro::CoroTask<bool> try_export_bootstrap(const ViewPlan& plan,
                                          ExportSink& sink) {
    namespace idx = utilities::indexer;
    namespace gzi = utilities::indexer::internal::gzip;
    if (!export_bootstrap_eligible(plan)) co_return false;

    for (const auto& f : plan.files) {
        dftracer::utils::StringIntern intern;
        BloomFold bloom(intern);
        DictFold dict(intern);
        std::array<Fold*, 2> folds{&bloom, &dict};
        IndexFoldDriver driver(intern, folds, f.file_path, f.index_path);
        SinkWriterVisitor writer(sink);

        gzi::GzipBuildArtifacts arts;
        bool ok = false;
        co_await dftracer::utils::run_coro_scope(
            [&](dftracer::utils::CoroScope& scope) -> coro::CoroTask<void> {
                idx::internal::Indexer::VisitorList vl;
                vl.emplace_back(driver);
                vl.emplace_back(writer);
                auto a = co_await gzi::build_gzip_index_artifacts(
                    f.file_path,
                    idx::internal::Indexer::DEFAULT_CHECKPOINT_SIZE, vl,
                    &scope);
                if (a) {
                    arts = std::move(*a);
                    ok = true;
                }
                co_return;
            });
        if (!ok) co_return false;
        driver.seal();
        persist_bootstrap_index(f.index_path, f.file_path, arts, bloom, dict);
    }
    co_return true;
}

}  // namespace

namespace {

// Re-point a read at a materialized filtered-trace view that subsumes it, if
// one exists. The predicate/window/phase are kept so the scan re-applies them
// on the (smaller) MV rows - the compensation that makes reuse correct even
// when the MV is broader than the query.
ViewPlan redirect_reads(const ViewPlan& plan, bool* redirected = nullptr) {
    ViewPlan eff = plan;
    if (auto mv = find_subsuming_view(plan)) {
        eff.files = std::move(*mv);
        if (redirected) *redirected = true;
    }
    return eff;
}

}  // namespace

coro::CoroTask<ExportStats> run_export(const ViewPlan& base_plan,
                                       ExportSink& sink) {
    bool from_mv = false;
    const ViewPlan plan = redirect_reads(base_plan, &from_mv);
    // First touch: one raw-gzip pass dumps the trace and builds its index.
    if (co_await try_export_bootstrap(plan, sink)) co_return ExportStats{};

    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);
    std::mutex sink_mtx;
    // Pagination: skip `offset` events then write `limit`. The scan cap stops
    // the fan-out once offset+limit events are produced; the offset/limit
    // window is applied under the sink lock, so it is scan-order (not a stable
    // page).
    const std::uint64_t cap = plan.limit ? plan.offset + plan.limit : 0;
    std::uint64_t seen = 0, written = 0;
    // Build whatever index artifacts this scan can establish along the way.
    // Nothing attaches unless the scan already satisfies it, so this cannot
    // make the read slower than not having it.
    dftracer::utils::StringIntern intern;
    auto folds = select_index_folds(plan, vdef, intern);
    std::vector<Fold*> fold_ptrs;
    for (auto& f : folds) fold_ptrs.push_back(f.get());
    // Stream matched events; --select projects each one to the chosen fields
    // (SQL-style), otherwise they are written verbatim. The scan driver owns
    // the gather + fan-out. The projection parser is used under sink_mtx.
    simdjson::dom::parser proj_parser;
    ExportStats stats = co_await for_each_scanned_batch(
        plan, vdef, /*num_slots=*/0, cap,
        [&](std::size_t, const std::vector<std::string_view>& events) {
            std::lock_guard<std::mutex> lk(sink_mtx);
            for (const auto& ev : events) {
                if (seen++ < plan.offset) continue;
                if (plan.limit && written >= plan.limit) break;
                if (plan.select.empty()) {
                    sink.write(ev);
                } else {
                    sink.write(project_event(proj_parser, ev, plan.select));
                }
                sink.write("\n");
                ++written;
            }
        },
        fold_ptrs, &intern);
    stats.served_from_mv = from_mv;
    co_return stats;
}

coro::CoroTask<ExportStats> run_export_trace(const ViewPlan& plan,
                                             const TraceWriteOptions& opts) {
    namespace pfw = utilities::fileio::parallel;
    namespace cmp = utilities::fileio::compress;
    // Member size defaults to the checkpoint granularity (a member == a chunk).
    constexpr std::size_t DEFAULT_FLUSH_BYTES =
        constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    constexpr std::size_t BUFFER_HEADROOM_BYTES = 1 * 1024 * 1024;

    if (opts.build_index)
        co_return co_await run_export_trace_indexed(plan, opts);

    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);

    std::uint64_t skipped = 0;
    auto units = co_await gather_units(plan, vdef, skipped);

    const std::size_t baseline = std::max<std::size_t>(1, opts.num_workers);
    auto cw = pfw::make_writer_for_path(
        {opts.output_path, baseline,
         opts.member_size ? opts.member_size : DEFAULT_FLUSH_BYTES,
         BUFFER_HEADROOM_BYTES, opts.compress});
    const std::size_t num_workers = cw.sizing.num_workers;
    const std::size_t member_size = cw.sizing.flush_threshold;
    auto writer = std::move(cw.writer);

    std::vector<std::uint64_t> matched_v(num_workers, 0);
    std::vector<std::uint64_t> scanned_v(num_workers, 0);
    bool ok = true;

    // One scope owns the writer's internal packer (padded layout) plus the
    // per-worker producers; close() runs inside it so the packer is drained
    // before the scope joins.
    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        if (co_await writer->open(opts.output_path, num_workers, opts.compress,
                                  &scope) != 0) {
            ok = false;
            co_return;
        }

        co_await scope.scope([&](CoroScope& child) -> coro::CoroTask<void> {
            for (std::size_t w = 0; w < num_workers; ++w) {
                child.spawn([&, w](CoroScope&) -> coro::CoroTask<void> {
                    cmp::GzipMemberCompressor comp(opts.level);
                    std::string buf;
                    std::vector<std::uint8_t> scratch;

                    // Emit buf[0, cut) as one self-contained member (whole
                    // lines, so it is valid at any offset). Returns false on
                    // write failure.
                    auto emit = [&](std::size_t cut) -> coro::CoroTask<bool> {
                        ByteView chunk;
                        if (opts.compress) {
                            if (!comp.compress_member_into(scratch, buf.data(),
                                                           cut)) {
                                co_return false;
                            }
                            chunk = ByteView(
                                reinterpret_cast<const char*>(scratch.data()),
                                scratch.size());
                        } else {
                            chunk = ByteView(buf.data(), cut);
                        }
                        bool wrote =
                            co_await writer->write_chunk(w, chunk) == 0;
                        buf.erase(0, cut);
                        co_return wrote;
                    };

                    for (std::size_t i = w; i < units.size();
                         i += num_workers) {
                        if (is_cancelled(plan)) co_return;
                        ViewScannerInput sin =
                            make_scanner_input(units[i], vdef, vdef.query);
                        ViewScannerUtility scanner;
                        auto gen = scanner(sin);
                        while (auto b = co_await gen.next()) {
                            if (is_cancelled(plan)) co_return;
                            matched_v[w] += b->events_matched;
                            scanned_v[w] += b->events_scanned;
                            for (const auto& ev : b->events) {
                                buf.append(ev);
                                buf.push_back('\n');
                            }
                            while (buf.size() >= member_size) {
                                std::size_t cut = member_size;
                                while (cut < buf.size() && buf[cut] != '\n')
                                    ++cut;
                                if (cut >= buf.size()) break;  // no newline yet
                                ++cut;  // include it: member holds whole lines
                                if (!co_await emit(cut)) {
                                    ok = false;
                                    co_return;
                                }
                            }
                        }
                    }
                    if (!buf.empty() && !co_await emit(buf.size())) ok = false;
                    co_return;
                });
            }
            co_return;
        });

        if (co_await writer->close() != 0) ok = false;
    });

    if (ok && cw.layout.layout == pfw::FileLayout::SHARDED) {
        auto shards = writer->output_paths();
        if (co_await pfw::merge_shards(opts.output_path, shards) != 0)
            ok = false;
    }

    if (!ok) {
        throw DFTUtilsException(
            ErrorCode::IO, "export_trace failed writing " + opts.output_path);
    }

    ExportStats st;
    st.chunks_skipped = skipped;
    st.chunks_scanned = units.size();
    for (auto m : matched_v) st.events_matched += m;
    for (auto s : scanned_v) st.events_scanned += s;
    co_return st;
}

coro::CoroTask<ExportStats> run_folds(const ViewPlan& plan,
                                      std::span<Fold* const> folds,
                                      dftracer::utils::StringIntern& intern,
                                      DynamicPrune* dyn_prune) {
    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);
    co_return co_await fuse(plan, vdef, folds, intern, /*covered=*/nullptr,
                            /*limit=*/0, dyn_prune);
}

namespace {

// First-touch fast path: for a plan whose files have no index yet, one raw-gzip
// pass per file both aggregates it and builds its full index (members + bloom +
// hash) as a byproduct, so the query never triggers a separate eager index
// build. Returns false (caller falls through to the indexed path) unless every
// file is a clean first touch and nothing materialized can answer instead.
coro::CoroTask<bool> try_collect_bootstrap(const ViewPlan& plan,
                                           dataframe::AggStatePtr& merged) {
    namespace idx = utilities::indexer;
    namespace gzi = utilities::indexer::internal::gzip;
    // A time window still needs the indexed path (the bootstrap has no ts
    // pruning), and a predicate is only applied here when every field it names
    // is POD-evaluable; otherwise defer to the scan, which filters properly.
    if (!collect_bootstrap_eligible(plan)) co_return false;

    // A transform on a resolved (hash-backed) group key must resolve the hash
    // to its name before transforming, but the name tables are still being
    // built during this pass. Build the index in this pass, then re-aggregate
    // through the normal engine scan (which reads the now-complete tables).
    const bool resolved_transform = std::any_of(
        plan.group_by.begin(), plan.group_by.end(), [](const GroupKey& g) {
            return g.transform != GroupKey::Transform::None &&
                   (g.kind == GroupKey::Kind::FilePath ||
                    g.kind == GroupKey::Kind::FileName ||
                    g.kind == GroupKey::Kind::HostName ||
                    g.kind == GroupKey::Kind::Rank);
        });

    for (const auto& f : plan.files) {
        dftracer::utils::StringIntern intern;
        EngineAggFold agg(plan, intern, /*apply_query=*/true);
        BloomFold bloom(intern);
        DictFold dict(intern);
        std::array<Fold*, 3> folds{&agg, &bloom, &dict};
        IndexFoldDriver driver(intern, folds, f.file_path, f.index_path,
                               extra_capture_fields(plan));

        gzi::GzipBuildArtifacts arts;
        bool ok = false;
        co_await dftracer::utils::run_coro_scope(
            [&](dftracer::utils::CoroScope& scope) -> coro::CoroTask<void> {
                idx::internal::Indexer::VisitorList vl;
                vl.emplace_back(driver);
                auto a = co_await gzi::build_gzip_index_artifacts(
                    f.file_path,
                    idx::internal::Indexer::DEFAULT_CHECKPOINT_SIZE, vl,
                    &scope);
                if (a) {
                    arts = std::move(*a);
                    ok = true;
                }
                co_return;
            });
        if (!ok) co_return false;
        driver.seal();
        persist_bootstrap_index(f.index_path, f.file_path, arts, bloom, dict);
        if (resolved_transform) continue;  // index only; re-scan below
        if (!merged)
            merged = dataframe::agg_deserialize(
                dataframe::agg_serialize(agg.state()));
        else
            dataframe::agg_merge(*merged, agg.state());
        apply_ranks(plan, agg.ranks());
    }
    // The index now carries the name tables; a normal engine scan resolves the
    // transformed key correctly. Reset the resolver the per-file folds cached
    // before the tables existed so the re-scan rebuilds it from the fresh
    // index.
    if (resolved_transform) {
        ViewPlan rescan = plan;
        rescan.resolver.reset();
        merged = co_await build_engine_agg_state(rescan);
    }
    co_return true;
}

// Releases an MV directory lock (see lock_view_dir) on scope exit, so the
// single-flight lock is dropped on every path including a throw from the build.
class ViewDirLock {
   public:
    explicit ViewDirLock(int fd) noexcept : fd_(fd) {}
    ~ViewDirLock() { unlock_view_dir(fd_); }
    ViewDirLock(const ViewDirLock&) = delete;
    ViewDirLock& operator=(const ViewDirLock&) = delete;

   private:
    int fd_;
};

}  // namespace

coro::CoroTask<ExportStats> run_materialize(const ViewPlan& plan,
                                            const ProgressFn* progress) {
    ensure_schema(plan);

    // Row query (no aggregation): materialize a filtered trace + index into a
    // slug directory under .dftindex-views, then record its manifest so a later
    // subsuming query reads it instead of rescanning the base.
    if (plan.group_by.empty() && plan.agg.empty() &&
        !plan.auto_numeric_metrics) {
        if (view_is_fresh(plan)) co_return ExportStats{};  // already built
        const std::string dir = materialize_view_dir(plan);
        if (dir.empty()) co_return ExportStats{};
        // Single-flight: one builder per slug dir. A loser skips - the winner
        // produces the MV. Re-check freshness under the lock in case the winner
        // just finished.
        const int lock = lock_view_dir(dir);
        if (lock < 0) co_return ExportStats{};
        ViewDirLock lock_guard(lock);
        if (view_is_fresh(plan)) co_return ExportStats{};
        constexpr std::uint64_t DEFAULT_PART_SIZE = 128ull * 1024 * 1024;
        TraceWriteOptions opts;
        opts.output_path = (fs::path(dir) / "part.pfw.gz").string();
        opts.build_index = true;
        opts.compress = true;
        // One gzip member per index checkpoint, so the user's checkpoint_size
        // is the member size to write at.
        opts.member_size = plan.mv_checkpoint_size;
        opts.part_size =
            plan.mv_part_size ? plan.mv_part_size : DEFAULT_PART_SIZE;
        ExportStats stats =
            co_await run_export_trace_indexed(plan, opts, progress);
        register_view(dir, plan);
        co_return stats;
    }

    const std::string rdir = rollup_index_path(plan);
    // Idempotent prewarm: skip if this view is already materialized.
    if (!rdir.empty() && fs::exists(fs::path(rdir) / "CURRENT")) {
        try {
            auto db = open_rollup_db(
                rdir, rocksdb::RocksDatabase::OpenMode::ReadOnly);
            if (db && rollup_exists(*db, plan_signature(plan)))
                co_return ExportStats{};
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("rollup check skipped: %s", e.what());
        }
    }

    // A build wants full coverage: scan through the engine group-by to a
    // complete mergeable AggState, then persist its per-group partials.
    auto state = co_await build_engine_agg_state(plan);
    if (!rdir.empty()) {
        try {
            auto db = open_rollup_db(
                rdir, rocksdb::RocksDatabase::OpenMode::ReadWrite);
            if (db)
                persist_rollup(*db, plan_signature(plan), rest_signature(plan),
                               plan.time_bucket_us, plan.group_by, *state);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("rollup materialize skipped: %s", e.what());
        }
    }
    co_return ExportStats{};
}

// Serve `plan` from a subsuming persisted rollup with no scan: re-aggregate the
// stored AggState partials to `plan`'s grouping and finalize. Shared by the
// engine collect path and the session so the two agree on what a rollup
// answers.
std::optional<dataframe::DataFrame> try_serve_rollup(const ViewPlan& plan) {
    const std::string rdir = rollup_index_path(plan);
    if (rdir.empty() || !fs::exists(fs::path(rdir) / "CURRENT"))
        return std::nullopt;
    // A materialize() query opens ReadWrite so a miss reuses the handle to
    // persist below (avoids a RO-then-RW conflict).
    const auto mode = plan.materialize
                          ? rocksdb::RocksDatabase::OpenMode::ReadWrite
                          : rocksdb::RocksDatabase::OpenMode::ReadOnly;
    try {
        auto db = open_rollup_db(rdir, mode);
        if (db) return find_subsuming_rollup(*db, plan);
    } catch (const std::exception& e) {
        // A locked/unreadable rollup just falls through to a normal compute;
        // never fail the query over the cache.
        DFTRACER_UTILS_LOG_WARN("rollup read skipped: %s", e.what());
    }
    return std::nullopt;
}

// The no-scan fast paths: the first-touch raw-gzip bootstrap, then the
// aggregation tier. On a hit fills `out` (a mergeable engine AggState, finalize
// with finalize_engine_result) and returns true; false means the query must
// scan. The AggState rollup is served separately by try_serve_rollup.
coro::CoroTask<bool> try_serve_aggregate_no_scan(const ViewPlan& plan,
                                                 dataframe::AggStatePtr& out) {
    if (co_await try_collect_bootstrap(plan, out)) co_return true;
    if (agg_tier_collect(plan, out)) co_return true;
    co_return false;
}

// Resolve a min-aligned bucket origin to the trace's minimum timestamp, read
// from the index zone maps (no event scan), in the post-time_scale unit the
// fold buckets in. Idempotent: a plan not requesting min alignment (or with no
// bucket) is returned unchanged, so terminals can call it defensively. A
// missing/locked index leaves the origin at 0 (absolute).
ViewPlan resolve_bucket_origin(const ViewPlan& plan) {
    if (!plan.bucket_origin_min || plan.time_bucket_us == 0) return plan;
    namespace idx = utilities::indexer;
    std::uint64_t global_min = std::numeric_limits<std::uint64_t>::max();
    for (const auto& f : plan.files) {
        if (f.index_path.empty() || !fs::exists(f.index_path)) continue;
        try {
            idx::IndexDatabase db(f.index_path, idx::IndexOpenMode::ReadOnly);
            const int fid = db.get_file_info_id(
                idx::internal::get_logical_path(f.file_path));
            if (fid < 0) continue;
            const auto b = db.query_time_bounds(fid);
            if (b.valid && b.min_timestamp_us < global_min)
                global_min = b.min_timestamp_us;
        } catch (const std::exception&) {
            // Leave this file out; origin falls back to 0 if none resolve.
        }
    }
    ViewPlan p = plan;
    p.bucket_origin_min = false;
    p.bucket_origin_us =
        global_min == std::numeric_limits<std::uint64_t>::max()
            ? 0
            : static_cast<std::uint64_t>(static_cast<double>(global_min) *
                                         p.time_scale);
    return p;
}

coro::CoroTask<TypedResult> run_collect_typed(const ViewPlan& plan,
                                              int shard_begin, int shard_end,
                                              const ProgressFn* progress) {
    ensure_schema(plan);
    TypedResult out;
    if (events_profiles_collect(plan, out.regular, out.aggregated, shard_begin,
                                shard_end, progress)) {
        system_collect(plan, out.counters, shard_begin, shard_end, progress);
        co_return out;
    }
    // The tier could not answer (a ph/ts predicate it cannot key on, or no tier
    // built); scan the raw trace so collect_typed
    // returns rows rather than silently empty.
    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);
    dftracer::utils::StringIntern intern;
    TypedCollectFold fold(intern);
    std::array<Fold*, 1> folds{&fold};
    co_await fuse(plan, vdef, folds, intern);
    out.regular = fold.build_regular();
    out.aggregated = fold.build_aggregated();
    out.counters = fold.build_counters();
    co_return out;
}

bool is_row_query(const ViewPlan& plan) {
    return plan.group_by.empty() && plan.agg.empty() &&
           !plan.auto_numeric_metrics;
}

coro::CoroTask<dataframe::DataFrame> run_collect_rows(const ViewPlan& plan) {
    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);
    dftracer::utils::StringIntern intern;
    // resolved.*/r.* select columns need the index name tables; build the
    // resolver once (shared across parallel slices) only when asked for.
    std::shared_ptr<const GroupResolver> resolver;
    if (select_needs_resolver(plan.select)) {
        std::vector<std::string> index_paths;
        index_paths.reserve(plan.files.size());
        for (const auto& f : plan.files) index_paths.push_back(f.index_path);
        resolver = std::make_shared<const GroupResolver>(index_paths);
    }
    NativeRowFold fold(intern, plan.select, plan.time_scale, resolver,
                       plan.phase == Phase::Metadata);
    std::array<Fold*, 1> folds{&fold};
    co_await execute(lower_fused_folds(plan, vdef, folds), intern);
    dataframe::DataFrame b = fold.build();
    // sort_col/topk_col name a column the way a caller would select it (bare
    // or "args."-prefixed); canonicalize to match build_row_frame's actual
    // output name before resolving against the built frame.
    if (!plan.sort_col.empty())
        b = b.sort_by(canonical_row_column_name(plan.sort_col), plan.sort_desc);
    if (!plan.topk_col.empty())
        b = b.topk(canonical_row_column_name(plan.topk_col), plan.topk_k,
                   plan.topk_largest);
    if (plan.offset || plan.limit) {
        const std::int64_t off = static_cast<std::int64_t>(plan.offset);
        const std::int64_t len =
            plan.limit ? static_cast<std::int64_t>(plan.limit) : b.num_rows();
        b = b.slice(off, len);
    }
    co_return b;
}

namespace {

template <class R>
coro::CoroTask<R> run_containment_terminal(
    const ViewPlan& plan, std::vector<std::string> partition,
    std::string ts_field, std::string dur_field, std::string name_field,
    std::vector<std::string> group, R (ContainmentFold::*build)() const) {
    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);
    dftracer::utils::StringIntern intern;
    ContainmentFold fold(intern, std::move(partition), std::move(ts_field),
                         std::move(dur_field), std::move(name_field),
                         plan.time_scale, std::move(group));
    std::array<Fold*, 1> folds{&fold};
    co_await fuse(plan, vdef, folds, intern);
    co_return (fold.*build)();
}

}  // namespace

coro::CoroTask<dataframe::DataFrame> run_call_tree(
    const ViewPlan& plan, std::vector<std::string> partition,
    std::string ts_field, std::string dur_field, std::string name_field) {
    return run_containment_terminal(
        plan, std::move(partition), std::move(ts_field), std::move(dur_field),
        std::move(name_field), {}, &ContainmentFold::call_tree);
}

coro::CoroTask<dataframe::DataFrame> run_flamegraph(
    const ViewPlan& plan, std::vector<std::string> partition,
    std::string ts_field, std::string dur_field, std::string name_field,
    std::vector<std::string> group) {
    return run_containment_terminal(
        plan, std::move(partition), std::move(ts_field), std::move(dur_field),
        std::move(name_field), std::move(group), &ContainmentFold::flamegraph);
}

coro::CoroTask<std::pair<dataframe::DataFrame, dataframe::DataFrame>>
run_containment(const ViewPlan& plan, std::vector<std::string> partition,
                std::string ts_field, std::string dur_field,
                std::string name_field, std::vector<std::string> group) {
    return run_containment_terminal(
        plan, std::move(partition), std::move(ts_field), std::move(dur_field),
        std::move(name_field), std::move(group), &ContainmentFold::containment);
}

coro::CoroTask<std::string> run_flamegraph_partial(
    const ViewPlan& plan, std::vector<std::string> partition,
    std::string ts_field, std::string dur_field, std::string name_field,
    std::vector<std::string> group) {
    return run_containment_terminal(plan, std::move(partition),
                                    std::move(ts_field), std::move(dur_field),
                                    std::move(name_field), std::move(group),
                                    &ContainmentFold::flamegraph_partial);
}

namespace {

// One-byte wire tag prefixing every aggregate partial (distributed merge; never
// persisted), so the merge side can reject a foreign blob. Every partial is now
// a mergeable engine AggState (the scan path and the index-only tier fast path
// both emit it); a partial with any other leading byte is rejected loudly.
enum class AggWireTag : std::uint8_t { AggState = 2 };

std::string encode_agg_partial(const dataframe::AggState& st) {
    std::string out(1, static_cast<char>(AggWireTag::AggState));
    out += dataframe::agg_serialize(st);
    return out;
}

// Merge every partial into one AggState. An empty or unknown-tagged blob is
// rejected, not misparsed.
dataframe::AggStatePtr decode_partials(
    const std::vector<std::string_view>& partials) {
    dataframe::AggStatePtr state;
    for (std::string_view p : partials) {
        if (p.empty())
            throw DFTUtilsException::cat(
                ErrorCode::AGGREGATION,
                "aggregate partial: empty (missing wire tag)");
        if (static_cast<std::uint8_t>(p[0]) !=
            static_cast<std::uint8_t>(AggWireTag::AggState))
            throw DFTUtilsException::cat(ErrorCode::AGGREGATION,
                                         "aggregate partial: unknown wire tag");
        dataframe::AggStatePtr st =
            dataframe::agg_deserialize(std::string(p.substr(1)));
        if (!state)
            state = std::move(st);
        else
            dataframe::agg_merge(*state, *st);
    }
    return state;
}

// An empty engine AggState shaped like `plan`'s aggregation (right specs and
// key columns, zero groups), so a merge with no partials still finalizes to the
// correct empty columns.
dataframe::AggStatePtr empty_engine_state(const ViewPlan& plan) {
    AggInputSpec spec = make_agg_input_spec(plan);
    dataframe::LoweredGroupAggs lowered =
        dataframe::lower_group_aggs(spec.gaggs);
    auto st = dataframe::agg_new(lowered.specs, spec.dyn_specs);
    const std::size_t nkeys =
        (plan.time_bucket_us > 0 ? 1u : 0u) + plan.group_by.size();
    dataframe::agg_seed_begin(*st, nkeys);
    dataframe::agg_seed_finalize(*st);
    return st;
}

}  // namespace

// Distributed materialize: combine the rank-local partials and persist the
// merged rollup.
coro::CoroTask<void> run_materialize_partials(
    const ViewPlan& plan, const std::vector<std::string_view>& partials) {
    ensure_schema(plan);
    const std::string rdir = rollup_index_path(plan);
    if (rdir.empty()) co_return;
    dataframe::AggStatePtr state = decode_partials(partials);
    if (!state) co_return;
    try {
        auto db =
            open_rollup_db(rdir, rocksdb::RocksDatabase::OpenMode::ReadWrite);
        if (db)
            persist_rollup(*db, plan_signature(plan), rest_signature(plan),
                           plan.time_bucket_us, plan.group_by, *state);
    } catch (const std::exception& e) {
        DFTRACER_UTILS_LOG_WARN("rollup materialize (partials) skipped: %s",
                                e.what());
    }
    co_return;
}

std::optional<dataframe::DataFrame> run_reconstruct_if_cached(
    const ViewPlan& plan) {
    if (plan.group_by.empty() && plan.agg.empty()) return std::nullopt;
    ensure_schema(plan);
    return try_serve_rollup(plan);
}

coro::CoroTask<ExportStats> run_scan_batches(
    const std::shared_ptr<const ViewPlan>& plan_ptr, std::size_t num_slots,
    std::uint64_t limit,
    const std::function<void(std::size_t,
                             const std::vector<std::string_view>&)>& on_batch) {
    bool from_mv = false;
    const ViewPlan plan = redirect_reads(*plan_ptr, &from_mv);
    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);
    // map_batches wants a fixed slot count; treat 0 as 1.
    ExportStats stats = co_await for_each_scanned_batch(
        plan, vdef, num_slots ? num_slots : 1, limit, on_batch);
    stats.served_from_mv = from_mv;
    co_return stats;
}

// An externally-built Fold joined to the shared scan (the plugin/JIT seam).
// `make` builds it with the scan's intern so ids agree and per-worker slices
// merge; `finalize` runs after the merge.
struct FoldFactory {
    std::function<std::unique_ptr<Fold>(dftracer::utils::StringIntern&)> make;
    std::function<void()> finalize;
};

struct ViewSessionState {
    std::shared_ptr<const ViewPlan> plan;
    std::vector<BranchHooks> branches;
    std::vector<FoldFactory> fold_factories;
    /// A narrowing an attached fold offered for the shared scan, applied by
    /// execute() only when no other branch exists. The scan feeds every
    /// branch, so narrowing it for one would starve the rest; whether that
    /// holds is knowable only here, after every branch has been added.
    std::optional<query::Query> proposed_prune;
};

std::shared_ptr<ViewSessionState> make_view_session_state(
    std::shared_ptr<const ViewPlan> plan) {
    auto st = std::make_shared<ViewSessionState>();
    st->plan = std::move(plan);
    return st;
}

void propose_base_prune(ViewSessionState& state, query::Query q) {
    state.proposed_prune = std::move(q);
}

void add_branch(ViewSessionState& state, BranchHooks hooks) {
    state.branches.push_back(std::move(hooks));
}

void add_fold_factory(
    ViewSessionState& state,
    std::function<std::unique_ptr<Fold>(dftracer::utils::StringIntern&)> make,
    std::function<void()> finalize) {
    state.fold_factories.push_back({std::move(make), std::move(finalize)});
}

void add_fold_branch(ViewSessionState& state, Query predicate,
                     std::function<BranchConsumer()> make_consumer,
                     std::function<void()> finalize) {
    BranchHooks h;
    h.predicate = std::move(predicate);
    h.make_consumer = std::move(make_consumer);
    h.finalize = std::move(finalize);
    add_branch(state, std::move(h));
}

void add_fold_branch(ViewSessionState& state,
                     std::function<BranchConsumer()> make_consumer,
                     std::function<void()> finalize) {
    BranchHooks h;  // predicate unset = match all scanned events
    h.make_consumer = std::move(make_consumer);
    h.finalize = std::move(finalize);
    add_branch(state, std::move(h));
}

void add_materialize_branch(ViewSessionState& state,
                            std::vector<GroupKey> group_by,
                            std::vector<AggSpec> agg) {
    auto plan = std::make_shared<ViewPlan>(*state.plan);
    plan->group_by = std::move(group_by);
    plan->agg = std::move(agg);
    plan->schema.reset();
    plan->resolver.reset();
    ensure_schema(*plan);

    BranchHooks h;
    // The engine builds AggState partials from columnar batches, which the
    // session's per-event fold cannot feed, so the materialize branch does no
    // per-event work and (re)builds the rollup through the engine in finalize.
    h.make_consumer = []() -> BranchConsumer {
        return [](const json::JsonValue&, std::string_view) {};
    };
    h.finalize = [plan]() {
        const std::string rdir = rollup_index_path(*plan);
        if (rdir.empty()) return;
        try {
            dataframe::AggStatePtr state_out;
            dftracer::utils::default_runtime().run_blocking(
                "session_materialize",
                [&](dftracer::utils::CoroScope&) -> coro::CoroTask<void> {
                    state_out = co_await build_engine_agg_state(*plan);
                    co_return;
                });
            if (!state_out) return;
            auto db = open_rollup_db(
                rdir, rocksdb::RocksDatabase::OpenMode::ReadWrite);
            if (db)
                persist_rollup(*db, plan_signature(*plan),
                               rest_signature(*plan), plan->time_bucket_us,
                               plan->group_by, *state_out);
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("fused rollup materialize skipped: %s",
                                    e.what());
        }
    };
    add_branch(state, std::move(h));
}

BranchHooks make_export_branch(ExportSink& sink,
                               std::shared_ptr<ExportStats> out) {
    // The sink is one shared object, so writes still serialize on a mutex; only
    // the count is per worker.
    auto mtx = std::make_shared<std::mutex>();
    auto counts =
        std::make_shared<std::vector<std::shared_ptr<std::uint64_t>>>();

    BranchHooks h;
    h.make_consumer = [&sink, mtx, counts]() -> BranchConsumer {
        auto count = std::make_shared<std::uint64_t>(0);
        counts->push_back(count);
        return
            [&sink, mtx, count](const json::JsonValue&, std::string_view raw) {
                std::lock_guard<std::mutex> lk(*mtx);
                sink.write(raw);
                sink.write("\n");
                ++*count;
            };
    };
    h.finalize = [out, counts]() {
        std::uint64_t total = 0;
        for (const auto& c : *counts) total += *c;
        out->events_matched = total;
    };
    return h;
}

// Drives the session's raw fold/export branches over the fused scan: parses
// each raw line once, then dispatches to every branch whose predicate matches.
// Sharing one parse across the branches is why they ride one fold instead of
// one fold each. Each slice owns its branches' consumers, so nothing is shared
// between workers; the unsliced fold holds none and only runs finalize.
class BranchDriverFold : public Fold {
   public:
    explicit BranchDriverFold(
        std::shared_ptr<std::vector<const BranchHooks*>> branches)
        : branches_(std::move(branches)) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool wants_raw() const override { return true; }

    std::unique_ptr<Fold> slice() const override {
        auto s = std::make_unique<BranchDriverFold>(branches_);
        s->consumers_.reserve(branches_->size());
        for (const auto* br : *branches_)
            s->consumers_.push_back(br->make_consumer());
        return s;
    }

    void step(const FoldBatch& b) override {
        for (std::string_view line : b.raw) {
            buf_.assign(line);
            auto res = parser_.parse(buf_);
            if (res.error()) continue;
            auto root = res.value_unsafe();
            if (!root.is_object()) continue;
            json::JsonValue jv(root);
            for (std::size_t i = 0; i < branches_->size(); ++i) {
                const BranchHooks* br = (*branches_)[i];
                if (!br->predicate || br->predicate->evaluate(jv))
                    consumers_[i](jv, line);
            }
        }
    }

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}
    // Each slice's consumers hold their own state and the branch reduces them
    // in finalize, so there is nothing to fold slice-to-slice here.
    void merge(Fold&) override {}

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        for (const auto* br : *branches_)
            if (br->finalize) br->finalize();
        co_return false;
    }

   private:
    std::shared_ptr<std::vector<const BranchHooks*>> branches_;
    std::vector<BranchConsumer> consumers_;
    simdjson::dom::parser parser_;
    std::string buf_;
};

coro::CoroTask<ExportStats> run_session(
    std::shared_ptr<ViewSessionState> state) {
    const ViewPlan& plan = *state->plan;

    // Serve each match-all aggregation branch from a rollup or the aggregation
    // tier with no scan; collect the rest. A match-all agg branch that must
    // scan carries its full plan (base + group_by/agg) so it can drive an
    // EngineAggFold.
    struct ScanBranch {
        const BranchHooks* br;
        std::shared_ptr<ViewPlan> agg_plan;  // set only for a match-all agg
        bool apply_query = false;  // branch EngineAggFold filters per event
    };
    std::vector<ScanBranch> scan_branches;
    scan_branches.reserve(state->branches.size());
    for (const auto& br : state->branches) {
        if (br.agg && !br.predicate) {
            auto full = br.agg->plan ? std::make_shared<ViewPlan>(*br.agg->plan)
                                     : std::make_shared<ViewPlan>(plan);
            if (!br.agg->plan) {
                full->group_by = br.agg->group_by;
                full->agg = br.agg->agg;
                if (br.agg->query) full->query = br.agg->query;
            }
            full->schema.reset();
            full->resolver.reset();
            ensure_schema(*full);
            // A per-branch predicate is not servable from a match-all rollup or
            // the tier; a partial wants the raw (unresolved) map. Both scan.
            if (!br.agg->apply_query && !br.agg->partial_out) {
                if (auto df = try_serve_rollup(*full)) {
                    *br.agg->out = apply_agg_post_ops(std::move(*df), *full);
                    continue;
                }
                dataframe::AggStatePtr served;
                if (co_await try_serve_aggregate_no_scan(*full, served)) {
                    *br.agg->out = apply_agg_post_ops(
                        finalize_engine_result(*served, *full), *full);
                    continue;
                }
            }
            scan_branches.push_back(
                {&br, std::move(full), br.agg->apply_query});
            continue;
        }
        scan_branches.push_back({&br, nullptr, false});
    }
    const bool has_factories = !state->fold_factories.empty();
    if (scan_branches.empty() && !has_factories) co_return ExportStats{};

    std::vector<const ScanBranch*> agg_b, raw_b;
    for (const auto& sb : scan_branches)
        (sb.agg_plan ? agg_b : raw_b).push_back(&sb);

    // A lone aggregation with no raw branches runs through the engine (the
    // rollup/tier fast paths already missed above, so this is the scan). A fold
    // factory (plugin) rides the shared fused scan and a partial wants the raw
    // map, so both disqualify it.
    if (agg_b.size() == 1 && raw_b.empty() && !has_factories &&
        !agg_b[0]->br->agg->partial_out) {
        auto agg_state = co_await build_engine_agg_state(*agg_b[0]->agg_plan);
        *agg_b[0]->br->agg->out = apply_agg_post_ops(
            finalize_engine_result(*agg_state, *agg_b[0]->agg_plan),
            *agg_b[0]->agg_plan);
        co_return ExportStats{};
    }

    // One fused scan drives every branch: an EngineAggFold per aggregation and
    // one BranchDriverFold parsing raw lines for the fold/export branches.
    ViewPlan scan_plan = plan;
    // A fold-only session may narrow the shared scan: with no other branch
    // there is nobody to starve, and the index can skip whole chunks.
    if (state->branches.empty() && state->proposed_prune) {
        if (scan_plan.query) {
            scan_plan.query = query::parse_or_throw(
                "(" + scan_plan.query->source() + ") and (" +
                state->proposed_prune->source() + ")");
        } else {
            scan_plan.query = *state->proposed_prune;
        }
    }
    if (auto mv = find_subsuming_view(plan)) scan_plan.files = std::move(*mv);
    ViewDefinition avdef = make_vdef(scan_plan, /*for_aggregation=*/true);
    // A Rank group key harvests the PR metadata during the scan, but make_vdef
    // dropped metadata for the base plan; if any branch wants Rank the fused
    // scan must keep it.
    const bool any_wants_rank =
        std::any_of(agg_b.begin(), agg_b.end(), [](const ScanBranch* sb) {
            return std::any_of(sb->agg_plan->group_by.begin(),
                               sb->agg_plan->group_by.end(),
                               [](const GroupKey& g) {
                                   return g.kind == GroupKey::Kind::Rank;
                               });
        });
    if (any_wants_rank) {
        avdef.include_metadata = true;
        avdef.emit_all_metadata = true;
    }
    dftracer::utils::StringIntern intern;

    std::vector<std::unique_ptr<EngineAggFold>> aggs;
    aggs.reserve(agg_b.size());
    for (const auto* sb : agg_b)
        aggs.push_back(std::make_unique<EngineAggFold>(*sb->agg_plan, intern,
                                                       sb->apply_query));

    std::unique_ptr<BranchDriverFold> raw_fold;
    if (!raw_b.empty()) {
        auto branches = std::make_shared<std::vector<const BranchHooks*>>();
        branches->reserve(raw_b.size());
        for (const auto* sb : raw_b) branches->push_back(sb->br);
        raw_fold = std::make_unique<BranchDriverFold>(std::move(branches));
    }

    // Externally-built folds (plugins), constructed with the shared intern so
    // ids agree and per-worker slices merge. A plugin needs metadata (its own
    // hash lookups), so keep it on the shared scan.
    std::vector<std::unique_ptr<Fold>> factory_folds;
    factory_folds.reserve(state->fold_factories.size());
    for (auto& ff : state->fold_factories)
        if (auto f = ff.make(intern)) factory_folds.push_back(std::move(f));
    if (has_factories) {
        avdef.include_metadata = true;
        avdef.emit_all_metadata = true;
    }

    std::vector<Fold*> fold_ptrs;
    fold_ptrs.reserve(aggs.size() + factory_folds.size() + 1);
    for (auto& a : aggs) fold_ptrs.push_back(a.get());
    if (raw_fold) fold_ptrs.push_back(raw_fold.get());
    for (auto& f : factory_folds) fold_ptrs.push_back(f.get());

    // A scan cap (viz-style early-out) applies only to a pure raw session; an
    // aggregation or plugin must see every event, and its limit is
    // post-aggregation.
    const std::uint64_t scan_cap =
        (agg_b.empty() && !has_factories) ? plan.limit : 0;
    ExportStats stats =
        co_await execute(lower_fused_folds(scan_plan, avdef, fold_ptrs), intern,
                         nullptr, scan_cap);

    for (std::size_t i = 0; i < agg_b.size(); ++i) {
        // A partial serializes the mergeable AggState for a distributed merge;
        // a collect finalizes it (rank/resolver relabel) into the DataFrame.
        if (agg_b[i]->br->agg->partial_out) {
            *agg_b[i]->br->agg->partial_out =
                encode_agg_partial(aggs[i]->state());
            continue;
        }
        apply_ranks(*agg_b[i]->agg_plan, aggs[i]->ranks());
        *agg_b[i]->br->agg->out = apply_agg_post_ops(
            finalize_engine_result(aggs[i]->state(), *agg_b[i]->agg_plan),
            *agg_b[i]->agg_plan);
    }
    // Factory folds published their results in Fold::finalize during the fuse;
    // let the caller pull them (while the folds are still alive here).
    for (auto& ff : state->fold_factories)
        if (ff.finalize) ff.finalize();
    co_return stats;
}

// export_counters with bounded memory: workers aggregate their share of chunks
// into in-memory maps, spilling to sorted runs when over budget; the runs are
// k-way merged and streamed to the sink, so peak memory stays bounded no matter
// the group cardinality. memory_budget == 0 keeps it purely in-memory.
coro::CoroTask<ExportStats> run_export_counters(const ViewPlan& plan,
                                                ExportSink& sink) {
    ensure_schema(plan);
    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/true);
    dftracer::utils::StringIntern intern;
    EngineAggFold agg(plan, intern);
    std::array<Fold*, 1> folds{&agg};
    ExportStats stats = co_await fuse(plan, vdef, folds, intern);
    apply_ranks(plan, agg.ranks());
    emit_counters_from_state(agg.state(), plan, sink);
    co_return stats;
}

// Rank-local counter aggregation for distributed runs: aggregate this view's
// (shard of) files in memory and serialize the groups into an opaque partial
// buffer. The transport (MPI etc.) lives in the caller; only bytes cross ranks.
coro::CoroTask<std::string> run_aggregate_partial(const ViewPlan& plan) {
    // Index-only fast path: the EVENT-only tier answers without reading traces
    // (a sharded reader merges straight from indexes). Everything else scans;
    // both emit the same mergeable AggState wire partial.
    const bool has_occupancy =
        std::any_of(plan.agg.begin(), plan.agg.end(),
                    [](const AggSpec& s) { return is_occupancy_op(s.op); });
    if (plan.phase != Phase::Counters && !plan.auto_numeric_metrics &&
        !has_occupancy) {
        dataframe::AggStatePtr tier;
        if (agg_tier_collect(plan, tier)) co_return encode_agg_partial(*tier);
    }
    auto state = co_await build_engine_agg_state(plan);
    co_return encode_agg_partial(*state);
}

// Merge partials and emit each combined group as a ph="C" counter event.
ExportStats merge_counters_partials(
    const ViewPlan& plan, const std::vector<std::string_view>& partials,
    ExportSink& sink) {
    const ViewPlan p = resolve_bucket_origin(plan);
    ensure_schema(p);
    dataframe::AggStatePtr state = decode_partials(partials);
    ExportStats st;
    if (state) {
        emit_counters_from_state(*state, p, sink);
        st.events_matched =
            static_cast<std::uint64_t>(dataframe::agg_num_groups(*state));
    }
    return st;
}

// Merge partials into a materialized DataFrame (distributed collect()).
dataframe::DataFrame merge_partials_to_table(
    const ViewPlan& plan, const std::vector<std::string_view>& partials) {
    const ViewPlan p = resolve_bucket_origin(plan);
    ensure_schema(p);
    dataframe::AggStatePtr state = decode_partials(partials);
    if (!state) state = empty_engine_state(p);
    return finalize_engine_result(*state, p);
}

}  // namespace dftracer::utils::trace::views::detail
