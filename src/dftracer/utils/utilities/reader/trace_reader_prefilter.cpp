#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/reader/internal/trace_reader_prefilter.h>
#include <simdjson.h>

#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

using common::query::Query;

namespace {

bool collect_and_eq_literals(const common::query::QueryNode& node,
                             std::vector<std::string>& out) {
    return std::visit(
        [&out](const auto& n) -> bool {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, common::query::CompareNode>) {
                if (n.op != common::query::CompareOp::EQ) return false;
                std::string lit;
                lit.reserve(n.field.path.size() + 16);
                lit += '"';
                lit += n.field.path;
                lit += "\":";
                const auto& val = n.value.value;
                if (std::holds_alternative<std::string>(val)) {
                    lit += '"';
                    lit += std::get<std::string>(val);
                    lit += '"';
                } else if (std::holds_alternative<int64_t>(val)) {
                    lit += std::to_string(std::get<int64_t>(val));
                } else if (std::holds_alternative<uint64_t>(val)) {
                    lit += std::to_string(std::get<uint64_t>(val));
                } else if (std::holds_alternative<bool>(val)) {
                    lit += std::get<bool>(val) ? "true" : "false";
                } else {
                    return false;  // double or other: skip pre-filter
                }
                out.push_back(std::move(lit));
                return true;
            } else if constexpr (std::is_same_v<T, common::query::AndNode>) {
                return collect_and_eq_literals(*n.left, out) &&
                       collect_and_eq_literals(*n.right, out);
            }
            return false;  // OrNode, NotNode, InNode, NotInNode, CompareNode
                           // with non-EQ op: conservative skip
        },
        node.data);
}

// Top-level JSON keys in dftracer events. Anything else in the query DSL
// (e.g. `epoch == 0`, `fhash == "..."`) refers to a field nested under
// "args"; the same convention collect_query_fields relies on when it
// folds nested object keys into the flat ValueMap.
bool is_top_level_event_key(std::string_view k) {
    return k == "id" || k == "name" || k == "cat" || k == "pid" || k == "tid" ||
           k == "ts" || k == "dur" || k == "ph";
}

// Walk a CompareNode-with-EQ leaf into a probe. Returns false on
// unsupported shapes (more than one '.' or a literal type the simdjson
// get_X path can't compare directly).
bool compile_eq_leaf(const common::query::CompareNode& n,
                     CompiledEqProbe& out) {
    if (n.op != common::query::CompareOp::EQ) return false;
    auto dot = n.field.path.find('.');
    if (dot == std::string::npos) {
        if (is_top_level_event_key(n.field.path)) {
            out.top_key = n.field.path;
            out.nested_key.clear();
        } else {
            // Bare arg-style key: foo -> args.foo.
            out.top_key = "args";
            out.nested_key = n.field.path;
        }
    } else {
        if (n.field.path.find('.', dot + 1) != std::string::npos) return false;
        out.top_key = n.field.path.substr(0, dot);
        out.nested_key = n.field.path.substr(dot + 1);
    }
    return std::visit(
        [&out](auto&& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                out.kind = CompiledEqProbe::Kind::String;
                out.s_val = v;
                return true;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out.kind = CompiledEqProbe::Kind::Int64;
                out.i64_val = v;
                return true;
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                out.kind = CompiledEqProbe::Kind::UInt64;
                out.u64_val = v;
                return true;
            } else if constexpr (std::is_same_v<T, double>) {
                out.kind = CompiledEqProbe::Kind::Double;
                out.d_val = v;
                return true;
            } else if constexpr (std::is_same_v<T, bool>) {
                out.kind = CompiledEqProbe::Kind::Bool;
                out.b_val = v;
                return true;
            } else {
                return false;
            }
        },
        n.value.value);
}

bool probe_matches_value(const CompiledEqProbe& p,
                         simdjson::ondemand::value val) {
    switch (p.kind) {
        case CompiledEqProbe::Kind::String: {
            auto r = val.get_string();
            if (r.error()) return false;
            auto sv = r.value_unsafe();
            return sv.size() == p.s_val.size() &&
                   std::memcmp(sv.data(), p.s_val.data(), sv.size()) == 0;
        }
        case CompiledEqProbe::Kind::Int64: {
            auto t = val.type();
            if (t.error()) return false;
            if (t.value_unsafe() == simdjson::ondemand::json_type::number) {
                auto num = val.get_number();
                if (num.error()) return false;
                auto n = num.value_unsafe();
                if (n.is_int64()) return n.get_int64() == p.i64_val;
                if (n.is_uint64()) {
                    if (p.i64_val < 0) return false;
                    return n.get_uint64() ==
                           static_cast<std::uint64_t>(p.i64_val);
                }
                return n.get_double() == static_cast<double>(p.i64_val);
            }
            return false;
        }
        case CompiledEqProbe::Kind::UInt64: {
            auto num = val.get_number();
            if (num.error()) return false;
            auto n = num.value_unsafe();
            if (n.is_uint64()) return n.get_uint64() == p.u64_val;
            if (n.is_int64()) {
                auto v = n.get_int64();
                if (v < 0) return false;
                return static_cast<std::uint64_t>(v) == p.u64_val;
            }
            return n.get_double() == static_cast<double>(p.u64_val);
        }
        case CompiledEqProbe::Kind::Double: {
            auto r = val.get_double();
            if (r.error()) return false;
            return r.value_unsafe() == p.d_val;
        }
        case CompiledEqProbe::Kind::Bool: {
            auto r = val.get_bool();
            if (r.error()) return false;
            return r.value_unsafe() == p.b_val;
        }
    }
    return false;
}

}  // namespace

// Try to compile the query AST as an AND of EQ leaves. nullopt on
// unsupported shapes; the ValueMap path handles those.
std::optional<std::vector<CompiledEqProbe>> try_compile_eq_probes(
    const common::query::QueryNode& node) {
    using namespace common::query;
    return std::visit(
        [&](const auto& n) -> std::optional<std::vector<CompiledEqProbe>> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                CompiledEqProbe p;
                if (!compile_eq_leaf(n, p)) return std::nullopt;
                return std::vector<CompiledEqProbe>{std::move(p)};
            } else if constexpr (std::is_same_v<T, AndNode>) {
                auto l = try_compile_eq_probes(*n.left);
                if (!l) return std::nullopt;
                auto r = try_compile_eq_probes(*n.right);
                if (!r) return std::nullopt;
                l->insert(l->end(), std::make_move_iterator(r->begin()),
                          std::make_move_iterator(r->end()));
                return l;
            } else {
                return std::nullopt;
            }
        },
        node.data);
}

// Evaluate compiled AND-of-EQ probes by directly probing simdjson fields.
bool eval_compiled_eq(const std::vector<CompiledEqProbe>& probes,
                      simdjson::ondemand::document_reference doc) {
    for (const auto& p : probes) {
        doc.rewind();
        auto top_r = doc.find_field_unordered(
            std::string_view(p.top_key.data(), p.top_key.size()));
        if (top_r.error()) return false;
        auto top_v = top_r.value();
        if (p.nested_key.empty()) {
            if (!probe_matches_value(p, top_v)) return false;
        } else {
            auto obj_r = top_v.get_object();
            if (obj_r.error()) return false;
            auto inner_r = obj_r.value().find_field_unordered(
                std::string_view(p.nested_key.data(), p.nested_key.size()));
            if (inner_r.error()) return false;
            if (!probe_matches_value(p, inner_r.value())) return false;
        }
    }
    return true;
}

LinePrefilter build_prefilter(const Query& q) {
    // Short literals like `"pid":1000` or `"epoch":0` are common enough in
    // practice that memmem on every line costs more than it saves on the
    // parse side. Only keep literals long enough that rarity is plausible
    // (hashes, filenames, host names).
    constexpr std::size_t MIN_LITERAL_LEN = 16;

    LinePrefilter pf;
    std::vector<std::string> tmp;
    if (collect_and_eq_literals(q.root(), tmp)) {
        for (auto& lit : tmp) {
            if (lit.size() >= MIN_LITERAL_LEN) {
                pf.required.push_back(std::move(lit));
            }
        }
    }
    return pf;
}

}  // namespace dftracer::utils::utilities::reader::internal
