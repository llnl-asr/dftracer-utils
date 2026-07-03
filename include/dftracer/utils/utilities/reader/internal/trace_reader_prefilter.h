#ifndef DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_PREFILTER_H
#define DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_PREFILTER_H

#include <dftracer/utils/utilities/common/query/query.h>
#include <simdjson.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::reader::internal {

// Cheap byte-level pre-filter derived from a query AST.
//
// The filter holds a list of literal substrings that MUST appear (verbatim) in
// any line matching the query. Currently populated only for ASTs of the form
// "AND of field == literal"; the common shape of dftindex equality queries.
// For unsupported shapes (range ops, OR, NOT, IN/NOT IN, non-equality compares)
// `required` is left empty and `may_match` trivially returns true.
//
// Semantically false-positive-safe: any line we accept still gets re-checked
// against the real query downstream. Lines we reject are guaranteed not to
// match because the literal representation of the comparison is missing.
struct LinePrefilter {
    std::vector<std::string> required;

    bool empty() const { return required.empty(); }

    bool may_match(std::string_view bytes) const {
        for (const auto& lit : required) {
            if (::memmem(bytes.data(), bytes.size(), lit.data(), lit.size()) ==
                nullptr)
                return false;
        }
        return true;
    }
};

// AND-of-EQ predicates with concrete typed literals can be evaluated
// directly against simdjson without going through ValueMap (which costs
// wyhash + per-field std::string allocation per row). Anything more
// complex (OR/NOT/IN/range) falls back to the generic visitor.
struct CompiledEqProbe {
    std::string top_key;     // "pid", "args", "name", etc.
    std::string nested_key;  // "" for top-level, else e.g. "fhash"
    enum class Kind { String, Int64, UInt64, Double, Bool };
    Kind kind = Kind::String;
    std::string s_val;
    std::int64_t i64_val = 0;
    std::uint64_t u64_val = 0;
    double d_val = 0.0;
    bool b_val = false;
};

LinePrefilter build_prefilter(const common::query::Query& q);

std::optional<std::vector<CompiledEqProbe>> try_compile_eq_probes(
    const common::query::QueryNode& node);

bool eval_compiled_eq(const std::vector<CompiledEqProbe>& probes,
                      simdjson::ondemand::document_reference doc);

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_UTILITIES_READER_INTERNAL_TRACE_READER_PREFILTER_H
