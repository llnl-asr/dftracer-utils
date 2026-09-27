#ifndef DFTRACER_UTILS_TRACE_VIEWS_EVENT_SOURCE_H
#define DFTRACER_UTILS_TRACE_VIEWS_EVENT_SOURCE_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/trace/event.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <simdjson.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// Source-agnostic event extraction: the aggregation fold reads through an
// EventSource, so the derived logic (agg_field, group values) is written once
// and a live simdjson element and the fold-fusion POD cannot diverge. The
// concept, satisfied by DomSource here and PodSource later:
//   phase() -> RecordPhase
//   number(field) -> optional<double>       top-level then args, raw
//   append_value(out, field) / value(field) top-level then args, as text
//   for_each_numeric_arg(fn)                 fn(key, number)
// number and append_value use no intermediate string, so the fold stays
// zero-copy.
namespace dftracer::utils::trace::views::detail {

/// EventSource over a live simdjson DOM element. The six fields the fold reads
/// hot (name/cat/pid/tid/ts/dur) plus args are captured in one object walk, so
/// per-field reads do not rescan the object.
class DomSource {
   public:
    explicit DomSource(simdjson::dom::element root) : root_(root) {
        auto obj = root.get_object();
        if (obj.error()) return;
        for (auto kv : obj.value_unsafe()) {
            std::string_view k = kv.key;
            if (k == "name") {
                name_ = {kv.value, true};
            } else if (k == "cat") {
                cat_ = {kv.value, true};
            } else if (k == "pid") {
                pid_ = {kv.value, true};
            } else if (k == "tid") {
                tid_ = {kv.value, true};
            } else if (k == "ts") {
                ts_ = {kv.value, true};
            } else if (k == "dur") {
                dur_ = {kv.value, true};
            } else if (k == "args" && kv.value.is_object()) {
                args_ = kv.value;
                has_args_ = true;
            }
        }
    }

    RecordPhase phase() const {
        auto p = root_["ph"];
        return p.error() ? RecordPhase::UNKNOWN
                         : read_phase(json::JsonValue(p.value_unsafe()));
    }

    std::optional<double> number(std::string_view field) const {
        auto [e, ok] = resolve(field);
        return ok ? as_number(e) : std::nullopt;
    }

    std::optional<dftracer::utils::dataframe::FieldNum> number_typed(
        std::string_view field) const {
        auto [e, ok] = resolve(field);
        return ok ? as_typed_number(e) : std::nullopt;
    }

    void append_value(std::string& out, std::string_view field) const {
        auto [e, ok] = resolve(field);
        if (ok) append_text(out, e);
    }

    void append_arg(std::string& out, std::string_view key) const {
        if (!has_args_) return;
        json::JsonValue av = json::JsonValue(args_).at(strip_args_prefix(key));
        if (av.exists()) append_text(out, av.element());
    }

    std::string value(std::string_view field) const {
        std::string s;
        append_value(s, field);
        return s;
    }

    template <class F>
    void for_each_numeric_arg(F&& fn) const {
        if (!has_args_) return;
        for (auto field : args_.get_object())
            if (auto n = as_number(field.value)) fn(field.key, *n);
    }

   private:
    using Cached = std::pair<simdjson::dom::element, bool>;

    // Top-level (schema/hot) first so a bare schema name never resolves a
    // same-named arg; then the flat arg key (dotted or not, prefix optional);
    // then a genuinely nested non-args path.
    Cached resolve(std::string_view field) const {
        if (auto t = top(field); t.second) return t;
        if (has_args_) {
            json::JsonValue av =
                json::JsonValue(args_).at(strip_args_prefix(field));
            if (av.exists()) return {av.element(), true};
        }
        if (is_nested_path(field)) {
            json::JsonValue rv = json::JsonValue(root_).at(field);
            if (rv.exists()) return {rv.element(), true};
        }
        return {{}, false};
    }

    // The captured element for a hot field, else a live lookup so an uncaptured
    // field still resolves.
    Cached top(std::string_view field) const {
        if (field == "name") return name_;
        if (field == "cat") return cat_;
        if (field == "pid") return pid_;
        if (field == "tid") return tid_;
        if (field == "ts") return ts_;
        if (field == "dur") return dur_;
        auto r = root_[field];
        return r.error() ? Cached{{}, false} : Cached{r.value_unsafe(), true};
    }

    static std::optional<double> as_number(simdjson::dom::element e) {
        double d;
        if (e.get_double().get(d) == simdjson::SUCCESS) return d;
        std::int64_t i;
        if (e.get_int64().get(i) == simdjson::SUCCESS)
            return static_cast<double>(i);
        std::uint64_t u;
        if (e.get_uint64().get(u) == simdjson::SUCCESS)
            return static_cast<double>(u);
        return std::nullopt;
    }

    // Integer types are tried before double so an integer field keeps its exact
    // domain (a double would round timestamps/durations past 2^53).
    static std::optional<dftracer::utils::dataframe::FieldNum> as_typed_number(
        simdjson::dom::element e) {
        std::int64_t i;
        if (e.get_int64().get(i) == simdjson::SUCCESS)
            return dftracer::utils::dataframe::FieldNum::of(i);
        std::uint64_t u;
        if (e.get_uint64().get(u) == simdjson::SUCCESS)
            return dftracer::utils::dataframe::FieldNum::of(u);
        double d;
        if (e.get_double().get(d) == simdjson::SUCCESS)
            return dftracer::utils::dataframe::FieldNum::of(d);
        return std::nullopt;
    }

    // Byte-for-byte the same text the group key and per-group value must agree
    // on: string as-is, integers via to_chars, double via to_string, bool
    // spelled out.
    static void append_text(std::string& out, simdjson::dom::element e) {
        std::string_view s;
        if (e.get_string().get(s) == simdjson::SUCCESS) {
            out.append(s);
            return;
        }
        char buf[24];
        std::int64_t i;
        if (e.get_int64().get(i) == simdjson::SUCCESS) {
            char* p = to_chars_i64(buf, buf + sizeof(buf), i);
            out.append(buf, static_cast<std::size_t>(p - buf));
            return;
        }
        std::uint64_t u;
        if (e.get_uint64().get(u) == simdjson::SUCCESS) {
            char* p = to_chars_u64(buf, buf + sizeof(buf), u);
            out.append(buf, static_cast<std::size_t>(p - buf));
            return;
        }
        double d;
        if (e.get_double().get(d) == simdjson::SUCCESS) {
            out.append(dftracer::utils::double_text(d));
            return;
        }
        bool b;
        if (e.get_bool().get(b) == simdjson::SUCCESS)
            out.append(b ? "true" : "false");
    }

    simdjson::dom::element root_;
    simdjson::dom::element args_{};
    Cached name_{{}, false}, cat_{{}, false}, pid_{{}, false}, tid_{{}, false};
    Cached ts_{{}, false}, dur_{{}, false};
    bool has_args_ = false;
};

/// EventSource over an owned interned FoldEvent. Resolves interned ids through
/// the same table the event was built with, so a field read reproduces the
/// bytes the DOM path would have produced. Only the fields the POD captured are
/// visible; a query naming an uncaptured top-level field is the planner's job
/// to keep off this path.
class PodSource {
   public:
    PodSource(const FoldEvent& ev, const dftracer::utils::StringIntern& intern)
        : ev_(ev), intern_(intern) {}

    RecordPhase phase() const { return ev_.phase; }

    std::optional<double> number(std::string_view field) const {
        if (ev_.by_path) return as_double(find_arg_by(field));
        if (field == "pid") return static_cast<double>(ev_.pid);
        if (field == "tid") return static_cast<double>(ev_.tid);
        if (field == "ts") return static_cast<double>(ev_.ts);
        if (field == "dur")
            return ev_.has_dur
                       ? std::optional<double>(static_cast<double>(ev_.dur))
                       : std::nullopt;
        return as_double(is_schema_field(field) ? find_top(field)
                                                : find_arg(field));
    }

    std::optional<dftracer::utils::dataframe::FieldNum> number_typed(
        std::string_view field) const {
        using dftracer::utils::dataframe::FieldNum;
        const auto* v = ev_.by_path ? find_arg_by(field) : nullptr;
        if (ev_.by_path) {
            if (v) {
                if (const auto* d = std::get_if<double>(v))
                    return FieldNum::of(*d);
                if (const auto* i = std::get_if<std::int64_t>(v))
                    return FieldNum::of(*i);
                if (const auto* u = std::get_if<std::uint64_t>(v))
                    return FieldNum::of(*u);
            }
            return std::nullopt;
        }
        // Top-level POD scalars are unsigned integers.
        if (field == "pid") return FieldNum::of(ev_.pid);
        if (field == "tid") return FieldNum::of(ev_.tid);
        if (field == "ts") return FieldNum::of(ev_.ts);
        if (field == "dur")
            return ev_.has_dur ? std::optional<FieldNum>(FieldNum::of(ev_.dur))
                               : std::nullopt;
        v = is_schema_field(field) ? find_top(field) : find_arg(field);
        if (v) {
            if (const auto* d = std::get_if<double>(v)) return FieldNum::of(*d);
            if (const auto* i = std::get_if<std::int64_t>(v))
                return FieldNum::of(*i);
            if (const auto* u = std::get_if<std::uint64_t>(v))
                return FieldNum::of(*u);
        }
        return std::nullopt;
    }

    /// Appends the text of `field`; false when the event lacks it.
    bool append_value(std::string& out, std::string_view field) const {
        if (ev_.by_path) return append_found(out, find_arg_by(field));
        if (field == "cat") return append_id(out, ev_.cat_id);
        if (field == "name") return append_id(out, ev_.name_id);
        if (field == "pid") return append_u64(out, ev_.pid);
        if (field == "tid") return append_u64(out, ev_.tid);
        if (field == "ts") return append_u64(out, ev_.ts);
        if (field == "dur") return ev_.has_dur && append_u64(out, ev_.dur);
        // ph/id/type: the top-level value only, never a same-named arg.
        if (is_schema_field(field)) return append_found(out, find_top(field));
        return append_arg(out, field);
    }

    /// Appends the text of arg `key`; false when the event lacks it.
    bool append_arg(std::string& out, std::string_view key) const {
        if (ev_.by_path) return append_found(out, find_arg_by(key));
        const std::string_view bare = strip_args_prefix(key);
        if (bare == "fhash") return append_id(out, ev_.fhash_id);
        if (bare == "hhash") return append_id(out, ev_.hhash_id);
        return append_found(out, find_arg(key));
    }

    std::string value(std::string_view field) const {
        std::string s;
        append_value(s, field);
        return s;
    }

    /// Whether the event holds `field`, found the way append_value finds it.
    bool has(std::string_view field) const {
        constexpr auto NO_ID = dftracer::utils::StringIntern::NO_ID;
        if (ev_.by_path) return find_arg_by(field) != nullptr;
        if (field == "cat") return ev_.cat_id != NO_ID;
        if (field == "name") return ev_.name_id != NO_ID;
        if (field == "pid" || field == "tid" || field == "ts") return true;
        if (field == "dur") return ev_.has_dur;
        if (is_schema_field(field)) return find_top(field) != nullptr;
        const std::string_view bare = strip_args_prefix(field);
        if (bare == "fhash") return ev_.fhash_id != NO_ID;
        if (bare == "hhash") return ev_.hhash_id != NO_ID;
        return find_arg(field) != nullptr;
    }

    /// The null, bool or empty container the event holds at `field`, found the
    /// way append_value finds a field.
    const FoldEvent::Special* special(std::string_view field) const {
        if (ev_.specials.empty()) return nullptr;
        const auto find = [this](std::string_view f,
                                 bool top) -> const FoldEvent::Special* {
            const std::uint32_t id = this->intern_lookup(f);
            for (const auto& s : ev_.specials)
                if (s.key == id && s.top == top) return &s.kind;
            return nullptr;
        };
        if (ev_.by_path) return find(field, false);
        if (is_schema_field(field)) return find(field, true);
        if (const auto* v = find(field, false)) return v;
        const std::string_view bare = strip_args_prefix(field);
        return bare == field ? nullptr : find(bare, false);
    }

    template <class F>
    void for_each_numeric_arg(F&& fn) const {
        for (const auto& [key_id, v] : ev_.args) {
            if (const auto* d = std::get_if<double>(&v))
                fn(intern_.resolve(key_id), *d);
            else if (const auto* i = std::get_if<std::int64_t>(&v))
                fn(intern_.resolve(key_id), static_cast<double>(*i));
            else if (const auto* u = std::get_if<std::uint64_t>(&v))
                fn(intern_.resolve(key_id), static_cast<double>(*u));
        }
    }

   private:
    static std::optional<double> as_double(const FoldEvent::ArgValue* v) {
        if (v) {
            if (const auto* d = std::get_if<double>(v)) return *d;
            if (const auto* i = std::get_if<std::int64_t>(v))
                return static_cast<double>(*i);
            if (const auto* u = std::get_if<std::uint64_t>(v))
                return static_cast<double>(*u);
        }
        return std::nullopt;
    }

    // Nested/extra fields are captured under their full name including the
    // "args." prefix (capture_extra_field), while top-level args are stored
    // bare. Match the field as written first, then its stripped form, so both
    // "args.meta.host" (nested, prefixed) and "args.cqe.raw_ns" (flat arg key,
    // stored bare) resolve.
    const FoldEvent::ArgValue* find_arg(std::string_view field) const {
        if (const auto* v = find_arg_by(field)) return v;
        const std::string_view bare = strip_args_prefix(field);
        return bare == field ? nullptr : find_arg_by(bare);
    }

    const FoldEvent::ArgValue* find_arg_by(std::string_view field) const {
        const std::uint32_t id = intern_lookup(field);
        if (id == dftracer::utils::StringIntern::NO_ID) return nullptr;
        for (const auto& [key_id, v] : ev_.args)
            if (key_id == id) return &v;
        return nullptr;
    }

    const FoldEvent::ArgValue* find_top(std::string_view field) const {
        const std::uint32_t id = intern_lookup(field);
        if (id == dftracer::utils::StringIntern::NO_ID) return nullptr;
        for (const auto& [key_id, v] : ev_.top_fields)
            if (key_id == id) return &v;
        return nullptr;
    }

    void append_arg_value(std::string& out,
                          const FoldEvent::ArgValue& v) const {
        if (const auto* id = std::get_if<std::uint32_t>(&v))
            out.append(intern_.resolve(*id));
        else if (const auto* i = std::get_if<std::int64_t>(&v))
            append_i64(out, *i);
        else if (const auto* u = std::get_if<std::uint64_t>(&v))
            append_u64(out, *u);
        else
            append_number(out, std::get<double>(v));
    }

    std::uint32_t intern_lookup(std::string_view field) const {
        // The event's arg keys are already interned, so resolving a query field
        // to an id is a lookup, not an insert; an unknown field simply misses.
        return const_cast<dftracer::utils::StringIntern&>(intern_)
            .get_or_insert(field);
    }

    bool append_found(std::string& out, const FoldEvent::ArgValue* v) const {
        if (!v) return false;
        append_arg_value(out, *v);
        return true;
    }

    bool append_id(std::string& out, std::uint32_t id) const {
        if (id == dftracer::utils::StringIntern::NO_ID) return false;
        out.append(intern_.resolve(id));
        return true;
    }

    static bool append_u64(std::string& out, std::uint64_t v) {
        char buf[24];
        char* p = to_chars_u64(buf, buf + sizeof(buf), v);
        out.append(buf, static_cast<std::size_t>(p - buf));
        return true;
    }

    static void append_i64(std::string& out, std::int64_t v) {
        char buf[24];
        char* p = to_chars_i64(buf, buf + sizeof(buf), v);
        out.append(buf, static_cast<std::size_t>(p - buf));
    }

    // Doubles that hold an integer must print as an integer to match the DOM
    // path, which never widened them.
    static void append_number(std::string& out, double d) {
        auto i = static_cast<std::int64_t>(d);
        if (static_cast<double>(i) == d) {
            char buf[24];
            char* p = to_chars_i64(buf, buf + sizeof(buf), i);
            out.append(buf, static_cast<std::size_t>(p - buf));
        } else {
            out.append(dftracer::utils::double_text(d));
        }
    }

    const FoldEvent& ev_;
    const dftracer::utils::StringIntern& intern_;
};

/// The array or object at `path` rebuilt from the event's flattened keys
/// (`path.0`, `path.key`), or nullopt when none lies under it.
std::optional<duql::Cell> container_cell(
    const FoldEvent& ev, const dftracer::utils::StringIntern& intern,
    std::string_view path);

inline duql::Cell special_cell(FoldEvent::Special s) {
    switch (s) {
        case FoldEvent::Special::NULL_VALUE:
            return duql::Cell::null();
        case FoldEvent::Special::FALSE_VALUE:
            return false;
        case FoldEvent::Special::TRUE_VALUE:
            return true;
        case FoldEvent::Special::EMPTY_ARRAY:
            return duql::Cell::json("[]", true);
        case FoldEvent::Special::EMPTY_OBJECT:
            return duql::Cell::json("{}", false);
        case FoldEvent::Special::JSON_ARRAY:
        case FoldEvent::Special::JSON_OBJECT:
            break;
    }
    return duql::Cell::null();
}

/// Evaluate `q` on a parsed event: a field reads as a number when the event
/// holds one, else as its string (an empty string included), and a null, a
/// bool or an array or object as itself; an absent field is left out, so it
/// is missing, as on the JSON path. `scratch` is reused across calls.
inline bool pod_matches(const duql::Query& q, const FoldEvent& ev,
                        const dftracer::utils::StringIntern& intern,
                        duql::ValueMap& scratch) {
    using dftracer::utils::dataframe::FieldStatDomain;
    PodSource src(ev, intern);
    scratch.clear();
    for (std::string_view f : q.fields()) {
        if (const auto* sp = src.special(f)) {
            if (*sp == FoldEvent::Special::JSON_ARRAY ||
                *sp == FoldEvent::Special::JSON_OBJECT)
                scratch[f] = duql::Cell::json(
                    src.value(f), *sp == FoldEvent::Special::JSON_ARRAY);
            else
                scratch[f] = special_cell(*sp);
            continue;
        }
        if (!src.has(f)) {
            if (q.has_expressions())
                if (auto c = container_cell(ev, intern, f))
                    scratch[f] = std::move(*c);
            continue;
        }
        if (const auto n = src.number_typed(f)) {
            switch (n->domain) {
                case FieldStatDomain::I64:
                    scratch[f] = n->i;
                    break;
                case FieldStatDomain::U64:
                    scratch[f] = n->u;
                    break;
                case FieldStatDomain::F64:
                    scratch[f] = n->d;
                    break;
            }
        } else {
            scratch[f] = src.value(f);
        }
    }
    // An any() field reads the flattened positions `<path>.<k>`; a dftracer
    // event's args carry no "args." prefix.
    for (const std::string& path : q.any_paths()) {
        const std::string_view bare =
            std::string_view(path).starts_with("args.")
                ? std::string_view(path).substr(5)
                : std::string_view(path);
        for (const auto& [key, value] : ev.args) {
            const std::string_view name = intern.resolve(key);
            if (name.size() <= bare.size() || !name.starts_with(bare) ||
                name[bare.size()] != '.')
                continue;
            duql::LiteralValue lit = std::visit(
                [&](auto x) -> duql::LiteralValue {
                    if constexpr (std::is_same_v<decltype(x), std::uint32_t>)
                        return std::string(intern.resolve(x));
                    else
                        return x;
                },
                value);
            scratch[name] = std::move(lit);
        }
        for (const auto& s : ev.specials) {
            if (s.top || s.kind == FoldEvent::Special::JSON_ARRAY ||
                s.kind == FoldEvent::Special::JSON_OBJECT)
                continue;
            const std::string_view name = intern.resolve(s.key);
            if (name.size() > bare.size() && name.starts_with(bare) &&
                name[bare.size()] == '.')
                scratch[name] = special_cell(s.kind);
        }
    }
    return q.evaluate(scratch);
}

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_EVENT_SOURCE_H
