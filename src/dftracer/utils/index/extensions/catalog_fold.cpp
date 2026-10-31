#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/index/extensions/catalog_fold.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/index/tables.h>

#include <exception>
#include <string>

namespace dftracer::utils::index::extensions {

void observe_catalog_path(index::store::PathStat& stat, std::uint8_t tag) {
    const auto t = static_cast<index::store::PathType>(tag);
    stat.type = index::store::join(stat.type, t);
    if (t <= index::store::PathType::Mixed)
        stat.seen |= static_cast<std::uint8_t>(1U << tag);
    if (t != index::store::PathType::NullValue) ++stat.count;
}

void CatalogFold::step(const trace::views::detail::FoldBatch& batch) {
    FileState& fs = files_[batch.unit.file_path];
    if (fs.index_path.empty()) fs.index_path = batch.unit.index_path;
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

coro::CoroTask<bool> CatalogFold::finalize(
    const trace::views::detail::CoverageSet& covered) {
    using index::store::IndexExtension;
    bool wrote = false;
    for (auto& [file, fs] : files_) {
        if (fs.index_path.empty() || fs.paths.empty()) continue;
        if (!covered.covers_file(file)) continue;

        const std::string logical =
            index::store::internal::get_logical_path(file);
        try {
            index::store::IndexDatabase ro(
                fs.index_path, index::store::IndexOpenMode::ReadOnly);
            const int file_id = ro.get_file_info_id(logical);
            if (file_id < 0 ||
                ro.extension_current(file_id, IndexExtension::Catalog))
                continue;
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("catalog check failed for %s: %s",
                                    fs.index_path.c_str(), e.what());
            continue;
        }

        auto opened = index::Index::open(
            index::store::internal::normalize_index_root(fs.index_path));
        auto build = opened
                         ? index::rebuild(*opened, logical)
                         : dstd::Expected<index::FileBuild, dstd::Error>(
                               dstd::Unexpected<dstd::Error>(opened.error()));
        std::string failure;
        if (build) {
            for (const auto& [id, stat] : fs.paths)
                index::put_catalog_path(*build, intern_->resolve(id), stat);
            build->finish(IndexExtension::Catalog, params_hash_);
            auto committed = co_await build->commit();
            if (!committed) failure = std::string(committed.error().message());
        } else {
            failure = std::string(build.error().message());
        }
        if (!failure.empty()) {
            DFTRACER_UTILS_LOG_WARN("catalog write skipped for %s: %s",
                                    fs.index_path.c_str(), failure.c_str());
            continue;
        }
        wrote = true;
    }
    co_return wrote;
}

}  // namespace dftracer::utils::index::extensions
