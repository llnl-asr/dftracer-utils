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
#include <dftracer/utils/store/store.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace dftracer::utils::index {

namespace kv = dftracer::utils::store;

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
    struct Copy {
        kv::Namespace ns;
        kv::Batch batch;
        std::uint64_t params_hash = 0;
        /// Puts since the last stage, and puts that stage() already committed.
        std::size_t puts = 0;
        std::size_t staged_puts = 0;
        bool finished = false;
        bool failed = false;
    };

    std::string root;
    kv::Store store;
    kv::ScopeKey scope;
    dstd::Vector<SourceStat> sources;
    dstd::Vector<Copy> copies;
    /// Extensions whose copy of the file the commit drops.
    dstd::Vector<kv::Namespace> cleared;
    /// The first error of a put or a finish, reported by commit.
    dstd::Optional<dstd::Error> error;
    bool committed = false;

    Impl(std::string index_root, kv::Store s, kv::ScopeKey key,
         const SourceStat& stat);

    Copy* copy_of(const kv::NamespaceSpec& spec);

    static coro::CoroTask<dstd::Expected<void, dstd::Error>> commit_task(
        dstd::SharedPtr<Impl> self);
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
    static void finish(FileBuild& build, const kv::NamespaceSpec& spec,
                       std::uint64_t params_hash);
    static void fail(FileBuild& build, const kv::NamespaceSpec& spec,
                     std::uint64_t params_hash);
    static void record_error(FileBuild& build, dstd::Error error);
    static void clear(FileBuild& build, const kv::NamespaceSpec& spec);
    static coro::CoroTask<dstd::Expected<void, dstd::Error>> stage(
        FileBuild& build);

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
