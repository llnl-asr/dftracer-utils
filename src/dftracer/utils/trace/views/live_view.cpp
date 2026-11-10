// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/build/new_members.h>
#include <dftracer/utils/index/build/resolve_and_build.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/plan/prune.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/live_view.h>
#include <dftracer/utils/trace/views/view_plan.h>
#include <dftracer/utils/trace/views/view_plan_ops.h>

#include <algorithm>
#include <utility>

namespace dftracer::utils::trace::views {

namespace {

dstd::Result<dstd::Vector<dstd::String>> trace_files_under(
    const dstd::String& dir) {
    runtime::fs::FindOptions options;
    options.patterns =
        runtime::fs::names_of(common::constants::indexer::INPUT_FILE_PATTERNS);
    options.with_stat = false;
    options.skip_unreadable = true;

    auto scoped = coro::with_scope([&](coro::Scope& scope) {
        return runtime::fs::find_all(scope, dstd::PathView(dir), options);
    });
    auto found = default_runtime().block_on(std::move(scoped));
    if (!found) return found.error();

    dstd::Vector<dstd::String> out;
    out.reserve(found->size());
    for (const auto& e : *found) out.emplace_back(e.path.str());
    std::sort(out.begin(), out.end());
    return out;
}

dstd::Result<dstd::Vector<dstd::String>> files_of(
    const dstd::Vector<dstd::String>& paths) {
    dstd::HashSet<dstd::String> seen;
    dstd::Vector<dstd::String> out;
    for (const dstd::String& p : paths) {
        if (dstd::fs::is_directory(p).value_or(false)) {
            auto under = trace_files_under(p);
            if (!under) return under.error();
            for (dstd::String& f : *under)
                if (seen.insert(f).second) out.push_back(std::move(f));
        } else if (seen.insert(p).second) {
            out.push_back(p);
        }
    }
    return out;
}

/// A View of `pinned` that reads each index through the snapshot its pin came
/// from, so a later refresh cannot show it rows past the pin.
View pinned_view(
    dstd::Vector<ViewFile> files,
    const dstd::Vector<dstd::SharedPtr<const index::Snapshot>>& snapshots) {
    auto plan = dstd::make_shared<detail::ViewPlan>(
        *detail::scan::from_files(std::move(files)));
    for (std::size_t i = 0; i < plan->files.size(); ++i) {
        const dstd::String& root = plan->files[i].index_path;
        const bool is_known = std::any_of(
            plan->index_snapshots.begin(), plan->index_snapshots.end(),
            [&](const auto& s) { return s.first == root; });
        if (!is_known) plan->index_snapshots.emplace_back(root, snapshots[i]);
    }
    return detail::view_of(std::move(plan));
}

}  // namespace

LiveView::LiveView(dstd::Vector<dstd::String> paths, dstd::String index_path)
    : paths_(std::move(paths)), index_path_(std::move(index_path)) {}

dstd::Result<View> LiveView::snapshot() {
    auto fresh_files = refresh();
    if (!fresh_files) return fresh_files.error();
    dstd::Vector<PinnedFile> refreshed = std::move(*fresh_files);
    if (refreshed.empty()) return View();
    dstd::Vector<ViewFile> pinned;
    dstd::Vector<dstd::SharedPtr<const index::Snapshot>> snapshots;
    pinned.reserve(refreshed.size());
    snapshots.reserve(refreshed.size());
    for (PinnedFile& p : refreshed) {
        pinned.push_back(std::move(p.file));
        snapshots.push_back(std::move(p.index));
    }
    return pinned_view(std::move(pinned), snapshots);
}

dstd::Result<dstd::Optional<View>> LiveView::new_rows() {
    auto fresh_files = refresh();
    if (!fresh_files) return fresh_files.error();
    dstd::Vector<PinnedFile> refreshed = std::move(*fresh_files);
    dstd::Vector<ViewFile> fresh;
    dstd::Vector<dstd::SharedPtr<const index::Snapshot>> snapshots;
    for (PinnedFile& p : refreshed) {
        if (p.has_open_line) continue;
        ViewFile& f = p.file;
        const auto seen = returned_.find(f.file_path);
        if (seen != returned_.end()) {
            index::SourceStat stored;
            stored.size = seen->second.c_end;
            stored.hash = seen->second.hash;
            const bool is_same = f.pinned_c_end == seen->second.c_end &&
                                 p.hash == seen->second.hash;
            if (is_same) continue;
            if (index::is_append_of(f.file_path, f.pinned_c_end, stored))
                f.pinned_uc_begin = seen->second.uc_end;
        }
        returned_.insert_or_assign(
            f.file_path, ReturnedEnd{f.pinned_c_end, f.pinned_uc_end, p.hash});
        if (f.pinned_uc_end > f.pinned_uc_begin) {
            fresh.push_back(std::move(f));
            snapshots.push_back(std::move(p.index));
        }
    }
    if (fresh.empty()) return dstd::Optional<View>{};
    return dstd::Optional<View>(pinned_view(std::move(fresh), snapshots));
}

dstd::Result<dstd::Vector<LiveView::PinnedFile>> LiveView::refresh() {
    auto listed = files_of(paths_);
    if (!listed) return listed.error();
    const dstd::Vector<dstd::String> files = std::move(*listed);
    dstd::Vector<common::InputFileStat> stats(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!common::stat_input(files[i], stats[i]))
            return common::make_error<"cannot stat {}">(common::ErrorCode::Io,
                                                        files[i]);
    }

    index::build::ResolveAndBuildInput in;
    for (const auto& f : files) in.files.emplace_back(f);
    in.index_dir = index_path_;
    in.require_checkpoints = true;
    in.whole_members = true;
    index::build::ResolverResult built;
    if (!files.empty()) {
        auto resolved = default_runtime().block_on(
            coro::named("live_view_snapshot",
                        index::build::resolve_and_build_index(std::move(in))));
        if (!resolved) return resolved.error();
        built = std::move(*resolved);
    }
    if (!built.failures.empty())
        return common::make_error<"index of {}: {}">(
            common::ErrorCode::Io, built.failures.front().file_path,
            built.failures.front().message);

    // The build reads the file again, so a size from before it can be behind
    // what the index holds.
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!common::stat_input(files[i], stats[i]))
            return common::make_error<"cannot stat {}">(common::ErrorCode::Io,
                                                        files[i]);
    }

    status_.clear();
    status_.reserve(files.size());
    dstd::Vector<PinnedFile> pinned;
    pinned.reserve(files.size());
    dstd::Vector<
        dstd::Pair<dstd::String, dstd::SharedPtr<const index::Snapshot>>>
        opened;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const dstd::String& path = files[i];
        LiveFileStatus st;
        st.path = path;
        const dstd::String root =
            internal::determine_index_path(path, index_path_);
        std::uint64_t uc_end = 0;
        std::size_t members = 0;
        std::uint64_t hash = 0;
        bool has_open_line = false;
        dstd::SharedPtr<const index::Snapshot> held;
        if (common::input_exists(index::store_of(root))) {
            for (const auto& [r, s] : opened)
                if (r == root) held = s;
            if (!held) {
                auto taken = index::plan::open_snapshot(root);
                if (!taken) return taken.error();
                held =
                    dstd::make_shared<const index::Snapshot>(std::move(*taken));
                opened.emplace_back(root, held);
            }
            const index::Snapshot& snap = *held;
            const auto ref =
                snap.file(index::store::internal::get_logical_path(path));
            // A record of old bytes that a new file replaced holds no rows of
            // this one until a member of it is whole.
            if (ref &&
                (stats[i].size == ref->stat().size ||
                 index::is_append_of(path, stats[i].size, ref->stat()))) {
                st.verified_bytes = ref->stat().size;
                hash = ref->stat().hash;
                const auto meta = index::file_metadata(snap, *ref);
                if (!meta) return meta.error();
                if (*meta) {
                    uc_end = (*meta)->max_bytes;
                    has_open_line = (*meta)->has_open_line;
                }
                const auto listed_members = index::members(snap, *ref);
                if (!listed_members) return listed_members.error();
                members = listed_members->size();
            }
        }
        st.skipped_bytes = stats[i].size - st.verified_bytes;
        if (st.skipped_bytes > 0) {
            st.state = LiveFileState::Tail;
            auto file = dstd::File::open(dstd::PathView(path), {});
            if (!file) return file.error();
            auto scan = index::build::find_member_after_hole(
                file->native_handle(), st.verified_bytes, stats[i].size);
            auto searched = default_runtime().block_on(std::move(scan));
            if (!searched) return searched.error();
            const dstd::Optional<std::uint64_t> after = *searched;
            if (after) {
                st.state = LiveFileState::Hole;
                st.hole_bytes = *after - st.verified_bytes;
            }
        }
        if (st.verified_bytes > 0) {
            ViewFile vf{path, root};
            vf.pinned_c_end = st.verified_bytes;
            vf.pinned_uc_end = uc_end;
            // From the pinned index, so a scan plans the chunks the pin saw.
            vf.uncompressed_size = uc_end;
            vf.checkpoint_count = members;
            pinned.push_back(PinnedFile{std::move(vf), std::move(held), hash,
                                        has_open_line});
        }
        status_.push_back(std::move(st));
    }
    return pinned;
}

}  // namespace dftracer::utils::trace::views
