#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/utilities/reader/internal/trace_reader_prefilter.h>
#include <simdjson.h>

#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

using duql::Query;

namespace {

// Walk a CompareNode-with-EQ leaf into a probe. A bare key resolves as the
// evaluator does, top level first and then under "args"; "args.<key>" reads
// args only. Other dotted paths may name a nested object or a flat dotted
// key, so they are left to the full evaluator (returns false).
bool compile_eq_leaf(const duql::CompareNode& n, CompiledEqProbe& out) {
    if (n.op != duql::CompareOp::EQ) return false;
    const std::string& path = n.field.path;
    const auto dot = path.find('.');
    if (dot == std::string::npos) {
        out.top_key = path;
        out.nested_key.clear();
        out.args_fallback = true;
    } else if (path.compare(0, dot, "args") == 0 &&
               path.find('.', dot + 1) == std::string::npos) {
        out.top_key = "args";
        out.nested_key = path.substr(dot + 1);
        out.args_fallback = false;
    } else {
        return false;
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
    const duql::QueryNode& node) {
    using namespace duql;
    return std::visit(
        [&](const auto& n) -> std::optional<std::vector<CompiledEqProbe>> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, CompareNode>) {
                CompiledEqProbe p;
                if (n.field.any || !compile_eq_leaf(n, p)) return std::nullopt;
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
    // The value of `key` in `obj`: nullopt when absent, an empty value when
    // it is null.
    using Found = std::optional<std::optional<simdjson::ondemand::value>>;
    auto field = [](auto&& obj, const std::string& key) -> Found {
        auto r = obj.find_field_unordered(std::string_view(key));
        if (r.error()) return std::nullopt;
        simdjson::ondemand::value v = r.value_unsafe();
        bool null = false;
        if (v.is_null().get(null) != simdjson::SUCCESS || null)
            return Found{std::in_place};
        return Found{v};
    };
    for (const auto& p : probes) {
        doc.rewind();
        if (p.nested_key.empty()) {
            if (auto v = field(doc, p.top_key)) {
                if (!*v || !probe_matches_value(p, **v)) return false;
                continue;
            }
            if (!p.args_fallback) return false;
            doc.rewind();
        }
        auto args = field(doc, "args");
        if (!args || !*args) return false;
        auto obj = (*args)->get_object();
        if (obj.error()) return false;
        auto v = field(obj.value_unsafe(),
                       p.nested_key.empty() ? p.top_key : p.nested_key);
        if (!v || !*v || !probe_matches_value(p, **v)) return false;
    }
    return true;
}

}  // namespace dftracer::utils::utilities::reader::internal
