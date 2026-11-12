// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_VIEWS_JOIN_TYPE_H
#define DFTRACER_UTILS_TRACE_VIEWS_JOIN_TYPE_H

namespace dftracer::utils::trace::views {

/// How a join keeps rows: the matching rows (Inner), plus the unmatched rows
/// of one side (Left, Right) or of both (Full), only the left rows that have a
/// match (LeftSemi) or only those with none (LeftAnti).
enum class JoinType { Inner, Left, Right, Full, LeftSemi, LeftAnti };

}  // namespace dftracer::utils::trace::views

#endif  // DFTRACER_UTILS_TRACE_VIEWS_JOIN_TYPE_H
