#ifndef DFTRACER_UTILS_DUQL_WILDCARD_H
#define DFTRACER_UTILS_DUQL_WILDCARD_H

#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/parser.h>
#include <dftracer/utils/duql/syntax/tree.h>

namespace dftracer::utils::duql {

/// The scalar leaf paths a query may read, dotted, as the source writes them
/// (`args.counters.a.p50`). Called at most once per expansion, and not at all
/// when the query has no wildcard.
using LeafPaths = dstd::Function<dstd::Vector<dstd::String>()>;

/// Replaces every wildcard path of `p`, nested pipelines included, with the
/// leaf paths it matches: a `select`, `drop` or `unpivot` item by one item per
/// match, and `any(p) op v` / `all(p) op v` by the OR / AND of `m op v` over
/// the matches. Matches are ordered segment by segment, numeric segments by
/// value and before the others, which go by bytes. With `args_fallback`, a
/// pattern that does not start with `args` also matches the leaves under
/// `args`, written without it. A pattern with no match is an error naming it.
dftracer::utils::dstd::Expected<void, DuqlError> expand_wildcards(
    syntax::Pipeline& p, const LeafPaths& leaves, bool args_fallback,
    dstd::StringView text);

}  // namespace dftracer::utils::duql

#endif  // DFTRACER_UTILS_DUQL_WILDCARD_H
