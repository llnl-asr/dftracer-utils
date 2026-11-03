// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_ARROW_PARTITION_ROUTER_H
#define DFTRACER_UTILS_UTILITIES_ARROW_PARTITION_ROUTER_H

#include <dftracer/utils/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/utilities/arrow/arrow_export.h>
#include <dftracer/utils/utilities/arrow/partition_writer.h>

#include <cstdint>

namespace dftracer::utils::utilities::arrow {

struct RouterWriteStats {
    dstd::HashMap<dstd::String, PartitionWriteStats> partitions;
    int64_t total_rows = 0;
    int64_t total_uncompressed_bytes = 0;
};

struct PartitionConfig {
    enum class Mode {
        None,
        Column,
        Bucketed,
        View,
    };

    Mode mode = Mode::None;
    dstd::Vector<dstd::String> partition_columns;
    int num_buckets = 0;
    dstd::Vector<dstd::Pair<dstd::String, dstd::Optional<dstd::String>>> views;
};

using PredicateEvaluator =
    dstd::Function<bool(const dstd::HashMap<dstd::String, dstd::String>&)>;

/**
 * Routes Arrow record batches to partitioned output directories.
 * Supports column-based, bucketed, and view-based partitioning.
 */
class PartitionRouter {
   public:
    PartitionRouter() = default;
    ~PartitionRouter();

    PartitionRouter(const PartitionRouter&) = delete;
    PartitionRouter& operator=(const PartitionRouter&) = delete;
    PartitionRouter(PartitionRouter&& other) noexcept;
    PartitionRouter& operator=(PartitionRouter&& other) noexcept;

    int open(dstd::StringView output_dir, const PartitionConfig& config,
             int64_t chunk_size_bytes,
             IpcCompression compression = DEFAULT_ARROW_IPC_COMPRESSION);

    coro::CoroTask<int> write_batch(ArrowExportResult& batch);
    coro::CoroTask<RouterWriteStats> close();

    bool is_open() const noexcept { return is_open_; }

   private:
    dstd::String output_dir_;
    PartitionConfig config_;
    int64_t chunk_size_bytes_ = 0;
    IpcCompression compression_ = DEFAULT_ARROW_IPC_COMPRESSION;
    bool is_open_ = false;

    dstd::HashMap<dstd::String, dstd::UniquePtr<PartitionWriter>> writers_;
    dstd::HashMap<dstd::String, PredicateEvaluator> predicates_;

    coro::CoroTask<PartitionWriter*> get_or_create_writer(
        dstd::StringView partition_key);
    dstd::String partition_path(dstd::StringView partition_key) const;
    int compute_bucket(const dstd::Vector<dstd::String>& values) const;

    coro::CoroTask<int> route_none(ArrowExportResult& batch);
    coro::CoroTask<int> route_column(ArrowExportResult& batch);
    coro::CoroTask<int> route_bucketed(ArrowExportResult& batch);
    coro::CoroTask<int> route_view(ArrowExportResult& batch);
};

}  // namespace dftracer::utils::utilities::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_UTILITIES_ARROW_PARTITION_ROUTER_H
