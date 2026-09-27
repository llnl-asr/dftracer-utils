#include <dftracer/utils/core/common/field_ref.h>
#include <dftracer/utils/core/common/to_chars.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/index/schemas/dft/agg/reserved_args.h>
#include <dftracer/utils/json/canonical.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/agg_fold.h>
#include <dftracer/utils/trace/views/event_source.h>
#include <dftracer/utils/trace/views/native_row_fold.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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
    if (sel.starts_with(AGG_KEY_JSON_PREFIX)) {
        field = sel.substr(AGG_KEY_JSON_PREFIX.size());
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

constexpr std::string_view WINDOW_PREFIX = "__win:";
constexpr std::string_view CLIP_PREFIX = "__clip:";
constexpr std::string_view LIST_PREFIX = "__list:";

// "__list:<spec>:<path>", as list_token writes it.
bool parse_list_sel(std::string_view sel, std::string_view& spec,
                    std::string_view& path) {
    if (!sel.starts_with(LIST_PREFIX)) return false;
    const std::string_view s = sel.substr(LIST_PREFIX.size());
    const std::size_t colon = s.find(':');
    if (colon == std::string_view::npos) return false;
    spec = s.substr(0, colon);
    path = s.substr(colon + 1);
    return true;
}

struct WindowSel {
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    std::string_view rest;
};

// "<prefix><lo>:<hi>:<rest>", as window_token and clip_token write it.
bool parse_window_sel(std::string_view sel, std::string_view prefix,
                      WindowSel& w) {
    if (sel.substr(0, prefix.size()) != prefix) return false;
    const std::string_view s = sel.substr(prefix.size());
    const std::size_t a = s.find(':');
    if (a == std::string_view::npos) return false;
    const std::size_t b = s.find(':', a + 1);
    if (b == std::string_view::npos) return false;
    std::from_chars(s.data(), s.data() + a, w.lo);
    std::from_chars(s.data() + a + 1, s.data() + b, w.hi);
    w.rest = s.substr(b + 1);
    return true;
}

std::string window_sel(std::string_view prefix, std::uint64_t lo,
                       std::uint64_t hi, std::string_view rest) {
    std::string out(prefix);
    out += std::to_string(lo);
    out += ':';
    out += std::to_string(hi);
    out += ':';
    out += rest;
    return out;
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

// One array element or struct field value.
struct Scalar {
    enum class K : std::uint8_t { NUL, BOOL, INT, DBL, STR, JSON };
    K k = K::NUL;
    std::int64_t i = 0;
    double d = 0;
    std::string s;
};

Scalar scalar_of(simdjson::dom::element e) {
    using T = simdjson::dom::element_type;
    Scalar out;
    switch (e.type()) {
        case T::BOOL:
            out.k = Scalar::K::BOOL;
            out.i = e.get_bool().value_unsafe() ? 1 : 0;
            break;
        case T::INT64:
            out.k = Scalar::K::INT;
            out.i = e.get_int64().value_unsafe();
            break;
        case T::UINT64: {
            const std::uint64_t u = e.get_uint64().value_unsafe();
            if (u <= static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max())) {
                out.k = Scalar::K::INT;
                out.i = static_cast<std::int64_t>(u);
            } else {
                out.k = Scalar::K::DBL;
                out.d = static_cast<double>(u);
            }
            break;
        }
        case T::DOUBLE:
            out.k = Scalar::K::DBL;
            out.d = e.get_double().value_unsafe();
            break;
        case T::STRING:
            out.k = Scalar::K::STR;
            out.s = e.get_string().value_unsafe();
            break;
        case T::ARRAY:
        case T::OBJECT:
            out.k = Scalar::K::JSON;
            dftracer::utils::json::append_canonical_json(out.s, e);
            break;
        case T::BIGINT:
            out.k = Scalar::K::STR;
            out.s = e.get_bigint().value_unsafe();
            break;
        default:
            break;
    }
    return out;
}

// The letter the values share: i, f, s, b, or j when they mix.
char infer_kind(const std::vector<Scalar>& vals) {
    bool i = false, d = false, s = false, b = false, j = false;
    for (const Scalar& v : vals) {
        i |= v.k == Scalar::K::INT;
        d |= v.k == Scalar::K::DBL;
        s |= v.k == Scalar::K::STR;
        b |= v.k == Scalar::K::BOOL;
        j |= v.k == Scalar::K::JSON;
    }
    if (j ||
        static_cast<int>(i || d) + static_cast<int>(s) + static_cast<int>(b) >
            1)
        return 'j';
    if (d) return 'f';
    if (i) return 'i';
    if (b) return 'b';
    return 's';
}

df::Series column_of(const std::vector<Scalar>& vals, char kind) {
    const auto n = static_cast<std::int64_t>(vals.size());
    std::vector<bool> present(vals.size(), false);
    if (kind == 'i' || kind == 'f') {
        std::vector<std::int64_t> is(vals.size(), 0);
        std::vector<double> ds(vals.size(), 0);
        for (std::size_t r = 0; r < vals.size(); ++r) {
            const Scalar& v = vals[r];
            if (v.k == Scalar::K::INT) {
                is[r] = v.i;
                ds[r] = static_cast<double>(v.i);
                present[r] = true;
            } else if (v.k == Scalar::K::DBL && kind == 'f') {
                ds[r] = v.d;
                present[r] = true;
            }
        }
        auto vbits = validity_of(present);
        const std::uint8_t* valid = vbits.empty() ? nullptr : vbits.data();
        return kind == 'i' ? df::Series::flat_i64(is.data(), n, valid)
                           : df::Series::flat_f64(ds.data(), n, valid);
    }
    if (kind == 'b') {
        std::vector<std::uint8_t> bits((vals.size() + 7) / 8, 0);
        for (std::size_t r = 0; r < vals.size(); ++r) {
            if (vals[r].k != Scalar::K::BOOL) continue;
            present[r] = true;
            if (vals[r].i)
                bits[r >> 3] |= static_cast<std::uint8_t>(1u << (r & 7));
        }
        auto vbits = validity_of(present);
        return df::Series::flat(df::TypeId::Bool, bits.data(), n,
                                vbits.empty() ? nullptr : vbits.data());
    }
    std::deque<std::string> owned;
    std::vector<std::string_view> text(vals.size());
    for (std::size_t r = 0; r < vals.size(); ++r) {
        const Scalar& v = vals[r];
        if (v.k == Scalar::K::NUL || (kind == 's' && v.k != Scalar::K::STR))
            continue;
        present[r] = true;
        if (v.k == Scalar::K::STR && kind == 's') {
            text[r] = v.s;
            continue;
        }
        std::string& t = owned.emplace_back();
        switch (v.k) {
            case Scalar::K::STR:
                t += '"';
                dftracer::utils::json::append_json_escaped(t, v.s);
                t += '"';
                break;
            case Scalar::K::BOOL:
                t = v.i ? "true" : "false";
                break;
            case Scalar::K::INT:
                t = std::to_string(v.i);
                break;
            case Scalar::K::DBL:
                t = double_text(v.d);
                break;
            default:
                t = v.s;
        }
        text[r] = t;
    }
    auto vbits = validity_of(present);
    return df::Series::strings(std::span<const std::string_view>(text),
                               vbits.empty() ? nullptr : vbits.data());
}

// The array at `path` as JSON text, or nullopt when the event holds no
// array there.
// `a[0].b` as the flattened key `a.0.b` events store it under.
std::string flat_key(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (const char c : path) {
        if (c == '[')
            out += '.';
        else if (c != ']')
            out += c;
    }
    return out;
}

std::optional<std::string> array_text(
    const FoldEvent& ev, const dftracer::utils::StringIntern& intern,
    std::string_view at) {
    const std::string path = flat_key(at);
    PodSource src(ev, intern);
    if (const auto* sp = src.special(path)) {
        if (*sp == FoldEvent::Special::EMPTY_ARRAY) return "[]";
        if (*sp == FoldEvent::Special::JSON_ARRAY)
            return std::string(src.value(path));
        return std::nullopt;
    }
    if (src.has(path)) return std::nullopt;
    auto cell = container_cell(ev, intern, path);
    if (!cell || cell->kind != duql::Cell::Kind::ARRAY) return std::nullopt;
    return std::get<std::string>(std::move(cell->value));
}

df::Series list_column(const std::vector<FoldEvent>& evs,
                       const dftracer::utils::StringIntern& intern,
                       std::string_view spec, std::string_view path) {
    std::vector<std::pair<std::string, char>> fields;
    const bool fixed_struct = spec.starts_with('{');
    if (fixed_struct) {
        std::string_view body = spec.substr(1, spec.size() - 2);
        while (!body.empty()) {
            const std::size_t comma = std::min(body.find(','), body.size());
            const std::string_view f = body.substr(0, comma);
            const std::size_t eq = f.find('=');
            fields.emplace_back(std::string(f.substr(0, eq)),
                                eq + 1 < f.size() ? f[eq + 1] : 'j');
            body.remove_prefix(std::min(comma + 1, body.size()));
        }
    }
    dftracer::utils::json::RecordParser parser;
    std::vector<std::int32_t> offsets{0};
    std::vector<std::int64_t> rows;
    bool null_row = false;
    std::vector<Scalar> elems;
    std::vector<std::vector<std::pair<std::string, Scalar>>> objects;
    for (std::size_t r = 0; r < evs.size(); ++r) {
        const auto text = array_text(evs[r], intern, path);
        simdjson::dom::element root;
        simdjson::dom::array arr;
        if (!text || parser.parse(*text).get(root) != simdjson::SUCCESS ||
            root.get_array().get(arr) != simdjson::SUCCESS) {
            offsets.push_back(offsets.back());
            rows.push_back(-1);
            null_row = true;
            continue;
        }
        for (auto el : arr) {
            elems.push_back(scalar_of(el));
            if (!fixed_struct) continue;
            auto& obj = objects.emplace_back();
            simdjson::dom::object o;
            if (el.get_object().get(o) == simdjson::SUCCESS)
                for (auto kv : o) obj.emplace_back(kv.key, scalar_of(kv.value));
        }
        offsets.push_back(static_cast<std::int32_t>(elems.size()));
        rows.push_back(static_cast<std::int64_t>(r));
    }
    df::Series values;
    if (fixed_struct) {
        std::vector<std::string> names;
        std::vector<df::Series> cols;
        for (const auto& [name, kind] : fields) {
            std::vector<Scalar> vals(objects.size());
            for (std::size_t e = 0; e < objects.size(); ++e)
                for (auto& [k, v] : objects[e])
                    if (k == name) vals[e] = v;
            names.push_back(name);
            cols.push_back(column_of(vals, kind));
        }
        values = df::Series::structs(std::move(names), std::move(cols));
    } else {
        values = column_of(elems, spec == "a" ? infer_kind(elems) : spec[0]);
    }
    df::Series list = df::Series::list(offsets, std::move(values));
    if (null_row) return list.take(rows);
    return list;
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

// The column type a set of arg values needs: int64 while every value is one,
// uint64 once a value is above int64 and none is negative, float64 once a
// real (or a negative next to a uint64) appears, string when every value is
// text, and JSON once text and numbers mix.
struct ArgType {
    enum class Kind : std::uint8_t { Int, Uint, Dbl, Str, Json };
    Kind kind = Kind::Int;
    bool negative = false;
    bool number = false;

    void see(const FoldEvent::ArgValue& v) {
        if (kind == Kind::Json) return;
        const bool text = std::holds_alternative<std::uint32_t>(v);
        if (kind == Kind::Str) {
            if (!text) kind = Kind::Json;
            return;
        }
        if (text) {
            kind = number ? Kind::Json : Kind::Str;
            return;
        }
        number = true;
        if (std::holds_alternative<double>(v)) {
            kind = Kind::Dbl;
        } else if (const auto* i = std::get_if<std::int64_t>(&v)) {
            negative = negative || *i < 0;
            if (kind == Kind::Uint && negative) kind = Kind::Dbl;
        } else if (kind == Kind::Int) {
            kind = negative ? Kind::Dbl : Kind::Uint;
        }
    }
};

double arg_double(const FoldEvent::ArgValue& v) {
    return std::visit([](auto x) { return static_cast<double>(x); }, v);
}

std::uint64_t arg_uint(const FoldEvent::ArgValue& v) {
    if (const auto* u = std::get_if<std::uint64_t>(&v)) return *u;
    return static_cast<std::uint64_t>(std::get<std::int64_t>(v));
}

// A number as JSON text: integers exactly, doubles in shortest round-trip
// form.
std::string number_text(const FoldEvent::ArgValue& v) {
    if (const auto* i = std::get_if<std::int64_t>(&v))
        return std::to_string(*i);
    if (const auto* u = std::get_if<std::uint64_t>(&v))
        return std::to_string(*u);
    return dftracer::utils::double_text(std::get<double>(v));
}

// The column of `vals` (one per row, null when absent) as `type`.
df::Series typed_arg_column(const std::vector<const FoldEvent::ArgValue*>& vals,
                            const ArgType& type,
                            const dftracer::utils::StringIntern& intern) {
    using Kind = ArgType::Kind;
    const auto n = static_cast<std::int64_t>(vals.size());
    std::vector<bool> present(vals.size());
    for (std::size_t r = 0; r < vals.size(); ++r) present[r] = vals[r];
    auto vbits = validity_of(present);
    const std::uint8_t* valid = vbits.empty() ? nullptr : vbits.data();
    switch (type.kind) {
        case Kind::Json: {
            std::vector<std::string> cells(vals.size());
            std::vector<std::string_view> out(vals.size());
            for (std::size_t r = 0; r < vals.size(); ++r) {
                if (!vals[r]) continue;
                if (const auto* id = std::get_if<std::uint32_t>(vals[r])) {
                    cells[r] += '"';
                    dftracer::utils::json::append_json_escaped(
                        cells[r], intern.resolve(*id));
                    cells[r] += '"';
                } else {
                    cells[r] = number_text(*vals[r]);
                }
                out[r] = cells[r];
            }
            return df::Series::strings(std::span<const std::string_view>(out),
                                       valid)
                .as_json();
        }
        case Kind::Str: {
            // Interned strings are views into stable intern storage; only
            // number text needs owning, in a deque so views stay valid.
            std::deque<std::string> owned;
            std::vector<std::string_view> out(vals.size());
            for (std::size_t r = 0; r < vals.size(); ++r) {
                if (!vals[r]) continue;
                if (const auto* id = std::get_if<std::uint32_t>(vals[r]))
                    out[r] = intern.resolve(*id);
                else
                    out[r] = owned.emplace_back(number_text(*vals[r]));
            }
            return df::Series::strings(std::span<const std::string_view>(out),
                                       valid);
        }
        case Kind::Dbl: {
            std::vector<double> out(vals.size(), 0.0);
            for (std::size_t r = 0; r < vals.size(); ++r)
                if (vals[r]) out[r] = arg_double(*vals[r]);
            return df::Series::flat(df::TypeId::Float64, out.data(), n, valid);
        }
        case Kind::Uint: {
            std::vector<std::uint64_t> out(vals.size(), 0);
            for (std::size_t r = 0; r < vals.size(); ++r)
                if (vals[r]) out[r] = arg_uint(*vals[r]);
            return df::Series::flat(df::TypeId::Uint64, out.data(), n, valid);
        }
        case Kind::Int: {
            std::vector<std::int64_t> out(vals.size(), 0);
            for (std::size_t r = 0; r < vals.size(); ++r)
                if (vals[r]) out[r] = std::get<std::int64_t>(*vals[r]);
            return df::Series::flat(df::TypeId::Int64, out.data(), n, valid);
        }
    }
    return df::Series::nulls(df::TypeId::Int64, n);
}

// Build one arg column, typed as ArgType says; a row lacking the key is null.
// A nested/extra field is captured under its full name and a flat arg under
// its bare key, so `keyid` (full) is tried first and `keyid_alt` (bare) as a
// fallback, matching PodSource::find_arg; pass NO_ID for no fallback.
df::Series arg_column(const std::vector<FoldEvent>& evs, std::uint32_t keyid,
                      std::uint32_t keyid_alt,
                      const dftracer::utils::StringIntern& intern) {
    std::vector<const FoldEvent::ArgValue*> vals;
    vals.reserve(evs.size());
    ArgType type;
    for (const auto& ev : evs) {
        const auto* v = find_arg(ev, keyid);
        if (!v && keyid_alt != dftracer::utils::StringIntern::NO_ID)
            v = find_arg(ev, keyid_alt);
        if (v) type.see(*v);
        vals.push_back(v);
    }
    return typed_arg_column(vals, type, intern);
}

// Every arg column of the empty select, named "args.<key>" in sorted key order,
// in two passes over the events rather than one pass per column. Each column is
// typed and filled exactly as arg_column(evs, key, NO_ID) would.
void append_all_arg_columns(const std::vector<FoldEvent>& evs,
                            const dftracer::utils::StringIntern& intern,
                            df::DataFrame& out, std::string_view prefix) {
    struct Col {
        std::uint32_t key = 0;
        ArgType type;
        std::vector<const FoldEvent::ArgValue*> vals;
    };
    std::unordered_map<std::uint32_t, std::uint32_t> slot;
    std::vector<Col> cols;
    const std::size_t n = evs.size();
    for (std::size_t r = 0; r < n; ++r)
        for (const auto& [k, v] : evs[r].args) {
            auto [it, inserted] =
                slot.emplace(k, static_cast<std::uint32_t>(cols.size()));
            if (inserted) {
                Col& fresh = cols.emplace_back();
                fresh.key = k;
                fresh.vals.assign(n, nullptr);
            }
            Col& c = cols[it->second];
            if (c.vals[r]) continue;
            c.vals[r] = &v;
            c.type.see(v);
        }
    if (cols.empty()) return;

    std::vector<std::pair<std::string_view, std::uint32_t>> order;
    order.reserve(cols.size());
    for (std::uint32_t c = 0; c < cols.size(); ++c)
        order.emplace_back(intern.resolve(cols[c].key), c);
    std::sort(order.begin(), order.end());
    for (const auto& [name, idx] : order) {
        out.names.push_back(std::string(prefix) + std::string(name));
        out.columns.push_back(
            typed_arg_column(cols[idx].vals, cols[idx].type, intern));
    }
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

// `field` of `src` as canonical JSON text; false when absent or null.
bool append_json_key(const PodSource& src, std::string& out,
                     std::string_view field) {
    if (const auto* sp = src.special(field)) {
        switch (*sp) {
            case FoldEvent::Special::NULL_VALUE:
                return false;
            case FoldEvent::Special::FALSE_VALUE:
                out += "false";
                return true;
            case FoldEvent::Special::TRUE_VALUE:
                out += "true";
                return true;
            case FoldEvent::Special::EMPTY_ARRAY:
                out += "[]";
                return true;
            case FoldEvent::Special::EMPTY_OBJECT:
                out += "{}";
                return true;
            case FoldEvent::Special::JSON_ARRAY:
            case FoldEvent::Special::JSON_OBJECT:
                return src.append_value(out, field);
        }
    }
    if (src.number_typed(field)) return src.append_value(out, field);
    if (!src.has(field)) return false;
    out += '"';
    dftracer::utils::json::append_json_escaped(out, src.value(field));
    out += '"';
    return true;
}

// A group-key string column (see AGG_KEY_ARG_PREFIX): null where the event
// lacks the value or holds JSON null, so a missing key never merges with "".
// A `json` key column holds canonical JSON text.
df::Series group_key_str_column(const std::vector<FoldEvent>& evs,
                                const dftracer::utils::StringIntern& intern,
                                std::string_view field, bool arg_only,
                                bool json) {
    std::string data;
    std::vector<std::size_t> ends;
    std::vector<bool> present;
    ends.reserve(evs.size());
    present.reserve(evs.size());
    for (const auto& ev : evs) {
        PodSource src(ev, intern);
        if (json) {
            const bool has = append_json_key(src, data, field);
            ends.push_back(data.size());
            present.push_back(has);
            continue;
        }
        bool has = arg_only ? src.append_arg(data, field)
                            : src.append_value(data, field);
        if (!has)
            if (const auto* sp = src.special(field)) {
                if (*sp == FoldEvent::Special::EMPTY_ARRAY) {
                    data += "[]";
                    has = true;
                } else if (*sp == FoldEvent::Special::EMPTY_OBJECT) {
                    data += "{}";
                    has = true;
                }
            }
        ends.push_back(data.size());
        present.push_back(has);
    }
    std::vector<std::string_view> vals;
    vals.reserve(evs.size());
    std::size_t begin = 0;
    for (std::size_t end : ends) {
        vals.emplace_back(data.data() + begin, end - begin);
        begin = end;
    }
    auto vbits = validity_of(present);
    df::Series out =
        df::Series::strings(std::span<const std::string_view>(vals),
                            vbits.empty() ? nullptr : vbits.data());
    if (json) return out.as_json();
    return out;
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
                if (!std::holds_alternative<std::uint32_t>(*v))
                    num = arg_double(*v);
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

// An agg-engine token, built the same way for every record_schema.
bool is_path_token(std::string_view sel) {
    std::string_view f;
    bool arg_only = false;
    return is_agg_key_field(sel, f, arg_only) || is_num_arg_field(sel, f) ||
           is_derived_agg_field(sel, f);
}

df::Series window_mask_column(const std::vector<FoldEvent>& evs,
                              const WindowSel& w) {
    const std::int64_t n = static_cast<std::int64_t>(evs.size());
    std::vector<std::int64_t> ones(evs.size(), 1);
    std::vector<bool> present;
    present.reserve(evs.size());
    for (const auto& ev : evs) present.push_back(ev.ts >= w.lo && ev.ts < w.hi);
    auto vbits = validity_of(present);
    return df::Series::flat_i64(ones.data(), n,
                                vbits.empty() ? nullptr : vbits.data());
}

df::Series clip_column(const std::vector<FoldEvent>& evs, const WindowSel& w) {
    const bool is_ts = w.rest == "ts";
    const std::int64_t n = static_cast<std::int64_t>(evs.size());
    std::vector<std::uint64_t> vals;
    std::vector<bool> present;
    vals.reserve(evs.size());
    present.reserve(evs.size());
    for (const auto& ev : evs) {
        const std::uint64_t s = std::max(ev.ts, w.lo);
        if (is_ts) {
            vals.push_back(s);
            present.push_back(true);
            continue;
        }
        const std::uint64_t e = std::min(ev.ts + ev.dur, w.hi);
        vals.push_back(ev.has_dur && e > s ? e - s : 0);
        present.push_back(ev.has_dur);
    }
    auto vbits = validity_of(present);
    return df::Series::flat(df::TypeId::Uint64, vals.data(), n,
                            vbits.empty() ? nullptr : vbits.data());
}

bool any_hash_present(const std::vector<FoldEvent>& evs,
                      std::uint32_t FoldEvent::* field) {
    for (const auto& ev : evs)
        if (ev.*field != dftracer::utils::StringIntern::NO_ID) return true;
    return false;
}

}  // namespace

std::string window_token(std::uint64_t lo, std::uint64_t hi,
                         std::string_view sel) {
    return window_sel(WINDOW_PREFIX, lo, hi, sel);
}

std::string clip_token(std::uint64_t lo, std::uint64_t hi,
                       std::string_view part) {
    return window_sel(CLIP_PREFIX, lo, hi, part);
}

std::string list_token(std::string_view spec, std::string_view path) {
    std::string out(LIST_PREFIX);
    out += spec;
    out += ':';
    out += path;
    return out;
}

std::string_view select_source_field(std::string_view sel) {
    sel = window_inner(sel);
    if (is_clip_token(sel)) return {};
    std::string_view spec;
    std::string_view path;
    if (parse_list_sel(sel, spec, path)) return path;
    std::string_view f;
    bool arg_only = false;
    if (is_agg_key_field(sel, f, arg_only) || is_num_arg_field(sel, f) ||
        is_derived_agg_field(sel, f))
        return f;
    return sel;
}

std::string_view window_inner(std::string_view sel) {
    WindowSel w;
    return parse_window_sel(sel, WINDOW_PREFIX, w) ? w.rest : sel;
}

bool is_clip_token(std::string_view sel) {
    return sel.substr(0, CLIP_PREFIX.size()) == CLIP_PREFIX;
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
    for (const std::string& token : select) {
        const std::string_view sel = window_inner(token);
        if (is_clip_token(sel)) continue;
        std::string_view f = sel;
        std::string_view list_spec;
        if (parse_list_sel(sel, list_spec, f)) {
            add(f);
            continue;
        }
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
        // io_cat and acc_pat are computed, not captured raw.
        if (is_iocat_field(f) || is_accpat_field(f)) continue;
        add(f);
    }
    return out;
}

std::string canonical_row_column_name(std::string_view sel, bool by_path) {
    if (sel.substr(0, WINDOW_PREFIX.size()) == WINDOW_PREFIX ||
        is_clip_token(sel))
        return std::string(sel);
    std::string_view spec;
    std::string_view path;
    if (parse_list_sel(sel, spec, path))
        return canonical_row_column_name(path, by_path);
    if (by_path && !is_path_token(sel)) return std::string(sel);
    std::string_view f;
    bool arg_only = false;
    std::string_view num_name;
    std::string_view deriv_name;
    if (is_agg_key_field(sel, f, arg_only)) return std::string(sel);
    if (is_num_arg_field(sel, num_name)) return std::string(sel);
    if (is_derived_agg_field(sel, deriv_name)) return std::string(deriv_name);
    if (is_top_level(sel)) return std::string(sel);
    if (is_iocat_field(sel)) return std::string(sel);
    if (is_accpat_field(sel)) return std::string(sel);
    const std::string_view key = strip_args_prefix(sel);
    if (is_hash_field(key)) return std::string(key);
    return std::string(dftracer::utils::ARGS_PREFIX) + std::string(key);
}

dataframe::TypeId row_column_type(std::string_view sel, bool by_path) {
    WindowSel w;
    if (parse_window_sel(sel, WINDOW_PREFIX, w))
        return w.rest.empty() ? df::TypeId::Int64
                              : row_column_type(w.rest, by_path);
    if (parse_window_sel(sel, CLIP_PREFIX, w)) return df::TypeId::Uint64;
    std::string_view spec;
    std::string_view path;
    if (parse_list_sel(sel, spec, path)) return df::TypeId::List;
    if (by_path && !is_path_token(sel)) return df::TypeId::Unknown;
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
    if (is_iocat_field(sel)) return df::TypeId::Int64;
    if (is_accpat_field(sel)) return df::TypeId::String;
    const std::string_view key = strip_args_prefix(sel);
    if (is_hash_field(key)) return df::TypeId::String;
    return df::TypeId::Unknown;  // a flattened arg: type is data-dependent
}

namespace {

df::Series select_column(const std::vector<FoldEvent>& evs,
                         const dftracer::utils::StringIntern& intern,
                         const std::string& sel, double time_scale,
                         bool by_path) {
    WindowSel win;
    if (parse_window_sel(sel, WINDOW_PREFIX, win)) {
        df::Series mask = window_mask_column(evs, win);
        if (win.rest.empty()) return mask;
        df::Series col = select_column(evs, intern, std::string(win.rest),
                                       time_scale, by_path);
        return col.where(mask.valid_mask(),
                         df::Series::nulls(col.type(), col.length()));
    }
    if (parse_window_sel(sel, CLIP_PREFIX, win)) return clip_column(evs, win);
    std::string_view list_spec;
    std::string_view list_path;
    if (parse_list_sel(sel, list_spec, list_path))
        return list_column(evs, intern, list_spec, list_path);
    if (by_path && !is_path_token(sel)) {
        auto& mut = const_cast<dftracer::utils::StringIntern&>(intern);
        const std::uint32_t id = mut.get_or_insert(flat_key(sel));
        return arg_column(evs, id, id, intern);
    }
    std::string_view agg_key_f;
    bool agg_key_arg_only = false;
    std::string_view num_arg_name;
    std::string_view deriv_name;
    if (is_agg_key_field(sel, agg_key_f, agg_key_arg_only)) {
        return group_key_str_column(evs, intern, agg_key_f, agg_key_arg_only,
                                    sel.starts_with(AGG_KEY_JSON_PREFIX));
    } else if (is_num_arg_field(sel, num_arg_name)) {
        return num_arg_column(evs, num_arg_name, intern);
    } else if (is_derived_agg_field(sel, deriv_name)) {
        return derived_agg_column(evs, deriv_name, intern);
    } else if (is_top_level(sel)) {
        return top_column(evs, sel, intern, time_scale);
    } else if (is_iocat_field(sel)) {
        return iocat_column(evs, intern);
    } else if (is_accpat_field(sel)) {
        return accpat_column(evs);
    } else if (const std::string_view key = strip_args_prefix(sel);
               is_hash_field(key)) {
        return hash_column(evs, key, intern);
    } else {
        // A nested/extra field (args.n.v) is captured under its full
        // name, a flat arg under its bare key; try the full name first,
        // then the stripped key, matching PodSource::find_arg.
        auto& mut = const_cast<dftracer::utils::StringIntern&>(intern);
        const std::uint32_t id_full = mut.get_or_insert(flat_key(sel));
        const std::uint32_t id_bare = mut.get_or_insert(flat_key(key));
        return arg_column(evs, id_full, id_bare, intern);
    }
}

}  // namespace

dataframe::DataFrame build_row_frame(
    const std::vector<FoldEvent>& evs,
    const dftracer::utils::StringIntern& intern,
    const std::vector<std::string>& select, double time_scale, bool by_path) {
    df::DataFrame out;
    const dftracer::utils::StringIntern* intern_ = &intern;
    const std::vector<std::string>& select_ = select;

    if (select_.empty() && by_path) {
        append_all_arg_columns(evs, *intern_, out, {});
    } else if (select_.empty()) {
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
        append_all_arg_columns(evs, *intern_, out,
                               dftracer::utils::ARGS_PREFIX);
    } else {
        for (const std::string& sel : select_) {
            out.names.push_back(canonical_row_column_name(sel, by_path));
            out.columns.push_back(
                select_column(evs, *intern_, sel, time_scale, by_path));
        }
    }

    return out;
}

std::vector<std::pair<std::string, dataframe::Series>>
build_dyn_numeric_columns(const std::vector<FoldEvent>& evs,
                          const dftracer::utils::StringIntern& intern,
                          const std::vector<std::string>& select) {
    namespace agg = dftracer::utils::index::schemas::dft::agg;
    // A windowed select holds only the events that start in the window.
    std::optional<WindowSel> win;
    for (const std::string& sel : select) {
        WindowSel w;
        if (parse_window_sel(sel, WINDOW_PREFIX, w) ||
            parse_window_sel(sel, CLIP_PREFIX, w)) {
            win = w;
            break;
        }
    }
    std::optional<df::Series> keep;
    if (win) keep = window_mask_column(evs, *win).valid_mask();
    // Discover the numeric-arg names present in this batch, matching
    // fold_numeric_args_t: the io-cat-derived "size" plus every non-reserved,
    // non-preagg numeric arg. Sorted (std::set) is not required (the agg
    // finalize sorts the name union), but keeps a deterministic layout.
    std::set<std::string> names;
    for (const FoldEvent& ev : evs) {
        if (win && !(ev.ts >= win->lo && ev.ts < win->hi)) continue;
        PodSource src(ev, intern);
        if (derived_size_t(src)) names.insert("size");
        src.for_each_numeric_arg([&](std::string_view key, double) {
            if (agg::is_reserved_arg(key) || agg::is_preagg_suffix(key)) return;
            names.insert(std::string(key));
        });
    }
    std::vector<std::pair<std::string, dataframe::Series>> out;
    out.reserve(names.size());
    for (const std::string& name : names) {
        df::Series col = num_arg_column(evs, name, intern);
        if (keep)
            col = col.where(*keep, df::Series::nulls(col.type(), col.length()));
        out.emplace_back(std::string(AGG_NUM_ARG_PREFIX) + name,
                         std::move(col));
    }
    return out;
}

dataframe::DataFrame NativeRowFold::build() {
    dataframe::DataFrame out =
        build_row_frame(events_, *intern_, select_, time_scale_, by_path_);
    events_.clear();
    return out;
}

}  // namespace dftracer::utils::trace::views::detail
