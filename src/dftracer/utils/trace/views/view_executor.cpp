// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/common/memory_budget.h>
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/coro/executor.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/when_all.h>
#include <dftracer/utils/dataframe/agg.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/fileio/gzip_line_writer.h>
#include <dftracer/utils/fileio/parallel/merge.h>
#include <dftracer/utils/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/dom_form.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/json/value.h>
#include <dftracer/utils/json/value_access.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/build/batch_builder.h>
#include <dftracer/utils/trace/index/build/chunk_indexer.h>
#include <dftracer/utils/trace/index/build/file_admission.h>
#include <dftracer/utils/trace/index/build/file_group.h>
#include <dftracer/utils/trace/index/build/index_fold_driver.h>
#include <dftracer/utils/trace/index/cache/mv_store.h>
#include <dftracer/utils/trace/index/cache/rollup_store.h>
#include <dftracer/utils/trace/index/extensions/bloom_fold.h>
#include <dftracer/utils/trace/index/extensions/kinds/time_bounds.h>
#include <dftracer/utils/trace/index/extensions/rowset_fold.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/trace/index/gzip/gzip_indexer.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/plan/prune.h>
#include <dftracer/utils/trace/index/plan/view_resolver.h>
#include <dftracer/utils/trace/index/source.h>
#include <dftracer/utils/trace/index/store/root_layout.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/internal/index_root.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/containment_fold.h>
#include <dftracer/utils/trace/views/coverage.h>
#include <dftracer/utils/trace/views/engine_agg_fold.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/index_rule_fold.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/pipeline.h>
#include <dftracer/utils/trace/views/stream_row_fold.h>
#include <dftracer/utils/trace/views/typed_collect_fold.h>
#include <dftracer/utils/trace/views/view_agg_engine.h>
#include <dftracer/utils/trace/views/view_agg_partials.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_counter_format.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/trace/views/view_system_collect.h>
#include <fcntl.h>
#include <simdjson.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <numeric>

// The View terminals: each lowers a ViewPlan onto the module pieces and runs a
// pruned parallel scan. The scan Source lives in view_scan (gather_units +
// make_vdef); the group/agg fold in view_aggregate; ph="C" emission in
// view_counter_format; out-of-core spill in view_spill. A terminal is therefore
// "gather units -> fan out a scan -> feed each batch to its own body (write /
// fold / spill / write_chunk / branch-route)". The bodies differ on purpose;
// only the shared scan setup is factored out.
namespace dftracer::utils::trace::views::detail {

// Every file is a clean first touch: it has an index path with no store yet,
// so a bootstrap can build it (and there is nothing to read instead).
static bool all_files_fresh(const ViewPlan& plan) {
    const dstd::String* checked = nullptr;
    for (const auto& f : plan.files) {
        if (f.index_path.empty()) return false;
        if (checked && *checked == f.index_path) continue;
        if (common::input_exists(
                dftracer::utils::trace::index::store_of(f.index_path)))
            return false;
        checked = &f.index_path;
    }
    return true;
}

bool export_bootstrap_eligible(const ViewPlan& plan) {
    if (plan.query || plan.time_range || plan.offset || plan.limit ||
        plan.phase != Phase::Any || !plan.select.empty() ||
        plan.files.empty() || plan.cancelled)
        return false;
    // The pass writes every line, or every line but the metadata records.
    const dstd::String& data = plan_record_schema(plan).data;
    if (!plan.all_records && !data.empty() &&
        data != dftracer::utils::trace::index::NO_METADATA)
        return false;
    return all_files_fresh(plan);
}

// Project one NDJSON event to the selected paths, emitted flat as a JSON
// object keyed by path - the raw-event form of project_columns. A path the
// record lacks reads it under `args` when `args_fallback`. `parser` is
// caller-owned: a thread_local parser is unsafe here because the export emit
// runs on pooled coroutine threads.
static dstd::String project_event(simdjson::dom::parser& parser,
                                  dstd::StringView ev,
                                  const dstd::Vector<dstd::String>& select,
                                  bool args_fallback) {
    simdjson::padded_string padded(ev);
    simdjson::dom::element doc;
    if (parser.parse(padded).get(doc) &&
        json::parse_refused(parser, ev).get(doc))
        return dstd::String(ev);
    if (!doc.is_object()) return dstd::String(ev);
    const json::JsonValue root(doc);
    dstd::String out = "{";
    bool first = true;
    for (const auto& name : select) {
        json::JsonValue val = root.at(name);
        if (!val.exists() && args_fallback && !common::has_args_prefix(name))
            val = root.at("args." + name);
        if (!val.exists()) continue;
        if (!first) out += ',';
        first = false;
        out += '"';
        json::append_json_escaped(out, name);
        out += "\":";
        json::append_canonical_json(out, val.raw());
    }
    out += '}';
    return out;
}

bool collect_bootstrap_eligible(const ViewPlan& plan) {
    if (plan.files.empty() || plan.time_range) return false;
    if (plan.query && !query_evaluable_by_fold(*plan.query)) return false;
    // The pass applies plan.query, not the schema's data rowset filter that
    // effective_query adds.
    const dstd::String& data = plan_record_schema(plan).data;
    if (!plan.all_records && !data.empty() &&
        data != dftracer::utils::trace::index::NO_METADATA)
        return false;
    // A partial source (chunk-stats fast path) answers only from an existing
    // index, so it contributes nothing once all_files_fresh holds - no need to
    // exclude it here.
    return all_files_fresh(plan);
}

namespace {

// Builds the gzip index artifacts of `file` with `visitors` in `scope` into
// `*arts` and sets `*ok` when it did. A named coroutine with by-value
// parameters rather than a capturing lambda that holds the visitor list on its
// frame (CP.51; see the GCC frame notes in the coroutine caveats).
//
// The descriptor is the one open_bootstrap_source hashed.
coro::CoroTask<void> build_bootstrap_artifacts(
    coro::Scope& scope, dstd::String file,
    dftracer::utils::trace::index::gzip::CheckpointIndexer::VisitorList
        visitors,
    dftracer::utils::trace::index::gzip::GzipBuildArtifacts* arts, bool* ok,
    std::size_t ways, dstd::SharedPtr<dstd::File> source) {
    const int fd = source->native_handle();
    if (fd < 0) co_return;
    auto built = co_await dftracer::utils::trace::index::gzip::
        build_gzip_index_artifacts(
            fd, file,
            dftracer::utils::trace::index::gzip::CheckpointIndexer::
                DEFAULT_CHECKPOINT_SIZE,
            visitors, &scope, nullptr, ways);
    if (built) {
        *arts = std::move(*built);
        *ok = true;
    }
}

// The file's descriptor, its size and its hash: the build needs them before
// the folds run, and the artifacts read the same descriptor.
struct BootstrapSource {
    dstd::SharedPtr<dstd::File> fd;
    std::uint64_t size = 0;
    std::uint64_t hash = 0;
    bool ok = false;
};

BootstrapSource open_bootstrap_source(dstd::StringView file,
                                      dstd::SharedPtr<dstd::File> shared) {
    // A region, not an async op: a task that resumes on another worker frees
    // its memory away from the owning heap, and the view's peak grows.
    coro::BlockingRegion blocking;
    BootstrapSource out;
    if (!shared) {
        auto opened = dstd::File::open(dstd::PathView(file), {});
        if (!opened) return out;
        shared = dstd::make_shared<dstd::File>(std::move(*opened));
    }
    const auto size = shared->size();
    if (!size) return out;
    out.fd = std::move(shared);
    out.size = *size;
    out.hash = fileio::calculate_file_hash(*out.fd, out.size);
    out.ok = true;
    return out;
}

struct BootstrapRoot {
    dstd::String index_path;
    dstd::Vector<std::size_t> members;
    dstd::Optional<dftracer::utils::trace::index::Index> index;
    /// The index before this bootstrap; empty when it could not be read.
    dstd::Optional<dftracer::utils::trace::index::Snapshot> before;
    dstd::String index_error;
};

// Every index root of a bootstrap is held read-write until the last commit.
struct BootstrapStores {
    BootstrapStores() = default;
    BootstrapStores(const BootstrapStores&) = delete;
    BootstrapStores& operator=(const BootstrapStores&) = delete;

    dstd::Map<trace::internal::IndexRoot*,
              dstd::SharedPtr<trace::internal::IndexRoot>>
        held;
    dstd::Vector<trace::internal::RootPermit> building;
    dstd::Vector<dstd::UniquePtr<BootstrapRoot>> roots;
};

// A build holds the root read-write until its last commit. A scan that starts
// meanwhile would take the root for a built index, so the build is serialized
// with the metadata reads of the same root.
coro::CoroTask<void> open_bootstrap_stores(const ViewPlan& plan,
                                           BootstrapStores& out) {
    const std::size_t n = plan.files.size();
    dstd::Map<dstd::String, dstd::Vector<std::size_t>> by_index;
    for (std::size_t i = 0; i < n; ++i)
        by_index[plan.files[i].index_path].push_back(i);
    for (const auto& entry : by_index) {
        auto root = trace::internal::index_root(entry.first);
        out.held.emplace(root.get(), std::move(root));
    }
    for (const auto& entry : out.held) {
        co_await entry.second->gate.acquire(
            trace::internal::IndexRoot::ALL_PERMITS);
        out.building.emplace_back(*entry.second,
                                  trace::internal::IndexRoot::ALL_PERMITS);
    }
    out.roots.reserve(by_index.size());
    for (const auto& [index_path, members] : by_index) {
        auto store = dstd::make_unique<BootstrapRoot>();
        store->index_path = index_path;
        store->members.assign(members.begin(), members.end());
        auto opened = dftracer::utils::trace::index::Index::open(
            dftracer::utils::trace::index::normalize_index_root(index_path));
        if (opened) {
            store->index.emplace(std::move(*opened));
            auto taken = store->index->snapshot();
            if (taken) store->before.emplace(std::move(*taken));
        } else
            store->index_error = dstd::String(opened.error().message());
        out.roots.push_back(std::move(store));
    }
}

// The build of `file`, which must exist before its folds run so one that
// spills writes its runs into the file it parses. `failure` is set when the
// index cannot hold it.
struct BootstrapBuild {
    dstd::Optional<dftracer::utils::trace::index::FileBuild> build;
    dstd::String failure;
    bool fresh = false;
};

BootstrapBuild open_bootstrap_build(BootstrapRoot& root, dstd::StringView file,
                                    std::uint64_t mtime,
                                    const BootstrapSource& source) {
    BootstrapBuild out;
    if (!root.index) {
        out.failure = root.index_error;
        return out;
    }
    const dstd::String logical = fileio::get_logical_path(file);
    out.fresh = root.before && !root.before->file(logical).has_value();
    auto started = root.index->build(
        logical, trace::index::SourceStat{mtime, source.size, source.hash});
    if (started)
        out.build.emplace(std::move(*started));
    else
        out.failure = dstd::String(started.error().message());
    return out;
}

// Puts the file's index, the tier its runs staged included, into its build.
dstd::Result<void> fill_bootstrap_file(
    dftracer::utils::trace::index::FileBuild& build,
    const dftracer::utils::trace::index::gzip::GzipBuildArtifacts& arts,
    const dftracer::utils::trace::index::RecordSchema& schema,
    dftracer::utils::trace::index::extensions::BloomFold& bloom,
    dftracer::utils::trace::index::extensions::RowSetFold& rowsets,
    bool fresh) {
    trace::index::gzip::persist_gzip_index_artifacts(build, arts);
    // The new members renumber the chunks the old copies describe. A fresh
    // file has none, so it skips the check, which costs a store snapshot per
    // file, and the record per namespace that hides older copies.
    if (fresh)
        build.keep_older_copies();
    else
        trace::index::clear_chunk_copies(build);
    trace::index::put_profile(build, schema.id);
    build.finish(trace::index::store::IndexExtension::Profile,
                 schema.params_hash());
    auto bloom_written = bloom.write(build);
    if (!bloom_written) return bloom_written.error();
    return rowsets.write(build);
}

// Publishes the files of `group` in one record. The lines are already out in
// the export, so a failed index write is logged and the export stays whole.
// Returns false when a file did not publish.
coro::CoroTask<bool> commit_bootstrap(
    dftracer::utils::trace::index::build::FileGroup& group) {
    auto result = co_await group.commit();
    for (const auto& failure : result.failures)
        DFTRACER_UTILS_LOG_ERROR("bootstrap index write failed for {}: {}",
                                 failure.path, failure.message);
    co_return result.failures.empty();
}

// An IndexVisitor that streams a member's plaintext lines straight to the
// export sink, without the metadata records when `drop_metadata`. Carries the
// partial last line across on_chunk calls so an event straddling an inflate
// boundary is written whole.
class SinkWriterVisitor
    : public dftracer::utils::trace::index::build::IndexVisitor {
   public:
    SinkWriterVisitor(ExportSink& sink, bool drop_metadata)
        : sink_(&sink), drop_metadata_(drop_metadata) {}
    void begin(std::size_t) override {}
    coro::CoroTask<dstd::Result<void>> on_checkpoint(std::size_t) override {
        co_return dstd::Result<void>{};
    }
    coro::CoroTask<dstd::Result<void>> on_chunk(const char* data,
                                                std::size_t len,
                                                std::size_t) override {
        buf_.append(data, len);
        const std::size_t last_nl = buf_.rfind('\n');
        if (last_nl == dstd::String::npos) co_return dstd::Result<void>{};
        if (!drop_metadata_) {
            sink_->write(dstd::StringView(buf_.data(), last_nl + 1));
        } else {
            kept_.clear();
            std::size_t pos = 0;
            while (pos <= last_nl) {
                const std::size_t nl = buf_.find('\n', pos);
                const dstd::StringView line(buf_.data() + pos, nl - pos);
                if (phase_of(line) != trace::RecordPhase::Metadata) {
                    kept_.append(line);
                    kept_.push_back('\n');
                }
                pos = nl + 1;
            }
            if (!kept_.empty()) sink_->write(kept_);
        }
        buf_.erase(0, last_nl + 1);
        co_return dstd::Result<void>{};
    }

   private:
    trace::RecordPhase phase_of(dstd::StringView line) {
        auto parsed = doc_.parse(line);
        return parsed ? trace::event_phase(*parsed)
                      : trace::RecordPhase::Unknown;
    }

    ExportSink* sink_;
    bool drop_metadata_;
    dstd::String buf_;
    dstd::String kept_;
    json::Document doc_;
};

// First-touch full export: for an unindexed, unfiltered whole-trace dump, one
// raw-gzip pass per file streams every line to the sink AND builds the index.
// Returns false unless every file is a clean first touch and the export is a
// plain full dump (no filter, window, pagination, or phase/metadata selection),
// so anything else falls through to the streaming scanner path.
coro::CoroTask<bool> try_export_bootstrap(const ViewPlan& plan,
                                          ExportSink& sink) {
    if (!export_bootstrap_eligible(plan)) co_return false;

    BootstrapStores stores;
    co_await open_bootstrap_stores(plan, stores);
    dstd::HashMap<dstd::String, std::size_t> slot_of;
    for (std::size_t r = 0; r < stores.roots.size(); ++r)
        slot_of.emplace(stores.roots[r]->index_path, r);

    dstd::Vector<dftracer::utils::trace::index::build::FileGroup> pending(
        stores.roots.size());
    for (std::size_t i = 0; i < plan.files.size(); ++i) {
        const auto& f = plan.files[i];
        const std::size_t slot = slot_of.find(f.index_path)->second;
        BootstrapRoot& root = *stores.roots[slot];
        const auto source = open_bootstrap_source(f.file_path, f.fd);
        if (!source.ok) {
            for (auto& group : pending) co_await commit_bootstrap(group);
            co_return false;
        }
        auto started = open_bootstrap_build(
            root, f.file_path,
            static_cast<std::uint64_t>(
                fileio::get_file_modification_time(f.file_path)),
            source);
        dftracer::utils::dstd::StringIntern intern;
        const auto& schema = plan_record_schema(plan);
        dftracer::utils::trace::index::extensions::BloomFold bloom(
            intern,
            dftracer::utils::trace::index::build::for_schema({}, schema));
        dftracer::utils::trace::index::extensions::RowSetFold rowsets(
            intern, dftracer::utils::trace::index::indexed_rowsets(schema));
        std::array<Fold*, 2> folds{&bloom, &rowsets};
        dftracer::utils::trace::index::build::IndexFoldDriver driver(
            intern, folds, dstd::String(dstd::StringView(f.file_path)),
            dstd::String(dstd::StringView(f.index_path)));
        driver.set_record_schema(schema);
        // Path-decoded records have no phase: every one is data.
        SinkWriterVisitor writer(sink, !plan.all_records &&
                                           !schema.data.empty() &&
                                           !plan_by_path(plan));

        trace::index::gzip::GzipBuildArtifacts arts;
        bool ok = false;
        trace::index::gzip::CheckpointIndexer::VisitorList visitors;
        visitors.emplace_back(driver);
        visitors.emplace_back(writer);
        co_await coro::with_scope([&](coro::Scope& scope) {
            return build_bootstrap_artifacts(scope, f.file_path,
                                             std::move(visitors), &arts, &ok,
                                             std::size_t{0}, source.fd);
        });
        if (!ok) {
            for (auto& group : pending) co_await commit_bootstrap(group);
            co_return false;
        }
        driver.seal();
        if (!started.build) {
            DFTRACER_UTILS_LOG_ERROR("bootstrap index write failed for {}: {}",
                                     f.file_path, started.failure);
            continue;
        }
        const auto filled = fill_bootstrap_file(*started.build, arts, schema,
                                                bloom, rowsets, started.fresh);
        if (!filled) {
            DFTRACER_UTILS_LOG_ERROR("bootstrap index write failed for {}: {}",
                                     f.file_path, filled.error().message());
            continue;
        }
        const dstd::String staging =
            co_await pending[slot].add(std::move(*started.build), 0,
                                       fileio::get_logical_path(f.file_path));
        if (!staging.empty()) {
            DFTRACER_UTILS_LOG_ERROR("bootstrap index write failed for {}: {}",
                                     f.file_path, staging);
            continue;
        }
        if (pending[slot].full()) co_await commit_bootstrap(pending[slot]);
    }
    for (auto& group : pending) co_await commit_bootstrap(group);
    co_return true;
}

}  // namespace

namespace {

// Re-point a read at a materialized filtered-trace view that subsumes it, if
// one exists. The predicate/window/phase are kept so the scan re-applies them
// on the (smaller) MV rows - the compensation that makes reuse correct even
// when the MV is broader than the query.
dstd::Result<ViewPlan> redirect_reads(const ViewPlan& plan,
                                      bool* redirected = nullptr) {
    if (auto resolved = resolve_record_schema(plan); !resolved)
        return resolved.error();
    ViewPlan eff = plan;
    if (auto mv =
            dftracer::utils::trace::index::cache::find_subsuming_view(plan)) {
        eff.files = std::move(*mv);
        if (redirected) *redirected = true;
    }
    return eff;
}

}  // namespace

coro::CoroTask<dstd::Result<ExportStats>> run_export(const ViewPlan& base_plan,
                                                     ExportSink& sink) {
    bool from_mv = false;
    auto redirected = redirect_reads(base_plan, &from_mv);
    if (!redirected) co_return redirected.error();
    const ViewPlan plan = std::move(*redirected);
    // First touch: one raw-gzip pass dumps the trace and builds its index.
    // Named, not awaited in the condition; see try_serve_aggregate_no_scan.
    const bool bootstrapped = co_await try_export_bootstrap(plan, sink);
    if (bootstrapped) co_return ExportStats{};

    auto made = make_vdef(plan, /*for_aggregation=*/false);
    if (!made) co_return made.error();
    ViewDefinition vdef = std::move(*made);
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
    dftracer::utils::dstd::StringIntern intern;
    auto selected = select_index_folds(plan, vdef, intern);
    if (!selected) co_return selected.error();
    auto folds = std::move(*selected);
    dstd::Vector<Fold*> fold_ptrs;
    for (auto& f : folds) fold_ptrs.push_back(f.get());
    // Stream matched events; --select projects each one to the chosen fields
    // (SQL-style), otherwise they are written verbatim. The scan driver owns
    // the gather + fan-out. The projection parser is used under sink_mtx.
    dftracer::utils::json::RecordParser proj_parser;
    auto scanned = co_await for_each_scanned_batch(
        plan, vdef, /*slot_count=*/0, cap,
        [&](std::size_t, const dstd::Vector<dstd::StringView>& events) {
            std::lock_guard<std::mutex> lk(sink_mtx);
            for (const auto& ev : events) {
                if (seen++ < plan.offset) continue;
                if (plan.limit && written >= plan.limit) break;
                if (plan.select.empty()) {
                    sink.write(ev);
                } else {
                    sink.write(project_event(proj_parser, ev, plan.select,
                                             vdef.args_fallback));
                }
                sink.write("\n");
                ++written;
            }
        },
        fold_ptrs, &intern);
    if (!scanned) co_return scanned.error();
    ExportStats stats = *scanned;
    stats.served_from_mv = from_mv;
    co_return stats;
}

coro::CoroTask<dstd::Result<ExportStats>> run_export_trace(
    const ViewPlan& plan, const TraceWriteOptions& opts) {
    // Member size defaults to the checkpoint granularity (a member == a chunk).
    constexpr std::size_t DEFAULT_FLUSH_BYTES =
        common::constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    constexpr std::size_t BUFFER_HEADROOM_BYTES = 1 * 1024 * 1024;

    if (opts.build_index)
        co_return co_await run_export_trace_indexed(plan, opts);

    auto made = make_vdef(plan, /*for_aggregation=*/false);
    if (!made) co_return made.error();
    ViewDefinition vdef = std::move(*made);

    std::uint64_t skipped = 0;
    auto gathered = co_await gather_units(plan, vdef, skipped);
    if (!gathered) co_return gathered.error();
    auto units = std::move(*gathered);

    const std::size_t baseline = std::max<std::size_t>(1, opts.worker_count);
    const auto sizing =
        fileio::parallel::make_writer_for_path(
            {dstd::String(opts.output_path), baseline,
             opts.member_size ? opts.member_size : DEFAULT_FLUSH_BYTES,
             BUFFER_HEADROOM_BYTES, opts.compress})
            .sizing;
    const std::size_t worker_count = sizing.worker_count;

    fileio::GzipWriterOptions wopts;
    wopts.member_size = sizing.flush_threshold;
    wopts.level = opts.level;
    wopts.workers = worker_count;
    wopts.compress = opts.compress;
    wopts.ordered = false;
    wopts.memory_budget = plan.memory_budget;
    auto opened = co_await fileio::GzipLineWriter::open(
        dstd::String(opts.output_path), std::move(wopts));
    if (!opened) {
        co_return common::make_error<"export_trace failed writing {}">(
            common::ErrorCode::Io, opts.output_path);
    }
    auto writer = std::move(*opened);

    dstd::Vector<ScanCounts> counts_v(worker_count);
    dstd::Vector<dstd::Optional<dstd::Error>> scan_errors(worker_count);
    bool ok = true;

    co_await coro::with_scope([&](coro::Scope& scope) -> coro::CoroTask<void> {
        for (std::size_t w = 0; w < worker_count; ++w) {
            scope.spawn([&, w]() -> coro::CoroTask<void> {
                auto prod = writer.producer();
                if (!prod) {
                    ok = false;
                    co_return;
                }
                bool stop = false;
                for (std::size_t i = w; !stop && i < units.size();
                     i += worker_count) {
                    if (is_cancelled(plan)) break;
                    ViewScannerInput sin =
                        make_scanner_input(units[i], vdef, vdef.query);
                    ViewScannerUtility scanner;
                    auto gen = scanner(sin);
                    for (;;) {
                        auto next = co_await gen.next();
                        if (!next) break;
                        if (!*next) {
                            scan_errors[w] = next->error();
                            stop = true;
                            break;
                        }
                        auto& b = *next;
                        if (is_cancelled(plan)) {
                            stop = true;
                            break;
                        }
                        counts_v[w].add(*b);
                        std::size_t total = 0;
                        for (const auto& ev : b->events) total += ev.size() + 1;
                        if (total == 0) continue;
                        const auto append_with_ok = co_await prod->append_with(
                            total, [&](char* dst, std::size_t) {
                                char* p = dst;
                                for (const auto& ev : b->events) {
                                    std::memcpy(p, ev.data(), ev.size());
                                    p += ev.size();
                                    *p++ = '\n';
                                }
                                return static_cast<std::size_t>(p - dst);
                            });
                        if (!append_with_ok) {
                            ok = false;
                            co_return;
                        }
                    }
                }
                const auto flush_ok = co_await prod->flush();
                if (!flush_ok) ok = false;
                co_return;
            });
        }
        co_return;
    });

    const auto close_ok = co_await writer.close();
    if (!close_ok) ok = false;

    for (auto& e : scan_errors)
        if (e) co_return *e;
    if (!ok) {
        co_return common::make_error<"export_trace failed writing {}">(
            common::ErrorCode::Io, opts.output_path);
    }

    ExportStats st;
    st.chunks_skipped = skipped;
    st.chunks_scanned = units.size();
    for (const auto& c : counts_v) c.add_to(st);
    co_return st;
}

coro::CoroTask<dstd::Result<ExportStats>> run_folds(
    const ViewPlan& plan, dstd::Span<Fold* const> folds,
    dftracer::utils::dstd::StringIntern& intern, DynamicPrune* dyn_prune,
    coro::AsyncSemaphore* gate) {
    auto made = make_vdef(plan, /*for_aggregation=*/false);
    if (!made) co_return made.error();
    ViewDefinition vdef = std::move(*made);
    auto selected = select_catalog_fold(plan, vdef, intern);
    if (!selected) co_return selected.error();
    auto catalog = std::move(*selected);
    dstd::Vector<Fold*> all(folds.begin(), folds.end());
    if (catalog) all.push_back(catalog.get());
    co_return co_await fuse(plan, vdef, all, intern,
                            /*covered=*/nullptr,
                            /*limit=*/0, dyn_prune, gate);
}

#if DFTRACER_UTILS_TEST_PROBES
BootstrapCounters& bootstrap_counters() {
    static BootstrapCounters counters;
    return counters;
}
#endif

namespace {

// First-touch fast path: for a plan whose files have no index yet, one raw-gzip
// pass per file both aggregates it and builds its full index (members + bloom +
// hash) as a byproduct, so the query never triggers a separate eager index
// build. Returns false (caller falls through to the indexed path) unless every
// file is a clean first touch and nothing materialized can answer instead.

// `state` is set with `keep_state`. `ok` is false when the build failed.
struct BootstrapFile {
    bool ok = false;
    dataframe::AggStatePtr state;
};

struct BootstrapBlock {
    BootstrapRoot* root;
    std::size_t begin;
    std::size_t end;
};

struct BootstrapRun {
    const ViewPlan* plan;
    BootstrapStores* stores;
    dstd::Vector<BootstrapBlock> blocks;
    // The merged state of each block's files, in file order.
    dstd::Vector<dataframe::AggStatePtr> states;
    dftracer::utils::trace::index::build::FileAdmission* admission = nullptr;
    bool keep_state;
    dstd::Optional<AggQuery> rule_query;
    std::atomic<std::size_t> next_block{0};
    std::atomic<bool> failed{false};
    // Shared by every file's fold driver, so a file starts with the capacity
    // the files before it grew.
    dftracer::utils::trace::index::build::FoldBufferPool buffers;
};

coro::CoroTask<dstd::Result<BootstrapFile>> bootstrap_file(
    BootstrapRun* run, ViewFile f, BootstrapRoot* root,
    dftracer::utils::trace::index::build::FileGroup* group) {
    const ViewPlan* plan = run->plan;
    BootstrapFile out;
    std::uint64_t compressed = 0;
    if (f.uncompressed_size == 0) {
        // A file never indexed has no size in the index.
        coro::BlockingRegion blocking;
        if (!f.fd) {
            auto opened = dstd::File::open(dstd::PathView(f.file_path), {});
            if (opened)
                f.fd = dstd::make_shared<dstd::File>(std::move(*opened));
        }
        if (f.fd) {
            const auto size = f.fd->size();
            if (size) compressed = *size;
        }
    }
    const std::uint64_t plaintext =
        dftracer::utils::trace::index::build::FileAdmission::plaintext_estimate(
            f.uncompressed_size, compressed);
    auto admitted = co_await run->admission->admit(plaintext);
    if (!admitted) co_return admitted.error();
    auto ticket = std::move(*admitted);
#if DFTRACER_UTILS_TEST_PROBES
    auto& counters = bootstrap_counters();
    const auto now =
        counters.in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
    auto seen = counters.max_in_flight.load(std::memory_order_relaxed);
    while (now > seen && !counters.max_in_flight.compare_exchange_weak(
                             seen, now, std::memory_order_relaxed)) {
    }
    struct Leave {
        BootstrapCounters& c;
        ~Leave() { c.in_flight.fetch_sub(1, std::memory_order_relaxed); }
    } leave{counters};
#endif
    dftracer::utils::dstd::StringIntern intern;
    const auto& schema = plan_record_schema(*plan);
    const auto source = open_bootstrap_source(f.file_path, f.fd);
    if (!source.ok) co_return out;
    auto started = open_bootstrap_build(
        *root, f.file_path,
        static_cast<std::uint64_t>(
            fileio::get_file_modification_time(f.file_path)),
        source);
    if (!started.build) {
        DFTRACER_UTILS_LOG_ERROR("bootstrap index write failed for {}: {}",
                                 f.file_path, started.failure);
        co_return out;
    }
    dstd::UniquePtr<IndexRuleFold> rule_agg;
    dstd::UniquePtr<EngineAggFold> frame_agg;
    if (run->rule_query) {
        auto made = IndexRuleFold::make(*plan, *run->rule_query, intern,
                                        /*apply_query=*/true);
        if (!made) co_return made.error();
        rule_agg = std::move(*made);
    } else {
        auto spec = make_agg_input_spec(*plan);
        if (!spec) co_return spec.error();
        frame_agg = dstd::make_unique<EngineAggFold>(
            *plan, intern, std::move(*spec), /*apply_query=*/true);
    }
    Fold* agg = rule_agg ? static_cast<Fold*>(rule_agg.get())
                         : static_cast<Fold*>(frame_agg.get());
    dftracer::utils::trace::index::extensions::BloomFold bloom(
        intern, dftracer::utils::trace::index::build::for_schema({}, schema));
    dftracer::utils::trace::index::extensions::RowSetFold rowsets(
        intern, dftracer::utils::trace::index::indexed_rowsets(schema));
    if (ticket.elastic_bytes() != memory::NO_SPILL_BUDGET)
        bloom.enable_spill(ticket.elastic_bytes(), *started.build);
    std::array<Fold*, 3> folds{agg, &bloom, &rowsets};
    dftracer::utils::trace::index::build::IndexFoldDriver driver(
        intern, folds, dstd::String(dstd::StringView(f.file_path)),
        dstd::String(dstd::StringView(f.index_path)),
        extra_capture_fields(*plan));
    driver.set_record_schema(schema);
    driver.share_buffers(run->buffers);
    driver.set_parse_gate(run->admission->gate());

    trace::index::gzip::GzipBuildArtifacts arts;
    bool built = false;
    trace::index::gzip::CheckpointIndexer::VisitorList visitors;
    visitors.emplace_back(driver);
    co_await coro::with_scope([&](coro::Scope& scope) {
        return build_bootstrap_artifacts(
            scope, f.file_path, std::move(visitors), &arts, &built,
            run->admission->inflate_ways(), source.fd);
    });
    if (!built) co_return out;
    ExportStats read;
    read.chunks_scanned = arts.members.size();
    read.events_scanned = arts.total_lines;
    counted(*plan, read);
    driver.seal();
    const auto filled = fill_bootstrap_file(*started.build, arts, schema, bloom,
                                            rowsets, started.fresh);
    if (!filled) {
        DFTRACER_UTILS_LOG_ERROR("bootstrap index write failed for {}: {}",
                                 f.file_path, filled.error().message());
        co_return out;
    }
    const dstd::String failure = co_await group->add(
        std::move(*started.build), 0, fileio::get_logical_path(f.file_path));
    if (!failure.empty()) {
        DFTRACER_UTILS_LOG_ERROR("bootstrap index write failed for {}: {}",
                                 f.file_path, failure);
        co_return out;
    }
    if (run->keep_state) {
        if (rule_agg) {
            CoverageSet none;
            co_await rule_agg->finalize(none);
            out.state = rule_agg->take_state();
        } else {
            out.state = frame_agg->take_state();
        }
    }
    out.ok = true;
    co_return out;
}

// False when a file failed. A worker that fails stops the others from taking
// more files.
coro::CoroTask<dstd::Result<bool>> bootstrap_worker(BootstrapRun* run) {
    while (!run->failed.load(std::memory_order_relaxed)) {
        const std::size_t b =
            run->next_block.fetch_add(1, std::memory_order_relaxed);
        if (b >= run->blocks.size()) break;
        const BootstrapBlock block = run->blocks[b];
        auto& merged = run->states[b];
        dftracer::utils::trace::index::build::FileGroup group;
        // The files finished before a failure still publish.
        dstd::Optional<dstd::Error> error;
        bool stop = false;
        for (std::size_t k = block.begin; k < block.end && !stop; ++k) {
            if (run->failed.load(std::memory_order_relaxed)) break;
            const std::size_t i = block.root->members[k];
            auto filed = co_await bootstrap_file(run, run->plan->files[i],
                                                 block.root, &group);
            if (!filed) {
                error = filed.error();
                break;
            }
            BootstrapFile file = std::move(*filed);
            if (!file.ok) {
                run->failed.store(true, std::memory_order_relaxed);
                break;
            }
            if (group.full()) {
                const bool published = co_await commit_bootstrap(group);
                stop = !published;
            }
            if (!file.state) continue;
            if (!merged)
                merged = std::move(file.state);
            else
                dataframe::agg_merge(*merged, *file.state);
        }
        const bool published = co_await commit_bootstrap(group);
        if (error) {
            run->failed.store(true, std::memory_order_relaxed);
            co_return *error;
        }
        if (stop || !published || run->failed.load(std::memory_order_relaxed)) {
            run->failed.store(true, std::memory_order_relaxed);
            co_return false;
        }
    }
    co_return !run->failed.load(std::memory_order_relaxed);
}

coro::CoroTask<dstd::Result<void>> bootstrap_workers(coro::Scope& scope,
                                                     BootstrapRun* run,
                                                     std::size_t workers) {
    dstd::Vector<coro::SpawnFuture<dstd::Result<bool>>> done;
    done.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w)
        done.push_back(
            scope.spawn_future([run]() { return bootstrap_worker(run); }));
    auto results = co_await coro::when_all(std::move(done));
    for (auto& r : results)
        if (!r.has_value()) co_return r.error();
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<bool>> try_collect_bootstrap(
    const ViewPlan& plan, dataframe::AggStatePtr& merged) {
    // A time window still needs the indexed path (the bootstrap has no ts
    // pruning), and a predicate is only applied here when every field it names
    // is POD-evaluable; otherwise defer to the scan, which filters properly.
    if (!collect_bootstrap_eligible(plan)) co_return false;

    // A transform on a file, host or rank key must resolve the key to its
    // value before transforming, but the row sets are still being built during
    // this pass. Build the index in this pass, then re-aggregate through the
    // normal engine scan (which reads the now-complete row sets).
    const bool resolved_transform = std::any_of(
        plan.group_by.begin(), plan.group_by.end(), [](const GroupKey& g) {
            return g.transform != GroupKey::Transform::None &&
                   (g.kind == GroupKey::Kind::FilePath ||
                    g.kind == GroupKey::Kind::FileName ||
                    g.kind == GroupKey::Kind::HostName ||
                    g.kind == GroupKey::Kind::Rank);
        });

    BootstrapStores stores;
    co_await open_bootstrap_stores(plan, stores);

    const std::size_t workers =
        std::max<std::size_t>(1, common::available_parallelism());
    BootstrapRun run;
    run.plan = &plan;
    run.stores = &stores;
    run.keep_state = !resolved_transform;
    if (run.keep_state) {
        auto rule = trace_fold_query(plan);
        if (!rule) co_return rule.error();
        run.rule_query = std::move(*rule);
    }
    for (const auto& store : stores.roots) {
        dstd::Vector<std::uint64_t> costs;
        costs.reserve(store->members.size());
        for (std::size_t i : store->members)
            costs.push_back(plan.files[i].uncompressed_size);
        const auto starts = dftracer::utils::trace::index::build::cut_blocks(
            store->members.size(), costs, workers);
        for (std::size_t b = 0; b + 1 < starts.size(); ++b)
            run.blocks.push_back({store.get(), starts[b], starts[b + 1]});
    }
    run.states.resize(run.blocks.size());
    const std::size_t at_once =
        std::max<std::size_t>(1, std::min(run.blocks.size(), workers));
    const std::uint64_t budget =
        plan.memory_budget == memory::NO_SPILL_BUDGET
            ? memory::NO_SPILL_BUDGET
            : memory::resolve_spill_budget(plan.memory_budget);
    const auto sinks =
        dftracer::utils::trace::index::build::FileAdmission::plan_sinks(
            budget, workers,
            dftracer::utils::trace::index::build::g_max_sink_bytes.load(
                std::memory_order_relaxed));
    const std::size_t active = std::min(at_once, sinks.workers);
    dftracer::utils::trace::index::build::FileAdmission admission(
        budget, sinks.workers,
        budget == memory::NO_SPILL_BUDGET ? 0 : sinks.sink_bytes * active,
        plan.files.size());
    run.admission = &admission;
    auto worked = co_await coro::with_scope([&](coro::Scope& scope) {
        return bootstrap_workers(scope, &run, active);
    });
    if (!worked) co_return worked.error();
    if (run.failed.load(std::memory_order_relaxed)) co_return false;
    if (!resolved_transform) {
        // Blocks are runs of consecutive files, so their first files give
        // the file order.
        dstd::Vector<std::size_t> order(run.blocks.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        const auto first_file = [&run](std::size_t b) {
            return run.blocks[b].root->members[run.blocks[b].begin];
        };
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) {
                      return first_file(a) < first_file(b);
                  });
        for (const std::size_t b : order) {
            if (!run.states[b]) continue;
            if (!merged)
                merged = std::move(run.states[b]);
            else
                dataframe::agg_merge(*merged, *run.states[b]);
        }
    }
    // The index now carries the row sets; a normal engine scan resolves
    // the transformed key correctly. Reset the resolver the per-file folds
    // cached before they existed so the re-scan rebuilds it from the fresh
    // index.
    if (resolved_transform) {
        // The rescan reads the metadata of the files, which waits for the lock.
        stores.building.clear();
        ViewPlan rescan = plan;
        rescan.resolver.reset();
        auto rebuilt = co_await build_engine_agg_state(rescan);
        if (!rebuilt) co_return rebuilt.error();
        merged = std::move(*rebuilt);
    }
    co_return true;
}

// Releases an MV directory lock (see lock_view_dir) on scope exit, so the
// single-flight lock is dropped on every path, including an error from the
// build.
class ViewDirLock {
   public:
    explicit ViewDirLock(dstd::File file) noexcept : file_(std::move(file)) {}
    ~ViewDirLock() {
        dftracer::utils::trace::index::cache::unlock_view_dir(file_);
    }
    ViewDirLock(const ViewDirLock&) = delete;
    ViewDirLock& operator=(const ViewDirLock&) = delete;

   private:
    dstd::File file_;
};

}  // namespace

coro::CoroTask<dstd::Result<ExportStats>> run_materialize(
    const ViewPlan& plan, const ProgressFn* progress) {
    ensure_schema(plan);
    if (auto resolved = resolve_record_schema(plan); !resolved)
        co_return resolved.error();

    // Row query (no aggregation): materialize a filtered trace + index into a
    // view root under the index root's views, then record its manifest so a
    // later subsuming query reads it instead of rescanning the base.
    if (plan.group_by.empty() && plan.agg.empty() &&
        !plan.auto_numeric_metrics) {
        if (dftracer::utils::trace::index::cache::view_is_fresh(plan))
            co_return ExportStats{};  // already built
        const dstd::String dir =
            dftracer::utils::trace::index::cache::materialize_view_dir(plan);
        if (dir.empty()) co_return ExportStats{};
        // Single-flight: one builder per view root. A loser skips - the winner
        // produces the MV. Re-check freshness under the lock in case the winner
        // just finished.
        dstd::File lock =
            dftracer::utils::trace::index::cache::lock_view_dir(dir);
        if (!lock.is_open()) co_return ExportStats{};
        ViewDirLock lock_guard(std::move(lock));
        if (dftracer::utils::trace::index::cache::view_is_fresh(plan))
            co_return ExportStats{};
        constexpr std::uint64_t DEFAULT_PART_SIZE = 128ull * 1024 * 1024;
        TraceWriteOptions opts;
        opts.output_path = dstd::String(
            (dstd::PathView(
                 dftracer::utils::trace::index::view_parts_dir(dir)) /
             "part.pfw.gz")
                .str());
        opts.index_path = dir;
        opts.build_index = true;
        opts.compress = true;
        // One gzip member per index checkpoint, so the user's checkpoint_size
        // is the member size to write at.
        opts.member_size = plan.mv_checkpoint_size;
        opts.part_size =
            plan.mv_part_size ? plan.mv_part_size : DEFAULT_PART_SIZE;
        auto exported = co_await run_export_trace_indexed(plan, opts, progress);
        if (!exported) co_return exported.error();
        dftracer::utils::trace::index::cache::register_view(dir, plan);
        co_return *exported;
    }

    const dstd::String rdir =
        dftracer::utils::trace::index::cache::rollup_cache_path(plan);
    // Idempotent prewarm: skip if this view is already materialized.
    if (!rdir.empty() &&
        dftracer::utils::trace::index::cache::CacheStore::exists(rdir)) {
        auto db = dftracer::utils::trace::index::cache::open_rollup_db(
            rdir,
            dftracer::utils::trace::index::cache::CacheStore::Mode::ReadOnly);
        if (db) {
            const auto exists =
                dftracer::utils::trace::index::cache::rollup_exists(
                    *db,
                    dftracer::utils::trace::index::cache::plan_signature(plan));
            if (!exists.has_value())
                DFTRACER_UTILS_LOG_WARN("rollup check skipped: {}",
                                        exists.error().message());
            else if (*exists)
                co_return ExportStats{};
        }
    }

    dataframe::AggStatePtr state;
    const auto from_partials = co_await partials_collect(plan, state);
    if (!from_partials.has_value()) co_return from_partials.error();
    if (!*from_partials) {
        auto built = co_await build_engine_agg_state(plan);
        if (!built) co_return built.error();
        state = std::move(*built);
    }
    if (!rdir.empty()) {
        auto db = dftracer::utils::trace::index::cache::open_rollup_db(
            rdir,
            dftracer::utils::trace::index::cache::CacheStore::Mode::ReadWrite);
        if (db) {
            const auto persisted =
                dftracer::utils::trace::index::cache::persist_rollup(
                    *db,
                    dftracer::utils::trace::index::cache::plan_signature(plan),
                    dftracer::utils::trace::index::cache::rest_signature(plan),
                    plan.time_bucket_us, plan.group_by, *state);
            if (!persisted)
                DFTRACER_UTILS_LOG_WARN("rollup materialize skipped: {}",
                                        persisted.error().message());
        }
    }
    co_return ExportStats{};
}

// Serve `plan` from a subsuming persisted rollup with no scan: re-aggregate the
// stored AggState partials to `plan`'s grouping and finalize. Shared by the
// engine collect path and the session so the two agree on what a rollup
// answers.
dstd::Optional<dataframe::DataFrame> try_serve_rollup(const ViewPlan& plan) {
    const dstd::String rdir =
        dftracer::utils::trace::index::cache::rollup_cache_path(plan);
    if (rdir.empty() ||
        !dftracer::utils::trace::index::cache::CacheStore::exists(rdir))
        return dstd::nullopt;
    // A materialize() query opens ReadWrite so a miss reuses the handle to
    // persist below (avoids a RO-then-RW conflict).
    const auto mode =
        plan.materialize
            ? dftracer::utils::trace::index::cache::CacheStore::Mode::ReadWrite
            : dftracer::utils::trace::index::cache::CacheStore::Mode::ReadOnly;
    auto db = dftracer::utils::trace::index::cache::open_rollup_db(rdir, mode);
    if (db) {
        auto found =
            dftracer::utils::trace::index::cache::find_subsuming_rollup(*db,
                                                                        plan);
        if (found) return std::move(*found);
        // A locked/unreadable rollup just falls through to a normal compute;
        // never fail the query over the cache.
        DFTRACER_UTILS_LOG_WARN("rollup read skipped: {}",
                                found.error().message());
    }
    return dstd::nullopt;
}

// On a hit fills `out` (a mergeable engine AggState, finalize with
// finalize_engine_result) and returns true. False means the query must scan.
coro::CoroTask<dstd::Result<bool>> try_serve_aggregate_no_scan(
    const ViewPlan& plan, dataframe::AggStatePtr& out) {
    // Named, not awaited in the condition: GCC 12.1 on x86_64 compiled the
    // awaited temporary there into a trap, and every first aggregate of a
    // file died with SIGILL.
    auto bootstrapped = co_await try_collect_bootstrap(plan, out);
    if (!bootstrapped.has_value()) co_return bootstrapped.error();
    if (*bootstrapped) co_return true;
    // Named for the same reason.
    auto from_partials = co_await partials_collect(plan, out);
    co_return from_partials;
}

// Resolve a min-aligned bucket origin to the trace's minimum timestamp, read
// from the index zone maps (no event scan), in the post-time_scale unit the
// fold buckets in. Idempotent: a plan not requesting min alignment (or with no
// bucket) is returned unchanged, so terminals can call it defensively. A
// missing/locked index leaves the origin at 0 (absolute).
ViewPlan resolve_bucket_origin(const ViewPlan& plan) {
    if (!plan.bucket_origin_min || plan.time_bucket_us == 0) return plan;
    std::uint64_t global_min = std::numeric_limits<std::uint64_t>::max();
    const bool by_path = plan_by_path(plan);
    const trace::index::Roles& roles = plan_record_schema(plan).roles;
    dstd::Map<dstd::String, trace::index::Snapshot> snapshots;
    // The least time of one file; a file whose index cannot be read is left
    // out, and the origin falls back to 0 if none resolve.
    auto file_minimum = [&](const ViewFile& f) -> dstd::Result<void> {
        auto at = snapshots.find(f.index_path);
        if (at == snapshots.end()) {
            auto taken = snapshot_of(plan, f.index_path);
            if (!taken) return taken.error();
            at = snapshots.emplace(f.index_path, std::move(*taken)).first;
        }
        const trace::index::Snapshot& snapshot = at->second;
        const auto file = snapshot.file(fileio::get_logical_path(f.file_path));
        if (!file) return {};
        const bool is_past =
            f.pinned_c_end > 0 &&
            (f.pinned_uc_begin > 0 || file->stat().size != f.pinned_c_end);
        dstd::Vector<trace::index::gzip::ChunkSpan> spans;
        if (is_past) {
            auto listed = trace::index::spans(snapshot, *file);
            if (!listed) return listed.error();
            spans = std::move(*listed);
        }
        const auto is_in_view = [&](std::uint64_t chunk) {
            return !is_past || (chunk < spans.size() &&
                                spans[chunk].uc_offset < f.pinned_uc_end &&
                                spans[chunk].uc_offset + spans[chunk].uc_size >
                                    f.pinned_uc_begin);
        };
        if (by_path) {
            auto ranges = trace::index::extensions::kinds::chunk_value_ranges(
                snapshot, *file, roles.time);
            if (!ranges) return ranges.error();
            dstd::Optional<double> lowest;
            for (const auto& [chunk, range] : *ranges) {
                if (is_in_view(chunk))
                    lowest =
                        std::min(lowest.value_or(range.first), range.first);
            }
            if (lowest && *lowest >= 0)
                global_min = std::min(
                    global_min,
                    static_cast<std::uint64_t>(std::llround(
                        *lowest * trace::index::micros_per(roles.time_unit))));
            return {};
        }
        if (is_past) {
            auto bounds = trace::index::extensions::kinds::chunk_time_bounds(
                snapshot, *file);
            if (!bounds) return bounds.error();
            for (const auto& [chunk, range] : *bounds)
                if (is_in_view(chunk))
                    global_min = std::min(global_min, range.first);
            return {};
        }
        const auto b =
            dftracer::utils::trace::index::extensions::kinds::file_time_bounds(
                snapshot, *file);
        if (!b) return b.error();
        if (b->valid && b->min_timestamp_us < global_min)
            global_min = b->min_timestamp_us;
        return {};
    };
    for (const auto& f : plan.files) {
        if (f.index_path.empty() ||
            !common::input_exists(trace::index::store_of(f.index_path)))
            continue;
        file_minimum(f).discard();
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

coro::CoroTask<dstd::Result<TypedResult>> run_collect_typed(
    const ViewPlan& plan, int shard_begin, int shard_end,
    const ProgressFn* progress) {
    ensure_schema(plan);
    TypedResult out;
    const auto from_partials = co_await partials_collect_typed(
        plan, out.regular, out.aggregated, shard_begin, shard_end, progress);
    if (!from_partials.has_value()) co_return from_partials.error();
    if (*from_partials) {
        const auto from_columns = system_collect(
            plan, out.counters, shard_begin, shard_end, progress);
        if (!from_columns.has_value()) co_return from_columns.error();
        if (*from_columns) co_return out;
    }
    // Scan the raw trace, so collect_typed returns rows rather than silently
    // none.
    auto made = make_vdef(plan, /*for_aggregation=*/false);
    if (!made) co_return made.error();
    ViewDefinition vdef = std::move(*made);
    dftracer::utils::dstd::StringIntern intern;
    TypedCollectFold fold(intern);
    std::array<Fold*, 1> folds{&fold};
    auto fused = co_await fuse(plan, vdef, folds, intern);
    if (!fused) co_return fused.error();
    out.regular = fold.build_regular();
    out.aggregated = fold.build_aggregated();
    out.counters = fold.build_counters();
    co_return out;
}

bool is_row_query(const ViewPlan& plan) {
    return plan.group_by.empty() && plan.agg.empty() &&
           !plan.auto_numeric_metrics;
}

namespace {

// The value of arg `keyid` on `ev`, or nullptr if the event lacks it (a JSON
// null is a Special, not an arg, so it also reads as absent - the same null the
// built column gives it).
const FoldEvent::ArgValue* distinct_find_arg(const FoldEvent& ev,
                                             std::uint32_t keyid) {
    for (const auto& [k, v] : ev.args)
        if (k == keyid) return &v;
    return nullptr;
}

// Order-preserving distinct pushed into the scan. Each worker keeps, per key,
// the first row it saw together with that row's (unit, index) position; the
// slices merge keeping the smallest position and the result is sorted by it.
// The key tags every value with its kind and keeps doubles by bit pattern, so
// it is never coarser than the key of the built column: the rows kept are a
// superset of the host result, which still holds the first occurrence of every
// host key. Where kinds mix in a column the key may be finer than the host's,
// and needs_host_pass() says to dedup the built frame once more.
class DistinctRowFold : public Fold {
   public:
    struct Key {
        RowKeySource::Kind field = RowKeySource::Kind::Arg;
        std::uint32_t arg_full = dftracer::utils::dstd::StringIntern::NO_ID;
        std::uint32_t arg_bare = dftracer::utils::dstd::StringIntern::NO_ID;
    };

    DistinctRowFold(const dftracer::utils::dstd::StringIntern& intern,
                    dstd::Vector<dstd::String> select, dstd::Vector<Key> keys,
                    double time_scale, bool keep_metadata, bool by_path)
        : intern_(&intern),
          select_(std::move(select)),
          keys_(std::move(keys)),
          kinds_(keys_.size(), 0),
          time_scale_(time_scale),
          keep_metadata_(keep_metadata),
          by_path_(by_path) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool needs_args() const override { return true; }
    bool wants_metadata() const override { return keep_metadata_; }

    dstd::Vector<dstd::String> extra_captures() const override {
        return row_fold_extra_captures(select_);
    }

    dstd::UniquePtr<Fold> slice() const override {
        return dstd::make_unique<DistinctRowFold>(
            *intern_, select_, keys_, time_scale_, keep_metadata_, by_path_);
    }

    dstd::Result<void> step(const FoldBatch& batch) override {
        const auto seq = static_cast<std::int64_t>(batch.unit.seq);
        if (seq != cur_seq_) {
            cur_seq_ = seq;
            row_in_unit_ = 0;
        }
        for (const FoldEvent& ev : batch.events) {
            const Position pos{seq, row_in_unit_++};
            if (ev.phase == RecordPhase::Unknown ||
                (!keep_metadata_ && ev.phase == RecordPhase::Metadata))
                continue;
            scratch_.clear();
            append_key(ev);
            auto [it, inserted] = index_.try_emplace(scratch_, entries_.size());
            if (inserted) {
                entries_.push_back(Entry{pos, ev});
            } else {
                Entry& e = entries_[it->second];
                if (pos < e.pos) {
                    e.pos = pos;
                    e.ev = ev;
                }
            }
        }
        return {};
    }

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}

    dstd::Result<void> merge(Fold& other) override {
        auto& o = static_cast<DistinctRowFold&>(other);
        for (std::size_t i = 0; i < kinds_.size(); ++i)
            kinds_[i] |= o.kinds_[i];
        for (auto& [key, idx] : o.index_) {
            Entry& src = o.entries_[idx];
            auto [it, inserted] = index_.try_emplace(key, entries_.size());
            if (inserted) {
                entries_.push_back(std::move(src));
            } else {
                Entry& e = entries_[it->second];
                if (src.pos < e.pos) e = std::move(src);
            }
        }
        o.entries_.clear();
        o.index_.clear();
        return {};
    }

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        co_return true;
    }

    // True when some key column saw a double or more than one value kind, so
    // two rows with distinct fold keys may still be equal in the built column.
    bool needs_host_pass() const {
        for (std::size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i].field != RowKeySource::Kind::Arg) continue;
            const std::uint8_t m = kinds_[i];
            if ((m & KIND_DBL) || (m & (m - 1))) return true;
        }
        return false;
    }

    // The distinct candidates in first-position order.
    dataframe::DataFrame build() {
        dstd::Vector<std::size_t> order(entries_.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b) {
                      return entries_[a].pos < entries_[b].pos;
                  });
        dstd::Vector<FoldEvent> evs;
        evs.reserve(entries_.size());
        for (std::size_t i : order) evs.push_back(std::move(entries_[i].ev));
        entries_.clear();
        index_.clear();
        return build_row_frame(dstd::Span<const FoldEvent>(evs), *intern_,
                               select_, time_scale_, by_path_);
    }

   private:
    struct Position {
        std::int64_t seq;
        std::int64_t row;
        bool operator<(const Position& o) const {
            return seq != o.seq ? seq < o.seq : row < o.row;
        }
    };
    struct Entry {
        Position pos;
        FoldEvent ev;
    };

    static constexpr std::uint8_t KIND_STR = 1;
    static constexpr std::uint8_t KIND_INT = 2;
    static constexpr std::uint8_t KIND_UINT = 4;
    static constexpr std::uint8_t KIND_DBL = 8;

    static void append_id(dstd::String& out, std::uint32_t id) {
        if (id == dftracer::utils::dstd::StringIntern::NO_ID) {
            out.push_back('\0');
            return;
        }
        out.push_back('\1');
        out.append(reinterpret_cast<const char*>(&id), sizeof(id));
    }

    static void append_word(dstd::String& out, char tag, std::uint64_t v) {
        out.push_back(tag);
        out.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }

    void append_key(const FoldEvent& ev) {
        using Kind = RowKeySource::Kind;
        for (std::size_t j = 0; j < keys_.size(); ++j) {
            const Key& k = keys_[j];
            switch (k.field) {
                case Kind::Name:
                    append_id(scratch_, ev.name_id);
                    break;
                case Kind::Cat:
                    append_id(scratch_, ev.cat_id);
                    break;
                case Kind::Fhash:
                    append_id(scratch_, ev.fhash_id);
                    break;
                case Kind::Hhash:
                    append_id(scratch_, ev.hhash_id);
                    break;
                case Kind::Pid:
                    append_word(scratch_, '\1', ev.pid);
                    break;
                case Kind::Tid:
                    append_word(scratch_, '\1', ev.tid);
                    break;
                case Kind::Ts:
                    append_word(scratch_, '\1', scaled(ev.ts));
                    break;
                case Kind::Dur:
                    if (!ev.has_dur) {
                        scratch_.push_back('\0');
                        break;
                    }
                    append_word(scratch_, '\1', scaled(ev.dur));
                    break;
                case Kind::Ph:
                    append_word(scratch_, '\1',
                                static_cast<std::uint64_t>(ev.phase));
                    break;
                case Kind::Arg:
                case Kind::Refused: {
                    const FoldEvent::ArgValue* v =
                        distinct_find_arg(ev, k.arg_full);
                    if (!v && k.arg_bare !=
                                  dftracer::utils::dstd::StringIntern::NO_ID)
                        v = distinct_find_arg(ev, k.arg_bare);
                    if (!v) {
                        scratch_.push_back('\0');
                    } else if (const auto* id = std::get_if<std::uint32_t>(v)) {
                        kinds_[j] |= KIND_STR;
                        scratch_.push_back('\1');
                        scratch_.append(reinterpret_cast<const char*>(id),
                                        sizeof(*id));
                    } else if (const auto* i = std::get_if<std::int64_t>(v)) {
                        kinds_[j] |= KIND_INT;
                        append_word(scratch_, '\2',
                                    static_cast<std::uint64_t>(*i));
                    } else if (const auto* u = std::get_if<std::uint64_t>(v)) {
                        kinds_[j] |= KIND_UINT;
                        append_word(scratch_, '\3', *u);
                    } else {
                        kinds_[j] |= KIND_DBL;
                        std::uint64_t bits;
                        const double d = std::get<double>(*v);
                        std::memcpy(&bits, &d, sizeof(bits));
                        append_word(scratch_, '\4', bits);
                    }
                    break;
                }
            }
        }
    }

    std::uint64_t scaled(std::uint64_t v) const {
        if (time_scale_ == 1.0) return v;
        return static_cast<std::uint64_t>(static_cast<double>(v) * time_scale_ +
                                          0.5);
    }

    const dftracer::utils::dstd::StringIntern* intern_;
    dstd::Vector<dstd::String> select_;
    dstd::Vector<Key> keys_;
    dstd::Vector<std::uint8_t> kinds_;
    double time_scale_;
    bool keep_metadata_;
    bool by_path_;
    std::int64_t cur_seq_ = -1;
    std::int64_t row_in_unit_ = 0;
    dstd::String scratch_;
    dstd::Vector<Entry> entries_;
    dstd::HashMap<dstd::String, std::size_t> index_;
};

}  // namespace

bool distinct_keyable(const ViewPlan& plan,
                      const dstd::Vector<std::int64_t>& key_idx) {
    const bool by_path = plan_by_path(plan);
    for (std::int64_t at : key_idx) {
        if (at < 0 || static_cast<std::size_t>(at) >= plan.select.size())
            return false;
        if (row_key_source(plan.select[static_cast<std::size_t>(at)], by_path)
                .kind == RowKeySource::Kind::Refused)
            return false;
    }
    return true;
}

coro::CoroTask<dstd::Result<dataframe::DataFrame>> run_collect_rows(
    const ViewPlan& plan) {
    auto made = make_vdef(plan, /*for_aggregation=*/false);
    if (!made) co_return made.error();
    ViewDefinition vdef = std::move(*made);
    dftracer::utils::dstd::StringIntern intern;
    const bool by_path = plan_by_path(plan);
    NativeRowFold fold(intern, plan.select, plan.time_scale,
                       metadata_rows(plan), by_path);
    dstd::Vector<Fold*> folds{&fold};
    auto selected = select_catalog_fold(plan, vdef, intern);
    if (!selected) co_return selected.error();
    auto catalog = std::move(*selected);
    if (catalog) folds.push_back(catalog.get());
    auto fused = execute(lower_fused_folds(plan, vdef, folds), intern);
    auto fused_stats = co_await std::move(fused);
    if (!fused_stats) co_return fused_stats.error();
    dataframe::DataFrame b = fold.build();
    // sort_col/topk_col name a column the way a caller would select it (bare
    // or "args."-prefixed); canonicalize to match build_row_frame's actual
    // output name before resolving against the built frame.
    if (!plan.sort_col.empty()) {
        auto sorted = b.sort_by(
            canonical_row_column_name(plan.sort_col, by_path), plan.sort_desc);
        if (!sorted) co_return sorted.error();
        b = std::move(*sorted);
    }
    if (!plan.topk_col.empty()) {
        auto top = b.topk(canonical_row_column_name(plan.topk_col, by_path),
                          plan.topk_k, plan.topk_largest);
        if (!top) co_return top.error();
        b = std::move(*top);
    }
    if (plan.offset || plan.limit) {
        const std::int64_t off = static_cast<std::int64_t>(plan.offset);
        const std::int64_t len =
            plan.limit ? static_cast<std::int64_t>(plan.limit) : b.num_rows();
        b = b.slice(off, len);
    }
    co_return b;
}

coro::CoroTask<dstd::Result<dataframe::DataFrame>> run_collect_rows_distinct(
    const ViewPlan& plan, const dstd::Vector<std::int64_t>& key_idx) {
    auto made = make_vdef(plan, /*for_aggregation=*/false);
    if (!made) co_return made.error();
    ViewDefinition vdef = std::move(*made);
    dftracer::utils::dstd::StringIntern intern;
    const bool by_path = plan_by_path(plan);
    dstd::Vector<DistinctRowFold::Key> keys;
    dstd::Vector<dstd::String> subset;
    keys.reserve(key_idx.size());
    subset.reserve(key_idx.size());
    for (std::int64_t at : key_idx) {
        const dstd::String& sel = plan.select[static_cast<std::size_t>(at)];
        const RowKeySource src = row_key_source(sel, by_path);
        DistinctRowFold::Key k;
        k.field = src.kind;
        if (src.kind == RowKeySource::Kind::Arg) {
            k.arg_full = intern.get_or_insert(src.arg_full);
            if (!src.arg_bare.empty())
                k.arg_bare = intern.get_or_insert(src.arg_bare);
        }
        keys.push_back(k);
        subset.push_back(canonical_row_column_name(sel, by_path));
    }
    DistinctRowFold fold(intern, plan.select, std::move(keys), plan.time_scale,
                         metadata_rows(plan), by_path);
    dstd::Vector<Fold*> folds{&fold};
    auto selected = select_catalog_fold(plan, vdef, intern);
    if (!selected) co_return selected.error();
    auto catalog = std::move(*selected);
    if (catalog) folds.push_back(catalog.get());
    auto fused = execute(lower_fused_folds(plan, vdef, folds), intern);
    auto fused_stats = co_await std::move(fused);
    if (!fused_stats) co_return fused_stats.error();
    const bool host_pass = fold.needs_host_pass();
    dataframe::DataFrame b = fold.build();
    if (!host_pass) co_return b;
    // The frame holds only the fold's candidates, already in first-position
    // order, so the host first-occurrence dedup runs over far fewer rows.
    auto deduped =
        dataframe::LazyFrame::scan(
            dstd::make_shared<dataframe::InMemorySource>(std::move(b)))
            .unique(std::move(subset));
    auto collecting = deduped.collect();
    co_return co_await std::move(collecting);
}

namespace {

template <class R>
coro::CoroTask<dstd::Result<R>> run_containment_terminal(
    const ViewPlan& plan, dstd::Vector<dstd::String> partition,
    dstd::String ts_field, dstd::String dur_field, dstd::String name_field,
    dstd::Vector<dstd::String> group, R (ContainmentFold::*build)() const) {
    auto made = make_vdef(plan, /*for_aggregation=*/false);
    if (!made) co_return made.error();
    ViewDefinition vdef = std::move(*made);
    dftracer::utils::dstd::StringIntern intern;
    ContainmentFold fold(
        intern, std::move(partition), std::move(ts_field), std::move(dur_field),
        std::move(name_field), plan.time_scale, std::move(group),
        plan_by_path(plan) ? &plan_record_schema(plan) : nullptr);
    std::array<Fold*, 1> folds{&fold};
    auto fused = co_await fuse(plan, vdef, folds, intern);
    if (!fused) co_return fused.error();
    co_return (fold.*build)();
}

}  // namespace

coro::CoroTask<dstd::Result<dataframe::DataFrame>> run_call_tree(
    const ViewPlan& plan, dstd::Vector<dstd::String> partition,
    dstd::String ts_field, dstd::String dur_field, dstd::String name_field) {
    return run_containment_terminal(
        plan, std::move(partition), std::move(ts_field), std::move(dur_field),
        std::move(name_field), {}, &ContainmentFold::call_tree);
}

coro::CoroTask<dstd::Result<dataframe::DataFrame>> run_flamegraph(
    const ViewPlan& plan, dstd::Vector<dstd::String> partition,
    dstd::String ts_field, dstd::String dur_field, dstd::String name_field,
    dstd::Vector<dstd::String> group) {
    return run_containment_terminal(
        plan, std::move(partition), std::move(ts_field), std::move(dur_field),
        std::move(name_field), std::move(group), &ContainmentFold::flamegraph);
}

coro::CoroTask<
    dstd::Result<dstd::Pair<dataframe::DataFrame, dataframe::DataFrame>>>
run_containment(const ViewPlan& plan, dstd::Vector<dstd::String> partition,
                dstd::String ts_field, dstd::String dur_field,
                dstd::String name_field, dstd::Vector<dstd::String> group) {
    return run_containment_terminal(
        plan, std::move(partition), std::move(ts_field), std::move(dur_field),
        std::move(name_field), std::move(group), &ContainmentFold::containment);
}

coro::CoroTask<dstd::Result<dstd::String>> run_flamegraph_partial(
    const ViewPlan& plan, dstd::Vector<dstd::String> partition,
    dstd::String ts_field, dstd::String dur_field, dstd::String name_field,
    dstd::Vector<dstd::String> group) {
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

dstd::String encode_agg_partial(const dataframe::AggState& st) {
    dstd::String out(1, static_cast<char>(AggWireTag::AggState));
    out += dataframe::agg_serialize(st);
    return out;
}

// Merge every partial into one AggState. An empty or unknown-tagged blob is
// rejected, not misparsed.
dstd::Result<dataframe::AggStatePtr> decode_partials(
    const dstd::Vector<dstd::StringView>& partials) {
    dataframe::AggStatePtr state;
    for (dstd::StringView p : partials) {
        if (p.empty())
            return common::make_error(
                common::ErrorCode::Aggregation,
                "aggregate partial: empty (missing wire tag)");
        if (static_cast<std::uint8_t>(p[0]) !=
            static_cast<std::uint8_t>(AggWireTag::AggState))
            return common::make_error(common::ErrorCode::Aggregation,
                                      "aggregate partial: unknown wire tag");
        auto decoded = dataframe::agg_deserialize(dstd::String(p.substr(1)));
        if (!decoded) return decoded.error();
        dataframe::AggStatePtr st = std::move(*decoded);
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
dstd::Result<dataframe::AggStatePtr> empty_engine_state(const ViewPlan& plan) {
    auto made = make_agg_input_spec(plan);
    if (!made) return made.error();
    AggInputSpec& spec = *made;
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
coro::CoroTask<dstd::Result<void>> run_materialize_partials(
    const ViewPlan& plan, const dstd::Vector<dstd::StringView>& partials) {
    if (auto resolved = resolve_record_schema(plan); !resolved)
        co_return resolved.error();
    ensure_schema(plan);
    const dstd::String rdir =
        dftracer::utils::trace::index::cache::rollup_cache_path(plan);
    if (rdir.empty()) co_return dstd::Result<void>{};
    auto decoded = decode_partials(partials);
    if (!decoded) co_return decoded.error();
    dataframe::AggStatePtr state = std::move(*decoded);
    if (!state) co_return dstd::Result<void>{};
    auto db = dftracer::utils::trace::index::cache::open_rollup_db(
        rdir,
        dftracer::utils::trace::index::cache::CacheStore::Mode::ReadWrite);
    if (db) {
        const auto persisted =
            dftracer::utils::trace::index::cache::persist_rollup(
                *db, dftracer::utils::trace::index::cache::plan_signature(plan),
                dftracer::utils::trace::index::cache::rest_signature(plan),
                plan.time_bucket_us, plan.group_by, *state);
        if (!persisted)
            DFTRACER_UTILS_LOG_WARN("rollup materialize (partials) skipped: {}",
                                    persisted.error().message());
    }
    co_return dstd::Result<void>{};
}

dstd::Result<dstd::Optional<dataframe::DataFrame>> run_reconstruct_if_cached(
    const ViewPlan& plan) {
    if (plan.group_by.empty() && plan.agg.empty())
        return dstd::Optional<dataframe::DataFrame>();
    if (auto resolved = resolve_record_schema(plan); !resolved)
        return resolved.error();
    ensure_schema(plan);
    return try_serve_rollup(plan);
}

coro::CoroTask<dstd::Result<ExportStats>> run_scan_batches(
    const dstd::SharedPtr<const ViewPlan>& plan_ptr, std::size_t slot_count,
    std::uint64_t limit,
    const dstd::Function<
        void(std::size_t, const dstd::Vector<dstd::StringView>&)>& on_batch) {
    bool from_mv = false;
    auto redirected = redirect_reads(*plan_ptr, &from_mv);
    if (!redirected) co_return redirected.error();
    const ViewPlan plan = std::move(*redirected);
    auto made = make_vdef(plan, /*for_aggregation=*/false);
    if (!made) co_return made.error();
    ViewDefinition vdef = std::move(*made);
    // map_batches wants a fixed slot count; treat 0 as 1.
    auto scanned = co_await for_each_scanned_batch(
        plan, vdef, slot_count ? slot_count : 1, limit, on_batch);
    if (!scanned) co_return scanned.error();
    ExportStats stats = *scanned;
    stats.served_from_mv = from_mv;
    co_return stats;
}

// An externally-built Fold joined to the shared scan (the plugin/JIT seam).
// `make` builds it with the scan's intern so ids agree and per-worker slices
// merge; `finalize` runs after the merge.
struct FoldFactory {
    dstd::Function<dstd::UniquePtr<Fold>(dftracer::utils::dstd::StringIntern&)>
        make;
    dstd::Function<dstd::Result<void>()> finalize;
};

struct StreamBranch {
    dstd::SharedPtr<const ViewPlan> plan;
    dstd::SharedPtr<coro::UnboundedChannel<dataframe::Morsel>> channel;
    dstd::SharedPtr<coro::AsyncSemaphore> budget;
    dstd::SharedPtr<const std::atomic<bool>> dropped;
};

struct ViewSessionState {
    dstd::SharedPtr<const ViewPlan> plan;
    dstd::Vector<BranchHooks> branches;
    dstd::Vector<FoldFactory> fold_factories;
    dstd::Vector<StreamBranch> stream_branches;
    /// A narrowing an attached fold offered for the shared scan, applied by
    /// execute() only when no other branch exists. The scan feeds every
    /// branch, so narrowing it for one would starve the rest; whether that
    /// holds is knowable only here, after every branch has been added.
    dstd::Optional<duql::Query> proposed_prune;
    /// The first error a registration kept; run_session ends with it.
    dstd::Optional<dstd::Error> error;
};

dstd::SharedPtr<ViewSessionState> make_view_session_state(
    dstd::SharedPtr<const ViewPlan> plan) {
    auto st = dstd::make_shared<ViewSessionState>();
    st->plan = std::move(plan);
    return st;
}

void propose_base_prune(ViewSessionState& state, duql::Query q) {
    state.proposed_prune = std::move(q);
}

void add_branch(ViewSessionState& state, BranchHooks hooks) {
    state.branches.push_back(std::move(hooks));
}

void add_stream_branch(
    ViewSessionState& state, dstd::SharedPtr<const ViewPlan> plan,
    dstd::SharedPtr<coro::UnboundedChannel<dataframe::Morsel>> channel,
    dstd::SharedPtr<coro::AsyncSemaphore> budget,
    dstd::SharedPtr<const std::atomic<bool>> dropped) {
    state.stream_branches.push_back({std::move(plan), std::move(channel),
                                     std::move(budget), std::move(dropped)});
}

void add_fold_factory(
    ViewSessionState& state,
    dstd::Function<dstd::UniquePtr<Fold>(dftracer::utils::dstd::StringIntern&)>
        make,
    dstd::Function<dstd::Result<void>()> finalize) {
    state.fold_factories.push_back({std::move(make), std::move(finalize)});
}

void keep_session_error(ViewSessionState& state, dstd::Error error) {
    if (!state.error) state.error = std::move(error);
}

void add_fold_branch(ViewSessionState& state, Query predicate,
                     dstd::Function<BranchConsumer()> make_consumer,
                     dstd::Function<dstd::Result<void>()> finalize,
                     bool wants_metadata) {
    BranchHooks h;
    h.wants_metadata = wants_metadata;
    h.predicate = std::move(predicate);
    h.make_consumer = std::move(make_consumer);
    h.finalize = std::move(finalize);
    add_branch(state, std::move(h));
}

void add_fold_branch(ViewSessionState& state,
                     dstd::Function<BranchConsumer()> make_consumer,
                     dstd::Function<dstd::Result<void>()> finalize,
                     bool wants_metadata) {
    BranchHooks h;  // predicate unset = match all scanned events
    h.wants_metadata = wants_metadata;
    h.make_consumer = std::move(make_consumer);
    h.finalize = std::move(finalize);
    add_branch(state, std::move(h));
}

void add_selection_fold_branch(ViewSessionState& state,
                               dstd::Function<BranchConsumer()> make_consumer,
                               dstd::Function<dstd::Result<void>()> finalize) {
    auto eq = effective_query(*state.plan);
    if (!eq) {
        keep_session_error(state, eq.error());
        return;
    }
    if (*eq)
        add_fold_branch(state, std::move(**eq), std::move(make_consumer),
                        std::move(finalize));
    else
        add_fold_branch(state, std::move(make_consumer), std::move(finalize));
}

void add_materialize_branch(ViewSessionState& state,
                            dstd::Vector<GroupKey> group_by,
                            dstd::Vector<AggSpec> agg) {
    auto plan = dstd::make_shared<ViewPlan>(*state.plan);
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
        return [](json::Value&, dstd::StringView) {};
    };
    h.finalize = [plan]() -> dstd::Result<void> {
        const dstd::String rdir =
            dftracer::utils::trace::index::cache::rollup_cache_path(*plan);
        if (rdir.empty()) return {};
        dataframe::AggStatePtr state_out;
        const auto ran = dftracer::utils::default_runtime().block_on(
            coro::named("session_materialize",
                        [&]() -> coro::CoroTask<dstd::Result<void>> {
                            auto built = co_await build_engine_agg_state(*plan);
                            if (!built) co_return built.error();
                            state_out = std::move(*built);
                            co_return dstd::Result<void>{};
                        }()));
        if (!ran || !state_out) return {};
        auto db = dftracer::utils::trace::index::cache::open_rollup_db(
            rdir,
            dftracer::utils::trace::index::cache::CacheStore::Mode::ReadWrite);
        if (db) {
            const auto persisted =
                dftracer::utils::trace::index::cache::persist_rollup(
                    *db,
                    dftracer::utils::trace::index::cache::plan_signature(*plan),
                    dftracer::utils::trace::index::cache::rest_signature(*plan),
                    plan->time_bucket_us, plan->group_by, *state_out);
            if (!persisted)
                DFTRACER_UTILS_LOG_WARN("fused rollup materialize skipped: {}",
                                        persisted.error().message());
        }
        return {};
    };
    add_branch(state, std::move(h));
}

BranchHooks make_export_branch(ExportSink& sink,
                               dstd::SharedPtr<ExportStats> out) {
    // The sink is one shared object, so writes still serialize on a mutex; only
    // the count is per worker.
    auto mtx = dstd::make_shared<std::mutex>();
    auto counts =
        dstd::make_shared<dstd::Vector<dstd::SharedPtr<std::uint64_t>>>();

    BranchHooks h;
    h.make_consumer = [&sink, mtx, counts]() -> BranchConsumer {
        auto count = dstd::make_shared<std::uint64_t>(0);
        counts->push_back(count);
        return [&sink, mtx, count](json::Value&, dstd::StringView raw) {
            std::lock_guard<std::mutex> lk(*mtx);
            sink.write(raw);
            sink.write("\n");
            ++*count;
        };
    };
    h.finalize = [out, counts]() -> dstd::Result<void> {
        std::uint64_t total = 0;
        for (const auto& c : *counts) total += *c;
        out->events_matched = total;
        return {};
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
    // `gate_metadata`: the scan carries metadata records only for the
    // branches that read them, so the others must not see them.
    BranchDriverFold(dstd::SharedPtr<dstd::Vector<const BranchHooks*>> branches,
                     bool gate_metadata)
        : branches_(std::move(branches)), gate_metadata_(gate_metadata) {}

    bool accepts(const ScanShape&) const override { return true; }
    bool wants_raw() const override { return true; }

    dstd::UniquePtr<Fold> slice() const override {
        auto s = dstd::make_unique<BranchDriverFold>(branches_, gate_metadata_);
        s->consumers_.reserve(branches_->size());
        for (const auto* br : *branches_)
            s->consumers_.push_back(br->make_consumer());
        return s;
    }

    // One on-demand parse per line serves the metadata gate, every predicate
    // and every consumer, each starting from the beginning of the event.
    dstd::Result<void> step(const FoldBatch& b) override {
        for (dstd::StringView line : b.raw) {
            auto parsed = doc_.parse(line);
            if (!parsed) continue;
            json::Value& event = *parsed;
            if (event.type() != json::Value::Type::Object) continue;
            const bool metadata =
                gate_metadata_ &&
                trace::event_phase(event) == trace::RecordPhase::Metadata;
            // A line with a syntax error in a part that is read is dropped.
            if (json::detail::ValueAccess::broken(event)) continue;
            for (std::size_t i = 0; i < branches_->size(); ++i) {
                const BranchHooks* br = (*branches_)[i];
                if (metadata && !br->wants_metadata) continue;
                if (br->predicate && !br->predicate->matches(event)) {
                    if (json::detail::ValueAccess::broken(event)) break;
                    continue;
                }
                json::detail::ValueAccess::rewind(event);
                if (json::detail::ValueAccess::broken(event)) break;
                consumers_[i](event, line);
            }
        }
        return {};
    }

    void seal_unit(const ScanUnit&) override {}
    void drop_unit(const ScanUnit&) override {}
    // Each slice's consumers hold their own state and the branch reduces them
    // in finalize, so there is nothing to fold slice-to-slice here.
    dstd::Result<void> merge(Fold&) override { return {}; }

    coro::CoroTask<bool> finalize(const CoverageSet&) override {
        for (const auto* br : *branches_) {
            if (!br->finalize) continue;
            auto finished = br->finalize();
            if (!finished && !failure_) failure_ = finished.error();
        }
        co_return false;
    }

    /// The first error a branch finalize returned, after the scan.
    const dstd::Optional<dstd::Error>& failure() const { return failure_; }

   private:
    dstd::Optional<dstd::Error> failure_;
    dstd::SharedPtr<dstd::Vector<const BranchHooks*>> branches_;
    bool gate_metadata_;
    dstd::Vector<BranchConsumer> consumers_;
    json::Document doc_;
};

coro::CoroTask<dstd::Result<ExportStats>> run_session(
    dstd::SharedPtr<ViewSessionState> state) {
    if (state->error) co_return *state->error;
    const ViewPlan& plan = *state->plan;

    // Serve each match-all aggregation branch from a rollup or the aggregation
    // tier with no scan; collect the rest. A match-all agg branch that must
    // scan carries its full plan (base + group_by/agg) so it can drive an
    // EngineAggFold.
    struct ScanBranch {
        const BranchHooks* br;
        dstd::SharedPtr<ViewPlan> agg_plan;  // set only for a match-all agg
        bool apply_query = false;  // branch EngineAggFold filters per event
    };
    dstd::Vector<ScanBranch> scan_branches;
    scan_branches.reserve(state->branches.size());
    for (const auto& br : state->branches) {
        if (br.agg && !br.predicate) {
            auto full = br.agg->plan
                            ? dstd::make_shared<ViewPlan>(*br.agg->plan)
                            : dstd::make_shared<ViewPlan>(plan);
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
                    auto post = apply_agg_post_ops(std::move(*df), *full);
                    if (!post) co_return post.error();
                    *br.agg->out = std::move(*post);
                    continue;
                }
                dataframe::AggStatePtr served;
                const auto try_serve_aggregate_no_scan_ok =
                    co_await try_serve_aggregate_no_scan(*full, served);
                if (!try_serve_aggregate_no_scan_ok.has_value())
                    co_return try_serve_aggregate_no_scan_ok.error();
                if (*try_serve_aggregate_no_scan_ok) {
                    auto finalized = finalize_engine_result(*served, *full);
                    if (!finalized) co_return finalized.error();
                    auto post =
                        apply_agg_post_ops(std::move(*finalized), *full);
                    if (!post) co_return post.error();
                    *br.agg->out = std::move(*post);
                    continue;
                }
            }
            // It reads events the other branches must not see, so it scans
            // on its own.
            if (scans_by_overlap(*full)) {
                auto st = co_await build_engine_agg_state(*full);
                if (!st) co_return st.error();
                if (br.agg->partial_out) {
                    *br.agg->partial_out = encode_agg_partial(**st);
                } else {
                    auto finalized = finalize_engine_result(**st, *full);
                    if (!finalized) co_return finalized.error();
                    auto post =
                        apply_agg_post_ops(std::move(*finalized), *full);
                    if (!post) co_return post.error();
                    *br.agg->out = std::move(*post);
                }
                continue;
            }
            scan_branches.push_back(
                {&br, std::move(full), br.agg->apply_query});
            continue;
        }
        scan_branches.push_back({&br, nullptr, false});
    }
    const bool has_factories = !state->fold_factories.empty();
    const bool has_streams = !state->stream_branches.empty();
    if (scan_branches.empty() && !has_factories && !has_streams)
        co_return ExportStats{};

    dstd::Vector<const ScanBranch*> agg_b, raw_b;
    for (const auto& sb : scan_branches)
        (sb.agg_plan ? agg_b : raw_b).push_back(&sb);

    // A lone aggregation with no raw branches runs through the engine (the
    // rollup/tier fast paths already missed above, so this is the scan). A fold
    // factory (plugin) rides the shared fused scan and a partial wants the raw
    // map, so both disqualify it.
    if (agg_b.size() == 1 && raw_b.empty() && !has_factories && !has_streams &&
        !agg_b[0]->br->agg->partial_out) {
        auto agg_state = co_await build_engine_agg_state(*agg_b[0]->agg_plan);
        if (!agg_state) co_return agg_state.error();
        auto finalized =
            finalize_engine_result(**agg_state, *agg_b[0]->agg_plan);
        if (!finalized) co_return finalized.error();
        auto post =
            apply_agg_post_ops(std::move(*finalized), *agg_b[0]->agg_plan);
        if (!post) co_return post.error();
        *agg_b[0]->br->agg->out = std::move(*post);
        co_return ExportStats{};
    }

    // One fused scan drives every branch: an EngineAggFold per aggregation and
    // one BranchDriverFold parsing raw lines for the fold/export branches.
    ViewPlan scan_plan = plan;
    // A fold-only session may narrow the shared scan: with no other branch
    // there is nobody to starve, and the index can skip whole chunks.
    if (state->branches.empty() && state->proposed_prune) {
        if (scan_plan.query) {
            scan_plan.query =
                duql::conjoin(*scan_plan.query, *state->proposed_prune);
        } else {
            scan_plan.query = *state->proposed_prune;
        }
    }
    if (auto resolved = resolve_record_schema(plan); !resolved)
        co_return resolved.error();
    if (auto mv =
            dftracer::utils::trace::index::cache::find_subsuming_view(plan))
        scan_plan.files = std::move(*mv);
    auto made = make_vdef(scan_plan, /*for_aggregation=*/true);
    if (!made) co_return made.error();
    ViewDefinition avdef = std::move(*made);
    // Streamed morsels hold the intern their ids resolve through, and outlive
    // this scan in their consumers' hands.
    auto intern_owner =
        dstd::make_shared<dftracer::utils::dstd::StringIntern>();
    dftracer::utils::dstd::StringIntern& intern = *intern_owner;

    dstd::Vector<dstd::UniquePtr<StreamRowFold>> streams;
    streams.reserve(state->stream_branches.size());
    for (const StreamBranch& sb : state->stream_branches) {
        const bool keep_metadata = metadata_rows(*sb.plan);
        if (keep_metadata) avdef.include_metadata = true;
        streams.push_back(dstd::make_unique<StreamRowFold>(
            sb.channel, sb.budget, intern_owner, sb.plan->select,
            sb.plan->time_scale, keep_metadata, false, sb.plan, sb.dropped,
            plan_by_path(*sb.plan)));
    }

    dstd::Vector<dstd::UniquePtr<EngineAggFold>> aggs(agg_b.size());
    dstd::Vector<dstd::UniquePtr<IndexRuleFold>> rule_aggs(agg_b.size());
    for (std::size_t i = 0; i < agg_b.size(); ++i) {
        auto rule = IndexRuleFold::make(*agg_b[i]->agg_plan, intern,
                                        agg_b[i]->apply_query);
        if (!rule) co_return rule.error();
        rule_aggs[i] = std::move(*rule);
        if (rule_aggs[i]) continue;
        auto spec = make_agg_input_spec(*agg_b[i]->agg_plan);
        if (!spec) co_return spec.error();
        aggs[i] = dstd::make_unique<EngineAggFold>(*agg_b[i]->agg_plan, intern,
                                                   std::move(*spec),
                                                   agg_b[i]->apply_query);
    }

    dstd::UniquePtr<BranchDriverFold> raw_fold;
    if (!raw_b.empty()) {
        auto branches = dstd::make_shared<dstd::Vector<const BranchHooks*>>();
        branches->reserve(raw_b.size());
        bool any_metadata = false;
        for (const auto* sb : raw_b) {
            branches->push_back(sb->br);
            any_metadata = any_metadata || sb->br->wants_metadata;
        }
        const bool gate = any_metadata && !avdef.include_metadata;
        if (gate) avdef.include_metadata = true;
        raw_fold =
            dstd::make_unique<BranchDriverFold>(std::move(branches), gate);
    }

    // Externally-built folds (plugins), constructed with the shared intern so
    // ids agree and per-worker slices merge.
    dstd::Vector<dstd::UniquePtr<Fold>> factory_folds;
    factory_folds.reserve(state->fold_factories.size());
    for (auto& ff : state->fold_factories)
        if (auto f = ff.make(intern)) factory_folds.push_back(std::move(f));

    dstd::Vector<Fold*> fold_ptrs;
    fold_ptrs.reserve(aggs.size() + factory_folds.size() + 1);
    for (std::size_t i = 0; i < agg_b.size(); ++i)
        fold_ptrs.push_back(rule_aggs[i]
                                ? static_cast<Fold*>(rule_aggs[i].get())
                                : static_cast<Fold*>(aggs[i].get()));
    if (raw_fold) fold_ptrs.push_back(raw_fold.get());
    for (auto& f : factory_folds) fold_ptrs.push_back(f.get());
    for (auto& f : streams) fold_ptrs.push_back(f.get());

    // A scan cap (viz-style early-out) applies only to a pure raw session; an
    // aggregation or plugin must see every event, and its limit is
    // post-aggregation.
    const std::uint64_t scan_cap =
        (agg_b.empty() && !has_factories && !has_streams) ? plan.limit : 0;
    auto fused = execute(lower_fused_folds(scan_plan, avdef, fold_ptrs), intern,
                         nullptr, scan_cap);
    auto fused_stats = co_await std::move(fused);
    if (!fused_stats) co_return fused_stats.error();
    ExportStats stats = *fused_stats;

    if (raw_fold && raw_fold->failure()) co_return *raw_fold->failure();
    for (std::size_t i = 0; i < agg_b.size(); ++i) {
        dataframe::AggState& branch_state =
            rule_aggs[i] ? rule_aggs[i]->state() : aggs[i]->state();
        // A partial serializes the mergeable AggState for a distributed merge;
        // a collect finalizes it (rank/resolver relabel) into the DataFrame.
        if (agg_b[i]->br->agg->partial_out) {
            *agg_b[i]->br->agg->partial_out = encode_agg_partial(branch_state);
            continue;
        }
        auto finalized =
            finalize_engine_result(branch_state, *agg_b[i]->agg_plan);
        if (!finalized) co_return finalized.error();
        auto post =
            apply_agg_post_ops(std::move(*finalized), *agg_b[i]->agg_plan);
        if (!post) co_return post.error();
        *agg_b[i]->br->agg->out = std::move(*post);
    }
    // Factory folds published their results in Fold::finalize during the fuse;
    // let the caller pull them (while the folds are still alive here).
    for (auto& ff : state->fold_factories) {
        if (!ff.finalize) continue;
        auto finished = ff.finalize();
        if (!finished) co_return finished.error();
    }
    co_return stats;
}

// export_counters with bounded memory: workers aggregate their share of chunks
// into in-memory maps, spilling to sorted runs when over budget; the runs are
// k-way merged and streamed to the sink, so peak memory stays bounded no matter
// the group cardinality. memory_budget == 0 keeps it purely in-memory.
coro::CoroTask<dstd::Result<ExportStats>> run_export_counters(
    const ViewPlan& plan, ExportSink& sink) {
    ensure_schema(plan);
    auto made = make_vdef(plan, /*for_aggregation=*/true);
    if (!made) co_return made.error();
    ViewDefinition vdef = std::move(*made);
    dftracer::utils::dstd::StringIntern intern;
    auto spec = make_agg_input_spec(plan);
    if (!spec) co_return spec.error();
    EngineAggFold agg(plan, intern, std::move(*spec));
    std::array<Fold*, 1> folds{&agg};
    auto stats = co_await fuse(plan, vdef, folds, intern);
    if (!stats) co_return stats.error();
    if (auto emitted = emit_counters_from_state(agg.state(), plan, sink);
        !emitted)
        co_return emitted.error();
    co_return *stats;
}

// Rank-local counter aggregation for distributed runs: aggregate this view's
// (shard of) files in memory and serialize the groups into an opaque partial
// buffer. The transport (MPI etc.) lives in the caller; only bytes cross ranks.
coro::CoroTask<dstd::Result<dstd::String>> run_aggregate_partial(
    const ViewPlan& plan) {
    // Index-only fast path: the EVENT-only tier answers without reading traces
    // (a sharded reader merges straight from indexes). Everything else scans;
    // both emit the same mergeable AggState wire partial.
    const bool has_occupancy =
        std::any_of(plan.agg.begin(), plan.agg.end(),
                    [](const AggSpec& s) { return is_occupancy_op(s.op); });
    if (plan.phase != Phase::Counters && !plan.auto_numeric_metrics &&
        !has_occupancy) {
        dataframe::AggStatePtr served;
        const auto from_partials = co_await partials_collect(plan, served);
        if (!from_partials.has_value()) co_return from_partials.error();
        if (*from_partials) co_return encode_agg_partial(*served);
    }
    auto state = co_await build_engine_agg_state(plan);
    if (!state) co_return state.error();
    co_return encode_agg_partial(**state);
}

// Merge partials and emit each combined group as a ph="C" counter event.
dstd::Result<ExportStats> merge_counters_partials(
    const ViewPlan& plan, const dstd::Vector<dstd::StringView>& partials,
    ExportSink& sink) {
    if (auto resolved = resolve_record_schema(plan); !resolved)
        return resolved.error();
    const ViewPlan p = resolve_bucket_origin(plan);
    ensure_schema(p);
    auto decoded = decode_partials(partials);
    if (!decoded) return decoded.error();
    dataframe::AggStatePtr state = std::move(*decoded);
    ExportStats st;
    if (state) {
        if (auto emitted = emit_counters_from_state(*state, p, sink); !emitted)
            return emitted.error();
        st.events_matched =
            static_cast<std::uint64_t>(dataframe::agg_group_count(*state));
    }
    return st;
}

// Merge partials into a materialized DataFrame (distributed collect()).
dstd::Result<dataframe::DataFrame> merge_partials_to_table(
    const ViewPlan& plan, const dstd::Vector<dstd::StringView>& partials) {
    if (auto resolved = resolve_record_schema(plan); !resolved)
        return resolved.error();
    const ViewPlan p = resolve_bucket_origin(plan);
    ensure_schema(p);
    auto decoded = decode_partials(partials);
    if (!decoded) return decoded.error();
    dataframe::AggStatePtr state = std::move(*decoded);
    if (!state) {
        auto empty = empty_engine_state(p);
        if (!empty) return empty.error();
        state = std::move(*empty);
    }
    return finalize_engine_result(*state, p);
}

}  // namespace dftracer::utils::trace::views::detail
