// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DATAFRAME_INTERNAL_EXPR_WINDOWS_H
#define DFTRACER_UTILS_DATAFRAME_INTERNAL_EXPR_WINDOWS_H

#include <dftracer/utils/dataframe/expr.h>
#include <dftracer/utils/dataframe/series.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstdint>

namespace dftracer::utils::dataframe {

/// Expressions compiled once, as @ref eval_many compiles them, and evaluated a
/// row window at a time, so a caller that consumes the rows in windows never
/// holds a full-length result. The inputs are shared, not copied, and must
/// keep their rows while this lives. @ref eval may run on many threads at
/// once. A UDF in the program sees the cancel of the thread that compiled it.
class ExprWindows {
   public:
    static dstd::Result<ExprWindows> compile(
        dstd::Span<const Expr> roots, dstd::Span<const Series* const> inputs);
    ExprWindows(ExprWindows&&) noexcept;
    ExprWindows& operator=(ExprWindows&&) noexcept;
    ~ExprWindows();

    /// One column per root, aligned to the roots, holding rows
    /// [offset, offset + len) of the inputs as rows [0, len).
    dstd::Result<dstd::Vector<Series>> eval(std::int64_t offset,
                                            std::int64_t len) const;

   private:
    struct Impl;
    explicit ExprWindows(dstd::UniquePtr<Impl> impl) noexcept;
    dstd::UniquePtr<Impl> impl_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_INTERNAL_EXPR_WINDOWS_H
