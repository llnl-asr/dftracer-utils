#ifndef DFTRACER_UTILS_INDEX_STORE_STORE_BACKEND_H
#define DFTRACER_UTILS_INDEX_STORE_STORE_BACKEND_H

#include <dftracer/utils/dstd/expected.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/string_arena.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/index/store/index_write.h>
#include <dftracer/utils/index/store/store_layout.h>
#include <dftracer/utils/memory/manager.h>
#include <dftracer/utils/runtime/coro/task.h>
#include <dftracer/utils/store/record.h>
#include <dftracer/utils/store/snapshot.h>
#include <dftracer/utils/store/writer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include <vector>

namespace dftracer::utils {

class Runtime;

}  // namespace dftracer::utils

namespace dftracer::utils::index::cache {

class CacheStore;

}  // namespace dftracer::utils::index::cache

namespace dftracer::utils::index::store {

namespace kv = dftracer::utils::store;

class StoreIndexWrite;

/// One index root's store (data files under <root>/store). There is one per
/// root path per process, so every write of a root shares one writer and one
/// snapshot.
class StoreRoot : public std::enable_shared_from_this<StoreRoot> {
   public:
    /// Opened on its first write or snapshot, with its writer on the process
    /// runtime. Throws std::runtime_error when the store directory cannot be
    /// created.
    static std::shared_ptr<StoreRoot> get_or_open(
        const std::string& index_root);

    /// Closes the writer of every root this process holds. Call it while the
    /// runtime the writers wait on still runs. Returns the first error. Every
    /// root is closed either way.
    static std::optional<std::string> close_all();

    /// Closes the writer. Unlike close(), it cannot report a store error.
    ~StoreRoot();
    StoreRoot(const StoreRoot&) = delete;
    StoreRoot& operator=(const StoreRoot&) = delete;

    /// Assigns an id when the path is new. An id never changes for a path and
    /// only grows within a process. The table is seeded on first use from the
    /// file scopes the store holds.
    int file_id_for(std::string_view logical_path);
    /// Does not assign an id.
    std::optional<int> find_file_id(std::string_view logical_path);
    /// A run of `count` unused ids for a caller that numbers its files itself.
    /// The table binds one when the caller binds it to a path. Nothing else
    /// takes an id of the run.
    int reserve_file_id_range(std::size_t count);
    int next_file_id();
    std::optional<std::string> path_of(int file_id);

    /// Reopened after every commit through this root. Null for a root that was
    /// never written. Throws std::runtime_error when the store cannot be
    /// opened.
    std::shared_ptr<const kv::Snapshot> snapshot();

    /// Syncs and closes the data file, so a later snapshot sees its footer.
    /// Waits for the commits in flight. A later write opens a new writer.
    /// Throws std::runtime_error on a store error.
    void close();

    /// Drops the snapshot, so the next read sees the data files other processes
    /// wrote since. snapshot() does this on its own when it finds them changed.
    void refresh();

    /// Nothing the write buffers is visible until its commit(). A `staged`
    /// write lands its copies as building copies at each stage() and publishes
    /// them at commit().
    std::unique_ptr<StoreIndexWrite> begin_write(bool staged = false);

    memory::Pool& pool() const noexcept { return *pool_; }

   private:
    friend class StoreIndexWrite;
    friend class cache::CacheStore;

    struct NamespaceEntry {
        kv::NamespaceId id;
        std::uint32_t version = 0;
    };

    class WriterUse {
       public:
        explicit WriterUse(StoreRoot& root);
        ~WriterUse();
        WriterUse(const WriterUse&) = delete;
        WriterUse& operator=(const WriterUse&) = delete;

       private:
        StoreRoot& root_;
    };

    /// The writer keeps the params hash and sources a copy announced, so a
    /// later commit that changes either replaces the copy.
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
    void refresh_locked();
    /// The data files of the store directory but this root's own writer's.
    std::vector<std::pair<std::string, std::uint64_t>> foreign_files_locked()
        const;
    /// Looks at most once per INDEX_STORE_REFRESH_MS.
    void refresh_if_changed_locked();

    dstd::Expected<void, dstd::Error> run(
        coro::CoroTask<dstd::Expected<void, dstd::Error>> task);

    /// Waits for extent bytes while the writer is busy.
    dstd::Expected<void, dstd::Error> commit(kv::Writer& writer,
                                             kv::Batch batch);

    kv::Writer& writer();
    kv::NamespaceId namespace_id(const std::string& name,
                                 std::uint32_t version);
    /// The version this write gave `name`, else the highest the store declares,
    /// else 0.
    std::uint32_t resolved_namespace_version(const std::string& name);
    void bind_file_id(std::string_view logical_path, int file_id);
    void mark_committed(const std::string& name, const kv::ScopeKey& scope,
                        std::uint64_t params_hash,
                        const dstd::Vector<kv::SourceStat>* sources);
    void forget_committed(const std::string& name, const kv::ScopeKey& scope);
    bool was_committed(const std::string& name, const kv::ScopeKey& scope);
    std::optional<CommittedCopy> committed_copy(const std::string& name,
                                                const kv::ScopeKey& scope);
    /// The Ready copy another writer holds, with the snapshot it was read from.
    /// nullopt when there is none, or when this root committed or dropped a
    /// copy of the scope since. A snapshot behind a refresh for other
    /// processes' files does not serve.
    struct ForeignCopy {
        std::shared_ptr<const kv::Snapshot> snapshot;
        kv::CopyInfo info;
    };
    std::optional<ForeignCopy> foreign_copy(const std::string& name,
                                            std::uint32_t version,
                                            const kv::ScopeKey& scope);
    /// False when neither the file records of the write nor the Members copy
    /// hold the sources.
    bool sources_of(const kv::ScopeKey& scope,
                    const std::map<std::string, kv::SourceStat>& stats,
                    dstd::Vector<kv::SourceStat>& sources);
    void bump_version();
    /// Holds what the root keeps across queries (snapshot, writer, file id
    /// table), never the pool of the query that made the call. First, so it
    /// closes after every other member is gone.
    memory::PoolHandle pool_;
    /// A count, not a lock, so a write holds its use across a co_await and ends
    /// it on another thread.
    std::mutex use_mutex_;
    std::condition_variable use_cv_;
    std::size_t uses_ = 0;
    bool closing_ = false;
    mutable std::mutex mutex_;
    std::string index_root_;
    std::string store_dir_;
    std::shared_ptr<Runtime> runtime_;
    std::unique_ptr<kv::Writer> writer_;
    std::map<std::string, NamespaceEntry> namespaces_;
    std::map<std::string, int> ids_by_path_;
    std::map<int, std::string> paths_by_id_;
    /// Keyed by the index in committed_names_ (high half) and the file id, so a
    /// build of many files holds a small entry per file, not strings and a
    /// node.
    struct FileCopy {
        std::uint64_t params_hash = 0;
        kv::SourceStat source;
        bool has_source = false;
        bool dropped = false;
    };
    std::vector<std::string> committed_names_;
    dstd::HashMap<std::uint64_t, FileCopy> file_copies_;
    std::map<std::pair<std::string, kv::ScopeKey>, CommittedCopy> committed_;
    std::set<std::pair<std::string, kv::ScopeKey>> dropped_;
    std::optional<std::uint64_t> file_copy_key_locked(
        const std::string& name, const kv::ScopeKey& scope);
    void forget_scope_entry_locked(const std::string& name,
                                   const kv::ScopeKey& scope);
    std::shared_ptr<const kv::Snapshot> snapshot_;
    std::vector<std::pair<std::string, std::uint64_t>> foreign_seen_;
    std::chrono::steady_clock::time_point foreign_checked_{};
    /// Changed under mutex_, read without it by snapshot()'s fast path.
    std::atomic<std::uint64_t> version_{0};
    std::uint64_t snapshot_version_ = 0;
    /// snapshot() returns it under a shared lock, so readers never wait on each
    /// other or on mutex_. Written under mutex_, then this lock.
    mutable std::shared_mutex published_mutex_;
    std::shared_ptr<const kv::Snapshot> published_;
    std::uint64_t published_version_ = 0;
    /// steady_clock ticks before which no look is due. 0 makes every snapshot()
    /// look.
    std::atomic<std::int64_t> next_look_{0};
    std::uint64_t refreshed_version_ = 0;
    int next_file_id_ = 1;
    bool seeded_ = false;
    bool snapshot_taken_ = false;
};

/// One write to the store of a StoreRoot, buffered until commit(). A delete it
/// cannot express throws.
class StoreIndexWrite final : public IndexDatabaseWrite {
   public:
    StoreIndexWrite(const StoreIndexWrite&) = delete;
    StoreIndexWrite& operator=(const StoreIndexWrite&) = delete;
    ~StoreIndexWrite() override = default;

    void put(std::string_view key, std::string_view value) override;
    void delete_range(std::string_view begin, std::string_view end) override;
    void del(std::string_view key) override;

    /// A second call does nothing. Throws std::runtime_error on a store error.
    /// put() and the deletes throw when a data key names a file id the table
    /// does not bind.
    void commit() override;
    /// On a worker of the process runtime it waits without holding the worker.
    /// Anywhere else it blocks like commit().
    coro::CoroTask<void> commit_async() override;
    /// Commits what the write holds as building copies, which commit()
    /// publishes. Throws std::logic_error on a write that is not staged, and
    /// std::runtime_error on a store error.
    void stage() override;

    int file_id_for(std::string_view logical_path) override;
    void bind_file(std::string_view logical_path, int file_id) override;

   private:
    friend class StoreRoot;

    /// Keys stay in write order as entries over the write's arena, since a
    /// build puts millions of small keys. resolved_keys() sorts them once at
    /// commit.
    struct PendingKey {
        const char* bytes = nullptr;
        std::uint32_t key_length = 0;
        /// TOMBSTONE for a delete of the key.
        std::uint32_t value_length = 0;
    };
    static constexpr std::uint32_t TOMBSTONE = 0xFFFFFFFFU;

    struct PendingCopy {
        std::string name;
        std::uint32_t version = 0;
        /// The version is the one the store declares, not a fixed one.
        bool own_namespace = false;
        bool plugin = false;
        kv::ScopeKey scope;
        std::uint64_t params_hash = 0;
        bool has_manifest = false;
        bool failed = false;
        bool cleared = false;
        std::vector<PendingKey> keys;
        /// Key ranges [begin, end) erased from a copy another writer holds
        /// (empty end = to the last key). A rewrite of that copy leaves them
        /// out.
        std::vector<std::pair<std::string, std::string>> skips;
        /// The store keeps the file record as the sources of the Members copy.
        bool file_record = false;
    };

    /// Each key once, in key order, with its last value. A deleted key is left
    /// out.
    static std::vector<PendingKey> resolved_keys(const PendingCopy& copy);
    void put_key(PendingCopy& copy, std::string_view key,
                 std::string_view value);
    void delete_key(PendingCopy& copy, std::string_view key);
    const char* keep(std::string_view key, std::string_view value);

    StoreIndexWrite(std::shared_ptr<StoreRoot> root, bool staged);

    struct StagedCopy {
        std::string name;
        std::uint32_t version = 0;
        kv::ScopeKey scope;
        std::uint64_t params_hash = 0;
        bool has_manifest = false;
        bool failed = false;
        bool cleared = false;
        bool building = false;
    };
    coro::CoroTask<void> stage_body(kv::Writer& writer);
    coro::CoroTask<void> commit_body();
    bool building(const std::string& name, const kv::ScopeKey& scope) const;
    /// Returns how many keys of `foreign` it put into `batch`, those `copy`
    /// neither writes, deletes nor erases.
    static std::size_t carry_keys(const PendingCopy& copy,
                                  const StoreRoot::ForeignCopy& foreign,
                                  kv::Batch& batch);

    PendingCopy& pending_copy(layout::Ext ext, std::uint32_t file_id,
                              std::string_view plugin, std::string_view meta);
    kv::ScopeKey scope_of(std::uint32_t file_id);
    std::string namespace_of(const store_layout::KeyTarget& target) const;
    void host_put(const store_layout::HostTarget& target,
                  std::string_view value);
    void host_delete(const store_layout::HostTarget& target);
    void clear_copy(layout::Ext ext, std::uint32_t file_id,
                    std::string_view plugin, std::string_view meta = {});
    void clear_plugin_copies(std::uint32_t file_id);
    static void clear_pending(PendingCopy& copy);
    /// Hides the keys [begin, end) of the copy this root already committed, so
    /// the keys outside keep its generation, params hash and sources. `begin`
    /// and `end` are keys inside the copy.
    void erase_keys(const store_layout::KeyTarget& target,
                    std::string_view begin, std::string_view end);

    std::shared_ptr<StoreRoot> root_;
    /// Keys and values stay in place until the write ends.
    StringArena arena_;
    std::map<std::pair<std::string, kv::ScopeKey>, PendingCopy> pending_;
    std::map<std::string, kv::SourceStat> file_stats_;
    /// Hidden at commit() ahead of the copies of this write, so the write lands
    /// whole or not at all.
    struct PendingErase {
        std::string name;
        std::uint32_t version = 0;
        kv::ScopeKey scope;
        std::string begin;
        std::string end;
    };
    std::vector<PendingErase> erases_;
    /// Lets a run of keys of one copy skip the lookup.
    PendingCopy* last_copy_ = nullptr;
    layout::Ext last_ext_ = layout::Ext::Host;
    std::uint32_t last_file_id_ = 0;
    std::string last_plugin_;
    std::string last_meta_;
    bool committed_ = false;
    bool staged_ = false;
    std::map<std::pair<std::string, kv::ScopeKey>, StagedCopy> staged_copies_;
};

}  // namespace dftracer::utils::index::store

#endif  // DFTRACER_UTILS_INDEX_STORE_STORE_BACKEND_H
