// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_WRITER_OUTPUT_WRITER_H
#define DFTRACER_UTILS_TRACE_WRITER_OUTPUT_WRITER_H

#include <dftracer/utils/coro/scope.h>
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

/// The most partitions a write makes when OutputOptions::max_partitions is 0.
inline constexpr std::size_t OUTPUT_MAX_PARTITIONS = 1024;

/// The writer's own index holds the state of one file for each
/// OUTPUT_FED_FILE_BYTES of memory budget, between OUTPUT_FED_FILES_MIN and
/// OUTPUT_FED_FILES_MAX files; the other files are listed in
/// OutputSummary::unindexed.
inline constexpr std::uint64_t OUTPUT_FED_FILE_BYTES = 2 * 1024 * 1024;
inline constexpr std::uint64_t OUTPUT_FED_FILES_MIN = 4;
inline constexpr std::uint64_t OUTPUT_FED_FILES_MAX = 256;

/// The path of the partition of a row whose key cell is null.
inline constexpr dstd::StringView HIVE_DEFAULT_PARTITION =
    "__HIVE_DEFAULT_PARTITION__";

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
    /// Hive partitioning of a folder: the columns whose values name the
    /// `key=value/` folders a row goes to. Names and values are percent-encoded
    /// (a byte outside A-Z a-z 0-9 `.` `_` `-` `~` becomes `%XX`, and a value
    /// that equals @ref HIVE_DEFAULT_PARTITION has its first byte encoded); a
    /// null value is @ref HIVE_DEFAULT_PARTITION. The key columns stay in each
    /// part. A part holds the rows of one partition. Refused for a file.
    dstd::Vector<dstd::String> partition_by;
    /// More distinct keys than this fail the write at the first excess key; 0
    /// is
    /// @ref OUTPUT_MAX_PARTITIONS.
    std::size_t max_partitions = 0;
    /// Partitioned output: the most part files open at once. A part that is
    /// closed to stay under it is reopened for more rows, never finished early.
    std::size_t max_open_files = 64;
    std::size_t workers = 0;  ///< compression workers; 0 is the machine's
    /// Bytes the writer may hold in buffers and in-flight members; 0 is auto.
    std::uint64_t memory_budget = 0;
    /// Index the files. Records are indexed while they are written, up to a
    /// share of the memory budget of files, and the index is committed after
    /// the files are renamed; events, and the records of any file past that
    /// share, are indexed by @ref index_output after close(), because an
    /// aggregation tier reads the committed files. Plain output cannot be
    /// indexed.
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
    /// Partitioned output: the most bytes of rows that waited in partition
    /// buffers at once. It stays under the memory budget's share plus one
    /// batch of rows.
    std::uint64_t peak_buffer_bytes = 0;
    /// The files whose index the writer did not build: all of them for events
    /// and when the writer's own index could not be written, and for records
    /// the files past the share of the memory budget that it may hold. Pass the
    /// summary to @ref index_output.
    dstd::Vector<dstd::String> unindexed;
};

/// True for a path that names one file (`.pfw.gz` or `.pfw`).
bool is_file_output(dstd::StringView path);

/// Writes DFTracer events or table records as one file or a folder of parts,
/// into a staging path next to the target, and renames it into place at
/// close(). Rows are formatted and compressed in parallel members that one
/// writer appends in order; the bytes in flight stay under the memory budget.
///
/// Partitioned output keeps the rows of each partition in a buffer under one
/// byte budget, and writes the largest buffer as a gzip member when the
/// buffers pass it.
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

    /// Runs the index parse tasks of a records write on `scope`, which the
    /// caller joins after close(), instead of waiting for them at the end of
    /// each write(). The scope must outlive this writer. Without it each
    /// write() and close() runs its own scope.
    void bind_scope(coro::Scope& scope);

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

    /// After a failed write or close, before this writer is destroyed: waits
    /// for the tasks that parse the index and that compress, which run in the
    /// scope given to @ref bind_scope and read this writer's state.
    coro::CoroTask<void> abort();

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

/// Builds the index of `summary.unindexed` (under `options.index_dir`, or the
/// root of the files) with the engine's indexer. The index is a cache: a
/// failure logs a warning and the call still succeeds, and the files stay
/// readable and unindexed. Blocking: call it after close().
dstd::Result<void> index_output(const OutputSummary& summary,
                                const OutputOptions& options);

}  // namespace dftracer::utils::trace::writer

#endif  // DFTRACER_UTILS_TRACE_WRITER_OUTPUT_WRITER_H
