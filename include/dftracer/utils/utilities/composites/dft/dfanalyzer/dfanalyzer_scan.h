#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFANALYZER_DFANALYZER_SCAN_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFANALYZER_DFANALYZER_SCAN_H

#include <dftracer/utils/core/common/config.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregator_types.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::dfanalyzer {

using common::arrow::ArrowExportResult;

// RocksDB aggregation-index handle: the read-only DB plus an EventAggregator
// bound to its stored config hash.
struct AggDbHandle {
    std::shared_ptr<dftracer::utils::rocksdb::RocksDatabase> db;
    std::unique_ptr<aggregators::EventAggregator> agg;
};

// Open the aggregation index at `index_path`. On failure returns nullptr and
// sets `error_msg`.
std::unique_ptr<AggDbHandle> open_agg_db(const std::string& index_path,
                                         std::string& error_msg);

struct AggScanInput {
    const aggregators::EventAggregator* agg;
    aggregators::AggMapType target_type;
    aggregators::AggregationBatchType batch_type;
    std::int64_t batch_size;
    std::uint16_t shard_begin;
    std::uint16_t shard_end;
};

struct AggScanOutput {
    std::vector<ArrowExportResult> results;
};

// Raw aggregation scan over a shard range: one Arrow row per stored key/value.
AggScanOutput scan_aggregation_shard_range(AggScanInput input);

enum GroupByField : std::uint32_t {
    GB_CAT = 1u << 0,
    GB_FUNC_NAME = 1u << 1,
    GB_PID = 1u << 2,
    GB_TID = 1u << 3,
    GB_FILE_HASH = 1u << 4,
    GB_HOST_HASH = 1u << 5,
    GB_FILE_NAME = 1u << 6,
    GB_HOST_NAME = 1u << 7,
    GB_PROC_NAME = 1u << 8,
    GB_IO_CAT = 1u << 9,
    GB_ACC_PAT = 1u << 10,
    GB_TIME_RANGE = 1u << 11,
};

struct GroupByConfig {
    std::uint32_t mask = 0;
    std::vector<GroupByField> order;
    std::vector<std::string> names;  // matches `order`, used for schema
};

std::optional<GroupByField> parse_group_by_name(std::string_view name);

struct DfanalyzerScanInput {
    const aggregators::EventAggregator* agg;
    const aggregators::DfanalyzerContext* ctx;
    std::optional<aggregators::AggMapType> type_filter;
    std::int64_t batch_size;
    std::uint16_t shard_begin;
    std::uint16_t shard_end;
    const GroupByConfig* group_by = nullptr;  // null = full granularity
};

struct DfanalyzerScanOutput {
    std::vector<ArrowExportResult> events;
    std::vector<ArrowExportResult> profiles;
    std::vector<ArrowExportResult> system;
};

// dfanalyzer engine: scan a shard range and emit dfanalyzer-schema rows,
// either full granularity or grouped per `input.group_by`.
DfanalyzerScanOutput scan_dfanalyzer_shards(DfanalyzerScanInput input);

// Two-pass scan over the SYSTEM_METRICS column family producing the
// system-metrics Arrow buffers with a dynamically discovered schema.
std::vector<ArrowExportResult> scan_system_metrics_buffer(
    const aggregators::EventAggregator* agg,
    const aggregators::DfanalyzerContext* ctx, std::int64_t batch_size);

}  // namespace dftracer::utils::utilities::composites::dft::dfanalyzer

#endif  // DFTRACER_UTILS_ENABLE_ARROW
#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_DFANALYZER_DFANALYZER_SCAN_H
