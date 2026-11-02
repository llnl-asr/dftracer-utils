#ifndef DFTRACER_UTILS_DUQL_PARSER_H
#define DFTRACER_UTILS_DUQL_PARSER_H

#include <dftracer/utils/common/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/transparent_string_hash.h>
#include <dftracer/utils/duql/ast.h>

#include <cstddef>
#include <variant>

namespace dftracer::utils::duql {

/// Structured parse error with source location.
struct DuqlError {
    dstd::String message;    ///< Error description.
    std::size_t column = 0;  ///< Column in the line (0-indexed).
    dstd::String source;     ///< The source line holding the error.
    dstd::String indicator;  ///< Caret/tilde indicator line.
    std::size_t line = 1;    ///< Line of the error (1-indexed).

    /// Format as multi-line error message with source and indicator.
    dstd::String format() const;
};

/// Exception wrapping a DuqlError.
class DuqlParseError : public common::DFTUtilsException {
   public:
    explicit DuqlParseError(DuqlError err);
    const DuqlError& error() const { return err_; }

   private:
    DuqlError err_;
};

/// Values bound to `$name` parameters, by name without the `$`.
using Params = dstd::StringViewMap<ParamValue>;

/// Parse a duql filter into an AST. Throws nothing; a syntax error, or a
/// construct this engine cannot evaluate yet, is the error value.
dftracer::utils::dstd::Expected<QueryNodePtr, DuqlError> parse(
    dstd::StringView input);

/// parse with `$name` parameters bound to `params`; an unbound parameter is
/// an error.
dftracer::utils::dstd::Expected<QueryNodePtr, DuqlError> parse(
    dstd::StringView input, const Params& params);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_PARSER_H
