#ifndef DFTRACER_UTILS_DUQL_LOWER_H
#define DFTRACER_UTILS_DUQL_LOWER_H

#include <dftracer/utils/core/common/expected.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>

#include <string_view>

namespace dftracer::utils::duql {

/// The query tree today's engine evaluates for `program`, which must be a
/// filter: one `where` (written or implicit) and nothing else. Fails, naming
/// the construct and the stage that adds it, for anything outside that
/// subset, and for a parameter `params` does not bind. `source` is the text
/// `program` was parsed from, for error positions. Its `def`s and those on
/// `$DFTRACER_DUQL_PATH` expand first.
dftracer::utils::expected<duql::QueryNodePtr, duql::DuqlError> lower_filter(
    syntax::Program program, const duql::Params& params,
    std::string_view source);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_LOWER_H
