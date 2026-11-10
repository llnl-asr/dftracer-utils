// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/extensions/catalog_fold.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/store/root_layout.h>
#include <dftracer/utils/index/tables.h>

#include <exception>

namespace dftracer::utils::index::extensions {

void observe_catalog_path(index::store::PathStat& stat, std::uint8_t tag) {
    const auto t = static_cast<index::store::PathType>(tag);
    stat.type = index::store::join(stat.type, t);
    if (t <= index::store::PathType::Mixed)
        stat.seen |= static_cast<std::uint8_t>(1U << tag);
    if (t != index::store::PathType::NullValue) ++stat.count;
}

void add_path_stat(dstd::HashMap<dstd::String, index::store::PathStat>& paths,
                   dstd::StringView path, const index::store::PathStat& stat) {
    auto [pos, inserted] = paths.try_emplace(dstd::String(path), stat);
    if (inserted) return;
    pos->second.type = index::store::join(pos->second.type, stat.type);
    pos->second.seen |= stat.seen;
    pos->second.count += stat.count;
}

void CatalogFold::step(const trace::views::detail::FoldBatch& batch) {
    const dstd::StringView file = batch.unit.file_path;
    auto it = files_.find(file);
    if (it == files_.end()) it = files_.try_emplace(dstd::String(file)).first;
    FileState& fs = it->second;
    if (fs.index_path.empty())
        fs.index_path.assign(dstd::StringView(batch.unit.index_path));
    for (const auto& e : batch.events) {
        if (e.phase == trace::RecordPhase::Metadata) continue;
        for (const auto& [leaf_id, tag] : e.schema_leaves)
            observe_catalog_path(fs.paths[leaf_id], tag);
    }
}

void CatalogFold::merge(trace::views::detail::Fold& slice) {
    auto& other = static_cast<CatalogFold&>(slice);
    for (auto& [file, ofs] : other.files_) {
        FileState& fs = files_[file];
        if (fs.index_path.empty()) fs.index_path = ofs.index_path;
        for (const auto& [path, o] : ofs.paths) {
            auto& st = fs.paths[path];
            st.type = index::store::join(st.type, o.type);
            st.seen |= o.seen;
            st.count += o.count;
        }
    }
}

dstd::HashMap<dstd::String, index::store::PathStat> CatalogFold::paths() const {
    dstd::HashMap<dstd::String, index::store::PathStat> out;
    for (const auto& [file, fs] : files_) {
        for (const auto& [id, stat] : fs.paths)
            add_path_stat(out, intern_->resolve(id), stat);
    }
    return out;
}

coro::CoroTask<bool> CatalogFold::finalize(
    const trace::views::detail::CoverageSet& covered) {
    using index::store::IndexExtension;
    bool wrote = false;
    if (!should_write_) co_return false;
    dstd::HashMap<dstd::String, index::Snapshot> snapshots;
    for (auto& [file, fs] : files_) {
        if (fs.index_path.empty() || fs.paths.empty()) continue;
        if (!covered.covers_file(file)) continue;

        const dstd::String logical =
            index::store::internal::get_logical_path(file);
        try {
            auto at = snapshots.find(dstd::StringView(fs.index_path));
            if (at == snapshots.end()) {
                const auto opened = common::unwrap(index::Index::open(
                    index::normalize_index_root(fs.index_path)));
                at = snapshots
                         .emplace(dstd::String(fs.index_path),
                                  common::unwrap(opened.snapshot()))
                         .first;
            }
            const index::Snapshot& snapshot = at->second;
            const auto ref = snapshot.file(logical);
            if (!ref || index::extension_current(snapshot, *ref,
                                                 IndexExtension::Catalog))
                continue;
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("catalog check failed for {}: {}",
                                    fs.index_path, e.what());
            continue;
        }

        auto opened =
            index::Index::open(index::normalize_index_root(fs.index_path));
        auto build = opened
                         ? index::rebuild(*opened, logical)
                         : dstd::Expected<index::FileBuild, dstd::Error>(
                               dstd::Unexpected<dstd::Error>(opened.error()));
        dstd::String failure;
        if (build) {
            for (const auto& [id, stat] : fs.paths)
                index::put_catalog_path(*build, intern_->resolve(id), stat);
            build->finish(IndexExtension::Catalog, params_hash_);
            auto committed = co_await build->commit();
            if (!committed) failure = dstd::String(committed.error().message());
        } else {
            failure = dstd::String(build.error().message());
        }
        if (!failure.empty()) {
            DFTRACER_UTILS_LOG_WARN("catalog write skipped for {}: {}",
                                    fs.index_path, failure);
            continue;
        }
        wrote = true;
    }
    co_return wrote;
}

}  // namespace dftracer::utils::index::extensions
