#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_METADATA_COLLECTOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_METADATA_COLLECTOR_H

#include <dftracer/utils/core/tasks/task_context.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/indexer/indexer_factory.h>
#include <dftracer/utils/reader/reader_factory.h>

#include <string>

namespace dftracer::utils::utilities::composites::dft {

/**
 * @brief Input for DFTracer file metadata collection.
 */
struct MetadataCollectorUtilityInput {
    std::string file_path;
    std::string idx_path;  // Empty for plain files
    std::size_t checkpoint_size = Indexer::DEFAULT_CHECKPOINT_SIZE;
    bool force_rebuild = false;

    MetadataCollectorUtilityInput() = default;

    MetadataCollectorUtilityInput(
        std::string fpath, std::string ipath = "",
        std::size_t ckpt = Indexer::DEFAULT_CHECKPOINT_SIZE, bool force = false)
        : file_path(std::move(fpath)),
          idx_path(std::move(ipath)),
          checkpoint_size(ckpt),
          force_rebuild(force) {}

    static MetadataCollectorUtilityInput from_file(std::string path) {
        MetadataCollectorUtilityInput input;
        input.file_path = std::move(path);
        return input;
    }

    MetadataCollectorUtilityInput& with_index(std::string idx) {
        idx_path = std::move(idx);
        return *this;
    }

    MetadataCollectorUtilityInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }

    MetadataCollectorUtilityInput& with_force_rebuild(bool force) {
        force_rebuild = force;
        return *this;
    }

    bool operator==(const MetadataCollectorUtilityInput& other) const {
        return file_path == other.file_path && idx_path == other.idx_path &&
               checkpoint_size == other.checkpoint_size &&
               force_rebuild == other.force_rebuild;
    }
};

/**
 * @brief Output from DFTracer file metadata collection.
 */
struct MetadataCollectorUtilityOutput {
    std::string file_path;
    std::string idx_path;
    double size_mb = 0;
    std::size_t start_line = 0;
    std::size_t end_line = 0;
    std::size_t valid_events = 0;
    double size_per_line = 0;
    bool success = false;

    MetadataCollectorUtilityOutput() = default;

    bool operator==(const MetadataCollectorUtilityOutput& other) const {
        return file_path == other.file_path && idx_path == other.idx_path &&
               size_mb == other.size_mb && start_line == other.start_line &&
               end_line == other.end_line &&
               valid_events == other.valid_events &&
               size_per_line == other.size_per_line && success == other.success;
    }
};

/**
 * @brief Collects metadata (size, line count, events) from DFTracer trace
 * files.
 *
 * Supports both plain (.pfw) and compressed (.pfw.gz) files.
 * For compressed files, builds/uses an index for efficient access.
 */
class MetadataCollectorUtility
    : public utilities::Utility<MetadataCollectorUtilityInput,
                                MetadataCollectorUtilityOutput> {
   public:
    MetadataCollectorUtility() = default;

    MetadataCollectorUtilityOutput process(
        const MetadataCollectorUtilityInput& input) override;

   private:
    MetadataCollectorUtilityOutput process_compressed(
        const MetadataCollectorUtilityInput& input);
    MetadataCollectorUtilityOutput process_plain(
        const MetadataCollectorUtilityInput& input);
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFTRACER_METADATA_COLLECTOR_H
