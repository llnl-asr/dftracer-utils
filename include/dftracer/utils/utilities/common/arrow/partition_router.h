#ifndef DFTRACER_UTILS_UTILITIES_COMMON_ARROW_PARTITION_ROUTER_H
#define DFTRACER_UTILS_UTILITIES_COMMON_ARROW_PARTITION_ROUTER_H

#include <dftracer/utils/core/common/config.h>
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/common/arrow/arrow_export.h>
#include <dftracer/utils/utilities/common/arrow/partition_writer.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::common::arrow {

struct RouterWriteStats {
    std::unordered_map<std::string, PartitionWriteStats> partitions;
    int64_t total_rows = 0;
    int64_t total_uncompressed_bytes = 0;
};

struct PartitionConfig {
    enum class Mode {
        NONE,
        COLUMN,
        BUCKETED,
        VIEW,
    };

    Mode mode = Mode::NONE;
    std::vector<std::string> partition_columns;
    int num_buckets = 0;
    std::vector<std::pair<std::string, std::optional<std::string>>> views;
};

using PredicateEvaluator =
    std::function<bool(const std::unordered_map<std::string, std::string>&)>;

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

    int open(const std::string& output_dir, const PartitionConfig& config,
             int64_t chunk_size_bytes,
             IpcCompression compression = DEFAULT_ARROW_IPC_COMPRESSION);

    void register_predicate(const std::string& view_name,
                            PredicateEvaluator evaluator);

    coro::CoroTask<int> write_batch(ArrowExportResult& batch);
    coro::CoroTask<RouterWriteStats> close();

    bool is_open() const noexcept { return is_open_; }

   private:
    std::string output_dir_;
    PartitionConfig config_;
    int64_t chunk_size_bytes_ = 0;
    IpcCompression compression_ = DEFAULT_ARROW_IPC_COMPRESSION;
    bool is_open_ = false;

    std::unordered_map<std::string, std::unique_ptr<PartitionWriter>> writers_;
    std::unordered_map<std::string, PredicateEvaluator> predicates_;

    coro::CoroTask<PartitionWriter*> get_or_create_writer(
        const std::string& partition_key);
    std::string partition_path(const std::string& partition_key) const;
    int compute_bucket(const std::vector<std::string>& values) const;

    coro::CoroTask<int> route_none(ArrowExportResult& batch);
    coro::CoroTask<int> route_column(ArrowExportResult& batch);
    coro::CoroTask<int> route_bucketed(ArrowExportResult& batch);
    coro::CoroTask<int> route_view(ArrowExportResult& batch);
};

}  // namespace dftracer::utils::utilities::common::arrow

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC
#endif  // DFTRACER_UTILS_UTILITIES_COMMON_ARROW_PARTITION_ROUTER_H
