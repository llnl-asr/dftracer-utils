#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_EXPR_HANDLE_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_EXPR_HANDLE_H

#include <dftracer/utils/dataframe/expr.h>

#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils::dataframe {

// Access the Expr wrapped by a dftu_expr handle. Internal bridge so a C ABI in
// another translation unit (agg_expr.cpp) can compose the expr C ABI without
// re-declaring the opaque handle struct.
const Expr& expr_handle_unwrap(const dftu_expr* h);

// Wrap `e` as a new owned dftu_expr handle. The counterpart to
// expr_handle_unwrap, for a translation unit (provider_registry.cpp) that must
// hand a C++ Expr to a C source_vt as a dftu_expr* without re-declaring the
// opaque handle struct. Free with dftu_expr_free.
dftu_expr* expr_handle_wrap(Expr e);

// Structural hash of the expression tree, for plan fingerprints. An is_in set
// hashes by buffer identity.
std::uint64_t expr_fingerprint(const Expr& e);

// `e` with each col(i) replaced by `by_index[i]`; an out-of-range or empty
// entry keeps the column reference.
Expr expr_rebind_cols(const Expr& e, const std::vector<Expr>& by_index);

// A stable text for the expression tree, equal for equal trees in any process
// (literals and is_in values written out, no addresses): an identity that may
// be persisted.
std::string expr_canonical(const Expr& e);

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_EXPR_HANDLE_H
