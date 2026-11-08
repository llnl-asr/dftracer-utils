// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_INDEX_INDEX_INTERNAL_H
#define DFTRACER_UTILS_INDEX_INDEX_INTERNAL_H

#include <dftracer/utils/coro/async_mutex.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/store/store.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace dftracer::utils::index {

struct FileRef::Data {
    dstd::String path;
    dftracer::utils::store::ScopeKey scope;
    SourceStat stat;
};

struct Index::Impl {
    dstd::String root;
    dstd::String store_dir;
    std::mutex mutex;
    dstd::Optional<dftracer::utils::store::Store> store;

    /// Opens the store, which creates its directory.
    dstd::Expected<dftracer::utils::store::Store, dstd::Error> open_store();
    /// The store when it exists on disk, else nothing.
    dstd::Expected<dstd::Optional<dftracer::utils::store::Store>, dstd::Error>
    find_store();
};

struct Snapshot::Impl {
    /// Empty when the index has no store directory yet.
    dstd::Optional<dftracer::utils::store::Snapshot> snap;
};

struct FileBuild::Impl {
    static constexpr std::size_t EXTENSION_SLOTS = 16;

    struct Copy {
        dftracer::utils::store::Namespace ns;
        /// Opened by the first put, and again by the first put after a stage.
        dstd::Optional<dftracer::utils::store::Batch> batch;
        std::uint64_t params_hash = 0;
        /// Puts since the last stage, and puts the stages already committed.
        std::size_t puts = 0;
        std::size_t staged_puts = 0;
        bool finished = false;
        bool failed = false;
    };

    dstd::String root;
    dftracer::utils::store::Store store;
    dftracer::utils::store::ScopeKey scope;
    /// Held by the fold task that puts and stages a spilled run, so two tasks
    /// never write the copies below at once.
    coro::AsyncMutex write_mutex;
    dstd::Vector<SourceStat> sources;
    dstd::Vector<Copy> copies;
    /// The position in `copies` of each built-in extension, or -1.
    std::array<std::int16_t, EXTENSION_SLOTS> ext_copy;
    /// Extensions whose copy of the file the commit drops.
    dstd::Vector<dftracer::utils::store::Namespace> cleared;
    /// Also drop every copy of the file the build did not write, but the ones
    /// named in `kept`.
    bool drop_unwritten = false;
    dstd::Vector<dstd::String> kept;
    /// A written copy stands for the whole file and hides older ones. False
    /// for a file with no older copies, and for new members of a grown file.
    bool replaces_older = true;
    /// The first error of a put or a finish, reported by commit.
    dstd::Optional<dstd::Error> error;
    bool committed = false;

    Impl(dstd::String index_root, dftracer::utils::store::Store s,
         dftracer::utils::store::ScopeKey key, const SourceStat& stat);

    Copy* copy_of(const dftracer::utils::store::NamespaceSpec& spec);
    Copy* copy_of(store::IndexExtension ext);
    void put_to(Copy* copy, dstd::ByteView key, dstd::ByteView value);

    /// Writes the batches and appends the publish targets of the build.
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> prepare(
        Impl& build, dstd::Vector<dftracer::utils::store::Publish>& targets);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> commit_task(
        dstd::SharedPtr<Impl> self);
    static coro::CoroTask<dstd::Vector<dstd::Optional<dstd::Error>>>
    commit_group_task(dstd::Vector<dstd::SharedPtr<Impl>> group);
};

/// The state behind the public index classes, for tables.cpp and index.cpp.
/// Not public: no other area may use it.
struct IndexAccess {
    static FileRef make_file(dstd::String path,
                             dftracer::utils::store::ScopeKey scope,
                             const SourceStat& stat);
    static Snapshot make_snapshot(
        dstd::Optional<dftracer::utils::store::Snapshot> snap);
    static FileBuild make_build(dstd::String root,
                                dftracer::utils::store::Store store,
                                dftracer::utils::store::ScopeKey scope,
                                const SourceStat& stat);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> remove_task(
        dstd::SharedPtr<Index::Impl> impl, dstd::String path);

    /// Null when the snapshot has no store.
    static const dftracer::utils::store::Snapshot* store_snapshot(
        const Snapshot& snapshot);
    static const dftracer::utils::store::ScopeKey& scope(const FileRef& file);

    static void put(FileBuild& build,
                    const dftracer::utils::store::NamespaceSpec& spec,
                    dstd::ByteView key, dstd::ByteView value);
    /// As above for a built-in extension, which finds its copy without
    /// building the spec.
    static void put(FileBuild& build, store::IndexExtension ext,
                    dstd::ByteView key, dstd::ByteView value);
    static void finish(FileBuild& build,
                       const dftracer::utils::store::NamespaceSpec& spec,
                       std::uint64_t params_hash);
    static void fail(FileBuild& build,
                     const dftracer::utils::store::NamespaceSpec& spec,
                     std::uint64_t params_hash);
    static void record_error(FileBuild& build, dstd::Error error);
    static void drop_unwritten(
        FileBuild& build,
        dstd::Span<const dftracer::utils::store::NamespaceSpec> kept);
    static void clear(FileBuild& build,
                      const dftracer::utils::store::NamespaceSpec& spec);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> stage(
        FileBuild& build);
    static coro::AsyncMutex& write_mutex(FileBuild& build);
    static coro::CoroTask<dstd::Vector<dstd::Optional<dstd::Error>>>
    commit_group(dstd::Span<FileBuild* const> builds);
    /// Erases the keys in [begin, end) of a copy the build holds. A copy it
    /// does not hold needs no erasing.
    static void erase(FileBuild& build,
                      const dftracer::utils::store::NamespaceSpec& spec,
                      dstd::ByteView begin, dstd::ByteView end);

    static const dstd::String& root(const Index& index);
    /// Opens the store, which creates its directory.
    static dstd::Expected<dftracer::utils::store::Store, dstd::Error>
    open_store(const Index& index);
    /// The store when it exists on disk, else nothing.
    static dstd::Expected<dstd::Optional<dftracer::utils::store::Store>,
                          dstd::Error>
    find_store(const Index& index);
};

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_INDEX_INTERNAL_H
