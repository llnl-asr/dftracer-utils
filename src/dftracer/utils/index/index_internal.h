#ifndef DFTRACER_UTILS_INDEX_INDEX_INTERNAL_H
#define DFTRACER_UTILS_INDEX_INDEX_INTERNAL_H

#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/index.h>
#include <dftracer/utils/runtime/coro/async_mutex.h>
#include <dftracer/utils/store/store.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace dftracer::utils::index {

namespace kv = dftracer::utils::store;

/// Lets the index at `root` close once its current users finish. Index::open
/// keeps recently opened roots open until this or process exit.
void release_root(dstd::StringView root);
/// Closes every store of the process and the kept roots. For shutdown, while
/// the runtime still runs.
void release_all_roots();

struct FileRef::Data {
    dstd::String path;
    kv::ScopeKey scope;
    SourceStat stat;
};

struct Index::Impl {
    std::string root;
    std::string store_dir;
    std::mutex mutex;
    dstd::Optional<kv::Store> store;

    /// Opens the store, which creates its directory.
    dstd::Expected<kv::Store, dstd::Error> open_store();
    /// The store when it exists on disk, else nothing.
    dstd::Expected<dstd::Optional<kv::Store>, dstd::Error> find_store();
};

struct Snapshot::Impl {
    /// Empty when the index has no store directory yet.
    dstd::Optional<kv::Snapshot> snap;
};

struct FileBuild::Impl {
    static constexpr std::size_t EXTENSION_SLOTS = 16;

    struct Copy {
        kv::Namespace ns;
        /// Opened by the first put, and again by the first put after a stage.
        dstd::Optional<kv::Batch> batch;
        std::uint64_t params_hash = 0;
        /// Puts since the last stage, and puts the stages already committed.
        std::size_t puts = 0;
        std::size_t staged_puts = 0;
        bool finished = false;
        bool failed = false;
    };

    std::string root;
    kv::Store store;
    kv::ScopeKey scope;
    /// Held by the fold task that puts and stages a spilled run, so two tasks
    /// never write the copies below at once.
    coro::AsyncMutex write_mutex;
    dstd::Vector<SourceStat> sources;
    dstd::Vector<Copy> copies;
    /// The position in `copies` of each built-in extension, or -1.
    std::array<std::int16_t, EXTENSION_SLOTS> ext_copy;
    /// Extensions whose copy of the file the commit drops.
    dstd::Vector<kv::Namespace> cleared;
    /// Also drop every copy of the file the build did not write, but the ones
    /// named in `kept`.
    bool drop_unwritten = false;
    dstd::Vector<dstd::String> kept;
    /// The first error of a put or a finish, reported by commit.
    dstd::Optional<dstd::Error> error;
    bool committed = false;

    Impl(std::string index_root, kv::Store s, kv::ScopeKey key,
         const SourceStat& stat);

    Copy* copy_of(const kv::NamespaceSpec& spec);
    Copy* copy_of(store::IndexExtension ext);
    void put_to(Copy* copy, dstd::ByteView key, dstd::ByteView value);

    /// Writes the batches and appends the publish targets of the build.
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> prepare(
        Impl& build, dstd::Vector<kv::Publish>& targets);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> commit_task(
        dstd::SharedPtr<Impl> self);
    static coro::CoroTask<dstd::Vector<dstd::Optional<dstd::Error>>>
    commit_group_task(dstd::Vector<dstd::SharedPtr<Impl>> group);
};

/// The state behind the public index classes, for tables.cpp and index.cpp.
/// Not public: no other area may use it.
struct IndexAccess {
    static FileRef make_file(dstd::String path, kv::ScopeKey scope,
                             const SourceStat& stat);
    static Snapshot make_snapshot(dstd::Optional<kv::Snapshot> snap);
    static FileBuild make_build(std::string root, kv::Store store,
                                kv::ScopeKey scope, const SourceStat& stat);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> remove_task(
        dstd::SharedPtr<Index::Impl> impl, dstd::String path);

    /// Null when the snapshot has no store.
    static const kv::Snapshot* store_snapshot(const Snapshot& snapshot);
    static const kv::ScopeKey& scope(const FileRef& file);

    static void put(FileBuild& build, const kv::NamespaceSpec& spec,
                    dstd::ByteView key, dstd::ByteView value);
    /// As above for a built-in extension, which finds its copy without
    /// building the spec.
    static void put(FileBuild& build, store::IndexExtension ext,
                    dstd::ByteView key, dstd::ByteView value);
    static void finish(FileBuild& build, const kv::NamespaceSpec& spec,
                       std::uint64_t params_hash);
    static void fail(FileBuild& build, const kv::NamespaceSpec& spec,
                     std::uint64_t params_hash);
    static void record_error(FileBuild& build, dstd::Error error);
    static void drop_unwritten(FileBuild& build,
                               dstd::Span<const kv::NamespaceSpec> kept);
    static void clear(FileBuild& build, const kv::NamespaceSpec& spec);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> stage(
        FileBuild& build);
    static coro::AsyncMutex& write_mutex(FileBuild& build);
    static coro::CoroTask<dstd::Vector<dstd::Optional<dstd::Error>>>
    commit_group(dstd::Span<FileBuild* const> builds);
    /// Erases the keys in [begin, end) of a copy the build holds. A copy it
    /// does not hold needs no erasing.
    static void erase(FileBuild& build, const kv::NamespaceSpec& spec,
                      dstd::ByteView begin, dstd::ByteView end);

    static const std::string& root(const Index& index);
    /// Opens the store, which creates its directory.
    static dstd::Expected<kv::Store, dstd::Error> open_store(
        const Index& index);
    /// The store when it exists on disk, else nothing.
    static dstd::Expected<dstd::Optional<kv::Store>, dstd::Error> find_store(
        const Index& index);
};

/// Tells the StoreRoot of `root`, when this process holds one, about the
/// changes just published, so an old-API write never skips on stale
/// bookkeeping.
void note_applied(const std::string& root,
                  dstd::Span<const kv::Publish> applied);

}  // namespace dftracer::utils::index

#endif  // DFTRACER_UTILS_INDEX_INDEX_INTERNAL_H
