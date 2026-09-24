#ifndef DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_DB_OPEN_H
#define DFTRACER_UTILS_INDEX_SCHEMAS_DFT_AGG_AGG_DB_OPEN_H

#include <dftracer/utils/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/index/schemas/dft/agg/event_aggregator.h>
#include <dftracer/utils/index/store/database.h>

#include <memory>
#include <string>

namespace dftracer::utils::index::schemas::dft::agg {

/// RocksDB aggregation-index handle: the read-only DB, its tier config and an
/// EventAggregator over it.
struct AggDbHandle {
    std::shared_ptr<index::store::RocksDatabase> db;
    tier::Config config;
    std::unique_ptr<EventAggregator> agg;
};

/// Open the aggregation index at `index_path`. On failure returns nullptr and
/// sets `error_msg`.
std::unique_ptr<AggDbHandle> open_agg_db(const std::string& index_path,
                                         std::string& error_msg);

}  // namespace dftracer::utils::index::schemas::dft::agg

#endif
