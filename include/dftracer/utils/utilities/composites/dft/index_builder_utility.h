#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_INDEX_BUILDER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_INDEX_BUILDER_UTILITY_H

#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/utilities/utilities.h>
#include <dftracer/utils/utilities/composites/types.h>
#include <dftracer/utils/utilities/indexer/internal/indexer_factory.h>

#include <memory>
#include <string>

namespace dftracer::utils::utilities::composites::dft {

/**
 * @brief Input for building a single index file.
 */
struct IndexBuildUtilityInput {
    std::string file_path;
    std::string idx_path;
    std::size_t checkpoint_size = dftracer::utils::utilities::indexer::
        internal::Indexer::DEFAULT_CHECKPOINT_SIZE;
    bool force_rebuild = false;

    IndexBuildUtilityInput() = default;

    IndexBuildUtilityInput(std::string fpath, std::string ipath = "",
                           std::size_t ckpt =
                               dftracer::utils::utilities::indexer::internal::
                                   Indexer::DEFAULT_CHECKPOINT_SIZE,
                           bool force = false)
        : file_path(std::move(fpath)),
          idx_path(std::move(ipath)),
          checkpoint_size(ckpt),
          force_rebuild(force) {}

    static IndexBuildUtilityInput from_file(std::string path) {
        IndexBuildUtilityInput input;
        input.file_path = std::move(path);
        return input;
    }

    IndexBuildUtilityInput& with_index(std::string idx) {
        idx_path = std::move(idx);
        return *this;
    }

    IndexBuildUtilityInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }

    IndexBuildUtilityInput& with_force_rebuild(bool force) {
        force_rebuild = force;
        return *this;
    }
};

/**
 * @brief Output from building an index file.
 */
struct IndexBuildUtilityOutput {
    std::string file_path;
    std::string idx_path;
    bool success = false;
    bool was_built =
        false;  // true if index was newly built, false if already existed

    IndexBuildUtilityOutput() = default;
};

/**
 * @brief Utility for building index files.
 *
 * This utility ensures that index files are built/validated before
 * they are used for reading. This avoids concurrent write conflicts
 * when multiple readers try to build the same index simultaneously.
 *
 * Usage:
 * @code
 * IndexBuilder builder;
 * auto result = builder.process(IndexBuildUtilityInput::from_file("data.gz"));
 * if (result.success) {
 *     // Index is ready, can now create readers
 * }
 * @endcode
 */
class IndexBuilderUtility : public utilities::Utility<IndexBuildUtilityInput,
                                                      IndexBuildUtilityOutput> {
   public:
    IndexBuilderUtility() = default;

    IndexBuildUtilityOutput process(
        const IndexBuildUtilityInput& input) override {
        IndexBuildUtilityOutput output;
        output.file_path = input.file_path;
        output.idx_path = input.idx_path;
        output.success = false;
        output.was_built = false;

        try {
            // Validate input file exists
            if (!fs::exists(input.file_path)) {
                throw std::runtime_error("File does not exist: " +
                                         input.file_path);
            }

            // Determine final index path
            std::string final_idx_path = input.idx_path;
            if (final_idx_path.empty()) {
                final_idx_path = input.file_path + ".idx";
            }
            output.idx_path = final_idx_path;

            // Check if index needs to be built/rebuilt
            bool need_build =
                !fs::exists(final_idx_path) || input.force_rebuild;

            if (need_build) {
                // Remove old index if forcing rebuild
                if (input.force_rebuild && fs::exists(final_idx_path)) {
                    fs::remove(final_idx_path);
                }

                // Build new index
                auto indexer = dftracer::utils::utilities::indexer::internal::
                    IndexerFactory::create(input.file_path, final_idx_path,
                                           input.checkpoint_size, true);
                indexer->build();
                output.was_built = true;
            } else {
                // Check if existing index needs rebuild
                auto indexer = dftracer::utils::utilities::indexer::internal::
                    IndexerFactory::create(input.file_path, final_idx_path,
                                           input.checkpoint_size, false);

                if (indexer->need_rebuild()) {
                    // Rebuild the index
                    fs::remove(final_idx_path);
                    auto new_indexer = dftracer::utils::utilities::indexer::
                        internal::IndexerFactory::create(
                            input.file_path, final_idx_path,
                            input.checkpoint_size, true);
                    new_indexer->build();
                    output.was_built = true;
                }
            }

            output.success = true;
        } catch (const std::exception& e) {
            DFTRACER_UTILS_LOG_ERROR("Failed to build index for %s: %s",
                                     input.file_path.c_str(), e.what());
            output.success = false;
        }

        return output;
    }
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_INDEX_BUILDER_UTILITY_H
