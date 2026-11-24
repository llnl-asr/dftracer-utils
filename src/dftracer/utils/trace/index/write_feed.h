// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_WRITE_FEED_H
#define DFTRACER_UTILS_TRACE_INDEX_WRITE_FEED_H

#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/build/chunk_indexer.h>
#include <dftracer/utils/trace/index/indexer.h>

#include <cstdint>

namespace dftracer::utils::trace::index {

/// The bloom tier settings an @ref Indexer opened with `options` builds with;
/// the errors of Indexer::open for an option that is not valid.
dstd::Result<build::ChunkIndexerConfig> index_chunk_config(
    const IndexerOptions& options);

/// One gzip member of a written file, as the writer placed it.
struct FedMember {
    std::uint64_t c_offset = 0;
    std::uint64_t c_size = 0;
    std::uint64_t uc_size = 0;
    std::uint64_t lines = 0;
};

/// Indexes DFTracer files while a writer produces them, so no line is read back
/// or parsed twice: the writer hands over the text of each member as it forms
/// it, and the index state (member table, zonemaps, blooms, row sets) builds
/// next to the compression. Nothing reaches the index store until commit(), so
/// a write that fails leaves the store as it was.
///
/// The files are the ones a writer will rename to their final paths. A member
/// is fed whole and in file order: lines(), then end_member(), per member, the
/// header member of a table file first. The caller names where each member
/// landed in finish(). The result is the index that an @ref Indexer build of
/// the same file would make, except that the aggregation tier of an events
/// file is left unbuilt (a table's is empty). Not thread safe.
class WriteFeed {
   public:
    class File;

    /// Checks the options like @ref Indexer::open. `options.index_dir` is the
    /// index root, empty for the folder of each file. `options.schema` names
    /// the record schema of every file; empty is "dftracer".
    static dstd::Result<WriteFeed> open(IndexerOptions options);

    WriteFeed(WriteFeed&&) noexcept;
    WriteFeed& operator=(WriteFeed&&) noexcept;
    ~WriteFeed();

    /// A file that the writer renames to `final_path`. The pointer is valid
    /// while this feed lives.
    dstd::Result<File*> add_file(dstd::StringView final_path);

    /// `text` is whole lines of the member being fed. It is copied, and parsed
    /// and folded on a task of `scope` that holds at most a share of the
    /// memory budget of text, so a call waits when the parse is behind.
    /// Everything fed is done once drain() returns; the next call after it
    /// starts the task again in the scope it is given.
    coro::CoroTask<dstd::Result<void>> lines(File& file, dstd::StringView text,
                                             coro::Scope& scope);

    /// The member being fed has all its lines.
    coro::CoroTask<dstd::Result<void>> end_member(File& file,
                                                  coro::Scope& scope);

    /// Waits until everything fed so far is parsed and folded.
    coro::CoroTask<dstd::Result<void>> drain(File& file);

    /// The file is complete: `members` are its members in order, and
    /// `table_rows` the row count of its table header (-1 for events). Fails
    /// when they differ from the text that was fed.
    dstd::Result<void> finish(File& file, dstd::Vector<FedMember> members,
                              std::int64_t table_rows);

    /// Ends the stage of every file that was not drained and waits until no
    /// task of it runs, so this feed can be destroyed and its scope joined
    /// after a write that failed. What the stages still hold is dropped.
    coro::CoroTask<void> abandon();

    /// Writes the finished files into the index, named by their final paths.
    /// Call after the writer renamed them, with the files in place.
    coro::CoroTask<dstd::Result<void>> commit();

   private:
    struct Impl;
    explicit WriteFeed(dstd::UniquePtr<Impl> impl);
    dstd::UniquePtr<Impl> impl_;
};

}  // namespace dftracer::utils::trace::index

#endif  // DFTRACER_UTILS_TRACE_INDEX_WRITE_FEED_H
