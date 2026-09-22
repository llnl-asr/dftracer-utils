// dftu_provider_register/unregister and dftu_lazyframe_from_provider: a
// process-lifetime, name-keyed registry of Source vtables, mirroring the op
// registry in op_registry.cpp. dftu.svc.providers@0 (plugins/abi/providers.h)
// is a thin gated forwarder into this registry, but the registry itself is
// not plugin-scoped: a non-plugin C caller, or a future language binding,
// registers and reads it the same way a plugin does.

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/internal/dataframe_handle.h>
#include <dftracer/utils/dataframe/internal/expr_handle.h>
#include <dftracer/utils/dataframe/internal/lazyframe_handle.h>
#include <dftracer/utils/dataframe/internal/provider_source.h>
#include <dftracer/utils/dataframe/internal/schema_types.h>
#include <dftracer/utils/dataframe/lazyframe.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dftracer::utils::dataframe {
namespace {

struct ProviderEntry {
    ::dftu_source_vt vt;
    void* self;
};

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, ProviderEntry>& providers() {
    static std::unordered_map<std::string, ProviderEntry> m;
    return m;
}

std::optional<ProviderEntry> find_provider(const char* name) {
    if (!name) return std::nullopt;
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto it = providers().find(name);
    if (it == providers().end()) return std::nullopt;
    return it->second;
}

std::string describe(const ::dftu_error& err) {
    return err.message && *err.message ? err.message : "provider cursor failed";
}

// Any dftu_pushed value the host has not defined is untrusted and treated as
// DFTU_PUSHED_NO (always sound: the engine re-applies) rather than trusted,
// since a source has no way to prove an Exact claim at runtime.
Pushed to_pushed(std::int32_t v) {
    switch (v) {
        case DFTU_PUSHED_NO:
            return Pushed::No;
        case DFTU_PUSHED_INEXACT:
            return Pushed::Inexact;
        case DFTU_PUSHED_EXACT:
            return Pushed::Exact;
        default:
            return Pushed::No;
    }
}

std::string join_names(const std::vector<std::string>& names) {
    std::ostringstream out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out << ", ";
        out << names[i];
    }
    return out.str();
}

// A type this ABI lets a provider nest a child field under: DataType::fields
// holds one entry for List/LargeList/FixedSizeList/Map (the element or
// key/value entry type) and any number for Struct.
bool is_nestable(TypeId id) {
    switch (id) {
        case TypeId::List:
        case TypeId::LargeList:
        case TypeId::FixedSizeList:
        case TypeId::Struct:
        case TypeId::Map:
            return true;
        default:
            return false;
    }
}

/// How many children `id` accepts: a Struct takes any number, every other
/// nestable takes exactly one (the element, or a Map's entries).
bool accepts_another_child(const DataType& dt) {
    if (!is_nestable(dt.id)) return false;
    return dt.id == TypeId::Struct || dt.fields.empty();
}

Field to_field(const char* name, ::dftu_dtype type, std::int32_t nullable,
               ::dftu_time_unit time_unit, const char* tz,
               std::int32_t decimal_precision, std::int32_t decimal_scale,
               std::int32_t fixed_size) {
    Field f;
    f.name = name ? name : "";
    f.nullable = nullable != 0;
    f.type.id = static_cast<TypeId>(type);
    f.type.time_unit = static_cast<TimeUnit>(time_unit);
    if (tz) f.type.timezone = tz;
    f.type.decimal_precision = decimal_precision;
    f.type.decimal_scale = decimal_scale;
    f.type.fixed_size = fixed_size;
    return f;
}

// Resolves `schema->paths[index]` against `schema->top`, or NULL if `index`
// is out of range. Recomputed from the root on every call rather than
// cached, since a sibling insertion earlier in the same schema may have
// reallocated any vector along the path.
Field* resolve_field(::dftu_schema* schema, std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= schema->paths.size())
        return nullptr;
    const std::vector<std::int32_t>& path =
        schema->paths[static_cast<std::size_t>(index)];
    Field* f = &schema->top[static_cast<std::size_t>(path[0])];
    for (std::size_t i = 1; i < path.size(); ++i)
        f = &f->type.fields[static_cast<std::size_t>(path[i])];
    return f;
}

// A schema is well-formed only if every nested field count matches what its
// type allows: exactly one for List/LargeList/FixedSizeList/Map, any number
// for Struct, none for a scalar. A provider that gets this wrong (e.g. two
// children under a List) gets the whole declared schema discarded rather
// than a silently truncated or misinterpreted one.
bool is_well_formed(const DataType& dt) {
    if (dt.id == TypeId::Struct) {
        for (const Field& f : dt.fields)
            if (!is_well_formed(f.type)) return false;
        return true;
    }
    if (is_nestable(dt.id))
        return dt.fields.size() == 1 && is_well_formed(dt.fields[0].type);
    return dt.fields.empty();
}

// Converts a provider's declared dftu_schema into a Schema, but only if it
// names exactly `names`, in order, and every field is well-formed; otherwise
// returns nullopt so the caller falls back to Unknown for every column, the
// same as a provider that declared nothing.
std::optional<Schema> to_schema(const ::dftu_schema& built,
                                const std::vector<std::string>& names) {
    if (built.top.size() != names.size()) return std::nullopt;
    Schema s;
    s.fields.reserve(built.top.size());
    for (std::size_t i = 0; i < built.top.size(); ++i) {
        const Field& f = built.top[i];
        if (f.name != names[i] || !is_well_formed(f.type)) return std::nullopt;
        s.fields.push_back(f);
    }
    return s;
}

// Adapts a registered dftu_cursor_vt as a Cursor. next() follows the same
// dftu_task convention as dftu_plugin::on_batch/on_finalize: a returned task
// is awaited and freed here, the driving side, exactly as dftu_task_run does
// for a task crossing the ABI the same way.
class ProviderCursor final : public Cursor {
   public:
    ProviderCursor(const ::dftu_cursor_vt* vt, void* self,
                   std::vector<std::string> expected_projection)
        : vt_(vt),
          self_(self),
          expected_projection_(std::move(expected_projection)) {}

    ~ProviderCursor() override {
        if (vt_->destroy) vt_->destroy(self_);
    }

    ProviderCursor(const ProviderCursor&) = delete;
    ProviderCursor& operator=(const ProviderCursor&) = delete;

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        std::optional<Morsel> out;
        if (try_next(max_rows, out)) co_return out;
        auto* task = reinterpret_cast<coro::CoroTask<void>*>(pending_task_);
        pending_task_ = nullptr;
        co_await *task;
        delete task;
        co_return finish(pending_result_);
    }

    // Calls the provider's next() exactly once. A NULL returned task is the
    // documented dftu_cursor_vt::next contract for "answered without
    // suspending": *pending_result_* is already filled, so it can be
    // finished and returned here. A non-NULL task means the provider began a
    // genuinely async operation; that call cannot be repeated (it already
    // ran), so the task and its still-pending result are stashed in members
    // for next() to await and finish. Callers must fall back to next()
    // immediately on a false return rather than calling try_next again,
    // exactly as the Cursor::try_next contract requires.
    bool try_next(std::int64_t max_rows, std::optional<Morsel>& out) override {
        pending_result_ = ::dftu_result_frame{};
        if (::dftu_task* t = vt_->next(self_, max_rows, &pending_result_)) {
            pending_task_ = t;
            return false;
        }
        out = finish(pending_result_);
        return true;
    }

    // The three optional slots, each absent when the provider left it NULL.
    coro::CoroTask<bool> narrow(const Expr& predicate) override {
        if (!vt_->narrow) co_return false;
        std::int32_t applied = 0;
        ::dftu_expr* h = expr_handle_wrap(predicate);
        ::dftu_task* t = vt_->narrow(self_, h, &applied);
        if (t) {
            auto* task = reinterpret_cast<coro::CoroTask<void>*>(t);
            co_await *task;
            delete task;
        }
        ::dftu_expr_free(h);
        co_return applied != 0;
    }
    std::uint64_t resident_bytes() const override {
        return vt_->bytes ? vt_->bytes(self_) : 0;
    }
    coro::CoroTask<std::uint64_t> reclaim(std::uint64_t want) override {
        co_return vt_->reclaim ? vt_->reclaim(self_, want) : 0;
    }

   private:
    std::optional<Morsel> finish(const ::dftu_result_frame& out) {
        if (!DFTU_RESULT_OK(out))
            throw std::runtime_error(describe(DFTU_RESULT_ERROR(out)));

        ::dftu_dataframe* frame = DFTU_RESULT_VALUE(out);
        if (!frame) return std::nullopt;

        DataFrame df = dataframe_handle_take(frame);
        // Checked on every morsel, not just the first: the check is
        // O(projected columns), negligible next to the morsel's row data, and
        // the contract binds every dftu_dataframe the cursor produces, not
        // only its first.
        if (!expected_projection_.empty() && df.names != expected_projection_)
            throw std::runtime_error(
                "provider scan: frame columns [" + join_names(df.names) +
                "] do not match the requested projection [" +
                join_names(expected_projection_) + "]");

        Morsel m;
        m.rows = df.num_rows();
        m.columns = std::move(df.columns);
        return m;
    }

    const ::dftu_cursor_vt* vt_;
    void* self_;
    std::vector<std::string> expected_projection_;
    ::dftu_result_frame pending_result_{};
    ::dftu_task* pending_task_ = nullptr;
};

class ProviderSource final : public Source {
   public:
    explicit ProviderSource(ProviderEntry entry) : entry_(entry) {}

    Schema schema() const override {
        Schema s;
        const char* const* names = nullptr;
        const std::int32_t n = entry_.vt.schema(entry_.self, &names);
        if (n <= 0 || !names) return s;

        std::vector<std::string> name_strs;
        name_strs.reserve(static_cast<std::size_t>(n));
        for (std::int32_t i = 0; i < n; ++i)
            name_strs.push_back(names[i] ? names[i] : "");

        if (entry_.vt.schema_types) {
            ::dftu_schema built;
            entry_.vt.schema_types(entry_.self, &built);
            if (std::optional<Schema> typed = to_schema(built, name_strs))
                return *typed;
        }

        s.fields.reserve(name_strs.size());
        for (const std::string& name : name_strs)
            s.fields.push_back(Field{name, scalar(TypeId::Unknown), true});
        return s;
    }

    ScanResult scan(const ScanRequest& req) const override {
        ScanResult r;
        r.filters.assign(req.filters.size(), Pushed::No);

        std::vector<const char*> projection;
        projection.reserve(req.projection.size());
        for (const std::string& name : req.projection)
            projection.push_back(name.c_str());

        std::vector<dftu_expr*> owned_filters;
        owned_filters.reserve(req.filters.size());
        std::vector<const dftu_expr*> filters;
        filters.reserve(req.filters.size());
        for (const Expr& f : req.filters) {
            dftu_expr* h = expr_handle_wrap(f);
            owned_filters.push_back(h);
            filters.push_back(h);
        }

        ::dftu_scan_request creq{};
        creq.projection = projection.empty() ? nullptr : projection.data();
        creq.n_projection = static_cast<std::int32_t>(projection.size());
        creq.filters = filters.empty() ? nullptr : filters.data();
        creq.n_filters = static_cast<std::int32_t>(filters.size());
        creq.limit = req.limit;
        creq.memory_budget = req.memory_budget;

        // Pre-filled DFTU_PUSHED_NO: a provider that writes nothing, or writes
        // only some entries, degrades to "the engine re-applies", which is
        // always sound.
        std::vector<std::int32_t> out_pushed(filters.size(), DFTU_PUSHED_NO);

        void* cursor_self = nullptr;
        const ::dftu_cursor_vt* vt = nullptr;
        void* ok = entry_.vt.scan(entry_.self, &creq, out_pushed.data(),
                                  &cursor_self, &vt);

        for (dftu_expr* h : owned_filters) dftu_expr_free(h);

        if (!ok || !vt) return r;

        for (std::size_t i = 0; i < r.filters.size(); ++i)
            r.filters[i] = to_pushed(out_pushed[i]);
        r.cursor =
            std::make_unique<ProviderCursor>(vt, cursor_self, req.projection);
        return r;
    }

   private:
    ProviderEntry entry_;
};

}  // namespace

std::shared_ptr<Source> make_provider_source(const ::dftu_source_vt& vt,
                                             void* self) {
    return std::make_shared<ProviderSource>(ProviderEntry{vt, self});
}

}  // namespace dftracer::utils::dataframe

extern "C" {

int32_t dftu_schema_add_field(dftu_schema* schema, const char* name,
                              dftu_dtype type, int32_t nullable,
                              dftu_time_unit time_unit, const char* tz,
                              int32_t decimal_precision, int32_t decimal_scale,
                              int32_t fixed_size) {
    if (!schema || !name) return -1;
    if (type < DFTU_TYPE_UNKNOWN || type > DFTU_TYPE_MAP) return -1;
    schema->top.push_back(dftracer::utils::dataframe::to_field(
        name, type, nullable, time_unit, tz, decimal_precision, decimal_scale,
        fixed_size));
    const int32_t idx = static_cast<int32_t>(schema->paths.size());
    schema->paths.push_back({static_cast<int32_t>(schema->top.size() - 1)});
    return idx;
}

int32_t dftu_schema_add_child_field(dftu_schema* schema, int32_t parent_index,
                                    const char* name, dftu_dtype type,
                                    int32_t nullable, dftu_time_unit time_unit,
                                    const char* tz, int32_t decimal_precision,
                                    int32_t decimal_scale, int32_t fixed_size) {
    if (!schema || !name) return -1;
    if (type < DFTU_TYPE_UNKNOWN || type > DFTU_TYPE_MAP) return -1;
    dftracer::utils::dataframe::Field* parent =
        dftracer::utils::dataframe::resolve_field(schema, parent_index);
    // Refused at the call that would break the shape, so a schema under
    // construction is never in a state to_schema has to throw away: a second
    // element under a List is rejected here rather than discovered at the end.
    if (!parent ||
        !dftracer::utils::dataframe::accepts_another_child(parent->type))
        return -1;

    parent->type.fields.push_back(dftracer::utils::dataframe::to_field(
        name, type, nullable, time_unit, tz, decimal_precision, decimal_scale,
        fixed_size));

    std::vector<int32_t> path =
        schema->paths[static_cast<std::size_t>(parent_index)];
    path.push_back(static_cast<int32_t>(parent->type.fields.size() - 1));
    const int32_t idx = static_cast<int32_t>(schema->paths.size());
    schema->paths.push_back(std::move(path));
    return idx;
}

int32_t dftu_schema_field_count(const dftu_schema* schema) {
    if (!schema) return -1;
    return static_cast<int32_t>(schema->top.size());
}

int32_t dftu_schema_copy_field(dftu_schema* out, const dftu_schema* schema,
                               int32_t i) {
    if (!out || !schema) return -1;
    if (i < 0 || static_cast<std::size_t>(i) >= schema->top.size()) return -1;
    out->top.push_back(schema->top[static_cast<std::size_t>(i)]);
    const int32_t idx = static_cast<int32_t>(out->paths.size());
    out->paths.push_back({static_cast<int32_t>(out->top.size() - 1)});
    return idx;
}

int dftu_provider_register(const char* name, const dftu_source_vt* vt,
                           void* self) {
    if (!name || !vt) return 1;
    using dftracer::utils::dataframe::ProviderEntry;
    std::lock_guard<std::mutex> lock(
        dftracer::utils::dataframe::registry_mutex());
    auto& reg = dftracer::utils::dataframe::providers();
    if (reg.find(name) != reg.end()) return 1;
    reg.emplace(name, ProviderEntry{*vt, self});
    return 0;
}

int dftu_provider_unregister(const char* name) {
    if (!name) return 1;
    dftracer::utils::dataframe::ProviderEntry removed;
    {
        std::lock_guard<std::mutex> lock(
            dftracer::utils::dataframe::registry_mutex());
        auto& reg = dftracer::utils::dataframe::providers();
        auto it = reg.find(name);
        if (it == reg.end()) return 1;
        removed = it->second;
        reg.erase(it);
    }
    // Outside the lock: destroy() is caller code (a plugin's), which must not
    // run while the registry lock is held.
    if (removed.vt.destroy) removed.vt.destroy(removed.self);
    return 0;
}

dftu_lazyframe* dftu_lazyframe_from_provider(const char* name) {
    std::optional<dftracer::utils::dataframe::ProviderEntry> entry =
        dftracer::utils::dataframe::find_provider(name);
    if (!entry) return nullptr;
    std::shared_ptr<dftracer::utils::dataframe::Source> source =
        std::make_shared<dftracer::utils::dataframe::ProviderSource>(*entry);
    return dftracer::utils::dataframe::lazyframe_handle_wrap(
        dftracer::utils::dataframe::LazyFrame::scan(std::move(source)));
}

}  // extern "C"
