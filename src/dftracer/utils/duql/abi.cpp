#include <dftracer/utils/duql/abi.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/internal/duql_handle.h>
#include <dftracer/utils/duql/query.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

struct dftu_duql {
    dftracer::utils::duql::Query q;
};

namespace dftracer::utils::duql {
const Query& duql_handle_unwrap(const dftu_duql* h) { return h->q; }
}  // namespace dftracer::utils::duql

namespace {

using dftracer::utils::duql::CompareOp;
using dftracer::utils::duql::Expr;
using dftracer::utils::duql::MatchOp;
using dftracer::utils::duql::Query;

dftu_duql* wrap_expr(const Expr& e) {
    auto q = e.build();
    if (!q) return nullptr;
    return new dftu_duql{std::move(*q)};
}

dftu_duql* wrap_string(const std::string& s) {
    auto q = Query::from_string(s);
    if (!q) return nullptr;
    return new dftu_duql{std::move(*q)};
}

dftu_duql* build_in_i64(const char* field, const int64_t* values, int32_t n,
                        bool negate) {
    if (!field || (n > 0 && !values)) return nullptr;
    std::vector<std::int64_t> vals;
    for (int32_t i = 0; i < n; ++i) vals.push_back(values[i]);
    auto e = negate ? dftracer::utils::duql::field_not_in(field, vals)
                    : dftracer::utils::duql::field_in(field, vals);
    return wrap_expr(e);
}

dftu_duql* build_in_str(const char* field, const char* const* values, int32_t n,
                        bool negate) {
    if (!field || (n > 0 && !values)) return nullptr;
    std::vector<std::string> vals;
    for (int32_t i = 0; i < n; ++i) {
        if (!values[i]) return nullptr;
        vals.emplace_back(values[i]);
    }
    auto e = negate ? dftracer::utils::duql::field_not_in(field, vals)
                    : dftracer::utils::duql::field_in(field, vals);
    return wrap_expr(e);
}

}  // namespace

extern "C" {

dftu_duql* dftu_duql_parse(const char* text) {
    if (!text) return nullptr;
    auto r = dftracer::utils::duql::Query::from_string(text);
    if (!r) return nullptr;
    return new dftu_duql{std::move(*r)};
}

void dftu_duql_free(dftu_duql* q) { delete q; }

char* dftu_duql_to_string(const dftu_duql* q) {
    if (!q) return nullptr;
    std::string s = q->q.to_string();
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

void dftu_duql_string_free(char* s) { std::free(s); }

dftu_duql* dftu_duql_cmp_i64(const char* field, dftu_duql_cmp_op op,
                             int64_t value) {
    if (!field) return nullptr;
    return wrap_expr(dftracer::utils::duql::field_cmp(
        field, static_cast<CompareOp>(op),
        dftracer::utils::duql::detail::literal(
            static_cast<std::int64_t>(value))));
}

dftu_duql* dftu_duql_cmp_f64(const char* field, dftu_duql_cmp_op op,
                             double value) {
    if (!field) return nullptr;
    return wrap_expr(dftracer::utils::duql::field_cmp(
        field, static_cast<CompareOp>(op),
        dftracer::utils::duql::detail::literal(value)));
}

dftu_duql* dftu_duql_cmp_str(const char* field, dftu_duql_cmp_op op,
                             const char* value) {
    if (!field || !value) return nullptr;
    return wrap_expr(dftracer::utils::duql::field_cmp(
        field, static_cast<CompareOp>(op),
        dftracer::utils::duql::detail::literal(std::string_view(value))));
}

dftu_duql* dftu_duql_in_i64(const char* field, const int64_t* values,
                            int32_t n) {
    return build_in_i64(field, values, n, /*negate=*/false);
}

dftu_duql* dftu_duql_in_str(const char* field, const char* const* values,
                            int32_t n) {
    return build_in_str(field, values, n, /*negate=*/false);
}

dftu_duql* dftu_duql_not_in_i64(const char* field, const int64_t* values,
                                int32_t n) {
    return build_in_i64(field, values, n, /*negate=*/true);
}

dftu_duql* dftu_duql_not_in_str(const char* field, const char* const* values,
                                int32_t n) {
    return build_in_str(field, values, n, /*negate=*/true);
}

dftu_duql* dftu_duql_match(const char* field, dftu_duql_match_op match_op,
                           const char* pattern) {
    if (!field || !pattern) return nullptr;
    return wrap_expr(dftracer::utils::duql::field_match(
        field, static_cast<MatchOp>(match_op), pattern));
}

dftu_duql* dftu_duql_and(dftu_duql* a, dftu_duql* b) {
    if (!a || !b) {
        dftu_duql_free(a);
        dftu_duql_free(b);
        return nullptr;
    }
    std::string s = "(" + a->q.to_string() + " and " + b->q.to_string() + ")";
    dftu_duql_free(a);
    dftu_duql_free(b);
    return wrap_string(s);
}

dftu_duql* dftu_duql_or(dftu_duql* a, dftu_duql* b) {
    if (!a || !b) {
        dftu_duql_free(a);
        dftu_duql_free(b);
        return nullptr;
    }
    std::string s = "(" + a->q.to_string() + " or " + b->q.to_string() + ")";
    dftu_duql_free(a);
    dftu_duql_free(b);
    return wrap_string(s);
}

dftu_duql* dftu_duql_not(dftu_duql* a) {
    if (!a) return nullptr;
    std::string s = "not (" + a->q.to_string() + ")";
    dftu_duql_free(a);
    return wrap_string(s);
}
}
