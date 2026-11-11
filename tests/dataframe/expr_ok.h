// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TESTS_DATAFRAME_EXPR_OK_H
#define DFTRACER_UTILS_TESTS_DATAFRAME_EXPR_OK_H

// The value of an expression compile or evaluation that a test expects to
// succeed, and a check for the InvalidArgument error that a malformed
// expression gives.

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dstd/result.h>
#include <task_result.h>

#include <utility>

namespace dftu_utils_test {

inline dftracer::utils::dataframe::Series eval_ok(
    const dftracer::utils::dataframe::Expr& root,
    const dftracer::utils::dstd::Vector<
        const dftracer::utils::dataframe::Series*>& inputs) {
    return ok(dftracer::utils::dataframe::eval(root, inputs));
}

inline dftracer::utils::dstd::Vector<dftracer::utils::dataframe::Series>
eval_many_ok(const dftracer::utils::dstd::Vector<
                 dftracer::utils::dataframe::Expr>& roots,
             const dftracer::utils::dstd::Vector<
                 const dftracer::utils::dataframe::Series*>& inputs) {
    return ok(dftracer::utils::dataframe::eval_many(roots, inputs));
}

inline dftracer::utils::dataframe::DataType infer_type_ok(
    const dftracer::utils::dataframe::Expr& root,
    const dftracer::utils::dstd::Vector<dftracer::utils::dataframe::DataType>&
        input_types) {
    return ok(dftracer::utils::dataframe::infer_type(root, input_types));
}

inline dftracer::utils::dataframe::Expr built(
    dftracer::utils::dstd::Result<dftracer::utils::dataframe::Expr> expr) {
    return ok(std::move(expr));
}

template <typename T>
bool is_invalid(const dftracer::utils::dstd::Result<T>& result) {
    return !result.has_value() &&
           dftracer::utils::common::errorcode_of(result.error()) ==
               dftracer::utils::common::ErrorCode::InvalidArgument;
}

}  // namespace dftu_utils_test

#endif  // DFTRACER_UTILS_TESTS_DATAFRAME_EXPR_OK_H
