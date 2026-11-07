// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/common/str_format.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/map.h>
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

#include <algorithm>
#include <utility>

namespace dftracer::utils::trace::views {

namespace {

[[noreturn]] void throw_error(const dstd::Error& error) {
    throw common::DFTUtilsException(error);
}

dstd::Vector<dstd::String> trace_files_under(const dstd::String& dir) {
    runtime::fs::FindOptions options;
    options.patterns =
        runtime::fs::names_of(common::constants::indexer::INPUT_FILE_PATTERNS);
    options.with_stat = false;
    options.skip_unreadable = true;

    auto scoped = coro::with_scope([&](coro::Scope& scope) {
        return runtime::fs::find_all(scope, dstd::PathView(dir), options);
    });
    auto found = default_runtime().block_on(std::move(scoped));
    if (!found) throw_error(found.error());

    dstd::Vector<dstd::String> out;
    out.reserve(found->size());
    for (const auto& e : *found) out.emplace_back(e.path.str());
    std::sort(out.begin(), out.end());
    return out;
}

dstd::Vector<dstd::String> files_of(const dstd::Vector<dstd::String>& paths) {
    dstd::Set<dstd::String> seen;
    dstd::Vector<dstd::String> out;
    for (const dstd::String& p : paths) {
        if (dstd::fs::is_directory(p).value_or(false)) {
            for (dstd::String& f : trace_files_under(p))
                if (seen.insert(f).second) out.push_back(std::move(f));
        } else if (seen.insert(p).second) {
            out.push_back(p);
        }
    }
    return out;
}

}  // namespace

LiveView::LiveView(dstd::Vector<dstd::String> paths, dstd::String index_path)
    : paths_(std::move(paths)), index_path_(std::move(index_path)) {}

View LiveView::snapshot() {
    const dstd::Vector<dstd::String> files = files_of(paths_);
    dstd::Vector<common::InputFileStat> stats(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!common::stat_input(files[i], stats[i]))
            throw common::DFTUtilsException::cat(common::ErrorCode::Io,
                                                 "cannot stat ", files[i]);
    }

    index::build::ResolveAndBuildInput in;
    for (const auto& f : files) in.files.emplace_back(f);
    in.index_dir = index_path_;
    in.require_checkpoints = true;
    in.whole_members = true;
    index::build::ResolverResult built;
    if (!files.empty()) {
        built = default_runtime().block_on(
            coro::named("live_view_snapshot",
                        index::build::resolve_and_build_index(std::move(in))));
    }
    if (!built.failures.empty())
        throw common::DFTUtilsException::cat(common::ErrorCode::Io, "index of ",
                                             built.failures.front().file_path,
                                             ": ",
                                             built.failures.front().message);

    // The build reads the file again, so a size from before it can be behind
    // what the index holds.
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (!common::stat_input(files[i], stats[i]))
            throw common::DFTUtilsException::cat(common::ErrorCode::Io,
                                                 "cannot stat ", files[i]);
    }

    status_.clear();
    status_.reserve(files.size());
    dstd::Vector<ViewFile> pinned;
    pinned.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        const dstd::String& path = files[i];
        LiveFileStatus st;
        st.path = path;
        const dstd::String root =
            internal::determine_index_path(path, index_path_);
        std::uint64_t uc_end = 0;
        if (common::input_exists(index::store_of(root))) {
            const auto snap = common::unwrap(index::plan::open_snapshot(root));
            const auto ref =
                snap.file(index::store::internal::get_logical_path(path));
            // A record of old bytes that a new file replaced holds no rows of
            // this one until a member of it is whole.
            if (ref &&
                (stats[i].size == ref->stat().size ||
                 index::is_append_of(path, stats[i].size, ref->stat()))) {
                st.verified_bytes = ref->stat().size;
                const auto meta =
                    common::unwrap(index::file_metadata(snap, *ref));
                if (meta) uc_end = meta->max_bytes;
            }
        }
        st.skipped_bytes = stats[i].size - st.verified_bytes;
        if (st.skipped_bytes > 0) {
            st.state = LiveFileState::Tail;
            auto file = dstd::File::open(dstd::PathView(path), {});
            if (!file) throw_error(file.error());
            auto scan = index::build::find_member_after_hole(
                file->native_handle(), st.verified_bytes, stats[i].size);
            const dstd::Optional<std::uint64_t> after =
                default_runtime().block_on(std::move(scan));
            if (after) {
                st.state = LiveFileState::Hole;
                st.hole_bytes = *after - st.verified_bytes;
            }
        }
        if (st.verified_bytes > 0) {
            ViewFile vf{path, root};
            vf.pinned_c_end = st.verified_bytes;
            vf.pinned_uc_end = uc_end;
            pinned.push_back(std::move(vf));
        }
        status_.push_back(std::move(st));
    }
    return pinned.empty() ? View() : View::from_files(std::move(pinned));
}

}  // namespace dftracer::utils::trace::views
