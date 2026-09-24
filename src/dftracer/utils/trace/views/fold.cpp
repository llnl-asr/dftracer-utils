#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/store/file.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/view_aggregate.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

namespace dftracer::utils::trace::views::detail {

void DynamicPrune::exclude_file(const std::string& file_path) {
    std::lock_guard<std::mutex> lk(mu_);
    excluded_files_.insert(file_path);
}

void DynamicPrune::exclude_checkpoints(const std::string& file_path,
                                       std::vector<std::uint64_t> checkpoints) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& set = excluded_checkpoints_[file_path];
    for (std::uint64_t c : checkpoints) set.insert(c);
}

bool DynamicPrune::is_excluded(const std::string& file_path,
                               std::uint64_t checkpoint_idx) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (excluded_files_.count(file_path)) return true;
    auto it = excluded_checkpoints_.find(file_path);
    if (it == excluded_checkpoints_.end()) return false;
    return it->second.count(checkpoint_idx) != 0;
}

namespace {

struct ScanPermit {
    coro::CoroSemaphore& sem;
    std::uint64_t bytes;
    ScanPermit(coro::CoroSemaphore& s, std::uint64_t n) : sem(s), bytes(n) {}
    ~ScanPermit() { sem.release(bytes); }
    ScanPermit(const ScanPermit&) = delete;
    ScanPermit& operator=(const ScanPermit&) = delete;
};

std::uint32_t intern_string(dftracer::utils::StringIntern& intern,
                            simdjson::dom::element val) {
    if (!val.is_string()) return dftracer::utils::StringIntern::NO_ID;
    return intern.get_or_insert(val.get_string().value_unsafe());
}

std::uint8_t leaf_type_tag(simdjson::dom::element v) {
    using dftracer::utils::index::store::PathType;
    PathType t = PathType::NULL_VALUE;
    if (v.is_string())
        t = PathType::STRING;
    else if (v.is_int64())
        t = PathType::INT;
    else if (v.is_uint64())
        t = PathType::UINT;
    else if (v.is_double())
        t = PathType::DOUBLE;
    else if (v.is_bool())
        t = PathType::BOOL;
    return static_cast<std::uint8_t>(t);
}

// Walk `v` to every scalar or null leaf, emitting (interned dotted path ->
// PathType) into ev.schema_leaves. Objects recurse by key (a.b.c), arrays by
// index (a.0.b), so the harvest lists every column a row fold emits. `path` is
// a reused buffer (restored on return).
void enumerate_leaves(std::string& path, simdjson::dom::element v,
                      dftracer::utils::StringIntern& intern, FoldEvent& ev) {
    simdjson::dom::object obj;
    if (v.get_object().get(obj) == simdjson::SUCCESS) {
        for (auto kv : obj) {
            const std::size_t base = path.size();
            if (base) path.push_back('.');
            path.append(kv.key);
            enumerate_leaves(path, kv.value, intern, ev);
            path.resize(base);
        }
        return;
    }
    simdjson::dom::array arr;
    if (v.get_array().get(arr) == simdjson::SUCCESS) {
        std::size_t i = 0;
        for (auto el : arr) {
            const std::size_t base = path.size();
            if (base) path.push_back('.');
            dftracer::utils::trace::detail::append_index(path, i++);
            enumerate_leaves(path, el, intern, ev);
            path.resize(base);
        }
        return;
    }
    if (!path.empty())
        ev.schema_leaves.emplace_back(intern.get_or_insert(path),
                                      leaf_type_tag(v));
}

// Flatten a nested arg into dotted scalar keys named as event.h flatten_arg
// names them: objects "a.b", arrays "a.0". `path` holds the key so far.
void flatten_nested_arg(std::string& path, simdjson::dom::element v,
                        dftracer::utils::StringIntern& intern, FoldEvent& ev) {
    simdjson::dom::object obj;
    if (v.get_object().get(obj) == simdjson::SUCCESS) {
        for (auto kv : obj) {
            const std::size_t base = path.size();
            path.push_back('.');
            path.append(kv.key);
            flatten_nested_arg(path, kv.value, intern, ev);
            path.resize(base);
        }
        return;
    }
    simdjson::dom::array arr;
    if (v.get_array().get(arr) == simdjson::SUCCESS) {
        std::size_t i = 0;
        for (auto el : arr) {
            const std::size_t base = path.size();
            path.push_back('.');
            dftracer::utils::trace::detail::append_index(path, i++);
            flatten_nested_arg(path, el, intern, ev);
            path.resize(base);
        }
        return;
    }
    append_scalar_arg(ev.args, intern.get_or_insert(path), v, intern);
}

}  // namespace

void FoldPortBus::publish(std::uint64_t key, const void* data,
                          std::uint32_t len) {
    const std::size_t offset = arena_.size();
    const auto* p = static_cast<const std::byte*>(data);
    arena_.insert(arena_.end(), p, p + len);
    index_[key] = Entry{offset, len};
}

const void* FoldPortBus::consume(std::uint64_t key,
                                 std::uint32_t* out_len) const {
    auto it = index_.find(key);
    if (it == index_.end()) {
        if (out_len) *out_len = 0;
        return nullptr;
    }
    if (out_len) *out_len = it->second.len;
    return arena_.data() + it->second.offset;
}

void FoldPortBus::clear() {
    index_.clear();
    arena_.clear();
    frame.reset();
}

FoldEvent build_fold_event(const DFTracerEvent& scalars,
                           simdjson::dom::element args, bool has_args,
                           dftracer::utils::StringIntern& intern,
                           bool needs_args) {
    FoldEvent ev;
    ev.phase = scalars.phase;
    ev.pid = scalars.pid;
    ev.tid = scalars.tid;
    ev.ts = scalars.ts;
    ev.dur = scalars.dur;
    ev.has_dur = scalars.has_dur;
    if (!scalars.cat.empty()) ev.cat_id = intern.get_or_insert(scalars.cat);
    if (!scalars.name.empty()) ev.name_id = intern.get_or_insert(scalars.name);

    if (!has_args) return ev;

    // fhash/hhash are group dimensions even for folds that want no other args,
    // so they are always harvested; the rest only when a fold asks.
    for (auto field : args.get_object()) {
        std::string_view key = field.key;
        if (key == "fhash") {
            ev.fhash_id = intern_string(intern, field.value);
        } else if (key == "hhash") {
            ev.hhash_id = intern_string(intern, field.value);
        } else if (needs_args) {
            const simdjson::dom::element v = field.value;
            switch (v.type()) {
                case simdjson::dom::element_type::OBJECT:
                case simdjson::dom::element_type::ARRAY: {
                    std::string path(key);
                    flatten_nested_arg(path, v, intern, ev);
                    break;
                }
                default:
                    append_scalar_arg(ev.args, intern.get_or_insert(key), v,
                                      intern);
            }
        }
    }
    return ev;
}

FoldEvent extract_fold_event(simdjson::dom::element root,
                             dftracer::utils::StringIntern& intern,
                             bool needs_args,
                             const std::vector<std::string>* extra_fields,
                             bool capture_schema) {
    DFTracerEvent scalars;
    simdjson::dom::element args;
    bool has_args = false;
    if (!DFTracerEvent::parse_scalars(root, scalars, args, has_args))
        return FoldEvent{};
    FoldEvent ev =
        build_fold_event(scalars, args, has_args, intern, needs_args);
    if (extra_fields)
        for (const auto& name : *extra_fields)
            capture_extra_field(ev, root, intern, name);
    if (capture_schema) capture_schema_leaves(ev, root, intern);
    return ev;
}

namespace {

// The leaves of `v` under exact path `path`: scalars into ev.args and, with
// `schema`, every leaf, null included, into ev.schema_leaves. With `paths`
// (sorted), only those leaves, descending only toward them and stopping once
// `left` of them remain to find.
struct LeafDecode {
    dftracer::utils::StringIntern& intern;
    FoldEvent& ev;
    bool schema;
    const std::vector<std::string>* paths;
    std::size_t left;
    const dftracer::utils::index::RecordSchema* record_schema;
    // With `paths`, the declared field at each path (null when undeclared).
    const std::vector<const dftracer::utils::index::FieldSpec*>* path_fields;
    // Whether a json field can be decoded, so objects and arrays are checked.
    bool json;

    // The declared field at `path`, found at `pos` in `paths` when set.
    const dftracer::utils::index::FieldSpec* field(const std::string& path,
                                                   std::size_t pos) const {
        if (path_fields) return (*path_fields)[pos];
        return record_schema ? record_schema->field_at(path) : nullptr;
    }
};

// The position of `path` in the sorted `paths`, or nullopt.
std::optional<std::size_t> position(const std::vector<std::string>& paths,
                                    const std::string& path) {
    const auto it = std::lower_bound(paths.begin(), paths.end(), path);
    if (it == paths.end() || *it != path) return std::nullopt;
    return static_cast<std::size_t>(it - paths.begin());
}

// A declared field's value as its type: integers stay integers, whole
// doubles become integers, numbers become doubles for a float field, and
// strings stay strings; any other value is left out, so it reads as null.
// Role fields also set the event's time, duration or lane in microseconds.
void bind_field(const dftracer::utils::index::FieldSpec& f, std::uint32_t id,
                simdjson::dom::element v, LeafDecode& d) {
    namespace ix = dftracer::utils::index;
    using T = simdjson::dom::element_type;
    const T type = v.type();
    const bool numeric =
        type == T::INT64 || type == T::UINT64 || type == T::DOUBLE;
    double num = 0;
    if (numeric) num = v.get_double().value_unsafe();
    switch (f.type) {
        case ix::FieldType::STRING:
            if (type != T::STRING) return;
            append_scalar_arg(d.ev.args, id, v, d.intern);
            break;
        case ix::FieldType::BOOL:
            if (type != T::BOOL) return;
            append_scalar_arg(d.ev.args, id, v, d.intern);
            break;
        case ix::FieldType::FLOAT:
            if (!numeric) return;
            d.ev.args.emplace_back(id, num);
            break;
        case ix::FieldType::JSON: {
            std::string text;
            dftracer::utils::json::append_canonical_json(text, v);
            d.ev.args.emplace_back(id, d.intern.get_or_insert(text));
            return;
        }
        case ix::FieldType::INT:
            if (!numeric) return;
            if (type != T::DOUBLE) {
                append_scalar_arg(d.ev.args, id, v, d.intern);
            } else {
                const auto whole = ix::whole_int64(num);
                if (!whole) return;
                d.ev.args.emplace_back(id, *whole);
            }
            break;
    }
    const double micros =
        ix::micros_per(f.unit.value_or(ix::TimeUnit::US)) * num;
    switch (f.role) {
        case ix::Role::NONE:
            break;
        case ix::Role::TIME:
            if (micros >= 0)
                d.ev.ts = static_cast<std::uint64_t>(std::llround(micros));
            break;
        case ix::Role::DURATION:
            if (micros >= 0) {
                d.ev.dur = static_cast<std::uint64_t>(std::llround(micros));
                d.ev.has_dur = true;
            }
            break;
        case ix::Role::ENTITY: {
            const auto& value = d.ev.args.back().second;
            if (const auto* i = std::get_if<std::int64_t>(&value))
                d.ev.pid = static_cast<std::uint64_t>(*i);
            else if (const auto* sid = std::get_if<std::uint32_t>(&value))
                d.ev.pid = *sid;
            break;
        }
    }
}

bool wanted_below(const std::vector<std::string>& paths,
                  const std::string& path) {
    auto it = std::lower_bound(paths.begin(), paths.end(), path);
    for (; it != paths.end() && it->starts_with(path); ++it)
        if (it->size() > path.size() && (*it)[path.size()] == '.') return true;
    return false;
}

// A declared json field at an object or array: its canonical text, keyed by
// the field's path; the leaves below are decoded as well.
void capture_json(const std::string& path, simdjson::dom::element v,
                  LeafDecode& d) {
    std::size_t pos = 0;
    if (d.paths) {
        const auto at = position(*d.paths, path);
        if (!at) return;
        pos = *at;
    }
    const auto* f = d.field(path, pos);
    if (!f || f->type != dftracer::utils::index::FieldType::JSON) return;
    if (d.paths) --d.left;
    std::string text;
    dftracer::utils::json::append_canonical_json(text, v);
    d.ev.args.emplace_back(d.intern.get_or_insert(path),
                           d.intern.get_or_insert(text));
}

void decode_leaves(std::string& path, simdjson::dom::element v, LeafDecode& d) {
    if (d.paths && d.left == 0) return;
    if (d.json && !path.empty() && (v.is_object() || v.is_array()))
        capture_json(path, v, d);
    simdjson::dom::object obj;
    if (v.get_object().get(obj) == simdjson::SUCCESS) {
        if (d.paths && !path.empty() && !wanted_below(*d.paths, path)) return;
        for (auto kv : obj) {
            const std::size_t base = path.size();
            if (base) path.push_back('.');
            path.append(kv.key);
            decode_leaves(path, kv.value, d);
            path.resize(base);
            if (d.paths && d.left == 0) return;
        }
        return;
    }
    simdjson::dom::array arr;
    if (v.get_array().get(arr) == simdjson::SUCCESS) {
        if (d.paths && !path.empty() && !wanted_below(*d.paths, path)) return;
        std::size_t i = 0;
        for (auto el : arr) {
            const std::size_t base = path.size();
            if (base) path.push_back('.');
            dftracer::utils::trace::detail::append_index(path, i++);
            decode_leaves(path, el, d);
            path.resize(base);
            if (d.paths && d.left == 0) return;
        }
        return;
    }
    if (path.empty()) return;
    std::size_t pos = 0;
    if (d.paths) {
        const auto at = position(*d.paths, path);
        if (!at) return;
        pos = *at;
        --d.left;
    }
    const std::uint32_t id = d.intern.get_or_insert(path);
    if (d.schema) d.ev.schema_leaves.emplace_back(id, leaf_type_tag(v));
    if (v.is_null()) return;
    if (d.record_schema)
        if (const auto* f = d.field(path, pos)) {
            bind_field(*f, id, v, d);
            return;
        }
    append_scalar_arg(d.ev.args, id, v, d.intern);
}

}  // namespace

FoldEvent decode_record(
    simdjson::dom::element root, dftracer::utils::StringIntern& intern,
    bool capture_schema, const std::vector<std::string>* paths,
    const dftracer::utils::index::RecordSchema* record_schema,
    const std::vector<const dftracer::utils::index::FieldSpec*>* path_fields) {
    namespace ix = dftracer::utils::index;
    FoldEvent ev;
    ev.phase = RecordPhase::COMPLETE;
    ev.by_path = true;
    const auto* fields = paths ? path_fields : nullptr;
    bool json = record_schema && record_schema->has_json;
    if (json && fields)
        json = std::any_of(fields->begin(), fields->end(),
                           [](const ix::FieldSpec* f) {
                               return f && f->type == ix::FieldType::JSON;
                           });
    LeafDecode d{intern,
                 ev,
                 capture_schema,
                 paths,
                 paths ? paths->size() : 0,
                 record_schema,
                 fields,
                 json};
    std::string path;
    decode_leaves(path, root, d);
    return ev;
}

void capture_schema_leaves(FoldEvent& ev, simdjson::dom::element root,
                           dftracer::utils::StringIntern& intern) {
    // The axis and structural keys are not catalog paths.
    simdjson::dom::object obj;
    if (root.get_object().get(obj) != simdjson::SUCCESS) return;
    std::string path;
    for (auto kv : obj) {
        const std::string_view k = kv.key;
        if (k == "pid" || k == "tid" || k == "ts" || k == "dur" || k == "ph" ||
            k == "id")
            continue;
        path.assign(k);
        enumerate_leaves(path, kv.value, intern, ev);
    }
}

std::vector<std::string> extra_capture_fields(const ViewPlan& plan) {
    // Args come from needs_args, nested ones flattened; only top-level fields
    // the POD does not carry (anything but the six scalars) and nested paths
    // need capture, since a fold may not set needs_args.
    auto is_pod_scalar = [](const std::string& f) {
        return f == "name" || f == "cat" || f == "pid" || f == "tid" ||
               f == "ts" || f == "dur";
    };
    std::vector<std::string> out;
    auto add = [&](const std::string& f) {
        if (f.empty()) return;
        for (const auto& e : out)
            if (e == f) return;
        out.push_back(f);
    };
    for (const auto& gk : plan.group_by)
        if (gk.kind == GroupKey::Kind::Field && !is_pod_scalar(gk.arg))
            add(gk.arg);
        else if (gk.kind == GroupKey::Kind::Resolved) {
            const std::string_view key = resolved_key_field(gk);
            if (key != "fhash" && key != "hhash") add(std::string(key));
        }
    for (const auto& c : plan.computed)
        for (const auto& in : c.inputs)
            if (is_nested_path(in)) add(in);
    for (const auto& spec : plan.agg) {
        if (is_nested_path(spec.field)) add(spec.field);
        if (spec.op == AggOp::ArgMax && is_nested_path(spec.by)) add(spec.by);
    }
    return out;
}

namespace {

// Shared, worker-agnostic state for one fuse() run. Reference members point at
// fuse()'s locals, which outlive every worker (run_coro_scope joins before
// fuse returns). Passed by value into fuse_worker so the coroutine frame owns
// its copy of the (trivially copyable) handles.
struct FuseWorkerCtx {
    std::vector<std::vector<std::unique_ptr<Fold>>>& fslice;
    const std::vector<ScanUnit>& units;
    std::atomic<std::size_t>& next_unit;
    std::atomic<std::uint64_t>& produced;
    std::uint64_t cap;
    const ViewPlan& plan;
    const ViewDefinition& vdef;
    dftracer::utils::StringIntern& intern;
    const CoverageSet* covered;
    coro::CoroSemaphore& budget_sem;
    std::vector<std::uint64_t>& matched_v;
    std::vector<std::uint64_t>& scanned_v;
    std::vector<CoverageSet>& covered_v;
    const std::vector<std::string>& extra_fields;
    bool any_needs_args;
    bool any_wants_raw;
    bool any_wants_fold_event;
    bool any_wants_schema;
    DynamicPrune* dyn_prune;
};

// One fuse worker: drains the shared unit queue, decodes each unit, and pushes
// its batches into worker w's fold slices. Named (not a capturing-lambda
// coroutine) to avoid the GCC 12/13 coroutine-frame miscompile on non-trivial
// frame locals (the scanner/generator/batch below). Behavior is identical to
// the previous inline lambda.
coro::CoroTask<void> fuse_worker(FuseWorkerCtx ctx, std::size_t w) {
    auto& slices = ctx.fslice[w];
    FoldPortBus bus;
    for (auto& s : slices) s->bind_port_bus(&bus);
    PendingCoverage pending(ctx.covered_v[w]);

    for (;;) {
        if (is_cancelled(ctx.plan)) break;
        if (ctx.produced.load(std::memory_order_relaxed) >= ctx.cap) break;
        std::size_t i = ctx.next_unit.fetch_add(1, std::memory_order_relaxed);
        if (i >= ctx.units.size()) break;
        // A materialized aggregate already answered this chunk.
        if (ctx.covered && ctx.covered->covers(ctx.units[i].file_path,
                                               ctx.units[i].checkpoint_idx))
            continue;
        // A narrow() call mid-scan ruled this chunk out after gather_units
        // already listed it as a candidate.
        if (ctx.dyn_prune &&
            ctx.dyn_prune->is_excluded(ctx.units[i].file_path,
                                       ctx.units[i].checkpoint_idx)) {
            ctx.dyn_prune->record_skip();
            continue;
        }

        const std::uint64_t reserve =
            ctx.units[i].end_byte > ctx.units[i].start_byte
                ? ctx.units[i].end_byte - ctx.units[i].start_byte
                : 0;
        co_await ctx.budget_sem.acquire(reserve);
        ScanPermit permit{ctx.budget_sem, reserve};

        ViewScannerInput sin =
            make_scanner_input(ctx.units[i], ctx.vdef, ctx.vdef.query);
        // A FoldEvent fold gets the parsed stream; a raw fold parses the lines
        // itself. When both are present, keep both.
        sin.fold_intern = ctx.any_wants_fold_event ? &ctx.intern : nullptr;
        sin.fold_needs_args = ctx.any_needs_args;
        sin.fold_keep_raw = ctx.any_wants_raw && ctx.any_wants_fold_event;
        sin.fold_capture_schema = ctx.any_wants_schema;
        if (!ctx.extra_fields.empty())
            sin.fold_extra_fields = &ctx.extra_fields;
        ViewScannerUtility scanner;
        auto gen = scanner(sin);
        bool complete = true;
        while (auto b = co_await gen.next()) {
            if (is_cancelled(ctx.plan)) {
                complete = false;
                break;
            }
            if (ctx.produced.load(std::memory_order_relaxed) >= ctx.cap) {
                complete = false;
                break;
            }
            ctx.matched_v[w] += b->events_matched;
            ctx.scanned_v[w] += b->events_scanned;
            if (b->fold_events.empty() && b->events.empty()) continue;
            ctx.produced.fetch_add(b->fold_events.size() + b->events.size(),
                                   std::memory_order_relaxed);
            FoldBatch fb{std::span<const FoldEvent>(b->fold_events),
                         ctx.units[i],
                         std::span<const std::string_view>(b->events)};
            bus.clear();
            for (auto& s : slices) {
                s->step(fb);
                // Await an async plugin fold's task before the next step
                // recycles the batch; null for sync folds.
                if (auto* t = s->take_pending())
                    co_await *reinterpret_cast<coro::CoroTask<void>*>(t);
            }
        }
        if (!complete) {
            for (auto& s : slices) s->drop_unit(ctx.units[i]);
            break;
        }
        for (auto& s : slices) s->seal_unit(ctx.units[i]);
        pending.mark_complete(ctx.units[i].file_path,
                              ctx.units[i].checkpoint_idx);
        pending.seal();
    }
    co_return;
}

}  // namespace

coro::CoroTask<ExportStats> fuse(const ViewPlan& plan,
                                 const ViewDefinition& vdef_in,
                                 std::span<Fold* const> folds,
                                 dftracer::utils::StringIntern& intern,
                                 const CoverageSet* covered,
                                 std::uint64_t limit, DynamicPrune* dyn_prune) {
    // Folds that all drop metadata records need no chunk read for them.
    ViewDefinition vdef = vdef_in;
    if (std::none_of(folds.begin(), folds.end(),
                     [](const Fold* f) { return f->wants_metadata(); })) {
        vdef.include_metadata = false;
        vdef.emit_all_metadata = false;
    }
    std::uint64_t skipped = 0;
    auto units = co_await gather_units(plan, vdef, skipped);
    const std::uint64_t cap =
        limit > 0 ? limit : std::numeric_limits<std::uint64_t>::max();
    std::atomic<std::uint64_t> produced{0};

    ExportStats st;
    st.chunks_scanned = units.size();
    if (units.empty() || folds.empty()) {
        st.chunks_skipped = skipped;
        for (auto* f : folds) co_await f->finalize(CoverageSet{});
        co_return st;
    }

    bool any_needs_args = false;
    bool any_wants_raw = false;
    bool any_wants_fold_event = false;
    bool any_wants_schema = false;
    for (auto* f : folds) {
        any_needs_args |= f->needs_args();
        any_wants_schema |= f->wants_schema();
        if (f->wants_raw())
            any_wants_raw = true;
        else
            any_wants_fold_event = true;
    }

    std::vector<std::string> extra_fields = extra_capture_fields(plan);
    // Fold-declared captures (nested fields a fold resolves itself) merged in.
    for (auto* f : folds)
        for (const std::string& c : f->extra_captures()) {
            bool seen = false;
            for (const std::string& e : extra_fields)
                if (e == c) {
                    seen = true;
                    break;
                }
            if (!seen) extra_fields.push_back(c);
        }

    // Coarse fan-out: one worker coroutine per runtime slot draining the shared
    // unit queue, so under the elastic runtime live threads grow toward the
    // cap.
    const std::size_t nworkers =
        std::min<std::size_t>(units.size(), available_parallelism());
    // Bound bytes decoded concurrently so a scan cannot OOM the box. Always on:
    // an explicit memory_budget() or a RAM-fraction default. Workers park when
    // full, so a generous budget never throttles.
    coro::CoroSemaphore budget_sem(compute_memory_budget(plan.memory_budget));
    std::atomic<std::size_t> next_unit{0};
    std::vector<std::uint64_t> matched_v(nworkers, 0), scanned_v(nworkers, 0);
    std::vector<CoverageSet> covered_v(nworkers);
    // Per-worker slice of every fold, owned here so a slice outlives the
    // coroutine that filled it; merged into the shared folds after the join.
    std::vector<std::vector<std::unique_ptr<Fold>>> fslice(nworkers);
    for (std::size_t w = 0; w < nworkers; ++w)
        for (auto* f : folds) fslice[w].push_back(f->slice());

    FuseWorkerCtx ctx{fslice,
                      units,
                      next_unit,
                      produced,
                      cap,
                      plan,
                      vdef,
                      intern,
                      covered,
                      budget_sem,
                      matched_v,
                      scanned_v,
                      covered_v,
                      extra_fields,
                      any_needs_args,
                      any_wants_raw,
                      any_wants_fold_event,
                      any_wants_schema,
                      dyn_prune};
    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        for (std::size_t w = 0; w < nworkers; ++w)
            scope.spawn([&ctx, w](CoroScope&) -> coro::CoroTask<void> {
                return fuse_worker(ctx, w);
            });
        co_return;
    });

    for (std::size_t w = 0; w < nworkers; ++w)
        for (std::size_t k = 0; k < folds.size(); ++k)
            folds[k]->merge(*fslice[w][k]);

    const std::uint64_t dyn_skipped = dyn_prune ? dyn_prune->skipped() : 0;
    st.chunks_skipped = skipped + dyn_skipped;

    CoverageSet scanned;
    for (auto& c : covered_v) scanned.absorb(std::move(c));

    // A file is whole only if nothing was pruned (statically or by a
    // narrow() call mid-scan) and every unit of it sealed.
    if (skipped == 0 && dyn_skipped == 0) {
        std::unordered_map<std::string_view,
                           std::pair<std::size_t, std::size_t>>
            per_file;
        for (const auto& u : units) {
            auto& c = per_file[u.file_path];
            ++c.first;
            if (scanned.covers(u.file_path, u.checkpoint_idx)) ++c.second;
        }
        for (const auto& [file, c] : per_file)
            if (c.first == c.second) scanned.add_file(file);
    }

    for (auto* f : folds) co_await f->finalize(scanned);

    for (auto m : matched_v) st.events_matched += m;
    for (auto s : scanned_v) st.events_scanned += s;
    st.truncated = produced.load(std::memory_order_relaxed) >= cap;
    co_return st;
}

}  // namespace dftracer::utils::trace::views::detail
