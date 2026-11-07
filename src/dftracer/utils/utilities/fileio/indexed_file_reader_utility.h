// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_UTILITIES_FILEIO_INDEXED_FILE_READER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_FILEIO_INDEXED_FILE_READER_UTILITY_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/utilities/fileio/file_process_types.h>
#include <dftracer/utils/utilities/reader/internal/reader.h>

namespace dftracer::utils::utilities::fileio {

/**
 * @brief Workflow utility for managing indexed file reading.
 *
 * This workflow handles:
 * 1. Index existence checking
 * 2. Index building/rebuilding if needed
 * 3. Reader creation from indexed file
 *
 * This encapsulates the common pattern from your binaries where you need to
 * ensure an index exists before creating a Reader.
 *
 * Usage:
 * @code
 * IndexedFileReader reader_workflow;
 * auto reader = reader_workflow.process(
 *     IndexedReadInput{"file.gz", ".dftindex", checkpoint_size, false}
 * );
 * // Now use reader to read lines
 * @endcode
 */
class IndexedFileReaderUtility {
   public:
    /**
     * @brief Ensure the index exists (build/rebuild as needed) and open a
     * Reader over @p input.file_path.
     *
     * @param input Index configuration
     * @return Shared pointer to Reader ready for use
     */
    coro::CoroTask<dstd::SharedPtr<reader::internal::Reader>> operator()(
        const IndexedReadInput& input) const;
};

}  // namespace dftracer::utils::utilities::fileio

#endif  // DFTRACER_UTILS_UTILITIES_FILEIO_INDEXED_FILE_READER_UTILITY_H
