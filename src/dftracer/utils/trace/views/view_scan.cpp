#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/metadata_collector_utility.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/views/coverage.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view_planner_utility.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <simdjson.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

namespace dftracer::utils::trace::views::detail {

std::size_t checkpoint_size_or_default(std::size_t s) {
    return s != 0 ? s
                  : dftracer::utils::index::gzip::CheckpointIndexer::
                        DEFAULT_CHECKPOINT_SIZE;
}

const dftracer::utils::index::RecordSchema& plan_record_schema(
    const ViewPlan& plan) {
    namespace ix = dftracer::utils::index;
    if (auto p = plan.record_schema) return *p;
    std::vector<ix::plan::RowSetFile> files;
    files.reserve(plan.files.size());
    for (const auto& f : plan.files)
        files.push_back({f.file_path, f.index_path});
    const ix::RecordSchema* found = &ix::plan::files_schema(files);
    // Registered record_schemas live until the process exits, so the pointer
    // never dangles.
    plan.record_schema =
        std::shared_ptr<const ix::RecordSchema>(std::shared_ptr<void>(), found);
    return *found;
}

void require_role(const ViewPlan& plan, std::string_view op, TraceRole role) {
    const auto& record_schema = plan_record_schema(plan);
    auto need = [&](const std::string& path, std::string_view name) {
        if (path.empty())
            throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT, op,
                                         " needs a ", name, " role; schema ",
                                         record_schema.id, " binds none");
    };
    need(record_schema.roles.time, "time");
    if (role == TraceRole::DURATION)
        need(record_schema.roles.duration, "duration");
}

bool plan_by_path(const ViewPlan& plan) {
    return plan_record_schema(plan).decoder ==
           dftracer::utils::index::Decoder::PATH;
}

namespace {

std::optional<duql::Query> phase_query(const ViewPlan& plan) {
    // Path-decoded records are all data events and carry no phase field.
    if (plan_by_path(plan)) {
        if (plan.phase == Phase::Events || plan.phase == Phase::Any)
            return plan.query;
        throw DFTUtilsException(
            ErrorCode::INVALID_ARGUMENT,
            "record schema '" + plan_record_schema(plan).id +
                "' decodes records by path, which have no phase; only the "
                "events phase (every record) applies");
    }
    RecordPhase rp;
    if (plan.phase == Phase::Events) {
        rp = RecordPhase::COMPLETE;
    } else if (plan.phase == Phase::Counters) {
        rp = RecordPhase::COUNTER;
    } else if (plan.phase == Phase::Aggregated) {
        rp = RecordPhase::AGGREGATED;
    } else if (plan.phase == Phase::Metadata) {
        rp = RecordPhase::METADATA;
    } else {
        return plan.query;
    }
    // Match both the current integer "ph" and the legacy single-letter form so
    // the same View reads new- and old-format traces alike.
    duql::Query q = duql::parse_or_throw(
        "ph == " + std::to_string(phase_to_int(rp)) + " or ph == \"" +
        std::string(1, phase_to_letter(rp)) + "\"");
    if (plan.query) return duql::conjoin(*plan.query, q);
    return q;
}

}  // namespace

std::optional<duql::Query> effective_query(const ViewPlan& plan) {
    std::optional<duql::Query> q = phase_query(plan);
    if (!plan.all_records) {
        const std::string& data = plan_record_schema(plan).data;
        if (!data.empty() && data != dftracer::utils::index::NO_METADATA) {
            duql::Query d = duql::parse_or_throw(data);
            q = q ? duql::conjoin(*q, d) : std::move(d);
        }
    }
    if (!plan.build_step) return q;
    return plan.build_step->run(std::move(q));
}

namespace {

// Whether metadata records are rows of the plan: it reads `all`, or its
// source's `data` keeps them.
bool reads_metadata(const ViewPlan& plan) {
    return plan.all_records ||
           plan_record_schema(plan).data != dftracer::utils::index::NO_METADATA;
}

}  // namespace

bool metadata_rows(const ViewPlan& plan) {
    return plan.phase == Phase::Metadata ||
           (!plan_by_path(plan) && reads_metadata(plan));
}

bool is_cancelled(const ViewPlan& plan) {
    return plan.cancelled && plan.cancelled();
}

namespace {

// The paths a path-decoded scan of `plan` reads; empty to read every leaf (a
// full row collect, or metrics over every numeric field).
std::vector<std::string> needed_paths(const ViewPlan& plan) {
    const bool rows = plan.group_by.empty() && plan.agg.empty();
    if (plan.auto_numeric_metrics || (rows && plan.select.empty())) return {};
    std::vector<std::string> out;
    auto add = [&](std::string_view f) {
        if (!f.empty()) out.emplace_back(f);
    };
    for (const auto& sel : plan.select) add(select_source_field(sel));
    if (plan.query)
        for (std::string_view f : plan.query->fields()) add(f);
    for (const auto& gk : plan.group_by)
        if (gk.kind != GroupKey::Kind::Expr) add(gk.arg);
    for (const auto& a : plan.agg) {
        add(a.field);
        add(a.by);
    }
    for (const auto& c : plan.computed)
        for (const auto& in : c.inputs) add(select_source_field(in));
    add(plan.sort_col);
    add(plan.topk_col);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    // A computed column is not a record field, unless a computed column reads
    // the field of its name (a key computed as itself).
    std::erase_if(out, [&](const std::string& f) {
        const auto named = [&](const ComputedColumn& c) { return c.name == f; };
        const auto reads = [&](const ComputedColumn& c) {
            return std::any_of(c.inputs.begin(), c.inputs.end(),
                               [&](const std::string& in) {
                                   return select_source_field(in) == f;
                               });
        };
        return std::any_of(plan.computed.begin(), plan.computed.end(), named) &&
               std::none_of(plan.computed.begin(), plan.computed.end(), reads);
    });
    return out;
}

}  // namespace

namespace {

// For a schema's time field: a timed plan reads only records with a numeric,
// non-negative time, and a time range bounds that field in its unit, widened
// to whole units, so the field's zone map prunes; the window keeps the exact
// microsecond test.
void add_time_conditions(const ViewPlan& plan, ViewDefinition& vdef) {
    // ISO-8601 text times are compared after decoding, not on the raw record.
    const auto* time_field = plan_record_schema(plan).field_at(vdef.time_path);
    if (time_field &&
        time_field->type == dftracer::utils::index::FieldType::STRING)
        return;
    std::vector<std::string> conds;
    auto bound = [&](const char* op, double v) {
        conds.push_back(vdef.time_path + " " + op + " " +
                        std::to_string(static_cast<long long>(v)));
    };
    if (plan.timed) bound(">=", 0);
    if (plan.time_range) {
        bound("<", std::ceil(plan.time_range->second / vdef.time_factor));
        if (!vdef.window_overlap)
            bound(">=", std::floor(plan.time_range->first / vdef.time_factor));
    }
    if (conds.empty()) return;
    std::string q;
    for (const auto& c : conds) q += (q.empty() ? "" : " and ") + c;
    duql::Query bounds = duql::parse_or_throw(q);
    vdef.query = vdef.query ? duql::conjoin(*vdef.query, bounds) : bounds;
}

}  // namespace

ViewDefinition make_vdef(const ViewPlan& plan, bool for_aggregation) {
    ViewDefinition vdef;
    vdef.query = effective_query(plan);
    vdef.args_fallback = plan_record_schema(plan).args_fallback;
    if (plan_by_path(plan)) {
        namespace ix = dftracer::utils::index;
        const ix::RecordSchema& schema = plan_record_schema(plan);
        vdef.by_path = true;
        // A schema without fields changes no decoded value.
        if (!schema.fields.empty()) vdef.record_schema = &schema;
        vdef.paths = needed_paths(plan);
        const ix::Roles& roles = schema.roles;
        // Only trace operations read the roles.
        if (!vdef.paths.empty() && plan.timed) {
            for (const auto* p : {&roles.time, &roles.duration, &roles.entity,
                                  &roles.lane, &roles.name})
                if (!p->empty()) vdef.paths.push_back(*p);
            std::sort(vdef.paths.begin(), vdef.paths.end());
            vdef.paths.erase(std::unique(vdef.paths.begin(), vdef.paths.end()),
                             vdef.paths.end());
        }
        if (vdef.record_schema)
            for (const auto& p : vdef.paths)
                vdef.path_fields.push_back(schema.field_at(p));
        if (!roles.time.empty()) {
            vdef.time_path = roles.time;
            vdef.time_factor = ix::micros_per(roles.time_unit);
            vdef.duration_path = roles.duration;
            vdef.duration_factor = ix::micros_per(roles.duration_unit);
        }
    }
    // Rows select by start; occupancy needs every event that overlaps the
    // window, which the aggregation then clips (the engine's raw scan selects
    // the clipped columns).
    vdef.window = plan.time_range;
    vdef.window_overlap =
        (for_aggregation &&
         std::any_of(plan.agg.begin(), plan.agg.end(),
                     [](const AggSpec& s) { return is_occupancy_op(s.op); })) ||
        std::any_of(plan.select.begin(), plan.select.end(), is_clip_token);
    // The source's `data` leaves the metadata phase out, which
    // phase("metadata") selects alone; `all`, and a source whose `data` keeps
    // it, read metadata records as rows like any other. Path-decoded records
    // have no metadata phase.
    const bool metadata = plan.phase == Phase::Metadata;
    if (!plan_by_path(plan)) {
        const bool rows = reads_metadata(plan);
        vdef.include_metadata = rows || metadata;
        vdef.metadata_records = rows;
        vdef.filter_metadata = !rows && metadata;
    }
    if (!vdef.time_path.empty()) add_time_conditions(plan, vdef);
    return vdef;
}

ViewScannerInput make_scanner_input(const ScanUnit& u,
                                    const ViewDefinition& vdef,
                                    const std::optional<duql::Query>& q) {
    ViewScannerInput sin;
    sin.with_file_path(u.file_path)
        .with_index_path(u.index_path)
        .with_checkpoint_size(u.checkpoint_size)
        .with_byte_range(u.start_byte, u.end_byte)
        .with_checkpoint_idx(u.checkpoint_idx)
        .with_view(vdef);
    if (u.query)
        sin.query = *u.query;
    else
        sin.query = q;
    return sin;
}

coro::CoroTask<std::vector<ScanUnit>> gather_units(const ViewPlan& plan,
                                                   const ViewDefinition& vdef,
                                                   std::uint64_t& skipped_out) {
    std::vector<std::vector<ScanUnit>> per_file(plan.files.size());
    std::vector<std::uint64_t> skip_v(plan.files.size(), 0);
    std::vector<std::exception_ptr> errors(plan.files.size());

    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (std::size_t fi = 0; fi < plan.files.size(); ++fi) {
            scope.spawn([&, fi](CoroScope&) -> coro::CoroTask<void> {
                // The scope does not rethrow a spawned task's exception.
                try {
                    if (is_cancelled(plan)) co_return;
                    const auto& f = plan.files[fi];
                    const std::size_t ckpt =
                        checkpoint_size_or_default(f.checkpoint_size);
                    std::uint64_t uc_size = f.uncompressed_size;
                    std::size_t n_ckpts = f.num_checkpoints;
                    const std::string& index_path = f.index_path;

                    if (uc_size == 0 || n_ckpts == 0) {
                        trace::MetadataCollectorUtility meta;
                        auto md = co_await meta(
                            trace::MetadataCollectorUtilityInput::from_file(
                                f.file_path)
                                .with_checkpoint_size(ckpt)
                                .with_force_rebuild(false)
                                .with_index(index_path));
                        // Fail loudly: dropping the file would silently omit
                        // its events from the result.
                        if (!md.success)
                            throw DFTUtilsException(
                                ErrorCode::IO,
                                "gather_units: metadata read failed for " +
                                    f.file_path + ": " + md.error_message);
                        uc_size = md.uncompressed_size;
                        n_ckpts = md.num_checkpoints;
                    }

                    ViewPlannerInput pin;
                    pin.with_view(vdef)
                        .with_file_path(f.file_path)
                        .with_index_path(fs::exists(index_path) ? index_path
                                                                : "")
                        .with_uncompressed_size(uc_size)
                        .with_num_checkpoints(n_ckpts);
                    // Path-decoded files prune time through their time field.
                    if (plan.time_range && !vdef.by_path)
                        pin.with_time_range(plan.time_range->first,
                                            plan.time_range->second);

                    ViewPlannerUtility planner;
                    auto planned = co_await planner(pin);
                    // A planner error must propagate; file_may_match == false
                    // is a legitimate prune, so that one stays a quiet skip.
                    if (!planned) throw DFTUtilsException(planned.error());
                    skip_v[fi] = planned->skipped_checkpoints;
                    if (!planned->file_may_match) co_return;

                    auto& out = per_file[fi];
                    for (const auto& c : planned->candidates)
                        out.push_back(ScanUnit{f.file_path, index_path, ckpt,
                                               c.checkpoint_idx, c.start_byte,
                                               c.end_byte, 0, nullptr});
                } catch (...) {
                    errors[fi] = std::current_exception();
                }
            });
        }
        co_return;
    });

    for (const auto& e : errors)
        if (e) std::rethrow_exception(e);
    skipped_out = 0;
    for (auto s : skip_v) skipped_out += s;
    std::vector<ScanUnit> units;
    for (auto& v : per_file)
        for (auto& u : v) units.push_back(std::move(u));
    co_return units;
}

ScanShape scan_shape(const ViewDefinition& vdef) {
    ScanShape s;
    // A phase selector is folded into the query, so a ph-filtered scan reports
    // itself filtered without any user predicate - which is what artifacts
    // covering other phases need to see.
    s.filtered = vdef.query.has_value();
    // Path-decoded records have no metadata phase, so none is left out.
    s.include_metadata = vdef.include_metadata || vdef.by_path;
    return s;
}

namespace {

// True if any file's index still lacks the pruner bloom (or has no index yet).
// Bloom presence means the index was fully built, so once every file has it the
// ride-along stops attaching and a repeat scan pays no re-parse.
bool any_file_missing_bloom(const ViewPlan& plan) {
    for (const auto& f : plan.files) {
        try {
            dftracer::utils::index::store::IndexDatabase db(
                f.index_path,
                dftracer::utils::index::store::IndexOpenMode::ReadOnly);
            int fid = db.get_file_info_id(
                dftracer::utils::index::store::internal::get_logical_path(
                    f.file_path));
            if (fid < 0 || !db.pruning_tier_current(fid)) return true;
        } catch (const std::exception&) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<std::unique_ptr<Fold>> select_index_folds(
    const ViewPlan& plan, const ViewDefinition& vdef,
    dftracer::utils::StringIntern& intern) {
    std::vector<std::unique_ptr<Fold>> kept;
    if (plan.files.empty() || !any_file_missing_bloom(plan)) return kept;

    const ScanShape shape = scan_shape(vdef);
    auto bloom =
        std::make_unique<dftracer::utils::index::extensions::BloomFold>(
            intern, dftracer::utils::index::build::for_schema(
                        {}, plan_record_schema(plan)));
    if (bloom->accepts(shape)) kept.push_back(std::move(bloom));
    return kept;
}

coro::CoroTask<ExportStats> for_each_scanned_batch(
    const ViewPlan& plan, const ViewDefinition& vdef, std::size_t num_slots,
    std::uint64_t limit,
    const std::function<void(std::size_t,
                             const std::vector<std::string_view>&)>& on_batch,
    std::span<Fold* const> folds, dftracer::utils::StringIntern* intern) {
    std::uint64_t skipped = 0;
    auto units = co_await gather_units(plan, vdef, skipped);
    // 0 means one worker per unit (max parallelism); otherwise cap the fan-out.
    if (num_slots == 0) num_slots = std::max<std::size_t>(1, units.size());

    const std::uint64_t cap =
        limit > 0 ? limit : std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::size_t> next_unit{0};
    std::vector<ScanCounts> counts_v(num_slots);
    const std::size_t nworkers = std::min(num_slots, units.size());
    std::vector<CoverageSet> covered_v(nworkers);

    bool any_needs_args = false;
    bool any_wants_schema = false;
    bool any_reads_every_field = false;
    for (auto* f : folds) {
        any_needs_args |= f->needs_args();
        any_wants_schema |= f->wants_schema();
        any_reads_every_field |= f->reads_every_field();
    }
    // One slice of every fold per worker, owned here so a slice outlives the
    // coroutine that filled it; merged after the join.
    std::vector<std::vector<std::unique_ptr<Fold>>> fslice(nworkers);
    if (!folds.empty())
        for (std::size_t w = 0; w < nworkers; ++w)
            for (auto* f : folds) fslice[w].push_back(f->slice());

    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (std::size_t w = 0; w < nworkers; ++w) {
            // Worker `w` hands batches to slot `w`; a coroutine never runs
            // concurrently with itself, so per-slot state needs no lock.
            scope.spawn([&, w](CoroScope&) -> coro::CoroTask<void> {
                // Sealed per unit, so a cap or cancel cutting the current unit
                // short drops only that one. Draining the batch loop is the
                // sole path to a claim; every break leaves the unit pending.
                PendingCoverage pending(covered_v[w]);
                auto& slices = fslice[w];
                dftracer::utils::json::RecordParser parser;
                std::string parse_buf;
                std::vector<FoldEvent> fold_events;
                for (;;) {
                    if (produced.load(std::memory_order_relaxed) >= cap) break;
                    if (is_cancelled(plan)) break;
                    std::size_t i =
                        next_unit.fetch_add(1, std::memory_order_relaxed);
                    if (i >= units.size()) break;
                    ViewScannerInput sin =
                        make_scanner_input(units[i], vdef, vdef.query);
                    ViewScannerUtility scanner;
                    auto gen = scanner(sin);
                    bool complete = true;
                    while (auto b = co_await gen.next()) {
                        if (b->events.empty()) {
                            counts_v[w].add(*b);
                            continue;
                        }
                        if (produced.load(std::memory_order_relaxed) >= cap) {
                            complete = false;
                            break;
                        }
                        if (is_cancelled(plan)) {
                            complete = false;
                            break;
                        }
                        counts_v[w].add(*b);
                        on_batch(w, b->events);
                        if (!slices.empty()) {
                            // The scan buffer carries no simdjson padding, so
                            // parse each event into a reused padded buffer;
                            // owned FoldEvents let the folds outlive it.
                            fold_events.clear();
                            for (auto line : b->events) {
                                parse_buf.assign(line);
                                parse_buf.resize(
                                    line.size() + simdjson::SIMDJSON_PADDING,
                                    '\0');
                                auto doc = parser.parse(parse_buf.data(),
                                                        line.size(), false);
                                if (doc.error()) continue;
                                fold_events.push_back(
                                    vdef.by_path
                                        ? decode_record(
                                              doc.value_unsafe(), *intern,
                                              any_wants_schema,
                                              any_wants_schema ||
                                                      any_reads_every_field ||
                                                      vdef.paths.empty()
                                                  ? nullptr
                                                  : &vdef.paths,
                                              vdef.record_schema,
                                              &vdef.path_fields)
                                        : extract_fold_event(
                                              doc.value_unsafe(), *intern,
                                              any_needs_args, nullptr,
                                              any_wants_schema));
                            }
                            FoldBatch fb{
                                std::span<const FoldEvent>(fold_events),
                                units[i]};
                            for (auto& s : slices) s->step(fb);
                        }
                        produced.fetch_add(b->events.size(),
                                           std::memory_order_relaxed);
                    }
                    if (!complete) {
                        for (auto& s : slices) s->drop_unit(units[i]);
                        break;
                    }
                    for (auto& s : slices) s->seal_unit(units[i]);
                    pending.mark_complete(units[i].file_path,
                                          units[i].checkpoint_idx);
                    pending.seal();
                }
                co_return;
            });
        }
        co_return;
    });

    CoverageSet covered;
    for (auto& c : covered_v) covered.absorb(std::move(c));
    // Captured before whole-file coverage is added below, which would inflate
    // the count.
    const std::size_t member_units = covered.size();

    // A file is whole only if nothing was pruned away and every one of its
    // units sealed. Pruning is decided inside gather_units, after the folds
    // were attached, so it cannot be a requirement they declare - it has to
    // land here, as a coverage fact.
    if (skipped == 0) {
        std::unordered_map<std::string_view,
                           std::pair<std::size_t, std::size_t>>
            per_file;
        for (const auto& u : units) {
            auto& c = per_file[u.file_path];
            ++c.first;
            if (covered.covers(u.file_path, u.checkpoint_idx)) ++c.second;
        }
        for (const auto& [file, c] : per_file)
            if (c.first == c.second) covered.add_file(file);
    }

    for (std::size_t w = 0; w < nworkers; ++w)
        for (std::size_t k = 0; k < folds.size(); ++k)
            folds[k]->merge(*fslice[w][k]);

    ExportStats st;
    st.chunks_skipped = skipped;
    st.chunks_scanned = units.size();
    st.chunks_covered = member_units;
    for (const auto& c : counts_v) c.add_to(st);
    st.truncated = produced.load(std::memory_order_relaxed) >= cap;
    for (auto* f : folds)
        if (co_await f->finalize(covered)) st.artifacts_committed = true;
    co_return st;
}

}  // namespace dftracer::utils::trace::views::detail
