#ifndef DFTRACER_UTILS_INDEXER_INDEXER_H
#define DFTRACER_UTILS_INDEXER_INDEXER_H

#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/indexer/checkpoint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *dft_indexer_handle_t;

// C API function declarations
dft_indexer_handle_t dft_indexer_create(const char *gz_path,
                                        const char *idx_path,
                                        uint64_t checkpoint_size,
                                        int force_rebuild);
int dft_indexer_build(dft_indexer_handle_t indexer);
int dft_indexer_need_rebuild(dft_indexer_handle_t indexer);
int dft_indexer_exists(dft_indexer_handle_t indexer);
uint64_t dft_indexer_get_max_bytes(dft_indexer_handle_t indexer);
uint64_t dft_indexer_get_num_lines(dft_indexer_handle_t indexer);
int dft_indexer_find_checkpoint(dft_indexer_handle_t indexer,
                                size_t target_offset,
                                dft_indexer_checkpoint_t *checkpoint);
int dft_indexer_get_checkpoints(dft_indexer_handle_t indexer,
                                dft_indexer_checkpoint_t **checkpoints,
                                size_t *count);
void dft_indexer_destroy(dft_indexer_handle_t indexer);

#ifdef __cplusplus
}

#include <dftracer/utils/core/common/archive_format.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dftracer::utils {

/**
 * Abstract base interface for all indexer implementations.
 * This provides a common API for GZIP, TAR.GZ, and other archive indexers.
 */
class Indexer {
   public:
    static constexpr std::uint64_t DEFAULT_CHECKPOINT_SIZE =
        constants::indexer::DEFAULT_CHECKPOINT_SIZE;

    virtual ~Indexer() = default;

    // Core indexer operations
    virtual void build() const = 0;
    virtual bool need_rebuild() const = 0;
    virtual bool exists() const = 0;

    // Metadata accessors
    virtual const std::string &get_idx_path() const = 0;
    virtual const std::string &get_archive_path() const = 0;
    virtual std::uint64_t get_checkpoint_size() const = 0;
    virtual std::uint64_t get_max_bytes() const = 0;
    virtual std::uint64_t get_num_lines() const = 0;

    // Checkpoint-related functionality
    virtual bool find_checkpoint(std::size_t target_offset,
                                 IndexerCheckpoint &checkpoint) const = 0;
    virtual std::vector<IndexerCheckpoint> get_checkpoints() const = 0;
    virtual std::vector<IndexerCheckpoint> get_checkpoints_for_line_range(
        std::uint64_t start_line, std::uint64_t end_line) const = 0;

    // Archive format identification
    virtual ArchiveFormat get_format_type() const = 0;
    virtual const char *get_format_name() const = 0;

   protected:
    Indexer() = default;
    Indexer(const Indexer &) = delete;
    Indexer &operator=(const Indexer &) = delete;
};

}  // namespace dftracer::utils

#endif  // __cplusplus

#endif  // DFTRACER_UTILS_INDEXER_INDEXER_H
