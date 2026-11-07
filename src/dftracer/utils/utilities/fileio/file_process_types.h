// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_H

#include <dftracer/utils/dstd/hasher.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/gzip/checkpoint_indexer.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>

#include <cstddef>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Input for processing files in a directory.
 */
struct DirectoryProcessInput {
    dstd::String directory_path;
    bool recursive = false;
    dstd::Vector<dstd::String> extensions;  ///< e.g., {".pfw", ".pfw.gz"}

    DirectoryProcessInput() = default;

    DirectoryProcessInput(dstd::StringView path,
                          dstd::Vector<dstd::String> exts = {".pfw", ".pfw.gz"},
                          bool recurse = false)
        : directory_path(path),
          recursive(recurse),
          extensions(std::move(exts)) {}

    DirectoryProcessInput& with_recursive(bool rec) {
        recursive = rec;
        return *this;
    }

    bool operator==(const DirectoryProcessInput& other) const {
        return directory_path == other.directory_path &&
               recursive == other.recursive && extensions == other.extensions;
    }

    bool operator!=(const DirectoryProcessInput& other) const {
        return !(*this == other);
    }
};

/**
 * @brief Input for reading an indexed file.
 */
struct IndexedReadInput {
    dstd::String file_path;
    dstd::String index_path;  ///< Root-local `.dftindex` path.
    std::size_t checkpoint_size = dftracer::utils::index::gzip::
        CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE;
    bool force_rebuild = false;

    IndexedReadInput() = default;

    IndexedReadInput(dstd::StringView fpath, dstd::StringView ipath,
                     std::size_t ckpt_size = dftracer::utils::index::gzip::
                         CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE,
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

/**
 * @brief Input for batch processing lines from a file.
 */
struct LineBatchInput {
    dstd::String file_path;
    dstd::String index_path;     ///< Empty for plain text files
                                 ///< or `.dftindex` for indexed archives.
    std::size_t start_line = 0;  ///< 0 = from beginning
    std::size_t end_line = 0;    ///< 0 = to end
    std::size_t checkpoint_size = dftracer::utils::index::gzip::
        CheckpointIndexer::DEFAULT_CHECKPOINT_SIZE;

    LineBatchInput() = default;

    LineBatchInput(dstd::StringView fpath, dstd::StringView ipath = "",
                   std::size_t start = 0, std::size_t end = 0)
        : file_path(fpath),
          index_path(ipath),
          start_line(start),
          end_line(end) {}

    static LineBatchInput from_file(dstd::StringView path) {
        LineBatchInput input;
        input.file_path = path;
        return input;
    }

    LineBatchInput& with_index(dstd::StringView idx) {
        index_path = idx;
        return *this;
    }

    LineBatchInput& with_line_range(std::size_t start, std::size_t end) {
        start_line = start;
        end_line = end;
        return *this;
    }

    LineBatchInput& with_checkpoint_size(std::size_t size) {
        checkpoint_size = size;
        return *this;
    }
};

/**
 * @brief Output from processing a single file.
 */
struct FileProcessOutput {
    dstd::String file_path;
    bool success = false;
    std::size_t items_processed = 0;
    dstd::String error_message;

    FileProcessOutput() = default;

    FileProcessOutput(dstd::StringView path, bool succ, std::size_t items = 0,
                      dstd::StringView error = "")
        : file_path(path),
          success(succ),
          items_processed(items),
          error_message(error) {}
};

/**
 * @brief Aggregated output from processing multiple files.
 */
template <typename T>
struct BatchFileProcessOutput {
    dstd::Vector<T> results;
    std::size_t successful_count = 0;
    std::size_t failed_count = 0;
    std::size_t total_items_processed = 0;

    BatchFileProcessOutput() = default;

    void add(T result) { results.push_back(std::move(result)); }

    void finalize() {
        successful_count = 0;
        failed_count = 0;
        total_items_processed = 0;

        for (const auto& result : results) {
            if constexpr (std::is_same_v<T, FileProcessOutput>) {
                if (result.success) {
                    successful_count++;
                    total_items_processed += result.items_processed;
                } else {
                    failed_count++;
                }
            }
        }
    }
};

}  // namespace dftracer::utils::utilities::fileio

/// Hash specializations for using workflow types in hash-based containers
namespace dftracer::utils::dstd {

template <>
struct Hash<dftracer::utils::utilities::fileio::DirectoryProcessInput> {
    using is_avalanching = void;

    std::size_t operator()(
        const dftracer::utils::utilities::fileio::DirectoryProcessInput& input)
        const {
        ::dftracer::utils::utilities::hash::HasherUtility hasher;
        hasher.update(input.directory_path);
        hasher.update(input.recursive);
        for (const auto& ext : input.extensions) {
            hasher.update(ext);
        }
        return static_cast<std::size_t>(hash::splitmix64(hasher.hash().value));
    }
};

}  // namespace dftracer::utils::dstd

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_TYPES_H
