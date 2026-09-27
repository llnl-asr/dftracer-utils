#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/dataframe/batch_ops.h>
#include <dftracer/utils/dataframe/internal/ipc.h>
#include <dftracer/utils/index/plan/rowsets.h>
#include <dftracer/utils/index/store/internal/helpers.h>

#include <algorithm>
#include <map>

namespace dftracer::utils::index::plan {

namespace df = dftracer::utils::dataframe;

const RecordSchema* recorded_schema(const store::IndexDatabase& db,
                                    const std::string& trace) {
    const int fid =
        db.get_file_info_id(index::store::internal::get_logical_path(trace));
    if (fid < 0) return nullptr;
    const auto id = db.file_schema(fid);
    return id ? &get_schema(*id) : nullptr;
}

const RecordSchema& files_schema(const std::vector<RowSetFile>& files) {
    std::map<std::string, std::optional<store::IndexDatabase>, std::less<>> dbs;
    // A schema that cannot be read here (an unreadable index or trace) is
    // dftracer's; the scan itself reports a missing trace or a broken index.
    std::vector<const RecordSchema*> schemas;
    schemas.reserve(files.size());
    for (const auto& f : files) {
        const RecordSchema* p = nullptr;
        if (!f.index.empty() && fs::exists(f.index)) {
            load_index_schemas(f.index);
            auto [it, fresh] = dbs.try_emplace(f.index);
            if (fresh) {
                try {
                    it->second.emplace(f.index, store::IndexOpenMode::ReadOnly);
                } catch (const std::exception&) {
                }
            }
            // A recorded schema that is not registered is an error, not a
            // reason to detect again.
            if (it->second) p = recorded_schema(*it->second, f.trace);
        }
        if (!p && fs::exists(f.trace)) p = &detect_file_schema(f.trace);
        schemas.push_back(p ? p : &get_schema("dftracer"));
    }
    if (schemas.empty()) return get_schema("dftracer");
    const RecordSchema* found = schemas.front();
    if (std::all_of(schemas.begin(), schemas.end(),
                    [&](const RecordSchema* s) { return s == found; }))
        return *found;
    found = nullptr;
    std::size_t voter = 0;
    const RecordSchema* fallback = nullptr;
    for (std::size_t i = 0; i < schemas.size(); ++i) {
        const auto d = fs::exists(files[i].trace)
                           ? explain_file_schema(files[i].trace)
                           : SchemaDetection{};
        if (d.records == 0) {
            if (!fallback && d.objects > 0) fallback = schemas[i];
            continue;
        }
        if (found && found != schemas[i])
            throw DFTUtilsException::cat(
                ErrorCode::INVALID_ARGUMENT,
                "a View reads files of one record schema: ", files[voter].trace,
                " is ", found->id, " and ", files[i].trace, " is ",
                schemas[i]->id);
        if (!found) {
            found = schemas[i];
            voter = i;
        }
    }
    if (found) return *found;
    return fallback ? *fallback : get_schema("generic");
}

std::optional<df::DataFrame> stored_rowset(const std::vector<RowSetFile>& files,
                                           std::string_view name) {
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (files.empty()) return std::nullopt;
    std::map<std::string, std::optional<store::IndexDatabase>, std::less<>> dbs;
    std::vector<df::DataFrame> parts;
    try {
        for (const auto& f : files) {
            if (f.index.empty()) return std::nullopt;
            auto [it, fresh] = dbs.try_emplace(f.index);
            if (fresh)
                it->second.emplace(f.index, store::IndexOpenMode::ReadOnly);
            const store::IndexDatabase& db = *it->second;
            if (db.check_freshness(f.trace) !=
                store::IndexDatabase::Freshness::Fresh)
                return std::nullopt;
            const int fid =
                db.get_file_info_id(store::internal::get_logical_path(f.trace));
            if (fid < 0) return std::nullopt;
            auto bytes = db.rowset(fid, name);
            if (!bytes) return std::nullopt;
            auto frame = df::frame_from_ipc(*bytes);
            if (!frame) return std::nullopt;
            parts.push_back(std::move(*frame));
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (parts.size() == 1) return std::move(parts.front());
    std::vector<const df::DataFrame*> ptrs;
    for (const auto& p : parts) ptrs.push_back(&p);
    return df::concat(ptrs, df::ConcatHow::Diagonal);
#else
    (void)files;
    (void)name;
    return std::nullopt;
#endif
}

bool has_stored_rowsets(const std::vector<RowSetFile>& files) {
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    if (files.empty()) return false;
    std::map<std::string, std::optional<store::IndexDatabase>, std::less<>> dbs;
    try {
        for (const auto& f : files) {
            if (f.index.empty()) return false;
            auto [it, fresh] = dbs.try_emplace(f.index);
            if (fresh)
                it->second.emplace(f.index, store::IndexOpenMode::ReadOnly);
            const store::IndexDatabase& db = *it->second;
            if (db.check_freshness(f.trace) !=
                store::IndexDatabase::Freshness::Fresh)
                return false;
            const int fid =
                db.get_file_info_id(store::internal::get_logical_path(f.trace));
            if (fid < 0 ||
                !db.extension_current(fid, store::IndexExtension::ROWSET))
                return false;
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
#else
    (void)files;
    return false;
#endif
}

}  // namespace dftracer::utils::index::plan
