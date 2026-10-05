#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/index/build/index_write_lock.h>
#include <dftracer/utils/index/extensions/catalog_fold.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/internal/helpers.h>

#include <exception>
#include <mutex>

namespace dftracer::utils::index::extensions {

void observe_catalog_path(index::store::PathStat& stat, std::uint8_t tag) {
    const auto t = static_cast<index::store::PathType>(tag);
    stat.type = index::store::join(stat.type, t);
    if (t <= index::store::PathType::MIXED)
        stat.seen |= static_cast<std::uint8_t>(1U << tag);
    if (t != index::store::PathType::NULL_VALUE) ++stat.count;
}

void CatalogFold::step(const trace::views::detail::FoldBatch& batch) {
    FileState& fs = files_[batch.unit.file_path];
    if (fs.index_path.empty()) fs.index_path = batch.unit.index_path;
    for (const auto& e : batch.events) {
        if (e.phase == trace::RecordPhase::METADATA) continue;
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

        int file_id = -1;
        try {
            index::store::IndexDatabase ro(
                fs.index_path, index::store::IndexOpenMode::ReadOnly);
            file_id = ro.get_file_info_id(
                index::store::internal::get_logical_path(file));
            if (file_id < 0 ||
                ro.extension_current(file_id, IndexExtension::CATALOG))
                continue;
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("catalog check failed for %s: %s",
                                    fs.index_path.c_str(), e.what());
            continue;
        }

        std::unique_lock<std::mutex> lk(
            index::build::index_write_mutex(fs.index_path));
        try {
            index::store::IndexDatabase db(fs.index_path);
            auto writer = db.begin_write();
            index::store::records::clear_file(*writer, IndexExtension::CATALOG,
                                              file_id);
            for (const auto& [id, stat] : fs.paths)
                index::store::records::put_catalog_path(
                    *writer, file_id, intern_->resolve(id), stat);
            index::store::records::put_manifest(
                *writer, file_id, IndexExtension::CATALOG, params_hash_);
            writer->commit();
            wrote = true;
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_WARN("catalog write skipped for %s: %s",
                                    fs.index_path.c_str(), e.what());
        }
    }
    co_return wrote;
}

}  // namespace dftracer::utils::index::extensions
