#ifndef DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_CHECKPOINT_H
#define DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_CHECKPOINT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct dft_indexer_checkpoint_t {
    uint64_t checkpoint_idx;
    uint64_t uc_offset;
    uint64_t uc_size;
    uint64_t c_offset;
    uint64_t c_size;
    int bits;
    unsigned char *dict_compressed;
    size_t dict_size;
    uint64_t num_lines;
    uint64_t first_line_num;
    uint64_t last_line_num;
} dft_indexer_checkpoint_t;

void dft_indexer_free_checkpoint(dft_indexer_checkpoint_t *checkpoint);
void dft_indexer_free_checkpoints(dft_indexer_checkpoint_t *checkpoints,
                                  size_t count);

#ifdef __cplusplus
}  // extern "C"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dftracer::utils::utilities::indexer::internal {
struct IndexerCheckpoint {
    std::uint64_t checkpoint_idx;
    std::uint64_t uc_offset;
    std::uint64_t uc_size;
    std::uint64_t c_offset;
    std::uint64_t c_size;
    int bits;
    std::vector<unsigned char> dict_compressed;
    std::uint64_t num_lines;
    std::uint64_t first_line_num;
    std::uint64_t last_line_num;
};
}  // namespace dftracer::utils::utilities::indexer::internal

#endif

#endif  // DFTRACER_UTILS_UTILITIES_INDEXER_INTERNAL_CHECKPOINT_H
