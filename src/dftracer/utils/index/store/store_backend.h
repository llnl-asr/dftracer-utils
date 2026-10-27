#ifndef DFTRACER_UTILS_INDEX_STORE_STORE_BACKEND_H
#define DFTRACER_UTILS_INDEX_STORE_STORE_BACKEND_H

#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/store/record.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/writer.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>

namespace dftracer::utils {

class Runtime;

}  // namespace dftracer::utils

namespace dftracer::utils::index::store {

/// The store library. Its names would otherwise collide with this namespace.
namespace kv = dftracer::utils::store;

class StoreIndexWrite;

/// One index root's store: the data files under <root>/store, the writer of
/// this process for them, the file id table and the snapshot reads see. One
/// per root path per process, so every write of a root shares one writer and
/// one snapshot.
class StoreRoot : public std::enable_shared_from_this<StoreRoot> {
   public:
    /// The root of `index_root`, opened on its first write or snapshot. Throws
    /// std::runtime_error when the store directory cannot be created.
    static std::shared_ptr<StoreRoot> get_or_open(
        const std::string& index_root);

    /// Closes the writer, as close() does; a destructor cannot report the
    /// store error close() throws.
    ~StoreRoot();
    StoreRoot(const StoreRoot&) = delete;
    StoreRoot& operator=(const StoreRoot&) = delete;

    /// The id of `logical_path`, assigned when the path is new. An id never
    /// changes for a path and only grows within a process. The table is
    /// seeded, on its first use, from the file scopes the store holds, in
    /// logical path order.
    int file_id_for(std::string_view logical_path);
    /// The id of `logical_path` when the table binds it, without assigning
    /// one.
    std::optional<int> find_file_id(std::string_view logical_path);
    /// `count` ids no path holds yet, handed out in a run for a caller that
    /// numbers its files itself. The table binds them when the caller binds
    /// each to its path; nothing else takes an id of the run.
    int reserve_file_id_range(std::size_t count);
    /// The first id no path holds yet.
    int next_file_id();
    /// The path bound to `file_id`, or nullopt when the table does not bind
    /// it.
    std::optional<std::string> path_of(int file_id);

    /// The store as it stands, reopened after every commit through this root.
    /// A root that was never written has no store directory and yields null: a
    /// read of it answers nothing. Throws std::runtime_error when it cannot be
    /// opened.
    std::shared_ptr<const kv::Snapshot> snapshot();

    /// Syncs what the writer holds and closes its data file, so a later
    /// snapshot of the directory sees its footer. Waits for the commits in
    /// flight; a later write opens a new writer. Throws std::runtime_error on
    /// a store error.
    void close();

    /// One write over this root. Nothing it buffers is visible until its
    /// commit().
    std::unique_ptr<StoreIndexWrite> begin_write();

   private:
    friend class StoreIndexWrite;

    struct NamespaceEntry {
        kv::NamespaceId id;
        std::uint32_t version = 0;
    };

    /// The copy this root committed for one (namespace, scope): the writer
    /// keeps the params hash and sources it announced for a copy, so a later
    /// commit that changes either replaces the copy instead.
    struct CommittedCopy {
        std::uint64_t params_hash = 0;
        dstd::Vector<kv::SourceStat> sources;
    };

    explicit StoreRoot(std::string index_root);

    void ensure_dir_locked();
    void ensure_writer_locked();
    std::shared_ptr<const kv::Snapshot> open_snapshot_locked();
    std::shared_ptr<const kv::Snapshot> snapshot_locked();
    void seed_file_ids_locked();
    void close_locked();

    /// Runs one writer coroutine to completion on the runtime of this root.
    dstd::Expected<void, dstd::Error> run(
        coro::CoroTask<dstd::Expected<void, dstd::Error>> task);

    kv::Writer& writer();
    kv::NamespaceId namespace_id(const std::string& name,
                                 std::uint32_t version);
    /// The format version of `name`: the one this write gave it, else the
    /// highest one the store declares, else 0.
    std::uint32_t resolved_namespace_version(const std::string& name);
    void bind_file_id(std::string_view logical_path, int file_id);
    void mark_committed(const std::string& name, const kv::ScopeKey& scope,
                        std::uint64_t params_hash,
                        const dstd::Vector<kv::SourceStat>* sources);
    void forget_committed(const std::string& name, const kv::ScopeKey& scope);
    bool was_committed(const std::string& name, const kv::ScopeKey& scope);
    /// The copy this root committed for (name, scope); nullopt when it holds
    /// none.
    std::optional<CommittedCopy> committed_copy(const std::string& name,
                                                const kv::ScopeKey& scope);
    /// The sources of `scope`, one per path, from the file records of the
    /// write that asked or from the Members copy of the path; false when
    /// either holds none.
    bool sources_of(const kv::ScopeKey& scope,
                    const std::map<std::string, kv::SourceStat>& stats,
                    dstd::Vector<kv::SourceStat>& sources);
    void bump_version();

    /// Held shared by a commit for its whole run and exclusively by close(),
    /// so close() never frees the writer a commit uses. Taken before mutex_.
    std::shared_mutex gate_;
    mutable std::mutex mutex_;
    std::string index_root_;
    std::string store_dir_;
    std::shared_ptr<Runtime> runtime_;
    std::unique_ptr<kv::Writer> writer_;
    std::map<std::string, NamespaceEntry> namespaces_;
    std::map<std::string, int> ids_by_path_;
    std::map<int, std::string> paths_by_id_;
    std::map<std::pair<std::string, kv::ScopeKey>, CommittedCopy> committed_;
    std::shared_ptr<const kv::Snapshot> snapshot_;
    std::uint64_t version_ = 0;
    std::uint64_t snapshot_version_ = 0;
    int next_file_id_ = 1;
    bool seeded_ = false;
    bool snapshot_taken_ = false;
};

/// One write to the store of a StoreRoot, buffered until commit(): the data
/// keys of a copy are collected per namespace and scope, host keys bind file
/// ids and the copy state, and a delete it cannot express throws.
///
/// The family of the seam is not part of the mapping: a key's extension names
/// its namespace.
class StoreIndexWrite final : public IndexDatabaseWrite {
   public:
    StoreIndexWrite(const StoreIndexWrite&) = delete;
    StoreIndexWrite& operator=(const StoreIndexWrite&) = delete;
    ~StoreIndexWrite() override = default;

    void put(layout::Family family, std::string_view key,
             std::string_view value) override;
    void delete_range(layout::Family family, std::string_view begin,
                      std::string_view end) override;
    void del(layout::Family family, std::string_view key) override;

    /// Writes the pending copies in namespace and scope order and flushes the
    /// writer. A second call does nothing. Throws std::runtime_error on a
    /// store error. put() and the deletes throw when a data key names a file
    /// id the table does not bind.
    void commit() override;

    int file_id_for(std::string_view logical_path) override;
    /// Does nothing: a namespace version carries the format of the data it
    /// holds.
    void init_schema() override {}

   private:
    friend class StoreRoot;

    /// One copy a write is about to produce: the keys it holds, the state and
    /// the sources its manifest entry declares, and whether a delete cleared
    /// the whole copy.
    struct PendingCopy {
        std::string name;
        std::uint32_t version = 0;
        /// The namespace belongs to one plugin or one aggregation record, so
        /// its version is the one the store declares, not a fixed one.
        bool own_namespace = false;
        /// The copy holds a plugin extension.
        bool plugin = false;
        kv::ScopeKey scope;
        std::uint64_t params_hash = 0;
        bool has_manifest = false;
        bool failed = false;
        bool cleared = false;
        std::map<std::string, std::string> keys;
    };

    explicit StoreIndexWrite(std::shared_ptr<StoreRoot> root);

    PendingCopy& pending_copy(layout::Ext ext, std::uint32_t file_id,
                              std::string_view plugin, std::string_view meta);
    kv::ScopeKey scope_of(std::uint32_t file_id);
    std::string namespace_of(const store_layout::KeyTarget& target) const;
    void host_put(const store_layout::HostTarget& target,
                  std::string_view value);
    void host_delete(const store_layout::HostTarget& target);
    void clear_copy(layout::Ext ext, std::uint32_t file_id,
                    std::string_view plugin, std::string_view meta = {});
    /// Clears the copy of every plugin namespace of one file, in name order.
    void clear_plugin_copies(std::uint32_t file_id);
    static void clear_pending(PendingCopy& copy);
    /// Throws when the copy of (name, scope) was committed by this root and
    /// this write does not clear it whole: the store hides a copy whole, so a
    /// gap inside one has no representation.
    void check_narrow_delete(const std::string& name,
                             const kv::ScopeKey& scope);

    std::shared_ptr<StoreRoot> root_;
    std::map<std::pair<std::string, kv::ScopeKey>, PendingCopy> pending_;
    std::map<std::string, kv::SourceStat> file_stats_;
    /// The copy the last data key went to, so a run of keys of one copy skips
    /// the lookup.
    PendingCopy* last_copy_ = nullptr;
    layout::Ext last_ext_ = layout::Ext::Host;
    std::uint32_t last_file_id_ = 0;
    std::string last_plugin_;
    std::string last_meta_;
    bool committed_ = false;
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_STORE_BACKEND_H
