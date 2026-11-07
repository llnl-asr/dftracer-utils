// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_PARSER_H
#define DFTRACER_UTILS_DUQL_PARSER_H

#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/detail/ast.h>
#include <dftracer/utils/duql/error.h>
#include <dftracer/utils/duql/types.h>

namespace dftracer::utils::duql {

/// Parse a duql filter into an AST. Throws nothing; a syntax error, or a
/// construct this engine cannot evaluate yet, is the error value.
dftracer::utils::dstd::Expected<detail::QueryNodePtr, DuqlError> parse(
    dstd::StringView input);

/// parse with `$name` parameters bound to `params`; an unbound parameter is
/// an error.
dftracer::utils::dstd::Expected<detail::QueryNodePtr, DuqlError> parse(
    dstd::StringView input, const Params& params);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_PARSER_H
