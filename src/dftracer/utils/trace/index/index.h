// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_INDEX_H
#define DFTRACER_UTILS_TRACE_INDEX_INDEX_H

#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/store/store.h>
#include <dftracer/utils/trace/index/store/file.h>

#include <cstdint>
#include <utility>

namespace dftracer::utils::trace::index {

using SourceStat = dftracer::utils::store::SourceStat;

/// True when the file at `path`, now `size` bytes, holds more than `stored` and
/// its first `stored.size` bytes still match the sampled hash in `stored`. The
/// bytes past `stored.size` are not read and may be short or bad.
bool is_append_of(dstd::StringView path, std::uint64_t size,
                  const SourceStat& stored);

/// What a snapshot knows about a file or one of its extensions.
enum class Freshness : std::uint8_t { Current, Stale, Missing, Failed };

struct IndexAccess;

/// One indexed trace file of a Snapshot. Small and copyable. It is valid only
/// with the Snapshot it came from.
class FileRef {
   public:
    dstd::StringView path() const;
    const SourceStat& stat() const;

   private:
    friend struct IndexAccess;

    struct Data;

    explicit FileRef(dstd::SharedPtr<const Data> data)
        : data_(std::move(data)) {}

    dstd::SharedPtr<const Data> data_;
};

/// The index as it was when the snapshot was taken, for one query or one build
/// check. Cheap to copy and thread safe. A commit after the snapshot is not
/// visible in it.
class Snapshot {
   public:
    /// Null when the index holds no usable copy of the file.
    dstd::Optional<FileRef> file(dstd::StringView path) const;
    /// Every indexed file, in path order.
    dstd::Expected<dstd::Vector<FileRef>, dstd::Error> files() const;
    /// Compares the stored stat with `now`. Only mtime and size count.
    Freshness freshness(dstd::StringView path, const SourceStat& now) const;
    /// Current when the extension is built for the file's stat with
    /// `params_hash`. A plugin extension is always Missing here.
    Freshness extension(const FileRef& file, store::IndexExtension ext,
                        std::uint64_t params_hash) const;

   private:
    friend struct IndexAccess;

    struct Impl;

    explicit Snapshot(dstd::SharedPtr<const Impl> impl)
        : impl_(std::move(impl)) {}

    dstd::SharedPtr<const Impl> impl_;
};

/// The data of one trace file, collected until commit() publishes it. Nothing
/// is visible before the commit, and a build that is never committed leaves no
/// trace. Not thread safe.
class FileBuild {
   public:
    /// Marks the extension complete for `params_hash`. An extension that was
    /// written must be finished before commit().
    void finish(store::IndexExtension ext, std::uint64_t params_hash);

    /// Keeps the older copies of the file, so the written keys add to them and
    /// replace only the same keys. By default a written extension stands for
    /// the whole file. Call before commit().
    void keep_older_copies();

    /// Drops the file's copy of the extension in the same record as the
    /// commit. An extension the build neither writes nor clears keeps its
    /// copy. Writing and clearing one extension is an error.
    void clear(store::IndexExtension ext);

    /// Publishes every extension of the build, the failed ones and the cleared
    /// ones in one record, so a reader sees all of them or none. The build must
    /// stay alive until the task ends.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> commit();

   private:
    friend struct IndexAccess;

    struct Impl;

    explicit FileBuild(dstd::SharedPtr<Impl> impl) : impl_(std::move(impl)) {}

    dstd::SharedPtr<Impl> impl_;
};

/// The index of one root directory. Cheap to copy and thread safe.
class Index {
   public:
    /// `root` is the index directory. Nothing is created until the first
    /// build.
    static dstd::Expected<Index, dstd::Error> open(dstd::StringView root);

    dstd::Expected<Snapshot, dstd::Error> snapshot() const;

    /// Starts the data of `path`, which is named as snapshots name it, with
    /// the stat of the trace file the data is read from.
    dstd::Expected<FileBuild, dstd::Error> build(dstd::StringView path,
                                                 const SourceStat& stat) const;

    /// Drops every extension of `path`. Removing a file the index does not hold
    /// is not an error.
    coro::CoroTask<dstd::Expected<void, dstd::Error>> remove(
        dstd::StringView path) const;

   private:
    friend struct IndexAccess;

    struct Impl;

    explicit Index(dstd::SharedPtr<Impl> impl) : impl_(std::move(impl)) {}

    dstd::SharedPtr<Impl> impl_;
};

/// Closes the kept handle of one index root so a later open sees a fresh
/// root. Call it before the root is deleted or rebuilt.
void release_root(dstd::StringView root);
/// Closes every kept root, every cache folder and every store writer, for an
/// exit while the runtime still runs. Each step is safe to repeat.
void release_all_roots();

}  // namespace dftracer::utils::trace::index

#endif  // DFTRACER_UTILS_TRACE_INDEX_INDEX_H
