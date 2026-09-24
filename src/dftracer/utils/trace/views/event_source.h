#ifndef DFTRACER_UTILS_TRACE_VIEWS_EVENT_SOURCE_H
#define DFTRACER_UTILS_TRACE_VIEWS_EVENT_SOURCE_H

#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/dataframe/field_stat.h>
#include <dftracer/utils/query/query.h>
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
            out.append(std::to_string(d));
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
        if (field == "pid") return static_cast<double>(ev_.pid);
        if (field == "tid") return static_cast<double>(ev_.tid);
        if (field == "ts") return static_cast<double>(ev_.ts);
        if (field == "dur")
            return ev_.has_dur
                       ? std::optional<double>(static_cast<double>(ev_.dur))
                       : std::nullopt;
        const auto* v =
            is_schema_field(field) ? find_top(field) : find_arg(field);
        if (v) {
            if (const auto* d = std::get_if<double>(v)) return *d;
            if (const auto* i = std::get_if<std::int64_t>(v))
                return static_cast<double>(*i);
        }
        return std::nullopt;
    }

    std::optional<dftracer::utils::dataframe::FieldNum> number_typed(
        std::string_view field) const {
        using dftracer::utils::dataframe::FieldNum;
        // Top-level POD scalars are unsigned integers.
        if (field == "pid") return FieldNum::of(ev_.pid);
        if (field == "tid") return FieldNum::of(ev_.tid);
        if (field == "ts") return FieldNum::of(ev_.ts);
        if (field == "dur")
            return ev_.has_dur ? std::optional<FieldNum>(FieldNum::of(ev_.dur))
                               : std::nullopt;
        const auto* v =
            is_schema_field(field) ? find_top(field) : find_arg(field);
        if (v) {
            if (const auto* d = std::get_if<double>(v)) return FieldNum::of(*d);
            if (const auto* i = std::get_if<std::int64_t>(v))
                return FieldNum::of(*i);
        }
        return std::nullopt;
    }

    void append_value(std::string& out, std::string_view field) const {
        if (field == "cat") {
            append_id(out, ev_.cat_id);
        } else if (field == "name") {
            append_id(out, ev_.name_id);
        } else if (field == "pid") {
            append_u64(out, ev_.pid);
        } else if (field == "tid") {
            append_u64(out, ev_.tid);
        } else if (field == "ts") {
            append_u64(out, ev_.ts);
        } else if (field == "dur") {
            if (ev_.has_dur) append_u64(out, ev_.dur);
        } else if (is_schema_field(field)) {
            // ph/id/type: the top-level value only, never a same-named arg.
            if (const auto* v = find_top(field)) append_arg_value(out, *v);
        } else {
            append_arg(out, field);
        }
    }

    void append_arg(std::string& out, std::string_view key) const {
        const std::string_view bare = strip_args_prefix(key);
        if (bare == "fhash")
            append_id(out, ev_.fhash_id);
        else if (bare == "hhash")
            append_id(out, ev_.hhash_id);
        else if (const auto* v = find_arg(key))
            append_arg_value(out, *v);
    }

    std::string value(std::string_view field) const {
        std::string s;
        append_value(s, field);
        return s;
    }

    template <class F>
    void for_each_numeric_arg(F&& fn) const {
        for (const auto& [key_id, v] : ev_.args) {
            if (const auto* d = std::get_if<double>(&v))
                fn(intern_.resolve(key_id), *d);
            else if (const auto* i = std::get_if<std::int64_t>(&v))
                fn(intern_.resolve(key_id), static_cast<double>(*i));
        }
    }

   private:
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
        else
            append_number(out, std::get<double>(v));
    }

    std::uint32_t intern_lookup(std::string_view field) const {
        // The event's arg keys are already interned, so resolving a query field
        // to an id is a lookup, not an insert; an unknown field simply misses.
        return const_cast<dftracer::utils::StringIntern&>(intern_)
            .get_or_insert(field);
    }

    void append_id(std::string& out, std::uint32_t id) const {
        if (id != dftracer::utils::StringIntern::NO_ID)
            out.append(intern_.resolve(id));
    }

    static void append_u64(std::string& out, std::uint64_t v) {
        char buf[24];
        char* p = to_chars_u64(buf, buf + sizeof(buf), v);
        out.append(buf, static_cast<std::size_t>(p - buf));
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
            out.append(std::to_string(d));
        }
    }

    const FoldEvent& ev_;
    const dftracer::utils::StringIntern& intern_;
};

/// Evaluate `q` on a parsed event: a field reads as a number when the event
/// holds one, else as its string, and an absent field is left out (which
/// evaluates false, as on the JSON path). `scratch` is reused across calls.
inline bool pod_matches(const query::Query& q, const FoldEvent& ev,
                        const dftracer::utils::StringIntern& intern,
                        query::ValueMap& scratch) {
    PodSource src(ev, intern);
    scratch.clear();
    for (std::string_view f : q.fields()) {
        if (std::optional<double> n = src.number(f)) {
            scratch[f] = *n;
        } else {
            std::string v = src.value(f);
            if (!v.empty()) scratch[f] = std::move(v);
        }
    }
    return q.evaluate(scratch);
}

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_EVENT_SOURCE_H
