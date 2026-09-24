#ifndef DFTRACER_UTILS_TRACE_VIEWS_FOLD_EVENT_H
#define DFTRACER_UTILS_TRACE_VIEWS_FOLD_EVENT_H

#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/trace/event.h>
#include <simdjson.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// The owned event the fold-fusion scan core batches, kept in its own header so
// both the Fold interface and the event accessor depend on the data, not on
// each other.
namespace dftracer::utils::trace::views::detail {

/// An event owned independently of the simdjson parse that produced it: strings
/// are interned to ids, never views into the reused parser buffer, so a whole
/// batch stays valid at once. `args` is filled only for folds that need it,
/// with nested object and array args flattened to dotted keys (`a.b`, `a.0`).
struct FoldEvent {
    std::uint32_t cat_id = 0xFFFFFFFF;
    std::uint32_t name_id = 0xFFFFFFFF;
    std::uint32_t fhash_id = 0xFFFFFFFF;
    std::uint32_t hhash_id = 0xFFFFFFFF;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    RecordPhase phase = RecordPhase::UNKNOWN;
    bool has_dur = false;
    /// A record decoded by exact path (decode_record): every field is in
    /// `args` under its path, and the fixed fields above are unset.
    bool by_path = false;

    /// A metric (double for reals, int64 for exact integers so values above
    /// 2^53 survive) or an interned string id (group dimensions), keyed by the
    /// interned arg-name id.
    using ArgValue = std::variant<double, std::int64_t, std::uint32_t>;
    std::vector<std::pair<std::uint32_t, ArgValue>> args;
    /// Top-level values for schema fields the POD does not carry as a scalar
    /// (type/ph/id), kept apart from `args` so a same-named args key cannot
    /// shadow the real top-level field. Same encoding as `args`.
    std::vector<std::pair<std::uint32_t, ArgValue>> top_fields;

    /// Every scalar or null leaf of the record, arbitrarily nested, as
    /// (interned exact path id -> index::store::PathType): "args.io.off",
    /// "args.hosts.1", a top-level "type". Filled only when the scan captures
    /// schema (the index build); empty otherwise.
    std::vector<std::pair<std::uint32_t, std::uint8_t>> schema_leaves;
};

/// The fixed top-level fields of the trace schema. A bare reference to one of
/// these resolves to the top-level value, never a same-named args key; reach an
/// args field with the same name through the explicit `args.<name>` path.
inline bool is_schema_field(std::string_view f) {
    return f == "name" || f == "cat" || f == "pid" || f == "tid" || f == "ts" ||
           f == "dur" || f == "ph" || f == "id" || f == "type";
}

/// True when `field` addresses a nested value (`a.b`, `a[0]`, `a.0.b`), so it
/// needs path resolution rather than a single object-key lookup.
inline bool is_nested_path(std::string_view field) {
    return field.find_first_of(".[") != std::string_view::npos;
}

/// Resolve a dotted/bracketed path from `root` (`a.b[0].c`, `a.b.0`); `ok` is
/// false if any segment is missing. Rooted at `root` with no args fallback
/// (capture_extra_field adds it). Delegates to the
/// single path walker (JsonValue::at) so flat dotted member keys resolve here
/// too; on failure `ok` is false and `root` is returned unchanged.
inline simdjson::dom::element resolve_json_path(simdjson::dom::element root,
                                                std::string_view path,
                                                bool& ok) {
    json::JsonValue v = json::JsonValue(root).at(path);
    ok = v.exists();
    return ok ? v.element() : root;
}

/// Append `v` under `key_id` when it is a scalar: a string interned, an integer
/// exact as int64 (a uint64 above INT64_MAX as double), a bool as 0/1. Nulls,
/// objects and arrays append nothing.
inline void append_scalar_arg(
    std::vector<std::pair<std::uint32_t, FoldEvent::ArgValue>>& into,
    std::uint32_t key_id, simdjson::dom::element v,
    dftracer::utils::StringIntern& intern) {
    using T = simdjson::dom::element_type;
    switch (v.type()) {
        case T::STRING:
            into.emplace_back(
                key_id, intern.get_or_insert(v.get_string().value_unsafe()));
            break;
        case T::INT64:
            into.emplace_back(key_id, v.get_int64().value_unsafe());
            break;
        case T::UINT64: {
            const std::uint64_t u = v.get_uint64().value_unsafe();
            if (u <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max()))
                into.emplace_back(key_id, static_cast<std::int64_t>(u));
            else
                into.emplace_back(key_id, static_cast<double>(u));
            break;
        }
        case T::DOUBLE:
            into.emplace_back(key_id, v.get_double().value_unsafe());
            break;
        case T::BOOL:
            into.emplace_back(
                key_id, static_cast<std::int64_t>(v.get_bool().value_unsafe()));
            break;
        default:
            break;
    }
}

/// Capture a field the POD does not natively carry (top-level type/ph/id, or a
/// nested a.b/a[0]), JSON type preserved. A name that misses at the event root
/// and is neither `args.`-prefixed nor a bare schema field resolves under
/// `args`, as the query evaluator does, and is stored under `name`. A bare
/// schema field goes to `top_fields` so a same-named args key cannot shadow it;
/// everything else to `args`. No-op when the path is absent.
inline void capture_extra_field(FoldEvent& ev, simdjson::dom::element root,
                                dftracer::utils::StringIntern& intern,
                                const std::string& name) {
    bool ok = false;
    simdjson::dom::element v = resolve_json_path(root, name, ok);
    const bool schema_field = !is_nested_path(name) && is_schema_field(name);
    if (!ok && !schema_field && !dftracer::utils::has_args_prefix(name)) {
        simdjson::dom::element args;
        if (root["args"].get(args) == simdjson::SUCCESS)
            v = resolve_json_path(args, name, ok);
    }
    if (!ok) return;
    const std::uint32_t key_id = intern.get_or_insert(name);
    auto& into = schema_field ? ev.top_fields : ev.args;
    for (const auto& [k, existing] : into)
        if (k == key_id) return;
    append_scalar_arg(into, key_id, v, intern);
}

/// Enumerate every scalar or null leaf of `root` (arbitrarily nested) into
/// ev.schema_leaves by exact path for the index build's path catalog; the
/// axis and structural keys (pid/tid/ts/dur/ph/id) are excluded.
void capture_schema_leaves(FoldEvent& ev, simdjson::dom::element root,
                           dftracer::utils::StringIntern& intern);

/// A record of no known trace format as an owned data event (by_path): every
/// scalar leaf, by exact path ("op", "io.off", "hosts.1"), in `args`, and,
/// with `capture_schema`, every leaf in `schema_leaves`. With `paths` (sorted,
/// borrowed), only those leaves. With `record_schema`, a declared field's
/// value is converted to its type (null when it does not convert) and role
/// fields set ts, dur and pid in microseconds; otherwise the fixed fields are
/// unset. `path_fields`, aligned with `paths`, gives the declared field at
/// each path so no lookup runs per leaf.
FoldEvent decode_record(
    simdjson::dom::element root, dftracer::utils::StringIntern& intern,
    bool capture_schema = true, const std::vector<std::string>* paths = nullptr,
    const dftracer::utils::index::RecordSchema* record_schema = nullptr,
    const std::vector<const dftracer::utils::index::FieldSpec*>* path_fields =
        nullptr);

/// Build an owned event from already-parsed scalars + the args element, for
/// callers (like the index parse) that have run DFTracerEvent::parse_scalars
/// already. Every string is interned, so the result outlives `args`'s parser.
FoldEvent build_fold_event(const DFTracerEvent& scalars,
                           simdjson::dom::element args, bool has_args,
                           dftracer::utils::StringIntern& intern,
                           bool needs_args);

/// Parse a DOM object into an owned event. Every string is interned, so the
/// result stays valid after the parser that produced `root` is reused. Args are
/// captured only when `needs_args`. `extra_fields`, if given, names fields the
/// POD does not natively carry (type/ph, a nested a.b/a[0]) to capture into the
/// event. When `capture_schema`, every scalar leaf (arbitrarily nested) is
/// enumerated into `schema_leaves` for the index build's column harvest.
FoldEvent extract_fold_event(
    simdjson::dom::element root, dftracer::utils::StringIntern& intern,
    bool needs_args, const std::vector<std::string>* extra_fields = nullptr,
    bool capture_schema = false);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_FOLD_EVENT_H
