// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DUQL_SYNTAX_PARSER_H
#define DFTRACER_UTILS_DUQL_SYNTAX_PARSER_H

#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>

namespace dftracer::utils::duql::syntax {

/// Parses duql text into a syntax tree. Fails with the position of the first
/// syntax error.
dftracer::utils::dstd::Expected<Program, duql::DuqlError> parse(
    dstd::StringView source);

/// Whether `word` names a stage (`where`, `sort`, ...), ignoring case.
bool is_stage_word(dstd::StringView word);

}  // namespace dftracer::utils::duql::syntax

#endif  // DFTRACER_UTILS_DUQL_SYNTAX_PARSER_H
