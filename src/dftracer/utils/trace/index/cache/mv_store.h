// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_CACHE_MV_STORE_H
#define DFTRACER_UTILS_TRACE_INDEX_CACHE_MV_STORE_H

#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/views/view.h>

#include <vector>

namespace dftracer::utils::trace::index::cache {

struct ViewPlan;

/// Root directory holding materialized filtered-trace views: `<index
/// root>/views`, overridable via plan.views_root. Empty when the plan has no
/// single index root.
dstd::String views_root(const trace::views::detail::ViewPlan& plan);

/// Human-readable, collision-safe directory name for this row query's MV:
/// a sanitized rendering of the predicate/phase/window plus an 8-hex identity
/// suffix. The suffix, not the readable part, guarantees uniqueness.
dstd::String view_slug(const trace::views::detail::ViewPlan& plan);

/// True if this exact plan is already materialized (its view root holds a
/// manifest and a part). The slug encodes the base files' size/mtime, so a
/// changed base yields a different slug and this returns false.
bool view_is_fresh(const trace::views::detail::ViewPlan& plan);

/// The view root `<views_root>/<slug>` a materialize() writes into, created on
/// demand with its parts directory. Empty if there is no views_root.
dstd::String materialize_view_dir(const trace::views::detail::ViewPlan& plan);

/// Single-flight guard for one view root: an advisory, non-blocking exclusive
/// lock on `<view root>/.lock`. Returns the open lock file to pass to
/// unlock_view_dir on success, or a closed File if another builder holds it
/// (the caller should skip and let the winner finish). The lock is released on
/// unlock and, on a crash, by the OS.
dstd::File lock_view_dir(dstd::StringView dir);
void unlock_view_dir(dstd::File& lock);

/// Record the manifest (predicate, phase, window, base-file identities) for an
/// MV materialized at `dir`, so a later query can match and reuse it.
void register_view(dstd::StringView dir,
                   const trace::views::detail::ViewPlan& plan);

/// Find a materialized view that subsumes `plan` - same base files (unchanged),
/// a compatible phase, a window covering the query's, a predicate the query
/// implies, and a smaller on-disk footprint than the base - and return the
/// trace file(s) to redirect the scan onto (with their shared index). nullopt
/// when none qualifies, so the caller scans the base.
dstd::Optional<dstd::Vector<trace::views::ViewFile>> find_subsuming_view(
    const trace::views::detail::ViewPlan& plan);

}  // namespace dftracer::utils::trace::index::cache

#endif  // DFTRACER_UTILS_TRACE_INDEX_CACHE_MV_STORE_H
