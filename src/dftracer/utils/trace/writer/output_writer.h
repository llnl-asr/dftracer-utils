// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_WRITER_OUTPUT_WRITER_H
#define DFTRACER_UTILS_TRACE_WRITER_OUTPUT_WRITER_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dataframe/dataframe.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>

namespace dftracer::utils::trace::writer {

/// What a row of an output is. Events are DFTracer event lines. Records are
/// the rows of a dftu.table file: a header line (ph 4) and one ph 3 line per
/// row.
enum class OutputRows : std::uint8_t { Events, Records };

/// What a write does when the target exists. Error refuses it. Overwrite
/// replaces it: a file by one atomic rename, a folder by renaming the old one
/// aside, renaming the new one in and removing the old one.
enum class OutputMode : std::uint8_t { Error, Overwrite };

/// The member size when OutputOptions::member_size is 0.
inline constexpr std::size_t OUTPUT_AUTO_MEMBER_SIZE = 4 * 1024 * 1024;

/// The width of the nrows slot of a records header.
inline constexpr std::size_t NROWS_SLOT_WIDTH = 20;

struct OutputOptions {
    /// A path ending in `.pfw.gz` or `.pfw` is one file; any other path is a
    /// folder of `part-NNNNN.pfw.gz` files.
    dstd::String path;
    OutputRows rows = OutputRows::Records;
    OutputMode mode = OutputMode::Error;
    /// Must agree with the extension of a file path; a folder is gzip only.
    bool compress = true;
    int level = 6;
    /// Uncompressed bytes per gzip member; 0 is @ref OUTPUT_AUTO_MEMBER_SIZE.
    std::size_t member_size = 0;
    /// Rows per part of a folder; 0 is one part. Refused for a file.
    std::size_t part_size = 0;
    std::size_t workers = 0;  ///< compression workers; 0 is the machine's
    /// Bytes the writer may hold in buffers and in-flight members; 0 is auto.
    std::uint64_t memory_budget = 0;
    /// Build the index of the written files with index_output.
    bool build_index = true;
    dstd::String index_dir;  ///< empty is the default root of the files

    /// Records: the args object of the header without "nrows". The writer
    /// puts the row count in a fixed-width slot at the start of the object.
    dstd::String header_args_json;
    /// Records: the header args hold no "fields"; the writer takes them from
    /// the first batch (a zero-batch write has none). Each column is typed by
    /// its engine type (the nullable pandas names: Int8 to Int64, UInt8 to
    /// UInt64, Float32, Float64, boolean, object). A later batch whose columns
    /// do not fit is refused.
    bool fields_from_first_batch = false;
    /// Records: string columns whose cells hold JSON text, written as is.
    dstd::Vector<dstd::String> raw_columns;
    /// Records: the total row count when it is known before the first row
    /// (every part header then has its real count), else -1 (every part header
    /// holds -1 until its part is finished, and then its row count).
    std::int64_t known_rows = -1;
};

struct OutputSummary {
    dstd::String root;                 ///< the file or the folder
    dstd::Vector<dstd::String> files;  ///< the written files, in part order
    std::uint64_t rows = 0;
};

/// True for a path that names one file (`.pfw.gz` or `.pfw`).
bool is_file_output(dstd::StringView path);

/// Writes DFTracer events or table records as one file or a folder of parts,
/// into a staging path next to the target, and renames it into place at
/// close(). Rows are formatted and compressed in parallel members that one
/// writer appends in order; the bytes in flight stay under the memory budget.
///
/// A records header is its own gzip member of stored deflate blocks, or the
/// first line of a plain file. A header written before the row count is known
/// holds -1 in its fixed-width slot, which close() overwrites with the part's
/// row count, patching the member's CRC32 for gzip, before the fsync and the
/// rename. A write that fails or is dropped before close() leaves nothing at
/// the target and removes its staging path. Not thread-safe.
class OutputWriter {
   public:
    struct Impl;

    /// Checks the options, makes the staging path and removes the leftover
    /// staging siblings of the target (names `.<name>.tmp-<digits>-<digits>`
    /// and `.<name>.old-<digits>-<digits>`; never one whose process is alive).
    /// Fails when the target exists in OutputMode::Error.
    static dstd::Result<OutputWriter> open(OutputOptions options);

    OutputWriter(OutputWriter&&) noexcept;
    OutputWriter& operator=(OutputWriter&&) noexcept;
    ~OutputWriter();

    /// Appends every row of `batch`. Records: the batch's columns are the
    /// header's fields, in order. Events: the columns are name, cat, pid, tid,
    /// ts and ph, optionally dur, hhash, fhash and shash (written as args
    /// keys), and any number of `args.<key>` columns; a null args cell leaves
    /// its key out.
    coro::CoroTask<dstd::Result<void>> write(const dataframe::DataFrame& batch);

    /// Finishes the parts, fsyncs, renames into place and fsyncs the parent.
    /// The files are final when this returns; build their index with
    /// @ref index_output. When the final fsync of the parent fails, the error
    /// is returned after the rename: the target then holds the new output.
    /// OutputMode::Error renames without replacing, so a target created after
    /// open() is refused and left untouched.
    coro::CoroTask<dstd::Result<OutputSummary>> close();

   private:
    explicit OutputWriter(dstd::UniquePtr<Impl> impl);
    dstd::UniquePtr<Impl> impl_;
};

/// Replaces the folder `target` by the folder `staging`: renames `target`
/// aside to `aside`, then `staging` to `target`. When the second rename fails
/// the first is undone and `target` is as it was; when that fails too the error
/// names both paths and `aside` is left in place. On success the caller removes
/// `aside`.
dstd::Result<void> replace_folder(dstd::StringView staging,
                                  dstd::StringView target,
                                  dstd::StringView aside);

/// Builds one index store for the written files (under `options.index_dir`, or
/// the default root of the files) with the engine's indexer. Blocking: call it
/// outside a coroutine, after close().
dstd::Result<void> index_output(const OutputSummary& summary,
                                const OutputOptions& options);

}  // namespace dftracer::utils::trace::writer

#endif  // DFTRACER_UTILS_TRACE_WRITER_OUTPUT_WRITER_H
