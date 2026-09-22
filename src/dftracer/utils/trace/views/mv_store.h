#ifndef DFTRACER_UTILS_TRACE_VIEWS_MV_STORE_H
#define DFTRACER_UTILS_TRACE_VIEWS_MV_STORE_H

#include <dftracer/utils/trace/views/view.h>

#include <optional>
#include <string>
#include <vector>

namespace dftracer::utils::trace::views::detail {

struct ViewPlan;

/// Root directory holding materialized filtered-trace views: a sibling of the
/// aggregation anchor, `<parent-of-anchor>/.dftindex-views`, overridable via
/// plan.views_root. Empty when the plan has no single anchor.
std::string views_root(const ViewPlan& plan);

/// Human-readable, collision-safe directory name for this row query's MV:
/// a sanitized rendering of the predicate/phase/window plus an 8-hex identity
/// suffix. The suffix, not the readable part, guarantees uniqueness.
std::string view_slug(const ViewPlan& plan);

/// True if this exact plan is already materialized (its slug directory holds a
/// manifest and a trace). The slug encodes the base files' size/mtime, so a
/// changed base yields a different slug and this returns false.
bool view_is_fresh(const ViewPlan& plan);

/// The directory `<views_root>/<slug>` a materialize() writes into, created on
/// demand. Empty if there is no views_root.
std::string materialize_view_dir(const ViewPlan& plan);

/// Single-flight guard for one MV directory: an advisory, non-blocking
/// exclusive lock on `<dir>/.lock`. Returns an fd to pass to unlock_view_dir on
/// success, or -1 if another builder holds it (the caller should skip and let
/// the winner finish). The lock is released on unlock and, on a crash, by the
/// OS.
int lock_view_dir(const std::string& dir);
void unlock_view_dir(int fd);

/// Record the manifest (predicate, phase, window, base-file identities) for an
/// MV materialized at `dir`, so a later query can match and reuse it.
void register_view(const std::string& dir, const ViewPlan& plan);

/// Find a materialized view that subsumes `plan` - same base files (unchanged),
/// a compatible phase, a window covering the query's, a predicate the query
/// implies, and a smaller on-disk footprint than the base - and return the
/// trace file(s) to redirect the scan onto (with their shared index). nullopt
/// when none qualifies, so the caller scans the base.
std::optional<std::vector<ViewFile>> find_subsuming_view(const ViewPlan& plan);

}  // namespace dftracer::utils::trace::views::detail

#endif  // DFTRACER_UTILS_TRACE_VIEWS_MV_STORE_H
