// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_READER_FILE_PROCESS_TYPES_H
#define DFTRACER_UTILS_TRACE_READER_FILE_PROCESS_TYPES_H

#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/trace/index/gzip/checkpoint_indexer.h>

#include <cstddef>

namespace dftracer::utils::trace::reader {

/**
 * @brief Input for reading an indexed file.
 */
struct IndexedReadInput {
    dstd::String file_path;
    dstd::String index_path;  ///< Root-local `.dftindex` path.
    std::size_t checkpoint_size = dftracer::utils::trace::index::gzip::
        CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE;
    bool force_rebuild = false;

    IndexedReadInput() = default;

    IndexedReadInput(dstd::StringView fpath, dstd::StringView ipath,
                     std::size_t ckpt_size = dftracer::utils::trace::index::
                         gzip::CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE,
                     bool force = false)
        : file_path(fpath),
          index_path(ipath),
          checkpoint_size(ckpt_size),
          force_rebuild(force) {}

    static IndexedReadInput from_file(dstd::StringView path) {
        IndexedReadInput input;
        input.file_path = path;
        return input;
    }

    IndexedReadInput& with_index(dstd::StringView idx) {
        index_path = idx;
        return *this;
    }

    IndexedReadInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }

    IndexedReadInput& with_force_rebuild(bool force) {
        force_rebuild = force;
        return *this;
    }
};

}  // namespace dftracer::utils::trace::reader

#endif  // DFTRACER_UTILS_TRACE_READER_FILE_PROCESS_TYPES_H
