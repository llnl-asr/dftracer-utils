#include <dftracer/utils/index/schemas/dft/agg/agg_db_open.h>
#include <dftracer/utils/index/store/db_manager.h>

#include <string>

namespace dftracer::utils::index::schemas::dft::agg {

std::unique_ptr<AggDbHandle> open_agg_db(const std::string& index_path,
                                         std::string& error_msg) {
    std::shared_ptr<index::store::RocksDatabase> db;
    try {
        db = tier::open(index_path,
                        index::store::RocksDatabase::OpenMode::ReadOnly);
    } catch (...) {
        auto& mgr = index::store::RocksDBManager::instance();
        mgr.reset(index_path);
        db = mgr.get_or_open(index_path,
                             index::store::RocksDatabase::OpenMode::ReadOnly);
    }
    if (!db || !db->is_open()) {
        error_msg = "Failed to open aggregation database";
        return nullptr;
    }
    auto config = tier::read_config(*db);
    if (!config) {
        error_msg = "No aggregation config found - was aggregation enabled?";
        return nullptr;
    }
    auto handle = std::make_unique<AggDbHandle>();
    handle->db = db;
    handle->config = *config;
    handle->agg = std::make_unique<EventAggregator>(db);
    return handle;
}

}  // namespace dftracer::utils::index::schemas::dft::agg
