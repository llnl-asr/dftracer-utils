#ifndef DFTRACER_UTILS_TRACE_AGGREGATORS_AGG_DB_OPEN_H
#define DFTRACER_UTILS_TRACE_AGGREGATORS_AGG_DB_OPEN_H

#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/trace/aggregators/event_aggregator.h>

#include <memory>
#include <string>

namespace dftracer::utils::trace::aggregators {

/// RocksDB aggregation-index handle: the read-only DB plus an EventAggregator
/// bound to its stored config hash.
struct AggDbHandle {
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db;
    std::unique_ptr<EventAggregator> agg;
};

/// Open the aggregation index at `index_path`. On failure returns nullptr and
/// sets `error_msg`.
std::unique_ptr<AggDbHandle> open_agg_db(const std::string& index_path,
                                         std::string& error_msg);

}  // namespace dftracer::utils::trace::aggregators

#endif
