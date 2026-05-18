#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_VISITOR_H

#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_output.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics.h>
#include <dftracer/utils/utilities/composites/dft/dft_event_visitor.h>
#include <dftracer/utils/utilities/indexer/index_database_sst_writer_context.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::aggregators {

class AggregationVisitor : public DftEventVisitor {
   public:
    /// Legacy mode: flush directly to a live RocksDatabase via Merge/Put
    /// during parse. FLUSH_THRESHOLD commits the visitor's own batch to
    /// bound memory. Used by `aggregator_utility.cpp`,
    /// `dftracer_aggregator.cpp`, `dftracer_organize.cpp`.
    AggregationVisitor(std::shared_ptr<rocksdb::RocksDatabase> db,
                       std::uint32_t config_hash, AggregationConfig config,
                       std::string file_path);

    /// Distributed mode: flush to a per-visitor SstWriterContext rooted at
    /// `staging_dir`. FLUSH_THRESHOLD emits partial SSTs (mixed Put+Merge)
    /// so the in-memory map never exceeds the threshold. At
    /// `on_file_complete`, the writer context is committed and its
    /// Artifacts are embedded in the ChunkAggregationOutput so the worker
    /// / coordinator can forward them to the main `SstArtifactRegistry`.
    ///
    /// `staging_dir` is typically the same node-local dir the rest of the
    /// SST pipeline uses. `batch_id_prefix` is joined with a per-file
    /// suffix to form a unique SstWriterContext root (so concurrent
    /// per-file visitors never collide).
    AggregationVisitor(std::string staging_dir, std::string batch_id_prefix,
                       std::uint32_t config_hash, AggregationConfig config,
                       std::string file_path);

    void begin(std::size_t num_checkpoints) override;
    void on_checkpoint(std::size_t checkpoint_idx) override;
    void on_event(const EventRecord& record) override;
    coro::CoroTask<void> on_file_complete() override;
    bool needs_args_map() const override { return true; }

    ChunkAggregationOutput take_output();
    void flush_to_batch(rocksdb::RocksDatabase::Batch& batch);

    const std::unordered_set<std::string>& observed_extra_keys() const {
        return observed_extra_keys_;
    }
    const std::unordered_set<std::string>& observed_custom_metrics() const {
        return observed_custom_metrics_;
    }

    /// Distributed mode only: one or more per-flush SST artifact sets
    /// produced by this visitor after `on_file_complete`. Each flush
    /// emits its own SST(s) because `SstFileWriter` requires strictly
    /// ascending keys and merge operands for the same key across flushes
    /// would violate that invariant. Empty in legacy mode.
    std::vector<indexer::IndexDatabaseSstWriterContext::Artifacts>&
    aggregation_artifacts() noexcept {
        return sst_artifacts_;
    }

   private:
    void seal_local_buffer();
    void handle_system_event(const EventRecord& record);

    // Legacy (RocksDatabase-backed) mode.
    std::shared_ptr<rocksdb::RocksDatabase> db_;
    std::vector<rocksdb::RocksDatabase::Batch> pending_batches_;

    // Distributed (SST-backed) mode. The visitor rotates sst_sink_ per
    // flush to keep each SST's key space strictly ascending (merge
    // operands for the same key across flushes must live in separate
    // SSTs). `sst_staging_dir_` and `sst_batch_prefix_` persist across
    // rotations so the next SstWriterContext can be constructed at
    // flush time.
    std::unique_ptr<indexer::IndexDatabaseSstWriterContext> sst_sink_;
    std::string sst_staging_dir_;
    std::string sst_batch_prefix_;
    std::size_t sst_flush_counter_ = 0;
    std::vector<indexer::IndexDatabaseSstWriterContext::Artifacts>
        sst_artifacts_;

    std::uint32_t config_hash_;
    AggregationConfig config_;
    std::string file_path_;

    std::shared_ptr<AssociationTracker> tracker_;
    std::size_t events_processed_ = 0;

    static constexpr std::size_t FLUSH_THRESHOLD = 65536;
    std::unordered_map<std::string, AggregationMetrics, TransparentStringHash,
                       TransparentStringEqual>
        local_buffer_;
    std::string key_buf_;
    std::string val_buf_;

    AggregationMetrics* last_entry_ = nullptr;
    std::string_view last_key_;

    // System metrics buffer (keyed by hhash + time_bucket)
    std::unordered_map<std::string, SystemAggregationMetrics,
                       TransparentStringHash, TransparentStringEqual>
        system_buffer_;
    std::string system_key_buf_;
    std::string system_val_buf_;

    std::unordered_set<std::string> observed_extra_keys_;
    std::unordered_set<std::string> observed_custom_metrics_;
    std::unordered_set<std::string> observed_system_metrics_;

    std::uint64_t min_time_bucket_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_time_bucket_ = 0;
};

}  // namespace dftracer::utils::utilities::composites::dft::aggregators

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_AGGREGATORS_AGGREGATION_VISITOR_H
