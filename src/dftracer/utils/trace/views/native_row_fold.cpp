#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/trace/aggregators/reserved_args.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/agg_fold.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/native_row_fold.h>
#include <dftracer/utils/trace/views/view_resolver.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace dftracer::utils::trace::views::detail {

namespace {

namespace df = dftracer::utils::dataframe;

bool is_top_level(std::string_view f) {
    return f == "name" || f == "cat" || f == "pid" || f == "tid" || f == "ts" ||
           f == "dur" || f == "ph";
}

// fhash/hhash are group dimensions parsed into dedicated interned-id fields
// (not `ev.args`), so the row builder resolves them the same way group_by does.
bool is_hash_field(std::string_view f) { return f == "fhash" || f == "hhash"; }

// io_cat is a computed dimension (dfanalyzer I/O category), derived per row
// from the event name, not a stored field.
bool is_iocat_field(std::string_view f) { return f == "io_cat"; }

// acc_pat is a computed dimension whose key is the constant "0" for every event
// (the engine agg path uses '0'), not a stored field.
bool is_accpat_field(std::string_view f) { return f == "acc_pat"; }

// An agg-engine group-key-string request (see native_row_fold.h). Sets `field`
// to the underlying field name and `arg_only` (append_arg vs append_value).
bool is_agg_key_field(std::string_view sel, std::string_view& field,
                      bool& arg_only) {
    if (sel.substr(0, AGG_KEY_ARG_PREFIX.size()) == AGG_KEY_ARG_PREFIX) {
        field = sel.substr(AGG_KEY_ARG_PREFIX.size());
        arg_only = true;
        return true;
    }
    if (sel.substr(0, AGG_KEY_FIELD_PREFIX.size()) == AGG_KEY_FIELD_PREFIX) {
        field = sel.substr(AGG_KEY_FIELD_PREFIX.size());
        arg_only = false;
        return true;
    }
    return false;
}

// An agg-engine numeric-arg value-column request (see native_row_fold.h). Sets
// `name` to the arg name (or the "size" pseudo-field).
bool is_num_arg_field(std::string_view sel, std::string_view& name) {
    if (sel.substr(0, AGG_NUM_ARG_PREFIX.size()) != AGG_NUM_ARG_PREFIX)
        return false;
    name = sel.substr(AGG_NUM_ARG_PREFIX.size());
    return true;
}

// An agg-engine fold-derived value-column request (see native_row_fold.h). Sets
// `name` to the derived field ("size" or "te").
bool is_derived_agg_field(std::string_view sel, std::string_view& name) {
    if (sel.substr(0, AGG_DERIVED_PREFIX.size()) != AGG_DERIVED_PREFIX)
        return false;
    name = sel.substr(AGG_DERIVED_PREFIX.size());
    return true;
}

// An Arrow-layout validity bitmap (1 = valid) from a per-row present flag;
// empty (no nulls) when every row is present.
std::vector<std::uint8_t> validity_of(const std::vector<bool>& present) {
    if (std::all_of(present.begin(), present.end(), [](bool b) { return b; }))
        return {};
    std::vector<std::uint8_t> v((present.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < present.size(); ++i)
        if (present[i]) v[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7));
    return v;
}

// The value of arg `keyid` on `ev`, or nullptr if the event lacks it.
const FoldEvent::ArgValue* find_arg(const FoldEvent& ev, std::uint32_t keyid) {
    for (const auto& [k, v] : ev.args)
        if (k == keyid) return &v;
    return nullptr;
}

df::Series u64_column(const std::vector<FoldEvent>& evs,
                      std::uint64_t FoldEvent::* field, double scale = 1.0) {
    std::vector<std::uint64_t> vals;
    vals.reserve(evs.size());
    if (scale == 1.0)
        for (const auto& ev : evs) vals.push_back(ev.*field);
    else
        for (const auto& ev : evs)
            vals.push_back(static_cast<std::uint64_t>(
                static_cast<double>(ev.*field) * scale + 0.5));
    if (vals.empty()) return df::Series::flat(df::TypeId::Uint64, nullptr, 0);
    const void* p = vals.data();
    return df::Series::from_borrowed(df::TypeId::Uint64, p, vals.size(),
                                     std::move(vals));
}

df::Series str_id_column(const std::vector<FoldEvent>& evs,
                         std::uint32_t FoldEvent::* field,
                         const dftracer::utils::StringIntern& intern) {
    std::vector<std::string_view> vals;
    std::vector<bool> present;
    vals.reserve(evs.size());
    present.reserve(evs.size());
    for (const auto& ev : evs) {
        const std::uint32_t id = ev.*field;
        if (id == dftracer::utils::StringIntern::NO_ID) {
            vals.emplace_back();
            present.push_back(false);
        } else {
            vals.push_back(intern.resolve(id));
            present.push_back(true);
        }
    }
    auto vbits = validity_of(present);
    return df::Series::strings(std::span<const std::string_view>(vals),
                               vbits.empty() ? nullptr : vbits.data());
}

// Build one arg column: int64 unless a real forces Float64 or a string value
// forces String; a row lacking the key (or, in the String case, nothing) is
// null. A nested/extra field is captured under its full name and a flat arg
// under its bare key, so `keyid` (full) is tried first and `keyid_alt` (bare)
// as a fallback, matching PodSource::find_arg; pass NO_ID for no fallback.
df::Series arg_column(const std::vector<FoldEvent>& evs, std::uint32_t keyid,
                      std::uint32_t keyid_alt,
                      const dftracer::utils::StringIntern& intern) {
    auto lookup = [&](const FoldEvent& ev) -> const FoldEvent::ArgValue* {
        if (const auto* v = find_arg(ev, keyid)) return v;
        return keyid_alt == dftracer::utils::StringIntern::NO_ID
                   ? nullptr
                   : find_arg(ev, keyid_alt);
    };
    bool any_str = false, any_dbl = false;
    for (const auto& ev : evs)
        if (const auto* v = lookup(ev)) {
            if (std::holds_alternative<std::uint32_t>(*v))
                any_str = true;
            else if (std::holds_alternative<double>(*v))
                any_dbl = true;
        }

    const std::int64_t n = static_cast<std::int64_t>(evs.size());
    std::vector<bool> present;
    present.reserve(evs.size());

    if (any_str) {
        // Mixed numeric/string collapses to String (numbers stringified).
        std::vector<std::string> owned;  // keeps stringified numbers alive
        owned.reserve(evs.size());
        std::vector<std::string_view> vals;
        vals.reserve(evs.size());
        for (const auto& ev : evs) {
            const auto* v = lookup(ev);
            if (!v) {
                owned.emplace_back();
                present.push_back(false);
            } else if (const auto* s = std::get_if<std::uint32_t>(v)) {
                owned.emplace_back(intern.resolve(*s));
                present.push_back(true);
            } else if (const auto* i = std::get_if<std::int64_t>(v)) {
                owned.push_back(std::to_string(*i));
                present.push_back(true);
            } else {
                owned.push_back(std::to_string(std::get<double>(*v)));
                present.push_back(true);
            }
        }
        for (const auto& s : owned) vals.emplace_back(s);
        auto vbits = validity_of(present);
        return df::Series::strings(std::span<const std::string_view>(vals),
                                   vbits.empty() ? nullptr : vbits.data());
    }

    if (any_dbl) {
        std::vector<double> vals;
        vals.reserve(evs.size());
        for (const auto& ev : evs) {
            const auto* v = lookup(ev);
            if (!v) {
                vals.push_back(0.0);
                present.push_back(false);
            } else if (const auto* d = std::get_if<double>(v)) {
                vals.push_back(*d);
                present.push_back(true);
            } else {
                vals.push_back(static_cast<double>(std::get<std::int64_t>(*v)));
                present.push_back(true);
            }
        }
        auto vbits = validity_of(present);
        return df::Series::flat(df::TypeId::Float64, vals.data(), n,
                                vbits.empty() ? nullptr : vbits.data());
    }

    std::vector<std::int64_t> vals;
    vals.reserve(evs.size());
    for (const auto& ev : evs) {
        const auto* v = lookup(ev);
        if (const auto* i = v ? std::get_if<std::int64_t>(v) : nullptr) {
            vals.push_back(*i);
            present.push_back(true);
        } else {
            vals.push_back(0);
            present.push_back(false);
        }
    }
    auto vbits = validity_of(present);
    return df::Series::flat(df::TypeId::Int64, vals.data(), n,
                            vbits.empty() ? nullptr : vbits.data());
}

df::Series top_column(const std::vector<FoldEvent>& evs, std::string_view name,
                      const dftracer::utils::StringIntern& intern,
                      double time_scale) {
    if (name == "name") return str_id_column(evs, &FoldEvent::name_id, intern);
    if (name == "cat") return str_id_column(evs, &FoldEvent::cat_id, intern);
    if (name == "pid") return u64_column(evs, &FoldEvent::pid);
    if (name == "tid") return u64_column(evs, &FoldEvent::tid);
    if (name == "ts") return u64_column(evs, &FoldEvent::ts, time_scale);
    if (name == "dur") return u64_column(evs, &FoldEvent::dur, time_scale);
    // ph
    std::vector<std::int64_t> vals;
    vals.reserve(evs.size());
    for (const auto& ev : evs)
        vals.push_back(static_cast<std::int64_t>(ev.phase));
    return df::Series::flat(df::TypeId::Int64, vals.data(),
                            static_cast<std::int64_t>(vals.size()));
}

df::Series hash_column(const std::vector<FoldEvent>& evs, std::string_view f,
                       const dftracer::utils::StringIntern& intern) {
    return f == "fhash" ? str_id_column(evs, &FoldEvent::fhash_id, intern)
                        : str_id_column(evs, &FoldEvent::hhash_id, intern);
}

// The dfanalyzer I/O category enum value per event, from the event name. Kept
// an Int64 (the enum's integer, matching the engine agg path's i64 form) so
// the group-by collapses and renders it identically to a numeric key.
df::Series iocat_column(const std::vector<FoldEvent>& evs,
                        const dftracer::utils::StringIntern& intern) {
    std::vector<std::int64_t> vals;
    vals.reserve(evs.size());
    for (const auto& ev : evs) {
        const std::string_view name =
            ev.name_id == dftracer::utils::StringIntern::NO_ID
                ? std::string_view{}
                : intern.resolve(ev.name_id);
        vals.push_back(static_cast<std::int64_t>(
            static_cast<int>(trace::internal::io_category(name))));
    }
    return df::Series::flat(df::TypeId::Int64, vals.data(),
                            static_cast<std::int64_t>(vals.size()));
}

// The acc_pat group key: the constant "0" String for every event, matching the
// engine agg path (a constant '0'). A single-group, byte-identical key.
df::Series accpat_column(const std::vector<FoldEvent>& evs) {
    return df::Series::strings(std::vector<std::string>(evs.size(), "0"));
}

// A group-key string column rendered exactly as the engine agg path builds its
// key (PodSource append_arg for an Arg key, append_value for a Field key), so
// the engine group-by is byte-identical: a missing value is the empty string,
// numbers are stringified.
df::Series group_key_str_column(const std::vector<FoldEvent>& evs,
                                const dftracer::utils::StringIntern& intern,
                                std::string_view field, bool arg_only) {
    std::vector<std::string> vals;
    vals.reserve(evs.size());
    std::string buf;
    for (const auto& ev : evs) {
        buf.clear();
        PodSource src(ev, intern);
        if (arg_only)
            src.append_arg(buf, field);
        else
            src.append_value(buf, field);
        vals.push_back(buf);
    }
    return df::Series::strings(vals);
}

// One auto-discovered numeric arg as a Float64 value column for the agg
// engine's dyn path: the arg's numeric value where the event carries it as a
// number (int64 or double), null otherwise. Matches the engine agg path's
// per-arg FieldStat, fed only by PodSource::for_each_numeric_arg (string args
// contribute nothing). The "size" pseudo-field resolves to the io-cat-derived
// byte size (derived_size_t), falling back to a literal numeric "size" arg, so
// the discovered "size" metric matches fold_numeric_args_t.
df::Series num_arg_column(const std::vector<FoldEvent>& evs,
                          std::string_view name,
                          const dftracer::utils::StringIntern& intern) {
    const bool is_size = name == "size";
    const std::uint32_t keyid =
        const_cast<dftracer::utils::StringIntern&>(intern).get_or_insert(name);
    const std::int64_t n = static_cast<std::int64_t>(evs.size());
    std::vector<double> vals;
    std::vector<bool> present;
    vals.reserve(evs.size());
    present.reserve(evs.size());
    for (const auto& ev : evs) {
        std::optional<double> num;
        if (is_size) {
            PodSource src(ev, intern);
            num = derived_size_t(src);
        }
        if (!num) {
            if (const auto* v = find_arg(ev, keyid)) {
                if (const auto* d = std::get_if<double>(v))
                    num = *d;
                else if (const auto* i = std::get_if<std::int64_t>(v))
                    num = static_cast<double>(*i);
            }
        }
        if (num) {
            vals.push_back(*num);
            present.push_back(true);
        } else {
            vals.push_back(0.0);
            present.push_back(false);
        }
    }
    auto vbits = validity_of(present);
    return df::Series::flat(df::TypeId::Float64, vals.data(), n,
                            vbits.empty() ? nullptr : vbits.data());
}

// One fold-derived field as a Uint64 value column for the agg engine's value
// path, keeping the U64 domain agg_field_typed_t assigns so a Sum/Min/Max
// matches the engine agg path: "size" is the io-cat-derived byte size
// (derived_size_t), "te" is ts+dur (null when the event has no dur, matching
// number_typed("dur")). ts/dur are read RAW; a non-identity time_scale is
// reapplied on the engine side (SCALED_TE_COL), never baked in here.
df::Series derived_agg_column(const std::vector<FoldEvent>& evs,
                              std::string_view name,
                              const dftracer::utils::StringIntern& intern) {
    const bool is_size = name == "size";
    const std::int64_t n = static_cast<std::int64_t>(evs.size());
    std::vector<std::uint64_t> vals;
    std::vector<bool> present;
    vals.reserve(evs.size());
    present.reserve(evs.size());
    for (const auto& ev : evs) {
        std::optional<std::uint64_t> num;
        if (is_size) {
            PodSource src(ev, intern);
            if (auto s = derived_size_t(src))
                num = static_cast<std::uint64_t>(*s);
        } else if (ev.has_dur) {
            num = ev.ts + ev.dur;
        }
        if (num) {
            vals.push_back(*num);
            present.push_back(true);
        } else {
            vals.push_back(0);
            present.push_back(false);
        }
    }
    auto vbits = validity_of(present);
    return df::Series::flat(df::TypeId::Uint64, vals.data(), n,
                            vbits.empty() ? nullptr : vbits.data());
}

// A resolved.* / r.* virtual field maps to a hash field resolved through the
// index name tables (fpath <- fhash, hostname/host <- hhash).
enum class ResolvedKind { None, File, Host };
ResolvedKind resolved_kind(std::string_view f) {
    std::string_view rest;
    if (f.rfind("resolved.", 0) == 0)
        rest = f.substr(9);
    else if (f.rfind("r.", 0) == 0)
        rest = f.substr(2);
    else
        return ResolvedKind::None;
    if (rest == "fpath") return ResolvedKind::File;
    if (rest == "hostname" || rest == "host") return ResolvedKind::Host;
    return ResolvedKind::None;
}

// An all-null String column of length n (validity all zero).
df::Series null_string_column(std::size_t n) {
    std::vector<std::uint8_t> vbits((n + 7) / 8, 0);
    return df::Series::strings(std::vector<std::string_view>(n),
                               n ? vbits.data() : nullptr);
}

// Resolve each event's fhash/hhash through the name tables to its path/host
// string; null where the hash field is unset or the name is unknown.
df::Series resolved_column(const std::vector<FoldEvent>& evs, ResolvedKind kind,
                           const dftracer::utils::StringIntern& intern,
                           const GroupResolver& resolver) {
    std::vector<std::string_view> vals;
    std::vector<bool> present;
    vals.reserve(evs.size());
    present.reserve(evs.size());
    for (const auto& ev : evs) {
        const std::uint32_t id =
            kind == ResolvedKind::File ? ev.fhash_id : ev.hhash_id;
        if (id == dftracer::utils::StringIntern::NO_ID) {
            vals.emplace_back();
            present.push_back(false);
            continue;
        }
        const std::string hash(intern.resolve(id));
        const std::string& s = kind == ResolvedKind::File
                                   ? resolver.file_path(hash)
                                   : resolver.host_name(hash);
        if (s.empty()) {
            vals.emplace_back();
            present.push_back(false);
        } else {
            vals.emplace_back(s);
            present.push_back(true);
        }
    }
    auto vbits = validity_of(present);
    return df::Series::strings(std::span<const std::string_view>(vals),
                               vbits.empty() ? nullptr : vbits.data());
}

bool any_hash_present(const std::vector<FoldEvent>& evs,
                      std::uint32_t FoldEvent::* field) {
    for (const auto& ev : evs)
        if (ev.*field != dftracer::utils::StringIntern::NO_ID) return true;
    return false;
}

}  // namespace

bool select_needs_resolver(const std::vector<std::string>& select) {
    for (const std::string& s : select)
        if (resolved_kind(s) != ResolvedKind::None) return true;
    return false;
}

std::vector<std::string> row_fold_extra_captures(
    const std::vector<std::string>& select) {
    auto is_pod_scalar = [](std::string_view f) {
        return f == "name" || f == "cat" || f == "pid" || f == "tid" ||
               f == "ts" || f == "dur";
    };
    std::vector<std::string> out;
    auto add = [&](std::string_view f) {
        if (f.empty() || is_pod_scalar(f)) return;
        for (const auto& e : out)
            if (e == f) return;
        out.emplace_back(f);
    };
    for (const std::string& sel : select) {
        std::string_view f = sel;
        std::string_view uf;
        bool ao = false;
        if (is_agg_key_field(sel, uf, ao)) f = uf;
        // A numeric-arg value column captures its underlying arg; "size" is
        // derived from other args (ret/size_sum/image_size, already captured by
        // needs_args), so it is computed, not captured raw.
        std::string_view nf;
        if (is_num_arg_field(sel, nf)) {
            if (nf == "size") continue;
            f = nf;
        }
        // size (from ret/size_sum/image_size, captured by needs_args) and te
        // (from ts/dur scalars) are derived, not captured raw.
        std::string_view df_name;
        if (is_derived_agg_field(sel, df_name)) continue;
        // resolved.*/r.*, io_cat and acc_pat are computed, not captured raw.
        if (resolved_kind(f) != ResolvedKind::None || is_iocat_field(f) ||
            is_accpat_field(f))
            continue;
        add(f);
    }
    return out;
}

std::string canonical_row_column_name(std::string_view sel) {
    std::string_view f;
    bool arg_only = false;
    std::string_view num_name;
    std::string_view deriv_name;
    if (is_agg_key_field(sel, f, arg_only)) return std::string(sel);
    if (is_num_arg_field(sel, num_name)) return std::string(sel);
    if (is_derived_agg_field(sel, deriv_name)) return std::string(deriv_name);
    if (is_top_level(sel)) return std::string(sel);
    if (resolved_kind(sel) != ResolvedKind::None) return std::string(sel);
    if (is_iocat_field(sel)) return std::string(sel);
    if (is_accpat_field(sel)) return std::string(sel);
    const std::string_view key = strip_args_prefix(sel);
    if (is_hash_field(key)) return std::string(key);
    return std::string(dftracer::utils::ARGS_PREFIX) + std::string(key);
}

dataframe::TypeId row_column_type(std::string_view sel) {
    std::string_view f;
    bool arg_only = false;
    std::string_view num_name;
    std::string_view deriv_name;
    if (is_agg_key_field(sel, f, arg_only)) return df::TypeId::String;
    if (is_num_arg_field(sel, num_name)) return df::TypeId::Float64;
    if (is_derived_agg_field(sel, deriv_name)) return df::TypeId::Uint64;
    if (is_top_level(sel)) {
        if (sel == "name" || sel == "cat") return df::TypeId::String;
        if (sel == "ph") return df::TypeId::Int64;
        return df::TypeId::Uint64;  // pid, tid, ts, dur
    }
    if (resolved_kind(sel) != ResolvedKind::None) return df::TypeId::String;
    if (is_iocat_field(sel)) return df::TypeId::Int64;
    if (is_accpat_field(sel)) return df::TypeId::String;
    const std::string_view key = strip_args_prefix(sel);
    if (is_hash_field(key)) return df::TypeId::String;
    return df::TypeId::Unknown;  // a flattened arg: type is data-dependent
}

dataframe::DataFrame build_row_frame(
    const std::vector<FoldEvent>& evs,
    const dftracer::utils::StringIntern& intern,
    const std::vector<std::string>& select, double time_scale,
    const GroupResolver* resolver) {
    df::DataFrame out;
    const dftracer::utils::StringIntern* intern_ = &intern;
    const std::vector<std::string>& select_ = select;

    if (select_.empty()) {
        // Every column: fixed top-level order, then the sorted union of arg
        // keys, so the schema is deterministic across runs.
        for (const char* c : {"name", "cat", "pid", "tid", "ts", "dur", "ph"}) {
            out.names.emplace_back(c);
            out.columns.push_back(top_column(evs, c, *intern_, time_scale));
        }
        // fhash/hhash live in dedicated fields, not `ev.args`; emit them when
        // any event carries one so they are readable per-event, not just as
        // group_by keys.
        if (any_hash_present(evs, &FoldEvent::fhash_id)) {
            out.names.emplace_back("fhash");
            out.columns.push_back(hash_column(evs, "fhash", *intern_));
        }
        if (any_hash_present(evs, &FoldEvent::hhash_id)) {
            out.names.emplace_back("hhash");
            out.columns.push_back(hash_column(evs, "hhash", *intern_));
        }
        std::set<std::uint32_t> keys;
        for (const auto& ev : evs)
            for (const auto& [k, v] : ev.args) {
                (void)v;
                keys.insert(k);
            }
        std::vector<std::pair<std::string, std::uint32_t>> named;
        named.reserve(keys.size());
        for (std::uint32_t k : keys)
            named.emplace_back(std::string(intern_->resolve(k)), k);
        std::sort(named.begin(), named.end());
        for (const auto& [nm, k] : named) {
            out.names.push_back(std::string(dftracer::utils::ARGS_PREFIX) + nm);
            out.columns.push_back(arg_column(
                evs, k, dftracer::utils::StringIntern::NO_ID, *intern_));
        }
    } else {
        for (const std::string& sel : select_) {
            out.names.push_back(canonical_row_column_name(sel));
            std::string_view agg_key_f;
            bool agg_key_arg_only = false;
            std::string_view num_arg_name;
            std::string_view deriv_name;
            if (is_agg_key_field(sel, agg_key_f, agg_key_arg_only)) {
                out.columns.push_back(group_key_str_column(
                    evs, *intern_, agg_key_f, agg_key_arg_only));
            } else if (is_num_arg_field(sel, num_arg_name)) {
                out.columns.push_back(
                    num_arg_column(evs, num_arg_name, *intern_));
            } else if (is_derived_agg_field(sel, deriv_name)) {
                out.columns.push_back(
                    derived_agg_column(evs, deriv_name, *intern_));
            } else if (is_top_level(sel)) {
                out.columns.push_back(
                    top_column(evs, sel, *intern_, time_scale));
            } else if (const ResolvedKind rk = resolved_kind(sel);
                       rk != ResolvedKind::None) {
                // resolved.*/r.* virtual fields: resolve fhash/hhash to a name;
                // an all-null column when no index name table is loaded.
                out.columns.push_back(
                    resolver ? resolved_column(evs, rk, *intern_, *resolver)
                             : null_string_column(evs.size()));
            } else if (is_iocat_field(sel)) {
                out.columns.push_back(iocat_column(evs, *intern_));
            } else if (is_accpat_field(sel)) {
                out.columns.push_back(accpat_column(evs));
            } else if (const std::string_view key = strip_args_prefix(sel);
                       is_hash_field(key)) {
                out.columns.push_back(hash_column(evs, key, *intern_));
            } else {
                // A nested/extra field (args.n.v) is captured under its full
                // name, a flat arg under its bare key; try the full name first,
                // then the stripped key, matching PodSource::find_arg.
                auto& mut =
                    const_cast<dftracer::utils::StringIntern&>(*intern_);
                const std::uint32_t id_full = mut.get_or_insert(sel);
                const std::uint32_t id_bare = mut.get_or_insert(key);
                out.columns.push_back(
                    arg_column(evs, id_full, id_bare, *intern_));
            }
        }
    }

    return out;
}

std::vector<std::pair<std::string, dataframe::Series>>
build_dyn_numeric_columns(const std::vector<FoldEvent>& evs,
                          const dftracer::utils::StringIntern& intern) {
    namespace agg = trace::aggregators;
    // Discover the numeric-arg names present in this batch, matching
    // fold_numeric_args_t: the io-cat-derived "size" plus every non-reserved,
    // non-preagg numeric arg. Sorted (std::set) is not required (the agg
    // finalize sorts the name union), but keeps a deterministic layout.
    std::set<std::string> names;
    for (const FoldEvent& ev : evs) {
        PodSource src(ev, intern);
        if (derived_size_t(src)) names.insert("size");
        src.for_each_numeric_arg([&](std::string_view key, double) {
            if (agg::is_reserved_arg(key) || agg::is_preagg_suffix(key)) return;
            names.insert(std::string(key));
        });
    }
    std::vector<std::pair<std::string, dataframe::Series>> out;
    out.reserve(names.size());
    for (const std::string& name : names)
        out.emplace_back(std::string(AGG_NUM_ARG_PREFIX) + name,
                         num_arg_column(evs, name, intern));
    return out;
}

dataframe::DataFrame NativeRowFold::build() {
    dataframe::DataFrame out = build_row_frame(events_, *intern_, select_,
                                               time_scale_, resolver_.get());
    events_.clear();
    return out;
}

}  // namespace dftracer::utils::trace::views::detail
