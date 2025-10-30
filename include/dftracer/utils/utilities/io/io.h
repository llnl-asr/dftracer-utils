#ifndef DFTRACER_UTILS_UTILITIES_IO_IO_H
#define DFTRACER_UTILS_UTILITIES_IO_IO_H

/**
 * @file io.h
 * @brief Convenience header that includes all I/O utilities.
 *
 * This header provides access to:
 * - File readers (StreamingFileReader, FileReader, BinaryFileReader)
 * - File writers (StreamingFileWriter)
 * - Streaming utilities (ChunkIterator, ChunkRange)
 * - I/O types (RawData, CompressedData, ChunkSpec, ChunkManifest)
 * - Line-based I/O (LineRange, LineBytesRange, StreamingLineReader)
 *
 * Usage:
 * @code
 * #include <dftracer/utils/utilities/io/io.h>
 *
 * // All I/O utilities are now available
 * auto reader = std::make_shared<StreamingFileReader>();
 * StreamReadInput input{"/path/to/file.txt", 64 * 1024};
 * ChunkRange chunks = reader->process(input);
 * @endcode
 */

// File readers and writers
#include <dftracer/utils/utilities/io/binary_file_reader_utility.h>
#include <dftracer/utils/utilities/io/file_reader_utility.h>
#include <dftracer/utils/utilities/io/streaming_file_reader_utility.h>
#include <dftracer/utils/utilities/io/streaming_file_writer_utility.h>

// I/O types
#include <dftracer/utils/utilities/io/types/types.h>

// Line-based I/O
#include <dftracer/utils/utilities/io/lines/lines.h>

#endif  // DFTRACER_UTILS_UTILITIES_IO_IO_H
