#ifndef DFTRACER_UTILS_DUQL_SYNTAX_PARSER_H
#define DFTRACER_UTILS_DUQL_SYNTAX_PARSER_H

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>

#include <string_view>

namespace dftracer::utils::duql::syntax {

/// Parses duql text into a syntax tree. Fails with the position of the first
/// syntax error.
dftracer::utils::expected<Program, duql::DuqlError> parse(
    std::string_view source);

/// Whether `word` names a stage (`where`, `sort`, ...), ignoring case.
bool is_stage_word(std::string_view word);

}  // namespace dftracer::utils::duql::syntax

#endif  // DFTRACER_UTILS_DUQL_SYNTAX_PARSER_H
